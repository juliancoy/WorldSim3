#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$ROOT_DIR/build"
DEFAULT_BINARY="$build_dir/worldsim3"

binary="$DEFAULT_BINARY"
output_dir="$ROOT_DIR/profiles/callstack-$(date +%Y%m%d-%H%M%S)"
frequency="99"
call_graph="fp"
event="cycles"
duration=""
pid=""
build="0"
command=()

usage() {
  cat <<'EOF'
Usage:
  scripts/capture-callstack.sh [options] [-- program args...]

Captures native CPU call stacks with perf and writes reports to an output
directory. By default it records ./build/worldsim3.

Options:
  --build                 Run ./build.sh before recording.
  --build-dir DIR         Build directory passed to ./build.sh. Default: build
  --binary PATH           Program to run. Default: ./build/worldsim3
  --pid PID               Attach to an already-running process instead of launching.
  --duration SECONDS      Stop recording after this many seconds.
  --output-dir DIR        Directory for perf.data and generated reports.
  --frequency HZ          Sampling frequency. Default: 99
  --call-graph MODE       perf call graph mode: fp or dwarf. Default: fp
  --event EVENT           perf event. Default: cycles
  --help                  Show this help.

Examples:
  scripts/capture-callstack.sh --build --duration 20
  scripts/capture-callstack.sh --pid "$(pidof worldsim3)" --duration 15
  scripts/capture-callstack.sh --call-graph dwarf -- ./build/worldsim3
EOF
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required command not found: $1" >&2
    exit 1
  fi
}

