# Native port architecture

Audit date: 2026-10-18. Status labels describe evidence, not percentage completion.
See [PORT_PROGRESS.md](PORT_PROGRESS.md) for this iteration's executed checks and
[native/recompiler/ORIGINAL_STARTUP.md](native/recompiler/ORIGINAL_STARTUP.md)
for the detailed compatibility contracts and earlier experiments.

## Two build products

* **VERIFIED — GameCube reconstruction:** `Makefile`, `obj_files.mk`,
  `static.lcf`, `partial.lcf`, `src/`, `include/`, and `asm/` build PowerPC
  `main.dol` and `wlp0/1/2.rel` under `build/hmawl_us_r0`. Much game logic
  remains assembly. This iteration inspected that build path; it did not rebuild it.
* **VERIFIED — native compilation:** root CMake builds `native/recompiler`.
  `hmawl_generate_original_startup` verifies the USA revision 0 DOL SHA-1
  `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`, then translates selected manifest
  ranges to C++. `hmawl_generate_original_scene` translates verified `wlp1.rel`
  ranges. Generated headers stay in ignored build directories. MSVC compiles
  them into `hmawl_original_startup_runtime` and `hmawl_original_startup.exe`.
  No Dolphin process or runtime PowerPC instruction decoder is used.
* **PARTIAL — coverage:** `original_startup_manifest.h` selects known required
  ranges. Unsupported instructions and missing translations stop explicitly.
  Compilation does not establish complete CPU or game compatibility.
* **VERIFIED — separate prototype:** `native/windows` builds an invented
  Direct3D renderer demonstration. It is not the original game's renderer.

## Execution and data path

| Component | Relationship and current boundary |
|---|---|
| DOL/REL loading | **VERIFIED:** `dol_image`, `rel_image`, `disc_import`, SHA-1 verification; executable data comes from the user's image. **PARTIAL:** startup executes original SDK linking for the observed scene, registers relocated native scene blocks, and removes run-scoped bindings on exit. Arbitrary module coverage/rebasing is not established. |
| Dispatch | **VERIFIED:** `NativeDispatcher` maps exact guest addresses to compiled functions, checks code words, preserves guest call/exception callbacks, and enforces block/call-depth limits. Direct calls, LR/CTR branches, local jump tables and missing-target failures have tests. **PARTIAL:** only registered translations can execute. |
| CPU semantics | **PARTIAL:** `ppc_recompiler`, `ppc_state`, `ppc_float` implement integer/CR/XER, floating point and selected paired-single operations. Unsupported forms fail; coverage is tested by generated semantics tests, not a claim of full Gekko conformance. |
| Memory | **VERIFIED:** 24 MiB checked guest RAM with physical, cached and uncached aliases; explicit big-endian loads/stores. Original stack/SDA/object layouts are retained. **PARTIAL:** coherent-memory model and selected hardware registers replace physical caches and devices. |
| Startup and OS | **PARTIAL:** original entry, OS initialization, constructors, allocator, interrupts, saved thread contexts, alarms and main-loop work execute. Native profiles supply clock, SRAM, console, DVD hardware and other hardware services. Cooperative execution is not cycle-accurate scheduling. |
| Disc and filesystem | **VERIFIED:** `RvzImage` reads/decompresses requested RVZ groups; validated FST resolves files without bulk extraction. `InstallOriginalStartupDisc` supplies boot metadata and DI DMA backend. Original DVD code handles startup requests/interrupts. Separate reader/queue fixtures test HLE paths; these do not replace the production DI path. |
| Archives and decoding | **VERIFIED in fixtures:** original CLZ/U8 code is compiled and compared with checked readers in `clz_image`/`disc_import`. Live worker progress measures decoded output bytes at completed-token boundaries. **UNKNOWN:** full live resource publication and intro completion until observed. |
| Graphics | **PARTIAL:** `native_gx`, `native_renderer`, `native_efb`, `native_vi` consume supported GX FIFO state, rasterize a bounded textured-quad profile, copy EFB to XFB, and deliver VI timing/interrupts. Windows GDI displays the actual EFB. Unsupported pipelines fail explicitly. A license preview is not proof of intro rendering or VI scanout. |
| Input | **NOT IMPLEMENTED for gameplay:** production SI profile reports disconnected controllers. Prototype WASD input does not control the original game. |
| Audio | **PARTIAL:** compiled DSP microcode, mailboxes, DMA, AI timing and Windows PCM delivery exist. Initial silent command lists have fixtures. General voice mixing/ADPCM and correct audible game music are not established. |
| Saves | **NOT IMPLEMENTED:** memory-card slots are disconnected; native console SRAM persistence is not game save support. |

## NWiiRecomp evaluation

