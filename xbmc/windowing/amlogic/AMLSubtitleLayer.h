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

#include "windowing/Resolution.h"

class CAMLDisplay;
class CAMLGBMUtils;

/*
 * Dedicated subtitle plane on the Amlogic display pipeline.
 *
 * The GUI/OSD renders into the primary GBM surface which is flipped to the
 * primary DRM plane. Subtitles are rendered into a second GBM/EGL surface
 * that is flipped to a separate overlay plane, so they can be tone-mapped
 * independently of the GUI (hardware OSD HDR2 block).
 *
 * Usage per frame (called from the GUI render thread, around overlay render):
 *   layer.BeginRender();   // make subtitle EGL surface current, clear
 *   ... render overlays ...
 *   layer.EndRender();     // swap subtitle surface, lock front buffer
 *
 * The resulting framebuffer id is handed to CAMLDisplay::FlipPage() so both
 * planes are committed atomically.
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
  // Returns false (and leaves the main surface current) if the layer is
  // not usable so callers can fall back to rendering into the GUI.
  bool BeginRender();
  // Swap the subtitle surface, lock its front buffer into a DRM fb and
  // restore the main surface as current.
  void EndRender();

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
