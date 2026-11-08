#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/Utils/Blackboard.h>
#include <EditorPluginUsd/PartitionImport/UsdPrimTreeView.h>
#include <EditorUi/Controls.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>

namespace
{
  constexpr float s_fIndentStep = 14.0f;
  constexpr float s_fCellPadding = 8.0f;
  constexpr float s_fNameLeft = 42.0f;
  constexpr float s_fBox = 14.0f;

  /// Text columns, in the order of plUsdPrimTreeView::Column.
  constexpr const char* s_szTitles[] = {"Type", "Payload and variants", "Size"};
  constexpr float s_fMinWidths[] = {60.0f, 100.0f, 70.0f};
  constexpr float s_fMaxWidths[] = {170.0f, 100000.0f, 150.0f};

  /// Qt measures this many rows for the columns that fit their content.
  constexpr plUInt32 s_uiMeasuredItems = 1000;

  const plColorGammaUB s_Transparent(0, 0, 0, 0);

  plString Key(const char* szPrefix, plUInt32 uiColumn)
  {
    plStringBuilder sKey;
    sKey.SetFormat("{}{}", szPrefix, uiColumn);
    return sKey;
  }

  plString Title(plUInt32 uiColumn)
  {
    // column titles use the uppercase heading of every table and list
    plStringBuilder sTitle = uiColumn < PL_ARRAY_SIZE(s_szTitles) ? s_szTitles[uiColumn] : "Prim";
    sTitle.ToUpper();
    return sTitle;
  }

  float MeasureCell(plStringView sText, bool bCode)
  {
    return bCode ? plEditorUi::MeasureCodeText(sText, plEditorUi::FontSize - 2) : plEditorUi::MeasureText(sText, plEditorUi::FontSize - 1);
  }

  plUiBoundText* BoundText(plUiElement* pParent, plStringView sText, plStringView sTint, bool bCode)
  {
    using namespace plEditorUi;

    plUiBoundText* pText = PL_DEFAULT_NEW(plUiBoundText);
    pText->m_sTextBinding = sText;
    pText->m_sTintBinding = sTint;
    pText->m_hFont = bCode ? GetCodeFont() : GetDefaultFont();
    pText->m_uiFontSize = bCode ? FontSize - 2 : FontSize - 1;
    pText->m_Color = plColorGammaUB(255, 255, 255, 255);
    pText->m_VAlign = plUiTextVAlign::Center;
    pText->m_Overflow = plUiTextOverflow::Ellipsis;
    pParent->m_Children.PushBack(pText);
    return pText;
  }

  /// A row-template button without a click event: rows are addressed by their pointer events.
  plUiButton* RowButton(plUiElement* pParent, plStringView sName)
  {
    plUiButton* pButton = plEditorUi::Button(pParent, "", sName);
    pButton->m_sClickEvent.Clear();
    pButton->ClearChildren();
    pButton->m_Color = s_Transparent;
    pButton->m_vCornerRadius = plVec4::MakeZero();
    return pButton;
  }
} // namespace

plUsdPrimTreeView::plUsdPrimTreeView(plEditorUiSurface& ref_surface, plStringView sName)
  : m_Surface(ref_surface)
  , m_sName(sName)
{
  m_PointerSubscription = m_Surface.m_PointerEvents.AddEventHandler(plMakeDelegate(&plUsdPrimTreeView::OnPointer, this));
  m_KeySubscription = m_Surface.m_KeyEvents.AddEventHandler(plMakeDelegate(&plUsdPrimTreeView::OnKey, this));

  m_pRows = PL_DEFAULT_NEW(plEditorUiRowWindow, m_Surface, ElementName("Rows"), ElementName("List"), plEditorUi::RowHeight,
    plEditorUiRowWindow::MakeRow(&plUsdPrimTreeView::MakeRowData, this));

  MeasureColumns(0);
}

plUsdPrimTreeView::~plUsdPrimTreeView()
{
  m_Surface.m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
  m_Surface.m_KeyEvents.RemoveEventHandler(m_KeySubscription);
}

plString plUsdPrimTreeView::ElementName(plStringView sSuffix) const
{
  return plStringBuilder(m_sName, ".", sSuffix);
}

