#include <UsdBridge/UsdBridgePCH.h>

#include <Core/Graphics/Geometry.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/Map.h>
#include <Foundation/Containers/Set.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Time/Timestamp.h>
#include <UsdBridge/UsdRuntime.h>
#include <UsdBridge/UsdStage.h>
#include <UsdBridge/UsdStageAccess.h>

#pragma warning(push, 0)
#include <pxr/base/gf/quatf.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/changeList.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/notice.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/capsule.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/shadowAPI.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/utils.h>
#pragma warning(pop)

using plUsdText::FromStd;
using plUsdText::HashString;
using plUsdText::ToStd;

namespace
{
  plString NormalizedPath(plStringView sPath)
  {
    plStringBuilder s = sPath;
    s.MakeCleanPath();

    if (!s.IsEmpty() && !s.IsAbsolutePath())
    {
      s = plOSFile::MakePathAbsoluteWithCWD(s);
      s.MakeCleanPath();
    }

    return s;
  }

  bool SamePath(plStringView a, plStringView b)
  {
    return NormalizedPath(a).IsEqual_NoCase(NormalizedPath(b));
  }

  plString CollectErrors(pxr::TfErrorMark& ref_mark)
  {
    plStringBuilder sErrors;
    for (auto it = ref_mark.GetBegin(); it != ref_mark.GetEnd(); ++it)
    {
      if (!sErrors.IsEmpty())
        sErrors.Append("; ");
      sErrors.Append(it->GetCommentary().c_str());
    }
    ref_mark.Clear();
    return sErrors;
  }

  /// OpenUSD matrices are row-vector (p' = p * M); plMat4d is column-vector. One is the transpose of the other.
  plMat4d FromGf(const pxr::GfMatrix4d& gf)
  {
    plMat4d m;
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        m.Element(c, r) = gf[c][r];
    return m;
  }

  pxr::GfMatrix4d ToGf(const plMat4d& m)
  {
    pxr::GfMatrix4d gf;
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        gf[c][r] = m.Element(c, r);
    return gf;
  }

  template <typename T>
  bool GetValue(const pxr::UsdAttribute& attr, T& out_value)
  {
    if (!attr)
      return false;

    // Static reads: the default value when there is one, otherwise the first time sample.
    return attr.Get(&out_value, pxr::UsdTimeCode::EarliestTime());
  }

  plColor ToColor(const pxr::GfVec3f& c, float fAlpha = 1.0f)
  {
    return plColor(c[0], c[1], c[2], fAlpha);
  }

  /// Shader ids whose inputs we read, and the input names that carry each surface value.
  struct SurfaceInputNames
  {
    const char* m_szShaderId;
    const char* m_szBaseColor;
    const char* m_szBaseWeight;
    const char* m_szMetallic;
    const char* m_szRoughness;
    const char* m_szOpacity;
    const char* m_szOpacityThreshold;
    const char* m_szNormal;
  };

  constexpr SurfaceInputNames s_SurfaceShaders[] = {
    {"UsdPreviewSurface", "diffuseColor", nullptr, "metallic", "roughness", "opacity", "opacityThreshold", "normal"},
    {"ND_UsdPreviewSurface_surfaceshader", "diffuseColor", nullptr, "metallic", "roughness", "opacity", "opacityThreshold", "normal"},
    {"ND_open_pbr_surface_surfaceshader", "base_color", "base_weight", "base_metalness", "specular_roughness", "geometry_opacity", nullptr, "geometry_normal"},
    {"ND_standard_surface_surfaceshader", "base_color", "base", "metalness", "specular_roughness", "opacity", nullptr, "normal"},
    {"ND_gltf_pbr_surfaceshader", "base_color", nullptr, "metallic", "roughness", "alpha", "alpha_cutoff", "normal"},
  };

  /// Follows an input to whatever produces its value. Returns a texture file when a texture node feeds it, otherwise
  /// leaves the value in out_value.
  template <typename T>
  bool ReadShaderInput(const pxr::UsdShadeShader& shader, const char* szInput, T& out_value, plString* out_pTexture)
  {
    if (szInput == nullptr)
      return false;

    pxr::UsdShadeInput input = shader.GetInput(pxr::TfToken(szInput));
    if (!input)
      return false;

    for (const pxr::UsdAttribute& attr : input.GetValueProducingAttributes())
    {
      if (pxr::UsdShadeUtils::GetType(attr.GetName()) == pxr::UsdShadeAttributeType::Output)
      {
        // A node output: if it is a texture, report the file and sample its fallback value.
        pxr::UsdShadeShader source(attr.GetPrim());
        pxr::TfToken id;
        source.GetShaderId(&id);

        const std::string& sId = id.GetString();
        const bool bTexture = sId == "UsdUVTexture" || sId.rfind("ND_image_", 0) == 0 || sId.rfind("ND_tiledimage_", 0) == 0;

        if (bTexture && out_pTexture)
        {
          pxr::SdfAssetPath file;
          if (pxr::UsdShadeInput fileInput = source.GetInput(pxr::TfToken("file")); fileInput && fileInput.Get(&file))
          {
            const std::string& sResolved = file.GetResolvedPath().empty() ? file.GetAssetPath() : file.GetResolvedPath();
            *out_pTexture = NormalizedPath(FromStd(sResolved));
          }
        }

        return false;
      }

      return GetValue(attr, out_value);
    }

    return false;
  }
} // namespace

bool plUsdSurface::operator==(const plUsdSurface& rhs) const
{
  return m_sMaterialPath == rhs.m_sMaterialPath && m_BaseColor == rhs.m_BaseColor && m_fMetallic == rhs.m_fMetallic &&
         m_fRoughness == rhs.m_fRoughness && m_fOpacity == rhs.m_fOpacity && m_fOpacityThreshold == rhs.m_fOpacityThreshold &&
         m_sBaseColorTexture == rhs.m_sBaseColorTexture &&
         m_sNormalTexture == rhs.m_sNormalTexture && m_sRoughnessTexture == rhs.m_sRoughnessTexture &&
         m_sMetallicTexture == rhs.m_sMetallicTexture && m_sOpacityTexture == rhs.m_sOpacityTexture && m_bDoubleSided == rhs.m_bDoubleSided;
}

bool plUsdLight::operator==(const plUsdLight& rhs) const
{
  return m_Shape == rhs.m_Shape && m_Color == rhs.m_Color && m_bUseTemperature == rhs.m_bUseTemperature && m_fTemperature == rhs.m_fTemperature &&
         m_fLuminance == rhs.m_fLuminance && m_bNormalize == rhs.m_bNormalize && m_bUnrealUnits == rhs.m_bUnrealUnits && m_fRadius == rhs.m_fRadius &&
         m_fWidth == rhs.m_fWidth &&
         m_fHeight == rhs.m_fHeight && m_fLength == rhs.m_fLength && m_ConeAngle == rhs.m_ConeAngle && m_fConeSoftness == rhs.m_fConeSoftness &&
         m_fDiffuse == rhs.m_fDiffuse && m_fSpecular == rhs.m_fSpecular && m_bCastShadows == rhs.m_bCastShadows;
}

bool plUsdCamera::operator==(const plUsdCamera& rhs) const
{
  return m_bOrthographic == rhs.m_bOrthographic && m_fFocalLength == rhs.m_fFocalLength && m_fHorizontalAperture == rhs.m_fHorizontalAperture &&
         m_fVerticalAperture == rhs.m_fVerticalAperture && m_fOrthoWidth == rhs.m_fOrthoWidth && m_fNear == rhs.m_fNear && m_fFar == rhs.m_fFar &&
         m_fFStop == rhs.m_fFStop && m_fFocusDistance == rhs.m_fFocusDistance && m_fExposure == rhs.m_fExposure;
}

plUInt64 plUsdLight::ComputeHash(plUInt64 uiSeed) const
{
  plUInt64 uiHash = uiSeed;
  VisitFields(*this, [&](const auto& value)
    { uiHash = plHashingUtils::xxHash64(&value, sizeof(value), uiHash); });
  return uiHash;
}

plUInt64 plUsdCamera::ComputeHash(plUInt64 uiSeed) const
{
  plUInt64 uiHash = uiSeed;
  VisitFields(*this, [&](const auto& value)
    { uiHash = plHashingUtils::xxHash64(&value, sizeof(value), uiHash); });
  return uiHash;
}

namespace
{
  plUsdPrimKind GetPrimKind(const pxr::UsdPrim& prim)
  {
    if (!prim)
      return plUsdPrimKind::Group;
    if (prim.IsA<pxr::UsdGeomMesh>())
      return plUsdPrimKind::Mesh;
    if (prim.IsA<pxr::UsdGeomCube>() || prim.IsA<pxr::UsdGeomSphere>() || prim.IsA<pxr::UsdGeomCylinder>() || prim.IsA<pxr::UsdGeomCone>() || prim.IsA<pxr::UsdGeomCapsule>())
      return plUsdPrimKind::Gprim;
    if (prim.HasAPI<pxr::UsdLuxLightAPI>())
      return plUsdPrimKind::Light;
    if (prim.IsA<pxr::UsdGeomCamera>())
      return plUsdPrimKind::Camera;
    if (prim.IsA<pxr::UsdGeomPointInstancer>())
      return plUsdPrimKind::Instancer;
    return plUsdPrimKind::Group;
  }

  /// See plUsdComponent.
  constexpr plStringView ComponentSchemaPrefix = "PlasmaComponentAPI:";

  pxr::TfToken GetComponentSchema(plStringView sType)
  {
    return pxr::TfToken(ToStd(plStringBuilder(ComponentSchemaPrefix, sType)));
  }

  std::string GetComponentNamespace(plStringView sType)
  {
    return ToStd(plStringBuilder("plasma:", sType));
  }

  /// The component type a "PlasmaComponentAPI:<type>" schema name stands for; empty for other schemas.
  plStringView GetComponentType(const pxr::TfToken& schema)
  {
    const plStringView sSchema(schema.GetText());
    if (!sSchema.StartsWith(ComponentSchemaPrefix))
      return {};

    return plStringView(sSchema.GetStartPointer() + ComponentSchemaPrefix.GetElementCount(), sSchema.GetEndPointer());
  }

  bool ToUsdValue(const plVariant& value, pxr::VtValue& out_value, pxr::SdfValueTypeName& out_type)
  {
    const auto& names = pxr::SdfValueTypeNames;

    switch (value.GetType())
    {
      case plVariantType::Bool:
        out_value = pxr::VtValue(value.Get<bool>());
        out_type = names->Bool;
        return true;
      case plVariantType::Int8:
      case plVariantType::Int16:
      case plVariantType::Int32:
        out_value = pxr::VtValue(value.ConvertTo<plInt32>());
        out_type = names->Int;
        return true;
      case plVariantType::UInt8:
      case plVariantType::UInt16:
      case plVariantType::UInt32:
        out_value = pxr::VtValue(value.ConvertTo<plUInt32>());
        out_type = names->UInt;
        return true;
      case plVariantType::Int64:
        out_value = pxr::VtValue((int64_t)value.Get<plInt64>());
        out_type = names->Int64;
        return true;
      case plVariantType::UInt64:
        out_value = pxr::VtValue((uint64_t)value.Get<plUInt64>());
        out_type = names->UInt64;
        return true;
      case plVariantType::Float:
        out_value = pxr::VtValue(value.Get<float>());
        out_type = names->Float;
        return true;
      case plVariantType::Double:
        out_value = pxr::VtValue(value.Get<double>());
        out_type = names->Double;
        return true;
      case plVariantType::String:
      case plVariantType::StringView:
        out_value = pxr::VtValue(ToStd(value.ConvertTo<plString>()));
        out_type = names->String;
        return true;
      case plVariantType::Vector2:
      {
        const plVec2 v = value.Get<plVec2>();
        out_value = pxr::VtValue(pxr::GfVec2f(v.x, v.y));
        out_type = names->Float2;
        return true;
      }
      case plVariantType::Vector3:
      {
        const plVec3 v = value.Get<plVec3>();
        out_value = pxr::VtValue(pxr::GfVec3f(v.x, v.y, v.z));
        out_type = names->Float3;
        return true;
      }
      case plVariantType::Vector4:
      {
        const plVec4 v = value.Get<plVec4>();
        out_value = pxr::VtValue(pxr::GfVec4f(v.x, v.y, v.z, v.w));
        out_type = names->Float4;
        return true;
      }
      case plVariantType::Vector2I:
      {
        const plVec2I32 v = value.Get<plVec2I32>();
        out_value = pxr::VtValue(pxr::GfVec2i(v.x, v.y));
        out_type = names->Int2;
        return true;
      }
      case plVariantType::Vector3I:
      {
        const plVec3I32 v = value.Get<plVec3I32>();
        out_value = pxr::VtValue(pxr::GfVec3i(v.x, v.y, v.z));
        out_type = names->Int3;
        return true;
      }
      case plVariantType::Vector4I:
      {
        const plVec4I32 v = value.Get<plVec4I32>();
        out_value = pxr::VtValue(pxr::GfVec4i(v.x, v.y, v.z, v.w));
        out_type = names->Int4;
        return true;
      }
      case plVariantType::Color:
      {
        const plColor c = value.Get<plColor>();
        out_value = pxr::VtValue(pxr::GfVec4f(c.r, c.g, c.b, c.a));
        out_type = names->Color4f;
        return true;
      }
      case plVariantType::Quaternion:
      {
        const plQuat q = value.Get<plQuat>();
        out_value = pxr::VtValue(pxr::GfQuatf(q.w, q.x, q.y, q.z));
        out_type = names->Quatf;
        return true;
      }
      default:
        return false;
    }
  }

  /// The reverse of ToUsdValue; tokens read as strings. Values of other types fail.
  bool FromUsdValue(const pxr::VtValue& value, const pxr::SdfValueTypeName& type, plVariant& out_value)
  {
    const auto& names = pxr::SdfValueTypeNames;

    if (value.IsHolding<bool>())
      out_value = value.UncheckedGet<bool>();
    else if (value.IsHolding<int>())
      out_value = (plInt32)value.UncheckedGet<int>();
    else if (value.IsHolding<unsigned int>())
      out_value = (plUInt32)value.UncheckedGet<unsigned int>();
    else if (value.IsHolding<int64_t>())
      out_value = (plInt64)value.UncheckedGet<int64_t>();
    else if (value.IsHolding<uint64_t>())
      out_value = (plUInt64)value.UncheckedGet<uint64_t>();
    else if (value.IsHolding<float>())
      out_value = value.UncheckedGet<float>();
    else if (value.IsHolding<double>())
      out_value = value.UncheckedGet<double>();
    else if (value.IsHolding<std::string>())
      out_value = FromStd(value.UncheckedGet<std::string>());
    else if (value.IsHolding<pxr::TfToken>())
      out_value = FromStd(value.UncheckedGet<pxr::TfToken>().GetString());
    else if (value.IsHolding<pxr::GfVec2f>())
    {
      const pxr::GfVec2f& v = value.UncheckedGet<pxr::GfVec2f>();
      out_value = plVec2(v[0], v[1]);
    }
    else if (value.IsHolding<pxr::GfVec3f>())
    {
      const pxr::GfVec3f& v = value.UncheckedGet<pxr::GfVec3f>();
      out_value = plVec3(v[0], v[1], v[2]);
    }
    else if (value.IsHolding<pxr::GfVec4f>())
    {
      const pxr::GfVec4f& v = value.UncheckedGet<pxr::GfVec4f>();
      if (type == names->Color4f)
        out_value = plColor(v[0], v[1], v[2], v[3]);
      else
        out_value = plVec4(v[0], v[1], v[2], v[3]);
    }
    else if (value.IsHolding<pxr::GfVec2i>())
    {
      const pxr::GfVec2i& v = value.UncheckedGet<pxr::GfVec2i>();
      out_value = plVec2I32(v[0], v[1]);
    }
    else if (value.IsHolding<pxr::GfVec3i>())
    {
      const pxr::GfVec3i& v = value.UncheckedGet<pxr::GfVec3i>();
      out_value = plVec3I32(v[0], v[1], v[2]);
    }
    else if (value.IsHolding<pxr::GfVec4i>())
    {
      const pxr::GfVec4i& v = value.UncheckedGet<pxr::GfVec4i>();
      out_value = plVec4I32(v[0], v[1], v[2], v[3]);
    }
    else if (value.IsHolding<pxr::GfQuatf>())
    {
      const pxr::GfQuatf& q = value.UncheckedGet<pxr::GfQuatf>();
      out_value = plQuat(q.GetImaginary()[0], q.GetImaginary()[1], q.GetImaginary()[2], q.GetReal());
    }
    else
      return false;

    return true;
  }

