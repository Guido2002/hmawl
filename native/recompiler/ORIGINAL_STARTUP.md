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
Internal RTC/ROM, SRAM writes and AD16 transactions fail explicitly. No game saves exist.
DI configuration is an explicitly mapped read-only value of 1, not a DVD
command implementation. Cache operations use the
existing checked coherent-RAM implementation. Time-base reads use the real
monotonic native clock at the GameCube's 40.5 MHz rate. Original `OSReport`
and `OSRegisterVersion` run as compiled game code. The C-library `vprintf`
boundary is an original-word-verified native console service with bounded
guest strings and PowerPC GPR/overflow argument decoding. Plain text, `%s`, `%%`,
32-bit `%d`/`%i`/`%u`/`%x`/`%X` and integer width/zero padding are supported.
Widths are bounded at 4096 and total output at 64 KiB. Other conversions,
flags, missing sinks and output errors fail explicitly before a success return.
It emits the actual SDK
version string; it does not discard logging.
The loader supplies 24 MiB physical/simulated memory size, a cold PI reset code,
162 MHz bus and 486 MHz CPU clock metadata in low memory,
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

DSP hardware is explicitly mapped with 16 MiB ARAM, halted reset/mailbox
clearing and checked bidirectional RAM/ARAM DMA. DMA completion follows actual
byte copies; W1C status/masks are retained. A BIOS-free native bootstrap validates
all 128 installed payload bytes before executing the build-time-compiled original
DSP initializer. The DSP ROM loader itself is not executed; this is an explicit
native handoff contract, not DSP-ROM emulation. General DSP tasks, outgoing CPU
mail and DSP/audio interrupts remain unsupported and fail explicitly.

The bounded DSP compiler emits native C++ for the init subset, not a runtime
instruction decoder. Instruction/data-ROM load loops can be eliminated only when
the loaded accumulator is immediately unconditionally overwritten, no other
instruction observes its value and address/wrap/range guards are satisfied.
It retains loop counts and address increments without supplying invented ROM
contents. Unsupported or live-ROM-dependent code is rejected. RAM stores and
mailbox stores still execute from the original instructions. Execution is limited
to 65536 DSP instructions; busy mailbox waits fail at that bound, not with a
fabricated acknowledgement. Reading mailbox high does not consume a message;
reading low consumes its pending bit once. Unmasked pending DSP interrupts,
like SI interrupts, cannot silently pass an enabled EE.

### Native console SRAM

The CLI loads `native-console.sram` beside the input DOL, or the path supplied
with `--sram <64-byte-profile>`. On Windows only, an absent file is created
exclusively with a new English/NTSC/stereo configuration and valid additive
checksums; no Nintendo BIOS or physical console identity is supplied. An existing
file must be exactly 64 bytes and is read unchanged, never reset or overwritten.
Creation/read/flush failures are explicit errors. The runtime API's default
state has the same new profile in memory; tests do not create production files.

Original code issues command `0x20000100` and DMA-reads all 64 bytes through the
checked EXI bus. Only this full SRAM read is implemented: RTC, ROM, partial
commands and SRAM writes remain explicit errors, so game-initiated configuration
updates are not yet supported. This is console configuration, not a memory card.

`original_startup_manifest.h` is the reusable list of original address ranges.
The build generator translates these ranges to C++, never executes PPC, and
never writes extracted game code to the source tree. It splits at every branch,
direct branch target and range boundary, with at most 32 guest instructions per
compiled block. Every loop therefore crosses a dispatcher budget boundary;
the instruction count is bounded above by 32 times the block budget.
Calls also have a separate depth bound. This is static compiled dispatch, not
an instruction decoder/interpreter at runtime. The dispatcher checks original
instruction words at registration and every block entry. Copied exception-vector
bindings validate their DOL installation source at registration and the installed
physical destination on execution, so an absent or altered vector cannot run.
The original system-call vector executes compiled instructions, including HID0
DCFA, synchronization and rfi; sc saves SRR0/SRR1 and masks exception MSR state
without changing LR. Exception execution shares block/call-depth budgets.
The other supported rfi path is original real-mode RAM configuration: entry
disables IR/DR with EE clear and a physical RAM target; return requires the exact
24 MiB RAM BAT profile and a cached RAM target. BAT writes are accepted only in
that real-mode window, for the supported RAM entries/values. Original Config24MB
executes from its physical alias with normal instruction-integrity checks.
This is not an arbitrary MMU: other BAT mappings, protection regions, general
interrupt delivery and context-switching rfi remain unsupported. Native MI setup
supports only all-readable/writable permissions, interrupt acknowledgement and
the address-error interrupt mask; missing devices and other settings fail.
The recompiler's
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

