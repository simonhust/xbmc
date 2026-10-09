/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AEStreamInfo.h"

#include "utils/AV3AHeader.h"
#include "utils/log.h"

#include <algorithm>
#include <string.h>

#define DTS_PREAMBLE_14BE 0x1FFFE800
#define DTS_PREAMBLE_14LE 0xFF1F00E8
#define DTS_PREAMBLE_16BE 0x7FFE8001
#define DTS_PREAMBLE_16LE 0xFE7F0180
#define DTS_PREAMBLE_HD 0x64582025
#define DTS_PREAMBLE_XCH 0x5a5a5a5a
#define DTS_PREAMBLE_XXCH 0x47004a03
#define DTS_PREAMBLE_X96K 0x1d95f262
#define DTS_PREAMBLE_XBR 0x655e315e
#define DTS_PREAMBLE_LBR 0x0a801921
#define DTS_PREAMBLE_XLL 0x41a29547
#define DTS_PREAMBLE_UHD_SYNC 0x40411bf2
#define DTS_PREAMBLE_UHD_NOSYNC 0x71c442e8
#define DTS_SFREQ_COUNT 16
#define MAX_EAC3_BLOCKS 6
#define UNKNOWN_DTS_EXTENSION 255

static const uint16_t AC3Bitrates[] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                       192, 224, 256, 320, 384, 448, 512, 576, 640};
static const uint16_t AC3FSCod[] = {48000, 44100, 32000, 0};
static const uint8_t AC3BlkCod[] = {1, 2, 3, 6};
static const uint8_t AC3Channels[] = {2, 1, 2, 3, 3, 4, 4, 5};
static const uint8_t DTSChannels[] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};
static const uint8_t THDChanMap[] = {2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1};

static const uint32_t DTSSampleRates[DTS_SFREQ_COUNT] = {0,     8000,  16000, 32000, 64000,  128000,
                                                         11025, 22050, 44100, 88200, 176400, 12000,
                                                         24000, 48000, 96000, 192000};

// DTS-UHD FTOC parsing, see ETSI TS 103 491 V1.2.1.
namespace
{
// Field length tables, indexed by the prefix code (clause 5.2.3.1).
constexpr unsigned int UHD_FTOC_SIZE_TABLE[4] = {5, 8, 10, 12};
constexpr unsigned int UHD_MD_CHUNK_SIZE_TABLE[4] = {6, 9, 12, 15};
constexpr unsigned int UHD_AUDIO_CHUNK_ID_TABLE[4] = {2, 4, 6, 8};
constexpr unsigned int UHD_AUDIO_CHUNK_SIZE_TABLE[4] = {9, 11, 13, 16};
// Clause 6.4.6.4 and 6.4.6.6.
constexpr unsigned int UHD_BASE_DURATION[4] = {512, 480, 384, 0};
constexpr unsigned int UHD_CLOCK_RATE[4] = {32000, 44100, 48000, 0};
// Largest FTOC payload, clause 6.4.4.3.
constexpr unsigned int UHD_FTOC_SIZE_MAX = 5408;
// A frame has to fit in the parser buffer.
constexpr unsigned int UHD_FRAME_SIZE_MAX = MAX_IEC61937_PACKET;

struct UHDFTOCInfo
{
  bool syncFrame = false;
  bool fullChannelBasedMix = false;
  bool hasStreamParams = false;
  unsigned int ftocBytes = 0;
  /* Frame length in bytes, 0 when the FTOC does not carry it. */
  unsigned int frameBytes = 0;
  unsigned int sampleRate = 0;
  unsigned int samplesPerFrame = 0;
};

enum class UHDFTOCResult
{
  INVALID,        // not a FTOC sync word, or the FTOC is corrupt
  VALID,          // FTOC parsed, CRC verified where the stream carries one
  NEED_MORE_DATA, // not enough bytes buffered yet to tell
};

/* MSB first bit reader over a byte buffer. */
struct UHDBitReader
{
  const uint8_t* data;
  unsigned int sizeBits;
  unsigned int pos = 0;

