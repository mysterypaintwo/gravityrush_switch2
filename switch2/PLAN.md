# Switch 2 (Ounce) port plan

## Why this differs from the N64 ports

The PC port is not a source recomp. `src/probe.c` maps the PS4 game's own
x86-64 `eboot.bin` and `.prx` modules and jumps into them, and
`src/runtime_*.c` replace Sony's system libraries. The Switch 2 has an ARM64
CPU, so the game code has to be translated to ARM64.

## Chosen approach: embedded Box64 dynarec + the existing HLE runtime

The host program is ARM64: the loader, the runtime (`src/runtime_*.c`), the
shadPS4 video core, audio and input. Box64's ARM64 dynarec runs the game's
x86-64 code. When guest code calls a PS4 import, a Box64 "bridge" (an `int3`
stub plus a signature wrapper) calls the native C function in the runtime.

### Box64 vs FEX (both MIT, so compatible with GPL-2.0+)

| | Box64 | FEX |
|---|---|---|
| Language | C, like the runtime | C++20, jemalloc, fextl containers |
| Block linking | Jump table in **data** memory, so code isn't patched after it is emitted | Patches branches into **live code** (`ExitFunctionLink`) |
| Fit with kaiHEN JIT (staging buffer published by an IPC copy to RO) | Good: one publish per block, batched with `kaihenJitPublishRanges` | Poor: every link or unlink is a publish IPC |
| Calling host code | Bridges and signature wrappers (`wrapper/`), suited to HLE | Thunk libraries built with an x86 cross-compiler |
| Existing non-Linux backend | `src/os/os_wine.c`, `wine/wow64` | `Source/Windows/{WOW64,ARM64EC}` |
| Accuracy | Lower (game-oriented, tunable: strongmem, fastnan, …) | Higher, TSO-correct |

Box64 wins on fit. FEX remains the fallback if Box64's accuracy breaks the
game in ways its settings can't fix.

## Hard problems to settle early (on the console)

1. **Address space.** The runtime puts guest memory in
   `0x10_0000_0000–0xfc_0000_0000`, and it must stay below 1 TiB because GPU
   descriptors hold 40-bit addresses. Horizon is 39-bit (512 GiB), and the
   kernel decides where the heap and alias regions go. The PS4 user range has
   to be remapped into regions we can actually map at fixed addresses.
2. **Physical aliasing.** One memfd backs the 5,056 MiB direct-memory pool,
   so the same pages can be mapped at several addresses. Horizon has no
   memfd. Candidates: `svcMapPhysicalMemory` in the alias region, the
   code-memory alias trick that kaiHEN's RO uses, or a translation layer if
   the game rarely aliases. First measure how often GR really aliases (log it
   in `runtime_memory.c` on PC).
3. **Memory budget.** We need about 5 GiB of guest direct memory, plus
   flexible memory, plus the code cache and Vulkan. We have to measure what a
   kaiHEN game-role NRO actually gets.
4. **Faults.** shadPS4's `page_manager` and the runtime's fault recovery use
   `mprotect` and SIGSEGV. On Horizon these become `svcSetMemoryPermission`
   plus the libnx user exception handler. If that isn't allowed, use write
   tracking inside the dynarec instead.
5. **JIT throughput.** Publishing goes through an IPC copy, so measure its
   cost per KiB and per call. The design is a large code cache with batched
   publishes and no patching of published code.
6. **CPU budget.** The PS4 has 8 Jaguar cores at 1.6 GHz; the Switch 2 has
   about 6 usable A78C cores at around 1 GHz. The target is 30 fps.

## Probe results (console run 1, 2026-11-16)

- **Address space:** 39-bit (`0x8000000`–`0x80_0000_0000`). Heap region
  `0x46c4400000` (12 GiB), alias region `0x1eb5c00000`–`0x3eb5c00000`
  (128 GiB), stack region 2 GiB. Every region is below 1 TiB, so 40-bit GPU
  addresses work. The PS4 user range `0x10_0000_0000–0xfc_0000_0000` has to
  shrink into the alias region (128 GiB is plenty).
- **Memory:** 9,024 MiB total for the process. hbloader gives 8,889 MiB of it
  to the heap, and the largest malloc was 8,640 MiB. Six cores (mask `0x3f`).
- **`svcMapPhysicalMemory`:** the process has 64 MiB of system resource, so
  the call is available. It failed with `0x10801` (LimitReached) only because
  the heap held the whole budget. Plan: override `__libnx_initheap` (it is
  weak), keep a smaller host heap with `svcSetHeapSize`, and map guest memory
  at fixed alias-region addresses.
- **kaiHEN JIT:** 512 MiB works, and generated code runs. A full publish takes
  190 ms (never do it). A range publish costs about 24 µs plus 1.6 µs per KiB
  (1 range 0.024 ms, 16 ranges 0.064 ms, 256 ranges 0.43 ms), which is fine
  for batched per-block publishes. The staging buffer is extra RAM about the
  size of the cache, so size the cache at 128–256 MiB.
