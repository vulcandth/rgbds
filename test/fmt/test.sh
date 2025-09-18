#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$script_dir"
tmpdir="$(mktemp -d)"

# Immediate expansion is the desired behavior.
# shellcheck disable=SC2064
trap "cd; rm -rf ${tmpdir@Q}" EXIT

cp "$src"/../../rgbfmt "$tmpdir"
cd "$tmpdir" || exit 1

RGBFMT=./rgbfmt

bold="$(tput bold)"
resbold="$(tput sgr0)"
red="$(tput setaf 1)"
green="$(tput setaf 2)"
rescolors="$(tput op)"

tests=0
failed=0
rc=0

tryDiff() {
	if ! diff -au --strip-trailing-cr "$1" "$2"; then
		echo "${bold}${red}${3:-$1} mismatch!${rescolors}${resbold}"
		return 1
	fi
	return 0
}

run_case() {
	local input=$1
	local base
	base=$(basename "$input" .in)
	local expected="$src/cases/$base.out"
	local flagsFile="$src/cases/$base.flags"
	local -a flags=()
	local flagLine
	if [[ -f "$flagsFile" ]]; then
		read -r flagLine <"$flagsFile"
		if [[ -n "$flagLine" ]]; then
			read -r -a flags <<<"$flagLine"
		fi
	fi

	((++tests))
	echo "${bold}${green}${base}...${rescolors}${resbold}"

	local our_rc=0
	local workFile="$base"
	local stdoutFile="$base.stdout"
	local stderrFile="$base.stderr"
	: >"$stdoutFile"
	: >"$stderrFile"

	if [[ " ${flags[*]} " == *" --in-place "* ]]; then
		cp "$input" "$workFile"
		"$RGBFMT" "${flags[@]}" "$workFile" >"$stdoutFile" 2>"$stderrFile" || our_rc=$?
		if ! tryDiff "$expected" "$workFile" "$base"; then
			our_rc=1
		fi
		if [[ -s "$stdoutFile" ]]; then
			echo "${bold}${red}${base} produced unexpected stdout${rescolors}${resbold}"
			our_rc=1
		fi
	else
		"$RGBFMT" "${flags[@]}" "$input" >"$stdoutFile" 2>"$stderrFile" || our_rc=$?
		if ! tryDiff "$expected" "$stdoutFile" "$base"; then
			our_rc=1
		fi
	fi

	if [[ -s "$stderrFile" ]]; then
		echo "${bold}${red}${base} produced stderr:${rescolors}${resbold}"
		cat "$stderrFile"
		our_rc=1
	fi

	if [[ $our_rc -ne 0 ]]; then
		((++failed))
	fi

	if [[ $our_rc -ne 0 ]]; then
		rc=1
	fi
}

for input in "$src"/cases/*.in; do
	run_case "$input"
done

if [[ $failed -eq 0 ]]; then
	echo "${bold}${green}All ${tests} tests passed!${rescolors}${resbold}"
else
	echo "${bold}${red}${failed} test(s) failed.${rescolors}${resbold}"
fi

exit $rc
