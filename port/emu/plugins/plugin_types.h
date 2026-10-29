#pragma once

#ifndef _PLUGIN_HEADER_
#error beef
#endif

//******************************************************
//******************* Plugin system ********************
//******************************************************

enum PluginType
{
	Plugin_ARM = 1,
	Plugin_GFX = 2,
	Plugin_SPU = 3,
	Plugin_SH4 = 4,
};

#define PLUGIN_I_F_VERSION     1
#define ARM_PLUGIN_I_F_VERSION 1

#define rv_ARM_ok 0

// Callbacks the ARM plugin calls back into the emulator core with
typedef void (*MenuCallback)(u32 id, void* wnd, void* param);

struct emu_info
{
	void* RootMenu;
	void  (*AddMenuItem)(void* menu, int pos, const char* label, MenuCallback cb, u32 id);
	int   (*ConfigLoadInt)(const char* section, const char* key, int defval);
	void  (*ConfigSaveInt)(const char* section, const char* key, int val);
};

// Passed to armInit() — gives the plugin access to AICA RAM and register callbacks
struct arm_init_params
{
	u8*   aica_ram;
	u32   (*ReadMem_aica_reg) (u32 addr, u32 size);
	void  (*WriteMem_aica_reg)(u32 addr, u32 data, u32 size);
};

struct common_plugin_interface
{
	u32        InterfaceVersion;
	char       Name[128];
	PluginType Type;

	s32  (FASTCALL *Load)  (emu_info* em);
	void (FASTCALL *Unload)(void);
};

struct arm_plugin_interface
{
	s32  (FASTCALL *Init)              (arm_init_params* p);
	void (FASTCALL *Reset)             (bool manual);
	void (FASTCALL *Term)              (void);
	void (FASTCALL *Update)            (u32 cycles);
	void (FASTCALL *ArmInterruptChange)(u32 bits, u32 L);
	void (*ExeptionHanlder)            (void);   // typo kept intentionally for ABI compat
	void (FASTCALL *SetResetState)     (u32 state);
};

struct plugin_interface
{
	u32                    InterfaceVersion;
	common_plugin_interface common;
	arm_plugin_interface    arm;
};


/* There are for easy porting from ndc */
#define DC_PLATFORM_NORMAL		0
#define DC_PLATFORM_NAOMI2		2
// The Initial D3 port builds this core as a NAOMI 2 (-DDC_PLATFORM=2).
#ifndef DC_PLATFORM
#define DC_PLATFORM DC_PLATFORM_NORMAL
#endif

#if (DC_PLATFORM==DC_PLATFORM_NORMAL)

	#define BUILD_DREAMCAST 1
	#define FLASH_SIZE (128*1024)

	//DC : 16 mb ram, 8 mb vram, 2 mb aram, 2 mb bios, 128k flash
	#define RAM_SIZE (16*1024*1024) // 16MB RAM
	#define VRAM_SIZE (8*1024*1024) // 8MB VRAM
	#define ARAM_SIZE (2*1024*1024)
	#define BIOS_SIZE (2*1024*1024)

#elif (DC_PLATFORM==DC_PLATFORM_NAOMI2)

	#define BUILD_NAOMI 1
	#define BUILD_NAOMI2 1
	// The area-0 window at 0x00200000 is the 32 KB battery-backed SRAM on NAOMI.
	#define FLASH_SIZE (32*1024)

	// NAOMI 2: 32 MB RAM, 2x16 MB VRAM (one emulated, as in Flycast), 8 MB
	// AICA RAM, 2 MB BIOS (unused: the port boots BIOS-free), 32 MB ELAN RAM.
	#define RAM_SIZE (32*1024*1024)
	#define VRAM_SIZE (16*1024*1024)
	#define ARAM_SIZE (8*1024*1024)
	#define BIOS_SIZE (2*1024*1024)
	// Backing store for that 2 MB window: the game only reads the BIOS
	// service library (0x60000-0x67000), so 512 KB are kept and the rest of
	// the window mirrors them (saves 1.5 MB of the Wii's heap).
	#define BIOS_STORE_SIZE (512*1024)
	#define ELAN_RAM_SIZE (32*1024*1024)

