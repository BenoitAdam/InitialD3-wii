/*
	SCIF (Serial Communication Interface with FIFO)
	Used for serial I/O on the Dreamcast, externally available via the serial cable.

	Without an attached device (scif_SetPipe) the port behaves like an idle
	line: transmitted bytes are dropped once their character time elapses and
	nothing is ever received. With a device - on NAOMI, Initial D's card
	reader/writer - bytes move both ways at the programmed baud rate through
	the 16-byte FIFOs, with the status flags and TXI/RXI/ERI/BRI interrupts
	the SH7091 manual describes.
*/

#include "types.h"
#include "dc/mem/sh4_internal_reg.h"
#include "intc.h"
#include "scif.h"

//SCIF SCSMR2 0xFFE80000 0x1FE80000 16 0x0000 0x0000 Held Held Pclk
SCSMR2_type SCIF_SCSMR2;

//SCIF SCBRR2 0xFFE80004 0x1FE80004 8 0xFF 0xFF Held Held Pclk
u8 SCIF_SCBRR2;

//SCIF SCSCR2 0xFFE80008 0x1FE80008 16 0x0000 0x0000 Held Held Pclk
SCSCR2_type SCIF_SCSCR2;

//SCIF SCFTDR2 0xFFE8000C 0x1FE8000C 8 Undefined Undefined Held Held Pclk
u8 SCIF_SCFTDR2;

//SCIF SCFSR2 0xFFE80010 0x1FE80010 16 0x0060 0x0060 Held Held Pclk
// BUG FIX: was incorrectly typed as SCSCR2_type -- must be SCFSR2_type
SCFSR2_type SCIF_SCFSR2;

//SCIF SCFRDR2 0xFFE80014 0x1FE80014 8 Undefined Undefined Held Held Pclk
// Read ONLY
u8 SCIF_SCFRDR2;

//SCIF SCFCR2 0xFFE80018 0x1FE80018 16 0x0000 0x0000 Held Held Pclk
SCFCR2_type SCIF_SCFCR2;

// Read ONLY
//SCIF SCFDR2 0xFFE8001C 0x1FE8001C 16 0x0000 0x0000 Held Held Pclk
SCFDR2_type SCIF_SCFDR2;

//SCIF SCSPTR2 0xFFE80020 0x1FE80020 16 0x0000 0x0000 Held Held Pclk
SCSPTR2_type SCIF_SCSPTR2;

//SCIF SCLSR2 0xFFE80024 0x1FE80024 16 0x0000 0x0000 Held Held Pclk
SCLSR2_type SCIF_SCLSR2;

// ---------------------------------------------------------------------------
// Line model
// ---------------------------------------------------------------------------
enum
{
	SR_DR   = 0x01,
	SR_RDF  = 0x02,
	SR_PER  = 0x04,
	SR_FER  = 0x08,
	SR_BRK  = 0x10,
	SR_TDFE = 0x20,
	SR_TEND = 0x40,
	SR_ER   = 0x80,
};

struct Fifo16
{
	u8 data[16];
	u32 head, count;

	void clear() { head = count = 0; }
	bool full() const { return count == 16; }
	void push(u8 v) { data[(head + count++) & 15] = v; }
	u8 pop()
	{
		const u8 v = data[head];
		head = (head + 1) & 15;
		count--;
		return v;
	}
};

static ScifPipe* s_pipe;
static Fifo16 s_tx, s_rx;
static bool s_transmitting;
static s32 s_txTimer;            // cycles left on the character being shifted out
static s32 s_rxTimer;            // cycles until the next receive slot
static s32 s_frameCycles = 1;    // one character time in SH-4 cycles
static u16 s_statusLastRead;     // flags may only be cleared after being read as 1

static void updateInterrupts()
{
	const u16 sr = SCIF_SCFSR2.full;
	InterruptPend(sh4_SCIF_TXI, sr & SR_TDFE);
	InterruptMask(sh4_SCIF_TXI, SCIF_SCSCR2.TIE);

	InterruptPend(sh4_SCIF_RXI, sr & (SR_RDF | SR_DR));
	InterruptMask(sh4_SCIF_RXI, SCIF_SCSCR2.RIE);

	InterruptPend(sh4_SCIF_BRI, sr & SR_BRK);
	InterruptMask(sh4_SCIF_BRI, SCIF_SCSCR2.RIE | SCIF_SCSCR2.REIE);

	InterruptPend(sh4_SCIF_ERI, sr & (SR_ER | SR_FER | SR_PER));
	InterruptMask(sh4_SCIF_ERI, SCIF_SCSCR2.RIE | SCIF_SCSCR2.REIE);
}

