#include <UsdBridge/UsdBridgePCH.h>

#include <UsdBridge/UsdBasis.h>

plUsdBasis::plUsdBasis(UpAxis upAxis, double fMetersPerUnit)
  : m_UpAxis(upAxis)
  , m_fMetersPerUnit(fMetersPerUnit > 0.0 ? fMetersPerUnit : 0.01)
{
}

plMat3d plUsdBasis::GetAxisMap() const
{
  if (m_UpAxis == UpAxis::Y)
  {
    // (x, y, z) -> (x, z, y)
    return plMat3d::MakeFromValues(1, 0, 0, 0, 0, 1, 0, 1, 0);
  }

  // (x, y, z) -> (x, -y, z)
  return plMat3d::MakeFromValues(1, 0, 0, 0, -1, 0, 0, 0, 1);
}

plMat3d plUsdBasis::GetLookCorrection() const
{
  plMat3d m;
  m.SetColumn(0, DirectionToPlasma(plVec3d(0, 0, -1)));
  m.SetColumn(1, DirectionToPlasma(plVec3d(1, 0, 0)));
  m.SetColumn(2, DirectionToPlasma(plVec3d(0, 1, 0)));
  return m;
}

plVec3d plUsdBasis::DirectionToPlasma(const plVec3d& v) const
{
  if (m_UpAxis == UpAxis::Y)
    return plVec3d(v.x, v.z, v.y);

  return plVec3d(v.x, -v.y, v.z);
}

plVec3d plUsdBasis::DirectionToUsd(const plVec3d& v) const
{
  // The axis map is its own inverse.
  return DirectionToPlasma(v);
}

plVec3d plUsdBasis::PointToPlasma(const plVec3d& v) const
{
  return DirectionToPlasma(v) * m_fMetersPerUnit;
}

plVec3d plUsdBasis::PointToUsd(const plVec3d& v) const
{
  return DirectionToUsd(v) / m_fMetersPerUnit;
}

static plMat4d MakeBasisMatrix(const plMat3d& mAxis, double fScale)
{
  plMat4d m = plMat4d::MakeIdentity();

  for (plUInt32 c = 0; c < 3; ++c)
  {
    for (plUInt32 r = 0; r < 3; ++r)
    {
      m.Element(c, r) = mAxis.Element(c, r) * fScale;
    }
  }

  return m;
}

plMat4d plUsdBasis::MatrixToPlasma(const plMat4d& mUsd) const
{
  const plMat3d mAxis = GetAxisMap();
  const plMat4d mTo = MakeBasisMatrix(mAxis, m_fMetersPerUnit);
  const plMat4d mFrom = MakeBasisMatrix(mAxis, 1.0 / m_fMetersPerUnit);

  return mTo * mUsd * mFrom;
}

plMat4d plUsdBasis::MatrixToUsd(const plMat4d& mPlasma) const
{
  const plMat3d mAxis = GetAxisMap();
  const plMat4d mTo = MakeBasisMatrix(mAxis, 1.0 / m_fMetersPerUnit);
  const plMat4d mFrom = MakeBasisMatrix(mAxis, m_fMetersPerUnit);

  return mTo * mPlasma * mFrom;
}

