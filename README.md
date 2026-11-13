# FastPhysics

Scrap Mechanic mod. Replaces the exact constraint solver of the game's physics (`btDantzigSolver::solveMLCP`,
Bullet) with a faster implementation of the same algorithm. Big creations with lots of bearings, pistons and parts
pressing against each other spend 10-30 ms per tick in that one routine; this does the same work in 1-3 ms.
Nothing else in the simulation changes.

Mod page: https://www.nexusmods.com/scrapmechanic/mods/63

## How it works

The game solves every simulation island with `btMLCPSolver` + `btDantzigSolver` (Bullet 2.8x). It builds a dense
matrix `A` for the island (about 6 rows per bearing, 1 per contact normal plus 2 friction rows) and calls
`solveMLCP(A, b, x, lo, hi, limitDependency)`. The mod patches that vtable slot (the class is found through MSVC RTTI,
`.?AVbtDantzigSolver@@`), reads the matrix and vectors from the Bullet objects (layout checked on every call; anything
odd goes to the game's own routine) and solves with `src/fastlcp.cpp`:

- Dantzig's principal pivoting, ported from Bullet's `btDantzigLCP.cpp` (itself from ODE)
- AVX2/FMA kernels for dot products, triangular solves and the LDL^T factorisation (`src/lcpkernels.h`);
  pivots accumulate in double, the float version stalled on closed kinematic loops
- the unbounded rows (joints) and, with the warm start, the rows that were strictly inside their bounds on the previous
  tick are factorised as one block, then checked; wrong guesses are evicted, so the solution is exact either way
- the warm-start cache keys a problem by its structure (row count, friction dependencies, bound kinds), not by the
  bound values, which drift every tick
- on a numerical stall: retry cold, then in the game's row order, then hand the problem back to the game's solver

On one 45-body workshop mech (569-583 rows, 265 of them joints): game 10-14 ms per solve, Bullet's own code rebuilt
with AVX2 7 ms, this 1.0-1.3 ms warm / 1.5-2 ms cold. With six mechs in view the physics step went from 13 ms to
7.6 ms per tick.

## Also in the DLL since 1.1: two fixes for big static builds

Not physics, but found with the same profiler and easier to ship in one mod:

- **Reflection probe placement.** `ReflectionProbeGenerator::run` finds the ground height under the camera by
  sweeping a 32 m `btSphereShape` 250 m down through the world (`PhysicsBase` cast, collision mask 0x89787),
  every frame the camera moves. Against a 49k-part build that query costs 40-50 ms. The two immediates that
  set the sphere radius are patched to `ProbeSweepRadius` (8 m by default): same purpose, a few ms, no frames
  over 33 ms left in the test hall at the highest reflection setting.
- **Navmesh static boxes.** `NavMesh.cpp` registers each box into its cells after a linear search of the cell's
  box list for a duplicate id, which only feeds an assert. The search is jumped over.

Both are byte signatures that must match exactly once; otherwise the DLL leaves the code alone and says so in
the log. Both apply even on CPUs without AVX2 (the solver part then stays off).

## Experimental Scrap Mechanic 1.0.7 safety/performance test build

Branch `test/scrap-mechanic-1.0.7-signature-diagnostics` produces the separate package
`FastPhysics-1.2.7.zip`. It is intentionally not the stable 1.2.0 release.

### Build and first diagnostic pass

From the repository root on Windows with Visual Studio 2022 C++ tools installed:

```bat
release.bat "D:\SteamLibrary\steamapps\common\Scrap Mechanic\Release\ScrapMechanic.exe"
```

The build runs the local installer/source checks, builds both DLLs and the self-test, tests the target
`ScrapMechanic.exe` as a non-executed image, and creates `dist/FastPhysics-1.2.7.zip`. The self-test prints
the target executable SHA-256 and the two optional signature counts. The resulting package ships with a
blank executable pin, so its first in-game load is diagnostic-only and cannot hook or patch the game.

### Fail-closed compatibility after game updates

- `ExpectedGameExeSHA256` in `FastPhysics.ini` is an explicit allowlist pin for the exact game executable.
  The default is blank. With a blank or mismatched hash, the DLL only logs the executable SHA-256 and scans
  signatures; it does **not** patch game code or install the physics hook.
- When Scrap Mechanic is updated, the executable hash changes and FastPhysics stays inactive until that new
  executable is tested and explicitly pinned. A version number alone is not treated as proof of compatibility.
- Probe-sweep and NavMesh patches are independently configurable. This test config requests an 8 m probe radius
  and enables the NavMesh fix; each patch still requires the exact executable hash gate and one exact, unique
  signature. Missing or duplicate signatures leave that feature unchanged. Relaxed signatures are diagnostic
  only and are never used to modify executable memory.
- Solver buffers are checked for committed/readable memory; the output vector must also be writable. Unexpected
  layouts and sizes fall back to the original solver.

### Protection against a wrong physics result

- The first 512 solver calls run both the game solver and FastPhysics. The vanilla output is saved first.
- If either solver fails, their results differ beyond the configured tolerance, or an independent LCP
  complementarity/bounds check fails, the vanilla output is restored and FastPhysics is disabled for the rest
  of that game process.
- After the verification window, the independent LCP validation still runs on every FastPhysics result. A
  failure returns control to the vanilla solver and disables FastPhysics for the process.
- The verification window costs extra CPU time because it runs both solvers. Measure performance only after
  the first 512 solver calls have completed and the log confirms no mismatch.

This is a conservative test path, not a mathematical guarantee against every possible game bug or crash. The
mod does not open or write world-save files, but physics affects the live simulation; test on a copied world
and keep the original save untouched.

## Layout

- `src/FastPhysics.cpp` - the DLL: hook, settings, warm-start cache, stats, log
- `src/fastlcp.cpp`, `src/fastlcp.h`, `src/lcpkernels.h` - the solver, standalone
- `tests/SelfTest.cpp` - loads `ScrapMechanic.exe` as an image and checks the hook target; solves generated problems in
  every kernel variant and validates them against the LCP conditions; runs the hook on fake Bullet objects
- `installer/` - player package: `Install.bat` / `Uninstall.bat`, PowerShell scripts, `FastPhysics.ini`, README
- `SMModLoader/` - the small loader the mod ships with (replaces `vcruntime140_1.dll` and loads `Release\DLLModules\*.dll`)
- `docs/` - the Nexus page text and images

## Building

Visual Studio 2022 with the C++ workload (`build.bat` finds it through vswhere).

```
build.bat                         builds bin\FastPhysics.dll and build\SelfTest.exe
build\SelfTest.exe --solver-only  runs solver, hook and protected-write tests without the game installed
build\SelfTest.exe [game.exe]     also checks the target game image and byte signatures
release.bat                       builds, tests the loader, runs the full self-test and packs dist\
```

Only `src/fastlcp.cpp` is compiled with `/arch:AVX2`. The host code and CPU feature check are built for baseline x64,
so systems without AVX2/FMA are rejected before entering the optimized kernels. The self-test exits before executing
AVX2 tests when the processor or OS lacks AVX2/FMA.

## Settings

`FastPhysics.ini` next to the DLL:

- `ProbeSweepRadius=8`: set a radius from 1 to 31 metres; `0` keeps the game's 32 m radius unchanged.
- `NavMeshFix=1`: skips the duplicate static-box walk; `0` leaves the game's code unchanged.
- These two patches are independent. A patch only runs after the exact EXE SHA-256 pin matches and its full
  signature appears exactly once. A missing or duplicate signature causes a no-op, not a relaxed-signature patch.
- `DiagnoseSignatures=1` logs exact and relaxed candidates; relaxed candidates are never patched.
- Other settings include `Enabled`, `WarmStart`, `LogIntervalSeconds`, `Verify` (runs the game's solver too and logs
  the difference), `MinRows`, and `DumpFailures`.

Set either optional feature to `0` to test the other one in isolation.

## Licence

zlib (see `LICENSE`). `src/fastlcp.cpp` is derived from Bullet Physics' `btDantzigLCP.cpp` (zlib), which is a port
of the Open Dynamics Engine LCP solver (BSD / LGPL dual licence); the file keeps the original notices.


## MAKAKAIglitch hardening fork

This is a community-maintained hardening fork of [drake1ch/FastPhysics](https://github.com/drake1ch/FastPhysics).
It keeps the original project attribution and zlib licence. The Bullet/ODE-derived solver notices in
`src/fastlcp.cpp` are retained. Changes in this fork are documented in [docs/HARDENING.md](docs/HARDENING.md).

This fork is not an upstream release and its binaries are not certified malware-free. Review the source and scan
release artifacts before installing them. Windows CI builds and exercises the loader, solver and packaging checks;
the full game-image compatibility test still requires the installed Scrap Mechanic executable.
