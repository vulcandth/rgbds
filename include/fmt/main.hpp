// SPDX-License-Identifier: MIT

#ifndef RGBDS_FMT_MAIN_HPP
#define RGBDS_FMT_MAIN_HPP

#include <cstddef>

struct Options {
	bool inPlace = false;                    // Rewrite files in-place
	char const *output = nullptr;           // Requested output file when formatting a single input
	bool useTabs = true;                    // Indentation style
	std::size_t indentWidth = 4;            // Spaces per indent level when tabs are disabled
	std::size_t baseIndent = 1;             // Base indentation depth for instruction lines
	std::size_t maxConsecutiveBlankLines = 1; // Limit of consecutive blank lines to preserve
	bool trimTrailingWhitespace = true;     // Trim trailing whitespace from each line
};

extern Options options;

#endif // RGBDS_FMT_MAIN_HPP
