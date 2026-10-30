#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/Set.h>
#include <Foundation/Math/Angle.h>
#include <Foundation/Math/Color.h>
#include <Foundation/Math/Transform.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Time/Timestamp.h>
#include <Foundation/Types/Status.h>
#include <Foundation/Types/UniquePtr.h>
#include <Foundation/Types/Variant.h>
#include <UsdBridge/UsdBasis.h>

class plGeometry;

/// \brief What a prim turns into on the Plasma side.
enum class plUsdPrimKind : plUInt8
{
  Group,    ///< Xform, Scope or any other imageable prim without geometry of its own.
  Mesh,     ///< UsdGeomMesh.
  Gprim,    ///< Cube, Sphere, Cylinder, Cone or Capsule, tessellated on import.
  Light,     ///< Any UsdLux light.
  Camera,    ///< UsdGeomCamera.
  Instancer, ///< UsdGeomPointInstancer: draws copies of its prototypes' meshes, see plUsdInstancer.
};

/// \brief Surface values read from a bound UsdShade material, or from displayColor when nothing is bound.
struct plUsdSurface
{
  plString m_sMaterialPath; ///< Prim path of the bound material, empty when the colour comes from displayColor.
  plColor m_BaseColor = plColor::White;
  float m_fMetallic = 0.0f;
  float m_fRoughness = 0.5f;
  float m_fOpacity = 1.0f;
  float m_fOpacityThreshold = 0.0f; ///< Opacity below this is cut away entirely; 0 when the material asks for no cut.
  plString m_sBaseColorTexture;     ///< Absolute path of a texture feeding the base colour, if any.
  plString m_sNormalTexture;
  plString m_sRoughnessTexture;
  plString m_sMetallicTexture;
  plString m_sOpacityTexture; ///< Opacity mask: foliage cards, decals.
  bool m_bDoubleSided = false; ///< From the gprim's doubleSided attribute rather than from the material.

  bool operator==(const plUsdSurface& rhs) const;
};

/// \brief The faces of a mesh that share one material.
///
/// Group 0 is the mesh's own binding (or its displayColor) and covers every face no subset claims. The groups after it
/// are the mesh's "materialBind" GeomSubsets, in the order OpenUSD lists them.
struct plUsdMaterialGroup
{
  plString m_sSubset; ///< Name of the GeomSubset; empty for group 0.
  bool m_bHasSurface = false;
  plUsdSurface m_Surface;

  bool operator==(const plUsdMaterialGroup& rhs) const { return m_sSubset == rhs.m_sSubset && m_bHasSurface == rhs.m_bHasSurface && m_Surface == rhs.m_Surface; }
};

/// \brief Which UsdLux light schema a Light prim uses.
enum class plUsdLightShape : plUInt8
{
  Sphere,   ///< SphereLight: a point light, or a spot light when a shaping cone narrows it.
  Distant,  ///< DistantLight.
  Rect,     ///< RectLight.
  Disk,     ///< DiskLight.
  Cylinder, ///< CylinderLight.
  Other,    ///< DomeLight, PortalLight, geometry lights and anything else.
};

