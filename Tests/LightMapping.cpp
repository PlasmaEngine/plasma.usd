// Standalone native regression test. Link against the locally built plEditorPluginUsd and engine SDK import libraries.
#include <Foundation/Basics.h>
#include <Core/World/GameObject.h>
#include <EditorPluginUsd/LiveLink/UsdPrimComponents.h>
#include <Foundation/Configuration/Startup.h>
#include <Foundation/Application/Application.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Logging/ConsoleWriter.h>
#include <Foundation/Serialization/DdlSerializer.h>
#include <Foundation/Serialization/RttiConverter.h>
#include <Foundation/Types/ScopeExit.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <RendererCore/Lights/Photometry.h>
#include <RendererCore/Lights/RectLightComponent.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>
#include <ToolsFoundation/Reflection/ToolsReflectionUtils.h>
#include <ToolsFoundation/Serialization/DocumentObjectConverter.h>
#include <ToolsFoundation/Serialization/ToolsSerializationUtils.h>
#include <UsdBridge/UsdRuntime.h>
#include <cstdio>

int RunLightMappingTests()
{
  plDocumentObjectManager manager;
  plDocumentObject* object = manager.CreateObject(plGetStaticRTTI<plGameObject>());
  manager.AddObject(object, nullptr, "Children", -1);

  plUsdPrim prim;
  prim.m_Kind = plUsdPrimKind::Light;
  prim.m_Light.m_Shape = plUsdLightShape::Rect;
  prim.m_Light.m_bUnrealUnits = true;
  prim.m_Light.m_fWidth = 1.0f;
  prim.m_Light.m_fHeight = 2.0f;
  prim.m_Light.m_fLuminance = 2.0f;
  prim.m_Light.m_Color = plColor(0.2f, 0.5f, 1.0f);

  const plUuid componentId = plUuid::MakeUuid();
  plUsdPrimComponents::Apply(&manager, object, prim, componentId);
  plDocumentObject* component = manager.GetObject(componentId);

  if (component == nullptr)
  {
    return 1;
  }

  const auto& values = component->GetTypeAccessor();
  const float expected = 2.0f;
  const float actual = values.GetValue("Intensity").ConvertTo<float>();
  if (values.GetValue("IntensityUnit").ConvertTo<plInt64>() != plAreaLightUnit::Nits || plMath::Abs(actual - expected) > 1e-4f)
  {
    std::fprintf(stderr, "Blue rectangle: expected %f nits, got %f\n", expected, actual);
    return 2;
  }

  plUsdLight light;
  plUsdCamera camera;
  if (plUsdPrimComponents::ReadBack(component, prim, light, camera))
  {
    return 3;
  }

  if (manager.SetValue(component, "Intensity", actual * 2.0f).Failed() ||
      !plUsdPrimComponents::ReadBack(component, prim, light, camera) || light.m_bNormalize ||
      plMath::Abs(light.m_fLuminance - 4.0f) > 1e-4f)
  {
    return 4;
  }

  // Unnormalized USD lights preserve luminance when resized.
  if (manager.SetValue(component, "Width", 2.0f).Failed() ||
      !plUsdPrimComponents::ReadBack(component, prim, light, camera) ||
      plMath::Abs(light.m_fLuminance - 4.0f) > 1e-4f)
  {
    return 5;
  }

  manager.DestroyAllObjects();
  std::puts("Light mapping passed: source nits, unchanged read-back, intensity edit and resized emitter");
  return 0;
}

