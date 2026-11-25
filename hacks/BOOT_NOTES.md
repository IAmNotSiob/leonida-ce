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
