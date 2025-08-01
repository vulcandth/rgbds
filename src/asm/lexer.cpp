// SPDX-License-Identifier: MIT

#include "asm/lexer.hpp"
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>

#include "helpers.hpp"
#include "util.hpp"

#include "asm/fixpoint.hpp"
#include "asm/format.hpp"
#include "asm/fstack.hpp"
#include "asm/macro.hpp"
#include "asm/main.hpp"
#include "asm/rpn.hpp"
#include "asm/symbol.hpp"
#include "asm/warning.hpp"
// Include this last so it gets all type & constant definitions
#include "parser.hpp" // For token definitions, generated from parser.y

// Bison 3.6 changed token "types" to "kinds"; cast to int for simple compatibility
#define T_(name) static_cast<int>(yy::parser::token::name)

struct Token {
	int type;
	std::variant<std::monostate, uint32_t, std::string> value;

	Token() : type(T_(NUMBER)), value(std::monostate{}) {}
	Token(int type_) : type(type_), value(std::monostate{}) {}
	Token(int type_, uint32_t value_) : type(type_), value(value_) {}
	Token(int type_, std::string const &value_) : type(type_), value(value_) {}
	Token(int type_, std::string &&value_) : type(type_), value(value_) {}
};

// This map lists all RGBASM keywords which `yylex_NORMAL` lexes as identifiers.
// All non-identifier tokens are lexed separately.
static UpperMap<int> const keywordDict{
    {"ADC",           T_(SM83_ADC)         },
    {"ADD",           T_(SM83_ADD)         },
    {"AND",           T_(SM83_AND)         },
    {"BIT",           T_(SM83_BIT)         },
    {"CALL",          T_(SM83_CALL)        },
    {"CCF",           T_(SM83_CCF)         },
    {"CPL",           T_(SM83_CPL)         },
    {"CP",            T_(SM83_CP)          },
    {"DAA",           T_(SM83_DAA)         },
    {"DEC",           T_(SM83_DEC)         },
    {"DI",            T_(SM83_DI)          },
    {"EI",            T_(SM83_EI)          },
    {"HALT",          T_(SM83_HALT)        },
    {"INC",           T_(SM83_INC)         },
    {"JP",            T_(SM83_JP)          },
    {"JR",            T_(SM83_JR)          },
    {"LD",            T_(SM83_LD)          },
    {"LDI",           T_(SM83_LDI)         },
    {"LDD",           T_(SM83_LDD)         },
    {"LDIO",          T_(SM83_LDH)         },
    {"LDH",           T_(SM83_LDH)         },
    {"NOP",           T_(SM83_NOP)         },
    {"OR",            T_(SM83_OR)          },
    {"POP",           T_(SM83_POP)         },
    {"PUSH",          T_(SM83_PUSH)        },
    {"RES",           T_(SM83_RES)         },
    {"RETI",          T_(SM83_RETI)        },
    {"RET",           T_(SM83_RET)         },
    {"RLCA",          T_(SM83_RLCA)        },
    {"RLC",           T_(SM83_RLC)         },
    {"RLA",           T_(SM83_RLA)         },
    {"RL",            T_(SM83_RL)          },
    {"RRC",           T_(SM83_RRC)         },
    {"RRCA",          T_(SM83_RRCA)        },
    {"RRA",           T_(SM83_RRA)         },
    {"RR",            T_(SM83_RR)          },
    {"RST",           T_(SM83_RST)         },
    {"SBC",           T_(SM83_SBC)         },
    {"SCF",           T_(SM83_SCF)         },
    {"SET",           T_(SM83_SET)         },
    {"SLA",           T_(SM83_SLA)         },
    {"SRA",           T_(SM83_SRA)         },
    {"SRL",           T_(SM83_SRL)         },
    {"STOP",          T_(SM83_STOP)        },
    {"SUB",           T_(SM83_SUB)         },
    {"SWAP",          T_(SM83_SWAP)        },
    {"XOR",           T_(SM83_XOR)         },

    {"NZ",            T_(CC_NZ)            },
    {"Z",             T_(CC_Z)             },
    {"NC",            T_(CC_NC)            },
    // There is no `T_(CC_C)`; it's handled before as `T_(TOKEN_C)`

    {"AF",            T_(MODE_AF)          },
    {"BC",            T_(MODE_BC)          },
    {"DE",            T_(MODE_DE)          },
    {"HL",            T_(MODE_HL)          },
    {"SP",            T_(MODE_SP)          },
    {"HLD",           T_(MODE_HL_DEC)      },
    {"HLI",           T_(MODE_HL_INC)      },

    {"A",             T_(TOKEN_A)          },
    {"B",             T_(TOKEN_B)          },
    {"C",             T_(TOKEN_C)          },
    {"D",             T_(TOKEN_D)          },
    {"E",             T_(TOKEN_E)          },
    {"H",             T_(TOKEN_H)          },
    {"L",             T_(TOKEN_L)          },

    {"DEF",           T_(OP_DEF)           },

    {"FRAGMENT",      T_(POP_FRAGMENT)     },
    {"BANK",          T_(OP_BANK)          },
    {"ALIGN",         T_(POP_ALIGN)        },

    {"SIZEOF",        T_(OP_SIZEOF)        },
    {"STARTOF",       T_(OP_STARTOF)       },

    {"ROUND",         T_(OP_ROUND)         },
    {"CEIL",          T_(OP_CEIL)          },
    {"FLOOR",         T_(OP_FLOOR)         },
    {"DIV",           T_(OP_FDIV)          },
    {"MUL",           T_(OP_FMUL)          },
    {"FMOD",          T_(OP_FMOD)          },
    {"POW",           T_(OP_POW)           },
    {"LOG",           T_(OP_LOG)           },
    {"SIN",           T_(OP_SIN)           },
    {"COS",           T_(OP_COS)           },
    {"TAN",           T_(OP_TAN)           },
    {"ASIN",          T_(OP_ASIN)          },
    {"ACOS",          T_(OP_ACOS)          },
    {"ATAN",          T_(OP_ATAN)          },
    {"ATAN2",         T_(OP_ATAN2)         },

    {"HIGH",          T_(OP_HIGH)          },
    {"LOW",           T_(OP_LOW)           },
    {"ISCONST",       T_(OP_ISCONST)       },

    {"BITWIDTH",      T_(OP_BITWIDTH)      },
    {"TZCOUNT",       T_(OP_TZCOUNT)       },

    {"BYTELEN",       T_(OP_BYTELEN)       },
    {"READFILE",      T_(OP_READFILE)      },
    {"STRBYTE",       T_(OP_STRBYTE)       },
    {"STRCAT",        T_(OP_STRCAT)        },
    {"STRCHAR",       T_(OP_STRCHAR)       },
    {"STRCMP",        T_(OP_STRCMP)        },
    {"STRFIND",       T_(OP_STRFIND)       },
    {"STRFMT",        T_(OP_STRFMT)        },
    {"STRIN",         T_(OP_STRIN)         },
    {"STRLEN",        T_(OP_STRLEN)        },
    {"STRLWR",        T_(OP_STRLWR)        },
    {"STRRFIND",      T_(OP_STRRFIND)      },
    {"STRRIN",        T_(OP_STRRIN)        },
    {"STRRPL",        T_(OP_STRRPL)        },
    {"STRSLICE",      T_(OP_STRSLICE)      },
    {"STRSUB",        T_(OP_STRSUB)        },
    {"STRUPR",        T_(OP_STRUPR)        },

    {"CHARCMP",       T_(OP_CHARCMP)       },
    {"CHARLEN",       T_(OP_CHARLEN)       },
    {"CHARSIZE",      T_(OP_CHARSIZE)      },
    {"CHARSUB",       T_(OP_CHARSUB)       },
    {"CHARVAL",       T_(OP_CHARVAL)       },
    {"INCHARMAP",     T_(OP_INCHARMAP)     },
    {"REVCHAR",       T_(OP_REVCHAR)       },

    {"INCLUDE",       T_(POP_INCLUDE)      },
    {"PRINT",         T_(POP_PRINT)        },
    {"PRINTLN",       T_(POP_PRINTLN)      },
    {"EXPORT",        T_(POP_EXPORT)       },
    {"DS",            T_(POP_DS)           },
    {"DB",            T_(POP_DB)           },
    {"DW",            T_(POP_DW)           },
    {"DL",            T_(POP_DL)           },
    {"SECTION",       T_(POP_SECTION)      },
    {"ENDSECTION",    T_(POP_ENDSECTION)   },
    {"PURGE",         T_(POP_PURGE)        },

    {"RSRESET",       T_(POP_RSRESET)      },
    {"RSSET",         T_(POP_RSSET)        },

    {"INCBIN",        T_(POP_INCBIN)       },
    {"CHARMAP",       T_(POP_CHARMAP)      },
    {"NEWCHARMAP",    T_(POP_NEWCHARMAP)   },
    {"SETCHARMAP",    T_(POP_SETCHARMAP)   },
    {"PUSHC",         T_(POP_PUSHC)        },
    {"POPC",          T_(POP_POPC)         },

    {"FAIL",          T_(POP_FAIL)         },
    {"WARN",          T_(POP_WARN)         },
    {"FATAL",         T_(POP_FATAL)        },
    {"ASSERT",        T_(POP_ASSERT)       },
    {"STATIC_ASSERT", T_(POP_STATIC_ASSERT)},

    {"MACRO",         T_(POP_MACRO)        },
    {"ENDM",          T_(POP_ENDM)         },
    {"SHIFT",         T_(POP_SHIFT)        },

    {"REPT",          T_(POP_REPT)         },
    {"FOR",           T_(POP_FOR)          },
    {"ENDR",          T_(POP_ENDR)         },
    {"BREAK",         T_(POP_BREAK)        },

    {"LOAD",          T_(POP_LOAD)         },
    {"ENDL",          T_(POP_ENDL)         },

    {"IF",            T_(POP_IF)           },
    {"ELSE",          T_(POP_ELSE)         },
    {"ELIF",          T_(POP_ELIF)         },
    {"ENDC",          T_(POP_ENDC)         },

    {"UNION",         T_(POP_UNION)        },
    {"NEXTU",         T_(POP_NEXTU)        },
    {"ENDU",          T_(POP_ENDU)         },

    {"WRAM0",         T_(SECT_WRAM0)       },
    {"VRAM",          T_(SECT_VRAM)        },
    {"ROMX",          T_(SECT_ROMX)        },
    {"ROM0",          T_(SECT_ROM0)        },
    {"HRAM",          T_(SECT_HRAM)        },
    {"WRAMX",         T_(SECT_WRAMX)       },
    {"SRAM",          T_(SECT_SRAM)        },
    {"OAM",           T_(SECT_OAM)         },

    {"RB",            T_(POP_RB)           },
    {"RW",            T_(POP_RW)           },
    // There is no `T_(POP_RL)`; it's handled before as `T_(SM83_RL)`

    {"EQU",           T_(POP_EQU)          },
    {"EQUS",          T_(POP_EQUS)         },
    {"REDEF",         T_(POP_REDEF)        },

    {"PUSHS",         T_(POP_PUSHS)        },
    {"POPS",          T_(POP_POPS)         },
    {"PUSHO",         T_(POP_PUSHO)        },
    {"POPO",          T_(POP_POPO)         },

    {"OPT",           T_(POP_OPT)          },
};