  unsigned int Read(unsigned int count)
  {
    unsigned int value = 0;
    while (count-- && pos < sizeBits)
    {
      value = (value << 1) | ((data[pos >> 3] >> (7 - (pos & 7))) & 1);
      pos++;
    }
    return value;
  }
};

/* Variable length field, clause 5.2.3.1: a 0/10/110/111 prefix selects the
   width of the value that follows. */
unsigned int ReadVarLenField(UHDBitReader& reader, const unsigned int* table)
{
  // Prefix code to field index and prefix length, clause 5.2.3.1.
  static const unsigned int indexTable[8] = {0, 0, 0, 0, 1, 1, 2, 3};
  static const unsigned int prefixBitsTable[8] = {1, 1, 1, 1, 2, 2, 3, 3};

  const unsigned int code = reader.Read(3);
  const unsigned int index = indexTable[code];
  reader.pos -= 3 - prefixBitsTable[code]; // rewind the unused prefix bits

  unsigned int value = 0;
  for (unsigned int i = 0; i < index; i++)
    value += 1u << table[i];
  return value + reader.Read(table[index]);
}

/* CRC16 with the nibble lookup table of clause 6.3.8, initial value 0xFFFF. */
uint16_t UHDCRC16(const uint8_t* data, unsigned int length)
{
  static const uint16_t table[16] = {0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5,
                                     0x60c6, 0x70e7, 0x8108, 0x9129, 0xa14a, 0xb16b,
                                     0xc18c, 0xd1ad, 0xe1ce, 0xf1ef};
  uint16_t crc = 0xFFFF;
  for (unsigned int i = 0; i < length; i++)
  {
    crc = (crc << 4) ^ table[(crc >> 12) ^ (data[i] >> 4)];
    crc = (crc << 4) ^ table[(crc >> 12) ^ (data[i] & 0x0f)];
  }
  return crc;
}

/* A frame starts with the FTOC, whose CRC16 - the check a decoder shall use to
   detect a false sync word (clause 6.4.18) - covers everything up to the two
   CRC bytes at the end of the FTOC. */
bool CheckUHDFTOCCRC(const uint8_t* data, unsigned int ftocBytes)
{
  const uint16_t stored = static_cast<uint16_t>((data[ftocBytes - 2] << 8) | data[ftocBytes - 1]);
  return UHDCRC16(data, ftocBytes - 2) == stored;
}

/* Parses the FTOC of a DTS-UHD frame and, for streams whose audio is a full
   channel based mix, its chunk navigation data, which is where the frame length
   comes from (clause 6.4.13). Object based content (full channel based mix
   flag false) needs the audio chunk list of the previous frames, so it is left
   to the sync word search in SyncDTS. */
UHDFTOCResult ParseUHDFTOC(const uint8_t* data,
                           unsigned int size,
                           DTSStreamParams& params,
                           UHDFTOCInfo& info)
{
  if (size < 6)
    return UHDFTOCResult::NEED_MORE_DATA;

  const unsigned int syncWord = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
  if (syncWord == DTS_PREAMBLE_UHD_SYNC)
    info.syncFrame = true;
  else if (syncWord == DTS_PREAMBLE_UHD_NOSYNC)
    info.syncFrame = false;
  else
    return UHDFTOCResult::INVALID;

  UHDBitReader reader{data, size * 8};
  reader.Read(32);
  info.ftocBytes = ReadVarLenField(reader, UHD_FTOC_SIZE_TABLE) + 1;
  if (info.ftocBytes > UHD_FTOC_SIZE_MAX || info.ftocBytes < 4)
    return UHDFTOCResult::INVALID;
  if (size < info.ftocBytes)
    return UHDFTOCResult::NEED_MORE_DATA;

  if (info.syncFrame)
  {
    info.fullChannelBasedMix = reader.Read(1) != 0;

    // Stream version, only coded when the content is not a full channel based
    // mix (clause 6.4.6.3).
    if (!info.fullChannelBasedMix)
    {
      const bool shortRevision = reader.Read(1) != 0;
      reader.Read(shortRevision ? 6 : 12);
    }

    const unsigned int baseDuration = UHD_BASE_DURATION[reader.Read(2)];
    const unsigned int frameDuration = baseDuration * (reader.Read(3) + 1);
    const unsigned int clockRate = UHD_CLOCK_RATE[reader.Read(2)];
    if (!baseDuration || !clockRate)
      return UHDFTOCResult::INVALID;

    // Time stamp is optional (clause 6.4.6.7): a presence flag, an update flag
    // for parameters that exist with a predefined value in sync frames, the
    // value itself and four more bits to complete the 36 bit field.
    if (reader.Read(1))
    {
      if (reader.Read(1))
      {
        reader.Read(32);
        reader.Read(4);
      }
    }

    const unsigned int sampleRate = clockRate << reader.Read(2);
    // Guard against a misparsed FTOC: the stream parameters have to fit in the
    // FTOC and the frame duration has to convert to a whole number of samples.
    if (reader.pos > (info.ftocBytes - 2) * 8 || (frameDuration * sampleRate) % clockRate)
      return UHDFTOCResult::INVALID;
    if (!CheckUHDFTOCCRC(data, info.ftocBytes))
      return UHDFTOCResult::INVALID;

    info.hasStreamParams = true;
    info.sampleRate = sampleRate;
    info.samplesPerFrame = frameDuration * sampleRate / clockRate;
    params.valid = true;
    params.fullChannelBasedMix = info.fullChannelBasedMix;
  }
  else
  {
    // Stream parameters are transmitted in sync frames only (clause 6.4.5), so
    // reuse them. The FTOC CRC word is carried by every frame only when the
    // content is not a full channel based mix (clause 6.4.6.2).
    info.fullChannelBasedMix = params.fullChannelBasedMix;
    if (params.valid && !params.fullChannelBasedMix && !CheckUHDFTOCCRC(data, info.ftocBytes))
      return UHDFTOCResult::INVALID;
    if (!params.valid)
      return UHDFTOCResult::VALID; // nothing to derive, let SyncDTS search the next frame
  }

  if (info.fullChannelBasedMix)
  {
    // A full channel based mix has one metadata chunk in sync frames and none
    // otherwise, plus exactly one audio chunk (clause 6.4.6.1, 6.4.13).
    unsigned int audioChunkId;
    unsigned int audioChunkSize = 0;
    unsigned int payloadBytes = 0;
    if (info.syncFrame)
    {
      payloadBytes += ReadVarLenField(reader, UHD_MD_CHUNK_SIZE_TABLE);
      audioChunkId = ReadVarLenField(reader, UHD_AUDIO_CHUNK_ID_TABLE);
      params.audioChunkId = audioChunkId;
      params.audioChunkIdValid = audioChunkId != 0;
    }
    else
    {
      audioChunkId = params.audioChunkIdValid ? params.audioChunkId : 1;
    }
    if (audioChunkId)
      audioChunkSize = ReadVarLenField(reader, UHD_AUDIO_CHUNK_SIZE_TABLE);
    payloadBytes += audioChunkSize;
    if (info.syncFrame)
      params.audioChunkSize = audioChunkSize;

    const unsigned int ftocBits = (info.ftocBytes - (info.syncFrame ? 2 : 0)) * 8;
    if (reader.pos <= ftocBits && payloadBytes && info.ftocBytes + payloadBytes <= UHD_FRAME_SIZE_MAX)
      info.frameBytes = info.ftocBytes + payloadBytes;
    else if (!info.syncFrame && params.valid && audioChunkId)
    {
      // Non-sync frames carry no FTOC; their payload is the audio chunk whose
      // size the preceding sync frame announced (clause 6.4.6.1).
      if (info.ftocBytes + params.audioChunkSize <= UHD_FRAME_SIZE_MAX)
        info.frameBytes = info.ftocBytes + params.audioChunkSize;
    }
  }

  return UHDFTOCResult::VALID;
}
} // namespace

