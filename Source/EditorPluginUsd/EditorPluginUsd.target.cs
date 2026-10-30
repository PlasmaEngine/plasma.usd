using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginUsdTarget : TargetRules
{
    public EditorPluginUsdTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEditorPluginUsd";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        UseIncrementalLinking = true;

        // OpenUSD's headers open a versioned namespace by macro and are sensitive to include order; merging them into
        // one translation unit with the plugin's own code is asking for collisions that read as OpenUSD bugs.
        UseUnityBuild = false;

        ExtraModules.Add("EditorPluginUsdModule");
        ExtraModules.Add("UsdBridgeModule");
    }
}
