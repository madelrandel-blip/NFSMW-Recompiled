# =============================================================================
#  NFSMW Recomp - Launcher
#
#  PowerShell + Windows Forms: both ship with Windows, there is nothing to
#  install. It opens from LANZADOR.bat.
#
#  Fixed, and why:
#    --readback_resolve=fast   without this the image comes out washed out and
#                              the sun blown out. It is a fix, not a
#                              preference.
#    --gpu_plugin xenos        it is the only backend built.
#    --mnk_mode                keyboard and mouse in addition to the gamepad.
#    --gpu_backend=...         always, even if it matches the toml. See the
#                              "Graphics API" group: it is what keeps you from
#                              being unable to play after choosing an API that
#                              does not work.
#
#
#  THE TWO RESOLUTIONS, WHICH ARE NOT THE SAME
#  ===========================================
#  This took a while to understand and it is worth writing down.
#
#  --resolution  (OUTPUT)
#    Changes the guest's video mode -what VdQueryVideoMode answers the game
#    when it asks what resolution the screen has- and, incidentally, the
#    window size: Window::Create receives 1280x720 but only as a request, and
#    ResolveWindowWidth/Height override it with this preset.
#
#    WHAT IT DOES NOT DO: force the game to render more finely. Most Wanted,
#    like almost every 360 game, draws into its own fixed-size render targets
#    and lets the console's scaler stretch the result up to the video mode.
#    So raising this enlarges the image, it does not improve it.
#
#  --resolution_scale  (RENDERING)
#    This one does. It multiplies the size of the render targets and the
#    emulated EDRAM, so the game genuinely draws double or triple the pixels.
#    It is the resolution scale inherited from Xenia. Its description in the
#    SDK itself: "Draw resolution scale for both X and Y axes".
#
#    It is expensive on the GPU and grows with the square: 2x is four times
#    the pixels. If the card cannot handle the requested scale, the SDK lowers
#    it on its own and writes it to the log ("reducing to NxN").
#
#
#  VSYNC AND FPS LIMIT: THEY NEED THE PATCH
#  ========================================
#  Out of the box neither of them works:
#
#    - "vsync" exists as a cvar but does not sync anything. It is read in a
#      single place in the SDK, and it only decides whether the command
#      processor sleeps or spins during guest waits. The D3D12 presenter's
#      Present had SyncInterval pinned to 0.
#
#    - There was no fps limiter at all. None.
#
#  tools\parche_presentador.py fixes both things. If it is not applied, this
#  window warns about it at the top in red.
# =============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

# ---- Where we are -----------------------------------------------------------
#
# This script lives in TWO PLACES and must work in both:
#
#   project    NFSMW Recomp\tools\lanzador.ps1
#              The executable is in app\out\build\win-amd64-release\, the
#              logs and configuration hang off the project root, and the SDK
#              source is next to it, so it can be checked whether the
#              presenter patch is applied.
#
#   folder     build\lanzador.ps1  (next to nfsmw.exe)
#              Here there is no project or SDK: only the game. Everything
#              -exe, ISO, logs, settings- lives in this same folder.
#
# It is told apart by the most reliable thing there is: if the executable is
# NEXT TO the script, we are in the distribution folder.
#
# THE GAME IS CALLED nfsmw.exe. Since Lanzador.exe exists, in build\ the name
# NFS_Most_Wanted.exe belongs to THE LAUNCHER, so that double-clicking the
# game icon brings up the options window. Looking for the pretty name here
# would make this script launch itself, in a loop.
#
# The old name is still accepted further down, for folders put together
# before the change, where NFS_Most_Wanted.exe is still the game.
$JUEGO = $null
foreach ($n in @('nfsmw.exe', 'NFS_Most_Wanted.exe')) {
    $c = Join-Path $PSScriptRoot $n
    if (Test-Path -LiteralPath $c) { $JUEGO = $c; break }
}
$DISTRIBUIDA = [bool]$JUEGO