/// \brief A UsdLux light's values, in metres.
///
/// UsdLux lights emit luminance, intensity * 2^exposure, in nits, from every point of their surface. A normalized light
/// divides that by its surface area instead, so its power stays the same whatever its size. A distant light's value is
/// read as illuminance in lux.
///
/// Unreal's exporter writes a sphere light's luminance as its candela over the sphere's whole surface, 4 pi r^2, where
/// USD's model gives the disc the sphere shows, pi r^2. Its sphere lights are four times brighter than their luminance
/// says; m_bUnrealUnits marks them.
struct plUsdLight
{
  plUsdLightShape m_Shape = plUsdLightShape::Other;
  plColor m_Color = plColor::White; ///< Linear.
  bool m_bUseTemperature = false;
  float m_fTemperature = 6500.0f; ///< Kelvin.
  float m_fLuminance = 1.0f;      ///< intensity * 2^exposure: nits, or lux for distant lights.
  bool m_bNormalize = false;
  bool m_bUnrealUnits = false; ///< The stage was written by Unreal (plUsdStage::IsUnrealAuthored).
  float m_fRadius = 0.0f; ///< Sphere, disk and cylinder.
  float m_fWidth = 0.0f;  ///< Rect.
  float m_fHeight = 0.0f; ///< Rect.
  float m_fLength = 0.0f; ///< Cylinder.
  plAngle m_ConeAngle = plAngle::MakeFromDegree(180.0f); ///< Full opening angle of a shaping cone; 180 degrees means none.
  float m_fConeSoftness = 0.0f;                          ///< 0 to 1: how much of the cone fades out.
  float m_fDiffuse = 1.0f;
  float m_fSpecular = 1.0f;
  bool m_bCastShadows = true;

  bool operator==(const plUsdLight& rhs) const;

  /// \brief Of every field, without the struct's padding.
  plUInt64 ComputeHash(plUInt64 uiSeed) const;

  /// \brief Calls func(field) for every field, for hashing and serialising them one by one.
  template <typename Light, typename Func>
  static void VisitFields(Light& ref_light, Func&& func)
  {
    func(ref_light.m_Shape);
    func(ref_light.m_Color);
    func(ref_light.m_bUseTemperature);
    func(ref_light.m_fTemperature);
    func(ref_light.m_fLuminance);
    func(ref_light.m_bNormalize);
    func(ref_light.m_bUnrealUnits);
    func(ref_light.m_fRadius);
    func(ref_light.m_fWidth);
    func(ref_light.m_fHeight);
    func(ref_light.m_fLength);
    func(ref_light.m_ConeAngle);
    func(ref_light.m_fConeSoftness);
    func(ref_light.m_fDiffuse);
    func(ref_light.m_fSpecular);
    func(ref_light.m_bCastShadows);
  }
};

/// \brief A UsdGeomCamera's values.
///
/// USD gives lens values in tenths of a scene unit, which is millimetres on a centimetre stage; they are converted to
/// millimetres here. Only their ratios matter for the field of view.
struct plUsdCamera
{
  bool m_bOrthographic = false;
  float m_fFocalLength = 50.0f;          ///< Millimetres.
  float m_fHorizontalAperture = 20.955f; ///< Millimetres.
  float m_fVerticalAperture = 15.2908f;  ///< Millimetres.
  float m_fOrthoWidth = 0.0f;            ///< Metres, for orthographic cameras: the horizontal aperture.
  float m_fNear = 0.01f;                 ///< Metres.
  float m_fFar = 10000.0f;               ///< Metres.
  float m_fFStop = 0.0f;                 ///< 0 means everything is in focus.
  float m_fFocusDistance = 0.0f;         ///< Metres.
  float m_fExposure = 0.0f;              ///< Stops, added to the image's exposure.

  bool operator==(const plUsdCamera& rhs) const;

  /// \brief Of every field, without the struct's padding.
  plUInt64 ComputeHash(plUInt64 uiSeed) const;

  /// \brief Calls func(field) for every field, for hashing and serialising them one by one.
  template <typename Camera, typename Func>
  static void VisitFields(Camera& ref_camera, Func&& func)
  {
    func(ref_camera.m_bOrthographic);
    func(ref_camera.m_fFocalLength);
    func(ref_camera.m_fHorizontalAperture);
    func(ref_camera.m_fVerticalAperture);
    func(ref_camera.m_fOrthoWidth);
    func(ref_camera.m_fNear);
    func(ref_camera.m_fFar);
    func(ref_camera.m_fFStop);
    func(ref_camera.m_fFocusDistance);
    func(ref_camera.m_fExposure);
  }
};

/// \brief One mesh of a point instancer's prototypes and where the instancer draws it.
struct plUsdInstancedMesh
{
  plString m_sMeshPath; ///< A Mesh or Gprim prim: a prototype, or below one.