plTransform plUsdBasis::MatrixToTransform(const plMat4d& m)
{
  plVec3d vAxis[3];
  for (plUInt32 c = 0; c < 3; ++c)
  {
    vAxis[c].Set(m.Element(c, 0), m.Element(c, 1), m.Element(c, 2));
  }

  plVec3d vScale(vAxis[0].GetLength(), vAxis[1].GetLength(), vAxis[2].GetLength());

  // A mirrored matrix has a negative determinant; fold the mirror into the X scale so the remaining 3x3 is a rotation.
  if (vAxis[0].CrossRH(vAxis[1]).Dot(vAxis[2]) < 0.0)
  {
    vScale.x = -vScale.x;
  }

  plMat3d mRot = plMat3d::MakeIdentity();
  for (plUInt32 c = 0; c < 3; ++c)
  {
    const double fLen = (c == 0) ? vScale.x : (c == 1 ? vScale.y : vScale.z);
    const plVec3d vCol = (plMath::Abs(fLen) > 1e-12) ? vAxis[c] / fLen : plVec3d::MakeZero();

    mRot.Element(c, 0) = vCol.x;
    mRot.Element(c, 1) = vCol.y;
    mRot.Element(c, 2) = vCol.z;
  }

  plQuatd qRot = plQuatd::MakeFromMat3(mRot);
  qRot.Normalize();

  plTransform res;
  res.m_vPosition.Set((float)m.Element(3, 0), (float)m.Element(3, 1), (float)m.Element(3, 2));
  res.m_vScale.Set((float)vScale.x, (float)vScale.y, (float)vScale.z);
  res.m_qRotation = plQuat((float)qRot.x, (float)qRot.y, (float)qRot.z, (float)qRot.w);
  return res;
}

plMat4d plUsdBasis::TransformToMatrix(const plTransform& t)
{
  const plQuatd qRot(t.m_qRotation.x, t.m_qRotation.y, t.m_qRotation.z, t.m_qRotation.w);
  const plMat3d mRot = qRot.GetAsMat3();

  plMat4d m = plMat4d::MakeIdentity();
  const double fScale[3] = {t.m_vScale.x, t.m_vScale.y, t.m_vScale.z};

  for (plUInt32 c = 0; c < 3; ++c)
  {
    for (plUInt32 r = 0; r < 3; ++r)
    {
      m.Element(c, r) = mRot.Element(c, r) * fScale[c];
    }
  }

  m.Element(3, 0) = t.m_vPosition.x;
  m.Element(3, 1) = t.m_vPosition.y;
  m.Element(3, 2) = t.m_vPosition.z;
  return m;
}

plMat4 plUsdBasis::ToFloat(const plMat4d& m)
{
  plMat4 result;
  for (plUInt32 c = 0; c < 4; ++c)
  {
    for (plUInt32 r = 0; r < 4; ++r)
    {
      result.Element(c, r) = (float)m.Element(c, r);
    }
  }
  return result;
}

plMat3 plUsdBasis::NormalMatrix(const plMat4& mTransform)
{
  // Columns of the inverse transpose times the determinant are the cross products of the other two columns; the sign
  // of the determinant keeps normals of mirroring transforms pointing out of the surface.
  const plVec3 c0 = mTransform.GetColumn(0).GetAsVec3();
  const plVec3 c1 = mTransform.GetColumn(1).GetAsVec3();
  const plVec3 c2 = mTransform.GetColumn(2).GetAsVec3();
  const float fSign = c0.Dot(c1.CrossRH(c2)) < 0.0f ? -1.0f : 1.0f;

  const plVec3 n0 = c1.CrossRH(c2) * fSign;
  const plVec3 n1 = c2.CrossRH(c0) * fSign;
  const plVec3 n2 = c0.CrossRH(c1) * fSign;

  plMat3 result;
  for (plUInt32 r = 0; r < 3; ++r)
  {
    result.Element(0, r) = n0.GetData()[r];
    result.Element(1, r) = n1.GetData()[r];
    result.Element(2, r) = n2.GetData()[r];
  }
  return result;
}

