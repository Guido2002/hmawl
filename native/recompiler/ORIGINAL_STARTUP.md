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
No connected disc/EXI identity is invented.

The loader handoff has IR/DR set (`MSR=0x30`), external interrupts disabled and
idle zero PI/MI/DSP/AI/EXI register state explicitly enabled. EXI idle transfer
controls, status W1C, sticky ROMDIS, one-hot chip selection and immediate
transactions are enabled. External memory-card slots and Serial Port 1 are
explicitly empty: the backend consumes writes and returns zero for absent
external devices, rather than claiming a connected peripheral identity.
Internal RTC/SRAM/ROM and AD16 transactions fail explicitly. No saves exist.
DI configuration is an explicitly mapped read-only value of 1, not a DVD
command implementation. Cache operations use the
existing checked coherent-RAM implementation. Time-base reads use the real
monotonic native clock at the GameCube's 40.5 MHz rate. Original `OSReport`
and `OSRegisterVersion` run as compiled game code. The C-library `vprintf`
boundary is an original-word-verified native console service with bounded
guest strings and PowerPC GPR/overflow argument decoding. Currently only plain
text, `%s` and `%%` are supported; other conversions, missing sinks and output
errors fail explicitly before a success return. It emits the actual SDK
version string; it does not discard logging.
The loader supplies 162 MHz bus and 486 MHz CPU clock metadata in low memory,
consistent with the 40.5 MHz time base. VI cable status is explicitly analog
(DTV bit zero); this does not implement a display or renderer.
SI has disabled-polling timing, communication RAM, error W1C and checked transfer
semantics. All four controller ports are explicitly disconnected. A transaction
sets the selected channel's NOREP error and COMERR/TCINT, without inventing reply
bytes. Backend failures or invalid results cannot commit hardware completion.
Automatic controller polling remains an explicit error. Completion interrupts
remain pending while EE is disabled; enabling EE or dispatching with an unmasked,
pending SI completion fails explicitly until guest interrupt delivery exists.
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
follows original `EXIGetID`/lock/select/immediate/sync/deselect code. The two-byte
ID command and four-byte read run against the explicit empty SP1 port. Original
code unlocks the channel, probes both empty memory-card slots, reports the SDK
version to the console and returns from **EXIInit**.

Original **SIInit also returns** after version reporting, sampling configuration,
interrupt handler registration/unmasking, the first controller type transaction
and queuing the remaining three probes. The first transaction has completed with
the configured disconnected-port error, but its original guest interrupt handler
has not run. This is not completed controller enumeration or usable input.

The next untranslated dependency is **__OSInitSram at PC `0x801B5E98`,
LR `0x801B0F94`**. `EXIInit` and `SIInit` have returned; `OSInit` has **not**.
The game main loop, GX renderer,
DVD startup, threads, audio, input and saves have not run.

## Targeted regression

With testing enabled, build `hmawl_original_startup_tests` and run CTest's
`hmawl_original_startup_tests`. It uses the production library and real verified
DOL, checking the exact SRAM initialization call, two original EXI transaction
requests, cleared channel lock, both empty-slot probe results, exact console
messages, the original SI command and three queued probes, sampling/handler state,
pending no-response completion, missing SI backend/VI profile,
missing EXI peripheral backend/console failures, initialization
evidence, missing clock and unbound dispatcher dependencies, on-disk DOL
truncation/mutation rejection, loaded-code integrity, block/call-depth bounds,
the original nonconverging `OSGetTime` sampling loop, callback restoration and
rejection of resuming an incomplete startup as fresh. Disk fixtures are created
in a uniquely owned directory beside the build-local DOL and narrowly cleaned.
Generated bindings and the input DOL remain local build inputs.
