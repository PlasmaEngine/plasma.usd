#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/Graphics/Camera.h>
#include <Core/World/Component.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <EditorPluginUsd/LiveLink/UsdPrimComponents.h>
#include <RendererCore/Components/CameraComponent.h>
#include <RendererCore/Lights/CylinderLightComponent.h>
#include <RendererCore/Lights/DirectionalLightComponent.h>
#include <RendererCore/Lights/DiskLightComponent.h>
#include <RendererCore/Lights/Photometry.h>
#include <RendererCore/Lights/PointLightComponent.h>
#include <RendererCore/Lights/RectLightComponent.h>
#include <RendererCore/Lights/SpotLightComponent.h>
#include <RendererCore/Meshes/InstancedMeshComponent.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>
#include <ToolsFoundation/Object/ObjectDirectAccessor.h>

namespace
{
  /// A spot light's cone is capped by the engine; wider cones are point lights.
  constexpr float MaxSpotAngleDegrees = 160.0f;

  bool IsSpot(const plUsdLight& light)
  {
    return light.m_Shape == plUsdLightShape::Sphere && light.m_ConeAngle.GetDegree() < MaxSpotAngleDegrees;
  }

  /// Where the mapping writes: a component in a document, or a table of the values it would set.
  struct Setter
  {
    plDocumentObjectManager* m_pManager = nullptr;
    plDocumentObject* m_pComponent = nullptr;
    plHashTable<plString, plVariant>* m_pRecord = nullptr;

    void operator()(plStringView sProperty, const plVariant& value) const
    {
      if (m_pRecord != nullptr)
        (*m_pRecord)[sProperty] = value;
      else
        m_pManager->SetValue(m_pComponent, sProperty, value).IgnoreResult();
    }
  };

  /// Colour and brightness: USD colours can exceed 1, the component's colour can't, so the excess moves into the intensity.
  float ApplyColor(const Setter& set, const plUsdLight& light)
  {
    plColor color = light.m_Color;
    const float fMax = plMath::Max(color.r, color.g, color.b);
    float fScale = 1.0f;
    if (fMax > 1.0f)
    {
      fScale = fMax;
      color.r /= fMax;
      color.g /= fMax;
      color.b /= fMax;
    }

    set("UseColorTemperature", light.m_bUseTemperature);
    set("LightColor", plColorGammaUB(color));
    set("Temperature", (plUInt32)plMath::Clamp(light.m_fTemperature, 1000.0f, 15000.0f));

    set("SpecularMultiplier", plMath::Max(light.m_fSpecular, 0.0f));
    set("CastShadows", light.m_bCastShadows);
    return fScale;
  }

  /// Candela per nit of a sphere light: a glowing sphere seen from anywhere shows a disc of its radius, pi r^2. Unreal
  /// spreads the candela over the whole surface, 4 pi r^2 (plUsdLight::m_bUnrealUnits).
  float GetSphereCandelaPerNit(const plUsdLight& light)
  {
    const float fDisc = plMath::Pi<float>() * light.m_fRadius * light.m_fRadius;
    return light.m_bUnrealUnits ? 4.0f * fDisc : fDisc;
  }

