#pragma once

#include <Foundation/Algorithm/HashingUtils.h>
#include <UsdBridge/UsdBridgePCH.h>
#include <UsdBridge/UsdStage.h>

#include <string>
#include <unordered_map>

/// \brief String helpers at the pxr boundary, for UsdBridge sources.
namespace plUsdText
{
  inline std::string ToStd(plStringView s)
  {
    return std::string(s.GetStartPointer(), s.GetElementCount());
  }

  inline plString FromStd(const std::string& s)
  {
    return plString(plStringView(s.data(), (plUInt32)s.size()));
  }

  inline plUInt64 HashString(const std::string& s, plUInt64 uiSeed)
  {
    return plHashingUtils::xxHash64(s.data(), s.size(), uiSeed);
  }
} // namespace plUsdText

/// \brief Bridge-internal access to a plUsdStage's OpenUSD stage and to its geometry helpers.
///
/// Includes pxr headers: only for UsdBridge sources, never for its public headers.
struct plUsdStageAccess
{
  static pxr::UsdStageRefPtr GetStage(const plUsdStage& stage);

  static plUsdPrimKind GetKind(const pxr::UsdPrim& prim);

  /// \brief Snapshots a Mesh or Gprim, as plUsdStage::CaptureGeometry does. Works on instance proxies.
  static plUniquePtr<plUsdGeometrySource> CaptureGeometry(const plUsdBasis& basis, const pxr::UsdPrim& prim, plStatus& out_status);

  /// \brief OpenUSD's row-vector matrix as a column-vector plMat4d.
  static plMat4d FromGfMatrix(const pxr::GfMatrix4d& m);

  /// \brief Seed of plUsdGeometryKey::m_uiIdentity: geometry converted with different bases differs.
  static plUInt64 BasisSeed(const plUsdBasis& basis);

  /// \brief Hash of the layer specs a Mesh's or Gprim's geometry is composed from. Seeded with BasisSeed, this is
  /// plUsdGeometryKey::m_uiIdentity.
  static plUInt64 HashGeometrySpecs(const pxr::UsdPrim& prim, plUInt64 uiSeed);

  /// Layer real path -> hash of its modification time and size; shared by calls on one scan so each file is read once.
  using LayerStamps = std::unordered_map<std::string, plUInt64>;

  /// \brief HashGeometrySpecs, and mixes the files of those specs with their modification times and sizes into
  /// inout_uiFileStamp: equal stamps mean the geometry comes from the same, unchanged files.
  static plUInt64 HashGeometrySpecs(const pxr::UsdPrim& prim, plUInt64 uiSeed, LayerStamps& ref_layerStamps, plUInt64& inout_uiFileStamp);
};
