#pragma once
#include <boost/asio/awaitable.hpp>
#include <tuple>
#include <type_traits>

namespace httplib::util
{
    template <typename Function>
    struct function_traits;

    template <typename Return, typename... Arguments>
    struct function_traits<Return (*)(Arguments...)>
    {
        using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
        using return_type = Return;
    };

    template <typename Return, typename... Arguments>
    struct function_traits<Return (*)(Arguments...) noexcept>
    {
        using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
        using return_type = Return;
    };

    template <typename Return, typename... Arguments>
    struct function_traits<Return(Arguments...)>
    {
        using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
        using return_type = Return;
    };

    template <typename Return, typename... Arguments>
    struct function_traits<Return(Arguments...) noexcept>
    {
        using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
        using return_type = Return;
    };

    template <typename This, typename Return, typename... Arguments>
    struct function_traits<Return (This::*)(Arguments...)>
    {
        using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
        using return_type = Return;
        using class_type = This;
    };

    template <typename This, typename Return, typename... Arguments>
    struct function_traits<Return (This::*)(Arguments...) noexcept>
    {
        using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
        using return_type = Return;
        using class_type = This;
    };

    template <typename This, typename Return, typename... Arguments>
    struct function_traits<Return (This::*)(Arguments...) const>
    {
        using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
        using return_type = Return;
        using class_type = This;
    };

    template <typename This, typename Return, typename... Arguments>
    struct function_traits<Return (This::*)(Arguments...) const noexcept>
    {
        using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
        using return_type = Return;
        using class_type = This;
    };

    template <typename Return>
    struct function_traits<Return (*)()>
    {
        using parameters_type = void;
        using return_type = Return;
    };

    template <typename Return>
    struct function_traits<Return (*)() noexcept>
    {
        using parameters_type = void;
        using return_type = Return;
    };

    template <typename Return>
    struct function_traits<Return (&)()>
    {
        using parameters_type = void;
        using return_type = Return;
    };

    template <typename Return>
    struct function_traits<Return (&)() noexcept>
    {
        using parameters_type = void;
        using return_type = Return;
    };

    template <typename Return>
    struct function_traits<Return()>
    {
        using parameters_type = void;
        using return_type = Return;
    };

    template <typename Return>
    struct function_traits<Return() noexcept>
    {
        using parameters_type = void;
        using return_type = Return;
    };

    template <typename This, typename Return>
    struct function_traits<Return (This::*)()>
    {
        using parameters_type = void;
        using return_type = Return;
        using class_type = This;
    };

    template <typename This, typename Return>
    struct function_traits<Return (This::*)() noexcept>
    {
        using parameters_type = void;
        using return_type = Return;
        using class_type = This;
    };

    template <typename This, typename Return>
    struct function_traits<Return (This::*)() const>
    {
        using parameters_type = void;
        using return_type = Return;
        using class_type = This;
    };

    template <typename This, typename Return>
    struct function_traits<Return (This::*)() const noexcept>
    {
        using parameters_type = void;
        using return_type = Return;
        using class_type = This;
    };

    template <class Function>
    struct function_traits : function_traits<decltype(&Function::operator())>
    {
    };

    template <typename Function>
    using class_type_t = typename function_traits<std::remove_cvref_t<Function>>::class_type;

    template <typename Test, template <typename...> class Ref>
    struct is_specialization : std::false_type
    {
    };

    template <template <typename...> class Ref, typename... Args>
    struct is_specialization<Ref<Args...>, Ref> : std::true_type
    {
    };

    template <typename Test, template <typename...> class Ref>
    inline constexpr bool is_specialization_v = is_specialization<Test, Ref>::value;

    template <typename T>
    constexpr inline bool is_awaitable_v = is_specialization_v<std::remove_cvref_t<T>, boost::asio::awaitable>;

} // namespace httplib::util
