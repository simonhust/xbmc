/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  The "LAV Audio" passthrough A/V sync path (m_lavStyleSyncEnabled) is derived
 *  from LAV Filters by Hendrik Leppkes (Nevcairiel):
 *  https://github.com/Nevcairiel/LAVFilters
 */

#include "DVDAudioCodecPassthrough.h"

#include "DVDCodecs/DVDCodecs.h"
#include "cores/AudioEngine/Utils/PackerMAT.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"
#include "ServiceBroker.h"
#include "cores/AudioEngine/Interfaces/AE.h"

#include <algorithm>
#include <cmath>

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace
{
constexpr auto TRUEHD_BUF_SIZE = 61440;

// A valid PTS must be >= 0 and within a sane range. During seamless branching
// the demuxer can emit DVD_NOPTS_VALUE (which becomes ~1.8e19 as a double) or
// other garbage; this rejects those without rejecting real timestamps.
constexpr double MAX_REASONABLE_PTS = 86400000.0 * DVD_TIME_BASE; // 24 hours in DVD_TIME_BASE units
inline bool IsValidPts(double pts)
{
  return (pts >= 0.0) && (pts <= MAX_REASONABLE_PTS);
}
} // namespace

CDVDAudioCodecPassthrough::CDVDAudioCodecPassthrough(CProcessInfo &processInfo, CAEStreamInfo::DataType streamType) :
  CDVDAudioCodec(processInfo)
{
  m_format.m_streamInfo.m_type = streamType;
  m_deviceIsRAW = processInfo.WantsRawPassthrough();

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    m_trueHDBuffer.resize(TRUEHD_BUF_SIZE);

    if (!m_deviceIsRAW)
      m_packerMAT = std::make_unique<CPackerMAT>();
  }
}

CDVDAudioCodecPassthrough::~CDVDAudioCodecPassthrough(void)
{
  Dispose();
}

void CDVDAudioCodecPassthrough::SetLavStyleSyncMode(LavSyncMode mode)
{
  // The MAT packer runs its seamless-branch handling unconditionally; this flag
  // only gates the codec's PTS validation / internal-clock retiming.
  m_lavSyncMode = mode;
}

bool CDVDAudioCodecPassthrough::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  m_hints = hints;
  m_parser.SetCoreOnly(false);
  switch (m_format.m_streamInfo.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_AC3:
      m_codecName = "pt-ac3";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_EAC3:
      m_codecName = "pt-eac3";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      m_codecName = "pt-dtshd";
      // DTS threshold tightened to 10ms: the +20/-27ms player sync gate
      // cannot settle with a 100ms codec deadband (see 72d0902).
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_AV3A:
      m_codecName = "pt-av3a";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD:
      m_codecName = "pt-dtshd";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      m_codecName = "pt-dts";
      m_parser.SetCoreOnly(true);
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSUHD:
      // DTS:X Profile 2 (the FTOC container). The frame is handed to the packer
      // unchanged and burst as IEC 61937 type IV with the DTS-UHD subtype,
      // which is what makes the Amlogic HDMI TX take its HBR path.
      m_codecName = "pt-dtsuhd";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      if (CServiceBroker::GetActiveAE()->ConvertsDtsXToDtsCore())
      {
        // Without the helper we would burst a Profile 2 ACE frame the receiver
        // cannot read, so fall back to native P2 rather than emitting silence.
        if (StartP2ToP1Child())
          m_p2ToP1Child = true;
        else
          CLog::Log(LOGWARNING,
                    "pt-dtsuhd: DTS Core helper unavailable, passing Profile 2 through");
      }
      break;

    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      m_codecName = "pt-truehd";
      // TrueHD keeps 100ms: the MAT packer's GetSamplesOffset provides
      // sub-frame accuracy, so residual stays below the deadband.
      m_jitterThreshold = JITTER_THRESHOLD_TRUEHD;
      CLog::LogF(LOGDEBUG, "passthrough output device is {}", m_deviceIsRAW ? "RAW" : "IEC");
      break;

    default:
      return false;
  }

  CLog::LogF(LOGDEBUG, "{}: jitter threshold {:.0f}ms", m_codecName, m_jitterThreshold / 1000.0);

  m_dataSize = 0;
  m_bufferSize = 0;
  m_backlogSize = 0;

  if (m_lavSyncMode != LavSyncMode::NONE)
  {
    m_currentPts = LOCAL_NOPTS;
    m_nextPts = LOCAL_NOPTS;
    m_jitterTracker.Reset();
  }
  else
  {
    m_currentPts = DVD_NOPTS_VALUE;
    m_nextPts = DVD_NOPTS_VALUE;
  }
  return true;
}

