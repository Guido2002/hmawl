# Harvest Moon: A Wonderful Life
A WIP disassembly/decompilation of Harvest Moon: A Wonderful Life (US version only) for Nintendo GameCube.
It builds the following DOL:

- main.dol: ``sha1: 1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374``

And the folowing rels:

- wlp0.rel: ``sha1: cf82697ee0d50ce11602b2b531e844f3d68c7318``
- wlp1.rel: ``sha1: 509f77d903c89472be5590d45ddf65ddb2e93055``
- wlp2.rel: ``sha1: a160e6651fd50af8471a487b9a187044f683427c``

Unfortunately, the disassembled rels, when built, do not 100% match the original game's rels at the moment due to unknown issues, but main.dol and at least wlp1 and wlp2 ***are shiftable***, meaning you can edit the disassemblies to add and remove code as you see fit.

# Building
1. Download DEVKITPROPPC, python3, and gcc.
2. Download GC_WII_COMPILERS.zip and extract it to tools/mwcc_compiler/.
3. Run the make command.

# Contributions
Contributions and PRs are more than welcome!

# Native recompilation status
The native port is intended to be a **static recompile**: translate the US
GameCube build's PowerPC game code to native Windows machine code, then run it
with Windows implementations of the GameCube platform services. The Windows
program must load game data supplied from the user's own disc image; it must
not start Dolphin or contain the game's assets.

That pipeline is **not implemented yet**. This repository currently contains a
partial GameCube decompilation, and its Makefile builds GameCube DOL/REL files,
not native game code. A new experimental tool can read code ranges from an
DOL text section and translate a small subset of integer PowerPC instructions
to C++. It can also extract and verify the original US revision 0 executables
from a GameCube RVZ. It cannot yet discover all game functions, translate the
full instruction set, or run the game. No playable native Windows version is
available yet.

The recompiler can inspect the disc ID and revision from the RVZ header without
extracting or modifying the image:

```powershell
build/native/native/recompiler/Release/hmawl_recompiler.exe --inspect-rvz "Harvest Moon - A Wonderful Life (USA).rvz"
```

Extract the original DOL, file-system table, and three REL modules into a
**new** local output directory:

```powershell
build\native\native\recompiler\Release\hmawl_recompiler.exe --extract-rvz "Harvest Moon - A Wonderful Life (USA).rvz" build\game-us-r0
```

The importer requires disc ID `GYWEE9`, disc 0, revision 0. It verifies the RVZ
header hashes, decompresses only the required groups, decodes RVZ literal/PRNG
packing, validates the DOL/FST structure, and checks all four executable SHA-1
hashes listed above **before writing anything**. Existing output directories
are refused to avoid overwriting local files. An I/O failure is reported and
may leave a partial output directory; choose a new directory for a retry.
The source image is opened read-only. Keep extracted files in the ignored
`build/` tree; do not commit or redistribute game code or assets.

Supported input is GameCube RVZ using NONE or Zstandard compression; Wii,
other compression methods, ISO/GCM import, and bulk resource extraction are
not implemented. Parsing has explicit size limits (64 MiB per executable/FST
and decompressed metadata table, and chunks up to 64 MiB).

Individual disc resources can now be listed and read without extracting the
whole disc. The executable importer and resource reader share the validated
FST parser, including directory hierarchy, duplicate-path and disc-range checks.
Disc paths are exact, case-sensitive FST paths with `/` separators; they are
not Windows filesystem paths and are not normalized.

```powershell
build\native\native\recompiler\Release\hmawl_recompiler.exe --list-rvz-files "Harvest Moon - A Wonderful Life (USA).rvz"
build\native\native\recompiler\Release\hmawl_recompiler.exe --extract-rvz-file "Harvest Moon - A Wonderful Life (USA).rvz" arowana.arc build\arowana.arc
```

Listing outputs tab-separated path, byte length and logical disc offset.
Single-file extraction refuses an existing output; parent folders must exist.
Unlike `--extract-rvz`, these generic disc-file commands do not certify the
US executable hashes. They return raw file bytes without asset decompression.

For the game's version-0 `CLZ` resources, a separate command decompresses the
selected disc file into a **new** local output file:

```powershell
build\native\native\recompiler\Release\hmawl_recompiler.exe --extract-rvz-clz "Harvest Moon - A Wonderful Life (USA).rvz" roomcommon.arc.clz build\roomcommon.arc
```

This uses a checked decoder with a 24 MiB output limit, rejecting truncated
headers/tokens, invalid block sizes/strides and out-of-window back-references
before creating output. The displayed size and SHA-1 describe the decompressed
bytes. RVZ container compression and game CLZ asset compression are separate.

The recompiler foundation has unit tests and can be built on Windows with CMake
and Visual Studio 2019 or newer. The first configure needs Git and internet
access to fetch Zstandard 1.5.7, pinned to commit
`f8745da6ff1ad1e7bab384bd1f9d742439278e99`. It is built and linked statically;
no Dolphin installation or external Zstandard executable is needed.
For an offline build, pre-provision that source tree and pass
`-DFETCHCONTENT_SOURCE_DIR_ZSTD=<local-source-directory>` to CMake.

```powershell
cmake -S . -B build/native -G "Visual Studio 16 2019" -A x64
cmake --build build/native --config Release
ctest --test-dir build/native -C Release --output-on-failure
```

The `hmawl_recompiler` tool can inspect/import an RVZ, or take an extracted
DOL, a guest code address, and a byte count, and write translated C++ for that
range:

```powershell
build\native\native\recompiler\Release\hmawl_recompiler.exe build\game-us-r0\main.dol 0x80003100 0x40 build\translated.cpp
```