static auto ldio = keywordDict.find("LDIO");

static bool isWhitespace(int c) {
	return c == ' ' || c == '\t';
}

static LexerState *lexerState = nullptr;
static LexerState *lexerStateEOL = nullptr;

bool lexer_AtTopLevel() {
	return lexerState == nullptr;
}

void LexerState::clear(uint32_t lineNo_) {
	mode = LEXER_NORMAL;
	atLineStart = true;
	lastToken = T_(YYEOF);
	nextToken = 0;

	ifStack.clear();

	capturing = false;
	captureBuf = nullptr;

	disableMacroArgs = false;
	disableInterpolation = false;
	macroArgScanDistance = 0;
	expandStrings = true;

	expansions.clear();

	lineNo = lineNo_; // Will be incremented at next line start
}

static void nextLine() {
	++lexerState->lineNo;
}

uint32_t lexer_GetIFDepth() {
	return lexerState->ifStack.size();
}

void lexer_IncIFDepth() {
	lexerState->ifStack.push_front({.ranIfBlock = false, .reachedElseBlock = false});
}

void lexer_DecIFDepth() {
	if (lexerState->ifStack.empty()) {
		fatal("Found ENDC outside of an IF construct");
	}

	lexerState->ifStack.pop_front();
}

bool lexer_RanIFBlock() {
	return lexerState->ifStack.front().ranIfBlock;
}

bool lexer_ReachedELSEBlock() {
	return lexerState->ifStack.front().reachedElseBlock;
}

void lexer_RunIFBlock() {
	lexerState->ifStack.front().ranIfBlock = true;
}

void lexer_ReachELSEBlock() {
	lexerState->ifStack.front().reachedElseBlock = true;
}

void LexerState::setAsCurrentState() {
	lexerState = this;
}

void LexerState::setFileAsNextState(std::string const &filePath, bool updateStateNow) {
	if (filePath == "-") {
		path = "<stdin>";
		content.emplace<BufferedContent>(STDIN_FILENO);
		verbosePrint("Opening stdin\n"); // LCOV_EXCL_LINE
	} else {
		struct stat statBuf;
		if (stat(filePath.c_str(), &statBuf) != 0) {
			// LCOV_EXCL_START
			fatal("Failed to stat file \"%s\": %s", filePath.c_str(), strerror(errno));
			// LCOV_EXCL_STOP
		}
		path = filePath;

		if (size_t size = static_cast<size_t>(statBuf.st_size); statBuf.st_size > 0) {
			// Read the entire file for better performance
			// Ideally we'd use C++20 `auto ptr = std::make_shared<char[]>(size)`,
			// but it has insufficient compiler support
			auto ptr = std::shared_ptr<char[]>(new char[size]);

			if (std::ifstream fs(path, std::ios::binary); !fs) {
				// LCOV_EXCL_START
				fatal("Failed to open file \"%s\": %s", path.c_str(), strerror(errno));
				// LCOV_EXCL_STOP
			} else if (!fs.read(ptr.get(), size)) {
				// LCOV_EXCL_START
				fatal("Failed to read file \"%s\": %s", path.c_str(), strerror(errno));
				// LCOV_EXCL_STOP
			}
			content.emplace<ViewedContent>(ptr, size);

			verbosePrint("File \"%s\" is fully read\n", path.c_str()); // LCOV_EXCL_LINE
		} else {
			// LCOV_EXCL_START
			if (statBuf.st_size == 0) {
				verbosePrint("File \"%s\" is empty\n", path.c_str());
			} else {
				verbosePrint("Failed to stat file \"%s\": %s\n", path.c_str(), strerror(errno));
			}
			// LCOV_EXCL_STOP

			// Have a fallback if reading the file failed
			int fd = open(path.c_str(), O_RDONLY);
			if (fd < 0) {
				// LCOV_EXCL_START
				fatal("Failed to open file \"%s\": %s", path.c_str(), strerror(errno));
				// LCOV_EXCL_STOP
			}
			content.emplace<BufferedContent>(fd);

			verbosePrint("File \"%s\" is opened\n", path.c_str()); // LCOV_EXCL_LINE
		}
	}

	clear(0);
	if (updateStateNow) {
		lexerState = this;
	} else {
		lexerStateEOL = this;
	}
}

void LexerState::setViewAsNextState(char const *name, ContentSpan const &span, uint32_t lineNo_) {
	path = name; // Used to report read errors in `.peek()`
	content.emplace<ViewedContent>(span);
	clear(lineNo_);
	lexerStateEOL = this;
}

void lexer_RestartRept(uint32_t lineNo) {
	if (std::holds_alternative<ViewedContent>(lexerState->content)) {
		std::get<ViewedContent>(lexerState->content).offset = 0;
	}
	lexerState->clear(lineNo);
}

LexerState::~LexerState() {
	// A big chunk of the lexer state soundness is the file stack ("fstack").
	// Each context in the fstack has its own *unique* lexer state; thus, we always guarantee
	// that lexer states lifetimes are always properly managed, since they're handled solely
	// by the fstack... with *one* exception.
	// Assume a context is pushed on top of the fstack, and the corresponding lexer state gets
	// scheduled at EOF; `lexerStateEOL` thus becomes a (weak) ref to that lexer state...
	// It has been possible, due to a bug, that the corresponding fstack context gets popped
	// before EOL, deleting the associated state... but it would still be switched to at EOL.
	// This assumption checks that this doesn't happen again.
	// It could be argued that deleting a state that's scheduled for EOF could simply clear
	// `lexerStateEOL`, but there's currently no situation in which this should happen.
	assume(this != lexerStateEOL);
}

bool Expansion::advance() {
	assume(offset <= size());
	return ++offset > size();
}

BufferedContent::~BufferedContent() {
	close(fd);
}

void BufferedContent::advance() {
	assume(offset < std::size(buf));
	if (++offset == std::size(buf)) {
		offset = 0; // Wrap around if necessary
	}
	if (size > 0) {
		--size;
	}
}

void BufferedContent::refill() {
	size_t target = std::size(buf) - size; // Aim: making the buf full

	// Compute the index we'll start writing to
	size_t startIndex = (offset + size) % std::size(buf);

	// If the range to fill passes over the buffer wrapping point, we need two reads
	if (startIndex + target > std::size(buf)) {
		size_t nbExpectedChars = std::size(buf) - startIndex;
		size_t nbReadChars = readMore(startIndex, nbExpectedChars);

		startIndex += nbReadChars;
		if (startIndex == std::size(buf)) {
			startIndex = 0;
		}

		// If the read was incomplete, don't perform a second read
		target -= nbReadChars;
		if (nbReadChars < nbExpectedChars) {
			target = 0;
		}
	}
	if (target != 0) {
		readMore(startIndex, target);
	}
}

