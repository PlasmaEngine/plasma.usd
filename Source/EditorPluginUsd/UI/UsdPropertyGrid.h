#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <GuiFoundation/PropertyGrid/PropertyGridExtension.h>

/// \brief Marks the transform, Active and light or camera value rows of a generated object's properties: the icon is
/// lit where this editor's layer overrides the stage, and clicking it removes that opinion so the other layers' value
/// shows again.
///
/// The property grid shows the first decorator that accepts a row, so this one only accepts those rows, and only for
/// objects that come from USD.
class plUsdPropertyRowDecorator final : public plPropertyGridRowDecorator
{
public:
  virtual bool GetState(const plPropertyGridModel::Row& row, bool& out_bActive, bool& out_bMixed, plStringBuilder& out_sTooltip) const override;
  virtual void Toggle(const plPropertyGridModel::Row& row) const override;
  virtual plStringView GetIconResource() const override { return ":/UsdPlugin/Icons/UsdLive.svg"; }
};

/// \brief The "USD Prim" or "USD Link" section the property grid shows above a generated object's or a link root's
/// properties: the prim's path, type, deciding layer and state, its variant sets and its bound materials. Registered
/// with plPropertyGridHeaderExtension; it is built again whenever a link reports new prim states.
class plUsdPropertyGridHeaders
{
public:
  static void Register();
  static void Unregister();
};
