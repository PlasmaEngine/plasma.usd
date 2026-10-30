#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/Graphics/Geometry.h>
#include <EditorPluginUsd/Import/UsdModelImporter.h>
#include <EditorPluginUsd/Import/UsdSkelImport.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/Set.h>
#include <Foundation/IO/Archive/Archive.h>
#include <Foundation/IO/Archive/ArchiveUtils.h>
#include <Foundation/IO/MemoryMappedFile.h>
#include <Foundation/IO/Stream.h>
#include <Foundation/Math/Color8UNorm.h>
#include <Foundation/Strings/PathUtils.h>
#include <RendererCore/Declarations.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>
#include <UsdBridge/UsdSkel.h>
#include <UsdBridge/UsdStage.h>

struct plUsdModelImporter::PackageFile
{
  plString m_sPath;
  plMemoryMappedFile m_File;
  plArchiveTOC m_TOC;
  bool m_bValid = false;
};

namespace
{
  using namespace plModelImporter2;

  /// Point instances are copied into the mesh until they make up this many triangles; the rest are left out.
  constexpr plUInt64 MaxInstancedTriangles = 10'000'000;

  /// What one import knows about each prim ReadPrims reported, by the same index.
  struct PrimState
  {
    bool m_bImported = false;          ///< Active and visible, and so is every ancestor.
    bool m_bHasGeometry = false;       ///< An imported Mesh or implicit shape.
    bool m_bPrototypeGeometry = false; ///< A Mesh or implicit shape below a point instancer, drawn only through it.
    bool m_bInstancer = false;         ///< An imported point instancer; copies of its prototypes go into the mesh.
    bool m_bInstancesIncluded = false; ///< For instancers: whether the name filters keep its instances.
    bool m_bIsNode = false;            ///< Imported, with geometry at or below it.
    plInt32 m_iNode = -1;              ///< Index into m_OutputNodes.

    /// Stage root frame, uniform import scale included.
    plTransform m_GlobalTransform = plTransform::MakeIdentity();

    /// For geometry prims, per material group: the output material, and whether the name filters keep its faces.
    plHybridArray<plUInt32, 4> m_GroupMaterials;
    plHybridArray<bool, 4> m_GroupIncluded;

    bool IsSelected() const { return m_GroupIncluded.Contains(true); }
  };

  /// The uniform scale of the asset's import transform. Its axis part describes DCC export conventions for formats that
  /// carry none, while a stage states its own up axis and unit, which the bridge has already applied.
  float GetImportScale(const plMat3& mRootTransform, plStringView sFile)
  {
    const plVec3 vScale = mRootTransform.GetScalingFactors();
    const float fScale = (vScale.x + vScale.y + vScale.z) / 3.0f;
    if (!(fScale > 0.0f))
      return 1.0f;

    // Identity is what callers that know nothing about the asset pass, the preset is what a new mesh asset starts with.
    const plMeshImportTransform::Enum preset = plMeshImportTransform::Default;
    const plBasisAxis::Enum rightDir = plMeshImportTransform::GetRightDir(preset, plBasisAxis::PositiveY);
    const plBasisAxis::Enum upDir = plMeshImportTransform::GetUpDir(preset, plBasisAxis::PositiveZ);
    const bool bFlipForward = plMeshImportTransform::GetFlipForward(preset, false);
    const plBasisAxis::Enum forwardDir = plBasisAxis::GetOrthogonalAxis(rightDir, upDir, !bFlipForward);
    const plMat3 mPreset = plBasisAxis::CalculateTransformationMatrix(forwardDir, rightDir, upDir);

    const plMat3 mAxes = mRootTransform * (1.0f / fScale);
    if (!mAxes.IsIdentity(0.001f) && !mAxes.IsEqual(mPreset, 0.001f))
    {
      plLog::Warning("'{}': the import axis settings are ignored for USD. The stage's upAxis and metersPerUnit define its frame; only the uniform scaling applies.", sFile);
    }

    return fScale;
  }

  void AppendFloatBits(plStringBuilder& ref_sKey, float f)
  {
    plUInt32 uiBits = 0;
    plMemoryUtils::Copy(&uiBits, reinterpret_cast<const plUInt32*>(&f), 1);
    ref_sKey.AppendFormat("|{}", plArgU(uiBits, 8, true, 16));
  }

  /// Equal keys for equal surfaces, so faces that look the same share one material and one sub-mesh.
  plString MakeSurfaceKey(const plUsdMaterialGroup& group)
  {
    const plUsdSurface& s = group.m_Surface;

    plStringBuilder sKey;
    if (!group.m_bHasSurface)
    {
      sKey.SetFormat("<none>|{}", s.m_bDoubleSided ? 1 : 0);
      return sKey;
    }

    sKey.SetFormat("{}|{}", s.m_sMaterialPath, s.m_bDoubleSided ? 1 : 0);

    for (float f : {s.m_BaseColor.r, s.m_BaseColor.g, s.m_BaseColor.b, s.m_fMetallic, s.m_fRoughness, s.m_fOpacity, s.m_fOpacityThreshold})
      AppendFloatBits(sKey, f);

    for (const plString* pFile : {&s.m_sBaseColorTexture, &s.m_sNormalTexture, &s.m_sRoughnessTexture, &s.m_sMetallicTexture, &s.m_sOpacityTexture})
      sKey.AppendFormat("|{}", *pFile);

    return sKey;
  }

