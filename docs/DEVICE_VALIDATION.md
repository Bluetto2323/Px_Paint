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


---

# Blocco 2 — PxPaint: prova manuale sul Galaxy A33

Nessun PC necessario. Disinstalla prima la build del Blocco 1 (firma debug
diversa), installa `PxPaint-block2-debug-….apk`. L'icona ora si chiama
**PxPaint**. La diagnostica del Blocco 1 è in *Impostazioni → Diagnostica
del motore* (stesso DEVICE TEST / REPORT di prima).

Annota per ogni passo **OK / KO + cosa hai visto**; fai screenshot dei KO.

### 1. Verifica KI-1 (ridimensionamento Vulkan) — prima di tutto

1. PxPaint → ⚙ (in alto a destra) → *Diagnostica del motore*.
2. Attendi `SelfTest PASSED`, tocca **DEVICE TEST**, non toccare il telefono.
3. **REPORT → SHARE/COPY** e incolla il testo in chat.
   Atteso: sezione D con tre righe `-> surface <stessa dimensione> … OK`.
   Solo questo report può chiudere KI-1.
4. Indietro per tornare a PxPaint.

### 2. Galleria e nuova tela

| # | Azione | Atteso |
|---|---|---|
| 2.1 | Primo avvio | tema scuro, nessun lampo bianco, stato vuoto "Qui vivono le tue tele" e scheda *Nuova tela* |
| 2.2 | Nuova tela → preset *Schermo*, carta bianca → Crea | si apre l'area di lavoro, tela bianca centrata e adattata |
| 2.3 | Nuova tela → *Personalizzata* 16384 × 16384 | riga archiviazione "64 × 64 tile · fino a … allocati solo dove dipingi" (budget = ¼ della RAM, max 1,5 GiB: su un A33 da 6/8 GB è accettata). Annota il testo esatto. Se crei: apertura senza crash, memoria allocata solo dove disegni (Info tela) |
| 2.4 | Personalizzata 4096 × 4096 | info "tile" (a tile), Crea funziona |
| 2.5 | Personalizzata 0 e 20000 | "Ogni lato deve essere di almeno 1 px" / "al massimo 16384 px", Crea disattivato |
| 2.6 | Carta *Trasparente* | scacchiera nell'area di lavoro e nella miniatura |

### 3. Area di lavoro — tela GPU

| # | Azione | Atteso |
|---|---|---|
| 3.1 | Disegna con un dito (Marcatore) | segno continuo sotto il dito, senza ritardo percepibile |
| 3.2 | Due dita: pizzica, trascina, ruota | lo zoom resta centrato tra le dita, il punto sotto le dita non scivola; il chip mostra % e gradi |
| 3.3 | Ruota a ~3° da 0/90/180° e solleva le dita | aggancio all'angolo retto |
| 3.4 | Appoggia il secondo dito subito dopo il primo | nessun puntino indesiderato |
| 3.5 | Tocca il chip zoom | la tela si riadatta allo schermo |
| 3.6 | Menu ⋯ (o pressione lunga sul chip) → *Pixel reali (100%)* / *Azzera rotazione* / *Centra la tela* | 100 %, 0°, tela centrata |
| 3.7 | Zoom massimo su un bordo della tela | pixel netti, bordi dritti, nessuna riga/fessura tra tile (tela 4096²) |
| 3.8 | Gomma | cancella mostrando la carta (o la scacchiera se trasparente) |
| 3.9 | Strumento Sposta | un dito sposta la tela, non disegna |
| 3.10 | Colore: tocca il cerchio del colore, scegli un colore, disegna | colore corretto; compare tra i recenti |
| 3.11 | Livelli: cambia colore carta | la carta cambia subito, il disegno resta |
| 3.12 | Menu ⋯ → *Verifica il rendering della tela* | "Rendering della tela verificato ✓" (self-test GPU della tela) |
| 3.13 | Menu ⋯ → *Informazioni tela* | dimensioni, archiviazione (singola/tile), tile allocati, backend Vulkan, GPU Mali-G68 |

### 4. Ciclo di vita e salvataggio

