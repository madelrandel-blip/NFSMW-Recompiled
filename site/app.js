// Idioma de la web y boton de descarga de la ultima version publicada en GitHub.

const REPO = "madelrandel-blip/NFSMW-Recompiled";

const TEXTOS = {
  en: {
    "nav.features": "Features", "nav.play": "How to play", "nav.req": "Requirements", "nav.blacklist": "Blacklist",
    "hero.tagline": "The 2005 Xbox 360 classic, natively recompiled for PC with a Vulkan renderer.",
    "hero.download": "Download for Windows", "hero.download_one": "Download", "hero.android": "Download for Android", "hero.android_note": "Android: community port by", "hero.version": "Latest version",
    "features.title": "Features",
    "f1.t": "Native code", "f1.d": "The game's PowerPC code is translated to C++ and compiled for your PC. It is not an emulator.",
    "f2.t": "Vulkan renderer", "f2.d": "Draws the game directly with Vulkan instead of emulating the Xbox 360 GPU: far higher frame rates.",
    "f3.t": "PAL Spain and USA", "f3.d": "Pick your .iso or folder and the launcher detects the edition and starts the right executable.",
    "f4.t": "Graphics your way", "f4.d": "Low to Ultra profiles, resolution up to your screen's, 30/60/120 FPS, shadows, reflections, FXAA and the original Xbox 360 settings.",
    "f5.t": "Keyboard, controller and wheel", "f5.d": "Remap keys and buttons from the launcher. Works with steering wheels, pedals and vibration.",
    "f6.t": "Saves and cheats", "f6.d": "Export, import and restore your saved games. Turn on cheats from a text file.",
    "play.title": "How to play",
    "s1.t": "Download and unzip", "s1.d": "Download the .zip and extract the whole folder anywhere.",
    "s2.t": "Open NFSMW Recomp.exe", "s2.d": "It is the only file you need to open. Everything else lives in the data folder.",
    "s3.t": "Pick your game", "s3.d": "Choose your .iso or the extracted game folder (PAL Spain or USA).",
    "s4.t": "Press Play", "s4.d": "The first time, the shaders are built from your disc. It only happens once.",
    "play.av": "If your antivirus blocks the program, it is a false positive: the executables are not signed yet. Add the folder as an exclusion.",
    "req.title": "Requirements",
    "r1": "Windows 10 or 11, 64-bit", "r2": "Graphics card with Vulkan 1.1", "r3": "Your own copy of Need for Speed: Most Wanted (Xbox 360), PAL Spain or USA", "r4": "Controller, keyboard or steering wheel",
    "cred.founder": "Project creator", "cred.android": "Android port", "cred.renderer": "Vulkan 1.1 renderer", "foot.thanks": "Vulkan 1.1 renderer by Madeline_dev and Camille LaVey. Built on ReXGlue.",
    "bl.title": "The Blacklist", "bl.intro": "Rockport's 15 most wanted drivers. Beat them one by one, from 15 to 1, to win back your BMW M3 GTR.", "foot.art": "Game art: “NFSMW: Upscaled HUD” by LunicAura106", "foot.art_ea": "Original art © Electronic Arts.",
    "foot.legal": "Need for Speed and Most Wanted are trademarks of Electronic Arts. This project is not affiliated with EA or Microsoft. Code under the GPL-3.0 license."
  },
  fr: {
    "nav.features": "Fonctionnalités", "nav.play": "Comment jouer", "nav.req": "Configuration", "nav.blacklist": "Liste noire",
    "hero.tagline": "Le classique Xbox 360 de 2005, recompilé nativement pour PC avec un moteur de rendu Vulkan.",
    "hero.download": "Télécharger pour Windows", "hero.download_one": "Télécharger", "hero.android": "Télécharger pour Android", "hero.android_note": "Android : portage communautaire par", "hero.version": "Dernière version",
    "features.title": "Fonctionnalités",
    "f1.t": "Code natif", "f1.d": "Le code PowerPC du jeu est traduit en C++ et compilé pour votre PC. Ce n'est pas un émulateur.",
    "f2.t": "Rendu Vulkan", "f2.d": "Affiche le jeu directement avec Vulkan au lieu d'émuler le GPU de la Xbox 360 : bien plus d'images par seconde.",
    "f3.t": "PAL Espagne et USA", "f3.d": "Choisissez votre .iso ou votre dossier : le lanceur détecte l'édition et ouvre le bon exécutable.",
    "f4.t": "Graphismes à votre goût", "f4.d": "Profils de Bas à Ultra, résolution jusqu'à celle de votre écran, 30/60/120 FPS, ombres, reflets, FXAA et réglages d'origine de la Xbox 360.",
    "f5.t": "Clavier, manette et volant", "f5.d": "Réassignez touches et boutons depuis le lanceur. Compatible volants, pédaliers et vibrations.",
    "f6.t": "Sauvegardes et codes", "f6.d": "Exportez, importez et restaurez vos sauvegardes. Activez des codes depuis un fichier texte.",
    "play.title": "Comment jouer",
    "s1.t": "Téléchargez et décompressez", "s1.d": "Téléchargez le .zip et extrayez le dossier complet où vous voulez.",
    "s2.t": "Ouvrez NFSMW Recomp.exe", "s2.d": "C'est le seul fichier à ouvrir. Tout le reste se trouve dans le dossier data.",
    "s3.t": "Choisissez votre jeu", "s3.d": "Sélectionnez votre .iso ou le dossier extrait du jeu (PAL Espagne ou USA).",
    "s4.t": "Appuyez sur Jouer", "s4.d": "La première fois, les shaders sont générés depuis votre disque. Cela n'arrive qu'une fois.",
    "play.av": "Si votre antivirus bloque le programme, c'est un faux positif : les exécutables ne sont pas encore signés. Ajoutez le dossier aux exclusions.",
    "req.title": "Configuration requise",
    "r1": "Windows 10 ou 11, 64 bits", "r2": "Carte graphique compatible Vulkan 1.1", "r3": "Votre propre copie de Need for Speed: Most Wanted (Xbox 360), PAL Espagne ou USA", "r4": "Manette, clavier ou volant",
    "cred.founder": "Création du projet", "cred.android": "Portage Android", "cred.renderer": "Moteur de rendu Vulkan 1.1", "foot.thanks": "Moteur de rendu Vulkan 1.1 de Madeline_dev et Camille LaVey. Construit sur ReXGlue.",
    "bl.title": "La Liste noire", "bl.intro": "Les 15 pilotes les plus recherchés de Rockport. Battez-les un par un, du 15 au 1, pour récupérer votre BMW M3 GTR.", "foot.art": "Illustrations du jeu : « NFSMW: Upscaled HUD » de LunicAura106", "foot.art_ea": "Illustrations originales © Electronic Arts.",
    "foot.legal": "Need for Speed et Most Wanted sont des marques d'Electronic Arts. Ce projet n'est affilié ni à EA ni à Microsoft. Code sous licence GPL-3.0."
  },
  it: {
    "nav.features": "Caratteristiche", "nav.play": "Come giocare", "nav.req": "Requisiti", "nav.blacklist": "Lista nera",
    "hero.tagline": "Il classico Xbox 360 del 2005, ricompilato in modo nativo per PC con un renderer Vulkan.",
    "hero.download": "Scarica per Windows", "hero.download_one": "Scarica", "hero.android": "Scarica per Android", "hero.android_note": "Android: port della community di", "hero.version": "Ultima versione",
    "features.title": "Caratteristiche",
    "f1.t": "Codice nativo", "f1.d": "Il codice PowerPC del gioco viene tradotto in C++ e compilato per il tuo PC. Non è un emulatore.",
    "f2.t": "Renderer Vulkan", "f2.d": "Disegna il gioco direttamente con Vulkan invece di emulare la GPU della Xbox 360: molti più FPS.",
    "f3.t": "PAL Spagna e USA", "f3.d": "Scegli il tuo .iso o la cartella: il launcher riconosce l'edizione e apre l'eseguibile giusto.",
    "f4.t": "Grafica a modo tuo", "f4.d": "Profili da Basso a Ultra, risoluzione fino a quella del tuo schermo, 30/60/120 FPS, ombre, riflessi, FXAA e impostazioni originali della Xbox 360.",
    "f5.t": "Tastiera, controller e volante", "f5.d": "Assegna tasti e pulsanti dal launcher. Compatibile con volanti, pedaliere e vibrazione.",
    "f6.t": "Salvataggi e trucchi", "f6.d": "Esporta, importa e ripristina i tuoi salvataggi. Attiva i trucchi da un file di testo.",
    "play.title": "Come giocare",
    "s1.t": "Scarica ed estrai", "s1.d": "Scarica il .zip ed estrai l'intera cartella dove vuoi.",
    "s2.t": "Apri NFSMW Recomp.exe", "s2.d": "È l'unico file da aprire. Tutto il resto è nella cartella data.",
    "s3.t": "Scegli il tuo gioco", "s3.d": "Seleziona il tuo .iso o la cartella estratta del gioco (PAL Spagna o USA).",
    "s4.t": "Premi Gioca", "s4.d": "La prima volta gli shader vengono generati dal tuo disco. Succede una sola volta.",
    "play.av": "Se l'antivirus blocca il programma, è un falso positivo: gli eseguibili non sono ancora firmati. Aggiungi la cartella alle esclusioni.",
    "req.title": "Requisiti",
    "r1": "Windows 10 o 11 a 64 bit", "r2": "Scheda video compatibile con Vulkan 1.1", "r3": "La tua copia di Need for Speed: Most Wanted (Xbox 360), PAL Spagna o USA", "r4": "Controller, tastiera o volante",
    "cred.founder": "Creazione del progetto", "cred.android": "Port Android", "cred.renderer": "Renderer Vulkan 1.1", "foot.thanks": "Renderer Vulkan 1.1 di Madeline_dev e Camille LaVey. Basato su ReXGlue.",
    "bl.title": "La Lista nera", "bl.intro": "I 15 piloti più ricercati di Rockport. Battili uno a uno, dal 15 all'1, per riprenderti la tua BMW M3 GTR.", "foot.art": "Grafica del gioco: «NFSMW: Upscaled HUD» di LunicAura106", "foot.art_ea": "Grafica originale © Electronic Arts.",
    "foot.legal": "Need for Speed e Most Wanted sono marchi di Electronic Arts. Questo progetto non è affiliato a EA né a Microsoft. Codice con licenza GPL-3.0."
  },
  de: {
    "nav.features": "Funktionen", "nav.play": "So spielst du", "nav.req": "Systemanforderungen", "nav.blacklist": "Blacklist",
    "hero.tagline": "Der Xbox-360-Klassiker von 2005, nativ für den PC rekompiliert, mit einem Vulkan-Renderer.",
    "hero.download": "Für Windows herunterladen", "hero.download_one": "Herunterladen", "hero.android": "Für Android herunterladen", "hero.android_note": "Android: Community-Port von", "hero.version": "Neueste Version",
    "features.title": "Funktionen",
    "f1.t": "Nativer Code", "f1.d": "Der PowerPC-Code des Spiels wird nach C++ übersetzt und für deinen PC kompiliert. Es ist kein Emulator.",
    "f2.t": "Vulkan-Renderer", "f2.d": "Zeichnet das Spiel direkt mit Vulkan, statt die GPU der Xbox 360 zu emulieren: deutlich mehr FPS.",
    "f3.t": "PAL Spanien und USA", "f3.d": "Wähle deine .iso oder deinen Ordner: Der Launcher erkennt die Edition und startet die passende Datei.",
    "f4.t": "Grafik nach deinem Geschmack", "f4.d": "Profile von Niedrig bis Ultra, Auflösung bis zu der deines Bildschirms, 30/60/120 FPS, Schatten, Spiegelungen, FXAA und die originalen Xbox-360-Einstellungen.",
    "f5.t": "Tastatur, Controller und Lenkrad", "f5.d": "Belege Tasten und Knöpfe im Launcher neu. Unterstützt Lenkräder, Pedale und Vibration.",
    "f6.t": "Spielstände und Cheats", "f6.d": "Exportiere, importiere und stelle deine Spielstände wieder her. Aktiviere Cheats über eine Textdatei.",
    "play.title": "So spielst du",
    "s1.t": "Herunterladen und entpacken", "s1.d": "Lade die .zip herunter und entpacke den ganzen Ordner, wohin du willst.",
    "s2.t": "NFSMW Recomp.exe öffnen", "s2.d": "Das ist die einzige Datei, die du öffnen musst. Alles andere liegt im Ordner data.",
    "s3.t": "Spiel auswählen", "s3.d": "Wähle deine .iso oder den entpackten Spielordner (PAL Spanien oder USA).",
    "s4.t": "Auf Spielen drücken", "s4.d": "Beim ersten Start werden die Shader von deiner Disc erzeugt. Das passiert nur einmal.",
    "play.av": "Wenn dein Virenschutz das Programm blockiert, ist das ein Fehlalarm: Die Dateien sind noch nicht signiert. Füge den Ordner als Ausnahme hinzu.",
    "req.title": "Systemanforderungen",
    "r1": "Windows 10 oder 11, 64 Bit", "r2": "Grafikkarte mit Vulkan 1.1", "r3": "Deine eigene Kopie von Need for Speed: Most Wanted (Xbox 360), PAL Spanien oder USA", "r4": "Controller, Tastatur oder Lenkrad",
    "cred.founder": "Projektgründung", "cred.android": "Android-Port", "cred.renderer": "Vulkan-1.1-Renderer", "foot.thanks": "Vulkan-1.1-Renderer von Madeline_dev und Camille LaVey. Basiert auf ReXGlue.",
    "bl.title": "Die Blacklist", "bl.intro": "Die 15 meistgesuchten Fahrer von Rockport. Besiege sie einen nach dem anderen, von 15 bis 1, und hol dir deinen BMW M3 GTR zurück.", "foot.art": "Spielgrafik: „NFSMW: Upscaled HUD“ von LunicAura106", "foot.art_ea": "Originalgrafik © Electronic Arts.",
    "foot.legal": "Need for Speed und Most Wanted sind Marken von Electronic Arts. Dieses Projekt steht in keiner Verbindung zu EA oder Microsoft. Code unter der GPL-3.0-Lizenz."
  },
  ja: {
    "nav.features": "特徴", "nav.play": "遊び方", "nav.req": "動作環境", "nav.blacklist": "ブラックリスト",
    "hero.tagline": "2005年のXbox 360の名作を、Vulkanレンダラーを使いPC向けにネイティブで再コンパイル。",
    "hero.download": "Windows版をダウンロード", "hero.download_one": "ダウンロード", "hero.android": "Android版をダウンロード", "hero.android_note": "Android版：コミュニティによる移植 —", "hero.version": "最新バージョン",
    "features.title": "特徴",
    "f1.t": "ネイティブコード", "f1.d": "ゲームのPowerPCコードをC++に変換し、PC向けにコンパイルしています。エミュレーターではありません。",
    "f2.t": "Vulkanレンダラー", "f2.d": "Xbox 360のGPUをエミュレートせず、Vulkanで直接描画。フレームレートが大幅に向上します。",
    "f3.t": "PAL(スペイン)とUSA", "f3.d": ".isoまたはフォルダーを選ぶと、ランチャーがエディションを判別して正しい実行ファイルを起動します。",
    "f4.t": "自由なグラフィック設定", "f4.d": "低〜ウルトラのプリセット、画面解像度までの解像度、30/60/120 FPS、影、反射、FXAA、Xbox 360の初期設定に対応。",
    "f5.t": "キーボード・コントローラー・ハンドル", "f5.d": "ランチャーでキーやボタンを割り当て可能。ハンドル、ペダル、振動に対応しています。",
    "f6.t": "セーブデータとチート", "f6.d": "セーブデータのエクスポート、インポート、復元が可能。テキストファイルからチートを有効にできます。",
    "play.title": "遊び方",
    "s1.t": "ダウンロードして展開", "s1.d": ".zipをダウンロードし、フォルダーごと好きな場所に展開します。",
    "s2.t": "NFSMW Recomp.exe を開く", "s2.d": "開くのはこのファイルだけです。その他はすべて data フォルダーに入っています。",
    "s3.t": "ゲームを選ぶ", "s3.d": ".isoまたは展開したゲームフォルダー(PAL スペイン版またはUSA版)を選択します。",
    "s4.t": "「プレイ」を押す", "s4.d": "初回のみ、ディスクからシェーダーが生成されます。",
    "play.av": "ウイルス対策ソフトがブロックする場合は誤検知です。実行ファイルはまだ署名されていません。フォルダーを除外に追加してください。",
    "req.title": "動作環境",
    "r1": "Windows 10 / 11(64ビット)", "r2": "Vulkan 1.1 対応のグラフィックカード", "r3": "ご自身でお持ちの Need for Speed: Most Wanted(Xbox 360)、PAL スペイン版またはUSA版", "r4": "コントローラー、キーボード、またはハンドル",
    "cred.founder": "プロジェクト創設", "cred.android": "Android版の移植", "cred.renderer": "Vulkan 1.1 レンダラー", "foot.thanks": "Vulkan 1.1 レンダラーは Madeline_dev と Camille LaVey による。ReXGlue をベースに構築。",
    "bl.title": "ブラックリスト", "bl.intro": "ロックポートで最も危険な15人のドライバー。15位から1位まで一人ずつ倒して、BMW M3 GTR を取り戻そう。", "foot.art": "ゲームアート: LunicAura106 による「NFSMW: Upscaled HUD」", "foot.art_ea": "オリジナルアート © Electronic Arts",
    "foot.legal": "Need for Speed および Most Wanted は Electronic Arts の商標です。本プロジェクトは EA および Microsoft とは関係ありません。コードは GPL-3.0 ライセンスです。"
  },
  pt: {
    "nav.features": "Recursos", "nav.play": "Como jogar", "nav.req": "Requisitos", "nav.blacklist": "Lista Negra",
    "hero.tagline": "O clássico de 2005 do Xbox 360, recompilado de forma nativa para PC com um renderizador Vulkan.",
    "hero.download": "Baixar para Windows", "hero.download_one": "Baixar", "hero.android": "Baixar para Android", "hero.android_note": "Android: port da comunidade feito por", "hero.version": "Última versão",
    "features.title": "Recursos",
    "f1.t": "Código nativo", "f1.d": "O código PowerPC do jogo é traduzido para C++ e compilado para o seu PC. Não é um emulador.",
    "f2.t": "Renderizador Vulkan", "f2.d": "Desenha o jogo diretamente com Vulkan em vez de emular a GPU do Xbox 360: muito mais FPS.",
    "f3.t": "PAL Espanha e USA", "f3.d": "Escolha o seu .iso ou pasta e o iniciador detecta a edição e abre o executável certo.",
    "f4.t": "Gráficos do seu jeito", "f4.d": "Perfis de Baixo a Ultra, resolução até a da sua tela, 30/60/120 FPS, sombras, reflexos, FXAA e os valores originais do Xbox 360.",
    "f5.t": "Teclado, controle e volante", "f5.d": "Atribua teclas e botões no iniciador. Compatível com volantes, pedais e vibração.",
    "f6.t": "Jogos salvos e trapaças", "f6.d": "Exporte, importe e restaure seus jogos salvos. Ative trapaças a partir de um arquivo de texto.",
    "play.title": "Como jogar",
    "s1.t": "Baixe e extraia", "s1.d": "Baixe o .zip e extraia a pasta inteira onde quiser.",
    "s2.t": "Abra NFSMW Recomp.exe", "s2.d": "É o único arquivo que precisa abrir. O resto fica na pasta data.",
    "s3.t": "Escolha o seu jogo", "s3.d": "Selecione o seu .iso ou a pasta extraída do jogo (PAL Espanha ou USA).",
    "s4.t": "Aperte Jogar", "s4.d": "Na primeira vez, os shaders são gerados a partir do seu disco. Só acontece uma vez.",
    "play.av": "Se o antivírus bloquear o programa, é um falso positivo: os executáveis ainda não são assinados. Adicione a pasta como exclusão.",
    "req.title": "Requisitos",
    "r1": "Windows 10 ou 11 de 64 bits", "r2": "Placa de vídeo compatível com Vulkan 1.1", "r3": "Sua própria cópia de Need for Speed: Most Wanted (Xbox 360), PAL Espanha ou USA", "r4": "Controle, teclado ou volante",
    "cred.founder": "Criação do projeto", "cred.android": "Port para Android", "cred.renderer": "Renderizador Vulkan 1.1", "foot.thanks": "Renderizador Vulkan 1.1 de Madeline_dev e Camille LaVey. Construído sobre o ReXGlue.",
    "bl.title": "A Lista Negra", "bl.intro": "Os 15 pilotos mais procurados de Rockport. Vença um por um, do 15 ao 1, para recuperar seu BMW M3 GTR.", "foot.art": "Arte do jogo: «NFSMW: Upscaled HUD» de LunicAura106", "foot.art_ea": "Arte original © Electronic Arts.",
    "foot.legal": "Need for Speed e Most Wanted são marcas da Electronic Arts. Este projeto não é afiliado à EA nem à Microsoft. Código sob a licença GPL-3.0."
  }
};

