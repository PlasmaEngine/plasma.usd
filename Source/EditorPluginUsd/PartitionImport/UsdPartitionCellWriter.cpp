#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/World/GameObject.h>
#include <EditorFramework/Assets/AssetDocumentInfo.h>
#include <EditorPluginScene/Scene/SceneLayerContentInfo.h>
#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionCellWriter.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/HashSet.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Serialization/AbstractObjectGraph.h>
#include <Foundation/Serialization/DdlSerializer.h>
#include <Foundation/Serialization/RttiConverter.h>
#include <RendererCore/Meshes/InstancedMeshComponent.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Serialization/ToolsSerializationUtils.h>

namespace
{
  /// Instances per instanced mesh component; each is a document object of its own in the editor.
  constexpr plUInt32 MaxInstancesPerComponent = 20000;

  void SetProperty(plAbstractObjectNode* pNode, plStringView sName, const plVariant& value)
  {
    if (pNode->FindProperty(sName) != nullptr)
      pNode->ChangeProperty(sName, value);
    else
      pNode->AddProperty(sName, value);
  }

  plVariantArray GetArray(const plAbstractObjectNode* pNode, plStringView sName)
  {
    const plAbstractObjectNode::Property* pProperty = pNode != nullptr ? pNode->FindProperty(sName) : nullptr;
    if (pProperty != nullptr && pProperty->m_Value.IsA<plVariantArray>())
      return pProperty->m_Value.Get<plVariantArray>();
    return {};
  }

  bool HasGeneratedTag(const plAbstractObjectNode* pNode)
  {
    if (pNode->GetType() != plGetStaticRTTI<plGameObject>()->GetTypeName())
      return false;

    for (const plVariant& tag : GetArray(pNode, "Tags"))
    {
      if (tag.IsA<plString>() && tag.Get<plString>() == plUsdPartitionCellWriter::GeneratedTag)
        return true;
    }
    return false;
  }

  /// Removes the generated objects from a kept node's children.
  void DropGeneratedChildren(plAbstractObjectNode* pNode, const plHashSet<plUuid>& generated)
  {
    const plVariantArray children = GetArray(pNode, "Children");
    plVariantArray kept;
    for (const plVariant& child : children)
    {
      if (!child.IsA<plUuid>() || !generated.Contains(child.Get<plUuid>()))
        kept.PushBack(child);
    }

    if (kept.GetCount() != children.GetCount())
      pNode->ChangeProperty("Children", kept);
  }

  /// Every Uuid a value names, also inside arrays and dictionaries.
  void CollectUuids(const plVariant& value, plDynamicArray<plUuid>& out_uuids)
  {
    if (value.IsA<plUuid>())
    {
      out_uuids.PushBack(value.Get<plUuid>());
    }
    else if (value.IsA<plVariantArray>())
    {
      for (const plVariant& item : value.Get<plVariantArray>())
        CollectUuids(item, out_uuids);
    }
    else if (value.IsA<plVariantDictionary>())
    {
      for (auto it : value.Get<plVariantDictionary>())
        CollectUuids(it.Value(), out_uuids);
    }
  }

  /// The nodes reachable from the starts through Uuid properties, without passing generated objects.
  void CollectKept(const plAbstractObjectGraph& graph, plArrayPtr<const plUuid> starts, const plHashSet<plUuid>& generated, plHashSet<plUuid>& inout_kept)
  {
    plDeque<plUuid> open;
    for (const plUuid& start : starts)
      open.PushBack(start);

    plDynamicArray<plUuid> uuids;
    while (!open.IsEmpty())
    {
      const plUuid guid = open.PeekFront();
      open.PopFront();

      if (generated.Contains(guid) || inout_kept.Contains(guid))
        continue;

      const plAbstractObjectNode* pNode = graph.GetNode(guid);
      if (pNode == nullptr)
        continue;

      inout_kept.Insert(guid);

      uuids.Clear();
      for (const plAbstractObjectNode::Property& property : pNode->GetProperties())
        CollectUuids(property.m_Value, uuids);
      for (const plUuid& next : uuids)
        open.PushBack(next);
    }
  }

