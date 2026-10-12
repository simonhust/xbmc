/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AEBitstreamPacker.h"

#include "AEPackIEC61937.h"
#include "AEStreamInfo.h"
#include "utils/log.h"
#include "ServiceBroker.h"
#include "cores/AudioEngine/Interfaces/AE.h"

#include <array>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace
{
constexpr auto BURST_HEADER_SIZE = 8;
constexpr auto EAC3_MAX_BURST_PAYLOAD_SIZE = 24576 - BURST_HEADER_SIZE;
} // namespace

CAEBitstreamPacker::CAEBitstreamPacker()
{
  Reset();
}

CAEBitstreamPacker::~CAEBitstreamPacker()
{
}

void CAEBitstreamPacker::Pack(CAEStreamInfo &info, uint8_t* data, int size)
{
  m_pauseDuration = 0;
  switch (info.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      m_dataSize = CAEPackIEC61937::PackTrueHD(data + IEC61937_DATA_OFFSET,
                                               size - IEC61937_DATA_OFFSET, m_packedBuffer);
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      PackDTSHD (info, data, size);
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSUHD:
      /* Two very different inputs reach here. With "Convert DTS:X to DTS Core"
       * enabled the bytes come from the vendor dtsx_pt helper, which already
       * emits complete IEC 61937 frames (0x72F81F4E preamble, data-type 0x0B)
       * at the sink's burst period. Wrapping that again would double-encapsulate
       * it, so it is forwarded untouched.
       * Otherwise the bytes are the raw Profile 2 FTOC frame and still need the
       * DTS-UHD subtype applied here. */
      if (CServiceBroker::GetActiveAE()->ConvertsDtsXToDtsCore())
      {
        /* The helper's reply is already IEC 61937 framed and arrives in
         * chunks no larger than one sink buffer (see
         * CDVDAudioCodecPassthrough::GetData), so forward it as-is. */
        if (static_cast<unsigned int>(size) > MAX_IEC61937_PACKET)
        {
          CLog::Log(LOGERROR, "CAEBitstreamPacker::Pack - converted DTS:X frame too large");
          break;
        }
        memcpy(m_packedBuffer, data, size);
        m_dataSize = size;
      }
      else
      {
        PackDTSUHD (info, data, size);
      }
      break;

    case CAEStreamInfo::STREAM_TYPE_AV3A:
      /* IEC 61937-17: one data burst per AVS3-P3 frame, m_dtsPeriod carries the
       * frame length in samples (1024 or 512). */
      m_dataSize = CAEPackIEC61937::PackAV3A(data, size, m_packedBuffer, info.m_dtsPeriod);
      break;

    case CAEStreamInfo::STREAM_TYPE_AC3:
      m_dataSize = CAEPackIEC61937::PackAC3(data, size, m_packedBuffer);
      break;

    case CAEStreamInfo::STREAM_TYPE_EAC3:
      PackEAC3 (info, data, size);
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
      m_dataSize = CAEPackIEC61937::PackDTS_512(data, size, m_packedBuffer, info.m_dataIsLE);
      break;

    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
      m_dataSize = CAEPackIEC61937::PackDTS_1024(data, size, m_packedBuffer, info.m_dataIsLE);
      break;

    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
      m_dataSize = CAEPackIEC61937::PackDTS_2048(data, size, m_packedBuffer, info.m_dataIsLE);
      break;

    default:
      CLog::Log(LOGERROR, "CAEBitstreamPacker::Pack - no pack function");
  }
}

bool CAEBitstreamPacker::PackPause(CAEStreamInfo &info, unsigned int millis, bool iecBursts)
{
  // re-use last buffer
  if (m_pauseDuration == millis)
    return false;

  switch (info.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
    case CAEStreamInfo::STREAM_TYPE_EAC3:
    case CAEStreamInfo::STREAM_TYPE_AV3A:
      /* IEC 61937-17 table 2 recommends a pause data-burst repetition period of
       * four IEC 60958 frames. */
      m_dataSize = CAEPackIEC61937::PackPause(m_packedBuffer, millis, GetOutputChannelMap(info).Count() * 2, GetOutputRate(info), 4, info.m_sampleRate);
      m_pauseDuration = millis;
      break;

    case CAEStreamInfo::STREAM_TYPE_AC3:
    case CAEStreamInfo::STREAM_TYPE_DTSHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
      m_dataSize = CAEPackIEC61937::PackPause(m_packedBuffer, millis, GetOutputChannelMap(info).Count() * 2, GetOutputRate(info), 3, info.m_sampleRate);
      m_pauseDuration = millis;
      break;

    default:
      CLog::Log(LOGERROR, "CAEBitstreamPacker::Pack - no pack function");
  }

  if (!iecBursts)
  {
    memset(m_packedBuffer, 0, m_dataSize);
  }

  return true;
}

unsigned int CAEBitstreamPacker::GetSize() const
{
  return m_dataSize;
}

uint8_t* CAEBitstreamPacker::GetBuffer()
{
  return m_packedBuffer;
}