The importer test target `hmawl_import_tests` uses synthetic images, including
compressed/uncompressed and zero groups, chunk-boundary reads, literal/PRNG
packing, malformed headers/tables/FST, and SHA-1 known-answer vectors. Tests do
not require a game image.

### Compiled startup tests

`hmawl_generated_tests` tests the actual generated C++ after the host compiler
has built it, rather than only inspecting emitted strings. It covers register
logical operations, shifts (including counts above 63), CR0, XER carry, CTR/XER
transfers, conditional calls and multiple-register loads.
Signed/unsigned register comparisons and `nor.` are also executed against
expected results, including nonzero CR fields and preservation of XER SO.

To also compile and test selected original startup functions from your verified
DOL, opt in locally:

```powershell
cmake -S . -B build\native -DHMAWL_US_DOL="C:\path\to\hmawl\build\game-us-r0\main.dol"
cmake --build build\native --config Release
ctest --test-dir build\native -C Release --output-on-failure
```

This adds `hmawl_us_startup_tests`. Generation and execution both verify the
US revision 0 DOL hash. Generated game-derived code stays in the ignored build
tree and is never committed. Leave `HMAWL_US_DOL` empty to build only synthetic
tests without game data.

The optional tests load DOL sections into a zero-initialized 24 MiB RAM model
with physical/cached/uncached aliases. They execute the original entry point's
first call and register initialization, pad/reset check, SDA flag helpers,
byte-copy routine, and argument-pointer relocation. The reset service is a
test callback, not an implemented runtime service.

The original ROM/BSS initializer and its original `memcpy`/`memset`
dependencies are now compiled too. Tests poison the three BSS ranges and
verify that initialization clears them, preserves adjacent guard bytes,
leaves loaded text intact, and restores the stack/callee-saved registers.
Memory-function tests cover empty buffers, four alignments, boundary lengths,
fill-byte truncation and both overlapping copy directions.
The verified DOL's ROM-copy records already have identical source and
destination addresses, so they do not require a copy or cache flush.
A modified test record exercises copying and verifies an explicit failure at
the still-unimplemented cache synchronization service; it is not a no-op stub.
The memory initializer is tested **independently** of the hardware setup.

The runtime now stores all 32 floating-point registers as raw 64-bit primary
and paired lanes, plus FPSCR state. Compiled `lfd`/`stfd` instructions support
immediate, indexed and base-update forms, with big-endian bytes, word alignment
and checked RAM ranges. Register moves and sign operations
(`fmr`, `fneg`, `fabs`, `fnabs`, including record forms) preserve NaN payloads,
signed zero and the independent paired lane; record forms copy FPSCR's summary
field into CR1, not the integer comparison flags. No host floating-point
conversion or arithmetic is used.

Independent original-code tests execute the scalar register-clear block from
`__OSFPRInit` (`0x801b0bcc`) and the SDK's f14..f31 save/restore helpers.
They check exact bits, untouched paired lanes, flags and whole-RAM guards.
The scalar block now executes its original `mtfsf` at `0x801b0c4c`, clears
FPSCR and returns. Compiled `mtfsf`, `mtfsfi` and `mffs` support masked status
writes, immediate fields, derived invalid/enabled-exception summaries, boxed
FPSCR reads and optional CR1 updates. Tests execute every field mask, every
immediate field/value and both record forms, including zero masks, exception
cause/enable combinations, sticky FX preservation and all rounding-mode bits.
The original `PPCMffpscr`/`PPCMtfpscr` SDK wrappers run with checked stack
boundaries and preserved f31/paired lanes.

This is status-register support only: no host rounding environment is changed,
and floating-point arithmetic or MSR-dependent program-exception delivery is
not implemented. Privileged setup and paired-single initialization are not
skipped in a claimed full startup. DVD callback context restoration includes
both FPR lanes and FPSCR, including cancellation and throwing callbacks.

Paired-single register moves/sign operations and all four lane merges now
compile too, including record forms and overlapping source/destination registers.
Immediate `psq_l`/`psq_lu` and indexed `psq_lx`/`psq_lux` support unquantized
single-precision data and unsigned/signed 8-bit and 16-bit integer data, W=0 two-value loads and W=1
loads with a 1.0 second lane. Single values
are widened using integer bit operations, preserving signed zero, subnormals,
infinities and signaling/quiet NaN payloads without host arithmetic. All eight
GQRs have compiled SPR read/write support. Integer loads apply the signed
six-bit GQR scale exactly using integer bit operations. Tests exhaust every
8/16-bit input for all 64 scales and all four integer types (8,421,376
input/type/scale combinations), then exercise generated loads through all
GQR selectors, W/update modes, RAM aliases and byte/halfword boundary cases.
Both addressing forms share the same checked load/conversion path. Indexed
tests additionally cover RA=0, RB=0, overlapping base/index registers,
32-bit address wrapping, preserved index registers and no partial base/FPR
updates after failed reads. W and GQR selector bits are decoded separately
from the indexed subopcode; invalid record/update forms are rejected.
Reserved load types 1..3, paired stores and paired arithmetic remain
explicitly unsupported.

The original GQR-reset block (`0x801b15a4`) and FPR initialization body
(`0x801b0b44`) now run independently in the startup tests: the original paired
load, 31 paired register copies, scalar clear and FPSCR reset leave all 64
lanes and FPSCR zero, with no RAM writes. DVD callbacks also restore GQR state.
The full original `__OSFPRInit` (`0x801b0b2c`) also runs independently, including
its MSR FP-enable prefix and HID2 branch: scalar-only mode preserves secondary
lanes, while paired mode clears both lanes. Compiled `mfmsr`/`mtmsr` share the
existing native interrupt mask as the sole EE source; FP is retained as guest
status. HID2 SPR transfers support PSE/LSQE changes. All other MSR/HID2 bit
changes fail before mutation, rather than silently claiming support for MMU,
privilege, exception or DMA modes. Floating-point availability exceptions and
HID2 instruction gating are not modeled; translated instructions remain usable
in isolated fixtures.

