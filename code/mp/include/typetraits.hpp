#pragma once

#include <warning.hpp>
#include <cstdint>

namespace MP
{
    // Typelist FW
    template<typename... Ts>
    struct Typelist;


    /**
     * @brief Wraps a compile-time constant of type T.
     * @tparam T   Type of the constant.
     * @tparam VAL Value of the constant.
     */
    template<typename T, T VAL>
    struct constant
    {
        static constexpr T value {VAL};
    };

    /**
     * @brief Holds a bool value that is true when T and U are the same type.
     * @tparam T First type to compare.
     * @tparam U Second type to compare.
     */
    template<typename T, typename U>
    struct is_same : constant<bool, false>
    {
    };

    /// @brief Specialization for T and U being the same type.
    template<typename T>
    struct is_same<T,T> : constant<bool, true>
    {
    };

    /// @brief Shortcut to is_same<T,U>::value.
    template<typename T, typename U>
    constexpr bool is_same_v = is_same<T,U>::value;

    /**
     * @brief Holds a bool value that is true when no type appears twice in Ts.
     * @tparam Ts Pack of types to check.
     * @note This primary template is the base case: an empty pack is unique.
     */
    template<typename... Ts>
    struct is_unique : constant<bool, true>
    {

    };

    /// @brief Recursive case: T is not repeated in the rest of the pack, and the rest is unique.
    template<typename T, typename... Ts>
    struct is_unique<T,Ts...> : constant<bool, ((true && ... && !is_same_v<T,Ts>) && (is_unique<Ts...>::value))>
    {
    };

    /// @brief Shortcut to is_unique<Ts...>::value.
    template<typename... Ts>
    constexpr bool is_unique_v = is_unique<Ts...>::value;

    /**
     * @brief Identity trait: exposes T as the member type.
     * @tparam T Type to expose.
     */
    template<typename T>
    struct type_id
    {
        using type = T;
    };

    /**
     * @brief Retrieves the N-th type of a type pack.
     * @tparam N  Zero-based index of the desired type.
     * @tparam Ts Pack of types to look into.
     * @note The pack must not be empty.
     */
    template<std::size_t N, typename... Ts>
    struct nth_type
    {
        compileTimeAssertion(sizeof...(Ts) != 0, "Error: TypeList with 0 types.");
    };

    /// @brief Shortcut to nth_type<N, Ts...>::type.
    template<std::size_t N, typename... Ts>
    using nth_type_t = typename nth_type<N, Ts...>::type;

    /// @brief Base case: index 0 yields the first type of the pack.
    template<typename T, typename... Ts>
    struct nth_type<0, T, Ts...> : type_id<T>
    {

    };

    /// @brief Recursive case: discards the first type and decrements N.
    template<std::size_t N, typename T, typename... Ts>
    struct nth_type<N, T, Ts...> : type_id < nth_type_t<N-1,Ts...> >
    {

    };

    /**
     * @brief Holds the position of type T in a type pack.
     * @tparam T  Type to look for.
     * @tparam Ts Pack of types to look into.
     * @note T must be part of Ts.
     */
    template<typename T, typename... Ts>
    struct pos_type
    {
        compileTimeAssertion(sizeof...(Ts) != 0 );
    };

    /// @brief Base case: T is the first type of the pack, position 0.
    template<typename T, typename... Ts>
    struct pos_type<T,T,Ts...> : constant<std::size_t,0>
    {

    };

    /// @brief Recursive case: discards the first type and adds 1 to the position.
    template<typename T, typename U,typename... Ts>
    struct pos_type<T,U,Ts...> : constant<std::size_t, pos_type<T,Ts...>::value + 1>
    {

    };

    /// @brief Shortcut to pos_type<T, Ts...>::value.
    template<typename T, typename... Ts>
    constexpr std::size_t pos_type_v = pos_type<T, Ts...>::value;

    /**
     * @brief Retrieves the type at a given position in a parameter pack.
     * @tparam i  Zero-based index of the desired type.
     * @tparam Ts Parameter pack to look into.
     * @note i must be less than sizeof...(Ts).
     */
    template<std::size_t i, typename... Ts>
    struct type_at
    {
        compileTimeAssertion(i < sizeof...(Ts));
    };

    /// @brief Base case: index 0 yields the first type of the pack.
    template<typename T, typename... Ts>
    struct type_at<0, T, Ts...> : type_id<T>
    {

    };

    /// @brief Recursive case: discards the first type and decrements i.
    template<std::size_t i, typename T, typename... Ts>
    struct type_at<i,T,Ts...> : type_at<i-1,Ts...>
    {

    };

    /// @brief Shortcut to type_at<i, Ts...>::type.
    template<std::size_t i, typename... Ts>
    using type_at_t = typename type_at<i, Ts...>::type;

    /**
     * @brief Compile-time conditional: yields T when C is true, F otherwise.
     * @tparam C Condition to evaluate.
     * @tparam T Type yielded when C is true.
     * @tparam F Type yielded when C is false.
     */
    template<bool C, typename T, typename F>
    struct IFT : type_id<F>
    {

    };

