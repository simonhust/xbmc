/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <vector>

class CDVDOverlayImage;
class CDVDOverlaySpu;
class CDVDOverlaySSA;
typedef struct ass_image ASS_Image;

namespace OVERLAY
{

struct SQuad
{
  int u, v;
  unsigned char r, g, b, a;
  int x, y;
  int w, h;
};

struct SQuads
{
  int size_x{0};
  int size_y{0};
  std::vector<uint8_t> texture;
  std::vector<SQuad> quad;
};

void convert_rgba(const CDVDOverlayImage& o, bool mergealpha, std::vector<uint32_t>& rgba);
void convert_rgba(const CDVDOverlayImage& o,
                  const std::vector<uint32_t>& palette,
                  bool mergealpha,
                  std::vector<uint32_t>& rgba);
void convert_rgba(const CDVDOverlaySpu& o,
                  bool mergealpha,
                  int& min_x,
                  int& max_x,
                  int& min_y,
                  int& max_y,
                  std::vector<uint32_t>& rgba);
bool convert_quad(ASS_Image* images, SQuads& quads, int max_x);
int GetStereoscopicDepth(bool isPgs, int subtitleDepth);

// Pre-bakes an HDR-authored PGS palette (PQ-encoded BT.2020 RGB as produced by
// the FFmpeg PGS decoder with its hardcoded BT.709 matrix) into sRGB-encoded
// RGB. target2020 keeps the BT.2020 primaries for the kernel OSD HDR2 path
// (gamut conversion bypassed via osd_gamut_bypass); otherwise the palette is
// baked to BT.709 sRGB so an SDR output shows it directly. Runs once per
// subtitle texture at creation time (256 entries, no per-pixel cost).
// Returns a palette with RGB replaced, alpha preserved.
std::vector<uint32_t> prebake_hdr_pgs_palette(const std::vector<uint32_t>& palette,
                                              float peakScale,
                                              float saturation,
                                              bool target2020);

} // namespace OVERLAY
