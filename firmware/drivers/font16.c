/* The 16-segment glyphs (font16.h). Our own design: upper case, digits and the
   symbols the UI uses. The dot after a character is a separate segment, so '.' is
   blank here and ui/seg9.c lights the dot instead. tools/xcore/s49run.py reads this
   table to turn the emulated displays back into text, so keep one glyph per line. */
#include "font16.h"

#define A   (SEG_A1 | SEG_A2)
#define D   (SEG_D1 | SEG_D2)
#define G   (SEG_G1 | SEG_G2)

static const uint16_t glyphs[64] = {   /* ' ' .. '_' */
    [' ' - 32] = 0,
    ['!' - 32] = SEG_I,
    ['"' - 32] = SEG_F | SEG_I,
    ['#' - 32] = SEG_B | SEG_C | G | SEG_I | SEG_L,
    ['$' - 32] = A | SEG_F | G | SEG_C | D | SEG_I | SEG_L,
    ['%' - 32] = SEG_A1 | SEG_F | SEG_J | SEG_K | SEG_C | SEG_D2,
    ['&' - 32] = SEG_A1 | SEG_H | SEG_J | SEG_G1 | SEG_E | SEG_D1 | SEG_M,
    ['\'' - 32] = SEG_I,
    ['(' - 32] = SEG_A2 | SEG_I | SEG_L | SEG_D2,
    [')' - 32] = SEG_A1 | SEG_I | SEG_L | SEG_D1,
    ['*' - 32] = SEG_H | SEG_I | SEG_J | G | SEG_K | SEG_L | SEG_M,
    ['+' - 32] = G | SEG_I | SEG_L,
    [',' - 32] = SEG_K,
    ['-' - 32] = G,
    ['.' - 32] = 0,
    ['/' - 32] = SEG_J | SEG_K,
    ['0' - 32] = A | SEG_B | SEG_C | D | SEG_E | SEG_F | SEG_J | SEG_K,
    ['1' - 32] = SEG_B | SEG_C | SEG_J,
    ['2' - 32] = A | SEG_B | G | SEG_E | D,
    ['3' - 32] = A | SEG_B | SEG_G2 | SEG_C | D,
    ['4' - 32] = SEG_F | G | SEG_B | SEG_C,
    ['5' - 32] = A | SEG_F | G | SEG_C | D,
    ['6' - 32] = A | SEG_F | SEG_E | D | SEG_C | G,
    ['7' - 32] = A | SEG_B | SEG_C,
    ['8' - 32] = A | SEG_B | SEG_C | D | SEG_E | SEG_F | G,
    ['9' - 32] = A | SEG_B | SEG_C | D | SEG_F | G,
    [':' - 32] = SEG_I | SEG_L,
    [';' - 32] = SEG_I | SEG_K,
    ['<' - 32] = SEG_J | SEG_M,
    ['=' - 32] = G | D,
    ['>' - 32] = SEG_H | SEG_K,
    ['?' - 32] = A | SEG_B | SEG_G2 | SEG_L,
    ['@' - 32] = A | SEG_B | SEG_G2 | SEG_I | SEG_E | SEG_F | D,
    ['A' - 32] = A | SEG_B | SEG_C | SEG_E | SEG_F | G,
    ['B' - 32] = A | SEG_B | SEG_C | D | SEG_G2 | SEG_I | SEG_L,
    ['C' - 32] = A | SEG_E | SEG_F | D,
    ['D' - 32] = A | SEG_B | SEG_C | D | SEG_I | SEG_L,
    ['E' - 32] = A | SEG_E | SEG_F | D | SEG_G1,
    ['F' - 32] = A | SEG_E | SEG_F | SEG_G1,
    ['G' - 32] = A | SEG_F | SEG_E | D | SEG_C | SEG_G2,
    ['H' - 32] = SEG_B | SEG_C | SEG_E | SEG_F | G,
    ['I' - 32] = A | D | SEG_I | SEG_L,
    ['J' - 32] = SEG_B | SEG_C | D | SEG_E,
    ['K' - 32] = SEG_E | SEG_F | SEG_G1 | SEG_J | SEG_M,
    ['L' - 32] = SEG_E | SEG_F | D,
    ['M' - 32] = SEG_B | SEG_C | SEG_E | SEG_F | SEG_H | SEG_J,
    ['N' - 32] = SEG_B | SEG_C | SEG_E | SEG_F | SEG_H | SEG_M,
    ['O' - 32] = A | SEG_B | SEG_C | D | SEG_E | SEG_F,
    ['P' - 32] = A | SEG_B | SEG_E | SEG_F | G,
    ['Q' - 32] = A | SEG_B | SEG_C | D | SEG_E | SEG_F | SEG_M,
    ['R' - 32] = A | SEG_B | SEG_E | SEG_F | G | SEG_M,
    ['S' - 32] = A | SEG_H | SEG_G2 | SEG_C | D,
    ['T' - 32] = A | SEG_I | SEG_L,
    ['U' - 32] = SEG_B | SEG_C | D | SEG_E | SEG_F,
    ['V' - 32] = SEG_E | SEG_F | SEG_J | SEG_K,
    ['W' - 32] = SEG_B | SEG_C | SEG_E | SEG_F | SEG_K | SEG_M,
    ['X' - 32] = SEG_H | SEG_J | SEG_K | SEG_M,
    ['Y' - 32] = SEG_H | SEG_J | SEG_L,
    ['Z' - 32] = A | D | SEG_J | SEG_K,
    ['[' - 32] = SEG_A1 | SEG_F | SEG_E | SEG_D1,
    ['\\' - 32] = SEG_H | SEG_M,
    [']' - 32] = SEG_A2 | SEG_B | SEG_C | SEG_D2,
    ['^' - 32] = SEG_K | SEG_M,
    ['_' - 32] = D,
};

uint16_t font16(char c)
{
    if (c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    if (c == '|')
        return SEG_I | SEG_L;
    if (c < 32 || c > '_')
        return 0;
    return glyphs[c - 32];
}
