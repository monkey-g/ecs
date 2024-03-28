#ifndef ECS_DETAIL_COMPONENT_POOL_H
#define ECS_DETAIL_COMPONENT_POOL_H

#include <functional>
#include <memory>
#include <vector>
#include <utility>
#include <ranges>

#include "tls/collect.h"

#include "../entity_id.h"
#include "../entity_range.h"
#include "parent_id.h"
#include "tagged_pointer.h"
#include "stride_view.h"
#include "scatter_allocator.h"
#include "power_list.h"

#include "component_pool_base.h"
#include "../flags.h"
#include "options.h"

#ifdef _MSC_VER
#define MSVC msvc::
#else
#define MSVC
#endif

namespace ecs::detail {

template <typename ForwardIt, typename BinaryPredicate>
ForwardIt std_combine_erase(ForwardIt first, ForwardIt last, BinaryPredicate&& p) noexcept {
	if (first == last)
		return last;

	ForwardIt result = first;
	while (++first != last) {
		auto const pred_res = p(*result, *first);
		if (!pred_res && ++result != first) {
			*result = std::move(*first);
		}
	}
	return ++result;
}

template <typename Cont, typename BinaryPredicate>
void combine_erase(Cont& cont, BinaryPredicate&& p) noexcept {
	auto const end = std_combine_erase(cont.begin(), cont.end(), static_cast<BinaryPredicate&&>(p));
	cont.erase(end, cont.end());
}

ECS_EXPORT template <typename T, typename Alloc = std::allocator<T>>
class component_pool final : public component_pool_base {
private:
	static_assert(!is_parent<T>::value, "can not have pools of any ecs::parent<type>");

	struct chunk {
		// The range this chunk covers.
		entity_range range;

		// The data for the full range of the chunk
		T* data;
	};
	static_assert(sizeof(chunk) == 16);

	//
	struct entity_empty {
		entity_range rng;
		entity_empty(entity_range r) noexcept : rng{r} {}
	};
	struct entity_data_member : entity_empty {
		T data;
		entity_data_member(entity_range r, T const& t) noexcept : entity_empty{r}, data(t) {}
		entity_data_member(entity_range r, T&& t) noexcept : entity_empty{r}, data(std::forward<T>(t)) {}
	};
	struct entity_span_member : entity_empty {
		std::span<const T> data;
		entity_span_member(entity_range r, std::span<const T> t) noexcept : entity_empty{r}, data(t) {}
	};
	struct entity_gen_member : entity_empty {
		std::function<T(entity_id)> data;
		entity_gen_member(entity_range r, std::function<T(entity_id)>&& t) noexcept
			: entity_empty{r}, data(std::forward<std::function<T(entity_id)>>(t)) {}
	};

	using entity_data = std::conditional_t<unbound<T>, entity_empty, entity_data_member>;
	using entity_span = std::conditional_t<unbound<T>, entity_empty, entity_span_member>;
	using entity_gen = std::conditional_t<unbound<T>, entity_empty, entity_gen_member>;

	using chunk_iter = typename std::vector<chunk>::iterator;
	using chunk_const_iter = typename std::vector<chunk>::const_iterator;

	array_scatter_allocator<T> data_alloc;
	array_scatter_allocator<chunk> chunk_alloc;
	power_list<chunk> chunks;
	std::vector<component_pool_base*> variants;

	// Status flags
	bool components_added : 1 = false;
	bool components_removed : 1 = false;
	bool components_modified : 1 = false;

	// Keep track of which components to add/remove each cycle
	[[MSVC no_unique_address]] tls::collect<std::vector<entity_data>, std::vector, component_pool<T>> deferred_adds;
	[[MSVC no_unique_address]] tls::collect<std::vector<entity_span>, std::vector, component_pool<T>> deferred_spans;
	[[MSVC no_unique_address]] tls::collect<std::vector<entity_gen>, std::vector, component_pool<T>> deferred_gen;
	[[MSVC no_unique_address]] tls::collect<std::vector<entity_range>, std::vector, component_pool<T>> deferred_removes;
#if ECS_ENABLE_CONTRACTS_AUDIT
	[[MSVC no_unique_address]] tls::unique_collect<std::vector<entity_range>> deferred_variants;
#endif
	[[MSVC no_unique_address]] Alloc alloc;

public:
	component_pool() noexcept {
		if constexpr (global<T>) {
			chunks.emplace_back(entity_range::all(), entity_range::all(), nullptr, true, false);
			chunks.front().data = new T[1];
		}
	}

	component_pool(component_pool const&) = delete;
	component_pool(component_pool&&) = delete;
	component_pool& operator=(component_pool const&) = delete;
	component_pool& operator=(component_pool&&) = delete;
	~component_pool() noexcept override {
		if constexpr (global<T>) {
			delete [] chunks.front().data.pointer();
			chunks.clear();
		} else {
			free_all_chunks();
			deferred_adds.clear();
			deferred_spans.clear();
			deferred_gen.clear();
			deferred_removes.clear();
#if ECS_ENABLE_CONTRACTS_AUDIT
			deferred_variants.clear();
#endif
		}
	}

	// Add a span of component to a range of entities
	// Pre: entities has not already been added, or is in queue to be added
	//      This condition will not be checked until 'process_changes' is called.
	// Pre: range and span must be same size.
	void add_span(entity_range const range, std::span<const T> span) noexcept requires(!detail::unbound<T>) {
		Pre(range.count() == std::ssize(span), "range and span must be same size");
		remove_from_variants(range);
		// Add the range and function to a temp storage
		deferred_spans.local().emplace_back(range, span);
	}

	// Add a component to a range of entities, initialized by the supplied user function generator
	// Pre: entities has not already been added, or is in queue to be added
	//      This condition will not be checked until 'process_changes' is called.
	template <typename Fn>
	void add_generator(entity_range const range, Fn&& gen) {
		remove_from_variants(range);
		// Add the range and function to a temp storage
		deferred_gen.local().emplace_back(range, std::forward<Fn>(gen));
	}

	// Add a component to a range of entity.
	// Pre: entities has not already been added, or is in queue to be added
	//      This condition will not be checked until 'process_changes' is called.
	void add(entity_range const range, T&& component) noexcept {
		remove_from_variants(range);
		if constexpr (tagged<T>) {
			deferred_adds.local().emplace_back(range);
		} else {
			deferred_adds.local().emplace_back(range, std::forward<T>(component));
		}
	}

	// Add a component to a range of entity.
	// Pre: entities has not already been added, or is in queue to be added
	//      This condition will not be checked until 'process_changes' is called.
	void add(entity_range const range, T const& component) noexcept {
		remove_from_variants(range);
		if constexpr (tagged<T>) {
			deferred_adds.local().emplace_back(range);
		} else {
			deferred_adds.local().emplace_back(range, component);
		}
	}

	// Adds a variant to this component pool
	void add_variant(component_pool_base* variant) {
		Pre(nullptr != variant, "variant can not be null");
		if (std::ranges::find(variants, variant) == variants.end())
			variants.push_back(variant);
	}

	// Return the shared component
	T& get_shared_component() noexcept requires global<T> {
		return chunks.front().data[0];
	}

	// Remove an entity from the component pool.
	void remove(entity_id const id) noexcept {
		remove({id, id});
	}

	// Remove an entity from the component pool.
	void remove(entity_range const range) noexcept {
		deferred_removes.local().push_back(range);
	}

	// Returns an entities component.
	// Returns nullptr if the entity is not found in this pool
	T* find_component_data(entity_id const id) noexcept requires(!global<T>) {
		return const_cast<T*>(std::as_const(*this).find_component_data(id));
	}

