# Original USA native startup runner

`hmawl_original_startup` is a console executable for Windows (also portable to
other native C++ hosts). It executes the original USA revision 0 DOL entry,
not the Direct3D renderer prototype or a synthetic game. This path is available
with `BUILD_TESTING=OFF`; its reusable library is
`hmawl_original_startup_runtime`.

## Build and run

First extract your own supported RVZ with the existing `hmawl_recompiler
--extract-rvz` command. Keep the DOL and all generated game code under ignored
`build\`. From a Windows CMake/Visual Studio environment:

```powershell
cmake -S . -B build\native -DHMAWL_ORIGINAL_US_DOL:FILEPATH="$PWD\build\game-us-r0\main.dol"
cmake --build build\native --config Release --target hmawl_original_startup
build\native\native\recompiler\Release\hmawl_original_startup.exe --original-startup build\game-us-r0\main.dol
```

Optional positive decimal limits are `--max-blocks 1000000` and
`--max-depth 128`. The runner never reports a successful game boot. Exit codes:
`1` input/configuration error, `2` invalid invocation, `3` unsupported dependency
or integrity error, `4` exhausted execution limit, `5` unexpected entry return.
Both normal progress and exact stopping diagnostics are printed; PC, LR, SP and
R3–R5 are preserved in the reusable result.

## What actually runs

The build-time generator and runtime independently verify DOL SHA-1
`1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`. The loader copies original DOL
sections into 24 MiB of coherent guest RAM, with physical/cached/uncached aliases.
Unspecified RAM starts zero; this is a documented native loader profile, not
emulation of IPL/apploader execution or a dump of physical-console power-on RAM.
No disc identity, EXI identity or device responses are invented.

The loader handoff has IR/DR set (`MSR=0x30`), external interrupts disabled and
idle zero PI/MI/DSP/AI/EXI register state explicitly enabled. EXI idle transfer
controls and the restricted original initialization/ROMDIS writes are enabled;
chip selection and transfers remain unsupported. Cache operations use the
existing checked coherent-RAM implementation. Time-base reads use the real
monotonic native clock at the GameCube's 40.5 MHz rate. No guest calls are
replaced with test hooks, native service callbacks or success-returning stubs.
This deliberately limited platform profile is not complete GameCube hardware.

`original_startup_manifest.h` is the reusable list of original address ranges.
The build generator translates these ranges to C++, never executes PPC, and
never writes extracted game code to the source tree. It splits at every branch,
direct branch target and range boundary, with at most 32 guest instructions per
compiled block. Every loop therefore crosses a dispatcher budget boundary;
the instruction count is bounded above by 32 times the block budget.
Calls also have a separate depth bound. This is static compiled dispatch, not
an instruction decoder/interpreter at runtime. The dispatcher checks original
instruction words at registration and every block entry. The recompiler's
`yieldAtBranches` option routes control transfers back through dispatch, including
self-loops; `precisePc` records the exact PC before every original instruction.

## Current boundary — not a game boot

The actual entry reaches original hardware/FPR/GQR/cache setup, original
ROM/BSS initialization, argument setup and `DBInit`, then executes the original
`OSInit` prefix: start time, interrupt masking, performance resets, arena and
exception/system-call setup, alarms/modules, complete interrupt setup, reset
handler, context and warm cache initialization. It enters original `EXIInit`,
resets idle EXI status/ROMDIS state, installs the original EXI handlers and
follows original `EXIGetID`/lock/select code.

It stops explicitly at **PC `0x801F6F24`**, original `EXISelect`'s attempt to
write **`0x00000200` to `0xCC006800`**, selecting channel 0 device 2.
This requires a real supported EXI peripheral model; no ID is fabricated.
`EXIInit` and `OSInit` have **not returned**. The game main loop, GX renderer,
DVD startup, threads, audio, input and saves have not run.

## Targeted regression

With testing enabled, build `hmawl_original_startup_tests` and run CTest's
`hmawl_original_startup_tests`. It uses the production library and real verified
DOL, checking the exact stopping instruction and MMIO value, initialization
evidence, missing clock and unbound dispatcher dependencies, on-disk DOL
truncation/mutation rejection, loaded-code integrity, block/call-depth bounds,
the original nonconverging `OSGetTime` sampling loop, callback restoration and
rejection of resuming an incomplete startup as fresh. Disk fixtures are created
in a uniquely owned directory beside the build-local DOL and narrowly cleaned.
Generated bindings and the input DOL remain local build inputs.
