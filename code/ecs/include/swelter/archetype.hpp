#pragma once

/**
 * @file archetype.hpp
 * @brief Archetype: the storage of all the entities that share the same set of components.
 */

#include <swelter/config.hpp>
#include <swelter/registry.hpp>
#include <swelter/entity.hpp>
#include <swelter/chunk.hpp>
#include <bitset>
#include <span>
#include <cstring>
#include <optional>
#include <memory>
#include <algorithm>
#include <iterator>
#include <vector>

namespace Swelter
{
    using ArchetypeId_t = std::uint32_t;
    inline constexpr ArchetypeId_t INVALID_ARCHETYPE = UINT32_MAX;

    namespace detail
    {
        /**
         * @brief Rounds @p val up to the next multiple of @p align.
         * @param val Value to round.
         * @param align Alignment. Must be a power of two.
         * @return The smallest multiple of @p align that is not less than @p val.
         */
        template <typename T>
        constexpr T align_up(T val, T align)
        {
            return (val + (align - 1)) & ~(align - 1);
        }
    }

    /**
     * @brief Layout of the chunks of an archetype: how many rows fit and where each column starts.
     *
     * The entity column is always at offset 0, followed by one column per component in ID order.
     */
    struct ArchetypeLayout
    {
        std::uint32_t capacity {};           ///< Rows per chunk (N).
        std::vector<std::uint32_t> offsets;  ///< Byte offset of each component column, in the same order as the IDs.
    };

    /**
     * @brief Computes the capacity and the column offsets of a chunk.
     *
     * Starts from `N = CHUNK_SIZE / (sizeof(Entity) + sum of sizes)` and lowers N
     * until all the columns, each aligned to COLUMN_ALIGN, fit in the chunk.
     * Tags (size 0) get an offset but take no space.
     * @param cmps_info0 Information of the components, sorted by ID.
     * @return The layout of the archetype.
     * @pre The resulting capacity is at least MIN_CHUNK_CAPACITY.
     */
    inline ArchetypeLayout computeLayout(const std::vector<ComponentInfo>& cmps_info0)
    {

        ArchetypeLayout compute_result;
        compute_result.offsets.resize(cmps_info0.size());

        std::size_t total_size = sizeof(Entity);
        for(const auto& cmp_info0 : cmps_info0)
        {
            total_size += cmp_info0.size;
        }

        std::size_t N = (CHUNK_SIZE / total_size);

        std::size_t cursor {};
        do
        {
            cursor = sizeof(Entity) * N;

            for(std::size_t i {0}; i < compute_result.offsets.size(); ++i)
            {
                compute_result.offsets[i] =  detail::align_up(cursor, COLUMN_ALIGN );
                cursor = compute_result.offsets[i] + (cmps_info0[i].size * N);
            }
            compute_result.capacity = N;
            --N;
        }while(cursor > CHUNK_SIZE);

        debugAssertion(compute_result.capacity >= MIN_CHUNK_CAPACITY);


        return compute_result;
    }


    /**
     * @brief Builds the component bitset of an archetype.
     * @param ids Component IDs of the archetype.
     * @return A bitset with the bit of each ID set.
     */
    inline std::bitset<MAX_COMPONENTS> buildBitSet(const std::vector<CmpRegistry::CmpId_t>& ids)
    {
        std::bitset<MAX_COMPONENTS> bitset;

        for(const auto id : ids)
        {
            bitset.set(id, true);
        }

        return bitset;
    }

    /**
     * @brief Gathers the ComponentInfo of each ID from the registry.
     * @param ids Component IDs, already registered.
     * @return Their information, in the same order as @p ids.
     */
    inline std::vector<ComponentInfo> buildComponentInfo(const std::vector<CmpRegistry::CmpId_t>& ids)
    {
        std::vector<ComponentInfo> info;
        info.reserve(ids.size());

        for(const auto id : ids)
        {
            info.push_back(CmpRegistry::info(id));
        }

        return info;
    }

    /**
     * @brief Position of a row inside an archetype.
     *
     * Uses the chunk index instead of a Chunk*, because chunks can change when rows are removed.
     */
    struct RowLocation
    {
        std::uint32_t chunk_idx   {0};  ///< Index of the chunk in the archetype.
        std::uint32_t row         {0};  ///< Row inside that chunk.
    };

    /**
     * @brief Storage of all the entities that have exactly the same set of components.
     *
     * Rows are stored in fixed-size chunks taken from the ChunkPool, with SoA layout:
     * an entity column first, then one column per component in ID order, each aligned
     * to COLUMN_ALIGN. Tags are part of the archetype but have no column.
     *
     * All chunks are full except the last one: a removed row is filled with the last
     * row of the last chunk (deletion policy b), so there is no internal fragmentation.
     *
     * The identity of an archetype is its sorted list of IDs; the bitset is only used
     * to match queries quickly. An archetype can't be copied or moved.
     * @note Archetypes are never destroyed while the world is alive.
     */
    struct Archetype
    {
        /**
         * @brief Metadata of a chunk, kept outside the chunk itself.
         */
        struct ChunkDescriptor
        {
            Chunk* chunk        {nullptr};  ///< Chunk memory, owned by the ChunkPool.
            std::uint32_t count {0};        ///< Number of rows in use.
        };

        /**
         * @brief Creates an empty archetype (without chunks) for a set of components.
         * @param ids0 Component IDs, already registered.
         * @pre @p ids0 is sorted and has no repeated IDs.
         */
        explicit Archetype(const std::vector<CmpRegistry::CmpId_t>& ids0) :
            ids(ids0),
            cmps_info(buildComponentInfo(ids0)),
            layout(computeLayout(cmps_info)),
            bitset(buildBitSet(ids0)),
            chunks()
        {
            // The archetype identity is the sorted list of ids: getColumn<T> relies on it (lower_bound)
            debugAssertion(std::ranges::is_sorted(ids), "Archetype ids must be sorted");
            debugAssertion(std::ranges::adjacent_find(ids) == ids.end(), "Archetype ids must not be repeated");
        }

        /**
         * @brief Destroys the components of every row and returns all the chunks to the pool.
         *
         * Trivially copyable components and tags are skipped.
         */
        ~Archetype()
        {
            for(std::size_t c{}; c < chunks.size(); ++c)
            {
                // Components outside, rows inside: trivial cmps are skipped once per chunk and each column is walked in order
                for(std::size_t k{}; k < cmps_info.size(); ++k)
                {
                    const ComponentInfo& cmp_info = cmps_info[k];
                    if(cmp_info.size == 0 || cmp_info.trivially_copyable) continue;

                    std::byte* column = reinterpret_cast<std::byte*>(getColumn(c, k));
                    for(std::size_t r{}; r < chunks[c].count; ++r)
                    {
                        cmp_info.destroy(column + r * cmp_info.size);
                    }
                }
                ChunkPool::deallocate(chunks[c].chunk);
            }
            chunks.clear();
        }

        /**
         * @brief Read-only view of the chunk descriptors.
         */
        std::span<const ChunkDescriptor> getChunks() const noexcept
        {
            return chunks;
        }

        /**
         * @brief Adds a row at the end of the archetype and writes its entity.
         *
         * Takes a new chunk from the pool if there are none or the last one is full.
         * @param e Entity that owns the row.
         * @return Location of the new row.
         * @throws std::bad_alloc If a new chunk is needed and can't be allocated.
         * @warning Only the entity is written: the caller must construct every
         *          component of the row (e.g. with std::construct_at) before using it.
         */
        RowLocation addRow(const Entity& e)
        {
            if(chunks.empty() || chunks.back().count == layout.capacity)
            {
                //Add new chunk
                Chunk* newchunk = ChunkPool::allocate();
                ChunkDescriptor& newrow = chunks.emplace_back();
                newrow.chunk = newchunk;
            }
            Chunk* current = chunks.back().chunk;
            std::uint32_t current_count = chunks.back().count++;

            // Write Entity in his column
            std::construct_at(reinterpret_cast<Entity*>(current->data) + current_count, e);

            return { static_cast<std::uint32_t>(chunks.size() - 1), current_count};
        }

        /**
         * @brief Type-erased pointer to the start of a component column.
         * @param chunk_i Index of the chunk.
         * @param cmp_i Index of the component in #ids (not its ID).
         * @return Pointer to the first element of the column.
         * @pre `chunk_i < getChunks().size()` and `cmp_i < ids.size()`.
         */
        void* getColumn(const std::size_t chunk_i, const std::size_t cmp_i) noexcept
        {
            debugAssertion(chunk_i < chunks.size() && cmp_i < layout.offsets.size());

            return chunks[chunk_i].chunk->data + layout.offsets[cmp_i];
        }

        /**
         * @brief Typed pointer to the start of the column of component @p T.
         *
         * Finds the column with a binary search over #ids.
         * @tparam T Component type.
         * @param chunk_i Index of the chunk.
         * @return Pointer to the first @p T of the column. Only the first `getChunks()[chunk_i].count` elements are valid.
         * @pre The archetype has component @p T and `chunk_i < getChunks().size()`.
         */
        template<typename T>
        T* getColumn(const std::size_t chunk_i) noexcept
        {
            auto it = std::ranges::lower_bound(ids, CmpRegistry::id<T>());
            debugAssertion(it != ids.end());
            debugAssertion(*it == CmpRegistry::id<T>());
            std::size_t cmp_i = std::distance(ids.begin(),it);

            return static_cast<T*>(getColumn(chunk_i, cmp_i));
        }

