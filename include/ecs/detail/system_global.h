#ifndef ECS_SYSTEM_GLOBAL_H
#define ECS_SYSTEM_GLOBAL_H

#include "system.h"

namespace ecs::detail {
	// The implementation of a system specialized on its components
	template <typename Options, typename UpdateFn, bool FirstIsEntity, typename ComponentsList, typename PoolsList>
	class system_global final : public system<Options, UpdateFn, FirstIsEntity, ComponentsList, PoolsList> {
		static_assert(false == FirstIsEntity, "global systems does not work on entities, they are stand-alone");
		using base = system<Options, UpdateFn, FirstIsEntity, ComponentsList, PoolsList>;

	public:
		system_global(UpdateFn func, component_pools<PoolsList>&& in_pools)
			: base{func, std::forward<component_pools<PoolsList>>(in_pools)} {
			this->process_changes(true);
		}

	private:
		operation make_operation() override {
			return operation{(argument*)0, this->get_update_func()};
		}

		template <typename... Ts>
		static auto make_argument(auto... args) {
			return [=](UpdateFn& fn, entity_id, entity_offset offset) {
				for_all_types<ComponentsList>([&]<typename... Types>() {
					fn(extract_arg_lambda<Types>(args, offset, 0)...);
				});
			};
		}

		void do_run() override {
			for_all_types<PoolsList>([&]<typename... Types>() {
				this->update_func(this->pools.template get<Types>().get_shared_component()...);
			});
		}

		void do_build() override {}

		using argument = std::remove_cvref_t<decltype(for_all_types<ComponentsList>([]<typename... Types>() {
			return make_argument<Types...>(component_argument<Types>{}...);
		}))>;
	};
} // namespace ecs::detail

#endif // !ECS_SYSTEM_GLOBAL_H
