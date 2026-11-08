#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <Foundation/Types/Uuid.h>
#include <UsdBridge/UsdStage.h>

class plDocumentObject;
class plDocumentObjectManager;
class plRTTI;

/// \brief The Plasma component that stands for a light or camera prim.
///
/// SphereLight becomes a point light, or a spot light when a shaping cone narrows it. DistantLight, RectLight, DiskLight
/// and CylinderLight become directional, rect, disk and cylinder lights. Other lights (dome, portal, geometry) and prims
/// that are neither get no component. Cameras become camera components with the lens set from focal length and
/// aperture, so the field of view matches the DCC's.
///
/// Used for the live link's objects and for the bake prefab's, so both carry the same components.
class PL_EDITORPLUGINUSD_DLL plUsdPrimComponents
{
public:
  /// Direct removals have no undo owner. Destroy the detached object and its children as well.
  static plStatus RemoveObject(plDocumentObjectManager* pManager, const plDocumentObject* pObject);

  /// Binds materials on the component so geometry can be shared independently of its materials.
  static void ApplyMaterials(plDocumentObjectManager* pManager, const plDocumentObject* pComponent,
    plStringView sLinkId, plArrayPtr<const plUsdMaterialGroup> groups);

  /// \brief The component type for a prim, or nullptr when it needs none.
  static const plRTTI* GetComponentType(const plUsdPrim& prim);

  /// \brief Makes the object carry exactly the component the prim needs, with the given GUID: adds it, replaces it when
  /// the type changed (e.g. a cone was added to a sphere light), updates its values, or removes it. No undo step.
  static void Apply(plDocumentObjectManager* pManager, const plDocumentObject* pObject, const plUsdPrim& prim, const plUuid& componentGuid);

  /// \brief The reverse of Apply: the prim's light or camera values with every component property the user changed since
  /// Apply taken over, converted back to USD's model (luminance, normalize, millimetres). A property counts as changed
  /// when it differs from what Apply would set for the prim's current values. Returns whether anything changed.
  static bool ReadBack(const plDocumentObject* pComponent, const plUsdPrim& applied, plUsdLight& out_light, plUsdCamera& out_camera);

  /// \brief The factor the camera's lens values are scaled by to fit the component's ranges; 1 for ordinary lenses.
  static float GetLensFit(const plUsdCamera& camera);

  /// \brief How many instances one point instancer shows at most, counted over all its instanced mesh components. Each
  /// instance is an element of a component's InstanceData, a document object the editor mirrors to the engine on its own;
  /// instances past this are left out, with a warning.
  static constexpr plUInt32 MaxInstances = 20000;

  /// \brief Makes an instanced mesh component show a mesh file at the given transforms, relative to its object: sets its
  /// mesh, colour and InstanceData, adding and removing elements at the end and leaving unchanged values alone. New
  /// elements get GUIDs made from sGuidSeed and their index. No undo step.
  static void ApplyInstances(plDocumentObjectManager* pManager, const plDocumentObject* pComponent, plStringView sMeshResource, const plColor& color,
    plArrayPtr<const plTransform> instances, plStringView sGuidSeed);

  // --- Plasma components carried in USD (see plUsdComponent) --------------------------------------------------------
  //
  // Only plain values travel: numbers, vectors, colours, quaternions, strings (resource references included), angles
  // (degrees), times (seconds), enumerations and bitflags (by name). Arrays, maps, embedded objects and references to
  // other objects stay in Plasma.

  /// \brief Reads the components the user put on a generated object: every component whose GUID is not in generated.
  /// A value is included when it differs from its default or the stage already has it (stage), so a value reset to its
  /// default overrides what weaker layers set.
  static void ReadUserComponents(const plDocumentObject* pObject, plArrayPtr<const plUuid> generated, plArrayPtr<const plUsdComponent> stage,
    plDynamicArray<plUsdComponent>& out_components);

  /// \brief Makes the object carry a prim's components: each type's component is found among the object's components
  /// that are not in generated, or added with a GUID made from sGuidSeed and the type. Values the stage dropped since
  /// previous go back to their defaults, and components of types previous has and components doesn't are removed.
  /// Types this editor doesn't know are skipped with a warning. No undo step.
  static void ApplyUserComponents(plDocumentObjectManager* pManager, const plDocumentObject* pObject, plArrayPtr<const plUsdComponent> components,
    plArrayPtr<const plUsdComponent> previous, plArrayPtr<const plUuid> generated, plStringView sGuidSeed);
};