static void setStatus(u16 bits)
{
	s_statusLastRead &= ~bits;
	SCIF_SCFSR2.full |= bits;
}

static bool txBelowTrigger()
{
	// TTRG: 8, 4, 2, 1 bytes left
	return s_tx.count <= (1u << (3 - (SCIF_SCFCR2.full >> 4 & 3)));
}

static bool rxAtTrigger()
{
	static const u32 levels[4] = { 1, 4, 8, 14 };
	return s_rx.count >= levels[SCIF_SCFCR2.full >> 6 & 3];
}

static void updateBaudRate()
{
	// Pclk = 50 MHz; bit rate = Pclk / (32 * 4^n * (N + 1)), n = CKS.
	const u32 cks = SCIF_SCSMR2.full & 3;
	const u32 bitCycles = 4u * 32u * (SCIF_SCBRR2 + 1u) * (1u << (cks * 2));   // SH-4 cycles per bit
	const u32 frameBits = 1 + (8 - SCIF_SCSMR2.CHR) + SCIF_SCSMR2.PE + 1 + SCIF_SCSMR2.STOP;
	s_frameCycles = (s32)(bitCycles * frameBits);
}

static void startCharacter(u8 data)
{
	if (s_pipe)
		s_pipe->write(data);
	s_transmitting = true;
	s_txTimer = s_frameCycles;
}

void UpdateSCIF(u32 cycles)
{
	bool changed = false;

	// Transmitter: when a character time elapses, the next FIFO byte moves
	// into the shift register (and to the device).
	if (s_transmitting)
	{
		s_txTimer -= (s32)cycles;
		while (s_transmitting && s_txTimer <= 0)
		{
			if ((SCIF_SCFCR2.full & 4) == 0 && s_tx.count)
			{
				startCharacter(s_tx.pop());
				s_txTimer += s_frameCycles;
				if (txBelowTrigger())
					setStatus(SR_TDFE);
			}
			else
			{
				s_transmitting = false;
				setStatus(SR_TEND | SR_TDFE);
			}
			changed = true;
		}
	}

	// Receiver: one character per character time from the device.
	if (s_pipe)
	{
		s_rxTimer -= (s32)cycles;
		while (s_rxTimer <= 0)
		{
			s_rxTimer += s_frameCycles;
			if (s_pipe->available() > 0)
			{
				const u8 v = s_pipe->read();
				if (SCIF_SCSCR2.RE == 0 || (SCIF_SCFCR2.full & 2))
					continue;
				if (s_rx.full())
					SCIF_SCLSR2.ORER = 1;   // overrun: the byte is lost
				else
				{
					s_rx.push(v);
					if (rxAtTrigger())
						setStatus(SR_RDF);
				}
				changed = true;
			}
			else
			{
				// Line idle with bytes below the trigger level: data ready.
				if (s_rx.count && !(SCIF_SCFSR2.full & SR_DR))
				{
					setStatus(SR_DR);
					changed = true;
				}
				break;
			}
		}
	}

	if (changed)
		updateInterrupts();
}

