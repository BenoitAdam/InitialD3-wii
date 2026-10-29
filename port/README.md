# Initial D 3 for Wii: developer notes

This folder holds all the code. It builds for the **Wii** (devkitPPC + libogc) and for
**Windows** (MSVC) from the same sources. For installation and usage see the
[top-level README](../README.md). The licences are in [LICENSE.md](LICENSE.md).

Nothing here contains game data. You need your own `gds-0033.chd`, `317-0384-com.pic` and a
NAOMI BIOS zip to run the game.

## How it works

1. **Launcher** (`src/`): a text-mode menu shared by both platforms. It opens the CHD
   (libchdr), checks the GD-ROM layout, reads `317-0384-com.pic` and checks its CRC32, finds
   the encrypted game image named by the PIC in the GD-ROM's ISO9660 file system, and finds
   the BIOS zip. It checks that the game is Initial D Arcade Stage 3 (NAOMI 2).
2. **On-demand decryption** (`src/core/dimm_image.*`): the DIMM-board DES decryption runs
   straight from the CHD as the game reads, so the image (hundreds of MB) is never held in RAM.
3. **BIOS-free boot** (`emu/naomi/naomi_hle_boot.cpp`): the launcher copies the program
   segments to SH-4 RAM exactly as the NAOMI BIOS would, leaves the boot header where the
   game expects it, and jumps to the entry point. Only the BIOS's service library
   (a small part of the ROM) is read from the user's BIOS zip.
4. **Emulator core** (`emu/`): a fork of nullDC4Wii (SH-4 interpreter and PowerPC dynarec,
   PowerVR to GX renderer, AICA and ARM7 sound), built with `DC_PLATFORM=2` for the NAOMI 2
   memory map, plus the NAOMI 2 layer in `emu/naomi/`: DIMM board, JVS I/O board and MIE
   bridge, card reader, and the ELAN 3D chip.
5. **ELAN 3D**: NAOMI 2 transforms and lights its geometry on the ELAN chip. On the Wii,
   `emu/naomi/naomi_elan_gx.*` hands opaque lists to GX's hardware transform and lighting,
   and the GPU fetches vertices straight from the guest's memory (`naomi_elan_fetch.inc`).
   Remaining lists are transformed in software (`naomi_elan_tnl.cpp`).
6. **Memory** (`emu/wii/wii_fastmem.*`): NAOMI 2 needs 88 MB (32 MB RAM, 16 MB VRAM, 8 MB
   sound RAM, 32 MB ELAN RAM), which is more than a Wii has free. Guest memory is mapped by
   the MMU and paged on demand, with a compressed page store for pages that are rarely used.

## Building

**Wii:** devkitPro with devkitPPC and libogc.

```
port\build_wii.bat            (runs make && make package inside devkitPro's shell)
cd port/wii && make           (from a devkitPro shell)
```

Output: `port/wii/boot.dol`, and the SD card layout in `port/wii/dist/` after `make package`.

**Windows:** Visual Studio 2022 or Build Tools with the C++ workload (CMake and Ninja
included).

```
port\build_windows.bat        (builds into build-port\ and runs the tests)
```

Manual equivalent from an x64 Native Tools prompt:

```
cmake -S port -B build-port -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-port
ctest --test-dir build-port --output-on-failure
```

Targets: `idas3wii` (launcher with the same text screens as the Wii), `idas3wii_run`
(headless runner, below) and `idas3wii_tests`.

## Tests

`idas3wii_tests` needs no game data. It builds a **synthetic GD-ROM CHD** (uncompressed
CHD v5 with the real 3-track GD-ROM layout, an ISO9660 file system, a DES-encrypted fake
NAOMI 2 image and a matching 16 KB PIC) and runs the whole load pipeline on it:

- DES matches the FIPS known-answer vector, and little-endian block handling is checked.
- PIC parsing, ISO9660 name matching and the 256-byte descriptor-file indirection.
- Random-access decrypted reads across sector and run boundaries, and past the end.
- Program load into RAM, boot-header copy and extraction.
- Failure paths: wrong key, missing PIC, a file that is not a CHD.

The compressed CHD codecs are compiled from libchdr but the fixture exercises only the
uncompressed path.

## Windows tools

The launcher has a headless mode, useful with a real CHD:

```
idas3wii.exe --check D:\naomi\gds-0033.chd                  verify only
idas3wii.exe --check ... --load --plan full|noelan|ram      verify and load the program
idas3wii.exe --check ... --dump 0 0x500                     hexdump decrypted bytes
idas3wii.exe --check ... --extract dimm.bin                 write the decrypted image
idas3wii.exe --dir D:\naomi --keys "A,WAIT,A,A,B" --dump-screens screens.txt
                                                            drive the menu from a script
```

`idas3wii_run` boots the core without a window and runs N emulated frames, which makes it the
main bring-up and profiling tool on a PC. It can dump RAM, VRAM, sound RAM and ELAN RAM,
write frame snapshots, trace PCs, press buttons on a schedule (`--press`) and compare the
GPU transform path against the software path (`--gpu-tnl`). Read `src/run/run_main.cpp` for
the full list of options.

## Profiling on the Wii

Turn on **SPEED OVERLAY** and **PROFILER** in the launcher's options. The profiler writes
per-second lines (SH-4, ELAN, render, sound and texture time, paging, GPU counters) to
`idas3wii_log.txt`. Attach that log to reports. Options are remembered in `options.txt`.
What has been optimised so far, and what is left, is in [OPTIMIZATION.md](OPTIMIZATION.md).

## Source map

```
port/
  src/main.cpp                  launcher entry point, headless CLI
  src/run/run_main.cpp          headless NAOMI 2 runner (Windows)
  src/core/chd_gdrom.*          GD-ROM over CHD: track map, FAD to hunk/frame, 2048-byte user data
  src/core/naomi_pic.*          security PIC: DES key and game file name
  src/core/naomi_des.*          DIMM-board DES
  src/core/dimm_image.*         ISO9660 lookup and on-demand decrypted image
  src/core/naomi_boot.*         NAOMI boot header
  src/core/guest_memory.*       NAOMI 2 memory regions and SH-4 address mapping
  src/core/game_session.*       the check and load pipeline, reported as status lines
  src/core/bios_image.*         finds the BIOS service library in the user's zip
  src/game/                     game runner and the launcher's option table
  src/ui/                       the menus (shared by both platforms)
  src/platform/                 platform interface, Wii (libogc) and Windows implementations
  emu/                          emulator core (nullDC4Wii fork)
  emu/naomi/                    NAOMI 2 layer: DIMM board, JVS, card reader, ELAN, HLE boot
  emu/wii/                      Wii back-ends: fast memory, audio, exit handling, PPC dynarec driver
  emu/plugs/                    renderer (GX), AICA, ARM7, Maple devices
  tests/                        synthetic-CHD fixture and tests
  third_party/libchdr/          libchdr with LZMA, zlib (inflate), zstd (decompress), dr_flac
  wii/                          Makefile, meta.xml, icon.png
```

All game data is little-endian and the Wii is big-endian, so game bytes are only read
through `src/core/bytes.h`.

## Known limits

- Races run at roughly 40 fps with every frame drawn on a real Wii. Frameskipping brings the
  game's logic to full speed, but music tempo follows emulation speed, so it sounds right
  only at 100%.
- Specular highlights are not applied on the GPU transform path.
- Memory is tight in races. The compressed page store and the guest-memory page pool were
  tuned on a real console; very different homebrew setups (large heaps, other loaders) may
  run out.
- CHD files must be under 2 GB on the Wii (newlib's 32-bit file offsets).
