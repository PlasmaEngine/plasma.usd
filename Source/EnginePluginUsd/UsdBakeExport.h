#pragma once

#include <EditorEngineProcessFramework/SceneExport/SceneExportModifier.h>
#include <EnginePluginUsd/EnginePluginUsdDLL.h>

/// \brief Exports a link's bake instead of its live objects.
///
/// On a scene or prefab export, every object tagged UsdBake is activated, so its prefab reference is written active,
/// and every live object that is not below another live object is tagged Exclude From Export, which leaves it and its
/// subtree out of the written scene. plUsdBakeSwitchModule is told to keep the bakes active for the rest of the
/// export. Simulate and Play the Game are left alone: they run the live objects.
///
/// This changes the document's engine world. The scene document sends the whole document to the engine again after
/// an export, which rebuilds that world; if it stayed, the next frame would switch the bakes back.
class PL_ENGINEPLUGINUSD_DLL plUsdBakeExportModifier : public plSceneExportModifier
{
  PL_ADD_DYNAMIC_REFLECTION(plUsdBakeExportModifier, plSceneExportModifier);

public:
  virtual void ModifyWorld(plWorld& ref_world, plStringView sDocumentType, const plUuid& documentGuid, bool bForExport) override;
};
