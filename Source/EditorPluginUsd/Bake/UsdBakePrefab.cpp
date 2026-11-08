#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/Prefabs/PrefabReferenceComponent.h>
#include <Core/World/GameObject.h>
#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorPluginUsd/Bake/UsdBakePrefab.h>
#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <EditorPluginUsd/LiveLink/UsdPrimComponents.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <RendererCore/Meshes/InstancedMeshComponent.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Document/DocumentManager.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>
#include <ToolsFoundation/Object/ObjectDirectAccessor.h>
#include <ToolsFoundation/Project/ToolsProject.h>

namespace
{
  plDocument* FindOpenDocument(plStringView sPath)
  {
    for (plDocumentManager* pManager : plDocumentManager::GetAllDocumentManagers())
    {
      if (plDocument* pDocument = pManager->GetDocumentByPath(sPath))
        return pDocument;
    }

    return nullptr;
  }

  plUuid MakeGuid(plStringView sLinkId, plStringView sPrimPath, plStringView sSuffix)
  {
    plStringBuilder s("usdbake:", sLinkId, sPrimPath, sSuffix);
    return plUuid::MakeStableUuidFromString(s);
  }

  bool HasTag(const plDocumentObject* pObject, plStringView sTag)
  {
    const plVariant tags = pObject->GetTypeAccessor().GetValue("Tags");
    if (tags.IsA<plVariantArray>())
    {
      for (const plVariant& tag : tags.Get<plVariantArray>())
      {
        if (tag.IsA<plString>() && tag.Get<plString>() == sTag)
          return true;
      }
    }
    return false;
  }

  plString ToPrefabReference(const plUuid& guid)
  {
    plStringBuilder s;
    plConversionUtils::ToString(guid, s);
    return s;
  }
} // namespace

plString plUsdBakePrefab::GetPrefabPath(plStringView sDocumentPath, plStringView sStageFile, plStringView sLinkId)
{
  plStringBuilder s = plToolsProject::GetSingleton()->GetProjectDirectory();
  s.AppendPath("UsdBake", sDocumentPath.GetFileName());
  s.AppendFormat("/{}-{}.plPrefab", sStageFile.GetFileName(), sLinkId);
  s.MakeCleanPath();
  return s;
}

plUInt64 plUsdBakePrefab::GetContentHash(plArrayPtr<const plUsdBakeNode> nodes, const plHashTable<plString, plUInt64>& materialHashes)
{
  plUInt64 uiHash = 0;
  auto add = [&](const auto& value)
  { uiHash = plHashingUtils::xxHash64(&value, sizeof(value), uiHash); };
  auto addString = [&](plStringView s)
  { uiHash = plHashingUtils::xxHash64String(s, uiHash); };

  for (const plUsdBakeNode& node : nodes)
  {
    const plUsdPrim& prim = node.m_Prim;
    addString(prim.m_sPath);
    addString(prim.m_sName);
    add(prim.m_iParent);
    add(prim.m_LocalTransform.m_vPosition);
    add(prim.m_LocalTransform.m_qRotation);
    add(prim.m_LocalTransform.m_vScale);
    add(prim.m_bActive);
    add(prim.m_bVisible);
    add(prim.m_bPrototype);
    add(prim.m_Instancer.m_uiHash);
    add(node.m_uiMeshParts);

    for (const plUsdComponent& component : prim.m_Components)
    {
      addString(component.m_sType);
      for (const plUsdComponentValue& value : component.m_Values)
      {
        addString(value.m_sProperty);
        uiHash = value.m_Value.ComputeHash(uiHash);
      }
    }

    if (node.m_uiMeshParts > 0)
    {
      const plColor tint = plUsdBakeStore::GetTint(prim.m_MaterialGroups);
      add(tint);
      addString(node.m_sMesh);
      for (const auto& group : prim.m_MaterialGroups)
      {
        addString(group.m_Surface.m_sMaterialPath);
        add(group.m_Surface.m_bDoubleSided);
      }
    }

    if (prim.m_Kind == plUsdPrimKind::Light)
      uiHash = prim.m_Light.ComputeHash(uiHash);
    else if (prim.m_Kind == plUsdPrimKind::Camera)
      uiHash = prim.m_Camera.ComputeHash(uiHash);
  }

  // Sorted, so the order materials were built in does not matter.
  plDynamicArray<plString> materials;
  for (auto it = materialHashes.GetIterator(); it.IsValid(); ++it)
    materials.PushBack(it.Key());
  materials.Sort();

  for (const plString& sMaterial : materials)
  {
    addString(sMaterial);
    add(*materialHashes.GetValue(sMaterial));
  }

  return uiHash;
}

