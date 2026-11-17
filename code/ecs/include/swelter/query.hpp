#pragma once

/**
 * @file query.hpp
 * @brief Cached queries: the archetypes that match a set of terms.
 */

#include<swelter/registry.hpp>
#include<typetraits.hpp>
#include<warning.hpp>

namespace Swelter
{
    /**
     * @brief Query over the archetypes that match a set of terms.
     *
     * Each term is one of:
     * - `T`: required component, accessed for writing.
     * - `const T`: required component, accessed for reading.
     * - `With<T>`: required component or tag, without access to its data.
     * - `Without<T>`: the archetype must not have T.
     *
     * Checked at compile time:
     * - The component type of every term satisfies Component.
     * - There is at least one term.
     * - No component appears twice, in any form (`T` together with `Without<T>` is a contradiction).
     * - No term is a reference.
     * - Tags are only used through With or Without, since they have no data to access.
     * @tparam Terms Query terms.
     */
    template<typename... Terms> requires (Component<MP::base_t<Terms>> && ...)
    class Query
    {
        compileTimeAssertion(sizeof...(Terms) > 0, "Empty querys are not allowed");
        compileTimeAssertion(MP::is_unique_v<MP::base_t<Terms>...>, "Query types must be uniques.");
        compileTimeAssertion((!std::is_reference_v<Terms> && ...), "Reference is not a valid type.");
        compileTimeAssertion(( ( (MP::isWith_v<Terms> || MP::isWithout_v<Terms>) || !std::is_empty_v<MP::base_t<Terms>> ) && ... ), "Tag used as data term: use With<T>");
        public:
            using Data_t = MP::Filter_t<Terms...>; ///< Typelist with the data terms (`T` and `const T`), in the order they were written. E.g. `MP::Typelist<Position, const Velocity>`.
        
    };
} // namespace Swelter