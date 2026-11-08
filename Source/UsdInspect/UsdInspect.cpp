#include <UsdInspect/UsdInspectPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Foundation/Configuration/Startup.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Logging/ConsoleWriter.h>
#include <Foundation/Strings/PathUtils.h>
#include <Foundation/Strings/StringConversion.h>
#include <ModelImporter2/MeshletLodGenerator.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>
#include <UsdBridge/UsdBasis.h>
#include <UsdBridge/UsdMtlxFileFormat.h>
#include <UsdBridge/UsdPartitionScan.h>
#include <UsdBridge/UsdRuntime.h>
#include <UsdBridge/UsdStage.h>
#include <UsdBridge/UsdTexture.h>
#include <UsdBridge/UsdTangents.h>

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>

#include <psapi.h>

// windows.h defines near and far as empty macros; this file has a lambda called near.
#undef near
#undef far

// plUsdInspect --selftest
// plUsdInspect --texture <output.dds> <colour file> [opacity file]
// plUsdInspect <stage.usda> [--geometry]
// plUsdInspect <stage.usda> --profile [max mesh builds] [tangents]
// plUsdInspect <asset.usda> --roundtrip <primPath>
// plUsdInspect <stage.usda | document.mtlx> --materialx <absolute path of plEditorPluginMaterialX.dll>
// plUsdInspect --partition-list <stage> [primPath]
// plUsdInspect --partition-scan <stage> [--roots p1,p2] [--cell 128] [--variant set=value] [--batch 8] [--split <cell>] [--capture 50] [--keys 1]
//              [--max-cells 65536] [--max-payloads 1]
//
// --partition-scan opens the stage without payloads and scans one root at a time as a world partition import would:
// loads it under the variant overrides, collects its placeables, captures and builds a few prototypes and loose gprims,
// splits the loose meshes larger than --split along the cells, and unloads it again. Prints the plan statistics per root
// and in total. --batch 0 places every placeable as its own object. Without --roots it scans the highest payload prims.
// A root with more than --max-payloads unloaded payloads below it is refused (0: no limit).
//
// --roundtrip creates a live session beside the asset, moves the prim through the session's participant layer, saves,
// reopens the session from disk and checks the prim came back where it was put. The session folder is left behind for
// inspection in other tools.
//
// --materialx loads plasma.materialx's editor plugin, which .mtlx layers are read through, and prints the stage. Given a
// .mtlx, it first prints the layer the document reads into as .usda.

namespace
{
  const char* KindName(plUsdPrimKind kind)
  {
    switch (kind)
    {
      case plUsdPrimKind::Group:
        return "group";
      case plUsdPrimKind::Mesh:
        return "mesh";
      case plUsdPrimKind::Gprim:
        return "shape";
      case plUsdPrimKind::Light:
        return "light";
      case plUsdPrimKind::Camera:
        return "camera";
      case plUsdPrimKind::Instancer:
        return "instancer";
    }
    return "?";
  }

  int Fail(plStringView sMessage)
  {
    plLog::Error("{}", sMessage);
    return 1;
  }

  int FailFmt(const plFormatString& message)
  {
    plStringBuilder sTemp;
    return Fail(message.GetText(sTemp));
  }

  bool EqualTangentGeometry(const plGeometry& reference, const plGeometry& actual)
  {
    if (reference.GetPolygons().GetCount() != actual.GetPolygons().GetCount())
    {
      return false;
    }

    for (plUInt32 face = 0; face < reference.GetPolygons().GetCount(); ++face)
    {
      const auto& a = reference.GetPolygons()[face].m_Vertices;
      const auto& b = actual.GetPolygons()[face].m_Vertices;
      if (a.GetCount() != b.GetCount())
      {
        return false;
      }

      for (plUInt32 corner = 0; corner < a.GetCount(); ++corner)
      {
        const auto& av = reference.GetVertices()[a[corner]];
        const auto& bv = actual.GetVertices()[b[corner]];
        if (av.m_vPosition != bv.m_vPosition || av.m_vNormal != bv.m_vNormal || av.m_vTexCoord != bv.m_vTexCoord ||
            av.m_Color != bv.m_Color || av.m_BoneIndices != bv.m_BoneIndices || av.m_BoneWeights != bv.m_BoneWeights ||
            !av.m_vTangent.IsEqual(bv.m_vTangent, 1e-5f) || av.m_fBiTangentSign != bv.m_fBiTangentSign)
        {
          return false;
        }
      }
    }

    return true;
  }

  int TangentRegression()
  {
    for (int fixture = 0; fixture < 4; ++fixture)
    {
      plGeometry reference;
      const plVec3 positions[] = {plVec3(0, 0, 0), plVec3(1, 0, 0), plVec3(1, 1, 0), plVec3(0, 1, 0)};
      const plVec2 uvs[] = {plVec2(0, 0), plVec2(1, 0), plVec2(1, 1), plVec2(0, 1)};
      for (plUInt32 i = 0; i < 4; ++i)
      {
        plVec2 uv = uvs[i];
        if (fixture == 1)
        {
          uv.x = -uv.x;
        }
        else if (fixture == 2)
        {
          uv.SetZero();
        }

        reference.AddVertex(positions[i], plVec3(0, 0, 1), uv, plColor(0.2f, 0.4f, 0.6f), plVec4U16(1, 2, 3, 4));
      }

      plUInt32 quad[] = {0, 1, 2, 3};
      plUInt32 first[] = {0, 1, 2};
      plUInt32 second[] = {0, 2, 3};
      if (fixture == 3)
      {
        reference.AddPolygon(quad, false);
      }
      else
      {
        reference.AddPolygon(first, false);
        reference.AddPolygon(second, false);
      }

      plGeometry actual = reference;
      reference.ComputeTangents();
      const plStatus result = plUsdTangents::Compute(actual);
      if (result.Failed() || !EqualTangentGeometry(reference, actual))
      {
        return FailFmt(plFmt("Tangent regression {} differs from the engine's MikkTSpace result", fixture));
      }
    }

    plLog::Success("Tangent regression passed: triangles, mirrored UVs, degenerate UVs, quads and vertex attributes");
    return 0;
  }

  int MeshProfile(plStringView file, plStringView path)
  {
    plUsdStage stage;
    plStatus status = stage.Open(file, plUsdStage::Load::None);
    if (status.Failed())
    {
      return Fail(status.m_sMessage);
    }

    auto source = stage.CaptureGeometry(path, status);
    if (!source)
    {
      return Fail(status.m_sMessage);
    }

    plGeometry reference, actual;
    if (source->Build(reference).Failed() || source->Build(actual).Failed())
    {
      return Fail("Could not build benchmark geometry");
    }

    plLog::Info("Benchmark {}: {} vertices, {} triangles", path, reference.GetVertices().GetCount(), reference.GetPolygons().GetCount());
    plTime start = plTime::Now();
    reference.ComputeTangents();
    const double oldMs = (plTime::Now() - start).GetMilliseconds();
    start = plTime::Now();
    status = plUsdTangents::Compute(actual);
    const double newMs = (plTime::Now() - start).GetMilliseconds();
    if (status.Failed() || !EqualTangentGeometry(reference, actual))
    {
      return Fail("Optimized tangents differ from engine tangents on the benchmark mesh");
    }

    plLog::Info("Engine tangents {} ms; USD tangents {} ms; matching per-corner results", plArgF(oldMs, 1), plArgF(newMs, 1));
    return 0;
  }

  int LightRegression(plStringView sFile)
  {
    plUsdStage stage;
    const plStatus opened = stage.Open(sFile);
    if (opened.Failed())
    {
      return Fail(opened.m_sMessage);
    }

    if (!stage.IsUnrealAuthored())
    {
      return Fail("Unreal light fixture was not recognized");
    }

    const plVec3d direction = stage.GetBasis().DirectionToPlasma(plVec3d(0, 0, -1));
    const plVec3 expected((float)direction.x, (float)direction.y, (float)direction.z);
    plTransform panelTransform = plTransform::MakeIdentity();

    for (const char* path : {"/World/Panel", "/World/Disk", "/World/Camera"})
    {
      plUsdPrim prim;
      if (stage.ReadPrim(path, prim).Failed())
      {
        return FailFmt(plFmt("missing regression prim {}", path));
      }

      const bool area = prim.m_Kind == plUsdPrimKind::Light;
      const plVec3 forward = prim.m_LocalTransform.m_qRotation * plVec3(area ? -1.0f : 1.0f, 0, 0);
      if (!forward.IsEqual(expected, 1e-5f))
      {
        return FailFmt(plFmt("incorrect emission/view direction for {}", path));
      }

      if (plStringView(path) == "/World/Panel")
      {
        panelTransform = prim.m_LocalTransform;
        const float lumens = prim.m_Light.m_fLuminance * prim.m_Light.GetUnrealLumensPerNit();
        if (plMath::Abs(lumens - 4.0f * plMath::Pi<float>()) > 1e-4f)
        {
          return Fail("2 nits over a 2 m^2 blue rectangle must import as 12.56637 lumens");
        }
      }
    }

    plUsdPrim marker;
    if (stage.ReadPrim("/World/Panel/Marker", marker).Failed() ||
        !panelTransform.TransformPosition(marker.m_LocalTransform.m_vPosition).IsEqual(expected, 1e-5f))
    {
      return Fail("area-light correction moved its child");
    }

    plUsdPrim point;
    if (stage.ReadPrim("/World/Point", point).Failed() ||
        plMath::Abs(point.m_Light.GetUnrealLumensPerNit() - 4.0f * plMath::Pi<float>() * 0.0001f) > 1e-6f)
    {
      return Fail("zero-radius Unreal point light lost its clamped emitting area");
    }

    plUsdPrim spot;
    const float solidAngle = 2.0f * plMath::Pi<float>() * (1.0f - plMath::Cos(plAngle::MakeFromDegree(30)));
    if (stage.ReadPrim("/World/Spot", spot).Failed() ||
        plMath::Abs(spot.m_Light.GetUnrealLumensPerNit() - solidAngle * solidAngle * 0.25f) > 1e-5f)
    {
      return Fail("Unreal spotlight power must use its cone solid angle and source radius");
    }

    plLog::Success("Light regression passed: area emission, child transforms, lumens, point radius and spot cone");
    return 0;
  }

