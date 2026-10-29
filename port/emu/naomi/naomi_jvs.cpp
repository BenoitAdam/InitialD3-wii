// NAOMI maple port A: the main board's MIE and the JVS I/O board behind it.
//
// On a NAOMI the SH-4 never talks to controllers directly. Maple port A is
// the MIE, a small Z80 on the main board that answers maple frames itself
// and bridges them to the JVS bus (RS-485) where the cabinet's I/O board
// sits. Games use a few NAOMI-specific maple commands:
//   0x80  upload the MIE firmware (the BIOS's own copy runs anyway)
//   0x82  read the MIE ID string               -> 0x83 frames
//   0x84  self test                            -> 0x85
//   0x86  "JVS" commands with a sub-command    -> 0x87
//           0x13       store a request the MIE repeats every transfer
//           0x15       read what the I/O boards answered
//           0x17/19/21/27/33/35  send JVS packets (+ the stored request)
//           0x31       main board ports: DIP switches, TEST, SERVICE
//           0x03/0x0B  read / write the 128-byte serial EEPROM
// This is a high-level model of what that firmware does, written for this
// port from the observable protocol (Flycast's MIE/JVS emulation, GPL, and
// the JVS specification were the references; no code is shared).
//
// The I/O board is a Sega 837-13551 (the common "JVS I/O BD"): 2 players
// x 13 switches, 2 coin chutes, 8 analog channels, 6 outputs. Initial D
// reads steering on analog 0 and the pedals on 1 and 2.
#include "types.h"
#include "dc/maple/maple_devs.h"
#include "naomi.h"

#include <cstdio>
#include <cstring>

#if !defined(BUILD_NAOMI2)
#error "naomi_jvs.cpp is only part of the NAOMI 2 build"
#endif

namespace {

NaomiInputs s_in;
NaomiJvsStats s_stats;
NaomiInputPollFn s_poll;
u8 s_eeprom[128];
bool s_eepromDirty;

// Guest maple buffers hold host-order 32-bit words; the protocol is bytes in
// little-endian order, so byte i lives at host_ptr_xor(base + i).
struct Bytes
{
    u8* base;
    u32 size;
    u8 operator[](u32 i) const { return i < size ? *host_ptr_xor(base + i) : 0; }
};

struct Writer
{
    u8* base;
    u32 pos = 0;
    void w8(u8 v) { *host_ptr_xor(base + pos++) = v; }
    void w32(u32 v)
    {
        for (int i = 0; i < 4; i++)
            w8((u8)(v >> (i * 8)));
    }
    // Maple frame header: command, recipient (host), sender (port A), words.
    void header(u8 command, u8 words)
    {
        w8(command);
        w8(0x00);
        w8(0x20);
        w8(words);
    }
    void pad4()
    {
        while (pos & 3)
            w8(0);
    }
};

void logUnknown(const char* what, u32 code)
{
    static u32 seen[16];
    static u32 count;
    for (u32 i = 0; i < count; i++)
        if (seen[i] == code + 1)
            return;
    if (count < 16)
        seen[count++] = code + 1;
    s_stats.unknown++;
    naomi_Log("[jvs] unsupported %s %02X\n", what, code);
}

// ---------------------------------------------------------------------------
// JVS I/O board (node 1)
// ---------------------------------------------------------------------------
class IoBoard
{
public:
    static constexpr u8 kNode = 1;

    // `cmd` holds the packet's command bytes (no sync/address/length/sum).
    // Writes the board's complete reply packet to `out`; returns its size,
    // or 0 when the board stays silent (reset, address not ours).
    u32 handle(const u8* cmd, u32 len, u8* out)
    {
        if (len == 0 || cmd[0] == 0xF0)           // bus reset: no answer
            return 0;
        if (cmd[0] == 0xF1 && (len < 2 || cmd[1] != kNode))
            return 0;                             // address meant for another board

        u32 n = 0;
        out[n++] = 0xE0;                          // sync
        out[n++] = 0x00;                          // to the master
        const u32 lenAt = n++;
        out[n++] = 1;                             // status: normal

        if (cmd[0] == 0xF1)
        {
            out[n++] = 1;                         // report: OK
            out[n++] = 5;                         // as the MIE firmware expects (Flycast)
        }
        else
        {
            for (u32 i = 0; i < len;)
            {
                const u32 used = command(cmd + i, len - i, out, n);
                if (used == 0)
                {
                    out[n++] = 2;                 // report: unknown command, stop here
                    break;
                }
                i += used;
            }
        }

        out[lenAt] = (u8)(n - lenAt);             // bytes after the length, sum included
        u8 sum = 0;
        for (u32 i = 1; i < n; i++)
            sum += out[i];
        out[n++] = sum;
        s_stats.jvsMessages++;
        return n;
    }

private:
    u16 m_coins[2] = {};
    bool m_coinWas[2] = {};
    u8 m_outputs[4] = {};

