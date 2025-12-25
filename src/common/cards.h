#ifndef CARDS_H
#define CARDS_H

#include "proto.h"

// Returns NULL if id invalid
const card_def_t* get_card_def(uint16_t id);

// Fills definitions array (optional if we expose global)
// We will just expose a getter.

#endif