// El espanol es el texto que ya trae el HTML: se guarda al cargar para poder volver a el.
const ORIGINAL = {};
document.querySelectorAll("[data-t]").forEach((el) => { ORIGINAL[el.dataset.t] = el.textContent; });

let versionActual = "";

// Banderas dibujadas en SVG (las banderas emoji no se ven en Windows).
const BANDERAS = {
  es: '<rect width="30" height="20" fill="#c60b1e"/><rect y="5" width="30" height="10" fill="#ffc400"/>',
  en: '<rect width="30" height="20" fill="#012169"/><path d="M0 0l30 20M30 0L0 20" stroke="#fff" stroke-width="4"/>' +
      '<path d="M0 0l30 20M30 0L0 20" stroke="#c8102e" stroke-width="1.6"/>' +
      '<path d="M15 0v20M0 10h30" stroke="#fff" stroke-width="6"/><path d="M15 0v20M0 10h30" stroke="#c8102e" stroke-width="3.6"/>',
  fr: '<rect width="10" height="20" fill="#002395"/><rect x="10" width="10" height="20" fill="#fff"/><rect x="20" width="10" height="20" fill="#ed2939"/>',
  it: '<rect width="10" height="20" fill="#009246"/><rect x="10" width="10" height="20" fill="#fff"/><rect x="20" width="10" height="20" fill="#ce2b37"/>',
  de: '<rect width="30" height="7" fill="#000"/><rect y="6.6" width="30" height="7" fill="#dd0000"/><rect y="13.3" width="30" height="6.7" fill="#ffce00"/>',
  pt: '<rect width="30" height="20" fill="#009c3b"/><path d="M15 2.5L27.5 10 15 17.5 2.5 10z" fill="#ffdf00"/><circle cx="15" cy="10" r="4.3" fill="#002776"/>',
  ja: '<rect width="30" height="20" fill="#fff"/><circle cx="15" cy="10" r="6" fill="#bc002d"/>'
};

