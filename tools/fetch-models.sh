#!/usr/bin/env bash
# Download the models listed in models/registry.json.
#
# Usage: tools/fetch-models.sh <id> [<id> ...]
#
# - Idempotent: a file already in models/ with the registry size is skipped.
# - Downloads to <name>.part and renames on success, so a finished file is never partial.
# - A download that is interrupted keeps its .part and resumes on the next run.
# - A size mismatch against the registry fails with a non-zero exit code.
# - An entry with "convert_from" whose hosted file is not there (HTTP 401 or 404; Hugging Face
#   answers 401 for a repository that does not exist) is built from its upstream checkpoints.
#   They are fetched into models/.firered/<id>/ and converted by tools/convert-firered-vad.py.
# - Works from any directory: the repo root is resolved from this script's location.
# - Bash 3.2 compatible (macOS /bin/bash).

set -eu
set -o pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REGISTRY="$ROOT/models/registry.json"
MODELS_DIR="$ROOT/models"
CONVERTER="$ROOT/tools/convert-firered-vad.py"

die() {
  echo "error: $*" >&2
  exit 1
}

# Print the byte size of a file, or -1 when it does not exist.
file_size() {
  if [ -f "$1" ]; then
    wc -c < "$1" | tr -d ' '
  else
    echo -1
  fi
}

# registry_query ids              -> one model id per line
# registry_query files <id>       -> name<TAB>url<TAB>size<TAB>role per file
# registry_query upstream <id>    -> model_url<TAB>cmvn_url, or nothing without convert_from
registry_query() {
  python3 -I -c '
import json, sys
mode, reg_path, want = sys.argv[1], sys.argv[2], sys.argv[3]
with open(reg_path, encoding="utf-8") as fh:
    reg = json.load(fh)
if mode == "ids":
    for m in reg["models"]:
        print(m["id"])
    sys.exit(0)
for m in reg["models"]:
    if m["id"] == want:
        if mode == "upstream":
            source = m.get("convert_from")
            if source:
                print("\t".join([source["model"], source["cmvn"]]))
            sys.exit(0)
        for f in m["files"]:
            print("\t".join([f["name"], f["url"], str(f["size"]), f["role"]]))
        sys.exit(0)
sys.exit(4)
' "$1" "$REGISTRY" "${2:-}"
}

usage() {
  echo "Usage: $(basename "$0") <id> [<id> ...]"
  echo "Downloads the files of each model id into models/."
  echo "Known ids:"
  if [ -f "$REGISTRY" ] && command -v python3 >/dev/null 2>&1; then
    registry_query ids | sed 's/^/  /'
  fi
}

# HTTP status of a GET of <url> with the body discarded; "000" when the server is unreachable.
http_status() {
  curl --silent --location --range 0-0 --output /dev/null --write-out '%{http_code}' "$1" \
    </dev/null || true
}

# Download <url> into <part>, resuming a non-empty <part>. Returns 4 when the hosted copy is not
# there (HTTP 401 or 404) so the caller can fall back to upstream. Other failures stop the script.
download_url() {
  url="$1"
  part="$2"
  rc=0
  fresh=1
  if [ "$(file_size "$part")" -gt 0 ]; then
    echo "          resuming at byte $(file_size "$part")"
    curl --fail --location --retry 3 --retry-delay 2 -C - -o "$part" "$url" </dev/null || rc=$?
    if [ "$rc" -eq 33 ]; then
      # curl exit 33: the server ignored the Range header. Start over.
      echo "          server cannot resume; restarting"
      rm -f "$part"
      rc=0
    else
      fresh=0
    fi
  fi
  if [ "$fresh" -eq 1 ] && [ "$rc" -eq 0 ]; then
    curl --fail --location --retry 3 --retry-delay 2 -o "$part" "$url" </dev/null || rc=$?
  fi
  if [ "$rc" -eq 0 ]; then
    return 0
  fi
  # curl reports some HTTP errors with other exit codes (56 for a 401), so ask for the status.
  case "$(http_status "$url")" in
    401|404) return 4 ;;
  esac
  die "download failed (curl exit $rc): $url; any partial file is kept at $part, rerun to resume"
}