  void ApplyLight(const Setter& set, const plUsdLight& light)
  {
    const float fScale = ApplyColor(set, light);
    const float fLuminance = light.m_fLuminance * fScale;

    // A normalized light keeps its power whatever its size: luminance * pi lumens, which the components take directly.
    const float fNormalizedLumens = fLuminance * plMath::Pi<float>();

    switch (light.m_Shape)
    {
      case plUsdLightShape::Sphere:
      {
        if (light.m_bNormalize)
        {
          set("IntensityUnit", (plInt64)plPunctualLightUnit::Lumen);
          set("Intensity", fNormalizedLumens);
        }
        else
        {
          // A zero radius has no surface, so its value is taken as candela directly.
          const float fCandela = light.m_fRadius > 0.0f ? fLuminance * GetSphereCandelaPerNit(light) : fLuminance;
          set("IntensityUnit", (plInt64)plPunctualLightUnit::Candela);
          set("Intensity", fCandela);
        }

        if (IsSpot(light))
        {
          // The cone masks the sphere's light rather than focusing it.
          const plAngle outer = light.m_ConeAngle;
          set("Reflector", false);
          set("OuterSpotAngle", outer);
          set("InnerSpotAngle", outer * plMath::Clamp(1.0f - light.m_fConeSoftness, 0.0f, 1.0f));
        }
        break;
      }

      case plUsdLightShape::Distant:
        set("Intensity", fLuminance);
        break;

      case plUsdLightShape::Rect:
      case plUsdLightShape::Disk:
      case plUsdLightShape::Cylinder:
      {
        if (light.m_bNormalize)
        {
          set("IntensityUnit", (plInt64)plAreaLightUnit::Lumen);
          set("Intensity", fNormalizedLumens);
        }
        else
        {
          set("IntensityUnit", (plInt64)plAreaLightUnit::Nits);
          set("Intensity", fLuminance);
        }

        if (light.m_Shape == plUsdLightShape::Rect)
        {
          set("Width", plMath::Max(light.m_fWidth, 0.01f));
          set("Height", plMath::Max(light.m_fHeight, 0.01f));
        }
        else if (light.m_Shape == plUsdLightShape::Disk)
        {
          set("Radius", plMath::Max(light.m_fRadius, 0.01f));
        }
        else
        {
          // USD cylinder lights emit from their sides only.
          set("Radius", plMath::Max(light.m_fRadius, 0.001f));
          set("Length", plMath::Max(light.m_fLength, 0.01f));
          set("EndCaps", false);
        }
        break;
      }

      case plUsdLightShape::Other:
        break;
    }
  }

  void ApplyCamera(const Setter& set, const plUsdCamera& camera)
  {
    set("NearPlane", plMath::Clamp(camera.m_fNear, 0.01f, 4.0f));
    set("FarPlane", plMath::Clamp(camera.m_fFar, 5.0f, 10000.0f));

    if (camera.m_bOrthographic)
    {
      set("Mode", (plInt64)plCameraMode::OrthoFixedWidth);
      set("Dimensions", plMath::Clamp(camera.m_fOrthoWidth, 0.01f, 10000.0f));
    }
    else
    {
      const float fFit = plUsdPrimComponents::GetLensFit(camera);
      const float fFocal = plMath::Max(camera.m_fFocalLength, 0.001f) * fFit;
      const float fWidth = plMath::Max(camera.m_fHorizontalAperture, 0.001f) * fFit;
      const float fHeight = plMath::Max(camera.m_fVerticalAperture, 0.001f) * fFit;

      set("Mode", (plInt64)plCameraMode::PerspectiveFixedFovX);
      set("FovMode", (plInt64)plCameraFovMode::FocalLength);
      set("SensorPreset", (plInt64)plCameraSensorPreset::Custom);
      set("GateFit", (plInt64)plCameraGateFit::Horizontal);
      set("FocalLength", fFocal);
      set("SensorWidth", plMath::Clamp(fWidth, 1.0f, 100.0f));
      set("SensorHeight", plMath::Clamp(fHeight, 1.0f, 100.0f));
    }

    if (camera.m_fFStop > 0.0f)
      set("Aperture", plMath::Clamp(camera.m_fFStop, 1.0f, 32.0f));

    if (camera.m_fFocusDistance > 0.0f)
    {
      set("FocusMode", (plInt64)plCameraFocusMode::Manual);
      set("FocusDistance", plMath::Clamp(camera.m_fFocusDistance, 0.0f, 100000.0f));
    }
    else
    {
      set("FocusMode", (plInt64)plCameraFocusMode::Auto);
    }

    set("ExposureCompensation", plMath::Clamp(camera.m_fExposure, -32.0f, 32.0f));
  }
} // namespace

