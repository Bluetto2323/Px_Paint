# PxPaint — Blocco 10 (selezione) sul motore Paint Engine

App di pittura nativa Android (Kotlin + C++/NDK, Vulkan con fallback OpenGL ES 3.x).

* **Blocco 1**: il motore — selezione backend, rilevamento hardware, ciclo di
  vita, diagnostica, scena di prova (triangolo). Ora vive in
  `DiagnosticsActivity` (processo `:diagnostics`), aperta da
  *Impostazioni → Diagnostica del motore*.
* **Blocco 2**: l'app **PxPaint** — galleria progetti, nuova tela, area di
  lavoro con tela GPU (zoom/pan/rotazione esatti, texture singola o a tile
  secondo hardware e memoria), impostazioni, layout adattivi.
* **Blocco 3**: **brush engine** nativo (`app/src/main/cpp/brush/`):
  tratti continui (spline Catmull-Rom centripeta attraverso ogni campione),
  tocchi rotondi antialias a spaziatura costante, opacità esatta (maschera
  di copertura per tratto, niente scurimenti nelle sovrapposizioni),
  gomma, annullamento del tratto. La tela è un raster CPU a tile
  (fonte di verità) copiato sulla GPU solo nelle regioni modificate; su
  Vulkan gli upload sono registrati nel frame (nessun submit+attesa).
  Salvataggio a tile compressi (`content.pxtiles`), import dei progetti del
  Blocco 2. Pressione/stilo, livelli e undo sono fuori scope (Blocco 4+).
  Dettagli: `docs/ENGINE_API.md`, `docs/BLOCK3_REPORT.md`.
* **Blocco 4**: **stilo e pressione**. Dito, stilo e gomma dello stilo
  distinti per tipo di puntatore; assi letti solo se il dispositivo li
  dichiara (pressione normalizzata, inclinazione, orientamento, distanza);
  pressione filtrata (1-euro), curva di risposta configurabile,
  dimensione/opacità minima, inclinazione → pennello ellittico; rifiuto del
  palmo; modalità del dito (disegna / sposta / solo gesti); schermata
  *Stilo e pressione* con editor della curva e area di prova. Il dito
  disegna esattamente come nel Blocco 3. Dettagli: `docs/BLOCK4_REPORT.md`.
* **Blocco 5**: **libreria pennelli**. 12 preset definiti come dati
  (`assets/brushes/builtin.pxb`), resi dallo stesso motore con un modello
  esteso: durezza, rotondità/angolo, punte procedurali (gesso, carboncino,
  setole), grana della carta legata alla tela, variazioni casuali
  deterministiche, accumulo, bordo bagnato. Libreria con categorie,
  preferiti, recenti, ricerca, dimensione/opacità rapide; duplica, rinomina,
  elimina, ripristina; editor con anteprime disegnate dal motore e area di
  prova. Dettagli: `docs/BRUSHES.md`, `docs/BLOCK5_REPORT.md`.
* **Blocco 6**: **livelli**.
  * Ogni livello ha i propri tile (id persistente, nome, visibilità, blocco,
    opacità, modalità di fusione).
  * La composizione avviene su CPU, identica su Vulkan e GLES: 8 modalità W3C
    che si fondono anche con la carta, aggiornamento per regioni modificate.
  * Operazioni: aggiungi, elimina, duplica, rinomina, sposta, nascondi, blocca,
    opacità, modalità, unisci in basso, unisci tutto.
  * Nuovo formato di progetto a livelli; le tele precedenti vengono migrate.
  * Pannello Livelli con miniature.
  * Dettagli: `docs/LAYERS.md`, `docs/BLOCK6_REPORT.md`.