    /// @brief Specialization for a true condition.
    template<typename T, typename F>
    struct IFT<true,T,F> : type_id<T>
    {

    };

    /// @brief Shortcut to IFT<C,T,U>::type.
    template<bool C, typename T, typename U>
    using IFT_t = IFT<C,T,U>::type;

    /**
     * @brief Builds the type New<Ts...> from the types stored in a Typelist.
     * @tparam New  Template to instantiate.
     * @tparam List Typelist whose types are used as arguments of New.
     */
    template<template <typename...> class New, typename List>
    struct replace
    {

    };

    /// @brief Specialization that unpacks the types of the Typelist.
    template<template <typename...> class New, typename... Ts>
    struct replace<New, Typelist<Ts...>> : type_id<New<Ts...>>
    {

    };

    /// @brief Shortcut to replace<New, List>::type.
    template<template <typename...> class New, typename List>
    using replace_t =  replace<New, List>::type;

    /**
     * @brief Builds a Typelist where each type T of List is wrapped as Container<T>.
     * @tparam Container Template used to wrap each type.
     * @tparam List      Typelist with the types to wrap.
     */
    template<template <class... > class Container, typename List>
    struct for_all_insert_template
    {

    };

    /// @brief Specialization that unpacks the types of the Typelist.
    template<template <class... > class Container,  typename... Ts>
    struct for_all_insert_template<Container, Typelist<Ts...>> : type_id<Typelist<Container<Ts>...>>
    {

    };

    /// @brief Shortcut to for_all_insert_template<Container, List>::type.
    template<template <class... > class Container, typename List>
    using for_all_insert_template_t = for_all_insert_template<Container, List>::type;



    /**
     * @brief Compile-time list of types.
     * @tparam Ts Types stored in the list.
     */
    template<typename... Ts>
    struct Typelist
    {
        /// @brief Number of types in the list.
        consteval static uint8_t size() noexcept
        {
            return sizeof...(Ts);
        }

        /**
         * @brief Checks whether T is part of the list.
         * @tparam T Type to look for.
         * @return true if T is in the list.
         */
        template<typename T>
        consteval static bool contains() noexcept
        {
            return (false || ... || is_same_v<T,Ts>);
        }

        /**
         * @brief Checks whether OTS is include in the list.
         * @tparam OTs Types.
         * @return true if OTs are include in the list.
         */
        template<typename... OTs>
        consteval static bool includes(Typelist<OTs...>) noexcept
        {
            return (true && ... && contains<OTs>());
        }

        /**
         * @brief Position of T in the list.
         * @tparam T Type to look for.
         * @return Zero-based position of T.
         * @note T must be part of the list.
         */
        template<typename T>
        consteval static std::size_t pos() noexcept
        {
            compileTimeAssertion(contains<T>() && "T is not part of the typelist");
            return pos_type_v<T,Ts...>;
        }

        /**
         * @brief Type stored at position i of the list.
         * @tparam i Zero-based index of the desired type.
         */
        template<std::size_t i>
        using at = type_at_t<i, Ts...>;

    };

    /**
     * @brief Basic traits for a Typelist: size, type ids and bit masks.
     * @tparam TL Typelist to extract the traits from.
     * @note The list can hold at most 64 types.
     */
    template<typename TL>
    struct type_traits
    {
        constexpr static uint8_t list_size = TL::size();
        compileTimeAssertion(list_size <= 64);

        /// @brief Smallest unsigned integer able to hold one bit per type of the list.
        using mask_type =   IFT_t< list_size <= 32 ,
                                    IFT_t< list_size <= 16,
                                    IFT_t<list_size <= 8, uint8_t,
                                    uint16_t>,
                                    uint32_t>,
                                    uint64_t>;

        /// @brief Number of types in the list.
        consteval static uint8_t size() noexcept
        {
            return TL::size();
        }

        /**
         * @brief Identifier of T, which is its position in the list.
         * @tparam T Type to look for.
         * @note T must be part of the list.
         */
        template<typename T>
        consteval static uint8_t id() noexcept
        {
            compileTimeAssertion(TL::template contains<T>());
            return TL::template pos<T>();
        }

        /**
         * @brief Bit mask with the bit of each type in Ts set.
         * @tparam Ts Types whose bits are set. All must be part of the list.
         */
        template<typename... Ts>
        consteval static mask_type mask() noexcept
        {
            return ( mask_type{0} | ... | ( mask_type{1} << id<Ts>() ) );
        }

        /**
         * @brief Bit mask with the bit of each type in Ts set.
         * @tparam Ts Types whose bits are set. All must be part of the list.
         */
        template<typename... Ts>
        consteval static mask_type mask(Typelist<Ts...>) noexcept
        {
            return (mask<Ts...>()) ;
        }

        /**
         * @brief Type stored at position i of the list.
         * @tparam i Zero-based index of the desired type.
         */
        template<std::size_t i>
        using at = TL::template at<i>;
    };

}
