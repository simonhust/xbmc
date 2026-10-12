/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AEPackIEC61937.h"

#include "utils/log.h"

#include <cassert>
#include <string.h>

#define IEC61937_PREAMBLE1  0xF872
#define IEC61937_PREAMBLE2  0x4E1F

inline void SwapEndian(uint16_t *dst, const uint16_t *src, unsigned int size)
{
  for (unsigned int i = 0; i < size; ++i, ++dst, ++src)
    *dst = ((*src & 0xFF00) >> 8) | ((*src & 0x00FF) << 8);
}

/* Copies one compressed frame into the packet payload as 16 bit words, byte
 * swapped on little endian hosts. A frame with an odd length keeps its last
 * byte and pads the remaining half word; rounding the size up and swapping it
 * would read one byte past the frame. Returns the size the payload occupies. */
inline unsigned int CopyFrameSwapped(uint8_t* dst, const uint8_t* src, unsigned int size)
{
#ifdef __BIG_ENDIAN__
  memcpy(dst, src, size);
#else
  SwapEndian(reinterpret_cast<uint16_t*>(dst), reinterpret_cast<const uint16_t*>(src), size >> 1);
  if (size & 0x1)
  {
    dst[size - 1] = src[size - 1];
    dst[size] = 0;
  }
#endif
  return size + (size & 0x1);
}

int CAEPackIEC61937::PackAC3(uint8_t *data, unsigned int size, uint8_t *dest)
{
  assert(size <= OUT_FRAMESTOBYTES(AC3_FRAME_SIZE) - IEC61937_DATA_OFFSET);
  auto packet = (struct IEC61937Packet*)dest;

  packet->m_preamble1 = IEC61937_PREAMBLE1;
  packet->m_preamble2 = IEC61937_PREAMBLE2;
  packet->m_length    = size << 3;

  if (data == NULL)
    data = packet->m_data;
#ifdef __BIG_ENDIAN__
  else
    memcpy(packet->m_data, data, size);
#else

  int bitstream_mode  = data[5] & 0x7;
  packet->m_type      = IEC61937_TYPE_AC3 | (bitstream_mode << 8);

  size = CopyFrameSwapped(packet->m_data, data, size);
#endif

  const int pad = (int)OUT_FRAMESTOBYTES(AC3_FRAME_SIZE) - (int)IEC61937_DATA_OFFSET - (int)size;
  if (pad > 0) memset(packet->m_data + size, 0, pad);

  return OUT_FRAMESTOBYTES(AC3_FRAME_SIZE);
}

int CAEPackIEC61937::PackEAC3(uint8_t *data, unsigned int size, uint8_t *dest)
{
  assert(size <= OUT_FRAMESTOBYTES(EAC3_FRAME_SIZE) - IEC61937_DATA_OFFSET);
  auto packet = (struct IEC61937Packet*)dest;

  packet->m_preamble1 = IEC61937_PREAMBLE1;
  packet->m_preamble2 = IEC61937_PREAMBLE2;
  packet->m_type      = IEC61937_TYPE_EAC3;
  packet->m_length    = size;

  if (data == NULL)
    data = packet->m_data;
#ifdef __BIG_ENDIAN__
  else
    memcpy(packet->m_data, data, size);
#else
  size = CopyFrameSwapped(packet->m_data, data, size);
#endif

  const int pad = (int)OUT_FRAMESTOBYTES(EAC3_FRAME_SIZE) - (int)IEC61937_DATA_OFFSET - (int)size;
  if (pad > 0) memset(packet->m_data + size, 0, pad);

  return OUT_FRAMESTOBYTES(EAC3_FRAME_SIZE);
}

int CAEPackIEC61937::PackDTS_512(uint8_t *data, unsigned int size, uint8_t *dest, bool littleEndian)
{
  return PackDTS(data, size, dest, littleEndian, OUT_FRAMESTOBYTES(DTS1_FRAME_SIZE), IEC61937_TYPE_DTS1);
}