// Orden del desplegable, con el nombre de cada idioma en su propia lengua.
const IDIOMAS = [
  ["es", "Español"], ["en", "English"], ["fr", "Français"], ["it", "Italiano"],
  ["de", "Deutsch"], ["pt", "Português"], ["ja", "日本語"],
];

const bandera = (lang) => `<svg class="bandera" viewBox="0 0 30 20" aria-hidden="true">${BANDERAS[lang]}</svg>`;

const botonIdioma = document.getElementById("idioma-actual");
const listaIdiomas = document.getElementById("idioma-lista");

listaIdiomas.innerHTML = IDIOMAS.map(([lang, nombre]) =>
  `<li role="option" tabindex="0" data-lang="${lang}">${bandera(lang)}<span>${nombre}</span></li>`).join("");

function abrirLista(abrir) {
  listaIdiomas.hidden = !abrir;
  botonIdioma.setAttribute("aria-expanded", String(abrir));
}

function ponerIdioma(lang) {
  const tabla = TEXTOS[lang] || {};
  document.querySelectorAll("[data-t]").forEach((el) => {
    el.textContent = tabla[el.dataset.t] || ORIGINAL[el.dataset.t];
  });
  document.documentElement.lang = lang;
  botonIdioma.innerHTML = `${bandera(lang)}<span>${lang.toUpperCase()}</span>`;
  listaIdiomas.querySelectorAll("li").forEach((li) => li.setAttribute("aria-selected", String(li.dataset.lang === lang)));
  if (versionActual) document.getElementById("version").textContent += ": " + versionActual;
  try { localStorage.setItem("idioma", lang); } catch (e) { /* sin almacenamiento: no pasa nada */ }
}