  /// The material prim's name, a name made from the display colour, or "DefaultMaterial" for faces without either.
  void MakeMaterialBaseName(const plUsdMaterialGroup& group, plStringBuilder& out_sName)
  {
    const plUsdSurface& s = group.m_Surface;

    if (!group.m_bHasSurface)
    {
      out_sName = "DefaultMaterial";
    }
    else if (!s.m_sMaterialPath.IsEmpty())
    {
      plStringView sName = s.m_sMaterialPath;
      if (const char* szSlash = sName.FindLastSubString("/"))
        sName.SetStartPosition(szSlash + 1);

      out_sName = sName;
    }
    else
    {
      const plColorLinearUB color(s.m_BaseColor);
      out_sName.SetFormat("DisplayColor_{}{}{}", plArgU(color.r, 2, true, 16, true), plArgU(color.g, 2, true, 16, true), plArgU(color.b, 2, true, 16, true));
    }

    if (out_sName.IsEmpty())
      out_sName = "Material";
  }

  /// Material names become file names of material assets, so they are made valid and unique regardless of case.
  void MakeUniqueMaterialName(const plUsdMaterialGroup& group, plSet<plString>& ref_usedNames, plString& out_sName)
  {
    plStringBuilder sBase, sValid;
    MakeMaterialBaseName(group, sBase);
    plPathUtils::MakeValidFilename(sBase, '_', sValid);

    plStringBuilder sName = sValid;
    plStringBuilder sLower;

    for (plUInt32 uiSuffix = 2;; ++uiSuffix)
    {
      sLower = sName;
      sLower.ToLower();

      if (!ref_usedNames.Contains(sLower))
        break;

      sName.SetFormat("{}_{}", sValid, uiSuffix);
    }

    ref_usedNames.Insert(sLower);
    out_sName = sName;
  }

  void FillOpenPBR(const plUsdMaterialGroup& group, OpenPBRMaterialDesc& out_desc)
  {
    const plUsdSurface& s = group.m_Surface;

    out_desc.m_bTwoSided = s.m_bDoubleSided;

    // Faces without a surface keep the OpenPBR defaults.
    if (!group.m_bHasSurface)
      return;

    out_desc.m_BaseColor.m_Value = plColor(s.m_BaseColor.r, s.m_BaseColor.g, s.m_BaseColor.b);
    out_desc.m_BaseColor.m_sTexturePath = s.m_sBaseColorTexture;

    // In USD a texture connected to an input replaces the input's value; OpenPBR multiplies the two, so a textured
    // input's constant is 1.
    out_desc.m_BaseMetalness.m_fValue = s.m_sMetallicTexture.IsEmpty() ? s.m_fMetallic : 1.0f;
    out_desc.m_BaseMetalness.m_sTexturePath = s.m_sMetallicTexture;
    out_desc.m_SpecularRoughness.m_fValue = s.m_sRoughnessTexture.IsEmpty() ? s.m_fRoughness : 1.0f;
    out_desc.m_SpecularRoughness.m_sTexturePath = s.m_sRoughnessTexture;

    // One image feeding both is the glTF occlusion/roughness/metalness packing, which is what exporters that pack
    // these write. A standalone map is read from its first channel.
    if (!s.m_sMetallicTexture.IsEmpty() && s.m_sMetallicTexture == s.m_sRoughnessTexture)
    {
      out_desc.m_SpecularRoughness.m_Channel = TextureChannel::Green;
      out_desc.m_BaseMetalness.m_Channel = TextureChannel::Blue;
    }

    out_desc.m_sNormalTexture = s.m_sNormalTexture;

    out_desc.m_GeometryOpacity.m_fValue = s.m_fOpacity;
    out_desc.m_GeometryOpacity.m_sTexturePath = s.m_sOpacityTexture;
    out_desc.m_GeometryOpacity.m_Channel = (!s.m_sOpacityTexture.IsEmpty() && s.m_sOpacityTexture == s.m_sBaseColorTexture) ? TextureChannel::Alpha : TextureChannel::Red;

    if (!s.m_sOpacityTexture.IsEmpty())
    {
      // An opacity texture cuts: foliage cards, fences and chains are what it is used for. A threshold of 0 would
      // blend by the letter of UsdPreviewSurface, which renders those unsorted and without shadows.
      out_desc.m_AlphaMode = OpenPBRAlphaMode::Mask;
      out_desc.m_fAlphaCutoff = s.m_fOpacityThreshold > 0.0f ? s.m_fOpacityThreshold : 0.5f;
    }
    else if (s.m_fOpacity < 0.999f)
    {
      out_desc.m_AlphaMode = s.m_fOpacityThreshold > 0.0f ? OpenPBRAlphaMode::Mask : OpenPBRAlphaMode::Blend;
      out_desc.m_fAlphaCutoff = s.m_fOpacityThreshold;
    }
  }

  bool MatchesTag(const plUsdPrim& prim, plStringView sTag)
  {
    return prim.m_sPath.StartsWith_NoCase(sTag) || prim.m_sPath.EndsWith_NoCase(sTag) || prim.m_sName.StartsWith_NoCase(sTag) ||
           prim.m_sName.EndsWith_NoCase(sTag);
  }

  /// Moves geometry by a transform: normals by its inverse transpose, and a mirroring transform reverses the winding so
  /// front faces stay in front.
  void TransformGeometry(plGeometry& ref_geom, const plMat4& mTransform)
  {
    if (mTransform.IsIdentity(plMath::SmallEpsilon<float>()))
      return;

    const plMat3 mNormals = plUsdBasis::NormalMatrix(mTransform);
    const bool bMirrored = plUsdBasis::IsMirror(mTransform);

    for (plGeometry::Vertex& vertex : ref_geom.GetVertices())
    {
      vertex.m_vPosition = mTransform.TransformPosition(vertex.m_vPosition);
      vertex.m_vNormal = mNormals * vertex.m_vNormal;
      vertex.m_vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
    }

    if (bMirrored)
    {
      for (plGeometry::Polygon& poly : ref_geom.GetPolygons())
      {
        const plUInt32 uiCount = poly.m_Vertices.GetCount();
        for (plUInt32 i = 0; i < uiCount / 2; ++i)
          plMath::Swap(poly.m_Vertices[i], poly.m_Vertices[uiCount - 1 - i]);
      }
    }
  }

