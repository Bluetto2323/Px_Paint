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

---

# Blocco 4 — Stilo e pressione: checklist sul Galaxy A33

**L'A33 non ha uno stilo attivo**: la pressione, l'inclinazione e la gomma
dello stilo **non si possono provare su questo telefono**. Qui si verifica
che tutto il resto funzioni come prima e che le funzioni dello stilo siano
indicate correttamente come non disponibili. (Una penna capacitiva passiva
viene vista dal telefono come un dito: deve comportarsi esattamente come un
dito.)

Installa `PxPaint-block4-debug-….apk` (disinstalla prima la build precedente).

### 1. Il dito disegna come nel Blocco 3

| # | Azione | Atteso |
|---|---|---|
| 1.1 | Pennello 24 px: tratti veloci, lenti, tap | identici al Blocco 3: spessore costante, nessun buco, punta sotto il dito |
| 1.2 | Pennello → *La pressione controlla la dimensione* **attivo**, minimo 5 %, poi disegna col dito | lo spessore **non cambia** (il dito non fornisce pressione: si usa sempre la dimensione piena) |
| 1.3 | *La pressione controlla l'opacità* attivo, disegna col dito | opacità piena, nessuna variazione |
| 1.4 | Indicatore delle prestazioni | latenza paragonabile al Blocco 3 (annota media/massimo) |

### 2. Modalità del dito (Impostazioni → Gesti → *Un dito*)

| # | Azione | Atteso |
|---|---|---|
| 2.1 | *Disegna* | un dito disegna; due dita spostano/zoomano/ruotano |
| 2.2 | *Sposta la tela* | un dito sposta la tela, non disegna |
| 2.3 | *Solo gesti* | un dito non fa nulla; due dita spostano/zoomano/ruotano |
| 2.4 | Torna a *Disegna*, appoggia un dito e subito il secondo | nessun segno lasciato (come prima) |

### 3. Indicazioni dello stilo

| # | Azione | Atteso |
|---|---|---|
| 3.1 | Pannello del pennello (tocca di nuovo lo strumento) | sezione *Stilo* con il testo "Nessuno stilo con pressione rilevato…" |
| 3.2 | *L'inclinazione modella il pennello* | interruttore disattivato, con "Non disponibile…" |
| 3.3 | Impostazioni → *Stilo e pressione* → *Curva di pressione e prova* | "Questo dispositivo non riporta alcuno stilo attivo…" |
| 3.4 | Nell'area di prova tocca col dito | "Tocco col dito: nessuna pressione viene letta" (nessun grafico inventato) |
| 3.5 | Curva: scegli *Morbida*, trascina i due punti, sensibilità e smussamento | la curva si aggiorna; uscendo e rientrando i valori restano |
| 3.6 | *Ripristina i valori predefiniti* | curva lineare, sensibilità 0, smussamento 35 % |

### 4. Compatibilità

| # | Azione | Atteso |
|---|---|---|
| 4.1 | Crea un disegno, chiudi l'app dalle recenti, riaprilo | disegno identico (il formato dei progetti non cambia nel Blocco 4) |
| 4.2 | Diagnostica → DEVICE TEST → REPORT | come prima (sezione D decide KI-1) |
| 4.3 | Motore grafico OpenGL ES, ripeti 1.1 | uguale |

### Se hai un telefono/tablet con stilo attivo (facoltativo)

Su un dispositivo con S Pen o penna USI: pressione → spessore e opacità,
curva (area di prova: grigio = pressione dello stilo, colore = risposta),
inclinazione (con l'opzione attiva), gomma della penna, appoggio del palmo
mentre lo stilo disegna (nessun segno del palmo). Anche questo risultato
va riportato come "testato su <modello>".

# Blocco 5 — Libreria pennelli: checklist sul Galaxy A33

Installa `PxPaint-block5-debug-….apk` **sopra** la build del Blocco 4, senza disinstallarla, per verificare la migrazione. Poi ripeti i test da un'installazione pulita.

### 1. Migrazione e compatibilità

| # | Azione | Atteso |
|---|---|---|
| 1.1 | Apri una tela creata nei blocchi precedenti | si apre identica, nessun pixel cambiato |
| 1.2 | Tocca lo strumento *Pennello* (pannello) | pennello attivo **Pennello duro** con la tua dimensione e opacità di prima |
| 1.3 | Disegna col dito | tratto identico al Blocco 4 |
| 1.4 | Installazione pulita: primo pennello | **Tondo base** (bordo leggermente morbido); gomma = *Pennello duro* |

### 2. Libreria

| # | Azione | Atteso |
|---|---|---|
| 2.1 | Pannello del pennello → riga del pennello, oppure **pressione lunga** sul pulsante Pennello | si apre la libreria, la tela resta visibile intorno |
| 2.2 | Scorri i 12 pennelli | ogni riga ha un'anteprima disegnata dal motore (non un'icona), tutte diverse |
| 2.3 | Filtri: Preferiti, Recenti, Schizzo, Inchiostro, Pittura, Aerografo, Tecniche secche, I miei | elenchi coerenti; messaggi chiari se vuoti |
| 2.4 | Cerca: "carb", "acquerello", "ink", "matita sfumatura" | trovati per nome (anche italiano), categoria, etichette |
| 2.5 | Stella su due pennelli → filtro *Preferiti* | ci sono, nell'ordine scelto; restano dopo il riavvio dell'app |
| 2.6 | Cambia pennello più volte | nessuna pausa percettibile; *Recenti* in ordine d'uso |
| 2.7 | Cursori Dimensione / Opacità nella libreria | cambiano il pennello selezionato; tornando a quel pennello i valori restano |