    // One command: appends its report byte and data, returns bytes consumed
    // from the request (0 = not supported).
    u32 command(const u8* c, u32 avail, u8* out, u32& n)
    {
        auto need = [&](u32 bytes) { return avail >= bytes; };
        switch (c[0])
        {
        case 0x10:                                // I/O identification
        {
            static const char id[] = "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";
            out[n++] = 1;
            for (const char* p = id; *p; p++)
                out[n++] = (u8)*p;
            out[n++] = 0;
            return 1;
        }
        case 0x11: out[n++] = 1; out[n++] = 0x11; return 1;   // command format 1.1
        case 0x12: out[n++] = 1; out[n++] = 0x20; return 1;   // JVS 2.0
        case 0x13: out[n++] = 1; out[n++] = 0x10; return 1;   // communication 1.0
        case 0x14:                                // feature list
        {
            static const u8 features[] = {
                0x01, 2, 13, 0,                   // switches: 2 players, 13 each
                0x02, 2, 0, 0,                    // coin slots
                0x03, 8, 0x10, 0,                 // analog: 8 channels, 16 bits
                0x12, 6, 0, 0,                    // general outputs
                0x00,
            };
            out[n++] = 1;
            for (u8 b : features)
                out[n++] = b;
            return 1;
        }
        case 0x15:                                // main board ID string
        {
            u32 i = 1;
            while (i < avail && c[i] != 0)
                i++;
            out[n++] = 1;
            return i < avail ? i + 1 : avail;
        }
        case 0x20:                                // switches: players, bytes per player
        {
            if (!need(3))
                return 0;
            s_stats.inputReads++;
            out[n++] = 1;
            out[n++] = s_in.test ? 0x80 : 0x00;   // system: TEST, tilts
            for (u32 p = 0; p < c[1]; p++)
            {
                const u16 bits = p < 2 ? s_in.players[p] : 0;
                for (u32 b = 0; b < c[2]; b++)
                    out[n++] = b == 0 ? (u8)(bits >> 8) : b == 1 ? (u8)bits : 0;
            }
            return 3;
        }
        case 0x21:                                // coin counters
        {
            if (!need(2))
                return 0;
            for (int i = 0; i < 2; i++)
            {
                if (s_in.coin[i] && !m_coinWas[i])
                    m_coins[i]++;
                m_coinWas[i] = s_in.coin[i];
            }
            out[n++] = 1;
            for (u32 s = 0; s < c[1]; s++)
            {
                const u16 v = s < 2 ? m_coins[s] : 0;
                out[n++] = (u8)((v >> 8) & 0x3F);   // top bits: slot status (0 = OK)
                out[n++] = (u8)v;
            }
            return 2;
        }
        case 0x22:                                // analog channels
        {
            if (!need(2))
                return 0;
            out[n++] = 1;
            for (u32 ch = 0; ch < c[1]; ch++)
            {
                u16 v = ch < 8 ? s_in.analog[ch] : 0x8000;
                // The I/O board's low byte behaves as signed; keep the value
                // monotonic the way the games read it (as Flycast does).
                if (v > 0xFF7F)
                    v = 0xFF7F;
                if (v & 0x80)
                    v += 0x100;
                out[n++] = (u8)(v >> 8);
                out[n++] = (u8)v;
            }
            return 2;
        }
        case 0x30:                                // decrease coins: slot, amount
        case 0x31:                                // increase coins
        {
            if (!need(4))
                return 0;
            if (c[1] >= 1 && c[1] <= 2)
            {
                const u16 amount = (u16)((c[2] << 8) | c[3]);
                m_coins[c[1] - 1] += c[0] == 0x30 ? (u16)-amount : amount;
            }
            out[n++] = 1;
            return 4;
        }
        case 0x32:                                // general outputs (lamps): count, bytes
        {
            if (!need(2) || !need(2u + c[1]))
                return 0;
            for (u32 i = 0; i < c[1] && i < 4; i++)
                m_outputs[i] = c[2 + i];
            out[n++] = 1;
            return 2u + c[1];
        }
        case 0x33:                                // analog outputs: channels, 2 bytes each
        {
            if (!need(2) || !need(2u + 2u * c[1]))
                return 0;
            out[n++] = 1;
            return 2u + 2u * c[1];
        }
        case 0x34:                                // character output: count, bytes
        {
            if (!need(2) || !need(2u + c[1]))
                return 0;
            out[n++] = 1;
            return 2u + c[1];
        }
        default:
            logUnknown("JVS command", c[0]);
            return 0;
        }
    }
};

// ---------------------------------------------------------------------------
// MIE (maple device at port A)
// ---------------------------------------------------------------------------
struct MieDevice : maple_device
{
    IoBoard board;
    u8 repeat[256];               // stored request for node 1 ([0] = length)
    u8 rx[32][300];               // received JVS replies per channel
    u32 rxLen[32];