**Selected as a design reference, not linked or vendored.** NWiiRecomp's
[runtime overview](https://github.com/BlackLineInteractive/NWiiRecomp) describes
an interpreter fallback for generated trampolines, streamed overlays and other
code absent from the DOL. That is a useful pattern, but its `CPUContext`/MMU and
runtime are separate from this port's `PpcState`, checked RAM/MMIO handlers,
paired-single bit representation and guest-call callbacks; it is not a
drop-in fallback. Falling back for an address that is present in the verified
DOL but missing from the manifest would also mask a static-coverage dependency
and weaken the existing code-integrity checks. Any future fallback should share
`PpcState` and be limited to positively identified runtime-generated code.

The upstream [LICENSE](https://github.com/BlackLineInteractive/NWiiRecomp/blob/595f176f1d24cc54ff2e8389feed12d7fb553cc2/LICENSE)
is a custom non-commercial-only license requiring attribution; it does not
grant unrestricted commercial use. No upstream source has been copied or
linked into this repository. Using NWiiRecomp as a reference does not change
the existing static native recompilation path.

## Diagnostics and regression workflow

**VERIFIED:** `startup_diagnostics.{h,cpp}` captures terminal CPU registers,
instruction word when RAM-mapped, active SDK thread pointer, bounded stack
backchain slots, recent compiled-block entries, recent disc transfers and FST
paths, decoder progress, and graphics counters. JSON includes explicit unavailable
fields and a failure key. Error-message classification routes investigation; it
does not assert a root cause. Captured backchain LR slots are not a validated call
stack. Native source locations and allocation history are **NOT IMPLEMENTED**.
An opt-in bounded generated-instruction PC/opcode ring is available through
`--trace-instructions`; its default capacity is zero.

Reports are written to unique `diagnostics/startup-*/report.json` and `summary.txt`
before the preview wait. `--diagnostics <directory>` selects another location;
`--trace-blocks 0` disables the default 128-entry history (maximum 65536).
`--trace-instructions 0` disables instruction capture (default); nonzero
capacities are limited to 65536 entries.
Collection never performs MMIO reads or queries the guest clock. Host overhead
can still affect production wall-clock scheduling; generated code checks for an
observer per instruction, while the callback and ring writes are skipped when
disabled. Report-write errors preserve the runner's original exit code. Process
crashes outside the caught C++ execution path and pre-run input errors are
**NOT IMPLEMENTED** report cases.

**VERIFIED:** CTest targets cover CPU compilation, generated instructions,
floating point, GX/VI/audio, RVZ imports, REL links, original resource decoding,
startup and diagnostic capture. Optional disc tests require the user's inputs.
`tools/native_smoke.ps1` runs the headless production CLI, parses its report and
emits a compact checkpoint summary. Preserve the log/report, classify the failure,
isolate it in a fixture, build, run focused tests, then compare a new run. Budget
exhaustion is a bounded observation, not a passing intro test.

## Windows build and validation

The configured build uses Visual Studio 2019 Build Tools and Windows SDK
10.0.22621.0. The default PATH in this session resolves `cmake`/`ctest` to MSYS2,
which cannot use the existing Visual Studio generator. Use the native tools:

```powershell
$cmake = 'C:/Program Files (x86)/Microsoft Visual Studio/2019/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
& $cmake -S . -B build/native -DHMAWL_ORIGINAL_US_DOL:FILEPATH="$PWD/build/game-us-r0/main.dol" -DHMAWL_US_DOL:FILEPATH="$PWD/build/game-us-r0/main.dol" -DHMAWL_US_REL_DIRECTORY:PATH="$PWD/build/game-us-r0" -DHMAWL_US_RVZ:FILEPATH="$PWD/Harvest Moon - A Wonderful Life (USA).rvz"
& $cmake --build build/native --config Release
& $ctest --test-dir build/native -C Release --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File tools/native_smoke.ps1 -MaxBlocks 150000000
```

The build uses cached zstd v1.5.7 source pinned by commit; a fresh configuration
needs that dependency available. Extract the user's supported RVZ with
`hmawl_recompiler --extract-rvz <input.rvz> build/game-us-r0` if necessary.
Keep extracted executables/resources and generated game code local and ignored.

Exit codes: `1` input/configuration error, `2` usage error, `3` dependency/integrity
stop, `4` execution limit, `5` unexpected original entry return.

## Milestones

| Milestone | Status |
|---|---|
| M0 Native startup | **VERIFIED** for the supported local image/profile; original game loop reached. |
| M1 License screen | **PARTIAL:** existing documentation records visual confirmation; current headless checks verify render/copy activity, not new reference-image comparison. |
| M2 Intro resources | **PARTIAL:** original `commonall.arc.clz` and `preload.arc.clz` decoder calls return in the live runner; full intro resource publication/initialization remains under investigation. |
| M3 Intro logic | **UNKNOWN:** scene prolog/constructor progress does not certify original intro initialization. |
| M4 Intro advances/rendering | **UNKNOWN:** no complete visually validated intro sequence. |
| M5 Main menu; M6 New game; M7 First area | **UNKNOWN:** not demonstrated. |
| M8 Controls | **NOT IMPLEMENTED** in the production input profile. |
| M9 Correct audio | **PARTIAL:** PCM infrastructure exists, audible game audio unverified. |
| M10 Save/load | **NOT IMPLEMENTED**. |
| M11 Major gameplay; M12 Stable release | **UNKNOWN / not achieved**. |
