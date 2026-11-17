#pragma once
#include <stdint.h>
#define X77_LAYOUT_ID 1

static uint8_t lighting_column(uint8_t row, uint8_t col)
{
    // The ISO key beside left Shift is wired to matrix column 14.
    // Animation geometry follows physical order; LED addressing stays unchanged.
    if (row == 4) {
        if (col == 14) return 1;
        if (col >= 1 && col <= 10) return col + 1;
    }
    return col;
}
