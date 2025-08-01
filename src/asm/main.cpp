// SPDX-License-Identifier: MIT

#include "asm/main.hpp"

#include <algorithm>
#include <errno.h>
#include <limits.h>
#include <memory>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "diagnostics.hpp"
#include "extern/getopt.hpp"
#include "helpers.hpp"
#include "parser.hpp" // Generated from parser.y
#include "usage.hpp"
#include "util.hpp" // UpperMap
#include "version.hpp"

#include "asm/charmap.hpp"
#include "asm/fstack.hpp"
#include "asm/opt.hpp"
#include "asm/output.hpp"
#include "asm/symbol.hpp"
#include "asm/warning.hpp"

Options options;

// Escapes Make-special chars from a string
static std::string make_escape(std::string &str) {
	std::string escaped;
	size_t pos = 0;
	for (;;) {
		// All dollars needs to be doubled
		size_t nextPos = str.find('$', pos);
		if (nextPos == std::string::npos) {
			break;
		}
		escaped.append(str, pos, nextPos - pos);
		escaped.append("$$");
		pos = nextPos + literal_strlen("$");
	}
	escaped.append(str, pos, str.length() - pos);
	return escaped;
}

// Short options
static char const *optstring = "b:D:Eg:hI:M:o:P:p:Q:r:s:VvW:wX:";

// Variables for the long-only options
static int depType; // Variants of `-M`

// Equivalent long options
// Please keep in the same order as short opts.
// Also, make sure long opts don't create ambiguity:
// A long opt's name should start with the same letter as its short opt,
// except if it doesn't create any ambiguity (`verbose` versus `version`).
// This is because long opt matching, even to a single char, is prioritized
// over short opt matching.
static option const longopts[] = {
    {"binary-digits",   required_argument, nullptr,  'b'},
    {"define",          required_argument, nullptr,  'D'},
    {"export-all",      no_argument,       nullptr,  'E'},
    {"gfx-chars",       required_argument, nullptr,  'g'},
    {"help",            no_argument,       nullptr,  'h'},
    {"include",         required_argument, nullptr,  'I'},
    {"dependfile",      required_argument, nullptr,  'M'},
    {"MC",              no_argument,       &depType, 'C'},
    {"MG",              no_argument,       &depType, 'G'},
    {"MP",              no_argument,       &depType, 'P'},
    {"MQ",              required_argument, &depType, 'Q'},
    {"MT",              required_argument, &depType, 'T'},
    {"output",          required_argument, nullptr,  'o'},
    {"preinclude",      required_argument, nullptr,  'P'},
    {"pad-value",       required_argument, nullptr,  'p'},
    {"q-precision",     required_argument, nullptr,  'Q'},
    {"recursion-depth", required_argument, nullptr,  'r'},
    {"state",           required_argument, nullptr,  's'},
    {"version",         no_argument,       nullptr,  'V'},
    {"verbose",         no_argument,       nullptr,  'v'},
    {"warning",         required_argument, nullptr,  'W'},
    {"max-errors",      required_argument, nullptr,  'X'},
    {nullptr,           no_argument,       nullptr,  0  }
};

// clang-format off: long string literal
static Usage usage(
    "Usage: rgbasm [-EhVvw] [-b chars] [-D name[=value]] [-g chars] [-I path]\n"
    "              [-M depend_file] [-MC] [-MG] [-MP] [-MT target_file] [-MQ target_file]\n"
    "              [-o out_file] [-P include_file] [-p pad_value] [-Q precision]\n"
    "              [-r depth] [-s features:state_file] [-W warning] [-X max_errors]\n"
    "              <file>\n"
    "Useful options:\n"
    "    -E, --export-all               export all labels\n"
    "    -M, --dependfile <path>        set the output dependency file\n"
    "    -o, --output <path>            set the output object file\n"
    "    -p, --pad-value <value>        set the value to use for `ds'\n"
    "    -s, --state <features>:<path>  set an output state file\n"
    "    -V, --version                  print RGBASM version and exit\n"
    "    -W, --warning <warning>        enable or disable warnings\n"
    "\n"
    "For help, use `man rgbasm' or go to https://rgbds.gbdev.io/docs/\n"
);
// clang-format on