function idiomaInicial() {
  try {
    const guardado = localStorage.getItem("idioma");
    if (guardado && BANDERAS[guardado]) return guardado;
  } catch (e) { /* sin almacenamiento */ }
  const nav = (navigator.language || "es").slice(0, 2);
  return BANDERAS[nav] ? nav : "en";
}

botonIdioma.addEventListener("click", (e) => {
  e.stopPropagation();
  abrirLista(listaIdiomas.hidden);
});
listaIdiomas.addEventListener("click", (e) => {
  const li = e.target.closest("li");
  if (!li) return;
  ponerIdioma(li.dataset.lang);
  abrirLista(false);
});
listaIdiomas.addEventListener("keydown", (e) => {
  if (e.key === "Enter" && e.target.dataset.lang) { ponerIdioma(e.target.dataset.lang); abrirLista(false); botonIdioma.focus(); }
});
// Boton Descargar: abre el menu para elegir Windows o Android.
const botonDescargar = document.getElementById("boton-descargar");
const menuDescarga = document.getElementById("menu-descarga");

function abrirDescarga(abrir) {
  menuDescarga.hidden = !abrir;
  botonDescargar.setAttribute("aria-expanded", String(abrir));
}

botonDescargar.addEventListener("click", (e) => {
  e.stopPropagation();
  abrirDescarga(menuDescarga.hidden);
});

