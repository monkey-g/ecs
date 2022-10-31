#define CATCH_CONFIG_MAIN
#include "catch.hpp"
#include <ecs/detail/type_list.h>

#include <ecs/detail/options.h>
#include <ecs/parent.h>

using namespace ecs::detail;

using tl1 = type_list<int, float>;
using tl2 = type_list<double, short, int>;
using tl1_concat_tl2 = type_list<int, float, double, short, int>;
using tl1_merge_tl2 = type_list<int, float, double, short>;

using parent_test_1 = ecs::parent<int, float>;
using parent_test_2 = ecs::parent<>;
static_assert(is_parent<parent_test_1>::value);
static_assert(is_parent<parent_test_2>::value);

static_assert(std::is_same_v<void, test_option_type_or<is_parent, type_list<int, char>, void>>);
static_assert(!std::is_same_v<void, test_option_type_or<is_parent, type_list<int, parent_test_1, char>, void>>);
static_assert(std::is_same_v<void, test_option_type_or<is_parent, type_list<int, parent_test_1*, char>, void>>);
static_assert(!std::is_same_v<void, test_option_type_or<is_parent, type_list<int, parent_test_1&, char>, void>>);

template <typename... Ts>
using total_size = std::integral_constant<std::size_t, (sizeof(Ts) + ...)>;

TEST_CASE("type_list") {
	using TL = type_list<char, int, float, void*>;

	SECTION("type_list_size") {
		static_assert(0 == type_list_size<type_list<>>);
		static_assert(1 == type_list_size<type_list<char>>);
		static_assert(3 == type_list_size<type_list<char, int, float>>);
	}

	SECTION("type_list_indices") {
		using TLI = type_list_indices<TL>;
		static_assert(3 == TLI::index_of(static_cast<void**>(nullptr)));
		static_assert(2 == TLI::index_of(static_cast<float*>(nullptr)));
		static_assert(1 == TLI::index_of(static_cast<int*>(nullptr)));
		static_assert(0 == TLI::index_of(static_cast<char*>(nullptr)));
	}

	SECTION("index_of") {
		static_assert(3 == index_of<void*, TL>());
		static_assert(2 == index_of<float, TL>());
		static_assert(1 == index_of<int, TL>());
		static_assert(0 == index_of<char, TL>());
	}

	SECTION("transform_type") {
		using PTR_TL = transform_type<TL, std::add_pointer_t>;
		static_assert(std::is_same_v<PTR_TL, type_list<char*, int*, float*, void**>>);
	}

	SECTION("transform_type_all") {
		using Size = transform_type_all<TL, total_size>;
		static_assert(Size::value == (sizeof(char) + sizeof(int) + sizeof(float) + sizeof(void*)));
	}

	SECTION("split_types_if") {
		using Pair = split_types_if<TL, std::is_integral>;

		using Integrals = Pair::first;
		using NotIntegrals = Pair::second;

		static_assert(std::is_same_v<Integrals, type_list<char, int>>);
		static_assert(std::is_same_v<NotIntegrals, type_list<float, void*>>);
	}

	SECTION("for_each_type") {
		int pointers = 0, non_pointers = 0;

		for_each_type<TL>([&]<typename T>() {
			if constexpr (std::is_pointer_v<T>)
				pointers += 1;
			else
				non_pointers += 1;
		});

		REQUIRE(pointers == 1);
		REQUIRE(non_pointers == 3);
	}

	SECTION("for_specific_type") {
		int found_ints = 0;

		for_specific_type<int, TL>([&]() {
			found_ints += 1;
		});

		REQUIRE(1 == found_ints);
	}

	SECTION("for_all_types") {
		std::size_t num_types = 0;
		std::size_t sizes = 0;

		for_all_types<TL>([&]<typename... Ts>() {
			num_types = sizeof...(Ts);
			sizes = (sizeof(Ts) + ...);
		});

		REQUIRE(num_types == std::size_t{4});
		REQUIRE(sizes == (sizeof(char) + sizeof(int) + sizeof(float) + sizeof(void*)));
	}

	SECTION("all_of_type") {
		bool const is_all_pointers = all_of_type<TL>([]<typename T>() {
			return std::is_pointer_v<T>;
		});
		REQUIRE(false == is_all_pointers);
	}

	SECTION("any_of_type") {
		bool const is_any_pointers = any_of_type<TL>([]<typename T>() {
			return std::is_pointer_v<T>;
		});
		REQUIRE(true == is_any_pointers);
	}

	SECTION("run_if") {
        using TL_o = type_list<int, ecs::opts::group<1>>;
        using TL2 = type_list<int, float>;

        int runs = 0;
        int const ret_run = run_if<is_group, TL_o>([&]<typename>() {
            runs += 1;
            return runs;
        });
        run_if<is_group, TL2>([&]<typename>() {
            runs += 1;
        });

        CHECK(1 == runs);
        CHECK(1 == ret_run);
    }

	SECTION("count_type_if") {
		std::size_t const num_size_4 = count_type_if<TL>([]<typename T>() {
			return 4 == sizeof(T);
		});
		REQUIRE(num_size_4 == std::size_t{2});
	}

    SECTION("is_unique_types") {
        static_assert(is_unique_types<tl1>());
        static_assert(is_unique_types<tl2>());
        static_assert(not is_unique_types<tl1_concat_tl2>());
    }

    SECTION("contains_type") {
        static_assert(contains_type<float, TL>());
        static_assert(not contains_type<long, TL>());
    }

    SECTION("concat_type_lists") {
        static_assert(std::is_same_v<tl1_concat_tl2, concat_type_lists<tl1, tl2>>);
    }

    SECTION("merge_type_lists") {
        static_assert(std::is_same_v<tl1_merge_tl2, merge_type_lists<tl1, tl2>>);
        static_assert(std::is_same_v<tl1, merge_type_lists<tl1, tl1>>);
    }
}
