// nfsmw - textos de la interfaz en seis idiomas (ver idioma.h).

#include "idioma.h"

#include <rex/cvar.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
// Declarada a mano en vez de incluir windows.h, que define min/max como macros.
extern "C" __declspec(dllimport) unsigned short __stdcall GetUserDefaultUILanguage(void);
#endif

REXCVAR_DEFINE_STRING(nfsmw_idioma, "", "Interfaz",
                      "Idioma del lanzador y de los menus: es, en, pt, de, it, fr (vacio = el del sistema)")
    .allowed({"", "es", "en", "pt", "de", "it", "fr"});

namespace nfsmw::idioma {
namespace {

struct Texto {
  const char* clave;
  const char* t[kNumIdiomas];  // es, en, pt, de, it, fr
};

// clang-format off
constexpr Texto kTextos[] = {
  // ---- General ----------------------------------------------------------------
  {"app.subtitulo", {"Recompilación nativa para PC", "Native PC recompilation", "Recompilação nativa para PC", "Native Rekompilierung für PC", "Ricompilazione nativa per PC", "Recompilation native pour PC"}},
  {"app.subtitulo_android", {"Recompilación nativa para Android", "Native Android recompilation", "Recompilação nativa para Android", "Native Rekompilierung für Android", "Ricompilazione nativa per Android", "Recompilation native pour Android"}},
  {"nav.inicio", {"Inicio", "Home", "Início", "Start", "Home", "Accueil"}},
  {"nav.graficos", {"Gráficos", "Graphics", "Gráficos", "Grafik", "Grafica", "Graphismes"}},
  {"nav.controles", {"Controles", "Controls", "Controles", "Steuerung", "Controlli", "Commandes"}},
  {"nav.audio", {"Audio", "Audio", "Áudio", "Audio", "Audio", "Audio"}},
  {"nav.config", {"Configuración", "Settings", "Configurações", "Einstellungen", "Impostazioni", "Paramètres"}},
  {"nav.acerca", {"Acerca de", "About", "Sobre", "Über", "Informazioni", "À propos"}},
  {"btn.jugar", {"JUGAR", "PLAY", "JOGAR", "SPIELEN", "GIOCA", "JOUER"}},
  {"btn.reiniciar", {"APLICAR Y REINICIAR", "APPLY AND RESTART", "APLICAR E REINICIAR", "ÜBERNEHMEN UND NEU STARTEN", "APPLICA E RIAVVIA", "APPLIQUER ET REDÉMARRER"}},
  {"btn.salir", {"Salir", "Exit", "Sair", "Beenden", "Esci", "Quitter"}},
  {"btn.cancelar", {"Cancelar", "Cancel", "Cancelar", "Abbrechen", "Annulla", "Annuler"}},
  {"btn.iso", {"Elegir ISO…", "Choose ISO…", "Escolher ISO…", "ISO wählen…", "Scegli ISO…", "Choisir l'ISO…"}},
  {"btn.carpeta", {"Elegir carpeta…", "Choose folder…", "Escolher pasta…", "Ordner wählen…", "Scegli cartella…", "Choisir un dossier…"}},
  {"btn.exportar_log", {"Exportar log", "Export log", "Exportar log", "Log exportieren", "Esporta log", "Exporter le journal"}},
  {"btn.repo", {"Repositorio en GitHub", "GitHub repository", "Repositório no GitHub", "GitHub-Repository", "Repository su GitHub", "Dépôt GitHub"}},
  {"btn.logs", {"Carpeta de logs", "Logs folder", "Pasta de logs", "Log-Ordner", "Cartella dei log", "Dossier des journaux"}},
  {"btn.guardados", {"Carpeta de partidas", "Saves folder", "Pasta de saves", "Spielstand-Ordner", "Cartella dei salvataggi", "Dossier des sauvegardes"}},
  {"aviso.reinicio", {"Hay cambios que se aplican al reiniciar: el juego se reiniciará solo.",
                      "Some changes apply on restart: the game will restart by itself.",
                      "Algumas mudanças valem ao reiniciar: o jogo reiniciará sozinho.", "Einige Änderungen gelten erst nach einem Neustart: Das Spiel startet von selbst neu.", "Alcune modifiche si applicano al riavvio: il gioco si riavvierà da solo.", "Certains changements s'appliquent au redémarrage : le jeu redémarrera tout seul."}},
  {"aviso.sin_juego", {"Elige tu copia del juego para poder jugar.", "Choose your game copy to play.",
                       "Escolha sua cópia do jogo para jogar.", "Wähle deine Spielkopie, um zu spielen.", "Scegli la tua copia del gioco per giocare.", "Choisis ta copie du jeu pour jouer."}},

  // ---- Inicio -------------------------------------------------------------------
  {"juego.titulo", {"Copia del juego", "Game copy", "Cópia do jogo", "Spielkopie", "Copia del gioco", "Copie du jeu"}},
  {"juego.nota", {"Elige la ISO de tu juego o una carpeta ya extraída (con default.xex dentro). La primera vez "
                  "con una ISO se extrae una copia junto al juego; después arranca directo.",
                  "Choose your game ISO or an already extracted folder (with default.xex inside). The first time "
                  "with an ISO a copy is extracted next to the game; after that it starts right away.",
                  "Escolha a ISO do seu jogo ou uma pasta já extraída (com default.xex dentro). Na primeira vez "
                  "com uma ISO uma cópia é extraída ao lado do jogo; depois inicia direto.", "Wähle die ISO deines Spiels oder einen bereits entpackten Ordner (mit default.xex darin). Beim ersten Mal wird aus einer ISO eine Kopie neben dem Spiel entpackt; danach startet es direkt.", "Scegli la ISO del tuo gioco o una cartella già estratta (con default.xex dentro). La prima volta da una ISO viene estratta una copia accanto al gioco; dopo parte subito.", "Choisis l'ISO de ton jeu ou un dossier déjà extrait (avec default.xex dedans). La première fois, une copie de l'ISO est extraite à côté du jeu ; ensuite il démarre directement."}},
  {"juego.nota_directa", {"Elige la ISO de tu juego o una carpeta ya extraída (con default.xex dentro). La ISO se "
                          "juega tal cual, sin extraer nada.",
                          "Choose your game ISO or an already extracted folder (with default.xex inside). The ISO is "
                          "played as it is, nothing is extracted.",
                          "Escolha a ISO do seu jogo ou uma pasta já extraída (com default.xex dentro). A ISO é "
                          "jogada como está, sem extrair nada.", "Wähle die ISO deines Spiels oder einen bereits entpackten Ordner (mit default.xex darin). Die ISO wird direkt gespielt, nichts wird entpackt.", "Scegli la ISO del tuo gioco o una cartella già estratta (con default.xex dentro). La ISO si gioca così com'è, senza estrarre nulla.", "Choisis l'ISO de ton jeu ou un dossier déjà extrait (avec default.xex dedans). L'ISO se joue telle quelle, rien n'est extrait."}},
  {"juego.ninguno", {"Todavía no has elegido tu copia del juego.", "You have not chosen your game copy yet.",
                     "Você ainda não escolheu sua cópia do jogo.", "Du hast noch keine Spielkopie gewählt.", "Non hai ancora scelto la tua copia del gioco.", "Tu n'as pas encore choisi ta copie du jeu."}},
  {"estado.titulo", {"Estado", "Status", "Status", "Status", "Stato", "État"}},
  {"estado.juego", {"Juego", "Game", "Jogo", "Spiel", "Gioco", "Jeu"}},
  {"estado.config", {"Configuración", "Settings", "Configuração", "Einstellungen", "Impostazioni", "Paramètres"}},
  {"estado.iso_nueva", {"ISO: se extraerá al pulsar Jugar", "ISO: extracted when you press Play",
                        "ISO: será extraída ao clicar em Jogar", "ISO: wird beim Klick auf Spielen entpackt", "ISO: verrà estratta premendo Gioca", "ISO : sera extraite en appuyant sur Jouer"}},
  {"estado.iso_lista", {"ISO ya extraída, lista para jugar", "ISO already extracted, ready to play",
                        "ISO já extraída, pronta para jogar", "ISO bereits entpackt, spielbereit", "ISO già estratta, pronta per giocare", "ISO déjà extraite, prête à jouer"}},
  {"estado.iso_directa", {"ISO lista para jugar", "ISO ready to play", "ISO pronta para jogar", "ISO spielbereit", "ISO pronta per giocare", "ISO prête à jouer"}},
  {"estado.carpeta", {"Carpeta lista para jugar", "Folder ready to play", "Pasta pronta para jogar", "Ordner spielbereit", "Cartella pronta per giocare", "Dossier prêt à jouer"}},
  {"estado.invalida", {"Esa carpeta no contiene default.xex", "That folder does not contain default.xex",
                       "Essa pasta não contém default.xex", "Dieser Ordner enthält keine default.xex", "Quella cartella non contiene default.xex", "Ce dossier ne contient pas default.xex"}},
  {"estado.falta", {"No se encuentra el archivo elegido", "The chosen file cannot be found",
                    "O arquivo escolhido não foi encontrado", "Die gewählte Datei wurde nicht gefunden", "Il file scelto non è stato trovato", "Le fichier choisi est introuvable"}},
  {"inicio.atajos", {"Durante el juego: F3 muestra los FPS y F1 vuelve al lanzador.", "In game: F3 shows the FPS and F1 goes back to the launcher.", "No jogo: F3 mostra o FPS e F1 volta ao iniciador.", "Im Spiel: F3 zeigt die FPS, F1 kehrt zum Launcher zurück.", "In gioco: F3 mostra gli FPS, F1 torna al launcher.", "En jeu : F3 affiche les FPS, F1 revient au lanceur."}},
  {"extraer.titulo", {"Extrayendo la ISO…", "Extracting the ISO…", "Extraindo a ISO…", "ISO wird entpackt…", "Estrazione della ISO…", "Extraction de l'ISO…"}},
  {"extraer.nota", {"Solo hace falta la primera vez. Puede tardar varios minutos.",
                    "Only needed the first time. It may take several minutes.",
                    "Só é necessário na primeira vez. Pode levar vários minutos.", "Nur beim ersten Mal nötig. Das kann einige Minuten dauern.", "Serve solo la prima volta. Può richiedere alcuni minuti.", "Nécessaire seulement la première fois. Cela peut prendre plusieurs minutes."}},
  {"extraer.error", {"No se pudo extraer la ISO", "Could not extract the ISO", "Não foi possível extrair a ISO", "Die ISO konnte nicht entpackt werden", "Impossibile estrarre la ISO", "Impossible d'extraire l'ISO"}},

  // ---- Shaders y actualizaciones --------------------------------------------------
  {"estado.shaders", {"Shaders", "Shaders", "Shaders", "Shader", "Shader", "Shaders"}},
  {"shaders.listos", {"Generados", "Generated", "Gerados", "Erstellt", "Generati", "Générés"}},
  {"shaders.faltan", {"Se generarán al pulsar Jugar", "Generated when you press Play", "Serão gerados ao clicar em Jogar", "Werden beim Klick auf Spielen erstellt", "Verranno generati premendo Gioca", "Seront générés en appuyant sur Jouer"}},
  {"shaders.titulo", {"Generando los shaders…", "Generating the shaders…", "Gerando os shaders…", "Shader werden erstellt…", "Generazione degli shader…", "Génération des shaders…"}},
  {"shaders.nota", {"Se traducen los shaders de tu copia del juego para tu tarjeta gráfica. Solo hace falta una vez.",
                    "Your game copy's shaders are translated for your graphics card. Only needed once.",
                    "Os shaders da sua cópia do jogo são traduzidos para sua placa de vídeo. Só é necessário uma vez.", "Die Shader deiner Spielkopie werden für deine Grafikkarte übersetzt. Nur einmal nötig.", "Gli shader della tua copia del gioco vengono tradotti per la tua scheda video. Serve una sola volta.", "Les shaders de ta copie du jeu sont traduits pour ta carte graphique. Une seule fois suffit."}},
  {"shaders.error", {"No se pudieron generar los shaders", "Could not generate the shaders",
                     "Não foi possível gerar os shaders", "Die Shader konnten nicht erstellt werden", "Impossibile generare gli shader", "Impossible de générer les shaders"}},
  {"fase.imagen", {"Leyendo el ejecutable", "Reading the executable", "Lendo o executável", "Programmdatei wird gelesen", "Lettura dell'eseguibile", "Lecture de l'exécutable"}},
  {"fase.buscar", {"Buscando shaders en el disco", "Finding shaders on the disc", "Procurando shaders no disco", "Shader auf der Disc werden gesucht", "Ricerca degli shader sul disco", "Recherche des shaders sur le disque"}},
  {"fase.traducir", {"Traduciendo", "Translating", "Traduzindo", "Übersetzen", "Traduzione", "Traduction"}},
  {"fase.compilar", {"Compilando para Vulkan", "Compiling for Vulkan", "Compilando para Vulkan", "Für Vulkan kompilieren", "Compilazione per Vulkan", "Compilation pour Vulkan"}},
  {"fase.empaquetar", {"Guardando la biblioteca", "Saving the library", "Salvando a biblioteca", "Bibliothek wird gespeichert", "Salvataggio della libreria", "Enregistrement de la bibliothèque"}},
  {"btn.regenerar", {"Regenerar shaders", "Regenerate shaders", "Regenerar shaders", "Shader neu erstellen", "Rigenera shader", "Régénérer les shaders"}},
  {"cfg.shaders", {"Shaders", "Shaders", "Shaders", "Shader", "Shader", "Shaders"}},
  {"cfg.shaders_nota", {"La biblioteca de shaders se genera a partir de tu copia del juego. Regénérala si cambias de "
                        "edición o si algo se ve mal.",
                        "The shader library is generated from your game copy. Regenerate it if you change edition "
                        "or if something looks wrong.",
                        "A biblioteca de shaders é gerada a partir da sua cópia do jogo. Gere de novo se mudar de "
                        "edição ou se algo aparecer errado.", "Die Shader-Bibliothek wird aus deiner Spielkopie erstellt. Erstelle sie neu, wenn du die Edition wechselst oder etwas falsch aussieht.", "La libreria degli shader viene generata dalla tua copia del gioco. Rigenerala se cambi edizione o se qualcosa si vede male.", "La bibliothèque de shaders est générée depuis ta copie du jeu. Régénère-la si tu changes d'édition ou si quelque chose s'affiche mal."}},
  {"act.nueva", {"Nueva versión disponible", "New version available", "Nova versão disponível", "Neue Version verfügbar", "Nuova versione disponibile", "Nouvelle version disponible"}},
  {"act.tienes", {"tienes la", "you have", "você tem a", "du hast", "hai la", "tu as la"}},
  {"btn.descargar", {"Descargar", "Download", "Baixar", "Herunterladen", "Scarica", "Télécharger"}},
  {"btn.seguir", {"Seguir con esta", "Keep this one", "Continuar com esta", "Diese behalten", "Continua con questa", "Garder celle-ci"}},
  {"acerca.version", {"Versión", "Version", "Versão", "Version", "Versione", "Version"}},

  // ---- Graficos -----------------------------------------------------------------
  {"btn.xbox360", {"Restaurar valores de Xbox 360", "Restore Xbox 360 defaults", "Restaurar padrões do Xbox 360", "Xbox-360-Standard wiederherstellen", "Ripristina valori Xbox 360", "Restaurer les valeurs Xbox 360"}},
  {"graf.xbox360_nota", {"Deja el juego como en la consola original: 1280×720 a 30 fps y sombras, reflejos y filtros "
                         "sin cambios.",
                         "Sets the game as on the original console: 1280×720 at 30 FPS, with shadows, reflections "
                         "and filters unchanged.",
                         "Deixa o jogo como no console original: 1280×720 a 30 FPS, com sombras, reflexos e filtros "
                         "sem mudanças.", "Stellt das Spiel wie auf der Originalkonsole ein: 1280×720 mit 30 FPS, Schatten, Spiegelungen und Filter unverändert.", "Imposta il gioco come sulla console originale: 1280×720 a 30 FPS, con ombre, riflessi e filtri invariati.", "Règle le jeu comme sur la console d'origine : 1280×720 à 30 FPS, ombres, reflets et filtres inchangés."}},
  {"perfil.titulo", {"Perfil de calidad", "Quality preset", "Perfil de qualidade", "Qualitätsprofil", "Profilo di qualità", "Profil de qualité"}},
  {"perfil.bajo", {"Bajo", "Low", "Baixo", "Niedrig", "Basso", "Bas"}},
  {"perfil.medio", {"Medio", "Medium", "Médio", "Mittel", "Medio", "Moyen"}},
  {"perfil.alto", {"Alto", "High", "Alto", "Hoch", "Alto", "Élevé"}},
  {"perfil.ultra", {"Ultra", "Ultra", "Ultra", "Ultra", "Ultra", "Ultra"}},
  {"perfil.personalizado", {"Personalizado", "Custom", "Personalizado", "Benutzerdefiniert", "Personalizzato", "Personnalisé"}},
  {"perfil.nota", {"Elige un perfil y ajusta lo que quieras: al tocar cualquier opción pasa a Personalizado.",
                   "Pick a preset and tweak anything: changing any option switches to Custom.",
                   "Escolha um perfil e ajuste o que quiser: mudar qualquer opção passa para Personalizado.", "Wähle ein Profil und passe an, was du willst: Jede Änderung wechselt zu Benutzerdefiniert.", "Scegli un profilo e regola ciò che vuoi: modificare qualsiasi opzione passa a Personalizzato.", "Choisis un profil et ajuste ce que tu veux : modifier une option passe en Personnalisé."}},
  {"graf.pantalla", {"Pantalla", "Display", "Tela", "Anzeige", "Schermo", "Affichage"}},
  {"graf.completa", {"Pantalla completa", "Fullscreen", "Tela cheia", "Vollbild", "Schermo intero", "Plein écran"}},
  {"graf.resolucion", {"Resolución interna", "Internal resolution", "Resolução interna", "Interne Auflösung", "Risoluzione interna", "Résolution interne"}},
  {"graf.limite", {"Límite de fps", "FPS limit", "Limite de FPS", "FPS-Limit", "Limite FPS", "Limite d'FPS"}},
  {"graf.limite_60", {"60 fps", "60 FPS", "60 FPS", "60 FPS", "60 FPS", "60 FPS"}},
  {"graf.limite_30", {"30 fps estables", "Steady 30 FPS", "30 FPS estáveis", "Stabile 30 FPS", "30 FPS stabili", "30 FPS stables"}},
  {"ctl.vibracion", {"Vibración", "Vibration", "Vibração", "Vibration", "Vibrazione", "Vibration"}},
  {"estado.edicion", {"Edición", "Edition", "Edição", "Edition", "Edizione", "Édition"}},
  {"ed.desconocida", {"Desconocida: se intentará con la de este ejecutable", "Unknown: this executable's edition will be tried", "Desconhecida: será tentada a deste executável", "Unbekannt: Die Edition dieser Programmdatei wird versucht", "Sconosciuta: verrà provata quella di questo eseguibile", "Inconnue : celle de cet exécutable sera essayée"}},
  {"ed.falta_exe", {"Falta el módulo de esta edición:", "This edition's module is missing:", "Falta o módulo desta edição:", "Das Modul dieser Edition fehlt:", "Manca il modulo di questa edizione:", "Le module de cette édition est manquant :"}},
  {"tk.izq", {"Girar a la izquierda", "Steer left", "Virar à esquerda", "Nach links lenken", "Sterza a sinistra", "Tourner à gauche"}},
  {"tk.der", {"Girar a la derecha", "Steer right", "Virar à direita", "Nach rechts lenken", "Sterza a destra", "Tourner à droite"}},
  {"tk.arriba", {"Stick izquierdo arriba", "Left stick up", "Analógico esquerdo para cima", "Linker Stick hoch", "Levetta sinistra su", "Stick gauche haut"}},
  {"tk.abajo", {"Stick izquierdo abajo", "Left stick down", "Analógico esquerdo para baixo", "Linker Stick runter", "Levetta sinistra giù", "Stick gauche bas"}},
  {"tk.camara_arriba", {"Cámara arriba", "Camera up", "Câmera para cima", "Kamera hoch", "Telecamera su", "Caméra haut"}},
  {"tk.camara_abajo", {"Cámara abajo", "Camera down", "Câmera para baixo", "Kamera runter", "Telecamera giù", "Caméra bas"}},
  {"tk.camara_izq", {"Cámara izquierda", "Camera left", "Câmera à esquerda", "Kamera links", "Telecamera a sinistra", "Caméra gauche"}},
  {"tk.camara_der", {"Cámara derecha", "Camera right", "Câmera à direita", "Kamera rechts", "Telecamera a destra", "Caméra droite"}},
  {"tk.cruceta_arriba", {"Cruceta arriba", "D-pad up", "Direcional para cima", "Steuerkreuz hoch", "Croce direzionale su", "Croix directionnelle haut"}},
  {"tk.cruceta_abajo", {"Cruceta abajo", "D-pad down", "Direcional para baixo", "Steuerkreuz runter", "Croce direzionale giù", "Croix directionnelle bas"}},
  {"tk.cruceta_izq", {"Cruceta izquierda", "D-pad left", "Direcional à esquerda", "Steuerkreuz links", "Croce direzionale a sinistra", "Croix directionnelle gauche"}},
  {"tk.cruceta_der", {"Cruceta derecha", "D-pad right", "Direcional à direita", "Steuerkreuz rechts", "Croce direzionale a destra", "Croix directionnelle droite"}},
  {"ctl.pulsa", {"Pulsa una tecla…  (Esc cancela, Supr borra)", "Press a key…  (Esc cancels, Del clears)", "Aperte uma tecla…  (Esc cancela, Del apaga)", "Drücke eine Taste…  (Esc bricht ab, Entf löscht)", "Premi un tasto…  (Esc annulla, Canc cancella)", "Appuie sur une touche…  (Échap annule, Suppr efface)"}},
  {"ctl.mueve", {"Mueve el eje a fondo…", "Move the axis all the way…", "Mova o eixo até o fim…", "Bewege die Achse ganz aus…", "Muovi l'asse fino in fondo…", "Bouge l'axe jusqu'au bout…"}},
  {"ctl.eje", {"Eje", "Axis", "Eixo", "Achse", "Asse", "Axe"}},
  {"ctl.mapa_mando", {"Botones del mando", "Controller buttons", "Botões do controle", "Controller-Tasten", "Tasti del controller", "Boutons de la manette"}},
  {"ctl.mapa_mando_nota", {"Cada botón físico del mando (izquierda) hace la acción elegida (derecha). Solo con el sistema de entrada SDL.", "Each physical controller button (left) performs the chosen action (right). SDL input backend only.", "Cada botão físico do controle (esquerda) faz a ação escolhida (direita). Só com o sistema de entrada SDL.", "Jede physische Controller-Taste (links) führt die gewählte Aktion (rechts) aus. Nur mit dem SDL-Eingabesystem.", "Ogni tasto fisico del controller (sinistra) esegue l'azione scelta (destra). Solo con il sistema di input SDL.", "Chaque bouton physique de la manette (gauche) effectue l'action choisie (droite). Uniquement avec le système d'entrée SDL."}},
  {"ctl.volante", {"Volante", "Steering wheel", "Volante", "Lenkrad", "Volante", "Volant"}},
  {"ctl.volante_activo", {"Usar volantes y joysticks", "Use wheels and joysticks", "Usar volantes e joysticks", "Lenkräder und Joysticks verwenden", "Usa volanti e joystick", "Utiliser volants et joysticks"}},
  {"ctl.volante_dispositivo", {"Dispositivo", "Device", "Dispositivo", "Gerät", "Dispositivo", "Périphérique"}},
  {"ctl.volante_ninguno", {"No hay ningún volante conectado", "No steering wheel connected", "Nenhum volante conectado", "Kein Lenkrad verbunden", "Nessun volante collegato", "Aucun volant connecté"}},
  {"ctl.volante_dir", {"Dirección", "Steering", "Direção", "Lenkung", "Sterzo", "Direction"}},
  {"ctl.volante_acel", {"Acelerador", "Throttle", "Acelerador", "Gas", "Acceleratore", "Accélérateur"}},
  {"ctl.volante_freno", {"Freno", "Brake", "Freio", "Bremse", "Freno", "Frein"}},
  {"ctl.volante_invertir", {"Pedales invertidos (sueltos = máximo)", "Inverted pedals (released = max)", "Pedais invertidos (soltos = máximo)", "Pedale invertiert (losgelassen = Maximum)", "Pedali invertiti (rilasciati = massimo)", "Pédales inversées (relâchées = maximum)"}},
  {"ctl.volante_giro", {"Giro para girar a tope", "Rotation for full lock", "Giro para virar ao máximo", "Drehung für vollen Lenkeinschlag", "Rotazione per sterzata completa", "Rotation pour braquage maximal"}},
  {"ctl.volante_muerta", {"Zona muerta", "Dead zone", "Zona morta", "Totzone", "Zona morta", "Zone morte"}},
  {"ctl.boton_volante", {"Botón", "Button", "Botão", "Taste", "Tasto", "Bouton"}},
  {"ctl.volante_nota", {"Conecta el volante antes de abrir el lanzador. Pulsa una función y mueve el eje o pulsa el botón del volante para asignarlo. Solo con el sistema de entrada SDL.", "Connect the wheel before opening the launcher. Click a function and move the axis or press the wheel button to assign it. SDL input backend only.", "Conecte o volante antes de abrir o iniciador. Clique numa função e mova o eixo ou aperte o botão do volante para atribuí-lo. Só com o sistema de entrada SDL.", "Verbinde das Lenkrad, bevor du den Launcher öffnest. Klicke auf eine Funktion und bewege die Achse oder drücke die Lenkradtaste, um sie zuzuweisen. Nur mit dem SDL-Eingabesystem.", "Collega il volante prima di aprire il launcher. Fai clic su una funzione e muovi l'asse o premi il tasto del volante per assegnarlo. Solo con il sistema di input SDL.", "Connecte le volant avant d'ouvrir le lanceur. Clique sur une fonction puis bouge l'axe ou appuie sur le bouton du volant pour l'assigner. Uniquement avec le système d'entrée SDL."}},
  {"btn.restaurar_teclas", {"Restaurar teclas", "Reset keys", "Restaurar teclas", "Tasten zurücksetzen", "Ripristina tasti", "Réinitialiser les touches"}},
  {"btn.restaurar_mando", {"Restaurar botones", "Reset buttons", "Restaurar botões", "Tasten zurücksetzen", "Ripristina pulsanti", "Réinitialiser les boutons"}},
  {"btn.restaurar_volante", {"Restaurar volante", "Reset wheel", "Restaurar volante", "Lenkrad zurücksetzen", "Ripristina volante", "Réinitialiser le volant"}},
  {"graf.sombras", {"Sombras proyectadas", "Cast shadows", "Sombras projetadas", "Schattenwurf", "Ombre proiettate", "Ombres portées"}},
  {"graf.resolucion_nota", {"1440p y 4K son experimentales: piden mucha más GPU y pueden dar fallos gráficos.", "1440p and 4K are experimental: they need much more GPU power and may cause graphical glitches.", "1440p e 4K são experimentais: exigem muito mais GPU e podem causar falhas gráficas.", "1440p und 4K sind experimentell: Sie brauchen viel mehr GPU-Leistung und können Grafikfehler verursachen.", "1440p e 4K sono sperimentali: richiedono molta più GPU e possono causare difetti grafici.", "1440p et 4K sont expérimentaux : ils demandent beaucoup plus de GPU et peuvent provoquer des défauts graphiques."}},
  {"graf.limite_120", {"120 (exp.)", "120 (exp.)", "120 (exp.)", "120 (exp.)", "120 (sperim.)", "120 (exp.)"}},
  {"graf.limite_ilimitado", {"Ilimitado (exp.)", "Unlimited (exp.)", "Ilimitado (exp.)", "Unbegrenzt (exp.)", "Illimitato (sperim.)", "Illimité (exp.)"}},
  {"graf.limite_nota", {"120 e Ilimitado son experimentales: el juego puede ir más rápido de lo normal o con tirones.", "120 and Unlimited are experimental: the game may run faster than normal or stutter.", "120 e Ilimitado são experimentais: o jogo pode rodar mais rápido que o normal ou travar.", "120 und Unbegrenzt sind experimentell: Das Spiel kann schneller als normal laufen oder ruckeln.", "120 e Illimitato sono sperimentali: il gioco può andare più veloce del normale o scattare.", "120 et Illimité sont expérimentaux : le jeu peut tourner plus vite que la normale ou saccader."}},
  {"nav.partidas", {"Partidas", "Save data", "Jogos salvos", "Spielstände", "Salvataggi", "Sauvegardes"}},
  {"nav.trucos", {"Trucos", "Cheats", "Trapaças", "Cheats", "Trucchi", "Astuces"}},
  {"part.titulo", {"Partidas guardadas", "Saved games", "Jogos salvos", "Gespeicherte Spiele", "Partite salvate", "Parties sauvegardées"}},
  {"part.ninguna", {"Aún no hay partidas guardadas.", "No saved games yet.", "Ainda não há jogos salvos.", "Noch keine gespeicherten Spiele.", "Ancora nessuna partita salvata.", "Aucune partie sauvegardée pour l'instant."}},
  {"part.perfil", {"Perfil", "Profile", "Perfil", "Profil", "Profilo", "Profil"}},
  {"part.fecha", {"Modificada", "Modified", "Modificado", "Geändert", "Modificata", "Modifiée"}},
  {"part.tam", {"Tamaño", "Size", "Tamanho", "Größe", "Dimensione", "Taille"}},
  {"part.nota", {"Exportar copia la partida a una carpeta para llevarla a otro PC. Restaurar vuelve a la copia anterior que se guarda sola cada vez que el juego guarda.", "Export copies the save to a folder to take it to another PC. Restore goes back to the previous copy, kept automatically each time the game saves.", "Exportar copia o jogo salvo para uma pasta para levá-lo a outro PC. Restaurar volta à cópia anterior, guardada sozinha cada vez que o jogo salva.", "Exportieren kopiert den Spielstand in einen Ordner, um ihn auf einen anderen PC mitzunehmen. Wiederherstellen kehrt zur vorherigen Kopie zurück, die bei jedem Speichern automatisch angelegt wird.", "Esporta copia il salvataggio in una cartella per portarlo su un altro PC. Ripristina torna alla copia precedente, conservata da sola ogni volta che il gioco salva.", "Exporter copie la sauvegarde dans un dossier pour l'emporter sur un autre PC. Restaurer revient à la copie précédente, conservée automatiquement à chaque sauvegarde du jeu."}},
  {"part.ok", {"Hecho.", "Done.", "Feito.", "Erledigt.", "Fatto.", "Terminé."}},
  {"part.confirmar", {"¿Borrar esta partida? No se puede deshacer.", "Delete this save? This cannot be undone.", "Apagar este jogo salvo? Não dá para desfazer.", "Diesen Spielstand löschen? Das kann nicht rückgängig gemacht werden.", "Eliminare questo salvataggio? Non si può annullare.", "Supprimer cette sauvegarde ? C'est irréversible."}},
  {"btn.exportar", {"Exportar", "Export", "Exportar", "Exportieren", "Esporta", "Exporter"}},
  {"btn.importar", {"Importar partida", "Import save", "Importar jogo salvo", "Spielstand importieren", "Importa salvataggio", "Importer une sauvegarde"}},
  {"btn.restaurar", {"Restaurar anterior", "Restore previous", "Restaurar anterior", "Vorherigen wiederherstellen", "Ripristina precedente", "Restaurer la précédente"}},
  {"btn.borrar", {"Borrar", "Delete", "Apagar", "Löschen", "Elimina", "Supprimer"}},
  {"btn.confirmar", {"Sí, borrar", "Yes, delete", "Sim, apagar", "Ja, löschen", "Sì, elimina", "Oui, supprimer"}},
  {"btn.recargar", {"Recargar", "Reload", "Recarregar", "Neu laden", "Ricarica", "Recharger"}},
  {"btn.editar", {"Editar fichero de trucos", "Edit cheats file", "Editar arquivo de trapaças", "Cheat-Datei bearbeiten", "Modifica file dei trucchi", "Modifier le fichier d'astuces"}},
  {"tr.titulo", {"Trucos", "Cheats", "Trapaças", "Cheats", "Trucchi", "Astuces"}},
  {"tr.ninguno", {"No hay trucos en nfsmw_trucos.txt. Abre el fichero: dentro se explica cómo añadirlos.", "There are no cheats in nfsmw_trucos.txt. Open the file: it explains how to add them.", "Não há trapaças em nfsmw_trucos.txt. Abra o arquivo: lá dentro explica como adicioná-las.", "In nfsmw_trucos.txt gibt es keine Cheats. Öffne die Datei: Darin wird erklärt, wie man welche hinzufügt.", "Non ci sono trucchi in nfsmw_trucos.txt. Apri il file: dentro spiega come aggiungerli.", "Il n'y a aucune astuce dans nfsmw_trucos.txt. Ouvre le fichier : il explique comment en ajouter."}},
  {"tr.errores", {"Líneas no entendidas en el fichero", "Lines not understood in the file", "Linhas não entendidas no arquivo", "Nicht verstandene Zeilen in der Datei", "Righe non comprese nel file", "Lignes non comprises dans le fichier"}},
  {"tr.nota", {"Los trucos activos se aplican al jugar. Escriben en la memoria del juego: úsalos bajo tu responsabilidad y haz una copia de tu partida antes.", "Active cheats apply when you play. They write to the game's memory: use them at your own risk and back up your save first.", "As trapaças ativas valem ao jogar. Elas escrevem na memória do jogo: use por sua conta e faça uma cópia do seu jogo salvo antes.", "Aktive Cheats gelten beim Spielen. Sie schreiben in den Speicher des Spiels: Benutzung auf eigene Gefahr, sichere vorher deinen Spielstand.", "I trucchi attivi si applicano quando giochi. Scrivono nella memoria del gioco: usali a tuo rischio e fai prima una copia del salvataggio.", "Les astuces actives s'appliquent en jeu. Elles écrivent dans la mémoire du jeu : utilise-les à tes risques et sauvegarde ta partie avant."}},
  {"graf.imagen", {"Imagen", "Image", "Imagem", "Bild", "Immagine", "Image"}},
  {"graf.aa", {"FXAA", "FXAA", "FXAA", "FXAA", "FXAA", "FXAA"}},
  {"graf.msaa", {"MSAA", "MSAA", "MSAA", "MSAA", "MSAA", "MSAA"}},
  {"graf.fsr", {"Nitidez FSR1", "FSR1 sharpening", "Nitidez FSR1", "FSR1-Schärfe", "Nitidezza FSR1", "Netteté FSR1"}},
  {"graf.aa_nota", {"MSAA suaviza los bordes de la escena (la Xbox 360 usaba 4x) y pide reiniciar; FXAA suaviza la "
                    "imagen final y se puede combinar con MSAA. La nitidez FSR1 (RCAS de AMD) no se usa con FXAA.",
                    "MSAA smooths the scene's edges (the Xbox 360 used 4x) and needs a restart; FXAA smooths the final "
                    "image and can be combined with MSAA. FSR1 sharpening (AMD RCAS) is not used with FXAA.",
                    "MSAA suaviza as bordas da cena (o Xbox 360 usava 4x) e pede reinício; FXAA suaviza a imagem "
                    "final e pode ser combinado com MSAA. A nitidez FSR1 (RCAS da AMD) não é usada com FXAA.", "MSAA glättet die Kanten der Szene (die Xbox 360 nutzte 4x) und erfordert einen Neustart; FXAA glättet das fertige Bild und lässt sich mit MSAA kombinieren. Die FSR1-Schärfe (AMD RCAS) wird mit FXAA nicht verwendet.", "MSAA smussa i bordi della scena (la Xbox 360 usava 4x) e richiede un riavvio; FXAA smussa l'immagine finale e si può combinare con MSAA. La nitidezza FSR1 (RCAS di AMD) non si usa con FXAA.", "Le MSAA lisse les contours de la scène (la Xbox 360 utilisait 4x) et demande un redémarrage ; le FXAA lisse l'image finale et peut se combiner au MSAA. La netteté FSR1 (RCAS d'AMD) n'est pas utilisée avec le FXAA."}},
  {"val.baja", {"Baja", "Low", "Baixa", "Niedrig", "Bassa", "Basse"}},
  {"graf.resolucion_baja_nota", {"Por debajo de 720p bajan el posproceso y las copias; la escena se sigue dibujando "
                                 "a 720p.", "Below 720p, post-processing and copies are reduced; the scene is still "
                                 "drawn at 720p.", "Abaixo de 720p, o pós-processamento e as cópias são reduzidos; a "
                                 "cena continua sendo desenhada em 720p.", "Unter 720p werden Nachbearbeitung und Kopien reduziert; die Szene wird weiter in 720p gezeichnet.", "Sotto i 720p si riducono la post-elaborazione e le copie; la scena continua a essere disegnata a 720p.", "En dessous de 720p, le post-traitement et les copies sont réduits ; la scène reste dessinée en 720p."}},
  {"graf.aniso", {"Filtrado anisotrópico", "Anisotropic filtering", "Filtragem anisotrópica", "Anisotrope Filterung", "Filtro anisotropico", "Filtrage anisotrope"}},
  {"graf.color", {"Color", "Color", "Cor", "Farbe", "Colore", "Couleur"}},
  {"graf.color_nota", {"Se aplican al momento, también con el juego en marcha. El filtro de color es el tono amarillo, "
                       "la desaturación y la viñeta del propio juego.",
                       "They apply right away, also while playing. The color filter is the game's own yellow tint, "
                       "desaturation and vignette.",
                       "São aplicados na hora, também com o jogo rodando. O filtro de cor é o tom amarelo, a "
                       "dessaturação e a vinheta do próprio jogo.", "Wird sofort angewendet, auch während des Spiels. Der Farbfilter ist der gelbe Farbton, die Entsättigung und die Vignette des Spiels selbst.", "Si applicano subito, anche durante il gioco. Il filtro colore è la tinta gialla, la desaturazione e la vignettatura del gioco stesso.", "S'appliquent tout de suite, même en jeu. Le filtre de couleur est la teinte jaune, la désaturation et le vignettage du jeu lui-même."}},
  {"graf.filtro", {"Filtro de color del juego", "Game color filter", "Filtro de cor do jogo", "Farbfilter des Spiels", "Filtro colore del gioco", "Filtre de couleur du jeu"}},
  {"graf.posproceso", {"Posproceso", "Post-processing", "Pós-processamento", "Nachbearbeitung", "Post-elaborazione", "Post-traitement"}},
  {"graf.detalle", {"Detalle y rendimiento", "Detail & performance", "Detalhe e desempenho", "Details & Leistung", "Dettaglio e prestazioni", "Détails et performances"}},
  {"graf.sombras_dist", {"Distancia de sombras", "Shadow distance", "Distância das sombras", "Schattendistanz", "Distanza delle ombre", "Distance des ombres"}},
  {"graf.sombras_res", {"Resolución de sombras", "Shadow resolution", "Resolução das sombras", "Schattenauflösung", "Risoluzione delle ombre", "Résolution des ombres"}},
  {"graf.sombras_veg", {"Sombras de árboles y vegetación", "Tree and foliage shadows", "Sombras de árvores e vegetação", "Schatten von Bäumen und Vegetation", "Ombre di alberi e vegetazione", "Ombres des arbres et de la végétation"}},
  {"graf.reflejos", {"Reflejos de la carretera", "Road reflections", "Reflexos da pista", "Straßenspiegelungen", "Riflessi della strada", "Reflets de la route"}},
  {"graf.coche", {"Reflejos del coche", "Car reflections", "Reflexos do carro", "Fahrzeugspiegelungen", "Riflessi dell'auto", "Reflets de la voiture"}},
  {"graf.distancia", {"Distancia de dibujado", "Draw distance", "Distância de desenho", "Sichtweite", "Distanza di disegno", "Distance d'affichage"}},
  {"graf.reinicio", {"Se aplica al reiniciar", "Applies on restart", "Vale ao reiniciar", "Gilt nach Neustart", "Si applica al riavvio", "S'applique au redémarrage"}},
  {"val.apagado", {"Desactivado", "Off", "Desativado", "Aus", "Disattivato", "Désactivé"}},
  {"val.original", {"Original", "Original", "Original", "Original", "Originale", "Original"}},
  {"val.natural", {"Natural", "Natural", "Natural", "Natürlich", "Naturale", "Naturel"}},
  {"val.suave", {"Suave", "Soft", "Suave", "Sanft", "Leggero", "Doux"}},
  {"val.corta", {"Corta", "Short", "Curta", "Kurz", "Corta", "Courte"}},
  {"val.media", {"Media", "Medium", "Média", "Mittel", "Media", "Moyenne"}},
  {"val.normal", {"Normal (como el juego)", "Normal (as the game)", "Normal (como o jogo)", "Normal (wie im Spiel)", "Normale (come il gioco)", "Normale (comme le jeu)"}},
  {"val.alta", {"Alta", "High", "Alta", "Hoch", "Alta", "Haute"}},
  {"val.reducida", {"Reducida", "Reduced", "Reduzida", "Reduziert", "Ridotta", "Réduite"}},
  {"val.completo", {"Completo", "Full", "Completo", "Vollständig", "Completo", "Complet"}},
  {"val.cada8", {"Cada 8 fotogramas", "Every 8 frames", "A cada 8 quadros", "Alle 8 Frames", "Ogni 8 fotogrammi", "Toutes les 8 images"}},
  {"val.cada4", {"Cada 4 fotogramas", "Every 4 frames", "A cada 4 quadros", "Alle 4 Frames", "Ogni 4 fotogrammi", "Toutes les 4 images"}},
  {"val.cada2", {"Cada 2 fotogramas", "Every 2 frames", "A cada 2 quadros", "Alle 2 Frames", "Ogni 2 fotogrammi", "Toutes les 2 images"}},
  {"val.cada1", {"Cada fotograma", "Every frame", "A cada quadro", "Jeder Frame", "Ogni fotogramma", "Chaque image"}},
  {"pp.cine", {"Cine", "Cinema", "Cinema", "Kino", "Cinema", "Cinéma"}},
  {"pp.sepia", {"Sepia", "Sepia", "Sépia", "Sepia", "Seppia", "Sépia"}},
  {"pp.noir", {"Noir", "Noir", "Noir", "Noir", "Noir", "Noir"}},
  {"pp.frio", {"Frío", "Cold", "Frio", "Kalt", "Freddo", "Froid"}},
  {"pp.calido", {"Cálido", "Warm", "Quente", "Warm", "Caldo", "Chaud"}},
  {"pp.vivo", {"Vivo", "Vivid", "Vivo", "Lebhaft", "Vivido", "Vif"}},
  {"pp.matrix", {"Matrix", "Matrix", "Matrix", "Matrix", "Matrix", "Matrix"}},
  {"pp.crt", {"CRT", "CRT", "CRT", "CRT", "CRT", "CRT"}},

  // ---- Controles ----------------------------------------------------------------
  {"ctl.teclado", {"Teclado y ratón", "Keyboard & mouse", "Teclado e mouse", "Tastatur & Maus", "Tastiera e mouse", "Clavier et souris"}},
  {"ctl.jugar_teclado", {"Jugar con teclado", "Play with keyboard", "Jogar com teclado", "Mit Tastatur spielen", "Gioca con la tastiera", "Jouer au clavier"}},
  {"ctl.raton", {"Ratón para la cámara (stick derecho)", "Mouse for the camera (right stick)",
                 "Mouse para a câmera (analógico direito)", "Maus für die Kamera (rechter Stick)", "Mouse per la telecamera (levetta destra)", "Souris pour la caméra (stick droit)"}},
  {"ctl.sensibilidad", {"Sensibilidad del ratón", "Mouse sensitivity", "Sensibilidade do mouse", "Mausempfindlichkeit", "Sensibilità del mouse", "Sensibilité de la souris"}},
  {"ctl.teclado_nota", {"El teclado y el mando funcionan a la vez: sus botones se combinan.",
                        "Keyboard and controller work at the same time; their inputs are combined.",
                        "Teclado e controle funcionam ao mesmo tempo; os comandos são combinados.", "Tastatur und Controller funktionieren gleichzeitig; ihre Eingaben werden kombiniert.", "Tastiera e controller funzionano insieme; i loro comandi vengono combinati.", "Le clavier et la manette fonctionnent en même temps ; leurs commandes sont combinées."}},
  {"ctl.mando", {"Mando", "Controller", "Controle", "Controller", "Controller", "Manette"}},
  {"ctl.entrada", {"Sistema de entrada", "Input backend", "Sistema de entrada", "Eingabesystem", "Sistema di input", "Système d'entrée"}},
  {"ctl.sdl", {"SDL (recomendado)", "SDL (recommended)", "SDL (recomendado)", "SDL (empfohlen)", "SDL (consigliato)", "SDL (recommandé)"}},
  {"ctl.guia", {"Botón Guide / PS", "Guide / PS button", "Botão Guide / PS", "Guide-/PS-Taste", "Tasto Guide / PS", "Bouton Guide / PS"}},
  {"ctl.mando_nota", {"SDL reconoce sin configurar los mandos de Xbox y PlayStation. Steam o DS4Windows abiertos "
                      "pueden ocultar el mando: ciérralos. XInput solo sirve para mandos de Xbox.",
                      "SDL recognizes Xbox and PlayStation controllers without setup. Steam or DS4Windows can hide "
                      "the controller while open: close them. XInput only works with Xbox controllers.",
                      "O SDL reconhece controles de Xbox e PlayStation sem configurar. Steam ou DS4Windows abertos "
                      "podem esconder o controle: feche-os. XInput só funciona com controles de Xbox.", "SDL erkennt Xbox- und PlayStation-Controller ohne Einrichtung. Geöffnetes Steam oder DS4Windows kann den Controller verbergen: Schließe sie. XInput funktioniert nur mit Xbox-Controllern.", "SDL riconosce i controller Xbox e PlayStation senza configurarli. Steam o DS4Windows aperti possono nascondere il controller: chiudili. XInput funziona solo con i controller Xbox.", "SDL reconnaît les manettes Xbox et PlayStation sans configuration. Steam ou DS4Windows ouverts peuvent masquer la manette : ferme-les. XInput ne fonctionne qu'avec les manettes Xbox."}},
  {"ctl.mapa", {"Mapa del teclado", "Keyboard map", "Mapa do teclado", "Tastaturbelegung", "Mappa della tastiera", "Disposition du clavier"}},
  {"ctl.boton", {"Botón de Xbox 360", "Xbox 360 button", "Botão do Xbox 360", "Xbox-360-Taste", "Tasto Xbox 360", "Bouton Xbox 360"}},
  {"ctl.tecla", {"Tecla", "Key", "Tecla", "Taste", "Tasto", "Touche"}},
  {"ctl.mapa_nota", {"Pulsa una acción y luego la tecla. Con Shift pulsado se guarda como Shift + tecla.", "Click an action, then press the key. Holding Shift saves it as Shift + key.", "Clique numa ação e depois aperte a tecla. Com Shift apertado, salva como Shift + tecla.", "Klicke auf eine Aktion und drücke dann die Taste. Mit gedrückter Umschalttaste wird sie als Umschalt + Taste gespeichert.", "Fai clic su un'azione, poi premi il tasto. Tenendo premuto Maiusc si salva come Maiusc + tasto.", "Clique sur une action, puis appuie sur la touche. En maintenant Maj, elle est enregistrée comme Maj + touche."}},
  {"k.espacio", {"Espacio", "Space", "Espaço", "Leertaste", "Spazio", "Espace"}},
  {"k.retroceso", {"Retroceso", "Backspace", "Backspace", "Rücktaste", "Backspace", "Retour arrière"}},
  {"k.flechas", {"Flechas", "Arrow keys", "Setas", "Pfeiltasten", "Frecce", "Flèches"}},
  {"k.sinasignar", {"Sin asignar", "Unassigned", "Não atribuído", "Nicht belegt", "Non assegnato", "Non assigné"}},
  {"tk.rt", {"Gatillo derecho (RT) · acelerar", "Right trigger (RT) · accelerate", "Gatilho direito (RT) · acelerar", "Rechter Trigger (RT) · Gas", "Grilletto destro (RT) · accelera", "Gâchette droite (RT) · accélérer"}},
  {"tk.lt", {"Gatillo izquierdo (LT) · frenar", "Left trigger (LT) · brake", "Gatilho esquerdo (LT) · frear", "Linker Trigger (LT) · Bremse", "Grilletto sinistro (LT) · frena", "Gâchette gauche (LT) · freiner"}},
  {"tk.lb", {"Bumper izquierdo (LB)", "Left bumper (LB)", "Bumper esquerdo (LB)", "Linke Schultertaste (LB)", "Dorsale sinistro (LB)", "Bouton supérieur gauche (LB)"}},
  {"tk.rb", {"Bumper derecho (RB)", "Right bumper (RB)", "Bumper direito (RB)", "Rechte Schultertaste (RB)", "Dorsale destro (RB)", "Bouton supérieur droit (RB)"}},
  {"tk.stickizq", {"Stick izquierdo · girar", "Left stick · steer", "Analógico esquerdo · virar", "Linker Stick · lenken", "Levetta sinistra · sterzare", "Stick gauche · tourner"}},
  {"tk.stickder", {"Stick derecho · cámara", "Right stick · camera", "Analógico direito · câmera", "Rechter Stick · Kamera", "Levetta destra · telecamera", "Stick droit · caméra"}},
  {"tk.pulsarizq", {"Pulsar stick izquierdo", "Left stick click", "Clique do analógico esquerdo", "Linken Stick drücken", "Pressione levetta sinistra", "Clic du stick gauche"}},
  {"tk.pulsarder", {"Pulsar stick derecho", "Right stick click", "Clique do analógico direito", "Rechten Stick drücken", "Pressione levetta destra", "Clic du stick droit"}},
  {"tk.cruceta", {"Cruceta", "D-pad", "Direcional", "Steuerkreuz", "Croce direzionale", "Croix directionnelle"}},

  // ---- Audio --------------------------------------------------------------------
  {"audio.titulo", {"Sonido", "Sound", "Som", "Ton", "Suono", "Son"}},
  {"audio.silenciar", {"Silenciar el audio", "Mute audio", "Silenciar o áudio", "Ton stummschalten", "Disattiva l'audio", "Couper le son"}},
  {"audio.nota", {"El volumen de la música, los efectos y las voces se ajusta en el menú de opciones del propio "
                  "juego. El sonido sale por el dispositivo predeterminado del sistema.",
                  "Music, effects and voice volume are set in the game's own options menu. Sound plays through "
                  "the system default device.",
                  "O volume da música, efeitos e vozes é ajustado no menu de opções do próprio jogo. O som sai "
                  "pelo dispositivo padrão do sistema.", "Die Lautstärke von Musik, Effekten und Stimmen stellst du im Optionsmenü des Spiels ein. Der Ton läuft über das Standardgerät des Systems.", "Il volume di musica, effetti e voci si regola nel menu opzioni del gioco. L'audio esce dal dispositivo predefinito del sistema.", "Le volume de la musique, des effets et des voix se règle dans le menu des options du jeu. Le son passe par le périphérique par défaut du système."}},

  // ---- Configuracion ------------------------------------------------------------
  {"cfg.idioma", {"Idioma", "Language", "Idioma", "Sprache", "Lingua", "Langue"}},
  {"cfg.idioma_auto", {"Automático (el del sistema)", "Automatic (system)", "Automático (do sistema)", "Automatisch (System)", "Automatica (sistema)", "Automatique (système)"}},
  {"cfg.inicio", {"Inicio", "Startup", "Inicialização", "Start", "Avvio", "Démarrage"}},
  {"cfg.lanzador", {"Mostrar este lanzador al iniciar", "Show this launcher at startup",
                    "Mostrar este launcher ao iniciar", "Diesen Launcher beim Start anzeigen", "Mostra questo launcher all'avvio", "Afficher ce lanceur au démarrage"}},
  {"cfg.lanzador_nota", {"Si lo desactivas, el juego arrancará directamente. Para recuperarlo, borra nfsmw.toml "
                         "o pon nfsmw_lanzador = true en él.",
                         "If you turn it off, the game will start directly. To bring it back, delete nfsmw.toml "
                         "or set nfsmw_lanzador = true in it.",
                         "Se desativar, o jogo iniciará direto. Para recuperá-lo, apague nfsmw.toml ou coloque "
                         "nfsmw_lanzador = true nele.", "Wenn du ihn ausschaltest, startet das Spiel direkt. Um ihn zurückzuholen, lösche nfsmw.toml oder setze darin nfsmw_lanzador = true.", "Se lo disattivi, il gioco partirà direttamente. Per riaverlo, elimina nfsmw.toml o imposta nfsmw_lanzador = true al suo interno.", "Si tu le désactives, le jeu démarrera directement. Pour le retrouver, supprime nfsmw.toml ou mets nfsmw_lanzador = true dedans."}},
  {"cfg.carpetas", {"Carpetas", "Folders", "Pastas", "Ordner", "Cartelle", "Dossiers"}},

  // ---- Acerca de ----------------------------------------------------------------
  {"acerca.texto", {"Recompilación estática nativa de Need for Speed: Most Wanted (2005) para Xbox 360. El código "
                    "PowerPC del juego se traduce a C++ antes de compilar y se ejecuta como código nativo: no es un "
                    "emulador.",
                    "A native static recompilation of Need for Speed: Most Wanted (2005) for Xbox 360. The game's "
                    "PowerPC code is translated to C++ ahead of time and runs as native code: it is not an "
                    "emulator.",
                    "Recompilação estática nativa de Need for Speed: Most Wanted (2005) para Xbox 360. O código "
                    "PowerPC do jogo é traduzido para C++ antes de compilar e roda como código nativo: não é um "
                    "emulador.", "Eine native statische Rekompilierung von Need for Speed: Most Wanted (2005) für Xbox 360. Der PowerPC-Code des Spiels wird vorab in C++ übersetzt und läuft als nativer Code: Es ist kein Emulator.", "Una ricompilazione statica nativa di Need for Speed: Most Wanted (2005) per Xbox 360. Il codice PowerPC del gioco viene tradotto in C++ prima della compilazione e gira come codice nativo: non è un emulatore.", "Une recompilation statique native de Need for Speed: Most Wanted (2005) pour Xbox 360. Le code PowerPC du jeu est traduit en C++ à l'avance et s'exécute en code natif : ce n'est pas un émulateur."}},
  {"acerca.base", {"Renderizador Vulkan 1.1 escrito desde cero por Madeline_dev y Camille LaVey. Construido sobre ReXGlue.", "Vulkan 1.1 renderer written from scratch by Madeline_dev and Camille LaVey. Built on ReXGlue.", "Renderizador Vulkan 1.1 escrito do zero por Madeline_dev e Camille LaVey. Construído sobre o ReXGlue.", "Vulkan-1.1-Renderer, von Grund auf geschrieben von Madeline_dev und Camille LaVey. Basiert auf ReXGlue.", "Renderer Vulkan 1.1 scritto da zero da Madeline_dev e Camille LaVey. Basato su ReXGlue.", "Moteur de rendu Vulkan 1.1 écrit de zéro par Madeline_dev et Camille LaVey. Basé sur ReXGlue."}},
  {"acerca.legal", {"Necesitas tu propia copia del juego: este proyecto no incluye ni distribuye datos del juego. "
                    "Need for Speed y Most Wanted son marcas de Electronic Arts; este proyecto no está afiliado a "
                    "EA. Código bajo licencia GPLv3.",
                    "You need your own copy of the game: this project does not include or distribute any game "
                    "data. Need for Speed and Most Wanted are trademarks of Electronic Arts; this project is not "
                    "affiliated with EA. Code licensed under GPLv3.",
                    "Você precisa da sua própria cópia do jogo: este projeto não inclui nem distribui dados do "
                    "jogo. Need for Speed e Most Wanted são marcas da Electronic Arts; este projeto não é afiliado "
                    "à EA. Código sob licença GPLv3.", "Du brauchst deine eigene Kopie des Spiels: Dieses Projekt enthält und verbreitet keine Spieldaten. Need for Speed und Most Wanted sind Marken von Electronic Arts; dieses Projekt steht in keiner Verbindung zu EA. Code unter GPLv3-Lizenz.", "Ti serve una tua copia del gioco: questo progetto non include né distribuisce dati del gioco. Need for Speed e Most Wanted sono marchi di Electronic Arts; questo progetto non è affiliato a EA. Codice con licenza GPLv3.", "Tu as besoin de ta propre copie du jeu : ce projet n'inclut ni ne distribue aucune donnée du jeu. Need for Speed et Most Wanted sont des marques d'Electronic Arts ; ce projet n'est pas affilié à EA. Code sous licence GPLv3."}},
};
// clang-format on

const std::unordered_map<std::string_view, const Texto*>& Indice() {
  static const auto indice = [] {
    std::unordered_map<std::string_view, const Texto*> m;
    for (const auto& t : kTextos) m.emplace(t.clave, &t);
    return m;
  }();
  return indice;
}

constexpr const char* kCodigos[kNumIdiomas] = {"es", "en", "pt", "de", "it", "fr"};
constexpr const char* kNombres[kNumIdiomas] = {"Español", "English", "Português", "Deutsch", "Italiano",
                                               "Français"};

int DelSistema() {
#ifdef _WIN32
  // Idioma primario = los 10 bits bajos del LANGID: 0x0A espanol, 0x16 portugues, 0x07 aleman, 0x10 italiano y
  // 0x0C frances.
  switch (GetUserDefaultUILanguage() & 0x3FF) {
    case 0x0A:
      return 0;
    case 0x16:
      return 2;
    case 0x07:
      return 3;
    case 0x10:
      return 4;
    case 0x0C:
      return 5;
    default:
      return 1;
  }
#else
  const char* lang = std::getenv("LANG");
  if (!lang) return 1;
  for (int i = 0; i < kNumIdiomas; ++i)
    if (std::strncmp(lang, kCodigos[i], 2) == 0) return i;
  return 1;
#endif
}


}  // namespace

int Actual() {
  const std::string v = REXCVAR_GET(nfsmw_idioma);
  for (int i = 0; i < kNumIdiomas; ++i)
    if (v == kCodigos[i]) return i;
  static const int sistema = DelSistema();
  return sistema;
}

void Elegir(int idioma) {
  rex::cvar::SetFlagByName("nfsmw_idioma", idioma >= 0 && idioma < kNumIdiomas ? kCodigos[idioma] : "");
}

const char* T(const char* clave) {
  const auto& indice = Indice();
  const auto it = indice.find(clave);
  return it == indice.end() ? clave : it->second->t[Actual()];
}

const char* Nombre(int idioma) { return idioma >= 0 && idioma < kNumIdiomas ? kNombres[idioma] : ""; }

}  // namespace nfsmw::idioma