float plUsdPrimComponents::GetLensFit(const plUsdCamera& camera)
{
  // The field of view depends only on focal length over aperture. All three are scaled into the component's ranges when
  // a stage's units put them outside, keeping the ratios.
  const float fFocal = plMath::Max(camera.m_fFocalLength, 0.001f);
  const float fWidth = plMath::Max(camera.m_fHorizontalAperture, 0.001f);
  const float fHeight = plMath::Max(camera.m_fVerticalAperture, 0.001f);

  const float fLargest = plMath::Max(fWidth, fHeight);
  const float fSmallest = plMath::Min(fWidth, fHeight);
  float fFit = 1.0f;
  if (fLargest > 100.0f)
    fFit = 100.0f / fLargest;
  else if (fSmallest < 1.0f)
    fFit = 1.0f / fSmallest;
  if (fFocal * fFit < 4.0f)
    fFit = 4.0f / fFocal;
  else if (fFocal * fFit > 1200.0f)
    fFit = 1200.0f / fFocal;

  return fFit;
}

bool plUsdPrimComponents::ReadBack(const plDocumentObject* pComponent, const plUsdPrim& applied, plUsdLight& out_light, plUsdCamera& out_camera)
{
  out_light = applied.m_Light;
  out_camera = applied.m_Camera;

  if (pComponent == nullptr || pComponent->GetType() != GetComponentType(applied))
    return false;

  // What the mapping set from the prim's current USD values; only properties that differ from it were edited.
  plHashTable<plString, plVariant> expected;
  Setter record;
  record.m_pRecord = &expected;
  if (applied.m_Kind == plUsdPrimKind::Camera)
    ApplyCamera(record, applied.m_Camera);
  else
    ApplyLight(record, applied.m_Light);

  auto get = [&](plStringView sProperty)
  { return pComponent->GetTypeAccessor().GetValue(sProperty); };

  auto changed = [&](plStringView sProperty)
  {
    const plVariant* pExpected = expected.GetValue(sProperty);
    if (pExpected == nullptr)
      return false;

    const plVariant actual = get(sProperty);
    if (pExpected->IsNumber() && actual.IsNumber())
      return plMath::Abs(pExpected->ConvertTo<double>() - actual.ConvertTo<double>()) > 1e-4 * plMath::Max(1.0, plMath::Abs(pExpected->ConvertTo<double>()));

    return *pExpected != actual;
  };

  bool bChanged = false;

  if (applied.m_Kind == plUsdPrimKind::Camera)
  {
    plUsdCamera& c = out_camera;
    const float fFit = GetLensFit(applied.m_Camera);

    if (changed("Mode"))
    {
      const plInt64 iMode = get("Mode").ConvertTo<plInt64>();
      c.m_bOrthographic = iMode == plCameraMode::OrthoFixedWidth || iMode == plCameraMode::OrthoFixedHeight;
      bChanged = true;
    }
    if (changed("Dimensions"))
    {
      c.m_fOrthoWidth = get("Dimensions").ConvertTo<float>();
      bChanged = true;
    }
    if (changed("FocalLength"))
    {
      c.m_fFocalLength = get("FocalLength").ConvertTo<float>() / fFit;
      bChanged = true;
    }
    if (changed("SensorWidth"))
    {
      c.m_fHorizontalAperture = get("SensorWidth").ConvertTo<float>() / fFit;
      bChanged = true;
    }
    if (changed("SensorHeight"))
    {
      c.m_fVerticalAperture = get("SensorHeight").ConvertTo<float>() / fFit;
      bChanged = true;
    }
    if (changed("NearPlane") || changed("FarPlane"))
    {
      c.m_fNear = get("NearPlane").ConvertTo<float>();
      c.m_fFar = get("FarPlane").ConvertTo<float>();
      bChanged = true;
    }
    if (changed("ExposureCompensation"))
    {
      c.m_fExposure = get("ExposureCompensation").ConvertTo<float>();
      bChanged = true;
    }

    // Aperture is only mapped when USD has an f-stop; an edit sets one.
    if (applied.m_Camera.m_fFStop > 0.0f ? changed("Aperture") : get("Aperture") != plVariant(8.0f))
    {
      c.m_fFStop = get("Aperture").ConvertTo<float>();
      bChanged = true;
    }

    if (changed("FocusMode") || changed("FocusDistance"))
    {
      const bool bManual = get("FocusMode").ConvertTo<plInt64>() == plCameraFocusMode::Manual;
      c.m_fFocusDistance = bManual ? get("FocusDistance").ConvertTo<float>() : 0.0f;
      bChanged = true;
    }

    return bChanged;
  }

  plUsdLight& l = out_light;

  if (changed("LightColor"))
  {
    l.m_Color = plColor(get("LightColor").ConvertTo<plColorGammaUB>());
    bChanged = true;
  }
  if (changed("UseColorTemperature"))
  {
    l.m_bUseTemperature = get("UseColorTemperature").ConvertTo<bool>();
    bChanged = true;
  }
  if (changed("Temperature"))
  {
    l.m_fTemperature = get("Temperature").ConvertTo<float>();
    bChanged = true;
  }
  if (changed("SpecularMultiplier"))
  {
    l.m_fSpecular = get("SpecularMultiplier").ConvertTo<float>();
    bChanged = true;
  }
  if (changed("CastShadows"))
  {
    l.m_bCastShadows = get("CastShadows").ConvertTo<bool>();
    bChanged = true;
  }

  // Lumen is USD's normalized light: its output stays the same whatever its size.
  if (changed("Intensity") || changed("IntensityUnit"))
  {
    const float fIntensity = get("Intensity").ConvertTo<float>();

    if (l.m_Shape == plUsdLightShape::Distant)
    {
      l.m_fLuminance = fIntensity;
    }
    else
    {
      const bool bLumen = get("IntensityUnit").ConvertTo<plInt64>() == 0;
      l.m_bNormalize = bLumen;

      if (bLumen)
        l.m_fLuminance = fIntensity / plMath::Pi<float>();
      else if (l.m_Shape == plUsdLightShape::Sphere && l.m_fRadius > 0.0f)
        l.m_fLuminance = fIntensity / GetSphereCandelaPerNit(l);
      else
        l.m_fLuminance = fIntensity;
    }

    bChanged = true;
  }

  if (changed("OuterSpotAngle") || changed("InnerSpotAngle"))
  {
    const plAngle outer = get("OuterSpotAngle").ConvertTo<plAngle>();
    const plAngle inner = get("InnerSpotAngle").ConvertTo<plAngle>();
    l.m_ConeAngle = outer;
    l.m_fConeSoftness = outer.GetRadian() > 0.0f ? plMath::Clamp(1.0f - inner.GetRadian() / outer.GetRadian(), 0.0f, 1.0f) : 0.0f;
    bChanged = true;
  }

  for (const char* szSize : {"Width", "Height", "Radius", "Length"})
  {
    if (!changed(szSize))
      continue;

    const float fValue = get(szSize).ConvertTo<float>();
    const plStringView sSize = szSize;
    if (sSize == "Width")
      l.m_fWidth = fValue;
    else if (sSize == "Height")
      l.m_fHeight = fValue;
    else if (sSize == "Radius")
      l.m_fRadius = fValue;
    else
      l.m_fLength = fValue;
    bChanged = true;
  }

  return bChanged;
}

