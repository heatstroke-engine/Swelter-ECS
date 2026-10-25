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

int main() {

    logInit();

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

    Swelter::CmpRegistry::registerComponents<Position,Health>();
    std::vector<Swelter::ComponentInfo> g; 
    g.push_back(Swelter::CmpRegistry::info<Position>()); 
    g.push_back(Swelter::CmpRegistry::info<Health>());
    Swelter::ArchetypeLayout layout =  Swelter::computeLayout(g);
    debugAssertion(layout.capacity == 816);
    debugAssertion(layout.offsets[0] == 6528);
    debugAssertion(layout.offsets[1] == 13056);

    // Archetype: chunks are only requested when the first row arrives
    std::vector<Swelter::CmpRegistry::CmpId_t> ids = {Swelter::CmpRegistry::id<Position>(),Swelter::CmpRegistry::id<Health>()};
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
        [[maybe_unused]] Swelter::EntInfo info = arch.addRow(Swelter::Entity{static_cast<std::uint64_t>(i)});
        debugAssertion(info.chunk_idx == 0 && info.row == i);
    }
    debugAssertion(arch.getChunks().size() == 1);
    debugAssertion(arch.getChunks()[0].count == capacity);

    // One more row opens a second chunk at row 0
    [[maybe_unused]] Swelter::EntInfo extra = arch.addRow(Swelter::Entity{capacity});
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
            debugAssertion(entities[r].dummy == c * capacity + r);
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
    std::vector<Swelter::CmpRegistry::CmpId_t> rm_ids = {Swelter::CmpRegistry::id<Position>(),
                                                         Swelter::CmpRegistry::id<Health>(),
                                                         Swelter::CmpRegistry::id<Name>()};
    std::ranges::sort(rm_ids);
    Swelter::Archetype rm_arch {rm_ids};
    const std::uint32_t rm_cap = rm_arch.layout.capacity;

    // Full chunk 0 plus 2 rows in chunk 1. Row data depends on the entity, so a moved row can be recognised
    const std::size_t rm_rows = rm_cap + 2;
    for(std::size_t i{}; i < rm_rows; ++i)
    {
        auto [c, r] = rm_arch.addRow(Swelter::Entity{static_cast<std::uint64_t>(i)});
        std::construct_at(rm_arch.getColumn<Position>(c) + r, Position{static_cast<float>(i), static_cast<float>(i)});
        std::construct_at(rm_arch.getColumn<Health>(c) + r, Health{static_cast<float>(i) * 2});
        std::construct_at(rm_arch.getColumn<Name>(c) + r, Name{longName(i)});
    }
    debugAssertion(rm_arch.getChunks().size() == 2);
    debugAssertion(rm_arch.getChunks()[1].count == 2);
    [[maybe_unused]] const std::size_t free_before = Swelter::ChunkPool::freeCount();

    // 1. Remove a middle row: the last row of the last chunk (entity rm_rows - 1) fills the hole
    [[maybe_unused]] std::optional<Swelter::Entity> moved = rm_arch.removeRow(0, 5);
    [[maybe_unused]] const std::size_t last_e = rm_rows - 1;
    debugAssertion(moved.has_value() && moved->dummy == last_e);
    debugAssertion(rm_arch.getEntities(0)[5].dummy == last_e);
    debugAssertion(rm_arch.getColumn<Position>(0)[5].x == static_cast<float>(last_e));
    debugAssertion(rm_arch.getColumn<Health>(0)[5].h == static_cast<float>(last_e) * 2);
    debugAssertion(rm_arch.getColumn<Name>(0)[5].s == longName(last_e));
    debugAssertion(rm_arch.getChunks()[1].count == 1);
    // Neighbour rows are untouched
    debugAssertion(rm_arch.getEntities(0)[4].dummy == 4 && rm_arch.getEntities(0)[6].dummy == 6);
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
    [[maybe_unused]] const std::uint64_t same_chunk_last = rm_arch.getEntities(0)[rm_cap - 2].dummy;
    moved = rm_arch.removeRow(0, 10);
    debugAssertion(moved.has_value() && moved->dummy == same_chunk_last);
    debugAssertion(rm_arch.getEntities(0)[10].dummy == same_chunk_last);
    debugAssertion(rm_arch.getColumn<Name>(0)[10].s == longName(same_chunk_last));
    debugAssertion(rm_arch.getChunks()[0].count == rm_cap - 2);

    // Destructor: every non trivial component is destroyed exactly once and every chunk goes back to the pool
    [[maybe_unused]] std::size_t free_before_dtor = 0;
    [[maybe_unused]] std::size_t dtor_chunks = 0;
    {
        std::vector<Swelter::CmpRegistry::CmpId_t> tr_ids = {Swelter::CmpRegistry::id<Position>(),
                                                             Swelter::CmpRegistry::id<Tracked>()};
        std::ranges::sort(tr_ids);
        Swelter::Archetype tr_arch {tr_ids};
        const std::uint32_t tr_cap = tr_arch.layout.capacity;

        // 2 full chunks and 3 rows in a third one
        const std::size_t tr_rows = 2 * tr_cap + 3;
        for(std::size_t i{}; i < tr_rows; ++i)
        {
            auto [c, r] = tr_arch.addRow(Swelter::Entity{static_cast<std::uint64_t>(i)});
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

    return 0;
}