  /// One per instance of the prototype, relative to the instancer, in Plasma's frame. Each includes the prototype's own
  /// transform and the mesh's place inside the prototype.
  plDynamicArray<plTransform> m_Instances;
};

/// \brief What a UsdGeomPointInstancer draws, at the stage's start time.
///
/// Instances the instancer's mask switches off (invisibleIds, inactiveIds) are left out, and so are invisible and
/// inactive prims inside the prototypes. Prototypes that are instancers themselves are not drawn.
struct plUsdInstancer
{
  plDynamicArray<plUsdInstancedMesh> m_Meshes;
  plUInt32 m_uiInstanceCount = 0; ///< Instances of the instancer, over all prototypes.
  plUInt64 m_uiHash = 0;          ///< Of everything above, to tell cheaply whether an instancer changed.
};

/// \brief One property value of a Plasma component carried in USD.
struct plUsdComponentValue
{
  plString m_sProperty;

  /// bool, plInt32, plUInt32, plInt64, plUInt64, float, double, plString, plVec2, plVec3, plVec4, plVec2I32, plVec3I32,
  /// plVec4I32, plColor (linear) or plQuat. Enumerations and bitflags are their names, as plString.
  plVariant m_Value;

  bool operator==(const plUsdComponentValue& rhs) const { return m_sProperty == rhs.m_sProperty && m_Value == rhs.m_Value; }
};

/// \brief A Plasma component carried in USD by the codeless PlasmaComponentAPI schema.
///
/// The prim applies "PlasmaComponentAPI:<type>", a multiple-apply API schema whose instance name is the component's type,
/// and holds the component's values as "plasma:<type>:<property>" attributes. Tools that don't know the schema keep
/// both as they are. One component of each type per prim.
struct plUsdComponent
{
  plString m_sType; ///< The component's type name, e.g. "plJoltDynamicActorComponent".
  plDynamicArray<plUsdComponentValue> m_Values;

  bool operator==(const plUsdComponent& rhs) const { return m_sType == rhs.m_sType && m_Values == rhs.m_Values; }
};

/// \brief One prim of a composed stage, already converted into Plasma's frame.
struct plUsdPrim
{
  plString m_sPath;
  plString m_sName;
  plString m_sTypeName;
  plUsdPrimKind m_Kind = plUsdPrimKind::Group;
  plInt32 m_iParent = -1; ///< Index into the array ReadPrims filled, or -1 for a top-level prim.

  /// In Plasma's frame, metres. A light or camera's transform includes plUsdBasis::GetLookCorrection, so a Plasma light or
  /// camera component on its object faces the way the prim does; its children's transforms take the correction out
  /// again. AuthorLocalTransform expects transforms in the same form.
  plTransform m_LocalTransform = plTransform::MakeIdentity();
  bool m_bVisible = true;            ///< False when this prim authors visibility = invisible.
  bool m_bActive = true;             ///< False when the prim is deactivated; it then has no children.
  bool m_bTransformAnimated = false; ///< Some xform op has time samples.

  /// Below a point instancer: its prototypes and their children are drawn only through the instancer, as OpenUSD's
  /// renderers do.
  bool m_bPrototype = false;

  /// \brief For meshes and shapes: at least one entry, see plUsdMaterialGroup. Empty for everything else.
  plHybridArray<plUsdMaterialGroup, 1> m_MaterialGroups;

  plUsdLight m_Light;         ///< For Light prims.
  plUsdCamera m_Camera;       ///< For Camera prims.
  plUsdInstancer m_Instancer; ///< For active Instancer prims.

  /// Plasma components the prim carries, in the order it applies them; see plUsdComponent. Empty for inactive prims.
  plDynamicArray<plUsdComponent> m_Components;
};