const plRTTI* plUsdPrimComponents::GetComponentType(const plUsdPrim& prim)
{
  if (prim.m_Kind == plUsdPrimKind::Camera)
    return plGetStaticRTTI<plCameraComponent>();

  if (prim.m_Kind != plUsdPrimKind::Light)
    return nullptr;

  switch (prim.m_Light.m_Shape)
  {
    case plUsdLightShape::Sphere:
      return IsSpot(prim.m_Light) ? plGetStaticRTTI<plSpotLightComponent>() : plGetStaticRTTI<plPointLightComponent>();
    case plUsdLightShape::Distant:
      return plGetStaticRTTI<plDirectionalLightComponent>();
    case plUsdLightShape::Rect:
      return plGetStaticRTTI<plRectLightComponent>();
    case plUsdLightShape::Disk:
      return plGetStaticRTTI<plDiskLightComponent>();
    case plUsdLightShape::Cylinder:
      return plGetStaticRTTI<plCylinderLightComponent>();
    case plUsdLightShape::Other:
      return nullptr;
  }

  return nullptr;
}

void plUsdPrimComponents::Apply(plDocumentObjectManager* pManager, const plDocumentObject* pObject, const plUsdPrim& prim, const plUuid& componentGuid)
{
  plObjectDirectAccessor accessor(pManager);
  const plRTTI* pType = GetComponentType(prim);

  const plDocumentObject* pExisting = pManager->GetObject(componentGuid);
  if (pExisting != nullptr && pExisting->GetType() != pType)
  {
    accessor.RemoveObject(pExisting).IgnoreResult();
    pExisting = nullptr;
  }

  if (pType == nullptr)
    return;

  if (pExisting == nullptr)
  {
    plUuid guid = componentGuid;
    if (accessor.AddObjectByName(pObject, "Components", -1, pType, guid).Failed())
      return;
  }

  Setter set;
  set.m_pManager = pManager;
  set.m_pComponent = pManager->GetObject(componentGuid);
  if (set.m_pComponent == nullptr)
    return;

  if (prim.m_Kind == plUsdPrimKind::Camera)
    ApplyCamera(set, prim.m_Camera);
  else
    ApplyLight(set, prim.m_Light);
}

