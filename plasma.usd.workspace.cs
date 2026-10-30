// #pl-version 1
//
// Makes this package directory buildable on its own, outside the engine checkout.

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaUsdWorkspace : WorkspaceRules
{
    public PlasmaUsdWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("EditorPluginUsd");
        TargetNames.Add("EnginePluginUsd");
        TargetNames.Add("UsdInspect");
    }
}
