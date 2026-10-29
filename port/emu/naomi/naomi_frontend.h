// Port-facing lifecycle of the NAOMI 2 core (see naomi_frontend.cpp).
#pragma once

bool naomi_Init(bool useDynarec);
// Boots BIOS-free and runs until naomi_Stop() is called (typically from the
// host's VBlank hook).
void naomi_Run();
void naomi_Stop();
void naomi_Term();