  /// See plUsdComponent. Components are listed in the order the prim applies them; their values in name order.
  void ReadComponents(const pxr::UsdPrim& prim, plDynamicArray<plUsdComponent>& out_components)
  {
    out_components.Clear();

    for (const pxr::TfToken& schema : prim.GetPrimTypeInfo().GetAppliedAPISchemas())
    {
      const plStringView sType = GetComponentType(schema);
      if (sType.IsEmpty())
        continue;

      plUsdComponent& component = out_components.ExpandAndGetRef();
      component.m_sType = sType;

      const std::string sNamespace = GetComponentNamespace(sType);
      for (const pxr::UsdProperty& property : prim.GetAuthoredPropertiesInNamespace(sNamespace))
      {
        const pxr::UsdAttribute attribute = property.As<pxr::UsdAttribute>();
        pxr::VtValue value;
        if (!attribute || !attribute.Get(&value))
          continue;

        plUsdComponentValue entry;
        entry.m_sProperty = FromStd(property.GetName().GetString().substr(sNamespace.size() + 1));
        if (FromUsdValue(value, attribute.GetTypeName(), entry.m_Value))
          component.m_Values.PushBack(std::move(entry));
      }
    }
  }

  bool IsComponentProperty(const std::string& sName)
  {
    return sName.rfind("plasma:", 0) == 0;
  }

  /// Removes the edit layer's items of a prim's apiSchemas list for which bRemove says so, both applied and deleted ones.
  template <typename Predicate>
  void RemoveApiSchemaItems(const pxr::SdfPrimSpecHandle& spec, Predicate bRemove)
  {
    if (!spec->HasInfo(pxr::UsdTokens->apiSchemas))
      return;

    pxr::SdfTokenListOp listOp = spec->GetInfo(pxr::UsdTokens->apiSchemas).Get<pxr::SdfTokenListOp>();

    auto filter = [&](const pxr::SdfTokenListOp::ItemVector& items)
    {
      pxr::SdfTokenListOp::ItemVector kept;
      for (const pxr::TfToken& item : items)
      {
        if (!bRemove(item))
          kept.push_back(item);
      }
      return kept;
    };

    if (listOp.IsExplicit())
    {
      listOp.SetExplicitItems(filter(listOp.GetExplicitItems()));
    }
    else
    {
      listOp.SetPrependedItems(filter(listOp.GetPrependedItems()));
      listOp.SetAppendedItems(filter(listOp.GetAppendedItems()));
      listOp.SetDeletedItems(filter(listOp.GetDeletedItems()));
    }

    if (listOp.HasKeys() || listOp.IsExplicit())
      spec->SetInfo(pxr::UsdTokens->apiSchemas, pxr::VtValue(listOp));
    else
      spec->ClearInfo(pxr::UsdTokens->apiSchemas);
  }

  bool HasLookCorrection(plUsdPrimKind kind)
  {
    return kind == plUsdPrimKind::Light || kind == plUsdPrimKind::Camera;
  }

  plMat4d GetLookCorrection4(const plUsdBasis& basis)
  {
    return plMat4d(basis.GetLookCorrection(), plVec3d::MakeZero());
  }

  /// A prim's local matrix in Plasma's frame -> the matrix of its object, and back. See plUsdPrim::m_LocalTransform.
  plMat4d AddLookCorrection(const plUsdBasis& basis, const pxr::UsdPrim& prim, const plMat4d& mLocal)
  {
    const plMat4d mLook = GetLookCorrection4(basis);
    plMat4d m = mLocal;

    if (HasLookCorrection(GetPrimKind(prim)))
      m = m * mLook;
    if (HasLookCorrection(GetPrimKind(prim.GetParent())))
      m = mLook.GetTranspose() * m;

    return m;
  }

  plMat4d RemoveLookCorrection(const plUsdBasis& basis, const pxr::UsdPrim& prim, const plMat4d& mObject)
  {
    const plMat4d mLook = GetLookCorrection4(basis);
    plMat4d m = mObject;

    if (HasLookCorrection(GetPrimKind(prim)))
      m = m * mLook.GetTranspose();
    if (HasLookCorrection(GetPrimKind(prim.GetParent())))
      m = mLook * m;

    return m;
  }

  bool HasUnrealAttribute(const pxr::SdfLayerHandle& layer)
  {
    bool bFound = false;
    layer->Traverse(pxr::SdfPath::AbsoluteRootPath(), [&](const pxr::SdfPath& path)
      { bFound = bFound || (path.IsPropertyPath() && pxr::TfStringStartsWith(path.GetName(), "unreal")); });
    return bFound;
  }

  /// The stage's own layers first; a stage of lights alone carries the marks only in the meshes it references.
  bool IsUnrealStage(const pxr::UsdStageRefPtr& stage)
  {
    for (const pxr::SdfLayerHandle& layer : stage->GetLayerStack(false))
    {
      if (HasUnrealAttribute(layer))
        return true;
    }

    for (const pxr::SdfLayerHandle& layer : stage->GetUsedLayers(false))
    {
      if (HasUnrealAttribute(layer))
        return true;
    }

    return false;
  }

  void ReadLight(const pxr::UsdPrim& prim, double fMetersPerUnit, bool bUnrealAuthored, plUsdLight& out_light)
  {
    const float k = (float)fMetersPerUnit;
    const pxr::UsdLuxLightAPI light(prim);

    out_light.m_bUnrealUnits = bUnrealAuthored;

    float fIntensity = 1.0f;
    float fExposure = 0.0f;
    GetValue(light.GetIntensityAttr(), fIntensity);
    GetValue(light.GetExposureAttr(), fExposure);
    out_light.m_fLuminance = fIntensity * plMath::Pow(2.0f, fExposure);

    pxr::GfVec3f color(1.0f);
    if (GetValue(light.GetColorAttr(), color))
      out_light.m_Color = ToColor(color);

    GetValue(light.GetNormalizeAttr(), out_light.m_bNormalize);
    GetValue(light.GetEnableColorTemperatureAttr(), out_light.m_bUseTemperature);
    GetValue(light.GetColorTemperatureAttr(), out_light.m_fTemperature);
    GetValue(light.GetDiffuseAttr(), out_light.m_fDiffuse);
    GetValue(light.GetSpecularAttr(), out_light.m_fSpecular);

    // Shadows are on unless the ShadowAPI turns them off.
    if (prim.HasAPI<pxr::UsdLuxShadowAPI>())
      GetValue(pxr::UsdLuxShadowAPI(prim).GetShadowEnableAttr(), out_light.m_bCastShadows);

    float fValue = 0.0f;

    if (prim.IsA<pxr::UsdLuxSphereLight>())
    {
      out_light.m_Shape = plUsdLightShape::Sphere;
      if (GetValue(pxr::UsdLuxSphereLight(prim).GetRadiusAttr(), fValue))
        out_light.m_fRadius = fValue * k;

      if (prim.HasAPI<pxr::UsdLuxShapingAPI>())
      {
        const pxr::UsdLuxShapingAPI shaping(prim);
        if (GetValue(shaping.GetShapingConeAngleAttr(), fValue))
          out_light.m_ConeAngle = plAngle::MakeFromDegree(plMath::Clamp(fValue * 2.0f, 0.0f, 180.0f));
        GetValue(shaping.GetShapingConeSoftnessAttr(), out_light.m_fConeSoftness);
      }
    }
    else if (prim.IsA<pxr::UsdLuxDistantLight>())
    {
      out_light.m_Shape = plUsdLightShape::Distant;
    }
    else if (prim.IsA<pxr::UsdLuxRectLight>())
    {
      const pxr::UsdLuxRectLight rect(prim);
      out_light.m_Shape = plUsdLightShape::Rect;
      if (GetValue(rect.GetWidthAttr(), fValue))
        out_light.m_fWidth = fValue * k;
      if (GetValue(rect.GetHeightAttr(), fValue))
        out_light.m_fHeight = fValue * k;
    }
    else if (prim.IsA<pxr::UsdLuxDiskLight>())
    {
      out_light.m_Shape = plUsdLightShape::Disk;
      if (GetValue(pxr::UsdLuxDiskLight(prim).GetRadiusAttr(), fValue))
        out_light.m_fRadius = fValue * k;
    }
    else if (prim.IsA<pxr::UsdLuxCylinderLight>())
    {
      const pxr::UsdLuxCylinderLight cylinder(prim);
      out_light.m_Shape = plUsdLightShape::Cylinder;
      if (GetValue(cylinder.GetRadiusAttr(), fValue))
        out_light.m_fRadius = fValue * k;
      if (GetValue(cylinder.GetLengthAttr(), fValue))
        out_light.m_fLength = fValue * k;
    }
  }

  void ReadCamera(const pxr::UsdPrim& prim, double fMetersPerUnit, plUsdCamera& out_camera)
  {
    const float k = (float)fMetersPerUnit;
    const pxr::UsdGeomCamera camera(prim);

    pxr::TfToken projection;
    if (GetValue(camera.GetProjectionAttr(), projection))
      out_camera.m_bOrthographic = projection == pxr::UsdGeomTokens->orthographic;

    // Tenths of a scene unit -> millimetres.
    const float fToMillimetres = k * 100.0f;
    float fValue = 0.0f;

    if (GetValue(camera.GetFocalLengthAttr(), fValue))
      out_camera.m_fFocalLength = fValue * fToMillimetres;
    if (GetValue(camera.GetHorizontalApertureAttr(), fValue))
    {
      out_camera.m_fHorizontalAperture = fValue * fToMillimetres;
      out_camera.m_fOrthoWidth = fValue * 0.1f * k;
    }
    if (GetValue(camera.GetVerticalApertureAttr(), fValue))
      out_camera.m_fVerticalAperture = fValue * fToMillimetres;

    pxr::GfVec2f clipping;
    if (GetValue(camera.GetClippingRangeAttr(), clipping))
    {
      out_camera.m_fNear = clipping[0] * k;
      out_camera.m_fFar = clipping[1] * k;
    }

    GetValue(camera.GetFStopAttr(), out_camera.m_fFStop);
    if (GetValue(camera.GetFocusDistanceAttr(), fValue))
      out_camera.m_fFocusDistance = fValue * k;
    GetValue(camera.GetExposureAttr(), out_camera.m_fExposure);
  }
} // namespace

//////////////////////////////////////////////////////////////////////////

namespace
{
  /// Properties plUsdGeometrySource::Build reads, or that pick what it reads. GeomSubset properties belong to the mesh
  /// above the subset.
  bool IsGeometryProperty(const pxr::TfToken& name, bool& out_bOfSubset)
  {
    static const pxr::TfToken s_Mesh[] = {pxr::TfToken("points"), pxr::TfToken("faceVertexCounts"), pxr::TfToken("faceVertexIndices"),
      pxr::TfToken("orientation"), pxr::TfToken("normals"), pxr::TfToken("size"), pxr::TfToken("radius"), pxr::TfToken("height"), pxr::TfToken("axis")};
    static const pxr::TfToken s_Subset[] = {pxr::TfToken("indices"), pxr::TfToken("familyName"), pxr::TfToken("elementType")};

    out_bOfSubset = false;

    for (const pxr::TfToken& t : s_Mesh)
    {
      if (name == t)
        return true;
    }

    for (const pxr::TfToken& t : s_Subset)
    {
      if (name == t)
      {
        out_bOfSubset = true;
        return true;
      }
    }

    // Normals and UVs; display colour and opacity only tint the mesh component.
    const std::string& s = name.GetString();
    return s.rfind("primvars:", 0) == 0 && s.rfind("primvars:displayColor", 0) != 0 && s.rfind("primvars:displayOpacity", 0) != 0;
  }
} // namespace

class plUsdNoticeListener : public pxr::TfWeakBase
{
public:
  void OnObjectsChanged(const pxr::UsdNotice::ObjectsChanged& notice, const pxr::UsdStageWeakPtr& sender)
  {
    PL_LOCK(m_GeometryMutex);

    for (const pxr::SdfPath& path : notice.GetResyncedPaths())
    {
      m_Resynced.Insert(FromStd(path.GetString()));

      if (path.IsPrimPath() || path.IsAbsoluteRootPath())
        m_GeometryChanges.m_Resynced.Insert(FromStd(path.GetString()));
      else
        NoteGeometryProperty(path);
    }

    for (const pxr::SdfPath& path : notice.GetChangedInfoOnlyPaths())
    {
      m_ChangedInfo.Insert(FromStd(path.GetString()));
      NoteGeometryProperty(path);
    }
  }

  /// Every layer change, of any stage's layers. Edits of the specs remembered by Remember mark their prims edited.
  void OnLayersChanged(const pxr::SdfNotice::LayersDidChange& notice)
  {
    PL_LOCK(m_GeometryMutex);

    if (m_SpecPrims.empty())
      return;

    for (const auto& layerChanges : notice.GetChangeListVec())
    {
      const auto itLayer = m_SpecPrims.find(layerChanges.first->GetIdentifier());
      if (itLayer == m_SpecPrims.end())
        continue;

      const SpecToPrims& specs = itLayer->second;

      for (const auto& entry : layerChanges.second.GetEntryList())
      {
        const pxr::SdfPath& path = entry.first;

        if (path.IsAbsoluteRootPath())
        {
          // Layer metadata, e.g. its sublayers, unless the whole content was replaced, as a reload may do.
          if (entry.second.flags.didReloadContent || entry.second.flags.didReplaceContent)
          {
            for (const auto& it : specs)
              MarkEdited(it.second);
          }
        }
        else if (path.IsPropertyPath())
        {
          if (const auto it = specs.find(path); it != specs.end())
            MarkEdited(it->second);
        }
        else if (path.IsPrimPath() || path.IsPrimVariantSelectionPath())
        {
          // A prim spec added, removed or renamed takes its property specs with it.
          for (const auto& it : specs)
          {
            if (it.first.HasPrefix(path))
              MarkEdited(it.second);
          }
        }
      }
    }
  }

  /// Remembers which specs a prim's geometry is composed from, replacing what was remembered for it before.
  void Remember(const pxr::SdfPath& prim, std::vector<std::pair<std::string, pxr::SdfPath>>&& specs)
  {
    PL_LOCK(m_GeometryMutex);

    if (const auto it = m_PrimSpecs.find(prim); it != m_PrimSpecs.end())
    {
      for (const auto& spec : it->second)
      {
        if (const auto itLayer = m_SpecPrims.find(spec.first); itLayer != m_SpecPrims.end())
        {
          if (const auto itSpec = itLayer->second.find(spec.second); itSpec != itLayer->second.end())
          {
            itSpec->second.erase(prim);
            if (itSpec->second.empty())
              itLayer->second.erase(itSpec);
          }
        }
      }
    }

    for (const auto& spec : specs)
      m_SpecPrims[spec.first][spec.second].insert(prim);

    m_PrimSpecs[prim] = std::move(specs);
  }

  void ConsumeGeometryChanges(plUsdGeometryChanges& out_changes)
  {
    PL_LOCK(m_GeometryMutex);
    out_changes.m_Edited.Swap(m_GeometryChanges.m_Edited);
    out_changes.m_Resynced.Swap(m_GeometryChanges.m_Resynced);
    m_GeometryChanges.m_Edited.Clear();
    m_GeometryChanges.m_Resynced.Clear();
  }

  void ClearGeometry()
  {
    PL_LOCK(m_GeometryMutex);
    m_SpecPrims.clear();
    m_PrimSpecs.clear();
    m_GeometryChanges.m_Edited.Clear();
    m_GeometryChanges.m_Resynced.Clear();
  }

  plSet<plString> m_Resynced;
  plSet<plString> m_ChangedInfo;

private:
  using SpecToPrims = std::map<pxr::SdfPath, std::set<pxr::SdfPath>>;

  void NoteGeometryProperty(const pxr::SdfPath& path)
  {
    if (!path.IsPropertyPath())
      return;

    bool bOfSubset = false;
    if (!IsGeometryProperty(path.GetNameToken(), bOfSubset))
      return;

    const pxr::SdfPath prim = path.GetPrimPath();
    m_GeometryChanges.m_Edited.Insert(FromStd((bOfSubset ? prim.GetParentPath() : prim).GetString()));
  }

  void MarkEdited(const std::set<pxr::SdfPath>& prims)
  {
    for (const pxr::SdfPath& prim : prims)
      m_GeometryChanges.m_Edited.Insert(FromStd(prim.GetString()));
  }

  plMutex m_GeometryMutex; ///< Layer notices arrive on whichever thread changed a layer, which may serve another stage.
  plUsdGeometryChanges m_GeometryChanges;
  std::map<std::string, SpecToPrims> m_SpecPrims; ///< Layer identifier -> geometry spec -> prims composed from it.
  std::unordered_map<pxr::SdfPath, std::vector<std::pair<std::string, pxr::SdfPath>>, pxr::SdfPath::Hash> m_PrimSpecs;
};

struct plUsdStage::Impl
{
  pxr::UsdStageRefPtr m_Stage;
  plString m_sRootLayerFile;
  plUsdBasis m_Basis;
  double m_fStartTime = 0.0;
  bool m_bUnrealAuthored = false;

  pxr::SdfLayerHandle m_EditLayer; // The stage keeps its layer stack alive.
  plString m_sEditLayer;

  plUsdNoticeListener m_Listener;
  pxr::TfNotice::Key m_NoticeKey;
  pxr::TfNotice::Key m_LayersNoticeKey;