/// \brief Where a prim's own values come from, for showing it next to the prim.
struct plUsdPrimOpinions
{
  /// Real path of the strongest root-stack layer that defines the prim or sets anything on it. Empty when only
  /// referenced or payload layers do, i.e. the prim comes from the asset's references.
  plString m_sWinningLayer;
  bool m_bEditXform = false;      ///< The edit layer moves the prim.
  bool m_bEditVisibility = false; ///< The edit layer sets its visibility.
  bool m_bEditActive = false;     ///< The edit layer sets whether it is active.
  bool m_bEditValues = false;     ///< The edit layer sets other attributes: a light's or camera's values.
  bool m_bEditVariants = false;   ///< The edit layer selects variants of the prim's variant sets.
  bool m_bEditComponents = false; ///< The edit layer adds, changes or removes Plasma components; see plUsdComponent.

  bool HasEditOpinion() const { return m_bEditXform || m_bEditVisibility || m_bEditActive || m_bEditValues || m_bEditVariants || m_bEditComponents; }

  bool operator==(const plUsdPrimOpinions& rhs) const
  {
    return m_sWinningLayer == rhs.m_sWinningLayer && m_bEditXform == rhs.m_bEditXform && m_bEditVisibility == rhs.m_bEditVisibility &&
           m_bEditActive == rhs.m_bEditActive && m_bEditValues == rhs.m_bEditValues && m_bEditVariants == rhs.m_bEditVariants &&
           m_bEditComponents == rhs.m_bEditComponents;
  }
};

/// \brief One variant set of a prim.
struct plUsdVariantSet
{
  plString m_sName;
  plDynamicArray<plString> m_Variants;
  plString m_sSelection; ///< The variant the stage composes; empty when none is selected.
};

/// \brief Paths the stage reported as changed since the last call to ConsumeChanges.
struct plUsdChanges
{
  plDynamicArray<plString> m_ResyncedPaths;    ///< Prims (and their subtrees) whose structure or composition changed.
  plDynamicArray<plString> m_ChangedInfoPaths; ///< Property or prim paths whose values changed.

  bool IsEmpty() const { return m_ResyncedPaths.IsEmpty() && m_ChangedInfoPaths.IsEmpty(); }
};

/// \brief A snapshot of one prim's geometry.
///
/// Captured on the thread that owns the stage. Build and GetHash can then run on any thread, and later edits or reloads
/// of the stage do not affect the snapshot.
class plUsdGeometrySource
{
public:
  virtual ~plUsdGeometrySource() = default;

  /// \brief Tessellates into Plasma's frame with triangle winding already corrected. Face corners that share a point,
  /// normal and UV are welded into one vertex. Tangents are left to the caller.
  ///
  /// The result is triangles only, ordered by material group (see plUsdMaterialGroup). When out_pGroupTriangleCounts is
  /// given it receives the number of triangles of each group, including empty ones.
  virtual plStatus Build(plGeometry& out_geometry, plDynamicArray<plUInt32>* out_pGroupTriangleCounts = nullptr) const = 0;

  /// \brief Hash of everything Build reads. Reads all of the geometry, so it is computed on first use, on any thread.
  virtual plUInt64 GetHash() const = 0;

  /// \brief The points the source holds, to budget the memory of captures; 0 for shapes, which hold none.
  virtual plUInt64 GetPointCount() const { return 0; }
};

/// \brief Which layer specs a prim's geometry is composed from; see plUsdStage::ComputeGeometryKey.
struct plUsdGeometryKey
{
  /// Equal for prims whose geometry comes from the same specs, e.g. every copy of one referenced asset, so their
  /// geometry is the same. Says nothing about the specs' values: those changing is what ConsumeGeometryChanges reports.
  plUInt64 m_uiIdentity = 0;

  /// Of the files those specs are in and their modification times, to tell whether something built from the geometry
  /// in an earlier session is still current.
  plUInt64 m_uiFileStamp = 0;

  plString m_sName; ///< Name of the prim whose spec holds the points, for readable file names.
};

