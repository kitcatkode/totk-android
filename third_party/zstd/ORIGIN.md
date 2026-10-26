# zstd decompressor (single file)

`zstddeclib.c`: Zstandard 1.5.6's decompressor as one file, generated from the release tarball
(https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz, sha256
8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1) with its own tool:
`cd build/single_file_libs && python3 combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c zstddeclib-in.c`.
`zstd.h`: the public header from lib/, for the callers. Unmodified. License: BSD (LICENSE; zstd is dual BSD / GPLv2, used here under BSD).

Used by runtime/bottom_assets.c to read the game's .zs files (with the dictionaries from Pack/ZsDic.pack.zs).
