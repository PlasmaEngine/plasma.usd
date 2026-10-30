#include <UsdBridge/UsdBridgePCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Texture/Image/Formats/ImageFileFormat.h>
#include <Texture/Image/Image.h>
#include <Texture/Image/ImageConversion.h>
#include <Texture/Image/ImageUtils.h>
#include <UsdBridge/UsdTexture.h>

namespace
{
  /// Whether every channel of the format is one plain byte, so pixels can be averaged byte by byte whatever the
  /// channel order.
  bool HasByteChannels(plImageFormat::Enum format)
  {
    return !plImageFormat::IsCompressed(format) && plImageFormat::GetDataType(format) == plImageFormatDataType::UNORM &&
           plImageFormat::GetBitsPerPixel(format) == 8 * plImageFormat::GetNumChannels(format);
  }

  /// Box-filters the first mip of an image with byte channels down by 2^uiShift in each direction, in one pass over
  /// the source rows. Blocks cut off by the right or bottom edge average the pixels they have.
  void Downsample(plImage& ref_image, plUInt32 uiShift)
  {
    const plUInt32 uiBytesPerPixel = plImageFormat::GetBitsPerPixel(ref_image.GetImageFormat()) / 8;
    const plUInt32 uiFactor = 1u << uiShift;
    const plUInt32 uiWidth = ref_image.GetWidth();
    const plUInt32 uiHeight = ref_image.GetHeight();
    const plUInt32 uiNewWidth = (uiWidth + uiFactor - 1) >> uiShift;
    const plUInt32 uiNewHeight = (uiHeight + uiFactor - 1) >> uiShift;

    plImageHeader header;
    header.SetImageFormat(ref_image.GetImageFormat());
    header.SetWidth(uiNewWidth);
    header.SetHeight(uiNewHeight);

    plImage reduced;
    reduced.ResetAndAlloc(header);

    plDynamicArray<plUInt32> sums;
    sums.SetCountUninitialized(uiNewWidth * uiBytesPerPixel);

    for (plUInt32 y = 0; y < uiNewHeight; ++y)
    {
      plMemoryUtils::ZeroFill(sums.GetData(), sums.GetCount());

      const plUInt32 uiFirstRow = y << uiShift;
      const plUInt32 uiEndRow = plMath::Min(uiFirstRow + uiFactor, uiHeight);

      for (plUInt32 sy = uiFirstRow; sy < uiEndRow; ++sy)
      {
        const plUInt8* pRow = ref_image.GetPixelPointer<plUInt8>(0, 0, 0, 0, sy);
        for (plUInt32 sx = 0; sx < uiWidth; ++sx)
        {
          plUInt32* pSum = &sums[(sx >> uiShift) * uiBytesPerPixel];
          const plUInt8* pPixel = pRow + sx * uiBytesPerPixel;
          for (plUInt32 c = 0; c < uiBytesPerPixel; ++c)
            pSum[c] += pPixel[c];
        }
      }

      plUInt8* pOut = reduced.GetPixelPointer<plUInt8>(0, 0, 0, 0, y);
      for (plUInt32 x = 0; x < uiNewWidth; ++x)
      {
        const plUInt32 uiColumns = plMath::Min((x + 1) << uiShift, uiWidth) - (x << uiShift);
        const plUInt32 uiCount = uiColumns * (uiEndRow - uiFirstRow);
        for (plUInt32 c = 0; c < uiBytesPerPixel; ++c)
          pOut[x * uiBytesPerPixel + c] = (plUInt8)((sums[x * uiBytesPerPixel + c] + uiCount / 2) / uiCount);
      }
    }

    ref_image.ResetAndMove(std::move(reduced));
  }