/// \brief Prims whose geometry may have changed; see plUsdStage::ConsumeGeometryChanges.
struct plUsdGeometryChanges
{
  /// Prims whose geometry specs, or geometry properties, changed. Their geometry has to be built again.
  plSet<plString> m_Edited;

  /// Prims (and their subtrees) whose composition changed. Their geometry keys have to be computed again; their
  /// geometry changed only when the key did, or when they are also in m_Edited.
  plSet<plString> m_Resynced;

  bool IsEmpty() const { return m_Edited.IsEmpty() && m_Resynced.IsEmpty(); }
};

/// \brief One layer of the stage's root layer stack.
struct plUsdLayerInfo
{
  plString m_sIdentifier;
  plString m_sRealPath;          ///< Empty for anonymous layers.
  plTimestamp m_ModificationTime; ///< Of the file on disk; invalid for anonymous layers.
  bool m_bIsEditTarget = false;
  bool m_bDirty = false;
  bool m_bMuted = false;
};

/// \brief A composed OpenUSD stage, seen through Plasma types.
///
/// Everything that touches the OpenUSD API is behind this class, so nothing that includes this header needs pxr
/// headers or its build settings. Not thread-safe: use it from one thread.
class plUsdStage
{
  PL_DISALLOW_COPY_AND_ASSIGN(plUsdStage);

public:
  plUsdStage();
  ~plUsdStage();

  /// \brief Which payloads a stage loads when it opens.
  enum class Load : plUInt8
  {
    All,  ///< Every payload, so the whole stage is composed.
    None, ///< No payload; subtrees are loaded on demand (see plUsdPartitionScan).
  };

  /// \brief Opens a stage from a root layer file. Any stage already open is closed first.
  plStatus Open(plStringView sRootLayerFile, Load load = Load::All);
  void Close();
  bool IsOpen() const;

  plStringView GetRootLayerFile() const;
  const plUsdBasis& GetBasis() const;
  double GetStartTimeCode() const;

  /// \brief Whether Unreal's exporter wrote the stage: some layer it uses has an attribute in Unreal's namespace, such as
  /// unrealNanite on meshes or unrealCameraPrimName on cameras. Decided when the stage opens.
  bool IsUnrealAuthored() const;

  /// \brief Reads every imageable prim under the pseudo-root, parents before children. Deactivated prims are included,
  /// without children.
  void ReadPrims(plDynamicArray<plUsdPrim>& out_prims) const;

  /// \brief Finds which layer of the root layer stack decides a prim's values, and what the edit layer sets on it.
  /// A handful of spec lookups per prim, cheap enough to run for every prim on each sync.
  void QueryPrimOpinions(plStringView sPrimPath, plUsdPrimOpinions& out_opinions) const;

  /// \brief Reads one prim. Fails when the path names no imageable prim.
  plResult ReadPrim(plStringView sPrimPath, plUsdPrim& out_prim) const;

  /// \brief Reads the surface of a UsdShade material prim, as a gprim bound to it would get it (without the gprim's
  /// doubleSided). Fails when the path names no material or its surface shader is none Plasma reads.
  plResult ReadMaterial(plStringView sMaterialPath, plUsdSurface& out_surface) const;

  /// \brief The variant sets of a prim, in the order the prim lists them. Empty for prims without any.
  void GetVariantSets(plStringView sPrimPath, plDynamicArray<plUsdVariantSet>& out_sets) const;

  /// \brief Snapshots the geometry of a Mesh or Gprim for building on another thread. Returns nullptr and a failure
  /// status when the prim has no geometry the engine can use.
  plUniquePtr<plUsdGeometrySource> CaptureGeometry(plStringView sPrimPath, plStatus& out_status) const;

  /// \brief CaptureGeometry followed by Build, on the calling thread.
  plStatus ExtractGeometry(plStringView sPrimPath, plGeometry& out_geometry) const;