  /// Times each step a live link's first sync takes on the editor's main thread, then the mesh builds its worker tasks
  /// run, so a slow stage shows where the time goes. Builds at most uiMaxBuilds distinct meshes.
  int Profile(plStringView sFile, plUInt32 uiMaxBuilds, bool bTangents)
  {
    plTime start = plTime::Now();
    auto lap = [&](const plFormatString& what)
    {
      const plTime now = plTime::Now();
      plStringBuilder sTemp;
      plLog::Info("{}: {} ms", what.GetText(sTemp), plArgF((now - start).GetMilliseconds(), 1));
      start = now;
    };

    plUsdStage stage;
    plStatus res = stage.Open(sFile);
    if (res.Failed())
      return Fail(res.m_sMessage);
    lap("Open");

    plDynamicArray<plUsdPrim> prims;
    stage.ReadPrims(prims);
    lap(plFmt("ReadPrims ({} prims)", prims.GetCount()));

    stage.ReadPrims(prims);
    lap("ReadPrims again");

    for (const plUsdPrim& prim : prims)
    {
      plUsdPrimOpinions opinions;
      stage.QueryPrimOpinions(prim.m_sPath, opinions);
    }
    lap("QueryPrimOpinions, every prim");

    plMap<plUInt64, plUInt32> copies;
    plDynamicArray<plString> distinct;
    for (const plUsdPrim& prim : prims)
    {
      if (!prim.m_bActive || (prim.m_Kind != plUsdPrimKind::Mesh && prim.m_Kind != plUsdPrimKind::Gprim))
        continue;

      plUsdGeometryKey key;
      if (stage.ComputeGeometryKey(prim.m_sPath, key).Succeeded() && copies[key.m_uiIdentity]++ == 0)
        distinct.PushBack(prim.m_sPath);
    }
    lap(plFmt("ComputeGeometryKey, every mesh prim ({} distinct geometries)", distinct.GetCount()));

    plDynamicArray<plUniquePtr<plUsdGeometrySource>> sources;
    plSet<plUInt64> hashes;
    for (const plString& sPath : distinct)
    {
      if (plUniquePtr<plUsdGeometrySource> pSource = stage.CaptureGeometry(sPath, res))
        sources.PushBack(std::move(pSource));
    }
    lap("CaptureGeometry, one prim per distinct geometry");

    plUInt64 uiCapturedPoints = 0;
    for (const plUniquePtr<plUsdGeometrySource>& pSource : sources)
    {
      uiCapturedPoints += pSource->GetPointCount();
      hashes.Insert(pSource->GetHash());
    }
    lap(plFmt("GetHash of those ({} distinct contents)", hashes.GetCount()));
    plLog::Info("Captured {} points across {} geometries", uiCapturedPoints, sources.GetCount());

    plUInt64 uiVertices = 0;
    plUInt64 uiTriangles = 0;
    plTime buildTime;
    plTime tangentTime;
    const plUInt32 uiBuilds = plMath::Min(uiMaxBuilds, sources.GetCount());

    for (plUInt32 i = 0; i < uiBuilds; ++i)
    {
      plGeometry geom;
      plDynamicArray<plUInt32> groupTriangles;

      plTime t = plTime::Now();
      if (sources[i]->Build(geom, &groupTriangles).Failed())
        continue;
      buildTime += plTime::Now() - t;

      if (bTangents)
      {
        t = plTime::Now();
        geom.ComputeTangents();
        tangentTime += plTime::Now() - t;
      }

      uiVertices += geom.GetVertices().GetCount();
      uiTriangles += geom.GetPolygons().GetCount();

      // Meshlet LODs of the first part, as the live link splits and builds it: how far the hierarchy reduces LOD0.
      {
        constexpr plUInt32 uiMaxPartVertices = 250000;
        plGeometry part;
        plDynamicArray<plUInt32> remap;
        remap.SetCount(geom.GetVertices().GetCount(), plInvalidIndex);
        for (const plGeometry::Polygon& poly : geom.GetPolygons())
        {
          if (part.GetVertices().GetCount() + 3 > uiMaxPartVertices)
            break;

          plUInt32 triangle[3];
          for (plUInt32 c = 0; c < 3; ++c)
          {
            const plUInt32 v = poly.m_Vertices[c];
            if (remap[v] == plInvalidIndex)
            {
              remap[v] = part.GetVertices().GetCount();
              part.GetVertices().PushBack(geom.GetVertices()[v]);
            }
            triangle[c] = remap[v];
          }
          part.AddPolygon(plArrayPtr<plUInt32>(triangle), false);
        }

        plMeshResourceDescriptor desc;
        desc.SetMaterial(0, "Material");
        desc.MeshBufferDesc().AddCommonStreams();
        desc.MeshBufferDesc().AllocateStreamsFromGeometry(part, plGALPrimitiveTopology::Triangles);
        desc.AddSubMesh(part.GetPolygons().GetCount(), 0, 0);
        desc.ComputeBounds();

        t = plTime::Now();
        const plResult meshletResult = plModelImporter2::GenerateMeshletLods(desc);
        const plTime meshletTime = plTime::Now() - t;

        plUInt32 uiLod0 = 0;
        plUInt32 uiLod0Roots = 0;
        plUInt32 uiRoots = 0;
        plUInt32 uiMaxLevel = 0;
        const auto meshlets = desc.GetMeshlets();
        const auto links = desc.GetMeshletGroupLinks();
        for (const auto& meshlet : meshlets)
          uiMaxLevel = plMath::Max<plUInt32>(uiMaxLevel, meshlet.m_uiLodLevel);

        for (plUInt32 m = 0; m < meshlets.GetCount(); ++m)
        {
          // A whole-level chain has no links: its coarsest cut is its last level.
          const bool bRoot = links.IsEmpty() ? meshlets[m].m_uiLodLevel == uiMaxLevel : links[m].m_uiParentGroup == plInvalidIndex;
          uiRoots += bRoot ? 1 : 0;
          if (meshlets[m].m_uiLodLevel != 0)
            continue;
          ++uiLod0;
          uiLod0Roots += bRoot ? 1 : 0;
        }

        plLog::Info("  meshlets: {} is {}", i, i < distinct.GetCount() ? distinct[i].GetView() : plStringView());
        plLog::Info("  meshlets: {} part {} verts {} tris -> {} ({}): {} meshlets, LOD0 {} of which {} never coarsen, coarsest cut {}, {} levels, {} ms", i,
          part.GetVertices().GetCount(), part.GetPolygons().GetCount(), meshletResult.Succeeded() ? "ok" : "FAILED", desc.GetMeshletGroups().GetCount(),
          meshlets.GetCount(), uiLod0, uiLod0Roots, uiRoots, uiMaxLevel + 1, plArgF(meshletTime.GetMilliseconds(), 0));

        // The proxy the live link writes ahead of the mesh, round-tripped through the file format.
        plMeshResourceDescriptor proxy;
        float fProxyError = 0.0f;
        t = plTime::Now();
        if (meshletResult.Succeeded() && plModelImporter2::GenerateMeshletProxy(desc, 1.0f / 16.0f, proxy, fProxyError).Succeeded())
        {
          const plTime proxyTime = plTime::Now() - t;
          const plUInt32 uiProxyTriangles = proxy.GetSubMeshes()[0].m_uiPrimitiveCount;
          const plUInt64 uiProxyBytes = proxy.EstimateGpuBytes();

          plContiguousMemoryStreamStorage storage;
          plMemoryStreamWriter writer(&storage);
          desc.SaveWithProxy(writer, proxy, fProxyError);

          plMeshResourceDescriptor loadedProxy;
          plMeshResourceDescriptor loadedFull;
          plMeshResourceDescriptor::DetailInfo proxyInfo;
          plMeshResourceDescriptor::DetailInfo fullInfo;
          plMemoryStreamReader proxyReader(&storage);
          plMemoryStreamReader fullReader(&storage);
          const bool bProxyOk = loadedProxy.Load(proxyReader, plMeshResourceDescriptor::LoadDetail::ProxyIfAny, &proxyInfo).Succeeded() && proxyInfo.m_bLoadedProxy &&
                                loadedProxy.GetSubMeshes()[0].m_uiPrimitiveCount == uiProxyTriangles && loadedProxy.ValidateMeshletData().Succeeded();
          const bool bFullOk = loadedFull.Load(fullReader, plMeshResourceDescriptor::LoadDetail::Full, &fullInfo).Succeeded() && !fullInfo.m_bLoadedProxy &&
                               fullInfo.m_bHasProxy && loadedFull.GetSubMeshes()[0].m_uiPrimitiveCount == part.GetPolygons().GetCount() && loadedFull.ValidateMeshletData().Succeeded();

          plLog::Info("  proxy: {} tris ({}%), error {} (mesh radius {}), {} KB vs full {} KB, {} meshlets, {} ms, round trip proxy {} full {}", uiProxyTriangles,
            plArgF(100.0 * uiProxyTriangles / part.GetPolygons().GetCount(), 1), fProxyError, desc.GetBounds().m_fSphereRadius, uiProxyBytes / 1024,
            desc.EstimateGpuBytes() / 1024, proxy.GetMeshlets().GetCount(), plArgF(proxyTime.GetMilliseconds(), 0), bProxyOk ? "ok" : "FAILED", bFullOk ? "ok" : "FAILED");
        }
        else
        {
          plLog::Info("  proxy: none");
        }
      }
    }

    plLog::Info("Built {} distinct geometries: {} vertices, {} triangles; Build {} ms, ComputeTangents {} ms", uiBuilds, uiVertices, uiTriangles,
      plArgF(buildTime.GetMilliseconds(), 1), plArgF(tangentTime.GetMilliseconds(), 1));

    plUInt32 uiMostCopies = 0;
    for (auto it = copies.GetIterator(); it.IsValid(); ++it)
      uiMostCopies = plMath::Max(uiMostCopies, it.Value());
    plLog::Info("Most copies of one geometry: {}", uiMostCopies);
    return 0;
  }

