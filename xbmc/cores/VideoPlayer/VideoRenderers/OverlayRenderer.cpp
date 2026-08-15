/*
 *      Initial code sponsored by: Voddler Inc (voddler.com)
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OverlayRenderer.h"

#include "OverlayRendererUtil.h"
#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlay.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayImage.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayLibass.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlaySpu.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "video/PlayerController.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <mutex>
#include <utility>

using namespace KODI;
using namespace OVERLAY;

COverlay::COverlay()
{
  m_x = 0.0f;
  m_y = 0.0f;
  m_width = 0.0f;
  m_height = 0.0f;
  m_type = TYPE_NONE;
  m_align = ALIGN_SCREEN;
  m_pos = POSITION_RELATIVE;
}

COverlay::~COverlay() = default;

void OVERLAY::MarkDirty()
{
  CServiceBroker::GetGUI()->GetWindowManager().MarkDirty();
}

unsigned int CRenderer::m_textureid = 1;

CRenderer::CRenderer()
{
  CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()->RegisterObserver(this);
}

CRenderer::~CRenderer()
{
  CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()->UnregisterObserver(this);
  Flush();
}

void CRenderer::AddOverlay(std::shared_ptr<CDVDOverlay> o, double pts, int index)
{
  std::unique_lock lock(m_section);

  SElement   e;
  e.pts = pts;
  e.overlay_dvd = std::move(o);
  m_buffers[index].push_back(e);
}

void CRenderer::Release(std::vector<SElement>& list)
{
  list.clear();
}

void CRenderer::UnInit()
{
  Flush();
}

void CRenderer::Flush()
{
  std::unique_lock lock(m_section);

  for(std::vector<SElement>& buffer : m_buffers)
    Release(buffer);

  ReleaseCache();
  Reset();
}

void CRenderer::Reset()
{
  m_subtitlePosition = 0;
  m_subtitleViewHeight = 0;
}

void CRenderer::Release(int idx)
{
  std::unique_lock lock(m_section);
  Release(m_buffers[idx]);
}

void CRenderer::ReleaseCache()
{
  m_textureCache.clear();
  m_textureid++;
}

void CRenderer::ReleaseUnused()
{
  for (auto it = m_textureCache.begin(); it != m_textureCache.end(); )
  {
    bool found = false;
    for (auto& buffer : m_buffers)
    {
      for (auto& dvdoverlay : buffer)
      {
        if (dvdoverlay.overlay_dvd && dvdoverlay.overlay_dvd->m_textureid == it->first)
        {
          found = true;
          break;
        }
      }
      if (found)
        break;
    }
    if (!found)
    {
      it = m_textureCache.erase(it);
    }
    else
      ++it;
  }
}

void CRenderer::Render(int idx, float depth)
{
  std::unique_lock lock(m_section);

  std::vector<SElement>& list = m_buffers[idx];
  for(std::vector<SElement>::iterator it = list.begin(); it != list.end(); ++it)
  {
    if (it->overlay_dvd)
    {
      std::shared_ptr<COverlay> o = Convert(*it);

      if (o)
        Render(o.get());
    }
  }

  ReleaseUnused();
}

void CRenderer::Render(COverlay* o)
{
  SRenderState state;
  state.x = o->m_x;
  state.y = o->m_y;
  state.width = o->m_width;
  state.height = o->m_height;

  COverlay::EPosition pos = o->m_pos;
  COverlay::EAlign align = o->m_align;

  if (pos == COverlay::POSITION_RELATIVE)
  {
    float scale_x = 1.0;
    float scale_y = 1.0;
    float scale_w = 1.0;
    float scale_h = 1.0;

    if (align == COverlay::ALIGN_SCREEN)
    {
      scale_x = m_rv.Width();
      scale_y = m_rv.Height();
      scale_w = scale_x;
      scale_h = scale_y;
    }
    else if (align == COverlay::ALIGN_SCREEN_AR)
    {
      // Align to screen by keeping aspect ratio to fit into the screen area
      float source_width = o->m_source_width > 0 ? o->m_source_width : m_rs.Width();
      float source_height = o->m_source_height > 0 ? o->m_source_height : m_rs.Height();
      float ratio = std::min<float>(m_rv.Width() / source_width, m_rv.Height() / source_height);
      scale_x = m_rv.Width();
      scale_y = m_rv.Height();
      scale_w = ratio;
      scale_h = ratio;
    }
    else if (align == COverlay::ALIGN_VIDEO)
    {
      scale_x = m_rs.Width();
      scale_y = m_rs.Height();
      scale_w = scale_x;
      scale_h = scale_y;
    }

    state.x *= scale_x;
    state.y *= scale_y;
    state.width *= scale_w;
    state.height *= scale_h;

    pos = COverlay::POSITION_ABSOLUTE;
  }

  if (pos == COverlay::POSITION_ABSOLUTE)
  {
    if (align == COverlay::ALIGN_SCREEN || align == COverlay::ALIGN_SCREEN_AR)
    {
      state.x += m_rv.x1;
      state.y += m_rv.y1;
    }
    else if (align == COverlay::ALIGN_VIDEO)
    {
      float scale_x = m_rd.Width() / m_rs.Width();
      float scale_y = m_rd.Height() / m_rs.Height();

      state.x *= scale_x;
      state.y *= scale_y;
      state.width *= scale_x;
      state.height *= scale_y;

      state.x += m_rd.x1;
      state.y += m_rd.y1;
    }
  }

  state.x += GetStereoscopicDepth(o->m_pgsSubtitle, o->m_3dSubtitleDepth);

  // Classify overlays based on final screen y position
  // Only subtitle overlays in the bottom 20% of the screen are marked as dynamic
  // ALIGN_VIDEO overlays (effect subtitles) are never moved to avoid breaking visual effects
  if (o->m_align == COverlay::ALIGN_VIDEO || o->m_align == COverlay::ALIGN_SCREEN_AR ||
      o->m_align == COverlay::ALIGN_SUBTITLE)
  {
    const float dynamicThreshold = m_rv.y1 + m_rv.Height() * 0.8f;
    o->m_isDynamic = (state.y >= dynamicThreshold);
  }

  // Apply dynamic subtitle offset (percentage of screen height)
  // Only affects subtitles marked as dynamic (m_isDynamic == true)
  if (o->m_isDynamic)
  {
    state.y += m_rv.Height() * m_subtitleDynamicOffset.load(std::memory_order_relaxed) / 100.0f;
  }

  o->Render(state);
}

bool CRenderer::HasVisibleOverlay(int idx) const
{
  std::unique_lock lock(m_section);
  if (idx < 0 || idx >= NUM_BUFFERS)
    return false;

  for (const auto& e : m_buffers[idx])
  {
    if (!e.overlay_dvd)
      continue;

    const CDVDOverlay& o = *e.overlay_dvd;
    // PGS/DVB and DVD SPU: ProcessOverlays inserts these into m_buffers
    // only at PTS values where the bitmap is on screen, so finding one
    // here means it is visible.
    if (o.IsOverlayType(DVDOVERLAY_TYPE_IMAGE) || o.IsOverlayType(DVDOVERLAY_TYPE_SPU))
      return true;

    // libass (TEXT/SSA): the container stays in m_buffers for the whole
    // video (iPTSStopTime=DVD_NOPTS_VALUE). Visibility means
    // ass_render_frame returned images for the current PTS, cached by
    // PrepareOverlays in e.renderedImages.
    if (o.IsOverlayType(DVDOVERLAY_TYPE_TEXT) || o.IsOverlayType(DVDOVERLAY_TYPE_SSA))
    {
      if (e.renderedImages != nullptr)
        return true;
    }
  }
  return false;
}

void CRenderer::SetVideoRect(CRect &source, CRect &dest, CRect &view)
{
  if (m_rv != view) // Screen resolution is changed
  {
    m_rv = view;
    OnViewChange();
  }
  m_rs = source;
  m_rd = dest;
}

void CRenderer::OnViewChange()
{
  m_isSettingsChanged = true;
}

void CRenderer::SetStereoMode(const std::string &stereomode)
{
  m_stereomode = stereomode;
}

void CRenderer::SetSubtitleVerticalPosition(const int value, bool save)
{
  std::unique_lock lock(m_section);
  m_subtitlePosition = value;
}

void CRenderer::SetDynamicSubtitleOffset(const float value)
{
  m_subtitleDynamicOffset.store(value, std::memory_order_relaxed);
  // Force immediate subtitle redraw so the position change is visible
  // on screen without waiting for the next content change event.
  MarkDirty();
}

void CRenderer::ResetSubtitlePosition()
{
  // In the 'pos' var the vertical margin has been substracted because
  // we need to know the actual text baseline position on screen
  int pos{0};
  RESOLUTION_INFO resInfo = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();

  m_subtitleVerticalMargin = static_cast<int>(
      static_cast<float>(m_rv.Height()) / 100 *
      CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()->GetVerticalMarginPerc());

  pos = static_cast<int>(m_rv.Height()) - m_subtitleVerticalMargin + resInfo.Overscan.top;

  // Update player value (and callback to CRenderer::SetSubtitleVerticalPosition)
  auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  appPlayer->SetSubtitleVerticalPosition(pos, false);
}

void CRenderer::CreateSubtitlesStyle()
{
  m_overlayStyle = std::make_shared<SUBTITLES::STYLE::style>();
  const auto settings{CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()};

  m_overlayStyle->fontName = settings->GetFontName();
  m_overlayStyle->fontSize = static_cast<double>(settings->GetFontSize());

  SUBTITLES::FontStyle fontStyle = settings->GetFontStyle();
  if (fontStyle == SUBTITLES::FontStyle::BOLD_ITALIC)
    m_overlayStyle->fontStyle = SUBTITLES::STYLE::FontStyle::BOLD_ITALIC;
  else if (fontStyle == SUBTITLES::FontStyle::BOLD)
    m_overlayStyle->fontStyle = SUBTITLES::STYLE::FontStyle::BOLD;
  else if (fontStyle == SUBTITLES::FontStyle::ITALIC)
    m_overlayStyle->fontStyle = SUBTITLES::STYLE::FontStyle::ITALIC;

  m_overlayStyle->fontColor = settings->GetFontColor();
  m_overlayStyle->fontBorderSize = settings->GetBorderSize();
  m_overlayStyle->fontBorderColor = settings->GetBorderColor();
  m_overlayStyle->fontOpacity = settings->GetFontOpacity();

  SUBTITLES::BackgroundType backgroundType = settings->GetBackgroundType();
  if (backgroundType == SUBTITLES::BackgroundType::NONE)
    m_overlayStyle->borderStyle = SUBTITLES::STYLE::BorderType::OUTLINE_NO_SHADOW;
  else if (backgroundType == SUBTITLES::BackgroundType::SHADOW)
    m_overlayStyle->borderStyle = SUBTITLES::STYLE::BorderType::OUTLINE;
  else if (backgroundType == SUBTITLES::BackgroundType::BOX)
    m_overlayStyle->borderStyle = SUBTITLES::STYLE::BorderType::BOX;
  else if (backgroundType == SUBTITLES::BackgroundType::SQUAREBOX)
    m_overlayStyle->borderStyle = SUBTITLES::STYLE::BorderType::SQUARE_BOX;

  m_overlayStyle->backgroundColor = settings->GetBackgroundColor();
  m_overlayStyle->backgroundOpacity = settings->GetBackgroundOpacity();

  m_overlayStyle->shadowColor = settings->GetShadowColor();
  m_overlayStyle->shadowOpacity = settings->GetShadowOpacity();
  m_overlayStyle->shadowSize = settings->GetShadowSize();

  SUBTITLES::Align subAlign = settings->GetAlignment();
  if (subAlign == SUBTITLES::Align::TOP_INSIDE || subAlign == SUBTITLES::Align::TOP_OUTSIDE)
    m_overlayStyle->alignment = SUBTITLES::STYLE::FontAlign::TOP_CENTER;
  else
    m_overlayStyle->alignment = SUBTITLES::STYLE::FontAlign::SUB_CENTER;

  m_overlayStyle->assOverrideFont = settings->IsOverrideFonts();

  SUBTITLES::OverrideStyles overrideStyles = settings->GetOverrideStyles();
  if (overrideStyles == SUBTITLES::OverrideStyles::POSITIONS)
    m_overlayStyle->assOverrideStyles = SUBTITLES::STYLE::OverrideStyles::POSITIONS;
  else if (overrideStyles == SUBTITLES::OverrideStyles::STYLES)
    m_overlayStyle->assOverrideStyles = SUBTITLES::STYLE::OverrideStyles::STYLES;
  else if (overrideStyles == SUBTITLES::OverrideStyles::STYLES_POSITIONS)
    m_overlayStyle->assOverrideStyles = SUBTITLES::STYLE::OverrideStyles::STYLES_POSITIONS;
  else
    m_overlayStyle->assOverrideStyles = SUBTITLES::STYLE::OverrideStyles::DISABLED;

  // Changing vertical margin while in playback causes side effects when you
  // rewind the video, displaying the previous text position (test Libass 15.2)
  // for now vertical margin setting will be disabled during playback
  m_overlayStyle->marginVertical =
      static_cast<int>(SUBTITLES::STYLE::VIEWPORT_HEIGHT / 100 *
                       static_cast<double>(settings->GetVerticalMarginPerc()));

  m_overlayStyle->blur = settings->GetBlurSize();
  m_overlayStyle->lineSpacing = settings->GetLineSpacing();
}

void CRenderer::PrepareOverlays(int idx)
{
  std::unique_lock lock(m_section);
  if (idx < 0 || idx >= NUM_BUFFERS)
    return;

  bool doMarkDirty = false;
  bool hasImageSpu = false;
  for (auto& e : m_buffers[idx])
  {
    // Clear last frame's cached output; libass may have invalidated the
    // pointer on its next ass_render_frame call.
    // (assDetectChange is consumed by ConvertLibass, not here.)
    e.renderedImages = nullptr;

    if (!e.overlay_dvd)
      continue;

    CDVDOverlay& o = *e.overlay_dvd;

    // PGS/DVB and DVD SPU: only added to m_buffers at their visible PTS,
    // so finding one means it is on screen now. m_textureid == 0 is the
    // "new arrival" signal (also true every frame for animated PGS where
    // each frame is a fresh CDVDOverlay). Disappearance is caught after
    // the loop by the hasImageSpu vs m_prevHadImageSpu check.
    if (o.IsOverlayType(DVDOVERLAY_TYPE_IMAGE) || o.IsOverlayType(DVDOVERLAY_TYPE_SPU))
    {
      hasImageSpu = true;
      if (o.m_textureid == 0)
        doMarkDirty = true;
      continue;
    }

    if (!o.IsOverlayType(DVDOVERLAY_TYPE_TEXT) && !o.IsOverlayType(DVDOVERLAY_TYPE_SSA))
      continue;

    CDVDOverlayLibass& ovAss = static_cast<CDVDOverlayLibass&>(o);
    if (!ovAss.GetLibassHandler())
      continue;

    bool updateStyle = !m_overlayStyle || m_isSettingsChanged;
    if (updateStyle)
    {
      m_isSettingsChanged = false;
      LoadSettings();
      CreateSubtitlesStyle();
    }

    // rOpts setup moved from CRenderer::ConvertLibass; duplicated in CDebugRenderer::CRenderer::Render.
    SUBTITLES::STYLE::renderOpts rOpts;

    // Three rects: source (subtitle canvas), video (playing size), frame
    // (render target; may exceed video to include letterbox bars so libass
    // can place subtitles in them).
    rOpts.sourceWidth = m_rs.Width();
    rOpts.sourceHeight = m_rs.Height();
    rOpts.videoWidth = m_rd.Width();
    rOpts.videoHeight = m_rd.Height();
    rOpts.frameWidth = m_rv.Width();
    rOpts.frameHeight = m_rv.Height();

    // Detect view height changes (resolution change) and reset position
    if (m_subtitleViewHeight != m_rv.Height())
    {
      m_subtitleViewHeight = static_cast<int>(m_rv.Height());
      ResetSubtitlePosition();
      // Restore the dynamic offset from the per-resolution calibration value
      RESOLUTION_INFO resInfo = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
      m_subtitleDynamicOffset.store(static_cast<float>(resInfo.iSubtitleOffset),
                                    std::memory_order_relaxed);
      // Re-apply the PlayerController's persistent offset from the current
      // fullscreen session (e.g. when switching to the next episode without
      // closing fullscreen).  This ensures the sub up/down adjustment from
      // the previous video carries over instantly to the new video.
      float pcOffset = CPlayerController::GetInstance().GetSubtitleOffset();
      if (pcOffset != 0.0f)
      {
        m_subtitleDynamicOffset.store(pcOffset, std::memory_order_relaxed);
      }
    }

    RESOLUTION_INFO resInfo = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
    rOpts.m_par = resInfo.fPixelRatio;

    // rOpts.position and margins (set to style) can invalidate the text
    // positions to subtitles type that make use of margins to position text on
    // the screen (e.g. ASS/WebVTT) then we allow to set them when position
    // override setting is enabled only
    if (ovAss.IsForcedMargins())
    {
      rOpts.marginsMode = SUBTITLES::STYLE::MarginsMode::DISABLED;
    }
    else if (m_subtitleAlign == SUBTITLES::Align::MANUAL)
    {
      // When vertical margins are used Libass apply a displacement in percentage
      // of the height available to line position, this displacement causes
      // problems with subtitle calibration bar on Video Calibration window,
      // so when you moving the subtitle bar of the GUI the text will no longer
      // match the bar, this calculation compensates for the displacement.
      // Note also that the displacement compensation will cause a different
      // default position of the text, different from the other alignment positions
      double posPx = static_cast<double>(m_subtitlePosition - resInfo.Overscan.top);

      double frameHeight = static_cast<double>(rOpts.frameHeight);

      if (m_stereomode == "top_bottom" || m_stereomode == "bottom_top")
      {
        // only half-ou video, ou video don't need to correct frame height
        if (rOpts.sourceWidth / rOpts.sourceHeight > 1.2f)
          frameHeight *= 2.0;
      }

      int assPlayResY = ovAss.GetLibassHandler()->GetPlayResY();
      double assVertMargin = static_cast<double>(m_overlayStyle->marginVertical) *
                             (static_cast<double>(assPlayResY) / 720);
      double vertMarginScaled = assVertMargin / assPlayResY * frameHeight;
      double pos = posPx / (frameHeight - vertMarginScaled);
      // 字幕校准值只影响屏幕底部 20% 区域（从上往下高度 80% 以上）
      // 上方 0-80% 区域不受校准值影响
      rOpts.position = std::min(20.0, 100 - pos * 100);
    }
    else if (m_subtitleAlign == SUBTITLES::Align::BOTTOM_OUTSIDE)
    {
      // To keep consistent the position of text as other alignment positions
      // we avoid apply the displacement compensation
      double posPx =
          static_cast<double>(m_subtitlePosition + m_subtitleVerticalMargin - resInfo.Overscan.top);
      // 字幕校准值只影响屏幕底部 20% 区域（从上往下高度 80% 以上）
      // 上方 0-80% 区域不受校准值影响
      rOpts.position = std::min(20.0, 100 - posPx / static_cast<double>(rOpts.frameHeight) * 100);
    }
    else if (m_subtitleAlign == SUBTITLES::Align::BOTTOM_INSIDE ||
             m_subtitleAlign == SUBTITLES::Align::TOP_INSIDE)
    {
      rOpts.marginsMode = SUBTITLES::STYLE::MarginsMode::INSIDE_VIDEO;
    }

    // Set the horizontal text alignment (currently used to improve readability on CC subtitles only)
    // This setting influence style->alignment property
    if (ovAss.IsTextAlignEnabled())
    {
      if (m_subtitleHorizontalAlign == SUBTITLES::HorizontalAlign::LEFT)
        rOpts.horizontalAlignment = SUBTITLES::STYLE::HorizontalAlign::LEFT;
      else if (m_subtitleHorizontalAlign == SUBTITLES::HorizontalAlign::RIGHT)
        rOpts.horizontalAlignment = SUBTITLES::STYLE::HorizontalAlign::RIGHT;
      else
        rOpts.horizontalAlignment = SUBTITLES::STYLE::HorizontalAlign::CENTER;
    }

    e.renderedFrameWidth = rOpts.frameWidth;
    e.renderedFrameHeight = rOpts.frameHeight;

    // Pull the libass output for this PTS. Cached on the SElement until
    // ConvertLibass consumes it later in this frame's GUI walk.
    int currentChange = 0;
    e.renderedImages = ovAss.GetLibassHandler()->RenderImage(e.pts, rOpts, updateStyle,
                                                             m_overlayStyle, &currentChange);
    if (currentChange > 0)
    {
      // Persist on the overlay so a skipped GUI render does not drop the change.
      ovAss.m_pendingChange = currentChange;
      doMarkDirty = true;
    }
  }

  // PGS/DVB/SPU disappearance: arrival is caught by m_textureid==0 in
  // the loop above. Without this, a PGS subtitle ending leaves its
  // cached bitmap on the GUI plane until something else dirties.
  if (hasImageSpu != m_prevHadImageSpu)
    doMarkDirty = true;
  m_prevHadImageSpu = hasImageSpu;

  if (doMarkDirty)
    MarkDirty();
}

std::shared_ptr<COverlay> CRenderer::ConvertLibass(SElement& e)
{
  // If no images not execute the renderer
  if (!e.renderedImages)
    return nullptr;

  CDVDOverlayLibass& o = static_cast<CDVDOverlayLibass&>(*e.overlay_dvd);

  if (o.m_textureid)
  {
    if (o.m_pendingChange == 0)
    {
      std::map<unsigned int, std::shared_ptr<COverlay>>::iterator it =
          m_textureCache.find(o.m_textureid);
      if (it != m_textureCache.end())
        return it->second;
    }
  }

  std::shared_ptr<COverlay> overlay =
      COverlay::Create(e.renderedImages, e.renderedFrameWidth, e.renderedFrameHeight);

  m_textureCache[m_textureid] = overlay;
  o.m_textureid = m_textureid;
  m_textureid++;
  o.m_pendingChange = 0; // consume
  return overlay;
}

std::shared_ptr<COverlay> CRenderer::Convert(SElement& e)
{
  if (!e.overlay_dvd)
    return nullptr;

  CDVDOverlay& o = *e.overlay_dvd;
  std::shared_ptr<COverlay> r = NULL;

  if (o.IsOverlayType(DVDOVERLAY_TYPE_TEXT) || o.IsOverlayType(DVDOVERLAY_TYPE_SSA))
  {
    CDVDOverlayLibass& ovAss = static_cast<CDVDOverlayLibass&>(o);
    if (!ovAss.GetLibassHandler())
      return nullptr;

    // Build the COverlay from libass output PrepareOverlays cached on e
    // earlier this frame; avoids re-entering libass during render.
    r = ConvertLibass(e);

    if (!r)
      return nullptr;
  }
  else if (o.m_textureid)
  {
    std::map<unsigned int, std::shared_ptr<COverlay>>::iterator it =
        m_textureCache.find(o.m_textureid);
    if (it != m_textureCache.end())
      r = it->second;
  }

  if (r)
  {
    return r;
  }

  if (o.IsOverlayType(DVDOVERLAY_TYPE_IMAGE))
    r = COverlay::Create(static_cast<CDVDOverlayImage&>(o), m_rs);
  else if (o.IsOverlayType(DVDOVERLAY_TYPE_SPU))
    r = COverlay::Create(static_cast<CDVDOverlaySpu&>(o));

  m_textureCache[m_textureid] = r;
  o.m_textureid = m_textureid;
  m_textureid++;

  return r;
}

void CRenderer::Notify(const Observable& obs, const ObservableMessage msg)
{
  switch (msg)
  {
    case ObservableMessageSettingsChanged:
    {
      m_isSettingsChanged = true;
      break;
    }
    default:
      break;
  }
}

void CRenderer::LoadSettings()
{
  const auto settings{CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()};
  m_subtitleHorizontalAlign = settings->GetHorizontalAlignment();
  m_subtitleAlign = settings->GetAlignment();
  ResetSubtitlePosition();
}
