# jemalloc MTE prototype review and guest tests

These tests target a FreeBSD AArch64 guest with the prototype libc. Do not
use host macOS execution to validate MTE faults. The reported guest results
and a kernel prerequisite are described below.

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
  in `mte_fork`/`mte_copy_thread`. The allocator patch does not change
  kernel/debugger sources; the separate fork fix below is a kernel prerequisite.
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

### Kernel prerequisite exposed by the guest test

The first reported guest run passed with `MALLOC_CONF=mte:false`. With
`MALLOC_CONF=mte:true`, the kernel panicked during fork before the test could
complete.

A guest panic reporting `invalid prot 21` in `pmap_protect()` during
`vmspace_fork()` is a kernel protection-mask bug. The number is hexadecimal:
`0x21` is `PROT_READ | PROT_MTE`. Fork removes `PROT_WRITE` to establish
copy-on-write, but the arm64 assertion originally accepted only the ordinary
read/write/execute bits. This happens at the first fork in
`thread_and_fork()`, before the deliberate wrong-tag/use-after-free checks.

The separate kernel fix allows `PROT_MTE` at this boundary and removes it
from the local access-permission calculation. Existing PTE memory attributes
remain unchanged. `PROT_NONE | PROT_MTE` must still remove the mapping;
merely widening the assertion would leave that case readable.

This fix requires rebuilding and booting the corrected `GENERIC-MTE` kernel.
A libc-only rebuild or a fresh image containing the old kernel cannot fix
the panic. The cheribuild configuration used for the allocator experiment
has `skip-kernel` enabled; override that when preparing the corrected kernel.

The `mmap__mte_fork` and `mmap__mte_prot_none_after_fork` cases in
`tests/sys/vm/mmap_test.c` exercise this boundary independently of allocator
tagging. After rebuilding that test and booting the corrected kernel, run:

```sh
env MALLOC_CONF=mte:false /usr/tests/sys/vm/mmap_test mmap__mte_fork
env MALLOC_CONF=mte:false /usr/tests/sys/vm/mmap_test mmap__mte_prot_none_after_fork
```

The first checks fork and data copy-on-write isolation. The second requires
`SIGSEGV/SEGV_ACCERR` when reading a populated copy-on-write mapping after
`mprotect(PROT_NONE | PROT_MTE)`. Neither test verifies nonzero allocation-tag
inheritance; rerun the jemalloc MTE tests for that coverage. These new kernel
regressions have not yet been run in a guest.

The repeated `writing new sctlr val` lines come from a debug `printf()` in
`mte_sysarch_ctrl()`. The prototype's per-operation MTE control syscall makes
that message frequent; it is separate from the protection-mask panic.

### Allocator transfer

The initial allocator patch and the kernel fork fix have been transferred
from `codex/jemalloc-mte` to `/Users/qian/Desktop/m5/freebsd-src` on
`mte-dev-new`. Make subsequent changes directly in that existing checkout,
preserving unrelated debugger edits. The experimental worktree is no longer
needed for development.

Keep using the existing cheribuild source configuration and build directory.
The libc rebuild has updated the shared staged root filesystem. The kernel
fix still requires a kernel rebuild and a new guest boot before validation;
a separate image filename does not isolate the shared staging directory.
