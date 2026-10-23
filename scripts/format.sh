#!/usr/bin/env bash
# scripts/format.sh — clang-format over the library's own sources. Tooling, not library
# code: nothing here is built or linked. Run it from the repo root: the three source dirs
# are named explicitly (src/xud, tests, examples), which is what keeps reference/ (raylib
# headers, NEVER EDIT), src/external/ (vendored Volk + GLFW — upstream code, never
# reformatted in place) and build/, build-san/, bin/ (generated) out of the file list by
# construction. Never widen the list to a bare `src` — that glob now covers src/external/.
# If a new source dir is ever added, add it to the find list.
#
#   ./scripts/format.sh          # format in place
#   ./scripts/format.sh --check  # report + exit non-zero if anything is unformatted
#                                # (GNU xargs answers 123 when any invocation fails, not 1)
#
# clang-format -i exits 0 whether or not it changed anything, so --check is the only form
# that can gate a commit. Style comes from .clang-format at the repo root — if that file is
# missing clang-format falls back to LLVM style silently, so check it exists before trusting
# a run. The tree DOES satisfy it today (verified 2026-10-23: --check is green over all 17
# sources under src/xud, tests and examples), so --check is the commit gate; if it ever turns
# red, run format.sh -i first and re-check.

if [ "${1:-}" = --check ]; then
    find src/xud tests examples -name '*.[ch]' -print0 | xargs -0 -r clang-format --dry-run --Werror
else
    find src/xud tests examples -name '*.[ch]' -print0 | xargs -0 -r clang-format -i
fi