The complete original `__OSPSInit` now runs, including the HID2 SDK wrappers,
`ICFlashInvalidate`, `sync`, all eight GQR resets and the original stack/return
epilogue. HID0 SPR transfers support cache enable bits ICE/DCE, synchronous,
self-clearing instruction-cache invalidation ICFI and the speculation-disable
status bit SPD. Other changed HID0 bits
(including cache locking and data-cache flash invalidation) fail before
mutation. `sync` and `isync` issue host memory fences; guest RAM aliases already
share coherent storage, and static translations still undergo code-word checks
at dispatch. This is not emulation of cache timing, speculative execution or
self-modifying code. Tests confirm invalidation retains valid bindings and
does not hide modified-code failures. Original HID0 wrappers, `ICEnable` and
`DCEnable` also run with RAM/stack/flag guards.

The complete original `__init_hardware` (`0x80003400`) now returns, including
`__OSCacheInit`, original L2 invalidation loops, original `DBPrintf` (which only
saves arguments in this build), and original DMA error-handler registration.
Tests cover all 16 initial L1/L2/interrupt combinations, the already-enabled L2
path, masked invalidation, restored MSR/ABI, original diagnostic call ordering
and RAM guards. L2CR supports L2E/L2I changes with synchronous invalidation over
coherent RAM and L2IP clear; other configuration changes and invalidation while
enabled fail explicitly. This does not emulate cache timing or deliver DMA
exceptions. The installed DMA handler is not executed, and the floating-point
exception-handler path of `OSSetErrorHandler` is not certified by these tests.
DVD callbacks restore L2CR/HID0/MSR/HID2 along with floating-point state after
success, cancellation and exceptions.

These tests **do not boot
the game**: integrated original entry-point execution now runs register,
hardware, ROM/BSS and argument initialization followed by original `DBInit`,
then stops at `0x80003264` before `OSInit`. The fixture assumes loader-enabled
MSR IR/DR bits and zero-initialized RAM; it does not implement an MMU. BSS
poisoning excludes the two zero constants that hardware reads before BSS clear.
Tests check BSS, stack linkage, DB interface/verbose globals and the physical
exception destination installed by `DBInit`. OS services, floating-point
arithmetic/rounding conversions, MSR-dependent exception handling and full paired-single support,
GX, audio and dynamic REL runtime integration are still missing. Unmapped addresses,
including MMIO other than explicitly enabled register support below, throw
rather than being silently treated as RAM.

Continuing that verified state, an independent original `OSInit` prefix now
sets `AreWeInitialized`, calls the original `__OSGetSystemTime`, records
`__OSStartTime`, masks interrupts and calls the six original MMCR0/1 and PMC1..4
reset wrappers, then executes original `PPCDisableSpeculation` and
`PPCSetFpNonIEEEMode` to set HID0 SPD and FPSCR NI. The original boot-info
and arena setup then runs through `OSSetArenaLo`/`OSSetArenaHi`, followed by
original `OSExceptionInit`, `__OSInitSystemCall`, `OSInitAlarm` and
`__OSModuleInit`. It stops at `0x801b0f6c`, before `__OSInterruptInit`;
first-time `OSInit` does not yet return. Its
already-initialized branch and return epilogue are tested separately, not used
to skip the unfinished first-time path.

Original `OSGetTime` uses the existing native time-base callback and retries a
torn upper/lower sample; `__OSGetSystemTime` adds the low-memory 64-bit offset
under the original interrupt mask. Tests cover rollover retry, carry/full-width
wrap, offset combinations, both prior interrupt states and stack/ABI guards.
New compiled `addc`/`adde` reuse checked carry-state logic, including record
forms, overlapping registers and preservation of other XER bits; overflow-enable
forms remain unsupported. No performance counters, OS scheduler or RTC backend
are implied by these time tests.

Performance-monitor SPR writes currently support zero resets only, with
explicit guest register state preserved across native DVD callbacks.
Nonzero writes (including enabling measurement), reads and record forms are
rejected; hardware event counting is not implemented. Tests cover each register,
every nonzero single-bit input, repeated resets, interrupt/state preservation,
original reset-call order and the unchanged already-initialized skip path.

Compiled `mtfsb0`/`mtfsb1` now clear/set individual FPSCR bits, recompute VX/FEX
and optionally update CR1. Setting a previously clear exception-cause bit also
sets sticky FX; setting an already-set cause does not recreate a cleared FX.
Tests cover all 32 bit selectors, both operations/record forms, repeated writes,
exception summaries, register/RAM preservation and unchanged host rounding.
The original `PPCSetFpNonIEEEMode` wrapper (`0x801b0b24`) independently sets NI
and returns without disturbing rounding bits or FPR lanes. This is guest status
support, not implementation of NI arithmetic or completion of `OSInit`.
Original `PPCDisableSpeculation` (`0x801b0afc`) also runs independently, using
the original HID0 getter/setter. Tests cover both initial SPD states, all
cache-enable combinations, repeated calls, preserved loader bits, both interrupt
states and stack/ABI guards. SPD is retained guest control state, not emulation
of a speculative processor pipeline.