//////////////////////////////////////////////////////////////////////////
// Items

void plUsdPrimTreeView::SetItems(plDynamicArray<Item>&& items)
{
  m_Items = std::move(items);

  const plUInt32 uiCount = m_Items.GetCount();
  m_Children.Clear();
  m_Children.SetCount(uiCount + 1);
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    const plUInt32 uiParent = m_Items[i].m_uiParent;
    m_Children[uiParent < i ? uiParent : uiCount].PushBack(i);
  }

  m_Expanded.Clear();
  m_Expanded.SetCount(uiCount, false);
  m_uiCurrent = plInvalidIndex;

  for (float& fWidth : m_fNaturalWidths)
    fWidth = 0.0f;
  MeasureColumns(0);

  UpdateStates();
  Flatten();
}

void plUsdPrimTreeView::Clear()
{
  SetItems(plDynamicArray<Item>());
}

void plUsdPrimTreeView::UpdateStates()
{
  // Children come after their parents, so walking backwards finishes every child before its parent.
  struct Children
  {
    bool m_bChecked = false;
    bool m_bUnchecked = false;
    bool m_bPartial = false;
  };

  const plUInt32 uiCount = m_Items.GetCount();
  plDynamicArray<Children> children;
  children.SetCount(uiCount);
  m_States.SetCount(uiCount);

  for (plUInt32 i = uiCount; i-- > 0;)
  {
    const Item& item = m_Items[i];

    Check state = Check::None;
    if (!item.m_bEnabled)
    {
      state = Check::None;
    }
    else if (item.m_bCheckable)
    {
      state = item.m_bChecked ? Check::Checked : Check::Unchecked;
    }
    else if (item.m_bAutoTristate)
    {
      // children without a box (excluded prims) do not count
      const Children& c = children[i];
      if (c.m_bPartial || (c.m_bChecked && c.m_bUnchecked))
        state = Check::Partial;
      else if (c.m_bChecked)
        state = Check::Checked;
      else if (c.m_bUnchecked)
        state = Check::Unchecked;
    }

    m_States[i] = state;

    if (item.m_uiParent < i && state != Check::None)
    {
      Children& parent = children[item.m_uiParent];
      parent.m_bChecked |= state == Check::Checked;
      parent.m_bUnchecked |= state == Check::Unchecked;
      parent.m_bPartial |= state == Check::Partial;
    }
  }
}

void plUsdPrimTreeView::SetChecked(plUInt32 uiItem, bool bChecked)
{
  // sets the boxes below without updating in between
  struct Local
  {
    static void Set(plUsdPrimTreeView& ref_tree, plUInt32 uiItem, bool bChecked)
    {
      Item& item = ref_tree.m_Items[uiItem];
      if (item.m_bCheckable)
      {
        item.m_bChecked = bChecked;
        return;
      }

      if (!item.m_bAutoTristate)
        return;

      for (plUInt32 uiChild : ref_tree.m_Children[uiItem])
      {
        if (ref_tree.m_States[uiChild] != Check::None)
          Set(ref_tree, uiChild, bChecked);
      }
    }
  };

  if (uiItem >= m_Items.GetCount() || m_States[uiItem] == Check::None)
    return;

  Local::Set(*this, uiItem, bChecked);
  UpdateStates();
  m_pRows->Refresh();
}

void plUsdPrimTreeView::SetAllChecked(bool bChecked)
{
  for (Item& item : m_Items)
  {
    if (item.m_bEnabled && item.m_bCheckable)
    {
      item.m_bChecked = bChecked;
    }
  }

  UpdateStates();
  m_pRows->Refresh();
}

bool plUsdPrimTreeView::AreAllChecked() const
{
  bool bHasCheckBox = false;
  for (const Item& item : m_Items)
  {
    if (item.m_bEnabled && item.m_bCheckable)
    {
      if (!item.m_bChecked)
      {
        return false;
      }

      bHasCheckBox = true;
    }
  }

  return bHasCheckBox;
}

void plUsdPrimTreeView::ToggleCheck(plUInt32 uiItem)
{
  if (uiItem >= m_Items.GetCount() || m_States[uiItem] == Check::None)
    return;

  // a partly checked box checks every child, as a checked one unchecks them
  SetChecked(uiItem, m_States[uiItem] != Check::Checked);
  m_ChecksChanged.Broadcast(uiItem);
}

