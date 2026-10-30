#include <UsdBridge/UsdBridgePCH.h>

#include <UsdBridge/UsdMtlxFileFormat.h>
#include <UsdBridge/UsdMtlxReader.h>

#pragma warning(push, 0)
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/usdaFileFormat.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#pragma warning(pop)

#include <set>

/// \brief The OpenUSD file format for .mtlx.
///
/// OpenUSD names a TfType after its C++ class, and "plUsdMtlxFileFormat" is the key plasmaMtlx/resources/plugInfo.json declares,
/// so the class stays outside any namespace. The constructor is public for pxr::Sdf_FileFormatFactory.
class plUsdMtlxFileFormat : public pxr::SdfFileFormat
{
public:
  plUsdMtlxFileFormat();
  ~plUsdMtlxFileFormat() override;

  bool CanRead(const std::string& sFile) const override;
  bool Read(pxr::SdfLayer* pLayer, const std::string& sResolvedPath, bool bMetadataOnly) const override;
  bool WriteToFile(const pxr::SdfLayer& layer, const std::string& sFilePath, const std::string& sComment, const FileFormatArguments& args) const override;
  bool ReadFromString(pxr::SdfLayer* pLayer, const std::string& sText) const override;
  bool WriteToString(const pxr::SdfLayer& layer, std::string* out_pText, const std::string& sComment) const override;
  bool WriteToStream(const pxr::SdfSpecHandle& spec, std::ostream& ref_out, size_t uiIndent) const override;
};

namespace
{
  constexpr const char* FileFormatTypeName = "plUsdMtlxFileFormat";

  std::string ToStd(plStringView s)
  {
    return std::string(s.GetStartPointer(), s.GetElementCount());
  }

  /// Posts an OpenUSD error, which is how Sdf expects a file format to report a failure.
  void PostError(const std::string& sMessage)
  {
    using namespace pxr;
    TF_RUNTIME_ERROR("%s", sMessage.c_str());
  }

  /// A valid prim name for sName that no name in ref_used has taken yet.
  pxr::TfToken MakeUniqueName(plStringView sName, std::set<std::string>& ref_used)
  {
    const std::string sBase = pxr::TfMakeValidIdentifier(ToStd(sName));
    std::string sResult = sBase;

    for (int i = 1; !ref_used.insert(sResult).second; ++i)
      sResult = sBase + "_" + std::to_string(i);

    return pxr::TfToken(sResult);
  }

  /// The types usdMtlx gives MaterialX's float, color3, vector3 and boolean.
  const pxr::SdfValueTypeName& GetUsdType(plUsdMtlxValueType type)
  {
    switch (type)
    {
      case plUsdMtlxValueType::Float:
        return pxr::SdfValueTypeNames->Float;
      case plUsdMtlxValueType::Color3:
        return pxr::SdfValueTypeNames->Color3f;
      case plUsdMtlxValueType::Vector3:
        return pxr::SdfValueTypeNames->Float3;
      case plUsdMtlxValueType::Bool:
        return pxr::SdfValueTypeNames->Bool;
    }
    return pxr::SdfValueTypeNames->Float;
  }

  const char* GetImageNodeId(plUsdMtlxValueType type)
  {
    switch (type)
    {
      case plUsdMtlxValueType::Color3:
        return "ND_image_color3";
      case plUsdMtlxValueType::Vector3:
        return "ND_image_vector3";
      default:
        return "ND_image_float";
    }
  }

  pxr::VtValue ToVtValue(const plUsdMtlxInput& input)
  {
    switch (input.m_Type)
    {
      case plUsdMtlxValueType::Float:
        if (input.m_Value.IsNumber())
          return pxr::VtValue(input.m_Value.ConvertTo<float>());
        break;
      case plUsdMtlxValueType::Color3:
        if (input.m_Value.IsA<plColor>())
        {
          const plColor c = input.m_Value.Get<plColor>();
          return pxr::VtValue(pxr::GfVec3f(c.r, c.g, c.b));
        }
        break;
      case plUsdMtlxValueType::Bool:
        if (input.m_Value.IsA<bool>())
          return pxr::VtValue(input.m_Value.Get<bool>());
        break;
      case plUsdMtlxValueType::Vector3:
        break;
    }
    return {};
  }

