/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "guilib/guiinfo/CEGUIInfoRegistry.h"
#include "utils/Base64.h"
#include "utils/TimeUtils.h"

#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include <libavutil/dovi_meta.h>
#include <libavutil/mastering_display_metadata.h>
}

// Live DV/HDR metadata payloads located in the stream by the Amlogic codec and
// published base64 encoded under the player.process(video.sidedata) label.

struct AMLFrameMetadata
{
  std::string doviConfig;
  std::vector<std::string> flags;
  std::string structure;
  std::string doviRpu;
  std::string hdr10pSei;
  std::string cuvaSei;
  std::string hdrMdcv;
  std::string hdrCll;

  bool operator==(const AMLFrameMetadata&) const = default;

  // a frame without a payload holds the last carried one instead of
  // flickering empty
  void Inherit(const AMLFrameMetadata& prev)
  {
    if (doviRpu.empty())
      doviRpu = prev.doviRpu;
    if (hdr10pSei.empty())
      hdr10pSei = prev.hdr10pSei;
    if (cuvaSei.empty())
      cuvaSei = prev.cuvaSei;
    if (hdrMdcv.empty())
      hdrMdcv = prev.hdrMdcv;
    if (hdrCll.empty())
      hdrCll = prev.hdrCll;
  }

  // values are base64 or plain flag tokens, so no JSON escaping is needed
  std::string ComposeSideData() const
  {
    std::string flagsJson;
    for (const auto& flag : flags)
    {
      flagsJson += flagsJson.empty() ? '[' : ',';
      flagsJson += '"';
      flagsJson += flag;
      flagsJson += '"';
    }
    if (!flagsJson.empty())
      flagsJson += ']';

    const std::pair<const char*, const std::string*> entries[] = {
        {"dovi.config", &doviConfig}, {"structure", &structure},
        {"dovi.rpu", &doviRpu},       {"hdr10plus", &hdr10pSei},
        {"cuva", &cuvaSei},           {"mdcv", &hdrMdcv},
        {"cll", &hdrCll}};
    std::string json;
    if (!flagsJson.empty())
    {
      json += "{\"flags\":";
      json += flagsJson;
    }
    for (const auto& [key, value] : entries)
    {
      if (value->empty())
        continue;
      json += json.empty() ? "{\"" : ",\"";
      json += key;
      json += "\":\"";
      json += *value;
      json += '"';
    }
    if (!json.empty())
      json += '}';
    return json;
  }
};

// A stream switch opens the successor codec before the predecessor is closed,
// so clearing is gated on an ownership token instead of happening blindly.
class CAMLFrameMetadataStore
{
public:
  static CAMLFrameMetadataStore& GetInstance()
  {
    static CAMLFrameMetadataStore store;
    return store;
  }

  uint32_t Register()
  {
    std::lock_guard lock(m_lock);
    m_owner = ++m_nextToken;
    m_meta = {};
    return m_owner;
  }

  void Unregister(uint32_t token)
  {
    std::lock_guard lock(m_lock);
    if (m_owner == token)
    {
      m_owner = 0;
      m_meta = {};
    }
  }

  void Publish(uint32_t token, const AMLFrameMetadata& meta)
  {
    std::lock_guard lock(m_lock);
    if (token != 0 && m_owner == token && !(m_meta == meta))
      m_meta = meta;
  }

  AMLFrameMetadata Get() const
  {
    std::lock_guard lock(m_lock);
    return m_meta;
  }

private:
  CAMLFrameMetadataStore() = default;

  mutable std::mutex m_lock;
  AMLFrameMetadata m_meta;
  uint32_t m_owner{0};
  uint32_t m_nextToken{0};
};

// Orders metadata by presentation: committed per pts at decode, released when
// the drain target reaches their pts. All methods run on the VideoPlayerVideo
// thread.
class CAMLFrameMetadataSequencer
{
public:
  void Commit(double pts, const AMLFrameMetadata& meta)
  {
    // a pts far below the newest queued entry means the feed jumped backwards
    // without a flush, and the stranded entries would otherwise win eviction
    if (!m_queue.empty() && pts + BACKWARD_JUMP < m_queue.rbegin()->first)
      m_queue.clear();
    m_queue[pts] = meta;
    if (m_queue.size() > MAX_DEPTH)
      Compact();
    // the oldest entries are the next to be consumed, so overflow drops newest
    while (m_queue.size() > MAX_DEPTH)
      m_queue.erase(std::prev(m_queue.end()));
  }

  // newest entry at or before pts, consuming everything up to it. A miss keeps
  // the queue intact so the caller can hold the last published values.
  bool Consume(double pts, AMLFrameMetadata& meta)
  {
    auto it = m_queue.upper_bound(pts + PTS_TOLERANCE);
    if (it == m_queue.begin())
      return false;
    --it;
    meta = it->second;
    m_queue.erase(m_queue.begin(), std::next(it));
    return true;
  }

  bool Empty() const { return m_queue.empty(); }