int CAEPackIEC61937::PackDTS_1024(uint8_t *data, unsigned int size, uint8_t *dest, bool littleEndian)
{
  return PackDTS(data, size, dest, littleEndian, OUT_FRAMESTOBYTES(DTS2_FRAME_SIZE), IEC61937_TYPE_DTS2);
}

int CAEPackIEC61937::PackDTS_2048(uint8_t *data, unsigned int size, uint8_t *dest, bool littleEndian)
{
  return PackDTS(data, size, dest, littleEndian, OUT_FRAMESTOBYTES(DTS3_FRAME_SIZE), IEC61937_TYPE_DTS3);
}

int CAEPackIEC61937::PackTrueHD(const uint8_t* data, unsigned int size, uint8_t* dest)
{
  if (size == 0)
    return OUT_FRAMESTOBYTES(TRUEHD_FRAME_SIZE);

  assert(size <= OUT_FRAMESTOBYTES(TRUEHD_FRAME_SIZE));
  struct IEC61937Packet *packet = (struct IEC61937Packet*)dest;
  packet->m_preamble1 = IEC61937_PREAMBLE1;
  packet->m_preamble2 = IEC61937_PREAMBLE2;
  packet->m_type = IEC61937_TYPE_TRUEHD;
  packet->m_length = 61424;

  if (data == NULL)
    data = packet->m_data;
#ifdef __BIG_ENDIAN__
  else
    memcpy(packet->m_data, data, size);
#else
  size = CopyFrameSwapped(packet->m_data, data, size);
#endif

  memset(packet->m_data + size, 0, OUT_FRAMESTOBYTES(TRUEHD_FRAME_SIZE) - IEC61937_DATA_OFFSET - size);
  return OUT_FRAMESTOBYTES(TRUEHD_FRAME_SIZE);
}

int CAEPackIEC61937::PackDTSHD(uint8_t *data, unsigned int size, uint8_t *dest, unsigned int period)
{
  unsigned int subtype;
  switch (period)
  {
    case   512: subtype = 0; break;
    case  1024: subtype = 1; break;
    case  2048: subtype = 2; break;
    case  4096: subtype = 3; break;
    case  8192: subtype = 4; break;
    case 16384: subtype = 5; break;

    default:
      return 0;
  }

  struct IEC61937Packet *packet = (struct IEC61937Packet*)dest;

  /* The burst holds the payload, the packer prepends a DTS-HD start code and a
   * length word to it (see CAEBitstreamPacker::PackDTSHD), so it has to fit the
   * packet buffer and the header. */
  const unsigned int burstsize = period << 2;
  const unsigned int payload = size + (size & 0x1);
  if (burstsize > MAX_IEC61937_PACKET || payload > burstsize - IEC61937_DATA_OFFSET)
  {
    CLog::Log(LOGERROR, "CAEPackIEC61937::PackDTSHD - {} byte frame does not fit a {} byte burst",
              size, burstsize);
    return 0;
  }

  packet->m_preamble1 = IEC61937_PREAMBLE1;
  packet->m_preamble2 = IEC61937_PREAMBLE2;
  packet->m_type      = IEC61937_TYPE_DTSHD | (subtype << 8);

  /* Align so that (length_code & 0xf) == 0x8. This is reportedly needed
   * with some receivers, but the exact requirement is unconfirmed. */
  packet->m_length    = ((size + 0x17) &~ 0x0f) - 0x08;

  if (data == NULL)
    data = packet->m_data;
#ifdef __BIG_ENDIAN__
  else
    memcpy(packet->m_data, data, size);
#else
  size = CopyFrameSwapped(packet->m_data, data, size);
#endif

  memset(packet->m_data + size, 0, burstsize - IEC61937_DATA_OFFSET - size);
  return burstsize;
}

