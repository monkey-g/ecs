#include <ecs/ecs.h>
#include <ecs/systems.h>
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

using systems = ecs::systems<hello_sys, greeting_sys>;

int main() {
	//ecs::runtime<systems> rt;

	systems s;
	s.run(greeting{"alright "});

	// The system
	//rt.make_system([](greeting const& g) { std::cout << g.msg; });

	// The entities
	//rt.add_component({0, 2}, greeting{"alright "});

	// Run the system on all entities with a 'greeting' component
	//rt.update();
}