void plUsdPrimTreeView::SetSize(plUInt32 uiItem, plStringView sSize, plStringView sToolTip)
{
  if (uiItem >= m_Items.GetCount())
    return;

  m_Items[uiItem].m_sSize = sSize;
  m_Items[uiItem].m_sSizeToolTip = sToolTip;

  m_fNaturalWidths[SizeColumn] = plMath::Max(m_fNaturalWidths[SizeColumn], plMath::Ceil(MeasureCell(sSize, true)) + 2.0f * s_fCellPadding);

  const plInt32 iRow = FindRow(uiItem);
  if (iRow >= 0)
    m_pRows->RefreshRow(static_cast<plUInt32>(iRow));
}

//////////////////////////////////////////////////////////////////////////
// Expansion and rows

void plUsdPrimTreeView::SetExpanded(plUInt32 uiItem, bool bExpanded)
{
  if (uiItem >= m_Items.GetCount() || m_Expanded[uiItem] == bExpanded)
    return;

  m_Expanded[uiItem] = bExpanded;
  Flatten();
}

void plUsdPrimTreeView::ExpandAll()
{
  for (plUInt32 i = 0; i < m_Items.GetCount(); ++i)
    m_Expanded[i] = !m_Children[i].IsEmpty();

  Flatten();
}

void plUsdPrimTreeView::ExpandToDepth(plUInt32 uiDepth)
{
  // parents come first, so their depth is known
  plDynamicArray<plUInt32> depths;
  depths.SetCount(m_Items.GetCount());

  for (plUInt32 i = 0; i < m_Items.GetCount(); ++i)
  {
    const plUInt32 uiParent = m_Items[i].m_uiParent;
    depths[i] = uiParent < i ? depths[uiParent] + 1 : 0;
    m_Expanded[i] = depths[i] <= uiDepth && !m_Children[i].IsEmpty();
  }

  Flatten();
}

void plUsdPrimTreeView::Flatten()
{
  m_Rows.Clear();

  if (!m_Children.IsEmpty())
  {
    for (plUInt32 uiItem : m_Children.PeekBack())
      AppendRows(uiItem, 0);
  }

  if (m_pRows->GetRowCount() != m_Rows.GetCount())
    m_pRows->SetRowCount(m_Rows.GetCount());
  else
    m_pRows->Refresh();
}

void plUsdPrimTreeView::AppendRows(plUInt32 uiItem, plUInt32 uiDepth)
{
  m_Rows.PushBack({uiItem, uiDepth});

  if (!m_Expanded[uiItem])
    return;

  for (plUInt32 uiChild : m_Children[uiItem])
    AppendRows(uiChild, uiDepth + 1);
}

plInt32 plUsdPrimTreeView::FindRow(plUInt32 uiItem) const
{
  for (plUInt32 i = 0; i < m_Rows.GetCount(); ++i)
  {
    if (m_Rows[i].m_uiItem == uiItem)
      return static_cast<plInt32>(i);
  }

  return -1;
}

plUInt32 plUsdPrimTreeView::GetCurrentItem() const
{
  return m_uiCurrent;
}

void plUsdPrimTreeView::SetCurrentRow(plInt32 iRow)
{
  if (m_Rows.IsEmpty())
    return;

  iRow = plMath::Clamp(iRow, 0, static_cast<plInt32>(m_Rows.GetCount()) - 1);
  m_uiCurrent = m_Rows[iRow].m_uiItem;
  m_pRows->Refresh();
  m_pRows->ScrollToRow(static_cast<plUInt32>(iRow));
}

//////////////////////////////////////////////////////////////////////////
// Columns

