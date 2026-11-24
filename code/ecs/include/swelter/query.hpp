#pragma once

/**
 * @file query.hpp
 * @brief Cached queries: the archetypes that match a set of terms.
 */

#include <swelter/registry.hpp>
#include <swelter/archetype.hpp>
#include <swelter/world.hpp>
#include <typetraits.hpp>
#include <warning.hpp>
#include <bitset>
#include <vector>
#include <array>
#include <algorithm>
#include <span>
#include <utility>

namespace Swelter
{

    
    /**
     * @brief Query over the archetypes that match a set of terms.
     *
     * Each term is one of:
     * - `T`: required component, accessed for writing.
     * - `const T`: required component, accessed for reading.
     * - `With<T>`: required component or tag without access to its data.
     * - `Without<T>`: the archetype must not have T.
     *
     * Checked at compile time:
     * - The component type of every term satisfies Component.
     * - There is at least one term.
     * - No component appears twice, in any form (`T` together with `Without<T>` is a contradiction).
     * - No term is a reference.
     * - Tags are only used through With or Without, since they have no data to access.
     *
     * The query stores archetypes, not entities, so structural changes (create, destroy,
     * add, remove) never touch it. It keeps the matching archetypes in a cache, and on each
     * run it only checks the archetypes created since the last one: archetypes are never
     * deleted and only appended, so a counter is enough. For each matching archetype it also
     * stores the column of each data term, so iterating needs no search.
     *
     * Creating a query costs O(archetypes), so it is created once and reused: whoever uses
     * it keeps it.
     *
     * @code
     * auto q = world.query<Position, const Velocity, With<TPlayer>, Without<TDead>>();
     * q.each([](Position& p, const Velocity& v) { p.x += v.x; });
     * @endcode
     *
     * @note A copy of a query takes its cache with it; from then on each one updates on its own.
     * @warning The query keeps a pointer to its World: it must not be used after the World is destroyed.
     * @tparam Terms Query terms.
     */
    template<typename... Terms> requires (Component<MP::base_t<Terms>> && ...)
    class Query
    {
        compileTimeAssertion(sizeof...(Terms) > 0, "Empty query is not allowed");
        compileTimeAssertion(MP::is_unique_v<MP::base_t<Terms>...>, "Query terms must be unique.");
        compileTimeAssertion((!std::is_reference_v<Terms> && ...), "Reference is not a valid type.");
        compileTimeAssertion(( ( (MP::isWith_v<Terms> || MP::isWithout_v<Terms>) || !std::is_empty_v<MP::base_t<Terms>> ) && ... ), "Tag used as data term: use With<T>");
        public:
            using Data_t = MP::Filter_t<Terms...>; ///< Typelist with the data terms (`T` and `const T`), in the order they were written. E.g. `MP::Typelist<Position, const Velocity>`.

            /**
             * @brief Builds the query and finds every archetype of @p world0 that matches it.
             *
             * Registers the component of every term that wasn't registered yet (also the ones in Without).
             * Usually created through World::query.
             * @param world0 World whose archetypes the query looks at.
             * @note O(archetypes).
             */
            explicit Query(World& world0) :
                world(&world0),
                required(),
                excluded(),
                reads(),
                writes(),
                archetypes(),
                checked(0)
            {
                computeBitsets();
                update();

            }

            /**
             * @brief Components the query reads: its `const T` terms.
             *
             * Meant for the scheduler: two queries can run at the same time if neither writes
             * what the other reads or writes.
             */
            const std::bitset<MAX_COMPONENTS>& getReads() const noexcept
            {
                return reads;
            }

            /**
             * @brief Components the query writes: its `T` terms.
             * @copydetails getReads()
             */
            const std::bitset<MAX_COMPONENTS>& getWrites() const noexcept
            {
                return writes;
            }