    MieDevice()
    {
        config = nullptr;
        repeat[0] = 0;
        memset(rxLen, 0, sizeof(rxLen));
    }

    bool IsRaw() const override { return true; }
    u32 Dma(u32, u32*, u32, u32*, u32& outLen) override
    {
        outLen = 0;
        return 0xFD;              // not used: the raw path handles everything
    }

    // Board ports as the MIE reports them (active low). Bit 0 set = 31 kHz
    // monitor; bits 1-3 DIP switches 2-4 (Flycast's tested default leaves
    // 2 and 3 on); bit 4 TEST, bit 5 SERVICE.
    u8 boardPorts() const
    {
        u8 v = 0xF9;
        if (s_in.boardTest)
            v &= ~0x10;
        if (s_in.boardService)
            v &= ~0x20;
        return v;
    }

    static u8 senseLine(u32 node) { return node == IoBoard::kNode ? 0x8E : 0x8F; }

    void deliver(u32 node, u32 channel, bool useRepeat, u32 len, const u8* cmd, bool repeatFirst)
    {
        if (node != IoBoard::kNode && node != 0xFF)
            return;
        u8 packet[512];
        u32 n = 0;
        const u32 rep = useRepeat ? repeat[0] : 0;
        if (rep && repeatFirst)
        {
            memcpy(packet, &repeat[1], rep);
            n = rep;
        }
        memcpy(packet + n, cmd, len);
        n += len;
        if (rep && !repeatFirst)
        {
            memcpy(packet + n, &repeat[1], rep);
            n += rep;
        }

        u8 reply[256];
        const u32 replyLen = board.handle(packet, n, reply);
        if (s_stats.jvsMessages <= 24)
        {
            // Bring-up trace of the first exchanges.
            char text[400];
            u32 t = (u32)snprintf(text, sizeof(text), "[jvs] node %u ch %u:", node, channel);
            for (u32 i = 0; i < n && t < 180; i++)
                t += (u32)snprintf(text + t, sizeof(text) - t, " %02X", packet[i]);
            t += (u32)snprintf(text + t, sizeof(text) - t, "  ->");
            for (u32 i = 0; i < replyLen && t < sizeof(text) - 8; i++)
                t += (u32)snprintf(text + t, sizeof(text) - t, " %02X", reply[i]);
            naomi_Log("%s\n", text);
        }
        if (replyLen == 0 || rxLen[channel] + replyLen + 3 > sizeof(rx[0]))
            return;
        u8* p = &rx[channel][rxLen[channel]];
        p[0] = IoBoard::kNode;
        p[1] = 0x00;              // 0 OK (2 timeout, 3 bad node, 4 bad checksum)
        p[2] = (u8)replyLen;
        memcpy(p + 3, reply, replyLen);
        rxLen[channel] += replyLen + 3;
    }

