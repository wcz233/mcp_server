# External Dependencies

`external/` stores source-level third-party dependencies that are locked by Git submodule commits.

Current dependencies:

- `external/libuv`: `v1.52.1`
- `external/jansson`: `v2.15.0`

Initialize or refresh them with:

```powershell
git submodule sync --recursive
git submodule update --init --recursive
```

Do not commit downloaded tarballs into this repository. Record source URLs and locked commits in `third_party.lock` and `docs/dependency_policy.md`.
