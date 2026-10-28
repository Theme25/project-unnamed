# Exercises the GUI's isolated worker against the normal CLI; never builds or replaces either executable.
param(
    [Parameter(Mandatory=$true)][string]$Viewer,
    [Parameter(Mandatory=$true)][string]$Cli,
    [Parameter(Mandatory=$true)][string]$OutDir
)
$ErrorActionPreference = 'Stop'
$Viewer = (Resolve-Path -LiteralPath $Viewer).Path
$Cli = (Resolve-Path -LiteralPath $Cli).Path
$runDir = Join-Path ([IO.Path]::GetFullPath($OutDir)) ('rbview-worker-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$script:serial = 0
function Assert([bool]$ok, [string]$message) {
    if (-not $ok) { throw $message }
    Write-Output "PASS $message"
}
function Quote-Argument([string]$s) {
    # Windows command-line quoting, not shell quoting.
    '"' + [regex]::Replace([regex]::Replace($s, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}
function Run-Worker([string[]]$Arguments, [int]$StopAfter = 0, [bool]$ParentGone = $false) {
    $script:serial++
    $eventName = 'Local\rbview-test-' + [guid]::NewGuid().ToString('N')
    $event = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $eventName)
    $log = Join-Path $runDir "worker-$script:serial.log"
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Viewer
    $start.WorkingDirectory = $runDir
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $parentId = if ($ParentGone) { '0' } else { $PID.ToString() }
    $all = @('--smartforce-worker', $eventName, $log, $parentId) + $Arguments
    $start.Arguments = ($all | ForEach-Object { Quote-Argument $_ }) -join ' '
    $proc = [Diagnostics.Process]::Start($start)
    try {
        if ($StopAfter -gt 0) { Start-Sleep -Milliseconds $StopAfter; [void]$event.Set() }
        if (-not $proc.WaitForExit(30000)) { [void]$event.Set(); throw "Worker did not finish within 30 seconds: $log" }
        [pscustomobject]@{Code=$proc.ExitCode; Text=[IO.File]::ReadAllText($log); Log=$log}
    } finally { $proc.Dispose(); $event.Dispose() }
}
function Saved-Rows([string]$path) {
    @([IO.File]::ReadAllLines($path) | Where-Object { $_ -match '^\s*[0-9]+\s|^\s*(?:[ndweaSqW][0-9]+)+\s*$' }) -join "`n"
}
Push-Location $runDir
try {
    $prefix = Join-Path $runDir "prefix's & inputs.txt"
    [IO.File]::WriteAllText($prefix, "d2n1`nd3n1`n")
    $cases = @(
        @{Name='deterministic'; Args=@('--inputs','d1..8e1d3','--report','alive@5','--idle-up')},
        @{Name='exact-exclusions'; Args=@('--inputs','(d2n1|d3n1|a3)','--report','alive@3','--idle-up','--stochastic','--seed','123','--exclude',$prefix)},
        @{Name='no-matches'; Args=@('--inputs','d1..3','--report','vx > 999')},
        @{Name='reduced-taps'; Args=@('--inputs','<d20+a1..5,r>','--report','alive@20')},
        @{Name='reduced-taps-stochastic'; Args=@('--inputs','<d20+a1..5,r,30..70%>','--report','alive@20','--stochastic','--seed','123','--update','0')},
        @{Name='reduced-taps-list'; Args=@('--inputs',("<{"+$prefix+"}[+2]:a0..2,r>"),'--list','--top','1')},
        @{Name='new-file-and-jump-syntax'; Args=@('--inputs',("{"+$prefix+"}[-1][+2]<d6+j1..3,20..70%>"),'--report','vx > -100@10, vy < 100','--hardfilter','--idle-up')},
        @{Name='state-aware-brute'; Args=@('--level','1','--inputs','n20e1<d30+b10..20,j5..6,30..40%>','--report','alive@51','--idle-up')},
        @{Name='state-aware-brute-stochastic'; Args=@('--level','1','--inputs','n20e1<d30+b10..20,j5..6,30..40%>','--report','alive@51','--idle-up','--stochastic','--seed','123','--update','0')},
        @{Name='state-aware-early-empty'; Args=@('--level','1','--inputs','n20e1<d6+b6,j2,50..50%>','--report','alive@1')},
        @{Name='resume-search'; Args=@('--inputs','(d1|n1|a1)(d1|n1|a1)(d1|n1|a1)n2','--report','alive@5','--stochastic','--seed','553776661331543456','--start-percent','37.25','--samples','7','--idle-up','--update','0')},
        @{Name='resume-list'; Args=@('--inputs','(d1|n1|a1)(d1|n1|a1)n2','--list','--stochastic','--seed','0','--start-percent','50%','--top','0')},
        @{Name='stochastic'; Args=@('--inputs','(d1..4|a1..4)n0..3e1','--report','alive@5','--pad','6','--stochastic','--seed','0','--samples','17','--update','0','--sortby','-vx')},
        @{Name='merge-and-physics'; Args=@('--inputs','(d2|d2|a2)n1..2','--report','alive@5','--merge-report','--tolerance','0','--raw-states','--merge-every','1','--pad','3','--gless','--mathspikes','0','--window','1100x800','--noscale','--trig','intel')},
        @{Name='list'; Args=@('--inputs',("{"+$prefix+"}[-1]e1<d6+j1..3,20..70%>"),'--list','--top','1')}
    )
    foreach ($case in $cases) {
        $expected = Join-Path $runDir ($case.Name + '-cli.txt')
        $actual = Join-Path $runDir ($case.Name + '-gui.txt')
        $args = @('smartforce','--level','17','--threads','2','--maxmem','0.1') + $case.Args
        # These assertions compare the selected route text. Concurrent equivalent
        # matches may legitimately retain different first-arriving representatives.
        if ($case.Args -contains '--hardfilter' -or $case.Args -contains '--merge-report') { $args += @('--threads','1') }
        $cliOutput = @(& $Cli @args --save $expected 2>&1 | ForEach-Object { $_.ToString() })
        $cliCode = $LASTEXITCODE
        $r = Run-Worker ($args + @('--save',$actual))
        Assert ($r.Code -eq $cliCode -and $r.Code -le 1) ($case.Name + ': same completion status')
        Assert ((Saved-Rows $expected) -ceq (Saved-Rows $actual)) ($case.Name + ': identical saved rows')
        Assert ($r.Text -match 'saved .* to ') ($case.Name + ': stderr reaches full log')
    }
    $names = Run-Worker @('smartforce','--level','17','--names')
    Assert ($names.Code -eq 0 -and $names.Text -match 'jumpPlatform1' -and $names.Text -match 'star') 'body names without a pattern'
    $bad = Run-Worker @('smartforce','--level','17','--inputs','<d5+jBAD>')
    Assert ($bad.Code -eq 2 -and $bad.Text.Length -gt 20) 'parser error is logged and returned'
    $cancel = Join-Path $runDir 'cancel-results.txt'
    $r = Run-Worker @('smartforce','--level','17','--inputs','d1..100n1..100a1..100e1..100','--list','--top','1','--maxmem','0.1','--save',$cancel) -StopAfter 200
    Assert ($r.Code -eq 0 -and $r.Text -match 'stopped by Ctrl\+C' -and $r.Text -match 'saved') 'Stop completes saved list output'
    Assert ((Get-Item -LiteralPath $cancel).Length -gt 100) 'saved list is not capped by --top 1'
    $cancelSearch = Join-Path $runDir 'cancel-search.txt'
    $r = Run-Worker @('smartforce','--level','17','--inputs','d1..100n1..100a1..100','--report','px > 0','--stochastic','--seed','9','--threads','2','--maxmem','0.1','--save',$cancelSearch) -StopAfter 400
    Assert ($r.Code -le 1 -and $r.Text -match 'Ctrl\+C' -and $r.Text -match 'saved') 'Stop finishes search and saves results'
    $r = Run-Worker @('smartforce','--level','17','--inputs','d1..100n1..100a1..100e1..100','--list','--top','0') -ParentGone $true
    Assert ($r.Code -eq 0 -and $r.Text -match 'stopped by Ctrl\+C') 'orphaned worker stops gracefully'
    $native = Run-Worker @('test')
    Assert ($native.Code -eq 0 -and $native.Text -match 'ALL PASSED') 'native physics tests pass inside rbview worker'
    $route = 'd4n1a1e1d10e2d5n2d1a2n1d1n3d10w1d2e1d35e3d21e19d19e1d14e1d54e1w9n1d9e3d2e1d1e33d1e1d2e8d9e1n1'
    $replayArgs = @('run','--level','17','--inputs',$route,'--hex')
    $expectedReplay = @(& $Cli @replayArgs 2>&1 | ForEach-Object { $_.ToString() }) -join "`n"
    $replay = Run-Worker $replayArgs
    Assert ($replay.Code -eq 0 -and ($replay.Text -replace "`r`n","`n").TrimEnd() -ceq $expectedReplay.TrimEnd()) 'Level 17 star replay agrees in hexadecimal on every logged frame'
    Write-Output "Evidence: $runDir"
} finally { Pop-Location }
