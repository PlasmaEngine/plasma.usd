#include <UsdBridge/UsdBridgePCH.h>

#include <Foundation/Threading/Mutex.h>
#include <Foundation/Types/Status.h>
#include <ModelImporter2/Importer/MaterialGraph.h>
#include <UsdBridge/UsdMtlxReader.h>

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>

namespace
{
  using plModelImporter2::MaterialGraphDesc;
  using plModelImporter2::OpenPBRAlphaMode;
  using plModelImporter2::OpenPBRColorInput;
  using plModelImporter2::OpenPBRMaterialDesc;
  using plModelImporter2::OpenPBRScalarInput;
  using plModelImporter2::TextureChannel;

  /// plasma.materialx's editor plugin. The editor loads a package's plugin from a copy in a numbered folder, and the copy
  /// keeps this name, so the module is found whichever copy is loaded.
  constexpr const wchar_t* MaterialXModule = L"plEditorPluginMaterialX.dll";

  // plMaterialXGraphReader::ReadDocument and plMaterialXGraphReader::GetStandardLibraryPath, by their MSVC-decorated
  // names. The decoration spells out every parameter type and the return type, so a plasma.materialx whose functions have
  // another signature fails the lookup instead of being called with the wrong arguments.
  constexpr const char* ReadDocumentSymbol =
    "?ReadDocument@plMaterialXGraphReader@@YA?AUplStatus@@VplStringView@@0AEAV?$plDynamicArray@UMaterialGraphDesc@plModelImporter2@@"
    "UplDefaultAllocatorWrapper@@@@@Z";
  constexpr const char* GetStandardLibraryPathSymbol =
    "?GetStandardLibraryPath@plMaterialXGraphReader@@YA?AU?$plHybridString@$0CA@UplDefaultAllocatorWrapper@@@@PEAV?$plDynamicArray@"
    "U?$plHybridString@$0CA@UplDefaultAllocatorWrapper@@@@UplDefaultAllocatorWrapper@@@@@Z";

  using ReadDocumentFunc = plStatus (*)(plStringView, plStringView, plDynamicArray<MaterialGraphDesc>&);
  using GetStandardLibraryPathFunc = plString (*)(plDynamicArray<plString>*);

  plMutex s_ReadMutex;

  /// Set once a problem that holds for the whole process has been logged. Guarded by s_ReadMutex.
  bool s_bReportedUnavailable = false;

  void ReportOnce(const plFormatString& message)
  {
    if (s_bReportedUnavailable)
      return;

    s_bReportedUnavailable = true;

    plStringBuilder sTemp;
    plLog::Warning("{}", message.GetText(sTemp));
  }

  /// A reference on plasma.materialx's module for as long as its functions are being called.
  class MaterialXModuleReference
  {
  public:
    MaterialXModuleReference()
    {
      // No flags: finding the module adds a reference to it, which FreeLibrary drops again.
      if (GetModuleHandleExW(0, MaterialXModule, &m_hModule) == 0)
        m_hModule = nullptr;
    }

    ~MaterialXModuleReference()
    {
      if (m_hModule != nullptr)
        FreeLibrary(m_hModule);
    }

    HMODULE m_hModule = nullptr;
  };

  plString MakeAbsolute(plStringView sFile, plStringView sDocumentDir)
  {
    plStringBuilder sPath = sFile;
    sPath.MakeCleanPath();

    if (!sPath.IsEmpty() && !sPath.IsAbsolutePath())
    {
      plStringBuilder sAbsolute = sDocumentDir;
      sAbsolute.AppendPath(sPath);
      sAbsolute.MakeCleanPath();
      return sAbsolute;
    }

    return sPath;
  }

  bool IsGrey(const plColor& c)
  {
    return c.r == c.g && c.g == c.b;
  }

  const char* GetChannelName(TextureChannel channel)
  {
    switch (channel)
    {
      case TextureChannel::Red:
        return "r";
      case TextureChannel::Green:
        return "g";
      case TextureChannel::Blue:
        return "b";
      case TextureChannel::Alpha:
        return "a";
    }
    return "?";
  }

  /// Writes the inputs of one open_pbr_surface and collects what the network cannot carry.
  struct Conversion
  {
    plUsdMtlxMaterial& m_Material;
    plStringView m_sDocumentDir;
    plDynamicArray<plString>& m_Lost;