  /// Decodes an image file into 8-bit RGBA no larger than uiMaxSize on its longest edge.
  ///
  /// Sources with byte channels are reduced before they are converted to RGBA, so the conversion only touches the
  /// reduced pixels; a 4K source is otherwise sixteen times the work. The reduction averages encoded values, which is
  /// close enough for a preview.
  plStatus DecodeFile(plStringView sFile, plUInt32 uiMaxSize, plImage& out_image)
  {
    plOSFile file;
    if (file.Open(sFile, plFileOpenMode::Read).Failed())
      return plStatus(plFmt("cannot open '{}'", sFile));

    plDynamicArray<plUInt8> data;
    file.ReadAll(data);
    file.Close();

    const plStringView sExtension = sFile.GetFileExtension();
    plImageFileFormat* pFormat = plImageFileFormat::GetReaderFormat(sExtension);
    if (pFormat == nullptr)
      return plStatus(plFmt("no image reader for '.{}'", sExtension));

    plImage decoded;
    plRawMemoryStreamReader reader(data.GetData(), data.GetCount());
    if (pFormat->ReadImage(reader, decoded, sExtension).Failed())
      return plStatus(plFmt("could not decode '{}'", sFile));

    data.Clear();

    plUInt32 uiShift = 0;
    while (plMath::Max(decoded.GetWidth(), decoded.GetHeight()) > (uiMaxSize << uiShift))
      ++uiShift;

    if (uiShift > 0 && HasByteChannels(decoded.GetImageFormat()))
    {
      Downsample(decoded, uiShift);
      uiShift = 0;
    }

    // A source that is already sRGB stays sRGB so its bytes are not linearised. Converted into a separate image: source
    // and target of a conversion must not share storage.
    const plImageFormat::Enum target = plImageFormat::IsSrgb(decoded.GetImageFormat()) ? plImageFormat::R8G8B8A8_UNORM_SRGB : plImageFormat::R8G8B8A8_UNORM;
    if (decoded.GetImageFormat() == target)
    {
      out_image.ResetAndMove(std::move(decoded));
    }
    else if (plImageConversion::Convert(decoded, out_image, target).Failed())
    {
      return plStatus(plFmt("could not convert '{}' from {} to RGBA8", sFile, plImageFormat::GetName(decoded.GetImageFormat())));
    }

    // Wider channels, e.g. 16-bit PNGs, are reduced after the conversion instead.
    if (uiShift > 0)
      Downsample(out_image, uiShift);

    return plStatus(PL_SUCCESS);
  }
} // namespace

bool plUsdTexture::HasSoftAlpha(const plImage& image)
{
  if (plImageFormat::GetBitsPerPixel(image.GetImageFormat()) != 32 || !HasByteChannels(image.GetImageFormat()))
    return false;

  plUInt64 uiCovered = 0;
  plUInt64 uiPartial = 0;

  for (plUInt32 y = 0; y < image.GetHeight(); ++y)
  {
    const plUInt8* pRow = image.GetPixelPointer<plUInt8>(0, 0, 0, 0, y);
    for (plUInt32 x = 0; x < image.GetWidth(); ++x)
    {
      const plUInt8 uiAlpha = pRow[x * 4 + 3];
      if (uiAlpha > 12)
      {
        ++uiCovered;
        if (uiAlpha < 243)
          ++uiPartial;
      }
    }
  }

  return uiPartial * 2 > uiCovered;
}

plStatus plUsdTexture::ReadSoftAlpha(plStringView sPreparedFile, bool& out_bSoftAlpha)
{
  plImage image;
  const plStatus res = DecodeFile(sPreparedFile, 1u << 16, image);
  if (res.Failed())
    return res;

  out_bSoftAlpha = HasSoftAlpha(image);
  return plStatus(PL_SUCCESS);
}

