#!/bin/bash
# Parser and wiring fixtures for scripts/run_tests.sh.

set -euo pipefail

# The end-to-end fixtures re-enter this file as a fake headless runner. It
# proves the suite script hands every test its own artifact, preset and home
# directories, rather than only unit-testing the allocation helpers.
if [ "${SPATIAL_ECHO_HEADLESS_FIXTURE_RUNNER:-0}" = "1" ]; then
    if [ "${1:-}" = "--list" ]; then
        printf '%s\n' ListedPass ListedSecond
        exit 0
    fi
    fixture_test="${2:?missing fixture test name}"
    fixture_artifact_dir="${SPATIAL_ECHO_TEST_ARTIFACT_DIR:?missing artifact directory}"
    fixture_preset_dir="${SPATIAL_ECHO_TEST_PRESET_DIR:?missing preset directory}"
    fixture_home="${CFFIXED_USER_HOME:?missing private home}"
    printf '%s\n' "$fixture_test" > "${fixture_artifact_dir}/fixture-marker.txt"
    printf '%s\n' "$fixture_test" > "${fixture_preset_dir}/fixture-preset-marker.txt"
    sleep 0.3
    case "$fixture_test" in
        *Fail)
            echo "${fixture_test} [FAIL]"
            exit 1
            ;;
        HomeWriter)
            printf 'stray\n' > "${fixture_home}/stray.txt"
            echo "HomeWriter [PASS]"
            exit 0
            ;;
        *)
            echo "${fixture_test} [PASS]"
            exit 0
            ;;
    esac
fi

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

# shellcheck source=run_tests.sh
source "${SCRIPT_DIR}/run_tests.sh"

failures=0

pass() {
    echo "ok   $1"
}

fail() {
    echo "FAIL $1"
    failures=$((failures + 1))
}

# expect <name> <expected> <stdout_content> <stderr_content> <exit_code> <test_name>
expect() {
    local case_name="$1"
    local expected="$2"
    local stdout_content="$3"
    local stderr_content="$4"
    local exit_code="$5"
    local test_name="$6"

    local stdout_path="${TMP_DIR}/${case_name}.out"
    local stderr_path="${TMP_DIR}/${case_name}.err"
    printf '%s' "$stdout_content" > "$stdout_path"
    printf '%s' "$stderr_content" > "$stderr_path"

    local actual
    actual="$(classify_result "$stdout_path" "$stderr_path" "$exit_code" "$test_name")"
    if [ "$actual" = "$expected" ]; then
        pass "${case_name}: ${actual}"
    else
        fail "${case_name}: expected ${expected}, got ${actual}"
    fi
}

