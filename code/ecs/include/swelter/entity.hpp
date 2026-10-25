#pragma once

/**
 * @file entity.hpp
 * @brief Entity handle: index + generation.
 */

#include <cstdint>
#include <type_traits>
#include <warning.hpp>


namespace Swelter
{
    /**
     * @brief Handle of an entity: an index in the EntityTable plus a generation.
     *
     * The index is reused when the entity is destroyed. The generation tells a live
     * handle from an old copy with the same index: every destruction increments the
     * generation stored in the table, so old copies no longer match it.
     *
     * A default constructed Entity is NULL_ENTITY.
     * @see EntityTable::isAlive
     */
    struct Entity
    {
        std::uint32_t index         {UINT32_MAX};   ///< Index of the record in the EntityTable.
        std::uint32_t generation    {UINT32_MAX};   ///< Generation of the index when this handle was created.

        bool operator==(const Entity&) const = default;
    };

    /**
     * @brief Handle that never refers to an entity.
     *
     * Its index is never handed out, so EntityTable::isAlive is always false for it.
     */
    inline constexpr Entity NULL_ENTITY = {UINT32_MAX, UINT32_MAX};

    compileTimeAssertion(sizeof(Entity) == 8);
    compileTimeAssertion(std::is_trivially_copyable_v<Entity>);
}
