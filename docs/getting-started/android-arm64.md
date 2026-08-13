# Android ARM64 build

FloofClaw cross-compiles its ordinary CLI binary for 64-bit Android. The
result is:

```text
dist/android-arm64/fclaw
```

It has the same `main()` and commands as the host build. Android applications
may choose their own APK transport or installation mechanism after this build;
that packaging does not change or rename the FloofClaw product artifact.

## Inputs

The build requires:

- an Android NDK exposed as `ANDROID_NDK_ROOT` (or `ANDROID_NDK_HOME`);
- an ARM64/API-compatible static libcurl installation;
- an ARM64/API-compatible static OpenSSL installation.

Use one dependency root with this layout:

```text
android-arm64-static-deps/
├── curl/
│   ├── include/curl/curl.h
│   └── lib/libcurl.a
└── openssl/
    ├── include/openssl/ssl.h
    └── lib/
        ├── libssl.a
        └── libcrypto.a
```

Then build:

```bash
ANDROID_NDK_ROOT="$ANDROID_HOME/ndk/27.0.12077973" \
FCLAW_ANDROID_DEPS_ROOT=/absolute/path/to/android-arm64-static-deps \
make android-arm64
```

The default Android API is 28. Override it only when the dependency archives
were built for a compatible API:

```bash
FCLAW_ANDROID_API=28 make android-arm64
```

Separate dependency roots are also accepted:

```bash
FCLAW_ANDROID_CURL_ROOT=/absolute/path/to/curl \
FCLAW_ANDROID_OPENSSL_ROOT=/absolute/path/to/openssl \
make android-arm64
```

`FCLAW_ANDROID_OUTPUT` may relocate the result, but its basename must remain
`fclaw`. This prevents Android application packaging conventions from leaking
back into the FloofClaw build.

## Verification

The builder verifies that the output is an ELF64 AArch64 executable using
Android's `/system/bin/linker64`. A connected ARM64 emulator or device can run
the ordinary version command directly:

```bash
adb push dist/android-arm64/fclaw /data/local/tmp/fclaw
adb shell chmod 0755 /data/local/tmp/fclaw
adb shell /data/local/tmp/fclaw --version -h
```

For HTTPS providers and TLS channels, set the normal certificate environment
for the installation, such as `SSL_CERT_FILE`, to the device's readable CA
bundle. FloofClaw configuration, floops, actions, and workspace remain normal
runtime files beside the installed executable; they are not compiled into an
Android-specific runtime.
