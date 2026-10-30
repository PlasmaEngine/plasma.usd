#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorPluginUsd/Bake/UsdBakePrefab.h>
#include <EditorPluginUsd/Import/UsdPrefabGenerator.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <Foundation/IO/OSFile.h>
#include <UsdBridge/UsdStage.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plUsdPrefabDocumentGenerator, 1, plRTTIDefaultAllocator<plUsdPrefabDocumentGenerator>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plUsdPrefabDocumentGenerator::plUsdPrefabDocumentGenerator()
{
  for (plStringView sExtension : plUsdStage::GetFileExtensions())
    AddSupportedFileType(sExtension);
}

plUsdPrefabDocumentGenerator::~plUsdPrefabDocumentGenerator() = default;

void plUsdPrefabDocumentGenerator::GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const
{
  // Below the Mesh import, which stays the default for model files.
  plAssetDocumentGenerator::ImportMode& info = out_modes.ExpandAndGetRef();
  info.m_Priority = plAssetDocGeneratorPriority::LowPriority;
  info.m_sName = "UsdPrefabImport";
  info.m_sIcon = ":/UsdPlugin/Icons/UsdLive.svg";
}

plStatus plUsdPrefabDocumentGenerator::Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments)
{
  const plStringBuilder sOutFile = GetImportTargetPath(sInputFileAbs);

  // Imported before: leave that prefab and its link as they are.
  if (plOSFile::ExistsFile(sOutFile))
    return plStatus(PL_SUCCESS);

  plDocument* pDocument = plEditorApp::GetSingleton()->CreateDocument(sOutFile, plDocumentFlags::None);
  plGameObjectDocument* pGameObjectDocument = plDynamicCast<plGameObjectDocument*>(pDocument);
  if (pGameObjectDocument == nullptr)
    return plStatus(plFmt("Could not create the prefab '{}'.", sOutFile));

  plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
  PL_SUCCEED_OR_RETURN(pManager->LinkStage(pGameObjectDocument, sInputFileAbs));

  plHybridArray<plUsdLiveLink*, 1> links;
  pManager->GetLinks(pDocument, links);
  if (links.IsEmpty())
    return plStatus("The stage could not be linked.");

  // The bake prefab exists from the start, so the USD Bake object can reference it; the background bake fills it once
  // this document is closed after the import.
  const plUsdLiveLink* pLink = links.PeekBack();
  const plString sBake = plUsdBakePrefab::GetPrefabPath(sOutFile, pLink->GetSettings().m_sStageFile, pLink->GetLinkId());

  plUuid bakeGuid;
  PL_SUCCEED_OR_RETURN(plUsdBakePrefab::Write(sBake, pLink->GetLinkId(), {}, bakeGuid));
  plUsdBakePrefab::EnsureBakeObject(pDocument, pLink->GetRootObject(), pLink->GetLinkId(), bakeGuid);

  out_generatedDocuments.PushBack(pDocument);
  return plStatus(PL_SUCCESS);
}