    void Lose(const plFormatString& text)
    {
      plStringBuilder sTemp;
      m_Lost.PushBack(text.GetText(sTemp));
    }

    plUsdMtlxInput& Add(const char* szName, plUsdMtlxValueType type)
    {
      plUsdMtlxInput& input = m_Material.m_Inputs.ExpandAndGetRef();
      input.m_sName = szName;
      input.m_Type = type;
      return input;
    }

    void Scalar(const char* szName, const OpenPBRScalarInput& source)
    {
      plUsdMtlxInput& input = Add(szName, plUsdMtlxValueType::Float);

      if (!source.HasTexture())
      {
        input.m_Value = source.m_fValue;
        return;
      }

      input.m_sImageFile = MakeAbsolute(source.m_sTexturePath, m_sDocumentDir);

      // A float image node samples the first channel of its file, and a connected input has no constant to scale it by.
      if (source.m_Channel != TextureChannel::Red)
        Lose(plFmt("'{}' reads channel {} of its image; the image node reads the first", szName, GetChannelName(source.m_Channel)));

      if (source.m_fValue != 1.0f)
        Lose(plFmt("'{}' scales its image by {}", szName, source.m_fValue));
    }

    /// \a bScaleMoved: the grey scale on an image-driven colour has been moved onto the weight that multiplies it.
    void Color(const char* szName, const OpenPBRColorInput& source, bool bScaleMoved = false)
    {
      plUsdMtlxInput& input = Add(szName, plUsdMtlxValueType::Color3);

      if (!source.HasTexture())
      {
        input.m_Value = plColor(source.m_Value.r, source.m_Value.g, source.m_Value.b);
        return;
      }

      input.m_sImageFile = MakeAbsolute(source.m_sTexturePath, m_sDocumentDir);

      const plColor& c = source.m_Value;
      const bool bWhite = c.r == 1.0f && c.g == 1.0f && c.b == 1.0f;

      if (!bWhite && !(bScaleMoved && IsGrey(c)))
        Lose(plFmt("'{}' tints its image by ({}, {}, {})", szName, c.r, c.g, c.b));
    }

    void Float(const char* szName, float fValue)
    {
      Add(szName, plUsdMtlxValueType::Float).m_Value = fValue;
    }

    void ColorConstant(const char* szName, const plColor& value)
    {
      Add(szName, plUsdMtlxValueType::Color3).m_Value = plColor(value.r, value.g, value.b);
    }

    void Bool(const char* szName, bool bValue)
    {
      Add(szName, plUsdMtlxValueType::Bool).m_Value = bValue;
    }

    void Normal(const char* szName, plStringView sImage)
    {
      if (!sImage.IsEmpty())
        Add(szName, plUsdMtlxValueType::Vector3).m_sImageFile = MakeAbsolute(sImage, m_sDocumentDir);
    }
  };