size_t BufferedContent::readMore(size_t startIndex, size_t nbChars) {
	// This buffer overflow made me lose WEEKS of my life. Never again.
	assume(startIndex + nbChars <= std::size(buf));
	ssize_t nbReadChars = read(fd, &buf[startIndex], nbChars);

	if (nbReadChars == -1) {
		// LCOV_EXCL_START
		fatal("Error while reading \"%s\": %s", lexerState->path.c_str(), strerror(errno));
		// LCOV_EXCL_STOP
	}

	size += nbReadChars;

	// `nbReadChars` cannot be negative, so it's fine to cast to `size_t`
	return static_cast<size_t>(nbReadChars);
}

void lexer_SetMode(LexerMode mode) {
	lexerState->mode = mode;
}

void lexer_ToggleStringExpansion(bool enable) {
	lexerState->expandStrings = enable;
}

// Functions for the actual lexer to obtain characters

static void beginExpansion(std::shared_ptr<std::string> str, std::optional<std::string> name) {
	if (name) {
		lexer_CheckRecursionDepth();
	}

	// Do not expand empty strings
	if (str->empty()) {
		return;
	}

	lexerState->expansions.push_front({.name = name, .contents = str, .offset = 0});
}

void lexer_CheckRecursionDepth() {
	if (lexerState->expansions.size() > options.maxRecursionDepth + 1) {
		fatal("Recursion limit (%zu) exceeded", options.maxRecursionDepth);
	}
}

static bool isMacroChar(char c) {
	return c == '@' || c == '#' || c == '<' || (c >= '1' && c <= '9');
}

// Forward declarations for `readBracketedMacroArgNum`
static int peek();
static void shiftChar();
static int bumpChar();
static int nextChar();
static uint32_t readDecimalNumber(int initial);

static uint32_t readBracketedMacroArgNum() {
	bool disableMacroArgs = lexerState->disableMacroArgs;
	bool disableInterpolation = lexerState->disableInterpolation;
	lexerState->disableMacroArgs = false;
	lexerState->disableInterpolation = false;
	Defer restoreExpansions{[&] {
		lexerState->disableMacroArgs = disableMacroArgs;
		lexerState->disableInterpolation = disableInterpolation;
	}};

	int32_t num = 0;
	int c = peek();
	bool empty = false;
	bool symbolError = false;
	bool negative = c == '-';

	if (negative) {
		c = nextChar();
	}

	if (c >= '0' && c <= '9') {
		uint32_t n = readDecimalNumber(bumpChar());
		if (n > INT32_MAX) {
			error("Number in bracketed macro argument is too large");
			return 0;
		}
		num = negative ? -n : static_cast<int32_t>(n);
	} else if (startsIdentifier(c) || c == '#') {
		if (c == '#') {
			c = nextChar();
			if (!startsIdentifier(c)) {
				error("Empty raw symbol in bracketed macro argument");
				return 0;
			}
		}

		std::string symName;
		for (; continuesIdentifier(c); c = nextChar()) {
			symName += c;
		}

		if (Symbol const *sym = sym_FindScopedValidSymbol(symName); !sym) {
			if (sym_IsPurgedScoped(symName)) {
				error("Bracketed symbol \"%s\" does not exist; it was purged", symName.c_str());
			} else {
				error("Bracketed symbol \"%s\" does not exist", symName.c_str());
			}
			num = 0;
			symbolError = true;
		} else if (!sym->isNumeric()) {
			error("Bracketed symbol \"%s\" is not numeric", symName.c_str());
			num = 0;
			symbolError = true;
		} else {
			num = static_cast<int32_t>(sym->getConstantValue());
		}
	} else {
		empty = true;
	}

	c = bumpChar();
	if (c != '>') {
		error("Invalid character in bracketed macro argument %s", printChar(c));
		return 0;
	} else if (empty) {
		error("Empty bracketed macro argument");
		return 0;
	} else if (num == 0 && !symbolError) {
		error("Invalid bracketed macro argument '\\<0>'");
		return 0;
	} else {
		return num;
	}
}

static std::shared_ptr<std::string> readMacroArg() {
	if (int c = bumpChar(); c == '@') {
		std::shared_ptr<std::string> str = fstk_GetUniqueIDStr();
		if (!str) {
			error("'\\@' cannot be used outside of a macro or REPT/FOR block");
		}
		return str;
	} else if (c == '#') {
		MacroArgs *macroArgs = fstk_GetCurrentMacroArgs();
		if (!macroArgs) {
			error("'\\#' cannot be used outside of a macro");
			return nullptr;
		}

		std::shared_ptr<std::string> str = macroArgs->getAllArgs();
		assume(str); // '\#' should always be defined (at least as an empty string)
		return str;
	} else if (c == '<') {
		int32_t num = readBracketedMacroArgNum();
		if (num == 0) {
			// The error was already reported by `readBracketedMacroArgNum`.
			return nullptr;
		}

		MacroArgs *macroArgs = fstk_GetCurrentMacroArgs();
		if (!macroArgs) {
			error("'\\<%" PRIu32 ">' cannot be used outside of a macro", num);
			return nullptr;
		}

		std::shared_ptr<std::string> str = macroArgs->getArg(num);
		if (!str) {
			error("Macro argument '\\<%" PRId32 ">' not defined", num);
		}
		return str;
	} else {
		assume(c >= '1' && c <= '9');

		MacroArgs *macroArgs = fstk_GetCurrentMacroArgs();
		if (!macroArgs) {
			error("'\\%c' cannot be used outside of a macro", c);
			return nullptr;
		}

		std::shared_ptr<std::string> str = macroArgs->getArg(c - '0');
		if (!str) {
			error("Macro argument '\\%c' not defined", c);
		}
		return str;
	}
}

int LexerState::peekChar() {
	// This is `.peekCharAhead()` modified for zero lookahead distance
	for (Expansion &exp : expansions) {
		if (exp.offset < exp.size()) {
			return static_cast<uint8_t>((*exp.contents)[exp.offset]);
		}
	}

	if (std::holds_alternative<ViewedContent>(content)) {
		auto &view = std::get<ViewedContent>(content);
		if (view.offset < view.span.size) {
			return static_cast<uint8_t>(view.span.ptr[view.offset]);
		}
	} else {
		auto &cbuf = std::get<BufferedContent>(content);
		if (cbuf.size == 0) {
			cbuf.refill();
		}
		assume(cbuf.offset < std::size(cbuf.buf));
		if (cbuf.size > 0) {
			return static_cast<uint8_t>(cbuf.buf[cbuf.offset]);
		}
	}

	// If there aren't enough chars, give up
	return EOF;
}

int LexerState::peekCharAhead() {
	// We only need one character of lookahead, for macro arguments
	uint8_t distance = 1;

	for (Expansion &exp : expansions) {
		// An expansion that has reached its end will have `exp.offset` == `exp.size()`,
		// and `.peekCharAhead()` will continue with its parent
		assume(exp.offset <= exp.size());
		if (size_t idx = exp.offset + distance; idx < exp.size()) {
			// Macro args can't be recursive, since `peek()` marks them as scanned, so
			// this is a failsafe that (as far as I can tell) won't ever actually run.
			return static_cast<uint8_t>((*exp.contents)[idx]); // LCOV_EXCL_LINE
		}
		distance -= exp.size() - exp.offset;
	}

	if (std::holds_alternative<ViewedContent>(content)) {
		auto &view = std::get<ViewedContent>(content);
		if (view.offset + distance < view.span.size) {
			return static_cast<uint8_t>(view.span.ptr[view.offset + distance]);
		}
	} else {
		auto &cbuf = std::get<BufferedContent>(content);
		assume(distance < std::size(cbuf.buf));
		if (cbuf.size <= distance) {
			cbuf.refill();
		}
		if (cbuf.size > distance) {
			return static_cast<uint8_t>(cbuf.buf[(cbuf.offset + distance) % std::size(cbuf.buf)]);
		}
	}

	// If there aren't enough chars, give up
	return EOF;
}

// Forward declarations for `peek`
static std::shared_ptr<std::string> readInterpolation(size_t depth);