            /**
             * @brief Calls @p f once per matching entity, with a reference to each of its data components.
             *
             * Two signatures are accepted, detected at compile time:
             * - `f(T&...)`: one reference per data term, in the order of the query.
             * - `f(Entity, T&...)`: the same with the entity first (`Entity` or `const Entity&`).
             *
             * `const T` terms are passed as `const T&`, and With/Without add no argument.
             * Built on eachChunk, so it updates the cache first and skips empty archetypes.
             *
             * @code
             * q.each([](Position& p, const Velocity& v) { p.x += v.x; });
             * q.each([](Entity e, Position& p, const Velocity& v) { ... });
             * @endcode
             * @tparam F Function to call.
             * @param f Function to call.
             * @note Take the components by reference: a copy would lose the writes.
             * @note If @p f can be called in both ways (e.g. `[](auto&... xs)`), the one with the entity is used.
             * @warning @p f must not make structural changes (create, destroy, add, remove): they move rows and chunks while they are being iterated.
             */
            template<typename F>
            void each(F&& f)
            {
               [&]<typename... Ts>(MP::Typelist<Ts...>) {
                    if constexpr (std::is_invocable_v<F&, const Entity&, Ts&...>)
                    {
                        eachChunk([&](std::span<const Entity> es, auto... spans){
                            for(std::size_t i {}; i < es.size(); ++i)
                            {
                                f(es[i], spans[i]...);
                            }
                        });
                    }
                    else if constexpr (std::is_invocable_v<F&, Ts&...>)
                    {
                        eachChunk([&](std::span<const Entity> es, auto... spans){
                            for(std::size_t i {}; i < es.size(); ++i)
                            {
                                f(spans[i]...);
                            }
                        });
                    }
                    else
                    {
                        compileTimeAssertion(false, "each: the function must take (T&...) or (Entity, T&...), one reference per data term in the order of the query. Use const T& for const terms.");
                    }
                }(Data_t{});
            }

            /**
             * @brief Calls @p f once per non-empty chunk of the matching archetypes.
             *
             * @p f receives the entities of the chunk and one span per data term, in the order
             * of the query: `f(std::span<const Entity>, std::span<T>...)`. `const T` terms give
             * `std::span<const T>`, and With/Without add no span. All the spans have the same size,
             * and row `i` of each span belongs to entity `i`.
             *
             * First it updates the cache with the archetypes created since the last run. Empty
             * archetypes have no chunks, so they are skipped without checking them.
             *
             * The chunk is the unit of work: in the multithreaded version each call can be a job.
             *
             * @code
             * q.eachChunk([](std::span<const Entity> es, std::span<Position> p, std::span<const Velocity> v) {
             *     for(std::size_t i{}; i < es.size(); ++i) p[i].x += v[i].x;
             * });
             * @endcode
             * @tparam F Function to call.
             * @param f Function to call.
             * @warning @p f must not make structural changes (create, destroy, add, remove): they move rows and chunks while they are being iterated.
             */
            template<typename F>
            void eachChunk(F&& f)
            {
                [&]<typename... Ts>(MP::Typelist<Ts...>) {
                    compileTimeAssertion(std::is_invocable_v<F&, std::span<const Entity>, std::span<Ts>...>, "eachChunk: the function must take (span<const Entity>, span<T>...), one span per data term in the order of the query. Use span<const T> for const terms.");
                }(Data_t{});

                update();

                for(MatchedArchetype& m : archetypes)
                {
                    std::span<const Archetype::ChunkDescriptor> chunks = m.arch->getChunks();
                    for(std::size_t i{}; i < chunks.size(); ++i)
                    {
                        const std::uint32_t count = chunks[i].count;
                        debugAssertion(count > 0);
                        
                        std::span<const Entity> entities = m.arch->getEntities(i);
                        [&]<typename... Ts>(MP::Typelist<Ts...>) {
                            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                                f(entities, std::span<Ts>(static_cast<Ts*>(m.arch->getColumn(i, m.columns[Is])), count) ...);
                            }(std::index_sequence_for<Ts...>{});
                        }(Data_t{});
                    }
                }
            }

            #ifdef HS_DEBUG_MODE
                /**
                 * @brief Test only: looks for matches in the archetypes created since the last update.
                 *
                 * Only available in debug builds.
                 */
                void debugUpdate()
                {
                    update();
                }

                /**
                 * @brief Test only: number of matching archetypes found so far.
                 *
                 * Only available in debug builds.
                 */
                std::size_t debugMatchedCount() const noexcept
                {
                    return archetypes.size();
                }

