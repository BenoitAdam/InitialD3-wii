// Everything between "the user picked gds-0033.chd" and "the program is in
// NAOMI RAM, ready for the SH-4 runtime": open the CHD, find and check the
// security PIC, locate and decrypt the game image, parse its boot header,
// allocate guest memory and copy the program segments.
//
// Shared by the Wii and Windows builds; reports progress as status lines so
// the menu can show each step as it happens.
#pragma once

#include "chd_gdrom.h"
#include "dimm_image.h"
#include "guest_memory.h"
#include "naomi_boot.h"
#include "naomi_pic.h"

#include <functional>
#include <string>
#include <vector>

namespace idas3 {

enum class CheckStatus
{
    Ok,
    Warn,
    Fail,
    Info,
};

struct CheckLine
{
    CheckStatus status;
    std::string text;
};

class GameSession
{
public:
    // Called after every new status line and while a long step runs
    // (`current` describes the running step, empty when idle).
    using Progress = std::function<void(const std::vector<CheckLine>& lines, const std::string& current)>;

    // Step 1: verify the CHD and its PIC, decrypt the boot header.
    bool check(const std::string& chdPath, const Progress& progress);
    // Step 2: allocate guest memory and load the program (after check()).
    bool load(MemoryPlan plan, const Progress& progress);
    // Releases guest memory and closes the image.
    void close();

    const std::vector<CheckLine>& lines() const { return lines_; }
    const NaomiBootHeader& header() const { return header_; }
    const std::string& chdPath() const { return chdPath_; }
    const std::string& picPath() const { return picPath_; }
    bool isSupportedGame() const { return supportedGame_; }
    bool checked() const { return checked_; }
    bool loaded() const { return loaded_; }

    uint32_t loadedBytes() const { return loadedBytes_; }
    uint32_t programCrc32() const { return programCrc_; }
    uint32_t loadMicroseconds() const { return loadUs_; }
    DimmImage& dimm() { return dimm_; }
    const ChdGdrom& gdrom() const { return gdrom_; }
    GuestMemory& memory() { return memory_; }

    // Locates the security PIC for a CHD: 317-0384-com.pic next to it, any
    // .pic next to it (one with the known CRC preferred), then the parent
    // folder. Empty when none is found.
    static std::string findPic(const std::string& chdPath);

private:
    void add(CheckStatus status, const std::string& text, const Progress& progress);

    std::string chdPath_;
    std::string picPath_;
    std::vector<CheckLine> lines_;
    ChdGdrom gdrom_;
    NaomiPic pic_;
    DimmImage dimm_;
    NaomiBootHeader header_;
    GuestMemory memory_;
    bool checked_ = false;
    bool loaded_ = false;
    bool supportedGame_ = false;
    uint32_t loadedBytes_ = 0;
    uint32_t programCrc_ = 0;
    uint32_t loadUs_ = 0;
};

// Small path helpers shared with the UI ('/' and '\\' both accepted).
std::string pathDirectory(const std::string& path);   // with trailing separator
std::string pathFileName(const std::string& path);
std::string toLower(std::string s);

} // namespace idas3
