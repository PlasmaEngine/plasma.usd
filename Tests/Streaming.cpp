#include <Core/Graphics/Geometry.h>
#include <Core/World/GameObject.h>
#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/LiveLink/UsdLiveResources.h>
#include <EditorPluginUsd/LiveLink/UsdMeshCache.h>
#include <EditorPluginUsd/LiveLink/UsdPrimComponents.h>
#include <Foundation/Application/Application.h>
#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Logging/ConsoleWriter.h>
#include <Foundation/Serialization/AbstractObjectGraph.h>
#include <Foundation/System/StackTracer.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <Foundation/Types/ScopeExit.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <RendererCore/Meshes/InstancedMeshComponent.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>
#include <ToolsFoundation/Application/ApplicationServices.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>
#include <ToolsFoundation/Project/ToolsProject.h>
#include <ToolsFoundation/Serialization/DocumentObjectConverter.h>
#include <UsdBridge/UsdRuntime.h>
#include <cstdio>

namespace
{
  class TestDocument : public plDocument
  {
  public:
    TestDocument(plStringView path)
      : plDocument(path, PL_DEFAULT_NEW(plDocumentObjectManager))
    {
    }

    plDocumentInfo* CreateDocumentInfo() override { return PL_DEFAULT_NEW(plDocumentInfo); }
  };

  void CollectMeshResources(const plDocumentObject* object, plDynamicArray<plString>& resources, plDynamicArray<plString>& bindings)
  {
    if (object->GetType() == plGetStaticRTTI<plMeshComponent>())
    {
      resources.PushBack(object->GetTypeAccessor().GetValue("Mesh").ConvertTo<plString>());
      bindings.PushBack(object->GetTypeAccessor().GetValue("Materials", 0).ConvertTo<plString>());
    }
    for (const auto* child : object->GetChildren())
    {
      CollectMeshResources(child, resources, bindings);
    }
  }

  const plDocumentObject* FindPrimObject(const plDocumentObject* object, const plUsdLiveLink& link, plStringView path)
  {
    if (link.GetPrimPath(object->GetGuid()) == path)
    {
      return object;
    }
    for (const auto* child : object->GetChildren())
    {
      if (const auto* found = FindPrimObject(child, link, path))
      {
        return found;
      }
    }
    return nullptr;
  }

