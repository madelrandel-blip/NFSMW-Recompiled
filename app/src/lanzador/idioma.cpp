// nfsmw - textos de la interfaz en espanol, ingles y portugues (ver idioma.h).

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
                      "Idioma del lanzador y de los menus: es, en, pt (vacio = el del sistema)")
    .allowed({"", "es", "en", "pt"});

namespace nfsmw::idioma {
namespace {

struct Texto {
  const char* clave;
  const char* t[kNumIdiomas];  // es, en, pt
};

// clang-format off
constexpr Texto kTextos[] = {
  // ---- General ----------------------------------------------------------------
  {"app.subtitulo", {"Recompilación nativa para PC", "Native PC recompilation", "Recompilação nativa para PC"}},
  {"nav.inicio", {"Inicio", "Home", "Início"}},
  {"nav.graficos", {"Gráficos", "Graphics", "Gráficos"}},
  {"nav.controles", {"Controles", "Controls", "Controles"}},
  {"nav.audio", {"Audio", "Audio", "Áudio"}},
  {"nav.config", {"Configuración", "Settings", "Configurações"}},
  {"nav.acerca", {"Acerca de", "About", "Sobre"}},
  {"btn.jugar", {"JUGAR", "PLAY", "JOGAR"}},
  {"btn.reiniciar", {"APLICAR Y REINICIAR", "APPLY AND RESTART", "APLICAR E REINICIAR"}},
  {"btn.salir", {"Salir", "Exit", "Sair"}},
  {"btn.cancelar", {"Cancelar", "Cancel", "Cancelar"}},
  {"btn.iso", {"Elegir ISO…", "Choose ISO…", "Escolher ISO…"}},
  {"btn.carpeta", {"Elegir carpeta…", "Choose folder…", "Escolher pasta…"}},
  {"btn.repo", {"Repositorio en GitHub", "GitHub repository", "Repositório no GitHub"}},
  {"btn.logs", {"Carpeta de logs", "Logs folder", "Pasta de logs"}},
  {"btn.guardados", {"Carpeta de partidas", "Saves folder", "Pasta de saves"}},
  {"aviso.reinicio", {"Hay cambios que se aplican al reiniciar: el juego se reiniciará solo.",
                      "Some changes apply on restart: the game will restart by itself.",
                      "Algumas mudanças valem ao reiniciar: o jogo reiniciará sozinho."}},
  {"aviso.sin_juego", {"Elige tu copia del juego para poder jugar.", "Choose your game copy to play.",
                       "Escolha sua cópia do jogo para jogar."}},

  // ---- Inicio -------------------------------------------------------------------
  {"juego.titulo", {"Copia del juego", "Game copy", "Cópia do jogo"}},
  {"juego.nota", {"Elige la ISO de tu juego o una carpeta ya extraída (con default.xex dentro). La primera vez "
                  "con una ISO se extrae una copia junto al juego; después arranca directo.",
                  "Choose your game ISO or an already extracted folder (with default.xex inside). The first time "
                  "with an ISO a copy is extracted next to the game; after that it starts right away.",
                  "Escolha a ISO do seu jogo ou uma pasta já extraída (com default.xex dentro). Na primeira vez "
                  "com uma ISO uma cópia é extraída ao lado do jogo; depois inicia direto."}},
  {"juego.ninguno", {"Todavía no has elegido tu copia del juego.", "You have not chosen your game copy yet.",
                     "Você ainda não escolheu sua cópia do jogo."}},
  {"estado.titulo", {"Estado", "Status", "Status"}},
  {"estado.juego", {"Juego", "Game", "Jogo"}},
  {"estado.config", {"Configuración", "Settings", "Configuração"}},
  {"estado.iso_nueva", {"ISO: se extraerá al pulsar Jugar", "ISO: extracted when you press Play",
                        "ISO: será extraída ao clicar em Jogar"}},
  {"estado.iso_lista", {"ISO ya extraída, lista para jugar", "ISO already extracted, ready to play",
                        "ISO já extraída, pronta para jogar"}},
  {"estado.carpeta", {"Carpeta lista para jugar", "Folder ready to play", "Pasta pronta para jogar"}},
  {"estado.invalida", {"Esa carpeta no contiene default.xex", "That folder does not contain default.xex",
                       "Essa pasta não contém default.xex"}},
  {"estado.falta", {"No se encuentra el archivo elegido", "The chosen file cannot be found",
                    "O arquivo escolhido não foi encontrado"}},
  {"inicio.atajos", {"Durante el juego: F3 muestra los FPS.", "In game: F3 shows the FPS.", "No jogo: F3 mostra o FPS."}},
  {"extraer.titulo", {"Extrayendo la ISO…", "Extracting the ISO…", "Extraindo a ISO…"}},
  {"extraer.nota", {"Solo hace falta la primera vez. Puede tardar varios minutos.",
                    "Only needed the first time. It may take several minutes.",
                    "Só é necessário na primeira vez. Pode levar vários minutos."}},
  {"extraer.error", {"No se pudo extraer la ISO", "Could not extract the ISO", "Não foi possível extrair a ISO"}},

  // ---- Shaders y actualizaciones --------------------------------------------------
  {"estado.shaders", {"Shaders", "Shaders", "Shaders"}},
  {"shaders.listos", {"Generados", "Generated", "Gerados"}},
  {"shaders.faltan", {"Se generarán al pulsar Jugar", "Generated when you press Play", "Serão gerados ao clicar em Jogar"}},
  {"shaders.sin_dxc", {"Falta dxcompiler.dll: no se pueden generar", "dxcompiler.dll is missing: cannot generate",
                       "Falta dxcompiler.dll: não é possível gerar"}},
  {"shaders.titulo", {"Generando los shaders…", "Generating the shaders…", "Gerando os shaders…"}},
  {"shaders.nota", {"Se traducen los shaders de tu copia del juego para tu tarjeta gráfica. Solo hace falta una vez.",
                    "Your game copy's shaders are translated for your graphics card. Only needed once.",
                    "Os shaders da sua cópia do jogo são traduzidos para sua placa de vídeo. Só é necessário uma vez."}},
  {"shaders.error", {"No se pudieron generar los shaders", "Could not generate the shaders",
                     "Não foi possível gerar os shaders"}},
  {"fase.imagen", {"Leyendo el ejecutable", "Reading the executable", "Lendo o executável"}},
  {"fase.buscar", {"Buscando shaders en el disco", "Finding shaders on the disc", "Procurando shaders no disco"}},
  {"fase.traducir", {"Traduciendo", "Translating", "Traduzindo"}},
  {"fase.compilar", {"Compilando para Vulkan", "Compiling for Vulkan", "Compilando para Vulkan"}},
  {"fase.empaquetar", {"Guardando la biblioteca", "Saving the library", "Salvando a biblioteca"}},
  {"btn.regenerar", {"Regenerar shaders", "Regenerate shaders", "Regenerar shaders"}},
  {"cfg.shaders", {"Shaders", "Shaders", "Shaders"}},
  {"cfg.shaders_nota", {"La biblioteca de shaders se genera a partir de tu copia del juego. Regénérala si cambias de "
                        "edición o si algo se ve mal.",
                        "The shader library is generated from your game copy. Regenerate it if you change edition "
                        "or if something looks wrong.",
                        "A biblioteca de shaders é gerada a partir da sua cópia do jogo. Gere de novo se mudar de "
                        "edição ou se algo aparecer errado."}},
  {"act.nueva", {"Nueva versión disponible", "New version available", "Nova versão disponível"}},
  {"act.tienes", {"tienes la", "you have", "você tem a"}},
  {"btn.descargar", {"Descargar", "Download", "Baixar"}},
  {"btn.seguir", {"Seguir con esta", "Keep this one", "Continuar com esta"}},
  {"acerca.version", {"Versión", "Version", "Versão"}},

  // ---- Graficos -----------------------------------------------------------------
  {"btn.xbox360", {"Restaurar valores de Xbox 360", "Restore Xbox 360 defaults", "Restaurar padrões do Xbox 360"}},
  {"graf.xbox360_nota", {"Deja el juego como en la consola original: 1280×720 a 30 fps y sombras, reflejos y filtros "
                         "sin cambios.",
                         "Sets the game as on the original console: 1280×720 at 30 FPS, with shadows, reflections "
                         "and filters unchanged.",
                         "Deixa o jogo como no console original: 1280×720 a 30 FPS, com sombras, reflexos e filtros "
                         "sem mudanças."}},
  {"perfil.titulo", {"Perfil de calidad", "Quality preset", "Perfil de qualidade"}},
  {"perfil.bajo", {"Bajo", "Low", "Baixo"}},
  {"perfil.medio", {"Medio", "Medium", "Médio"}},
  {"perfil.alto", {"Alto", "High", "Alto"}},
  {"perfil.ultra", {"Ultra", "Ultra", "Ultra"}},
  {"perfil.personalizado", {"Personalizado", "Custom", "Personalizado"}},
  {"perfil.nota", {"Elige un perfil y ajusta lo que quieras: al tocar cualquier opción pasa a Personalizado.",
                   "Pick a preset and tweak anything: changing any option switches to Custom.",
                   "Escolha um perfil e ajuste o que quiser: mudar qualquer opção passa para Personalizado."}},
  {"graf.motor", {"Motor gráfico", "Graphics engine", "Motor gráfico"}},
  {"graf.renderizador", {"Renderizador", "Renderer", "Renderizador"}},
  {"graf.nativo", {"Nativo (recomendado)", "Native (recommended)", "Nativo (recomendado)"}},
  {"graf.emulado", {"Emulación Xenos (lento)", "Xenos emulation (slow)", "Emulação Xenos (lento)"}},
  {"graf.renderizador_nota", {"El nativo dibuja el juego directamente con Vulkan y es mucho más rápido. La "
                              "emulación queda como alternativa si algo se ve mal.",
                              "Native draws the game directly with Vulkan and is much faster. Emulation remains "
                              "as a fallback if something looks wrong.",
                              "O nativo desenha o jogo direto com Vulkan e é muito mais rápido. A emulação fica "
                              "como alternativa se algo aparecer errado."}},
  {"graf.pantalla", {"Pantalla", "Display", "Tela"}},
  {"graf.completa", {"Pantalla completa", "Fullscreen", "Tela cheia"}},
  {"graf.resolucion", {"Resolución interna", "Internal resolution", "Resolução interna"}},
  {"graf.limite", {"Límite de fps", "FPS limit", "Limite de FPS"}},
  {"graf.limite_60", {"60 fps", "60 FPS", "60 FPS"}},
  {"graf.limite_30", {"30 fps estables", "Steady 30 FPS", "30 FPS estáveis"}},
  {"estado.edicion", {"Edición", "Edition", "Edição"}},
  {"ed.desconocida", {"Desconocida: se intentará con la de este ejecutable", "Unknown: this executable's edition will be tried", "Desconhecida: será tentada a deste executável"}},
  {"ed.falta_exe", {"Falta el ejecutable de esta edición:", "This edition's executable is missing:", "Falta o executável desta edição:"}},
  {"tk.izq", {"Girar a la izquierda", "Steer left", "Virar à esquerda"}},
  {"tk.der", {"Girar a la derecha", "Steer right", "Virar à direita"}},
  {"tk.arriba", {"Stick izquierdo arriba", "Left stick up", "Analógico esquerdo para cima"}},
  {"tk.abajo", {"Stick izquierdo abajo", "Left stick down", "Analógico esquerdo para baixo"}},
  {"tk.camara_arriba", {"Cámara arriba", "Camera up", "Câmera para cima"}},
  {"tk.camara_abajo", {"Cámara abajo", "Camera down", "Câmera para baixo"}},
  {"tk.camara_izq", {"Cámara izquierda", "Camera left", "Câmera à esquerda"}},
  {"tk.camara_der", {"Cámara derecha", "Camera right", "Câmera à direita"}},
  {"tk.cruceta_arriba", {"Cruceta arriba", "D-pad up", "Direcional para cima"}},
  {"tk.cruceta_abajo", {"Cruceta abajo", "D-pad down", "Direcional para baixo"}},
  {"tk.cruceta_izq", {"Cruceta izquierda", "D-pad left", "Direcional à esquerda"}},
  {"tk.cruceta_der", {"Cruceta derecha", "D-pad right", "Direcional à direita"}},
  {"ctl.pulsa", {"Pulsa una tecla…  (Esc cancela, Supr borra)", "Press a key…  (Esc cancels, Del clears)", "Aperte uma tecla…  (Esc cancela, Del apaga)"}},
  {"ctl.mueve", {"Mueve el eje a fondo…", "Move the axis all the way…", "Mova o eixo até o fim…"}},
  {"ctl.eje", {"Eje", "Axis", "Eixo"}},
  {"ctl.mapa_mando", {"Botones del mando", "Controller buttons", "Botões do controle"}},
  {"ctl.mapa_mando_nota", {"Cada botón físico del mando (izquierda) hace la acción elegida (derecha). Solo con el sistema de entrada SDL.", "Each physical controller button (left) performs the chosen action (right). SDL input backend only.", "Cada botão físico do controle (esquerda) faz a ação escolhida (direita). Só com o sistema de entrada SDL."}},
  {"ctl.volante", {"Volante", "Steering wheel", "Volante"}},
  {"ctl.volante_activo", {"Usar volantes y joysticks", "Use wheels and joysticks", "Usar volantes e joysticks"}},
  {"ctl.volante_dispositivo", {"Dispositivo", "Device", "Dispositivo"}},
  {"ctl.volante_ninguno", {"No hay ningún volante conectado", "No steering wheel connected", "Nenhum volante conectado"}},
  {"ctl.volante_dir", {"Dirección", "Steering", "Direção"}},
  {"ctl.volante_acel", {"Acelerador", "Throttle", "Acelerador"}},
  {"ctl.volante_freno", {"Freno", "Brake", "Freio"}},
  {"ctl.volante_invertir", {"Pedales invertidos (sueltos = máximo)", "Inverted pedals (released = max)", "Pedais invertidos (soltos = máximo)"}},
  {"ctl.volante_giro", {"Giro para girar a tope", "Rotation for full lock", "Giro para virar ao máximo"}},
  {"ctl.volante_muerta", {"Zona muerta", "Dead zone", "Zona morta"}},
  {"ctl.boton_volante", {"Botón", "Button", "Botão"}},
  {"ctl.volante_nota", {"Conecta el volante antes de abrir el lanzador. Pulsa una función y mueve el eje o pulsa el botón del volante para asignarlo. Solo con el sistema de entrada SDL.", "Connect the wheel before opening the launcher. Click a function and move the axis or press the wheel button to assign it. SDL input backend only.", "Conecte o volante antes de abrir o iniciador. Clique numa função e mova o eixo ou aperte o botão do volante para atribuí-lo. Só com o sistema de entrada SDL."}},
  {"btn.restaurar_teclas", {"Restaurar teclas", "Reset keys", "Restaurar teclas"}},
  {"btn.restaurar_mando", {"Restaurar botones", "Reset buttons", "Restaurar botões"}},
  {"btn.restaurar_volante", {"Restaurar volante", "Reset wheel", "Restaurar volante"}},
  {"graf.sombras", {"Sombras proyectadas", "Cast shadows", "Sombras projetadas"}},
  {"graf.resolucion_nota", {"1440p y 4K son experimentales: piden mucha más GPU y pueden dar fallos gráficos.", "1440p and 4K are experimental: they need much more GPU power and may cause graphical glitches.", "1440p e 4K são experimentais: exigem muito mais GPU e podem causar falhas gráficas."}},
  {"graf.limite_120", {"120 (exp.)", "120 (exp.)", "120 (exp.)"}},
  {"graf.limite_ilimitado", {"Ilimitado (exp.)", "Unlimited (exp.)", "Ilimitado (exp.)"}},
  {"graf.limite_nota", {"120 e Ilimitado son experimentales: el juego puede ir más rápido de lo normal o con tirones.", "120 and Unlimited are experimental: the game may run faster than normal or stutter.", "120 e Ilimitado são experimentais: o jogo pode rodar mais rápido que o normal ou travar."}},
  {"nav.partidas", {"Partidas", "Save data", "Jogos salvos"}},
  {"nav.trucos", {"Trucos", "Cheats", "Trapaças"}},
  {"part.titulo", {"Partidas guardadas", "Saved games", "Jogos salvos"}},
  {"part.ninguna", {"Aún no hay partidas guardadas.", "No saved games yet.", "Ainda não há jogos salvos."}},
  {"part.perfil", {"Perfil", "Profile", "Perfil"}},
  {"part.fecha", {"Modificada", "Modified", "Modificado"}},
  {"part.tam", {"Tamaño", "Size", "Tamanho"}},
  {"part.nota", {"Exportar copia la partida a una carpeta para llevarla a otro PC. Restaurar vuelve a la copia anterior que se guarda sola cada vez que el juego guarda.", "Export copies the save to a folder to take it to another PC. Restore goes back to the previous copy, kept automatically each time the game saves.", "Exportar copia o jogo salvo para uma pasta para levá-lo a outro PC. Restaurar volta à cópia anterior, guardada sozinha cada vez que o jogo salva."}},
  {"part.ok", {"Hecho.", "Done.", "Feito."}},
  {"part.confirmar", {"¿Borrar esta partida? No se puede deshacer.", "Delete this save? This cannot be undone.", "Apagar este jogo salvo? Não dá para desfazer."}},
  {"btn.exportar", {"Exportar", "Export", "Exportar"}},
  {"btn.importar", {"Importar partida", "Import save", "Importar jogo salvo"}},
  {"btn.restaurar", {"Restaurar anterior", "Restore previous", "Restaurar anterior"}},
  {"btn.borrar", {"Borrar", "Delete", "Apagar"}},
  {"btn.confirmar", {"Sí, borrar", "Yes, delete", "Sim, apagar"}},
  {"btn.recargar", {"Recargar", "Reload", "Recarregar"}},
  {"btn.editar", {"Editar fichero de trucos", "Edit cheats file", "Editar arquivo de trapaças"}},
  {"tr.titulo", {"Trucos", "Cheats", "Trapaças"}},
  {"tr.ninguno", {"No hay trucos en nfsmw_trucos.txt. Abre el fichero: dentro se explica cómo añadirlos.", "There are no cheats in nfsmw_trucos.txt. Open the file: it explains how to add them.", "Não há trapaças em nfsmw_trucos.txt. Abra o arquivo: lá dentro explica como adicioná-las."}},
  {"tr.errores", {"Líneas no entendidas en el fichero", "Lines not understood in the file", "Linhas não entendidas no arquivo"}},
  {"tr.nota", {"Los trucos activos se aplican al jugar. Escriben en la memoria del juego: úsalos bajo tu responsabilidad y haz una copia de tu partida antes.", "Active cheats apply when you play. They write to the game's memory: use them at your own risk and back up your save first.", "As trapaças ativas valem ao jogar. Elas escrevem na memória do jogo: use por sua conta e faça uma cópia do seu jogo salvo antes."}},
  {"graf.imagen", {"Imagen", "Image", "Imagem"}},
  {"graf.aa", {"Antialiasing", "Anti-aliasing", "Antisserrilhado"}},
  {"graf.aniso", {"Filtrado anisotrópico", "Anisotropic filtering", "Filtragem anisotrópica"}},
  {"graf.cielo", {"Resplandor del cielo", "Sky glow", "Brilho do céu"}},
  {"graf.filtro", {"Filtro de color del juego", "Game color filter", "Filtro de cor do jogo"}},
  {"graf.posproceso", {"Posproceso", "Post-processing", "Pós-processamento"}},
  {"graf.detalle", {"Detalle y rendimiento", "Detail & performance", "Detalhe e desempenho"}},
  {"graf.sombras_dist", {"Distancia de sombras", "Shadow distance", "Distância das sombras"}},
  {"graf.sombras_res", {"Resolución de sombras", "Shadow resolution", "Resolução das sombras"}},
  {"graf.sombras_veg", {"Sombras de árboles y vegetación", "Tree and foliage shadows", "Sombras de árvores e vegetação"}},
  {"graf.reflejos", {"Reflejos de la carretera", "Road reflections", "Reflexos da pista"}},
  {"graf.coche", {"Reflejos del coche", "Car reflections", "Reflexos do carro"}},
  {"graf.distancia", {"Distancia de dibujado", "Draw distance", "Distância de desenho"}},
  {"graf.reinicio", {"Se aplica al reiniciar", "Applies on restart", "Vale ao reiniciar"}},
  {"val.apagado", {"Desactivado", "Off", "Desativado"}},
  {"val.original", {"Original", "Original", "Original"}},
  {"val.natural", {"Natural", "Natural", "Natural"}},
  {"val.suave", {"Suave", "Soft", "Suave"}},
  {"val.corta", {"Corta", "Short", "Curta"}},
  {"val.media", {"Media", "Medium", "Média"}},
  {"val.normal", {"Normal (como el juego)", "Normal (as the game)", "Normal (como o jogo)"}},
  {"val.alta", {"Alta", "High", "Alta"}},
  {"val.reducida", {"Reducida", "Reduced", "Reduzida"}},
  {"val.completo", {"Completo", "Full", "Completo"}},
  {"val.cada8", {"Cada 8 fotogramas", "Every 8 frames", "A cada 8 quadros"}},
  {"val.cada4", {"Cada 4 fotogramas", "Every 4 frames", "A cada 4 quadros"}},
  {"val.cada2", {"Cada 2 fotogramas", "Every 2 frames", "A cada 2 quadros"}},
  {"val.cada1", {"Cada fotograma", "Every frame", "A cada quadro"}},
  {"pp.cine", {"Cine", "Cinema", "Cinema"}},
  {"pp.sepia", {"Sepia", "Sepia", "Sépia"}},
  {"pp.noir", {"Noir", "Noir", "Noir"}},
  {"pp.frio", {"Frío", "Cold", "Frio"}},
  {"pp.calido", {"Cálido", "Warm", "Quente"}},
  {"pp.vivo", {"Vivo", "Vivid", "Vivo"}},
  {"pp.matrix", {"Matrix", "Matrix", "Matrix"}},
  {"pp.crt", {"CRT", "CRT", "CRT"}},

  // ---- Controles ----------------------------------------------------------------
  {"ctl.teclado", {"Teclado y ratón", "Keyboard & mouse", "Teclado e mouse"}},
  {"ctl.jugar_teclado", {"Jugar con teclado", "Play with keyboard", "Jogar com teclado"}},
  {"ctl.raton", {"Ratón para la cámara (stick derecho)", "Mouse for the camera (right stick)",
                 "Mouse para a câmera (analógico direito)"}},
  {"ctl.sensibilidad", {"Sensibilidad del ratón", "Mouse sensitivity", "Sensibilidade do mouse"}},
  {"ctl.teclado_nota", {"El teclado y el mando funcionan a la vez: sus botones se combinan.",
                        "Keyboard and controller work at the same time; their inputs are combined.",
                        "Teclado e controle funcionam ao mesmo tempo; os comandos são combinados."}},
  {"ctl.mando", {"Mando", "Controller", "Controle"}},
  {"ctl.entrada", {"Sistema de entrada", "Input backend", "Sistema de entrada"}},
  {"ctl.sdl", {"SDL (recomendado)", "SDL (recommended)", "SDL (recomendado)"}},
  {"ctl.guia", {"Botón Guide / PS", "Guide / PS button", "Botão Guide / PS"}},
  {"ctl.mando_nota", {"SDL reconoce sin configurar los mandos de Xbox y PlayStation. Steam o DS4Windows abiertos "
                      "pueden ocultar el mando: ciérralos. XInput solo sirve para mandos de Xbox.",
                      "SDL recognizes Xbox and PlayStation controllers without setup. Steam or DS4Windows can hide "
                      "the controller while open: close them. XInput only works with Xbox controllers.",
                      "O SDL reconhece controles de Xbox e PlayStation sem configurar. Steam ou DS4Windows abertos "
                      "podem esconder o controle: feche-os. XInput só funciona com controles de Xbox."}},
  {"ctl.mapa", {"Mapa del teclado", "Keyboard map", "Mapa do teclado"}},
  {"ctl.boton", {"Botón de Xbox 360", "Xbox 360 button", "Botão do Xbox 360"}},
  {"ctl.tecla", {"Tecla", "Key", "Tecla"}},
  {"ctl.mapa_nota", {"Pulsa una acción y luego la tecla. Con Shift pulsado se guarda como Shift + tecla.", "Click an action, then press the key. Holding Shift saves it as Shift + key.", "Clique numa ação e depois aperte a tecla. Com Shift apertado, salva como Shift + tecla."}},
  {"k.espacio", {"Espacio", "Space", "Espaço"}},
  {"k.retroceso", {"Retroceso", "Backspace", "Backspace"}},
  {"k.flechas", {"Flechas", "Arrow keys", "Setas"}},
  {"k.sinasignar", {"Sin asignar", "Unassigned", "Não atribuído"}},
  {"tk.rt", {"Gatillo derecho (RT) · acelerar", "Right trigger (RT) · accelerate", "Gatilho direito (RT) · acelerar"}},
  {"tk.lt", {"Gatillo izquierdo (LT) · frenar", "Left trigger (LT) · brake", "Gatilho esquerdo (LT) · frear"}},
  {"tk.lb", {"Bumper izquierdo (LB)", "Left bumper (LB)", "Bumper esquerdo (LB)"}},
  {"tk.rb", {"Bumper derecho (RB)", "Right bumper (RB)", "Bumper direito (RB)"}},
  {"tk.stickizq", {"Stick izquierdo · girar", "Left stick · steer", "Analógico esquerdo · virar"}},
  {"tk.stickder", {"Stick derecho · cámara", "Right stick · camera", "Analógico direito · câmera"}},
  {"tk.pulsarizq", {"Pulsar stick izquierdo", "Left stick click", "Clique do analógico esquerdo"}},
  {"tk.pulsarder", {"Pulsar stick derecho", "Right stick click", "Clique do analógico direito"}},
  {"tk.cruceta", {"Cruceta", "D-pad", "Direcional"}},

  // ---- Audio --------------------------------------------------------------------
  {"audio.titulo", {"Sonido", "Sound", "Som"}},
  {"audio.silenciar", {"Silenciar el audio", "Mute audio", "Silenciar o áudio"}},
  {"audio.nota", {"El volumen de la música, los efectos y las voces se ajusta en el menú de opciones del propio "
                  "juego. El sonido sale por el dispositivo predeterminado del sistema.",
                  "Music, effects and voice volume are set in the game's own options menu. Sound plays through "
                  "the system default device.",
                  "O volume da música, efeitos e vozes é ajustado no menu de opções do próprio jogo. O som sai "
                  "pelo dispositivo padrão do sistema."}},

  // ---- Configuracion ------------------------------------------------------------
  {"cfg.idioma", {"Idioma", "Language", "Idioma"}},
  {"cfg.idioma_auto", {"Automático (el del sistema)", "Automatic (system)", "Automático (do sistema)"}},
  {"cfg.inicio", {"Inicio", "Startup", "Inicialização"}},
  {"cfg.lanzador", {"Mostrar este lanzador al iniciar", "Show this launcher at startup",
                    "Mostrar este launcher ao iniciar"}},
  {"cfg.lanzador_nota", {"Si lo desactivas, el juego arrancará directamente. Para recuperarlo, borra nfsmw.toml "
                         "o pon nfsmw_lanzador = true en él.",
                         "If you turn it off, the game will start directly. To bring it back, delete nfsmw.toml "
                         "or set nfsmw_lanzador = true in it.",
                         "Se desativar, o jogo iniciará direto. Para recuperá-lo, apague nfsmw.toml ou coloque "
                         "nfsmw_lanzador = true nele."}},
  {"cfg.carpetas", {"Carpetas", "Folders", "Pastas"}},

  // ---- Acerca de ----------------------------------------------------------------
  {"acerca.texto", {"Recompilación estática nativa de Need for Speed: Most Wanted (2005) para Xbox 360. El código "
                    "PowerPC del juego se traduce a C++ antes de compilar y se ejecuta como código nativo: no es un "
                    "emulador.",
                    "A native static recompilation of Need for Speed: Most Wanted (2005) for Xbox 360. The game's "
                    "PowerPC code is translated to C++ ahead of time and runs as native code: it is not an "
                    "emulator.",
                    "Recompilação estática nativa de Need for Speed: Most Wanted (2005) para Xbox 360. O código "
                    "PowerPC do jogo é traduzido para C++ antes de compilar e roda como código nativo: não é um "
                    "emulador."}},
  {"acerca.base", {"Gracias a StevensND por el renderizador nativo en Vulkan (nfsmw-nx), base de este port. Construido sobre ReXGlue.", "Thanks to StevensND for the native Vulkan renderer (nfsmw-nx) this port is built on. Built on ReXGlue.", "Obrigado ao StevensND pelo renderizador nativo em Vulkan (nfsmw-nx), base deste port. Construído sobre o ReXGlue."}},
  {"acerca.legal", {"Necesitas tu propia copia del juego: este proyecto no incluye ni distribuye datos del juego. "
                    "Need for Speed y Most Wanted son marcas de Electronic Arts; este proyecto no está afiliado a "
                    "EA. Código bajo licencia GPLv3.",
                    "You need your own copy of the game: this project does not include or distribute any game "
                    "data. Need for Speed and Most Wanted are trademarks of Electronic Arts; this project is not "
                    "affiliated with EA. Code licensed under GPLv3.",
                    "Você precisa da sua própria cópia do jogo: este projeto não inclui nem distribui dados do "
                    "jogo. Need for Speed e Most Wanted são marcas da Electronic Arts; este projeto não é afiliado "
                    "à EA. Código sob licença GPLv3."}},
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

int DelSistema() {
#ifdef _WIN32
  // Idioma primario = los 10 bits bajos del LANGID. 0x0A espanol, 0x16 portugues.
  switch (GetUserDefaultUILanguage() & 0x3FF) {
    case 0x0A:
      return 0;
    case 0x16:
      return 2;
    default:
      return 1;
  }
#else
  const char* lang = std::getenv("LANG");
  if (lang && std::strncmp(lang, "es", 2) == 0) return 0;
  if (lang && std::strncmp(lang, "pt", 2) == 0) return 2;
  return 1;
#endif
}

constexpr const char* kCodigos[kNumIdiomas] = {"es", "en", "pt"};
constexpr const char* kNombres[kNumIdiomas] = {"Español", "English", "Português"};

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
