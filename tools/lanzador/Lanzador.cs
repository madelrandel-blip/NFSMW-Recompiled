// =============================================================================
//  NFS Most Wanted - Recompilation : Launcher
//
//  Native Windows window, in C# with WinForms. Replaces lanzador.ps1 and
//  does exactly the same, with the game cover art on the side like an
//  installer.
//
//  It is built with CONSTRUIR_LANZADOR.bat, which uses the csc.exe from the
//  .NET Framework that ALREADY SHIPS with Windows. There is no need to install
//  Visual Studio, the .NET SDK, or anything: the compiler is at
//  C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe since Windows 8.
//
//  WHY C# AND NOT SOMETHING ELSE
//  =============================
//  A real .exe was needed, with its icon, and not depending on installing
//  anything. The options were:
//
//    - Raw Win32 C++: it produces a small exe, but hand-building a window
//      with twenty controls is a huge amount of code for what this is.
//    - Packaged Python: you have to install Python and PyInstaller, and the
//      exe ends up weighing 30 MB.
//    - C# with the compiler Windows already ships: a single file, the same
//      controls the PowerShell launcher already used -WinForms is what was
//      underneath-, icon and cover art inside the exe, and zero installs.
//
//  THIS IS BUILT WITH AN OLD csc
//  =============================
//  The one Windows ships is C# 5 (2012). So nothing modern can be used here:
//  no interpolated strings $"...", no ?., no nameof, no expression-bodied
//  members =>. Everything with string.Format and classic syntax. If any of
//  that slips in, the error it prints does not say "you need a newer
//  compiler", it says weird things about missing ';', and you lose half an
//  afternoon.
//
//  THE SETTINGS ARE SHARED WITH THE OLD LAUNCHER
//  =============================================
//  The SAME lanzador.json is read and written, with the same field names.
//  So any configuration you already had is preserved, and both launchers
//  coexist without stepping on each other.
// =============================================================================

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace NfsmwRecomp
{
    // -------------------------------------------------------------------------
    //  A plain json parser, by hand
    //
    //  The settings file is a dozen or so non-nested key/value pairs.
    //  For that there is no need to bring in Newtonsoft (which would have to be
    //  downloaded) nor JavaScriptSerializer (which forces referencing
    //  System.Web.Extensions). The only thing to be careful about is the
    //  backslashes in Windows paths, which are doubled in json.
    // -------------------------------------------------------------------------
    internal static class Json
    {
        public static Dictionary<string, string> Leer(string texto)
        {
            Dictionary<string, string> d = new Dictionary<string, string>();
            if (texto == null)
                return d;

            int i = 0;
            while (i < texto.Length)
            {
                // Find the quote that opens a key.
                while (i < texto.Length && texto[i] != '"')
                    i++;
                if (i >= texto.Length)
                    break;

                string clave = LeerCadena(texto, ref i);

                // Skip to the colon.
                while (i < texto.Length && texto[i] != ':')
                    i++;
                if (i >= texto.Length)
                    break;
                i++;

                while (i < texto.Length && char.IsWhiteSpace(texto[i]))
                    i++;
                if (i >= texto.Length)
                    break;

                string valor;
                if (texto[i] == '"')
                {
                    valor = LeerCadena(texto, ref i);
                }
                else
                {
                    int desde = i;
                    while (i < texto.Length && texto[i] != ',' && texto[i] != '}' &&
                           texto[i] != '\r' && texto[i] != '\n')
                        i++;
                    valor = texto.Substring(desde, i - desde).Trim();
                }

                if (clave.Length > 0)
                    d[clave] = valor;
            }
            return d;
        }

        // Enters pointing at the opening quote, exits after the closing one.
        private static string LeerCadena(string texto, ref int i)
        {
            StringBuilder sb = new StringBuilder();
            i++;  // the opening quote
            while (i < texto.Length && texto[i] != '"')
            {
                if (texto[i] == '\\' && i + 1 < texto.Length)
                {
                    i++;
                    char c = texto[i];
                    if (c == 'n') sb.Append('\n');
                    else if (c == 'r') sb.Append('\r');
                    else if (c == 't') sb.Append('\t');
                    else if (c == 'u' && i + 4 < texto.Length)
                    {
                        int cod;
                        if (int.TryParse(texto.Substring(i + 1, 4), NumberStyles.HexNumber,
                                         CultureInfo.InvariantCulture, out cod))
                        {
                            sb.Append((char)cod);
                            i += 4;
                        }
                    }
                    else sb.Append(c);   // \\ and \/ and \" fall through here
                }
                else
                {
                    sb.Append(texto[i]);
                }
                i++;
            }
            i++;  // the closing quote
            return sb.ToString();
        }

        public static string Escapar(string s)
        {
            StringBuilder sb = new StringBuilder();
            foreach (char c in s)
            {
                if (c == '"' || c == '\\') { sb.Append('\\'); sb.Append(c); }
                else if (c == '\n') sb.Append("\\n");
                else if (c == '\r') sb.Append("\\r");
                else if (c == '\t') sb.Append("\\t");
                else if (c < ' ') sb.Append("\\u" + ((int)c).ToString("x4"));
                else sb.Append(c);
            }
            return sb.ToString();
        }
    }

    // -------------------------------------------------------------------------
    //  The cover panel
    //
    //  It is painted by hand instead of using a PictureBox because control is
    //  needed over HOW the image fits. The cover is 760x1064 -ratio 0.71- and
    //  the panel is much narrower and taller than that.
    //
    //  If it were stretched to fill the panel, it would have to be cropped on
    //  the sides and that would eat part of the title, which spans the full
    //  width at the top. So it is drawn WHOLE, pinned to the top, and the gap
    //  below is used for the project name. Which is exactly the look of an
    //  installer's side banner.
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    //  Dark theme, phosphor-green terminal style
    //
    //  Colors centralized here to avoid repeating the same Color.FromArgb in
    //  twenty places. AplicarTema() (further down, in Ventana) spreads them
    //  automatically by walking the control tree, so adding a new control does
    //  not force you to remember to color it by hand.
    // -------------------------------------------------------------------------
    internal static class Tema
    {
        public static readonly Color Fondo = Color.FromArgb(8, 13, 8);
        public static readonly Color FondoPanel = Color.FromArgb(14, 22, 14);
        public static readonly Color FondoCampo = Color.FromArgb(4, 8, 4);
        public static readonly Color Borde = Color.FromArgb(58, 105, 58);
        public static readonly Color BordeSuave = Color.FromArgb(34, 60, 34);
        public static readonly Color Texto = Color.FromArgb(160, 225, 160);
        public static readonly Color TextoTitulo = Color.FromArgb(200, 255, 200);
        public static readonly Color TextoNota = Color.FromArgb(95, 145, 95);
        public static readonly Color Aviso = Color.FromArgb(255, 150, 90);
        public static readonly Color Acento = Color.FromArgb(255, 210, 90);
        public static readonly Font Mono = new Font("Consolas", 9f);
    }

    // -------------------------------------------------------------------------
    //  GroupBox replacement for the dark theme
    //
    //  The native GroupBox paints its border with the Windows visual theme
    //  (UxTheme), which assumes a light background: over a dark panel it leaves
    //  a halo or a line that matches nothing. The border and title are drawn by
    //  hand -same trick PanelPortada already used for the cover- instead of
    //  fighting the native style.
    // -------------------------------------------------------------------------
    internal sealed class PanelSeccion : Panel
    {
        public string Titulo;

        public PanelSeccion(string titulo)
        {
            Titulo = titulo;
            BackColor = Tema.FondoPanel;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            Graphics g = e.Graphics;

            using (SolidBrush fondo = new SolidBrush(Tema.FondoPanel))
                g.FillRectangle(fondo, ClientRectangle);

            using (Font f = new Font("Segoe UI", 8.5f, FontStyle.Bold))
            {
                SizeF medida = string.IsNullOrEmpty(Titulo) ? SizeF.Empty : g.MeasureString(Titulo, f);

                const int y = 7;
                const int huecoInicio = 10;
                int huecoFin = medida.Width > 0 ? (int)(huecoInicio + medida.Width + 8) : huecoInicio;

                // The border leaves a gap where the title goes, like a
                // traditional GroupBox: two line segments with a space in the
                // middle instead of a full rectangle.
                using (Pen borde = new Pen(Tema.Borde))
                {
                    g.DrawLine(borde, 0, y, huecoInicio, y);
                    if (huecoFin < Width)
                        g.DrawLine(borde, huecoFin, y, Width - 1, y);
                    g.DrawLine(borde, 0, y, 0, Height - 1);
                    g.DrawLine(borde, Width - 1, y, Width - 1, Height - 1);
                    g.DrawLine(borde, 0, Height - 1, Width - 1, Height - 1);
                }

                if (!string.IsNullOrEmpty(Titulo))
                {
                    using (SolidBrush textoBrush = new SolidBrush(Tema.TextoTitulo))
                        g.DrawString(Titulo, f, textoBrush, huecoInicio + 4, y - medida.Height / 2f);
                }
            }
        }
    }

    internal sealed class PanelPortada : Panel
    {
        private readonly Image portada;

        public PanelPortada(Image portada)
        {
            this.portada = portada;
            BackColor = Color.Black;
            // Without this the image flickers when resizing and when dragging
            // the window over others.
            //
            // ResizeRedraw IS THE ONE THAT MATTERS HERE. Without it, when the
            // panel grows -maximizing the window, for example- Windows only
            // invalidates the new exposed STRIP, not the whole panel: the top
            // portion keeps the old pixels, painted for the old height, and
            // the new bottom strip is painted separately with OnPaint
            // recalculating the image for the NEW height -a different "cover"
            // scale-. The result is two crops of the same cover, one on top of
            // the other, each with its own "Native recompilation" text:
            // exactly the "duplicated banner" that showed up when enlarging
            // the window. With ResizeRedraw, any size change invalidates the
            // WHOLE panel and OnPaint runs again entirely with the new height,
            // with no leftovers from the previous one.
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            e.Graphics.Clear(Color.Black);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;

            if (portada != null)
            {
                // "Cover", not "fit": it scales along whichever axis is
                // needed to cover the WHOLE panel -width and height at once-,
                // cropping whatever is left over on the other axis. It used to
                // scale only by width and stop if the image did not reach the
                // panel height: in a column taller than it is wide -which this
                // one is-, that left an empty black strip under the photo.
                // Covering the whole thing leaves no gap, at any window ratio.
                double escala = Math.Max((double)Width / portada.Width, (double)Height / portada.Height);
                int ancho = (int)Math.Ceiling(portada.Width * escala);
                int alto = (int)Math.Ceiling(portada.Height * escala);
                g.DrawImage(portada, (Width - ancho) / 2, (Height - alto) / 2, ancho, alto);
            }
            else
            {
                // Without portada.jpg -the cover is EA art and therefore not
                // in the repository, see CONSTRUIR_LANZADOR.bat- the panel was
                // plain black with two lines of text stuck at the bottom: it
                // read as an empty gap, not as a side banner.
                //
                // This fills that gap with something of its own: a bar pattern
                // -equalizer/data-readout style- that reproduces nothing from
                // the game, it only decorates in the same green as the rest of
                // the window. Fixed seed so it neither flickers nor changes
                // between repaints.
                DibujarPatronDeRelleno(g, Math.Max(0, Height - 110));
            }

            // Permanent gradient on the bottom strip, WHATEVER HAPPENS with
            // the image -whether it covers everything or not-: it is what
            // keeps the text readable over any photo, it does not just hide a
            // seam.
            {
                int difuminado = Math.Min(110, Height);
                Rectangle r = new Rectangle(0, Height - difuminado, Width, difuminado);
                if (r.Height > 0)
                {
                    using (LinearGradientBrush b = new LinearGradientBrush(
                               r, Color.FromArgb(0, 0, 0, 0), Color.FromArgb(230, 0, 0, 0), 90f))
                        g.FillRectangle(b, r);
                }
            }

            using (Font f1 = new Font("Segoe UI", 12f, FontStyle.Bold))
            using (Font f2 = new Font("Segoe UI", 8.25f))
            using (SolidBrush brillante = new SolidBrush(Tema.TextoTitulo))
            using (SolidBrush apagado = new SolidBrush(Tema.TextoNota))
            {
                int y = Height - 96;
                g.DrawString("Native recompilation", f1, brillante, 18, y);
                g.DrawString("Xbox 360 ported to PC with ReXGlue.\n" +
                             "Requires your own copy of the game.",
                             f2, apagado, new RectangleF(18, y + 26, Width - 36, 60));
            }
        }

        // Vertical bars of pseudo-random height -equalizer style-, inside a
        // rectangle altoDisponible px tall from the top. Random with a fixed
        // seed: always the same drawing, it does not change between repaints
        // nor flicker when resizing.
        private void DibujarPatronDeRelleno(Graphics g, int altoDisponible)
        {
            if (altoDisponible <= 0 || Width <= 0)
                return;

            Random azar = new Random(454107); // NFSMW's title id, just to use something fixed
            const int anchoBarra = 5;
            const int hueco = 3;
            int paso = anchoBarra + hueco;

            using (SolidBrush apagada = new SolidBrush(Color.FromArgb(28, Tema.Texto)))
            using (SolidBrush media = new SolidBrush(Color.FromArgb(55, Tema.Texto)))
            {
                for (int x = paso; x < Width - paso; x += paso)
                {
                    double t = (double)x / Width;
                    // Two soft "hills" so it is not pure flat noise:
                    // the pattern rises toward the center and falls back down.
                    double envolvente = 0.35 + 0.65 * Math.Sin(t * Math.PI);
                    int alturaBase = (int)(altoDisponible * envolvente * (0.25 + azar.NextDouble() * 0.55));
                    if (alturaBase < 4)
                        continue;

                    bool destacada = azar.NextDouble() > 0.82;
                    Brush brocha = destacada ? media : apagada;
                    int y0 = altoDisponible - alturaBase;
                    g.FillRectangle(brocha, x, y0, anchoBarra, alturaBase);
                }
            }
        }
    }

    internal sealed class Ventana : Form
    {
        // ---- Presets, taken from the SDK's TryParseResolutionPreset ----------
        private static readonly string[,] Presets = {
            { "480p  - 640 x 480",   "480p"   },
            { "540p  - 960 x 540",   "540p"   },
            { "720p  - 1280 x 720",  "720p"   },
            { "900p  - 1600 x 900",  "900p"   },
            { "1080p - 1920 x 1080", "1080p"  },
            { "1440p - 2560 x 1440", "1440p"  },
            { "1800p - 3200 x 1800", "1800p"  },
            { "2160p - 3840 x 2160", "2160p"  },
            { "Custom",              "custom" },
        };

        // The texts carry the "x" in front because that is the number people
        // look for: it is the same control as the "internal resolution x2" of
        // any emulator. They are saved as-is in lanzador.json, so changing them
        // breaks compatibility with saved settings; that is why CargarAjustes
        // falls back to the first option when it does not recognize the text,
        // instead of failing.
        private static readonly string[,] Escalas = {
            { "x1  - Xbox 360 original",       "1" },
            { "x2  - 4 times the pixels",      "2" },
            { "x3  - 9 times the pixels",      "3" },
            { "x4  - 16 times the pixels",     "4" },
        };

        // Post-process antialiasing (--swap_post_effect). The value is the one
        // the recomp cvar expects: none / fxaa / fxaa_extreme. It is applied
        // when restarting the game, same as the resolution.
        private static readonly string[,] Antialias = {
            { "Off",          "none" },
            { "FXAA",         "fxaa" },
            { "FXAA Extreme", "fxaa_extreme" },
        };

        // Anisotropic filtering (--anisotropic_override). The recomp forces
        // texture filtering even if the game does not ask for it; 0 turns it
        // off.
        private static readonly string[,] Anisotropico = {
            { "Off (bilinear)", "0" },
            { "1x",                     "1" },
            { "2x",                     "2" },
            { "4x",                     "3" },
            { "8x",                     "4" },
            { "16x",                    "5" },
        };

        // Effect when presenting the final image to the window
        // (--present_effect). These are the ones the ReXGlue SDK ships
        // (FidelityFX); if this runtime did not have them, the cvar rejects
        // the value and stays on bilinear, without breaking.
        private static readonly string[,] Efectos = {
            { "None (bilinear)", "bilinear" },
            { "CAS (sharpness)", "cas" },
            { "FSR (FidelityFX)", "fsr" },
        };

        // ---- Where we are -----------------------------------------------------
        private string raiz;
        private string exeJuego;
        private string dirLogs;
        private string ficheroAjustes;
        private string fuentePresentador;
        private string logEjecucion;
        private bool distribuida;

        // ---- Controls ---------------------------------------------------------
        private Panel panelContenido;
        private Panel panelColumna;
        private PanelPortada banda;
        private TextBox txtIso;
        private ComboBox cboRes, cboEsc, cboAA, cboAniso, cboEfecto, cboMon;
        private NumericUpDown numAncho, numAlto, numFps, numNitidez;
        private RadioButton rbCompleta, rbVentana;
        private CheckBox chkVsync, chkLimite;
        private RadioButton rbVidAuto, rbVidRtv, rbVidRov;
        private RadioButton rbApiDx, rbApiVk;
        private Label lblParche, lblEstado, lblEscala;
        private TextBox txtCmd;
        private Button btnJugar, btnSalir;
        private bool cargando = true;

        [DllImport("user32.dll")]
        private static extern bool SetProcessDPIAware();

        [STAThread]
        public static void Main()
        {
            // Without this, on a monitor with Windows scaling (125%, 150%...)
            // the system does NOT actually rescale the window: it draws it at
            // normal size and then stretches the resulting bitmap, and
            // everything comes out blurry -text included-. With the process
            // marked DPI-aware, Windows stops stretching, and AutoScaleMode.Dpi
            // in Ventana (see Construir) makes the pixel coordinates in this
            // file -designed for 96 DPI- actually rescale, not just look
            // sharp.
            //
            // SetProcessDPIAware (System-DPI-aware) and not the newer
            // Per-Monitor V2 mode: the latter requires declaring it in an
            // embedded manifest, and csc.exe -the old compiler used here- has
            // no simple way to embed one without extra tools. This covers the
            // real case -opening on your usual monitor with its usual scaling-
            // without depending on anything else.
            try
            {
                SetProcessDPIAware();
            }
            catch
            {
                // Very old Windows without this API: it just continues
                // unmarked, and the worst case is the usual one (stretched
                // bitmap). Not a reason to refuse to open.
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new Ventana());
        }

        public Ventana()
        {
            LocalizarTodo();
            Construir();
            CargarAjustes();
            cargando = false;
            EstadoInicial();
            Refrescar();
        }

        // ---------------------------------------------------------------------
        //  Where everything is
        //
        //  The exe can live in two places and must work in both:
        //
        //    distribution  the portable folder, next to nfsmw.exe. There is no
        //              project or SDK there: only the game, and everything
        //              -logs, settings, ISO- hangs off that same folder.
        //    project   the root of "NFSMW Recomp". The game is compiled in
        //              app\out\build\..., and there is an SDK next to it to
        //              check.
        //
        //  THE GAME IS CALLED nfsmw.exe, NOT NFS_Most_Wanted.exe
        //  ======================================================
        //  In the portable folder, "NFS_Most_Wanted.exe" IS THIS LAUNCHER. The
        //  real game is called nfsmw.exe, which is also its name in the project
        //  tree, so both modes look for the same name.
        //
        //  The reason is just that: double-clicking the game icon brings up this
        //  window. It is what any game with a launcher does, and swapping the
        //  names is how to achieve it without touching the game code.
        //
        //  It is NOT that the game cannot start on its own: it can. nfsmw_app.h
        //  sets gpu_plugin, mnk_mode and readback_resolve if nobody asked for
        //  them, and looks for an ISO in its own folder. Opening nfsmw.exe
        //  directly still works, and it is a useful fallback if the launcher
        //  caused trouble.
        //
        //  One nuance of that ISO finder: it prefers the one named the SAME as
        //  the executable, and otherwise takes the first in alphabetical order.
        //  After the rename, an NFS_Most_Wanted.iso is no longer preferred and
        //  starts matching the second rule. With a single ISO in the folder it
        //  makes no difference; with several, it could pick another one. It does
        //  not matter when opening from here, because this window passes an
        //  explicit --game_data_root.
        //
        //  And it does not change where the game keeps its things: the SDK
        //  derives that folder from GetName(), which is in the code -"nfsmw"-,
        //  not from the file name. See rex_app.cpp:
        //  user_dir = GetUserFolder() / GetName(). So the shader cache stays
        //  where it was.
        // ---------------------------------------------------------------------
        private void LocalizarTodo()
        {
            string mio = Path.GetDirectoryName(Application.ExecutablePath);

            // It may be in the project root or inside tools\; one level up is
            // also checked before giving up.
            string[] candidatos = { mio, Path.GetFullPath(Path.Combine(mio, "..")) };

            foreach (string c in candidatos)
            {
                if (File.Exists(Path.Combine(c, "nfsmw.exe")))
                {
                    distribuida = true;
                    raiz = c;
                    exeJuego = Path.Combine(c, "nfsmw.exe");
                    dirLogs = Path.Combine(c, "logs");
                    ficheroAjustes = Path.Combine(c, "lanzador.json");
                    fuentePresentador = null;
                    logEjecucion = Path.Combine(dirLogs, "lanzador.log");
                    return;
                }
            }

            foreach (string c in candidatos)
            {
                string j = Path.Combine(c, @"app\out\build\win-amd64-release\nfsmw.exe");
                if (File.Exists(j) || Directory.Exists(Path.Combine(c, "app")))
                {
                    distribuida = false;
                    raiz = c;
                    exeJuego = j;
                    dirLogs = Path.Combine(c, "logs");
                    ficheroAjustes = Path.Combine(c, @"config\lanzador.json");
                    fuentePresentador = Path.Combine(
                        Path.GetFullPath(Path.Combine(c, "..")),
                        @"rexglue-sdk\src\ui\d3d12\d3d12_presenter.cpp");
                    logEjecucion = Path.Combine(dirLogs, "lanzador.log");
                    return;
                }
            }

            // Neither one nor the other. Project is assumed and it will warn on
            // startup that it cannot find the executable; it is better to open
            // the window with a warning than to open nothing.
            distribuida = false;
            raiz = mio;
            exeJuego = Path.Combine(mio, @"app\out\build\win-amd64-release\nfsmw.exe");
            dirLogs = Path.Combine(mio, "logs");
            ficheroAjustes = Path.Combine(mio, @"config\lanzador.json");
            fuentePresentador = null;
            logEjecucion = Path.Combine(dirLogs, "lanzador.log");
        }

        private static Image CargarRecurso(string nombre)
        {
            try
            {
                Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream(nombre);
                if (s == null)
                    return null;
                using (s)
                    return Image.FromStream(s);
            }
            catch
            {
                // Without the cover the window looks odd, but it shows. Not a
                // reason to prevent playing.
                return null;
            }
        }

        // ---------------------------------------------------------------------
        //  The window
        // ---------------------------------------------------------------------
        private const int AnchoBanda = 380;
        // Two columns of settings side by side instead of a single stacked one:
        // almost half the height -less vertical scroll- and it actually uses
        // the width left over on a normal screen, not just centering it with a
        // gap beside it. AltoUtil, X1 and AnchoTotalColumnas are recalculated
        // further down from where each section lands; the numbers here are the
        // final result, noted down so there is no need to reread all of
        // Construir() every time the section order is touched.
        private const int AltoUtil = 810;
        private const int X0 = 20;   // left column
        private const int AnchoCol = 580;
        private const int GapCol = 24;
        private const int X1 = X0 + AnchoCol + GapCol;   // right column
        private const int AnchoTotalColumnas = AnchoCol * 2 + GapCol;   // width of what spans the full width

        private static string[,] Monitores()
        {
            Screen[] pantallas = Screen.AllScreens;
            string[,] m = new string[pantallas.Length + 1, 2];
            m[0, 0] = "Automatic (default)";
            m[0, 1] = "0";
            for (int i = 0; i < pantallas.Length; i++)
            {
                m[i + 1, 0] = "Monitor " + (i + 1) + " - " + pantallas[i].Bounds.Width + "x" +
                              pantallas[i].Bounds.Height + " (" + pantallas[i].DeviceName + ")";
                m[i + 1, 1] = (i + 1).ToString(CultureInfo.InvariantCulture);
            }
            return m;
        }

        private void Construir()
        {
            Text = "Need for Speed: Most Wanted - Recompilation";

            // ------------------------------------------------------------------
            //  Window size: it fits the screen, and can be scaled
            //
            //  AltoUtil (1066) is the NATURAL height of all the controls, but
            //  it does not fit whole on a 1366x768 laptop. The window OPENS
            //  shorter/narrower when needed, and the right-hand content
            //  -everything except the cover- lives in a scrollable panel
            //  (further down).
            //
            //  Also the window is RESIZABLE (Sizable, with a maximize button):
            //  the banner and panelContenido are anchored, so enlarging the
            //  window stretches the cover vertically and the settings panel
            //  genuinely gains width and height -not just more scroll-, and if
            //  enlarged enough the scroll disappears on its own because the
            //  content already fits entirely.
            // ------------------------------------------------------------------
            int anchoTotal = AnchoBanda + AnchoTotalColumnas + 40;
            Rectangle area = Screen.PrimaryScreen.WorkingArea;
            int altoForm = Math.Min(AltoUtil, Math.Max(420, area.Height - 60));
            int anchoForm = Math.Min(anchoTotal, Math.Max(760, area.Width - 60));

            ClientSize = new Size(anchoForm, altoForm);
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.Sizable;
            MaximizeBox = true;
            MinimumSize = new Size(760, 480);
            BackColor = Tema.Fondo;
            Font = new Font("Segoe UI", 8.25f);

            try
            {
                Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
            }
            catch
            {
                // It does not matter: the compiler already sets the exe icon.
            }

            banda = new PanelPortada(CargarRecurso("portada.jpg"));
            banda.Location = new Point(0, 0);
            banda.Size = new Size(AnchoBanda, altoForm);
            // WITHOUT Anchor. Width, height and position are recalculated by
            // AjustarLayout on every resize (further down) -it is the ONLY one
            // that touches them-, for both the banner and panelContenido.
            // Mixing Anchor with manual Size assignments on the same control
            // is the classic recipe for one stepping on the other at different
            // points of the layout cycle: that was what left the banner taller
            // than its real content and the cover repeated to fill that excess.
            Controls.Add(banda);

            // Everything else -the settings groups, the command and the
            // buttons- lives in here. AutoScroll adds a vertical bar on its own
            // as soon as the content (AltoUtil) does not fit in altoForm: that
            // is what makes the launcher usable on small screens without
            // touching the rest of the layout.
            panelContenido = new Panel();
            panelContenido.Location = new Point(AnchoBanda, 0);
            panelContenido.Size = new Size(anchoForm - AnchoBanda, altoForm);
            // Also without Anchor, same reason as the banner.
            panelContenido.AutoScroll = true;
            panelContenido.BackColor = BackColor;
            Controls.Add(panelContenido);

            // panelColumna is the NATURAL width of the content -the same as
            // always, AnchoCol+40- placed inside panelContenido. In a wide
            // window, panelContenido has more room than needed; AjustarLayout
            // centers panelColumna in that excess instead of leaving everything
            // stuck to the left with a huge gap on the right. The groups,
            // buttons and so on hang off HERE, not off panelContenido directly.
            panelColumna = new Panel();
            panelColumna.Size = new Size(AnchoTotalColumnas + 40, AltoUtil);
            panelColumna.BackColor = BackColor;
            panelContenido.Controls.Add(panelColumna);
            panelContenido.AutoScrollMinSize = new Size(panelColumna.Width, AltoUtil);

            // ---- Game copy: ISO or already extracted folder --------------------
            //
            // The SDK (rex_app.cpp) requires --game_data_root to be a FOLDER:
            // it does not know how to mount a .iso directly. So both things are
            // accepted here and, if a .iso is chosen, a copy is extracted to
            // game_root_cache\ the first time (see ExtractorXdvdfs further
            // down); subsequent times with the same ISO start directly, without
            // extracting again.
            PanelSeccion gIso = Grupo("Game copy (ISO or extracted folder)", X0, 14, 110,
                                      AnchoTotalColumnas);

            txtIso = new TextBox();
            txtIso.Location = new Point(14, 26);
            txtIso.Size = new Size(AnchoTotalColumnas - 224, 23);
            txtIso.TextChanged += delegate { Refrescar(); };
            gIso.Controls.Add(txtIso);

            Button btnIso = new Button();
            btnIso.Text = "ISO...";
            btnIso.Location = new Point(AnchoTotalColumnas - 196, 25);
            btnIso.Size = new Size(94, 25);
            btnIso.Click += ElegirIso;
            gIso.Controls.Add(btnIso);

            Button btnCarpeta = new Button();
            btnCarpeta.Text = "Folder...";
            btnCarpeta.Location = new Point(AnchoTotalColumnas - 98, 25);
            btnCarpeta.Size = new Size(94, 25);
            btnCarpeta.Click += ElegirCarpeta;
            gIso.Controls.Add(btnCarpeta);

            gIso.Controls.Add(Nota(14, 58, AnchoTotalColumnas - 40, 44,
                "You can choose a .iso or an already extracted folder (with default.xex " +
                "inside, like the one EXTRAER_XEX.bat builds). The first time with an ISO a " +
                "copy is extracted into game_root_cache\\; subsequent times it starts directly " +
                "with that copy."));

            // ---- Display and resolution --------------------------------------
            //
            // THE TWO SETTINGS HERE ARE NOT THE SAME, AND THEY GET CONFUSED
            // ==============================================================
            // It is THE confusion of this window, so the names are chosen so it
            // does not happen:
            //
            //   "Window size"        -> --resolution. Changes the video mode
            //       the game believes it has and the window size. It does NOT
            //       ask the game to draw more finely: Most Wanted, like almost
            //       every 360 game, draws into its own fixed-size render targets
            //       and lets the scaler stretch the result. Raising this
            //       enlarges the image, it does not improve it.
            //
            //   "Internal resolution" -> --resolution_scale. THIS is the one
            //       people look for: the same "x2" as any emulator. It
            //       multiplies the size of the render targets and the emulated
            //       EDRAM, so the game genuinely draws more pixels.
            //
            // They used to be called "Output resolution" and "Render scale",
            // and with those names it is easy to touch the first expecting the
            // second, see that nothing changes and call it broken.
            PanelSeccion gPant = Grupo("Display and resolution", 132, 252);

            gPant.Controls.Add(Etiqueta("Window size", 14, 26, 150));
            cboRes = new ComboBox();
            cboRes.DropDownStyle = ComboBoxStyle.DropDownList;
            cboRes.Location = new Point(168, 23);
            cboRes.Size = new Size(200, 23);
            for (int i = 0; i < Presets.GetLength(0); i++)
                cboRes.Items.Add(Presets[i, 0]);
            cboRes.SelectedIndexChanged += delegate { Refrescar(); };
            gPant.Controls.Add(cboRes);

            numAncho = Numero(168, 52, 70, 320, 7680);
            numAlto = Numero(250, 52, 70, 240, 4320);
            gPant.Controls.Add(Etiqueta("Custom", 14, 55, 150));
            gPant.Controls.Add(numAncho);
            gPant.Controls.Add(Etiqueta("x", 240, 55, 12));
            gPant.Controls.Add(numAlto);

            gPant.Controls.Add(Etiqueta("Internal resolution", 14, 87, 150));
            cboEsc = new ComboBox();
            cboEsc.DropDownStyle = ComboBoxStyle.DropDownList;
            cboEsc.Location = new Point(168, 84);
            cboEsc.Size = new Size(200, 23);
            for (int i = 0; i < Escalas.GetLength(0); i++)
                cboEsc.Items.Add(Escalas[i, 0]);
            cboEsc.SelectedIndexChanged += delegate { Refrescar(); };
            gPant.Controls.Add(cboEsc);

            // What the chosen scale actually does, written on every change.
            // Without this, choosing x2 and choosing x1 look the same until you
            // start the game.
            lblEscala = new Label();
            lblEscala.Location = new Point(168, 110);
            lblEscala.Size = new Size(AnchoCol - 190, 32);
            gPant.Controls.Add(lblEscala);

            rbCompleta = Radio("Fullscreen", 14, 146, 150);
            rbVentana = Radio("Windowed", 168, 146, 150);
            gPant.Controls.Add(rbCompleta);
            gPant.Controls.Add(rbVentana);

            gPant.Controls.Add(Etiqueta("Output monitor", 14, 180, 150));
            cboMon = new ComboBox();
            cboMon.DropDownStyle = ComboBoxStyle.DropDownList;
            cboMon.Location = new Point(168, 177);
            cboMon.Size = new Size(200, 23);
            string[,] mon = Monitores();
            for (int i = 0; i < mon.GetLength(0); i++)
                cboMon.Items.Add(mon[i, 0]);
            cboMon.SelectedIndexChanged += delegate { Refrescar(); };
            gPant.Controls.Add(cboMon);

            gPant.Controls.Add(Nota(14, 214, AnchoCol - 40, 36,
                "They are not the same: window size only ENLARGES the image. The one that " +
                "makes it finer is the internal resolution, which is the same \"x2\" as the " +
                "emulators, and it is expensive: x2 is four times the pixels to draw."));

            // ---- Image quality -------------------------------------------------
            //
            // These are all ReXGlue SDK cvars that the recomp reads from the
            // command line; neither the game nor the runtime is touched here.
            PanelSeccion gCal = Grupo("Image quality", 388, 196);

            gCal.Controls.Add(Etiqueta("Antialiasing", 14, 26, 150));
            cboAA = new ComboBox();
            cboAA.DropDownStyle = ComboBoxStyle.DropDownList;
            cboAA.Location = new Point(168, 23);
            cboAA.Size = new Size(200, 23);
            for (int i = 0; i < Antialias.GetLength(0); i++)
                cboAA.Items.Add(Antialias[i, 0]);
            cboAA.SelectedIndexChanged += delegate { Refrescar(); };
            gCal.Controls.Add(cboAA);

            gCal.Controls.Add(Etiqueta("Anisotropic filtering", 14, 58, 150));
            cboAniso = new ComboBox();
            cboAniso.DropDownStyle = ComboBoxStyle.DropDownList;
            cboAniso.Location = new Point(168, 55);
            cboAniso.Size = new Size(200, 23);
            for (int i = 0; i < Anisotropico.GetLength(0); i++)
                cboAniso.Items.Add(Anisotropico[i, 0]);
            cboAniso.SelectedIndexChanged += delegate { Refrescar(); };
            gCal.Controls.Add(cboAniso);

            gCal.Controls.Add(Etiqueta("Presentation effect", 14, 90, 150));
            cboEfecto = new ComboBox();
            cboEfecto.DropDownStyle = ComboBoxStyle.DropDownList;
            cboEfecto.Location = new Point(168, 87);
            cboEfecto.Size = new Size(200, 23);
            for (int i = 0; i < Efectos.GetLength(0); i++)
                cboEfecto.Items.Add(Efectos[i, 0]);
            cboEfecto.SelectedIndexChanged += delegate { Refrescar(); };
            gCal.Controls.Add(cboEfecto);

            gCal.Controls.Add(Etiqueta("Sharpness (CAS)", 14, 122, 150));
            numNitidez = Numero(168, 119, 90, 0, 100);
            gCal.Controls.Add(numNitidez);
            gCal.Controls.Add(Etiqueta("%", 264, 122, 20));

            gCal.Controls.Add(Nota(14, 156, AnchoCol - 40, 34,
                "Anisotropic filtering sharpens textures and the presentation effect polishes " +
                "the image as it is passed to the window. They are applied when the game " +
                "restarts."));

            // ---- Frames -------------------------------------------------------
            PanelSeccion gFps = Grupo("Frames", X1, 132, 124);

            chkVsync = Marca("Vertical sync (vsync)", 14, 24, 250);
            gFps.Controls.Add(chkVsync);

            chkLimite = Marca("Limit to", 14, 52, 90);
            gFps.Controls.Add(chkLimite);
            numFps = Numero(108, 50, 70, 20, 300);
            gFps.Controls.Add(numFps);
            gFps.Controls.Add(Etiqueta("fps", 184, 53, 40));

            gFps.Controls.Add(Nota(14, 82, AnchoCol - 40, 34,
                "Both require parche_presentador.py. The game speed does not depend on " +
                "this: it is adjusted from the F4 menu."));

            // ---- Video engine --------------------------------------------------
            PanelSeccion gVideo = Grupo("Video engine (EDRAM emulation)", X1, 264, 92);

            rbVidAuto = Radio("Automatic", 14, 24, 110);
            rbVidRtv = Radio("Fast (rtv)", 134, 24, 120);
            rbVidRov = Radio("Accurate (rov)", 264, 24, 120);
            gVideo.Controls.Add(rbVidAuto);
            gVideo.Controls.Add(rbVidRtv);
            gVideo.Controls.Add(rbVidRov);

            gVideo.Controls.Add(Nota(14, 50, AnchoCol - 40, 34,
                "Automatic uses whatever nfsmw.toml says. Fast can double the fps on " +
                "integrated graphics. Accurate always looks right and runs slower."));

            // ---- Graphics API ---------------------------------------------------
            //
            // THIS GROUP IS AN EMERGENCY EXIT, AND THAT IS WHY IT HAS NO
            // 'AUTOMATIC'. See the long comment in ConstruirArgumentos.
            PanelSeccion gApi = Grupo("Graphics API", X1, 364, 92);

            rbApiDx = Radio("DirectX 12 (recommended)", 14, 24, 190);
            rbApiVk = Radio("Vulkan (experimental)", 214, 24, 190);
            gApi.Controls.Add(rbApiDx);
            gApi.Controls.Add(rbApiVk);

            gApi.Controls.Add(Nota(14, 50, AnchoCol - 40, 34,
                "This window overrides nfsmw.toml, so choosing wrong here never leaves the " +
                "game unable to open: you come back and change it."));

            // From here on everything spans the FULL WIDTH, below the two
            // columns (the left one -Display+Quality- is the tallest, down to
            // y=584; see the comments on gPant/gCal and gFps/gVideo/gApi above
            // if the section order is changed).
            const int yDebajoColumnas = 600;

            // ---- Patch warning ----------------------------------------------
            lblParche = new Label();
            lblParche.Location = new Point(X0, yDebajoColumnas);
            lblParche.Size = new Size(AnchoTotalColumnas, 32);
            lblParche.ForeColor = Tema.Aviso;
            panelColumna.Controls.Add(lblParche);

            // ---- What is going to run ----------------------------------------
            PanelSeccion gCmd = Grupo("What is going to run", X0, yDebajoColumnas + 40, 100,
                                      AnchoTotalColumnas);
            txtCmd = new TextBox();
            txtCmd.Location = new Point(12, 20);
            txtCmd.Size = new Size(AnchoTotalColumnas - 24, 72);
            txtCmd.Multiline = true;
            txtCmd.ReadOnly = true;
            txtCmd.ScrollBars = ScrollBars.Vertical;
            txtCmd.BackColor = Tema.FondoCampo;
            txtCmd.ForeColor = Tema.TextoTitulo;
            txtCmd.Font = new Font("Consolas", 7.5f);
            gCmd.Controls.Add(txtCmd);

            // ---- Buttons ------------------------------------------------------
            int yBotones = yDebajoColumnas + 40 + 100 + 10;
            btnJugar = new Button();
            btnJugar.Text = "PLAY";
            btnJugar.Location = new Point(X1 + AnchoCol - 230, yBotones);
            btnJugar.Size = new Size(120, 30);
            btnJugar.Font = new Font("Segoe UI", 9.75f, FontStyle.Bold);
            btnJugar.Click += Jugar;
            panelColumna.Controls.Add(btnJugar);
            AcceptButton = btnJugar;

            btnSalir = new Button();
            btnSalir.Text = "Exit";
            btnSalir.Location = new Point(X1 + AnchoCol - 100, yBotones);
            btnSalir.Size = new Size(100, 30);
            btnSalir.Click += delegate { Close(); };
            panelColumna.Controls.Add(btnSalir);

            lblEstado = new Label();
            lblEstado.Location = new Point(X0, yBotones + 6);
            lblEstado.Size = new Size(320, 32);
            lblEstado.ForeColor = Tema.TextoNota;
            panelColumna.Controls.Add(lblEstado);

            // Everything that changes the command line gets refreshed.
            EventHandler r = delegate { Refrescar(); };
            chkVsync.CheckedChanged += r;
            chkLimite.CheckedChanged += r;
            rbCompleta.CheckedChanged += r;
            rbVidAuto.CheckedChanged += r;
            rbVidRtv.CheckedChanged += r;
            rbVidRov.CheckedChanged += r;
            rbApiDx.CheckedChanged += r;
            rbApiVk.CheckedChanged += r;
            numAncho.ValueChanged += r;
            numAlto.ValueChanged += r;
            numFps.ValueChanged += r;
            numNitidez.ValueChanged += r;

            AplicarTema(this);

            Resize += delegate { AjustarLayout(); };
            AjustarLayout();
        }

        // ---------------------------------------------------------------------
        //  Banner and settings panel share the window width
        //
        //  The banner wants its traditional AnchoBanda (380) px, but in a
        //  narrow window that leaves the settings panel with less than AnchoCol
        //  and a horizontal scroll appears in addition to the vertical one
        //  -uncomfortable, and exactly what "work on all kinds of screens" asks
        //  to avoid.
        //
        //  So the banner gives way: the window's leftover width is calculated
        //  after giving the panel its minimum width (AnchoCol + 24, the same as
        //  AutoScrollMinSize) and the banner keeps that, between 0 and
        //  AnchoBanda. It is still visible -"keeps the side banner"- at any
        //  size equal to or greater than MinimumSize; it just narrows.
        //
        //  AND THE OTHER WAY AROUND -window WIDER than the content needs-
        //  panelColumna (the natural width, AnchoCol+40) is CENTERED in the
        //  excess instead of staying stuck to the left with a huge gap on the
        //  right: it is the other half of "distribute the space better". The
        //  banner does not widen beyond AnchoBanda -there is no more cover to
        //  show-, so that excess is all for centering the column.
        //
        //  It is called once at build time and on every Resize: that is why
        //  the banner and panelContenido do NOT have a width Anchor (Left+Right
        //  competing with this would cause stutter), only Top+Bottom for
        //  height.
        // ---------------------------------------------------------------------
        private void AjustarLayout()
        {
            if (banda == null || panelContenido == null || panelColumna == null)
                return;

            // + the width of the vertical scrollbar: with AltoUtil (1066) there
            // is almost always vertical scroll, and that bar really eats into
            // the panel's width. Without this margin, the calculation fit
            // exactly WITHOUT the bar, the bar appeared, and those ~17px it
            // stole also pushed a horizontal scroll -exactly the problem this
            // method exists to avoid.
            int contenidoMinimo = panelColumna.Width + SystemInformation.VerticalScrollBarWidth;
            int anchoBandaReal = Math.Max(0, Math.Min(AnchoBanda, ClientSize.Width - contenidoMinimo));

            // Explicit height for both, always the current window height:
            // that is what prevents the banner from staying taller than the
            // settings panel -and the cover having to fill that excess by
            // repeating itself- if something leaves the height out of sync
            // between the two.
            banda.Size = new Size(anchoBandaReal, ClientSize.Height);
            panelContenido.Location = new Point(anchoBandaReal, 0);
            panelContenido.Size = new Size(ClientSize.Width - anchoBandaReal, ClientSize.Height);

            int sobra = panelContenido.ClientSize.Width - panelColumna.Width;
            panelColumna.Location = new Point(Math.Max(0, sobra / 2), 0);
        }

        // ---------------------------------------------------------------------
        //  Spreads the dark theme across the whole control tree
        //
        //  Simpler and harder to forget than coloring each control where it is
        //  created: a new control added to Construir() gets themed without
        //  having to remember.
        //
        //  Controls with DYNAMIC color -lblParche, lblEstado, lblEscala, which
        //  change color in Refrescar/EstadoInicial/AlTerminar depending on the
        //  state- use the Tema tones directly in those places, not this step:
        //  this one only runs once, when building the window.
        // ---------------------------------------------------------------------
        private static void AplicarTema(Control raiz)
        {
            foreach (Control c in raiz.Controls)
            {
                if (c is PanelSeccion)
                {
                    // It already paints itself in its own OnPaint.
                }
                else if (c is PanelPortada)
                {
                    // The cover keeps its traditional black.
                }
                else if (c is Panel)
                {
                    c.BackColor = Tema.Fondo;
                }
                else if (c is TextBox)
                {
                    TextBox t = (TextBox)c;
                    t.BackColor = Tema.FondoCampo;
                    t.ForeColor = Tema.Texto;
                    t.BorderStyle = BorderStyle.FixedSingle;
                }
                else if (c is ComboBox)
                {
                    ComboBox cb = (ComboBox)c;
                    cb.BackColor = Tema.FondoCampo;
                    cb.ForeColor = Tema.Texto;
                    cb.FlatStyle = FlatStyle.Flat;
                }
                else if (c is Button)
                {
                    Button b = (Button)c;
                    b.BackColor = Tema.FondoPanel;
                    b.ForeColor = Tema.TextoTitulo;
                    b.FlatStyle = FlatStyle.Flat;
                    b.FlatAppearance.BorderColor = Tema.Borde;
                    b.FlatAppearance.MouseOverBackColor = Tema.BordeSuave;
                }
                else if (c is Label)
                {
                    if (c.BackColor != Tema.FondoCampo)
                        c.BackColor = Color.Transparent;
                }

                if (c.HasChildren)
                    AplicarTema(c);
            }
        }

        // ---- Control factories, to avoid repeating six lines each time --------
        // Three forms, all fall into the four-argument one: X0 and AnchoCol
        // (left column, one column wide) by default, so the calls that already
        // existed do not have to be touched.
        private PanelSeccion Grupo(string texto, int y, int alto)
        {
            return Grupo(texto, X0, y, alto, AnchoCol);
        }

        private PanelSeccion Grupo(string texto, int x, int y, int alto)
        {
            return Grupo(texto, x, y, alto, AnchoCol);
        }

        private PanelSeccion Grupo(string texto, int x, int y, int alto, int ancho)
        {
            PanelSeccion g = new PanelSeccion(texto);
            g.Location = new Point(x, y);
            g.Size = new Size(ancho, alto);
            panelColumna.Controls.Add(g);
            return g;
        }

        private static Label Etiqueta(string texto, int x, int y, int ancho)
        {
            Label l = new Label();
            l.Text = texto;
            l.Location = new Point(x, y);
            l.Size = new Size(ancho, 20);
            l.ForeColor = Tema.Texto;
            l.BackColor = Color.Transparent;
            return l;
        }

        private static Label Nota(int x, int y, int ancho, int alto, string texto)
        {
            Label l = new Label();
            l.Text = texto;
            l.Location = new Point(x, y);
            l.Size = new Size(ancho, alto);
            l.ForeColor = Tema.TextoNota;
            l.BackColor = Color.Transparent;
            return l;
        }

        private static RadioButton Radio(string texto, int x, int y, int ancho)
        {
            RadioButton b = new RadioButton();
            b.Text = texto;
            b.Location = new Point(x, y);
            b.Size = new Size(ancho, 22);
            b.ForeColor = Tema.Texto;
            b.BackColor = Color.Transparent;
            return b;
        }

        private static CheckBox Marca(string texto, int x, int y, int ancho)
        {
            CheckBox c = new CheckBox();
            c.Text = texto;
            c.Location = new Point(x, y);
            c.Size = new Size(ancho, 22);
            c.ForeColor = Tema.Texto;
            c.BackColor = Color.Transparent;
            return c;
        }

        private static NumericUpDown Numero(int x, int y, int ancho, int min, int max)
        {
            NumericUpDown n = new NumericUpDown();
            n.Location = new Point(x, y);
            n.Size = new Size(ancho, 23);
            n.Minimum = min;
            n.Maximum = max;
            n.Increment = 1;
            n.BackColor = Tema.FondoCampo;
            n.ForeColor = Tema.Texto;
            n.BorderStyle = BorderStyle.FixedSingle;
            return n;
        }

        // ---------------------------------------------------------------------
        //  Settings: the same file and the same names as lanzador.ps1
        // ---------------------------------------------------------------------
        private void CargarAjustes()
        {
            Dictionary<string, string> a = new Dictionary<string, string>();
            try
            {
                if (File.Exists(ficheroAjustes))
                    a = Json.Leer(File.ReadAllText(ficheroAjustes, Encoding.UTF8));
            }
            catch
            {
                // A broken json cannot prevent the launcher from opening.
            }

            txtIso.Text = Cadena(a, "iso", "");

            // Default 1080p + x2, not 720p + x1: it is the same as nfsmw.toml
            // already ships (video_mode 1920x1080, resolution_scale 2
            // -"recommended" according to its own comment-), so someone opening
            // the launcher for the first time, without a lanzador.json yet, sees
            // the SAME quality they would get starting nfsmw.exe directly.
            // Without this the launcher dropped the real resolution from 1080p
            // to 720p without anyone asking, just for not matching the toml.
            int i = IndiceDe(cboRes, Cadena(a, "preset", "1080p - 1920 x 1080"));
            cboRes.SelectedIndex = i >= 0 ? i : 4;

            numAncho.Value = Acotar(numAncho, Entero(a, "ancho", 1920));
            numAlto.Value = Acotar(numAlto, Entero(a, "alto", 1080));

            int e = IndiceDe(cboEsc, Cadena(a, "escala", "x2  - 4 times the pixels"));
            cboEsc.SelectedIndex = e >= 0 ? e : 1;

            int mo = IndiceDe(cboMon, Cadena(a, "monitor", "Automatic (default)"));
            cboMon.SelectedIndex = mo >= 0 ? mo : 0;

            int aa = IndiceDe(cboAA, Cadena(a, "antialiasing", "Off"));
            cboAA.SelectedIndex = aa >= 0 ? aa : 0;

            int an = IndiceDe(cboAniso, Cadena(a, "anisotropico", "8x"));
            cboAniso.SelectedIndex = an >= 0 ? an : 4;

            int ef = IndiceDe(cboEfecto, Cadena(a, "efecto", "None (bilinear)"));
            cboEfecto.SelectedIndex = ef >= 0 ? ef : 0;

            numNitidez.Value = Acotar(numNitidez, Entero(a, "nitidez", 50));

            bool completa = Booleano(a, "pantalla", true);
            rbCompleta.Checked = completa;
            rbVentana.Checked = !completa;

            chkVsync.Checked = Booleano(a, "vsync", false);
            chkLimite.Checked = Booleano(a, "limitar", false);
            numFps.Value = Acotar(numFps, Entero(a, "fps", 60));

            string v = Cadena(a, "video", "auto");
            rbVidRtv.Checked = v == "rtv";
            rbVidRov.Checked = v == "rov";
            rbVidAuto.Checked = !(rbVidRtv.Checked || rbVidRov.Checked);

            bool vulkan = Cadena(a, "api", "d3d12") == "vulkan";
            rbApiVk.Checked = vulkan;
            rbApiDx.Checked = !vulkan;
        }

        private void GuardarAjustes()
        {
            try
            {
                string dir = Path.GetDirectoryName(ficheroAjustes);
                if (!Directory.Exists(dir))
                    Directory.CreateDirectory(dir);

                StringBuilder sb = new StringBuilder();
                sb.AppendLine("{");
                sb.AppendLine("  \"iso\":  \"" + Json.Escapar(txtIso.Text) + "\",");
                sb.AppendLine("  \"preset\":  \"" + Json.Escapar(TextoDe(cboRes)) + "\",");
                sb.AppendLine("  \"ancho\":  " + ((int)numAncho.Value) + ",");
                sb.AppendLine("  \"alto\":  " + ((int)numAlto.Value) + ",");
                sb.AppendLine("  \"escala\":  \"" + Json.Escapar(TextoDe(cboEsc)) + "\",");
                sb.AppendLine("  \"monitor\":  \"" + Json.Escapar(TextoDe(cboMon)) + "\",");
                sb.AppendLine("  \"pantalla\":  " + (rbCompleta.Checked ? "true" : "false") + ",");
                sb.AppendLine("  \"vsync\":  " + (chkVsync.Checked ? "true" : "false") + ",");
                sb.AppendLine("  \"limitar\":  " + (chkLimite.Checked ? "true" : "false") + ",");
                sb.AppendLine("  \"fps\":  " + ((int)numFps.Value) + ",");
                sb.AppendLine("  \"antialiasing\":  \"" + Json.Escapar(TextoDe(cboAA)) + "\",");
                sb.AppendLine("  \"anisotropico\":  \"" + Json.Escapar(TextoDe(cboAniso)) + "\",");
                sb.AppendLine("  \"efecto\":  \"" + Json.Escapar(TextoDe(cboEfecto)) + "\",");
                sb.AppendLine("  \"nitidez\":  " + ((int)numNitidez.Value) + ",");
                sb.AppendLine("  \"video\":  \"" + VideoElegido() + "\",");
                sb.AppendLine("  \"api\":  \"" + ApiElegida() + "\"");
                sb.Append("}");
                File.WriteAllText(ficheroAjustes, sb.ToString(), new UTF8Encoding(false));
            }
            catch
            {
                // Saving preferences is a luxury, not a condition for playing.
            }
        }

        private static string Cadena(Dictionary<string, string> a, string k, string porDefecto)
        {
            string v;
            if (a.TryGetValue(k, out v) && v != null && v.Length > 0 && v != "null")
                return v;
            return porDefecto;
        }

        private static int Entero(Dictionary<string, string> a, string k, int porDefecto)
        {
            string v;
            int n;
            if (a.TryGetValue(k, out v) && int.TryParse(v, NumberStyles.Integer,
                                                        CultureInfo.InvariantCulture, out n))
                return n;
            return porDefecto;
        }

        private static bool Booleano(Dictionary<string, string> a, string k, bool porDefecto)
        {
            string v;
            if (a.TryGetValue(k, out v))
            {
                if (v == "true" || v == "True" || v == "1") return true;
                if (v == "false" || v == "False" || v == "0") return false;
            }
            return porDefecto;
        }

        private static decimal Acotar(NumericUpDown n, int v)
        {
            if (v < n.Minimum) return n.Minimum;
            if (v > n.Maximum) return n.Maximum;
            return v;
        }

        private static int IndiceDe(ComboBox c, string texto)
        {
            for (int i = 0; i < c.Items.Count; i++)
                if ((string)c.Items[i] == texto)
                    return i;
            return -1;
        }

        private static string TextoDe(ComboBox c)
        {
            return c.SelectedItem == null ? "" : (string)c.SelectedItem;
        }

        // ---------------------------------------------------------------------
        //  The command line
        // ---------------------------------------------------------------------
        private string SalidaElegida()
        {
            int i = cboRes.SelectedIndex;
            if (i < 0)
                return "720p";
            string v = Presets[i, 1];
            if (v == "custom")
                return string.Format(CultureInfo.InvariantCulture, "{0}x{1}",
                                     (int)numAncho.Value, (int)numAlto.Value);
            return v;
        }

        private int EscalaElegida()
        {
            int i = cboEsc.SelectedIndex;
            if (i < 0)
                return 1;
            return int.Parse(Escalas[i, 1], CultureInfo.InvariantCulture);
        }

        private string VideoElegido()
        {
            if (rbVidRtv.Checked) return "rtv";
            if (rbVidRov.Checked) return "rov";
            return "auto";
        }

        private string ApiElegida()
        {
            return rbApiVk.Checked ? "vulkan" : "d3d12";
        }

        private string AAElegida()
        {
            int i = cboAA.SelectedIndex;
            if (i < 0)
                return "none";
            return Antialias[i, 1];
        }

        private int AnisotropicoElegido()
        {
            int i = cboAniso.SelectedIndex;
            if (i < 0)
                return 4;
            return int.Parse(Anisotropico[i, 1], CultureInfo.InvariantCulture);
        }

        private string EfectoElegido()
        {
            int i = cboEfecto.SelectedIndex;
            if (i < 0)
                return "bilinear";
            return Efectos[i, 1];
        }

        private decimal NitidezElegida()
        {
            return ((decimal)numNitidez.Value) / 100m;
        }

        private int MonitorElegido()
        {
            int i = cboMon.SelectedIndex;
            if (i <= 0 || i > Screen.AllScreens.Length)
                return 0;
            return i;
        }

        // rutaJuego is the FOLDER passed as --game_data_root: either the one
        // the user chose directly (already extracted .xex format), or the
        // cache where ResolverRutaJuego left the extracted ISO. Never a bare
        // .iso: the SDK requires a directory (rex_app.cpp validates with
        // std::filesystem::is_directory) and does not know how to mount images.
        private string ConstruirArgumentos(string rutaJuego)
        {
            List<string> a = new List<string>();
            a.Add("--log_level info");
            a.Add("--log_file \"" + logEjecucion + "\"");
            a.Add("--game_data_root \"" + rutaJuego + "\"");
            a.Add("--gpu_plugin xenos");
            a.Add("--mnk_mode");

            // Fixed, and not a preference: without this the image comes out
            // washed out and the sun blown out.
            a.Add("--readback_resolve=fast");

            // ALWAYS, even if it matches what nfsmw.toml already says.
            //
            // In the SDK's cvar priority order the command line overrides the
            // config file:
            //
            //     kDefault < kConfig < kEnvironment < kCommandLine < kRuntime
            //
            // gpu_backend can also be changed from the F4 menu, and that is
            // where the danger is: if you pick an API that gives a black screen
            // on your machine, save and restart, the value stays written in the
            // toml and there is no way back -to change it you need the menu, and
            // to reach the menu you need to see something-. It really happened.
            //
            // By always passing it from here, this window beats the toml and
            // that cannot happen. That is also why there is no "automatic"
            // option in the API group: an automatic that passed nothing would
            // hand control back to the toml, which is exactly the hole.
            a.Add("--gpu_backend=" + ApiElegida());

            a.Add("--resolution " + SalidaElegida());

            int esc = EscalaElegida();
            if (esc > 1)
                a.Add("--resolution_scale " + esc);

            // Antialiasing: ALWAYS passed, like the API. So choosing "Off"
            // here beats whatever nfsmw.toml says, instead of handing control
            // back to the file.
            a.Add("--swap_post_effect=" + AAElegida());

            // Image quality: aniso and sharpness always (this window rules);
            // the presentation effect only when it is not the default one.
            a.Add("--anisotropic_override " + AnisotropicoElegido());
            if (EfectoElegido() != "bilinear")
                a.Add("--present_effect=" + EfectoElegido());
            a.Add("--present_cas_additional_sharpness " +
                  string.Format(CultureInfo.InvariantCulture, "{0:0.##}", NitidezElegida()));

            a.Add(rbCompleta.Checked ? "--fullscreen=true" : "--fullscreen=false");
            a.Add("--monitor " + MonitorElegido());
            a.Add(chkVsync.Checked ? "--vsync=true" : "--vsync=false");
            if (chkLimite.Checked)
                a.Add("--max_fps " + ((int)numFps.Value));

            // These two only if chosen by hand. In automatic nothing is passed
            // and the toml rules, which ships "rtv". Opposite to the API: here
            // choosing wrong does not make the game invisible, only slower or
            // with an odd stripe, so letting the file rule is not dangerous.
            if (rbVidRtv.Checked) a.Add("--render_target_path_d3d12=rtv");
            if (rbVidRov.Checked) a.Add("--render_target_path_d3d12=rov");

            return string.Join(" ", a.ToArray());
        }

        private void Refrescar()
        {
            if (cargando)
                return;

            bool esCustom = cboRes.SelectedIndex >= 0 &&
                            Presets[cboRes.SelectedIndex, 1] == "custom";
            numAncho.Enabled = esCustom;
            numAlto.Enabled = esCustom;
            numFps.Enabled = chkLimite.Checked;

            // Make it visible, BEFORE starting, that the scale does something.
            // Without this the only place where x1 and x2 differ is the command
            // line below, which almost nobody reads.
            //
            // The resolution in pixels is deliberately not shown: the scale does
            // NOT multiply the window size, it multiplies the game's render
            // targets, which have a size of their own that is not known from
            // here. Writing "2560 x 1440" would be making it up.
            int esc = EscalaElegida();
            if (esc <= 1)
            {
                lblEscala.ForeColor = Tema.TextoNota;
                lblEscala.Text = "The game draws at its original Xbox 360 resolution.";
            }
            else
            {
                lblEscala.ForeColor = Tema.Acento;
                lblEscala.Text = string.Format(
                    "The game draws {0} times wider and taller: {1} times the pixels.\n" +
                    "It looks finer, and the GPU works {1} times harder.", esc, esc * esc);
            }

            // Preview: uses exactly what is written in the ISO box, even if it
            // is a .iso. The real extraction (if needed) only happens when
            // pressing PLAY, in ResolverRutaJuego -doing it here, on every
            // keystroke, would be extremely expensive.
            string vista = txtIso.Text.Length > 0 ? txtIso.Text : "(none selected)";
            txtCmd.Text = Path.GetFileName(exeJuego) + " " + ConstruirArgumentos(vista);
            if (vista.Length > 0 && vista.EndsWith(".iso", StringComparison.OrdinalIgnoreCase))
            {
                txtCmd.Text += "\r\n(the ISO is extracted to game_root_cache\\ the first time PLAY " +
                               "is pressed; after that the copy is used)";
            }
        }

        // ---------------------------------------------------------------------
        //  Initial state: auto-found ISO and patch warning
        // ---------------------------------------------------------------------
        private void EstadoInicial()
        {
            if (txtIso.Text.Length == 0)
            {
                try
                {
                    string[] isos = Directory.GetFiles(raiz, "*.iso", SearchOption.TopDirectoryOnly);
                    if (isos.Length > 0)
                    {
                        txtIso.Text = isos[0];
                    }
                    else
                    {
                        // No ISO beside it: an already extracted "game_root"
                        // folder also works (same criterion as OnConfigurePaths
                        // in nfsmw_app.h).
                        string carpetaGr = Path.Combine(raiz, "game_root");
                        if (Directory.Exists(carpetaGr) &&
                            File.Exists(Path.Combine(carpetaGr, "default.xex")))
                        {
                            txtIso.Text = carpetaGr;
                        }
                    }
                }
                catch
                {
                }
            }

            // The SDK SOURCE is checked, not the DLL: that is where the truth
            // lives and it is cheap to check.
            //
            // In the distribution folder there is no source to check, but it
            // does not doubt either: that folder is built from an already
            // patched tree.
            bool? parche = null;
            if (distribuida)
            {
                parche = true;
            }
            else if (fuentePresentador != null && File.Exists(fuentePresentador))
            {
                try
                {
                    parche = File.ReadAllText(fuentePresentador)
                                 .Contains("PARCHE LOCAL - vsync real y limitador de fps");
                }
                catch
                {
                }
            }

            if (parche == false)
            {
                lblParche.Text = "WARNING: vsync and the fps limit will NOT do anything yet. Out of " +
                                 "the box the SDK does not sync and does not ship a limiter. Apply " +
                                 "tools\\parche_presentador.py and rebuild the SDK.";
            }
            else if (parche == null)
            {
                lblParche.ForeColor = Tema.TextoNota;
                lblParche.Text = "I cannot find the SDK source, so I do not know whether the " +
                                 "vsync patch is applied.";
            }

            if (!File.Exists(exeJuego))
            {
                lblEstado.Text = "Warning: there is no compiled executable yet.";
                lblEstado.ForeColor = Tema.Aviso;
            }
        }

        private void ElegirIso(object s, EventArgs e)
        {
            using (OpenFileDialog d = new OpenFileDialog())
            {
                d.Filter = "Disk image (*.iso)|*.iso|All files (*.*)|*.*";
                d.Title = "Choose the Need for Speed: Most Wanted ISO";
                try
                {
                    if (txtIso.Text.Length > 0 && File.Exists(txtIso.Text))
                        d.InitialDirectory = Path.GetDirectoryName(txtIso.Text);
                    else
                        d.InitialDirectory = raiz;
                }
                catch
                {
                }
                if (d.ShowDialog(this) == DialogResult.OK)
                {
                    txtIso.Text = d.FileName;
                    Refrescar();
                }
            }
        }

        private void ElegirCarpeta(object s, EventArgs e)
        {
            using (FolderBrowserDialog d = new FolderBrowserDialog())
            {
                d.Description =
                    "Choose the folder with the already extracted game (it must contain default.xex)";
                try
                {
                    if (txtIso.Text.Length > 0 && Directory.Exists(txtIso.Text))
                        d.SelectedPath = txtIso.Text;
                    else
                        d.SelectedPath = raiz;
                }
                catch
                {
                }

                if (d.ShowDialog(this) == DialogResult.OK)
                {
                    txtIso.Text = d.SelectedPath;
                    Refrescar();
                }
            }
        }

        // ---------------------------------------------------------------------
        //  From what the user chose to the folder the SDK needs
        //
        //  If it is already a folder (extracted .xex format), it is used as-is.
        //  If it is a .iso, it must be extracted first: rex_app.cpp requires
        //  --game_data_root to be a real directory and in all of rexglue-sdk
        //  there is no .iso reader (checked by hand: zero references to XDVDFS
        //  or to mounting images). Without this step, passing the ISO as-is
        //  produces exactly "--game_data_root does not exist: ...iso".
        //
        //  The full extraction is several GB and takes minutes, so it is only
        //  repeated if the ISO changed: CacheValida compares path and size
        //  against the marker EscribirMarcador leaves from the previous time.
        // ---------------------------------------------------------------------
        private string ResolverRutaJuego(string entrada, bool esCarpeta)
        {
            if (esCarpeta)
                return entrada;

            string carpetaCache = Path.Combine(raiz, "game_root_cache",
                ExtractorXdvdfs.SanearNombre(Path.GetFileNameWithoutExtension(entrada)));

            if (ExtractorXdvdfs.CacheValida(carpetaCache, entrada))
                return carpetaCache;

            using (VentanaExtraccion ve = new VentanaExtraccion(entrada, carpetaCache))
            {
                DialogResult r = ve.ShowDialog(this);
                if (r != DialogResult.OK)
                {
                    if (ve.Error != null)
                    {
                        MessageBox.Show(this,
                            "The ISO could not be extracted:\n\n" + ve.Error.Message,
                            "Extraction error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    }
                    return null;
                }
            }
            return carpetaCache;
        }

        // ---------------------------------------------------------------------
        //  Play
        //
        //  The game is waited for ON ANOTHER THREAD. The PowerShell launcher did
        //  WaitForExit on the window thread, and while you played the window
        //  hung -Windows painted it white and marked it as "not responding"-.
        //  Here it is launched separately and control returns to the window with
        //  Invoke when it finishes.
        // ---------------------------------------------------------------------
        private void Jugar(object s, EventArgs e)
        {
            if (!File.Exists(exeJuego))
            {
                MessageBox.Show(this,
                    "I cannot find the executable:\n\n" + exeJuego + "\n\nBuild it first.",
                    "Missing executable", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            string entrada = txtIso.Text;
            bool esIso = entrada.Length > 0 && File.Exists(entrada) &&
                        entrada.EndsWith(".iso", StringComparison.OrdinalIgnoreCase);
            bool esCarpeta = entrada.Length > 0 && Directory.Exists(entrada);
            if (!esIso && !esCarpeta)
            {
                MessageBox.Show(this,
                    "Choose an ISO or a folder with the already extracted game (.xex format) that exists.",
                    "Missing game", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            // Save BEFORE launching: if the game crashes, the preferences stay
            // saved anyway.
            GuardarAjustes();

            // If it is an ISO, ResolverRutaJuego extracts it to
            // game_root_cache\ (or reuses the previous extraction if it is still
            // the same ISO) and returns that folder. Null means the user
            // cancelled the extraction or it failed -and a warning was already
            // shown-, so nothing gets launched.
            string rutaJuego = ResolverRutaJuego(entrada, esCarpeta);
            if (rutaJuego == null)
                return;

            try
            {
                if (!Directory.Exists(dirLogs))
                    Directory.CreateDirectory(dirLogs);
            }
            catch
            {
            }

            btnJugar.Enabled = false;
            lblEstado.ForeColor = Tema.TextoNota;
            lblEstado.Text = "Playing... (F3 to see fps)";

            string argumentos = ConstruirArgumentos(rutaJuego);
            Thread hilo = new Thread(delegate ()
            {
                int codigo = 0;
                try
                {
                    ProcessStartInfo psi = new ProcessStartInfo(exeJuego, argumentos);
                    psi.WorkingDirectory = Path.GetDirectoryName(exeJuego);
                    psi.UseShellExecute = false;
                    using (Process p = Process.Start(psi))
                    {
                        if (p == null)
                            throw new InvalidOperationException(
                                "Windows did not manage to create the process.");

                        // Scheduling priority higher than Normal. The audio
                        // thread and the GPU command thread suffer the most if
                        // Windows takes CPU away from them to make room for
                        // something else -it is literally the audio "whine"
                        // symptom that unsticking fixed-, and on a machine with
                        // the CPU busy (Discord, the browser, an antivirus
                        // scanning) scheduling earlier helps without touching a
                        // single pixel of what is drawn.
                        //
                        // High and not RealTime: RealTime can starve Windows
                        // itself -mouse and keyboard included- if the game gets
                        // stuck in a tight loop, which is exactly the kind of
                        // hang this project already watches for elsewhere (see
                        // ArrancarVigilante in nfsmw_app.h). If it fails
                        // -permissions, or the process already ended- it is not
                        // a reason to refuse to play: it would stay on Normal.
                        try
                        {
                            p.PriorityClass = ProcessPriorityClass.High;
                        }
                        catch
                        {
                        }

                        p.WaitForExit();
                        codigo = p.ExitCode;
                    }
                }
                catch (Exception ex)
                {
                    string mensaje = ex.Message;
                    EnLaVentana(delegate
                    {
                        MessageBox.Show(this, "Could not launch:\n\n" + mensaje, "Error",
                                        MessageBoxButtons.OK, MessageBoxIcon.Error);
                        btnJugar.Enabled = true;
                        lblEstado.Text = "";
                    });
                    return;
                }

                int cod = codigo;
                EnLaVentana(delegate { AlTerminar(cod); });
            });
            hilo.IsBackground = true;
            hilo.Start();
        }

        // Return to the window thread from the thread waiting for the game.
        //
        // With the check up front on purpose: if you close the launcher while
        // playing, when the game ends there is no window to return to anymore,
        // and Invoke on a destroyed form crashes with an unhandled exception and
        // a .NET error window. Having the launcher crash AFTER you closed it is
        // especially absurd.
        private void EnLaVentana(MethodInvoker que)
        {
            try
            {
                if (IsDisposed || !IsHandleCreated)
                    return;
                Invoke(que);
            }
            catch (ObjectDisposedException)
            {
                // It was closed between the check and the Invoke. Nothing to do.
            }
            catch (InvalidOperationException)
            {
                // Same: the handle was destroyed along the way.
            }
        }

        private void AlTerminar(int codigo)
        {
            btnJugar.Enabled = true;
            lblEstado.Text = "";

            // If a scale was requested and the GPU could not handle it, the SDK
            // lowers it on its own and writes it to the log.
            string bajada = BuscarEnLog(new string[] { "draw resolution scale is not supported" },
                                        true);
            if (bajada != null)
            {
                MessageBox.Show(this,
                    "The render scale you requested is not supported by your machine, so the SDK " +
                    "lowered it on its own:\n\n" + bajada,
                    "Reduced scale", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }

            if (codigo != 0)
            {
                string pistas = BuscarEnLog(new string[] { "[critical]", "FATAL", "unregistered" },
                                            false);
                MessageBox.Show(this,
                    string.Format("The game exited with code {0}.{1}\n\nLog: {2}",
                                  codigo, pistas == null ? "" : "\n\n" + pistas, logEjecucion),
                    "Exited with error", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        // Returns the first line containing any of the needles, or the last
        // eight joined if soloLaPrimera is false. Null if there are none.
        private string BuscarEnLog(string[] agujas, bool soloLaPrimera)
        {
            try
            {
                if (!File.Exists(logEjecucion))
                    return null;

                List<string> encontradas = new List<string>();
                using (StreamReader r = new StreamReader(logEjecucion))
                {
                    string linea;
                    while ((linea = r.ReadLine()) != null)
                    {
                        foreach (string aguja in agujas)
                        {
                            if (linea.IndexOf(aguja, StringComparison.Ordinal) >= 0)
                            {
                                if (soloLaPrimera)
                                    return linea;
                                encontradas.Add(linea);
                                break;
                            }
                        }
                    }
                }

                if (encontradas.Count == 0)
                    return null;
                int desde = Math.Max(0, encontradas.Count - 8);
                return string.Join("\n", encontradas.GetRange(desde, encontradas.Count - desde)
                                                    .ToArray());
            }
            catch
            {
                return null;
            }
        }
    }

    // ===========================================================================
    //  XDVDFS extractor: from Xbox 360 .iso to folder, with no dependencies
    //
    //  C# port of tools\fase1_extraer.py ("2. Extract EVERYTHING" option). It
    //  exists because rexglue-sdk does not know how to read .iso images:
    //  Runtime/ReXApp require --game_data_root to already be a folder
    //  (rex_app.cpp, is_directory). This step is done here on its own, with no
    //  need to install Python separately -the distribution build cannot depend
    //  on that.
    //
    //  Format (XDVDFS, "MICROSOFT*XBOX*MEDIA"):
    //    - Volume descriptor 32 sectors from the partition base.
    //    - The base varies with the disc type (XGD1/2/3 or an already trimmed
    //      image); the known offsets are tried and, if none matches, the image
    //      is swept looking for the magic.
    //    - Each directory's tree is a flat binary tree: every entry carries
    //      left child, right child, sector, size, attributes and name. Child
    //      indexes are "logical sector / 4", not bytes.
    // ===========================================================================
    internal static class ExtractorXdvdfs
    {
        private const int Sector = 2048;
        private static readonly byte[] Magic = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");

        // 0 = raw partition / already trimmed image; the rest are XGD2, XGD3
        // and XGD1 (original Xbox), in that real-frequency order.
        private static readonly long[] BasesConocidas =
            { 0x00000000L, 0x0FD90000L, 0x02080000L, 0x18300000L };

        private sealed class Entrada
        {
            public string Nombre;
            public uint Sector;
            public uint Tam;
            public bool Dir;
        }

        public delegate void Progreso(string archivo, int hechos, int total);
        public delegate bool Cancelado();

        // -----------------------------------------------------------------
        //  XDVDFS
        // -----------------------------------------------------------------
        private static bool MagicEn(FileStream fh, long offset)
        {
            try
            {
                fh.Seek(offset + 32L * Sector, SeekOrigin.Begin);
                byte[] buf = new byte[Magic.Length];
                int leido = fh.Read(buf, 0, buf.Length);
                if (leido != buf.Length)
                    return false;
                for (int i = 0; i < buf.Length; i++)
                {
                    if (buf[i] != Magic[i])
                        return false;
                }
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static int Buscar(byte[] buf, int longitudValida, int desde)
        {
            if (desde < 0)
                desde = 0;
            int limite = longitudValida - Magic.Length;
            for (int i = desde; i <= limite; i++)
            {
                bool ok = true;
                for (int j = 0; j < Magic.Length; j++)
                {
                    if (buf[i + j] != Magic[j])
                    {
                        ok = false;
                        break;
                    }
                }
                if (ok)
                    return i;
            }
            return -1;
        }

        private static long DetectarBase(FileStream fh)
        {
            for (int i = 0; i < BasesConocidas.Length; i++)
            {
                if (MagicEn(fh, BasesConocidas[i]))
                    return BasesConocidas[i];
            }

            // None of the known offsets matches: brute-force sweep in 16 MB
            // chunks, with overlap so as not to miss the magic split between
            // two chunks.
            long tam = fh.Length;
            long tope = Math.Min(tam, 1L << 30);
            const int chunk = 16 << 20;
            int solapa = Magic.Length;
            byte[] buf = new byte[chunk + solapa];
            long pos = 0;
            while (pos < tope)
            {
                fh.Seek(pos, SeekOrigin.Begin);
                int leido = fh.Read(buf, 0, buf.Length);
                if (leido <= 0)
                    break;

                int idx = Buscar(buf, leido, 0);
                while (idx != -1)
                {
                    long absOff = pos + idx;
                    if (absOff % Sector == 0 && absOff >= 32L * Sector)
                    {
                        long candidata = absOff - 32L * Sector;
                        if (MagicEn(fh, candidata))
                            return candidata;
                    }
                    idx = Buscar(buf, leido, idx + 1);
                }
                pos += chunk;
            }

            throw new InvalidOperationException(
                "No XDVDFS file system was found in the image.\n" +
                "Check that it is an Xbox 360 ISO and not a compressed CCI/GOD/ZAR.");
        }

        private static void LeerDescriptor(FileStream fh, long baseP, out uint sectorRaiz,
                                           out uint tamRaiz)
        {
            fh.Seek(baseP + 32L * Sector, SeekOrigin.Begin);
            byte[] vd = new byte[Sector];
            int leido = fh.Read(vd, 0, vd.Length);
            if (leido < Sector || !IgualPrefijo(vd, Magic))
                throw new InvalidOperationException("Invalid volume descriptor.");

            sectorRaiz = BitConverter.ToUInt32(vd, 0x14);
            tamRaiz = BitConverter.ToUInt32(vd, 0x18);
        }

        private static bool IgualPrefijo(byte[] datos, byte[] patron)
        {
            if (datos.Length < patron.Length)
                return false;
            for (int i = 0; i < patron.Length; i++)
            {
                if (datos[i] != patron[i])
                    return false;
            }
            return true;
        }

        // Raw nodes of ONE directory's binary tree (without walking
        // subdirectories: Recorrer does that). offsetInicial is 0, the root of
        // this table's tree.
        private static List<KeyValuePair<string, Entrada>> Entradas(byte[] tabla)
        {
            List<KeyValuePair<string, Entrada>> resultado = new List<KeyValuePair<string, Entrada>>();
            Stack<int> pila = new Stack<int>();
            HashSet<int> vistos = new HashSet<int>();
            pila.Push(0);

            while (pila.Count > 0)
            {
                int off = pila.Pop();
                if (vistos.Contains(off))
                    continue;
                if (off + 14 > tabla.Length)
                    continue;
                vistos.Add(off);

                ushort izq = BitConverter.ToUInt16(tabla, off);
                ushort der = BitConverter.ToUInt16(tabla, off + 2);
                uint sector = BitConverter.ToUInt32(tabla, off + 4);
                uint tamEntrada = BitConverter.ToUInt32(tabla, off + 8);
                byte attrs = tabla[off + 12];
                byte largo = tabla[off + 13];

                // 0 and 0xFFFF mark "no child" (offset 0 is only valid for the
                // root, which was already processed on entry).
                if (izq != 0 && izq != 0xFFFF)
                    pila.Push(izq * 4);
                if (der != 0 && der != 0xFFFF)
                    pila.Push(der * 4);

                int finNombre = off + 14 + largo;
                if (largo == 0 || finNombre > tabla.Length)
                    continue;

                string nombre = Encoding.GetEncoding("ISO-8859-1").GetString(tabla, off + 14, largo);
                Entrada e = new Entrada();
                e.Nombre = nombre;
                e.Sector = sector;
                e.Tam = tamEntrada;
                e.Dir = (attrs & 0x10) != 0;
                resultado.Add(new KeyValuePair<string, Entrada>(nombre, e));
            }
            return resultado;
        }

        private static int CompararNombres(KeyValuePair<string, Entrada> a, KeyValuePair<string, Entrada> b)
        {
            return string.Compare(a.Key, b.Key, StringComparison.OrdinalIgnoreCase);
        }

        private static void Recorrer(FileStream fh, long baseP, uint sector, uint tam, string prefijo,
                                     List<KeyValuePair<string, Entrada>> salida)
        {
            if (tam == 0 || tam > (256 << 20))
                return;

            fh.Seek(baseP + (long)sector * Sector, SeekOrigin.Begin);
            byte[] tabla = new byte[tam];
            fh.Read(tabla, 0, tabla.Length);   // if it comes up short, it continues with what was read

            List<KeyValuePair<string, Entrada>> hijos = Entradas(tabla);
            hijos.Sort(CompararNombres);

            foreach (KeyValuePair<string, Entrada> par in hijos)
            {
                string ruta = prefijo.Length > 0 ? prefijo + "/" + par.Key : par.Key;
                salida.Add(new KeyValuePair<string, Entrada>(ruta, par.Value));
                if (par.Value.Dir)
                    Recorrer(fh, baseP, par.Value.Sector, par.Value.Tam, ruta, salida);
            }
        }

        private static void ExtraerArchivo(FileStream fh, long baseP, Entrada e, string destino)
        {
            string dir = Path.GetDirectoryName(destino);
            if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                Directory.CreateDirectory(dir);

            fh.Seek(baseP + (long)e.Sector * Sector, SeekOrigin.Begin);
            long restante = e.Tam;
            byte[] buf = new byte[1 << 20];
            using (FileStream salida = new FileStream(destino, FileMode.Create, FileAccess.Write))
            {
                while (restante > 0)
                {
                    int aLeer = (int)Math.Min(buf.Length, restante);
                    int leido = fh.Read(buf, 0, aLeer);
                    if (leido <= 0)
                    {
                        throw new InvalidOperationException(
                            "Unexpected end of file reading " + e.Nombre + ". Incomplete image?");
                    }
                    salida.Write(buf, 0, leido);
                    restante -= leido;
                }
            }
        }

        // Extracts ALL the content of the game partition to destino. The game
        // needs the full tree at runtime -not just default.xex-, because
        // game_data_root is mounted as D:\ itself.
        public static void Extraer(string isoPath, string destino, Progreso progreso, Cancelado cancelado)
        {
            using (FileStream fh = new FileStream(isoPath, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                long baseP = DetectarBase(fh);
                uint sectorRaiz, tamRaiz;
                LeerDescriptor(fh, baseP, out sectorRaiz, out tamRaiz);

                List<KeyValuePair<string, Entrada>> entradas = new List<KeyValuePair<string, Entrada>>();
                Recorrer(fh, baseP, sectorRaiz, tamRaiz, "", entradas);

                if (entradas.Count == 0)
                {
                    throw new InvalidOperationException(
                        "The file system is empty. Corrupted image?");
                }

                int total = 0;
                foreach (KeyValuePair<string, Entrada> par in entradas)
                {
                    if (!par.Value.Dir)
                        total++;
                }

                int hechos = 0;
                foreach (KeyValuePair<string, Entrada> par in entradas)
                {
                    if (cancelado != null && cancelado())
                        throw new OperationCanceledException();
                    if (par.Value.Dir)
                        continue;

                    string destinoArchivo = Path.Combine(destino,
                        par.Key.Replace('/', Path.DirectorySeparatorChar));
                    ExtraerArchivo(fh, baseP, par.Value, destinoArchivo);
                    hechos++;

                    if (progreso != null && (hechos % 10 == 0 || hechos == total))
                        progreso(par.Key, hechos, total);
                }

                if (!File.Exists(Path.Combine(destino, "default.xex")))
                {
                    throw new InvalidOperationException(
                        hechos + " files were extracted but default.xex did not show up " +
                        "at the root. The ISO may not be an Xbox 360 one, or it may not be " +
                        "Need for Speed: Most Wanted.");
                }
            }
        }

        // -----------------------------------------------------------------
        //  Cache: do not extract the same ISO again
        //
        //  The marker stores path+size of the source ISO. If they match and
        //  default.xex is still there, the cache is considered good. More
        //  precision is not needed -a hash of the whole file would be more
        //  reliable but requires reading the same GB that are meant to be
        //  avoided re-reading.
        // -----------------------------------------------------------------
        private static string RutaMarcador(string carpetaCache)
        {
            return Path.Combine(carpetaCache, ".origen_iso.txt");
        }

        public static bool CacheValida(string carpetaCache, string isoPath)
        {
            try
            {
                if (!File.Exists(Path.Combine(carpetaCache, "default.xex")))
                    return false;

                string marcador = RutaMarcador(carpetaCache);
                if (!File.Exists(marcador))
                    return false;

                string[] partes = File.ReadAllText(marcador, Encoding.UTF8).Split('|');
                if (partes.Length < 2)
                    return false;

                long tamGuardado;
                if (!long.TryParse(partes[1], out tamGuardado))
                    return false;

                FileInfo fi = new FileInfo(isoPath);
                return string.Equals(partes[0], Path.GetFullPath(isoPath),
                                     StringComparison.OrdinalIgnoreCase) &&
                       fi.Length == tamGuardado;
            }
            catch
            {
                return false;
            }
        }

        public static void EscribirMarcador(string carpetaCache, string isoPath)
        {
            try
            {
                FileInfo fi = new FileInfo(isoPath);
                File.WriteAllText(RutaMarcador(carpetaCache),
                    Path.GetFullPath(isoPath) + "|" + fi.Length, new UTF8Encoding(false));
            }
            catch
            {
                // If the marker cannot be written, next time it extracts again.
                // Slow, but it breaks nothing.
            }
        }

        public static string SanearNombre(string s)
        {
            StringBuilder sb = new StringBuilder();
            char[] invalidos = Path.GetInvalidFileNameChars();
            foreach (char c in s)
                sb.Append(Array.IndexOf(invalidos, c) >= 0 ? '_' : c);
            return sb.Length > 0 ? sb.ToString() : "iso";
        }
    }

    // ===========================================================================
    //  Modal window with the extraction progress
    //
    //  The extraction runs on a separate thread -same as the game in Jugar()-
    //  so the window does not stay "not responding" while several GB are
    //  copied. The result is read from DialogResult (OK / Cancel) and, if
    //  something failed, from the Error field.
    // ===========================================================================
    internal sealed class VentanaExtraccion : Form
    {
        private readonly string iso;
        private readonly string destino;
        private readonly Label lbl;
        private readonly ProgressBar barra;
        private readonly Button btnCancelar;
        private volatile bool cancelar;
        private Exception error;

        public Exception Error { get { return error; } }

        public VentanaExtraccion(string iso, string destino)
        {
            this.iso = iso;
            this.destino = destino;

            Text = "Extracting the ISO...";
            ClientSize = new Size(460, 122);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MaximizeBox = false;
            MinimizeBox = false;
            ControlBox = false;
            Font = new Font("Segoe UI", 8.25f);
            BackColor = Tema.Fondo;

            lbl = new Label();
            lbl.Location = new Point(16, 14);
            lbl.Size = new Size(428, 44);
            lbl.ForeColor = Tema.Texto;
            lbl.Text = "Extracting " + Path.GetFileName(iso) + "...\n" +
                      "Only needed the first time with this ISO; it can take several minutes.";
            Controls.Add(lbl);

            barra = new ProgressBar();
            barra.Location = new Point(16, 66);
            barra.Size = new Size(428, 20);
            barra.Style = ProgressBarStyle.Marquee;
            barra.MarqueeAnimationSpeed = 30;
            Controls.Add(barra);

            btnCancelar = new Button();
            btnCancelar.Text = "Cancel";
            btnCancelar.Location = new Point(360, 92);
            btnCancelar.Size = new Size(84, 24);
            btnCancelar.BackColor = Tema.FondoPanel;
            btnCancelar.ForeColor = Tema.TextoTitulo;
            btnCancelar.FlatStyle = FlatStyle.Flat;
            btnCancelar.FlatAppearance.BorderColor = Tema.Borde;
            btnCancelar.Click += delegate
            {
                cancelar = true;
                btnCancelar.Enabled = false;
                lbl.Text = "Cancelling...";
            };
            Controls.Add(btnCancelar);

            Load += VentanaExtraccion_Load;
        }

        private void VentanaExtraccion_Load(object s, EventArgs e)
        {
            Thread hilo = new Thread(delegate ()
            {
                try
                {
                    if (Directory.Exists(destino))
                    {
                        try { Directory.Delete(destino, true); }
                        catch { /* leftovers from a half-finished previous attempt; they get overwritten anyway */ }
                    }
                    Directory.CreateDirectory(destino);

                    ExtractorXdvdfs.Extraer(iso, destino,
                        delegate (string archivo, int hechos, int total)
                        {
                            ActualizarProgreso(archivo, hechos, total);
                        },
                        delegate { return cancelar; });

                    ExtractorXdvdfs.EscribirMarcador(destino, iso);

                    TerminarEn(delegate { DialogResult = DialogResult.OK; Close(); });
                }
                catch (OperationCanceledException)
                {
                    TerminarEn(delegate { DialogResult = DialogResult.Cancel; Close(); });
                }
                catch (Exception ex)
                {
                    error = ex;
                    TerminarEn(delegate { DialogResult = DialogResult.Cancel; Close(); });
                }
            });
            hilo.IsBackground = true;
            hilo.Start();
        }

        private void ActualizarProgreso(string archivo, int hechos, int total)
        {
            try
            {
                if (IsDisposed || !IsHandleCreated)
                    return;
                Invoke((MethodInvoker)delegate
                {
                    if (IsDisposed)
                        return;
                    if (barra.Style != ProgressBarStyle.Continuous && total > 0)
                    {
                        barra.Style = ProgressBarStyle.Continuous;
                        barra.Minimum = 0;
                        barra.Maximum = total;
                    }
                    if (total > 0)
                        barra.Value = Math.Min(hechos, total);
                    lbl.Text = string.Format("Extracting {0}/{1}: {2}", hechos, total, archivo);
                });
            }
            catch (ObjectDisposedException) { }
            catch (InvalidOperationException) { }
        }

        private void TerminarEn(MethodInvoker que)
        {
            try
            {
                if (IsDisposed || !IsHandleCreated)
                    return;
                Invoke(que);
            }
            catch (ObjectDisposedException) { }
            catch (InvalidOperationException) { }
        }
    }
}
