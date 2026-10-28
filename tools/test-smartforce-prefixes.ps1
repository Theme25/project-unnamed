# Run only against a separately rebuilt candidate. This script does not compile or replace any executable.
param(
    [Parameter(Mandatory=$true)][string]$OutDir,
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$BaselineBinary
)
$ErrorActionPreference = 'Stop'
$Binary = (Resolve-Path -LiteralPath $Binary).Path
$BaselineBinary = (Resolve-Path -LiteralPath $BaselineBinary).Path
$runDir = Join-Path $OutDir ('smartforce-prefixes-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$fixture = Join-Path $runDir 'prefixes with spaces.txt'
@('n40', 'n39d1', 'd1n39') | Set-Content -LiteralPath $fixture

function Check([string]$name, [string]$pattern, [string]$report, [int]$threads,
               [string]$cacheMode = 'on', [string[]]$extra = @(), [string]$memory = '0.1') {
    $results = @()
    $candidateLog = @()
    foreach ($exe in @($BaselineBinary, $Binary)) {
        $label = if ($exe -eq $Binary) { 'new' } else { 'old' }
        $file = Join-Path $runDir ($name + '-' + $label + '.txt')
        $arguments = @('smartforce','--level','17','--inputs',$pattern,'--report',$report,
            '--threads',"$threads",'--pad','20','--top','1','--update','0','--maxmem',$memory,'--save',$file) + $extra
        $log = @(& $exe @arguments 2>&1 | ForEach-Object { $_.ToString() })
        if ($LASTEXITCODE -ne 0) { throw "$name/$label failed: $($log -join ' ')" }
        $log | Set-Content -LiteralPath ($file + '.log')
        $results += ,@(Get-Content -LiteralPath $file | Where-Object { $_ -notmatch '^#' })
        if ($label -eq 'new') { $candidateLog = $log }
    }
    if (($results[0] -join "`n") -cne ($results[1] -join "`n")) { throw "Result mismatch: $name" }
    $stats = @($candidateLog | Where-Object { $_ -match '^prefix savestates:' })
    if ($cacheMode -eq 'off') {
        if ($stats.Count) { throw "Unexpected prefix cache: $name" }
    } else {
        if ($stats.Count -ne 1 -or $stats[0] -notmatch '^prefix savestates: (\d+) built, (\d+) reused, (\d+) evicted') {
            throw "Missing prefix cache statistics: $name"
        }
        if ([long]$Matches[1] -lt 1 -or [long]$Matches[2] -lt 1) { throw "Cache not exercised: $name" }
        if ($cacheMode -eq 'evict' -and [long]$Matches[3] -lt 1) { throw "Eviction not exercised: $name" }
        if ($stats[0] -notmatch 'peak (\d+)/(\d+)') { throw "Missing cache occupancy: $name" }
        if ([long]$Matches[1] -gt $threads -or [long]$Matches[2] -ne $threads) { throw "Cache exceeded thread cap: $name" }
    }
    Write-Output "PASS $name"
}

Check 'nine-continuations' 'n40d0..8' 'alive@50' 1 'off'
Check 'ten-continuations' 'n40d0..9' 'alive@50' 1
Check 'pad-is-not-a-multiplier' 'n40d0..8' 'alive@50' 1 'off' @('--pad','100')
Check 'file-prefixes' "{$fixture}d0..10" 'alive@50' 1
Check 'twelve-threads' "{$fixture}d0..10" 'alive@50' 12
Check 'ordered-report-progress' "{$fixture}d0..10" 'alive@10; alive@50' 1
Check 'early-match-counts' "{$fixture}d0..10" 'alive@10' 12
Check 'early-failure' "{$fixture}d0..10" 'px>1e30@10' 12
Check 'same-file-after-different-pasts' "(d2|a2){$fixture}d0..10" 'alive@50' 1
Check 'trim-and-pad' "{$fixture}[-2][+3]d0..10" 'alive@50' 1
Check 'hardfilter-claims' "{$fixture}d0..10" 'alive@10; alive@50' 1 'on' @('--hardfilter','--merge-every','7','--tolerance','0')
Check 'raw-state-claims' "{$fixture}d0..10" 'alive@50' 1 'on' @('--hardfilter','--raw-states','--merge-every','1','--tolerance','0')
Check 'merge-report-original-path' "{$fixture}d0..10" 'alive@50' 1 'off' @('--hardfilter','--merge-report','--tolerance','0')
Check 'stochastic-original-path' "{$fixture}d0..10" 'alive@50' 1 'off' @('--stochastic','--seed','123')
Check 'small-cache-eviction' "{$fixture}d0..10" 'alive@50' 1 'evict' @() '0.003'

$route = 'd4n1a1e1d10e2d5n2d1a2n1d1n3d10w1d2e1d35e3d21e19d19e1d14e1d54e1w9n1d9e3d2e1d1e33d1e1d2e8d9'
Check 'crown-jump' ($route + 'e1n1d0..10') 'alive@300' 12
Check 'crown-jump-hardfilter' ($route + 'e1n1d0..10') 'alive@300' 1 'on' @('--hardfilter','--merge-every','7','--tolerance','0')
Write-Output "All checks passed. Results: $runDir"
