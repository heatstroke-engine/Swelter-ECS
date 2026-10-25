#pragma once

/**
 * @file chunk.hpp
 * @brief Fixed-size memory chunks and the global pool that recycles them.
 */

#include <swelter/config.hpp>
#include <warning.hpp>
#include <memory>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstddef>


namespace Swelter
{
    /**
     * @brief Raw block of CHUNK_SIZE bytes aligned to COLUMN_ALIGN.
     *
     * It holds the rows of an archetype in SoA layout. It has no header: the row
     * count and other metadata live in the archetype (Archetype::ChunkDescriptor).
     * It can't be copied or moved, so pointers to its data stay valid.
     * @note The data is left uninitialized on purpose: the archetype constructs the objects in place.
     */
    struct alignas(COLUMN_ALIGN) Chunk
    {
        compileTimeAssertion(CHUNK_SIZE % COLUMN_ALIGN == 0);

        Chunk() = default;

        // Deleted functions
        Chunk(const Chunk&) = delete;
        Chunk(Chunk&&) = delete;
        Chunk& operator=(const Chunk&) = delete;
        Chunk& operator=(Chunk&&) = delete;
        // Chunk data
        std::byte data[CHUNK_SIZE]; ///< Raw storage of the chunk.
    };
    compileTimeAssertion(sizeof(Chunk) == CHUNK_SIZE);


    /**
     * @brief Global pool of chunks shared by all the archetypes.
     *
     * Chunks are never returned to the system while the program runs: a freed
     * chunk goes back to the pool and any archetype can reuse it. Since all
     * chunks have the same size, there is no external fragmentation.
     * The pool owns every chunk it creates and frees them at program exit.
     *
     * In debug (HS_DEBUG_MODE) allocated chunks are filled with 0xCD and freed
     * chunks with 0xDD, to spot reads of uninitialized or freed memory.
     * @warning Not thread-safe.
     */
    struct ChunkPool
    {
        /**
         * @brief Gets a chunk: the last freed one (LIFO), or a new one if the pool has none.
         *
         * LIFO reuse returns the chunk most likely to still be in cache.
         * @return A non-null chunk with uninitialized data.
         * @throws std::bad_alloc If a new chunk can't be allocated.
         */
        static Chunk* allocate()
        {
            Chunk* ret = nullptr;

            if(frees.empty())
            {
                // Reserve first: if this throws, owned is untouched and nothing leaks.
                // Afterwards, deallocate can always push into frees without allocating.
                frees.reserve(owned.size() + 1);
                owned.emplace_back(std::make_unique_for_overwrite<Chunk>());
                ret = owned.back().get();
            }
            else
            {
                ret = frees.back();
                frees.pop_back();
            }
            #ifdef HS_DEBUG_MODE
                std::memset(ret->data,0xCD, CHUNK_SIZE);
            #endif

            return ret;
        }

        /**
         * @brief Returns a chunk to the pool so it can be reused.
         *
         * The objects in the chunk must have been destroyed before: the pool doesn't call any destructor.
         * @param chunk Chunk to return.
         * @pre @p chunk is not null, was obtained with allocate() and is not already free.
         */
        static void deallocate(Chunk* chunk) noexcept
        {
            debugAssertion(chunk != nullptr);
            debugAssertion(std::ranges::find(frees, chunk) == frees.end());
            debugAssertion(std::ranges::find(owned, chunk, &std::unique_ptr<Swelter::Chunk>::get) != owned.end());

            #ifdef HS_DEBUG_MODE
                std::memset(chunk->data,0xDD, CHUNK_SIZE);
            #endif

            frees.emplace_back(chunk);
        }

        /**
         * @brief Number of chunks created by the pool (free and in use).
         */
        static std::size_t ownedCount() noexcept
        {
            return owned.size();
        }

        /**
         * @brief Number of chunks currently free in the pool.
         */
        static std::size_t freeCount() noexcept
        {
            return frees.size();
        }

        private:
            // TODO: Use mutex
            inline static constinit std::vector<Chunk*> frees;                  ///< Free chunks, used as a LIFO stack.
            inline static constinit std::vector<std::unique_ptr<Chunk>> owned;  ///< Every chunk created; owns their memory.
    };
}