  /// Last seen modification time per layer file, to reload only what changed on disk.
  plMap<plString, plTimestamp> m_LayerTimes;

  pxr::UsdPrim GetPrim(plStringView sPath) const
  {
    if (!m_Stage || sPath.IsEmpty())
      return {};

    return m_Stage->GetPrimAtPath(pxr::SdfPath(ToStd(sPath)));
  }

  void RememberLayerTimes()
  {
    m_LayerTimes.Clear();

    for (const pxr::SdfLayerHandle& layer : m_Stage->GetUsedLayers())
    {
      const std::string& sReal = layer->GetRealPath();
      if (sReal.empty())
        continue;

      plFileStats stats;
      if (plOSFile::GetFileStats(sReal.c_str(), stats).Succeeded())
      {
        m_LayerTimes[NormalizedPath(sReal.c_str())] = stats.m_LastModificationTime;
      }
    }
  }

  bool ReadSurface(const pxr::UsdPrim& prim, plUsdSurface& out_surface) const;
  bool ReadMaterialSurface(const pxr::UsdShadeMaterial& material, plUsdSurface& out_surface) const;
  bool ReadPrimDesc(const pxr::UsdPrim& prim, plUsdPrim& out_prim) const;
  void ReadInstancer(const pxr::UsdPrim& prim, plUsdInstancer& out_instancer) const;
};

bool plUsdStage::Impl::ReadMaterialSurface(const pxr::UsdShadeMaterial& material, plUsdSurface& out_surface) const
{
  // "mtlx" first: every Material declares the universal output, and OpenUSD stops at it even when nothing connects it,
  // which would hide a network authored only on outputs:mtlx:surface.
  const pxr::TfTokenVector contexts = {pxr::TfToken("mtlx"), pxr::UsdShadeTokens->universalRenderContext};
  pxr::UsdShadeShader surface = material.ComputeSurfaceSource(contexts);
  if (!surface)
    return false;

  pxr::TfToken id;
  surface.GetShaderId(&id);

  for (const SurfaceInputNames& names : s_SurfaceShaders)
  {
    if (id != names.m_szShaderId)
      continue;

    out_surface.m_sMaterialPath = FromStd(material.GetPath().GetString());

    pxr::GfVec3f baseColor(1.0f);
    ReadShaderInput(surface, names.m_szBaseColor, baseColor, &out_surface.m_sBaseColorTexture);

    float fWeight = 1.0f;
    ReadShaderInput(surface, names.m_szBaseWeight, fWeight, nullptr);
    out_surface.m_BaseColor = ToColor(baseColor * fWeight);

    ReadShaderInput(surface, names.m_szMetallic, out_surface.m_fMetallic, &out_surface.m_sMetallicTexture);
    ReadShaderInput(surface, names.m_szRoughness, out_surface.m_fRoughness, &out_surface.m_sRoughnessTexture);

    // Standard surface's opacity is a colour; everything else uses a float. A texture feeding it is a mask.
    float fOpacity = 1.0f;
    if (!ReadShaderInput(surface, names.m_szOpacity, fOpacity, &out_surface.m_sOpacityTexture))
    {
      pxr::GfVec3f opacity(1.0f);
      if (ReadShaderInput(surface, names.m_szOpacity, opacity, &out_surface.m_sOpacityTexture))
        fOpacity = opacity[0];
    }
    out_surface.m_fOpacity = fOpacity;

    ReadShaderInput(surface, names.m_szOpacityThreshold, out_surface.m_fOpacityThreshold, nullptr);

    pxr::GfVec3f unusedNormal;
    ReadShaderInput(surface, names.m_szNormal, unusedNormal, &out_surface.m_sNormalTexture);
    return true;
  }

  return false;
}

bool plUsdStage::Impl::ReadSurface(const pxr::UsdPrim& prim, plUsdSurface& out_surface) const
{
  bool bFound = false;

  if (pxr::UsdGeomGprim gprim(prim); gprim)
  {
    GetValue(gprim.GetDoubleSidedAttr(), out_surface.m_bDoubleSided);
  }

  pxr::UsdShadeMaterial material = pxr::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
  if (material)
  {
    bFound = ReadMaterialSurface(material, out_surface);
  }

  if (!bFound)
  {
    // No material Plasma understands: fall back to displayColor, which every DCC writes.
    pxr::UsdGeomGprim gprim(prim);
    if (!gprim)
      return false;

    pxr::VtVec3fArray colors;
    if (pxr::UsdGeomPrimvar pv = gprim.GetDisplayColorPrimvar(); pv && pv.ComputeFlattened(&colors, pxr::UsdTimeCode::EarliestTime()) && !colors.empty())
    {
      out_surface.m_BaseColor = ToColor(colors[0]);
      bFound = true;
    }

    pxr::VtFloatArray opacities;
    if (pxr::UsdGeomPrimvar pv = gprim.GetDisplayOpacityPrimvar(); pv && pv.ComputeFlattened(&opacities, pxr::UsdTimeCode::EarliestTime()) && !opacities.empty())
    {
      out_surface.m_fOpacity = opacities[0];
      bFound = true;
    }
  }

  return bFound;
}

//////////////////////////////////////////////////////////////////////////

plUsdStage::plUsdStage()
  : m_pImpl(PL_DEFAULT_NEW(Impl))
{
}

plUsdStage::~plUsdStage()
{
  Close();
}

plStatus plUsdStage::Open(plStringView sRootLayerFile, Load load)
{
  Close();

  const plString sFile = NormalizedPath(sRootLayerFile);
  if (!plOSFile::ExistsFile(sFile))
    return plStatus(plFmt("USD file not found: '{}'", sFile));

  // Other tools save over the root layer, e.g. when a session is committed. Without the drive, so the pattern matches
  // however the identifier spells it.
  {
    plStringView sPattern = sFile;
    if (const char* szColon = sPattern.FindSubString(":"))
      sPattern.SetStartPosition(szColon + 1);
    plUsdRuntime::DetachLayers(sPattern);

    // A session root, <folder>/.live/<asset name>/<session>/session.usda, has the asset as its weakest sublayer.
    if (const char* szLive = sPattern.FindLastSubString("/.live/"))
    {
      plStringView sAssetName(szLive + 7, sPattern.GetEndPointer());
      if (const char* szSlash = sAssetName.FindSubString("/"))
      {
        plStringBuilder sAsset(plStringView(sPattern.GetStartPointer(), szLive), "/", plStringView(sAssetName.GetStartPointer(), szSlash), ".");
        plUsdRuntime::DetachLayers(sAsset);
      }
    }
  }

  pxr::TfErrorMark mark;
  pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(ToStd(sFile), load == Load::None ? pxr::UsdStage::LoadNone : pxr::UsdStage::LoadAll);

  if (!stage)
  {
    const plString sErrors = CollectErrors(mark);
    return plStatus(plFmt("OpenUSD could not open '{}': {}", sFile, sErrors.IsEmpty() ? plStringView("unknown error") : sErrors.GetView()));
  }

  if (!mark.IsClean())
  {
    plLog::Warning("OpenUSD reported problems opening '{}': {}", sFile, CollectErrors(mark));
  }

  Impl& impl = *m_pImpl;
  impl.m_Stage = stage;
  impl.m_sRootLayerFile = sFile;
  impl.m_fStartTime = stage->GetStartTimeCode();

  const plUsdBasis::UpAxis upAxis = pxr::UsdGeomGetStageUpAxis(stage) == pxr::UsdGeomTokens->z ? plUsdBasis::UpAxis::Z : plUsdBasis::UpAxis::Y;
  impl.m_Basis = plUsdBasis(upAxis, pxr::UsdGeomGetStageMetersPerUnit(stage));
  impl.m_bUnrealAuthored = IsUnrealStage(stage);

  impl.m_NoticeKey = pxr::TfNotice::Register(pxr::TfCreateWeakPtr(&impl.m_Listener), &plUsdNoticeListener::OnObjectsChanged, pxr::UsdStageWeakPtr(stage));
  impl.m_LayersNoticeKey = pxr::TfNotice::Register(pxr::TfCreateWeakPtr(&impl.m_Listener), &plUsdNoticeListener::OnLayersChanged);

  impl.RememberLayerTimes();
  return plStatus(PL_SUCCESS);
}

void plUsdStage::Close()
{
  Impl& impl = *m_pImpl;

  if (impl.m_NoticeKey.IsValid())
  {
    pxr::TfNotice::Revoke(impl.m_NoticeKey);
  }

  if (impl.m_LayersNoticeKey.IsValid())
  {
    pxr::TfNotice::Revoke(impl.m_LayersNoticeKey);
  }

  impl.m_EditLayer = {};
  impl.m_sEditLayer.Clear();
  impl.m_Stage = {};
  impl.m_sRootLayerFile.Clear();
  impl.m_bUnrealAuthored = false;
  impl.m_LayerTimes.Clear();
  impl.m_Listener.m_Resynced.Clear();
  impl.m_Listener.m_ChangedInfo.Clear();
  impl.m_Listener.ClearGeometry();
}

bool plUsdStage::IsOpen() const
{
  return m_pImpl->m_Stage != nullptr;
}

plStringView plUsdStage::GetRootLayerFile() const
{
  return m_pImpl->m_sRootLayerFile;
}

const plUsdBasis& plUsdStage::GetBasis() const
{
  return m_pImpl->m_Basis;
}

double plUsdStage::GetStartTimeCode() const
{
  return m_pImpl->m_fStartTime;
}

bool plUsdStage::IsUnrealAuthored() const
{
  return m_pImpl->m_bUnrealAuthored;
}

void plUsdStage::ReadPrims(plDynamicArray<plUsdPrim>& out_prims) const
{
  out_prims.Clear();

  const Impl& impl = *m_pImpl;
  if (!impl.m_Stage)
    return;

  plMap<plString, plInt32> pathToIndex;

  // The default traversal skips inactive prims. They are kept, so a prim Plasma's layer deactivated stays visible in
  // the scene tree and can be reactivated from there; their children are not composed, so they arrive without any.
  // An inactive prim also reports itself as not loaded, so "loaded" is not part of the predicate either; the stage is
  // opened with every payload loaded.
  pxr::UsdPrimRange range = pxr::UsdPrimRange::Stage(impl.m_Stage, pxr::UsdPrimIsDefined && !pxr::UsdPrimIsAbstract);
  for (auto it = range.begin(); it != range.end(); ++it)
  {
    const pxr::UsdPrim& prim = *it;

    // Materials, shaders, subsets and the like have no place in the scene tree, and neither do their children.
    if (!prim.IsA<pxr::UsdGeomImageable>())
    {
      it.PruneChildren();
      continue;
    }

    plUsdPrim desc;
    if (!impl.ReadPrimDesc(prim, desc))
    {
      it.PruneChildren();
      continue;
    }

    const std::string sParent = prim.GetParent().GetPath().GetString();
    if (auto itParent = pathToIndex.Find(FromStd(sParent)); itParent.IsValid())
    {
      desc.m_iParent = itParent.Value();
    }

    pathToIndex[desc.m_sPath] = (plInt32)out_prims.GetCount();
    out_prims.PushBack(std::move(desc));
  }
}

plResult plUsdStage::ReadPrim(plStringView sPrimPath, plUsdPrim& out_prim) const
{
  pxr::UsdPrim prim = m_pImpl->GetPrim(sPrimPath);
  if (!prim || !prim.IsA<pxr::UsdGeomImageable>())
    return PL_FAILURE;

  return m_pImpl->ReadPrimDesc(prim, out_prim) ? PL_SUCCESS : PL_FAILURE;
}

plResult plUsdStage::ReadMaterial(plStringView sMaterialPath, plUsdSurface& out_surface) const
{
  out_surface = plUsdSurface();

  pxr::UsdShadeMaterial material(m_pImpl->GetPrim(sMaterialPath));
  if (!material)
    return PL_FAILURE;

  return m_pImpl->ReadMaterialSurface(material, out_surface) ? PL_SUCCESS : PL_FAILURE;
}

void plUsdStage::GetVariantSets(plStringView sPrimPath, plDynamicArray<plUsdVariantSet>& out_sets) const
{
  out_sets.Clear();

  pxr::UsdPrim prim = m_pImpl->GetPrim(sPrimPath);
  if (!prim || !prim.HasVariantSets())
    return;

  pxr::UsdVariantSets sets = prim.GetVariantSets();
  for (const std::string& sName : sets.GetNames())
  {
    pxr::UsdVariantSet set = sets.GetVariantSet(sName);

    plUsdVariantSet& out = out_sets.ExpandAndGetRef();
    out.m_sName = FromStd(sName);
    out.m_sSelection = FromStd(set.GetVariantSelection());
    for (const std::string& sVariant : set.GetVariantNames())
      out.m_Variants.PushBack(FromStd(sVariant));
  }
}

bool plUsdStage::Impl::ReadPrimDesc(const pxr::UsdPrim& prim, plUsdPrim& out_prim) const
{
  const Impl& impl = *this;

  out_prim.m_sPath = FromStd(prim.GetPath().GetString());
  out_prim.m_sName = FromStd(prim.GetName().GetString());
  out_prim.m_sTypeName = FromStd(prim.GetTypeName().GetString());
  out_prim.m_bActive = prim.IsActive();
  out_prim.m_Kind = GetPrimKind(prim);

  if (pxr::UsdGeomXformable xformable(prim); xformable)
  {
    pxr::GfMatrix4d mLocal(1.0);
    bool bResetsXformStack = false;
    xformable.GetLocalTransformation(&mLocal, &bResetsXformStack, pxr::UsdTimeCode::EarliestTime());

    out_prim.m_LocalTransform = plUsdBasis::MatrixToTransform(AddLookCorrection(impl.m_Basis, prim, impl.m_Basis.MatrixToPlasma(FromGf(mLocal))));
    out_prim.m_bTransformAnimated = xformable.TransformMightBeTimeVarying();
  }

  if (out_prim.m_Kind == plUsdPrimKind::Light)
    ReadLight(prim, impl.m_Basis.GetMetersPerUnit(), impl.m_bUnrealAuthored, out_prim.m_Light);
  else if (out_prim.m_Kind == plUsdPrimKind::Camera)
    ReadCamera(prim, impl.m_Basis.GetMetersPerUnit(), out_prim.m_Camera);
  else if (out_prim.m_Kind == plUsdPrimKind::Instancer && out_prim.m_bActive)
    impl.ReadInstancer(prim, out_prim.m_Instancer);

  if (out_prim.m_bActive)
    ReadComponents(prim, out_prim.m_Components);

  for (pxr::UsdPrim parent = prim.GetParent(); parent && !parent.IsPseudoRoot(); parent = parent.GetParent())
  {
    if (parent.IsA<pxr::UsdGeomPointInstancer>())
    {
      out_prim.m_bPrototype = true;
      break;
    }
  }

  pxr::TfToken visibility;
  if (GetValue(pxr::UsdGeomImageable(prim).GetVisibilityAttr(), visibility))
  {
    out_prim.m_bVisible = visibility != pxr::UsdGeomTokens->invisible;
  }

  if (out_prim.m_Kind == plUsdPrimKind::Mesh || out_prim.m_Kind == plUsdPrimKind::Gprim)
  {
    plUsdMaterialGroup& meshGroup = out_prim.m_MaterialGroups.ExpandAndGetRef();
    meshGroup.m_bHasSurface = impl.ReadSurface(prim, meshGroup.m_Surface);

    if (pxr::UsdGeomMesh mesh(prim); mesh)
    {
      // Double-sidedness belongs to the mesh; the subsets' materials inherit it.
      for (const pxr::UsdGeomSubset& subset : pxr::UsdShadeMaterialBindingAPI(prim).GetMaterialBindSubsets())
      {
        plUsdMaterialGroup& group = out_prim.m_MaterialGroups.ExpandAndGetRef();
        group.m_sSubset = FromStd(subset.GetPrim().GetName().GetString());
        group.m_bHasSurface = impl.ReadSurface(subset.GetPrim(), group.m_Surface);
        group.m_Surface.m_bDoubleSided = meshGroup.m_Surface.m_bDoubleSided;
      }
    }
  }

  return true;
}

