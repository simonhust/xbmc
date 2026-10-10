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
// Table A.9 of T/UWA 009.1-2023 defines only 0x0 to 0x4 (192000, 96000,
// 48000, 44100, 32000) and reserves 0x5 to 0xf. The reference decoder
// (attf_head.c) and the UWA packager's FFmpeg (avs3_samplingrate_table)
// both extend it with 24000, 22050, 16000 and 8000, which is what encoders
// in the field emit, so the extended table is used here. Index 15 is the
// escape for an explicit rate and is handled by the caller.
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

inline unsigned int FrameBytesForBitrate(uint64_t bitrate, unsigned int sampleRate)
{
  if (!bitrate || !sampleRate)
    return 0;
  // ceil(bitrate / sampleRate * SAMPLES_PER_FRAME / 8)
  return static_cast<unsigned int>((bitrate * SAMPLES_PER_FRAME + sampleRate * 8ULL - 1) /
                                   (sampleRate * 8ULL));
}
} // namespace detail

/* Whether an AVS3-P3 stream at this rate can be carried by the IEC 61937
   interface. Clause 5.3.1 and table 3 of IEC 61937-17:2025 list exactly five
   rates: "AVS3-P3 supports sample rates of 32 kHz, 44.1 kHz, 48 kHz, 96 kHz, and
   192 kHz." They are the first five entries of the sampling frequency table,
   indices 0x0 to 0x4.
 *
   An AVS3-P3 encoder can also produce 24 kHz, 22.05 kHz, 16 kHz and 8 kHz, and
   those decode and play over PCM, but they cannot be passed on over HDMI, so a
   stream at one of those rates must not be offered for passthrough. */
inline bool IsInterfaceSampleRate(unsigned int sampleRate)
{
  switch (sampleRate)
  {
    case 32000:
    case 44100:
    case 48000:
    case 96000:
    case 192000:
      return true;
    default:
      return false;
  }
}

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

  // Lossless audio, codec id 1 (av3a.01). Tables A.4 and A.6 of T/UWA 009.1-2023:
  //
  //   aatf_frame()          syncword 12, audio_codec_id 4, anc_data_index 1
  //   aatf_frame_header()   coding_profile 3, sampling_frequency_index 4,
  //                         sampling_frequency 24 when the index is 15,
  //                         raw_frame_length 16, aatf_error_check() 8,
  //                         channel_number {4; 8}, resolution 2
  //
  // So unlike general audio it states its own frame length and has no nn_type
  // and no bitrate index.
  if (header.codecId != 1)
    return header;

  readBits(1); // ancillary data
  header.profile = readBits(3);
  if (header.profile > 2)
    return header;

  const unsigned int sampleRateIndex = readBits(4);
  if (sampleRateIndex == 0x0f)
  {
    // Index 15 carries an explicit 24 bit rate instead of a table entry. The
    // standard allows any value up to 16777214 Hz there, which rules nothing
    // out, so bound it to what an encoder can produce: a rate outside that is a
    // misparse. Whether the rate can also be passed on is a separate question,
    // see IsInterfaceSampleRate.
    const unsigned int explicitRate = readBits(24);
    if (explicitRate < 8000 || explicitRate > 192000)
      return header;
    header.sampleRate = explicitRate;
  }
  else
  {
    if (sampleRateIndex >= 9)
      return header;
    header.sampleRate = detail::SAMPLING_RATES[sampleRateIndex];
  }

  // The frame length is transmitted here, in bytes. Clamp it to what is
  // actually buffered rather than trusting it outright: it decides where the
  // next frame is expected, and a wrong value walks the parser off the stream.
  const unsigned int rawFrameLength = readBits(16);
  if (!rawFrameLength)
    return header;

  readBits(8); // aatf_error_check

  // Channel count: four bits below 15, otherwise 15 is an escape to a full
  // eight bit count (table A.6). The semantics in A.2 are explicit that the
  // eight bits *are* the channel count, not an offset from 15 - the reference
  // decoder reads only four more bits there and adds 15, which does not match.
  unsigned int channelNumber = readBits(4);
  if (channelNumber == 15)
    channelNumber = readBits(8);
  if (!channelNumber)
    return header;

  readBits(2); // resolution

  header.channels = channelNumber;
  header.frameBytes = rawFrameLength;
  header.valid = true;
  return header;
}
} // namespace AV3A