  void WriteMaterials(const pxr::UsdStageRefPtr& stage, plStringView sSourceFile, const plDynamicArray<plUsdMtlxMaterial>& materials)
  {
    const pxr::SdfPath materialsPath("/MaterialX/Materials");
    std::set<std::string> usedMaterialNames;

    for (const plUsdMtlxMaterial& source : materials)
    {
      const pxr::SdfPath materialPath = materialsPath.AppendChild(MakeUniqueName(source.m_sName, usedMaterialNames));
      pxr::UsdShadeMaterial material = pxr::UsdShadeMaterial::Define(stage, materialPath);

      plStringBuilder sDoc;
      sDoc.SetFormat("MaterialX {} from '{}', read as open_pbr_surface through plasma.materialx", source.m_sSourceShader, sSourceFile);
      material.GetPrim().SetDocumentation(ToStd(sDoc));

      std::set<std::string> usedNodeNames;

      pxr::UsdShadeShader shader = pxr::UsdShadeShader::Define(stage, materialPath.AppendChild(MakeUniqueName(source.m_sShaderName, usedNodeNames)));
      shader.CreateIdAttr(pxr::VtValue(pxr::TfToken(ToStd(source.m_sShaderId))));

      // outputs:mtlx:surface is usdMtlx's output; outputs:surface is there too because ComputeSurfaceSource stops at an
      // unauthored universal output when the universal context is listed before "mtlx", the order plUsdStage asks in.
      const pxr::UsdShadeOutput shaderSurface = shader.CreateOutput(pxr::UsdShadeTokens->surface, pxr::SdfValueTypeNames->Token);
      material.CreateSurfaceOutput(pxr::TfToken("mtlx")).ConnectToSource(shaderSurface);
      material.CreateSurfaceOutput().ConnectToSource(shaderSurface);

      for (const plUsdMtlxInput& sourceInput : source.m_Inputs)
      {
        const pxr::SdfValueTypeName& type = GetUsdType(sourceInput.m_Type);
        pxr::UsdShadeInput input = shader.CreateInput(pxr::TfToken(ToStd(sourceInput.m_sName)), type);

        if (sourceInput.m_sImageFile.IsEmpty())
        {
          if (const pxr::VtValue value = ToVtValue(sourceInput); !value.IsEmpty())
            input.Set(value);

          continue;
        }

        // Each image connects straight to the input it drives: the one connection plUsdStage follows to a texture. For a
        // normal map that is UsdPreviewSurface's convention, a tangent-space image on the normal input, where MaterialX
        // puts a normalmap node in between.
        const plStringBuilder sImageName(sourceInput.m_sName, "_image");
        pxr::UsdShadeShader image = pxr::UsdShadeShader::Define(stage, materialPath.AppendChild(MakeUniqueName(sImageName, usedNodeNames)));
        image.CreateIdAttr(pxr::VtValue(pxr::TfToken(GetImageNodeId(sourceInput.m_Type))));
        image.CreateInput(pxr::TfToken("file"), pxr::SdfValueTypeNames->Asset).Set(pxr::SdfAssetPath(ToStd(sourceInput.m_sImageFile)));
        input.ConnectToSource(image.CreateOutput(pxr::TfToken("out"), type));
      }
    }

    if (const pxr::UsdPrim root = stage->GetPrimAtPath(pxr::SdfPath("/MaterialX")))
      stage->SetDefaultPrim(root);

    plStringBuilder sLayerDoc;
    sLayerDoc.SetFormat("Read from '{}' through plasma.materialx", sSourceFile);
    stage->GetRootLayer()->SetDocumentation(ToStd(sLayerDoc));
  }
} // namespace

plUsdMtlxFileFormat::plUsdMtlxFileFormat()
  : pxr::SdfFileFormat(pxr::TfToken("mtlx"), pxr::TfToken("1.0"), pxr::TfToken("usd"), "mtlx")
{
}

plUsdMtlxFileFormat::~plUsdMtlxFileFormat() = default;

