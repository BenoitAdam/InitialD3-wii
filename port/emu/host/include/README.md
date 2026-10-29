Windows-only stand-ins for the few libogc headers the shared nullDC4Wii
sources include (timebase, vsync wait, pad scan, cache flush). This folder is
on the include path of the MSVC build only; the Wii build uses real libogc.
