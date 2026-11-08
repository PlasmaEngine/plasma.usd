#pragma once

#include <Foundation/Types/Status.h>

class plGeometry;

/// MikkTSpace with a hash-based output-vertex weld, preserving every vertex attribute and tangent seam.
namespace plUsdTangents
{
  plStatus Compute(plGeometry& ref_geometry);
}