	// Returns an entities component.
	// Returns nullptr if the entity is not found in this pool
	T const* find_component_data(entity_id const id) const noexcept requires(!global<T>) {
		if (chunks.empty())
			return nullptr;

		thread_local std::size_t tls_cached_chunk_index = 0;
		auto chunk_index = tls_cached_chunk_index;
		if (chunk_index >= std::size(chunks)) [[unlikely]] {
			// Happens when component pools are reset
			chunk_index = 0;
		}

		// Try the cached chunk index first. This will load 2 chunks into a cache line
		if (!chunks[chunk_index].active.contains(id)) {
			// Wasn't found at cached location, so try looking in next chunk.
			// This should result in linear walks being very cheap.
			if ((1+chunk_index) != std::size(chunks) && chunks[1+chunk_index].active.contains(id)) {
				chunk_index += 1;
				tls_cached_chunk_index = chunk_index;
			} else {
				// The id wasn't found in the cached chunks, so do a binary lookup
				auto const range_it = find_in_ordered_active_ranges({id, id});
				if (range_it != chunks.cend() && range_it->active.contains(id)) {
					// cache the index
					chunk_index = static_cast<std::size_t>(ranges_dist(range_it));
					tls_cached_chunk_index = chunk_index;
				} else {
					return nullptr;
				}
			}
		}

		// Do the lookup
		auto const offset = chunks[chunk_index].range.offset(id);
		return &chunks[chunk_index].data[offset];
	}

	// Merge all the components queued for addition to the main storage,
	// and remove components queued for removal
	void process_changes() override {
		if constexpr (!global<T>) {
			process_remove_components();
			process_add_components();
		}
	}

	// Returns the number of active entities in the pool
	ptrdiff_t num_entities() const noexcept {
		ptrdiff_t count = 0;

		for (chunk const& c : chunks) {
			count += c.active.count();
		}

		return count;
	}

	// Returns the number of active components in the pool
	ptrdiff_t num_components() const noexcept {
		if constexpr (unbound<T>)
			return 1;
		else
			return num_entities();
	}

	// Returns the number of chunks in use
	ptrdiff_t num_chunks() const noexcept {
		return std::ssize(chunks);
	}

	chunk_const_iter get_head_chunk() const noexcept {
		return chunks.begin();
	}

	// Clears the pools state flags
	void clear_flags() noexcept override {
		components_added = false;
		components_removed = false;
		components_modified = false;
	}

	// Returns true if components has been added since last clear_flags() call
	bool has_more_components() const noexcept {
		return components_added;
	}

	// Returns true if components has been removed since last clear_flags() call
	bool has_less_components() const noexcept {
		return components_removed;
	}

	// Returns true if components has been added/removed since last clear_flags() call
	bool has_component_count_changed() const noexcept {
		return components_added || components_removed;
	}

	bool has_components_been_modified() const noexcept {
		return has_component_count_changed() || components_modified;
	}

	// Returns the pools entities
	stride_view<sizeof(chunk), entity_range const> get_entities() const noexcept {
		if (!chunks.empty())
			return {&chunks[0].active, chunks.size()};
		else
			return {};
	}

	// Returns true if an entity has a component in this pool
	bool has_entity(entity_id const id) const noexcept {
		return has_entity({id, id});
	}

	// Returns true if an entity range has components in this pool
	bool has_entity(entity_range range) const noexcept {
		auto it = find_in_ordered_active_ranges(range);
		while (it != chunks.end()) {
			if (it->range.first() > range.last())
				return false;

			if (it->active.contains(range))
				return true;

			if (it->active.overlaps(range)) {
				auto const [r, m] = entity_range::remove(range, it->active);
				if (m)
					return false;
				range = r;
			}

			std::advance(it, 1);
		}

		return false;
	}

	// Clear all entities from the pool
	void clear() noexcept override {
		// Remember if components was removed from the pool
		bool const is_removed = (!chunks.empty());

		// Clear all data
		free_all_chunks();
		deferred_adds.clear();
		deferred_spans.clear();
		deferred_gen.clear();
		deferred_removes.clear();
		chunks.clear();
		clear_flags();

		// Save the removal state
		components_removed = is_removed;
	}

	// Flag that components has been modified
	void notify_components_modified() noexcept {
		components_modified = true;
	}

	// Called from other component pools
	void remove_variant(entity_range const& range) noexcept override {
		deferred_removes.local().push_back(range);
#if ECS_ENABLE_CONTRACTS_AUDIT
		deferred_variants.local().push_back(range);
#endif
	}

private:
	template <typename U>
	static bool ensure_no_intersection_ranges(std::vector<entity_range> const& a, std::vector<U> const& b) {
		auto it_a_curr = a.begin();
		auto it_b_curr = b.begin();
		auto const it_a_end = a.end();
		auto const it_b_end = b.end();

		while (it_a_curr != it_a_end && it_b_curr != it_b_end) {
			if (it_a_curr->overlaps(it_b_curr->rng)) {
				return false;
			}

			if (it_a_curr->last() < it_b_curr->rng.last()) { // range a is inside range b, move to
															 // the next range in a
				++it_a_curr;
			} else if (it_b_curr->rng.last() < it_a_curr->last()) { // range b is inside range a,
																	// move to the next range in b
				++it_b_curr;
			} else { // ranges are equal, move to next ones
				++it_a_curr;
				++it_b_curr;
			}
		}

		return true;
	}

