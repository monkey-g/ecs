#include <ecs/ecs.h>
#include <ecs/detail/static_context.h>
#include <iostream>

// The component
struct greeting {
	char const* msg;
};

void hello_sys(greeting const&) {
	std::cout << "hello ";
};

auto constexpr greeting_sys = [](greeting const& g) {
	std::cout << g.msg;
};

using context = ecs::detail::static_context<
	hello_sys,
	greeting_sys>;

int main() {
	context ctx;
	ctx.add_component({0, 2}, greeting{"alright "});
	ctx.build();
	ctx.run();
}
