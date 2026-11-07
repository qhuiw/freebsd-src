# jemalloc MTE prototype review and guest tests

This is source for a future FreeBSD AArch64 guest test. It has not been built
or run as part of the implementation. Do not use host macOS execution to
validate MTE faults.

## Implementation contract

- `MALLOC_CONF=mte:true` enables synchronous heap tagging at allocator startup.
  It defaults to false. `mallctl("config.mte", ...)` reports compiled support;
  `mallctl("opt.mte", ...)` reports the startup option. Unsupported hardware,
  missing kernel support, or incompatible allocator configuration makes
  allocator initialization fail; it does not silently disable protection.
- The FreeBSD AArch64 libc configuration defines `JEMALLOC_MTE`. Its minimum
  class is 16 bytes, including when tagging is off. Other architectures and
  capability pointers do not enable this implementation. This patch targets
  the FreeBSD libc build, not an independently configured upstream jemalloc.
- Metadata, bootstrap allocations, free objects and cached objects use tag 0.
  The ordinary allocation completion path assigns a random tag from 1–15 to
  every granule of the usable size, after zero/junk initialization. Ordinary
  free validates the caller's tag, looks up the real size, resets the memory
  tags to 0, and only then fills/caches/releases the object.
- Dedicated malloc/free fast paths fall back to the ordinary paths. Thread
  caches remain enabled and contain untagged pointers. Sized deallocation
  verifies the supplied size class before changing memory tags. Batch
  allocation uses the ordinary allocation path. Metadata lookups strip tags;
  payload accesses keep matching tags.
- `realloc`/`rallocx` allocate, copy with both tagged pointers, then free.
  Failure leaves the old allocation and tag intact. `xallocx` returns the
  existing usable size without resizing. Optimized resize support is deferred.
- Custom extent-hook changes and arena reset/destruction return `ENOTSUP`.
  Creation of an arena with custom hooks is also rejected. Profiling, HPA,
  and DSS configurations are not supported when MTE is enabled.
- The helper re-enables synchronous MTE via `ARM64_MTE_CTRL` before allocation
  tagging and pointer validation, preserving errno. There is intentionally no
  TLS cache of this state: this branch's kernel omits `SCTLR_ATA0` inheritance
  in `mte_fork`/`mte_copy_thread`. No kernel/debugger sources are changed.
  The syscall on each operation is prototype overhead and may produce kernel
  diagnostic output with this kernel branch. Applications must not change the
  MTE control policy behind the allocator.
- Instruction assembly is confined to `src/mte.c` using `.arch_extension
  memtag`. There is no global `-march` change. Mapping/recommit/guard protection
  paths preserve `PROT_MTE` while enabled. Startup headers must include this
  branch's MTE interfaces before a later libc-only rebuild.

Tagging protects the usable size, not the exact requested byte count. Tag
collisions with neighbours or reused allocations remain possible. Reserving
tag 0 detects stale nonzero-tagged accesses before reuse, but does not provide
a quarantine or guaranteed detection after reuse.

## Later validation, after review and installation in a guest

Build only inside the chosen FreeBSD AArch64 test environment:

```sh
make -C tools/test/malloc/mte
```

Run these as separate processes so each option is set before malloc starts:

```sh
env MALLOC_CONF=mte:false tools/test/malloc/mte/jemalloc_mte_test off
env MALLOC_CONF=mte:true,tcache:true tools/test/malloc/mte/jemalloc_mte_test on
env MALLOC_CONF=mte:true,tcache:false tools/test/malloc/mte/jemalloc_mte_test on
env MALLOC_CONF=mte:true,junk:true tools/test/malloc/mte/jemalloc_mte_test on
env MALLOC_CONF=mte:true,zero:true tools/test/malloc/mte/jemalloc_mte_test on
```

The test covers tiny/rounded/large allocations, all usable granules, adjacent
tiny allocations, repeated cache hits, calloc and overflow, alignment, sized
free, realloc failure/data preservation, xallocx's conservative result,
cross-thread ownership and child-process free. Deliberate wrong-tag access and
use-after-free must deliver `SIGSEGV` with `SEGV_MTESERR`. A random crash is not
accepted as a successful test. The intentional accesses use assembly to avoid
compiler removal of C undefined behavior.

Each fault check prints its own result. A successful run with tagging enabled
ends with:

```text
PASS: wrong-tag: SIGSEGV, si_code=SEGV_MTESERR (4)
PASS: use-after-free: SIGSEGV, si_code=SEGV_MTESERR (4)
PASS: jemalloc MTE on
```

On failure, the parent names the check and prints the expected signal/cause,
the actual signal and numeric `si_code` (with a symbolic name for recognized
`SIGSEGV` causes), whether the fault occurred during the deliberate access,
and the child's exit status. Signal handlers send a small report through a
pipe using only async-signal-safe operations. Missing or incomplete reports
fail the check;
if an unhandled signal kills the child, the parent reports that signal and
states that detailed signal information is unavailable. An access that does
not fault and a child setup/check failure have distinct diagnostics. Both
fault checks run even if the first fails, and either failure makes the test
exit nonzero without printing the overall PASS line.

Before treating the patch as usable, also run the existing allocator/libc
tests, test purge/decommit/recommit and memory pressure, check syscall APIs
with tagged buffers, and validate dynamic and static applications. None of
those runtime checks has been performed for this source-only change.

## Worktree and build isolation

Keep the implementation in the `codex/jemalloc-mte` worktree for review. Do not
point the existing cheribuild configuration at it or share its exact object
directory. After review, transfer the allocator/build/test patch to the
existing source checkout without overwriting unrelated debugger edits, then
use that checkout's existing build environment for the libc rebuild. This
worktree does not contain the debugger checkout's uncommitted changes.
