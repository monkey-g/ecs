#pragma once
#include <tuple>
#include <bitset>
#include "decompose_func.h"
#include "options.h"
#include "parent_id.h"
#include "system_defs.h"
#include "type_list.h"
#include "static_system_ranged.h"
#include "compile_time_value.h"

namespace ecs::detail {
	// Holds all the systems to be used
	template <auto... Systems> // TODO concept
		requires(sizeof...(Systems) > 0)
	class static_context {
		template<auto System>
		struct system_wrap {
			static constexpr auto sys = System;
		};

		// Given a list of decomposed systems, transform them into a
		// list that contains the list of arguments for each system
		template <typename... Ts>
		using xform_to_param_types = type_list<typename Ts::param_types...>;

		// Given a list of decomposed systems, transform them into a
		// list that contains the list of naked arguments for each system
		template <typename... Ts>
		using xform_to_naked_types = type_list<typename Ts::naked_types...>;

		// Given a list of system argument lists, flatten them into a
		// single type_list that contains all the naked arguments (without duplicates)
		template <typename... Ts>
		using xform_flatten_param_types = reduce_lists<merge_type_lists, Ts...>;

		// Transform a list of naked arguments into a tuple of component pools
		template <typename... Ts>
		using xform_to_pools = std::tuple<component_pool<Ts>...>;

		using systems_info = decompose_all<Systems...>;
		using all_wrapped_systems = type_list<system_wrap<Systems>...>;
		using all_param_types = transform_type_all<systems_info, xform_to_param_types>;
		using all_naked_types = transform_type_all<systems_info, xform_to_naked_types>;
		using flattened_naked_types = transform_type_all<all_naked_types, xform_flatten_param_types>;
		using all_pools = transform_type_all<flattened_naked_types, xform_to_pools>;

		// Transform a list of system info into a tuple of internal systems
		template <typename... SystemInfos>
		using xform_to_systems = std::tuple<static_system_ranged<typename SystemInfos::param_types>...>;
		using all_systems = transform_type_all<systems_info, xform_to_systems>;

		// A pipeline of system indices
		template<int N>
		using pipeline = std::array<int, N>;

		// Bitset of visited types
		using bs_pipeline = std::bitset<type_list_size<systems_info>>;

	public:
		constexpr void build() {
			//auto d = build_dependency_matrix();
			//auto p = build_pipelines();

			auto const process_pools = [](auto&... ps) { (ps.process_changes(), ...); };
			auto const build_systems = [&](auto& ...sys) { (sys.build(*this), ...); };

			std::apply(process_pools, pools);
			std::apply(build_systems, systems);
		}

		constexpr void run() {
			for_each_type<all_wrapped_systems>([&]<typename T>() {
				constexpr auto index = index_of<T, all_wrapped_systems>();
				std::get<index>(systems).run(T::sys);
			});
		}

		constexpr all_pools& get_pools() {
			return pools;
		}

		// Add several components to a range of entities. Will not be added until 'commit_changes()' is called.
		// Pre: entity does not already have the component, or have it in queue to be added
		template <typename... T>
		constexpr void add_component(entity_range const range, T&&... vals) {
			static_assert(detail::is_unique_type_args<T...>(), "the same component type was specified more than once");
			static_assert((!detail::global<T> && ...), "can not add global components to entities");
			static_assert((!std::is_pointer_v<std::remove_cvref_t<T>> && ...), "can not add pointers to entities; wrap them in a struct");
			static_assert((!detail::is_variant_of_pack<T...>()), "Can not add more than one component from the same variant");

			auto const adder = []<typename Type>(auto& in_pools, entity_range const range, Type&& val) {
				// Add it to the component pool
				if constexpr (detail::is_parent<Type>::value) {
					auto& pool = std::get<component_pool<parent_id>>(in_pools);
					Pre(!pool.has_entity(range), "one- or more entities in the range already has this type");
					pool.add(range, detail::parent_id{val.id()});
				} else if constexpr (std::is_reference_v<Type>) {
					using DerefT = std::remove_cvref_t<Type>;
					static_assert(std::copyable<DerefT>, "Type must be copyable");

					auto& pool = std::get<component_pool<DerefT>>(in_pools);
					Pre(!pool.has_entity(range), "one- or more entities in the range already has this type");
					pool.add(range, val);
				} else {
					static_assert(std::copyable<Type>, "Type must be copyable");

					auto& pool = std::get<component_pool<Type>>(in_pools);
					Pre(!pool.has_entity(range), "one- or more entities in the range already has this type");
					pool.add(range, std::forward<Type>(val));
				}
			};

			(adder(pools, range, std::forward<T>(vals)), ...);
		}

