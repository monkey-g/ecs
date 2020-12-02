#include <iostream>
#include <ecs/ecs.h>

//     ______1_________              100
//    /      |         \              |
//   4       3          2            101
//  /|\     /|\       / | \
// 5 6 7   8 9 10   11  12 13
// |         |             |
// 14        15            16

using namespace ecs;
using std::cout;

// Print children, filtered on their parent
auto constexpr print_roots          = [](entity_id id, int, parent<>*) { cout << id << ' '; };
auto constexpr print_all_children   = [](entity_id id, parent<> /*p*/) { cout << id << ' '; };
auto constexpr print_short_children = [](entity_id id, parent<short> const& p) { cout << id << '(' << p.get<short>() << ") "; };
auto constexpr print_long_children  = [](entity_id id, parent<long> const& p) { cout << id << '(' << p.get<long>() << ") "; };
auto constexpr print_float_children = [](entity_id id, parent<float> const& p) { cout << id << '(' << p.get<float>() << ") "; };

int main() {
    // The root
    add_component({1}, int{});

    // The children
    add_component(4, parent{1}, int{}, short{10});
    add_component(3, parent{1}, int{}, long{20});
    add_component(2, parent{1}, int{}, float{30});

    // The grandchildren
    add_component({5, 7}, parent{4}, int{});  // short children, parent 4 has a short
    add_component({8, 10}, parent{3}, int{}); // long children, parent 3 has a long
    add_component({11, 13}, parent{2}, int{});// float children, parent 2 has a float

    // The great-grandchildren
    add_component(14, parent{5}, int{});
    add_component(15, parent{9}, int{});
    add_component(16, parent{13}, int{});

    // second small tree
    add_component({100}, int{});
    add_component({101}, parent{100}, int{});

    // hierarchial systems are implicitly serial, so no need for opts::not_parallel
    auto& sys_roots = make_system(print_roots);
    auto& sys_all = make_system(print_all_children);
    auto& sys_short = make_system(print_short_children);
    auto& sys_long = make_system(print_long_children);
    auto& sys_float = make_system(print_float_children);

    commit_changes();

    // Run the systems
    cout << "All roots        : ";   sys_roots.run();  cout << '\n'; // 1 100
    cout << "All children     : ";   sys_all.run();    cout << '\n'; // 2-16 101
    cout << "short children   : ";   sys_short.run();  cout << '\n'; // 5-7
    cout << "long children    : ";   sys_long.run();   cout << '\n'; // 8-10
    cout << "floating children: ";   sys_float.run();  cout << '\n'; // 11-13
}
