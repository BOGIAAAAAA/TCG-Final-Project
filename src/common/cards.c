#include "cards.h"
#include <stddef.h>

// Static Card Definitions
// User's New Card System (IDs 100+)

static const card_def_t g_cards[] = {
    // ID, TYPE, COST, VALUE, DUR, NAME
    // ATK
    { 100, CT_ATK,    1, 3, 0, "Slash" },
    { 101, CT_ATK,    2, 5, 0, "Heavy Hit" },
    { 102, CT_ATK,    3, 8, 0, "Execute" },
    
    // HEAL
    { 200, CT_HEAL,   2, 4, 0, "Bandage" },
    { 201, CT_HEAL,   3, 7, 0, "Potion" },
    
    // SHIELD
    { 300, CT_SHIELD, 1, 3, 0, "Block" },
    { 301, CT_SHIELD, 2, 6, 0, "Barrier" },
    
    // BUFF (Value = amount to add to next attack)
    { 400, CT_BUFF,   1, 2, 0, "Sharpen" },
    { 401, CT_BUFF,   2, 4, 0, "Empower" },
    
    // POISON (Value = turns to add)
    { 500, CT_POISON, 2, 2, 0, "Toxic Dagger" },
    { 501, CT_POISON, 3, 3, 0, "Venom" }
};

#define CARD_COUNT (sizeof(g_cards)/sizeof(g_cards[0]))

const card_def_t* get_card_def(uint16_t id) {
    for (size_t i = 0; i < CARD_COUNT; i++) {
        if (g_cards[i].id == id) {
            return &g_cards[i];
        }
    }
    return NULL;
}
