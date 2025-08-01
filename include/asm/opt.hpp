// SPDX-License-Identifier: MIT

#ifndef RGBDS_ASM_OPT_HPP
#define RGBDS_ASM_OPT_HPP

#include <stdint.h>

void opt_B(char const binDigits[2]);
void opt_G(char const gfxDigits[4]);
void opt_P(uint8_t padByte);
void opt_Q(uint8_t fixPrecision);
void opt_W(char const *flag);
void opt_Parse(char const *option);

void opt_Push();
void opt_Pop();
void opt_CheckStack();

#endif // RGBDS_ASM_OPT_HPP
