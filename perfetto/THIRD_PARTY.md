# Third-party dependency

This module uses the official **Perfetto v58.2 C++ SDK**, maintained by the
Perfetto authors / The Android Open Source Project, under the Apache License 2.0.
The downloaded source retains the upstream copyright and license notices.

- Source: https://github.com/google/perfetto/tree/v58.2
- SDK archive: https://github.com/google/perfetto/releases/download/v58.2/perfetto-cpp-sdk-src.zip
- SHA-256: `e56126ff81f0f914d35fe03c72419d147acf2890f36fbda7d4558fd5b84c4b09`
- License: https://github.com/google/perfetto/blob/v58.2/LICENSE

The SDK is fetched into the build directory, or supplied through
`PERFLENS_PERFETTO_SDK_DIR`. It is compiled into `libperflens_trace`.
The command-line tool executes an external official `tracebox`; it does not
download or install executables at runtime.
