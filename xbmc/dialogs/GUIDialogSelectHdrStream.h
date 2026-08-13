/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/Interface/StreamInfo.h"

#include <vector>

/*!
 * \brief Dialog for selecting HDR stream type in mixed HDR content.
 *
 * When a video file contains multiple HDR formats (e.g., DV + HDR10+ + CUVA),
 * this dialog allows the user to choose which format to play.
 */
class CGUIDialogSelectHdrStream
{
public:
  /*!
   * \brief Show the HDR stream selection dialog and return the user's choice.
   *
   * \param availableTypes List of HDR types available in the current stream.
   * \return The selected HDR type, or HDR_TYPE_NONE if cancelled.
   */
  static StreamHdrType ShowDialog(const std::vector<StreamHdrType>& availableTypes);

private:
  /*!
   * \brief Convert HDR type to display string.
   */
  static std::string HdrTypeToString(StreamHdrType type);
};