plStatus plUsdTexture::PrepareForPreview(
  plStringView sColorFile, plStringView sOpacityFile, bool bSrgb, plUInt32 uiMaxSize, plStringView sOutputFile, bool* out_pSoftAlpha)
{
  plImage color;

  if (!sColorFile.IsEmpty())
  {
    plStatus res = DecodeFile(sColorFile, uiMaxSize, color);
    if (res.Failed())
      return res;
  }

  if (!sOpacityFile.IsEmpty())
  {
    plImage opacity;
    plStatus res = DecodeFile(sOpacityFile, uiMaxSize, opacity);
    if (res.Failed())
      return res;

    if (!color.IsValid())
    {
      // A mask without a colour texture: white, so the material's base colour shows through unchanged.
      plImageHeader header;
      header.SetImageFormat(plImageFormat::R8G8B8A8_UNORM);
      header.SetWidth(opacity.GetWidth());
      header.SetHeight(opacity.GetHeight());
      color.ResetAndAlloc(header);

      auto blob = color.GetBlobPtr<plUInt8>();
      plMemoryUtils::PatternFill(blob.GetPtr(), 0xFF, (plUInt32)blob.GetCount());
    }

    // The mask's first channel becomes the colour texture's alpha, sampled nearest if the sizes differ.
    const plUInt32 uiWidth = color.GetWidth();
    const plUInt32 uiHeight = color.GetHeight();
    for (plUInt32 y = 0; y < uiHeight; ++y)
    {
      const plUInt32 oy = (plUInt32)((plUInt64)y * opacity.GetHeight() / uiHeight);
      plUInt8* pColor = color.GetPixelPointer<plUInt8>(0, 0, 0, 0, y);
      const plUInt8* pMask = opacity.GetPixelPointer<plUInt8>(0, 0, 0, 0, oy);

      for (plUInt32 x = 0; x < uiWidth; ++x)
      {
        const plUInt32 ox = (plUInt32)((plUInt64)x * opacity.GetWidth() / uiWidth);
        pColor[x * 4 + 3] = pMask[ox * 4];
      }
    }
  }

  if (!color.IsValid())
    return plStatus("no texture to prepare");

  const bool bSoftAlpha = !sOpacityFile.IsEmpty() && HasSoftAlpha(color);
  if (out_pSoftAlpha)
    *out_pSoftAlpha = bSoftAlpha;

  const plImageFormat::Enum outputFormat = bSrgb ? plImageFormat::R8G8B8A8_UNORM_SRGB : plImageFormat::R8G8B8A8_UNORM;
  color.ReinterpretAs(outputFormat);

  // Mipmaps are generated in linear float - GenerateMipMaps accepts nothing else - and brought back to 8 bit afterwards.
  // Converting from the sRGB format linearises the colour, so the filtering averages light rather than encoded values.
  plImage linear;
  if (plImageConversion::Convert(color, linear, plImageFormat::R32G32B32A32_FLOAT).Failed())
    return plStatus("could not convert to float for mipmapping");

  // A cut-out keeps its coverage in the smaller mips, or foliage thins out with distance. Soft alpha is averaged like
  // colour, since it is blended.
  plImageUtils::MipMapOptions options;
  options.m_preserveCoverage = !sOpacityFile.IsEmpty() && !bSoftAlpha;

  plImage linearMips;
  plImageUtils::GenerateMipMaps(linear, linearMips, options);

  plImage mipped;
  if (plImageConversion::Convert(linearMips, mipped, outputFormat).Failed())
    return plStatus("could not convert the mipmaps back to 8 bit");

  plImageFileFormat* pWriter = plImageFileFormat::GetWriterFormat("dds");
  if (pWriter == nullptr)
    return plStatus("no DDS writer");

  plContiguousMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  if (pWriter->WriteImage(writer, mipped, "dds").Failed())
    return plStatus("could not encode DDS");

  plStringBuilder sFolder = sOutputFile;
  sFolder.PathParentDirectory();
  if (plOSFile::CreateDirectoryStructure(sFolder).Failed())
    return plStatus(plFmt("could not create '{}'", sFolder));

  plStringBuilder sTemp(sOutputFile, ".tmp");
  {
    plOSFile out;
    if (out.Open(sTemp, plFileOpenMode::Write).Failed())
      return plStatus(plFmt("could not write '{}'", sTemp));

    out.Write(storage.GetData(), storage.GetStorageSize64()).IgnoreResult();
  }

  if (plOSFile::ExistsFile(sOutputFile))
    plOSFile::DeleteFile(sOutputFile).IgnoreResult();

  if (plOSFile::MoveFileOrDirectory(sTemp, sOutputFile).Failed())
  {
    plOSFile::DeleteFile(sTemp).IgnoreResult();
    return plStatus(plFmt("could not move '{}' into place", sTemp));
  }

  return plStatus(PL_SUCCESS);
}

void plUsdTexture::GetConversionArguments(plStringView sColorFile, plStringView sOpacityFile, Usage usage, bool bCutout, plUInt32 uiMaxSize,
  plStringView sOutputFile, plDynamicArray<plString>& out_arguments)
{
  PL_ASSERT_DEV(usage == Usage::Color || sOpacityFile.IsEmpty(), "Only a colour texture takes an opacity mask");

  out_arguments.Clear();

  auto add = [&](plStringView sArgument)
  { out_arguments.PushBack(sArgument); };

  plStringBuilder sMaxSize;
  sMaxSize.SetFormat("{}", uiMaxSize);

  add("-out");
  add(sOutputFile);
  add("-type");
  add("2D");
  add("-mipmaps");
  add("Linear");
  add("-maxRes");
  add(sMaxSize);

  switch (usage)
  {
    case Usage::Color:
      // Medium picks BC7 whether or not there is alpha; High would pick BC1 for opaque colour.
      add("-usage");
      add("Color");
      add("-compression");
      add("Medium");

      if (bCutout)
        add("-mipsPreserveCoverage");

      if (!sColorFile.IsEmpty() && !sOpacityFile.IsEmpty())
      {
        add("-in0");
        add(sColorFile);
        add("-in1");
        add(sOpacityFile);
        add("-rgb");
        add("in0.rgb");
        add("-a");
        add("in1.r");
      }
      else if (!sOpacityFile.IsEmpty())
      {
        // White, so the material's base colour shows through unchanged.
        add("-in0");
        add(sOpacityFile);
        add("-rgb");
        add("white");
        add("-a");
        add("in0.r");
      }
      else
      {
        // The colour keeps its own alpha, as in the preview.
        add("-in0");
        add(sColorFile);
        add("-rgba");
        add("in0.rgba");
      }
      break;

    case Usage::Normal:
      // Medium would leave a normal map uncompressed (R8G8); High picks BC5.
      add("-usage");
      add("NormalMap");
      add("-compression");
      add("High");
      add("-in0");
      add(sColorFile);
      add("-rgb");
      add("in0.rgb");
      break;

    case Usage::Linear:
      add("-usage");
      add("Linear");
      add("-compression");
      add("Medium");
      add("-in0");
      add(sColorFile);
      add("-r");
      add("in0.r");
      break;
  }
}
