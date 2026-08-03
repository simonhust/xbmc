/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>

/*
 * DRM_IOCTL_MESON_OSD_HDR_LUT kernel ioctl wrapper.
 *
 * Configures the OSD HDR2 hardware pipeline (Matrix IN, EOTF, OETF, OOTF)
 * on a specific OSD module to convert sRGB content to the HDR transfer
 * function (PQ/HLG/CUVA). Used by the subtitle plane (OSD1_HDR) to
 * display sRGB subtitles correctly over HDR video.
 */
class CAMLHdrLut
{
public:
  CAMLHdrLut() = default;
  ~CAMLHdrLut() = default;

  enum ProcessSelect
  {
    PROC_BYPASS = 0,
    PROC_SDR_HDR = 1,
    PROC_SDR_HLG = 2,
    PROC_SDR_CUVA = 3,
  };

  enum ModuleSelect
  {
    MODULE_OSD1 = 4,  // OSD1_HDR
    MODULE_OSD2 = 5,  // OSD2_HDR
    MODULE_OSD3 = 10, // OSD3_HDR
  };

  /*
   * Configure the OSD HDR2 pipeline via the DRM ioctl.
   * drmFd: open DRM device file descriptor
   * module: OSD module to configure (OSD1_HDR for subtitle plane)
   * process: SDR->HDR transfer (SDR_HDR, SDR_HLG, or BYPASS)
   * Returns true on success.
   */
  bool Configure(int drmFd, ModuleSelect module, ProcessSelect process);
};