Boot-info tests cover BI2 metadata, the low-memory fallback and absent metadata,
including full-word versus byte-sized debug flags, the debug threshold at 2,
PAD specification propagation and the DVD long-filename flag. Arena tests cover
loader-supplied bounds, default bounds (`0x8035efa0`/`0x81700000`) and the
debug-dependent low bound (`0x8035cfa0`). Original arena getters/setters retain
exact guest values, including zero and unaligned inputs; they do not allocate
host memory or validate an allocation range. Tests check exact RAM changes,
setter order, register/stack guards and the unchanged already-initialized path.
The integrated entry continuation exercises the absent-metadata/default-bound
path; a complete loader boot-info contract still remains.

Original `OSExceptionInit` installs the debug integrator when absent, copies the
15 exception-vector templates where not owned by the debugger, and initializes
the default-handler table. Tests compare exact RAM contents across debug modes,
marked-exception masks and an existing integrator, including temporary template
patches/restoration, original cache-call ranges and preserved stack/registers.
The installed vector code and default handlers are not executed: exception
delivery, privileged exception-return instructions and system calls remain
unsupported.

Original `__OSInitSystemCall` replaces the first `0x1c` bytes at `0x80000c00`
with the original system-call vector and synchronizes the `0x100`-byte range
using the original cache routines. Tests verify exact copied bytes, untouched
trailing data, call order/range arguments, repeated installation, both interrupt
states and stack/register preservation. The entry-derived OSInit test checks
this replacement after exception installation. Copying the vector does not
enable system-call execution: `sc` and `rfi` remain explicitly rejected.

Original `OSInitAlarm` initializes the alarm queue, installs the decrementer
handler through original exception-table accessors, and registers its original
reset node. Tests verify exact RAM changes, an existing reset-list entry,
both interrupt states and idempotent re-entry without losing queued alarms.
Original `OSRegisterResetFunction` is separately tested for empty/head/middle/
tail insertion, stable equal priorities and unsigned maximum priority.
This installs data and handler pointers only: alarm scheduling, decrementer
interrupt delivery and execution of reset callbacks are not yet implemented.

Original `__OSModuleInit` clears exactly the three low-memory module words at
`0x800030c8`, `0x800030cc` and `0x800030d0`. Tests cover zero/poisoned values,
repeated initialization, both interrupt states and exact CPU/RAM preservation,
including untouched module payload data. The entry-derived OSInit continuation
also clears poisoned module metadata. This does not link/unlink RELs, register
native translations, or connect the SDK module list to the existing host linker.

Original `__OSInterruptInit` now runs independently and from the saved
entry-derived OSInit state through its RAM setup: it installs the handler-table
pointer, clears all 32 handler slots and resets the two software mask words.
Without optional PI support it explicitly rejects the interrupt-mask write at `0x801b3b08`
(`0xcc003004`); its stack frame remains active, so this is not a completed call.
Tests verify exact RAM/stack writes, poisoned tables, both interrupt states and
repeated rejection without RAM changes. Original interrupt-handler accessors
are tested across all 32 slots, cached/uncached table aliases, null handlers,
previous-handler returns and signed low-halfword index behavior. These SDK
accessors do not validate indices; native memory bounds checks still apply.
General device interrupt masks, interrupt dispatch and hardware delivery remain
unimplemented. The completed OSInit prefix still ends at `0x801b0f6c`.

Optional PI mask storage now accepts the original bootstrap write `0xf0` and
32-bit readback at exactly `0xcc003004`. It is disabled by default; other values,
widths, unaligned/neighboring addresses and unsupported aliases remain errors.
It retains device state rather than restoring it with DVD callback CPU context,
including after cancellation or a throwing callback. It does not implement
interrupt causes, event sources or delivery, and is not a general PI model.
With this enabled, original `__OSMaskInterrupts`/`SetInterruptMask` execute through
the software-mask update and then reject the MI mask write at `0x801b3be0`
(`0xcc00401c`). Tests check exact RAM/stack changes independently and from the
entry-derived state. New compiled `andc` supports this path, with record/SO,
source/destination alias and XER-preservation tests.

Optional MI mask storage supports the original 16-bit zero reset and readback
at exactly `0xcc00401c`. It is disabled by default, rejects nonzero writes
before mutation and does not enable interrupt sources or delivery. Other access
widths, neighboring/unaligned addresses and physical aliases remain unmapped.
Like PI state, MI device writes persist through DVD completion, cancellation and
throwing callbacks rather than being restored with CPU context.
With both options enabled, original interrupt initialization clears MI mask
state and reaches the next unsupported DSP control read at `0x801b3bfc`
(`0xcc00500a`). Tests cover zero/poisoned initial masks, both interrupt states,
exact RAM/stack writes and continuation from the saved entry-derived state.
The interrupt call has not returned; the completed OSInit prefix is unchanged.

Optional DSP control state supports 16-bit readback at `0xcc00500a` and the
original startup's interrupt-disable write. It clears only the three enable
bits (`0x150`); pending status (`0xa8`) is retained because the original code
writes zero to those write-one-to-clear fields. Halt, DMA status and init-code
status are preserved. Reset/assert/init commands, reserved state, acknowledgements,
enabling interrupts and other control changes remain explicitly unsupported.
The option is disabled by default and rejects other widths/addresses/aliases.
Tests cover all 512 combinations of the supported initial register bits, repeats,
halfword truncation, rejected changes and persistence through DVD callbacks.
With PI/MI/DSP state enabled, original interrupt setup now reaches the unsupported
AI control read at `0x801b3c4c` (`0xcc006c00`), independently and from entry-derived
state with exact RAM/stack guards. This is not DSP execution, DMA, audio output
or interrupt delivery; interrupt initialization and OSInit still do not return.

