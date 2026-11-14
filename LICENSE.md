# Licence

Copyright (C) 2026 BenoitAdam94 and the contributors to Speed Boy 3 (Initial D 3 for Wii).

The project is distributed under the **GNU General Public License, version 2** (GPL-2),
because the combined program includes GPL-2-or-later code (the ARM7 interpreter in
`port/emu/plugs/vbaARM/`). The full licence text is in [port/COPYING](port/COPYING).

This program is free software; you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation; either version 2 of the
License, or (at your option) any later version. It is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

## Components with their own licences

Each source file keeps its own licence header. The licences of the components are listed in
[port/LICENSE.md](port/LICENSE.md):

| Component | Licence |
| --- | --- |
| Launcher, NAOMI 2 layer and the nullDC4Wii-derived core (`port/`, except the lines below) | MIT (see [port/emu/LICENSE](port/emu/LICENSE)), usable under GPL-2 |
| ARM7 interpreter adapted from VisualBoyAdvance (`port/emu/plugs/vbaARM/`) | GPL v2 or later |
| DES tables and algorithm after MAME (`port/src/core/naomi_des.*`) | BSD-3-Clause, Olivier Galibert |
| libchdr (`port/third_party/libchdr/`) | BSD-3-Clause |
| LZMA SDK 19.00 | Public domain |
| zlib 1.3.1 (inflate only) | zlib licence |
| zstd 1.5.6 (decompression only) | BSD-3-Clause |
| dr_flac | Public domain / MIT-0 |

The MIT-licensed files remain available under the MIT licence on their own.

## No game data

This repository contains no game images, BIOS, security data (PIC), extracted assets or other
copyrighted inputs, and grants no right to them. See [LEGAL.md](LEGAL.md).

"Initial D" and "Initial D Arcade Stage 3" are trademarks of their respective owners. This project
is not affiliated with, endorsed by, or sponsored by Sega, Kodansha, Shuichi Shigeno, or Nintendo.