bool plUsdMtlxFileFormat::CanRead(const std::string& sFile) const
{
  return pxr::TfStringToLowerAscii(pxr::TfGetExtension(sFile)) == "mtlx";
}

bool plUsdMtlxFileFormat::Read(pxr::SdfLayer* pLayer, const std::string& sResolvedPath, bool bMetadataOnly) const
{
  // The whole document is read even when only metadata is asked for; documents are small.
  PL_IGNORE_UNUSED(bMetadataOnly);

  plStringBuilder sFile = plStringView(sResolvedPath.data(), (plUInt32)sResolvedPath.size());
  sFile.MakeCleanPath();

  // The reader logs why it failed; OpenUSD then reports the layer as one it could not open.
  plDynamicArray<plUsdMtlxMaterial> materials;
  if (plUsdMtlxReader::ReadDocument(sFile, materials).Failed())
    return false;

  // Authored on a stage of its own and moved into the layer in one step, as usdMtlx does.
  pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory();
  WriteMaterials(stage, sFile, materials);

  pLayer->TransferContent(stage->GetRootLayer());
  return true;
}

bool plUsdMtlxFileFormat::WriteToFile(const pxr::SdfLayer& layer, const std::string& sFilePath, const std::string& sComment, const FileFormatArguments& args) const
{
  PL_IGNORE_UNUSED(layer);
  PL_IGNORE_UNUSED(sComment);
  PL_IGNORE_UNUSED(args);

  PostError("Cannot write '" + sFilePath + "': .mtlx layers are read-only in plasma.usd.");
  return false;
}

bool plUsdMtlxFileFormat::ReadFromString(pxr::SdfLayer* pLayer, const std::string& sText) const
{
  PL_IGNORE_UNUSED(pLayer);
  PL_IGNORE_UNUSED(sText);

  PostError("A .mtlx layer can only be read from a file: plasma.materialx reads documents from disk.");
  return false;
}

bool plUsdMtlxFileFormat::WriteToString(const pxr::SdfLayer& layer, std::string* out_pText, const std::string& sComment) const
{
  // The layer holds ordinary scene description, so it prints as .usda.
  return pxr::SdfFileFormat::FindById(pxr::SdfUsdaFileFormatTokens->Id)->WriteToString(layer, out_pText, sComment);
}

bool plUsdMtlxFileFormat::WriteToStream(const pxr::SdfSpecHandle& spec, std::ostream& ref_out, size_t uiIndent) const
{
  return pxr::SdfFileFormat::FindById(pxr::SdfUsdaFileFormatTokens->Id)->WriteToStream(spec, ref_out, uiIndent);
}

void plUsdMtlx::RegisterFileFormat()
{
  static bool s_bRegistered = false;
  if (s_bRegistered)
    return;

  s_bRegistered = true;

  // A TfType gets its C++ class and factory once per process and OpenUSD cannot take them back, so a type that already has
  // them keeps the ones it has.
  const pxr::TfType type = pxr::TfType::FindByName(FileFormatTypeName);
  if (!type.IsUnknown() && type.GetTypeid() != typeid(void))
    return;

  pxr::SdfDefineFileFormat<plUsdMtlxFileFormat, pxr::SdfFileFormat>();
}

plStatus plUsdMtlx::ReadAsUsda(plStringView sAbsFile, plStringBuilder& out_sUsda)
{
  pxr::TfErrorMark mark;
  const pxr::SdfLayerRefPtr layer = pxr::SdfLayer::FindOrOpen(ToStd(sAbsFile));

  std::string sErrors;
  for (auto it = mark.GetBegin(); it != mark.GetEnd(); ++it)
    sErrors += (sErrors.empty() ? "" : "; ") + it->GetCommentary();
  mark.Clear();

  if (!layer)
    return plStatus(plFmt("'{}' did not open as a layer{}{}", sAbsFile, sErrors.empty() ? "" : ": ", sErrors.c_str()));

  std::string sText;
  if (!layer->ExportToString(&sText))
    return plStatus(plFmt("'{}' opened but could not be printed as .usda", sAbsFile));

  out_sUsda = sText.c_str();
  return plStatus(PL_SUCCESS);
}
