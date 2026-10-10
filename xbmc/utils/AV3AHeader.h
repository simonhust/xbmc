/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <stdint.h>

/* Audio Vivid (AVS3-P3) AATF frame parsing, see T/UWA 009.1 annex A.
 *
 * The field layout and the tables below follow the AATF frame header written by
 * the UWA packaging tools: a 12 bit sync word, the codec id, the coding profile
 * and the sampling frequency index, followed by the profile specific channel
 * configuration and the bitrate index. A frame always carries 1024 audio
 * samples; its length in bytes is not transmitted but derived from the bitrate
 * of the channel configuration it uses. */
namespace AV3A
{
// Frame sync word, twelve bits wide.
constexpr unsigned int SYNC_WORD = 0xFFF;
// Audio samples in one frame.
constexpr unsigned int SAMPLES_PER_FRAME = 1024;
// A frame header never exceeds this many bytes.
constexpr unsigned int MAX_HEADER_BYTES = 9;

struct FrameHeader
{
  bool valid = false;
  // 1: lossless audio (codec string av3a.01), 2: general audio (av3a.02).
  unsigned int codecId = 0;
  // 0: channel based, 1: objects, 2: HOA.
  unsigned int profile = 0;
  unsigned int sampleRate = 0;
  // 0 when the header does not define it, e.g. for object only content.
  unsigned int channels = 0;
  // Frame length in bytes, 0 when it cannot be derived from the header.
  unsigned int frameBytes = 0;
};

namespace detail
{
// Table A.9 of T/UWA 009.1; identical in the UWA packager's FFmpeg
// (avs3_samplingrate_table) and in the reference decoder (attf_head.c).
constexpr unsigned int SAMPLING_RATES[9] = {192000, 96000, 48000, 44100, 32000, 24000,
                                            22050,  16000, 8000};

constexpr int64_t BITRATE_MONO[16] = {16000,  32000,  44000,  56000,  64000,  72000,
                                      80000,  96000,  128000, 144000, 164000, 192000,
                                      0,      0,      0,      0};
constexpr int64_t BITRATE_STEREO[16] = {24000,  32000,  48000,  64000,  80000,  96000,
                                        128000, 144000, 192000, 256000, 320000, 0,
                                        0,      0,      0,      0};
constexpr int64_t BITRATE_MC_5_1[16] = {192000, 256000, 320000, 384000, 448000, 512000,
                                        640000, 720000, 144000, 96000,  128000, 160000,
                                        0,      0,      0,      0};
constexpr int64_t BITRATE_MC_7_1[16] = {192000, 480000, 256000, 384000, 576000, 640000,
                                        128000, 160000, 0,      0,      0,      0,
                                        0,      0,      0,      0};
// No bitrate table is defined for 10.2 and 22.2.
constexpr int64_t BITRATE_MC_4_0[16] = {48000, 96000, 128000, 192000, 256000, 0, 0, 0,
                                        0,     0,     0,      0,      0,      0, 0, 0};
constexpr int64_t BITRATE_MC_5_1_2[16] = {152000, 320000, 480000, 576000, 0, 0, 0, 0,
                                          0,      0,      0,      0,      0, 0, 0, 0};
constexpr int64_t BITRATE_MC_5_1_4[16] = {176000, 384000, 576000, 704000, 256000, 448000,
                                          0,      0,      0,      0,      0,      0,
                                          0,      0,      0,      0};
constexpr int64_t BITRATE_MC_7_1_2[16] = {216000, 480000, 576000, 384000, 768000, 0, 0, 0,
                                          0,      0,      0,      0,      0,      0, 0, 0};
constexpr int64_t BITRATE_MC_7_1_4[16] = {240000, 608000, 384000, 512000, 832000, 0, 0, 0,
                                          0,      0,      0,      0,      0,      0, 0, 0};
constexpr int64_t BITRATE_HOA_1[16] = {48000, 96000, 128000, 192000, 256000, 0, 0, 0,
                                       0,     0,     0,      0,      0,      0, 0, 0};
constexpr int64_t BITRATE_HOA_2[16] = {192000, 256000, 320000, 384000, 480000, 512000, 640000, 0,
                                       0,      0,      0,      0,      0,      0,      0,      0};
constexpr int64_t BITRATE_HOA_3[16] = {256000, 320000, 384000, 512000, 640000, 896000, 0, 0,
                                       0,      0,      0,      0,      0,      0,      0, 0};

/* Channel configurations, mirroring the enumeration the decoder library uses
   (avs3_stat_com.h). */
struct ChannelConfig
{
  unsigned int channels;
  const int64_t* bitrate;
};

constexpr ChannelConfig CHANNEL_CONFIGS[14] = {
    {1, BITRATE_MONO},     // mono
    {2, BITRATE_STEREO},   // stereo
    {6, BITRATE_MC_5_1},   // 5.1
    {8, BITRATE_MC_7_1},   // 7.1
    {12, nullptr},         // 10.2
    {24, nullptr},         // 22.2
    {4, BITRATE_MC_4_0},   // 4.0
    {8, BITRATE_MC_5_1_2}, // 5.1.2
    {10, BITRATE_MC_5_1_4},// 5.1.4
    {10, BITRATE_MC_7_1_2},// 7.1.2
    {12, BITRATE_MC_7_1_4},// 7.1.4
    {4, BITRATE_HOA_1},    // HOA order 1
    {9, BITRATE_HOA_2},    // HOA order 2
    {16, BITRATE_HOA_3},   // HOA order 3
};

constexpr unsigned int CHANNEL_CONFIG_COUNT = 14;

/* The sampling frequency index codes one of the nine rates above; index 15 is
   reserved in aatf_frame() to carry an explicit 24 bit rate instead
   (T/UWA 009.2 section 5.2.2.2 and 8.3.5). The standard does not enumerate
   what an explicit rate may be, so it is accepted only when the rest of the
   pipeline can actually carry it: the rates of the table above, plus the ones
   the HDMI audio InfoFrame sample rate field can encode (CAESinkALSA
   GetAESParams). An arbitrary value here is a misparse, and a wrong rate
   reaches the sink as a stream parameter, where it drives the frame duration,
   the buffer timing and the AES3 sample rate field. */
inline bool IsSupportedSampleRate(unsigned int sampleRate)
{
  static constexpr unsigned int rates[] = {8000,   16000,  22050,  24000,  32000, 44100,
                                           48000,  88200,  96000,  176400, 192000};
  for (const unsigned int rate : rates)
  {
    if (rate == sampleRate)
      return true;
  }
  return false;
}

inline unsigned int FrameBytesForBitrate(uint64_t bitrate, unsigned int sampleRate)
{
  if (!bitrate || !sampleRate)
    return 0;
  // ceil(bitrate / sampleRate * SAMPLES_PER_FRAME / 8)
  return static_cast<unsigned int>((bitrate * SAMPLES_PER_FRAME + sampleRate * 8ULL - 1) /
                                   (sampleRate * 8ULL));
}
} // namespace detail

/* A frame starts with a 12 bit sync word; anything else is not a frame. The
   codec id (bits 12-15) and, for general audio, the coding profile (bits 20-22)
   are checked too so that payload data with a coincidental 0xFFF pattern is
   not mistaken for a frame start. Lossless audio (codec id 1) uses a different
   layout, so it is only gated on the codec id here and validated in full by
   ParseFrameHeader. */
inline bool HasSyncWord(const uint8_t* data, unsigned int size)
{
  if (size < 3 || data[0] != 0xFF || (data[1] & 0xF0) != 0xF0)
    return false;
  const unsigned int codecId = data[1] & 0x0F;
  if (codecId == 1)
    return true;
  if (codecId != 2)
    return false;
  const unsigned int profile = (data[2] >> 1) & 0x07;
  return profile <= 2;
}

/* Parses the frame header, which needs at least MAX_HEADER_BYTES bytes. The
   returned header is only valid when it could be parsed, which makes it usable
   to tell a false sync word from a real frame start. */
inline FrameHeader ParseFrameHeader(const uint8_t* data, unsigned int size)
{
  FrameHeader header;
  if (size < MAX_HEADER_BYTES)
    return header;

  unsigned int bitPos = 0;
  const auto readBits = [&](unsigned int count) {
    unsigned int value = 0;
    while (count--)
    {
      value = (value << 1) | ((data[bitPos >> 3] >> (7 - (bitPos & 7))) & 1);
      bitPos++;
    }
    return value;
  };

  if (readBits(12) != SYNC_WORD)
    return header;

  header.codecId = readBits(4);

  if (header.codecId == 2)
  {
    readBits(1); // ancillary data
    readBits(3); // neural network type
    header.profile = readBits(3);
    const unsigned int sampleRateIndex = readBits(4);
    readBits(8); // CRC, first part

    if (header.profile > 2 || sampleRateIndex >= 9)
      return header;

    header.sampleRate = detail::SAMPLING_RATES[sampleRateIndex];

    unsigned int channelConfig = detail::CHANNEL_CONFIG_COUNT;
    uint64_t bitrate = 0;

    if (header.profile == 0)
    {
      channelConfig = readBits(7);
      if (channelConfig >= detail::CHANNEL_CONFIG_COUNT)
        return header;
      header.channels = detail::CHANNEL_CONFIGS[channelConfig].channels;
    }
    else if (header.profile == 1)
    {
      const unsigned int soundBedType = readBits(2);
      if (soundBedType == 0)
      {
        const unsigned int objects = readBits(7) + 1;
        const unsigned int bitrateIndex = readBits(4);
        bitrate = detail::BITRATE_MONO[bitrateIndex] * objects;
        header.channels = objects;
      }
      else
      {
        channelConfig = readBits(7);
        const unsigned int bedBitrateIndex = readBits(4);
        const unsigned int objects = readBits(7) + 1;
        const unsigned int objectBitrateIndex = readBits(4);
        if (channelConfig >= detail::CHANNEL_CONFIG_COUNT)
          return header;
        header.channels = detail::CHANNEL_CONFIGS[channelConfig].channels + objects;
        const int64_t* bedBitrate = detail::CHANNEL_CONFIGS[channelConfig].bitrate;
        bitrate = (bedBitrate ? bedBitrate[bedBitrateIndex] : 0) +
                  detail::BITRATE_MONO[objectBitrateIndex] * objects;
      }
    }
    else
    {
      const unsigned int hoaOrder = readBits(4) + 1;
      if (hoaOrder > 3)
        return header;
      channelConfig = 10 + hoaOrder; // HOA order 1 to 3 are the last three entries
      header.channels = detail::CHANNEL_CONFIGS[channelConfig].channels;
    }

    readBits(2); // bit depth

    if (header.profile != 1)
    {
      const unsigned int bitrateIndex = readBits(4);
      const int64_t* table = channelConfig < detail::CHANNEL_CONFIG_COUNT
                                 ? detail::CHANNEL_CONFIGS[channelConfig].bitrate
                                 : nullptr;
      bitrate = table ? table[bitrateIndex] : 0;
    }
    readBits(8); // CRC, second part

    header.frameBytes = detail::FrameBytesForBitrate(bitrate, header.sampleRate);
    header.valid = true;
    return header;
  }

  // Lossless audio (codec id 1, av3a.01). Its frame head differs from the
  // general layout: there is no nn_type and no bitrate index, and the channel
  // count is coded directly instead of through a channel configuration
  // (T/UWA 009.2 section 5.2.2.2, the decoder specific config that mirrors
  // aatf_frame() for codec id 1).
  if (header.codecId != 1 || header.profile > 2)
    return header;

  const unsigned int sampleRateIndex = readBits(4);
  if (sampleRateIndex == 0x0f)
  {
    // Index 15 carries an explicit 24 bit rate instead of a table entry.
    const unsigned int explicitRate = readBits(24);
    if (!detail::IsSupportedSampleRate(explicitRate))
      return header;
    header.sampleRate = explicitRate;
  }
  else
  {
    // 9 to 14 are unassigned; only lossless reserves 15 for an explicit rate.
    if (sampleRateIndex >= 9)
      return header;
    header.sampleRate = detail::SAMPLING_RATES[sampleRateIndex];
  }

  readBits(1); // ancillary data
  header.profile = readBits(3);
  readBits(4); // reserved
  const unsigned int channelNumber = readBits(8);
  readBits(2); // bit depth
  const unsigned int additionInfoLength = readBits(8);
  // The frame length is not transmitted, and the addition info it points at is
  // the only payload length the head states. Without it there is nothing to
  // resynchronise on, so report the header without a frame size and let the
  // caller resynchronise the way SyncAC3 does for TrueHD.
  if (bitPos + additionInfoLength * 8 > size * 8)
    return header;
  readBits(additionInfoLength * 8);

  header.channels = channelNumber;
  header.frameBytes = 0;
  header.valid = channelNumber != 0;
  return header;
}
} // namespace AV3A