		// Get an entities component from a component pool
		template <typename Component>
		[[nodiscard]] auto get_component(entity_id const entity) {
			using T = std::remove_cvref_t<Component>;

			if constexpr (std::is_pointer_v<T>) {
				// Filter: return a nullptr
				static_cast<void>(entity);
				return static_cast<T*>(nullptr);
			} else if constexpr (tagged<T>) {
				// Tag: return a pointer to some dummy storage
				thread_local char dummy_arr[sizeof(T)];
				return reinterpret_cast<T*>(dummy_arr);
			} else if constexpr (global<T>) {
				// Global: return the shared component
				return &std::get<component_pool<T>>(pools).get_shared_component();
			} else if constexpr (std::is_same_v<reduce_parent_t<T>, parent_id>) {
				return std::get<component_pool<parent_id>>(pools).find_component_data(entity);
			} else {
				// Standard: return the component from the pool
				return std::get<component_pool<T>>(pools).find_component_data(entity);
			}
		}

	private:
		// Returns true if the system component can be written to
		template <int SystemIndex, int ComponentIndex>
		static consteval bool writes_to_component() {
			using system_params = typename type_at<SystemIndex, systems_info>::param_types;
			using component = type_at<ComponentIndex, system_params>;

			// TODO parent types

			return !is_read_only<component>();
		}

		// Returns a tuple of array<int>'s, one for each system.
		// The arrays hold the index to the previous system that used the type
		static constexpr auto build_dependency_matrix() {
			constexpr auto num_params = type_list_size<flattened_naked_types>;
			std::array<int, num_params> last_index;
			last_index.fill(-1);

			// Iterate over all the systems, getting each parameter list
			return with_all_types<all_naked_types>([&last_index, index = 0]<typename... ParamLists>() mutable {
				// Iterate over the systems parameters
				return std::tuple{
					with_all_types<ParamLists>([&last_index, I = index++]<typename... Params>() {
						// Get the last index where the types were used
						auto arr = std::array{last_index[index_of<Params, flattened_naked_types>()]...};

						// Update the indices of the parameter types to the current index
						((last_index[index_of<Params, flattened_naked_types>()] = I), ...);

						return arr;
					})...
				};
			});
		}

		template <int I>
		static constexpr auto find_pipeline() -> bs_pipeline {
			if constexpr (I == -1)
				return {0};
			else {
				constexpr auto arr_I = std::get<I>(dependency_matrix);

				auto const this_pipe = bs_pipeline{1ull << I};
				auto const dependent_pipes = ct_iseq<arr_I.size()>([&](auto... Is) {
					return (... | (find_pipeline<arr_I[Is]>()));
				});

				return this_pipe | dependent_pipes;
			}
		}

		static constexpr auto build_pipelines_vec() {
			int const num_deps = std::tuple_size_v<decltype(dependency_matrix)>;

			std::vector<bs_pipeline> pipelines;

			ct_loop(ct<0>, ct<num_deps>, [&](auto I) {
				pipelines.push_back(find_pipeline<I>());
			});

			// Merge overlapping pipes
			size_t index = 0;
			while (index < pipelines.size()) {
				for (size_t next = index + 1; next < pipelines.size();) {
					if ((pipelines[index] & pipelines[next]).any()) {
						pipelines[index] |= pipelines[next];
						pipelines[next] = pipelines.back();
						pipelines.pop_back();
					} else {
						next += 1;
					}
				}

				index += 1;
			}

			return pipelines;
		}

		static constexpr auto build_pipelines() {
			std::array<bs_pipeline, build_pipelines_vec().size()> arr_pipelines;
			std::ranges::copy(build_pipelines_vec(), arr_pipelines.begin());
			return arr_pipelines;
		}
	private:
		// Holds all the component pools used by all the systems
		all_pools pools;

		// Holds all the systems
		all_systems systems;

		static constexpr auto dependency_matrix = build_dependency_matrix();

		static constexpr auto pipelines = build_pipelines();
	};
} // namespace ecs