  /// Every input of open_pbr_surface that OpenPBRMaterialDesc has a value for, in the nodedef's order.
  void ConvertOpenPBR(const OpenPBRMaterialDesc& pbr, Conversion& ref_c)
  {
    // A connected input has no constant of its own. The resolver keeps the scale an image is multiplied by as a grey
    // constant beside it; for the base and emission colours that scale moves onto the weight that multiplies the colour.
    OpenPBRScalarInput baseWeight = pbr.m_BaseWeight;
    const bool bBaseScaleMoved = pbr.m_BaseColor.HasTexture() && !baseWeight.HasTexture() && IsGrey(pbr.m_BaseColor.m_Value);
    if (bBaseScaleMoved)
      baseWeight.m_fValue *= pbr.m_BaseColor.m_Value.r;

    float fEmissionLuminance = pbr.m_fEmissionLuminance;
    const bool bEmissionScaleMoved = pbr.m_EmissionColor.HasTexture() && IsGrey(pbr.m_EmissionColor.m_Value);
    if (bEmissionScaleMoved)
      fEmissionLuminance *= pbr.m_EmissionColor.m_Value.r;

    ref_c.Scalar("base_weight", baseWeight);
    ref_c.Color("base_color", pbr.m_BaseColor, bBaseScaleMoved);
    ref_c.Scalar("base_metalness", pbr.m_BaseMetalness);
    ref_c.Float("base_diffuse_roughness", pbr.m_fBaseDiffuseRoughness);

    ref_c.Scalar("specular_weight", pbr.m_SpecularWeight);
    ref_c.Color("specular_color", pbr.m_SpecularColor);
    ref_c.Scalar("specular_roughness", pbr.m_SpecularRoughness);
    ref_c.Float("specular_ior", pbr.m_fSpecularIor);
    ref_c.Float("specular_roughness_anisotropy", pbr.m_fSpecularRoughnessAnisotropy);

    ref_c.Scalar("coat_weight", pbr.m_CoatWeight);
    ref_c.Color("coat_color", pbr.m_CoatColor);
    ref_c.Scalar("coat_roughness", pbr.m_CoatRoughness);
    ref_c.Float("coat_roughness_anisotropy", pbr.m_fCoatRoughnessAnisotropy);
    ref_c.Float("coat_ior", pbr.m_fCoatIor);
    ref_c.Float("coat_darkening", pbr.m_fCoatDarkening);

    ref_c.Scalar("fuzz_weight", pbr.m_FuzzWeight);
    ref_c.Color("fuzz_color", pbr.m_FuzzColor);
    ref_c.Scalar("fuzz_roughness", pbr.m_FuzzRoughness);

    ref_c.Scalar("subsurface_weight", pbr.m_SubsurfaceWeight);
    ref_c.Color("subsurface_color", pbr.m_SubsurfaceColor);
    ref_c.Float("subsurface_radius", pbr.m_fSubsurfaceRadius);
    ref_c.ColorConstant("subsurface_radius_scale", pbr.m_SubsurfaceRadiusScale);
    ref_c.Float("subsurface_scatter_anisotropy", pbr.m_fSubsurfaceScatterAnisotropy);

    ref_c.Scalar("transmission_weight", pbr.m_TransmissionWeight);
    ref_c.Color("transmission_color", pbr.m_TransmissionColor);
    ref_c.Float("transmission_depth", pbr.m_fTransmissionDepth);
    ref_c.ColorConstant("transmission_scatter", pbr.m_TransmissionScatter);
    ref_c.Float("transmission_scatter_anisotropy", pbr.m_fTransmissionScatterAnisotropy);
    ref_c.Float("transmission_dispersion_scale", pbr.m_fTransmissionDispersionScale);
    ref_c.Float("transmission_dispersion_abbe_number", pbr.m_fTransmissionDispersionAbbeNumber);

    ref_c.Scalar("thin_film_weight", pbr.m_ThinFilmWeight);
    ref_c.Float("thin_film_thickness", pbr.m_fThinFilmThickness);
    ref_c.Float("thin_film_ior", pbr.m_fThinFilmIor);

    ref_c.Float("emission_luminance", fEmissionLuminance);
    ref_c.Color("emission_color", pbr.m_EmissionColor, bEmissionScaleMoved);

    ref_c.Scalar("geometry_opacity", pbr.m_GeometryOpacity);
    ref_c.Bool("geometry_thin_walled", pbr.m_bThinWalled);
    ref_c.Normal("geometry_normal", pbr.m_sNormalTexture);
    ref_c.Normal("geometry_coat_normal", pbr.m_sCoatNormalTexture);

    // What open_pbr_surface has no input for.
    if (pbr.m_fSpecularAnisotropyRotation != 0.0f || pbr.m_fCoatAnisotropyRotation != 0.0f)
      ref_c.Lose("anisotropy rotation (open_pbr_surface takes a tangent instead)");

    if (pbr.m_Occlusion.HasTexture() || pbr.m_Occlusion.m_fValue != 1.0f)
      ref_c.Lose("occlusion");

    if (!pbr.m_sDisplacementTexture.IsEmpty())
      ref_c.Lose("displacement");

    if (pbr.m_AlphaMode == OpenPBRAlphaMode::Mask && pbr.m_fAlphaCutoff != 0.5f)
      ref_c.Lose(plFmt("alpha cutoff {} (image-driven opacity is cut at 0.5)", pbr.m_fAlphaCutoff));
  }
} // namespace

