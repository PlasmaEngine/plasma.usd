#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Time/Timestamp.h>

/// \brief Text helpers the USD Live panel's Qt and engine front ends share.
namespace plUsdLiveText
{
  /// Separates the parts of a line of text: " · " in UTF-8.
  constexpr const char* Sep = " \xC2\xB7 ";

  /// "just now", "12 s ago", "3 min ago" and so on; "never" for an invalid time.
  PL_EDITORPLUGINUSD_DLL plString FormatAge(const plTimestamp& time);

  /// Whether a file lies directly in a folder (case-insensitive).
  PL_EDITORPLUGINUSD_DLL bool IsInFolder(plStringView sFile, plStringView sFolder);

  /// Whether two paths name the same file once cleaned (case-insensitive).
  PL_EDITORPLUGINUSD_DLL bool IsSameFile(plStringView sA, plStringView sB);

  /// Participant layers are named "<tool>-<user>.usda", e.g. "blender-sam.usda": the tool's display name and the user.
  PL_EDITORPLUGINUSD_DLL void SplitParticipant(plStringView sLayerFile, plString& out_sTool, plString& out_sUser);
} // namespace plUsdLiveText
