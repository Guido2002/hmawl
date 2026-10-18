# Native port progress

## 2026-10-18 — Reproduce historical stop and add diagnostic capture

**Current verified milestone:** M0 startup and repeated original game loops.
Existing M1 license-preview evidence remains documented in `ORIGINAL_STARTUP.md`;
this iteration uses headless integration runs and does not claim new visual validation.

**Historical blocker:** `0x80154D00` is `func_80154D00` in
`asm/main/game/code_800F3228.s`. Its two instructions are `0x38600000`
(`li r3,0`) and `0x4E800020` (`blr`). The adjustment thunk at `0x80154FB4`
subtracts `0x18` from R3 and tail-branches there. Both were already present in
the current manifest. New tests execute both and verify the zero result and LR
return. No historical register dump or original failure log was found, so the
precise historical cause is **UNKNOWN**; missing registration in an older build
is a hypothesis, not a verified diagnosis. CLI exit 3 comes from the dependency
result returned by the caught C++ execution exception, not a guest exit at this PC.

**Evidence:** rebuilding the current source, then running headless with
50,000,000 blocks, ended at `PC=0x801F3DB8`, `LR=0x801F410C`, exit 4 (budget).
It completed 703 loops, produced 2,078,941/5,410,544 CLZ output bytes, and made
705 XFB copies. Log: `build/native/baseline-startup.txt`. This independently
demonstrates progress beyond the historical stop; partial output is not decoder
failure evidence.

**Implementation:** added `startup_diagnostics.{h,cpp}`, CLI report options,
`startup_diagnostics_tests.cpp`, CMake wiring, ignored `diagnostics/`, and
`tools/native_smoke.ps1`. Reports include CPU state, bounded block history,
RAM-only stack slots, SDK thread pointer, disc paths/offsets/destinations,
decoder starts/progress, graphics counters, error classification and failure key.
They explicitly label absent data. No guest registers, memory, clock or device
read handlers are changed by capture. Default history is 128 blocks, configurable
through `--trace-blocks`; zero disables history.

**Validation:** native Visual Studio Release build succeeded. Focused tests passed
for both historical routines, trace wrap/disable/bounds, JSON escaping, invalid
stack/MMIO capture, file correlation, stop classification, and deterministic
startup CPU/RAM parity with observation enabled/disabled. The smoke script's
1000-block case produced parseable JSON and correctly preserved exit 4.

**Build environment:** PATH selected MSYS2 CMake, incompatible with the configured
Visual Studio generator. Used the existing native Visual Studio CMake instead.
Building required access to Windows SDK metadata outside the sandbox. No SDK
installation or project architecture replacement was needed.

## 2026-10-18 — Post-decode constructor dependency

**Current verified milestone:** live decoding advanced beyond the earlier partial
resource. With a 150,000,000-block ceiling, the runner completed two worker decoder
calls before reaching an unrelated missing translation. M2 remains **PARTIAL**:
decoder returns alone do not prove complete intro resource publication or rendering.

**Observed blocker:** `PC=0x80101C34`, instruction `0x9421FFD0` (stack prologue),
`LR=0x80154848`, `SP=0x8035CDE8`, active SDK thread `0x80317F70`.
The original call at `0x80154844` enters an object constructor. The dispatcher
reported `No native translation registered at 0x80101C34.` after 105,900,406
block entries and 2,190 loops. The latest decoder was `preload.arc.clz`, with
722,688/722,688 output bytes and aggregate starts/returns 2/2.
Evidence: `diagnostics/startup-1792340632048722-0/report.json` and
`build/native/diagnostic-startup.txt`.

**Root cause — VERIFIED:** this observed constructor was absent from the compiled
binding set. It was not an exit instruction, a decoder stall, or an invalid
object pointer inferred from resource progress.

