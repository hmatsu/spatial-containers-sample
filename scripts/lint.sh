#!/usr/bin/env bash
# Runs the repository's linters and static analysis.
#
# Usage: scripts/lint.sh [--fix]
#   --fix  apply automatic fixes (formatting) before checking
#
# Tools (versions used in CI): clang-format and clang-tidy 18, gdtoolkit 4.5
# (gdformat, gdlint), markdownlint-cli2 0.18, actionlint 1.7 with shellcheck,
# editorconfig-checker 3, shellcheck. Override the clang tools with CLANG_FORMAT / CLANG_TIDY.
# clang-tidy needs a compile database; it is configured in LINT_BUILD_DIR
# (default: build-lint) if missing.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

CLANG_FORMAT=${CLANG_FORMAT:-clang-format}
CLANG_TIDY=${CLANG_TIDY:-clang-tidy}
LINT_BUILD_DIR=${LINT_BUILD_DIR:-build-lint}
GD_LINE_LENGTH=120

fix=false
if [[ "${1:-}" == "--fix" ]]; then fix=true; fi

mapfile -t cpp_files < <(git ls-files '*.cpp' '*.h')
mapfile -t gd_files < <(git ls-files '*.gd')
mapfile -t md_files < <(git ls-files '*.md')
mapfile -t all_files < <(git ls-files)
mapfile -t sh_files < <(git ls-files '*.sh')
# Direct3D 11 code only compiles on Windows, so clang-tidy skips it here.
mapfile -t tidy_files < <(git ls-files '*.cpp' | grep -v '_d3d11\.cpp$')

failed=()
run() {
    local name=$1
    shift
    echo "== $name"
    if ! "$@"; then failed+=("$name"); fi
}

if $fix; then
    "$CLANG_FORMAT" -i "${cpp_files[@]}"
    gdformat --line-length "$GD_LINE_LENGTH" "${gd_files[@]}"
    markdownlint-cli2 --fix "${md_files[@]}" > /dev/null || true
fi

run clang-format "$CLANG_FORMAT" --dry-run --Werror "${cpp_files[@]}"

if [[ ! -f "$LINT_BUILD_DIR/compile_commands.json" ]]; then
    cmake -S samples/cpp-minimal -B "$LINT_BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null
fi
run clang-tidy "$CLANG_TIDY" -p "$LINT_BUILD_DIR" --quiet "${tidy_files[@]}"

run gdformat gdformat --check --line-length "$GD_LINE_LENGTH" "${gd_files[@]}"
run gdlint gdlint "${gd_files[@]}"
run markdownlint markdownlint-cli2 "${md_files[@]}"
run actionlint actionlint
if ((${#sh_files[@]})); then run shellcheck shellcheck "${sh_files[@]}"; fi
run editorconfig ec "${all_files[@]}"

if ((${#failed[@]})); then
    echo "Failed: ${failed[*]}"
    exit 1
fi
echo "All checks passed."