void plUsdStage::Impl::ReadInstancer(const pxr::UsdPrim& prim, plUsdInstancer& out_instancer) const
{
  out_instancer = {};

  const pxr::UsdGeomPointInstancer instancer(prim);
  const pxr::UsdTimeCode time(m_fStartTime);

  pxr::SdfPathVector prototypes;
  instancer.GetPrototypesRel().GetForwardedTargets(&prototypes);

  // The prototypes' own transforms are included: OpenUSD applies each prototype's local transform under its instance's.
  pxr::VtIntArray protoIndices;
  pxr::VtArray<pxr::GfMatrix4d> instances;
  if (prototypes.empty() || !instancer.GetProtoIndicesAttr().Get(&protoIndices, time) ||
      !instancer.ComputeInstanceTransformsAtTime(&instances, time, time, pxr::UsdGeomPointInstancer::IncludeProtoXform, pxr::UsdGeomPointInstancer::IgnoreMask) ||
      instances.size() != protoIndices.size())
    return;

  // Empty when every instance is shown.
  const std::vector<bool> mask = instancer.ComputeMaskAtTime(time);

  plUInt64 uiHash = 0;
  pxr::UsdGeomXformCache xformCache(time);

  for (size_t uiProto = 0; uiProto < prototypes.size(); ++uiProto)
  {
    const pxr::UsdPrim proto = m_Stage->GetPrimAtPath(prototypes[uiProto]);
    if (!proto || !proto.IsActive())
      continue;

    plHybridArray<plUInt32, 64> used;
    for (size_t i = 0; i < protoIndices.size(); ++i)
    {
      if (protoIndices[i] == (int)uiProto && (mask.empty() || mask[i]))
        used.PushBack((plUInt32)i);
    }

    if (used.IsEmpty())
      continue;

    out_instancer.m_uiInstanceCount += used.GetCount();

    pxr::UsdPrimRange range(proto);
    for (auto it = range.begin(); it != range.end(); ++it)
    {
      const pxr::UsdPrim& part = *it;

      pxr::TfToken visibility;
      if (!part.IsA<pxr::UsdGeomImageable>() || part.IsA<pxr::UsdGeomPointInstancer>() ||
          (GetValue(pxr::UsdGeomImageable(part).GetVisibilityAttr(), visibility) && visibility == pxr::UsdGeomTokens->invisible))
      {
        it.PruneChildren();
        continue;
      }

      const plUsdPrimKind kind = GetPrimKind(part);
      if (kind != plUsdPrimKind::Mesh && kind != plUsdPrimKind::Gprim)
        continue;

      // Everything between the prototype and the mesh; the prototype's own transform is in the instance transforms.
      bool bResetsXformStack = false;
      const pxr::GfMatrix4d mInPrototype = part == proto ? pxr::GfMatrix4d(1.0) : xformCache.ComputeRelativeTransform(part, proto, &bResetsXformStack);

      plUsdInstancedMesh& mesh = out_instancer.m_Meshes.ExpandAndGetRef();
      mesh.m_sMeshPath = FromStd(part.GetPath().GetString());
      mesh.m_Instances.Reserve(used.GetCount());

      // USD's matrices apply left to right: the mesh's place in the prototype, then the instance's.
      for (plUInt32 i : used)
      {
        const plTransform t = plUsdBasis::MatrixToTransform(m_Basis.MatrixToPlasma(FromGf(mInPrototype * instances[i])));
        mesh.m_Instances.PushBack(t);
      }

      uiHash = plHashingUtils::xxHash64String(mesh.m_sMeshPath, uiHash);
      uiHash = plHashingUtils::xxHash64(mesh.m_Instances.GetData(), mesh.m_Instances.GetCount() * sizeof(plTransform), uiHash);
    }
  }

  out_instancer.m_uiHash = uiHash;
}

//////////////////////////////////////////////////////////////////////////
// Geometry

namespace
{
  /// Picks the primvar that holds the primary UV set. DCCs disagree on its name.
  pxr::UsdGeomPrimvar FindUvPrimvar(const pxr::UsdPrim& prim)
  {
    pxr::UsdGeomPrimvarsAPI primvars(prim);

    for (const char* szName : {"st", "st0", "UVMap", "uv", "map1"})
    {
      pxr::UsdGeomPrimvar pv = primvars.GetPrimvar(pxr::TfToken(szName));
      if (pv && pv.HasValue())
        return pv;
    }

    for (const pxr::UsdGeomPrimvar& pv : primvars.GetPrimvarsWithValues())
    {
      if (pv.GetTypeName().GetRole() == pxr::SdfValueRoleNames->TextureCoordinate)
        return pv;
    }

    return {};
  }

  /// Hashes the layer specs a Mesh's or Gprim's geometry attributes are composed from, specs not values: prims composed
  /// from the same specs have the same geometry. Calls onSpec(spec) for each spec in hashing order.
  template <typename OnSpec>
  plUInt64 HashGeometrySpecsImpl(const pxr::UsdPrim& prim, plUInt64 uiSeed, OnSpec onSpec)
  {
    plUInt64 uiHash = HashString(prim.GetTypeName().GetString(), uiSeed);

    auto addAttribute = [&](const pxr::UsdAttribute& attr)
    {
      if (!attr)
        return;

      uiHash = HashString(attr.GetName().GetString(), uiHash);
      for (const pxr::SdfPropertySpecHandle& spec : attr.GetPropertyStack(pxr::UsdTimeCode::EarliestTime()))
      {
        uiHash = HashString(spec->GetLayer()->GetIdentifier(), uiHash);
        uiHash = HashString(spec->GetPath().GetString(), uiHash);
        onSpec(spec);
      }
    };

    if (const pxr::UsdGeomMesh mesh(prim); mesh)
    {
      addAttribute(mesh.GetPointsAttr());
      addAttribute(mesh.GetFaceVertexCountsAttr());
      addAttribute(mesh.GetFaceVertexIndicesAttr());
      addAttribute(mesh.GetOrientationAttr());
      addAttribute(mesh.GetNormalsAttr());

      if (pxr::UsdGeomPrimvar normalsPv = pxr::UsdGeomPrimvarsAPI(prim).GetPrimvar(pxr::TfToken("normals")))
      {
        addAttribute(normalsPv.GetAttr());
        addAttribute(normalsPv.GetIndicesAttr());
      }

      if (pxr::UsdGeomPrimvar uvPv = FindUvPrimvar(prim))
      {
        addAttribute(uvPv.GetAttr());
        addAttribute(uvPv.GetIndicesAttr());
      }

      for (const pxr::UsdGeomSubset& subset : pxr::UsdShadeMaterialBindingAPI(prim).GetMaterialBindSubsets())
      {
        uiHash = HashString(subset.GetPrim().GetName().GetString(), uiHash);
        addAttribute(subset.GetIndicesAttr());
      }
    }
    else
    {
      for (const char* szName : {"size", "radius", "height", "axis"})
        addAttribute(prim.GetAttribute(pxr::TfToken(szName)));
    }

    return uiHash;
  }

  /// Picks an element of a primvar or normal array by its interpolation, through the primvar's indices when it has any.
  template <typename T>
  const T* PickElement(const pxr::VtArray<T>& values, const pxr::VtIntArray& indices, const pxr::TfToken& interpolation, size_t uiFace, size_t uiCorner, int iPoint)
  {
    size_t uiIndex = 0;

    if (interpolation == pxr::UsdGeomTokens->faceVarying)
      uiIndex = uiCorner;
    else if (interpolation == pxr::UsdGeomTokens->uniform)
      uiIndex = uiFace;
    else if (interpolation == pxr::UsdGeomTokens->constant)
      uiIndex = 0;
    else // vertex, varying
      uiIndex = (size_t)iPoint;

    if (!indices.empty())
    {
      if (uiIndex >= indices.size() || indices[uiIndex] < 0)
        return nullptr;

      uiIndex = (size_t)indices[uiIndex];
    }

    return uiIndex < values.size() ? &values[uiIndex] : nullptr;
  }

  template <typename T>
  plUInt64 HashArray(const pxr::VtArray<T>& values, plUInt64 uiSeed)
  {
    return plHashingUtils::xxHash64(values.cdata(), values.size() * sizeof(T), uiSeed);
  }

  /// The arrays are VtArrays: copying one only takes a reference, and a later edit or reload of the layer creates new
  /// storage rather than touching this one. Indexed primvars are kept as values and indices; Build resolves them, so
  /// capturing on the stage's thread copies nothing.
  class plUsdMeshSource final : public plUsdGeometrySource
  {
  public:
    plStatus Build(plGeometry& out_geometry, plDynamicArray<plUInt32>* out_pGroupTriangleCounts) const override;
    plUInt64 GetHash() const override;
    plUInt64 GetPointCount() const override { return m_Points.size(); }

    plUsdBasis m_Basis;
    pxr::VtVec3fArray m_Points;
    pxr::VtIntArray m_FaceCounts;
    pxr::VtIntArray m_FaceIndices;
    std::vector<pxr::VtIntArray> m_SubsetFaces; ///< Face indices of each materialBind subset, in plUsdMaterialGroup order.
    pxr::VtVec3fArray m_Normals;
    pxr::VtIntArray m_NormalIndices;
    pxr::TfToken m_NormalsInterpolation;
    pxr::VtVec2fArray m_Uvs;
    pxr::VtIntArray m_UvIndices;
    pxr::TfToken m_UvInterpolation;
    bool m_bLeftHanded = false;

  private:
    mutable plUInt64 m_uiHash = 0; ///< Computed on first use: hashing a large mesh reads all of it.
  };

  plUInt64 plUsdMeshSource::GetHash() const
  {
    if (m_uiHash != 0)
      return m_uiHash;

    plUInt64 uiHash = m_bLeftHanded ? 1 : 0;
    for (const pxr::VtIntArray& faces : m_SubsetFaces)
      uiHash = HashArray(faces, uiHash + 1);
    uiHash = HashArray(m_Points, uiHash);
    uiHash = HashArray(m_FaceCounts, uiHash);
    uiHash = HashArray(m_FaceIndices, uiHash);
    uiHash = HashArray(m_Normals, uiHash);
    uiHash = HashArray(m_NormalIndices, uiHash);
    uiHash = HashArray(m_Uvs, uiHash);
    uiHash = HashArray(m_UvIndices, uiHash);
    uiHash = HashString(m_NormalsInterpolation.GetString(), uiHash);
    uiHash = HashString(m_UvInterpolation.GetString(), uiHash);
    const double fBasis[2] = {m_Basis.GetUpAxis() == plUsdBasis::UpAxis::Y ? 0.0 : 1.0, m_Basis.GetMetersPerUnit()};
    uiHash = plHashingUtils::xxHash64(fBasis, sizeof(fBasis), uiHash);

    m_uiHash = uiHash != 0 ? uiHash : 1;
    return m_uiHash;
  }

  plUniquePtr<plUsdGeometrySource> CaptureMesh(const plUsdBasis& basis, const pxr::UsdGeomMesh& mesh, plStatus& out_status)
  {
    plUniquePtr<plUsdMeshSource> pSource = PL_DEFAULT_NEW(plUsdMeshSource);
    plUsdMeshSource& src = *pSource;
    src.m_Basis = basis;

    if (!GetValue(mesh.GetPointsAttr(), src.m_Points) || !GetValue(mesh.GetFaceVertexCountsAttr(), src.m_FaceCounts) ||
        !GetValue(mesh.GetFaceVertexIndicesAttr(), src.m_FaceIndices))
    {
      out_status = plStatus("the mesh has no points or no faces");
      return nullptr;
    }

    pxr::TfToken orientation = pxr::UsdGeomTokens->rightHanded;
    GetValue(mesh.GetOrientationAttr(), orientation);
    src.m_bLeftHanded = orientation == pxr::UsdGeomTokens->leftHanded;

    pxr::UsdGeomPrimvar normalsPv = pxr::UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(pxr::TfToken("normals"));
    if (normalsPv && normalsPv.HasValue() && normalsPv.Get(&src.m_Normals, pxr::UsdTimeCode::EarliestTime()))
    {
      normalsPv.GetIndices(&src.m_NormalIndices, pxr::UsdTimeCode::EarliestTime());
      src.m_NormalsInterpolation = normalsPv.GetInterpolation();
    }
    else if (mesh.GetNormalsAttr().HasAuthoredValue() && GetValue(mesh.GetNormalsAttr(), src.m_Normals))
    {
      src.m_NormalsInterpolation = mesh.GetNormalsInterpolation();
    }

    if (pxr::UsdGeomPrimvar uvPv = FindUvPrimvar(mesh.GetPrim()); uvPv && uvPv.Get(&src.m_Uvs, pxr::UsdTimeCode::EarliestTime()))
    {
      uvPv.GetIndices(&src.m_UvIndices, pxr::UsdTimeCode::EarliestTime());
      src.m_UvInterpolation = uvPv.GetInterpolation();
    }

    // Same order as the material groups ReadPrim reports, so group i + 1 is subset i.
    for (const pxr::UsdGeomSubset& subset : pxr::UsdShadeMaterialBindingAPI(mesh.GetPrim()).GetMaterialBindSubsets())
    {
      pxr::VtIntArray faces;
      GetValue(subset.GetIndicesAttr(), faces);
      src.m_SubsetFaces.push_back(faces);
    }

    out_status = plStatus(PL_SUCCESS);
    return pSource;
  }

  plStatus plUsdMeshSource::Build(plGeometry& out_geometry, plDynamicArray<plUInt32>* out_pGroupTriangleCounts) const
  {
    const plUsdBasis& basis = m_Basis;
    const pxr::VtVec3fArray& points = m_Points;
    const pxr::VtIntArray& faceCounts = m_FaceCounts;
    const pxr::VtIntArray& faceIndices = m_FaceIndices;
    const pxr::VtVec3fArray& normals = m_Normals;
    const pxr::VtIntArray& normalIndices = m_NormalIndices;
    const pxr::TfToken& normalsInterpolation = m_NormalsInterpolation;
    const pxr::VtVec2fArray& uvs = m_Uvs;
    const pxr::VtIntArray& uvIndices = m_UvIndices;
    const pxr::TfToken& uvInterpolation = m_UvInterpolation;

    // USD's front faces follow the right-hand rule unless the mesh says otherwise. The basis is a reflection, which
    // reverses winding once more.
    const bool bLeftHanded = m_bLeftHanded;
    const bool bFlipWinding = plUsdBasis::FlipsWinding() != bLeftHanded;

    // Without authored normals, smooth normals per point, accumulated from area-weighted face normals (Newell's
    // method, so polygons with more than three corners are handled too).
    std::vector<pxr::GfVec3f> smoothNormals;
    if (normals.empty())
    {
      smoothNormals.assign(points.size(), pxr::GfVec3f(0.0f));

      size_t uiCorner = 0;
      for (size_t uiFace = 0; uiFace < faceCounts.size(); ++uiFace)
      {
        const int iCount = faceCounts[uiFace];
        if (iCount < 3 || uiCorner + iCount > faceIndices.size())
        {
          uiCorner += plMath::Max(iCount, 0);
          continue;
        }

        pxr::GfVec3f n(0.0f);
        for (int i = 0; i < iCount; ++i)
        {
          const pxr::GfVec3f& a = points[faceIndices[uiCorner + i]];
          const pxr::GfVec3f& b = points[faceIndices[uiCorner + (i + 1) % iCount]];
          n[0] += (a[1] - b[1]) * (a[2] + b[2]);
          n[1] += (a[2] - b[2]) * (a[0] + b[0]);
          n[2] += (a[0] - b[0]) * (a[1] + b[1]);
        }

        if (bLeftHanded)
          n = -n;

        for (int i = 0; i < iCount; ++i)
        {
          const int iPoint = faceIndices[uiCorner + i];
          if (iPoint >= 0 && (size_t)iPoint < smoothNormals.size())
            smoothNormals[iPoint] += n;
        }

        uiCorner += iCount;
      }
    }

    plHybridArray<plUInt32, 16> polygon;

    // Face corners that share a position, a normal and a UV become one vertex. Without this every corner is its own
    // vertex, which for typical quad meshes is four times the vertex data. By position rather than by point index:
    // some exporters write a separate point for every corner of every triangle, and a mesh whose triangles share no
    // vertices cannot be simplified into LODs at all.
    struct CornerKey
    {
      PL_DECLARE_POD_TYPE();

      plUInt32 m_uiPosition[3];
      plUInt32 m_uiNormal[3];
      plUInt32 m_uiUv[2];

      bool operator==(const CornerKey& rhs) const { return plMemoryUtils::IsEqual(this, &rhs); }
    };

    struct CornerKeyHash
    {
      static plUInt32 Hash(const CornerKey& key) { return plHashingUtils::xxHash32(&key, sizeof(CornerKey)); }
      static bool Equal(const CornerKey& a, const CornerKey& b) { return a == b; }
    };

    plHashTable<CornerKey, plUInt32, CornerKeyHash> welded;
    welded.Reserve((plUInt32)points.size());

    // First corner of every face, so faces can be visited in material-group order rather than file order.
    std::vector<size_t> faceStart(faceCounts.size());
    {
      size_t uiCorner = 0;
      for (size_t uiFace = 0; uiFace < faceCounts.size(); ++uiFace)
      {
        const int iCount = faceCounts[uiFace];
        if (iCount < 0 || uiCorner + iCount > faceIndices.size())
          return plStatus(plFmt("face {} indexes past the end of faceVertexIndices", uiFace));

        faceStart[uiFace] = uiCorner;
        uiCorner += iCount;
      }
    }

    // Group 0 holds every face no subset claims; group i + 1 is subset i. A face listed by two subsets goes to the last.
    const plUInt32 uiNumGroups = 1 + (plUInt32)m_SubsetFaces.size();
    std::vector<plUInt32> faceGroup(faceCounts.size(), 0);
    for (plUInt32 s = 0; s < m_SubsetFaces.size(); ++s)
    {
      for (int iFace : m_SubsetFaces[s])
      {
        if (iFace >= 0 && (size_t)iFace < faceGroup.size())
          faceGroup[iFace] = s + 1;
      }
    }

    // Faces in group order; within a group they keep their file order.
    std::vector<plUInt32> faceOrder(faceCounts.size());
    for (size_t uiFace = 0; uiFace < faceOrder.size(); ++uiFace)
      faceOrder[uiFace] = (plUInt32)uiFace;
    std::stable_sort(faceOrder.begin(), faceOrder.end(), [&](plUInt32 a, plUInt32 b) { return faceGroup[a] < faceGroup[b]; });

    if (out_pGroupTriangleCounts)
    {
      out_pGroupTriangleCounts->Clear();
      out_pGroupTriangleCounts->SetCount(uiNumGroups, 0);
    }

    for (plUInt32 uiFace : faceOrder)
    {
      const plUInt32 uiGroup = faceGroup[uiFace];
      const int iCount = faceCounts[uiFace];
      if (iCount < 3)
        continue;

      size_t uiCorner = faceStart[uiFace];
      polygon.Clear();

      for (int i = 0; i < iCount; ++i, ++uiCorner)
      {
        const int iPoint = faceIndices[uiCorner];
        if (iPoint < 0 || (size_t)iPoint >= points.size())
          return plStatus(plFmt("faceVertexIndices[{}] = {} is out of range", uiCorner, iPoint));

        const pxr::GfVec3f& p = points[iPoint];
        const plVec3d vPos = basis.PointToPlasma(plVec3d(p[0], p[1], p[2]));

        pxr::GfVec3f n(0.0f, 0.0f, 1.0f);
        if (!normals.empty())
        {
          if (const pxr::GfVec3f* pN = PickElement(normals, normalIndices, normalsInterpolation, uiFace, uiCorner, iPoint))
            n = *pN;
        }
        else
        {
          n = smoothNormals[iPoint];
        }

        plVec3d vNormal = basis.DirectionToPlasma(plVec3d(n[0], n[1], n[2]));
        vNormal.NormalizeIfNotZero(plVec3d(0, 0, 1)).IgnoreResult();

        plVec2 vUv(0.0f);
        if (!uvs.empty())
        {
          if (const pxr::GfVec2f* pUv = PickElement(uvs, uvIndices, uvInterpolation, uiFace, uiCorner, iPoint))
          {
            // USD's texture origin is bottom-left, the engine's top-left.
            vUv.Set((*pUv)[0], 1.0f - (*pUv)[1]);
          }
        }

        const plVec3 vNormalF((float)vNormal.x, (float)vNormal.y, (float)vNormal.z);

        CornerKey key;
        plMemoryUtils::ZeroFill(&key, 1);
        plMemoryUtils::Copy(key.m_uiPosition, reinterpret_cast<const plUInt32*>(p.data()), 3);
        plMemoryUtils::Copy(key.m_uiNormal, reinterpret_cast<const plUInt32*>(&vNormalF.x), 3);
        plMemoryUtils::Copy(key.m_uiUv, reinterpret_cast<const plUInt32*>(&vUv.x), 2);

        plUInt32 uiVertex = 0;
        if (!welded.TryGetValue(key, uiVertex))
        {
          uiVertex = out_geometry.AddVertex(plVec3((float)vPos.x, (float)vPos.y, (float)vPos.z), vNormalF, vUv);
          welded.Insert(key, uiVertex);
        }

        polygon.PushBack(uiVertex);
      }

      // Fanned into triangles here, so each triangle's material group is known without asking the triangulator.
      for (plUInt32 i = 1; i + 1 < polygon.GetCount(); ++i)
      {
        plUInt32 triangle[3] = {polygon[0], polygon[i], polygon[i + 1]};
        out_geometry.AddPolygon(plArrayPtr<plUInt32>(triangle), bFlipWinding);
      }

      if (out_pGroupTriangleCounts)
        (*out_pGroupTriangleCounts)[uiGroup] += polygon.GetCount() - 2;
    }

    return plStatus(PL_SUCCESS);
  }