  int TestScheduler(plStringView stageFile, plStringView outputFolder)
  {
    std::puts("Scheduler: creating editor services");
    plEditorApp editor;
    plEditorEngineProcessConnection connection;
    std::puts("Scheduler: initializing USD");
    if (plUsdRuntime::Initialize().Failed())
    {
      return 40;
    }

    plStringBuilder guid;
    plConversionUtils::ToString(plUuid::MakeUuid(), guid);
    const plString folder = plStringBuilder(outputFolder, "/Scheduler-", guid);
    std::puts("Scheduler: opening isolated project");
    if (plOSFile::CreateDirectoryStructure(folder).Failed() || plToolsProject::CreateProject(plStringBuilder(folder, "/plProject")).Failed())
    {
      return 41;
    }
    PL_SCOPE_EXIT(plToolsProject::CloseProject());

    std::puts("Scheduler: creating live manager");
    plUsdLiveManager live;
    std::puts("Scheduler: creating document");
    TestDocument document(plStringBuilder(folder, "/Test.plScene"));
    auto* manager = document.GetObjectManager();
    auto* root = manager->CreateObject(plGetStaticRTTI<plGameObject>());
    manager->AddObject(root, nullptr, "Children", -1);
    plUsdLiveLink::Settings settings;
    settings.m_sStageFile = plStringBuilder(folder, "/Fixture.usda");
    if (plOSFile::CopyFile(stageFile, settings.m_sStageFile).Failed())
    {
      return 49;
    }
    plUsdLiveLink link(&document, root->GetGuid(), settings);
    std::puts("Scheduler: opening link");
    plString shared;

    for (plUInt32 pass = 0; pass < 2; ++pass)
    {
      const plTime start = plTime::Now();
      if (link.Open().Failed())
      {
        return 42;
      }
      bool preview = false;
      do
      {
        link.Update();
        plDynamicArray<plString> resources, bindings;
        CollectMeshResources(root, resources, bindings);
        for (const auto& resource : resources)
        {
          preview |= resource.EndsWith(".preview.plBinMesh");
        }
        if (plTime::Now() - start > plTime::MakeFromSeconds(30))
        {
          return 43;
        }
        plThreadUtils::Sleep(plTime::MakeFromMilliseconds(10));
      } while (link.GetPendingMeshCount() > 0);

      plDynamicArray<plString> resources, bindings;
      CollectMeshResources(root, resources, bindings);
      if (resources.GetCount() != 2 || resources[0] != resources[1] || bindings[0] == bindings[1] || (pass == 0 && !preview))
      {
        return 44;
      }
      if (pass == 1 && resources[0] != shared)
      {
        return 45;
      }

      shared = resources[0];
      std::printf("Scheduler %s passed: both bindings share one final resource; preview seen=%s; %.3f s\n",
        pass == 0 ? "cold" : "warm", preview ? "yes" : "no", (plTime::Now() - start).GetSeconds());

      if (pass == 0)
      {
        const plString busyMesh = plStringBuilder(folder, "/BusyGeometry");
        if (!plUsdMeshCache::TryAcquire(busyMesh, 5'000'000))
        {
          return 57;
        }
        PL_SCOPE_EXIT(plUsdMeshCache::Release(busyMesh));

        link.RequestBake(true);
        link.Update();
        link.Update();
        if (link.GetPendingMeshCount() != 0)
        {
          return 58;
        }
        std::puts("Scheduler cached adoption passed while the geometry build budget is exhausted");
      }
      link.Close(true);
    }

    auto* secondRoot = manager->CreateObject(plGetStaticRTTI<plGameObject>());
    manager->AddObject(secondRoot, nullptr, "Children", -1);
    plUsdLiveLink second(&document, secondRoot->GetGuid(), settings);
    if (second.Open().Failed())
    {
      return 46;
    }
    const plTime deadline = plTime::Now() + plTime::MakeFromSeconds(30);
    do
    {
      second.Update();
      if (plTime::Now() > deadline)
      {
        return 47;
      }
      plThreadUtils::Sleep(plTime::MakeFromMilliseconds(10));
    } while (second.GetPendingMeshCount() > 0);
    second.Update();
    plDynamicArray<plString> resources, bindings;
    CollectMeshResources(secondRoot, resources, bindings);
    if (resources.GetCount() != 2 || resources[0] != shared || resources[1] != shared)
    {
      return 48;
    }
    std::puts("Scheduler cross-link reuse passed");

    if (second.StartSession("regression", "plasma-test").Failed())
    {
      return 50;
    }
    second.Update();
    if (second.GetPendingMeshCount() != 0 ||
        second.SetSurfaceValue("/Root/Materials/Blue", plUsdStage::SurfaceValue::BaseColor, plColor(0, 1, 0)).Failed())
    {
      return 51;
    }
    second.Update();
    const auto* meshB = FindPrimObject(secondRoot, second, "/Root/B");
    if (meshB == nullptr || manager->SetValue(manager->GetObject(meshB->GetGuid()), "LocalPosition", plVec3(7, 0, 0)).Failed())
    {
      return 52;
    }
    second.Update();
    resources.Clear();
    bindings.Clear();
    CollectMeshResources(secondRoot, resources, bindings);
    if (second.GetPendingMeshCount() != 0 || resources.GetCount() != 2 || resources[0] != shared || resources[1] != shared)
    {
      return 53;
    }
    std::puts("Scheduler incremental transform/material edits passed: no geometry jobs or resource changes");

    const auto* meshA = FindPrimObject(secondRoot, second, "/Root/A");
    if (meshA == nullptr || second.SetVariantSelection(meshA->GetGuid(), "shape", "large").Failed())
    {
      return 54;
    }
    const plTime variantDeadline = plTime::Now() + plTime::MakeFromSeconds(30);
    do
    {
      second.Update();
      if (plTime::Now() > variantDeadline)
      {
        return 55;
      }
      plThreadUtils::Sleep(plTime::MakeFromMilliseconds(10));
    } while (second.GetPendingMeshCount() > 0);
    second.Update();
    resources.Clear();
    bindings.Clear();
    CollectMeshResources(secondRoot, resources, bindings);
    if (resources.GetCount() != 2 || resources[0] == resources[1] || !resources.Contains(shared))
    {
      return 56;
    }
    std::puts("Scheduler geometry variant edit passed: changed geometry replaced; unchanged geometry retained");
    return 0;
  }

