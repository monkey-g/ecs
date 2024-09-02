#pragma once
#include <type_traits>
#include "../entity_range.h"
#include "entity_range.h"

namespace ecs::detail {

// Manages arguments using ranges. Very fast linear traversal and minimal storage overhead.
template <typename ComponentsList>
class static_system_ranged final {
private:
	using argument = decltype(with_all_types<ComponentsList>([]<typename... Types>() {
		return std::tuple(entity_range{0, 0}, component_argument<Types>{}...);
	}));

public:
	constexpr static_system_ranged() = default;

	void run(auto& fn) {
		with_all_types<ComponentsList>([&]<typename... Ts>() {
			for (argument& arg : arguments) {
				entity_range const range = std::get<0>(arg);
				for (entity_id const ent : range) {
					auto const offset = ent - range.first();
					fn(get_arg<Ts>(arg, offset, 0)...);
				}
			}
		});
	}

	// Convert a set of entities into arguments that can be passed to the system
	void build(auto& ctx) {
		// TODO add/remove instead of full rebuild every time

		// Clear current arguments
		arguments.clear();

		with_all_types<ComponentsList>([&]<typename... Type>() {
			find_entity_pool_intersections_cb<ComponentsList>(ctx.get_pools(), [&](entity_range found_range) {
				arguments.emplace_back(found_range, ctx.template get_component<Type>(found_range.first())...);
			});
		});
	}

private:
	// Extracts a component argument from a pointer+offset
	template <typename Component>
	decltype(auto) get_arg(argument& arg, [[maybe_unused]] ptrdiff_t offset, [[maybe_unused]] auto pools = std::ptrdiff_t{0}) {
		using T = std::remove_cvref_t<Component>;

		if constexpr (std::same_as<T, entity_id>) {
			return std::get<entity_range>(arg).first() + offset;
		} else if constexpr (std::is_pointer_v<T>) {
			return static_cast<T>(nullptr);
		} else if constexpr (detail::unbound<T>) {
			T* ptr = std::get<T>(arg);
			return *ptr;
		} else if constexpr (detail::is_parent<T>::value) {
			parent_id const pid = *(std::get<parent_id*>(arg) + offset);

			// TODO store this in separate container in system_hierarchy? might not be
			//      needed after O(1) pool lookup implementation
			return for_all_types<parent_type_list_t<T>>([&]<typename... ParentTypes>() {
				return T{pid, get_component<ParentTypes>(pid, pools)...};
			});
		} else {
			T* ptr = std::get<T*>(arg);
			return *(ptr + offset);
		}
	}

	std::vector<argument> arguments;
};

} // namespace ecs::detail