  void ReadHeaderInfo(const plAbstractObjectGraph& header, plAssetDocumentInfo& out_info)
  {
    plRttiConverterContext context;
    plRttiConverterReader reader(&header, &context);
    if (const plAbstractObjectNode* pHeaderNode = header.GetNodeByName("Header"))
      reader.ApplyPropertiesToObject(pHeaderNode, out_info.GetDynamicRTTI(), &out_info);
  }

  plSceneLayerContentInfo* GetContentInfo(plAssetDocumentInfo& ref_info)
  {
    for (plReflectedClass* pMeta : ref_info.m_MetaInfo)
    {
      if (plSceneLayerContentInfo* pContent = plDynamicCast<plSceneLayerContentInfo*>(pMeta))
        return pContent;
    }

    plSceneLayerContentInfo* pContent = PL_DEFAULT_NEW(plSceneLayerContentInfo);
    ref_info.m_MetaInfo.PushBack(pContent);
    return pContent;
  }

  plAbstractObjectNode* AddNode(plAbstractObjectGraph& ref_graph, const plUuid& guid, const plRTTI* pType, plSet<const plRTTI*>& inout_types)
  {
    inout_types.Insert(pType);
    return ref_graph.AddNode(guid, pType->GetTypeName(), pType->GetTypeVersion());
  }

  plAbstractObjectNode* AddGameObject(plAbstractObjectGraph& ref_graph, const plUuid& guid, plStringView sName, const plTransform& local, bool bMesh,
    plSet<const plRTTI*>& inout_types)
  {
    plAbstractObjectNode* pNode = AddNode(ref_graph, guid, plGetStaticRTTI<plGameObject>(), inout_types);
    pNode->AddProperty("Active", true);
    pNode->AddProperty("Name", plString(sName));
    pNode->AddProperty("GlobalKey", plString());
    pNode->AddProperty("LocalPosition", local.m_vPosition);
    pNode->AddProperty("LocalRotation", local.m_qRotation);
    pNode->AddProperty("LocalScaling", local.m_vScale);
    pNode->AddProperty("LocalUniformScaling", 1.0f);

    plVariantArray tags;
    tags.PushBack(plString(plUsdPartitionCellWriter::GeneratedTag));
    if (bMesh)
      tags.PushBack(plString("CastShadow"));
    pNode->AddProperty("Tags", tags);

    pNode->AddProperty("Children", plVariantArray());
    pNode->AddProperty("Components", plVariantArray());
    return pNode;
  }

  void AddComponentToObject(plAbstractObjectNode* pObject, const plUuid& component)
  {
    plVariantArray components = GetArray(pObject, "Components");
    components.PushBack(component);
    SetProperty(pObject, "Components", components);
  }

