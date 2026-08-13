/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIDialogSelectHdrStream.h"

#include "GUIDialogSelect.h"
#include "ServiceBroker.h"
#include "guilib/GUIWindowManager.h"
#include "utils/StreamDetails.h"
#include "utils/log.h"

#include <string>
#include <vector>

StreamHdrType CGUIDialogSelectHdrStream::ShowDialog(
    const std::vector<StreamHdrType>& availableTypes)
{
  if (availableTypes.empty())
    return StreamHdrType::HDR_TYPE_NONE;

  if (availableTypes.size() == 1)
    return availableTypes[0];

  CGUIDialogSelect* dialog =
      CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
          WINDOW_DIALOG_SELECT);

  if (!dialog)
  {
    CLog::Log(LOGERROR, "CGUIDialogSelectHdrStream::ShowDialog - "
              "Failed to get dialog instance");
    return StreamHdrType::HDR_TYPE_NONE;
  }

  dialog->Reset();
  dialog->SetHeading(39600); // "Select HDR Format"

  for (const auto& type : availableTypes)
  {
    dialog->Add(CStreamDetails::HdrTypeToString(type));
  }

  dialog->SetSelected(0);
  dialog->Open();

  if (dialog->IsConfirmed())
  {
    int selected = dialog->GetSelectedItem();
    if (selected >= 0 && selected < static_cast<int>(availableTypes.size()))
    {
      CLog::Log(LOGINFO, "CGUIDialogSelectHdrStream::ShowDialog - "
                "User selected: {}", CStreamDetails::HdrTypeToString(availableTypes[selected]));
      return availableTypes[selected];
    }
  }

  CLog::Log(LOGINFO, "CGUIDialogSelectHdrStream::ShowDialog - "
            "User cancelled, using first available type");
  return availableTypes[0];
}