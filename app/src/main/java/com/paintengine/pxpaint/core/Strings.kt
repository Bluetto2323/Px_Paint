package com.paintengine.pxpaint.core

import java.util.Locale

/**
 * UI strings, English and Italian. Each entry carries both translations, so a
 * missing translation is a compile error. StringsTest checks that format
 * placeholders match between the languages.
 */
@Suppress("unused")
enum class S(val en: String, val it: String) {
    APP_NAME("PxPaint", "PxPaint"),
    GALLERY_TITLE("Gallery", "Galleria"),
    NEW_CANVAS("New canvas", "Nuova tela"),
    SEARCH_HINT("Search canvases", "Cerca tele"),
    SORT("Sort", "Ordina"),
    SORT_RECENT("Recent", "Recenti"),
    SORT_NAME("Name", "Nome"),
    SORT_SIZE("Size", "Dimensione"),
    SORT_OLDEST("Oldest", "Meno recenti"),
    EMPTY_TITLE("Your canvases live here", "Qui vivono le tue tele"),
    EMPTY_BODY(
        "Create a canvas to start. Pick a preset or your screen size: you can zoom and rotate it freely while you work.",
        "Crea una tela per iniziare. Scegli un formato o la dimensione dello schermo: mentre lavori potrai ingrandirla e ruotarla liberamente.",
    ),
    NO_RESULTS("No canvas matches “%s”", "Nessuna tela corrisponde a “%s”"),
    CANVAS_COUNT("%d canvases", "%d tele"),
    CANVAS_COUNT_ONE("1 canvas", "1 tela"),

    ACTION_OPEN("Open", "Apri"),
    ACTION_RENAME("Rename", "Rinomina"),
    ACTION_DUPLICATE("Duplicate", "Duplica"),
    ACTION_DELETE("Delete", "Elimina"),
    ACTION_CANCEL("Cancel", "Annulla"),
    ACTION_CREATE("Create", "Crea"),
    ACTION_SAVE("Save", "Salva"),
    ACTION_DONE("Done", "Fatto"),
    ACTION_GOT_IT("Got it", "Ho capito"),
    ACTION_CLOSE("Close", "Chiudi"),
    ACTION_CLEAR("Clear", "Svuota"),
    MORE_OPTIONS("More options", "Altre opzioni"),
    SETTINGS("Settings", "Impostazioni"),

    DELETE_TITLE("Delete “%s”?", "Eliminare “%s”?"),
    DELETE_BODY(
        "This canvas will be removed from this device. This cannot be undone.",
        "La tela verrà rimossa da questo dispositivo. L'operazione non si può annullare.",
    ),
    RENAME_TITLE("Rename canvas", "Rinomina tela"),
    COPY_SUFFIX("copy", "copia"),
    UNTITLED("Untitled", "Senza titolo"),
    DELETED("Canvas deleted", "Tela eliminata"),
    DUPLICATED("Canvas duplicated", "Tela duplicata"),
    RENAMED("Canvas renamed", "Tela rinominata"),
    EDITED_JUST_NOW("Edited just now", "Modificata ora"),
    EDITED_MINUTES("Edited %d min ago", "Modificata %d min fa"),
    EDITED_HOURS("Edited %d h ago", "Modificata %d h fa"),
    EDITED_YESTERDAY("Edited yesterday", "Modificata ieri"),
    EDITED_DAYS("Edited %d days ago", "Modificata %d giorni fa"),
    EDITED_ON("Edited %s", "Modificata il %s"),
    STORE_ERROR("Storage error: %s", "Errore di archiviazione: %s"),

    // New canvas
    PRESET_SCREEN("Screen", "Schermo"),
    PRESET_SQUARE("Square", "Quadrato"),
    PRESET_A4("A4 · 300 dpi", "A4 · 300 dpi"),
    PRESET_A5("A5 · 300 dpi", "A5 · 300 dpi"),
    PRESET_FULL_HD("Full HD", "Full HD"),
    PRESET_4K("4K UHD", "4K UHD"),
    PRESET_POST("Portrait post", "Post verticale"),
    PRESET_CUSTOM("Custom", "Personalizzata"),
    NAME("Name", "Nome"),
    WIDTH("Width", "Larghezza"),
    HEIGHT("Height", "Altezza"),
    SWAP_SIDES("Swap width and height", "Scambia larghezza e altezza"),
    BACKGROUND("Background", "Sfondo"),
    BG_WHITE("White", "Bianco"),
    BG_PAPER("Warm paper", "Carta calda"),
    BG_GREY("Grey", "Grigio"),
    BG_BLACK("Black", "Nero"),
    BG_TRANSPARENT("Transparent", "Trasparente"),
    STORAGE_SINGLE("One GPU texture · %s", "Una texture GPU · %s"),
    STORAGE_TILED(
        "%d × %d tiles · up to %s, allocated only where you paint",
        "%d × %d tile · fino a %s, allocati solo dove dipingi",
    ),
    SIZE_TOO_SMALL("Each side must be at least 1 px", "Ogni lato deve essere di almeno 1 px"),
    SIZE_TOO_LARGE("Each side can be at most %d px", "Ogni lato può essere al massimo %d px"),
    SIZE_OVER_BUDGET(
        "Too large for this device's memory (square canvases up to about %d px)",
        "Troppo grande per la memoria di questo dispositivo (tele quadrate fino a circa %d px)",
    ),
    SIZE_CHECKED_ON_DEVICE("Checked against this device's GPU", "Verificato sulla GPU di questo dispositivo"),
    SIZE_ESTIMATE("Estimate: confirmed when the canvas opens", "Stima: confermata all'apertura della tela"),

