# Capturing Full Function Call Stacks

This document describes how to capture the program's call stacks as function
names, primarily with Linux `perf`. The goal is to produce profiles that show
which functions are running, who called them, and how much sampled time is
attributed to each path.

## What We Want To Capture

A useful call stack profile should answer:

- Which functions are consuming CPU time.
- Which caller paths lead to those functions.
- Whether time is spent in WorldSim3 code, third-party code, graphics drivers,
  kernel code, or system libraries.
- Whether the stacks are complete enough to trust.

For this project, the main target is the native executable:

```bash
./build/worldsim3
```

The app can also be launched through:

```bash
./run.sh
```

However, profiling the final executable directly is usually clearer once the
build already exists.

## Build Requirements

Function names and complete stacks depend heavily on how the binary is built.
Before profiling, prefer a build with debug symbols and frame pointers.

Recommended CMake configuration:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer"
cmake --build build -j"$(nproc)"
```

Why this matters:

- `-g` keeps enough debug information for source/function symbol resolution.
- `-fno-omit-frame-pointer` makes stack unwinding much more reliable.
- `RelWithDebInfo` keeps the program close to release performance while still
  being debuggable.

If the binary is stripped, `perf` may still collect samples, but reports may
show raw addresses or incomplete function names. Avoid stripping the local
profiling build.

## System Setup

Install the usual profiling tools:

```bash
sudo apt-get update
sudo apt-get install -y linux-tools-common linux-tools-generic linux-tools-$(uname -r) \
  elfutils binutils