  int TestMaterialState()
  {
    for (plUInt32 fixture = 0; fixture < 5; ++fixture)
    {
      plUsdSurface surface;
      surface.m_sMaterialPath = "/Materials/Regression";
      surface.m_bDoubleSided = fixture == 1;
      if (fixture == 2)
      {
        surface.m_fOpacity = 0.5f;
      }
      if (fixture >= 3)
      {
        surface.m_sOpacityTexture = "mask.dds";
      }

      plDynamicArray<plUInt8> bytes;
      plUsdLiveResources::BuildMaterial(surface, fixture == 4, bytes);
      plRawMemoryStreamReader reader(bytes.GetData(), bytes.GetCount());
      plAssetFileHeader header;
      if (header.Read(reader).Failed())
      {
        return 13;
      }

      plUInt8 version, compression;
      plString base, materialSurface, shader;
      reader >> version >> compression >> base >> materialSurface >> shader;
      if (version != 8 || compression != 0 || shader != "Shaders/Materials/DefaultMaterial.plShader")
      {
        return 14;
      }

      // Apply the serialized, flattened state after an unrelated unlit, transparent, two-sided draw, as the
      // render context does. Missing values retain this preceding draw's state instead of shader defaults.
      plHashTable<plString, plString> state;
      state["SHADING_MODE"] = "SHADING_MODE_FULLBRIGHT";
      state["SHADING_WORKFLOW"] = "SHADING_WORKFLOW_SPECULAR_GLOSS";
      state["BLEND_MODE"] = "BLEND_MODE_TRANSPARENT";
      state["TWO_SIDED"] = "TRUE";
      state["ENABLE_WEATHER"] = "TRUE";
      state["SNOW_VERTEX_COLOR"] = "TRUE";
      state["SSS_PROFILE"] = "SSS_PROFILE_1";

      plUInt16 count;
      reader >> count;
      for (plUInt16 i = 0; i < count; ++i)
      {
        plString name, value;
        reader >> name >> value;
        state[name] = value;
      }

      const char* blend = fixture == 2 || fixture == 4 ? "BLEND_MODE_TRANSPARENT" : fixture == 3 ? "BLEND_MODE_MASKED"
                                                                                                 : "BLEND_MODE_OPAQUE";
      if (state["SHADING_MODE"] != "SHADING_MODE_LIT" || state["SHADING_WORKFLOW"] != "SHADING_WORKFLOW_METALLIC_ROUGHNESS" ||
          state["BLEND_MODE"] != blend || state["TWO_SIDED"] != (surface.m_bDoubleSided ? "TRUE" : "FALSE") ||
          state["ENABLE_WEATHER"] != "FALSE" || state["SNOW_VERTEX_COLOR"] != "FALSE" || state["SSS_PROFILE"] != "SSS_PROFILE_NONE")
      {
        std::printf("Material regression failed: fixture %u inherited the preceding draw's shader state\n", fixture);
        return 15;
      }
    }

    std::puts("Material regression passed: opaque, two-sided, transparent, masked and soft-mask materials reset preceding FULLBRIGHT/blend/workflow state");
    return 0;
  }

  class Source : public plUsdGeometrySource
  {
  public:
    plGeometry m_Geometry;
    plDynamicArray<plUInt32> m_Groups;
    mutable plUInt32 m_uiBuilds = 0;

