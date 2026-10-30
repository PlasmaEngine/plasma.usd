#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/Variant.h>

/// \brief JSON as the package reads and writes it: the live hub's messages, bake.json, the .usdlinks sidecars, the
/// partition import manifests and the files of other tools (Houdini packages, Blender's extension list).
///
/// An object is a plVariantDictionary: strings are plString, numbers double, true and false bool, null an invalid
/// variant, arrays plVariantArray and objects plVariantDictionary. Numbers that must stay exact (64-bit hashes, time
/// stamps) are written as strings.
namespace plUsdJson
{
  /// Reads a JSON object; false, with an empty object, when the text is not one. Parse errors are not logged.
  PL_EDITORPLUGINUSD_DLL bool Parse(plStringView sText, plVariantDictionary& out_object);

  /// Reads a file holding a JSON object; false when it cannot be read or holds none.
  PL_EDITORPLUGINUSD_DLL bool ReadFile(plStringView sPath, plVariantDictionary& out_object);

  /// The object as JSON: on one line without whitespace, or indented for files people read.
  PL_EDITORPLUGINUSD_DLL plString Write(const plVariantDictionary& object, bool bIndented);

  /// Writes the object indented into a file, replacing it.
  PL_EDITORPLUGINUSD_DLL plResult WriteFile(plStringView sPath, const plVariantDictionary& object);

  /// Writes text into a file as it is, replacing it.
  PL_EDITORPLUGINUSD_DLL plResult WriteTextFile(plStringView sPath, plStringView sText);

  /// A member's string; empty when it is missing or no string.
  PL_EDITORPLUGINUSD_DLL plString GetString(const plVariantDictionary& object, plStringView sKey);

  /// A member's number; fDefault when it is missing or no number.
  PL_EDITORPLUGINUSD_DLL double GetNumber(const plVariantDictionary& object, plStringView sKey, double fDefault = 0.0);

  PL_EDITORPLUGINUSD_DLL bool GetBool(const plVariantDictionary& object, plStringView sKey, bool bDefault = false);

  /// A member that is an object or an array; an empty one when it is missing or something else.
  PL_EDITORPLUGINUSD_DLL const plVariantDictionary& GetObject(const plVariantDictionary& object, plStringView sKey);
  PL_EDITORPLUGINUSD_DLL const plVariantArray& GetArray(const plVariantDictionary& object, plStringView sKey);

  /// An array element or a variant as an object or a string; empty when it is something else.
  PL_EDITORPLUGINUSD_DLL const plVariantDictionary& AsObject(const plVariant& value);
  PL_EDITORPLUGINUSD_DLL plString AsString(const plVariant& value);

  /// 64-bit values kept exact as strings: lower-case hex without leading zeros, or decimal. Reading gives 0 for a
  /// missing or malformed string.
  PL_EDITORPLUGINUSD_DLL plString HexString(plUInt64 uiValue);
  PL_EDITORPLUGINUSD_DLL plUInt64 ParseHex(plStringView sText);
  PL_EDITORPLUGINUSD_DLL plString DecimalString(plInt64 iValue);
  PL_EDITORPLUGINUSD_DLL plInt64 ParseDecimal(plStringView sText);
} // namespace plUsdJson
