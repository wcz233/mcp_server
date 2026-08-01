# Dependency Policy

## Default Policy

- Third-party source is tracked through Git submodules under `external/`.
- Default builds use bundled source from `external/libuv`, `external/jansson`, and `external/mbedtls`.
- `MCP_ALLOW_FETCHCONTENT` remains disabled by policy so the default build stays reproducible and auditable.
- System-library mode is optional and must be enabled explicitly with CMake options.

## Locked Dependencies

```text
libuv
  source repo: https://github.com/libuv/libuv.git
  release tarball: https://dist.libuv.org/dist/v1.52.1/libuv-v1.52.1.tar.gz
  locked version: v1.52.1

jansson
  source repo: https://github.com/akheron/jansson.git
  release tarball: https://github.com/akheron/jansson/releases/download/v2.15.0/jansson-2.15.0.tar.gz
  locked version: v2.15.0

mbedtls
  source repo: https://github.com/Mbed-TLS/mbedtls.git
  release tarball: https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/mbedtls-4.1.1.tar.gz
  locked version: mbedtls-4.1.1
```

Exact commits are recorded in `third_party.lock`.

## Update Procedure

1. Move the target submodule to the desired upstream tag or approved fork commit.
2. Update `third_party.lock` and this document in the same change.
3. Run the repository bootstrap command and a full configure/build/test pass.
4. Keep dependency updates isolated from unrelated feature changes.

## Modification Policy

- Do not patch files directly inside `external/libuv`, `external/jansson`, or `external/mbedtls` in normal development.
- If a patch is unavoidable, prefer a maintained fork or an explicit patch workflow documented in the repository.
- Never replace the submodule workflow with committed archive files.
