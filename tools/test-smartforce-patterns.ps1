# Run against a rebuilt binary. This script never builds or replaces an executable.
param(
    [Parameter(Mandatory=$true)][string]$OutDir,
    [string]$Binary = (Join-Path (Split-Path $PSScriptRoot -Parent) 'rbsim.exe'),
    [string]$BaselineBinary
)
$ErrorActionPreference = 'Stop'
$Binary = (Resolve-Path -LiteralPath $Binary).Path
$runDir = Join-Path $OutDir ('smartforce-patterns-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$script:serial = 0

function List-Routes([string]$pattern, [string]$budget = '0.1', [string[]]$extra = @(), [string]$exe = $Binary) {
    $script:serial++
    $file = Join-Path $runDir ("list-$script:serial.txt")
    $log = @(& $exe smartforce --level 17 --inputs $pattern --list --top 1 --maxmem $budget --save $file @extra 2>&1 | ForEach-Object { $_.ToString() })
    $log | Set-Content -LiteralPath "$file.log"
    $script:lastListLog = $log
    if ($LASTEXITCODE -ne 0) { throw "Failed pattern $pattern`: $($log -join ' ')" }
    Get-Content -LiteralPath $file | Where-Object { $_ -cmatch '^(?:[ndweaSqW][0-9]+)+$' }
}

function Same([string[]]$actual, [string[]]$expected, [string]$name) {
    if (($actual -join "`n") -cne ($expected -join "`n")) { throw "Mismatch: $name" }
    Write-Output "PASS $name ($($actual.Count) routes)"
}

function Decode([string]$route) {
    $s = [Text.StringBuilder]::new()
    foreach ($m in [regex]::Matches($route, '([ndweaSqW])([0-9]+)')) {
        [void]$s.Append([char]$m.Groups[1].Value, [int]$m.Groups[2].Value)
    }
    $s.ToString()
}

# Count and sum signatures from every subset, independent of the generator's placement formula.
function Check-Sums([int]$length, [int]$low, [int]$high, [int]$percent, [string]$separator, [int]$minimumPercent = 0) {
    $all = @{}
    $maximum = 0
    for ($mask = 0; $mask -lt (1 -shl $length); $mask++) {
        $count = 0; $sum = 0
        for ($i = 0; $i -lt $length; $i++) {
            if ($mask -band (1 -shl $i)) { $count++; $sum += $length - $i }
        }
        if ($count -ge $low -and $count -le $high) {
            $all["${count}:$sum"] = $sum
            $maximum = [Math]::Max($maximum, $sum)
        }
    }
    $cap = [long][Math]::Floor($maximum * $percent / 100.0)
    $floor = [long][Math]::Ceiling($maximum * $minimumPercent / 100.0)
    $expected = @($all.Keys | Where-Object { $all[$_] -ge $floor -and $all[$_] -le $cap } | Sort-Object)
    $pattern = '<d' + $length + $separator + 'j' + $low + '..' + $high + ', ' + $minimumPercent + '..' + $percent + '%>'
    $routes = @(List-Routes $pattern)
    $actual = @{}
    foreach ($route in $routes) {
        $frames = Decode $route
        if ($frames.Length -ne $length) { throw "Wrong length: $pattern" }
        $count = 0; $sum = 0
        $press = if ($separator -eq '+') { 'e' } else { 'w' }
        for ($i = 0; $i -lt $length; $i++) {
            if ($frames[$i] -ceq [char]$press) { $count++; $sum += $length - $i }
            elseif ($frames[$i] -cne [char]'d') { throw "Wrong base keys: $route" }
        }
        $key = "${count}:$sum"
        if ($actual.ContainsKey($key)) { throw "Duplicate placement signature: $pattern $key" }
        $actual[$key] = $true
    }
    Same @($actual.Keys | Sort-Object) $expected $pattern
}

$fixture = Join-Path $runDir 'routes with spaces.txt'
@('# fixture', 'd2e1', 'a1', 'd2e2', 'n0', 'd2e1') | Set-Content -LiteralPath $fixture
Same @(List-Routes "{$fixture}[-1]") @('n0','d2','d2e1') 'trim, dedupe, and empty strings'
Same @(List-Routes "{$fixture}[+2]") @('n2','a1n2','d2e1n2','d2e2n2') 'append no-input frames'
Same @(List-Routes "{$fixture}[-2][+1]") @('n1','d1n1','d2n1') 'chained suffixes'
Same @(List-Routes "{${fixture}:2}[-1]") @('n0','d2') 'top selection before trimming'
Same @(List-Routes "({$fixture}[-1])=2") @('d2') 'length filter after trimming'
Same @(List-Routes "{$fixture}[-100]d1") @('d1') 'trim past beginning, then continue'
Same @(List-Routes "{$fixture}[+0][-0]") @(List-Routes "{$fixture}") 'zero suffixes'
$empty = Join-Path $runDir 'empty.txt'
@(List-Routes "{$fixture}[-100]") | Set-Content -LiteralPath $empty
Same @(List-Routes "{$empty}[+2]") @('n2') 'empty route round trip'

foreach ($separator in @(':','+')) {
    foreach ($percent in @(0,1,25,50,99,100)) {
        Check-Sums 8 0 8 $percent $separator
    }
    Check-Sums 10 1 2 50 $separator
    Check-Sums 8 2 4 75 $separator
    Check-Sums 4 1 8 50 $separator
    Check-Sums 10 1 2 50 $separator 25
    Check-Sums 8 0 8 75 $separator 25
    Check-Sums 8 0 8 50 $separator 50
    Check-Sums 8 0 8 100 $separator 100
}
Same @(List-Routes '<d0:j0, 0%>') @('n0') 'zero-length jump base'
Same @(List-Routes '<d4:j4, 100%>') @('w4') 'all frames pressed'
$pattern = '<d30:j0..30, 25..50%>'
$stored = @(List-Routes $pattern)
Same @(List-Routes $pattern '0.0001') $stored 'lazy versus stored jump generation'
Same @(List-Routes $pattern '0.0001' @('--stochastic','--seed','321') | Sort-Object) @($stored | Sort-Object) 'stochastic complete domain'
Same @(List-Routes '<d10:j1..2, 50%>') @(List-Routes '<d10:j1..2, 0..50%>') 'single percentage shorthand'
Same @(List-Routes '<d3..8+j0..3, 25..50%>') @((3..8 | ForEach-Object { List-Routes ('<d' + $_ + '+j0..3, 25..50%>') })) 'range calculated per base length'
Same @(List-Routes '<d3..8+j0..3, 50%>') @((3..8 | ForEach-Object { List-Routes ('<d' + $_ + '+j0..3, 50%>') })) 'cap calculated per base length'

$null = @(List-Routes '<d10:j1..2, 25..50%><n4+j1>')
Same @($script:lastListLog | Where-Object { $_ -match '^jump ' }) @(
    'jump 1 - min: d5w1d4, max: d2w1d6w1',
    'jump 2 - min: n3w1, max: w1n3'
) 'startup extrema per block'
$null = @(List-Routes '<d3..8:j0..3, 25..50%>')
Same @($script:lastListLog | Where-Object { $_ -match '^jump ' }) @(
    'jump 1 - min: d1w1d1, max: d1w1d4w2'
) 'startup extrema across lengths'
$null = @(List-Routes '<<d4:j1>:j1>')
Same @($script:lastListLog | Where-Object { $_ -match '^jump ' }) @(
    'jump 1 - min: w1d2w1, max: w1d3',
    'jump 2 - min: d3w1, max: w1d3'
) 'nested blocks numbered by opening bracket'
$null = @(List-Routes '<d0:j0, 0%>')
Same @($script:lastListLog | Where-Object { $_ -match '^jump ' }) @('jump 1 - min: n0, max: n0') 'empty startup witnesses'
$null = @(List-Routes '(<d4:j4, 99%>|d1)')
Same @($script:lastListLog | Where-Object { $_ -match '^jump ' }) @('jump 1 - min: none, max: none') 'empty alternative block'
$null = @(List-Routes '<d30:j0..30, 25..50%>' '0.1')
$storedSummary = @($script:lastListLog | Where-Object { $_ -match '^jump ' })
$null = @(List-Routes '<d30:j0..30, 25..50%>' '0.0001')
Same @($script:lastListLog | Where-Object { $_ -match '^jump ' }) $storedSummary 'lazy and stored startup witnesses'

foreach ($pattern in @('<d4:j1, 101%>','<d4:j1, -1%>','<d4:j1, 1.5%>','<d4:j1, 50>','<d4:j1,w1>',
        "{$fixture}[1]", "{$fixture}[-]", "{$fixture}[+1.5]", "{$fixture}[+1000001]", '<d4:j4, 99%>',
        '<d4:j1, 50..25%>', '<d4:j1, 0..101%>', '<d4:j1, 25..%>', '<d4:j1, 25.5..50%>',
        '<d10:j1..2, 50..50%>')) {
    $log = @(& $Binary smartforce --level 17 --inputs $pattern --list 2>&1 | ForEach-Object { $_.ToString() })
    if ($LASTEXITCODE -ne 2) { throw "Expected rejection: $pattern ($($log -join ' '))" }
    Write-Output "PASS rejected $pattern"
}

if ($BaselineBinary) {
    $BaselineBinary = (Resolve-Path -LiteralPath $BaselineBinary).Path
    foreach ($pattern in @('<d10:j1..2>','<d8+j>','<n30+j0..30>','<d3..8:j0..3>')) {
        $original = @(List-Routes $pattern '0.0001' @() $BaselineBinary)
        Same @(List-Routes $pattern '0.0001') $original "unchanged syntax $pattern"
        Same @(List-Routes ($pattern.Substring(0,$pattern.Length-1) + ', 100%>') '0.0001') $original "100% preserves old output $pattern"
        Same @(List-Routes ($pattern.Substring(0,$pattern.Length-1) + ', 0..100%>') '0.0001') $original "0..100% preserves old output $pattern"
    }
}
Write-Output "All checks passed. Results: $runDir"
