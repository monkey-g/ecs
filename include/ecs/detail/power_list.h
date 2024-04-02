#ifndef ECS_DETAIL_POWER_LIST_H
#define ECS_DETAIL_POWER_LIST_H

#include "contract.h"
#include "scatter_allocator.h"
#include <memory>
#include <queue>
#include <iterator>
#include <ranges>

namespace ecs::detail {
	template <typename T>
	class power_list {
		struct node {
			node* next[2];
			T data;
		};

		struct balance_helper {
			struct stepper {
				std::size_t target;
				std::size_t size;
				node* from;
				constexpr bool operator<(stepper const& r) const {
					return (r.target < target);
				}
				constexpr auto operator<=>(stepper const&) const = default;
			};

			node* curr{};
			std::size_t count{};
			std::size_t log_n{};
			std::size_t index{0};
			stepper* steppers{};

			constexpr balance_helper(balance_helper const& other)
				: curr(other.curr), count(other.count), log_n(other.log_n), steppers(new stepper[log_n]) {
				// Copy steppers
				for (std::size_t i = 0; i < log_n; i++) {
					steppers[i] = other.steppers[i];
				}
			}
			constexpr balance_helper(node* const n, std::size_t count) : curr(n), count(count), log_n(std::bit_width(count - 1)), steppers(new stepper[log_n]) {
				// Set up steppers
				node* current = n;
				for (std::size_t i = 0, step = count; current != nullptr && i < log_n; i++, step >>= 1) {
					steppers[log_n - 1 - i] = {i + step, step, current};
					current = current->next[0];
				}
			}
			constexpr ~balance_helper() {
				while (*this)
					balance_current_and_advance();

				// TODO! Instead of just pointing to the last element, which
				//       will make deletions harder, drop them down a power,
				//       eg. 32 drops to 16+1
				for (std::size_t i = 0; i < log_n; i++)
					steppers[i].from->next[1] = curr;
				delete[] steppers;
			}
			constexpr void balance_current_and_advance() {
				Assert(*this, "Called while invalid");

				std::span<stepper> heap(steppers, steppers + log_n);
				while (heap.front().target == index) {
					heap.front().from->next[1] = curr->next[0];
					heap.front().from = curr;
					heap.front().target += heap.front().size;
					drop_front_in_heap(heap);
				}

				curr = curr->next[0];
				index += 1;
			}
			constexpr operator bool() const {
				return nullptr != curr->next[0];
			}

		private:
			// Drops the front stepper down the heap until the heap property is restored
			constexpr static void drop_front_in_heap(std::span<stepper> heap) {
				std::size_t index = 0;
				do {
					std::size_t const max = 2*index + (heap[2 * index] > heap[2 * index + 1]);
					if (heap[index] > heap[max]) {
						std::swap<stepper>(heap[index], heap[max]);
						index = max;
					} else {
						break;
					}
				} while (2 * index + 1 < heap.size());
			}
		};

	public:
		struct iterator {
			friend class power_list;

			// iterator traits
			using difference_type = ptrdiff_t;
			using value_type = T;
			using pointer = const T*;
			using reference = const T&;
			using iterator_category = std::forward_iterator_tag;

			constexpr iterator() noexcept = default;
			constexpr iterator(iterator const& other) noexcept {
				curr = other.curr;
				prev = other.prev;
				helper = other.helper ? new balance_helper(other.helper->curr, other.helper->count) : nullptr;
			}
			constexpr iterator(iterator&& other) : curr(other.curr), prev(other.prev), helper(std::exchange(other.helper, nullptr)) {}
			constexpr iterator(node *const n, std::size_t count) : curr(n) {
				if (count > 0) // ? TODO > 16
					helper = new balance_helper(n, count);
			}
			constexpr iterator(node* curr, node* prev) : curr(curr), prev(prev) {}
			constexpr ~iterator() {
				delete helper;
			}

			constexpr iterator& operator=(iterator const& other) {
				curr = other.curr;
				prev = other.prev;
				helper = other.helper ? new balance_helper(other.helper->curr, other.helper->count) : nullptr;
				return *this;
			}

			constexpr iterator& operator=(iterator&& other) {
				curr = other.curr;
				prev = other.prev;
				delete helper;
				helper = std::exchange(other.helper, nullptr);
				return *this;
			}

