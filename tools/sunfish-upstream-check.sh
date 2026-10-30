#!/bin/bash
# Fetch the upstream projects in tools/upstream.list and list the commits not surveyed yet (daily upstream check,
# CLAUDE.md "Upstream check"). Clones go to the DIR column (outside git; new ones shallow-history, blobless).
#   tools/sunfish-upstream-check.sh              fetch all, print per project: new commits (oldest last, up to 40)
#   tools/sunfish-upstream-check.sh --mark NAME  record NAME's current head as surveyed (edits tools/upstream.list)
# The full lists also go to research/upstream/new-YYYY-MM-DD.txt.
set -uo pipefail
cd "$(dirname "$0")/.."
L=tools/upstream.list
head_of() {	# DIR BRANCH -> the remote head commit
	if [ "$2" = - ]; then
		git -C "$1" remote set-head origin -a >/dev/null 2>&1
		git -C "$1" rev-parse --short origin/HEAD 2>/dev/null
	else
		git -C "$1" rev-parse --short "origin/$2" 2>/dev/null
	fi
}
if [ "${1:-}" = --mark ]; then
	name=${2:?usage: $0 --mark NAME}
	read -r _ dir _ branch _ < <(grep -E "^$name[[:space:]]" $L) || { echo "no $name in $L"; exit 1; }
	h=$(head_of "$dir" "$branch")
	[ -n "$h" ] || { echo "$name: no head (fetch first)"; exit 1; }
	sed -i -E "s/^($name[[:space:]].*[[:space:]])[^[:space:]]+$/\1$h/" $L
	echo "$name surveyed up to $h"
	exit 0
fi
mkdir -p research/upstream
out=research/upstream/new-$(date +%F).txt
: > "$out"
grep -v '^#' $L | while read -r name dir url branch surveyed; do
	[ -n "$name" ] || continue
	if [ ! -d "$dir/.git" ]; then
		git clone -q --filter=blob:none --no-checkout "$url" "$dir" 2>/dev/null || { echo "== $name: clone FAILED ($url)"; continue; }
	fi
	git -C "$dir" fetch -q origin 2>/dev/null || echo "== $name: fetch FAILED"
	h=$(head_of "$dir" "$branch")
	[ -n "$h" ] || { echo "== $name: no remote head"; continue; }
	if [ "$surveyed" = - ]; then range=$h; n=$(git -C "$dir" rev-list --count "$h"); else range=$surveyed..$h; n=$(git -C "$dir" rev-list --count "$range" 2>/dev/null || echo "?"); fi
	last=$(git -C "$dir" log -1 --format=%cs "$h")
	echo "== $name: $n new commits (head $h, $last; surveyed $surveyed)" | tee -a "$out"
	[ "$n" = 0 ] && continue
	git -C "$dir" log --no-merges --format='   %h %cs %s' "$range" 2>/dev/null | head -40
	git -C "$dir" log --no-merges --format='   %h %cs %s' "$range" >> "$out" 2>/dev/null
done
echo "full lists: $out"
