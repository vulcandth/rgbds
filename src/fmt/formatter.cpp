// SPDX-License-Identifier: MIT

#include "fmt/formatter.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool isSpace(char c) {
	return c == ' ' || c == '\t';
}

void rtrimInPlace(std::string &text) {
	while (!text.empty() && isSpace(text.back())) {
		text.pop_back();
	}
}

std::string_view ltrim(std::string_view text) {
	std::size_t idx = 0;
	while (idx < text.size() && isSpace(text[idx])) {
		++idx;
	}
	return text.substr(idx);
}

std::string makeIndent(std::size_t depth, FormatterConfig const &config) {
	if (depth == 0) {
		return {};
	}
	if (config.useTabs) {
		return std::string(depth, '\t');
	}
	return std::string(depth * config.indentWidth, ' ');
}

bool isLabelStart(char c) {
	unsigned char uc = static_cast<unsigned char>(c);
	return std::isalpha(uc) || c == '_' || c == '.' || c == '@' || c == '?' || c == '$';
}

bool isLabelChar(char c) {
	unsigned char uc = static_cast<unsigned char>(c);
	return std::isalnum(uc) || c == '_' || c == '.' || c == '@' || c == '?' || c == '$';
}

struct LineSplit {
	std::string label;
	std::string body;
};

LineSplit splitLabel(std::string_view code) {
	LineSplit info{};

	if (code.empty()) {
		return info;
	}

	if (code[0] == ':') {
		std::size_t idx = 1;
		while (idx < code.size() && (code[idx] == '+' || code[idx] == '-')) {
			++idx;
		}
		info.label = std::string(code.substr(0, idx));
		info.body = std::string(ltrim(code.substr(idx)));
		return info;
	}

	if (!isLabelStart(code[0])) {
		info.body = std::string(code);
		return info;
	}

	std::size_t idx = 1;
	while (idx < code.size() && isLabelChar(code[idx])) {
		++idx;
	}
	if (idx >= code.size() || code[idx] != ':') {
		info.body = std::string(code);
		return info;
	}

	++idx;
	if (idx < code.size() && code[idx] == ':') {
		++idx;
	}
	info.label = std::string(code.substr(0, idx));
	info.body = std::string(ltrim(code.substr(idx)));
	return info;
}

std::size_t findComment(std::string const &line) {
	bool inSingle = false;
	bool inDouble = false;

	for (std::size_t i = 0; i < line.size(); ++i) {
		char c = line[i];
		if (c == '\\') {
			++i;
			continue;
		}
		if (c == '"' && !inSingle) {
			inDouble = !inDouble;
			continue;
		}
		if (c == '\'' && !inDouble) {
			inSingle = !inSingle;
			continue;
		}
		if (c == ';' && !inSingle && !inDouble) {
			return i;
		}
	}

	return std::string::npos;
}

std::string uppercase(std::string_view token) {
	std::string out(token);
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
		return static_cast<char>(std::toupper(c));
	});
	return out;
}

bool matchesToken(std::string const &token, std::initializer_list<std::string_view> values) {
	for (std::string_view candidate : values) {
		if (token == candidate) {
			return true;
		}
	}
	return false;
}

std::string firstToken(std::string_view text) {
	text = ltrim(text);
	std::size_t end = 0;
	while (end < text.size() && !isSpace(text[end])) {
		++end;
	}
	return std::string(text.substr(0, end));
}

} // namespace

Formatter::Formatter(FormatterConfig const &config_) : config(config_) {}

