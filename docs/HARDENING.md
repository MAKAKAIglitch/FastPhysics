# FastPhysics hardening notes

This document tracks the first hardening pass in the MAKAKAIglitch fork.

## Upstream and provenance

- Upstream: https://github.com/drake1ch/FastPhysics
- Audited upstream base: `2c922269b90e396513e8a4e5dcb6d8874b0698b9`
- Fork maintainer: MAKAKAIglitch
- Mod version: 1.2.0
- Loader version: 1.1.0

The original zlib `LICENSE` is preserved. The source distribution includes it, and the player package carries a copy.
The Bullet/ODE attribution and licence notices in `src/fastlcp.cpp` are retained. This fork does not relicense
third-party code.

## Changes

### DLL initialization

Both DLL entry points hand initialization to a worker thread and return without file I/O, PE scans, vtable/code
patching or nested `LoadLibraryExW` calls while the Windows loader lock is held. The worker is never waited on by
`DllMain`. Thread notifications remain enabled because the projects use the static CRT (/MT) and TLS.

The loader test waits for its worker-completion event before unloading the test DLL. The process-scoped mutex handle
is checked and retained for the process lifetime so reloading the loader does not load the same mod set twice.

The loader resolves each mod by its full path with
`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`.
This limits dependency lookup to the mod directory, game directory and System32; the current directory and PATH are
not searched. This requires Windows 8 or newer (the game's documented target is Windows 10/11).

The loader remains a plugin host, not a trust scanner: every DLL intentionally placed in
`Release\\DLLModules` is executed in the game process with the game's permissions. Third-party mods in that
directory must be independently trusted and checked.

### CPU dispatch

Only `src/fastlcp.cpp` is compiled with `/arch:AVX2`. The DLL host/dispatch source is built for baseline x64 and
checks AVX2, FMA, OSXSAVE and XCR0 before the solver hook can call the optimized kernels. The self-test exits before
executing AVX2 code on unsupported systems.

### Memory patching

`WriteProtected` saves original bytes, checks instruction-cache flushing and page-protection restoration, and
attempts rollback if a patch cannot be completed. The two reflection-probe immediates are changed in one write
transaction. Rollback remains best-effort if the operating system also refuses the rollback operation.

### Solver storage and counters

The solver's boolean state array owns actual `bool` objects instead of reinterpreting byte-vector storage as
`bool`. Diagnostic LCP counters are thread-local, matching the solver's thread-local scratch state.

### Installer

The installer checks Authenticode status and the signer certificate subject when identifying the original Microsoft
runtime, rather than trusting only PE `CompanyName` metadata. An existing backup is preserved and installation
stops if it is not Microsoft-signed. Copies are staged to a temporary sibling and SHA-256 verified before replacement.
The installer no longer explicitly calls `Unblock-File`.

### Packaging and tests

- The player and source archives include the original `LICENSE`.
- `release.bat` runs the mod-loader integration test.
- `SelfTest.exe --solver-only` supports CI without distributing the proprietary game executable.
- Windows CI builds both DLLs, runs loader/solver/patch-helper and installer-backup tests, checks native/script sources for unexpected network, remote-process, persistence and dynamic-download capabilities, and verifies source archive contents.
- On a successful run, CI uploads the two DLLs and both ZIP packages with a SHA-256 manifest for independent scanning.

## Verification limits

Passing CI is not a malware verdict and does not prove compatibility with every Scrap Mechanic build. The game-image
self-test must be run against the exact game executable being targeted. Code-signature checks require Windows to
validate the embedded signature chain; a failed or unavailable validation stops installation rather than trusting
version metadata.

No CI step uploads binaries to VirusTotal or any third-party scanning service. Scan exact ZIP and DLL hashes from
each published release before installing them. The loader and solver still perform invasive, version-specific
in-process hooks; test with the exact game version and keep a backup of the game installation.