namespace
{
  /// Properties a component carries in USD; see plUsdPrimComponents.
  bool IsCarriedProperty(const plAbstractProperty* pProperty)
  {
    if (pProperty->GetCategory() != plPropertyCategory::Member || pProperty->GetFlags().IsSet(plPropertyFlags::ReadOnly))
      return false;

    return pProperty->GetFlags().IsAnySet(plPropertyFlags::StandardType | plPropertyFlags::IsEnum | plPropertyFlags::Bitflags);
  }

  /// A document value as USD carries it, or an invalid variant for values that don't travel.
  plVariant ToCarriedValue(const plAbstractProperty* pProperty, const plVariant& value)
  {
    if (pProperty->GetFlags().IsAnySet(plPropertyFlags::IsEnum | plPropertyFlags::Bitflags))
    {
      plStringBuilder sName;
      if (plReflectionUtils::EnumerationToString(pProperty->GetSpecificType(), value.ConvertTo<plInt64>(), sName, plReflectionUtils::EnumConversionMode::ValueNameOnly))
        return plString(sName);
      return {};
    }

    switch (value.GetType())
    {
      case plVariantType::Angle:
        return value.Get<plAngle>().GetDegree();
      case plVariantType::Time:
        return value.Get<plTime>().GetSeconds();
      case plVariantType::ColorGamma:
        return plColor(value.Get<plColorGammaUB>());
      case plVariantType::HashedString:
      case plVariantType::TempHashedString:
        return value.ConvertTo<plString>();
      case plVariantType::Uuid:
      case plVariantType::Matrix3:
      case plVariantType::Matrix4:
      case plVariantType::Transform:
      case plVariantType::DataBuffer:
        return {};
      default:
        return value;
    }
  }

  /// The reverse of ToCarriedValue: a USD value as the document stores the property.
  plVariant FromCarriedValue(const plAbstractProperty* pProperty, const plVariant& value)
  {
    if (pProperty->GetFlags().IsAnySet(plPropertyFlags::IsEnum | plPropertyFlags::Bitflags))
    {
      plInt64 iValue = 0;
      if (value.IsA<plString>() && plReflectionUtils::StringToEnumeration(pProperty->GetSpecificType(), value.Get<plString>(), iValue))
        return iValue;
      return {};
    }

    const plVariantType::Enum type = pProperty->GetSpecificType()->GetVariantType();
    switch (type)
    {
      case plVariantType::Angle:
        return plAngle::MakeFromDegree(value.ConvertTo<float>());
      case plVariantType::Time:
        return plTime::MakeFromSeconds(value.ConvertTo<double>());
      case plVariantType::ColorGamma:
        return plColorGammaUB(value.ConvertTo<plColor>());
      default:
        return value.CanConvertTo(type) ? value.ConvertTo(type) : plVariant();
    }
  }