    plStatus Build(plGeometry& geometry, plDynamicArray<plUInt32>* groups) const override
    {
      ++m_uiBuilds;
      geometry = m_Geometry;
      if (groups != nullptr)
      {
        *groups = m_Groups;
      }

      return plStatus(PL_SUCCESS);
    }

    plUInt64 GetHash() const override
    {
      return 42;
    }
  };

  int TestSharedCache(plStringView folder)
  {
    const plString mesh = plStringBuilder(folder, "/SharedGeometry");
    const plString other = plStringBuilder(folder, "/OtherGeometry");
    if (!plUsdMeshCache::TryAcquire(mesh, 3'000'000) || plUsdMeshCache::TryAcquire(mesh) ||
        plUsdMeshCache::CanAdmit(2'000'000) || plUsdMeshCache::TryAcquire(other, 2'000'000))
    {
      return 20;
    }

    plUsdMeshCache::Release(mesh);
    if (!plUsdMeshCache::TryAcquire(other, 5'000'000) || plUsdMeshCache::CanAdmit(1))
    {
      return 21;
    }
    plUsdMeshCache::Release(other);
    if (!plUsdMeshCache::CanAdmit(1))
    {
      return 22;
    }

    Source source;
    source.m_Geometry.AddVertex(plVec3(0, 0, 0), plVec3(0, 0, 1), plVec2(0, 0));
    source.m_Geometry.AddVertex(plVec3(1, 0, 0), plVec3(0, 0, 1), plVec2(1, 0));
    source.m_Geometry.AddVertex(plVec3(0, 1, 0), plVec3(0, 0, 1), plVec2(0, 1));
    plUInt32 triangle[] = {0, 1, 2};
    source.m_Geometry.AddPolygon(triangle, false);
    source.m_Groups.PushBack(1);
    const plString material = plUsdLiveResources::DefaultMaterial;
    const auto materials = plArrayPtr<const plString>(&material, 1);
    plDynamicArray<plDynamicArray<plUInt8>> parts;
    if (plUsdLiveResources::BuildMeshParts(source, materials, 42, true, parts, {}, {}, []()
          { return true; })
          .Succeeded() ||
        source.m_uiBuilds != 0)
    {
      return 23;
    }

    plDynamicArray<plUInt8> preview;
    if (plUsdLiveResources::BuildMeshPreview(source.m_Geometry, source.m_Groups, materials, 42, preview).Failed() ||
        plUsdBakeStore::WriteFile(plUsdMeshCache::GetPreviewResource(mesh), preview).Failed() ||
        plUsdMeshCache::Publish(mesh, 0, true).Failed())
    {
      return 24;
    }

    auto cached = plUsdMeshCache::Read(mesh);
    if (cached.m_uiParts != 0 || cached.m_sPreviewResource.IsEmpty())
    {
      return 25;
    }

    if (plUsdLiveResources::BuildMeshParts(source, materials, 42, true, parts).Failed() || parts.GetCount() != 1 ||
        plUsdBakeStore::WriteFile(plUsdBakeStore::GetMeshResource(mesh, 0), parts[0]).Failed() ||
        plUsdMeshCache::Publish(mesh, 1, true).Failed())
    {
      return 26;
    }

    cached = plUsdMeshCache::Read(mesh);
    if (cached.m_uiParts != 1 || cached.m_sPreviewResource.IsEmpty() || cached.m_PartSizes[0] != parts[0].GetCount())
    {
      return 27;
    }

    // A valid old preview must be rejected without invalidating its completed full-quality mesh.
    const plString oldPreview = plStringBuilder(mesh, ".preview.plBinMesh");
    plUsdBakeStore::Manifest legacy;
    legacy.m_Meshes[mesh] = cached;
    legacy.m_Meshes[mesh].m_sPreviewResource = oldPreview;
    if (plUsdBakeStore::WriteFile(oldPreview, preview).Failed() ||
        plUsdBakeStore::SaveManifestFile(plUsdBakeStore::ToAbsolutePath(plStringBuilder(mesh, ".cache.json")), legacy).Failed())
    {
      return 59;
    }

    cached = plUsdMeshCache::Read(mesh);
    if (cached.m_uiParts != 1 || !cached.m_sPreviewResource.IsEmpty() || plUsdMeshCache::Publish(mesh, 1, true).Failed())
    {
      return 60;
    }

    if (plUsdBakeStore::WriteFile(plUsdBakeStore::GetMeshResource(mesh, 0), parts[0].GetArrayPtr().GetSubArray(0, 64)).Failed())
    {
      return 28;
    }
    cached = plUsdMeshCache::Read(mesh);
    if (cached.m_uiParts != 0 || cached.m_sPreviewResource.IsEmpty())
    {
      return 29;
    }

    std::puts("Shared cache passed: coalesced leases, global memory admission, oversized progress, pre-build cancellation, preview resume, final reopen, preview migration and truncated-part recovery");
    return 0;
  }