  plBasisAxis::Enum ToPlasmaAxis(const plUsdBasis& basis, const pxr::TfToken& axis)
  {
    plVec3d v(0, 0, 1);
    if (axis == pxr::UsdGeomTokens->x)
      v.Set(1, 0, 0);
    else if (axis == pxr::UsdGeomTokens->y)
      v.Set(0, 1, 0);

    const plVec3d p = basis.DirectionToPlasma(v);

    if (p.x > 0.5)
      return plBasisAxis::PositiveX;
    if (p.x < -0.5)
      return plBasisAxis::NegativeX;
    if (p.y > 0.5)
      return plBasisAxis::PositiveY;
    if (p.y < -0.5)
      return plBasisAxis::NegativeY;
    if (p.z > 0.5)
      return plBasisAxis::PositiveZ;
    return plBasisAxis::NegativeZ;
  }

  /// Implicit shapes are generated straight into Plasma's frame, so they need no winding flip.
  class plUsdGprimSource final : public plUsdGeometrySource
  {
  public:
    enum class Shape
    {
      Cube,
      Sphere,
      Cylinder,
      Cone,
      Capsule,
    };

    plStatus Build(plGeometry& out_geometry, plDynamicArray<plUInt32>* out_pGroupTriangleCounts) const override
    {
      const plStatus res = BuildShape(out_geometry);

      // Shapes have no subsets: everything is group 0, triangulated like a mesh.
      out_geometry.TriangulatePolygons();
      if (out_pGroupTriangleCounts)
      {
        out_pGroupTriangleCounts->Clear();
        out_pGroupTriangleCounts->PushBack(out_geometry.GetPolygons().GetCount());
      }

      return res;
    }

    plStatus BuildShape(plGeometry& out_geometry) const
    {
      const float k = (float)m_Basis.GetMetersPerUnit();
      const float fSize = (float)m_fSize * k;
      const float fRadius = (float)m_fRadius * k;
      const float fHeight = (float)m_fHeight * k;

      plGeometry::GeoOptions options;
      options.m_MainAxis = m_Axis;

      switch (m_Shape)
      {
        case Shape::Cube:
          out_geometry.AddBox(plVec3(fSize), true, options);
          break;

        case Shape::Sphere:
          out_geometry.AddGeodesicSphere(fRadius, 3, options);
          break;

        case Shape::Cylinder:
          out_geometry.AddCylinder(fRadius, fRadius, fHeight * 0.5f, fHeight * 0.5f, true, true, 32, options);
          break;

        case Shape::Cone:
          // USD centres the cone on its origin; the engine builds it from the base up.
          options.m_Transform = plMat4::MakeTranslation(-plBasisAxis::GetBasisVector(m_Axis) * (fHeight * 0.5f));
          out_geometry.AddCone(fRadius, fHeight, true, 32, options);
          break;

        case Shape::Capsule:
          out_geometry.AddCapsule(fRadius, fHeight, 32, 8, options);
          break;
      }

      return plStatus(PL_SUCCESS);
    }

    plUInt64 GetHash() const override
    {
      const double values[] = {(double)m_Shape, m_fSize, m_fRadius, m_fHeight, (double)m_Axis, m_Basis.GetMetersPerUnit()};
      return plHashingUtils::xxHash64(values, sizeof(values));
    }

    plUsdBasis m_Basis;
    Shape m_Shape = Shape::Cube;
    double m_fSize = 2.0;
    double m_fRadius = 1.0;
    double m_fHeight = 2.0;
    plBasisAxis::Enum m_Axis = plBasisAxis::PositiveZ;
  };

  plUniquePtr<plUsdGeometrySource> CaptureGprim(const plUsdBasis& basis, const pxr::UsdPrim& prim, plStatus& out_status)
  {
    plUniquePtr<plUsdGprimSource> pSource = PL_DEFAULT_NEW(plUsdGprimSource);
    plUsdGprimSource& src = *pSource;
    src.m_Basis = basis;

    pxr::TfToken axis = pxr::UsdGeomTokens->z;

    if (pxr::UsdGeomCube cube(prim); cube)
    {
      src.m_Shape = plUsdGprimSource::Shape::Cube;
      GetValue(cube.GetSizeAttr(), src.m_fSize);
    }
    else if (pxr::UsdGeomSphere sphere(prim); sphere)
    {
      src.m_Shape = plUsdGprimSource::Shape::Sphere;
      GetValue(sphere.GetRadiusAttr(), src.m_fRadius);
    }
    else if (pxr::UsdGeomCylinder cylinder(prim); cylinder)
    {
      src.m_Shape = plUsdGprimSource::Shape::Cylinder;
      GetValue(cylinder.GetAxisAttr(), axis);
      GetValue(cylinder.GetRadiusAttr(), src.m_fRadius);
      GetValue(cylinder.GetHeightAttr(), src.m_fHeight);
    }
    else if (pxr::UsdGeomCone cone(prim); cone)
    {
      src.m_Shape = plUsdGprimSource::Shape::Cone;
      GetValue(cone.GetAxisAttr(), axis);
      GetValue(cone.GetRadiusAttr(), src.m_fRadius);
      GetValue(cone.GetHeightAttr(), src.m_fHeight);
    }
    else if (pxr::UsdGeomCapsule capsule(prim); capsule)
    {
      src.m_Shape = plUsdGprimSource::Shape::Capsule;
      GetValue(capsule.GetAxisAttr(), axis);
      GetValue(capsule.GetRadiusAttr(), src.m_fRadius);
      GetValue(capsule.GetHeightAttr(), src.m_fHeight);
    }
    else
    {
      out_status = plStatus(plFmt("'{}' is not a shape the engine can tessellate", prim.GetTypeName().GetString().c_str()));
      return nullptr;
    }

    src.m_Axis = ToPlasmaAxis(basis, axis);
    out_status = plStatus(PL_SUCCESS);
    return pSource;
  }
} // namespace

plUniquePtr<plUsdGeometrySource> plUsdStage::CaptureGeometry(plStringView sPrimPath, plStatus& out_status) const
{
  pxr::UsdPrim prim = m_pImpl->GetPrim(sPrimPath);
  if (!prim)
  {
    out_status = plStatus(plFmt("no prim at '{}'", sPrimPath));
    return nullptr;
  }

  if (pxr::UsdGeomMesh mesh(prim); mesh)
    return CaptureMesh(m_pImpl->m_Basis, mesh, out_status);

  return CaptureGprim(m_pImpl->m_Basis, prim, out_status);
}

plStatus plUsdStage::ExtractGeometry(plStringView sPrimPath, plGeometry& out_geometry) const
{
  plStatus res;
  plUniquePtr<plUsdGeometrySource> pSource = CaptureGeometry(sPrimPath, res);
  if (pSource == nullptr)
    return res;

  return pSource->Build(out_geometry, nullptr);
}

plResult plUsdStage::ComputeGeometryKey(plStringView sPrimPath, plUsdGeometryKey& out_key)
{
  Impl& impl = *m_pImpl;
  out_key = {};

  const pxr::UsdPrim prim = impl.GetPrim(sPrimPath);
  const plUsdPrimKind kind = GetPrimKind(prim);
  if (!prim || (kind != plUsdPrimKind::Mesh && kind != plUsdPrimKind::Gprim))
    return PL_FAILURE;

  std::vector<std::pair<std::string, pxr::SdfPath>> specs;
  std::vector<pxr::SdfLayerHandle> layers;

  const plUInt64 uiIdentity = HashGeometrySpecsImpl(prim, plUsdStageAccess::BasisSeed(impl.m_Basis),
    [&](const pxr::SdfPropertySpecHandle& spec)
    {
      const pxr::SdfLayerHandle layer = spec->GetLayer();
      const pxr::SdfPath& specPath = spec->GetPath();
      specs.emplace_back(layer->GetIdentifier(), specPath);

      if (std::find(layers.begin(), layers.end(), layer) == layers.end())
        layers.push_back(layer);

      if (out_key.m_sName.IsEmpty())
        out_key.m_sName = FromStd(specPath.GetPrimPath().GetName());
    });

  if (out_key.m_sName.IsEmpty())
    out_key.m_sName = FromStd(prim.GetName().GetString());

  plUInt64 uiFiles = 0;
  for (const pxr::SdfLayerHandle& layer : layers)
  {
    const std::string& sReal = layer->GetRealPath();
    if (sReal.empty())
    {
      uiFiles = HashString(layer->GetIdentifier(), uiFiles);
      continue;
    }

    const plString sKey = NormalizedPath(sReal.c_str());
    plInt64 iTime = 0;
    if (auto it = impl.m_LayerTimes.Find(sKey); it.IsValid())
    {
      iTime = it.Value().GetInt64(plSIUnitOfTime::Microsecond);
    }
    else
    {
      plFileStats stats;
      if (plOSFile::GetFileStats(sKey, stats).Succeeded())
        iTime = stats.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond);
    }

    uiFiles = plHashingUtils::xxHash64String(sKey, uiFiles);
    uiFiles = plHashingUtils::xxHash64(&iTime, sizeof(iTime), uiFiles);
  }

  out_key.m_uiIdentity = uiIdentity != 0 ? uiIdentity : 1;
  out_key.m_uiFileStamp = uiFiles;

  impl.m_Listener.Remember(prim.GetPath(), std::move(specs));
  return PL_SUCCESS;
}

void plUsdStage::ConsumeGeometryChanges(plUsdGeometryChanges& out_changes)
{
  m_pImpl->m_Listener.ConsumeGeometryChanges(out_changes);
}

//////////////////////////////////////////////////////////////////////////
// Layers

void plUsdStage::GetUsedLayerFiles(plDynamicArray<plString>& out_files) const
{
  out_files.Clear();

  if (!m_pImpl->m_Stage)
    return;

  for (const pxr::SdfLayerHandle& layer : m_pImpl->m_Stage->GetUsedLayers())
  {
    const std::string& sReal = layer->GetRealPath();
    if (!sReal.empty())
      out_files.PushBack(NormalizedPath(sReal.c_str()));
  }
}

void plUsdStage::GetReferencedAssetFiles(plDynamicArray<plString>& out_files) const
{
  out_files.Clear();

  if (!m_pImpl->m_Stage)
    return;

  plSet<plString> unique;

  for (const pxr::UsdPrim& prim : m_pImpl->m_Stage->TraverseAll())
  {
    for (const pxr::UsdAttribute& attr : prim.GetAttributes())
    {
      if (attr.GetTypeName() != pxr::SdfValueTypeNames->Asset)
        continue;

      pxr::SdfAssetPath asset;
      if (GetValue(attr, asset) && !asset.GetResolvedPath().empty())
      {
        unique.Insert(NormalizedPath(asset.GetResolvedPath().c_str()));
      }
    }
  }

  for (const plString& s : unique)
    out_files.PushBack(s);
}

void plUsdStage::GetLayerStack(plDynamicArray<plUsdLayerInfo>& out_layers) const
{
  out_layers.Clear();

  const Impl& impl = *m_pImpl;
  if (!impl.m_Stage)
    return;

  for (const pxr::SdfLayerHandle& layer : impl.m_Stage->GetLayerStack(false))
  {
    plUsdLayerInfo& info = out_layers.ExpandAndGetRef();
    info.m_sIdentifier = FromStd(layer->GetIdentifier());
    info.m_sRealPath = layer->GetRealPath().empty() ? plString() : NormalizedPath(layer->GetRealPath().c_str());
    info.m_bIsEditTarget = impl.m_EditLayer && layer == impl.m_EditLayer;
    info.m_bDirty = layer->IsDirty();
    info.m_bMuted = impl.m_Stage->IsLayerMuted(layer->GetIdentifier());

    plFileStats stats;
    if (!info.m_sRealPath.IsEmpty() && plOSFile::GetFileStats(info.m_sRealPath, stats).Succeeded())
      info.m_ModificationTime = stats.m_LastModificationTime;
  }

  // Muted layers leave the layer stack, but the panel still has to list them to unmute them again.
  for (const std::string& sMuted : impl.m_Stage->GetMutedLayers())
  {
    pxr::SdfLayerHandle layer = pxr::SdfLayer::Find(sMuted);
    plUsdLayerInfo& info = out_layers.ExpandAndGetRef();
    info.m_sIdentifier = FromStd(sMuted);
    info.m_sRealPath = layer && !layer->GetRealPath().empty() ? NormalizedPath(layer->GetRealPath().c_str()) : plString();
    info.m_bMuted = true;
  }
}

plUInt32 plUsdStage::CountLayerOpinions(plStringView sLayerIdentifier) const
{
  pxr::SdfLayerHandle layer = pxr::SdfLayer::Find(ToStd(sLayerIdentifier));
  if (!layer)
    return 0;

  plUInt32 uiCount = 0;
  layer->Traverse(pxr::SdfPath::AbsoluteRootPath(), [&](const pxr::SdfPath& path)
    {
      if (path.IsPropertyPath())
        ++uiCount;
      else if (path.IsPrimPath() && layer->HasField(path, pxr::SdfFieldKeys->Active))
        ++uiCount;
    });

  return uiCount;
}

