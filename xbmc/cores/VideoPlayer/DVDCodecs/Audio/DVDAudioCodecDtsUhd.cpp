#include "DVDAudioCodecDtsUhd.h"

#include "DVDCodecs/DVDCodecs.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "cores/AudioEngine/Utils/AEChannelInfo.h"
#include "utils/log.h"

#include <dts_uhd/dts_uhd_wrapper.h>

static constexpr int DTS_UHD_FRAMELEN = 1024;
static constexpr int DTS_UHD_MAX_FRAMES = 32;

static CAEChannelInfo GetDtsUhdChannelLayout(int channels)
{
  switch (channels)
  {
    case 1:
      return CAEChannelInfo(AE_CH_LAYOUT_1_0);
    case 2:
      return CAEChannelInfo(AE_CH_LAYOUT_2_0);
    case 6:
      return CAEChannelInfo(AE_CH_LAYOUT_5_1);
    case 8:
      return CAEChannelInfo(AE_CH_LAYOUT_7_1);
    default:
      if (channels > 8)
      {
        CAEChannelInfo info(AE_CH_LAYOUT_7_1);
        return info;
      }
      return CAEChannelInfo(AE_CH_LAYOUT_5_1);
  }
}

CDVDAudioCodecDtsUhd::CDVDAudioCodecDtsUhd(CProcessInfo& processInfo)
  : CDVDAudioCodec(processInfo)
{
}

CDVDAudioCodecDtsUhd::~CDVDAudioCodecDtsUhd()
{
  Dispose();
}

std::unique_ptr<CDVDAudioCodec> CDVDAudioCodecDtsUhd::Create(CProcessInfo& processInfo)
{
  return std::make_unique<CDVDAudioCodecDtsUhd>(processInfo);
}

bool CDVDAudioCodecDtsUhd::Register()
{
  CDVDFactoryCodec::RegisterHWAudioCodec("dts_uhd", &CDVDAudioCodecDtsUhd::Create);
  return true;
}

bool CDVDAudioCodecDtsUhd::Open(CDVDStreamInfo& hints, CDVDCodecOptions& options)
{
  if (hints.codec != AV_CODEC_ID_DTS_UHD)
    return false;

  m_ctx = dts_uhd_decoder_init();
  if (!m_ctx)
  {
    CLog::Log(LOGERROR, "CDVDAudioCodecDtsUhd::Open() failed to initialize decoder");
    return false;
  }

  m_format.m_sampleRate = hints.samplerate > 0 ? hints.samplerate : 48000;
  m_format.m_channelLayout = GetDtsUhdChannelLayout(6);
  m_format.m_dataFormat = AE_FMT_S16NE;
  m_format.m_frameSize = m_format.m_channelLayout.Count() * 2;
  m_format.m_frameLength = DTS_UHD_FRAMELEN;

  m_codecName = "dts_uhd";
  m_hasOutput = false;

  CLog::Log(LOGINFO, "CDVDAudioCodecDtsUhd::Open() opened DTS-UHD decoder: {}Hz {}ch",
            m_format.m_sampleRate, m_format.m_channelLayout.Count());

  return true;
}

void CDVDAudioCodecDtsUhd::Dispose()
{
  if (m_ctx)
  {
    dts_uhd_decoder_close(m_ctx);
    m_ctx = nullptr;
  }
  m_outputBuffer.clear();
  m_hasOutput = false;
}

bool CDVDAudioCodecDtsUhd::AddData(const DemuxPacket& packet)
{
  if (!m_ctx || !packet.pData || packet.iSize <= 0)
    return false;

  int channels_out = 0;
  int sample_rate_out = 0;

  m_outputBuffer.resize(static_cast<size_t>(DTS_UHD_MAX_FRAMES) * DTS_UHD_FRAMELEN * 8);

  int frames = dts_uhd_decoder_decode(
      m_ctx, packet.pData, packet.iSize,
      m_outputBuffer.data(), DTS_UHD_MAX_FRAMES,
      &channels_out, &sample_rate_out);

  if (frames < 0)
  {
    CLog::Log(LOGDEBUG, "CDVDAudioCodecDtsUhd::AddData decode failed");
    m_hasOutput = false;
    return false;
  }

  if (frames > 0 && channels_out > 0)
  {
    m_outputBuffer.resize(static_cast<size_t>(frames) * DTS_UHD_FRAMELEN * channels_out);

    if (sample_rate_out > 0 && sample_rate_out != m_format.m_sampleRate)
      m_format.m_sampleRate = sample_rate_out;

    if (channels_out != m_format.m_channelLayout.Count())
    {
      m_format.m_channelLayout = GetDtsUhdChannelLayout(channels_out);
      m_format.m_frameSize = m_format.m_channelLayout.Count() * 2;
    }

    m_hasOutput = true;
  }
  else
  {
    m_hasOutput = false;
  }

  return true;
}

void CDVDAudioCodecDtsUhd::GetData(DVDAudioFrame& frame)
{
  if (!m_hasOutput || m_outputBuffer.empty())
  {
    frame.nb_frames = 0;
    frame.hasTimestamp = false;
    return;
  }

  frame.data[0] = reinterpret_cast<uint8_t*>(m_outputBuffer.data());
  frame.nb_frames = static_cast<unsigned int>(m_outputBuffer.size() / m_format.m_channelLayout.Count());
  frame.framesize = m_format.m_frameSize;
  frame.planes = 1;
  frame.format = m_format;
  frame.bits_per_sample = 16;
  frame.passthrough = false;
  frame.hasTimestamp = false;
  frame.hasDownmix = false;

  m_hasOutput = false;
}

void CDVDAudioCodecDtsUhd::Reset()
{
  Dispose();
  m_ctx = dts_uhd_decoder_init();
  m_hasOutput = false;
}
