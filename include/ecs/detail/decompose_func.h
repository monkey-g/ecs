#pragma once
#include "type_list.h"
#include "verification.h"

namespace ecs::detail {
	template <typename R, typename... Args>
	struct decomposed {
		using return_type = R;
		using param_types = type_list<Args...>;
		using naked_types = type_list<naked_component_t<Args>...>;

		template <typename IR, typename... IArgs>
		consteval decomposed(IR(IArgs...)) {}
		template <typename IR, typename IC, typename... IArgs>
		consteval decomposed(IR (IC::*)(IArgs...) const) {}
	};
	template <typename R, typename... Args>
	decomposed(R(Args...)) -> decomposed<R, Args...>;
	template <typename R, typename C, typename... Args>
	decomposed(R (C::*)(Args...) const) -> decomposed<R, Args...>;

	template <type_is_lambda F>
	consteval auto decomposer(F) {
		return decomposed(&F::operator());
	}
	consteval auto decomposer(type_is_function auto f) {
		return decomposed(f);
	}

	template<auto T>
	using decompose = decltype(decomposer(T));

	template<auto... Ts>
	using decompose_all = type_list<decompose<Ts>...>;

} // namespace ecs::detail