  void PrintStage(const plUsdStage& stage, bool bGeometry)
  {
    const plUsdBasis& basis = stage.GetBasis();
    plLog::Info("Stage '{}': upAxis {}, metersPerUnit {}", stage.GetRootLayerFile(), basis.GetUpAxis() == plUsdBasis::UpAxis::Y ? "Y" : "Z",
      basis.GetMetersPerUnit());

    plDynamicArray<plUsdLayerInfo> layers;
    stage.GetLayerStack(layers);
    for (const plUsdLayerInfo& layer : layers)
    {
      plLog::Info("  layer {}{}", layer.m_sRealPath.IsEmpty() ? layer.m_sIdentifier : layer.m_sRealPath, layer.m_bIsEditTarget ? "  (edit target)" : "");
    }

    plDynamicArray<plUsdPrim> prims;
    stage.ReadPrims(prims);
    plLog::Info("{} imageable prims", prims.GetCount());

    for (const plUsdPrim& prim : prims)
    {
      plUInt32 uiDepth = 0;
      for (plInt32 i = prim.m_iParent; i >= 0; i = prims[i].m_iParent)
        ++uiDepth;

      plStringBuilder sIndent;
      for (plUInt32 i = 0; i < uiDepth; ++i)
        sIndent.Append("  ");

      const plVec3& p = prim.m_LocalTransform.m_vPosition;
      const plVec3& s = prim.m_LocalTransform.m_vScale;
      plStringBuilder sLine;
      sLine.SetFormat("{}{} [{} {}] pos ({}, {}, {}) scale ({}, {}, {})", sIndent, prim.m_sName, prim.m_sTypeName, KindName(prim.m_Kind),
        plArgF(p.x, 3), plArgF(p.y, 3), plArgF(p.z, 3), plArgF(s.x, 2), plArgF(s.y, 2), plArgF(s.z, 2));

      if (!prim.m_bVisible)
        sLine.Append(" invisible");
      if (prim.m_bTransformAnimated)
        sLine.Append(" animated");
      if (prim.m_bPrototype)
        sLine.Append(" prototype");

      plLog::Info("{}", sLine);

      {
        plDynamicArray<plUsdVariantSet> sets;
        stage.GetVariantSets(prim.m_sPath, sets);
        for (const plUsdVariantSet& set : sets)
        {
          plStringBuilder sSet;
          sSet.SetFormat("{}  variant set '{}':", sIndent, set.m_sName);
          for (const plString& sVariant : set.m_Variants)
            sSet.AppendFormat(" {}{}", sVariant, sVariant == set.m_sSelection ? "*" : "");
          plLog::Info("{}", sSet);
        }
      }

      for (const plUsdInstancedMesh& mesh : prim.m_Instancer.m_Meshes)
      {
        plStringBuilder sMesh;
        sMesh.SetFormat("{}  instances {} x {}", sIndent, mesh.m_Instances.GetCount(), mesh.m_sMeshPath);
        if (!mesh.m_Instances.IsEmpty())
        {
          const plTransform& t = mesh.m_Instances[0];
          sMesh.AppendFormat(", first at ({}, {}, {}) scale ({}, {}, {})", plArgF(t.m_vPosition.x, 3), plArgF(t.m_vPosition.y, 3), plArgF(t.m_vPosition.z, 3),
            plArgF(t.m_vScale.x, 3), plArgF(t.m_vScale.y, 3), plArgF(t.m_vScale.z, 3));
        }
        plLog::Info("{}", sMesh);
      }

      {
        const bool bArea = prim.m_Light.m_Shape == plUsdLightShape::Rect || prim.m_Light.m_Shape == plUsdLightShape::Disk;
        const plVec3 vLook = prim.m_LocalTransform.m_qRotation * plVec3(bArea ? -1.0f : 1.0f, 0, 0);

        plStringBuilder sView;

        if (prim.m_Kind == plUsdPrimKind::Light)
        {
          const plUsdLight& l = prim.m_Light;
          const char* szShapes[] = {"sphere", "distant", "rect", "disk", "cylinder", "other"};
          sView.SetFormat("{}  light {} luminance {}{} colour ({}, {}, {})", sIndent, szShapes[(int)l.m_Shape], plArgF(l.m_fLuminance, 3),
            l.m_bNormalize ? " normalized" : "", plArgF(l.m_Color.r, 2), plArgF(l.m_Color.g, 2), plArgF(l.m_Color.b, 2));
          sView.AppendFormat(" radius {} size {} x {} length {}", plArgF(l.m_fRadius, 3), plArgF(l.m_fWidth, 3), plArgF(l.m_fHeight, 3), plArgF(l.m_fLength, 3));
          sView.AppendFormat(" cone {} deg softness {} shadows {}", plArgF(l.m_ConeAngle.GetDegree(), 1), plArgF(l.m_fConeSoftness, 2), l.m_bCastShadows);
          if (l.m_bUnrealUnits && !l.m_bNormalize && l.m_Shape != plUsdLightShape::Distant && l.m_Shape != plUsdLightShape::Other && l.m_Shape != plUsdLightShape::Cylinder)
          {
            sView.AppendFormat(" Unreal power {} lm", plArgF(l.m_fLuminance * l.GetUnrealLumensPerNit(), 4));
          }
        }
        else if (prim.m_Kind == plUsdPrimKind::Camera)
        {
          const plUsdCamera& c = prim.m_Camera;
          sView.SetFormat("{}  camera {} focal {} mm aperture {} x {} mm", sIndent, c.m_bOrthographic ? "ortho" : "perspective", plArgF(c.m_fFocalLength, 2),
            plArgF(c.m_fHorizontalAperture, 2), plArgF(c.m_fVerticalAperture, 2));
          sView.AppendFormat(" clip {} - {} m fstop {} focus {} m exposure {}", plArgF(c.m_fNear, 3), plArgF(c.m_fFar, 1), plArgF(c.m_fFStop, 2),
            plArgF(c.m_fFocusDistance, 2), plArgF(c.m_fExposure, 2));
        }

        if (!sView.IsEmpty())
        {
          sView.AppendFormat(" looks ({}, {}, {})", plArgF(vLook.x, 2), plArgF(vLook.y, 2), plArgF(vLook.z, 2));
          plLog::Info("{}", sView);
        }
      }

      for (const plUsdMaterialGroup& group : prim.m_MaterialGroups)
      {
        if (!group.m_bHasSurface)
          continue;

        const plUsdSurface& surface = group.m_Surface;
        const plColor& c = surface.m_BaseColor;

        plStringBuilder sGroup;
        sGroup.SetFormat("{}  [{}] colour ({}, {}, {})", sIndent, group.m_sSubset.IsEmpty() ? plStringView("mesh") : group.m_sSubset.GetView(), plArgF(c.r, 2),
          plArgF(c.g, 2), plArgF(c.b, 2));
        if (!surface.m_sMaterialPath.IsEmpty())
          sGroup.AppendFormat(" material {} metallic {} roughness {}", surface.m_sMaterialPath, plArgF(surface.m_fMetallic, 2), plArgF(surface.m_fRoughness, 2));
        if (!surface.m_sBaseColorTexture.IsEmpty())
          sGroup.AppendFormat(" texture {}", surface.m_sBaseColorTexture.GetFileName());
        if (!surface.m_sNormalTexture.IsEmpty())
          sGroup.AppendFormat(" normal {}", surface.m_sNormalTexture.GetFileName());
        if (!surface.m_sRoughnessTexture.IsEmpty())
          sGroup.AppendFormat(" roughness {}", surface.m_sRoughnessTexture.GetFileName());
        if (!surface.m_sMetallicTexture.IsEmpty())
          sGroup.AppendFormat(" metallic {}", surface.m_sMetallicTexture.GetFileName());
        if (!surface.m_sOpacityTexture.IsEmpty())
          sGroup.AppendFormat(" mask {}", surface.m_sOpacityTexture.GetFileName());

        plLog::Info("{}", sGroup);
      }

      if (bGeometry && (prim.m_Kind == plUsdPrimKind::Mesh || prim.m_Kind == plUsdPrimKind::Gprim))
      {
        plGeometry geom;
        plDynamicArray<plUInt32> groupTriangles;
        plStatus res;
        plUniquePtr<plUsdGeometrySource> pSource = stage.CaptureGeometry(prim.m_sPath, res);
        if (pSource != nullptr)
          res = pSource->Build(geom, &groupTriangles);

        if (res.Failed())
        {
          plLog::Error("{}  geometry failed: {}", sIndent, res.m_sMessage);
          continue;
        }

        plStringBuilder sGroups;
        for (plUInt32 uiCount : groupTriangles)
          sGroups.AppendFormat("{}{}", sGroups.IsEmpty() ? "" : "/", uiCount);

        plBoundingBox bounds = plBoundingBox::MakeInvalid();
        plVec2 vUvMin(plMath::MaxValue<float>()), vUvMax(-plMath::MaxValue<float>());
        for (const auto& v : geom.GetVertices())
        {
          bounds.ExpandToInclude(v.m_vPosition);
          vUvMin = vUvMin.CompMin(v.m_vTexCoord);
          vUvMax = vUvMax.CompMax(v.m_vTexCoord);
        }

        plLog::Info("{}  {} vertices, {} triangles (per group {}), bounds ({}, {}, {}) - ({}, {}, {})", sIndent, geom.GetVertices().GetCount(), geom.GetPolygons().GetCount(), sGroups,
          plArgF(bounds.m_vMin.x, 3), plArgF(bounds.m_vMin.y, 3), plArgF(bounds.m_vMin.z, 3), plArgF(bounds.m_vMax.x, 3), plArgF(bounds.m_vMax.y, 3),
          plArgF(bounds.m_vMax.z, 3));
        plLog::Info("{}  uv ({}, {}) - ({}, {})", sIndent, plArgF(vUvMin.x, 2), plArgF(vUvMin.y, 2), plArgF(vUvMax.x, 2), plArgF(vUvMax.y, 2));
      }
    }
  }

  int RoundTrip(plStringView sAsset, plStringView sPrimPath)
  {
    plString sSessionRoot, sParticipantLayer;
    plStatus res = plUsdStage::CreateSession(sAsset, "inspect-roundtrip", "plasma-inspect", sSessionRoot, sParticipantLayer);
    if (res.Failed())
      return Fail(res.m_sMessage);

    plTransform target = plTransform::MakeIdentity();
    target.m_vPosition.Set(1.5f, -2.25f, 0.75f);
    target.m_qRotation = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(40.0f));
    target.m_vScale.Set(1.0f, 1.0f, 2.0f);

    {
      plUsdStage stage;
      if (res = stage.Open(sSessionRoot); res.Failed())
        return Fail(res.m_sMessage);

      if (res = stage.SetEditLayer(sParticipantLayer); res.Failed())
        return Fail(res.m_sMessage);

      if (res = stage.AuthorLocalTransform(sPrimPath, target); res.Failed())
        return Fail(res.m_sMessage);

      if (res = stage.AuthorVisibility(sPrimPath, false); res.Failed())
        return Fail(res.m_sMessage);

      if (res = stage.SaveEditLayer(); res.Failed())
        return Fail(res.m_sMessage);

      plDynamicArray<plString> layers;
      stage.GetAttributeLayerStack(sPrimPath, "xformOpOrder", layers);
      for (const plString& s : layers)
        plLog::Info("xformOpOrder authored in {}", s);
    }

    // Reopen from disk, as another tool would.
    plUsdStage reopened;
    if (res = reopened.Open(sSessionRoot); res.Failed())
      return Fail(res.m_sMessage);

    plUsdPrim prim;
    if (reopened.ReadPrim(sPrimPath, prim).Failed())
      return FailFmt(plFmt("'{}' is missing after the round trip", sPrimPath));

    if (!prim.m_LocalTransform.IsEqual(target, 1e-4f))
    {
      const plVec3& p = prim.m_LocalTransform.m_vPosition;
      return FailFmt(plFmt("transform did not round-trip: position ({}, {}, {})", p.x, p.y, p.z));
    }

    if (prim.m_bVisible)
      return Fail("visibility did not round-trip");

    plLog::Success("Round trip OK through '{}'", sParticipantLayer);

    // Revert, and check the asset's own value comes back.
    {
      plUsdStage stage;
      if (res = stage.Open(sSessionRoot); res.Failed())
        return Fail(res.m_sMessage);
      if (res = stage.SetEditLayer(sParticipantLayer); res.Failed())
        return Fail(res.m_sMessage);
      if (res = stage.ClearEditLayerOpinions(sPrimPath, ""); res.Failed())
        return Fail(res.m_sMessage);
      if (stage.HasEditLayerOpinion(sPrimPath, ""))
        return Fail("opinions remained after clearing");
      if (res = stage.SaveEditLayer(); res.Failed())
        return Fail(res.m_sMessage);
    }

    plUsdStage original;
    if (res = original.Open(sAsset); res.Failed())
      return Fail(res.m_sMessage);

    plUsdPrim originalPrim;
    if (original.ReadPrim(sPrimPath, originalPrim).Failed())
      return Fail("prim missing in the asset");

    plUsdStage cleared;
    if (res = cleared.Open(sSessionRoot); res.Failed())
      return Fail(res.m_sMessage);

    plUsdPrim clearedPrim;
    if (cleared.ReadPrim(sPrimPath, clearedPrim).Failed() || !clearedPrim.m_LocalTransform.IsEqual(originalPrim.m_LocalTransform, 1e-4f) ||
        clearedPrim.m_bVisible != originalPrim.m_bVisible)
      return Fail("clearing Plasma's opinions did not restore the asset's values");