# Download one registry file into models/, verifying its size. Returns 4 when it is not hosted.
fetch_file() {
  name="$1"
  url="$2"
  want="$3"
  role="$4"
  dest="$MODELS_DIR/$name"
  part="$dest.part"

  if [ "$(file_size "$dest")" = "$want" ]; then
    echo "ok        $name ($role, already present)"
    return 0
  fi

  have="$(file_size "$part")"
  if [ "$have" -gt "$want" ]; then
    echo "discard   $name.part (larger than the registry size)"
    rm -f "$part"
    have=-1
  fi

  if [ "$have" != "$want" ]; then
    echo "download  $name ($role, $want bytes)"
    download_url "$url" "$part" || return $?
  fi

  got="$(file_size "$part")"
  if [ "$got" != "$want" ]; then
    rm -f "$part"
    die "size mismatch for $name: got $got bytes, registry says $want (partial removed)"
  fi

  mv -f "$part" "$dest"
  echo "done      $name"
}

# Fetch one upstream checkpoint file into models/.firered/. An existing file is kept.
fetch_upstream() {
  url="$1"
  dest="$2"
  if [ -f "$dest" ]; then
    echo "ok        ${dest#"$ROOT"/} (upstream, already present)"
    return 0
  fi
  echo "download  $url"
  download_url "$url" "$dest.part" || die "upstream file is not available (HTTP 401 or 404): $url"
  mv -f "$dest.part" "$dest"
}

# Build a registry file from the upstream checkpoints it was converted from.
# $4 is the registry's convert_from pair: model_url<TAB>cmvn_url.
convert_from_upstream() {
  id="$1"
  name="$2"
  want="$3"
  upstream="$4"
  dir="$MODELS_DIR/.firered/$id"
  mkdir -p "$dir"
  fetch_upstream "$(printf '%s' "$upstream" | cut -f1)" "$dir/model.pth.tar"
  fetch_upstream "$(printf '%s' "$upstream" | cut -f2)" "$dir/cmvn.ark"

  dest="$MODELS_DIR/$name"
  part="$dest.part"
  rm -f "$part"
  echo "convert   $name (from $dir)"
  python3 -I "$CONVERTER" "$dir/model.pth.tar" "$dir/cmvn.ark" "$part" </dev/null ||
    die "conversion failed for $name"
  got="$(file_size "$part")"
  if [ "$got" != "$want" ]; then
    rm -f "$part"
    die "size mismatch for $name: converted $got bytes, registry says $want (partial removed)"
  fi
  mv -f "$part" "$dest"
  echo "done      $name"
}

if [ $# -eq 0 ]; then
  usage
  exit 2
fi
case "$1" in
  -h|--help)
    usage
    exit 0
    ;;
esac

command -v python3 >/dev/null 2>&1 || die "python3 is required to read $REGISTRY"
command -v curl >/dev/null 2>&1 || die "curl is required"
[ -f "$REGISTRY" ] || die "registry not found: $REGISTRY"

known="$(registry_query ids)" || die "cannot read $REGISTRY"
for id in "$@"; do
  if ! printf '%s\n' "$known" | grep -Fqx -- "$id"; then
    echo "error: unknown model id '$id'" >&2
    usage >&2
    exit 2
  fi
done

mkdir -p "$MODELS_DIR"

for id in "$@"; do
  echo "== $id"
  entries="$(registry_query files "$id")" || die "cannot read the files of $id from $REGISTRY"
  upstream="$(registry_query upstream "$id")" || die "cannot read convert_from of $id from $REGISTRY"
  while IFS="$(printf '\t')" read -r name url size role; do
    rc=0
    fetch_file "$name" "$url" "$size" "$role" || rc=$?
    if [ "$rc" -eq 4 ]; then
      if [ -z "$upstream" ]; then
        die "$url is not available (HTTP 401 or 404), and $id has no convert_from"
      fi
      echo "missing   $name is not hosted at $url; building it from upstream"
      convert_from_upstream "$id" "$name" "$size" "$upstream"
    elif [ "$rc" -ne 0 ]; then
      exit "$rc"
    fi
  done <<< "$entries"
done

echo "all requested models are in $MODELS_DIR"
