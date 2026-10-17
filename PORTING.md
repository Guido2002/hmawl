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
  cancellation, synchronous SDK read/close, OS scheduling and the original game
  decode queue remain unimplemented.
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

1. Continue beyond the verified first startup call: entry reaches
   `0x80003158`, whose next call is `__init_hardware` at `0x80003400`.
   Plan explicit native replacements for its hardware/OS initialization
   dependencies rather than treating unsupported hardware instructions as
   successful no-ops. Current startup tests do not execute this call.
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
