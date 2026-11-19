[![Discord](https://img.shields.io/discord/286429969104764928?label=NullDC4wii&logo=discord&logoColor=FFFFFF)](https://discord.gg/sJst6jmQyH)

# Initial D Arcade Stage 3 Emulator for Wii

Forked from https://github.com/distilledorion-sketch/Initial-D-Arcade-Stage-3-Native-SH-4-Recompilation-WIP-
and build around https://github.com/BenoitAdam/nullDC4Wii

An experimental **NAOMI 2 → Wii** port of *Initial D Arcade Stage 3* (GDS-0033), built on
an emulator core forked from nullDC4Wii with a NAOMI 2
hardware layer on top (DIMM board, JVS I/O, ELAN 3D chip). It runs on a Wii or vWii through
the Homebrew Channel. A Windows build of the same launcher is kept for debugging.

> **Speed Boy 3 is an independent open-source software project. It is not affiliated with,
> endorsed by, or sponsored by Sega, Kodansha, Shuichi Shigeno, or Nintendo. "Initial D" and
> "Initial D Arcade Stage 3" are registered trademarks of their respective owners. This
> repository contains no game assets, copyrighted code, or system BIOS. Users must supply
> their own legally obtained ROM dumps to run this software.**

**Status: beta (1.00).** The game boots to its menus and races are playable, but speed on real
hardware is not yet full. Menus and attract mode run at full speed; races currently run
around 50/60 fps with every frame drawn. Expect bugs and frequent changes.

## Requirements

- A Wii or vWii with the Homebrew Channel, and an SD card or USB drive (FAT32).
- Three files that you must supply yourself, from hardware and media you legally own:

| File | What it is |
| --- | --- |
| `gds-0033.chd` | The *Initial D Arcade Stage 3* (NAOMI 2) GD-ROM, as a CHD. Under 2 GB on the Wii. |
| `317-0384-com.pic` | The game's security PIC dump. Its CRC32 is checked. |
| `naomi.zip` | A NAOMI / NAOMI 2 BIOS set (MAME layout). Only the service library is read, and it is streamed from the zip. |

None of these files are included in this repository or in the releases, and none may be
submitted to it.

## Quick start

1. Download the latest version of the software from the
   [**Releases**](https://github.com/BenoitAdam/Initial-D-3-wii/releases) page and extract it.
2. Copy the contents to the root of your SD card (or USB drive):

   ```
   sd:/apps/initiald3wii/boot.dol, meta.xml, icon.png
   sd:/initiald3/            <- put your own gds-0033.chd, 317-0384-com.pic and naomi.zip here
   ```

   `usb:/initiald3/` works too, and `sd:/naomi/` and `sd:/roms/` are also searched. Press **2**
   in the file browser to switch between SD and USB.
3. Start **Speed Boy 3** from the Homebrew Channel.
4. Pick `gds-0033.chd` in the file browser. The launcher checks the CHD, the PIC and the BIOS
   and shows a green or red line for each. Adjust the options if you want, then launch.
5. The log is written next to `boot.dol` (`idas3wii_log.txt`, the previous run in
   `idas3wii_log_prev.txt`). Please attach it to bug reports.

### Controls

| Action | GameCube | Wii Remote + Nunchuk | Classic Controller |
| --- | --- | --- | --- |
| Steer | Control stick | Stick | Left stick |
| Gas / brake | R / L | A / B | R / L |
| Shift up / down | A / B | Z / C | a / b |
| Change view | X | 1 | x |
| Start | Start | + | + |
| Coin | Z | − | − |
| Test / service | D-pad left / right | | |
| Quit to the loader | Start + Z + Y | Home | Home |

A Wii Remote on its own also works (held sideways). Up to four controllers of each kind can be
connected at once. Menu navigation: D-pad to move, **A** to select, **B** to go back. To start
a race, insert a coin, then press Start. The full list, including the launcher's keys, is in
[control.txt](control.txt).

## 2 players splitscreen !

Set preset to "2 players" and enter any race. Controler 2 control the opponent !

Specific presets :

| Option |	Default |	Alternative behavior |
| --- | --- | --- |
| P2 CAR |	FULL PHYSICS |	ON RAILS (Assisted driving) |
| P2 SCENERY |	AROUND EACH CAR	| AROUND P1 (Maybe more FPS) |

Known limits:

* Player 2 can't shift by hand. If you pick a manual gearbox, player 2 stays in its gear; automatic works.
* Player 2's HUD still shows player 1's gear and ADVANTAGE.
* Player 2's view still shows its own bonnet.

## Options

The launcher's option pages let you trade accuracy for speed: the SH-4 CPU mode (dynamic
recompiler or interpreter), GPU or CPU 3D transform and lighting, frameskipping, texture
cache quality, depth and sorting behaviour, audio sync, and a set of emulator fixes. Choices
are saved by name in `options.txt` next to `boot.dol`. An on-screen speed overlay and a
profiler that writes timings to the log are available for performance testing.

Music tempo follows emulation speed, so it only sounds right when the game runs at 100%.

## FAQ

### Is this an Emulator or a Recomp ?

It's an **Emulator**. Initially I was thinking I was doing a recomp but by asking to use NullDC4Wii to help, Claude understood to make it the emulator way. It's not really important (except to get full 100% speed), but maybe someone will do a recomp one day.

### How does a Wii Emulate NAOMI 2 ? It has more Ram than Wii !

Yes but all Ram isn't use everytime, so there is dynamic stuff to take only whats necessary

### Will you make other NAOMI 2 games ?

No, me and my friend are just big fans of **Initial D !**

### What's NullDC4Wii ?

It's a Dreamcast emulator for Wii 
https://github.com/BenoitAdam/nullDC4Wii

### Can I Fork your project ?

Of course you are free to, but please contact me anyway when doing a side-project

### Do you have a discord ?

Yes : https://discord.gg/sJst6jmQyH

### Do you accept donation ?

Yes ! Initially I was against donation but spending this much time & effort (and sometimes money) to this project led me to ask for donation. Not trying to getting rich, just staying afloat.

#### Buy Me a Coffee / Ko-Fi

This is very popular amongst small dev project, I don't know what's the difference or if some people would prefer one over the other one, anyway here are the links : 

- https://buymeacoffee.com/nulldc4wii
- https://ko-fi.com/nulldc4wii

#### Patreon

The classic "Patreon" subscribing is here :

https://www.patreon.com/cw/NullDC4Wii

#### Paypal

For those used to Paypal, the link is here :
https://www.paypal.com/ncp/payment/2NE7AYS77K6AJ

## Reporting problems

Open a GitHub issue with the launcher options you used, what you were doing, and the newest
`idas3wii_log.txt`. **Do not attach or link game images, BIOS dumps, PIC/CHD data, extracted
assets, memory dumps or recordings of the game.**

## Credits and licenses

- nullDC4Wii (MIT, same author) is the basis of the emulator core.
- [Flycast](https://github.com/flyinghead/flycast) and [MAME](https://www.mamedev.org/)
  documented NAOMI / NAOMI 2 hardware behaviour. The DES code follows MAME's `naomigd.cpp`
  (BSD-3-Clause, Olivier Galibert).
- The ARM7 sound core is adapted from VisualBoyAdvance (GPL v2 or later).
- libchdr (BSD-3-Clause), the LZMA SDK (public domain), zlib, zstd (BSD-3-Clause) and dr_flac
  (public domain / MIT-0).

Because the ARM7 core is GPL, the Wii binary as a whole is distributed under the
**GNU General Public License v2**. The full text is in [port/COPYING](port/COPYING); the
individual licences are listed in [port/LICENSE.md](port/LICENSE.md). Source files keep their
own licence headers.

## Legal

Speed Boy 3 is an independent open-source software project. It is not affiliated with,
endorsed by, or sponsored by Sega, Kodansha, Shuichi Shigeno, or Nintendo. "Initial D" and
"Initial D Arcade Stage 3" are registered trademarks of their respective owners. This
repository contains no game assets, copyrighted code, or system BIOS. Users must supply their
own legally obtained ROM dumps to run this software. See also [LEGAL.md](LEGAL.md).
