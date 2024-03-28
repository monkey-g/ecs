#ifndef ECS_DETAIL_ARRAY_SCATTER_ALLOCATOR_H
#define ECS_DETAIL_ARRAY_SCATTER_ALLOCATOR_H

#include "contract.h"
#include <algorithm>
#include <memory>
#include <span>

namespace ecs::detail {
	template <typename Fn, typename T>
	concept callback_takes_a_span = std::invocable<Fn, std::span<T>>;

	// the 'Scatter Allocator'.
	// * A single allocation can result in many addresses being returned, as the
	//   allocator fills in holes in the internal pools of memory.
	// * It is *not* thread safe.
	// * Deallocated memory is reused before new memory is taken from pools.
	//   This way old pools will be filled with new data before newer pools are tapped.
	//   Filling it 'from the back' like this should keep fragmentation down.
	template <typename T, std::size_t DefaultStartingSize = 16>
	struct scatter_allocator {
		static_assert(DefaultStartingSize > 0);

		constexpr scatter_allocator() noexcept = default;
		constexpr scatter_allocator(scatter_allocator const&) noexcept = delete;
		constexpr scatter_allocator(scatter_allocator&& other) noexcept {
			pools.swap(other.pools);
			free_list.swap(other.free_list);
		}

		constexpr std::vector<std::span<T>> allocate(std::size_t const count) {
			std::vector<std::span<T>> r;
			allocate_with_callback(count, [&r](std::span<T> s) {
				r.push_back(s);
			});
			return r;
		}

		constexpr T* allocate_one() {
			T* t{};
			allocate_with_callback(1, [&t](std::span<T> s) {
				Assert(t == nullptr, "Something has gone terribly wrong");
				Assert(s.size() == 1, "Something has gone terribly wrong");
				t = s.data();
			});
			return t;
		}

		constexpr void allocate_with_callback(std::size_t const count, callback_takes_a_span<T> auto&& alloc_callback) {
			std::size_t remaining_count = count;

			// Take space from free list
			std::unique_ptr<free_block>* ptr_free = &free_list;
			while (*ptr_free) {
				free_block* ptr = ptr_free->get();
				std::size_t const min_space = std::min(remaining_count, ptr->span.size());
				if (min_space == 0) {
					ptr_free = &ptr->next;
					continue;
				}

				std::span<T> span = ptr->span.subspan(0, min_space);
				alloc_callback(span);

				remaining_count -= min_space;
				if (remaining_count == 0)
					return;

				if (min_space == (int)ptr->span.size()) {
					auto next = std::move(ptr->next);
					*ptr_free = std::move(next);
				} else {
					ptr->span = ptr->span.subspan(min_space + 1);
					ptr_free = &ptr->next;
				}
			}

			// Take space from pools
			pool* ptr_pool = pools.get();
			while (remaining_count > 0) {
				if (ptr_pool == nullptr) {
					auto const next_pow2_size = std::size_t{1} << std::bit_width(remaining_count);
					ptr_pool = add_pool(pools ? pools->capacity << 1 : std::max(next_pow2_size, DefaultStartingSize));
				}

				pool& p = *ptr_pool;
				ptr_pool = ptr_pool->next.get();

				std::size_t const min_space = std::min({remaining_count, (p.capacity - p.next_available)});
				if (min_space == 0)
					continue;

				std::span<T> span{p.base.get() + p.next_available, min_space};
				alloc_callback(span);

				p.next_available += min_space;
				remaining_count -= min_space;
			}
		}

		constexpr void deallocate(std::span<T> const span) {
			PreAudit(validate_addr(span), "Invalid address passed to deallocate()");
			free_list = std::make_unique<free_block>(std::move(free_list), span);
		}

	private:
		constexpr auto* add_pool(std::size_t const size) {
			pools = std::make_unique<pool>(std::move(pools), std::make_unique_for_overwrite<T[]>(size), 0, size);
			return pools.get();
		}

		static constexpr bool valid_addr(T* p, T* begin, T* end) {
			// It is undefined behavior to compare pointers directly,
			// so use distances instead. This also works at compile time.
			auto const size = std::distance(begin, end);
			return std::distance(begin, p) >= 0 && std::distance(p, end) <= size;
		}

		constexpr bool validate_addr(std::span<T> const span) {
			for (auto* p = pools.get(); p; p = p->next.get()) {
				if (valid_addr(span.data(), p->base.get(), p->base.get() + p->capacity))
					return true;
			}
			return false;
		}

		struct free_block {
			std::unique_ptr<free_block> next;
			std::span<T> span;
		};

		struct pool {
			std::unique_ptr<pool> next;
			std::unique_ptr<T[]> base;
			std::size_t next_available;
			std::size_t capacity;
		};

		std::unique_ptr<pool> pools;
		std::unique_ptr<free_block> free_list;
	};

	// UNIT TESTS
	static_assert(
		[] {
			constexpr std::size_t elems_to_alloc = 123;
			scatter_allocator<int> alloc;
			std::size_t total_alloc = 0;
			alloc.allocate_with_callback(elems_to_alloc, [&](std::span<int> s) {
				total_alloc += s.size();
			});
			return elems_to_alloc == total_alloc;
		}(),
		"Array-scatter allocator allocates correctly");

	static_assert(
		[] {
			scatter_allocator<int> alloc;
			std::vector<std::span<int>> r = alloc.allocate(10);
			auto const subspan = r[0].subspan(3, 4);
			alloc.deallocate(subspan);
			return true;
		}(),
		"Array-scatter allocator frees correctly");

	static_assert(
		[] {
			scatter_allocator<int, 16> alloc;
			auto vec = alloc.allocate(10);
			alloc.deallocate(vec[0].subspan(2, 2));
			alloc.deallocate(vec[0].subspan(4, 2));

			// Fills in the two holes (2+2), the rest of the first pool (6),
			// and remaining in new second pool (10)
			int count = 0;
			std::size_t sizes[] = {2, 2, 6, 10};
			alloc.allocate_with_callback(20, [&](auto span) {
				Assert(sizes[count] == span.size(), "unexpected span size");
				count += 1;
			});
			return (count == 4);
		}(),
		"Array-scatter allocator scatters correctly");
} // namespace ecs::detail

#endif // !ECS_DETAIL_ARRAY_SCATTER_ALLOCATOR_H