if ($DISTRIBUIDA) {
    $RAIZ    = $PSScriptRoot
    $EXE     = $JUEGO
    $LOGDIR  = Join-Path $PSScriptRoot 'logs'
    $AJUSTES = Join-Path $PSScriptRoot 'lanzador.json'
    # There is no SDK to check. And it is not needed: the distribution folder
    # is put together with the dist target, which only exists in a tree where
    # the patch is already applied.
    $FUENTE_PRESENTADOR = $null
} else {
    $RAIZ    = Split-Path -Parent $PSScriptRoot
    $EXE     = Join-Path $RAIZ 'app\out\build\win-amd64-release\nfsmw.exe'
    $LOGDIR  = Join-Path $RAIZ 'logs'
    $AJUSTES = Join-Path $RAIZ 'config\lanzador.json'
    $FUENTE_PRESENTADOR = Join-Path (Split-Path -Parent $RAIZ) 'rexglue-sdk\src\ui\d3d12\d3d12_presenter.cpp'
}
$RUNLOG  = Join-Path $LOGDIR 'lanzador.log'

# Presets the SDK knows how to interpret, taken from TryParseResolutionPreset
# in include/rex/graphics/video_mode_util.h. It also accepts "WIDTHxHEIGHT".
$PRESETS = [ordered]@{
    '480p  - 640 x 480'    = '480p'
    '540p  - 960 x 540'    = '540p'
    '720p  - 1280 x 720'   = '720p'
    '900p  - 1600 x 900'   = '900p'
    '1080p - 1920 x 1080'  = '1080p'
    '1440p - 2560 x 1440'  = '1440p'
    '1800p - 3200 x 1800'  = '1800p'
    '2160p - 3840 x 2160'  = '2160p'
    'Custom'               = 'custom'
}

$ESCALAS = [ordered]@{
    '1x  - native game resolution' = 1
    '2x  - 4 times the pixels'     = 2
    '3x  - 9 times the pixels'     = 3
}

$defectos = @{
    iso      = ''
    preset   = '720p  - 1280 x 720'
    ancho    = 1280
    alto     = 720
    escala   = '1x  - native game resolution'
    pantalla = $true     # the SDK starts in fullscreen by default
    vsync    = $false
    limitar  = $false
    fps      = 60
    # 'auto' = pass nothing and let nfsmw.toml rule, which ships "rtv".
    # Without the toml, 'auto' means the SDK decides: ROV on Intel, RTV on the
    # rest. Which is exactly the per-brand decision we want to be able to skip.
    video    = 'auto'
    # The graphics API. THERE IS DELIBERATELY NO 'auto' HERE, and that is what
    # makes this window an emergency exit: see the group comment.
    api      = 'd3d12'
}

function Cargar-Ajustes {
    $a = $defectos.Clone()
    if (Test-Path -LiteralPath $AJUSTES) {
        try {
            $j = Get-Content -LiteralPath $AJUSTES -Raw | ConvertFrom-Json
            foreach ($k in @($a.Keys)) {
                if ($j.PSObject.Properties.Name -contains $k) { $a[$k] = $j.$k }
            }
        } catch {
            # A corrupt json must not prevent the launcher from opening.
        }
    }
    return $a
}

function Guardar-Ajustes($a) {
    try {
        $dir = Split-Path -Parent $AJUSTES
        if (-not (Test-Path -LiteralPath $dir)) {
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
        }
        $a | ConvertTo-Json | Set-Content -LiteralPath $AJUSTES -Encoding UTF8
    } catch {
        # Saving preferences is a luxury, not a condition for playing.
    }
}

# It checks the SDK SOURCE, not the DLL: that is where the truth lives and it
# is cheap to check. If it is patched but not rebuilt, the warning below says
# so.
function Parche-Aplicado {
    # In the distribution folder there is no source to check, but it does not
    # doubt either: that folder is built from an already patched tree.
    # Returning $null there would only serve to show whoever receives it a
    # warning about an SDK they do not have in front of them.
    if ($DISTRIBUIDA) { return $true }
    if (-not $FUENTE_PRESENTADOR) { return $null }
    if (-not (Test-Path -LiteralPath $FUENTE_PRESENTADOR)) { return $null }
    try {
        return [bool](Select-String -LiteralPath $FUENTE_PRESENTADOR -SimpleMatch `
                        -Pattern 'PARCHE LOCAL - vsync real y limitador de fps' -Quiet)
    } catch {
        return $null
    }
}

$cfg = Cargar-Ajustes

# =============================================================================
#  Window
# =============================================================================
$form                 = New-Object System.Windows.Forms.Form
$form.Text            = 'NFS Most Wanted - Recompilation'
$form.Size            = New-Object System.Drawing.Size(560, 888)
$form.StartPosition   = 'CenterScreen'
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox     = $false

function Nuevo-Grupo($texto, $y, $alto) {
    $g          = New-Object System.Windows.Forms.GroupBox
    $g.Text     = $texto
    $g.Location = New-Object System.Drawing.Point(12, $y)
    $g.Size     = New-Object System.Drawing.Size(520, $alto)
    $form.Controls.Add($g)
    return $g
}

function Nueva-Nota($padre, $x, $y, $ancho, $alto, $texto) {
    $l           = New-Object System.Windows.Forms.Label
    $l.Location  = New-Object System.Drawing.Point($x, $y)
    $l.Size      = New-Object System.Drawing.Size($ancho, $alto)
    $l.ForeColor = [System.Drawing.Color]::DimGray
    $l.Text      = $texto
    $padre.Controls.Add($l)
    return $l
}

# ---- ISO --------------------------------------------------------------------
$gIso = Nuevo-Grupo 'Game image' 8 78

$txtIso          = New-Object System.Windows.Forms.TextBox
$txtIso.Location = New-Object System.Drawing.Point(12, 24)
$txtIso.Size     = New-Object System.Drawing.Size(390, 22)
$txtIso.Text     = [string]$cfg.iso
$gIso.Controls.Add($txtIso)

$btnIso          = New-Object System.Windows.Forms.Button
$btnIso.Text     = 'Examinar...'
$btnIso.Location = New-Object System.Drawing.Point(410, 23)
$btnIso.Size     = New-Object System.Drawing.Size(96, 24)
$gIso.Controls.Add($btnIso)

[void](Nueva-Nota $gIso 12 52 494 18 `
    'Read on the fly: nothing is copied to disk, so it must still be there.')

# ---- Display ----------------------------------------------------------------
$gPant = Nuevo-Grupo 'Display and resolution' 92 210

$rbVentana          = New-Object System.Windows.Forms.RadioButton
$rbVentana.Text     = 'Windowed'
$rbVentana.Location = New-Object System.Drawing.Point(14, 22)
$rbVentana.Size     = New-Object System.Drawing.Size(120, 22)
$gPant.Controls.Add($rbVentana)

$rbCompleta          = New-Object System.Windows.Forms.RadioButton
$rbCompleta.Text     = 'Fullscreen'
$rbCompleta.Location = New-Object System.Drawing.Point(150, 22)
$rbCompleta.Size     = New-Object System.Drawing.Size(160, 22)
$gPant.Controls.Add($rbCompleta)

if ([bool]$cfg.pantalla) { $rbCompleta.Checked = $true } else { $rbVentana.Checked = $true }

# Output
$lblSalida          = New-Object System.Windows.Forms.Label
$lblSalida.Text     = 'Output'
$lblSalida.Location = New-Object System.Drawing.Point(14, 54)
$lblSalida.Size     = New-Object System.Drawing.Size(80, 20)
$gPant.Controls.Add($lblSalida)

$cboRes               = New-Object System.Windows.Forms.ComboBox
$cboRes.Location      = New-Object System.Drawing.Point(100, 51)
$cboRes.Size          = New-Object System.Drawing.Size(220, 22)
$cboRes.DropDownStyle = 'DropDownList'
foreach ($k in $PRESETS.Keys) { [void]$cboRes.Items.Add($k) }
$gPant.Controls.Add($cboRes)

$numAncho          = New-Object System.Windows.Forms.NumericUpDown
$numAncho.Location = New-Object System.Drawing.Point(330, 51)
$numAncho.Size     = New-Object System.Drawing.Size(70, 22)
$numAncho.Minimum  = 640        # limits applied by the SDK itself
$numAncho.Maximum  = 4095
$numAncho.Value    = [int]$cfg.ancho
$gPant.Controls.Add($numAncho)

$lblPor          = New-Object System.Windows.Forms.Label
$lblPor.Text     = 'x'
$lblPor.Location = New-Object System.Drawing.Point(406, 54)
$lblPor.Size     = New-Object System.Drawing.Size(12, 20)
$gPant.Controls.Add($lblPor)

$numAlto          = New-Object System.Windows.Forms.NumericUpDown
$numAlto.Location = New-Object System.Drawing.Point(422, 51)
$numAlto.Size     = New-Object System.Drawing.Size(70, 22)
$numAlto.Minimum  = 480
$numAlto.Maximum  = 4095
$numAlto.Value    = [int]$cfg.alto
$gPant.Controls.Add($numAlto)

[void](Nueva-Nota $gPant 100 76 400 32 `
    ("Window and final image size. It does NOT make the game draw " +
     "more finely: it only stretches what it already draws."))

# Rendering
$lblEsc          = New-Object System.Windows.Forms.Label
$lblEsc.Text     = 'Rendering'
$lblEsc.Location = New-Object System.Drawing.Point(14, 116)
$lblEsc.Size     = New-Object System.Drawing.Size(80, 20)
$gPant.Controls.Add($lblEsc)

$cboEsc               = New-Object System.Windows.Forms.ComboBox
$cboEsc.Location      = New-Object System.Drawing.Point(100, 113)
$cboEsc.Size          = New-Object System.Drawing.Size(220, 22)
$cboEsc.DropDownStyle = 'DropDownList'
foreach ($k in $ESCALAS.Keys) { [void]$cboEsc.Items.Add($k) }
$gPant.Controls.Add($cboEsc)

[void](Nueva-Nota $gPant 100 138 400 60 `
    ("THIS is the real internal resolution: it multiplies the render targets " +
     "and the emulated EDRAM. It is expensive and grows with the square (2x is " +
     "four times the pixels). If the GPU cannot handle it, the SDK lowers it " +
     "on its own and notes it in the log."))

# ---- Frames -----------------------------------------------------------------
$gFps = Nuevo-Grupo 'Frames' 310 130

$chkVsync          = New-Object System.Windows.Forms.CheckBox
$chkVsync.Text     = 'Vsync (sync to the screen)'
$chkVsync.Location = New-Object System.Drawing.Point(14, 22)
$chkVsync.Size     = New-Object System.Drawing.Size(300, 22)
$chkVsync.Checked  = [bool]$cfg.vsync
$gFps.Controls.Add($chkVsync)

[void](Nueva-Nota $gFps 32 44 474 18 `
    'Uncheck it to measure real fps: with vsync everything reports what the monitor does.')

$chkLimite          = New-Object System.Windows.Forms.CheckBox
$chkLimite.Text     = 'Limit to'
$chkLimite.Location = New-Object System.Drawing.Point(14, 68)
$chkLimite.Size     = New-Object System.Drawing.Size(80, 22)
$chkLimite.Checked  = [bool]$cfg.limitar
$gFps.Controls.Add($chkLimite)

$numFps          = New-Object System.Windows.Forms.NumericUpDown
$numFps.Location = New-Object System.Drawing.Point(100, 67)
$numFps.Size     = New-Object System.Drawing.Size(60, 22)
$numFps.Minimum  = 10
$numFps.Maximum  = 1000
$numFps.Value    = [int]$cfg.fps
$gFps.Controls.Add($numFps)

$lblFps          = New-Object System.Windows.Forms.Label
$lblFps.Text     = 'fps'
$lblFps.Location = New-Object System.Drawing.Point(166, 70)
$lblFps.Size     = New-Object System.Drawing.Size(30, 20)
$gFps.Controls.Add($lblFps)

[void](Nueva-Nota $gFps 32 92 474 30 `
    ("A real limiter, inside the presenter: it sleeps until the next frame is " +
     "due. Useful so the GPU does not run flat out for no reason."))

# ---- Video engine -----------------------------------------------------------
#
# The Xbox 360 does not have normal render targets: it has 10 MB of embedded
# memory -the EDRAM- where fixed-function hardware does blending and the depth
# test. Emulating that can be done in two ways, and they are not equivalent in
# speed or in accuracy. See nfsmw.toml, which tells the whole story.
$gVideo = Nuevo-Grupo 'Video engine (EDRAM emulation)' 444 88

$rbVidAuto          = New-Object System.Windows.Forms.RadioButton
$rbVidAuto.Text     = 'Automatic'
$rbVidAuto.Location = New-Object System.Drawing.Point(14, 22)
$rbVidAuto.Size     = New-Object System.Drawing.Size(110, 22)
$gVideo.Controls.Add($rbVidAuto)

$rbVidRtv          = New-Object System.Windows.Forms.RadioButton
$rbVidRtv.Text     = 'Fast (rtv)'
$rbVidRtv.Location = New-Object System.Drawing.Point(134, 22)
$rbVidRtv.Size     = New-Object System.Drawing.Size(120, 22)
$gVideo.Controls.Add($rbVidRtv)

$rbVidRov          = New-Object System.Windows.Forms.RadioButton
$rbVidRov.Text     = 'Accurate (rov)'
$rbVidRov.Location = New-Object System.Drawing.Point(264, 22)
$rbVidRov.Size     = New-Object System.Drawing.Size(120, 22)
$gVideo.Controls.Add($rbVidRov)

switch ([string]$cfg.video) {
    'rtv'   { $rbVidRtv.Checked  = $true }
    'rov'   { $rbVidRov.Checked  = $true }
    default { $rbVidAuto.Checked = $true }
}

[void](Nueva-Nota $gVideo 14 46 494 34 `
    ("Automatic uses whatever nfsmw.toml says. Fast can double the fps on " +
     "integrated graphics, but on some it leaves a weird horizontal stripe. " +
     "Accurate always looks right and runs quite a bit slower."))

# ---- Graphics API -----------------------------------------------------------
#
# THIS GROUP IS AN EMERGENCY EXIT, AND THAT IS WHY IT HAS NO 'AUTOMATIC'
# ======================================================================
# gpu_backend can also be changed from the F4 menu, inside the game.
# The problem is that if you choose an API that gives a black screen on your
# machine, save and restart, the value stays written in nfsmw.toml and there
# is no way back: to change it you need the menu, and to reach the menu you
# need to see something. That happened, and that is why this window exists.
#
# The rule that fixes it is from the SDK itself: in the cvar priority order
# the command line overrides the configuration file
# -kDefault < kConfig < kEnvironment < kCommandLine < kRuntime-. So if the
# launcher ALWAYS passes --gpu_backend, whatever the toml says does not
# matter: the window always wins.
#
# That is why there is no 'automatic' option here. An automatic that passed
# nothing would hand control back to the toml, which is exactly the hole
# through which one gets locked out. In 'Video engine', which cannot leave the
# game invisible, it does make sense.
$gApi = Nuevo-Grupo 'Graphics API' 540 86

$rbApiDx          = New-Object System.Windows.Forms.RadioButton
$rbApiDx.Text     = 'DirectX 12 (recommended)'
$rbApiDx.Location = New-Object System.Drawing.Point(14, 22)
$rbApiDx.Size     = New-Object System.Drawing.Size(200, 22)
$gApi.Controls.Add($rbApiDx)

$rbApiVk          = New-Object System.Windows.Forms.RadioButton
$rbApiVk.Text     = 'Vulkan (experimental)'
$rbApiVk.Location = New-Object System.Drawing.Point(234, 22)
$rbApiVk.Size     = New-Object System.Drawing.Size(200, 22)
$gApi.Controls.Add($rbApiVk)

if ([string]$cfg.api -eq 'vulkan') { $rbApiVk.Checked = $true } else { $rbApiDx.Checked = $true }

[void](Nueva-Nota $gApi 14 46 494 34 `
    ("DirectX 12 is the one that has been tested. Vulkan is compiled but on " +
     "Intel graphics it can come up black; if that happens, come back here and " +
     "select DirectX 12, because this window overrides nfsmw.toml."))

# ---- Patch warning ----------------------------------------------------------
$lblParche           = New-Object System.Windows.Forms.Label
$lblParche.Location  = New-Object System.Drawing.Point(14, 632)
$lblParche.Size      = New-Object System.Drawing.Size(516, 32)
$lblParche.ForeColor = [System.Drawing.Color]::Firebrick
$form.Controls.Add($lblParche)

# ---- Command line -----------------------------------------------------------
$gCmd = Nuevo-Grupo 'What is going to run' 668 86

$txtCmd            = New-Object System.Windows.Forms.TextBox
$txtCmd.Location   = New-Object System.Drawing.Point(12, 20)
$txtCmd.Size       = New-Object System.Drawing.Size(494, 56)
$txtCmd.Multiline  = $true
$txtCmd.ReadOnly   = $true
$txtCmd.ScrollBars = 'Vertical'
$txtCmd.BackColor  = [System.Drawing.Color]::WhiteSmoke
$txtCmd.Font       = New-Object System.Drawing.Font('Consolas', 8)
$gCmd.Controls.Add($txtCmd)

# ---- Buttons ----------------------------------------------------------------
$btnJugar          = New-Object System.Windows.Forms.Button
$btnJugar.Text     = 'PLAY'
$btnJugar.Location = New-Object System.Drawing.Point(300, 766)
$btnJugar.Size     = New-Object System.Drawing.Size(120, 34)
$btnJugar.Font     = New-Object System.Drawing.Font('Segoe UI', 10, [System.Drawing.FontStyle]::Bold)
$form.Controls.Add($btnJugar)

$btnSalir          = New-Object System.Windows.Forms.Button
$btnSalir.Text     = 'Exit'
$btnSalir.Location = New-Object System.Drawing.Point(430, 766)
$btnSalir.Size     = New-Object System.Drawing.Size(100, 34)
$form.Controls.Add($btnSalir)

$lblEstado           = New-Object System.Windows.Forms.Label
$lblEstado.Location  = New-Object System.Drawing.Point(14, 772)
$lblEstado.Size      = New-Object System.Drawing.Size(280, 40)
$lblEstado.ForeColor = [System.Drawing.Color]::DimGray
$form.Controls.Add($lblEstado)

# =============================================================================
#  Logic
# =============================================================================

function Salida-Elegida {
    $clave = [string]$cboRes.SelectedItem
    if (-not $clave) { return '720p' }
    $v = $PRESETS[$clave]
    if ($v -eq 'custom') { return ('{0}x{1}' -f [int]$numAncho.Value, [int]$numAlto.Value) }
    return $v
}

function Escala-Elegida {
    $clave = [string]$cboEsc.SelectedItem
    if (-not $clave) { return 1 }
    return [int]$ESCALAS[$clave]
}

function Construir-Argumentos {
    $a = New-Object System.Collections.Generic.List[string]
    $a.Add('--log_level info')
    $a.Add('--log_file "{0}"' -f $RUNLOG)
    $a.Add('--game_data_root "{0}"' -f $txtIso.Text)
    $a.Add('--gpu_plugin xenos')
    $a.Add('--mnk_mode')
    $a.Add('--readback_resolve=fast')

    # ALWAYS, even when it matches what the toml already says. It is what
    # turns this window into the emergency exit: by passing it here, a bad
    # gpu_backend saved from F4 cannot leave the game invisible.
    $a.Add('--gpu_backend={0}' -f $(if ($rbApiVk.Checked) { 'vulkan' } else { 'd3d12' }))

    $a.Add('--resolution {0}' -f (Salida-Elegida))

    $esc = Escala-Elegida
    if ($esc -gt 1) { $a.Add('--resolution_scale {0}' -f $esc) }

    if ($rbCompleta.Checked) { $a.Add('--fullscreen=true') } else { $a.Add('--fullscreen=false') }
    if ($chkVsync.Checked)   { $a.Add('--vsync=true') }       else { $a.Add('--vsync=false') }
    if ($chkLimite.Checked)  { $a.Add('--max_fps {0}' -f [int]$numFps.Value) }

    # Only passed if chosen by hand. In automatic nothing is set, and thus
    # whatever nfsmw.toml says keeps ruling: command-line arguments override
    # the configuration file, not the other way around.
    if ($rbVidRtv.Checked) { $a.Add('--render_target_path_d3d12=rtv') }
    if ($rbVidRov.Checked) { $a.Add('--render_target_path_d3d12=rov') }

    return ($a -join ' ')
}

function Refrescar {
    $esCustom = ([string]$cboRes.SelectedItem -and $PRESETS[[string]$cboRes.SelectedItem] -eq 'custom')
    $numAncho.Enabled = $esCustom
    $numAlto.Enabled  = $esCustom
    $numFps.Enabled   = $chkLimite.Checked
    $txtCmd.Text      = (Split-Path -Leaf $EXE) + ' ' + (Construir-Argumentos)
}

$rbVidAuto.Add_CheckedChanged({ Refrescar })
$rbVidRtv.Add_CheckedChanged({ Refrescar })
$rbVidRov.Add_CheckedChanged({ Refrescar })
$rbApiDx.Add_CheckedChanged({ Refrescar })
$rbApiVk.Add_CheckedChanged({ Refrescar })

$btnIso.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Filter = 'Disk image (*.iso)|*.iso|All files (*.*)|*.*'
    $dlg.Title  = 'Choose the Need for Speed: Most Wanted ISO'
    if ($txtIso.Text -and (Test-Path -LiteralPath $txtIso.Text)) {
        $dlg.InitialDirectory = Split-Path -Parent $txtIso.Text
    } else {
        $dlg.InitialDirectory = $RAIZ
    }
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        $txtIso.Text = $dlg.FileName
        Refrescar
    }
})