Optional AI control state supports 32-bit readback at `0xcc006c00` and the
original idle-startup interrupt-disable write. It clears the enable bit (`0x4`)
while preserving pending write-one-to-clear status (`0x8`), interrupt validity
and frequency selections. Active playback, sample-counter reset, acknowledgements,
reserved state and other control changes remain explicitly unsupported.
It is disabled by default; other widths, neighboring/unaligned addresses and
physical aliases remain errors. All 32 supported initial combinations, repeated
writes, rejected changes and DVD callback device-state persistence are tested.
Original interrupt setup now reaches the unsupported EXI channel 0 control read
at `0x801b3c7c` (`0xcc006800`), independently and from entry-derived state with
exact RAM/stack guards. No audio playback, sample-counter progression or interrupt
delivery is implemented; the completed OSInit prefix remains unchanged.

Optional EXI status state covers the three 32-bit channel parameter registers
at `0xcc006800`, `0xcc006814` and `0xcc006828`, independently enabled.
Only idle startup interrupt-disable writes are supported: pending write-one-to-clear
flags, channel 0/1 device presence and clock bits are retained. Chip selection,
ROM-disable state, enabling interrupts, acknowledgements, reserved bits, transfers
and other register accesses remain explicitly unsupported. Tests exhaust all
1,024 supported initial combinations for each of channels 0/1 and 128 for channel 2,
including repeated writes, access rejection and DVD callback persistence.
With all startup device registers enabled, original `__OSInterruptInit` now
completes: it clears the software masks/table, disables device interrupt enables,
installs `ExternalInterruptHandler` and restores interrupts, stack and callee-saved
registers. Exact RAM/stack writes and partial opt-in boundaries are tested.
Entry-derived execution returns to `0x801b0f70`; the existing OSInit prefix fixture
still ends before this call; a separate continuation is described below. This does
not implement EXI devices, memory-card transfers or hardware interrupt delivery.

A separate original OSInit continuation fixture now executes `0x801b0f6c` through
`0x801b0f80` from the saved entry-derived state with startup device support enabled.
It completes interrupt initialization, registers the original reset-switch handler
in slot 22 and executes original `__OSContextInit`. Context initialization installs
the FPU exception handler, clears the FPU-owner pointer at `0x800000d8` and runs
original `DBPrintf` (argument saves only, no host output). Standalone tests cover
cached/uncached exception-table aliases, poisoned/zero owner state, repeats, both
interrupt states, exact RAM/stack writes and CPU/FPR preservation. Continuation
now also executes the warm `__OSCacheInit` call and stops at `0x801b0f88` before
`EXIInit`; OSInit has not returned. The earlier
prefix remains separate for default-unmapped and re-entry tests. No context
switching, scheduling or FPU exception delivery is implied.

The OSInit cache call preserves the enabled L1/L2 caches established by hardware
startup, including HID0 SPD and floating-point state, and re-registers the original
DMA error handler. Tests check the exact dependency sequence, previous-handler
return, repeated calls, both interrupt states, callee-saved registers and exact
RAM/stack writes. The existing full hardware-startup tests continue to cover cold
cache enable/invalidation paths. This uses coherent native RAM and controlled
guest cache state; it does not implement physical cache timing or DMA errors.

The original `EXIInit` prefix now reads independently opt-in 32-bit transfer
control state at `0xcc00680c`, `0xcc006820` and `0xcc006834`. Only inactive
state is readable; busy or reserved state throws explicitly rather than
entering an unbounded original polling loop. All writes, unsupported widths,
neighboring/unaligned addresses and physical aliases remain errors. Tests cover
all 32 inactive control combinations per channel, repeated reads and rejection
without mutation. Original startup checks all three channels and calls
`__OSMaskInterrupts` with `0x007f8000`; the already-masked software state avoids
new device writes. Independent and entry-derived tests verify exact RAM/stack
writes, CPU/device preservation and partial opt-in boundaries. The compiled
prefix ends at `0x801f74a4`, before EXI status reset; EXIInit has not returned.
No transfer completion, device probing or EXI data movement is implemented.

Compiled `dcbf`/`icbi` check the addressed 32-byte RAM block and issue a host
memory fence over coherent storage. They retain static translation bindings
and do not suppress dispatch code-word checks. Original `DCFlushRangeNoSync`
and `ICInvalidateRange` now run their own alignment/iteration logic; tests cover
aliases, wrapping effective addresses, range alignment, zero lengths, unmapped
rejection and modified-code detection. This is not a CPU cache model or dynamic
translation of the newly installed vectors.

### REL module parsing and linking

Inspect a locally extracted version 3 module without changing it:

```powershell
build\native\native\recompiler\Release\hmawl_recompiler.exe --inspect-rel build\game-us-r0\wlp0.rel
```

The host-side linker loads REL sections and zero-initialized, aligned BSS into
the RAM model. It resolves self imports, cross-module imports, and module 0
references (absolute DOL addresses). Supported fixups are `ADDR32`,
`ADDR16_LO`, `ADDR16_HI`, `ADDR16_HA`, and `REL24`, plus SECTION/NOP/END/NONE
stream records. These cover all relocation types in the verified US modules.
Unsupported versions/types, missing modules, invalid sections/streams,
misalignment, overlapping load ranges and out-of-range branches are errors.
Fixups are staged before guest memory is modified, so a failed link does not
leave partially loaded modules. Callers must supply reserved memory ranges
for the DOL, stack and other allocations.

`hmawl_rel_tests` runs synthetic linker tests without game files. To also
validate the three original modules, configure:

```powershell
cmake -S . -B build\native -DHMAWL_US_REL_DIRECTORY="C:\path\to\hmawl\build\game-us-r0"
cmake --build build\native --config Release
ctest --test-dir build\native -C Release --output-on-failure
```

