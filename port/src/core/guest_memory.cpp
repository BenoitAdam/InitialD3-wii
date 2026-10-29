#include "guest_memory.h"
#include "log.h"
#include "../platform/platform.h"

#include <cstdio>
#include <cstring>

namespace idas3 {

const char* memoryPlanLabel(MemoryPlan plan)
{
    switch (plan)
    {
    case MemoryPlan::Full: return "FULL NAOMI 2 (88 MB)";
    case MemoryPlan::NoElan: return "NO ELAN RAM (56 MB)";
    case MemoryPlan::RamOnly: return "SH-4 RAM ONLY (32 MB)";
    }
    return "?";
}

static int regionCount(MemoryPlan plan)
{
    switch (plan)
    {
    case MemoryPlan::Full: return 4;
    case MemoryPlan::NoElan: return 3;
    case MemoryPlan::RamOnly: return 1;
    }
    return 1;
}

uint32_t memoryPlanBytes(MemoryPlan plan)
{
    const uint32_t sizes[4] = {kNaomiRamSize, kNaomiVramSize, kNaomiAramSize, kNaomiElanSize};
    uint32_t total = 0;
    for (int i = 0; i < regionCount(plan); i++)
        total += sizes[i];
    return total;
}

GuestMemory::~GuestMemory()
{
    release();
}

bool GuestMemory::allocate(MemoryPlan plan, std::string& error)
{
    release();
    const int count = regionCount(plan);
    for (int i = 0; i < count; i++)
    {
        Region& r = regions_[i];
        const size_t freeBefore = platform::largeFreeBytes();
        r.data = (uint8_t*)platform::allocLarge(r.size);
        if (!r.data)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s needs %u KB but only %u KB is free (%s)", r.name,
                     r.size / 1024, (unsigned)(freeBefore / 1024), platform::memoryReport().c_str());
            error = buf;
            log::write("[mem] allocation failed: %s\n", buf);
            release();
            return false;
        }
        memset(r.data, 0, r.size);
        log::write("[mem] %s: %u KB at %p\n", r.name, r.size / 1024, (void*)r.data);
    }
    log::write("[mem] after allocation: %s\n", platform::memoryReport().c_str());
    return true;
}

void GuestMemory::release()
{
    // Reverse order: the Wii allocator is a stack carved from MEM2.
    for (int i = 3; i >= 0; i--)
    {
        if (regions_[i].data)
        {
            platform::freeLarge(regions_[i].data, regions_[i].size);
            regions_[i].data = nullptr;
        }
    }
}

uint32_t GuestMemory::allocatedBytes() const
{
    uint32_t total = 0;
    for (const Region& r : regions_)
        if (r.data)
            total += r.size;
    return total;
}

uint8_t* GuestMemory::ramPointer(uint32_t address, uint32_t size) const
{
    if (!ram())
        return nullptr;
    // Strip the P1/P2/P3 segment bits, then require area 3 (0x0C000000-0x0FFFFFFF),
    // where the 32 MB of RAM is mirrored.
    const uint32_t phys = address & 0x1fffffff;
    if ((phys & 0x1c000000) != 0x0c000000)
        return nullptr;
    const uint32_t offset = phys & (kNaomiRamSize - 1);
    if (size > kNaomiRamSize - offset)
        return nullptr;
    return ram() + offset;
}

} // namespace idas3
