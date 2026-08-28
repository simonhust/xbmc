/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "TimingConstants.h"
#include "addons/kodi-dev-kit/include/kodi/c-api/addon-instance/inputstream/demux_packet.h"

#include <string>

#define DMX_SPECIALID_STREAMINFO DEMUX_SPECIALID_STREAMINFO
#define DMX_SPECIALID_STREAMCHANGE DEMUX_SPECIALID_STREAMCHANGE

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

  struct DemuxPacket : DEMUX_PACKET
  {
    DemuxPacket()
    {
      pData = nullptr;
      iSize = 0;
      iStreamId = -1;
      isDualStream = false;
      isELPackage = false;
      isNoElEpMap = false;
      isMultiClip = false;
      isBluray = false;
      isDirectPair = false;
      m_seekTime = DVD_NOPTS_VALUE;
      demuxerId = -1;
      iGroupId = -1;
      subtitlePlane = 0;

      pSideData = nullptr;
      iSideDataElems = 0;

      pts = DVD_NOPTS_VALUE;
      dts = DVD_NOPTS_VALUE;
      duration = 0;
      dispTime = 0;
      recoveryPoint = false;

      subtitlePlane = 0;

      cryptoInfo = nullptr;
    }

    //! @brief PTS offset correction applied to the PTS and DTS.
    double m_ptsOffsetCorrection{0};
    //! @brief Indicate package is from a Dolby Vision dual stream source.
    bool isDualStream;
    //! @brief Indicate package is from a Dolby Vision enhancement layer.
    bool isELPackage;
    //! @brief Indicate the EL stream has no EP_map (no entry points for the DV EL PID).
    bool isNoElEpMap;
    //! @brief Indicate the Blu-ray playlist has multiple clips.
    bool isMultiClip;
    //! @brief Indicate the packet comes from a Blu-ray input.
    bool isBluray;
    //! @brief Indicate non-Bluray dual-stream, use direct dual-queue pairing.
    bool isDirectPair;
    //! @brief Seek target time (DVD_TIME_BASE), used for filtering orphan frames.
    double m_seekTime;
    /// @brief The 3D MVC subtitle plane
    int subtitlePlane;
    //! @brief Live DV/HDR metadata latched by the demuxer from the untouched
    //! payload (base64-encoded, empty when the packet carried none).
    std::string doviRpu;
    std::string hdr10pSei;
    std::string cuvaSei;
    std::string hdrMdcv;
    std::string hdrCll;
  };

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */
