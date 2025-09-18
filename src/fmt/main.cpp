// SPDX-License-Identifier: MIT

#include "fmt/main.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diagnostics.hpp"
#include "extern/getopt.hpp"
#include "helpers.hpp"
#include "platform.hpp"
#include "style.hpp"
#include "usage.hpp"
#include "version.hpp"

#include "fmt/formatter.hpp"
#include "fmt/warning.hpp"

Options options;

namespace {

enum LongOption {
	OPT_INDENT_SPACES = 1,
	OPT_INDENT_TABS,
	OPT_INDENT_BASE,
	OPT_MAX_BLANK_LINES,
	OPT_TRIM_TRAILING,
	OPT_NO_TRIM_TRAILING,
};

// Short options
static char const *optstring = "hio:V";

static int longOpt;

static option const longopts[] = {
    {"help",                        no_argument,       nullptr,         'h'},
    {"in-place",                    no_argument,       nullptr,         'i'},
    {"output",                      required_argument, nullptr,         'o'},
    {"version",                     no_argument,       nullptr,         'V'},
    {"indent-spaces",               required_argument, &longOpt,        OPT_INDENT_SPACES},
    {"indent-tabs",                 no_argument,       &longOpt,        OPT_INDENT_TABS},
    {"indent-base",                 required_argument, &longOpt,        OPT_INDENT_BASE},
    {"max-blank-lines",             required_argument, &longOpt,        OPT_MAX_BLANK_LINES},
    {"trim-trailing-whitespace",    no_argument,       &longOpt,        OPT_TRIM_TRAILING},
    {"no-trim-trailing-whitespace", no_argument,       &longOpt,        OPT_NO_TRIM_TRAILING},
    {"color",                       required_argument, &longOpt,        'c'},
    {nullptr,                       no_argument,       nullptr,         0  },
};

// clang-format off: nested initializers
static Usage usage = {
    .name = "rgbfmt",
    .flags = {
        "[-i | -o <file>]",
        "[--indent-tabs | --indent-spaces <n>]",
        "[--indent-base <n>]",
        "[--max-blank-lines <n>]",
        "[--no-trim-trailing-whitespace]",
        "[file ...]",
    },
    .options = {
        {{"-i", "--in-place"}, {"rewrite each file in place"}},
        {{"-o", "--output <path>"}, {"write formatted output to <path>"}},
        {{"--indent-tabs"}, {"indent using tabs (default)"}},
        {{"--indent-spaces <n>"}, {"indent using <n> spaces per level"}},
        {{"--indent-base <n>"}, {"set the base indentation applied before nested blocks"}},
        {{"--max-blank-lines <n>"}, {"keep at most <n> consecutive blank lines (default 1)"}},
        {{"--trim-trailing-whitespace"}, {"remove trailing whitespace (default)"}},
        {{"--no-trim-trailing-whitespace"}, {"preserve trailing whitespace"}},
        {{"--color <mode>"}, {"set diagnostics color mode"}},
        {{"-V", "--version"}, {"print RGBFMT version and exit"}},
        {{"-h", "--help"}, {"display this help and exit"}},
    },
};
// clang-format on

std::size_t parseSize(char const *arg, char const *name, bool allowZero) {
	char *end = nullptr;
	errno = 0;
	unsigned long value = strtoul(arg, &end, 10);
	if (errno || end == arg || *end != '\0') {
		fatal("Invalid numeric argument for %s: '%s'", name, arg);
	}
	if (!allowZero && value == 0) {
		fatal("%s must be greater than zero", name);
	}
	return static_cast<std::size_t>(value);
}

std::string readAll(FILE *stream, char const *name) {
	std::string buffer;
	char chunk[4096];
	while (true) {
		size_t read = fread(chunk, 1, sizeof(chunk), stream);
		buffer.append(chunk, read);
		if (read < sizeof(chunk)) {
			if (ferror(stream)) {
				fatal("Failed to read '%s': %s", name, strerror(errno));
			}
			break;
		}
	}
	return buffer;
}

std::string loadFile(char const *path) {
	if (strcmp(path, "-") == 0) {
		(void)setmode(STDIN_FILENO, O_BINARY);
		return readAll(stdin, "<stdin>");
	}

	FILE *file = fopen(path, "rb");
	if (!file) {
		fatal("Failed to open \"%s\" for reading: %s", path, strerror(errno));
	}
	Defer closeFile{[&] { fclose(file); }};
	return readAll(file, path);
}

void writeFile(char const *path, std::string const &content) {
	FILE *file = fopen(path, "wb");
	if (!file) {
		fatal("Failed to open \"%s\" for writing: %s", path, strerror(errno));
	}
	Defer closeFile{[&] { fclose(file); }};
	if (!content.empty() && fwrite(content.data(), 1, content.size(), file) != content.size()) {
		fatal("Failed to write to \"%s\": %s", path, strerror(errno));
	}
}

void writeStdout(std::string const &content) {
	(void)setmode(STDOUT_FILENO, O_BINARY);
	if (!content.empty() && fwrite(content.data(), 1, content.size(), stdout) != content.size()) {
		fatal("Failed to write formatted output: %s", strerror(errno));
	}
}

} // namespace

int main(int argc, char *argv[]) {
	std::vector<char const *> inputs;

	while (true) {
		int c = musl_getopt_long_only(argc, argv, optstring, longopts, nullptr);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'h':
			usage.printAndExit(0);
			break;
		case 'i':
			options.inPlace = true;
			break;
		case 'o':
			options.output = musl_optarg;
			break;
		case 'V':
			printf("rgbfmt %s\n", get_package_version_string());
			return 0;
		case 0:
			switch (longOpt) {
			case OPT_INDENT_SPACES:
				options.useTabs = false;
				options.indentWidth = parseSize(musl_optarg, "--indent-spaces", false);
				break;
			case OPT_INDENT_TABS:
				options.useTabs = true;
				break;
			case OPT_INDENT_BASE:
				options.baseIndent = parseSize(musl_optarg, "--indent-base", false);
				break;
			case OPT_MAX_BLANK_LINES:
				options.maxConsecutiveBlankLines = parseSize(musl_optarg, "--max-blank-lines", true);
				break;
			case OPT_TRIM_TRAILING:
				options.trimTrailingWhitespace = true;
				break;
			case OPT_NO_TRIM_TRAILING:
				options.trimTrailingWhitespace = false;
				break;
			case 'c':
				if (!style_Parse(musl_optarg)) {
					fatal("Invalid argument for option '--color'");
				}
				break;
			default:
				usage.printAndExit(1);
			}
			break;
		default:
			usage.printAndExit(1);
		}
	}

	for (int i = musl_optind; i < argc; ++i) {
		inputs.push_back(argv[i]);
	}

	if (options.inPlace && options.output) {
		fatal("Options '--in-place' and '--output' are mutually exclusive");
	}

	if (options.output && inputs.size() != 1) {
		fatal("Option '--output' requires exactly one input file");
	}

	if (options.inPlace && inputs.empty()) {
		fatal("Option '--in-place' requires at least one input file");
	}

	if (inputs.size() > 1 && !options.inPlace) {
		fatal("Multiple input files require '--in-place'");
	}

	for (char const *path : inputs) {
		if (options.inPlace && strcmp(path, "-") == 0) {
			fatal("Cannot use '--in-place' with standard input");
		}
	}

	FormatterConfig config{
	    .useTabs = options.useTabs,
	    .indentWidth = options.indentWidth,
	    .baseIndent = options.baseIndent,
	    .maxConsecutiveBlankLines = options.maxConsecutiveBlankLines,
	    .trimTrailingWhitespace = options.trimTrailingWhitespace,
	};

	auto process = [&](char const *path, char const *dest) {
		std::string original = loadFile(path);
		std::string formatted = formatBuffer(original, config);

		if (strcmp(dest, "-") == 0) {
			writeStdout(formatted);
			return;
		}

		if (options.inPlace && formatted == original) {
			return;
		}

		writeFile(dest, formatted);
	};

	if (inputs.empty()) {
		process("-", "-");
		return 0;
	}

	if (options.inPlace) {
		for (char const *path : inputs) {
			process(path, path);
		}
	} else if (options.output) {
		process(inputs.front(), options.output);
	} else {
		process(inputs.front(), "-");
	}

	return 0;
}