This adds `hmawl_us_rel_tests`; it verifies all four executable hashes, links
all three RELs at explicit **test-only** addresses, and checks all 32,637
resulting fixups, module BSS and unchanged DOL text.
The linker reports section addresses and prolog/epilog/unresolved entrypoints
but does not execute them, update the original SDK's module linked list, or
implement the SDK's `OSLink`/`OSUnlink` ABI. Dynamic allocation, unloading,
deferred unresolved imports and automatic discovery/binding of all game
functions are still missing. Linking PowerPC bytes in RAM is preparation
for static recompilation, not execution through an interpreter or a game boot.

### Native function dispatch

The native dispatcher maps exact guest entry addresses to precompiled C++
functions. It connects generated direct/indirect calls, tail branches and
returns through the existing guest-call callback. Registration is atomic and
module-scoped. REL bindings additionally identify an executable section and
offset, and must match their compiled load address; arbitrary rebasing is
not implemented. Registration and each function entry compare the relocated
guest words with the translation's expected code, rejecting stale or changed
code instead of executing a mismatched native function. This is a code-identity
guard, not an instruction interpreter or a complete self-modifying-code model.

`hmawl_generated_tests` tests compiled direct/indirect calls across module
registrations, tail calls, return addresses, missing/duplicate bindings,
unregistration/re-registration, callback restoration and explicit function-entry
and call-depth limits. These limits do not interrupt a loop wholly inside one
compiled function. Registration/removal during execution and reentrant use of
the same dispatcher are rejected; the dispatcher is single-threaded.
Unregistration removes native bindings only, not guest memory or SDK module state.

With `HMAWL_US_REL_DIRECTORY` enabled, `hmawl_us_rel_native_tests` also generates,
compiles and executes **two small original wlp0 functions**: a zero-test of an
object field and a field getter. Generation and execution both verify the DOL
and all three REL hashes, then link at the same test-only addresses. Tests
check imported DOL data references, results, return/ABI state, invalid placement,
changed code and binding removal. Game-derived generated C++ remains in the
ignored build tree.

The same optional suite now compiles all **13 original wlp0 static constructors**
(6,428 PowerPC instructions), the module's prolog/epilog, destructor-chain
helpers and required original DOL register-save/restore and object-construction
functions. Each constructor runs independently with nonvolatile-register and
stack/SDA checks. Tests check selected complete member-function-pointer table
copies, the constructed global object and its registered destructor, and that
all writes stay inside the module's data/BSS or the test stack.

The original prolog calls all 13 constructors through its relocated array,
then reaches its scene-selection code. Tests cover its default, mode-8 and
mode-9 selection paths and verify an explicit failure at the **missing game
allocator `0x80178E88`**, with the original allocation size/source-line
arguments and no assigned scene object. The constructed data/BSS must match
the standalone constructor chain. No allocator success or scene construction
is simulated. With a fresh test call frame after that expected failure, the
original epilog invokes the registered destructor and empties its chain;
calling it again exercises the empty-chain case without a second destruction.
`extsh`/`extsh.` are compiled and tested for the destructor's signed flag.

An additional binding group now translates the original
`Memory::InitializeWithArena`, heap-descriptor constructors, allocation
virtual methods, scalar `new` and scalar `delete`; it does not substitute a
host allocator or return fabricated allocations. The local test supplies a
64 KiB arena at an explicit test address and constructs the original 128/4096
descriptor arrays. Tests cover 32-byte alignment, zero-size requests, splitting,
exact fit, allocation/high-water counters, allocation timestamps, descriptor
exhaustion, out-of-memory arguments, null deletion, neighboring-block
coalescing and reuse. Guest allocator quirks remain unchanged; these tests
do not certify every allocation/free ordering or array allocation.
Out-of-memory stops at the unimplemented `OSPanic` binding, not a
success-shaped null fallback.

The original `OSGetTick` is translated using `mftb`. A native monotonic
time-base source uses `std::chrono::steady_clock` at the GameCube's 40.5 MHz
rate, with a per-source epoch. Low/high time-base reads, conversion,
truncation, long-duration overflow avoidance, missing-source failure and
host-clock advancement are tested; allocator expectations use a deterministic
test clock. This does not implement wall-clock time, alarms, scheduling or
timing synchronization with rendering/audio. `crxor`/`crclr` and `subfic`
translation are also covered by compiled semantics tests.

With only the allocator/time-base bindings installed, the original module
prolog allocates the selected scene object and stops at the unregistered
default, mode-8 or mode-9 scene constructor.

Additional, opt-in scene bindings now translate the integer prefixes of all
three constructors, stopping before their first floating-point instructions.
The original shared base constructor executes with checked object fields,
return ABI and write boundaries. The default path executes two original
calendar getters using compiled `divwu`/`mullw`, then stops at `0x8000FEE8`.
Mode 8 constructs its first nested object's intrusive lists and adjusted
vtables, then stops at the unimplemented registration routine `0x8017CD5C`.
Mode 9 clears its buffers through the original `memset`, constructs its first
nested DOL object and stops at the next REL constructor `0x8101DDB0`.
Tests check allocation counts, arguments and whole-RAM write boundaries.

A further opt-in dependency group now executes the original registration
manager constructor, including its bitset initialization and singleton
publication. Its original priority-sorted registration/list code is tested with
empty and populated lists, duplicate priorities, unsigned extremes, earlier
reprioritization and moves between managers. The calendar period lookup is
compiled and checked at bucket boundaries across its full 32-bit input range
and at saturation. This preserves the original routines, not replacement
host containers; arbitrary in-place reprioritization is not certified.