  /// Whether the UVs are laid out as UDIM tiles: every triangle lies within one tile, at least one lies outside tile
  /// 1001, and every tile is within columns 0-9 and rows 0-5. The bridge flips V to a top-left origin, so tile (column
  /// c, row r) covers u in [c, c + 1] and v in [-r, 1 - r]. Half-float UVs lose texel precision past the first tile.
  bool HasUdimLayout(const plDynamicArray<plGeometry::Vertex>& vertices, const plDynamicArray<plUInt32>& indices)
  {
    // a corner within this distance of a tile edge counts for the tiles on both sides
    constexpr float fEdgeEpsilon = 1e-4f;
    bool bLeavesFirstTile = false;

    for (plUInt32 uiTri = 0; uiTri + 2 < indices.GetCount(); uiTri += 3)
    {
      plInt32 iFloorULo = plMath::MinValue<plInt32>();
      plInt32 iFloorUHi = plMath::MaxValue<plInt32>();
      plInt32 iFloorVLo = plMath::MinValue<plInt32>();
      plInt32 iFloorVHi = plMath::MaxValue<plInt32>();

      for (plUInt32 uiCorner = 0; uiCorner < 3; ++uiCorner)
      {
        const plVec2& uv = vertices[indices[uiTri + uiCorner]].m_vTexCoord;

        // positive range test, so NaN fails it too
        if (!(uv.x >= -fEdgeEpsilon && uv.x <= 10.0f + fEdgeEpsilon && uv.y >= -5.0f - fEdgeEpsilon && uv.y <= 1.0f + fEdgeEpsilon))
          return false;

        iFloorULo = plMath::Max(iFloorULo, static_cast<plInt32>(plMath::Floor(uv.x - fEdgeEpsilon)));
        iFloorUHi = plMath::Min(iFloorUHi, static_cast<plInt32>(plMath::Floor(uv.x + fEdgeEpsilon)));
        iFloorVLo = plMath::Max(iFloorVLo, static_cast<plInt32>(plMath::Floor(uv.y - fEdgeEpsilon)));
        iFloorVHi = plMath::Min(iFloorVHi, static_cast<plInt32>(plMath::Floor(uv.y + fEdgeEpsilon)));
      }

      // the corners share no tile, so the triangle crosses a tile edge
      if (iFloorULo > iFloorUHi || iFloorVLo > iFloorVHi)
        return false;

      // a triangle lying along a tile edge belongs to the candidate tile nearest 1001
      const plInt32 iColumn = plMath::Clamp<plInt32>(0, iFloorULo, iFloorUHi);
      const plInt32 iRow = -plMath::Clamp<plInt32>(0, iFloorVLo, iFloorVHi);

      if (iColumn < 0 || iColumn > 9 || iRow < 0 || iRow > 5)
        return false;

      if (iColumn != 0 || iRow != 0)
        bLeavesFirstTile = true;
    }

    return bLeavesFirstTile;
  }
} // namespace

plUsdModelImporter::plUsdModelImporter() = default;
plUsdModelImporter::~plUsdModelImporter() = default;