// Clic fuera o Escape: se cierran los dos menus.
document.addEventListener("click", () => { abrirLista(false); abrirDescarga(false); });
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape") { abrirLista(false); abrirDescarga(false); }
});

ponerIdioma(idiomaInicial());

// Ultima release: el boton apunta directo al .zip y se muestra el numero de version.
fetch(`https://api.github.com/repos/${REPO}/releases/latest`)
  .then((r) => (r.ok ? r.json() : null))
  .then((rel) => {
    if (!rel) return;
    const zip = (rel.assets || []).find((a) => a.name.endsWith(".zip"));
    if (zip) document.getElementById("descargar").href = zip.browser_download_url;
    versionActual = rel.tag_name;
    document.getElementById("version-pc").textContent = ".zip · " + versionActual;
    document.getElementById("version").textContent += ": " + versionActual;
  })
  .catch(() => { /* sin red: el boton sigue llevando a la pagina de releases */ });

// Android: el port de la comunidad de victorgbd. El boton apunta directo a su ultimo .apk.
fetch("https://api.github.com/repos/victorgbd/NFSMW-Recompiled-Mobile/releases/latest")
  .then((r) => (r.ok ? r.json() : null))
  .then((rel) => {
    const apk = rel && (rel.assets || []).find((a) => a.name.endsWith(".apk"));
    if (apk) document.getElementById("descargar-android").href = apk.browser_download_url;
  })
  .catch(() => { /* sin red: lleva a la pagina de releases del fork */ });