plStatus plUsdStage::SetLayerMuted(plStringView sLayerIdentifier, bool bMuted)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_Stage)
    return plStatus("no stage is open");

  const std::string sIdentifier = ToStd(sLayerIdentifier);
  if (sIdentifier == impl.m_Stage->GetRootLayer()->GetIdentifier() || (impl.m_EditLayer && sIdentifier == impl.m_EditLayer->GetIdentifier()))
    return plStatus("the root layer and the layer being written cannot be muted");

  if (bMuted)
    impl.m_Stage->MuteLayer(sIdentifier);
  else
    impl.m_Stage->UnmuteLayer(sIdentifier);

  return plStatus(PL_SUCCESS);
}

plUInt32 plUsdStage::ReloadChangedLayers(plArrayPtr<const plString> skipLayers)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_Stage)
    return 0;

  std::set<pxr::SdfLayerHandle> changed;

  for (const pxr::SdfLayerHandle& layer : impl.m_Stage->GetUsedLayers())
  {
    if (impl.m_EditLayer && layer == impl.m_EditLayer)
      continue;

    const std::string& sReal = layer->GetRealPath();
    if (sReal.empty())
      continue;

    bool bSkip = false;
    for (const plString& sSkip : skipLayers)
      bSkip |= SamePath(sSkip, sReal.c_str());
    if (bSkip)
      continue;

    plFileStats stats;
    if (plOSFile::GetFileStats(sReal.c_str(), stats).Failed())
      continue;

    const plString sKey = NormalizedPath(sReal.c_str());
    auto it = impl.m_LayerTimes.Find(sKey);
    if (!it.IsValid() || !it.Value().Compare(stats.m_LastModificationTime, plTimestamp::CompareMode::Identical))
    {
      changed.insert(layer);
    }
  }

  if (changed.empty())
    return 0;

  pxr::TfErrorMark mark;
  pxr::SdfLayer::ReloadLayers(changed, true);

  if (!mark.IsClean())
  {
    plLog::Warning("OpenUSD reported problems reloading layers: {}", CollectErrors(mark));
  }

  // A reloaded root layer can bring new sublayers with it, so the set of files is re-read rather than updated. Skipped
  // layers keep their old time, so they reload once they are no longer skipped.
  plMap<plString, plTimestamp> skippedTimes;
  for (const plString& sSkip : skipLayers)
  {
    if (auto it = impl.m_LayerTimes.Find(NormalizedPath(sSkip)); it.IsValid())
      skippedTimes[it.Key()] = it.Value();
  }

  impl.RememberLayerTimes();

  for (auto it = skippedTimes.GetIterator(); it.IsValid(); ++it)
    impl.m_LayerTimes[it.Key()] = it.Value();

  return (plUInt32)changed.size();
}

plString plUsdStage::ExportEditLayer() const
{
  const Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return {};

  std::string sText;
  impl.m_EditLayer->ExportToString(&sText);
  return FromStd(sText);
}

plStatus plUsdStage::ImportLayerContent(plStringView sLayerFile, plStringView sUsda)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_Stage)
    return plStatus("no stage is open");

  for (const pxr::SdfLayerHandle& layer : impl.m_Stage->GetUsedLayers())
  {
    if (layer->GetRealPath().empty() || !SamePath(sLayerFile, layer->GetRealPath().c_str()))
      continue;

    if (impl.m_EditLayer && layer == impl.m_EditLayer)
      return plStatus("the edit layer only changes through this editor");

    pxr::TfErrorMark mark;
    if (!layer->ImportFromString(ToStd(sUsda)))
      return plStatus(plFmt("could not read the content sent for '{}': {}", sLayerFile, CollectErrors(mark)));

    return plStatus(PL_SUCCESS);
  }

  return plStatus(plFmt("'{}' is not a layer of this stage", sLayerFile));
}

void plUsdStage::ConsumeChanges(plUsdChanges& out_changes)
{
  out_changes.m_ResyncedPaths.Clear();
  out_changes.m_ChangedInfoPaths.Clear();

  plUsdNoticeListener& listener = m_pImpl->m_Listener;

  for (const plString& s : listener.m_Resynced)
    out_changes.m_ResyncedPaths.PushBack(s);

  for (const plString& s : listener.m_ChangedInfo)
    out_changes.m_ChangedInfoPaths.PushBack(s);

  listener.m_Resynced.Clear();
  listener.m_ChangedInfo.Clear();
}

//////////////////////////////////////////////////////////////////////////
// Writing back

plStatus plUsdStage::SetEditLayer(plStringView sLayerFile)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_Stage)
    return plStatus("no stage is open");

  for (const pxr::SdfLayerHandle& layer : impl.m_Stage->GetLayerStack(false))
  {
    if (!layer->GetRealPath().empty() && SamePath(layer->GetRealPath().c_str(), sLayerFile))
    {
      impl.m_EditLayer = layer;
      impl.m_sEditLayer = NormalizedPath(layer->GetRealPath().c_str());
      impl.m_Stage->SetEditTarget(pxr::UsdEditTarget(layer));
      return plStatus(PL_SUCCESS);
    }
  }

  return plStatus(plFmt("'{}' is not part of the stage's layer stack", sLayerFile));
}

plStringView plUsdStage::GetEditLayer() const
{
  return m_pImpl->m_sEditLayer;
}

bool plUsdStage::HasEditLayer() const
{
  return m_pImpl->m_EditLayer != nullptr;
}

plStatus plUsdStage::AuthorLocalTransform(plStringView sPrimPath, const plTransform& localTransformPlasma)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return plStatus("there is no edit layer to write into");

  pxr::UsdPrim prim = impl.GetPrim(sPrimPath);
  pxr::UsdGeomXformable xformable(prim);
  if (!xformable)
    return plStatus(plFmt("'{}' is not transformable", sPrimPath));

  const pxr::GfMatrix4d target = ToGf(impl.m_Basis.MatrixToUsd(RemoveLookCorrection(impl.m_Basis, prim, plUsdBasis::TransformToMatrix(localTransformPlasma))));

  pxr::TfErrorMark mark;

  // Prefer the translate / rotateXYZ / scale channels every DCC shows its users. The decomposition is checked by
  // recomposing it; anything it cannot express exactly (shear, a pivot) is written as a single matrix instead.
  if (pxr::UsdGeomXformCommonAPI common(prim); common)
  {
    const pxr::GfTransform decomposed(target);
    const pxr::GfVec3d vEuler = decomposed.GetRotation().Decompose(pxr::GfVec3d::XAxis(), pxr::GfVec3d::YAxis(), pxr::GfVec3d::ZAxis());
    const pxr::GfVec3d vScale = decomposed.GetScale();
    const pxr::GfVec3d vTranslate = decomposed.GetTranslation();

    const pxr::GfRotation rotation = pxr::GfRotation(pxr::GfVec3d::XAxis(), vEuler[0]) * pxr::GfRotation(pxr::GfVec3d::YAxis(), vEuler[1]) *
                                     pxr::GfRotation(pxr::GfVec3d::ZAxis(), vEuler[2]);

    const pxr::GfMatrix4d recomposed = pxr::GfMatrix4d(1.0).SetScale(vScale) * pxr::GfMatrix4d(1.0).SetRotate(rotation) *
                                       pxr::GfMatrix4d(1.0).SetTranslate(vTranslate);

    if (pxr::GfIsClose(recomposed, target, 1e-6) && pxr::GfIsClose(decomposed.GetScaleOrientation().GetAngle(), 0.0, 1e-6))
    {
      if (common.SetXformVectors(vTranslate, pxr::GfVec3f((float)vEuler[0], (float)vEuler[1], (float)vEuler[2]), pxr::GfVec3f(vScale),
            pxr::GfVec3f(0.0f), pxr::UsdGeomXformCommonAPI::RotationOrderXYZ, pxr::UsdTimeCode::Default()))
      {
        return plStatus(PL_SUCCESS);
      }
    }
  }

  xformable.ClearXformOpOrder();
  pxr::UsdGeomXformOp op = xformable.MakeMatrixXform();
  if (!op || !op.Set(target))
    return plStatus(plFmt("could not author a transform on '{}': {}", sPrimPath, CollectErrors(mark)));

  return plStatus(PL_SUCCESS);
}

plStatus plUsdStage::AuthorVisibility(plStringView sPrimPath, bool bVisible)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return plStatus("there is no edit layer to write into");

  pxr::UsdGeomImageable imageable(impl.GetPrim(sPrimPath));
  if (!imageable)
    return plStatus(plFmt("'{}' is not imageable", sPrimPath));

  if (!imageable.CreateVisibilityAttr().Set(bVisible ? pxr::UsdGeomTokens->inherited : pxr::UsdGeomTokens->invisible))
    return plStatus(plFmt("could not author visibility on '{}'", sPrimPath));

  return plStatus(PL_SUCCESS);
}

plStatus plUsdStage::AuthorActive(plStringView sPrimPath, bool bActive)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return plStatus("there is no edit layer to write into");

  pxr::UsdPrim prim = impl.GetPrim(sPrimPath);
  if (!prim)
  {
    // An inactive prim is not on the stage, so reactivating it goes through the layer directly.
    pxr::SdfPrimSpecHandle spec = impl.m_EditLayer->GetPrimAtPath(pxr::SdfPath(ToStd(sPrimPath)));
    if (!spec)
      return plStatus(plFmt("no prim at '{}'", sPrimPath));

    spec->SetActive(bActive);
    return plStatus(PL_SUCCESS);
  }

  if (!prim.SetActive(bActive))
    return plStatus(plFmt("could not change whether '{}' is active", sPrimPath));

  return plStatus(PL_SUCCESS);
}

plStatus plUsdStage::AuthorVariantSelection(plStringView sPrimPath, plStringView sVariantSet, plStringView sVariant)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return plStatus("there is no edit layer to write into");

  pxr::UsdPrim prim = impl.GetPrim(sPrimPath);
  if (!prim || !prim.GetVariantSets().HasVariantSet(ToStd(sVariantSet)))
    return plStatus(plFmt("'{}' has no variant set '{}'", sPrimPath, sVariantSet));

  pxr::UsdVariantSet set = prim.GetVariantSet(ToStd(sVariantSet));
  if (!set.HasAuthoredVariant(ToStd(sVariant)))
    return plStatus(plFmt("variant set '{}' of '{}' has no variant '{}'", sVariantSet, sPrimPath, sVariant));

  if (!set.SetVariantSelection(ToStd(sVariant)))
    return plStatus(plFmt("could not select variant '{}' of '{}'", sVariant, sPrimPath));

  return plStatus(PL_SUCCESS);
}

plStatus plUsdStage::AuthorComponents(plStringView sPrimPath, plArrayPtr<const plUsdComponent> components)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return plStatus("there is no edit layer to write into");

  pxr::UsdPrim prim = impl.GetPrim(sPrimPath);
  if (!prim)
    return plStatus(plFmt("no prim at '{}'", sPrimPath));

  plDynamicArray<plUsdComponent> current;
  ReadComponents(prim, current);

  for (const plUsdComponent& component : current)
  {
    bool bListed = false;
    for (const plUsdComponent& wanted : components)
      bListed |= wanted.m_sType == component.m_sType;

    if (bListed)
      continue;

    const pxr::TfToken schema = GetComponentSchema(component.m_sType);

    if (pxr::SdfPrimSpecHandle spec = impl.m_EditLayer->GetPrimAtPath(pxr::SdfPath(ToStd(sPrimPath))))
    {
      // What the edit layer added itself goes first; only a component weaker layers apply needs a delete.
      RemoveApiSchemaItems(spec, [&](const pxr::TfToken& item)
        { return item == schema; });

      const std::string sPrefix = GetComponentNamespace(component.m_sType) + ":";

      std::vector<pxr::SdfPropertySpecHandle> toRemove;
      for (const pxr::SdfPropertySpecHandle& property : spec->GetProperties())
      {
        if (property->GetName().rfind(sPrefix, 0) == 0)
          toRemove.push_back(property);
      }

      for (const pxr::SdfPropertySpecHandle& property : toRemove)
        spec->RemoveProperty(property);
    }

    // Changing the applied schemas recomposes the prim, which expires the handle, so the prim is looked up again. A
    // delete also hides the component where weaker layers apply it; their values stay, unused.
    prim = impl.GetPrim(sPrimPath);
    if (!prim)
      return plStatus(plFmt("no prim at '{}'", sPrimPath));

    const pxr::TfTokenVector& stillApplied = prim.GetPrimTypeInfo().GetAppliedAPISchemas();
    if (std::find(stillApplied.begin(), stillApplied.end(), schema) != stillApplied.end() && !prim.RemoveAppliedSchema(schema))
      return plStatus(plFmt("could not remove component '{}' from '{}'", component.m_sType, sPrimPath));
  }

  prim = impl.GetPrim(sPrimPath);
  const pxr::TfTokenVector applied = prim ? prim.GetPrimTypeInfo().GetAppliedAPISchemas() : pxr::TfTokenVector();

  for (const plUsdComponent& component : components)
  {
    const pxr::TfToken schema = GetComponentSchema(component.m_sType);
    if (std::find(applied.begin(), applied.end(), schema) != applied.end())
      continue;

    prim = impl.GetPrim(sPrimPath);
    if (!prim || !prim.AddAppliedSchema(schema))
      return plStatus(plFmt("could not add component '{}' to '{}'", component.m_sType, sPrimPath));
  }

  prim = impl.GetPrim(sPrimPath);
  if (!prim)
    return plStatus(plFmt("'{}' is gone after adding its components", sPrimPath));

  for (const plUsdComponent& component : components)
  {
    const std::string sNamespace = GetComponentNamespace(component.m_sType);

    for (const plUsdComponentValue& entry : component.m_Values)
    {
      pxr::VtValue value;
      pxr::SdfValueTypeName type;
      if (!ToUsdValue(entry.m_Value, value, type))
        continue;

      const pxr::TfToken name(sNamespace + ":" + ToStd(entry.m_sProperty));

      pxr::UsdAttribute attribute = prim.GetAttribute(name);
      pxr::VtValue existing;
      if (attribute && attribute.Get(&existing) && existing == value)
        continue;

      if (!attribute)
        attribute = prim.CreateAttribute(name, type, /*custom*/ true);

      if (!attribute || !attribute.Set(value))
        return plStatus(plFmt("could not write '{}' on '{}'", name.GetText(), sPrimPath));
    }
  }

  return plStatus(PL_SUCCESS);
}

namespace
{
  bool Differs(float fValue, float fCurrent)
  {
    return plMath::Abs(fValue - fCurrent) > 1e-5f * plMath::Max(1.0f, plMath::Abs(fCurrent));
  }
} // namespace

plStatus plUsdStage::AuthorLight(plStringView sPrimPath, const plUsdLight& light)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return plStatus("there is no edit layer to write into");

  pxr::UsdPrim prim = impl.GetPrim(sPrimPath);
  if (!prim || !prim.HasAPI<pxr::UsdLuxLightAPI>())
    return plStatus(plFmt("'{}' is not a light", sPrimPath));

  const float k = (float)impl.m_Basis.GetMetersPerUnit();

  plUsdLight current;
  ReadLight(prim, k, impl.m_bUnrealAuthored, current);

  pxr::UsdLuxLightAPI api(prim);

  if (!(light.m_Color == current.m_Color))
    api.CreateColorAttr().Set(pxr::GfVec3f(light.m_Color.r, light.m_Color.g, light.m_Color.b));
  if (light.m_bUseTemperature != current.m_bUseTemperature)
    api.CreateEnableColorTemperatureAttr().Set(light.m_bUseTemperature);
  if (Differs(light.m_fTemperature, current.m_fTemperature))
    api.CreateColorTemperatureAttr().Set(light.m_fTemperature);
  if (light.m_bNormalize != current.m_bNormalize)
    api.CreateNormalizeAttr().Set(light.m_bNormalize);
  if (Differs(light.m_fSpecular, current.m_fSpecular))
    api.CreateSpecularAttr().Set(light.m_fSpecular);

  // Luminance is intensity * 2^exposure; the exposure stays as the DCC set it.
  if (Differs(light.m_fLuminance, current.m_fLuminance))
  {
    float fExposure = 0.0f;
    GetValue(api.GetExposureAttr(), fExposure);
    api.CreateIntensityAttr().Set(light.m_fLuminance / plMath::Pow(2.0f, fExposure));
  }

  if (light.m_bCastShadows != current.m_bCastShadows)
    pxr::UsdLuxShadowAPI::Apply(prim).CreateShadowEnableAttr().Set(light.m_bCastShadows);

  switch (current.m_Shape)
  {
    case plUsdLightShape::Sphere:
      if (Differs(light.m_fRadius, current.m_fRadius))
        pxr::UsdLuxSphereLight(prim).CreateRadiusAttr().Set(light.m_fRadius / k);
      if (Differs(light.m_ConeAngle.GetDegree(), current.m_ConeAngle.GetDegree()) || Differs(light.m_fConeSoftness, current.m_fConeSoftness))
      {
        pxr::UsdLuxShapingAPI shaping = pxr::UsdLuxShapingAPI::Apply(prim);
        shaping.CreateShapingConeAngleAttr().Set(light.m_ConeAngle.GetDegree() * 0.5f);
        shaping.CreateShapingConeSoftnessAttr().Set(light.m_fConeSoftness);
      }
      break;

    case plUsdLightShape::Rect:
      if (Differs(light.m_fWidth, current.m_fWidth))
        pxr::UsdLuxRectLight(prim).CreateWidthAttr().Set(light.m_fWidth / k);
      if (Differs(light.m_fHeight, current.m_fHeight))
        pxr::UsdLuxRectLight(prim).CreateHeightAttr().Set(light.m_fHeight / k);
      break;

    case plUsdLightShape::Disk:
      if (Differs(light.m_fRadius, current.m_fRadius))
        pxr::UsdLuxDiskLight(prim).CreateRadiusAttr().Set(light.m_fRadius / k);
      break;

    case plUsdLightShape::Cylinder:
      if (Differs(light.m_fRadius, current.m_fRadius))
        pxr::UsdLuxCylinderLight(prim).CreateRadiusAttr().Set(light.m_fRadius / k);
      if (Differs(light.m_fLength, current.m_fLength))
        pxr::UsdLuxCylinderLight(prim).CreateLengthAttr().Set(light.m_fLength / k);
      break;

    case plUsdLightShape::Distant:
    case plUsdLightShape::Other:
      break;
  }

  return plStatus(PL_SUCCESS);
}