**Implementation:** added the exact original constructor range
`0x80101C34..0x80101D9C` (0x16c bytes) to `original_startup_manifest.h`.
A focused fixture verifies its vtables, self-linked list fields, singleton,
next-call argument, stack frame, and whole-RAM write boundaries before its
next missing nested constructor at `0x800FB5A4`. Recursive discovery exceeded
the generator's existing code bound; the final change preserves that bound and
uses an explicit range. No guessed constructor or successful stub was added.

**Regression correction:** the existing binding test expected `0x8018A778`,
a return instruction inside a generated block, to have a standalone dispatcher
entry. Inspection confirmed it is translated within the function at `0x8018A460`.
The assertion now checks that actual function entry. The initial suite reported
13/15 passing, with both startup tests failing this same assertion.

**Production validation — VERIFIED:** the bounded translation now executes and
reaches `0x800FB5A4`, `LR=0x80101D54`, `SP=0x8035CDB8`, `R3=0x80FB63E4`,
thread `0x80317F70`. It stops explicitly with exit 3 after 110,792,930 block
entries, 2,370 loops and 2,371 XFB copies. Both decoder calls returned.
Evidence:
`diagnostics/smoke-ba464eb9b7e645ada89d08d202191e96/startup-1792340972484410-0/report.json`.
The smoke script parsed the report and checked its exit code against the process.

**Resource identification — VERIFIED:** the retained decoder-start records match
`commonall.arc.clz` to source `0x804ED100`, destination `0x80FCBDC0`, and a
5,410,544-byte declared output. Its FST compressed-file size is 3,794,228 bytes.
The next resource is `preload.arc.clz`, producing 722,688 bytes. The current
progress counter measures decoded output at completed-token boundaries, not
compressed input consumed. The exact historical 1.19 MB sample cannot be
independently reconstructed from the missing historical log.

**Regression check:** Release build and 14/15 full-suite tests passed initially;
the disc test reached its intended render checkpoint but its old fixed timing
counters differed. Current deterministic observations are 4,219 completed PCM
periods, 675,200 submitted frames, 4,216/4,213 handlers/callbacks, 12,649 CPU mails,
134,783 DSP data transfers/40,905,100 bytes, 4,214 task IRQs, 4,216 DSP mails,
DSP PC `0x4A` (the old log printed decimal 74), and 69 thread context returns. These replace the previous
4,210-period/54-context baseline; exact CPU/RAM/device parity checks remain in
place. Final parity validation is recorded below when complete.

**Additional diagnostic checks — VERIFIED:** an unwritable report destination
(an existing file) logs a write failure without changing execution exit 4;
`--trace-blocks 65537` is rejected before running. JSON parsing and resource
correlation were exercised in real smoke runs. Whitespace checks pass with
`git -c core.whitespace=cr-at-eol diff --check` for this CRLF working tree.

**Next step:** inspect the next captured dependency and isolate its original
behavior before extending translation coverage. Intro, main menu, controls,
audible game audio and saving remain unverified or unimplemented; no playable
release is claimed.

## 2026-10-18 — Post-load nested constructor chain

**Production evidence — VERIFIED:** the latest headless smoke used the local
USA DOL/RVZ, a 150,000,000-block ceiling, and display disabled. The production
runner stopped with exit 3 at `PC=0x80104F80`, `LR=0x801027FC`, after 1,752
game-loop iterations. Both original CLZ calls returned; `preload.arc.clz`
produced its full 722,688 bytes. The report/process exit codes matched.
Report:
`diagnostics/smoke-9c80cdb82ec24786b39050df9425e738/startup-1792343695216807-0/report.json`.

**Implementation:** added only assembly-bounded functions
`0x8010274C..0x80102838` (0xEC bytes) and
`0x80186DF4..0x80186E10` (0x1C bytes), followed by the confirmed trivial base
initializer `0x800FCA98..0x800FCAB4` (0x1C bytes). The production-binding
fixture expects `0x80104F80` next and checks the constructor's object/stack
write boundary. The production runner has progressed through two successive
constructor dependencies; this does not prove intro completion or a displayed
frame. The fixture's whole-RAM boundary check was later exercised by the
existing Visual Studio CTest task; see the latest validation below.