    plLog::Success("Revert OK");
    return 0;
  }

  /// Writes new values for the stage's first light, first camera and first material through a session, checks they read
  /// back unchanged from disk, then clears them and checks the asset's values return.
  int ValuesRoundTrip(plStringView sAsset)
  {
    plString sSessionRoot, sParticipantLayer;
    plStatus res = plUsdStage::CreateSession(sAsset, "inspect-values", "plasma-inspect", sSessionRoot, sParticipantLayer);
    if (res.Failed())
      return Fail(res.m_sMessage);

    plUsdPrim light, camera;
    plUsdMaterialGroup material;
    {
      plUsdStage stage;
      if (res = stage.Open(sAsset); res.Failed())
        return Fail(res.m_sMessage);

      plDynamicArray<plUsdPrim> prims;
      stage.ReadPrims(prims);
      for (const plUsdPrim& prim : prims)
      {
        if (prim.m_Kind == plUsdPrimKind::Light && light.m_sPath.IsEmpty() && prim.m_Light.m_Shape == plUsdLightShape::Sphere)
          light = prim;
        if (prim.m_Kind == plUsdPrimKind::Camera && camera.m_sPath.IsEmpty())
          camera = prim;
        for (const plUsdMaterialGroup& group : prim.m_MaterialGroups)
        {
          if (material.m_Surface.m_sMaterialPath.IsEmpty() && !group.m_Surface.m_sMaterialPath.IsEmpty())
            material = group;
        }
      }
    }

    plUsdLight newLight = light.m_Light;
    newLight.m_fLuminance *= 2.0f;
    newLight.m_Color = plColor(0.25f, 0.5f, 1.0f);
    newLight.m_ConeAngle = plAngle::MakeFromDegree(50.0f);
    newLight.m_fConeSoftness = 0.25f;
    newLight.m_bCastShadows = false;

    plUsdCamera newCamera = camera.m_Camera;
    newCamera.m_fFocalLength *= 1.5f;
    newCamera.m_fFStop = 4.0f;
    newCamera.m_fNear = 0.5f;
    newCamera.m_fExposure = -1.0f;

    const plColor newColor(0.8f, 0.1f, 0.2f);

    {
      plUsdStage stage;
      if ((res = stage.Open(sSessionRoot)).Failed() || (res = stage.SetEditLayer(sParticipantLayer)).Failed())
        return Fail(res.m_sMessage);

      if (!light.m_sPath.IsEmpty() && (res = stage.AuthorLight(light.m_sPath, newLight)).Failed())
        return Fail(res.m_sMessage);
      if (!camera.m_sPath.IsEmpty() && (res = stage.AuthorCamera(camera.m_sPath, newCamera)).Failed())
        return Fail(res.m_sMessage);

      const plString& sMaterial = material.m_Surface.m_sMaterialPath;
      if (!sMaterial.IsEmpty())
      {
        if (material.m_Surface.m_sBaseColorTexture.IsEmpty() && (res = stage.AuthorSurfaceValue(sMaterial, plUsdStage::SurfaceValue::BaseColor, newColor)).Failed())
          return Fail(res.m_sMessage);
        if (material.m_Surface.m_sRoughnessTexture.IsEmpty() && (res = stage.AuthorSurfaceValue(sMaterial, plUsdStage::SurfaceValue::Roughness, 0.125f)).Failed())
          return Fail(res.m_sMessage);
      }

      if ((res = stage.SaveEditLayer()).Failed())
        return Fail(res.m_sMessage);
    }

    auto near = [](float a, float b)
    { return plMath::Abs(a - b) <= 1e-4f * plMath::Max(1.0f, plMath::Abs(b)); };

    {
      plUsdStage reopened;
      if ((res = reopened.Open(sSessionRoot)).Failed())
        return Fail(res.m_sMessage);

      plUsdPrim prim;
      if (!light.m_sPath.IsEmpty())
      {
        if (reopened.ReadPrim(light.m_sPath, prim).Failed())
          return Fail("the light is missing after writing");
        const plUsdLight& l = prim.m_Light;
        if (!near(l.m_fLuminance, newLight.m_fLuminance) || !l.m_Color.IsEqualRGB(newLight.m_Color, 1e-4f) || !near(l.m_ConeAngle.GetDegree(), 50.0f) ||
            !near(l.m_fConeSoftness, 0.25f) || l.m_bCastShadows)
          return FailFmt(plFmt("light values did not round-trip: luminance {} cone {} softness {}", l.m_fLuminance, l.m_ConeAngle.GetDegree(), l.m_fConeSoftness));
        plLog::Success("Light values round-trip through '{}'", sParticipantLayer);
      }

      if (!camera.m_sPath.IsEmpty())
      {
        if (reopened.ReadPrim(camera.m_sPath, prim).Failed())
          return Fail("the camera is missing after writing");
        const plUsdCamera& c = prim.m_Camera;
        if (!near(c.m_fFocalLength, newCamera.m_fFocalLength) || !near(c.m_fFStop, 4.0f) || !near(c.m_fNear, 0.5f) || !near(c.m_fExposure, -1.0f) ||
            !near(c.m_fFar, camera.m_Camera.m_fFar))
          return FailFmt(plFmt("camera values did not round-trip: focal {} fstop {} near {}", c.m_fFocalLength, c.m_fFStop, c.m_fNear));
        plLog::Success("Camera values round-trip");
      }

      if (!material.m_Surface.m_sMaterialPath.IsEmpty())
      {
        plDynamicArray<plUsdPrim> prims;
        reopened.ReadPrims(prims);
        for (const plUsdPrim& p : prims)
        {
          for (const plUsdMaterialGroup& group : p.m_MaterialGroups)
          {
            if (group.m_Surface.m_sMaterialPath != material.m_Surface.m_sMaterialPath)
              continue;
            if ((material.m_Surface.m_sBaseColorTexture.IsEmpty() && !group.m_Surface.m_BaseColor.IsEqualRGB(newColor, 1e-4f)) ||
                (material.m_Surface.m_sRoughnessTexture.IsEmpty() && !near(group.m_Surface.m_fRoughness, 0.125f)))
              return Fail("material values did not round-trip");
          }
        }
        plLog::Success("Material values round-trip on '{}'", material.m_Surface.m_sMaterialPath);
      }
    }

    // Clear everything this test wrote, and check the asset's values are back.
    {
      plUsdStage stage;
      if ((res = stage.Open(sSessionRoot)).Failed() || (res = stage.SetEditLayer(sParticipantLayer)).Failed())
        return Fail(res.m_sMessage);

      if (!material.m_Surface.m_sMaterialPath.IsEmpty())
      {
        if (!stage.HasSurfaceOpinion(material.m_Surface.m_sMaterialPath))
          return Fail("the edit layer should hold the material values");
        stage.ClearSurfaceValues(material.m_Surface.m_sMaterialPath).IgnoreResult();
      }
      if (!light.m_sPath.IsEmpty())
        stage.ClearEditLayerOpinions(light.m_sPath, "values").IgnoreResult();
      if (!camera.m_sPath.IsEmpty())
        stage.ClearEditLayerOpinions(camera.m_sPath, "values").IgnoreResult();

      if ((res = stage.SaveEditLayer()).Failed())
        return Fail(res.m_sMessage);

      plUsdPrim prim;
      if (!light.m_sPath.IsEmpty() && (stage.ReadPrim(light.m_sPath, prim).Failed() || !(prim.m_Light == light.m_Light)))
        return Fail("clearing did not restore the light");
      if (!camera.m_sPath.IsEmpty() && (stage.ReadPrim(camera.m_sPath, prim).Failed() || !(prim.m_Camera == camera.m_Camera)))
        return Fail("clearing did not restore the camera");
      if (!material.m_Surface.m_sMaterialPath.IsEmpty() && stage.HasSurfaceOpinion(material.m_Surface.m_sMaterialPath))
        return Fail("clearing left material opinions");
    }

    plLog::Success("Clearing restores the asset's values");
    return 0;
  }

  /// Switches the first variant set of the stage to its other variants in a session layer, checks the prims below
  /// follow and the selection shows as an edit opinion, then reverts and checks the asset's selection is back.
  int VariantTest(plStringView sAsset)
  {
    plString sSessionRoot, sParticipantLayer;
    plStatus res = plUsdStage::CreateSession(sAsset, "inspect-variants", "plasma-inspect", sSessionRoot, sParticipantLayer);
    if (res.Failed())
      return Fail(res.m_sMessage);

    plUsdStage stage;
    if ((res = stage.Open(sSessionRoot)).Failed() || (res = stage.SetEditLayer(sParticipantLayer)).Failed())
      return Fail(res.m_sMessage);

    plDynamicArray<plUsdPrim> prims;
    stage.ReadPrims(prims);

    plString sPrimPath;
    plUsdVariantSet set;
    for (const plUsdPrim& prim : prims)
    {
      plDynamicArray<plUsdVariantSet> sets;
      stage.GetVariantSets(prim.m_sPath, sets);
      if (!sets.IsEmpty() && sets[0].m_Variants.GetCount() > 1)
      {
        sPrimPath = prim.m_sPath;
        set = sets[0];
        break;
      }
    }

    if (sPrimPath.IsEmpty())
      return Fail("the stage has no variant set with two or more variants");

    auto countBelow = [&](plUsdStage& s)
    {
      plDynamicArray<plUsdPrim> current;
      s.ReadPrims(current);
      plStringBuilder sPrefix(sPrimPath, "/");
      plStringBuilder sNames;
      for (const plUsdPrim& p : current)
      {
        if (p.m_sPath.StartsWith(sPrefix))
          sNames.Append(p.m_sName, " ");
      }
      return plString(sNames);
    };

    const plString sAssetSelection = set.m_sSelection;
    const plString sAssetPrims = countBelow(stage);
    plLog::Info("'{}' set '{}' selects '{}': {}", sPrimPath, set.m_sName, sAssetSelection, sAssetPrims);

    for (const plString& sVariant : set.m_Variants)
    {
      if (sVariant == sAssetSelection)
        continue;

      if ((res = stage.AuthorVariantSelection(sPrimPath, set.m_sName, sVariant)).Failed())
        return Fail(res.m_sMessage);

      plDynamicArray<plUsdVariantSet> sets;
      stage.GetVariantSets(sPrimPath, sets);
      plUsdPrimOpinions opinions;
      stage.QueryPrimOpinions(sPrimPath, opinions);
      if (sets.IsEmpty() || sets[0].m_sSelection != sVariant || !opinions.m_bEditVariants)
        return FailFmt(plFmt("selecting '{}' did not take", sVariant));

      plLog::Info("  '{}': {}", sVariant, countBelow(stage));
    }

    if ((res = stage.AuthorVariantSelection(sPrimPath, set.m_sName, "no-such-variant")).Succeeded())
      return Fail("an unknown variant should be refused");

    if ((res = stage.SaveEditLayer()).Failed())
      return Fail(res.m_sMessage);

    if (!stage.HasEditLayerOpinion(sPrimPath, "variants"))
      return Fail("the edit layer should hold a variant selection");

    stage.ClearEditLayerOpinions(sPrimPath, "variants").IgnoreResult();
    if ((res = stage.SaveEditLayer()).Failed())
      return Fail(res.m_sMessage);

    plDynamicArray<plUsdVariantSet> sets;
    stage.GetVariantSets(sPrimPath, sets);
    if (stage.HasEditLayerOpinion(sPrimPath, "variants") || sets.IsEmpty() || sets[0].m_sSelection != sAssetSelection || countBelow(stage) != sAssetPrims)
      return Fail("reverting did not restore the asset's selection");

    plLog::Success("Variant selections switch, show as edit opinions and revert");
    return 0;
  }

  /// Carries two Plasma components on the first mesh through a participant layer: writes them, reads them back after a
  /// reopen, removes one, commits, and checks the asset holds exactly the remaining one. Writes the asset: run it on a
  /// copy.
  int ComponentTest(plStringView sAsset)
  {
    if (!plUsdRuntime::IsApiSchemaRegistered("PlasmaComponentAPI"))
      return Fail("OpenUSD does not know the PlasmaComponentAPI schema");
    plLog::Success("PlasmaComponentAPI is registered");

    plString sSessionRoot, sParticipantLayer;
    plStatus res = plUsdStage::CreateSession(sAsset, "inspect-components", "plasma-inspect", sSessionRoot, sParticipantLayer);
    if (res.Failed())
      return Fail(res.m_sMessage);

    plString sPrimPath;
    {
      plUsdStage stage;
      if ((res = stage.Open(sSessionRoot)).Failed())
        return Fail(res.m_sMessage);

      plDynamicArray<plUsdPrim> prims;
      stage.ReadPrims(prims);
      for (const plUsdPrim& prim : prims)
      {
        if (prim.m_Kind == plUsdPrimKind::Mesh || prim.m_Kind == plUsdPrimKind::Gprim)
        {
          sPrimPath = prim.m_sPath;
          break;
        }
      }
    }

    if (sPrimPath.IsEmpty())
      return Fail("the stage has no mesh");

    plHybridArray<plUsdComponent, 2> components;
    {
      plUsdComponent& a = components.ExpandAndGetRef();
      a.m_sType = "plRotorComponent";
      a.m_Values.PushBack({"Axis", plString("PosZ")});
      a.m_Values.PushBack({"AxisDeviation", 12.5f});
      a.m_Values.PushBack({"DegreesToRotate", (plInt32)90});
      a.m_Values.PushBack({"Running", false});

      plUsdComponent& b = components.ExpandAndGetRef();
      b.m_sType = "plPointLightComponent";
      b.m_Values.PushBack({"LightColor", plColor(1.0f, 0.5f, 0.25f, 1.0f)});
      b.m_Values.PushBack({"Offset", plVec3(0.0f, 0.0f, 2.0f)});
      b.m_Values.PushBack({"Rotation", plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(30.0f))});
      b.m_Values.PushBack({"Tag", plString("lamp")});
    }

    // plUsdStage reads values in name order; compares regardless of order.
    auto same = [](const plUsdComponent& lhs, const plUsdComponent& rhs)
    {
      if (lhs.m_sType != rhs.m_sType || lhs.m_Values.GetCount() != rhs.m_Values.GetCount())
        return false;

      for (const plUsdComponentValue& value : lhs.m_Values)
      {
        bool bFound = false;
        for (const plUsdComponentValue& other : rhs.m_Values)
        {
          if (other.m_sProperty != value.m_sProperty)
            continue;

          bFound = value.m_Value.IsA<plQuat>() ? other.m_Value.IsA<plQuat>() && other.m_Value.Get<plQuat>().IsEqualRotation(value.m_Value.Get<plQuat>(), 1e-5f)
                                               : other.m_Value == value.m_Value;
        }

        if (!bFound)
        {
          plLog::Error("'{}' of '{}' did not round-trip", value.m_sProperty, lhs.m_sType);
          return false;
        }
      }
      return true;
    };

    {
      plUsdStage stage;
      if ((res = stage.Open(sSessionRoot)).Failed() || (res = stage.SetEditLayer(sParticipantLayer)).Failed() ||
          (res = stage.AuthorComponents(sPrimPath, components)).Failed() || (res = stage.SaveEditLayer()).Failed())
        return Fail(res.m_sMessage);
    }

    {
      plUsdStage stage;
      if ((res = stage.Open(sSessionRoot)).Failed() || (res = stage.SetEditLayer(sParticipantLayer)).Failed())
        return Fail(res.m_sMessage);

      plUsdPrim prim;
      if (stage.ReadPrim(sPrimPath, prim).Failed() || prim.m_Components.GetCount() != 2 || !same(components[0], prim.m_Components[0]) ||
          !same(components[1], prim.m_Components[1]))
        return FailFmt(plFmt("the components did not round-trip: {} read back", prim.m_Components.GetCount()));

      plUsdPrimOpinions opinions;
      stage.QueryPrimOpinions(sPrimPath, opinions);
      if (!opinions.m_bEditComponents || opinions.m_bEditValues)
        return Fail("the components should count as component opinions, and only as those");

      plLog::Success("Components round-trip on '{}'", sPrimPath);

      // Unchanged values write nothing; a removed type goes with its values.
      components[0].m_Values[1].m_Value = 45.0f;
      components.PopBack();
      if ((res = stage.AuthorComponents(sPrimPath, components)).Failed() || (res = stage.SaveEditLayer()).Failed())
        return Fail(res.m_sMessage);

      if (stage.ReadPrim(sPrimPath, prim).Failed() || prim.m_Components.GetCount() != 1 || !same(components[0], prim.m_Components[0]))
        return Fail("removing a component or changing a value did not take");
    }

    plUInt32 uiOpinions = 0;
    if ((res = plUsdStage::CommitSession(sSessionRoot, uiOpinions)).Failed())
      return Fail(res.m_sMessage);

    {
      plUsdStage asset;
      if ((res = asset.Open(sAsset)).Failed())
        return Fail(res.m_sMessage);

      plUsdPrim prim;
      if (asset.ReadPrim(sPrimPath, prim).Failed() || prim.m_Components.GetCount() != 1 || !same(components[0], prim.m_Components[0]))
        return Fail("the asset does not hold exactly the remaining component after the commit");
    }

    plLog::Success("Commit carries the components into the asset");

    // Removing the committed component in a new session and committing again must take it out of the asset.
    {
      plUsdStage stage;
      if ((res = stage.Open(sSessionRoot)).Failed() || (res = stage.SetEditLayer(sParticipantLayer)).Failed() ||
          (res = stage.AuthorComponents(sPrimPath, {})).Failed() || (res = stage.SaveEditLayer()).Failed())
        return Fail(res.m_sMessage);
    }

    if ((res = plUsdStage::CommitSession(sSessionRoot, uiOpinions)).Failed())
      return Fail(res.m_sMessage);

    {
      plUsdStage asset;
      if ((res = asset.Open(sAsset)).Failed())
        return Fail(res.m_sMessage);

      plUsdPrim prim;
      if (asset.ReadPrim(sPrimPath, prim).Failed() || !prim.m_Components.IsEmpty())
        return Fail("the asset still holds a component removed in a committed session");
    }

    plLog::Success("Commit carries a removal into the asset");
    return 0;
  }

  /// Commits a session into the asset. Writes the asset: run it on a copy. Two participants move the same prim, the
  /// stronger one deactivates another and changes the first light; after the commit the asset alone must show the
  /// stronger participant's values and both layers must be empty.
  int CommitTest(plStringView sAsset)
  {
    // Joining through CreateSession puts the new participant strongest, so the stronger one joins second.
    plString sSessionRoot, sStrong, sWeak;
    plStatus res = plUsdStage::CreateSession(sAsset, "inspect-commit", "blender-inspect", sSessionRoot, sWeak);
    if (res.Succeeded())
      res = plUsdStage::CreateSession(sAsset, "inspect-commit", "plasma-inspect", sSessionRoot, sStrong);
    if (res.Failed())
      return Fail(res.m_sMessage);

    if (plUsdStage::GetSessionOwnerLayer(sSessionRoot) != plStringView(sStrong))
      return FailFmt(plFmt("the owner should be '{}', not '{}'", sStrong, plUsdStage::GetSessionOwnerLayer(sSessionRoot)));

    plDynamicArray<plUsdPrim> prims;
    {
      plUsdStage stage;
      if ((res = stage.Open(sAsset)).Failed())
        return Fail(res.m_sMessage);
      stage.ReadPrims(prims);
    }

    const plUsdPrim* pMoved = nullptr;
    const plUsdPrim* pHidden = nullptr;
    const plUsdPrim* pLight = nullptr;
    for (const plUsdPrim& prim : prims)
    {
      if (prim.m_iParent >= 0 && prim.m_Kind == plUsdPrimKind::Mesh && pMoved == nullptr)
        pMoved = &prim;
      else if (prim.m_iParent >= 0 && prim.m_Kind == plUsdPrimKind::Camera && pHidden == nullptr)
        pHidden = &prim;
      else if (prim.m_Kind == plUsdPrimKind::Light && pLight == nullptr)
        pLight = &prim;
    }
    if (pMoved == nullptr || pHidden == nullptr || pLight == nullptr)
      return Fail("the test needs a mesh, a camera and a light below the root");

    plTransform weakMove = pMoved->m_LocalTransform, strongMove = pMoved->m_LocalTransform;
    weakMove.m_vPosition += plVec3(1, 0, 0);
    strongMove.m_vPosition += plVec3(0, 2, 0);

    plUsdLight light = pLight->m_Light;
    light.m_fLuminance *= 3.0f;

    for (const plString* pLayer : {&sWeak, &sStrong})
    {
      plUsdStage stage;
      if ((res = stage.Open(sSessionRoot)).Failed() || (res = stage.SetEditLayer(*pLayer)).Failed())
        return Fail(res.m_sMessage);

      const bool bStrong = pLayer == &sStrong;
      if ((res = stage.AuthorLocalTransform(pMoved->m_sPath, bStrong ? strongMove : weakMove)).Failed())
        return Fail(res.m_sMessage);
      if (bStrong && ((res = stage.AuthorActive(pHidden->m_sPath, false)).Failed() || (res = stage.AuthorLight(pLight->m_sPath, light)).Failed()))
        return Fail(res.m_sMessage);
      if ((res = stage.SaveEditLayer()).Failed())
        return Fail(res.m_sMessage);
    }

    plUInt32 uiOpinions = 0;
    if ((res = plUsdStage::CommitSession(sSessionRoot, uiOpinions)).Failed())
      return Fail(res.m_sMessage);
    plLog::Info("Committed {} opinions", uiOpinions);

    plUsdStage asset;
    if ((res = asset.Open(sAsset)).Failed())
      return Fail(res.m_sMessage);

    plUsdPrim prim;
    if (asset.ReadPrim(pMoved->m_sPath, prim).Failed() || !prim.m_LocalTransform.IsEqual(strongMove, 1e-4f))
      return Fail("the asset does not hold the stronger participant's move");
    if (asset.ReadPrim(pHidden->m_sPath, prim).Failed() || prim.m_bActive)
      return Fail("the asset does not hold the deactivation");
    if (asset.ReadPrim(pLight->m_sPath, prim).Failed() || plMath::Abs(prim.m_Light.m_fLuminance - light.m_fLuminance) > 1e-3f * light.m_fLuminance)
      return Fail("the asset does not hold the light change");

    for (const plString* pLayer : {&sWeak, &sStrong})
    {
      plUsdStage stage;
      if ((res = stage.Open(sSessionRoot)).Failed() || (res = stage.SetEditLayer(*pLayer)).Failed())
        return Fail(res.m_sMessage);
      if (stage.CountLayerOpinions(*pLayer) != 0)
        return FailFmt(plFmt("'{}' is not empty after the commit", *pLayer));
    }

    plLog::Success("Commit folds the session into the asset and empties the participant layers");
    return 0;
  }

  int InspectWithMaterialX(plStringView sFile, plStringView sMaterialXPlugin)
  {
    // The editor loads it as a plugin. Only its exported MaterialX reader is used here, so it is loaded as a plain library
    // and its plugin hooks do not run.
    if (LoadLibraryExW(plStringWChar(sMaterialXPlugin), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr)
      return FailFmt(plFmt("could not load '{}' (error {})", sMaterialXPlugin, (plUInt32)GetLastError()));

    if (plPathUtils::HasExtension(sFile, "mtlx"))
    {
      plStringBuilder sUsda;
      if (const plStatus res = plUsdMtlx::ReadAsUsda(sFile, sUsda); res.Failed())
        return Fail(res.m_sMessage);

      plLog::Info("'{}' reads as:\n{}", sFile, sUsda);
    }

    plUsdStage stage;
    if (const plStatus res = stage.Open(sFile); res.Failed())
      return Fail(res.m_sMessage);

    PrintStage(stage, false);

    // A .mtlx layer is read-only: an opinion authored into it stays in memory, and saving it fails and leaves the file alone.
    plDynamicArray<plUsdPrim> prims;
    stage.ReadPrims(prims);

    plDynamicArray<plUsdLayerInfo> layers;
    if (!prims.IsEmpty())
      stage.GetLayerStack(layers);

    for (const plUsdLayerInfo& layer : layers)
    {
      if (!plPathUtils::HasExtension(layer.m_sRealPath, "mtlx"))
        continue;

      plFileStats before, after;
      if (plOSFile::GetFileStats(layer.m_sRealPath, before).Failed())
        return FailFmt(plFmt("'{}' has no file", layer.m_sRealPath));

      plStatus res = stage.SetEditLayer(layer.m_sRealPath);
      if (res.Succeeded())
        res = stage.AuthorLocalTransform(prims[0].m_sPath, plTransform::Make(plVec3(1.0f, 2.0f, 3.0f)));
      if (res.Failed())
        return FailFmt(plFmt("could not author into '{}': {}", layer.m_sRealPath, res.m_sMessage));

      res = stage.SaveEditLayer();
      if (res.Succeeded())
        return FailFmt(plFmt("'{}' was saved", layer.m_sRealPath));

      if (plOSFile::GetFileStats(layer.m_sRealPath, after).Failed() || after.m_uiFileSize != before.m_uiFileSize ||
          !after.m_LastModificationTime.Compare(before.m_LastModificationTime, plTimestamp::CompareMode::Identical))
        return FailFmt(plFmt("'{}' changed on disk", layer.m_sRealPath));

      plLog::Success("'{}' refuses to be saved: {}", layer.m_sRealPath, res.m_sMessage);
    }

    return 0;
  }

  /// Private bytes now and at peak, in GB.
  void GetMemory(double& out_fCurrent, double& out_fPeak)
  {
    PROCESS_MEMORY_COUNTERS counters = {};
    counters.cb = sizeof(counters);
    K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    out_fCurrent = counters.PagefileUsage / (1024.0 * 1024.0 * 1024.0);
    out_fPeak = counters.PeakPagefileUsage / (1024.0 * 1024.0 * 1024.0);
  }

  plString MemoryText()
  {
    double fCurrent = 0, fPeak = 0;
    GetMemory(fCurrent, fPeak);
    plStringBuilder s;
    s.SetFormat("mem {} GB, peak {} GB", plArgF(fCurrent, 2), plArgF(fPeak, 2));
    return s;
  }

  struct PartitionPlanSettings
  {
    float m_fCellSize = 128.0f;
    float m_fBatchSize = 8.0f; ///< 0: every placeable is its own object.
    float m_fSplitSize = 0.0f; ///< 0: the cell size.
    plUInt32 m_uiMaxSplitCells = 65536;
  };

  plUInt64 CellKey(const plVec2I32& cell)
  {
    return plUsdPartitionScan::PackCellKey(cell);
  }

  float ExtentXY(const plBoundingBox& bounds)
  {
    return bounds.IsValid() ? plMath::Max(bounds.m_vMax.x - bounds.m_vMin.x, bounds.m_vMax.y - bounds.m_vMin.y) : 0.0f;
  }

  /// Places a range of placeables into cells as the import plans to, and prints what that gives.
  void PrintPlan(plStringView sLabel, const plUsdScanResult& result, plUInt32 uiFirst, plUInt32 uiCount, const PartitionPlanSettings& settings)
  {
    plUInt32 uiInstances = 0, uiPointInstances = 0, uiGprims = 0, uiLights = 0, uiCameras = 0, uiNoBounds = 0;
    plUInt32 uiSplit = 0, uiSplitPieces = 0, uiOversized = 0, uiPivotElsewhere = 0, uiBatchedPlaceables = 0;
    plUInt32 uiSingles = 0, uiInstanceBatches = 0, uiGprimBatches = 0;
    plHashTable<plUInt64, plUInt32> objectsPerCell;
    plHashSet<plUInt64> batches;
    plHashSet<plUInt64> geometryKeys;
    plHashSet<plUInt32> prototypes;
    plBoundingBox extent = plBoundingBox::MakeInvalid();

    for (plUInt32 i = uiFirst; i < uiFirst + uiCount; ++i)
    {
      const plUsdPlaceable& p = result.m_Placeables[i];

      if (p.m_Kind == plUsdPlaceableKind::Light || p.m_Kind == plUsdPlaceableKind::Camera)
      {
        (p.m_Kind == plUsdPlaceableKind::Light ? uiLights : uiCameras)++;
        continue;
      }

      if (!p.m_WorldBounds.IsValid())
        ++uiNoBounds;
      else
        extent.ExpandToInclude(p.m_WorldBounds);

      const plVec3 vCentre = p.m_WorldBounds.IsValid() ? p.m_WorldBounds.GetCenter() : p.m_WorldTransform.m_vPosition;
      const plVec2I32 cell = plUsdPartitionScan::CellOf(vCentre, settings.m_fCellSize);
      const plUInt64 uiCell = CellKey(cell);
      const float fExtent = ExtentXY(p.m_WorldBounds);

      if (plUsdPartitionScan::CellOf(p.m_WorldTransform.m_vPosition, settings.m_fCellSize) != cell)
        ++uiPivotElsewhere;

      if (p.m_Kind == plUsdPlaceableKind::Gprim)
      {
        ++uiGprims;
        if (p.m_uiGeometryKey != 0)
          geometryKeys.Insert(p.m_uiGeometryKey);

        if (fExtent > settings.m_fSplitSize)
        {
          // One piece per touched cell at most; the real split below counts the cells its triangles land in.
          ++uiSplit;
          const plVec2I32 c0 = plUsdPartitionScan::CellOf(p.m_WorldBounds.m_vMin, settings.m_fCellSize);
          const plVec2I32 c1 = plUsdPartitionScan::CellOf(p.m_WorldBounds.m_vMax, settings.m_fCellSize);
          for (plInt32 x = c0.x; x <= c1.x; ++x)
          {
            for (plInt32 y = c0.y; y <= c1.y; ++y)
            {
              objectsPerCell[CellKey(plVec2I32(x, y))]++;
              ++uiSplitPieces;
            }
          }
          continue;
        }

        if (settings.m_fBatchSize > 0.0f && fExtent < settings.m_fBatchSize)
        {
          // Merged per cell and parent prim.
          const plStringView sPath = result.GetPath(p);
          const char* szSlash = sPath.FindLastSubString("/");
          const plUInt64 uiBatch = plHashingUtils::xxHash64(sPath.GetStartPointer(), szSlash ? (size_t)(szSlash - sPath.GetStartPointer()) : 0, uiCell);
          ++uiBatchedPlaceables;
          if (!batches.Insert(uiBatch))
          {
            objectsPerCell[uiCell]++;
            ++uiGprimBatches;
          }
          continue;
        }
      }
      else
      {
        (p.m_Kind == plUsdPlaceableKind::Instance ? uiInstances : uiPointInstances)++;
        prototypes.Insert(p.m_uiPrototype);

        if (fExtent > settings.m_fSplitSize)
          ++uiOversized;

        if (settings.m_fBatchSize > 0.0f && fExtent < settings.m_fBatchSize)
        {
          // One instanced object per cell and prototype.
          const plUInt64 uiBatch = plHashingUtils::xxHash64(&p.m_uiPrototype, sizeof(plUInt32), uiCell + 0x9e3779b97f4a7c15ull);
          ++uiBatchedPlaceables;
          if (!batches.Insert(uiBatch))
          {
            objectsPerCell[uiCell]++;
            ++uiInstanceBatches;
          }
          continue;
        }
      }

      objectsPerCell[uiCell]++;
      ++uiSingles;
    }

    plDynamicArray<plUInt32> counts;
    plUInt64 uiObjects = 0;
    for (auto it = objectsPerCell.GetIterator(); it.IsValid(); ++it)
    {
      counts.PushBack(it.Value());
      uiObjects += it.Value();
    }
    counts.Sort();

    auto percentile = [&](float f)
    { return counts.IsEmpty() ? 0u : counts[plMath::Min(counts.GetCount() - 1, (plUInt32)(f * counts.GetCount()))]; };

    plLog::Info("{}: {} placeables = {} instances + {} point instances + {} gprims, {} lights, {} cameras; {} without bounds", sLabel, uiCount, uiInstances,
      uiPointInstances, uiGprims, uiLights, uiCameras, uiNoBounds);
    plLog::Info("  {} prototypes used, {} distinct loose geometries; extent {} x {} m", prototypes.GetCount(), geometryKeys.GetCount(),
      plArgF(extent.IsValid() ? extent.m_vMax.x - extent.m_vMin.x : 0.0f, 0), plArgF(extent.IsValid() ? extent.m_vMax.y - extent.m_vMin.y : 0.0f, 0));
    plLog::Info("  {} cells of {} m, {} objects ({} single, {} instance batches, {} merged gprim groups, {} split pieces; {} placeables batched); objects per cell p50 {} p90 {} max {}",
      counts.GetCount(), settings.m_fCellSize, uiObjects, uiSingles, uiInstanceBatches, uiGprimBatches, uiSplitPieces, uiBatchedPlaceables, percentile(0.5f),
      percentile(0.9f), counts.IsEmpty() ? 0u : counts.PeekBack());
    plLog::Info("  {} loose meshes over {} m split into <= {} pieces; {} instances over {} m (not split); {} placeables have their pivot in another cell",
      uiSplit, settings.m_fSplitSize, uiSplitPieces, uiOversized, settings.m_fSplitSize, uiPivotElsewhere);
  }

  /// Captures and builds geometry of the root just scanned: some of its new prototypes and loose gprims, and every loose
  /// mesh over the split size, split along the cells.
  void CaptureRoot(const plUsdPartitionScan& scan, const plUsdScanResult& result, plUInt32 uiRoot, plUInt32 uiMaxCaptures, const PartitionPlanSettings& settings)
  {
    const plUsdScanRoot& root = result.m_Roots[uiRoot];

    auto build = [](const plUsdCapturedMesh& mesh, plGeometry& out_geometry, plDynamicArray<plUInt32>& out_groups) -> plResult
    {
      const plStatus res = mesh.m_pSource->Build(out_geometry, &out_groups);
      if (res.Failed())
        plLog::Error("  build failed: {}", res.m_sMessage);
      return res.Succeeded() ? PL_SUCCESS : PL_FAILURE;
    };

    plTime captureTime, buildTime;
    plUInt32 uiProtos = 0, uiFailed = 0;
    plUInt64 uiVertices = 0, uiTriangles = 0, uiMaterials = 0;

    for (plUInt32 i = 0; i < result.m_Prototypes.GetCount() && uiProtos < uiMaxCaptures; ++i)
    {
      const plUsdScanPrototype& proto = result.m_Prototypes[i];
      if (proto.FindSource(uiRoot) == nullptr)
        continue;

      ++uiProtos;
      plTime t = plTime::Now();
      plUsdCapturedMesh mesh;
      const plStatus res = scan.CapturePrototype(proto, uiRoot, mesh);
      captureTime += plTime::Now() - t;
      if (res.Failed())
      {
        plLog::Error("  prototype '{}': {}", proto.m_sName, res.m_sMessage);
        ++uiFailed;
        continue;
      }

      if (mesh.m_uiSkipped > 0)
        plLog::Warning("  prototype '{}': {} gprim(s) skipped, first {}", proto.m_sName, mesh.m_uiSkipped, mesh.m_sSkipped);

      plGeometry geom;
      plDynamicArray<plUInt32> groups;
      t = plTime::Now();
      if (build(mesh, geom, groups).Failed())
      {
        ++uiFailed;
        continue;
      }
      buildTime += plTime::Now() - t;

      uiVertices += geom.GetVertices().GetCount();
      uiTriangles += geom.GetPolygons().GetCount();
      uiMaterials += mesh.m_Materials.GetCount();

      // The merged mesh must land where the scan says the prototype is.
      plBoundingBox built = plBoundingBox::MakeInvalid();
      for (const auto& v : geom.GetVertices())
        built.ExpandToInclude(v.m_vPosition);
      if (proto.m_LocalBounds.IsValid() && built.IsValid() && !proto.m_LocalBounds.Contains(built.GetCenter()))
        plLog::Warning("  prototype '{}': built bounds centre lies outside the scanned bounds", proto.m_sName);
    }

    plLog::Info("  captured {} prototypes ({} failed): {} vertices, {} triangles, {} material groups; capture {} ms, build {} ms", uiProtos, uiFailed,
      uiVertices, uiTriangles, uiMaterials, plArgF(captureTime.GetMilliseconds(), 1), plArgF(buildTime.GetMilliseconds(), 1));

    // Loose gprims: the first few, then every one over the split size.
    plUInt32 uiLoose = 0;
    plUInt32 uiSplit = 0, uiPieces = 0, uiSeamVertices = 0, uiSeamUnmatched = 0, uiSeamCracks = 0;
    plTime splitTime;
    float fMaxOverhang = 0.0f;
    uiVertices = uiTriangles = 0;
    captureTime = buildTime = plTime();

    for (plUInt32 i = root.m_uiFirstPlaceable; i < root.m_uiFirstPlaceable + root.m_uiPlaceableCount; ++i)
    {
      const plUsdPlaceable& p = result.m_Placeables[i];
      if (p.m_Kind != plUsdPlaceableKind::Gprim)
        continue;

      const bool bSplit = ExtentXY(p.m_WorldBounds) > settings.m_fSplitSize;
      if (!bSplit && uiLoose >= uiMaxCaptures)
        continue;

      const plStringView sPath = result.GetPath(p);
      plTime t = plTime::Now();
      plUsdCapturedMesh mesh;
      if (const plStatus res = scan.CaptureGprim(sPath, mesh); res.Failed())
      {
        plLog::Error("  gprim '{}': {}", sPath, res.m_sMessage);
        continue;
      }
      captureTime += plTime::Now() - t;

      plGeometry geom;
      plDynamicArray<plUInt32> groups;
      t = plTime::Now();
      if (build(mesh, geom, groups).Failed())
        continue;
      buildTime += plTime::Now() - t;

      if (!bSplit)
      {
        ++uiLoose;
        uiVertices += geom.GetVertices().GetCount();
        uiTriangles += geom.GetPolygons().GetCount();
        continue;
      }

      t = plTime::Now();
      plDynamicArray<plUsdCellPiece> pieces;
      const plStatus splitRes = plUsdPartitionScan::SplitByCell(geom, groups, p.m_WorldTransform.GetAsMat4(), settings.m_fCellSize, settings.m_uiMaxSplitCells, pieces);
      const plTime thisSplit = plTime::Now() - t;
      splitTime += thisSplit;
      if (splitRes.Failed())
      {
        plLog::Warning("  split {}: not split, {}", sPath.GetFileName(), splitRes.m_sMessage);
        continue;
      }

      // Cutting must keep the surface area.
      auto area = [](const plGeometry& g, const plMat4& m)
      {
        double fArea = 0.0;
        for (const auto& poly : g.GetPolygons())
        {
          const plVec3 a = m.TransformPosition(g.GetVertices()[poly.m_Vertices[0]].m_vPosition);
          for (plUInt32 c = 1; c + 1 < poly.m_Vertices.GetCount(); ++c)
          {
            const plVec3 b = m.TransformPosition(g.GetVertices()[poly.m_Vertices[c]].m_vPosition);
            const plVec3 d = m.TransformPosition(g.GetVertices()[poly.m_Vertices[c + 1]].m_vPosition);
            fArea += 0.5 * (b - a).CrossRH(d - a).GetLength();
          }
        }
        return fArea;
      };

      const double fSourceArea = area(geom, p.m_WorldTransform.GetAsMat4());
      double fPieceArea = 0.0;
      plUInt32 uiPieceTriangles = 0;
      float fOverhang = 0.0f;

      // Placed vertex positions of each piece, by their bits.
      auto positionKey = [](const plVec3& v) { return plHashingUtils::xxHash64(&v, sizeof(plVec3)); };
      plHashTable<plUInt64, plHashSet<plUInt64>> placed;
      plHashTable<plUInt64, const plUsdCellPiece*> pieceByCell;
      for (const plUsdCellPiece& piece : pieces)
      {
        pieceByCell[CellKey(piece.m_Cell)] = &piece;
        plHashSet<plUInt64>& keys = placed[CellKey(piece.m_Cell)];
        for (const auto& v : piece.m_Geometry.GetVertices())
          keys.Insert(positionKey(v.m_vPosition + piece.m_vOrigin));
      }

      // An unmatched border vertex with a neighbour vertex this close is a crack; without one it is the mesh's own edge.
      auto nearInNeighbour = [&](const plVec3& vPos, const plVec2I32& cell)
      {
        const plUsdCellPiece* const* ppPiece = pieceByCell.GetValue(CellKey(cell));
        if (ppPiece == nullptr)
          return false;
        for (const auto& v : (*ppPiece)->m_Geometry.GetVertices())
        {
          if ((v.m_vPosition + (*ppPiece)->m_vOrigin - vPos).GetLengthSquared() < 1e-6f)
            return true;
        }
        return false;
      };

      plUInt32 uiBorderVertices = 0, uiUnmatched = 0, uiCracks = 0;
      for (const plUsdCellPiece& piece : pieces)
      {
        uiPieceTriangles += piece.m_Geometry.GetPolygons().GetCount();
        fPieceArea += area(piece.m_Geometry, plMat4::MakeIdentity());

        // How far the piece reaches past its cell's square, and whether its border vertices are in the neighbours.
        const plBoundingBox rect = plUsdPartitionScan::CellRect(piece.m_Cell, settings.m_fCellSize);
        for (const auto& v : piece.m_Geometry.GetVertices())
        {
          const plVec3 vPos = v.m_vPosition + piece.m_vOrigin;
          fOverhang = plMath::Max(fOverhang, plMath::Max(rect.m_vMin.x - vPos.x, rect.m_vMin.y - vPos.y));
          fOverhang = plMath::Max(fOverhang, plMath::Max(vPos.x - rect.m_vMax.x, vPos.y - rect.m_vMax.y));

          const plInt32 dx = vPos.x == rect.m_vMin.x ? -1 : (vPos.x == rect.m_vMax.x ? 1 : 0);
          const plInt32 dy = vPos.y == rect.m_vMin.y ? -1 : (vPos.y == rect.m_vMax.y ? 1 : 0);
          if (dx == 0 && dy == 0)
            continue;

          ++uiBorderVertices;
          const plUInt64 uiKey = positionKey(vPos);
          auto inNeighbour = [&](plInt32 iX, plInt32 iY)
          {
            const plHashSet<plUInt64>* pKeys = placed.GetValue(CellKey(plVec2I32(piece.m_Cell.x + iX, piece.m_Cell.y + iY)));
            return pKeys != nullptr && pKeys->Contains(uiKey);
          };
          const bool bMissX = dx != 0 && !inNeighbour(dx, 0);
          const bool bMissY = dy != 0 && !inNeighbour(0, dy);
          if (bMissX || bMissY)
          {
            ++uiUnmatched;
            if ((bMissX && nearInNeighbour(vPos, plVec2I32(piece.m_Cell.x + dx, piece.m_Cell.y))) ||
                (bMissY && nearInNeighbour(vPos, plVec2I32(piece.m_Cell.x, piece.m_Cell.y + dy))))
              ++uiCracks;
          }
        }
      }

      ++uiSplit;
      uiPieces += pieces.GetCount();
      fMaxOverhang = plMath::Max(fMaxOverhang, fOverhang);
      uiSeamVertices += uiBorderVertices;
      uiSeamUnmatched += uiUnmatched;
      uiSeamCracks += uiCracks;
      const bool bAreaKept = plMath::Abs(fPieceArea - fSourceArea) <= 1e-3 * plMath::Max(fSourceArea, 1.0);
      plLog::Info("  split {}: {} m, {} triangles -> {} pieces, {} triangles, area {} m2 {}, overhang {} m, border vertices {} ({} on the mesh edge, {} "
                  "cracks), {} ms",
        sPath.GetFileName(), plArgF(ExtentXY(p.m_WorldBounds), 0), geom.GetPolygons().GetCount(), pieces.GetCount(), uiPieceTriangles, plArgF(fSourceArea, 0),
        bAreaKept ? "kept" : "CHANGED", plArgF(fOverhang, 2), uiBorderVertices, uiUnmatched - uiCracks, uiCracks, plArgF(thisSplit.GetMilliseconds(), 1));
    }

    plLog::Info("  captured {} loose gprims: {} vertices, {} triangles; split {} meshes into {} pieces (max overhang {} m; {} border vertices, {} on the "
                "mesh edge, {} cracks); capture {} ms, build {} ms, split {} ms",
      uiLoose, uiVertices, uiTriangles, uiSplit, uiPieces, plArgF(fMaxOverhang, 2), uiSeamVertices, uiSeamUnmatched - uiSeamCracks, uiSeamCracks,
      plArgF(captureTime.GetMilliseconds(), 1), plArgF(buildTime.GetMilliseconds(), 1), plArgF(splitTime.GetMilliseconds(), 1));
  }

  int PartitionList(plStringView sFile, plStringView sPrimPath)
  {
    plUsdPartitionScan scan;
    const plTime start = plTime::Now();
    if (const plStatus res = scan.Open(sFile); res.Failed())
      return Fail(res.m_sMessage);
    plLog::Info("Opened without payloads in {} ms, {}", plArgF((plTime::Now() - start).GetMilliseconds(), 0), MemoryText());

    plDynamicArray<plUsdPartitionScan::ChildInfo> children;
    scan.ListChildren(sPrimPath, children);
    for (const plUsdPartitionScan::ChildInfo& child : children)
    {
      plStringBuilder sLine;
      sLine.SetFormat("{} [{}]{}{}", child.m_sPath, child.m_sTypeName, child.m_bHasPayload ? " payload" : "", child.m_bLoaded ? "" : " (unloaded)");
      for (const plUsdVariantSet& set : child.m_VariantSets)
        sLine.AppendFormat(" {}={}", set.m_sName, set.m_sSelection);
      if (child.m_WorldBounds.IsValid())
      {
        const plVec3 vSize = child.m_WorldBounds.GetHalfExtents() * 2.0f;
        sLine.AppendFormat(" size {} x {} x {} m", plArgF(vSize.x, 0), plArgF(vSize.y, 0), plArgF(vSize.z, 0));
      }
      else
      {
        sLine.Append(" size -");
      }
      plLog::Info("{}", sLine);
    }
    return 0;
  }

  int PartitionScan(plStringView sFile, plArrayPtr<const char*> args)
  {
    PartitionPlanSettings settings;
    plUsdScanOptions options;
    plDynamicArray<plString> roots;
    plUInt32 uiMaxCaptures = 50;

    for (plUInt32 i = 0; i + 1 < args.GetCount(); i += 2)
    {
      const plStringView sArg = args[i];
      const plStringView sValue = args[i + 1];
      double fValue = 0.0;

      if (sArg == "--roots")
      {
        plHybridArray<plStringView, 8> parts;
        sValue.Split(false, parts, ",");
        for (plStringView s : parts)
          roots.PushBack(s);
      }
      else if (sArg == "--variant")
      {
        const char* szEquals = sValue.FindSubString("=");
        if (szEquals == nullptr)
          return FailFmt(plFmt("--variant expects set=value, not '{}'", sValue));
        plUsdVariantOverride& v = options.m_Variants.ExpandAndGetRef();
        v.m_sSet = plStringView(sValue.GetStartPointer(), szEquals);
        v.m_sVariant = plStringView(szEquals + 1, sValue.GetEndPointer());
      }
      else if (sArg == "--capture")
      {
        plConversionUtils::StringToUInt(sValue, uiMaxCaptures).IgnoreResult();
      }
      else if (sArg == "--keys")
      {
        options.m_bGeometryKeys = sValue != "0";
      }
      else if (sArg == "--max-cells")
      {
        plConversionUtils::StringToUInt(sValue, settings.m_uiMaxSplitCells).IgnoreResult();
      }
      else if (sArg == "--max-payloads")
      {
        plConversionUtils::StringToUInt(sValue, options.m_uiMaxRootPayloads).IgnoreResult();
      }
      else if (plConversionUtils::StringToFloat(sValue, fValue).Succeeded() && (sArg == "--cell" || sArg == "--batch" || sArg == "--split"))
      {
        (sArg == "--cell" ? settings.m_fCellSize : sArg == "--batch" ? settings.m_fBatchSize : settings.m_fSplitSize) = (float)fValue;
      }
      else
      {
        return FailFmt(plFmt("unknown option '{}'", sArg));
      }
    }

    if (settings.m_fSplitSize <= 0.0f)
      settings.m_fSplitSize = settings.m_fCellSize;

    const plTime totalStart = plTime::Now();
    plUsdPartitionScan scan;
    if (const plStatus res = scan.Open(sFile); res.Failed())
      return Fail(res.m_sMessage);

    const plUsdBasis& basis = scan.GetBasis();
    plLog::Info("Opened '{}' without payloads in {} ms: upAxis {}, metersPerUnit {}; {}", sFile, plArgF((plTime::Now() - totalStart).GetMilliseconds(), 0),
      basis.GetUpAxis() == plUsdBasis::UpAxis::Y ? "Y" : "Z", basis.GetMetersPerUnit(), MemoryText());

    if (roots.IsEmpty())
    {
      // The highest payload prims, a few levels deep; the top level when there are none.
      auto excluded = [&](plStringView sPath)
      {
        for (const plString& sExcluded : options.m_ExcludePaths)
        {
          if (sPath == sExcluded || (sPath.StartsWith(sExcluded) && sPath.GetElementCount() > sExcluded.GetElementCount() &&
                                      sPath.GetStartPointer()[sExcluded.GetElementCount()] == '/'))
            return true;
        }
        return false;
      };

      plDynamicArray<plString> level;
      level.PushBack("/");
      for (plUInt32 uiDepth = 0; uiDepth < 8 && !level.IsEmpty(); ++uiDepth)
      {
        plDynamicArray<plString> next;
        for (const plString& sParent : level)
        {
          plDynamicArray<plUsdPartitionScan::ChildInfo> children;
          scan.ListChildren(sParent, children);
          for (const auto& child : children)
          {
            if (excluded(child.m_sPath))
              continue;
            (child.m_bHasPayload ? roots : next).PushBack(child.m_sPath);
          }
        }
        level = std::move(next);
      }

      if (roots.IsEmpty())
      {
        plDynamicArray<plUsdPartitionScan::ChildInfo> children;
        scan.ListChildren("/", children);
        for (const auto& child : children)
        {
          if (!excluded(child.m_sPath))
            roots.PushBack(child.m_sPath);
        }
      }

      plLog::Info("No --roots: scanning {} roots", roots.GetCount());
    }

    plUsdScanResult result;
    for (const plString& sRoot : roots)
    {
      // Variant sets the options do not name take their most detailed variant, as the editor's import does.
      plUsdScanOptions rootOptions = options;
      scan.AddPreferredVariants(sRoot, rootOptions.m_Variants);

      const plUInt32 uiRoot = result.m_Roots.GetCount();
      if (const plStatus res = scan.ScanRoot(sRoot, rootOptions, result); res.Failed())
      {
        plLog::Error("{}: {}", sRoot, res.m_sMessage);
        continue;
      }

      const plUsdScanRoot& root = result.m_Roots[uiRoot];
      const plVec3 vSize = root.m_WorldBounds.IsValid() ? root.m_WorldBounds.GetHalfExtents() * 2.0f : plVec3::MakeZero();
      plLog::Info("Root {}: load {} ms, scan {} ms (bounds {}, prototypes {}, geometry keys {}), {}", sRoot, plArgF(root.m_LoadTime.GetMilliseconds(), 0),
        plArgF(root.m_ScanTime.GetMilliseconds(), 0), plArgF(root.m_BoundsTime.GetMilliseconds(), 0), plArgF(root.m_PrototypeTime.GetMilliseconds(), 0),
        plArgF(root.m_KeyTime.GetMilliseconds(), 0), MemoryText());
      plLog::Info("  {} prims visited; skipped {} guide/proxy, {} invisible, {} excluded, {} unsupported; {} new prototypes; bounds {} x {} x {} m",
        root.m_uiPrims, root.m_uiSkippedPurpose, root.m_uiSkippedInvisible, root.m_uiSkippedExcluded, root.m_uiSkippedUnsupported, root.m_uiNewPrototypes,
        plArgF(vSize.x, 0), plArgF(vSize.y, 0), plArgF(vSize.z, 0));

      PrintPlan("  plan", result, root.m_uiFirstPlaceable, root.m_uiPlaceableCount, settings);

      const plTime captureStart = plTime::Now();
      CaptureRoot(scan, result, uiRoot, uiMaxCaptures, settings);
      plLog::Info("  geometry checks {} ms, {}", plArgF((plTime::Now() - captureStart).GetMilliseconds(), 0), MemoryText());

      const plTime releaseStart = plTime::Now();
      scan.ReleaseRoot(sRoot);
      plLog::Info("  released in {} ms, {}", plArgF((plTime::Now() - releaseStart).GetMilliseconds(), 0), MemoryText());
    }

    plUInt64 uiPrototypePoints = 0;
    for (const plUsdScanPrototype& proto : result.m_Prototypes)
      uiPrototypePoints += proto.m_uiPoints;

    plLog::Info("Total over {} roots: {} prototypes ({} points)", result.m_Roots.GetCount(), result.m_Prototypes.GetCount(), uiPrototypePoints);
    PrintPlan("Total", result, 0, result.m_Placeables.GetCount(), settings);
    plLog::Info("Total time {} s, {}", plArgF((plTime::Now() - totalStart).GetSeconds(), 1), MemoryText());
    return 0;
  }
} // namespace