plStatus plUsdStage::AuthorCamera(plStringView sPrimPath, const plUsdCamera& camera)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return plStatus("there is no edit layer to write into");

  pxr::UsdPrim prim = impl.GetPrim(sPrimPath);
  pxr::UsdGeomCamera usdCamera(prim);
  if (!prim || !prim.IsA<pxr::UsdGeomCamera>())
    return plStatus(plFmt("'{}' is not a camera", sPrimPath));

  const float k = (float)impl.m_Basis.GetMetersPerUnit();

  plUsdCamera current;
  ReadCamera(prim, k, current);

  // Millimetres -> tenths of a scene unit.
  const float fToTenths = 1.0f / (k * 100.0f);

  if (camera.m_bOrthographic != current.m_bOrthographic)
    usdCamera.CreateProjectionAttr().Set(camera.m_bOrthographic ? pxr::UsdGeomTokens->orthographic : pxr::UsdGeomTokens->perspective);

  if (Differs(camera.m_fFocalLength, current.m_fFocalLength))
    usdCamera.CreateFocalLengthAttr().Set(camera.m_fFocalLength * fToTenths);

  // The orthographic width is the horizontal aperture, in scene units rather than tenths.
  if (Differs(camera.m_fOrthoWidth, current.m_fOrthoWidth))
    usdCamera.CreateHorizontalApertureAttr().Set(camera.m_fOrthoWidth / (0.1f * k));
  else if (Differs(camera.m_fHorizontalAperture, current.m_fHorizontalAperture))
    usdCamera.CreateHorizontalApertureAttr().Set(camera.m_fHorizontalAperture * fToTenths);

  if (Differs(camera.m_fVerticalAperture, current.m_fVerticalAperture))
    usdCamera.CreateVerticalApertureAttr().Set(camera.m_fVerticalAperture * fToTenths);

  if (Differs(camera.m_fNear, current.m_fNear) || Differs(camera.m_fFar, current.m_fFar))
    usdCamera.CreateClippingRangeAttr().Set(pxr::GfVec2f(camera.m_fNear / k, camera.m_fFar / k));

  if (Differs(camera.m_fFStop, current.m_fFStop))
    usdCamera.CreateFStopAttr().Set(camera.m_fFStop);
  if (Differs(camera.m_fFocusDistance, current.m_fFocusDistance))
    usdCamera.CreateFocusDistanceAttr().Set(camera.m_fFocusDistance / k);
  if (Differs(camera.m_fExposure, current.m_fExposure))
    usdCamera.CreateExposureAttr().Set(camera.m_fExposure);

  return plStatus(PL_SUCCESS);
}

namespace
{
  /// The surface shader of a material and the input names its shader id uses, or false for a shader Plasma does not read.
  bool FindSurfaceShader(const pxr::UsdPrim& materialPrim, pxr::UsdShadeShader& out_shader, const SurfaceInputNames*& out_pNames)
  {
    pxr::UsdShadeMaterial material(materialPrim);
    if (!material)
      return false;

    // The same order as ReadSurface, so values are written to the shader the stage is read from.
    const pxr::TfTokenVector contexts = {pxr::TfToken("mtlx"), pxr::UsdShadeTokens->universalRenderContext};
    out_shader = material.ComputeSurfaceSource(contexts);
    if (!out_shader)
      return false;

    pxr::TfToken id;
    out_shader.GetShaderId(&id);

    for (const SurfaceInputNames& names : s_SurfaceShaders)
    {
      if (id == names.m_szShaderId)
      {
        out_pNames = &names;
        return true;
      }
    }

    return false;
  }

  /// Removes the edit layer's "inputs:" opinions on one prim.
  bool ClearInputs(const pxr::SdfLayerHandle& layer, const pxr::SdfPath& path, bool bRemove)
  {
    pxr::SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
    if (!spec)
      return false;

    std::vector<pxr::SdfPropertySpecHandle> inputs;
    for (const pxr::SdfPropertySpecHandle& prop : spec->GetProperties())
    {
      if (prop->GetName().rfind("inputs:", 0) == 0)
        inputs.push_back(prop);
    }

    if (bRemove)
    {
      for (const pxr::SdfPropertySpecHandle& prop : inputs)
        spec->RemoveProperty(prop);
    }

    return !inputs.empty();
  }
} // namespace

plStatus plUsdStage::AuthorSurfaceValue(plStringView sMaterialPath, SurfaceValue value, const plVariant& data)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return plStatus("there is no edit layer to write into");

  pxr::UsdShadeShader shader;
  const SurfaceInputNames* pNames = nullptr;
  if (!FindSurfaceShader(impl.GetPrim(sMaterialPath), shader, pNames))
    return plStatus(plFmt("'{}' has no surface shader Plasma can write", sMaterialPath));

  const char* szInput = nullptr;
  switch (value)
  {
    case SurfaceValue::BaseColor:
      szInput = pNames->m_szBaseColor;
      break;
    case SurfaceValue::Metallic:
      szInput = pNames->m_szMetallic;
      break;
    case SurfaceValue::Roughness:
      szInput = pNames->m_szRoughness;
      break;
    case SurfaceValue::Opacity:
      szInput = pNames->m_szOpacity;
      break;
  }

  if (szInput == nullptr)
    return plStatus("this shader has no such input");

  pxr::UsdShadeInput input = shader.GetInput(pxr::TfToken(szInput));

  // The attribute that decides the value: the shader's input, or the material's interface input it is connected to.
  pxr::UsdAttribute target;
  if (input)
  {
    for (const pxr::UsdAttribute& attr : input.GetValueProducingAttributes())
    {
      if (pxr::UsdShadeUtils::GetType(attr.GetName()) == pxr::UsdShadeAttributeType::Output)
        return plStatus("a texture or another node drives this value; change it in the tool that owns the material");

      target = attr;
      break;
    }
  }

  const bool bColor = value == SurfaceValue::BaseColor;
  if (!target)
  {
    target = shader.CreateInput(pxr::TfToken(szInput), bColor ? pxr::SdfValueTypeNames->Color3f : pxr::SdfValueTypeNames->Float).GetAttr();
  }

  bool bOk = false;
  if (bColor)
  {
    plColor color = data.ConvertTo<plColor>();

    // OpenPBR and standard surface scale the base colour by a weight; the weight stays as it is.
    float fWeight = 1.0f;
    ReadShaderInput(shader, pNames->m_szBaseWeight, fWeight, nullptr);
    if (fWeight > 0.0f)
      color = plColor(color.r / fWeight, color.g / fWeight, color.b / fWeight);

    bOk = target.Set(pxr::GfVec3f(color.r, color.g, color.b));
  }
  else
  {
    const float fValue = data.ConvertTo<float>();

    // Standard surface's opacity is a colour.
    if (target.GetTypeName() == pxr::SdfValueTypeNames->Color3f)
      bOk = target.Set(pxr::GfVec3f(fValue));
    else
      bOk = target.Set(fValue);
  }

  return bOk ? plStatus(PL_SUCCESS) : plStatus(plFmt("could not write '{}' on '{}'", szInput, sMaterialPath));
}

plStatus plUsdStage::ClearSurfaceValues(plStringView sMaterialPath)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return plStatus("there is no edit layer");

  pxr::SdfChangeBlock block;
  ClearInputs(impl.m_EditLayer, pxr::SdfPath(ToStd(sMaterialPath)), true);

  pxr::UsdShadeShader shader;
  const SurfaceInputNames* pNames = nullptr;
  if (FindSurfaceShader(impl.GetPrim(sMaterialPath), shader, pNames))
    ClearInputs(impl.m_EditLayer, shader.GetPath(), true);

  impl.m_EditLayer->RemoveInertSceneDescription();
  return plStatus(PL_SUCCESS);
}

bool plUsdStage::HasSurfaceOpinion(plStringView sMaterialPath) const
{
  const Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return false;

  if (ClearInputs(impl.m_EditLayer, pxr::SdfPath(ToStd(sMaterialPath)), false))
    return true;

  pxr::UsdShadeShader shader;
  const SurfaceInputNames* pNames = nullptr;
  return FindSurfaceShader(impl.GetPrim(sMaterialPath), shader, pNames) && ClearInputs(impl.m_EditLayer, shader.GetPath(), false);
}

namespace
{
  bool IsXformProperty(const std::string& sName)
  {
    return sName == "xformOpOrder" || sName.rfind("xformOp:", 0) == 0;
  }

  bool MatchesWhat(const std::string& sName, plStringView sWhat)
  {
    if (sWhat.IsEmpty())
      return true;
    if (sWhat == "xform")
      return IsXformProperty(sName);
    if (sWhat == "visibility")
      return sName == "visibility";
    if (sWhat == "values")
      return !IsXformProperty(sName) && sName != "visibility" && !IsComponentProperty(sName);
    if (sWhat == "components")
      return IsComponentProperty(sName);
    return false;
  }

  bool IsComponentSchema(const pxr::TfToken& schema)
  {
    return !GetComponentType(schema).IsEmpty();
  }

  /// Whether the spec's apiSchemas list adds or deletes a Plasma component.
  bool HasComponentSchemaItems(const pxr::SdfPrimSpecHandle& spec)
  {
    if (!spec->HasInfo(pxr::UsdTokens->apiSchemas))
      return false;

    const pxr::SdfTokenListOp listOp = spec->GetInfo(pxr::UsdTokens->apiSchemas).Get<pxr::SdfTokenListOp>();
    for (const pxr::SdfTokenListOp::ItemVector* pItems : {&listOp.GetExplicitItems(), &listOp.GetPrependedItems(), &listOp.GetAppendedItems(), &listOp.GetDeletedItems()})
    {
      for (const pxr::TfToken& item : *pItems)
      {
        if (IsComponentSchema(item))
          return true;
      }
    }

    return false;
  }
} // namespace

plStatus plUsdStage::ClearEditLayerOpinions(plStringView sPrimPath, plStringView sWhat)
{
  Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return plStatus("there is no edit layer");

  pxr::SdfPrimSpecHandle spec = impl.m_EditLayer->GetPrimAtPath(pxr::SdfPath(ToStd(sPrimPath)));
  if (!spec)
    return plStatus(PL_SUCCESS);

  pxr::SdfChangeBlock block;

  std::vector<pxr::SdfPropertySpecHandle> toRemove;
  for (const pxr::SdfPropertySpecHandle& prop : spec->GetProperties())
  {
    if (MatchesWhat(prop->GetName(), sWhat))
      toRemove.push_back(prop);
  }

  for (const pxr::SdfPropertySpecHandle& prop : toRemove)
  {
    spec->RemoveProperty(prop);
  }

  if (sWhat.IsEmpty() || sWhat == "active")
  {
    spec->ClearActive();
  }

  if (sWhat.IsEmpty() || sWhat == "variants")
  {
    std::vector<std::string> sets;
    for (const auto& selection : spec->GetVariantSelections())
      sets.push_back(selection.first);

    // An empty name removes the opinion rather than selecting nothing.
    for (const std::string& sSet : sets)
      spec->SetVariantSelection(sSet, "");
  }

  // The schemas applied for a light's shaping cone or shadow setting go with the values, the component schemas with the
  // components.
  if (sWhat.IsEmpty())
  {
    spec->ClearInfo(pxr::UsdTokens->apiSchemas);
  }
  else if (sWhat == "values")
  {
    RemoveApiSchemaItems(spec, [](const pxr::TfToken& item)
      { return !IsComponentSchema(item); });
  }
  else if (sWhat == "components")
  {
    RemoveApiSchemaItems(spec, [](const pxr::TfToken& item)
      { return IsComponentSchema(item); });
  }

  impl.m_EditLayer->RemoveInertSceneDescription();
  return plStatus(PL_SUCCESS);
}

void plUsdStage::QueryPrimOpinions(plStringView sPrimPath, plUsdPrimOpinions& out_opinions) const
{
  out_opinions = plUsdPrimOpinions();

  const Impl& impl = *m_pImpl;
  if (!impl.m_Stage)
    return;

  const pxr::SdfPath path(ToStd(sPrimPath));

  // Strongest first; muted layers are not part of the stack.
  for (const pxr::SdfLayerHandle& layer : impl.m_Stage->GetLayerStack(false))
  {
    pxr::SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
    if (!spec)
      continue;

    bool bXform = false;
    bool bVisibility = false;
    bool bValues = false;
    bool bComponents = false;
    for (const pxr::SdfPropertySpecHandle& prop : spec->GetProperties())
    {
      if (IsXformProperty(prop->GetName()))
        bXform = true;
      else if (prop->GetName() == "visibility")
        bVisibility = true;
      else if (IsComponentProperty(prop->GetName()))
        bComponents = true;
      else
        bValues = true;
    }

    const bool bVariants = !spec->GetVariantSelections().empty();
    bComponents |= HasComponentSchemaItems(spec);

    if (impl.m_EditLayer && layer == impl.m_EditLayer)
    {
      out_opinions.m_bEditXform = bXform;
      out_opinions.m_bEditVisibility = bVisibility;
      out_opinions.m_bEditActive = spec->HasActive();
      out_opinions.m_bEditValues = bValues;
      out_opinions.m_bEditVariants = bVariants;
      out_opinions.m_bEditComponents = bComponents;
    }

    // A pass-through "over" that only leads to children decides nothing about this prim.
    const bool bDecides = !spec->GetProperties().empty() || spec->HasActive() || bVariants || bComponents || spec->GetSpecifier() == pxr::SdfSpecifierDef;
    if (bDecides && out_opinions.m_sWinningLayer.IsEmpty())
    {
      const std::string& sReal = layer->GetRealPath();
      out_opinions.m_sWinningLayer = sReal.empty() ? FromStd(layer->GetIdentifier()) : NormalizedPath(sReal.c_str());
    }
  }
}

bool plUsdStage::HasEditLayerOpinion(plStringView sPrimPath, plStringView sWhat) const
{
  const Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return false;

  pxr::SdfPrimSpecHandle spec = impl.m_EditLayer->GetPrimAtPath(pxr::SdfPath(ToStd(sPrimPath)));
  if (!spec)
    return false;

  for (const pxr::SdfPropertySpecHandle& prop : spec->GetProperties())
  {
    if (MatchesWhat(prop->GetName(), sWhat))
      return true;
  }

  if ((sWhat.IsEmpty() || sWhat == "variants") && !spec->GetVariantSelections().empty())
    return true;

  if ((sWhat.IsEmpty() || sWhat == "components") && HasComponentSchemaItems(spec))
    return true;

  return (sWhat.IsEmpty() || sWhat == "active") && spec->HasActive();
}

void plUsdStage::GetAttributeLayerStack(plStringView sPrimPath, plStringView sAttribute, plDynamicArray<plString>& out_layers) const
{
  out_layers.Clear();

  pxr::UsdPrim prim = m_pImpl->GetPrim(sPrimPath);
  if (!prim)
    return;

  pxr::UsdAttribute attr = prim.GetAttribute(pxr::TfToken(ToStd(sAttribute)));
  if (!attr)
    return;

  for (const pxr::SdfPropertySpecHandle& spec : attr.GetPropertyStack(pxr::UsdTimeCode::Default()))
  {
    const std::string& sReal = spec->GetLayer()->GetRealPath();
    out_layers.PushBack(sReal.empty() ? FromStd(spec->GetLayer()->GetIdentifier()) : NormalizedPath(sReal.c_str()));
  }
}

bool plUsdStage::IsEditLayerDirty() const
{
  return m_pImpl->m_EditLayer && m_pImpl->m_EditLayer->IsDirty();
}

