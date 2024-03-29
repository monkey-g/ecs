#include <ecs/ecs.h>
#include <exception>
#include <numeric>
#include <catch2/catch_test_macros.hpp>

// Override the default handler for contract violations.
#include "override_contract_handler_to_throw.h"

// A helper class that counts invocations of constructers/destructor
struct ctr_counter {
	inline static size_t def_ctr_count = 0;
	inline static size_t ctr_count = 0;
	inline static size_t copy_count = 0;
	inline static size_t move_count = 0;
	inline static size_t dtr_count = 0;

	ctr_counter() noexcept {
		def_ctr_count++;
		ctr_count++;
	}
	ctr_counter(ctr_counter const& /*other*/) noexcept {
		copy_count++;
		ctr_count++;
	}
	ctr_counter(ctr_counter&& /*other*/) noexcept {
		move_count++;
		ctr_count++;
	}
	~ctr_counter() noexcept {
		dtr_count++;
	}

	ctr_counter& operator=(ctr_counter&&) = default;
	ctr_counter& operator=(ctr_counter const&) = default;
};

// A bunch of tests to ensure that the component_pool behaves as expected
TEST_CASE("Component pool specification", "[component]") {
	SECTION("A new component pool is empty") {
		ecs::detail::component_pool<int> pool;
		REQUIRE(pool.num_entities() == 0);
		REQUIRE(pool.num_components() == 0);
		REQUIRE(pool.has_component_count_changed() == false);
	}

	SECTION("An empty pool") {
		SECTION("does not throw on bad component access") {
			ecs::detail::component_pool<int> pool;
			REQUIRE(nullptr == pool.find_component_data(0));
		}
		SECTION("grows when data is added to it") {
			ecs::detail::component_pool<int> pool;
			pool.add({0, 4}, 0);
			pool.process_changes();

			REQUIRE(pool.num_entities() == 5);
			REQUIRE(pool.num_components() == 5);
			REQUIRE(pool.has_more_components());
		}
	}

	SECTION("Adding components") {
		SECTION("does not perform unneeded copies of components") {
			ecs::detail::component_pool<ctr_counter> pool;
			pool.add({0, 2}, ctr_counter{});
			pool.process_changes();
			pool.remove({0, 2});
			pool.process_changes();

			static constexpr std::size_t expected_copy_count = 3;
			REQUIRE(ctr_counter::copy_count == expected_copy_count);
			REQUIRE(ctr_counter::ctr_count == ctr_counter::dtr_count);
		}
		SECTION("with a span is valid") {
			std::vector<int> ints(10);
			std::iota(ints.begin(), ints.end(), 0);

			ecs::detail::component_pool<int> pool;
			pool.add_span({0, 9}, ints);
			pool.process_changes();

			REQUIRE(10 == pool.num_components());

			for (int i = 0; i <= 9; i++) {
				REQUIRE (i == *pool.find_component_data(i));
			}
		}
		SECTION("with a generator is valid") {
			ecs::detail::component_pool<int> pool;
			pool.add_generator({0, 9}, [](auto i) {
				return static_cast<int>(i);
			});
			pool.process_changes();

			REQUIRE(10 == pool.num_components());

			for (int i = 0; i <= 9; i++) {
				REQUIRE(i == *pool.find_component_data(i));
			}
		}
		SECTION("with negative entity ids is fine") {
			ecs::detail::component_pool<int> pool;
			pool.add({-999, -950}, 0);
			pool.process_changes();

			REQUIRE(50 == pool.num_components());
			REQUIRE(50 == pool.num_entities());
		}
		SECTION("has_entity works correctly") {
			ecs::detail::component_pool<int> pool;
			pool.add({0, 10}, 0);
			pool.process_changes();

			pool.remove({1, 10});
			pool.process_changes();

			pool.add({8, 15}, 1);
			pool.process_changes();
		}
	}

	SECTION("Testing components") {
		SECTION("stradling ranges works") {
			ecs::detail::component_pool<int> pool;
			pool.add({0, 10}, 0);
			pool.process_changes();
			pool.add({11, 20}, 0);
			pool.process_changes();

			CHECK(2 == pool.num_chunks());
			REQUIRE(pool.has_entity({5, 15}));
		}

		SECTION("stradling ranges with gaps works") {
			ecs::detail::component_pool<int> pool;
			pool.add({0, 9}, 0);
			pool.process_changes();
			pool.add({11, 20}, 0);
			pool.process_changes();
			pool.add({21, 30}, 0);
			pool.process_changes();

			CHECK(3 == pool.num_chunks());
			REQUIRE(!pool.has_entity({5, 15})); // entity 10 missing
		}
	}

	SECTION("Removing components") {
		SECTION("from the back does not invalidate other components") {
			std::vector<int> ints(11);
			std::iota(ints.begin(), ints.end(), 0);

			ecs::detail::component_pool<int> pool;
			pool.add_span({0, 10}, ints);
			pool.process_changes();

			pool.remove({9, 10});
			pool.process_changes();

			REQUIRE(pool.num_components() == 9);
			for (int i = 0; i <= 8; i++) {
				REQUIRE(i == *pool.find_component_data(i));
			}
		}
		SECTION("from the front does not invalidate other components") {
			std::vector<int> ints(11);
			std::iota(ints.begin(), ints.end(), 0);

			ecs::detail::component_pool<int> pool;
			pool.add_span({0, 10}, ints);
			pool.process_changes();

			pool.remove({0, 1});
			pool.process_changes();

			REQUIRE(pool.num_components() == 9);
			for (int i = 2; i <= 10; i++) {
				REQUIRE(i == *pool.find_component_data(i));
			}
		}
		SECTION("from the middle does not invalidate other components") {
			std::vector<int> ints(11);
			std::iota(ints.begin(), ints.end(), 0);

			ecs::detail::component_pool<int> pool;
			pool.add_span({0, 10}, ints);
			pool.process_changes();

			pool.remove({4, 5});
			pool.process_changes();

			REQUIRE(pool.num_components() == 9);
			for (int i = 0; i <= 3; i++) {
				REQUIRE(i == *pool.find_component_data(i));
			}
			for (int i = 6; i <= 10; i++) {
				REQUIRE(i == *pool.find_component_data(i));
			}
		}
		SECTION("piecewise does not invalidate other components") {
			std::vector<int> ints(11);
			std::iota(ints.begin(), ints.end(), 0);

			ecs::detail::component_pool<int> pool;
			pool.add_span({0, 10}, ints);
			pool.process_changes();

			pool.remove({10, 10});
			pool.remove({9, 9});
			pool.process_changes();

			REQUIRE(pool.num_components() == 9);
			for (int i = 0; i <= 8; i++) {
				REQUIRE(i == *pool.find_component_data(i));
			}
		}
		SECTION("that span multiple chunks") {
			ecs::detail::component_pool<int> pool;
			pool.add({0, 5}, int{});
			pool.process_changes();
			pool.add({6, 10}, int{});
			pool.process_changes();

			pool.remove({0, 10});
			pool.process_changes();

			REQUIRE(pool.num_components() == 0);
		}
		SECTION("that don't exist does nothing") {
			ecs::detail::component_pool<int> pool;
			pool.remove({0, 5});
			pool.process_changes();

			pool.add({6, 10}, int{});
			pool.process_changes();
			pool.remove({0, 5});
			pool.process_changes();
			SUCCEED();
		}
	}

	SECTION("A non empty pool") {
		std::vector<int> ints(10);
		std::iota(ints.begin(), ints.end(), 0);

		ecs::detail::component_pool<int> pool;
		pool.add_span({0, 9}, ints);
		pool.process_changes();

		// "has the correct entities"
		REQUIRE(10 == pool.num_entities());
		REQUIRE(pool.has_entity({0, 9}));

		// "has the correct components"
		REQUIRE(10 == pool.num_components());
		for (int i = 0; i <= 9; i++) {
			REQUIRE(i == *pool.find_component_data({i}));
		}

		// "does not throw when accessing invalid entities"
		REQUIRE(nullptr == pool.find_component_data(10));

		// "shrinks when entities are removed"
		pool.remove({4});
		pool.process_changes();

		REQUIRE(9 == pool.num_entities());
		REQUIRE(9 == pool.num_components());
		REQUIRE(pool.has_less_components());

		// "becomes empty after clear"
		pool.clear();
		REQUIRE(0 == pool.num_entities());
		REQUIRE(0 == pool.num_components());
		REQUIRE(!pool.has_more_components());
		REQUIRE(pool.has_less_components());

		// "remains valid after internal growth"
		int const* org_p = pool.find_component_data(0);

		for (int i = 10; i < 32; i++) {
			pool.add({i, i}, i);
			pool.process_changes();
		}

		for (int i = 10; i < 32; i++) {
			REQUIRE(i == *pool.find_component_data(i));
		}

		// memory address has not changed
		REQUIRE(org_p == pool.find_component_data(0));
	}

	SECTION("Transient components") {
		SECTION("are automatically removed in process_changes()") {
			struct tr_test {
				using ecs_flags = ecs::flags<ecs::transient>;
			};
			ecs::detail::component_pool<tr_test> pool;
			pool.add({0, 9}, tr_test{});

			pool.process_changes(); // added
			pool.process_changes(); // automatically removed
			REQUIRE(0 == pool.num_components());
		}
	}

	SECTION("Tagged components") {
		SECTION("maintains sorting of entities") { // test case is response to a found bug
			struct some_tag {
				using ecs_flags = ecs::flags<ecs::tag>;
			};
			ecs::detail::component_pool<some_tag> pool;
			pool.add({0, 0}, {});
			pool.process_changes();
			pool.add({-2, -2}, {});
			pool.process_changes();

			auto const ev = pool.get_entities();
			REQUIRE((*ev).range.first() == -2);
		}
	}

	SECTION("Global components") {
		SECTION("are always available") {
			struct some_global {
				using ecs_flags = ecs::flags<ecs::global>;
				int v = 0;
			};
			ecs::detail::component_pool<some_global> pool;

			// if the component is not available, this will crash/fail
			pool.get_shared_component().v += 1;
		}
	}

	SECTION("chunked memory") {
	}
}
