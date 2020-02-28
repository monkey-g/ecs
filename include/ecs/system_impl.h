#pragma once
#include <utility>
#include <algorithm>
#include <type_traits>
#include <gsl/gsl>

#include "system.h"
#include "component_pool.h"

namespace ecs {
	class entity;
}

namespace ecs::detail
{
	// The implementation of a system specialized on its components
	template <int Group, class ExecutionPolicy, typename UserUpdateFunc, class FirstComponent, class ...Components>
	class system_impl final : public system
	{
		// Determines if the first component is an entity
		static constexpr bool is_first_arg_entity = std::is_same_v<FirstComponent, entity_id> || std::is_same_v<FirstComponent, entity>;

		// Calculate the number of components
		static constexpr size_t num_components = sizeof...(Components) + (is_first_arg_entity ? 0 : 1);

		// The first type in the system, entity or component
		using first_type = std::conditional_t<is_first_arg_entity, FirstComponent, FirstComponent*>;

		// Alias for stored pools
		template <class T>
		using pool = gsl::not_null<component_pool<T>*> const;

		// Tuple holding all pools used by this system
		using tup_pools = std::conditional_t<is_first_arg_entity,
			std::tuple<                      pool<Components>...>,
			std::tuple<pool<FirstComponent>, pool<Components>...>>;

		// Holds an entity range and a pointer to the first component from each pool in that range
		using range_arguments = std::conditional_t<is_first_arg_entity,
			std::tuple<entity_range,                  Components* ...>,
			std::tuple<entity_range, FirstComponent*, Components* ...>>;

		// Holds the arguments for a range of entities
		std::vector<range_arguments> arguments;

		// A tuple of the fully typed component pools used by this system
		tup_pools const pools;

		// The user supplied system
		UserUpdateFunc update_func;

	public:
		// Constructor for when the first argument to the system is _not_ an entity
		system_impl(UserUpdateFunc update_func, pool<FirstComponent> first_pool, pool<Components>... pools)
			: pools{ first_pool, pools... }
			, update_func{ update_func }
		{
			build_args();
		}

		// Constructor for when the first argument to the system _is_ an entity
		system_impl(UserUpdateFunc update_func, pool<Components> ... pools)
			: pools{ pools... }
			, update_func{ update_func }
		{
			build_args();
		}

		void update() override
		{
			if (!is_enabled()) {
				return;
			}

			// Call the system for all pairs of components that match the system signature
			for (auto const& argument : arguments) {
				auto const& range = std::get<entity_range>(argument);
				std::for_each(ExecutionPolicy{}, range.begin(), range.end(), [this, &argument, first_id = range.first()](ecs::entity_id ent) {
					// Small helper function
					auto const extract_arg = [](auto ptr, /*[[maybe_unused]]*/ ptrdiff_t offset) {
						using T = std::remove_cv_t<std::remove_reference_t<decltype(*ptr)>>;
						if constexpr (!detail::Shared<T>) {
							GSL_SUPPRESS(bounds.1) // this access is checked in the loop
							return ptr + offset;
						}
						else {
							static_cast<void>(offset); // silence unused parameter warning
							return ptr;
						}
					};


					auto const offset = ent - first_id;

					if constexpr (is_first_arg_entity) {
						update_func(ent,
									*extract_arg(std::get<Components*>(argument), offset)...);
					}
					else {
						update_func(*extract_arg(std::get<FirstComponent*>(argument), offset),
									*extract_arg(std::get<Components*>(argument), offset)...);
					}
				});
			}
		}

		[[nodiscard]] int get_group() const noexcept override {
			return Group;
		}

	private:
		// Handle changes when the component pools change
		void process_changes(bool force_rebuild) override
		{
			if (force_rebuild) {
				build_args();
				return;
			}

			if (!is_enabled()) {
				return;
			}

			auto constexpr is_pools_modified = [](auto ...pools) { return (pools->is_data_modified() || ...); };
			bool const is_modified = std::apply(is_pools_modified, pools);
	
			if (is_modified) {
				build_args();
			}
		}

		void build_args()
		{
			entity_range_view const entities = std::get<0>(pools)->get_entities();

			if constexpr (num_components == 1)
			{
				// Build the arguments
				build_args(entities);
			}
			else
			{
				// When there are more than one component required for a system,
				// find the intersection of the sets of entities that have those components

				auto constexpr do_intersection = [](entity_range_view initial, entity_range_view first, auto ...rest) {
					// Intersects two ranges of entities
					auto constexpr intersector = [](entity_range_view view_a, entity_range_view view_b) {
						std::vector<entity_range> result;

						if (view_a.empty() || view_b.empty()) {
							return result;
						}

						auto it_a = view_a.cbegin();
						auto it_b = view_b.cbegin();

						while (it_a != view_a.cend() && it_b != view_b.cend()) {
							if (it_a->overlaps(*it_b)) {
								result.push_back(entity_range::intersect(*it_a, *it_b));
							}

							if (it_a->last() < it_b->last()) { // range a is inside range b, move to the next range in a
								++it_a;
							}
							else if (it_b->last() < it_a->last()) { // range b is inside range a, move to the next range in b
								++it_b;
							}
							else { // ranges are equal, move to next ones
								++it_a;
								++it_b;
							}
						}

						return result;
					};

					std::vector<entity_range> intersect = intersector(initial, first);
					((intersect = intersector(intersect, rest)), ...);
					return intersect;
				};

				// Build the arguments
				auto const intersect = do_intersection(entities, get_pool<Components>().get_entities()...);
				build_args(intersect);
			}
		}

		// Convert a set of entities into arguments that can be passed to the system
		void build_args(entity_range_view entities)
		{
			// Build the arguments for the ranges
			arguments.clear();
			for (auto const& range : entities) {
				if constexpr (is_first_arg_entity) {
					arguments.emplace_back(range, get_component<Components>(range.first())...);
				}
				else {
					arguments.emplace_back(range, get_component<FirstComponent>(range.first()), get_component<Components>(range.first())...);
				}
			}
		}

		template <typename Component>
		[[nodiscard]] component_pool<Component>& get_pool() const
		{
			return *std::get<pool<Component>>(pools);
		}

		template <typename Component>
		[[nodiscard]] Component* get_component(entity_id const entity)
		{
			return get_pool<Component>().find_component_data(entity);
		}
	};
}