  const plUsdComponent* FindComponent(plArrayPtr<const plUsdComponent> components, plStringView sType)
  {
    for (const plUsdComponent& component : components)
    {
      if (component.m_sType == sType)
        return &component;
    }
    return nullptr;
  }

  const plUsdComponentValue* FindValue(const plUsdComponent* pComponent, plStringView sProperty)
  {
    if (pComponent != nullptr)
    {
      for (const plUsdComponentValue& value : pComponent->m_Values)
      {
        if (value.m_sProperty == sProperty)
          return &value;
      }
    }
    return nullptr;
  }

  /// The object's component of a type that is not one of the generated ones.
  const plDocumentObject* FindUserComponent(const plDocumentObject* pObject, plArrayPtr<const plUuid> generated, plStringView sType)
  {
    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      if (pChild->GetParentProperty() == "Components" && !generated.Contains(pChild->GetGuid()) && pChild->GetType()->GetTypeName() == sType)
        return pChild;
    }
    return nullptr;
  }
} // namespace

void plUsdPrimComponents::ReadUserComponents(const plDocumentObject* pObject, plArrayPtr<const plUuid> generated, plArrayPtr<const plUsdComponent> stage,
  plDynamicArray<plUsdComponent>& out_components)
{
  out_components.Clear();

  for (const plDocumentObject* pChild : pObject->GetChildren())
  {
    if (pChild->GetParentProperty() != "Components" || generated.Contains(pChild->GetGuid()))
      continue;

    // One component of each type travels; see plUsdComponent.
    const plRTTI* pType = pChild->GetType();
    if (FindComponent(out_components, pType->GetTypeName()) != nullptr)
      continue;

    const plUsdComponent* pStage = FindComponent(stage, pType->GetTypeName());

    plUsdComponent& component = out_components.ExpandAndGetRef();
    component.m_sType = pType->GetTypeName();

    plHybridArray<const plAbstractProperty*, 32> properties;
    pType->GetAllProperties(properties);

    for (const plAbstractProperty* pProperty : properties)
    {
      if (!IsCarriedProperty(pProperty))
        continue;

      const plVariant value = pChild->GetTypeAccessor().GetValue(pProperty->GetPropertyName());
      if (FindValue(pStage, pProperty->GetPropertyName()) == nullptr && value == plReflectionUtils::GetDefaultValue(pProperty))
        continue;

      plUsdComponentValue& entry = component.m_Values.ExpandAndGetRef();
      entry.m_sProperty = pProperty->GetPropertyName();
      entry.m_Value = ToCarriedValue(pProperty, value);

      if (!entry.m_Value.IsValid())
        component.m_Values.PopBack();
    }
  }
}