    // 0x87 frame carrying the MIE's port bytes and whatever the boards sent.
    void receive(Writer& w, u32 channel)
    {
        const u32 len = rxLen[channel];
        const u32 dataBytes = len ? (19 + len + 3) & ~3u : 20;
        w.header(0x87, (u8)(dataBytes / 4));
        w.w8(len ? 0x16 : 0x32);
        w.w8(0xFF);
        w.w8(0xFF);
        w.w8(0xFF);
        w.w32(0xFFFFFF00);
        w.w32(0);
        w.w32(0);
        if (len == 0)
        {
            w.w32(0);
            return;
        }
        w.w8(0);
        w.w8((u8)channel);
        w.w8(senseLine(rx[channel][0]));
        for (u32 i = 0; i < len; i++)
            w.w8(rx[channel][i]);
        w.pad4();
        rxLen[channel] = 0;
    }

    void ack(Writer& w, u8 code, u32 channel, u8 sense)
    {
        w.header(0x87, 1);
        w.w8(code);
        w.w8((u8)channel);
        w.w8(sense);
        w.w8(0);
    }

    void jvsCommand(Writer& w, const Bytes& in)
    {
        if (in.size == 0)
        {
            w.header(0x87, 0);
            return;
        }
        // Copy the request out of guest RAM once (it is a byte stream).
        u8 req[1024];
        const u32 size = in.size < sizeof(req) ? in.size : sizeof(req);
        for (u32 i = 0; i < size; i++)
            req[i] = in[i];

        const u8 sub = req[0];
        u32 node = 0, len = 0, channel = 0;
        const u8* cmd = req;
        if (size >= 3)
        {
            if (sub != 0x13 && size >= 8)
            {
                node = req[6];
                len = req[7];
                cmd = &req[8];
                channel = req[5] & 0x1F;
            }
            else
            {
                node = req[1];
                len = req[2];
                cmd = &req[3];
            }
        }
        if (cmd + len > req + size)
            len = (u32)(req + size - cmd);

        switch (sub)
        {
        case 0x13:                                // store repeated request
            if (len > 0 && node == IoBoard::kNode && len < sizeof(repeat))
            {
                repeat[0] = (u8)len;
                memcpy(&repeat[1], cmd, len);
            }
            w.header(0x87, 1);
            w.w8(sub + 1);
            w.w8(0);
            w.w8((u8)(len + 1));
            w.w8(0);
            break;
        case 0x15:                                // read replies
            receive(w, req[1] & 0x1F);
            break;
        case 0x17:                                // send, no repeat
            rxLen[channel] = 0;
            deliver(node, channel, false, len, cmd, false);
            ack(w, 0x18, channel, 0x8E);
            break;
        case 0x19:                                // send, stored request first
            rxLen[channel] = 0;
            deliver(node, channel, true, len, cmd, true);
            ack(w, 0x18, channel, senseLine(node));
            break;
        case 0x21:                                // send, stored request after
            rxLen[channel] = 0;
            deliver(node, channel, true, len, cmd, false);
            ack(w, 0x18, channel, senseLine(node));
            break;
        case 0x35:                                // read, then like 0x27
            receive(w, channel);
            // fall through
        case 0x27:                                // several packets
        {
            rxLen[channel] = 0;
            const u32 count = req[6];
            u32 idx = 7;
            for (u32 i = 0; i < count && idx + 2 <= size; i++)
            {
                node = req[idx];
                len = req[idx + 1];
                if (idx + 2 + len > size)
                    break;
                deliver(node, channel, true, len, &req[idx + 2], false);
                idx += len + 2;
            }
            ack(w, 0x26, channel, senseLine(node));
            break;
        }
        case 0x33:                                // read, then like 0x21
            receive(w, channel);
            deliver(node, channel, true, len, cmd, false);
            ack(w, 0x18, channel, senseLine(node));
            break;
        case 0x31:                                // main board ports
            w.header(0x87, 5);
            w.w8(0x32);
            w.w8(0xFF);                           // in(0)
            w.w8(0xFF);                           // in(1)
            w.w8(0xFF);                           // in(2)
            w.w8(0x00);
            w.w8(0xFF);                           // in(4)
            w.w8(boardPorts());                   // in(5): monitor, DIP, TEST, SERVICE
            w.w8(0xFF);                           // in(6)
            w.w32(0);
            w.w32(0);
            w.w32(0);
            break;
        case 0x01:
            w.header(0x87, 1);
            w.w32(0x02);
            break;
        case 0x0B:                                // EEPROM write: address, size, -, data
        {
            const u32 address = req[1] & 0x7F;
            u32 bytes = req[2];
            if (bytes > 128 - address)
                bytes = 128 - address;
            for (u32 i = 0; i < bytes && 4 + i < size; i++)
                s_eeprom[address + i] = req[4 + i];
            s_eepromDirty = true;
            w.header(0x87, 1);
            for (u32 i = 0; i < 4; i++)
                w.w8(s_eeprom[i]);
            break;
        }
        case 0x03:                                // EEPROM read: 128 bytes from address
        {
            const u32 address = req[1] & 0x7F;
            w.header(0x87, 0x20);
            for (u32 i = 0; i < 128; i++)
                w.w8(s_eeprom[(address + i) & 0x7F]);
            break;
        }
        case 0x4D:                                // RS-422 receive: no data
            w.header(0x87, 1);
            w.w32(0xFF000000);
            break;
        case 0x41: case 0x47: case 0x49: case 0x4B: case 0x4F:
            break;                                // RS-422 port: nothing attached
        default:
            logUnknown("0x86 sub-command", sub);
            w.header(0xFD, 0);
            break;
        }
    }