			constexpr std::strong_ordering operator<=>(iterator const& other) const {
				if (curr && !other.curr)
					return std::strong_ordering::less;
				if (!curr && other.curr)
					return std::strong_ordering::greater;
				return curr->data <=> other.curr->data;
			}

			constexpr iterator& operator++() {
				Pre(curr != nullptr, "Trying to step past end of list");
				if (helper && *helper)
					helper->balance_current_and_advance();
				prev = curr;
				curr = curr->next[0];
				return *this;
			}

			constexpr iterator operator++(int) {
				iterator const retval = *this;
				++(*this);
				return retval;
			}

			constexpr bool operator==(std::default_sentinel_t) const {
				return curr == nullptr;
			}

			constexpr bool operator==(iterator other) const {
				return curr == other.curr;
			}

			constexpr bool operator!=(iterator other) const {
				return !(*this == other);
			}

			constexpr operator bool() const {
				return curr != nullptr;
			}

			constexpr value_type operator*() const {
				Pre(curr != nullptr, "Dereferencing null");
				return curr->data;
			}

			constexpr pointer operator->() const {
				Pre(curr != nullptr, "Dereferencing null");
				return &curr->data;
			}

		private:
			node* curr{};
			node* prev{};
			balance_helper* helper{};
		};
		using const_iterator = iterator;

		constexpr power_list() = default;

		constexpr power_list(power_list const& other) {
			assign_range(other);
		}

		constexpr power_list(power_list&& other) {
			head = std::exchange(other.head, nullptr);
			count = std::exchange(other.count, 0);
			needs_rebalance = std::exchange(other.needs_rebalance, false);
			alloc = std::move(other.alloc);
		}

		constexpr power_list(std::ranges::sized_range auto const& range) {
			assign_range(range);
		}

		constexpr ~power_list() {
			destroy_nodes();
		}

		constexpr bool operator==(power_list const& pl) const {
			if (head == pl.head)
				return true;

			if (head == nullptr || pl.head == nullptr)
				return false;

			if (count != pl.count)
				return false;

			if (head->data != pl.head->data)
				return false;

			if (head->next[1] != nullptr && head->next[1]->data != pl.head->next[1]->data)
				return false;

			node *a = head, *b = pl.head;
			while (a) {
				if (a->data != b->data)
					return false;
				a = a->next[0];
				b = b->next[0];
			}

			return true;
		}

		[[nodiscard]] constexpr iterator begin() {
			return {head, static_cast<std::size_t>(needs_rebalance ? count : 0)};
		}
		[[nodiscard]] constexpr iterator begin() const {
			return {head, static_cast<std::size_t>(needs_rebalance ? count : 0)};
		}
		[[nodiscard]] constexpr const_iterator cbegin() const {
			return {head, std::size_t{0}};
		}

		[[nodiscard]] constexpr std::default_sentinel_t end() {
			return {};
		}
		[[nodiscard]] constexpr std::default_sentinel_t end() const {
			return {};
		}
		[[nodiscard]] constexpr std::default_sentinel_t cend() const {
			return {};
		}

		[[nodiscard]] constexpr std::size_t size() const {
			return count;
		}

		[[nodiscard]] constexpr T front() {
			Pre(!empty(), "Can not call 'front()' on an empty list");
			return head->data;
		}

		[[nodiscard]] constexpr T back() {
			Pre(!empty(), "Can not call 'back()' on an empty list");
			return head->next[1] ? head->next[1]->data : head->data;
		}

		[[nodiscard]] constexpr bool empty() const {
			return nullptr == head;
		}

		constexpr void clear() {
			destroy_nodes();
			alloc = scatter_allocator<node>{};
			head = nullptr;
			count = 0;
			needs_rebalance = false;
		}

