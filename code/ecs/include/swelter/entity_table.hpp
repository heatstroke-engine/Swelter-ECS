#pragma once

/**
 * @file entity_table.hpp
 * @brief Paged table that hands out entity IDs and stores where each entity lives.
 */

#include <swelter/archetype.hpp>
#include <swelter/config.hpp>
#include <warning.hpp>
#include <vector>
#include <memory>
#include <stdexcept>
#include <cstdint>

namespace Swelter
{
    /**
     * @brief Record of one index of the EntityTable.
     *
     * A record is in one of three states:
     * - **Free:** in the free list. `location.row` holds the index of the next free record.
     * - **Reserved:** its ID was handed out, but the entity has no row yet (`archetype == INVALID_ARCHETYPE`).
     * - **Placed:** the entity lives in `archetype`, at `location`.
     */
    struct EntityRecord
    {
        std::uint32_t   generation;     ///< Current generation of the index.
        ArchetypeId_t   archetype;      ///< Archetype of the entity, or INVALID_ARCHETYPE if it is not placed.
        RowLocation     location;       ///< Row of the entity in its archetype. In a free record, `row` is the next free index.
    };
    compileTimeAssertion(sizeof(EntityRecord) == 16);

    /**
     * @brief Where an entity lives: its archetype and its row inside it.
     */
    struct EntityLocation
    {
        ArchetypeId_t   archetype;      ///< Archetype of the entity, or INVALID_ARCHETYPE if it is not placed.
        RowLocation     location;       ///< Chunk and row inside the archetype.
    };


    /**
     * @brief Hands out entity IDs and maps each live entity to its archetype and row.
     *
     * Records are stored in pages of ENTITY_PAGE_SIZE records, allocated when needed.
     * Pages never move, and finding a record needs no division: page = `index >> PAGE_SHIFT`,
     * record = `index & PAGE_MASK`.
     *
     * Freed indices are reused in LIFO order (the most recently freed record is likely still
     * in cache). The free list is implicit: it lives inside the free records themselves, so it
     * takes no extra memory.
     *
     * Reserving an ID (reserve) and placing the entity in an archetype (place) are separate steps,
     * so an ID can be handed out before the entity has a row (e.g. for deferred creation).
     *
     * The table only manages IDs and locations: adding and removing rows is up to the caller.
     * Each world owns its own table.
     * @warning Not thread-safe.
     */
    struct EntityTable
    {
        /**
         * @brief Hands out a new entity ID, reserved but not placed.
         *
         * Reuses the last freed index if there is one; otherwise takes a new index.
         * @return A live entity with no archetype (isPlaced() is false).
         * @throws std::length_error If every index has been handed out.
         * @throws std::bad_alloc If a new page is needed and can't be allocated.
         */
        Entity reserve()
        {
            Entity reserved;

            if(free_head != UINT32_MAX)
            {
                std::uint32_t new_idx = free_head;
                EntityRecord& rec = record(new_idx);
                free_head = rec.location.row;
                rec.archetype = INVALID_ARCHETYPE;

                reserved.index = new_idx;
                reserved.generation = rec.generation;

            }
            else
            {
                if(next_index == UINT32_MAX)
                {
                    throw std::length_error{"Entity index space exhausted"};
                }
                std::uint32_t new_idx = next_index;
                ensurePage(new_idx);
                ++next_index;

                EntityRecord& rec = record(new_idx);
                rec.archetype = INVALID_ARCHETYPE;
                rec.generation = 0;

                reserved.index = new_idx;
                reserved.generation = rec.generation;
            }
            return reserved;
        }

        /**
         * @brief Destroys an entity ID: every copy of @p e stops being alive.
         *
         * Increments the generation of the index and puts it in the free list.
         * If the generation reaches its maximum, the index is retired and never handed out again.
         * @param e Entity to release.
         * @pre `isAlive(e)`. Its row must already have been removed from its archetype.
         */
        void release(const Entity e) noexcept
        {
            debugAssertion(isAlive(e));

            EntityRecord& rec = record(e.index);
            ++rec.generation;
            rec.archetype = INVALID_ARCHETYPE;
            if(rec.generation == UINT32_MAX)
            {
                return;
            }
            rec.location.row = free_head;
            free_head = e.index;
        }

