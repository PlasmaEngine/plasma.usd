#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/Bake/UsdBakePrefab.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/LiveLink/UsdObjectPolicy.h>
#include <GuiFoundation/UIServices/UIServices.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

namespace
{
  /// The link that generated pObject, unless the link is adding or removing objects itself right now.
  plUsdLiveLink* FindOwningLink(const plDocumentObject* pObject)
  {
    plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
    if (pManager == nullptr || pObject == nullptr)
      return nullptr;

    const plDocument* pDocument = pObject->GetDocumentObjectManager()->GetDocument();
    plUsdLiveLink* pLink = pManager->FindLinkForObject(pDocument, pObject->GetGuid());
    if (pLink == nullptr || pLink->IsApplyingRemote() || !pLink->IsGeneratedObject(pObject->GetGuid()))
      return nullptr;

    return pLink;
  }

  /// The link whose USD Bake object pObject is, unless the link is changing it itself right now.
  plUsdLiveLink* FindBakeLink(const plDocumentObject* pObject)
  {
    plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
    if (pManager == nullptr || pObject == nullptr || pObject->GetParent() == nullptr)
      return nullptr;

    const plDocument* pDocument = pObject->GetDocumentObjectManager()->GetDocument();
    plUsdLiveLink* pLink = pManager->FindLinkForObject(pDocument, pObject->GetParent()->GetGuid());
    if (pLink == nullptr || pLink->IsApplyingRemote() || pLink->GetRootObject() != pObject->GetParent()->GetGuid() ||
        plUsdBakePrefab::GetBakeObjectGuid(pLink->GetLinkId()) != pObject->GetGuid())
      return nullptr;

    return pLink;
  }

  constexpr const char* BakeObjectMessage = "The USD Bake object is what exports and games use for this stage, so it stays while the stage is "
                                            "linked. Unlink the stage to keep the bake as ordinary objects.";
} // namespace

plStatus plUsdObjectPolicy::CanRemove(const plDocumentObject* pObject) const
{
  if (FindOwningLink(pObject) != nullptr)
    return plStatus("Objects generated from a USD stage are deactivated, not deleted. Use 'Deactivate Prim'.");

  if (FindBakeLink(pObject) != nullptr)
    return plStatus(BakeObjectMessage);

  return plStatus(PL_SUCCESS);
}

plStatus plUsdObjectPolicy::CanMove(const plDocumentObject* pObject, const plDocumentObject* pNewParent, plStringView sParentProperty, const plVariant& index) const
{
  if (FindOwningLink(pObject) != nullptr)
    return plStatus("Objects generated from a USD stage follow the stage's hierarchy. Reparent the prim in the tool that owns the stage.");

  if (FindBakeLink(pObject) != nullptr)
    return plStatus(BakeObjectMessage);

  // Generated objects and everything below them are rebuilt from the stage and never saved.
  if (sParentProperty == "Children" && FindOwningLink(pNewParent) != nullptr)
    return plStatus("Objects under objects generated from a USD stage are not saved. Put it under the link's root object instead, or add the "
                    "prim in the tool that owns the stage.");

  return plStatus(PL_SUCCESS);
}

void plUsdObjectPolicy::FilterDeleteRequest(const plDocument* pDocument, plDeque<const plDocumentObject*>& inout_objects) const
{
  plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
  if (pManager == nullptr)
    return;

  struct LinkPrims
  {
    plUsdLiveLink* m_pLink = nullptr;
    plHybridArray<plUuid, 16> m_Objects;
  };

  plHybridArray<LinkPrims, 4> prims;
  plHybridArray<plUsdLiveLink*, 4> roots;

  bool bKeptBake = false;

  for (plUInt32 i = 0; i < inout_objects.GetCount();)
  {
    const plDocumentObject* pObject = inout_objects[i];

    if (FindBakeLink(pObject) != nullptr)
    {
      bKeptBake = true;
      inout_objects.RemoveAtAndCopy(i);
      continue;
    }

    plUsdLiveLink* pLink = pManager->FindLinkForObject(pDocument, pObject->GetGuid());

    if (pLink == nullptr)
    {
      ++i;
      continue;
    }

    if (pLink->GetRootObject() == pObject->GetGuid())
    {
      // Decided below; the root is only deleted if the user agrees to unlink.
      roots.PushBack(pLink);
      ++i;
      continue;
    }

    LinkPrims* pEntry = nullptr;
    for (LinkPrims& entry : prims)
    {
      if (entry.m_pLink == pLink)
        pEntry = &entry;
    }

    if (pEntry == nullptr)
    {
      pEntry = &prims.ExpandAndGetRef();
      pEntry->m_pLink = pLink;
    }

    pEntry->m_Objects.PushBack(pObject->GetGuid());
    inout_objects.RemoveAtAndCopy(i);
  }

  if (bKeptBake)
    plEditorUiServices::MessageBoxInformation(BakeObjectMessage);

  // Prims first: unlinking below destroys the link.
  for (LinkPrims& entry : prims)
  {
    if (!entry.m_pLink->IsWritable())
    {
      plEditorUiServices::MessageBoxInformation(
        "This USD stage is linked read-only, so its prims cannot be deactivated. Start a session in the USD Live panel to write your own layer.");
      continue;
    }

    const plStatus res = entry.m_pLink->SetPrimsActive(entry.m_Objects, false);
    if (res.Failed())
      plEditorUiServices::MessageBoxStatus(res, "Deactivating the prims failed.");
  }

  for (plUsdLiveLink* pLink : roots)
  {
    const plEditorUiMessageButton::Enum answer = plEditorUiServices::MessageBoxQuestion(
      plFmt("Deleting this object unlinks '{}' and removes the objects generated from it. Nothing is written to USD.", pLink->GetSettings().m_sStageFile),
      plEditorUiMessageButton::Ok | plEditorUiMessageButton::Cancel, plEditorUiMessageButton::Cancel);

    const plUuid root = pLink->GetRootObject();
    if (answer == plEditorUiMessageButton::Ok)
    {
      pManager->Unlink(pLink, true);
      continue;
    }

    for (plUInt32 i = 0; i < inout_objects.GetCount(); ++i)
    {
      if (inout_objects[i]->GetGuid() == root)
      {
        inout_objects.RemoveAtAndCopy(i);
        break;
      }
    }
  }
}
