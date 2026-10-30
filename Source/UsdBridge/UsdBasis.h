#pragma once

#include <Foundation/Math/Mat3.h>
#include <Foundation/Math/Mat4.h>
#include <Foundation/Math/Transform.h>
#include <Foundation/Types/Status.h>

/// \brief Converts positions, directions and transforms between a USD stage's frame and Plasma's.
///
/// Plasma is left-handed with +X forward, +Y right and +Z up, and measures in metres. USD is right-handed, with a
/// per-stage up axis (Y by default) and a per-stage unit (metersPerUnit, 0.01 by default). The axis map is the one
/// Unreal's USD code uses, so a stage lands at the same place in both engines apart from their units:
///
///   upAxis Y:  p_plasma = k * (x, z, y)
///   upAxis Z:  p_plasma = k * (x, -y, z)
///
/// where k is metersPerUnit. Both axis maps are reflections (determinant -1) and are their own inverse. So geometry
/// has its triangle winding flipped, while object transforms stay rotations: M_plasma = (S C) M_usd (C S^-1), where S
/// scales by k.
///
/// Every conversion in both directions goes through this class, so a value read from USD and written back is
/// unchanged.
///
/// Matrices on both sides use the column-vector convention (plMat4d); converting to and from OpenUSD's row-vector
/// GfMatrix4d is a transpose done at the pxr boundary.
class plUsdBasis
{
public:
  enum class UpAxis : plUInt8
  {
    Y,
    Z,
  };

  plUsdBasis() = default;
  plUsdBasis(UpAxis upAxis, double fMetersPerUnit);

  UpAxis GetUpAxis() const { return m_UpAxis; }
  double GetMetersPerUnit() const { return m_fMetersPerUnit; }

  /// \brief The axis map C. Symmetric and its own inverse.
  plMat3d GetAxisMap() const;

  plVec3d PointToPlasma(const plVec3d& vUsd) const;
  plVec3d PointToUsd(const plVec3d& vPlasma) const;

  /// \brief Directions and normals: the axis map without the unit scale.
  plVec3d DirectionToPlasma(const plVec3d& vUsd) const;
  plVec3d DirectionToUsd(const plVec3d& vPlasma) const;

  plMat4d MatrixToPlasma(const plMat4d& mUsd) const;
  plMat4d MatrixToUsd(const plMat4d& mPlasma) const;

  /// \brief Rotation from Plasma's camera and light frame into a USD camera or light's converted frame.
  ///
  /// USD cameras and lights look along their local -Z with +Y up; Plasma's look along +X with +Z up and +Y to the right.
  /// The columns are the converted USD directions -Z, +X and +Y, so a camera or light object's transform times this
  /// matrix makes a Plasma component on it face the way the prim does.
  plMat3d GetLookCorrection() const;

  /// \brief Geometry converted with this basis must have every triangle's winding reversed.
  static constexpr bool FlipsWinding() { return true; }

  /// \brief Splits a matrix into position, rotation and scale. Handles mirrored matrices by negating the X scale, which
  /// plTransform::MakeFromMat4 does not.
  static plTransform MatrixToTransform(const plMat4d& m);
  static plMat4d TransformToMatrix(const plTransform& t);

  static plMat4 ToFloat(const plMat4d& m);

  /// \brief The matrix that moves normals with mTransform: its inverse transpose up to a positive factor. Built from
  /// cofactors, so it needs no inversion and works for tiny and zero scales; normals still need normalising.
  static plMat3 NormalMatrix(const plMat4& mTransform);

  /// \brief A mirroring transform reverses the winding of the geometry it moves.
  static bool IsMirror(const plMat4& mTransform) { return mTransform.GetRotationalPart().GetDeterminant() < 0.0f; }

  /// \brief Checks the properties above (determinant, involution, round trips) for both up axes. Returns a failure
  /// status that names the first check that failed.
  static plStatus SelfTest();

private:
  UpAxis m_UpAxis = UpAxis::Y;
  double m_fMetersPerUnit = 0.01;
};
