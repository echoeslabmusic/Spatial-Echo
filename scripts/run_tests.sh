#!/bin/bash
# Runs the headless test suite in parallel. The default list is whatever the
# runner reports with --list, so the registry in
# plugin/Source/Tests/Headless/HeadlessTestRunner.cpp is the only list.
#
# Usage: ./scripts/run_tests.sh [--debug] [-j N] [--tests A,B] [--test Name]
#                               [--stop-on-failure] [--no-build] [--log-path Path]
#
# Uses the Release headless runner unless --debug is given.
# SPATIAL_ECHO_HEADLESS_RUNNER points at another runner (for example the
# Windows build under Git Bash, with --no-build). SPATIAL_ECHO_HEADLESS_LOG_DIR
# moves the log parent (default: build/).

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT_DIR="$(dirname "${SCRIPT_DIR}")"
CONFIG="release"
JOBS=""
BUILD_FIRST=1
STOP_ON_FAILURE=0
LOG_PATH=""
TESTS=()

# Tests allowed to report [SKIP] without failing the suite: their skip is an
# external-fixture precondition, not a masked defect. Every other [SKIP],
# untagged result, or stderr assertion fails closed.
OPTIONAL_SKIP_TESTS=(
    "PresetRosterRenders"
)

# Tests whose contract deliberately provokes a defensive JUCE assertion
# (undersized host buffers) to prove graceful handling; exactly these keep
# token classification despite assertion text on stderr. Release builds
# compile assertions out, so there the same tests simply report their token.
# Any other test with a stderr assertion fails as ASSERTION.
EXPECTED_ASSERTION_TESTS=(
    "ReverbUndersizedBufferContract"
    "TapeDelayUndersizedBufferContract"
    "ToneBufferBoundsContract"
)

usage() {
    echo "Usage: $0 [--debug] [-j N] [--tests A,B] [--test Name] [--stop-on-failure] [--no-build] [--log-path Path]"
}

contains_name() {
    local candidate="$1"
    shift
    local name
    for name in "$@"; do
        if [ "$name" = "$candidate" ]; then
            return 0
        fi
    done
    return 1
}

is_optional_skip() {
    contains_name "$1" "${OPTIONAL_SKIP_TESTS[@]}"
}

is_expected_assertion() {
    contains_name "$1" "${EXPECTED_ASSERTION_TESTS[@]}"
}

# classify_result <stdout_path> <stderr_path> <exit_code> <test_name>
# Prints exactly one of: PASS FAIL ERROR SKIP_OPTIONAL SKIP_REQUIRED
# UNTAGGED ASSERTION. Only PASS and SKIP_OPTIONAL do not fail the suite.
classify_result() {
    local stdout_path="$1"
    local stderr_path="$2"
    local exit_code="$3"
    local test_name="$4"

    if grep -q 'JUCE Assertion failure' "$stderr_path" 2>/dev/null \
        && ! is_expected_assertion "$test_name"; then
        echo "ASSERTION"
        return
    fi
    if grep -q '\[ERROR\]' "$stdout_path" "$stderr_path" 2>/dev/null; then
        echo "ERROR"
        return
    fi
    if grep -q '\[FAIL\]' "$stdout_path" "$stderr_path" 2>/dev/null; then
        echo "FAIL"
        return
    fi
    if grep -q '\[SKIP\]' "$stdout_path" "$stderr_path" 2>/dev/null; then
        if is_optional_skip "$test_name"; then
            echo "SKIP_OPTIONAL"
        else
            echo "SKIP_REQUIRED"
        fi
        return
    fi
    if grep -q '\[PASS\]' "$stdout_path" 2>/dev/null; then
        if [ "$exit_code" -eq 0 ]; then
            echo "PASS"
        else
            echo "ERROR"
        fi
        return
    fi
    echo "UNTAGGED"
}

