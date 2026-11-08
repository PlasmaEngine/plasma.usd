#include <UsdBridge/UsdBridgePCH.h>

#include <Core/Graphics/Geometry.h>
#include <Foundation/Containers/HashTable.h>
#include <UsdBridge/UsdTangents.h>
#include <mikktspace/mikktspace.h>

#define XXH_INLINE_ALL
#include <Foundation/ThirdParty/xxHash/xxhash.h>

namespace
{
  struct VertexKey
  {
    PL_DECLARE_POD_TYPE();

    plUInt32 m_Words[19];

    bool operator==(const VertexKey& rhs) const
    {
      return plMemoryUtils::IsEqual(m_Words, rhs.m_Words, PL_ARRAY_SIZE(m_Words));
    }
  };

  struct VertexHash
  {
    static plUInt32 Hash(const VertexKey& key)
    {
      return XXH32(key.m_Words, sizeof(key.m_Words), 0);
    }

    static bool Equal(const VertexKey& a, const VertexKey& b)
    {
      return a == b;
    }
  };

  VertexKey MakeKey(const plGeometry::Vertex& vertex)
  {
    VertexKey key;
    const float values[] = {vertex.m_vPosition.x, vertex.m_vPosition.y, vertex.m_vPosition.z,
      vertex.m_vNormal.x, vertex.m_vNormal.y, vertex.m_vNormal.z,
      vertex.m_vTangent.x, vertex.m_vTangent.y, vertex.m_vTangent.z, vertex.m_fBiTangentSign,
      vertex.m_vTexCoord.x, vertex.m_vTexCoord.y, vertex.m_Color.r, vertex.m_Color.g, vertex.m_Color.b, vertex.m_Color.a};

    for (plUInt32 i = 0; i < PL_ARRAY_SIZE(values); ++i)
    {
      // Numeric equality considers positive and negative zero equal, as the engine's vertex comparison does.
      const float value = values[i] == 0.0f ? 0.0f : values[i];
      plMemoryUtils::Copy(&key.m_Words[i], reinterpret_cast<const plUInt32*>(&value), 1);
    }

    plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(&key.m_Words[16]), reinterpret_cast<const plUInt8*>(&vertex.m_BoneIndices), 8);
    plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(&key.m_Words[18]), reinterpret_cast<const plUInt8*>(&vertex.m_BoneWeights), 4);
    return key;
  }

  struct Context
  {
    const plGeometry* m_pSource = nullptr;
    plDeque<plGeometry::Vertex> m_Vertices;
    plDeque<plGeometry::Polygon> m_Polygons;
    plHashTable<VertexKey, plUInt32, VertexHash> m_Welded;

    static Context& Get(const SMikkTSpaceContext* context)
    {
      return *static_cast<Context*>(context->m_pUserData);
    }

    static const plGeometry::Vertex& Vertex(const SMikkTSpaceContext* context, int face, int corner)
    {
      const auto* source = Get(context).m_pSource;
      return source->GetVertices()[source->GetPolygons()[face].m_Vertices[corner]];
    }

    static int FaceCount(const SMikkTSpaceContext* context)
    {
      return (int)Get(context).m_pSource->GetPolygons().GetCount();
    }

    static int CornerCount(const SMikkTSpaceContext* context, int face)
    {
      return (int)Get(context).m_pSource->GetPolygons()[face].m_Vertices.GetCount();
    }

    static void Position(const SMikkTSpaceContext* context, float output[], int face, int corner)
    {
      const auto& value = Vertex(context, face, corner).m_vPosition;
      output[0] = value.x;
      output[1] = value.y;
      output[2] = value.z;
    }

    static void Normal(const SMikkTSpaceContext* context, float output[], int face, int corner)
    {
      const auto& value = Vertex(context, face, corner).m_vNormal;
      output[0] = value.x;
      output[1] = value.y;
      output[2] = value.z;
    }

    static void Uv(const SMikkTSpaceContext* context, float output[], int face, int corner)
    {
      const auto& value = Vertex(context, face, corner).m_vTexCoord;
      output[0] = value.x;
      output[1] = value.y;
    }

    static void Tangent(const SMikkTSpaceContext* context, const float tangent[], float sign, int face, int corner)
    {
      auto& data = Get(context);
      auto vertex = Vertex(context, face, corner);
      vertex.m_vTangent.Set(tangent[0], tangent[1], tangent[2]);
      vertex.m_fBiTangentSign = sign;

      const VertexKey key = MakeKey(vertex);
      plUInt32 index = 0;
      if (!data.m_Welded.TryGetValue(key, index))
      {
        index = data.m_Vertices.GetCount();
        data.m_Vertices.PushBack(vertex);
        data.m_Welded.Insert(key, index);
      }

      data.m_Polygons[face].m_Vertices[corner] = index;
    }
  };
}

plStatus plUsdTangents::Compute(plGeometry& ref_geometry)
{
  for (const auto& polygon : ref_geometry.GetPolygons())
  {
    if (polygon.m_Vertices.GetCount() < 3 || polygon.m_Vertices.GetCount() > 4)
    {
      return plStatus("Tangent generation requires triangles or quads.");
    }
  }

  Context data;
  data.m_pSource = &ref_geometry;
  data.m_Polygons = ref_geometry.GetPolygons();
  data.m_Welded.Reserve(ref_geometry.GetVertices().GetCount());

  SMikkTSpaceInterface mikkInterface = {};
  mikkInterface.m_getNumFaces = Context::FaceCount;
  mikkInterface.m_getNumVerticesOfFace = Context::CornerCount;
  mikkInterface.m_getPosition = Context::Position;
  mikkInterface.m_getNormal = Context::Normal;
  mikkInterface.m_getTexCoord = Context::Uv;
  mikkInterface.m_setTSpaceBasic = Context::Tangent;
  SMikkTSpaceContext context = {&mikkInterface, &data};

  if (!genTangSpaceDefault(&context))
  {
    return plStatus("MikkTSpace could not generate tangents.");
  }

  ref_geometry.GetVertices() = std::move(data.m_Vertices);
  ref_geometry.GetPolygons() = std::move(data.m_Polygons);
  return plStatus(PL_SUCCESS);
}