  /// Adds an import object, with its components, as a child of the group; returns its game object's GUID.
  plUuid AddImportObject(plAbstractObjectGraph& ref_graph, plStringView sImportId, const plUsdImportObject& object, const plVec3& vGroupPosition,
    plSet<const plRTTI*>& inout_types)
  {
    const plUuid guid = plUsdPartitionImportData::MakeGuid(sImportId, object.m_sKey);

    plTransform local = object.m_Transform;
    local.m_vPosition -= vGroupPosition;

    plAbstractObjectNode* pObject = AddGameObject(ref_graph, guid, object.m_sName, local, object.m_uiParts > 0, inout_types);

    plStringBuilder sComponentKey;
    for (plUInt32 uiPart = 0; uiPart < object.m_uiParts; ++uiPart)
    {
      const plString sMesh = plUsdPartitionImportData::GetMeshResource(object.m_sMesh, uiPart);

      if (object.m_Kind == plUsdImportObjectKind::Mesh)
      {
        sComponentKey.SetFormat("{}#mesh{}", object.m_sKey, uiPart);
        const plUuid componentGuid = plUsdPartitionImportData::MakeGuid(sImportId, sComponentKey);

        plAbstractObjectNode* pComponent = AddNode(ref_graph, componentGuid, plGetStaticRTTI<plMeshComponent>(), inout_types);
        pComponent->AddProperty("Active", true);
        pComponent->AddProperty("Mesh", sMesh);
        AddComponentToObject(pObject, componentGuid);
        continue;
      }

      if (object.m_Kind != plUsdImportObjectKind::Instances)
        continue;

      for (plUInt32 uiFirst = 0; uiFirst < object.m_Instances.GetCount(); uiFirst += MaxInstancesPerComponent)
      {
        sComponentKey.SetFormat("{}#instances{}#{}", object.m_sKey, uiPart, uiFirst / MaxInstancesPerComponent);
        const plUuid componentGuid = plUsdPartitionImportData::MakeGuid(sImportId, sComponentKey);

        plAbstractObjectNode* pComponent = AddNode(ref_graph, componentGuid, plGetStaticRTTI<plInstancedMeshComponent>(), inout_types);
        pComponent->AddProperty("Active", true);
        pComponent->AddProperty("Mesh", sMesh);

        const plUInt32 uiEnd = plMath::Min(uiFirst + MaxInstancesPerComponent, object.m_Instances.GetCount());
        plVariantArray elements;
        elements.Reserve(uiEnd - uiFirst);

        for (plUInt32 i = uiFirst; i < uiEnd; ++i)
        {
          const plUuid elementGuid = plUsdPartitionImportData::MakeGuid(sImportId, sComponentKey, i + 1);
          const plTransform& t = object.m_Instances[i];

          plAbstractObjectNode* pElement = AddNode(ref_graph, elementGuid, plGetStaticRTTI<plMeshInstanceData>(), inout_types);
          pElement->AddProperty("LocalPosition", t.m_vPosition);
          pElement->AddProperty("LocalRotation", t.m_qRotation);
          pElement->AddProperty("LocalScaling", t.m_vScale);
          pElement->AddProperty("Color", plColor::White);
          elements.PushBack(elementGuid);
        }

        pComponent->AddProperty("InstanceData", elements);
        AddComponentToObject(pObject, componentGuid);
      }
    }

    return guid;
  }

  /// Every type and its base types, for the file's type list.
  void GatherTypes(plSet<const plRTTI*>& inout_types)
  {
    plDynamicArray<const plRTTI*> all;
    for (const plRTTI* pType : inout_types)
      all.PushBack(pType);

    for (const plRTTI* pType : all)
    {
      for (const plRTTI* pParent = pType->GetParentType(); pParent != nullptr; pParent = pParent->GetParentType())
        inout_types.Insert(pParent);
    }
  }

} // namespace

