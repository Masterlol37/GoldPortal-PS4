#include "lang.h"

enum { L_EN, L_DE, L_FR, L_ES, L_IT, L_NL, L_PT, L_COUNT };
static int g_lang = L_EN;

// One row per text: en, de, fr, es, it, nl, pt (UTF-8, Latin-1 characters only).
static const char *const k_text[S_COUNT_][L_COUNT] = {
    [S_READY]         = { "Ready", "Bereit", "Pr\xC3\xAAt", "Listo", "Pronto", "Gereed", "Pronto" },
    [S_EMPTY]         = { "empty", "leer", "vide", "vac\xC3\xADo", "vuoto", "leeg", "vazio" },
    [S_SLOT_N]        = { "Slot %d", "Slot %d", "Emplacement %d", "Ranura %d", "Slot %d", "Slot %d", "Slot %d" },
    [S_TRAP_SLOT]     = { "Trap slot", "Fallen-Slot", "Emplacement pi\xC3\xA8ge", "Ranura de trampa", "Slot trappola",
                          "Valslot", "Slot de armadilha" },
    [S_VEHICLE_SLOT]  = { "Vehicle slot", "Fahrzeug-Slot", "Emplacement v\xC3\xA9hicule", "Ranura de veh\xC3\xAD" "culo",
                          "Slot veicolo", "Voertuigslot", "Slot de ve\xC3\xAD" "culo" },
    [S_TOPS]          = { "Tops", "Oberteile", "Hauts", "Partes superiores", "Parti superiori", "Bovenstukken",
                          "Partes superiores" },
    [S_BOTTOMS]       = { "Bottoms", "Unterteile", "Bas", "Partes inferiores", "Parti inferiori", "Onderstukken",
                          "Partes inferiores" },
    [S_PICK_RIGHT]    = { "Pick a figure on the right", "Figur rechts ausw\xC3\xA4hlen", "Choisis une figurine \xC3\xA0 droite",
                          "Elige una figura a la derecha", "Scegli una statuina a destra", "Kies rechts een figuur",
                          "Escolhe uma figura \xC3\xA0 direita" },
    [S_SELECT]        = { "Select", "Ausw\xC3\xA4hlen", "Choisir", "Elegir", "Scegli", "Kiezen", "Escolher" },
    [S_REMOVE]        = { "Remove", "Entfernen", "Retirer", "Quitar", "Rimuovi", "Verwijderen", "Remover" },
    [S_RESCAN]        = { "Rescan", "Neu einlesen", "Actualiser", "Volver a leer", "Aggiorna", "Opnieuw inlezen",
                          "Atualizar" },
    [S_CLOSE]         = { "Close", "Schlie\xC3\x9F" "en", "Fermer", "Cerrar", "Chiudi", "Sluiten", "Fechar" },
    [S_FIG_FOR_SLOT]  = { "Figure for slot %d", "Figur f\xC3\xBCr Slot %d", "Figurine pour l'emplacement %d",
                          "Figura para la ranura %d", "Statuina per lo slot %d", "Figuur voor slot %d",
                          "Figura para o slot %d" },
    [S_TRAP_FOR_SLOT] = { "Trap for slot %d", "Falle f\xC3\xBCr Slot %d", "Pi\xC3\xA8ge pour l'emplacement %d",
                          "Trampa para la ranura %d", "Trappola per lo slot %d", "Val voor slot %d",
                          "Armadilha para o slot %d" },
    [S_VEHICLE_FOR_SLOT] = { "Vehicle for slot %d", "Fahrzeug f\xC3\xBCr Slot %d", "V\xC3\xA9hicule pour l'emplacement %d",
                          "Veh\xC3\xAD" "culo para la ranura %d", "Veicolo per lo slot %d", "Voertuig voor slot %d",
                          "Ve\xC3\xAD" "culo para o slot %d" },
    [S_SWAP_FOR_SLOT] = { "Swap Force: %s for slot %d", "Swap Force: %s f\xC3\xBCr Slot %d",
                          "Swap Force : %s pour l'emplacement %d", "Swap Force: %s para la ranura %d",
                          "Swap Force: %s per lo slot %d", "Swap Force: %s voor slot %d", "Swap Force: %s para o slot %d" },
    [S_TOP]           = { "top", "Oberteil", "haut", "parte superior", "parte superiore", "bovenstuk", "parte superior" },
    [S_BOTTOM]        = { "bottom", "Unterteil", "bas", "parte inferior", "parte inferiore", "onderstuk", "parte inferior" },
    [S_COUNT_CHASE]   = { "%d figures   Chase: %s", "%d Figuren   Chase: %s", "%d figurines   Chase : %s",
                          "%d figuras   Chase: %s", "%d statuine   Chase: %s", "%d figuren   Chase: %s",
                          "%d figuras   Chase: %s" },
    [S_ON]            = { "on", "an", "oui", "s\xC3\xAD", "s\xC3\xAC", "aan", "sim" },
    [S_OFF]           = { "off", "aus", "non", "no", "no", "uit", "n\xC3\xA3o" },
    [S_GAME]          = { "Game", "Spiel", "Jeu", "Juego", "Gioco", "Spel", "Jogo" },
    [S_KIND]          = { "Type", "Art", "Type", "Tipo", "Tipo", "Soort", "Tipo" },
    [S_ALL]           = { "All", "Alle", "Tous", "Todos", "Tutti", "Alle", "Todos" },
    [S_CAT_FIGURES]   = { "Figures", "Figuren", "Figurines", "Figuras", "Statuine", "Figuren", "Figuras" },
    [S_CAT_ITEMS]     = { "Magic Items", "Magic Items", "Objets magiques", "Objetos m\xC3\xA1gicos", "Oggetti magici",
                          "Magische items", "Itens m\xC3\xA1gicos" },
    [S_CAT_TRAPS]     = { "Traps", "Fallen", "Pi\xC3\xA8ges", "Trampas", "Trappole", "Vallen", "Armadilhas" },
    [S_CAT_VEHICLES]  = { "Vehicles", "Fahrzeuge", "V\xC3\xA9hicules", "Veh\xC3\xAD" "culos", "Veicoli", "Voertuigen",
                          "Ve\xC3\xAD" "culos" },
    [S_CAT_CRYSTALS]  = { "Crystals", "Kristalle", "Cristaux", "Cristales", "Cristalli", "Kristallen", "Cristais" },
    [S_SWAP_PARTS]    = { "Swap parts", "Swap-Teile", "Moiti\xC3\xA9s Swap", "Partes Swap", "Parti Swap", "Swap-delen",
                          "Partes Swap" },
    [S_NO_MATCH]      = { "No matching figures", "Keine passenden Figuren", "Aucune figurine correspondante",
                          "Ninguna figura coincide", "Nessuna statuina corrispondente", "Geen passende figuren",
                          "Nenhuma figura corresponde" },
    [S_NO_FIGURES_DIR] = { "No figures for this game in %s", "Keine Figuren f\xC3\xBCr dieses Spiel in %s",
                          "Aucune figurine pour ce jeu dans %s", "No hay figuras para este juego en %s",
                          "Nessuna statuina per questo gioco in %s", "Geen figuren voor dit spel in %s",
                          "Nenhuma figura para este jogo em %s" },
    [S_CHASE_TAG]     = { "(chase variant)", "(Chase-Variante)", "(variante chase)", "(variante chase)",
                          "(variante chase)", "(chase-variant)", "(variante chase)" },
    [S_PLACE]         = { "Place", "Aufsetzen", "Poser", "Colocar", "Posiziona", "Plaatsen", "Colocar" },
    [S_CHASE_SHOW]    = { "Show chase", "Chase zeigen", "Afficher chase", "Mostrar chase", "Mostra chase",
                          "Chase tonen", "Mostrar chase" },
    [S_CHASE_HIDE]    = { "Hide chase", "Chase ausblenden", "Masquer chase", "Ocultar chase", "Nascondi chase",
                          "Chase verbergen", "Ocultar chase" },
    [S_BACK]          = { "Back", "Zur\xC3\xBC" "ck", "Retour", "Atr\xC3\xA1s", "Indietro", "Terug", "Voltar" },
    [S_PAGE]          = { "Page", "Seite", "Page", "P\xC3\xA1gina", "Pagina", "Pagina", "P\xC3\xA1gina" },
    [S_PAGE_N]        = { "Page %d/%d", "Seite %d/%d", "Page %d/%d", "P\xC3\xA1gina %d/%d", "Pagina %d/%d",
                          "Pagina %d/%d", "P\xC3\xA1gina %d/%d" },
    [S_CANT_READ]     = { "Cannot read %s", "Kann %s nicht lesen", "Impossible de lire %s", "No se puede leer %s",
                          "Impossibile leggere %s", "Kan %s niet lezen", "N\xC3\xA3o foi poss\xC3\xADvel ler %s" },
    [S_PLACED]        = { "%s on slot %d", "%s auf Slot %d", "%s sur l'emplacement %d", "%s en la ranura %d",
                          "%s nello slot %d", "%s op slot %d", "%s no slot %d" },
    [S_SLOT_FREE]     = { "Slot %d empty", "Slot %d frei", "Emplacement %d libre", "Ranura %d libre", "Slot %d libero",
                          "Slot %d vrij", "Slot %d livre" },
    [S_FOUND_N]       = { "%d figures found", "%d Figuren gefunden", "%d figurines trouv\xC3\xA9" "es",
                          "%d figuras encontradas", "%d statuine trovate", "%d figuren gevonden",
                          "%d figuras encontradas" },
    [S_CHASE_HIDDEN]  = { "Chase variants hidden", "Chase-Varianten ausgeblendet", "Variantes chase masqu\xC3\xA9" "es",
                          "Variantes chase ocultas", "Varianti chase nascoste", "Chase-varianten verborgen",
                          "Variantes chase ocultas" },
    [S_CHASE_SHOWN]   = { "Chase variants shown", "Chase-Varianten sichtbar", "Variantes chase affich\xC3\xA9" "es",
                          "Variantes chase visibles", "Varianti chase visibili", "Chase-varianten zichtbaar",
                          "Variantes chase vis\xC3\xAD" "veis" },
    [S_SWAP_NEXT]     = { "%s on slot %d - now pick %s", "%s auf Slot %d - jetzt %s w\xC3\xA4hlen",
                          "%s sur l'emplacement %d - choisis %s", "%s en la ranura %d - ahora elige %s",
                          "%s nello slot %d - ora scegli %s", "%s op slot %d - kies nu %s",
                          "%s no slot %d - agora escolhe %s" },
    [S_A_BOTTOM]      = { "a bottom", "ein Unterteil", "un bas", "una parte inferior", "una parte inferiore",
                          "een onderstuk", "uma parte inferior" },
    [S_A_TOP]         = { "a top", "ein Oberteil", "un haut", "una parte superior", "una parte superiore",
                          "een bovenstuk", "uma parte superior" },
    [S_NO_FIGURES_IN] = { "No figures in %s", "Keine Figuren in %s", "Aucune figurine dans %s", "No hay figuras en %s",
                          "Nessuna statuina in %s", "Geen figuren in %s", "Nenhuma figura em %s" },
    [S_ICONS_LOADING] = { "Loading pictures %d%%", "Bilder werden geladen %d%%", "Chargement images %d%%",
                          "Cargando im\xC3\xA1genes %d%%", "Caricamento immagini %d%%", "Afbeeldingen laden %d%%",
                          "A carregar imagens %d%%" },
    [S_ICONS_FROM]    = { "Pictures: %s", "Bilder: %s", "Images : %s", "Im\xC3\xA1genes: %s", "Immagini: %s",
                          "Afbeeldingen: %s", "Imagens: %s" },
};

static const char *const k_codes[L_COUNT] = { "en", "de", "fr", "es", "it", "nl", "pt" };

void lang_init(int sys)
{
    // SceSystemServiceParamId LANG values
    switch (sys) {
    case 4:                  g_lang = L_DE; break;
    case 2: case 22:         g_lang = L_FR; break;
    case 3: case 20:         g_lang = L_ES; break;
    case 5:                  g_lang = L_IT; break;
    case 6:                  g_lang = L_NL; break;
    case 7: case 17:         g_lang = L_PT; break;
    default:                 g_lang = L_EN; break;
    }
}

const char *T(str_id id)
{
    const char *s = k_text[id][g_lang];
    return s ? s : k_text[id][L_EN];
}

const char *lang_code(void) { return k_codes[g_lang]; }
