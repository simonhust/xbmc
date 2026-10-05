#include "DVDAudioCodecAvs3a.h"

#include "DVDCodecs/DVDCodecs.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "cores/AudioEngine/Utils/AEChannelInfo.h"
#include "utils/log.h"

#include <avs3a/avs3a_wrapper.h>

/* Extra headroom (samples) for the decode output buffer */
#define AVS3A_FRAME_SLACK 4096

/* AVS3-P3 height-channel downmix is done inside the decoder wrapper:
   the last 4 channels (TFL/TFR[/TBL/TBR]) are dropped, so the PCM we
   receive is always bed-only. Map a coded channel count to bed channels. */
static int Av3aBedChannels(int codedChannels)
{
  if (codedChannels == 10 || codedChannels == 12)
    return codedChannels - 4;
  if (codedChannels > 0)
    return codedChannels;
  return 6;
}

static CAEChannelInfo GetAv3aChannelLayout(int channels)
{
  switch (channels)
  {
    case 1:
      return CAEChannelInfo(AE_CH_LAYOUT_1_0);
    case 2:
      return CAEChannelInfo(AE_CH_LAYOUT_2_0);
    case 4:
      return CAEChannelInfo(AE_CH_LAYOUT_4_0);
    case 6:
      return CAEChannelInfo(AE_CH_LAYOUT_5_1);
    case 8:
    default:
      return CAEChannelInfo(AE_CH_LAYOUT_7_1);
  }
}

CDVDAudioCodecAvs3a::CDVDAudioCodecAvs3a(CProcessInfo& processInfo)
  : CDVDAudioCodec(processInfo)
{
}

CDVDAudioCodecAvs3a::~CDVDAudioCodecAvs3a()
{
  Dispose();
}

std::unique_ptr<CDVDAudioCodec> CDVDAudioCodecAvs3a::Create(CProcessInfo& processInfo)
{
  return std::make_unique<CDVDAudioCodecAvs3a>(processInfo);
}

bool CDVDAudioCodecAvs3a::Register()
{
  CDVDFactoryCodec::RegisterHWAudioCodec("avs3a_dec", &CDVDAudioCodecAvs3a::Create);
  return true;
}

bool CDVDAudioCodecAvs3a::Open(CDVDStreamInfo& hints, CDVDCodecOptions& options)
{
  if (hints.codec != AV_CODEC_ID_AVS3_AUDIO)
    return false;

  const char* model_path = "/usr/lib/avs3a/model.bin";
  m_ctx = avs3a_decoder_init(model_path);
  if (!m_ctx)
  {
    CLog::Log(LOGERROR, "CDVDAudioCodecAvs3a::Open() failed to initialize decoder");
    return false;
  }

  /* The container/ffmpeg layer already reports the coded channel count
   * (10 = 5.1.4, 12 = 7.1.4) and the sample rate. The decoder wrapper
   * drops the 4 height channels, so the PCM we hand over is always
   * bed-only (6 or 8 channels). */
  const int codedChannels = hints.channels;
  const int bedChannels = Av3aBedChannels(codedChannels);

  m_format.m_sampleRate = hints.samplerate > 0 ? hints.samplerate : 48000;
  m_format.m_channelLayout = GetAv3aChannelLayout(bedChannels);
  m_format.m_dataFormat = AE_FMT_S16NE;
  m_format.m_frameSize = m_format.m_channelLayout.Count() * 2;
  m_format.m_frameLength = 1024;
  m_samplesPerFrame = m_format.m_frameLength * bedChannels;

  m_codecName = "avs3a";
  m_hasOutput = false;

  CLog::Log(LOGINFO, "CDVDAudioCodecAvs3a::Open() opened AVS3A decoder: {}Hz {}ch (coded {})",
            m_format.m_sampleRate, m_format.m_channelLayout.Count(), codedChannels);

  return true;
}

void CDVDAudioCodecAvs3a::Dispose()
{
  if (m_ctx)
  {
    avs3a_decoder_close(m_ctx);
    m_ctx = nullptr;
  }
  m_outputBuffer.clear();
  m_hasOutput = false;
}

bool CDVDAudioCodecAvs3a::AddData(const DemuxPacket& packet)
{
  if (!m_ctx || !packet.pData || packet.iSize <= 0)
    return false;

  int num_samples = 0;
  int sample_rate_out = 0, channels_out = 0;

  /* The wrapper decodes one AVS frame per call and writes
   * m_samplesPerFrame bed-channel samples; keep some headroom so a
   * slightly larger frame can never overrun the buffer. */
  m_outputBuffer.resize(m_samplesPerFrame + packet.iSize + AVS3A_FRAME_SLACK);

  int ret = avs3a_decoder_decode(
      m_ctx, packet.pData, packet.iSize,
      m_outputBuffer.data(), static_cast<int>(m_outputBuffer.size()),
      &num_samples, &sample_rate_out, &channels_out);

  if (ret != 0)
  {
    CLog::Log(LOGDEBUG, "CDVDAudioCodecAvs3a::AddData decode failed");
    m_hasOutput = false;
    return false;
  }

  if (num_samples > 0)
  {
    m_outputBuffer.resize(num_samples);

    if (sample_rate_out > 0 && sample_rate_out != m_format.m_sampleRate)
    {
      m_format.m_sampleRate = sample_rate_out;
    }
    if (channels_out > 0 && channels_out != m_format.m_channelLayout.Count())
    {
      m_format.m_channelLayout = GetAv3aChannelLayout(channels_out);
      m_format.m_frameSize = m_format.m_channelLayout.Count() * 2;
      m_samplesPerFrame = m_format.m_frameLength * channels_out;
    }

    m_hasOutput = true;
  }
  else
  {
    m_hasOutput = false;
  }

  return true;
}

void CDVDAudioCodecAvs3a::GetData(DVDAudioFrame& frame)
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

void CDVDAudioCodecAvs3a::Reset()
{
  Dispose();

  const char* model_path = "/usr/lib/avs3a/model.bin";
  m_ctx = avs3a_decoder_init(model_path);
  if (m_ctx)
  {
    /* The decoder is not initialised until the first frame arrives, so
     * keep the channel/frame count derived from the stream hints. */
    m_samplesPerFrame = m_format.m_frameLength *
                        m_format.m_channelLayout.Count();
  }
  m_hasOutput = false;
}
