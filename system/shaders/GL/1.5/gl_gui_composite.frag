/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 150

in vec2 v_tex;
out vec4 fragColor;
uniform sampler2D u_samp;       // GUI FBO texture (sRGB BT.709)

void main()
{
  vec4 gui = texture(u_samp, v_tex);

  // Skip pixels the GUI never wrote to. Blend unit would still preserve video
  // bit-exactly via DST*(1-0)+garbage*0=DST, but discard makes it structural
  // and avoids the BO read and BO write for those pixels.
  if (gui.a == 0.0)
    discard;

  // The GUI is composited in sRGB BT.709. The kernel DV OSD path converts
  // from sRGB BT.709 to PQ BT.2020 via its core2 pipeline (EOTF, 709→2020
  // matrix, OETF). No userspace conversion is needed here.

#ifdef KODI_LIMITED_RANGE
  gui.rgb = gui.rgb * ((235.0 - 16.0) / 255.0) + (16.0 / 255.0);
#endif

  fragColor = gui;
}