    // Workspace
    BACK_TO_GALLERY("Back to gallery", "Torna alla galleria"),
    UNDO("Undo", "Annulla"),
    REDO("Redo", "Ripeti"),
    UNDO_SOON("Undo and redo arrive in a later version", "Annulla e ripeti arriveranno in una versione successiva"),
    TOOL_MARKER("Marker", "Pennarello"),
    TOOL_MARKER_DESC(
        "Round, smooth, antialiased brush. Size, opacity and spacing below.",
        "Pennello tondo, continuo e antialias. Dimensione, opacità e spaziatura qui sotto.",
    ),
    TOOL_ERASER("Eraser", "Gomma"),
    TOOL_ERASER_DESC("Erases back to the background.", "Cancella fino allo sfondo."),
    TOOL_MOVE("Move", "Sposta"),
    TOOL_MOVE_DESC(
        "One finger moves the canvas. Two fingers always move, zoom and rotate.",
        "Un dito sposta la tela. Con due dita sposti, ingrandisci e ruoti sempre.",
    ),
    COLOR("Color", "Colore"),
    LAYERS("Layers", "Livelli"),
    LAYERS_SOON_TITLE("One layer for now", "Un livello, per ora"),
    LAYERS_SOON_BODY(
        "Your canvas has a single layer. Multiple layers, blend modes and reordering are coming in a future update.",
        "La tua tela ha un solo livello. Più livelli, modalità di fusione e riordino arriveranno in un prossimo aggiornamento.",
    ),
    LAYER_BACKGROUND("Background", "Sfondo"),
    LAYER_PAINT("Layer 1", "Livello 1"),
    SIZE("Size", "Dimensione"),
    OPACITY("Opacity", "Opacità"),
    SPACING("Spacing", "Spaziatura"),
    SPACING_HINT(
        "Low spacing gives a continuous stroke; raise it to see the individual dabs.",
        "Con spaziatura bassa il tratto è continuo; aumentala per vedere i singoli tocchi.",
    ),
    CANVAS_MENU("Canvas", "Tela"),
    FIT("Fit to screen", "Adatta allo schermo"),
    ACTUAL_PIXELS("Actual pixels (100%)", "Pixel reali (100%)"),
    RESET_ROTATION("Reset rotation", "Azzera rotazione"),
    CENTER("Center canvas", "Centra la tela"),
    CLEAR_CANVAS("Clear canvas", "Svuota la tela"),
    CLEAR_CONFIRM(
        "Clear everything on this canvas? This cannot be undone yet.",
        "Cancellare tutto su questa tela? Per ora non si può annullare.",
    ),
    CANVAS_INFO("Canvas info", "Informazioni tela"),
    TEST_RENDERING("Check canvas rendering", "Verifica il rendering della tela"),
    HUE("Hue", "Tonalità"),
    RECENT_COLORS("Recent colors", "Colori recenti"),
    TIP_TITLE("Welcome to your canvas", "Benvenuto sulla tua tela"),
    TIP_BODY(
        "One finger paints · Two fingers move, zoom and rotate · Tap the zoom indicator to fit the canvas",
        "Un dito dipinge · Due dita spostano, ingrandiscono e ruotano · Tocca l'indicatore di zoom per adattare la tela",
    ),
    CANVAS_OPEN_FAILED("This canvas could not be opened: %s", "Impossibile aprire la tela: %s"),
    SAVE_FAILED("Could not save: %s", "Salvataggio non riuscito: %s"),
    INFO_SIZE("Size", "Dimensione"),
    INFO_STORAGE("GPU storage", "Memoria GPU"),
    INFO_GRAPHICS("Graphics", "Grafica"),
    INFO_TILES("Tiles in memory", "Tile in memoria"),
    INFO_CONTENT("Painted tiles (CPU raster)", "Tile dipinti (raster CPU)"),
    SELFTEST_RUNNING("Checking canvas rendering…", "Verifica del rendering…"),
    SELFTEST_PASSED("Canvas rendering verified ✓", "Rendering della tela verificato ✓"),
    SELFTEST_FAILED("Canvas rendering check failed: %s", "Verifica del rendering fallita: %s"),
    ZOOM_CHIP_DESC("Zoom %s, rotation %s. Tap to fit.", "Zoom %s, rotazione %s. Tocca per adattare."),

