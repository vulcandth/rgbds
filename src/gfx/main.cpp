// SPDX-License-Identifier: MIT

#include "gfx/main.hpp"

#include <algorithm>
#include <ctype.h>
#include <inttypes.h>
#include <ios>
#include <limits>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"
#include "extern/getopt.hpp"
#include "file.hpp"
#include "platform.hpp"
#include "usage.hpp"
#include "version.hpp"

#include "gfx/pal_spec.hpp"
#include "gfx/process.hpp"
#include "gfx/reverse.hpp"
#include "gfx/warning.hpp"

using namespace std::literals::string_view_literals;

Options options;

static struct LocalOptions {
	char const *externalPalSpec;
	bool autoAttrmap;
	bool autoTilemap;
	bool autoPalettes;
	bool autoPalmap;
	bool groupOutputs;
	bool reverse;
} localOptions;

void Options::verbosePrint(uint8_t level, char const *fmt, ...) const {
	// LCOV_EXCL_START
	if (verbosity >= level) {
		va_list ap;

		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
	// LCOV_EXCL_STOP
}

// Short options
static char const *optstring = "-Aa:B:b:Cc:d:hi:L:l:mN:n:Oo:Pp:Qq:r:s:Tt:U:uVvW:wXx:YZ";

// Equivalent long options
// Please keep in the same order as short opts.
// Also, make sure long opts don't create ambiguity:
// A long opt's name should start with the same letter as its short opt,
// except if it doesn't create any ambiguity (`verbose` versus `version`).
// This is because long opt matching, even to a single char, is prioritized
// over short opt matching.
static option const longopts[] = {
    {"auto-attr-map",    no_argument,       nullptr, 'A'},
    {"attr-map",         required_argument, nullptr, 'a'},
    {"background-color", required_argument, nullptr, 'B'},
    {"base-tiles",       required_argument, nullptr, 'b'},
    {"color-curve",      no_argument,       nullptr, 'C'},
    {"colors",           required_argument, nullptr, 'c'},
    {"depth",            required_argument, nullptr, 'd'},
    {"help",             no_argument,       nullptr, 'h'},
    {"input-tileset",    required_argument, nullptr, 'i'},
    {"slice",            required_argument, nullptr, 'L'},
    {"base-palette",     required_argument, nullptr, 'l'},
    {"mirror-tiles",     no_argument,       nullptr, 'm'},
    {"nb-tiles",         required_argument, nullptr, 'N'},
    {"nb-palettes",      required_argument, nullptr, 'n'},
    {"group-outputs",    no_argument,       nullptr, 'O'},
    {"output",           required_argument, nullptr, 'o'},
    {"auto-palette",     no_argument,       nullptr, 'P'},
    {"palette",          required_argument, nullptr, 'p'},
    {"auto-palette-map", no_argument,       nullptr, 'Q'},
    {"palette-map",      required_argument, nullptr, 'q'},
    {"reverse",          required_argument, nullptr, 'r'},
    {"palette-size",     required_argument, nullptr, 's'},
    {"auto-tilemap",     no_argument,       nullptr, 'T'},
    {"tilemap",          required_argument, nullptr, 't'},
    {"unit-size",        required_argument, nullptr, 'U'},
    {"unique-tiles",     no_argument,       nullptr, 'u'},
    {"version",          no_argument,       nullptr, 'V'},
    {"verbose",          no_argument,       nullptr, 'v'},
    {"warning",          required_argument, nullptr, 'W'},
    {"mirror-x",         no_argument,       nullptr, 'X'},
    {"trim-end",         required_argument, nullptr, 'x'},
    {"mirror-y",         no_argument,       nullptr, 'Y'},
    {"columns",          no_argument,       nullptr, 'Z'},
    {nullptr,            no_argument,       nullptr, 0  }
};

// clang-format off: long string literal
static Usage usage(
    "Usage: rgbgfx [-r stride] [-ChmOuVXYZ] [-v [-v ...]] [-a <attr_map> | -A]\n"
    "       [-b <base_ids>] [-c <colors>] [-d <depth>] [-i <tileset_file>]\n"
    "       [-L <slice>] [-l <base_pal>] [-N <nb_tiles>] [-n <nb_pals>]\n"
    "       [-o <out_file>] [-p <pal_file> | -P] [-q <pal_map> | -Q]\n"
    "       [-s <nb_colors>] [-t <tile_map> | -T] [-x <nb_tiles>] <file>\n"
    "Useful options:\n"
    "    -m, --mirror-tiles    optimize out mirrored tiles\n"
    "    -o, --output <path>   output the tile data to this path\n"
    "    -t, --tilemap <path>  output the tile map to this path\n"
    "    -u, --unique-tiles    optimize out identical tiles\n"
    "    -V, --version         print RGBGFX version and exit\n"
    "\n"
    "For help, use `man rgbgfx' or go to https://rgbds.gbdev.io/docs/\n"
);
// clang-format on

// Parses a number at the beginning of a string, moving the pointer to skip the parsed characters.
// Returns the provided errVal on error.
static uint16_t parseNumber(char *&string, char const *errPrefix, uint16_t errVal = UINT16_MAX) {
	uint8_t base = 10;
	if (*string == '\0') {
		error("%s: expected number, but found nothing", errPrefix);
		return errVal;
	} else if (*string == '$') {
		base = 16;
		++string;
	} else if (*string == '%') {
		base = 2;
		++string;
	} else if (*string == '0' && string[1] != '\0') {
		// Check if we have a "0x" or "0b" here
		if (string[1] == 'x' || string[1] == 'X') {
			base = 16;
			string += 2;
		} else if (string[1] == 'b' || string[1] == 'B') {
			base = 2;
			string += 2;
		}
	}

	// Turns a digit into its numeric value in the current base, if it has one.
	// Maximum is inclusive. The string_view is modified to "consume" all digits.
	// Returns 255 on parse failure (including wrong char for base), in which case
	// the string_view may be pointing on garbage.
	auto charIndex = [&base](unsigned char c) -> uint8_t {
		unsigned char index = c - '0'; // Use wrapping semantics
		if (base == 2 && index >= 2) {
			return 255;
		} else if (index < 10) {
			return index;
		} else if (base != 16) {
			return 255; // Letters are only valid in hex
		}
		index = tolower(c) - 'a'; // OK because we pass an `unsigned char`
		if (index < 6) {
			return index + 10;
		}
		return 255;
	};

	if (charIndex(*string) == 255) {
		error(
		    "%s: expected digit%s, but found nothing", errPrefix, base != 10 ? " after base" : ""
		);
		return errVal;
	}
	uint16_t number = 0;
	do {
		// Read a character, and check if it's valid in the given base
		uint8_t index = charIndex(*string);
		if (index == 255) {
			break; // Found an invalid character, end
		}
		++string;

		number *= base;
		number += index;
		// The lax check covers the addition on top of the multiplication
		if (number >= UINT16_MAX / base) {
			error("%s: the number is too large!", errPrefix);
			return errVal;
		}
	} while (*string != '\0'); // No more characters?

	return number;
}

static void skipWhitespace(char *&arg) {
	arg += strspn(arg, " \t");
}

static void registerInput(char const *arg) {
	if (!options.input.empty()) {
		usage.printAndExit(
		    "Input image specified more than once! (first \"%s\", then \"%s\")",
		    options.input.c_str(),
		    arg
		);
	} else if (arg[0] == '\0') { // Empty input path
		usage.printAndExit("Input image path cannot be empty");
	} else {
		options.input = arg;
	}
}

// Turn an at-file's contents into an argv that `getopt` can handle, appending them to `argPool`.
static std::vector<size_t> readAtFile(std::string const &path, std::vector<char> &argPool) {
	File file;
	if (!file.open(path, std::ios_base::in)) {
		fatal("Error reading @%s: %s", file.c_str(path), strerror(errno));
	}

	// We only filter out `EOF`, but calling `isblank()` on anything else is UB!
	static_assert(
	    std::streambuf::traits_type::eof() == EOF,
	    "isblank(std::streambuf::traits_type::eof()) is UB!"
	);
	std::vector<size_t> argvOfs;

	for (;;) {
		int c;

		// First, discard any leading whitespace
		do {
			c = file->sbumpc();
			if (c == EOF) {
				return argvOfs;
			}
		} while (isblank(c));

		switch (c) {
		case '#': // If it's a comment, discard everything until EOL
			while ((c = file->sbumpc()) != '\n') {
				if (c == EOF) {
					return argvOfs;
				}
			}
			continue; // Start processing the next line
		// If it's an empty line, ignore it
		case '\r':          // Assuming CRLF here
			file->sbumpc(); // Discard the upcoming '\n'
			[[fallthrough]];
		case '\n':
			continue; // Start processing the next line
		}

		// Alright, now we can parse the line
		do {
			// Read one argument (until the next whitespace char).
			// We know there is one because we already have its first character in `c`.
			argvOfs.push_back(argPool.size());
			// Reading and appending characters one at a time may be inefficient, but I'm counting
			// on `vector` and `sbumpc` to do the right thing here.
			argPool.push_back(c); // Push the character we've already read
			for (;;) {
				c = file->sbumpc();
				if (c == EOF || c == '\n' || isblank(c)) {
					break;
				} else if (c == '\r') {
					file->sbumpc(); // Discard the '\n'
					break;
				}
				argPool.push_back(c);
			}
			argPool.push_back('\0');

			// Discard whitespace until the next argument (candidate)
			while (isblank(c)) {
				c = file->sbumpc();
			}
			if (c == '\r') {
				c = file->sbumpc(); // Skip the '\n'
			}
		} while (c != '\n' && c != EOF); // End if we reached EOL
	}
}

// Parses an arg vector, modifying `options` and `localOptions` as options are read.
// The `localOptions` struct is for flags which must be processed after the option parsing finishes.
// Returns `nullptr` if the vector was fully parsed, or a pointer (which is part of the arg vector)
// to an "at-file" path if one is encountered.
static char *parseArgv(int argc, char *argv[]) {
	for (int ch; (ch = musl_getopt_long_only(argc, argv, optstring, longopts, nullptr)) != -1;) {
		char *arg = musl_optarg; // Make a copy for scanning
		uint16_t number;
		switch (ch) {
		case 'A':
			localOptions.autoAttrmap = true;
			break;
		case 'a':
			localOptions.autoAttrmap = false;
			if (!options.attrmap.empty()) {
				warnx("Overriding attrmap file %s", options.attrmap.c_str());
			}
			options.attrmap = musl_optarg;
			break;
		case 'B':
			parseBackgroundPalSpec(musl_optarg);
			break;
		case 'b':
			number = parseNumber(arg, "Bank 0 base tile ID", 0);
			if (number >= 256) {
				error("Bank 0 base tile ID must be below 256");
			} else {
				options.baseTileIDs[0] = number;
			}
			if (*arg == '\0') {
				options.baseTileIDs[1] = 0;
				break;
			}
			skipWhitespace(arg);
			if (*arg != ',') {
				error(
				    "Base tile IDs must be one or two comma-separated numbers, not \"%s\"",
				    musl_optarg
				);
				break;
			}
			++arg; // Skip comma
			skipWhitespace(arg);
			number = parseNumber(arg, "Bank 1 base tile ID", 0);
			if (number >= 256) {
				error("Bank 1 base tile ID must be below 256");
			} else {
				options.baseTileIDs[1] = number;
			}
			if (*arg != '\0') {
				error(
				    "Base tile IDs must be one or two comma-separated numbers, not \"%s\"",
				    musl_optarg
				);
				break;
			}
			break;
		case 'C':
			options.useColorCurve = true;
			break;
		case 'c':
			localOptions.externalPalSpec = nullptr; // Allow overriding a previous pal spec
			if (musl_optarg[0] == '#') {
				options.palSpecType = Options::EXPLICIT;
				parseInlinePalSpec(musl_optarg);
			} else if (strcasecmp(musl_optarg, "embedded") == 0) {
				// Use PLTE, error out if missing
				options.palSpecType = Options::EMBEDDED;
			} else if (strncasecmp(musl_optarg, "dmg=", literal_strlen("dmg=")) == 0) {
				options.palSpecType = Options::DMG;
				parseDmgPalSpec(&musl_optarg[literal_strlen("dmg=")]);
			} else {
				options.palSpecType = Options::EXPLICIT;
				localOptions.externalPalSpec = musl_optarg;
			}
			break;
		case 'd':
			options.bitDepth = parseNumber(arg, "Bit depth", 2);
			if (*arg != '\0') {
				error("Bit depth (-b) argument must be a valid number, not \"%s\"", musl_optarg);
			} else if (options.bitDepth != 1 && options.bitDepth != 2) {
				error("Bit depth must be 1 or 2, not %" PRIu8, options.bitDepth);
				options.bitDepth = 2;
			}
			break;
		case 'h':
			usage.printAndExit(0); // LCOV_EXCL_LINE
		case 'i':
			if (!options.inputTileset.empty()) {
				warnx("Overriding input tileset file %s", options.inputTileset.c_str());
			}
			options.inputTileset = musl_optarg;
			break;
		case 'L':
			options.inputSlice.left = parseNumber(arg, "Input slice left coordinate");
			if (options.inputSlice.left > INT16_MAX) {
				error("Input slice left coordinate is out of range!");
				break;
			}
			skipWhitespace(arg);
			if (*arg != ',') {
				error("Missing comma after left coordinate in \"%s\"", musl_optarg);
				break;
			}
			++arg;
			skipWhitespace(arg);
			options.inputSlice.top = parseNumber(arg, "Input slice upper coordinate");
			skipWhitespace(arg);
			if (*arg != ':') {
				error("Missing colon after upper coordinate in \"%s\"", musl_optarg);
				break;
			}
			++arg;
			skipWhitespace(arg);
			options.inputSlice.width = parseNumber(arg, "Input slice width");
			skipWhitespace(arg);
			if (options.inputSlice.width == 0) {
				error("Input slice width may not be 0!");
			}
			if (*arg != ',') {
				error("Missing comma after width in \"%s\"", musl_optarg);
				break;
			}
			++arg;
			skipWhitespace(arg);
			options.inputSlice.height = parseNumber(arg, "Input slice height");
			if (options.inputSlice.height == 0) {
				error("Input slice height may not be 0!");
			}
			if (*arg != '\0') {
				error("Unexpected extra characters after slice spec in \"%s\"", musl_optarg);
			}
			break;
		case 'l':
			number = parseNumber(arg, "Base palette ID", 0);
			if (*arg != '\0') {
				error("Base palette ID must be a valid number, not \"%s\"", musl_optarg);
			} else if (number >= 256) {
				error("Base palette ID must be below 256");
			} else {
				options.basePalID = number;
			}
			break;
		case 'm':
			options.allowMirroringX = true; // Imply `-X`
			options.allowMirroringY = true; // Imply `-Y`
			[[fallthrough]];                // Imply `-u`
		case 'u':
			options.allowDedup = true;
			break;
		case 'N':
			options.maxNbTiles[0] = parseNumber(arg, "Number of tiles in bank 0", 256);
			if (options.maxNbTiles[0] > 256) {
				error("Bank 0 cannot contain more than 256 tiles");
			}
			if (*arg == '\0') {
				options.maxNbTiles[1] = 0;
				break;
			}
			skipWhitespace(arg);
			if (*arg != ',') {
				error(
				    "Bank capacity must be one or two comma-separated numbers, not \"%s\"",
				    musl_optarg
				);
				break;
			}
			++arg; // Skip comma
			skipWhitespace(arg);
			options.maxNbTiles[1] = parseNumber(arg, "Number of tiles in bank 1", 256);
			if (options.maxNbTiles[1] > 256) {
				error("Bank 1 cannot contain more than 256 tiles");
			}
			if (*arg != '\0') {
				error(
				    "Bank capacity must be one or two comma-separated numbers, not \"%s\"",
				    musl_optarg
				);
				break;
			}
			break;
		case 'n':
			number = parseNumber(arg, "Number of palettes", 256);
			if (*arg != '\0') {
				error("Number of palettes (-n) must be a valid number, not \"%s\"", musl_optarg);
			}
			if (number > 256) {
				error("Number of palettes (-n) must not exceed 256!");
			} else if (number == 0) {
				error("Number of palettes (-n) may not be 0!");
			} else {
				options.nbPalettes = number;
			}
			break;
		case 'O':
			localOptions.groupOutputs = true;
			break;
		case 'o':
			if (!options.output.empty()) {
				warnx("Overriding tile data file %s", options.output.c_str());
			}
			options.output = musl_optarg;
			break;
		case 'P':
			localOptions.autoPalettes = true;
			break;
		case 'p':
			localOptions.autoPalettes = false;
			if (!options.palettes.empty()) {
				warnx("Overriding palettes file %s", options.palettes.c_str());
			}
			options.palettes = musl_optarg;
			break;
		case 'Q':
			localOptions.autoPalmap = true;
			break;
		case 'q':
			localOptions.autoPalmap = false;
			if (!options.palmap.empty()) {
				warnx("Overriding palette map file %s", options.palmap.c_str());
			}
			options.palmap = musl_optarg;
			break;
		case 'r':
			localOptions.reverse = true;
			options.reversedWidth = parseNumber(arg, "Reversed image stride");
			if (*arg != '\0') {
				error("Reversed image stride (-r) must be a valid number, not \"%s\"", musl_optarg);
			}
			break;
		case 's':
			options.nbColorsPerPal = parseNumber(arg, "Number of colors per palette", 4);
			if (*arg != '\0') {
				error("Palette size (-s) must be a valid number, not \"%s\"", musl_optarg);
			}
			if (options.nbColorsPerPal > 4) {
				error("Palette size (-s) must not exceed 4!");
			} else if (options.nbColorsPerPal == 0) {
				error("Palette size (-s) may not be 0!");
			}
			break;
		case 'T':
			localOptions.autoTilemap = true;
			break;
		case 't':
			localOptions.autoTilemap = false;
			if (!options.tilemap.empty()) {
				warnx("Overriding tilemap file %s", options.tilemap.c_str());
			}
			options.tilemap = musl_optarg;
			break;
		case 'V':
			// LCOV_EXCL_START
			printf("rgbgfx %s\n", get_package_version_string());
			exit(0);
			// LCOV_EXCL_STOP
		case 'v':
			// LCOV_EXCL_START
			if (options.verbosity < Options::VERB_VVVVVV) {
				++options.verbosity;
			}
			break;
			// LCOV_EXCL_STOP
		case 'W':
			warnings.processWarningFlag(musl_optarg);
			break;
		case 'w':
			warnings.state.warningsEnabled = false;
			break;
		case 'x':
			options.trim = parseNumber(arg, "Number of tiles to trim", 0);
			if (*arg != '\0') {
				error("Tile trim (-x) argument must be a valid number, not \"%s\"", musl_optarg);
			}
			break;
		case 'X':
			options.allowMirroringX = true;
			options.allowDedup = true; // Imply `-u`
			break;
		case 'Y':
			options.allowMirroringY = true;
			options.allowDedup = true; // Imply `-u`
			break;
		case 'Z':
			options.columnMajor = true;
			break;
		case 1: // Positional argument, requested by leading `-` in opt string
			if (musl_optarg[0] == '@') {
				// Instruct the caller to process that at-file
				return &musl_optarg[1];
			} else {
				registerInput(musl_optarg);
			}
			break;
		default:
			usage.printAndExit(1); // LCOV_EXCL_LINE
		}
	}

	return nullptr; // Done processing this argv
}

static void verboseOutputConfig() {
	fprintf(stderr, "rgbgfx %s\n", get_package_version_string());

	if (options.verbosity >= Options::VERB_VVVVVV) {
		putc('\n', stderr);
		// clang-format off: vertically align values
		static std::array<uint16_t, 21> gfx{
		    0b0111111110,
		    0b1111111111,
		    0b1110011001,
		    0b1110011001,
		    0b1111111111,
		    0b1111111111,
		    0b1110000001,
		    0b1111000011,
		    0b0111111110,
		    0b0001111000,
		    0b0111111110,
		    0b1111111111,
		    0b1111111111,
		    0b1111111111,
		    0b1101111011,
		    0b1101111011,
		    0b0011111100,
		    0b0011001100,
		    0b0111001110,
		    0b0111001110,
		    0b0111001110,
		};
		// clang-format on
		static std::array<char const *, 3> textbox{
		    "  ,----------------------------------------.",
		    "  | Augh, dimensional interference again?! |",
		    "  `----------------------------------------'",
		};
		for (size_t i = 0; i < gfx.size(); ++i) {
			uint16_t row = gfx[i];
			for (uint8_t _ = 0; _ < 10; ++_) {
				unsigned char c = row & 1 ? '0' : ' ';
				putc(c, stderr);
				// Double the pixel horizontally, otherwise the aspect ratio looks wrong
				putc(c, stderr);
				row >>= 1;
			}
			if (i < textbox.size()) {
				fputs(textbox[i], stderr);
			}
			putc('\n', stderr);
		}
		putc('\n', stderr);
	}

	fputs("Options:\n", stderr);
	if (options.columnMajor) {
		fputs("\tVisit image in column-major order\n", stderr);
	}
	if (options.allowDedup) {
		fputs("\tAllow deduplicating tiles\n", stderr);
	}
	if (options.allowMirroringX) {
		fputs("\tAllow deduplicating horizontally mirrored tiles\n", stderr);
	}
	if (options.allowMirroringY) {
		fputs("\tAllow deduplicating vertically mirrored tiles\n", stderr);
	}
	if (options.useColorCurve) {
		fputs("\tUse color curve\n", stderr);
	}
	fprintf(stderr, "\tBit depth: %" PRIu8 "bpp\n", options.bitDepth);
	if (options.trim != 0) {
		fprintf(stderr, "\tTrim the last %" PRIu64 " tiles\n", options.trim);
	}
	fprintf(stderr, "\tMaximum %" PRIu16 " palettes\n", options.nbPalettes);
	fprintf(stderr, "\tPalettes contain %" PRIu8 " colors\n", options.nbColorsPerPal);
	fprintf(stderr, "\t%s palette spec\n", [] {
		switch (options.palSpecType) {
		case Options::NO_SPEC:
			return "No";
		case Options::EXPLICIT:
			return "Explicit";
		case Options::EMBEDDED:
			return "Embedded";
		case Options::DMG:
			return "DMG";
		}
		return "???";
	}());
	if (options.palSpecType == Options::EXPLICIT) {
		fputs("\t[\n", stderr);
		for (auto const &pal : options.palSpec) {
			fputs("\t\t", stderr);
			for (auto const &color : pal) {
				if (color) {
					fprintf(stderr, "#%06x, ", color->toCSS() >> 8);
				} else {
					fputs("#none, ", stderr);
				}
			}
			putc('\n', stderr);
		}
		fputs("\t]\n", stderr);
	}
	fprintf(
	    stderr,
	    "\tInput image slice: %" PRIu16 "x%" PRIu16 " pixels starting at (%" PRIu16 ", %" PRIu16
	    ")\n",
	    options.inputSlice.width,
	    options.inputSlice.height,
	    options.inputSlice.left,
	    options.inputSlice.top
	);
	fprintf(
	    stderr,
	    "\tBase tile IDs: [%" PRIu8 ", %" PRIu8 "]\n",
	    options.baseTileIDs[0],
	    options.baseTileIDs[1]
	);
	fprintf(stderr, "\tBase palette ID: %" PRIu8 "\n", options.basePalID);
	fprintf(
	    stderr,
	    "\tMaximum %" PRIu16 " tiles in bank 0, %" PRIu16 " in bank 1\n",
	    options.maxNbTiles[0],
	    options.maxNbTiles[1]
	);
	auto printPath = [](char const *name, std::string const &path) {
		if (!path.empty()) {
			fprintf(stderr, "\t%s: %s\n", name, path.c_str());
		}
	};
	printPath("Input image", options.input);
	printPath("Output tile data", options.output);
	printPath("Output tilemap", options.tilemap);
	printPath("Output attrmap", options.attrmap);
	printPath("Output palettes", options.palettes);
	fputs("Ready.\n", stderr);
}

// Manual implementation of std::filesystem::path.replace_extension().
// macOS <10.15 did not support std::filesystem::path.
static void replaceExtension(std::string &path, char const *extension) {
	constexpr std::string_view chars =
// Both must start with a dot!
#if defined(_MSC_VER) || defined(__MINGW32__)
	    "./\\"sv;
#else
	    "./"sv;
#endif
	size_t len = path.npos;
	if (size_t i = path.find_last_of(chars); i != path.npos && path[i] == '.') {
		// We found the last dot, but check if it's part of a stem
		// (There must be a non-path separator character before it)
		if (i != 0 && chars.find(path[i - 1], 1) == chars.npos) {
			// We can replace the extension
			len = i;
		}
	}
	path.assign(path, 0, len);
	path.append(extension);
}

int main(int argc, char *argv[]) {
	struct AtFileStackEntry {
		int parentInd;            // Saved offset into parent argv
		std::vector<char *> argv; // This context's arg pointer vec

		AtFileStackEntry(int parentInd_, std::vector<char *> argv_)
		    : parentInd(parentInd_), argv(argv_) {}
	};
	std::vector<AtFileStackEntry> atFileStack;

	int curArgc = argc;
	char **curArgv = argv;
	std::vector<std::vector<char>> argPools;
	for (;;) {
		char *atFileName = parseArgv(curArgc, curArgv);
		if (atFileName) {
			// We need to allocate a new arg pool for each at-file, so as not to invalidate pointers
			// previous at-files may have generated to their own arg pools.
			// But for the same reason, the arg pool must also outlive the at-file's stack entry!
			std::vector<char> &argPool = argPools.emplace_back();

			// Copy `argv[0]` for error reporting, and because option parsing skips it
			AtFileStackEntry &stackEntry =
			    atFileStack.emplace_back(musl_optind, std::vector{atFileName});
			// It would be nice to compute the char pointers on the fly, but reallocs don't allow
			// that; so we must compute the offsets after the pool is fixed
			std::vector<size_t> offsets = readAtFile(&musl_optarg[1], argPool);
			stackEntry.argv.reserve(offsets.size() + 2); // Avoid a bunch of reallocs
			for (size_t ofs : offsets) {
				stackEntry.argv.push_back(&argPool.data()[ofs]);
			}
			stackEntry.argv.push_back(nullptr); // Don't forget the arg vector terminator!

			curArgc = stackEntry.argv.size() - 1;
			curArgv = stackEntry.argv.data();
			musl_optind = 1; // Don't use 0 because we're not scanning a different argv per se
			continue;        // Begin scanning that arg vector
		}

		if (musl_optind != curArgc) {
			// This happens if `--` is passed, process the remaining arg(s) as positional
			assume(musl_optind < curArgc);
			for (int i = musl_optind; i < curArgc; ++i) {
				registerInput(argv[i]);
			}
		}

		// Pop off the top stack entry, or end parsing if none
		if (atFileStack.empty()) {
			break;
		}
		// OK to restore `optind` directly, because `optpos` must be 0 right now.
		// (Providing 0 would be a "proper" reset, but we want to resume parsing)
		musl_optind = atFileStack.back().parentInd;
		atFileStack.pop_back();
		if (atFileStack.empty()) {
			curArgc = argc;
			curArgv = argv;
		} else {
			std::vector<char *> &vec = atFileStack.back().argv;
			curArgc = vec.size();
			curArgv = vec.data();
		}
	}

	if (options.nbColorsPerPal == 0) {
		options.nbColorsPerPal = 1u << options.bitDepth;
	} else if (options.nbColorsPerPal > 1u << options.bitDepth) {
		error(
		    "%" PRIu8 "bpp palettes can only contain %u colors, not %" PRIu8,
		    options.bitDepth,
		    1u << options.bitDepth,
		    options.nbColorsPerPal
		);
	}

	auto autoOutPath = [](bool autoOptEnabled, std::string &path, char const *extension) {
		if (!autoOptEnabled) {
			return;
		}
		path = localOptions.groupOutputs ? options.output : options.input;
		if (path.empty()) {
			usage.printAndExit(
			    "No %s specified",
			    localOptions.groupOutputs ? "output tile data file" : "input image"
			);
		}
		replaceExtension(path, extension);
	};
	autoOutPath(localOptions.autoAttrmap, options.attrmap, ".attrmap");
	autoOutPath(localOptions.autoTilemap, options.tilemap, ".tilemap");
	autoOutPath(localOptions.autoPalettes, options.palettes, ".pal");
	autoOutPath(localOptions.autoPalmap, options.palmap, ".palmap");

	// Execute deferred external pal spec parsing, now that all other params are known
	if (localOptions.externalPalSpec) {
		parseExternalPalSpec(localOptions.externalPalSpec);
	}

	// LCOV_EXCL_START
	if (options.verbosity >= Options::VERB_CFG) {
		verboseOutputConfig();
	}
	// LCOV_EXCL_STOP

	// Do not do anything if option parsing went wrong.
	requireZeroErrors();

	if (!options.input.empty()) {
		if (localOptions.reverse) {
			reverse();
		} else {
			process();
		}
	} else if (!options.palettes.empty() && options.palSpecType == Options::EXPLICIT
	           && !localOptions.reverse) {
		processPalettes();
	} else {
		usage.printAndExit("No input image specified");
	}

	requireZeroErrors();
	return 0;
}

void Palette::addColor(uint16_t color) {
	for (size_t i = 0; true; ++i) {
		assume(i < colors.size()); // The packing should guarantee this
		if (colors[i] == color) {  // The color is already present
			break;
		} else if (colors[i] == UINT16_MAX) { // Empty slot
			colors[i] = color;
			break;
		}
	}
}

// Returns the ID of the color in the palette, or `size()` if the color is not in
uint8_t Palette::indexOf(uint16_t color) const {
	return color == Rgba::transparent
	           ? 0
	           : std::find(begin(), colors.end(), color) - begin() + options.hasTransparentPixels;
}

auto Palette::begin() -> decltype(colors)::iterator {
	// Skip the first slot if reserved for transparency
	return colors.begin() + options.hasTransparentPixels;
}

auto Palette::end() -> decltype(colors)::iterator {
	// Return an iterator pointing past the last non-empty element.
	// Since the palette may contain gaps, we must scan from the end.
	return std::find_if(
	           colors.rbegin(), colors.rend(), [](uint16_t c) { return c != UINT16_MAX; }
	).base();
}

auto Palette::begin() const -> decltype(colors)::const_iterator {
	// Skip the first slot if reserved for transparency
	return colors.begin() + options.hasTransparentPixels;
}

auto Palette::end() const -> decltype(colors)::const_iterator {
	// Same as the non-const end().
	return std::find_if(
	           colors.rbegin(), colors.rend(), [](uint16_t c) { return c != UINT16_MAX; }
	).base();
}

uint8_t Palette::size() const {
	return end() - colors.begin();
}
