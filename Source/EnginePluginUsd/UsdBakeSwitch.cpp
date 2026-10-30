#include <EnginePluginUsd/EnginePluginUsdPCH.h>

#include <Core/World/World.h>
#include <EnginePluginUsd/UsdBakeSwitch.h>
#include <Foundation/Communication/GlobalEvent.h>
#include <Foundation/Types/TagRegistry.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

// clang-format off
PL_IMPLEMENT_WORLD_MODULE(plUsdBakeSwitchModule);

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plUsdBakeSwitchModule, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  /// Counts application frames. Written at the start of a frame, before any world update, and read inside them.
  plUInt64 s_uiAppFrame = 0;

  /// While the object count keeps changing (a link streaming, objects spawning in Play the Game), scans are this far
  /// apart at least.
  const plTime s_MinScanInterval = plTime::MakeFromMilliseconds(100);

  /// A world is rescanned this often even if its object count stays the same, which finds tags set on objects that
  /// already existed.
  const plTime s_MaxScanInterval = plTime::MakeFromSeconds(2);
} // namespace

// The start of a frame is on the main thread, after the editor's messages were applied and before any world update,
// so no other code holds a world's write marker. The worlds of the main views are the ones plGameApplication updates.
PL_ON_GLOBAL_EVENT(GameApp_BeginAppTick)
{
  PL_IGNORE_UNUSED(param0);
  PL_IGNORE_UNUSED(param1);
  PL_IGNORE_UNUSED(param2);
  PL_IGNORE_UNUSED(param3);

  ++s_uiAppFrame;

  plUsdBakeSwitchModule::CreateInUpdatedWorlds();
}

plUsdBakeSwitchModule::plUsdBakeSwitchModule(plWorld* pWorld)
  : plWorldModule(pWorld)
{
}

plUsdBakeSwitchModule::~plUsdBakeSwitchModule() = default;

void plUsdBakeSwitchModule::CreateInUpdatedWorlds()
{
  for (const plViewHandle& hView : plRenderWorld::GetMainViews())
  {
    plView* pView = nullptr;
    if (!plRenderWorld::TryGetView(hView, pView))
      continue;

    plWorld* pWorld = pView->GetWorld();
    if (pWorld == nullptr)
      continue;

    PL_LOCK(pWorld->GetWriteMarker());
    pWorld->GetOrCreateModule<plUsdBakeSwitchModule>();
  }
}

void plUsdBakeSwitchModule::HoldBakesActive()
{
  m_bHoldActive = true;
  m_uiHoldFrame = s_uiAppFrame;
}

void plUsdBakeSwitchModule::Initialize()
{
  SUPER::Initialize();

  m_pBakeTag = &plTagRegistry::GetGlobalRegistry().RegisterTag(plUsdBakeTags::Bake);
  m_pLiveTag = &plTagRegistry::GetGlobalRegistry().RegisterTag(plUsdBakeTags::Live);

  // The first update scans the world.
  m_LastScan = plTime::Now() - s_MaxScanInterval;

  // Runs while the world is not simulated too: in the editor viewport the bake has to give way to the live objects.
  auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plUsdBakeSwitchModule::UpdateBakes, this);
  desc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PreAsync;
  desc.m_bOnlyUpdateWhenSimulating = false;
  RegisterUpdateFunction(desc);
}

void plUsdBakeSwitchModule::WorldClear()
{
  m_BakeObjects.Clear();
  m_bScanPending = true;

  SUPER::WorldClear();
}

void plUsdBakeSwitchModule::UpdateBakes(const plWorldModule::UpdateContext& context)
{
  PL_IGNORE_UNUSED(context);

  plWorld* pWorld = GetWorld();
  const plTime now = plTime::Now();

  if (pWorld->GetObjectCount() != m_uiScannedObjectCount)
    m_bScanPending = true;

  const plTime sinceScan = now - m_LastScan;
  if ((m_bScanPending && sinceScan >= s_MinScanInterval) || sinceScan >= s_MaxScanInterval)
  {
    FindBakeObjects(now);
  }

  if (m_bHoldActive && m_uiHoldFrame != s_uiAppFrame)
    m_bHoldActive = false;

  for (plUInt32 i = 0; i < m_BakeObjects.GetCount();)
  {
    plGameObject* pBake = nullptr;
    if (!pWorld->TryGetObject(m_BakeObjects[i], pBake))
    {
      m_BakeObjects.RemoveAtAndSwap(i);
      continue;
    }

    pBake->SetActiveFlag(m_bHoldActive || !HasLiveSibling(pBake));
    ++i;
  }
}

void plUsdBakeSwitchModule::FindBakeObjects(plTime now)
{
  plWorld* pWorld = GetWorld();

  m_BakeObjects.Clear();

  for (auto it = pWorld->GetObjects(); it.IsValid(); ++it)
  {
    if (it->GetTags().IsSet(*m_pBakeTag))
      m_BakeObjects.PushBack(it->GetHandle());
  }

  m_uiScannedObjectCount = pWorld->GetObjectCount();
  m_LastScan = now;
  m_bScanPending = false;
}

bool plUsdBakeSwitchModule::HasLiveSibling(const plGameObject* pBake) const
{
  const plGameObject* pParent = pBake->GetParent();
  if (pParent == nullptr)
    return false;

  for (auto it = pParent->GetChildren(); it.IsValid(); ++it)
  {
    const plGameObject* pChild = it;
    if (pChild != pBake && pChild->GetTags().IsSet(*m_pLiveTag))
      return true;
  }

  return false;
}