// Parse a comma-separated string of '-s/--state' features
static std::vector<StateFeature> parseStateFeatures(char *str) {
	std::vector<StateFeature> features;
	for (char *feature = str; feature;) {
		// Split "<feature>,<rest>" so `feature` is "<feature>" and `next` is "<rest>"
		char *next = strchr(feature, ',');
		if (next) {
			*next++ = '\0';
		}
		// Trim whitespace from the beginning of `feature`...
		feature += strspn(feature, " \t");
		// ...and from the end
		if (char *end = strpbrk(feature, " \t"); end) {
			*end = '\0';
		}
		// A feature must be specified
		if (*feature == '\0') {
			fatal("Empty feature for option 's'");
		}
		// Parse the `feature` and update the `features` list
		static UpperMap<StateFeature> const featureNames{
		    {"EQU",   STATE_EQU  },
		    {"VAR",   STATE_VAR  },
		    {"EQUS",  STATE_EQUS },
		    {"CHAR",  STATE_CHAR },
		    {"MACRO", STATE_MACRO},
		};
		if (!strcasecmp(feature, "all")) {
			if (!features.empty()) {
				warnx("Redundant feature before \"%s\" for option 's'", feature);
			}
			features.assign({STATE_EQU, STATE_VAR, STATE_EQUS, STATE_CHAR, STATE_MACRO});
		} else if (auto search = featureNames.find(feature); search == featureNames.end()) {
			fatal("Invalid feature for option 's': \"%s\"", feature);
		} else if (StateFeature value = search->second;
		           std::find(RANGE(features), value) != features.end()) {
			warnx("Ignoring duplicate feature for option 's': \"%s\"", feature);
		} else {
			features.push_back(value);
		}
		feature = next;
	}
	return features;
}