plResult plUsdModelImporter::DoImport()
{
  plUsdStage stage;
  if (plStatus res = stage.Open(m_Options.m_sSourceFile); res.Failed())
  {
    plLog::Error("{}", res.m_sMessage);
    return PL_FAILURE;
  }

  plDynamicArray<plUsdPrim> prims;
  stage.ReadPrims(prims);

  const float fScale = GetImportScale(m_Options.m_RootTransform, m_Options.m_sSourceFile);
  const plTransform rootTransform = plTransform::Make(plVec3::MakeZero(), plQuat::MakeIdentity(), plVec3(fScale));

  // --- UsdSkel: the skeleton, a clip, and the influences of a skinned import. ---
  const bool bSkinned = m_Options.m_bImportSkinningData;
  const bool bSkeletal = bSkinned || m_Options.m_pSkeletonOutput != nullptr || m_Options.m_pAnimationOutput != nullptr;

  // Clip names are listed for skeletal imports, for imports without outputs (which ask what the file holds), and for
  // any import of a stage that has skeletons.
  bool bHasSkeletons = false;
  for (const plUsdPrim& prim : prims)
    bHasSkeletons = bHasSkeletons || prim.m_sTypeName == "Skeleton" || prim.m_sTypeName == "SkelRoot";

  plUsdSkelScene skelScene;
  if (bSkeletal || bHasSkeletons || m_Options.m_pMeshOutput == nullptr)
  {
    if (plStatus res = skelScene.Open(stage.GetRootLayerFile(), stage.GetBasis()); res.Failed())
    {
      if (bSkeletal)
      {
        plLog::Error("{}", res.m_sMessage);
        return PL_FAILURE;
      }

      plLog::Warning("The stage's animations cannot be listed: {}", res.m_sMessage);
    }

    for (const plUsdSkelAnimation& anim : skelScene.GetAnimations())
      m_OutputAnimationNames.PushBack(anim.m_sName);
  }

  const plUsdSkeletalFrame skeletalFrame = plUsdSkeletalFrame::Make(fScale);
  plInt32 iSkeleton = -1;

  if (bSkinned || m_Options.m_pSkeletonOutput != nullptr)
  {
    iSkeleton = plUsdSkelImport::PickSkeleton(skelScene);
    if (iSkeleton < 0)
    {
      plLog::Error("The stage has no UsdSkel Skeleton.");
      return PL_FAILURE;
    }
  }

  if (m_Options.m_pSkeletonOutput != nullptr)
  {
    PL_SUCCEED_OR_RETURN(plUsdSkelImport::ImportSkeleton(skelScene, iSkeleton, skeletalFrame, *m_Options.m_pSkeletonOutput));
  }

  if (m_Options.m_pAnimationOutput != nullptr)
  {
    PL_SUCCEED_OR_RETURN(plUsdSkelImport::ImportAnimation(skelScene, m_Options, skeletalFrame, *m_Options.m_pAnimationOutput));
  }

  plDynamicArray<PrimState> states;
  states.SetCount(prims.GetCount());

  // --- Which prims are imported. ReadPrims lists parents before children. ---
  for (plUInt32 i = 0; i < prims.GetCount(); ++i)
  {
    const plUsdPrim& prim = prims[i];
    PrimState& state = states[i];
    const PrimState* pParent = prim.m_iParent >= 0 ? &states[prim.m_iParent] : nullptr;
    const bool bGeometry = (prim.m_Kind == plUsdPrimKind::Mesh || prim.m_Kind == plUsdPrimKind::Gprim) && !prim.m_MaterialGroups.IsEmpty();

    // Prototypes are drawn only through their instancers, as OpenUSD's renderers do, wherever they sit; the instancer
    // leaves out the invisible parts of a prototype itself.
    if (prim.m_bPrototype)
    {
      state.m_bPrototypeGeometry = bGeometry && prim.m_bActive;
      continue;
    }

    // Invisibility is inherited; a deactivated prim has no children on the stage.
    if ((pParent != nullptr && !pParent->m_bImported) || !prim.m_bActive || !prim.m_bVisible)
      continue;

    state.m_bImported = true;
    state.m_GlobalTransform = (pParent != nullptr ? pParent->m_GlobalTransform : rootTransform) * prim.m_LocalTransform;
    state.m_bHasGeometry = bGeometry;
    state.m_bInstancer = prim.m_Kind == plUsdPrimKind::Instancer && !prim.m_Instancer.m_Meshes.IsEmpty();
  }

  // Branches without geometry, such as material scopes, lights and cameras, are left out of the hierarchy.
  for (plUInt32 i = prims.GetCount(); i > 0; --i)
  {
    PrimState& state = states[i - 1];
    if (state.m_bHasGeometry || state.m_bInstancer)
      state.m_bIsNode = true;

    if (state.m_bIsNode && prims[i - 1].m_iParent >= 0)
      states[prims[i - 1].m_iParent].m_bIsNode = true;
  }

  // --- Materials: one per distinct surface, named and ordered over the whole stage, so a filtered import names every
  // material the way the unfiltered one did. ---
  {
    plHashTable<plString, plUInt32> surfaceToMaterial;
    plSet<plString> usedNames;

    for (plUInt32 i = 0; i < prims.GetCount(); ++i)
    {
      PrimState& state = states[i];
      if (!state.m_bHasGeometry && !state.m_bPrototypeGeometry)
        continue;

      for (const plUsdMaterialGroup& group : prims[i].m_MaterialGroups)
      {
        const plString sKey = MakeSurfaceKey(group);

        plUInt32 uiMaterial = 0;
        if (!surfaceToMaterial.TryGetValue(sKey, uiMaterial))
        {
          uiMaterial = m_OutputMaterials.GetCount();
          surfaceToMaterial.Insert(sKey, uiMaterial);

          plModelImporter2::OutputMaterial& material = m_OutputMaterials.ExpandAndGetRef();
          MakeUniqueMaterialName(group, usedNames, material.m_sName);
          FillOpenPBR(group, material.m_OpenPBR);
        }

        state.m_GroupMaterials.PushBack(uiMaterial);
      }
    }
  }

  for (const plModelImporter2::OutputMaterial& material : m_OutputMaterials)
  {
    const plModelImporter2::OpenPBRMaterialDesc& pbr = material.m_OpenPBR;
    for (const plString* pFile : {&pbr.m_BaseColor.m_sTexturePath, &pbr.m_BaseMetalness.m_sTexturePath, &pbr.m_SpecularRoughness.m_sTexturePath, &pbr.m_sNormalTexture, &pbr.m_GeometryOpacity.m_sTexturePath})
      AddPackagedTexture(*pFile);
  }

  // --- Nodes and mesh names. ---
  for (plUInt32 i = 0; i < prims.GetCount(); ++i)
  {
    const plUsdPrim& prim = prims[i];
    PrimState& state = states[i];
    if (!state.m_bIsNode)
      continue;

    const plInt32 iNode = (plInt32)m_OutputNodes.GetCount();
    const plInt32 iParentNode = prim.m_iParent >= 0 ? states[prim.m_iParent].m_iNode : -1;
    state.m_iNode = iNode;

    plModelImporter2::OutputNode& node = m_OutputNodes.ExpandAndGetRef();
    node.m_sName = prim.m_sName;
    node.m_iParentIndex = iParentNode;
    node.m_LocalTransform = iParentNode >= 0 ? prim.m_LocalTransform : rootTransform * prim.m_LocalTransform;

    if (state.m_bHasGeometry || state.m_bInstancer)
    {
      node.m_MeshNames.PushBack(prim.m_sPath);
      m_OutputMeshNames.PushBack(prim.m_sPath);
    }

    if (iParentNode >= 0)
      m_OutputNodes[iParentNode].m_ChildNodeIndices.PushBack(iNode);
  }

  // --- Name filters, per material group. ---
  plDynamicArray<plStringView> nameIncludeTags;
  plDynamicArray<bool> materialIncluded;
  materialIncluded.SetCount(m_OutputMaterials.GetCount(), false);
  bool bHasIncludeTags = false;

  for (const plString& sTag : m_Options.m_MeshIncludeTags)
  {
    if (sTag.IsEmpty())
      continue;

    bHasIncludeTags = true;

    bool bNamesMaterial = false;
    for (plUInt32 m = 0; m < m_OutputMaterials.GetCount(); ++m)
    {
      if (m_OutputMaterials[m].m_sName.IsEqual_NoCase(sTag))
      {
        materialIncluded[m] = true;
        bNamesMaterial = true;
      }
    }

    if (!bNamesMaterial)
      nameIncludeTags.PushBack(sTag);
  }

  plDynamicArray<plUInt32> selectedPrims;

  for (plUInt32 i = 0; i < prims.GetCount(); ++i)
  {
    const plUsdPrim& prim = prims[i];
    PrimState& state = states[i];

    // An instancer is picked by its own path or name, and its instances go or stay together. Include tags that name a
    // material also keep the instances' faces of that material; see the geometry below.
    if (state.m_bInstancer)
    {
      if (!m_Options.m_MeshExactIncludeNames.IsEmpty())
      {
        for (const plString& sName : m_Options.m_MeshExactIncludeNames)
          state.m_bInstancesIncluded |= prim.m_sPath == sName || prim.m_sName == sName;
      }
      else if (bHasIncludeTags)
      {
        for (plStringView sTag : nameIncludeTags)
          state.m_bInstancesIncluded |= MatchesTag(prim, sTag);
      }
      else
      {
        state.m_bInstancesIncluded = true;
        for (const plString& sTag : m_Options.m_MeshExcludeTags)
        {
          if (!sTag.IsEmpty() && MatchesTag(prim, sTag))
            state.m_bInstancesIncluded = false;
        }
      }

      if (state.m_bInstancesIncluded)
        selectedPrims.PushBack(i);

      continue;
    }

    if (!state.m_bHasGeometry)
      continue;

    const plUInt32 uiNumGroups = prim.m_MaterialGroups.GetCount();
    state.m_GroupIncluded.SetCount(uiNumGroups, false);

    if (!m_Options.m_MeshExactIncludeNames.IsEmpty())
    {
      bool bIncluded = false;
      for (const plString& sName : m_Options.m_MeshExactIncludeNames)
      {
        if (prim.m_sPath == sName || prim.m_sName == sName)
        {
          bIncluded = true;
          break;
        }
      }

      for (plUInt32 g = 0; g < uiNumGroups; ++g)
        state.m_GroupIncluded[g] = bIncluded;
    }
    else if (bHasIncludeTags)
    {
      bool bPrimIncluded = false;
      for (plStringView sTag : nameIncludeTags)
      {
        if (MatchesTag(prim, sTag))
        {
          bPrimIncluded = true;
          break;
        }
      }

      for (plUInt32 g = 0; g < uiNumGroups; ++g)
        state.m_GroupIncluded[g] = bPrimIncluded || materialIncluded[state.m_GroupMaterials[g]];
    }
    else
    {
      bool bExcluded = false;
      for (const plString& sTag : m_Options.m_MeshExcludeTags)
      {
        if (!sTag.IsEmpty() && MatchesTag(prim, sTag))
        {
          bExcluded = true;
          break;
        }
      }

      for (plUInt32 g = 0; g < uiNumGroups; ++g)
        state.m_GroupIncluded[g] = !bExcluded;
    }

    if (state.IsSelected())
      selectedPrims.PushBack(i);
  }

  if (!m_Options.m_MeshExactIncludeNames.IsEmpty() && selectedPrims.IsEmpty())
  {
    plLog::Warning("No Mesh, shape or point instancer prim of the stage has a path or name listed in the exact include names.");
  }

  if (m_Options.m_pMeshOutput == nullptr)
    return PL_SUCCESS;

  // A single prim picked by name keeps its own frame; its placement comes from the node hierarchy. Skinned geometry is
  // where its skeleton binds it instead.
  const bool bPrimFrame = !bSkinned && !m_Options.m_MeshExactIncludeNames.IsEmpty() && selectedPrims.GetCount() == 1;
  const plMat4 mScale = plMat4::MakeScaling(plVec3(fScale));

  // Without an exact selection every geometry prim is built, so nodes report the bounds of filtered-out helper
  // geometry too, as the hierarchy describes the whole stage.
  const bool bBuildAll = m_Options.m_MeshExactIncludeNames.IsEmpty();

  // --- Geometry. ---
  plDynamicArray<plGeometry::Vertex> vertices;
  plDynamicArray<plDynamicArray<plUInt32>> materialIndices;
  materialIndices.SetCount(m_OutputMaterials.GetCount());

  plDynamicArray<plUInt32> vertexRemap;
  plDynamicArray<plUInt32> groupTriangles;

  // Skinned imports: influences parallel to 'vertices', and per vertex of the geometry being added.
  plDynamicArray<plUsdSkinVertex> skinVertices;
  plDynamicArray<plUsdSkinVertex> geomSkin;
  plUInt32 uiTruncatedPoints = 0;

  // Adds the triangles of the material groups isIncluded keeps. Build orders the triangles by material group, so each
  // group is one range.
  auto addTriangles = [&](const plGeometry& geom, const plHybridArray<plUInt32, 4>& groupMaterials, auto isIncluded)
  {
    const plDeque<plGeometry::Vertex>& geomVertices = geom.GetVertices();
    const plDeque<plGeometry::Polygon>& geomPolygons = geom.GetPolygons();

    vertexRemap.Clear();
    vertexRemap.SetCount(geomVertices.GetCount(), plInvalidIndex);

    plUInt32 uiPolygon = 0;
    for (plUInt32 g = 0; g < groupTriangles.GetCount(); ++g)
    {
      const plUInt32 uiGroupEnd = plMath::Min(uiPolygon + groupTriangles[g], geomPolygons.GetCount());

      if (!isIncluded(g))
      {
        uiPolygon = uiGroupEnd;
        continue;
      }

      plDynamicArray<plUInt32>& indices = materialIndices[groupMaterials[g]];

      for (; uiPolygon < uiGroupEnd; ++uiPolygon)
      {
        const plGeometry::Polygon& poly = geomPolygons[uiPolygon];

        // Fanned, in case a polygon arrives with more than three corners.
        for (plUInt32 c = 1; c + 1 < poly.m_Vertices.GetCount(); ++c)
        {
          for (plUInt32 uiCorner : {0u, c, c + 1})
          {
            const plUInt32 uiVertex = poly.m_Vertices[uiCorner];
            if (vertexRemap[uiVertex] == plInvalidIndex)
            {
              vertexRemap[uiVertex] = vertices.GetCount();
              vertices.PushBack(geomVertices[uiVertex]);

              if (bSkinned)
              {
                const plVec4U16& vSkinIndex = geomVertices[uiVertex].m_BoneIndices;
                skinVertices.PushBack(geomSkin[vSkinIndex.x | ((plUInt32)vSkinIndex.y << 16)]);
              }
            }

            indices.PushBack(vertexRemap[uiVertex]);
          }
        }
      }
    }
  };

  // Point instancers find their prototype meshes by path.
  plHashTable<plString, plUInt32> geometryPrims;
  for (plUInt32 i = 0; i < prims.GetCount(); ++i)
  {
    if (states[i].m_bHasGeometry || states[i].m_bPrototypeGeometry)
      geometryPrims.Insert(prims[i].m_sPath, i);
  }

  plUInt64 uiInstancedTriangles = 0;
  plUInt32 uiInstancesLeftOut = 0;

  for (plUInt32 i = 0; i < prims.GetCount(); ++i)
  {
    const plUsdPrim& prim = prims[i];
    const PrimState& state = states[i];

    // A copy of each prototype mesh per instance, placed by the instance's transform. Skinned imports leave instancers
    // out, as UsdSkel binds no instances.
    if (state.m_bInstancer)
    {
      const bool bMaterialTags = bHasIncludeTags && m_Options.m_MeshExactIncludeNames.IsEmpty();
      if (bSkinned || (!state.m_bInstancesIncluded && !bMaterialTags))
        continue;

      const plMat4 mInstancer = bPrimFrame ? mScale : state.m_GlobalTransform.GetAsMat4();

      for (const plUsdInstancedMesh& mesh : prim.m_Instancer.m_Meshes)
      {
        plUInt32 uiPrototype = 0;
        if (!geometryPrims.TryGetValue(mesh.m_sMeshPath, uiPrototype))
          continue;

        const PrimState& prototype = states[uiPrototype];

        plStatus res;
        plGeometry prototypeGeom;
        plUniquePtr<plUsdGeometrySource> pSource = stage.CaptureGeometry(mesh.m_sMeshPath, res);
        if (pSource != nullptr)
          res = pSource->Build(prototypeGeom, &groupTriangles);

        if (res.Succeeded() && groupTriangles.GetCount() != prototype.m_GroupMaterials.GetCount())
          res = plStatus("its geometry and its prim have different material groups");

        if (res.Failed())
        {
          plLog::Warning("Skipping the instances of '{}': {}", mesh.m_sMeshPath, res.m_sMessage);
          continue;
        }

        for (const plTransform& instance : mesh.m_Instances)
        {
          if (uiInstancedTriangles >= MaxInstancedTriangles)
          {
            ++uiInstancesLeftOut;
            continue;
          }

          plGeometry geom = prototypeGeom;
          TransformGeometry(geom, mInstancer * instance.GetAsMat4());

          if (m_Options.m_bRecomputeNormals)
          {
            geom.ComputeFaceNormals();
            geom.ComputeSmoothVertexNormals();
          }

          geom.ComputeTangents();

          plModelImporter2::OutputNode& node = m_OutputNodes[state.m_iNode];
          for (const plGeometry::Vertex& vertex : geom.GetVertices())
            node.m_MeshBounds.ExpandToInclude(vertex.m_vPosition);

          addTriangles(geom, prototype.m_GroupMaterials, [&](plUInt32 g)
            { return state.m_bInstancesIncluded || (bMaterialTags && materialIncluded[prototype.m_GroupMaterials[g]]); });

          uiInstancedTriangles += geom.GetPolygons().GetCount();
        }
      }

      continue;
    }

    if (!state.m_bHasGeometry)
      continue;

    const bool bSelected = state.IsSelected();
    if (!bSelected && !bBuildAll)
      continue;

    plInt32 iSkinnedPrim = -1;
    if (bSkinned)
    {
      iSkinnedPrim = skelScene.FindSkinnedPrim(prim.m_sPath);
      if (iSkinnedPrim < 0 || skelScene.GetSkinnedPrims()[iSkinnedPrim].m_iSkeleton != iSkeleton)
      {
        if (bSelected)
          plLog::Warning("'{}' is not deformed by Skeleton '{}', so it is not part of the skinned mesh.", prim.m_sPath, skelScene.GetSkeletons()[iSkeleton].m_sPath);
        continue;
      }
    }

    plStatus res;
    plUniquePtr<plUsdGeometrySource> pSource = stage.CaptureGeometry(prim.m_sPath, res);
    if (pSource == nullptr)
    {
      plLog::Warning("Skipping '{}': {}", prim.m_sPath, res.m_sMessage);
      continue;
    }

    plGeometry geom;
    res = pSource->Build(geom, &groupTriangles);
    if (res.Failed())
    {
      plLog::Warning("Skipping '{}': {}", prim.m_sPath, res.m_sMessage);
      continue;
    }

    if (groupTriangles.GetCount() != prim.m_MaterialGroups.GetCount())
    {
      plLog::Warning("Skipping '{}': its geometry has {} material groups, its prim {}.", prim.m_sPath, groupTriangles.GetCount(), prim.m_MaterialGroups.GetCount());
      continue;
    }

    if (geom.GetPolygons().IsEmpty())
      continue;

    if (bSkinned)
    {
      const plUsdSkinnedPrim& skinned = skelScene.GetSkinnedPrims()[iSkinnedPrim];

      if (bSelected)
      {
        if (skinned.m_bHasBlendShapes)
          plLog::Warning("'{}' has blend shapes, which are not imported.", prim.m_sPath);
        if (skinned.m_bDualQuaternion)
          plLog::Warning("'{}' asks for dual quaternion skinning; it is skinned linearly.", prim.m_sPath);

        // Influences are matched to vertices by position, so this runs on the geometry as Build made it.
        res = plUsdSkelImport::ComputeSkinVertices(skelScene, (plUInt32)iSkinnedPrim, geom, m_Options.m_bNormalizeWeights, geomSkin, uiTruncatedPoints);
        if (res.Failed())
        {
          plLog::Warning("Skipping '{}': {}", prim.m_sPath, res.m_sMessage);
          continue;
        }

        // Tangent generation rebuilds the vertex list, merging equal vertices and splitting others, so every vertex
        // carries its index into geomSkin. That also keeps coincident vertices with different influences apart.
        plDeque<plGeometry::Vertex>& buildVertices = geom.GetVertices();
        for (plUInt32 v = 0; v < buildVertices.GetCount(); ++v)
          buildVertices[v].m_BoneIndices.Set((plUInt16)(v & 0xFFFF), (plUInt16)(v >> 16), 0, 0);
      }

      TransformGeometry(geom, plUsdSkelImport::GetSkinnedGeometryTransform(skelScene, (plUInt32)iSkinnedPrim, skeletalFrame));
    }
    else
    {
      TransformGeometry(geom, bPrimFrame ? mScale : state.m_GlobalTransform.GetAsMat4());
    }

    if (m_Options.m_bRecomputeNormals)
    {
      geom.ComputeFaceNormals();
      geom.ComputeSmoothVertexNormals();
    }

    // USD has no tangents to import, so they are always generated, the way the live link does.
    geom.ComputeTangents();

    {
      plModelImporter2::OutputNode& node = m_OutputNodes[state.m_iNode];
      for (const plGeometry::Vertex& vertex : geom.GetVertices())
        node.m_MeshBounds.ExpandToInclude(vertex.m_vPosition);
    }

    if (!bSelected)
      continue;

    addTriangles(geom, state.m_GroupMaterials, [&](plUInt32 g)
      { return state.m_GroupIncluded[g]; });
  }

  if (uiInstancesLeftOut > 0)
  {
    plLog::Warning("{} point instances are left out: the instances already make up {} triangles.", uiInstancesLeftOut, MaxInstancedTriangles);
  }

  // --- Mesh output: one sub-mesh per material that has faces, in material order. ---
  plUInt32 uiNumTriangles = 0;
  for (const plDynamicArray<plUInt32>& indices : materialIndices)
    uiNumTriangles += indices.GetCount() / 3;

  if (uiNumTriangles == 0)
  {
    if (bSkinned)
      plLog::Error("No Mesh or shape that Skeleton '{}' deforms is selected.", skelScene.GetSkeletons()[iSkeleton].m_sPath);

    return PL_SUCCESS;
  }

  if (uiTruncatedPoints > 0)
  {
    plLog::Info("{} points have more than four joint influences; each keeps its four strongest.", uiTruncatedPoints);
  }

  bool bHighPrecision = m_Options.m_bHighPrecision;
  if (!bHighPrecision)
  {
    for (const plDynamicArray<plUInt32>& indices : materialIndices)
    {
      if (HasUdimLayout(vertices, indices))
      {
        plLog::Info("The stage has a UDIM UV layout. Importing with high-precision vertex data.");
        bHighPrecision = true;
        break;
      }
    }
  }

  // Tiled textures: half-float texture coordinates lose texels past the first tile; see plUsdLiveResources.
  if (!bHighPrecision)
  {
    for (const plGeometry::Vertex& vertex : vertices)
    {
      if (plMath::Abs(vertex.m_vTexCoord.x) > 1.0f || plMath::Abs(vertex.m_vTexCoord.y) > 1.0f)
      {
        plLog::Info("The stage's texture coordinates reach beyond one tile. Importing with high-precision vertex data.");
        bHighPrecision = true;
        break;
      }
    }
  }

  plMeshResourceDescriptor& desc = *m_Options.m_pMeshOutput;
  plMeshBufferResourceDescriptor& buffer = desc.MeshBufferDesc();
  buffer.AddCommonStreams(bHighPrecision);
  if (bSkinned)
    buffer.AddStream(plMeshVertexStreamType::SkinningData);
  buffer.AllocateStreams(vertices.GetCount(), plGALPrimitiveTopology::Triangles, uiNumTriangles, true);

  for (plUInt32 v = 0; v < vertices.GetCount(); ++v)
  {
    const plGeometry::Vertex& vertex = vertices[v];
    buffer.SetPosition(v, vertex.m_vPosition);
    buffer.SetNormal(v, vertex.m_vNormal);
    buffer.SetTangent(v, vertex.m_vTangent.GetAsVec4(vertex.m_fBiTangentSign));
    buffer.SetTexCoord0(v, vertex.m_vTexCoord);
  }

  if (bSkinned)
  {
    plUsdSkelImport::WriteSkinning(skelScene, iSkeleton, skeletalFrame, skinVertices, desc);
  }

  plUInt32 uiTriangle = 0;
  plUInt32 uiSubMesh = 0;

  for (plUInt32 m = 0; m < materialIndices.GetCount(); ++m)
  {
    const plDynamicArray<plUInt32>& indices = materialIndices[m];
    if (indices.IsEmpty())
      continue;

    const plUInt32 uiFirstTriangle = uiTriangle;
    for (plUInt32 idx = 0; idx + 2 < indices.GetCount(); idx += 3)
    {
      buffer.SetTriangleIndices(uiTriangle, indices[idx], indices[idx + 1], indices[idx + 2]);
      ++uiTriangle;
    }

    m_OutputMaterials[m].m_iReferencedByMesh = (plInt32)uiSubMesh;
    desc.SetMaterial(uiSubMesh, m_OutputMaterials[m].m_sName);
    desc.AddSubMesh(uiTriangle - uiFirstTriangle, uiFirstTriangle, uiSubMesh);
    ++uiSubMesh;
  }

  desc.ComputeBounds();
  return PL_SUCCESS;
}