```

On some systems, kernel perf access is restricted. Check:

```bash
cat /proc/sys/kernel/perf_event_paranoid
```

If needed for local profiling, temporarily relax it:

```bash
sudo sysctl kernel.perf_event_paranoid=1
sudo sysctl kernel.kptr_restrict=0
```

Use the narrowest setting that allows the capture we need, and restore stricter
settings afterwards if this is a shared machine.

## Capture A CPU Call Graph With perf

The repository includes a wrapper script for the workflow described here:

```bash
scripts/capture-callstack.sh --build --duration 20
```

The script runs `./build.sh` when `--build` is provided, records `perf.data`,
and writes text reports plus raw stack samples under `profiles/`.

The most direct workflow is:

```bash
perf record -F 99 -g --call-graph fp -- ./build/worldsim3
```

Then interactively inspect it:

```bash
perf report
```

Useful `perf report` controls:

- Press `Enter` on a hot symbol to expand callers and callees.
- Use caller/callee views to distinguish "this function is expensive" from
  "this function is called by an expensive path."
- Look for `[unknown]`, raw addresses, or broken short stacks; these usually
  mean missing symbols or failed unwinding.

The key options:

- `-F 99` samples at 99 Hz. This is usually enough for an interactive app
  without creating too much overhead.
- `-g` records call graphs.
- `--call-graph fp` uses frame-pointer unwinding. This is reliable when the app
  is built with `-fno-omit-frame-pointer`.
- `-- ./build/worldsim3` runs the app under `perf`.

For a shorter capture, run the app, exercise the slow behavior, then quit the
app. `perf` writes the recording to `perf.data` by default.

## Attach To An Already Running Process

If the app is already running:

```bash
pidof worldsim3
perf record -F 99 -g --call-graph fp -p <pid>
```

Stop the capture with `Ctrl-C`, then inspect:

```bash
perf report
```

This is useful when the issue only appears after a specific UI or data-loading
sequence.

## Capture All Threads

WorldSim3 uses background work in some paths, and graphics drivers may create
additional threads. To capture all threads in the target process, attaching to
the process with `-p <pid>` is usually enough. In `perf report`, check the
thread/process columns if one thread dominates.

To make thread attribution more visible:

```bash
perf report --stdio --sort comm,pid,tid,dso,symbol
```

The `comm`, `pid`, and `tid` fields help separate main-thread rendering, worker
threads, driver threads, and library work.

## Generate A Text Report

For review, save a non-interactive report:

```bash
perf report --stdio --no-children > perf-report.txt
```

Or include caller information:

```bash
perf report --stdio --children > perf-report-with-children.txt
```

Notes:

- `--children` attributes time from callees up into callers. This is often the
  best view for finding expensive high-level paths.
- `--no-children` focuses on where samples landed directly.
- Both views are useful and can tell different parts of the story.

## Generate A Folded Stack File

Folded stacks are useful for flame graphs and for reviewing full call paths in
plain text.

```bash
perf script > perf-script.txt
```

If FlameGraph tools are installed:

```bash
perf script | stackcollapse-perf.pl > perf-folded.txt
flamegraph.pl perf-folded.txt > perf-flamegraph.svg
```

The folded format contains one full stack per line, followed by a sample count.
That makes it easy to search for specific WorldSim3 functions and see their
caller chains.

## DWARF Unwinding Alternative

If frame pointers are unavailable, try DWARF unwinding:

```bash
perf record -F 99 -g --call-graph dwarf -- ./build/worldsim3
```

DWARF unwinding can recover stacks without frame pointers, but it has more
overhead and can still fail with optimized code, JITs, signal frames, or missing
debug information. Prefer frame pointers for repeatable local profiling.

## Resolve Function Names After Capture

If `perf report` shows addresses instead of names:

1. Confirm the binary has symbols:

   ```bash
   file ./build/worldsim3
   readelf -Ws ./build/worldsim3 | head
   ```

2. Confirm debug sections exist:

   ```bash
   readelf -S ./build/worldsim3 | rg '\.debug|\.symtab'
   ```

3. Confirm `perf` is looking at the same binary that was executed.

4. If system libraries are the problem, install matching debug symbol packages.
   On Ubuntu/Debian this may require enabling ddebs/debug symbol repositories.

5. For C++ names, demangling is normally automatic in `perf report`. If needed,
   use:

   ```bash
   perf report --stdio --demangle
   ```

## Capture Kernel And Driver Time

By default, `perf` may include time in kernel functions, Vulkan loader code,
GPU driver userspace libraries, and libc/pthread functions. This is useful:
rendering stalls, memory allocation, synchronization, file I/O, and driver work
often appear outside project-owned source files.

To focus only on userspace:

```bash
perf record -F 99 -g --call-graph fp -e cycles:u -- ./build/worldsim3
```

To include kernel samples where permitted:

```bash
perf record -F 99 -g --call-graph fp -e cycles -- ./build/worldsim3
```

If kernel symbols appear as addresses or `[k]` unknown entries, this is usually
a system symbol permission or package issue, not a WorldSim3 build issue.

## Profiling A Specific Scenario

Recommended process for a useful capture:

1. Build with `RelWithDebInfo`, `-g`, and `-fno-omit-frame-pointer`.
2. Start recording with `perf record`.
3. Exercise exactly one scenario, such as:
   - initial startup,
   - panning/zooming the map,
   - enabling a specific layer,
   - downloading or loading data,
   - waiting during a suspected stall.
4. Stop the app or press `Ctrl-C` if attached.
5. Save both report views:

   ```bash
   perf report --stdio --children > perf-report-with-children.txt
   perf report --stdio --no-children > perf-report-no-children.txt
   ```

6. Optionally save full stack samples:

   ```bash
   perf script > perf-script.txt
   ```

7. Record the exact command, commit, scenario, and approximate duration.

## Similar Tools

`perf` is the baseline because it is available on Linux and works well with
native C++ programs. Other useful tools:

- `hotspot`: GUI viewer for `perf.data`; helpful for call graph exploration.
- `FlameGraph`: converts `perf script` output into SVG flame graphs.
- `uftrace`: function-level tracing; useful for call flow, but higher overhead
  than sampling.
- `gprofng`: another profiler for native Linux applications.
- `valgrind --tool=callgrind`: deterministic call graph profiling with very
  high overhead; useful when runtime slowdown is acceptable.
- `gdb`: can capture a point-in-time stack with `thread apply all bt full`, but
  this is not a statistical performance profile.

For GPU-specific timing, use graphics-focused tools in addition to CPU stack
captures. `perf` can show CPU-side Vulkan and driver activity, but it does not
explain GPU queue execution by itself.

## Interpreting The Result

Treat the first capture as a map, not a verdict. Before changing code, check:

- Are WorldSim3 functions visible by name?
- Are stacks deep enough to show meaningful callers?
- Does the hot path match the scenario that was exercised?
- Is time direct self-time, inherited child time, or blocking in a library?
- Are there multiple hot threads?
- Is the result stable across two or three captures?

If function names are missing or stacks are shallow, fix build flags and symbols
before drawing conclusions from the profile.
