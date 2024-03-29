#ifndef ECS_DETAIL_GORKING_LIST_H
#define ECS_DETAIL_GORKING_LIST_H

#include "contract.h"
#include "scatter_allocator.h"
//#include <algorithm>
#include <memory>
#include <queue>
#include <ranges>

namespace ecs::detail {
	template <typename T>
	class power_list {
		struct node {
			node* next[2]{};
			T data{};
		};

		struct balance_helper {
			struct stepper {
				std::size_t target;
				std::size_t size;
				node* from;
				constexpr bool operator<(stepper const& a) const {
					return (a.target < target);
				}
			};

			node* curr{};
			std::size_t count{};
			std::size_t log_n{};
			std::size_t index{0};
			std::array<stepper, 32> steppers{};

			constexpr balance_helper(node* const n, std::size_t count) : curr(n), count(count), log_n(std::bit_width(count - 1)) {
				Pre(std::cmp_less(log_n, steppers.size()), "List is too large, increase array capacity");

				// Load up steppers
				node* current = n;
				for (std::size_t i = 0, step = count; i < log_n; i++, step >>= 1) {
					steppers[i] = {i + step, step, current};
					current = current->next[0];
				}
			}
			constexpr ~balance_helper() {
				while (*this)
					balance_current_and_advance();

				// TODO! Instead of just pointing to the last element, which
				//       will make deletions, drop them down a power,
				//       eg. 32 drops to 16+1
				for (std::size_t i = 0; i < log_n; i++)
					steppers[i].from->next[1] = curr;
			}
			constexpr void balance_current_and_advance() {
				Assert(*this, "Called without checking validity");
				stepper& min_step = steppers[log_n - 1];
				while (steppers[0].target == index) {
					std::pop_heap(steppers.data(), steppers.data() + log_n);
					min_step.from->next[1] = curr->next[0];
					min_step.from = curr;
					min_step.target += min_step.size;
					std::push_heap(steppers.data(), steppers.data() + log_n);
				}

				curr = curr->next[0];
				index += 1;
			}
			constexpr operator bool() const {
				return nullptr != curr->next[0];
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

		constexpr iterator begin() {
			return {head, static_cast<std::size_t>(needs_rebalance ? count : 0)};
		}
		constexpr iterator begin() const {
			return {head, std::size_t{0}};
		}

		constexpr std::default_sentinel_t end() { return {}; }
		constexpr std::default_sentinel_t end() const { return {}; }

		constexpr std::size_t size() const {
			return count;
		}

		constexpr T front(this auto&&) {
			Pre(!empty(), "Can not call 'front()' on an empty list");
			return head->data;
		}

		constexpr T back(this auto&&) {
			Pre(!empty(), "Can not call 'back()' on an empty list");
			return head->next[1] ? head->next[1]->data : head->data;
		}

		[[nodiscard]]
		constexpr bool empty() const {
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

			count = std::size(range);
			alloc.allocate_with_callback(count, [&](auto span) {
				Assert(span.size() == count, "This should be a single allocation");
				Assert(nullptr == head, "This shouldn't happen");
				head = span.data();
			});
			Post(head != nullptr, "Allocation failed");

			iterator it_rebalance;
			auto const logN = (int)std::bit_width(count); // why is this unsigned in libstdc++?
			for (int i = 0; auto val : range) {
				std::construct_at(&head[i], node{{&head[i + 1], &head[i + 1]}, val});

				i += 1;

				if (i == logN) {
					// The rebalancer needs logN nodes before it can start.
					it_rebalance = iterator{head, count};
				} else if (i > logN) {
					++it_rebalance;
				}
			}
			head[count - 1].next[0] = nullptr;
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

		constexpr void insert_after(iterator pos, T val) {
		}

		constexpr void remove(T val) {
			erase(find(val));
		}

		constexpr void erase(iterator it) {
			if (!it)
				return;

			node* n = it.curr;
			node* next = n->next[0];
			alloc.deallocate({n, 1});
			if (it.prev == nullptr) // head
				head = next;
			else {
				if (next == nullptr) // tail
					head->next[1] = it.prev;
				it.prev->next[0] = next;
			}
			count -= 1;
			needs_rebalance = true;
		}

		constexpr void rebalance() {
			if (head && needs_rebalance) {
				balance_helper bh(head, count);
				while (bh)
					bh.balance_current_and_advance();
			}

			needs_rebalance = false;
		}

		constexpr struct iterator find(T const& val) const {
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

		constexpr iterator lower_bound(T const& val) const {
			if (empty())
				return {};
			if (val < head->data)
				return {head, nullptr};
			//if (val > head->next[1]->data)
			//	return {head->next[1]};

			node* prev = head;
			node* curr = head->next[val > head->data];
			Assert(curr != nullptr, "invalid node found");
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
			Assert(true, "");
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
			return true;
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
			auto const iota = std::views::iota(-200, 200);
			power_list<int> list;
			for (int v : iota)
				list.insert(v);
			list.rebalance();
			return list.contains(1);
		}(),
		"Explicit rebalance");

	static_assert(
		[] {
			auto const iota = std::views::iota(-100, 200);
			power_list<int> list;
			for (int v : iota)
				list.insert(v);
			int sum = 0;
			for (int v : list)
				sum += v;
			return sum > 0 && list.contains(1);
		}(),
		"Implicit rebalance");
#endif
} // namespace ecs::detail

#endif // !ECS_DETAIL_GORKING_LIST_H
