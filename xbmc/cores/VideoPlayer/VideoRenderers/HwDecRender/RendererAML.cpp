/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererAML.h"

#include "cores/VideoPlayer/DVDCodecs/Video/AMLCodec.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/VideoRenderers/RenderCapture.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "ServiceBroker.h"
#include "settings/AdvancedSettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/ScreenshotAML.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include "platform/linux/SysfsPath.h"

CRendererAML::CRendererAML()
 : m_prevVPts(DVD_NOPTS_VALUE)
 , m_bConfigured(false)
{
  CLog::Log(LOGINFO, "Constructing CRendererAML");
}

CRendererAML::~CRendererAML()
{
  Reset();
  CServiceBroker::GetWinSystem()->GetGfxContext().SetTransferPQ(false);

  // Restore the kernel OSD pipeline to the SDR state. OpenDecoder writes the
  // OSD params at their SDR defaults (osd_pq_bypass=0, osd_gamut_bypass=0,
  // osd_bypass_enable=0); on teardown we force osd_pq_bypass=1 so the GUI
  // (sRGB BT.709, no GLES bake) passes through unconverted on the SDR sink
  // instead of being washed out by a PQ encode on SDR gamma.
  CSysfsPath("/sys/module/aml_media/parameters/osd_pq_bypass", 1);
  CSysfsPath("/sys/module/aml_media/parameters/osd_gamut_bypass", 0);
  CSysfsPath("/sys/module/aml_media/parameters/osd_bypass_enable", 0);
}

CBaseRenderer* CRendererAML::Create(CVideoBuffer *buffer)
{
  if (buffer && dynamic_cast<CAMLVideoBuffer*>(buffer))
    return new CRendererAML();
  return nullptr;
}

bool CRendererAML::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("amlogic", CRendererAML::Create);
  return true;
}

bool CRendererAML::Configure(const VideoPicture &picture, float fps, unsigned int orientation)
{
  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;

  m_iFlags = GetFlagsChromaPosition(picture.chroma_position) |
             GetFlagsColorMatrix(picture.color_space, picture.iWidth, picture.iHeight) |
             GetFlagsColorPrimaries(picture.color_primaries) |
             GetFlagsStereoMode(picture.stereoMode);

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();

  // cpm-style HDR output: the GLES GUI shader (KODI_TRANSFER_PQ) performs the
  // full sRGB->linear->BT.2020->PQ conversion of the GUI/OSD plane, and the
  // kernel is told (CAMLCodec::OpenDecoder osd_pq_bypass=1) plus routed
  // (HDR_BYPASS for HDR sources) to leave the OSD untouched. SetTransferPQ
  // drives the shader variant; it must be true for any HDR10/HDR10+/DV content
  // so the OSD matches the HDR/PQ video output. On SDR content it stays false.
  const bool hdrContent = picture.hdrType == StreamHdrType::HDR_TYPE_HDR10 ||
                          picture.hdrType == StreamHdrType::HDR_TYPE_HDR10PLUS ||
                          picture.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION;
  CLog::Log(LOGDEBUG,
            "CRendererAML::Configure hdrType={} transfer={} hdrContent={}",
            static_cast<int>(picture.hdrType), picture.color_transfer, hdrContent);

  CServiceBroker::GetWinSystem()->GetGfxContext().SetTransferPQ(hdrContent);

  m_bConfigured = true;

  return true;
}

CRenderInfo CRendererAML::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = m_numRenderBuffers;
  info.opaque_pointer = (void *)this;
  return info;
}

bool CRendererAML::RenderCapture(int index, CRenderCapture* capture)
{
  capture->BeginRender();
  capture->EndRender();
  CScreenshotAML::CaptureVideoFrame((unsigned char *)capture->GetRenderBuffer(), capture->GetWidth(), capture->GetHeight());
  return true;
}

void CRendererAML::AddVideoPicture(const VideoPicture &picture, int index)
{
  ReleaseBuffer(index);

  BUFFER &buf(m_buffers[index]);
  if (picture.videoBuffer)
  {
    buf.videoBuffer = picture.videoBuffer;
    buf.videoBuffer->Acquire();
  }
}

void CRendererAML::ReleaseBuffer(int idx)
{
  BUFFER &buf(m_buffers[idx]);
  if (buf.videoBuffer)
  {
    CAMLVideoBuffer *amli(dynamic_cast<CAMLVideoBuffer*>(buf.videoBuffer));
    if (amli)
    {
      if (amli->m_amlCodec)
      {
        amli->m_amlCodec->ReleaseFrame(amli->m_bufferIndex, true);
        amli->m_amlCodec = nullptr; // Released
      }
      amli->Release();
    }
    buf.videoBuffer = nullptr;
  }
}

bool CRendererAML::Supports(ERENDERFEATURE feature) const
{
  if (feature == RENDERFEATURE_ZOOM ||
      feature == RENDERFEATURE_CONTRAST ||
      feature == RENDERFEATURE_BRIGHTNESS ||
      feature == RENDERFEATURE_NONLINSTRETCH ||
      feature == RENDERFEATURE_VERTICAL_SHIFT ||
      feature == RENDERFEATURE_STRETCH ||
      feature == RENDERFEATURE_PIXEL_RATIO ||
      feature == RENDERFEATURE_ROTATION)
    return true;

  return false;
}

void CRendererAML::Reset()
{
  std::array<int, 2> reset_arr[m_numRenderBuffers];
  m_prevVPts = DVD_NOPTS_VALUE;

  for (int i = 0 ; i < m_numRenderBuffers ; ++i)
  {
    reset_arr[i][0] = i;

    if (m_buffers[i].videoBuffer)
      reset_arr[i][1] = dynamic_cast<CAMLVideoBuffer *>(m_buffers[i].videoBuffer)->m_bufferIndex;
    else
      reset_arr[i][1] = 0;
  }

  std::sort(std::begin(reset_arr), std::end(reset_arr),
    [](const std::array<int, 2>& u, const std::array<int, 2>& v)
    {
      return u[1] < v[1];
    });

  for (int i = 0; i < m_numRenderBuffers; ++i)
  {
    if (m_buffers[reset_arr[i][0]].videoBuffer)
    {
      m_buffers[reset_arr[i][0]].videoBuffer->Release();
      m_buffers[reset_arr[i][0]].videoBuffer = nullptr;
    }
  }
}

bool CRendererAML::Flush(bool saveBuffers)
{
  Reset();
  return saveBuffers;
};

void CRendererAML::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  ManageRenderArea();

  CAMLVideoBuffer *amli = dynamic_cast<CAMLVideoBuffer *>(m_buffers[index].videoBuffer);
  if(amli && amli->m_amlCodec)
  {
    uint64_t pts = amli->m_omxPts;
    if (pts != m_prevVPts)
    {
      amli->m_amlCodec->ReleaseFrame(amli->m_bufferIndex);
      amli->m_amlCodec->SetVideoRect(m_sourceRect, m_destRect);
      amli->m_amlCodec = nullptr; //Mark frame as processed
      m_prevVPts = pts;
    }
  }
  CAMLCodec::PollFrame();
}
