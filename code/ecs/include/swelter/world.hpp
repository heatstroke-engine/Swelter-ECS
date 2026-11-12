#pragma once

/**
 * @file world.hpp
 * @brief World: owns the entities, the archetypes and the archetype graph.
 */

#include <swelter/config.hpp>
#include <swelter/registry.hpp>
#include <swelter/archetype.hpp>
#include <swelter/entity_table.hpp>
#include <typetraits.hpp>
#include <algorithm>
#include <iterator>
#include <vector>
#include <memory>
#include <ranges>
#include <unordered_map>

namespace Swelter
{
    /**
     * @brief Container of entities and their components.
     *
     * Each entity lives in the archetype that matches its exact set of components.
     * Archetypes are linked in a graph: an edge says where an entity goes when a
     * component (or a bundle of components) is added or removed, so the hot path
     * follows an edge instead of searching. When there is no edge yet, the archetype
     * is found (or created) by its set of IDs in a map, and the edge is stored.
     *
     * Archetypes are never destroyed while the world is alive, so an archetype ID
     * stays valid. The root archetype (no components) always has ID 0.
     * @warning Adding or removing components and destroying entities moves rows, so
     *          references returned by get() can stop being valid after those calls.
     * @warning Not thread-safe.
     */
    class World
    {
        public:
            static const ArchetypeId_t ROOT_ARCHETYPE = 0U;     ///< Archetype without components, where every entity starts.

            /**
             * @brief Creates an empty world with only the root archetype (ID 0).
             */
            World() :
                table(),
                archetypes(),
                cold_map()
            {
                findOrCreateArchetype({});
            }

            /**
             * @brief Creates an entity with the given components.
             *
             * The destination is found by following the edge of the bundle @p Ts from the
             * root archetype, so only the first call with a set of types searches or creates
             * the archetype. The order of the arguments doesn't matter: every order leads to
             * the same archetype. rvalues are moved and lvalues are copied. With no arguments
             * the entity lives in the root archetype.
             * @tparam Ts Component types. cv-qualifiers and references are ignored.
             * @param values Initial value of each component.
             * @return The new entity.
             * @pre No type is repeated (checked at compile time).
             */
            template<typename... Ts>
                requires (Component<std::remove_cvref_t<Ts>> && ...)
            Entity create(Ts&&... values)
            {
                compileTimeAssertion(MP::is_unique_v<std::remove_cvref_t<Ts>...>);

                ArchetypeId_t destination = archetypeWithBundle(ROOT_ARCHETYPE,BundleRegistry::id<Ts...>());
                Archetype* arch =  archetypes[destination].get();
                
                Entity new_e = table.reserve();
                RowLocation loc = arch->addRow(new_e);

                ([&]() {
                    if constexpr (!std::is_empty_v<std::remove_cvref_t<Ts>>) {
                        std::construct_at(arch->getColumn<std::remove_cvref_t<Ts>>(loc.chunk_idx) + loc.row, std::forward<Ts>(values));
                    }
                }(), ...);

                table.place(new_e, destination, loc);
                
                return new_e;
            }

            /**
             * @brief Destroys an entity and all its components.
             *
             * The last row of its archetype fills the hole, and the location of the
             * moved entity is updated. The index is released with the next generation,
             * so old copies of @p e are no longer valid().
             * @param e Entity to destroy.
             * @pre @p e is alive. Otherwise it is reported by runTimeAssertion and nothing is done.
             * @warning References returned by get() for other entities of the same archetype can stop being valid.
             */
            void destroy(const Entity& e)
            {
                if(runTimeAssertion(table.isAlive(e)))
                {
                    if(table.isPlaced(e))
                    {
                        EntityLocation loc = table.location(e);
                        std::optional<Entity> moved_e = archetypes[loc.archetype]->removeRow(loc.location.chunk_idx,loc.location.row);
                        if(moved_e)
                        {
                            table.place(*moved_e,loc.archetype,{loc.location.chunk_idx,loc.location.row});
                        }
                    }
                    table.release(e);
                }
            }

            /**
             * @brief Checks whether an entity is alive in this world.
             * @param e Entity to check.
             * @return true if @p e was created and not destroyed (its generation matches).
             */
            bool valid(const Entity e) const noexcept
            {
                return table.isAlive(e);
            }