expect pass_clean            PASS          "MixSafety: peak=0.02 [PASS]" "" 0 "MixSafety"
expect fail_token            FAIL          "MixSafety: bad [FAIL]" "" 1 "MixSafety"
expect fail_token_exit_zero  FAIL          "MixSafety: bad [FAIL]" "" 0 "MixSafety"
expect error_token           ERROR         "[ERROR] Unknown test: Nope" "" 2 "Nope"
expect error_on_stderr       ERROR         "" "[ERROR] runner blew up" 2 "MixSafety"
expect skip_optional         SKIP_OPTIONAL "PresetRosterRenders: SPATIAL_ECHO_ROSTER_SPEC not set [SKIP]" "" 3 "PresetRosterRenders"
expect skip_required         SKIP_REQUIRED "PerTapSpatialization: not positioning [SKIP]" "" 3 "PerTapSpatialization"
expect untagged_text         UNTAGGED      "some chatty output with no token" "" 0 "MixSafety"
expect untagged_empty        UNTAGGED      "" "" 0 "MixSafety"
expect assertion_beats_pass  ASSERTION     "PerTapSpatialization: fine [PASS]" "JUCE Assertion failure in juce_String.cpp:327" 0 "PerTapSpatialization"
expect assertion_beats_skip  ASSERTION     "Whatever [SKIP]" "JUCE Assertion failure in juce_Component.cpp:12" 3 "PresetRosterRenders"
expect pass_nonzero_exit     ERROR         "MixSafety: fine [PASS]" "" 1 "MixSafety"
expect fail_beats_pass       FAIL          "step one [PASS] step two [FAIL]" "" 1 "MixSafety"
expect error_beats_fail      ERROR         "[FAIL] then [ERROR]" "" 2 "MixSafety"
expect benign_stderr_pass    PASS          "MixSafety: fine [PASS]" "WARNING: Low HRTF coherence (0.23)" 0 "MixSafety"
expect expected_assert_pass  PASS          "ReverbUndersizedBufferContract: peak=0.0 [PASS]" "JUCE Assertion failure in Reverbprocessor.cpp:458" 0 "ReverbUndersizedBufferContract"
expect expected_assert_fail  FAIL          "TapeDelayUndersizedBufferContract: bad [FAIL]" "JUCE Assertion failure in TapeDelay.cpp:995" 1 "TapeDelayUndersizedBufferContract"
expect tone_assert_pass      PASS          "ToneBufferBoundsContract: bounded [PASS]" "JUCE Assertion failure in ToneFiltering.cpp:211" 0 "ToneBufferBoundsContract"
# Release builds compile jassert out: the boundary tests then pass on their token alone.
expect release_no_assert     PASS          "ReverbUndersizedBufferContract: peak=0.0 [PASS]" "" 0 "ReverbUndersizedBufferContract"

# Concurrent suite invocations must never share per-test capture files.
capture_a="$(create_run_capture_dir "$TMP_DIR")"
capture_b="$(create_run_capture_dir "$TMP_DIR")"
if [ "$capture_a" != "$capture_b" ] && [ -d "$capture_a" ] && [ -d "$capture_b" ]; then
    pass unique_capture_dirs
else
    fail "unique_capture_dirs: suite invocations can collide"
fi

# Artifacts default to the invocation's private capture directory; an
# explicit caller override stays authoritative.
unset SPATIAL_ECHO_TEST_ARTIFACT_DIR
default_artifact_dir="$(resolve_run_artifact_dir "$capture_a")"
explicit_artifact_dir="${TMP_DIR}/explicit-artifacts"
override_artifact_dir="$(SPATIAL_ECHO_TEST_ARTIFACT_DIR="$explicit_artifact_dir" resolve_run_artifact_dir "$capture_a")"
if [ "$default_artifact_dir" = "${capture_a}/artifacts" ] && [ -d "$default_artifact_dir" ] \
    && [ "$override_artifact_dir" = "$explicit_artifact_dir" ] && [ -d "$override_artifact_dir" ]; then
    pass isolated_artifact_dir
else
    fail "isolated_artifact_dir: artifact routing is not private or override-safe"
fi

blocked_parent="${TMP_DIR}/artifact-parent-is-a-file"
printf 'not a directory\n' > "$blocked_parent"
if SPATIAL_ECHO_TEST_ARTIFACT_DIR="${blocked_parent}/child" resolve_run_artifact_dir "$capture_a" >/dev/null 2>&1; then
    fail "artifact_dir_creation_failure: unusable publication path was accepted"
else
    pass artifact_dir_creation_failure
fi

unset SPATIAL_ECHO_TEST_PRESET_DIR
default_preset_root="$(resolve_run_preset_root "$capture_a")"
explicit_preset_root="${TMP_DIR}/explicit-presets"
override_preset_root="$(SPATIAL_ECHO_TEST_PRESET_DIR="$explicit_preset_root" resolve_run_preset_root "$capture_a")"
if [ "$default_preset_root" = "${capture_a}/presets" ] && [ -d "$default_preset_root" ] \
    && [ "$override_preset_root" = "$explicit_preset_root" ] && [ -d "$override_preset_root" ]; then
    pass isolated_preset_root
else
    fail "isolated_preset_root: preset routing is not private or override-safe"
fi