void plUsdPrimTreeView::MeasureColumns(plUInt32 uiFirstItem)
{
  for (plUInt32 c = 0; c < TextColumnCount; ++c)
  {
    const float fTitle = plMath::Ceil(plEditorUi::MeasureText(Title(c), plEditorUi::MinFontSize)) + 2.0f * s_fCellPadding;
    m_fNaturalWidths[c] = plMath::Max(m_fNaturalWidths[c], fTitle);
  }

  const plUInt32 uiEnd = plMath::Min(m_Items.GetCount(), uiFirstItem + s_uiMeasuredItems);
  for (plUInt32 i = uiFirstItem; i < uiEnd; ++i)
  {
    for (plUInt32 c = 0; c < TextColumnCount; ++c)
    {
      const float fCell = plMath::Ceil(MeasureCell(GetCell(m_Items[i], c), c == SizeColumn)) + 2.0f * s_fCellPadding;
      m_fNaturalWidths[c] = plMath::Max(m_fNaturalWidths[c], fCell);
    }
  }
}

void plUsdPrimTreeView::LayoutColumns(float fWidth)
{
  // Type and Size fit their content; the notes too, up to 40 % of the width; the name takes the rest
  for (plUInt32 c = 0; c < TextColumnCount; ++c)
    m_fColumnWidths[c] = plMath::Clamp(m_fNaturalWidths[c], s_fMinWidths[c], s_fMaxWidths[c]);

  m_fColumnWidths[NotesColumn] = plMath::Min(m_fColumnWidths[NotesColumn], plMath::Max(s_fMinWidths[NotesColumn], fWidth * 0.4f));

  const float fMinPrim = fWidth * 0.3f;
  const float fRest = fWidth - m_fColumnWidths[TypeColumn] - m_fColumnWidths[NotesColumn] - m_fColumnWidths[SizeColumn];
  if (fRest < fMinPrim)
    m_fColumnWidths[NotesColumn] = plMath::Max(s_fMinWidths[NotesColumn], m_fColumnWidths[NotesColumn] - (fMinPrim - fRest));

  m_fPrimWidth = plMath::Max(fWidth - m_fColumnWidths[TypeColumn] - m_fColumnWidths[NotesColumn] - m_fColumnWidths[SizeColumn], 0.0f);
}

plString plUsdPrimTreeView::GetCell(const Item& item, plUInt32 uiColumn) const
{
  switch (uiColumn)
  {
    case TypeColumn:
      return item.m_sType;
    case NotesColumn:
      return item.m_sNotes;
    default:
      return item.m_sSize;
  }
}

//////////////////////////////////////////////////////////////////////////
// Building

void plUsdPrimTreeView::Build(plUiElement* pParent, const plRectFloat& rect)
{
  using namespace plEditorUi;

  m_Rect = rect;
  LayoutColumns(rect.width);

  plUiPanel* pRoot = Panel(pParent);
  SetRect(pRoot, rect);
  pRoot->m_Color = s_Transparent;
  pRoot->m_bClipChildren = true;

  BuildHeader(pRoot);

  plUiPanel* pRow = Panel();
  pRow->m_Color = s_Transparent;
  BuildRowTemplate(pRow);

  plUiItemList* pList = List(pRoot, ElementName("Rows"), Finish(pRow), RowHeight);
  Fill(pList, plVec4(0, HeaderHeight, 0, 0));
  pList->m_sName = ElementName("List");
  pList->m_fSpacing = 0;

  // the frame goes on top, so rows and header do not cover it
  plUiPanel* pFrame = Panel(pRoot);
  Fill(pFrame);
  pFrame->m_Color = s_Transparent;
  pFrame->m_fBorderWidth = 1.0f;
  pFrame->m_BorderColor = Line();
  pFrame->m_vCornerRadius = plVec4(4.0f);

  m_pRows->SetRowCount(m_Rows.GetCount());
}

void plUsdPrimTreeView::BuildHeader(plUiElement* pRoot)
{
  using namespace plEditorUi;

  plUiPanel* pHeader = Panel(pRoot);
  pHeader->m_vAnchorMax.y = 0;
  pHeader->m_vOffsets = plVec4(0, 0, 0, HeaderHeight);
  pHeader->m_Color = Surface();

  plUiPanel* pRule = Panel(pHeader);
  pRule->m_vAnchorMin.y = 1;
  pRule->m_vOffsets = plVec4(0, -1, 0, 1);
  pRule->m_Color = Line();

  TextAt(pHeader, Title(TextColumnCount), plRectFloat(s_fCellPadding, 0, plMath::Max(m_fPrimWidth - 2.0f * s_fCellPadding, 0.0f), HeaderHeight), Dim(), MinFontSize);

  float x = m_fPrimWidth;
  for (plUInt32 c = 0; c < TextColumnCount; ++c)
  {
    plUiText* pTitle = TextAt(pHeader, Title(c), plRectFloat(x + s_fCellPadding, 0, plMath::Max(m_fColumnWidths[c] - 2.0f * s_fCellPadding, 0.0f), HeaderHeight), Dim(), MinFontSize);
    if (c == SizeColumn)
      pTitle->m_HAlign = plUiTextHAlign::Right;

    x += m_fColumnWidths[c];
  }
}