plUuid plUsdBakePrefab::GetBakeObjectGuid(plStringView sLinkId)
{
  return MakeGuid(sLinkId, "", "#bake");
}

plStatus plUsdBakePrefab::Write(plStringView sPrefabPath, plStringView sLinkId, plArrayPtr<const plUsdBakeNode> nodes, plUuid& out_prefabGuid)
{
  plEditorApp* pApp = plEditorApp::GetSingleton();

  plDocument* pDocument = FindOpenDocument(sPrefabPath);
  const bool bWasOpen = pDocument != nullptr;

  if (pDocument == nullptr)
  {
    if (plOSFile::ExistsFile(sPrefabPath))
    {
      pDocument = pApp->OpenDocument(sPrefabPath, plDocumentFlags::None);
    }
    else
    {
      plStringBuilder sFolder = sPrefabPath;
      sFolder.PathParentDirectory();
      if (plOSFile::CreateDirectoryStructure(sFolder).Failed())
        return plStatus(plFmt("could not create '{}'", sFolder));

      pDocument = pApp->CreateDocument(sPrefabPath, plDocumentFlags::None);
    }
  }

  if (pDocument == nullptr)
    return plStatus(plFmt("could not open or create '{}'", sPrefabPath));

  plDocumentObjectManager* pManager = pDocument->GetObjectManager();
  plObjectDirectAccessor accessor(pManager);

  // Rebuilt from scratch each time. The GUIDs come from the prim paths, so they are the same in every bake.
  {
    pDocument->GetCommandHistory()->ClearUndoHistory();
    pDocument->GetCommandHistory()->ClearRedoHistory();
    plHybridArray<const plDocumentObject*, 64> previous;
    for (const plDocumentObject* pChild : pManager->GetRootObject()->GetChildren())
    {
      if (pChild->GetParentProperty() == "Children")
        previous.PushBack(pChild);
    }

    for (const plDocumentObject* pChild : previous)
      plUsdPrimComponents::RemoveObject(pManager, pChild).IgnoreResult();
  }

  const plRTTI* pGameObjectType = plGetStaticRTTI<plGameObject>();
  const plRTTI* pMeshType = plGetStaticRTTI<plMeshComponent>();
  const plRTTI* pInstancedType = plGetStaticRTTI<plInstancedMeshComponent>();
  auto set = [&](const plDocumentObject* pObject, plStringView sProperty, const plVariant& value)
  { pManager->SetValue(pManager->GetObject(pObject->GetGuid()), sProperty, value).IgnoreResult(); };

  // Instancers draw the files of their prototype meshes.
  plHashTable<plString, const plUsdBakeNode*> nodesByPath;
  for (const plUsdBakeNode& node : nodes)
    nodesByPath.Insert(node.m_Prim.m_sPath, &node);

  for (const plUsdBakeNode& node : nodes)
  {
    const plUsdPrim& prim = node.m_Prim;

    const plDocumentObject* pParent = prim.m_iParent >= 0 ? accessor.GetObject(MakeGuid(sLinkId, nodes[prim.m_iParent].m_Prim.m_sPath, "")) : pManager->GetRootObject();
    if (pParent == nullptr)
      continue;

    plUuid objectGuid = MakeGuid(sLinkId, prim.m_sPath, "");
    if (accessor.AddObjectByName(pParent, "Children", -1, pGameObjectType, objectGuid).Failed())
      continue;

    const plDocumentObject* pObject = accessor.GetObject(objectGuid);
    set(pObject, "Name", prim.m_sName);
    set(pObject, "LocalPosition", prim.m_LocalTransform.m_vPosition);
    set(pObject, "LocalRotation", prim.m_LocalTransform.m_qRotation);
    set(pObject, "LocalScaling", prim.m_LocalTransform.m_vScale);
    set(pObject, "Active", prim.m_bActive && prim.m_bVisible && !prim.m_bPrototype);

    if (node.m_uiMeshParts > 0)
    {
      pManager->InsertValue(pManager->GetObject(objectGuid), "Tags", plString("CastShadow"), -1).IgnoreResult();

      const plColor tint = plUsdBakeStore::GetTint(prim.m_MaterialGroups);
      for (plUInt32 i = 0; i < node.m_uiMeshParts; ++i)
      {
        plStringBuilder sSuffix;
        sSuffix.SetFormat("#mesh{}", i);
        plUuid componentGuid = MakeGuid(sLinkId, prim.m_sPath, sSuffix);

        if (accessor.AddObjectByName(pObject, "Components", -1, pMeshType, componentGuid).Succeeded())
        {
          const plDocumentObject* pComponent = accessor.GetObject(componentGuid);
          set(pComponent, "Mesh", plUsdBakeStore::GetMeshResource(node.m_sMesh, i));
          set(pComponent, "Color", tint);
          plUsdPrimComponents::ApplyMaterials(pManager, pComponent, sLinkId, prim.m_MaterialGroups);
        }
      }
    }

    if (prim.m_bActive && plUsdPrimComponents::GetComponentType(prim) != nullptr)
    {
      plUsdPrimComponents::Apply(pManager, pObject, prim, MakeGuid(sLinkId, prim.m_sPath, "#component"));
    }

    // As the live link shows it: one instanced mesh component per part of each prototype mesh, within the same budget.
    plUInt32 uiBudget = plUsdPrimComponents::MaxInstances;
    bool bInstanced = false;
    for (const plUsdInstancedMesh& mesh : prim.m_Instancer.m_Meshes)
    {
      const plUsdBakeNode* pMeshNode = nullptr;
      if (!nodesByPath.TryGetValue(mesh.m_sMeshPath, pMeshNode))
        continue;

      for (plUInt32 uiPart = 0; uiPart < pMeshNode->m_uiMeshParts; ++uiPart)
      {
        const plUInt32 uiCount = plMath::Min(mesh.m_Instances.GetCount(), uiBudget);
        uiBudget -= uiCount;
        if (uiCount == 0)
          continue;

        plStringBuilder sSeed;
        sSeed.SetFormat("usdbake:{}{}#instances{}#{}", sLinkId, prim.m_sPath, mesh.m_sMeshPath, uiPart);
        plUuid componentGuid = plUuid::MakeStableUuidFromString(sSeed);

        if (accessor.AddObjectByName(pObject, "Components", -1, pInstancedType, componentGuid).Succeeded())
        {
          plUsdPrimComponents::ApplyInstances(pManager, accessor.GetObject(componentGuid), plUsdBakeStore::GetMeshResource(pMeshNode->m_sMesh, uiPart),
            plUsdBakeStore::GetTint(pMeshNode->m_Prim.m_MaterialGroups), mesh.m_Instances.GetArrayPtr().GetSubArray(0, uiCount), sSeed);
          bInstanced = true;
          plUsdPrimComponents::ApplyMaterials(pManager, accessor.GetObject(componentGuid), sLinkId, pMeshNode->m_Prim.m_MaterialGroups);
        }
      }
    }

    if (bInstanced && node.m_uiMeshParts == 0)
      pManager->InsertValue(pManager->GetObject(objectGuid), "Tags", plString("CastShadow"), -1).IgnoreResult();

    // Everything on the object so far is generated; the prim's Plasma components come on top.
    if (!prim.m_Components.IsEmpty())
    {
      plHybridArray<plUuid, 8> generated;
      for (const plDocumentObject* pChild : pObject->GetChildren())
      {
        if (pChild->GetParentProperty() == "Components")
          generated.PushBack(pChild->GetGuid());
      }

      plUsdPrimComponents::ApplyUserComponents(pManager, pObject, prim.m_Components, {}, generated, plStringBuilder("usdbake:", sLinkId, prim.m_sPath));
    }
  }

  const plStatus res = pDocument->SaveDocument(true);
  out_prefabGuid = pDocument->GetGuid();

  if (!bWasOpen)
  {
    pDocument->GetDocumentManager()->CloseDocument(pDocument);
  }

  return res;
}

