# Run after building rbsim.exe. OutDir receives a fresh subdirectory with test results.
param(
    [Parameter(Mandatory=$true)][string]$OutDir,
    [string]$Binary = (Join-Path (Split-Path $PSScriptRoot -Parent) 'rbsim.exe')
)
$ErrorActionPreference = 'Stop'
$Binary = (Resolve-Path -LiteralPath $Binary).Path
$runDir = Join-Path $OutDir ('smartforce-save-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDir | Out-Null

function Check-Save([string]$name, [int]$length, [int]$cutoff, [int]$threads, [string]$budget, [string[]]$extra) {
    $file = Join-Path $runDir ($name + '.txt')
    $arguments = @('smartforce','--level','17','--inputs',('<d' + $length + ':a0..' + $length + '>'),
        '--report',('alive@' + $cutoff),'--idle-up','--threads',"$threads",'--maxmem',$budget,
        '--update','0','--top','3','--save',$file) + $extra
    $log = @(& $Binary @arguments 2>&1 | ForEach-Object { $_.ToString() })
    if ($LASTEXITCODE -ne 0) { throw "$name failed: $($log -join [Environment]::NewLine)" }
    $log | Set-Content -LiteralPath (Join-Path $runDir ($name + '.log'))
    $routes = @(Get-Content -LiteralPath $file | ForEach-Object { $_.Trim() } | Where-Object { $_ -cmatch '^(?:[ad][1-9][0-9]*)+$' })
    $unique = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($route in $routes) {
        $decoded = [Text.StringBuilder]::new()
        foreach ($token in [regex]::Matches($route,'([ad])([0-9]+)')) {
            [void]$decoded.Append([char]$token.Groups[1].Value,[int]$token.Groups[2].Value)
        }
        if ($decoded.Length -ne $cutoff -or -not $unique.Add($decoded.ToString())) { throw "$name has an invalid or duplicate route" }
    }
    $expected = [int][Math]::Pow(2,$cutoff)
    if ($routes.Count -ne $expected) { throw "$name lost routes: $($routes.Count) of $expected" }
    if ($extra -notcontains '--list') {
        $rows = @(Get-Content -LiteralPath $file | Where-Object { $_ -match '^\s*\d+\s+' })
        $multiplicity = [int][Math]::Pow(2,$length-$cutoff)
        foreach ($row in $rows) {
            if ([int](($row.Trim() -split '\s+')[-1]) -ne $multiplicity) { throw "$name lost duplicate-prefix counts" }
        }
        if ($rows.Count -ne $expected) { throw "$name lost result metadata" }
    }
    if (Test-Path -LiteralPath ($file + '.parts')) { throw "$name leaked temporary batches" }
    if (@($log | Where-Object { $_ -cmatch '^(?:[ad][0-9]+)+$|^\s*\d+\s+.*\s[ad][0-9]' }).Count -ne 3) {
        throw "$name did not preserve the console's --top 3 limit"
    }
    Write-Output "PASS $name`: all $expected distinct strings saved"
}

Check-Save 'single-thread-spill' 14 14 1 '0.003' @()
Check-Save 'twelve-thread-spill' 15 15 12 '0.025' @()
Check-Save 'stochastic-spill' 14 14 12 '0.025' @('--stochastic','--seed','123')
Check-Save 'prefix-counts' 15 11 12 '0.025' @('--stochastic','--seed','456')
Check-Save 'list-save' 12 12 1 '0.025' @('--list')
Write-Output "All checks passed. Results: $runDir"