With these dependencies installed, the default scene reaches `0x8001BD04`.
Mode 8 registers its first nested object with the original manager and
reaches constructor `0x8100DCD8`. Mode 9 enters `0x8101DDB0`, executes an
original virtual archive initializer for the empty-resource case, constructs
two nested self-linked lists and reaches game-object constructor `0x8002283C`.
Without the separate archive bindings, nonempty archive initialization still
stops explicitly at `ARCInitHandle` (`0x80203B4C`).

The opt-in archive bindings compile the original U8 handle initializer, path
lookup, indexed file open, file pointer/length/close helpers and the game's
indexed resource getters. A synthetic nested archive tests case-insensitive
names, absolute/relative paths, dot/parent components, missing names, zero-byte
files and rejection of directories/invalid indices. Original separator behavior
is preserved: an extra separator inside a path restarts lookup from the root,
rather than applying host path normalization. Payloads and archive bytes remain
unchanged, with whole-RAM write guards around handle/output/stack buffers.
Invalid magic reaches the still-unimplemented `OSPanic`; unmapped metadata
accesses fail through the guest RAM bounds checks. This is not a hardened U8
validator: the original SDK trusts archive metadata, and the game's indexed
getters assume valid file indices.

An optional real-disc integration test now connects RVZ random access and FST
file lookup to the compiled original U8/game archive APIs. It verifies the
US disc identity, executable/FST hashes and one original resource archive hash,
then checks all five file pointers/lengths/payloads and its nested animation
archive. All input remains local; no extracted assets are added to the source.

```powershell
cmake -S . -B build\native -DHMAWL_US_REL_DIRECTORY="C:\path\to\hmawl\build\game-us-r0" -DHMAWL_US_RVZ="C:\path\to\hmawl\Harvest Moon - A Wonderful Life (USA).rvz"
build\native\native\recompiler\Release\hmawl_us_rel_native_tests.exe build\game-us-r0 --rvz "Harvest Moon - A Wonderful Life (USA).rvz"
```

`HMAWL_US_RVZ` also registers `hmawl_us_resource_tests` when the optional REL
tests are enabled. It also decompresses all eleven original `.clz` archives
using the statically compiled original decoder (`0x8017837c`, with block decoder
`0x80178450`), compares every output byte with the checked decoder and opens
each result through the original U8/game APIs. All file pointers and lengths
are verified, with ABI and memory-write guards. Synthetic tests cover empty
output, literals, overlapping references, maximum distance/length, padded
multi-block streams and invalid magic/version. The original decoder trusts
its input; malformed streams are tested only against the checked decoder.
The optional DVD bindings also compile original FST initialization,
`DVDConvertPathToEntrynum`, `DVDOpen`, `DVDChangeDir`, `DVDGetCurrentDir` and
the game's fresh `DvdFile` constructor/open path. The real-disc test opens
all 1,660 files through original `DVDOpen` and checks their disc offsets,
sizes and `DVDFileInfo` fields against the validated FST. The fixture supplies
the boot FST pointer and long-filename mode; this does not execute OS startup.
Synthetic tests preserve original case-insensitive SDK lookup, parent/current
directory handling, separator quirks, truncation and empty-file semantics.
Missing-file reporting and invalid short-name validation still reach explicit
unimplemented `OSReport`/`OSPanic` boundaries; no success stub is substituted.

`NativeDvdReader` consumes original `DVDFileInfo` offsets/sizes and reads
validated disc ranges into checked guest RAM. It allows the original
less-than-32-byte EOF padding, copying actual disc bytes rather than inventing
zeros. Invalid metadata/ranges, unmapped destinations and disc-read errors
leave guest memory unchanged. It is a synchronous host-side primitive, not a
replacement for `DVDReadPrio` or an open/close handle registry.
The `subfe`/`subfe.` carry semantics required by the game wrapper are tested
with carry/borrow boundaries, register aliases and CR/XER preservation;
overflow-enabled variants remain unsupported.

The original `DVDReadAsyncPrio` (`0x801ba9dc`) and its SDK callback forwarder
(`0x801baa9c`) are now compiled as well. Explicit host-service bindings replace
the hardware-dependent low-level read submission (`0x801bc9d8`) and queued
cancellation (`0x801bd1cc`); these are native replacements, not original code
translations. Both bindings check their original code words at registration
and entry, and fail explicitly when their `PpcState` service is not installed.

`NativeDvdQueue` defers transfer and notification until a host `Pump()` at a
cooperative guest safe point. It completes one request per pump in priority
order (SDK priorities 0..3, FIFO within a priority), writes SDK command
state/count fields, invokes compiled callbacks and restores CPU registers.
Callbacks can submit subsequent reads, but cannot recursively pump the queue.
Queued cancellation transfers no bytes, sets status 10 and invokes the read
callback with -3 before the optional cancel callback with 0. Terminal-command
cancellation notifies only the cancel callback. Duplicate/aliased command
blocks, conflicting command/buffer ranges and changed queued metadata are
rejected. Read failures deliver -1, leave payload memory unchanged and propagate
the error; callback failures propagate without replaying completed commands.
Callers must keep the queue, reader and dispatcher alive while service
callbacks or queued requests refer to them.

Real-disc tests now run original `DvdFile` construction, compiled
`DVDReadAsyncPrio`, queued RVZ reads, the original SDK callback forwarder
(with a test-only completion observer), then original CLZ/U8 initialization
for raw and compressed resources. Submission is checked to leave payloads
unchanged and callbacks uncalled until pumping. Cancellation and whole-RAM
write guards are tested too. Synthetic queue tests additionally cover priority,
FIFO, CPU restoration, requeueing, zero/null callbacks, missing bindings,
metadata changes, callback exceptions and a truncated-input I/O failure.

