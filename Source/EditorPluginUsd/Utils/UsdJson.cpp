#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/Utils/UsdJson.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/JSONWriter.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/ConversionUtils.h>

namespace
{
  /// Swallows the parser's messages: text that is not JSON is an answer, not an error.
  class QuietLog : public plLogInterface
  {
  public:
    void HandleLogMessage(const plLoggingEventData& le) override {}
  };

  const plVariantDictionary& EmptyObject()
  {
    static plVariantDictionary s_Empty;
    return s_Empty;
  }

  const plVariantArray& EmptyArray()
  {
    static plVariantArray s_Empty;
    return s_Empty;
  }
} // namespace

bool plUsdJson::Parse(plStringView sText, plVariantDictionary& out_object)
{
  out_object.Clear();

  plRawMemoryStreamReader stream(sText.GetStartPointer(), sText.GetElementCount());

  QuietLog log;
  plJSONReader reader;
  reader.SetLogInterface(&log);

  if (reader.Parse(stream).Failed() || reader.GetTopLevelElementType() != plJSONReader::ElementType::Dictionary)
    return false;

  out_object = reader.GetTopLevelObject();
  return true;
}

bool plUsdJson::ReadFile(plStringView sPath, plVariantDictionary& out_object)
{
  out_object.Clear();

  plOSFile file;
  if (file.Open(sPath, plFileOpenMode::Read).Failed())
    return false;

  plDynamicArray<plUInt8> content;
  file.ReadAll(content);
  file.Close();

  return Parse(plStringView(reinterpret_cast<const char*>(content.GetData()), content.GetCount()), out_object);
}

plString plUsdJson::Write(const plVariantDictionary& object, bool bIndented)
{
  plContiguousMemoryStreamStorage storage;
  plMemoryStreamWriter stream(&storage);

  plStandardJSONWriter writer;
  writer.SetOutputStream(&stream);
  writer.SetWhitespaceMode(bIndented ? plJSONWriter::WhitespaceMode::LessIndentation : plJSONWriter::WhitespaceMode::None);
  writer.WriteVariant(plVariant(object));

  return plStringView(reinterpret_cast<const char*>(storage.GetData()), static_cast<plUInt32>(storage.GetStorageSize64()));
}

plResult plUsdJson::WriteFile(plStringView sPath, const plVariantDictionary& object)
{
  return WriteTextFile(sPath, Write(object, true));
}

plResult plUsdJson::WriteTextFile(plStringView sPath, plStringView sText)
{
  plStringBuilder sFolder = sPath;
  sFolder.PathParentDirectory();
  if (!sFolder.IsEmpty())
    PL_SUCCEED_OR_RETURN(plOSFile::CreateDirectoryStructure(sFolder));

  plOSFile file;
  PL_SUCCEED_OR_RETURN(file.Open(sPath, plFileOpenMode::Write));
  return file.Write(sText.GetStartPointer(), sText.GetElementCount());
}

plString plUsdJson::GetString(const plVariantDictionary& object, plStringView sKey)
{
  const plVariant* pValue = object.GetValue(sKey);
  return pValue != nullptr ? AsString(*pValue) : plString();
}

double plUsdJson::GetNumber(const plVariantDictionary& object, plStringView sKey, double fDefault)
{
  const plVariant* pValue = object.GetValue(sKey);
  if (pValue == nullptr || !pValue->IsNumber())
    return fDefault;

  return pValue->ConvertTo<double>();
}

bool plUsdJson::GetBool(const plVariantDictionary& object, plStringView sKey, bool bDefault)
{
  const plVariant* pValue = object.GetValue(sKey);
  return pValue != nullptr && pValue->IsA<bool>() ? pValue->Get<bool>() : bDefault;
}

const plVariantDictionary& plUsdJson::GetObject(const plVariantDictionary& object, plStringView sKey)
{
  const plVariant* pValue = object.GetValue(sKey);
  return pValue != nullptr ? AsObject(*pValue) : EmptyObject();
}

const plVariantArray& plUsdJson::GetArray(const plVariantDictionary& object, plStringView sKey)
{
  const plVariant* pValue = object.GetValue(sKey);
  return pValue != nullptr && pValue->IsA<plVariantArray>() ? pValue->Get<plVariantArray>() : EmptyArray();
}

const plVariantDictionary& plUsdJson::AsObject(const plVariant& value)
{
  return value.IsA<plVariantDictionary>() ? value.Get<plVariantDictionary>() : EmptyObject();
}

plString plUsdJson::AsString(const plVariant& value)
{
  return value.IsA<plString>() ? value.Get<plString>() : plString();
}

plString plUsdJson::HexString(plUInt64 uiValue)
{
  plStringBuilder s;
  s.SetFormat("{}", plArgU(uiValue, 1, false, 16));
  return s;
}

plUInt64 plUsdJson::ParseHex(plStringView sText)
{
  plUInt64 uiValue = 0;
  if (sText.IsEmpty() || sText.GetElementCount() > 16 || plConversionUtils::ConvertHexStringToUInt64(sText, uiValue).Failed())
    return 0;

  return uiValue;
}

plString plUsdJson::DecimalString(plInt64 iValue)
{
  plStringBuilder s;
  s.SetFormat("{}", iValue);
  return s;
}

plInt64 plUsdJson::ParseDecimal(plStringView sText)
{
  plInt64 iValue = 0;
  if (plConversionUtils::StringToInt64(sText, iValue).Failed())
    return 0;

  return iValue;
}