  int TestMaterialBindings()
  {
    plDocumentObjectManager manager;
    PL_SCOPE_EXIT(manager.DestroyAllObjects());
    auto* object = manager.CreateObject(plGetStaticRTTI<plGameObject>());
    manager.AddObject(object, nullptr, "Children", -1);
    plUsdMaterialGroup a, b;
    a.m_bHasSurface = b.m_bHasSurface = true;
    a.m_Surface.m_sMaterialPath = "/Materials/A";
    b.m_Surface.m_sMaterialPath = "/Materials/B";
    plUsdMaterialGroup groups[] = {a, b};
    for (const auto* type : {plGetStaticRTTI<plMeshComponent>(), plGetStaticRTTI<plInstancedMeshComponent>()})
    {
      auto* component = manager.CreateObject(type);
      manager.AddObject(component, object, "Components", -1);
      plUsdPrimComponents::ApplyMaterials(&manager, component, "link-a", groups);
      const auto& data = component->GetTypeAccessor();
      if (data.GetCount("Materials") != 2 || data.GetValue("Materials", 1) != plVariant(plUsdBakeStore::GetMaterialResource("link-a", b)))
      {
        return 30;
      }

      plUsdPrimComponents::ApplyMaterials(&manager, component, "link-b", plArrayPtr<const plUsdMaterialGroup>(&b, 1));
      if (data.GetCount("Materials") != 1 || data.GetValue("Materials", 0) != plVariant(plUsdBakeStore::GetMaterialResource("link-b", b)))
      {
        return 31;
      }

      plAbstractObjectGraph graph;
      plDocumentObjectConverterWriter writer(&graph, &manager);
      writer.AddObjectToGraph(component, "Mesh");
      plDocumentObjectManager restored;
      plDocumentObjectConverterReader reader(&graph, &restored, plDocumentObjectConverterReader::Mode::CreateOnly);
      const auto* node = graph.GetNodeByName("Mesh");
      auto* copy = reader.CreateObjectFromNode(node);
      reader.ApplyPropertiesToObject(node, copy);
      if (copy->GetTypeAccessor().GetValue("Materials", 0) != data.GetValue("Materials", 0))
      {
        return 32;
      }
      restored.DestroyObject(copy);
    }

    std::puts("Material binding passed: separate links, binding replacement/removal, mesh and instancer serialization");
    return 0;
  }