The original game async read entry (`0x8017b07c`), alignment helper, read
callback (`0x8017acf0`) and virtual completion handler (`0x8017b30c`) are now
compiled too. Real raw/compressed resource tests submit through this game
entry, pump the native read, then invoke the original SDK/game callbacks and
the original completion-flag observer (`0x8017b558`). They verify the game's
done flag, observer consumption, exact bytes and cancellation without a false
success flag. This observer is the game's own completion class, not a host
callback substitution. Closed-file rejection, the original DMA rounding
rules and `DVDGetCommandBlockStatus` normalization are tested separately.

Game status switches require local `bctr` jump-table targets. The recompiler
now emits one static local-target switch per affected function; external
targets still go through compiled-only dispatch. Synthetic native tests cover
conditional/fallthrough, backward, aligned and external branch targets.

Explicit, original-code-checked `OSDisableInterrupts`, `OSEnableInterrupts`
and `OSRestoreInterrupts` host bindings maintain a cooperative external
interrupt flag, including nested save/restore behavior. `Pump()` leaves
requests untouched while this flag is disabled. Read/cancel callbacks execute
with it disabled and restore the caller's mask and CPU registers even on
exceptions. This models safe-point delivery only, not privileged MSR state,
hardware interrupts, preemption or a Windows thread scheduler. The fixture
starts with delivery enabled; it has not run the original OS startup.

The original game `DvdFile::Close`, SDK `DVDClose`, synchronous cancellation
wrapper, cancel-completion callback and `OSWakeupThread` are compiled too.
Real raw/compressed resources exercise pending-read cancellation through close,
repeated close, reopen, reopen while a read is queued, and close after completion.
Tests check observer delivery, untouched canceled-read destinations, interrupt
mask restoration, terminal SDK states and explicit missing-service/unowned-command
failures. Original behavior is retained: `Close` does not clear the game's open
flag; `Open(nullptr)` clears that flag and the rounded size. No host handle
state is substituted for these original object fields.
Wakeup tests cover empty queues and suspended waiters; the DVD close fixture has
no sleeping threads. Runnable-thread selection remains an explicit untranslated
boundary, not a successful scheduler stub.

The original decode queue reset/enqueue/observer setup and worker entry
(`0x8019a6bc`) are now compiled. A synthetic fixture and a real
`roomcommon.arc.clz` test exercise both normal-heap and alternate-pool allocation: original
game DVD read into an original allocation, original worker allocation and CLZ
decode, original source-release routing, completion notification, and U8
initialization. Every decoded byte is compared with the checked decoder.
The tests verify ABI/RAM guards, completion flags, no callback replay and
complete heap coalescing after releasing the output. The optional completion
observer is test-only; the worker and resource lifetime logic are original.
The alternate pool uses its original allocator, membership lookup and release
functions with separate fixture RAM and descriptors. Four-entry synthetic
batches mix input/output pools, verify FIFO completion and first/last/no
observer behavior, and free results in a different order. Both arenas must
coalesce completely; RAM guards allow only actual output bytes and metadata
writes. Exhausting the alternate pool still reaches the original `OSPanic`
boundary without losing the input or reporting successful decoding. The
original allocation-counter change on this fatal path is preserved.

The worker's `DCFlushRange` call has an explicit original-code-checked native
binding. Guest RAM aliases already share coherent host memory; this service
validates the cache-line-rounded RAM range and issues a host memory fence.
It does not emulate the GameCube cache, syscall handler or GPU cache behavior.
Tests cover aliases, zero lengths, boundaries, overflow and MMIO rejection.

The decode fixture calls the worker only after its input is ready. It does
not start the original OS thread: starting a nonempty queue still fails
explicitly at the unimplemented thread launcher (`0x8019a668`). The original
worker busy-waits for unready entries, so it is not a cooperative scheduler.
Thread waits and overlapping DVD/decode execution are not integrated.

This remains controlled integration, not a full DVD/OS service. Pumping is
explicit and single-threaded, not interrupt or thread scheduling; cancellation
of an in-flight hardware transfer, synchronous SDK reads and full decode
queue scheduling/resource-manager integration are still missing.
These are controlled fixtures with explicitly supplied system/context globals,
not a full initialization of those game systems. Scene assignment to `g_system`
has not completed. Floating-point continuations remain unregistered;
undefined zero-divisor `divwu` fails explicitly and overflow-enabled variants
are unsupported.
Module initialization is still **incomplete**; this is not a game boot or
an implementation of `OSLink`/`OSUnlink`.

Only supported instructions are translated. The tool reports the guest address
when it reaches an unsupported instruction instead of silently generating
incorrect game code. Initial coverage includes common integer immediates and
logical operations, count-leading-zeros, rotates/masks, multiple and indexed word/halfword/byte
loads and stores, basic arithmetic, condition-register comparisons,
direct/conditional branches, link-register calls/returns, double-precision
loads/stores and floating-point register moves/sign operations. Generated output
is an experimental building block; it does not yet provide the complete game
runtime or a playable executable.

`native/windows/` contains a separate Direct3D renderer test. It draws an
invented placeholder scene to test window/device setup; it does not run the
game and is not part of the eventual port. It is also included in the Windows
CMake build. Build its target with:

```powershell
cmake --build build/native --config Release --target hmawl_renderer_prototype
```

Run `build/native/native/windows/Release/hmawl_renderer_prototype.exe`; use
arrow keys or WASD to
move its placeholder character, and Escape to quit. This is just a renderer
test. See [PORTING.md](PORTING.md) for the actual static-recompilation plan.
