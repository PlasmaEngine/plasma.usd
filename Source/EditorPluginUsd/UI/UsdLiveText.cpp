#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/UI/UsdLiveText.h>
#include <Foundation/Strings/StringBuilder.h>

plString plUsdLiveText::FormatAge(const plTimestamp& time)
{
  if (!time.IsValid())
    return "never";

  const double fSeconds = (plTimestamp::CurrentTimestamp() - time).GetSeconds();

  plStringBuilder s;
  if (fSeconds < 5.0)
    s = "just now";
  else if (fSeconds < 60.0)
    s.SetFormat("{} s ago", (int)fSeconds);
  else if (fSeconds < 3600.0)
    s.SetFormat("{} min ago", (int)(fSeconds / 60.0));
  else if (fSeconds < 86400.0)
    s.SetFormat("{} h ago", (int)(fSeconds / 3600.0));
  else
    s.SetFormat("{} d ago", (int)(fSeconds / 86400.0));
  return s;
}

bool plUsdLiveText::IsInFolder(plStringView sFile, plStringView sFolder)
{
  if (sFile.IsEmpty() || sFolder.IsEmpty())
    return false;

  plStringBuilder sParent = sFile;
  sParent.PathParentDirectory();
  sParent.Trim(nullptr, "/");

  plStringBuilder sTrimmedFolder = sFolder;
  sTrimmedFolder.Trim(nullptr, "/");
  return sParent.IsEqual_NoCase(sTrimmedFolder);
}

bool plUsdLiveText::IsSameFile(plStringView sA, plStringView sB)
{
  plStringBuilder a = sA, b = sB;
  a.MakeCleanPath();
  b.MakeCleanPath();
  return !a.IsEmpty() && a.IsEqual_NoCase(b);
}

void plUsdLiveText::SplitParticipant(plStringView sLayerFile, plString& out_sTool, plString& out_sUser)
{
  const plStringBuilder sName = sLayerFile.GetFileName();

  plStringView sTool = sName;
  plStringView sUser;
  if (const char* szDash = sName.FindSubString("-"))
  {
    sTool = plStringView(sName.GetData(), szDash);
    sUser = plStringView(szDash + 1, sName.GetData() + sName.GetElementCount());
  }

  struct KnownTool
  {
    const char* m_szPrefix;
    const char* m_szName;
  };

  const KnownTool knownTools[] = {
    {"plasma", "Plasma Editor"},
    {"blender", "Blender"},
    {"houdini", "Houdini"},
    {"maya", "Maya"},
    {"max", "3ds Max"},
    {"unreal", "Unreal"},
  };

  out_sTool = sTool;
  for (const KnownTool& tool : knownTools)
  {
    if (sTool.IsEqual_NoCase(tool.m_szPrefix))
      out_sTool = tool.m_szName;
  }

  out_sUser = sUser;
}
