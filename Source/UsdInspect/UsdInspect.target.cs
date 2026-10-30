using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

// Command-line check of the OpenUSD bridge without the editor: the basis self-test, plugin discovery, reading a stage,
// tessellating its meshes, and a write-back round trip through a live session.
public class UsdInspectTarget : TargetRules
{
    public UsdInspectTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputName = "plUsdInspect";

        // Kept out of the package binary directory, which is what a local install copies into a project. Run it with
        // that directory (usd_ms.dll) and the SDK's Binaries directory on PATH.
        OutputDirectory = Path.Combine(context.ProjectRoot, "Bin", "Tools", $"{context.Platform}_x64_{context.Configuration}");

        UsePCHFiles = true;
        UseUnityBuild = false;
        UseIncrementalLinking = true;
        GenerateDebugInfo = true;

        ExtraModules.Add("UsdInspectModule");
        ExtraModules.Add("UsdBridgeModule");
    }
}
