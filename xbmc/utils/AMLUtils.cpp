/*
 *  Copyright (C) 2011-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <fcntl.h>
#include <regex>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "AMLUtils.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "ServiceBroker.h"
#include "utils/RegExp.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "platform/linux/SysfsPath.h"

#include <amcodec/codec.h>

int aml_get_cpufamily_id()
{
  static int aml_cpufamily_id = -1;
  if (aml_cpufamily_id == -1)
  {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::regex re(".*: (.*)$");

    for (std::string line; std::getline(cpuinfo, line);)
    {
      if (line.find("Serial") != std::string::npos)
      {
        std::smatch match;

        if (std::regex_match(line, match, re) && match.size() == 2)
        {
          std::ssub_match value = match[1];
          std::string cpu_family = value.str().substr(0, 2);
          aml_cpufamily_id = std::stoi(cpu_family, nullptr, 16);
          break;
        }
      }
    }
  }
  return aml_cpufamily_id;
}

std::string aml_get_cpufamily_name(int cpuid)
{
  switch(cpuid)
  {
    case AML_G12A:
      return "G12A";
    case AML_G12B:
      return "G12B";
    case AML_SM1:
      return "SM1";
    case AML_SC2:
      return "SC2";
    case AML_T7:
      return "T7";
    case AML_S4:
      return "S4";
    case AML_S5:
      return "S5";
    case AML_S7D:
      return "S7D";
    case AML_S6:
      return "S6";
    default:
      return aml_get_cpufamily_name(aml_get_cpufamily_id());
  }
  return "Unknown";
}

bool aml_display_is_widescreen()
{
  bool is_widescreen = true;
  CSysfsPath edid{"/sys/class/amhdmitx/amhdmitx0/edid"};

  if (edid.Exists())
  {
    std::string valstr = edid.Get<std::string>().value();
    size_t pos = valstr.find("Physical size(mm):");
    if (pos != std::string::npos)
    {
      int width_mm = 0, height_mm = 0;
      sscanf(valstr.c_str() + pos, "Physical size(mm): %d x %d", &width_mm, &height_mm);
      if (width_mm > 0 && height_mm > 0)
      {
          float ratio = static_cast<float>(width_mm) / height_mm;
          // 16:9 range (with some tolerance)
          is_widescreen = (ratio > 1.65f) ? 1 : 0;
          CLog::Log(LOGDEBUG, "AMLUtils: display {} wide screen ({}x{}mm)",
            is_widescreen ? "is" : "is not", width_mm, height_mm);
      }
    }
  }

  return is_widescreen;
}

bool aml_display_support_dv()
{
  static int support_dv = -1;

  if (support_dv == -1)
  {
    CRegExp regexp;
    regexp.RegComp("The Rx don't support DolbyVision");
    std::string valstr;
    CSysfsPath dv_cap{"/sys/devices/virtual/amhdmitx/amhdmitx0/dv_cap"};
    if (dv_cap.Exists())
    {
      valstr = dv_cap.Get<std::string>().value();
      support_dv = (regexp.RegFind(valstr) >= 0) ? 0 : 1;
    }
  }

  return support_dv;
}

bool aml_display_support_3d()
{
  static int support_3d = -1;

  if (support_3d == -1)
  {
    CSysfsPath amhdmitx0_support_3d{"/sys/class/amhdmitx/amhdmitx0/support_3d"};
    if (amhdmitx0_support_3d.Exists())
      support_3d = amhdmitx0_support_3d.Get<int>().value();
    else
      support_3d = 0;

    CLog::Log(LOGDEBUG, "AMLUtils: display support 3D: {}", bool(!!support_3d));
  }

  return (support_3d == 1);
}

static bool aml_support_vcodec_profile(const char *regex)
{
  int profile = 0;
  CRegExp regexp;
  regexp.RegComp(regex);
  std::string valstr;
  CSysfsPath vcodec_profile{"/sys/class/amstream/vcodec_profile"};
  if (vcodec_profile.Exists())
  {
    valstr = vcodec_profile.Get<std::string>().value();
    profile = (regexp.RegFind(valstr) >= 0) ? 1 : 0;
  }

  return profile;
}

bool aml_support_hevc()
{
  static int has_hevc = -1;

  if (has_hevc == -1)
      has_hevc = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):");

  return (has_hevc == 1);
}

bool aml_support_hevc_4k2k()
{
  static int has_hevc_4k2k = -1;

  if (has_hevc_4k2k == -1)
    has_hevc_4k2k = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):(?!\\;).*(4k|8k)");

  return (has_hevc_4k2k == 1);
}

bool aml_support_hevc_8k4k()
{
  static int has_hevc_8k4k = -1;

  if (has_hevc_8k4k == -1)
    has_hevc_8k4k = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):(?!\\;).*8k");

  return (has_hevc_8k4k == 1);
}

bool aml_support_hevc_10bit()
{
  static int has_hevc_10bit = -1;

  if (has_hevc_10bit == -1)
    has_hevc_10bit = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):(?!\\;).*10bit");

  return (has_hevc_10bit == 1);
}

bool aml_support_h266()
{
  static int has_h266 = -1;

  if (has_h266 == -1)
    has_h266 = aml_support_vcodec_profile("\\bh266\\b:");

  return (has_h266 == 1);
}

AML_SUPPORT_H264_4K2K aml_support_h264_4k2k()
{
  static AML_SUPPORT_H264_4K2K has_h264_4k2k = AML_SUPPORT_H264_4K2K_UNINIT;

  if (has_h264_4k2k == AML_SUPPORT_H264_4K2K_UNINIT)
  {
    has_h264_4k2k = AML_NO_H264_4K2K;

    if (aml_support_vcodec_profile("(\\bh264\\b|\\bmh264\\b):4k"))
      has_h264_4k2k = AML_HAS_H264_4K2K_SAME_PROFILE;
    else if (aml_support_vcodec_profile("\\bh264_4k2k\\b:"))
      has_h264_4k2k = AML_HAS_H264_4K2K;
  }
  return has_h264_4k2k;
}

bool aml_support_vp9()
{
  static int has_vp9 = -1;

  if (has_vp9 == -1)
    has_vp9 = aml_support_vcodec_profile("(\\bvp9\\b|\\bvp9_fb\\b):(?!\\;).*compressed");

  return (has_vp9 == 1);
}

bool aml_support_av1()
{
  static int has_av1 = -1;

  if (has_av1 == -1)
    has_av1 = aml_support_vcodec_profile("(\\bav1\\b|\\bav1_fb\\b):(?!\\;).*compressed");

  return (has_av1 == 1);
}

bool aml_support_avs2()
{
  static int has_avs2 = -1;

  if (has_avs2 == -1)
    has_avs2 = aml_support_vcodec_profile("(\\bavs2\\b|\\bavs2_fb\\b):(?!\\;).*compressed");

  return (has_avs2 == 1);
}

bool aml_support_avs3()
{
  static int has_avs3 = -1;

  if (has_avs3 == -1)
    has_avs3 = aml_support_vcodec_profile("\\bavs3\\b:(?!\\;).*compressed");

  return (has_avs3 == 1);
}

bool aml_support_dolby_vision()
{
  static int support_dv = -1;

  if (support_dv == -1)
  {
    CSysfsPath support_info{"/sys/class/amdolby_vision/support_info"};
    support_dv = 0;
    if (support_info.Exists())
    {
      support_dv = (int)((support_info.Get<int>().value() & 7) == 7);
      if (support_dv == 1) {
        CSysfsPath ko_info{"/sys/class/amdolby_vision/ko_info"};
        if (ko_info.Exists())
          CLog::Log(LOGINFO, "Amlogic Dolby Vision info: {}", ko_info.Get<std::string>().value().c_str());
      }
    }
  }

  return (support_dv == 1);
}

bool aml_dolby_vision_enabled()
{
  static int dv_enabled = -1;
  bool dv_user_enabled(!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE));

  if (dv_enabled == -1)
    dv_enabled = (!!aml_support_dolby_vision() && !!aml_display_support_dv());

  return ((dv_enabled && !!dv_user_enabled) == 1);
}

bool aml_convert_to_dv_by_vs_engine(StreamHdrType hdrType)
{
  static int convert_to_dv = -1;
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  bool dv_user_enabled(!settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE));
  bool user_convert_to_dv;

  if (hdrType == StreamHdrType::HDR_TYPE_NONE)
    user_convert_to_dv = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_SDR2DV);
  else if (hdrType == StreamHdrType::HDR_TYPE_HDR10PLUS)
    user_convert_to_dv = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR10PLUS2DV);
  else if (hdrType == StreamHdrType::HDR_TYPE_HDRVIVID)
    user_convert_to_dv = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_CUVA2DV);
  else
    user_convert_to_dv = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR2DV);

  if (convert_to_dv == -1)
    convert_to_dv = (!!aml_support_dolby_vision() && !!aml_display_support_dv());

  return ((convert_to_dv && !!user_convert_to_dv && !!dv_user_enabled) == 1);
}

AMLHdrPath aml_get_hdr_path(bool hasDv, bool hasHdr10Plus, bool hasCuva, StreamHdrType baseType)
{
  AMLHdrPath path;

  // When the stream itself carries Dolby Vision, source-side conversion
  // (vs10) is disabled entirely: it would conflict with the priority
  // selection (e.g. HDR10+ preferred while the DV RPU stays in the stream
  // and hdr10plus2dv is also enabled) and it is meaningless anyway, since
  // DV content is handled natively by the kernel. vs10 only serves DV-free
  // sources.
  const bool vs10Allowed = !hasDv;

  // Priority orders:
  //   DV:     DV > HDR10+ > CUVA
  //   HDR10+: HDR10+ > DV > CUVA
  //   CUVA:   CUVA > DV > HDR10+
  const int priority = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      CSettings::SETTING_COREELEC_AMLOGIC_MULTI_HDR_PRIORITY);

  std::vector<StreamHdrType> order;
  switch (priority)
  {
    case 1: // HDR10+
      order = {StreamHdrType::HDR_TYPE_HDR10PLUS, StreamHdrType::HDR_TYPE_DOLBYVISION,
               StreamHdrType::HDR_TYPE_HDRVIVID};
      break;
    case 2: // CUVA
      order = {StreamHdrType::HDR_TYPE_HDRVIVID, StreamHdrType::HDR_TYPE_DOLBYVISION,
               StreamHdrType::HDR_TYPE_HDR10PLUS};
      break;
    default: // DV
      order = {StreamHdrType::HDR_TYPE_DOLBYVISION, StreamHdrType::HDR_TYPE_HDR10PLUS,
               StreamHdrType::HDR_TYPE_HDRVIVID};
      break;
  }

  for (const auto& type : order)
  {
    const bool present = type == StreamHdrType::HDR_TYPE_DOLBYVISION
                             ? hasDv
                             : type == StreamHdrType::HDR_TYPE_HDR10PLUS ? hasHdr10Plus
                                                                         : hasCuva;
    if (present)
    {
      path.target = type;
      break;
    }
  }

  if (path.target == StreamHdrType::HDR_TYPE_NONE)
  {
    // No selectable format present. DV is always part of the priority order,
    // so a DV source can never reach here (hasDv == false by construction),
    // which also means vs10 is never disabled on this path: plain
    // HDR10/SDR/HLG streams convert to DV purely by their own settings.
    path.vs10 = aml_convert_to_dv_by_vs_engine(baseType);
    return path;
  }

  // Strip the SEI of non-target formats so the decoder only sees the
  // selected one. DV RPU cannot be stripped here, it goes to the gate.
  path.removeHdr10Plus = path.target != StreamHdrType::HDR_TYPE_HDR10PLUS;
  path.removeCuva = path.target != StreamHdrType::HDR_TYPE_HDRVIVID;

  // vs10 policy: when the user enabled VS-Engine conversion for the target
  // format, hand the clean single-format stream to DV.
  switch (path.target)
  {
    case StreamHdrType::HDR_TYPE_HDRVIVID:
      if (vs10Allowed && aml_convert_to_dv_by_vs_engine(StreamHdrType::HDR_TYPE_HDRVIVID))
      {
        // VS-Engine consumes the HDR10 base: strip the CUVA SEI too.
        path.removeCuva = true;
        path.vs10 = true;
      }
      break;
    case StreamHdrType::HDR_TYPE_HDR10PLUS:
      // Kernel absorbs HDR10+ (gate writes HDRP_BY_DV); open as DV so the
      // decoder enables the kernel DV path.
      path.vs10 = vs10Allowed && aml_convert_to_dv_by_vs_engine(StreamHdrType::HDR_TYPE_HDR10PLUS);
      break;
    case StreamHdrType::HDR_TYPE_DOLBYVISION:
    default:
      break;
  }

  return path;
}

void aml_set_hdr_gate(StreamHdrType hdrType, bool hasDv)
{
  CLog::Log(LOGINFO, "AMLUtils::{} - Setting HDR gate for type: {}",
            __FUNCTION__, static_cast<int>(hdrType));

  switch (hdrType)
  {
    case StreamHdrType::HDR_TYPE_DOLBYVISION:
      // DV: AMLCodec already enabled DV, ensure HDR10+ policy is correct.
      // Also ensure dolby_vision_enable=1 (critical for VS10-converted
      // content where hdrType becomes DOLBYVISION: the gate was called
      // with the unconverted type and may have written enable=0).
      // HDR10+ is NOT absorbed (bit 2 clear), goes to VPP HDR2 core.
      // HDR_BY_DV_F_SRC(0x02) | HLG_BY_DV_F_SRC(0x10) | SDR_BY_DV_F_SRC(0x40)
      CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_enable", 1);
      CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_hdr10_policy", 0x52);
      break;

    case StreamHdrType::HDR_TYPE_HDR10PLUS:
    {
      // Check if user wants HDR10+ to be converted to DV via VS-Engine.
      const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
      bool dv_user_enabled = !settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE);
      bool hdr10plus2dv = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR10PLUS2DV);
      bool device_support_dv = aml_support_dolby_vision() && aml_display_support_dv();

      // Absorb HDR10+ into DV only for DV-free sources: when the stream
      // itself carries DV the priority selection rules and conversion is
      // meaningless (and would fight the native DV RPU).
      if (!hasDv && dv_user_enabled && hdr10plus2dv && device_support_dv)
      {
        // hdr10plus2dv enabled: let DV absorb HDR10+.
        static constexpr unsigned int HDRP_BY_DV = 0x4;
        static constexpr unsigned int DV_HDR10_POLICY_DEFAULT = 0x52;
        CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_enable", 1);
        CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_hdr10_policy",
                   static_cast<int>(DV_HDR10_POLICY_DEFAULT | HDRP_BY_DV));
        CLog::Log(LOGINFO, "AMLUtils::{} - HDR10+ will be converted to DV (VS-Engine)",
                  __FUNCTION__);
      }
      else
      {
        // hdr10plus2dv disabled: let HDR10+ go to VPP HDR2 core.
        CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_enable", 0);
        CSysfsPath("/sys/class/amvecm/enable_hdr10plus", 1);
      }
      break;
    }

    case StreamHdrType::HDR_TYPE_HDRVIVID:
      // CUVA HDR Vivid: Goes through VPP HDR2 core.
      CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_enable", 0);
      break;

    case StreamHdrType::HDR_TYPE_HDR10:
      // HDR10: Standard HDR10 via VPP HDR2 core.
      CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_enable", 0);
      break;

    case StreamHdrType::HDR_TYPE_HLG:
      CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_enable", 0);
      break;

    case StreamHdrType::HDR_TYPE_NONE:
    default:
      CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_enable", 0);
      break;
  }
}

bool aml_video_started()
{
  CSysfsPath videostarted{"/sys/class/tsync/videostarted"};
  return (StringUtils::EqualsNoCase(videostarted.Get<std::string>().value(), "0x1"));
}

int aml_amdv_wait(StreamHdrType hdrType)
{
  if (hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
  {
    CSysfsPath amdv_wait_delay{"/sys/module/aml_media/parameters/amdv_wait_delay"};
    return amdv_wait_delay.Get<int>().value();
  }
  else
    return 0;
}

void aml_set_3d_video_mode(unsigned int mode, bool framepacking_support, int view_mode)
{
  int fd;
  if ((fd = open("/dev/amvideo", O_RDWR)) >= 0)
  {
    if (ioctl(fd, AMSTREAM_IOC_SET_3D_TYPE, mode) != 0)
      CLog::Log(LOGERROR, "AMLUtils::{} - unable to set 3D video mode 0x%x", __FUNCTION__, mode);
    close(fd);

    CSysfsPath("/sys/module/aml_media/parameters/g_framepacking_support", framepacking_support ? 1 : 0);
    CSysfsPath("/sys/module/amvdec_h264mvc/parameters/view_mode", view_mode);
  }
}