CAEStreamParser::CAEStreamParser() : m_syncFunc(&CAEStreamParser::DetectType)
{
  av_crc_init(m_crcTrueHD, 0, 16, 0x2D, sizeof(m_crcTrueHD));
}

double CAEStreamInfo::GetDuration() const
{
  double duration = 0;
  switch (m_type)
  {
    case STREAM_TYPE_AC3:
      duration = 1536.0 / m_sampleRate;
      break;
    case STREAM_TYPE_EAC3:
      duration = 6144.0 / m_sampleRate / 4;
      break;
    case STREAM_TYPE_TRUEHD:
      int rate;
      if (m_sampleRate == 48000 || m_sampleRate == 96000 || m_sampleRate == 192000)
        rate = 192000;
      else
        rate = 176400;
      duration = 3840.0 / rate;
      break;
    case STREAM_TYPE_DTS_512:
    case STREAM_TYPE_DTSHD_CORE:
    case STREAM_TYPE_DTSHD:
    case STREAM_TYPE_DTSHD_MA:
      duration = 512.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_1024:
      duration = 1024.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_2048:
      duration = 2048.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_UHD:
    case STREAM_TYPE_AV3A:
      // The frame duration comes from the DTS-UHD FTOC; AVS3 (and UHD streams
      // whose FTOC has not been parsed yet) use the fixed 1024 sample frame.
      duration = (m_samplesPerFrame ? m_samplesPerFrame : 1024.0) / m_sampleRate;
      break;
    default:
      CLog::Log(LOGERROR, "CAEStreamInfo::GetDuration - invalid stream type");
      break;
  }
  return duration * 1000;
}

bool CAEStreamInfo::operator==(const CAEStreamInfo& info) const
{
  if (m_type != info.m_type)
    return false;
  if (m_dataIsLE != info.m_dataIsLE)
    return false;
  if (m_repeat != info.m_repeat)
    return false;
  return true;
}

void CAEStreamParser::Reset()
{
  m_skipBytes = 0;
  m_bufferSize = 0;
  m_needBytes = 0;
  m_hasSync = false;
  m_uhdParams = DTSStreamParams{};
}

int CAEStreamParser::AddData(uint8_t* data,
                             unsigned int size,
                             uint8_t** buffer,
                             unsigned int* bufferSize)
{
  if (size == 0)
  {
    if (bufferSize)
      *bufferSize = 0;
    return 0;
  }

  if (m_skipBytes)
  {
    unsigned int canSkip = std::min(size, m_skipBytes);
    unsigned int room = sizeof(m_buffer) - m_bufferSize;
    unsigned int copy = std::min(room, canSkip);

    memcpy(m_buffer + m_bufferSize, data, copy);
    m_bufferSize += copy;
    m_skipBytes -= copy;

    if (m_skipBytes)
    {
      if (bufferSize)
        *bufferSize = 0;
      return copy;
    }

    GetPacket(buffer, bufferSize);
    return copy;
  }
  else
  {
    unsigned int consumed = 0;
    unsigned int offset = 0;
    unsigned int room = sizeof(m_buffer) - m_bufferSize;
    while (true)
    {
      if (!size)
      {
        if (bufferSize)
          *bufferSize = 0;
        return consumed;
      }

      unsigned int copy = std::min(room, size);
      memcpy(m_buffer + m_bufferSize, data, copy);
      m_bufferSize += copy;
      consumed += copy;
      data += copy;
      size -= copy;
      room -= copy;

      if (m_needBytes > m_bufferSize)
        continue;

      m_needBytes = 0;
      offset = (this->*m_syncFunc)(m_buffer, m_bufferSize);

      if (m_hasSync)
        break;
      else
      {
        // lost sync
        m_syncFunc = &CAEStreamParser::DetectType;
        m_info.m_type = CAEStreamInfo::STREAM_TYPE_NULL;
        m_info.m_repeat = 1;

        // if the buffer is full, or the offset < the buffer size
        if (m_bufferSize == sizeof(m_buffer) || offset < m_bufferSize)
        {
          m_bufferSize -= offset;
          room += offset;
          memmove(m_buffer, m_buffer + offset, m_bufferSize);
        }
      }
    }

    // if we got here, we acquired sync on the buffer

    // align the buffer
    if (offset)
    {
      m_bufferSize -= offset;
      memmove(m_buffer, m_buffer + offset, m_bufferSize);
    }

    // bytes to skip until the next packet
    m_skipBytes = std::max(0, (int)m_fsize - (int)m_bufferSize);
    if (m_skipBytes)
    {
      if (bufferSize)
        *bufferSize = 0;
      return consumed;
    }

    if (!m_needBytes)
      GetPacket(buffer, bufferSize);
    else if (bufferSize)
      *bufferSize = 0;

    return consumed;
  }
}

void CAEStreamParser::GetPacket(uint8_t** buffer, unsigned int* bufferSize)
{
  // if the caller wants the packet
  if (buffer)
  {
    // if it is dtsHD and we only want the core, just fetch that
    unsigned int size = m_fsize;
    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE)
      size = m_coreSize;

    // make sure the buffer is allocated and big enough
    if (!*buffer || !bufferSize || *bufferSize < size)
    {
      delete[] * buffer;
      *buffer = new uint8_t[size];
    }

    // copy the data into the buffer and update the size
    memcpy(*buffer, m_buffer, size);
    if (bufferSize)
      *bufferSize = size;
  }

  // remove the parsed data from the buffer
  m_bufferSize -= m_fsize;
  memmove(m_buffer, m_buffer + m_fsize, m_bufferSize);
  m_fsize = 0;
  m_coreSize = 0;
}

