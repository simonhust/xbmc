/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/Base64.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Live DV/HDR metadata payloads walked out of a demuxer packet by the demuxer
// (the single grab point, before the codec's bitstream conversion can rewrite
// or strip them) and attached to the packet for the Amlogic codec to fold into
// its per-frame metadata. The field names mirror AMLFrameMetadata so the values
// transfer without remapping.

struct AMLMetaLatch
{
  std::string doviRpu;
  std::string hdr10pSei;
  std::string cuvaSei;
  std::string hdrMdcv;
  std::string hdrCll;
};

enum
{
  HEVC_NAL_SEI_PREFIX = 39,
  HEVC_NAL_UNSPEC62 = 62 // Dolby Vision RPU
};

enum
{
  SEI_PAYLOAD_REGISTERED_ITU_T_T35 = 4,
  SEI_PAYLOAD_MASTERING_DISPLAY_COLOUR_VOLUME = 137,
  SEI_PAYLOAD_CONTENT_LIGHT_LEVEL_INFO = 144
};

enum
{
  OBU_METADATA = 5,
  OBU_METADATA_TYPE_ITUT_T35 = 4
};

// the T.35 header every HDR10+ payload starts with: country 0xB5,
// provider 0x003C, provider oriented code 0x0001, application identifier 4
inline bool AMLIsHdr10PlusT35(const uint8_t* data, size_t size)
{
  return size >= 8 && data[0] == 0xb5 && data[1] == 0x00 && data[2] == 0x3c &&
         data[3] == 0x00 && data[4] == 0x01 && data[5] == 0x04;
}

// CUVA/HDR Vivid T.35 header: country 0x26 (China), terminal provider 0x0004,
// provider oriented code 0x0005 (CUVA HDR Vivid standard)
inline bool AMLIsCuvaT35(const uint8_t* data, size_t size)
{
  return size >= 6 && data[0] == 0x26 && data[1] == 0x00 && data[2] == 0x04 &&
         data[3] == 0x00 && data[4] == 0x05;
}

// the payload layout shared by the HEVC SEI, the AV1 metadata OBU and the MKV
// block addition side data
inline void AMLLatchHdr10PlusT35(const uint8_t* data, size_t size, AMLMetaLatch& meta)
{
  if (data && AMLIsHdr10PlusT35(data, size))
    meta.hdr10pSei =
        Base64::Encode(reinterpret_cast<const char*>(data), static_cast<unsigned int>(size));
}

inline void AMLLatchCuvaT35(const uint8_t* data, size_t size, AMLMetaLatch& meta)
{
  if (data && AMLIsCuvaT35(data, size))
    meta.cuvaSei =
        Base64::Encode(reinterpret_cast<const char*>(data), static_cast<unsigned int>(size));
}

// Walks one demux packet for the DV RPU (HEVC NAL UNSPEC62). The latched
// payload keeps the escaped NAL including its 7C 01 header, the exact layout
// libdovi and dovi_tool consume. nalLengthSize is 0 for Annex-B input.
inline void AMLLatchHevcDoviRpu(const uint8_t* data,
                                size_t size,
                                int nalLengthSize,
                                AMLMetaLatch& meta)
{
  if (!data || size < 4)
    return;

  if (nalLengthSize >= 1 && nalLengthSize <= 4)
  {
    size_t pos = 0;
    while (pos + nalLengthSize <= size)
    {
      uint32_t len = 0;
      for (int i = 0; i < nalLengthSize; ++i)
        len = (len << 8) | data[pos + i];
      pos += nalLengthSize;
      if (len == 0 || len > size - pos)
        return;
      if (((data[pos] >> 1) & 0x3f) == HEVC_NAL_UNSPEC62)
      {
        meta.doviRpu = Base64::Encode(reinterpret_cast<const char*>(data + pos), len);
        return;
      }
      pos += len;
    }
    return;
  }

  // Annex-B with mixed 3- and 4-byte start codes
  size_t nal = SIZE_MAX;
  for (size_t i = 0; i + 2 < size; ++i)
  {
    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
      continue;
    if (nal != SIZE_MAX && ((data[nal] >> 1) & 0x3f) == HEVC_NAL_UNSPEC62)
    {
      // a 4-byte start code owns the zero before this prefix, and an RPU never
      // ends in 0x00 (rbsp_trailing_bits), so trailing zeros are not payload
      size_t end = i;
      while (end > nal && data[end - 1] == 0)
        end--;
      meta.doviRpu = Base64::Encode(reinterpret_cast<const char*>(data + nal),
                                    static_cast<unsigned int>(end - nal));
      return;
    }
    nal = i + 3;
    i += 2;
  }
  if (nal != SIZE_MAX && nal < size && ((data[nal] >> 1) & 0x3f) == HEVC_NAL_UNSPEC62)
  {
    size_t end = size;
    while (end > nal && data[end - 1] == 0)
      end--;
    meta.doviRpu = Base64::Encode(reinterpret_cast<const char*>(data + nal),
                                  static_cast<unsigned int>(end - nal));
  }
}

