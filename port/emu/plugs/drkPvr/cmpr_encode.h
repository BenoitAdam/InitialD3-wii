// GX CMPR (DXT1) block encoders, shared by gxRend.cpp (Wii) and the Windows
// runner's texture census (host/texcache_census.cpp), which checks them on the
// game's own VQ textures against cmpr_decode_block below.
//
// A GX CMPR block is 8 bytes for 4x4 texels:
//   [0-1] colour 0 RGB565 big-endian, [2-3] colour 1,
//   [4-7] 16 x 2-bit selectors, row-major, pixel 0 in bits 31-30.
// colour 0 > colour 1 (as 16-bit words): four colours, c0, c1, then
// (5 c0 + 3 c1) / 8 and (3 c0 + 5 c1) / 8 (the GameCube/Wii blend, 3/8 rather
// than DXT1's 1/3: Dolphin's decoder). colour 0 <= colour 1: c0, c1, their mean
// and TRANSPARENT black: a quarter of an opaque block's texels can vanish.
#pragma once

#include "types.h"

// Exact for x < 768 (171 == floor(512/3) + 1); callers pass x <= 189.
static inline u32 cmpr_div3_u8(u32 x) { return (x * 171) >> 9; }

// The encoder used until 2026-11-11 (VQ AS CMPR = ON (OLD), and the FMV
// paths): endpoints are the darkest and brightest texels by luma. Two faults:
// the mode GX reads is the unsigned order of the two packed words, which luma
// does not follow (bright green 0x07E0 packs below dark red 0x8000), so such a
// block lands in the 3-colour + transparent mode; and a block whose texels
// differ in hue more than in brightness gets two endpoints of one hue.
static void cmpr_encode_block_luma(const u16 *src, u8 *dst)
{
  u32 best_min = 0xFFFFFFFF, best_max = 0;
  int idx_min = 0, idx_max = 0;
  for (int i = 0; i < 16; i++)
  {
    u16 p  = src[i];
    u32 r  = (p >> 11) & 0x1F;
    u32 g  = (p >>  5) & 0x3F;
    u32 b  = (p      ) & 0x1F;
    u32 lm = r * 2 + g + b * 2;
    if (lm < best_min) { best_min = lm; idx_min = i; }
    if (lm > best_max) { best_max = lm; idx_max = i; }
  }

  u16 c0 = src[idx_max]; // brightest  -> color0 (must be >= color1)
  u16 c1 = src[idx_min]; // darkest    -> color1

  // Solid-color block: nudge c0 up so GX stays in 4-color mode (c0 > c1).
  if (c0 == c1)
  {
    u32 r = (c0 >> 11) & 0x1F;
    u32 g = (c0 >>  5) & 0x3F;
    u32 b = (c0      ) & 0x1F;
    if (r < 0x1F) r++; else if (g < 0x3F) g++; else if (b < 0x1F) b++;
    c0 = (u16)((r << 11) | (g << 5) | b);
  }

  u32 r0 = (c0 >> 11) & 0x1F, g0 = (c0 >> 5) & 0x3F, b0 = c0 & 0x1F;
  u32 r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
  u32 r2 = cmpr_div3_u8(2*r0+r1), g2 = cmpr_div3_u8(2*g0+g1), b2 = cmpr_div3_u8(2*b0+b1);
  u32 r3 = cmpr_div3_u8(r0+2*r1), g3 = cmpr_div3_u8(g0+2*g1), b3 = cmpr_div3_u8(b0+2*b1);

  u32 selectors = 0;
  for (int i = 0; i < 16; i++)
  {
    u16 p   = src[i];
    s32 pr6 = ((p >> 11) & 0x1F) << 1; // scale R/B to 6-bit for fair distance
    s32 pg  = (p >>  5) & 0x3F;
    s32 pb6 = ((p      ) & 0x1F) << 1;

    s32 dr0=pr6-(s32)(r0<<1), dg0=pg-(s32)g0, db0=pb6-(s32)(b0<<1);
    s32 dr1=pr6-(s32)(r1<<1), dg1=pg-(s32)g1, db1=pb6-(s32)(b1<<1);
    s32 dr2=pr6-(s32)(r2<<1), dg2=pg-(s32)g2, db2=pb6-(s32)(b2<<1);
    s32 dr3=pr6-(s32)(r3<<1), dg3=pg-(s32)g3, db3=pb6-(s32)(b3<<1);

    u32 d0=(u32)(dr0*dr0+dg0*dg0+db0*db0);
    u32 d1=(u32)(dr1*dr1+dg1*dg1+db1*db1);
    u32 d2=(u32)(dr2*dr2+dg2*dg2+db2*db2);
    u32 d3=(u32)(dr3*dr3+dg3*dg3+db3*db3);

    u32 best_d=d0, best_idx=0;
    if (d1<best_d){best_d=d1;best_idx=1;}
    if (d2<best_d){best_d=d2;best_idx=2;}
    if (d3<best_d){best_d=d3;best_idx=3;}

    selectors |= (best_idx << (30 - i * 2));
  }

  dst[0]=(u8)(c0>>8); dst[1]=(u8)(c0&0xFF);
  dst[2]=(u8)(c1>>8); dst[3]=(u8)(c1&0xFF);
  dst[4]=(u8)(selectors>>24); dst[5]=(u8)(selectors>>16);
  dst[6]=(u8)(selectors>> 8); dst[7]=(u8)(selectors    );
}

