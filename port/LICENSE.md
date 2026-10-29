# Licensing of `port/`

The Wii and Windows builds combine code under several licenses. The combined binary is
distributed under the **GNU General Public License, version 2** (see [COPYING](COPYING)),
because it includes GPL v2+ code. Each source file keeps its own license.

| Code | License |
| --- | --- |
| Launcher and NAOMI 2 layer (`src/`, `emu/naomi/`, `emu/wii/`, tests), original to this project | MIT (see `emu/LICENSE`), usable under GPL-2 in the combined binary |
| nullDC4Wii core (`emu/` outside the lines below) | MIT (see `emu/LICENSE`) |
| ARM7 interpreter adapted from VisualBoyAdvance (`emu/plugs/vbaARM/`) | GPL v2 or later |
| DES tables and algorithm after MAME `naomigd.cpp` (`src/core/naomi_des.*`) | BSD-3-Clause, Olivier Galibert |
| libchdr (`third_party/libchdr/`) | BSD-3-Clause (see its `LICENSE.txt`) |
| LZMA SDK 19.00 | Public domain |
| zlib 1.3.1 (inflate only) | zlib license |
| zstd 1.5.6 (decompression only) | BSD-3-Clause (see its `LICENSE`) |
| dr_flac | Public domain / MIT-0 |

The Flycast and MAME sources were used as documentation of the hardware. Where a file says
its behaviour follows them, it was reimplemented rather than copied.

This project is not affiliated with Sega, Kodansha, Shuichi Shigeno, or Nintendo, and no game
data, BIOS, PIC or CHD file is included. See `../LEGAL.md`.
