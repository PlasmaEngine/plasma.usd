// #pl-version 1
//
// The OpenUSD drop this package is built against, and the runtime files that have to travel with it.
//
// OpenUSD is vendored prebuilt under Source/ThirdParty/OpenUSD: a monolithic, core-only build (no Python,
// no imaging, no MaterialX plugin) plus oneTBB, compiled with the same MSVC and the release CRT the
// Development/Release configurations use. It is only ever linked by this package; nothing in the engine
// sees these headers or libraries.
//
// Debug is not supported: the engine's Debug configuration uses the debug CRT (/MDd), and the STL types in
// OpenUSD's headers would not match a release-CRT usd_ms.dll. Build Development instead.

using System;
using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaPackageOpenUsd
{
    public static string Root(BuildContext context)
    {
        return Path.Combine(context.ProjectRoot, "Source", "ThirdParty", "OpenUSD");
    }

    /// \brief Include paths and import libraries for code that includes <pxr/...> headers.
    ///
    /// When bCopyRuntime is set, usd_ms.dll, tbb12.dll, the plugInfo resource trees OpenUSD reads at
    /// startup and the licences are staged next to the package's binaries.
    public static void ConfigureConsumer(ModuleRules rules, BuildContext context, bool bCopyRuntime)
    {
        if (context.Platform != TargetPlatform.Windows)
        {
            throw new PlatformNotSupportedException("plasma.usd ships an OpenUSD build for Windows x64 only.");
        }

        if (context.Configuration == BuildConfiguration.Debug)
        {
            throw new NotSupportedException(
                "plasma.usd links a release-CRT OpenUSD build, which cannot be mixed with the Debug configuration's " +
                "debug CRT. Build the Development configuration instead.");
        }

        var root = Root(context);

        rules.PublicIncludePaths.Add(Path.Combine(root, "include"));
        rules.PublicLibraries.Add(Path.Combine(root, "lib", "usd_ms.lib"));
        rules.PublicLibraries.Add(Path.Combine(root, "lib", "tbb12.lib"));

        // oneTBB's headers ask the linker for tbb12.lib by name with #pragma comment(lib); the explicit entry
        // above already provides it, so the implicit request is switched off rather than given a search path.
        rules.PublicDefinitions.Add("__TBB_NO_IMPLICIT_LINKAGE=1");

        // windows.h's min/max macros break OpenUSD's use of std::min and std::max.
        rules.PublicDefinitions.Add("NOMINMAX");

        if (bCopyRuntime)
        {
            var binaryDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
            var bin = Path.Combine(root, "bin");

            CopyIfDifferent(Path.Combine(bin, "usd_ms.dll"), Path.Combine(binaryDirectory, "usd_ms.dll"));
            CopyIfDifferent(Path.Combine(bin, "tbb12.dll"), Path.Combine(binaryDirectory, "tbb12.dll"));

            // OpenUSD finds its schema and file-format registrations through plugInfo.json files. The
            // monolithic library looks for "usd/" next to itself; the plugin loads "plugin/usd/" explicitly.
            CopyTree(Path.Combine(bin, "usd"), Path.Combine(binaryDirectory, "usd"));
            CopyTree(Path.Combine(bin, "plugin", "usd"), Path.Combine(binaryDirectory, "plugin", "usd"));

            // The package's own codeless schemas (PlasmaComponentAPI) join the plugin tree, whose plugInfo.json includes
            // every "*/resources/" folder beside it.
            CopyTree(Path.Combine(context.ProjectRoot, "Schemas", "plasmaSchemas"), Path.Combine(binaryDirectory, "plugin", "usd", "plasmaSchemas"));

            // The declaration of the .mtlx file format, whose implementation plUsdMtlx::RegisterFileFormat defines at run time.
            // It lives beside that code rather than in Schemas/, which DCCs put on their plugin path next to their own usdMtlx.
            CopyTree(Path.Combine(context.ProjectRoot, "Source", "UsdBridge", "plasmaMtlx"), Path.Combine(binaryDirectory, "plugin", "usd", "plasmaMtlx"));

            // Apache-2.0 (OpenUSD, oneTBB) asks that the licence and notice travel with the binaries.
            CopyIfDifferent(Path.Combine(root, "LICENSE-OpenUSD.txt"), Path.Combine(binaryDirectory, "OpenUSD-LICENSE.txt"));
            CopyIfDifferent(Path.Combine(root, "NOTICE-OpenUSD.txt"), Path.Combine(binaryDirectory, "OpenUSD-NOTICE.txt"));
            CopyIfDifferent(Path.Combine(root, "LICENSE-oneTBB.txt"), Path.Combine(binaryDirectory, "oneTBB-LICENSE.txt"));

            // The Blender, Houdini, Maya and 3ds Max connectors, which the USD Live panel's Apps tab installs into those tools. Staged
            // beside the plugin so an installed package has them; Python caches from running them in place stay behind.
            CopyTree(Path.Combine(context.ProjectRoot, "Connectors"), Path.Combine(binaryDirectory, "connectors"),
                relative => !relative.Contains("__pycache__") && !relative.EndsWith(".pyc", StringComparison.OrdinalIgnoreCase));
        }
    }

    private static void CopyTree(string sourceDirectory, string targetDirectory, Func<string, bool> include = null)
    {
        if (!Directory.Exists(sourceDirectory))
        {
            throw new DirectoryNotFoundException($"Missing OpenUSD resource directory '{sourceDirectory}'.");
        }

        foreach (var file in Directory.EnumerateFiles(sourceDirectory, "*", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(sourceDirectory, file);
            if (include != null && !include(relative))
            {
                continue;
            }

            CopyIfDifferent(file, Path.Combine(targetDirectory, relative));
        }
    }

    private static void CopyIfDifferent(string source, string target)
    {
        if (!File.Exists(source))
        {
            throw new FileNotFoundException($"Missing OpenUSD runtime file '{source}'.", source);
        }

        if (File.Exists(target))
        {
            var from = new FileInfo(source);
            var to = new FileInfo(target);

            if (from.Length == to.Length && from.LastWriteTimeUtc <= to.LastWriteTimeUtc)
            {
                return;
            }
        }

        Directory.CreateDirectory(Path.GetDirectoryName(target));
        File.Copy(source, target, overwrite: true);
    }
}