void plUsdPrimTreeView::BuildRowTemplate(plUiElement* pRoot)
{
  using namespace plEditorUi;

  // hover and hit target for the whole row
  plUiButton* pCatcher = RowButton(pRoot, ElementName("row"));
  Fill(pCatcher);
  pCatcher->m_HoverColor = Surface();
  pCatcher->m_PressedColor = Surface();

  plUiPanel* pCurrent = Panel(pRoot);
  pCurrent->m_Color = Selection();
  pCurrent->m_sVisibleBinding = "Current";

  // the keyboard cursor: a 1 px ring while keys move focus
  plUiPanel* pRing = Panel(pRoot);
  pRing->m_Color = s_Transparent;
  pRing->m_fBorderWidth = 1.0f;
  pRing->m_BorderColor = Accent();
  pRing->m_sVisibleBinding = "Ring";

  // the Prim column clips its indented content
  plUiPanel* pPrim = Panel(pRoot);
  pPrim->m_vAnchorMin = plVec2(0, 0);
  pPrim->m_vAnchorMax = plVec2(0, 1);
  pPrim->m_vOffsets = plVec4(0, 0, m_fPrimWidth, 0);
  pPrim->m_Color = s_Transparent;
  pPrim->m_bClipChildren = true;

  plUiOffsetPanel* pContent = PL_DEFAULT_NEW(plUiOffsetPanel);
  Fill(pContent);
  pContent->m_Color = s_Transparent;
  pContent->m_sOffsetBinding = "Depth";
  pContent->m_fStep = s_fIndentStep;
  pPrim->m_Children.PushBack(pContent);

  plUiButton* pDisclosure = RowButton(pContent, ElementName("disclosure"));
  pDisclosure->m_vAnchorMin = pDisclosure->m_vAnchorMax = plVec2(0, 0.5f);
  pDisclosure->m_vOffsets = plVec4(4, -8, 16, 16);
  pDisclosure->m_HoverColor = Raised();
  pDisclosure->m_sVisibleBinding = "HasChildren";
  ButtonIcon(pDisclosure, Icon::Expand, "Expanded")->m_Color = Muted();
  ButtonIcon(pDisclosure, Icon::Next, "Collapsed")->m_Color = Muted();

  // the check box: a field well, filled with the selection colour and an accent edge when checked or partly checked
  plUiButton* pCheck = RowButton(pContent, ElementName("check"));
  pCheck->m_vAnchorMin = pCheck->m_vAnchorMax = plVec2(0, 0.5f);
  pCheck->m_vOffsets = plVec4(22, -8, 16, 16);
  pCheck->m_HoverColor = Raised();
  pCheck->m_vCornerRadius = plVec4(3.0f);
  pCheck->m_sVisibleBinding = "HasBox";

  plUiPanel* pBox = Panel(pCheck);
  SetRect(pBox, plRectFloat(1, 1, s_fBox, s_fBox));
  pBox->m_Color = Field();
  pBox->m_fBorderWidth = 1.0f;
  pBox->m_BorderColor = Border();
  pBox->m_vCornerRadius = plVec4(3.0f);

  plUiPanel* pFilled = Panel(pBox);
  pFilled->m_Color = Selection();
  pFilled->m_fBorderWidth = 1.0f;
  pFilled->m_BorderColor = Accent();
  pFilled->m_vCornerRadius = plVec4(3.0f);
  pFilled->m_sVisibleBinding = "Filled";

  plUiImage* pTick = SvgImage(pBox, IconPath(Icon::Check));
  SetRect(pTick, plRectFloat(2, 2, 10, 10));
  pTick->m_Color = Accent();
  pTick->m_sVisibleBinding = "On";

  plUiPanel* pPartial = Panel(pBox);
  SetRect(pPartial, plRectFloat(3, 6, 8, 2));
  pPartial->m_Color = Accent();
  pPartial->m_sVisibleBinding = "Partial";

  plUiBoundText* pName = BoundText(pContent, "Name", "NameTint", false);
  Fill(pName, plVec4(s_fNameLeft, 0, 4, 0));
  pName->m_uiFontSize = FontSize;

  float x = m_fPrimWidth;
  for (plUInt32 c = 0; c < TextColumnCount; ++c)
  {
    plUiBoundText* pCell = BoundText(pRoot, Key("C", c), Key("T", c), c == SizeColumn);
    pCell->m_vAnchorMin = plVec2(0, 0);
    pCell->m_vAnchorMax = plVec2(0, 1);
    pCell->m_vOffsets = plVec4(x + s_fCellPadding, 0, plMath::Max(m_fColumnWidths[c] - 2.0f * s_fCellPadding, 0.0f), 0);
    if (c == SizeColumn)
      pCell->m_HAlign = plUiTextHAlign::Right;

    x += m_fColumnWidths[c];
  }

  // hairline between rows
  plUiPanel* pRule = Panel(pRoot);
  pRule->m_vAnchorMin.y = 1;
  pRule->m_vOffsets = plVec4(0, -1, 0, 1);
  pRule->m_Color = plColorGammaUB(Line().r, Line().g, Line().b, 140);
}