void CAEBitstreamPacker::Reset()
{
  m_dataSize = 0;
  m_pauseDuration = 0;
  m_packedBuffer[0] = 0;
}

void CAEBitstreamPacker::PackDTSHD(CAEStreamInfo &info, uint8_t* data, int size)
{
  static const uint8_t dtshd_start_code[10] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xfe };
  unsigned int dataSize = sizeof(dtshd_start_code) + 2 + size;

  if (dataSize > m_dtsHDSize)
  {
    m_dtsHDSize = dataSize;
    m_dtsHD.resize(dataSize);
    memcpy(m_dtsHD.data(), dtshd_start_code, sizeof(dtshd_start_code));
  }

  m_dtsHD[sizeof(dtshd_start_code) + 0] = ((uint16_t)size & 0xFF00) >> 8;
  m_dtsHD[sizeof(dtshd_start_code) + 1] = ((uint16_t)size & 0x00FF);
  memcpy(m_dtsHD.data() + sizeof(dtshd_start_code) + 2, data, size);

  m_dataSize =
      CAEPackIEC61937::PackDTSHD(m_dtsHD.data(), dataSize, m_packedBuffer, info.m_dtsPeriod);
}

void CAEBitstreamPacker::PackDTSUHD(CAEStreamInfo &info, uint8_t* data, int size)
{
  /* DTS:X Profile 2. IEC 61937-5 carries it as data-type 0x11 with the
   * DTS-UHD subtype (Pc[11]~Pc[12] = 0x8), which is what the Amlogic HDMI TX
   * path expects (AUD_CODEC_TYPE_DTS_UHD). No start code is prepended: the
   * burst payload is the raw DTS:X frame as it appears in the container. */
  m_dataSize = CAEPackIEC61937::PackDTSUHD(data, size, m_packedBuffer, info.m_dtsPeriod);
}

void CAEBitstreamPacker::PackEAC3(CAEStreamInfo &info, uint8_t* data, int size)
{
  unsigned int framesPerBurst = info.m_repeat;

  if (m_eac3FramesPerBurst != framesPerBurst)
  {
    /* switched streams, discard partial burst */
    m_eac3Size = 0;
    m_eac3FramesPerBurst = framesPerBurst;
  }

  if (m_eac3FramesPerBurst == 1)
  {
    /* simple case, just pass through */
    m_dataSize = CAEPackIEC61937::PackEAC3(data, size, m_packedBuffer);
  }
  else
  {
    /* multiple frames needed to achieve 6 blocks as required by IEC 61937-3:2007 */

    if (m_eac3.empty())
      m_eac3.resize(EAC3_MAX_BURST_PAYLOAD_SIZE);

    unsigned int newsize = m_eac3Size + size;
    bool overrun = newsize > EAC3_MAX_BURST_PAYLOAD_SIZE;

    if (!overrun)
    {
      memcpy(m_eac3.data() + m_eac3Size, data, size);
      m_eac3Size = newsize;
      m_eac3FramesCount++;
    }

    if (m_eac3FramesCount >= m_eac3FramesPerBurst || overrun)
    {
      m_dataSize = CAEPackIEC61937::PackEAC3(m_eac3.data(), m_eac3Size, m_packedBuffer);
      m_eac3Size = 0;
      m_eac3FramesCount = 0;
    }
  }
}

unsigned int CAEBitstreamPacker::GetOutputRate(const CAEStreamInfo& info)
{
  unsigned int rate;
  switch (info.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_AC3:
      rate = info.m_sampleRate;
      break;
    case CAEStreamInfo::STREAM_TYPE_EAC3:
      rate = info.m_sampleRate * 4;
      break;
    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      if (info.m_sampleRate == 48000 ||
          info.m_sampleRate == 96000 ||
          info.m_sampleRate == 192000)
        rate = 192000;
      else
        rate = 176400;
      break;
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      rate = info.m_sampleRate;
      break;
    case CAEStreamInfo::STREAM_TYPE_DTSHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      rate = 192000;
      break;
    case CAEStreamInfo::STREAM_TYPE_DTSUHD:
      rate = 192000;
      break;
    case CAEStreamInfo::STREAM_TYPE_AV3A:
      /* AV3A carries four bytes per sample (one frame per burst), so the IEC 60958
       * frame rate equals the audio sample rate. */
      rate = info.m_sampleRate;
      break;
    default:
      rate = 48000;
      break;
  }
  return rate;
}

CAEChannelInfo CAEBitstreamPacker::GetOutputChannelMap(const CAEStreamInfo& info)
{
  int channels = 2;
  switch (info.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_AC3:
    case CAEStreamInfo::STREAM_TYPE_EAC3:
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
    case CAEStreamInfo::STREAM_TYPE_DTSHD:
    case CAEStreamInfo::STREAM_TYPE_DTSUHD:
    case CAEStreamInfo::STREAM_TYPE_AV3A:
      channels = 2;
      break;

    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      channels = 8;
      break;

    default:
      break;
  }

  CAEChannelInfo channelMap;
  for (int i=0; i<channels; i++)
  {
    channelMap += AE_CH_RAW;
  }

  return channelMap;
}
