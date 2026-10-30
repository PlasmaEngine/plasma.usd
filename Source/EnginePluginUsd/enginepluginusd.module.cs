using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EnginePluginUsdModule : ModuleRules
{
    public EnginePluginUsdModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EnginePluginUsd",
            "EnginePluginUsdPCH.h", "BUILDSYSTEM_BUILDING_ENGINEPLUGINUSD_LIB");

        // Loaded into the editor's engine process only. It binds to the engine and to the scene export hook
        // (plSceneExportModifier), and to nothing of the editor, Qt or OpenUSD: it works on tags and game objects.
        PlasmaPackageSdk.AddImportLibrary(this, context, "EditorEngineProcessFramework");
    }
}
