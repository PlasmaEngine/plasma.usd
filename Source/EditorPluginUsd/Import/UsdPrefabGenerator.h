#pragma once

#include <EditorFramework/Assets/AssetDocumentGenerator.h>
#include <EditorPluginUsd/EditorPluginUsdDLL.h>

/// \brief The "USD Prefab (live)" import method: a prefab whose content is a live link to the USD file.
///
/// Opening the prefab streams the stage like a scene link, with sessions and Blender. Its USD Bake object references
/// the link's bake prefab, which the background bake fills after the import, so instances placed in scenes and games
/// show the bake.
class plUsdPrefabDocumentGenerator : public plAssetDocumentGenerator
{
  PL_ADD_DYNAMIC_REFLECTION(plUsdPrefabDocumentGenerator, plAssetDocumentGenerator);

public:
  plUsdPrefabDocumentGenerator();
  ~plUsdPrefabDocumentGenerator();

  virtual void GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const override;
  virtual plStringView GetDocumentExtension() const override { return "plPrefab"; }
  virtual plStringView GetGeneratorGroup() const override { return "Prefabs"; }
  virtual plStatus Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments) override;
};