| # | Azione | Atteso |
|---|---|---|
| 4.1 | Disegna, tasto Home, attendi 5 s, rientra | disegno intatto |
| 4.2 | Blocca/sblocca lo schermo | disegno intatto, nessun crash |
| 4.3 | Ruota il telefono (verticale ↔ orizzontale) | barra strumenti diventa laterale in orizzontale, disegno e zoom coerenti, nessuna deformazione |
| 4.4 | Split-screen con un'altra app, poi a schermo intero | layout si adatta, tela non deformata |
| 4.5 | Indietro alla galleria | la miniatura mostra il disegno |
| 4.6 | Chiudi l'app dalle recenti, riaprila, apri la tela | disegno ricaricato identico |
| 4.7 | Galleria: rinomina, duplica, elimina (con conferma) | operazioni corrette; la copia contiene lo stesso disegno |
| 4.8 | Cerca per nome e per "4096", ordina per Nome/Dimensione | risultati corretti |

### 5. Impostazioni e backend

| # | Azione | Atteso |
|---|---|---|
| 5.1 | Tema chiaro | tutta l'interfaccia in chiaro, barre di sistema leggibili |
| 5.2 | Dimensione interfaccia 130 % | testi e pulsanti più grandi, nessun testo tagliato |
| 5.3 | Lingua English / Italiano | testi cambiati subito |
| 5.4 | Etichette strumenti, lato della barra, rotazione con due dita off, *Pulsanti più grandi* | effetto visibile nell'area di lavoro |
| 5.5 | *Indicatore delle prestazioni* on | riquadro con backend, ms di lavoro, tile, memoria, operazioni |
| 5.6 | *Motore grafico* = OpenGL ES → chiudi l'app dalle recenti e riapri | *Informazioni tela* riporta OpenGL ES; ripeti 3.1–3.7 e 3.12 |
| 5.7 | Torna ad *Automatico* e riavvia | Vulkan |
| 5.8 | *Sempre fluido* con l'indicatore attivo | fps ≈ refresh del display; *Risparmio batteria* torna on-demand (nessun fps mostrato a riposo) |

### Cosa mandare

1. Il REPORT della diagnostica (passo 1).
2. La tabella OK/KO dei passi 2–5 con eventuali screenshot.
3. Per qualsiasi crash: il passo esatto e cosa era aperto.

---

# Blocco 3 — Brush engine: checklist sul Galaxy A33

Installa `PxPaint-block3-debug-….apk` (disinstalla prima la build
precedente: firma debug diversa, vedi nota in fondo). Prima di iniziare:
*Impostazioni → Indicatore delle prestazioni* **attivo**: mostra latenza
input→schermo, tempo del pennello per frame e campioni ricevuti/elaborati.

Per ogni passo annota **OK / KO** e, dove indicato, i numeri dell'indicatore.

### 0. Verifica preliminare

1. ⚙ → *Diagnostica del motore* → **DEVICE TEST** → **REPORT** (sezione D
   decide KI-1).
2. Nuova tela A4, bianca. Menu ⋯ → *Verifica il rendering della tela*:
   "Rendering della tela verificato ✓".

### 1. Disegno veloce continuo

| # | Azione | Atteso |
|---|---|---|
| 1.1 | Scarabocchi molto veloci (cerchi, zig-zag) per 10 s, pennello 24 px | linea continua, nessun buco, nessuna sequenza di cerchi visibili, curve morbide senza spigoli |
| 1.2 | Guarda la punta mentre disegni | la linea arriva sotto il dito senza "rincorrerlo" |
| 1.3 | Indicatore: `samples` | i due numeri (elaborati/ricevuti) uguali a riposo |
| 1.4 | Indicatore: `latency` | annota media e massimo (ms) |
| 1.5 | Solleva il dito di colpo alla fine di un tratto veloce | il tratto finisce esattamente dove hai sollevato il dito |

### 2. Disegno lento e dettagliato

