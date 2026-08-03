/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "AMLSubtitleLayer.h"
#include "utils/EGLUtils.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "utils/GlobalsHandling.h"
#include "utils/StreamDetails.h"
#include "WinSystemAmlogic.h"

namespace KODI
{
namespace WINDOWING
{
namespace AML
{

class CWinSystemAmlogicGLESContext : public CWinSystemAmlogic, public CRenderSystemGLES
{
public:
  CWinSystemAmlogicGLESContext();
  virtual ~CWinSystemAmlogicGLESContext() = default;

  using CWinSystemAmlogic::Register;
  static void Register();
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  // Implementation of CWinSystemBase via CWinSystemAmlogic
  CRenderSystemBase *GetRenderSystem() override { return this; }
  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;
  bool CreateNewWindow(const std::string& name,
                       bool fullScreen,
                       RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;

  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;

  virtual std::unique_ptr<CVideoSync> GetVideoSync(CVideoReferenceClock *clock) override;

  bool SupportsStereo(const RenderStereoMode mode) const override;
  void PresentRender(bool rendered, bool videoLayer) override;

  // Subtitle plane rendering. Returns true if a separate subtitle surface was
  // bound and the caller should render overlays into it before EndSubtitleRender.
  bool BeginSubtitleRender();
  void EndSubtitleRender();
  bool HasActiveSubtitleLayer() const override { return m_subtitleRendering; }
  bool HasSubtitleLayer() const override { return m_subtitleLayer && m_subtitleLayer->IsActive(); }
  void UpdateSubtitleHdrState(uint32_t hdrType) override;

  EGLDisplay GetEGLDisplay() const;
  EGLSurface GetEGLSurface() const;
  EGLContext GetEGLContext() const;
  EGLConfig  GetEGLConfig() const;
protected:
  void SetVSyncImpl(bool enable) override;
  void PresentRenderImpl(bool rendered) override {};

private:
  void InitSubtitleLayer(const RESOLUTION_INFO& res);
  void CleanupSubtitleLayer();
  std::unique_ptr<CEGLContextUtils> m_pGLContext;
  std::unique_ptr<CAMLSubtitleLayer> m_subtitleLayer;
  bool m_subtitleRendering{false};
  StreamHdrType m_hdrType = StreamHdrType::HDR_TYPE_NONE;
};

}
}
}