void plUsdBakePrefab::EnsureBakeObject(plDocument* pDocument, const plUuid& rootObject, plStringView sLinkId, const plUuid& prefabGuid)
{
  const plDocumentObjectManager* pManager = pDocument->GetObjectManager();

  const plDocumentObject* pRoot = pManager->GetObject(rootObject);
  if (pRoot == nullptr || !prefabGuid.IsValid())
    return;

  const plString sPrefab = ToPrefabReference(prefabGuid);
  plUuid bakeGuid = GetBakeObjectGuid(sLinkId);
  plUuid componentGuid = MakeGuid(sLinkId, "", "#bakeprefab");

  {
    const plDocumentObject* pBake = pManager->GetObject(bakeGuid);
    const plDocumentObject* pComponent = pManager->GetObject(componentGuid);
    if (pBake != nullptr && HasTag(pBake, BakeTag) && pComponent != nullptr &&
        pComponent->GetTypeAccessor().GetValue("Prefab").ConvertTo<plString>() == sPrefab)
      return;
  }

  // An undoable step, so the document knows it has changes to save.
  plObjectCommandAccessor accessor(pDocument->GetCommandHistory());
  accessor.StartTransaction("Update USD Bake");

  const plDocumentObject* pBake = accessor.GetObject(bakeGuid);
  if (pBake == nullptr)
  {
    if (accessor.AddObjectByName(pRoot, "Children", -1, plGetStaticRTTI<plGameObject>(), bakeGuid).Succeeded())
    {
      pBake = accessor.GetObject(bakeGuid);
      accessor.SetValueByName(pBake, "Name", plString("USD Bake")).IgnoreResult();
    }
  }

  if (pBake != nullptr && !HasTag(pBake, BakeTag))
  {
    accessor.InsertValueByName(pBake, "Tags", plString(BakeTag), -1).IgnoreResult();
  }

  const plDocumentObject* pComponent = accessor.GetObject(componentGuid);
  if (pBake != nullptr && pComponent == nullptr)
  {
    if (accessor.AddObjectByName(pBake, "Components", -1, plGetStaticRTTI<plPrefabReferenceComponent>(), componentGuid).Succeeded())
      pComponent = accessor.GetObject(componentGuid);
  }

  if (pComponent != nullptr && pComponent->GetTypeAccessor().GetValue("Prefab").ConvertTo<plString>() != sPrefab)
  {
    accessor.SetValueByName(pComponent, "Prefab", sPrefab).IgnoreResult();
  }

  accessor.FinishTransaction();
}

bool plUsdBakePrefab::IsBakeObject(const plDocument* pDocument, const plUuid& object)
{
  const plDocumentObject* pObject = pDocument->GetObjectManager()->GetObject(object);
  return pObject != nullptr && pObject->GetType()->IsDerivedFrom<plGameObject>() && HasTag(pObject, BakeTag);
}