int main(int argc, const char** argv)
{
  plStartup::StartupCoreSystems();
  plGlobalLog::AddLogWriter(plLogWriter::Console::LogMessageHandler);

  int iResult = 0;

  {
    plDynamicArray<plString> searched;
    plStatus res = plUsdRuntime::Initialize(&searched);
    if (res.Failed())
    {
      for (const plString& s : searched)
        plLog::Info("searched {}", s);

      iResult = Fail(res.m_sMessage);
    }
    else
    {
      plLog::Info("OpenUSD {} from '{}'", plUsdStage::GetOpenUsdVersion(), plUsdRuntime::GetOpenUsdDirectory());
    }
  }

  if (iResult == 0)
  {
    const plStatus basis = plUsdBasis::SelfTest();
    if (basis.Failed())
      iResult = FailFmt(plFmt("basis self-test failed: {}", basis.m_sMessage));
    else
      plLog::Success("Basis self-test passed");
  }

  if (iResult == 0 && argc >= 2 && plStringView(argv[1]) == "--tangent-test")
  {
    iResult = TangentRegression();
  }
  else if (iResult == 0 && argc >= 3 && plStringView(argv[1]) == "--texture")
  {
    // plUsdInspect --texture <output.dds> <colour file> [opacity file]
    const plStringView sOpacity = argc >= 5 ? plStringView(argv[4]) : plStringView();
    const plStatus res = plUsdTexture::PrepareForPreview(argv[3], sOpacity, true, 1024, argv[2]);
    if (res.Failed())
      iResult = Fail(res.m_sMessage);
    else
      plLog::Success("Wrote '{}'", argv[2]);
  }
  else if (iResult == 0 && argc >= 3 && plStringView(argv[1]) == "--partition-list")
  {
    iResult = PartitionList(argv[2], argc >= 4 ? plStringView(argv[3]) : plStringView("/"));
  }
  else if (iResult == 0 && argc >= 3 && plStringView(argv[1]) == "--partition-scan")
  {
    iResult = PartitionScan(argv[2], plArrayPtr<const char*>(argv + 3, argc - 3));
  }
  else if (iResult == 0 && argc >= 2 && plStringView(argv[1]) != "--selftest")
  {
    const plStringView sFile = argv[1];

    if (argc >= 4 && plStringView(argv[2]) == "--roundtrip")
    {
      iResult = RoundTrip(sFile, argv[3]);
    }
    else if (argc >= 3 && plStringView(argv[2]) == "--values-roundtrip")
    {
      iResult = ValuesRoundTrip(sFile);
    }
    else if (argc >= 4 && plStringView(argv[2]) == "--mesh-profile")
    {
      iResult = MeshProfile(sFile, argv[3]);
    }
    else if (argc >= 3 && plStringView(argv[2]) == "--light-test")
    {
      iResult = LightRegression(sFile);
    }
    else if (argc >= 3 && plStringView(argv[2]) == "--profile")
    {
      plUInt32 uiMaxBuilds = 0xFFFFFFFF;
      if (argc >= 4)
        plConversionUtils::StringToUInt(argv[3], uiMaxBuilds).IgnoreResult();
      iResult = Profile(sFile, uiMaxBuilds, argc >= 5 && plStringView(argv[4]) == "tangents");
    }
    else if (argc >= 3 && plStringView(argv[2]) == "--commit-test")
    {
      iResult = CommitTest(sFile);
    }
    else if (argc >= 3 && plStringView(argv[2]) == "--variant-test")
    {
      iResult = VariantTest(sFile);
    }
    else if (argc >= 3 && plStringView(argv[2]) == "--component-test")
    {
      iResult = ComponentTest(sFile);
    }
    else if (argc >= 4 && plStringView(argv[2]) == "--materialx")
    {
      iResult = InspectWithMaterialX(sFile, argv[3]);
    }
    else
    {
      plUsdStage stage;
      plStatus res = stage.Open(sFile);
      if (res.Failed())
        iResult = Fail(res.m_sMessage);
      else
        PrintStage(stage, argc >= 3 && plStringView(argv[2]) == "--geometry");
    }
  }

  plGlobalLog::RemoveLogWriter(plLogWriter::Console::LogMessageHandler);
  plStartup::ShutdownCoreSystems();
  return iResult;
}
