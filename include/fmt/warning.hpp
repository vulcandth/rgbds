// SPDX-License-Identifier: MIT

#ifndef RGBDS_FMT_WARNING_HPP
#define RGBDS_FMT_WARNING_HPP

#include <cstdarg>

[[gnu::format(printf, 1, 2), noreturn]]
void fatal(char const *fmt, ...);

#endif // RGBDS_FMT_WARNING_HPP
