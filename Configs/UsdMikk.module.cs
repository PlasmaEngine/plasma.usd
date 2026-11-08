using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

// Compile the SDK's unchanged MikkTSpace source into the package. Its engine DLL symbols are not exported.
public class UsdMikkModule : ModuleRules
{
    public UsdMikkModule(BuildContext context) : base(context)
    {
        Type = ModuleType.ThirdParty;
        SourceDirectory = Path.Combine(PlasmaPackageSdk.Root, "Code", "ThirdParty", "mikktspace");
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Code", "ThirdParty"));
        ExportsSymbols = false;
        CStandard = CStandard.C17;
        OptimizeCode = OptimizationLevel.Shipping;
    }
}
