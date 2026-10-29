// NAOMI BIOS ROM image. The port never runs the BIOS, but Initial D3 copies
// the BIOS's runtime service library (28 KB at ROM offset 0x60000) into RAM
// and calls it, so a BIOS image has to be supplied. Any NAOMI or NAOMI 2
// BIOS carries the library.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace idas3 {

struct BiosImage
{
    std::vector<uint8_t> data;   // ROM bytes (2 MB)
    std::string source;          // "naomi.zip:epr-21578h.ic27", "epr-23605c.ic27"...
};

// Looks in each folder for, in order: naomi2.zip, naomi.zip (MAME BIOS
// sets, read directly), then a loose *.ic27 ROM. Fills `error` when nothing
// usable is found.
bool findBios(const std::vector<std::string>& folders, BiosImage& out, std::string& error);

// Loads one file: a .zip BIOS set or a raw ROM image.
bool loadBios(const std::string& path, BiosImage& out, std::string& error);

} // namespace idas3