static int peek() {
	int c = lexerState->peekChar();

	if (lexerState->macroArgScanDistance > 0) {
		return c;
	}

	++lexerState->macroArgScanDistance; // Do not consider again

	if (c == '\\' && !lexerState->disableMacroArgs) {
		// If character is a backslash, check for a macro arg
		++lexerState->macroArgScanDistance;
		if (!isMacroChar(lexerState->peekCharAhead())) {
			return c;
		}

		// If character is a macro arg char, do macro arg expansion
		shiftChar();
		if (std::shared_ptr<std::string> str = readMacroArg(); str) {
			beginExpansion(str, std::nullopt);

			// Mark the entire macro arg expansion as "painted blue"
			// so that macro args can't be recursive
			// https://en.wikipedia.org/wiki/Painted_blue
			lexerState->macroArgScanDistance += str->length();
		}

		return peek(); // Tail recursion
	} else if (c == '{' && !lexerState->disableInterpolation) {
		// If character is an open brace, do symbol interpolation
		shiftChar();
		if (std::shared_ptr<std::string> str = readInterpolation(0); str) {
			beginExpansion(str, *str);
		}

		return peek(); // Tail recursion
	} else {
		return c;
	}
}

static void shiftChar() {
	if (lexerState->capturing) {
		if (lexerState->captureBuf) {
			lexerState->captureBuf->push_back(peek());
		}
		++lexerState->captureSize;
	}

	--lexerState->macroArgScanDistance;

	for (;;) {
		if (!lexerState->expansions.empty()) {
			// Advance within the current expansion
			if (Expansion &exp = lexerState->expansions.front(); exp.advance()) {
				// When advancing would go past an expansion's end,
				// move up to its parent and try again to advance
				lexerState->expansions.pop_front();
				continue;
			}
		} else {
			// Advance within the file contents
			if (std::holds_alternative<ViewedContent>(lexerState->content)) {
				++std::get<ViewedContent>(lexerState->content).offset;
			} else {
				std::get<BufferedContent>(lexerState->content).advance();
			}
		}
		return;
	}
}

static int bumpChar() {
	int c = peek();
	if (c != EOF) {
		shiftChar();
	}
	return c;
}

static int nextChar() {
	shiftChar();
	return peek();
}

template<typename P>
static int skipChars(P predicate) {
	int c = peek();
	while (predicate(c)) {
		c = nextChar();
	}
	return c;
}

static void handleCRLF(int c) {
	if (c == '\r' && peek() == '\n') {
		shiftChar();
	}
}

static auto scopedDisableExpansions() {
	lexerState->disableMacroArgs = true;
	lexerState->disableInterpolation = true;
	return Defer{[&] {
		lexerState->disableMacroArgs = false;
		lexerState->disableInterpolation = false;
	}};
}

// "Services" provided by the lexer to the rest of the program

uint32_t lexer_GetLineNo() {
	return lexerState->lineNo;
}

void lexer_DumpStringExpansions() {
	if (!lexerState) {
		return;
	}

	for (Expansion &exp : lexerState->expansions) {
		// Only register EQUS expansions, not string args
		if (exp.name) {
			fprintf(stderr, "while expanding symbol \"%s\"\n", exp.name->c_str());
		}
	}
}

// Functions to discard non-tokenized characters

static void discardBlockComment() {
	Defer reenableExpansions = scopedDisableExpansions();
	for (;;) {
		int c = bumpChar();

		switch (c) {
		case EOF:
			error("Unterminated block comment");
			return;
		case '\r':
			handleCRLF(c);
			[[fallthrough]];
		case '\n':
			if (lexerState->expansions.empty()) {
				nextLine();
			}
			continue;
		case '/':
			if (peek() == '*') {
				warning(WARNING_NESTED_COMMENT, "/* in block comment");
			}
			continue;
		case '*':
			if (peek() == '/') {
				shiftChar();
				return;
			}
			[[fallthrough]];
		default:
			continue;
		}
	}
}

static void discardComment() {
	Defer reenableExpansions = scopedDisableExpansions();
	for (;; shiftChar()) {
		if (int c = peek(); c == EOF || c == '\r' || c == '\n') {
			break;
		}
	}
}

static void discardLineContinuation() {
	for (;;) {
		if (int c = peek(); isWhitespace(c)) {
			shiftChar();
		} else if (c == '\r' || c == '\n') {
			shiftChar();
			handleCRLF(c);
			if (lexerState->expansions.empty()) {
				nextLine();
			}
			break;
		} else if (c == ';') {
			discardComment();
		} else if (c == EOF) {
			error("Invalid line continuation at end of file");
			break;
		} else {
			error("Invalid character after line continuation %s", printChar(c));
			break;
		}
	}
}

// Functions to read tokenizable values

static std::string readAnonLabelRef(char c) {
	// We come here having already peeked at one char, so no need to do it again
	uint32_t n = 1;
	while (nextChar() == c) {
		++n;
	}
	return sym_MakeAnonLabelName(n, c == '-');
}

static uint32_t readFractionalPart(uint32_t integer) {
	uint32_t value = 0, divisor = 1;
	uint8_t precision = 0;
	enum {
		READFRACTIONALPART_DIGITS,
		READFRACTIONALPART_PRECISION,
		READFRACTIONALPART_PRECISION_DIGITS,
	} state = READFRACTIONALPART_DIGITS;

	for (int c = peek();; c = nextChar()) {
		if (state == READFRACTIONALPART_DIGITS) {
			if (c == '_') {
				continue;
			} else if (c == 'q' || c == 'Q') {
				state = READFRACTIONALPART_PRECISION;
				continue;
			} else if (c < '0' || c > '9') {
				break;
			}
			if (divisor > (UINT32_MAX - (c - '0')) / 10) {
				warning(WARNING_LARGE_CONSTANT, "Precision of fixed-point constant is too large");
				// Discard any additional digits
				skipChars([](int d) { return (d >= '0' && d <= '9') || d == '_'; });
				break;
			}
			value = value * 10 + (c - '0');
			divisor *= 10;
		} else {
			if (c == '.' && state == READFRACTIONALPART_PRECISION) {
				state = READFRACTIONALPART_PRECISION_DIGITS;
				continue;
			} else if (c < '0' || c > '9') {
				break;
			}
			precision = precision * 10 + (c - '0');
		}
	}

	if (precision == 0) {
		if (state >= READFRACTIONALPART_PRECISION) {
			error("Invalid fixed-point constant, no significant digits after 'q'");
		}
		precision = options.fixPrecision;
	} else if (precision > 31) {
		error("Fixed-point constant precision must be between 1 and 31");
		precision = options.fixPrecision;
	}

	if (integer >= (1ULL << (32 - precision))) {
		warning(WARNING_LARGE_CONSTANT, "Magnitude of fixed-point constant is too large");
	}

	// Cast to unsigned avoids undefined overflow behavior
	uint32_t fractional =
	    static_cast<uint32_t>(round(static_cast<double>(value) / divisor * pow(2.0, precision)));

	return (integer << precision) | fractional;
}

static bool isValidDigit(char c) {
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.'
	       || c == '#' || c == '@';
}

static bool checkDigitErrors(char const *digits, size_t n, char const *type) {
	for (size_t i = 0; i < n; ++i) {
		char c = digits[i];

		if (!isValidDigit(c)) {
			error("Invalid digit for %s constant %s", type, printChar(c));
			return false;
		}

		if (c >= '0' && c < static_cast<char>(n + '0') && c != static_cast<char>(i + '0')) {
			error("Changed digit for %s constant %s", type, printChar(c));
			return false;
		}

		for (size_t j = i + 1; j < n; ++j) {
			if (c == digits[j]) {
				error("Repeated digit for %s constant %s", type, printChar(c));
				return false;
			}
		}
	}

	return true;
}

void lexer_SetBinDigits(char const digits[2]) {
	if (size_t n = std::size(options.binDigits); checkDigitErrors(digits, n, "binary")) {
		memcpy(options.binDigits, digits, n);
	}
}

void lexer_SetGfxDigits(char const digits[4]) {
	if (size_t n = std::size(options.gfxDigits); checkDigitErrors(digits, n, "graphics")) {
		memcpy(options.gfxDigits, digits, n);
	}
}

static uint32_t readBinaryNumber() {
	uint32_t value = 0;
	bool empty = true;

	for (int c = peek();; c = nextChar()) {
		int bit;

		if (c == '_' && !empty) {
			continue;
		} else if (c == '0' || c == options.binDigits[0]) {
			bit = 0;
		} else if (c == '1' || c == options.binDigits[1]) {
			bit = 1;
		} else {
			break;
		}
		if (value > (UINT32_MAX - bit) / 2) {
			warning(WARNING_LARGE_CONSTANT, "Integer constant is too large");
		}
		value = value * 2 + bit;

		empty = false;
	}

	if (empty) {
		error("Invalid integer constant, no digits after '%%'");
	}

	return value;
}

static uint32_t readOctalNumber() {
	uint32_t value = 0;
	bool empty = true;

	for (int c = peek();; c = nextChar()) {
		if (c == '_' && !empty) {
			continue;
		} else if (c >= '0' && c <= '7') {
			c = c - '0';
		} else {
			break;
		}

		if (value > (UINT32_MAX - c) / 8) {
			warning(WARNING_LARGE_CONSTANT, "Integer constant is too large");
		}
		value = value * 8 + c;

		empty = false;
	}

	if (empty) {
		error("Invalid integer constant, no digits after '&'");
	}

	return value;
}

