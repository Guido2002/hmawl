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
build\native\native\recompiler\Release\hmawl_original_startup.exe --original-startup build\game-us-r0\main.dol --rvz "Harvest Moon - A Wonderful Life (USA).rvz"
```

Optional positive decimal limits are `--max-blocks 1000000` and
`--max-depth 128`. The runner never reports a successful game boot. Exit codes:
`1` input/configuration error, `2` invalid invocation, `3` unsupported dependency
or integrity error, `4` exhausted execution limit, `5` unexpected entry return.
Both normal progress and exact stopping diagnostics are printed; PC, LR, SP and
R3–R5 are preserved in the reusable result.
`--rvz <path>` binds your verified USA revision 0 disc. Without it, DI hardware
and disc transfers are absent and startup fails explicitly inside DVDInit.

## What actually runs

The build-time generator and runtime independently verify DOL SHA-1
`1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`. The loader copies original DOL
sections into 24 MiB of coherent guest RAM, with physical/cached/uncached aliases.
Unspecified RAM starts zero; this is a documented native loader profile, not
emulation of IPL/apploader execution or a dump of physical-console power-on RAM.
No connected EXI identity is invented. With `--rvz`, all four executable hashes
are verified before the actual disc ID, BI2 and FST are installed. BI2 starts at
`0x81700000`; FST begins at `0x81702000`, above the original entry's adjusted
BI2 argument-list arena limit, so ClearArena cannot erase it. This is an explicit
native loader layout, not an apploader emulation.

The loader handoff has IR/DR set (`MSR=0x30`), external interrupts disabled and
idle zero PI/MI/DSP/AI/EXI register state explicitly enabled. EXI idle transfer
controls, status W1C, sticky ROMDIS, one-hot chip selection and immediate
transactions are enabled. External memory-card slots and Serial Port 1 are
explicitly empty: the backend consumes writes and returns zero for absent
external devices, rather than claiming a connected peripheral identity.
Internal RTC/ROM, SRAM writes and AD16 transactions fail explicitly. No game saves exist.
DI configuration is an explicitly mapped read-only value of 1. The RVZ profile
also enables DI command/DMA registers, status/mask/W1C and cover control.
Disc-to-RAM DMA validates alignment, lengths and bounds before calling its
backend. Only a complete backend result commits bytes, transfer status and IRQ.
Reads and disc-ID commands return real RVZ bytes. Inquiry reports the explicit
native virtual drive: revision 1, zero console-firmware/device/date fields.
It does not extract or fabricate physical-console firmware. Unsupported commands
and missing/failed backends fail explicitly without successful completion.
Original SDK low-level queue/interrupt handling is not replaced.
Cache operations use the
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
remain pending while EE is disabled. With explicit external-interrupt support,
unmasked SI completion enters the original installed vector, dispatcher and
controller callback chain. Without that profile it still fails explicitly.
PI cause reflects device interrupt flags independently of the PI mask.
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
The external vector separately verifies unpatched DOL source words at registration
and exception-number-patched installed words at runtime. Its first rfi enters the
original handler; original OSLoadContext's final rfi restores the interrupted
context and abandons the handler's nested native call chain. Both exception
stages retain shared dispatch budgets. Nested exceptions and thread switches fail
explicitly. This is not an arbitrary MMU: other BAT mappings, protection regions,
non-SI/DI interrupt delivery and context-switching rfi remain unsupported. Native MI setup
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
and queuing the remaining three probes. The first transaction completes with the configured disconnected-port error;
the four original type callbacks run once interrupts are enabled below.
This is not usable connected-controller input.

Original **__OSInitSram also returns**, with the exact native profile in its SRAM
control block, successful synchronization and released EXI/SRAM locks.
Original **__OSThreadInit also returns**: default thread/context, active-thread
list, stack guard/fill, 32 empty run queues and MSR RI/FP state are initialized.
This does not implement runnable-thread scheduling or context switching.

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
Original **OSEnableInterrupts now succeeds**. The four real disconnected-controller
transactions drain through the original external exception vector/dispatcher,
SI handlers, type callbacks and OSLoadContext, with scheduler-disable/enable
balance and complete interrupted CPU-context preservation. EE is enabled again
after every exception return. With the verified RVZ, original **DVDInit returns**.
The original inquiry command queues, performs a checked 32-byte DMA, arms its
actual timeout alarm and completes through the original DI interrupt and
InquiryCallback. DEC retains a deadline derived from the native time base;
an expired timer fails explicitly because timer-exception delivery is not yet
implemented. Original completion cancels the DVD alarm; no timeout is ignored.
**OSInit returns to its original caller at PC=LR `0x80003268`, SP `0x8035CF98`.**
The original post-OSInit entry continues through all **93 DOL C++ constructors**,
`__init_user` returns, and original game `main` is entered. Static build-time
traversal compiles supported direct dependencies and explicit original indirect
constructor targets; unsupported instruction paths remain unregistered, with
build diagnostics and explicit runtime failure if reached. No instruction decoder
runs at runtime. Persistent milestones observe verified compiled block entry,
without replacing original instructions or changing guest registers.
The original game-initialization call at `0x8000AD40` now executes its memory,
resource-manager and Screen setup. Original `Memory::Initialize` constructs heaps
from the real arena (`0x8035EFA0` to `0x81701FE0`). Nine original allocations,
including a nested `0x1730` resource allocation and `0x60000` FIFO buffer, complete.
The original Screen enters GXInit and prints the SDK's GX version.
The runner's next dependency stop is `PC=0x801B3E00`, LR `0x801B3EEC`,
SP `0x8035CEC0`, attempting PI mask `0x8FC` for graphics interrupts.
The original inquiry callback result remains `0x8001`, five external exceptions
returned and the entire real FST is intact. GXInit has not returned.
Real-disc tests verify every constructor in original table order and preservation
of its stack, SDA and nonvolatile integer/floating registers. Traced and untraced
startup produce identical RAM, CPU state, output and sampled time; damaged
constructor words fail integrity validation without claiming later milestones.
The game main loop, GX renderer,
scheduled game threads, audio, input and saves have not run.

The next connected milestone is checked CP/PE interrupt and GX FIFO handling,
followed by original GX/VI initialization. Enabling PI bits without the actual
graphics backend would hide missing dependencies and is deliberately rejected.
Reaching GXInit does not establish a completed graphics initialization, scheduled
game thread or frame. Generated translation registration uses non-inlined batches
of at most 64 regular bindings (plus the console binding in the final batch), avoiding
MSVC's oversized temporary stack frames without increasing the executable stack.

## Targeted regression

With testing enabled, build `hmawl_original_startup_tests` and run CTest's
`hmawl_original_startup_tests`. It uses the production library and real verified
DOL, checking the missing-DI boundary without a disc, two original external EXI transaction
requests, cleared channel lock, both empty-slot probe results, exact console
messages, all four original queued SI commands and no-response callbacks,
sampling/handler state, actual exception delivery/return counts, all interrupted
CPU registers versus uninterrupted execution, damaged vector/template rejection,
missing interrupt/SI backend/VI profile,
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

With `HMAWL_US_REL_DIRECTORY` and `HMAWL_US_RVZ` also bound, the separate
`hmawl_original_disc_startup_tests` case verifies all 93 constructor returns,
the user-init return and the exact graphics-initialization dependency boundary. Its
observer checks each original table target and per-constructor ABI restoration;
a second execution compares the complete 24 MiB RAM image, CPU registers,
console output and deterministic clock samples against the production runner.
It also verifies original heap descriptors, nine allocation return addresses,
requested sizes, 32-byte alignment, arena bounds, non-overlap, allocation count,
manager singleton pointers and the Screen's FIFO pointer.
The synthetic compiled-code suite additionally covers immediate single-precision
loads/stores and update forms: signed zero, normal/subnormal values, infinities,
NaN payloads, paired-lane behavior, truncating stores, four-byte write guards,
RAM aliases, absolute addressing and transactional invalid accesses. These
transfers do not add floating-point arithmetic or general paired-single stores.