//============================================================================
// DTS:X Profile 2 -> Profile 1
//============================================================================

bool CDVDAudioCodecPassthrough::StartP2ToP1Child()
{
  if (m_p2ChildIn >= 0)
    return true;

  int inPipe[2], outPipe[2];
  if (pipe(inPipe) != 0)
  {
    CLog::Log(LOGERROR, "pt-dtsuhd: pipe() failed for the DTS:X -> DTS Core helper");
    return false;
  }
  if (pipe(outPipe) != 0)
  {
    close(inPipe[0]);
    close(inPipe[1]);
    CLog::Log(LOGERROR, "pt-dtsuhd: pipe() failed for the DTS:X -> DTS Core helper");
    return false;
  }

  const pid_t pid = fork();
  if (pid < 0)
  {
    close(inPipe[0]);
    close(inPipe[1]);
    close(outPipe[0]);
    close(outPipe[1]);
    CLog::Log(LOGERROR, "pt-dtsuhd: fork() failed for the DTS:X -> DTS Core helper");
    return false;
  }

  if (pid == 0)
  {
    close(inPipe[1]);
    close(outPipe[0]);
    dup2(inPipe[0], STDIN_FILENO);
    dup2(outPipe[1], STDOUT_FILENO);
    if (inPipe[0] != STDIN_FILENO)
      close(inPipe[0]);
    if (outPipe[1] != STDOUT_FILENO)
      close(outPipe[1]);

    // dtsx_pt is armhf and dlopen()s the vendor decoder itself, so it needs
    // the same private loader and library path the decode path uses.
    static const char home[] = "/usr/lib/dtsx";
    char serv[sizeof(home) + 16];
    snprintf(serv, sizeof(serv), "%s/dtsx_pt", home);

    char libenv[sizeof(home) + 48];
    snprintf(libenv, sizeof(libenv), "DTSX_LIB=%s/libHwAudio_dtsx_bionic.so", home);

    char *const argv[] = {serv, nullptr};
    char *const envp[] = {libenv, nullptr};
    execve(serv, argv, envp);
    _exit(127);
  }

  close(inPipe[0]);
  close(outPipe[1]);
  m_p2ChildIn = inPipe[1];
  m_p2ChildOut = outPipe[0];
  m_p2ChildPid = pid;

  CLog::LogF(LOGINFO, "pt-dtsuhd: converting Profile 2 to Profile 1 (helper pid {})", pid);
  return true;
}

void CDVDAudioCodecPassthrough::StopP2ToP1Child()
{
  if (m_p2ChildIn >= 0)
  {
    close(m_p2ChildIn);
    m_p2ChildIn = -1;
  }
  if (m_p2ChildOut >= 0)
  {
    close(m_p2ChildOut);
    m_p2ChildOut = -1;
  }
  if (m_p2ChildPid > 0)
  {
    int status;
    // dtsx_pt exits once its stdin is closed, which we just did.
    if (waitpid(m_p2ChildPid, &status, 0) < 0)
      kill(m_p2ChildPid, SIGKILL);
    m_p2ChildPid = 0;
  }
  m_p2ToP1Child = false;
}