write_own_function_hierarchy() {
  local report="$1"
  local output="$2"

  awk '
  function trim(s) { sub(/^[[:space:]]+/, "", s); sub(/[[:space:]]+$/, "", s); return s }
  function function_name(sym, name) {
    name = sym
    sub(/\(.*/, "", name)
    sub(/[[:space:]]+const$/, "", name)
    return trim(name)
  }
  function is_project(sym, head) {
    if (sym == "" || sym == "[unknown]" || sym ~ /^#/ || sym ~ /^0x[0-9a-fA-F]+$/ || sym ~ /^[0-9]+$/) return 0
    head = function_name(sym)
    if (head ~ /^(std::|__gnu_cxx::|nlohmann::|Im[A-Za-z_]*|stb[A-Za-z_]*|glfw[A-Za-z_]*|vk[A-Z][A-Za-z_]*|duckdb::|operator new|operator delete|malloc|free|realloc|memcpy|memmove|memset|memcmp|pthread_|X[A-Z]|dlopen|entry_|asm_|do_|x64_|handle_|security_|filemap_|generic_|vfs_|ksys_|rm_|nv_|os_)/) return 0
    if (head ~ /^[_]/ || head ~ /^__/) return 0
    if (head == "main") return 1
    if (head ~ /^\(anonymous namespace\)::/) return 1
    if (head ~ /^[A-Z][A-Za-z0-9_]*::/) return 1
    if (head ~ /^[A-Z][A-Za-z0-9_:~<>]*$/) return 1
    if (head ~ /^[a-z][A-Za-z0-9_:~<>]*[A-Z][A-Za-z0-9_:~<>]*$/) return 1
    return 0
  }
  function clean(sym) {
    gsub(/ \[clone [^]]+\]/, "", sym)
    gsub(/ \[clone [^]]+\] \[clone [^]]+\]/, "", sym)
    return trim(sym)
  }
  {
    raw = $0
    if (raw !~ /--/) next
    depth = int((match(raw, /[^[:space:]]/) - 1) / 3)
    line = raw
    if (line ~ /--[0-9.]+%--/) sub(/^.*--[0-9.]+%--/, "", line)
    else sub(/^.*---/, "", line)
    sym = clean(line)
    while (top > 0 && depth <= retained_depth[top]) {
      delete retained_depth[top]
      --top
    }
    if (!is_project(sym)) next
    name = function_name(sym)
    rows[++n] = top "\t" name
    retained_depth[++top] = depth
  }
  END {
    previous = ""
    for (r = 1; r <= n; ++r) {
      split(rows[r], parts, "\t")
      depth = parts[1]
      name = parts[2]
      if (name == previous) continue
      for (i = 0; i < depth; ++i) printf "  "
      print name
      previous = name
    }
  }
  ' "$report" > "$output"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build)
      build="1"
      shift
      ;;
    --build-dir)
      if [[ "$2" = /* ]]; then
        build_dir="$2"
      else
        build_dir="$ROOT_DIR/${2#./}"
      fi
      binary="$build_dir/worldsim3"
      shift 2
      ;;
    --binary)
      binary="${2:?missing value for --binary}"
      shift 2
      ;;
    --pid)
      pid="${2:?missing value for --pid}"
      shift 2
      ;;
    --duration)
      duration="${2:?missing value for --duration}"
      shift 2
      ;;
    --output-dir)
      output_dir="${2:?missing value for --output-dir}"
      shift 2
      ;;
    --frequency)
      frequency="${2:?missing value for --frequency}"
      shift 2
      ;;
    --call-graph)
      call_graph="${2:?missing value for --call-graph}"
      shift 2
      ;;
    --event)
      event="${2:?missing value for --event}"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      command=("$@")
      break
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -n "$pid" && ${#command[@]} -gt 0 ]]; then
  echo "error: use either --pid or a command after --, not both" >&2
  exit 1
fi

if [[ "$call_graph" != "fp" && "$call_graph" != "dwarf" ]]; then
  echo "error: --call-graph must be fp or dwarf" >&2
  exit 1
fi

require_command perf

cd "$ROOT_DIR"

if [[ "$build" == "1" ]]; then
  CFLAGS="${CFLAGS:-} -fno-omit-frame-pointer" \
    CXXFLAGS="${CXXFLAGS:-} -fno-omit-frame-pointer" \
    "$ROOT_DIR/build.sh" --build-dir "$build_dir"
fi

if [[ -z "$pid" && ${#command[@]} -eq 0 ]]; then
  command=("$binary")
fi

if [[ -z "$pid" && ! -x "${command[0]}" ]]; then
  echo "error: target is not executable: ${command[0]}" >&2
  echo "hint: run with --build, pass --binary PATH, or pass a command after --" >&2
  exit 1
fi

mkdir -p "$output_dir"

perf_data="$output_dir/perf.data"
metadata="$output_dir/metadata.txt"

{
  echo "timestamp=$(date --iso-8601=seconds)"
  echo "root_dir=$ROOT_DIR"
  echo "output_dir=$output_dir"
  echo "frequency=$frequency"
  echo "call_graph=$call_graph"
  echo "event=$event"
  echo "duration=${duration:-until-process-exits-or-ctrl-c}"
  if [[ -n "$pid" ]]; then
    echo "mode=attach"
    echo "pid=$pid"
    ps -p "$pid" -o pid,ppid,comm,args || true
  else
    echo "mode=launch"
    printf 'command='
    printf '%q ' "${command[@]}"
    printf '\n'
  fi
  git rev-parse --short HEAD 2>/dev/null | sed 's/^/git_commit=/'
  cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null | sed 's/^/perf_event_paranoid=/'
  cat /proc/sys/kernel/kptr_restrict 2>/dev/null | sed 's/^/kptr_restrict=/'
} > "$metadata"

record_cmd=(perf record -F "$frequency" -g --call-graph "$call_graph" -e "$event" -o "$perf_data")

if [[ -n "$pid" ]]; then
  record_cmd+=(-p "$pid")
else
  record_cmd+=(-- "${command[@]}")
fi

echo "Writing profile to: $output_dir"
if [[ -n "$duration" ]]; then
  require_command timeout
  timeout --preserve-status --kill-after=10s "$duration" "${record_cmd[@]}" || status=$?
  status="${status:-0}"
  if [[ "$status" != "0" && "$status" != "124" && "$status" != "137" && "$status" != "143" ]]; then
    echo "error: perf record failed with exit status $status" >&2
    exit "$status"
  fi
else
  "${record_cmd[@]}"
fi

if [[ ! -s "$perf_data" ]]; then
  echo "error: perf did not create a non-empty perf.data file" >&2
  exit 1
fi

perf report -i "$perf_data" --stdio --children > "$output_dir/perf-report-with-children.txt" || true
perf report -i "$perf_data" --stdio --no-children > "$output_dir/perf-report-no-children.txt" || true
perf script -i "$perf_data" > "$output_dir/perf-script.txt" || true
write_own_function_hierarchy "$output_dir/perf-report-with-children.txt" "$output_dir/own-function-hierarchy.txt"

if command -v stackcollapse-perf.pl >/dev/null 2>&1; then
  stackcollapse-perf.pl "$output_dir/perf-script.txt" > "$output_dir/perf-folded.txt" || true
fi

if command -v flamegraph.pl >/dev/null 2>&1 && [[ -s "$output_dir/perf-folded.txt" ]]; then
  flamegraph.pl "$output_dir/perf-folded.txt" > "$output_dir/perf-flamegraph.svg" || true
fi

echo "Generated:"
find "$output_dir" -maxdepth 1 -type f -printf '  %f\n' | sort
