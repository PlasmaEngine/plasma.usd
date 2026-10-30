#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <ToolsFoundation/Object/DocumentObjectPolicy.h>

/// \brief Keeps the objects a USD link generates in step with the stage.
///
/// Generated objects cannot be moved to another parent or deleted by the user, because neither can be written back as
/// USD. Deleting one deactivates its prim in this editor's layer instead, which the stage and every other tool see; the
/// object stays, hidden and dimmed in the scene tree, so it can be reactivated. Deleting a link's root object asks
/// whether to unlink the stage first.
class plUsdObjectPolicy : public plDocumentObjectPolicy
{
public:
  virtual plStatus CanRemove(const plDocumentObject* pObject) const override;
  virtual plStatus CanMove(const plDocumentObject* pObject, const plDocumentObject* pNewParent, plStringView sParentProperty, const plVariant& index) const override;
  virtual void FilterDeleteRequest(const plDocument* pDocument, plDeque<const plDocumentObject*>& inout_objects) const override;
};