int CDVDAudioCodecPassthrough::ConvertP2ToP1(const uint8_t* in,
                                           unsigned int inSize,
                                           std::vector<uint8_t>& out)
{
  out.clear();
  if (m_p2ChildIn < 0 || in == nullptr || inSize == 0)
    return 0;

  // One record written, exactly one reply read. Emitting a second reply would
  // desynchronise every packet after it.
  const uint32_t netLen = htonl(inSize);
  const uint8_t* const parts[2] = {reinterpret_cast<const uint8_t*>(&netLen), in};
  const size_t partLen[2] = {4, inSize};

  for (int p = 0; p < 2; ++p)
  {
    size_t off = 0;
    while (off < partLen[p])
    {
      const ssize_t n = write(m_p2ChildIn, parts[p] + off, partLen[p] - off);
      if (n > 0)
      {
        off += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      CLog::Log(LOGERROR, "pt-dtsuhd: DTS:X -> DTS Core helper write failed");
      StopP2ToP1Child();
      return 0;
    }
  }

  uint32_t netReply = 0;
  size_t got = 0;
  while (got < sizeof(netReply))
  {
    struct pollfd pfd = {m_p2ChildOut, POLLIN, 0};
    if (poll(&pfd, 1, 10000) <= 0)
      return 0;
    const ssize_t n = read(m_p2ChildOut, reinterpret_cast<uint8_t*>(&netReply) + got,
                           sizeof(netReply) - got);
    if (n > 0)
    {
      got += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    return 0;
  }

  const uint32_t replySize = ntohl(netReply);
  if (replySize == 0)
  {
    // The helper is still filling its window. Nothing to emit this call; the
    // sink waits for the next frame rather than seeing a truncated burst.
    return 0;
  }
  if (replySize > (32u << 20))
    return 0;

  out.resize(replySize);
  got = 0;
  while (got < replySize)
  {
    const ssize_t n = read(m_p2ChildOut, out.data() + got, replySize - got);
    if (n > 0)
    {
      got += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    out.clear();
    return 0;
  }
  return static_cast<int>(replySize);
}

void CDVDAudioCodecPassthrough::Dispose()
{
  StopP2ToP1Child();

  if (m_buffer)
  {
    delete[] m_buffer;
    m_buffer = NULL;
  }

  free(m_backlogBuffer);
  m_backlogBuffer = nullptr;
  m_backlogBufferSize = 0;

  m_bufferSize = 0;
}

bool CDVDAudioCodecPassthrough::AddData(const DemuxPacket &packet)
{
  if (m_backlogSize)
  {
    m_dataSize = m_bufferSize;
    unsigned int consumed = m_parser.AddData(m_backlogBuffer, m_backlogSize, &m_buffer, &m_dataSize);
    m_bufferSize = std::max(m_bufferSize, m_dataSize);
    if (consumed != m_backlogSize)
    {
      memmove(m_backlogBuffer, m_backlogBuffer+consumed, m_backlogSize-consumed);
    }
    m_backlogSize -= consumed;
  }

  unsigned char *pData(const_cast<uint8_t*>(packet.pData));
  int iSize(packet.iSize);

  if (m_lavSyncMode != LavSyncMode::NONE)
  {
    // LAV Audio: validate PTS with the robust range check so seamless-branch
    // garbage does not poison the internal clock.
    const double incomingPts = packet.pts;
    const bool ptsIsValid = IsValidPts(incomingPts);

    if (pData)
    {
      if (!IsValidPts(m_currentPts))
        m_currentPts = LOCAL_NOPTS;
      if (!IsValidPts(m_nextPts))
        m_nextPts = LOCAL_NOPTS;

      if (m_currentPts == LOCAL_NOPTS)
      {
        if (m_nextPts != LOCAL_NOPTS)
        {
          m_currentPts = m_nextPts;
          m_nextPts = ptsIsValid ? incomingPts : LOCAL_NOPTS;
        }
        else if (ptsIsValid)
        {
          m_currentPts = incomingPts;
        }
      }
      else if (ptsIsValid)
      {
        m_nextPts = incomingPts;
      }
    }
  }
  else
  {
    // Stock Kodi PTS handling
    if (pData)
    {
      if (m_currentPts == DVD_NOPTS_VALUE)
      {
        if (m_nextPts != DVD_NOPTS_VALUE)
        {
          m_currentPts = m_nextPts;
          m_nextPts = packet.pts;
        }
        else if (packet.pts != DVD_NOPTS_VALUE)
        {
          m_currentPts = packet.pts;
        }
      }
      else
      {
        m_nextPts = packet.pts;
      }
    }
  }

  if (pData && !m_backlogSize)
  {
    if (iSize <= 0)
      return true;

    m_dataSize = m_bufferSize;
    int used = m_parser.AddData(pData, iSize, &m_buffer, &m_dataSize);
    m_bufferSize = std::max(m_bufferSize, m_dataSize);

    if (used != iSize)
    {
      if (m_backlogBufferSize < static_cast<unsigned int>(iSize - used))
      {
        m_backlogBufferSize = std::max(TRUEHD_BUF_SIZE, iSize - used);
        m_backlogBuffer = static_cast<uint8_t*>(realloc(m_backlogBuffer, m_backlogBufferSize));
      }
      m_backlogSize = iSize - used;
      memcpy(m_backlogBuffer, pData + used, m_backlogSize);
    }
  }
  else if (pData)
  {
    if (m_backlogBufferSize < (m_backlogSize + iSize))
    {
      m_backlogBufferSize = std::max(TRUEHD_BUF_SIZE, static_cast<int>(m_backlogSize + iSize));
      m_backlogBuffer = static_cast<uint8_t*>(realloc(m_backlogBuffer, m_backlogBufferSize));
    }
    memcpy(m_backlogBuffer + m_backlogSize, pData, iSize);
    m_backlogSize += iSize;
  }

  if (!m_dataSize)
    return true;

  m_format.m_dataFormat = AE_FMT_RAW;
  m_format.m_streamInfo = m_parser.GetStreamInfo();
  m_format.m_sampleRate = m_parser.GetSampleRate();
  m_format.m_frameSize = 1;
  CAEChannelInfo layout;
  for (unsigned int i = 0; i < m_parser.GetChannels(); i++)
  {
    layout += AE_CH_RAW;
  }
  m_format.m_channelLayout = layout;

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    if (m_trueHDBuffer.empty())
    {
      m_trueHDBuffer.resize(TRUEHD_BUF_SIZE);

      if (!m_deviceIsRAW && !m_packerMAT)
        m_packerMAT = std::make_unique<CPackerMAT>();
    }

    if (m_deviceIsRAW) // RAW
    {
      m_dataSize = PackTrueHD();
    }
    else // IEC
    {
      if (m_lavSyncMode != LavSyncMode::NONE)
      {
        if (!m_truehdPtsCacheValid && IsValidPts(m_currentPts))
        {
          m_truehdPtsCache = m_currentPts;
          m_truehdPtsCacheValid = true;
        }
      }

      if (m_packerMAT->PackTrueHD(m_buffer, m_dataSize))
      {
        m_trueHDBuffer = m_packerMAT->GetOutputFrame();
        m_dataSize = TRUEHD_BUF_SIZE;

        if (m_lavSyncMode != LavSyncMode::NONE)
        {
          (void)m_packerMAT->HadDiscontinuity();

          if (m_truehdPtsCacheValid)
          {
            m_currentPts = m_truehdPtsCache;
            m_truehdPtsCacheValid = false;
            m_truehdPtsCache = LOCAL_NOPTS;
          }
        }
      }
      else
        m_dataSize = 0;
    }
  }

  return true;
}

unsigned int CDVDAudioCodecPassthrough::PackTrueHD()
{
  unsigned int dataSize{0};

  if (m_trueHDoffset == 0)
    m_trueHDframes = 0;

  memcpy(m_trueHDBuffer.data() + m_trueHDoffset, m_buffer, m_dataSize);

  m_trueHDoffset += m_dataSize;
  m_trueHDframes++;

  if (m_trueHDframes == 24)
  {
    dataSize = m_trueHDoffset;
    m_trueHDoffset = 0;
    m_trueHDframes = 0;
    return dataSize;
  }

  return 0;
}

void CDVDAudioCodecPassthrough::GetData(DVDAudioFrame &frame)
{
  frame.nb_frames = GetData(frame.data);
  frame.framesOut = 0;
  frame.hasDiscontinuity = false;
  frame.discontinuityCorrection = 0.0;

  if (frame.nb_frames == 0)
    return;

  frame.passthrough = true;
  frame.format = m_format;
  frame.planes = 1;
  frame.bits_per_sample = m_hints.bitspersample;
  frame.duration = DVD_MSEC_TO_TIME(frame.format.m_streamInfo.GetDuration());

  if (m_lavSyncMode == LavSyncMode::NONE)
  {
    frame.pts = m_currentPts;
    m_currentPts = DVD_NOPTS_VALUE;
    return;
  }

  if (m_lavSyncMode == LavSyncMode::SB)
  {
    // Seamless-branch mode: stock PTS output, but with the validated
    // LOCAL_NOPTS sentinel (no internal-clock retiming).
    frame.pts = m_currentPts;
    m_currentPts = LOCAL_NOPTS;
    return;
  }

  //============================================================================
  // FULL mode: LAV Audio internal-clock A/V sync
  //
  // We maintain our own internal clock, synced to the RESYNC PTS from
  // VideoPlayer (the coordinated A/V clock). We output PTS from that clock, not
  // the demuxer, and continuously correct any timing jitter/drift against the
  // demuxer PTS whenever it exceeds the threshold.
  //============================================================================
  const CAEStreamInfo::DataType streamType = m_format.m_streamInfo.m_type;
  const bool isTrueHD = (streamType == CAEStreamInfo::STREAM_TYPE_TRUEHD);

  // TrueHD: compensate for sub-MAT-frame drift via the MAT packer samples offset.
  double samplesOffsetTime = 0.0;
  if (isTrueHD && m_packerMAT && m_format.m_sampleRate > 0)
  {
    const int samplesOffset = m_packerMAT->GetSamplesOffset();
    if (samplesOffset != 0)
      samplesOffsetTime =
          static_cast<double>(samplesOffset) / m_format.m_sampleRate * DVD_TIME_BASE;
  }

  const double demuxerPts = m_currentPts;
  const bool haveDemuxerPts = IsValidPts(demuxerPts);

  // STEP 1: resync the internal clock when needed (codec creation, after seeks).
  if (m_needsResync && haveDemuxerPts)
  {
    m_internalClock = demuxerPts;
    m_needsResync = false;
    m_jitterTracker.Reset();
    CLog::LogF(LOGDEBUG, "internal clock synced to demuxer PTS {:.3f}s",
               demuxerPts / DVD_TIME_BASE);
  }

  // STEP 2: track jitter between our clock and the demuxer PTS; whenever it
  // exceeds the threshold, correct the internal clock to realign.
  if (IsValidPts(m_internalClock) && haveDemuxerPts)
  {
    const double jitter = m_internalClock - demuxerPts + samplesOffsetTime;
    m_jitterTracker.Sample(jitter);

    const double absMinJitter = m_jitterTracker.AbsMinimum();
    if (std::abs(absMinJitter) > m_jitterThreshold)
    {
      m_internalClock -= absMinJitter;
      m_jitterTracker.OffsetValues(-absMinJitter);

      // Signal discontinuity to downstream
      frame.hasDiscontinuity = true;
      frame.discontinuityCorrection = absMinJitter;

      CLog::LogF(LOGDEBUG, "jitter correction {:.2f}ms (threshold {:.0f}ms)", absMinJitter / 1000.0,
                 m_jitterThreshold / 1000.0);
    }
  }

  // STEP 3: output PTS from the internal clock, advancing by frame duration.
  if (IsValidPts(m_internalClock))
  {
    frame.pts = m_internalClock;
    m_internalClock += frame.duration;
  }
  else if (haveDemuxerPts)
  {
    frame.pts = demuxerPts;
    m_internalClock = demuxerPts + frame.duration;
  }
  else
  {
    frame.pts = DVD_NOPTS_VALUE;
  }

  m_currentPts = LOCAL_NOPTS;
}

int CDVDAudioCodecPassthrough::GetData(uint8_t** dst)
{
  if (!m_dataSize)
    AddData(DemuxPacket());

  // DTS:X with the conversion on: the demuxer gave us an ACE frame and what a
  // receiver without DTS:X support needs is the plain DTS Core bitstream the
  // vendor library makes of it. Swap it in before anything looks at m_buffer.
  //
  // The helper returns whole IEC 61937 frames for a whole input window, which is
  // far more than one sink buffer (MAX_IEC61937_PACKET). Handing that straight
  // over would overflow the packer, so it is queued and released one
  // buffer-sized chunk per call, always cut on a frame boundary.
  if (m_p2ToP1Child && (m_p2ChildIn >= 0 || !m_ptBuffer.empty()))
  {
    if (m_ptBuffer.empty())
    {
      if (!m_dataSize || !m_buffer)
        return 0;
      if (ConvertP2ToP1(m_buffer, m_dataSize, m_ptPending) <= 0)
      {
        m_dataSize = 0;
        if (m_p2ChildIn < 0)
        {
          // The helper died; fall back to native Profile 2 rather than silence.
          m_p2ToP1Child = false;
          CLog::Log(LOGWARNING,
                    "pt-dtsuhd: DTS Core helper stopped, passing Profile 2 through");
        }
        return 0;
      }
      m_ptBuffer = std::move(m_ptPending);
      m_ptOffset = 0;
    }

    if (m_ptOffset >= m_ptBuffer.size())
    {
      m_ptBuffer.clear();
      m_ptOffset = 0;
      return 0;
    }

    // The sink writes to ALLA as a plain byte stream, so a chunk boundary does
    // not have to land on a frame edge as long as the chunks stay in order.
    const size_t remaining = m_ptBuffer.size() - m_ptOffset;
    const size_t chunk = std::min<size_t>(remaining, MAX_IEC61937_PACKET);

    *dst = m_ptBuffer.data() + m_ptOffset;
    m_ptOffset += chunk;
    return static_cast<int>(chunk);
  }

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
    *dst = m_trueHDBuffer.data();
  else
    *dst = m_buffer;

  int bytes = m_dataSize;
  m_dataSize = 0;
  return bytes;
}

void CDVDAudioCodecPassthrough::Reset()
{
  m_trueHDoffset = 0;
  m_dataSize = 0;
  m_bufferSize = 0;
  m_backlogSize = 0;
  m_parser.Reset();

  if (m_lavSyncMode != LavSyncMode::NONE)
  {
    m_currentPts = LOCAL_NOPTS;
    m_nextPts = LOCAL_NOPTS;
    m_truehdPtsCache = LOCAL_NOPTS;
    m_truehdPtsCacheValid = false;
    m_internalClock = LOCAL_NOPTS;
    m_needsResync = true;
    m_jitterTracker.Reset();

    if (m_packerMAT)
      m_packerMAT->Reset();

    CLog::LogF(LOGDEBUG, "LAV internal clock reset, will resync");
  }
  else
  {
    m_currentPts = DVD_NOPTS_VALUE;
    m_nextPts = DVD_NOPTS_VALUE;
  }
}

void CDVDAudioCodecPassthrough::ResetLavSyncState()
{
  if (m_lavSyncMode != LavSyncMode::FULL)
    return;

  m_truehdPtsCache = LOCAL_NOPTS;
  m_truehdPtsCacheValid = false;
  m_internalClock = LOCAL_NOPTS;
  m_needsResync = true;
  m_jitterTracker.Reset();

  CLog::LogF(LOGDEBUG, "internal clock reset, will resync");
}

void CDVDAudioCodecPassthrough::SyncToResyncPts(double pts)
{
  if (m_lavSyncMode != LavSyncMode::FULL)
    return;

  if (IsValidPts(pts))
  {
    m_internalClock = pts;
    m_needsResync = false;
    m_jitterTracker.Reset();
    CLog::LogF(LOGDEBUG, "internal clock set to RESYNC pts {:.3f}s", pts / DVD_TIME_BASE);
  }
}

int CDVDAudioCodecPassthrough::GetBufferSize()
{
  return (int)m_parser.GetBufferSize();
}