* **Blocco 7**: **annulla / ripeti**.
  * Cronologia nel motore, indipendente dall'interfaccia: un comando per
    operazione, con `undo()` e `redo()` esatti.
  * Tratti e gomma: per ogni tile toccato, solo il rettangolo modificato,
    salvato come XOR prima/dopo compresso (RLE) e verificato con hash prima
    di ogni ripristino. Il risultato è identico bit per bit, alpha compreso,
    senza istantanee della tela.
  * Tutte le operazioni sui livelli sono annullabili: un livello eliminato
    torna con lo stesso id e contenuto; unisci in basso e unisci tutto sono
    reversibili.
  * Budget di memoria (1/32 della RAM) e limite di 100 passi: i più vecchi
    vengono scartati.
  * Pulsanti ↶ ↷ attivi o disattivati, pannello Cronologia.
  * Dettagli: `docs/HISTORY.md`, `docs/BLOCK7_REPORT.md`.
* **Blocco 8**: **nuova interfaccia per telefoni e tablet**.
  * Layout scelto dalla finestra reale (non dal modello del dispositivo):
    * telefono verticale: barra in basso e pannelli dal basso;
    * telefono orizzontale: colonna laterale e pannelli laterali;
    * tablet: una barra in alto, pannelli sospesi oppure un pannello fissato
      e ridimensionabile.
  * Cursori di dimensione e opacità sul bordo, con anteprima reale.
  * Spazio colore: ruota, HSV, RGB, HEX, recenti, tavolozza, contagocce.
  * Gesti configurabili: due dita annulla, tre ripeti, quattro nasconde
    l'interfaccia, pizzico rapido adatta.
  * Token di design con contrasto WCAG verificato.
  * Dettagli: `docs/UI.md`, `docs/BLOCK8_REPORT.md`.
* **Blocco 9**: **animazioni e identità visiva**.
  * Splash di sistema con il logo PxPaint e animazione d'avvio (tile, tratto,
    nome, luce), che non ritarda mai l'avvio.
  * Transizioni tra schermate; il progetto si apre crescendo dalla sua scheda.
  * Pannelli, livelli (FLIP), pennelli, colori.
  * Indicatori di caricamento e di salvataggio.
  * Impostazione Animazioni: complete, ridotte o disattivate.
  * Dettagli: `docs/MOTION.md`, `docs/BLOCK9_REPORT.md`.
* **Blocco 10**: **selezione**.
  * Rettangolo, ellisse, lazo, poligono, selezione dal contenuto visibile;
    bordi antialias.
  * Nuova, aggiungi, sottrai, interseca, inverti, seleziona tutto,
    deseleziona.
  * Maschera nativa a tile (vuoti / pieni senza memoria), indipendente dai
    livelli; pennello e gomma restano dentro la selezione.
  * Svuota, copia, copia unito, taglia, incolla e duplica in un nuovo
    livello; spostamento dei pixel selezionati.
  * Ogni operazione è un passo della cronologia (pixel e selezione).
  * Formiche in marcia, area esterna scurita, barra delle azioni contestuale.
  * Dettagli: `docs/SELECTION.md`, `docs/BLOCK10_REPORT.md`.

L'interfaccia parla col motore **solo** attraverso `NativeEngine`
(documentata in `docs/ENGINE_API.md`).

## Struttura