Original **__OSInitSram also returns**, with the exact native profile in its SRAM
control block, successful synchronization and released EXI/SRAM locks.
Original **__OSThreadInit also returns**: default thread/context, active-thread
list, stack guard/fill, 32 empty run queues and MSR RI/FP state are initialized.
This does not implement scheduling, context switching or guest interrupt delivery.

Original **__OSInitAudioSystem now executes** its RAM backup/DSP-payload copy,
DCFlushRange and installed system-call exception vector, halted DSP reset,
ARAM configuration, both real 32-byte RAM-to-ARAM transfers and original tick wait.
At unhalt the native bootstrap validates the installed program and executes its
compiled DSP instructions: 16404 original instruction steps, including eliminated
dead-ROM reads, all 4096 DSP RAM word clears, one real program-derived mailbox
message and HALT. The original PPC initializer consumes that message, resets the
DSP a second time, restores its complete 128-byte RAM backup and **returns**.
Startup applies the original HID2 mask and executes **__OSInitMemoryProtection**:
MI setup, five handler registrations, reset registration, real-mode RAM BAT
configuration (12 writes/two rfi transitions), interrupt mask and return.
Original kernel reporting prints actual memory and arena bounds. With the
explicit cold reset code, original ClearArena clears the complete arena.
It stops inside **OSEnableInterrupts, PC `0x801B3A60`, LR `0x801B114C`**: the
disconnected-controller completion remains pending and EE cannot be enabled
without real guest interrupt delivery. EE stays disabled; no completion is
discarded. DVDInit has not run and `OSInit` has **not** returned.
The game main loop, GX renderer,
DVD startup, scheduled game threads, audio, input and saves have not run.

## Targeted regression

With testing enabled, build `hmawl_original_startup_tests` and run CTest's
`hmawl_original_startup_tests`. It uses the production library and real verified
DOL, checking the exact interrupt-enable boundary, two original external EXI transaction
requests, cleared channel lock, both empty-slot probe results, exact console
messages, the original SI command and three queued probes, sampling/handler state,
pending no-response completion, missing SI backend/VI profile,
exact original SRAM command/DMA bytes, SRAM locks/sync, default thread/context,
active/run queues, stack guard and MSR state, missing DMA backend and preserved
malformed profiles plus fresh-profile persistence/reload,
original sc/rfi return state and original DSP payload, both ARAM DMA transfers,
complete ARAM guards, compiled DSP instruction/mail counts, poisoned DSP RAM
clearing, RAM backup restoration, halted reset/mailboxes, missing DSP hardware
and execution backend plus modified DSP program rejection,
original real-mode BAT/SRR state, five MI handler slots, exact native RAM metadata,
complete poisoned arena clearing and its exclusive upper guard, integer kernel
reports and absent memory/BAT/reset-profile failures,
missing EXI peripheral backend/console failures, initialization
evidence, missing clock and unbound dispatcher dependencies, on-disk DOL
truncation/mutation rejection, loaded-code integrity, block/call-depth bounds,
the original nonconverging `OSGetTime` sampling loop, callback restoration and
rejection of resuming an incomplete startup as fresh. Disk fixtures are created
in a uniquely owned directory beside the build-local DOL and narrowly cleaned.
Generated bindings and the input DOL remain local build inputs.
