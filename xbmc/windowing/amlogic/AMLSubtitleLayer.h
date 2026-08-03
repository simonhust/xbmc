/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <EGL/egl.h>

#include "AMLHdrLut.h"
#include "windowing/Resolution.h"

class CAMLDisplay;
class CAMLGBMUtils;

/*
 * Dedicated subtitle plane on the Amlogic display pipeline.
 *
 * The subtitle plane (osd0, primary) carries subtitle content with hardware
 * HDR2 conversion (sRGB->PQ/HLG via OSD1_HDR2 LUT), while the GUI renders
 * in SDR on the overlay plane (osd1).
 *
 * Usage per frame (called from the GUI render thread, around overlay render):
 *   layer.BeginRender();   // make subtitle EGL surface current, clear
 *   ... render overlays ...
 *   layer.EndRender();     // swap subtitle surface, lock front buffer
 *
 * On HDR mode changes, call UpdateHdrState() to configure the hardware LUT.
 */
class CAMLSubtitleLayer
{
public:
  CAMLSubtitleLayer(CAMLDisplay* display, CAMLGBMUtils* gbmUtils);
  ~CAMLSubtitleLayer();

  bool Init(int width, int height, EGLDisplay eglDisplay, EGLConfig eglConfig,
            EGLContext eglContext);
  void Cleanup();

  // Make the subtitle EGL surface current and clear it for a new frame.
  bool BeginRender();
  void EndRender();

  // Configure the hardware OSD HDR2 pipeline for subtitle color mapping.
  // hdrType: HDR_TYPE_HDR10/PQ → SDR_HDR, HDR_TYPE_HLG → SDR_HLG,
  //          HDR_TYPE_NONE (or other) → BYPASS (disable).
  // drmFd: DRM device file descriptor for the ioctl.
  void UpdateHdrState(uint32_t hdrType, int drmFd);

  bool IsActive() const { return m_active; }
  uint32_t GetFbId() const { return m_fbId; }
  bool HasFb() const { return m_fbId != 0; }
  int GetWidth() const { return m_width; }
  int GetHeight() const { return m_height; }

private:
  bool CreateEglSurface();
  void DestroyEglSurface();

  CAMLDisplay* m_display{nullptr};
  CAMLGBMUtils* m_gbmUtils{nullptr};
  CAMLHdrLut m_hdrLut;

  EGLDisplay m_eglDisplay{EGL_NO_DISPLAY};
  EGLConfig m_eglConfig{};
  EGLContext m_eglContext{EGL_NO_CONTEXT};
  EGLSurface m_eglSurface{EGL_NO_SURFACE};

  int m_width{0};
  int m_height{0};
  uint32_t m_fbId{0};
  bool m_active{false};
  EGLSurface m_prevSurface{EGL_NO_SURFACE};
};