void scif_SetPipe(ScifPipe* pipe)
{
	s_pipe = pipe;
	s_rxTimer = s_frameCycles;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------
// SCFTDR2 - transmit FIFO data
static void SerialWrite(u32 data)
{
	u8 v = (u8)data;
	if (SCIF_SCFCR2.full & 4)          // TFRST: FIFO held in reset
		return;
	if (SCIF_SCSMR2.CHR)
		v &= 0x7F;
	if (!s_transmitting && s_tx.count == 0 && SCIF_SCSCR2.TE)
	{
		// Empty line: the byte goes straight to the shift register.
		startCharacter(v);
		setStatus(SR_TDFE);
		updateInterrupts();
	}
	else if (!s_tx.full())
		s_tx.push(v);
}

// SCFSR2 read
static u32 ReadSerialStatus()
{
	s_statusLastRead = SCIF_SCFSR2.full;
	return SCIF_SCFSR2.full;
}

// SCFSR2 write: a flag is cleared by writing 0 after it was read as 1, and
// TDFE/RDF/DR stay set while their condition still holds.
static void WriteSerialStatus(u32 data)
{
	u16 keep = (u16)(data | ~0x00F3u | ~(u32)s_statusLastRead);
	if (txBelowTrigger())
		keep |= SR_TDFE;
	if (rxAtTrigger())
		keep |= SR_RDF;
	if (s_rx.count)
		keep |= SR_DR;
	SCIF_SCFSR2.full &= keep;
	s_statusLastRead &= keep;
	updateInterrupts();
}

// SCFRDR2 - receive FIFO data
static u32 ReadSerialData()
{
	if (s_rx.count == 0)
		return 0;
	return s_rx.pop();
}

// SCFDR2 - FIFO byte counts
static u32 Read_SCFDR2()
{
	return s_rx.count | (s_tx.count << 8);
}

// SCSCR2 - serial control
static void WriteSerialControl(u32 data)
{
	SCIF_SCSCR2.full = (u16)(data & 0x00FA);
	if (SCIF_SCSCR2.TE == 0)
		setStatus(SR_TEND);
	updateInterrupts();
}

// SCFCR2 - FIFO control (TFRST/RFRST reset the FIFOs, trigger levels)
static void WriteFifoControl(u32 data)
{
	const bool txWasReset = (SCIF_SCFCR2.full & 4) != 0;
	SCIF_SCFCR2.full = (u16)(data & 0x00FF);
	if (SCIF_SCFCR2.full & 4)
	{
		s_tx.clear();
		s_transmitting = false;
	}
	else if (txWasReset)
		setStatus(SR_TEND | SR_TDFE);
	if (SCIF_SCFCR2.full & 2)
		s_rx.clear();
	updateInterrupts();
}

static void WriteSerialMode(u32 data)
{
	SCIF_SCSMR2.full = (u16)(data & 0x007B);
	updateBaudRate();
}

static void WriteBitRate(u32 data)
{
	SCIF_SCBRR2 = (u8)data;
	updateBaudRate();
}

// SCSPTR2 - port pins: CTS reads as asserted (low)
static u32 ReadSerialPort()
{
	return SCIF_SCSPTR2.full & ~0x10u;
}

static void WriteSerialPort(u32 data)
{
	SCIF_SCSPTR2.full = (u16)(data & 0x00F3);
}

// SCLSR2 - overrun flag, cleared by writing 0
static void WriteLineStatus(u32 data)
{
	SCIF_SCLSR2.full &= (u16)(data | ~1u);
}

void scif_Init()
{
	// SCSMR2 - Serial mode register (R/W, 16-bit)
	SCIF[(SCIF_SCSMR2_addr & 0xFF) >> 2].flags         = REG_16BIT_READWRITE | REG_READ_DATA;
	SCIF[(SCIF_SCSMR2_addr & 0xFF) >> 2].readFunction  = 0;
	SCIF[(SCIF_SCSMR2_addr & 0xFF) >> 2].writeFunction = WriteSerialMode;
	SCIF[(SCIF_SCSMR2_addr & 0xFF) >> 2].data16        = &SCIF_SCSMR2.full;

	// SCBRR2 - Bit rate register (R/W, 8-bit)
	SCIF[(SCIF_SCBRR2_addr & 0xFF) >> 2].flags         = REG_8BIT_READWRITE | REG_READ_DATA;
	SCIF[(SCIF_SCBRR2_addr & 0xFF) >> 2].readFunction  = 0;
	SCIF[(SCIF_SCBRR2_addr & 0xFF) >> 2].writeFunction = WriteBitRate;
	SCIF[(SCIF_SCBRR2_addr & 0xFF) >> 2].data8         = &SCIF_SCBRR2;

	// SCSCR2 - Serial control register (R/W, 16-bit)
	SCIF[(SCIF_SCSCR2_addr & 0xFF) >> 2].flags         = REG_16BIT_READWRITE | REG_READ_DATA;
	SCIF[(SCIF_SCSCR2_addr & 0xFF) >> 2].readFunction  = 0;
	SCIF[(SCIF_SCSCR2_addr & 0xFF) >> 2].writeFunction = WriteSerialControl;
	SCIF[(SCIF_SCSCR2_addr & 0xFF) >> 2].data16        = &SCIF_SCSCR2.full;

	// SCFTDR2 - Serial transmit FIFO data register (Write ONLY, 8-bit)
	SCIF[(SCIF_SCFTDR2_addr & 0xFF) >> 2].flags         = REG_8BIT_READWRITE;
	SCIF[(SCIF_SCFTDR2_addr & 0xFF) >> 2].readFunction  = 0;
	SCIF[(SCIF_SCFTDR2_addr & 0xFF) >> 2].writeFunction = SerialWrite;
	SCIF[(SCIF_SCFTDR2_addr & 0xFF) >> 2].data8         = &SCIF_SCFTDR2;

	// SCFSR2 - Serial FIFO status register (R/W, 16-bit)
	SCIF[(SCIF_SCFSR2_addr & 0xFF) >> 2].flags         = REG_16BIT_READWRITE;
	SCIF[(SCIF_SCFSR2_addr & 0xFF) >> 2].readFunction  = ReadSerialStatus;
	SCIF[(SCIF_SCFSR2_addr & 0xFF) >> 2].writeFunction = WriteSerialStatus;
	SCIF[(SCIF_SCFSR2_addr & 0xFF) >> 2].data16        = &SCIF_SCFSR2.full;

	// SCFRDR2 - Serial receive FIFO data register (Read ONLY, 8-bit)
	SCIF[(SCIF_SCFRDR2_addr & 0xFF) >> 2].flags         = REG_8BIT_READWRITE;
	SCIF[(SCIF_SCFRDR2_addr & 0xFF) >> 2].readFunction  = ReadSerialData;
	SCIF[(SCIF_SCFRDR2_addr & 0xFF) >> 2].writeFunction = 0;
	SCIF[(SCIF_SCFRDR2_addr & 0xFF) >> 2].data8         = &SCIF_SCFRDR2;

	// SCFCR2 - Serial FIFO control register (R/W, 16-bit)
	SCIF[(SCIF_SCFCR2_addr & 0xFF) >> 2].flags         = REG_16BIT_READWRITE | REG_READ_DATA;
	SCIF[(SCIF_SCFCR2_addr & 0xFF) >> 2].readFunction  = 0;
	SCIF[(SCIF_SCFCR2_addr & 0xFF) >> 2].writeFunction = WriteFifoControl;
	SCIF[(SCIF_SCFCR2_addr & 0xFF) >> 2].data16        = &SCIF_SCFCR2.full;

	// SCFDR2 - Serial FIFO data count register (Read ONLY, 16-bit)
	SCIF[(SCIF_SCFDR2_addr & 0xFF) >> 2].flags         = REG_16BIT_READWRITE;
	SCIF[(SCIF_SCFDR2_addr & 0xFF) >> 2].readFunction  = Read_SCFDR2;
	SCIF[(SCIF_SCFDR2_addr & 0xFF) >> 2].writeFunction = 0;
	SCIF[(SCIF_SCFDR2_addr & 0xFF) >> 2].data16        = &SCIF_SCFDR2.full;

	// SCSPTR2 - Serial port register (R/W, 16-bit)
	SCIF[(SCIF_SCSPTR2_addr & 0xFF) >> 2].flags         = REG_16BIT_READWRITE;
	SCIF[(SCIF_SCSPTR2_addr & 0xFF) >> 2].readFunction  = ReadSerialPort;
	SCIF[(SCIF_SCSPTR2_addr & 0xFF) >> 2].writeFunction = WriteSerialPort;
	SCIF[(SCIF_SCSPTR2_addr & 0xFF) >> 2].data16        = &SCIF_SCSPTR2.full;

	// SCLSR2 - Line status register (R/W, 16-bit)
	SCIF[(SCIF_SCLSR2_addr & 0xFF) >> 2].flags         = REG_16BIT_READWRITE | REG_READ_DATA;
	SCIF[(SCIF_SCLSR2_addr & 0xFF) >> 2].readFunction  = 0;
	SCIF[(SCIF_SCLSR2_addr & 0xFF) >> 2].writeFunction = WriteLineStatus;
	SCIF[(SCIF_SCLSR2_addr & 0xFF) >> 2].data16        = &SCIF_SCLSR2.full;
}

void scif_Reset(bool Manual)
{
	/*
	Reset values per SH7091 hardware manual:
	SCSMR2  H'FFE80000  16  H'0000
	SCBRR2  H'FFE80004   8  H'FF
	SCSCR2  H'FFE80008  16  H'0000
	SCFTDR2 H'FFE8000C   8  Undefined
	SCFSR2  H'FFE80010  16  H'0060
	SCFRDR2 H'FFE80014   8  Undefined
	SCFCR2  H'FFE80018  16  H'0000
	SCFDR2  H'FFE8001C  16  H'0000
	SCSPTR2 H'FFE80020  16  H'0000
	SCLSR2  H'FFE80024  16  H'0000
	*/
	SCIF_SCSMR2.full  = 0x0000;
	SCIF_SCBRR2       = 0xFF;
	SCIF_SCSCR2.full  = 0x0000;
	SCIF_SCFSR2.full  = 0x0060;
	SCIF_SCFCR2.full  = 0x0000;
	SCIF_SCFDR2.full  = 0x0000;
	SCIF_SCSPTR2.full = 0x0000;
	SCIF_SCLSR2.full  = 0x0000;

	s_tx.clear();
	s_rx.clear();
	s_transmitting = false;
	s_statusLastRead = 0;
	updateBaudRate();
	s_rxTimer = s_frameCycles;
}

void scif_Term()
{
	// Nothing to clean up -- the attached device belongs to its owner.
}
