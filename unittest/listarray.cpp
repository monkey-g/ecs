#include "ecs/detail/power_list.h"
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <iostream>
#include <queue>
#include <ranges>

struct node {
	node* next[2];
	int data;
};

struct stepper {
	int target;
	int size;
	node* from;
	bool operator<(stepper other) const {
		return (other.target < target);
	}
};

int search(node* n, int val) {
	std::cout << n->data;

	if (val < n->data || val > n->next[1]->data)
		return -1;

	int steps = 0;
	while (val > n->data) {
		n = n->next[val >= n->next[1]->data];
		std::cout << " -> " << n->data;
		steps += 1;
	}
	return (val == n->data) ? steps : -1;
}

TEST_CASE("Gorking list") {
	constexpr int N = 64;
	int constexpr log_n = std::bit_width((unsigned)N-1);

	std::array<node, N> nodes{};

	// Init linked list
	for (int i = 0; i < N - 1; i += 1) {
		node* const next = &nodes[i + 1];
		nodes[i] = {{next, next}, i};
	}
	nodes[N - 1] = {{nullptr, &nodes[N - 1]}, N - 1};

	// Load up steppers
	node* current = &nodes[0];
	stepper stack[32];
	for (int i = 0; i < log_n; i++) {
		int const step = 1 << (log_n - i);
		stack[log_n - 1 - i] = {i + step, step, current};
		current = current->next[0];
	}
	// std::make_heap(stack, stack + log_n);

	// Rebuild the jump points in O(n log log n) time
	int i = 0;
	current = &nodes[0];
	stepper* min_step = &stack[log_n - 1];
	while (current->next[0] != nullptr) {
		while (stack[0].target == i) {
			std::pop_heap(stack, stack + log_n);
			min_step->from->next[1] = current->next[0];
			min_step->from = current;
			min_step->target = i + min_step->size;
			std::push_heap(stack, stack + log_n);
		}

		i += 1;
		current = current->next[0];
	}
	for (i = 0; i < log_n; i++) {
		stack[i].from->next[1] = current;
	}

#if 1
	for (node& n : nodes) {
		std::cout << n.data;
		if (n.next[1] != nullptr) {
			auto const dist = std::distance(&nodes[n.data], n.next[1]);
			std::cout << "\t -> " << (n.data + dist);
			//std::cout << "  distance: " << dist;

			if (dist > 1) {
				int ix = std::bit_width(unsigned(dist - 2));
				std::cout << "\t  pow2: " << (1 << ix);

				int const next_pow2 = 1 << std::bit_width(unsigned((ix | (ix - 1)) + 1));
				std::cout << "\t  next pow2: " << (next_pow2);
			}
		}
		std::cout << '\n';
	}
#endif
#if 1
	node* root = &nodes[0];
	int total_steps = 0;
	int max_steps = 0;
	for (node& n : nodes) {
		std::cout << "search '" << n.data << "' : \t";
		int steps = search(root, n.data);
		total_steps += steps;
		max_steps = std::max(max_steps, steps);
		std::cout << " (" << steps << ")\n";
	}
	std::cout << "\nMaximum steps: " << max_steps << '\n';
	std::cout << "Total steps  : " << total_steps << '\n';
#endif

	auto const iota = std::views::iota(-200, 200);
	ecs::detail::power_list<int> list(iota);

#if 0
	ecs::detail::power_list<int> list(std::views::iota(-2, 100));
	for (int const val : std::views::iota(-2, 100))
		REQUIRE(list.contains(val));
	REQUIRE(!list.contains(-3));
	REQUIRE(!list.contains(101));

	list.insert(101);
	REQUIRE(list.contains(101));
	list.insert(-3);
	REQUIRE(list.contains(-3));
	list.insert(22);
	REQUIRE(list.contains(22));

	ecs::detail::power_list<int> list2;
	for (int const val : std::views::iota(-2, 100))
		list2.insert(val);
	for (int val : list2)
		val++;
	REQUIRE(list2.contains(83));

	list2.remove(83);
	REQUIRE(list2.contains(82));
	REQUIRE(!list2.contains(83));
	REQUIRE(list2.contains(84));

#endif
}


#include <bit>
#include <cassert>
#include <format>
#include <iostream>

void print_steppers_bitpattern() {
	constexpr unsigned N = 256;
	constexpr unsigned log_N = std::bit_width(N);

	// Diag data
	unsigned arr[N + 1]{};
	unsigned max[N + 1]{};
	unsigned mask[N + 1]{};
	std::fill_n(arr, N, 0);
	std::fill_n(max, N, 0);
	std::fill_n(mask, N, 0);

	struct stepper {
		unsigned target;
		unsigned size;
	};

	constexpr unsigned num_steppers = log_N;
	std::array<stepper, num_steppers> steppers;
	for (unsigned curr_log = 0; curr_log < num_steppers; curr_log++) {
		unsigned const log_stepsize = (log_N - 1 - curr_log);
		unsigned const stepsize = 1 << log_stepsize;
		steppers[log_stepsize] = {curr_log + stepsize, stepsize};

		arr[curr_log] = stepsize;
		max[curr_log] = stepsize;
		mask[curr_log] = stepsize;
	}

	for (unsigned i = num_steppers; i < N; i++) {
		unsigned mask_i = 0;
		mask[i] = mask_i;

		for (unsigned curr_log = 0; curr_log < num_steppers; curr_log++) {
			if (steppers[curr_log].target == i) {
				arr[i] |= steppers[curr_log].size;
				max[i] = std::max(max[i], steppers[curr_log].size);
				steppers[curr_log].target += steppers[curr_log].size;
			}
		}
	}

	std::cout << std::format("\nN = {:>3}, log(N) = {:>3}\n\n", N, log_N);
	for (unsigned i = 0; i < N; i++) {
		std::cout << std::format("{:>3} : {:>12b} - {:>12} - {}\n", i, arr[i], mask[i], max[i]);
	}
}