    u32 RawDma(u32* frameIn, u32 frameInLen, u32* frameOut) override
    {
        s_stats.mapleFrames++;
        if (s_poll)
            s_poll(s_in);
        const Bytes frame{(u8*)frameIn, frameInLen};
        const Bytes in{(u8*)frameIn + 4, frameInLen >= 4 ? frameInLen - 4 : 0};
        Writer w{(u8*)frameOut, 0};

        const u8 command = frame[0];
        switch (command)
        {
        case 0x01:                                // device request
            w.header(0x05, 0);
            break;
        case 0x02:                                // all status
            w.header(0x06, 0);
            break;
        case 0x03:                                // reset
        case 0x04:                                // kill
            w.header(0x07, 0);
            break;
        case 0x80:                                // firmware upload (not run: HLE)
        {
            if (in[1] == 0xFF)
            {
                repeat[0] = 0;                    // the new firmware starts clean
                w.header(0x07, 0);
                break;
            }
            u8 sum = 0;
            for (u32 i = 0; i < 0x1C; i++)
                sum += in[i];
            w.header(0x80, 1);
            w.w32(sum);
            w.header(0x07, 0);
            break;
        }
        case 0x82:                                // ID: two frames of 28 characters
        {
            static const char id1[] = "315-6149    COPYRIGHT SEGA E";
            static const char id2[] = "NTERPRISES CO,LTD.  1998    ";
            w.header(0x83, 7);
            for (u32 i = 0; i < 28; i++)
                w.w8((u8)id1[i]);
            w.header(0x83, 7);
            for (u32 i = 0; i < 28; i++)
                w.w8((u8)id2[i]);
            break;
        }
        case 0x84:                                // self test: OK
            w.header(0x85, 1);
            w.w32(0);
            break;
        case 0x86:
            jvsCommand(w, in);
            break;
        default:
            logUnknown("maple command", command);
            w.header(0xFD, 0);
            break;
        }
        return w.pos;
    }
};

} // namespace

maple_device* naomi_CreateMieDevice()
{
    return new MieDevice();
}

void naomi_DefaultInputs(NaomiInputs& in)
{
    memset(&in, 0, sizeof(in));
    for (u16& a : in.analog)
        a = 0x8000;
    in.analog[1] = 0;                             // Initial D pedals released
    in.analog[2] = 0;
}

void naomi_SetInputs(const NaomiInputs& in)
{
    s_in = in;
}

void naomi_SetInputPoll(NaomiInputPollFn fn)
{
    s_poll = fn;
}

void naomi_SetEeprom(const u8* data128)
{
    memcpy(s_eeprom, data128, sizeof(s_eeprom));
    s_eepromDirty = false;
}

const u8* naomi_Eeprom()
{
    return s_eeprom;
}

bool naomi_EepromDirty(bool clear)
{
    const bool dirty = s_eepromDirty;
    if (clear)
        s_eepromDirty = false;
    return dirty;
}

const NaomiJvsStats& naomi_JvsStats()
{
    return s_stats;
}

// Static initialisation: neutral controls until the host sets its own.
static struct JvsInit
{
    JvsInit() { naomi_DefaultInputs(s_in); }
} s_jvsInit;
