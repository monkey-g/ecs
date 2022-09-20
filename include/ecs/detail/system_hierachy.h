#ifndef ECS_SYSTEM_HIERARCHY_H_
#define ECS_SYSTEM_HIERARCHY_H_

#include <map>
#include <unordered_map>

#include "tls/collect.h"

#include "../parent.h"
#include "entity_offset.h"
#include "entity_range_iterator.h"
#include "find_entity_pool_intersections.h"
#include "pool_entity_walker.h"
#include "system.h"
#include "system_defs.h"
#include "type_list.h"

namespace ecs::detail {
template <class Options, class UpdateFn, class TupPools, bool FirstIsEntity, class ComponentsList>
class system_hierarchy final : public system<Options, UpdateFn, TupPools, FirstIsEntity, ComponentsList> {
	using base = system<Options, UpdateFn, TupPools, FirstIsEntity, ComponentsList>;

	// Determine the execution policy from the options (or lack thereof)
	using execution_policy = std::conditional_t<ecs::detail::has_option<opts::not_parallel, Options>(), std::execution::sequenced_policy,
												std::execution::parallel_policy>;

	struct entity_info {
		int parent_count = 0;
		entity_type root_id;
	};

	using info_map = std::unordered_map<entity_type, entity_info>;
	using info_iterator = typename info_map::const_iterator;

	template <typename... Types>
	using tuple_from_types = std::tuple<entity_id, component_argument<Types>..., entity_info>;
	using argument = transform_type_all<ComponentsList, tuple_from_types>;

public:
	system_hierarchy(UpdateFn func, TupPools in_pools)
		: base{func, in_pools}, parent_pools{make_parent_types_tuple()} {
		pool_parent_id = &detail::get_pool<parent_id>(this->pools);
		this->process_changes(true);
	}

private:
	void do_run() override {
		auto const e_p = execution_policy{}; // cannot pass directly to 'for_each' in gcc
		std::for_each(e_p, argument_spans.begin(), argument_spans.end(), [this](auto const local_span) {
			for (argument& arg : local_span) {
				apply_type<ComponentsList>([&]<typename... Types>(){
					if constexpr (FirstIsEntity) {
						this->update_func(std::get<entity_id>(arg), extract<Types>(arg)...);
					} else {
						this->update_func(/*                     */ extract<Types>(arg)...);
					}
					});
			}
		});
	}

	// Convert a set of entities into arguments that can be passed to the system
	void do_build(/* entity_range_view ranges*/) override {
		std::vector<entity_range> ranges;
		std::vector<entity_range> ents_to_remove;

		// Find the entities
		find_entity_pool_intersections_cb<ComponentsList>(this->pools, [&](entity_range range) {
			ranges.push_back(range);

			// Get the parent ids in the range
			parent_id const* pid_ptr = pool_parent_id->find_component_data(range.first());

			// the ranges to remove
			for (entity_id const ent : range) {
				parent_id const pid = pid_ptr[range.offset(ent)];

				// Does tests on the parent sub-components to see they satisfy the constraints
				// ie. a 'parent<int*, float>' will return false if the parent does not have a float or
				// has an int.
				for_each_type<parent_component_list>([&pid, this, ent, &ents_to_remove]<typename T>() {
					// Get the pool of the parent sub-component
					auto const& sub_pool = detail::get_pool<T>(this->pools);

					if constexpr (std::is_pointer_v<T>) {
						// The type is a filter, so the parent is _not_ allowed to have this component
						if (sub_pool.has_entity(pid)) {
							merge_or_add(ents_to_remove, entity_range{ent, ent});
						}
					} else {
						// The parent must have this component
						if (!sub_pool.has_entity(pid)) {
							merge_or_add(ents_to_remove, entity_range{ent, ent});
						}
					}
				});
			}
		});

		// Remove entities from the result
		ranges = difference_ranges(ranges, ents_to_remove);

		// Clear the arguments
		arguments.clear();
		argument_spans.clear();

		if (ranges.empty()) {
			return;
		}

		// Count the number of arguments to be constructed
		size_t count = 0;
		for (auto const& range : ranges)
			count += range.ucount();

		arguments.resize(count, apply_type<ComponentsList>([]<typename... Types>() {
			return argument{entity_id{0}, component_argument<Types>{0}..., entity_info{}};
		}));

		// TODO insert in set with top. ordering?

		// map of entity and root info
		std::unordered_map<entity_type, int> roots;
		roots.reserve(count);

		// Build the arguments for the ranges
		std::atomic<int> index = 0;
		auto conv = entity_offset_conv{ranges};
		pool_entity_walker<TupPools> walker;
		info_map info;
		info.reserve(count);

		for (entity_range const& range : ranges) {
			walker.reset(&this->pools, entity_range_view{{range}});
			while (!walker.done()) {
				entity_id const entity = walker.get_entity();

				info_iterator const ent_info = fill_entity_info(info, entity, index);

				// Add the argument for the entity
				auto const ent_offset = static_cast<size_t>(conv.to_offset(entity));

				apply_type<ComponentsList>([&]<typename... Types>() {
					arguments[ent_offset] = argument(entity, walker.template get<Types>()..., ent_info->second);
				});

				// Update the root child count
				auto const root_index = ent_info->second.root_id;
				roots[root_index] += 1;

				walker.next();
			}
		}

		// Do the topological sort of the arguments
		std::sort(std::execution::par, arguments.begin(), arguments.end(), topological_sort_func);

		// Create the argument spans
		size_t offset = 0;
		argument_spans.reserve(roots.size());
		for (auto const& [id, child_count] : roots) {
			argument_spans.emplace_back(arguments.data() + offset, child_count);
			offset += child_count;
		}
	}

