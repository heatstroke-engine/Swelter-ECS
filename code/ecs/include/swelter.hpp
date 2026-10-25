#pragma once

#include <warning.hpp>
#include <type_traits>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include <cstddef> 
#include <cstring>
#include <algorithm>
#include <bitset>
#include <span>
#include <stdexcept>

namespace Swelter 
{
    constexpr std::size_t bytes_in_kb = 1024;   // Number of bytes in a KB
    constexpr std::size_t number_of_kb = 16;    // Number of desire KB
    constexpr std::size_t CHUNK_SIZE = number_of_kb * bytes_in_kb; // Chunk size = KB * 1024
    constexpr std::size_t MIN_CHUNK_CAPACITY = 16;
    constexpr std::size_t COLUMN_ALIGN = 64; // Align column

    constexpr std::size_t MAX_COMPONENTS = 256; // Max allowed cmps

    // Provisional entity with expected real size
    struct Entity
    {
        std::uint64_t dummy;
    };
    
    
    struct ComponentInfo
    {
        // Component data
        std::uint32_t size          {0}; 
        std::uint32_t align         {0};
        bool trivially_copyable     {false};
        bool is_empty               {false};
        // Component type-erase functions
        void (*destroy)(void*) = nullptr;   
        void (*move_construct)(void*, void*) = nullptr;
    };

    template<typename T>
    ComponentInfo makeCMPInfo()
    {
        return ComponentInfo{
            .size  =  std::is_empty_v<T> ?  std::uint32_t{0} : static_cast<std::uint32_t>(sizeof(T)),
            .align = alignof(T),
            .trivially_copyable = std::is_trivially_copyable_v<T>,
            .is_empty = std::is_empty_v<T>,
            .destroy = ([](void* ptr) {
                std::destroy_at(static_cast<T*>(ptr));
            }),
            .move_construct = ([](void* to, void* from) {
                std::construct_at(static_cast<T*>(to), std::move(*static_cast<T*>(from)));
            }) 
        };
    }

    template<typename T>
    concept Component = (!std::is_array_v<T> && 
                        std::is_object_v<T> && 
                        (alignof(T) <= COLUMN_ALIGN) && 
                        std::is_nothrow_move_constructible_v<T> && 
                        std::is_nothrow_destructible_v<T>
                    );

    struct CmpRegistry
    {
        using CmpId_t = std::uint32_t;

        template<typename T>
        static CmpId_t id() requires Component<std::remove_cvref_t<T>>
        {           
            return typeId<std::remove_cvref_t<T>>();
        }

        template<typename... Ts>
        static void registerComponents()
        {            
            (id<Ts>(), ...);
        }

        static ComponentInfo info(CmpId_t id) 
        {
            debugAssertion(id < cmpinfo.size(), "Out of bounds id");

            return cmpinfo[id];
        }

        template<typename T>
        static ComponentInfo info() 
        {
            return info(id<T>());
        }

        static std::size_t count() noexcept
        {
            return cmpinfo.size();
        }

        private:
            template<Component T>
            static CmpId_t typeId() noexcept
            {
                static const CmpId_t id = enlistType(makeCMPInfo<T>());
                
                return id;
            }

            static CmpId_t enlistType(ComponentInfo cmpinfo0) 
            {
                if(cmpinfo.size() + 1 <= MAX_COMPONENTS)
                {
                    cmpinfo.emplace_back(cmpinfo0);
                }
                else
                {
                    throw std::length_error{"Max component capacity exeded."};
                }

                return static_cast<CmpId_t>(cmpinfo.size() - 1UL);
            }

