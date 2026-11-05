# The Legend of Zelda: Ocarina of Time (Switch 2)

Ocarina of Time (0400486024DEC000, v131072) stopped four times between boot and
gameplay. Each stop is fixed below, in the order the game reached it; the user
confirmed on the console that the game now boots, plays its movies and renders.

## 1. The fourth GPU queue never ran (no frame, CPU polling a zero word)

The game spun on a word in its first two-page semaphore region (0x210622A030 in one
run; the address moves between runs, the offset 0x30 does not). Each of its four
queues releases 3 and then 7 into its own 16-byte slot of the region's second page at
start-up; channels 1-3 did, the fourth never executed a single command of its own.

The submit snapshot (`EDEN_NX2_PB_SNAPSHOT`, on by default) copies the command words
through the channel's address space when the game submits them. Switch 2 memory
reaches the address spaces later, from the GPU thread (the registry's listener
calls), and Ocarina registers the fourth queue's command memory just before its first
submits: the snapshot read zeros (`gpu_mapped=0`, first word 0 against `0x20012000`
in guest memory), which execute nothing.

`nvhost_gpu.cpp` now reads an entry the channel's address space does not fully map
yet through the mapping the game made (`Nx2GpuMemoryRegistry::TranslateRange`, a
lookup in the registry's own table, which changes as the game maps). Every other
entry is read as before.

The puller also mirrors a release that lands in a region's second page into its first
again (`Nx2SemaphoreMirror`). The lookup was removed in Eglantine 60ef6d2750 while the
region table that feeds it stayed; Ocarina's CPU polls and its channels' acquires
read the first page, which nothing else writes.

## 2. sead::MainThread jumped to address 0 (the load restarted)

About 12 s in, the game opens `auddmg:u`, which Eden did not have: the placeholder
session's empty reply to command 0 left the SDK a null proxy (sdk+0x2ACDD0 reads its
vtable from 0 and calls through it). `nx2_audio_device_manager.h` adds the service
after Lichii (zortos293/Lichii): command 0 returns a virtual device manager with one
stereo 48 kHz device. Lichii's sound mixer (`audsmx:u`, sysmix) is not ported; stream
attach and detach succeed without routing.

## 3. Movies were black with sound (AV1)

The movies are AV1 (`FFmpeg has no decoder for codec 10`). The PS5 FFmpeg now
decodes AV1 through dav1d 1.5.1 (BSD-2-Clause, pinned in `tools/deps.json`),
cross-built with its x86-64 assembly by `tools/build-headless-dav1d.sh` and linked
after libavcodec (`headless/CMakeLists.txt`); its notice ships as
`licenses/dav1d-BSD-2-Clause.txt`. ProsperoEden-Encore has no AV1 decoder either.

## 4. Gameplay frames were black (rejected compute shaders)

Ten compute programs were rejected while the game set up its passes:

- seven on 0x8A5, uniform IMAD.WIDE with an immediate (a divide by a constant,
  `UIMAD.WIDE.U32 UR4, UR6, 0x88888889, URZ`). It now runs as 0x825 on the uniform
  registers' scratch pairs, like 0x8A4 and Lichii's lowering.
- three on 0x23C, HMMA.16816.F16, which only had a cooperative-matrix path the
  PS5's RADV lacks. Without one (`EDEN_NX2_TENSOR=fast` on a capable driver keeps
  that path) it now expands in Maxwell instructions, Lichii's inline form: SHFL.IDX
  fetches the A and B halves each lane needs, FP32 FFMA accumulates, and the sums
  round to FP16 after each K=8 group, as the hardware does. K=8 (HMMA.1688) is
  accepted too.

## Kernel

`WaitForAddress` accepts arbitration type 3 (WaitIfEqual64), ported from Lichii
c9ebb5c3 (from Eglantine PR #567); it was rejected with InvalidEnumValue.

## Checks

`python3 tools/check-software-hmma.py` runs the HMMA expansion on a 32-lane
interpreter against RTX 4090 reference fragments (Lichii's test, after
Duck912/eden-fefw): K=8 and K=16, four destinations, with and without uniform
registers, other registers unchanged. 211,360 assertions passed; on the previous
lowerer all 16 cases are rejected. The uniform IMAD.WIDE case (Ocarina's
instruction, other pairs, signed, an accumulator pair against the 64-bit sum) is in
`ampere_integer_multiply.cpp`: `tools/check-imad-wide-carry-output.py` passed 13
cases (7,374,702 assertions); the new case fails on the previous lowerer. Both use
Clang 18 with address and undefined-behavior sanitizers and the local Catch2
libraries (`EDEN_CATCH2_BUILD` names another checkout's build).

The snapshot, service and decoder fixes were each found and confirmed from console
logs; there is no host test for them. No other title was retested.