$cboRes.Add_SelectedIndexChanged({ Refrescar })
$cboEsc.Add_SelectedIndexChanged({ Refrescar })
$chkLimite.Add_CheckedChanged({ Refrescar })
$chkVsync.Add_CheckedChanged({ Refrescar })
$rbCompleta.Add_CheckedChanged({ Refrescar })
$numAncho.Add_ValueChanged({ Refrescar })
$numAlto.Add_ValueChanged({ Refrescar })
$numFps.Add_ValueChanged({ Refrescar })
$txtIso.Add_TextChanged({ Refrescar })

$btnSalir.Add_Click({ $form.Close() })

$btnJugar.Add_Click({
    if (-not (Test-Path -LiteralPath $EXE)) {
        [void][System.Windows.Forms.MessageBox]::Show(
            ("I cannot find the executable:`n`n{0}`n`nBuild it first." -f $EXE),
            'Missing executable', 'OK', 'Warning')
        return
    }
    if (-not $txtIso.Text -or -not (Test-Path -LiteralPath $txtIso.Text)) {
        [void][System.Windows.Forms.MessageBox]::Show(
            'Choose an ISO that exists.', 'Missing ISO', 'OK', 'Warning')
        return
    }

    # Save before launching: if the game crashes, the preferences stay.
    Guardar-Ajustes @{
        iso      = $txtIso.Text
        preset   = [string]$cboRes.SelectedItem
        ancho    = [int]$numAncho.Value
        alto     = [int]$numAlto.Value
        escala   = [string]$cboEsc.SelectedItem
        pantalla = [bool]$rbCompleta.Checked
        vsync    = [bool]$chkVsync.Checked
        limitar  = [bool]$chkLimite.Checked
        fps      = [int]$numFps.Value
        video    = $(if ($rbVidRtv.Checked) { 'rtv' } elseif ($rbVidRov.Checked) { 'rov' } else { 'auto' })
        api      = $(if ($rbApiVk.Checked) { 'vulkan' } else { 'd3d12' })
    }

    if (-not (Test-Path -LiteralPath $LOGDIR)) {
        New-Item -ItemType Directory -Path $LOGDIR -Force | Out-Null
    }

    $btnJugar.Enabled = $false
    $lblEstado.Text   = 'Playing... (F3 to see fps)'
    $form.Refresh()

    try {
        $p = Start-Process -FilePath $EXE -ArgumentList (Construir-Argumentos) `
                           -WorkingDirectory (Split-Path -Parent $EXE) -PassThru
        $p.WaitForExit()
        $codigo = $p.ExitCode
    } catch {
        [void][System.Windows.Forms.MessageBox]::Show(
            ("Could not launch:`n`n{0}" -f $_.Exception.Message), 'Error', 'OK', 'Error')
        $btnJugar.Enabled = $true
        $lblEstado.Text   = ''
        return
    }

    $btnJugar.Enabled = $true
    $lblEstado.Text   = ''

    # If a scale was requested and the GPU could not handle it, the SDK lowers it and writes it down.
    if (Test-Path -LiteralPath $RUNLOG) {
        $bajada = Select-String -LiteralPath $RUNLOG -SimpleMatch `
                    -Pattern 'draw resolution scale is not supported' |
                  Select-Object -First 1
        if ($bajada) {
            [void][System.Windows.Forms.MessageBox]::Show(
                ("The render scale you requested is not supported by your machine, " +
                 "so the SDK lowered it on its own:`n`n{0}" -f $bajada.Line),
                'Reduced scale', 'OK', 'Information')
        }
    }

    if ($codigo -ne 0) {
        $pistas = ''
        if (Test-Path -LiteralPath $RUNLOG) {
            $m = Select-String -LiteralPath $RUNLOG -SimpleMatch `
                    -Pattern '[critical]', 'FATAL', 'unregistered' |
                 Select-Object -Last 8 | ForEach-Object { $_.Line }
            if ($m) { $pistas = "`n`n" + ($m -join "`n") }
        }
        [void][System.Windows.Forms.MessageBox]::Show(
            ("The game exited with code {0}.{1}`n`nLog: {2}" -f $codigo, $pistas, $RUNLOG),
            'Exited with error', 'OK', 'Warning')
    }
})