blocked_preset_parent="${TMP_DIR}/preset-parent-is-a-file"
printf 'not a directory\n' > "$blocked_preset_parent"
if SPATIAL_ECHO_TEST_PRESET_DIR="${blocked_preset_parent}/child" resolve_run_preset_root "$capture_a" >/dev/null 2>&1; then
    fail "preset_root_creation_failure: unusable preset path was accepted"
else
    pass preset_root_creation_failure
fi

directory_log_source="${TMP_DIR}/directory-log-source.log"
directory_log_target="${TMP_DIR}/directory-log-target"
printf 'complete suite log\n' > "$directory_log_source"
mkdir -p "$directory_log_target"
if publish_suite_log "$directory_log_source" "$directory_log_target" >/dev/null 2>&1; then
    fail "directory_log_target: directory was accepted as a log file"
else
    pass directory_log_target
fi

# run_suite <log_dir> <output_file> [suite arguments...]: runs the suite
# script against this file as a fake runner and returns its exit status.
run_suite() {
    local log_dir="$1"
    local output_file="$2"
    shift 2
    mkdir -p "$log_dir"
    env SPATIAL_ECHO_HEADLESS_FIXTURE_RUNNER=1 \
        SPATIAL_ECHO_HEADLESS_LOG_DIR="$log_dir" \
        SPATIAL_ECHO_HEADLESS_RUNNER="${SCRIPT_DIR}/test_headless_suite_parsing.sh" \
        "${SCRIPT_DIR}/run_tests.sh" --no-build "$@" > "$output_file" 2>&1
}

count_files() {
    find "$1" -type f -name "$2" | wc -l | tr -d ' '
}

# Without --tests the suite runs exactly the names the runner lists.
listed_root="${TMP_DIR}/listed"
set +e
run_suite "$listed_root" "${TMP_DIR}/listed.out" -j 2
listed_status=$?
set -e
if [ "$listed_status" -eq 0 ] \
    && grep -q 'PASS *ListedPass' "${TMP_DIR}/listed.out" \
    && grep -q 'PASS *ListedSecond' "${TMP_DIR}/listed.out" \
    && grep -q 'pass=2 ' "${TMP_DIR}/listed.out"; then
    pass default_list_from_runner
else
    fail "default_list_from_runner: status=${listed_status}"
fi

# One invocation runs tests in parallel; each gets its own artifact, preset
# and home directory, and the summary attributes every outcome.
parallel_root="${TMP_DIR}/parallel"
set +e
run_suite "$parallel_root" "${TMP_DIR}/parallel.out" -j 3 --tests ParallelA,ParallelB,ParallelFail
parallel_status=$?
set -e
parallel_markers="$(count_files "$parallel_root" fixture-marker.txt)"
parallel_presets="$(count_files "$parallel_root" fixture-preset-marker.txt)"
parallel_stdout="$(count_files "$parallel_root" stdout.log)"
if [ "$parallel_status" -eq 1 ] \
    && [ "$parallel_markers" -eq 3 ] && [ "$parallel_presets" -eq 3 ] && [ "$parallel_stdout" -eq 3 ] \
    && grep -q 'pass=2 fail=1 ' "${TMP_DIR}/parallel.out" \
    && grep -q 'ParallelFail:FAIL' "${TMP_DIR}/parallel.out"; then
    pass parallel_isolated_run
else
    fail "parallel_isolated_run: status=${parallel_status} markers=${parallel_markers} presets=${parallel_presets} stdout=${parallel_stdout}"
fi

# A test that leaves a file in the home directory fails even with [PASS].
home_root="${TMP_DIR}/home-guard"
set +e
run_suite "$home_root" "${TMP_DIR}/home.out" --test HomeWriter
home_status=$?
set -e
if [ "$home_status" -eq 1 ] && grep -q 'HomeWriter:ERROR' "${TMP_DIR}/home.out"; then
    pass home_write_guard
else
    fail "home_write_guard: status=${home_status}"
fi

