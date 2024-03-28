#include <catch2/catch_test_macros.hpp>
#include <ecs/detail/scatter_allocator.h>

using ecs::detail::scatter_allocator;

TEST_CASE("Scatter allocator") {
	scatter_allocator<int, 16> alloc;
	auto vec = alloc.allocate(10);
	alloc.deallocate(vec[0].subspan(2, 2));
	alloc.deallocate(vec[0].subspan(4, 2));

	// Fills in the two holes (4), the rest of the first pool (6), 
	// and remaining in new second pool (10)
	int count = 0;
	std::size_t sizes[] = {2, 2, 6, 10};
	alloc.allocate_with_callback(20, [&](auto span) {
		CHECK(sizes[count] == span.size());
		count += 1;
	});
	REQUIRE(count == 4);
}
