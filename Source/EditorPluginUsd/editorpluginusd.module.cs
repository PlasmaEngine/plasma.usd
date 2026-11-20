using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginUsdModule : ModuleRules
{
    public EditorPluginUsdModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EditorPluginUsd",
            "EditorPluginUsdPCH.h", "BUILDSYSTEM_BUILDING_EDITORPLUGINUSD_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);

        if (context.Configuration == BuildConfiguration.Development)
        {
            OptimizeCode = OptimizationLevel.Shipping;
        }

        // The live hub is a local TCP server on Winsock.
        PrivateLibraries.Add("Ws2_32.lib");

        // The Apps tab reads where Houdini, Maya and 3ds Max are installed from the registry.
        PrivateLibraries.Add("Advapi32.lib");

        // The Apps tab shows each tool with its executable's icon.
        PrivateLibraries.Add("User32.lib");
        PrivateLibraries.Add("Gdi32.lib");

        // Everything that touches OpenUSD is in UsdBridge, compiled into this plugin.
        PublicDependencies.Add("UsdBridgeModule");
        PublicDependencies.Add("UsdMeshoptModule");

        // Importing a stage as a world partition writes cells of the scene plugin's partitions.
        PublicIncludePaths.Add(System.IO.Path.Combine(PlasmaPackageSdk.Root, "Code", "EditorPlugins", "Scene"));
        PlasmaPackageSdk.AddImportLibrary(this, context, "plEditorPluginScene");

        // The property grid's USD header and row icons are engine UI elements.
        PlasmaPackageSdk.AddImportLibrary(this, context, "plEditorUi");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plGameUiPlugin");
    }
}
