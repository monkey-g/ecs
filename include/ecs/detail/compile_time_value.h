#pragma once
#include <type_traits>
#include <concepts>

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

	static_assert(ct<42> == 42);
	static_assert(*ct<42> == 42);
	static_assert(ct<42>() == 42);

	template <typename CT>
	concept ct_value = requires { typename CT::compile_time_value_type; };

	template <typename CT, typename T>
	concept ct_value_as = ct_value<CT> && std::is_same_v<typename CT::compile_time_value_type, T>;

	// TODO ct_for, ct_while, etc...
	constexpr void ct_loop(ct_value auto from, ct_value auto to, auto&& fn) {
		if constexpr (*from < *to) {
			fn(from);
			ct_loop(ct<from + 1>, to, fn);
		} else if constexpr (*from > *to) {
			fn(from);
			ct_loop(ct<from - 1>, to, fn);
		} else {
		}
	}

	consteval auto ct_loop(ct_value auto to, auto&& fn) {
		return ct_loop(ct<0>, to, fn);
	}

	consteval void ct_while(ct_value_as<bool> auto b, auto&& fn) {
		if constexpr (b) {
			ct_while(fn(), fn);
		}
	}

	template<std::integral auto Count, typename Fn>
	consteval auto ct_iseq(Fn&& fn) {
		using I = decltype(Count);
		return [fn = std::forward<Fn>(fn)]<I... Is>(std::integer_sequence<I, Is...>) {
			return fn(ct<Is>...);
		}
		(std::make_integer_sequence<I, Count>{});
	}
} // namespace ecs::detail