// SYNC FUNCTIONS

// This function looks for sync words across the types in parallel, and only does an exhaustive
// test if it finds a syncword. Once sync has been established, the relevant sync function sets
// m_syncFunc to itself. This function will only be called again if total sync is lost, which
// allows is to switch stream types on the fly much like a real receiver does.
unsigned int CAEStreamParser::DetectType(uint8_t* data, unsigned int size)
{
  unsigned int skipped = 0;
  unsigned int possible = 0;

  while (size > 8)
  {
    // if it could be DTS
    unsigned int header = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
    if (header == DTS_PREAMBLE_14LE || header == DTS_PREAMBLE_14BE || header == DTS_PREAMBLE_16LE ||
        header == DTS_PREAMBLE_16BE)
    {
      unsigned int skip = SyncDTS(data, size);
      if (m_hasSync || m_needBytes)
        return skipped + skip;
      else
        possible = skipped;
    }

    // if it could be DTS-UHD (ETSI TS 103 491)
    if (header == DTS_PREAMBLE_UHD_SYNC || header == DTS_PREAMBLE_UHD_NOSYNC)
    {
      unsigned int skip = SyncDTS(data, size);
      if (m_hasSync || m_needBytes)
        return skipped + skip;
      else
        possible = skipped;
    }

    // if it could be AV3A (Audio Vivid / AVS3-P3)
    if (AV3A::HasSyncWord(data, size))
    {
      unsigned int skip = SyncDTS(data, size);
      if (m_hasSync || m_needBytes)
        return skipped + skip;
      else
        possible = skipped;
    }

    // if it could be AC3
    if (data[0] == 0x0b && data[1] == 0x77)
    {
      unsigned int skip = SyncAC3(data, size);
      if (m_hasSync || m_needBytes)
        return skipped + skip;
      else
        possible = skipped;
    }

    // if it could be TrueHD
    if (data[4] == 0xf8 && data[5] == 0x72 && data[6] == 0x6f && data[7] == 0xba)
    {
      unsigned int skip = SyncTrueHD(data, size);
      if (m_hasSync)
        return skipped + skip;
      else
        possible = skipped;
    }

    // move along one byte
    --size;
    ++skipped;
    ++data;
  }

  return possible ? possible : skipped;
}

bool CAEStreamParser::TrySyncAC3(uint8_t* data,
                                 unsigned int size,
                                 bool resyncing,
                                 bool wantEAC3dependent)
{
  if (size < 8)
    return false;

  // look for an ac3 sync word
  if (data[0] != 0x0b || data[1] != 0x77)
    return false;

  uint8_t bsid = data[5] >> 3;
  uint8_t acmod = data[6] >> 5;
  uint8_t lfeon;

  int8_t pos = 4;
  if ((acmod & 0x1) && (acmod != 0x1))
    pos -= 2;
  if (acmod & 0x4)
    pos -= 2;
  if (acmod == 0x2)
    pos -= 2;
  if (pos < 0)
    lfeon = (data[7] & 0x64) ? 1 : 0;
  else
    lfeon = ((data[6] >> pos) & 0x1) ? 1 : 0;

  if (bsid > 0x11 || acmod > 7)
    return false;

  if (bsid <= 10)
  {
    // Normal AC-3

    if (wantEAC3dependent)
      return false;

    uint8_t fscod = data[4] >> 6;
    uint8_t frmsizecod = data[4] & 0x3F;
    if (fscod == 3 || frmsizecod > 37)
      return false;

    // get the details we need to check crc1 and framesize
    unsigned int bitRate = AC3Bitrates[frmsizecod >> 1];
    unsigned int framesize = 0;
    switch (fscod)
    {
      case 0:
        framesize = bitRate * 2;
        break;
      case 1:
        framesize = (320 * bitRate / 147 + (frmsizecod & 1 ? 1 : 0));
        break;
      case 2:
        framesize = bitRate * 4;
        break;
    }

    m_fsize = framesize << 1;
    m_info.m_sampleRate = AC3FSCod[fscod];

    // dont do extensive testing if we have not lost sync
    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_AC3 && !resyncing)
      return true;

    // this may be the main stream of EAC3
    unsigned int fsizeMain = m_fsize;
    unsigned int reqBytes = fsizeMain + 8;
    if (size < reqBytes)
    {
      // not enough data to check for E-AC3 dependent frame, request more
      m_needBytes = reqBytes;
      m_fsize = 0;
      // no need to resync => return true
      return true;
    }
    m_info.m_frameSize = fsizeMain;
    if (TrySyncAC3(data + fsizeMain, size - fsizeMain, resyncing, true))
    {
      // concatenate the main and dependent frames
      m_fsize += fsizeMain;
      return true;
    }

    unsigned int crc_size;
    // if we have enough data, validate the entire packet, else try to validate crc2 (5/8 of the packet)
    if (framesize <= size)
      crc_size = framesize - 1;
    else
      crc_size = (framesize >> 1) + (framesize >> 3) - 1;

    if (crc_size <= size)
      if (av_crc(av_crc_get_table(AV_CRC_16_ANSI), 0, &data[2], crc_size * 2))
        return false;

    // if we get here, we can sync
    m_hasSync = true;
    m_info.m_channels = AC3Channels[acmod] + lfeon;
    m_syncFunc = &CAEStreamParser::SyncAC3;
    m_info.m_type = CAEStreamInfo::STREAM_TYPE_AC3;
    m_info.m_frameSize += m_fsize;
    m_info.m_repeat = 1;

    CLog::Log(LOGINFO, "CAEStreamParser::TrySyncAC3 - AC3 stream detected ({} channels, {}Hz)",
              m_info.m_channels, m_info.m_sampleRate);
    return true;
  }
  else
  {
    // Enhanced AC-3
    uint8_t strmtyp = data[2] >> 6;
    if (strmtyp == 3)
      return false;

    if (strmtyp != 1 && wantEAC3dependent)
      return false;

    unsigned int framesize = (((data[2] & 0x7) << 8) | data[3]) + 1;
    uint8_t fscod = (data[4] >> 6) & 0x3;
    uint8_t cod = (data[4] >> 4) & 0x3;
    uint8_t acmod = (data[4] >> 1) & 0x7;
    uint8_t lfeon = data[4] & 0x1;
    uint8_t blocks;

    if (fscod == 0x3)
    {
      if (cod == 0x3)
        return false;

      blocks = 6;
      m_info.m_sampleRate = AC3FSCod[cod] >> 1;
    }
    else
    {
      blocks = AC3BlkCod[cod];
      m_info.m_sampleRate = AC3FSCod[fscod];
    }

    m_fsize = framesize << 1;
    m_info.m_repeat = MAX_EAC3_BLOCKS / blocks;

    // EAC3 can have a dependent stream too
    if (!wantEAC3dependent)
    {
      unsigned int fsizeMain = m_fsize;
      unsigned int reqBytes = fsizeMain + 8;
      if (size < reqBytes)
      {
        // not enough data to check for E-AC3 dependent frame, request more
        m_needBytes = reqBytes;
        m_fsize = 0;
        // no need to resync => return true
        return true;
      }
      m_info.m_frameSize = fsizeMain;
      if (TrySyncAC3(data + fsizeMain, size - fsizeMain, resyncing, true))
      {
        // concatenate the main and dependent frames
        m_fsize += fsizeMain;
        return true;
      }
    }

    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_EAC3 && m_hasSync && !resyncing)
      return true;

    // if we get here, we can sync
    m_hasSync = true;
    m_info.m_channels = AC3Channels[acmod] + lfeon;
    m_syncFunc = &CAEStreamParser::SyncAC3;
    m_info.m_type = CAEStreamInfo::STREAM_TYPE_EAC3;
    m_info.m_frameSize += m_fsize;

    CLog::Log(LOGINFO, "CAEStreamParser::TrySyncAC3 - E-AC3 stream detected ({} channels, {}Hz)",
              m_info.m_channels, m_info.m_sampleRate);
    return true;
  }
}

