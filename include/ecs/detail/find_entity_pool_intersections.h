#ifndef ECS_FIND_ENTITY_POOL_INTERSECTIONS_H
#define ECS_FIND_ENTITY_POOL_INTERSECTIONS_H

#include "entity_range.h"
#include "system_defs.h"
#include "component_pools.h"
#include <array>
#include <vector>
#include <algorithm>

namespace ecs::detail {

// Given a list of components, return an array containing the corresponding component pools
template <typename ComponentsList>
	requires(std::is_same_v<ComponentsList, transform_type<ComponentsList, naked_component_t>>)
auto get_pool_iterators([[maybe_unused]] auto const& pools) {
	if constexpr (type_list_is_empty<ComponentsList>) {
		return std::array<stride_view<0, char const>, 0>{};
	} else {
		// Verify that the component list passed has a corresponding pool
		//for_each_type<ComponentsList>([]<typename T>() {
		//	static_assert(contains_type<T, PoolsList>(), "A component is missing its corresponding component pool");
		//});

		return with_all_types<ComponentsList>([&]<typename... Components>() {
			return std::to_array({std::get<component_pool<Components>>(pools).get_entities()...});
		});
	}
}


// Find the intersection of the sets of entities in the specified pools
template <typename InputList, typename F>
void find_entity_pool_intersections_cb(auto const& pools, F&& callback) {
	static_assert(not type_list_is_empty<InputList>, "Empty component list supplied");

	// Split the type_list into filters and non-filters (regular components).
	using FilterComponentPairList = split_types_if<InputList, std::is_pointer>;
	using FilterList = transform_type<typename FilterComponentPairList::first, naked_component_t>;
	using ComponentList = typename FilterComponentPairList::second;

	// Filter local components.
	// Global components are available for all entities
	// so don't bother wasting cycles on testing them.
	using LocalComponentList = transform_type<filter_types_if<ComponentList, detail::is_local>, naked_component_t>;
	auto iter_components = get_pool_iterators<LocalComponentList>(pools);

	// If any of the pools are empty, bail
	bool const any_emtpy_pools = std::ranges::any_of(iter_components, [](auto p) {
		return p.current() == nullptr;
	});
	if (any_emtpy_pools)
		return;

	// Get the iterators for the filters
	auto iter_filters = get_pool_iterators<FilterList>(pools);

	// Sort the filters
	std::sort(iter_filters.begin(), iter_filters.end(), [](auto const& a, auto const& b) {
		if (a.current() && b.current())
			return *a.current() < *b.current();
		else
			return nullptr != a.current();
	});

	// helper lambda to test if an iterator has reached its end
	auto const done = [](auto const& it) {
		return it.done();
	};

	while (!std::any_of(iter_components.begin(), iter_components.end(), done)) {
		// Get the starting range to test other ranges against
		entity_range curr_range = *iter_components[0].current();

		// Find all intersections
		if constexpr (type_list_size<LocalComponentList> == 1) {
			iter_components[0].next();
		} else {
			bool intersection_found = false;
			for (size_t i = 1; i < iter_components.size(); ++i) {
				auto& it_a = iter_components[i - 1];
				auto& it_b = iter_components[i];

				if (curr_range.overlaps(*it_b.current())) {
					curr_range = entity_range::intersect(curr_range, *it_b.current());
					intersection_found = true;
				}

				if (it_a.current()->last() < it_b.current()->last()) {
					// range a is inside range b, move to
					// the next range in a
					it_a.next();
					if (it_a.done())
						break;

				} else if (it_b.current()->last() < it_a.current()->last()) {
					// range b is inside range a,
					// move to the next range in b
					it_b.next();
				} else {
					// ranges are equal, move to next ones
					it_a.next();
					if (it_a.done())
						break;

					it_b.next();
				}
			}

			if (!intersection_found)
				continue;
		}

		// Filter the range, if needed
		if constexpr (type_list_size<FilterList> > 0) {
			bool completely_filtered = false;
			for (auto& it : iter_filters) {
				while(!done(it)) {
					if (it.current()->contains(curr_range)) {
						// 'curr_range' is contained entirely in filter range,
						// which means that it will not be sent to the callback
						completely_filtered = true;
						break;
					} else if (curr_range < *it.current()) {
						// The whole 'curr_range' is before the filter, so don't touch it
						break;
					} else if (*it.current() < curr_range) {
						// The filter precedes the range, so advance it and restart
						it.next();
						continue;
					} else {
						// The two ranges overlap
						auto const res = entity_range::remove(curr_range, *it.current());

						if (res.second) {
							// 'curr_range' was split in two by the filter.
							// Send the first range and update
							// 'curr_range' to be the second range
							callback(res.first);
							curr_range = *res.second;

							it.next();
						} else {
							// The result is an endpiece, so update the current range.
							curr_range = res.first;

							// The next filter might remove more from 'curr_range', so don't just
							// skip past it without checking
							if (curr_range.first() >= it.current()->first()) {
								it.next();
							}
						}
					}
				}
			}

			if (!completely_filtered)
				callback(curr_range);
		} else {
			// No filters on this range, so send
			callback(curr_range);
		}
	}
}

} // namespace ecs::detail

#endif // !ECS_FIND_ENTITY_POOL_INTERSECTIONS_H