# --stop-on-failure stops scheduling; the remaining tests count as not run.
stop_root="${TMP_DIR}/stop"
set +e
run_suite "$stop_root" "${TMP_DIR}/stop.out" -j 1 --stop-on-failure --tests FirstFail,NeverRuns
stop_status=$?
set -e
if [ "$stop_status" -eq 1 ] && grep -q 'not_run=1' "${TMP_DIR}/stop.out" \
    && ! grep -q 'NeverRuns' "${TMP_DIR}/stop.out"; then
    pass stop_on_failure
else
    fail "stop_on_failure: status=${stop_status}"
fi

# Two whole suites at once with opposing outcomes keep separate captures and
# logs.
concurrent_root="${TMP_DIR}/concurrent-suite"
set +e
run_suite "$concurrent_root" "${TMP_DIR}/pass-wrapper.out" --test ConcurrentPass &
pass_pid=$!
run_suite "$concurrent_root" "${TMP_DIR}/fail-wrapper.out" --test ConcurrentFail &
fail_pid=$!
wait "$pass_pid"
pass_status=$?
wait "$fail_pid"
fail_status=$?
set -e

log_count="$(find "$concurrent_root" -maxdepth 1 -type f -name 'test-run-*.log' | wc -l | tr -d ' ')"
capture_count="$(find "$concurrent_root" -maxdepth 1 -type d -name 'test-run.*' | wc -l | tr -d ' ')"
marker_count="$(count_files "$concurrent_root" fixture-marker.txt)"
pass_log_count="$( { grep -l 'Suite status: PASS' "$concurrent_root"/test-run-*.log 2>/dev/null || true; } | wc -l | tr -d ' ')"
fail_log_count="$( { grep -l 'Suite status: FAIL' "$concurrent_root"/test-run-*.log 2>/dev/null || true; } | wc -l | tr -d ' ')"
if [ "$pass_status" -eq 0 ] && [ "$fail_status" -eq 1 ] \
    && [ "$log_count" -eq 2 ] && [ "$capture_count" -eq 2 ] && [ "$marker_count" -eq 2 ] \
    && [ "$pass_log_count" -eq 1 ] && [ "$fail_log_count" -eq 1 ]; then
    pass concurrent_suite_wiring
else
    fail "concurrent_suite_wiring: pass=${pass_status} fail=${fail_status} logs=${log_count} captures=${capture_count} markers=${marker_count} passLogs=${pass_log_count} failLogs=${fail_log_count}"
fi

# A shared explicit log path may be reused by accident: each suite assembles
# its log privately and publishes one complete record, never an interleaving.
explicit_root="${TMP_DIR}/explicit-log-suite"
explicit_log="${explicit_root}/shared.log"
set +e
run_suite "$explicit_root" "${TMP_DIR}/first-wrapper.out" --test ConcurrentPass --log-path "$explicit_log" &
first_pid=$!
run_suite "$explicit_root" "${TMP_DIR}/second-wrapper.out" --test ConcurrentPass --log-path "$explicit_log" &
second_pid=$!
wait "$first_pid"
first_status=$?
wait "$second_pid"
second_status=$?
set -e

shared_start_count="$( { grep -c '^Test run started' "$explicit_log" 2>/dev/null || true; } )"
shared_result_count="$( { grep -c '^result=PASS$' "$explicit_log" 2>/dev/null || true; } )"
shared_status_count="$( { grep -c '^Suite status: PASS$' "$explicit_log" 2>/dev/null || true; } )"
if [ "$first_status" -eq 0 ] && [ "$second_status" -eq 0 ] \
    && [ "$shared_start_count" -eq 1 ] && [ "$shared_result_count" -eq 1 ] && [ "$shared_status_count" -eq 1 ]; then
    pass atomic_explicit_log
else
    fail "atomic_explicit_log: first=${first_status} second=${second_status} starts=${shared_start_count} results=${shared_result_count} statuses=${shared_status_count}"
fi

if [ "$failures" -gt 0 ]; then
    echo "Parser fixtures FAILED: ${failures} case(s)"
    exit 1
fi
echo "Parser fixtures PASSED."