plStatus plUsdPartitionCellWriter::WriteLayer(plStringView sTargetFile, plStringView sStagingFolder, plStringView sImportId, const plUsdLayerIdentity& identity,
  plArrayPtr<const plUsdLayerGroup> groups, const plUsdLayerExtraObjects* pExtra, const plHashSet<plUuid>* pKnownGenerated, plUsdLayerWriteResult& out_result)
{
  out_result = plUsdLayerWriteResult();

  // The game objects this write makes; tagged objects with these GUIDs are replaced even when the index lacks them.
  plHashSet<plUuid> made;
  for (const plUsdLayerGroup& group : groups)
  {
    if (group.m_Objects.IsEmpty())
      continue;

    made.Insert(plUsdPartitionImportData::MakeGuid(sImportId, group.m_sKey));
    for (const plUsdImportObject* pObject : group.m_Objects)
      made.Insert(plUsdPartitionImportData::MakeGuid(sImportId, pObject->m_sKey));
  }
  if (pExtra != nullptr)
  {
    for (const plUuid& root : pExtra->m_Roots)
      made.Insert(root);
  }

  const plRTTI* pRootType = plRTTI::FindTypeByName("plSceneDocumentRoot");
  const plRTTI* pSettingsType = plRTTI::FindTypeByName("plLayerDocumentSettings");
  if (pRootType == nullptr || pSettingsType == nullptr)
    return plStatus("The scene editor plugin is not loaded.");

  plAssetDocumentInfo info;
  plAbstractObjectGraph objects;
  plAbstractObjectGraph types;
  plSet<const plRTTI*> newTypes;

  plVariantArray rootChildren;
  plBoundingBox bounds = plBoundingBox::MakeInvalid();
  plHashTable<plUuid, plDynamicArray<plUuid>> orphansByParent; ///< User objects that were children of generated ones.
  plUuid treeGuid;

  if (plOSFile::ExistsFile(sTargetFile))
  {
    plUniquePtr<plAbstractObjectGraph> pHeader, pObjects, pTypes;
    PL_SUCCEED_OR_RETURN(plDocument::ReadDocument(sTargetFile, pHeader, pObjects, pTypes));
    ReadHeaderInfo(*pHeader, info);

    const plAbstractObjectNode* pTree = pObjects->GetNodeByName("ObjectTree");
    if (pTree == nullptr)
      return plStatus(plFmt("'{}' has no object tree.", sTargetFile));
    treeGuid = pTree->GetGuid();

    // Tagged objects the import made; tagged copies the user made have other GUIDs and are kept.
    plHashSet<plUuid> generated;
    for (auto it = pObjects->GetAllNodes().GetIterator(); it.IsValid(); ++it)
    {
      if (HasGeneratedTag(it.Value()) && (pKnownGenerated == nullptr || pKnownGenerated->Contains(it.Key()) || made.Contains(it.Key())))
        generated.Insert(it.Key());
    }

    plDynamicArray<plUuid> keptRoots;
    for (const plVariant& child : GetArray(pTree, "Children"))
    {
      if (child.IsA<plUuid>() && !generated.Contains(child.Get<plUuid>()))
        keptRoots.PushBack(child.Get<plUuid>());
    }

    plDynamicArray<plUuid> orphans;
    for (const plUuid& guid : generated)
    {
      for (const plVariant& child : GetArray(pObjects->GetNode(guid), "Children"))
      {
        if (child.IsA<plUuid>() && !generated.Contains(child.Get<plUuid>()) && pObjects->GetNode(child.Get<plUuid>()) != nullptr)
        {
          orphans.PushBack(child.Get<plUuid>());
          orphansByParent[guid].PushBack(child.Get<plUuid>());
        }
      }
    }

    plHashSet<plUuid> kept;
    CollectKept(*pObjects, keptRoots, generated, kept);
    CollectKept(*pObjects, orphans, generated, kept);

    if (const plAbstractObjectNode::Property* pSettings = pTree->FindProperty("Settings"); pSettings != nullptr && pSettings->m_Value.IsA<plUuid>())
      kept.Insert(pSettings->m_Value.Get<plUuid>());

    objects.CopyNodeIntoGraph(pTree);
    for (const plUuid& guid : kept)
    {
      // A generated object the user put under a kept one goes back under its group, or away with its prim.
      if (const plAbstractObjectNode* pNode = pObjects->GetNode(guid))
        DropGeneratedChildren(objects.CopyNodeIntoGraph(pNode), generated);
    }

    for (const plUuid& root : keptRoots)
    {
      rootChildren.PushBack(root);
      if (const plAbstractObjectNode* pRoot = pObjects->GetNode(root))
      {
        if (const plAbstractObjectNode::Property* pPosition = pRoot->FindProperty("LocalPosition"); pPosition != nullptr && pPosition->m_Value.IsA<plVec3>())
          bounds.ExpandToInclude(plBoundingBox::MakeFromCenterAndHalfExtents(pPosition->m_Value.Get<plVec3>(), plVec3(0.5f)));
      }
    }
    out_result.m_uiUserRoots = keptRoots.GetCount();

    for (auto it = pTypes->GetAllNodes().GetIterator(); it.IsValid(); ++it)
      types.CopyNodeIntoGraph(it.Value());
  }
  else
  {
    info.m_DocumentID = identity.m_Layer;
    info.SetAssetsDocumentTypeName("Layer");

    const plUInt64 uiLayerSeed = plHashingUtils::xxHash64(&identity.m_Layer, sizeof(plUuid));
    const plUuid settingsGuid = plUsdPartitionImportData::MakeGuid(sImportId, "#settings", uiLayerSeed);
    AddNode(objects, settingsGuid, pSettingsType, newTypes);

    treeGuid = plUsdPartitionImportData::MakeGuid(sImportId, "#tree", uiLayerSeed);
    newTypes.Insert(pRootType);
    plAbstractObjectNode* pTree = objects.AddNode(treeGuid, pRootType->GetTypeName(), pRootType->GetTypeVersion(), "ObjectTree");
    pTree->AddProperty("Children", plVariantArray());
    pTree->AddProperty("Settings", settingsGuid);
  }

  plAbstractObjectNode* pTree = objects.GetNode(treeGuid);

  // The cell identity, in the settings and repeated in the header.
  if (identity.m_Partition.IsValid())
  {
    const plAbstractObjectNode::Property* pSettingsProperty = pTree->FindProperty("Settings");
    plAbstractObjectNode* pSettings = pSettingsProperty != nullptr && pSettingsProperty->m_Value.IsA<plUuid>() ? objects.GetNode(pSettingsProperty->m_Value.Get<plUuid>()) : nullptr;
    if (pSettings == nullptr)
      return plStatus(plFmt("'{}' has no settings to hold a cell's identity.", sTargetFile));

    SetProperty(pSettings, "OwningScene", identity.m_Scene);
    SetProperty(pSettings, "Partition", identity.m_Partition);
    SetProperty(pSettings, "CellX", identity.m_iX);
    SetProperty(pSettings, "CellY", identity.m_iY);
  }

  // The import's objects.
  plHashSet<plUuid> written;
  for (const plUsdLayerGroup& group : groups)
  {
    if (group.m_Objects.IsEmpty())
      continue;

    const plUuid groupGuid = plUsdPartitionImportData::MakeGuid(sImportId, group.m_sKey);
    plTransform groupTransform = plTransform::MakeIdentity();
    groupTransform.m_vPosition = group.m_vPosition;
    plAbstractObjectNode* pGroup = AddGameObject(objects, groupGuid, group.m_sName, groupTransform, false, newTypes);
    written.Insert(groupGuid);

    plVariantArray children;
    children.Reserve(group.m_Objects.GetCount());
    for (const plUsdImportObject* pObject : group.m_Objects)
    {
      if (objects.GetNode(plUsdPartitionImportData::MakeGuid(sImportId, pObject->m_sKey)) != nullptr)
        return plStatus(plFmt("Two objects of '{}' have the key '{}'.", sTargetFile, pObject->m_sKey));

      const plUuid guid = AddImportObject(objects, sImportId, *pObject, group.m_vPosition, newTypes);
      children.PushBack(guid);
      written.Insert(guid);

      if (pObject->m_Bounds.IsValid())
        bounds.ExpandToInclude(pObject->m_Bounds);
      else
        bounds.ExpandToInclude(plBoundingBox::MakeFromCenterAndHalfExtents(pObject->m_Transform.m_vPosition, plVec3(0.5f)));
    }

    pGroup->ChangeProperty("Children", children);
    rootChildren.PushBack(groupGuid);
    out_result.m_uiObjects += children.GetCount() + 1;
  }

  if (pExtra != nullptr && pExtra->m_pGraph != nullptr)
  {
    for (auto it = pExtra->m_pGraph->GetAllNodes().GetIterator(); it.IsValid(); ++it)
    {
      if (objects.GetNode(it.Key()) == nullptr)
      {
        objects.CopyNodeIntoGraph(it.Value());
        if (const plRTTI* pType = plRTTI::FindTypeByName(it.Value()->GetType()))
          newTypes.Insert(pType);
      }
    }

    for (const plUuid& root : pExtra->m_Roots)
    {
      rootChildren.PushBack(root);
      written.Insert(root);
      ++out_result.m_uiObjects;

      if (const plAbstractObjectNode* pRoot = objects.GetNode(root))
      {
        if (const plAbstractObjectNode::Property* pPosition = pRoot->FindProperty("LocalPosition"); pPosition != nullptr && pPosition->m_Value.IsA<plVec3>())
          bounds.ExpandToInclude(plBoundingBox::MakeFromCenterAndHalfExtents(pPosition->m_Value.Get<plVec3>(), plVec3(0.5f)));
      }
    }
  }

  // User objects that were under generated objects: under the new object of the same GUID, else roots.
  for (auto it = orphansByParent.GetIterator(); it.IsValid(); ++it)
  {
    plAbstractObjectNode* pParent = written.Contains(it.Key()) ? objects.GetNode(it.Key()) : nullptr;
    for (const plUuid& orphan : it.Value())
    {
      if (pParent != nullptr)
      {
        plVariantArray children = GetArray(pParent, "Children");
        children.PushBack(orphan);
        SetProperty(pParent, "Children", children);
      }
      else
      {
        rootChildren.PushBack(orphan);
        ++out_result.m_uiUserRoots;
      }
    }
  }

  SetProperty(pTree, "Children", rootChildren);
  out_result.m_uiRoots = rootChildren.GetCount();

  // Types of the new nodes, next to the file's own.
  {
    GatherTypes(newTypes);
    plAbstractObjectGraph serialized;
    plToolsSerializationUtils::SerializeTypes(newTypes, serialized);
    for (auto it = serialized.GetAllNodes().GetIterator(); it.IsValid(); ++it)
    {
      if (types.GetNode(it.Key()) == nullptr)
        types.CopyNodeIntoGraph(it.Value());
    }
  }

  plSceneLayerContentInfo* pContent = GetContentInfo(info);
  if (identity.m_Partition.IsValid())
  {
    pContent->m_OwningScene = identity.m_Scene;
    pContent->m_Partition = identity.m_Partition;
    pContent->m_iCellX = identity.m_iX;
    pContent->m_iCellY = identity.m_iY;
  }
  else if (identity.m_Scene.IsValid())
  {
    pContent->m_OwningScene = identity.m_Scene;
  }
  pContent->m_uiRootCount = rootChildren.GetCount();
  pContent->m_uiAllRootCount = rootChildren.GetCount();
  pContent->m_uiMisplacedCount = 0;
  pContent->m_bFromEngine = false;
  pContent->SetBounds(bounds);
  info.m_uiSettingsHash = 0;

  plAbstractObjectGraph header;
  {
    plRttiConverterContext context;
    plRttiConverterWriter writer(&header, &context, true, true);
    context.RegisterObject(info.m_DocumentID, info.GetDynamicRTTI(), &info);
    writer.AddObjectToGraph(&info, "Header");
  }

  plContiguousMemoryStreamStorage storage;
  {
    plMemoryStreamWriter writer(&storage);
    plAbstractGraphDdlSerializer::WriteDocument(writer, &header, &objects, &types, false);
  }

  plStringBuilder sStaging(sStagingFolder, "/", sTargetFile.GetFileNameAndExtension());
  sStaging.MakeCleanPath();

  const plArrayPtr<const plUInt8> content(storage.GetData(), storage.GetStorageSize32());
  if (plUsdBakeStore::WriteAbsoluteFile(sTargetFile, content, &out_result.m_bChanged, sStaging).Failed())
    return plStatus(plFmt("Could not write '{}'.", sTargetFile));

  return plStatus(PL_SUCCESS);
}