            /**
             * @brief Access to a component of an entity.
             * @tparam T Component type. Must not be a tag (checked at compile time).
             * @param e Entity.
             * @return Reference to the component, inside its chunk.
             * @pre @p e is alive and has @p T.
             * @warning The reference stops being valid after add(), remove() or destroy()
             *          of an entity of the same archetype.
             */
            template<Component T>
            T& get(const Entity& e)
            {
                compileTimeAssertion(!std::is_empty_v<T>);
                debugAssertion(table.isAlive(e));

                EntityLocation loc = table.location(e);
                debugAssertion(loc.archetype != INVALID_ARCHETYPE);

                Archetype* arch = archetypes[loc.archetype].get();

                return *(arch->getColumn<T>(loc.location.chunk_idx) + loc.location.row);
            }

            /**
             * @brief Read-only access to a component of an entity.
             * @copydetails get(const Entity&)
             */
            template<Component T>
            const T& get(const Entity& e) const
            {
                compileTimeAssertion(!std::is_empty_v<T>);
                debugAssertion(table.isAlive(e));

                EntityLocation loc = table.location(e);
                debugAssertion(loc.archetype != INVALID_ARCHETYPE);

                Archetype* arch = archetypes[loc.archetype].get();
                
                return *(arch->getColumn<T>(loc.location.chunk_idx) + loc.location.row);
            }

            /**
             * @brief Checks whether an entity has a component.
             *
             * Works with tags too.
             * @tparam T Component type.
             * @param e Entity.
             * @return true if the archetype of @p e has @p T.
             * @pre @p e is alive.
             */
            template<Component T>
            bool has(const Entity& e) const
            {
                debugAssertion(table.isAlive(e));

                EntityLocation loc = table.location(e);
                debugAssertion(loc.archetype != INVALID_ARCHETYPE);

                Archetype* arch = archetypes[loc.archetype].get();

                return arch->bitset.test(CmpRegistry::id<T>());
            }

            /**
             * @brief Adds one or more components to an entity with a single move.
             *
             * The destination is found by following the edge of the bundle @p Ts, so there
             * are no intermediate archetypes. The components can overlap with the ones the
             * entity already has: those are overwritten and the rest are constructed. If it
             * already has all of them, the entity doesn't move and only the values change.
             * Tags it already has are left as they are.
             * @tparam Ts Component types. cv-qualifiers and references are ignored.
             * @param e Entity.
             * @param values Value of each component: rvalues are moved and lvalues are copied.
             * @pre @p e is alive. No type is repeated (checked at compile time).
             * @warning References returned by get() can stop being valid.
             */
            template<typename... Ts> requires (sizeof...(Ts) > 0 && (Component<std::remove_cvref_t<Ts>> && ...))
            void add(const Entity e, Ts&&... values)
            {
                compileTimeAssertion(MP::is_unique_v<std::remove_cvref_t<Ts>...>);
                debugAssertion(table.isAlive(e));
                
                std::tuple<std::remove_cvref_t<Ts>...> tmp{std::forward<Ts>(values)...};

                const EntityLocation e_loc = table.location(e);

                const ArchetypeId_t src = e_loc.archetype;
                const ArchetypeId_t dst = archetypeWithBundle(src,BundleRegistry::id<Ts...>());

                const RowLocation loc = src != dst ? moveEntity(e,dst) : e_loc.location;
    
                ([&]() {
                    using U = std::remove_cvref_t<Ts>;

                    if constexpr (!std::is_empty_v<U>)
                    {
                        U* p = archetypes[dst]->getColumn<U>(loc.chunk_idx) + loc.row;
                        const CmpId_t cmp_id = CmpRegistry::id<U>();
                        
                        if(archetypes[src]->bitset.test(cmp_id))
                        {
                            *p = std::move(std::get<U>(tmp));
                        }
                        else
                        {
                            std::construct_at(p,std::move(std::get<U>(tmp)));
                        }
                    }
                }(), ...);
                
            }

            /**
             * @brief Removes one or more components from an entity with a single move.
             *
             * The destination is found by following the edge of the bundle @p Ts. The removed
             * components are destroyed. The ones the entity doesn't have are ignored; if it
             * has none of them, nothing is done and a warning is logged.
             * @tparam Ts Component types. cv-qualifiers and references are ignored.
             * @param e Entity.
             * @pre @p e is alive. No type is repeated (checked at compile time).
             * @warning References returned by get() can stop being valid.
             */
            template<typename... Ts> requires (sizeof...(Ts) > 0 && (Component<std::remove_cvref_t<Ts>> && ...))
            void remove(const Entity e)
            {
                compileTimeAssertion(MP::is_unique_v<std::remove_cvref_t<Ts>...>);
                debugAssertion(table.isAlive(e));

                const ArchetypeId_t src = table.location(e).archetype;
                const ArchetypeId_t dst = archetypeWithoutBundle(src,BundleRegistry::id<Ts...>());

                if(dst == src)
                {
                    warning("Removing non present/s CMP/s");
                }
                else
                {
                    moveEntity(e, dst);
                }
            }

