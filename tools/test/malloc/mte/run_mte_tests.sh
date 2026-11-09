#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Run inside the FreeBSD AArch64 guest.  Keep beside jemalloc_mte_test.

set -u

usage()
{
	printf 'Usage: %s [new-results-directory]\n' "$0"
	printf 'Runs allocator stress/configurations, inheritance, libc and mmap tests.\n'
	printf 'Default results: a new results.XXXXXXXX directory beside this script.\n'
}

if [ "$#" -eq 1 ] && [ "$1" = "--help" ]; then
	usage
	exit 0
fi
if [ "$#" -gt 1 ]; then
	usage >&2
	exit 2
fi

die()
{
	printf 'ERROR: %s\n' "$*" >&2
	exit 2
}

[ "$(uname -s)" = FreeBSD ] || die 'Run this script inside the FreeBSD guest.'
case $(uname -m) in
arm64|aarch64) ;;
*) die 'This test suite requires AArch64.' ;;
esac

# Validate the installed guest libc, not an interposed or temporary library.
[ -z "${LD_LIBRARY_PATH-}" ] && [ -z "${LD_PRELOAD-}" ] ||
	die 'Unset LD_LIBRARY_PATH and LD_PRELOAD to test the installed guest libc.'
LC_ALL=C
MALLOC_CONF=mte:false
export LC_ALL MALLOC_CONF
umask 077
ulimit -c 0 || die 'Cannot disable core dumps for deliberate fault tests.'

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd -P) || die 'Cannot locate script.'
test_bin=$script_dir/jemalloc_mte_test
vm_suite=/usr/tests/sys/vm/Kyuafile
libc_dir=/usr/tests/lib/libc/string
libc_programs='strchr_test strchrnul_test strrchr_test strerror_test strerror2_test'
[ -x "$test_bin" ] || die "Missing executable: $test_bin"
[ -r "$vm_suite" ] || die "Missing VM suite: $vm_suite"
[ -x /usr/tests/sys/vm/mmap_test ] || die 'Missing mmap_test executable.'
for program in $libc_programs; do
	[ -x "$libc_dir/$program" ] || die "Missing executable: $libc_dir/$program"
done
for utility in kyua sha256 ldd mktemp; do
	command -v "$utility" >/dev/null 2>&1 || die "Missing command: $utility"
done

if [ "$#" -eq 1 ]; then
	case $1 in
	/*) results=$1 ;;
	*) results=$PWD/$1 ;;
	esac
	mkdir "$results" || die 'Results directory must be new and its parent must exist.'
else
	results=$(mktemp -d "$script_dir/results.XXXXXXXX") || die 'Cannot create results directory.'
fi
results=$(CDPATH= cd "$results" && pwd -P) || die 'Cannot resolve results directory.'
summary=$results/summary.tsv
printf 'group\tresult\texit_code\n' >"$summary" || die 'Cannot write summary.'
{
	date -u
	uname -a
	printf '\nTest executable: %s\n' "$test_bin"
	sha256 "$test_bin" /lib/libc.so.7 /boot/kernel/kernel
	printf '\nLoaded libraries:\n'
	ldd "$test_bin"
} >"$results/environment.txt" 2>&1 || die 'Cannot record the guest environment.'

# Kyua sanitizes the environment.  Set the allocator option inside each
# ATF process rather than relying on the runner's inherited MALLOC_CONF.
for mode in off on; do
	case $mode in off) enabled=false ;; on) enabled=true ;; esac
	wrapper_dir=$results/libc-$mode
	mkdir "$wrapper_dir" || die 'Cannot create ATF wrapper directory.'
	printf 'syntax(2)\ntest_suite("libc")\n' >"$wrapper_dir/Kyuafile" || die 'Cannot write Kyuafile.'
	for program in $libc_programs; do
		printf 'atf_test_program{name="%s"}\n' "$program" >>"$wrapper_dir/Kyuafile" ||
		    die 'Cannot update Kyuafile.'
		printf '#!/bin/sh\nexec env MALLOC_CONF=mte:%s %s/%s "$@"\n' \
		    "$enabled" "$libc_dir" "$program" >"$wrapper_dir/$program" ||
		    die 'Cannot write ATF wrapper.'
		chmod 755 "$wrapper_dir/$program" || die 'Cannot make ATF wrapper executable.'
	done
done

passed=0
failed=0
run_case()
{
	case_name=$1
	shift
	case_log=$results/$case_name.log
	printf 'RUN: %s\n' "$case_name"
	# One argument per line avoids ambiguous quoting in the saved command.
	printf '%s\n' "$@" >"$results/$case_name.argv" || die 'Cannot record command.'
	if "$@" >"$case_log" 2>&1; then
		case_exit=0
		case_result=PASS
		passed=$((passed + 1))
	else
		case_exit=$?
		case_result=FAIL
		failed=$((failed + 1))
	fi
	printf '%s\t%s\t%s\n' "$case_name" "$case_result" "$case_exit" >>"$summary" ||
	    die 'Cannot update summary.'
	printf '%s: %s (exit %s)\n' "$case_result" "$case_name" "$case_exit"
	if [ "$case_exit" -ne 0 ]; then
		tail -n 20 "$case_log"
		printf 'Full log: %s\n' "$case_log"
	fi
}

run_kyua()
(
	kyua_suite=$1
	kyua_db=$2
	expected=$3
	shift 3
	kyua_exit=0
	env MALLOC_CONF=mte:false kyua test --results-file="$kyua_db" \
	    -k "$kyua_suite" "$@" || kyua_exit=$?
	kyua_report=${kyua_db%.db}.report.txt
	env MALLOC_CONF=mte:false kyua report --results-file="$kyua_db" \
	    >"$kyua_report" 2>&1 || exit 1
	cat "$kyua_report"
	[ "$kyua_exit" -eq 0 ] || exit "$kyua_exit"
	# Kyua can return success when cases skip.  A baseline needs all cases.
	grep -Fqx "Test cases: $expected total, 0 skipped, 0 expected failures, 0 broken, 0 failed" \
	    "$kyua_report" || {
		printf 'FAIL: expected all %s ATF cases to pass without skips.\n' "$expected"
		exit 1
	}
)

printf 'Results: %s\n' "$results"
run_case inheritance env MALLOC_CONF=mte:true "$test_bin" on inherit
run_case stress-off env MALLOC_CONF=mte:false "$test_bin" off stress
run_case stress-on env MALLOC_CONF=mte:true "$test_bin" on stress
run_case stress-no-tcache env MALLOC_CONF=mte:true,tcache:false "$test_bin" on stress
run_case stress-junk env MALLOC_CONF=mte:true,junk:true "$test_bin" on stress
run_case stress-zero env MALLOC_CONF=mte:true,zero:true "$test_bin" on stress
run_case libc-off env MALLOC_CONF=mte:false "$test_bin" off libc
run_case libc-on env MALLOC_CONF=mte:true "$test_bin" on libc
run_case mmap run_kyua "$vm_suite" "$results/mmap.db" 2 \
    mmap_test:mmap__mte_fork mmap_test:mmap__mte_prot_none_after_fork
run_case libc-atf-off run_kyua "$results/libc-off/Kyuafile" "$results/libc-off.db" 21
run_case libc-atf-on run_kyua "$results/libc-on/Kyuafile" "$results/libc-on.db" 21

printf '\nSUMMARY: %s passed, %s failed\n' "$passed" "$failed"
printf 'Results: %s\n' "$results"
if [ "$failed" -ne 0 ]; then
	exit 1
fi
printf 'PASS: complete jemalloc MTE baseline checklist\n'
