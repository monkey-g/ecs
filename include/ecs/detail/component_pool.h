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

		bool operator<(chunk const& other) const {
			return range < other.range;
		}
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

	scatter_allocator<T> alloc_data;
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
			chunks.insert({entity_range::all(), new T[1]});
		}
	}

	component_pool(component_pool const&) = delete;
	component_pool(component_pool&&) = delete;
	component_pool& operator=(component_pool const&) = delete;
	component_pool& operator=(component_pool&&) = delete;
	~component_pool() noexcept override {
		if constexpr (global<T>) {
			Assert(chunks.size() == 1, "Invalid size of global component pool; should be 1.");
			delete [] chunks.front().data;
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

		thread_local auto tls_cached_chunk_index = chunks.begin();
		auto chunk_it = tls_cached_chunk_index;
		if (chunk_it == chunks.end()) [[unlikely]] {
			// Happens when component pools are reset
			chunk_it = chunks.begin();
		}

		// Try the cached chunk index first. This will load 2 chunks into a cache line
		if (!chunk_it->range.contains(id)) {
			// Wasn't found at cached location, so try looking in next chunk.
			// This should result in linear walks being very cheap.
			chunk_it += 1;
			if (chunk_it != chunks.end() && chunk_it->range.contains(id)) {
				tls_cached_chunk_index = chunk_it;
			} else {
				// The id wasn't found in the cached chunks, so do a binary lookup
				auto const range_it = chunks.find({id, id});
				if (range_it != chunks.end() && range_it->range.contains(id)) {
					// cache the index
					chunk_it = range_it;
					tls_cached_chunk_index = chunk_it;
				} else {
					return nullptr;
				}
			}
		}

		// Do the lookup
		auto const offset = chunk_it->range.offset(id);
		return &chunk_it->data[offset];
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
			count += c.range.count();
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
	auto get_entities() const noexcept {
		if (!chunks.empty())
			return chunks.begin();
		else
			return decltype(chunks.begin()){};
	}

	// Returns true if an entity has a component in this pool
	bool has_entity(entity_id const id) const noexcept {
		return has_entity({id, id});
	}

	// Returns true if an entity range has components in this pool
	bool has_entity(entity_range range) const noexcept {
		auto it = chunks.lower_bound({range, nullptr});
		while (it != chunks.end()) {
			if (it->range.first() > range.last())
				return false;

			if (it->range.contains(range))
				return true;

			if (it->range.overlaps(range)) {
				auto const [r, m] = entity_range::remove(range, it->range);
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
		chunks.clear();
		deferred_adds.clear();
		deferred_spans.clear();
		deferred_gen.clear();
		deferred_removes.clear();
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

	void free_all_chunks() {
		if constexpr (!unbound<T>) {
			for (chunk const& c : chunks)
				alloc_data.deallocate({c.data, c.range.count()});
		}
		chunks.clear();
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

		// TODO
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
		auto it_chunk = chunks.begin();
		auto it_rem = removes.begin();

		while (it_chunk != chunks.end() && it_rem != removes.end()) {
			if (it_chunk->range < *it_rem) {
				std::advance(it_chunk, 1);
			} else if (*it_rem < it_chunk->range) {
				++it_rem;
			} else {
				//if (it_chunk->active == *it_rem) {
				if (it_rem->contains(it_chunk->range)) {
					// Delete the chunk and potentially its data
					it_chunk = chunks.erase(it_chunk);
				} else {
					// remove partial range
					auto const [left_range, maybe_split_range] = entity_range::remove(it_chunk->range, *it_rem);

					// Update the range
					it_chunk->range = left_range;

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