### 3. I 12 pennelli col dito (stessa tela)

| # | Pennello | Atteso |
|---|---|---|
| 3.1 | Tondo base / Pennello duro | bordo morbido / bordo netto |
| 3.2 | Aerografo morbido | velatura leggera che si scurisce ripassando nello stesso tratto |
| 3.3 | Matita / Matita grafite | grana della carta visibile; la grafite è più larga e si accumula ripassando |
| 3.4 | Penna a inchiostro | pennino piatto: tratti orizzontali e verticali di spessore diverso |
| 3.5 | Fineliner | linea sottile costante |
| 3.6 | Pennarello | semitrasparente; due tratti sovrapposti si scuriscono, un tratto da solo no |
| 3.7 | Carboncino / Gessetto | bordi irregolari e grana ruvida; il gessetto è più "a puntini" |
| 3.8 | Pennello materico | striature parallele al tratto in ogni direzione, trama della tela |
| 3.9 | Acquerello | bordo più scuro dell'interno, granulazione leggera |
| 3.10 | Indicatore prestazioni: tratti lunghi con Pennello materico e Aerografo a dimensione massima | annota *brush ms* e la fluidità |

### 4. Personalizzazione

| # | Azione | Atteso |
|---|---|---|
| 4.1 | Libreria → ⋮ → *Modifica pennello* su *Gessetto* | editor con anteprima (tratto, dimensioni, opacità) e area di prova |
| 4.2 | Disegna nell'area di prova, poi cambia *Profondità della grana* e *Durezza* | il disegno di prova si aggiorna con i nuovi parametri |
| 4.3 | Torna alla libreria | *Gessetto · Modificato* |
| 4.4 | ⋮ → *Ripristina originale* | torna ai valori originali, la scritta *Modificato* sparisce |
| 4.5 | ⋮ → *Duplica* | "Gessetto copia" nel filtro *I miei* |
| 4.6 | Rinomina la copia ("Mio gessetto"); prova anche un nome vuoto | rinominata; il nome vuoto viene rifiutato |
| 4.7 | Elimina la copia | sparisce da libreria, preferiti e recenti; un pennello incluso non si può eliminare |
| 4.8 | Chiudi l'app (anche dal multitasking) e riaprila | preferiti, recenti, pennelli personali, modifiche e pennello selezionato restano |

### 5. Gomma

| # | Azione | Atteso |
|---|---|---|
| 5.1 | Strumento Gomma → libreria → *Gessetto* | la gomma cancella con la forma del gesso |
| 5.2 | Torna al Pennello | il pennello non è cambiato; dimensione e pennello della gomma restano separati |

Riporta OK / KO per ogni riga; per i KO aggiungi uno screenshot e, se
possibile, il REPORT della diagnostica.

# Blocco 6 — Livelli: checklist sul Galaxy A33

Installa `PxPaint-block6-debug-….apk` **sopra** la build precedente, così si verifica anche la migrazione delle tele esistenti.

### 1. Tele esistenti

| # | Azione | Atteso |
|---|---|---|
| 1.1 | Apri una tela dei blocchi precedenti | identica; nel pannello Livelli c'è un solo livello, "Layer 1" |
| 1.2 | Disegna, torna alla galleria, riapri | il disegno c'è; la tela ora è salvata nel formato a livelli |

### 2. Operazioni

| # | Azione | Atteso |
|---|---|---|
| 2.1 | Pannello Livelli → + tre volte | nuovi livelli in cima, l'ultimo è attivo (evidenziato) |
| 2.2 | Disegna su livelli diversi, toccando la riga per sceglierli | il tratto va solo sul livello attivo; le miniature mostrano solo il proprio livello |
| 2.3 | Occhio (nascondi / mostra) | il livello sparisce e ricompare, senza perdere nulla |
| 2.4 | Lucchetto, poi prova a disegnare su quel livello | non disegna e compare "Il livello è bloccato: sbloccalo prima" |
| 2.5 | Livello nascosto attivo, prova a disegnare | "Il livello è nascosto: mostralo prima" |
| 2.6 | Opacità del livello 50 % con un pennello al 50 % | il tratto appare al 25 %; riportando l'opacità al 100 % torna al 50 % |
| 2.7 | Modalità: Moltiplica, Scolora, Sovrapponi, Scurisci, Schiarisci, Aggiungi, Differenza su un livello sopra un altro | ogni modalità cambia il risultato; nessun alone sui bordi trasparenti |
| 2.8 | Sposta su / Sposta giù | l'ordine sulla tela cambia di conseguenza |
| 2.9 | Duplica, Rinomina, Elimina (con conferma) | funzionano; l'ultimo livello non si può eliminare (pulsante disattivato) |
| 2.10 | Unisci in basso | due livelli diventano uno, la tela non cambia (livelli Normali) |
| 2.11 | ⋮ → Unisci tutti i livelli (con un livello nascosto) | la conferma dice che il livello nascosto verrà eliminato; poi resta un solo livello, la tela non cambia |
| 2.12 | Sfondo color crema + livello in Moltiplica | il livello si fonde anche con la carta |