append_csv_tests() {
    local csv="$1"
    local old_ifs="$IFS"
    local part
    IFS=','
    for part in $csv; do
        part="${part#"${part%%[![:space:]]*}"}"
        part="${part%"${part##*[![:space:]]}"}"
        if [ -n "$part" ]; then
            TESTS+=("$part")
        fi
    done
    IFS="$old_ifs"
}

# Default parallelism: the performance cores on Apple silicon, otherwise every
# core the system reports.
default_jobs() {
    local count=""
    if command -v sysctl >/dev/null 2>&1; then
        count="$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || true)"
        if [ -z "$count" ]; then
            count="$(sysctl -n hw.physicalcpu 2>/dev/null || true)"
        fi
    fi
    if [ -z "$count" ] && command -v nproc >/dev/null 2>&1; then
        count="$(nproc 2>/dev/null || true)"
    fi
    if [ -z "$count" ]; then
        count="${NUMBER_OF_PROCESSORS:-4}"
    fi
    printf '%s\n' "$count"
}

# One private capture directory per suite invocation, so concurrent suites
# never overwrite each other's per-test files.
create_run_capture_dir() {
    local parent_dir="$1"
    mkdir -p "$parent_dir"
    mktemp -d "${parent_dir}/test-run.XXXXXX"
}

# Artifact root for this invocation; each test gets its own child directory.
# SPATIAL_ECHO_TEST_ARTIFACT_DIR publishes them somewhere else on purpose.
resolve_run_artifact_dir() {
    local capture_dir="$1"
    local artifact_dir="${SPATIAL_ECHO_TEST_ARTIFACT_DIR:-${capture_dir}/artifacts}"
    mkdir -p "$artifact_dir" || return 1
    printf '%s\n' "$artifact_dir"
}

# User-preset tests exercise the real save/load/rename/delete code against a
# preset library private to each test, never the user's own folder.
resolve_run_preset_root() {
    local capture_dir="$1"
    local preset_root="${SPATIAL_ECHO_TEST_PRESET_DIR:-${capture_dir}/presets}"
    mkdir -p "$preset_root" || return 1
    printf '%s\n' "$preset_root"
}

# Assemble the aggregate log privately, then replace the requested path in
# one rename so concurrent callers cannot interleave partial output.
publish_suite_log() {
    local source_path="$1"
    local target_path="$2"
    local target_dir
    local target_name
    local staging_path
    if [ -d "$target_path" ]; then
        return 1
    fi
    target_dir="$(dirname "$target_path")"
    target_name="$(basename "$target_path")"
    mkdir -p "$target_dir" || return 1
    staging_path="$(mktemp "${target_dir}/.${target_name}.XXXXXX")" || return 1
    if cp "$source_path" "$staging_path" && mv -f "$staging_path" "$target_path"; then
        return 0
    fi
    rm -f "$staging_path"
    return 1
}

# Start the slowest tests first: order by the durations the previous run
# recorded (tests without a record go first), keeping list order for ties.
order_by_previous_duration() {
    local durations_file="$1"
    if [ ! -f "$durations_file" ]; then
        return 0
    fi
    local ordered=()
    local name
    while IFS= read -r name; do
        ordered+=("$name")
    done < <(printf '%s\n' "${TESTS[@]}" \
        | awk -v file="$durations_file" '
            BEGIN { while ((getline line < file) > 0) { split(line, f, "\t"); seconds[f[1]] = f[2] } }
            { printf "%s\t%d\t%s\n", (($0 in seconds) ? seconds[$0] : 1e9), NR, $0 }' \
        | sort -t "$(printf '\t')" -k1,1gr -k2,2n | cut -f3)
    TESTS=("${ordered[@]}")
}

# Record this run's durations for the next run's ordering, keeping entries
# for tests that did not run this time. Written to a temporary file and
# renamed, so concurrent suites never leave a partial file.
record_durations() {
    local durations_file="$1"
    local staging
    staging="$(mktemp "${durations_file}.XXXXXX")" || return 0
    {
        local i
        for ((i = 0; i < total; i++)); do
            if [ -n "${OUTCOMES[$i]:-}" ]; then
                printf '%s\t%s\n' "${TESTS[$i]}" "${ELAPSED[$i]}"
            fi
        done
    } > "$staging"
    if [ -f "$durations_file" ]; then
        awk -F '\t' 'NR == FNR { seen[$1] = 1; next } !($1 in seen)' "$staging" "$durations_file" \
            > "${staging}.kept" || true
        cat "${staging}.kept" >> "$staging" 2>/dev/null || true
        rm -f "${staging}.kept"
    fi
    mv -f "$staging" "$durations_file" || rm -f "$staging"
}

# When sourced (parser fixture tests), expose the functions above and stop
# before argument parsing or any build/run side effect.
if [ "${BASH_SOURCE[0]}" != "$0" ]; then
    return 0
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --debug)
            CONFIG="debug"
            shift
            ;;
        -j|--jobs)
            if [ "$#" -lt 2 ] || ! [[ "$2" =~ ^[1-9][0-9]*$ ]]; then
                echo "Error: $1 requires a positive number"
                exit 2
            fi
            JOBS="$2"
            shift 2
            ;;
        -j[1-9]*)
            JOBS="${1#-j}"
            if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
                echo "Error: -j requires a positive number"
                exit 2
            fi
            shift
            ;;
        --tests)
            if [ "$#" -lt 2 ]; then
                echo "Error: --tests requires a comma-separated value"
                exit 2
            fi
            append_csv_tests "$2"
            shift 2
            ;;
        --test)
            if [ "$#" -lt 2 ]; then
                echo "Error: --test requires a value"
                exit 2
            fi
            TESTS+=("$2")
            shift 2
            ;;
        --stop-on-failure)
            STOP_ON_FAILURE=1
            shift
            ;;
        --log-path)
            if [ "$#" -lt 2 ]; then
                echo "Error: --log-path requires a value"
                exit 2
            fi
            LOG_PATH="$2"
            shift 2
            ;;
        --no-build)
            BUILD_FIRST=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown option '$1'"
            usage
            exit 2
            ;;
    esac