void plUsdModelImporter::AddPackagedTexture(plStringView sTexturePath)
{
  // A file inside a package resolves to "<package>[<file in package>]".
  const char* szOpen = sTexturePath.FindSubString("[");
  if (szOpen == nullptr || !sTexturePath.EndsWith("]"))
    return;

  const plString sKey = sTexturePath;
  if (m_OutputTextures.Find(sKey).IsValid())
    return;

  const plStringView sPackage(sTexturePath.GetStartPointer(), szOpen);
  const plStringView sEntry(szOpen + 1, sTexturePath.GetEndPointer() - 1);

  if (sEntry.FindSubString("[") != nullptr)
  {
    plLog::Warning("Texture '{}' is inside a package inside a package, which is not supported.", sTexturePath);
    return;
  }

  PackageFile* pPackage = nullptr;
  for (const plUniquePtr<PackageFile>& pExisting : m_Packages)
  {
    if (pExisting->m_sPath == sPackage)
    {
      pPackage = pExisting.Borrow();
      break;
    }
  }

  if (pPackage == nullptr)
  {
    plUniquePtr<PackageFile> pNew = PL_DEFAULT_NEW(PackageFile);
    pNew->m_sPath = sPackage;

    if (pNew->m_File.Open(sPackage, plMemoryMappedFile::Mode::ReadOnly).Succeeded() && plArchiveUtils::ExtractZipTOC(pNew->m_File, pNew->m_TOC).Succeeded())
    {
      pNew->m_bValid = true;
    }
    else
    {
      plLog::Warning("Package '{}' could not be read, its textures are not imported.", sPackage);
    }

    pPackage = pNew.Borrow();
    m_Packages.PushBack(std::move(pNew));
  }

  if (!pPackage->m_bValid)
    return;

  const plUInt32 uiEntry = pPackage->m_TOC.FindEntry(sEntry);
  if (uiEntry == plInvalidIndex)
  {
    plLog::Warning("Texture '{}' is not in package '{}'.", sEntry, sPackage);
    return;
  }

  const plArchiveEntry& entry = pPackage->m_TOC.m_Entries[uiEntry];
  const void* pArchiveData = pPackage->m_File.GetReadPointer();

  plConstByteArrayPtr data;
  if (entry.m_CompressionMode == plArchiveCompressionMode::Uncompressed)
  {
    data = plConstByteArrayPtr(static_cast<const plUInt8*>(pArchiveData) + entry.m_uiDataStartOffset, (plUInt32)entry.m_uiUncompressedDataSize);
  }
  else
  {
    plUniquePtr<plStreamReader> pReader = plArchiveUtils::CreateEntryReader(entry, pArchiveData);
    if (pReader == nullptr)
    {
      plLog::Warning("Texture '{}' in package '{}' uses a compression that cannot be read.", sEntry, sPackage);
      return;
    }

    plDynamicArray<plUInt8>& unpacked = m_UnpackedTextures.ExpandAndGetRef();
    unpacked.SetCountUninitialized((plUInt32)entry.m_uiUncompressedDataSize);
    unpacked.SetCount((plUInt32)pReader->ReadBytes(unpacked.GetData(), unpacked.GetCount()));
    data = unpacked.GetArrayPtr();
  }

  // Folders become part of the name, so equally named files from different folders stay apart once written out.
  plStringBuilder sName = sEntry;
  sName.ReplaceAll("/", "_");

  plStringBuilder sExtension = plPathUtils::GetFileExtension(sEntry);
  sExtension.ToLower();

  plModelImporter2::OutputTexture& texture = m_OutputTextures[sKey];
  texture.m_sFilename = sName;
  texture.m_sFileFormatExtension = sExtension;
  texture.m_RawData = data;
}
