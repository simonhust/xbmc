/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <algorithm>
#include <math.h>
#include <utility>

#include "DVDCodecs/DVDFactoryCodec.h"
#include "utils/MemUtils.h"
#include "DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "AMLCodec.h"
#include "ServiceBroker.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/Thread.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

extern "C"
{
#include <libavutil/hdr_dynamic_metadata.h>
}

#define __MODULE_NAME__ "DVDVideoCodecAmlogic"

CAMLVideoBufferPool::~CAMLVideoBufferPool()
{
  CLog::Log(LOGDEBUG, "CAMLVideoBufferPool::~CAMLVideoBufferPool: Deleting {:d} buffers", static_cast<unsigned int>(m_videoBuffers.size()) );
  for (auto buffer : m_videoBuffers)
    delete buffer;
}

CVideoBuffer* CAMLVideoBufferPool::Get()
{
  std::unique_lock<CCriticalSection> lock(m_criticalSection);

  if (m_freeBuffers.empty())
  {
    m_freeBuffers.push_back(m_videoBuffers.size());
    m_videoBuffers.push_back(new CAMLVideoBuffer(static_cast<int>(m_videoBuffers.size())));
  }
  int bufferIdx(m_freeBuffers.back());
  m_freeBuffers.pop_back();

  m_videoBuffers[bufferIdx]->Acquire(shared_from_this());

  return m_videoBuffers[bufferIdx];
}

void CAMLVideoBufferPool::Return(int id)
{
  std::unique_lock<CCriticalSection> lock(m_criticalSection);
  if (m_videoBuffers[id]->m_amlCodec)
  {
    m_videoBuffers[id]->m_amlCodec->ReleaseFrame(m_videoBuffers[id]->m_bufferIndex, true);
    m_videoBuffers[id]->m_amlCodec = nullptr;
  }
  m_freeBuffers.push_back(id);
}

/***************************************************************************/

CDVDVideoCodecAmlogic::CDVDVideoCodecAmlogic(CProcessInfo &processInfo)
  : CDVDVideoCodec(processInfo)
  , m_pFormatName("amcodec")
  , m_opened(false)
  , m_codecControlFlags(0)
  , m_framerate(0.0)
  , m_video_rate(0)
  , m_mpeg2_sequence(NULL)
  , m_h264_sequence(NULL)
  , m_has_keyframe(false)
  , m_bitparser(NULL)
  , m_bitstream(NULL)
{
}

CDVDVideoCodecAmlogic::~CDVDVideoCodecAmlogic()
{
  Close();
}

std::unique_ptr<CDVDVideoCodec> CDVDVideoCodecAmlogic::Create(CProcessInfo& processInfo)
{
  return std::make_unique<CDVDVideoCodecAmlogic>(processInfo);
}

bool CDVDVideoCodecAmlogic::Register()
{
  CDVDFactoryCodec::RegisterHWVideoCodec("amlogic_dec", CDVDVideoCodecAmlogic::Create);
  return true;
}

bool CDVDVideoCodecAmlogic::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  if (!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEAMCODEC))
    return false;
  if ((hints.stills && hints.fpsrate == 0) || hints.width == 0)
    return false;

  // close open decoder if necessary
  if (m_opened)
    Close();

  m_hints = hints;
  m_hints.pClock = hints.pClock;

  m_nalLengthSize = 0;
  m_streamMeta = {};
  m_stripHdr10Plus = false;
  m_metadataSequencer.Reset();

  CLog::Log(LOGDEBUG, "{}::{} - codec {:d} profile:{:d} extra_size:{:d} fps:{:d}/{:d}",
    __MODULE_NAME__, __FUNCTION__, m_hints.codec, m_hints.profile, m_hints.extradata.GetSize(), m_hints.fpsrate, m_hints.fpsscale);

  switch(m_hints.codec)
  {
    case AV_CODEC_ID_MJPEG:
      m_pFormatName = "am-mjpeg";
      break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
      if (m_hints.width <= CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECMPEG2))
        goto FAIL;

      switch(m_hints.profile)
      {
        case AV_PROFILE_MPEG2_422:
          CLog::Log(LOGDEBUG, "{}: MPEG2 unsupported hints.profile({:d})", __MODULE_NAME__, m_hints.profile);
          goto FAIL;
      }

      // if we have SD PAL content assume it is widescreen
      // correct aspect ratio will be detected later anyway
      if ((m_hints.width == 720 || m_hints.width == 544 || m_hints.width == 480) && m_hints.height == 576 && m_hints.aspect == 0.0)
          m_hints.aspect = 16.0 / 9.0;

      m_mpeg2_sequence_pts = 0;
      m_mpeg2_sequence = new mpeg2_sequence;
      m_mpeg2_sequence->width  = m_hints.width;
      m_mpeg2_sequence->height = m_hints.height;
      m_mpeg2_sequence->ratio  = m_hints.aspect;
      m_mpeg2_sequence->fps_rate  = m_hints.fpsrate;
      m_mpeg2_sequence->fps_scale  = m_hints.fpsscale;
      m_pFormatName = "am-mpeg2";
      break;
    case AV_CODEC_ID_H264:
      if (m_hints.width <= CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECH264))
      {
        CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::h264 size check failed {:d}",CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECH264));
        goto FAIL;
      }
      switch(hints.profile)
      {
        case AV_PROFILE_H264_HIGH_10:
        case AV_PROFILE_H264_HIGH_10_INTRA:
        case AV_PROFILE_H264_HIGH_422:
        case AV_PROFILE_H264_HIGH_422_INTRA:
        case AV_PROFILE_H264_HIGH_444_PREDICTIVE:
        case AV_PROFILE_H264_HIGH_444_INTRA:
        case AV_PROFILE_H264_CAVLC_444:
          CLog::Log(LOGDEBUG, "{}: H264 unsupported hints.profile({:d})", __MODULE_NAME__, m_hints.profile);
          goto FAIL;
      }
      if ((aml_support_h264_4k2k() == AML_NO_H264_4K2K) && ((m_hints.width > 1920) || (m_hints.height > 1088)))
      {
        CLog::Log(LOGDEBUG, "{}::{} - 4K H264 is supported only on Amlogic S802 and S812 chips or newer", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }

      if (m_hints.aspect == 0.0)
      {
        m_h264_sequence_pts = 0;
        m_h264_sequence = new h264_sequence;
        m_h264_sequence->width  = m_hints.width;
        m_h264_sequence->height = m_hints.height;
        m_h264_sequence->ratio  = m_hints.aspect;
      }

      if (m_hints.codec_tag == MKTAG('M', 'V', 'C', '1'))
        m_pFormatName = "am-h264mvc";
      else
        m_pFormatName = "am-h264";
      // convert h264-avcC to h264-annex-b as h264-avcC
      // under streamers can have issues when seeking.
      if (m_hints.extradata && m_hints.extradata.GetData()[0] == 1)
      {
        m_bitstream = new CBitstreamConverter;
        m_bitstream->Open(m_hints.codec, m_hints.extradata.GetData(), m_hints.extradata.GetSize(), true);
        m_bitstream->ResetStartDecode();
        // make sure we do not leak the existing m_hints.extradata
        m_hints.extradata = {};
        m_hints.extradata = FFmpegExtraData(m_bitstream->GetExtraSize());
        memcpy(m_hints.extradata.GetData(), m_bitstream->GetExtraData(), m_hints.extradata.GetSize());
      }
      else
      {
        m_bitparser = new CBitstreamParser();
        m_bitparser->Open();
      }

      // if we have SD PAL content assume it is widescreen
      // correct aspect ratio will be detected later anyway
      if (m_hints.width == 720 && m_hints.height == 576 && m_hints.aspect == 0.0)
          m_hints.aspect = 16.0 / 9.0;

      // assume widescreen for "HD Lite" channels
      // correct aspect ratio will be detected later anyway
      if ((m_hints.width == 1440 || m_hints.width ==1280) && m_hints.height == 1080 && m_hints.aspect == 0.0)
          m_hints.aspect = 16.0 / 9.0;;

      break;
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_MSMPEG4V2:
    case AV_CODEC_ID_MSMPEG4V3:
      if (m_hints.width <= CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECMPEG4))
        goto FAIL;
      m_pFormatName = "am-mpeg4";
      break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
      // amcodec can't handle h263
      CLog::Log(LOGDEBUG, "{}::{} - amcodec does not support H263", __MODULE_NAME__, __FUNCTION__);
      goto FAIL;