static uint32_t readDecimalNumber(int initial) {
	assume(initial >= '0' && initial <= '9');
	uint32_t value = initial - '0';

	for (int c = peek();; c = nextChar()) {
		if (c == '_') {
			continue;
		} else if (c >= '0' && c <= '9') {
			c = c - '0';
		} else {
			break;
		}

		if (value > (UINT32_MAX - c) / 10) {
			warning(WARNING_LARGE_CONSTANT, "Integer constant is too large");
		}
		value = value * 10 + c;
	}

	return value;
}

static uint32_t readHexNumber() {
	uint32_t value = 0;
	bool empty = true;

	for (int c = peek();; c = nextChar()) {
		if (c == '_' && !empty) {
			continue;
		} else if (c >= 'a' && c <= 'f') {
			c = c - 'a' + 10;
		} else if (c >= 'A' && c <= 'F') {
			c = c - 'A' + 10;
		} else if (c >= '0' && c <= '9') {
			c = c - '0';
		} else {
			break;
		}

		if (value > (UINT32_MAX - c) / 16) {
			warning(WARNING_LARGE_CONSTANT, "Integer constant is too large");
		}
		value = value * 16 + c;

		empty = false;
	}

	if (empty) {
		error("Invalid integer constant, no digits after '$'");
	}

	return value;
}

static uint32_t readGfxConstant() {
	uint32_t bitPlaneLower = 0, bitPlaneUpper = 0;
	uint8_t width = 0;

	for (int c = peek();; c = nextChar()) {
		uint32_t pixel;

		if (c == '_' && width > 0) {
			continue;
		} else if (c == '0' || c == options.gfxDigits[0]) {
			pixel = 0;
		} else if (c == '1' || c == options.gfxDigits[1]) {
			pixel = 1;
		} else if (c == '2' || c == options.gfxDigits[2]) {
			pixel = 2;
		} else if (c == '3' || c == options.gfxDigits[3]) {
			pixel = 3;
		} else {
			break;
		}

		if (width < 8) {
			bitPlaneLower = bitPlaneLower << 1 | (pixel & 1);
			bitPlaneUpper = bitPlaneUpper << 1 | (pixel >> 1);
		}
		if (width < 9) {
			++width;
		}
	}

	if (width == 0) {
		error("Invalid graphics constant, no digits after '`'");
	} else if (width == 9) {
		warning(
		    WARNING_LARGE_CONSTANT, "Graphics constant is too long, only first 8 pixels considered"
		);
	}

	return bitPlaneUpper << 8 | bitPlaneLower;
}

// Functions to read identifiers and keywords

static Token readIdentifier(char firstChar, bool raw) {
	std::string identifier(1, firstChar);
	int tokenType = firstChar == '.' ? T_(LOCAL) : T_(SYMBOL);

	// Continue reading while the char is in the identifier charset
	for (int c = peek(); continuesIdentifier(c); c = nextChar()) {
		// Write the char to the identifier's name
		identifier += c;

		// If the char was a dot, the identifier is a local label
		if (c == '.') {
			tokenType = T_(LOCAL);
		}
	}

	// Attempt to check for a keyword if the identifier is not raw
	if (!raw) {
		if (auto search = keywordDict.find(identifier); search != keywordDict.end()) {
			if (search == ldio) {
				warning(WARNING_OBSOLETE, "LDIO is deprecated; use LDH");
			}
			return Token(search->second);
		}
	}

	// Label scopes `.` and `..` are the only nonlocal identifiers that start with a dot
	if (identifier.find_first_not_of('.') == identifier.npos) {
		tokenType = T_(SYMBOL);
	}

	return Token(tokenType, identifier);
}

// Functions to read strings

static std::shared_ptr<std::string> readInterpolation(size_t depth) {
	if (depth > options.maxRecursionDepth) {
		fatal("Recursion limit (%zu) exceeded", options.maxRecursionDepth);
	}

	std::string fmtBuf;
	FormatSpec fmt{};

	// In a context where `lexerState->disableInterpolation` is true, `peek` will expand
	// nested interpolations itself, which can lead to stack overflow. This lets
	// `readInterpolation` handle its own nested expansions, increasing `depth` each time.
	bool disableInterpolation = lexerState->disableInterpolation;
	lexerState->disableInterpolation = true;

	// Reset `lexerState->disableInterpolation` when exiting this loop
	for (Defer reset{[&] { lexerState->disableInterpolation = disableInterpolation; }};;) {
		if (int c = peek(); c == '{') { // Nested interpolation
			shiftChar();
			if (std::shared_ptr<std::string> str = readInterpolation(depth + 1); str) {
				beginExpansion(str, *str);
			}
			continue; // Restart, reading from the new buffer
		} else if (c == EOF || c == '\r' || c == '\n' || c == '"') {
			error("Missing }");
			break;
		} else if (c == '}') {
			shiftChar();
			break;
		} else if (c == ':' && !fmt.isFinished()) { // Format spec, only once
			shiftChar();
			for (char f : fmtBuf) {
				fmt.useCharacter(f);
			}
			fmt.finishCharacters();
			if (!fmt.isValid()) {
				error("Invalid format spec '%s'", fmtBuf.c_str());
			}
			fmtBuf.clear(); // Now that format has been set, restart at beginning of string
		} else {
			shiftChar();
			fmtBuf += c;
		}
	}

	if (fmtBuf.starts_with('#')) {
		// Skip a '#' raw symbol prefix, but after expanding any nested interpolations.
		fmtBuf.erase(0, 1);
	} else if (keywordDict.find(fmtBuf) != keywordDict.end()) {
		// Don't allow symbols that alias keywords without a '#' prefix.
		error(
		    "Interpolated symbol \"%s\" is a reserved keyword; add a '#' prefix to use it as a raw "
		    "symbol",
		    fmtBuf.c_str()
		);
		return nullptr;
	}

	if (Symbol const *sym = sym_FindScopedValidSymbol(fmtBuf); !sym || !sym->isDefined()) {
		if (sym_IsPurgedScoped(fmtBuf)) {
			error("Interpolated symbol \"%s\" does not exist; it was purged", fmtBuf.c_str());
		} else {
			error("Interpolated symbol \"%s\" does not exist", fmtBuf.c_str());
		}
	} else if (sym->type == SYM_EQUS) {
		auto buf = std::make_shared<std::string>();
		fmt.appendString(*buf, *sym->getEqus());
		return buf;
	} else if (sym->isNumeric()) {
		auto buf = std::make_shared<std::string>();
		fmt.appendNumber(*buf, sym->getConstantValue());
		return buf;
	} else {
		error("Interpolated symbol \"%s\" is not a numeric or string symbol", fmtBuf.c_str());
	}
	return nullptr;
}

static void appendExpandedString(std::string &str, std::string const &expanded) {
	if (lexerState->mode != LEXER_RAW) {
		str.append(expanded);
		return;
	}

	for (char c : expanded) {
		// Escape characters that need escaping
		switch (c) {
		case '\n':
			str += "\\n";
			break;
			// LCOV_EXCL_START
		case '\r':
			// A literal CR in a string may get treated as a LF, so '\r' is not tested.
			str += "\\r";
			break;
			// LCOV_EXCL_STOP
		case '\t':
			str += "\\t";
			break;
		case '\0':
			str += "\\0";
			break;
		case '\\':
		case '"':
		case '\'':
		case '{':
			str += '\\';
			[[fallthrough]];
		default:
			str += c;
			break;
		}
	}
}

static void appendCharInLiteral(std::string &str, int c) {
	bool rawMode = lexerState->mode == LEXER_RAW;

	// Symbol interpolation
	if (c == '{') {
		// We'll be exiting the string/character scope, so re-enable expansions
		// (Not interpolations, since they're handled by the function itself...)
		lexerState->disableMacroArgs = false;
		if (std::shared_ptr<std::string> interpolation = readInterpolation(0); interpolation) {
			appendExpandedString(str, *interpolation);
		}
		lexerState->disableMacroArgs = true;
		return;
	}

	// Regular characters will just get copied
	if (c != '\\') {
		str += c;
		return;
	}

	c = peek();
	switch (c) {
	// Character escape
	case '\\':
	case '"':
	case '\'':
	case '{':
	case '}':
		if (rawMode) {
			str += '\\';
		}
		str += c;
		shiftChar();
		break;
	case 'n':
		str += rawMode ? "\\n" : "\n";
		shiftChar();
		break;
	case 'r':
		str += rawMode ? "\\r" : "\r";
		shiftChar();
		break;
	case 't':
		str += rawMode ? "\\t" : "\t";
		shiftChar();
		break;
	case '0':
		if (rawMode) {
			str += "\\0";
		} else {
			str += '\0';
		}
		shiftChar();
		break;

	// Line continuation
	case ' ':
	case '\t':
	case '\r':
	case '\n':
		discardLineContinuation();
		break;

	// Macro arg
	case '@':
	case '#':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	case '<':
		if (std::shared_ptr<std::string> arg = readMacroArg(); arg) {
			appendExpandedString(str, *arg);
		}
		break;

	case EOF: // Can't really print that one
		error("Illegal character escape at end of input");
		str += '\\';
		break;

	default:
		error("Illegal character escape %s", printChar(c));
		str += c;
		shiftChar();
		break;
	}
}

