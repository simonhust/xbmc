/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AMLHdrLut.h"

#include "utils/log.h"

#include <drm/drm.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* DRM_IOCTL_MESON_OSD_HDR_LUT: DRM_COMMAND_BASE(0x40) + 0x30 */
#define MESON_DRM_IOCTL_OSD_HDR_LUT \
  DRM_IOWR(0x40 + 0x30, struct drm_meson_osd_hdr_lut)

struct drm_meson_osd_hdr_lut
{
  uint32_t module_sel;
  uint32_t process_select;
  uint32_t reserved[2];
};

bool CAMLHdrLut::Configure(int drmFd, ModuleSelect module, ProcessSelect process)
{
  if (drmFd < 0)
  {
    CLog::Log(LOGERROR, "CAMLHdrLut::{} - invalid drm fd", __FUNCTION__);
    return false;
  }

  struct drm_meson_osd_hdr_lut lut = {};
  lut.module_sel = static_cast<uint32_t>(module);
  lut.process_select = static_cast<uint32_t>(process);

  if (ioctl(drmFd, MESON_DRM_IOCTL_OSD_HDR_LUT, &lut) != 0)
  {
    CLog::Log(LOGERROR, "CAMLHdrLut::{} - ioctl failed: {}", __FUNCTION__, strerror(errno));
    return false;
  }

  CLog::Log(LOGDEBUG, "CAMLHdrLut::{} - module={} process={}", __FUNCTION__,
            lut.module_sel, lut.process_select);
  return true;
}