done

if [ "$CONFIG" = "debug" ]; then
    PRESET="macos-debug"
    DEFAULT_RUNNER="${ROOT_DIR}/build/cmake/macos-debug/Debug/Spatial-Echo-HeadlessTests"
else
    PRESET="macos-release-headless"
    DEFAULT_RUNNER="${ROOT_DIR}/build/cmake/macos-release-headless/Release/Spatial-Echo-HeadlessTests"
fi
RUNNER="${SPATIAL_ECHO_HEADLESS_RUNNER:-$DEFAULT_RUNNER}"
LOG_DIR="${SPATIAL_ECHO_HEADLESS_LOG_DIR:-${ROOT_DIR}/build}"
if [ -z "$JOBS" ]; then
    JOBS="$(default_jobs)"
fi

if [ "$BUILD_FIRST" -eq 1 ]; then
    cmake --preset "$PRESET"
    cmake --build --preset "$PRESET" --target SpatialEchoHeadlessTests
fi
if [ ! -x "$RUNNER" ]; then
    echo "Error: headless runner not found at ${RUNNER}"
    echo "Build it with: cmake --preset ${PRESET} && cmake --build --preset ${PRESET} --target SpatialEchoHeadlessTests"
    exit 2
fi

if [ "${#TESTS[@]}" -eq 0 ]; then
    if ! test_list="$("$RUNNER" --list)"; then
        echo "Error: ${RUNNER} --list failed"
        exit 2
    fi
    while IFS= read -r test_name; do
        test_name="${test_name%$'\r'}"
        if [ -n "$test_name" ]; then
            TESTS+=("$test_name")
        fi
    done <<< "$test_list"
fi

if [ "${#TESTS[@]}" -eq 0 ]; then
    echo "Error: no tests selected"
    exit 2
fi

DURATIONS_FILE="${LOG_DIR}/test-durations-${CONFIG}.txt"
mkdir -p "$LOG_DIR"
order_by_previous_duration "$DURATIONS_FILE"

capture_dir="$(create_run_capture_dir "$LOG_DIR")"
artifact_root="$(resolve_run_artifact_dir "$capture_dir")"
preset_root="$(resolve_run_preset_root "$capture_dir")"
if [ -z "$LOG_PATH" ]; then
    capture_suffix="${capture_dir##*.}"
    LOG_PATH="${LOG_DIR}/test-run-${CONFIG}-$(date +%Y%m%d_%H%M%S)_${capture_suffix}.log"
