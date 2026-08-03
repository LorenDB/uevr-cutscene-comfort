param(
    [string]$Config = 'Release',
    [string]$UevrRepo = (Join-Path $PSScriptRoot '..\UEVR')
)

$ErrorActionPreference = 'Stop'

cmake -S $PSScriptRoot -B (Join-Path $PSScriptRoot 'build') `
    -G 'Visual Studio 17 2022' -A x64 `
    "-DUEVR_REPO=$UevrRepo"
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }

cmake --build (Join-Path $PSScriptRoot 'build') --config $Config
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

Write-Output "DLL: $(Join-Path $PSScriptRoot "build\bin\$Config\CutsceneComfort.dll")"
