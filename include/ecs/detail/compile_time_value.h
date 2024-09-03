#pragma once
#include <type_traits>

// https://godbolt.org/z/n6xMrGfvs
namespace ecs::detail {
	template <auto V>
	struct ct_t {
		using compile_time_value_type = decltype(V); // For concepts

		consteval operator auto() const { return V; }
		consteval auto operator()() const { return V; }
		consteval auto operator->() const requires(!std::is_fundamental_v<decltype(V)>) { return &V; }
		consteval auto operator*() const { return V; }
	};

	template <auto V>
	static constexpr auto ct = ct_t<V>{};


	template <typename CT>
	concept ct_value = requires { typename CT::compile_time_value_type; };

	template <typename CT, typename T>
	concept ct_value_as = ct_value<CT> && std::is_same_v<typename CT::compile_time_value_type, T>;
} // namespace ecs::detail

// TODO ct_for, ct_while, etc...