**Measured workflow:** latest focused startup-test target build took 63.92 s;
production-runner build took 53.09 s; latest production smoke took 123.75 s
(preceding smoke: 141.82 s). These are current measurements, not a controlled
before/after speedup: different constructor paths and loop counts were reached,
and the pre-change `0x8010274C` boot was not timed. A speedup claim is therefore
not made. CMake Tools still reports no targets and cannot configure/run CTest;
the MSBuild-generated Visual Studio targets compiled successfully, but the
startup test executable was subsequently run through the existing Visual Studio
CTest task; see the latest validation below.

**NWiiRecomp decision:** evaluated as an architectural reference for its
documented dynamic-code interpreter fallback. Its separate `CPUContext`/MMU is
not compatible as a drop-in for this runtime's `PpcState` and checked memory
handlers. Its current custom license permits non-commercial use only and
requires attribution. No source was copied or linked; the static native
recompilation path remains unchanged. Any later fallback should share
`PpcState` and apply only to positively identified runtime-generated code, not
mask a missing translation for verified DOL code.

**Next confirmed blocker:** `0x80104F80`, whose assembly-bounded function ends
at `0x801050C8` (0x148 bytes) and calls `0x80182578`. The production run did
not reach the intro, main menu, gameplay input, audible audio, or VI pixel
scanout.

## 2026-10-18 — Bounded generated-instruction trace

**Implementation:** added an opt-in observer in the generated PowerPC
instruction path and a bounded PC/opcode ring in startup reports. The CLI and
`tools/native_smoke.ps1` accept `--trace-instructions` / `-TraceInstructions`
(0–65536; default 0). The no-trace mode avoids callback calls and ring writes;
generated instructions perform only a null-observer check. Reports distinguish
block entries from instruction entries and no longer label captured history as
unavailable when tracing is enabled. Tests cover the exact two instructions of
the historical `0x80154D00` routine, ring wrap/order, disabled capture and
capacity rejection.

**Validation:** the Release startup-diagnostics test target and production
runner both compiled successfully using the configured Visual Studio solution.
A one-million-block bounded production smoke with capacity 16 captured
7,930,572 guest instructions and the final 16 chronological PC/opcode pairs;
the JSON report and process both reported exit 4 at the configured budget.
Report:
`diagnostics/smoke-b5ffc569e2ca4ccba4137efbd5712d4c/startup-1792344045121140-0/report.json`.
The bounded smoke and smoke-script verification passed. The C++ test executable
was built, but CMake Tools could not configure the project at that point. The
existing Visual Studio CTest task later ran the complete suite successfully;
see the latest validation below.

**Timing:** the incremental Release build after the code changes took 2.86 s.
Three bounded 1M-entry runs measured a 0.653 s median on the pre-instrumentation
runner and 0.592 s on the new runner with tracing disabled. Their terminal PCs
varied substantially because timed game/device work changes the executed path;
this is not a controlled same-workload comparison, so it is **not** claimed as
a speedup. An untraced/trace-enabled smoke pair also differed in terminal PC,
so no trace-overhead percentage is asserted.

**Blocker at that point in the work:** the last full production smoke before
adding the observer/report path stopped at `0x80104F80` after 1,752 game-loop
iterations. The subsequent constructor-chain run below supersedes that blocker.

## 2026-10-18 — Constructor chain cleared; execution budget is current stop

**Production evidence — VERIFIED:** the latest full headless smoke ran with the
150,000,000-entry ceiling for 188.48 s and stopped with exit 4 at
`PC=0x801F3D80`, `LR=0x801F410C`, after 1,941 game-loop iterations. It did not
report a missing native translation. Both CLZ resource decodes completed;
`preload.arc.clz` produced all 722,688 bytes. Report:
`diagnostics/smoke-09d4d5cdbeed424a9e8fb1875b45e12a/startup-1792344803836131-0/report.json`.

