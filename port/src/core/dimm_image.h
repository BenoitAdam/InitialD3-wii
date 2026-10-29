// The decrypted game image, as the NAOMI DIMM board presents it to the game.
//
// On real hardware the DIMM board copies the encrypted file from the GD-ROM
// into its RAM and decrypts it. Flycast keeps the whole decrypted image in
// host memory; that is impossible on the Wii (88 MB total RAM), so this class
// decrypts on demand straight from the CHD: any (offset, length) read is
// served by reading the covering GD-ROM sectors and DES-decrypting them.
#pragma once

#include "chd_gdrom.h"
#include "naomi_des.h"
#include "naomi_pic.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace idas3 {

struct IsoEntry
{
    std::string name;
    uint32_t lba = 0;
    uint32_t size = 0;
    bool isDir = false;
};

class DimmImage
{
public:
    // Locates the encrypted game file named by the PIC and prepares the key.
    bool open(ChdGdrom& gdrom, const NaomiPic& pic, std::string& error);
    void close();
    bool isOpen() const { return gdrom_ != nullptr; }

    // Decrypted read. Bytes past the end of the file read as zero, like the
    // DIMM RAM past the loaded image.
    bool read(uint32_t offset, uint32_t length, uint8_t* dst);

    uint32_t size() const { return fileSize_; }
    uint32_t fileLba() const { return fileLba_; }
    const std::string& fileName() const { return fileName_; }
    const std::vector<IsoEntry>& rootEntries() const { return rootEntries_; }

    // Writes the whole decrypted image to `path`. The callback receives
    // (bytesDone, bytesTotal) and may return false to cancel.
    bool extract(const char* path, const std::function<bool(uint32_t, uint32_t)>& progress,
                 std::string& error);

private:
    bool readHd(uint32_t lba, uint8_t* dst2048);

    ChdGdrom* gdrom_ = nullptr;
    NaomiDes des_;
    std::string fileName_;
    uint32_t fileLba_ = 0;
    uint32_t fileSize_ = 0;
    std::vector<IsoEntry> rootEntries_;

    // Last decrypted run of sectors (sequential reads are the common case).
    static constexpr uint32_t kRunSectors = 8;
    std::vector<uint8_t> run_;
    uint32_t runFirst_ = ~0u;
    uint32_t runCount_ = 0;
};

// Parses one 2048-byte ISO9660 directory sector (exposed for tests).
std::vector<IsoEntry> parseIsoDirectorySector(const uint8_t* sector);

// MAME/Flycast name matching: exact, or "*.EXT" style suffix match.
bool isoNameMatches(const std::string& fileName, const char* pattern);

} // namespace idas3
