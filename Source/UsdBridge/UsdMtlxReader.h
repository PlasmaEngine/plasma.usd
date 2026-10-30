#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/Variant.h>

/// \brief The MaterialX type of one converted shader input.
enum class plUsdMtlxValueType : plUInt8
{
  Float,
  Color3,
  Vector3, ///< Normals. Only ever driven by an image.
  Bool,
};

/// \brief One input of a converted surface shader: a constant, or an image that drives it.
struct plUsdMtlxInput
{
  plString m_sName; ///< The MaterialX input name, e.g. "base_color".
  plUsdMtlxValueType m_Type = plUsdMtlxValueType::Float;
  plVariant m_Value;    ///< float, plColor or bool, matching m_Type. Unused when m_sImageFile is set.
  plString m_sImageFile; ///< Absolute path of the image that drives the input, if one does.
};

/// \brief A MaterialX material as one surface shader with constant inputs, some of them driven by images.
struct plUsdMtlxMaterial
{
  plString m_sName;         ///< The MaterialX material's name.
  plString m_sShaderName;   ///< The name of its surface shader node.
  plString m_sShaderId;     ///< The nodedef the shader is written as: "ND_open_pbr_surface_surfaceshader".
  plString m_sSourceShader; ///< The surface shader the document used, e.g. "standard_surface".
  plDynamicArray<plUsdMtlxInput> m_Inputs;
};

/// \brief Reads MaterialX documents through the plasma.materialx package.
///
/// plasma.materialx is looked up among the modules already loaded in this process, by the name its editor plugin
/// has in every slot the editor loads it into. This package neither links it nor lists it as a dependency, so it
/// loads and works without it; only .mtlx files fail to read.
///
/// Each document is read with plMaterialXGraphReader and every material is lowered onto OpenPBR by
/// plModelImporter2::ResolveMaterialGraph: the same reading plasma.materialx uses when it imports a .mtlx as material
/// assets. The result is always an open_pbr_surface, whatever surface shader the document used.
namespace plUsdMtlxReader
{
  /// \brief Reads every material in a .mtlx file.
  ///
  /// Failures are logged. When plasma.materialx is not loaded, or is a build whose reader this one cannot call, that
  /// is logged once per process, naming the package; every later .mtlx fails the same way without another message.
  /// What a material loses on the way to OpenPBR and to the network the file format writes is logged per material.
  ///
  /// Safe to call from any thread; reads are serialised.
  plResult ReadDocument(plStringView sAbsFile, plDynamicArray<plUsdMtlxMaterial>& out_materials);

} // namespace plUsdMtlxReader