		constexpr void assign_range(std::ranges::sized_range auto const& range) {
			Pre(std::ranges::is_sorted(range), "Input range must be sorted");
			if (range.empty())
				return;

			if (!empty())
				clear();

			// Save the range size
			count = std::size(range);

			// Do the allocation
			std::span<node> nodes;
			alloc.allocate_with_callback(count, [&](std::span<node> span) {
				Assert(span.size() == count, "This should be a single allocation");
				Assert(nodes.empty(), "This should be empty");
				Assert(nullptr == head, "This shouldn't happen");
				nodes = span;
			});
			head = nodes.data();

			// Get the ranges iterator pair
			auto curr = range.begin();
			auto const end = range.end();

			// Process first part of the range.
			// The rebalancer needs `logN` nodes before it can start.
			std::size_t const logN = (std::size_t)std::bit_width(count - 1); // why is this unsigned in libstdc++? Should be 'int' according to standard.
			std::size_t i = 0;
			for (; i < logN; i += 1) {
				Assert(curr != end, "iterator/size mismatch");
				std::construct_at(&nodes[i], node{{&nodes[i + 1], &nodes[i + 1]}, *curr++});
			}

			// Create a rebalancing iterator
			iterator it_rebalance{head, count};

			// Process the rest of the range while also rebalancing
			for (; i < (count-1); i += 1) {
				Assert(curr != end, "iterator/size mismatch");
				std::construct_at(&nodes[i], node{{&nodes[i + 1], &nodes[i + 1]}, *curr++});
				++it_rebalance;
			}

			// Finally set up the tail node
			std::construct_at(&nodes[i], node{{nullptr, &nodes[i]}, *curr++});

			// Sanity checks
			Assert(curr == end, "Iterator should be at the end here");
		}

		constexpr void insert(T val) {
			node* n = alloc.allocate_one();
			std::construct_at(n, node{{nullptr, nullptr}, val});

			if (head == nullptr) { // empty
				head = n;
				head->next[1] = n;
			} else if (val < head->data) { // before head
				n->next[0] = head;
				n->next[1] = head->next[1];
				head = n;
			} else if (node* last = head->next[1]; last && (last->data < val)) { // after tail
				last->next[0] = n;
				last->next[1] = n;
				head->next[1] = n;
			} else { // middle
				iterator it = lower_bound(val);
				it.prev->next[0] = n;
				n->next[0] = it.curr;
				n->next[1] = it.curr->next[1];
			}

			count += 1;
			needs_rebalance = true;
		}

		constexpr void insert_after(iterator , T ) {
			// TODO
		}

		constexpr void remove(T val) {
			erase(find(val));
		}

		constexpr void erase(iterator it) {
			if (!it)
				return;

			node* n = it.curr;
			node* next = n->next[0];
			if (it.prev == nullptr) { // head
				if (next != nullptr) {
					node* tail = n->next[1];
					next->next[1] = tail;
				}
				head = next;
			} else {
				if (next == nullptr) // tail
					head->next[1] = it.prev;
				it.prev->next[0] = next;
			}

			std::destroy_at(n);
			alloc.deallocate({n, 1});
			count -= 1;
			needs_rebalance = true;
		}

		constexpr void rebalance() {
			if (head && needs_rebalance) {
				balance_helper bh(head, count);
				while (bh)
					bh.balance_current_and_advance();
	
				needs_rebalance = false;
			}

		}

		[[nodiscard]] constexpr iterator find(T const& val) const {
			if (head == nullptr || val < head->data || val > head->next[1]->data)
				return {};

			node* prev = nullptr;
			node* n = head;
			while (n->next[0] && val > n->next[0]->data) {
				prev = n;
				n = n->next[val > n->next[1]->data];
			}
			while (n->data < val) {
				// The only node in the list that can have 'next[0] == nullptr' is
				// the last node in the list. It would have been reached in the above loop.
				Assert(n->next[0] != nullptr, "This should not be possible, by design");

				prev = n;
				n = n->next[0];
			}

			if (n->data == val)
				return {n, prev};
			else
				return {};
		}

		[[nodiscard]] constexpr iterator lower_bound(T const& val) const {
			if (empty())
				return {};
			if (val < head->data)
				return {head, nullptr};

			node* prev = nullptr;
			node* curr = head;
			while (val > curr->data) {
				prev = curr;
				curr = curr->next[val > curr->next[1]->data];
			}
			return {curr, prev};
		}

		constexpr bool contains(T const& val) const {
			return find(val);
		}

	private:
		constexpr void destroy_nodes() {
			node* n = head;
			while (n) {
				node* next = n->next[0];
				Assert(n != next, "Node points to itself");
				std::destroy_at(n);
				n = next;
			}
		}