            #ifdef HS_DEBUG_MODE
                /**
                 * @brief Test only: number of archetypes, including the root one.
                 *
                 * Only available in debug builds.
                 */
                std::size_t debugArchetypeCount() const noexcept
                {
                    return archetypes.size();
                }

                /**
                 * @brief Test only: read-only access to an archetype.
                 *
                 * Only available in debug builds.
                 * @param id Archetype ID.
                 * @pre `id < debugArchetypeCount()`.
                 */
                const Archetype& debugArchetype(const ArchetypeId_t id) const noexcept
                {
                    return *archetypes[id];
                }

                /**
                 * @brief Test only: archetype where a placed entity lives.
                 *
                 * Only available in debug builds.
                 * @param e Live and placed entity.
                 */
                ArchetypeId_t debugArchetypeOf(const Entity& e) const noexcept
                {
                    return table.location(e).archetype;
                }

                /**
                 * @brief Test only: exposes findOrCreateArchetype.
                 *
                 * Only available in debug builds.
                 */
                ArchetypeId_t debugFindOrCreateArchetype(const std::vector<CmpId_t>& sorted_ids)
                {
                    return findOrCreateArchetype(sorted_ids);
                }

                /**
                 * @brief Test only: exposes archetypeWith.
                 *
                 * Only available in debug builds.
                 */
                ArchetypeId_t debugArchetypeWith(const ArchetypeId_t src, const CmpId_t id)
                {
                    return archetypeWith(src, id);
                }

                /**
                 * @brief Test only: exposes archetypeWithout.
                 *
                 * Only available in debug builds.
                 */
                ArchetypeId_t debugArchetypeWithout(const ArchetypeId_t src, const CmpId_t id)
                {
                    return archetypeWithout(src, id);
                }

                /**
                 * @brief Test only: exposes archetypeWithBundle.
                 *
                 * Only available in debug builds.
                 */
                ArchetypeId_t debugArchetypeWithBundle(const ArchetypeId_t src, const BundleId_t id)
                {
                    return archetypeWithBundle(src, id);
                }

                /**
                 * @brief Test only: exposes archetypeWithoutBundle.
                 *
                 * Only available in debug builds.
                 */
                ArchetypeId_t debugArchetypeWithoutBundle(const ArchetypeId_t src, const BundleId_t id)
                {
                    return archetypeWithoutBundle(src, id);
                }
            #endif


            // A world can't be copied or moved
            World(const World&) = delete;
            World(World&&) = delete;
            World& operator=(const World&) = delete;
            World& operator=(World&&) = delete;

        private:

            /**
             * @brief Moves an entity from its archetype to @p dst.
             *
             * Walks the sorted IDs of both archetypes at the same time (like a merge):
             * - in both: the component is moved (memcpy if trivially copyable, move + destroy otherwise);
             * - only in the source: the component is destroyed;
             * - only in the destination: nothing, the caller must construct it.
             *
             * The row in @p dst is added first.
             * Then the hole in the source is filled with fillHole() (its components are
             * already moved or destroyed) and both entities are updated in the table:
             * @p e and the one moved into the hole, if any.
             * @param e Live and placed entity.
             * @param dst Destination archetype.
             * @return Location of the new row of @p e in @p dst.
             * @pre @p dst is not the current archetype of @p e.
             * @warning The components that are only in @p dst are left unconstructed.
             */
            RowLocation moveEntity(const Entity e, const ArchetypeId_t dst)
            {
                debugAssertion(table.isAlive(e));
                EntityLocation src_loc = table.location(e);
                debugAssertion(dst != src_loc.archetype);

                RowLocation dst_loc = archetypes[dst]->addRow(e);

                const std::vector<CmpId_t>& src_ids = archetypes[src_loc.archetype]->ids; 
                const std::vector<CmpId_t>& dst_ids = archetypes[dst]->ids; 

                std::size_t i {};
                std::size_t j {};
                while(i < src_ids.size())
                {
                    const CmpId_t src_id = src_ids[i];
                    const ComponentInfo src_info = archetypes[src_loc.archetype]->cmps_info[i];
                    void* from = static_cast<std::byte*>(archetypes[src_loc.archetype]->getColumn(src_loc.location.chunk_idx, i)) + src_loc.location.row * src_info.size;

                    const CmpId_t dst_id = j == dst_ids.size() ?  MAX_COMPONENTS : dst_ids[j];
                    
                    if(src_id == dst_id)
                    {
                        if(!src_info.is_empty)
                        {

                            const ComponentInfo dst_info =  archetypes[dst]->cmps_info[j];   
                            void* to = static_cast<std::byte*>(archetypes[dst]->getColumn(dst_loc.chunk_idx, j)) + dst_loc.row * dst_info.size;

                            if(src_info.trivially_copyable)
                            {
                                std::memcpy(to,from,dst_info.size);
                            }
                            else
                            {
                                dst_info.move_construct(to,from);
                                src_info.destroy(from);
                            }
                        }
                        ++i;
                        ++j;
                    }
                    else if(dst_id < src_id)
                    {
                        ++j;
                    }
                    else{
                        if(!src_info.trivially_copyable && !src_info.is_empty)
                        {
                            src_info.destroy(from);
                        }
                        ++i;
                    }

                }
                std::optional<Entity> moved = archetypes[src_loc.archetype]->fillHole(src_loc.location.chunk_idx, src_loc.location.row);
                if(moved)
                {
                    table.place(*moved, src_loc.archetype, src_loc.location);
                }
                table.place(e, dst, dst_loc);

                return dst_loc;
            }

