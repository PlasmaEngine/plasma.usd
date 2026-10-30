using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class UsdInspectModule : ModuleRules
{
    public UsdInspectModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/UsdInspect",
            "UsdInspectPCH.h", "BUILDSYSTEM_BUILDING_USDINSPECT_LIB");

        PublicDependencies.Add("UsdBridgeModule");

        // --profile builds meshlet LODs the way the live link does.
        PlasmaPackageSdk.AddImportLibrary(this, context, "ModelImporter2");
    }
}