fi
RUN_LOG_PATH="${capture_dir}/suite.log"

total="${#TESTS[@]}"
echo "Running ${total} test(s) with ${JOBS} job(s) on ${RUNNER}"

# Start one test in the background. Its files live in a directory named after
# its position in the list: stdout.log, stderr.log, exit_code, elapsed.
# CFFIXED_USER_HOME points macOS home-directory lookups at an empty folder, so
# a test that writes to the home directory is caught (see home_files below)
# instead of touching the real one.
start_test() {
    local index="$1"
    local name="${TESTS[$index]}"
    local safe_name
    safe_name="$(printf '%s' "$name" | sed 's/[^A-Za-z0-9_-]/_/g')"
    local slot
    slot="$(printf '%03d' $((index + 1)))_${safe_name}"
    local test_dir="${capture_dir}/${slot}"
    local artifact_dir="${artifact_root}/${slot}"
    local preset_dir="${preset_root}/${slot}"
    mkdir -p "$test_dir/home" "$artifact_dir" "$preset_dir"
    TEST_DIRS[index]="$test_dir"
    (
        export SPATIAL_ECHO_TEST_ARTIFACT_DIR="$artifact_dir"
        export SPATIAL_ECHO_TEST_PRESET_DIR="$preset_dir"
        export CFFIXED_USER_HOME="$test_dir/home"
        TIMEFORMAT='%R'
        set +e
        { time "$RUNNER" --test "$name" > "$test_dir/stdout.log" 2> "$test_dir/stderr.log"; \
          echo "$?" > "$test_dir/exit_code"; } 2> "$test_dir/elapsed"
    ) &
    PIDS[index]=$!
}

home_files() {
    find "$1/home" -type f 2>/dev/null | awk 'NR <= 5' || true
}

FAILURES=()
RESULTS=()
TEST_DIRS=()
PIDS=()
OUTCOMES=()
ELAPSED=()
count_pass=0
count_fail=0
count_error=0
count_skip_optional=0
count_skip_required=0
count_untagged=0
count_assertion=0
completed=0
next_index=0
running=0
stop_scheduling=0
suite_start=$SECONDS
trap 'kill $(jobs -p) 2>/dev/null; exit 130' INT TERM

finish_test() {
    local index="$1"
    local name="${TESTS[$index]}"
    local test_dir="${TEST_DIRS[$index]}"
    wait "${PIDS[$index]}" 2>/dev/null || true
    PIDS[index]=""
    local exit_code
    exit_code="$(cat "$test_dir/exit_code" 2>/dev/null || echo 2)"
    local elapsed
    elapsed="$(tr -d ' \r\n' < "$test_dir/elapsed" 2>/dev/null || true)"
    elapsed="${elapsed:-0}"
    local outcome
    outcome="$(classify_result "$test_dir/stdout.log" "$test_dir/stderr.log" "$exit_code" "$name")"
    local stray
    stray="$(home_files "$test_dir")"
    if [ -n "$stray" ] && { [ "$outcome" = "PASS" ] || [ "$outcome" = "SKIP_OPTIONAL" ]; }; then
        outcome="ERROR"
        printf '[ERROR] wrote outside its test directory:\n%s\n' "$stray" >> "$test_dir/stderr.log"
    fi
    OUTCOMES[index]="$outcome"
    ELAPSED[index]="$elapsed"
    completed=$((completed + 1))

    case "$outcome" in
        PASS)          count_pass=$((count_pass + 1)) ;;
        SKIP_OPTIONAL) count_skip_optional=$((count_skip_optional + 1)) ;;
        FAIL)          count_fail=$((count_fail + 1)) ;;
        ERROR)         count_error=$((count_error + 1)) ;;
        SKIP_REQUIRED) count_skip_required=$((count_skip_required + 1)) ;;
        UNTAGGED)      count_untagged=$((count_untagged + 1)) ;;
        ASSERTION)     count_assertion=$((count_assertion + 1)) ;;
    esac
    printf '[%3d/%d] %-13s %-46s %8ss\n' "$completed" "$total" "$outcome" "$name" "$elapsed"
    if [ "$outcome" != "PASS" ] && [ "$outcome" != "SKIP_OPTIONAL" ]; then
        FAILURES+=("${name}:${outcome}")
        sed -n '1,20p' "$test_dir/stdout.log" | sed 's/^/    /'
        if [ "$STOP_ON_FAILURE" -eq 1 ]; then
            stop_scheduling=1
        fi
    fi
}