        /**
         * @brief Tells whether @p e refers to a live entity.
         *
         * True if its index has been handed out and its generation matches the one in the table.
         * False for NULL_ENTITY and for copies of destroyed entities, even if their index was reused.
         * @param e Entity to check.
         */
        bool isAlive(const Entity e) const noexcept
        {
            return (e.index < next_index && record(e.index).generation == e.generation);
        }

        /**
         * @brief Sets the archetype and row of an entity.
         *
         * Used to place a reserved entity, to move it to another archetype and to update the
         * entity that Archetype::removeRow moves into the removed row.
         * @param e Entity to place.
         * @param arch Archetype that holds its row.
         * @param loc Chunk and row inside @p arch.
         * @pre `isAlive(e)` and `arch != INVALID_ARCHETYPE`.
         */
        void place(const Entity e, const ArchetypeId_t arch, const RowLocation loc) noexcept
        {
            debugAssertion(isAlive(e) && arch != INVALID_ARCHETYPE);

            EntityRecord& rec = record(e.index);
            rec.archetype = arch;
            rec.location = loc;
        }

        /**
         * @brief Where an entity lives.
         * @param e Entity to look up.
         * @return Its archetype and row. The archetype is INVALID_ARCHETYPE if it is not placed.
         * @pre `isAlive(e)`. Not checked in release.
         */
        EntityLocation location(const Entity e) const noexcept
        {
            debugAssertion(isAlive(e));

            const EntityRecord& rec = record(e.index);
            return {rec.archetype, rec.location};
        }

        /**
         * @brief Tells whether a live entity already has a row in an archetype.
         * @param e Entity to check.
         * @return False if it was reserved but not placed yet.
         * @pre `isAlive(e)`.
         */
        bool isPlaced(const Entity e) const noexcept
        {
            debugAssertion(isAlive(e));

            return record(e.index).archetype != INVALID_ARCHETYPE;
        }

        #ifdef HS_DEBUG_MODE
            /**
             * @brief Test only: overwrites the generation of an index.
             *
             * Lets tests reach generations that would need billions of reserve/release cycles.
             * Only available in debug builds.
             * @param index Index already handed out.
             * @param generation New generation.
             */
            void debugSetGeneration(const std::uint32_t index, const std::uint32_t generation) noexcept
            {
                record(index).generation = generation;
            }
        #endif

        private:
            /**
             * @brief Record of an index: page `index >> PAGE_SHIFT`, position `index & PAGE_MASK`.
             * @pre `index < next_index`.
             */
            EntityRecord& record(const std::uint32_t index) noexcept
            {
                debugAssertion(index < next_index);

                return pages[index >> PAGE_SHIFT][index & PAGE_MASK];
            }

            /**
             * @copydoc record(const std::uint32_t)
             */
            const EntityRecord& record(const std::uint32_t index) const noexcept
            {
                debugAssertion(index < next_index);

                return pages[index >> PAGE_SHIFT][index & PAGE_MASK];
            }

            /**
             * @brief Allocates every missing page up to the one that holds @p index.
             * @throws std::bad_alloc If a page can't be allocated.
             */
            void ensurePage(const std::uint32_t index)
            {
                while((index >> PAGE_SHIFT) >= pages.size())
                {
                    pages.emplace_back(std::make_unique_for_overwrite<EntityRecord[]>(ENTITY_PAGE_SIZE));
                }
            }

            std::vector<std::unique_ptr<EntityRecord[]>> pages;     ///< Pages of records. The vector may reallocate, the pages never move.
            std::uint32_t next_index{};                             ///< Number of indices handed out so far: the next new index.
            std::uint32_t free_head{UINT32_MAX};                    ///< First index of the free list, or UINT32_MAX if it is empty.
    };
}