    // Settings
    SEC_APPEARANCE("Appearance", "Aspetto"),
    THEME("Theme", "Tema"),
    THEME_SYSTEM("System", "Sistema"),
    THEME_DARK("Dark", "Scuro"),
    THEME_LIGHT("Light", "Chiaro"),
    UI_SIZE("Interface size", "Dimensione interfaccia"),
    LANGUAGE("Language", "Lingua"),
    LANG_SYSTEM("System", "Sistema"),
    LANG_ENGLISH("English", "English"),
    LANG_ITALIAN("Italiano", "Italiano"),
    SEC_TOOLBAR("Toolbar", "Barra degli strumenti"),
    DOCK_SIDE("Side rail position", "Posizione della barra laterale"),
    DOCK_SIDE_DESC("Used in landscape and on tablets", "Usata in orizzontale e sui tablet"),
    SIDE_AUTO("Auto", "Auto"),
    SIDE_LEFT("Left", "Sinistra"),
    SIDE_RIGHT("Right", "Destra"),
    TOOL_LABELS("Show tool names", "Mostra i nomi degli strumenti"),
    SEC_GESTURES("Gestures", "Gesti"),
    TWO_FINGER_ROTATE("Rotate with two fingers", "Ruota con due dita"),
    ROTATION_SNAP("Snap rotation to right angles", "Aggancia la rotazione agli angoli retti"),
    FINGER_DRAWING("Paint with a finger", "Dipingi con il dito"),
    FINGER_DRAWING_DESC(
        "When off, one finger moves the canvas (useful with a stylus)",
        "Se disattivato, un dito sposta la tela (utile con il pennino)",
    ),
    SEC_PERFORMANCE("Performance", "Prestazioni"),
    RENDERING("Rendering", "Rendering"),
    RENDER_BATTERY("Battery saver", "Risparmio batteria"),
    RENDER_BATTERY_DESC("Draws only when something changes (recommended)", "Disegna solo quando qualcosa cambia (consigliato)"),
    RENDER_SMOOTH("Always smooth", "Sempre fluido"),
    RENDER_SMOOTH_DESC("Redraws every frame and uses more battery", "Ridisegna a ogni frame e consuma più batteria"),
    GRAPHICS_ENGINE("Graphics engine", "Motore grafico"),
    BACKEND_AUTO("Automatic", "Automatico"),
    BACKEND_AUTO_DESC("Vulkan when available, otherwise OpenGL ES", "Vulkan se disponibile, altrimenti OpenGL ES"),
    BACKEND_VULKAN("Vulkan", "Vulkan"),
    BACKEND_GLES("OpenGL ES", "OpenGL ES"),
    BACKEND_APPLIES_LATER("Applies the next time PxPaint starts", "Si applica al prossimo avvio di PxPaint"),
    PERF_OVERLAY("Performance overlay", "Indicatore delle prestazioni"),
    PERF_OVERLAY_DESC("Frame time, tiles and memory on the canvas", "Tempo di frame, tile e memoria sulla tela"),
    SEC_ACCESSIBILITY("Accessibility", "Accessibilità"),
    LARGE_TARGETS("Larger touch targets", "Pulsanti più grandi"),
    REDUCE_MOTION("Reduce motion", "Riduci le animazioni"),
    HIGH_CONTRAST("High-contrast outlines", "Contorni ad alto contrasto"),
    SEC_ABOUT("About", "Informazioni"),
    VERSION("Version", "Versione"),
    ENGINE_DIAGNOSTICS("Engine diagnostics", "Diagnostica del motore"),
    ENGINE_DIAGNOSTICS_DESC(
        "GPU report, self-tests and the device validation run",
        "Report GPU, auto-test e validazione del dispositivo",
    ),
    GRAPHICS_IN_USE("Graphics in use", "Grafica in uso"),
    NOT_STARTED_YET("Starts with the first canvas", "Si avvia con la prima tela"),
    RESET_TIPS("Show tips again", "Mostra di nuovo i suggerimenti"),
    TIPS_RESET_DONE("Tips will show again", "I suggerimenti verranno mostrati di nuovo"),
    ;

    companion object {
        /** Set from the settings (LanguagePref) and the system locale. */
        @Volatile
        var italian: Boolean = false

        fun applyLanguage(pref: LanguagePref, systemLocale: Locale) {
            italian = when (pref) {
                LanguagePref.ITALIAN -> true
                LanguagePref.ENGLISH -> false
                LanguagePref.SYSTEM -> systemLocale.language == "it"
            }
        }
    }
}

fun tr(s: S): String = if (S.italian) s.it else s.en

fun tr(s: S, vararg args: Any): String =
    String.format(if (S.italian) Locale.ITALIAN else Locale.US, tr(s), *args)