            /**
             * @brief Cold path: finds the archetype of a set of IDs, creating it if it doesn't exist.
             *
             * The key of the map is the bitset of the IDs, which is unique for each set.
             * @param sorted_ids Component IDs, sorted and without repeats.
             * @return ID of the archetype.
             */
            ArchetypeId_t findOrCreateArchetype(const std::vector<CmpId_t>& sorted_ids)
            {
                ArchetypeId_t ret = INVALID_ARCHETYPE;

                std::bitset<MAX_COMPONENTS> ids_bitset = buildBitSet(sorted_ids);

                auto it = cold_map.find(ids_bitset);

                if(it == cold_map.end())
                {
                    archetypes.emplace_back(std::make_unique<Archetype>(sorted_ids,ids_bitset));
                    ret = archetypes.size() - 1;
                    cold_map.emplace(ids_bitset, ret);
                }
                else
                {
                    ret = it->second;
                }

                return ret;
            }

            /**
             * @brief Archetype reached by adding component @p id to @p src.
             *
             * Follows the add edge of @p src if there is one. Otherwise it gets the archetype
             * from findOrCreateArchetype() and stores the edge in both directions: add in
             * @p src and remove in the destination.
             * @param src Source archetype.
             * @param id Component ID.
             * @return Destination archetype, or @p src if it already has @p id (no edge is stored).
             */
            ArchetypeId_t archetypeWith(const ArchetypeId_t src, const CmpId_t id)
            {
                debugAssertion(src < archetypes.size());
                
                ArchetypeId_t dst = INVALID_ARCHETYPE;

                Archetype* src_arch = archetypes[src].get();
                if (src_arch->bitset.test(id))
                {
                    return src;
                }
                std::vector<Edge>& add = src_arch->add_edges;

                const auto it = std::ranges::find_if(add, [id](const Edge& e){ return e.id == id; });

                dst = (it == add.end()) ? INVALID_ARCHETYPE : it->target;

                if(dst == INVALID_ARCHETYPE)
                {
                    std::vector<CmpId_t> new_ids;
                    new_ids.reserve(src_arch->ids.size() + 1);

                    std::ranges::set_union(src_arch->ids,std::views::single(id),std::back_inserter(new_ids));
                    dst = findOrCreateArchetype(new_ids);       // If archetypes stops holding unique_ptr, a reallocation could invalidate src_arch.
                    Archetype* dst_arch = archetypes[dst].get();

                    src_arch->add_edges.push_back(Edge{id, dst});
                    dst_arch->remove_edges.push_back(Edge{id, src});
                }

                return dst;
            }