  int TestParts()
  {
    Source source;
    constexpr plUInt32 TriangleCount = 60000;
    for (plUInt32 face = 0; face < TriangleCount; ++face)
    {
      const float x = (float)(face % 300);
      const float y = (float)(face / 300);
      const plUInt32 first = source.m_Geometry.GetVertices().GetCount();
      source.m_Geometry.AddVertex(plVec3(x, y, 0), plVec3(0, 0, 1), plVec2(0, 0));
      source.m_Geometry.AddVertex(plVec3(x + 0.8f, y, 0), plVec3(0, 0, 1), plVec2(2, 0));
      source.m_Geometry.AddVertex(plVec3(x, y + 0.8f, 0), plVec3(0, 0, 1), plVec2(0, 2));
      plUInt32 indices[] = {first, first + 1, first + 2};
      source.m_Geometry.AddPolygon(indices, false);
    }

    source.m_Groups.PushBack(TriangleCount / 2);
    source.m_Groups.PushBack(TriangleCount / 2);
    plString materials[] = {"MaterialA", "MaterialB"};
    plDynamicArray<plDynamicArray<plUInt8>> baseline, streamed;
    if (plUsdLiveResources::BuildMeshParts(source, materials, 42, false, baseline).Failed() || baseline.GetCount() < 2)
    {
      return 1;
    }

    bool previewSeen = false;
    plUInt32 received = 0;
    auto preview = [&](plUInt32 part, plArrayPtr<const plUInt8> bytes) -> plStatus
    {
      plRawMemoryStreamReader reader(bytes.GetPtr(), bytes.GetCount());
      plAssetFileHeader header;
      plMeshResourceDescriptor descriptor;
      if (part != 0 || received != 0 || header.Read(reader).Failed() || descriptor.Load(reader).Failed() ||
          descriptor.MeshBufferDesc().GetPrimitiveCount() != TriangleCount || descriptor.GetMaterials().GetCount() != 2 ||
          descriptor.GetSubMeshes().GetCount() != 2 || !descriptor.GetMeshlets().IsEmpty())
      {
        return plStatus("The preview dropped disconnected surfaces, lost material groups or ran too late.");
      }

      const auto box = descriptor.GetBounds().GetBox();
      if (box.m_vMax.x < 299.7f || box.m_vMax.y < 199.7f)
      {
        return plStatus("The coarse preview lost the source bounds.");
      }

      previewSeen = true;
      return plStatus(PL_SUCCESS);
    };
    auto part = [&](plUInt32 index, plArrayPtr<const plUInt8> bytes) -> plStatus
    {
      if (!previewSeen || index != received || index >= baseline.GetCount() || bytes.GetCount() != baseline[index].GetCount() ||
          !plMemoryUtils::IsEqual(bytes.GetPtr(), baseline[index].GetData(), bytes.GetCount()))
      {
        return plStatus("Streamed full-quality part differs from the ordinary build or arrived out of order.");
      }

      ++received;
      return plStatus(PL_SUCCESS);
    };

    if (plUsdLiveResources::BuildMeshParts(source, materials, 42, false, streamed, part, preview).Failed() ||
        !previewSeen || received != baseline.GetCount() || streamed.GetCount() != baseline.GetCount())
    {
      return 2;
    }

    for (const auto& bytes : streamed)
    {
      if (!bytes.IsEmpty())
      {
        return 3;
      }
    }

    plUInt32 failedParts = 0;
    auto fail = [&](plUInt32, plArrayPtr<const plUInt8>) -> plStatus
    {
      ++failedParts;
      return plStatus("Simulated write failure.");
    };
    if (plUsdLiveResources::BuildMeshParts(source, materials, 42, false, streamed, fail).Succeeded() || failedParts != 1)
    {
      return 4;
    }

    std::puts("Streaming passed: preview first, disconnected surfaces/materials/bounds retained, byte-identical final parts, released buffers and write failure propagation");
    return 0;
  }

