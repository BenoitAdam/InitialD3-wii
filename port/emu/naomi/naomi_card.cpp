// Initial D's magnetic card reader/writer (Sanwa CRP-1231BR), on the SH-4
// SCIF serial port.
//
// Protocol (from the community documentation also used by Flycast and
// YACardEmu; reimplemented here):
//   host -> reader  STX LEN CMD 0 0 0 [data] ETX CRC     reader answers ACK
//   host -> reader  ENQ                                 reader sends the reply
//   reader -> host  STX LEN CMD S1 S2 S3 [data] ETX CRC
// LEN counts the bytes after itself (CRC included); CRC is the XOR of LEN..ETX.
//   S1  sensors: bits 7-6 shutter (1 closed, 2 open), bit 5 card stock OK,
//       bits 4-0 card position (0 none, 0x18 card inside)
//   S2  error ('0' none)          S3  command state ('0' OK, '4' no card)
// A card holds three magnetic tracks of 0x45 bytes.
#include "types.h"
#include "dc/sh4/scif.h"
#include "naomi.h"

#include <cstring>

namespace {

constexpr u8 STX = 0x02;
constexpr u8 ETX = 0x03;
constexpr u8 ENQ = 0x05;
constexpr u8 ACK = 0x06;
constexpr u32 kTrack = 0x45;

class CardReader final : public ScifPipe
{
public:
    void write(u8 b) override
    {
        if (m_inLen == 0 && b == ENQ)
        {
            reply();
            return;
        }
        if (m_inLen >= sizeof(m_in))
            m_inLen = 0;                              // runaway packet: resync
        m_in[m_inLen++] = b;
        if (m_inLen < 3)
            return;
        if (m_in[0] != STX)
        {
            m_inLen = 0;
            return;
        }
        const u32 total = m_in[1] + 2u;
        if (m_inLen < total)
            return;
        u8 crc = 0;
        for (u32 i = 1; i + 1 < total; i++)
            crc ^= m_in[i];
        if (crc == m_in[total - 1])
        {
            push(ACK);
            m_cmdLen = total - 3;                     // CMD .. ETX
            memcpy(m_cmd, &m_in[2], m_cmdLen);
            m_commands++;
        }
        m_inLen = 0;
    }

    int available() override { return (int)m_outCount; }

    u8 read() override
    {
        if (m_outCount == 0)
            return 0;
        const u8 v = m_out[m_outHead];
        m_outHead = (m_outHead + 1) % sizeof(m_out);
        m_outCount--;
        return v;
    }

    bool inserted = false;
    u8 card[kTrack * 3] = {};
    bool dirty = false;
    u32 m_commands = 0;

private:
    u8 m_in[300];
    u32 m_inLen = 0;
    u8 m_cmd[300];
    u32 m_cmdLen = 0;
    u8 m_out[1024];
    u32 m_outHead = 0, m_outCount = 0;
    bool m_shutterOpen = false;

    void push(u8 v)
    {
        if (m_outCount < sizeof(m_out))
            m_out[(m_outHead + m_outCount++) % sizeof(m_out)] = v;
    }

    u8 sensors() const
    {
        return (u8)(((m_shutterOpen ? 2 : 1) << 6) | 0x20 | (inserted ? 0x18 : 0));
    }

    // Offset and size of the tracks a command's track selector names.
    static bool tracks(u8 sel, u32& offset, u32& size)
    {
        switch (sel)
        {
        case '0': offset = 0;          size = kTrack;     return true;
        case '1': offset = kTrack;     size = kTrack;     return true;
        case '2': offset = kTrack * 2; size = kTrack;     return true;
        case '3': offset = 0;          size = kTrack * 2; return true;
        case '5': offset = kTrack;     size = kTrack * 2; return true;
        case '6': offset = 0;          size = kTrack * 3; return true;
        default: return false;                        // '4' (1+3) handled by the caller
        }
    }

    void reply()
    {
        if (m_cmdLen == 0)
            return;
        const u8 cmd = m_cmd[0];
        const u8 arg = m_cmdLen > 4 ? m_cmd[4] : 0;
        u8 state = '0';
        const u8* data[2] = {nullptr, nullptr};
        u32 dataLen[2] = {0, 0};

        switch (cmd)
        {
        case 0xD0:                                    // shutter: '1' open
            m_shutterOpen = arg == '1';
            break;
        case 0xB0:                                    // take a new card from the stock
            inserted = true;
            m_shutterOpen = false;
            break;
        case 0x80:                                    // eject
            inserted = false;
            break;
        case 0x53:                                    // write: mode, parity, tracks, data
        {
            const u8 sel = m_cmdLen > 6 ? m_cmd[6] : 0;
            const u8* src = &m_cmd[7];
            const u32 avail = m_cmdLen > 8 ? m_cmdLen - 8 : 0;   // up to ETX
            u32 off, size;
            if (sel == '4')
            {
                if (avail >= kTrack * 2)
                {
                    memcpy(card, src, kTrack);
                    memcpy(card + kTrack * 2, src + kTrack, kTrack);
                }
            }
            else if (tracks(sel, off, size) && avail >= size)
                memcpy(card + off, src, size);
            dirty = true;
            break;
        }
        case 0x33:                                    // read: mode, parity, tracks
        {
            const u8 sel = m_cmdLen > 6 ? m_cmd[6] : 0;
            if (!inserted || m_shutterOpen)
            {
                state = inserted ? '0' : '4';
                break;
            }
            if (arg == '2')                           // pull the card in, no data
                break;
            u32 off, size;
            if (sel == '4')
            {
                data[0] = card;                 dataLen[0] = kTrack;
                data[1] = card + kTrack * 2;    dataLen[1] = kTrack;
            }
            else if (tracks(sel, off, size))
            {
                data[0] = card + off;
                dataLen[0] = size;
            }
            break;
        }
        default:                                      // init, status, cancel, print, clean...
            break;
        }

        const u32 n = dataLen[0] + dataLen[1];
        u8 crc = 0;
        auto out = [&](u8 v) {
            push(v);
            crc ^= v;
        };
        push(STX);
        out((u8)(6 + n));
        out(cmd);
        out(sensors());
        out('0');
        out(state);
        for (int i = 0; i < 2; i++)
            for (u32 j = 0; j < dataLen[i]; j++)
                out(data[i][j]);
        out(ETX);
        push(crc);
    }
};

CardReader s_reader;

} // namespace

void naomi_CardReaderAttach()
{
    scif_SetPipe(&s_reader);
}

void naomi_CardInsert(bool inserted)
{
    s_reader.inserted = inserted;
}

void naomi_CardSetData(const u8* data, u32 size)
{
    memcpy(s_reader.card, data, size < sizeof(s_reader.card) ? size : sizeof(s_reader.card));
}

const u8* naomi_CardData(u32* size, bool* dirty)
{
    if (size)
        *size = sizeof(s_reader.card);
    if (dirty)
    {
        *dirty = s_reader.dirty;
        s_reader.dirty = false;
    }
    return s_reader.card;
}

u32 naomi_CardCommands()
{
    return s_reader.m_commands;
}
