# Contributing

Bug reports and focused pull requests are welcome. See [README.md](README.md) for how to build.

**Never attach, link to, or commit** game images, BIOS or firmware dumps, PIC/CHD data, extracted assets, memory snapshots, or recordings of the game. Test only with materials you are legally permitted to use.

A useful bug report includes:

- The launcher options you used (they are saved in `options.txt`).
- What you were doing (menu, car select, race, course) and what went wrong.
- The newest `idas3wii_log.txt` from next to `boot.dol`, and whether you ran on a Wii, a vWii or Dolphin.

Code contributions are accepted under the project's licensing (see [port/LICENSE.md](port/LICENSE.md)). Do not paste code from projects with an incompatible licence.

## Building

**Wii:** devkitPro with devkitPPC and libogc. Run `port\build_wii.bat`, or run `make` in
`port/wii` from a devkitPro shell. It builds the SD card layout in `port/wii/dist/`
(`apps/initiald3wii/boot.dol`): copy its contents to the root of the card.

**Windows (debug build and tests):** Visual Studio 2022 or Build Tools with the C++ workload.
Run `port\build_windows.bat`, or from an x64 Native Tools prompt:

```
cmake -S port -B build-port -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-port
ctest --test-dir build-port --output-on-failure
```

The tests need no game data: they build a synthetic GD-ROM CHD with a fake encrypted image
and run the whole load pipeline on it. See [port/README.md](port/README.md) for the source
map and details.

## Repository layout

- `port/src/` – launcher: CHD / PIC / DES handling, menus, game runner.
- `port/emu/` – emulator core: SH-4 interpreter and PowerPC dynarec, PVR → GX renderer,
  AICA / ARM7 sound, and the NAOMI 2 layer (`naomi/`).
- `port/third_party/` – libchdr and its dependencies (LZMA, zlib, zstd, dr_flac).
- `port/wii/` – Wii Makefile, `meta.xml`, icon.
- `port/tests/` – tests that need no game data.
