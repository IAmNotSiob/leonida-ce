# Boot hacks: getting `gta6rt --run` past the documented chain (Linux, Wine 9)

**Status: exploratory hacks, not pull-request quality.** Measured on one machine (Ubuntu 24.04, Wine 9.0,
3x RTX 6000 Ada, the Xbox Series 1.0.2.1 package) on 2026-11-23/24. Everything here is a stand-in to see the next
stop; most of it needs a real implementation. The scripts hard-code the paths of that machine (`/root/...`).

Some stand-ins use addresses inside the 1.0.2.1 executable (the watchdog flag, the exported allocation hooks
fallback). They only work with that exact version, which `make_game.py --check` enforces.

## Where it gets

About a minute into startup under Wine, past the first frame. The renderer is initialised: the device, queues, the 4K swap-chain resources,
the descriptor heaps at the title's addresses, all startup compute shaders but four, and every graphics
pipeline the title asks for. DirectStorage streams and inflates `shaders.rpf`. The title submits GPU work,
signals and polls its fences, and calls `PresentX` with its first frame. The current stop is a descriptor write
to a destination of a few hundred bytes (a descriptor-memory base that comes back as 0, likely from one of the
console-only command-list slots 70-75, 126, 139 or 142 that are not forwarded yet).

## Stops and what got past them

Continues `docs/retail_boot_chain.md` (stops 1-13). Slots are 0-based vtable indexes.

| Stop | Finding | Stand-in |
|---|---|---|
| GameInput | The title imports `gameinput.dll!#1` **by ordinal**; the shim registers it by name only. Without SDL3 the shim is silently left out of the build | `-DLEONIDA_SDL3_FETCH=ON`, `#1` registered too |
| DirectStorage | `DStorageGetFactory` with a console factory IID. Factory slot 4 = OpenFile, 5 = CreateStatusArray, 10 = queue creation. Queue slot 3 = EnqueueRequest, 4 = EnqueueStatus, 6 = Submit; status slot 3 = IsComplete. Request: +0 options (low byte 1 = zlib), +8 destination, +0x18 destination size, +0x20 file, +0x28 offset, +0x30 source size | `shims/dstorage_x.cpp`: synchronous reads, zlib through `shader_library.h`'s inflater, always complete |
| Device slot 26 | GetCustomHeapProperties with the struct-return buffer in RCX and the device in RDX | special case |
| Device slot 48 | CreateCommandQueueX with console flags 2 and 3 | flags masked to bit 0 |
| Device slot 73 | The title asks for a console graphics-list IID | created and wrapped as `ID3D12GraphicsCommandList` in upstream's console list proxy (`boot-hacks` first had its own list wrapper; dropped in the merge) |
| Physical pages | Wine 9's `MapViewOfFile3` rejects `PAGE_WRITECOMBINE` (error 87) | write-combine/no-cache bits dropped |
| Stop 13 (descriptor heap) | The heap description is 32 bytes: the PC 16, then `{address, size}` of memory the title reserved | upstream's console-addressed heap (issue #313) after its build fix: the title's memory is the heap's storage. (`boot-hacks` first used an address-translation stand-in; dropped in the merge) |
| Device slots 25, 108 | GetResourceAllocationInfo / ...Info1 with swapped struct return; some console formats (A8_UNORM render targets) give UINT64_MAX on the PC | special cases plus a size estimate |
| Device slot 127 | CreateCommittedResource3 shape | routed to CreateCommittedResource |
| Resource memory | The console D3D runtime takes resource memory from the title's exported `__XMemAllocationHooks__` (allocator id 128 in bits 16-23, memory type 0x8000); the title checks its pools afterwards | slot 127 calls the title's hook with the resource size |
| Device slot 41 | Indirect argument type 105 (console only, 12-byte stride); Leonida has no DISPATCH_MESH signatures | both mapped to DISPATCH |
| Device slots 61, 70 | Sampler and buffer-view writes with console descriptions | ignored (logged) |
| Device slot 74 | `(va, page_count, &out)` after the title maps its ~6 GB physical-page GPU pool | a logging probe object |
| Device slot 57 | Not a PC description: a 0x160-byte console pipeline record (root signature, six `{ptr, size}` blobs, hashes). Blobs 2 and 4 are native code; their **first 24 bytes match exactly one corpus FSSM program** | `psomatch.py` maps those headers to corpus shaders; the pipeline is built from the corpus DXBC with an input layout from the shader's input signature |
| Runtime compute shaders | The title's compute pipelines carry FSPX programs from `shaders.rpf` that the offline stage never carved | dumped at run time, translated by `leonida-shaders-pass.sh` into `spirv/fspx_game/` (loaded as a third index folder); 38 of 42 translate |
| Hang watchdog | A title thread kills the process when something stalls 30 s; Vulkan pipeline creation is much slower than the console's precompiled pipelines | the title's own off switch is set at start |
| Queue slot 18 | **GetTimestampFrequency** (the console queue runs 2 slots ahead of the PC one past Wait). Forwarding it to PC GetDesc wrote 16 bytes into an 8-byte local and corrupted the caller's saved RBX: this was the "null render context" crash | queue 18/19 mapped to GetTimestampFrequency/GetClockCalibration |
| Queue slots 16, 17 | **Signal(fence, value)** and **Wait**: this queue is two slots ahead of the PC one from Signal on (16-19 = PC 14-17). Forwarding 16 to PC 16 (GetTimestampFrequency) wrote 1,000,000,000 over the fence proxy's vtable pointer and never signalled the fence | routed to Signal/Wait with the fence unwrapped |
| Fence slot 12 | Returns a pointer to the fence's completed value, which the title polls in memory (the console GPU writes it) | a per-fence UINT64 kept current from GetCompletedValue by one thread |
| Queue slot 32 | **PresentX** `(planeCount, planes, params)`, the newer layout `xbox_present.h` lists | accepted and logged (no window yet); the title presents its first frame |
| Queue slot 35 | ExecuteCommandLists with two extra words | routed to ExecuteCommandLists, lists unwrapped as slot 10 |
| Console slots past the PC vtable | The child proxy refused slots past the PC table **before** the console handlers ran, so queue 32/35 and fence 12 never reached them | the console handlers run first |

## Loops

- `leonida-try.sh <name> <seconds>`: repackage the runtime and run the title once under Wine (`/root/<name>.log`
  and the startup trace `/root/<name>.jsonl`).
- `leonida-shaders-pass.sh`: translate compute containers the runtime dumped to `/root/leonida-dump/`.
- `psomatch.py`: match code headers the runtime wanted (`/root/leonida-psowant.txt`) against the FSSM corpus and
  append them to `/root/leonida-psomap.txt` (not committed: it holds bytes of the title's code).
- `gdbat.sh <seconds> <gdb file>`: start the title and attach gdb after a delay. Hardware watchpoints do not work
  under Wine (it rewrites the debug registers); bisect with breakpoints instead.

## Next

1. The descriptor-memory base: which console list slot (70-75, 126, 139, 142) hands it out.
2. Device slot 86, another pipeline-creation variant the title calls thousands of times (refused so far).
3. Read back `PresentX` plane 0 and write frames to disk, so frames can be checked without a display.
3. Real implementations for the ignored sampler and view slots (61, 70) and the probe at 74.

## 2026-11-24: where loading stops now (after 2e85f84)

- At about 87 s the title's texture-object creation (game+0x34beb0, then +0x193de10) fails for small 160x90
  render textures (formats 24 and 26): the title's own GPU memory allocator (game+0x2474690, then +0x221ad20,
  allocator 2 via +0x4fc99c0, then +0x4fac3c0) returns null for a 0x4b0000-byte, 64 KiB-aligned request.
  The title then asserts (+0x2219650, +0x1647d03, +0x6eb888) and reads through the null object (+0x6eb759).
- Only about 450 MiB has been placed through device slot 133 by then, so it is one pool, not memory as a
  whole. Next: find allocator 2's backing region and why it fills (a leak, or a size the PC side gets wrong).
- The slot 127 charge to the title allocator (xs_title_d3d_alloc) is never freed; removing it did not move the
  stop and made a 2 MB budget soft assert (+0x247af2d) appear early, so it stays for now.
- Breakpoints under gdb: patch the title's soft-assert int3s to nop first (set {unsigned char}ADDR = 0x90),
  or they stop gdb before yours. The gdb breakpoint on a failure path of a long function, then on each
  return-false site, found this stop in three runs.
- Run settings that matter: LEONIDA_ARENA_MB=4095 (the 256 MiB default fills with compute programs),
  LEONIDA_SKIP_INT3=1 (step over soft asserts), LEONIDA_CE_STACKS=1 (thread stacks at presents 300, 20000).

## 2026-11-24 (later): after console footprints (9bd2a8d)

- The 2.2 GB GPU pool (title allocator 2, object at 0x1471c2808: base +0x20, size +0x30 = 0x89700000, used
  +0x58) no longer fills: allocation answers are console footprints now. The title runs to about 124 s and
  binds its UI pipelines (VertexCxformEAlpha, BatchVertexCxformEAlpha); frames are still all zero.
- Next stop at about 124 s: soft asserts in the title's free (game+0x1887b0; +0x187fb, +0x1881f, +0x1884a check
  the block header at -0x10/-0xc/-0x2), then a null read at +0x538d0e6 under a recursive teardown
  (+0x183aaf / +0x18435f). A block freed there is not from that heap, or its header was overwritten.
- The bad free (gdb, break at game+0x1400187fb with the other soft asserts patched to nop): the block's
  allocated bit (header word at -0x2, bit 10) is clear, so it was already freed (double free) or its header was
  overwritten; its payload is float data (1.0f). It is freed by a container's teardown (game+0x455aeb0: count at
  +0x32, entries at +0x38, each freed through the allocator at 0x14abcc310, vtable +0x60), under +0x515ad70.
  Teardown at this point is an error path: something earlier failed. The first anomaly in the run is a write
  past a stream buffer's capacity at about 98 s (soft assert +0x4dcbc9d, game+0x4dcbc55: remaining < size;
  the write is skipped and returns false). Next: what fills that buffer, and why it is too small here.
- The overflowing buffer (game+0x4dcbc55, about 100 s) is a shared command stream, not a DSP queue: records
  tagged 0x...04002821 carry float parameters (0.55, 0.2, 1000, 2000). Writers flush a local buffer into it
  (game+0x4e52a40: lock at owner+8, append via +0x4dcbc40) from +0x5ab13e8 through +0x4e529a0. The stream comes
  from the table at *0x149ae9950 (+0x4dcbc10): per slot an object with several 0x28-byte streams and a current
  index at +0xc8, so a consumer is meant to switch streams each frame and drain the old one (the switch is in the
  large function around +0x4dc3df2/+0x4dc3e11). No thread was in that code in the stack dumps, and the stream fills
  in about 100 s. A null ALSA device (ALSA_CONFIG_PATH with pcm.!default type null, run setting only) removed the
  ALSA open errors but did not change this. Next: who calls the switch, and what it waits on.
- gdb from 20 s to 280 s: the stream append (+0x4dcbc72) runs on several threads (gdb threads 27, 96), about
  1,000 times in that window; neither write of the stream index (+0x4dc3e11, +0x4dc2e23) ever runs. Either the
  consumer is elsewhere or its thread never runs. AcpHal now acknowledges commands (97ca179), but the title only
  registers for messages (command 2) before this stop, so the coprocessor is not what the consumer waits on.
  gdb note: a breakpoint must be on an instruction boundary (check game.asm), or the run breaks silently.
- The stream consumer, found: game+0x43a6b40 walks every stream-table entry (count at 0x149ae9958) and is called
  only from the manager update +0x6d8da0, which is reached from +0x43ae2f2 and from the job callback +0x537f810
  (CAS on a frame counter at obj+0x78, then the update, then clears obj+0x7c and decrements obj+0x80). Under gdb
  from 20 s the update ran once (gdb thread 17) and never again, so the per-frame job is not submitted again, or
  the scheduler holds it back on those counters. Next: who submits the +0x537f810 job each frame, and what it
  checks (obj+0x78/+0x7c/+0x80) before submitting.
- Why the consumer stops: the per-frame tick game+0x43ae230 (it runs the stream update inline or queues the
  +0x537f810 job; the job descriptor is static at 0x149ae9988, busy flag 0x149ae9a04, pending 0x149ae9a08) is
  called only from the audio thread's main loop at game+0x1e95xxx. That thread (seen in the stack dumps as
  ... G+a7c3f3 G+4bd6 G+5594661 G+1e95166) is stuck at the first instruction of +0x5594650: the title's own lock
  routine +0x4b40 on the audio object (not an imported CRITICAL_SECTION or SRW lock). So another thread holds
  that object's lock and never releases it, and the audio frame never comes. Next: break at +0x559465c to get
  the object, read its lock word after the stall, and find the owning thread's stack.
- The audio lock (gdb at 110 s, hacks: owner.gdb pattern): the title's lock word is one u64, owner thread index
  in the high half (the index is TLS slot 0 block +0x1454), recursion count in the low half, 0xFFFFFFFF00000000
  free. The audio object's lock reads 0x15000000003: thread 336, held three deep, and no live thread has index
  336. The owner exited while holding it. Next: which short-lived game thread takes the audio object's lock
  (index 336, so the title has made several hundred threads by then) and why it ends without unlocking (a thread
  the runtime ends early, or an exception path).
- The process has about 1,190 threads at 110 s: the background compute compiles make one thread each, and most
  wait for one of the 40 slots. To do: a fixed pool of compile workers.
- Correction: the audio lock was never taken. Under gdb from 1 s, no thread with index 336 ever acquires a lock
  (break at game+0x4bdf if the new owner is 336), and the audio object (heap, e.g. 0x7bf358747a40) already reads
  0x0000015000000002 when the audio thread first locks it (its first frame, about 61 s). The word was overwritten
  with data (2 and 0x150), not locked: memory corruption of the audio object. Suspect first: the runtime's
  allocations made at audio start (trace: XMemAlloc attributes 0xf0814000, 393216 bytes at 61.79 s; ApuCreateHeap
  and SHAPE contexts at 61.78 s) overlapping or under-sizing; then the silent xapu/chad status and result writes.
  Next: log the object's address against those allocations, and break on writes to it (software: single-step is
  too slow; check each runtime writer's target range instead).
- Not the runtime's audio-time allocations: in one run the corrupted audio object was 0x7b0ecd569140, the APU
  cached heap 0x7b08778f0000+0x1c700000, the non-cached one 0x7b0c39c60000+0x226e00, and the two XMemAlloc blocks
  (0xf0814000) 0x7b0c26548440+0x60000 and 0x7b08981a0080+0x1e00000; none contains it. The silent xapu result
  record matches VIctory's 0x50-byte layout, so dequeues do not overrun either. Next: which allocator made the
  audio object (the title's own heaps), and a check of other runtime writers into title memory (bottom-of-pipe
  writes 71/72, the console texture refresh, CBV snapshots).
- Shell note: never `pkill -f` a pattern that also appears in the command running it (it kills its own shell,
  exit 144); use `pkill -x gdb`.
- Not the bottom-of-pipe writes (list slots 71/72): with every write logged that carries 0x150 in its high half
  (none) and the first 40 targets (0x795cb29000xx and 0x795e1259xxxx, values 0 or counters), none lands on the
  audio object (0x795e17541ec0 in that run). Left to check: the console texture refresh and CBV snapshots (both
  read title memory, they do not write it), the title's own heaps (which allocator the object came from and its
  neighbours), and whether the object was ever constructed (a lock word that was never initialised would read
  as whatever the allocator left there).
- The "audio object" lock is a member: the audio thread locks engine+0x3ced0 (rsi = the audio engine, the same
  object the manager update reads at +0x3cdf8 and +0x3dd80). Its word differs between runs (0x15000000001,
  ...02, ...03), so it is being locked by a path other than +0x4bdf (the recursive path, or an inlined lock),
  not a one-off overwrite. Forcing it free under gdb at the audio thread's first lock (hacks: unlock.gdb pattern)
  did not help: the stream update still ran once and the run still ended at +0x538d0e6 (117 s). So this lock is
  not the only thing holding the audio thread. Next: the audio thread's stack after a forced unlock, and the
  inlined lock sites that write engine+0x3ced0.
- Correction (TLS): the title's thread index is in its own TLS slot, not slot 0. The exe's TLS directory says
  AddressOfIndex 0x146e08b2c (slot 2 in these runs); the earlier "owner 336 does not exist" read slot 0.
  With the right slot the audio engine's lock (engine+0x3ced0, word 0x15000000003) belongs to thread index 336,
  a job worker that is idle at its entry wait (G+221c3c2) by present 300, holding it three deep. No exception
  on any thread before about 119 s, so it was not skipped by an unwind: the engine is locked across jobs on
  purpose, and the job that unlocks never runs. Prime suspect: the XMA decoder contexts the title polls (VIctory's
  audio.rs decodes XMA contexts on a thread and advances their read/write blocks; here nothing advances them),
  so the first audio frame never completes. Next: port VIctory's XMA context pump, silent (consume input packets,
  write zero output blocks, advance the pointers), with credit.
- Stack dumps now print each thread's title index ("tid N idx M"), read through the title's TLS slot.
- Not XMA either: with the silent XMA pump in place (acphal, after VIctory) the title still sends only
  coprocessor command 2 before the stop, so no context is ever enabled; the audio thread stays on the same lock.
- The engine lock's word has 2 or 3 in its low half, but the lock routine +0x4b40 always writes 1 there (its
  recursion count lives at +0x10), so another routine (a recursive or inlined variant) takes this lock; that is
  why the breakpoint at +0x4bdf never saw owner 336. Next: find the lock routines that add to the low half.
- FIXED (audio): the lock leak is the title's own function +0x5267720, which takes the engine lock and skips the
  unlock when its object's count at +0x52c is 0 (branch +0x5267747). Patched at device setup to go to the
  unlocking tail (+0x5267c7f). The audio thread now runs and the command stream no longer overflows. What +0x52c
  counts (and why it is 0 here) is still open; the patch only makes the zero case unlock like its sibling.
- The run still ends at about 121 s: the bad free (+0x187fb, +0x1881f, +0x1884a) then +0x538d0e6. That is a
  separate fault, not the overflow's error path as guessed before.
- Not the XMemAlloc hooks: routing XMemAlloc/XMemFree through __XMemAllocationHooks__ (as VIctory does) is in,
  and the bad free still comes at about 123 s. Not command allocator slot 9 either (an "allocation callback"
  VIctory ignores; the caller at +0x188501 ignores its result).
- The stop at about 120-130 s has two forms that alternate between runs: (a) the bad free (+0x187fb..) then a
  null read at +0x538d0e6, (b) a read of address -1 at +0x13360a3 (stack +0x186424, +0x18573b, +0x184f42,
  +0x1847d8, +0x1837a2). Both run through the +0x18xxxx code next to the title's free (+0x187b0), so they likely
  share a cause. hacks: dfree.gdb dumps the freeing container (+0x455aeb0: count +0x32, entries of 0x20 at +0x38,
  pointer at +0x18) to test for a double entry, when the run takes form (a).
- The bad-freed block sits at a fixed offset: 0x72e67db40590 in one run, 0x7b582db40590 in another (same low
  28 bits, ...db40590), so it lives at a fixed place inside one large reservation, not at a random heap address.
  The -1 read form (+0x13360a3) is a virtual call through the object at rsi (call *0xb0(vtable)) whose vtable is
  not a canonical address: a freed or overwritten object, consistent with the double free. hacks: uaf.gdb.
  Next: which reservation holds ...db40590 (the title's XMemVirtualAlloc regions and pools vs the runtime's
  VirtualAllocs, e.g. console texture memory), from /proc/<pid>/maps against the trace.
- Not physical-page aliasing in xmem.cpp: a code review and a full-scale Wine test (111,136 pages in the 6.5 GB
  and 710 MB windows, offsets past 4 GiB, a 24 GiB placeholder) found pages distinct and shared mappings correct.
- Queue slot 31 is CopyPageMappingsBatchX (VIctory slot 29 + 2) and is now implemented; its calls succeed. The
  bad free still comes (at 131 s in that run). Open, from the same review: VIctory keeps XMemVirtualAlloc
  reservations without an address field below 2^40 (the console keeps title memory there); CE places them near
  0x7xxx_xxxx_xxxx. If the title packs pointers into fewer bits, that would corrupt memory. Next candidate.
- Not the address range either: with title reservations below 2^40 (as on the console) the bad free still comes
  at about 125 s. Ruled out so far for this stop: audio lock (fixed), XMemAlloc hooks, xmem aliasing, page
  mapping batches (implemented), the address range, the runtime's audio allocations and list writes.
- Both forms of the 120-130 s stop are the same corruption: float data overwrites live objects. The bad-freed
  block held 1.0f floats; in form (b) the object at rsi (0x14f341440 in one run) is rows of 0x3f800000 (1.0f,
  patterns like 1,1,1,0), its "vtable" two floats. That address is not in the exe (image ends 0x14b06d000): it
  is inside the title's 0x183 pool, reserved at 0x14b200000 (0x25400000 bytes) since title reservations stay
  below 2^40, which also makes pool addresses repeat between runs. Next: if the object's address repeats,
  write-protect its page under gdb before 120 s (call mprotect in the inferior) and stop on the fault to find
  the writer of the floats.
- The corrupted object's page repeats between runs (0x14f341440, 0x14f3414e0: page 0x14f341000, the 0x183 pool
  at 0x14b200000). hacks: trap.gdb write-protects it from 90 s (mprotect in the inferior) and logs each writer.
  In that run nothing wrote the page for the whole gdb session and the title did not crash at 120-130 s: it ran
  to 349 s, loading world data (DirectStorage opens of data/ttdb terrain and map tiles, the furthest so far).
  The only fault was a later write at +0xaa0c2b (an allocator writing an all-ones block header, legitimate) once
  gdb had left the page protected. So the float corruption is a race that the gdb slowdown avoided.
  Next: the same trap inside the runtime (VirtualProtect at about 90 s plus a vectored handler that logs the
  writer and re-protects; and a periodic float check), which keeps the timing. If floats appear without a
  trapped CPU write, the writer is the GPU path (memory the GPU writes through host-imported pages).
- In-process trap (LEONIDA_CE_TRAP_PAGE=14f341000, LEONIDA_CE_TRAP_DELAY=30): objects are built in that page at
  about 70 s (game+0xd3d471/+0xd3d4e3 <- +0xd40ba9 <- +0xd3d398 <- +0xd3cf37 <- +0xf59db5 <- +0x45e3dc4, with the
  allocator header writes at +0xaa0c2b); at about 110 s a memcpy from game+0x550e60f (a row copier calling memcpy
  through a callback) fills the same page with float rows, so the allocator gave the block to a second owner
  while the first still used it. Root is the double free (form a): find the first free of a block that is
  still referenced. Idea: trap the title's free (+0x187b0) for blocks in this page.
- The objects in the trapped page come from a bump arena (allocator +0xaa0bf0; arena object on the heap, e.g.
  0x71fb5d79e5b8: [+8] first block 0x14e928620, [+0x10] current block). They are carved in a burst of about 15 ms,
  and about 0.3 s later the float rows land in the same page. Trapping the arena's own page (to catch a reset of
  +0x10) slowed things enough that no corruption happened and the run went on to world data (no crash in 300 s),
  so it is a race. Pinning the title to 8 CPUs (taskset -c 0-7; Wine may still report all 128) did not avoid it:
  the stop came at 143 s, after the world tiles had started loading. To check next: the CPU count the title sees
  (GetSystemInfo / logical processor info under Wine) and whether its job system sizes worker counts from it, and
  who resets or shares this arena across threads.
- Not the processor count: the title reads GetLogicalProcessorInformation (and Get/SetThreadGroupAffinity);
  cut to 16 logical processors (LEONIDA_CE_CPUS=16, shims/sysinfo_ce.cpp) it still stops at 134 s.
- Not DirectStorage ordering: with asynchronous, in-order completion (as VIctory and the console) the stop still
  comes at 131 s. Next: which threads touch the arena in the trapped page (log the thread id and the title
  thread index for the arena's allocations and for the float writes), to see whether two threads share one
  arena or whether the arena is reset while another thread still holds blocks from it.
- One thread: the arena's allocations in the trapped page (+0xaa0c2b) and the float rows later copied there are
  both on the same thread (tid 180, title index 384 in that run). So it is not two threads sharing one arena:
  that thread reuses the arena's memory (a reset to its first block, about 0.28 s after the objects were carved)
  while another thread still calls through an object it holds there. The allocator chains new blocks when full
  (+0xaa0dc0); it does not wrap. The writes at +0xaa05ff/+0xaa0734/+0xaa08d9 are list-node setups, not the
  reset. Next: find the reset (a write of [+8] into [+0x10], or a block free) with LEONIDA_CE_TRAP_ARENA=1 on a
  run where the floats do appear, and what it waits for first (the other thread's use should end before it).
- The arena object itself is destroyed. A watcher that polls the arena's +0x10 every millisecond (no page
  protection, timing kept) saw it go 0x14f341f30 -> 0x100000002 -> 0 -> 0xffffffff00000000 -> float data, i.e.
  the arena struct was freed and its memory handed out again while its owner (tid 180) kept allocating from it.
  At that moment tid 180 was in the recursive walk +0x183aaf/+0x18435f under +0x1826cb <- +0x503d5f5 <- +0x180f2d
  <- +0x503ce73 <- +0x41f3f8: a work queue (+0x503ce45 pops items with +0x515ada0) dispatching each item through
  +0x180f00 (vtable +0x28), whose walk frees nodes. Hypothesis: an item reaches that queue twice (or after its
  owner is gone), so it is processed and freed twice. Next: log items popped at +0x503ce5e and look for repeats.
- Work queue pops (gdb at +0x503ce63, hacks/pops.gdb): items do come back (6 of 24 distinct items popped twice,
  queues 0x14ece91a0, 0x14ece9b90, 0x14f1eef80, all on one thread), but recurring items may be normal. To tell,
  next: also log the title's free (+0x187b0) of those item addresses and look for a pop after a free.
- Pops vs frees (hacks/popfree.gdb): no popped work item was freed through +0x187b0, so "an item popped after its
  free" is not it. That run ended in a third form of the same stop: +0x515acf5, an allocator's block lookup (a
  tree search for the block holding a pointer) finding none and reading through null, called from the container
  teardown +0x455aeb0 under +0x1335dce <- +0x186424 (the same function family whose virtual call is form b). All
  three forms are frees or calls through objects the recursive walk +0x183aaf/+0x18435f has already destroyed.
  Next: find what decides that walk destroys a subtree (its caller +0x503d5f5 and the item flags it reads), or
  try skipping the destroy step there as a hack (as with the audio lock) to see how far the title gets.
- Not frame pacing: presents at 30 per second (LEONIDA_CE_FPS, default now) and the stop still comes (117 s).
  The walk's step at +0x1842c9 is an allocation (general allocator 0x14abcc310, vtable +0x70, 0xa0 bytes, flags
  0x47): when the arena struct was overwritten, that allocator was handing out the arena's memory, so the arena
  had been freed before. Ruled out for this stop so far: audio lock, XMem hooks, aliasing, page mapping batches,
  address range, CPU count, DirectStorage ordering, frame pacing, runtime writes into title memory.
- TURN: it is an overflow, not a use-after-free. Watching the arena object's first word: at the instant it changed
  (0xc002010 -> 0), its owner tid 180 was inside the float-row memcpy (host memcpy <- +0x550e613 <- +0x1335bea <-
  +0x186424 <- the walk). The row copy writes past the end of its destination and over its neighbours (the arena
  object, the objects in the trapped page, heap headers): that is the clobbered vtable, the "freed" arena and the
  bad free alike, and why it depends on layout and timing. At +0x1335be6 the title calls *0x40 on the object at
  [rsi+0x30] with destination [rsi+0x50]+0x18 and a source from [rsi+0x58] (vtable +0x10); the copier +0x550e5xx
  then writes [item+0x38]*16 bytes for each of [item+0x3c] items. Next: compare the destination buffer's size with
  what is copied, and find which of the two the runtime feeds (a size or count it answers differently from the
  console: allocation info, texture layout, a query result).
- The overflowing write, measured with in-process probes (LEONIDA_CE_PROBE=14550e60f,141335bd6): at +0x1335bd6 the
  object being handled is rsi=0x14f340560; the row copy +0x550e60f then writes 0x1000 bytes (256 rows of 16: a
  256-entry float4 lookup table the title computes with sqrt/round, clamped at 255; item [r14+0x38]=256,
  [r14+0x3c]=1) at 0x14f340600 = rsi+0xa0, i.e. inline after a 0xa0-byte object, and over into page 0x14f341000
  where the objects that later break live. The walk allocates its nodes at exactly 0xa0 bytes (+0x1842c9:
  allocator 0x14abcc310 vtable +0x70, size 0xa0, flags 0x47). Next: where the 256 comes from ([item+0x38] of an
  item in the pool) and whether that node should have been allocated larger for it; then which runtime answer
  feeds either (the title may size the table from a count the console reports smaller).
- The row copy's destination comes from a texture map (+0x13367a0 fills {.., pitch at +8, data at +0x18}) inside
  the fill function +0x550e2d0 (vtable 0x1466314b0 slot +0x40, called at +0x1335be6): it writes a 256x1
  R32G32B32A32 lookup table. The texture computer reports that texture correctly ([xg] log: 256x1 fmt 2, layout
  0x100 -> size 0x1000, align 0x100, mip0 at 0), and the write is exactly 0x1000. Padding every footprint by
  4 KiB (LEONIDA_CE_PAD=4096) did not help. So the table's 4 KiB lands right after a 0xa0-byte node, in memory
  the arena also gave to other objects. Next: what size the title requests for that block (probe the arena
  allocator entry +0xaa0bf0: rdx = size, for the allocation that returns the node), against 0xa0 + 0x1000.
- Probing the arena's end-marker write (+0xaa0c2b) for allocations in pages 0x14f338000-0x14f342000
  (LEONIDA_CE_PROBE_RANGE): rdx there is 0 or 8 (an alignment, not the size), and the first 60 hits were small
  allocations (headers, list nodes) from several call chains (+0xcf91cf, +0x538d2b0, +0x109da9d, +0x11d0b4c ...),
  not yet the block that later receives the 256-entry table. To do: probe the arena allocator entry for the size
  argument with a range filter on the returned block, or log the allocation that returns the table's node.

## Summary of the 120-140 s stop (for whoever picks this up)
- What happens: on one worker thread (title index ~384), the recursive walk +0x183aaf/+0x18435f builds nodes from
  a bump arena; the fill function +0x550e2d0 maps a 256x1 R32G32B32A32 lookup texture and writes its 4 KiB
  (+0x550e60f -> memcpy) right after a 0xa0-byte node; that overruns into the following nodes, the arena object
  and heap headers. Every crash form (bad free +0x187fb.., virtual call through floats +0x13360a3, null in the
  block lookup +0x515acf5, +0x538d0e6) follows from it. Slowing the title (gdb, page traps) avoids it, and runs
  then go on to load world data (ttdb terrain/map tiles).
- Ruled out (each tested): audio lock (fixed, 62ad4fd), XMem hooks, physical-page aliasing, page-mapping batches
  (implemented), address range (below 2^40 now), CPU count (LEONIDA_CE_CPUS), DirectStorage ordering (async now),
  frame pacing (30 fps now), runtime writes into title memory, texture footprint/layout sizes (LEONIDA_CE_PAD).
- Tools: LEONIDA_CE_STACKS, LEONIDA_CE_TRAP_PAGE/_DELAY/_ARENA (page write trap + arena watcher),
  LEONIDA_CE_PROBE (+_RANGE) in-process code probes; hacks/*.gdb.
- LEONIDA_CE_SKIP_LUT=1 (nop the two row copies +0x550e57e/+0x550e60f): the crash stays (try137, +0x538d68e at 118.9 s,
  `and %ecx,(%rax,%r9,4)` with a trashed pointer, same walk). So the table copy is not the only corruption.
- try138 (trap): the arena object the walk uses (rcx at +0xaa0c2b, 0x7a3a2efce5b8) lies on thread 180's own stack,
  0xcb8 bytes BELOW the crashing rsp (0x7a3a2efcf270), and its header changes (c002010 -> 1) just before the crash.
  Working hypothesis: the arena lives in a stack frame that has already returned (use after return), so any deeper
  call overwrites it; timing decides what gets called there. Next: find the function that builds that arena on its
  stack and whether it returns before the cleanup walk +0x182bdc uses it.
- try139 (owner unwind at the first arena write): the arena object is the scratch arena at +0x120 of a job context
  that +0x45e1170 builds on its stack ([rsp+0x28], constructor +0xcf86d0, destructor +0x2101350, which frees the
  arena's chunks with +0x15a0630). Later the same thread's stack is above that frame and the arena is reset.
- The walk's node allocations (+0x182b18 size 0xa0, +0x182bd9 size 0xe0) use allocator slot +0x70 = +0x538d040,
  "allocate in the pool that owns this hint pointer" (slots: +0x50 = +0x538d930 plain allocate(pool, size),
  +0x60 free, +0x70 allocate-near). The hint is an older node; the pool lookup of a stale hint is +0x538d0e6.
- LEONIDA_CE_NO_HINT=1 (call slot +0x50 at both sites): crash moves to a plain allocation finding 1.0f in the free
  list (+0x515b22d, try141). LEONIDA_CE_LEAK_SCRATCH=1 (skip the destructor's chunk release): crash at 122 s,
  +0x538d738 writing through a free-list link that holds 0x146639ac0 (try142).
- 0x146639ac0 is the base vtable that +0x5390160 stores; the walk's destructors (+0x18226b, +0x182480, +0x1841d8
  ...) free the object's arrays, store that base vtable, then free the object. A base vtable in a freed block's
  link = an object destroyed after (or twice after) its block was freed: a double delete in the walk.
- Two cores (taskset -c 0-1, LEONIDA_CE_CPUS=2, try143): still crashes, later (159 s, +0x515acf5). Not conclusive.
- Next: watch the walk's object destruction for the same object destroyed twice (probe +0x5390160 with rcx, log
  repeats per thread), and who holds the second reference.
- Stale-owner hypothesis (from a DeepSeek first pass over +0x181b00..+0x184400, every address checked): a parent
  caches a child at [owner+0x28] (+0x182abe, +0x184338) and never clears it when the child is detached by
  +0x182870 (clears child+0x30, tail-calls vtable +0x18), so a later get-or-create could hand a dead child to the
  attach function +0x183190 (called from +0x18435a, on every crash stack). The only refcount in the walk is
  +0x183930 (lock dec [rcx+8], deleting destructor on zero); the only atomic hand-off is +0x1841f0 (xchg [+0x10]).
- LEONIDA_CE_PROBE=140183190 LEONIDA_CE_PROBE_STALE=1 logs a child whose first qword is the base vtable or not a
  vtable at attach. try144: 0 hits, crash as usual at 123.7 s (+0x13360a3). So no child is visibly dead when it
  is attached; a block reused by a new object would still pass, so the hypothesis is weakened, not ruled out.
- French-General (issue #2) reports Windows-native runs at 1403e90 do NOT hit this stop: they keep presenting.
  So it is likely Wine- or Linux-shim-specific; compare what differs (threading primitives, timers, TLS).
- Thread-local storage under Wine (try146/try147, LEONIDA_CE_STACKS=1 now also sweeps every thread's title TLS
  block every 300 presents): 65-70 threads, every one with its own block, no block shared, none changed between
  sweeps up to the crash. A stale or swapped TLS block (a lead from a DeepSeek audit of pe_loader.cpp) is unlikely.
  idx -1 in the dumps = a thread with a block but no title thread index (not a missing block, which reads -2).
- C++ runtime: under Wine the title's 113 MSVCP140 imports (and vcruntime140, concrt140) bind to Wine's builtin
  reimplementations (PE-Wine in the module list); on Windows they bind to Microsoft's (the one gta6rt itself loads).
  The package's own msvcp140 is 14.29 and cannot be used: gta6rt (built with MSVC 14.44) crashes in its first
  std::mutex::lock with it (try149). Next: run with Microsoft's current VC++ 2022 runtime as native DLLs in a copy
  of the prefix (winetricks vcrun2022), to see whether the 120-140 s stop follows Wine's builtin C++ runtime.
- Ruled out: Wine's builtin C++ runtime. With Microsoft's VC++ 2022 runtime (14.44.35211, winetricks vcrun2022 in a
  copy of the prefix, WINEDLLOVERRIDES msvcp140/vcruntime140/concrt140=n,b; module list shows PE, not PE-Wine) the
  stop is unchanged (try151: +0x538d0e6 at 119.9 s). Note: the package's own 14.29 DLLs next to gta6rt.exe make
  gta6rt itself crash at start (std::mutex), so do not leave them in the output folder.
- Native Windows (Win10 VM with an RTX 6000 Ada, same build): no 120-140 s crash, but loading stopped after the 12
  .rpf archives (no ttdb world data, 71 compute pipelines vs 2290 under Wine). Cause: the title's positional read
  loop +0x314200 (ReadFile with an OVERLAPPED offset, under the I/O lock +0x1478f5e00 taken at +0x314003) got
  ERROR_NOACCESS (998) reading 0x1027d000 bytes into console memory at 0x5a00e0000, and retries forever on any
  error but 6/0x26/0x57 (Sleep(1)). XMem commits mapped physical pages on first touch from a vectored handler,
  which the Windows kernel never triggers; Wine's ReadFile copies in user mode, so it worked there.
  Fix: shims/readfile_ce.cpp commits the destination (rt::xmem::commit_physical_range) before ReadFile. With it the
  Windows run opens all 40 files (world data) and builds 1028 compute pipelines, then stops at 98 s at +0xb5a5f8
  (read of 0x68): the next problem on Windows.
- Windows, after the ReadFile fix: +0xb5a5f8 reads [record+0x68] with record = 0 (20-100 s). +0xb5a5d0 interns a
  definition record picked from a table: cfg->[0xc78][bits 15..23 of cfg->[0xc68][row]] (callers +0x5bb7c58 with
  cfg = [rsi+0x650], +0x1dc5ca1 with cfg = global). Index data is valid (row word 0x04000001 -> index 0); the entry
  stays null (not a race: every lookup misses). No ReadFile fails any more. Hack (default on, Windows and Wine):
  a vectored handler turns that exact fault into "return 0" to the caller; the run then reaches 52 s and stops at
  +0x5392b80 (read of 0x38), probably the null it got back. Next: what registers those table entries
  (stores to +0xc68/+0xc78 at +0x247bcf8/+0x247bd8b), and why not on Windows.
- A probe on +0x141dc5ca1 / +0x145bb7c58 also waits up to 10 s for a null slot (LEONIDA_CE_PROBE): no hits there in
  the runs so far, though the crash came through those callers; unexplained.
- The 52 s Windows stop (+0x5392b80, null at +0x38) is in the same task as the Wine 120-140 s stop (+0x41f4xx,
  +0x4039633, +0x4fc68ba, +0x4fc6849, +0x1186xx on both stacks): once Windows loads, it reaches the same phase and
  dies there too, so that corruption is not Wine-specific. Wine never hits the missing-definition lookups (0 in
  try153); Windows does on every lookup.
- Definition tables (DeepSeek, verified): the deserializer +0x247b6e0.. bump-allocates them inside cfg from
  cfg+0xd18; c68 (index dwords), c70 (B 24-byte entries), c78 (B record pointers, only reserved here, filled later)
  via the stream reader +0x51d2260 (return values unchecked). Probe at +0x247bd99 (rax = &c78[0], rcx = B): B is
  identical on Windows and Wine (0xc, 2, 8 ...). So the stream data matches; the later step that fills c78 runs
  under Wine but not on Windows. Next: find the writer of c78 slots (a write trap on one table's c78 page).
- ROOT CAUSE of the 120-140 s stop (Wine) and of the Windows null-definition stop: an incomplete PSO map.
  Slot 57 builds graphics pipelines from leonida-psomap.txt; a code header missing from it fails CreatePipelineState
  (E_INVALIDARG, header appended to leonida-psowant.txt). The title's definition tables (+0x247c833 stores the
  result of +0x1d65750 into cfg->c78[i]) then hold null records: on Windows the lookup faults at +0xb5a5f8; under
  Wine the walk over them corrupts its arena (the stop chased since a1e6ef1). A write trap on c78 (LEONIDA_CE_SLOT_TRAP
  with a probe on +0x247bd99) found the writer; a probe on +0x247c833 showed 6 null records on Windows.
  Fix: run hacks/psomatch.py (wanted headers from both runs, 8747) -> the map grows from 911 to 9658 entries, all
  matched. With it both Windows and Wine run 300 s without a crash (frame 4096+, 11000 graphics pipelines under
  Wine). Frames are still black: the title still submits no draws of its own (only the runtime's 4 detile draws).
  To reproduce on another machine: run once, then `python3 hacks/psomatch.py` (it reads leonida-psowant.txt), and
  run again. The map is not committed (it holds bytes of the title's code).
- Black frames with the full PSO map: the title calls DrawInstanced (list slot 12) about once per frame (its final
  pass) and no other draws; [list] lines (every 300 presents with LEONIDA_CE_STACKS=1) now print per-slot call counts.
  The thread dump at present 4000 shows it still creating graphics pipelines (workers in gta6rt from +0x247757e,
  a definition fill waiting in +0x2477400 <- +0x1d65b83 <- +0x247c828), main thread waiting: still loading, bound by
  pipeline creation (Wine 11000+ in 300 s, Windows ~1500). VIctory keeps pipelines on disk for this reason.
- List put pointer and limits (+0x18..+0x38) are now all zero as in VIctory's list.rs (every inline packet check
  fails, commands arrive as calls); the old pinning left +0x20/+0x38 at 0 with a non-zero put pointer. No visible
  change in the slots called; LEONIDA_CE_OLD_CURSOR=1 restores it.
- Late Windows stop (507 s, after compute dispatches start around frame 13800): ten slot 57 pipelines always fail
  (E_INVALIDARG): the world's cable pipelines (PS_Cable_*, PS_Cable_MirrorReflection_*) carry the pixel shader in blob
  #2 and blob #4 is empty; their vertex stage (blob #0, 0x1e8 bytes, or #3, 0x830) is not in the PSO map (a merged
  geometry stage, as VIctory lists unsupported). Failing them left null definitions. Now slot 57 hands back a stand-in
  pipeline (the first working one) instead (LEONIDA_CE_NO_PLACEHOLDER=1 keeps the failure); failures go to 0.
  Failing slot 57 calls also log all six blobs (length, head bytes, PSO map entry).
- pipeline_cache.bin is now written back (every 60 s when it grew; LEONIDA_PIPELINE_CACHE_SAVE=0 turns it off): Wine
  saved 1.9 GB and the next run loads it. Drafted by the DeepSeek helper, integrated by hand.
- Windows null definitions, root cause: the record publisher +0x2476e20 returns false when +0xfa09f0 fails; that
  helper unpacks the root signature from a shader's PRIV chunk, has it serialized by D3D12SerializeVersionedRootSignature
  (our d3d12_xs.dll shim -> the host d3d12.dll) and creates it (device slot 16). Windows 10's built-in d3d12.dll
  refuses the Shader Model 6.6 flags CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED (0x400) / SAMPLER_HEAP_DIRECTLY_INDEXED (0x800)
  ("Unsupported bit-flag set"); Wine's serializer takes them. The shim now retries without them. Windows then runs
  180 s with no null definition and no crash (frame 4800). Still black: one draw per frame, no dispatches.
  (Found with failure-only probes on +0x1d65b83 / +0x2476f9f; DeepSeek's analyses of +0x1d65750 and +0x2476e20 were
  both right.)
- Compute pipelines that cannot be built (slot 67) now also get a stand-in pipeline, like slot 57.
- On an unhandled crash the runtime prints "[error] I have fallen, and I can't get up! exception ... at ..." first.
- Black frames, fast checks (LEONIDA_CE_SMOKE=1 paints a magenta corner after every draw and logs draw shapes):
  present/capture work and the title's per-frame draw lands on the presented target (magenta shows). The draw is the
  UI renderer (sga_dr_vs_pos_color_tex + default_ps_tex): 3 vertices, topology 17 (the console RECTLIST; we draw it
  as one triangle, VIctory expands it: primitive.rs; a C++ port is drafted), full-screen viewport.
- Its vertex view (VA 0xb8df10.., 4080 bytes, stride 24) lies in a 174 MB placed upload ring at 0xb10000 whose GPU
  copy is a stand-in nothing refreshed: the draw read zeros. 0xb8df10 is a reserved XMem view (MEM_MAPPED, commits on
  touch). The replay now reads each vertex view from title memory (guarded copy) and writes it with vkCmdUpdateBuffer
  before the draw (LEONIDA_CE_NO_VERTEX_SYNC=1 off). The data is one vertex (colour 0xffffffff, rest zero): the
  shader builds the rect itself, so the black now comes from what default_ps_tex samples.
- Placed buffers up to 8 MiB are synced from title memory before each submission when they change; texture and buffer
  refreshes commit XMem views first. The UI-sized textures (2048x2048 fmt 87, 1280x720 fmt 42) still read 0% non-zero.
- The game window ("Americas") shows on the Windows VM desktop; black is the real output (white was before the first
  present).
- Black frames, the full chain: the per-frame UI draw's root argument (user data dword 0, e.g. 0x16adea0) points into
  the title's upload ring; the shaders read it at heap_base + VA, but the title writes it in its own memory. A ring
  sync (replay -> sync_title_range, only inside placed buffers; LEONIDA_CE_NO_RING_SYNC=1 off) now copies an 8 KiB
  window around each root-argument address and each vertex view into the arena before the draw. The block then holds
  GFX10 descriptors: V# 0x00bbde20, T# base 0x100001900 (word0 0x01000019), V# 0x016adc40. The texture at
  0x100001900 (XMem reservation at 0x100000000, above the 4 GiB arena) is all zeros in title memory too: nothing
  has drawn into the UI layer yet. So the black is real: the title composites an empty layer each frame; its
  frontend (boot screens / intro video / menus) has not produced anything. Next: what should fill it, and what the
  title waits for before it does. (Also: textures above 4 GiB cannot be reached through heap_base + VA at all.)
- Device slot 86 is the stream CreatePipelineState (desc, riid, out) like slot 57 (VIctory device.rs f[86] =
  pipeline::create_stream) and now shares slot 57's handler (it answered E_NOTIMPL: every pipeline made through it was
  missing; with it the title adds a second draw per frame, VS_BlitInteriorStencilQuads). Slot 81 is frame statistics
  (token, kinds, count*, out): count 0, S_OK (VIctory frame.rs).
- The console list slots dropped as "not forwarded" (70, 74, 75, 126, 131/132, 142) are, in this title's layout
  (2020-era + 2 from slot 59), depth bounds, wait-value, PS depth order, shading rate, sync: VIctory ignores them too.
- Service-call trace ([svc] <function> from <caller>, first 3 calls of each runtime service function): the boot
  makes thread/net/user-change/PLM/display/accessibility/speech/title-id/sandbox calls, then signs in
  (user_add_async/result, get_id, get_local_id, duplicate, close) and makes NO further service call. The main thread
  waits each frame on a frame-slot semaphore (0x1475e0e78, normal pacing; DeepSeek analysis). Next: what the boot flow
  waits for after sign-in.
- Boot state machine (static analysis by a subagent): dispatcher 0x141D83960 (jump table 0x14674C120), called each
  frame from 0x1421B6CBC; state dword 0x1498F828C via setter 0x141AC7760; state 1 set by 0x1459965B0 -> 0x141C99520.
  States 3 (sign-in) -> 4 -> 5 (queue a profile-load task) -> 6/13 (spin while task status 0x1478B2818 is 2/3). The
  profile task (inner state 0x1478B4AC0) waits in 0x142894220 for an ACTIVE PAD: dword 0x146C31B50 <= 3 (initial -1,
  set only from the profile manager's event handler 0x142206150 when a controller pairs with the user), byte
  0x1498489C0 != 0, bit 0x02 of [0x1498482B8 + pad*0x150 + 0x144]. With no controller that never happens; a hack now
  makes pad 0 active from present 60 (LEONIDA_CE_NO_PAD_HACK=1: off). [boot] lines (every 300 presents) log them.
- But measured: the state dword stays 0 and none of 0x1421B6CBC / 0x1459965B0 / 0x141C99520 / 0x1421B8F00 runs.
  0x1421B8F00 (tail-jump to the 'state 1' entry) is registered as a named callback (hash 0x672FD9D1, registry at
  obj+0x5A0, add via 0x141D674F0) by 0x145B47FF0: the boot machine starts only when that entry is activated.
- Windows: the vertex/ring sync read a garbage vertex VA (0xfffffffd1ab52500) and the fault was fatal there; title
  copies now check VirtualQuery first (only committed memory or reserved XMem views). Console format 65
  (A8_UNORM, a 4K render target; core Vulkan has no A8) maps to R8_UNORM (French-General, issue #2). XDisplay is
  reported available and the sandbox-id query only needs room for the string, as in VIctory.
- Black screen, diagnosed (subagents, with confidence ratings):
  - Frontend gate (Very likely, measured on Windows): the loading-screen/frontend tick 0x142257700 runs every frame but
    returns early: flag 0x147453ED4 (bit 0; ED5 next to it) stays 0. Only 0x141C996B0 sets it, called only from the
    boot state machine (0x1421B745E, 0x1421B8E02, 0x1459965C8). So the movie manager update 0x143A1AEB0 never runs,
    no movie is requested, BinkOpenWithOptions (IAT 0x146794210) is never called (Bink itself is initialised), and
    nothing renders into the UI target at 0x100001900. Poking the flags by hand opened the gate once, then the thread
    blocked in 0x1406F97C0 (critical section [0x149E25800]+0x1E0) and presents stopped: not a viable hack.
  - PLM gate (Definite: not the cause): the main thread's lifecycle wait (0x1421B8A3E..A56, byte 0x1471B9048 == 2)
    passes on its first check, with or without a PLM shim. shims/plm_ce.cpp now accepts the app-state/constrained
    registrations and never calls them (as a console leaves a running title alone); harmless, kept.
  - Boot node (in progress): 0x145B47FF0 builds a task-graph node (name hash 0x672FD9D1, body functor 0x1421C3B60,
    stored at obj+0x5A0 of the graph 0x141DECC30 builds) with edges to nodes 0xEA79F4B6, 0xB198EC07, 0x13B28C68,
    0x5FDC36D5, 0x198352B0, 0xA599B5CB; the body never runs, so the boot state machine never starts.
- BOOT UNBLOCKED (2026-11-24). Two causes:
  1. My own pad hack: it wrote byte 0x1498489C0, which is the online system's "already initialized" flag. Whenever the
     online-system init 0x143DA1560 ran after present 60, its step 0x143DA1F00 saw the flag set and failed; the init
     shut the system down and left the singleton at 0x1498C0F20 dangling, and 0x14502B2E0 (reached from the boot task
     0xB198EC07 via 0x1422082D0 -> 0x142208CA0) faulted on it (read of 0x40 at 0x14502B2F1, swallowed by the job
     worker). The node never finished, so the boot node 0x672FD9D1 (0x1421C3B60 -> 0x1421B8F00 -> state 1) never ran.
     Found by a subagent with probes and a 6-byte patch test (Definite). The pad hack is now opt-in
     (LEONIDA_CE_PAD_HACK=1); the title sets pad 0 and that flag itself.
  2. Pipeline creation: the startup graph creates 43,000+ graphics pipelines; compiling them at creation took many
     minutes (Windows: NVIDIA's compiler; cdb showed the job workers in pipeline creation). Graphics pipelines are now
     built on first draw (LEONIDA_CE_EAGER_PSO=1 restores creation-time compiles).
  Also: the per-submission placed-buffer sync is opt-in (LEONIDA_CE_BUFFER_SYNC=1): hashing hundreds of placed buffers
  per submission stalled the present thread (cdb + llvm-symbolizer: xs_refresh_placed_buffers); the vertex-sync
  readability walk runs only on the host path.
  Result on native Windows: boot state 1 by frame 600; new service calls save_init_async, user_get_gamertag,
  save_init_result, user_get_is_guest, user_get_age_group, user_check_privilege, save_container_info; compute
  dispatches; Scaleform UI pipelines (VBatchVertexCxformEAlpha / FVertexCxformEAlpha) and the first indexed draws.
  Then a crash at 78.8 s: +0x13360A3 reading -1 (the old walk crash site), next to fix.
- Tools now set up: cdb (Debugging Tools for Windows) on the VM (C:\zroot\dbgdump.ps1: non-invasive attach, !runaway,
  ~*k, !locks; output is UTF-16), Ghidra 12.2 headless on the GPU box (/root/ghidra-scripts/Decomp.java, JDK 25).
- THE WALK CRASH IS FIXED (2026-11-24). The crash at +0x13360a3 (and its bad-free / +0x538d68e / +0x538d0e6 forms) was
  a heap overrun by the title itself, caught in the act under Wine (probes, Definite):
  - The generated-texture request +0x1335a20 fails to lock the texture's own image (+0x1336230 -> +0x1336320, vtable
    +8 returns null on our runtime; why is still open) and falls back to a CPU staging image in format word 0x10000a
    (R32G32B32A32).
  - The title's pitch function +0x1555c10 (and size +0x1555b60) handles format&0xfff 1-4, 9 and >0x14 only: 0x0a gives
    pitch 0, size 0, a 0-byte image.
  - The generator fill +0x550e2d0 (a 256x1 float4 colour ramp, 4 KiB) writes it anyway, over the live 0xa0-byte request
    node 0xef0 bytes on: its vtable becomes 0x3f8000003f800000 (1.0f, 1.0f) and call *0xb0 at +0x13360a3 faults.
  Hack: xs_patch_fmt10_pitch jumps from +0x1555c29 to a stub that returns width*16 for format 0x0a
  (LEONIDA_CE_NO_FMT10_PITCH=1 off). Native Windows: no exception in 600 s, boot state 3, frame 6300, speech streaming.
  Not the cause: failed compute pipelines (an empty placeholder for untranslated compute programs is in anyway,
  LEONIDA_CE_NO_CS_PLACEHOLDER=1 off). LEONIDA_CE_SKIP_LUT never helped because it nops two of the four row copies.
- Graphics pipelines now build in the background on a pool of threads (half the cores); a draw whose pipeline is
  not ready yet is left out instead of freezing the frame (or failing the whole list). LEONIDA_CE_SYNC_PSO=1: at the draw.
- Probe mode LEONIDA_CE_PROBE_TEX=1 (or ",tex" at the end of LEONIDA_CE_PROBE) logs generated-texture requests
  (+0x1335be6 / +0x1335c05 / +0x133609a): image mips against the node and whether its vtable survives.
- Open: why the texture lock fails (the console takes the direct path); the screen is still black at state 3.
- Compute shader coverage (2026-11-24): the runtime had dumped 2,680 untranslated compute containers. Parallel pass:
  2,675 unpack, 1,536 translate (was 621); in-game untranslated programs 2,064 -> 1,148. Use rdna2spirv --shard i/n
  and --merge for parallel runs (plain parallel runs corrupt the shared index.json).
  Of the 1,139 failures, 938 are "fall-through into undecodable words" and about 500+ of those are one thing: a 4-byte
  word 0xF800xxxx that llvm-mc (gfx1030) decodes as an 8-byte EXP, desyncing the rest of the listing (30,343
  occurrences; the dword after it is always ordinary code, e.g. s_waitcnt 0xBF8CC07F). The low 16 bits look like byte
  offsets (0x1160, 0x540, 0xc20, 0x470: multiples of 16; 224 distinct values) and it often sits inside s_clause next
  to s_load/s_buffer_load: probably an Xbox-GPU-specific compact scalar memory op (load or prefetch). Unknown semantics:
  treating it as s_nop would be a guess. Other failures: CS thread-group-size SGPR (90), s_bitreplicate on a lane mask
  (26), ds_append (23), branch into data (20).

## agSA (sender loop, 2026-11-24 evening)
- LEONIDA_CE_RING_IN_ARENA=1 now also registers the 64 MiB ring (arena VA 0x10000) as a ResourceBinding over the arena
  VkBuffer (reserve_console_upload_ring), so buffer_at/host_at/RECTLIST resolve ring VAs. Wine sa1 and Windows rb1:
  PAINT=0x100001900 presents 100% ff7fbf40 at frame 600. Windows composite path works with it (Definite).
- After boot state 3 (Windows frame ~1200-2400; Wine never gets there, stalls ~frame 700) the UI layer gets no draws;
  a new 3840x2160 target at 0x100053700/0x100053800 (run-dependent) gets 19 draws/frame. Presented, 0x100000100 and
  it all read back exactly 1/8 bytes non-zero: pixels (0,0,1)-ish dither, i.e. near black.
- TDIAG and RTDIAG_PAINT accept <lo>-<hi>; TDIAG range also logs textures in range bound for sampling.

## agA2 (sender loop restart, 2026-11-24 night)
- rc3 (build 3): 0 RECTLIST misses, every topo-17 draw takes the IA expansion. The state-3 RECTLIST is
  VS_Blit_a250f417 with NO pixel shader (colour output undefined in Vulkan; the old (0,0,1) upper-left triangle).
  Its table at root = V#(base = IA view, 0x60 bytes) + the same 3 vertices (-1,1),(-1,-1),(1,-1) stride 24.
- rb2b/rc3: PAINT on the state-3 target (0x1000537xx..53axx) makes presented 100% teal: it IS composited (Definite).
- State-3 content = Scaleform: %VVertexCxformEAlpha / _VBatchVertexCxformEAlpha (~17 draws/frame). Batch entries
  (64 B: cxform mul, add, matrix rows 0/1) in root V#0: 37 entries mul rgb 1.8e-7 (black), 9 entries 0.976 (white),
  translations y=-0.92, x 0.29..0.76 (text along the bottom of the screen).
- rc5: on native the ring snapshots xs_gpu_address makes at IASetVertexBuffers/IASetIndexBuffer time go STALE for the
  Scaleform buffers (vertex 1.1 MB, index 393 KB, title d84c00780/d84d20800). LEONIDA_CE_SNAP_RESYNC=1 re-copies them
  at replay. Wine never shows it.
- Pre-existing crash: xs_refresh_placed_textures -> xs_upload_placed_texture -> GetDesc on a released stand-in
  (vtable 0, read 0x50); guarded in build 6.
- New env: LEONIDA_CE_RECT_VS (moot), LEONIDA_CE_RECT_TABLE_SRC (default on), LEONIDA_CE_SNAP_RESYNC,
  LEONIDA_CE_CXFORM_FORCE (experiment).

## agA3 (sender gen 3, 2026-11-24 night)
- Scaleform input layout mismatch (Definite, from PSODIAG under Wine, sc1): _VBatchVertexCxformEAlpha / %VVertexCxformEAlpha
  declare COLOR0 R32G32B32A32_FLOAT, COLOR1 R32G32B32A32_FLOAT, POSITION0 R32G32_FLOAT, all APPEND_ALIGNED (0/16/32,
  stride 40), but IASetVertexBuffers gives stride 16 and the bytes are {rgba8 colour, rgba8 factor, float x, float y}
  (rc6 tdiag: ffffffff ff004a00 c5a12800 c6f9f000). VTexTGCxform*: COLOR0 float4 + POSITION float2 @16 (same story).
  The VS (disasm) uses v10 = factor.z: index = v10*1020.04 + {0.1,1.1,2.1,3.1} -> entry*4 + row of V#0 (rows: cxform
  mul, add, matrix row0, row1), i.e. factor byte 2 = batch entry. factor byte 3 = edge-AA alpha (ff inner / 00 fringe).
  Positions are float, integral, in +-32767 (XY16i-style quantised local space); matrix ~4.8e-7*32767 = 0.016 NDC
  = ~30 px at 4K, i.e. glyph/icon sized. Earlier "byte 2 always 0" was only the first 4 vertices of each draw.
  Fix: VS names containing "Cxform" get COLOR* as R8G8B8A8_UNORM (attrs 0/4/8), default on, LEONIDA_CE_SF_VCOLOR=0 off.
  LEONIDA_CE_VCOLOR_FIX takes a comma list now.
- Crash game+0x55e2ef6 (rb2, rc4, rc7) = the title TLS callback (game+0x55e2ebc) on DLL_THREAD_DETACH (edx==3):
  mov r14,[gs:58][tls_index]; mov rdi,[r14+0x10] with r14 == 0 (the thread has no block in the slot). Called from
  pe_loader on_host_thread_event -> run_callbacks. Hack: thread attach/detach callbacks run under SEH (AV only),
  "[hack] TLS callback .. faulted"; LEONIDA_CE_TLS_CB_SEH=0 off.
- rc7b: SNAP_RESYNC run reached state 3 at 1200 then hung (like rc5b); all probes 0. Added a hang watchdog
  (LEONIDA_CE_HANG_WATCH, default on): no present for 15/45/120 s -> "[hang]" per-thread stacks.
- rc8 (build 8 = layout fix): FIRST Scaleform output on Windows (Definite): presented = full-screen (32,32,44) =
  vertex colour ff2c2020 of the first %VVertexCxformEAlpha draw (matrix 3.05e-5 = 1/32768, so a +-32767 quad = whole
  screen), plus a 1-2 px white anti-diagonal (seam/edge-AA of that quad, not understood). Correction to agA2: the
  batch entry rows are {add ~1e-7, mul 1.0 / 0.976, matrix row0, row1} in effect: the GPU output equals the vertex
  colour, so the "1.8e-7 = black" reading was wrong.
- rc8/rc8b: the stall at state 3 is NOT the GPU readbacks (rc8b has DUMP=0). Busy game thread at both stalls:
  game+0x164f01 -> slot 18 (CreateShaderResourceView) -> xs_upload_placed_texture (detile memcpy / WC readback loop),
  which also blocked up to 2 s per texture on a fence of the title's own queue. Build 9: no blocking wait (objects
  freed when the fence completes; LEONIDA_CE_UPLOAD_WAIT=1 = old), sparser WC sampling, NO_PLACED_REFRESH really works
  now (was compared with 1, not '1'). Only one [hang] block was ever printed and the log froze, so the watchdog now
  writes with WriteFile to C:\zroot\logs\hang.txt (LEONIDA_CE_HANG_FILE) at 15/30/60/90/150 s.
- rc9 (build 9): no pre-state-3 stall any more (upload wait gone), state 3 at 1200, same navy frame, then no log
  lines at all and NO watchdog output (hang.txt never created) => presents kept coming (the heartbeat is in PresentX),
  so it is a crawl, not a deadlock (Likely). Suspect: SNAP_RESYNC memcmp + zero scan READ the ring (GPU-mapped,
  likely write-combined) ~1.5 MB per Scaleform draw. Build 10: blind write-only title->ring copy after the first 64
  views (LEONIDA_CE_SNAP_RESYNC=2 = old compare), plus "[pace] frame N, X ms for the last 60" lines (WriteFile) from
  frame 1080. Every state-3 run with SNAP_RESYNC=1 stalled; runs without it (rb2b, rc3) ran to 4500.
- rd1 (receiver B3, build 9, RTDIAG off): DEADLOCK at frame 329 (Definite, cdb): xs_dump_thread_stacks
  (LEONIDA_CE_STACKS=1, frame 300) suspended a game thread that was inside fprintf(stderr) (xs_upload_placed_texture
  log) and then called fprintf itself before ResumeThread -> CRT stderr lock held forever, every logging thread
  blocks. Build 11 resumes right after GetThreadContext. (Any SuspendThread + CRT/heap use is a deadlock hazard.)
- rd2 (build 11): NO freeze; frames run past state 3 (1200 -> 1620+), but ~6 fps from frame 1140 onward. B3 with cdb
  (2 samples): the submit thread (ExecuteCommandLists -> prepare_draw) burns a full core of KERNEL time in
  VirtualQuery (copy_title_bytes / title_readable, 2 queries per call, many calls per draw). Build 13: per-thread
  cache of VirtualQuery answers (200 ms), LEONIDA_CE_VQ_CACHE=0 off.
- PSO translation (slot 57, d3d12_xs_device.cpp ~4285) builds the D3D12 PSO from the shaders ONLY: blend off, depth
  off, cull none, RT R8G8B8A8, triangle topology (Definite; PSODIAG under Wine: all 13388 pipelines blend off).
  Build 12/13: Scaleform ("Cxform" VS) gets SRC_ALPHA/INV_SRC_ALPHA (LEONIDA_CE_SF_BLEND=0 off). The real fix is to
  decode the console PSO stream's blend/depth/raster state.
- Screen state (rc8..rd2): navy (32,32,44) full screen = Scaleform backdrop + a white anti-diagonal seam; nothing else yet.
- rd3 (build 13): VQ cache in vulkan_replay.h -> ~6 s per 60 frames (rd2 9-10 s; ~10 fps). SF blending made the
  screen BLACK (navy quad contributes nothing with SRC_ALPHA blending => PS alpha output is not ~1; unknown why), so
  build 14 turns SF blending OFF by default (LEONIDA_CE_SF_BLEND=1 on). rd3 cdb: remaining kernel VirtualQuery
  in xs_readable (xs_gpu_address :822 via list slot 48, xs_refresh_placed_textures :1546) -> build 14 caches positive
  xs_readable answers per thread (200 ms), same switch LEONIDA_CE_VQ_CACHE.
- Wine note: a first-chance AV at ntdll+0x563ac from gta6rt+0x19371 (thread gta6rt+0xe77d2) appears ~110 s in sc5 and
  sc7 (builds 10 and 14); not fatal there, not investigated.
- Recommended env (Windows): RING_IN_ARENA=1, CB_FIX=1, VCOLOR_FIX=sga_dr_vs, SNAP_RESYNC=1 (+RTDIAG for probes);
  defaults on: SF_VCOLOR, TLS_CB_SEH, HANG_WATCH, VQ_CACHE, NOPS_NOCOLOR; SF_BLEND off.
- Next: (1) why the Scaleform content beyond the backdrop is invisible (glyph/text draws use VTexTG* + textures;
  check their textures and the cxform rows); (2) the white anti-diagonal seam; (3) decode the console PSO stream's
  blend/depth state instead of the slot-57 defaults; (4) remaining fps (pace lines).
- rd4 (build 14, receiver B3): [pace] ~2.4-2.6 s per 60 frames (~24 fps; rd2 6 fps), state 3 at 1200, navy back (probe 1200 = ff2c2020). Build 14 is the current best.

## agA4 (sender gen 4, 2026-11-24 late night)
- Console PSO record (slot 57/86, 0x160 B): w[0] root sig, w[1..12] six {ptr,len} blobs; #2/#4 are VS/PS code,
  #1 = input signature (POSITION/COLOR/TEXCOORD names), #3/#5 = per-stage resource tables, #0 (0x17c or 0x184 B) =
  GCN context registers at FIXED offsets (Very likely, from 3365 records under Wine sd1):
    +0xd0..+0xef CB_BLEND0..7_CONTROL (0x80000000 = off + DISABLE_ROP3; 0xe5010504 = SRC_ALPHA/INV_SRC_ALPHA colour,
                 ONE/INV_SRC_ALPHA alpha; e5010508 = DST_COLOR (multiply); e1000104 = SRC_ALPHA/ONE (additive))
    +0xcc CB_TARGET_MASK (ffffffff normal, 0 = no colour writes), +0x78 CB_SHADER_MASK-like (0xf)
    +0x3c DB_DEPTH_CONTROL-like (0x10 off/LESS, 0x12 test, 0x14, 0x18 on all Scaleform), +0x38 082c034x (raster? unknown)
  Every Scaleform shader pair exists in ~18 state variants (tmask 0 mask draws, alpha, additive, multiply).
- Build 15: RT0 blend + write mask come from blob #0 (LEONIDA_CE_PSO_BLEND=0 / LEONIDA_CE_PSO_WMASK=0 off).
  "[psodec]" lines log the decode; pipeline-name logs end in " [cb0 .. tm .. db ..]". Depth/stencil not translated.
- State 3 on Windows (rd4) uses only two Scaleform pipelines: %VVertexCxformEAlpha (backdrop, 30 indices) and
  _VBatchVertexCxformEAlpha (vector shapes, incl. the bottom-right text batch). No VTexTG* glyph pipeline is drawn.
- Scaleform PS (%FVertexCxformEAlpha): rgb = sat(col*param3 + param2), a = sat(col.a*param3.a + param2.a)*factor.a;
  batch VS: param2 = entry row 0 (add), param3 = row 1 (mul), rows 2/3 = matrix.
- rd5 (build 15, real blend): unchanged screen, navy + white diagonal; dumps hold exactly 2 colours. The rd3 "black
  with SF_BLEND" is explained by rd6a/b: with the navy draw (30@0) dropped, the frame is black + the diagonal.
- Build 16: LEONIDA_CE_SKIP_DRAWS="count@first,.." drops matching draws at replay (experiment switch).
  rd6a (30@0,60@32 dropped) / rd6b (30@0): black + the SAME white diagonal => the diagonal is not the backdrop seam.
- Diagonal = the text (Likely): the batch VS reads rows {add, mul, m0, m1} of entry e with buffer_load_dwordx4 idxen
  (index e*4+row) through root V#0, but the title V# there has stride 0 (e.g. 01421800 00000000 00000600 30014fac,
  num_records in bytes), so every row = row 0: m0 == m1 -> x == y -> every glyph lands on the corner-to-corner
  line; white entries (add 0.976 + col*0.976) saturate to 255. rdna2spirv honours V# stride exactly, so the
  console V# must carry stride 16 from somewhere we do not emulate (not found).
- Build 17/18: LEONIDA_CE_SF_STRIDE (default on, =0 off): for pipelines whose VS name contains "VBatch" (table
  filled at slot-57 create; 512 entries were too few in 17, 16384 in 18) the replay ORs stride 16 into that V#
  word 1 in title memory before the draw ("[hack] SF_STRIDE" lines).
- rd8 (build 18, receiver B3): FIRST REAL UI (Definite). SF_STRIDE fired; diagonal gone; presented dump shows the
  bottom-right prompt bar "ACCOUNT PICKER (Y)  SCREEN NARRATION (OFF) (X)  PLAYER (A)" (white text with black
  outline, round button glyphs, AA edges) over the navy backdrop, px x 2461-3762, y 2044-2104
  (/root/rb/rd8-crop-a4.png). State 3 = the account/profile prompt, waiting for a button press.
- Recommended env (Windows): RING_IN_ARENA=1, CB_FIX=1, VCOLOR_FIX=sga_dr_vs, SNAP_RESYNC=1; defaults on:
  PSO_BLEND, PSO_WMASK, SF_STRIDE, SF_VCOLOR, TLS_CB_SEH, HANG_WATCH, VQ_CACHE, NOPS_NOCOLOR; SF_BLEND off (superseded).
- Next: input (press A/Y) to move past the prompt; stencil/depth (DB_DEPTH_CONTROL +0x3c, Scaleform mask variants with
  tmask 0) not translated; the stride-0 V# probably affects other idxen shaders too (a generic fix would find where
  the console derives the stride, likely blob #3/#5 resource tables, which carry 0x10 values).

## agA5 (sender gen 5, 2026-11-24/25 night)
- Input path (Definite, Wine se1): the title registers GameInput device callbacks (kind 0x40000 gamepad, then 0x10,
  status filter 0x1 connected, blocking enumeration) and once it runs, polls GetCurrentReading(0x40000, <that device>)
  + GetGamepadState once per frame. No reading callbacks. The account picker itself is already handled: XUserAddAsync
  without the silent flag returns the offline default user (xgameruntime_user.cpp).
- Build 19: LEONIDA_CE_PRESS="BTN@when[/every],..." (gameinput.cpp, agA5 HACK): when = seconds since the first poll or
  "s3+N" = N s after boot state 3 (dword 0x1498F828C, read by the shim with VirtualQuery). Buttons A B X Y UP DOWN LEFT
  RIGHT LB RB MENU VIEW; hold LEONIDA_CE_PRESS_HOLD_MS (250). Setting it (or LEONIDA_CE_VPAD=1) forces the keyboard pad
  (SDL virtual gamepad) to "always", so a connected pad exists from the first poll. "[press]" lines log state changes,
  each press/release with title call counters, and "title saw buttons 0x.." when GetGamepadState returns a press.
- Wine (se1) reached state 3 at t=97 s, then stops presenting (the known Wine stop), so presses are Windows-only tests.
- re1 (build 19, receiver B3): the title SAW the presses ("title saw buttons 0x4"); press #2 moved boot state 3 -> 13
  (Definite). State 3 (0x1421b7070) loops until FUN_14041e8c0(0x1478ac9c8, [0x1478acba0]); state 13 (0x1421b7530)
  is the loading loop (waits on hash 0xbaab6434 while ticking frames, then 0x10 or 0). A Bink movie pipeline started.
  Then heap corruption 0xc0000374: proxy_slot<70> (placed SRV) -> CreateShaderResourceView -> Object::from(res) ->
  QueryInterface+Release on a stand-in the title had already released -> second delete (Definite, symbolized).
- Build 20: g_placed holds its own reference on every recorded resource (LEONIDA_CE_PLACED_REF=0 off) and is a deque
  (xs_find_placed pointers used to dangle after push_back). "xN" press limit. LEONIDA_CE_RTDIAG_EVERY (build 21 fixes
  the caller's own %300 gate).
- re2 (build 20, receiver B4): NO crash; one A press -> state 13 -> a first-boot settings page (Definite, dump f1800):
  navy backdrop, centre title "NAVIGATION EXPERIENCE", selector "<  (   )  >" with the option text missing, 3-dot
  pager (first filled), bottom-left rounded preview box with only a north/player-arrow icon, a caption with most
  glyphs missing ("Mi i  p di pl y dy  i lly  d  d"), bottom right "CONTINUE (A)". [boot]: state 13, task 4, inner 7.
  In state 13 the title stops calling GetCurrentReading (reads frozen), so later presses are not seen.
- The pad poll (Wine se3): GetCurrentReading(0x40000, dev) is called from 0x142211ba3, a method at ~0x142211b40
  (obj+0x20 device, obj+0x28 lock); GameInputCreate from 0x1459258c8 (singleton getter 0x1459258a0).
- dstorage comp=14 (= BCPack, ttdb_graphics.ttpf) is unimplemented in the runtime (E_NOTIMPL); tools/bcpack.py has a
  decoder. Build 21: LEONIDA_CE_DS_ZEROFILL=1 answers them zero-filled with success (untested).
- Build 21: LEONIDA_CE_STACKS_AT=<frames> dumps all thread stacks at those presents; more [press] call logging.
- re1 (build 19, receiver B3): the title SAW the presses ("title saw buttons 0x4"); press #2 moved boot state 3 -> 13
  (Definite). State 3 (0x1421b7070) loops until FUN_14041e8c0(0x1478ac9c8, [0x1478acba0]); state 13 (0x1421b7530)
  is the loading loop (waits on hash 0xbaab6434 while ticking frames, then 0x10 or 0). A Bink movie pipeline started.
  Then heap corruption 0xc0000374: proxy_slot<70> (placed SRV) -> CreateShaderResourceView -> Object::from(res) ->
  QueryInterface+Release on a stand-in the title had already released -> second delete (Definite, symbolized).
- Build 20: g_placed holds its own reference on every recorded resource (LEONIDA_CE_PLACED_REF=0 off) and is a deque
  (xs_find_placed pointers used to dangle after push_back). "xN" press limit. LEONIDA_CE_RTDIAG_EVERY (build 21 fixes
  the caller's own %300 gate).
- re2 (build 20, receiver B4): NO crash; one A press -> state 13 -> a first-boot settings page (Definite, dump f1800):
  navy backdrop, centre title "NAVIGATION EXPERIENCE", selector "<  (   )  >" with the option text missing, 3-dot
  pager (first filled), bottom-left rounded preview box with only a north/player-arrow icon, a caption with most
  glyphs missing ("Mi i  p di pl y dy  i lly  d  d"), bottom right "CONTINUE (A)". [boot]: state 13, task 4, inner 7.
  In state 13 the title stops calling GetCurrentReading (reads frozen), so later presses are not seen.
- The pad poll (Wine se3): GetCurrentReading(0x40000, dev) is called from 0x142211ba3, a method at ~0x142211b40
  (obj+0x20 device, obj+0x28 lock); GameInputCreate from 0x1459258c8 (singleton getter 0x1459258a0).
- dstorage comp=14 (= BCPack, ttdb_graphics.ttpf) is unimplemented in the runtime (E_NOTIMPL); tools/bcpack.py has a
  decoder. Build 21: LEONIDA_CE_DS_ZEROFILL=1 answers them zero-filled with success (untested).
- Build 21: LEONIDA_CE_STACKS_AT=<frames> dumps all thread stacks at those presents; more [press] call logging.
- re3 (build 21): presses read in state 13 again (all from 0x142211ba3; never GetUiNavigationState or Next/Previous).
  One A: state 3 -> 6 -> 13. The presses step through a first-boot setup flow (Definite, dumps): NAVIGATION EXPERIENCE ->
  UI TEXT SIZE -> SUBTITLES -> BRIGHTNESS (slider, ADJUST / RESTORE DEFAULT / CONTINUE / BACK) -> FINISH SETUP? (pool
  and flamingo art, SETTINGS / CONTINUE (A) / BACK (B)) -> LEGAL NOTICES ("Photosensitivity Health Warning" item,
  "Next >" button, COPYRIGHT, purple building art, BACK (B), and a white anti-diagonal again). It stays on LEGAL NOTICES
  (A presses do nothing visible) to f7800. Boot state stays 13 (task 4, inner 7). ~20 fps. Body text (Scaleform glyph
  text) misses most glyphs on every page; selector option text is blank. The headings (vector text) are complete.
- Build 22 hashed every placed texture <= 4 MiB in full for the 30-present refresh: re4 dropped to ~3 fps (abandoned).
  Build 23: full hash only for A8/R8 (glyph-cache candidates), plus SF_STRIDE looks for the batch V# (word 3 0x30014fac)
  in root slots 1..7 as well (VBatchTexTG*), for the diagonal on LEGAL NOTICES.
- The pad-scan for "who pressed" is 0x1406aece0 (GetGamepadState via vtable +0xb0, buttons mask 0xffffffdf); the
  general pad update 0x1407a4150 is virtual.
- re5 (build 23): pace 13-19 fps; no diagonal anywhere (also on LEGAL NOTICES); SF_STRIDE only ever hit slot 0. DOWN
  moves the LEGAL NOTICES list focus (Photosensitivity Health Warning <-> Copyright; the right panel shows the notice:
  "PHOTOSENSITIVITY HEALTH WARNING / IMPORTANT HEALTH WARNING READ BEFORE PLAYING" + body). Body glyphs NOT fixed by the
  A8/R8 full hash. Pattern (Very likely, from many captions): the body font draws exactly the glyphs with ascenders or
  descenders, capitals, digits and (c) (i l t f h y d g p b k, A G R T I V S C ...); every x-height-only lowercase glyph
  (a e o n s r c u m w v x z) is missing. Suspects: glyph-cache region for short glyphs never uploaded (the packer groups
  glyphs by height), or a per-glyph size/clip test. Build 24 logs "[glyph?]" candidate textures.
- re5 continued (B4): LEGAL NOTICES -> (DOWN to "Next >", A) -> ROCKSTAR GAMES LEGAL TERMS & POLICIES (crocodile art;
  PRIVACY POLICY checkbox, TERMS OF SERVICE checkbox "I have read and agree to the Terms of Service", greyed "Confirm >";
  right pane shows the policy text; SCROLL / SELECT (A) / BACK (B)). A on an item ticks it and moves focus down; the
  scripted DOWN then skipped the ToS box, so re5 ended on a greyed Confirm. Some 250 ms presses were never seen.
- re7 plan (build-25.txt): env-only, same exe as 24: A/DOWN alternation x16, then UP, A, A every 30 s; hold 350 ms.
- re6 (build 24): no crash; did not get past TERMS. ToS item after an A tap shows a small ring/progress icon that does not
  stay; Confirm stays greyed (hold-to-confirm suspected, Plausible). Presses after t~484 s were not seen (nz frozen while
  reads continue; not understood). Glyph-cache candidates: va de17f0000 2048x2048 A8 and de1d4a000 1024x1024 A8 (layout
  0x100 = linear), both Map()ped by the title (PC side R8); 3840x2160 A8 dd7c40000.
- BUG found: the shim's env() helper reads at most 63 chars, so a longer LEONIDA_CE_PRESS did not force the virtual pad
  (re7 wasted). Build 26: the pad check uses the full variable; "BTN:ms@.." per-press hold; "[refresh] A8 re-upload" log.
- re8 (build 26, 600 s, receiver B4's last run): reached ROCKSTAR GAMES LEGAL TERMS & POLICIES again; ended (f6600-f8400)
  with focus on the PRIVACY POLICY box (filled square), the ToS row showing the small speaker-like icon, Confirm greyed:
  the UP / A(2.5 s hold) / A(2.5 s hold) cycle did not enable Confirm. No crash. "[refresh] A8 re-upload": 9 x the
  2048x2048 A8 at de17f0000 (glyph cache IS re-uploaded; 1024x1024 A8 never), yet the x-height glyphs stay missing, so
  the glyph problem is not (only) refresh: suspect the A8 upload/detile (layout 0x100, swizzle 0, R8 on PC) or the
  glyph draw itself.
- Next for whoever continues: (1) TERMS screen: find the checkbox semantics (try A taps on the ToS item without moving,
  RIGHT, or scroll the policy text to the end with the right stick first: "SCROLL" prompt; many EULA screens require
  reading to the bottom before the tick is accepted -> add stick axes to the press script). (2) Body glyphs: dump the
  de17f0000 texture (title memory vs uploaded) and check which glyphs are present.
- Recommended env (Windows): RING_IN_ARENA=1, CB_FIX=1, VCOLOR_FIX=sga_dr_vs, SNAP_RESYNC=1, plus for input
  LEONIDA_CE_PRESS=A@s3+20/15x16,DOWN@s3+27/15x16 (+ LEONIDA_CE_PRESS_HOLD_MS=350).
- CORRECTION (re8 final from B4): Confirm WAS enabled once (f5400, /root/rb/re8d/p005400.png: both boxes ticked = the
  speaker-like icon is the TICKED state, Confirm bright and focused). A taps toggle the checkboxes; no hold is needed.
  The next scripted UP moved focus back to Privacy and the A unticked it, so the UP/A/A cycle kept toggling Privacy.
  Next script: after the 16 A/DOWN cycles, press A alone (no UP) every 30 s, e.g.
  LEONIDA_CE_PRESS=A@s3+20/15x16,DOWN@s3+27/15x16,A@s3+272/30  (build 26 exe, env only). The earlier guess that the
  policy text must be scrolled first is withdrawn.

## agA6 (sender gen 6, 2026-11-25)
- Missing x-height glyphs, cause #1 (Very likely, re8 tdiag): those glyphs are NOT vector shapes but glyph-cache quads
  drawn with VText_a4ab6e63 + FText_a4ab6e63 (pipeline 21797b360b0, 12-22 quads per draw). Their vertices are stride 20
  {rgba8 colour ffe6e6e6, float2 uv (1/2048 units), float2 pos} but the layout (sc5 psodiag) says COLOR0 R32G32B32A32,
  TEXCOORD0 R32G32, POSITION0 R32G32 APPEND_ALIGNED (0/16/24): uv and position came from the wrong bytes. The glyphs
  with ascenders/descenders and capitals are big enough that Scaleform draws them as vector shapes (_VBatchVertexCxform),
  which is why only they showed. Build 27: SF_VCOLOR default also matches "VText" (LEONIDA_CE_SF_VCOLOR=0 off).
- Cause #2 (Definite by reading): FText samples dmask 0x8 (.w only) from T# 0x100001b00 (2048x2048, FMT 8_UNORM,
  DST_SEL (X,0,0,1)): the xs layer turns A8 (65) views into R8 with the title's identity mapping, so .w read 1 (would
  give solid boxes once positions are right). Build 27: slot 18 gives A8 SRVs mapping (0,0,0,R) composed with the
  title's ("[hack] A8 SRV: .. mapping 1688 -> 1124"; null descs of placed A8 textures get a Texture2D desc).
  LEONIDA_CE_A8_SWZ=0 off.
- TERMS: A on a box ticks it and moves focus down; A on a greyed Confirm does nothing. re9 (build 26) ended with both
  boxes unticked and focus on Confirm. Script now cycles UP,UP,A,A,A every 30 s plus UP,A every 60 s (parity breaker).
- dstorage comp=14: destinations of consecutive failing requests are 0x30 apart with dst_size 65536 (re8), so the +8
  field is probably not a flat memory destination for BCPack requests: LEONIDA_CE_DS_ZEROFILL=1 would write overlapping
  64 KiB ranges (UNSAFE, Plausible). Build 28 logs the raw 0x48-byte request for comp>1 ("[dstorage] comp=14 raw req").
  The decoder (tools/bcpack.py, 1488 lines + tables, some entries provisional) was not ported.
- Bink: Bink2Scarlett.dll is the title's own; VS_Bink/PS_BinkNoAlpha pipelines already draw in state 13 (re8).
- re9 (build 26, B5): TERMS reached, ended with both boxes unticked, focus on greyed Confirm; body glyphs still missing.
- re10 (build 27, B5): GLYPHS FIXED (Definite): /root/rb/re10d/p002400.png "< Minimap (Dynamic) >", "Minimap displays
  dynamically or on demand.", TERMS labels/Confirm/prompts complete. ~13.8 fps median. No crash.
  CORRECTION to agA5: on TERMS, A toggles the focused box and focus does NOT move (re10 frames: UP,UP,A,A,A only flipped
  Privacy). Filled white square = focused+unticked? / tick icon alternate on each A.
  REGRESSION: TERMS right-pane policy body and the Privacy row label vanished (both scroll/masked text fields).
  Suspect (Plausible): the 3840x2160 A8 render target (va dd7c40000, flags 0x300001) is a mask; on PC it is R8 and stores
  the shader's .r, so the A8 swizzle reads 0 there. Build 29: A8 SRVs of resources with ALLOW_RENDER_TARGET keep the R8
  mapping ("[hack] A8 SRV: res .. is a render target"); LEONIDA_CE_A8_SWZ=2 = build 27 behaviour, =0 off.
- Build 28 = 27 + comp=14 raw request log. Build 29 = 28 + A8 RT exemption. Wine sf1/sf3 smoke OK (state 1),
  sf2 psodiag confirms VText attrs at 0/4/12 with R8G8B8A8 colour.
- TERMS script (re11/re12): cycle 40 s UP,UP,A,DOWN,A,DOWN,A (+ UP,UP,A every 120 s as a parity breaker).
- re11 (build 28, B5): PAST TERMS (Definite). The A/DOWN phase itself ticked both boxes and A #16 (t=308.6) pressed Confirm
  (p003900 both ticked + Confirm focused; p004050 right pane "CONFIRM / Confirm setup and start the game."). Boot state
  13 -> 0 at t=309.3 with a settings save (save_create_container/write/submit/close). p004200 = solid navy, no UI. Last
  present frame 4345, then AV at t=325.6: game+0x501eb50 `mov qword [rdi],0` with rdi = [0x14ab1a6c8]+0xa0 = 0xa0.
  Function 0x14501ea20 (called from 0x140164474) zeroes 0x14ab1a688/0x14ab1a6c8, then calls 0x144db7900(obj
  [0x14ab1a5c0], &0x14ab1a688) and later (obj [0x14ab1a5b8], &0x14ab1a6c8) to fill them (looks like a Map/acquire of a
  buffer); the second returned false -> null base -> crash. Next: find what 0x144db7900 wraps (probably a resource
  Map or an XMem allocation) and why it fails on PC. The re10 "vanished Privacy label" was focus-dependent (re11 shows it
  with the swizzle on); the long policy body is not seen in either build. dstorage comp=14 raw req (re11): dw0 0x0020000e
  (comp 14, byte 2 = 0x20 = dest type?), dw2/3 = dest 0xd849aa358 (0x30 apart = array of destination descriptors, NOT
  memory: zero-fill unsafe), dw6 0x10000 = dest size, dw8/9 file ptr, dw10 offset, dw12 compressed size.

## agA7 (sender gen 7, 2026-11-25)
- Post-TERMS crash (re11/re12, game+0x501eb50 write to 0xa0), Very likely: 0x14501ea20 (called via 0x140164474)
  calls 0x144db7900 = thunk to 0x145121550 = rage grcBuffer lock. It picks the resource record at [obj+(frame%n)*8];
  record+0x20 = ID3D12Resource*, and it calls vtable+0x40 = Map(sub, range, &p). A texture (flags&0x1f80 == 0x980) uses
  Map(0, layout range); otherwise Map(record sub, {0,size}). If HRESULT < 0 it returns false, the base stays null, and
  0x14501ea20 writes 0x80 x 0x5c0 bytes at base+0xa0 anyway. Our Resource::Map refused DEFAULT-heap buffers
  (mapped_ is only set for UPLOAD/READBACK) and textures without console_va_. On the console every heap is CPU-visible.
- Build 30: LEONIDA_CE_MAP_ANY (default 1). A refused buffer Map returns arena.mapped+va_ (placed/arena buffers:
  the same bytes the GPU reads), or vkMapMemory for host-visible own memory. =2 also hands anything else a zeroed CPU
  shadow (calloc; the GPU never sees it). Logs the first 48 refused Maps ("[hack] Map(..) .. -> arena|vkmap|shadow|fail").
  Wine sg1 (state 1): no refused Maps.
- re12 (B, build 29): the saved profile skips first-boot; Confirm at t~118, crash at t=136.4. Same stack.
- re13 (build 30, B, MAP_ANY=2): +0x501eb50 AV GONE (Very likely fixed). All 48 refused Maps were the same kind:
  DEFAULT-heap (heap=1) UAV buffers in the arena (RING_IN_ARENA), placed=0, 9 buffers (3 x 27440, 3 x 188416,
  3 x 1144064, triple-buffered), caller 0x14512176E, -> "arena". No vkmap/shadow/fail, so MAP_ANY=1 would be enough.
  Confirm at t=155.6 -> flat navy (#2c2020 BGRA). At t=180.8: int3 skipped at game+0xf7c5c (assert: range outside
  pool cpu [0x149e1dac0]=0x1800e0000 size [..c8]=0x600000000; gpu base [..d0]=0x7800e0000, from the Wine sg2 log),
  then dstorage worker memcpy (dstorage_x.cpp:228, comp=1) first-chance AVs at 0x1cd7d40000/0x1cd7f40000, int3 at
  game+0xf2cc4b (XMemMapPhysicalPagesScatter at 0x145f3fd20 returned FALSE; caller 0x140f2cc43), and
  "CopyPageMappingsBatchX: a page mapping failed (pool E52E00000, error 487)". Then no presents for 76 s; the process
  died at 261 s. 0x1cd7d40000 = 0xE52E00000 + 0xE84F40000: it looks like the tiled pool base was added to an address
  that was already absolute (Trust me bro).
- Build 31: diagnostics only. Scatter calls and failure reasons are logged, along with the pool globals. Dstorage
  logs "odd dst" for destinations that are free, reserved but not mapped, or in 0x1000000000..0x1fffffffff.
- re14 (build 31, B): EARLY crash in state 3 at t=80.9, before any press, never reached TERMS. It began with an
  int3 at game+0x2474606, a deliberate fatal path: call [0x1467939c8] = SetUnhandledExceptionFilter(rsi), then int3,
  i.e. a rage Quitf/crash-dump trigger; the vtable 0x420 call and 0x145f3fda0 come just before it. A cascade of AVs
  followed (0xd0002b0ff at +0x43a6b7a, exec 0, ...). Build 31 adds only logging; Scatter had no failures (only 7 calls,
  all at init), and there was no "odd dst" and no error 487. Probably a nondeterministic title fatal (Plausible); RTDIAG_EVERY=30
  makes frames slower, which may trip a watchdog (Trust me bro). Needs a rerun (re14b) of build 31, then build 30 if it
  repeats.
- Status: build 30 = the fix (MAP_ANY). The post-Confirm screen is still flat navy; what follows is a texture-streaming
  burst ~25 s later and the Scatter/CopyPageMappings failure, with diagnostics in build 31 still unread.
- re14b (build 31 rerun, B): passed Confirm again (13 -> 0 at t=158.1), so re14 was a flake (Likely). It had NO Scatter
  failures and NO odd dst. "CopyPageMappingsBatchX: a page mapping failed (pool E52E00000, error 0)": error 0 means
  no API failed, so a range check failed in copy_page_mappings_x (inside(): range.start+count > pool_pages, or a
  kind > 3), or map_alias returned false without SetLastError (Likely). Then at t=188.2 an int3 at game+0x4da6315 and
  an AV write 0xbf521b75 at +0x4da63f8, then no presents ([hang] 56 s; main game thread tid 3976 blocked in a wait at
  game+0x23cf729 <- +0x8126a7 <- +0x53ec040).
  NEXT: log each failing CopyPageMappingsBatchX range (batch dest, kind, start, count, pool_pages, which check
  failed), and decompile +0x4da6315/+0x4da63f8 and the wait at +0x23cf729.

## agA8 (sender gen 8, 2026-11-25)
- Texture pool mechanics (Definite, disassembly of game+0xf2c4xx..0xf2cd17 and 0x141e6c110):
  - 0x141e6c110(size) XMemVirtualAlloc-s three windows: the CPU title pool [0x149e1dac0] = 0x1800e0000 (24 GiB VA), its GPU
    mirror [0x149e1dad0] = 0x7800e0000, and the page pool [0x1471bfaa8] = 0xe52e00000.
  - It allocates 99776 physical pages and XMemMapPhysicalPages-es them all at the page pool; device slot 74 registers the
    pool (va, pages).
  - The title keeps the pool's free list itself: head dword 0x1471bfcd8, tail +4, free count 0x1471bfce0, links in a
    packed 21-bit array at [0x1471bfcd0], 0x1fffff = end. Pool page j's physical page number /16 sits in the packed array
    [0x1471bfcc0].
  - GPU slot table [0x1471bfb08][idx]: pool page index in bits 20..40; the low 15 bits must be non-zero.
  - The only CopyPageMappingsBatchX caller is game+0xf2cc7f, with args (count, batches, pool = [0x1471bfaa8], pages =
    dword at 0x1471bfaca = size>>16, 0).
    - Map: batch.dest = gpu base + idx*64K, range {kind 1, start = free-list head, count 1}.
    - Unmap: {kind 2, start 0, count n}. The freed pool page comes from 0x14221ba60(idx), which returns -1 for an empty
      slot, and the -1 is pushed onto the free list unchecked.
  - The same pool page indices drive XMemMapPhysicalPagesScatter for the CPU view (0x140f2cc3e): entry {lo = pool page or
    -1, hi = cpu idx}, where -1 unmaps.
- re14b "CopyPageMappingsBatchX ... error 0" must come from a range check (Likely): kind 1 with start >= 99776, i.e. a
  free-list head of 0x1fffff or 0xffffffff (exhausted, or a -1 pushed by an unmap of an empty slot). re13's Scatter FALSE +
  487 fits the same bad index.
- game+0x4da6315 (Definite by reading): the int3 at the end of 0x144da62d0, which looks up the region (a {start,size} table,
  0x18 each) that contains a pointer. No region -> int3 -> returns 0 -> 0x144da6330 writes through the unrelocated
  pointer (AV at +0x4da63f8). Probably a streamed resource whose bytes are wrong (a consequence, Plausible).
- game+0x23cf729 (main thread wait): a 1 ms sleep loop until 0x1423cb280() is true, i.e. a render/stream sync. It is a
  consequence of the other thread dying.
- game+0xf7c5c: a range assert in 0x1400f7c20 for an address outside the CPU pool [0x149e1dac0, +[0x149e1dac8]).
- Build 32:
  - a log line per failing range, and periodic stats with the title's free-list head/tail/free;
  - map_alias failure reasons with the host VirtualQuery state;
  - int3_skipped events carry registers and a stack;
  - LEONIDA_CE_PAGECOPY_LENIENT (default 1): a failing range, or a source pool page that is not mapped, unmaps the
    destination (the console copies an invalid PTE) instead of failing.
  - Wine sh1: stats lines OK, 0 failed ranges, state 1.
- re15 (build 32, B6, 300 s):
  - Confirm at t=153.4. NO crash and NO hang to the 300 s stop (re14b died at 188 s), so the lenient page copies helped (Likely).
  - Boot stays "state 0 task 5 inner 5". About 12-14 fps, dipping to about 8 fps at t~165-190.
  - Free count never low (88-98k of 99776), so the pool is not exhausted (CORRECTS the guess above).
  - The only failures were 6 kind-1 maps at call 123 (t~180-188). Their destinations, 0x11800e0000 and 0x12300c0000..0x1230040000,
    are gpu base + slots 0xA0000..0xAAFFE. Those lie past the title's 0x60000-slot window, in host MEM_FREE memory.
  - Same slot on the CPU side: the int3 at game+0xf7c5c has r14 = 0xb800e0000 = cpu base + 0xA0000<<16, caller 0x145238556
    (stack +0x523857e <- +0x14c740 <- +0x50143bc .. worker +0x2215eca).
  - The slot comes from the title's slot buddy allocator 0x145238780:
    - order lists: dwords at 0x1471bfb24 + 4*order; max order [0x1471bfb1c];
    - table [0x1471bfb08], 8 bytes per slot: bits 15-19 order, 18-19 state, 20-40 pool page, 41-61 next;
    - count [0x1471bfb18] = 0x60000, seeded as order 18 @0 + order 17 @0x40000 by 0x14555b0f0.
    - The free path 0x14221b7d0 bounds-checks the buddy against count, so a slot >= 0x60000 means the lists were corrupted
      (a race on lock 0x1471bfad8, or a bad entry: unknown, Plausible).
  - Screen (/root/rb/re15d, montage /root/rb/re15-montage.png):
    - f840-1200: navy with footer "ACCOUNT PICKER (Y)  PLAYER (A)";
    - f1230-1710: near-black (17,17,17);
    - f1740-1950: plain navy;
    - from ~f1980: the presented image is ALL ZERO (black). Dumps are only written for non-zero images, which is why they stop.
      target0 stays navy (ff2c2020).
- Next:
  - Find who corrupts the slot buddy lists. Watch [0x1471bfb24..] for values >= 0x60000, e.g. from the CopyPageMappings hook
    (it runs after every streaming step), and log the first bad head with the table entries.
  - Or clamp: a slot past count -> treat as allocation failure. That needs a code patch of 0x145238780 (return 0x1fffff).
  - Then find why the presented image goes black after the page failure.
- re16 (build 32, env-only, 600 s planned):
  - Confirm at t=175.3, then an AV at t=223.3 at game+0x28c8f86 reading 0xffffffffffffffff.
    Stack: +0x56eba8 <- +0x2a896aa <- +0xfc247 <- +0x4f99784 <- ... <- worker +0x2215eca.
  - There were no CopyPageMappings failures, no f7c5c, and fewer than 128 calls.
  - Presents stopped at f2820 and the process died at 281 s.
  - So the post-Confirm stretch is still unstable, with a different site each run (nondeterministic, like re14). Screens as in re15.
  - target0/1 dumps are in /root/rb/re16t.
- Build 33 = build 32 + "[hack] slot buddy" watch (see /root/handoff/build-33.txt). Wine sh2 OK. re17 (build 33, 300 s) was handed to
  B at the end of agA8.

## agA9 (sender gen 9, 2026-11-25)
- re17 (build 33, B): Confirm at t=137.7. An A press at t=163.5 (state 0, "PLAYER (A)" screen) was followed 4 s later by
  CopyPageMappings call 125 (t=167.6). At that call the slot buddy heads were order 0 = 0xab1e8 and order 20 = 0x1f5b80,
  and the pool page free-list head was 0x124949 (> 99776). All were bad at once, and calls 1-124 were clean. Then the
  usual cascade: Scatter "unknown page 0xab5550", which is a consequence (the bad pool head indexed past the page array,
  Likely), f7c5c, f2cc4b, dstorage memcpy AVs into reserved stream heaps (0xc31fxxxxx) and reads of -1. Presents
  stopped at f1829 and the process exited at 243 s.
- Allocator facts (Definite, disassembly):
  - alloc 0x145238780(slots, power of 2), pop/split 0x145238830, free 0x14221b7d0, unlink 0x14221b910, push 0x145238960.
  - decref 0x1400e8890(slot): refcount in bits 0-14 of the block head; at 0 it tail-jumps to free.
  - sub-slot (4K) alloc 0x145238590 with 16 lists at 0x1471bfb7c; entries are marked 0xc0000 with a bitmap in bits 2-17.
    Sub-free is 0x144f8f0a0.
  - Lock 0x1471bfad8 is a recursive lock: owner = TLS[0]+0x1454 thread id, with a kernel semaphore slow path. It is held
    at every alloc and free caller (0x14221b9e0, 0x14014c640, 0x140f2c5xx unmap loop, 0x144f658c8, 0x144f8f330). So a
    lock race is unlikely (Likely). TLS is per-thread: 53246 gs:[58h] sites are patched and the tls sweep shows 0 shared.
  - The heap layout is table1 (count*8), then table2 [0x1471bfb10] (count*8), then the pool links: Wine shows
    table 0xd84479810, links 0xd84a79830. So slot >= 0x60000 reads table2, and slot >= 0xc0000 reads the pool links.
  - The free path's unlink 0x14221b9bb picks the list head to update from the entry's own order field. An entry whose order
    field disagrees with its list therefore corrupts another list's head. That is how an order-20 head can appear.
  - 0x144f8f330 (free by address) has an int3 for an address outside the pool, then falls into the free anyway:
    LEONIDA_SKIP_INT3 would free an out-of-range slot there. It was not seen in re17.
- dstorage is not the writer (Likely): every "odd dst" request is comp=1 with option word 1 (plain memory destination).
  comp=14 (BCPack) requests are refused and write nothing. Their "dst" values (0xd849a7b58.., 0x30 apart) fall inside
  table2's range, so the BCPack destination looks like a pointer to a per-slot descriptor (Plausible).
- Build 34: runtime/src/slot_guard.h, LEONIDA_CE_SLOT_GUARD (0 off, 1 default, 2 full).
  - It hooks the direct callers of alloc, free, decref, suballoc and subfree (remap thunks, arg 0 unchanged).
  - Before each op it drops bad buddy heads. Mode 2 also walks every list and cuts at the first bad node.
  - It walks the pool list every 32 ops, from head to tail (the title never writes the tail's link). A bad pool list is
    rebuilt from the slot table (pages named by refcounted slots are used, the rest are free), only from alloc, suballoc
    or subfree, never inside the unmap loop.
  - At the first bad state it prints the last 32 ops (kind, arg, tid, tick), which separates an allocator op from an outside writer.
  - Wine si2/si3: 0 problems.
- re18 (build 34, SLOT_GUARD=2, B):
  - Confirm at t=129.5, crash at about t=162, exit at 231 s.
  - The FIRST problem (op 33447, t~154-161) was not a bad list. It was "decref of a slot past the count": slot 0xcf400,
    then 0xcf401.. consecutively, all entry 0. The 32 ops before it were sane D/F of slots 0x409f0-0x474b1 on one thread.
  - Later args were 0x8cc88, 0x1aadb3, 0x14d564 and 0x14d52e. The pool list was rebuilt once (free 90076 -> 90125).
  - The same cascade followed: Scatter unknown page, f2cc4b, dstorage memcpy AVs, reads of -1 at +0x4d9eac7/+0x52df9a4.
- Root path (Very likely):
  - 0x144f658c0(handle) releases a range handle: handle = slot | n<<21, n consecutive decrefs, no bounds check, a no-op
    when handle < 0x200000.
  - It is called by 0x1400e8960 / 0x144f65820, which drain a deferred-release object: state byte +0x78 == 1, a fence-like
    object at +0x30 with vtbl+0x18(0) true, retains at +0x70[+0x6c] via 0x144f65910, and releases from the vector at
    +0x20..+0x28.
  - Garbage handles walk the decref/free past the 3 MiB slot table into table2 and the pool links. That explains the
    re15-re17 list and pool corruption and the "consecutive" nature.
  - Why the handles are garbage is unknown: a stale or reused deferred-release object, or memory overwritten by a
    texture or stream write (Plausible). Next: log the object address, its +0x20/+0x28 vector and the caller when a bad
    handle is seen, and watch who writes it.
- Build 35: the range guard. Handles whose range leaves the table become 0 (no-op) and are logged ("range handle ..
  skipped"). It hooks 0x144f658c0 (2 calls) and 0x144f65910 (3 calls). Wine si5: 7 hooks, no problems.
  LEONIDA_CE_SLOT_WATCH=1 makes the slot table and pool links read-only and logs foreign writers through a VEH. It is not
  run-tested: in Wine si4 no allocator op came within 120 s.
- Black/navy after Confirm (Job 2), Likely:
  - target0 itself is navy and then all zero (re16t f2400). So it is the game's own render, not a present problem.
  - The "ACCOUNT PICKER (Y) / PLAYER (A)" landing screen draws 282 draws to target0 (re17 f1770) but only the navy clear
    shows. Its art comes from streamed textures in the slot pool, and BCPack (comp=14) requests are refused.
  - The near-black and navy phases are the load transition started by the A press. The load dies in the allocator
    corruption above, so no loading screen or menu appears.
- re19 (build 35, 300 s, B):
  - Confirm at t=142.2. Presents stopped at f1870 (t~173); exit at 261 s.
  - The guard held: no list or pool problems, no CopyPageMappings or Scatter failures, no odd dst, no f7c5c/f2cc4b, and
    none of the read-of--1 AVs.
  - 5 range-handle skips at op 33580 (t=169-173). The handles were 0xaaaaaaac, 0xf5d5d454, 0xaaad3500, 0x9f5aaaaa,
    0xf59f59f5: fill or data patterns. So the deferred-release object's retain/release arrays (+0x70, +0x20) were already
    overwritten or freed when it was drained. This is the real bug upstream of re15-re18 (Likely).
  - New crash: AV reading 0x88 at game+0x2593576 <- +0x2592d69 <- worker +0x2215eca. The same worker (+0x2592d0f) was in
    the earlier f7c5c and f2cc4b stacks.
  - Screens unchanged: navy+footer, near-black, navy; at f1860 presented/target1/2 are zero while target0 stays navy.
- NEXT (agA10):
  1. Decompile the drain functions' owner (0x144f657c5 queues 0x144f65820 as a callback; 0x1400e88f0 maps via
     0x140f2c460(obj+0x38) and then retains) and the worker frames +0x2592d0f/+0x2592d69/+0x2593576. Find who allocates and
     frees these objects.
  2. Pattern-0xaa memory suggests a freed or debug-filled block: check whether the object is freed while queued
     (a race between the worker and the main thread, or a fence completing early: the vtbl+0x18 at obj+0x30 may be OUR
     fence/sync shim reporting complete too soon, or never resetting).
  3. LEONIDA_CE_SLOT_WATCH=1 is implemented but untested on Windows.

## agA10 (sender gen 10, 2026-11-25)
- The "deferred-release objects" (Definite, disassembly + runtime log): the title's DirectStorage streaming device
  (vtable 0x1462fc550; 0x1478f5e78 = our DStorage factory probe) makes 17 batch contexts (0x80 bytes, tag "sdsm") at
  boot via vtbl+0x210 = 0x1428c9460(dev, capacity 4096, want_vector, kind 2, prio). 16 come from game+0x28cb836 with a
  release vector; the global one [0x1479f6d70] (lock 0x1479f6d80, from 0x145cd3f50) has none.
  Layout: +0x10 dev, +0x18 OUR IDStorageQueue probe (factory slot 10), +0x20/+0x28 release vector (cap*4, push = *(+0x28)++),
  +0x30 OUR IDStorageStatusArray probe (factory slot 5, capacity 1; vtbl+0x18 = IsComplete(0)), +0x38/+0x40/+0x48 page-
  mapping list (flushed by 0x140f2c460), +0x60 capacity, +0x64 requests, +0x68 retain cap, +0x6c retain count,
  +0x70 retain array, +0x78 state.
  Methods: +0x218 destroy (0x1428c9610), +0x220/+0x228/+0x230 enqueue (0x1428c9680 / 0x1428c9730 / 0x1428c97c0; the
  +0x70 push is 0x144ffd520, the +0x28 push is inline; NO capacity checks), +0x240 = 0x1400e88f0 (flush page mappings,
  retain +0x70 handles, queue->EnqueueStatus(array,0) + Submit, state 1), +0x248 = 0x1400e8960 (sleep-wait IsComplete,
  unretain, release vector, state 0), +0x260 = 0x144f65820 (try-drain). 0x144f657a0 = flush of the global ctx.
- So the "+0x30 sync object" is our dstorage status array, not the D3D12 fence shim. Reading dstorage_x.cpp: an entry
  only turns complete after every earlier request on its queue finished (flush_locked) - looks correct (Likely).
- re17's int3 at f7c5c had rbx = 0xd81b1ef38 = ctx 0xD81B1EF00 + 0x38 (that ctx's page-mapping list).
- Build 36 (dsctx_guard.h, vtable wrapper): broken (my g_orig table too small -> execute AV 0x44f65801 at t~10, re20/re21-b36).
- Build 37 = fix. re20b (B, re19 env): Confirm t=151.3. 48 garbage retains at t~183 (0x55555556 x14, 0xaaaaaaac,
  0x6496496c, 0x6d96d96d, 0xa8a8a8a8, ...), then a bad pool free list (cut/rebuild), then a host heap AV in ntdll at
  t=325.5 (gta6rt+0xebefe), presents stop, exit 394 s. The +0x2593576 crash did not recur (nondeterministic victim).
  NO context ever went over capacity (max retains/vector never >= 256 except the global ctx's requests 3677/4096 at
  boot). => the capacity theory is wrong (Definite); the arrays are overwritten from OUTSIDE by pattern/texture-like
  data (Very likely), the same writer that hits the pool links and other heap blocks.
- Build 38: LEONIDA_CE_DSCTX_ISOLATE=1 moves each ctx's retain array / release vector to private VirtualAlloc pages at
  creation (old title blocks stay allocated with a canary); =2 also makes the old blocks' interior pages read-only and
  logs FOREIGN WRITEs (rip, regs, stack guesses). Diag always scans arrays for bad handles before map/drain.
  Wine sj3: 17 ctx isolated, no writes, state 1.
- re21 (build 37, DSCTX_CAP=0x4000, B, stopped early): still a bad handle 0x29280000 + pool list cut, AVs at
  game+0x122310 (read -1) and +0x2591ee9 (worker family again). Capacity is irrelevant.
- re22 (build 38, ISOLATE=2, B): ran 420 s with NO crash (presents to f4920+, ~13 fps), no bad handles in the (moved)
  live arrays. 40 FOREIGN WRITEs at t~188.8 (~22 s after Confirm at 167.1): rip = VCRUNTIME140 memcpy store loop
  (+0x123e3..0x1242e) on 4 threads = OUR dstorage workers; rcx (cursor) 0xd84adffc0, rax (memcpy's original dst) =
  0xd849804a0/6a0/7a0/8a0 = entries of TABLE2, r8 = 0xa07e0..0x2a08e0 bytes left. All 31 abandoned-block canaries were
  overwritten with exactly the garbage-handle patterns of re19/re20b/re21 (0x29280000, aaaaaaac, 55555556, ...).
  Pool links still got corrupted (same writer: links 0xd84a79ab0.. lie between table2 and the ctx blocks).
- ROOT CAUSE (Very likely): DStorage request option bit 21 (0x200000). 0x144ffd520 sets it when 0x144ffd8c0(dest)
  finds the destination in the GPU texture pool window (and table1[slot] has bit 63): then +8 = &table2[slot]
  (table2 = [0x1471bfb10]) and +0x10 = low 16 bits of the destination, instead of a memory pointer. (agA9 already saw
  comp=14 requests 0x0020000e with "dst" in table2.) Our shim memcpy'd every such request's payload to +8 -> table2,
  pool links and the heap behind them. This is the upstream cause of the re13-re22 slot/pool corruption, the garbage range
  handles, the f7c5c/f2cc4b asserts and the varying worker crashes (+0x2593576, +0x2591ee9, ...).
  NOT our fence/status shim (that part is correct).
- Build 39 (THE FIX, runtime/shims/dstorage_x.cpp + xmem.cpp + page_alias.h, LEONIDA_CE_DS_DESC default 1): bit-21
  requests write to gpu base [0x149e1dad0] + slot*64K + low16 via the physical pages mapped there (new
  rt::xmem::write_phys_hook, writes through the section). Unmapped page -> nothing written, E_FAIL. Logs
  "[dstorage] slot-descriptor request ... -> va ..: written|NOT WRITTEN". BCPack (comp=14) still E_NOTIMPL.
  Wine sj4: OK (no such requests before state 1).
- re23 (build 39, SLOT_GUARD=2 + ISOLATE=2 tripwires, B): Confirm t=122.9. CORRUPTION GONE (Very likely fixed): 0 foreign
  writes, 0 canaries, 0 bad handles, 0 range skips, 0 pool-list cuts/rebuilds (SLOT_GUARD no longer has to skip anything).
  32 slot-descriptor requests (opts 0x00200001, comp 1): 16 written (va 0xbdbc10000.. via the GPU-view pages), 16 NOT
  WRITTEN (va 0xc31ae0000, 0xc31ce0000, 0xc320e0000..: no physical page in xmem's table) -> E_FAIL. comp=14 requests (opts
  0x0020000e) still refused. New fatal at t~234-241: int3 +0x15e69a8 / +0x233804 (stack +0x22ca78, +0x50ed0bc ..
  +0x221c29c), then the Quitf path int3 +0x2474606, then AVs (memcpy from game+0x52676e8 reading 0xd00000886,
  exec 0, +0x25782ed). Presents stop at f2305, exit 334 s. Screens: navy + hint, then flat navy. Nothing new.
  The fatal is probably the title reacting to the failed (E_FAIL / E_NOTIMPL) stream requests (Plausible).
- Build 40: slot-descriptor writes fall back to the CPU pool view pages, then to committed writable memory at either VA;
  log says which path; failures print the host state. re24 handed to B.
- re24 (build 40, B): Confirm t=145.0. Still no corruption. Slot-descriptor requests: 16 "written via gpu pages", 16
  NOT WRITTEN; the fallbacks (CPU view pages, committed memory) never helped: host state MEM_RESERVE / MEM_PRIVATE at
  both VAs (e.g. va 0xdc8810000, 0xe38c40000, 0xe38c4d000 - note non-zero low16 = 4K sub-slot destinations, and several
  requests share one descriptor). In this run the title heap/table2 sat at 0x784xxxxxx (descs 0x7849xxxxx), so the
  layout moves between runs. Fatal at t~223: int3 +0x2474606 (Quitf), exec AV 0x10c94673a43, AVs +0x41ebcc7,
  +0x5205230, +0x61af; presents stop f2399; exit 295 s. Screens unchanged (navy).
- NEXT (agA11):
  1. The unwritten slot-descriptor requests: find why their slot has no page at write time. Candidates: sub-slot (4K,
     0xc0000-marker) destinations whose 64K page is mapped some other way; or the mapping (CopyPageMappings kind 1 via
     ctx+0x38 flush in 0x140f2c460) only happens after our worker ran (check order: our queue auto-submits at
     capacity/2 = 0x81f; a request could run before the 0x240 flush). Cheap test: when the page is not mapped, park the
     request and retry at the next CopyPageMappingsBatchX / before completing its status; or log the slot's table1/table2
     entries and whether the slot is in the ctx+0x38 list.
  2. Find what fails at +0x2474606 (Quitf caller chain +0x4fb0c69, +0x4fb0ac4, +0x979620 ...): likely the failed reads
     (E_FAIL) and the refused BCPack (comp=14, opts 0x0020000e, also slot-descriptor destinations) requests.
  3. BCPack decode (comp=14) is still missing; the account-picker art and later textures depend on it.
- Recommended env: re19.env (+ DS_DESC default on). SLOT_GUARD=2 and DSCTX_ISOLATE=2 are only tripwires now.

## agA11 (sender gen 11, 2026-11-25)
- table2 decoded (Definite, disassembly 0x1455aa6a0(slot, page)): table1[slot] bits 20..40 = pool page index;
  table2[slot] = ((packed[0x1471bfcc0][page/3] >> 21*(page%3)) & 0x1fffff) << 4 = the slot's PHYSICAL page number, i.e.
  the XMemAllocatePhysicalPages page number (4K frame number of a 64K page; xmem.cpp g_pages key). So an option-bit-21
  DStorage request is a DMA to physical page table2[slot] at offset low16, independent of any VA mapping. Build 39/40 went
  via the GPU VA, which is only MEM_RESERVE for slots whose CopyPageMappings has not happened yet -> the 16 NOT WRITTEN.
- Build 41: rt::xmem::write_page_hook (page_alias.h / xmem.cpp xmem_write_page) + dstorage_x.cpp write_out "phys page"
  path (first), continuing with table2[slot+1].. when a request crosses 64K; the build-40 VA paths are the fallback
  (LEONIDA_CE_DS_DESC=2 = VA paths only). comp=14 (BCPack) now zero-filled through the same path with S_OK by default
  (LEONIDA_CE_DS_ZEROFILL=0 = old E_NOTIMPL). md5 4e297d29.
- The Quitf at +0x2474606 (Definite, disassembly + re24 regs): 0x142474500 is the title's GPU-hang/device-removed handler;
  0x144fb0ad0 is a fence wait (monitorx/mwaitx on the fence's completed-value address from vtbl+0x60 = our fence mirror,
  rdi = 0x1319d690bb8 next to the mirrors logged at 0x1319d690c58) and calls it when the value is UINT64_MAX. Our
  GetCompletedValue returns UINT64_MAX on device loss (d3d12_com.cpp:1456) - but re26 proved there was NO device loss
  (see below), so the semaphore itself reached ~0 (a Signal of ~0) or something wrote the mirror. Preceded at t~203 by "GPU address arena full (raise
  LEONIDA_ARENA_MB)" (275 refused buffers; arena already at the 4094 MiB cap) and one "ExecuteCommandLists failed: invalid
  subresource". Likely chain (Plausible): arena full -> buffers refused (E_OUTOFMEMORY, the console never fails) -> title
  uses null/garbage VAs -> GPU fault -> device lost -> Quitf.
- Build 42 = 41 + "[hack] arena full #k" lines (live MiB by use, free blocks, largest block; 1st and 2^k-th failure) +
  LEONIDA_CE_ARENA_SPILL=1 (a refused default-heap buffer gets its own VkDeviceMemory, VA = device address - arena base,
  like upload buffers). md5 3a129e21.
- BCPack: not ported (tools/bcpack.py is ~1500 lines of rANS + six block rebuilders + tables; a multi-hour port). The
  comp=14 requests are opts 0x0020000e with no codec bits (0x20/0x40/0x60/0x80 from 0x144ffd520's switch).
- re25 (build 41, B): Confirm t=116.9. All 15 slot-descriptor requests "written via phys page", 0 NOT WRITTEN, 0 page-path
  misses; 4 comp=14 zero-filled, 0 request FAILED; still clean (0 foreign/canary/bad handle). => jobs 1 and 3 (safe answer)
  done (Very likely). BUT the int3 +0x2474606 still fires, earlier (t=173.4), same signature; then exec AVs (0x10e48fd8130,
  0) and AVs +0x516b8cb, +0x25782ed, +0x5205230. No arena full this time; just before: "bindless image array is full;
  dimension 3" (= 1D, cap 256; dim 1 = 2D-array, cap 1024, full at boot) and one "ExecuteCommandLists failed: unsupported".
  Last present f1744, exit 261 s. Screens: navy + ACCOUNT PICKER hint, then flat navy (/root/rb/re25d, re25v).
- Build 42 (md5 8fc8b46b): LEONIDA_CE_DEVICE_FAULT=1 -> VK_EXT_device_fault on (verified "on" with the NVIDIA RTX 6000 Ada
  under Wine sk3); leonida::device_lost_hook()/report_device_lost() in src/vulkan_backend.h fire once at the 3 places
  device_lost_ is set; the hook prints vkGetDeviceFaultInfoEXT description/addresses/vendor codes. Plus arena-full stats
  and ARENA_SPILL. Handed as re26 (re26.env).
- re26 (build 42, B): "device fault reporting: on", but NO DEVICE LOST line -> no Vulkan device loss (Very likely; the
  hook is called at all 3 device_lost_ stores). int3 +0x2474606 at t=256.3, rdi 0x1710cc6dfc8 = the FIRST logged fence
  mirror (Definite) -> that fence's completed value was UINT64_MAX without a loss. Arena: full at t=197.3 with live
  1677.6 MiB, 2416 MiB free in 44238 blocks, largest 64 KiB = FRAGMENTATION from 64 KiB alignment of tiny buffers
  (44974 allocations). 354 refused buffers. 1 slot-descriptor request NOT WRITTEN because table2[slot] = 0 at write time
  (desc 0xD84789EC0, low 0x5000/0x8000; slot unmapped/released by then) -> request FAILED comp=1 terrain. Confirm t=126.5.
  Screens unchanged (re26d/re26v).
- Build 43 (md5 1a49ff43, NOT RUN on the VM yet): LEONIDA_CE_ARENA_SMALL_ALIGN=256 (buffers < 64 KiB at 256-byte
  alignment) + tracing of who makes a fence ~0 ("[hack] fence mirror .. -> UINT64_MAX", "[hack] fence/queue Signal(..,
  >=2^48) from <ret>", "[hack] deferred list write of UINT64_MAX"). Wine sk4: state 1 / f600 fine. Handoff: build-43.txt,
  re27.env.
- NEXT (agA12): run re27. If a Signal(~0) shows up, decompile its return address (the title may Signal ~0 on purpose
  while tearing down / on a timeout path; then find why). If the arena still fills, measure live buffers (leak?).
  table2[slot]==0 requests: park and retry, or answer S_OK and drop. BCPack port still open (tools/bcpack.py).
- Recommended env: /root/handoff/re27.env with build 43.

## agA12 (sender gen 12, 2026-11-25)
- re27 (build 43, re27.env, B gen 8): Confirm (state 13->0) t=124.9. NO "arena full" at all (SMALL_ALIGN=256 fixed the
  fragmentation; Very likely). 4 "invalid subresource" list failures (t~144-181, after "SRV dimension not supported for
  buffer: 4/9") + 4 boot "invalid copy or resolve". Presents stop t~244.5; then all 5 fence mirrors flip to UINT64_MAX in
  ONE poll (incl. fences still at 0); int3 +0x2474606 at t=250.35 (rdi = the 0x1286 mirror, rsi 0x1288); exec AV 0;
  AV +0x61af; exit 327 s. ZERO "fence Signal"/"queue Signal"/"deferred list write" lines.
- => it IS a Vulkan DEVICE LOSS (Very likely): the mirror line logs GetCompletedValue(), which is UINT64_MAX for every
  fence at once only via context.is_device_lost() (vulkan_backend.h Fence::completed / d3d12_com.cpp:1456). agA11's
  "no device loss" was wrong: the device-fault hook printed nothing (hook silent/hung or vkGetDeviceFaultInfoEXT
  unavailable). Not a Signal(~0), not the arena.
- Title side (disassembly 0x144fb0ad0): fence values carry a tag in the top byte; "not reached" = bit 55 of
  (completed - target); ~0 -> 0x142474870 -> 0x142474500 (Quitf). Loop body 0x144fb0e90 is only a yield.
- Build 44 (md5 7d7922cf): report_device_lost() always prints "[hack] DEVICE LOST seen"; LEONIDA_CE_CHECKPOINTS=1 =
  VK_NV_device_diagnostic_checkpoints (src/ce_checkpoints.h; checkpoint before every draw/dispatch, marker = VkPipeline,
  names registered at pipeline build: "cs <name>" or "vs + gs + ps"), printed per queue on loss before the fault query;
  small arena buffers packed top-down; "[hack] arena: N allocations, M releases, live" every 8192 allocs; fence mirror
  FOREIGN-write log + AddRef; LEONIDA_CE_FENCE_NO_MAX=1 hides ~0 (useless once the device is really lost);
  DStorage table2[slot]==0 parts dropped with S_OK (LEONIDA_CE_DS_DROP0=0 = old). Wine sl3: checkpoints on, boots.
  Handed as re28 (re28.env = re27.env + CHECKPOINTS=1, rtdump48).
- Build 45 (md5 0c9bde3e) = 44 + BCPack integration: LEONIDA_CE_BCPACK (default on; =0 = zero fill only) decodes comp=14 with shims/bcpack_decode
  (the other agent's port, strict mode, kAuto), refused chunks fall back to zero fill; logs "comp=14 BCPack decoded/
  refused". bcpack_decode.cpp added to runtime/CMakeLists.txt. Wine sl4 boots (no comp=14 before state 1 there).
- re28 (build 44, B): Confirm t=126.1; mirrors -> UINT64_MAX again (t~245), int3 +0x2474606 t=247.9, but NO "DEVICE LOST
  seen" => vkGetSemaphoreCounterValue itself returned UINT64_MAX with VK_SUCCESS. Windows System log: nvlddmkm Event 153
  (RC error / TDR recovery, "Error occurred on GPUID: 300") at exactly the loss moment in re27 AND re28 (and earlier runs)
  => GPU RC/TDR (Definite that the driver hit an error then; Very likely the cause). Loss ~122-125 s after Confirm both runs.
  ARENA LEAK (Definite): "[hack] arena: 57344 allocations, 0 releases, live 1919 MiB" - no arena buffer is EVER released
  (+~140 MiB per 8192); fine for ~5 min with SMALL_ALIGN, will fill eventually. Who keeps them (title never Releases, or a
  proxy/ref leak)? Open. 16 DROP0 lines (t~190-200). 5 "invalid subresource" list failures (t~145-175).
- Build 46 (md5 6f62c16a) = 45 + Fence::completed(): a timeline reading UINT64_MAX calls mark_device_lost() -> "DEVICE LOST
  seen" + NV checkpoints + fault info. Handed as re29 (re29.env = re28.env, rtdump49). BCPack default ON.
- NEXT (agA13): read re29's CHECKPOINTS (the pipeline each queue was in at the TDR) and kill/skip that shader (hang =
  infinite/long loop in a translated shader is the leading theory; zero-filled/undecoded inputs or wave64-on-wave32 may
  feed it). Find the arena leak (count Resource releases vs creates per kind). "invalid subresource": skip the bad command
  instead of failing the whole list.
- (superseded below)
- re29 (build 46, read by me from /root/rb/re29.err.log): "DEVICE LOST seen" fired at the RC error. CHECKPOINTS: only one
  direct queue had work: top+bottom marker = "cs fspx_game/cs_bedf02010668b70ebdd31e4f4a0d010a07b74d275991a560c0af9e59faf5d857".
  Device fault: type 6 (IP) 0x21df5a2c0, type 1 READ_INVALID 0x3f5ff070000 (prec 0x1000) => THE CULPRIT (Very likely): that
  compute shader dereferences a garbage GPU VA. BCPack (default on): EVERY comp=14 chunk refused (129+; "mode 1: lane N
  ... fragment did not end at its home state; mode 7: BC7 ... unknown built-in endpoint history / inferred-only rank") ->
  zero fill as before. Either the request source needs an offset/header strip, or these are BC7/BC6H (unsupported) -
  tell the BCPack agent.
- Build 47 (md5 adae1192): LEONIDA_CE_SKIP_CS=<substrings> -> empty placeholder compute program. re30.env =
  re29.env + LEONIDA_CE_SKIP_CS=cs_bedf02010668b70e (rtdump50), 420 s. Handed to B. Real fix: find which VA the shader
  reads (its disasm in /root/leonida-work/shaders/fspx_game/disasm/cs_bedf0201...s; read-only) and why it is garbage
  (likely from a buffer the title fills via an unsupported path / zero-filled BCPack / a never-released arena buffer).
- Recommended env: /root/handoff/re30.env with build 47.
- re29 extra (B): 900 C++ exceptions (0xe06d7363) from t=98 on, all from host gta6rt.exe+0x14f169 <- +0x1554b0 <- +0x14f02d
  ... = most likely the BCPack decoder throwing internally on every refused chunk (caught, but slow). If BCPack stays
  0-decoded, run with LEONIDA_CE_BCPACK=0. A new late "invalid subresource" (line 15279) came just before the loss, and
  "view starts past the texture's mips or layers; first mip 5" was the last line before DEVICE LOST. The faulting read
  0x3f5ff070000 is far outside the 4 GiB arena.

## agA13 (sender gen 13, 2026-11-25)
- re30 (build 47, SKIP_CS=cs_bedf0201, B): skip worked, but a DIFFERENT fspx compute faulted ~20 s earlier (loss t~233,
  Confirm t=124.5): checkpoints = cs fspx_game/cs_25d19264fce42489..., fault READ_INVALID 0x1e1f7f013000 (looks like a
  host pointer). Same RC/TDR -> fences ~0 -> int3 +0x2474606 chain. Arena still 0 releases. 18 "invalid subresource".
- Disassembly (read only): cs_bedf0201 = mip copy: T# src (table+0x00) image_load_mip at lod cb.w, image_store to T# dst
  (table+0x20) at cb offset; bounds only vs cb extents -> an out-of-range lod/coord is UB; no robustness feature was ever
  enabled in the runtime (grep "robust" = nothing). cs_25d19264 = reads two V#s via s_buffer_load (table+0x00, +0x40 =
  CBVs) and stores a 1D image: its READ fault must be a CBV base that is no buffer (Likely). CreateConstantBufferView
  wrote desc->BufferLocation verbatim (a title CPU pointer stays a CPU pointer; shader adds heap_base).
- Arena leak: d3d12_xs_device.cpp xs_record_placed AddRefs every slot-133 / slot-70/71 stand-in forever (agA5,
  LEONIDA_CE_PLACED_REF), and the slot-70/71 stand-ins also keep their creation ref. Wine sm2: of ~6k records only ~400
  overlap an older one, and only 2 of the title-held ones were ever released by the title -> eviction alone will not
  stop the growth; the 57k arena allocations are mostly NOT g_placed records (re29: 6824 slot-133 calls). Build 48
  prints an "arena census" (DEFAULT buffer creators by caller RVA + resources made/freed) to find them.
- Build 48 (md5 9764ec10): LEONIDA_CE_ROBUST_IMAGE=1 (1.3 robustImageAccess via Vulkan13Features::want_robust_image_access),
  LEONIDA_CE_CBV_FIX=1 (unknown CBV location -> rt::d3d12_xs::upload_snapshot, else null view; VulkanContext::va_known),
  LEONIDA_CE_SKIP_BAD=1 (vulkan_replay.h prepare(): CE_BAD_SUBRESOURCE() -> p.skip/p.ce_bad + goto ce_next_command;
  emit() skips ce_bad; draws via prepared_draw.skip), LEONIDA_CE_PLACED_EVICT=1|2 (live interval map g_placed_live;
  overlap eviction releases our refs; =2 also sweeps title-released slot-133 records), arena census (always).
  Wine sm1-sm3: boots to state 3, robust on (supported 1), no new errors. sm1 died at t~180 in ntdll RtlVirtualUnwind
  from gta6rt+0x1a3f1 (LEONIDA_CE_STACKS stack walk under Wine; Plausible unrelated).
- Handed: re31.env (build 48 fixes, no SKIP_CS), re32.env (+ SKIP_CS both shaders) if re31 still faults.

## agA13 (sender gen 13, 2026-11-25)
- re30 (build 47, SKIP_CS=cs_bedf0201, B): skip worked, but a DIFFERENT fspx compute faulted ~20 s earlier (loss t~233,
  Confirm t=124.5): checkpoints = cs fspx_game/cs_25d19264fce42489..., fault READ_INVALID 0x1e1f7f013000 (looks like a
  host pointer). Same RC/TDR -> fences ~0 -> int3 +0x2474606 chain. Arena still 0 releases. 18 "invalid subresource".
- Disassembly (read only): cs_bedf0201 = mip copy: T# src (table+0x00) image_load_mip at lod cb.w, image_store to T# dst
  (table+0x20) at cb offset; bounds only vs cb extents -> an out-of-range lod/coord is UB; no robustness feature was ever
  enabled in the runtime (grep "robust" = nothing). cs_25d19264 = reads two V#s via s_buffer_load (table+0x00, +0x40 =
  CBVs) and stores a 1D image: its READ fault must be a CBV base that is no buffer (Likely). CreateConstantBufferView
  wrote desc->BufferLocation verbatim (a title CPU pointer stays a CPU pointer; the shader adds heap_base).
- Arena leak: d3d12_xs_device.cpp xs_record_placed AddRefs every slot-133 / slot-70/71 stand-in forever (agA5,
  LEONIDA_CE_PLACED_REF), and the slot-70/71 stand-ins also keep their creation ref. Wine sm2: of ~6k records only ~400
  overlap an older one, and only 2 title-held ones were ever released by the title -> eviction alone will not stop the
  growth; the 57k arena allocations are mostly NOT g_placed records (re29: 6824 slot-133 calls). Build 48 prints an
  "arena census" (DEFAULT buffer creators by caller RVA + resources made/freed) to find them.
- Build 48 (md5 9764ec10): LEONIDA_CE_ROBUST_IMAGE=1 (1.3 robustImageAccess via Vulkan13Features::want_robust_image_access),
  LEONIDA_CE_CBV_FIX=1 (unknown CBV location -> rt::d3d12_xs::upload_snapshot, else null view; VulkanContext::va_known),
  LEONIDA_CE_SKIP_BAD=1 (vulkan_replay.h prepare(): CE_BAD_SUBRESOURCE() -> p.skip/p.ce_bad + goto ce_next_command;
  emit() skips ce_bad; draws via prepared_draw.skip), LEONIDA_CE_PLACED_EVICT=1|2 (live interval map g_placed_live;
  overlap eviction releases our refs; =2 also sweeps title-released slot-133 records), arena census (always on).
  Wine sm1-sm3: boots to state 3, "robust image access: on (supported 1)", no new errors. sm1 died at t~180 in ntdll
  RtlVirtualUnwind called from gta6rt+0x1a3f1 (LEONIDA_CE_STACKS stack walk under Wine; Plausible unrelated).
- Handed: re31.env (build 48 fixes, no SKIP_CS), re32.env (+ SKIP_CS of both shaders) if re31 still faults.
- re31 (build 48, no SKIP_CS, B): robust on; loss EARLIER (t~146, Confirm t=113.4) in a THIRD fspx compute
  cs_87dac0f88356d831 (T# load + 2 CBVs at +0x40/+0x60 read up to +0x9c0 + 3D image store), READ_INVALID 0x2f514510000.
  CBV_FIX 0 lines (all CBV locations are known buffers). SKIP_BAD: 10x "vulkan_replay.h:3456 command 0 of 1 (kind 9)"
  and NO invalid-subresource list failures any more. Arena census: DEFAULT buffers by caller +0x9d042 = proxy_slot<133>
  3413 / 1032 MiB (flat, title-held) and +0x83b94 = proxy_slot<70> 1972 -> 10153 / 118 MiB (the growth: placed-SRV
  stand-ins at fresh VAs). resources made 19802 freed 4302, arena buffers freed 255 (first releases ever).
- Build 49 (md5 75b4c240): CBV_FIX also in the xs proxy: slot-17 CBV / root CBV,SRV,UAV (PC 37-42) at a title pointer
  past the arena -> covering placed buffer VA; the vulkan-side fix only acts past the arena (low addresses collide with
  arena VAs; sm4 showed 0xb4ff20-type CBVs that are arena VAs).
- Build 50 (md5 61fe3807): LEONIDA_CE_PLACED_CAP=N: at most N runtime-owned slot-70/71 stand-ins live (oldest released).
- Build 51 (md5 5f867552): LEONIDA_CE_BUFFER_PAD=N: own-memory buffers (upload/readback/spill) get N bytes of slack and
  the arena top keeps N (64K-rounded) unallocated. THEORY (Plausible): all 3 faults are page-aligned READ_INVALIDs by
  s_buffer_load past the end of a CB in a separately allocated buffer / at the arena top (translated scalar loads have
  no num_records bound; AMD returns 0). Handed as re34 (re34.env: no SKIP_CS, CAP=4096, PAD=65536).
- B ran re32 = re31 + SKIP_CS of all three shaders (result below if it came in).
- re32 (build 48 + SKIP_CS of 3 shaders, B): Confirm t=133.7; a 4th fspx compute faulted at t~270 (~136 s after Confirm):
  cs_1bc231e4b1579577 (twin of cs_25d19264: CBs at table+0x00/+0x40 read up to +0x60, 1D image store), READ_INVALID
  0x4b8392935000 (page aligned again). Arena (no CAP): +0x83b94 proxy_slot<70> 51096 buffers / 694 MiB and growing; 9009
  arena releases (PLACED_EVICT works but does not keep up). Screens: navy+footer, #111111, then flat navy #20202C to the end.
- re34 (build 51, PAD=65536, CAP=4096, NO SKIP_CS, B): Confirm t=140.2. None of the 4 page-fault shaders faulted, and there is NO
  READ_INVALID any more (Likely: PAD fixed the s_buffer_load overreads). Loss at t~206 in a 5th fspx compute,
  cs_f33ac9e947af1490, with 22 fault addresses, all type 4 (IP unknown) = a HANG/TDR, not a page fault. That shader loops on
  buffer_load glc dlc (a spin-wait on another workgroup / a garbage loop bound). CAP works: placed live ~10k, arena
  65536 allocs / 55966 releases, live 1231 MiB (the leak is stopped). ECL failures 0, SKIP_BAD 24.
- NEXT: re35.env = re34 + SKIP_CS=cs_f33ac9e947af1490 (rtdump55); then the cs_f33ac9e9 spin (inter-workgroup sync
  on NVIDIA without forward progress, or zeroed inputs). Recommended env: re34.env (+ the re35 skip) with build 51.

## agA14 (sender gen 14, 2026-11-25)
- re34 AV gta6rt+0xb8fe5 (t=203.7, ~2.5 s before the TDR) = xs_safe_get_desc (inside __try, caught) <- xs_upload_placed_texture
  <- xs_refresh_placed_textures (+0xece76) <- child_slot<0,32>: a stand-in released by PLACED_EVICT/CAP during the refresh; the
  execute-AV at 0x0 right after it is the same freed vtable. agA13 had already fixed it in the tree (XsHold AddRef); build 52 is
  the first build to ship the fix. Plausibly unrelated to the TDR.
- cs_f33ac9e9 (re34 TDR) disasm: ONE 1024-thread workgroup, LDS 3x1024 dwords, coalescing a free-block heap (buffer table+0x00,
  block list table+0x20, CB table+0x40: 0x0c shift, 0x10 ?, 0x14 header size, 0x18 base, 0x24 count, 0x28 bucket size).
  No cross-workgroup sync (glc/dlc loads read stores from the same workgroup) => not a forward-progress issue (Likely). Hang
  mechanisms: the barrier-pass loop runs s_ff1(count) times, and count 0 -> -1 -> 2^32-1 barrier rounds; the chain walk does not
  advance when the header size and a header both read 0. A console would hang the same way => our inputs are zero/stale (Plausible).
- re35 (build 51, SKIP_CS f33ac9e9, B): Confirm t=110.8, loss t~175 in fspx cs_e8c1251e (bindless: loads V#s from buffers at
  dynamic offsets), READ_INVALID 0xd0c680f000. The line right before it: the FIRST EVER "unimplemented SetComputeRootSignature(signature
  that does not fit the compute layout)" + "root arguments beyond 32 user-data dwords (spill not modelled)" (d3d12_com.cpp:
  bind_root/write). The dispatch is still recorded with zero/truncated root data => garbage descriptor tables (Likely).
  Note: fault precision is 0x1000, so "page aligned" fault addresses are just page-granular reports, NOT proof of an
  overread at a buffer end (weakens the BUFFER_PAD theory; re34 had no READ_INVALID though).
- The translator bounds s_buffer_load by num_records (spvasm of f33ac9e9: UGreaterThanEqual size checks), so the
  "unbounded scalar loads" part of agA13's PAD theory is wrong for that shader at least. Device address = heap_base (push
  constant dwords 32-33 = arena device address) + 48-bit V# base. Own-memory buffers have VA = devaddr - arena address (anything).
- Build 52 (md5 c9d72a4d): LEONIDA_CE_LOOP_CAP=subs|1 (src/spirv_loop_cap.h: per-loop counter on structured loops whose continue
  block ends in OpBranchConditional header/merge; dispatcher loop cap _INNER default 4M blocks, barrier-round loop cap _OUTER default
  256K rounds; all 1536 fspx modules spirv-val clean; 55 have loops), LEONIDA_CE_SKIP_UNFIT=1 (d3d12_com.cpp: RootState.overflow;
  dispatch/indirect dispatch/draw skipped when the layout is not ok for the bind point or args overflowed), always-on log lines
  "[hack] BUFFER_PAD: ..." and "[hack] arena: device address 0x.. size 0x..". Wine sn1: state 3, 55 LOOP_CAP lines, ok.
- Navy screen (re35 rtdiag frame 2280, state 0 task 5 inner 5): 210 draws/frame, 189 into a 3840x2160 RT (flags 1) and 21 into the
  two flags-5 targets, one presented; all three probe as solid ff2c2020 (navy) => the draws land but write nothing visible, or
  only the navy fill (Scaleform cxform/black-fade issue from agA2 is the prime suspect). Post-Confirm the title creates many 2D
  textures (1024-2560 wide) and big buffers = world/asset streaming; the screen is most likely the loading screen with its UI
  layer not visible (Plausible). ~13 fps (4656 ms / 60 frames).
- Handed: re36.env (= re35 without SKIP_CS + LOOP_CAP=fspx_game + SKIP_UNFIT=1, rtdump56); re37.env (+ SKIP_CS=cs_e8c1251e80f7e330).
- re36 (build 52, LOOP_CAP + SKIP_UNFIT, B): arena device address 0x300010000 size 0xffe00000; 55 LOOP_CAP; Confirm t=124.0;
  first unfit compute root signature + "beyond 32 user-data" at t~185-190 -> 24+ SKIP_UNFIT dispatches -> DEVICE LOST t~190 in
  ANOTHER fspx compute cs_c5b1005e, READ_INVALID 0x50700010000 (VA ~5 TB). No hang (LOOP_CAP did its job or the hang input never
  came). The losses in re35/re36 come ~65 s after Confirm, right after the first unfit (spilled) compute root signature:
  the fspx effect that starts then needs root-argument spill (>32 user-data dwords), which is not modelled (d3d12_com.cpp write()).
  REAL FIX (open): model the console's user-data spill (the root arguments past s31 go into a memory table whose pointer
  is in a user SGPR); see rt::rootsig layout / "spill array not modelled".
- Build 53 (md5 9cf4e6d7): LEONIDA_CE_VA_GUARD=subs|1 (src/spirv_va_guard.h): every OpConvertUToPtr(OpIAdd(heap_base, va)) gets
  va' = va within +-LEONIDA_CE_VA_GUARD_WINDOW (default 2^38) of the arena ? heap_base+va : heap_base. 1528/1536 fspx modules
  guarded, all spirv-val clean. Wine sn2 ok (state 3, 16 VA_GUARD lines). Handed as re39 (= re36 + VA_GUARD=fspx_game).
- Recommended env: build 53 + /root/handoff/re39.env.
- re39 (build 53, VA_GUARD=fspx_game): STALLED before boot state 1 (last present 1665, "background compute cs_af640243: 0
  after 103421 ms"), host AV in an ntdll stack walk (gta6rt+0xef35e) at t=120, exit 220 s. Guarding all ~1528 fspx modules
  defeats the pipeline cache and the NVIDIA compiles stall boot (Likely). Use a targeted list only.
- Handed re40 = re36 + VA_GUARD=cs_bedf0201,cs_25d19264,cs_87dac0f8,cs_1bc231e4,cs_e8c1251e,cs_c5b1005e,cs_f33ac9e9 (result pending).
- Recommended env now: build 53 + /root/handoff/re40.env. NEXT: model the root-argument spill (the real cause of the
  post-Confirm fspx faults, Likely); then the one-colour navy screen (try CXFORM_FORCE=1; per-draw dumps of the 189-draw target).

## agA15 (sender gen 15, 2026-11-25)
- re40 (build 53, VA_GUARD on 7 shaders, B): Confirm t=130.5; loss t~196-199 (66 s after Confirm) in NEW fspx cs_86e6e299 on a
  DIRECT queue, READ_INVALID 0x28fea0f000 (arena 0x300010000 + ~152 GiB: inside the 256 GiB VA_GUARD window, so a guard would
  not catch it). Again right after the first "SetComputeRootSignature(signature that does not fit the compute layout)". The 24
  "ExecuteCommandLists failed: unsupported" lines come AFTER the loss (re36 as well); "list is invalid; refusing batch" was never
  printed in any run. Screens: picker, flat black, flat navy (re37 the same).
- The unfit signature (Definite, Wine so1 log): 8 root views then one 14-dword constant parameter (8V + C14). Console layout:
  V@s4..s18 fits (28 SGPRs), C14 does not. pack() (from s0) fits it, so it was "graphics only"; bound for compute, every compute
  offset was kNoSgpr -> ALL root data dropped (zero) + list marked invalid.
- Census (index.json, user_sgpr_reads): no game (26,503) or fspx (1,536) compute shader reads s0..s3 as a pointer; max read s13.
  So the spill array is never read by translated game shaders (Very likely); only the inline part matters. (bvh/ = driver BVH
  shaders read s[0:1] as their own table, unrelated.)
- Build 54 (md5 7ec3077f): LEONIDA_CE_ROOT_SPILL (default ON, =0 old refusal). d3d12_vulkan_device.cpp CreateRootSignature:
  when pack_stage(Compute) refuses, lay out the console spill layout (parameters inline from s4 while each fits, the first that
  does not and all after it packed into a spill array: constant 4 B, root view 8 B, table 4 B (table size NOT measured)).
  RootArgumentLayout::Parameter.compute_spill / RootArgumentLayout.compute_spill_dwords (d3d12_com.h). d3d12_com.cpp:
  RootState.spill, put() routes a parameter dword inline or to the spill array, finish_spill() on Dispatch and indirect dispatch
  copies the array into the console upload ring (rt::d3d12_xs::upload_snapshot) and puts its 40-bit VA in s[0:1].
  Logs "[hack] ROOT_SPILL: compute signature spills N dwords behind s[0:1]: <shape>" and "...dispatch #N ...".
  Wine so1: signature accepted ("V@s4 ... V@s18 C14@spill+0"), no "usable for graphics only", no validation errors; Wine does not
  reach Confirm in 150 s.
- Handed re42.env (build 54; = re40 without LOOP_CAP/SKIP_UNFIT/VA_GUARD, + ROOT_SPILL=1, rtdump62), re43.env (+LOOP_CAP).
- re42 (build 54, no LOOP_CAP): spill signature created, 0 "dispatch #N" lines before the loss (cs_f33ac9e9 hang again, type-4
  faults, ~65 s after Confirm). re43 (+LOOP_CAP): spill WORKS (512+ "ROOT_SPILL: dispatch", ring VAs 0xc49800..), no "does not fit",
  but STILL a loss at t~184 (69 s after Confirm): direct queue cs_86e6e299 (top=bottom), compute queue bottom cs_e8c1251e, READ_INVALID
  0xd0d80f000. => the missing spill was NOT the cause of the losses (Very likely); it only coincides with the same phase.
- Trigger (Likely): in every lost run, the loss follows within ~1-8 s the scripted PRESS 12 / PRESS 0 that follows the 8th
  A press after Confirm (re34 205.2, re36 190.2, re40 195.5, re42 193.9, re43 ~181-189). The scripted menu input starts a new phase
  (world load / story start) whose fspx passes fault. re37 (no loss up to t=205) made no press after t=169.
- cs_e8c1251e (bindless) was in flight on the compute queue in re35/re40/re43. It loads 64-bit pointers from buffers (s_load from
  s[42:43]). LEAD (Plausible): the title stores console GPU VAs from its physical pool in GPU-visible data. The log says "pool cpu
  1800e0000 size 600000000 gpu 7800e0000" (re43; in re40 gpu a32000000). re43: fault VA 0xd0d80f000 minus
  heap_base 0x300010000 = 0xa0d7ff000, INSIDE that pool's GPU range: a title GPU-pool pointer the arena does not map
  (re40's 0x25ea9ff000 is not in its range, so this is not the whole story).
  Next: check whether pool-GPU VAs can be mapped (the arena is only 4 GiB, the pool 24 GiB), or translate them in VA_GUARD
  (va in [pool_gpu, pool_gpu+size) -> the matching arena VA, if the pool's CPU side is mirrored there).
- re44.env (build 54) = re43 + SKIP_CS=cs_e8c1251e80f7e330 + CXFORM_FORCE=1 (rtdump64), with B.
- re44 (build 54, re43 + SKIP_CS=cs_e8c1251e + CXFORM_FORCE=1): Confirm t=113.9; loss t~179.5 (66 s after Confirm), right after
  PRESS 12 (#7). Direct queue top cs_00f50ab8 / bottom cs_d9ea3225, compute queue cs_a039b209; READ_INVALID 0x748b80f000.
  The low bits match across runs: re35 0xd0c680f000, re43 0xd0d80f000, re44 0x748b80f000 all end in ...80f000. That points to ONE bad
  pointer source (same offset 0x80f000 into differently based blocks) read by whichever shader runs, not to per-shader bugs
  (Plausible). Skipping shaders one by one will not converge. Next: find what holds base+0x80f000. Log every DEFAULT/placed buffer
  whose title VA or arena VA is near such an address, or dump the 64-bit pointers cs_e8c1251e reads (s[42:43]) via a debug
  printf / readback.
  CXFORM_FORCE: 6 lines, all before Confirm; post-Confirm frames are still flat navy (re44_frames: 8 picker, 16 black, 18 navy).
- Recommended env now: build 54 + /root/handoff/re43.env (spill + LOOP_CAP); add SKIP_CS=cs_e8c1251e80f7e330 if wanted. ROOT_SPILL
  stays default ON (correct per the console layout, verified working on Windows; harmless).

## agA16 (sender gen 16, 2026-11-25)
- ri45 (B; build 54, re45.env = re43 with PRESS trimmed to A@s3+20/15x4,DOWN@s3+27/15x3): Confirm t=112.4, last press t=128.2 (3 presses after
  Confirm), DEVICE LOST t~176 = 64 s after Confirm and 48 s after the last press. => hypothesis (b) "a press triggers it" DROPPED (Likely): the
  loss is tied to Confirm + ~65 s, not to the presses. Checkpoints: cs_86e6e299 (direct + compute), cs_c5b1005e (compute). READ_INVALID
  0x50700010000 = EXACTLY the re36 address. Screens unchanged (navy+hint, gray 17, flat navy 32,32,44 after Confirm).
- Fault addresses form two families: 0x50700010000 (re36, ri45) and ...80f000 (re35 0xd0c680f000, re43 0xd0d80f000, re44 0x748b80f000; re40
  0x28fea0f000). As VA (minus arena 0x300010000): 0x504_0000_0000 exactly, or the last 4K page below a 2 MiB boundary (...7ff000/...9ff000).
  None is an arena CPU pointer (arena cpu 0x25DF5240000 etc. per run), none lies in the title pool windows (re43 pool gpu a32000000).
- cs_86e6e299 (disasm, 42 lines): table at s4 (1 dword); T#s at table+0x00..0x7f (image_store only), CB V# at table+0x80 (slot 4), one
  s_buffer_load_dwordx2 at CB+0x18. Its only memory reads are the table and that CB => the 0x50700010000 READ is the CB V# base
  (d0=0, d1&0xffff=0x504, num_records>0 since translated s_buffer_loads are bounded) (Likely). Either a garbage descriptor in the slot, or a
  V# to an own-memory buffer (VA = devaddr - arena) that was freed (0x507_0001_0000 is a plausible VkBuffer device address) (Plausible).
- Build 55 (md5 2eb29e5b): LEONIDA_CE_PTR_CHECK=1|2 (d3d12_vulkan_device.cpp ce_ptr_check; hook leonida::ce::ptr_check_fn in ce_checkpoints.h,
  called from vulkan_replay.h prepare() for every Dispatch; Dispatch.ce_view_mask/ce_table_mask (command_list.h) filled in d3d12_com.cpp
  record_dispatch from the root layout). Scans root views, the first 32 slots of each table (V# bases; T# at 2) and at 2 the first 1 KiB of
  reachable in-arena buffers for pointer-like qwords; anything >= arena size that va_known() does not know is logged with a class and "dev"
  (= fault-comparable device address). Wine sp1/sp2 fine (Wine scans only a handful of dispatches before Confirm).
- Lead (Plausible): PLACED_EVICT/PLACED_CAP release stand-ins (arena census: ~56k arena buffers freed by the loss) whose descriptors the title
  may still use; their arena ranges get reused -> garbage V#s/pointers. Env-only test re48 = re46 without EVICT/CAP.
- ri46 (build 55, PTR_CHECK=2): DEADLOCK before state 3. ce_ptr_check runs in replay prepare(), which holds resource_mutex_ shared;
  va_known() locked it shared again behind a waiting writer (CreateCommittedResource -> add_resource). ri47 (level 1) hung the same way.
  Build 56 (md5 dbd1235d): VulkanContext::va_known_locked (no lock), used by the check.
  ri46 data before the hang: cs_87dac0f8 s5 tables hold HOST STACK/heap garbage; one slot = [00000014 07060504 00000003 000002aa] =
  V# base 0x504_0000_0014 -> dev 0x50700010014 = EXACTLY the re36/ri45 READ_INVALID (family A explained, Very likely).
- ri49 (build 56, re49 = PTR_CHECK=2, EVICT/CAP on): Confirm t=114.0, loss ~t 137-154, checkpoint (direct) = cs_87dac0f8, READ_INVALID
  0x8aa4020d000. 400 PTR_CHECK lines (cap hit), ALL from cs_87dac0f8 (209), cs_25d19264 (105), cs_1bc231e4 (86) = the re30/re31/re32 faulters.
  The slots cs_87dac0f8 really reads (T# slot 0, CBVs slot 2/3 at +0x40/+0x60) hold FLOATS (3f79f9fe x3, 3f800000), and cs_1bc231e4 slot 2
  holds (1.0,-1.0,0,ffffffff,..). => the s5 descriptor-table VA points at memory that is NOT a descriptor table (constants/stack copies) -
  the ONE bad pointer source is the s5 table pointer of these fspx dispatches (Very likely), not the descriptors themselves.
  Table VAs 0x306af4a0..0x31acd340 step 0x20000-0x40000 (a ring). Candidates: compute tables keep only the LOW DWORD of the VA
  (d3d12_com.cpp address(): one_dword_table) -> a title table above 4 GiB is truncated into an unrelated arena range; or the title's
  transient descriptor ring is not copied into the arena before the dispatch runs (ring/snapshot timing).
- Build 57 (md5 607c9c9c): logs "PTR_CHECK: compute table N VA 0x.. does not fit 32 bits" (first 32). Handed ri50 = build 57 + re50.env.
  ri48 (build 56, EVICT/CAP off) was running at hand-back.
- NEXT: from ri50, if tables >4 GiB: translate them (xs_va_from_cpu on the table handle in the proxy, or keep tables in the arena).
  If not: find who writes the ring at 0x30xxxxxx-0x31xxxxxx (log the slot-14/CopyDescriptors destinations around those VAs and the
  title SetComputeRootDescriptorTable handles for cs_87dac0f8); compare to the title handle before translation.
- Recommended env: build 54/56 + re43.env (no PTR_CHECK; it slows boot). Press theory (b) dropped.

## agA17 (sender gen 17, 2026-11-25)
- ri48 (B; build 56, re48 = PTR_CHECK=2, EVICT/CAP OFF): device still lost, later: ~98 s after Confirm (t~222; Confirm t=123.7), checkpoint
  cs_25d19264 (direct), READ_INVALID 0x60033eaba000. PTR_CHECK: only cs_87dac0f8/cs_25d19264/cs_1bc231e4 again, 13647 bad / 2048 dispatches;
  0x50700010014 garbage V# again (same [00000014 07060504 00000003 ...] slot). Frames: post-Confirm flat navy (32,32,44).
- ROOT CAUSE of the bad s5 tables (Very likely): NOT truncation. The table VAs 0x2da8f..-0x31acd.. lie inside the title placed buffer
  CreatePlacedResourceX(va=2d5b0000, width=182452224 = 174 MiB) - title memory whose arena stand-in has the SAME VA (arena VA == title VA
  for low title memory). Draws copy an 8 KiB window of title memory around each root word into the arena (ring sync, sync_title_range,
  vulkan_replay.h prepare_draw); DISPATCHES never did. Compute tables therefore read what an earlier draw sync left there (floats, CBs,
  stack copies) -> garbage V#s -> faults in whichever fspx shader hits a bad one first (different shader/address each run).
- Console descriptor heap (now logged): type 0 (CBV_SRV_UAV), 1245184 descriptors, address 0x10000, size 0x2600000 (38 MiB). Its
  GPU handle is the console address 0x10000, which as an arena VA is the console upload ring (RING_IN_ARENA ring at VA 0x10000, 64 MiB):
  tables into this heap would read the ring, not the title heap (Plausible issue, unverified whether the title binds tables there).
- Build 58 (md5 210eb601): LEONIDA_CE_DISPATCH_SYNC=1 (vulkan_replay.h, Dispatch prepare): sync each compute table (1 KiB exact +
  8 KiB window) and 1-dword root view, then each buffer V# of the first 32 slots (<= 64 KiB) from title memory into the arena.
  LEONIDA_CE_HEAP_MIRROR=1 (experimental, off): the console heap gets an arena mirror (GPU handle = mirror offset, registered for the
  unified-handle translation) and ExecuteCommandLists memcpys the whole 38 MiB heap into it (d3d12_com.cpp -> ce heap_sync_fn). Wine sq1
  ran with it to state 3 (2048 syncs / 150 s), no crash. Always-on logs: console heap creation; "table handle ... above 4 GiB" in the xs
  list proxy (pc 31/32), none under Wine. Wine sq2 (DISPATCH_SYNC=1): boots to state 3, "64 dispatches, 128 tables, 132 V# buffers".
- Handed ri51 (re51 = re48 + DISPATCH_SYNC, PTR_CHECK=2, 420 s) and ri52 (re52 = re48 - PTR_CHECK + DISPATCH_SYNC, 480 s).
- CORRECTION (agA17, later): ri50 had 0 "does not fit 32 bits" lines, so truncation is ruled out (Definite for ri50). ri51 (build 58,
  DISPATCH_SYNC=1 incl. V# buffers) was WORSE: lost at t~70 in state 3 (cs_87dac0f8, READ_INVALID 0x191c7b930000). Reason: placed-buffer
  stand-ins are committed resources at their OWN arena VA (slot 133 "not tied to va"), so copying title memory to arena[title VA]
  overwrites whatever arena allocation lives there. (The draw "ring sync" does the same, only in small windows.) DISPATCH_SYNC is now
  =1 tables only / =2 +V#s; do not use either.
- REAL ROOT CAUSE (Very likely; Wine sq4 with LEONIDA_CE_TABLE_LOG / TABLE_DIAG): the fspx compute tables are raw handles INTO THE CONSOLE
  DESCRIPTOR HEAP (e.g. s5 0x1aaf340, 0x1acf340, ... stepping 0x20000, inside heap 0x10000 + 38 MiB; s4 0xa2e180). Title memory there
  holds real descriptors (s5 slot 0 T# 01000003 03800000 00000000 90000fac); the arena at that VA, which the shader reads, holds zeros
  (Wine: upload ring) or other buffers' bytes (Windows: floats, host pointers -> the device loss). The heap GPU handle == title
  address, and arena VA X is not title memory X. Unregistered (cpu == gpu), so the unified-handle translation never touched it.
- FIX: LEONIDA_CE_HEAP_MIRROR=1 (build 59, md5 a40ba493): arena mirror of the console heap, GPU handle = mirror offset (registered,
  so table handles translate into it), whole-heap memcpy title -> mirror on every ExecuteCommandLists. Wine sq3/sq4: same boot timing
  with and without it. Handed ri53 (re53 = re48 + HEAP_MIRROR + TABLE_DIAG=12, PTR_CHECK=2) and ri54 (re54 = re48 - PTR_CHECK + HEAP_MIRROR).
  Also new: LEONIDA_CE_TABLE_LOG=N (xs list proxy: raw compute table handles), LEONIDA_CE_TABLE_DIAG=N (replay: title vs arena words).
- ri53 (B; build 59, re53 = HEAP_MIRROR + TABLE_DIAG=12 + PTR_CHECK=2): heap at 0x148a0000 mirrored to arena VA 0xee10000; table handles
  now land in the mirror (s5 0x1032f340.. step 0x20000, s4 0xf82de00). PTR_CHECK DOES read the mirror: s5 slot 0 = the real T#
  [01000003 03800000 00000000 90000fac] (flagged only because its base 0x100000300 is a title texture above the arena). The remaining
  "bad" lines are slots 6..31 (title heap bytes the title never filled for this table; the scan reads 32 slots blindly), so
  PTR_CHECK totals are no longer a valid metric here. STILL LOST ~120 s after Confirm (t=192.2 -> ~312), but checkpoints moved: no
  cs_87dac0f8/cs_25d19264; compute queues at cs_c5b1005e, cs_344692c2, cs_cee65a2d; direct at a list-begin marker. No fault address
  (vkGetDeviceFaultInfoEXT -13). COST: the 38 MiB per-ExecuteCommandLists copy makes it ~3x slower (boot 2x slower, 14 fps falling to
  4 fps). No frames (B's puller exited early). ri54 (screens, no PTR_CHECK) NOT run (B out of time).
- NEXT: (1) cut the mirror cost: copy only dirty/used ranges (e.g. per-table windows of the mirror at replay prepare for dispatches and
  draws, from title heap + (va - mirror)), or write-watch (GetWriteWatch / VirtualProtect) the title heap pages and copy only dirtied
  64 KiB pages per submit. (2) run ri54-like (no PTR_CHECK) to see whether the loss persists without the PTR_CHECK slowdown, and get frames.
  (3) the new faulters cs_c5b1005e / cs_344692c2 / cs_cee65a2d (compute queue): check whether their tables/root views also point into
  the heap (TABLE_LOG) or into placed buffers (stand-ins at their own VA: same class of bug for V# bases that name placed-buffer title VAs).
- Recommended env now: build 59 + re54.env (re48 - PTR_CHECK + HEAP_MIRROR=1). Never DISPATCH_SYNC.

## agA18 (sender gen 18, 2026-11-25)
- ri54 (B; build 59 + re54 = HEAP_MIRROR=1, no PTR_CHECK, 480 s): Confirm t=123.5, DEVICE LOST ~138 s after Confirm (t~261; furthest yet).
  Checkpoints: direct queue stage 0x1 marker 0xCE01000000000000 = TOP OF AN ExecuteIndirect DRAW (kind 0; bottom = list begin);
  compute queues at cs_c5b1005e (x2), cs_344692c2, cs_cee65a2d. ri53 had the same direct-queue marker. vkGetDeviceFaultInfo -13.
  nvlddmkm Event 13 texts: "Graphics Exception: Class 0x0 Subchannel 0x0 Mismatch", "ESR 0x4041b0=0x0", "ESR 0x404000=0x80000002"
  (FE/front-end method decoder: the GPU executed garbage methods), no MMU fault text. => Likely NOT a shader reading a bad address;
  candidates: an indirect draw with garbage arguments/count (args written by GPU work through a title VA whose stand-in lives
  elsewhere, same class as the heap bug), or corrupted command memory. Pace with the 38 MiB mirror: 12.5 fps before Confirm,
  5-8 fps after. Presented-image probe reported all-zero frames; RTDIAG_DUMP wrote no ppm (use B's vshots.sh for screens).
  Just before the loss in ri53 AND ri54 the title makes its FIRST calls of list slots 26 (ResourceBarrier), 43 (root SRV),
  37, 45 (root UAV at title VA 0x103ddf21a0) and 80 (console-only, not forwarded, args 1,1,1,0xe00008020): a new render phase.
- Title heap memory (Wine VirtualQuery): MEM_MAPPED, MEM_RESERVE, one 64 KiB view per physical page (xmem physical-page window,
  committed on touch). VirtualProtect fails there with error 487 under Wine => write-watch / page protection cannot track it
  (GetWriteWatch needs MEM_PRIVATE + MEM_WRITE_WATCH; also impossible).
- Build 60 (gta6rt-60.exe, md5 ea00932c): LEONIDA_CE_HEAP_MIRROR=3 = host shadow + per-4 KiB compare each ExecuteCommandLists, write only changed
  pages to the arena mirror. Wine sr4/sr5: 164 KiB/sync early, 9-14 KiB/sync later (vs 38912 KiB/sync), same boot timing.
  =2 = VirtualProtect write tracking (VEH ce_hm_veh, also called first from xmem physical_page_fault via
  rt::xmem::write_track_hook) with compare fallback; under Wine every protect fails -> same as =3. =1 unchanged.
  LEONIDA_CE_HEAP_MIRROR_ROLL (default 8 chunks/sync) rolling re-compare (only matters for =2).
  LEONIDA_CE_PTR_CHECK_ONLY=sub,sub: PTR_CHECK only for matching pipeline names (Wine sr5: works).
  LEONIDA_CE_INDIRECT_LOG=1 (vulkan_replay.h ExecuteIndirect: one line per new kind/stride/max_count/count-buffer shape),
  LEONIDA_CE_SKIP_INDIRECT=1|2 (skip indirect draws | + indirect dispatches; a test, not a fix).
- Handed: ri55 = build 60 + re55 (re54 with HEAP_MIRROR=3 + INDIRECT_LOG), ri57 = re55 + SKIP_INDIRECT=1, ri56 = re55 + PTR_CHECK_ONLY on
  the three compute faulters.
- ri55 (B; build 60 + re55 = HEAP_MIRROR=3 + INDIRECT_LOG): Confirm t=143.6, pace before Confirm 4.5-5.1 s/60 (12-13 fps, = ri54), after
  Confirm 4.7-16 s/60 (4-13 fps) => the post-Confirm slowdown is NOT the mirror copy (Likely); mirror now writes 8 KiB/sync on Windows
  (8192 syncs, 67 MiB written in all). STILL DEVICE LOST, and the ONLY two INDIRECT lines of the whole run come right before it:
  "kind 2 stride 12 max_count 1 count_buffer no args 0xfee6da00" then "kind 0 stride 16 max_count 1 count_buffer no args 0xfee85000",
  then DEVICE LOST. => the first-ever indirect draw (args read from arena VA 0xfee85000) kills the device (Likely; with the FE exception
  text). Next: ri57 (SKIP_INDIRECT=1); then find what writes the args at 0xfee85000 (title placed buffer whose stand-in is there? GPU
  writes through a title VA?) and dump the 16 arg bytes at record/submit time.
  vshots: the title window is black in VM shots (presents go to the passthrough GPU); a stale Application Error dialog from an earlier
  run sits on screen. re57 adds LEONIDA_CE_PREVIEW=60 (GDI paint of read-back presents) so vshots can see frames.
- ri57 (B; build 60 + re57 = re55 + SKIP_INDIRECT=1 + PREVIEW=60): Confirm t=159.3, LOST ~45 s after Confirm, BEFORE any ExecuteIndirect (0
  INDIRECT/SKIP lines; SKIP never exercised). Different signature: direct queue at cs_25d19264 (old faulter), FaultInfo OK: READ_INVALID
  0x51a3ff40e000, IP 0x20c5a9ca0; only Event 153, no FE Event 13. So two failure modes now: (a) FE exception at the first indirect
  draw (ri53/54/55), (b) garbage-pointer read in fspx (ri57).
  Frame state (B): target0 (tex 100053700) gets ~300 draws, all navy ff2c2020; the PRESENTED resource (tex 100000000) and target2 get 15
  draws and are all zero => the window is black because the presented surface is not the one drawn into. PREVIEW=60 printed nothing.
- Build 61 (md5 f89c44c5): HEAP_MIRROR=3 + an unconditional rolling title->mirror copy (HEAP_MIRROR_ROLL, default 32 chunks = 2 MiB/sync,
  full refresh every 19 syncs). Theory (Plausible): another arena writer (title-VA ring sync) clobbers the mirror range; =1 repaired it
  each submit, =3 did not -> ri57's cs_25d19264 garbage read. Handed ri58 (re58 = re55 + ROLL=32) and ri59 (re59 = re58 + PTR_CHECK only
  on cs_25d19264/c5b1005e/344692c2/cee65a2d).
- NEXT: (1) ri58/ri59 results. (2) Indirect-draw FE fault: dump the 16 arg bytes at arena 0xfee85000 at submit (CPU view) and find the
  resource there (slot 133 placed buffers / stand-ins); clamp/validate args (vertexCount, instanceCount) in a prepass if garbage.
  (3) Check whether draw ring sync can write into the mirror range (log sync_title_range hits in [mirror, mirror+38 MiB)).
  (4) Present surface mismatch: 100000000 vs the drawn 100053700.
- Recommended env now: build 61 + re58.env.

## agA20 (sender gen 20, 2026-11-25)
- Tree: /root/leonida-ce branch "experimental" (was first-light a28601df9). Commits allowed there (author IAmNotSiob, Co-Authored-By
  trailer), never push / never touch main or first-light. agA20 commits: 6a526986e, 0a62e036a (+ later ones below).
- Build 62 (md5 dbc13842) = a28601df9 as is (build 61 + agA19 INDIRECT_DRAW + MIRROR_GUARD + main CI merge). Sender 19's own
  pre-merge build 62 was never run: renamed /root/handoff/gta6rt-62-a19-unsent.exe.
- ri58 / ri59 (build 61): lost ~110/136 s after Confirm at the first indirect draw (marker 0xCE01 kind 0), presented all zero.
- rk62 (B; build 62 + re62 = re58): Confirm 123.9 s, LOST ~124 s after Confirm, but now NOT at an indirect draw: the indexed
  indirect draws (kind 1, stride 20, args res +0x5bef628..) were recorded but never replayed (no replay log, last present frame
  2340 before them). Checkpoints: direct queue cs_f39d1d8b (top) / cs_4ae22e7a, compute queues cs_c5b1005e. FaultInfo works for
  the first time: 334 records, ALL type INSTRUCTION_POINTER_UNKNOWN in one 16 KiB code range, no memory fault => a runaway shader,
  not a bad pointer (Likely). The only indirect op replayed before it: the kind-2 indirect DISPATCH (stand-in args at 0xfee6d200).
  cs_f39d1d8b is straight-line (no loops, 1-D, 32 threads) - a huge group count from garbage args fits (Plausible).
  MIRROR_GUARD: 0 hits => the title-VA ring sync never writes into the mirror: mirror-overwrite theory KILLED (Very likely, rk62).
  The new render phase is always the same: first calls of list slots 26, 148, 43, 37, 45, 80 + first ROOT_SPILL dispatches.
- Presented surface zero with HEAP_MIRROR: ri45/48/49/50 (no mirror) presented navy from frame ~870 in state 3; ri53..rk62
  (mirror) zero from frame 0, identical otherwise. Wine never gets there (presented zero in every Wine run, mirror or not).
  Mechanism (Likely, static): replay hacks read the descriptor TABLE from TITLE memory at the table VA (RECTLIST vertices at
  table+0x20 = the composite blits, CB fix (also WRITES there), SF_STRIDE, VSHARP). Without the mirror the table VA is the title
  heap address (0x10000+x) so title memory there is right; with the mirror it is the mirror's arena VA (0xee10000+x) and title
  memory there is unrelated (and the CB fix wrote into it).
- Build 63 (md5 e767d096): LEONIDA_CE_MIRROR_REMAP (default on with HEAP_MIRROR): copy_title_bytes/title_readable and the
  CB-fix/SF_STRIDE writes map a mirror VA back to the title heap. LEONIDA_CE_INDIRECT_SRC=0..4 (draws + dispatches with a
  placed-resource stand-in arg buffer): logs stand-in / title CPU / arena[title VA] arg bytes; 4 = pick a valid source, else
  drop the draw / zero the dispatch. Crash dialogs: SetErrorMode at startup + title SetUnhandledExceptionFilter ignored
  (LEONIDA_CE_KEEP_UEF=0 restores) - the title installs 3 filters at boot, which is why "I have fallen" never printed on Windows;
  the VM "Application Error game+0x43a6b7a" box was ri57's crash (t=206), not rk62.
- Wine: st1/st2/st3 (builds 62/63) boot to state 1 and stall at frame ~610-640 with a hang dump, then crash at a Wine-module
  address 0x6fffffca63ac reading 0x202; sr5 (build 60) did the same => pre-existing, not a regression (Likely).
- rk64 (B; build 63 + re64 = re62 + INDIRECT_SRC=4): Confirm 129.5 s, LOST ~142 s after Confirm; checkpoints direct cs_956fdcc6,
  compute cs_c5b1005e (as rk62/ri54). "[error] I have fallen ... game+0x55aa29b read 0x99d53e910" now printed (filter shim works,
  no dialog). MIRROR_REMAP active (title heap at 0x7480000 this run). Presented STILL all zero (remap alone does not fix it).
  MIRROR_GUARD: 16384+ hits (ring-sync windows 0xf9ae000.. inside the mirror) => the mirror-overwrite was REAL in this layout
  (rk62's 0 hits depended on that run's heap/placed addresses); ri59's float-filled cs_c5b1005e table (VA 0xfe2f640) lies in the
  mirror next to those hits => ri57/ri59's garbage tables were that clobber (Likely). Fixed by the guard (build 62+).
  INDIRECT_SRC: ALL indirect args (49 dispatches: cs_152795e7, cs_6be10ff0 x17, cs_5c890e88, cs_e659191f, ...; 1 draw) are ZERO at
  submit in the stand-in, title memory and arena[title VA] => GPU-generated later (or never). The title placed buffer at title VA
  0x7980000 (0xae00000 bytes) has its stand-in at arena 0x4010000: D3D-level users (ExecuteIndirect, CBV_FIX root views) use the
  stand-in offset, shaders with title-VA V#s use arena[title VA] => GPU-written args never meet their reader. This is the
  "single address space" problem (docs/d3d12_vulkan.md 3.3), not fixable by a hack in one pass. Mode 4 (CPU pick) is meaningless
  for GPU-written args. Indirect ops do not cause the loss (dispatches were zero-group) (Likely).
- Build 64 (md5 de4eaa8b): LEONIDA_CE_MIRROR_GFX=0 = graphics tables keep the raw title-heap handle (pre-mirror behaviour), compute
  tables use the mirror; LEONIDA_CE_INDIRECT_SRC=1 = args from arena[title VA]. Wine st4: boots, "MIRROR_GFX=0 ... kept raw".
- rk66 (B; build 64 + re66 = re62 + MIRROR_GFX=0 + INDIRECT_SRC=1): PRESENTED SURFACE BACK. First non-zero presented frame 780
  (state 3): navy ff2c2020 with the title's "ACCOUNT PICKER (Y)  PLAYER (A)" prompts (/root/rb/rk66_frames/p000780.png); frame 990
  gray 17 (transition); after Confirm (123.9 s) navy loading screen through frame 2220. => job 4 fixed by MIRROR_GFX=0 (Definite
  for the probe/pulled frames; the mirror broke graphics only). Why graphics break with the mirror is still open: the graphics
  "tables" are the title's per-draw root blocks (V# + inline vertices, e.g. table 0xfbae200 = [V# 0x95a400/0x60 bytes][3 verts]..)
  written into the heap memory; MIRROR_REMAP fixed the CPU-side reads but not the GPU result (rk64), so something on the GPU side
  (V#s naming title VAs next to the table, or a sync window the raw path gets) differs.
  Device STILL lost after the new phase: checkpoints cs_956fdcc6 (direct) + cs_c5b1005e (compute), as rk64. INDIRECT_SRC=1 used
  arena[title VA] for 16 cs_6be10ff0 dispatches (all args 0 at submit): no effect on the loss.
  cs_c5b1005e / cs_956fdcc6 / cs_f39d1d8b have no backward branches (no loops) => not runaway shaders; the IP-only FaultInfo
  records are where waves sat at the loss. cs_c5b1005e (43 lines) loads 3 V#s from its heap table and STORES through two of them
  (title-VA bases -> arena[title VA], which for the big placed buffer is inside another stand-in) - a plausible corrupter.
- Handed rk68 = re66 + LEONIDA_CE_SKIP_CS=cs_c5b1005e,cs_956fdcc6 (env-only bisect of the loss).
- rk66 final: lost ~150 s after Confirm (checkpoints direct cs_956fdcc6, compute cs_c5b1005e x2, cs_cee65a2d, cs_344692c2; FaultInfo
  3 IP-only records). VM vshots /root/rb/rk66v v005-v013: the game WINDOW is gray 17 then navy - first visible screen this session.
- rk68 (re66 + SKIP_CS=cs_c5b1005e,cs_956fdcc6): both skipped; LOST EARLIER (~frame 1680, shortly after Confirm), checkpoint a
  graphics UI PSO (VVertexCxformEAlpha). => those two are not the sole cause; skipping them just breaks something sooner
  (victims, Plausible). The loss is a post-Confirm memory-consistency problem, most likely the two-address-scheme arena
  overlap (title-VA writes/ring-sync windows landing in other stand-ins).
- Build 65 (md5 3877fd30, respun once - the first 384fe704 had arena_host published only with the ring sync on):
  LEONIDA_CE_MIRROR_POKE (default on): CB-fix / SF_STRIDE patches of the per-draw root block also write the arena mirror. Theory
  (Plausible) for "graphics through the mirror = zero composite": the mirror was synced at ExecuteCommandLists BEFORE the replay
  patched the title heap, so the GPU used the unpatched CB V# (title base 0x40ade20 -> zeros) and the Cxform composite multiplied
  everything to transparent black. Also arena_host/arena_bytes now published even with LEONIDA_CE_NO_RING_SYNC.
  Handed rk69 (re69 = re62: graphics through the mirror) and rk70 (re69 + NO_RING_SYNC=1).
- rk69 (build 65 + re69 = re62, graphics THROUGH the mirror + MIRROR_POKE): presented non-zero from frame 810 (navy), vshots
  gray 17 then navy => MIRROR_POKE fixes the mirror composite (Very likely: build 63/rk64 without the poke = zero, same env).
  So the zero composite was the CB-fix patch reaching the mirror one submit late. Loss ~124 s after Confirm (cs_f39d1d8b /
  cs_4ae22e7a direct, cs_c5b1005e x2, cs_344692c2, cs_cee65a2d compute; FaultInfo -13).
- rk70 (re69 + NO_RING_SYNC=1): presented ALL ZERO again (the composite still needs the title-VA ring sync for its non-table
  data, e.g. the CB at root-0x80 / vertex V#s), loss ~144 s after Confirm (new checkpoints cs_7acaea81 direct, cs_752cc8dc +
  cs_c5b1005e compute). => the ring-sync overwrites are NOT the loss cause (Likely).
- Build 66 (md5 3733bb8c): LEONIDA_CE_PLACED_IDENTITY=1 (default off): slot-133 BUFFER stand-ins at arena VA == title VA when
  that range is free (hint via rt::d3d12_xs::set_arena_want_hook -> arena_allocate exact placement); heap mirror from the arena
  top. Wine st8: the 174 MiB buffer (title 0xb10000 under Wine) overlaps the upload ring -> not placed; all other placed buffers
  are above 4 GiB (0xd..-0xe.. title VAs) and can never be identity-placed. Handed rk71 = re69 + PLACED_IDENTITY=1.
- rk71 (build 66 + re71 = re69 + PLACED_IDENTITY=1): the 174 MiB buffer WAS placed at identity (title VA 0x27f80000 this run);
  all other slot-133 buffers sit above 4 GiB (0xd99..-0xe39..) and cannot be. Presented navy from frame 750. Confirm 143.0 s,
  STILL LOST at ~frame 2400 (checkpoints cs_edc5d4c7 / cs_a039b209 / cs_c5b1005e x3, FaultInfo -13, then an execute AV at
  0x1402bc345b8). => unifying the one low placed buffer does not fix the loss (Likely, 1 run).
- SUMMARY agA20: presented surface FIXED (HEAP_MIRROR + MIRROR_POKE, or MIRROR_GFX=0): title screen with UI and the
  post-Confirm navy loading screen are presented and visible in the VM window. Indirect draw FE fault gone since build 62 (the
  old path emitted the draw outside any render pass; no FE exception since). Mirror-overwrite theory CONFIRMED and guarded
  (MIRROR_GUARD hits 16k in rk64). The post-Confirm loss (~120-150 s after Confirm, new render phase: first list slots 26/148/
  43/37/45/80 + ROOT_SPILL dispatches) remains; it is not: indirect args (zero-group dispatches), ring-sync overwrites (rk70),
  cs_c5b1005e/cs_956fdcc6 alone (rk68), runaway loops (no backward branches), the big placed buffer address split (rk71).
- NEXT: (1) the high title VAs (0xd9..-0xe3.., all placed buffers except the big one) are outside the arena: any title V# naming
  them is translated how? (heap_base + VA overflows the arena => robust drop or MMU fault) - log the V# bases the fspx compute
  shaders of the new phase use (PTR_CHECK_ONLY=cs_c5b1005e,cs_edc5d4c7,cs_a039b209,cs_f39d1d8b on build 66) and check whether
  they name those high VAs. (2) "list slot 80 (not forwarded) args 1,1,1,0xe00008020" is the last new console call before every
  loss - find what it is (a console-only GPU op skipped = missing work/sync). (3) Event 153 with FaultInfo -13 or IP-only
  records = TDR-style hang: try VK timeouts per queue / one submit per list to localize.
- Recommended env: build 66 (gta6rt-66.exe) + /root/handoff/re69.env (HEAP_MIRROR=3, ROLL=32, graphics through the mirror with
  MIRROR_POKE/REMAP/GUARD default on). Alternative: re66 (MIRROR_GFX=0). Never DISPATCH_SYNC; NO_RING_SYNC breaks the composite.

## agA21 (sender gen 21, 2026-11-25)
- Lead 2 SOLVED statically: "console list slot 80 (not forwarded) args 1,1,1,0xe00008020" comes from 0x1800149C0 = XboxBvhBuilder.dll
  (image base 0x180000000) RVA 0x149c0, right after the call at 0x149ba that docs/d3d12_command_objects.md 3.3a/3.5a measured as the
  vtable fallback of the five-dword dispatch packet 0x4003cc00,x,1,1,W: slot 80 = Dispatch(x, 1, 1) + W (0x8020 here; the high half
  of a4 is stale r9). The runtime returned 0 for it = every such BVH-build dispatch after Confirm was SILENTLY DROPPED. The other
  new-phase calls (26, 148, 43, 37, 45) are the same builder's barrier / upload / root bindings. So "new render phase" = first
  acceleration-structure builds; with part of the build dispatches missing, the BVH holds stale/garbage nodes and anything
  traversing it can spin until TDR (fits Event 153 with IP-only/-13 FaultInfo and no MMU fault). (Likely, static + timing.)
- Build 67 (gta6rt-67.exe, md5 f4cc6049, commit ce3d97df9): LEONIDA_CE_SLOT80_DISPATCH (default ON, =0 = old drop): slot 80 ->
  real list Dispatch(a1, a2, a3) (sanity: all non-zero and <= 65535). Log "[hack] SLOT80_DISPATCH #k from <caller>: Dispatch(..) W=..".
  Handed rk73 = build 67 + re73.env (= re69, rtdump93). Also handed rk72 = build 66 + re72 (= re69 + PTR_CHECK=2 on cs_c5b1005e,
  cs_edc5d4c7, cs_a039b209, cs_f39d1d8b, SLOTS=24), lead 1.
- Other console list slots still dropped (first call each, rk71): 62 from game+0x979699 (dst VA 0xdaa7ac080, src VA 0xbdd8f0000,
  0x4000 bytes, 0) looks like a GPU memcpy by VA (fn 0x140979650: dst [rdx+0x50], src = f([rdx+0x30]), size [rdx+0x24]); 101 at
  0x1450a6651 = (2, 1); 129 via helper 0x14555e640 (0x5c, 1, 1, ..); 70, 74, 75, 126, 131, 132, 142. All start BEFORE Confirm (the
  screens work with them dropped), but slot 62 is the next candidate if a missing copy shows up.
- rk72 (build 66 + re72 PTR_CHECK): stopped by B before Confirm (budget) to make room for rk73 - lead 1 (high-VA V#s) still untested.
- rk73 (build 67 + re73): slot 80 forwarded (SLOT80_DISPATCH up to #256: callers BVH RVAs 0x149c0 (1,1,1,W=0x8020), 0x15263 / 0x153ba
  (x=1..7, W=0x20)). STILL LOST ~139 s after Confirm (Confirm 132.5 s, last probe frame ~2400), same checkpoints (direct
  cs_956fdcc6, compute cs_c5b1005e x2 / cs_cee65a2d / cs_344692c2), FaultInfo 2 records type 4 (IP_UNKNOWN) 0x2084d0190/0x2084d00a0.
  Screen: navy from frame 810 to the end (flat loading screen, no UI); vshots /root/rb/rk73v, frames /root/rb/rk73_frames.
- W word decoded (Likely, RDNA COMPUTE_DISPATCH_INITIATOR): bit 5 USE_THREAD_DIMENSIONS (x,y,z = THREAD counts, hardware masks the
  partial group), bit 15 CS_W32_EN. The builder computes x as an element count (0x14972 site: ceil((144 n^2 + 64)/16) or [rcx+0x38]).
  Build 67 therefore over-dispatched by the workgroup size.
- Build 68 (gta6rt-68.exe, md5 28058f36, commit d02be80ec): LEONIDA_CE_SLOT80_THREADS (default ON with SLOT80_DISPATCH; =0 = build 67):
  groups = ceil(threads / workgroup size of the bound compute PSO) (list slot 25 tracked per list proxy; Vulkan PSO workgroup size
  through leonida::ce::local_size_fn, src/ce_checkpoints.h). Partial last group runs full width (no hardware masking on Vulkan).
- rk74 (build 68 + re74 = re69, rtdump94): conversion works (e.g. (75,1,1) W=0x20 -> 2 groups of 64; ls 32 for the W32 kind).
  STILL LOST ~148 s after Confirm (Confirm 166.8 s, last probe frame 2640; direct queue had NO checkpoint), same compute checkpoints (cs_c5b1005e x2, cs_344692c2, cs_cee65a2d), FaultInfo
  1 record type 4 0x20851bb90. => the BVH slot-80 dispatches (dropped, over-dispatched, or exact) are NOT the loss cause (Likely,
  2 runs). Keep build 68 anyway: it is the correct behaviour for slot 80.
- NEXT: (1) lead 1 still open: staged as rk75 = build 68 + /root/handoff/re75.env, PTR_CHECK_ONLY on cs_c5b1005e,cs_344692c2,cs_cee65a2d,cs_956fdcc6 (the four
  checkpoint shaders of rk73/rk74), let it run past Confirm. (2) The loss is at a near-constant ~135-145 s after Confirm while the
  streamer reads core_bvh4.rpf / ttdb terrain (dstorage #80000, ~2.1 GiB) - check arena/VRAM growth (LEONIDA_ARENA_LOG) right
  before the loss; a time-based trigger (streaming reaching some asset) fits better than a first-call trigger now. (3) Per-queue
  localization needs a new switch (no env routes all queues to one VkQueue today): e.g. a vkQueueWaitIdle after each submit on
  compute queues only, to see whether the hang is on the compute or direct side.
- Recommended env: build 68 + /root/handoff/re74.env (= re69). Screens: title UI (ACCOUNT PICKER / PLAYER prompts) then the navy
  post-Confirm loading screen; nothing past it yet.
- (new receiver a93593c06bc558af6 names runs rlN)
- rl75 (build 68 + re75, PTR_CHECK on cs_c5b1005e/344692c2/cee65a2d/956fdcc6): Confirm 129.4 s, LOST ~139 s after Confirm. Checkpoints
  direct cs_00f50ab8, compute cs_c5b1005e x2 + cs_752cc8dc. FaultInfo: type 6 IP 0x20ada0940 + type 1 READ_INVALID 0x6b9b8f7da000.
  PTR_CHECK: no V# base in 0xd9..-0xe3.. (lead 1 negative for these shaders). Its "bad" entries (floats in table slots 4/5/16/21/22)
  are FALSE POSITIVES: cs_c5b1005e (disasm, 43 lines, loop-free) reads only table slots 0, 1, 2 (s_load s4+0x0/0x20/0x40).
- Build 69 (md5 aa040ef8, 75e031bcd): LEONIDA_CE_INDIRECT_CLAMP=1 (default off): every count-less ExecuteIndirect DISPATCH goes
  through the existing count-gating pass with a clamp variant (src/indirect_dispatch_clamp_spv.h, hand-edited SPIR-V, spirv-val ok):
  records with any group count > 65535 become (0,0,0). Never run alone.
- Build 70 (md5 279ac0ed, 4e06d28e9): PTR_CHECK prints the TITLE heap bytes for a bad entry inside the heap mirror.
- rl77 (build 70 + re77 = re69 + INDIRECT_CLAMP=1 + PTR_CHECK): every flagged mirror entry "(= mirror)": the mirror is faithful,
  the floats are title data in slots the shader never reads => descriptor tables OK (Very likely). INDIRECT_CLAMP active (16+
  dispatches) and STILL LOST at ~frame 2670 => giant GPU-written indirect dispatch counts are not the cause (Likely).
  At the loss 4 compute queues sit at the SAME marker = the cs_c5b1005e pipeline (marker = VkPipeline handle), direct cs_d80e80ca.
- Arena never frees ("0 releases", live 1.3 -> 1.9 GiB over the run, largest free block 2.7 -> 2.0 GiB): a leak but not exhausted
  at the loss. Bindless "2D-array full" before Confirm and "1D full" ~at Confirm+60 s in every run (misses fall back; the T# hash table
  probes are bounded at 8, no infinite probe) - probably harmless.
- Build 71 (md5 dfdf1661, 87f18b27f): LEONIDA_CE_DISPATCH_LOG=1 (direct dispatch per-pipeline group-count highs >= 4096 with s0-s11),
  LEONIDA_CE_DISPATCH_MAX=N (skip direct dispatches over N groups). Handed rl78 = build 71 + re78 (= re69 + DISPATCH_LOG=1, rtdump98).
- Wine note: gta6rt.sh --run needs DISPLAY=:1 here; st9 (build 71) without it hit int3 at game+0x19f4f58 after the HDR query, with
  it stalled right after the HDR query within 150 s (did not investigate; st8 by agA20 ran fine, launch recipe not recorded).
- KEY OBSERVATION (rl77, also rk71/rk73 by line position): the loss comes within the same second as the FIRST ray-tracing
  acceleration-structure build: list slot 26/148/43/37/45 first calls (XboxBvhBuilder.dll), then ROOT_SPILL dispatches #1..#256
  (the builder compute signature "14 spill dwords behind s[0:1]"), then DEVICE LOST. The ~139-148 s after Confirm is when the world
  load finishes and the first BVH build is recorded (time-based, not frame-based: pace varies 7-17 s/60 frames, the delay does not).
  Theory (Likely): the builder receives geometry / dest / scratch addresses from the title as DATA (build descs, root constants,
  slot-148 upload records of count*40 bytes) - raw title VAs, many above 4 GiB (title placed buffers 0xd9..-0xe3..; slot 45 root UAV
  0x103ddf21a0) - which the runtime never translates, so its shaders read/write heap_base + title VA, far outside the arena
  (READ_INVALID 0x6b9b8f7da000 in rl75, garbage writes elsewhere -> hang). Root views from the title ARE translated (CBV_FIX), data
  is not.
- Build 72 (md5 ee0cfc38, 9fc74143a): LEONIDA_CE_SKIP_BVH=1 (default off, bisect): list slots 14/80/61 called from the builder image
  (return address 0x180000000..0x1800de000) are dropped. Handed rl79 = build 72 + re79 (= re78 + SKIP_BVH=1, rtdump99).
- FIX DESIGN if rl79 survives: wrap the game imports of XboxBvhBuilder exports (BuildRaytracingAccelerationStructure RVA 0x21610,
  ...Batched2X 0x216a0, CopyRaytracingAccelerationStructure 0x21980, Rehydrate*) and rewrite every GPU VA in the descs
  (DestAccelerationStructureData, ScratchAccelerationStructureData, SourceAccelerationStructureData, geometry VertexBuffer.StartAddress,
  IndexBuffer, Transform3x4, AABBs.StartAddress, instance descs) from title VA to the placed buffer stand-in arena VA
  (xs_placed_buffer_va), docs/xdxr.md + d3d12_command_objects.md 3.3b have the layouts. TLAS instance descs hold BLAS addresses in
  GPU memory (written by the title) - those need the same translation (CPU pass over the instance buffer, or identity placement).
- rl78 (build 71 + re78, DISPATCH_LOG): Confirm 177.8 s, LOST 130 s after Confirm; 12 DISPATCH_LOG lines, largest 65536 groups, none
  for the checkpoint shaders => no giant direct dispatch (Likely). FaultInfo 332 IP-only records in one 8 KiB code range.
- rl79 (build 72 + re79, SKIP_BVH=1): builder dispatches dropped (slot 14 callers 0x180021057/1cba2/209af/1d3e7/1bdb8, slot 80
  0x1800149c0/153ba/15263, >1024 each). STILL LOST, 164 s after Confirm (later than the 130-148 s of the other runs, 1 run). Between
  the first builder call and the loss the game now records ~40 new world-render compute passes (DISPATCH_LOG: 160x90, 320x180,
  40x23x17 froxels, 23400/57600-group 1-D passes) => the BVH build is NOT the cause either (Likely); the loss follows whatever heavy
  GPU work starts when the world load completes. Probe target0 changed tint (ff2e1e23 vs navy ff2c2020); screen still dark navy.
- EVERY checkpoint shader of every run is loop-free (d80e80ca, d5613de2, c5b1005e, f39d1d8b, 4ae22e7a, 956fdcc6, 00f50ab8, 752cc8dc,
  344692c2, cee65a2d, edc5d4c7, a039b209: 0 backward branches in the disasm). With no giant dispatch (rl78) and no bad descriptors
  (rl77), a true hang is hard to explain.
- NEW THEORY (Plausible): a TDR from SLOW work, not a hang. The arena (every buffer, heap mirror, upload ring) is ONE 4 GiB
  HOST_VISIBLE|COHERENT allocation (d3d12_vulkan_device.cpp ~6621, DEVICE_LOCAL only preferred) and the VM GPU has no Resizable BAR
  ("upload heaps in system memory (no Resizable BAR)"), so all buffer traffic goes over PCIe from system RAM. Pace is already 4-8 fps
  on a loading screen; when world rendering starts, one submission can exceed the 2 s TDR => Event 153, IP-only FaultInfo, all
  busy queues sitting in ordinary shaders, ~constant time after Confirm (= load time).
  Tests (not done; need the user): raise TdrDelay/TdrDdiDelay in the VM (HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers,
  then reboot) - my request to the receiver for this VM registry change was refused by the permission classifier, so it needs the
  user's OK; or enable Resizable BAR for the passthrough GPU. Code-side: an arena whose GPU-only part is DEVICE_LOCAL (split CPU-
  written ranges - upload ring, heap mirror - into a small host-visible region and keep the rest device-local, with staging copies
  for CPU writes), or measure per-submit GPU time with timestamp queries first.
- Recommended env: build 72 (default switches = build 68 behaviour + diagnostics off) + /root/handoff/re74.env (= re69).
- VM facts (receiver, read-only): GPU NVIDIA RTX 6000 Ada (48 GiB), driver 582.70, passthrough; BAR1 = 256 MiB (Resizable BAR NOT
  active; 228 MiB already used after rl79); TdrDelay/TdrDdiDelay/TdrLevel unset (default 2 s). Every loss is nvlddmkm Event 153
  (channel timeout/reset), never Event 13 MMU / 4101. => supports the slow-work TDR theory (Plausible -> Likely): the 4 GiB arena
  cannot be DEVICE_LOCAL+HOST_VISIBLE without ReBAR, so it sits in system RAM, and a 48 GiB GPU runs every buffer access over PCIe.
  NEXT (needs user approval for VM changes): enable ReBAR for the passthrough GPU or raise TdrDelay; code-side: a DEVICE_LOCAL arena
  with host writes staged (largest job), or first time per-submit GPU work with timestamp queries to confirm >2 s submissions.
- USER APPROVED + APPLIED TdrDelay=60 / TdrDdiDelay=60 on the VM (rebooted 09:30). Receiver gen 14 = ab6cc60c6208b83d3 (runs rmN).
- rm1 (build 72 + re74, TDR 60 s): boot slow after the reboot (13->0 at 294 s). STILL LOST: Event 153 at ~Confirm +184 s. Presents
  STOPPED right after the first BVH build (list slot 148 first call, SLOT80 #1), "[hang] no present for 15718 / 30187 ms", then the
  loss ~60 s later = the new TDR limit. => NOT slow work: the GPU makes no progress at all (a real hang or a wait that never ends).
  SLOW-WORK THEORY REFUTED (Very likely). Checkpoints direct + 2 compute: cs_d80e80ca (top) / cs_d5613de2 (0x2000); FaultInfo empty.
- Build 73 (md5 d368a866, 54adace4c): LEONIDA_CE_GPU_TIME=1 (+ _MS threshold): GPU timestamp pair per replayed command buffer on
  direct/compute queues, read back without waiting; logs slow submissions and per-queue summaries (avg/max per 512).
- Build 74 (md5 67164d7e, 2770f26f3): WAIT_DUMP (always on): queue Wait/Signal and CPU fence Signal noted per (queue, fence); the hang
  watch prints every recent wait with the fence completed value and the highest signal issued ("^ BLOCKED - NO signal of that value
  was ever issued" marks a wait nobody will satisfy). Handed rm3 = build 74 + re82 (GPU_TIME on).
- Device-local arena (the requested "real fix" for the slow theory) NOT started: rm1 refuted the theory it was meant to fix. Design
  notes for later (perf, not the loss): CPU writes to the arena come from many places (descriptor heaps written in place, Map() of
  upload buffers, upload ring written by the title through CPU pointers, heap mirror, shader code, ring sync); a device-local arena
  needs a host shadow (VirtualAlloc MEM_WRITE_WATCH + VK_EXT_external_memory_host import) and per-submit page uploads limited to
  CPU-writable allocations, plus readbacks for READBACK-heap buffers; placed buffers imported from title pages stay in system RAM.
- rm3 (build 74 + re82, TDR 60, last run before the tools agent takes the VM): Confirm 143.0 s. GPU_TIME: direct queue submissions
  are SHORT - avg 0.03-1.3 ms, max 11-13 ms per 512-submission window, 0 submissions >= 250 ms, up to 11264 timed (no compute-queue
  summary line: fewer than 512 timed there). => the GPU is not slow per submission; the low fps is CPU-side (Very likely).
  Hang again right after the first BVH build (slot 148 first call at log line 16159, "[hang] no present for 15312 ms" at 16311).
  WAIT_DUMP: "0 (queue, fence) waits, 164 signal sources" - the title never calls ID3D12CommandQueue::Wait, so no D3D-level fence
  wait cycle (Likely). Internal runtime waits (storage-init / bindless-upload / ordered-timestamp semaphores, vkCmdWaitEvents) are
  not covered by the dump.
  Between the BVH start and the hang: many GPU-driven INDIRECT DRAWS (args in the big placed-buffer stand-in 0x8c2cdb0 and in
  0xffdc0400.., all zero at record time, written by the GPU later) with "RECTLIST miss". Indirect draws are NOT covered by
  INDIRECT_CLAMP (dispatches only): a draw with GPU-written garbage counts (title-VA vs stand-in split, agA20 rk64) could run for
  minutes => candidate for the hang (Plausible). ri53/54 already saw FE exceptions at the first indirect draw.
- FOR THE TOOLS AGENT (validation / GPU-AV / sync validation / Aftermath): (1) Aftermath dump of the hang: which draw/dispatch is
  active on the direct queue after the first BVH build; check indirect DRAW argument values at execution (GPU-AV indirect checks).
  (2) GPU-AV on the XboxBvhBuilder dispatches (ROOT_SPILL compute signature) and the indirect draws right after them. (3) Sync
  validation across direct/compute queues around the BVH build (global UAV barrier slot 146 -> GCL7 Barrier). (4) Internal waits:
  vkCmdWaitEvents (order_timestamp_copy), timeline waits on storage_init / bindless_upload semaphores.
- rm3 addendum (receiver): Event 153 at ~Confirm +170 s. Only ONE slow submission in the whole run: direct #11952 took 362.0 ms on the GPU (log line 15875, ~440 lines before the first hang dump), right after an INDIRECT dispatch of cs_152795e7 (stand-in args all 0 at record time) - a GPU-written indirect arg candidate for the tools agent. Checkpoint at the loss: direct queue cs_956fdcc6. Last frame 2429.

## agA22 (sender gen 22, 2026-11-25)
- Receiver a1bd4ce8bc63f1976 (runs rnN). Base env /root/handoff/tv6.env (re74 + VA_GUARD on 81 fspx shaders, window 4 GiB).
- Build 75 (gta6rt-75.exe md5 d4454d16, commit 9a85d2b53): SYNC_DUMP (always on; printed by the hang watch after WAIT_DUMP):
  every queue's submitted/completed (progress timeline counter), outstanding submissions (last 64 per queue) with each wait
  "-> ok / PENDING / BLOCKED - NO SIGNAL / DESTROYED / UNREGISTERED", pending + HELD waits, storage-init / bindless-upload
  semaphore+value, and every timeline semaphore that a GPU submission waited on with current / max-signal / max-wait and
  "<== DEADLOCK" tags. Registry: ce_sync:: in vulkan_backend.h (submit(), sparse bind, VulkanFence ctor/dtor/CPU signal).
  Also LEONIDA_CE_QUERY_NOWAIT=1 (query result copies without WAIT_BIT) and a fix: a destroyed queue now also clears the
  bindless-upload wait (only storage-init was cleared: a queue created later would have waited on a dead semaphore handle).
- rn1 (build 75 + tv6, = rn1.env): Confirm t=166.6, hang ~135 s later (last frame 2822), again right after the first BVH build
  (slot 26/148/43/37/45 first calls). SYNC_DUMP (/root/rb/rn1.sync1.txt): NO deadlock. Direct queue: completed 28036, first
  outstanding ticket 28037 has 0 waits -> stalled INSIDE its command buffer. Compute queues 1-4 (fresh: 6-8 submissions each, all
  at the BVH start) also stalled inside their first outstanding cmdbuf with all waits satisfied (their only waits are
  queue0 >= 17983 / 21918, long reached). Dump 2 (15 s later) identical. Every wait in the process is satisfied.
  => THE "DEADLOCK" THEORY IS REFUTED (Definite for timeline semaphores / D3D fences / bindless+storage waits / held batches):
  nothing waits on an unsignalled value; the GPU stops executing inside command buffers on all queues at once.
  Checkpoints at the loss: direct + 2 compute queues report the SAME pair: top cs_d80e80ca, bottom cs_d5613de2. FaultInfo 0.
- rn2 (build 75 + QUERY_NOWAIT=1): STILL hangs (~140 s after Confirm, frame 2644), same SYNC_DUMP picture (compute queues had
  completed 12-19 submissions this time). Checkpoint direct cs_edc5d4c7 (top+bottom); FaultInfo type 6 IP 0x20af931d0 and
  type 1 READ_INVALID 0x49ec40d3c000 (garbage address again, despite VA_GUARD). => query WAIT_BIT is not it (Very likely; the
  title never calls Begin/End/ResolveQuery anyway: console slots 57/58/59 never get a first call).
  Note: SYNC_DUMP shows only the last 64 tickets per queue; in rn2 the real first outstanding (28227) was beyond that.
- Screens: rn1/rn2 identical to before - setup screen (ACCOUNT PICKER / PLAYER prompts) before Confirm, then flat #111111 /
  #20202C frames (nothing rendered) until the hang. /root/rb/rn1_frames, /root/rb/rn2_frames.
- Build 76 (gta6rt-76.exe md5 77a2807d, commit 156df8afb): LEONIDA_CE_CP_FINE=1 = an NV checkpoint before every replayed command
  (0xCE03 | variant index << 32 | command index; the device-lost dump decodes it; variant table in build-76.txt / command_list.h
  667), LEONIDA_CE_INDIRECT_CLAMP_MAX=N (patches the clamp pass constant 65535). Handed rn3 (tv6 + CP_FINE) and rn4 (rn3 +
  SKIP_INDIRECT=2 = every ExecuteIndirect draw and dispatch skipped, bisect).
- rn3 (build 76 + CP_FINE): hang (frame 2553). CP_FINE markers only on the 2 compute queues that had FINISHED all work (their
  last command = a BufferBarrier; not the stall); the stuck direct queue and fresh compute queues 3/4 returned 0 checkpoints.
- rn4 (build 76 + CP_FINE + SKIP_INDIRECT=2, every ExecuteIndirect draw+dispatch skipped, 1024+ skips): STILL hangs (frame 2328,
  ~155 s after Confirm) => ExecuteIndirect is NOT the cause (Likely). Direct queue checkpoints: BOTTOM = pipeline marker of
  cs_d5613de2 (= everything BEFORE its dispatch is done), TOP = CP_FINE "before command #16, variant 23 (Dispatch)" = the CP
  reached the next dispatch and waits there => the cs_d5613de2 dispatch itself never finishes. rn1 had bottom cs_d5613de2 too.
- rn5 (VA_GUARD=1 on every CS) INVALID: it also guards the runtime's bvh/* kernels and the boot stalls at state 0 (black).
  Use a list (or a game-only pattern) for VA_GUARD, never 1.
- ROOT CAUSE FOUND (Likely): cs_d5613de2 is NOT loop-free (the agA21 "0 backward branches" check missed "s_cbranch_vccz 65519" =
  -17 dwords). It is a 1-D blur: s8 = s_buffer_load(V# from table s4+0xca0, +0x43c) (float radius); v3 = -s8; loop { sample;
  v3 += 1.0 } while s8 >= v3. With a garbage/huge s8 the loop runs ~2*s8 times per thread, and for s8 >= 2^24 v3 + 1.0 == v3
  => endless. LOOP_CAP=fspx_game was set but spirv_cap_loops() returned 0 for EVERY module with more than one OpFunction
  ("functions != 1"), and cs_d5613de2 has 3 functions => never capped. Offline (g++ harness over spirv/fspx_game/cs_*.spv):
  before the fix only single-function modules were capped; after it 1396 / 1536 modules get caps (10135 loops), spirv-val
  (vulkan1.2) passes on ALL of them. Default LOOP_CAP_INNER = 4M iterations is still a hang for a full-screen blur => set
  LEONIDA_CE_LOOP_CAP_INNER=4096.
- Build 77 (gta6rt-77.exe md5 3c57e2c8, commit 491960f2e): spirv_loop_cap.h caps loops in multi-function modules (counter var in the
  loop's own function); SYNC_DUMP keeps 512 tickets/queue. Handed rn6 = build 77 + rn6.env (= rn3 + LOOP_CAP_INNER=4096).
- Open: the radius s8 is garbage because its cbuffer V# / data is wrong (same family as the fspx garbage-V# issue); the cap
  only stops the hang. cs_d80e80ca / cs_edc5d4c7 / cs_956fdcc6 checkpoints in other runs were the dispatches queued right
  after (CP waiting), not the culprits - check their predecessors for loops the same way.
- rn6 (build 77 + LOOP_CAP=fspx_game, INNER=4096 -> 1396 modules changed): never got past boot state 0 in 210+ s (black, 1502
  background compiles = cold pipeline cache, or a capped boot shader). Stopped. Lesson: changing every module = cold cache.
- rn7 (build 77 + LOOP_CAP=cs_d5613de2 only, INNER 4096): boots normally (warm cache), d5613de2 capped, STILL hangs ~128 s after
  Confirm (frame 2420). Now only the direct queue has work (no compute submissions at all), checkpoint BOTTOM = cs_f33ac9e9
  (the re34 endless-barrier-loop coalescer), TOP = CP_FINE before command #90 (BufferBarrier). NOT a fair test: the pattern
  "cs_d5613de2" dropped the caps "fspx_game" used to give the 55 single-function modules, f33ac9e9 among them.
  => the hang follows whichever data-dependent loop is uncapped (Plausible-Likely): the stuck dispatch was d5613de2 in rn1/rn4,
  f33ac9e9 once uncapped.
- rn8 = build 77 + rn8.env: LOOP_CAP = the 55 previously capped modules + cs_d5613de2, INNER 16384 (warm cache except 56 modules).
  rn9 = LOOP_CAP=fspx_game (all 1396) INNER 65536, cold cache, ~900 s.
- rn8 (55 old caps + d5613de2, INNER 16384, 56 LOOP_CAP lines): STILL hangs (frame 2170, ~137 s after Confirm). Stuck dispatch
  (direct BOTTOM) = cs_d94af257 (TOP = before command #30, TextureBarrier). cs_d94af257 has the SAME float-radius blur loop
  ("s_cbranch_vccz 65519") and 5 OpFunctions => was never capped. Every run: the stuck dispatch is the next uncapped loopy shader.
- Loopy list: /root/handoff/loopy_fspx_cs.txt = 245 fspx_game CS whose disasm has a real backward branch (simm16 >= 33000;
  the agA21 "0 backward branches" claim was wrong for d5613de2 / d94af257). rn10.env = rn8 + LOOP_CAP on those 245 + the old 55
  (294 modules, partly cold cache). rn9 = LOOP_CAP=fspx_game everywhere (cold: boot 0 -> 1 only at t=363 s).
- NEXT: (1) run rn10 (or rn9 again on the warmed cache) to confirm no device loss after Confirm with all loops capped, and see
  what renders. (2) The radius is a cbuffer float (V# from table s4+0xca0, +0x43c) - garbage => find why that cbuffer is wrong
  (likely the fspx garbage-V# issue, VA_GUARD only hides it). (3) LOOP_CAP default INNER (4M) is useless for per-pixel loops;
  16384 looks safe, 4096 untested for boot (rn6 was cold).
- rn9 (LOOP_CAP=fspx_game everywhere, INNER 65536, cold cache): boot 0 -> 1 only at t=363 s (1593 background compiles), then the
  process CRASHED host-side at t=377.8: 0xc0000005 in ntdll (read 0xd84cbff818, stack-like) from gta6rt.exe+0xf1dce <- +0xf3cb9 on a
  runtime thread, pace 19-32 s/60 frames before it. No verdict on the hang. Unclear whether the pipeline cache got saved (it is
  written near the end of a run). The crash site gta6rt.exe+0xf1dce is not investigated.
- Recommended env (agA22): build 77 + /root/handoff/rn10.env (tv6 + CP_FINE + LOOP_CAP on the 294 loopy/old modules, INNER 16384),
  UNTESTED. Proven-safe fallback: build 77 + tv6.env (behaves like build 74; the multi-function cap only acts where LOOP_CAP names it).
  Every run's files: /root/rb/rnN.err.log, rnN.syncN.txt, rnN_frames/.

## agA23 (sender gen 23, 2026-11-25)
- Receiver ada4c52511e1337a1 (B16). Builds 78+.
- rn9 host crash (t=377.8, AV in ntdll reading 0xd84cbff818): gta6rt+0xf1dce = xs_dump_thread_stacks (d3d12_xs_device.cpp,
  the LEONIDA_CE_STACKS frame-4000 dumper thread; +0xf3cb9 = its CreateThread lambda). NOT a game/runtime-path bug: the dumper
  resumes each thread (agA3 deadlock fix) and then RtlVirtualUnwind walks the LIVE stack; rn9 printed every [stacks] line twice
  (two dumpers walking each other) while background compiles finished (threads exiting, stacks freed) => fault (Likely).
  Fix (commit 7b93ae6e4): unwind in a __try helper (" <fault>" appended), one dump at a time, TEB read in
  xs_game_thread_index guarded. Advice: leave LEONIDA_CE_STACKS unset in normal runs (1500+ threads on a cold boot).
- ro1 (build 77 + rn10 = 294 loop-capped modules, INNER 16384): STILL hangs (Confirm t=130.7, last frame 2352, Event 153 at
  Confirm+146 s; host then crashed executing 0x0 after device loss). Checkpoints: direct + compute queues 2/3/4 ALL report
  BOTTOM = pipeline marker cs_956fdcc6, TOP = CP_FINE before command #64 (variant 4); SYNC_DUMP: direct ticket 26503 and
  compute tickets 4 (submitted within 50 ms of each other) stalled inside their cmdbufs, 0 waits, no deadlock.
  cs_956fdcc6 has NO GCN backward branch; its SPIR-V has 4 OpLoopMerge, but they are the recompiler's structurizer state
  loops (a pc variable stored before each), not data-dependent loops. So "every hang is an uncapped loopy blur" does not
  hold for ro1 (Plausible). Candidates: a huge dispatch (DISPATCH_LOG will say), garbage data hurting a loop-free shader, or
  the stall belongs to other work in flight at that moment (every hang is ~130-150 s after Confirm, right after the first
  BVH build calls, slots 26/148/43/37/45).
- Build 78 (gta6rt-78.exe md5 f617231e, commit 839ad5a1a): stack-dumper fix + analyst patch LEONIDA_CE_CBV_SHADOW (see
  /root/handoff/analysis-fspx-cb.md): title-memory CBVs (slot 17) get a stable arena shadow refreshed before every
  ExecuteCommandLists instead of a one-time copy in the wrapping 64 MiB upload ring. Default OFF here (the patch had it on);
  =2 logs "changed after its view was made". Compiled clean.
- Commit fc41fc1c1 (build 79+): LEONIDA_CE_CBV_SHADOW_ROOT=<bytes>: root CBVs (pc 37/38) at unknown title addresses
  (passed raw before; CBV_FIX logged 0xba04e0000) get a shadow too. Off by default.
- rp1 = build 78 + rp1.env (ro1 without any LOOP_CAP + CBV_SHADOW=2 + DISPATCH_LOG=1); rp2 = the same with the ro1 caps.
- rp1 (build 78, caps OFF, CBV_SHADOW=2): Confirm t=164.2, device lost about Confirm+150 s (nvlddmkm 153 before the 15 s present
  watch, so no SYNC_DUMP), last frame 1620; then a game AV at game+0x55aa29b (after the loss: fences read UINT64_MAX). Shadow pool
  64 MiB at arena VA 0x11470000, 4216 views by Confirm, 24 "changed after its view was made" (defect 1 is real). Checkpoints only
  CP_FINE (direct before cmd #1 CopyTexture; compute queues at Buffer/TextureBarriers). Pace slower (10-25 s / 60 frames).
- rp2 (build 78, ro1 caps + CBV_SHADOW=2): the same picture (Confirm t=183, loss Confirm+130 s, frame 1650, same game AV after).
  FaultInfo: address 0x57d7c0000 = console VA 0x27d7b0000 (arena device base 0x300010000, size 4 GiB) => a garbage pointer
  OUTSIDE the arena, a page fault, not the endless blur.
- rp3 (build 79, ro1 caps, no shadow, CB_PEEK): THE BLUR CB IS OUR OWN UPLOAD-RING SNAPSHOT (Very likely):
  cs_d5613de2 / cs_d94af257 table(s4)+0xca0 V# = [00649400 00000000 00000d60 30014fac]: base 0x649400 (3424 bytes) lies INSIDE
  the 64 MiB console upload ring (VA 0x10000..0x4010000); heap mirror entry = title heap (the descriptor itself is what we wrote
  for the slot-17 CBV). Radius at +0x43c = ea84302a (-8e25) = garbage. cs_f33ac9e9 CB (s5+0x40) base 0x6a9300 = ring too.
  cs_956fdcc6 (s4+0x380, its loops are bounded by CB[0x704]/[0x72c]) V# base 0xacd3830 / 0x9ad5868, stride 0x7fc, 1 record.
  => analyst defect 2 (the ring wraps under long-lived CBVs) confirmed; LEONIDA_CE_CBV_SHADOW is the fix for this path.
  rp3 hung (frame 2246) and then CRASHED host-side in the hang watch: its stack walk (startup_trace stack_from) unwinds a
  resumed live thread (same class as rn9). Fixed in build 81 (commit 30a47ce31): walk under SEH ("<walk faulted>").
- cs_956fdcc6 does have 3 backward SCALAR branches (lines 1693/1751/1802, offsets 65495/65485/65512); the loopy list missed
  them. Full list from the main session: /root/handoff/loopy_fspx_cs_all.txt (1826). LOOP_CAP=fspx_game (rn9/rp4) covers all.
- Builds: 79 (f6a950a8b, CB_PEEK), 80 (184817dc9, CB_PEEK prints title bytes at the base too), 81 (30a47ce31, hang-watch SEH).
- CB_PEEK targets that matter: d5613de2:4:0xca0:0x43c, d94af257:4:0xca0:0x43c, f33ac9e9:5:0x40:0x24, 956fdcc6:4:0x380:0x704
  (needs LEONIDA_CE_PTR_CHECK=1 + PTR_CHECK_ONLY on the same shaders to keep the scan cheap).
- rp5 (build 80, caps OFF, CBV_SHADOW=2, CB_PEEK): THE SHADOW FIXES THE BLUR CB (Very likely). cs_d5613de2 / cs_d94af257 V# =
  [1158e300 00000000 00000d60 30014fac] (base in the shadow pool at 0x11470000+), radius +0x43c = 0.0, scale 2.7e-12 (was -8e25
  from the clobbered ring copy); cs_f33ac9e9 CB in the pool too (+0x24 = 0x400, 0x579). Confirm t=184.5, still device lost
  (last frame 1800) with the rp1/rp2 signature (CP_FINE direct before cmd #1 CopyTexture, compute at barriers), then a game AV
  (game+0x4dad047) after the loss. So the endless-blur hang is solved without caps; the remaining loss is a different bug.
- Next blocker (Plausible, main-session analysis of rp2 FaultInfo): a GPU pointer into the title TEXTURE POOL CPU window
  (reservation 0x1800e0000.., slot 0xFD6D; GPU mirror window 0x7800e0000 = [0x149e1dad0]); neither window has a GPU mapping in
  the 4 GiB arena. Fix idea: a pool GPU buffer (sparse-bound to the pool pages or a refreshed copy) + rebase pool-window V#s
  in the heap-mirror pass. Handed rp6 = build 81 + rp6.env (rp5 + PTR_CHECK=2 on every dispatch) to census pool-cpu/pool-gpu
  bases (ce_pc_class already classifies them).
- Recommended env (agA23): build 81 + rp5.env minus PTR_CHECK/CB_PEEK, i.e. /root/handoff/rp1.env (caps OFF + CBV_SHADOW=2;
  CBV_SHADOW=1 once the change log is not needed). LOOP_CAP is no longer needed for the blur shaders. Leave LEONIDA_CE_STACKS unset.

## agA24 (sender gen 24, 2026-11-25)
- Receiver ada4c52511e1337a1 (runs rqN). Builds 82+.
- rp6 census (build 81, PTR_CHECK=2 on every dispatch; receiver): 19 pool lines (11 pool-cpu, 8 pool-gpu), 291 host-committed,
  88 other, 2 page-pool. Real ones are table V#s in cs_87dac0f8 / cs_25d19264 / cs_1bc231e4 (s5 slots 16-30): GPU-window base
  0xbc3960000 (64 KiB-aligned, a slot base), CPU-window 0x234453ed0 / 0x2f7bff390; the same values also sit as raw qwords in
  buffers (so descriptor rewriting alone could never cover them). Qwords like 0x900000008 / 0x400000008 are false positives.
  Older ri46-ri59 logs had 60 pool-cpu / 119 pool-gpu lines in the same three shaders. => buffer V#s AND raw pointers into both
  windows (Very likely). rp6 lost the device at Confirm+165 s on a READ_INVALID at device 0x7ae218cf000 (~7.7 TiB: not pool,
  garbage) in cs_edc5d4c7; rp2 had a WRITE_INVALID (type 2) in the pool CPU window. So the pool is ONE of the bad-address sources.
- Options checked on the local RTX 6000 (Linux driver, test programs in /root/agA24): sparse buffers take ONLY device-local memory
  (memoryTypeBits 0x2), so neither the host-visible arena nor imported host pages can be sparse-bound => "make the arena sparse"
  and "sparse-bind the host pool pages" are impossible on NVIDIA. But bufferDeviceAddressCaptureReplay with an arbitrary opaque
  address places a sparse buffer EXACTLY where asked (0x2ae6e0000 wanted = got): a buffer at heap_base + 0x1800e0000 makes
  heap_base + VA valid for every pool VA with no rewriting.
- Build 82 (gta6rt-82.exe md5 05936516, commit 0fd9c7c41): LEONIDA_CE_POOL_GPU=1|2 (default off; LO/END/CHUNK overridable):
  - sparse window buffer [0x1800e0000, 0xd800e0000) at heap_base + LO (capture replay enabled on the device only with the switch;
    refuses with "POOL_GPU: OFF: <why>" if the driver places it elsewhere);
  - =1: unbound (reads 0, writes dropped): fault-proof only;
  - =2: xmem page hook (page_alias.h page_map_hook / for_each_mapped_hook, called from map_backing / unmap_page) queues every
    window page map; at ExecuteCommandLists (ce::pool_sync_fn) each is sparse-bound to a device-local copy of its physical page
    (64 MiB device chunks keyed by the xmem section scratch view, so CPU and GPU slots of one page alias), and the page host bytes
    are copied in (imported host chunk -> window buffer) at each map event; CPU waits for bind+copy. GPU writes are not copied back.
  - Wine sq2 (local GPU, rp1 env + POOL_GPU=2): placement OK, 6720 pages bound in the first flush, 0 failures, boot state 3,
    768 MiB device-local after ~12k binds / 11k unbinds.
- Env note: VA_GUARD_WINDOW=0x100000000 (rp1) redirects pool VAs in the 81 guarded shaders; with POOL_GPU use 0xd800e0000.
- Handed rq1 = build 82 + rq1.env (rp1 + POOL_GPU=2 + VA_GUARD_WINDOW=0xd800e0000 + CB_PEEK on cs_edc5d4c7 CB+0/8/0x30/0x40,
  main-session request: that shader takes a V# embedded in its CB and a bindless index CB[0x40]) and rq2 = rq1 with POOL_GPU=1.
- rq1 (build 82 + rq1.env, receiver B16): POOL_GPU never switched on ("POOL_GPU: OFF: vkCreateBuffer(sparse at heap_base + LO)
  failed (wanted 0x4800f0000, got 0x0)"): the Windows driver refused the 48 GiB sparse buffer at that address (Linux places it).
  Device still lost at Confirm+~150 s (FaultInfo -13, CP_FINE checkpoints as rp1/rp2/rp5). cs_edc5d4c7 was never dispatched
  (no CB_PEEK/PTR_CHECK lines). So rq1 tested nothing new.
- Build 83 (eb70d393a, md5 5f2aed66; never run): POOL_GPU=2 also re-copies a GPU page copy after dstorage writes the host page
  (xmem_write_page / write_phys -> page_dirty_hook).
- Build 84 (779e1be1a, md5 3b394faa): the window buffer is tried whole, then in 8 GiB / 4 GiB / 1 GiB / 256 MiB pieces
  (LEONIDA_CE_POOL_GPU_PIECES), each failure logged with VkResult and address, plus an unplaced full-size sparse probe. Handed
  rq2 = build 84 + rq2.env (= rq1.env, rtdump_rq2) to receiver B18 (af5ee748c55a71403).
- Note on the rp6 "pool V#s": the 16-byte records are {u64 pool pointer, u64 HOST pointer 0x7ffc....} (e.g.
  [c3960000 0000000b 8082285b 00007ffc]) - raw 64-bit pointers, not V#s. Anything that rewrites them must treat them as qwords.
- Fallback if Windows refuses every placement (not built): put the window buffer wherever the driver wants (D, must be above
  heap_base) and add D - heap_base - LO to pool-window qwords/V# bases in the heap-mirror copy (and CBV shadow copies). That cannot
  reach raw pointers inside buffers.
- Identity placement also works for IMPORTED HOST memory on Linux (/root/agA24/cr2.c: VkMemoryOpaqueCaptureAddressAllocateInfo on an
  import + a plain buffer = device address exactly heap_base + title VA). Next step candidate: place every slot-133 title-placed
  buffer (create_host_buffer, VAs 0xd9..-0xe3.. and 0x1000000000+) at heap_base + its title VA, which would end the
  "two address schemes" problem (agA20) for them. Only if capture replay works on the VM driver (rq2 tells).
- rq2 (build 84 + rq2.env, receiver B18): POOL_GPU OFF. Every placed piece (48 GiB down to 256 MiB) failed with
  VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: the Windows driver only accepts capture addresses it handed out (the Linux driver takes
  any free one). The unplaced probe landed at 0x3ffe10000 = heap_base + 0xffe00000, right after the arena. Loss at Confirm+~149 s
  (FaultInfo -13), CP_FINE checkpoints as rp1/rp5, last frame 1440. Serves as the no-pool baseline.
- Build 85 (365f3e31e, md5 8c4597aa): ADJACENT window buffer: one UNPLACED sparse buffer of END - arena size + 1 GiB; if it starts at
  heap_base + X <= LO and reaches END, VA v = buffer offset v - X (identity, no capture replay, no rewriting). Then placed pieces
  (Linux), then a REBASE fallback (heap-mirror qwords in [LO, END) shifted).
- rq3 (build 85 + rq3.env = rp1 + POOL_GPU=2 + VA_GUARD_WINDOW=0xd800e0000 + edc5d4c7 CB_PEEK): POOL_GPU ON on the VM
  ("adjacent sparse buffer of 0xcc02e0000 bytes -> 0 at 0x3ffe10000 = heap_base + 0xffe00000: COVERS the window"). Flushes ok until
  the hang: 115k binds / 33k unbinds, 80 chunks = 5.6 GiB device-local, 35k copy regions, 21k dstorage refreshes, 0 chunk failures.
  Confirm t=194.4; presents stop at frame 1548 (pace 22-45 s / 60 frames after Confirm, slower than rq2: every ExecuteCommandLists
  with pending binds waits on the CPU for bind+copy on the direct VkQueue). SYNC_DUMP: direct ticket stalled INSIDE its cmdbuf, 0
  waits (the old picture); the pool flush waits then time out ("FAILED at wait/bind": consequence). TDR ev153 ~Confirm+200 s, DEVICE
  LOST seen Confirm+262 s. FaultInfo: IP 0x20b1415e0 + READ_INVALID 0xfea1c325000 (~16 TiB: garbage, NOT pool). Checkpoints
  compute cmd #57 / #19 (v4).
  => The pool windows are now GPU-addressable on the real target (Definite for placement, Very likely for contents), but the
  post-Confirm hang/loss is NOT caused by the missing pool mapping (Likely, 1 run each with/without). The rp2 pool-window
  WRITE_INVALID was one more garbage-address symptom, not the root cause.
- edc5d4c7 (rq3 CB_PEEK, first dispatch near the hang): its CB V# (s5+0x40) = [11c89200 0 60 30014fac] (a CBV_SHADOW copy), and
  CB+0 = 00008008 0002c804 000ac404 00075804 = EXACTLY the 16 bytes in the same table's slot 3 (s5+0x60, heap mirror = title heap).
  The shader takes CB+0 as a V# (s_buffer_load_dwordx4 ... buffer_load_format_x idxen), so its base is 0xc80400008008 -> heap_base +
  that ~= 220 TiB: a far READ like the FaultInfo one (Plausible link). Slot 4 = all ones, slot 5 = small ints: this table area holds
  16-bit data, not descriptors. So either the CBV that the shadow copies points at the wrong title bytes (the CB is the descriptor
  table itself?), or the title fills those slots later on the GPU. Next: find which title address the shadowed CBV came from
  (CBV_SHADOW source VA for shadow 0x11c89200) and whether it overlaps the descriptor heap; Ghidra who writes the s5 table slots 3-5.
- Recommended env (agA24): build 85 + rp1.env (POOL_GPU off) behaves like build 81. POOL_GPU=2 (rq3.env) is correct but slower and
  does not fix the loss; keep it for work on the pool / ray-tracing data paths. Identity placement of imported host buffers via capture
  replay is NOT possible on the Windows driver (same refusal); the ADJACENT trick only works for the first big buffer after the arena.

## agA25 (sender gen 25, 2026-11-25)
- Receiver: the main session (runs rsN on the VM). Builds 86+. Build from Linux: PATH=/usr/lib/llvm-21/bin:$PATH cmake --build
  runtime/build/windows --target gta6rt (llvm-lib must be on PATH), then make_game.py --stages runtime to package.
- A24's lead was a misread (Very likely, from the disasm): cs_edc5d4c7 CB+0 is NOT a V#. The dwordx4 at CB+0 is unpacked as
  scalars (s16 & 0xff = stride, s16 >> 8 = offset; same for s19): 00008008 = stride 8 / offset 0x80, i.e. a vertex-layout record,
  valid data. Its V#s come from the BINDLESS heap table s6 at CB[0x40]*32 and CB[0x44]*32 (s_load_dwordx4 s[12:13], idx<<5).
- Why VA_GUARD (which listed cs_edc5d4c7) never caught the far reads (Definite, offline count): spirv_guard_va rewrites only
  OpConvertUToPtr(heap_base + va). The translator forms per-component accesses as (heap_base + va) + k, a second add, so in
  edc5d4c7 only 50 of 108 pointers were guarded (the other 58 = every 2nd..4th dword of a store/load).
- Build 86 (eb439aae, d5cbb92c7): CB_PEEK appends "| SRC shadow .. = title .. INSIDE/not in title heap ..; CBtitle-entry = N;
  title now [..] shadow [..]" (CBV_SHADOW source of a CB, via leonida::ce::cbv_title_fn). LEONIDA_CE_CB_VSHARP_GUARD (zero a CB-
  loaded V#) - WRONG premise for edc5d4c7 (it zeroes the layout record); keep it off. rs1 = build 86 + rs1.env.
- Build 87 (ad21754c, 08f8d60f6): LEONIDA_CE_BINDLESS_GUARD=name:cbsgpr:cbvoff:idxoff:heapsgpr (needs PTR_CHECK>=1): leave the
  dispatch out (ce_bad) when the CB-indexed bindless heap entry is outside the arena or its V# base is unmapped. PtrCheckFn now
  returns bool (skip). rs2 = build 87 + rs2.env (edc5d4c7:5:0x40:0x40:6,edc5d4c7:5:0x40:0x44:6).
- Build 88 (8db0756f, f5f6c6c43): LEONIDA_CE_VSHARP_GUARD_ALL=1 - blanket pass (src/spirv_va_guard.h spirv_guard_ptr_all) on
  EVERY module, compute and graphics: each u64 OpConvertUToPtr operand p is checked, p >= heap_base + LIMIT -> redirected.
  heap_base = the OpBitwiseOr operand voted from adds feeding ConvertUToPtr (module left alone if none). VA_GUARD list ignored
  when on. (88 also tripped p < heap_base and touched bvh/: superseded by 89.)
- Build 89 (4f54745d, 9b6c2165c): VSHARP_GUARD_ALL final form:
  - LIMIT = LEONIDA_CE_VSHARP_GUARD_LIMIT (default 1 TiB above heap_base); pointers below heap_base pass; bvh/* excluded.
  - Tripped pointers go to a sink at arena VA 0x10000 (512 KiB); with the switch the arena allocator starts at 0x100000.
  - Compute modules record trips (selection branch, not in loop-header blocks): 16 B per module at arena 0x90000 + slot*16
    = atomic count, bad pointer lo, hi. Thread (5 s): "[warning] <shader>: overly big address 0x.. ignored (count N)", first
    time and each doubling.
  - Offline: 123 modules (62 cs, 60 gfx, edc5d4c7), 6650/6650 pointers, spirv-val vulkan1.3 clean. Wine 300 s (local GPU):
    ~16k modules / 86k pointers guarded, no pipeline failures, state 3 + presses, 0 warnings.
  - Cost: every module changes -> driver pipeline cache misses on the first run with the switch.

## agA26 (sender gen 26, 2026-11-26)
- Aftermath rs8/rs9: the device loss ~130-200 s after A is a COPY ENGINE write page fault (rs8 heap_base + 0x26EC1E000, pool
  window unmapped; rs9 with POOL_GPU=2 heap_base + 0xFB6A00000, outside every reservation). Build 90 adds copy logging.
- LEONIDA_CE_COPY_LOG=1 (default off; src/ce_copy_log.h): `[copylog]` lines. VK-FIRST / REC-FIRST once per (kind, site,
  title caller); VK-OUTSIDE / REC-OUTSIDE / VA-OUTSIDE for every copy outside its VkBuffer (size from a wrapped
  vkCreateBuffer), D3D12 buffer resource (GetDesc().Width), or past LEONIDA_CE_COPY_VA_LIMIT (default 0xE52E00000).
  LEONIDA_CE_COPY_GUARD=1 (implies the log) skips a copy whose DESTINATION is outside (VK-SKIP / REC-SKIP / VA-SKIP).
- Copy sites covered:
  - Vulkan level (wrapped in Functions::load, so every user of ctx->vk): vkCmdCopyBuffer, CopyBufferToImage
    (source offset only), CopyImageToBuffer (destination offset), CopyImage (first-seen only), FillBuffer, UpdateBuffer,
    CopyQueryPoolResults. The replay sets a site (replay.CopyBuffer / CopyTexture / CopyResource / WriteImmediate /
    AtomicCopyBuffer), POOL_GPU sets pool.flush; anything else logs site=?. Each line has the queue family and the
    D3D12 resource ids.
  - Record level (d3d12_com.cpp): CopyBufferRegion, CopyTextureRegion (footprint side), CopyResource, CopyTiles (linear
    side). The title caller comes from the console list proxy (list_slot, _ReturnAddress()) through a thread-local. The
    replay looks up the last caller that copied that resource id.
  - xdxr_copy (CopyRaytracingAccelerationStructure, title VA, compute not CE): VA check.
  - NOT covered: vulkan_bindless.h table updates (own function table; runtime-owned buffers), vulkan_predicate.h /
    vulkan_indirect_dispatch_count.h scratch (fn<> lookups), present blits. DirectStorage is a CPU memcpy (no GPU path).
    UpdateTileMappings / POOL binds are vkQueueBindSparse (no copy).
- Build 90 = build 89 + the above. No shader change.

## agA27 (bcpack-rs integration, 2026-11-26)
- Upstream: https://github.com/Ahmet-Dedeler/bcpack-rs commit f5145dda7fa6d1a22b499d82f3e63029713b71b6 (v0.2.0), cloned to
  /root/vendor/bcpack-rs on cs3, NOT vendored here. License check: Cargo.toml `license = "MIT OR Apache-2.0"`, LICENSE-MIT and
  LICENSE-APACHE present; src/tables.rs + rans.rs port Microsoft silicon-rans (MIT, THIRD_PARTY_NOTICES.md). Both are
  GPLv3-compatible (Definite). Credit bcpack-rs if any of its code is ever copied in (its headers ask for that).
- Correction to the brief: shims/bcpack_decode is not "BC1 only"; it tries BC1/3/4/5/6H/7 in strict mode, but BC7 (and BC6H)
  refuse any chunk that references the built-in endpoint history/dictionary, which our tables do not have. Every comp=14
  chunk in rs1-rs11 was refused for that reason (mode 7 "unknown built-in endpoint history entry N" / "signature rank N is
  inferred-only"). bcpack-rs ships the 255-entry BC7 dictionary and the 285 control codes, so it decodes them.
- API mapping: a DirectStorage comp=14 request is one RDS5 chunk: file offset/size = the chunk's stored bytes minus the pad
  (rs11's 8 logged requests: size = chunk file_len - pad exactly), dst_size = the chunk's virtual length (65536). No
  format: opts 0x0020000e has no codec bits; the format lives in the store's chunk table ((a >> 24) & 0x3C) which the
  request does not carry. bcpack-rs needs (chunk, mode, unpacked_len): bc1::decode(chunk, blocks=dst/8, window, quads),
  bcn::decode(chunk, dst, tables[BC3/4/5]), bc7::decode(chunk, dst, tables). Every lane emits dst/8 symbols; a wrong mode
  fails at the rANS layer (lane end states + exact byte consumption + zero pad), so auto detection works like ours.
- Wrapper: /root/vendor/bcpack-ffi (cdylib, path dep, default-features off, static CRT). Exports
  bcpack_decode(src, src_len, dst, dst_len, format_hint) -> mode (1,3,4,5,7) or <0 (-1 args, -2 no mode, -3 unsupported,
  -4 panic caught, -5 ambiguous, -6 size), bcpack_decode_ex(..., BcpackInfo*) and bcpack_ffi_version() = 0x10000.
  format_hint 0 = auto (order BC7, BC1, BC5, BC4, BC3), 0x100 = try all modes and refuse if >1 validates (tests).
  BC6H: entropy layer only upstream, refused.
- Runtime (dstorage_x.cpp): LEONIDA_CE_BCPACK_RS=1 (default 0): LoadLibraryExW(<exe dir>\bcpack_ffi.dll) once; a comp=14
  chunk our decoder refuses goes to bcpack_decode_ex (auto); =2 skips our decoder (avoids its internal exceptions).
  Logs: "[dstorage] bcpack-rs: loaded ..." / "LoadLibrary(...) failed", "comp=14 bcpack-rs decoded BC7 #k", "bcpack-rs
  failed #k (rc ..)", and "comp=14 summary after N: ours decoded/refused | bcpack-rs decoded (per BC mode) failed (per
  reason)" at 1, 2, 4, ... and every 256th comp=14 request. Failure still zero-fills as before.
- Offline test on cs3 (bctest harness in /root/vendor/bcpack-ffi, game files read in place):
  - rs11's 8 logged comp=14 requests: all 8 are BC7 chunks (store truth), bcpack-rs decodes 8/8 with the right mode,
    unambiguous (only BC7 validates), 0 unknown controls; ours refuses 8/8.
  - 7 stores (graphics full, 6 others every 15th chunk): mode detection right on 100% (BC1 1160, BC3 1, BC5 105, BC7 87
    chunks), 0 wrong, 0 ambiguous. Ours: BC1 1157/1160, all BC3/BC5/BC7 refused.
  - BC1 ours vs bcpack-rs: endpoints identical everywhere; 0.4% of blocks (93,903 / 23.9 M) differ in ONE index-word
    quadrant, always a left/right mirror of a 2x2 quadrant code = the ~8 codebook entries bcpack-rs itself calls uncertain.
    Which side is right: unknown (Trust me bro). BC1 stays on our decoder (RS=1 only gets what ours refuses).
  - Windows DLL under Wine (dlltest.exe, CPU only): same output hash as the Linux build; null/garbage input -> -1/-2.
  - Speed: BC7 64 KiB chunk ~10-20 ms in the check-all test mode, single core; fine for 4 workers.
- Build 92 = 91b (1834dbd4f) + the above. Handed: gta6rt-92.exe + bcpack_ffi.dll + rs14.env (= rs11.env +
  LEONIDA_CE_BCPACK_RS=1). Not booted here (no GPU runs on cs3; Wine has no comp=14 before state 1).
- Caveat: decoded output is in stream (Gfx10 tiled) order, the same as our decoder's; whatever consumes the slot pool
  expects what the console MDU writes (Likely the same order). Textures decoded at last, so watch for new GPU faults.

## agA28 (sender gen 28, 2026-11-26 overnight)
- Receiver B20 (a4598b35c771d8b72), runs rtN. Builds 94+.
- Fault VA pattern (Aftermath, all Copy Engine WRITE "failed to translate"): rs8 0x56EC2E000 (no POOL_GPU), rs10 0x12B69B0000 /
  rs13 0x12A6D00000 (POOL_GPU=2: +~0xCC0000000 = the pool sparse buffer size, which sits right after the arena at 0x3ffe10000),
  rs17 0x142BB20000 (NO_FREE=2: leaked memory pushes it higher). So the faulting VA tracks the frontier of driver VA handed out
  at runtime: something allocated late and then gone (Likely), not a fixed title address.
- rs17 (build 93, NO_FREE=2 = never vkFreeMemory): still a CE write fault ~178 s after 13->0 => freed VkDeviceMemory is not the
  cause (Likely).
- Build 94 (018f71249): LEONIDA_CE_VA_REPORT=<file> (src/ce_va_report.h; VK_EXT_device_address_binding_report through a
  debug-utils messenger: every GPU VA bind/unbind incl. driver-internal, WATCH range to stderr, lookups at device loss) and
  LEONIDA_CE_COPY_ON_DIRECT=1 (COPY queues on graphics-family VkQueues: no Copy Engine for title copies). Both default off.
  /root/a28/valookup.py <va.txt> <hex va>.. does the lookup offline.
- Build 95 (cb0a3b50e): FENCE LIFETIME BUG. VulkanQueue::signal pinned the D3D12 fence only when the Signal was held; an unheld
  Signal was submitted with nothing keeping the fence alive (waits were pinned to their ticket; signals were not). A fence
  released right after queue->Signal destroyed its VkSemaphore under a pending GPU signal (VUID-vkDestroySemaphore-semaphore-
  05149); on a COPY queue the Copy Engine writes the payload into freed driver memory = a CE write to an untranslatable VA in
  freshly allocated driver VA space (fits every rs8-rs17 observation, NO_FREE included; Plausible until a run says).
  Fix: the unheld path retires the pin with the submission ticket. Default ON (LEONIDA_CE_FENCE_PIN=0 = old). Logs FENCE_PIN
  (a pin dropped the last reference of a fence = the title released it with work in flight) and FENCE_EARLY (destroyed with a
  queued signal still pending).
- rt1 (build 94 + VA_REPORT): GPU HANG 252 s after 13->0 (Aftermath "Hung", Executing marker = cs_d80e80ca on the direct
  queue, bottom cs_d5613de2 = the agA22 blur pair). VA_REPORT works: obj types 8 memory / 9 buffer / 10 image; NVIDIA reports
  no driver-internal (i) binds; images are never unbound; ~27k memory binds / 7.6k memory unbinds per run.
- rt3 (build 95, fence pin): FENCE_PIN 128+ (the title DOES release fences with signals in flight: a real bug, fixed), but
  still the CE write fault: VA 0x12be820000 == EXACTLY the end of a live 15 MiB image mapping, next bind after a 64 KiB gap.
- rt2 INVALID (COPY_ON_DIRECT compared cv[0] with 1, not '1': my ssh-quoted edit ate the quotes; fixed in 96). It hung ~261 s.
- Build 96 (467ee2fd5): LEONIDA_CE_IMG_CHECK=1|2 (src/ce_image_check.h) - image registry, region bounds checks (mip, layers,
  extent) on CopyBufferToImage / ImageToBuffer / CopyImage / ResolveImage / Clear*, =2 drops misses; IMG lines in the VA file.
- rt4 (build 96, IMG_CHECK=2): 0 region misses, CE fault again at the end of a 15 MiB image (0x12b6220000). VA file names it:
  VK_FORMAT_D32_SFLOAT 2560x1440, 1 mip, MUTABLE_FORMAT, usage TRANSFER_SRC|DST|SAMPLED|DEPTH_STENCIL_ATTACHMENT. Its dedicated
  VkDeviceMemory (0xF00000) is bound at one VA and the IMAGE has its own second VA mapping right after it (NVIDIA depth page
  kind; colour images share their memory VA). The fault is at the end of the depth image mapping (Definite).
- Build 97 (4f5cdced9): IMG_CHECK also checks aspects, BIND-SHORT (requirements.size vs allocation at vkBindImageMemory) and
  DS-ON-TRANSFER (DEPTH/STENCIL aspect copies on the transfer-only family; Vulkan forbids them for buffer<->image copies,
  VUID-vkCmdCopyBufferToImage-commandBuffer-07739).
- rt5 (build 96 + COPY_ON_DIRECT=1, really on): NO CE fault; the run lived to 252 s after 13->0 (every CE-fault run died at
  178-197 s) and then hung like rt1/rt2 (SYNC_DUMP: direct + 2 compute queues stalled inside cmdbufs, all waits satisfied).
- ROOT CAUSE of the CE fault (Likely): D3D12 COPY queues ran on NVIDIA's transfer-only family; depth copies there are invalid
  Vulkan and the Copy Engine overruns the depth image's mapping. Build 98 (00cfc0615) = THE FIX: COPY queues go to the
  graphics family when the copy family has no GRAPHICS bit (default on; LEONIDA_CE_COPY_ON_DIRECT=0 = old). Cost: no async
  copy engine for title copies (perf only).
- Remaining wall: the ~250 s GPU hang (rt1/rt2/rt5). rt7 = build 98 + LOOP_CAP on cs_d5613de2,cs_d94af257,cs_f33ac9e9.
- CORRECTION (rt9): build 98 is NOT the fix. rt9 (build 99, COPY queues on the graphics family) lost the device ~120 s after
  13->0 to the same CE WRITE "failed to translate" fault (0x123BA20000). rt5 surviving the window was luck/timing (1 run).
  NVIDIA evidently runs some graphics-queue copies on a Copy Engine too. Build 98's routing stays (it is still correct
  Vulkan: DEPTH/STENCIL buffer<->image copies are not allowed on a family without GRAPHICS) but does not stop the loss.
- rt6 / rt8 died on the CPU ~25 s after 13->0 at host 0x7ff85eee2023 (CRT memcpy; the title itself passes 0x7ff85eee27c2 as
  an argument, so a host CRT module, not the driver) reading title memory 0xbf46../0xbf56.. right after "[refresh] re-upload"
  of placed textures nearby: xs_readable() then the title decommits the pages (TOCTOU). Build 99 (91b8882ca): the refresh hash
  and the upload detile run in __try frames ("[hack] TITLE_READ_FAULT"). rt9: no early AV (0 TITLE_READ_FAULT lines).
- rt7 (build 98 + LOOP_CAP on the 3 blur shaders, POOL_GPU=2): CE READ of VA 0 (device_fault agrees, precision 0x1000),
  ~150 s after 13->0. rt8 (POOL_GPU=1) died early on the CPU AV above. Open.
- Build 100 (c77932698): IMG_CHECK logs every copy/clear touching a DEPTH/STENCIL aspect on any queue ("DS-COPY").
- rt10 (build 100, DS-COPY log + VA report): CE WRITE at 0x123B950000 = end of image 0x22e83fb0e60 (D32_SFLOAT 2560x1440).
  The ONLY D32S8 -> D32 depth copy of the run: t=302.381 vkCmdCopyImage src fmt 130 (D32_SFLOAT_S8_UINT 2560x1440) aspect
  DEPTH -> dst that image; device lost at t=302.444. ROOT CAUSE of the CE fault (Very likely): a depth copy between DIFFERENT
  depth formats (Vulkan allows depth/stencil vkCmdCopyImage only between identical formats, VUID-vkCmdCopyImage-srcImage-01548);
  NVIDIA lays D32S8 and D32 out differently and the Copy Engine overruns the D32 image's own VA mapping. It is a one-off ~120-200 s
  after the landing (the first gameplay frames), which is why the loss time varied run to run.
- Build 101 (079995e58): LEONIDA_CE_UPLOAD_DS (placed depth/stencil textures not uploaded from title memory; default skip, =1
  old). Not needed for the fault; untested (rt11+ ran with =1).
- Build 102 (e06a90e71) = THE CE FIX: depth/stencil copies between different formats take the existing depth<->colour staging
  path (image -> copy scratch -> image). LEONIDA_CE_DS_XFMT_STAGE (default on, =0 old). "[hack] DS_XFMT_STAGE ..." log.
- rt11 (build 102): early CPU AV again (0x7ff85eee2023, read 0xbf469f800; build 99 guard not on that path). 0xbf4.. is inside the
  texture-pool CPU window [0x1800e0000, 0xd800e0000). Build 103 (8e6ae7fd9): the crash report prints the faulting thread stack.
- rt12 (build 103): DS_XFMT_STAGE fired (fmt 130 -> 126, 2x); NO CE fault; the device lived to t=321.6 (137 s after 13->0, past
  rt10's loss point). NEW loss: Graphics / ROP READ page fault 48 KiB past an R16_SFLOAT 640x360 colour target (bound 0x78000,
  fault +0x8C000, non-mutable image). Theory (Plausible): attachment views use the PSO's RTV/DSV format, not the image's;
  a bigger texel size makes the ROP address a bigger surface.
- Build 104 (622848832): RT_FMT_GUARD (draws whose PSO attachment format differs in texel size / depth aspects from the image
  are left out; default on, =0 old) + draw scissor clamped to the render area. rt13 = its test.

## agA29 (sender gen 29, 2026-11-26 overnight)
- Receiver = the main session. Builds 105+.
- rt13 (build 104) INCONCLUSIVE: boot state 0->1 took 317 s (55 s in rt12); the pipeline cache was warm (loaded exactly what rt12
  saved, +10 MB as usual), but background compute compiles averaged 7.6 s (0.5 s in rt12; 428 over 2 s vs 50-67). Not our code:
  builds 102-104 touch no shader/pipeline code, and the scissor clamp is dynamic state. rt14 (same build) booted normally.
- rt14 (build 104, 900 s): RT_FMT_GUARD 2048+ draws left out: colour slot 0 image fmt 97 (R16G16B16A16_SFLOAT, 2560x1440 and
  128x128) / 101 (R32G32_UINT) / 16 (R8G8_UNORM) viewed as PSO fmt 37 (R8G8B8A8_UNORM). No fmt 76 640x360 line, no ROP fault,
  no CE fault (DS_XFMT_STAGE 5x). HANG at frame 2183, device lost t=394.8 (227 s after 13->0), Aftermath "Reset", 14 warps at one
  PC; direct checkpoints top cs_d9ea3225 / bottom cs_00f50ab8. cs_d9ea3225 is loop-free (no backward branch / barrier / atomic in
  RDNA, 0 OpLoopMerge in SPIR-V; plain buffer loads/stores) => memory/corruption, not a spin (Likely).
- Build 105 (072e4fb0e): RT_RETARGET - instead of skipping, a colour slot whose texel size differs from the PSO RTV format is
  viewed in the IMAGE format with a pipeline variant built for that format (GraphicsPipeline::retarget; variants keyed by
  (unbound mask, format overrides), background-built). Same numeric class only; depth mismatches still skipped. =0 old.
  Rationale: on the console the CB format comes from the RTV (resource), the PSO only sets the export format (Plausible).
- GUARD GAP (Plausible as the hang cause): VSHARP_GUARD_ALL sinks pointers >= heap_base + 0x1000000000; the POOL_GPU=1 adjacent
  sparse buffer (unbound: reads 0, writes dropped) covers heap_base + [0xffe00000, 0xdc00e0000). VA [0xdc00e0000, 0x1000000000)
  is raw driver VA: rt14 had 9397 live memory/image ranges there (rt12's ROP-fault R16F target 0x120f140000 = VA 0xf0f130000
  is inside it), and the title's placed buffers' raw VAs 0xd9..-0xe3.. land in it. Env-only fix: LEONIDA_CE_POOL_GPU_END=
  0x1000000000 grows the sparse buffer to reach the guard limit (no shader change). rt16 tests it.
- rt15 (build 105): RT_RETARGET works (222 retargeted, 0 variant failures; fmt 101 R32G32_UINT vs 37 still left out, class
  mismatch). HANG again at frame 2348, loss t=394.6 (189 s after 13->0), checkpoint cs_edc5d4c7 (loop-free too), Aftermath
  "compute_01" size 215296 @0x4e80 = the same shader/PC as rs11.
- rt16 (build 105 + POOL_GPU_END=0x1000000000): cover took (0xf40200000 bytes, COVERS); STILL hangs (frame 2222, pace collapse,
  checkpoints compute cs_c5b1005e / cs_344692c2 / cs_752cc8dc). The guard gap is not the hang cause (Likely); keep or drop.
- ROOT CAUSE CANDIDATE (Likely): every hang dump rt14/rt15/rt16 has an Aftermath "Faulted Warps: Misaligned Address Error"
  just before the parked warps (PC 0xcac0/0xcb00, 0x4de0/0x4e80, 0x51d0/0x5200). The translator emits u32 "Aligned 4" PSB
  accesses at heap_base + VA + k; RDNA scalar loads drop the 2 low bits and buffer ops run unaligned, NVIDIA traps and the
  trapped warp hangs the queue. Guard warnings show non-4-aligned bases (0x...63e, 0x...6be).
- Build 106 (a5f101c67): LEONIDA_CE_PTR_ALIGN=1 (default off, compute only): VSHARP_GUARD_ALL ANDs each guarded pointer with
  ~(pointee scalar size - 1). spirv-val clean (400 modules + hang shaders). Offline harness /root/a29/gt.cpp. rt17 tests it.
- rt17 (build 106 + PTR_ALIGN=1, 1200 s): cold compute recompile (1594 compiles, avg 20.6 s; 13->0 at t=536.9). NO device
  loss before the cap; post-press frames before the stall: rt14 563, rt15 488, rt16 479, rt17 1461 (~630 s after the press).
  At the very end the direct queue's first ticket sat 31 s "stalled INSIDE its command buffer" = the hang signature again,
  cut by the cap before TDR (no Aftermath). => PTR_ALIGN fixes the compute misaligned traps (Likely); a later hang remains,
  maybe a GRAPHICS module (not aligned) -> build 107 LEONIDA_CE_PTR_ALIGN_GFX=1 (recompiles every graphics pipeline).
- Pace: 2-3 s/60 frames before the press, 11-20 s/60 after (transition, not a leak). The arena does leak: 0 releases ever,
  live 1333 -> 2096 MiB, largest free block 2753 -> 1892 MiB over ~660 s of gameplay (placed resources never give back arena
  space; caller +0x84612 66k buffers / 911 MiB). That fills the 4 GiB arena in roughly 20-30 more minutes: a future blocker.
- The GPU box hard-reset at ~22:13 (no Xid/panic; not from a run or build).
- Build 107 (84b8eca4e): LEONIDA_CE_PTR_ALIGN_GFX=1 (with PTR_ALIGN=1) aligns graphics modules too. Default off.
- rt17 frames (/root/rb/rt17_frames): frame_004096 and frame_006000 (~540 frames after the A) are flat navy #20202C: no new
  screen yet (same as since agA8).
- rt18 (build 106, rt17 env, after the host reset): NOT A TEST. All 6 A presses fired at s3+8.., but the title's reading
  count stayed at 611 during the whole press window (landing ran 6.9 s/60 frames, compiles avg 8.2 s), so no press
  registered and 3->13 never came; frame_010000 = the landing UI (ACCOUNT PICKER / PLAYER). Fix: more presses
  (rt19.env: A@s3+8/10x40).
- rt19 (build 106 + PTR_ALIGN, A@s3+8/10x40): A registered (13->0 t=550.6); lived ~860 s after the press (longest yet).
  Loss = PAGE FAULT (not a hang): compute READ, Faulted Warp "MMU Fault Error" in cs_bedf0201 (image_load_mip +
  image_store), VA 0x1796b56000 = +0x26000 from a live 256x256 BC3 9-mip image (0x15e00 bytes, mapped 0x20000; the next
  64 KiB is a hole). Aftermath named a destroyed 5.4 MiB staging buffer only because it was the last thing mapped there.
  Cause (Likely): the T# names another format (uint view for a BC compress/mip kernel); vulkan_view_format() gives a view of
  another size class on a non-mutable image, extent = image texels => NVIDIA reads past the image (SRV/UAV twin of rt12).
- Build 108 (7c332a6d9): VIEW_FMT_GUARD (refuse T# views whose texel/block size differs from the image; =0 old) and
  GUARD_SLOT_HASH (VSHARP_GUARD_ALL hit slot = FNV-1a(name) % 16384 instead of compile order; the order-based slot made the
  compute SPIR-V differ every run => cache misses, 8-20 s compiles on "warm" runs). rt20 tests both.
- Translator note (tools/rdna2spirv.py): buffer_load / buffer_store of dwords are "byte exact" (addr = V#.base + offsets,
  OpLoad %uint ... Aligned 4) with no handling for a base that is not 4-aligned, while buffer_load_sub and
  buffer_load_format already split into an aligned dword + byte shift. PTR_ALIGN rounds such addresses down (it stops the
  trap but returns the aligned dword, not the unaligned one RDNA's unaligned mode would give). The exact fix is in the
  translator (two aligned loads + funnel shift for b3 != 0; read-modify-write for stores), which means regenerating SPIR-V.
- rt20 (build 108, cold compute once more: avg 19.4 s, 13->0 t=831): VIEW_FMT_GUARD 53 refusals (BC<->uncompressed and
  BC1<->BC3/BC7 pairs = texture-pool images whose VkImage format is not the T#'s). Loss "Hung" ~490 s after the press, tiny dump
  (no warps). The first no-present had every queue completed (CPU-side wait; a title thread in game+0x23cf735).
- rt21 (build 108, warm): GUARD_SLOT_HASH WORKS - compiles avg 176 ms, 1->3 t=78.6, 3->13 t=88.4, 13->0 t=140 (fastest).
  Hung ~110 s after the press; checkpoints direct: VS fullScreen + PS_MIRROR_PBR_MATERIAL_CLEARCOAT_5f34c83e (top) /
  _STANDARD_5f34c83e (bottom): 2 MB pixel shaders with 17 / 13 loops; graphics stages had no loop cap and no alignment.
- Build 109 (a98cae57f): LEONIDA_CE_LOOP_CAP_GFX=<substrings|1>, LEONIDA_CE_PTR_ALIGN_GFX takes a substring list. rt22 =
  rt21 env + both on PS_MIRROR_PBR.
- rt21 frame_002048 (~105 s after the press) = flat navy.
- rt22 (build 109, LOOP_CAP_GFX/PTR_ALIGN_GFX=PS_MIRROR_PBR: CLEARCOAT 17, STANDARD/EMISSIVE/ANISOTROPY 13 loops capped):
  NO DEVICE LOSS in 1139 s (boot 13->0 t=166.9, compiles avg 171 ms). But after the press the pace crawled (11.4 s/60, then
  ~5 s per frame): the capped mirror PS run to the 16384-block cap every frame => their loop bounds/inputs are garbage
  (Plausible: a root CBV passed raw, like the agA22 blur CB). Then a CPU AV at game+0x5290e15 (read 0) on 2 threads:
  FUN_145290d80 = lock-free pool allocator (traps at 0x1fffe = full), called from FUN_1423d0ea0 (object factory) - pool
  exhaustion after the crawl (Plausible).
- Build 110 (30a61a3d9): LEONIDA_CE_LOOP_CAP_GFX_INNER (graphics cap; else LOOP_CAP_INNER). rt23 = rt22 env +
  LOOP_CAP_GFX_INNER=512 + CBV_SHADOW_ROOT=4096 (two changes at once, on purpose).
- rt23 (build 110 + LOOP_CAP_GFX_INNER=512 + CBV_SHADOW_ROOT=4096): *** FIRST POST-LANDING SCREEN *** No device loss in the
  full 1800 s (~1650 s after the press at t=148.8); compiles avg 198 ms; pace ~12-16 s/60 frames after the press (4-5 fps).
  LOOP_CAP_GFX capped 29 PS_MIRROR_PBR_MATERIAL_* variants (13-25 loops each). frame_004096 / frame_006000
  (/root/rb/rt23_frames) = the title's "COULDN'T AUTOSAVE" dialog (blue, palm trees, RETRY (A) / GOT IT (B)); the scripted A
  presses kept hitting RETRY. The title made one XGameSave submit (save_submit_async/_result once); its result was not logged.
  The two rt23 changes are confounded (cap 512 vs the shadow root); CBV_SHADOW_ROOT logs nothing on success.
- Build 111 (3e81da0eb): xgamesave submit logging. rt24 = rt23 env + PRESS A@s3+8/10x10,DOWN@s3+12/10x3,B@s3+200/15x40.
- LEAD (Plausible, not tested): the PS_MIRROR_PBR "loops" are WATERFALL loops for non-uniform descriptor indexing
  (v_readfirstlane s16, v104 / v_cmpx_eq_u32 s16, v104 / s_load_dwordx8 T# / image_sample_lz / s_andn2_wrexec / s_cbranch_execnz,
  disasm lines 2136/2184/2560/2608). rdna2spirv.py translates readfirstlane in wave32 fragments with a ballot of the active
  lanes; a HELPER lane that the driver leaves out of its subgroup ballot but that is still active in the emulated EXEC never
  matches the broadcast value (an empty ballot reads lane 0 of the half), so it stays in EXEC and the loop never ends ->
  pixel-shader TDR (rt21) or cap-bound crawl (rt22). Translator fix: when the ballot word is empty for an active lane (or
  the lane itself is not in it), readfirstlane returns the lane's own value, so the waterfall retires it. Needs a SPIR-V
  regeneration of the fragment shaders.
- rt24 (build 111, B presses from s3+200): no "xgamesave: submit" line - the run never reached the save point. Pace decayed
  2 -> 12.5 s/60 frames; hang at frame 3341, DEVICE LOST ~840 s after the press (direct queue stalled inside a cmdbuf at a
  list begin; compute checkpoints cs_56009f29 / cs_f5e0d89c / cs_c5b1005e; no Aftermath collected). Frames only up to 2048.
  VM save dir: rt23's autosave DID write saves\local-user\Player (Save%20Game%20Data 2012 bytes, header 296, .display 6 at
  00:24:15) yet the title said "COULDN'T AUTOSAVE" => the submit result or a later step (quota / used-space / read-back) is
  what the title rejects (Plausible). Build 111's submit log will show the HRESULT once a run reaches the dialog again.
- SUMMARY agA29 / NEXT:
  1. Hang/loss still comes ~110-860 s after the press with high variance (rt21 PS waterfall, rt24 compute list begin). Run
     with af-get each time; try LOOP_CAP_GFX on more loop-heavy PS families, or fix the waterfall readfirstlane lead (above)
     in the translator.
  2. Autosave failure: read the "xgamesave: submit" HRESULT; check what the title calls after submit (quota, enumerate).
  3. Arena leak (0 releases; ~1 GiB per 10 min of gameplay) will end longer runs.
  4. Recommended env: build 111 + /root/handoff/rt24.env (= rt23 env + B presses). Keep LEONIDA_CE_PTR_ALIGN=1,
     LOOP_CAP_GFX=PS_MIRROR_PBR + LOOP_CAP_GFX_INNER=512 + PTR_ALIGN_GFX=PS_MIRROR_PBR; VIEW_FMT_GUARD / RT_RETARGET /
     GUARD_SLOT_HASH are default on. Cache is warm for this set (compiles ~0.2 s).

## agA30 (sender gen 30, 2026-11-26 overnight)
- Receiver = the main session. Builds 112+. Work files /root/a30 (xcall.py = call/jmp rel32 xrefs into a game address;
  tr1.sh OUT NAME.. = translate single shaders from work/shaders/dxbc/disasm into OUT/spirv).
- AUTOSAVE ROOT CAUSE (Likely): the title's save code is FUN_1428d1480 (list saves), FUN_1428d1ea0 (autosave: create
  update, write "Save Game Header" 0x128 bytes + "Save Game Data", SubmitUpdateAsync), FUN_1428d2170 (submit result: state
  5 ok / 1 fail). The listing enumerates containers (FUN_1428d1340: mode 0 skips names starting "PARC"/"GA", mode 1 keeps only PARC,
  mode 2 only GA; the VM's containers are "Player" and "SettingsProfileSettings", listed in mode 0), enumerates blobs by name
  "Save Game Header" and sizes its sync XGameSaveReadBlobData buffer itself as strlen(name) + 0x19 + size
  (FUN_1428d1830) = sizeof(XGameSaveBlob) + name + NUL + bytes, no padding. Our HLE 8-aligned the bytes and the name:
  344 needed vs 337 given -> E_GS_PROVIDED_BUFFER_TOO_SMALL on every listing read -> the list's state = 1 (error) even
  though the submit wrote the files. Build 112 (ebdd74ebf): tight packing (name+NUL then bytes), logs the first 16 sync
  reads ("[rt] xgamesave: read ..."), new exact-size test (58 checks pass under wine). rt26 tests it.
  Checked and fine: XAsyncGetResult with no payload (state extracted at completion -> S_OK), the async read path
  (title uses XAsyncGetResultSize), the identity constants.
- WATERFALL / HELPER LANES (Plausible): v_readfirstlane in pixel shaders: an active lane missing from its own ballot (a
  helper a driver leaves out, SHADER-1016) got another lane's value, so the cmpx never retired it and the loop spun
  until LOOP_CAP_GFX (pace crawl) or forever (rt21 TDR). Translator fix (592c8c173): such a lane takes its own value,
  wave32 fragment path and generic (wave64 PS) path; lanes in the ballot unchanged. 739 translator tests pass.
  Census: s_andn2_wrexec waterfalls are in 129 wave32 PS, 1316 wave64 PS, 1118 CS, 278 MS, 42 VS. Only the 30
  PS_MIRROR_PBR_* modules are redeployed so far.
- Build 113 (592c8c173): LEONIDA_CE_SPIRV_OVERRIDE=<folder> (index.json + .spv translated alone; entries replace the
  main ones by (stage, fssm_sha256)). /root/handoff/ovr113.tar = the 30 mirror PS. The deployed corpus is from Nov 23,
  so the override modules also carry every translator change since then (index metadata identical). rt27 tests it.
- rt25 (build 111, 2400 s): "xgamesave: submit 2 op(s), 2 write(s), 2308 bytes ... -> 0x00000000" = the submit SUCCEEDS
  (fits the read-size diagnosis: the failure was the listing). Pace 8.7 -> 293 s/60 frames from ~frame 1920 (crawl), then the
  title pool AV game+0x5290e15 (read 0) on 2 threads, exit 936 s. frame_002000 = real 3D geometry (pink/purple, broken).
- rt27 (build 113 + ovr113): SPIRV_OVERRIDE 30 replaced, 0 pipeline errors. SAVE READ FIX CONFIRMED: "read "Player" 1
  blob(s), buffer 337, need 337 -> ok" and the same for "SettingsProfileSettings" (Definite for the read; the dialog itself
  not re-tested: no submit before the loss). Pace after the press 7.8-15.8 s/60 frames to frame 2040 (rt25 11.6-21),
  then the crawl again at ~2160 (128-237 s/60) and device loss ~775 s after the press (Aftermath Hung, compute checkpoints
  cs_51ddc6bd / cs_344692c2 / cs_c5b1005e). cs_c5b1005e (43 lines) and cs_344692c2 (66) are loop-free: bystanders (Likely).
  cs_51ddc6bd has one strided loop (v0 += 32 while v0 < CB count s19): long only for a garbage count.
- The crawl onset in both runs is where the 3D scene starts: RT_FMT_GUARD retargets, INDIRECT_SRC/INDIRECT_DRAW and RECTLIST
  lines appear for the first time. Arena: still 0 buffer releases; the growth is proxy_slot<70> (placed SRV stand-ins at
  fresh VAs, owned) and proxy_slot<133>; PLACED_EVICT/CAP (agA13) stop it but were dropped for descriptor-lifetime risk.
- Build 114 (2609aea47): LEONIDA_CE_LIST_TIME=1 (+ _MS, default 50): timestamp pair per replayed list, list markers from
  ce::checkpoint (CHECKPOINTS=1), slow lists logged with their pipelines, top-12 pipelines by GPU time every 1024 lists.
  rt28 = rt27 env + LIST_TIME=1, LIST_TIME_MS=100.
- /root/handoff/ovr114.tar (1.9 GB): all 1445 PS with waterfalls (129 wave32 + 1316 wave64) retranslated with the fix,
  1445/1445 spirv-val, index metadata identical. Not run yet (cold compile of 1445 PS pipelines).
- rt28 (build 114, LIST_TIME): SAVE PATH END TO END: both listing reads 337/337 ok and "submit 2 op(s) ... -> 0x00000000"
  (no re-list before the loss; the dialog itself not yet seen either way). Device loss ~850 s after the press (Hung, tiny
  dump). LIST_TIME: the crawl is NOT GPU-bound (Likely): frames 2280-2340 took 215 s of wall time with only ~20 s of
  direct-queue GPU time (1024-list summaries of 0.4-3.2 s). Top GPU users: Scaleform UI lists of 60-130 draws at
  320-360 ms each (VBatchVertexCxformEAlpha_bc8bd7de, VText_a4ab6e63, VVertexCxformEAlpha_dc4780ad, VS_Blit), fspx
  cs_fbda83d0 / cs_5b9456a5 / cs_02a02d7d / cs_8513f5a0 / cs_acb7f63b, one 1330 ms list of 76 dispatches (cs_acb7f63b,
  cs_cd41306b, cs_ecb62afb, cs_3cc33dec). The UI lists are slow per draw (~3 ms) and worth a look, but most of the
  wall time is spent off the GPU.
- Build 115 (8340345c3): LEONIDA_CE_HANG_SAMPLE_MS/_FROM/_MAX: stack dump of every thread (+ queue/wait dumps) for each
  frame slower than _MS after frame _FROM. rt29 = rt28 env + SAMPLE_MS=2000, FROM=1500, MAX=120.
- Build 116 (d5944b3da): LEONIDA_CE_CAPTURE_EVERY=N / _FROM=M (default 2000): capture every N-th frame from M on.
- rt29 (build 115 + rt29.env): *** PAST THE AUTOSAVE: INTRO CREDITS *** VM screenshots (/root/rb/rt29v): s04 (02:21) =
  the opening credits ("Written By / RUPERT HUMPHRIES / ..." pink text bottom-left) over a full-screen teal/black NOISE
  background (the cutscene render is garbage), s05-s08 the same noise with black letterbox bars. No COULDN'T AUTOSAVE.
  Both listing reads 337/337 ok, no submit this run. Device loss ~845 s after the press, then AV game+0x400b8d1.
- CRAWL LEAD (Likely, from 120 HANG_SAMPLE blocks, frames 1769-2154): 31/120 samples catch a title thread inside
  gta6rt xs_find_placed (d3d12_xs_device.cpp ~1235: without LEONIDA_CE_PLACED_EVICT it scans the whole g_placed deque
  from the back) called from proxy_slot<70> (placed SRV) - g_placed holds 120832 records by then and never shrinks.
  Every placed SRV/UAV, CBV_FIX lookup (xs_placed_buffer_va) and placed_buffer_holds walks it: O(records) per call, so
  the CPU cost grows with play time. FIX (next): an interval index kept without eviction (e.g. map start -> index plus
  the max record size, scanning only starts in [va - max_size, va] and taking the highest index that covers va =
  the same answer as the reverse scan). Everything else in the samples is waits (title job threads in
  game+0x221c3c2 / 0x22511f, our compile pool / dstorage workers idle).
- Teal-noise cutscene: check which VIEW_FMT_GUARD / RT_FMT_GUARD refusals hit 2560x1440 / 3840x2160 targets then.
- SUMMARY agA30 / NEXT:
  1. Autosave: FIXED (build 112, read buffer size). Save path complete end to end (rt28 submit S_OK); rt29 reached the intro.
  2. Crawl: CPU-side (rt28 LIST_TIME); fix xs_find_placed's linear scan first, then re-measure with LIST_TIME/HANG_SAMPLE.
  3. Hang/loss ~775-850 s after the press persists (tiny "Hung" dumps; compute checkpoints are bystanders).
  4. Waterfall helper-lane fix: 30 mirror PS deployed (ovr113, pace 7.8-15.8 vs 11.6-21 s/60 before the crawl);
     /root/handoff/ovr114.tar (all 1445 waterfall PS) is ready, untested.
  5. Arena leak (0 buffer releases, 2.6 GB by the loss) and the same g_placed growth: proxy_slot<70> stand-ins.
  Recommended env: build 116 + /root/handoff/rt29.env (add CAPTURE_EVERY=50 to see the screen).

## agA31 (sender gen 31, 2026-11-26 overnight)
- Receiver = the main session. Builds 117+. Work files /root/a31 (commit messages). Each handoff exe now has its pdb next to
  it (/root/handoff/gta6rt-N.pdb): the build dir pdb is overwritten by the next build, so offsets of older runs' stacks
  only symbolize against the matching pdb (rt29's samples symbolize only approximately now).
- rt29 HANG_SAMPLE census (top non-waiting frames, 120 samples): xs_find_placed ~26, placed_title_of ~25 (ExecuteIndirect
  argument buffer lookup), placed_buffer_holds ~15, arena_allocate ~10 => ~2/3 of slow-frame samples in O(n) walks
  (g_placed 120k records; arena 88k free blocks = alignment gaps, 0 releases).
- Build 117 (ac0d1e547, 791e8386a, 006073b0e):
  * src/ce_placed_index.h: PaintIndex (disjoint segments tagged with the newest covering record = the reverse scan's
    answer), ContainIndex (antichain of buffer ranges = placed_buffer_holds), resource -> newest record map. Only
    without PLACED_EVICT. LEONIDA_CE_PLACED_INDEX=0 old scans, =2 cross-check ("[hack] PLACED_INDEX" census,
    "PLACED_INDEX MISMATCH"). Offline random test vs the scans: 4.8M queries, 0 mismatches (Definite for the logic).
  * Arena: largest free block per 1 MiB bucket of start offsets; first-fit / top-down walks skip buckets that cannot
    fit => same block chosen. LEONIDA_CE_ARENA_BUCKETS=0 old, =2 verifies the table. Offline: 300k random ops give
    identical offsets with and without (Definite), ~4.5x faster on a fragmented arena.
  * PLACED_RESTAND (default 1): slot 70/71 texture view over the newest record being a TEXTURE of another description
    (or another start) => reuse an exact earlier record at that va, else a fresh stand-in. rt29's ~37k VIEW_FMT_GUARD
    refusals (BC1 image viewed as BC7, RGBA8 as BC7, ...) were all such stale records (Likely). =2 also for requests
    landing in a buffer record (off: it would hide the buffer from CBV_FIX lookups). =0 old.
  * LEONIDA_CE_NV_DIAG=<flags> VK_NV_device_diagnostics_config (0x2 resource tracking, 0x8 error reporting, 0x1 debug
    info = cold shader cache, 0x4 auto checkpoints).
- Build 118 (9b39c3440): SKIP_BAD also leaves out a refused texture copy (rt29: 14 whole submissions dropped,
  "[copy] rejected at line 1906" = buffer<->image copy with no VkBuffer; 4 during the intro) - LEONIDA_CE_SKIP_BAD_COPY=0
  old; LEONIDA_CE_RT_RETARGET=2 retargets across numeric classes (rt29: ~15k intro draws into 2560x1440 R32G32_UINT /
  R32G32B32A32_UINT / R16_UINT targets left out for PSO fmt R8G8B8A8_UNORM; fragment outputs are float vec4 for every
  target anyway, uint PSOs included).
- rt30 (build 116, CAPTURE_EVERY=50): VM shots show the intro as exploded geometry (screen-wide triangles fanning
  from one point, pink/green/grey), credits "Art Production / JAMES NICHOLSON ..." on top: the 3D scene draws, but
  vertex positions are garbage (NaN/huge or a common point) => vertex/transform data, not just a refused texture.
- rt30 (build 116, main session): two xgamesave submits S_OK (251528 bytes = a real game save, then 2308). Pace 9-22 s/60
  then 284-333 s/60 from frame 2040. 65536+ "draw left out" (uint 2560x1440 targets), ~40k VIEW_FMT_GUARD refusals,
  18 "ExecuteCommandLists failed". frame_002050 = exploded geometry, 2100-2250 flat purple. No device loss in 1800 s.
- rt31 (build 118 + PLACED_INDEX=2, ARENA_BUCKETS=2, NV_DIAG=0x2, RT_RETARGET=2): PLACED_INDEX 0 mismatches in 1M
  cross-checks (135k records, 72k segments); ARENA_BUCKETS verify 0 bad; VIEW_FMT_GUARD 0 (was ~40k); "draw left out" 0
  (234 retarget lines); "ExecuteCommandLists failed" 0; PLACED_RESTAND 65536+: the old lookup handed 1x1 textures
  (allocation 64 KiB, 4 KiB apart) their NEIGHBOUR's resource ("record @dd7bc7000 -> request va dd7bc8000") and
  2048x512 textures the previous texture at overlapping addresses - every such view showed the wrong texture before.
  NEW boot states after the press: 0->9 t=411, 9->10, 10->0, 0->11, 11->0 (t=432); never seen before (Plausible: the
  load/save flow advancing). No device loss in 1500 s. Screens: navy ~10 min, then PURPLE noise, then GREEN noise: the
  full-screen layer still shows noise (hue follows the scene grade). CRAWL NOT FIXED: 10-17 s/60, then 99-265 s/60 from
  ~frame 2100. Its 65 samples (98 busy stacks, gta6rt-118.pdb): DirectStorage zlib inflate ~21, LayoutTracker copy in
  VulkanQueue::execute ~12, placed texture uploads ~14 (proxy_slot<70> 9, refresh 6), placed scans only 2 (the =2
  cross-check itself).
- Build 119 (3bde79880): LEONIDA_CE_STANDIN_UPLOAD=1 (default off) copies title bytes into slot-70 BUFFER stand-ins
  (made at fresh VAs by CreateCommittedResource; nothing ever filled them: BUFFER_SYNC off, the ring sync writes
  arena[title VA]) on each view made over them. ~95k such stand-ins / 1.2 GB by the intro.
- Build 120 (e9f244142): LayoutTracker undo log (LEONIDA_CE_TRACKER_UNDO, default on): VulkanQueue::execute copied the
  whole tracker per submission for rollback; now edits in place with a first-touch undo log (tracker_mutex_ held from
  the copy point to the commit, so identical). rt32 = build 120 + rt32.env (STANDIN_UPLOAD=1, PLACED_INDEX=1,
  ARENA_BUCKETS=1).
- rt31 noise analysis (frame_002300, 3840x2160): G = 211 everywhere, R and B jump 0/255 per pixel (R and B together), and
  the pattern repeats EXACTLY every 64 px in x and y => a 64x64 tile at full scale, not uninitialised memory (Definite
  for the periodicity). Candidate: the 64x64x32 blue-noise array the Scaleform dither PS read
  (FTexTGDither_99e97f05: image_load (x&63, y&63, s0&31) unorm, triangular noise * 1/127.5 added to all 4 channels;
  log has FTexTGDither / FTexTGCxformDither / _FTexTGCxformEAlphaDither in the lists). Why only R and B: open (exp is
  compr: v1 = (R,G) halves, v2 = (B,A); R and B are the low halves - Trust me bro).
- rt31 slow-frame SYNC_DUMP: GPU idle (direct queue submitted == completed), queue state "busy" = a thread inside
  VulkanQueue::execute replaying lists on the CPU => the crawl is the single submit thread's CPU replay (Likely).
- rt32 (build 120 + STANDIN_UPLOAD=1): states 9/10/11 ~2 min sooner (t=285-297); pace 7.0/7.7/24.4 s/60, then 53.6 s
  at frame 2220 (rt31 265 s) => the LayoutTracker undo log cut the crawl ~5x (Likely). STANDIN_UPLOAD 32581 copies /
  306 MiB, 187 unreadable. Died at t~400: AV in memcpy from proxy_slot<70> on a vanished title page (0xd4dfe0000):
  xs_copy_title_seh's __try compiled to a 5-byte tail call into memcpy (memcpy is nounwind; no -fasync-exceptions)
  => no SEH frame. xs_hash_title_seh / xs_detile_seh use the same pattern with inlined loads: probably unprotected too.
- Build 121 (7a48d05bd): LEONIDA_CE_DIAG_TEX=WxH logs new SRV/UAV views of WxH images. Build 122 (8d697954d):
  STANDIN_UPLOAD copies with ReadProcessMemory. rt33 = build 122 + rt33.env (rt32.env + DIAG_TEX=64x64).
- rt33 (build 122 + DIAG_TEX=64x64): no early AV (ReadProcessMemory: 63705 copies / 1271 MiB, 1831 unreadable). States
  9/10/11 at t=260-273. Pace 7.4/7.6/14.4 s/60, 67 s at 2220, 123 s at 2280. DEVICE LOST t~674 (~530 s after the press),
  Aftermath "Reset" (hang), vertex shader "vertex_01" (size 34048) warps parked @0x3350/0x3370, no faulted warp; direct
  checkpoint VS_HairStrandGeometry_8e7634e5 + PS_HairStrandVBuffer_8e7634e5 (hair strands: character rendering - new;
  most likely it only gets real input now that STANDIN_UPLOAD fills the stand-ins). device_fault address 0x22531f480 =
  3.4 GB BELOW heap_base (0x300010000): VSHARP_GUARD_ALL lets pointers below heap_base pass (upload buffers live there).
- NOISE LEAD (Plausible): DIAG_TEX shows the 64x64 noise-sized SRVs are 3D volumes (T# type 10 = SQ_RSRC_IMG_3D,
  64x64x32 / 64x64x64: R16F, RGBA16F, R32_UINT, R8, RG16F), while the Scaleform dither PS reads its noise with
  image_load dim:SQ_RSRC_IMG_2D_ARRAY (layer = frame & 31). rdna2spirv picks the bindless table from the INSTRUCTION's
  dim (%images_arr for 2D_ARRAY), but bind_image registers a 3D T# only in the 3D table, and the tables have separate
  index spaces (vulkan_bindless.h add_image) => the dither reads slot <3D index> of the 2D-array table = some unrelated
  texture, tiled 64x64 => full-scale periodic noise. On RDNA the T# type wins over the instruction dim. Fix options:
  (a) bind_image: for a 3D T# also make a 2D-array twin image (W x H x depth layers; vkCmdCopyImage 3D <-> 2D array is
  legal with maintenance1) and put it in the 2D-array table at the SAME index (reserve the index in both tables);
  (b) translator: when the instruction dim and the T# type can differ, select the table from T# TYPE (word3 [31:28]) at
  run time. Not done (time).
- Build 123 (a33d72d96): REFRESH_CURRENT (default on, =0 old): the periodic placed-texture refresh only hashes records
  still newest at their start (paint index). rt34: "pass 64: 7638 textures, 3043 superseded skipped".
- rt34 (build 123 + LOOP_CAP_GFX / PTR_ALIGN_GFX = PS_MIRROR_PBR,HairStrand): HairStrand VS/PS capped (1 loop each,
  512 blocks) + PS_MIRROR_PBR_MATERIAL_HAIR 13 loops. PACE HEALTHY: 7.6 / 7.7 / 15.5 / 11.9 s per 60 frames at frame
  2220 (rt29-31: 130-290 s; rt33: 67 s) => the crawl is fixed at that point (Likely: index + tracker undo + refresh).
  No GPU loss, but a CPU AV at t~370: memcpy+0x2023 reading 0xbf3d9fd00 from xs_cbv_shadow_refresh (gta6rt+0xf8dae)
  <- child_slot<0,35> (console ExecuteCommandLists) on a title worker: the CB shadow memcmp against title memory
  after an xs_readable check (TOCTOU). Same module offset as rt6 / rt8 / rt11's early AV (Likely the same bug).
- Build 124 (c1f36082f): CBV_SHADOW refresh and first copy read title bytes with ReadProcessMemory. rt35 = build 124 +
  rt35.env (= rt34.env).
- Builds 124 AND 125 exited 2-4 s after launch on the VM (right after the title's first SetUnhandledExceptionFilter,
  before xgameruntime loads; no "[error]" line, empty exit code; /root/rb/rt35-b125-fail.err.log), build 123 runs. CAUSE
  (Likely): both added a "static thread_local std::vector" scratch in xs_cbv_shadow_refresh, which grew gta6rt's .tls block
  0x1458 -> 0x1590 bytes and added dynamic TLS init + a TLS destructor; the loader's title-TLS handling does not survive
  that. RULE: no thread_local objects in gta6rt; check "llvm-objdump -p gta6rt.exe" (.tls raw data size) before a handoff.
  Build 125 (xs_title_copy: memcpy through an opaque pointer in __try; disassembly shows a real SEH landing pad) and 126
  (same pattern in ce_copy_title / xs_hash_title_seh, + another thread_local) were reverted (954a5865f) unrun; the SEH
  pattern itself was never tested on the VM.
- Build 127 (9881293c2) = build 123 + CBV_SHADOW reads via ReadProcessMemory with a plain static scratch (.tls 0x1458 =
  build 123). rt36 = build 127 + rt36.env (= rt34.env).
  NOTE for TOCTOU reads of title memory: gta6rt is built without /EHa (only xmem_test / tiny_dll have it), so __try
  around inlined loads or a direct memcpy protects nothing (ce_copy_title, xs_hash_title_seh, xs_detile_seh included).
- Build 128 (5f0d4f05a): LEONIDA_CE_3D_TWIN=1 (default off): a sampled 3D view takes an index free in BOTH the 3D and
  2D-array tables (vulkan_bindless.h add_image_twin) and leaves the fallback in the 2D-array slot, so a 2D_ARRAY read of
  a 3D T# returns 0 (dither adds a constant -1/127.5) instead of another texture. Freed from both on release. rt36 =
  build 128 + rt36.env (rt34.env + 3D_TWIN=1).
- rt36 (build 128 + rt34.env + 3D_TWIN=1): *** FURTHEST RUN *** no early AV (CBV refresh via ReadProcessMemory), pace
  2.7-4.3 s/60 frames to frame ~2000 (best ever), 13.7 at 2220, 4-10 s/60 to 2940, 28-32 s at 3180-3240, then
  133-156 s/60 at 3300-3360. States 9/10/11 at t~214. Frames 2000-2650 black, 2700-3200 flat navy (16,16,22), 3250-3350
  the noise layer again (yellow (255,255,0) / blue (0,0,255) per pixel: R,G and B saturate in opposite directions = a
  large signed noise, not a mono dither). DEVICE LOST t=749.9, no fault address; direct checkpoint
  VS/PS_DistantWindowEmissives_4eb040bb (city skyline windows - the 3D scene is the open world now), then a title AV
  game+0x5267e47 (read 0xd00000008) after the loss. 3D_TWIN: 128+ twinned, 64+ "NO common index" (no index free in both
  tables); noise unchanged => 3D_TWIN alone does not remove it (Likely).
- Build 129 (b49ebb030): LEONIDA_CE_UPLOAD_3D=1 (default off): placed TEXTURE3D are uploaded from title memory (SRV
  creation via slot 18 / 70, and the periodic refresh) with the xg Tex3d layout; before only TEXTURE2D were, so placed 3D
  volumes (the intro's 64x64x32/64 noise volumes) held whatever the allocation contained. rt37 = build 129 + rt37.env.
- rt36 also logged "[rt] d3d12: a bindless image array is full; dimension 1": the sampled 2D-array table (1024) ran FULL,
  so every later 2D-array view fell back to index 0 (broken rendering of every texture array after that point), and
  3D_TWIN found "NO common index" for the 64x64x64 volumes (the 3D table holds 256). bindless set at boot: 65536 2D,
  3936 samplers, 1024 2D-array, 256 3D, 256 1D, 4096 storage indices in 2D.
- Build 130 (39e5d7d06): LEONIDA_CE_BINDLESS_2DA=<n> / LEONIDA_CE_BINDLESS_3D=<n> (default 1024 / 256) size those tables
  (runtime arrays in the SPIR-V; the set layout change = one cold pipeline compile). rt38 = build 130 + rt38.env
  (rt37.env + 2DA=8192, 3D=1024).
- rt37 (build 129 + UPLOAD_3D=1): starts, 47 "uploaded placed texture" lines; noise still there (blue/black at 05:03);
  pace 3.3 s/60 at 1620-1920, 13.8 at 2220, 7.7 at 2520, 16.5 at 2820, 104 at 3120. All three image tables FULL again
  (2D-array line 10651, 1D 19437, 2D 24264 - in rt36 too: 10423 / 18318 / 22871): from the intro on, new 2D views fall
  back to index 0. DEVICE LOST t=1050.5 (the latest yet), direct checkpoint VS_TransformPreSkin_VfxVBuffer_01a5f889 +
  PS_VfxVBuffer_01a5f889, device_fault VA 0x22f6da1a0 = 3.4 GB below heap_base (like rt33's 0x22531f480: a pointer
  heap_base + VA with a small negative VA; VSHARP_GUARD_ALL lets pointers below heap_base pass).
- Build 131 (ca830e4f6): LEONIDA_CE_BINDLESS_2D=<n> raises the 2D growth target (default 65536). The tables fill because
  bindless entries live as long as their images and the placed stand-ins never die (the stand-in / arena leak again).
  rt38 = build 131 + rt38.env (rt37.env + BINDLESS_2DA=8192, BINDLESS_3D=1024, BINDLESS_2D=262144; cold compile).
- SUMMARY agA31 / NEXT:
  1. CRAWL: FIXED for the first ~3000 frames (Likely). rt36: 2.7-4.3 s/60 before the intro (was 7-10), 4-17 s/60 through
     frame ~3000 (was 130-330). Causes removed: O(n) g_placed scans (PLACED_INDEX, exact, cross-checked), arena first-fit
     walk (ARENA_BUCKETS, exact), LayoutTracker copy per submission (TRACKER_UNDO), superseded-texture refresh
     (REFRESH_CURRENT). A slower crawl returns past frame ~3100 (28 -> 104-156 s/60): next samples show CopyPageMappings
     (map_alias per page, std::map), DirectStorage zlib inflate, child_slot<0,37>.
  2. WRONG TEXTURES: PLACED_RESTAND (default on) - the old lookup gave 1x1 / small textures their neighbour's resource
     and reused stale records after memory reuse (65k+ restands per run, 40k VIEW_FMT_GUARD refusals -> 0).
  3. CRASHES fixed: rt34's CPU AV (CB shadow memcmp on released title pages, = rt6/rt8/rt11's) via ReadProcessMemory;
     STANDIN_UPLOAD copies via ReadProcessMemory. gta6rt has no /EHa: __try does not protect inline loads/memcpy.
     Never add thread_local objects (builds 124/125 died at start); check the .tls size.
  4. INTRO NOISE: still there. It is a 64-px-periodic full-scale signed noise (R/G vs B saturate oppositely), from frame
     ~3250 in rt36. Tried: RT_RETARGET=2 (all left-out draws now drawn), 3D_TWIN, UPLOAD_3D - none removed it. Open
     leads: the image tables are FULL by then (everything new samples index 0) - rt38 tests bigger tables; the 2D_ARRAY-
     instruction-on-3D-T# mismatch (3D_TWIN could not twin the 64x64x64 volumes in rt36: no common index).
  5. DEVICE LOSS moved later: t=674 (rt33, HairStrand VS, before the cap), 750 (rt36, DistantWindowEmissives), 1050
     (rt37, VfxVBuffer); rt33/rt37 have a fault VA ~3.4 GB below heap_base: graphics pointers with negative VAs.
     Candidate: a lower-bound guard (sink pointers below heap_base - X) for graphics modules, keeping the upload-buffer
     window.
  6. Recommended env: /root/handoff/rt38.env with build 131 (or rt36.env + build 128 if the cold compile is unwanted).
     New defaults on: PLACED_INDEX, ARENA_BUCKETS, TRACKER_UNDO, PLACED_RESTAND, REFRESH_CURRENT, SKIP_BAD_COPY (with
     SKIP_BAD=1). Opt-in used: RT_RETARGET=2, STANDIN_UPLOAD=1, NV_DIAG=0x2, LOOP_CAP_GFX/PTR_ALIGN_GFX +HairStrand,
     3D_TWIN=1, UPLOAD_3D=1, BINDLESS_*.
- rt37 Aftermath (main session): "Reset", vertex_01 (size 89600) 142 active warps @0x9260, FAULTED WARP "Misaligned
  Address Error" in vertex_01 @0x9220 => the compute PTR_ALIGN case, now in a VERTEX shader (VS_TransformPreSkin_
  VfxVBuffer). rt38.env sets LEONIDA_CE_PTR_ALIGN_GFX=1 (all graphics modules; same cold compile as the layout change).
- rt38 (build 131 + rt38.env: BINDLESS_2D=262144 / 2DA=8192 / 3D=1024, PTR_ALIGN_GFX=1, UPLOAD_3D, 3D_TWIN, STANDIN_UPLOAD;
  cold compile): *** LONGEST RUN: NO device loss, NO AV in 1800 s, frame 6120+ *** ("bindless set: 262144 2D, 8192
  2D-array, 1024 3D"; only the 1D table (256) filled; 3D_TWIN 0 "NO common index"). States 9/10/11 at t~736 (cold
  compile). Pace 3-8 s/60 frames 3400-5520, then 21.9 s at 5820 and 158 s at 6120 (a late crawl again). Frames 3000-5800
  flat dark / navy; from ~5950 the teal 64-px noise layer WITH a GTA help box "(L) Move ... / (R) Move the camera"
  (VM shot rt38v/s14.ppm 05:37; most glyphs missing) => GAMEPLAY (the control tutorial) has started, under the noise
  (Likely). The noise is NOT the full tables, the 3D twin or the 3D uploads (all on here).
  PTR_ALIGN_GFX=1 (all graphics) is the probable reason the VS misaligned-address hang (rt37) did not come back.
- NEXT (agA32): (1) the 64-px full-scale noise layer over the game view (find the full-screen pass: LIST_TIME / checkpoint
  names of the last full-screen draws before the UI; DIAG_TEX on the 64x64 2D-array SRVs too); (2) the late crawl past
  frame ~5800 (HANG_SAMPLE with gta6rt-131.pdb); (3) glyph cache (help text glyphs missing); (4) stand-in / bindless
  leak (entries never freed while g_placed holds every stand-in).

## agA32 (sender gen 32, 2026-11-26 early morning)
- Receiver = the main session. Builds 132+. Work files /root/a32 (edit scripts edit13N.py, commit messages, tile.py,
  census.py = HANG_SAMPLE census of non-waiting stacks). Symbolize: copy gta6rt-N.exe/.pdb as gta6rt.exe/.pdb into one
  folder (/root/a32/sym131) and run llvm-symbolizer --obj=gta6rt.exe 0x100000<rva> (ImageBase 0x10000000000).
- NOISE ANALYSIS (rt36/rt37/rt38 frames): the 64x64 tile has EXACTLY 2048 texels in one class and 1877 + 171 in the
  other, and B is a ramp that takes 171 distinct values once each (0,1,3,4,...,253) => out = saturate(S + g*(2n-1)) with
  n a 64x64 noise of 4096 distinct levels (16-bit / float), S ~ 0 (black scene), g per channel: B ~ +12 in all three runs
  (identical B histograms), R/G = +-huge or ~0 and different per run (rt36 R,G = -big; rt38 G = +big, R ~ 0) (Definite for
  the arithmetic). Static across frames (layer index not changing, or the same garbage).
- THE PASS (Likely): PS_TAA2_SpatialUpscaleWithDithering_08f754c8 (2560x1440 -> output upscale):
  out.rgb = bicubic(scene T# at table s9+0x0) / w + CB[0xc0], CB[0xc4], CB[0xc8] * (2 * image_load(x&63, y&63,
  CB[0xbc]&31, 2D_ARRAY unorm, T# at s8+0x2a0) - 1); CB V# at s9+0x20. Exactly the observed form: the per-channel dither
  gains CB+0xc0..0xc8 are garbage (should be ~1/255-ish), the scene term is ~0.
- CBV_SHADOW POOL: "CBV_SHADOW: pool full (~205k views)" in rt36 (after frame 3300), rt37 (~3000), rt38 (~5800): later
  CBVs fall back to the one-time ring snapshot (clobbered when the ring wraps). Coincides with the noise onset (rt38),
  close to it in rt36 (noise already at 3250) (Plausible as the gain source).
- LATE CRAWL ROOT CAUSE (Likely): rt38 HANG_SAMPLE frames >= 5800 (90 samples, gta6rt-131.pdb): title threads in
  proxy_slot<17> (CreateConstantBufferView) -> xs_cbv_shadow waiting on g_cbv_mu (37), the submit thread in
  xs_cbv_shadow_refresh (28) <- child_slot<0,35>; the rest is nvgpucomp (pipeline compiles for gameplay). The refresh does
  one ReadProcessMemory + memcmp per shadow per ExecuteCommandLists: 205k per sync by then ("16384 syncs, 205530 views").
- GLYPHS: not lost, LATE: rt38v s14 (05:37) shows "M .. d / M .. th", s16 (05:41) "Move around / Move the camera".
  The 2048x2048 A8 glyph cache (0x400000 bytes, fully hashed) is refreshed every 30 presents = ~60 s at the crawl's pace.
- Build 132 (156e03e1e): LEONIDA_CE_FRAME_TRACE=<f>[,<a>-<b>] (every replayed list of those frames, pipelines in order),
  RTDIAG per-target pipeline names ("[rtdiag]     pipe xN"), LEONIDA_CE_RTDIAG_DUMP_FROM, RGBA16F target dumps,
  LEONIDA_CE_BINDLESS_1D=<n> (the 256-entry 1D table filled in rt36-rt38, "dimension 3").
- Build 133 (59e062195): LEONIDA_CE_DRAW_PEEK=name:off:cboff:len (CB bytes a draw reads: arena and title view, CBV_SHADOW
  source).
- Build 134 (7ca9d790a): LEONIDA_CE_CBV_SHADOW_CHUNK=1 (default off): shadows per 64 KiB title chunk shared by every CBV
  over those bytes; refresh = one read per chunk, changed 4 KiB pages written. Use with CBV_SHADOW_MB=256.
- rt39 (build 132 + rt39.env, cold compile): "bindless set: ... 16384 1D", NO "array is full; dimension 3" (1D fixed).
  CBV_SHADOW pool full again (204909 views). Intro reached only at frame ~7800 (cold compile shifted frame numbers, so the
  FRAME_TRACE frames 5000-6101 were a 3-draw loading screen). Noise STILL there (purple, VM shot 06:26) => the 1D table
  is not the cause. DEVICE LOST at frame 7916 (Aftermath "Hung", direct checkpoint VS/PS_DistantWindowEmissivesDeferred_
  58ac1373 - loop-free VS/PS, like rt36's DistantWindowEmissives hang), then title AVs (VCRUNTIME memcpy write from
  game+0x4e5b4b8; game+0x43adfb0 read 0x40). rtdiag 7800: the presented image is written only by sga_dr_vs_pos_color_tex +
  default_ps_tex (a blit of a 3840x2160 image); 18k draws still pending-pso then.
- rt40 (build 134 + rt40.env = rt39.env + CBV_SHADOW_CHUNK=1, CBV_SHADOW_MB=256, FRAME_TRACE 7600..10000, DUMP from 7200,
  DRAW_PEEK on SpatialUpscaleWithDithering) was QUEUED when agA32 stopped; results not analysed.
- NEXT (agA33): (1) read rt40: "CBV_SHADOW_CHUNK:" chunk count / no "pool full", pace past the intro, DRAW_PEEK lines (the
  dither gains at CB+0xc0..0xc8 as the GPU sees them, their source: CBV_SHADOW chunk/view or ring) and whether the noise
  is gone. If the gains are garbage in the arena but sane in title memory -> a CB sync problem; if title memory is also
  garbage -> wrong V# (table offset / root layout). (2) The scene term is ~0 (black) even where the noise is not: check
  the dumps' 2560x1440 targets. (3) Device hang in DistantWindowEmissives(Deferred) VS (rt36, rt39). (4) Glyphs: refresh
  glyph caches more often than every 30 presents (or fix the crawl).

## agA33 (summary written by agA34; agA33 left no section)
- Build 135 (c1e446dd4): LEONIDA_CE_CBV_PLACED=1, so a CBV inside a slot-133 placed BUFFER names that buffer's stand-in
  instead of a shadow of the title bytes. LEONIDA_CE_CBV_PLACED_WATCH=1 copies changed title granules into the stand-in.
  The premise came from rt40 DRAW_PEEK: the CB of PS_TAA2_SpatialUpscaleWithDithering (V# s9+0x20, 0xd0 bytes) is exactly
  the placed buffer at title 0xde22af580, and its title bytes are garbage (dither gains cb+0xc0..0xc8 = int 8, 0, 11.95).
- rt41 (build 135 + rt41.env): the noise is GONE, and the screen is PURE BLACK. All dumps (target0 2560x1440 RGBA16F,
  frames 3750-5400) are zero. States 11->0 at t=276; pace 3.6-6.7 s/60 to frame 3120, then 16-57 s/60. No device loss
  in 1500 s.

## agA34 (sender gen 34, 2026-11-26 morning)
- Receiver = the main session. Work files /root/a34 (edit scripts, commit messages).
- rt41 READING (Definite for the bytes): CBV_PLACED retargeted 9.9M CBVs onto 23 placed buffers, and the watch wrote 0
  granules. Every stand-in is all zero. The title bytes of the TAA CB never change: 150 DRAW_PEEK reads give identical
  values at +0x0 (ed558ccd ff51afd7 40b5ac30 00000001 14ffedf0 000000a8 ...) and +0xb0. They look like CPU data (0x1_40b5ac30
  is a game-image address, 0xa8_14ffedf0 a heap/stack pointer), not constants.
  => Neither the title bytes (rt40: noise) nor the newest record's stand-in (rt41: zeros) hold the TAA constants.
  CAUTION: the main session first read "title bytes change", but it was comparing the +0x0 line with the +0xb0 line.
- THEORY (Plausible): ALIASING. The CB memory is covered by an older and/or bigger placed buffer record, and the real
  constants reach that one: a GPU write, or an init-time copy into a record that existed then. Every record has its own
  stand-in here, and xs_find_placed answers the NEWEST record, so we bind an empty one. The arena is 4 GiB, and
  0xde22af580 is outside it and not pool memory, so a raw-VA GPU write cannot land in any copy we read.
- Build 136 (c584ce050):
  * CBV_PLACED alias diagnostics. "CBV_PLACED aliases of <va> +<size>: N overlapping record(s)" lists up to 12 overlapping
    records with g_placed index, newest/owned/CONTAINS and GPU VA (first 48 buffers).
  * The CBV refresh hashes every containing buffer's stand-in at that range, every 256 syncs. Changes are logged as
    "CBV_PLACED probe #k (title ..) record i @va stand-in ... CHANGED" (400 lines max).
  * LEONIDA_CE_CBV_PLACED_ALIAS=1 (default off): CBV_PLACED CBVs, and CBV_FIX root views in placed buffers, name the
    OLDEST buffer record that contains them. Root lookups are cached per record (8192 scans max). Counts are in the
    CBV_PLACED summary ("ALIAS n CBV + m root rebinds") and in the "CBV_PLACED_ALIAS:" lines.
  * LEONIDA_CE_DRAW_SRV=name:lo-hi[,..]: scans a named draw's user-data tables (lo..hi, 16-byte steps) for T#s of known
    images. Logs "DRAW_SRV ..." and probes / dumps them at every RTDIAG dump (tag srv_<name>_ud<k>_<off>, 32 max).
  * LEONIDA_CE_RTDIAG_TOP=<n> (default 8, max 24): probe more of the busiest colour targets.
  * Note: the probe writes a .ppm only when the target has non-zero bytes. "[rtdiag] probe ... N of M bytes non-zero" is
    logged for every probed target either way, so a missing file means all zero (or a format that is not dumped).
- rt42 = build 136 + rt42.env (rt41.env + CBV_PLACED_ALIAS=1, RTDIAG_TOP=24, DRAW_SRV on SpatialUpscaleWithDithering
  0x0-0x2c0 / CompositeNMB / SSSResolve 0x0-0x100, dumps every 300 frames from 3600).

## agA35 (sender gen 35, 2026-11-26 morning)
- Receiver = the main session. Work files /root/a35 (edit137.py, msg137.txt).
- THE #58 "ALIAS" IS NOT REAL (Very likely): record #58 = 3840x2160 fmt 19 (R32G8X24) at 0xddefa0000, size 0x3fc0000 = the PC
  GetResourceAllocationInfo size. The title placed its next resources at 0xde17f0000 (+0x2850000 = ~40.3 MiB = D32 + S8 planes
  of 3840x2160 with tile padding), then 0xde1c3b000.. and the CB pool 0xde22a....: on the console the texture ends before the TAA
  CB. The CB 0xde22af580 (+0xd0, slot 133, flags 0) sits among small CB buffers packed in one 64 KiB page (de22ad930,
  de22afdd0.., rt41 lines 8413-8530). No bigger BUFFER record covers it. So memory aliasing (shared VkDeviceMemory) is not the fix
  for this CB, and it was not implemented. If it is ever needed: placed textures are separate VkImages with their own memory
  (create_texture, even in a D3D12 heap), and buffers are arena ranges (create_placed_buffer); the arena memory is HOST_VISIBLE
  (prefers DEVICE_LOCAL), so an image would have to be bound into arena.memory at a heap offset (check memoryTypeBits).
- THE WRITER (Likely): the CB title bytes never change and look like recycled CPU data, so the CPU does not write them, and
  xmem aliases physical pages for the CPU (map_alias), so another CPU mapping would show at the title VA too. The 23 CBV_PLACED
  stand-ins stay zero, so no recorded D3D12 copy, UAV or root write reaches them. Console list slot 62 = GPU memcpy by VA
  (dst VA, src VA, bytes, flags; agA21: fn 0x140979650, first call dst 0xdaa3e7ef0 <- src 0xbdbc50000, 0x4000) is still
  DROPPED by the list proxy ("console list slot 62 (not forwarded)" in rt41). It is the only dropped GPU writer of title memory.
- Build 137 (c2d88bf13): LEONIDA_CE_LIST_COPY62.
  * =1 records and logs the calls: "LIST_COPY62 #k from <ra>: dst .. <- src .., bytes | dst rec .. [CBV_PLACED] | src rec ..".
    It logs the first 48, every 2^k-th, up to 200 onto placed buffers and up to 200 onto CBV_PLACED buffers.
  * =2 also performs the recorded copies, in list order, right BEFORE the list's ExecuteCommandLists (before
    xs_cbv_shadow_refresh). It reads src with ReadProcessMemory, writes the dst title bytes with WriteProcessMemory, and copies
    into every placed BUFFER stand-in in the arena under dst ("LIST_COPY62 done ..." lines). The summary line ends with
    "CBV_PLACED stand-ins non-zero: N of M".
  * =3 is like 2, but only for copies whose dst is a placed buffer.
  * Limits: a CPU copy at submit reads the src as the CPU left it. If the src is GPU-written in the same frame, it is stale;
    then the next step is a real in-list arena-to-arena vkCmdCopyBuffer when both ends resolve to stand-ins.
  * .tls unchanged.
- rt43 = build 137 + rt43.env (rt42.env + LIST_COPY62=2), requested from the main session. Results are not seen by agA35.
- NEXT (agA36):
  (1) Read rt43: do slot-62 dsts cover 0xde22af580 / CBV_PLACED buffers? What are the srcs (upload ring, a placed buffer, title
      heap)? What is the non-zero count, the DRAW_PEEK values, and is the picture still black?
  (2) If the dsts never touch the CBs, look at the other dropped list slots (74, 101, 126, 129, 131, 132, 142, 28-30, 70, 75),
      and at slots 71/72 (write32/64, applied only after the fence).
  (3) If the srcs are GPU-written, do the copy on the GPU in list order.
- rt43 RESULT (main session): only ONE slot-62 copy in the whole run (dst 0xdaa41b9e0 <- src 0xbdbc10000, 0x4000), none onto a
  CBV_PLACED buffer; non-zero CBV_PLACED stand-ins 0 of 23. => slot 62 is NOT the CB writer (Very likely). DEVICE LOST at
  558 s (rt42 also lost at 713 s; rt41 without ALIAS ran 1500 s clean); no dumps (died before frame 3600).
- Build 138 (7d545ad09): LEONIDA_CE_WATCH_RANGE=lo-hi. "[watch] <kind> #k: title va +bytes | rec -> stand-in | extra" for
  device CBV (17, final BufferLocation), buffer SRV/UAV (18/19), placed SRV/UAV (70/71, bound record, owned), root
  CBV/SRV/UAV (final translation), list CopyBufferRegion / CopyResource / CopyResourceX (dst + src), list write32/64
  (71/72), slot-133 placements. 600 lines max. Off = no lookups. NOT covered: raw V#s written straight into descriptor
  heaps, WriteBufferImmediate.
- rt44 = build 138 + rt44.env (rt42.env - CBV_PLACED_ALIAS, DUMP_FROM 3300, + WATCH_RANGE=0xde22a0000-0xde22b0000).
- NEXT (agA36): read rt44 [watch] lines: a UAV / copy / write32 / root UAV onto 0xde22af580..650, and the stand-in it
  resolves to vs the CB's (0xffdd4c00). If something writes a DIFFERENT stand-in, bind CBV_PLACED to it (or alias the two
  arena ranges). If nothing shows, the writer is a raw V# in a descriptor heap (compute) - hook the V# translation
  (VSHARP_GUARD path) for bases in the range.
- rt44 (build 138 + WATCH_RANGE, main session; Definite): the TAA CB 0xde22af580 (+ 0xde22ad930, 0xde22affc0) is written
  every frame by a list CopyBufferRegion: dst = the CB placed buffer (stand-in 0xffdd4c00, resolved correctly), src = a
  placed buffer over the title's UPLOAD RING (src title VA moves across 0x3ab9da50..0x3c39da50; caller 0x1404F4600). The copy
  is forwarded, but the source stand-in is never synced from title memory (BUFFER_SYNC off), so the GPU copies zeros: the
  black screen. Also: placed SRVs (slot 70) at 0xde22af650 +0x50 / 0xde22af6a0 +0x310 were bound to texture record #58 (its
  PC size overruns the console size), so they got a null view.
- Build 139 (2fd63afdf):
  * LEONIDA_CE_COPY_SRC_SYNC=1. Each list CopyBufferRegion whose source is a title placed buffer (<= 1 MiB) is remembered.
    Right before the list's ExecuteCommandLists, the title source bytes are written into the source stand-in at the copy's
    source GPU VA, so the forwarded GPU copy moves them in list order. =2 also writes the dst stand-in and dst title on the
    CPU. Logs: "COPY_SRC_SYNC #k" (recorded), "COPY_SRC_SYNC done #k" (first words), and the summary
    "CBV_PLACED stand-ins non-zero: N of M".
  * LEONIDA_CE_PLACED_BUF_OVER_TEX=1: a buffer view over a texture record that starts elsewhere gets its own buffer stand-in.
- rt45 = build 139 + rt45.env (rt44.env + COPY_SRC_SYNC=1 + PLACED_BUF_OVER_TEX=1), requested from the main session. Not seen
  by agA35.
- NEXT (agA36): read rt45 (non-zero count, DRAW_PEEK gains, is the world visible).
  If the CB is still zero: try COPY_SRC_SYNC=2, and check that the forwarded copy executes (not skipped as invalid).
  If other CBs / vertex data come through the same ring by CopyBufferRegion with sources > 1 MiB, raise the cap.
  The general form of this bug: any title placed buffer that the CPU writes and the GPU reads through a copy or a view needs
  a sync. BUFFER_SYNC hashes everything, which is too slow; syncing per use, as here, is the cheap form.

## Checkpoint 2026-11-26 08:50 PST (main session) - paused by the user

Where it stands:
- Boot -> intro -> gameplay (control prompt) works. The 3D view is black (was dither noise before build 135).
- Cause, found in rt44 with build 138 WATCH_RANGE (Definite): the TAA upscaler CB (placed buffer title 0xde22af580,
  208 bytes; also 0xde22ad930, 0xde22affc0) is filled every frame by a list CopyBufferRegion from a placed buffer over
  the title's CPU-written upload ring (0x2.../0x3a..-0x3c...). The copy is forwarded to D3D12, but nothing syncs the
  title ring bytes into the source stand-in, so the GPU copies zeros (black). Before build 135 we bound stale title bytes
  (noise).
- Build 139 (agA35) COPY_SRC_SYNC=1 syncs the source stand-ins at ExecuteCommandLists: 1700+ copies done, but rt46 still
  shows only 1 of 23 CBV_PLACED stand-ins non-zero and the TAA CB zero at frame 3729. Open question: why the synced
  GPU copy does not land in the CB stand-in (ordering vs the DRAW_PEEK read, or the src/dst GPU VA).
- Build 140 (main) fixes an AV in xs_hash_title_seh (rt45): __try didn't catch a vanished title page; now ReadProcessMemory.
- Not finished: rt47 = build 140 + rt47.env (COPY_SRC_SYNC=2, CPU writes the dst stand-in directly) was stopped at
  start. That is the next cheap check: if the screen stops being black, the data is right and only the GPU copy path is wrong.
- Device losses are back: rt42 (713 s), rt43 (558 s), rt46 (446 s); rt41 and rt44 were clean. Unexplained.
- Runs/logs: /root/rb/rt41..rt46.err.log; envs /root/handoff/rt41..rt47.env; builds /root/handoff/gta6rt-135..140.exe.

## agA36 (sender gen 36, 2026-11-26 afternoon)
- Receiver = the main session. Work files /root/a36 (edit141.py, edit141b.py, msg141.txt).
- rt46 IS INCONCLUSIVE about the COPY_SRC_SYNC=1 GPU copy (Very likely): it died (AV at 0x0, after fence mirrors went UINT64_MAX)
  at ~frame 3730, right after gameplay started. Only 2 frames of TAA copies ([watch] dst #4 and #8); the summary
  "CBV_PLACED stand-ins non-zero: 1 of 23" was printed at submit 128, BEFORE any TAA copy (submit 256 never came); the one
  DRAW_PEEK (frame 3729) is from replaying the first TAA frame. So "the synced copy doesn't land" was never measured.
- Ruled out by reading the code + rt46 (Likely to Very likely):
  * dst VA mismatch: the CB's CBV (device CBV #1), CBV_PLACED, the copy's dst ([watch] list CopyBufferRegion dst) and DRAW_PEEK's
    V# all name stand-in 0xffdc9f00 (rt46). The record is placed once (slot 133 placed #2), not re-placed per frame.
  * "CBV_SHADOW 0xffdc9f00 = title ..." in DRAW_PEEK is just cbv_title_fn's label for the range, not a different binding.
  * placed buffers = arena ranges: binding.buffer = arena.buffer, buffer_offset = va_ = GetGPUVirtualAddress, so
    g_arena_mapped + sg is exactly what the replay's vkCmdCopyBuffer reads. Buffer copies are not rejected in rt44/rt46 (the
    only "[copy] rejected" lines are line 1944, a texture path).
  * Apply order at ExecuteCommandListsX (slot 35): refresh placed buffers -> xs_lc62_apply (src sync) -> CBV refresh -> real
    ExecuteCommandLists. List extras are erased at ResetX (slot 139); recorded ~= done in rt46, so no list is left out.
  * Open: DRAW_PEEK reads the arena on the CPU while the draw is replayed, before that submit's copy runs. It shows an earlier
    frame's copy only once the GPU has finished it.
- Build 141 (c673281b6): LEONIDA_CE_COPY_SRC_TRACE=1 (needs WATCH_RANGE): CSS_TRACE rec / apply (dst stand-in BEFORE, src
  words) / gpu (after a runtime fence behind the submit: LANDED / ZERO / DIFFERENT) / unapplied lines for copies into the
  watch range. xs_queue_signal factored out of xs_schedule_list_writes. LEONIDA_CE_COPY_SRC_SYNC=3 = 2 + the forwarded GPU
  copy dropped for buffer-stand-in destinations (A/B). .tls 0x1459. Built with agA37's uncommitted WIP in the tree (SKIP_GFX,
  LOOP_CAP "pattern:N"); both inert under rt48/rt49.env.
- rt48 = build 141 + rt48.env (rt47.env, COPY_SRC_SYNC=1, COPY_SRC_TRACE=1); rt49 = rt48.env with COPY_SRC_SYNC=3.

## agA37 (sender gen 37, 2026-11-27) - the returning GPU losses
- Work files /root/a37 (edit141.py, msg143.txt, capt.cpp = offline LOOP_CAP + spirv-val over the compute corpus).
- All four new losses are HANGS (TDR; vkGetDeviceFaultInfo 0 addresses; rt47 Aftermath "Hung", 2.9 KB, no warps).
- rt46 + rt47 (Definite for the log facts): compute queue 4 never completed its FIRST submission ("sem queue 4 current 0
  max-signal 6"; rt47 marks it "awaited, signal submitted but not reached"); its checkpoint top AND bottom =
  cs_22383ee2; the direct queue waits on queue 4 => the whole GPU stalls. rt41/rt44 (clean): queue 4 completed 8502 / 731.
  cs_22383ee2 (64 threads, 1 group): s10 = CB[0x4] (CB V# at table+0x40), loop s12 = 0 .. ceil(s10/64) with a buffer
  load + subgroup sum + LDS atomic per iteration => a garbage count (e.g. 0x40b5ac30) = ~2^24-2^26 iterations = TDR.
  Cause of the hang (Likely): that loop with a bad count. Why the count is bad is open (CB source: CBV_PLACED stand-in,
  ring, or GPU-written). rt47 had 0 TAA copies and only 16 COPY_SRC_SYNC copies, so COPY_SRC_SYNC is not the trigger.
- rt42 / rt43 (ALIAS runs): direct queue checkpoint in VS/PS_DistantWindowEmissives_4eb040bb (top+bottom = its marker
  in rt42; rt43 top already at the next cs_87436348). Same pipeline as the rt36 hang, Deferred variant in rt39. Both
  shaders are loop-free (forward branches only; the VS global_load_dwordx3 reads an s_getpc constant table). Queue 4 was
  also behind in both (1579/1585, 575/581), so cs_22383ee2 may be the real culprit there too (Plausible).
  DWE theory (Trust me bro): garbage CB matrices / instance data give screen-sized quads x many instances. In
  rt41/rt44 the CBV_PLACED stand-ins are zero => all positions 0 => degenerate => cheap.
- Build 143 (e60ac2155; 141/142 belong to agA36):
  * LEONIDA_CE_LOOP_CAP items take "pattern:N" (own inner cap; first match wins). Recommended:
    LOOP_CAP=cs_d5613de2,cs_d94af257,cs_f33ac9e9,1:0x80000 (every compute program, 2^19 iterations per loop per
    invocation; counters are cumulative per invocation, so nested loops share the budget). Offline: 1396 of 1536
    fspx_game modules gain a cap, 1396/1396 spirv-val clean. Cold compute compile once.
  * LEONIDA_CE_SKIP_GFX=sub1,.. (default off): draws of pipelines with a matching stage name are left out (after
    DRAW_PEEK/DRAW_SRV); "[hack] SKIP_GFX #k" logs count/instances/indirect. Bisect knob for DistantWindowEmissives.
  * .tls 0x1459.
- rt50 = build 143 + rt50.env (= rt47.env + the LOOP_CAP line above), requested from the main session.
- NEXT: if rt50 still loses the device in DWE -> a run with SKIP_GFX=DistantWindowEmissives and
  DRAW_PEEK=DistantWindowEmissives:0x140:0x160:0x40,DistantWindowEmissives:0x120:0x58:0x10 (VS camera matrix / counts).
  If cs_22383ee2 stops hanging, find its CB (table+0x40, +0x4): probably a per-frame count written through the same
  upload ring -> CopyBufferRegion path as the TAA CB (the agA36 COPY_SRC work).
- rt51 (main session: gta6rt-143 = same md5 as 141 + rt48.env + agA37 LOOP_CAP 1:0x80000, PLACED_BUF_OVER_TEX dropped; cold
  compile, gameplay copies from frame 7967):
  THE TAA CB IS FIXED by COPY_SRC_SYNC=1 (Definite). CSS_TRACE: 28 rec, 28 apply, 28 gpu, 0 skipped, 0 unapplied; gpu: 19 LANDED,
  0 ZERO, 9 DIFFERENT (the DIFFERENT ones = 0xde22ad930, which gets TWO copies per list, so the later one is what shows - expected).
  DRAW_PEEK SpatialUpscaleWithDithering frame 7973, CB stand-in 0xffdd4c00: cb+0x0 = 2560, 1440, 1/2560, 1/1440, 3840, 2160,
  1/3840, 1/2160; cb+0xb0 = 1, 1, 0, 0x1f27, then dither gains cb+0xc0..0xcc = 1/255 each (rt40 had 8, 0, 11.95 = noise).
  So the forwarded GPU copy always worked. rt46/rt47 just never got far enough into gameplay to show it.
  NOT SEEN: whether the world shows. DEVICE LOST ~7 frames after the CBs filled (frame ~7973; last RTDIAG dump 7800 was
  before the copies, so all zero as expected). Then AV at game+0x55d5d2d.
  ("CBV_PLACED stand-ins non-zero: N of 23" is printed only at 2^k / every 1024 submits: stale, don't use it.)
- NEXT: keep COPY_SRC_SYNC=1 in the base env (COPY_SRC_TRACE is optional). The device loss (agA37's area) has to be fixed first;
  then a run that lives >= 300 frames past gameplay with RTDIAG dumps decides whether the scene shows. If still black: the
  next suspects are other CPU-written placed buffers read through copies > 1 MiB (raise the cap) or through views (needs
  per-use syncing like this one), and the TAA input colour (DRAW_SRV s9+0x0 2560x1440 fmt 19 images).
- rt51 (build 143 = 141, LOOP_CAP ...,1:0x80000, COPY_SRC_SYNC=1 + TRACE, no PLACED_BUF_OVER_TEX; main session): STILL
  LOST seconds into gameplay. Queue 4 now advances (max-signal 32), so the cs_22383ee2 cap works (Likely). New direct
  checkpoint: top cs_27004ef4 (a 20-line loop-free clear = bystander), bottom cs_d8b54205 (= the dispatch in flight).
  cs_d8b54205 is a culling kernel with barriers. Its one loop tests a point against planes s15 = 1 .. CB[0x1c4]
  (plane i at CB 0x104 + 16 i, one s_buffer_load per iteration), so a garbage plane count means up to 2^32 iterations.
  The per-invocation cap (2^19) does not save it: every wave of a many-group dispatch runs 2^19 iterations.
  The "LOOP_CAP:" log stops after 64 lines (told < 64), so a missing name does NOT mean "not capped".
- PATTERN (Plausible-Likely): every run that put NON-ZERO bytes into the CBV_PLACED CBs lost the device (rt42/43:
  CBV_PLACED_ALIAS binds the oldest record; rt46/47/51: COPY_SRC_SYNC copies ring bytes in; rt51 CSS_TRACE
  19 LANDED / 26 DIFFERENT / 8 ZERO). Both runs where those CBs stayed all zero (rt41, rt44) were clean: count 0 means
  zero loop iterations. So the counts the CBs now carry are wrong (stale ring bytes, or the wrong ring offset/lap).
- Envs (no new build): rt53 = rt51.env + LOOP_CAP=cs_d5613de2,cs_d94af257,cs_f33ac9e9,cs_d8b54205:0x100,
  cs_22383ee2:0x4000,1:0x80000 (only those two modules recompile). rt54 = rt53.env with COPY_SRC_SYNC=0 and no TRACE
  (A/B for the data trigger: expected clean like rt44).
- agA37 noted that every run with non-zero CBV_PLACED CBs lost the device; rt51 hung in the cs_d8b54205 plane loop (CB+0x1c4).
  My reading of rt51: no wrong bytes in the traced TAA range (the 9 DIFFERENT were several copies into the same dst per frame).
  But there is a TIMING HOLE (Plausible as the hang cause): the source stand-in over the upload ring is ONE buffer for every lap,
  and the ring laps every ~2 frames (TAA src ~9.4 MiB/frame through ~0x111a0000..0x125d0000). With the GPU >= 1 lap behind, a
  later submit-time sync overwrites bytes an earlier GPU copy has not read yet.
- Build 142 (771b3c4f7, on top of A37's 143): LEONIDA_CE_CSS_STAGE_MB=N gives each synced copy a private slot of an N MiB arena
  DEFAULT staging ring (CSS_STAGE: lines); the forwarded copy is redirected to it (a3/a4 replaced after the [watch] lines).
  LEONIDA_CE_CSS_STAGE_AT=record fills the slot at record time. The trace prints "title since record: same / CHANGED".
  rt56 = 142 + rt56.env (rt51.env + CSS_STAGE_MB=64); rt58 = rt56.env + CSS_STAGE_AT=record. Requested from the main session.
- rt52 (143, rt51.env warm): TAA CB correct again (CSS_TRACE 36 LANDED / 0 ZERO / 12 DIFFERENT; DRAW_PEEK 2560 1440 ...). VM shots
  in gameplay: the whole frame is max 1/255. => The TAA constants were NOT the last cause of black. DEVICE LOST at 651 s.
- UPSTREAM OF TAA (from the rt41/rt44 gameplay probes; CB-independent):
  * target0 = 2560x1440 fmt 10 RGBA16F, the busiest target (SSSResolve / TiledLighting, sga_dr, TexturedUnlit, ptfx, ...) is
    uniformly 0x7fff = FP16 NaN in EVERY probe (rt41 3750-5400, rt44 3600-4200) (Very likely). agA33's "target0 all zero" was the
    .ppm view of NaN. NaN through the TAA / tonemap to UNORM gives black.
  * Its inputs are empty (Likely): the 2560x1440 fmt 17 V-buffer (only HairStrandVBuffer draws listed) is zero, and so are the
    TAA SRVs (fmt 19 depth/stencil views, fmt 29, 2304x576 fmt 54) and SSSResolve ud9+0xc0. No world-geometry pipelines appear in the
    top-24 targets, so the world probably goes through a path that never lands (V-buffer by indirect / mesh / compute raster).
  * NEXT for black: find the world V-buffer / depth writer and why it is skipped (rt44 rtdiag: pending-pso 867, variant-skip
    462, no-target 4908 per 60 frames). Put an RTDIAG dump right after gameplay starts (DUMP_FROM ~3100, EVERY 60) so one
    lands before a device loss.
- CORRECTION (agA36): fmt 17 2560x1440 is the HAIR-strand V-buffer (docs/strand_grooms.md), not a world V-buffer.
  The world looks GPU-DRIVEN (Plausible): rt44 has ~430 draws/frame; INDIRECT_SRC/INDIRECT_DRAW lines (rt44/51/52, also after the
  first TAA copies in rt52) show args [3 0 0 0 0] = InstanceCount 0. Chain: culling CBs zero -> zero instance counts -> no world
  -> empty depth -> TiledLighting/SSSResolve NaN -> black. With COPY_SRC_SYNC the culling kernels get real CBs, which is where
  the new hangs start (cs_d8b54205 etc.). A NaN scrub would only show sky/UI. Check INDIRECT args after gameplay in rt58/rt54.
- rt58 (142 + CSS_STAGE_MB=64 + STAGE_AT=record + A37 tight caps; warm): CSS_STAGE pool at arena 0x5e630000; CSS_TRACE 184 LANDED,
  0 ZERO, 61 DIFFERENT (same-dst repeats); all 51 apply lines "title since record: same" => record vs submit timing does not matter
  (the .sum CHANGED count matches other lines). TAA CB correct. DEVICE LOST ~frame 3100 anyway (direct: DistantWindowEmissives,
  compute: cs_c5b1005e, queue 4 sem UINT64_MAX), so the lap aliasing is not the (only) loss cause. Shots s09/s10 = Windows desktop
  after the crash, not a picture. Last logged INDIRECT_SRC args still [3 0 0 0 0] (logging capped early; inconclusive).
- agA37 rt52 (cap 1:0x80000; main session): lost at 651 s. Direct checkpoint cs_56009f29 (screen-space image kernel,
  LOOP-FREE), compute top cs_c5b1005e (43 lines, loop-free bystander). LIST_TIME before the loss: one list (cs_c63f7a7c
  x5, cs_35a0bae6 x5, HeatHaze, all loop-free blurs) took 36 s and then 52 s of GPU time (rt41/rt44 max ~0.12 s, rt51
  1.7 s). Reading (Plausible): async compute waves stuck in capped-but-long loops (2^19 iterations each) hold the SMs, so
  direct-queue work crawls until a TDR. 2^19 per invocation is too high a cap.
- Static census (census2.py: backward branches whose exit compare is a scalar int compare): fspx_game compute 4232
  constant-bound (only 14 x 10000 and 3 x 65535 above 8192, i.e. safety guards) vs 13309 bound by an SGPR (CB/buffer data);
  graphics 17822 constant-bound (48 x 9999 + 4 x 65535 above 4096) vs 15401 SGPR-bound. So a low cap truncates
  almost no constant-bound loop; data-bound loops cannot be measured offline.
- Build 145 (39528ead1): LOOP_CAP_GFX takes "pattern:N" items too. rt59.env = rt58.env + LOOP_CAP=...,cs_d8b54205:0x100,
  1:0x2000 + LOOP_CAP_GFX=PS_MIRROR_PBR,HairStrand,1:0x400 + DISPATCH_LOG=1. Offline: graphics sample 2237/2650 capped,
  spirv-val clean. Cold compile of everything once.
- Existing generic knobs, not used in recent envs: LEONIDA_CE_INDIRECT_CLAMP=1 (GPU pass zeroes indirect dispatch records
  with a dimension > INDIRECT_CLAMP_MAX, default 65535) and DISPATCH_MAX=N (skip CPU dispatches over N groups). Candidates
  if loop-free hangs remain.

## agA39 (sender gen 39, 2026-11-27) - why there is no world: no depth, no MRT
- Work files /root/a39 (edit scripts, msg160/161.txt, osg.py = DXBC output-signature reader).
- FINDING (Definite, from the code): slot 57 (console PSO record -> PC CreateGraphicsPipelineState, d3d12_xs_device.cpp) builds
  EVERY title graphics pipeline from the shaders only. DSVFormat is UNKNOWN (no depth state), NumRenderTargets=1 with RGBA8,
  cull NONE and topology TRIANGLE. agA4 noted "depth/stencil not translated" and it was never revisited. Consequences:
  * prepare_draw attaches a DSV only when pipeline->depth_format != UNDEFINED, so NO draw has ever had a depth attachment.
    No "[rtdiag]   depth" line appears in any log rt4-rt59 (grep count 0). Depth buffers only ever see clears, so TiledLighting /
    SSSResolve reconstruct from cleared depth (Likely the NaN source; NANPROBE will name the pass).
  * render targets past slot 0 are dropped: G-buffer pixel shaders declare several (OSG1 of
    PS_PropFoliageDeferredTo_GBuffer: SV_Target0..2), so G-buffer slots 1+ were never written.
  * depth-only draws (prepass / shadows) render nothing; rt59 frame 10400 has 7832 "no-target" draws.
- variant-skip (rtdiag) = a draw whose attachment variant (unbound slot / RT_RETARGET format) is still compiling in the
  background. It is transient after cache-invalidating env changes (pending-pso is the same for base pipelines). The counts
  fell 29762 -> 9134 within 100 frames in rt59; it is not a cause of the black screen.
- Build 160 (302c00a40), diagnostics, all default off:
  * LEONIDA_CE_NANPROBE=<frame>[:<max>] | auto[:<min frame>[:<draws>]] (+ LEONIDA_CE_NANPROBE_TARGET, default 0x100001b00 =
    rt59 target0). While one frame is built, 8 texels of the target are copied to a 2 MiB arena region after every draw into
    it, every dispatch / ExecuteIndirect, clears/copies into it and at each list start (vkCmdCopyImageToBuffer with layout
    round trip from the tracker). 12 frames later "[nanprobe] #i list L cmd C <B|D|C|I|X> <pipeline> ...: k/8 bad" lines
    (changes only) + "[nanprobe] summary ... first bad #i". auto arms 2 frames after the first frame with >= <draws> draws
    into the target. Code: runtime/src/ce_nanprobe.h + detail::nanprobe_rt in vulkan_replay.h.
  * RTDIAG dump: "bindings: dsv-bound / depth-dropped / mrt-dropped" + top pipelines of no-target / depth-dropped / mrt-dropped.
  * LEONIDA_CE_PSO_STATE_LOG=<file>: one hex line per distinct (shader pair, blob #0), the data for decoding the console
    depth/stencil/raster state (+0x3c "db" values seen: 0x10, 0x12, 0x14 (PS_Copy_SRV_To_Depth), 0x18 (all Scaleform)).
    Blob #0 fields seen in the 6 old dumps: +0x100..0x11f = SX_MRT0..7_BLEND_OPT (0x06000600 = BLEND_DISABLED,
    matches cb0 0x80000000), +0xc0/+0xc4 0x0100ff00 = DB_STENCILREFMASK(_BF)-like, +0x78 0xf = CB_SHADER_MASK-like.
- Build 161 (ee2ec3913), the fix attempt, default off:
  * LEONIDA_CE_PSO_MRT=1: NumRenderTargets from the PS DXBC output signature (highest SV_Target register + 1), with a mask
    and blend per target from CB_TARGET_MASK / CB_BLENDi_CONTROL. RTV formats are RGBA8 placeholders that RT_RETARGET fixes.
  * LEONIDA_CE_PSO_DEPTH=1: test = db bit 1, write = db bit 2 or a PS with SV_Depth (Plausible decode), func =
    LEONIDA_CE_PSO_DEPTH_FUNC (ge default). The replay gives a pipeline with depth state but no DSV format
    (GraphicsPipeline::ce_depth_any) a variant whose depth/stencil attachment format = the bound DSV's VkFormat (variant
    key bits 16..31). Emit takes the aspects from the bound image.
  * .tls 0x1459 for both builds.
- Runs requested from the main session: rt80 = 160 + rt80.env (rt59.env + NANPROBE=auto:9000:150 + PSO_STATE_LOG);
  rt81 = 161 + rt81.env (rt80.env + PSO_DEPTH=1 + PSO_MRT=1). Proposed rt82 = rt81.env + PSO_DEPTH_FUNC=le if rt81 draws
  depth but stays black. Results not seen by agA39.
- NEXT: (1) rt80: read the nanprobe list (first D/C after which target0 is NaN) and the census (how many draws bind a DSV;
  which pipelines are mrt-dropped). (2) rt81: depth lines present? target0 still NaN? (3) Decode blob #0 from
  psostate_rt8x.txt: correlate db/+0x28/+0x38 with pass names (prepass, GBuffer, shadow, Copy_SRV_To_Depth, fullscreen),
  then translate the real depth func / stencil / cull. (4) The world is still GPU-driven (agA36: INDIRECT args [3 0 0 0 0]);
  the culling compute reads depth (HiZ from the previous frame's depth). Empty depth may be WHY instance counts are 0, so
  depth can fix both.

## agA38 (sender gen 38, 2026-11-27) - shader prebake

Goal (user): stop paying for shader compiles during boot and gameplay. Builds 150, 151 and 152 (commits ea51710a5, aaf35ca2c, 86b9ddd6b).
All switches default off.

Where things are (runtime/shims/d3d12_vulkan_device.cpp):
- Compute: create_compute -> pick_entry -> library->spirv -> compute_transforms (LOOP_CAP, VSHARP_GUARD_ALL with the slot
  hashed from the name, VA_GUARD; moved out of create_compute without changes) -> layout (push 144 B, plus the bindless
  set 0 layout when the module has bindings) -> subgroup_size_for -> AsyncCompute thread (40 slots). The title waits 5 ms,
  then gets an empty placeholder pipeline. compute_skip = the SKIP_CS and kNeverCompile lists.
- Graphics: create_graphics only stores a Recipe. variant(key) builds on first draw on compile_pool (half the cores). Until
  that build finishes, the replay skips the draw (rtdiag "pending-pso"). key = unbound slots + RT_RETARGET format
  overrides. One D3D12 PSO can have several Vulkan variants.
- The pipeline cache is pipeline_cache.bin next to the exe. PipelineCacheWriter saves it every 60 s if it grew, and save_now
  saves it at once (agA38).
- The "bg compiles N avg ms" figure the main session quotes is not in the code. Plausible: it is computed from the
  "[hack] background compute X: 0 after N ms" lines.

Switches:
- LEONIDA_CE_PSO_RECORD=<file>: appends, in first-use order, "G <record> <variant>" (graphics, at the variant's first draw)
  and, from build 151, "C <module>" (compute, at first creation). A record is the D3D12 description field by field as hex
  u32s, plus the input elements, plus the stage programs as FSSM SHA-256. Pointers, the root signature and CachedPSO are
  left out because create_graphics doesn't read them.
- LEONIDA_CE_PSO_PREBAKE=<file>: started at the title's first PSO. The jobs are the file's descriptions and modules in file
  order, on LEONIDA_CE_PREBAKE_THREADS threads (default: every core) at the LOWEST priority (151; 150 used below-normal
  and built graphics first, then all compute). A graphics job runs create_graphics on a prebake PipelineState
  (programs by hash, no arena copy, not registered) and build()s every recorded variant. The pipelines go to
  DeviceState::ce_prebaked, and a title PSO with the same record takes them at its first draw ("adopted"; the PSO
  destructor doesn't destroy those). Prebake PSO objects stay alive until ~Device, so their layouts outlive the
  pipelines. The re-encoded record must equal the file's ("re-encode mismatch" must be 0).
  LEONIDA_CE_PSO_ADOPT=0 only warms the cache.
- Demand bump (151): if a draw needs a variant whose job is queued, the job moves to the front and the draw waits up to
  LEONIDA_CE_PREBAKE_WAIT_MS (default 100). If the variant is still not built, the draw is skipped once and no second
  build starts. A compute module is moved to the front with no wait (LEONIDA_CE_PREBAKE_WAIT_CS_MS, default 0), then the
  normal path runs.
- LEONIDA_CE_PREBAKE_COMPUTE=1 (or a substring list): every translated compute module. In 151 this only applies when the
  list has no C lines or there is no list. The library has 2689 main cs, 18 fspx/, 52 bvh/ and 1536 fspx_game/
  modules: 4295 in all.
- The prebake must use the same transforms as the run. It uses this run's env, so it is right by construction, but only
  with GUARD_SLOT_HASH on (the default). With =0, slots follow creation order and the SPIR-V differs.

Offline numbers (this Linux box, RTX 6000 Ada, host NVIDIA driver, runtime/test/vk_pipeline_check built for Linux at
/root/agA38/vk_pipeline_check, 64 threads, __GL_SHADER_DISK_CACHE=0). The rt59 compute transforms were applied by
/root/agA38/xform.cpp, which reproduces LOOP_CAP and VSHARP_GUARD_ALL:
- fspx_game, 1536 cs, raw: 5881 CPU-s, 3.8 s mean, 113 s wall.
- fspx_game, transformed (1396 capped): 7512 CPU-s, 4.9 s mean, slowest 125 s, 141 s wall, 28 GB peak RSS.
- main FSSM cs, 2689 transformed: not finished after 41 min wall; stopped at ~2550. 729 modules took over 60 s each
  (92,818 CPU-s in those alone, max 757 s), and 680 of those are *_CS_RaytracingResolve_*. ScreenProbes_Activate* modules
  were still compiling after more than 20 min; kNeverCompile already skips some of them. Peak RSS was 250 GB at 64 jobs.
  A full eager compute bake is far too much, even on a 128-core host.
- The VM's Windows driver differs, so these are only a guide (Likely within 2x).

Expectations (Plausible): a cold compute cache costs hours of CPU time. Building only what was used, at lowest priority
and in first-use order, is what makes a LOOP_CAP change cheap. Graphics adoption removes pending-pso for every recorded
variant once its job is done, and the bump covers the start.
rt70 (build 150, eager PREBAKE_COMPUTE=1, all 4295 modules on every core): the title never reached gameplay. The boot
starved and stayed in the menu pace up to frame 13500. RaytracingResolve modules took 32-189 s each on the VM, and there
was no "done" line in 1500 s. The PSO record was only 9 lines. No device loss.
-> Build 151 (lazy, what the user asked for): only recorded first-use modules and variants, at lowest priority, with a
demand bump. Build 152: RaytracingResolve deferred to the end of the queue (LEONIDA_CE_PREBAKE_LAST), threads = a
quarter of the cores.
Runs requested: rt70 (150: all compute + record), rt71 (150: eager graphics prebake), rt72 (152: lazy graphics from rt70's
list, writes the first list with C lines), rt73 (152: full lazy from rt72's list).
Next: read rt72/rt73. If gameplay arrives, re-run with a changed LOOP_CAP (cold cache) and PSO_PREBAKE from the rt73
list, to measure the case this was built for. Plausible extras: a per-module "slow list" that the prebake never builds
and that create_compute always gives a placeholder (rt70 + offline: RaytracingResolve), and capping concurrent large
compiles by memory (offline peak was 250 GB).

## agA40 (sender gen 40, 2026-11-27) - time from launch to gameplay
Measured from warm logs (rt52 build 143, rt58 build 142, rt44). rt59 was a cold compile (919 s to 11->0).

| phase (boot state) | rt52 t (s) | s | cause |
|---|---|---|---|
| pre-init (0) | 0-52.8 | 53 | engine init, shaders.rpf + core_bvh4 (253 MiB DS), ~1300 background compute PSOs (warm cache); frames 0-1080 at ~20 fps. With a cold cache, 0->1 comes at 250-360 s |
| state 1 | 52.8-83.7 | 31 | the title polls input (~22/s), 2.7 s per 60 frames, DS 253->390 MiB |
| state 3 = landing, PLAYER (A) | 83.7-93.4 | 10 | ~8 s of it is the script (A@s3+8). In warm runs 3->13 took 8.8-48.8 s (avg ~19) because the first A is often missed and the retry comes 10 s later |
| state 13 = the load loop (0x1421b7530, NOT the landing) | 93.4-123.0 | 30 | DS 390->585 MiB in ~7 s, then ~20 s at 17 fps. 13->0 does not follow a press (rt45: 6.9 s after the last A) |
| state 0, post-load | 123-218 | 95 | GPU idle (LIST_TIME: 20-100 ms GPU per 1024 lists), DS idle (626->687 MiB), 4-5 fps, the title reads no input. A stall of 2 s or more on every frame that is 29 mod 30 = the placed-texture refresh pass (below) |
| save flow 0->10->0->11->0 (or 0->9->10->11->0) | 218-226 | 8 | autosave list/submit |
| intro + credits, world streaming | 226-~320 | ~95 | DS 687->2756 MiB, 2-4 fps; gameplay frames ~3000-3400. The refresh stalls continue (rt52 SAMPLE 10-28) |

- Ruled out: the timer resolution. SDL_InitTicks calls timeBeginPeriod(1), so the title's Sleep(1) poll loops (one is
  game+0x1e10183..0x1e101a1, where the main thread sits in the post-load phase) sleep about 1 ms. The diagnostic flags cost
  nothing measurable between runs (main session's rt40-rt58 table). The DOWN presses land in the load loop (13), where
  nothing reads them; they are harmless.
- HOT SPOT (Likely): xs_refresh_placed_textures runs inside every 30th PresentX (d3d12_xs_device.cpp ~4861). In rt52, rt58
  and rt44, every "[hang] SAMPLE" between 13->0 and the save flow is on a frame that is 29 mod 30 (rt52: 2069, 2099 ... 2309),
  and every thread is waiting at that moment. From frame ~2040 a pass covers 1253+ textures, with one ReadProcessMemory
  per 4 KiB sample plus one VirtualQuery per 64 KiB page (commit_physical_range).
- Build 170 (62d97faa4): hash reads in 64 KiB chunks (LEONIDA_CE_REFRESH_CHUNK_KB, 0 = old; same samples, same hash);
  commit only when xs_readable fails; "[refresh] pass N: textures, MiB, commits, uploads, ms" at powers of two, for passes
  over 250 ms, and for every pass with LEONIDA_CE_REFRESH_LOG=1.
- Press script: PRESS=A@s3+1/3x8,A@s3+30/10x40,DOWN@s3+12/10x3 (rt90.env). It presses PLAYER as soon as the landing is up,
  retries every 3 s, and keeps the old 10 s A cadence for the rest of the run.
- Runs requested: rt90 (build 170, rt59.env + the new PRESS) and rt91 (rt90 + NO_PLACED_REFRESH=1 = the upper bound of
  the refresh cost).

## agA41 (sender gen 41, 2026-11-27) - the real console depth/stencil decode
- Work files /root/a41 (rows.py = psostate loader, ed1/ed2.py = the edits, msg180.txt).
- FINDING (Definite): blob #0 +0x3c is NOT DB_DEPTH_CONTROL. +0x38/+0x3c and +0x4c/+0x50 are SPI_SHADER_PGM_RSRC1/RSRC2 pairs
  of two shader stages (RSRC1 0x082c03xx = FLOAT_MODE 0xc0 + DX10_CLAMP; RSRC2 0x10/0x12/0x14/0x16/0x18 = USER_SGPR 8..12, the
  same values at +0x3c and +0x50). So agA39's PSO_DEPTH (build 161, rt81) was driven by user-SGPR counts: blits got a depth
  test, UI got depth writes.
- FINDING (Very likely, 11210 rt80 blobs against pass names): GCN context registers in blob #0:
  * +0xb8 DB_DEPTH_CONTROL (bit0 stencil, 1 Z test, 2 Z write, 4-6 ZFUNC, 7 backface, 8-10 STENCILFUNC, 20-22 STENCILFUNC_BF;
    func 0 never, 1 less, 2 equal, 3 le, 4 greater, 5 ne, 6 ge, 7 always = D3D12 func - 1).
    0x007007f0 off: Scaleform, blits, BlurFilter, PostFX, TexturedUnlit_IM, sga_dr (mostly), BlitHDRGameScreenshot (6422 rows)
    0x007007e2 GE test-only: VfxVBuffer, ptfx, AlphaPrepass, DistantWindowEmissives (1470)
    0x007007e7 GE + write + stencil: HairStrandVBuffer/GBuffer, Octa, RopeTransformD+PS_DeferredTextured
    0x007007a2 EQUAL test-only: VS_TiledLighting/vs_classfied_fullScreen + TL_PS_* / PS_MIRROR_PBR_* / SSSResolve (material-depth
      classification: a full-screen quad per material at depth = id)
    0x007007f6/7 ALWAYS + write: ps_generate_material_mask, DownsampleMin/MaxDepth, VS_Blit (no PS), Copy_SRV_To_Depth (f7)
    0x007007b6 LE + write: CascadeShadows (forward Z for shadows; the main view is reversed Z)
    0x002002f1 stencil EQUAL, no Z: Scaleform masked Cxform (940); 0x00200291 PS_BlitColour (stencil-masked blit)
  * +0xbc DB_STENCIL_CONTROL (fail, zpass, zfail, then _BF, 4 bits each; 0x00030030 = zpass REPLACE).
  * +0xc0 / +0xc4 DB_STENCILREFMASK / _BF (testval 0-7 (0 in the blob; the ref is dynamic), mask 8-15, writemask 16-23, opval 24-31).
  * +0xa0 PA_SU_SC_MODE_CNTL: 0x00201a40 (no cull, poly offset front+back enabled, MULTI_PRIM_IB), 0x42 cull back, 0x41 cull
    front, 0x46 cull back + FACE (CW front). Most pipelines are cull none.
  * Also Plausible: +0x68 DB_SHADER_CONTROL (0x10 EARLY_Z_THEN_LATE_Z, 0x50 + KILL, 0x11 Z_EXPORT on DownsampleMinDepth),
    +0x70 SPI_SHADER_COL_FORMAT (4 = FP16_ABGR per target, 0x441 = 3 targets), +0x78 CB_SHADER_MASK.
- FINDING (Definite, code): rt81's "depth-dropped == dsv-bound" was a census bug. note_binding looked at the base pipeline's
  depth_format, which stays UNDEFINED for PSO_DEPTH pipelines even when their variant attaches the DSV.
- The world G-buffer pipelines barely appear in psostate (GBuffer-ish names: ~120 of 11210). The main world is rendered as a
  visibility buffer + compute + tiled lighting; target0 is mostly drawn by sga_dr (depth off), TiledLighting/SSSResolve (EQUAL),
  ptfx and DistantWindowEmissives (GE test).
- Build 180 (9dd08e6e3): LEONIDA_CE_PSO_DEPTH=1 translates DB_DEPTH_CONTROL + stencil; LEONIDA_CE_PSO_DEPTH_FUNC only
  overrides when set (never EQUAL/ALWAYS); LEONIDA_CE_PSO_STENCIL=0 = no stencil; LEONIDA_CE_PSO_CULL=1 (default off) = cull +
  winding. Census: "bindings: dsv-bound, depth-attached, depth-off, depth-dropped, mrt-dropped". .tls 0x1459.
- Stencil ref: the replay sets it from the command list's OMSetStencilRef (d3d12_com.cpp). Plausible that the title's refs come
  through that; if Scaleform masks or BlitColour vanish, A/B with LEONIDA_CE_PSO_STENCIL=0.
- Runs requested: rt100 = 180 + rt81.env renamed; rt101 = rt100 + PSO_CULL=1.
- rt81 (161, wrong decode; stopped at ~frame 4100): mrt-dropped 650-1100 (PSO_MRT works), 106 depth lines, still black.
- rt100 (180; stopped at ~1150 s, frame 2100, cold compile: pending-pso 16513 at frame 2000, 130-144 s per 60 frames, no
  device loss): "dsv-bound 42504, depth-attached 42504, depth-off 0, depth-dropped 0, mrt-dropped 717". The decisions look
  right (blits off; sga_dr GE test-only; DownsampleMinDepth ALWAYS+write; BlitColour stencil only). "func N" in the log is
  D3D12_COMPARISON_FUNC (1 never .. 8 always). It did not reach gameplay, so there was no nanprobe and no picture.
- rt102 = build 190 (main session: 180 + a title FPS thread) warm re-run of rt100.env.
- Build 171 (13aa30e9c): the refresh pass's re-uploads go in one upload buffer and one submission (rt52's log implies ~23000
  re-uploads in the run, each with its own buffer/allocator/list/fence/ExecuteCommandLists). LEONIDA_CE_REFRESH_BATCH=0 = old.
- CORRECTION from rt100 (build 180 contains 170/171), whose "[refresh] pass" log measured the pass: the 64 KiB whole-chunk reads of
  build 170 were a REGRESSION. ReadProcessMemory moves only ~200 MB/s here: 53 textures / 252 MiB = 1.28 s per pass already in
  the splash (passes 3-14), and 1214 textures / 4 GiB = 8.4 s after the load, on every 30th present. rt100: 0->1 61.9,
  1->3 96.4, 13->0 319.9, 11->0 397.6. The batching works.
- Build 173 (5414f8ef1): CHUNK_KB defaults to 0 again (one 8-byte read per sample). Non-glyph textures get one sample per 64 KiB
  physical page (LEONIDA_CE_REFRESH_STRIDE_KB); glyph caches are still hashed in full. A 40 ms budget per pass with a rotating
  cursor (LEONIDA_CE_REFRESH_BUDGET_MS, 0 = none). Plus LEONIDA_CE_PROF=1: a "[prof]" line every 60 presents with wall ms/f,
  pacing sleep, refresh, proxy total and the top proxy slots (inclusive, summed over threads). rt93 = rt102.env + PROF=1.
- The GPU box hard-reset at ~18:38 box time and left empty git objects for an agA40 commit (9e38f054e). They were moved to
  /root/a40-git-empty, experimental was reset to d352c6e58, and the work was committed again as 5414f8ef1. Two stale reflog
  lines still name 9e38f054e.
- Warm 0->1 is not compile-bound: all 1313 background compute PSOs finish before 0->1 (p50 16 ms, p99 6.5 s = the bvh ones).
- rt103 (main session: build 173 = 180 + agA40, rt103.env): gameplay at t=309 s, frame ~5880. Frame 5900: "dsv-bound 17201,
  depth-attached 16194, depth-off 1007, depth-dropped 0, mrt-dropped 102". target0 is STILL all NaN (7fff7fff, 2764800/2764800).
  DEVICE LOST right after frame 5900. Every vkGetDeviceFaultInfo address is type 4 (instruction pointer unknown = a
  timeout-like loss, not a page fault). Checkpoints: direct queue TexturedUnlit_IM, compute cs_c5b1005e / cs_344692c2 (rt52
  had the same bystander). Then a 0xc0000005 at game+0x5562369. rt80 (no PSO_DEPTH) ran to frame 10200 with no loss, so depth
  is a suspect (Plausible: real depth lets HiZ/GPU culling produce non-zero instance counts, so far more work per frame), but
  A40 build-173 changes are also in the build.
- IMPORTANT: in rt103 target0 moved to T# 0x100001c00 (it was 0x100001b00 in rt59/rt80/rt93). The default
  LEONIDA_CE_NANPROBE_TARGET (b00) would probe nothing there. /root/a41/nanprobe-auto-target.patch (NOT built or committed)
  adds NANPROBE_TARGET=auto: it locks onto the first 2560x1440 RGBA16F target drawn into from <min frame> on.
- HANDOFF / what agA41 would try next:
  (1) A/B the device loss: build 173+ with rt103.env vs the same with LEONIDA_CE_PSO_DEPTH=0. If only depth loses the device,
      bisect with LEONIDA_CE_PSO_STENCIL=0, then DEPTH_FUNC overrides, then SKIP_GFX on the heavy depth-tested pipelines.
  (2) The NaN: NANPROBE=auto:5850:100 with NANPROBE_TARGET=0x100001c00 (or apply the auto patch) so it arms in the first
      gameplay frames, before the loss (~frame 5900+). The rt80 probe said NaN from the first command (carried over), so a
      clear of target0 at frame start (or of the TAA/history buffers) is the next A/B: it shows whether a pass regenerates the NaN.
  (3) Decoded but untested: PSO_CULL=1 (PA_SU_SC_MODE_CNTL); stencil ref comes from OMSetStencilRef (Plausible).
  (4) Untranslated: depth bias (PA_SU_SC_MODE_CNTL has poly offset enabled on most pipelines, but the offset registers are not
      located yet) and depth bounds (DB_DEPTH_CONTROL bit 3, unused in the rt80 data).

## agA42 (sender gen 42, 2026-11-27) - summary written by agA43 (agA42 left no section)
- Builds 200 (FRAME_CLEAR), 201 (+FRAME_TRACE_AUTO, NANPROBE_TARGET=auto locks on the TiledLighting 2560x1440 RGBA16F target),
  202 (+LIST_SLOT_STATS). The tree was committed by agA43 as 1a23e04e3 (= build 202).
- rt200 (173 + SKIP_CS cs_8513f5a0, c5b1005e, 344692c2): device lost at 562 s (direct VS_Blit). A42 found cs_8513f5a0 writes NaN
  over all of target0 at the end of the frame in rt80.
- rt201 (201, FRAME_CLEAR=f16 + SKIP_CS cs_8513f5a0, 720 s): nanprobe frame 5611 target 0x100001b00: 2974 samples, ALL CLEAN, every
  px 0 - with the per-frame clear target0 is 0, not NaN, and nothing in that frame writes a non-zero texel. Still black.

## agA43 (sender gen 43, 2026-11-27) - the world is mesh shaders; their PSOs were never built
- Work files /root/a43 (ed*.py edits, msg*.txt, match6.py / psomatch6.py, vm/ = VM psomap + want copies, dump/ = record dumps).
- FINDING (Definite, rt203 LIST_SLOT_STATS=60): the "gameplay" frames after boot state 11->0 (frame ~5000-5900) are a near-empty
  frame (~10 list calls per frame). The real scene starts at frame ~5560-5940: 3000-4000 Dispatch, 2000-3400 ExecuteIndirect,
  2600-3400 SetPipelineState per frame. The pace drops to ~2 s per frame there. Every earlier "gameplay" probe (nanprobe at
  5611 etc.) looked at the empty frame. The device losses of rt103/rt200/rt204 came right at the scene start.
- FINDING (Definite, MESH_EI=1 in rt205): the scene's geometry is ExecuteIndirect with a DISPATCH_MESH (type 10) command
  signature, ~1100 per frame, every one with the slot 57 PLACEHOLDER PSO bound. Slot 41 had turned DISPATCH_MESH signatures
  into DISPATCH, so each such EI re-ran the last-bound compute PSO with the mesh arguments as group counts.
  rt205 (MESH_EI=2: those EIs skipped): no device loss in 1200 s (frame 6384); rt204 (same env without the skip) lost the
  device at frame 5652. (Likely: the wrong compute re-runs caused the scene-start losses; 1 run each.)
- FINDING (Definite, rt206/rt207 dumps): the console pipeline record holds 8 (ptr, size) pairs, not 6. NGG pipelines
  (PS_DeferredTextured*, PS_Cable*, CascadeShadows, AlphaPrepass, LinearDepth, PuddleDetect, ...) have #2 = PS code,
  #3 = its table, #4/#5 empty, #6 = the geometry-stage code, #7 = its table. #6 is a corpus "ms" program
  (e.g. _MS_VS_TransformD_5a4fa379 for PS_DeferredTextured_5a4fa379, MSNC_VS_Cable_*, MST_DS_PNTri_*). Slot 57 looked at
  #2/#4 only -> "no vertex stage" -> placeholder. 6325 MS headers seen so far, all matched (stage ms) by psomatch.
- Builds (commit 0d7b2e259 = build 209; 206-208 intermediate):
  * Slot 57: if #2/#4 give no VS, blob #6 is looked up (ms or vs); unknown -> want list. A mesh record is created with
    ID3D12Device2::CreatePipelineState (MS + PS + the same translated blend/depth/raster/MRT state). "[hack] MESH_PSO".
  * IndirectKind::mesh for DISPATCH_MESH signatures; ExecuteIndirect records a mesh Draw with the indirect buffer; the replay
    emits vkCmdDrawMeshTasksIndirect(Count)EXT. Console type 105 stays DISPATCH (no DISPATCH_MESH retry).
  * Mesh draw vs non-mesh pipeline: draw left out ("MESH_MISMATCH") instead of failing the list.
  * LEONIDA_CE_MESH_EI=1 log / 2 skip type 10 / 3 skip 10+105 (build 204+). PLACEHOLDER_PSO diagnostic + nvsNN dumps.
- VM psomap: C:\zroot\root\leonida-psomap.txt now = the 11/24 map + 6325 ms entries (/root/a43/vm/leonida-psomap.new.txt);
  backup C:\zroot\root\leonida-psomap.a43bak.txt. The want file was not re-run against the corpus since 11/24 before this.
- rt209 (209 + full map, 1800 s, stopped at ~1250 s): boot 0->1 at t=514 s (was 63 s: creating ~6300 mesh PSOs costs ~60 ms
  each on the title's loading thread, Likely in create_graphics' per-stage SPIR-V work), scene from frame ~13900.
  rtdiag 14120: 48261 draws / 20 frames, pending-pso 0, variant-skip 10. The G-buffer pass runs: PS_DeferredTextured*,
  PS_DeferredPedTexturedNC*, terrain, ~1150 mesh draws per frame into the 2560x1440 RGBA8 G-buffer targets, plus
  PositionOnly / LinearDepth depth prepasses (no-target). BUT every G-buffer target and the depth buffers stay 0 bytes;
  screen still black. No device loss in 1250 s.
- FINDING (Definite): "[rt] d3d12: no translated compute shader for FSPX kind-6 and kind-0" x1144 in every run (rt203-rt209):
  1144 of the title's compute programs have no SPIR-V and run as EMPTY pipelines. The VM dumped all 2060 containers
  (C:\zroot\root\leonida-dump\cs_*.dxbc, copied to /root/a43/csdump); only 916 are in spirv/fspx_game (index from 11/24).
  Translating them (/root/a43/csnew, fspx_unpack 16-way, slow: ~9 s per program for llvm-mc): 1 in 20 had a 24-byte
  prologue (fixed in tools/fspx_unpack.py, 66bcf3d8e); 19 in 20 still hold words llvm-mc gfx1030 rejects (SMEM
  e.g. 0xf4281a26 0x00000074 = s_buffer_load_dwordx4 with sdata s104 (Plausible), MIMG 0xf09c0708) and rdna2spirv stops at
  "fall-through into undecodable words". Sample of 20: 1 translated. This is a translator task (decoder for those
  encodings), not done.
- rt211 (build 210, INDIRECT_SRC logs for mesh draws): every world mesh EI record reads stand-in [x=0 y=1 z=1] (32-byte
  stride, placed buffer at title 0xde226a000, title CPU bytes 0): the GPU arg writer runs (y = z = 1 come from it through
  the D3D UAV / stand-in) but writes ZERO groups. So the world draws are issued with nothing to draw. Candidates: the
  count comes from culling/visibility compute that is among the 1144 empty pipelines, or culling inputs are wrong
  (HiZ / instance lists / camera; raw title-VA writes into the POOL_GPU=1 window are dropped). Open.
- Build 211 (LEONIDA_CE_MESH_FORCE_X=K, default off, HACK): each indirect DISPATCH_MESH record is drawn as a direct
  vkCmdDrawMeshTasksEXT(K, 1, 1) (up to 64 records per EI). Hides the real group count; a bisect knob for "does anything
  rasterize at all".
- rt212 (211 + MESH_FORCE_X=16): presents stopped at frame 13101 right as the scene started, DEVICE LOST ~60 s later; direct
  checkpoint = a forced _MS_VS_TransformD_c5fd36bb + PS_DeferredTextured draw (Definite). Forcing groups makes the mesh
  shaders run on whatever per-group data exists (none) -> hang. Keep MESH_FORCE_X off.
- Invalid-word census over 300 of the 1144 untranslated disasms (llvm-mc gfx1030): SMEM loads whose sdata range ends past
  s105 (s_load_dwordx8/x16 / s_buffer_load_dwordx4/x8 into s92..s107; e.g. 0xf40c190a 0xfa000d60 = op 3 sdata s100) and
  MIMG ops 36/39/47 (0xf09c0708 ...). After the first rejected word llvm-mc loses sync and the rest is garbage.
  Plausible: the console compiler uses s104..s107 (VCC aliases) as plain SGPRs. A translator fix = decode those words in
  tools/shader_disasm.py / rdna2spirv.py instead of relying on llvm-mc, with an SGPR file of 108.
- Translation attempt of the 1144 (fspx_unpack 66bcf3d8e + rdna2spirv --wave64-emulate, 16 shards): 9 of 1144 translate
  (/root/a43/spirv_new, NOT deployed). Failures: 934 fall-through into undecodable words, 90 CS thread-group-size SGPR,
  26 s_bitreplicate on a lane mask, 23 ds_append, 20 branch into data, 9 cannot re-anchor, a few v_cmp / mask
  destination and v_fma_mix field drops. Inputs kept: /root/a43/csnew (containers), /root/a43/fspx_new (unpacked + disasm).
- STATE / HANDOFF (agA43, Very likely for the chain, not proven end to end):
  world = mesh pipelines (fixed: built, drawn) -> their indirect args say 0 groups -> the GPU-driven count is not
  produced -> no G-buffer, no depth -> TiledLighting has nothing -> black. Most likely the count writers (culling /
  visibility compute: CPU writes [0 1 1], culling bumps x) are among the 1144 empty compute pipelines.
  NEXT: (1) teach the disassembler path the rejected SMEM (sdata up to s107) / MIMG op 36/39/47 encodings and the other
  translator gaps above, translate csnew, deploy to C:\zroot\root\leonida-work\shaders\spirv\fspx_game (back up index.json),
  re-run with build 210 + rt211.env; (2) find which compute writes the mesh arg buffer (title 0xde226a000, stand-in
  0xffde4f00 in rt211) - a DISPATCH_LOG / FRAME_TRACE on the scene frame (~13950) and check it against missing_cs.txt
  (/root/a43/missing_cs.txt = the 1144 FSPX code hashes); (3) speed up mesh PSO creation (0->1 at ~500 s).
  Hacks in the recommended env: FRAME_CLEAR=f16 (hides carried-over NaN, clears RGBA16F >= 1280 wide at first draw),
  SKIP_CS=cs_8513f5a0 (A42: it writes NaN over target0), SKIP_GFX=DistantWindowEmissives, LOOP_CAP / LOOP_CAP_GFX,
  INDIRECT_CLAMP (from rt103.env). MESH_FORCE_X must stay off.

## agA44 (sender gen 44, 2026-11-27) - the 1144 untranslated compute programs: 1141 translate now (offline)
- Work dir /root/a44: fx/ = translator input (fspx/ copied from /root/a43/fspx_new/fspx, disasm/ = python-decoded
  listings), fx2/ = the 94 that failed the first pass, sp5/ = their SPIR-V, index.merged.json (VM index + 1050),
  index.merged2.json (+91 = 2677 entries), pydis.py, tr.sh (N shards, nice 19), deploy_next.sh, ed*.py edits.
- ROOT CAUSE (Definite): llvm-mc was the problem, not the encodings. The game's pipeline-library CS are gfx1013-style
  code: (1) v_mad_f32 / v_mac_f32 (VOP3 0x141, VOP2 0x1f), which gfx1030 removed - A43 used fspx_unpack's default
  --mcpu gfx1030; (2) SGPR tuples that run into vcc_lo/vcc_hi (s106/s107) as ordinary SGPRs: s_load_dwordx8
  [s100..s105,vcc_lo,vcc_hi], an S# in [s104,s105,vcc_lo,vcc_hi], SMEM base vcc, v_cmp_*_e64 vcc_hi. LLVM 21
  rejects (2) on every CPU (LLVM 23 prints them as VCCTuple lists, which shader_ir already parses). The "exp
  invalid_target_22" / "SMEM rejected" words in A43's census were desync after the first rejected word. The
  repo's pure-Python decoder (tools/rdna2_decode.py, #1901) with mcpu gfx1013 decodes all but 3 programs cleanly,
  in 6 s for all 1144 (llvm-mc took hours).
- Translator changes: tools/fspx_unpack.py --backend python (+ grows the code range past the first s_endpgm while
  a branch targets a long-branch trampoline after it: 30 programs); rdna2spirv: wave32 vcc_hi = s107 (operands
  renamed, %vcc_hi aliased to %sgpr[107]), SMEM base vcc = [vcc_lo, s107], COMPUTE_PGM_RSRC2.TG_SIZE_EN (SGPR after
  the group ids = wave id << 6 | waves in group; Mesa layout, Likely), s_bitreplicate_b64_b32 of a constant into a
  wave64 mask, s_flbit_i32_b64 of a wave64 lane mask, ds_append/ds_consume (LDS; M0 taken as 0 - Plausible, the
  programs never write M0). Tests updated: test_salu_flbit_64, test_lds_basic_alloc (both asserted the old refusals);
  113 + 687 related semantic tests pass.
  Recipe: fspx_unpack.py <dxbc...> --out DIR --disasm --backend python --mcpu gfx1013; rdna2spirv --wave64-emulate.
- Result: 1141 of 1144 translate and pass spirv-val (3 left: the 3 with decoder-invalid words, reported as
  v_max_f32_dpp "field dropped" at 0x13c - a decoder desync, not looked at). NOT GPU-verified.
- Deployed to the VM (for rt213): 1050 .spv + merged index; original index kept as fspx_game\index.a44bak.json.
  NOT deployed yet: the 91 in /root/a44/sp5 -> run /root/a44/deploy_next.sh (expects 2677 .spv), then rt214
  (gta6rt-210.exe + /root/handoff/rt214.env = rt211.env family with WATCH_RANGE on the mesh arg buffer
  0xde226a000-0xde2272000). C:\zroot\sp3.tar (3 GB) was left on the VM disk: delete it.
- rt213 (210 + rt213.env, 1050 new CS): NO DATA. The host root disk hit 100% (my ~12 GB of translation outputs on
  top of an almost full disk) and the win10 VM died about 2 min in (libvirt: shut off, reason crashed). I cut
  /root/a44 to ~1.6 GB (11 GB free). /root/a43/fspx_new/disasm is 82 GB of llvm-mc listings (each warning repeats
  the whole byte string), fully superseded by /root/a44/fx/disasm - deleting it was not permitted to me; it needs
  the user. The VM was not restarted (main session: wait for free space).
- Step 1 (who writes the mesh args at 0xde226a000) is still open. Note: INDIRECT_SRC reads the stand-in on the CPU at
  replay time, before this frame's GPU work, so [0 1 1] there does not by itself prove the cull pass wrote 0 groups
  (Plausible).
- NEXT: free disk, start VM, deploy_next.sh, rt214 >= 1800 s; check the "no translated compute shader" count (expect
  ~3), INDIRECT_SRC mesh args, rtdiag G-buffer dumps, device loss (new shaders untested on GPU: if the device is
  lost, bisect by restoring index.a44bak.json + halves of the new entries).

## agA45 (sender gen 45, 2026-11-27) - why the 1141 new CS SPIR-V is so big (offline, no VM)
- Work dir /root/a45: out/ = 1141 .spv (bvh outlined + --opt, RECOMMENDED), out_noopt/ = bvh outlined only, tr.sh
  (N shards, nice 19), deploy.sh [out|out_noopt], prof.py (per-RDNA-opcode count of emitted SPIR-V lines), stats.py
  (size distribution vs the deployed sizes in /tmp/vs2), runs.py / ophist.py (repeated pure runs, words per opcode).
- ROOT CAUSE (Definite, measured): there is no general translator blow-up. The programs are huge: median 18,531
  instructions (p90 22,177, max 46,740 = 239 KB of code) vs 458 (p90 3,104) for the 1536 older shaders. SPIR-V
  bytes per source instruction are the SAME as the old corpus (new p10..p90 ~190-280 B/insn; old ~240-440), i.e.
  ~40-50 words per instruction from the register-in-Function-variable lowering (load, bitcast, op, NoContraction,
  bitcast, EXEC select, store). Structured control flow (939 structured, 186 emulated-wave64 structured), no
  dispatch loop, no waterfall/switch duplication: pure-run dedupe over a 19K-insn module would save only 21% of
  %main lines. spirv-opt (-Os, or scalar-replacement + ssa-rewrite + ADCE) makes it BIGGER (9.0 -> 11.5/12.4 MB:
  phi webs over ~100 live registers).
- The one real blow-up: image_bvh(64)_intersect_ray was inlined at every use, ~2,800 instructions each (24
  round-toward-zero slab products/subtractions = Dekker splits). 41 programs use it (2..72 sites); in cs_5c2c7ae5 it
  was 32% of the module. FIX (tools/rdna2spirv.py): the step is emitted once per module as %bvh_fn (DontInline;
  heap base + 14 raw register words -> v4uint, phi-merged tri/box/other results) and its slab test as %bvh_slab_fn
  (called 24x); the call site loads the registers, calls, writes vD..vD+3. Same semantics: the 16 image_bvh
  semantic tests pass (and fail if the slab args are swapped); full semantic suite 6015 tests, same 2 pre-existing
  failures (test_bitcast_round_trip, test_int32_to_f64); 2037 rdna2spirv pytest pass, 1 pre-existing failure
  (test_compute_thread_group_size_sgpr_is_rejected, asserts the refusal A44 removed).
- --opt (existing tools/spirv_opt.py peephole, off by default, NOT used for the old corpus): -26% across the board.
- Sizes (1141 each, all pass spirv-val; index entries identical to /root/a44/index.merged2.json, so index.json
  needs no change):
    before (A44)      total 3.40 GB  p50 3.87 MB  p90 4.68 MB  max 13.43 MB  >1.5MB 766  >3MB 706
    out_noopt (bvh)   total 3.36 GB  p50 3.86 MB  p90 4.67 MB  max  9.01 MB  >1.5MB 761  >3MB 701
    out (bvh + opt)   total 2.50 GB  p50 2.89 MB  p90 3.46 MB  max  6.70 MB  >1.5MB 719  >3MB 512
  (5c2c7ae5 13.4 -> 9.0 -> 6.7 MB; 5244844 13.1 -> 8.6 -> 6.4; e27b072b 12.7 -> 8.3 -> 6.2).
  The <1.5 MB target is NOT reachable by removing bloat: it needs ~3x fewer SPIR-V instructions per RDNA
  instruction (an SSA-style lowering of the register file), not a fix.
- NVIDIA data (rt214/rt215 logs, /root/rb): of 316 new CS attempted, 29 failed with -13 after 216-490 s, all
  3.1-4.9 MB; many of the same size succeeded after ~450-495 s. Compile time is the problem, size is a proxy.
  Not GPU-verified whether out/ compiles within the limit (VM was busy). %bvh_fn is DontInline: a hint only.
- NEXT: deploy (cd /root/a45 && ./deploy.sh out; backs up the replaced 1141 .spv to C:\zroot\a44spv_bak.tar), rerun,
  count -13. If still failing: raise/remove the runtime's background compile limit for these, or ship them via
  a pipeline cache built once; translator-side the next lever is fewer ops per VALU write (EXEC select +
  bitcasts are ~30% of words).