//    case AV_CODEC_ID_FLV1:
//      m_pFormatName = "am-flv1";
//      break;
    case AV_CODEC_ID_RV10:
    case AV_CODEC_ID_RV20:
    case AV_CODEC_ID_RV30:
    case AV_CODEC_ID_RV40:
      // m_pFormatName = "am-rv";
      // rmvb is not handled well by amcodec
      CLog::Log(LOGDEBUG, "{}::{} - amcodec does not support RMVB", __MODULE_NAME__, __FUNCTION__);
      goto FAIL;
    case AV_CODEC_ID_VC1:
      m_pFormatName = "am-vc1";
      break;
    case AV_CODEC_ID_WMV3:
      m_pFormatName = "am-wmv3";
      break;
    case AV_CODEC_ID_AVS:
    case AV_CODEC_ID_CAVS:
      m_pFormatName = "am-avs";
      break;
    case AV_CODEC_ID_AVS2:
      if (!aml_support_avs2())
      {
        CLog::Log(LOGDEBUG, "{}::{} - AVS2 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-avs2";
      break;
    case AV_CODEC_ID_AVS3:
      if (!aml_support_avs3())
      {
        CLog::Log(LOGDEBUG, "{}::{} - AVS3 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-avs3";
      break;
    case AV_CODEC_ID_VP9:
      if (!aml_support_vp9())
      {
        CLog::Log(LOGDEBUG, "{}::{} - VP9 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-vp9";
      break;
    case AV_CODEC_ID_AV1:
      if (!aml_support_av1())
      {
        CLog::Log(LOGDEBUG, "{}::{} - AV1 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-av1";
      break;
    case AV_CODEC_ID_HEVC:
      if (aml_support_hevc()) {
        if (!aml_support_hevc_8k4k() && ((m_hints.width > 4096) || (m_hints.height > 2176)))
        {
          CLog::Log(LOGDEBUG, "{}::{} - 8K HEVC hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
          goto FAIL;
        } else if (!aml_support_hevc_4k2k() && ((m_hints.width > 1920) || (m_hints.height > 1088)))
        {
          CLog::Log(LOGDEBUG, "{}::{} - 4K HEVC hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
          goto FAIL;
        }
      } else {
        CLog::Log(LOGDEBUG, "{}::{} - HEVC hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      if ((hints.profile == AV_PROFILE_HEVC_MAIN_10) && !aml_support_hevc_10bit())
      {
        CLog::Log(LOGDEBUG, "{}::{} - HEVC 10-bit hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-h265";
      m_bitstream = new CBitstreamConverter();
      m_bitstream->Open(m_hints.codec, m_hints.extradata.GetData(), m_hints.extradata.GetSize(), true);

      // length-prefix size from the original hvcC, read before the extradata
      // below becomes Annex-B. Stays 0 for Annex-B input
      if (m_hints.extradata.GetSize() > 21 && m_hints.extradata.GetData()[0] == 1)
        m_nalLengthSize = (m_hints.extradata.GetData()[21] & 0x3) + 1;

      // check for hevc-hvcC and convert to h265-annex-b
      if (m_hints.extradata && !m_hints.cryptoSession)
      {
        if (aml_support_dolby_vision())
        {
          bool user_dv_disable = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
              CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE);

          if (!user_dv_disable && CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
                  CSettings::SETTING_COREELEC_AMLOGIC_DV_LED) == AML_DV_TV_LED)
          {
            const bool zeroLevel5 = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
                CSettings::SETTING_VIDEOPLAYER_DOVIZEROLEVEL5);
            m_bitstream->SetDoviZeroLevel5(zeroLevel5);
            if (zeroLevel5)
              m_streamMeta.flags.push_back("l5-zeroed");
          }

          if ((m_hints.dovi.dv_profile == 4 || m_hints.dovi.dv_profile == 7) && !user_dv_disable)
          {
            if (aml_get_cpufamily_id() == AML_S5)
            {
              CLog::Log(LOGINFO, "{}::{} - HEVC bitstream profile {} will be converted to profile 8.1", __MODULE_NAME__, __FUNCTION__,
                m_hints.dovi.dv_profile);

              m_hints.dovi.dv_profile = 8;
              m_hints.dovi.el_present_flag = false;
              m_bitstream->SetConvertDovi(true);
              m_streamMeta.flags.push_back("converted");
            }
          }
        }
      }

      // make sure we do not leak the existing m_hints.extradata
      m_hints.extradata = {};
      m_hints.extradata = FFmpegExtraData(m_bitstream->GetExtraSize());
      memcpy(m_hints.extradata.GetData(), m_bitstream->GetExtraData(), m_hints.extradata.GetSize());
      break;
    case AV_CODEC_ID_VVC:
      if (!aml_support_h266())
      {
        CLog::Log(LOGDEBUG, "{}::{} - H266 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-h266";
      m_bitstream = new CBitstreamConverter();
      m_bitstream->Open(m_hints.codec, m_hints.extradata.GetData(), m_hints.extradata.GetSize(), true);
      if (m_hints.extradata.GetSize() == 0)
        m_bitstream->ResetStartDecode();
      break;
    default:
      CLog::Log(LOGDEBUG, "{}: Unknown hints.codec({:d})", __MODULE_NAME__, m_hints.codec);
      goto FAIL;
  }

  m_aspect_ratio = m_hints.aspect;

  m_Codec = std::shared_ptr<CAMLCodec>(new CAMLCodec(m_processInfo));
  if (!m_Codec)
  {
    CLog::Log(LOGERROR, "{}: Failed to create Amlogic Codec", __MODULE_NAME__);
    goto FAIL;
  }

  // allocate a dummy VideoPicture buffer.
  m_videobuffer.Reset();

  m_videobuffer.iWidth  = m_hints.width;
  m_videobuffer.iHeight = m_hints.height;

  m_videobuffer.iDisplayWidth  = m_videobuffer.iWidth;
  m_videobuffer.iDisplayHeight = m_videobuffer.iHeight;
  if (m_hints.aspect > 0.0 && !m_hints.forced_aspect)
  {
    m_videobuffer.iDisplayWidth  = ((int)lrint(m_videobuffer.iHeight * m_hints.aspect)) & ~3;
    if (m_videobuffer.iDisplayWidth > m_videobuffer.iWidth)
    {
      m_videobuffer.iDisplayWidth  = m_videobuffer.iWidth;
      m_videobuffer.iDisplayHeight = ((int)lrint(m_videobuffer.iWidth / m_hints.aspect)) & ~3;
    }
  }

  m_videobuffer.hdrType = m_hints.hdrType;
  m_videobuffer.color_space = m_hints.colorSpace;
  m_videobuffer.color_primaries = m_hints.colorPrimaries;
  m_videobuffer.color_transfer = m_hints.colorTransferCharacteristic;

  m_processInfo.SetVideoDecoderName(m_pFormatName, true);
  m_processInfo.SetVideoDimensions(m_hints.width, m_hints.height);
  m_processInfo.SetVideoDeintMethod("hardware");
  m_processInfo.SetVideoDAR(m_hints.aspect);

  m_has_keyframe = false;

  // No SEI stripping at open time: the multi-HDR filter (AddData) decides
  // after the first frame's bitstream detection. All SEIs pass through to
  // the decoder by default so the kernel can natively detect HDR formats.

  if (m_hints.contentLightMetadata)
    m_streamMeta.hdrCll = AMLSerializeContentLight(*m_hints.contentLightMetadata);
  if (m_hints.masteringMetadata &&
      (m_hints.masteringMetadata->has_primaries || m_hints.masteringMetadata->has_luminance))
    m_streamMeta.hdrMdcv = AMLSerializeMastering(*m_hints.masteringMetadata);
  // config record and EL presence from hints, not m_hints, which the P7 to P8
  // conversion above has already rewritten
  if (hints.dovi.dv_profile > 0)
    m_streamMeta.doviConfig = AMLSerializeDoviConfig(hints.dovi);
  m_dualLayer = hints.dovi.el_present_flag;

  m_pendingMeta = m_streamMeta;
  m_lastMeta = m_streamMeta;
  m_metadataToken = CAMLFrameMetadataStore::GetInstance().Register();
  CAMLFrameMetadataStore::GetInstance().Publish(m_metadataToken, m_streamMeta);

  CLog::Log(LOGINFO, "{}: Opened Amlogic Codec", __MODULE_NAME__);
  return true;
FAIL:
  Close();
  return false;
}

void CDVDVideoCodecAmlogic::Close(void)
{
  CLog::Log(LOGDEBUG, "{}::{}", __MODULE_NAME__, __FUNCTION__);

  // a successor codec may already own the store, so Unregister only clears our own values
  if (m_metadataToken)
  {
    CAMLFrameMetadataStore::GetInstance().Unregister(m_metadataToken);
    m_metadataToken = 0;
  }

  m_videoBufferPool = nullptr;

  if (m_Codec)
    m_Codec->CloseDecoder(), m_Codec = nullptr;

  m_videobuffer.iFlags = 0;

  if (m_mpeg2_sequence)
    delete m_mpeg2_sequence, m_mpeg2_sequence = NULL;
  if (m_h264_sequence)
    delete m_h264_sequence, m_h264_sequence = NULL;

  if (m_bitstream)
    delete m_bitstream, m_bitstream = NULL;

  if (m_bitparser)
    delete m_bitparser, m_bitparser = NULL;

  m_opened = false;

  ClearBitstreamCommon();
}

void CDVDVideoCodecAmlogic::ClearBitstreamCommon(void)
{
  for (auto& pkt : m_el_packages)
    KODI::MEMORY::AlignedFree(std::get<0>(pkt));
  m_el_packages.clear();

  for (auto& pkt : m_bl_packages)
    KODI::MEMORY::AlignedFree(std::get<0>(pkt));
  m_bl_packages.clear();

  for (auto& pkt : m_packages)
    KODI::MEMORY::AlignedFree(std::get<0>(pkt));
  m_packages.clear();

  for (auto& buf : m_resume_buffers)
    KODI::MEMORY::AlignedFree(buf.data);
  m_resume_buffers.clear();
  m_resume_pair_count = 0;

  m_last_added = true;
  m_last_pData = nullptr;
  m_last_iSize = 0;
  m_last_dts = DVD_NOPTS_VALUE;
  m_ready_to_pair = false;
  m_switched_to_dual = false;

  if (m_bitstream) m_bitstream->ResetStartDecode();
}

void CDVDVideoCodecAmlogic::DualLayerAccumulate(const DemuxPacket &packet)
{
  CLog::Log(LOGDEBUG, "DV: DualLayerAccumulate, EL={}, dts={:.3f}, el_q={}, bl_q={}, ready={}",
    packet.isELPackage, packet.dts / DVD_TIME_BASE, m_el_packages.size(), m_bl_packages.size(), m_ready_to_pair);

  /* Insert packet into the appropriate queue (sorted by DTS ascending) */
  auto pkt = static_cast<uint8_t*>(KODI::MEMORY::AlignedMalloc(packet.iSize + AV_INPUT_BUFFER_PADDING_SIZE, 16));
  memcpy(pkt, packet.pData, packet.iSize);
  DLDemuxPacket new_pkt(pkt, packet.iSize, packet.isELPackage, packet.dts);

  if (packet.isELPackage)
  {
    auto it = m_el_packages.begin();
    while (it != m_el_packages.end() && std::get<3>(*it) < packet.dts)
      ++it;
    m_el_packages.emplace(it, std::move(new_pkt));
  }
  else
  {
    auto it = m_bl_packages.begin();
    while (it != m_bl_packages.end() && std::get<3>(*it) < packet.dts)
      ++it;
    m_bl_packages.emplace(it, std::move(new_pkt));
  }

  /* Already ready to pair — no need to re-check */
  if (m_ready_to_pair)
    return;

  /* Need both queues to have accumulated enough packets */
  if (m_el_packages.size() <= 6 || m_bl_packages.size() <= 6)
    return;

  /* Find overlap range: max(el_min, bl_min) <= min(el_max, bl_max) */
  double el_min = std::get<3>(m_el_packages.front());
  double el_max = std::get<3>(m_el_packages.back());
  double bl_min = std::get<3>(m_bl_packages.front());
  double bl_max = std::get<3>(m_bl_packages.back());

  double overlap_start = std::max(el_min, bl_min);
  double overlap_end   = std::min(el_max, bl_max);

  if (overlap_start > overlap_end)
  {
    /* No overlap — queues are still disjoint, keep accumulating */
    return;
  }

  /* Trim queues: discard packets with DTS < overlap_start (impossible to pair).
   * Skip trim during the first second of playback — the hardware has tolerance
   * for initial bad frames, and trimming can cause unnecessary misalignment. */
  if (overlap_start >= DVD_TIME_BASE * 1.0)
  {
    auto trim_queue = [](std::list<DLDemuxPacket> &q, double boundary) {
      while (!q.empty() && std::get<3>(q.front()) < boundary)
      {
        KODI::MEMORY::AlignedFree(std::get<0>(q.front()));
        q.pop_front();
      }
    };
    trim_queue(m_el_packages, overlap_start);
    trim_queue(m_bl_packages, overlap_start);
  }

  m_ready_to_pair = true;
  CLog::Log(LOGDEBUG, "DV: DualLayerAccumulate — ready to pair, el_q={}, bl_q={}",
    m_el_packages.size(), m_bl_packages.size());
}

bool CDVDVideoCodecAmlogic::DualLayerTryPair()
{
  if (!m_ready_to_pair)
    return false;

  if (m_el_packages.empty() || m_bl_packages.empty())
    return false;

  double el_dts = std::get<3>(m_el_packages.front());
  double bl_dts = std::get<3>(m_bl_packages.front());

  if (std::abs(bl_dts - el_dts) > 5000.0)
    return false;

  /* Match! Convert and output one pair */
  DLDemuxPacket bl_pkt = std::move(m_bl_packages.front());
  m_bl_packages.pop_front();
  DLDemuxPacket el_pkt = std::move(m_el_packages.front());
  m_el_packages.pop_front();

  uint8_t *bl_data = std::get<0>(bl_pkt);
  uint32_t bl_size = std::get<1>(bl_pkt);
  uint8_t *el_data = std::get<0>(el_pkt);
  uint32_t el_size = std::get<1>(el_pkt);

  bool converted = m_bitstream->Convert(bl_data, bl_size, el_data, el_size);
  KODI::MEMORY::AlignedFree(bl_data);
  KODI::MEMORY::AlignedFree(el_data);

  if (converted)
  {
    m_last_pData = m_bitstream->GetConvertBuffer();
    m_last_iSize = m_bitstream->GetConvertSize();
    m_last_dts = bl_dts;
    m_last_added = true;
    return true;
  }

  return false;
}

bool CDVDVideoCodecAmlogic::SingleLayerConvert(uint8_t *pData, uint32_t iSize, const DemuxPacket &packet) const
{
  if (!m_bitstream->Convert(pData, iSize))
    return false;

  if (!m_bitstream->CanStartDecode())
  {
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::{}: waiting for keyframe (bitstream)", __FUNCTION__);
    return false;
  }

  return true;
}

bool CDVDVideoCodecAmlogic::AddData(const DemuxPacket &packet)
{
  // Handle Input, add demuxer packet to input queue, we must accept it or
  // it will be discarded as VideoPlayerVideo has no concept of "try again".

  DrainMetadataToClock();

  uint8_t *pData(packet.pData);
  uint32_t iSize(packet.iSize);
  bool doviIsFEL = false;
  bool IsHdr10Plus = false;
  bool IsHdrVivid = false;
  bool hasDv = false;
  AMLHdrPath hdrPath;

  if (pData)
  {
    // named by how the EL arrives; a track pair can open with solo packets, so
    // dt-dl may correct an early st-dl
    if (m_dualLayer && m_streamMeta.structure != "dt-dl")
    {
      m_streamMeta.structure = packet.isDualStream ? "dt-dl" : "st-dl";
      m_pendingMeta.structure = m_streamMeta.structure;
    }

    // latch from the original demuxer payload, before Convert() can strip or
    // rewrite it. For dual-track streams the RPU lives in the base layer, so
    // it is latched from the BL packets as they arrive (the EL carries its
    // own static SEIs with different values); this covers both the single
    // queue (m_packages) and the dual queue (m_bl_packages/m_el_packages)
    // pairing paths, which share this entry point.
    if (!packet.isDualStream && m_hints.hdrType != StreamHdrType::HDR_TYPE_NONE)
    {
      switch(m_hints.codec)
      {
        case AV_CODEC_ID_HEVC:
          AMLLatchHevcDoviRpu(pData, iSize, m_nalLengthSize, m_pendingMeta);
          AMLLatchHevcSei(pData, iSize, m_nalLengthSize, m_pendingMeta);
          // the statics repeat rarely, so they persist where a skip cannot drop them
          if (!m_pendingMeta.hdrMdcv.empty())
            m_streamMeta.hdrMdcv = m_pendingMeta.hdrMdcv;
          if (!m_pendingMeta.hdrCll.empty())
            m_streamMeta.hdrCll = m_pendingMeta.hdrCll;
          break;
        case AV_CODEC_ID_AV1:
          AMLLatchAv1Metadata(pData, iSize, m_pendingMeta);
          break;
        default:
          break;
      }
    }
    else if (packet.isDualStream && !packet.isELPackage &&
             m_hints.hdrType != StreamHdrType::HDR_TYPE_NONE)
    {
      // latch from packet.pData, the untouched demuxer payload, before the
      // packet is queued for either pairing path
      switch(m_hints.codec)
      {
        case AV_CODEC_ID_HEVC:
          AMLLatchHevcDoviRpu(packet.pData, packet.iSize, m_nalLengthSize, m_pendingMeta);
          AMLLatchHevcSei(packet.pData, packet.iSize, m_nalLengthSize, m_pendingMeta);
          if (!m_pendingMeta.hdrMdcv.empty())
            m_streamMeta.hdrMdcv = m_pendingMeta.hdrMdcv;
          if (!m_pendingMeta.hdrCll.empty())
            m_streamMeta.hdrCll = m_pendingMeta.hdrCll;
          break;
        case AV_CODEC_ID_AV1:
          AMLLatchAv1Metadata(packet.pData, packet.iSize, m_pendingMeta);
          break;
        default:
          break;
      }
    }

    if (m_bitstream)
    {
      if (!m_last_added)
      {
        pData = m_last_pData;
        iSize = m_last_iSize;
      }
      else
      {
        bool dual_layer_queued = false;
        if (packet.isDualStream)
        {
          if (packet.isNoElEpMap && !packet.isMultiClip)
          {
            /* Always feed the dual-queue for synchronization. */
            DualLayerAccumulate(packet);

            if (packet.isDirectPair)
            {
              /* Non-disc: direct dual-queue pairing, no 1s delay */
              if (!m_switched_to_dual)
              {
                m_switched_to_dual = true;

                while (!m_packages.empty())
                {
                  KODI::MEMORY::AlignedFree(std::get<0>(m_packages.front()));
                  m_packages.pop_front();
                }

                auto trim_to = [](std::list<DLDemuxPacket> &q, double boundary) {
                  while (!q.empty() && std::get<3>(q.front()) < boundary)
                  {
                    KODI::MEMORY::AlignedFree(std::get<0>(q.front()));
                    q.pop_front();
                  }
                };
                trim_to(m_el_packages, packet.dts);
                trim_to(m_bl_packages, packet.dts);

                m_ready_to_pair = false;
              }
            }
            else if (!m_switched_to_dual && packet.dts >= DVD_TIME_BASE * 1.0)
            {
              /* Blu-ray: switch at 1s boundary */
              m_switched_to_dual = true;

              while (!m_packages.empty())
              {
                KODI::MEMORY::AlignedFree(std::get<0>(m_packages.front()));
                m_packages.pop_front();
              }

              auto trim_to = [](std::list<DLDemuxPacket> &q, double boundary) {
                while (!q.empty() && std::get<3>(q.front()) < boundary)
                {
                  KODI::MEMORY::AlignedFree(std::get<0>(q.front()));
                  q.pop_front();
                }
              };
              trim_to(m_el_packages, packet.dts);
              trim_to(m_bl_packages, packet.dts);

              m_ready_to_pair = false;
            }

            if (m_switched_to_dual)
            {
              if (!DualLayerTryPair())
                dual_layer_queued = true;
            }
            else
            {
            /* EL has EP_map: use simple single-queue pairing */
            bool dual_layer_converted = false;

            if (!m_packages.empty())
            {
              DLDemuxPacket queued = m_packages.front();
              uint8_t *qData = std::get<0>(queued);
              uint32_t qSize = std::get<1>(queued);
              bool qIsEL = std::get<2>(queued);

              if (qIsEL != packet.isELPackage)
              {
                if (!packet.isELPackage)
                  dual_layer_converted = m_bitstream->Convert(pData, iSize, qData, qSize);
                else
                  dual_layer_converted = m_bitstream->Convert(qData, qSize, pData, iSize);
              }
            }

            if (dual_layer_converted)
            {
              KODI::MEMORY::AlignedFree(std::get<0>(m_packages.front()));
              m_packages.pop_front();
            }
            else
            {
              uint8_t *pkt = static_cast<uint8_t*>(KODI::MEMORY::AlignedMalloc(packet.iSize + AV_INPUT_BUFFER_PADDING_SIZE, 16));
              memcpy(pkt, packet.pData, packet.iSize);
              m_packages.emplace_back(pkt, iSize, packet.isELPackage, packet.dts);
              return true;
            }
          }
          }
          else
          {
            /* EL has EP_map: use simple single-queue pairing */
            /* Seek-time filter: active from seek until first BL+EL pair
             * succeeds. Skip orphan packets with DTS before the first
             * post-seek packet (the EP-map entry point).  Using the first
             * packet's DTS instead of seekTime avoids discarding the
             * entry-point frames that legitimately fall before the seek
             * target. */
            if (packet.m_seekTime != m_dv_seek_time_seen)
            {
              m_dv_seek_time_seen = packet.m_seekTime;
              m_dv_seek_filter_active = (packet.m_seekTime != DVD_NOPTS_VALUE);
              m_dv_seek_first_dts = DVD_NOPTS_VALUE;
            }

            if (!packet.isMultiClip && m_dv_seek_filter_active &&
                packet.m_seekTime >= DVD_TIME_BASE * 1.0 &&
                packet.dts != DVD_NOPTS_VALUE)
            {
              double dts_val = packet.dts;
              if (dts_val < 0)
                dts_val = 0;

              if (m_dv_seek_first_dts == DVD_NOPTS_VALUE)
                m_dv_seek_first_dts = dts_val;

              if (dts_val < m_dv_seek_first_dts)
              {
                CLog::Log(LOGDEBUG,
                          "CDVDVideoCodecAmlogic::{} - skip orphan packet: "
                          "dts={:.3f} < first_dts={:.3f}",
                          __FUNCTION__, dts_val / DVD_TIME_BASE,
                          m_dv_seek_first_dts / DVD_TIME_BASE);
                return true;
              }
            }

            bool dual_layer_converted = false;

            if (!m_packages.empty())
            {
              DLDemuxPacket queued = m_packages.front();
              uint8_t *qData = std::get<0>(queued);
              uint32_t qSize = std::get<1>(queued);
              bool qIsEL = std::get<2>(queued);

              if (qIsEL != packet.isELPackage)
              {
                if (!packet.isELPackage)
                  dual_layer_converted = m_bitstream->Convert(pData, iSize, qData, qSize);
                else
                  dual_layer_converted = m_bitstream->Convert(qData, qSize, pData, iSize);
              }
            }

            if (dual_layer_converted)
            {
              KODI::MEMORY::AlignedFree(std::get<0>(m_packages.front()));
              m_packages.pop_front();
              /* First BL+EL pair succeeded - disable seek filter */
              m_dv_seek_filter_active = false;
            }
            else
            {
              uint8_t *pkt = static_cast<uint8_t*>(KODI::MEMORY::AlignedMalloc(packet.iSize + AV_INPUT_BUFFER_PADDING_SIZE, 16));
              memcpy(pkt, packet.pData, packet.iSize);
              m_packages.emplace_back(pkt, iSize, packet.isELPackage, packet.dts);
              return true;
            }
          }
        }
        else
        {
          if (!SingleLayerConvert(pData, iSize, packet))
            return true;
        }

        if (dual_layer_queued)
        {
          /* DualLayerAccumulate queued the data, no output buffer to send */
          pData = nullptr;
          iSize = 0;
        }
        else
        {
          m_last_pData = pData = m_bitstream->GetConvertBuffer();
          m_last_iSize = iSize = m_bitstream->GetConvertSize();
        }
        doviIsFEL = m_bitstream->GetDoviIsFEL();
        IsHdr10Plus = m_bitstream->GetIsHdrPlus();
        IsHdrVivid = m_bitstream->GetIsHdrVivid();

        // Resolve the multi HDR stream path. SEI stripping only applies
        // when converting via VS-Engine (hdr10plus2dv/cuva2dv/hdr2dv):
        // the DV engine needs a clean single-format stream. For native
        // processing (amvecm/VPP HDR2) all metadata passes through so the
        // kernel can detect and use every available format natively.
        hasDv = m_hints.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION;
        hdrPath = aml_get_hdr_path(
            hasDv,
            m_hints.hdr10Plus || IsHdr10Plus, m_hints.hdrVivid || IsHdrVivid, m_hints.hdrType);
        if (hdrPath.vs10)
        {
          m_bitstream->SetRemoveHdr10Plus(hdrPath.target != StreamHdrType::HDR_TYPE_HDR10PLUS);
          m_bitstream->SetRemoveCuva(hdrPath.target != StreamHdrType::HDR_TYPE_HDRVIVID);
        }
        else
        {
          m_bitstream->SetRemoveHdr10Plus(false);
          m_bitstream->SetRemoveCuva(false);
        }
      }
    }
    else if (!m_has_keyframe && m_bitparser)
    {
      if (!m_bitparser->CanStartDecode(pData, iSize))
      {
        CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::{}: waiting for keyframe (bitparser)", __FUNCTION__);
        return true;
      }
      else
        m_has_keyframe = true;
    }
    FrameRateTracking( pData, iSize, packet.dts, packet.pts);

    // Delay the decoder open until the first decodable data is available.
    // For dual-track (DT-DL) DV the early packets are queued for BL+EL
    // pairing (pData stays null), and doviIsFEL is only known once the
    // first pair's Convert has parsed the EL RPU; opening earlier would
    // configure a P7 FEL stream as MEL and drop the enhancement layer.
    if (!m_opened && pData && iSize > 0)
    {
      if (packet.m_seekTime != DVD_NOPTS_VALUE && packet.isDualStream &&
          m_resume_pair_count < 5 && pData && iSize > 0)
      {
        /* Accumulate 5 BL+EL Convert outputs before opening decoder.
         * Only active on dual-stream DV resume (seekTime + dual-track flag).
         * After 5 pairs, the valve opens: queue starts draining
         * one frame per AddData call to the decoder.
         * Single-track streams (incl. VS10 conversions like cuva2dv)
         * never queue: their Convert output goes straight to the decoder. */
        uint8_t *buf = static_cast<uint8_t*>(KODI::MEMORY::AlignedMalloc(iSize, 16));
        if (buf)
        {
          memcpy(buf, pData, iSize);
          m_resume_buffers.push_back({buf, iSize, packet.dts});
        }
        m_resume_pair_count++;
        CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::{}: accumulate pair {}/5 (resume)", __FUNCTION__, m_resume_pair_count);
        pData = nullptr;
        iSize = 0;
        m_last_added = true;
        return true;
      }

      if (packet.pts == DVD_NOPTS_VALUE)
        m_hints.ptsinvalid = true;

      m_processInfo.SetDoviIsFEL(doviIsFEL);
      // Report the real format set from the demuxer side data (first packet),
      // merged with the first-frame SEI detection. The codec-side detection
      // alone is unreliable here: once the filter strips a format it stops
      // being detected.
      m_processInfo.SetIsHdr10Plus(IsHdr10Plus || m_hints.hdr10Plus);
      m_processInfo.SetIsHdrVivid(IsHdrVivid || m_hints.hdrVivid);

      // Update hints hdrType so the decoder (AMLCodec::OpenDecoder)
      // sees the target HDR format for sysfs configuration. When a target
      // was selected by the path it wins, otherwise keep the demuxer hint
      // upgraded by the detected SEI (DV 8.4/8.6 BL is HDR10+ compatible).
      if (hdrPath.target != StreamHdrType::HDR_TYPE_NONE)
        m_hints.hdrType = hdrPath.target;
      else if (m_hints.hdrType != StreamHdrType::HDR_TYPE_DOLBYVISION)
      {
        if (IsHdrVivid || m_hints.hdrVivid)
          m_hints.hdrType = StreamHdrType::HDR_TYPE_HDRVIVID;
        else if (IsHdr10Plus || m_hints.hdr10Plus)
          m_hints.hdrType = StreamHdrType::HDR_TYPE_HDR10PLUS;
      }
      m_videobuffer.hdrType = m_hints.hdrType;

      // Configure the kernel HDR gate for the target format (DV led /
      // HDR10+ or CUVA passthrough). The kernel then performs automatic
      // downgrade for sinks that don't support the format. hasDv gates the
      // HDR10+ absorption so mixed DV sources never get converted.
      aml_set_hdr_gate(m_hints.hdrType, hasDv);

      // vs10 path: hand the clean single-format stream to DV so the decoder
      // enables the kernel DV / VS-Engine path (IPT colour space for tone
      // mapping, same hardware pipeline used for native DV content).
      if (hdrPath.vs10)
      {
        m_hints.hdrType = StreamHdrType::HDR_TYPE_DOLBYVISION;
        m_videobuffer.hdrType = m_hints.hdrType;

        // Re-arm the gate with the DV target so dolby_vision_enable is set
        // to "Y" and the dv_mode/IPM path is enabled. The gate was called
        // above with the unconverted hdrType (e.g. HDR10) which wrote
        // enable=0; without this re-arm the DV engine stays off.
        aml_set_hdr_gate(m_hints.hdrType, hasDv);
      }

      // Report the resolved output format (multi-HDR priority target, SEI
      // upgrade or vs10 conversion) so the video stream info shows what is
      // actually being output instead of the static demuxer metadata.
      m_processInfo.SetVideoHdrType(m_hints.hdrType);

      CLog::Log(LOGINFO, "CDVDVideoCodecAmlogic::{}: Open decoder: fps:{:d}/{:d}", __FUNCTION__, m_hints.fpsrate, m_hints.fpsscale);
      if (m_Codec && !m_Codec->OpenDecoder(m_hints, doviIsFEL))
        CLog::Log(LOGERROR, "CDVDVideoCodecAmlogic::{}: Failed to open Amlogic Codec", __FUNCTION__);

      m_videoBufferPool = std::shared_ptr<CAMLVideoBufferPool>(new CAMLVideoBufferPool());

      m_opened = true;
    }

    /* Drain resume queue: one frame per AddData call.
     * The queue is a one-time buffer for the initial resume warm-up.
     * Once empty, m_resume_pair_count = 0 and normal passthrough resumes.
     * Current Convert output goes directly to decoder via the normal path below. */
    if (m_resume_pair_count >= 5 && !m_resume_buffers.empty())
    {
      auto &front = m_resume_buffers.front();
      if (m_Codec->AddData(front.data, front.size, front.dts, DVD_NOPTS_VALUE))
      {
        KODI::MEMORY::AlignedFree(front.data);
        m_resume_buffers.pop_front();
        CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::{}: drain resume queue ({} left)", __FUNCTION__, m_resume_buffers.size());
        if (m_resume_buffers.empty())
        {
          m_resume_pair_count = 0;
          CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::{}: resume queue drained, normal passthrough", __FUNCTION__);
        }
      }
    }

    /* The resume queue is a one-time warm-up buffer: it fills with the
     * first 5 Convert outputs (accumulate branch above), after which the
     * current Convert output goes straight to the decoder and the queue
     * drains one frame per AddData/GetPicture call to zero. Replenishing
     * it (count > 0) would keep the queue alive forever and could stall
     * the decoder when one drain AddData fails (ring buffer full). */
    if (m_resume_pair_count < 5 && pData && iSize > 0)
    {
      uint8_t *buf = static_cast<uint8_t*>(KODI::MEMORY::AlignedMalloc(iSize, 16));
      if (buf)
      {
        memcpy(buf, pData, iSize);
        m_resume_buffers.push_back({buf, iSize, packet.dts});
      }
      pData = nullptr;
      iSize = 0;
      m_last_added = true;
    }
  }

  if (pData && iSize > 0)
  {
    if (packet.pSideData && packet.iSideDataElems > 0)
    {
      const AVPacketSideData* sideData = av_packet_side_data_get(static_cast<AVPacketSideData*>(packet.pSideData),
                                                                 packet.iSideDataElems,
                                                                 AV_PKT_DATA_DYNAMIC_HDR10_PLUS_RAW);

      if (sideData && sideData->size)
      {
        AMLLatchHdr10PlusT35(sideData->data, sideData->size, m_pendingMeta);
        if (m_Codec->AddHDR10PData(sideData->data, sideData->size) < 0)
          CLog::Log(LOGWARNING, "CDVDVideoCodecAmlogic::{}: failed to set hdr10p data with size {}", __FUNCTION__,
            sideData->size);
      }
    }

    double used_dts = (m_last_dts != DVD_NOPTS_VALUE) ? m_last_dts : packet.dts;
    m_last_dts = DVD_NOPTS_VALUE;
    m_last_added = m_Codec->AddData(pData, iSize, used_dts, m_hints.ptsinvalid ? DVD_NOPTS_VALUE : packet.pts);

    if (m_last_added && packet.pData)
    {
      m_pendingMeta.Inherit(m_lastMeta);
      m_lastMeta = m_pendingMeta;
      if (m_hints.ptsinvalid || packet.pts == DVD_NOPTS_VALUE)
        CAMLFrameMetadataStore::GetInstance().Publish(m_metadataToken, m_pendingMeta);
      else
      {
        m_metadataSequencer.Commit(packet.pts, m_pendingMeta);
        m_lastCommitPts = packet.pts;
      }
      m_pendingMeta = m_streamMeta;
    }
  }
  else
  {
    /* no data from DualLayerAccumulate/DualLayerTryPair (still queuing), return true to keep going */
    m_last_added = true;
  }

  return m_last_added;
}

// the latency the renderer adds when it schedules a frame for display,
// see CRenderManager::PrepareNextRender and UpdateLatencyTweak
double CDVDVideoCodecAmlogic::RenderDisplayLatency()
{
  const auto winSystem = CServiceBroker::GetWinSystem();
  CGraphicContext& gfx = winSystem->GetGfxContext();

  const bool isHDRUsed = winSystem->GetOSHDRStatus() == HDR_STATUS::HDR_ON &&
                         m_hints.hdrType != StreamHdrType::HDR_TYPE_NONE;
  float refresh = gfx.GetFPS();
  if (gfx.GetVideoResolution() == RES_WINDOW)
    refresh = 0;

  const double latencyTweak = static_cast<double>(
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->GetLatencyTweak(
          refresh, isHDRUsed, gfx.GetResInfo().iScreenHeight));
  const double videoDelay =
      static_cast<double>(m_processInfo.GetVideoSettings().m_AudioDelay) * 1000.0;

  return DVD_MSEC_TO_TIME(latencyTweak + static_cast<double>(gfx.GetDisplayLatency()) -
                          videoDelay -
                          static_cast<double>(winSystem->GetFrameLatencyAdjustment()));
}

// publishes every committed value whose frame the renderer has scheduled
// for display. A miss keeps the last published values
void CDVDVideoCodecAmlogic::DrainMetadataToClock()
{
  if (!m_hints.pClock || m_metadataSequencer.Empty())
    return;

  double target = m_hints.pClock->GetClock();
  if (!m_hints.pClock->IsPaused())
    target += RenderDisplayLatency();

  AMLFrameMetadata meta;
  if (m_metadataSequencer.Consume(target, meta))
  {
    CAMLFrameMetadataStore::GetInstance().Publish(m_metadataToken, meta);
    if (!m_metaLeadLogged)
    {
      m_metaLeadLogged = true;
      CLog::Log(LOGDEBUG, "{}: frame metadata pts lead {:.3f}", __MODULE_NAME__,
                (m_lastCommitPts - target) / DVD_TIME_BASE);
    }
  }
}

void CDVDVideoCodecAmlogic::Reset(void)
{
  m_Codec->Reset();

  ClearBitstreamCommon();

  m_mpeg2_sequence_pts = 0;
  m_has_keyframe = false;
  m_metadataSequencer.Reset();
  m_pendingMeta = m_streamMeta;
}

CDVDVideoCodec::VCReturn CDVDVideoCodecAmlogic::GetPicture(VideoPicture* pVideoPicture)
{
  DrainMetadataToClock();

  if (!m_Codec)
    return VC_ERROR;

  /* Drain resume queue: if the decoder has no frame and the queue has
   * accumulated data, push one frame to the decoder so it can decode. */
  if (m_resume_pair_count >= 5 && !m_resume_buffers.empty())
  {
    auto &front = m_resume_buffers.front();
    if (m_Codec->AddData(front.data, front.size, front.dts, DVD_NOPTS_VALUE))
    {
      KODI::MEMORY::AlignedFree(front.data);
      m_resume_buffers.pop_front();
      CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::{}: drain resume queue from GetPicture ({} left)", __FUNCTION__, m_resume_buffers.size());
      if (m_resume_buffers.empty())
        m_resume_pair_count = 0;
    }
  }

  VCReturn retVal = m_Codec->GetPicture(&m_videobuffer);

  if (retVal == VC_PICTURE)
  {
    if (pVideoPicture->videoBuffer)
      pVideoPicture->videoBuffer->Release();
    pVideoPicture->videoBuffer = nullptr;
    pVideoPicture->SetParams(m_videobuffer);

    pVideoPicture->videoBuffer = m_videoBufferPool->Get();
    static_cast<CAMLVideoBuffer*>(pVideoPicture->videoBuffer)->Set(this, m_Codec,
     m_Codec->GetOMXPts(), m_Codec->GetAmlDuration(), m_Codec->GetBufferIndex());;
  }

  // check for mpeg2 aspect ratio changes
  if (m_mpeg2_sequence && pVideoPicture->pts >= m_mpeg2_sequence_pts)
    m_aspect_ratio = m_mpeg2_sequence->ratio;

  // check for h264 aspect ratio changes
  if (m_h264_sequence && pVideoPicture->pts >= m_h264_sequence_pts)
    m_aspect_ratio = m_h264_sequence->ratio;

  pVideoPicture->iDisplayWidth  = pVideoPicture->iWidth;
  pVideoPicture->iDisplayHeight = pVideoPicture->iHeight;
  if (m_aspect_ratio > 1.0f && !m_hints.forced_aspect)
  {
    pVideoPicture->iDisplayWidth  = ((int)lrint(pVideoPicture->iHeight * m_aspect_ratio)) & ~3;
    if (pVideoPicture->iDisplayWidth > pVideoPicture->iWidth)
    {
      pVideoPicture->iDisplayWidth  = pVideoPicture->iWidth;
      pVideoPicture->iDisplayHeight = ((int)lrint(pVideoPicture->iWidth / m_aspect_ratio)) & ~3;
    }
  }

  return retVal;
}

void CDVDVideoCodecAmlogic::SetCodecControl(int flags)
{
  if (m_codecControlFlags != flags)
  {
    CLog::Log(LOGDEBUG, LOGVIDEO, "{} {:x}->{:x}",  __func__, m_codecControlFlags, flags);
    m_codecControlFlags = flags;

    if (flags & DVD_CODEC_CTRL_DROP)
      m_videobuffer.iFlags |= DVP_FLAG_DROPPED;
    else
      m_videobuffer.iFlags &= ~DVP_FLAG_DROPPED;

    if (m_Codec)
      m_Codec->SetDrain((flags & DVD_CODEC_CTRL_DRAIN) != 0);
  }
}

int CDVDVideoCodecAmlogic::GetDataLevel() const
{
  if (m_Codec)
  {
    int data_len, free_len, size;
    return static_cast<int>(m_Codec->GetBufferLevel(0, data_len, free_len, size));
  }

  return 0;
}

void CDVDVideoCodecAmlogic::SetSpeed(int iSpeed)
{
  if (m_Codec)
    m_Codec->SetSpeed(iSpeed);
}

void CDVDVideoCodecAmlogic::FrameRateTracking(uint8_t *pData, int iSize, double dts, double pts)
{
  // mpeg2 handling
  if (m_mpeg2_sequence)
  {
    // probe demux for sequence_header_code NAL and
    // decode aspect ratio and frame rate.
    if (CBitstreamConverter::mpeg2_sequence_header(pData, iSize, m_mpeg2_sequence) &&
       (m_mpeg2_sequence->fps_rate > 0) && (m_mpeg2_sequence->fps_scale > 0))
    {
      if (!m_mpeg2_sequence->fps_scale || !m_mpeg2_sequence->fps_scale)
        return;

      m_mpeg2_sequence_pts = pts;
      if (m_mpeg2_sequence_pts == DVD_NOPTS_VALUE)
        m_mpeg2_sequence_pts = dts;

      CLog::Log(LOGDEBUG, "{}::{} fps:{:d}/{:d} mpeg2_fps:{:d}/{:d} options:0x{:2x}", __MODULE_NAME__, __FUNCTION__,
              m_hints.fpsrate, m_hints.fpsscale, m_mpeg2_sequence->fps_rate, m_mpeg2_sequence->fps_scale, m_hints.codecOptions);
      if  (!(m_hints.codecOptions & CODEC_INTERLACED))
      {
        m_hints.fpsrate = m_mpeg2_sequence->fps_rate;
        m_hints.fpsscale = m_mpeg2_sequence->fps_scale;
      }
      if (m_hints.fpsrate && m_hints.fpsscale)
      {
        m_framerate = static_cast<float>(m_hints.fpsrate) / m_hints.fpsscale;
        if (m_hints.codecOptions & CODEC_UNKNOWN_I_P)
          if (std::abs(m_framerate - 25.0) < 0.02 || std::abs(m_framerate - 29.97) < 0.02)
          {
            m_framerate += m_framerate;
            m_hints.fpsrate += m_hints.fpsrate;
          }
        m_video_rate = (int)(0.5 + (96000.0 / m_framerate));
      }
      m_hints.width    = m_mpeg2_sequence->width;
      m_hints.height   = m_mpeg2_sequence->height;
      m_hints.aspect   = m_mpeg2_sequence->ratio;

      m_processInfo.SetVideoFps(m_framerate);
      m_processInfo.SetVideoDAR(m_hints.aspect);
    }
    return;
  }

  // h264 aspect ratio handling
  if (m_h264_sequence)
  {
    // probe demux for SPS NAL and decode aspect ratio
    if (CBitstreamConverter::h264_sequence_header(pData, iSize, m_h264_sequence))
    {
      m_h264_sequence_pts = pts;
      if (m_h264_sequence_pts == DVD_NOPTS_VALUE)
          m_h264_sequence_pts = dts;

      CLog::Log(LOGDEBUG, "{}: detected h264 aspect ratio({:f})",
        __MODULE_NAME__, m_h264_sequence->ratio);
      m_hints.width    = m_h264_sequence->width;
      m_hints.height   = m_h264_sequence->height;
      m_hints.aspect   = m_h264_sequence->ratio;
    }
  }
}