std::string Formatter::format(std::string_view input) {
	std::vector<std::string> formatted;
	formatted.reserve(input.size() / 32 + 1);

	std::size_t indentLevel = 0;
	std::size_t blankCount = 0;

	auto emitLine = [&](std::string line) {
		formatted.emplace_back(std::move(line));
	};

	std::size_t pos = 0;
	while (pos <= input.size()) {
		std::size_t next = input.find('\n', pos);
		bool hasNewline = next != std::string_view::npos;
		std::size_t len = hasNewline ? next - pos : input.size() - pos;
		std::string raw(input.substr(pos, len));
		if (!raw.empty() && raw.back() == '\r') {
			raw.pop_back();
		}
		if (!hasNewline && pos == input.size()) {
			break;
		}

		if (config.trimTrailingWhitespace) {
			rtrimInPlace(raw);
		}

		std::size_t commentPos = findComment(raw);
		std::string comment;
		std::string code = raw;
		if (commentPos != std::string::npos) {
			comment = raw.substr(commentPos);
			code = raw.substr(0, commentPos);
		}
		if (config.trimTrailingWhitespace) {
			rtrimInPlace(code);
		}

		std::string_view codeView = ltrim(code);

		std::size_t nextPos = hasNewline ? next + 1 : input.size();

		bool hasComment = !comment.empty();
		if (codeView.empty() && !hasComment) {
			if (config.maxConsecutiveBlankLines != 0 && blankCount < config.maxConsecutiveBlankLines) {
				++blankCount;
				emitLine("");
			}
			pos = nextPos;
			if (!hasNewline) {
				break;
			}
			continue;
		}
		blankCount = 0;

		LineSplit split = splitLabel(codeView);
		if (config.trimTrailingWhitespace && !split.body.empty()) {
			rtrimInPlace(split.body);
		}
		bool hasLabel = !split.label.empty();
		bool hasBody = !split.body.empty();

		std::string token = uppercase(firstToken(split.body));

		bool dedentNow = matchesToken(
		    token,
		    {
		        "ENDM",
		        "ENDC",
		        "ENDR",
		        "ENDL",
		        "ENDU",
		    }
		)
		    || matchesToken(token, {"ELSE", "ELIF", "NEXTU"});

		bool reindentAfter = matchesToken(token, {"ELSE", "ELIF", "NEXTU"});
		bool suppressBaseIndent = !hasLabel && indentLevel == 0
		                         && matchesToken(token, {"SECTION", "ENDSECTION"});

		if (dedentNow && indentLevel > 0) {
			--indentLevel;
		}

		std::size_t indentDepth = indentLevel;
		bool commentOnly = !hasBody && hasComment;
		if (hasBody || commentOnly) {
			if (!suppressBaseIndent) {
				indentDepth += config.baseIndent;
			}
		}

		std::string indent = makeIndent(indentDepth, config);

		if (!comment.empty() && config.trimTrailingWhitespace) {
			rtrimInPlace(comment);
		}

		std::string line;
		if (hasLabel) {
			line += split.label;
			if (hasBody) {
				line += indent;
				line += split.body;
			} else if (commentOnly && !indent.empty()) {
				line += indent;
			}
		}

		if (!hasLabel) {
			if (hasBody) {
				line += indent;
				line += split.body;
			} else if (commentOnly) {
				line += indent;
			}
		}

		if (hasComment) {
			if (!line.empty() && !isSpace(line.back())) {
				line.push_back(' ');
			}
			line += comment;
		}

		emitLine(std::move(line));

		if (matchesToken(
		        token,
		        {
		            "MACRO",
		            "REPT",
		            "FOR",
		            "IF",
		            "IFDEF",
		            "IFNDEF",
		            "UNION",
		            "LOAD",
		        }
		    )
		    || reindentAfter) {
			++indentLevel;
		}

		pos = nextPos;
		if (!hasNewline) {
			break;
		}
	}

	while (!formatted.empty() && formatted.back().empty()) {
		formatted.pop_back();
	}

	std::string output;
	if (formatted.empty()) {
		return output;
	}

	std::size_t estimated = 1;
	for (std::string const &line : formatted) {
		estimated += line.size() + 1;
	}
	output.reserve(estimated);

	for (std::size_t i = 0; i < formatted.size(); ++i) {
		output += formatted[i];
		output.push_back('\n');
	}

	return output;
}

std::string formatBuffer(std::string_view input, FormatterConfig const &config) {
	Formatter formatter(config);
	return formatter.format(input);
}
