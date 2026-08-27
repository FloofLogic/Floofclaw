# Prebuilt binaries

Beginning with v0.30.0, each tagged release carries one native binary asset:

- `floofclaw-vX.Y.Z-darwin-arm64.tar.gz` (with a sibling `.sha256` file)

The archive contains the stripped all-adapter `release-small` binary,
`LICENSE`, and a short install note. The binary links OpenSSL statically and
uses Apple's system libcurl, so it has no Homebrew rpath. Extract it, put
`bin/fclaw` on `PATH`, and run `fclaw setup -h`.

Download the archive and its checksum from
[FloofClaw Releases](https://github.com/FloofLogic/Floofclaw/releases).

There is no prebuilt Linux binary. A portable one would have to be a static
musl build, which needs a container or a cross-compiler, and neither is part
of the release process. On Linux, build from source — it takes seconds:

```bash
make -j4
./bin/fclaw setup -h
```

See [Installation and Build](installation-and-build.md) for the two optional
libraries (OpenSSL 3, libcurl).

The archive is built and verified on the maintainer's Mac by
`scripts/build_binary_archive.sh`; no hosted CI is involved.