int CAEPackIEC61937::PackDTSUHD(uint8_t *data, unsigned int size, uint8_t *dest, unsigned int period)
{
  struct IEC61937Packet *packet = (struct IEC61937Packet*)dest;

  const unsigned int burstsize = period << 2;
  const unsigned int payload = size + (size & 0x1);
  if (burstsize > MAX_IEC61937_PACKET || payload > burstsize - IEC61937_DATA_OFFSET)
  {
    CLog::Log(LOGERROR, "CAEPackIEC61937::PackDTSUHD - {} byte frame does not fit a {} byte burst",
              size, burstsize);
    return 0;
  }

  packet->m_preamble1 = IEC61937_PREAMBLE1;
  packet->m_preamble2 = IEC61937_PREAMBLE2;
  /* Data-type 0x11 (type IV) with the DTS-UHD subtype in Pc[11]~Pc[12]. */
  packet->m_type = IEC61937_TYPE_DTSHD | (IEC61937_TYPE_DTSUHD << 8);
  packet->m_length = ((size + 0x17) &~ 0x0f) - 0x08;

  if (data == NULL)
    data = packet->m_data;
#ifdef __BIG_ENDIAN__
  else
    memcpy(packet->m_data, data, size);
#else
  size = CopyFrameSwapped(packet->m_data, data, size);
#endif

  memset(packet->m_data + size, 0, burstsize - IEC61937_DATA_OFFSET - size);
  return burstsize;
}

int CAEPackIEC61937::PackDTS(uint8_t *data, unsigned int size, uint8_t *dest, bool littleEndian,
                             unsigned int frameSize, uint16_t type)
{
  assert(size <= frameSize);

  /* BE is the standard endianness, byteswap needed if LE */
  bool byteSwapNeeded = littleEndian;

#ifndef __BIG_ENDIAN__
  /* on LE systems we want LE output, byteswap needed */
  byteSwapNeeded ^= true;
#endif

  struct IEC61937Packet *packet = (struct IEC61937Packet*)dest;
  uint8_t *dataTo;

  if (size == frameSize)
  {
    /* No packing possible or needed, DTS stream is suitable for direct output */
    dataTo = dest;
  }
  else if (size <= frameSize - IEC61937_DATA_OFFSET)
  {
    /* Fits to IEC61937, perform packing */
    packet->m_preamble1 = IEC61937_PREAMBLE1;
    packet->m_preamble2 = IEC61937_PREAMBLE2;
    packet->m_type      = type;
    packet->m_length    = size << 3;

    dataTo = packet->m_data;
  }
  else
  {
    /* Stream is unsuitable for both packing and direct output */
    return 0;
  }

  if (data == NULL)
    data = dataTo;
  else if (!byteSwapNeeded)
    memcpy(dataTo, data, size);

  if (byteSwapNeeded)
  {
    size += size & 0x1;
    SwapEndian((uint16_t*)dataTo, (uint16_t*)data, size >> 1);
  }

  if (size != frameSize)
    memset(packet->m_data + size, 0, frameSize - IEC61937_DATA_OFFSET - size);

  return frameSize;
}

