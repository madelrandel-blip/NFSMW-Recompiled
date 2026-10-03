# =============================================================================
#  Boot matrix: launches the game many times, under different conditions,
#  and reports in which ones it died.
#
#  WHAT IT IS FOR
#  The failure we are chasing is a RACE: the same binary starts on one machine
#  and dies on another, and sometimes on the same machine it depends on the
#  day. A single launch proves nothing -neither that it works nor that it
#  does not-. What is needed is a table: "this combination died 4 out of 5
#  times".
#
#  TWO THINGS THIS SCRIPT DOES THAT A DOUBLE CLICK DOES NOT
#
#  1. It empties the shader cache on every attempt.
#     It is what changes timing the most. With the cache populated, startup
#     pauses for two and a half seconds right where the problematic thread
#     starts, and that pause COVERS UP the race. With an empty cache it is
#     3 milliseconds. That is why a slow machine with a cache "works" and a
#     fast one without a cache does not: it is not the hardware, it is the
#     truce.
#
#     It is achieved with --user_data_root to a new folder each time.
#
#  2. It repeats. A race does not fail every time; it fails often. Without
#     repetitions, an "it worked" is noise.
#
#  WHY THE LOG LEVEL IS "info" AND NOT "debug"
#  Because writing the log costs time, and that time can cover up exactly the
#  race we are looking for. With -Detallado it can be raised, but then a "it
#  does not fail" is worth less: it may be that the log itself is hiding it.
#
#      powershell -ExecutionPolicy Bypass -File tools\matriz.ps1
#      powershell -ExecutionPolicy Bypass -File tools\matriz.ps1 -Repeticiones 10
#      powershell -ExecutionPolicy Bypass -File tools\matriz.ps1 -Segundos 45 -Detallado
#
#  It works the same from the project's tools\ as copied inside build\, so it
#  can be passed on to whoever has the portable folder.
# =============================================================================