unsigned int CAEStreamParser::SyncAC3(uint8_t* data, unsigned int size)
{
  unsigned int skip = 0;

  for (; size - skip > 7; ++skip, ++data)
  {
    bool resyncing = (skip != 0);
    if (TrySyncAC3(data, size - skip, resyncing, false))
      return skip;
  }

  // if we get here, the entire packet is invalid and we have lost sync
  CLog::Log(LOGINFO, "CAEStreamParser::SyncAC3 - AC3 sync lost");
  m_hasSync = false;
  return skip;
}

unsigned int CAEStreamParser::SyncDTS(uint8_t* data, unsigned int size)
{
  if (size < 13)
  {
    if (m_needBytes < 13)
      m_needBytes = 14;
    return 0;
  }

  unsigned int skip = 0;
  for (; size - skip > 13; ++skip, ++data)
  {
    unsigned int header = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];

    // AV3A (Audio Vivid / AVS3-P3) - 12 bit sync word, check before 4-byte DTS switch.
    // Skip AV3A detection if we already have DTS-UHD sync to avoid false positives.
    if (m_info.m_type != CAEStreamInfo::STREAM_TYPE_DTS_UHD && AV3A::HasSyncWord(data, size - skip))
    {
      const unsigned int available = size - skip;
      if (available < AV3A::MAX_HEADER_BYTES)
      {
        // Wait for the frame header before deciding on the frame size.
        m_needBytes = available + 1;
        m_fsize = 0;
        return skip;
      }

      const AV3A::FrameHeader frameHeader = AV3A::ParseFrameHeader(data, available);
      if (!frameHeader.valid)
        continue; // false sync word, keep searching

      // The frame length is not part of the header but follows from the bitrate
      // of the channel configuration; verify it against the next frame when that
      // one is buffered already.
      m_fsize = frameHeader.frameBytes;
      if (m_fsize && m_fsize + AV3A::MAX_HEADER_BYTES <= available &&
          !AV3A::ParseFrameHeader(data + m_fsize, available - m_fsize).valid)
        m_fsize = 0;

      // Without a usable frame length, the frame ends where the next header
      // that parses starts.
      if (!m_fsize)
      {
        for (unsigned int i = 2; i + AV3A::MAX_HEADER_BYTES <= available; i++)
        {
          if (!AV3A::HasSyncWord(data + i, available - i))
            continue;
          if (AV3A::ParseFrameHeader(data + i, available - i).valid)
          {
            m_fsize = i;
            break;
          }
        }
        if (!m_fsize)
          m_fsize = available; // next frame is not buffered yet
      }

      if (m_fsize > available && m_fsize <= sizeof(m_buffer))
      {
        // Frame is not buffered completely yet.
        m_needBytes = m_fsize;
        m_fsize = 0;
        return skip;
      }
      if (m_fsize > available)
        m_fsize = available;

      m_info.m_type = CAEStreamInfo::STREAM_TYPE_AV3A;
      m_info.m_sampleRate = frameHeader.sampleRate ? frameHeader.sampleRate : 48000;
      m_info.m_channels = frameHeader.channels;
      m_info.m_samplesPerFrame = AV3A::SAMPLES_PER_FRAME;
      m_info.m_dtsPeriod = m_info.m_samplesPerFrame;
      m_info.m_frameSize = m_fsize;
      m_info.m_repeat = 1;
      m_syncFunc = &CAEStreamParser::SyncDTS;
      if (!m_hasSync)
      {
        CLog::Log(LOGINFO,
                  "CAEStreamParser::SyncDTS - AV3A stream detected ({} channels, {}Hz, {} "
                  "samples/frame, {} bytes/frame)",
                  m_info.m_channels, m_info.m_sampleRate, m_info.m_samplesPerFrame, m_fsize);
      }
      m_hasSync = true;
      return skip;
    }
    /* If AV3A is already detected, do not switch to DTS-UHD to avoid false positives. */
    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_AV3A)
      return 0;

    unsigned int hd_sync = 0;
    unsigned int dtsBlocks;
    unsigned int amode;
    unsigned int sfreq;
    unsigned int target_rate;
    unsigned int extension = 0;
    unsigned int ext_type = UNKNOWN_DTS_EXTENSION;
    unsigned int lfe;
    int bits;

    switch (header)
    {
      // 14bit BE
      case DTS_PREAMBLE_14BE:
        if (data[4] != 0x07 || (data[5] & 0xf0) != 0xf0)
          continue;
        dtsBlocks = (((data[5] & 0x7) << 4) | ((data[6] & 0x3C) >> 2)) + 1;
        m_fsize = (((((data[6] & 0x3) << 8) | data[7]) << 4) | ((data[8] & 0x3C) >> 2)) + 1;
        amode = ((data[8] & 0x3) << 4) | ((data[9] & 0xF0) >> 4);
        target_rate = ((data[10] & 0x3e) >> 1);
        extension = ((data[11] & 0x1));
        ext_type = ((data[11] & 0xe) >> 1);
        sfreq = data[9] & 0xF;
        lfe = (data[12] & 0x18) >> 3;
        m_info.m_dataIsLE = false;
        bits = 14;
        break;

      // 14bit LE
      case DTS_PREAMBLE_14LE:
        if (data[5] != 0x07 || (data[4] & 0xf0) != 0xf0)
          continue;
        dtsBlocks = (((data[4] & 0x7) << 4) | ((data[7] & 0x3C) >> 2)) + 1;
        m_fsize = (((((data[7] & 0x3) << 8) | data[6]) << 4) | ((data[9] & 0x3C) >> 2)) + 1;
        amode = ((data[9] & 0x3) << 4) | ((data[8] & 0xF0) >> 4);
        target_rate = ((data[11] & 0x3e) >> 1);
        extension = ((data[10] & 0x1));
        ext_type = ((data[10] & 0xe) >> 1);
        sfreq = data[8] & 0xF;
        lfe = (data[13] & 0x18) >> 3;
        m_info.m_dataIsLE = true;
        bits = 14;
        break;

      // 16bit BE
      case DTS_PREAMBLE_16BE:
        dtsBlocks = (((data[4] & 0x1) << 7) | ((data[5] & 0xFC) >> 2)) + 1;
        m_fsize = (((((data[5] & 0x3) << 8) | data[6]) << 4) | ((data[7] & 0xF0) >> 4)) + 1;
        amode = ((data[7] & 0x0F) << 2) | ((data[8] & 0xC0) >> 6);
        sfreq = (data[8] & 0x3C) >> 2;
        target_rate = ((data[8] & 0x03) << 3) | ((data[9] & 0xe0) >> 5);
        extension = (data[10] & 0x10) >> 4;
        ext_type = (data[10] & 0xe0) >> 5;
        lfe = (data[10] >> 1) & 0x3;
        m_info.m_dataIsLE = false;
        bits = 16;
        break;

      // 16bit LE
      case DTS_PREAMBLE_16LE:
        dtsBlocks = (((data[5] & 0x1) << 7) | ((data[4] & 0xFC) >> 2)) + 1;
        m_fsize = (((((data[4] & 0x3) << 8) | data[7]) << 4) | ((data[6] & 0xF0) >> 4)) + 1;
        amode = ((data[6] & 0x0F) << 2) | ((data[9] & 0xC0) >> 6);
        sfreq = (data[9] & 0x3C) >> 2;
        target_rate = ((data[9] & 0x03) << 3) | ((data[8] & 0xe0) >> 5);
        extension = (data[11] & 0x10) >> 4;
        ext_type = (data[11] & 0xe0) >> 5;
        lfe = (data[11] >> 1) & 0x3;
        m_info.m_dataIsLE = true;
        bits = 16;
        break;

      // DTS-UHD (ETSI TS 103 491)
      case DTS_PREAMBLE_UHD_SYNC:
      case DTS_PREAMBLE_UHD_NOSYNC:
      {
        const unsigned int available = size - skip;
        UHDFTOCInfo ftoc;
        const UHDFTOCResult result = ParseUHDFTOC(data, available, m_uhdParams, ftoc);
        if (result == UHDFTOCResult::NEED_MORE_DATA)
        {
          // Wait until the whole FTOC is buffered instead of guessing a frame size.
          m_needBytes = ftoc.ftocBytes > available ? ftoc.ftocBytes : available + 1;
          m_fsize = 0;
          return skip;
        }
        if (result == UHDFTOCResult::INVALID)
          continue; // false sync word, keep searching (clause 6.4.18)

        // The FTOC states the frame length; verify it against the FTOC of the
        // next frame when that one is buffered already.
        m_fsize = ftoc.frameBytes;
        if (m_fsize && m_fsize + 6 <= available)
        {
          UHDFTOCInfo next;
          if (ParseUHDFTOC(data + m_fsize, available - m_fsize, m_uhdParams, next) ==
              UHDFTOCResult::INVALID)
            m_fsize = 0;
        }

        // Without a usable frame length, the frame ends where the next FTOC
        // that passes validation starts.
        if (!m_fsize)
        {
          for (unsigned int i = 4; i + 6 <= available; i++)
          {
            const unsigned int word =
                data[i] << 24 | data[i + 1] << 16 | data[i + 2] << 8 | data[i + 3];
            if (word != DTS_PREAMBLE_UHD_SYNC && word != DTS_PREAMBLE_UHD_NOSYNC)
              continue;
            UHDFTOCInfo next;
            if (ParseUHDFTOC(data + i, available - i, m_uhdParams, next) == UHDFTOCResult::VALID)
            {
              m_fsize = i;
              break;
            }
          }
          if (!m_fsize)
            m_fsize = available; // next frame is not buffered yet
        }

        if (m_fsize > available && m_fsize <= sizeof(m_buffer))
        {
          // Frame is not buffered completely yet.
          m_needBytes = m_fsize;
          m_fsize = 0;
          return skip;
        }
        if (m_fsize > available)
          m_fsize = available;

        if (ftoc.hasStreamParams)
        {
          // Stream parameters are transmitted in sync frames only (clause 6.4.5).
          m_info.m_sampleRate = ftoc.sampleRate;
          m_info.m_samplesPerFrame = ftoc.samplesPerFrame;
        }
        else if (!m_info.m_sampleRate)
        {
          // Sync was acquired on a non-sync frame, so the parameters are not
          // known before the next sync frame: assume the common configuration.
          m_info.m_sampleRate = 48000;
          m_info.m_samplesPerFrame = 1024;
        }

        m_info.m_dataIsLE = false;
        m_info.m_type = CAEStreamInfo::STREAM_TYPE_DTS_UHD;
        m_info.m_channels = 8; // default to 7.1 for passthrough; actual count decoded by receiver
        m_info.m_dtsPeriod = m_info.m_samplesPerFrame;
        m_info.m_frameSize = m_fsize;
        m_info.m_repeat = 1;
        m_syncFunc = &CAEStreamParser::SyncDTS;
        if (ftoc.hasStreamParams || !m_hasSync)
        {
          // One line per sync frame instead of one per frame.
          CLog::Log(LOGINFO,
                    "CAEStreamParser::SyncDTS - DTS-UHD {} frame detected ({}Hz, {} samples/frame, "
                    "FTOC {} bytes, {} bytes/frame)",
                    ftoc.syncFrame ? "sync" : "non-sync", m_info.m_sampleRate,
                    m_info.m_samplesPerFrame, ftoc.ftocBytes, m_fsize);
        }
        m_hasSync = true;
        return skip;
      }


      default:
        continue;
    }

    if (sfreq == 0 || sfreq >= DTS_SFREQ_COUNT)
      continue;

    // make sure the framesize is sane
    if (m_fsize < 96 || m_fsize > 16384)
      continue;

    CAEStreamInfo::DataType dataType{CAEStreamInfo::STREAM_TYPE_NULL};
    switch (dtsBlocks << 5)
    {
      case 512:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_512;
        break;
      case 1024:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_1024;
        break;
      case 2048:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_2048;
        break;
    }

    if (dataType == CAEStreamInfo::STREAM_TYPE_NULL)
      continue;

    // adjust the fsize for 14 bit streams
    if (bits == 14)
      m_fsize = m_fsize / 14 * 16;

    // we need enough data to check for DTS-HD
    if (size - skip < m_fsize + 10)
    {
      // we can assume DTS sync at this point
      m_syncFunc = &CAEStreamParser::SyncDTS;
      m_needBytes = m_fsize + 10;
      m_fsize = 0;

      return skip;
    }

    // look for DTS-HD
    hd_sync = (data[m_fsize] << 24) | (data[m_fsize + 1] << 16) | (data[m_fsize + 2] << 8) |
              data[m_fsize + 3];
    if (hd_sync == DTS_PREAMBLE_HD)
    {
      int hd_size;
      bool blownup = (data[m_fsize + 5] & 0x20) != 0;
      if (blownup)
        hd_size = (((data[m_fsize + 6] & 0x01) << 19) | (data[m_fsize + 7] << 11) |
                   (data[m_fsize + 8] << 3) | ((data[m_fsize + 9] & 0xe0) >> 5)) +
                  1;
      else
        hd_size = (((data[m_fsize + 6] & 0x1f) << 11) | (data[m_fsize + 7] << 3) |
                   ((data[m_fsize + 8] & 0xe0) >> 5)) +
                  1;

      int header_size;
      if (blownup)
        header_size = (((data[m_fsize + 5] & 0x1f) << 7) | ((data[m_fsize + 6] & 0xfe) >> 1)) + 1;
      else
        header_size = (((data[m_fsize + 5] & 0x1f) << 3) | ((data[m_fsize + 6] & 0xe0) >> 5)) + 1;

      hd_sync = data[m_fsize + header_size] << 24 | data[m_fsize + header_size + 1] << 16 |
                data[m_fsize + header_size + 2] << 8 | data[m_fsize + header_size + 3];

      // set the type according to core or not
      if (m_coreOnly)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD_CORE;
      else if (hd_sync == DTS_PREAMBLE_XLL)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD_MA;
      else if (hd_sync == DTS_PREAMBLE_XCH || hd_sync == DTS_PREAMBLE_XXCH ||
               hd_sync == DTS_PREAMBLE_X96K || hd_sync == DTS_PREAMBLE_XBR ||
               hd_sync == DTS_PREAMBLE_LBR)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD;
      else
        dataType = m_info.m_type;

      m_coreSize = m_fsize;
      m_fsize += hd_size;
    }

    unsigned int sampleRate = DTSSampleRates[sfreq];
    if (!m_hasSync || skip || dataType != m_info.m_type || sampleRate != m_info.m_sampleRate ||
        dtsBlocks != m_dtsBlocks)
    {
      m_hasSync = true;
      m_info.m_type = dataType;
      m_info.m_sampleRate = sampleRate;
      m_dtsBlocks = dtsBlocks;
      m_info.m_channels = DTSChannels[amode] + (lfe ? 1 : 0);
      m_syncFunc = &CAEStreamParser::SyncDTS;
      m_info.m_frameSize = m_fsize;
      m_info.m_repeat = 1;

      if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD_MA)
      {
        m_info.m_channels += 2; // FIXME: this needs to be read out, not sure how to do that yet
        m_info.m_dtsPeriod = (192000 * (8 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }
      else if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD)
      {
        m_info.m_dtsPeriod = (192000 * (2 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }
      else
      {
        m_info.m_dtsPeriod =
            (m_info.m_sampleRate * (2 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }

      std::string type;
      switch (dataType)
      {
        case CAEStreamInfo::STREAM_TYPE_DTSHD:
          type = "dtsHD";
          break;
        case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
          type = "dtsHD MA";
          break;
        case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
          type = "dtsHD (core)";
          break;
        default:
          type = "dts";
          break;
      }

      if (extension)
      {
        switch (ext_type)
        {
          case 0:
            type += " XCH";
            break;
          case 2:
            type += " X96";
            break;
          case 6:
            type += " XXCH";
            break;
          default:
            type += " ext unknown";
            break;
        }
      }

      CLog::Log(LOGINFO,
                "CAEStreamParser::SyncDTS - {} stream detected ({} channels, {}Hz, {}bit {}, "
                "period: {}, syncword: 0x{:x}, target rate: 0x{:x}, framesize {}))",
                type, m_info.m_channels, m_info.m_sampleRate, bits, m_info.m_dataIsLE ? "LE" : "BE",
                m_info.m_dtsPeriod, hd_sync, target_rate, m_fsize);
    }

    return skip;
  }

  // lost sync
  CLog::Log(LOGINFO, "CAEStreamParser::SyncDTS - DTS sync lost");
  m_hasSync = false;
  return skip;
}

inline unsigned int CAEStreamParser::GetTrueHDChannels(const uint16_t chanmap)
{
  int channels = 0;
  for (int i = 0; i < 13; ++i)
    channels += THDChanMap[i] * ((chanmap >> i) & 1);
  return channels;
}

unsigned int CAEStreamParser::SyncTrueHD(uint8_t* data, unsigned int size)
{
  unsigned int left = size;
  unsigned int skip = 0;

  // if MLP
  for (; left; ++skip, ++data, --left)
  {
    // if we dont have sync and there is less the 8 bytes, then break out
    if (!m_hasSync && left < 8)
      return size;

    // if its a major audio unit
    uint16_t length = ((data[0] & 0x0F) << 8 | data[1]) << 1;
    uint32_t syncword = ((((data[4] << 8 | data[5]) << 8) | data[6]) << 8) | data[7];
    if (syncword == 0xf8726fba)
    {
      // we need 32 bytes to sync on a master audio unit
      if (left < 32)
        return skip;

      // get the rate and ensure its valid
      int rate = (data[8] & 0xf0) >> 4;
      if (rate == 0xF)
        continue;

      unsigned int major_sync_size = 28;
      if (data[29] & 1)
      {
        // extension(s) present, look up count
        int extension_count = data[30] >> 4;
        major_sync_size += 2 + extension_count * 2;
      }

      if (left < 4 + major_sync_size)
        return skip;

      // verify the crc of the audio unit
      uint16_t crc = av_crc(m_crcTrueHD, 0, data + 4, major_sync_size - 4);
      crc ^= (data[4 + major_sync_size - 3] << 8) | data[4 + major_sync_size - 4];
      if (((data[4 + major_sync_size - 1] << 8) | data[4 + major_sync_size - 2]) != crc)
        continue;

      // get the sample rate and substreams, we have a valid master audio unit
      m_info.m_sampleRate = (rate & 0x8 ? 44100 : 48000) << (rate & 0x7);
      m_substreams = (data[20] & 0xF0) >> 4;

      // get the number of encoded channels
      uint16_t channel_map = ((data[10] & 0x1F) << 8) | data[11];
      if (!channel_map)
        channel_map = (data[9] << 1) | (data[10] >> 7);
      m_info.m_channels = CAEStreamParser::GetTrueHDChannels(channel_map);

      if (!m_hasSync)
        CLog::Log(LOGINFO,
                  "CAEStreamParser::SyncTrueHD - TrueHD stream detected ({} channels, {}Hz)",
                  m_info.m_channels, m_info.m_sampleRate);

      m_hasSync = true;
      m_fsize = length;
      m_info.m_type = CAEStreamInfo::STREAM_TYPE_TRUEHD;
      m_syncFunc = &CAEStreamParser::SyncTrueHD;
      m_info.m_frameSize = length;
      m_info.m_repeat = 1;
      return skip;
    }
    else
    {
      // we cant sink to a subframe until we have the information from a master audio unit
      if (!m_hasSync)
        continue;

      // if there is not enough data left to verify the packet, just return the skip amount
      if (left < (unsigned int)m_substreams * 4)
        return skip;

      // verify the parity
      int p = 0;
      uint8_t check = 0;
      for (int i = -1; i < m_substreams; ++i)
      {
        check ^= data[p++];
        check ^= data[p++];
        if (i == -1 || data[p - 2] & 0x80)
        {
          check ^= data[p++];
          check ^= data[p++];
        }
      }

      // if the parity nibble does not match
      if ((((check >> 4) ^ check) & 0xF) != 0xF)
      {
        // lost sync
        m_hasSync = false;
        CLog::Log(LOGINFO, "CAEStreamParser::SyncTrueHD - Sync Lost");
        continue;
      }
      else
      {
        m_fsize = length;
        return skip;
      }
    }
  }

  // lost sync
  m_hasSync = false;
  return skip;
}
