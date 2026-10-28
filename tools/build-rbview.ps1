# Build the standalone viewer with the same Smartforce sources as rbsim.
param(
    [string]$Cxx = 'g++',
    [string]$Windres = 'windres',
    [string]$Output = (Join-Path (Split-Path $PSScriptRoot -Parent) 'rbview.exe')
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$Output = [IO.Path]::GetFullPath($Output)
$buildDir = Join-Path ([IO.Path]::GetDirectoryName($Output)) 'rbview-build'
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
$resource = Join-Path $buildDir 'rbview.res.o'
Push-Location (Join-Path $repo 'viewer')
try {
    & $Windres -O coff -o $resource rbview.rc
    if ($LASTEXITCODE -ne 0) { throw 'Viewer resource compilation failed' }
} finally { Pop-Location }
$sources = @(
    'viewer/rbview.cpp', 'viewer/tas.cpp', 'viewer/smartforce_config.cpp', 'viewer/smartforce_ui.cpp',
    'src/libm_intel.S', 'src/b2collision.cpp', 'src/b2world.cpp', 'src/b2joints.cpp',
    'src/redball.cpp', 'src/flashpix.cpp', 'src/snapshot.cpp',
    'src/rbsim.cpp', 'src/verify.cpp', 'src/calib.cpp', 'src/smartforce.cpp'
) | ForEach-Object { Join-Path $repo $_ }
# These floating-point flags are required for the accepted port. Do not use fast-math or FMA contraction.
$flags = @('-O2','-g','-pthread','-std=c++17','-Wall','-Wextra','-ffp-contract=off','-fno-fast-math',
    '-fexcess-precision=standard','-D__USE_MINGW_ANSI_STDIO=1','-DRBSIM_EMBEDDED','-static')
# Reuse objects only with an identical compiler, build script and complete header set.
$compiler = (Get-Command $Cxx -ErrorAction Stop).Source
$signature = @((Get-FileHash -LiteralPath $compiler).Hash, (Get-FileHash -LiteralPath $PSCommandPath).Hash)
$signature += Get-ChildItem -LiteralPath (Join-Path $repo 'src'),(Join-Path $repo 'viewer') -Filter '*.h' | Sort-Object FullName | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName).Hash }
$signatureText = $signature -join "`n"
$signaturePath = Join-Path $buildDir 'signature.txt'
$rebuild = -not (Test-Path -LiteralPath $signaturePath)
if (-not $rebuild) { $rebuild = [IO.File]::ReadAllText($signaturePath) -cne $signatureText }
$objects = @()
foreach ($source in $sources) {
    $object = Join-Path $buildDir ((Split-Path $source -Leaf) + '.o')
    $hashPath = $object + '.sha256'
    $hash = (Get-FileHash -LiteralPath $source).Hash
    if ($rebuild -or -not (Test-Path -LiteralPath $object) -or -not (Test-Path -LiteralPath $hashPath) -or [IO.File]::ReadAllText($hashPath) -cne $hash) {
        Write-Output ('Compiling ' + (Split-Path $source -Leaf))
        & $Cxx @flags -c $source -o $object
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $source" }
        [IO.File]::WriteAllText($hashPath, $hash)
    }
    $objects += $object
}
[IO.File]::WriteAllText($signaturePath, $signatureText)
& $Cxx @flags -municode -mwindows -o $Output @objects $resource `
    -lgdiplus -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -lole32 -lshell32
if ($LASTEXITCODE -ne 0) { throw 'Viewer compilation failed' }
Write-Output "Built $Output"
