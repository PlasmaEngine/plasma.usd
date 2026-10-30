#pragma once

#include <EditorFramework/GUI/ObjectRowDecorator.h>
#include <EditorPluginUsd/EditorPluginUsdDLL.h>

struct plUsdLiveManagerEvent;

/// \brief Shows in the Scenegraph where each generated object comes from.
///
/// A prim's row shows its type as the icon, the layer that decides its values as a letter badge (P for this editor's
/// layer, the tool's initial for other participants, A for the asset), a dot when this editor's layer overrides
/// anything on it, and, when deactivated, dimmed and struck-through text. A link's root row shows the USD icon and the
/// session state. Rows of a participant picked in the USD Live panel get their letter highlighted.
class plUsdRowDecorator : public plObjectRowDecorator
{
public:
  plUsdRowDecorator();
  ~plUsdRowDecorator();

  virtual bool Decorate(const plDocumentObject* pObject, plObjectRowDecoration& inout_decoration) const override;

private:
  void ManagerEventHandler(const plUsdLiveManagerEvent& e);

  plEventSubscriptionID m_ManagerSubscription = 0;
};