### 3. Salvataggio e prestazioni

| # | Azione | Atteso |
|---|---|---|
| 3.1 | 5–10 livelli con nomi, opacità, modalità, uno nascosto e uno bloccato; chiudi l'app e riapri la tela | tutto identico: ordine, nomi, visibilità, blocchi, opacità, modalità, livello attivo |
| 3.2 | Indicatore prestazioni attivo, disegna con 10+ livelli | tratto fluido come prima (*brush ms* simile) |
| 3.3 | Trascina l'opacità di un livello grande | la tela si aggiorna in fretta; su tele molto grandi si aggiorna a zone in pochi istanti, l'app non si blocca |

Riporta OK / KO per ogni riga.


---

# Blocco 7 — Annulla / Ripeti: checklist sul Galaxy A33

Installa `PxPaint-block7-debug-….apk` **sopra** la build del Blocco 6 (i progetti restano).

### 1. Tratti

| # | Azione | Atteso |
|---|---|---|
| 1.1 | Apri una tela nuova | ↶ e ↷ in alto sono attenuati (disattivati) |
| 1.2 | Disegna un tratto | ↶ si attiva subito dopo aver sollevato il dito |
| 1.3 | ↶ | il tratto sparisce del tutto (anche i bordi sfumati); messaggio "Annullato: Tratto · …"; ↷ si attiva |
| 1.4 | ↷ | il tratto ricompare identico |
| 1.5 | 10 tratti, poi ↶ 10 volte | spariscono uno alla volta, dal più recente; poi ↶ si disattiva |
| 1.6 | ↶ 3 volte, poi disegna un tratto nuovo | ↷ si disattiva (i 3 passi annullati non sono più ripetibili) |
| 1.7 | Gomma su un tratto semitrasparente, poi ↶ | il tratto torna esattamente com'era |
| 1.8 | Pennelli diversi (acquerello, gesso, aerografo), ↶ e ↷ | ogni tratto sparisce e ricompare identico, grana compresa |

### 2. Livelli

| # | Azione | Atteso |
|---|---|---|
| 2.1 | Aggiungi un livello, ↶ | il livello sparisce; ↷ lo rimette |
| 2.2 | Disegna su un livello, eliminalo, ↶ | torna lo stesso livello (stesso nome, stesso disegno, stessa posizione) |
| 2.3 | Duplica, Sposta su/giù, Rinomina, Nascondi, Blocca, Modalità: ↶ dopo ognuno | ogni modifica torna indietro; il pannello Livelli si aggiorna |
| 2.4 | Trascina l'opacità di un livello, ↶ | torna all'opacità di prima del trascinamento, in un solo passo |
| 2.5 | Unisci in basso, ↶ | tornano i due livelli originali, con modalità e opacità |
| 2.6 | ⋮ → Unisci tutti i livelli, ↶ | tornano tutti i livelli, anche quelli nascosti |
| 2.7 | ⋮ → Svuota livello, ↶ | il disegno torna |

### 3. Cronologia, memoria, salvataggio

| # | Azione | Atteso |
|---|---|---|
| 3.1 | Tieni premuto ↶ (o ⋮ → Cronologia) | pannello Cronologia: passaggi dal più recente, quello attuale evidenziato, gli annullati attenuati, "Inizio" in fondo |
| 3.2 | Tocca un passaggio più in basso, poi uno più in alto | la tela salta a quello stato e poi torna avanti |
| 3.3 | Svuota cronologia (con conferma) | il disegno non cambia; ↶ e ↷ si disattivano |
| 3.4 | Più di 100 tratti | restano disponibili gli ultimi 100 (o meno su tele grandi); il pannello dice quanti sono stati eliminati |
| 3.5 | ↶ alcuni passi, torna alla galleria, riapri la tela | la tela è come la lasciavi (passi annullati esclusi); la cronologia è vuota |
| 3.6 | ↶ un'eliminazione di livello, chiudi l'app dalle recenti, riapri | il livello ripristinato c'è |
| 3.7 | Indicatore prestazioni, disegna velocemente | il tratto è fluido come nel Blocco 6 |

Riporta OK / KO per ogni riga.