            /**
             * @brief Archetype reached by removing component @p id from @p src.
             *
             * Same as archetypeWith(), with the edges in the other direction.
             * @param src Source archetype.
             * @param id Component ID.
             * @return Destination archetype, or @p src if it doesn't have @p id (no edge is stored).
             */
            ArchetypeId_t archetypeWithout(const ArchetypeId_t src, const CmpId_t id)
            {
                debugAssertion(src < archetypes.size());
                
                ArchetypeId_t dst = INVALID_ARCHETYPE;

                Archetype* src_arch = archetypes[src].get();
                if (!src_arch->bitset.test(id))
                {
                    return src;
                }
                std::vector<Edge>& remove = src_arch->remove_edges;

                const auto it = std::ranges::find_if(remove, [id](const Edge& e){ return e.id == id; });

                dst = (it == remove.end()) ? INVALID_ARCHETYPE : it->target;

                if(dst == INVALID_ARCHETYPE)
                {
                    std::vector<CmpId_t> new_ids;
                    new_ids.reserve(src_arch->ids.size() - 1);

                    std::ranges::set_difference(src_arch->ids,std::views::single(id),std::back_inserter(new_ids));
                    dst = findOrCreateArchetype(new_ids);       // If archetypes stops holding unique_ptr, a reallocation could invalidate src_arch.
                    Archetype* dst_arch = archetypes[dst].get();

                    src_arch->remove_edges.push_back(Edge{id, dst});
                    dst_arch->add_edges.push_back(Edge{id, src});
                }

                return dst;
            }

            /**
             * @brief Archetype reached by adding every component of bundle @p id to @p src.
             *
             * Follows the bundle add edge of @p src if there is one. Otherwise it gets the
             * archetype of the union of both sets of IDs and stores only that edge: the
             * inverse is not stored, because removing the bundle doesn't always go back to
             * @p src (`{A} + {A,B} = {A,B}`, but `{A,B} - {A,B} = {}`).
             * @param src Source archetype.
             * @param id Bundle ID.
             * @return Destination archetype. It is @p src if it already has the whole bundle.
             */
            ArchetypeId_t archetypeWithBundle(const ArchetypeId_t src, const BundleId_t id)
            {
                debugAssertion(src < archetypes.size());

                ArchetypeId_t dst = INVALID_ARCHETYPE;

                Archetype* src_arch = archetypes[src].get();
                std::vector<BundleEdge>& add = src_arch->bundle_add_edges;

                const auto it = std::ranges::find_if(add, [id](const BundleEdge& e){ return e.id == id; });

                dst = (it == add.end()) ? INVALID_ARCHETYPE : it->target;

                if(dst == INVALID_ARCHETYPE)
                {
                    std::vector<CmpId_t> new_ids;
                    const std::vector<CmpId_t>& cmps_ids = BundleRegistry::ids(id);
                    new_ids.reserve(src_arch->ids.size() + cmps_ids.size());

                    std::ranges::set_union(src_arch->ids, cmps_ids, std::back_inserter(new_ids));
                    dst = findOrCreateArchetype(new_ids);       // If archetypes stops holding unique_ptr, a reallocation could invalidate src_arch.

                    src_arch->bundle_add_edges.push_back(BundleEdge{id, dst});
                }

                return dst;
            }

            /**
             * @brief Archetype reached by removing every component of bundle @p id from @p src.
             *
             * Same as archetypeWithBundle(), with the difference of both sets of IDs. The
             * components of the bundle that @p src doesn't have are ignored. Only this
             * edge is stored, not the inverse.
             * @param src Source archetype.
             * @param id Bundle ID.
             * @return Destination archetype. It is @p src if it has none of the components.
             */
            ArchetypeId_t archetypeWithoutBundle(const ArchetypeId_t src, const BundleId_t id)
            {
                debugAssertion(src < archetypes.size());
                
                ArchetypeId_t dst = INVALID_ARCHETYPE;

                Archetype* src_arch = archetypes[src].get();

                std::vector<BundleEdge>& remove = src_arch->bundle_remove_edges;

                const auto it = std::ranges::find_if(remove, [id](const BundleEdge& e){ return e.id == id; });

                dst = (it == remove.end()) ? INVALID_ARCHETYPE : it->target;

                if(dst == INVALID_ARCHETYPE)
                {
                    std::vector<CmpId_t> new_ids;
                    const std::vector<CmpId_t>& cmps_ids = BundleRegistry::ids(id);

                    new_ids.reserve(src_arch->ids.size());

                    std::ranges::set_difference(src_arch->ids,cmps_ids,std::back_inserter(new_ids));
                    dst = findOrCreateArchetype(new_ids);       // If archetypes stops holding unique_ptr, a reallocation could invalidate src_arch.

                    src_arch->bundle_remove_edges.push_back(BundleEdge{id, dst});
                }

                return dst;
            }

            EntityTable table{};                                                        ///< Generation and location of every entity.
            std::vector<std::unique_ptr<Archetype> > archetypes{};                      ///< All archetypes, indexed by ID. unique_ptr keeps their address stable when the vector grows.
            std::unordered_map<std::bitset<MAX_COMPONENTS>, ArchetypeId_t> cold_map;    ///< Cold path: archetype of each set of IDs, used when there is no edge.
    };
} // namespace Swelter