#!/usr/bin/env bash
# scripts/format.sh — clang-format over the library's own sources. Tooling, not library
# code: nothing here is built or linked. Run it from the repo root: the three source dirs
# are named explicitly, which is what keeps reference/ (raylib headers, NEVER EDIT) and
# build/, build-san/, bin/ (generated) out of the file list by construction. If a new
# source dir is ever added, add it to the find list.
#
#   ./scripts/format.sh          # format in place
#   ./scripts/format.sh --check  # report + exit non-zero if anything is unformatted
#                                # (GNU xargs answers 123 when any invocation fails, not 1)
#
# clang-format -i exits 0 whether or not it changed anything, so --check is the only form
# that can gate a commit. Style comes from .clang-format at the repo root — if that file is
# missing clang-format falls back to LLVM style silently, so check it exists before trusting
# a run. The tree does NOT satisfy the committed .clang-format yet (see todo.md, 2026-10-22),
# so --check is red today: it reports all 17 sources.

if [ "${1:-}" = --check ]; then
    find src tests examples -name '*.[ch]' -print0 | xargs -0 -r clang-format --dry-run --Werror
else
    find src tests examples -name '*.[ch]' -print0 | xargs -0 -r clang-format -i
fi
