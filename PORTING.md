# Native Windows Static Recompilation Roadmap

## Goal and current state

The target is an executable which **statically recompiles the original game's
PowerPC code** to native Windows code and runs without a GameCube emulator.
The user provides their own supported US game image. The image supplies the
original executable code and data at build/first-run time; copyrighted game
assets are not distributed in this repository.

The current repository is an early decompilation/recompilation project. Its
Makefile builds GameCube `main.dol` and `wlp*.rel` files. The tracked game
sources comprise 31 assembly files and just three C/C++ implementation files.
An experimental native tool now extracts verified US executables from an RVZ,
reads DOL text sections, and translates a limited integer PowerPC subset to
generated C++. It is not yet a
complete static recompiler and there is no Windows replacement for the
GameCube runtime, so neither it nor the D3D test window builds the game.

A comparable technical approach is used by
[Double Dash Redux](https://github.com/Jaster111/double-dash-redux): translate
the game's PowerPC instructions to generated C++ and provide a native Windows
runtime. Its code is only a reference for the architecture; this project does
not copy its implementation.

An optional native console runner now executes the verified original DOL entry
continuously, without test-only guest callbacks. Original EXIInit now returns
after actual immediate transactions against explicitly empty external slots
and native console reporting. It reaches SIInit at `0x801F52F4`; full OSInit,
game initialization, the main loop and a rendered game frame remain unachieved.
See [runner configuration and limitations](native/recompiler/ORIGINAL_STARTUP.md).

## Phases and completion criteria

### 0. Establish a verified input and reference build

- Keep the existing GameCube recompilation target separate from the native
  Windows target.
- Identify the supported revision from the image's disc ID and validate
  extracted executable/modules against the repository's expected hashes.
- [x] Inspect the user's local RVZ header without modifying the image. It
  identifies GameCube disc ID `GYWEE9`, disc number 0, revision 0, and title
  `Harvest Moon: A Wonderful Life`.
- [x] Extract the original `main.dol`, `wlp0.rel`, `wlp1.rel`, and `wlp2.rel`
  from the local RVZ and verify all four hashes against this repository.
  All match the expected US revision 0 hashes. The FST is extracted too;
  general game resources have not been extracted.
- Make the original DOL/REL build reproducible as a behavioral reference;
  MWCC/MWLD and a working local reference build are not yet available.
- Keep game images, extracted copyrighted assets, and proprietary compiler
  binaries out of the repository. Users must provide any required game data
  locally and have the right to use it.

**Complete when:** the image version is verified, extracted executable hashes
match, and repeatable game behavior can be compared against the reference.

### 1. Extract the user's executable and game modules

- [x] Read and validate RVZ header metadata, including the embedded GameCube
  disc ID and revision.
- [x] Add GameCube RVZ random access with a pinned static Zstandard dependency,
  header SHA-1 verification, raw/group table validation, and literal/PRNG
  unpacking according to [Dolphin's format specification](https://github.com/dolphin-emu/dolphin/blob/master/docs/WiaAndRvz.md).
- [x] Extract the DOL, FST and three REL modules. Refuse unsupported revisions
  and executable hash mismatches before creating output files.
- [x] Add general resource listing/extraction and a read-only range reader
  backed by the FST/RVZ. Runtime DVD command scheduling remains separate work;
  ISO/GCM input remains optional future work.
- Generate intermediate/native build inputs locally; never add extracted
  copyrighted game data to version control or ship it inside the executable.
- Retain the existing GameCube build as an independent baseline.

**Complete when:** a supported user image is identified and the full DOL, all
RELs, and file-system metadata can be extracted and validated locally.
The local RVZ now passes this extraction milestone. Native resource reads work
in controlled integration tests, including a cooperative DVD read/callback
queue. Full runtime scheduling and device services are still needed.

### 2. Build a static PowerPC-to-native recompiler

- [x] Add a DOL text-section reader with bounds and alignment checks.
- [x] Add initial C++ code generation for a limited subset of integer
  instructions, common direct/conditional branches, link-register calls,
  multiple/indexed/immediate memory operations, and returns.
- [x] Add unit tests for DOL parsing, big-endian word reads, generated output,
  unsupported instructions, branch conditions, and guest-memory bounds.
- [x] Compile generated synthetic instruction tests with the host compiler and
  execute them, covering register logical/shift operations, XER carry,
  conditional calls, special registers and multiple-register loads.
- [x] Add DOL section loading into 24 MiB of RAM with physical, cached and
  uncached aliases; reject MMIO/unmapped access and invalid load ranges.
- [x] Compile seven selected US startup ranges from the hash-verified local
  DOL, including its first entry-point call. Verify register setup, startup
  helpers, byte copy and argument-pointer relocation. This is an opt-in test
  target, not a game boot or hardware/OS implementation.
- [x] Extend the compiled US tests with the original ROM/BSS initializer and
  original `memcpy`, `memset` and fill helper. Verify BSS clearing, text/guard
  preservation and ABI restoration, plus memory-function alignment, length
  and overlap cases. Add signed/unsigned register comparisons and `nor`.
  This initializer runs independently in tests; it does not bypass hardware
  initialization in a claimed game boot. ROM relocation still fails explicitly
  at the cache-flush dependency, which has no native implementation yet.
- [x] Add raw primary/paired FPR lanes and FPSCR state. Compile double-precision
  load/store immediate/indexed/update forms and scalar register move/sign
  operations, including CR1 record semantics. Test signed zero, infinities,
  normal/subnormal values, NaN payloads, separate FPR/GPR register indices,
  RAM aliases, alignment, address wrapping and transactional access failures.
  Execute the original scalar `__OSFPRInit` block independently and the
  original f14..f31 save/restore helpers with whole-RAM guards.
  Preserve both FPR lanes/FPSCR across DVD success, cancel and throwing callbacks.
  Arithmetic, rounding conversions, full paired-single execution and privileged
  hardware initialization remain unsupported; the entry-point boot boundary
  has not moved.
- [x] Compile FPSCR masked/immediate writes and boxed reads
  (`mtfsf`, `mtfsfi`, `mffs`, including record forms). Recompute VX/FEX from
  cause/enable flags while preserving explicitly written FX and unselected
  fields; verify all 256 masks, 8 fields, 16 immediate values and both record
  forms with independent reference tests. Reject reserved instruction bits.
  The original scalar FPR initialization now clears FPSCR and returns; original
  `PPCMffpscr`/`PPCMtfpscr` wrappers preserve stack boundaries, f31 and paired
  lanes. Rounding-mode bits are guest state only, not host-global settings.
  MSR-dependent program exceptions and arithmetic rounding remain unsupported;
  this does not bypass the full startup's hardware/paired-single requirements.
- [x] Compile paired register moves/sign operations and all four lane merges,
  preserving raw lane bits, overlapping sources/destinations and CR1 record
  behavior. Add all eight GQR SPR transfers and unquantized immediate
  `psq_l`/`psq_lu` with both W modes, sign-extended displacement and transactional
  whole-pair RAM validation. Widen IEEE single bits without host arithmetic;
  test normalization boundaries, NaN payloads, 65,536 deterministic finite
  conversion candidates, all GQR selectors, aliases, RAM boundaries and explicit
  rejection of every unsupported load type. Save GQRs across DVD callbacks.
  Execute original GQR reset and the original paired/scalar FPR initialization
  body; all 64 lanes and FPSCR clear, return ABI and whole RAM preserved.
  Privileged MSR/HID2/cache setup is not executed; paired stores and paired
  arithmetic remain unsupported. Integer load quantization and indexed loads
  are covered below.
- [x] Add unsigned/signed 8-bit and 16-bit integer dequantization for immediate
  `psq_l`/`psq_lu`, with exact signed six-bit GQR scaling, big-endian reads,
  type-dependent byte counts and W=1's fixed second lane.
  Use integer-only IEEE bit construction; every representable integer/scale
  combination fits exactly, without host rounding or arithmetic.
  Check all 8,421,376 input/type/scale combinations against an independent
  numeric reference. Exercise compiled loads across every GQR selector,
  scale, W/update mode and RAM alias, signed/unsigned extremes, unaligned
  source data, final legal RAM access and transactional boundary failures.
  Reserved load types still fail explicitly. This extends data loading,
  not paired arithmetic, GPU submission or the hardware startup boundary.
- [x] Compile indexed `psq_lx`/`psq_lux` through the shared immediate-load
  implementation. Run the same floating/integer payload, GQR-selector, W/update,
  scale, RAM-alias and boundary-failure checks for both addressing forms.
  Verify RA=0, RB=0, RA=RB update ordering, index preservation and 32-bit address
  wrapping with whole-RAM guards. Reject invalid record forms, update RA=0
  and still-unsupported paired-store encodings. This is instruction support,
  not proof of an original indexed game routine or a full game startup.
- [x] Compile `mfmsr`/`mtmsr` with FP status and the existing cooperative
  interrupt mask as the sole EE source. Compile HID2 SPR read/write with
  PSE/LSQE changes; reject every other changed bit before mutation, including
  unsupported MMU, exception, privilege and DMA mode changes. Test all allowed
  mode combinations, preserved non-changing MSR bits, reserved instruction
  fields and every unsupported changed bit in both directions.
  Execute the complete original `__OSFPRInit`, including its FP-enable prefix
  and both HID2 branch outcomes. Initially execute the original `__OSPSInit`
  prefix and HID2 wrappers up to `ICFlashInvalidate` at `0x801b159c` (the
  complete routine is covered below). Preserve MSR/HID2 across DVD callbacks,
  including cancellation and exceptions. This does not implement privileged
  execution, FP-unavailable exceptions or HID2 instruction gating, and does
  not move the entry-point boot boundary.
- [x] Compile HID0 SPR transfers for ICE/DCE, self-clearing ICFI and SPD, plus
  `sync`/`isync` as host memory fences over coherent guest RAM. Reject reserved
  instruction fields and all other changed HID0 bits atomically. Verify all
  enable/invalidation combinations, repeated invalidation, preserved unrelated
  bits, all unsupported-bit transitions and unchanged RAM/register guards.
  Retain static translation bindings and dispatch code checks after invalidation;
  cache timing, locking and self-modifying code are not implemented.
  Execute original HID0 wrappers, `ICFlashInvalidate`, `ICEnable`, `DCEnable`
  and complete `__OSPSInit`. Run the original `__init_hardware` prefix through
  paired and FPR initialization in order, checking stack writes and all
  GQR/FPR/FPSCR resets; stop before `__OSCacheInit` at `0x80003418`, not past
  its L2/error-handler dependencies. Restore HID0 across DVD callbacks.
  Fix normal PC advancement for non-branch opcode-19 instructions, with
  terminal `isync`/`crxor`/`crclr` regression tests. Entry-point boot is still
  incomplete; this is an independent hardware-prefix fixture.
- [x] Compile L2CR transfers for L2E/L2I, synchronously completing invalidation
  over coherent RAM with L2IP clear. Reject other changed bits and invalidation
  while enabled before mutation. Test allowed writes, repeated invalidation,
  preserved configuration and every unsupported changed bit in both directions.
  Execute complete original `__init_hardware`/`__OSCacheInit`, L2 invalidation
  loops, original `DBPrintf` and DMA error-handler registration, including the
  original interrupt disable/restore routines. Check all 16 L1/L2/interrupt
  initial-state combinations, diagnostic call order, old-handler return,
  stack/ABI, masked invalidation and whole-RAM guards. Preserve L2CR across
  DVD callbacks. DMA exception delivery, cache timing and the special FPU
  error-handler path are not verified.
- [x] Execute the original entry continuously through register initialization,
  complete hardware initialization, ROM/BSS clear, empty argument setup and
  original `DBInit`. Verify its globals/physical exception destination and stop
  before `OSInit` at `0x80003264`. Loader IR/DR state and initially zero RAM
  are fixture preconditions, not implemented MMU behavior. Preserve the two
  hardware zero constants while poisoning the remaining BSS; check original
  linkage writes, BSS clearing and whole-RAM guards. This is not a game boot.
- [x] Compile `addc`/`adde` (including record forms) using shared carry logic.
  Test carry-in/out, signed boundaries, source/destination overlap, CR0/SO and
  preservation of unrelated XER bits; overflow-enable forms remain rejected.
  Execute original `OSGetTime`/`__OSGetSystemTime` through the native time-base
  callback and original interrupt wrappers. Test torn-read retry, 64-bit offset
  carry/wrap, both interrupt states and RAM/ABI guards.
  Continue the verified entry state with the original `OSInit` prefix: set the
  initialization flag, record system start time and mask interrupts. Continue
  through six original performance-monitor reset wrappers, speculation disable
  and non-IEEE setup, boot-info/arena initialization and exception installation
  plus system-call vector, alarm and module initialization to `0x801b0f6c`,
  before `__OSInterruptInit`.
  Test the already-initialized
  skip and epilogue separately; never claim they complete first-time startup.
- [x] Compile FPSCR bit operations `mtfsb0`/`mtfsb1`, including record forms,
  derived VX/FEX summaries and sticky FX on newly set exception causes.
  Check all bit selectors, repeated writes, existing causes with FX cleared,
  CR1, reserved encoding rejection and unchanged RAM/registers/host rounding.
  Execute original `PPCSetFpNonIEEEMode` independently and in OSInit: NI is guest status,
  not arithmetic behavior or a bypass of the pending OSInit dependencies.
- [x] Compile zero-only `mtspr` resets for MMCR0/1 (SPR 952/956) and PMC1..4
  (953/954/957/958). Preserve their guest state across DVD completion,
  cancellation and throwing callbacks. Test all registers, repeated writes,
  both interrupt states, every nonzero single-bit rejection before mutation,
  unsupported read/record encodings and the original OSInit reset-call order.
  Nonzero monitor writes and hardware event counting remain unsupported.
- [x] Retain HID0 SPD (`0x200`) as guest control state and execute the original
  `PPCDisableSpeculation` through original HID0 wrappers. Test all SPD/cache
  combinations, repeated calls, both interrupt states, loader-bit preservation
  and stack/ABI guards. Continue OSInit through original SPD and NI setup in
  order; ensure its already-initialized path leaves both settings untouched.
  No speculative pipeline or NI arithmetic is implemented.
- [x] Execute original OSInit boot-info/arena setup and original arena
  getters/setters. Test BI2 metadata, low-memory fallback and absent metadata,
  debug values below/at/above 2, full-word versus low-byte debug handling, PAD
  specification propagation and the DVD long-filename flag. Verify default and
  loader-supplied bounds, debug-dependent stack reservation, exact RAM writes,
  setter order and preserved CPU state. Continue the entry-derived default path
  to `0x801b0f5c`; initialized re-entry must leave boot info and arenas untouched.
  Arena accessors store exact guest values, not host allocations; a complete
  loader boot-info contract remains pending; exception installation follows below.
- [x] Compile `dcbf`/`icbi` with checked, aligned 32-byte RAM blocks and host
  fences over coherent storage. Reject reserved encodings and unmapped blocks;
  verify aliases, effective-address wrap and preserved RAM/registers. Run
  original `DCFlushRangeNoSync`/`ICInvalidateRange`, testing zero/unaligned
  ranges, iteration counts and unmapped accesses. Keep static bindings and
  modified-code rejection after block invalidation.
- [x] Execute original `OSExceptionInit`, its debug-mark helper and default
  handler setter. Check exact vector/integrator/table contents across debug
  thresholds, marked masks and an existing integrator; verify cache-call ranges,
  template restoration, RAM guards and return ABI. Continue entry-derived OSInit
  to `0x801b0f60`. Installed vector/default-handler code is not executed:
  exception delivery and exception returns remain pending.
- [x] Execute original `__OSInitSystemCall` through original memcpy and cache
  routines. Check exact `0x1c`-byte vector replacement at `0x80000c00`, untouched
  trailing bytes, `0x100`-byte cache ranges and call order, repeated installation,
  both interrupt states and stack/register preservation. Continue entry-derived
  OSInit to `0x801b0f64` before alarm initialization. System-call execution is
  not implemented; compilation of `sc` and `rfi` remains explicitly rejected.
- [x] Execute original `OSInitAlarm`, exception-handler getter/setter and
  `OSRegisterResetFunction`. Verify exact alarm-queue clearing, decrementer
  handler and reset-node registration, both interrupt states, an existing reset
  list and idempotent already-installed/repeated initialization. Separately
  check stable reset-list priority ordering, head/middle/tail insertion and
  unsigned maximum priorities. Continue entry-derived OSInit to `0x801b0f68`.
  Alarm scheduling, decrementer delivery and reset-callback execution remain
  unsupported; registration is not execution.
- [x] Execute original `__OSModuleInit` and continue entry-derived OSInit to
  `0x801b0f6c`. Verify exact clearing of low-memory words `0x800030c8/cc/d0`,
  poisoned/zero values, repeated initialization, both interrupt states and
  unchanged module payloads/CPU state. This resets SDK metadata only; SDK
  `OSLink`/`OSUnlink`, module-list integration and native binding lifetime
  management remain pending.
- [x] Run original `__OSInterruptInit` through handler-table and software-mask
  RAM setup, independently and from a saved entry-derived OSInit state.
  Verify exact writes including the original nested memset stack frame, poisoned
  table contents, both interrupt states and repeatable rejection at the first
  PI mask write (`0x801b3b08`, address `0xcc003004`). Initialization has not
  returned; the completed OSInit prefix remains `0x801b0f6c`.
  Execute original interrupt handler getters/setters for all 32 slots, RAM
  aliases, null/previous handlers and signed low-halfword selectors, preserving
  the SDK's unchecked indexing and existing native memory bounds checks.
  PI/device masks, interrupt dispatch and delivery still need native support.
- [x] Add opt-in bootstrap PI mask state at exactly `0xcc003004`, supporting
  only the original `0xf0` write and 32-bit readback. Reject other values,
  widths/addresses and retain default unmapped behavior without opt-in.
  Device writes persist through DVD completion/cancellation/throwing callbacks,
  unlike saved CPU context. This is not interrupt delivery or a general PI model.
  Compile `andc` with record/SO and alias tests; run original `__OSMaskInterrupts`
  and `SetInterruptMask` through the software-mask update to the unsupported
  MI mask write (`0x801b3be0`, `0xcc00401c`), independently and from the saved
  entry-derived state. Verify exact RAM/stack changes; OSInit remains incomplete.
- [x] Add opt-in MI mask state at exactly `0xcc00401c`, supporting only the
  original 16-bit zero reset and readback. Reject nonzero writes without
  mutation, unsupported widths/addresses/aliases and accesses without opt-in.
  Check compiled `lhz`/`sth`, halfword truncation, repeated resets, all nonzero
  single-bit writes and unchanged RAM/status. MI writes persist through DVD
  completion/cancellation/throwing callbacks. Original interrupt setup now
  clears zero/poisoned MI masks and reaches the unsupported DSP control read
  (`0x801b3bfc`, `0xcc00500a`), with both interrupt states and exact RAM/stack
  guards; also verify entry-derived continuation. No interrupt delivery or
  complete MI/DSP backend; OSInit still has not returned.
- [x] Add opt-in DSP control readback and startup-only interrupt-disable writes
  at `0xcc00500a` (16-bit). Preserve pending write-one-to-clear status, halt,
  DMA status and init-code state while clearing all three interrupt enables.
  Reject control commands, acknowledgements, reserved state, enable writes and
  unsupported widths/addresses/aliases before mutation. Verify all 512 supported
  initial bit combinations, repeated writes, halfword truncation, status/RAM
  guards and device persistence through DVD callbacks. Original interrupt setup
  now reaches the unsupported AI control read (`0x801b3c4c`, `0xcc006c00`);
  verify independently and from entry-derived state. No DSP/audio/DMA execution
  or interrupt delivery; OSInit remains incomplete.
- [x] Add independently opt-in EXI status registers for all three channels
  (`0xcc006800/14/28`, 32-bit), supporting idle startup interrupt masking.
  Retain pending write-one-to-clear flags, read-only presence and clock bits;
  reject chip selection, ROM-disable state, acknowledgements, enable writes,
  reserved bits and unsupported access forms. Exhaust 1,024 supported initial
  combinations per channel 0/1 and 128 for channel 2; test repeats, no mutation
  on rejection, ordinary RAM/flat memory and DVD callback device persistence.
  Original `__OSInterruptInit` now completes with all startup registers enabled:
  external exception handler installation, interrupt/ABI restoration and exact
  RAM/stack guards pass independently and from entry-derived state, returning
  to `0x801b0f70`. The OSInit prefix fixture still ends at `0x801b0f6c`;
  later code is covered separately below. No EXI transfers or interrupt delivery.
- [x] Execute original OSInit continuation from the saved entry-derived state
  through complete interrupt setup, reset-switch handler registration (slot 22,
  `0x801b5974`) and original `__OSContextInit`. Install FPU exception handler
  `0x801b3274`, clear owner pointer `0x800000d8` and execute original `DBPrintf`
  without replacing it with host logging. Test aliases, zero/poisoned owner state,
  repeated calls, both interrupt states, exact RAM/stack writes, raw FPR bits,
  CR/XER and register preservation. Continue through warm `__OSCacheInit` to
  `0x801b0f88`, before `EXIInit`; earlier prefix/default-unmapped/re-entry tests
  remain intact.
  No context switching, scheduler or FPU exception delivery; OSInit is incomplete.
- [x] Execute the original OSInit cache call with already-enabled L1/L2 caches.
  Preserve cache state, HID0 SPD and raw floating-point state while re-registering
  the original DMA error handler. Verify exact warm-path dependency order,
  previous-handler return, both interrupt states, repeats, callee-saved registers
  and exact RAM/stack writes. Existing hardware tests retain cold-path coverage.
  No physical cache timing or DMA-error delivery; next startup dependency is EXIInit.
- [x] Execute the original EXIInit prefix through its three transfer-idle checks
  and software-mask reuse. Add independently opt-in read-only 32-bit transfer
  controls (`0xcc00680c/20/34`); reject busy/reserved states explicitly to avoid
  unbounded polling without a transfer backend. Test all 32 inactive combinations
  per channel, repeats, rejected writes/access forms and unchanged RAM/state.
  Verify original busy-state rejection, partial opt-in boundaries, exact RAM/stack
  writes, CPU/device preservation and entry-derived continuation. Stop at
  `0x801f74a4` before EXI status reset; EXIInit does not return. No transfers,
  completion events or device probing.
- [x] Continue the entry-derived EXI initialization through original idle status
  resets, channel-0 ROMDIS setup, all eight interrupt-handler registrations and
  uncached channel-0/device-2 identification. Translate original GetID, lock,
  interrupt-mask helper and select routines. Require explicit initialization
  opt-in plus mapped inactive controls for reset writes; exhaustively test
  valid status combinations, preserved pending/presence bits and sticky ROMDIS
  (zero writes cannot re-enable the mask ROM), plus rejected
  busy/reserved/unsupported states without mutation. Verify the connected chain's
  handler slots, software lock/selection, zero uncached ID, bounded calls,
  non-stack RAM and CPU/device guards. Stop on the real unsupported chip-select
  write at `0x801f6f24` (`0xcc006800`, `0x200`); do not fabricate a device ID or
  return success. EXIInit and OSInit remain incomplete.
  Register semantics: [YAGCD EXI hardware registers](https://gc-forever.com/yagcd/chap5.html#sec5.9).
- [x] Connect the real startup chain to a separate production Windows console
  executable. Generate bounded native blocks from a hash-verified DOL, including
  original entry OSInit/EXIInit call sites; retain original-word verification at
  registration and dispatch. Add opt-in recompiler branch yielding and precise
  instruction PC reporting, leaving existing function translations unchanged.
  Verify compiled direct/conditional/CTR/LR branches, linked-call continuations,
  self-loop budget exhaustion and exact fault PC. Run the production executable
  from `0x80003154` to the real EXI selection failure at `0x801F6F24`, with no
  supplied peripheral ID or substituted OSInit. Test dispatch/depth limits, a
  nonconverging original time-base loop, missing bindings/time source and
  truncated/mutated on-disk DOL plus mutated RAM-code rejection. This is a
  runnable original startup milestone, not full OSInit or a game boot.
- [x] Complete original EXIInit in the production runner under an explicit
  empty-external-port profile. Implement one-hot device selection, clocks,
  sticky ROMDIS, W1C interrupt status, immediate data and transaction controls.
  Commit completion/TCINT only after a successful backend call; missing or
  throwing backends never fabricate completion. Keep RTC/SRAM/ROM and AD16
  requests as explicit errors. Translate original EXIImm/EXISync/deselect and
  empty-card probing, plus unsigned high multiplication used by these routines.
  Run original OSReport/version registration through a checked native `%s`/`%%`
  console service using the guest ABI. Verify exact two-transfer ID sequence,
  channel unlock, empty-slot probe state, exact emitted version string,
  missing backend/sink failures, transaction dimensions and rejected operations.
  Original EXIInit returns and the runner calls SIInit (`0x801F52F4`,
  LR `0x801B0F90`). This is not full OSInit, input or game rendering.
  Empty-device transfer semantics were checked against the public
  [Dolphin EXI interface behavior](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/EXI/EXI_Device.cpp);
  no emulator code is copied or executed.
- [x] Add opt-in idle AI control readback and startup-only interrupt-disable
  writes at `0xcc006c00` (32-bit). Retain pending write-one-to-clear status,
  interrupt validity and frequency bits. Reject playback, counter-reset commands,
  acknowledgements, reserved state, enable writes and unsupported access forms
  without mutation. Verify all 32 supported initial combinations, repeat writes,
  RAM/status guards and DVD callback persistence. Original interrupt setup reaches
  the unsupported EXI channel 0 control read (`0x801b3c7c`, `0xcc006800`), also
  from entry-derived state with exact RAM/stack guards. No audio, sample timing
  or interrupt delivery; OSInit remains incomplete.
- [x] Parse version 3 REL sections, imports and relocation streams. Stage
  module loading and BSS initialization, resolve DOL/self/cross-module imports,
  and apply the relocation types present in the original US files. Synthetic
  tests cover fixup results, branch boundaries, invalid data, alias overlaps,
  long NOP deltas and no partial guest writes on failure.
- [x] Link all three hash-verified original RELs in test RAM and verify all
  32,637 fixups. These relocation-only tests resolve but do not execute entrypoints. This is not an
  implementation of the SDK module list, dynamic `OSLink`/`OSUnlink`, or
  native code binding.
- [x] Add module-scoped native function registration and dispatch through
  compiled direct/indirect calls, tail branches and returns. Reject missing,
  stale or incorrectly placed translations and remove module bindings explicitly.
  Test limits, registration atomicity and callback restoration.
- [x] Compile and execute two selected functions from the hash-verified,
  relocated US wlp0 module, checking DOL data imports, results and return/ABI
  state. Add `cntlzw`/`cntlzw.` translation and compiled semantics tests.
  The selected functions use fixed test placements, not a dynamic runtime.
- [x] Compile and run all 13 original wlp0 static constructors (6,428 PPC
  instructions), plus the original DOL ABI/object helpers they call. Check
  nonvolatile registers, stack/SDA, selected table copies, global construction
  and destructor registration; reject writes outside module data/BSS and stack.
- [x] Execute the original wlp0 prolog through its constructor array and verify
  its three scene-selection paths stop explicitly at the unimplemented game
  allocator `0x80178E88`. Execute the original epilog/destructor chain and its
  empty-chain case using a fresh test frame. Add `extsh`/`extsh.` semantics.
  This is partial module initialization, not successful module/game startup.
- [x] Translate original heap initialization, descriptor constructors, scalar
  new/delete and allocation virtual methods. Test zero/split/exact allocations,
  counters, timestamps, exhaustion, free-block coalescing and reuse in a local
  64 KiB test arena. Preserve original allocator behavior rather than replacing
  it with a stub; out-of-memory still stops at the missing `OSPanic` service.
- [x] Add translated low/high `mftb` reads backed by an explicit native
  steady-clock time base (40.5 MHz), plus compiled `crxor` and `subfic` semantics.
  Test missing-clock failure and deterministic allocation timestamps.
- [x] With allocator/time-base bindings, verify the original wlp0 prolog gets
  past allocation to each selected scene constructor; keep this missing-binding
  boundary tested separately from the opt-in scene translations.
- [x] Translate integer prefixes of all three scene constructors and their
  shared DOL base constructor. Verify base ABI and exact write boundaries,
  original calendar getters (`divwu`/`mullw`), nested list/vtable initialization,
  original `memset` buffer clearing, allocation counts and whole-RAM write
  boundaries. Controlled fixture globals are not initialized game systems.
  The default/mode-8/mode-9 paths stop at `0x8000FEE8`, `0x8017CD5C` and
  `0x8101DDB0`, respectively. Floating-point continuations and scene assignment
  remain incomplete.
- [x] Add an opt-in original dependency group: registration manager constructor,
  bitset initialization, priority-sorted object registration, detach/reparent,
  calendar period lookup and the next nested REL constructor. Test unsigned
  priority extremes, equal priorities, earlier reprioritization, moves between
  managers, all calendar bucket boundaries and saturation, return ABI and
  whole-RAM write guards. The extended default/mode-8/mode-9 paths stop at
  `0x8001BD04`, `0x8100DCD8` and `0x8002283C`. The original virtual archive
  initializer executes its empty-resource case; without the separate archive
  bindings, nonempty input still fails at `ARCInitHandle` (`0x80203B4C`).
- [x] Compile original U8 handle initialization, path lookup, indexed file open,
  pointer/length/close helpers and game indexed getters. Verify synthetic nested
  archive pointers/payloads, case-insensitive and relative/absolute lookup,
  dot/parent components, original separator behavior, invalid indices and
  directories, zero-byte files, empty resources, ABI and whole-RAM write guards.
  Invalid magic still reaches missing `OSPanic`; unmapped metadata accesses
  fail explicitly. Original metadata validation is not hardened; game getters
  require valid indices.
- [x] Share validated FST parsing between executable import and arbitrary
  disc-file reads. Add exact-path resource listing/read APIs and CLI commands,
  with hierarchy/name/type/duplicate-path/range tests and explicit missing-file
  failures. Single-file output refuses existing files.
- [x] Connect the user's verified US RVZ to the original compiled U8/game
  archive APIs in an optional integration test (`HMAWL_US_RVZ` with REL tests).
  Check executable/FST/resource hashes, all five files of one real archive and
  its nested animation archive, payload identity, ABI and whole-RAM write
  guards. This is controlled resource loading, not the game's DVD runtime;
  scheduling and game allocator/lifetime integration remain.
- [x] Translate the original CLZ header/size helpers and complete block/stream
  decompressors to native C++, including GPR save/restore dependencies. Add a
  checked version-0 host decoder and `--extract-rvz-clz`, preserving raw-file
  extraction behavior and refusing malformed input before output creation.
  Test literals, flag rollover, overlapping/distant references, padded
  multi-block streams and malformed bounds. Decode all eleven original US
  compressed archives through both implementations, compare every byte, and
  verify original U8/game file pointers and lengths with ABI/RAM-write guards.
  Original decode-queue scheduling and asset lifetime management remain
  unimplemented; SDK callback integration is covered below.
- [x] Compile original DVD FST initialization, path lookup/open, directory
  change/current-path helpers and fresh game `DvdFile` construction/open.
  Verify all 1,660 US file offsets/sizes through original `DVDOpen`, plus
  synthetic case/directory/separator/truncation/empty-file behavior and
  explicit missing-file `OSReport`/invalid-name `OSPanic` boundaries.
  Fixtures supply boot FST state and long-filename mode; OS startup is not run.
- [x] Add a synchronous native RVZ range reader consuming validated original
  `DVDFileInfo` metadata, with guest bounds, exact disc padding bytes, partial
  reads and transactional failures. Connect original game file construction
  to this primitive and then compiled CLZ/U8 initialization in real-disc tests.
  This primitive alone does not replace SDK read APIs or implement callbacks/
  close tracking; asynchronous read integration is covered below.
  Add required `subfe`/`subfe.` translation with carry, CR/XER, aliasing and
  unsupported-overflow tests.
- [x] Compile original `DVDReadAsyncPrio` and the SDK completion forwarder.
  Add explicit, original-code-checked host-service bindings for low-level read
  submission and queued cancellation (not translations of the hardware driver).
  A cooperative native queue defers transfers/notifications, orders priorities
  0..3 with FIFO ties, preserves callback CPU context and updates SDK status/
  transfer counts. Queued cancellation reports -3 before cancel completion 0;
  failed reads report -1 and propagate the error without partial payload writes.
  Verify original SDK submission/forwarding with real raw/compressed resources,
  original CLZ/U8 initialization and RAM/ABI guards. Test aliases/conflicting
  buffers, metadata changes, missing callbacks/services, requeueing, zero-size
  reads, non-reentrancy, callback exceptions and truncated RVZ I/O.
  Pumping is explicitly host-driven/single-threaded. In-flight transfer
  cancellation, synchronous SDK reads, OS scheduling and decode-queue scheduling
  remain unimplemented. Close integration is covered below.
- [x] Compile the original game async `DvdFile` read entry, DMA alignment,
  SDK command status, game read callback, virtual completion handler and
  original completion-flag observer. Verify raw/compressed real-disc reads
  and queued cancellation through this original callback chain, including
  exact payloads, done/observer flags and RAM/ABI guards. Preserve original
  closed-file rejection and alignment semantics.
  Translate local `bctr` jump tables to static C++ target switches, retaining
  compiled-only dispatch for external targets; test conditional, backward,
  aligned and external jumps. Add original-code-checked cooperative interrupt
  disable/enable/restore host bindings. Masked pumps defer completion and
  callbacks restore the caller's mask on success/failure. Test nested saved
  masks and original status normalization. These bindings do not emulate MSR,
  hardware interrupts, OS startup, preemptive threads or a complete scheduler.
- [x] Compile original game/SDK close, synchronous cancellation, its completion
  callback and `OSWakeupThread`, reusing the existing native queued-cancel service.
  Verify real raw/compressed-resource close/reopen, pending cancellation,
  repeated close, reopen with a pending read, completed/terminal states and
  original null-path open semantics. Check callback flags, unchanged canceled-read
  payloads, interrupt-mask restoration, RAM/ABI guards, missing services and
  unowned active-command rejection. Preserve the original open flag on close.
  Exercise original empty-queue wakeup and removal of suspended waiters.
  No OS wakeup stub is installed: runnable-thread selection still fails
  explicitly at its untranslated scheduler dependency. This supports close at
  cooperative safe points, not general synchronous I/O or OS-thread scheduling.
- [x] Compile original decode queue reset/enqueue/observer setup and its
  worker entry (`0x8019a6bc`). Verify synthetic CLZ and real
  `roomcommon.arc.clz` via original game DVD reads into original allocations,
  worker allocation/decompression, original source release and U8 opening.
  Compare every decoded byte and verify completion/no-replay, ABI/RAM guards
  and full heap coalescing after freeing the result. The completion observer
  is test-only; allocation, decoding and release execute original code.
  Bind `DCFlushRange` explicitly to checked coherent host RAM plus a memory
  fence, not a translated cache/syscall implementation. Test rounded ranges,
  aliases, zero length, boundary/overflow and MMIO errors.
  The ready-input worker is invoked directly; the nonempty queue's original
  OS thread launcher remains an explicit missing binding. Unready entries
  busy-wait in original code. Thread scheduling, waits/wakeups and concurrent
  read/decode execution remain unimplemented.
- [x] Compile the original alternate allocation-pool path, including allocation,
  membership checks and source/output release. Verify real and synthetic
  decode inputs through both allocation branches. Four-entry mixed-pool
  batches verify FIFO completion, first/last/null observers, payload guards
  and complete reclamation of both arenas after out-of-order result release.
  An exhausted alternate pool reaches the original `OSPanic` boundary without
  freeing its source or setting completion; original fatal-path allocation
  counter behavior is preserved. These remain ready-input worker fixtures,
  not OS-thread or concurrent DVD/decode scheduling.
- Decode the GameCube's PowerPC/Gekko instruction set and translate every
  reachable function into generated C++ or native code; do not execute the
  original instructions through an emulator or an instruction-at-a-time
  interpreter.
- Preserve the PowerPC calling convention, register/stack behavior, floating
  point, integer width, endianness, indirect calls, and relocations.
- Translate the main executable and dynamically loaded REL modules.
- Test individual instructions, function call boundaries, and generated
  functions against the GameCube reference before attempting whole-game startup.

**Current limitation:** generated output covers only requested code ranges and
the explicitly implemented instructions. Function discovery, complete PPC
semantics, dynamic REL lifetime/OSLink integration, automatic full-game native
binding, and full-game translation are not implemented.

**Complete when:** all executable code paths needed for startup and a test scene
are translated to native machine code and pass comparison with the reference.

### 3. Implement the Windows GameCube runtime

- Provide native implementations for the OS, threads, memory, DVD/resource
  access, input, timing, and audio services used by the translated code.
- Implement the GX rendering behavior needed by the game using Direct3D 11 (or
  another selected Windows renderer); a window and placeholder scene are not
  substitutes for the GX API.
- Support the original game data layout and dynamic REL loading without
  executing a GameCube emulator.

**Complete when:** translated startup code runs against native Windows
implementations through the original game's renderer, input, and resource
services.

### 4. Verify and expand gameplay

- Compare startup, scenes, controller input, timing, save operations, and audio
  against the supported US GameCube build.
- Expand missing instruction behavior and GX/runtime coverage based on
  observed differences.
- Ensure packaging requires the user's own game image and does not include the
  image or extracted assets.

**Complete when:** the intended game flow is playable on supported Windows
systems and the executable can be packaged without the GameCube build toolchain.

## Immediate next steps

1. Continue beyond the verified startup chain: entry reaches `0x80003264`,
   whose next call is `OSInit` at `0x801b0de0`. Register, hardware, ROM/BSS,
   empty-argument and `DBInit` initialization run together in the startup test.
   Implement remaining OS/platform dependencies explicitly rather than treating
   unsupported hardware instructions as successful no-ops.
   The initial `OSInit` prefix is independently verified through start-time
   capture/interrupt masking, MMCR/PMC resets and HID0 SPD/FPSCR NI setup
   plus boot-info/arena, exception and system-call vector installation
   and alarm/module initialization (`0x801b0f6c`);
   interrupt setup and remaining subsystem initialization
   still follow.
2. Expand the recompiler's opcode coverage, extend the tested REL/native-function
   bridge with function discovery and module lifetime management, implement
   the remaining scene dependencies (`0x8001BD04`, `0x8100DCD8`, `0x8002283C`)
   and floating-point continuations, and add differential tests against the
   GameCube reference. The three constructor prefixes do not yet return.
3. Rebuild the GameCube reference using the original compiler toolchain when
   available; MWCC/MWLD are not bundled with this repository.
4. Replace the renderer test with the native GX/runtime implementations needed
   to run translated game code.

## Constraints and risks

- The local RVZ's disc ID and all four executable hashes are verified; this
  does not establish full-disc integrity or correct native execution.
- The original MWCC/MWLD compilers are missing, and the current GameCube source
  is incomplete.
- Only an experimental subset recompiler exists; there is no full GameCube
  runtime in this repo.
- The new DOL reader and PowerPC translator are experimental; the translator
  supports only a small instruction subset and is not sufficient to boot the
  game.
- Function discovery, dynamic REL runtime integration, PPC semantics, GX, OS,
  audio, saves, and resource loading are substantial independent engineering work.
- The Direct3D placeholder scene is a development-only test and does not
  contain or implement Harvest Moon gameplay.
