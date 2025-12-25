#include "cards.h"
#include <stddef.h>

// Static Card Definitions
// ID 0 is invalid/empty

static const card_def_t g_cards[] = {
    // ID, TYPE, COST, VALUE, DUR, NAME
    { 1, CT_ATK,    1, 6, 0, "Strike" },
    { 2, CT_ATK,    2, 12, 0, "Heavy Blow" },
    { 3, CT_ATK,    3, 20, 0, "Finisher" },
    
    { 4, CT_HEAL,   1, 5, 0, "Bandage" },
    { 5, CT_HEAL,   2, 12, 0, "Potion" },
    
    { 6, CT_SHIELD, 1, 5, 0, "Block" },
    { 7, CT_SHIELD, 2, 12, 0, "Iron Wall" },
    
    { 8, CT_BUFF,   2, 2, 3, "Strength" }, // +2 dmg for 3 turns (implied logic)
    { 9, CT_POISON, 2, 3, 3, "Poison" },   // 3 dmg for 3 turns
    
    { 10, CT_ATK,   0, 3, 0, "Quick Stab" },
    { 11, CT_SHIELD,0, 3, 0, "Dodge" }
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