```
app/src/main/
  java/com/paintengine/
    pxpaint/PxPaintActivity.kt     launcher: pila di schermate, motore unico, impostazioni
    pxpaint/input/                 lettura assi dello stilo, rilevamento stili
    pxpaint/core/                  Kotlin puro (test JUnit): impostazioni, regole
                                   dimensioni tela, archivio progetti, ricerca,
                                   layout adattivi, gesti, testi IT/EN
    pxpaint/ui/                    tema, icone vettoriali in codice, widget,
                                   selettore colore, overlay, miniature
    pxpaint/screens/               galleria, nuova tela, area di lavoro, impostazioni
    app/DiagnosticsActivity.kt     diagnostica Blocco 1 (processo :diagnostics)
    engine/NativeEngine.kt         facciata JNI (nessuna chiamata GPU in Kotlin)
    engine/EngineSurfaceView.kt    SurfaceView -> eventi del ciclo di vita
  cpp/
    core/                          log, coda comandi thread-safe, tempo
    platform/                      PlatformWindow (Android / headless)
    graphics/abstraction/          API indipendente dal backend
      GraphicsBackend, GraphicsDevice, GraphicsCapabilities, RenderingContext,
      RenderTarget, Texture/Buffer/Shader/PipelineResource, ResourceManager,
      FrameScheduler
    graphics/BackendSelector.*     UNICO punto di scelta backend + fallback
    graphics/vulkan/               loader dinamico, device, swapchain, risorse
    graphics/opengles/             EGL + GLES 3.0 (3.1/3.2 rilevati a runtime)
    diagnostics/                   report GPU, stato live, auto-test
    brush/                         brush engine: BrushDynamics (pressione, curva,
                                   inclinazione, filtro), StrokePath (spline + spaziatura),
                                   StrokeCompositor (dab antialias, maschera, base),
                                   BrushEngine (coda input, ciclo di vita del tratto)
    engine/                        Engine (render thread), TestScene, CanvasController,
                                   CanvasDocument (raster + pennello, thread-safe)
    canvas/                        Viewport, TileGrid, StoragePolicy, RasterStore,
                                   CanvasSurface, CanvasRenderer, CanvasSelfTest,
                                   SelectionMask (maschera di selezione a tile)
    jni/                           JniBridge
    shaders/                       sorgenti GLSL + header SPIR-V generato
tools/
  compile_shaders.sh               rigenera shaders/generated/TriangleShaders.h
  host_test/                       verifica del motore su Linux (headless)
```

## Regole architetturali

* **Un solo thread GPU.** Il motore ha un render thread nativo che possiede
  tutte le risorse grafiche. JNI/Kotlin inviano solo comandi.
* **`surfaceDestroyed` è sincrono e senza timeout**: il thread UI revoca la
  superficie (nessun nuovo frame), il render thread termina l'eventuale frame
  in corso, distrugge swapchain/EGLSurface, *poi* rilascia l'`ANativeWindow`
  e segnala il completamento (anche in caso di eccezione). Se il render thread
  termina, il suo teardown esegue lo stesso rilascio ordinato. Un watchdog
  registra un avviso ogni secondo di attesa ma non abbandona mai l'attesa.
  Il render thread non aspetta mai il thread UI: nessun deadlock possibile.
  Dettagli in `docs/BLOCK1_REVIEW.md`.
* **Il device sopravvive alla perdita della superficie**: in background si
  distrugge solo la swapchain; shader, buffer e pipeline restano validi.
* **Distruzione differita**: una risorsa rilasciata viene distrutta solo quando
  la GPU ha completato il frame che poteva usarla. Allo shutdown il
  `ResourceManager` segnala ogni leak.
* **Nessuna assunzione sull'hardware**: Vulkan è caricato con `dlopen`; si
  richiede 1.1 solo se il loader lo supporta; estensioni e funzionalità sono
  usate solo dopo verifica. Nessuna funzionalità opzionale è abilitata finché
  non serve.