            inline constinit static std::vector<ComponentInfo> cmpinfo; // TODO: Use mutex.
    };

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
        std::byte data[CHUNK_SIZE];
    };
    compileTimeAssertion(sizeof(Chunk) == CHUNK_SIZE);


    struct ChunkPool
    {
        static Chunk* allocate()
        {
            Chunk* ret = nullptr;

            if(frees.empty())
            {
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

        static std::size_t ownedCount() noexcept
        {
            return owned.size();
        }

        static std::size_t freeCount() noexcept
        {
            return frees.size();
        }

        private:
            // TODO: Use mutex 
            inline static constinit std::vector<Chunk*> frees;
            inline static constinit std::vector<std::unique_ptr<Chunk>> owned;
    };

    namespace detail
    {
        template <typename T>
        constexpr T align_up(T val, T align) 
        {
            return (val + (align - 1)) & ~(align - 1);
        }
    }

    struct ArchetypeLayout
    {
        std::uint32_t capacity {};           // N
        std::vector<std::uint32_t> offsets;  // Cmps offset  
    };

    inline ArchetypeLayout computeLayout(const std::vector<ComponentInfo>& cmps_info0) 
    {

        ArchetypeLayout compute_result;
        compute_result.offsets.resize(cmps_info0.size()); 

        std::size_t total_size = sizeof(Entity);
        for(const auto cmp_info0 : cmps_info0)
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


    inline std::bitset<MAX_COMPONENTS> buildBitSet(const std::vector<CmpRegistry::CmpId_t>& ids)
    {
        std::bitset<MAX_COMPONENTS> bitset;

        for(const auto id : ids)
        {
            bitset.set(id, true);
        }

        return bitset;
    }

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

    struct EntInfo
    {
        std::uint32_t chunk_idx   {0};
        std::uint32_t row         {0};
    };

    struct Archetype
    {
        struct ChunkDescriptor
        {
            Chunk* chunk        {nullptr};
            std::uint32_t count {0};    // rows count
        };

        explicit Archetype(const std::vector<CmpRegistry::CmpId_t>& ids0) :
            ids(ids0),
            cmps_info(buildComponentInfo(ids0)),
            layout(computeLayout(cmps_info)),
            bitset(buildBitSet(ids0)),
            chunks()
        {

        }

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

        std::span<const ChunkDescriptor> getChunks() const noexcept
        {
            return chunks;
        }

        EntInfo addRow(const Entity& e) 
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

        void* getColumn(const std::size_t chunk_i, const std::size_t cmp_i) noexcept
        {
            debugAssertion(chunk_i < chunks.size() && cmp_i < layout.offsets.size());

            return chunks[chunk_i].chunk->data + layout.offsets[cmp_i];
        }

        template<typename T>
        T* getColumn(const std::size_t chunk_i) noexcept
        {
            auto it = std::ranges::lower_bound(ids, CmpRegistry::id<T>());
            debugAssertion(it != ids.end());
            debugAssertion(*it == CmpRegistry::id<T>());
            std::size_t cmp_i = std::distance(ids.begin(),it);

            return static_cast<T*>(getColumn(chunk_i, cmp_i));
        }

        std::span<const Entity> getEntities(const std::size_t chunk_i) const noexcept
        {
            debugAssertion(chunk_i < chunks.size());
            return {reinterpret_cast<const Entity*>(chunks[chunk_i].chunk->data), chunks[chunk_i].count};
        }
        std::span<Entity> getEntities(std::size_t chunk_i) noexcept
        {
            debugAssertion(chunk_i < chunks.size());
            return {reinterpret_cast<Entity*>(chunks[chunk_i].chunk->data), chunks[chunk_i].count};
        }
        std::optional<Entity> removeRow(const std::size_t chunk_i, const std::size_t row)
        {
            debugAssertion(chunk_i < chunks.size() && row < chunks[chunk_i].count);

            std::optional<Entity> moved_e;

            Chunk* target_chunk = chunks[chunk_i].chunk;
            void* row_start = target_chunk->data + row * sizeof(Entity);
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
            void* last_row = last_descriptor.chunk->data + last_count * sizeof(Entity);
            if(row_start != last_row)
            {
                moved_e.emplace();
                //Swap
                *reinterpret_cast<Entity*>(row_start) = *reinterpret_cast<Entity*>(last_row);
                moved_e = *reinterpret_cast<Entity*>(row_start);
                // Copy cmps
                for(std::size_t i{}; i < ids.size(); ++i)
                {
                    const ComponentInfo& cmp_info = cmps_info[i];
                    if (cmp_info.size == 0) continue;

                    void* to = reinterpret_cast<std::byte*>(getColumn(chunk_i, i)) + row * cmp_info.size;
                    void* from = reinterpret_cast<std::byte*>(getColumn(chunks.size() - 1, i)) + last_count * cmp_info.size;
                    const std::uint32_t cmp_size = cmp_info.size;

                    if(!cmp_info.trivially_copyable)
                    {
                        cmp_info.move_construct(to, from);
                        // Still need to destro nontrivials
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
        const std::vector<CmpRegistry::CmpId_t> ids;
        const std::vector<ComponentInfo> cmps_info;
        const ArchetypeLayout layout;
        const std::bitset<MAX_COMPONENTS> bitset;
        private:
            std::vector<ChunkDescriptor> chunks;
    };

} // Swelter namespace

