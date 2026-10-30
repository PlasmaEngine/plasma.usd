#pragma once

#include <Foundation/Strings/StringBuilder.h>
#include <Foundation/Types/Status.h>

/// \brief Lets OpenUSD open MaterialX documents (.mtlx) as layers, so stages can sublayer and reference them.
///
/// The OpenUSD this package ships is built without its MaterialX plugin. Documents are read through plasma.materialx
/// instead (see plUsdMtlxReader), so the process holds one MaterialX, not two.
///
/// A .mtlx layer has the layout of OpenUSD's own usdMtlx, so references written for other tools resolve here too:
///
///   /MaterialX/Materials/<material>                   Material, outputs:mtlx:surface and outputs:surface -> the shader
///   /MaterialX/Materials/<material>/<shader>          Shader, info:id "ND_open_pbr_surface_surfaceshader"
///   /MaterialX/Materials/<material>/<input>_image     Shader, info:id "ND_image_<type>", inputs:file = absolute path
///
/// Every node lives under its Material, so referencing @x.mtlx@</MaterialX/Materials/<material>> brings the whole
/// network. The layer's default prim is /MaterialX. Every material is an open_pbr_surface, whatever surface shader the
/// document used; plUsdMtlxReader describes what that loses.
///
/// Read-only: saving or exporting a .mtlx layer fails.
namespace plUsdMtlx
{
  /// \brief Defines the file format that plasmaMtlx/resources/plugInfo.json declares, staged into plugin/usd/plasmaMtlx.
  ///
  /// The plugInfo is a resource plugin: it names the type, its "mtlx" extension and "usd" target, and no library, so
  /// OpenUSD never loads a DLL for it. This gives the type its C++ class and factory, in whichever module this code is
  /// compiled into. OpenUSD keeps both, and the file format instance it creates, for the life of the process; plugins
  /// are only unloaded at shutdown, after the last stage has closed.
  ///
  /// Call before a stage opens a .mtlx. The plugInfo has to be registered before OpenUSD first looks up a file format,
  /// which is when it reads every format's declaration. Safe to call more than once.
  void RegisterFileFormat();

  /// \brief Opens a .mtlx through the registered file format and returns the layer it produces as .usda text.
  plStatus ReadAsUsda(plStringView sAbsFile, plStringBuilder& out_sUsda);

} // namespace plUsdMtlx
