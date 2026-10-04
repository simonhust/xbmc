/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <list>
#include <stdint.h>

#define MAX_IEC61937_PACKET  61440
#define IEC61937_DATA_OFFSET 8

#define DTS1_FRAME_SIZE   512
#define DTS2_FRAME_SIZE   1024
#define DTS3_FRAME_SIZE   2048
#define AC3_FRAME_SIZE    1536
#define EAC3_FRAME_SIZE   6144
#define TRUEHD_FRAME_SIZE 15360

#define OUT_SAMPLESIZE 16
/* AVS3-P3 (IEC 61937-17): the data type is signalled as an extended data type,
 * Pc bits 0 to 7 carry 0x1F and the extended data-type word Pe carries 0x0002.
 * Pc bits 8 to 9 select the frame length (0: 1024, 1: 512) and bits 10 to 11 the
 * repetition period (0: 1x, 1: 4x, 2: 8x, 3: 16x, see clause 4.2 and table 1). */
#define IEC61937_AVS3P3_DATA_TYPE      0x1F
#define IEC61937_AVS3P3_EXTENDED_TYPE  0x0002
#define IEC61937_AVS3P3_HEADER_WORDS   6

#define OUT_CHANNELS 2
#define OUT_FRAMESTOBYTES(a) ((a) * OUT_CHANNELS * (OUT_SAMPLESIZE>>3))

class CAEPackIEC61937
{
public:
  CAEPackIEC61937() = default;
  typedef int (*PackFunc)(uint8_t *data, unsigned int size, uint8_t *dest);

  static int PackAC3     (uint8_t *data, unsigned int size, uint8_t *dest);
  static int PackEAC3    (uint8_t *data, unsigned int size, uint8_t *dest);
  static int PackDTS_512 (uint8_t *data, unsigned int size, uint8_t *dest, bool littleEndian);
  static int PackDTS_1024(uint8_t *data, unsigned int size, uint8_t *dest, bool littleEndian);
  static int PackDTS_2048(uint8_t *data, unsigned int size, uint8_t *dest, bool littleEndian);
  static int PackTrueHD(const uint8_t* data, unsigned int size, uint8_t* dest);
  static int PackDTSHD(uint8_t* data, unsigned int size, uint8_t* dest, unsigned int period);
  static int PackAV3A (uint8_t* data, unsigned int size, uint8_t* dest, unsigned int frameLength);
  static int PackPause(uint8_t *dest, unsigned int millis, unsigned int framesize, unsigned int samplerate, unsigned int rep_period, unsigned int encodedRate);
private:

  static int PackDTS(uint8_t *data, unsigned int size, uint8_t *dest, bool littleEndian,
                     unsigned int frameSize, uint16_t type);

  enum IEC61937DataType
  {
    IEC61937_TYPE_NULL   = 0x00,
    IEC61937_TYPE_AC3    = 0x01,
    IEC61937_TYPE_DTS1   = 0x0B, /*  512 samples */
    IEC61937_TYPE_DTS2   = 0x0C, /* 1024 samples */
    IEC61937_TYPE_DTS3   = 0x0D, /* 2048 samples */
    IEC61937_TYPE_DTSHD  = 0x11,
    IEC61937_TYPE_EAC3   = 0x15,
    IEC61937_TYPE_TRUEHD = 0x16
  };

#ifdef __GNUC__
  struct __attribute__((__packed__)) IEC61937Packet
#else
  __pragma(pack(push, 1))
  struct IEC61937Packet
#endif
  {
    uint16_t m_preamble1;
    uint16_t m_preamble2;
    uint16_t m_type;
    uint16_t m_length;
    uint8_t  m_data[MAX_IEC61937_PACKET - IEC61937_DATA_OFFSET];
  };
#ifndef __GNUC__
  __pragma(pack(pop))
#endif
};

