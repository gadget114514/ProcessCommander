$ErrorActionPreference = 'Stop'

$issFile = Join-Path $PSScriptRoot 'ProcessCommander.iss'
$redistDir = Join-Path $PSScriptRoot 'redist'

$iscc = @(
    (Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

if (-not $iscc) {
    throw 'ISCC.exe was not found. Install Inno Setup 6 or 7, or add ISCC.exe to PATH.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsPath) {
        $crtDir = Get-ChildItem (Join-Path $vsPath 'VC\Redist\MSVC') -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
            Sort-Object { [version]$_.Name } -Descending |
            ForEach-Object {
                Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue
            } |
            Where-Object { Test-Path (Join-Path $_.FullName 'msvcp140.dll') } |
            Select-Object -First 1

        if ($crtDir) {
            New-Item -ItemType Directory -Force -Path $redistDir | Out-Null
            foreach ($dll in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
                Copy-Item (Join-Path $crtDir.FullName $dll) $redistDir -Force
            }
        }
    }
}

if (-not (Test-Path (Join-Path $redistDir 'msvcp140.dll'))) {
    throw "Visual C++ runtime DLLs were not found in '$redistDir'. Install the Visual Studio C++ toolchain and run this script again."
}

& $iscc $issFile
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Installer written to $(Join-Path (Split-Path -Parent $PSScriptRoot) 'dist')"