	chunk_iter create_new_chunk(chunk_iter it_loc, entity_range const range, entity_range const active, T* data = nullptr,
								bool owns_data = true, bool split_data = false) noexcept {
		Pre(range.contains(active), "active range is not contained in the total range");
		return chunks.emplace(it_loc, range, active, data, owns_data, split_data);
	}

	template <typename U>
	chunk_iter create_new_chunk(chunk_iter loc, typename std::vector<U>::const_iterator const& iter) noexcept {
		entity_range const r = iter->rng;
		chunk_iter c = create_new_chunk(loc, r, r);
		if constexpr (!unbound<T>) {
			c->data = alloc.allocate(r.ucount());
			construct_range_in_chunk(c, r, iter->data);
		}

		return c;
	}

	void free_chunk_data(chunk_iter c) noexcept {
		// Check for potential ownership transfer
		if (c->get_owns_data()) {
			auto next = std::next(c);
			if (c->get_has_split_data() && next != chunks.end()) {
				Assert(c->range == next->range, "ranges must be equal");
				// transfer ownership
				next->set_owns_data(true);
			} else {
				if constexpr (!unbound<T>) {
					// Destroy active range
					std::destroy_n(c->data.pointer(), c->active.ucount());

					// Free entire range
					alloc.deallocate(c->data.pointer(), c->range.ucount());

					// Debug
					c->data.clear();
				}
			}
		}
	}

	[[nodiscard]]
	chunk_iter free_chunk(chunk_iter c) noexcept {
		free_chunk_data(c);
		return remove_range_to_chunk(c);
	}

	void free_all_chunks() noexcept {
		if constexpr (!global<T>) {
			auto chunk_it = chunks.begin();
			while (chunk_it != chunks.end()) {
				free_chunk_data(chunk_it);
				std::advance(chunk_it, 1);
			}
			chunks.clear();
			set_data_removed();
		}
	}

	auto find_in_ordered_active_ranges(entity_range const rng) const noexcept {
		return std::ranges::lower_bound(chunks, rng, std::less{}, &chunk::active);
	}

	ptrdiff_t ranges_dist(typename std::vector<chunk>::const_iterator it) const noexcept {
		return std::distance(chunks.begin(), it);
	}

	// Removes a range and chunk from the map
	[[nodiscard]]
	chunk_iter remove_range_to_chunk(chunk_iter it) noexcept {
		return chunks.erase(it);
	}

	// Remove a range from the variants
	void remove_from_variants(entity_range const range) {
		for (component_pool_base* variant : variants) {
			variant->remove_variant(range);
		}
	}

	// Flag that components has been added
	void set_data_added() noexcept {
		components_added = true;
	}

	// Flag that components has been removed
	void set_data_removed() noexcept {
		components_removed = true;
	}

	static bool is_equal(T const& lhs, T const& rhs) noexcept requires std::equality_comparable<T> {
		return lhs == rhs;
	}
	static bool is_equal(T const& /*lhs*/, T const& /*rhs*/) noexcept requires tagged<T> {
		// Tags are empty, so always return true
		return true;
	}
	static bool is_equal(T const&, T const&) noexcept {
		// Type can not be compared, so always return false.
		// memcmp is a no-go because it also compares padding in types,
		// and it is not constexpr
		return false;
	}

