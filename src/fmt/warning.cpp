// SPDX-License-Identifier: MIT

#include "fmt/warning.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "style.hpp"

void fatal(char const *fmt, ...) {
	va_list ap;
	style_Set(stderr, STYLE_RED, true);
	fputs("FATAL: ", stderr);
	style_Reset(stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	putc('\n', stderr);
	exit(1);
}
