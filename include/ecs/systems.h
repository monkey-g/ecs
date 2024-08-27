#pragma once
#include "detail/component_pools.h"
#include "detail/decompose_func.h"
#include "detail/options.h"
#include "detail/parent_id.h"
#include "detail/system_defs.h"
#include "detail/system_global.h"
#include "detail/system_hierachy.h"
#include "detail/system_ranged.h"
#include "detail/system_sorted.h"
#include "detail/type_list.h"
#include "entity_id.h"
#include <tuple>

namespace ecs {

	// Holds all the systems to be used
	template <auto... Systems> // TODO concept
	class systems {
		// Given a list of decomposed systems, transform all their arguments into a
		// single type_list that contains all the naked arguments (without duplicates)
		template <typename... Ts>
		using xform_to_param_types = detail::type_list<typename Ts::naked_types...>;

		// Given a list of decomposed systems, transform all their arguments into a
		// single type_list that contains all the naked arguments (without duplicates)
		template <typename... Ts>
		using xform_flatten_param_types = detail::reduce_lists<detail::merge_type_lists, Ts...>;

		// Given a list of naked arguments,
		// transform them into a tuple of component pools
		template <typename... Ts>
		using xform_to_pools = std::tuple<detail::component_pool<Ts>...>;

		using test = detail::decompose_all<Systems...>;
		using all_param_types = detail::transform_type_all<test, xform_to_param_types>;
		using flattened_param_types = detail::transform_type_all<all_param_types, xform_flatten_param_types>;
		using all_pools = detail::transform_type_all<flattened_param_types, xform_to_pools>;

		// Holds all the component pools used by all the systems
		all_pools pools;

	public:
		void run(auto arg) {
			auto test = create_dependency_graph();
			(Systems(arg), ...);
		}

	private:
		static auto create_dependency_graph() {
			constexpr auto num_params = detail::type_list_size<flattened_param_types>;
			std::array<int, num_params> last_index{-1};

			// Iterate over all the parameter lists for each system
			auto const ret = detail::for_all_types<all_param_types>([&last_index, index = 0]<typename... ParamLists>() mutable {
				// Iterate over a systems parameters
				return std::tuple{
					detail::for_all_types<ParamLists>([&last_index, I = index++]<typename... Params>() {
						// Get the last index the types were used
						auto arr = std::array{last_index[detail::index_of<Params, flattened_param_types>()]...};

						// Update the indices of the parameter types to the current index
						((last_index[detail::index_of<Params, flattened_param_types>()] = I), ...);

						return arr;
					})...
				};
			});

			return ret;
		}

		// Get an entities component from a component pool
		template <typename Component>
		[[nodiscard]] auto get_component(entity_id const entity) {
			using T = std::remove_cvref_t<Component>;

			if constexpr (std::is_pointer_v<T>) {
				// Filter: return a nullptr
				static_cast<void>(entity);
				return static_cast<T*>(nullptr);
			} else if constexpr (detail::tagged<T>) {
				// Tag: return a pointer to some dummy storage
				thread_local char dummy_arr[sizeof(T)];
				return reinterpret_cast<T*>(dummy_arr);
			} else if constexpr (detail::global<T>) {
				// Global: return the shared component
				return &std::get<T>(pools).get_shared_component();
			} else if constexpr (std::is_same_v<detail::reduce_parent_t<T>, detail::parent_id>) {
				return std::get<detail::parent_id>(pools).find_component_data(entity);
			} else {
				// Standard: return the component from the pool
				return std::get<T>(pools).find_component_data(entity);
			}
		}
	};
} // namespace ecs
