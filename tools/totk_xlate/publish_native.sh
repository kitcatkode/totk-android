#!/bin/zsh
# Build the shader translation service the packaged app runs (totk_xlate --service): the Ryujinx shader translator
# (vendor/ryujinx) with this project's patches (patches/*.patch) and the service (Service.cs), compiled ahead of time
# with NativeAOT into one arm64 program that needs no .NET runtime.
# The Android build (android/build_android.py --shaders) runs prebuilt/<platform>/totk_xlate; run this once, and again
# after changing the translator, its patches or Service.cs. Needs the .NET 10 SDK and network access the first time
# (NuGet packages, the NativeAOT compiler).
# usage: tools/totk_xlate/publish_native.sh [dotnet]   -> prebuilt/linux-x64 (or linux-arm64)/totk_xlate (+ .sha256)
# NativeAOT on Linux also needs clang and zlib (Fedora: dnf install dotnet-sdk-10.0 clang zlib-devel).
set -e
HERE=${0:A:h}
REPO=${HERE:h:h}
DOTNET=${1:-dotnet}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM HUP
mkdir -p "$WORK/ryujinx-src" "$WORK/xlate"
cp -R "$REPO/vendor/ryujinx/." "$WORK/ryujinx-src/"
find "$WORK/ryujinx-src/src" -maxdepth 2 \( -name obj -o -name bin \) -type d -exec rm -rf {} +
for p in "$HERE"/patches/*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done
cp "$HERE"/*.cs "$HERE"/totk_xlate.csproj "$WORK/xlate/"
case "$(uname -s)-$(uname -m)" in
  Darwin-*) RID=osx-arm64; PLATFORM=macos-arm64 ;;
  Linux-x86_64) RID=linux-x64; PLATFORM=linux-x64 ;;
  Linux-aarch64) RID=linux-arm64; PLATFORM=linux-arm64 ;;
  *) echo "publish_native.sh: unsupported host $(uname -s)-$(uname -m)" >&2; exit 2 ;;
esac
"$DOTNET" publish "$WORK/xlate/totk_xlate.csproj" -c Release -r $RID -p:PublishAot=true -p:InvariantGlobalization=true \
  -p:RyuSrc="$WORK/ryujinx-src" -o "$WORK/out"
OUT=$REPO/prebuilt/$PLATFORM
mkdir -p "$OUT"
cp "$WORK/out/totk_xlate" "$OUT/totk_xlate"
if command -v shasum >/dev/null; then shasum -a 256 "$OUT/totk_xlate"; else sha256sum "$OUT/totk_xlate"; fi | awk '{print $1}' > "$OUT/totk_xlate.sha256"
echo "built $OUT/totk_xlate (sha256 $(cat "$OUT/totk_xlate.sha256"))"
