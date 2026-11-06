# Performance & Memory Architecture

This document details WISP's memory architecture, allocation policies, resource pooling, benchmarking results, and profiling methodology.

Related documentation:
- [Architecture](architecture.md)
- [Building & Installation](building.md)
- [Development Guide](development.md)

---

## Engineering Objectives

WISP was engineered to satisfy two primary criteria:
1. **Negligible Idle CPU**: Consume **0.00% CPU** between scheduled timer intervals.
2. **Strictly Bounded Resident Memory (RSS/PSS)**: Avoid memory growth during long-running desktop sessions and eliminate per-frame dynamic heap allocations.

---

## Memory Architecture & Allocation Strategy

### 1. Zero Per-Frame Heap Allocations
During normal rendering and timer ticks, WISP performs **no heap allocations** (`malloc`, `calloc`, `realloc`).
- Frame buffers are mapped once upon widget creation or resize (`mmap` via `memfd_create`).
- Damage tracking uses an in-place stack accumulator (`struct rect dmg`).
- Shape rasterization draws directly into the pre-allocated ARGB8888 buffer pointer.

### 2. Bounded Static Tables & Resource Pools
Rather than dynamic linked lists or resizable vectors, core subsystems use bounded arrays sized to peak desktop requirements:

| Subsystem | Capacity | Allocation Policy | Eviction / Strategy |
|---|---|---|---|
| Active Widgets | `MAX_WIDGETS = 8` | Static array (`struct widget widgets[8]`) | Slot reuse on destroy |
| Connected Monitors | `MAX_OUTPUTS = 8` | Static array (`struct output_entry outputs[8]`) | Dynamic bind/unbind |
| Master Image Cache | `MAX_IMAGES = 4` | Static array (`struct image images[4]`) | Reference counted, free slot reuse |
| Scaled Image Cache | `MAX_SCALED = 8` | Static array (`struct img_scaled scaled[8]`) | LRU tick eviction |
| FreeType Glyph Cache | `GLYPH_SLOTS = 128` | Heap array allocated once at init | Direct-mapped by codepoint (`cp % 128`) |

### 3. Reusable Shared-Memory Buffers
Each widget maintains a single reusable shared-memory buffer.
- Screen tearing is prevented using compositor release events (`wl_buffer.release`) and a tri-state buffer lifecycle (`BUF_AVAILABLE`, `BUF_ATTACHED`, `BUF_BUSY`).
- If a widget is hidden (`widget_hide()`), its surface is unmapped and `madvise(..., MADV_DONTNEED)` informs the Linux kernel that the backing pages can be reclaimed immediately.

### 4. Lua VM Memory Quotas & Accounting Allocator
WISP embeds a single shared Lua 5.4 VM using a custom allocator function:
- Tracks total active byte allocations in `lua_used`.
- Enforces a hard configurable memory ceiling (`WISP_LUA_MEM_LIMIT`, default 16 MB).
- Runs garbage collection automatically when widgets are unloaded or reloaded.

---

## Benchmarks & Memory Targets

Measurements conducted on Linux x86_64 with release builds:

### Binary Footprint (Release Stripped)

| Binary Configuration | Text Section | Data + BSS | Total On-Disk Size |
|---|---|---|---|
| `build-rel/wisp` (with embedded Lua 5.4.7) | ~416 KB | ~11 KB | ~460 KB |
| `build-nolua/wisp` (native C only) | ~162 KB | ~7 KB | ~375 KB |

### Runtime Memory Usage (RSS / PSS)

| Configuration | Typical RSS | Typical PSS | Idle CPU |
|---|---|---|---|
| **Empty Runtime** (no widgets loaded) | 4–6 MB | 2–3 MB | 0.00% |
| **Single Widget** (Digital Clock, 1 Hz tick) | 8–10 MB | 4–5 MB | 0.00% (spikes <0.1% at tick) |
| **Three Widgets** (Clock, Sysmon, Battery) | 12–16 MB | 6–8 MB | 0.00% between ticks |
| **Native-Only Build** (No Lua VM) | 4–7 MB | 2–3 MB | 0.00% |

> [!NOTE]
> RSS includes shared read-only mappings of system libraries (`glibc`, `libwayland-client`, `libfreetype`). PSS (*Proportional Set Size*) and Private Dirty memory accurately represent WISP's exclusive memory footprint.

---

## Profiling & Measurement Methodology

To ensure reproducible benchmarks across revisions, use the following tools:

### 1. Accurate Memory Breakdown (`smaps_rollup`)
Do not rely solely on `top` or `ps` VIRT/RES columns. Read `/proc/$PID/smaps_rollup` for exact kernel accounting:

```bash
pidof wisp
cat /proc/$(pidof wisp)/smaps_rollup | grep -E "(Rss:|Pss:|Private_Dirty:)"
```

### 2. Zero-Allocation Verification with Heaptrack
Verify that steady-state redraws do not allocate heap memory:

```bash
heaptrack ./build-rel/wisp scripts/clock.lua
# Let run for 60 seconds, terminate with SIGINT, and inspect summary
heaptrack_print heaptrack.wisp.*.gz
```

Expected result: Zero heap allocations during steady-state timer ticks.

### 3. CPU Profiling with Perf
Measure idle CPU utilization over a 30-second window:

```bash
perf stat -p $(pidof wisp) -- sleep 30
```

Expected result: Near-zero CPU migrations, 0.00% CPU utilization when idle.

### 4. Memory Leak Detection with AddressSanitizer
Run the debug build under ASan/UBSan:

```bash
./build/wisp --selftest
```

The test suite runs through widget creation, buffer allocation, hot-reload, rollback, and self-destruction, reporting zero leaks upon clean process exit.

---

## Optimization Guidelines for Widget Authors

1. **Choose Conservative Timer Intervals**: Avoid updating widgets faster than the human perception threshold:
   - Clocks: 1000 ms.
   - Resource monitors (CPU/RAM): 2000 ms to 3000 ms.
   - Battery & power status: 5000 ms to 10000 ms.
2. **Avoid Heavy String Formatting in Inner Loops**: Precompute static strings and reuse string buffers where possible.
3. **Keep Files Open or Read Minimal Bytes**: When querying `/proc` or `/sys`, read only the necessary lines and close file descriptors immediately.
4. **Leverage State Persistence**: Store previous counter metrics in `state` instead of creating global tables repeatedly.
5. **Hide Off-Screen Widgets**: Use `wisp.hide()` when widgets are not needed; hidden widgets stop firing timers and release backing memory pages to the OS.
