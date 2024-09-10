#include <iostream>
#include <ecs/ecs.h>
#include <ecs/detail/static_context.h>

// The component
struct greeting {
	char const* msg;
};

void test(int) {}

void hello_sys(greeting const&) {
	std::cout << "hello ";
};

auto constexpr greeting_sys = [](greeting const& g) {
	std::cout << g.msg;
};

using context = ecs::detail::static_context<
	hello_sys,
	greeting_sys,
	test>;

int main() {
	context ctx;
	ctx.add_component({0, 2}, greeting{"alright "});
	ctx.add_component({0,0}, 4);
	ctx.build();
	ctx.run();
}