  int TestPreviewSurface()
  {
    plGeometry geometry;
    constexpr plUInt32 Side = 200;
    for (plUInt32 y = 0; y <= Side; ++y)
    {
      for (plUInt32 x = 0; x <= Side; ++x)
      {
        geometry.AddVertex(plVec3((float)x, (float)y, 0), plVec3(0, 0, 1), plVec2((float)x / Side, (float)y / Side));
      }
    }

    for (plUInt32 y = 0; y < Side; ++y)
    {
      for (plUInt32 x = 0; x < Side; ++x)
      {
        if (x >= 90 && x < 110 && y >= 90 && y < 110)
        {
          continue; // An authored opening must remain an opening.
        }

        const plUInt32 a = y * (Side + 1) + x;
        plUInt32 first[] = {a, a + 1, a + Side + 1};
        plUInt32 second[] = {a + 1, a + Side + 2, a + Side + 1};
        geometry.AddPolygon(first, false);
        geometry.AddPolygon(second, false);
      }
    }

    const plUInt32 triangles = geometry.GetPolygons().GetCount();
    const plString material = plUsdLiveResources::DefaultMaterial;
    plDynamicArray<plUInt8> bytes;
    if (plUsdLiveResources::BuildMeshPreview(geometry, plArrayPtr<const plUInt32>(&triangles, 1), plArrayPtr<const plString>(&material, 1), 43, bytes).Failed())
    {
      return 61;
    }

    plRawMemoryStreamReader reader(bytes.GetData(), bytes.GetCount());
    plAssetFileHeader header;
    plMeshResourceDescriptor descriptor;
    if (header.Read(reader).Failed() || descriptor.Load(reader).Failed())
    {
      return 62;
    }

    const auto& mesh = descriptor.MeshBufferDesc();
    if (mesh.GetPrimitiveCount() >= triangles)
    {
      return 63;
    }

    const auto positions = mesh.GetPositionData();
    const auto& indices = mesh.GetIndexBufferData();
    double area = 0.0;
    for (plUInt32 i = 0; i < mesh.GetPrimitiveCount() * 3; i += 3)
    {
      const auto ReadIndex = [&](plUInt32 index) -> plUInt32
      {
        return mesh.Uses32BitIndices() ? reinterpret_cast<const plUInt32*>(indices.GetPtr())[index] : reinterpret_cast<const plUInt16*>(indices.GetPtr())[index];
      };

      const plVec3 a = positions[ReadIndex(i)];
      const plVec3 b = positions[ReadIndex(i + 1)];
      const plVec3 c = positions[ReadIndex(i + 2)];
      area += (b - a).CrossRH(c - a).z * 0.5;

      const plVec3 center(100, 100, 0);
      if ((b - a).CrossRH(center - a).z >= 0.0f && (c - b).CrossRH(center - b).z >= 0.0f && (a - c).CrossRH(center - c).z >= 0.0f)
      {
        return 64;
      }
    }

    if (plMath::Abs(area - 39600.0) > 0.01)
    {
      return 65;
    }

    std::puts("Preview surface passed: connected geometry simplifies while preserving filled area, silhouette and authored opening");
    return 0;
  }

  int TestCheckpoint(plStringView file)
  {
    plUsdBakeStore::Manifest checkpoint;
    checkpoint.m_Meshes["completed"] = {123456, 2};
    checkpoint.m_Prims["/Completed"] = {"completed", 2};
    // No prefab exists yet: a cold load is still running.
    if (plUsdBakeStore::SaveManifestFile(file, checkpoint).Failed())
    {
      return 6;
    }

    plUsdBakeStore::Manifest restored;
    plUsdBakeStore::LoadManifestFile(file, restored);
    const auto* mesh = restored.m_Meshes.GetValue("completed");
    if (restored.m_Prefab.IsValid() || mesh == nullptr || mesh->m_uiFileStamp != 123456 || mesh->m_uiParts != 2 ||
        restored.m_Meshes.Contains("preview"))
    {
      return 7;
    }

    checkpoint.m_Meshes["another-completed"] = {789, 1};
    if (plUsdBakeStore::SaveManifestFile(file, checkpoint).Failed())
    {
      return 8;
    }
    plUsdBakeStore::LoadManifestFile(file, restored);
    if (restored.m_Meshes.GetCount() != 2)
    {
      return 9;
    }

    std::puts("Checkpoint passed: completed meshes survive a pre-bake manifest save/reopen and atomic replacement");
    return 0;
  }