		node* head = nullptr;
		std::size_t count : 63 = 0;
		std::size_t needs_rebalance : 1 = false;
		scatter_allocator<node> alloc;
	};
	static_assert(std::ranges::sized_range<power_list<int>>);

	// UNIT TESTS
#if 1
	static_assert(
		[] {
			power_list<int> list;
			list.remove(123);
			return list.empty() && list.size() == 0 && !list.contains(0);
		}(),
		"Empty list");

	static_assert(
		[] {
			auto const iota = std::views::iota(-2, 2);
			power_list<int> list(iota);
			for (int v : iota)
				Assert(list.contains(v), "Value not found");
			return true;
		}(),
		"Construction from a range");

	static_assert(
		[] {
			auto const iota = std::views::iota(-2, 2);
			power_list<int> list(iota);
			power_list<int> list2(list);
			return list == list2;
		}(),
		"Copy construction");

	static_assert(
		[] {
			power_list<int> list;
			list.insert(23);
			return list.contains(23);
		}(),
		"Insert empty");

	static_assert(
		[] {
			power_list<int> list;
			list.insert(23);
			list.insert(22);
			return list.contains(23);
		}(),
		"Insert before head");

	static_assert(
		[] {
			power_list<int> list;
			list.insert(23);
			list.insert(24);
			return list.contains(23);
		}(),
		"Insert after tail");

	static_assert(
		[] {
			power_list<int> list;
			list.insert(22);
			list.insert(24);

			list.insert(23);
			return list.contains(23);
		}(),
		"Insert in middle");

	static_assert(
		[] {
			power_list<int> list;
			list.insert(23);
			list.remove(23);
			list.insert(24);
			return !list.contains(23) && list.contains(24);
		}(),
		"Insert/Remove/Insert");

	static_assert(
		[] {
			power_list<int> list(std::views::iota(-2, 2));
			list.assign_range(std::views::iota(0, 4));
			list.assign_range(std::views::iota(4, 8));
			Assert(list.size() == 4, "Invalid element count in list");
			for (int v : std::views::iota(4, 8))
				Assert(list.contains(v), "Value not found");
			return true;
		}(),
		"Assign from a range");

	static_assert(
		[] {
			power_list<int> list;
			list.remove(23);
			return list.empty();
		}(),
		"Remove from empty");

	static_assert(
		[] {
			power_list<int> list(std::views::iota(0, 1));
			list.remove(0);
			return list.empty();
		}(),
		"Remove one");

	static_assert(
		[] {
			power_list<int> list(std::views::iota(0, 8));
			list.remove(0);
			for (int v : std::views::iota(1, 8))
				if (!list.contains(v))
					return false;
			return 7 == list.size();
		}(),
		"Remove head");

	static_assert(
		[] {
			power_list<int> list(std::views::iota(0, 8));
			list.remove(7);
			for (int v : std::views::iota(0, 7))
				Assert(list.contains(v), "missing value");
			;
			return 7 == list.size();
		}(),
		"Remove tail");

	static_assert(
		[] {
			power_list<int> list(std::views::iota(0, 8));
			for (int v : std::views::iota(1, 7))
				list.remove(v);
			int items = 0;
			for (int v : std::views::iota(0, 8))
				items += list.contains(v);
			Post(items == 2, "Items missing from list");
			return 2 == list.size();
		}(),
		"Remove middle");

	static_assert(
		[] {
			auto const iota = std::views::iota(-20, 20);
			power_list<int> list;
			for (int v : iota)
				list.insert(v);
			list.rebalance();
			return list.contains(1);
		}(),
		"Explicit rebalance");

	static_assert(
		[] {
			auto const iota = std::views::iota(-10, 20);
			power_list<int> list;
			for (int v : iota)
				list.insert(v);
			int sum = 0;
			for (int v : list)
				sum += v;
			return sum > 0 && list.contains(1);
		}(),
		"Implicit rebalance");

	static_assert([] {
			auto const iota = std::views::iota(0, 20);
			power_list<int> list1(iota);
			power_list<int> list2(iota);
			if (list1 != list2)
				return false;

			power_list<int> list3;
			for (int val : iota)
				list3.insert(val);
			if (list1 != list3)
				return false;

			return true;
		}(),
		"Comparison operator");
#endif
} // namespace ecs::detail

#endif // !ECS_DETAIL_POWER_LIST_H
