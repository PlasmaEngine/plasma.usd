#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <EditorUi/RowWindow.h>
#include <Foundation/Communication/Event.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Rect.h>
#include <Foundation/Types/UniquePtr.h>

class plEditorUiSurface;
class plUiElement;
struct plEditorUiPointerEvent;
struct plEditorUiKeyEvent;

/// \brief The prims of the World Partition from USD dialog as an engine-drawn tree with check boxes and columns: Prim
/// (disclosure, check box and name), Type, Payload and variants, and Size (right-aligned in the code face).
///
/// Items with a box of their own are checked by a click on the box or by Space. Items whose box follows their children
/// (m_bAutoTristate) show checked, unchecked or partly checked from the children that have a box; a click checks all of
/// them, or unchecks them when all are checked. Disabled items are dimmed and have no box. Rows are windowed, so the cost
/// follows the viewport. Once clicked, the tree takes the keys: Up, Down, Home, End, Page Up and Page Down move the
/// current row, Left and Right collapse and expand, Space checks. A double-click on a row expands or collapses it.
/// Element names start with the tree's name.
class PL_EDITORPLUGINUSD_DLL plUsdPrimTreeView
{
public:
  enum class Check : plUInt8
  {
    None,
    Unchecked,
    Checked,
    Partial,
  };

  struct Item
  {
    plUInt32 m_uiParent = plInvalidIndex; ///< An item before this one; invalid for top-level items.
    plString m_sName;
    plString m_sToolTip;
    plString m_sType;
    plString m_sNotes;
    plString m_sSize;
    plString m_sSizeToolTip;
    bool m_bEnabled = true;
    bool m_bCheckable = false;    ///< Has a box of its own.
    bool m_bAutoTristate = false; ///< The box follows the children.
    bool m_bChecked = false;      ///< The state of a box of its own.
  };

  plUsdPrimTreeView(plEditorUiSurface& ref_surface, plStringView sName);
  ~plUsdPrimTreeView();

  /// Replaces the items, all collapsed.
  void SetItems(plDynamicArray<Item>&& items);
  void Clear();
  plUInt32 GetItemCount() const { return m_Items.GetCount(); }
  const Item& GetItem(plUInt32 uiItem) const { return m_Items[uiItem]; }

  /// The item's box as it shows.
  Check GetCheck(plUInt32 uiItem) const { return m_States[uiItem]; }

  /// What a click on the item's box does: a box of its own takes bChecked, a box that follows its children sets every
  /// child with a box.
  void SetChecked(plUInt32 uiItem, bool bChecked);

  /// Replaces the item's size column and its tooltip.
  void SetSize(plUInt32 uiItem, plStringView sSize, plStringView sToolTip);

  void SetExpanded(plUInt32 uiItem, bool bExpanded);
  bool IsExpanded(plUInt32 uiItem) const { return m_Expanded[uiItem]; }
  void ExpandAll();

  /// Expands the items down to uiDepth, 0 being the top-level items.
  void ExpandToDepth(plUInt32 uiDepth);

  /// Adds the header and the rows to a document under construction at rect, in pParent's units.
  void Build(plUiElement* pParent, const plRectFloat& rect);

  /// The tooltip at a position relative to the rect last built: the row's tooltip over the Prim column, the size's over
  /// the Size column when it has one, else sDefault.
  plString GetToolTipAt(const plVec2& vPosition, plStringView sDefault) const;

  /// The rows shown, top to bottom, as item indices.
  plUInt32 GetRowCount() const { return m_Rows.GetCount(); }
  plUInt32 GetRowItem(plUInt32 uiRow) const { return m_Rows[uiRow].m_uiItem; }

  /// The current row's item, or plInvalidIndex.
  plUInt32 GetCurrentItem() const;

  /// A box was clicked or Space pressed; the argument is the item.
  plEvent<plUInt32> m_ChecksChanged;

  static constexpr float HeaderHeight = 26.0f;

private:
  struct Row
  {
    plUInt32 m_uiItem = 0;
    plUInt32 m_uiDepth = 0;
  };

  enum Column : plUInt32
  {
    TypeColumn,
    NotesColumn,
    SizeColumn,
    TextColumnCount,
  };

  void UpdateStates();
  void Flatten();
  void AppendRows(plUInt32 uiItem, plUInt32 uiDepth);
  void MeasureColumns(plUInt32 uiFirstItem);
  void LayoutColumns(float fWidth);
  void BuildHeader(plUiElement* pRoot);
  void BuildRowTemplate(plUiElement* pRoot);
  plVariant MakeRowData(plUInt32 uiRow) const;
  plString GetCell(const Item& item, plUInt32 uiColumn) const;

  void OnPointer(const plEditorUiPointerEvent& event);
  void OnKey(const plEditorUiKeyEvent& event);
  bool IsOwnElement(plStringView sElement) const;
  plInt32 FindRow(plUInt32 uiItem) const;
  void SetCurrentRow(plInt32 iRow);
  void ToggleCheck(plUInt32 uiItem);
  plString ElementName(plStringView sSuffix) const;

  plEditorUiSurface& m_Surface;
  plString m_sName;

  plDynamicArray<Item> m_Items;
  plDynamicArray<plDynamicArray<plUInt32>> m_Children; ///< Per item; the last entry holds the top-level items.
  plDynamicArray<Check> m_States;
  plDynamicArray<bool> m_Expanded;
  plDynamicArray<Row> m_Rows;

  float m_fNaturalWidths[TextColumnCount] = {};
  float m_fColumnWidths[TextColumnCount] = {};
  float m_fPrimWidth = 0.0f;
  plRectFloat m_Rect = plRectFloat(0, 0, 0, 0);

  plUInt32 m_uiCurrent = plInvalidIndex; ///< The current item.
  bool m_bFocused = false;

  plUniquePtr<plEditorUiRowWindow> m_pRows;
  plEventSubscriptionID m_PointerSubscription = 0;
  plEventSubscriptionID m_KeySubscription = 0;
};

PL_DEFINE_AS_POD_TYPE(plUsdPrimTreeView::Check);