# Bash 3.2 (the macOS /bin/bash) has no `wait -n`, so poll the running jobs.
while :; do
    while [ "$stop_scheduling" -eq 0 ] && [ "$running" -lt "$JOBS" ] && [ "$next_index" -lt "$total" ]; do
        start_test "$next_index"
        next_index=$((next_index + 1))
        running=$((running + 1))
    done
    if [ "$running" -eq 0 ]; then
        break
    fi
    sleep 0.2
    for ((i = 0; i < next_index; i++)); do
        pid="${PIDS[$i]:-}"
        if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
            finish_test "$i"
            running=$((running - 1))
        fi
    done
done
wall_seconds=$((SECONDS - suite_start))
not_run=$((total - completed))

{
    echo "Test run started $(date +%Y-%m-%dT%H:%M:%S) (${CONFIG})"
    echo "Runner: ${RUNNER}"
    echo "Jobs: ${JOBS}"
    echo "Capture: ${capture_dir}"
    echo "Artifact root: ${artifact_root}"
    echo "Preset root: ${preset_root}"
    printf 'Tests:'
    printf ' %s' "${TESTS[@]}"
    echo
    for ((i = 0; i < total; i++)); do
        if [ -z "${OUTCOMES[$i]:-}" ]; then
            continue
        fi
        test_dir="${TEST_DIRS[$i]}"
        echo "==== ${TESTS[$i]} ===="
        cat "$test_dir/stdout.log" 2>/dev/null || true
        cat "$test_dir/stderr.log" 2>/dev/null || true
        echo "exit=$(cat "$test_dir/exit_code" 2>/dev/null || echo '?') elapsed=${ELAPSED[$i]}s"
        echo "result=${OUTCOMES[$i]}"
    done
} > "$RUN_LOG_PATH"

for ((i = 0; i < total; i++)); do
    if [ -n "${OUTCOMES[$i]:-}" ]; then
        RESULTS+=("${ELAPSED[$i]} ${TESTS[$i]}")
    fi
done

record_durations "$DURATIONS_FILE"

SUMMARY="pass=${count_pass} fail=${count_fail} error=${count_error} skip_optional=${count_skip_optional} skip_required=${count_skip_required} untagged=${count_untagged} assertion=${count_assertion} not_run=${not_run}"
{
    echo
    echo "Suite summary: ${SUMMARY}"
    echo "Wall time: ${wall_seconds}s with ${JOBS} job(s)"
    echo "Slowest tests:"
    printf '%s\n' "${RESULTS[@]}" | sort -rn | awk 'NR <= 10 { printf "  %8ss  %s\n", $1, $2 }'
    if [ "${#FAILURES[@]}" -gt 0 ]; then
        echo "Failures:"
        printf '  %s\n' "${FAILURES[@]}" | sort
    fi
} | tee -a "$RUN_LOG_PATH"

if [ "${#FAILURES[@]}" -gt 0 ] || [ "$not_run" -gt 0 ]; then
    echo "Suite status: FAIL" >> "$RUN_LOG_PATH"
    status=1
else
    echo "Suite status: PASS" >> "$RUN_LOG_PATH"
    status=0
fi

if ! publish_suite_log "$RUN_LOG_PATH" "$LOG_PATH"; then
    echo "Error: failed to publish suite log to ${LOG_PATH}"
    echo "Private log: ${RUN_LOG_PATH}"
    exit 2
fi
if [ "$status" -eq 0 ]; then
    echo "Suite PASSED."
else
    echo "Suite FAILED."
fi
echo "Log: ${LOG_PATH}"
echo "Per-test logs: ${capture_dir}"
exit "$status"
