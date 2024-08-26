#pragma once
#include "detail/options.h"
#include "detail/system_defs.h"
#include "detail/system_global.h"
#include "detail/system_hierachy.h"
#include "detail/system_ranged.h"
#include "detail/system_sorted.h"
#include "detail/type_list.h"
#include "detail/decompose_func.h"
#include "detail/component_pools.h"
#include <tuple>

namespace ecs {

	// Given a list of decomposed functions, transform all their arguments into a
	// single type_list that contains all the arguments (without duplicates)
	template<typename... Ts>
	using xform_to_param_types = detail::reduce_lists<detail::merge_type_lists, typename Ts::param_types...>;

	// Given a list of arguments, like eg. 'int const&', transform each argument
	// its naked form ('int')
	template<typename... Ts>
	using xform_to_naked = detail::type_list<detail::naked_component_t<Ts>...>;

	// Given a list of naked arguments,
	// transform them into a tuple of component pools
	template<typename... Ts>
	using xform_to_pools = std::tuple<ecs::detail::component_pool<Ts>...>;

	// Holds all the systems to be used
	template <auto... Systems>
	class systems {
		using test = detail::decompose_all<Systems...>;
		using all_param_types = detail::transform_type_all<test, xform_to_param_types>;
		using all_naked_types = detail::transform_type_all<all_param_types, xform_to_naked>;
		using all_pools = detail::transform_type_all<all_naked_types, xform_to_pools>;

		// Holds all the component pools used by all the systems
		all_pools pools;

	public:
		void run(auto arg) {
			(Systems(arg), ...);
		}
	};
} // namespace ecs
