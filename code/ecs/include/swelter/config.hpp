#pragma once

/**
 * @file config.hpp
 * @brief Compile-time configuration constants of the Swelter ECS.
 */

#include <warning.hpp>
#include <cstddef>
#include <bit>

namespace Swelter
{
    constexpr std::size_t bytes_in_kb = 1024;   ///< Number of bytes in a KB.
    constexpr std::size_t number_of_kb = 16;    ///< Size of a chunk, in KB.

    /**
     * @brief Size in bytes of every chunk (16 KB by default).
     *
     * All the chunks have the same size, so the pool never fragments.
     */
    constexpr std::size_t CHUNK_SIZE = number_of_kb * bytes_in_kb;

    /**
     * @brief Minimum number of rows that a chunk must hold.
     *
     * Keeps the padding waste of a chunk at 1/16 or less.
     * @see computeLayout
     */
    constexpr std::size_t MIN_CHUNK_CAPACITY = 16;

    /**
     * @brief Alignment in bytes of every column inside a chunk (one cache line).
     *
     * Columns never share a cache line (no false sharing between threads) and SIMD accesses are aligned.
     * Components with a stricter alignment are rejected by the Component concept.
     */
    constexpr std::size_t COLUMN_ALIGN = 64;

    /**
     * @brief Maximum number of component types that can be registered.
     *
     * It is also the size of the archetype bitset.
     * @see CmpRegistry
     */
    constexpr std::size_t MAX_COMPONENTS = 256;

    /**
     * @brief Number of records in each page of the EntityTable.
     *
     * Must be a power of two, so finding a record needs no division (see PAGE_SHIFT and PAGE_MASK).
     * With 16 B records, a page takes 16 KB.
     * @see EntityTable
     */
    constexpr std::size_t ENTITY_PAGE_SIZE = 1024;
    compileTimeAssertion(std::has_single_bit(ENTITY_PAGE_SIZE));

    /**
     * @brief Page of an entity index: `index >> PAGE_SHIFT` (log2 of ENTITY_PAGE_SIZE).
     */
    constexpr std::size_t PAGE_SHIFT = std::countr_zero(ENTITY_PAGE_SIZE);

    /**
     * @brief Position of an entity index inside its page: `index & PAGE_MASK`.
     */
    constexpr std::size_t PAGE_MASK = ENTITY_PAGE_SIZE - 1;

} // namespace Swelter