- **Exceptions:** libounce resumes at `__libnx_exception_returnentry` and calls
  `svcBreak` if the handler returns. A recovering handler has to restore the
  context itself, and that costs one scratch register (`x16`, see
  `probe/source/resume.s`). libounce also uses **one global exception stack
  and dump buffer**, so the port needs its own exception entry before faults
  can happen on several threads at once (shadPS4 page tracking).
- Run 1's `svcMapMemory` and fault-recovery results were probe bugs. v2
  retests them and adds transfer-memory aliasing, a mapping of the 5 GiB pool,
  and a clock and 6-core measurement.

## Probe results (console run 2)

- **ASLR:** region bases change on every launch (heap `0x7bb4200000`, alias
  `0x331ba00000` this time), so the guest layout has to be computed at
  startup.
- **CPU:** about 995 MHz per core, and all six cores run in parallel at full
  speed. That is roughly 60% of a Jaguar core's clock, with better IPC.
- **`svcMapMemory`:** works into the stack region, and the view reads
  correctly.
- **Heap:** hbloader's heap override starts 64 MiB into the heap region and
  isn't 2 MiB aligned. Shrink from the heap's real top.
- **Faults:** the process died on a fault that the handler should have
  recovered with longjmp. Runs 1 and 2 can't distinguish "exceptions not
  delivered" from "recovery broken", because the handler never logged on
  entry. v3 logs on entry and runs the fault tests last.
  - If faults can't be delivered: turn off Box64's SMC protection (the game
    doesn't modify its own code), use Box64's in-code alignment checks for
    atomics (the A78C has no LSE2), and replace shadPS4's page-fault write
    tracking with dynarec-emitted write tracking or content hashing.

## Probe results (console run 3)

- **Heap shrink works:** `svcSetHeapSize(2114 MiB)` succeeded, used memory
  dropped from 9,020 to 2,180 MiB, and malloc keeps working. That should leave
  about **6.8 GiB** for fixed-address guest memory. This is untested:
  `svcMapPhysicalMemory` hasn't run since the heap was shrunk.
- **`svcMapMemory`:** the source becomes perm 0 / attr locked while it is
  mapped, so it's not an alias usable from both sides.
- **Transfer memory:** the run died after `tmemCreateFromMemory` (owner
  perm none) succeeded and before `tmemMap` returned. The handler, which logs
  on entry, never logged in runs 1–3. The screen showed the standard
  "software needed to close" error.
- **Exception path:** kaiHEN's ounce-hbloader is the process's main module,
  and its `__libnx_exception_entry` (switch2/entry.S) jumps to the running
  NRO's entry. Our crt0 dispatches that to `__libnx_exception_entry`, so the
  chain looks complete on paper. v4 opens with a single guarded fault to tell
  "never delivered" apart from "the tmem crash isn't an exception".

## Probe results (console run 5, v5)

- Both copies of the log come from the fault path. The hold-B check never saw
  the button, so "run B" was a second run A. v6 uses an on-screen menu.
- `svcSetMemoryPermission(heap page, none)` succeeds. The read after it ended
  the process right away, with no 5 s delay and no "handler entered", so the
  exception **never reached this NRO's entry**. It was either sent to a
  debugger, not delivered to user mode, or dropped by the loader's entry.
- v5 read `InfoType_DebuggerAttached` with the process handle, which is
  invalid for that type, so it always printed -1. v6 queries it correctly and
  saves the loader's code so its exception entry can be disassembled.
- Planning assumption until shown otherwise: **no user-mode fault recovery.**
  Use the fallbacks listed under run 2 (no SMC protection, in-code alignment
  checks, dynarec-emitted write tracking for shadPS4).

## Probe results (console run 6, v6)

- **Debugger attached: 0.** The kernel isn't routing faults to a debugger, so
  the remaining suspects are user-exception delivery itself and the loader's
  entry (`ounce-hbloader`, the only static code module: text 0x14000 at the
  process entry). The NRO itself runs at `0x8000000`, the bottom of the ASLR
  region, outside any module the kernel knows about.
- The v6 fault run ended the same way as v5: the log stops at "reading it now".
- **Guest memory works.** After `svcSetHeapSize(2114 MiB)`,
  `svcMapPhysicalMemory` mapped 16 MiB, then the **whole 5,056 MiB PS4 direct
  pool** at a fixed alias-region address (used memory 2,140 → 7,196 MiB).
  `svcSetMemoryPermission` read-only/rw works inside it. The largest single
  physical map is **6,912 MiB**. This settles items 1 and 3: guest direct
  memory goes at fixed alias-region addresses computed at startup.
- `jitCreate(128 MiB)` adds no used memory at creation (pages come later).

## Exception delivery: settled (run 6 code dump)

- ounce-hbloader's entry is correct. On an exception (`x0 != 0`, `x1 != -1`)
  it branches to a stored pointer, which is the running NRO's entry. The
  loader sets that pointer right before it calls the NRO and clears it after.
  With no pointer set, it calls `svcReturnFromException(0xf801)`. The probe's
  crt0 dispatches to `__libnx_exception_entry` correctly too.
- The X run still crashed in under 2 s, so the 5 s sleep at our entry never
  ran: **the kernel doesn't enter user exception handlers at all.** The most
  likely cause is the stock kernel's "user exception handlers enabled" setting,
  which is off on retail units. Atmosphère turns it on, but kaiHEN runs on the
  stock kernel.
- **Decision:** the port never relies on catching a fault. Box64 runs without
  SMC protection, uses in-code alignment checks for atomics, and shadPS4's
  page tracking becomes dynarec-emitted write tracking (or content hashing).
  Any fault ends the process, so the runtime checks its pointers itself.
- No more fault probes are needed.

## Phase 1 progress: embedded Box64 (switch2/box64)

`switch2/box64/fetch_box64.sh` clones Box64 v0.4.4 into `third_party/box64` (ignored by git)
and applies `patches/box64-v0.4.4-embed.patch`. CMake builds the static library `box64e`
from the same core files as Box64's WoW64 build, with our OS layer (`os_embed.c`) and driver
(`box64e.c`, API in `box64e.h`). `box64e_smoke` runs hand-assembled x86-64 through it.

**Status (2026-11-18, Mac):** the smoke test passes with the dynarec and with the
interpreter: leaf code, loops, guest→native bridge calls (integer and float) and
native→guest callbacks from inside a bridge.

Decisions in the patch and the embed layer:
- **Guest r8 lives in x9, not x18.** macOS zeroes x18 on context switches (measured: a
  value set in x18 was 0 after a spin loop). x9 is caller-saved like x18, so Box64's
  register saving around native calls is unchanged. `TO_NAT`/`IS_GPR` and the three
  assembly files changed. Same mapping on Horizon.
- **Split JIT.** Code runs at its exec address and is written at exec + `box64_jit_wdelta`.
  On macOS it's a `mach_vm_remap` alias of one shared region (measured: works, the RW view
  stays writable while the RX view runs). On Horizon it will be kaiHEN's staging buffer.
  `dynablock_t` moved to the heap (Box64 writes its fields at runtime). Tables only C reads
  (instsize, arch, callrets, seps, relocs) are used through the write view. Each finished
  block is **published once** (header, code, table64, jmpnext): on Horizon that is one
  `kaihenJitPublishRanges` per block. The allocator's own marks stay in the write view.