plVariant plUsdPrimTreeView::MakeRowData(plUInt32 uiRow) const
{
  using namespace plEditorUi;

  const Row& row = m_Rows[uiRow];
  const Item& item = m_Items[row.m_uiItem];
  const Check state = m_States[row.m_uiItem];
  const bool bHasChildren = !m_Children[row.m_uiItem].IsEmpty();
  const bool bCurrent = m_bFocused && row.m_uiItem == m_uiCurrent;

  plVariantDictionary data;
  data.Insert("Depth", static_cast<float>(row.m_uiDepth));
  data.Insert("HasChildren", bHasChildren);
  data.Insert("Expanded", bHasChildren && m_Expanded[row.m_uiItem]);
  data.Insert("Collapsed", bHasChildren && !m_Expanded[row.m_uiItem]);
  data.Insert("Current", bCurrent);
  data.Insert("Ring", bCurrent && IsFocusVisible(m_Surface.GetBlackboard()));

  data.Insert("HasBox", state != Check::None);
  data.Insert("Filled", state == Check::Checked || state == Check::Partial);
  data.Insert("On", state == Check::Checked);
  data.Insert("Partial", state == Check::Partial);

  // disabled rows are dimmed in every column
  data.Insert("Name", item.m_sName);
  data.Insert("NameTint", plColor(item.m_bEnabled ? Foreground() : Dim()));

  for (plUInt32 c = 0; c < TextColumnCount; ++c)
  {
    data.Insert(Key("C", c), GetCell(item, c));
    data.Insert(Key("T", c), plColor(item.m_bEnabled ? Muted() : Dim()));
  }

  return data;
}

//////////////////////////////////////////////////////////////////////////
// Input

bool plUsdPrimTreeView::IsOwnElement(plStringView sElement) const
{
  return sElement.StartsWith(m_sName) && sElement.GetElementCount() > m_sName.GetElementCount() && sElement.GetStartPointer()[m_sName.GetElementCount()] == '.';
}