  int Benchmark(plStringView file)
  {
    if (plUsdRuntime::Initialize().Failed())
    {
      return 10;
    }
    plUsdStage stage;
    plStatus status = stage.Open(file, plUsdStage::Load::None);
    if (status.Failed())
    {
      return 10;
    }

    const plTime captureStart = plTime::Now();
    auto source = stage.CaptureGeometry("/Root/SM_His_Baz_Tableware_Plate_Clay_S_03_4", status);
    if (!source)
    {
      return 11;
    }

    std::printf("Benchmark capture: %.3f seconds\n", (plTime::Now() - captureStart).GetSeconds());
    const plTime start = plTime::Now();
    bool previewSeen = false;
    plUInt32 parts = 0;
    auto preview = [&](plUInt32, plArrayPtr<const plUInt8> bytes) -> plStatus
    {
      plRawMemoryStreamReader reader(bytes.GetPtr(), bytes.GetCount());
      plAssetFileHeader header;
      plMeshResourceDescriptor descriptor;
      PL_SUCCEED_OR_RETURN(header.Read(reader));
      PL_SUCCEED_OR_RETURN(descriptor.Load(reader));
      std::printf("Benchmark coarse ready: %.3f seconds, %u triangles, %u bytes\n",
        (plTime::Now() - start).GetSeconds(), descriptor.MeshBufferDesc().GetPrimitiveCount(), bytes.GetCount());
      previewSeen = true;
      return plStatus(PL_SUCCESS);
    };
    auto part = [&](plUInt32 index, plArrayPtr<const plUInt8>) -> plStatus
    {
      if (index == 0)
      {
        std::printf("Benchmark first final part: %.3f seconds\n", (plTime::Now() - start).GetSeconds());
      }

      ++parts;
      return plStatus(PL_SUCCESS);
    };

    plString material = "";
    plDynamicArray<plDynamicArray<plUInt8>> buffers;
    status = plUsdLiveResources::BuildMeshParts(*source, plArrayPtr<const plString>(&material, 1), source->GetHash(), true, buffers, part, preview);
    std::printf("Benchmark full quality ready: %.3f seconds, %u parts\n", (plTime::Now() - start).GetSeconds(), parts);
    return status.Succeeded() && previewSeen && parts > 0 ? 0 : 12;
  }
} // namespace

class StreamingTests : public plApplication
{
public:
  StreamingTests()
    : plApplication("UsdStreamingTests")
  {
  }

  Execution Run() override
  {
    std::setbuf(stdout, nullptr);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* exception) -> LONG
      {
        void* addresses[64];
        plArrayPtr<void*> trace(addresses);
        const plUInt32 count = plStackTracer::GetStackTrace(trace, exception->ContextRecord);
        plStackTracer::ResolveStackTrace(trace.GetSubArray(0, count), [](const char* line) { std::fputs(line, stderr); });
        return EXCEPTION_EXECUTE_HANDLER; });
    plSetAssertHandler([](const char* file, plUInt32 line, const char*, const char* expression, const char* message) -> bool
      {
        std::fprintf(stderr, "Assertion at %s:%u: %s (%s)\n", file, line, expression, message);
        std::exit(99); });
    plGlobalLog::AddLogWriter(plLogWriter::Console::LogMessageHandler);
    if (GetArgumentCount() > 3 && plStringView(GetArgument(1)) == "--scheduler")
    {
      SetReturnCode(TestScheduler(GetArgument(2), GetArgument(3)));
      return Execution::Quit;
    }
    int result = TestMaterialState();
    if (GetArgumentCount() > 1 && plStringView(GetArgument(1)) == "--material-test")
    {
      SetReturnCode(result);
      return Execution::Quit;
    }

    if (result == 0)
    {
      result = TestMaterialBindings();
    }
    if (result == 0)
    {
      result = TestPreviewSurface();
    }
    if (result == 0)
    {
      result = TestParts();
    }
    if (result == 0 && GetArgumentCount() > 1)
    {
      result = TestCheckpoint(GetArgument(1));
      if (result == 0)
      {
        result = TestSharedCache(plStringView(GetArgument(1)).GetFileDirectory());
      }
    }

    if (result == 0 && GetArgumentCount() > 2)
    {
      result = Benchmark(GetArgument(2));
    }

    SetReturnCode(result);
    return Execution::Quit;
  }
};

PL_CONSOLEAPP_ENTRY_POINT(StreamingTests);