int CAEPackIEC61937::PackAV3A(uint8_t* data, unsigned int size, uint8_t* dest, unsigned int frameLength)
{
  /* IEC 61937-17 table 1: the sub-data-type in Pc tells which AVS3-P3 frame
   * length the burst carries. */
  unsigned int subDataType;
  switch (frameLength)
  {
    case 512:
      subDataType = 1;
      break;
    case 1024:
      subDataType = 0;
      break;
    default:
      CLog::Log(LOGERROR, "CAEPackIEC61937::PackAV3A - unsupported frame length {}", frameLength);
      return 0;
  }

  /* One data burst carries one complete AVS3-P3 frame (IEC 61937-17, 5.3.1) and
   * fills the repetition period, which at 1x is frameLength IEC 60958 frames of
   * 4 bytes each; the rest of the burst is stuffing. */
  const unsigned int headerBytes = IEC61937_AVS3P3_HEADER_WORDS * 2;
  const unsigned int burstsize = frameLength << 2;
  /* An AVS3-P3 frame is not a whole number of 16 bit words: for general audio
   * the length is derived from the bitrate of the header and can end up odd
   * (mono at 16 kb/s and 48 kHz is 43 bytes), and a lossless frame carries its
   * length as a 16 bit field of its own. Such a frame keeps its last byte and
   * the remaining half word is zero padded, so it is not dropped here. */
  const unsigned int payloadSize = size + (size & 0x1);
  if (burstsize > MAX_IEC61937_PACKET || headerBytes + payloadSize > burstsize)
  {
    CLog::Log(LOGERROR, "CAEPackIEC61937::PackAV3A - {} byte frame does not fit a {} byte burst",
              size, burstsize);
    return 0;
  }

  uint16_t* header = reinterpret_cast<uint16_t*>(dest);
  header[0] = IEC61937_PREAMBLE1;                             /* Pa */
  header[1] = IEC61937_PREAMBLE2;                             /* Pb */
  header[2] = IEC61937_AVS3P3_DATA_TYPE | (subDataType << 8); /* Pc */
  /* Pd counts the burst-payload plus the conditional lengths of Pe and Pf. Clause
   * 6.1.9 of IEC 61937-1:2021: "The size of the Pa, Pb, Pc and Pd is not counted
   * in the value of the length-code. In other words, the length-code indicates
   * the number of bytes of the burst-payload in bytes, plus the conditional
   * lengths of Pe and Pf if they exist", and clause 6.1.8.3.1 confirms that Pe
   * and Pf are present for this data-type. Four bytes are added for them. */
  header[3] = payloadSize + 2 * (IEC61937_AVS3P3_HEADER_WORDS - 4);
  header[4] = IEC61937_AVS3P3_EXTENDED_TYPE;                  /* Pe */
  header[5] = 0;                                              /* Pf: reserved (IEC 61937-1, table 8) */

  uint8_t* payload = dest + headerBytes;
  if (data == nullptr)
    memset(payload, 0, payloadSize);
  else
  {
    /* The payload goes out byte for byte, unlike every other format here.
       IEC 61937-17, 5.3.1 requires the burst-payload to contain one complete
       AVS3-P3 frame, and an AVS3-P3 frame starts with the 12 bit sync word
       0xFFF (T/UWA 009.1-2023, table A.4), so the frame is already in
       network order and swapping 16 bit words would move the sync word from
       byte 0 to byte 1: a real AVS3-P3 frame starts ff f2, and byte swapped
       it starts f2 ff, which is no longer a frame a receiver can find. The
       formats that do get swapped are big endian bit streams, which AVS3-P3
       is not. */
    memcpy(payload, data, size);
    if (size & 0x1)
      payload[size] = 0;
  }

  memset(payload + payloadSize, 0, burstsize - headerBytes - payloadSize);
  return burstsize;
}

int CAEPackIEC61937::PackPause(uint8_t *dest, unsigned int millis, unsigned int framesize, unsigned int samplerate, unsigned int rep_period, unsigned int encodedRate)
{
  int periodInBytes = rep_period * framesize;
  double periodInTime = (double)rep_period / samplerate * 1000;
  int periodsNeeded = millis / periodInTime;
  int maxPeriods = MAX_IEC61937_PACKET / periodInBytes;
  if (periodsNeeded > maxPeriods)
    periodsNeeded = maxPeriods;
  uint16_t gap = encodedRate * millis / 1000;

  struct IEC61937Packet *packet = (struct IEC61937Packet*)dest;
  packet->m_preamble1 = IEC61937_PREAMBLE1;
  packet->m_preamble2 = IEC61937_PREAMBLE2;
  packet->m_type = 3;
  packet->m_length = 32;
  memset(packet->m_data, 0, periodInBytes - 8);

  for (int i=1; i<periodsNeeded; i++)
  {
    memcpy(dest+i*periodInBytes, dest, periodInBytes);
  }

  uint16_t *gapPtr = reinterpret_cast<uint16_t*>(packet->m_data);
  *gapPtr = gap;

  return periodsNeeded * periodInBytes;
}
