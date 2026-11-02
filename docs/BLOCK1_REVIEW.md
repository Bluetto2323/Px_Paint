# Blocco 1 — Revisione tecnica e hardening

Revisione del motore esistente: nessuna riscrittura, nessuna modifica alle
interfacce grafiche pubbliche (`graphics/abstraction/` invariata), nessuna
funzionalità del Blocco 2.

## 1. Protocollo `surfaceDestroyed`

### Problemi trovati

| # | Problema | Conseguenza |
|---|---|---|
| S1 | `surfaceDestroyed()` usava `postAndWait` con timeout di 3 s e **ritornava anche se il rilascio non era avvenuto**. | Con il render thread bloccato oltre 3 s (driver, vsync, fence) Android distruggeva la `Surface` mentre swapchain/EGLSurface la usavano ancora: use-after-free nel driver. |
| S2 | Il render thread non sapeva che la superficie stava per sparire: continuava a iniziare nuovi frame fino all'esecuzione del task. | Lavoro GPU inutile su una superficie in uscita e attesa più lunga sul thread UI. |
| S3 | Un'eccezione in un task della `TaskQueue` interrompeva il batch: i task successivi (incluso un eventuale rilascio) andavano persi. | Il thread UI poteva restare in attesa di un rilascio mai eseguito. |
| S4 | Eccezioni nel render loop non gestite → `std::terminate`. | Crash dell'app; finestra mai rilasciata. |
| S5 | `surfaceCreated` con una finestra ancora presente faceva `window_ = nuova` **prima** di staccare la vecchia. | `ANativeWindow_release` della vecchia finestra con la swapchain ancora collegata. |
| S6 | Una chiamata dopo/durante lo shutdown (`running_` già falso) ritornava subito, anche mentre il teardown era in corso. | Finestra potenzialmente ancora in uso. |
| S7 | Eccezioni C++ potevano attraversare il confine JNI. | Abort del processo. |

### Nuovo protocollo

1. Thread UI: `surfaceRevoked_ = true` → il render loop non inizia più frame
   su quella superficie (un frame già in corso termina: la `Surface` resta
   valida finché la callback Android non ritorna).
2. Thread UI: pubblica il task di rilascio e sveglia il render thread.
3. Render thread: `detachSurface()` (vkDeviceWaitIdle/glFinish, poi
   swapchain+VkSurfaceKHR o EGLSurface distrutti) → **poi** rilascio
   dell'`ANativeWindow` → segnale di completamento (RAII, anche su eccezione).
   Se il distacco fallisce, la reference della finestra viene volutamente
   *trattenuta* (leak) invece di rilasciarla sotto una superficie viva.
4. Thread UI: attende **senza timeout** il segnale *oppure* l'uscita del
   render thread (il cui teardown fa lo stesso rilascio ordinato). Un watchdog
   registra un avviso ogni secondo; non abbandona mai.

Deadlock: il render thread non chiama mai il thread UI (nessuna upcall JNI) e
non tiene mai un lock condiviso durante un'attesa; il thread UI tiene
`lifecycleMutex_` solo dentro `condition_variable::wait`. `surfaceDestroyed`
chiamata dal render thread stesso esegue il rilascio inline; `shutdown()`
dal render thread viene rifiutata (eviterebbe un self-join).

