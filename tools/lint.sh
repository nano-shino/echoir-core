#!/usr/bin/env bash
# Lint and format checks. CI runs this from .github/workflows/lint.yml, with the tool versions
# pinned there. Install the same versions to get the same results: clang-format 23.1.3,
# ruff 0.17.3, shellcheck 0.11.0 and actionlint 1.7.12.
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

all=0
if [[ "${1:-}" == --all ]]; then
  all=1
  shift
fi
base="${1:-origin/main}"

for tool in clang-format ruff shellcheck actionlint; do
  command -v "$tool" >/dev/null || { echo "lint.sh: $tool is not installed" >&2; exit 2; }
done

status=0

if (( all )); then
  changed="$(git ls-files)"
else
  merge_base="$(git merge-base "$base" HEAD)"
  changed="$(git diff --name-only --diff-filter=ACMR "$merge_base"; git ls-files --others --exclude-standard)"
fi

# Vendored code under third_party/ is copied verbatim and is not formatted.
cxx="$(printf '%s\n' "$changed" | grep -E '\.(c|cc|cpp|h|hpp)$' | grep -v '^third_party/' || true)"
py="$(printf '%s\n' "$changed" | grep -E '\.py$' || true)"

echo "== shellcheck"
git ls-files '*.sh' | xargs shellcheck || status=1

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
