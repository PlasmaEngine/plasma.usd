#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/Status.h>

class plImage;

/// \brief Turns a texture a USD material references into a DDS the engine can load.
///
/// There are two ways to do it. PrepareForPreview works in-process and quickly: reduced, uncompressed. The engine's
/// texture converter, TexConv, makes the full-resolution, block-compressed version a game ships; GetConversionArguments
/// builds its command line.
class plUsdTexture
{
public:
  /// \brief What a texture feeds, which decides its colour space, channels and compression.
  enum class Usage
  {
    Color,  ///< Base colour: sRGB, with an opacity mask in alpha.
    Normal, ///< Tangent-space normal map. Materials read x and y and rebuild z.
    Linear, ///< Roughness or metallic. Materials read the red channel.
  };

  /// \brief Decodes sColorFile (and optionally a mask whose first channel becomes alpha), reduces it until its longest
  /// edge is at most uiMaxSize, generates mipmaps and writes RGBA8 DDS to sOutputFile, marked sRGB when bSrgb is set.
  ///
  /// Either input may be empty: a mask alone produces a white texture with that alpha. The output is written under a
  /// temporary name and moved into place, so a reader never sees a half-written file. Safe to call from any thread.
  ///
  /// With a mask, out_pSoftAlpha (if given) receives HasSoftAlpha of the result, so the caller can choose between
  /// cutting the surface and blending it.
  static plStatus PrepareForPreview(plStringView sColorFile, plStringView sOpacityFile, bool bSrgb, plUInt32 uiMaxSize, plStringView sOutputFile,
    bool* out_pSoftAlpha = nullptr);

  /// \brief The TexConv arguments that write a texture at full resolution, its longest edge at most uiMaxSize, with
  /// mipmaps and block compression, to sOutputFile, which must end in ".dds".
  ///
  /// - Color: BC7 sRGB. RGB comes from sColorFile and alpha from the first channel of sOpacityFile, which TexConv scales
  ///   to the colour's size. A mask alone gets white RGB; a colour without a mask keeps its own alpha. bCutout keeps a
  ///   mask's coverage in the smaller mipmaps, as PrepareForPreview does for a mask that is not soft.
  /// - Normal: BC5. The mipmaps are filtered and renormalised from x, y and z; BC5 keeps x and y.
  /// - Linear: BC4 of the red channel.
  ///
  /// sOpacityFile is only used with Usage::Color.
  static void GetConversionArguments(plStringView sColorFile, plStringView sOpacityFile, Usage usage, bool bCutout, plUInt32 uiMaxSize,
    plStringView sOutputFile, plDynamicArray<plString>& out_arguments);

  /// \brief Whether an RGBA8 image's alpha is soft coverage, like grime or smoke, rather than a cut-out, like leaves or
  /// a fence: true when most texels with any coverage are neither nearly transparent nor nearly opaque. A cut-out has
  /// partial alpha only along its edges.
  static bool HasSoftAlpha(const plImage& image);

  /// \brief HasSoftAlpha of a file PrepareForPreview wrote earlier.
  static plStatus ReadSoftAlpha(plStringView sPreparedFile, bool& out_bSoftAlpha);
};