# ---- Initial state ----------------------------------------------------------
$idx = $cboRes.Items.IndexOf([string]$cfg.preset)
if ($idx -lt 0) { $idx = $cboRes.Items.IndexOf('720p  - 1280 x 720') }
if ($idx -lt 0) { $idx = 0 }
$cboRes.SelectedIndex = $idx

$idxE = $cboEsc.Items.IndexOf([string]$cfg.escala)
if ($idxE -lt 0) { $idxE = 0 }
$cboEsc.SelectedIndex = $idxE

if (-not $txtIso.Text) {
    $encontrada = Get-ChildItem -LiteralPath $RAIZ -Filter '*.iso' -File -ErrorAction SilentlyContinue |
                  Select-Object -First 1
    if ($encontrada) { $txtIso.Text = $encontrada.FullName }
}

$parche = Parche-Aplicado
if ($parche -eq $false) {
    $lblParche.Text = ("WARNING: vsync and the fps limit will NOT do anything yet. Out of the box the SDK " +
                       "does not sync (Present with SyncInterval 0) and does not ship a limiter. " +
                       "Apply tools\parche_presentador.py and rebuild the SDK.")
} elseif ($parche -eq $true) {
    $lblParche.Text = ''
} else {
    $lblParche.ForeColor = [System.Drawing.Color]::DimGray
    $lblParche.Text = 'I cannot find the SDK source, so I do not know whether the vsync patch is applied.'
}

if (-not (Test-Path -LiteralPath $EXE)) {
    $lblEstado.Text      = 'Warning: there is no compiled executable yet.'
    $lblEstado.ForeColor = [System.Drawing.Color]::Firebrick
}

Refrescar
[void]$form.ShowDialog()