param(
    [string]$Exe = '',
    [int]$Repeticiones = 5,
    [int]$Segundos = 25,
    [switch]$Detallado
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---- Locate the executable --------------------------------------------------
if (-not $Exe) {
    # nfsmw.exe FIRST: since the launcher takes the name NFS_Most_Wanted.exe,
    # the game in build\ is called that. The old names are still looked at
    # behind it, for folders from before the change.
    $candidatos = @(
        (Join-Path $PSScriptRoot 'nfsmw.exe')                                 # dentro de build\
        (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\nfsmw.exe')
        (Join-Path (Split-Path -Parent $PSScriptRoot) 'app\out\build\win-amd64-release\nfsmw.exe')
        (Join-Path $PSScriptRoot 'NFS_Most_Wanted.exe')
        (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\NFS_Most_Wanted.exe')
    )
    foreach ($c in $candidatos) {
        if (Test-Path -LiteralPath $c) { $Exe = $c; break }
    }
}
if (-not $Exe -or -not (Test-Path -LiteralPath $Exe)) {
    Write-Host 'Cannot find the executable.' -ForegroundColor Red
    Write-Host 'Pass it manually:  -Exe "C:\path\nfsmw.exe"'
    exit 1
}
$Exe    = (Resolve-Path -LiteralPath $Exe).Path
$CarpEx = Split-Path -Parent $Exe

# ---- Check that there is an ISO ---------------------------------------------
$isos = @(Get-ChildItem -LiteralPath $CarpEx -File -Filter '*.iso' -ErrorAction SilentlyContinue)
if ($isos.Count -eq 0) {
    Write-Host "There is no .iso next to the executable:" -ForegroundColor Red
    Write-Host "  $CarpEx"
    exit 1
}

# ---- The combinations -------------------------------------------------------
#
# Only THREAD SCHEDULING things are tested, which is where the suspicion
# lives. Both cvars come as true from the factory: the SDK ignores both the
# priorities and the affinities the game asks for. Setting them to false gives
# the game back the order it itself asked for, and that holds for any machine,
# which is what this is about.
$combos = @(
    @{ Nombre = 'base (as-is now)';              Args = @() }
    @{ Nombre = 'respect priorities';            Args = @('--ignore_thread_priorities=false') }
    @{ Nombre = 'respect affinities';            Args = @('--ignore_thread_affinities=false') }
    @{ Nombre = 'respect both';                  Args = @('--ignore_thread_priorities=false',
                                                          '--ignore_thread_affinities=false') }
)

$nivel   = if ($Detallado) { 'debug' } else { 'info' }
$raizTmp = Join-Path $env:TEMP ('nfsmw_matriz_' + [Guid]::NewGuid().ToString('N').Substring(0,8))
$salida  = Join-Path $CarpEx 'matriz'
New-Item -ItemType Directory -Path $salida -Force | Out-Null

Write-Host ''
Write-Host '============================================'
Write-Host '  Boot matrix'
Write-Host '============================================'
Write-Host ''
Write-Host "  Executable : $Exe"
Write-Host "  ISO        : $($isos[0].Name)"
Write-Host "  Attempts   : $Repeticiones per combination"
Write-Host "  Wait       : $Segundos s before considering a boot good"
Write-Host "  Log level  : $nivel"
if ($Detallado) {
    Write-Host '  WARNING: with debug the log costs time and can COVER UP the race.' -ForegroundColor DarkYellow
    Write-Host '         A "does not fail" with debug is worth less than one with info.' -ForegroundColor DarkYellow
}
Write-Host ''
Write-Host "  Worst case: about $([Math]::Round($combos.Count * $Repeticiones * $Segundos / 60.0, 1)) min."
Write-Host '  Failing boots die within a second, so it will be less.'
Write-Host ''

# ---- Classify a log ---------------------------------------------------------
#
# Returns a short label. It is worth telling apart WHAT failed, not just that
# it failed: if a combination changes the error, that is already information.
function Clasificar([string]$log) {
    if (-not (Test-Path -LiteralPath $log)) { return 'sin log' }
    $t = Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue
    if (-not $t) { return 'log vacio' }

    if ($t -match 'No function registered at ([0-9A-Fa-f]+)')      { return "sin funcion $($Matches[1])" }
    if ($t -match 'access violation: read of guest (0x[0-9A-Fa-f]+)')  { return "lectura nula $($Matches[1])" }
    if ($t -match 'access violation: write of guest (0x[0-9A-Fa-f]+)') { return "escritura nula $($Matches[1])" }
    if ($t -match 'Unhandled guest access violation')               { return 'access violation' }
    if ($t -match 'game_data_root')                                 { return 'falta la ISO' }
    if ($t -match '\[critical\]')                                   { return 'critical' }
    if ($t -match 'Execution complete')                             { return 'salio solo' }
    return 'murio sin decir nada'
}

# ---- One attempt ------------------------------------------------------------
function UnIntento($combo, [int]$n) {
    # NEW data folder: this is what empties the shader cache and removes the
    # 2.5 s pause that covers up the race.
    $datos = Join-Path $raizTmp ("run_{0}" -f [Guid]::NewGuid().ToString('N').Substring(0,6))
    $log   = Join-Path $salida  ("{0}_{1}.log" -f ($combo.Nombre -replace '[^\w]','_'), $n)
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }

    # WATCH OUT: the variable is NOT called $args. In PowerShell $args is
    # automatic -inside a function it contains the unbound arguments- and
    # overwriting it is one of those mistakes that do not show until they
    # cause a strange problem.
    $argumentos = @(
        '--log_level', $nivel
        '--log_file', ('"{0}"' -f $log)
        '--user_data_root', ('"{0}"' -f $datos)
        '--fullscreen=false'          # not fullscreen: it can be killed without drama
        '--readback_resolve=fast'     # the usual one, otherwise the image comes out washed out
    ) + $combo.Args

    $p = $null
    try {
        $p = Start-Process -FilePath $Exe -ArgumentList ($argumentos -join ' ') `
                           -WorkingDirectory $CarpEx -PassThru
    } catch {
        return @{ Estado = 'no arranco'; Detalle = $_.Exception.Message }
    }

    $vivo = -not $p.WaitForExit($Segundos * 1000)

    if ($vivo) {
        # It survived the wait. For what we are after, that is a good boot.
        #
        # THE [void] IS NOT REDUNDANT. WaitForExit(int) returns a bool, and in
        # PowerShell every value that is not captured goes to the function's
        # OUTPUT stream and mixes with the return. Without this, UnIntento
        # returned not the results table but @($true, @{Estado=...}), and the
        # caller received an array where it expected an object.
        try { $p.Kill(); [void]$p.WaitForExit(5000) } catch { }
        # The log is still checked: it may have survived while spewing errors.
        $c = Clasificar $log
        if ($c -in @('murio sin decir nada','salio solo')) {
            return @{ Estado = 'OK'; Detalle = '' }
        }
        return @{ Estado = 'OK'; Detalle = "but the log says: $c" }
    }

    return @{ Estado = 'FALLO'; Detalle = (Clasificar $log) }
}

# ---- Walk the matrix --------------------------------------------------------
$tabla = @()
foreach ($combo in $combos) {
    Write-Host ("-- {0}" -f $combo.Nombre)
    $ok = 0; $mal = 0; $motivos = @{}

    for ($i = 1; $i -le $Repeticiones; $i++) {
        $r = UnIntento $combo $i

        # SAFETY NET, not a patch. If some call writes to the output stream
        # again without capturing, an array would arrive here instead of an
        # object. Instead of dying with "the Estado property cannot be found",
        # the last one -which is the real return- is taken and A WARNING IS
        # ISSUED, so that the bug gets fixed instead of staying hidden.
        if ($r -is [System.Array]) {
            Write-Host ''
            Write-Host ("   [internal warning] UnIntento returned {0} values; something writes to the output stream." -f $r.Count) -ForegroundColor DarkYellow
            $r = $r[-1]
        }

        if ($r.Estado -eq 'OK') {
            $ok++
            Write-Host '   .' -NoNewline -ForegroundColor DarkGreen
        } else {
            $mal++
            Write-Host '   X' -NoNewline -ForegroundColor Red
            $d = [string]$r.Detalle
            if ($d) { $motivos[$d] = 1 + $(if ($motivos.ContainsKey($d)) { $motivos[$d] } else { 0 }) }
        }
    }
    Write-Host ''
    $det = ($motivos.Keys | Sort-Object) -join '; '
    Write-Host ("   {0}/{1} booted{2}" -f $ok, $Repeticiones,
                $(if ($det) { "   ->  $det" } else { '' }))
    Write-Host ''

    $tabla += [pscustomobject]@{
        Combinacion = $combo.Nombre
        Arrancaron  = "$ok/$Repeticiones"
        Fallos      = $mal
        Motivo      = $det
    }
}

# ---- Cleanup and summary ----------------------------------------------------
if (Test-Path -LiteralPath $raizTmp) {
    Remove-Item -LiteralPath $raizTmp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host '============================================'
Write-Host '  SUMMARY'
Write-Host '============================================'
$tabla | Format-Table -AutoSize | Out-String | Write-Host

$buenas = @($tabla | Where-Object { $_.Fallos -eq 0 })
$malas  = @($tabla | Where-Object { $_.Fallos -eq $Repeticiones })

if ($buenas.Count -eq $tabla.Count) {
    Write-Host '  All of them booted every time.' -ForegroundColor Green
    Write-Host ''
    Write-Host '  Mind what this means and what it does not. On THIS machine, with'
    Write-Host '  the cache empty, it does not reproduce. It does not prove it is'
    Write-Host '  fixed: a race may need more cores or more speed. Have whoever'
    Write-Host '  DOES see it fail run it too.'
} elseif ($buenas.Count -gt 0) {
    Write-Host '  THERE ARE COMBINATIONS THAT NEVER FAIL:' -ForegroundColor Green
    foreach ($b in $buenas) { Write-Host ("    - {0}" -f $b.Combinacion) }
    Write-Host ''
    Write-Host '  That is a real clue, not a per-machine patch: if respecting the'
    Write-Host '  priorities fixes the boot, it means the game COUNTED ON that order'
    Write-Host '  and the SDK was throwing it away.'
} else {
    Write-Host '  All of them failed.' -ForegroundColor Red
    Write-Host '  Thread scheduling is not the cause, or not the only one.'
    Write-Host '  The logs for each attempt are in:  matriz\'
}

if ($malas.Count -eq $tabla.Count -and $Repeticiones -gt 1) {
    Write-Host ''
    Write-Host '  It fails 100% of the time, so it is probably NOT a race but a'
    Write-Host '  deterministic failure. That is better: it is much easier to'
    Write-Host '  debug.' -ForegroundColor DarkYellow
}

$csv = Join-Path $salida 'resumen.csv'
$tabla | Export-Csv -LiteralPath $csv -NoTypeInformation -Encoding UTF8
Write-Host ''
Write-Host "  Table:  $csv"
Write-Host "  Logs :  $salida"
Write-Host ''
