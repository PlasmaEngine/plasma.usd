#include <UsdBridge/UsdBridgePCH.h>

#include <Foundation/IO/OSFile.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
#include <UsdBridge/UsdMtlxFileFormat.h>
#include <UsdBridge/UsdRuntime.h>

#pragma warning(push, 0)
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/usd/schemaRegistry.h>
#pragma warning(pop)

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>

plString plUsdRuntime::GetOpenUsdDirectory()
{
  HMODULE hModule = GetModuleHandleW(L"usd_ms.dll");
  if (hModule == nullptr)
    return {};

  wchar_t szPath[MAX_PATH * 2] = {};
  const DWORD uiLength = GetModuleFileNameW(hModule, szPath, PL_ARRAY_SIZE(szPath));
  if (uiLength == 0 || uiLength >= PL_ARRAY_SIZE(szPath))
    return {};

  plStringBuilder sDir = plStringUtf8(szPath).GetData();
  sDir.MakeCleanPath();
  sDir.PathParentDirectory();
  sDir.Trim(nullptr, "/");
  return sDir;
}

plStatus plUsdRuntime::Initialize(plDynamicArray<plString>* out_pSearchedPaths)
{
  const plString sDir = GetOpenUsdDirectory();
  if (sDir.IsEmpty())
    return plStatus("usd_ms.dll is not loaded in this process.");

  std::vector<std::string> pluginFolders;

  for (const char* szSub : {"usd", "plugin/usd"})
  {
    plStringBuilder sFolder(sDir, "/", szSub);

    if (out_pSearchedPaths)
      out_pSearchedPaths->PushBack(sFolder);

    plStringBuilder sInfo(sFolder, "/plugInfo.json");
    if (plOSFile::ExistsFile(sInfo))
    {
      pluginFolders.push_back(std::string(sFolder.GetData()));
    }
  }

  if (pluginFolders.empty())
    return plStatus(plFmt("No OpenUSD plugInfo.json found beside '{}/usd_ms.dll'.", sDir));

  pxr::PlugRegistry::GetInstance().RegisterPlugins(pluginFolders);

  // plugin/usd/plasmaMtlx declares the .mtlx file format; this defines it.
  plUsdMtlx::RegisterFileFormat();

  // Other tools save over the layers this process has open. A layer read the normal way keeps its .usdc memory-mapped
  // (and zero-copy arrays keep the mapping alive), which on Windows stops the other tool from replacing the file.
  // Detached layers are read completely into memory and keep no connection to the file, but detaching everything reads
  // and decompresses every referenced file in full when a stage opens. See DetachLayers.
  DetachLayers("/.live/");

  // Without the sdf plugin nothing can be read at all, and without usdGeom every stage looks empty. Checking both
  // here turns a silent "the stage has no prims" into an error that names the folders.
  if (!pxr::SdfFileFormat::FindByExtension("usda"))
    return plStatus(plFmt("OpenUSD loaded from '{}' but has no .usda file format - its plugInfo tree is incomplete.", sDir));

  if (pxr::UsdSchemaRegistry::GetInstance().FindConcretePrimDefinition(pxr::TfToken("Mesh")) == nullptr)
    return plStatus(plFmt("OpenUSD loaded from '{}' but the UsdGeom schemas are not registered.", sDir));

  // Components still travel without it (see plUsdComponent); only tools that validate schemas would miss it.
  if (!IsApiSchemaRegistered("PlasmaComponentAPI"))
    plLog::Warning("OpenUSD loaded from '{}' without the package's PlasmaComponentAPI schema (plugin/usd/plasmaSchemas).", sDir);

  // Only MaterialX sublayers and references need it.
  if (!pxr::SdfFileFormat::FindByExtension("mtlx"))
    plLog::Warning("OpenUSD loaded from '{}' without the package's .mtlx file format (plugin/usd/plasmaMtlx); MaterialX layers will not open.", sDir);

  return plStatus(PL_SUCCESS);
}

bool plUsdRuntime::IsApiSchemaRegistered(plStringView sSchema)
{
  const std::string sName(sSchema.GetStartPointer(), sSchema.GetElementCount());
  return pxr::UsdSchemaRegistry::GetInstance().FindAppliedAPIPrimDefinition(pxr::TfToken(sName)) != nullptr;
}

void plUsdRuntime::DetachLayers(plStringView sPattern)
{
  static plMutex s_Mutex;
  static std::vector<std::string> s_Patterns;

  PL_LOCK(s_Mutex);

  const std::string sNew(sPattern.GetStartPointer(), sPattern.GetElementCount());
  if (sNew.empty() || std::find(s_Patterns.begin(), s_Patterns.end(), sNew) != s_Patterns.end())
    return;

  s_Patterns.push_back(sNew);
  pxr::SdfLayer::SetDetachedLayerRules(pxr::SdfLayer::DetachedLayerRules().Include(s_Patterns));
}