int VerifyBazaarLights(plStringView stageFile)
{
  plGlobalLog::AddLogWriter(plLogWriter::Console::LogMessageHandler);
  if (plUsdRuntime::Initialize().Failed())
  {
    return 10;
  }

  plUsdStage stage;
  if (stage.Open(stageFile, plUsdStage::Load::None).Failed())
  {
    return 11;
  }

  plDocumentObjectManager manager;
  PL_SCOPE_EXIT(manager.DestroyAllObjects());
  plDocumentObject* object = manager.CreateObject(plGetStaticRTTI<plGameObject>());
  manager.AddObject(object, nullptr, "Children", -1);

  for (const char* path : {"/Root/RectLight13", "/Root/RectLight16"})
  {
    plUsdPrim prim;
    if (stage.ReadPrim(path, prim).Failed())
    {
      return 12;
    }

    const plUuid componentId = plUuid::MakeUuid();
    plUsdPrimComponents::Apply(&manager, object, prim, componentId);
    const auto* component = manager.GetObject(componentId);
    if (component == nullptr)
    {
      return 13;
    }

    const auto& values = component->GetTypeAccessor();
    const float intensity = values.GetValue("Intensity").ConvertTo<float>();
    const auto unit = static_cast<plAreaLightUnit::Enum>(values.GetValue("IntensityUnit").ConvertTo<plInt64>());
    const float width = values.GetValue("Width").ConvertTo<float>();
    const float height = values.GetValue("Height").ConvertTo<float>();
    const float effectiveNits = plRectLightComponent::ComputeFarFieldCandela(intensity, unit, width, height, false) / (width * height);

    if (unit != plAreaLightUnit::Nits || plMath::Abs(intensity - prim.m_Light.m_fLuminance) > 1e-6f ||
        plMath::Abs(effectiveNits - prim.m_Light.m_fLuminance) > 1e-6f)
    {
      return 14;
    }

    // Follow scene/prefab document serialization, including type metadata and migration patches.
    plAbstractObjectGraph header, graph, types;
    plDocumentObjectConverterWriter documentWriter(&graph, &manager);
    documentWriter.AddObjectToGraph(component, "Light");
    plSet<const plRTTI*> usedTypes;
    plToolsReflectionUtils::GatherObjectTypes(component, usedTypes);
    plToolsSerializationUtils::SerializeTypes(usedTypes, types);

    plContiguousMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    plAbstractGraphDdlSerializer::WriteDocument(writer, &header, &graph, &types);
    plMemoryStreamReader stream(&storage);
    plUniquePtr<plAbstractObjectGraph> loadedHeader, loadedGraph, loadedTypes;
    if (plAbstractGraphDdlSerializer::ReadDocument(stream, loadedHeader, loadedGraph, loadedTypes).Failed())
    {
      std::fprintf(stderr, "Could not read the serialized light document for %s\n", path);
      return 15;
    }

    plDocumentObjectManager restoredManager;
    PL_SCOPE_EXIT(restoredManager.DestroyAllObjects());
    plDocumentObjectConverterReader reader(loadedGraph.Borrow(), &restoredManager, plDocumentObjectConverterReader::Mode::CreateOnly);
    const auto* savedNode = loadedGraph->GetNode(componentId);
    auto* restoredLight = reader.CreateObjectFromNode(savedNode);
    if (restoredLight == nullptr)
    {
      return 16;
    }

    restoredManager.AddObject(restoredLight, nullptr, "Children", -1);
    reader.ApplyPropertiesToObject(savedNode, restoredLight);
    const auto& restored = restoredLight->GetTypeAccessor();
    const float savedIntensity = restored.GetValue("Intensity").ConvertTo<float>();
    const auto savedUnit = static_cast<plAreaLightUnit::Enum>(restored.GetValue("IntensityUnit").ConvertTo<plInt64>());
    const float savedWidth = restored.GetValue("Width").ConvertTo<float>();
    const float savedHeight = restored.GetValue("Height").ConvertTo<float>();
    const float savedNits = plRectLightComponent::ComputeFarFieldCandela(savedIntensity, savedUnit, savedWidth, savedHeight, false) / (savedWidth * savedHeight);
    if (savedUnit != plAreaLightUnit::Nits || plMath::Abs(savedIntensity - prim.m_Light.m_fLuminance) > 1e-6f ||
        plMath::Abs(savedNits - prim.m_Light.m_fLuminance) > 1e-6f)
    {
      std::fprintf(stderr, "%s changed during serialization: %.9f nits\n", path, savedNits);
      return 17;
    }

    std::printf("%s: USD %.9f, document %.9f, effective %.9f, saved effective %.9f nits\n", path, prim.m_Light.m_fLuminance, intensity, effectiveNits, savedNits);
  }

  manager.DestroyAllObjects();
  return 0;
}

