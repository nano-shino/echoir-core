#!/usr/bin/env bash
# Packs one non-Windows build into dist/echoir-<version>-<label>.<ext>. Windows uses
# tools/package-windows.ps1. Build first; model files are never included.
#
#   tools/package-native.sh desktop <preset> <label>
#       macOS or Linux: the echoir CLI, libechoir and the shared ggml, whisper, llama and mtmd
#       libraries, models/registry.json and the notices, as a .tar.gz. The build RPATH is relative,
#       so the folder runs from anywhere.
#   tools/package-native.sh android <preset> <label>
#       libechoir.so, the ggml, whisper, llama and mtmd .so files and the NDK's libomp.so, which
#       ggml's OpenMP needs and Android does not ship, as a .zip. Fails unless every LOAD segment
#       is 16 KB aligned. Needs ANDROID_NDK_HOME.
#   tools/package-native.sh ios <device-build-dir> <simulator-build-dir> <label>
#       Echoir.xcframework (one static library per slice, all static archives of the build merged,
#       and include/), as a .zip. Needs Xcode.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
# The release version is the project version in CMakeLists.txt (the indented VERSION line).
version="$(awk '/^[[:space:]]+VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+/ { print $2; exit }' "$root/CMakeLists.txt")"
[[ -n "$version" ]] || { echo "Cannot read the project version from CMakeLists.txt." >&2; exit 1; }
dist="$root/dist"
mkdir -p "$dist"
echo '*' > "$dist/.gitignore"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

copy_notices() {
  local dest="$1"
  for f in THIRD_PARTY_NOTICES LICENSE; do
    [[ -f "$root/$f" ]] && cp "$root/$f" "$dest/"
  done
  copy_licenses "$dest"
  return 0
}

# Copies the full third-party license texts into $1/licenses/. A missing source fails the build.
copy_licenses() {
  local dest="$1/licenses" src name
  mkdir -p "$dest"
  for src in "$root/third_party/single/"*.LICENSE.txt "$root/third_party/licenses/"*.txt; do
    [[ -f "$src" ]] || { echo "Missing license file: $src" >&2; exit 1; }
    cp "$src" "$dest/"
  done
  for src in "$root/third_party/whisper.cpp/LICENSE" "$root/third_party/llama.cpp/LICENSE"; do
    [[ -f "$src" ]] || { echo "Missing license file: $src" >&2; exit 1; }
  done
  cp "$root/third_party/whisper.cpp/LICENSE" "$dest/whisper.cpp.LICENSE.txt"
  cp "$root/third_party/llama.cpp/LICENSE" "$dest/llama.cpp.LICENSE.txt"
}

# Copies the runtime libraries matching lib{echoir,ggml,whisper,llama,mtmd}*.<ext> from $1 to $2,
# keeping versioned symlinks.
copy_runtime_libs() {
  local bin="$1" dest="$2" ext="$3"
  local found=0
  shopt -s nullglob
  for f in "$bin"/lib{echoir,ggml,whisper,llama,mtmd}*."$ext"*; do
    cp -P "$f" "$dest/"
    found=1
  done
  shopt -u nullglob
  [[ "$found" == 1 ]] || { echo "No runtime libraries in $bin." >&2; exit 1; }
}

mode="${1:-}"
case "$mode" in
  desktop)
    preset="$2"; label="$3"
    bin="$root/build/$preset/bin"
    [[ -x "$bin/echoir" ]] || { echo "$bin/echoir is missing. Build the preset first." >&2; exit 1; }
    if [[ "$(uname -s)" == Darwin ]]; then ext=dylib; else ext=so; fi
    name="echoir-$version-$label"
    pkg="$stage/$name"
    mkdir -p "$pkg/models"
    cp "$bin/echoir" "$pkg/"
    copy_runtime_libs "$bin" "$pkg" "$ext"
    [[ -e "$pkg/libggml.$ext" ]] || { echo "libggml.$ext is missing." >&2; exit 1; }
    # RelWithDebInfo embeds DWARF in ELF files (over 90% of their size); macOS keeps it in the
    # object files. The symbol table stays, so backtraces keep function names.
    if [[ "$ext" == so ]]; then
      find "$pkg" -maxdepth 1 -type f \( -name echoir -o -name '*.so*' \) -exec strip --strip-debug {} +
    fi
    cp "$root/models/registry.json" "$pkg/models/"
    copy_notices "$pkg"
    cat > "$pkg/README.txt" <<EOF
echoir $version ($label)

echoir                 command line: transcribe, bench, score, devices, models, serve
libechoir.$ext         C ABI library (include/echoir/echoir.h in the repository)
libggml*, libwhisper*,
libllama*, libmtmd*    runtime libraries, found next to the binaries
models/registry.json   model catalogue. Model files go in models/ next to echoir; fetch them
                       with tools/fetch-models.sh from the repository.