	decltype(auto) make_parent_types_tuple() const {
		return apply_type<parent_component_list>([this]<typename... T>() {
			return std::make_tuple(&get_pool<std::remove_pointer_t<T>>(this->pools)...);
		});
	}

	// Extracts a component argument from a tuple
	template <typename Component, typename Tuple>
	static decltype(auto) extract(Tuple& tuple) {
		using T = std::remove_cvref_t<Component>;

		if constexpr (std::is_pointer_v<T>) {
			return nullptr;
		} else if constexpr (detail::is_parent<T>::value) {
			return std::get<T>(tuple);
		} else {
			T* ptr = std::get<T*>(tuple);
			return *(ptr);
		}
	}

	static bool topological_sort_func(argument const& arg_l, argument const& arg_r) {
		auto const& [depth_l, root_l] = std::get<entity_info>(arg_l);
		auto const& [depth_r, root_r] = std::get<entity_info>(arg_r);

		// order by roots
		if (root_l != root_r)
			return root_l < root_r;
		else
			// order by depth
			return depth_l < depth_r;
	}

	info_iterator fill_entity_info(info_map& info, entity_id const entity, std::atomic<int>& index) const {
		// Get the parent id
		entity_id const* parent_id = pool_parent_id->find_component_data(entity);
		if (parent_id == nullptr) {
			// This entity does not have a 'parent_id' component,
			// which means that this entity is a root
			auto const [it, _] = info.emplace(std::make_pair(entity, entity_info{0, index++}));
			return it;
		}

		// look up the parent info
		info_iterator parent_it = info.find(*parent_id);
		if (parent_it == info.end())
			parent_it = fill_entity_info(info, *parent_id, index);

		// insert the entity info
		auto const& [count, root_index] = parent_it->second;
		auto const [it, _p] = info.emplace(std::make_pair(entity, entity_info{1 + count, root_index}));
		return it;
	}

private:
	using base::has_parent_types;
	using typename base::full_parent_type;
	using typename base::parent_component_list;
	using typename base::stripped_component_list;
	using typename base::stripped_parent_type;

	// Ensure we have a parent type
	static_assert(has_parent_types, "no parent component found");

	// The vector of unrolled arguments
	std::vector<argument> arguments;

	// The spans over each tree in the argument vector
	std::vector<std::span<argument>> argument_spans;

	// The pool that holds 'parent_id's
	component_pool<parent_id> const* pool_parent_id;

	// A tuple of the fully typed component pools used the parent component
	parent_pool_tuple_t<stripped_parent_type> const parent_pools;
};
} // namespace ecs::detail

#endif // !ECS_SYSTEM_HIERARCHY_H_