static void readString(std::string &str, bool rawString) {
	Defer reenableExpansions = scopedDisableExpansions();

	bool rawMode = lexerState->mode == LEXER_RAW;

	// We reach this function after reading a single quote, but we also support triple quotes
	bool multiline = false;
	if (rawMode) {
		str += '"';
	}
	if (peek() == '"') {
		if (rawMode) {
			str += '"';
		}
		if (nextChar() != '"') {
			// "" is an empty string, skip the loop
			return;
		}
		// """ begins a multi-line string
		shiftChar();
		if (rawMode) {
			str += '"';
		}
		multiline = true;
	}

	for (;;) {
		int c = peek();

		// '\r', '\n' or EOF ends a single-line string early
		if (c == EOF || (!multiline && (c == '\r' || c == '\n'))) {
			error("Unterminated string");
			return;
		}

		// We'll be staying in the string, so we can safely consume the char
		shiftChar();

		// Handle '\r' or '\n' (in multiline strings only, already handled above otherwise)
		if (c == '\r' || c == '\n') {
			handleCRLF(c);
			nextLine();
			str += '\n';
			continue;
		}

		if (c != '"') {
			// Append the character or handle special ones
			if (rawString) {
				str += c;
			} else {
				appendCharInLiteral(str, c);
			}
			continue;
		}

		// Close the string and return if it's terminated
		if (!multiline) {
			if (rawMode) {
				str += c;
			}
			return;
		}
		// Only """ ends a multi-line string
		if (peek() != '"') {
			str += c;
			continue;
		}
		if (nextChar() != '"') {
			str += "\"\"";
			continue;
		}
		shiftChar();
		if (rawMode) {
			str += "\"\"\"";
		}
		return;
	}
}

static void readCharacter(std::string &str) {
	// This is essentially a simplified `readString`
	Defer reenableExpansions = scopedDisableExpansions();

	bool rawMode = lexerState->mode == LEXER_RAW;

	// We reach this function after reading a single quote
	if (rawMode) {
		str += '\'';
	}

	for (;;) {
		switch (int c = peek(); c) {
		case '\r':
		case '\n':
		case EOF:
			// '\r', '\n' or EOF ends a character early
			error("Unterminated character");
			return;
		case '\'':
			// Close the character and return if it's terminated
			shiftChar();
			if (rawMode) {
				str += c;
			}
			return;
		default:
			// Append the character or handle special ones
			shiftChar();
			appendCharInLiteral(str, c);
		}
	}
}

// Lexer core

static Token yylex_SKIP_TO_ENDC(); // Forward declaration for `yylex_NORMAL`

// Must stay in sync with the `switch` in `yylex_NORMAL`!
static bool isGarbageCharacter(int c) {
	return c != EOF && !continuesIdentifier(c)
	       && (c == '\0' || !strchr("; \t~[](),+-*/|^=!<>:&%`\"\r\n\\", c));
}

static void reportGarbageCharacters(int c) {
	// '#' can be garbage if it doesn't start a raw string or identifier
	assume(isGarbageCharacter(c) || c == '#');
	if (isGarbageCharacter(peek())) {
		// At least two characters are garbage; group them into one error report
		std::string garbage = printChar(c);
		while (isGarbageCharacter(peek())) {
			c = bumpChar();
			garbage += ", ";
			garbage += printChar(c);
		}
		error("Unknown characters %s", garbage.c_str());
	} else {
		error("Unknown character %s", printChar(c));
	}
}

static Token yylex_NORMAL() {
	if (int nextToken = lexerState->nextToken; nextToken) {
		lexerState->nextToken = 0;
		return Token(nextToken);
	}

	for (;; lexerState->atLineStart = false) {
		int c = bumpChar();

		switch (c) {
			// Ignore whitespace and comments

		case ';':
			discardComment();
			[[fallthrough]];
		case ' ':
		case '\t':
			continue;

			// Handle unambiguous single-char tokens

		case '~':
			return Token(T_(OP_NOT));

		case '@': {
			std::string symName("@");
			return Token(T_(SYMBOL), symName);
		}

		case '(':
			return Token(T_(LPAREN));
		case ')':
			return Token(T_(RPAREN));
		case ',':
			return Token(T_(COMMA));

			// Handle ambiguous 1- or 2-char tokens

		case '[': // Either [ or [[
			if (peek() == '[') {
				shiftChar();
				return Token(T_(LBRACKS));
			}
			return Token(T_(LBRACK));

		case ']': // Either ] or ]]
			if (peek() == ']') {
				shiftChar();
				// `[[ Fragment literals ]]` inject an EOL token to end their contents
				// even without a newline. Retroactively lex the `]]` after it.
				lexerState->nextToken = T_(RBRACKS);
				return Token(T_(EOL));
			}
			return Token(T_(RBRACK));

		case '+': // Either +=, ADD, or CAT
			switch (peek()) {
			case '=':
				shiftChar();
				return Token(T_(POP_ADDEQ));
			case '+':
				shiftChar();
				return Token(T_(OP_CAT));
			default:
				return Token(T_(OP_ADD));
			}

		case '-': // Either -= or SUB
			if (peek() == '=') {
				shiftChar();
				return Token(T_(POP_SUBEQ));
			}
			return Token(T_(OP_SUB));

		case '*': // Either *=, MUL, or EXP
			switch (peek()) {
			case '=':
				shiftChar();
				return Token(T_(POP_MULEQ));
			case '*':
				shiftChar();
				return Token(T_(OP_EXP));
			default:
				return Token(T_(OP_MUL));
			}

		case '/': // Either /=, DIV, or a block comment
			switch (peek()) {
			case '=':
				shiftChar();
				return Token(T_(POP_DIVEQ));
			case '*':
				shiftChar();
				discardBlockComment();
				continue;
			default:
				return Token(T_(OP_DIV));
			}

		case '|': // Either |=, binary OR, or logical OR
			switch (peek()) {
			case '=':
				shiftChar();
				return Token(T_(POP_OREQ));
			case '|':
				shiftChar();
				return Token(T_(OP_LOGICOR));
			default:
				return Token(T_(OP_OR));
			}

		case '^': // Either ^= or XOR
			if (peek() == '=') {
				shiftChar();
				return Token(T_(POP_XOREQ));
			}
			return Token(T_(OP_XOR));

		case '=': // Either assignment or EQ
			if (peek() == '=') {
				shiftChar();
				return Token(T_(OP_LOGICEQU));
			}
			return Token(T_(POP_EQUAL));

		case '!': // Either a NEQ or negation
			if (peek() == '=') {
				shiftChar();
				return Token(T_(OP_LOGICNE));
			}
			return Token(T_(OP_LOGICNOT));

			// Handle ambiguous 1-, 2-, or 3-char tokens

		case '<': // Either <<=, LT, LTE, or left shift
			switch (peek()) {
			case '=':
				shiftChar();
				return Token(T_(OP_LOGICLE));
			case '<':
				shiftChar();
				if (peek() == '=') {
					shiftChar();
					return Token(T_(POP_SHLEQ));
				}
				return Token(T_(OP_SHL));
			default:
				return Token(T_(OP_LOGICLT));
			}

		case '>': // Either >>=, GT, GTE, or either kind of right shift
			switch (peek()) {
			case '=':
				shiftChar();
				return Token(T_(OP_LOGICGE));
			case '>':
				shiftChar();
				switch (peek()) {
				case '=':
					shiftChar();
					return Token(T_(POP_SHREQ));
				case '>':
					shiftChar();
					return Token(T_(OP_USHR));
				default:
					return Token(T_(OP_SHR));
				}
			default:
				return Token(T_(OP_LOGICGT));
			}

		case ':': // Either :, ::, or an anonymous label ref
			c = peek();
			switch (c) {
			case ':':
				shiftChar();
				return Token(T_(DOUBLE_COLON));
			case '+':
			case '-': {
				std::string symName = readAnonLabelRef(c);
				return Token(T_(ANON), symName);
			}
			default:
				return Token(T_(COLON));
			}

			// Handle numbers

		case '0': // Decimal, fixed-point, or base-prefix number
			switch (peek()) {
			case 'x':
			case 'X':
				shiftChar();
				return Token(T_(NUMBER), readHexNumber());
			case 'o':
			case 'O':
				shiftChar();
				return Token(T_(NUMBER), readOctalNumber());
			case 'b':
			case 'B':
				shiftChar();
				return Token(T_(NUMBER), readBinaryNumber());
			}
			[[fallthrough]];

			// Decimal or fixed-point number

		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': {
			uint32_t n = readDecimalNumber(c);

			if (peek() == '.') {
				shiftChar();
				n = readFractionalPart(n);
			}
			return Token(T_(NUMBER), n);
		}

		case '&': // Either &=, binary AND, logical AND, or an octal constant
			c = peek();
			if (c == '=') {
				shiftChar();
				return Token(T_(POP_ANDEQ));
			} else if (c == '&') {
				shiftChar();
				return Token(T_(OP_LOGICAND));
			} else if (c >= '0' && c <= '7') {
				return Token(T_(NUMBER), readOctalNumber());
			}
			return Token(T_(OP_AND));

		case '%': // Either %=, MOD, or a binary constant
			c = peek();
			if (c == '=') {
				shiftChar();
				return Token(T_(POP_MODEQ));
			} else if (c == '0' || c == '1' || c == options.binDigits[0]
			           || c == options.binDigits[1]) {
				return Token(T_(NUMBER), readBinaryNumber());
			}
			return Token(T_(OP_MOD));

		case '$': // Hex constant
			return Token(T_(NUMBER), readHexNumber());

		case '`': // Gfx constant
			return Token(T_(NUMBER), readGfxConstant());

			// Handle string and character literals

		case '"': {
			std::string str;
			readString(str, false);
			return Token(T_(STRING), str);
		}

		case '\'': {
			std::string chr;
			readCharacter(chr);
			return Token(T_(CHARACTER), chr);
		}

			// Handle newlines and EOF

		case '\r':
			handleCRLF(c);
			[[fallthrough]];
		case '\n':
			return Token(T_(NEWLINE));

		case EOF:
			return Token(T_(YYEOF));

			// Handle line continuations

		case '\\':
			// Macro args were handled by `peek`, and character escapes do not exist
			// outside of string literals, so this must be a line continuation.
			discardLineContinuation();
			continue;

			// Handle raw strings... or fall through if '#' is not followed by '"'

		case '#':
			if (peek() == '"') {
				shiftChar();
				std::string str;
				readString(str, true);
				return Token(T_(STRING), str);
			}
			[[fallthrough]];

			// Handle identifiers... or report garbage characters

		default:
			bool raw = c == '#';
			if (raw && startsIdentifier(peek())) {
				c = bumpChar();
			} else if (!startsIdentifier(c)) {
				reportGarbageCharacters(c);
				continue;
			}

			Token token = readIdentifier(c, raw);

			// An ELIF after a taken IF needs to not evaluate its condition
			if (token.type == T_(POP_ELIF) && lexerState->lastToken == T_(NEWLINE)
			    && lexer_GetIFDepth() > 0 && lexer_RanIFBlock() && !lexer_ReachedELSEBlock()) {
				return yylex_SKIP_TO_ENDC();
			}

			// If a keyword, don't try to expand
			if (token.type != T_(SYMBOL) && token.type != T_(LOCAL)) {
				return token;
			}

			// `token` is either a `SYMBOL` or a `LOCAL`, and both have a `std::string` value.
			assume(std::holds_alternative<std::string>(token.value));

			// Raw symbols and local symbols cannot be string expansions
			if (!raw && token.type == T_(SYMBOL) && lexerState->expandStrings) {
				// Attempt string expansion
				if (Symbol const *sym = sym_FindExactSymbol(std::get<std::string>(token.value));
				    sym && sym->type == SYM_EQUS) {
					beginExpansion(sym->getEqus(), sym->name);
					return yylex_NORMAL(); // Tail recursion
				}
			}

			// We need to distinguish between label definitions (which start with `LABEL`) and
			// macro invocations (which start with `SYMBOL`).
			//
			// If we had one `IDENTIFIER` token, the parser would need to perform "lookahead" to
			// determine which rule applies. But since macros need to enter "raw" mode to parse
			// their arguments, which may not even be valid tokens in "normal" mode, we cannot use
			// lookahead to check for the presence of a `COLON`.
			//
			// Instead, we have separate `SYMBOL` and `LABEL` tokens, lexing as a `LABEL` if a ':'
			// character *immediately* follows the identifier. Thus, "Label:" and "mac:" are treated
			// as label definitions, but "Label :" and "mac :" are treated as macro invocations.
			//
			// The alternative would be a "lexer hack" like C, where identifiers would lex as a
			// `SYMBOL` if they are already defined, otherwise as a `LABEL`.
			if (token.type == T_(SYMBOL) && peek() == ':') {
				token.type = T_(LABEL);
			}

			return token;
		}
	}
}