#else
	#error invalid build config
#endif

#ifndef BIOS_STORE_SIZE
#define BIOS_STORE_SIZE BIOS_SIZE
#endif

#define RAM_MASK	(RAM_SIZE-1)
#define VRAM_MASK	(VRAM_SIZE-1) // 0x7FFFFF
#define ARAM_MASK	(ARAM_SIZE-1)
#define BIOS_MASK	(BIOS_SIZE-1)
#define FLASH_MASK	(FLASH_SIZE-1)

#define SH4_CLOCK (200*1000*1000) // 200 Mhz (nominal Dreamcast SH4 clock)

// SH4 underclock preset (wii/main.cpp get_sh4_clock_preset / game_presets
// `sh4_clock`): effective SH4 clock in MHz, 150..200. Emulation timing/pacing
// (SPG video, arm_aica sample step, RTC, DMA) divides by SH4_CLOCK_EFF instead
// of the nominal SH4_CLOCK so that underclocking scales the audio/video/RTC
// anchors together — fewer emulated SH4 cycles are budgeted per frame, giving
// the Wii host more headroom, at the cost of the emulated machine behaving like
// a slower Dreamcast (CPU-heavy games drop internal frames). Defaults to 200,
// so with the preset off SH4_CLOCK_EFF == SH4_CLOCK exactly. Kept as a call
// (not a stored constant) so a live menu change takes effect immediately.
#ifdef __cplusplus
extern "C" {
#endif
int get_sh4_clock_preset(void); // effective SH4 clock in MHz (150..200)
#ifdef __cplusplus
}
#endif
#define SH4_CLOCK_EFF ((u32)(get_sh4_clock_preset() * 1000000))

enum ndc_error_codes
{
	rv_ok = 0,		//no error

	rv_error=-2,	//error
	rv_serror=-1,	//silent error , it has been reported to the user
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////

//******************************************************
//*********************** PowerVR **********************
//******************************************************

struct pvr_init_params
{
	HollyRaiseInterruptFP*	RaiseInterrupt;

	//Vram is allocated by the emu.A pointer is given to the buffer here :)
	u8*					vram; 
};

//******************************************************
//************************ GDRom ***********************
//******************************************************
enum DiscType
{
	CdDA=0x00,
	CdRom=0x10,
	CdRom_XA=0x20,
	CdRom_Extra=0x30,
	CdRom_CDI=0x40,
	GdRom=0x80,		
	NoDisk=0x1,
	Open=0x2,			//tray is open :)
	Busy=0x3			//busy -> needs to be autmaticaly done by gdhost
};

enum DiskArea
{
	SingleDensity,
	DoubleDensity
};

enum DriveEvent
{
	DiskChange=1	//disk ejected/changed
};

//passed on GDRom init call
struct gdr_init_params
{
	char* source;
};
void NotifyEvent_gdrom(u32 info,void* param);

//******************************************************
//************************ AICA ************************
//******************************************************

//passed on AICA init call
struct aica_init_params
{
	HollyRaiseInterruptFP*	RaiseInterrupt;
	
	u8*				aica_ram;
	u32*			SB_ISTEXT;			//SB_ISTEXT register , so that aica can cancel interrupts =)
	HollyCancelInterruptFP* CancelInterrupt;
};

//******************************************************
//****************** Maple devices ******************
//******************************************************

//wait, what ?//

//******************************************************
//********************* Ext.Device *********************
//******************************************************


//passed on Ext.Device init call
struct ext_device_init_params
{
	HollyRaiseInterruptFP*	RaiseInterrupt;
	u32* SB_ISTEXT;
	HollyCancelInterruptFP* CancelInterrupt;
};