licenses/              full license texts of the third-party libraries (THIRD_PARTY_NOTICES
                       lists them)

Quick start (from this folder)
  ./echoir models path whisper-tiny-en
  ./echoir transcribe <file.wav> --model whisper-tiny-en --offline
EOF
    out="$dist/$name.tar.gz"
    tar -C "$stage" -czf "$out" "$name"
    echo "Wrote $out"
    tar -tzvf "$out"
    ;;

  android)
    preset="$2"; label="$3"
    bin="$root/build/$preset/bin"
    [[ -f "$bin/libechoir.so" ]] || { echo "$bin/libechoir.so is missing. Build the preset first." >&2; exit 1; }
    [[ -f "$bin/libomp.so" ]] || { echo "$bin/libomp.so is missing; the Android build copies it from the NDK." >&2; exit 1; }
    : "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME is not set}"
    readelf="$(ls "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-readelf | head -n 1)"
    llvm_strip="$(dirname "$readelf")/llvm-strip"
    name="echoir-$version-$label"
    pkg="$stage/$name"
    mkdir -p "$pkg/arm64-v8a"
    copy_runtime_libs "$bin" "$pkg/arm64-v8a" so
    cp "$bin/libomp.so" "$pkg/arm64-v8a/"
    # The embedded DWARF is most of each library; the symbol table stays.
    "$llvm_strip" --strip-debug "$pkg"/arm64-v8a/*.so
    for so in "$pkg"/arm64-v8a/*.so; do
      # Every LOAD segment's alignment (last column) must be at least 0x4000 for 16 KB pages.
      while read -r align; do
        if (( align < 0x4000 )); then
          echo "$(basename "$so"): LOAD alignment $align is below 16 KB." >&2
          exit 1
        fi
      done < <("$readelf" -lW "$so" | awk '$1 == "LOAD" { print $NF }')
    done
    copy_notices "$pkg"
    cat > "$pkg/README.txt" <<EOF
echoir $version ($label)

arm64-v8a/   libechoir.so (C ABI), the ggml, whisper, llama and mtmd libraries and the NDK's
             libomp.so. Every library is 16 KB page aligned. libc++ is linked statically into
             each library, so libc++_shared.so is not needed.
licenses/    full license texts of the third-party libraries (THIRD_PARTY_NOTICES lists them)
EOF
    out="$dist/$name.zip"
    rm -f "$out"
    (cd "$stage" && zip -qry "$out" "$name")
    echo "Wrote $out"
    unzip -l "$out"
    ;;

  ios)
    device="$2"; simulator="$3"; label="$4"
    name="echoir-$version-$label"
    pkg="$stage/$name"
    mkdir -p "$pkg"
    args=()
    for dir in "$device" "$simulator"; do
      slice="$stage/$(basename "$dir")"
      mkdir -p "$slice"
      archives=()
      while IFS= read -r a; do archives+=("$a"); done < <(find "$dir" -name '*.a' -not -path '*/CMakeFiles/*' | sort)
      [[ "${#archives[@]}" -gt 0 ]] || { echo "No static libraries in $dir. Build the echoir target first." >&2; exit 1; }
      printf '%s: merging %s\n' "$(basename "$dir")" "${archives[*]##*/}"
      libtool -static -no_warning_for_no_symbols -o "$slice/libechoir.a" "${archives[@]}"
      # Debug info is most of the RelWithDebInfo archive; apps don't ship it.
      strip -S "$slice/libechoir.a"
      ls -l "$slice/libechoir.a"
      args+=(-library "$slice/libechoir.a" -headers "$root/include")
    done
    xcodebuild -create-xcframework "${args[@]}" -output "$pkg/Echoir.xcframework"
    copy_notices "$pkg"
    cat > "$pkg/README.txt" <<EOF
echoir $version ($label)

Echoir.xcframework   one static libechoir.a per slice (ios-arm64, ios-arm64-simulator) with the
                     C ABI, the engine and ggml, whisper, llama and mtmd merged; headers in
                     Headers/echoir/echoir.h. Define ECHOIR_STATIC. The device slice uses Metal
                     (shaders embedded); the simulator slice is CPU only.
Link with            libc++, Foundation, Metal, MetalKit, Accelerate, AVFoundation,
                     AudioToolbox, CoreAudio.
licenses/            full license texts of the third-party libraries (THIRD_PARTY_NOTICES
                     lists them)
EOF
    out="$dist/$name.zip"
    rm -f "$out"
    # --norsrc: no AppleDouble ._* entries for extended attributes.
    (cd "$stage" && ditto -c -k --norsrc --keepParent "$name" "$out")
    echo "Wrote $out"
    unzip -l "$out"
    ;;

  *)
    sed -n '2,16p' "$0" >&2
    exit 2
    ;;
esac
