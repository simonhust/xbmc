/*
 *  Copyright (C) 2011-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/StreamDetails.h"

enum AML_SUPPORT_H264_4K2K
{
  AML_SUPPORT_H264_4K2K_UNINIT = -1,
  AML_NO_H264_4K2K,
  AML_HAS_H264_4K2K,
  AML_HAS_H264_4K2K_SAME_PROFILE
};

enum AML_DISPLAY_DV_LED
{
  AML_DV_TV_LED = 0,
  AML_DV_PLAYER_LED
};

#define DV_RGB_444_8BIT     (int)(1<<3)
#define LL_YCbCr_422_12BIT  (int)(1<<5)

#define AML_GXBB    0x1F
#define AML_GXL     0x21
#define AML_GXM     0x22
#define AML_G12A    0x28
#define AML_G12B    0x29
#define AML_SM1     0x2B
#define AML_SC2     0x32
#define AML_T7      0x36
#define AML_S4      0x37
#define AML_S5      0x3E
#define AML_S7D     0x47
#define AML_S6      0x48

int  aml_get_cpufamily_id();
std::string aml_get_cpufamily_name(int cpuid = -1);
bool aml_display_is_widescreen();
bool aml_display_support_dv();
bool aml_display_support_3d();
bool aml_support_hevc();
bool aml_support_hevc_4k2k();
bool aml_support_hevc_8k4k();
bool aml_support_hevc_10bit();
bool aml_support_h266();
AML_SUPPORT_H264_4K2K aml_support_h264_4k2k();
bool aml_support_vp9();
bool aml_support_av1();
bool aml_support_avs2();
bool aml_support_avs3();
bool aml_support_dolby_vision();
bool aml_dolby_vision_enabled();
bool aml_convert_to_dv_by_vs_engine(StreamHdrType hdrType);
void aml_set_hdr_gate(StreamHdrType hdrType, bool hasDv);

/*!
 * \brief Resolved multi HDR stream path.
 *
 * Combines the configured priority (coreelec.amlogic.multihdrstreampriority),
 * the HDR formats actually present in the stream and the VS-Engine
 * conversion settings into a single decision:
 *   - target:    the format selected following the priority order
 *   - removeXxx: SEI stripping flags so the decoder sees a clean
 *                single-format stream (DV RPU is handled by the kernel gate)
 *   - vs10:      the clean single-format stream is handed to DV
 *                (VS-Engine absorbs it)
 */
struct AMLHdrPath
{
  StreamHdrType target = StreamHdrType::HDR_TYPE_NONE;
  bool removeHdr10Plus = false;
  bool removeCuva = false;
  bool vs10 = false;
};

/*!
 * \brief Resolve the HDR handling path for a video stream.
 * \param hasDv Whether the stream contains Dolby Vision (demuxer DOVI side data).
 * \param hasHdr10Plus Whether HDR10+ was detected (demux side data / first SEI).
 * \param hasCuva Whether CUVA HDR Vivid was detected (demux side data / first SEI).
 * \param baseType The demuxer hints HDR type, used when no selectable format
 *                 is present (HDR10/SDR → hdr2dv/sdr2dv conversion).
 */
AMLHdrPath aml_get_hdr_path(bool hasDv, bool hasHdr10Plus, bool hasCuva, StreamHdrType baseType);
bool aml_video_started();
int aml_amdv_wait(StreamHdrType hdrType);
void aml_set_3d_video_mode(unsigned int mode, bool framepacking_support, int view_mode);
