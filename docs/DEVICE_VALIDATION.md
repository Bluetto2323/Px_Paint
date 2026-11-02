# Blocco 1 — Validazione su dispositivo fisico

Procedura pensata per il **solo telefono** (niente PC, niente `adb`): l'app
raccoglie da sé le prove (cronologia del ciclo di vita, log nativo, risultati)
in un **REPORT** da condividere.

## Cosa fa l'app

| Pulsante | Funzione |
|---|---|
| MODE | rendering continuo ↔ on-demand |
| DIAG | overlay diagnostico on/off (non cambia la modalità) |
| SELF-TEST | verifica pixel + geometria del prossimo frame |
| BACKEND | AUTO → VULKAN → GLES: riavvio completo del motore sulla stessa superficie |
| DEVICE TEST | test automatico (1–2 minuti), vedi sotto |
| REPORT | report testuale: SHARE (condividi) / COPY (copia) / CLOSE |

### DEVICE TEST (automatico, usa il vero ciclo di vita Android)

| Sezione | Cosa verifica |
|---|---|
| A | motore attivo, backend/GPU/driver, tempi di avvio, auto-test |
| B | 5 s di rendering continuo: frame renderizzati, nessun frame perso per errori, 3 auto-test ripetuti |
| C | 10 cicli SurfaceView GONE→VISIBLE: Android distrugge e ricrea la superficie; ogni volta rilascio, ricollegamento, auto-test; durata massima di `surfaceDestroyed`; nessuna crescita di risorse GPU |
| D | 3 ridimensionamenti reali della superficie + ripristino: auto-test con controllo geometria (triangolo non deformato) |
| E | 20 pausa/ripresa: nessun frame in pausa, ripresa corretta |
| F | on-demand inattivo e senza FPS, frame su richiesta, ritorno al continuo con FPS misurati |
| G | cambio backend GLES → Vulkan: chiusura pulita del device precedente (risorse create = distrutte, nessun leak), backend riportato correttamente, auto-test, stessa scena |
| H | nessun errore del motore; validation layer e messaggi del driver riportati **a parte** (non sono errori del motore) |

## Passi manuali sul Galaxy A33

Prima: *Impostazioni → Display → Rotazione automatica* **attiva**.

1. Installa il nuovo APK (disinstalla prima la versione precedente).
2. Apri **Paint Engine** e attendi `SelfTest PASSED` nell'overlay.
3. Tocca **DEVICE TEST** e **non toccare il telefono** finché non compare il
   report (1–2 minuti; lo schermo può lampeggiare: è la superficie che viene
   distrutta e ricreata). Tocca **CLOSE**.
4. **Background/foreground**: tasto Home → attendi 3 s → riapri l'app dalle
   app recenti. Ripeti 3 volte.
5. **Blocco schermo**: tasto di accensione → attendi 3 s → sblocca. Ripeti 2 volte.
6. **Rotazione**: ruota in orizzontale, attendi 2 s, guarda il triangolo;
   torna in verticale. Ripeti 2 volte. *Controllo visivo*: il vertice
   **rosso** deve puntare verso l'**alto dello schermo** in entrambe le
   orientazioni, il triangolo non deve apparire schiacciato o allungato.
7. **Backend**: tocca **BACKEND** fino a `GLES` → attendi `SelfTest PASSED`
   e `Backend OpenGL ES` → tocca fino a `AUTO` (torna Vulkan).
8. **Modalità**: tocca **MODE** (continuo) per ~5 s, osserva che il triangolo
   resti stabile (nessun colore che sparisce, nessuno sfarfallio); tocca
   **MODE** di nuovo (on-demand).
9. Tocca **REPORT → SHARE** (o **COPY**) e incolla il testo nella chat.

Il report contiene la sezione `DEVICE VALIDATION` con PASS/FAIL e, nella
`APP TIMELINE`, ogni evento dei passi 4–8 (onPause/onResume, surfaceDestroyed
con durata, rotazioni, auto-test dopo ogni ricollegamento con dimensione e
trasformazione del display).

## Cosa resta una verifica visiva

L'auto-test legge l'immagine della swapchain, non lo schermo: dimostra che il
triangolo è disegnato correttamente nel buffer. L'orientamento finale sullo
schermo dopo la rotazione (fatto dal compositore, la pre-rotazione non è
implementata) e l'assenza di sfarfallio si possono confermare solo guardando.