static Token yylex_RAW() {
	// This is essentially a highly modified `readString`
	std::string str;
	size_t parenDepth = 0;
	int c;

	// Trim left whitespace (stops at a block comment)
	for (;;) {
		c = peek();
		if (isWhitespace(c)) {
			shiftChar();
		} else if (c == '\\') {
			c = nextChar();
			// If not a line continuation, handle as a normal char
			if (!isWhitespace(c) && c != '\n' && c != '\r') {
				goto backslash;
			}
			// Line continuations count as "whitespace"
			discardLineContinuation();
		} else {
			break;
		}
	}

	for (;;) {
		c = peek();

		switch (c) {
		case '"': // String literals inside macro args
			shiftChar();
			readString(str, false);
			break;

		case '\'': // Character literals inside macro args
			shiftChar();
			readCharacter(str);
			break;

		case '#': // Raw string literals inside macro args
			str += c;
			if (nextChar() == '"') {
				shiftChar();
				readString(str, true);
			}
			break;

		case ';': // Comments inside macro args
			discardComment();
			c = peek();
			[[fallthrough]];
		case '\r': // End of line
		case '\n':
		case EOF:
			goto finish;

		case '/': // Block comments inside macro args
			if (nextChar() == '*') {
				shiftChar();
				discardBlockComment();
				continue;
			}
			str += c; // Append the slash
			break;

		case ',': // End of macro arg
			if (parenDepth == 0) {
				goto finish;
			}
			goto append;

		case '(': // Open parentheses inside macro args
			if (parenDepth < UINT_MAX) {
				++parenDepth;
			}
			goto append;

		case ')': // Close parentheses inside macro args
			if (parenDepth > 0) {
				--parenDepth;
			}
			goto append;

		case '\\': // Character escape
			c = nextChar();

backslash:
			switch (c) {
			case ',': // Escapes only valid inside a macro arg
			case '(':
			case ')':
			case '\\': // Escapes shared with string literals
			case '"':
			case '\'':
			case '{':
			case '}':
				break;

			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case '0':
				c = '\0';
				break;

			case ' ':
			case '\t':
			case '\r':
			case '\n':
				discardLineContinuation();
				continue;

			case EOF: // Can't really print that one
				error("Illegal character escape at end of input");
				c = '\\';
				break;

				// Macro args were already handled by peek, so '\@',
				// '\#', and '\0'-'\9' should not occur here.

			default:
				error("Illegal character escape %s", printChar(c));
				break;
			}
			[[fallthrough]];

		default: // Regular characters will just get copied
append:
			str += c;
			shiftChar();
			break;
		}
	}

finish: // Can't `break` out of a nested `for`-`switch`
	// Trim right whitespace
	auto rightPos = std::find_if_not(str.rbegin(), str.rend(), isWhitespace);
	str.resize(rightPos.base() - str.begin());

	// Returning COMMAs to the parser would mean that two consecutive commas
	// (i.e. an empty argument) need to return two different tokens (STRING
	// then COMMA) without advancing the read. To avoid this, commas in raw
	// mode end the current macro argument but are not tokenized themselves.
	if (c == ',') {
		shiftChar();
		return Token(T_(STRING), str);
	}

	// The last argument may end in a trailing comma, newline, or EOF.
	// To allow trailing commas, raw mode will continue after the last
	// argument, immediately lexing the newline or EOF again (i.e. with
	// an empty raw string before it). This will not be treated as a
	// macro argument. To pass an empty last argument, use a second
	// trailing comma.
	if (!str.empty()) {
		return Token(T_(STRING), str);
	}
	lexer_SetMode(LEXER_NORMAL);

	if (c == '\r' || c == '\n') {
		shiftChar();
		handleCRLF(c);
		return Token(T_(NEWLINE));
	}

	return Token(T_(YYEOF));
}