int VerifyLightRanges(plStringView fixture, plStringView outputFolder)
{
  if (plUsdRuntime::Initialize().Failed())
  {
    return 20;
  }

  plStringBuilder id;
  plConversionUtils::ToString(plUuid::MakeUuid(), id);
  const plString folder = plStringBuilder(outputFolder, "/Ranges-", id);
  const plString source = plStringBuilder(folder, "/LightRanges.usda");
  if (plOSFile::CreateDirectoryStructure(folder).Failed() || plOSFile::CopyFile(fixture, source).Failed())
  {
    return 21;
  }

  plString session, layer;
  plUsdStage stage;
  if (plUsdStage::CreateSession(source, "range-test", "plasma", session, layer).Failed() ||
      stage.Open(session).Failed() || stage.SetEditLayer(layer).Failed())
  {
    return 22;
  }

  plDocumentObjectManager manager;
  PL_SCOPE_EXIT(manager.DestroyAllObjects());
  auto* object = manager.CreateObject(plGetStaticRTTI<plGameObject>());
  manager.AddObject(object, nullptr, "Children", -1);

  const char* names[] = {"Rect", "Point", "Spot", "Disk", "Cylinder", "EmitterOnly", "InvalidRange"};
  const float ranges[] = {12, 8, 6, 4, 3, 0, 0};
  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(names); ++i)
  {
    plUsdPrim prim;
    const plString path = plStringBuilder("/World/", names[i]);
    if (stage.ReadPrim(path, prim).Failed() || plMath::Abs(prim.m_Light.m_fRange - ranges[i]) > 1e-5f)
    {
      return 23;
    }

    const plUuid componentId = plUuid::MakeUuid();
    plUsdPrimComponents::Apply(&manager, object, prim, componentId);
    auto* component = manager.GetObject(componentId);
    if (component == nullptr || plMath::Abs(component->GetTypeAccessor().GetValue("Range").ConvertTo<float>() - ranges[i]) > 1e-5f)
    {
      return 24;
    }

    plUsdLight light;
    plUsdCamera camera;
    if (plUsdPrimComponents::ReadBack(component, prim, light, camera))
    {
      return 25;
    }
    if (manager.SetValue(component, "Range", 9.0f).Failed() ||
        !plUsdPrimComponents::ReadBack(component, prim, light, camera) || light.m_fRange != 9.0f ||
        light.m_fRadius != prim.m_Light.m_fRadius || stage.AuthorLight(path, light).Failed())
    {
      return 26;
    }
  }

  if (stage.SaveEditLayer().Failed())
  {
    return 27;
  }
  stage.Close();
  if (stage.Open(session).Failed())
  {
    return 28;
  }
  for (const char* name : names)
  {
    plUsdPrim prim;
    if (stage.ReadPrim(plStringBuilder("/World/", name), prim).Failed() || plMath::Abs(prim.m_Light.m_fRange - 9.0f) > 1e-5f)
    {
      return 29;
    }
  }
  std::puts("Light ranges passed: all local shapes, stage units, extension precedence, emitter separation, absent/invalid fallback and session round-trip");
  return 0;
}

class LightMappingTestApp : public plApplication
{
public:
  LightMappingTestApp()
    : plApplication("UsdLightMappingTests")
  {
  }

  Execution Run() override
  {
    if (GetArgumentCount() == 4 && plStringView(GetArgument(1)) == "--range")
    {
      SetReturnCode(VerifyLightRanges(GetArgument(2), GetArgument(3)));
      return Execution::Quit;
    }

    int result = RunLightMappingTests();
    if (result == 0 && GetArgumentCount() > 1)
    {
      result = VerifyBazaarLights(GetArgument(1));
    }

    SetReturnCode(result);
    return Execution::Quit;
  }
};

PL_CONSOLEAPP_ENTRY_POINT(LightMappingTestApp);