plResult plUsdMtlxReader::ReadDocument(plStringView sAbsFile, plDynamicArray<plUsdMtlxMaterial>& out_materials)
{
  PL_LOCK(s_ReadMutex);

  MaterialXModuleReference module;
  if (module.m_hModule == nullptr)
  {
    ReportOnce(plFmt("'{}' is a MaterialX document. OpenUSD reads .mtlx sublayers and references through the plasma.materialx "
                     "package, which is not loaded: install and enable plasma.materialx to use them. Other .mtlx files fail the "
                     "same way without another warning.",
      sAbsFile));
    return PL_FAILURE;
  }

  const auto readDocument = reinterpret_cast<ReadDocumentFunc>(GetProcAddress(module.m_hModule, ReadDocumentSymbol));
  const auto getStandardLibraryPath = reinterpret_cast<GetStandardLibraryPathFunc>(GetProcAddress(module.m_hModule, GetStandardLibraryPathSymbol));

  if (readDocument == nullptr || getStandardLibraryPath == nullptr)
  {
    ReportOnce(plFmt("'{}' is a MaterialX document. plasma.materialx is loaded, but not a build that exports the reader plasma.usd "
                     "calls (plMaterialXGraphReader::ReadDocument and GetStandardLibraryPath with their plasma.materialx 0.1.7 "
                     "signatures): update the two packages together. Other .mtlx files fail the same way without another warning.",
      sAbsFile));
    return PL_FAILURE;
  }

  // Without the standard library a document's nodedefs do not resolve; plasma.materialx refuses to read in that case.
  plDynamicArray<plString> searchedPaths;
  const plString sLibraryPath = getStandardLibraryPath(&searchedPaths);

  if (sLibraryPath.IsEmpty())
  {
    plStringBuilder sTried;
    for (const plString& sPath : searchedPaths)
      sTried.AppendFormat("\n  {}", sPath);

    ReportOnce(plFmt("'{}' is a MaterialX document, but plasma.materialx cannot find the MaterialX standard library, so no .mtlx "
                     "file can be read. Looked in:{}",
      sAbsFile, sTried));
    return PL_FAILURE;
  }

  plDynamicArray<MaterialGraphDesc> graphs;

  if (const plStatus res = readDocument(sAbsFile, sLibraryPath, graphs); res.Failed())
  {
    plLog::Warning("MaterialX: {}", res.m_sMessage);
    return PL_FAILURE;
  }

  plStringBuilder sDocumentDir = sAbsFile;
  sDocumentDir.PathParentDirectory();

  for (const MaterialGraphDesc& graph : graphs)
  {
    OpenPBRMaterialDesc pbr;
    plModelImporter2::MaterialGraphResolveReport report;

    // The resolver logs the surface shader it did not recognise.
    if (plModelImporter2::ResolveMaterialGraph(graph, pbr, &report).Failed())
      continue;

    plUsdMtlxMaterial& material = out_materials.ExpandAndGetRef();
    material.m_sName = graph.m_sName;
    material.m_sShaderName = graph.m_sSurfaceNode;
    material.m_sShaderId = "ND_open_pbr_surface_surfaceshader";
    material.m_sSourceShader = report.m_sMatchedSurface;

    plDynamicArray<plString> lost;
    for (const plString& sEntry : report.m_UnsupportedInputs)
      lost.PushBack(sEntry);

    for (const plString& sInput : report.m_UnmappedInputs)
    {
      plStringBuilder sEntry;
      sEntry.SetFormat("'{}' (no OpenPBR equivalent)", sInput);
      lost.PushBack(sEntry);
    }

    Conversion conversion{material, sDocumentDir, lost};
    ConvertOpenPBR(pbr, conversion);

    // A material that quietly lost an input looks like a shading bug; this is the only record of why.
    if (!lost.IsEmpty())
    {
      plStringBuilder sLost;
      for (const plString& sEntry : lost)
        sLost.AppendFormat("{}{}", sLost.IsEmpty() ? "" : "; ", sEntry);

      plLog::Warning("MaterialX material '{}' in '{}' reads into USD without: {}", graph.m_sName, sAbsFile, sLost);
    }
  }

  if (out_materials.IsEmpty())
  {
    plLog::Warning("MaterialX: '{}' has no material whose surface shader plasma.usd can read.", sAbsFile);
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}
