#include "gbench/include/benchmark/benchmark.h"
#include <ecs/ecs.h>
#include <random>

#include "global.h"

auto const test_component = std::string{"some large string to bypass sso"};

void component_generate(benchmark::State& state) {
    auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));

    for ([[maybe_unused]] auto const _ : state) {
        ecs::runtime ecs;

        ecs.add_component({0, nentities}, [](ecs::entity_id id) { return test_component + std::to_string(id); });
        ecs.commit_changes();
    }

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_generate);

void component_add(benchmark::State& state) {
    auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));

    for ([[maybe_unused]] auto const _ : state) {
        ecs::runtime ecs;

        ecs.add_component({0, nentities}, test_component);
        ecs.commit_changes();
    }

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_add);

void component_add_1k_blocks(benchmark::State& state) {
    auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));

    for ([[maybe_unused]] auto const _ : state) {
        ecs::runtime ecs;

        for (ecs::entity_id i = 0; i < nentities; i += 1024) {
			ecs.add_component({i, i+1023}, test_component);
			ecs.commit_changes();
		}
    }

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_add_1k_blocks);

void component_add_half_front(benchmark::State& state) {
    auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));

    for ([[maybe_unused]] auto const _ : state) {
        ecs::runtime ecs;

		state.BeginIgnoreTiming();
		ecs.add_component({nentities / 2 + 1, nentities}, test_component);
        ecs.commit_changes();
		state.EndIgnoreTiming();

        ecs.add_component({0, nentities / 2}, test_component);
        ecs.commit_changes();
    }

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_add_half_front);

void component_add_half_back(benchmark::State& state) {
    auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));

    for ([[maybe_unused]] auto const _ : state) {
        ecs::runtime ecs;

		state.BeginIgnoreTiming();
		ecs.add_component({0, nentities / 2}, test_component);
        ecs.commit_changes();
		state.EndIgnoreTiming();

        ecs.add_component({nentities / 2 + 1, nentities}, test_component);
        ecs.commit_changes();
    }

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_add_half_back);

// This is currently the worst case scenario. Every commit will move all other components as well.
void component_insert_worst_case(benchmark::State& state) {
	auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));
	constexpr int block_size = 256;

	for ([[maybe_unused]] auto const _ : state) {
		ecs::runtime ecs;

        for (auto i = nentities; i >= 0; i -= block_size) {
			ecs.add_component({i - block_size + 1, i}, test_component);
			ecs.commit_changes();
		}
	}

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_insert_worst_case);

void component_remove_all(benchmark::State& state) {
    auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));

    for ([[maybe_unused]] auto const _ : state) {
        ecs::runtime ecs;

		state.BeginIgnoreTiming();
		ecs.add_component({0, nentities}, test_component);
        ecs.commit_changes();
		state.EndIgnoreTiming();

        ecs.remove_component<std::string>({0, nentities});
        ecs.commit_changes();
    }

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_remove_all);

void component_remove_half_front(benchmark::State& state) {
    auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));

    for ([[maybe_unused]] auto const _ : state) {
        ecs::runtime ecs;

		state.BeginIgnoreTiming();
		ecs.add_component({0, nentities}, test_component);
        ecs.commit_changes();
		state.EndIgnoreTiming();

        ecs.remove_component<std::string>({0, nentities / 2});
        ecs.commit_changes();
    }

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_remove_half_front);

void component_remove_half_back(benchmark::State& state) {
    auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));

    for ([[maybe_unused]] auto const _ : state) {
        ecs::runtime ecs;

		state.BeginIgnoreTiming();
		ecs.add_component({0, nentities}, test_component);
        ecs.commit_changes();
		state.EndIgnoreTiming();

        ecs.remove_component<std::string>({nentities / 2 + 1, nentities});
        ecs.commit_changes();
    }

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_remove_half_back);

void component_remove_half_middle(benchmark::State& state) {
    auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));

    for ([[maybe_unused]] auto const _ : state) {
        ecs::runtime ecs;
		state.BeginIgnoreTiming();
            ecs.add_component({0, nentities}, test_component);
            ecs.commit_changes();
		state.EndIgnoreTiming();

        ecs.remove_component<std::string>({nentities / 4, nentities - nentities / 4});
        ecs.commit_changes();
    }

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_remove_half_middle);

void component_randomized_add(benchmark::State& state) {
    auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));

    std::vector<ecs::entity_id> ids;
    ids.reserve(nentities);
    std::generate_n(std::back_inserter(ids), nentities, [i = ecs::entity_id{0}]() mutable { return i++; });
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(ids.begin(), ids.end(), g);

    for ([[maybe_unused]] auto const _ : state) {
        ecs::runtime ecs;

        for (auto id : ids) {
			ecs.add_component(id, test_component);
        }
        ecs.commit_changes();
    }

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_randomized_add);

void component_randomized_remove(benchmark::State& state) {
    auto const nentities = static_cast<ecs::detail::entity_type>(state.range(0));

    std::vector<ecs::entity_id> ids;
    ids.reserve(nentities);
    std::generate_n(std::back_inserter(ids), nentities, [i = ecs::entity_id{0}]() mutable { return i++; });
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(ids.begin(), ids.end(), g);

    for ([[maybe_unused]] auto const _ : state) {
        ecs::runtime ecs;

        state.PauseTiming();
		ecs.add_component({0, nentities - 1}, test_component);
        ecs.commit_changes();
        state.ResumeTiming();

        for (auto id : ids) {
            ecs.remove_component<std::string>(id);
        }
        ecs.commit_changes();
    }

	state.SetItemsProcessed(state.iterations() * nentities);
}
ECS_BENCHMARK(component_randomized_remove);
