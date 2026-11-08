#pragma once

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
    class World
    {
        public:
            World() :
                table(),
                archetypes(),
                cold_map()
            {
                findOrCreateArchetype({});
            }

            template<typename... Ts>  
                requires (Component<std::remove_cvref_t<Ts>> && ...)
            Entity create(Ts&&... values)
            {
                compileTimeAssertion(MP::is_unique_v<std::remove_cvref_t<Ts>...>);

                Entity new_e = table.reserve();

                std::vector<CmpRegistry::CmpId_t> sorted_ids = {CmpRegistry::id<std::remove_cvref_t<Ts>>() ...};
                std::sort(sorted_ids.begin(), sorted_ids.end());

                ArchetypeId_t destination = findOrCreateArchetype(sorted_ids);
                Archetype* arch =  archetypes[destination].get();

                RowLocation loc = arch->addRow(new_e);

                ([&]() {
                    if constexpr (!std::is_empty_v<std::remove_cvref_t<Ts>>) {
                        std::construct_at(arch->getColumn<std::remove_cvref_t<Ts>>(loc.chunk_idx) + loc.row, std::forward<Ts>(values));
                    }
                }(), ...);

                table.place(new_e, destination, loc);
                
                return new_e;
            }

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

            bool valid(const Entity e) const noexcept
            {
                return table.isAlive(e);
            }

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

            template<Component T>
            bool has(const Entity& e) const 
            {
                debugAssertion(table.isAlive(e));

                EntityLocation loc = table.location(e);
                debugAssertion(loc.archetype != INVALID_ARCHETYPE);

                Archetype* arch = archetypes[loc.archetype].get();

                return arch->bitset.test(CmpRegistry::id<T>());
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
                ArchetypeId_t debugFindOrCreateArchetype(const std::vector<CmpRegistry::CmpId_t>& sorted_ids)
                {
                    return findOrCreateArchetype(sorted_ids);
                }

                /**
                 * @brief Test only: exposes archetypeWith.
                 *
                 * Only available in debug builds.
                 */
                ArchetypeId_t debugArchetypeWith(const ArchetypeId_t src, const CmpRegistry::CmpId_t id)
                {
                    return archetypeWith(src, id);
                }

                /**
                 * @brief Test only: exposes archetypeWithout.
                 *
                 * Only available in debug builds.
                 */
                ArchetypeId_t debugArchetypeWithout(const ArchetypeId_t src, const CmpRegistry::CmpId_t id)
                {
                    return archetypeWithout(src, id);
                }
            #endif


            World(const World&) = delete;
            World(World&&) = delete;
            World& operator=(const World&) = delete;
            World& operator=(World&&) = delete;

        private:
            
            RowLocation moveEntity(const Entity e, const ArchetypeId_t dst)
            {
                debugAssertion(table.isAlive(e));
                EntityLocation src_loc = table.location(e);
                debugAssertion(dst != src_loc.archetype);

                RowLocation dst_loc = archetypes[dst]->addRow(e);

                const std::vector<CmpRegistry::CmpId_t>& src_ids = archetypes[src_loc.archetype]->ids; 
                const std::vector<CmpRegistry::CmpId_t>& dst_ids = archetypes[dst]->ids; 

                std::size_t i {};
                std::size_t j {};
                while(i < src_ids.size() && j < dst_ids.size())
                {
                    const CmpRegistry::CmpId_t src_id = src_ids[i];
                    const ComponentInfo src_info = CmpRegistry::info(src_id);

                    const CmpRegistry::CmpId_t dst_id = dst_ids[j];
                    const ComponentInfo dst_info =  CmpRegistry::info(dst_id);

                    void* from = static_cast<std::byte*>(archetypes[src_loc.archetype]->getColumn(src_loc.location.chunk_idx, i)) + src_loc.location.row * src_info.size;
                    void* to = static_cast<std::byte*>(archetypes[dst]->getColumn(dst_loc.chunk_idx, j)) + dst_loc.row * dst_info.size;

                    if(src_id == dst_id)
                    {
                        if(!src_info.is_empty)
                        {

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
                        if(!src_info.trivially_copyable)
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
            ArchetypeId_t findOrCreateArchetype(const std::vector<CmpRegistry::CmpId_t>& sorted_ids)
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

            ArchetypeId_t archetypeWith(const ArchetypeId_t src, const CmpRegistry::CmpId_t id)
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
                    std::vector<CmpRegistry::CmpId_t> new_ids;
                    new_ids.reserve(src_arch->ids.size() + 1);

                    std::ranges::set_union(src_arch->ids,std::views::single(id),std::back_inserter(new_ids));
                    dst = findOrCreateArchetype(new_ids);       // If archetypes stops holding unique_ptr, a reallocation could invalidate src_arch.
                    Archetype* dst_arch = archetypes[dst].get();

                    src_arch->add_edges.push_back(Edge{id, dst});
                    dst_arch->remove_edges.push_back(Edge{id, src});
                }

                return dst;
            }

            ArchetypeId_t archetypeWithout(const ArchetypeId_t src, const CmpRegistry::CmpId_t id)
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
                    std::vector<CmpRegistry::CmpId_t> new_ids;
                    new_ids.reserve(src_arch->ids.size() - 1);

                    std::ranges::set_difference(src_arch->ids,std::views::single(id),std::back_inserter(new_ids));
                    dst = findOrCreateArchetype(new_ids);       // If archetypes stops holding unique_ptr, a reallocation could invalidate src_arch.
                    Archetype* dst_arch = archetypes[dst].get();

                    src_arch->remove_edges.push_back(Edge{id, dst});
                    dst_arch->add_edges.push_back(Edge{id, src});
                }

                return dst;
            }

            EntityTable table{};
            std::vector<std::unique_ptr<Archetype> > archetypes{};
            std::unordered_map<std::bitset<MAX_COMPONENTS>, ArchetypeId_t> cold_map;
    };
} // namespace Swelter