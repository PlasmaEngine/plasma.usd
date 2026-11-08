param(
    [Parameter(Mandatory)][string]$EngineRoot,
    [string]$BazaarStage
)

$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $packageRoot 'Bin/Tests'
$binaryDirectory = Join-Path $packageRoot 'Bin/Windows_x64_Development'
$engineBinaryDirectory = Join-Path $EngineRoot 'Binaries/Windows_Development'
$previousPath = $env:PATH
$previousSdk = $env:PLASMA_SDK_ROOT
$null = Get-Command cl.exe, link.exe -ErrorAction Stop
$null = New-Item -ItemType Directory -Path $outputDirectory -Force

Push-Location $packageRoot

try {
    $env:PLASMA_SDK_ROOT = $EngineRoot
    $env:PATH = "$binaryDirectory;$engineBinaryDirectory;$previousPath"

    foreach ($target in @('EditorPluginUsd', 'UsdInspect')) {
        & plasmabuild build $target -c Development --no-color

        if ($LASTEXITCODE -ne 0) {
            throw "Building $target failed."
        }
    }

    $compileResponse = Join-Path $packageRoot 'Intermediate/Windows_Development/EditorPluginUsdModule/EditorPluginUsd.cpp.compile.rsp'
    $flags = Get-Content -LiteralPath $compileResponse | Where-Object {
        $_ -match '^/[ID]' -and $_ -notmatch 'BUILDSYSTEM_BUILDING_EDITORPLUGINUSD_LIB'
    }
    $objectFile = Join-Path $outputDirectory 'LightMapping.obj'
    $executable = Join-Path $outputDirectory 'LightMapping.exe'
    $response = Join-Path $outputDirectory 'LightMapping.rsp'
    $arguments = @('/nologo', '/c', '/std:c++20', '/EHsc', '/MD', "/Fo`"$objectFile`"", "`"$PSScriptRoot/LightMapping.cpp`"") + $flags
    [IO.File]::WriteAllLines($response, $arguments)

    & cl.exe "@$response"

    if ($LASTEXITCODE -ne 0) {
        throw 'Compiling LightMapping failed.'
    }

    $inspectLinkResponse = Join-Path $packageRoot 'Bin/Tools/Windows_x64_Development/plUsdInspect.link.rsp'
    $linkFlags = Get-Content -LiteralPath $inspectLinkResponse | Where-Object {
        $_ -notmatch '^/(OUT|PDB|INCREMENTAL)' -and $_ -notmatch '[\\/]UsdInspectModule[\\/]'
    }
    $linkResponse = Join-Path $outputDirectory 'LightMapping.link.rsp'
    $linkArguments = @("/out:`"$executable`"", "`"$objectFile`"", "`"$binaryDirectory/plEditorPluginUsd.lib`"", "`"$engineBinaryDirectory/plToolsFoundation.lib`"") + $linkFlags
    [IO.File]::WriteAllLines($linkResponse, $linkArguments)

    & link.exe "@$linkResponse"

    if ($LASTEXITCODE -ne 0) {
        throw 'Linking LightMapping failed.'
    }

    if ($BazaarStage) {
        & $executable $BazaarStage
    }
    else {
        & $executable
    }

    if ($LASTEXITCODE -ne 0) {
        throw "LightMapping failed with exit code $LASTEXITCODE."
    }

    & $executable --range (Join-Path $PSScriptRoot 'LightRangeRegression.usda') $outputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Light range regression failed with exit code $LASTEXITCODE."
    }

    $inspect = Join-Path $packageRoot 'Bin/Tools/Windows_x64_Development/plUsdInspect.exe'

    & $inspect --tangent-test
    if ($LASTEXITCODE -ne 0) {
        throw 'Tangent regression failed.'
    }

    $streamObject = Join-Path $outputDirectory 'Streaming.obj'
    $streamExe = Join-Path $outputDirectory 'Streaming.exe'
    $streamResponse = Join-Path $outputDirectory 'Streaming.rsp'
    [IO.File]::WriteAllLines($streamResponse, @('/nologo', '/c', '/std:c++20', '/EHsc', '/MD', "/Fo`"$streamObject`"", "`"$PSScriptRoot/Streaming.cpp`"") + $flags)
    & cl.exe "@$streamResponse"
    if ($LASTEXITCODE -ne 0) {
        throw 'Compiling Streaming failed.'
    }

    $streamLink = Join-Path $outputDirectory 'Streaming.link.rsp'
    [IO.File]::WriteAllLines($streamLink, @("/out:`"$streamExe`"", "`"$streamObject`"", "`"$binaryDirectory/plEditorPluginUsd.lib`"", "`"$engineBinaryDirectory/plToolsFoundation.lib`"", "`"$engineBinaryDirectory/EditorFramework.lib`"") + $linkFlags)
    & link.exe "@$streamLink"
    if ($LASTEXITCODE -ne 0) {
        throw 'Linking Streaming failed.'
    }

    & $streamExe (Join-Path $outputDirectory 'CacheManifest/bake.json')
    if ($LASTEXITCODE -ne 0) {
        throw "Streaming regression failed with exit code $LASTEXITCODE."
    }

    & $streamExe --scheduler (Join-Path $PSScriptRoot 'StreamingRegression.usda') $outputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Live scheduler regression failed with exit code $LASTEXITCODE."
    }

    foreach ($fixture in @('LightRegression.usda', 'LightRegressionY.usda')) {
        & $inspect (Join-Path $PSScriptRoot $fixture) --light-test

        if ($LASTEXITCODE -ne 0) {
            throw "$fixture failed."
        }
    }
}
finally {
    $env:PATH = $previousPath
    $env:PLASMA_SDK_ROOT = $previousSdk
    Pop-Location
}
