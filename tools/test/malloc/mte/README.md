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
  TLS cache of this state. The kernel inheritance fix makes both `mte_fork`
  and `mte_copy_thread` preserve `SCTLR_ATA0` together with `TCF0`. Before that
  fix, `cpu_fork` already preserved `ATA0` through its later full `md_sctlr`
  copy, but new thread creation cleared it. The conservative helper remains
  compatible with those older kernels; removing its per-operation syscall
  is a separate optimization. Applications must not change the MTE control
  policy behind the allocator. The isolated inheritance probes below change
  it deliberately and make no subsequent allocator calls in the probe.
- Instruction assembly is confined to `src/mte.c` using `.arch_extension
  memtag`. There is no global `-march` change. Mapping/recommit/guard protection
  paths preserve `PROT_MTE` while enabled. Startup headers must include this
  branch's MTE interfaces before a later libc-only rebuild.

Tagging protects the usable size, not the exact requested byte count. Tag
collisions with neighbours or reused allocations remain possible. Reserving
tag 0 detects stale nonzero-tagged accesses before reuse, but does not provide
a quarantine or guaranteed detection after reuse.

This is functional allocator MTE support for ordinary FreeBSD AArch64
userspace heap experiments. It supplies a baseline for future revocation
research, but does not implement CHERI capability revocation or support
pure-capability pointers. The selected tests establish the allocator contract
above; they do not establish compatibility of every libc function or application.

## Reported guest validation

On 2026-11-09, the user reported that all 11 groups in `run_mte_tests.sh`
passed in the booted image. The runner tests the installed guest libc and
requires both mmap regressions and all 21 selected libc ATF cases per MTE
setting to pass without skips. That integrated result establishes the current
image as a validated prototype baseline for the covered allocator, inheritance,
mapping, and libc paths. Keep the runner's results directory, including
`environment.txt`, `summary.tsv`, logs, and Kyua databases, with the image
so its kernel, libc, test binary, and results remain identifiable.

The earlier component validation is recorded below.

After booting the corrected `GENERIC-MTE` kernel, the user reported the smoke
suite passing with tagging off and on, including the expected synchronous
wrong-tag and use-after-free faults. On 2026-11-08 the user also reported all
four MTE-on variants passing: `tcache:true`, `tcache:false`, `junk:true`, and
`zero:true`. The user subsequently reported stress mode passing with tagging
off and on, and `passed` results from the two directly invoked VM regression
cases. ATF warned that those direct invocations did not use Kyua isolation
or timeout handling.