**Stop context — VERIFIED from assembly:** `0x801F3D80` is a block within
`func_801F3B70` in `asm/main/libraries/dolphin.s`; its caller invokes that
function at `0x801F4108`, so the recorded LR is the following instruction at
`0x801F410C`. The function contains a bounded 0x9F-count loop. This establishes
where the budget was exhausted, not that this routine alone accounts for the
runtime or that the game is stuck in an infinite loop. The stop is a runner
budget limit, not evidence of a crash or unresolved DOL dependency.

**Graphics evidence and remaining gap:** the run recorded 2,039 quad draws,
502,196,893 rasterized pixel writes and 1,942 XFB copies; VI advanced 11,235
timed fields. However, pixel scanout remains unimplemented and the EFB report
includes an undefined power-on-pixel copy. These counters do not establish a
correctly visible intro frame.

**Regression fixture:** the post-load constructor test now checks registration
of the full set of assembly-confirmed nested initializer addresses, in
addition to executing `0x8010274C` in isolated state and checking object,
stack and write boundaries.

**Validation follow-up — VERIFIED:** the first test run exposed an undersized
fixture stack allowance: the original nested constructor chain wrote at
`0x8035CE80`, exactly `0x120` bytes below its initial stack pointer
`0x8035CFA0`. The fixture now checks the full observed stack range while
continuing to reject writes elsewhere in RAM. The Release Visual Studio
solution build succeeded, then the existing CTest task passed all 15/15 tests
in 360.53 s. The isolated original-startup test took 1.05 s; the disc-startup
test took 318.72 s. CMake Tools itself still reports no discovered tests, so
the working validation route is the repository's existing VS `RUN_TESTS`
project/task. This measures test duration, not a faster build/debug workflow.

**Current confirmed blocker:** execution budget exhaustion at `0x801F3D80`
after 1,941 completed game-loop iterations. The next useful milestone is to
obtain pixel scanout and verify the resulting frame against expected intro
output. The focused regression tests now run and pass; no visible intro frame
has yet been verified.

## 2026-10-18 - Scene cleanup, reload and export-guided coverage

The original scene cleanup entry and destructor callbacks are now discovered.
After original OSUnlink succeeds, run-scoped scene bindings are removed at a
validated DOL block boundary so the next original OSLink can register the scene
again. Dispatcher regressions reject unregistering the current module while
allowing an inactive module to be removed from that boundary.

Production smokes subsequently reached missing scene switch blocks at
`0x803A5014` (REL file offset `0x46074`) and `0x803618C4` (offset `0x2924`).
Both observed blocks were added as discovery roots, without replacing their
original instructions or forcing scene state. The next run reached the DOL
constructor at `0x800E359C`; its assembly-bounded range is now translated.

The user's local NWiiRecomp export was inspected read-only. Its function bounds
independently agree with `0x800E359C..0x800E36E8`,
`0x80184EC8..0x80184F4C`, and `0x800E3948..0x800E3958`. These last two
helpers are now included after checking the original assembly. No export
runtime or generated source was copied into the port.

The last completed production smoke stopped at `0x800E3948`, LR `0x800E3668`,
after 2,049 complete game loops and 205 resource-read notifications; both CLZ
decodes returned. Evidence:
`diagnostics/smoke-0c38d2df378d415e934d8ba8a8d5f37f/startup-1792351170522417-0/report.json`.
That four-instruction helper has since been translated. The following smoke
was interrupted before producing a report, so the next production blocker is
not yet confirmed. Correct intro rendering remains unverified.

Final Release solution build succeeded. Focused diagnostics/startup tests
passed 2/2 in 1.71 s; generated-code/dispatcher tests passed 1/1 in 21.63 s.
Earlier in this iteration the full suite passed 14/15, with the dispatcher
fixture failing; that fixture was corrected and its targeted test then passed.
The full suite was not rerun after the final coverage additions.