  /// \brief Identifies a Mesh's or Gprim's geometry by the layer specs it is composed from, without reading any of it.
  ///
  /// Only the properties Build reads count: points, faces, normals, UVs, orientation, materialBind subsets, or a
  /// shape's dimensions. The prim is remembered, so ConsumeGeometryChanges reports it when one of those specs changes.
  /// Fails for prims that are neither.
  plResult ComputeGeometryKey(plStringView sPrimPath, plUsdGeometryKey& out_key);

  /// \brief Returns the prims whose geometry may have changed since the last call, from edits, reloads and composition
  /// changes. Independent of ConsumeChanges.
  void ConsumeGeometryChanges(plUsdGeometryChanges& out_changes);

  /// \brief Real paths of every layer the stage uses, including referenced and payload layers.
  void GetUsedLayerFiles(plDynamicArray<plString>& out_files) const;

  /// \brief Real paths of files referenced by asset-valued attributes, such as textures.
  void GetReferencedAssetFiles(plDynamicArray<plString>& out_files) const;

  /// \brief The root layer and its sublayers, strongest first.
  void GetLayerStack(plDynamicArray<plUsdLayerInfo>& out_layers) const;

  /// \brief How many opinions a layer of the stack holds: authored properties, plus prims whose active state it sets.
  /// Walks the whole layer, so meant for participant layers, not for the asset.
  plUInt32 CountLayerOpinions(plStringView sLayerIdentifier) const;

  /// \brief Mutes or unmutes a layer of the stack for this stage only. Nothing is written to any file.
  plStatus SetLayerMuted(plStringView sLayerIdentifier, bool bMuted);

  /// \brief Reloads every used layer whose file changed on disk, except the edit layer, whose unsaved opinions would be
  /// lost, and the layers in skipLayers (real paths), whose content arrives another way. Returns the number of layers
  /// reloaded. A skipped layer is reloaded by the first call that no longer skips it, if its file changed meanwhile.
  plUInt32 ReloadChangedLayers(plArrayPtr<const plString> skipLayers = {});

  /// \brief The edit layer as usda text; empty without an edit layer.
  plString ExportEditLayer() const;

  /// \brief Replaces the content of a layer of the stage (found by real path) with usda text, in memory only. For the
  /// layers of other participants that arrive through the live hub; the stage recomposes as for a reload.
  plStatus ImportLayerContent(plStringView sLayerFile, plStringView sUsda);

  /// \brief Returns the paths changed since the last call. Covers reloads as well as edits made through this class.
  void ConsumeChanges(plUsdChanges& out_changes);

  // --- Writing back -------------------------------------------------------------------------------------------------

  /// \brief Directs every Author* call into the given layer, which must be in the root layer stack.
  plStatus SetEditLayer(plStringView sLayerFile);
  plStringView GetEditLayer() const;
  bool HasEditLayer() const;

  plStatus AuthorLocalTransform(plStringView sPrimPath, const plTransform& localTransformPlasma);
  plStatus AuthorVisibility(plStringView sPrimPath, bool bVisible);
  plStatus AuthorActive(plStringView sPrimPath, bool bActive);

  /// \brief Selects a variant of one of a prim's variant sets. The stage recomposes the prim and everything below it.
  plStatus AuthorVariantSelection(plStringView sPrimPath, plStringView sVariantSet, plStringView sVariant);

  /// \brief Makes a prim carry exactly these Plasma components (see plUsdComponent): applies the schema for each and
  /// writes each value that differs from the composed stage, and removes the components of other types. Values that are
  /// not listed keep whatever the stage has.
  plStatus AuthorComponents(plStringView sPrimPath, plArrayPtr<const plUsdComponent> components);

  /// \brief Writes a light's values, converted back to USD's units, for every field that differs from the composed
  /// stage. Adds the ShapingAPI or ShadowAPI when a cone or a shadow setting needs it.
  plStatus AuthorLight(plStringView sPrimPath, const plUsdLight& light);

  /// \brief Writes a camera's values, converted back to USD's units, for every field that differs from the composed stage.
  plStatus AuthorCamera(plStringView sPrimPath, const plUsdCamera& camera);

