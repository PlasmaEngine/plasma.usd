#include <EnginePluginUsd/EnginePluginUsdPCH.h>

#include <Core/World/World.h>
#include <EnginePluginUsd/UsdBakeExport.h>
#include <EnginePluginUsd/UsdBakeSwitch.h>
#include <Foundation/Types/TagRegistry.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plUsdBakeExportModifier, 1, plRTTIDefaultAllocator<plUsdBakeExportModifier>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

void plUsdBakeExportModifier::ModifyWorld(plWorld& ref_world, plStringView sDocumentType, const plUuid& documentGuid, bool bForExport)
{
  PL_IGNORE_UNUSED(sDocumentType);
  PL_IGNORE_UNUSED(documentGuid);

  if (!bForExport)
    return;

  PL_LOCK(ref_world.GetWriteMarker());

  const plTag& bakeTag = plTagRegistry::GetGlobalRegistry().RegisterTag(plUsdBakeTags::Bake);
  const plTag& liveTag = plTagRegistry::GetGlobalRegistry().RegisterTag(plUsdBakeTags::Live);
  const plTag& excludeTag = plTagRegistry::GetGlobalRegistry().RegisterTag(plUsdBakeTags::ExcludeFromExport);

  plHybridArray<plGameObjectHandle, 8> bakes;

  for (auto it = ref_world.GetObjects(); it.IsValid(); ++it)
  {
    plGameObject* pObject = it;
    const plTagSet& tags = pObject->GetTags();

    if (tags.IsSet(bakeTag))
    {
      bakes.PushBack(pObject->GetHandle());
    }
    else if (tags.IsSet(liveTag))
    {
      // The world writer skips an excluded object's subtree, so the topmost live object of each subtree is enough.
      const plGameObject* pParent = pObject->GetParent();
      if (pParent == nullptr || !pParent->GetTags().IsSet(liveTag))
        pObject->SetTag(excludeTag);
    }
  }

  // Activating a bake instantiates its prefab, which creates objects, so this happens after the iteration.
  for (const plGameObjectHandle& hBake : bakes)
  {
    plGameObject* pBake = nullptr;
    if (ref_world.TryGetObject(hBake, pBake))
      pBake->SetActiveFlag(true);
  }

  if (plUsdBakeSwitchModule* pModule = ref_world.GetModule<plUsdBakeSwitchModule>())
    pModule->HoldBakesActive();
}