// RGB565 -> 8 bits a channel, by bit replication as GX expands it.
static inline void cmpr_expand565(u16 p, s32 *rgb)
{
  const u32 r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
  rgb[0] = (s32)((r << 3) | (r >> 2));
  rgb[1] = (s32)((g << 2) | (g >> 4));
  rgb[2] = (s32)((b << 3) | (b >> 2));
}

// The encoder since 2026-11-11 (VQ AS CMPR = ON). Endpoints are the two texels
// farthest apart along the block's principal colour axis (power iteration on
// the colour covariance), so a block spanning two hues keeps both. Texels are
// real RGB565 values (VQ 565 source), so the endpoints are exact. The words
// are then put in the 4-colour order (c0 > c1; the palette is symmetric, so
// swapping them only swaps the selectors), and each texel takes the palette
// entry nearest along the endpoints' line, at GX's 3/8 and 5/8 points. A block
// of one colour is exact: c0 = that colour with c1 = c0 - 1 and every selector
// 0, or for black c0 = 1 and every selector 1.
static void cmpr_encode_block_pca(const u16 *src, u8 *dst)
{
  s32 px[16][3];
  s32 sum[3] = {0, 0, 0};
  bool solid = true;
  for (int i = 0; i < 16; i++)
  {
    cmpr_expand565(src[i], px[i]);
    sum[0] += px[i][0]; sum[1] += px[i][1]; sum[2] += px[i][2];
    solid &= src[i] == src[0];
  }

  u16 c0, c1;
  u32 selectors;
  if (solid)
  {
    c0 = src[0];
    if (c0)
    {
      c1 = (u16)(c0 - 1);
      selectors = 0;            // every texel colour 0
    }
    else
    {
      c0 = 1;
      c1 = 0;
      selectors = 0x55555555u;  // every texel colour 1 (black)
    }
  }
  else
  {
    // Covariance x 256 (16 * sum(xy) - sum(x) sum(y)): exact in 32 bits.
    s32 sxx = 0, sxy = 0, sxz = 0, syy = 0, syz = 0, szz = 0;
    for (int i = 0; i < 16; i++)
    {
      const s32 x = px[i][0], y = px[i][1], z = px[i][2];
      sxx += x * x; sxy += x * y; sxz += x * z;
      syy += y * y; syz += y * z; szz += z * z;
    }
    const float cxx = (float)(16 * sxx - sum[0] * sum[0]);
    const float cxy = (float)(16 * sxy - sum[0] * sum[1]);
    const float cxz = (float)(16 * sxz - sum[0] * sum[2]);
    const float cyy = (float)(16 * syy - sum[1] * sum[1]);
    const float cyz = (float)(16 * syz - sum[1] * sum[2]);
    const float czz = (float)(16 * szz - sum[2] * sum[2]);
    // Start from the covariance column of the widest channel: never
    // orthogonal to the principal axis unless that channel does not vary.
    float vx, vy, vz;
    if (cxx >= cyy && cxx >= czz)      { vx = cxx; vy = cxy; vz = cxz; }
    else if (cyy >= czz)               { vx = cxy; vy = cyy; vz = cyz; }
    else                               { vx = cxz; vy = cyz; vz = czz; }
    for (int it = 0; it < 4; it++)
    {
      const float nx = cxx * vx + cxy * vy + cxz * vz;
      const float ny = cxy * vx + cyy * vy + cyz * vz;
      const float nz = cxz * vx + cyz * vy + czz * vz;
      float m = nx < 0 ? -nx : nx;
      const float ay = ny < 0 ? -ny : ny, az = nz < 0 ? -nz : nz;
      if (ay > m) m = ay;
      if (az > m) m = az;
      if (m == 0.0f)
        break;                  // texels differ but are colinear through 0
      const float inv = 1.0f / m;
      vx = nx * inv; vy = ny * inv; vz = nz * inv;
    }
    if (vx == 0.0f && vy == 0.0f && vz == 0.0f)
      vx = vy = vz = 1.0f;
    int imin = 0, imax = 0;
    float dmin = 0.0f, dmax = 0.0f;
    for (int i = 0; i < 16; i++)
    {
      const float d = vx * px[i][0] + vy * px[i][1] + vz * px[i][2];
      if (i == 0 || d < dmin) { dmin = d; imin = i; }
      if (i == 0 || d > dmax) { dmax = d; imax = i; }
    }
    c0 = src[imax];
    c1 = src[imin];
    if (c0 == c1)
    {
      // Every texel projects alike: take the two most different words.
      for (int i = 0; i < 16; i++)
        if (src[i] != c0) { c1 = src[i]; break; }
    }
    if (c0 < c1) { const u16 t = c0; c0 = c1; c1 = t; }
    s32 e0[3], e1[3];
    cmpr_expand565(c0, e0);
    cmpr_expand565(c1, e1);
    const s32 dx = e0[0] - e1[0], dy = e0[1] - e1[1], dz = e0[2] - e1[2];
    const s32 dd = dx * dx + dy * dy + dz * dz;   // > 0: c0 != c1
    selectors = 0;
    for (int i = 0; i < 16; i++)
    {
      // t / dd in [0, 1] from colour 1 to colour 0; palette points at 0, 3/8,
      // 5/8, 1, so the boundaries are 3/16, 1/2, 13/16.
      const s32 t = 16 * ((px[i][0] - e1[0]) * dx + (px[i][1] - e1[1]) * dy + (px[i][2] - e1[2]) * dz);
      u32 sel;
      if (t < 3 * dd)       sel = 1;   // colour 1
      else if (t < 8 * dd)  sel = 3;   // (3 c0 + 5 c1) / 8
      else if (t < 13 * dd) sel = 2;   // (5 c0 + 3 c1) / 8
      else                  sel = 0;   // colour 0
      selectors |= sel << (30 - i * 2);
    }
  }

  dst[0] = (u8)(c0 >> 8); dst[1] = (u8)c0;
  dst[2] = (u8)(c1 >> 8); dst[3] = (u8)c1;
  dst[4] = (u8)(selectors >> 24); dst[5] = (u8)(selectors >> 16);
  dst[6] = (u8)(selectors >> 8);  dst[7] = (u8)selectors;
}

