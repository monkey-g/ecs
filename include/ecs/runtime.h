#ifndef ECS_RUNTIME_H
#define ECS_RUNTIME_H

#include <concepts>
#include <type_traits>

#include "detail/component_pool.h"
#include "detail/context.h"
#include "detail/contract.h"
#include "detail/system.h"
#include "detail/type_list.h"
#include "detail/variant.h"
#include "detail/verification.h"
#include "entity_id.h"
#include "flags.h"
#include "options.h"

namespace ecs {
	template<class Systems>
	ECS_EXPORT class runtime {
	public:
		// Add several components to a range of entities. Will not be added until 'commit_changes()' is called.
		// Pre: entity does not already have the component, or have it in queue to be added
		template <typename... T>
		constexpr void add_component(entity_range const range, T&&... vals) {
			static_assert(detail::is_unique_type_args<T...>(), "the same component type was specified more than once");
			static_assert((!detail::global<T> && ...), "can not add global components to entities");
			static_assert((!std::is_pointer_v<std::remove_cvref_t<T>> && ...), "can not add pointers to entities; wrap them in a struct");
			static_assert((!detail::is_variant_of_pack<T...>()), "Can not add more than one component from the same variant");

			auto const adder = [this, range]<typename Type>(Type&& val) {
				// Add it to the component pool
				if constexpr (detail::is_parent<Type>::value) {
					detail::component_pool<detail::parent_id>& pool = ctx.get_component_pool<detail::parent_id>();
					Pre(!pool.has_entity(range), "one- or more entities in the range already has this type");
					pool.add(range, detail::parent_id{val.id()});
				} else if constexpr (std::is_reference_v<Type>) {
					using DerefT = std::remove_cvref_t<Type>;
					static_assert(std::copyable<DerefT>, "Type must be copyable");

					detail::component_pool<DerefT>& pool = ctx.get_component_pool<DerefT>();
					Pre(!pool.has_entity(range), "one- or more entities in the range already has this type");
					pool.add(range, val);
				} else {
					static_assert(std::copyable<Type>, "Type must be copyable");

					detail::component_pool<Type>& pool = ctx.get_component_pool<Type>();
					Pre(!pool.has_entity(range), "one- or more entities in the range already has this type");
					pool.add(range, std::forward<Type>(val));
				}
			};

			(adder(std::forward<T>(vals)), ...);
		}

		// Adds a span of components to a range of entities. Will not be added until 'commit_changes()' is called.
		// Pre: entity does not already have the component, or have it in queue to be added
		// Pre: range and span must be same size
		void add_component_span(entity_range const range, std::ranges::contiguous_range auto const& vals) {
			static_assert(std::ranges::sized_range<decltype(vals)>, "Size of span is needed.");
			using T = std::remove_cvref_t<decltype(vals[0])>;
			static_assert(!detail::global<T>, "can not add global components to entities");
			static_assert(!std::is_pointer_v<std::remove_cvref_t<T>>, "can not add pointers to entities; wrap them in a struct");
			// static_assert(!detail::is_parent<std::remove_cvref_t<T>>::value, "adding spans of parents is not (yet?) supported"); //
			// should work
			static_assert(std::copyable<T>, "Type must be copyable");

			Pre(range.ucount() == std::size(vals), "range and span must be same size");

			// Add it to the component pool
			if constexpr (detail::is_parent<T>::value) {
				detail::component_pool<detail::parent_id>& pool = ctx.get_component_pool<detail::parent_id>();
				PreAudit(!pool.has_entity(range), "one- or more entities in the range already has this type");
				pool.add_span(range, vals | std::views::transform([](T v) {
										 return detail::parent_id{v.id()};
									 }));
			} else {
				detail::component_pool<T>& pool = ctx.get_component_pool<T>();
				PreAudit(!pool.has_entity(range), "one- or more entities in the range already has this type");
				pool.add_span(range, std::span{vals});
			}
		}

		template <typename Fn>
		void add_component_generator(entity_range const range, Fn&& gen) {
			// Return type of 'func'
			using ComponentType = decltype(std::declval<Fn>()(entity_id{0}));
			static_assert(!std::is_same_v<ComponentType, void>, "Initializer functions must return a component");

			if constexpr (detail::is_parent<std::remove_cvref_t<ComponentType>>::value) {
				auto const converter = [gen = std::forward<Fn>(gen)](entity_id id) {
					return detail::parent_id{gen(id).id()};
				};

				auto& pool = ctx.get_component_pool<detail::parent_id>();
				PreAudit(!pool.has_entity(range), "one- or more entities in the range already has this type");
				pool.add_generator(range, converter);
			} else {
				auto& pool = ctx.get_component_pool<ComponentType>();
				PreAudit(!pool.has_entity(range), "one- or more entities in the range already has this type");
				pool.add_generator(range, std::forward<Fn>(gen));
			}
		}

		// Add several components to an entity. Will not be added until 'commit_changes()' is called.
		// Pre: entity does not already have the component, or have it in queue to be added
		template <typename... T>
		void add_component(entity_id const id, T&&... vals) {
			add_component(entity_range{id, id}, std::forward<T>(vals)...);
		}

		// Removes a component from a range of entities.
		// Will not be removed until 'commit_changes()' is called.
		// Pre: entity has the component
		template <detail::persistent T>
		void remove_component(entity_range const range, T const& = T{}) {
			static_assert(!detail::global<T>, "can not remove or add global components to entities");

			// Remove the entities from the components pool
			detail::component_pool<T>& pool = ctx.get_component_pool<T>();
			Pre(pool.has_entity(range), "component pool does not contain some- or all of the entities in the range");
			pool.remove(range);
		}

		// Removes a component from an entity. Will not be removed until 'commit_changes()' is called.
		// Pre: entity has the component
		template <typename T>
		void remove_component(entity_id const id, T const& = T{}) {
			remove_component<T>({id, id});
		}

		// Returns a global component.
		template <detail::global T>
		T& get_global_component() {
			return ctx.get_component_pool<T>().get_shared_component();
		}

		// Returns the component from an entity, or nullptr if the entity is not found
		// NOTE: Pointers to components are only guaranteed to be valid
		//       until the next call to 'runtime::commit_changes' or 'runtime::update',
		//       after which the component might be reallocated.
		template <detail::local T>
		T* get_component(entity_id const id) {
			// Get the component pool
			detail::component_pool<T>& pool = ctx.get_component_pool<T>();
			return pool.find_component_data(id);
		}

		// Returns the components from an entity range, or an empty span if the entities are not found
		// or does not contain the component.
		// NOTE: Pointers to components are only guaranteed to be valid
		//       until the next call to 'runtime::commit_changes' or 'runtime::update',
		//       after which the component might be reallocated.
		template <detail::local T>
		std::span<T> get_components(entity_range const range) {
			if (!has_component<T>(range))
				return {};

			// Get the component pool
			detail::component_pool<T>& pool = ctx.get_component_pool<T>();
			return {pool.find_component_data(range.first()), range.ucount()};
		}

		// Returns the number of active components for a specific type of components
		template <typename T>
		ptrdiff_t get_component_count() {
			// Get the component pool
			detail::component_pool<T> const& pool = ctx.get_component_pool<T>();
			return pool.num_components();
		}

		// Returns the number of entities that has the component.
		template <typename T>
		ptrdiff_t get_entity_count() {
			// Get the component pool
			detail::component_pool<T> const& pool = ctx.get_component_pool<T>();
			return pool.num_entities();
		}

		// Return true if an entity contains the component
		template <typename T>
		bool has_component(entity_id const id) {
			detail::component_pool<T> const& pool = ctx.get_component_pool<T>();
			return pool.has_entity(id);
		}

		// Returns true if all entities in a range has the component.
		template <typename T>
		bool has_component(entity_range const range) {
			detail::component_pool<T>& pool = ctx.get_component_pool<T>();
			return pool.has_entity(range);
		}

		// Commits the changes to the entities.
		inline void commit_changes() {
			ctx.commit_changes();
		}

		// Calls the 'update' function on all the systems in the order they were added.
		inline void run_systems() {
			ctx.run_systems();
		}

		// Commits all changes and calls the 'update' function on all the systems in the order they were
		// added. Same as calling commit_changes() and run_systems().
		inline void update() {
			commit_changes();
			run_systems();
		}

		// Make a new system
		template <typename... Options, typename SystemFunc, typename SortFn = std::nullptr_t>
		decltype(auto) make_system(SystemFunc sys_func, SortFn sort_func = nullptr) {
			using opts = detail::type_list<Options...>;

			// verify the input
			constexpr static bool dummy_for_clang_13 = detail::make_system_parameter_verifier<opts, SystemFunc, SortFn>();
			(void)dummy_for_clang_13;

			if constexpr (ecs::detail::type_is_function<SystemFunc>) {
				// Build from regular function
				return ctx.create_system<opts, SystemFunc, SortFn>(sys_func, sort_func, sys_func);
			} else if constexpr (ecs::detail::type_is_lambda<SystemFunc>) {
				// Build from lambda
				return ctx.create_system<opts, SystemFunc, SortFn>(sys_func, sort_func, &SystemFunc::operator());
			} else {
				(void)sys_func;
				(void)sort_func;
				struct _invalid_system_type {
				} invalid_system_type;
				return invalid_system_type;
			}
		}

		// Set the memory resource to use to store a specific type of component
		/*template <typename Component>
		void set_memory_resource(std::pmr::memory_resource* resource) {
			auto& pool = ctx.get_component_pool<Component>();
			pool.set_memory_resource(resource);
		}

		// Returns the memory resource used to store a specific type of component
		template <typename Component>
		std::pmr::memory_resource* get_memory_resource() {
			auto& pool = ctx.get_component_pool<Component>();
			return pool.get_memory_resource();
		}

		// Resets the memory resource to the default
		template <typename Component>
		void reset_memory_resource() {
			auto& pool = ctx.get_component_pool<Component>();
			pool.set_memory_resource(std::pmr::get_default_resource());
		}*/

	private:
		detail::context ctx;
	};

} // namespace ecs

#endif // !ECS_RUNTIME_H