plStatus plUsdStage::SaveEditLayer()
{
  Impl& impl = *m_pImpl;
  if (!impl.m_EditLayer)
    return plStatus("there is no edit layer");

  if (!impl.m_EditLayer->IsDirty())
    return plStatus(PL_SUCCESS);

  pxr::TfErrorMark mark;
  if (!impl.m_EditLayer->Save())
    return plStatus(plFmt("could not save '{}': {}", impl.m_sEditLayer, CollectErrors(mark)));

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////
// Sessions

plStatus plUsdStage::CreateSession(plStringView sAssetRootLayer, plStringView sSessionName, plStringView sParticipant,
  plString& out_sSessionRootLayer, plString& out_sParticipantLayer)
{
  const plString sAsset = NormalizedPath(sAssetRootLayer);
  if (!plOSFile::ExistsFile(sAsset))
    return plStatus(plFmt("USD file not found: '{}'", sAsset));

  const plStringView sAssetFileName = sAsset.GetFileNameAndExtension();

  plStringBuilder sSessionDir(GetSessionsFolder(sAsset), "/", sSessionName);

  if (plOSFile::CreateDirectoryStructure(sSessionDir).Failed())
    return plStatus(plFmt("could not create '{}'", sSessionDir));

  plStringBuilder sParticipantLayer(sSessionDir, "/", sParticipant, ".usda");
  plStringBuilder sSessionRoot(sSessionDir, "/session.usda");

  pxr::TfErrorMark mark;

  if (!plOSFile::ExistsFile(sParticipantLayer))
  {
    pxr::SdfLayerRefPtr layer = pxr::SdfLayer::CreateNew(ToStd(sParticipantLayer));
    if (!layer)
      return plStatus(plFmt("could not create '{}': {}", sParticipantLayer, CollectErrors(mark)));

    plStringBuilder sDoc;
    sDoc.SetFormat("Opinions authored by {} during live session '{}'.", sParticipant, sSessionName);
    layer->SetDocumentation(ToStd(sDoc));
    layer->Save();
  }

  plStringBuilder sParticipantRel("./", sParticipant, ".usda");

  if (!plOSFile::ExistsFile(sSessionRoot))
  {
    pxr::SdfLayerRefPtr asset = pxr::SdfLayer::FindOrOpen(ToStd(sAsset));
    if (!asset)
      return plStatus(plFmt("could not open '{}': {}", sAsset, CollectErrors(mark)));

    pxr::SdfLayerRefPtr session = pxr::SdfLayer::CreateNew(ToStd(sSessionRoot));
    if (!session)
      return plStatus(plFmt("could not create '{}': {}", sSessionRoot, CollectErrors(mark)));

    // The root layer decides how the stage composes and how it is measured, so it repeats the asset's metadata.
    for (const char* szKey : {"upAxis", "metersPerUnit", "defaultPrim", "startTimeCode", "endTimeCode", "timeCodesPerSecond", "framesPerSecond"})
    {
      const pxr::TfToken key(szKey);
      if (asset->GetPseudoRoot()->HasInfo(key))
      {
        session->GetPseudoRoot()->SetInfo(key, asset->GetPseudoRoot()->GetInfo(key));
      }
    }

    session->SetDocumentation("Live session root. Each participant authors only into its own sublayer; the asset is the weakest layer.");

    plStringBuilder sAssetRel("../../../", sAssetFileName);
    session->SetSubLayerPaths({ToStd(sParticipantRel), ToStd(sAssetRel)});

    if (!session->Save())
      return plStatus(plFmt("could not save '{}': {}", sSessionRoot, CollectErrors(mark)));
  }
  else
  {
    pxr::SdfLayerRefPtr session = pxr::SdfLayer::FindOrOpen(ToStd(sSessionRoot));
    if (!session)
      return plStatus(plFmt("could not open '{}': {}", sSessionRoot, CollectErrors(mark)));

    std::vector<std::string> subLayers = session->GetSubLayerPaths();
    bool bPresent = false;
    for (const std::string& s : subLayers)
    {
      if (plStringView(s.c_str()).EndsWith_NoCase(plStringBuilder(sParticipant, ".usda")))
        bPresent = true;
    }

    if (!bPresent)
    {
      // A participant that joins later becomes the strongest layer; the session owner can reorder.
      session->InsertSubLayerPath(ToStd(sParticipantRel), 0);
      if (!session->Save())
        return plStatus(plFmt("could not save '{}': {}", sSessionRoot, CollectErrors(mark)));
    }
  }

  out_sSessionRootLayer = sSessionRoot;
  out_sParticipantLayer = sParticipantLayer;
  return plStatus(PL_SUCCESS);
}

plString plUsdStage::GetSessionsFolder(plStringView sAssetRootLayer)
{
  const plString sAsset = NormalizedPath(sAssetRootLayer);

  plStringBuilder sFolder = sAsset;
  sFolder.PathParentDirectory();
  sFolder.AppendFormat(".live/{}", sAsset.GetFileName());
  return sFolder;
}

void plUsdStage::ListSessions(plStringView sAssetRootLayer, plDynamicArray<plString>& out_sessions)
{
  out_sessions.Clear();

  const plString sFolder = GetSessionsFolder(sAssetRootLayer);
  if (!plOSFile::ExistsDirectory(sFolder))
    return;

  plFileSystemIterator it;
  for (it.StartSearch(sFolder, plFileSystemIteratorFlags::ReportFolders); it.IsValid(); it.Next())
  {
    plStringBuilder sSessionRoot(sFolder, "/", it.GetStats().m_sName, "/session.usda");
    if (plOSFile::ExistsFile(sSessionRoot))
      out_sessions.PushBack(it.GetStats().m_sName);
  }

  out_sessions.Sort();
}

namespace
{
  /// A session root's sublayers as real paths, strongest first; the last is the asset.
  bool GetSessionLayers(const pxr::SdfLayerRefPtr& session, std::vector<std::string>& out_layers)
  {
    out_layers.clear();
    for (const std::string& sSub : session->GetSubLayerPaths())
    {
      const std::string sPath = pxr::SdfComputeAssetPathRelativeToLayer(session, sSub);
      if (sPath.empty())
        return false;
      out_layers.push_back(sPath);
    }
    return out_layers.size() >= 2;
  }

  void MergePrimSpec(const pxr::SdfPrimSpecHandle& source, const pxr::SdfLayerHandle& target, plUInt32& ref_uiOpinions)
  {
    const pxr::SdfPath& path = source->GetPath();
    pxr::SdfPrimSpecHandle dest = target->GetPrimAtPath(path);

    // The asset keeps its definitions: an "over" only adds opinions, a "def" of a new prim adds the prim.
    if (!dest)
    {
      dest = pxr::SdfCreatePrimInLayer(target, path);
      if (!dest)
        return;
      dest->SetSpecifier(source->GetSpecifier());
    }
    else if (source->GetSpecifier() == pxr::SdfSpecifierDef && dest->GetSpecifier() == pxr::SdfSpecifierOver)
    {
      dest->SetSpecifier(pxr::SdfSpecifierDef);
    }

    if (!source->GetTypeName().IsEmpty() && (dest->GetTypeName().IsEmpty() || source->GetSpecifier() == pxr::SdfSpecifierDef))
      dest->SetTypeName(source->GetTypeName());

    if (source->HasActive())
    {
      dest->SetActive(source->GetActive());
      ++ref_uiOpinions;
    }

    // Applied schemas, e.g. a light's ShapingAPI, join the asset's prepended ones.
    if (source->HasInfo(pxr::UsdTokens->apiSchemas))
    {
      const pxr::SdfTokenListOp sourceOp = source->GetInfo(pxr::UsdTokens->apiSchemas).Get<pxr::SdfTokenListOp>();
      pxr::SdfTokenListOp destOp;
      if (dest->HasInfo(pxr::UsdTokens->apiSchemas))
        destOp = dest->GetInfo(pxr::UsdTokens->apiSchemas).Get<pxr::SdfTokenListOp>();

      pxr::TfTokenVector items = destOp.GetPrependedItems();
      for (const pxr::TfTokenVector* pList : {&sourceOp.GetPrependedItems(), &sourceOp.GetAppendedItems(), &sourceOp.GetExplicitItems()})
      {
        for (const pxr::TfToken& token : *pList)
        {
          if (std::find(items.begin(), items.end(), token) == items.end())
            items.push_back(token);
        }
      }

      // A removed Plasma component leaves the asset with its values. A schema the asset's root layer doesn't apply comes
      // from its references, so the delete moves into the root layer.
      pxr::TfTokenVector appended = destOp.GetAppendedItems();
      pxr::TfTokenVector deleted = destOp.GetDeletedItems();
      for (const pxr::TfToken& token : sourceOp.GetDeletedItems())
      {
        const size_t uiBefore = items.size() + appended.size();
        items.erase(std::remove(items.begin(), items.end(), token), items.end());
        appended.erase(std::remove(appended.begin(), appended.end(), token), appended.end());

        if (items.size() + appended.size() == uiBefore && std::find(deleted.begin(), deleted.end(), token) == deleted.end())
          deleted.push_back(token);

        const plStringView sType = GetComponentType(token);
        if (sType.IsEmpty())
          continue;

        const std::string sPrefix = GetComponentNamespace(sType) + ":";
        std::vector<pxr::SdfPropertySpecHandle> toRemove;
        for (const pxr::SdfPropertySpecHandle& property : dest->GetProperties())
        {
          if (property->GetName().rfind(sPrefix, 0) == 0)
            toRemove.push_back(property);
        }

        for (const pxr::SdfPropertySpecHandle& property : toRemove)
          dest->RemoveProperty(property);

        ++ref_uiOpinions;
      }

      destOp.SetPrependedItems(items);
      destOp.SetAppendedItems(appended);
      destOp.SetDeletedItems(deleted);
      if (destOp.HasKeys())
        dest->SetInfo(pxr::UsdTokens->apiSchemas, pxr::VtValue(destOp));
      else
        dest->ClearInfo(pxr::UsdTokens->apiSchemas);
    }

    for (const auto& selection : source->GetVariantSelections())
      dest->SetVariantSelection(selection.first, selection.second);

    for (const pxr::SdfPropertySpecHandle& prop : source->GetProperties())
    {
      if (pxr::SdfCopySpec(source->GetLayer(), prop->GetPath(), target, prop->GetPath()))
        ++ref_uiOpinions;
    }

    for (const pxr::SdfPrimSpecHandle& child : source->GetNameChildren())
      MergePrimSpec(child, target, ref_uiOpinions);
  }
} // namespace

plString plUsdStage::GetSessionOwnerLayer(plStringView sSessionRootLayer)
{
  pxr::SdfLayerRefPtr session = pxr::SdfLayer::FindOrOpen(ToStd(NormalizedPath(sSessionRootLayer)));
  if (!session)
    return {};

  std::vector<std::string> layers;
  if (!GetSessionLayers(session, layers))
    return {};

  return NormalizedPath(layers.front().c_str());
}

plStatus plUsdStage::CommitSession(plStringView sSessionRootLayer, plUInt32& out_uiOpinions)
{
  out_uiOpinions = 0;

  pxr::TfErrorMark mark;
  pxr::SdfLayerRefPtr session = pxr::SdfLayer::FindOrOpen(ToStd(NormalizedPath(sSessionRootLayer)));
  if (!session)
    return plStatus(plFmt("could not open the session '{}'", sSessionRootLayer));

  std::vector<std::string> layers;
  if (!GetSessionLayers(session, layers))
    return plStatus("the session has no participant layers");

  pxr::SdfLayerRefPtr asset = pxr::SdfLayer::FindOrOpen(layers.back());
  if (!asset)
    return plStatus(plFmt("could not open the asset '{}'", layers.back().c_str()));

  std::vector<pxr::SdfLayerRefPtr> participants;
  for (size_t i = 0; i + 1 < layers.size(); ++i)
  {
    pxr::SdfLayerRefPtr layer = pxr::SdfLayer::FindOrOpen(layers[i]);
    if (!layer)
      return plStatus(plFmt("could not open the participant layer '{}'", layers[i].c_str()));
    participants.push_back(layer);
  }

  {
    pxr::SdfChangeBlock block;

    // Weakest first, so the strongest participant's opinion is the one that stays.
    for (auto it = participants.rbegin(); it != participants.rend(); ++it)
    {
      for (const pxr::SdfPrimSpecHandle& root : (*it)->GetRootPrims())
        MergePrimSpec(root, asset, out_uiOpinions);
    }
  }

  if (!asset->Save())
    return plStatus(plFmt("could not save '{}': {}", layers.back().c_str(), CollectErrors(mark)));

  // Emptied, not removed: every tool keeps its layer in the session and goes on writing into it.
  for (const pxr::SdfLayerRefPtr& layer : participants)
  {
    const std::string sDocumentation = layer->GetDocumentation();
    layer->Clear();
    layer->SetDocumentation(sDocumentation);
    if (!layer->Save())
      return plStatus(plFmt("merged, but could not empty '{}': {}", layer->GetRealPath().c_str(), CollectErrors(mark)));
  }

  return plStatus(PL_SUCCESS);
}

plString plUsdStage::GetOpenUsdVersion()
{
  plStringBuilder s;
  s.SetFormat("{}.{}.{}", PXR_MAJOR_VERSION, PXR_MINOR_VERSION, PXR_PATCH_VERSION);
  return s;
}

plArrayPtr<const plStringView> plUsdStage::GetFileExtensions()
{
  static const plStringView s_Extensions[] = {"usd", "usda", "usdc", "usdz"};
  return plMakeArrayPtr(s_Extensions);
}

bool plUsdStage::IsUsdFile(plStringView sFile)
{
  for (plStringView sExtension : GetFileExtensions())
  {
    if (sFile.HasExtension(sExtension))
      return true;
  }
  return false;
}

plString plUsdStage::GetFileDialogFilter()
{
  plStringBuilder s("USD (");
  for (plStringView sExtension : GetFileExtensions())
    s.AppendFormat("{}*.{}", s.EndsWith("(") ? "" : " ", sExtension);
  s.Append(")");
  return s;
}

//////////////////////////////////////////////////////////////////////////
// Bridge-internal access

pxr::UsdStageRefPtr plUsdStageAccess::GetStage(const plUsdStage& stage)
{
  return stage.m_pImpl->m_Stage;
}

plUsdPrimKind plUsdStageAccess::GetKind(const pxr::UsdPrim& prim)
{
  return GetPrimKind(prim);
}

plUniquePtr<plUsdGeometrySource> plUsdStageAccess::CaptureGeometry(const plUsdBasis& basis, const pxr::UsdPrim& prim, plStatus& out_status)
{
  if (pxr::UsdGeomMesh mesh(prim); mesh)
    return CaptureMesh(basis, mesh, out_status);

  return CaptureGprim(basis, prim, out_status);
}

plMat4d plUsdStageAccess::FromGfMatrix(const pxr::GfMatrix4d& m)
{
  return FromGf(m);
}

plUInt64 plUsdStageAccess::BasisSeed(const plUsdBasis& basis)
{
  const double fBasis[2] = {basis.GetUpAxis() == plUsdBasis::UpAxis::Y ? 0.0 : 1.0, basis.GetMetersPerUnit()};
  return plHashingUtils::xxHash64(fBasis, sizeof(fBasis));
}

plUInt64 plUsdStageAccess::HashGeometrySpecs(const pxr::UsdPrim& prim, plUInt64 uiSeed)
{
  return HashGeometrySpecsImpl(prim, uiSeed, [](const pxr::SdfPropertySpecHandle&) {});
}

plUInt64 plUsdStageAccess::HashGeometrySpecs(const pxr::UsdPrim& prim, plUInt64 uiSeed, LayerStamps& ref_layerStamps, plUInt64& inout_uiFileStamp)
{
  std::vector<pxr::SdfLayerHandle> layers;
  const plUInt64 uiHash = HashGeometrySpecsImpl(prim, uiSeed,
    [&](const pxr::SdfPropertySpecHandle& spec)
    {
      const pxr::SdfLayerHandle layer = spec->GetLayer();
      if (std::find(layers.begin(), layers.end(), layer) == layers.end())
        layers.push_back(layer);
    });

  for (const pxr::SdfLayerHandle& layer : layers)
  {
    const std::string& sReal = layer->GetRealPath();
    if (sReal.empty())
    {
      inout_uiFileStamp = HashString(layer->GetIdentifier(), inout_uiFileStamp);
      continue;
    }

    auto it = ref_layerStamps.find(sReal);
    if (it == ref_layerStamps.end())
    {
      plUInt64 uiStamp = 0;
      plFileStats stats;
      if (plOSFile::GetFileStats(NormalizedPath(sReal.c_str()), stats).Succeeded())
      {
        const plInt64 iTime = stats.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond);
        uiStamp = plHashingUtils::xxHash64(&iTime, sizeof(iTime), stats.m_uiFileSize);
      }
      it = ref_layerStamps.emplace(sReal, uiStamp).first;
    }

    inout_uiFileStamp = HashString(sReal, inout_uiFileStamp);
    inout_uiFileStamp = plHashingUtils::xxHash64(&it->second, sizeof(plUInt64), inout_uiFileStamp);
  }

  return uiHash;
}