// This function uses the fact that `if`, etc. constructs are only valid when
// there's nothing before them on their lines. This enables filtering
// "meaningful" (= at line start) vs. "meaningless" (everything else) tokens.
// It's especially important due to macro args not being handled in this
// state, and lexing them in "normal" mode potentially producing such tokens.
static Token skipIfBlock(bool toEndc) {
	lexer_SetMode(LEXER_NORMAL);
	uint32_t startingDepth = lexer_GetIFDepth();

	bool atLineStart = lexerState->atLineStart;
	Defer notAtLineStart{[&] { lexerState->atLineStart = false; }};

	Defer reenableExpansions = scopedDisableExpansions();

	for (int c;; atLineStart = false) {
		// Read chars until EOL
		while (!atLineStart) {
			c = bumpChar();

			if (c == EOF) {
				return Token(T_(YYEOF));
			} else if (c == '\\') {
				// Unconditionally skip the next char, including line continuations
				c = bumpChar();
			} else if (c == '\r' || c == '\n') {
				atLineStart = true;
			}

			if (c == '\r' || c == '\n') {
				handleCRLF(c);
				// Do this both on line continuations and plain EOLs
				nextLine();
			}
		}

		// Skip leading whitespace
		for (;; shiftChar()) {
			c = peek();
			if (!isWhitespace(c)) {
				break;
			}
		}

		if (!startsIdentifier(c)) {
			continue;
		}
		shiftChar();
		switch (Token token = readIdentifier(c, false); token.type) {
		case T_(POP_IF):
			lexer_IncIFDepth();
			break;

		case T_(POP_ELIF):
			if (lexer_ReachedELSEBlock()) {
				// This should be redundant, as the parser handles this error first.
				fatal("Found ELIF after an ELSE block"); // LCOV_EXCL_LINE
			}
			if (!toEndc && lexer_GetIFDepth() == startingDepth) {
				return token;
			}
			break;

		case T_(POP_ELSE):
			if (lexer_ReachedELSEBlock()) {
				fatal("Found ELSE after an ELSE block");
			}
			lexer_ReachELSEBlock();
			if (!toEndc && lexer_GetIFDepth() == startingDepth) {
				return token;
			}
			break;

		case T_(POP_ENDC):
			if (lexer_GetIFDepth() == startingDepth) {
				return token;
			}
			lexer_DecIFDepth();
			break;

		default:
			break;
		}
	}
}

static Token yylex_SKIP_TO_ELIF() {
	return skipIfBlock(false);
}

static Token yylex_SKIP_TO_ENDC() {
	return skipIfBlock(true);
}

static Token yylex_SKIP_TO_ENDR() {
	lexer_SetMode(LEXER_NORMAL);

	bool atLineStart = lexerState->atLineStart;
	Defer notAtLineStart{[&] { lexerState->atLineStart = false; }};

	Defer reenableExpansions = scopedDisableExpansions();

	for (int c;; atLineStart = false) {
		// Read chars until EOL
		while (!atLineStart) {
			c = bumpChar();

			if (c == EOF) {
				return Token(T_(YYEOF));
			} else if (c == '\\') {
				// Unconditionally skip the next char, including line continuations
				c = bumpChar();
			} else if (c == '\r' || c == '\n') {
				atLineStart = true;
			}

			if (c == '\r' || c == '\n') {
				handleCRLF(c);
				// Do this both on line continuations and plain EOLs
				nextLine();
			}
		}

		c = skipChars(isWhitespace);

		if (!startsIdentifier(c)) {
			continue;
		}
		shiftChar();
		switch (readIdentifier(c, false).type) {
		case T_(POP_IF):
			lexer_IncIFDepth();
			break;

		case T_(POP_ENDC):
			lexer_DecIFDepth();
			break;

		default:
			break;
		}
	}
}

yy::parser::symbol_type yylex() {
	if (lexerState->atLineStart && lexerStateEOL) {
		lexerState = lexerStateEOL;
		lexerStateEOL = nullptr;
	}
	if (lexerState->lastToken == T_(EOB) && yywrap()) {
		return yy::parser::make_YYEOF();
	}
	// Newlines read within an expansion should not increase the line count
	if (lexerState->atLineStart && lexerState->expansions.empty()) {
		nextLine();
	}

	static Token (* const lexerModeFuncs[NB_LEXER_MODES])() = {
	    yylex_NORMAL,
	    yylex_RAW,
	    yylex_SKIP_TO_ELIF,
	    yylex_SKIP_TO_ENDC,
	    yylex_SKIP_TO_ENDR,
	};
	Token token = lexerModeFuncs[lexerState->mode]();

	// Captures end at their buffer's boundary no matter what
	if (token.type == T_(YYEOF) && !lexerState->capturing) {
		token.type = T_(EOB);
	}
	lexerState->lastToken = token.type;
	lexerState->atLineStart = token.type == T_(NEWLINE) || token.type == T_(EOB);

	// Uncomment this if you want to debug what the lexer is lexing:
	// fprintf(stderr, "{lexing %s}\n", yy::parser::symbol_type(token.type).name());

	if (std::holds_alternative<uint32_t>(token.value)) {
		return yy::parser::symbol_type(token.type, std::get<uint32_t>(token.value));
	} else if (std::holds_alternative<std::string>(token.value)) {
		return yy::parser::symbol_type(token.type, std::get<std::string>(token.value));
	} else {
		assume(std::holds_alternative<std::monostate>(token.value));
		return yy::parser::symbol_type(token.type);
	}
}

static Capture startCapture() {
	// Due to parser internals, it reads the EOL after the expression before calling this.
	// Thus, we don't need to keep one in the buffer afterwards.
	// The following assumption checks that.
	assume(lexerState->atLineStart);

	assume(!lexerState->capturing && lexerState->captureBuf == nullptr);
	lexerState->capturing = true;
	lexerState->captureSize = 0;

	uint32_t lineNo = lexer_GetLineNo();
	if (std::holds_alternative<ViewedContent>(lexerState->content)
	    && lexerState->expansions.empty()) {
		auto &view = std::get<ViewedContent>(lexerState->content);
		return {
		    .lineNo = lineNo, .span = {.ptr = view.makeSharedContentPtr(), .size = 0}
		};
	} else {
		assume(lexerState->captureBuf == nullptr);
		lexerState->captureBuf = std::make_shared<std::vector<char>>();
		// `.span.ptr == nullptr`; indicates to retrieve the capture buffer when done capturing
		return {
		    .lineNo = lineNo, .span = {.ptr = nullptr, .size = 0}
		};
	}
}

static void endCapture(Capture &capture) {
	// This being `nullptr` means we're capturing from the capture buffer, which is reallocated
	// during the whole capture process, and so MUST be retrieved at the end
	if (!capture.span.ptr) {
		capture.span.ptr = lexerState->makeSharedCaptureBufPtr();
	}
	capture.span.size = lexerState->captureSize;

	// ENDR/ENDM or EOF puts us past the start of the line
	lexerState->atLineStart = false;

	lexerState->capturing = false;
	lexerState->captureBuf = nullptr;
}

Capture lexer_CaptureRept() {
	Capture capture = startCapture();

	Defer reenableExpansions = scopedDisableExpansions();

	size_t depth = 0;

	for (int c;;) {
		nextLine();
		// We're at line start, so attempt to match a `REPT` or `ENDR` token
		do { // Discard initial whitespace
			c = bumpChar();
		} while (isWhitespace(c));
		// Now, try to match `REPT`, `FOR` or `ENDR` as a **whole** keyword
		if (startsIdentifier(c)) {
			switch (readIdentifier(c, false).type) {
			case T_(POP_REPT):
			case T_(POP_FOR):
				++depth;
				break; // Ignore the rest of that line

			case T_(POP_ENDR):
				if (depth) {
					--depth;
					break; // Ignore the rest of that line
				}
				endCapture(capture);
				// The final ENDR has been captured, but we don't want it!
				// We know we have read exactly "ENDR", not e.g. an EQUS
				capture.span.size -= literal_strlen("ENDR");
				return capture;

			default:
				break;
			}
		}

		// Just consume characters until EOL or EOF
		for (;; c = bumpChar()) {
			if (c == EOF) {
				error("Unterminated REPT/FOR block");
				endCapture(capture);
				capture.span.ptr = nullptr; // Indicates that it reached EOF before an ENDR
				return capture;
			} else if (c == '\n' || c == '\r') {
				handleCRLF(c);
				break;
			}
		}
	}
}

Capture lexer_CaptureMacro() {
	Capture capture = startCapture();

	Defer reenableExpansions = scopedDisableExpansions();

	for (int c;;) {
		nextLine();
		// We're at line start, so attempt to match an `ENDM` token
		do { // Discard initial whitespace
			c = bumpChar();
		} while (isWhitespace(c));
		// Now, try to match `ENDM` as a **whole** keyword
		if (startsIdentifier(c) && readIdentifier(c, false).type == T_(POP_ENDM)) {
			endCapture(capture);
			// The ENDM has been captured, but we don't want it!
			// We know we have read exactly "ENDM", not e.g. an EQUS
			capture.span.size -= literal_strlen("ENDM");
			return capture;
		}

		// Just consume characters until EOL or EOF
		for (;; c = bumpChar()) {
			if (c == EOF) {
				error("Unterminated macro definition");
				endCapture(capture);
				capture.span.ptr = nullptr; // Indicates that it reached EOF before an ENDM
				return capture;
			} else if (c == '\n' || c == '\r') {
				handleCRLF(c);
				break;
			}
		}
	}
}