plStatus plUsdBasis::SelfTest()
{
  const UpAxis axes[] = {UpAxis::Y, UpAxis::Z};
  const double units[] = {0.01, 1.0};

  for (UpAxis axis : axes)
  {
    for (double fUnit : units)
    {
      const plUsdBasis basis(axis, fUnit);
      const char* szAxis = axis == UpAxis::Y ? "Y" : "Z";

      const plMat3d mAxis = basis.GetAxisMap();
      if (plMath::Abs(mAxis.GetDeterminant() + 1.0) > 1e-12)
        return plStatus(plFmt("upAxis {}: the axis map is not a reflection (det {})", szAxis, mAxis.GetDeterminant()));

      if (!(mAxis * mAxis).IsEqual(plMat3d::MakeIdentity(), 1e-12))
        return plStatus(plFmt("upAxis {}: the axis map is not its own inverse", szAxis));

      // Up must land on Plasma's up.
      const plVec3d vUp = axis == UpAxis::Y ? plVec3d(0, 1, 0) : plVec3d(0, 0, 1);
      if (!basis.DirectionToPlasma(vUp).IsEqual(plVec3d(0, 0, 1), 1e-12))
        return plStatus(plFmt("upAxis {}: USD up does not map to Plasma +Z", szAxis));

      // A camera's view must stay a rotation: +X onto USD's -Z, and Plasma's up onto USD's up.
      const plMat3d mLook = basis.GetLookCorrection();
      if (plMath::Abs(mLook.GetDeterminant() - 1.0) > 1e-12)
        return plStatus(plFmt("upAxis {}: the look correction is not a rotation (det {})", szAxis, mLook.GetDeterminant()));

      if (!(mLook * plVec3d(0, 0, 1)).IsEqual(basis.DirectionToPlasma(plVec3d(0, 1, 0)), 1e-12))
        return plStatus(plFmt("upAxis {}: the look correction does not keep the camera's up", szAxis));

      const plVec3d vPoint(1.25, -3.5, 7.0);
      if (!basis.PointToUsd(basis.PointToPlasma(vPoint)).IsEqual(vPoint, 1e-9))
        return plStatus(plFmt("upAxis {} unit {}: point round trip failed", szAxis, fUnit));

      // A rotated, non-uniformly scaled, translated transform must survive USD -> Plasma -> USD, and stay a rotation.
      plTransform tUsd;
      tUsd.m_vPosition.Set(10.0f, -2.0f, 3.5f);
      tUsd.m_qRotation = plQuat::MakeFromAxisAndAngle(plVec3(0.3f, 0.8f, -0.2f).GetNormalized(), plAngle::MakeFromDegree(37.0f));
      tUsd.m_vScale.Set(1.0f, 2.0f, 0.5f);

      const plMat4d mUsd = TransformToMatrix(tUsd);
      const plMat4d mPlasma = basis.MatrixToPlasma(mUsd);

      if (mPlasma.GetRotationalPart().GetDeterminant() <= 0.0)
        return plStatus(plFmt("upAxis {}: a rotation became a reflection", szAxis));

      if (!basis.MatrixToUsd(mPlasma).IsEqual(mUsd, 1e-9))
        return plStatus(plFmt("upAxis {} unit {}: matrix round trip failed", szAxis, fUnit));

      const plTransform tPlasma = MatrixToTransform(mPlasma);
      if (!basis.MatrixToUsd(TransformToMatrix(tPlasma)).IsEqual(mUsd, 1e-4))
        return plStatus(plFmt("upAxis {} unit {}: transform decomposition round trip failed", szAxis, fUnit));

      // The object transform and the geometry map must agree: transforming a local point in USD and then converting
      // it gives the same result as converting both and transforming in Plasma.
      const plVec3d vLocal(0.5, 1.5, -2.0);
      const plVec3d vA = basis.PointToPlasma(mUsd.TransformPosition(vLocal));
      const plVec3d vB = mPlasma.TransformPosition(basis.PointToPlasma(vLocal));
      if (!vA.IsEqual(vB, 1e-9))
        return plStatus(plFmt("upAxis {} unit {}: geometry and transform maps disagree", szAxis, fUnit));
    }
  }

  // A mirrored transform must decompose to a negative scale and recompose to the same matrix.
  {
    plMat4d mMirror = plMat4d::MakeIdentity();
    mMirror.Element(0, 0) = -2.0;
    mMirror.Element(1, 1) = 3.0;
    mMirror.Element(3, 1) = 4.0;

    if (!TransformToMatrix(MatrixToTransform(mMirror)).IsEqual(mMirror, 1e-5))
      return plStatus("a mirrored matrix did not survive decomposition");
  }

  return plStatus(PL_SUCCESS);
}