| # | Azione | Atteso |
|---|---|---|
| 2.1 | Linea lentissima (5 s per 10 cm), pennello 4 px | spessore costante, bordi lisci, nessun "seghettato" |
| 2.2 | Tocchi singoli (tap) | punto rotondo per ogni tap |
| 2.3 | Scrittura a mano (firma) | leggibile, nessun punto mancante tra le lettere |

### 3. Tratti sovrapposti e opacità

| # | Azione | Atteso |
|---|---|---|
| 3.1 | Opacità 40 %, un solo tratto che si incrocia più volte (spirale) | tono uniforme, **nessun scurimento** dove il tratto si sovrappone a se stesso |
| 3.2 | Secondo tratto 40 % sopra il primo | dove si incrociano due tratti diversi il colore è più scuro (normale) |
| 3.3 | 20 tratti uno sull'altro allo 30 % | il colore si satura gradualmente, senza cambiare tinta |
| 3.4 | Tela trasparente, bianco al 50 % | nessun bordo scuro attorno al tratto |

### 4. Dimensioni del pennello

| # | Azione | Atteso |
|---|---|---|
| 4.1 | 1 px, linee lente e veloci | linea sottile continua, non puntinata |
| 4.2 | 500 px (massimo), tratti veloci | bordo dritto e liscio (nessuna ondulazione), tempo pennello nell'indicatore: annota il massimo |
| 4.3 | Spaziatura 100 % con pennello 60 px | si vedono i singoli tocchi (comportamento voluto) |
| 4.4 | Spaziatura di nuovo bassa (8 %) | tratto continuo |

### 5. Tela ingrandita e ruotata

| # | Azione | Atteso |
|---|---|---|
| 5.1 | Zoom 400 %, disegna | lo spessore sulla tela è quello impostato (sullo schermo 4× più grande), bordi antialias |
| 5.2 | Zoom 25 %, disegna | stesso spessore relativo alla tela, nessun buco |
| 5.3 | Ruota la tela di ~37°, disegna una linea dritta | la linea segue esattamente il dito |
| 5.4 | Disegna a cavallo dei bordi dei tile (tela 4096², zoom 800 %, tratto lungo) | nessuna giunzione o riga visibile |

### 6. Gesti e annullamento del tratto

| # | Azione | Atteso |
|---|---|---|
| 6.1 | Appoggia un dito e subito il secondo (pizzica) | nessun segno lasciato sulla tela |
| 6.2 | Disegna un tratto lungo, poi senza sollevare aggiungi un secondo dito | il tratto resta, poi la tela si muove/ingrandisce |
| 6.3 | Disegna e intanto arriva una notifica / premi Home | il tratto disegnato fino a quel punto resta |

### 7. Sessione lunga e salvataggio

| # | Azione | Atteso |
|---|---|---|
| 7.1 | 10 minuti di disegno continuo su A4 con pennelli diversi | nessun rallentamento progressivo, nessun crash; annota latenza media/massimo alla fine |
| 7.2 | Indietro alla galleria | miniatura uguale al disegno |
| 7.3 | Chiudi l'app dalle recenti, riapri il progetto | disegno identico (anche le parti trasparenti e semitrasparenti) |
| 7.4 | (Progetti del Blocco 2) | non verificabile sul telefono: disinstallare per installare una nuova build CI cancella i progetti. L'import dei vecchi progetti è coperto dai test automatici (C6b, `TileContentTest`) |
| 7.5 | Gomma 100 % e 50 % | cancella del tutto / a metà, senza aloni |
| 7.6 | Impostazioni → Motore grafico **OpenGL ES**, riavvia, ripeti 1.1, 3.1, 5.3 | stesso risultato visivo; annota la latenza |

### Cosa mandare

1. REPORT della diagnostica.
2. Tabella OK/KO dei passi 1–7.
3. I numeri dell'indicatore: latenza (media/massimo) con Vulkan e con OpenGL
   ES, tempo massimo del pennello con 24 px e 500 px.
4. Screenshot di qualsiasi difetto (buchi, ondulazioni, scurimenti, giunzioni).

Nota: disinstallare l'app cancella i suoi progetti (memoria privata
dell'app). Ogni build CI è firmata con una chiave debug diversa, quindi va
sempre disinstallata la precedente.