        /**
         * @brief Read-only view of the entities stored in a chunk.
         * @param chunk_i Index of the chunk.
         * @return A span with one entity per used row.
         * @pre `chunk_i < getChunks().size()`.
         */
        std::span<const Entity> getEntities(const std::size_t chunk_i) const noexcept
        {
            debugAssertion(chunk_i < chunks.size());
            return {reinterpret_cast<const Entity*>(chunks[chunk_i].chunk->data), chunks[chunk_i].count};
        }

        /**
         * @brief View of the entities stored in a chunk.
         * @copydetails getEntities(const std::size_t) const
         */
        std::span<Entity> getEntities(std::size_t chunk_i) noexcept
        {
            debugAssertion(chunk_i < chunks.size());
            return {reinterpret_cast<Entity*>(chunks[chunk_i].chunk->data), chunks[chunk_i].count};
        }

        /**
         * @brief Removes a row, destroying its components.
         *
         * Deletion policy b: if the row is not the last one, the last row of the last
         * chunk is moved into the hole. If the last chunk becomes empty, it goes back to the pool.
         * @param chunk_i Index of the chunk.
         * @param row Row inside that chunk.
         * @return The entity that was moved into the hole, if any. Its location must be
         *         updated to `{chunk_i, row}` by the caller.
         * @pre `chunk_i < getChunks().size()` and `row < getChunks()[chunk_i].count`.
         */
        std::optional<Entity> removeRow(const std::size_t chunk_i, const std::size_t row)
        {
            debugAssertion(chunk_i < chunks.size() && row < chunks[chunk_i].count);

            std::optional<Entity> moved_e;

            // Delete non trivial cmps
            for(std::size_t i{}; i < cmps_info.size(); ++i)
            {
                const ComponentInfo& cmp_info = cmps_info[i];
                if (cmp_info.size == 0) continue;

                if(!cmp_info.trivially_copyable)
                {
                    void* cmp_start = reinterpret_cast<std::byte*>(getColumn(chunk_i, i)) + row * cmp_info.size;
                    cmp_info.destroy(cmp_start);
                }
            }
            ChunkDescriptor& last_descriptor = chunks.back();
            std::uint32_t last_count = last_descriptor.count - 1;

            const std::size_t last_chunk = chunks.size() - 1;

            const bool is_last = chunk_i == last_chunk && row == last_count;
            if(!is_last)
            {
                //Swap
                Entity& hole = getEntities(chunk_i)[row];
                hole = getEntities(last_chunk)[last_count];
                moved_e = hole;
                // Copy cmps
                for(std::size_t i{}; i < ids.size(); ++i)
                {
                    const ComponentInfo& cmp_info = cmps_info[i];
                    if (cmp_info.size == 0) continue;

                    void* to = reinterpret_cast<std::byte*>(getColumn(chunk_i, i)) + row * cmp_info.size;
                    void* from = reinterpret_cast<std::byte*>(getColumn(last_chunk, i)) + last_count * cmp_info.size;
                    const std::uint32_t cmp_size = cmp_info.size;

                    if(!cmp_info.trivially_copyable)
                    {
                        cmp_info.move_construct(to, from);
                        // Still need to destroy nontrivials
                        cmp_info.destroy(from);
                    }
                    else
                    {
                        std::memcpy(to,from,cmp_size);
                    }
                }
            }
            if(--last_descriptor.count == 0)
            {
                ChunkPool::deallocate(last_descriptor.chunk);
                chunks.pop_back();
            }
            return moved_e;
        }
        // Deleted functions
        Archetype(const Archetype&) = delete;
        Archetype(Archetype&&) = delete;
        Archetype& operator=(const Archetype&) = delete;
        Archetype& operator=(Archetype&&) = delete;
        // Archetype data
        const std::vector<CmpRegistry::CmpId_t> ids;    ///< Sorted component IDs: the identity of the archetype. The position of an ID is its column index.
        const std::vector<ComponentInfo> cmps_info;     ///< Information of each component, in the same order as #ids.
        const ArchetypeLayout layout;                   ///< Capacity and column offsets of every chunk.
        const std::bitset<MAX_COMPONENTS> bitset;       ///< Same components as #ids, used to match queries with `(arch & query) == query`.
        private:
            std::vector<ChunkDescriptor> chunks;        ///< Chunks in use. All are full except the last one.
    };
}
