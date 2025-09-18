// SPDX-License-Identifier: MIT

#ifndef RGBDS_FMT_FORMATTER_HPP
#define RGBDS_FMT_FORMATTER_HPP

#include <cstddef>
#include <string>
#include <string_view>

struct FormatterConfig {
	bool useTabs = true;                     // Prefer tabs for indentation
	std::size_t indentWidth = 4;             // Number of spaces per indent level when tabs are disabled
	std::size_t baseIndent = 1;              // Base indentation applied to instruction lines
	std::size_t maxConsecutiveBlankLines = 1;// Maximum blank lines kept consecutively
	bool trimTrailingWhitespace = true;      // Remove trailing spaces and tabs
};

class Formatter {
public:
	explicit Formatter(FormatterConfig const &config);

	// Format the provided assembly buffer and return the formatted text.
	std::string format(std::string_view input);

private:
	FormatterConfig config;
};

std::string formatBuffer(std::string_view input, FormatterConfig const &config);

#endif // RGBDS_FMT_FORMATTER_HPP
