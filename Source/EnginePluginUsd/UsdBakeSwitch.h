#pragma once

#include <Core/World/WorldModule.h>
#include <EnginePluginUsd/EnginePluginUsdDLL.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/Tag.h>

/// Tag names shared with the editor plugin, which sets them on the objects it creates.
namespace plUsdBakeTags
{
  /// Objects a live link generates. They exist only while the link streams and are never saved.
  static constexpr const char* Live = "UsdLive";

  /// The saved "USD Bake" child of a link root. It holds a prefab reference to the link's bake prefab.
  static constexpr const char* Bake = "UsdBake";

  /// Scene and prefab export leaves out every object with this tag, together with everything below it.
  static constexpr const char* ExcludeFromExport = "Exclude From Export";
} // namespace plUsdBakeTags

/// \brief Shows a link's bake while the link has no live objects, and hides it while it has.
///
/// An object tagged UsdBake is inactive while its parent has at least one other direct child tagged UsdLive, and
/// active otherwise. Deactivating it also removes its prefab instance. The module is created in every world the
/// application updates and runs in every update, simulated or not, so the editor viewport, Simulate and Play the
/// Game show either the live objects or the bake, never both.
///
/// Bake objects are found by scanning the world, which happens when the world's object count changes (at most ten
/// times a second) and every two seconds regardless. Between scans only the known bake objects and their siblings
/// are looked at.
class PL_ENGINEPLUGINUSD_DLL plUsdBakeSwitchModule : public plWorldModule
{
  PL_DECLARE_WORLD_MODULE();
  PL_ADD_DYNAMIC_REFLECTION(plUsdBakeSwitchModule, plWorldModule);

public:
  plUsdBakeSwitchModule(plWorld* pWorld);
  ~plUsdBakeSwitchModule();

  /// \brief Keeps every bake object active until the application's next frame.
  ///
  /// Export calls this: the world updates that run between the export modifiers and the world writer must not
  /// hide the bakes again. The next frame switches them back to match the live objects.
  void HoldBakesActive();

  /// \brief Creates the module in every world the application updates this frame. Called at the start of each frame.
  static void CreateInUpdatedWorlds();

protected:
  virtual void Initialize() override;
  virtual void WorldClear() override;

private:
  void UpdateBakes(const plWorldModule::UpdateContext& context);
  void FindBakeObjects(plTime now);
  bool HasLiveSibling(const plGameObject* pBake) const;

  const plTag* m_pBakeTag = nullptr;
  const plTag* m_pLiveTag = nullptr;

  plDynamicArray<plGameObjectHandle> m_BakeObjects;
  plUInt32 m_uiScannedObjectCount = 0;
  plTime m_LastScan;
  bool m_bScanPending = true;

  bool m_bHoldActive = false;
  plUInt64 m_uiHoldFrame = 0;
};