* **Selezione backend** (`BackendSelector`): Vulkan → OpenGL ES. Un candidato è
  accettato solo se supera inizializzazione, creazione device, requisiti minimi
  verificati e creazione reale della swapchain/superficie. Device lost,
  context lost, errori ripetuti o eccezioni del render thread → ricreazione
  del device (le risorse della scena vengono ricreate). Due fallimenti dello
  stesso backend entro 60 s lo escludono **solo se resta un'alternativa**;
  più di 6 recuperi in 60 s, o 3 eccezioni consecutive, disattivano la
  grafica in modo pulito (l'app resta viva e il ciclo di vita funziona).
* **Modalità di rendering e diagnostica sono indipendenti**:
  `continuous` (un frame per vsync) oppure `on-demand` (default: un frame solo
  su richiesta/eventi); la diagnostica (log prestazioni periodico, auto-test
  pixel) funziona in entrambe e non forza mai il rendering continuo.

## Build automatica (GitHub Actions) e installazione dal telefono

La workflow `.github/workflows/android.yml` parte a ogni push su `main` (o a
mano da *Actions → Android build → Run workflow*) e:

1. installa sul runner Android SDK 35, build-tools 35.0.0, NDK 27.2.12479018 e
   CMake 3.22.1 ufficiali;
2. esegue `./gradlew :app:assembleDebug` (Kotlin + C++ per `arm64-v8a` e
   `x86_64`), poi i test unitari JVM (`testDebugUnitTest`) e il lint dei
   livelli API (`NewApi`/`InlinedApi`, minSdk 26);
3. verifica l'APK (`tools/ci/inspect_apk.sh`): ABI presenti, tutte le funzioni JNI di `NativeEngine.kt`
   esportate, allineamento 16 KB, `libvulkan.so` **non** collegata (dlopen),
   Vulkan dichiarato opzionale, firma valida, launcher `PxPaintActivity`,
   diagnostica nel processo `:diagnostics`;
4. pubblica l'APK come artifact `PxPaint-debug-apk-<commit>` (30 giorni);
5. in un job separato esegue i test host del motore su Mesa (GLES) e
   lavapipe (Vulkan software): test CPU `pe_canvas_test` e `pe_brush_test`,
   poi le suite `basic`, `lifecycle`, `faults`, `canvas`, `brush` e `perf`
   su entrambi i backend.

Installare dal telefono:

1. Apri il repository nel browser (il sito, non l'app GitHub: l'app non
   scarica gli artifact) → **Actions** → ultima esecuzione verde di
   *Android build* → sezione **Artifacts** → `PxPaint-debug-apk-…`.
2. Si scarica uno **.zip**: aprilo con *File* / *I miei file* di Samsung ed
   estrai `PxPaint-block5-debug-….apk`.
3. Tocca l'APK. La prima volta Android chiede di consentire l'installazione
   da quell'app (*Impostazioni → Installa app sconosciute*): consentila solo
   per *I miei file* / il browser usato.
4. Play Protect può avvisare che l'app non è nota: *Installa comunque*.
5. Ogni esecuzione CI firma con una chiave debug nuova: per installare una
   build più recente sopra una vecchia **disinstalla prima** la precedente
   (altrimenti "app non installata", firma diversa).

Leggere i log senza PC: il pulsante **LOG** scrive il report in logcat, che
dal telefono richiede un PC o un'app con permesso `READ_LOGS`; l'overlay
mostra comunque stato, backend, GPU, versione API, driver, superficie, tempi
frame, auto-test ed errori recenti — fai uno screenshot dell'overlay.

## Compilare e installare sul Samsung Galaxy A33 5G

Requisiti: Android Studio (Ladybug o successivo), SDK Platform 35,
**NDK 27.2.12479018** e **CMake 3.22.1** (SDK Manager → SDK Tools →
"Show Package Details"). Se hai un NDK diverso, cambia `ndkVersion` in
`app/build.gradle.kts`.

1. Apri la cartella `PaintEngine` in Android Studio e attendi il Gradle sync.
2. Sul telefono: *Impostazioni → Info sul telefono → Informazioni software*,
   tocca 7 volte *Numero build*; poi *Opzioni sviluppatore → Debug USB* attivo.
3. Collega il telefono via USB, accetta l'autorizzazione, premi **Run**.

Da riga di comando:

```bash
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.paintengine/.pxpaint.PxPaintActivity      # PxPaint
adb shell am start -n com.paintengine/.app.DiagnosticsActivity      # diagnostica Blocco 1
adb logcat -s 'PE.*'          # tutti i log del motore
```

Forzare un backend / disattivare il fallback / scegliere le modalità (solo
diagnostica; in PxPaint si sceglie da *Impostazioni → Prestazioni*):

```bash
adb shell am start -S -n com.paintengine/.app.DiagnosticsActivity --es backend vulkan --ez fallback false
adb shell am start -S -n com.paintengine/.app.DiagnosticsActivity --es backend gles
adb shell am start -S -n com.paintengine/.app.DiagnosticsActivity --es render continuous   # default: ondemand
adb shell am start -S -n com.paintengine/.app.DiagnosticsActivity --ez diagnostics false
```

Pulsanti: **MODE** (continuo ↔ on-demand), **DIAG** (diagnostica on/off,
non cambia la modalità), **SELF-TEST**, **LOG** (report completo in logcat).

### Cosa verificare sul dispositivo (diagnostica Blocco 1)

Per PxPaint (Blocchi 2, 3 e 4) vedi `docs/DEVICE_VALIDATION.md`.

| Test | Come | Atteso |
|---|---|---|
| Avvio + backend | apri l'app | overlay `State Running`, `Backend Vulkan`, GPU `Mali-G68` |
| Triangolo | guarda lo schermo | triangolo rosso (alto) / verde (sx) / blu (dx) su grigio scuro |
| Auto-test pixel | riga `SelfTest` | `PASSED - 8/8 probes match` |
| Report GPU | pulsante LOG REPORT, `adb logcat -s PE.Diagnostics` | estensioni, limiti, formati, subgroup size 16 |
| Ridimensionamento | ruota il telefono / split-screen | `Surface` cambia dimensione, `rebuilt` aumenta, triangolo corretto |
| Pausa/ripresa | tasto Home e rientra | `Frames` fermo in background, riparte al rientro, auto-test di nuovo PASSED |
| Rilascio superficie | Home, poi `adb logcat -s PE.SurfaceView` | `surfaceDestroyed: native surface released in N ms` (N tipicamente < 50) |
| Modalità | MODE con DIAG attivo | on-demand: `Frames` fermo; continuous: ~60/90/120 fps secondo il display |
| Perdita superficie | blocca/sblocca lo schermo | nessun crash, rendering riprende |
| Fallback GLES | `--es backend gles` | `Backend OpenGL ES`, auto-test PASSED |
| Leak | chiudi l'app (Back) | logcat: `Shutdown clean: created=N destroyed=N` |

Nota: con dispositivo ruotato e backend Vulkan il compositore ruota l'immagine
(pre-rotazione non ancora implementata); il driver può riportare
`SUBOPTIMAL`, che è gestito senza ricreazioni continue.

### Validation layer Vulkan (opzionale)

Nelle build debug il motore chiede `VK_LAYER_KHRONOS_validation`. Se non è
presente registra solo un avviso. Per attivarlo, scarica
`android-binaries-*.zip` dalle release di
[KhronosGroup/Vulkan-ValidationLayers](https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases)
e copia `arm64-v8a/libVkLayer_khronos_validation.so` in
`app/src/main/jniLibs/arm64-v8a/`.

## Shader

Gli shader Vulkan sono precompilati in SPIR-V 1.0 dentro
`shaders/generated/TriangleShaders.h`. Dopo una modifica:

```bash
tools/compile_shaders.sh /percorso/glslangValidator
```

## Verifica su host (senza dispositivo)

`tools/host_test` compila **gli stessi sorgenti del motore** (senza JNI) per
Linux ed esegue il ciclo di vita completo su una finestra headless:
OpenGL ES su Mesa llvmpipe, Vulkan su SwiftShader. La build di test abilita
`PE_ENABLE_TEST_HOOKS` (`core/TestHooks.h`) per allargare finestre temporali e
iniettare guasti; nella build Android gli hook non esistono.
Suite: `basic`, `lifecycle`, `faults`, `canvas`, `brush`, `perf`; più i
test CPU `pe_canvas_test` e `pe_brush_test`. Vedi
`tools/host_test/run_host_tests.sh` (`--asan` per AddressSanitizer+UBSan).