Limiti di blocco del render thread (quindi dell'attesa UI): acquire Vulkan
100 ms (prima 1 s), fence 3 s (oltre = device lost), `eglSwapBuffers`
limitato dal driver.

## 2. Audit Vulkan

| # | Problema | Correzione |
|---|---|---|
| V1 | Fence resettato in `beginFrame` prima del submit: un errore tra acquire e submit (o un frame abbandonato) lasciava un fence mai segnalato → al giro dopo attesa di 3 s e **falso DEVICE_LOST**. | Reset del fence immediatamente prima di `vkQueueSubmit`. |
| V2 | Dopo un frame abbandonato/submit fallito il semaforo di acquire restava segnalato e veniva riusato in `vkAcquireNextImageKHR` (uso non valido). | `recycleFrameSync()`: dopo idle ricrea fence (segnalato) e semaforo, marca la swapchain da ricostruire (l'immagine acquisita non presentata viene restituita). Usato anche da `detachSurface` dopo un frame abbandonato. |
| V3 | Il seriale del frame veniva assegnato allo slot prima del submit: un submit fallito faceva avanzare `completedSerial_` e il `ResourceManager` poteva distruggere risorse ancora usate dal frame precedente in volo. | Seriale assegnato solo dopo un submit riuscito. |
| V4 | Le ricostruzioni della swapchain distruggevano i semafori "render finished", che il presentation engine può ancora attendere (senza `VK_EXT_swapchain_maintenance1` non è osservabile). | Semafori riusati tra le ricostruzioni, distrutti solo con la swapchain (dopo idle). |
| V5 | Una ricostruzione fallita a metà (view/framebuffer) lasciava una swapchain "valida" con framebuffer mancanti → indicizzazione fuori limiti. | Ogni errore dopo `vkCreateSwapchainKHR` distrugge tutto (`valid()` falso, nuovo tentativo al frame successivo). |
| V6 | `vkDeviceWaitIdle` prima della ricostruzione ignorava `VK_ERROR_DEVICE_LOST`. | Propagato come `DeviceLost`. |
| V7 | Acquire con timeout 1 s rendeva lento `surfaceDestroyed`. | 100 ms (un timeout salta solo un frame). |

Verificati e corretti già prima: semafori render-finished per immagine,
OUT_OF_DATE (swapchain marcata e ricostruita al frame dopo, la attesa del
semaforo di present viene eseguita per specifica), SUBOPTIMAL accettato
(pre-rotazione non implementata), SURFACE_LOST → distacco e nuovo tentativo,
resize con device idle e pipeline invariata (viewport dinamico).

Ordine di distruzione verificato: framebuffer → image view → swapchain →
semafori → VkSurfaceKHR → (Engine) ANativeWindow; risorse (pipeline, buffer,
shader) → fence/semafori/command pool → render pass → pipeline cache →
VkDevice → debug messenger → VkInstance.

## 3. Audit OpenGL ES

| # | Problema | Correzione |
|---|---|---|
| G1 | Nessun rilevamento di reset GPU oltre a `EGL_CONTEXT_LOST` su `eglSwapBuffers`. | Se disponibile `EGL_EXT_create_context_robustness`: contesto con `LOSE_CONTEXT_ON_RESET` e controllo `glGetGraphicsResetStatus` (KHR/EXT) a fine frame. Tentativi di creazione in cascata (debug+robust → robust → debug → base). |
| G2 | Dopo la perdita del contesto il device continuava a chiamare GL (glFinish ecc.). | Stato `contextLost_` latched: `beginFrame` restituisce subito `DeviceLost`, nessun glFinish su contesto perso. |
| G3 | Mancava `eglReleaseThread` alla distruzione del device. | Aggiunto dopo aver tolto il contesto dal thread. |
| G4 | Errori di `eglMakeCurrent`/`eglDestroySurface` in distruzione non registrati. | Registrati come avvisi. |
| G5 | Frame abbandonato (eccezione) lasciava `inFrame_` vero. | Reset in `beginFrame`/`detachSurface`. |

Verificati: `EGLDisplay` mai terminato su Android (condiviso con HWUI),
contesto sempre e solo corrente sul render thread, EGLSurface resa non
corrente prima di distruggerla, `glFinish` prima della distruzione.
Ripristino dopo perdita del contesto: l'Engine distrugge il device (il
`ResourceManager` rilascia tutto), ne crea uno nuovo e la scena ricrea
buffer/shader/pipeline al primo frame.

## 4. Modalità di rendering

Prima: "diagnostica" = rendering continuo forzato. Ora tre impostazioni
esplicite e indipendenti in `EngineConfig` / JNI / Kotlin:

* `renderMode`: `Continuous` o `OnDemand` (default `OnDemand`);
* `diagnostics`: log prestazioni ogni 5 s + auto-test pixel dopo ogni attach;
* `autoSelfTest` (con diagnostica).

API: `setRenderMode()`, `setDiagnosticsEnabled()`. Pulsanti MODE e DIAG
nell'activity di test. Corretto anche uno snapshot di stato che poteva
restare vecchio in on-demand (pubblicazione limitata a 4/s e nessuna
pubblicazione finale prima di andare in idle).

## 5. Altri problemi trovati dai nuovi test

| # | Problema | Correzione |
|---|---|---|
| E1 | Fallimenti di un backend sommati per tutta la sessione ed esclusione permanente, **anche dell'unico backend disponibile**: dopo un'eccezione e, più tardi, una perdita di device, il motore restava senza grafica per sempre. | Fallimenti dimenticati dopo 60 s; l'ultimo backend rimasto non viene mai escluso; più di 6 recuperi in 60 s → grafica disattivata in modo pulito invece di un ciclo infinito. |

## 6. Hook di test

`core/TestHooks.h`: `PE_TEST_HOOK(point)` esiste solo con
`PE_ENABLE_TEST_HOOKS` (build host). Nella build Android è la costante
`false`: nessun costo e nessuna possibilità di attivazione.
Punti: `engine.frame.recorded`, `engine.frame.throw`,
`engine.frame.device_lost`, `surface.rebuild`.