  void Reset() { m_queue.clear(); }

private:
  // an entry equal to its pts predecessor can go; the newest stay untouched
  // since a late reordered commit could still land between them
  void Compact()
  {
    if (m_queue.size() <= REORDER_MARGIN)
      return;
    const auto stop = std::prev(m_queue.end(), REORDER_MARGIN);
    for (auto it = std::next(m_queue.begin()); it != stop;)
    {
      if (it->second == std::prev(it)->second)
        it = m_queue.erase(it);
      else
        ++it;
    }
  }

  static constexpr double PTS_TOLERANCE = 1000.0; // DVD_TIME_BASE is microseconds
  static constexpr double BACKWARD_JUMP = 5000000.0;
  // deep enough that after compaction only per frame churn can overflow it
  static constexpr size_t MAX_DEPTH = 512;
  static constexpr size_t REORDER_MARGIN = 64;

  std::map<double, AMLFrameMetadata> m_queue;
};

// one store snapshot per render pass so a consumer cannot mix payloads from
// different video frames, and thread_local avoids a shared cache lock
inline const std::string& AMLGetCachedSideData()
{
  thread_local std::string cached;
  thread_local unsigned int cachedFrameTime = 0;
  thread_local bool cachedOnce = false;

  const unsigned int frameTime = CTimeUtils::GetFrameTime();
  if (!cachedOnce || frameTime != cachedFrameTime)
  {
    cached = CAMLFrameMetadataStore::GetInstance().Get().ComposeSideData();
    cachedFrameTime = frameTime;
    cachedOnce = true;
  }
  return cached;
}

inline bool AMLFrameMetadataGetLabel(std::string& value, int info)
{
  if (static_cast<uint32_t>(info) != CE::GUIINFO::CE_PLAYER_PROCESS_VIDEO_SIDEDATA)
    return false;
  value = AMLGetCachedSideData();
  return true;
}

// dvcC/dvvC box layout per ffmpeg's ff_isom_put_dvcc_dvvc, so add-ons read one
// config layout no matter the container
inline std::string AMLSerializeDoviConfig(const AVDOVIDecoderConfigurationRecord& dovi)
{
  uint8_t out[24]{};
  out[0] = dovi.dv_version_major;
  out[1] = dovi.dv_version_minor;
  out[2] = ((dovi.dv_profile & 0x7f) << 1) | ((dovi.dv_level >> 5) & 0x01);
  out[3] = ((dovi.dv_level & 0x1f) << 3) | (!!dovi.rpu_present_flag << 2) |
           (!!dovi.el_present_flag << 1) | !!dovi.bl_present_flag;
  out[4] = ((dovi.dv_bl_signal_compatibility_id & 0x0f) << 4) |
           ((dovi.dv_md_compression & 0x03) << 2);
  return Base64::Encode(reinterpret_cast<const char*>(out), sizeof(out));
}

// MDCV SEI layout: primaries in green, blue, red order in 1/50000 units,
// luminance in 1/10000 nit units; ffmpeg structs store primaries red first
inline std::string AMLSerializeMastering(const AVMasteringDisplayMetadata& mastering)
{
  uint8_t out[24]{};
  size_t p = 0;
  const auto put16 = [&out, &p](long v)
  {
    out[p++] = (v >> 8) & 0xff;
    out[p++] = v & 0xff;
  };
  if (mastering.has_primaries)
  {
    static constexpr int seiOrder[3] = {1, 2, 0};
    for (const int c : seiOrder)
    {
      put16(std::lround(av_q2d(mastering.display_primaries[c][0]) * 50000.0));
      put16(std::lround(av_q2d(mastering.display_primaries[c][1]) * 50000.0));
    }
    put16(std::lround(av_q2d(mastering.white_point[0]) * 50000.0));
    put16(std::lround(av_q2d(mastering.white_point[1]) * 50000.0));
  }
  else
    p = 16;
  if (mastering.has_luminance)
  {
    const auto put32 = [&out, &p](long v)
    {
      out[p++] = (v >> 24) & 0xff;
      out[p++] = (v >> 16) & 0xff;
      out[p++] = (v >> 8) & 0xff;
      out[p++] = v & 0xff;
    };
    put32(std::lround(av_q2d(mastering.max_luminance) * 10000.0));
    put32(std::lround(av_q2d(mastering.min_luminance) * 10000.0));
  }
  return Base64::Encode(reinterpret_cast<const char*>(out), sizeof(out));
}

// CLL SEI layout: max then frame average light level, 16 bits each
inline std::string AMLSerializeContentLight(const AVContentLightMetadata& light)
{
  const uint8_t out[4] = {
      static_cast<uint8_t>(light.MaxCLL >> 8), static_cast<uint8_t>(light.MaxCLL & 0xff),
      static_cast<uint8_t>(light.MaxFALL >> 8), static_cast<uint8_t>(light.MaxFALL & 0xff)};
  return Base64::Encode(reinterpret_cast<const char*>(out), sizeof(out));
}