void plUsdPrimTreeView::OnPointer(const plEditorUiPointerEvent& event)
{
  using Type = plEditorUiPointerEvent::Type;

  if (event.m_Type != Type::Press && event.m_Type != Type::DoubleClick)
    return;

  const bool bRow = IsOwnElement(event.m_sElement) && event.m_iItemIndex >= 0 && event.m_iItemIndex < static_cast<plInt32>(m_Rows.GetCount());
  if (!bRow)
  {
    // a press elsewhere takes the keys away
    if (event.m_Type == Type::Press && m_bFocused)
    {
      m_bFocused = false;
      m_pRows->Refresh();
    }
    return;
  }

  const plUInt32 uiItem = m_Rows[event.m_iItemIndex].m_uiItem;
  m_bFocused = true;
  m_uiCurrent = uiItem;

  // the second click of a double-click on the disclosure or a box toggles again, as two clicks do
  if (event.m_sElement == ElementName("disclosure"))
  {
    SetExpanded(uiItem, !m_Expanded[uiItem]);
    m_pRows->Refresh();
    return;
  }

  if (event.m_sElement == ElementName("check"))
  {
    if (event.m_Button == plEditorUiPointerEvent::Button::Left)
      ToggleCheck(uiItem);
    m_pRows->Refresh();
    return;
  }

  if (event.m_Type == Type::DoubleClick && !m_Children[uiItem].IsEmpty())
    SetExpanded(uiItem, !m_Expanded[uiItem]);

  m_pRows->Refresh();
}

void plUsdPrimTreeView::OnKey(const plEditorUiKeyEvent& event)
{
  using Key = plEditorUiKeyEvent::Key;

  if (!m_bFocused || event.m_bHandled || m_Rows.IsEmpty())
    return;

  // a text field reached with Tab keeps its keys
  const plStringView sFocused = m_Surface.GetCanvas().GetFocusedElementName();
  if (!sFocused.IsEmpty() && !IsOwnElement(sFocused))
    return;

  const plInt32 iRow = plMath::Max(0, FindRow(m_uiCurrent));
  const plInt32 iPage = plMath::Max(1, static_cast<plInt32>((m_Rect.height - HeaderHeight) / plEditorUi::RowHeight) - 1);
  const plUInt32 uiItem = m_Rows[iRow].m_uiItem;

  switch (event.m_Key)
  {
    case Key::Up:
      SetCurrentRow(iRow - 1);
      break;
    case Key::Down:
      SetCurrentRow(iRow + 1);
      break;
    case Key::PageUp:
      SetCurrentRow(iRow - iPage);
      break;
    case Key::PageDown:
      SetCurrentRow(iRow + iPage);
      break;
    case Key::Home:
      SetCurrentRow(0);
      break;
    case Key::End:
      SetCurrentRow(static_cast<plInt32>(m_Rows.GetCount()) - 1);
      break;

    case Key::Left:
      if (m_Expanded[uiItem] && !m_Children[uiItem].IsEmpty())
        SetExpanded(uiItem, false);
      else if (m_Items[uiItem].m_uiParent < uiItem)
        SetCurrentRow(FindRow(m_Items[uiItem].m_uiParent));
      break;

    case Key::Right:
      if (!m_Children[uiItem].IsEmpty() && !m_Expanded[uiItem])
        SetExpanded(uiItem, true);
      else if (!m_Children[uiItem].IsEmpty())
        SetCurrentRow(iRow + 1);
      break;

    case Key::Space:
      m_uiCurrent = uiItem;
      ToggleCheck(uiItem);
      break;

    default:
      return;
  }

  event.m_bHandled = true;
  m_pRows->Refresh();
}

//////////////////////////////////////////////////////////////////////////
// Tooltips

plString plUsdPrimTreeView::GetToolTipAt(const plVec2& vPosition, plStringView sDefault) const
{
  if (vPosition.x < 0.0f || vPosition.x >= m_Rect.width || vPosition.y < HeaderHeight || vPosition.y >= m_Rect.height)
    return sDefault;

  const plInt32 iRow = static_cast<plInt32>((vPosition.y - HeaderHeight + m_pRows->GetScrollOffset()) / plEditorUi::RowHeight);
  if (iRow < 0 || iRow >= static_cast<plInt32>(m_Rows.GetCount()))
    return sDefault;

  const Item& item = m_Items[m_Rows[iRow].m_uiItem];
  if (vPosition.x < m_fPrimWidth)
    return item.m_sToolTip.IsEmpty() ? plString(sDefault) : item.m_sToolTip;

  const float fSizeLeft = m_Rect.width - m_fColumnWidths[SizeColumn];
  if (vPosition.x >= fSizeLeft && !item.m_sSizeToolTip.IsEmpty())
    return item.m_sSizeToolTip;

  return sDefault;
}
