#include <iostream>

#include <swelter.hpp>
#include <log.hpp>
#include <format>
#include <array>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>


void seetype(auto)
{
    std::cout << __PRETTY_FUNCTION__ << "\n";
}

struct TPlayer {};
struct NonMove
{
    NonMove(NonMove&& other) = delete;
};
struct Position
{
    float x;
    float y;
};
struct Health
{
    float h;
};
struct Name
{
    std::string s;
};

// Counts live instances: every constructor adds one and the destructor removes one
struct Tracked
{
    inline static int alive {0};
    int value {0};

    explicit Tracked(int v) noexcept : value(v) { ++alive; }
    Tracked(const Tracked& other) noexcept : value(other.value) { ++alive; }
    Tracked(Tracked&& other) noexcept : value(other.value) { ++alive; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;
    ~Tracked() { --alive; }
};

// Long enough to skip the small string optimization, so the string really owns heap memory
std::string longName(std::size_t i)
{
    return "entity_with_a_long_name_" + std::to_string(i);
}

// is_unique: every pair of types is compared, not only the first one with the rest
compileTimeAssertion(MP::is_unique_v<int>);
compileTimeAssertion(MP::is_unique_v<int, float, double>);
compileTimeAssertion(!MP::is_unique_v<int, int>);
compileTimeAssertion(!MP::is_unique_v<int, float, float>);
compileTimeAssertion(!MP::is_unique_v<int, float, int>);

// Chunk pool. Runs first: the counters assume a pool that has never been used
void testChunkPool()
{
    // LIFO reuse: a returned chunk is the next one handed out
    Swelter::Chunk* first = Swelter::ChunkPool::allocate();
    Swelter::ChunkPool::deallocate(first);
    Swelter::Chunk* reused = Swelter::ChunkPool::allocate();
    debugAssertion(first == reused);
    debugAssertion(Swelter::ChunkPool::ownedCount() == 1);
    debugAssertion(Swelter::ChunkPool::freeCount() == 0);
    Swelter::ChunkPool::deallocate(reused);

    // Alignment and no overlap between any pair of chunks
    std::array<Swelter::Chunk*, 5> chunks;
    for(auto& chunk : chunks)
    {
        chunk = Swelter::ChunkPool::allocate();
    }

    for(std::size_t i{}; i < chunks.size(); ++i)
    {
        [[maybe_unused]] const auto a = reinterpret_cast<std::uintptr_t>(chunks[i]);
        debugAssertion(a % Swelter::COLUMN_ALIGN == 0);

        for(std::size_t j = i + 1; j < chunks.size(); ++j)
        {
            [[maybe_unused]] const auto b = reinterpret_cast<std::uintptr_t>(chunks[j]);
            debugAssertion(std::max(a, b) - std::min(a, b) >= Swelter::CHUNK_SIZE);
        }
    }

    // Counters: the first chunk was reused, so only 5 were ever allocated
    debugAssertion(Swelter::ChunkPool::ownedCount() == chunks.size());
    debugAssertion(Swelter::ChunkPool::freeCount() == 0);

    for(std::size_t i{}; i < 4; ++i)
    {
        Swelter::ChunkPool::deallocate(chunks[i]);
    }
    debugAssertion(Swelter::ChunkPool::ownedCount() == 5);
    debugAssertion(Swelter::ChunkPool::freeCount() == 4);

    // Allocating again must reuse a free chunk, not allocate a new one
    chunks[0] = Swelter::ChunkPool::allocate();
    debugAssertion(Swelter::ChunkPool::ownedCount() == 5);
    debugAssertion(Swelter::ChunkPool::freeCount() == 3);
}

// Layout, archetype rows and columns, removeRow and the archetype destructor.
// Runs right after testChunkPool: it relies on the chunks that test left in the pool
void testArchetype()
{
    Swelter::CmpRegistry::registerComponents<Position,Health>();
    std::vector<Swelter::ComponentInfo> g; 
    g.push_back(Swelter::CmpRegistry::info<Position>()); 
    g.push_back(Swelter::CmpRegistry::info<Health>());
    Swelter::ArchetypeLayout layout =  Swelter::computeLayout(g);
    debugAssertion(layout.capacity == 816);
    debugAssertion(layout.offsets[0] == 6528);
    debugAssertion(layout.offsets[1] == 13056);

    // Archetype: chunks are only requested when the first row arrives
    std::vector<Swelter::CmpId_t> ids = {Swelter::CmpRegistry::id<Position>(),Swelter::CmpRegistry::id<Health>()};
    std::ranges::sort(ids);
    Swelter::Archetype arch {ids};
    const std::uint32_t capacity = arch.layout.capacity;
    debugAssertion(arch.getChunks().empty());
    debugAssertion(arch.bitset.test(Swelter::CmpRegistry::id<Position>()));
    debugAssertion(arch.bitset.test(Swelter::CmpRegistry::id<Health>()));
    debugAssertion(arch.bitset.count() == 2);

    // Fill exactly one chunk: every row lands in chunk 0, in order
    for(std::size_t i{}; i < capacity; ++i)
    {
        [[maybe_unused]] Swelter::RowLocation info = arch.addRow(Swelter::Entity{static_cast<std::uint32_t>(i),static_cast<std::uint32_t>(i)});
        debugAssertion(info.chunk_idx == 0 && info.row == i);
    }
    debugAssertion(arch.getChunks().size() == 1);
    debugAssertion(arch.getChunks()[0].count == capacity);

    // One more row opens a second chunk at row 0
    [[maybe_unused]] Swelter::RowLocation extra = arch.addRow(Swelter::Entity{capacity});
    debugAssertion(extra.chunk_idx == 1 && extra.row == 0);
    debugAssertion(arch.getChunks().size() == 2);
    debugAssertion(arch.getChunks()[1].count == 1);

    // The archetype took its 2 chunks from the free list of the pool (5 owned, 3 free before)
    debugAssertion(Swelter::ChunkPool::ownedCount() == 5);
    debugAssertion(Swelter::ChunkPool::freeCount() == 1);

    // Entity column: each row holds the entity it was added with
    for(std::size_t c{}; c < arch.getChunks().size(); ++c)
    {
        const auto& desc = arch.getChunks()[c];
        [[maybe_unused]] const std::span<Swelter::Entity> entities = arch.getEntities(c);
        for(std::size_t r{}; r < desc.count; ++r)
        {
            debugAssertion(entities[r].index == c * capacity + r);
        }
    }

    // Component columns: start where the layout says and are aligned
    Position* pos_col = arch.getColumn<Position>(0);
    Health* hp_col = arch.getColumn<Health>(0);
    debugAssertion(reinterpret_cast<std::byte*>(pos_col) == arch.getChunks()[0].chunk->data + arch.layout.offsets[0]);
    debugAssertion(reinterpret_cast<std::byte*>(hp_col) == arch.getChunks()[0].chunk->data + arch.layout.offsets[1]);
    debugAssertion(reinterpret_cast<std::uintptr_t>(pos_col) % Swelter::COLUMN_ALIGN == 0);
    debugAssertion(reinterpret_cast<std::uintptr_t>(hp_col) % Swelter::COLUMN_ALIGN == 0);

    // Write every row of chunk 0, then read everything back
    for(std::size_t i{}; i < capacity; ++i)
    {
        std::construct_at(pos_col + i, Position{static_cast<float>(i), static_cast<float>(i) + 0.5f});
        std::construct_at(hp_col + i, Health{static_cast<float>(i) * 2});
    }
    for(std::size_t i{}; i < capacity; ++i)
    {
        debugAssertion(pos_col[i].x == static_cast<float>(i));
        debugAssertion(pos_col[i].y == static_cast<float>(i) + 0.5f);
        debugAssertion(hp_col[i].h == static_cast<float>(i) * 2);
    }

    // Columns of different chunks are independent
    std::construct_at(arch.getColumn<Health>(1), Health{-1.f});
    debugAssertion(arch.getColumn<Health>(1)->h == -1.f);
    debugAssertion(hp_col[0].h == 0.f);

    // removeRow: archetype with trivial (Position, Health) and non trivial (Name) components
    std::vector<Swelter::CmpId_t> rm_ids = {Swelter::CmpRegistry::id<Position>(),
                                                         Swelter::CmpRegistry::id<Health>(),
                                                         Swelter::CmpRegistry::id<Name>()};
    std::ranges::sort(rm_ids);
    Swelter::Archetype rm_arch {rm_ids};
    const std::uint32_t rm_cap = rm_arch.layout.capacity;

    // Full chunk 0 plus 2 rows in chunk 1. Row data depends on the entity, so a moved row can be recognised
    const std::size_t rm_rows = rm_cap + 2;
    for(std::size_t i{}; i < rm_rows; ++i)
    {
        auto [c, r] = rm_arch.addRow(Swelter::Entity{static_cast<std::uint32_t>(i),static_cast<std::uint32_t>(i)});
        std::construct_at(rm_arch.getColumn<Position>(c) + r, Position{static_cast<float>(i), static_cast<float>(i)});
        std::construct_at(rm_arch.getColumn<Health>(c) + r, Health{static_cast<float>(i) * 2});
        std::construct_at(rm_arch.getColumn<Name>(c) + r, Name{longName(i)});
    }
    debugAssertion(rm_arch.getChunks().size() == 2);
    debugAssertion(rm_arch.getChunks()[1].count == 2);
    [[maybe_unused]] const std::size_t free_before = Swelter::ChunkPool::freeCount();

    // 1. Remove a middle row: the last row of the last chunk (entity rm_rows - 1) fills the hole
    [[maybe_unused]] std::optional<Swelter::Entity> moved = rm_arch.removeRow(0, 5);
    [[maybe_unused]] const std::uint32_t last_e = rm_rows - 1;
    debugAssertion(moved.has_value() && moved->index == last_e);
    debugAssertion(rm_arch.getEntities(0)[5].index == last_e);
    debugAssertion(rm_arch.getColumn<Position>(0)[5].x == static_cast<float>(last_e));
    debugAssertion(rm_arch.getColumn<Health>(0)[5].h == static_cast<float>(last_e) * 2);
    debugAssertion(rm_arch.getColumn<Name>(0)[5].s == longName(last_e));
    debugAssertion(rm_arch.getChunks()[1].count == 1);
    // Neighbour rows are untouched
    debugAssertion(rm_arch.getEntities(0)[4].index == 4 && rm_arch.getEntities(0)[6].index == 6);
    debugAssertion(rm_arch.getColumn<Name>(0)[6].s == longName(6));

    // 2. Remove the only row of chunk 1: it is the last row, nothing moves and the chunk goes back to the pool
    debugAssertion(!rm_arch.removeRow(1, 0).has_value());
    debugAssertion(rm_arch.getChunks().size() == 1);
    debugAssertion(rm_arch.getChunks()[0].count == rm_cap);
    debugAssertion(Swelter::ChunkPool::freeCount() == free_before + 1);

    // 3. Remove the last row of chunk 0: nothing moves
    debugAssertion(!rm_arch.removeRow(0, rm_cap - 1).has_value());
    debugAssertion(rm_arch.getChunks()[0].count == rm_cap - 1);

    // 4. Remove a middle row when the last row is in the same chunk
    [[maybe_unused]] const std::uint64_t same_chunk_last = rm_arch.getEntities(0)[rm_cap - 2].index;
    moved = rm_arch.removeRow(0, 10);
    debugAssertion(moved.has_value() && moved->index == same_chunk_last);
    debugAssertion(rm_arch.getEntities(0)[10].index == same_chunk_last);
    debugAssertion(rm_arch.getColumn<Name>(0)[10].s == longName(same_chunk_last));
    debugAssertion(rm_arch.getChunks()[0].count == rm_cap - 2);

    // Destructor: every non trivial component is destroyed exactly once and every chunk goes back to the pool
    [[maybe_unused]] std::size_t free_before_dtor = 0;
    [[maybe_unused]] std::size_t dtor_chunks = 0;
    {
        std::vector<Swelter::CmpId_t> tr_ids = {Swelter::CmpRegistry::id<Position>(),
                                                             Swelter::CmpRegistry::id<Tracked>()};
        std::ranges::sort(tr_ids);
        Swelter::Archetype tr_arch {tr_ids};
        const std::uint32_t tr_cap = tr_arch.layout.capacity;

        // 2 full chunks and 3 rows in a third one
        const std::size_t tr_rows = 2 * tr_cap + 3;
        for(std::size_t i{}; i < tr_rows; ++i)
        {
            auto [c, r] = tr_arch.addRow(Swelter::Entity{static_cast<std::uint32_t>(i),static_cast<std::uint32_t>(i)});
            std::construct_at(tr_arch.getColumn<Position>(c) + r, Position{static_cast<float>(i), 0.f});
            std::construct_at(tr_arch.getColumn<Tracked>(c) + r, static_cast<int>(i));
        }
        debugAssertion(tr_arch.getChunks().size() == 3);
        debugAssertion(Tracked::alive == static_cast<int>(tr_rows));

        // removeRow neither loses nor duplicates objects: one less alive per removed row
        tr_arch.removeRow(0, 7);                                    // middle row, the last row moves into it
        debugAssertion(Tracked::alive == static_cast<int>(tr_rows) - 1);
        debugAssertion(tr_arch.getColumn<Tracked>(0)[7].value == static_cast<int>(tr_rows) - 1);
        tr_arch.removeRow(2, tr_arch.getChunks()[2].count - 1);    // last row, nothing moves
        debugAssertion(Tracked::alive == static_cast<int>(tr_rows) - 2);

        free_before_dtor = Swelter::ChunkPool::freeCount();
        dtor_chunks = tr_arch.getChunks().size();
    }
    debugAssertion(Tracked::alive == 0);
    debugAssertion(Swelter::ChunkPool::freeCount() == free_before_dtor + dtor_chunks);
}

// Entity table: reserve, release, pages, free list, isAlive and place
void testEntityTable()
{
    Swelter::EntityTable table;

    [[maybe_unused]] Swelter::Entity e0 = table.reserve();
    [[maybe_unused]] Swelter::Entity e1 = table.reserve();
    [[maybe_unused]] Swelter::Entity e2 = table.reserve();
    debugAssertion(e0.index == 0 && e0.generation == 0);
    debugAssertion(e1.index == 1 && e1.generation == 0);
    debugAssertion(e2.index == 2 && e2.generation == 0);
    table.release(e2);
    [[maybe_unused]] Swelter::Entity e3 = table.reserve();
    debugAssertion(e3.index == 2 && e3.generation == 1);
    table.release(e3);
    [[maybe_unused]] Swelter::Entity e4 = table.reserve();
    debugAssertion(e4.index == 2 && e4.generation == 2);

    // Null entity: a default constructed Entity is the null one
    debugAssertion(Swelter::Entity{} == Swelter::NULL_ENTITY);
    debugAssertion(e0 != Swelter::NULL_ENTITY);

    // LIFO with two free indices: the last released comes back first, then a new index
    table.release(e0);
    table.release(e1);
    [[maybe_unused]] Swelter::Entity r1 = table.reserve();
    [[maybe_unused]] Swelter::Entity r0 = table.reserve();
    [[maybe_unused]] Swelter::Entity fresh = table.reserve();
    debugAssertion(r1.index == 1 && r1.generation == 1);
    debugAssertion(r0.index == 0 && r0.generation == 1);
    debugAssertion(fresh.index == 3 && fresh.generation == 0);

    // Pages: indices keep growing across the page boundary
    Swelter::EntityTable paged;
    const std::size_t paged_count = Swelter::ENTITY_PAGE_SIZE + 1;
    std::vector<Swelter::Entity> paged_es;
    for(std::size_t i{}; i < paged_count; ++i)
    {
        paged_es.push_back(paged.reserve());
        debugAssertion(paged_es.back().index == i && paged_es.back().generation == 0);
    }

    // Free list across pages: release one index from page 1, then one from page 0
    paged.release(paged_es[Swelter::ENTITY_PAGE_SIZE]);
    paged.release(paged_es[5]);
    [[maybe_unused]] Swelter::Entity from_p0 = paged.reserve();
    [[maybe_unused]] Swelter::Entity from_p1 = paged.reserve();
    debugAssertion(from_p0.index == 5 && from_p0.generation == 1);
    debugAssertion(from_p1.index == Swelter::ENTITY_PAGE_SIZE && from_p1.generation == 1);
    paged_es[5] = from_p0;
    paged_es[Swelter::ENTITY_PAGE_SIZE] = from_p1;

    // Long free list: release everything in order, it comes back in reverse order
    for(const Swelter::Entity& e : paged_es)
    {
        paged.release(e);
    }
    for(std::size_t i{}; i < paged_count; ++i)
    {
        [[maybe_unused]] Swelter::Entity e = paged.reserve();
        [[maybe_unused]] const Swelter::Entity& old = paged_es[paged_count - 1 - i];
        debugAssertion(e.index == old.index && e.generation == old.generation + 1);
    }
    // Free list empty again: the next index is a new one
    [[maybe_unused]] Swelter::Entity next_new = paged.reserve();
    debugAssertion(next_new.index == paged_count);

    // isAlive: the generation tells a live handle from an old copy with the same index
    debugAssertion(table.isAlive(e4));
    debugAssertion(!table.isAlive(e3));                         // same index as e4, older generation
    debugAssertion(!table.isAlive(e0) && !table.isAlive(e1));   // released, their indices are reused by r0 and r1
    debugAssertion(table.isAlive(r0) && table.isAlive(r1));
    debugAssertion(!table.isAlive(Swelter::NULL_ENTITY));
    debugAssertion(!table.isAlive(Swelter::Entity{100, 0}));    // index never handed out

    // place, location and isPlaced: a reserved entity has no row until it is placed
    debugAssertion(!table.isPlaced(fresh));
    table.place(fresh, 7, Swelter::RowLocation{2, 3});
    debugAssertion(table.isPlaced(fresh));
    [[maybe_unused]] const Swelter::EntityLocation fresh_loc = table.location(fresh);
    debugAssertion(fresh_loc.archetype == 7);
    debugAssertion(fresh_loc.location.chunk_idx == 2 && fresh_loc.location.row == 3);

    // A reused index starts unplaced again
    table.release(fresh);
    [[maybe_unused]] Swelter::Entity fresh_again = table.reserve();
    debugAssertion(fresh_again.index == fresh.index && fresh_again.generation == fresh.generation + 1);
    debugAssertion(!table.isPlaced(fresh_again));

#ifdef HS_DEBUG_MODE
    // Retirement: an index whose generation reaches the max never goes back to the free list
    Swelter::EntityTable retire;
    [[maybe_unused]] const Swelter::Entity victim = retire.reserve();
    retire.debugSetGeneration(victim.index, UINT32_MAX - 1);
    [[maybe_unused]] const Swelter::Entity last_gen {victim.index, UINT32_MAX - 1};
    debugAssertion(!retire.isAlive(victim));
    debugAssertion(retire.isAlive(last_gen));
    retire.release(last_gen);
    debugAssertion(!retire.isAlive(last_gen));

    [[maybe_unused]] const Swelter::Entity not_retired = retire.reserve();
    debugAssertion(not_retired.index == victim.index + 1 && not_retired.generation == 0);  // new index, not the retired one
    // Control: a normal index does go back to the free list
    retire.release(not_retired);
    [[maybe_unused]] const Swelter::Entity reused_again = retire.reserve();
    debugAssertion(reused_again.index == not_retired.index && reused_again.generation == 1);
#endif
}

// Entity table + archetype by hand, without the world
void testTableArchetypeIntegration()
{
    {
        std::vector<Swelter::CmpId_t> it_ids = {Swelter::CmpRegistry::id<Position>(),
                                                             Swelter::CmpRegistry::id<Name>()};
        std::ranges::sort(it_ids);
        Swelter::Archetype it_arch {it_ids};
        const Swelter::ArchetypeId_t it_arch_id = 0;
        Swelter::EntityTable it_table;

        // 2 full chunks and 5 rows in a third one. Fresh table: index i is the i-th entity created
        const std::size_t it_rows = 2 * it_arch.layout.capacity + 5;
        std::vector<Swelter::Entity> it_es;
        for(std::size_t i{}; i < it_rows; ++i)
        {
            const Swelter::Entity e = it_table.reserve();
            const Swelter::RowLocation loc = it_arch.addRow(e);
            std::construct_at(it_arch.getColumn<Position>(loc.chunk_idx) + loc.row, Position{static_cast<float>(e.index), 0.f});
            std::construct_at(it_arch.getColumn<Name>(loc.chunk_idx) + loc.row, Name{longName(e.index)});
            it_table.place(e, it_arch_id, loc);
            it_es.push_back(e);
        }

        // Destroy one entity: remove its row, fix the entity moved into the hole, release its id
        auto destroy = [&](const Swelter::Entity victim_e)
        {
            const Swelter::EntityLocation loc = it_table.location(victim_e);
            if(const std::optional<Swelter::Entity> moved_e = it_arch.removeRow(loc.location.chunk_idx, loc.location.row))
            {
                it_table.place(*moved_e, it_arch_id, loc.location);
            }
            it_table.release(victim_e);
        };

        // Every third entity: holes in every chunk, rows moved from the last chunk into the first ones
        [[maybe_unused]] std::size_t destroyed = 0;
        for(std::size_t i{}; i < it_rows; i += 3)
        {
            destroy(it_es[i]);
            ++destroyed;
        }
        // The last row of the archetype: nothing moves
        const std::span<const Swelter::Entity> last_chunk = std::as_const(it_arch).getEntities(it_arch.getChunks().size() - 1);
        destroy(last_chunk.back());
        ++destroyed;

        // Every live entity is where the table says, with its own components
        [[maybe_unused]] std::size_t live = 0;
        for(const Swelter::Entity& e : it_es)
        {
            if(!it_table.isAlive(e)) continue;
            ++live;

            [[maybe_unused]] const Swelter::EntityLocation loc = it_table.location(e);
            debugAssertion(loc.archetype == it_arch_id);
            debugAssertion(it_arch.getEntities(loc.location.chunk_idx)[loc.location.row] == e);
            debugAssertion(it_arch.getColumn<Position>(loc.location.chunk_idx)[loc.location.row].x == static_cast<float>(e.index));
            debugAssertion(it_arch.getColumn<Name>(loc.location.chunk_idx)[loc.location.row].s == longName(e.index));
        }

        // Rows in the archetype == live entities in the table
        [[maybe_unused]] std::size_t rows = 0;
        for(const auto& desc : it_arch.getChunks())
        {
            rows += desc.count;
        }
        debugAssertion(live == it_rows - destroyed);
        debugAssertion(rows == live);
    }
}

// World: archetype lookup and graph edges (needs the debug-only accessors)
void testWorldGraph()
{
#ifdef HS_DEBUG_MODE
    using Swelter::CmpRegistry;

    Swelter::World w;
    const Swelter::ArchetypeId_t root = 0;
    const Swelter::CmpId_t pos = CmpRegistry::id<Position>();
    const Swelter::CmpId_t hp  = CmpRegistry::id<Health>();
    const Swelter::CmpId_t tag = CmpRegistry::id<TPlayer>();

    // A new world only has the root archetype: no components
    debugAssertion(w.debugArchetypeCount() == 1);
    debugAssertion(w.debugArchetype(root).ids.empty());

    // Cold path: the same list gives the same archetype, a different one creates a new archetype
    std::vector<Swelter::CmpId_t> pos_hp_ids = {pos, hp};
    std::ranges::sort(pos_hp_ids);
    [[maybe_unused]] const Swelter::ArchetypeId_t pos_hp = w.debugFindOrCreateArchetype(pos_hp_ids);
    debugAssertion(w.debugFindOrCreateArchetype(pos_hp_ids) == pos_hp);
    debugAssertion(w.debugArchetypeCount() == 2);
    debugAssertion(w.debugFindOrCreateArchetype({}) == root);

    // root + Position: the first time creates the archetype and the edge
    [[maybe_unused]] const Swelter::ArchetypeId_t only_pos = w.debugArchetypeWith(root, pos);
    debugAssertion(w.debugArchetypeCount() == 3);
    debugAssertion(w.debugArchetype(only_pos).ids == std::vector<Swelter::CmpId_t>{pos});
    debugAssertion(w.debugArchetype(root).add_edges.size() == 1);

    // The second time follows the edge: no new archetype and no new edge
    debugAssertion(w.debugArchetypeWith(root, pos) == only_pos);
    debugAssertion(w.debugArchetypeCount() == 3);
    debugAssertion(w.debugArchetype(root).add_edges.size() == 1);

    // The inverse edge was stored too: removing Position goes back to the root without creating anything
    debugAssertion(w.debugArchetype(only_pos).remove_edges.size() == 1);
    debugAssertion(w.debugArchetypeWithout(only_pos, pos) == root);
    debugAssertion(w.debugArchetypeCount() == 3);

    // The order components are added in doesn't matter: {Position} + Health == {Health} + Position
    debugAssertion(w.debugArchetypeWith(only_pos, hp) == pos_hp);
    debugAssertion(w.debugArchetypeCount() == 3);                              // pos_hp already existed
    [[maybe_unused]] const Swelter::ArchetypeId_t only_hp = w.debugArchetypeWith(root, hp);
    debugAssertion(w.debugArchetypeCount() == 4);
    debugAssertion(w.debugArchetypeWith(only_hp, pos) == pos_hp);
    debugAssertion(w.debugArchetypeCount() == 4);

    // Removing walks back through the inverse edges created above
    debugAssertion(w.debugArchetypeWithout(pos_hp, hp) == only_pos);
    debugAssertion(w.debugArchetypeWithout(pos_hp, pos) == only_hp);
    debugAssertion(w.debugArchetypeCount() == 4);

    // Adding a component it already has, or removing one it doesn't have: same archetype, no edges stored
    [[maybe_unused]] const std::size_t pos_hp_add_edges = w.debugArchetype(pos_hp).add_edges.size();
    debugAssertion(w.debugArchetypeWith(pos_hp, pos) == pos_hp);
    debugAssertion(w.debugArchetype(pos_hp).add_edges.size() == pos_hp_add_edges);
    [[maybe_unused]] const std::size_t root_remove_edges = w.debugArchetype(root).remove_edges.size();
    debugAssertion(w.debugArchetypeWithout(root, pos) == root);
    debugAssertion(w.debugArchetype(root).remove_edges.size() == root_remove_edges);

    // A tag is part of the identity: {Position} and {Position, TPlayer} are different archetypes
    [[maybe_unused]] const Swelter::ArchetypeId_t pos_tag = w.debugArchetypeWith(only_pos, tag);
    debugAssertion(pos_tag != only_pos);
    debugAssertion(w.debugArchetype(pos_tag).bitset.test(tag));
    debugAssertion(w.debugArchetypeWithout(pos_tag, tag) == only_pos);
#endif
}

// World: create, get and has
void testWorldCreate()
{
    Swelter::World w;

    // rvalues
    [[maybe_unused]] const Swelter::Entity e = w.create(Position{1.f, 2.f}, Health{3.f});
    debugAssertion(w.get<Position>(e).x == 1.f && w.get<Position>(e).y == 2.f);
    debugAssertion(w.get<Health>(e).h == 3.f);
    debugAssertion(w.has<Position>(e) && w.has<Health>(e));
    debugAssertion(!w.has<Name>(e) && !w.has<TPlayer>(e));

    // get returns a reference into the chunk: writing through it changes the stored value
    w.get<Health>(e).h = 10.f;
    debugAssertion(w.get<Health>(e).h == 10.f);

    // const get
    [[maybe_unused]] const Swelter::World& cw = w;
    debugAssertion(cw.get<Position>(e).y == 2.f);

    // lvalues and const lvalues are copied, the originals are untouched
    Position p {5.f, 6.f};
    const Health h {7.f};
    [[maybe_unused]] const Swelter::Entity e_lvalue = w.create(p, h);
    debugAssertion(w.get<Position>(e_lvalue).x == 5.f && w.get<Health>(e_lvalue).h == 7.f);
    w.get<Position>(e_lvalue).x = -1.f;
    debugAssertion(p.x == 5.f);

    // Non trivial component
    [[maybe_unused]] const Swelter::Entity e_name = w.create(Name{longName(1)});
    debugAssertion(w.get<Name>(e_name).s == longName(1));

    // Tags have no data, but has() sees them
    [[maybe_unused]] const Swelter::Entity e_tag = w.create(Position{}, TPlayer{});
    debugAssertion(w.has<TPlayer>(e_tag) && w.has<Position>(e_tag));

    // No components: the entity lives in the root archetype
    [[maybe_unused]] const Swelter::Entity e_empty = w.create();
    debugAssertion(!w.has<Position>(e_empty));

    // Different entities get different ids
    debugAssertion(e != e_lvalue && e_lvalue != e_name);

#ifdef HS_DEBUG_MODE
    // The order of the arguments doesn't matter: same archetype
    [[maybe_unused]] const Swelter::Entity e_swapped = w.create(Health{1.f}, Position{2.f, 3.f});
    debugAssertion(w.debugArchetypeOf(e_swapped) == w.debugArchetypeOf(e));
    debugAssertion(w.debugArchetypeOf(e_lvalue) == w.debugArchetypeOf(e));
    debugAssertion(w.get<Position>(e_swapped).y == 3.f);

    debugAssertion(w.debugArchetypeOf(e_tag) != w.debugArchetypeOf(e));
    debugAssertion(w.debugArchetypeOf(e_empty) == 0);
#endif
}

// World: destroy
void testWorldDestroy()
{
    [[maybe_unused]] const int alive_before = Tracked::alive;
    {
        Swelter::World w;

        // Only the components stored in the world count: the temporaries passed to create are already destroyed
        std::vector<Swelter::Entity> es;
        for(int i{}; i < 3; ++i)
        {
            es.push_back(w.create(Position{static_cast<float>(i), 0.f}, Tracked{i}));
        }
        debugAssertion(Tracked::alive == alive_before + 3);

        debugAssertion(w.valid(es[0]) && w.valid(es[1]) && w.valid(es[2]));
        debugAssertion(!w.valid(Swelter::NULL_ENTITY));

        // Destroy the first one: the last row (es[2]) fills its hole and the table follows it
        w.destroy(es[0]);
        debugAssertion(!w.valid(es[0]));
        debugAssertion(w.valid(es[1]) && w.valid(es[2]));
        debugAssertion(Tracked::alive == alive_before + 2);
        debugAssertion(w.get<Tracked>(es[2]).value == 2 && w.get<Position>(es[2]).x == 2.f);
        debugAssertion(w.get<Tracked>(es[1]).value == 1 && w.get<Position>(es[1]).x == 1.f);

        // Destroy the last row: nothing moves
        w.destroy(es[2]);
        debugAssertion(Tracked::alive == alive_before + 1);
        debugAssertion(w.get<Tracked>(es[1]).value == 1);

        // The released index is reused (LIFO) with the next generation
        [[maybe_unused]] const Swelter::Entity reused = w.create(Position{}, Tracked{9});
        debugAssertion(reused.index == es[2].index && reused.generation == es[2].generation + 1);
        debugAssertion(w.valid(reused) && !w.valid(es[2]));     // same index, only the new handle is valid
        debugAssertion(w.get<Tracked>(reused).value == 9);
        debugAssertion(Tracked::alive == alive_before + 2);

        // Entity without components (root archetype)
        const Swelter::Entity empty = w.create();
        debugAssertion(w.valid(empty));
        w.destroy(empty);
        debugAssertion(!w.valid(empty));
    }
    // The world destructor destroys the components still alive
    debugAssertion(Tracked::alive == alive_before);

    // Many entities over several chunks: every live entity keeps its own components
    {
        Swelter::World w;
        std::vector<Swelter::Entity> es;
        const std::size_t count = 3000;     // a few chunks of {Position, Name}
        for(std::size_t i{}; i < count; ++i)
        {
            es.push_back(w.create(Position{static_cast<float>(i), 0.f}, Name{longName(i)}));
        }

        // Every third entity: holes in every chunk, filled with rows from the last one
        std::vector<bool> alive(count, true);
        for(std::size_t i{}; i < count; i += 3)
        {
            w.destroy(es[i]);
            alive[i] = false;
        }

        for(std::size_t i{}; i < count; ++i)
        {
            debugAssertion(w.valid(es[i]) == alive[i]);
            if(!alive[i]) continue;
            debugAssertion(w.get<Position>(es[i]).x == static_cast<float>(i));
            debugAssertion(w.get<Name>(es[i]).s == longName(i));
        }
    }

    // Destroying a dead entity stops the program in debug (runTimeAssertion), so it is not tested here
}

// World: add and remove (moving an entity between archetypes)
void testWorldAddRemove()
{
    // Values survive every move, in both directions
    {
        Swelter::World w;
        const Swelter::Entity e = w.create(Position{1.f, 2.f});

        // rvalue
        w.add(e, Health{3.f});
        debugAssertion(w.has<Position>(e) && w.has<Health>(e));
        debugAssertion(w.get<Position>(e).x == 1.f && w.get<Position>(e).y == 2.f);
        debugAssertion(w.get<Health>(e).h == 3.f);

        // lvalue: copied, the original keeps its value
        Name name {longName(1)};
        w.add(e, name);
        debugAssertion(name.s == longName(1));
        debugAssertion(w.get<Name>(e).s == longName(1));
        debugAssertion(w.get<Position>(e).y == 2.f && w.get<Health>(e).h == 3.f);

        // Remove them one by one, down to the root archetype
        w.remove<Health>(e);
        debugAssertion(!w.has<Health>(e));
        debugAssertion(w.get<Position>(e).x == 1.f && w.get<Name>(e).s == longName(1));

        w.remove<Name>(e);
        debugAssertion(!w.has<Name>(e) && w.get<Position>(e).y == 2.f);

        w.remove<Position>(e);
        debugAssertion(!w.has<Position>(e));
#ifdef HS_DEBUG_MODE
        debugAssertion(w.debugArchetypeOf(e) == 0);
#endif

        // From the root archetype, with a const lvalue
        const Health h {8.f};
        w.add(e, h);
        debugAssertion(w.get<Health>(e).h == 8.f);
    }

    // Live instances: a move creates and destroys nothing beyond the component added or removed
    [[maybe_unused]] const int alive_before = Tracked::alive;
    {
        Swelter::World w;
        const Swelter::Entity e = w.create(Position{1.f, 2.f}, Tracked{7});
        debugAssertion(Tracked::alive == alive_before + 1);

        // Only Tracked is removed. Together with the next block, both orders of the IDs are covered
        w.remove<Tracked>(e);
        debugAssertion(Tracked::alive == alive_before);
        debugAssertion(w.get<Position>(e).x == 1.f);

        w.add(e, Tracked{8});
        debugAssertion(Tracked::alive == alive_before + 1);
        debugAssertion(w.get<Tracked>(e).value == 8 && w.get<Position>(e).y == 2.f);

        // Tracked is moved, not removed
        w.remove<Position>(e);
        debugAssertion(Tracked::alive == alive_before + 1);
        debugAssertion(w.get<Tracked>(e).value == 8);

        w.add(e, Health{1.f});
        debugAssertion(Tracked::alive == alive_before + 1);
        debugAssertion(w.get<Tracked>(e).value == 8);

        // Last component of the destination list: the rest of the source must still be destroyed
        const Swelter::Entity only = w.create(Tracked{9});
        debugAssertion(Tracked::alive == alive_before + 2);
        w.remove<Tracked>(only);
        debugAssertion(Tracked::alive == alive_before + 1);
    }
    // The world destructor destroys what was left, at its new location
    debugAssertion(Tracked::alive == alive_before);

    // Adding a component it already has overwrites the value without moving the entity
    {
        Swelter::World w;
        const Swelter::Entity e = w.create(Tracked{1}, Position{});
#ifdef HS_DEBUG_MODE
        const Swelter::ArchetypeId_t arch = w.debugArchetypeOf(e);
        const std::size_t arch_count = w.debugArchetypeCount();
#endif
        w.add(e, Tracked{2});
        debugAssertion(w.get<Tracked>(e).value == 2);
        debugAssertion(Tracked::alive == alive_before + 1);

        Tracked lvalue {3};
        w.add(e, lvalue);
        debugAssertion(w.get<Tracked>(e).value == 3 && lvalue.value == 3);
#ifdef HS_DEBUG_MODE
        debugAssertion(w.debugArchetypeOf(e) == arch);
        debugAssertion(w.debugArchetypeCount() == arch_count);
#endif

        // Removing a component it doesn't have changes nothing
        w.remove<Health>(e);
        debugAssertion(w.get<Tracked>(e).value == 3);
#ifdef HS_DEBUG_MODE
        debugAssertion(w.debugArchetypeOf(e) == arch);
#endif
    }
    debugAssertion(Tracked::alive == alive_before);

    // The last row of the source archetype fills the hole and the table follows it
    {
        Swelter::World w;
        std::vector<Swelter::Entity> es;
        for(int i{}; i < 3; ++i)
        {
            es.push_back(w.create(Position{static_cast<float>(i), 0.f}, Tracked{i}));
        }

        w.remove<Position>(es[0]);
        debugAssertion(w.get<Tracked>(es[0]).value == 0);
        debugAssertion(w.get<Tracked>(es[2]).value == 2 && w.get<Position>(es[2]).x == 2.f);
        debugAssertion(w.get<Tracked>(es[1]).value == 1 && w.get<Position>(es[1]).x == 1.f);

        // Now es[2] is at row 0 and es[1] is the last row: moving the last row moves nothing else
        w.remove<Position>(es[1]);
        debugAssertion(w.get<Tracked>(es[1]).value == 1);
        debugAssertion(w.get<Tracked>(es[2]).value == 2 && w.get<Position>(es[2]).x == 2.f);
        debugAssertion(Tracked::alive == alive_before + 3);

        // destroy still finds a moved entity
        w.destroy(es[0]);
        debugAssertion(Tracked::alive == alive_before + 2);
        debugAssertion(w.get<Tracked>(es[1]).value == 1);
    }
    debugAssertion(Tracked::alive == alive_before);

    // Tags have no column, but they change the archetype
    {
        Swelter::World w;
        const Swelter::Entity e = w.create(Position{4.f, 5.f});
#ifdef HS_DEBUG_MODE
        const Swelter::ArchetypeId_t no_tag = w.debugArchetypeOf(e);
#endif
        w.add(e, TPlayer{});
        debugAssertion(w.has<TPlayer>(e) && w.get<Position>(e).x == 4.f);
#ifdef HS_DEBUG_MODE
        const Swelter::ArchetypeId_t with_tag = w.debugArchetypeOf(e);
        debugAssertion(with_tag != no_tag);
#endif

        // Adding it again does nothing
        w.add(e, TPlayer{});
#ifdef HS_DEBUG_MODE
        debugAssertion(w.debugArchetypeOf(e) == with_tag);
#endif

        // The tag is kept while another component is removed and added
        w.remove<Position>(e);
        debugAssertion(w.has<TPlayer>(e) && !w.has<Position>(e));
        w.add(e, Position{6.f, 7.f});
        debugAssertion(w.has<TPlayer>(e) && w.get<Position>(e).y == 7.f);

        w.remove<TPlayer>(e);
        debugAssertion(!w.has<TPlayer>(e) && w.get<Position>(e).x == 6.f);
#ifdef HS_DEBUG_MODE
        debugAssertion(w.debugArchetypeOf(e) == no_tag);
#endif

        // Only a tag left: removing it goes back to the root
        const Swelter::Entity only_tag = w.create(TPlayer{});
        w.remove<TPlayer>(only_tag);
        debugAssertion(!w.has<TPlayer>(only_tag));
    }

#ifdef HS_DEBUG_MODE
    // Going and coming back follows the edges: no new archetypes
    {
        Swelter::World w;
        const Swelter::Entity e = w.create(Position{});
        const Swelter::ArchetypeId_t start = w.debugArchetypeOf(e);

        w.add(e, Health{1.f});
        const std::size_t arch_count = w.debugArchetypeCount();

        w.remove<Health>(e);
        debugAssertion(w.debugArchetypeOf(e) == start);
        w.add(e, Health{2.f});
        w.remove<Health>(e);
        debugAssertion(w.debugArchetypeOf(e) == start);
        debugAssertion(w.debugArchetypeCount() == arch_count);
    }
#endif

    // Many entities over several chunks, moved there and back
    {
        Swelter::World w;
        std::vector<Swelter::Entity> es;
        const std::size_t count = 3000;
        for(std::size_t i{}; i < count; ++i)
        {
            es.push_back(w.create(Position{static_cast<float>(i), 0.f}, Name{longName(i)}));
        }

        // Every other entity gets a Health: holes in the source, rows appended to the destination
        for(std::size_t i{}; i < count; i += 2)
        {
            w.add(es[i], Health{static_cast<float>(i)});
        }
        for(std::size_t i{}; i < count; ++i)
        {
            debugAssertion(w.get<Position>(es[i]).x == static_cast<float>(i));
            debugAssertion(w.get<Name>(es[i]).s == longName(i));
            debugAssertion(w.has<Health>(es[i]) == (i % 2 == 0));
        }

        for(std::size_t i{}; i < count; i += 2)
        {
            debugAssertion(w.get<Health>(es[i]).h == static_cast<float>(i));
            w.remove<Health>(es[i]);
        }
        for(std::size_t i{}; i < count; ++i)
        {
            debugAssertion(!w.has<Health>(es[i]));
            debugAssertion(w.get<Position>(es[i]).x == static_cast<float>(i));
            debugAssertion(w.get<Name>(es[i]).s == longName(i));
        }
    }
}

// Bundles: registry and archetype graph edges
void testBundleGraph()
{
    using Swelter::BundleRegistry;
    using Swelter::CmpId_t;
    using Swelter::CmpRegistry;

    [[maybe_unused]] const CmpId_t pos  = CmpRegistry::id<Position>();
    [[maybe_unused]] const CmpId_t hp   = CmpRegistry::id<Health>();
    [[maybe_unused]] const CmpId_t name = CmpRegistry::id<Name>();
    [[maybe_unused]] const CmpId_t tag  = CmpRegistry::id<TPlayer>();
    [[maybe_unused]] auto sorted = [](std::vector<CmpId_t> v){ std::ranges::sort(v); return v; };

    // Registry: the id is stable and the stored ids are sorted, whatever the order of the types
    [[maybe_unused]] const Swelter::BundleId_t b_ph = BundleRegistry::id<Position, Health>();
    debugAssertion((BundleRegistry::id<Position, Health>() == b_ph));
    debugAssertion(BundleRegistry::ids(b_ph) == sorted({pos, hp}));
    debugAssertion((BundleRegistry::ids(BundleRegistry::id<Health, Position>()) == sorted({pos, hp})));
    [[maybe_unused]] const Swelter::BundleId_t b_hn = BundleRegistry::id<Health, Name>();
    debugAssertion(b_hn != b_ph);
    [[maybe_unused]] const Swelter::BundleId_t b_pt = BundleRegistry::id<Position, TPlayer>();
    debugAssertion(BundleRegistry::ids(b_pt) == sorted({pos, tag}));
    [[maybe_unused]] const Swelter::BundleId_t b_empty = BundleRegistry::id<>();
    debugAssertion(BundleRegistry::ids(b_empty).empty());

#ifdef HS_DEBUG_MODE
    Swelter::World w;
    const Swelter::ArchetypeId_t root = 0;

    // root + {Position, Health}: one archetype, one bundle edge and nothing else
    const Swelter::ArchetypeId_t pos_hp = w.debugArchetypeWithBundle(root, b_ph);
    debugAssertion(w.debugArchetypeCount() == 2);
    debugAssertion(w.debugArchetype(pos_hp).ids == sorted({pos, hp}));
    debugAssertion(w.debugArchetype(root).bundle_add_edges.size() == 1);
    debugAssertion(w.debugArchetype(root).add_edges.empty());               // component edges are untouched
    debugAssertion(w.debugArchetype(pos_hp).bundle_remove_edges.empty());   // no inverse edge
    debugAssertion(w.debugArchetype(pos_hp).remove_edges.empty());

    // The second time follows the edge
    debugAssertion(w.debugArchetypeWithBundle(root, b_ph) == pos_hp);
    debugAssertion(w.debugArchetypeCount() == 2);
    debugAssertion(w.debugArchetype(root).bundle_add_edges.size() == 1);

    // Same archetype as adding the components one by one
    const Swelter::ArchetypeId_t only_pos = w.debugArchetypeWith(root, pos);
    debugAssertion(w.debugArchetypeWith(only_pos, hp) == pos_hp);
    debugAssertion(w.debugArchetypeCount() == 3);

    // Several components in a single jump: no intermediate archetypes ({Position, Name} is never created)
    const Swelter::ArchetypeId_t pos_hp_name = w.debugArchetypeWithBundle(only_pos, b_hn);
    debugAssertion(w.debugArchetypeCount() == 4);
    debugAssertion(w.debugArchetype(pos_hp_name).ids == sorted({pos, hp, name}));

    // Overlap: {Position} + {Position, Health} == {Position, Health}
    debugAssertion(w.debugArchetypeWithBundle(only_pos, b_ph) == pos_hp);
    debugAssertion(w.debugArchetypeCount() == 4);

    // It already has the whole bundle: same archetype
    debugAssertion(w.debugArchetypeWithBundle(pos_hp, b_ph) == pos_hp);
    debugAssertion(w.debugArchetypeCount() == 4);

    // Empty bundle: same archetype
    debugAssertion(w.debugArchetypeWithBundle(pos_hp, b_empty) == pos_hp);
    debugAssertion(w.debugArchetypeCount() == 4);

    // Removing a bundle
    const std::size_t pos_hp_name_remove_edges = w.debugArchetype(pos_hp_name).remove_edges.size();
    const Swelter::ArchetypeId_t only_name = w.debugArchetypeWithoutBundle(pos_hp_name, b_ph);
    debugAssertion(w.debugArchetype(only_name).ids == std::vector<CmpId_t>{name});
    debugAssertion(w.debugArchetypeCount() == 5);
    debugAssertion(w.debugArchetype(pos_hp_name).bundle_remove_edges.size() == 1);
    debugAssertion(w.debugArchetype(pos_hp_name).remove_edges.size() == pos_hp_name_remove_edges);
    debugAssertion(w.debugArchetypeWithoutBundle(pos_hp_name, b_ph) == only_name);
    debugAssertion(w.debugArchetype(pos_hp_name).bundle_remove_edges.size() == 1);

    // Only part of the bundle is present: the missing components are ignored
    debugAssertion(w.debugArchetypeWithoutBundle(only_pos, b_ph) == root);
    debugAssertion(w.debugArchetypeWithoutBundle(only_name, b_ph) == only_name);

    // None of them is present
    debugAssertion(w.debugArchetypeWithoutBundle(root, b_ph) == root);
    debugAssertion(w.debugArchetypeCount() == 5);

    // Why there is no inverse edge: {Position} + {Position, Health} lands in pos_hp,
    // but removing {Position, Health} from there goes to the root, not back to {Position}
    debugAssertion(w.debugArchetypeWithoutBundle(pos_hp, b_ph) == root);

    // A tag in a bundle is part of the identity
    const Swelter::ArchetypeId_t pos_tag = w.debugArchetypeWithBundle(root, b_pt);
    debugAssertion(pos_tag != only_pos);
    debugAssertion(w.debugArchetype(pos_tag).bitset.test(tag) && w.debugArchetype(pos_tag).bitset.test(pos));
    debugAssertion(w.debugArchetypeWithoutBundle(pos_tag, b_pt) == root);
#endif
}

// Bundles: references and cv-qualifiers don't create new bundles
void testBundleRegistryCvref()
{
    using Swelter::BundleRegistry;

    debugAssertion((BundleRegistry::id<const Position&, Health&&>() == BundleRegistry::id<Position, Health>()));
    debugAssertion(BundleRegistry::id<const Name>() == BundleRegistry::id<Name>());
}

// World: create, add and remove with several components (bundles)
void testWorldBundles()
{
#ifdef HS_DEBUG_MODE
    // create: the order of the types doesn't matter, and the second time only follows the edge
    {
        Swelter::World w;
        const Swelter::Entity a = w.create(Position{}, Health{});
        const std::size_t arch_count = w.debugArchetypeCount();
        const std::size_t root_edges = w.debugArchetype(0).bundle_add_edges.size();

        const Swelter::Entity b = w.create(Position{}, Health{});
        debugAssertion(w.debugArchetypeOf(b) == w.debugArchetypeOf(a));
        debugAssertion(w.debugArchetypeCount() == arch_count);
        debugAssertion(w.debugArchetype(0).bundle_add_edges.size() == root_edges);

        // <Health, Position> is another bundle (one more edge), but the same archetype
        const Swelter::Entity c = w.create(Health{}, Position{});
        debugAssertion(w.debugArchetypeOf(c) == w.debugArchetypeOf(a));
        debugAssertion(w.debugArchetypeCount() == arch_count);
    }
#endif

    // add with several components: one move, no intermediate archetypes
    {
        Swelter::World w;
        const Swelter::Entity e = w.create(Position{1.f, 2.f});
#ifdef HS_DEBUG_MODE
        const std::size_t arch_count = w.debugArchetypeCount();
#endif
        const Name name {longName(1)};
        w.add(e, Health{3.f}, name);                // rvalue and const lvalue
        debugAssertion(w.get<Position>(e).x == 1.f && w.get<Position>(e).y == 2.f);
        debugAssertion(w.get<Health>(e).h == 3.f);
        debugAssertion(w.get<Name>(e).s == longName(1) && name.s == longName(1));
#ifdef HS_DEBUG_MODE
        debugAssertion(w.debugArchetypeCount() == arch_count + 1);   // only {Position, Health, Name}
#endif
    }

    [[maybe_unused]] const int alive_before = Tracked::alive;
    {
        Swelter::World w;
        const Swelter::Entity e = w.create(Position{4.f, 5.f}, Tracked{1});
        debugAssertion(Tracked::alive == alive_before + 1);

        // Overlap: Tracked is already there (overwritten), Health is new (constructed)
        w.add(e, Tracked{5}, Health{2.f});
        debugAssertion(Tracked::alive == alive_before + 1);
        debugAssertion(w.get<Tracked>(e).value == 5 && w.get<Health>(e).h == 2.f);
        debugAssertion(w.get<Position>(e).x == 4.f);

        // It already has all of them: no move, values overwritten
#ifdef HS_DEBUG_MODE
        const Swelter::ArchetypeId_t arch = w.debugArchetypeOf(e);
#endif
        w.add(e, Health{7.f}, Tracked{6});
        debugAssertion(w.get<Tracked>(e).value == 6 && w.get<Health>(e).h == 7.f);
        debugAssertion(Tracked::alive == alive_before + 1);
#ifdef HS_DEBUG_MODE
        debugAssertion(w.debugArchetypeOf(e) == arch);
#endif

        // Tags in a bundle: a new one changes the archetype, one it already has doesn't
        w.add(e, TPlayer{}, Name{longName(2)});
        debugAssertion(w.has<TPlayer>(e) && w.get<Name>(e).s == longName(2));
#ifdef HS_DEBUG_MODE
        const Swelter::ArchetypeId_t with_tag = w.debugArchetypeOf(e);
#endif
        w.add(e, TPlayer{}, Health{9.f});
        debugAssertion(w.get<Health>(e).h == 9.f);
#ifdef HS_DEBUG_MODE
        debugAssertion(w.debugArchetypeOf(e) == with_tag);
#endif

        // Now e has {Position, Health, Name, Tracked, TPlayer}
        w.remove<Health, Name>(e);
        debugAssertion(!w.has<Health>(e) && !w.has<Name>(e));
        debugAssertion(w.get<Position>(e).y == 5.f && w.get<Tracked>(e).value == 6 && w.has<TPlayer>(e));
        debugAssertion(Tracked::alive == alive_before + 1);

        // Only part of the bundle is present: Health is ignored, Tracked is destroyed
        w.remove<Health, Tracked>(e);
        debugAssertion(!w.has<Tracked>(e));
        debugAssertion(Tracked::alive == alive_before);
        debugAssertion(w.get<Position>(e).x == 4.f);

        // None of them is present: nothing changes
#ifdef HS_DEBUG_MODE
        const Swelter::ArchetypeId_t before = w.debugArchetypeOf(e);
#endif
        w.remove<Health, Name>(e);
#ifdef HS_DEBUG_MODE
        debugAssertion(w.debugArchetypeOf(e) == before);
#endif

        // Removing everything goes back to the root
        w.remove<TPlayer, Position>(e);
        debugAssertion(!w.has<Position>(e) && !w.has<TPlayer>(e));
#ifdef HS_DEBUG_MODE
        debugAssertion(w.debugArchetypeOf(e) == 0);
#endif

        // And from the root, a whole bundle at once
        w.add(e, Tracked{3}, Position{8.f, 8.f});
        debugAssertion(w.get<Tracked>(e).value == 3 && w.get<Position>(e).x == 8.f);
        debugAssertion(Tracked::alive == alive_before + 1);
    }
    debugAssertion(Tracked::alive == alive_before);

    // The last row of the source fills the hole left by a bundle move
    {
        Swelter::World w;
        std::vector<Swelter::Entity> es;
        for(int i{}; i < 3; ++i)
        {
            es.push_back(w.create(Position{static_cast<float>(i), 0.f}, Tracked{i}));
        }

        w.add(es[0], Health{10.f}, Name{longName(10)});
        debugAssertion(w.get<Tracked>(es[0]).value == 0 && w.get<Name>(es[0]).s == longName(10));
        debugAssertion(w.get<Tracked>(es[2]).value == 2 && w.get<Position>(es[2]).x == 2.f);
        debugAssertion(w.get<Tracked>(es[1]).value == 1 && w.get<Position>(es[1]).x == 1.f);

        w.remove<Position, Tracked>(es[2]);
        debugAssertion(w.get<Tracked>(es[1]).value == 1);
        debugAssertion(Tracked::alive == alive_before + 2);
    }
    debugAssertion(Tracked::alive == alive_before);

    // Many entities over several chunks, there and back with a bundle
    {
        Swelter::World w;
        std::vector<Swelter::Entity> es;
        const std::size_t count = 3000;
        for(std::size_t i{}; i < count; ++i)
        {
            es.push_back(w.create(Name{longName(i)}, Position{static_cast<float>(i), 0.f}));
        }

        for(std::size_t i{}; i < count; i += 3)
        {
            w.add(es[i], Health{static_cast<float>(i)}, Tracked{static_cast<int>(i)});
        }
        debugAssertion(Tracked::alive == alive_before + static_cast<int>((count + 2) / 3));
        for(std::size_t i{}; i < count; ++i)
        {
            debugAssertion(w.get<Position>(es[i]).x == static_cast<float>(i));
            debugAssertion(w.get<Name>(es[i]).s == longName(i));
            debugAssertion(w.has<Tracked>(es[i]) == (i % 3 == 0));
            if(i % 3 == 0)
            {
                debugAssertion(w.get<Tracked>(es[i]).value == static_cast<int>(i));
                debugAssertion(w.get<Health>(es[i]).h == static_cast<float>(i));
            }
        }

        for(std::size_t i{}; i < count; i += 3)
        {
            w.remove<Tracked, Health>(es[i]);
        }
        debugAssertion(Tracked::alive == alive_before);
        for(std::size_t i{}; i < count; ++i)
        {
            debugAssertion(!w.has<Health>(es[i]));
            debugAssertion(w.get<Position>(es[i]).x == static_cast<float>(i));
            debugAssertion(w.get<Name>(es[i]).s == longName(i));
        }
    }
}

int main() {

    logInit();

    testChunkPool();
    testArchetype();
    testEntityTable();
    testTableArchetypeIntegration();
    testWorldGraph();
    testWorldCreate();
    testWorldDestroy();
    testWorldAddRemove();
    testBundleGraph();
    testBundleRegistryCvref();
    testWorldBundles();

    return 0;
}
