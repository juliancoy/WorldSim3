#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-function-profile"
OUTPUT_DIR="$ROOT_DIR/profiles/function-exhaustive-$(date +%Y%m%d-%H%M%S)"
FREQUENCY="999"
CTEST_TIMEOUT=""
CTEST_ARGS=(--output-on-failure)
RUN_BUILD=1

usage() {
  cat <<'EOF'
Usage:
  scripts/capture-exhaustive-functions.sh [options] [-- ctest args...]

Builds a profiling-oriented binary, runs the automated CTest suite under perf,
and writes function-analysis artifacts:

  all-project-functions.txt       static project function names from nm
  observed-project-functions.txt  project function names observed by perf
  missing-project-functions.txt   static names not observed in perf samples
  observed-function-stacks.txt    project-only sampled stack paths, when enabled

Options:
  --build-dir DIR       Build directory. Default: build-function-profile
  --output-dir DIR      Output directory under profiles/.
  --frequency HZ        perf sample frequency. Default: 999
  --no-build            Reuse an existing build directory.
  --timeout SECONDS     Wrap ctest/perf in a timeout.
  --help                Show this help.

Any arguments after -- are passed to ctest. Example:
  scripts/capture-exhaustive-functions.sh -- --label-exclude visual

Set WS3_EXHAUSTIVE_STACK_PATHS=1 to also aggregate unique project stack paths.
That can be slow for large perf-script.txt files.
EOF
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required command not found: $1" >&2
    exit 1
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      if [[ "$2" = /* ]]; then BUILD_DIR="$2"; else BUILD_DIR="$ROOT_DIR/${2#./}"; fi
      shift 2
      ;;
    --output-dir)
      if [[ "$2" = /* ]]; then OUTPUT_DIR="$2"; else OUTPUT_DIR="$ROOT_DIR/${2#./}"; fi
      shift 2
      ;;
    --frequency)
      FREQUENCY="${2:?missing value for --frequency}"
      shift 2
      ;;
    --no-build)
      RUN_BUILD=0
      shift
      ;;
    --timeout)
      CTEST_TIMEOUT="${2:?missing value for --timeout}"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      CTEST_ARGS+=("$@")
      break
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

require_command perf
require_command ctest
require_command nm
require_command comm

cd "$ROOT_DIR"

if [[ "$RUN_BUILD" == "1" ]]; then
  CFLAGS="${CFLAGS:-} -O1 -g -fno-omit-frame-pointer -fno-inline" \
    CXXFLAGS="${CXXFLAGS:-} -O1 -g -fno-omit-frame-pointer -fno-inline" \
    "$ROOT_DIR/build.sh" --build-dir "$BUILD_DIR"
fi

BINARY="$BUILD_DIR/worldsim3"
if [[ ! -x "$BINARY" ]]; then
  echo "error: target binary is not executable: $BINARY" >&2
  exit 1
fi

mkdir -p "$OUTPUT_DIR"

metadata="$OUTPUT_DIR/metadata.txt"
perf_data="$OUTPUT_DIR/perf.data"
perf_script="$OUTPUT_DIR/perf-script.txt"

{
  echo "timestamp=$(date --iso-8601=seconds)"
  echo "root_dir=$ROOT_DIR"
  echo "build_dir=$BUILD_DIR"
  echo "binary=$BINARY"
  echo "output_dir=$OUTPUT_DIR"
  echo "frequency=$FREQUENCY"
  printf 'ctest_args='
  printf '%q ' "${CTEST_ARGS[@]}"
  printf '\n'
  git rev-parse --short HEAD 2>/dev/null | sed 's/^/git_commit=/'
  cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null | sed 's/^/perf_event_paranoid=/'
  cat /proc/sys/kernel/kptr_restrict 2>/dev/null | sed 's/^/kptr_restrict=/'
} > "$metadata"

nm -C --defined-only "$BINARY" |
  awk '
    function project_name(name, head) {
      if (name == "" || name ~ /^0x[0-9a-fA-F]+$/) return 0
      head = name
      sub(/\(.*/, "", head)
      if (head ~ /^(std::|__gnu_cxx::|nlohmann::|Im[A-Za-z_]*|stb[A-Za-z_]*|glfw[A-Za-z_]*|vk[A-Z][A-Za-z_]*|duckdb::|operator new|operator delete)/) return 0
      if (head ~ /^[_]/ || head ~ /^__/) return 0
      if (head == "main") return 1
      if (head ~ /^\(anonymous namespace\)::/) return 1
      if (head ~ /^[A-Z][A-Za-z0-9_]*::/) return 1
      if (head ~ /^[A-Z][A-Za-z0-9_:~<>]*$/) return 1
      if (head ~ /^[a-z][A-Za-z0-9_:~<>]*[A-Z][A-Za-z0-9_:~<>]*$/) return 1
      return 0
    }
    function clean(name) {
      gsub(/ \[clone [^]]+\]/, "", name)
      sub(/\(.*/, "", name)
      sub(/[[:space:]]+const$/, "", name)
      sub(/^[[:space:]]+/, "", name)
      sub(/[[:space:]]+$/, "", name)
      return name
    }
    /^[0-9a-fA-F]+ [TtWw] / {
      name = clean(substr($0, index($0, $3)))
      if (project_name(name)) print name
    }
  ' | sort -u > "$OUTPUT_DIR/all-project-functions.txt"

record_cmd=(perf record -F "$FREQUENCY" -g --call-graph fp -e cycles:u -o "$perf_data" -- ctest --test-dir "$BUILD_DIR" "${CTEST_ARGS[@]}")

echo "Running CTest under perf. Output: $OUTPUT_DIR"
set +e
if [[ -n "$CTEST_TIMEOUT" ]]; then
  timeout --preserve-status --kill-after=20s "$CTEST_TIMEOUT" "${record_cmd[@]}" > "$OUTPUT_DIR/ctest.log" 2>&1
  status=$?
else
  "${record_cmd[@]}" > "$OUTPUT_DIR/ctest.log" 2>&1
  status=$?
fi
set -e
echo "$status" > "$OUTPUT_DIR/ctest-exit-status.txt"

if [[ ! -s "$perf_data" ]]; then
  echo "error: perf did not create a non-empty perf.data file; see $OUTPUT_DIR/ctest.log" >&2
  exit 1
fi

perf report -i "$perf_data" --stdio --children > "$OUTPUT_DIR/perf-report-with-children.txt" || true
perf report -i "$perf_data" --stdio --no-children > "$OUTPUT_DIR/perf-report-no-children.txt" || true
perf script -i "$perf_data" > "$perf_script" || true

awk -v binary="$BINARY" '
  function clean(name) {
    gsub(/ \[clone [^]]+\]/, "", name)
    sub(/\+0x[0-9a-fA-F]+.*/, "", name)
    sub(/\(.*/, "", name)
    sub(/[[:space:]]+const$/, "", name)
    sub(/^[[:space:]]+/, "", name)
    sub(/[[:space:]]+$/, "", name)
    return name
  }
  function project_name(name, head) {
    if (name == "" || name == "[unknown]" || name ~ /^0x[0-9a-fA-F]+$/) return 0
    head = name
    if (head ~ /^(std::|__gnu_cxx::|nlohmann::|Im[A-Za-z_]*|stb[A-Za-z_]*|glfw[A-Za-z_]*|vk[A-Z][A-Za-z_]*|duckdb::|operator new|operator delete|malloc|free|realloc|memcpy|memmove|memset|memcmp|pthread_)/) return 0
    if (head ~ /^[_]/ || head ~ /^__/) return 0
    if (head == "main") return 1
    if (head ~ /^\(anonymous namespace\)::/) return 1
    if (head ~ /^[A-Z][A-Za-z0-9_]*::/) return 1
    if (head ~ /^[A-Z][A-Za-z0-9_:~<>]*$/) return 1
    if (head ~ /^[a-z][A-Za-z0-9_:~<>]*[A-Z][A-Za-z0-9_:~<>]*$/) return 1
    return 0
  }
  /^[[:space:]]+[0-9a-fA-F]+ / && index($0, "(" binary ")") {
    line = $0
    sub(/^[[:space:]]*[0-9a-fA-F]+[[:space:]]+/, "", line)
    name = clean(line)
    if (project_name(name)) print name
  }
  ' "$perf_script" | sort -u > "$OUTPUT_DIR/observed-project-functions.txt"

comm -23 "$OUTPUT_DIR/all-project-functions.txt" "$OUTPUT_DIR/observed-project-functions.txt" > "$OUTPUT_DIR/missing-project-functions.txt"

nm -C --defined-only "$BINARY" |
  awk '/^[0-9a-fA-F]+ [TtWw] / { addr=$1; name=substr($0, index($0,$3)); print addr "\t" name }' \
  > "$OUTPUT_DIR/nm-text-symbols.tsv"
cut -f1 "$OUTPUT_DIR/nm-text-symbols.tsv" | addr2line -e "$BINARY" > "$OUTPUT_DIR/nm-text-symbol-locations.txt"
paste "$OUTPUT_DIR/nm-text-symbols.tsv" "$OUTPUT_DIR/nm-text-symbol-locations.txt" |
  awk -F "\t" '
  function clean(name) {
    sub(/\(.*/, "", name)
    sub(/ \[clone .*/, "", name)
    sub(/[[:space:]]+const$/, "", name)
    sub(/^[[:space:]]+/, "", name)
    sub(/[[:space:]]+$/, "", name)
    return name
  }
  function keep_name(name) {
    if (name == "" || name ~ /^(_|__)/) return 0
    if (name ~ /^(std::|__gnu_cxx::|nlohmann::|Im[A-Za-z_]*|stb[A-Za-z_]*|glfw[A-Za-z_]*|vk[A-Z]|operator new|operator delete)/) return 0
    if (name ~ /std::|__gnu_cxx::|nlohmann::/) return 0
    return 1
  }
  {
    name = clean($2)
    loc = $3
    if (loc !~ /^\/mnt\/Cancer\/worldsim3\//) next
    if (loc ~ /\/third_party\//) next
    if (loc ~ /\/build-function-profile\//) next
    if (keep_name(name)) print name
  }' | sort -u > "$OUTPUT_DIR/source-owned-functions.txt"
comm -12 "$OUTPUT_DIR/source-owned-functions.txt" <(sort -u "$OUTPUT_DIR/observed-project-functions.txt") > "$OUTPUT_DIR/observed-source-owned-functions.txt"
comm -23 "$OUTPUT_DIR/source-owned-functions.txt" "$OUTPUT_DIR/observed-source-owned-functions.txt" > "$OUTPUT_DIR/missing-source-owned-functions.txt"

if [[ "${WS3_EXHAUSTIVE_STACK_PATHS:-0}" == "1" ]]; then
  awk -v binary="$BINARY" '
    function flush(    i,n,out,name) {
      n = 0
      for (i = depth; i >= 1; --i) {
        name = frames[i]
        if (name != "") stack[++n] = name
      }
      if (n > 0) {
        out = stack[1]
        for (i = 2; i <= n; ++i) out = out " -> " stack[i]
        print out
      }
      depth = 0
      delete frames
      delete stack
    }
    function clean(name) {
      gsub(/ \[clone [^]]+\]/, "", name)
      sub(/\+0x[0-9a-fA-F]+.*/, "", name)
      sub(/\(.*/, "", name)
      sub(/[[:space:]]+const$/, "", name)
      sub(/^[[:space:]]+/, "", name)
      sub(/[[:space:]]+$/, "", name)
      return name
    }
    function project_name(name, head) {
      if (name == "" || name == "[unknown]" || name ~ /^0x[0-9a-fA-F]+$/) return 0
      head = name
      if (head ~ /^(std::|__gnu_cxx::|nlohmann::|Im[A-Za-z_]*|stb[A-Za-z_]*|glfw[A-Za-z_]*|vk[A-Z][A-Za-z_]*|duckdb::|operator new|operator delete|malloc|free|realloc|memcpy|memmove|memset|memcmp|pthread_)/) return 0
      if (head ~ /^[_]/ || head ~ /^__/) return 0
      if (head == "main") return 1
      if (head ~ /^\(anonymous namespace\)::/) return 1
      if (head ~ /^[A-Z][A-Za-z0-9_]*::/) return 1
      if (head ~ /^[A-Z][A-Za-z0-9_:~<>]*$/) return 1
      if (head ~ /^[a-z][A-Za-z0-9_:~<>]*[A-Z][A-Za-z0-9_:~<>]*$/) return 1
      return 0
    }
    /^$/ { flush(); next }
    /^[^[:space:]].*cycles:/ { flush(); next }
    /^[[:space:]]+[0-9a-fA-F]+ / && index($0, "(" binary ")") {
      line = $0
      sub(/^[[:space:]]*[0-9a-fA-F]+[[:space:]]+/, "", line)
      name = clean(line)
      if (project_name(name)) frames[++depth] = name
    }
    END { flush() }
    ' "$perf_script" | sort -u > "$OUTPUT_DIR/observed-function-stacks.txt"
else
  : > "$OUTPUT_DIR/observed-function-stacks.txt"
fi

{
  echo "all_project_functions=$(wc -l < "$OUTPUT_DIR/all-project-functions.txt")"
  echo "observed_project_functions=$(wc -l < "$OUTPUT_DIR/observed-project-functions.txt")"
  echo "missing_project_functions=$(wc -l < "$OUTPUT_DIR/missing-project-functions.txt")"
  echo "source_owned_functions=$(wc -l < "$OUTPUT_DIR/source-owned-functions.txt")"
  echo "observed_source_owned_functions=$(wc -l < "$OUTPUT_DIR/observed-source-owned-functions.txt")"
  echo "missing_source_owned_functions=$(wc -l < "$OUTPUT_DIR/missing-source-owned-functions.txt")"
  echo "observed_function_stacks=$(wc -l < "$OUTPUT_DIR/observed-function-stacks.txt")"
  echo "ctest_exit_status=$status"
} > "$OUTPUT_DIR/summary.txt"

cat "$OUTPUT_DIR/summary.txt"
echo "Generated:"
find "$OUTPUT_DIR" -maxdepth 1 -type f -printf '  %f\n' | sort

exit "$status"