                /**
                 * @brief Test only: a matching archetype.
                 *
                 * Only available in debug builds.
                 * @param i Position in the cache, in the order they were found.
                 * @pre `i < debugMatchedCount()`.
                 */
                const Archetype& debugMatchedArchetype(const std::size_t i) const noexcept
                {
                    debugAssertion(i < archetypes.size());

                    return *archetypes[i].arch;
                }

                /**
                 * @brief Test only: column of a data term in a matching archetype.
                 *
                 * Only available in debug builds.
                 * @param i Position in the cache.
                 * @param k Position of the term in #Data_t.
                 * @pre `i < debugMatchedCount()` and `k < Data_t::size()`.
                 */
                std::uint32_t debugColumn(const std::size_t i, const std::size_t k) const noexcept
                {
                    debugAssertion(i < archetypes.size() && k < Data_t::size());

                    return archetypes[i].columns[k];
                }
            #endif

        private:

            /**
             * @brief An archetype that matches the query, with the columns of its data terms already resolved.
             */
            struct MatchedArchetype
            {
                Archetype* arch;                                        ///< The archetype. Stable: the World owns it through a unique_ptr and never deletes it.
                std::array<std::uint32_t, Data_t::size()> columns;      ///< Column of each data term in @ref arch, in the order of #Data_t (not of the IDs).
            };

            /**
             * @brief Tells whether an archetype has every required component and none of the excluded ones.
             * @param arch Archetype to check.
             */
            bool match(const Archetype* arch) const noexcept
            {
                return (arch->bitset & required) == required && (arch->bitset & excluded).none();
            }

            /**
             * @brief Adds to the cache the matching archetypes created since the last update.
             *
             * Only looks at the archetypes from #checked on. If there are no new ones it is a
             * single comparison. For each new match it resolves the column of each data term
             * once, with a binary search over the sorted IDs of the archetype.
             */
            void update()
            {
                while(checked < world->archetypes.size())
                {
                    Archetype* archetype = world->archetypes[checked].get();

                    if(match(archetype))
                    {
                        MatchedArchetype& mach = archetypes.emplace_back();

                        mach.arch = archetype;
                        std::size_t k {};
                        [&]<typename... Ts>(MP::Typelist<Ts...>) {
                        ([&]() {
                            auto it = std::lower_bound(archetype->ids.begin(), archetype->ids.end(), CmpRegistry::id<Ts>());
                            debugAssertion(it != archetype->ids.end() && *it == CmpRegistry::id<Ts>());
                            auto indice = it - archetype->ids.begin();
                            mach.columns[k++] = static_cast<std::uint32_t>(indice);
                        }(), ...);
                        }(Data_t{});
                    }
                    // Update checked arch counter.
                    ++checked;
                }
            }

            /**
             * @brief Fills the bitsets from the terms.
             *
             * - `T`: required and written.
             * - `const T`: required and read.
             * - `With<T>`: required only.
             * - `Without<T>`: excluded.
             */
            void computeBitsets()
            {
                ([&]() {

                    const CmpId_t term_id = CmpRegistry::id<MP::base_t<Terms>>();

                    if constexpr (MP::isWithout_v<Terms>)
                    {
                        excluded.set(term_id);
                    }
                    else
                    {
                        required.set(term_id);
                        if constexpr (!MP::isWith_v<Terms>)
                        {
                            if constexpr (std::is_const_v<Terms>)
                            {
                                reads.set(term_id);
                            }
                            else
                            {
                                writes.set(term_id);
                            }
                        }
                    }

                    

                }(), ...);
            }

            World* world;                               ///< World whose archetypes are matched. A pointer, not a reference, so the query can be assigned.
            std::bitset<MAX_COMPONENTS> required;       ///< Components the archetype must have: data terms and With.
            std::bitset<MAX_COMPONENTS> excluded;       ///< Components the archetype must not have: Without.
            std::bitset<MAX_COMPONENTS> reads;          ///< Components read: `const T` terms.
            std::bitset<MAX_COMPONENTS> writes;         ///< Components written: `T` terms.
            std::vector<MatchedArchetype> archetypes;   ///< Cache of matching archetypes, in the order they were found.
            std::size_t checked;                        ///< Number of archetypes of the World already checked: the next update starts here.
    };

   
} // namespace Swelter