// Walks one demux packet for the prefix SEI payloads worth publishing: the
// HDR10+ T.35, the mastering display colour volume and the content light
// level, each latched verbatim after unescaping.
inline void AMLLatchHevcSei(const uint8_t* data,
                            size_t size,
                            int nalLengthSize,
                            AMLMetaLatch& meta)
{
  if (!data || size < 4)
    return;

  const auto parseSeiNal = [&meta](const uint8_t* nal, size_t len)
  {
    if (len < 3 || ((nal[0] >> 1) & 0x3f) != HEVC_NAL_SEI_PREFIX)
      return;

    std::vector<uint8_t> rbsp;
    rbsp.reserve(len);
    for (size_t i = 2; i < len; ++i)
    {
      if (i + 2 < len && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3)
      {
        rbsp.push_back(0);
        rbsp.push_back(0);
        i += 2;
      }
      else
        rbsp.push_back(nal[i]);
    }

    size_t p = 0;
    const size_t end = rbsp.size();
    while (p + 2 < end)
    {
      uint32_t type = 0;
      while (p < end && rbsp[p] == 0xFF)
      {
        type += 255;
        ++p;
      }
      if (p >= end)
        break;
      type += rbsp[p++];

      uint32_t payload = 0;
      while (p < end && rbsp[p] == 0xFF)
      {
        payload += 255;
        ++p;
      }
      if (p >= end)
        break;
      payload += rbsp[p++];
      if (payload > end - p)
        break;

      if (type == SEI_PAYLOAD_REGISTERED_ITU_T_T35)
      {
        AMLLatchHdr10PlusT35(rbsp.data() + p, payload, meta);
        AMLLatchCuvaT35(rbsp.data() + p, payload, meta);
      }
      else if (type == SEI_PAYLOAD_MASTERING_DISPLAY_COLOUR_VOLUME && payload >= 24)
        meta.hdrMdcv = Base64::Encode(reinterpret_cast<const char*>(rbsp.data() + p), payload);
      else if (type == SEI_PAYLOAD_CONTENT_LIGHT_LEVEL_INFO && payload >= 4)
        meta.hdrCll = Base64::Encode(reinterpret_cast<const char*>(rbsp.data() + p), payload);
      p += payload;
    }
  };

  if (nalLengthSize >= 1 && nalLengthSize <= 4)
  {
    size_t pos = 0;
    while (pos + nalLengthSize <= size)
    {
      uint32_t len = 0;
      for (int i = 0; i < nalLengthSize; ++i)
        len = (len << 8) | data[pos + i];
      pos += nalLengthSize;
      if (len == 0 || len > size - pos)
        break;
      parseSeiNal(data + pos, len);
      pos += len;
    }
  }
  else
  {
    size_t nal = SIZE_MAX;
    for (size_t i = 0; i + 2 < size; ++i)
    {
      if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
        continue;
      if (nal != SIZE_MAX)
      {
        size_t end = i;
        while (end > nal && data[end - 1] == 0)
          end--;
        parseSeiNal(data + nal, end - nal);
      }
      nal = i + 3;
      i += 2;
    }
    if (nal != SIZE_MAX && nal < size)
      parseSeiNal(data + nal, size - nal);
  }
}

inline bool AMLReadLeb128(const uint8_t* data, size_t end, size_t& pos, uint64_t& value)
{
  value = 0;
  for (int i = 0; i < 8; ++i)
  {
    if (pos >= end)
      return false;
    const uint8_t b = data[pos++];
    value |= static_cast<uint64_t>(b & 0x7f) << (7 * i);
    if (!(b & 0x80))
      return true;
  }
  return false;
}

// The T.35 payload runs to the last non-zero byte of the OBU, which drops the
// trailing-bits marker and zero padding the same way ffmpeg's
// cbs_av1_get_payload_bytes_left does before it parses HDR10+.
inline size_t AMLAv1T35PayloadLength(const uint8_t* data, size_t size)
{
  size_t len = 0;
  for (size_t i = 0; i < size; ++i)
    if (data[i])
      len = i;
  return len;
}

// Scans AV1 OBUs for the T.35 metadata OBUs and latches the Dolby Vision RPU
// (provider 0x003B) and the HDR10+ payload (provider 0x003C), each from the
// country code on, so an add-on can tell the RPU from an HEVC one by its first
// byte. The RPU keeps the bytes to the end of the OBU, the layout libdovi
// consumes, while HDR10+ is trimmed like ffmpeg trims it.
inline void AMLLatchAv1Metadata(const uint8_t* data, size_t size, AMLMetaLatch& meta)
{
  if (!data)
    return;

  size_t pos = 0;
  while (pos < size)
  {
    const uint8_t hdr = data[pos];
    if (hdr & 0x80)
      return;
    const int type = (hdr >> 3) & 0x0f;
    const bool extension = hdr & 0x04;
    const bool hasSize = hdr & 0x02;
    pos++;
    if (extension)
      pos++;
    if (pos >= size)
      return;
    uint64_t obuSize = 0;
    if (hasSize)
    {
      if (!AMLReadLeb128(data, size, pos, obuSize))
        return;
    }
    else
      obuSize = size - pos;
    if (obuSize > size - pos)
      return;

    if (type == OBU_METADATA)
    {
      const size_t obuEnd = pos + obuSize;
      size_t p = pos;
      uint64_t metadataType = 0;
      if (AMLReadLeb128(data, obuEnd, p, metadataType) &&
          metadataType == OBU_METADATA_TYPE_ITUT_T35)
      {
        const size_t len = obuEnd - p;
        if (len >= 34 && data[p] == 0xb5 && data[p + 1] == 0x00 && data[p + 2] == 0x3b &&
            data[p + 3] == 0x00 && data[p + 4] == 0x00 && data[p + 5] == 0x08 &&
            data[p + 6] == 0x00)
        {
          meta.doviRpu = Base64::Encode(reinterpret_cast<const char*>(data + p),
                                        static_cast<unsigned int>(len));
        }
        else
        {
          AMLLatchHdr10PlusT35(data + p, AMLAv1T35PayloadLength(data + p, len), meta);
          AMLLatchCuvaT35(data + p, AMLAv1T35PayloadLength(data + p, len), meta);
        }
      }
    }
    pos += obuSize;
  }
}