// What GX shows for one block (Dolphin's decoder): 16 ARGB8888 texels,
// row-major. For the Windows checks only.
static inline void cmpr_decode_block(const u8 *src, u32 *out)
{
  const u16 c0 = (u16)(src[0] << 8 | src[1]), c1 = (u16)(src[2] << 8 | src[3]);
  s32 a[3], b[3];
  cmpr_expand565(c0, a);
  cmpr_expand565(c1, b);
  u32 pal[4];
  pal[0] = 0xFF000000u | (u32)a[0] << 16 | (u32)a[1] << 8 | (u32)a[2];
  pal[1] = 0xFF000000u | (u32)b[0] << 16 | (u32)b[1] << 8 | (u32)b[2];
  if (c0 > c1)
  {
    pal[2] = 0xFF000000u | (u32)((a[0] * 5 + b[0] * 3) >> 3) << 16 | (u32)((a[1] * 5 + b[1] * 3) >> 3) << 8 |
             (u32)((a[2] * 5 + b[2] * 3) >> 3);
    pal[3] = 0xFF000000u | (u32)((a[0] * 3 + b[0] * 5) >> 3) << 16 | (u32)((a[1] * 3 + b[1] * 5) >> 3) << 8 |
             (u32)((a[2] * 3 + b[2] * 5) >> 3);
  }
  else
  {
    pal[2] = 0xFF000000u | (u32)((a[0] + b[0]) / 2) << 16 | (u32)((a[1] + b[1]) / 2) << 8 | (u32)((a[2] + b[2]) / 2);
    pal[3] = 0;   // transparent black
  }
  const u32 sel = (u32)src[4] << 24 | (u32)src[5] << 16 | (u32)src[6] << 8 | src[7];
  for (int i = 0; i < 16; i++)
    out[i] = pal[(sel >> (30 - i * 2)) & 3];
}