void plUsdPrimComponents::ApplyUserComponents(plDocumentObjectManager* pManager, const plDocumentObject* pObject, plArrayPtr<const plUsdComponent> components,
  plArrayPtr<const plUsdComponent> previous, plArrayPtr<const plUuid> generated, plStringView sGuidSeed)
{
  plObjectDirectAccessor accessor(pManager);

  for (const plUsdComponent& old : previous)
  {
    if (FindComponent(components, old.m_sType) != nullptr)
      continue;

    if (const plDocumentObject* pComponent = FindUserComponent(pObject, generated, old.m_sType))
      accessor.RemoveObject(pComponent).IgnoreResult();
  }

  for (const plUsdComponent& component : components)
  {
    const plRTTI* pType = plRTTI::FindTypeByName(component.m_sType);
    if (pType == nullptr || !pType->IsDerivedFrom<plComponent>())
    {
      plLog::Warning("USD Live: '{}' is not a component type this editor knows; is the plugin that defines it loaded?", component.m_sType);
      continue;
    }

    const plDocumentObject* pExisting = FindUserComponent(pObject, generated, component.m_sType);
    if (pExisting == nullptr)
    {
      plUuid guid = plUuid::MakeStableUuidFromString(plStringBuilder(sGuidSeed, "#plasma:", component.m_sType));
      if (accessor.AddObjectByName(pObject, "Components", -1, pType, guid).Failed())
        continue;

      pExisting = pManager->GetObject(guid);
    }

    plDocumentObject* pComponent = pManager->GetObject(pExisting->GetGuid());
    const plUsdComponent* pPrevious = FindComponent(previous, component.m_sType);

    plHybridArray<const plAbstractProperty*, 32> properties;
    pType->GetAllProperties(properties);

    for (const plAbstractProperty* pProperty : properties)
    {
      if (!IsCarriedProperty(pProperty))
        continue;

      plVariant value;
      if (const plUsdComponentValue* pValue = FindValue(&component, pProperty->GetPropertyName()))
        value = FromCarriedValue(pProperty, pValue->m_Value);
      else if (FindValue(pPrevious, pProperty->GetPropertyName()) != nullptr)
        value = plReflectionUtils::GetDefaultValue(pProperty);

      if (value.IsValid() && pComponent->GetTypeAccessor().GetValue(pProperty->GetPropertyName()) != value)
        pManager->SetValue(pComponent, pProperty->GetPropertyName(), value).IgnoreResult();
    }
  }
}

void plUsdPrimComponents::ApplyInstances(plDocumentObjectManager* pManager, const plDocumentObject* pComponent, plStringView sMeshResource, const plColor& color,
  plArrayPtr<const plTransform> instances, plStringView sGuidSeed)
{
  plDocumentObject* pTarget = pManager->GetObject(pComponent->GetGuid());
  if (pTarget == nullptr)
    return;

  // Every value set is a message to the engine, so unchanged ones are skipped.
  auto set = [&](plDocumentObject* pObject, plStringView sProperty, const plVariant& value)
  {
    if (pObject->GetTypeAccessor().GetValue(sProperty) != value)
      pManager->SetValue(pObject, sProperty, value).IgnoreResult();
  };

  set(pTarget, "Mesh", plString(sMeshResource));
  set(pTarget, "MainColor", color);

  plObjectDirectAccessor accessor(pManager);
  const plIReflectedTypeAccessor& data = pTarget->GetTypeAccessor();

  for (plInt32 iCount = data.GetCount("InstanceData"); iCount > (plInt32)instances.GetCount(); --iCount)
  {
    const plDocumentObject* pElement = pManager->GetObject(data.GetValue("InstanceData", iCount - 1).ConvertTo<plUuid>());
    if (pElement == nullptr || accessor.RemoveObject(pElement).Failed())
      break;
  }

  const plRTTI* pElementType = plGetStaticRTTI<plMeshInstanceData>();
  const plUInt32 uiExisting = (plUInt32)data.GetCount("InstanceData");

  for (plUInt32 i = 0; i < instances.GetCount(); ++i)
  {
    plUuid elementGuid;
    if (i < uiExisting)
    {
      elementGuid = data.GetValue("InstanceData", i).ConvertTo<plUuid>();
    }
    else
    {
      plStringBuilder sSeed;
      sSeed.SetFormat("{}#{}", sGuidSeed, i);
      elementGuid = plUuid::MakeStableUuidFromString(sSeed);

      if (accessor.AddObjectByName(pTarget, "InstanceData", -1, pElementType, elementGuid).Failed())
        break;
    }

    plDocumentObject* pElement = pManager->GetObject(elementGuid);
    if (pElement == nullptr)
      continue;

    const plTransform& t = instances[i];
    set(pElement, "LocalPosition", t.m_vPosition);
    set(pElement, "LocalRotation", t.m_qRotation);
    set(pElement, "LocalScaling", t.m_vScale);
  }
}
