<#
.SYNOPSIS
Packages a kNVSE build as an archive laid out like the game's Data folder, ready for a mod manager:

    NVSE\Plugins\kNVSE.dll
    NVSE\Plugins\kNVSE.pdb          (unless -NoPdb)
    menus\knvse\knvse_menu.xml      (data\menus: the kNVSE menu needs these, the DLL alone crashes it)
    menus\prefabs\knvse\*.xml

.EXAMPLE
scripts\package.ps1                         # Release build -> artifacts\kNVSE-<version>-Release.7z
scripts\package.ps1 -Build                  # build Release first
scripts\package.ps1 -Configuration Debug -NoPdb
scripts\package.ps1 -Output artifacts\kNVSE.zip   # .zip instead of .7z (the CI release)
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    # Archive to write; its extension picks the format (.7z or .zip). Default: artifacts\kNVSE-<version>-<configuration>.7z
    [string]$Output,
    # Build the configuration with MSBuild first
    [switch]$Build,
    [switch]$NoPdb
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root 'nvse_plugin_example\nvse_plugin_example.vcxproj'
$buildDir = Join-Path $root "nvse_plugin_example\$Configuration"
$menus = Join-Path $root 'data\menus'

if ($Build) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $msbuild = Get-Command msbuild -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    if (-not $msbuild -and (Test-Path $vswhere)) {
        $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    }
    if (-not $msbuild) { throw 'MSBuild not found (not on PATH, no vswhere)' }
    # The post-build step installs into the game; packaging doesn't need it
    & $msbuild $project /m /nologo /v:minimal /p:Configuration=$Configuration /p:Platform=Win32 /p:PostBuildEventUseInBuild=false
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed ($LASTEXITCODE)" }
}

$version = (Select-String -Path (Join-Path $root 'nvse_plugin_example\knvse_version.h') -Pattern '#define VERSION_MAJOR (\d+)').Matches[0].Groups[1].Value
if (-not $Output) { $Output = Join-Path $root "artifacts\kNVSE-$version-$Configuration.7z" }
$Output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
$format = [IO.Path]::GetExtension($Output).ToLowerInvariant()
if ($format -notin '.7z', '.zip') { throw "Output must end in .7z or .zip: $Output" }

# The build's files
$dll = Join-Path $buildDir 'kNVSE.dll'
$pdb = Join-Path $buildDir 'kNVSE.pdb'
if (-not (Test-Path $dll)) { throw "No $Configuration build: $dll (run with -Build)" }
if (-not $NoPdb -and -not (Test-Path $pdb)) { throw "$pdb is missing (or pass -NoPdb)" }
# The DLL says which configuration it is (the DebugTools log line): catches a Debug DLL in the Release folder
$marker = [Text.Encoding]::ASCII.GetBytes("kNVSE $Configuration build")
$bytes = [IO.File]::ReadAllBytes($dll)
$found = $false
for ($i = [Array]::IndexOf($bytes, $marker[0]); $i -ge 0 -and $i -le $bytes.Length - $marker.Length; $i = [Array]::IndexOf($bytes, $marker[0], $i + 1)) {
    $match = $true
    for ($j = 1; $j -lt $marker.Length; $j++) { if ($bytes[$i + $j] -ne $marker[$j]) { $match = $false; break } }
    if ($match) { $found = $true; break }
}
if (-not $found) { throw "$dll is not a $Configuration build (no 'kNVSE $Configuration build' in it)" }
$newestSource = Get-ChildItem (Join-Path $root 'nvse_plugin_example') -Recurse -Include *.cpp, *.h -File |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newestSource.LastWriteTime -gt (Get-Item $dll).LastWriteTime) {
    Write-Warning "$($newestSource.Name) is newer than the DLL: the build may be stale (run with -Build)"
}
$menuFiles = Get-ChildItem $menus -Recurse -File -Filter *.xml
if (-not ($menuFiles | Where-Object Name -eq 'knvse_menu.xml')) { throw "data\menus has no knvse_menu.xml" }

# Stage the Data layout
$staging = Join-Path ([IO.Path]::GetTempPath()) "kNVSE-package-$PID"
Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue
try {
    $plugins = New-Item -ItemType Directory -Force (Join-Path $staging 'NVSE\Plugins')
    Copy-Item $dll $plugins
    if (-not $NoPdb) { Copy-Item $pdb $plugins }
    Copy-Item -Recurse $menus (Join-Path $staging 'menus')
    $expected = Get-ChildItem $staging -Recurse -File | ForEach-Object { $_.FullName.Substring($staging.Length + 1) } | Sort-Object

    New-Item -ItemType Directory -Force (Split-Path $Output) | Out-Null
    Remove-Item $Output -ErrorAction SilentlyContinue
    if ($format -eq '.7z') {
        $7z = Get-Command 7z, 7za -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
        if (-not $7z -and (Test-Path "$env:ProgramFiles\7-Zip\7z.exe")) { $7z = "$env:ProgramFiles\7-Zip\7z.exe" }
        if (-not $7z) { throw '7-Zip not found (7z on PATH or Program Files\7-Zip)' }
        Push-Location $staging
        try { & $7z a -t7z -mx=9 -bso0 -bsp0 $Output * } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "7z failed ($LASTEXITCODE)" }
        # -slt: one "Path = ..." line per entry; folders have "Attributes = D..."
        $listing = & $7z l -slt -ba $Output
        $entries = @()
        $path = $null
        foreach ($line in $listing) {
            if ($line -like 'Path = *') { $path = $line.Substring(7) }
            elseif ($line -like 'Attributes = *' -and $path) {
                if ($line -notlike 'Attributes = D*') { $entries += $path }
                $path = $null
            }
        }
    }
    else {
        Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $Output
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [IO.Compression.ZipFile]::OpenRead($Output)
        try { $entries = $zip.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName -replace '/', '\' } }
        finally { $zip.Dispose() }
    }

    $entries = $entries | Sort-Object
    if (Compare-Object $expected $entries) { throw "The archive's files differ from the staged ones:`n$(Compare-Object $expected $entries | Out-String)" }
}
finally {
    Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue
}

Write-Host "$Output ($([Math]::Round((Get-Item $Output).Length / 1MB, 2)) MB, kNVSE $version $Configuration):"
$entries | ForEach-Object { Write-Host "  $_" }
