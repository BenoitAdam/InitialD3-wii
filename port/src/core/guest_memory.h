// NAOMI 2 guest memory regions.
//
// Real NAOMI 2 hardware: 32 MB SH-4 RAM, 2x16 MB PowerVR VRAM (only one is
// needed, as in Flycast), 8 MB AICA sound RAM and 32 MB ELAN RAM: 88 MB in
// total, which is the whole of the Wii's RAM (24 MB MEM1 + 64 MB MEM2, part
// of MEM2 being owned by IOS). The plans below let the launcher measure what
// actually fits on a given console.
#pragma once

#include <cstdint>
#include <string>

namespace idas3 {

enum class MemoryPlan
{
    Full,      // RAM + VRAM + ARAM + ELAN (88 MB)
    NoElan,    // RAM + VRAM + ARAM (56 MB)
    RamOnly,   // RAM (32 MB): enough to load the program
};

constexpr uint32_t kNaomiRamBase = 0x0c000000;
constexpr uint32_t kNaomiRamSize = 32u << 20;
constexpr uint32_t kNaomiVramSize = 16u << 20;
constexpr uint32_t kNaomiAramSize = 8u << 20;
constexpr uint32_t kNaomiElanSize = 32u << 20;

const char* memoryPlanLabel(MemoryPlan plan);
uint32_t memoryPlanBytes(MemoryPlan plan);

class GuestMemory
{
public:
    GuestMemory() = default;
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // On failure `error` names the region that did not fit and how much
    // memory was left.
    bool allocate(MemoryPlan plan, std::string& error);
    void release();

    uint8_t* ram() const { return regions_[0].data; }
    uint8_t* vram() const { return regions_[1].data; }
    uint8_t* aram() const { return regions_[2].data; }
    uint8_t* elan() const { return regions_[3].data; }
    uint32_t allocatedBytes() const;

    // Host pointer for an SH-4 address in system RAM (any P0-P3 mirror),
    // nullptr when [address, address+size) is not entirely in RAM.
    uint8_t* ramPointer(uint32_t address, uint32_t size) const;

private:
    struct Region
    {
        const char* name;
        uint32_t size;
        uint8_t* data;
    };
    Region regions_[4] = {
        {"RAM", kNaomiRamSize, nullptr},
        {"VRAM", kNaomiVramSize, nullptr},
        {"ARAM", kNaomiAramSize, nullptr},
        {"ELAN", kNaomiElanSize, nullptr},
    };
};

} // namespace idas3