On 2026-11-08, the inheritance change was cross-built and tested in a
nonpersistent, two-vCPU QEMU snapshot of `freebsd-mte-stage2.img`. The same
new test executable passed all three fork cases and the disabled-thread
case on the old kernel (#4), and failed both enabled-thread cases at the
allocation-tag load, as expected. After booting the corrected kernel (#5)
in that snapshot, all six inheritance cases passed. Allocator stress with
MTE off and on also passed, and Kyua reported both mmap regressions passed
(0 broken, 0 failed, 0 skipped). The baseline image was not regenerated.
On 2026-11-09 the user reported the inheritance-image checklist passing,
including the additional `tcache:false`, `junk:true`, and `zero:true` stress
runs. Those results preceded the separate string-library fix below.

The initial negative-test diagnostic exposed a libc `strchr` overread in
`strerror`'s locale-catalogue lookup. On 2026-11-09 this was traced to a
32-byte load crossing a 16-byte allocation-tag boundary and fixed by selecting
Arm's existing MTE-compatible string-search implementations. The original
reproducer and the new `libc` mode now pass with the rebuilt libc. The focused
inheritance test keeps its numeric-only diagnostics.

## Complete guest checklist

`run_mte_tests.sh` runs the full current baseline checklist. It lives beside
`jemalloc_mte_test` in the tracked test directory and in the image overlay.
After booting an image containing the updated overlay, run:

```sh
cd /root/jemalloc-mte
./run_mte_tests.sh
```

Optionally name a new results directory (its parent must already exist):

```sh
./run_mte_tests.sh /root/mte-baseline-results
```

The default is a unique `results.XXXXXXXX` directory beside the script.
The runner checks for FreeBSD AArch64, the executable, Kyua, and required
installed ATF programs before starting. It tests the installed guest libc;
unset `LD_LIBRARY_PATH` and `LD_PRELOAD` if they were used for temporary
library experiments. It records the kernel version, kernel/libc/test hashes,
and the test's loaded libraries in `environment.txt`.

These are the standalone test commands, in the runner's order. Stress mode
already includes the ordinary smoke checks, so separate smoke runs are not
needed for this checklist:

```sh
cd /root/jemalloc-mte
env MALLOC_CONF=mte:true ./jemalloc_mte_test on inherit
env MALLOC_CONF=mte:false ./jemalloc_mte_test off stress
env MALLOC_CONF=mte:true ./jemalloc_mte_test on stress
env MALLOC_CONF=mte:true,tcache:false ./jemalloc_mte_test on stress
env MALLOC_CONF=mte:true,junk:true ./jemalloc_mte_test on stress
env MALLOC_CONF=mte:true,zero:true ./jemalloc_mte_test on stress
env MALLOC_CONF=mte:false ./jemalloc_mte_test off libc
env MALLOC_CONF=mte:true ./jemalloc_mte_test on libc
env MALLOC_CONF=mte:false kyua test -k /usr/tests/sys/vm/Kyuafile \
  mmap_test:mmap__mte_fork \
  mmap_test:mmap__mte_prot_none_after_fork
```

The runner also executes all cases in these existing ATF programs, once with
MTE off and once with MTE on: `strchr_test`, `strchrnul_test`, `strrchr_test`,
`strerror_test`, and `strerror2_test`. That is 21 cases per setting. It creates
small wrappers and Kyuafiles under the results directory that set
`MALLOC_CONF` inside each ATF process; setting it only on the Kyua command
would not reliably preserve it through Kyua's environment sanitization.
For those two groups it invokes:

```sh
# RESULTS is the absolute directory printed by the runner.
env MALLOC_CONF=mte:false kyua test \
  --results-file="$RESULTS/libc-off.db" -k "$RESULTS/libc-off/Kyuafile"
env MALLOC_CONF=mte:false kyua test \
  --results-file="$RESULTS/libc-on.db" -k "$RESULTS/libc-on/Kyuafile"
```

The runner saves a separate log and argument list for each of the 11 groups,
plus the three Kyua databases and text reports. `summary.tsv` records each
group's result and exit code. A failure does not prevent later groups from
running. Kyua groups must have exactly the expected 2 or 21 cases, with no
skips, expected failures, broken cases, or failures. A skipped case therefore
does not count as validating the baseline.

Successful completion ends with:

```text
SUMMARY: 11 passed, 0 failed
PASS: complete jemalloc MTE baseline checklist
```

Exit status is 0 only when all groups pass, 1 if any group fails, and 2 for a
setup error. Existing results directories are not overwritten. Each standalone
binary has its existing timeouts; Kyua supplies ATF isolation and timeouts.
No software is rebuilt or installed by the runner.

Adding this shell script requires no kernel, libc, or test-binary rebuild.
Regenerate the disk image to bake it in, or copy just `run_mte_tests.sh` into
`/root/jemalloc-mte/` in an already booted guest with the updated libc/test.

## Building and running the allocator test

Build inside the chosen FreeBSD AArch64 test environment:

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

The host can also cross-build and stage this test with the existing world:

```sh
cd /Users/qian/cheribuild
./cheribuild.py --freebsd-aarch64/subdir=tools/test/malloc/mte freebsd-aarch64
```

The installed binary is `/usr/tests/lib/libc/stdlib/jemalloc_mte_test` in the
staged root and in a subsequently rebuilt image.

The image overlay at
`/Users/qian/cheri/extra-files-freebsd/root/jemalloc-mte/` supplies the guest's
`/root/jemalloc-mte/` directory. Keep its `jemalloc_mte_test.c`, `Makefile`,
`run_mte_tests.sh`, and `README.md` synchronized with this tracked test directory,
and copy the rebuilt AArch64 binary there before regenerating the image. Updating only the
tracked source or staged `/usr/tests` binary does not refresh that overlay.
The overlay now also contains the prebuilt test, so the guest can run:

```sh
cd /root/jemalloc-mte
env MALLOC_CONF=mte:true ./jemalloc_mte_test on stress
```

Run `make` in that guest directory if rebuilding from its source is desired.
Each run prints its phase and effective tcache/zero settings. Smoke mode has
a 60-second timeout; stress mode has a 300-second timeout.

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

## Additional stress coverage

Append `stress` to run the smoke checks followed by:

- Four waves of four concurrent workers, each performing 16 rounds of 16
  mixed-size allocations, publishing batches through barriers for another
  thread to verify, resize, and free. This includes thread-cache flushing
  and thread teardown, with about 4,096 ownership transfers in total.
- Explicit tcache creation, allocation/free, flushing, reuse, and destruction.
  Explicit caches are exercised even when automatic tcaches are disabled.
- Eight forks with live allocations from tiny to 64 KiB. Child free/reallocation
  must preserve the parent's payload and allocation tags. Forks occur only
  after all worker threads have joined.
- Four purge/reallocation cycles in a dedicated arena with eight 1 MiB blocks,
  disabled automatic dirty decay, and no tcache. Statistics must show dirty
  pages before purging, an increased purged-page count, and no dirty/muzzy
  pages afterward. Reallocation checks zero initialization and every usable
  byte's accessibility, including after the final purge.

Run the new binary in the guest:

```sh
env MALLOC_CONF=mte:false /usr/tests/lib/libc/stdlib/jemalloc_mte_test off stress
env MALLOC_CONF=mte:true,tcache:true /usr/tests/lib/libc/stdlib/jemalloc_mte_test on stress
env MALLOC_CONF=mte:true,tcache:false /usr/tests/lib/libc/stdlib/jemalloc_mte_test on stress
env MALLOC_CONF=mte:true,junk:true /usr/tests/lib/libc/stdlib/jemalloc_mte_test on stress
env MALLOC_CONF=mte:true,zero:true /usr/tests/lib/libc/stdlib/jemalloc_mte_test on stress
```

The final line includes `(stress)` only after all additional checks pass.
The reclamation case requires `config.stats`, enabled by this libc build.
It verifies allocator purging, not guaranteed immediate physical reclamation
or a particular address being reused. Whether decommit/recommit occurs
instead of lazy purge depends on the kernel and allocator policy; these
checks do not claim to force that path. Sustained low-memory stress, forced
decommit/recommit, tagged-buffer syscall compatibility, static executables,
and the wider libc test suite remain outstanding.

## Tagged strings and strerror compatibility

The original reproducer is:

```c
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(void) { fprintf(stderr, "%s\n", strerror(EINVAL)); return 0; }
```

With the old libc, enabling MTE produces `SIGSEGV/SEGV_MTESERR` at
`strchr+0x70`, the `ld1 {v1.16b, v2.16b}, [x2], #32` instruction. Its caller
is `__catopen_l` in `lib/libc/nls/msgcat.c`, searching a heap copy of the locale
name. The allocated string fits in one 16-byte granule, but the implementation
reads two granules before checking for a terminator. The second granule can
have a different allocation tag. This is an actual library overread detected
by MTE; weakening checking or enlarging every small allocation would hide it.

The AArch64 wrappers now select `strchr-mte.S`, `strchrnul-mte.S`, and
`strrchr-mte.S` from the already imported Arm Optimized Routines. They check
one 16-byte granule before loading the next, retain `index`/`rindex` aliases,
and use baseline Advanced SIMD instructions rather than requiring MTE
instructions on CPUs without MTE. The sibling searches had the same unsafe
32-byte scanning behavior and fail the new boundary tests with the old libc.
The allocator's tag selection and 16-byte minimum classes are unchanged.

The existing test executable has a separate `libc` mode:

```sh
cd /root/jemalloc-mte
env MALLOC_CONF=mte:false ./jemalloc_mte_test off libc
env MALLOC_CONF=mte:true ./jemalloc_mte_test on libc
```

It checks `strchr`, `index`, `strchrnul`, `strrchr`, and `rindex` against a
bytewise reference at every initial offset within a granule, lengths 0–80,
both halves of a 32-byte boundary, and a page end. Each search is checked for
NUL, present/absent characters, high-bit characters, and integer-to-byte
conversion. Prefix NULs must be ignored. In MTE-on mode, string granules use
tag 1 and the neighbouring granules use tag 0, so overreads fail deterministically.
Guard pages and MTE-off runs also check ordinary behavior. A sixth case checks
repeated `strerror(EINVAL)`, `strerror_r`, formatting, and errno preservation.
Cases run in separate children so one crash does not hide the others. Each
child has a 30-second timeout; the complete libc run has a 240-second timeout.

Verified guest results on 2026-11-09:

- Old libc: all six cases pass with MTE off and all six fail with MTE on.
- Rebuilt libc: the original reproducer and all six cases pass with MTE on;
  the six cases also pass with MTE off.
- Existing `strchr_test`, `strchrnul_test`, `strrchr_test`, `strerror_test`, and
  `strerror2_test`: 21/21 cases pass under Kyua with MTE off and again with MTE
  on. Temporary wrappers explicitly set `LD_LIBRARY_PATH` and `MALLOC_CONF`
  in each test process so Kyua's environment handling cannot mask the settings.
- Allocator stress with MTE off/on and all six inheritance cases also pass
  with the rebuilt libc.

The host libc and test were rebuilt and installed into the staged root; the
updated test source, executable, and README were synchronized into the overlay.
The initial component validation used a temporary library directory and
`LD_LIBRARY_PATH`; the running guest's `/lib/libc.so.7` was not replaced and
no image was regenerated during that step. The subsequently reported complete
runner pass above validates the installed guest libc. To reproduce an image
containing the fix, build from the updated staging tree:

```sh
cd /Users/qian/cheribuild
./cheribuild.py --skip-update \
  --disk-image-freebsd-aarch64/path=/Users/qian/cheri/output/freebsd-mte-libc.img \
  disk-image-freebsd-aarch64
```

Boot that image and run the `libc` commands above. This change needs the rebuilt
libc and test, not another kernel/world rebuild. The inheritance image is a
useful reference for reproducing the original failure. A wider libc audit and
performance measurements remain separate work.

## MTE control inheritance

The kernel fix preserves `SCTLR_ATA0 | SCTLR_TCF0_MASK` in both MTE copy
helpers, leaving the existing GCR inheritance and exec reset unchanged.
Only new-thread behavior was broken in the complete call paths; fork's
later full-state copy already preserved `ATA0`.

Run the focused checks separately from smoke/stress:

```sh
cd /root/jemalloc-mte
env MALLOC_CONF=mte:true ./jemalloc_mte_test on inherit
```

There are six cases: fork and raw kernel thread creation, each with tagging
enabled/synchronous, tagging enabled/no fault reporting, and tagging
disabled with synchronous mode still selected. Each creates a live tagged
jemalloc allocation in its creator. With tag access enabled the probe first
checks `LDG` against the pointer tag, then checks the payload through the
matching pointer. A wrong-tag read must produce `SIGSEGV/SEGV_MTESERR` only
in the enabled/synchronous case; it must complete in the other two modes.
This checks preservation of both the enable bit and the fault mode, rather
than unconditionally enabling protection in every new context.

The tested fork and `thr_new` use direct system calls. The raw thread has
its own mapped stack, no TLS, and no pthread startup. Signal installation,
the handler's pipe report, and exit also use direct system calls to avoid
libthr signal trampolines and lazy symbol resolution. No allocator or
library call can restore MTE in the measured child/thread before its
accesses. An outer child isolates each case and reports the phase, signal,
`si_code`, error and wait status on failure. Each outer child has a 15-second
timeout and the full inheritance run has a 120-second timeout.

On the old stage2 kernel, the enabled raw-thread cases fail and the fork
cases pass. On the fixed kernel all six pass, ending with
`PASS: jemalloc MTE on (inheritance)`. These before/after outcomes were
verified in the snapshot guest described above.

Cross-build the updated test with the existing `tools/test/malloc/mte`
subdirectory command above, and refresh the overlay source and binary.
Rebuild/install the corrected kernel before making a new experimental image:

```sh
cd /Users/qian/cheribuild
./cheribuild.py --skip-update --skip-world --no-skip-kernel \
  --freebsd-aarch64/kernel-config=GENERIC-MTE freebsd-aarch64
./cheribuild.py --skip-update \
  --disk-image-freebsd-aarch64/path=/Users/qian/cheri/output/freebsd-mte-inherit.img \
  disk-image-freebsd-aarch64
```

Keep `freebsd-mte-stage2.img` as the earlier baseline. The initial validation
build used `--skip-install` for the host kernel target and copied that kernel
only into the disposable guest. The user has since installed the corrected
kernel into the staged root and built/booted the inheritance image. The test
is installed in the staged root and synchronized into the image overlay,
including source and binary. The inheritance fix alone needs no libc/world
or mmap-test rebuild; the subsequent string-library correction above needs
the rebuilt libc. Building an image does not update a running guest.

## Worktree and build isolation

### Kernel prerequisite exposed by the guest test

The first reported guest run passed with `MALLOC_CONF=mte:false`. The initial
`MALLOC_CONF=mte:true` run panicked during fork. Rebuilding and booting the
corrected kernel resolved that failure, and the smoke suite then passed.

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
tagging. Cross-build and stage them without rebuilding world:

```sh
cd /Users/qian/cheribuild
./cheribuild.py --freebsd-aarch64/subdir=tests/sys/vm freebsd-aarch64
```

After refreshing the image and booting the corrected kernel, run:

```sh
env MALLOC_CONF=mte:false kyua test -k /usr/tests/sys/vm/Kyuafile \
    mmap_test:mmap__mte_fork \
    mmap_test:mmap__mte_prot_none_after_fork
```

The first checks fork and data copy-on-write isolation. The second requires
`SIGSEGV/SEGV_ACCERR` when reading a populated copy-on-write mapping after
`mprotect(PROT_NONE | PROT_MTE)`. Neither test verifies nonzero allocation-tag
inheritance; use the jemalloc MTE tests for that coverage. Directly invoking
an ATF test binary prints a warning about missing runner isolation/timeouts;
a final `passed` still reports that case's result. Use Kyua as above for
managed runs. The standalone `jemalloc_mte_test` is not an ATF program and
continues to run directly.

The unconditional `writing new sctlr val` debug print has been removed from
`mte_sysarch_ctrl()`. Its MTE register updates and synchronization are
unchanged. Rebuild and boot the updated kernel to silence those messages;
QEMU's own diagnostics for deliberate tag faults are separate.

### Allocator transfer

The initial allocator patch and the kernel fork fix have been transferred
from `codex/jemalloc-mte` to `/Users/qian/Desktop/m5/freebsd-src` on
`mte-dev-new`. Make subsequent changes directly in that existing checkout,
preserving unrelated debugger edits. The experimental worktree is no longer
needed for development.

Keep using the existing cheribuild source configuration and build directory.
The libc rebuild has updated the shared staged root filesystem, and the
fork fix has been booted and smoke-tested. Further kernel or test rebuilds
update that staging directory; refresh the disk image before testing those
changes in a new guest. A separate image filename does not isolate the
shared staging directory.
