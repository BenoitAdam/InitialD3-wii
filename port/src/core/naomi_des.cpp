// DES for NAOMI GD-ROM images.
//
// Tables and algorithm from MAME's naomigd.cpp
//   license: BSD-3-Clause, copyright-holders: Olivier Galibert
// as adapted in Flycast's core/hw/naomi/gdcartridge.cpp. Only the byte-order
// handling differs: blocks are loaded/stored as explicit little-endian u64 so
// the Wii (big-endian) decrypts exactly what an x86 host does.
#include "naomi_des.h"
#include "bytes.h"

namespace idas3 {

namespace {

static const uint32_t DES_LEFTSWAP[] = {
    0x00000000, 0x00000001, 0x00000100, 0x00000101, 0x00010000, 0x00010001, 0x00010100, 0x00010101,
    0x01000000, 0x01000001, 0x01000100, 0x01000101, 0x01010000, 0x01010001, 0x01010100, 0x01010101
};

static const uint32_t DES_RIGHTSWAP[] = {
    0x00000000, 0x01000000, 0x00010000, 0x01010000, 0x00000100, 0x01000100, 0x00010100, 0x01010100,
    0x00000001, 0x01000001, 0x00010001, 0x01010001, 0x00000101, 0x01000101, 0x00010101, 0x01010101,
};

static const uint32_t DES_SBOX1[] = {
    0x00808200, 0x00000000, 0x00008000, 0x00808202, 0x00808002, 0x00008202, 0x00000002, 0x00008000,
    0x00000200, 0x00808200, 0x00808202, 0x00000200, 0x00800202, 0x00808002, 0x00800000, 0x00000002,
    0x00000202, 0x00800200, 0x00800200, 0x00008200, 0x00008200, 0x00808000, 0x00808000, 0x00800202,
    0x00008002, 0x00800002, 0x00800002, 0x00008002, 0x00000000, 0x00000202, 0x00008202, 0x00800000,
    0x00008000, 0x00808202, 0x00000002, 0x00808000, 0x00808200, 0x00800000, 0x00800000, 0x00000200,
    0x00808002, 0x00008000, 0x00008200, 0x00800002, 0x00000200, 0x00000002, 0x00800202, 0x00008202,
    0x00808202, 0x00008002, 0x00808000, 0x00800202, 0x00800002, 0x00000202, 0x00008202, 0x00808200,
    0x00000202, 0x00800200, 0x00800200, 0x00000000, 0x00008002, 0x00008200, 0x00000000, 0x00808002
};

static const uint32_t DES_SBOX2[] = {
    0x40084010, 0x40004000, 0x00004000, 0x00084010, 0x00080000, 0x00000010, 0x40080010, 0x40004010,
    0x40000010, 0x40084010, 0x40084000, 0x40000000, 0x40004000, 0x00080000, 0x00000010, 0x40080010,
    0x00084000, 0x00080010, 0x40004010, 0x00000000, 0x40000000, 0x00004000, 0x00084010, 0x40080000,
    0x00080010, 0x40000010, 0x00000000, 0x00084000, 0x00004010, 0x40084000, 0x40080000, 0x00004010,
    0x00000000, 0x00084010, 0x40080010, 0x00080000, 0x40004010, 0x40080000, 0x40084000, 0x00004000,
    0x40080000, 0x40004000, 0x00000010, 0x40084010, 0x00084010, 0x00000010, 0x00004000, 0x40000000,
    0x00004010, 0x40084000, 0x00080000, 0x40000010, 0x00080010, 0x40004010, 0x40000010, 0x00080010,
    0x00084000, 0x00000000, 0x40004000, 0x00004010, 0x40000000, 0x40080010, 0x40084010, 0x00084000
};

static const uint32_t DES_SBOX3[] = {
    0x00000104, 0x04010100, 0x00000000, 0x04010004, 0x04000100, 0x00000000, 0x00010104, 0x04000100,
    0x00010004, 0x04000004, 0x04000004, 0x00010000, 0x04010104, 0x00010004, 0x04010000, 0x00000104,
    0x04000000, 0x00000004, 0x04010100, 0x00000100, 0x00010100, 0x04010000, 0x04010004, 0x00010104,
    0x04000104, 0x00010100, 0x00010000, 0x04000104, 0x00000004, 0x04010104, 0x00000100, 0x04000000,
    0x04010100, 0x04000000, 0x00010004, 0x00000104, 0x00010000, 0x04010100, 0x04000100, 0x00000000,
    0x00000100, 0x00010004, 0x04010104, 0x04000100, 0x04000004, 0x00000100, 0x00000000, 0x04010004,
    0x04000104, 0x00010000, 0x04000000, 0x04010104, 0x00000004, 0x00010104, 0x00010100, 0x04000004,
    0x04010000, 0x04000104, 0x00000104, 0x04010000, 0x00010104, 0x00000004, 0x04010004, 0x00010100
};

static const uint32_t DES_SBOX4[] = {
    0x80401000, 0x80001040, 0x80001040, 0x00000040, 0x00401040, 0x80400040, 0x80400000, 0x80001000,
    0x00000000, 0x00401000, 0x00401000, 0x80401040, 0x80000040, 0x00000000, 0x00400040, 0x80400000,
    0x80000000, 0x00001000, 0x00400000, 0x80401000, 0x00000040, 0x00400000, 0x80001000, 0x00001040,
    0x80400040, 0x80000000, 0x00001040, 0x00400040, 0x00001000, 0x00401040, 0x80401040, 0x80000040,
    0x00400040, 0x80400000, 0x00401000, 0x80401040, 0x80000040, 0x00000000, 0x00000000, 0x00401000,
    0x00001040, 0x00400040, 0x80400040, 0x80000000, 0x80401000, 0x80001040, 0x80001040, 0x00000040,
    0x80401040, 0x80000040, 0x80000000, 0x00001000, 0x80400000, 0x80001000, 0x00401040, 0x80400040,
    0x80001000, 0x00001040, 0x00400000, 0x80401000, 0x00000040, 0x00400000, 0x00001000, 0x00401040
};

static const uint32_t DES_SBOX5[] = {
    0x00000080, 0x01040080, 0x01040000, 0x21000080, 0x00040000, 0x00000080, 0x20000000, 0x01040000,
    0x20040080, 0x00040000, 0x01000080, 0x20040080, 0x21000080, 0x21040000, 0x00040080, 0x20000000,
    0x01000000, 0x20040000, 0x20040000, 0x00000000, 0x20000080, 0x21040080, 0x21040080, 0x01000080,
    0x21040000, 0x20000080, 0x00000000, 0x21000000, 0x01040080, 0x01000000, 0x21000000, 0x00040080,
    0x00040000, 0x21000080, 0x00000080, 0x01000000, 0x20000000, 0x01040000, 0x21000080, 0x20040080,
    0x01000080, 0x20000000, 0x21040000, 0x01040080, 0x20040080, 0x00000080, 0x01000000, 0x21040000,
    0x21040080, 0x00040080, 0x21000000, 0x21040080, 0x01040000, 0x00000000, 0x20040000, 0x21000000,
    0x00040080, 0x01000080, 0x20000080, 0x00040000, 0x00000000, 0x20040000, 0x01040080, 0x20000080
};

static const uint32_t DES_SBOX6[] = {
    0x10000008, 0x10200000, 0x00002000, 0x10202008, 0x10200000, 0x00000008, 0x10202008, 0x00200000,
    0x10002000, 0x00202008, 0x00200000, 0x10000008, 0x00200008, 0x10002000, 0x10000000, 0x00002008,
    0x00000000, 0x00200008, 0x10002008, 0x00002000, 0x00202000, 0x10002008, 0x00000008, 0x10200008,
    0x10200008, 0x00000000, 0x00202008, 0x10202000, 0x00002008, 0x00202000, 0x10202000, 0x10000000,
    0x10002000, 0x00000008, 0x10200008, 0x00202000, 0x10202008, 0x00200000, 0x00002008, 0x10000008,
    0x00200000, 0x10002000, 0x10000000, 0x00002008, 0x10000008, 0x10202008, 0x00202000, 0x10200000,
    0x00202008, 0x10202000, 0x00000000, 0x10200008, 0x00000008, 0x00002000, 0x10200000, 0x00202008,
    0x00002000, 0x00200008, 0x10002008, 0x00000000, 0x10202000, 0x10000000, 0x00200008, 0x10002008
};

static const uint32_t DES_SBOX7[] = {
    0x00100000, 0x02100001, 0x02000401, 0x00000000, 0x00000400, 0x02000401, 0x00100401, 0x02100400,
    0x02100401, 0x00100000, 0x00000000, 0x02000001, 0x00000001, 0x02000000, 0x02100001, 0x00000401,
    0x02000400, 0x00100401, 0x00100001, 0x02000400, 0x02000001, 0x02100000, 0x02100400, 0x00100001,
    0x02100000, 0x00000400, 0x00000401, 0x02100401, 0x00100400, 0x00000001, 0x02000000, 0x00100400,
    0x02000000, 0x00100400, 0x00100000, 0x02000401, 0x02000401, 0x02100001, 0x02100001, 0x00000001,
    0x00100001, 0x02000000, 0x02000400, 0x00100000, 0x02100400, 0x00000401, 0x00100401, 0x02100400,
    0x00000401, 0x02000001, 0x02100401, 0x02100000, 0x00100400, 0x00000000, 0x00000001, 0x02100401,
    0x00000000, 0x00100401, 0x02100000, 0x00000400, 0x02000001, 0x02000400, 0x00000400, 0x00100001
};

static const uint32_t DES_SBOX8[] = {
    0x08000820, 0x00000800, 0x00020000, 0x08020820, 0x08000000, 0x08000820, 0x00000020, 0x08000000,
    0x00020020, 0x08020000, 0x08020820, 0x00020800, 0x08020800, 0x00020820, 0x00000800, 0x00000020,
    0x08020000, 0x08000020, 0x08000800, 0x00000820, 0x00020800, 0x00020020, 0x08020020, 0x08020800,
    0x00000820, 0x00000000, 0x00000000, 0x08020020, 0x08000020, 0x08000800, 0x00020820, 0x00020000,
    0x00020820, 0x00020000, 0x08020800, 0x00000800, 0x00000020, 0x08020020, 0x00000800, 0x00020820,
    0x08000800, 0x00000020, 0x08000020, 0x08020000, 0x08020020, 0x08000000, 0x00020000, 0x08000820,
    0x00000000, 0x08020820, 0x00020020, 0x08000020, 0x08020000, 0x08000800, 0x08000820, 0x00000000,
    0x08020820, 0x00020800, 0x00020800, 0x00000820, 0x00000820, 0x00020020, 0x08000000, 0x08020800
};

static const uint32_t DES_MASK_TABLE[] = {
    0x24000000, 0x10000000, 0x08000000, 0x02080000, 0x01000000,
    0x00200000, 0x00100000, 0x00040000, 0x00020000, 0x00010000,
    0x00002000, 0x00001000, 0x00000800, 0x00000400, 0x00000200,
    0x00000100, 0x00000020, 0x00000010, 0x00000008, 0x00000004,
    0x00000002, 0x00000001, 0x20000000, 0x10000000, 0x08000000,
    0x04000000, 0x02000000, 0x01000000, 0x00200000, 0x00100000,
    0x00080000, 0x00040000, 0x00020000, 0x00010000, 0x00002000,
    0x00001000, 0x00000808, 0x00000400, 0x00000200, 0x00000100,
    0x00000020, 0x00000011, 0x00000004, 0x00000002
};

static const uint8_t DES_ROTATE_TABLE[16] = {
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

inline void permutate(uint32_t& a, uint32_t& b, uint32_t m, int shift)
{
    uint32_t temp = ((a >> shift) ^ b) & m;
    a ^= temp << shift;
    b ^= temp;
}

uint64_t rev64(uint64_t src)
{
    uint64_t ret = 0;
    for (int i = 0; i < 8; i++)
        ret |= ((src >> (i * 8)) & 0xff) << (56 - i * 8);
    return ret;
}

void generateSubkeys(const uint64_t key, uint32_t* subkeys)
{
    uint32_t l = (uint32_t)(key >> 32);
    uint32_t r = (uint32_t)key;

    permutate(r, l, 0x0f0f0f0f, 4);
    permutate(r, l, 0x10101010, 0);

    l = (DES_LEFTSWAP[(l >> 0) & 0xf] << 3) |
        (DES_LEFTSWAP[(l >> 8) & 0xf] << 2) |
        (DES_LEFTSWAP[(l >> 16) & 0xf] << 1) |
        (DES_LEFTSWAP[(l >> 24) & 0xf] << 0) |
        (DES_LEFTSWAP[(l >> 5) & 0xf] << 7) |
        (DES_LEFTSWAP[(l >> 13) & 0xf] << 6) |
        (DES_LEFTSWAP[(l >> 21) & 0xf] << 5) |
        (DES_LEFTSWAP[(l >> 29) & 0xf] << 4);

    r = (DES_RIGHTSWAP[(r >> 1) & 0xf] << 3) |
        (DES_RIGHTSWAP[(r >> 9) & 0xf] << 2) |
        (DES_RIGHTSWAP[(r >> 17) & 0xf] << 1) |
        (DES_RIGHTSWAP[(r >> 25) & 0xf] << 0) |
        (DES_RIGHTSWAP[(r >> 4) & 0xf] << 7) |
        (DES_RIGHTSWAP[(r >> 12) & 0xf] << 6) |
        (DES_RIGHTSWAP[(r >> 20) & 0xf] << 5) |
        (DES_RIGHTSWAP[(r >> 28) & 0xf] << 4);

    l &= 0x0fffffff;
    r &= 0x0fffffff;

    for (int round = 0; round < 16; round++)
    {
        l = ((l << DES_ROTATE_TABLE[round]) | (l >> (28 - DES_ROTATE_TABLE[round]))) & 0x0fffffff;
        r = ((r << DES_ROTATE_TABLE[round]) | (r >> (28 - DES_ROTATE_TABLE[round]))) & 0x0fffffff;

        subkeys[round * 2] =
            ((l << 4) & DES_MASK_TABLE[0]) |
            ((l << 28) & DES_MASK_TABLE[1]) |
            ((l << 14) & DES_MASK_TABLE[2]) |
            ((l << 18) & DES_MASK_TABLE[3]) |
            ((l << 6) & DES_MASK_TABLE[4]) |
            ((l << 9) & DES_MASK_TABLE[5]) |
            ((l >> 1) & DES_MASK_TABLE[6]) |
            ((l << 10) & DES_MASK_TABLE[7]) |
            ((l << 2) & DES_MASK_TABLE[8]) |
            ((l >> 10) & DES_MASK_TABLE[9]) |
            ((r >> 13) & DES_MASK_TABLE[10]) |
            ((r >> 4) & DES_MASK_TABLE[11]) |
            ((r << 6) & DES_MASK_TABLE[12]) |
            ((r >> 1) & DES_MASK_TABLE[13]) |
            ((r >> 14) & DES_MASK_TABLE[14]) |
            ((r >> 0) & DES_MASK_TABLE[15]) |
            ((r >> 5) & DES_MASK_TABLE[16]) |
            ((r >> 10) & DES_MASK_TABLE[17]) |
            ((r >> 3) & DES_MASK_TABLE[18]) |
            ((r >> 18) & DES_MASK_TABLE[19]) |
            ((r >> 26) & DES_MASK_TABLE[20]) |
            ((r >> 24) & DES_MASK_TABLE[21]);

        subkeys[round * 2 + 1] =
            ((l << 15) & DES_MASK_TABLE[22]) |
            ((l << 17) & DES_MASK_TABLE[23]) |
            ((l << 10) & DES_MASK_TABLE[24]) |
            ((l << 22) & DES_MASK_TABLE[25]) |
            ((l >> 2) & DES_MASK_TABLE[26]) |
            ((l << 1) & DES_MASK_TABLE[27]) |
            ((l << 16) & DES_MASK_TABLE[28]) |
            ((l << 11) & DES_MASK_TABLE[29]) |
            ((l << 3) & DES_MASK_TABLE[30]) |
            ((l >> 6) & DES_MASK_TABLE[31]) |
            ((l << 15) & DES_MASK_TABLE[32]) |
            ((l >> 4) & DES_MASK_TABLE[33]) |
            ((r >> 2) & DES_MASK_TABLE[34]) |
            ((r << 8) & DES_MASK_TABLE[35]) |
            ((r >> 14) & DES_MASK_TABLE[36]) |
            ((r >> 9) & DES_MASK_TABLE[37]) |
            ((r >> 0) & DES_MASK_TABLE[38]) |
            ((r << 7) & DES_MASK_TABLE[39]) |
            ((r >> 7) & DES_MASK_TABLE[40]) |
            ((r >> 3) & DES_MASK_TABLE[41]) |
            ((r << 2) & DES_MASK_TABLE[42]) |
            ((r >> 21) & DES_MASK_TABLE[43]);
    }
}

template <bool decrypt>
uint64_t desCrypt(uint64_t src, const uint32_t* subkeysTable)
{
    uint32_t r = (uint32_t)(src & 0x00000000ffffffffULL);
    uint32_t l = (uint32_t)(src >> 32);

    permutate(l, r, 0x0f0f0f0f, 4);
    permutate(l, r, 0x0000ffff, 16);
    permutate(r, l, 0x33333333, 2);
    permutate(r, l, 0x00ff00ff, 8);
    permutate(l, r, 0x55555555, 1);

    int subkey = decrypt ? 30 : 0;

    for (int i = 0; i < 32; i += 4)
    {
        uint32_t temp;

        temp = ((r << 1) | (r >> 31)) ^ subkeysTable[subkey];
        l ^= DES_SBOX8[(temp >> 0) & 0x3f];
        l ^= DES_SBOX6[(temp >> 8) & 0x3f];
        l ^= DES_SBOX4[(temp >> 16) & 0x3f];
        l ^= DES_SBOX2[(temp >> 24) & 0x3f];
        subkey++;

        temp = ((r >> 3) | (r << 29)) ^ subkeysTable[subkey];
        l ^= DES_SBOX7[(temp >> 0) & 0x3f];
        l ^= DES_SBOX5[(temp >> 8) & 0x3f];
        l ^= DES_SBOX3[(temp >> 16) & 0x3f];
        l ^= DES_SBOX1[(temp >> 24) & 0x3f];
        subkey++;
        if (decrypt)
            subkey -= 4;

        temp = ((l << 1) | (l >> 31)) ^ subkeysTable[subkey];
        r ^= DES_SBOX8[(temp >> 0) & 0x3f];
        r ^= DES_SBOX6[(temp >> 8) & 0x3f];
        r ^= DES_SBOX4[(temp >> 16) & 0x3f];
        r ^= DES_SBOX2[(temp >> 24) & 0x3f];
        subkey++;

        temp = ((l >> 3) | (l << 29)) ^ subkeysTable[subkey];
        r ^= DES_SBOX7[(temp >> 0) & 0x3f];
        r ^= DES_SBOX5[(temp >> 8) & 0x3f];
        r ^= DES_SBOX3[(temp >> 16) & 0x3f];
        r ^= DES_SBOX1[(temp >> 24) & 0x3f];
        subkey++;
        if (decrypt)
            subkey -= 4;
    }

    permutate(r, l, 0x55555555, 1);
    permutate(l, r, 0x00ff00ff, 8);
    permutate(l, r, 0x33333333, 2);
    permutate(r, l, 0x0000ffff, 16);
    permutate(r, l, 0x0f0f0f0f, 4);

    return ((uint64_t)r << 32) | (uint64_t)l;
}

} // namespace

void NaomiDes::setKey(uint64_t picKey)
{
    // The DIMM board feeds the PIC key to DES byte-reversed (MAME: rev64).
    generateSubkeys(rev64(picKey), subkeys_);
}

uint64_t NaomiDes::decryptBlock(uint64_t block) const
{
    return desCrypt<true>(block, subkeys_);
}

uint64_t NaomiDes::encryptBlock(uint64_t block) const
{
    return desCrypt<false>(block, subkeys_);
}

void NaomiDes::decryptBuffer(uint8_t* data, size_t bytes) const
{
    for (size_t i = 0; i + 8 <= bytes; i += 8)
        wr_le64(data + i, desCrypt<true>(rd_le64(data + i), subkeys_));
}

} // namespace idas3