	template <typename Data>
	void construct_range_in_chunk(chunk_iter c, entity_range range, Data const& comp_data) noexcept requires(!unbound<T>) {
		// Offset into the chunks data
		auto const ent_offset = c->range.offset(range.first());

		entity_id ent = range.first();
		for (size_t i = 0; i < range.ucount(); ++i, ++ent) {
			// Construct from a value or a a span of values
			if constexpr (std::is_same_v<T, Data>) {
				std::construct_at(&c->data[ent_offset + i], comp_data);
			} else if constexpr (std::is_invocable_v<Data, entity_id>) {
				std::construct_at(&c->data[ent_offset + i], comp_data(ent));
			} else {
				std::construct_at(&c->data[ent_offset + i], comp_data[i]);
			}
		}
	}

	void fill_data_in_existing_chunk(chunk_iter& curr, entity_range r) noexcept {
		auto next = std::next(curr);

		// If split chunks are encountered, skip forward to the chunk closest to r
		if (curr->get_has_split_data()) {
			while (chunks.end() != next && next->range.contains(r) && next->active < r) {
				std::advance(curr, 1);
				next = std::next(curr);
			}
		}

		if (curr->active.adjacent(r) && curr->range.contains(r)) {
			// The two ranges are next to each other, so add the data to existing chunk
			entity_range active_range = entity_range::merge(curr->active, r);
			curr->active = active_range;

			// Check to see if this chunk can be collapsed into 'prev'
			if (chunks.begin() != curr) {
				auto prev = std::next(curr, -1);
				if (prev->active.adjacent(curr->active)) {
					active_range = entity_range::merge(prev->active, curr->active);
					prev = remove_range_to_chunk(prev);
					prev->active = active_range;

					curr = free_chunk(curr);
					next = std::next(curr);
				}
			}

			// Check to see if 'next' can be collapsed into this chunk
			if (chunks.end() != next) {
				if (curr->active.adjacent(next->active)) {
					active_range = entity_range::merge(curr->active, next->active);
					next = free_chunk(next);

					curr->active = active_range;

					// split_data is true if the next chunk is also in the current range
					curr->set_has_split_data((next != chunks.end()) && (curr->range == next->range));
				}
			}
		} else {
			// There is a gap between the two ranges, so split the chunk
			if (r < curr->active) {
				bool const curr_owns_data = curr->get_owns_data();
				curr->set_owns_data(false);
				curr = create_new_chunk(curr, curr->range, r, curr->data, curr_owns_data, true);
			} else {
				curr->set_has_split_data(true);
				curr = create_new_chunk(std::next(curr), curr->range, r, curr->data, false, false);
			}
		}
	}

	// Try to combine two ranges. With data
	static bool combiner_bound(entity_data& a, entity_data const& b) requires(!unbound<T>) {
		if (a.rng.adjacent(b.rng) && is_equal(a.data, b.data)) {
			a.rng = entity_range::merge(a.rng, b.rng);
			return true;
		} else {
			return false;
		}
	}

	// Try to combine two ranges. Without data
	static bool combiner_unbound(entity_data& a, entity_data const& b) requires(unbound<T>) {
		if (a.rng.adjacent(b.rng)) {
			a.rng = entity_range::merge(a.rng, b.rng);
			return true;
		} else {
			return false;
		}
	}

	template <typename U>
	void process_add_components(std::vector<U>& vec) {
		if (vec.empty()) {
			return;
		}

		// Do the insertions
		auto iter = vec.begin();
		auto curr = chunks.begin();

		// Fill in values
		while (iter != vec.end()) {
			if (chunks.empty()) {
				curr = create_new_chunk<U>(curr, iter);
			} else {
				entity_range const r = iter->rng;

				// Move current chunk iterator forward
				auto next = std::next(curr);
				while (chunks.end() != next && next->range < r) {
					curr = next;
					std::advance(next, 1);
				}

				if (curr->range.overlaps(r)) {
					// Delayed pre-condition check: Can not add components more than once to same entity
					Pre(!curr->active.overlaps(r), "entity already has a component of the type");

					if (!curr->active.overlaps(r)) {
						// The incoming range overlaps an unused area in the current chunk

						// split the current chunk and fill in the data
						entity_range const active_range = entity_range::intersect(curr->range, r);
						fill_data_in_existing_chunk(curr, active_range);
						if constexpr (!unbound<T>) {
							construct_range_in_chunk(curr, active_range, iter->data);
						}

						if (active_range != r) {
							auto const [remainder, x] = entity_range::remove(active_range, r);
							Assert(!x.has_value(), "internal: there should not be a range here; create an issue on Github and investigate");
							iter->rng = remainder;
							continue;
						}
					} else {
						// Incoming range overlaps the current one, so add it into 'curr'
						fill_data_in_existing_chunk(curr, r);
						if constexpr (!unbound<T>) {
							construct_range_in_chunk(curr, r, iter->data);
						}
					}
				} else if (curr->range < r) {
					// Incoming range is larger than the current one, so add it after 'curr'
					curr = create_new_chunk<U>(std::next(curr), iter);
					// std::advance(curr, 1);
				} else if (r < curr->range) {
					// Incoming range is less than the current one, so add it before 'curr' (after 'prev')
					curr = create_new_chunk<U>(curr, iter);
				}
			}

			std::advance(iter, 1);
		}
	}

