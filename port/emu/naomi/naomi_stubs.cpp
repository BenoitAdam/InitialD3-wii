// Pieces of the Dreamcast front end the shared core still references but a
// NAOMI 2 does not have: the GD-ROM image plugin, the Dreamcast controller
// family (the NAOMI's input is the JVS I/O board, see naomi_jvs.cpp), the
// in-game exit combo poll and the disc-read census. Both builds.
#include "types.h"
#include "plugins/plugin_manager.h"

// ---- GD-ROM image plugin (ImgReader in nullDC4Wii) ----------------------
s32 FASTCALL libGDR_Load() { return 0; }
void FASTCALL libGDR_Unload() {}
s32 FASTCALL libGDR_Init(gdr_init_params*) { return 0; }
void FASTCALL libGDR_Reset(bool) {}
void FASTCALL libGDR_Term() {}
void FASTCALL libGDR_ReadSector(u8*, u32, u32, u32) {}
void FASTCALL libGDR_ReadSubChannel(u8*, u32, u32) {}
void FASTCALL libGDR_GetToc(u32*, u32) {}
u32 FASTCALL libGDR_GetDiscType() { return 0; }
void FASTCALL libGDR_GetSessionInfo(u8*, u8) {}

extern "C" void disc_census_dump(double) {}
extern "C" void ExitCombo_HostPoll() {}

// ---- Dreamcast controllers (drkMapleDevices in nullDC4Wii) ---------------
// Kept so dc/maple links; every pad reads as "no button pressed".
u16 kcode[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
s8 joyx[4];
s8 joyy[4];
u8 rt[4];
u8 lt[4];

void InitControllers() {}
void InitLightGun() {}
void InitMaracas() {}
void InitKeyboard() {}
void InitFishingRod() {}
void UpdateInputState(u32) {}
void UpdateLightGunState(u32) {}
void UpdateMaracasState(u32) {}
void UpdateKeyboardState(u32) {}
void UpdateFishingRodState(u32) {}

struct DC_KeyboardReport;
extern "C" u16 GetLightGunX(int) { return 0; }
extern "C" u16 GetLightGunY(int) { return 0; }
extern "C" const DC_KeyboardReport* GetKeyboardReport(int) { return nullptr; }
