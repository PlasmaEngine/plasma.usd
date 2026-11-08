using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class UsdMeshoptModule : ModuleRules
{
    public UsdMeshoptModule(BuildContext context) : base(context)
    {
        Type = ModuleType.ThirdParty;
        SourceDirectory = Path.Combine(PlasmaPackageSdk.Root, "Code", "ThirdParty", "meshoptimizer");
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Code", "ThirdParty"));
        ExportsSymbols = false;
        CppStandard = CppStandard.Cpp20;
        OptimizeCode = OptimizationLevel.Shipping;
    }
}