- **No signal-based features.** `protectDB` doesn't `mprotect` (no SMC detection; the game
  doesn't modify its code), CALLRET is off (it patches live code and recovers through
  SIGILL), the dynacache is off, `setjmp` recovery is compiled out.
- **Host CPU features are the A78C's on both hosts**: ASIMD, LSE atomics, CRC32, AES/PMULL,
  SHA1/SHA2; no LSE2 (uscat), FlagM, FRINTTS, AFP, RNDR. The Mac then generates the code the
  console will run.
- **Bridges:** `int3 'S' 'C' wrapper fn ret`. `BOX64E_INT`/`BOX64E_FP` are "simple" wrappers,
  so the dynarec calls the C function directly with rdi…r9 in x0–x5 and xmm0–7 in v0–v7
  (AAPCS64 assigns integer and FP registers independently, like SysV). `BOX64E_STACK` adds
  stack arguments 7–12, and `box64e_bridge_emu` gives a function the whole guest state
  (varargs, odd ABIs).

Next: the ARM64 host program. Port `src/probe.c`'s loader (no x86 asm, no native jump into
the eboot) and the runtime to arm64 macOS. Every resolved import becomes a bridge, and every
place where the runtime calls a guest function pointer goes through `box64e_call_guest`.
The game data comes from the user's PKG: `scripts/pkg_extract.py --only eboot.bin --only
sce_module` (new option), then `prepare.py`, `link_libc.py`, `link_modules.py`,
`content_profile.py` and `patches.py` run unchanged on the Mac.

## Phases

0. **Console probe NRO** (`switch2/probe/`): print the address-space
   regions, the memory totals, which svcs are allowed (MapPhysicalMemory,
   SetMemoryPermission, user exception handler), and JIT publish timings. The
   answers decide items 1–5.
1. **ARM64 bring-up on the Mac** (M4 Pro, 16 KiB pages like the PS4):
   embed the Box64 core, make the runtime build for arm64 with bridges, and
   boot the eboot to its first unimplemented import. There's no GPU yet; lldb
   makes debugging the dynarec far easier than on the console.
2. **Horizon backend:** `os_horizon.c` for Box64 (kaiHEN JIT, memory,
   threads, exceptions), plus a Horizon memory layer for `runtime_memory.c`.
   Reach the same point on the console.
3. **GPU:** the shadPS4 video core on OunceVK, plus a window/present layer
   (replacing SDL3).
4. **Audio, input, saves and file I/O:** audren/SDL2, HID, the SD card.
5. **Performance:** Box64 tuning, a cached code cache, CPU core affinity.

## Game data

No game files (eboot.bin, .prx, PKG contents, saves) go into the repository
or a release. Players supply their own unpacked CUSA01113 01.00 folder, as on
PC. The development machine has little free disk, so extract only
`eboot.bin`, `sce_module/` and `sce_sys/param.sfo` for phases 0–2.