	// Add new queued entities and components to the main storage.
	void process_add_components() {
#if ECS_ENABLE_CONTRACTS_AUDIT
		std::vector<entity_range> vec_variants;
		deferred_variants.gather_flattened(std::back_inserter(vec_variants));
		std::sort(vec_variants.begin(), vec_variants.end());
#endif

		auto const adder = [&]<typename C>(std::vector<C>& vec) noexcept(false) {
			if (vec.empty())
				return;

			// Sort the input(s)
			auto const comparator = [](entity_empty const& l, entity_empty const& r) {
				return l.rng < r.rng;
			};
			std::sort(vec.begin(), vec.end(), comparator);

			// Merge adjacent ranges that has the same data
			if constexpr (std::is_same_v<entity_data*, decltype(vec.data())>) {
				if constexpr (unbound<T>)
					combine_erase(vec, combiner_unbound);
				else
					combine_erase(vec, combiner_bound);
			}

			PreAudit(ensure_no_intersection_ranges(vec_variants, vec),
				"Two variants have been added at the same time");

			this->process_add_components(vec);
			vec.clear();

			// Update the state
			set_data_added();
		};

		deferred_adds.for_each(adder);
		deferred_adds.clear();

		deferred_spans.for_each(adder);
		deferred_spans.clear();

		deferred_gen.for_each(adder);
		deferred_gen.clear();
	}

	// Removes the entities and components
	void process_remove_components() noexcept {
		deferred_removes.for_each([this](std::vector<entity_range>& vec) {
			if (vec.empty())
				return;

			// Sort the ranges to remove
			std::sort(vec.begin(), vec.end());

			// Remove the ranges
			this->process_remove_components(vec);

			// Update the state
			set_data_removed();
		});
		deferred_removes.clear();
	}

	void process_remove_components(std::vector<entity_range>& removes) noexcept {
		chunk_iter it_chunk = chunks.begin();
		auto it_rem = removes.begin();

		while (it_chunk != chunks.end() && it_rem != removes.end()) {
			if (it_chunk->active < *it_rem) {
				std::advance(it_chunk, 1);
			} else if (*it_rem < it_chunk->active) {
				++it_rem;
			} else {
				//if (it_chunk->active == *it_rem) {
				if (it_rem->contains(it_chunk->active)) {
					// Delete the chunk and potentially its data
					it_chunk = free_chunk(it_chunk);
				} else {
					// remove partial range
					auto const [left_range, maybe_split_range] = entity_range::remove(it_chunk->active, *it_rem);

					// Update the active range
					it_chunk->active = left_range;

					// Destroy the removed components
					if constexpr (!unbound<T>) {
						auto const offset = it_chunk->range.offset(it_rem->first());
						std::destroy_n(&it_chunk->data[offset], it_rem->ucount());
					}

					if (maybe_split_range.has_value()) {
						// If two ranges were returned, split this chunk
						it_chunk->set_has_split_data(true);
						it_chunk = create_new_chunk(std::next(it_chunk), it_chunk->range, maybe_split_range.value(), it_chunk->data, false);
					} else {
						std::advance(it_chunk, 1);
					}
				}
			}
		}
	}

	// Removes transient components
	void process_remove_components() noexcept requires transient<T> {
		// All transient components are removed each cycle
		free_all_chunks();
	}
};
} // namespace ecs::detail

#endif // !ECS_DETAIL_COMPONENT_POOL_H
