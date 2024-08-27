#ifndef ECS_DETAIL_SYSTEM_DEFS_H
#define ECS_DETAIL_SYSTEM_DEFS_H

// Contains definitions that are used by the systems classes
#include "component_pool.h"
#include "parent_id.h"
#include "type_list.h"

namespace ecs::detail {
template <typename T>
constexpr static bool is_entity = std::is_same_v<std::remove_cvref_t<T>, entity_id>;

// If given a parent, convert to detail::parent_id, otherwise do nothing
template <typename T>
using reduce_parent_t =
	std::conditional_t<std::is_pointer_v<T>,
		std::conditional_t<is_parent<std::remove_pointer_t<T>>::value, parent_id*, T>,
		std::conditional_t<is_parent<T>::value, parent_id, T>>;

// Given a component type, return the naked type without any modifiers.
// Also converts ecs::parent into ecs::detail::parent_id.
template <typename T>
using naked_component_t = std::remove_pointer_t<std::remove_cvref_t<reduce_parent_t<T>>>;

// Ensure that the passed type is naked, ie, it's
// an 'int' and not an 'int*' or 'int const&'.
template <typename T>
concept naked_component = std::same_as<T, naked_component_t<T>>;

// Alias for stored pools
template <typename T>
using pool = component_pool<naked_component_t<T>>* const;

// Returns true if a type is read-only
template <typename T>
constexpr bool is_read_only() {
	return detail::immutable<T> || detail::tagged<T> || std::is_const_v<std::remove_reference_t<T>>;
}

// Helper to extract the parent types
template <typename T>
struct parent_type_list; // primary template

template <>
struct parent_type_list<void> {
	using type = void;
}; // partial specialization for void

template <template <typename...> typename Parent, typename... ParentComponents> // partial specialization
struct parent_type_list<Parent<ParentComponents...>> {
	using type = type_list<ParentComponents...>;
};
template <typename T>
using parent_type_list_t = typename parent_type_list<std::remove_cvref_t<T>>::type;

// Helper to extract the parent pool types
template <typename T>
struct parent_pool_detect; // primary template

template <template <typename...> typename Parent, typename... ParentComponents> // partial specialization
struct parent_pool_detect<Parent<ParentComponents...>> {
	static_assert(!(is_parent<ParentComponents>::value || ...), "parents in parents not supported");
	using type = std::tuple<pool<ParentComponents>...>;
};
template <typename T>
using parent_pool_tuple_t = typename parent_pool_detect<T>::type;


// The type of a single component argument
template <typename Component>
using component_argument = std::conditional_t<is_parent<std::remove_cvref_t<Component>>::value,
											  std::remove_cvref_t<reduce_parent_t<Component>>*,	// parent components are stored as copies
											  std::remove_cvref_t<Component>*>; // rest are pointers
} // namespace ecs::detail

#endif // !ECS_DETAIL_SYSTEM_DEFS_H