  /// \brief The surface values a material's shader can take from Plasma.
  enum class SurfaceValue : plUInt8
  {
    BaseColor, ///< plColor, linear.
    Metallic,  ///< float
    Roughness, ///< float
    Opacity,   ///< float
  };

  /// \brief Writes one value of a material's surface shader (UsdPreviewSurface, OpenPBR, standard surface or glTF PBR
  /// inputs) into the edit layer. Fails when a texture drives that input, since the texture would win anyway.
  plStatus AuthorSurfaceValue(plStringView sMaterialPath, SurfaceValue value, const plVariant& data);

  /// \brief Removes the edit layer's opinions on a material's surface shader inputs.
  plStatus ClearSurfaceValues(plStringView sMaterialPath);

  /// \brief Whether the edit layer sets any of a material's surface shader inputs.
  bool HasSurfaceOpinion(plStringView sMaterialPath) const;

  /// \brief Removes the edit layer's opinions for a prim. sWhat is "xform", "visibility", "active", "values" (light and
  /// camera attributes: everything but transform, visibility and Plasma components), "variants" (variant selections),
  /// "components" (Plasma components) or empty for everything.
  plStatus ClearEditLayerOpinions(plStringView sPrimPath, plStringView sWhat);

  /// \brief Whether the edit layer holds an opinion for a prim. sWhat as for ClearEditLayerOpinions.
  bool HasEditLayerOpinion(plStringView sPrimPath, plStringView sWhat) const;

  /// \brief Layers that author the given attribute, strongest first, as identifiers.
  void GetAttributeLayerStack(plStringView sPrimPath, plStringView sAttribute, plDynamicArray<plString>& out_layers) const;

  bool IsEditLayerDirty() const;
  plStatus SaveEditLayer();

  // --- Sessions ----------------------------------------------------------------------------------------------------

  /// \brief Creates a session folder beside an asset: a session root layer that sublayers one participant layer over
  /// the asset, and copies the asset's stage metadata (upAxis, metersPerUnit, defaultPrim, time range) so the session
  /// composes exactly like the asset. Existing files are left alone, so this also joins an existing session.
  static plStatus CreateSession(plStringView sAssetRootLayer, plStringView sSessionName, plStringView sParticipant,
    plString& out_sSessionRootLayer, plString& out_sParticipantLayer);

  /// \brief The folder that holds every session of an asset: ".live/<asset>" beside the asset's root layer.
  static plString GetSessionsFolder(plStringView sAssetRootLayer);

  /// \brief Names of the sessions that exist beside an asset, i.e. the folders with a session root layer.
  static void ListSessions(plStringView sAssetRootLayer, plDynamicArray<plString>& out_sessions);

  /// \brief Real path of a session's strongest participant layer, whose participant owns the session and may commit it.
  static plString GetSessionOwnerLayer(plStringView sSessionRootLayer);

  /// \brief Folds a session into its asset: every participant layer's opinions, weakest first, are merged into the
  /// asset's root layer, which is saved; then every participant layer is emptied and saved. Merging keeps the asset's
  /// prim definitions: a participant's "over" adds opinions, its "def" adds prims. out_uiOpinions receives the number of
  /// properties and states merged.
  static plStatus CommitSession(plStringView sSessionRootLayer, plUInt32& out_uiOpinions);

  /// \brief OpenUSD's version, for the log.
  static plString GetOpenUsdVersion();

  /// \brief The extensions of USD stage files, without the dot.
  static plArrayPtr<const plStringView> GetFileExtensions();

  /// \brief Whether the file is a USD stage, by its extension.
  static bool IsUsdFile(plStringView sFile);

  /// \brief "USD (*.usd *.usda ...)", for file dialogs.
  static plString GetFileDialogFilter();

private:
  friend struct plUsdStageAccess; // Bridge-internal, see UsdStageAccess.h.

  struct Impl;
  plUniquePtr<Impl> m_pImpl;
};
