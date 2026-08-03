/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AMLSubtitleLayer.h"

#include "AMLDisplay.h"
#include "utils/EGLUtils.h"
#include "utils/log.h"
#include "utils/StreamDetails.h"

#include <drm_fourcc.h>

#include <GLES2/gl2.h>

CAMLSubtitleLayer::CAMLSubtitleLayer(CAMLDisplay* display, CAMLGBMUtils* gbmUtils)
  : m_display(display), m_gbmUtils(gbmUtils)
{
}

CAMLSubtitleLayer::~CAMLSubtitleLayer()
{
  Cleanup();
}

bool CAMLSubtitleLayer::Init(int width, int height, EGLDisplay eglDisplay, EGLConfig eglConfig,
                             EGLContext eglContext)
{
  Cleanup();

  if (!m_display || !m_gbmUtils)
  {
    CLog::Log(LOGERROR, "CAMLSubtitleLayer::{} - missing display/gbm utils", __FUNCTION__);
    return false;
  }

  if (!m_display->HasOverlayPlane())
  {
    CLog::Log(LOGDEBUG, "CAMLSubtitleLayer::{} - no overlay plane for GUI, subtitle layer disabled", __FUNCTION__);
    return false;
  }

  m_eglDisplay = eglDisplay;
  m_eglConfig = eglConfig;
  m_eglContext = eglContext;

  uint32_t format = DRM_FORMAT_ARGB8888;
  if (!m_gbmUtils->CreateSubtitleSurface(width, height, format))
  {
    CLog::Log(LOGERROR, "CAMLSubtitleLayer::{} - failed to create subtitle GBM surface",
              __FUNCTION__);
    return false;
  }

  if (!CreateEglSurface())
  {
    CLog::Log(LOGERROR, "CAMLSubtitleLayer::{} - failed to create subtitle EGL surface",
              __FUNCTION__);
    return false;
  }

  m_width = width;
  m_height = height;
  m_active = true;

  CLog::Log(LOGDEBUG, "CAMLSubtitleLayer::{} - subtitle layer {}x{} active", __FUNCTION__, width,
            height);
  return true;
}

void CAMLSubtitleLayer::Cleanup()
{
  m_active = false;
  m_fbId = 0;
  DestroyEglSurface();
  if (m_gbmUtils)
    m_gbmUtils->ReleaseSubtitleSurface();
  m_eglDisplay = EGL_NO_DISPLAY;
  m_eglConfig = {};
  m_eglContext = EGL_NO_CONTEXT;
  m_width = 0;
  m_height = 0;
}

bool CAMLSubtitleLayer::CreateEglSurface()
{
  if (m_eglDisplay == EGL_NO_DISPLAY || !m_gbmUtils->GetSubtitleSurface())
    return false;

  // createEGLSurface: build a window surface from the subtitle gbm_surface.
  // Use the platform surface path so the EGL driver picks the same config the
  // main surface uses (EGL_EXT_platform_base is available since the main
  // surface is created the same way).
#if defined(EGL_EXT_platform_base)
  if (CEGLUtils::HasClientExtension("EGL_EXT_platform_base"))
  {
    auto createPlatformWindowSurfaceEXT = CEGLUtils::GetRequiredProcAddress<
        PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC>("eglCreatePlatformWindowSurfaceEXT");
    m_eglSurface = createPlatformWindowSurfaceEXT(m_eglDisplay, m_eglConfig,
                                                  m_gbmUtils->GetSubtitleSurface(), nullptr);
  }
#endif

  if (m_eglSurface == EGL_NO_SURFACE)
  {
    CEGLUtils::Log(LOGERROR, "CAMLSubtitleLayer::CreateEglSurface - failed to create platform "
                             "window surface");
    return false;
  }

  return true;
}

void CAMLSubtitleLayer::DestroyEglSurface()
{
  if (m_eglDisplay != EGL_NO_DISPLAY && m_eglSurface != EGL_NO_SURFACE)
  {
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(m_eglDisplay, m_eglSurface);
    m_eglSurface = EGL_NO_SURFACE;
  }
}

bool CAMLSubtitleLayer::BeginRender()
{
  if (!m_active || m_eglDisplay == EGL_NO_DISPLAY || m_eglSurface == EGL_NO_SURFACE)
    return false;

  // Save the surface that is current so EndRender can restore it.
  m_prevSurface = eglGetCurrentSurface(EGL_DRAW);

  if (eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext) != EGL_TRUE)
  {
    CLog::Log(LOGERROR, "CAMLSubtitleLayer::{} - failed to make subtitle surface current",
              __FUNCTION__);
    m_prevSurface = EGL_NO_SURFACE;
    return false;
  }

  // Clear to fully transparent; only subtitle pixels are written this frame.
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  return true;
}

void CAMLSubtitleLayer::EndRender()
{
  if (!m_active || m_eglDisplay == EGL_NO_DISPLAY || m_eglSurface == EGL_NO_SURFACE)
    return;

  eglSwapBuffers(m_eglDisplay, m_eglSurface);

  m_fbId = 0;
  if (m_gbmUtils->LockSubtitleFrontBuffer(m_display->aml_get_Device_handle()))
    m_fbId = m_gbmUtils->GetSubtitleFBId();

  // Restore the surface that was current before BeginRender so the rest of
  // the GUI render pass keeps drawing into the main (primary plane) surface.
  eglMakeCurrent(m_eglDisplay, m_prevSurface, m_prevSurface, m_eglContext);
  m_prevSurface = EGL_NO_SURFACE;

  if (m_fbId == 0)
    CLog::Log(LOGDEBUG, "CAMLSubtitleLayer::{} - failed to get subtitle fb", __FUNCTION__);
}

void CAMLSubtitleLayer::UpdateHdrState(uint32_t hdrType, int drmFd)
{
  if (!m_active)
    return;

  CAMLHdrLut::ProcessSelect process;

  switch (hdrType)
  {
    case static_cast<uint32_t>(StreamHdrType::HDR_TYPE_HDR10):
    case static_cast<uint32_t>(StreamHdrType::HDR_TYPE_HDR10PLUS):
    case static_cast<uint32_t>(StreamHdrType::HDR_TYPE_DOLBYVISION):
      process = CAMLHdrLut::PROC_SDR_HDR;
      break;
    case static_cast<uint32_t>(StreamHdrType::HDR_TYPE_HLG):
      process = CAMLHdrLut::PROC_SDR_HLG;
      break;
    default:
      process = CAMLHdrLut::PROC_BYPASS;
      break;
  }

  // Configure OSD2_HDR (the subtitle overlay plane) for the selected transfer.
  // The overlay plane is the first OVERLAY DRM plane, typically osd1 = VPP_OSD2.
  m_hdrLut.Configure(drmFd, CAMLHdrLut::MODULE_OSD2, process);

  CLog::Log(LOGDEBUG, "CAMLSubtitleLayer::{} - hdrType={} process={}", __FUNCTION__,
            hdrType, static_cast<int>(process));
}
