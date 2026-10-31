#!/usr/bin/env bash
# Lint and format checks. CI runs this from .github/workflows/lint.yml. The tool versions are in
# tools/lint-versions.env, and this script refuses to run with other versions.
#
#   tools/lint.sh [--all] [base]
#
# Linters check every file: shellcheck on the shell scripts, actionlint on the workflows (it also
# runs shellcheck on their run: blocks), and ruff check on the Python. Formatters check only the
# files that changed since base (default origin/main), because some files predate the formatting
# rules and are not reformatted yet. Pass --all once they are.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
# shellcheck source=tools/lint-versions.env
. tools/lint-versions.env

all=0
if [[ "${1:-}" == --all ]]; then
  all=1
  shift
fi
base="${1:-origin/main}"

# require tool version command...: the tool must be installed, and its version output must
# contain the pinned version.
require() {
  local tool="$1" want="$2" have
  shift 2
  command -v "$tool" >/dev/null || { echo "lint.sh: $tool is not installed" >&2; exit 2; }
  have="$("$@" 2>&1 || true)"
  if [[ "$have" != *"$want"* ]]; then
    echo "lint.sh: $tool $want is pinned; found: $(head -n 1 <<< "$have")" >&2
    exit 2
  fi
}

require clang-format "$CLANG_FORMAT_VERSION" clang-format --version
require ruff "$RUFF_VERSION" ruff --version
require shellcheck "$SHELLCHECK_VERSION" shellcheck --version
require actionlint "$ACTIONLINT_VERSION" actionlint -version

status=0

if (( all )); then
  changed="$(git ls-files)"
else
  merge_base="$(git merge-base "$base" HEAD)"
  changed="$(
    git diff --name-only --diff-filter=ACMR "$merge_base"
    git ls-files --others --exclude-standard
  )"
fi

# Vendored code under third_party/ is copied verbatim and is not formatted.
cxx="$(printf '%s\n' "$changed" | grep -E '\.(c|cc|cpp|h|hpp)$' | grep -v '^third_party/' || true)"
py="$(printf '%s\n' "$changed" | grep -E '\.py$' || true)"

echo "== shellcheck"
git ls-files '*.sh' | xargs shellcheck -x || status=1

echo "== actionlint"
actionlint || status=1

echo "== ruff check"
git ls-files '*.py' | xargs ruff check --no-cache || status=1

echo "== clang-format"
if [[ -n "$cxx" ]]; then
  printf '%s\n' "$cxx" | xargs clang-format --style=file --dry-run --Werror || status=1
fi

echo "== ruff format"
if [[ -n "$py" ]]; then
  printf '%s\n' "$py" | xargs ruff format --check --no-cache || status=1
fi

if (( status == 0 )); then
  echo "lint: all checks passed"
fi
exit "$status"
