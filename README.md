# Swelter ECS

An archetype-based Entity Component System written in C++23 for **HeatStroke Engine**, the 2D engine I am building from scratch for my game *Cry War*.

> **Status: work in progress.** The core storage of the archetype ECS is done (component registry, chunk allocator, SoA archetypes and the entity table). The world, queries, command buffers and the job system are next (see [Status & roadmap](#status--roadmap)).

## Why

*Cry War* is a 2D RTS: thousands of units and projectiles that share a handful of component combinations and are updated every frame. That workload rewards contiguous, cache-friendly data and parallel iteration.

Instead of starting with the final design, I am building the ECS in steps and **measuring every step** against [EnTT](https://github.com/skypjack/entt) with Google Benchmark, so each design decision is backed by data.

## From the first ECS to archetypes

The first version (branch `sw/ecs/basic/1.0`) used one **slotmap per component**, generational handles and per-system caches of matching entities. Benchmarked against EnTT v4.0.0 (19 tests, up to 1M entities), it was faster in 7 of 19 tests, but it lost badly where it mattered:

| Problem | Measurement | Cause |
|---|---|---|
| Iteration | ~8–13× slower than EnTT (game scenario, 1M entities: 7.3 ms vs 0.71 ms) | Reading several components jumps between slotmaps: entity → key → slotmap → data |
| Structural changes with cached systems | 474× and 365× slower (add/remove component, destroy entity, 100K entities) | Removing an entity from a cache is a linear `erase`, so the cost is quadratic |

Both problems come from the storage model, not from micro-optimizations, so the second version is a redesign around **archetypes**: entities with the same set of components are stored together and contiguously, and queries cache archetypes instead of entities.

## Architecture

```
Archetype {Position, Health}
 ├─ Chunk 0  (16 KB, 64-byte aligned)
 │   ┌─────────────┬───┬───────────────┬───┬─────────────┐
 │   │ Entity[816] │pad│ Position[816] │pad│ Health[816] │
 │   └─────────────┴───┴───────────────┴───┴─────────────┘
 │    offset 0          offset 6528         offset 13056
 ├─ Chunk 1  (full)
 └─ Chunk 2  (last one, partially filled)
```

- **Component registry.** Any C++ type that satisfies a `Component` concept can be used without declaring it first. It gets a runtime ID on first use and a type-erased `ComponentInfo` (size, alignment, move and destroy functions), so the storage can handle types it does not know at compile time.
- **Chunks and chunk pool.** Rows live in fixed-size 16 KB chunks aligned to a cache line. A global pool recycles them in LIFO order: growing an archetype never reallocates or copies existing data, and since all chunks have the same size there is no external fragmentation. In debug builds chunks are filled with `0xCD`/`0xDD` patterns and double frees are caught.
- **SoA archetypes.** Inside a chunk there is one column per component (entity column first, then components sorted by ID), each one aligned to 64 bytes. The capacity and the column offsets are computed once per archetype. Removing a row moves the last row of the last chunk into the hole, so every chunk except the last one is always full. Trivially copyable components are moved with `memcpy`; the rest through their move constructor and destructor.
- **Entity table.** 64-bit entity IDs (32-bit index + 32-bit generation) detect stale handles. A paged table maps each entity to `{archetype, chunk, row}` without hashing or divisions (page = shift, slot = mask), and pages never move. Freed indices are reused in LIFO order through an implicit free list stored inside the free records themselves, and reserving an ID is separate from placing the entity, ready for deferred creation.

## Key design decisions

Each decision was made after comparing alternatives (mostly Unity DOTS, flecs, Bevy and EnTT):

- **Archetypes over sparse sets:** contiguous iteration that splits naturally into blocks for worker threads, at the cost of more expensive structural changes (to be mitigated with an archetype graph and command buffers).
- **Fixed-size chunks over growing columns:** no reallocation spikes, stable memory, and the chunk is the natural unit of work for the job system.
- **64-byte column alignment, not 4 KB:** no false sharing between columns and aligned SIMD access, with at most 64 bytes of padding per column.
- **Tags are part of the archetype** as zero-size components: filtering is free, but toggling a tag is a structural change.
- **Queries will cache archetypes, not entities:** structural changes never touch them, which removes the quadratic cost of the first version.
- **Structural changes during iteration go through explicit command buffers,** applied at sync points, with a deterministic order (needed for a lockstep RTS).

## Status & roadmap

| Step | State |
|---|---|
| First ECS with slotmaps + benchmarks vs EnTT | Done |
| Component registry (runtime IDs, type-erased info) | Done |
| Fixed-size chunks and chunk pool | Done |
| SoA archetypes | Done |
| Paged entity table with generational IDs | Done |
| World and archetype graph (cached transitions, bundles) | Next |
| Cached queries with incremental archetype matching | Planned |
| Command buffers for deferred structural changes | Planned |
| Systems | Planned |
| Benchmarks: archetype ECS vs first ECS vs EnTT | Planned |
| Job system: parallel iteration over chunks and system scheduling | Planned |

## Where to look

| File | What is interesting there |
|---|---|
| [`code/ecs/include/swelter/registry.hpp`](code/ecs/include/swelter/registry.hpp) | Runtime component IDs and type erasure |
| [`code/ecs/include/swelter/chunk.hpp`](code/ecs/include/swelter/chunk.hpp) | Chunk pool |
| [`code/ecs/include/swelter/archetype.hpp`](code/ecs/include/swelter/archetype.hpp) | SoA layout computation and row removal |
| [`code/ecs/include/swelter/entity_table.hpp`](code/ecs/include/swelter/entity_table.hpp) | Generational IDs, paging and the implicit free list |
| [`sandbox/main.cpp`](sandbox/main.cpp) | Tests, including instance counting to check that no object is lost or duplicated |

The library is header-only; include `<swelter.hpp>`. The public API is documented with Doxygen comments.

## Building

Requirements: CMake ≥ 3.23, Ninja and a C++23 compiler (tested with GCC on Windows through MSYS2).

```sh
cmake --preset debug
cmake --build --preset debug
./build/debug/hsecs_sandbox
```

Use the `release` preset for optimized builds. Debug builds enable the internal assertions and write a log to `log/execution/`.

## Author

Raúl Hidalgo Nicolás

## License

MIT. See [LICENSE](LICENSE).
