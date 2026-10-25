#pragma once
/* A 16-segment font. A cell is 16 bits, one per segment, in the S MK1 display's order
   (docs/hardware/s-mk1.md, Displays):

      --A1-- --A2--
     |\     |     /|      A1 0  A2 1  B 2   C 3   D2 4  D1 5  E 6   F 7
     F  H   I   J  B      H  8  I  9  J 10  G1 11 G2 12 K 13  L 14  M 15
     |    \ | /    |
      --G1-- --G2--       H, J, K, M are the diagonals from the centre to the
     |    / | \    |      top-left, top-right, bottom-left and bottom-right
     E  K   L   M  C      corners; I and L are the upper and lower centre bars.
     |/     |     \|
      --D1-- --D2--
*/
#include <stdint.h>

enum {
    SEG_A1 = 1 << 0, SEG_A2 = 1 << 1, SEG_B = 1 << 2, SEG_C = 1 << 3,
    SEG_D2 = 1 << 4, SEG_D1 = 1 << 5, SEG_E = 1 << 6, SEG_F = 1 << 7,
    SEG_H = 1 << 8, SEG_I = 1 << 9, SEG_J = 1 << 10, SEG_G1 = 1 << 11,
    SEG_G2 = 1 << 12, SEG_K = 1 << 13, SEG_L = 1 << 14, SEG_M = 1 << 15,
};

/* the segments of character c; lower case shows as upper case, unknown characters blank */
uint16_t font16(char c);
