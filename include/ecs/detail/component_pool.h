#ifndef ECS_COMPONENT_POOL
#define ECS_COMPONENT_POOL

#include <cstring> // for memcmp
#include <execution>
#include <functional>
#include <memory_resource>
#include <tuple>
#include <type_traits>
#include <vector>

#include "tls/collect.h"

#include "../entity_id.h"
#include "../entity_range.h"
#include "parent_id.h"

#include "component_pool_base.h"
#include "flags.h"
#include "options.h"

// Helpre macro for components that wish to support pmr.
// Declares the 'allocator_type' and default constructor/assignment
#define ECS_USE_PMR(ClassName)                                                                                                             \
	using allocator_type = std::pmr::polymorphic_allocator<>;                                                                              \
                                                                                                                                           \
	ClassName() : ClassName(allocator_type{}) {}                                                                                           \
	ClassName(ClassName const&) = default;                                                                                                 \
	ClassName(ClassName&&) = default;                                                                                                      \
	~ClassName() = default;                                                                                                                \
                                                                                                                                           \
	ClassName& operator=(ClassName const&) = default;                                                                                      \
	ClassName& operator=(ClassName&&) = default


namespace ecs::detail {

template <class ForwardIt, class BinaryPredicate>
ForwardIt std_combine_erase(ForwardIt first, ForwardIt last, BinaryPredicate p) {
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

template <class Cont, class BinaryPredicate>
void combine_erase(Cont& cont, BinaryPredicate p) {
	auto const end = std_combine_erase(cont.begin(), cont.end(), p);
	cont.erase(end, cont.end());
}


template <typename T>
class component_pool final : public component_pool_base {
private:
	static_assert(!is_parent<T>::value, "can not have pools of any ecs::parent<type>");

	using clock = std::chrono::steady_clock;
	using time_point = std::chrono::time_point<clock>;

	struct Tier0 {
		entity_range range;
		std::vector<T> data;
	};
	struct Tier1 {
		static constexpr std::chrono::seconds time_to_upgrade{45};

		entity_range range;  // the total range allocated
		entity_range active; // the range in use
		std::vector<T> data;
		time_point last_modified; // modified here means back/front insertion/deletion
	};
	struct Tier2 {
		static constexpr std::chrono::seconds time_to_upgrade{15};

		entity_range range;
		std::vector<uint32_t> skips;
		std::vector<T> data;
		time_point last_modified;

		void init_skips(entity_range r1, entity_range r2) {
			//     r1   r2
			// ---***---**--- range
			// #1    #2   #3

			// part 1
			auto count = r1.first() - range.first();
			auto it = skips.begin();
			while (count > 0) {
				*it = count;
				++it;
				--count;
			}

			// part 2
			count = r2.first() - r1.last() - 1;
			it = skips.begin() + r1.last() + 1;
			while (count > 0) {
				*it = count;
				++it;
				--count;
			}

			// part 3
			count = range.last() - r2.last();
			it = skips.begin() + r2.last() + 1;
			while (count > 0) {
				*it = count;
				++it;
				--count;
			}
		}
	};

	// The component tiers
	std::vector<Tier0> t0;
	std::vector<Tier1> t1;
	std::vector<Tier2> t2;

	// The total range of entities in the tiers. Ranges are only valid if the tier is not empty.
	// Used for broad-phase testing
	entity_range t0_range{entity_range::all()};
	entity_range t1_range{entity_range::all()};
	entity_range t2_range{entity_range::all()};

	// Cache of all active ranges
	std::vector<entity_range> cached_ranges;

	// The alloctor
	std::pmr::polymorphic_allocator<T> allocator;

	// Keep track of which components to add/remove each cycle
	using entity_data = std::conditional_t<unbound<T>, std::tuple<entity_range>, std::tuple<entity_range, T>>;
	using entity_init = std::tuple<entity_range, std::function<T(entity_id)>>;
	tls::collect<std::vector<entity_data>, component_pool<T>> deferred_adds;
	tls::collect<std::vector<entity_init>, component_pool<T>> deferred_inits;
	tls::collect<std::vector<entity_range>, component_pool<T>> deferred_removes;

	// Status flags
	bool components_added = false;
	bool components_removed = false;
	bool components_modified = false;

public:
	// Returns the current memory resource
	std::pmr::memory_resource* get_memory_resource() const {
		return allocator.resource();
	}

	// Sets the memory resource used to allocate components.
	// If components are already allocated, they will be moved.
	void set_memory_resource(std::pmr::memory_resource* /*resource*/) {
		return;
		// Do nothing if the memory resource is already set
		//if (allocator.resource() == resource)
		//	return;

		// Move the current data out
		//auto copy{std::move(components)};

		// Destroy the current container
		//std::destroy_at(&components);

		// Placement-new the data back with the new memory resource
		//std::construct_at(&components, std::move(copy), resource);

		// component addresses has changed, so make sure systems rebuilds their caches
		//components_added = true;
		//components_removed = true;
		//components_modified = true;
	}

	// Add a component to a range of entities, initialized by the supplied user function
	// Pre: entities has not already been added, or is in queue to be added
	//      This condition will not be checked until 'process_changes' is called.
	template <typename Fn>
	void add_init(entity_range const range, Fn&& init) {
		// Add the range and function to a temp storage
		deferred_inits.local().emplace_back(range, std::forward<Fn>(init));
	}

	// Add a component to a range of entity.
	// Pre: entities has not already been added, or is in queue to be added
	//      This condition will not be checked until 'process_changes' is called.
	void add(entity_range const range, T&& component) {
		if constexpr (tagged<T>) {
			deferred_adds.local().push_back(range);
		} else {
			deferred_adds.local().emplace_back(range, std::forward<T>(component));
		}
	}

	// Add a component to a range of entity.
	// Pre: entities has not already been added, or is in queue to be added
	//      This condition will not be checked until 'process_changes' is called.
	void add(entity_range const range, T const& component) {
		if constexpr (tagged<T>) {
			deferred_adds.local().push_back(range);
		} else {
			deferred_adds.local().emplace_back(range, component);
		}
	}

	// Return the shared component
	T& get_shared_component() requires unbound<T> {
		static T t{};
		return t;
	}

	// Remove an entity from the component pool. This logically removes the component from the
	// entity.
	void remove(entity_id const id) {
		remove_range({id, id});
	}

	// Remove an entity from the component pool. This logically removes the component from the
	// entity.
	void remove_range(entity_range const range) {
		deferred_removes.local().push_back(range);
	}

	// Returns an entities component.
	// Returns nullptr if the entity is not found in this pool
	T* find_component_data(entity_id const id) {
		return const_cast<T*>(std::as_const(*this).find_component_data(id));
	}

	// Returns an entities component.
	// Returns nullptr if the entity is not found in this pool
	T const* find_component_data(entity_id const id) const {
		entity_range const r{id, id};

		// search in tier 0
		if (!t0.empty()) {
			auto const it = std::ranges::lower_bound(t0, r, std::less{}, &Tier0::range);
			if (it != t0.end() && it->range.contains(id)) {
				auto const offset = it->range.offset(id);
				return &it->data[offset];
			}
		}

		// search in tier 1
		if (!t1.empty()) {
			auto const it = std::ranges::lower_bound(t1, r, std::less{}, &Tier1::range);
			if (it != t1.end() && it->active.contains(id)) {
				auto const offset = it->range.offset(id);
				return &it->data[offset];
			}
		}

		// search in tier 2
		if (!t2.empty()) {
			auto const it = std::ranges::lower_bound(t2, r, std::less{}, &Tier2::range);
			if (it != t2.end() && it->range.contains(id)) {
				auto const offset = it->range.offset(id);
				if (it->skips[offset] == 0)
					return &it->data[offset];
			}
		}

		return nullptr;
	}

	// Merge all the components queued for addition to the main storage,
	// and remove components queued for removal
	void process_changes() override {
		process_remove_components();

		size_t const t0_size_start = t0.size();
		size_t const t2_size_start = t2.size();

		process_add_components();
		handle_tier_upgrades();
		// TODO? collapse_adjacent_ranges()

		// Sort it
		// New ranges are never added to t1, but existing t1
		// can be downgraded to t2.
		if (t0_size_start == 0 && !t0.empty()) {
			std::ranges::sort(t0, std::less{}, &Tier0::range);
		} else if (t0_size_start != t0.size()) {
			auto const middle = t0.begin() + t0_size_start;

			// sort the newcomers
			std::ranges::sort(middle, t0.end(), std::less{}, &Tier0::range);

			// merge them into the rest
			std::ranges::inplace_merge(t0, middle, std::less{}, &Tier0::range);
		}
		if (t2_size_start == 0 && !t2.empty()) {
			std::ranges::sort(t2, std::less{}, &Tier2::range);
		} else if (t2_size_start != t2.size()) {
			auto const middle = t2.begin() + t2_size_start;

			// sort the newcomers
			std::ranges::sort(middle, t2.end(), std::less{}, &Tier2::range);

			// merge them into the rest
			std::ranges::inplace_merge(t2, middle, std::less{}, &Tier2::range);
		}

		update_cached_ranges();
	}

	// Returns the number of active entities in the pool
	size_t num_entities() const {
		size_t count = 0;
		for (Tier0 const& t : t0)
			count += t.range.ucount();
		for (Tier1 const& t : t1)
			count += t.active.ucount();
		for (Tier2 const& t : t2)
			for (auto skip : t.skips)
				count += (skip == 0);
		return count;
	}

	// Returns the number of active components in the pool
	size_t num_components() const {
		if constexpr (unbound<T>)
			return 1;
		else
			return num_entities();
	}

	// Clears the pools state flags
	void clear_flags() override {
		components_added = false;
		components_removed = false;
		components_modified = false;
	}

	// Returns true if components has been added since last clear_flags() call
	bool has_more_components() const {
		return components_added;
	}

	// Returns true if components has been removed since last clear_flags() call
	bool has_less_components() const {
		return components_removed;
	}

	// Returns true if components has been added/removed since last clear_flags() call
	bool has_component_count_changed() const {
		return components_added || components_removed;
	}

	bool has_components_been_modified() const {
		return has_component_count_changed() || components_modified;
	}

	// Returns the pools entities
	entity_range_view get_entities() const {
		if constexpr (detail::global<T>) {
			// globals are accessible to all entities
			static constexpr entity_range global_range{std::numeric_limits<ecs::detail::entity_type>::min(),
													   std::numeric_limits<ecs::detail::entity_type>::max()};
			return entity_range_view{&global_range, 1};
		} else {
			return cached_ranges;
		}
	}

	// Returns true if an entity has a component in this pool
	bool has_entity(entity_id const id) const {
		return has_entity({id, id});
	}

	// Returns true if an entity range has components in this pool
	bool has_entity(entity_range const& range) const {
		// search in tier 0
		if (!t0.empty()) {
			auto const it = std::ranges::lower_bound(t0, range, std::less{}, &Tier0::range);
			if (it != t0.end() && it->range.contains(range)) {
				return true;
			}
		}

		// search in tier 1
		if (!t1.empty()) {
			auto const it = std::ranges::lower_bound(t1, range, std::less{}, &Tier1::range);
			if (it != t1.end() && it->active.contains(range)) {
				return true;
			}
		}

		// search in tier 2
		if (!t2.empty()) {
			auto const it = std::ranges::lower_bound(t2, range, std::less{}, &Tier2::range);
			if (it != t2.end() && it->range.contains(range)) {
				auto const offset_start = it->range.offset(range.first());
				auto const offset_end = it->range.offset(range.first());

				auto const tester = [](auto skip) {
					return skip == 0;
				};

				if (std::all_of(&it->skips[offset_start], &it->skips[offset_end], tester))
					return true;
			}
		}

		return false;
	}

	// TODO remove?
	// Checks the current threads queue for the entity
	bool is_queued_add(entity_id const id) {
		return is_queued_add({id, id});
	}

	// Checks the current threads queue for the entity
	bool is_queued_add(entity_range const& range) {
		if (deferred_adds.local().empty()) {
			return false;
		}

		for (auto const& ents : deferred_adds.local()) {
			if (std::get<0>(ents).contains(range)) {
				return true;
			}
		}

		return false;
	}

	// Checks the current threads queue for the entity
	bool is_queued_remove(entity_id const id) {
		return is_queued_remove({id, id});
	}

	// Checks the current threads queue for the entity
	bool is_queued_remove(entity_range const& range) {
		if (deferred_removes.local().empty())
			return false;

		for (auto const& ents : deferred_removes.local()) {
			if (ents.contains(range))
				return true;
		}

		return false;
	}

	// Clear all entities from the pool
	void clear() override {
		// Remember if components was removed from the pool
		bool const is_removed = !t0.empty() || !t1.empty() || !t2.empty();

		// Clear the pool
		t0.clear();
		t1.clear();
		t2.clear();
		deferred_adds.reset();
		deferred_inits.reset();
		deferred_removes.reset();
		clear_flags();

		// Save the removal state
		components_removed = is_removed;
	}

	// Flag that components has been modified
	void notify_components_modified() {
		components_modified = true;
	}

private:
	// Flag that components has been added
	void set_data_added() {
		components_added = true;
	}

	// Flag that components has been removed
	void set_data_removed() {
		components_removed = true;
	}

	void handle_tier_upgrades() {
		auto const now = clock::now();

		for (auto it = t1.begin(); it != t1.end(); ) {
			if (now - it->last_modified >= Tier1::time_to_upgrade) {
				t0.push_back(upgrade_t1_to_t0(*it));
				it = t1.erase(it);
			} else {
				++it;
			}
		}

		// t2
	}

	void update_cached_ranges() {
		if (!components_added && !components_removed)
			return;

		cached_ranges.clear();

		// Find all ranges
		if (!t0.empty()) {
			t0_range = t0[0].range;
			for (size_t i = 0; i < t0.size(); ++i) {
				auto const& tier0 = t0[i];
				t0_range = entity_range::overlapping(t0_range, tier0.range);

				cached_ranges.push_back(tier0.range);
			}
		}

		if (!t1.empty()) {
			t1_range = t1[0].range;
			for (size_t i = 0; i < t1.size(); ++i) {
				auto const& tier1 = t1[i];
				t1_range = entity_range::overlapping(t1_range, tier1.active);

				cached_ranges.push_back(tier1.active);
			}
		}

		if (!t2.empty()) {
			t2_range = t2[0].range;
			for (size_t i = 0; i < t2.size(); ++i) {
				auto const& tier2 = t2[i];
				t2_range = entity_range::overlapping(t2_range, tier2.range);

				auto it = tier2.skips.begin();
				do {
					it += *it;
					if (it == tier2.skips.end())
						break;

					entity_type first = static_cast<entity_type>(tier2.range.first() + std::distance(tier2.skips.begin(), it));

					while (it != tier2.skips.end() && *it == 0) {
						++it;
					}

					entity_id last = static_cast<entity_type>(tier2.range.first() + std::distance(tier2.skips.begin(), it) - 1);
					cached_ranges.push_back({first, last});
				} while (it != tier2.skips.end());
			}
		}
	}

	// Verify the 'add*' functions precondition.
	// An entity can not have more than one of the same component
	static bool has_duplicate_entities(auto const& vec) {
		return vec.end() != std::adjacent_find(vec.begin(), vec.end(),
												[](auto const& l, auto const& r) { return l.range == r.range; });
	};

	static T get_data(entity_id /*id*/, entity_data& ed) {
		return std::get<1>(ed);
	}
	static T get_data(entity_id id, entity_init& ed) {
		return std::get<1>(ed)(id);
	}

	void add_to_t0(entity_range r, entity_data&) requires unbound<T> {
		t0.push_back(Tier0{r, {}});
	}

	void add_to_t0(entity_range r, entity_data& data) {
		t0.push_back(Tier0{r, std::vector<T>(r.ucount(), std::get<1>(data))}); // emplace_back is broken in clang -_-
	}

	void add_to_t0(entity_range r, entity_init& init) {
		std::vector<T> tier_data;
		tier_data.reserve(r.ucount());
		for (entity_id ent : r) {
			tier_data.push_back(std::get<1>(init)(ent));
		}
		t0.push_back(Tier0{r, std::move(tier_data)}); // emplace_back is broken in clang -_-
	}

	// Add new queued entities and components to the main storage.
	void process_add_components() {
		auto const processor = [this](auto& vec) {
			for (auto& data : vec) {
				entity_range const r = std::get<0>(data);

				if (!t1.empty() && t1_range.overlaps(r)) {
					// The new range may exist in a tier 1 range
					auto const it = std::ranges::lower_bound(t1, r, std::less{}, &Tier1::range);
					if (it != t1.end() && it->range.contains(r)) {
						Tier1& tier1 = *it;

						Expects(!tier1.active.overlaps(r)); // entity already has a component

						// If the ranges are adjacent, grow the active range
						if (tier1.active.adjacent(r)) {
							tier1.active = entity_range::merge(tier1.active, r);
							if constexpr (!detail::unbound<T>) {
								for (auto ent = r.first(); ent <= r.last(); ++ent) {
									auto const offset = tier1.range.offset(ent);
									tier1.data[offset] = get_data(ent, data);
								}
							}
						} else {
							// The ranges are separate, so downgrade to T2
							t2.push_back(downgrade_t1_to_t2(std::move(tier1), tier1.active, r));
							t1.erase(it);
						}

						// This range is handled, so continue on to the next one in 'vec'
						continue;
					}
				}

				if (!t2.empty() && t2_range.overlaps(r)) {
					// The new range may exist in a tier 2 range
					auto const it = std::ranges::lower_bound(t2, r, std::less{}, &Tier2::range);
					if (it != t2.end() && it->range.contains(r)) {
						Tier2& tier2 = *it;

						for (auto ent = r.first(); ent <= r.last(); ++ent) {
							auto offset = tier2.range.offset(ent);

							if constexpr (!detail::unbound<T>) {
								tier2.data[offset] = get_data(ent, data);
							}

							auto const current_skips = tier2.skips[offset];
							Expects(tier2.skips[offset] != 0); // entity already has a component
							while (tier2.skips[offset] != 0) {
								tier2.skips[offset] -= current_skips;
								if (offset == 0)
									break;
								offset -= 1;
							}
						}

						// This range is handled, so continue on to the next one in 'vec'y
						continue;
					}
				}

				// range has not been handled yet, so Kobe it into t0
				add_to_t0(r, data);
			}
			vec.clear();
		};

		// Move new components into the pool
		deferred_adds.for_each(processor);
		deferred_inits.for_each(processor);

		// Check it
		Expects(false == has_duplicate_entities(t0));

		// Update the state
		set_data_added();
	}

	// Removes the entities and components
	void process_remove_components() {
		// Collect all the ranges to remove
		std::vector<entity_range> vec;
		deferred_removes.gather_flattened(std::back_inserter(vec));

		// Dip if there is nothing to do
		if (vec.empty()) {
			return;
		}

		// Sort the ranges to remove
		std::ranges::sort(vec, std::ranges::less{}, &entity_range::first);

		// Remove tier 0 ranges, or downgrade them to tier 1 or 2
		if (!t0.empty()) {
			process_remove_components_tier0(vec);
			std::ranges::sort(t0, std::less{}, &Tier0::range);
		}

		// Remove tier 1 ranges, or downgrade them to tier 2
		if (!t1.empty()) {
			process_remove_components_tier1(vec);
			std::ranges::sort(t1, std::less{}, &Tier1::range);
		}

		// Remove tier 2 ranges
		if (!t2.empty()) {
			process_remove_components_tier2(vec);
			std::ranges::sort(t2, std::less{}, &Tier2::range);
		}

		// Update the state
		set_data_removed();
	}

	void process_remove_components_tier0(std::vector<entity_range>& removes) {
		auto it_t0 = t0.begin();
		auto it_rem = removes.begin();

		while (it_t0 != t0.end() && it_rem != removes.end()) {
			if (it_t0->range < *it_rem)
				++it_t0;
			else if (*it_rem < it_t0->range)
				++it_rem;
			else {
				if (it_t0->range == *it_rem) {
					// remove an entire range
					// todo: move to free-store?
				} else {
					// remove partial range
					auto const [left_range, maybe_split_range] = entity_range::remove(it_t0->range, *it_rem);
					
					// If two ranges were returned, downgrade to tier 2
					if (maybe_split_range.has_value()) {
						t2.push_back(downgrade_t0_to_t2(std::move(*it_t0), left_range, maybe_split_range.value()));
					} else {
						// downgrade to tier 1 with a partial range
						t1.push_back(downgrade_t0_to_t1(std::move(*it_t0), left_range));
					}
				}

				// TODO re-use?
				it_t0 = t0.erase(it_t0);
				it_rem = removes.erase(it_rem);
			}
		}
	}

	void process_remove_components_tier1(std::vector<entity_range>& removes) {
		auto it_t1 = t1.begin();
		auto it_rem = removes.begin();

		while (it_t1 != t1.end() && it_rem != removes.end()) {
			if (it_t1->range < *it_rem)
				++it_t1;
			else if (*it_rem < it_t1->range)
				++it_rem;
			else {
				if (it_t1->active == *it_rem || it_t1->range.contains(*it_rem)) {
					// remove an entire range
					// todo: move to free-store?
					it_t1 = t1.erase(it_t1);
				} else {
					// remove partial range
					auto const [left_range, maybe_split_range] = entity_range::remove(it_t1->range, *it_rem);
					
					// If two ranges were returned, downgrade to tier 2
					if (maybe_split_range.has_value()) {
						t2.push_back(downgrade_t1_to_t2(std::move(*it_t1), left_range, maybe_split_range.value()));
						it_t1 = t1.erase(it_t1);
					} else {
						// adjust the active range
						it_t1->active = left_range;

						// update the modification time
						it_t1->last_modified = clock::now();
					}
				}

				it_rem = removes.erase(it_rem);
			}
		}
	}

	void process_remove_components_tier2(std::vector<entity_range>& removes) {
		auto it_t2 = t2.begin();
		auto it_rem = removes.begin();

		while (it_t2 != t2.end() && it_rem != removes.end()) {
			if (it_t2->range < *it_rem)
				++it_t2;
			else if (*it_rem < it_t2->range)
				++it_rem;
			else {
				if (it_t2->range == *it_rem) {
					// remove an entire range
					// todo: move to free-store?
					it_t2 = t2.erase(it_t2);
				} else {
					// remove partial range
					// update the skips
					for (entity_type i = it_rem->first(); i <= it_rem->last(); ++i) {
						auto offset = it_t2->range.offset(i);

						Expects(it_t2->skips[offset] == 0); // trying to remove already removed entity

						// Skip this one entity
						auto skip_amount = 1;

						// If the entity to the right is also skipped,
						// added their skip-amount to our own to maintain
						// the chain of skip values.
						if(offset != it_t2->skips.size() - 1)
							skip_amount += it_t2->skips[offset + 1];

						// Store the skip
						it_t2->skips[offset] = skip_amount;

						// Increment previous skips if needed
						if (offset > 0) {
							auto j = offset - 1;
							while (it_t2->skips[j] > 0) {
								it_t2->skips[j] += skip_amount;
								j -= 1;
							}
						}
					}

					// If whole range is skipped, just erase it
					if (it_t2->skips[0] == it_t2->range.ucount())
						it_t2 = t2.erase(it_t2);
					else {
						// otherwise update the modification time
						it_t2->last_modified = clock::now();
					}
				}

				//it_rem = removes.erase(it_rem);
				++it_rem;
			}
		}
	}

	// Removes transient components
	void process_remove_components() requires transient<T> {
		// Transient components are removed each cycle
		t0.clear();
		t1.clear();
		t2.clear();
		set_data_removed();
	}

	Tier1 downgrade_t0_to_t1(Tier0 tier0, entity_range new_range) {
		return Tier1{tier0.range, new_range, std::move(tier0.data), clock::now()};
	}

	Tier2 downgrade_t0_to_t2(Tier0 tier0, entity_range r1, entity_range r2) {
		Tier2 tier2{tier0.range, std::vector<uint32_t>(tier0.data.size(), uint32_t{0}), std::move(tier0.data), clock::now()};
		tier2.init_skips(r1, r2);
		return tier2;
	}

	Tier2 downgrade_t1_to_t2(Tier1 tier1, entity_range r1, entity_range r2) {
		Tier2 tier2{tier1.range, std::vector<uint32_t>(tier1.data.size(), uint32_t{0}), std::move(tier1.data), clock::now()};
		tier2.init_skips(r1, r2);
		return tier2;
	}

	Tier0 upgrade_t1_to_t0(Tier1& tier1) {
		std::vector<T> stuff;

		auto const off_first = tier1.range.offset(tier1.active.first());
		auto const off_last = tier1.range.offset(tier1.active.last());
		stuff.assign(tier1.data.begin() + off_first, tier1.data.end() + off_last);

		return Tier0{tier1.active, std::move(stuff)};
	}
};
} // namespace ecs::detail

#endif // !ECS_COMPONENT_POOL
