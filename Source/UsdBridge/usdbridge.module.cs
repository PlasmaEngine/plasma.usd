using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

// Everything that includes <pxr/...> lives in this module. Its public headers expose only engine types, so the editor
// plugin that uses it never sees OpenUSD's headers or needs its build settings.
//
// Compiled into each target that lists it in ExtraModules (the editor plugin and plUsdInspect), not built as its own DLL.
public class UsdBridgeModule : ModuleRules
{
    public UsdBridgeModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/UsdBridge",
            "UsdBridgePCH.h", "BUILDSYSTEM_BUILDING_USDBRIDGE_LIB");

        PlasmaPackageOpenUsd.ConfigureConsumer(this, context, bCopyRuntime: true);
        PublicDependencies.Add("UsdMikkModule");

        // .mtlx layers: plasma.materialx reads a document into plModelImporter2 material graphs, which ModelImporter2's
        // resolver lowers onto OpenPBR (UsdMtlxReader.cpp).
        PlasmaPackageSdk.AddImportLibrary(this, context, "ModelImporter2");

        // OpenUSD's headers use RTTI (TfType) and some of its API reports failure by throwing.
        EnableExceptions = true;
        EnableRTTI = true;

        // Geometry conversion runs per face corner and over large USD arrays. Keep the Development ABI and
        // assertions, but optimize these loops instead of inheriting the workspace's unoptimized defaults.
        if (context.Configuration == BuildConfiguration.Development)
        {
            OptimizeCode = OptimizationLevel.Shipping;
        }
    }
}