int main(int argc, char *argv[]) {
	// Support SOURCE_DATE_EPOCH for reproducible builds
	// https://reproducible-builds.org/docs/source-date-epoch/
	time_t now = time(nullptr);
	if (char const *sourceDateEpoch = getenv("SOURCE_DATE_EPOCH"); sourceDateEpoch) {
		now = static_cast<time_t>(strtoul(sourceDateEpoch, nullptr, 0));
	}
	sym_Init(now);

	// Maximum of 100 errors only applies if rgbasm is printing errors to a terminal
	if (isatty(STDERR_FILENO)) {
		options.maxErrors = 100;
	}

	// Local options
	char const *dependFileName = nullptr;                                      // -M
	std::unordered_map<std::string, std::vector<StateFeature>> stateFileSpecs; // -s

	for (int ch; (ch = musl_getopt_long_only(argc, argv, optstring, longopts, nullptr)) != -1;) {
		switch (ch) {
			char *endptr;

		case 'b':
			if (strlen(musl_optarg) == 2) {
				opt_B(musl_optarg);
			} else {
				fatal("Must specify exactly 2 characters for option 'b'");
			}
			break;

			char *equals;
		case 'D':
			equals = strchr(musl_optarg, '=');
			if (equals) {
				*equals = '\0';
				sym_AddString(musl_optarg, std::make_shared<std::string>(equals + 1));
			} else {
				sym_AddString(musl_optarg, std::make_shared<std::string>("1"));
			}
			break;

		case 'E':
			sym_SetExportAll(true);
			break;

		case 'g':
			if (strlen(musl_optarg) == 4) {
				opt_G(musl_optarg);
			} else {
				fatal("Must specify exactly 4 characters for option 'g'");
			}
			break;

		case 'h':
			usage.printAndExit(0); // LCOV_EXCL_LINE

		case 'I':
			fstk_AddIncludePath(musl_optarg);
			break;

		case 'M':
			if (options.dependFile) {
				warnx("Overriding dependfile %s", dependFileName);
			}
			if (strcmp("-", musl_optarg)) {
				options.dependFile = fopen(musl_optarg, "w");
				dependFileName = musl_optarg;
			} else {
				options.dependFile = stdout;
				dependFileName = "<stdout>";
			}
			if (options.dependFile == nullptr) {
				// LCOV_EXCL_START
				fatal("Failed to open dependfile \"%s\": %s", dependFileName, strerror(errno));
				// LCOV_EXCL_STOP
			}
			break;

		case 'o':
			if (!options.objectFileName.empty()) {
				warnx("Overriding output filename %s", options.objectFileName.c_str());
			}
			options.objectFileName = musl_optarg;
			verbosePrint("Output filename %s\n", options.objectFileName.c_str()); // LCOV_EXCL_LINE
			break;

		case 'P':
			fstk_AddPreIncludeFile(musl_optarg);
			break;

			unsigned long padByte;
		case 'p':
			padByte = strtoul(musl_optarg, &endptr, 0);

			if (musl_optarg[0] == '\0' || *endptr != '\0') {
				fatal("Invalid argument for option 'p'");
			}

			if (padByte > 0xFF) {
				fatal("Argument for option 'p' must be between 0 and 0xFF");
			}

			opt_P(padByte);
			break;

		case 'Q': {
			char const *precisionArg = musl_optarg;
			if (precisionArg[0] == '.') {
				++precisionArg;
			}
			unsigned long precision = strtoul(precisionArg, &endptr, 0);

			if (musl_optarg[0] == '\0' || *endptr != '\0') {
				fatal("Invalid argument for option 'Q'");
			}

			if (precision < 1 || precision > 31) {
				fatal("Argument for option 'Q' must be between 1 and 31");
			}

			opt_Q(precision);
			break;
		}

		case 'r':
			options.maxRecursionDepth = strtoul(musl_optarg, &endptr, 0);

			if (musl_optarg[0] == '\0' || *endptr != '\0') {
				fatal("Invalid argument for option 'r'");
			}
			break;

		case 's': {
			// Split "<features>:<name>" so `musl_optarg` is "<features>" and `name` is "<name>"
			char *name = strchr(musl_optarg, ':');
			if (!name) {
				fatal("Invalid argument for option 's'");
			}
			*name++ = '\0';

			std::vector<StateFeature> features = parseStateFeatures(musl_optarg);

			if (stateFileSpecs.find(name) != stateFileSpecs.end()) {
				warnx("Overriding state filename %s", name);
			}
			verbosePrint("State filename %s\n", name); // LCOV_EXCL_LINE
			stateFileSpecs.emplace(name, std::move(features));
			break;
		}

		case 'V':
			printf("rgbasm %s\n", get_package_version_string());
			exit(0);

		case 'v':
			// LCOV_EXCL_START
			options.verbose = true;
			break;
			// LCOV_EXCL_STOP

		case 'W':
			opt_W(musl_optarg);
			break;

		case 'w':
			warnings.state.warningsEnabled = false;
			break;

		case 'X': {
			uint64_t maxErrors = strtoul(musl_optarg, &endptr, 0);

			if (musl_optarg[0] == '\0' || *endptr != '\0') {
				fatal("Invalid argument for option 'X'");
			}

			if (maxErrors > UINT64_MAX) {
				fatal("Argument for option 'X' must be between 0 and %" PRIu64, UINT64_MAX);
			}

			options.maxErrors = maxErrors;
			break;
		}

		// Long-only options
		case 0:
			switch (depType) {
			case 'C':
				options.missingIncludeState = GEN_CONTINUE;
				break;

			case 'G':
				options.missingIncludeState = GEN_EXIT;
				break;

			case 'P':
				options.generatePhonyDeps = true;
				break;

			case 'Q':
			case 'T': {
				std::string newTarget = musl_optarg;
				if (depType == 'Q') {
					newTarget = make_escape(newTarget);
				}
				if (!options.targetFileName.empty()) {
					options.targetFileName += ' ';
				}
				options.targetFileName += newTarget;
				break;
			}
			}
			break;

		// Unrecognized options
		default:
			usage.printAndExit(1); // LCOV_EXCL_LINE
		}
	}

	if (options.targetFileName.empty() && !options.objectFileName.empty()) {
		options.targetFileName = options.objectFileName;
	}

	if (argc == musl_optind) {
		usage.printAndExit("Please specify an input file (pass `-` to read from standard input)");
	} else if (argc != musl_optind + 1) {
		usage.printAndExit("More than one input file specified");
	}

	std::string mainFileName = argv[musl_optind];

	verbosePrint("Assembling %s\n", mainFileName.c_str()); // LCOV_EXCL_LINE

	if (options.dependFile && options.targetFileName.empty()) {
		fatal("Dependency files can only be created if a target file is specified with either "
		      "-o, -MQ or -MT");
	}
	options.printDep(mainFileName);

	charmap_New(DEFAULT_CHARMAP_NAME, nullptr);

	// Init lexer and file stack, providing file info
	fstk_Init(mainFileName);

	// Perform parse (`yy::parser` is auto-generated from `parser.y`)
	if (yy::parser parser; parser.parse() != 0) {
		if (warnings.nbErrors == 0) {
			warnings.nbErrors = 1;
		}
	}

	if (!fstk_FailedOnMissingInclude()) {
		sect_CheckUnionClosed();
		sect_CheckLoadClosed();
		sect_CheckSizes();

		charmap_CheckStack();
		opt_CheckStack();
		sect_CheckStack();
	}

	requireZeroErrors();

	// If parse aborted due to missing an include, and `-MG` was given, exit normally
	if (fstk_FailedOnMissingInclude()) {
		return 0;
	}

	out_WriteObject();

	for (auto [name, features] : stateFileSpecs) {
		out_WriteState(name, features);
	}

	return 0;
}
