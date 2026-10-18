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
build\native\native\recompiler\Release\hmawl_original_startup.exe --original-startup build\game-us-r0\main.dol --rvz "Harvest Moon - A Wonderful Life (USA).rvz" --max-blocks 50000000
```

The extracted `wlp1.rel` must be beside the DOL. The scene generator verifies its
USA SHA-1 and emits native C++ locally, including for `BUILD_TESTING=OFF`.

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
consistent with the 40.5 MHz time base. VI uses the NTSC 27 MHz clock selection at
`0xCC00206C`; the disconnected digital-cable pins read zero at `0xCC00206E`.
Neither register readback nor beam timing implements pixel scanout or a renderer.
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
native handoff contract, not DSP-ROM emulation. The same explicit contract supplies
a native task-loader service: its documented greeting is not counted as executed
ROM/task instructions or completion. Five ordered command/value pairs configure
RAM source, IRAM word destination, byte count, zero initial DRAM upload and entry.
Missing compiled bindings, malformed requests and changed original task bytes fail
before committing IRAM or final-mail consumption. The original 6624-byte task is
copied into 4096-word IRAM; task registers start at explicit profile-zero values.
The native loader does not send the task's init message or assert its interrupt.
Those stores execute from the statically compiled original microcode. Its DSP IRQ
uses PI bit `0x40` and runs the original installed CPU handler and task-init callback.
No DSP ROM is executed or supplied.

The ARAM controller also exposes checked configuration, readiness and refresh,
plus halfword address/count assembly; the count-low write triggers DMA. Transfers
are synchronous: after the validated byte copy, addresses advance, the count
clears and ARINT latches until W1C acknowledgement. The explicit disconnected-HSP
profile has no expansion storage. A DMA read from that absent device leaves the
destination bytes undriven/unchanged; writes have no recipient. Separate counters
report those accesses. This is a bounded no-device profile, not certification of
physical-console open-bus behavior. Internal/expansion-straddling transfers fail.

AI control, independent stereo-volume bytes, sample counter and interrupt timing
are mapped as aligned 32-bit registers. The NTSC clock uses the integer rational
108 MHz / 2248 or / 3372, relative to the 40.5 MHz time base. Pause, rate changes,
fractional ticks, reset, counter wrap, threshold crossing, interrupt inhibition,
masking and W1C are stateful. AI sample interrupts use PI bit `0x20`, distinct from
DSP's `0x40`, and route through the original guest exception/AI handler. A separate
threshold-crossing test verifies the original handler acknowledgement, SDK mask
restoration, exception return and complete interrupted CPU register preservation.
This clock produces no PCM data or sound. Disabled PCM-DMA address/block registers
retain configuration; enabling playback explicitly fails without a PCM backend.
The Windows CLI now binds the actual output backend described below.

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

The separate bounded task compiler follows static branches and emits native code
with per-instruction/operand integrity guards. Task register/status moves, signed
40-bit accumulator transfers with saturated reads, checked DRAM/mailbox stores,
conditional polling branches and HALT are supported. A 256-instruction cooperative
quantum and backward-wait yields let the original CPU service mail/interrupts;
this is not cycle-accurate DSP timing. CPU HALT pauses rather than restarts a live
task; reset clears the live protocol, and the explicit loader profile starts fresh
parameters/registers, with WR0..3 at their reset value `0xFFFF` (linear addressing).
Unsupported instructions, explicit stack-register accesses, task memory or
mailbox overwrites fail explicitly.

The original command header comparison, separate pointer consumption, command-list
DMA and instruction-memory table dispatch now execute natively. The compiler accepts
explicit build-time indirect roots from the original 19-entry command table and
four-entry scheduler-control table, not a runtime instruction decoder.
Instruction-data reads also check the compiled image.
Signed-40 arithmetic/status, accumulator comparison/negation/shift, indexed and
parallel transfers, native call/return and single/block loops execute the original
initialization and subsequent parameter preparation. Signed-32 AX addition/moves,
unsigned AX-low pointer addition, shift-by-16 and parallel dual loads now execute
the additive sample loops and stereo output interleave. Dual loads preserve the
same-1-Kiword-bank bus behavior; stores use pre-main-operation values.
Unsupported conflicting parallel accumulator writes fail explicitly.
Loops preserve remaining counts
across the 256-instruction quantum. Calls/loops share a conservative combined
four-entry bound; explicit stack-register manipulation, nonlinear addressing and
control-flow instructions at loop endpoints fail explicitly.

DSP-side DMA uses actual main RAM/DRAM in either direction, preserves big-endian
word order and validates the whole even-byte range before copying. This profile
is synchronous: control has no busy bit after the copy, and byte length reads zero.
This is not a cycle-timed DMA engine. Instruction-RAM replacement and DSP ROM
branches remain explicit dependencies. No DMA injects task mail or completion.
Initial silent command lists now execute their output DMA and send their own
`0xDCD10002` completion mail/IRQ. The original CPU handler responds with
`0xCDD10003`; the task's scheduler-control dispatch returns to word `0x4A` to wait
for another list, rather than halting or synthesizing completion. The deterministic
disc fixture verifies 133 lists, 4223 data transfers/1280140 bytes, 409/409 CPU mails,
136/136 DSP mails and 134 task IRQs (including task initialization). Production Windows
counts vary with host time. This does not implement general voice mixing, multiply/
accumulate or ADPCM; initial output buffers remain silent.

### Windows PCM output and DMA

On Windows, the CLI binds a lazily opened default `waveOut` device, not a silent
sink. Device/open/prepare/write/reset/queue errors propagate to the runner.
Packets retain their signed-16 stereo sample storage and prepared header until
Windows marks them done; stop/exit resets the device and releases its packets.
Cleanup failures are reported. A full 256-packet queue fails rather than discarding
an incoming buffer. Other hosts and the default library state have no
output backend and retain the explicit AIStartDMA dependency error.

The native DMA profile snapshots a full programmed buffer from actual big-endian
RAM on start/reload, converts sample byte order, and commits enable/accounting only
after its output backend accepts the buffer. This is explicitly buffer-granular,
not certification of the hardware's intra-buffer memory-fetch timing. Native
remaining-length reads count 32-byte blocks (eight stereo frames); the exact NTSC
clock advances a block per 10116 or 6744 time-base ticks, selected by AIDFR.
Start/stop, preserved pending configuration, reload, independently latched DSP
DMA IRQ, masks and W1C are stateful. The Windows format uses the closest integer
rates, 32028 or 48043 Hz; there is no sample-accurate host/DSP synchronization claim.
Live rate changes, missing/failed backends and excessive catch-up fail explicitly.
This is not yet certified glitch-free continuous playback.

Actual DMA periods enter the original AI DMA handler at `0x801C1BC0` and original
buffer callback at `0x801F2AB8`. Original ARAM/ARQ transfers also run alongside the
compiled task, with real copies and original completion callbacks. The cache-sync
`sc/rfi` inside the audio callback preserves the outer external-interrupt state;
external interrupts can also nest inside the original enabled handler, up to four
active levels. Each level retains its actual resume/MSR metadata and original
guest context restoration; handler reentry cannot be mistaken for a native return
sentinel. Vector-phase, system-call-phase and excessive nesting fail explicitly,
and all levels share the dispatch block/depth budgets. Arbitrary thread switching
inside nested interrupts remains unsupported. Unnested IRQ returns can resume
the original SDK-selected thread after validating its saved OS context.
Indexed single-float transfers and signed high multiplication execute the original
audio parameter/command preparation. The resulting CPU mail is the actual
`0xBABE0024` header, not an injected test command or synthesized DSP completion.
The original audio effect callback now runs through raw-low-word `stfiwx` stores.
The first game-system initialization returns to main at `0x8000AA6C`, followed by
eleven original startup objects and a real DVD resource read. The original loop
enters at `0x8000AC34`; preparation returns through `0x8000AC64` before the
SystemTask transition at `0x8017755C`. It now loads and links the original scene REL,
executes all 28 module constructors and enters mode-41 scene initialization.
Manager initialization, resource loading and SystemTask now return. All three
original task-list passes execute, including the first scene/asset update and
fade registration. Deferred audio/camera updates, queue cleanup and Screen frame
preparation now return. The kernel schedules its original render tasks; all eight
render callbacks, render queue cleanup and frame draw-sync submission now return.
The first complete main-loop iteration now executes its original post-render
lookup, retrace gate, framebuffer exchange, timing and controller update and takes
the backedge. The second loop completes its task passes and reaches its original
textured quad. Its native renderer now produces EFB pixels and a Windows EFB
preview. The original loader callback `0x8019A6BC`, CLZ decoder and post-render
transition callback now execute as production translations. The captured later
texture LOD state and out-of-depth geometry are supported; a production run now
reaches the unchanged execution budget inside the original CLZ decoder.
Initial PCM buffers are original zeros; Windows acceptance is not proof of audible
game audio, completed DSP mixing, a game window or a rendered frame.

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
stages retain shared dispatch budgets. A system call from an already-entered external
handler is supported separately; nested external interrupts and arbitrary thread
switches fail explicitly. This is not an arbitrary MMU: other BAT mappings,
protection regions and unsupported interrupt/context-switch paths remain explicit
dependencies. Native MI setup
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
expiry enters the original DEC vector and SDK alarm handler.
Original completion cancels the DVD alarm; no timeout is ignored.
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
from the real arena (`0x8035EFA0` to `0x81701FE0`). Eleven original allocations,
including a nested `0x1730` resource allocation, `0x60000` FIFO buffer and two
640x448x2-byte framebuffers, complete.
The original Screen enters GXInit and prints the SDK's GX version. Original
CP/PE setup, FIFO linking, write gathering, matrix stores, viewport/projection
arithmetic, texture-region callbacks and PE poke configuration execute.
**Original GXInit returns to Screen at `0x8017A41C`**, with SP `0x8035CF28`
and the original FIFO object `0x80318B20`. **Original VIInit returns at
`0x8017A42C`, VIConfigure at `0x8017A4C0`, and VIWaitForRetrace at `0x8017A674`.**
The real guest VI interrupt handler acknowledges the timed display source and
wakes the waiting original thread. Original OSSaveContext/OSLoadContext restore
the saved scheduler continuation at `0x801B6F80`; native dispatch unwinds the
abandoned C++ guest-call chain, not the guest's saved stack. The original current
context's PC/MSR/SP are checked, and the dispatch budget and code-integrity checks
remain in force. This is not a replacement scheduler; interrupt-time thread
switches beyond restoring the interrupted context remain explicitly unsupported.
**Original Screen construction returns at `0x8000ADDC`, SP `0x8035CF68`.**
Its first actual EFB copy writes a 640x448 YUYV image into the second original XFB
at physical `0x0157B160`, stride 1280, then clears the EFB rectangle to RGB 64/64/64
and depth `0xFFFFFF`. All 286720 copied pixels are still undefined power-on contents:
the copy occurs before the clear. This is not a rendered or presented game frame.
The original game continues through PAD setup, array constructors, collision-reset
vtables and kernel/camera calculations. Original vector subtraction produces
`(0,-20,-200)`; its original squared-length calculation produces exactly 40400.
Three original `PSVECNormalize` calls return, storing the actual unit vectors
`00000000 BDCBC89C BF7EBAC3`, `00000000 3DCBC89A 3F7EBAC0`, and
`3F800000 80000000 80000000`. Original look-at calculations continue, and
Original ARInit, ARQInit and AIInit subsequently return. Their actual guest flags
are all 1; the original probe records 16777216 internal bytes and zero expansion
bytes, including the corresponding low-memory total. Five ARAM DMA transfers
include two reads and one write to the explicitly disconnected expansion.
Original AI calibration advances four sample-clock steps and resets the counter
five times in the deterministic regression. Volume is zero and final control is
`0x46`; in the no-output library fixture PCM DMA remains disabled with 20 blocks configured.
The audio-engine/voice setup reaches 178 real heap allocations without overlaps
or escaped arena bounds. Original `DCStoreRangeNoSync` executes checked `dcbst`
operations against coherent RAM.
Original DSPInit now returns through the explicit native loader handoff, followed
by original DSPAddTask after ten CPU mail words and one 6624-byte IRAM upload.
The original task clears its accumulators, initializes DRAM, writes its init mail,
asserts DSP IRQ and waits. The original CPU DSP interrupt handler and init callback
each execute once, setting the actual task-initialized guest flag; original
audio-task initialization returns at `0x801F0FC4`.
Without a PCM backend, the next dependency is `PC=0x801C165C`, LR `0x801F2CAC`, SP `0x8035CEC8`:
original AIStartDMA enables playback without a native PCM backend. This fails
explicitly before enabling DMA. The task remains running/waiting at DSP word
`0x4A`, not halted or completed. The only completed DSP program remains the earlier
initializer. Total DSP output/consumption is 3/3: the initializer's real message,
the explicit native loader greeting and the task's actual init message. The task
has asserted exactly one IRQ. No ROM bytes or task-ready/completion messages are
fabricated. This no-PCM library profile retains its independently checked boundary.
With the real Windows backend bound, original AIStartDMA returns. Actual timed
reloads execute the original handler/callback and its cache-sync system call, then
send `0xBABE0024` and the separate pointer. Both are now consumed; the original CPU
submission returns while the DSP executes initialization, additive sample loops,
output interleave/DMA and its own completion mail/IRQ. The original nested CPU
handler replies through the task's scheduler-control table.
A measured runner execution returns from the first game-system initialization
and, with `--max-blocks 50000000`, executes eleven post-system-init constructors
and a real 128-byte resource read. Its original main loop enters the first iteration;
reset dispatch, update-mask calculation and resource/audio preparation return.
SystemTask now loads and links `wlp1.rel` through the original SDK `OSLink`.
The relocated prolog executes its 28 constructors, loads scene assets and returns.
SystemTask and the three original task-list passes now return. Deferred
audio/camera updates, queue cleanup, Screen frame preparation and kernel render
scheduling return. All eight scheduled render callbacks, render queue cleanup and
frame draw-sync submission return. The original post-render lookup, retrace gate,
framebuffer exchange, timing and controller publication now return and execution
takes the original backedge. One complete main-loop iteration is verified.
The second loop renders the original textured quad and fade. Production now
continues through the original loader worker and post-render transition callback.
A Windows run reached 688 complete loops, 787 quads and 690 EFB-to-XFB copies
before its unchanged 50-million-block budget expired inside the original CLZ
decoder (`PC=0x80178458`, LR `0x8017841C`, exit code 4).
DSP PC `0x4A` waits for the next list.
The default one-million-block limit can expire earlier.
Actual RAM frames are submitted at 32028 Hz. Steady-clock counts depend on
host elapsed time. Host timing can also expose other untranslated startup paths.
No mixed or audible
game audio or VI pixel scanout is claimed. The native EFB preview is not scanout.
The inquiry result remains `0x8001` and the entire real FST is intact.
The no-PCM deterministic regression uses 1000 ticks per clock sample when AI is stopped,
and 8 while its sample clock is running, resolving the SDK's sub-sample calibration
window without modifying guest code or injecting a calibration result.
It executes exactly 17507 total DSP steps, returns 170 external exceptions
(including 159 original VI handlers), advances 239 VI fields and restores one
scheduler context. EE is enabled and no VI source remains pending at the
PCM-enable boundary. Steady-clock runner field/interrupt
counts depend on elapsed host time, not a forced completion.
Real-disc tests verify every constructor in original table order and preservation
of its stack, SDA and nonvolatile integer/floating registers. Traced and untraced
startup produce identical RAM, CPU state, AI/DSP/ARAM state, output and sampled time; damaged
constructor words fail integrity validation without claiming later milestones.
The separate deterministic PCM fixture uses 8 ticks while either AI sampling or PCM
DMA is active. 133 actual timed DMA periods execute 133 original handlers/
callbacks and 133 complete DSP lists. It reaches original scene-manager initialization
with 21440 submitted silent frames, 409 CPU mail words sent/consumed, 136 DSP mails
sent/consumed and 134 task IRQs. The task is still running/waiting, not a completed
program; no active or suspended CPU interrupt remains. Observed and unobserved executions
match RAM/CPU state, DSP DRAM/registers, clock samples, DMA phase/counts and
interrupt/DSP steps. 4223 DSP data DMAs transfer 1280140 bytes; all 18 words of the
last command list match the actual CPU RAM source. The original CPU submission
handshake completes 133 times, and game-system initialization returns to main.
Eleven subsequent original object constructors return in the actual main-call order.
Tests verify the camera's actual vtable pointers and signed projection bounds for
the 640x448 screen. The resource read uses the original 13-status eligibility table,
low-level SDK read, SDK forwarding callback, game DvdFile virtual callback and
notification. Its full 128-byte DMA payload is compared with the actual RVZ bytes
in guest RAM before any resource processing; the original completed-read flag is
also checked. Original completed-read cancellation/close dispatch uses its own
13-entry table. Both tables have explicit build-time target guards. No native
replacement resource constructor or callback fabricates completion.
The final three constructors, two array elements, singleton/vtable state and three
handle initializations are verified. First-loop entry and six ordered preparation
blocks are checked through the original reset/update/resource/audio calls.
Corrupt late constructor code is rejected before reporting any loop entry.
The complete first-loop evidence below does not establish a GX renderer,
general scheduled game-thread execution, connected input or saves.

The original SystemTask update and transition are compiled from bounded original
ranges, keeping the 100000-instruction recursive discovery limit. Mode 41 requests
`wlp1.rel`; two genuine reads total 433440 bytes (128 initial resource bytes and
433312 aligned scene bytes). Original SDK `OSLink` executes, publishes the module
list, relocates sections and entrypoints and clears its separately allocated BSS.
Tests inspect all 15170 relocation records: DOL/self imports resolve to the actual
addresses, while imports from not-yet-loaded modules retain their original bytes.
OSLink success does not mean those other modules have been loaded.
Corrupt original OSLink code is rejected before the linked-module milestone.
The scene generator now compiles 1522 original instructions: the prolog, all 28
constructors, mode-41 scene creation, the connected state-24 asset setup case,
scene/asset update callbacks and the original seven-case asset-update switch.
Branches, PC and LR use the actual code allocation. For relocated immediates,
the operation/register selection remains statically compiled; only the operand at
its known instruction location is loaded after the dispatcher's exact word check.
Expected words are independently constructed from build-verified originals and
actual linked section addresses at registration, never accepted from a RAM snapshot.
Changing a relocated operand fails integrity validation, not runtime opcode decoding.
Registration is atomic and allowed only outside execution or at the validated
block-observer boundary. Native guest function bodies still cannot change bindings.
The current block is revalidated after the observer; changing its code or PC
cannot bypass the relocation guards.
Run-scoped scene bindings are removed on stop/failure, so subsequent fresh runs
cannot reuse stale addresses. General OSUnlink/reload integration is not implemented.

The original prolog now executes all 28 constructors in table order, preserving
stack/SDA and nonvolatile GPR/FPR state. Two alternative code/BSS placements are
linked by the actual SDK OSLink and execute the same constructors, including HA/LO
carry and separate BSS addressing. Tests check the constructor's DOL-to-module
data copies, atomic rejection of damaged code and post-binding operand guards.
Synthetic compiled tests cover relocated addi/addis, logical and shifted logical
forms (including correct high-half `andis.`), integer loads/stores and single-float
transfers at two bases with zero, signed-boundary and all-ones operands.
Original mode-41 scene/asset construction and resource loading now return.
All 31 DVD reads (8819232 bytes) are checked against actual RVZ bytes in guest RAM.
Original integer formatting prepares resource paths; original queue workers,
DVD/ARAM completions and ARCInitHandle publish a real archive/file table.
Tests verify the state-24 scene, 52 original loader-worker exits and a drained
queue. Bounded function ranges and checked original switch tables keep the
existing code-discovery limit unchanged.

DEC expiry enters original vector `0x900`, the SDK handler and alarm callback.
Writing zero expires after one tick; pending masked expiry is retained and
missed 32-bit wraps coalesce. Tests cover both directions of timer/external
nesting, budgets, damaged vectors and invalid thread contexts. MSR ME is retained
as context state, not implemented machine-check delivery. An unnested interrupt
can restore the exact original SDK-selected thread context and unwind the native
call chain without resetting its execution budget.
The deterministic full run verifies one timer exception/return and 54 thread
context returns. Observed/unobserved runs must match CPU/RAM, timer deadlines,
interrupts, PCM and DSP state. The module prolog returns at `0x80177C88`, followed
by the original post-load Memory check.

SystemTask's filter setup now returns to main, followed by the three task-list
passes at `0x8017CD7C`, `0x8017CDC0` and `0x8017CE04`. The original
`__ptmf_scall` executes the actual three-word virtual member-function descriptors;
no native replacement dispatch or injected callback completion is used.
The deterministic fixture verifies seven first-pass callbacks and eight in each
subsequent pass, in priority order, using the original `0x800` update mask.
The asset-update switch's seven targets come from checked section-5 ADDR32
relocations. The original asset changes state 0 to 1, starts its fade and registers
the fade task, which participates in the later passes. The task-list manager
toggles its active list; its constructor reset is checked separately before update.
The deferred queues contain the actual audio/camera and camera/scene subobjects
in sorted order. Both deferred callbacks now return: the audio listener publishes
its original orientation, inverse matrix and normalized volume, and the camera
publishes its active view and orthographic GX projection. The fixture independently
checks that inverse camera origin/direction map back through the source matrix and
that projection coefficients match the original 640x448 bounds and depth planes.
Original cleanup invokes both node callbacks, unlinks the nodes and empties the
update queue. Screen's draw-sync check and frame preparation return.
The original kernel inserts six render-state tasks; tests verify all eight resulting
entries, including camera and scene, in exact sorted order.
All eight original callbacks now execute, followed by eight original node-cleanup
callbacks. The six kernel callbacks apply alpha/blend/depth state, compute a
perspective matrix through the original tangent function (`0x8023DC28`), save and
restore the previous GX projection and restore the viewport. The camera callback
uses its original virtual dispatch; the scene-manager callback returns zero in the
original game code. No host tangent or replacement draw callback is substituted.
The fixture independently verifies every perspective matrix coefficient against
the original inputs, its six GX coefficients and perspective type, exact saved and
restored orthographic coefficients, ordered callback targets, all node links
cleared and an empty render queue. Injecting a failure at the first callback must
leave render completion, cleanup and draw-sync markers false and release native
scene bindings. Observed and unobserved CPU/RAM/interrupt/PCM/DSP states still match.
The original frame draw-sync submission returns at `0x8000ACF8`.
The accessor at `0x8000CC9C` returns the game's optional post-render callback
pointer (zero in this run); no replacement callback is installed.
Main's original retrace-count gate passes, then `Screen::UpdateFrameBuffer`
requests the copied alternate XFB through the original `VISetNextFrameBuffer`
and switches its next draw pointer back to the other allocation. This is a
framebuffer request, not proof of pixel scanout. The original timing update
increments the frame counter. Original `0x80213CE4` executes rumble maintenance,
SDK PAD sampling, the reset-switch wrapper, queued-sample processing and game
controller publication. All four PAD channels retain error -1 with zero
buttons/axes; no connected controller or native replacement PAD routine is used.

The fixture verifies these return sites in order, the actual retrace threshold,
copied XFB address, both persistent framebuffer allocations, next draw pointer,
VI request pointer, frame counter and published disconnected PAD statuses.
Only reaching the original `0x8000AC34` again after `0x8000AD3C` increments the
completed-loop counter; tail markers alone do not. A damaged backedge fails
code validation without reporting completion or leaking relocated bindings.
The first iteration completes and the second resumes with the same main stack.
It resets the camera's active flag, toggles the manager's list again, rebuilds
the deferred queues and completes its three task passes, including the newly
registered fade's timed update.
First-loop queue/fade/camera assertions are captured before those second-loop
changes, rather than weakening their expected values.

The second scene update runs original `0x800E10C4` through `0x800E11CC`,
registering six global render-state nodes. The RGBA clear color is white.
Original `0x800E34E4` selects the resource-backed sprite's render phase and
virtual node accessor; `0x80188D58` interpolates each fade channel and schedules
the fade's render node. The kernel merges these with its six tasks and the
camera/scene nodes into a 16-node queue. All six task-list passes across the two
iterations return. The second render pass reaches the sprite after its original
blend, depth, clear-color and textured-vertex-state callbacks.

The sprite callback traverses the actual loaded resource hierarchy through
`0x801816C4`, `0x801818C4` and `0x80181A38`. Original paired-single matrix scale
and translation execute before `GXLoadPosMtxImm`. The game selects disc texture
index 2 (431x147 RGB5A3), constructs its SDK texture object and resolves the original texture-region
callback. Complete SDK vertex-descriptor and attribute-format jump tables are
selected rather than replacing their indirect branches. The original quad has
direct signed-16 XY, RGBA8 and float ST attributes, format 0 and four vertices.

Native GX now retains incomplete opcode `0x80` packets until all four vertices
arrive through actual 32-byte gather bursts. The checked native software renderer
supports the observed unlit, orthographic, single-stage UI profile: direct
signed-16 XY/RGBA8/float ST with tiled RGB5A3, or direct signed-16 XYZ with
material color for the fade. It applies matrix/projection/viewport, culling,
top-left coverage, scissor, texture wrap/filtering, TEV modulation, alpha/Z24
tests and source-alpha blending. Unsupported profiles fail before any pixels
or draw counters are changed.

The later observed 359x127 texture uses image word `0x51F966`, LOD word `0x800`
and mode `0x190` or `0x1F90`. Nonzero LOD limits and signed bias are supported
when minification/magnification both use the same nearest or bilinear base-level
filter: neither can select a different mip level or filtering kernel. Mipmap
filters, differing min/mag filters, anisotropy and reserved bits remain rejected.
Texture rejections now report the actual image/address/mode/LOD/scale words.

Orthographic geometry is clipped to projected GX depth `[-1, 0]` before
rasterization rather than rejecting vertices outside the viewport's depth range.
Both constituent triangles retain interpolated ST, RGBA and viewport depth at
new clip vertices. Fully clipped primitives consume their FIFO packet and count
as submitted quads without pixel or depth writes. Disabled XF clipping is outside
the supported profile and fails explicitly. Pixel fixtures verify both planes,
exact boundary inclusion, clipping across both planes, shared-edge coverage,
interpolated textures/colors/blending and Z24, plus unchanged base-level pixels
under LOD limits/bias and atomic rejection of unsupported texture state.

The later timing-dependent pipeline rejection was caused by requiring reset fog
coefficients even when fog is disabled. The original SDK fog setter at `0x801D1978`
stores type in BP `0xF1` bits 21..23; type zero disables fog. Its retained A/B/shift/C
coefficients (BP `0xEE..0xF1`), projection selector and RGB color (BP `0xF2`) then
have no pixel effect. Captured disabled profiles include `EE=0x7F800`,
`EF=0x7AE165`, `F0=0x1E`, `F1=0`, and `EF=0x40000E`, `F0=1`, `F1=0`.
Validation now checks the fog type, not these inactive values. This is not enabled
fog support. All seven nonzero type encodings still fail before EFB mutation
or primitive consumption. Pixel tests send real FIFO BP updates for captured and
extreme inactive values, verify their retention and exact textured/fade EFB parity,
and verify rejection/retry for every nonzero type. Pipeline errors now include the
actual checked BP/XF/CP words rather than only a generic category.

After this fix, two Windows production runs reached the unchanged 50-million-block
budget with 2186194 and 2215054 decoded output bytes, and 688 and 675 XFB copies.
A different timing stopped at `0x801D1A44`, LR `0x80182030`, because PowerPC
non-IEEE denormal result arithmetic in the original fog coefficient normalization
is still unsupported. No coefficients, callback completions or game operations
are fabricated to bypass that CPU dependency.

The disc test stops at the first second-pass cleanup dispatch, before the final
fade packet has drained from write gathering. It verifies all 16 render callbacks,
the 431x147 sprite's 63357 pixel writes, original texture/matrix bindings and
CPU/RAM/hardware parity against an unobserved run stopped by the same block budget.
Synthetic pixel tests also verify transparency, tile boundaries, coverage/seams,
scissor, depth, blending, partial FIFO packets and atomic draw rejection.

On Windows, `--display window` presents the actual EFB using GDI. This is the
default with `--rvz`; `--display off` keeps unattended runs headless. The preview
uses the last copy's active height and visibly marks undefined EFB pixels.
It remains open after execution stops; close it to exit. The original license
screen has been visually verified in that window. The production path flushes
both sprite and fade and continues through repeated original loops and XFB copies.
This preview does not claim VI framebuffer scanout or a complete playable boot.

The production manifest now includes the complete original worker at `0x8019A6BC`,
CLZ header/getter and block/stream decoder at `0x801782F4..0x801784EC`,
allocation wrappers at `0x80190EF4`/`0x80190F24`, and the original post-render
transition callback at `0x8000CE50`. No host decoder replaces guest execution.
Production-binding fixtures compare the original decoder with the independent host
CLZ reader for overlapping maximum-length references and padded, cross-block
references. They check exact output, input/output guards, stack and nonvolatile
registers. An empty worker fixture verifies its actual active-flag clear and epilogue.

A separate real-disc fixture stops on entry to the ninth iteration, after eight
complete loops, 24 original task passes, 14 quads and nine EFB-to-XFB copies.
Six post-draw copies are fully defined 640x448 buffers with 1280-byte stride;
consecutive destinations differ as the original framebuffer exchange runs.
CPU/RAM, deterministic timing, GX/FIFO/EFB and VI state match an unobserved run
at the same execution boundary. The real worker starts once in this fixture;
completion of its live resource decompression is not claimed.

The eight-loop fixture captures the actual worker's source/destination at original
decoder entry, without changing guest registers, scheduling or output. Its 5410544-byte
resource is independently decoded by the host CLZ reader only as a test oracle.
After eight loops the original worker has produced 19558 matching bytes and entered
41161 decoder blocks. Observed/unobserved runs retain identical progress counters
as well as CPU/RAM and graphics state.

An isolated fresh-state replay copies the captured RAM and calls the production-bound
original decoder with the same source/destination. It returns all 5410544 bytes
in exactly 15507310 native block entries, below the unchanged 50-million-block
bound. Every output byte matches the oracle; nonvolatile registers and stack pointer
are preserved, and every RAM byte outside the declared output and 72-byte stack
scratch region remains unchanged. No host decoder substitutes for guest execution.
This replay is a decoder correctness/cost test, not a resumed game boot or a
completed live worker: flushing, freeing the compressed source, publishing the
entry and worker exit remain separate original operations.

The normal runner reports original worker decoder starts/returns and latest-resource
source, destination, declared output size, output bytes at completed-token boundaries,
and decoder block entries. Progress is sampled only in the original decoder's
matching stack frame/return path, so interrupt/thread register reuse is not counted.
It intentionally excludes an in-progress back-reference until the token completes.
Counts reset for each latest resource; decoder returns are not worker/publication
completion. Windows timing affects progress and can still expose unsupported
GX pipeline state before budget exhaustion.

At the earlier second-pass cleanup boundary the deterministic PCM fixture has 4210 completed DMA periods,
673760 submitted zero frames, 4207 original handlers and 4204 original callbacks/
command returns. CPU mail sent/consumed is 12622/12622, DSP mail 4207/4207 and
task IRQ count 4205; 134495 DSP data DMAs transfer 40817692 bytes.
The prior 31 DVD reads/8819232 bytes, 52 loader workers/exits, one DEC alarm
and 54 SDK thread-context returns remain unchanged. Observed/unobserved
CPU/RAM/PCM/DSP/interrupt parity is retained and GX/FIFO/EFB/XFB plus VI
register/timing parity is explicitly checked through this completed iteration.
The original first loop is complete, not a rendered scene. No runtime PPC decoder or
replacement linker is involved in the original startup path.
An independent `BUILD_TESTING=OFF` Windows build is used to verify the production path;
production execution does not depend on the test generator or fixture bindings.
General voice processing/
ADPCM remains an audio dependency, not a reason to substitute a replacement mixer.
The native loader, task startup, initial command output/completion and Windows PCM
backend are present.
Further original scene progression and XFB/VI pixel scanout are still required
for a complete displayed game frame; EFB preview alone does not establish one.
Generated translation registration uses non-inlined batches
of at most 64 regular bindings (plus the console binding in the final batch), avoiding
MSVC's oversized temporary stack frames without increasing the executable stack.

### Checked native graphics command pipeline

The native graphics profile holds CP control/status, split low/high FIFO registers,
PE interrupt enables/pending sources and token, PI CPU ring registers, WPAR and a
byte-accurate write-gather buffer. CP/PE PI enables require this profile; removing
it retains the previous explicit `0x801B3E00`/mask `0x8FC` dependency.
PE acknowledge bits are write-only: reading control returns enables, not pending
acknowledgements, so the original token handler cannot accidentally clear a
simultaneous finish interrupt.

Guest FIFO writes of one, two or four bytes assemble big-endian 32-byte bursts.
Each complete burst is actually written to checked guest RAM, advancing/wrapping
the CPU FIFO pointer. Linked CP consumption reads that RAM, tracks byte counts,
watermarks and breakpoints, and retains incomplete commands across bursts.
Disabled readers do not consume bytes; full/mismatched rings fail before
overwriting unread data. CP/BP/XF register loads, BP write masks, NOPs and
vertex-cache invalidation update explicit graphics state. Token/finish commands
assert their PE sources only after decoding, rather than reporting success early.
Draws, unsupported EFB copy modes, texture/TLUT DMA, display lists and unknown opcodes fail explicitly.
There is no replacement scene or generated frame.

The real Windows runner publishes **77 FIFO bursts** and decodes **404 register/NOP
commands**, with no pending command bytes, 15 still-gathered bytes and zero unread
CP FIFO distance at the audio-initialization boundary. A successful command prefix
is committed before the next command: retrying a rejected copy/DMA does not replay
an earlier token or copy/clear, nor stage the same RAM burst twice.
Original attribute/texcoord/texture-format tables supply bounded
build-time indirect branch roots, alongside original texture-region callbacks.
The six XF viewport words at `0x101A` after Screen setup are exactly `43A00000 C3600000 4B7FC000
44258000 440D8000 4B800000` (640x448 after original overscan adjustment,
original updated depth scale 16760832).

PE poke depth/blend/destination-alpha/alpha-test/alpha-read retain their actual
register state (`001F F038 0000 0700 0005`); no pixel operation is synthesized.
CP offset `+6` holds the performance selector, while `+0xE` holds the token.
Metric clear starts a new measured FIFO work interval without resetting lifetime
burst/command totals. Hardware clock/geometry metric reads remain unsupported.
Original paired matrix stores use bit-preserving
float transfers, including single-lane, immediate/indexed and update forms; integer
store quantization remains an explicit failure.

Scalar single/double arithmetic is evaluated with integer significands, guard/round/
sticky bits and a portable 53x53-bit product or long division, avoiding host FP
environment changes and single-result double rounding. Add/subtract/multiply/divide,
ordered/unordered comparisons and `fctiwz` preserve independent paired lanes or
duplicate single results as appropriate. Tests exercise all four rounding modes,
exact integer-bias subtraction, NaNs/infinities/signed zeros, sticky causes,
underflow/overflow, FPRF, integer bounds, register aliases and CR1 records.
100,000 deterministic finite input cases compare single and double results
against independent host IEEE arithmetic compiled with strict FP semantics.
Paired add/subtract/multiply/divide, `ps_muls0/1`, and `ps_sum0/1` use snapshot
operands so destination aliases cannot change the other lane. Both lanes are
evaluated before any architectural write: a rejected second lane leaves all FPRs
and FPSCR untouched. Paired FI/FR and exception causes combine both results; FPRF
classifies ps0, except `ps_sum1`, which classifies its sum in ps1. Horizontal-sum
copied lanes are bit-preserving moves and do not raise exceptions, even for sNaNs.

Scalar and paired multiply-add/subtract, including negative forms, use an exact
integer product/addend accumulator and round only once to their final precision.
Negative forms negate after rounding without changing NaN signs. The Gekko
single-precision multiplier is first rounded to a normalized 25-bit significand,
nearest with midpoint magnitudes rounded upward, independently of FPSCR RN.
The final result still follows RN. This includes non-single estimate operands;
replacing the intermediate with binary32 would lose the console's extra bit.
`frsp` supplies checked round-to-single conversion, preserving the undefined ps1
lane in this profile. `cror` supports the original camera comparison sequence.

Paired ordered/unordered comparisons select lane 0 or 1 and share the scalar
comparison's checked CR/FPSCR and NaN handling. Unselected lanes are untouched.
`fres` fills both destination lanes using the 32 measured reciprocal-ROM
base/decrement pairs, integer interpolation and the console's small/large-input
saturation thresholds, not an exact host reciprocal. Tests verify anchors, all
32768 interpolation steps at both tail endpoints with relative error below
1/4096, saturation endpoints, signed zeros/infinities/NaNs, aliases, CR1 and
unsupported-mode atomicity. Finite nonzero `fres` preserves FR/FI in this profile;
zero and special inputs clear them.

`frsqrte` uses integer normalization, exponent-parity selection and piecewise
linear interpolation of the 32 measured Gekko estimate-ROM base/decrement pairs.
The factual ROM data is recorded in Dolphin's
[FloatUtils.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Common/FloatUtils.cpp);
25-bit multiplier behavior is documented by its hardware-oriented
[FP utilities](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/PowerPC/Interpreter/Interpreter_FPUtils.h).
The native evaluators are independently implemented with integer significands:
no emulator runtime, host square-root substitution or copied emulator implementation
is linked. Tests check exact estimate anchors, every 11-bit interpolation step
of all 32 segments against the required relative-error bound of 1/4096, binary64
endpoint normalization, signed zero/infinity/NaN handling, flags and CR1.
FR/FI are architecturally undefined for estimates; this profile clears them for `frsqrte`.
20,000 strict host fused-reference iterations and 20,000 independent 25-bit
multiplier/conversion iterations complement rounding, alias and failure tests.
Original startup NI mode works for normal inputs/results; denormal NI behavior
and enabled FP exceptions still fail explicitly before mutation. Broadcast fused
forms and paired reciprocal estimates remain unsupported.

### Checked EFB clear and XFB RAM copy

An opt-in 640x528 software EFB stores RGB8/Z24 pixels and independent color/depth
defined bits. Cold backing values are deterministic `CD/CD/CD` representatives
with defined bits clear, not a fabricated clear or visible image. Production
reports copy/clear counts and undefined copied pixels explicitly.
The supported path copies non-AA RGB8/Z24 to checked Y0/Cb/Y1/Cr guest RAM using
limited-range BT.601 conversion, shared horizontal chroma, a seven-weight/three-row
vertical filter, source-edge clamp flags and unity vertical scale.
Copy precedes optional clear; original color/depth update masks determine which
source components become defined. Scaling, gamma changes, field/half copies,
other pixel formats, filter overflow, textures and AA remain explicit failures.
This limited conversion profile has not been certified as console-bit-exact for
arbitrary filtering/color inputs. Scanout remains separate; geometry is now
implemented by the bounded orthographic UI renderer described above.
The entire request, EFB dimensions, mapped destination and FIFO non-overlap are
validated before modifying guest RAM or pixels.

### Checked NTSC video timing and context continuation

The opt-in VI profile maps configuration, framebuffer and filter registers plus
four independent display-interrupt pairs. Reads are big-endian byte/halfword/word;
writes are checked halfword/word accesses, with rejected wide writes leaving the
whole profile unchanged. Reset clears interrupt pairs and scan state, retaining
programmed timing/filter coefficients. Display interrupts latch at programmed
beam crossings even when masked; mask bits gate PI bit `0x100`, and writing zero
to a status bit acknowledges only that comparator.

Only NTSC interlaced timing with half-line width 429 and the 27 MHz clock selection
is supported. The model advances at three 40.5 MHz time-base ticks per output pixel,
858 pixels per line and 525 half-lines per field. Dispatch samples the same clock
before each compiled block when display output is enabled; MMIO beam reads expose
that last sample. No host sleep, fabricated retrace callback or forced guest return
is used. Backwards time, elapsed-time overflow, invalid enabled comparators,
live scan-clock changes and other display modes fail explicitly.
PAL/MPAL/progressive modes, external beam latches and pixel scanout are not
implemented. EFB/XFB conversion is limited to the checked profile above.

`hmawl_native_vi_tests` verifies exact before/at/after comparator thresholds,
frame wrap, large jumps, masked latches/unmasking, independent acknowledgements,
read lanes and atomic invalid access/timing failures. Compiled CPU tests verify
signed division and nested thread-context RFI unwinding, saved-context validation,
shared entry budgets, callback cleanup and resumed-block code integrity.

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
complete original task IRAM bytes and guards, host/DSP mailbox counts, actual
task interrupt and CPU callback, running/waiting versus completed program state,
and corrupt-task rejection before IRAM upload or init callback,
original PCM start/remaining/reload/handler/buffer callback, real DSP header/pointer
consumption, exact command-list copy, 4223 data DMAs, 133 original list completions,
all-silent submitted samples, system-initialization return and next-constructor
dependency, system-call/nested-interrupt returns and complete observed/unobserved
PCM-path equivalence using an explicitly test-only output sink,
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
It also verifies original heap descriptors, the eleven named Screen-era allocation
return addresses and all subsequent observed allocator requests/returns (178 total),
requested sizes, 32-byte alignment, arena bounds, non-overlap, allocation count,
manager singleton pointers, the Screen's FIFO pointer and both XFB pointers.
It verifies exact CP/PE/ring state, burst/command counts and identical graphics
state with/without observation, including EFB color/depth/defined storage, copy
metadata/counters, actual XFB bytes and the exact clear rectangle/masks.
It also checks exact VI registers, elapsed timing, field counts,
original interrupt delivery and scheduler context-return counts. A separate injection
into the original GXInit-return snapshot, before VI startup, flushes the actual pending
matrix and submits token/finish FIFO commands.
Both original PE handlers run through the original OS interrupt vector/dispatcher,
acknowledge their sources, wake the original empty finish queue and restore all
interrupted GPR/FPR/CR/XER/LR/CTR/MSR state. Seven external exceptions then return.
This injection is test-only; normal startup additionally delivers real timed VI
interrupts, not injected PE token/finish events.
`hmawl_native_gx_tests` independently covers mixed-width gather, fragmented commands,
BP masking, XF bounds, ring wrap/full/guards, breakpoint resume, disabled readers,
watermark and PE interrupt state, and explicit unsupported command/access failures.
The synthetic compiled-code suite additionally covers immediate single-precision
loads/stores and update forms: signed zero, normal/subnormal values, infinities,
NaN payloads, paired-lane behavior, truncating stores, four-byte write guards,
RAM aliases, absolute addressing and transactional invalid accesses. These
transfers do not add floating-point arithmetic. Compiled paired-store tests cover
all eight GQR selectors, W/update and immediate/indexed variants, signed zeros,
normal/subnormal/NaN/infinity bits, RAM aliases and transactional invalid RAM/type
accesses. Integer-quantized stores remain unsupported.
