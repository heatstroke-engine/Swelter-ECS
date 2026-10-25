#pragma once

/**
 * @file registry.hpp
 * @brief Component registry: runtime IDs and type-erased component information.
 */

#include <swelter/config.hpp>
#include <warning.hpp>
#include <type_traits>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include <utility>
#include <cstdint>

namespace Swelter
{

    /**
     * @brief Type-erased description of a component type.
     *
     * The archetype storage only knows components through this struct, so it can
     * size, move and destroy them without knowing their C++ type.
     */
    struct ComponentInfo
    {
        // Component data
        std::uint32_t size          {0};        ///< Size in bytes. 0 for tags (empty types), which have no column.
        std::uint32_t align         {0};        ///< Alignment in bytes (alignof).
        bool trivially_copyable     {false};    ///< True if it can be moved with memcpy and needs no destructor call.
        bool is_empty               {false};    ///< True if it is a tag (std::is_empty_v).
        // Component type-erase functions
        void (*destroy)(void*) = nullptr;                   ///< Calls the destructor of the object at the given address.
        void (*move_construct)(void*, void*) = nullptr;     ///< Move-constructs into the first address from the object at the second one.
    };

    /**
     * @brief Builds the ComponentInfo of type @p T.
     * @tparam T Component type.
     * @return Size, alignment, traits and type-erased functions of @p T.
     */
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

    /**
     * @brief Requirements of a type to be used as a component.
     *
     * - An object type that is not an array.
     * - Alignment not greater than COLUMN_ALIGN, so it fits in an aligned column.
     * - Nothrow move and nothrow destruction: rows are moved and destroyed when
     *   entities are removed, and those operations must not fail halfway.
     */
    template<typename T>
    concept Component = (!std::is_array_v<T> &&
                        std::is_object_v<T> &&
                        (alignof(T) <= COLUMN_ALIGN) &&
                        std::is_nothrow_move_constructible_v<T> &&
                        std::is_nothrow_destructible_v<T>
                    );

    /**
     * @brief Global registry that gives each component type a runtime ID.
     *
     * Any C++ type that satisfies Component can be used without declaring it first:
     * the first time it is used it gets the next free ID (a counter), so IDs are small and dense.
     * @note IDs depend on the order of first use. They are not constexpr and are not stable between runs.
     * @warning Not thread-safe: register all the components before starting any parallel work.
     */
    struct CmpRegistry
    {
        using CmpId_t = std::uint32_t;  ///< Component ID type.

        /**
         * @brief Returns the ID of @p T, registering it the first time.
         *
         * cv-qualifiers and references are removed, so `T`, `const T` and `T&` share the same ID.
         * @tparam T Component type.
         * @return The ID of @p T.
         * @throws std::length_error If registering @p T exceeds MAX_COMPONENTS.
         */
        template<typename T>
        static CmpId_t id() requires Component<std::remove_cvref_t<T>>
        {
            return typeId<std::remove_cvref_t<T>>();
        }

        /**
         * @brief Registers several component types at once, in the given order.
         *
         * Optional: it only fixes the order of the IDs.
         * @tparam Ts Component types.
         * @throws std::length_error If registering them exceeds MAX_COMPONENTS.
         */
        template<typename... Ts>
        static void registerComponents()
        {
            (id<Ts>(), ...);
        }

        /**
         * @brief Returns the information of the component with ID @p id.
         * @param id ID of an already registered component.
         * @return A copy of its ComponentInfo.
         * @pre `id < count()`.
         */
        static ComponentInfo info(CmpId_t id)
        {
            debugAssertion(id < cmpinfo.size(), "Out of bounds id");

            return cmpinfo[id];
        }

        /**
         * @brief Returns the information of component @p T, registering it the first time.
         * @tparam T Component type.
         * @return A copy of its ComponentInfo.
         */
        template<typename T>
        static ComponentInfo info()
        {
            return info(id<T>());
        }

        /**
         * @brief Number of registered component types.
         */
        static std::size_t count() noexcept
        {
            return cmpinfo.size();
        }

        private:
            /**
             * @brief Holds the ID of @p T in a function-local static, so it is assigned only once.
             */
            template<Component T>
            static CmpId_t typeId()
            {
                static const CmpId_t id = enlistType(makeCMPInfo<T>());

                return id;
            }

            /**
             * @brief Appends a new component to the registry.
             * @param cmpinfo0 Information of the new component.
             * @return The ID given to it (its index in the registry).
             * @throws std::length_error If the registry already has MAX_COMPONENTS types.
             */
            static CmpId_t enlistType(ComponentInfo cmpinfo0)
            {
                if(cmpinfo.size() + 1 <= MAX_COMPONENTS)
                {
                    cmpinfo.emplace_back(cmpinfo0);
                }
                else
                {
                    throw std::length_error{"Max component capacity exceed."};
                }

                return static_cast<CmpId_t>(cmpinfo.size() - 1UL);
            }

            inline constinit static std::vector<ComponentInfo> cmpinfo; ///< Information of each component, indexed by ID. TODO: Use mutex.
    };
}
