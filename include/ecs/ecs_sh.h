#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <execution>
#include <forward_list>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#if __has_include(<stacktrace>)
#include <stacktrace>
#endif
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef ECS_EXPORT
#define ECS_EXPORT
#endif

#ifndef TLS_CACHE
#define TLS_CACHE


namespace tls {
// A class using a cache-line to cache data.
template <class Key, class Value, Key empty_slot = Key{}, size_t cache_line = 64UL>
class cache {
	// If you trigger this assert, then either your key- or value size is too large,
	// or you cache_line size is too small.
	// The cache should be able to hold at least 4 key/value pairs in order to be efficient.
	static_assert((sizeof(Key) + sizeof(Value)) <= (cache_line / 4), "key or value size too large");

	static constexpr size_t num_entries = (cache_line) / (sizeof(Key) + sizeof(Value));

public:
	constexpr cache() {
		reset();
	}

	// Returns the value if it exists in the cache,
	// otherwise inserts 'or_fn(k)' in cache and returns it
	template <class Fn>
	constexpr Value get_or(Key const k, Fn or_fn) {
		size_t index = num_entries;
		for (size_t i = 0; i < num_entries; i++) {
			// no break generates cmov's instead of jumps
			if (k == keys[i])
				index = i;
		}
		if (index != num_entries)
			return values[index];

		insert_val(k, or_fn(k));
		return values[0];
	}

	// Clears the cache
	constexpr void reset() {
		std::fill(keys, keys + num_entries, empty_slot);
		std::fill(values, values + num_entries, Value{});
	}

	// Returns the number of key/value pairs that can be cached
	static constexpr size_t max_entries() {
		return num_entries;
	}

protected:
	constexpr void insert_val(Key const k, Value const v) {
		// Move all pairs one step to the right
		std::shift_right(keys, keys + num_entries, 1);
		std::shift_right(values, values + num_entries, 1);

		// Insert the new pair at the front of the cache
		keys[0] = k;
		values[0] = v;
	}

private:
	Key keys[num_entries];
	Value values[num_entries];
};

} // namespace tls

#endif // !TLS_CACHE
#ifndef TLS_COLLECT_H
#define TLS_COLLECT_H


namespace tls {

// Pass this type to the 'Container' argument of 'tls::collect' to not store
// data from expired threads. Or use 'tls::split', which does the same thing.
template<typename>
class none {};

// Provides a thread-local instance of the type T for each thread that
// accesses it. Data is preserved when threads die.
// You can collect the thread_local T's with collect::gather*,
// which also resets the data on the threads by moving it.
// Use `tls::unique_collect` or pass different types to 'UnusedDifferentiatorType'
// to create different types.
template <typename T, template<class...> typename Container = std::vector, typename UnusedDifferentiatorType = void>
class collect final {
	// This struct manages the instances that access the thread-local data.
	// Its lifetime is marked as thread_local, which means that it can live longer than
	// the collect<> instance that spawned it.
	struct thread_data final {
		thread_data() {
			collect::init_thread(this);
		}

		~thread_data() {
			collect::remove_thread(this);
		}

		// Return a reference to an instances local data
		[[nodiscard]] T& get() noexcept {
			return data;
		}

		void remove() noexcept {
			data = {};
			next = nullptr;
		}

		[[nodiscard]] T* get_data() noexcept {
			return &data;
		}
		[[nodiscard]] T const* get_data() const noexcept {
			return &data;
		}

		void set_next(thread_data* ia) noexcept {
			next = ia;
		}
		[[nodiscard]] thread_data* get_next() noexcept {
			return next;
		}
		[[nodiscard]] thread_data const* get_next() const noexcept {
			return next;
		}

	private:
		T data{};
		thread_data* next = nullptr;
	};

private:
	static constexpr bool has_container = not std::same_as<Container<T>, none<T>>;

	// the head of the threads
	inline static std::forward_list<thread_data*> head{};

	// Mutex for serializing access for adding/removing thread-local instances
	inline static std::shared_mutex mtx;

	// Adds a new thread
	static void init_thread(thread_data* t) {
		std::unique_lock sl(mtx);
		head.push_front(t);
	}

	// Removes the thread
	static void remove_thread(thread_data* t) {
		std::unique_lock sl(mtx);

		if constexpr (has_container) {
			// Take the thread data
			T* local_data = t->get_data();
			collected_data().push_back(static_cast<T&&>(*local_data));
		}

		// Remove the thread from the linked list
		head.remove(t);
	}

	// Returns all the data collected from threads
	static Container<T>& collected_data()
		requires(has_container)
	{
		static Container<T> data{};
		return data;
	}

public:
	// Get the thread-local variable
	[[nodiscard]] static T& local() noexcept {
		thread_local thread_data var{};
		return var.get();
	}

	// Gathers all the threads data and returns it. This clears all stored data.
	[[nodiscard]]
	static Container<T> gather()
		requires(has_container)
	{
		std::unique_lock sl(mtx);

		auto& data = collected_data();
		for (thread_data* thread : head) {
			data.push_back(std::move(*thread->get_data()));
			*thread->get_data() = T{};
		}

		return std::move(data);
	}

	// Gathers all the threads data and sends it to the output iterator. This clears all stored data.
	static void gather_flattened(auto dest_iterator)
		requires(std::ranges::range<T> && has_container)
	{
		std::unique_lock sl(mtx);

		for (T& per_thread_data : collected_data()) {
			std::move(per_thread_data.begin(), per_thread_data.end(), dest_iterator);
		}
		collected_data().clear();

		for (thread_data* thread : head) {
			T* ptr_per_thread_data = thread->get_data();
			std::move(ptr_per_thread_data->begin(), ptr_per_thread_data->end(), dest_iterator);
			//*ptr_per_thread_data = T{};
			ptr_per_thread_data->clear();
		}
	}

	// Perform an action on all threads data
	template <class Fn>
	static void for_each(Fn&& fn) {
		if constexpr (std::invocable<Fn, T const&>) {
			std::shared_lock sl(mtx);
			for (thread_data const* thread : head) {
				fn(*thread->get_data());
			}

			if constexpr (has_container)
				for (auto const& d : collected_data())
					fn(d);
		} else {
			std::unique_lock sl(mtx);
			for (thread_data* thread : head) {
				fn(*thread->get_data());
			}

			if constexpr (has_container)
				for (auto& d : collected_data())
					fn(d);
		}
	}

	// Clears all data
	static void clear() {
		std::unique_lock sl(mtx);
		for (thread_data* thread : head) {
			*(thread->get_data()) = {};
		}

		if constexpr (has_container)
			collected_data().clear();
	}
};

template <typename T, template<class...> typename Container = std::vector, auto U = [] {}>
using unique_collect = collect<T, Container, decltype(U)>;

} // namespace tls

#endif // !TLS_COLLECT_H
#ifndef TLS_SPLIT_H
#define TLS_SPLIT_H


namespace tls {
template <typename T, typename UnusedDifferentiaterType = void>
using split = collect<T, none, UnusedDifferentiaterType>;

template <typename T, auto U = [] {}>
using unique_split = split<T, decltype(U)>;

} // namespace tls

#endif // !TLS_SPLIT_H
#ifndef ECS_DETAIL_TYPE_LIST_H
#define ECS_DETAIL_TYPE_LIST_H


namespace ecs::detail {

	//
	// Helper templates for working with types at compile-time

	// inspired by https://devblogs.microsoft.com/cppblog/build-throughput-series-more-efficient-template-metaprogramming/

	template <typename...>
	struct type_list;

	template <typename First, typename Second>
	struct type_pair {
		using first = First;
		using second = Second;
	};

// #if defined(_MSC_VER)
// #define ECS_NULLBODY ;
// #else
#define ECS_NULLBODY                                                                                                                       \
	{ return nullptr; }
	// #endif

	namespace impl {
		// type wrapper
		template <typename T>
		struct wrap_t {
			using type = T;
			wrap_t(...) {}
		};

		//
		// detect type_list
		template <typename... Types>
		consteval bool detect_type_list(type_list<Types...>*) {
			return true;
		}
		consteval bool detect_type_list(...) {
			return false;
		}

		//
		// type_list_size.
		template <typename>
		struct type_list_size;
		template <typename... Types>
		struct type_list_size<type_list<Types...>> {
			static constexpr std::size_t value = sizeof...(Types);
		};

		//
		// type_list concepts
		template <typename TL>
		concept TypeList = detect_type_list(static_cast<TL*>(nullptr));

		template <typename TL>
		concept NonEmptyTypeList = TypeList<TL> && (0 < type_list_size<TL>::value);

		//
		// type_list indices and type lookup
		template <typename T, class... Types>
		consteval int index_of(type_list<Types...>*) noexcept {
			constexpr int index = []<int... Ns>(std::integer_sequence<int, Ns...>) {
				int type_index = 0;
				((type_index += (std::is_same_v<T, Types> * (1 + Ns))) || ...);
				return type_index - 1;
			}(std::make_integer_sequence<int, sizeof...(Types)>{});
			static_assert(index != -1, "type not found in list");
			return index;
		}

		template <int N, typename... Types>
		consteval auto type_at(type_list<Types...>*) noexcept {
			return []<int... Ns>(std::integer_sequence<int, Ns...>) {
				return [&](wrap_t<decltype(Ns)>..., auto nth, auto...) {
					return nth;
				}(wrap_t<Types>{}...);
			}(std::make_integer_sequence<int, N>{});
		}

		//
		// helper functions

		// create a nullptr initialised type_list
		template <typename... Ts>
		constexpr type_list<Ts...>* null_list() {
			return nullptr;
		}
		template <TypeList TL>
		constexpr TL* null_tlist() {
			return nullptr;
		}

		template <typename... Types, typename F>
		constexpr void for_each_type(F&& f, type_list<Types...>*) {
			(f.template operator()<Types>(), ...);
		}

		template <typename T, typename... Types, typename F>
		constexpr void for_specific_type(F&& f, type_list<Types...>*) {
			auto const runner = [&f]<typename X>() {
				if constexpr (std::is_same_v<T, X>) {
					f();
				}
			};

			(runner.template operator()<Types>(), ...);
		}

		template <typename T, typename... Types, typename F, typename NF>
		constexpr void for_specific_type_or(F&& f, NF&& nf, type_list<Types...>*) {
			auto const runner = [&]<typename X>() {
				if constexpr (std::is_same_v<T, X>) {
					f();
				} else {
					nf();
				}
			};

			(runner.template operator()<Types>(), ...);
		}

		template <typename... Types, typename F>
		constexpr decltype(auto) for_all_types(F&& f, type_list<Types...>*) {
			return f.template operator()<Types...>();
		}

		template <typename... Types, typename F>
		constexpr bool all_of_type(F&& f, type_list<Types...>*) {
			return (f.template operator()<Types>() && ...);
		}

		template <typename... Types, typename F>
		constexpr bool any_of_type(F&& f, type_list<Types...>*) {
			return (f.template operator()<Types>() || ...);
		}

		template <template <typename O> typename Tester, typename... Types, typename F>
		constexpr auto run_if(F&& /*f*/, type_list<>*) {
			return;
		}

		template <template <typename O> typename Tester, typename FirstType, typename... Types, typename F>
		constexpr auto run_if(F&& f, type_list<FirstType, Types...>*) {
			if constexpr (Tester<FirstType>::value) {
				return f.template operator()<FirstType>();
			} else {
				return run_if<Tester, Types...>(f, static_cast<type_list<Types...>*>(nullptr));
			}
		}

		template <template <typename> typename Predicate, typename... Types>
		constexpr std::size_t count_type_if(type_list<Types...>*) {
			return static_cast<std::size_t>((Predicate<Types>::value + ...));
		}

		template <typename... Types, typename F>
		constexpr std::size_t count_type_if(F&& f, type_list<Types...>*) {
			return (static_cast<std::size_t>(f.template operator()<Types>()) + ...);
		}

		template <typename TL>
		struct first_type {
			template <typename First, typename... Types>
			constexpr static wrap_t<First>* helper(type_list<First, Types...>*);

			using type = typename std::remove_pointer_t<decltype(helper(static_cast<TL*>(nullptr)))>::type;
		};

		template <typename First, typename... Types>
		constexpr type_list<Types...>* skip_first_type(type_list<First, Types...>*) ECS_NULLBODY

			template <typename TL, template <typename O> typename Transformer>
			struct transform_type {
			template <typename... Types>
			constexpr static type_list<Transformer<Types>...>* helper(type_list<Types...>*);

			using type = std::remove_pointer_t<decltype(helper(static_cast<TL*>(nullptr)))>;
		};

		template <typename TL, template <typename... O> typename Transformer>
		struct transform_type_all {
			template <typename... Types>
			constexpr static Transformer<Types...>* helper(type_list<Types...>*);

			using type = std::remove_pointer_t<decltype(helper(static_cast<TL*>(nullptr)))>;
		};

		template <typename T, typename... Types>
		constexpr type_list<Types..., T>* add_type(type_list<Types...>*) ECS_NULLBODY

			template <typename TL, typename T>
			using add_type_t = std::remove_pointer_t<decltype(add_type<T>(static_cast<TL*>(nullptr)))>;

		template <typename TL, template <typename O> typename Predicate>
		struct split_types_if {
			template <typename ListTrue, typename ListFalse, typename Front, typename... Rest>
			constexpr static auto helper(type_list<Front, Rest...>*) {
				if constexpr (Predicate<Front>::value) {
					using NewListTrue = add_type_t<ListTrue, Front>;

					if constexpr (sizeof...(Rest) > 0) {
						return helper<NewListTrue, ListFalse>(static_cast<type_list<Rest...>*>(nullptr));
					} else {
						return static_cast<type_pair<NewListTrue, ListFalse>*>(nullptr);
					}
				} else {
					using NewListFalse = add_type_t<ListFalse, Front>;

					if constexpr (sizeof...(Rest) > 0) {
						return helper<ListTrue, NewListFalse>(static_cast<type_list<Rest...>*>(nullptr));
					} else {
						return static_cast<type_pair<ListTrue, NewListFalse>*>(nullptr);
					}
				}
			}

			using list_pair = std::remove_pointer_t<decltype(helper<type_list<>, type_list<>>(static_cast<TL*>(nullptr)))>;
		};

		template <typename TL, template <typename O> typename Predicate>
		struct filter_types_if {
			template <typename Result, typename Front, typename... Rest>
			constexpr static auto helper(type_list<Front, Rest...>*) {
				if constexpr (Predicate<Front>::value) {
					using NewResult = add_type_t<Result, Front>;

					if constexpr (sizeof...(Rest) > 0) {
						return helper<NewResult>(static_cast<type_list<Rest...>*>(nullptr));
					} else {
						return static_cast<NewResult*>(nullptr);
					}
				} else {
					if constexpr (sizeof...(Rest) > 0) {
						return helper<Result>(static_cast<type_list<Rest...>*>(nullptr));
					} else {
						return static_cast<Result*>(nullptr);
					}
				}
			}

			using result = std::remove_pointer_t<decltype(helper<type_list<>>(static_cast<TL*>(nullptr)))>;
		};

		template <typename First, typename... Types>
		constexpr bool is_unique_types(type_list<First, Types...>*) {
			if constexpr ((std::is_same_v<First, Types> || ...))
				return false;
			else {
				if constexpr (sizeof...(Types) == 0)
					return true;
				else
					return is_unique_types<Types...>({});
			}
		}

		template <typename T, typename... Types>
		constexpr bool contains_type(type_list<Types...>* = nullptr) {
			return (std::is_same_v<T, Types> || ...);
		}

		template <typename T, typename TL>
		constexpr bool list_contains_type(TL* tl = nullptr) {
			return contains_type<T>(tl);
		}

		template <typename... Types1, typename... Types2>
		constexpr auto concat_type_lists(type_list<Types1...>*, type_list<Types2...>*) -> type_list<Types1..., Types2...>*;

		struct merger {
#if 0 // defined(_MSC_VER) && !defined(__clang__)
	  // ! Triggers ICE in 17.9+

		// This optimization is only possible in msvc due to it not checking templates
		// before they are instantiated.

		// terminal node; the right list is empty, return the left list
		template <typename LeftList>
		consteval static auto helper(LeftList*, type_list<>*) -> LeftList*;

		// if the first type from the right list is not in the left list, add it and continue
		template <typename LeftList, typename FirstRight, typename... Right>
			requires(!list_contains_type<FirstRight, LeftList>())
		consteval static auto helper(LeftList* left, type_list<FirstRight, Right...>* right)
			-> decltype(merger::helper(add_type<FirstRight>(left), null_list<Right...>()));

		// skip the first type in the right list and continue
		template <typename LeftList, typename RightList>
		consteval static auto helper(LeftList* left, RightList* right)
			-> decltype(merger::helper(left, skip_first_type(right)));
#else
			template <typename LeftList>
			consteval static LeftList* helper(LeftList*, type_list<>*) {
				return nullptr;
			}

			template <typename LeftList, typename FirstRight, typename... Right>
				requires(!list_contains_type<FirstRight, LeftList>())
			consteval static auto helper(LeftList* left, type_list<FirstRight, Right...>*) {
				return merger::helper(add_type<FirstRight>(left), null_list<Right...>());
			}

			template <typename LeftList, typename RightList>
			consteval static auto helper(LeftList* left, RightList* right) {
				return merger::helper(left, skip_first_type(right));
			}
#endif
		};

	} // namespace impl

	template <impl::TypeList TL>
	constexpr std::size_t type_list_size = impl::type_list_size<TL>::value;

	template <impl::TypeList TL>
	constexpr bool type_list_is_empty = (0 == impl::type_list_size<TL>::value);

	// TODO change type alias to *_t

	// Get the index of a type in a type_list.
	// This only returns the index of the first type found.
	template <typename T, impl::NonEmptyTypeList TL>
	consteval int index_of() {
		return impl::index_of<T>((TL*)nullptr);
	}

	// Get the type of an index in a type_list
	template <int N, impl::NonEmptyTypeList TL>
		requires(N < type_list_size<TL>)
	using type_at = typename decltype(impl::type_at<N>(impl::null_tlist<TL>()))::type;

	// Return the first type in a type_list
	template <impl::NonEmptyTypeList TL>
	using first_type = typename impl::first_type<TL>::type;

	// Skip the first type in a type_list
	template <impl::NonEmptyTypeList TL>
	using skip_first_type = std::remove_pointer_t<decltype(impl::skip_first_type(static_cast<TL*>(nullptr)))>;

	// Add a type to a type_list
	template <impl::TypeList TL, typename T>
	using add_type = std::remove_pointer_t<decltype(impl::add_type<T>(static_cast<TL*>(nullptr)))>; // typename impl::add_type<TL, T>::type;

	// Transforms the types in a type_list
	// Takes transformer that results in new type, like remove_cvref_t
	template <impl::TypeList TL, template <typename O> typename Transformer>
	using transform_type = typename impl::transform_type<TL, Transformer>::type;

	// Transforms all the types in a type_list at once
	// Takes a transformer that results in a new type
	// Ex.
	// 	template <typename... Types>
	//  using transformer = std::tuple<entity_id, Types...>;
	template <impl::TypeList TL, template <typename... O> typename Transformer>
	using transform_type_all = typename impl::transform_type_all<TL, Transformer>::type;

	// Splits a type_list into two list depending on the predicate
	template <impl::TypeList TL, template <typename O> typename Predicate>
	using split_types_if = typename impl::split_types_if<TL, Predicate>::list_pair;

	// Filter a type_list depending on the predicate
	template <impl::TypeList TL, template <typename O> typename Predicate>
	using filter_types_if = typename impl::filter_types_if<TL, Predicate>::result;

	// Applies the functor F to each type in the type list.
	// Takes lambdas of the form '[]<typename T>() {}'
	template <impl::TypeList TL, typename F>
	constexpr void for_each_type(F&& f) {
		impl::for_each_type(f, static_cast<TL*>(nullptr));
	}

	// Applies the functor F to a specific type in the type list.
	// Takes lambdas of the form '[]() {}'
	template <typename T, impl::TypeList TL, typename F>
	constexpr void for_specific_type(F&& f) {
		impl::for_specific_type<T>(f, static_cast<TL*>(nullptr));
	}

	// Applies the functor F when a specific type in the type list is found,
	// applies NF when not found
	// Takes lambdas of the form '[]() {}'
	template <typename T, impl::TypeList TL, typename F, typename NF>
	constexpr void for_specific_type_or(F&& f, NF&& nf) {
		impl::for_specific_type_or<T>(f, nf, static_cast<TL*>(nullptr));
	}

	// Applies the functor F to all types in the type list.
	// Takes lambdas of the form '[]<typename ...T>() {}'
	template <impl::TypeList TL, typename F>
	constexpr decltype(auto) for_all_types(F&& f) {
		return impl::for_all_types(f, static_cast<TL*>(nullptr));
	}

	// Applies the bool-returning functor F to each type in the type list.
	// Returns true if all of them return true.
	// Takes lambdas of the form '[]<typename T>() -> bool {}'
	template <impl::TypeList TL, typename F>
	constexpr bool all_of_type(F&& f) {
		return impl::all_of_type(f, static_cast<TL*>(nullptr));
	}

	// Applies the bool-returning functor F to each type in the type list.
	// Returns true if any of them return true.
	// Takes lambdas of the form '[]<typename T>() -> bool {}'
	template <impl::TypeList TL, typename F>
	constexpr bool any_of_type(F&& f) {
		return impl::any_of_type(f, static_cast<TL*>(nullptr));
	}

	// Runs F once when a type satisfies the tester. F takes a type template parameter and can return a value.
	template <template <typename O> typename Tester, impl::TypeList TL, typename F>
	constexpr auto run_if(F&& f) {
		return impl::run_if<Tester>(f, static_cast<TL*>(nullptr));
	}

	// Returns the count of all types that satisfy the predicate. F takes a type template parameter and returns a boolean.
	template <impl::TypeList TL, typename F>
	constexpr std::size_t count_type_if(F&& f) {
		return impl::count_type_if(f, static_cast<TL*>(nullptr));
	}

	// Returns the count of all types that satisfy the predicate. F takes a type template parameter and returns a boolean.
	template <impl::TypeList TL, template <typename> typename Predicate>
	constexpr std::size_t count_type_if() {
		return impl::count_type_if<Predicate>(static_cast<TL*>(nullptr));
	}

	// Returns true if all types in the list are unique
	template <impl::TypeList TL>
	constexpr bool is_unique_types() {
		return impl::is_unique_types(static_cast<TL*>(nullptr));
	}

	// Returns true if a type list contains the type
	template <typename T, impl::TypeList TL>
	constexpr bool contains_type() {
		return impl::contains_type<T>(static_cast<TL*>(nullptr));
	}

	// Returns true if a type list 'TA' contains all the types of type list 'TB'
	template <impl::TypeList TA, impl::TypeList TB>
	constexpr bool contains_list() {
		return all_of_type<TB>([]<typename B>() {
			return impl::contains_type<B>(static_cast<TA*>(nullptr));
		});
	}

	// concatenates two type_list
	template <impl::TypeList TL1, impl::TypeList TL2>
	using concat_type_lists =
		std::remove_pointer_t<decltype(impl::concat_type_lists(static_cast<TL1*>(nullptr), static_cast<TL2*>(nullptr)))>;

	// merge two type_list, duplicate types are ignored
	template <impl::TypeList TL1, impl::TypeList TL2>
	using merge_type_lists = std::remove_pointer_t<decltype(impl::merger::helper(static_cast<TL1*>(nullptr), static_cast<TL2*>(nullptr)))>;

} // namespace ecs::detail
#endif // !ECS_DETAIL_TYPE_LIST_H
#ifndef ECS_DETAIL_CONTRACT_H
#define ECS_DETAIL_CONTRACT_H

#if __has_include(<stacktrace>)
#endif

// Contracts. If they are violated, the program is in an invalid state and is terminated.
namespace ecs::detail {
	// Concept for the contract violation interface.
	template <typename T>
	concept contract_violation_interface = requires(T t) {
		{ t.assertion_failed("", "") } -> std::same_as<void>;
		{ t.precondition_violation("", "") } -> std::same_as<void>;
		{ t.postcondition_violation("", "") } -> std::same_as<void>;
	};

	struct default_contract_violation_impl {
		void panic(char const* why, char const* what, char const* how) noexcept {
			std::cerr << why << ": \"" << how << "\"\n\t" << what << "\n\n";
#ifdef __cpp_lib_stacktrace
			// Dump a stack trace if available
			std::cerr << "** stack dump **\n" << std::stacktrace::current(3) << '\n';
#endif
			std::terminate();
		}

		void assertion_failed(char const* what, char const* how) noexcept {
			panic("Assertion failed", what, how);
		}

		void precondition_violation(char const* what, char const* how) noexcept {
			panic("Precondition violation", what, how);
		}

		void postcondition_violation(char const* what, char const* how) noexcept {
			panic("Postcondition violation", what, how);
		}
	};
} // namespace ecs::detail

// The contract violation interface, which can be overridden by users
ECS_EXPORT namespace ecs {
	template <typename...>
	auto contract_violation_handler = ecs::detail::default_contract_violation_impl{};
}

#if ECS_ENABLE_CONTRACTS

namespace ecs::detail {
	template <typename... DummyArgs>
		requires(sizeof...(DummyArgs) == 0)
	inline void do_assertion_failed(char const* what, char const* how) {
		ecs::detail::contract_violation_interface auto& cvi = ecs::contract_violation_handler<DummyArgs...>;
		cvi.assertion_failed(what, how);
	}

	template <typename... DummyArgs>
		requires(sizeof...(DummyArgs) == 0)
	inline void do_precondition_violation(char const* what, char const* how) {
		ecs::detail::contract_violation_interface auto& cvi = ecs::contract_violation_handler<DummyArgs...>;
		cvi.precondition_violation(what, how);
	}

	template <typename... DummyArgs>
		requires(sizeof...(DummyArgs) == 0)
	inline void do_postcondition_violation(char const* what, char const* how) {
		ecs::detail::contract_violation_interface auto& cvi = ecs::contract_violation_handler<DummyArgs...>;
		cvi.postcondition_violation(what, how);
	}
} // namespace ecs::detail

#if __has_cpp_attribute(assume)
#define Assume(expression) [[assume((expression))]]
#else
#ifdef _MSC_VER
#define Assume(expression) __assume((expression))
#elif defined(__clang)
#define Assume(expression) __builtin_assume((expression))
#elif defined(GCC)
#define Assume(expression) __attribute__((assume((expression))))
#else
#define Assume(expression) /* unknown */
#endif
#endif // __has_cpp_attribute

#define ECS_InvokeContractBreach(expression, message, func)                                                                                \
	do {                                                                                                                                   \
		if (!(expression)) {                                                                                                               \
			if (!std::is_constant_evaluated())                                                                                             \
				func(#expression, message);                                                                                                \
			/* Contract handler should not let execution reach here */                                                                     \
			std::unreachable();                                                                                                            \
		}                                                                                                                                  \
		/* (expression) is always true from here on out */                                                                                 \
		Assume(expression);                                                                                                                \
	} while (false)

#define Assert(expression, message) ECS_InvokeContractBreach(expression, message, ecs::detail::do_assertion_failed)
#define Pre(expression, message) ECS_InvokeContractBreach(expression, message, ecs::detail::do_precondition_violation)
#define Post(expression, message) ECS_InvokeContractBreach(expression, message, ecs::detail::do_postcondition_violation)

// Audit contracts. Used for expensive checks; can be disabled.
#if ECS_ENABLE_CONTRACTS_AUDIT
#define AssertAudit(expression, message) Assert(expression, message)
#define PreAudit(expression, message) Pre(expression, message)
#define PostAudit(expression, message) Post(expression, message)
#else
#define AssertAudit(expression, message)
#define PreAudit(expression, message)
#define PostAudit(expression, message)
#endif

#else

#define Assert(expression, message) Assume(expression)
#define Pre(expression, message) Assume(expression)
#define Post(expression, message) Assume(expression)
#define AssertAudit(expression, message)
#define PreAudit(expression, message)
#define PostAudit(expression, message)
#endif

#endif // !ECS_DETAIL_CONTRACT_H
#ifndef ECS_DETAIL_TYPE_HASH_H
#define ECS_DETAIL_TYPE_HASH_H


// Beware of using this with local defined structs/classes
// https://developercommunity.visualstudio.com/content/problem/1010773/-funcsig-missing-data-from-locally-defined-structs.html

namespace ecs::detail {

using type_hash = std::uint64_t;

template <typename T>
consteval type_hash get_type_hash() {
	type_hash const prime = 0x100000001b3;
#ifdef _MSC_VER
	char const* string = __FUNCDNAME__; // has full type info, but is not very readable
#else
	char const* string = __PRETTY_FUNCTION__;
#endif
	type_hash hash = 0xcbf29ce484222325;
	while (*string != '\0') {
		hash ^= static_cast<type_hash>(*string);
		hash *= prime;
		string += 1;
	}

	return hash;
}

template <typename TypesList>
consteval auto get_type_hashes_array() {
	return for_all_types<TypesList>([]<typename... Types>() {
		return std::array<detail::type_hash, sizeof...(Types)>{get_type_hash<Types>()...};
	});
}

} // namespace ecs::detail

#endif // !ECS_DETAIL_TYPE_HASH_H
#ifndef ECS_DETAIL_TAGGED_POINTER_H
#define ECS_DETAIL_TAGGED_POINTER_H


namespace ecs::detail {

// 1-bit tagged pointer
// Note: tags are considered separate from the pointer, and is
// therefore not reset when a new pointer is set
template <typename T>
struct tagged_pointer {
	tagged_pointer(T* in) noexcept : ptr(reinterpret_cast<uintptr_t>(in)) {
		Pre((ptr & TagMask) == 0, "pointer must not have its tag already set");
	}

	tagged_pointer() noexcept = default;
	tagged_pointer(tagged_pointer const&) noexcept = default;
	tagged_pointer(tagged_pointer&&) noexcept = default;
	tagged_pointer& operator=(tagged_pointer const&) noexcept = default;
	tagged_pointer& operator=(tagged_pointer&&) noexcept = default;

	tagged_pointer& operator=(T* in) noexcept {
		auto const set = ptr & TagMask;
		ptr = set | reinterpret_cast<uintptr_t>(in);
		return *this;
	}

	void clear() noexcept {
		ptr = 0;
	}
	void clear_bits() noexcept {
		ptr = ptr & PointerMask;
	}
	int get_tag() const noexcept {
		return ptr & TagMask;
	}
	void set_tag(int tag) noexcept {
		Pre(tag >= 0 && tag <= static_cast<int>(TagMask), "tag is outside supported range");
		ptr = (ptr & PointerMask) | static_cast<uintptr_t>(tag);
	}

	bool test_bit1() const noexcept
		requires(sizeof(void*) >= 2) {
		return ptr & static_cast<uintptr_t>(0b001);
	}
	bool test_bit2() const noexcept
		requires(sizeof(void*) >= 4) {
		return ptr & static_cast<uintptr_t>(0b010);
	}
	bool test_bit3() const noexcept
		requires(sizeof(void*) >= 8) {
		return ptr & static_cast<uintptr_t>(0b100);
	}

	void set_bit1() noexcept
		requires(sizeof(void*) >= 2) {
		ptr |= static_cast<uintptr_t>(0b001);
	}
	void set_bit2() noexcept
		requires(sizeof(void*) >= 4) {
		ptr |= static_cast<uintptr_t>(0b010);
	}
	void set_bit3() noexcept
		requires(sizeof(void*) >= 8) {
		ptr |= static_cast<uintptr_t>(0b100);
	}

	void clear_bit1() noexcept
		requires(sizeof(void*) >= 2) {
		ptr = ptr & ~static_cast<uintptr_t>(0b001);
	}
	void clear_bit2() noexcept
		requires(sizeof(void*) >= 4) {
		ptr = ptr & ~static_cast<uintptr_t>(0b010);
	}
	void clear_bit3() noexcept
		requires(sizeof(void*) >= 8) {
		ptr = ptr & ~static_cast<uintptr_t>(0b100);
	}

	T* pointer() noexcept {
		return reinterpret_cast<T*>(ptr & PointerMask);
	}
	T const* pointer() const noexcept {
		return reinterpret_cast<T*>(ptr & PointerMask);
	}

	T* operator->() noexcept {
		return pointer();
	}
	T const* operator->() const noexcept {
		return pointer();
	}

	operator T*() noexcept {
		return pointer();
	}
	operator T const *() const noexcept {
		return pointer();
	}

private:
	//constexpr static uintptr_t TagMask = 0b111;
	constexpr static uintptr_t TagMask = sizeof(void*) - 1;
	constexpr static uintptr_t PointerMask = ~TagMask;

	uintptr_t ptr = 0;
};

} // namespace ecs::detail

#endif // ECS_DETAIL_TAGGED_POINTER_H
#ifndef ECS_ENTITY_ID_H
#define ECS_ENTITY_ID_H

namespace ecs {
namespace detail {
using entity_type = int;
using entity_offset = unsigned int; // must cover the entire entity_type domain
} // namespace detail

// A simple struct that is an entity identifier.
ECS_EXPORT struct entity_id {
	// Uninitialized entity ids are not allowed, because they make no sense
	entity_id() = delete;

	constexpr entity_id(detail::entity_type _id) noexcept : id(_id) {}

	constexpr operator detail::entity_type&() noexcept {
		return id;
	}
	constexpr operator detail::entity_type const &() const noexcept {
		return id;
	}

private:
	detail::entity_type id;
};
} // namespace ecs

#endif // !ECS_ENTITY_ID_H
#ifndef ECS_DETAIL_ENTITY_ITERATOR_H
#define ECS_DETAIL_ENTITY_ITERATOR_H


namespace ecs::detail {

// Iterator support
class entity_iterator {
public:
	// iterator traits
	using difference_type = ptrdiff_t;
	using value_type = entity_type;
	using pointer = const entity_type*;
	using reference = const entity_type&;
	using iterator_category = std::random_access_iterator_tag;

	// entity_iterator() = delete; // no such thing as a 'default' entity
	constexpr entity_iterator() noexcept {};

	constexpr entity_iterator(entity_id ent) noexcept : ent_(ent) {}

	constexpr entity_iterator& operator++() {
		ent_ = step(ent_, 1);
		return *this;
	}

	constexpr entity_iterator operator++(int) {
		entity_iterator const retval = *this;
		++(*this);
		return retval;
	}

	constexpr entity_iterator operator+(difference_type diff) const {
		return entity_iterator{step(ent_, diff)};
	}

	// Operator exclusively for GCC. This operator is called by GCC's parallel implementation
	// for some goddamn reason. When has this ever been a thing?
	constexpr value_type operator[](int index) const {
		return step(ent_, index);
	}

	constexpr value_type operator-(entity_iterator other) const {
		return step(ent_, -other.ent_);
	}

	constexpr bool operator==(entity_iterator other) const {
		return ent_ == other.ent_;
	}

	constexpr bool operator!=(entity_iterator other) const {
		return !(*this == other);
	}

	constexpr entity_id operator*() {
		return {ent_};
	}

protected:
	constexpr entity_type step(entity_type start, difference_type diff) const {
		// ensures the value wraps instead of causing an overflow
		auto const diff_start = static_cast<entity_offset>(start);
		return static_cast<entity_type>(diff_start + diff);
	}

private:
	value_type ent_{0};
};
} // namespace ecs::detail

#endif // !ECS_DETAIL_ENTITY_ITERATOR_H
#ifndef ECS_DETAIL_OPTIONS_H
#define ECS_DETAIL_OPTIONS_H


namespace ecs::detail {

//
// Check if type is an interval
template <typename T>
struct is_interval {
	static constexpr bool value = false;
};
template <typename T>
requires requires {
	T::ms;
	T::us;
}
struct is_interval<T> {
	static constexpr bool value = true;
};

//
// Check if type is a parent
template <typename T>
struct is_parent {
	static constexpr bool value = false;
};
template <typename T>
requires requires { typename std::remove_cvref_t<T>::_ecs_parent; }
struct is_parent<T> {
	static constexpr bool value = true;
};



// Contains detectors for the options
namespace detect {
	template <template <typename O> typename Tester, typename ListOptions>
	constexpr int find_tester_index() {
		int found = 0;
		int index = 0;
		for_each_type<ListOptions>([&]<typename OptionFromList>() {
			found = !found && Tester<OptionFromList>::value;
			index += !found;
		});

		if (index == type_list_size<ListOptions>)
			return -1;
		return index;
	}

	template <typename Option, typename ListOptions>
	constexpr int find_type_index() {
		int found = 0;
		int index = 0;
		for_each_type<ListOptions>([&]<typename OptionFromList>() {
			found = !found && std::is_same_v<Option, OptionFromList>;
			index += !found;
		});

		if (index == type_list_size<ListOptions>)
			return -1;
		return index;
	}

	// A detector that applies Tester to each option.
	template <template <typename O> typename Tester, typename ListOptions, typename NotFoundType = void>
	constexpr auto test_option() {
		auto const lambda = []<typename T>() {
			return static_cast<std::remove_cvref_t<T>*>(nullptr);
		};
		using Type = decltype(run_if<Tester, ListOptions>(lambda));

		if constexpr (!std::is_same_v<Type, void>) {
			return Type{};
		} else {
			return static_cast<NotFoundType*>(nullptr);
		}
	}

	template <typename Type>
	struct type_detector {
		template<typename... Types>
		consteval static bool test(type_list<Types...>*) noexcept {
			return (test(static_cast<Types*>(nullptr)) || ...);
		}

		consteval static bool test(Type*) noexcept {
			return true;
		}
		consteval static bool test(...) noexcept {
			return false;
		}
	};

} // namespace detect

// Use a tester to check the options. Takes a tester structure and a type_list of options to test against.
// The tester must have static member 'value' that determines if the option passed to it
// is what it is looking for, see 'is_group' for an example.
// STL testers like 'std::is_execution_policy' can also be used
template <template <typename O> typename Tester, typename TypelistOptions>
using test_option_type = std::remove_pointer_t<decltype(detect::test_option<Tester, TypelistOptions>())>;

// Use a tester to check the options. Results in 'NotFoundType' if the tester
// does not find a viable option.
template <template <typename O> typename Tester, typename TypelistOptions, typename NotFoundType>
using test_option_type_or = std::remove_pointer_t<decltype(detect::test_option<Tester, TypelistOptions, NotFoundType>())>;

// Use a tester to find the index of a type in the tuple. Results in -1 if not found
template <template <typename O> typename Tester, typename TypelistOptions>
static constexpr int test_option_index = detect::find_tester_index<Tester, TypelistOptions>();

template <typename Option, typename ListOptions>
constexpr bool has_option() {
	return detect::type_detector<Option>::test(static_cast<ListOptions*>(nullptr));
}

} // namespace ecs::detail

#endif // !ECS_DETAIL_OPTIONS_H
#ifndef ECS_ENTITY_RANGE_H
#define ECS_ENTITY_RANGE_H



namespace ecs {
// Defines a range of entities.
// 'last' is included in the range.
ECS_EXPORT class entity_range final {
	detail::entity_type first_;
	detail::entity_type last_;

public:
	entity_range() = delete; // no such thing as a 'default' range

	constexpr entity_range(detail::entity_type first, detail::entity_type last) : first_(first), last_(last) {
		Pre(first <= last, "invalid interval; first entity can not be larger than the last entity");
	}

	static constexpr entity_range all() {
		return {std::numeric_limits<detail::entity_type>::min(), std::numeric_limits<detail::entity_type>::max()};
	}

	[[nodiscard]] constexpr detail::entity_iterator begin() const {
		return {first_};
	}

	[[nodiscard]] constexpr detail::entity_iterator end() const {
		return {last_ + 1};
	}

	[[nodiscard]] constexpr bool operator==(entity_range const& other) const {
		return equals(other);
	}

	// For sort
	[[nodiscard]] constexpr bool operator<(entity_range const& other) const {
		return /*first_ < other.first() &&*/ last_ < other.first();
	}

	[[nodiscard]] constexpr bool operator<(entity_id const& id) const {
		return last_ < id;
	}

	// Returns the first entity in the range
	[[nodiscard]] constexpr entity_id first() const {
		return entity_id{first_};
	}

	// Returns the last entity in the range
	[[nodiscard]] constexpr entity_id last() const {
		return entity_id{last_};
	}

	// Returns the number of entities in this range
	[[nodiscard]] constexpr ptrdiff_t count() const {
		return static_cast<ptrdiff_t>(last_ - first_ + 1);
	}

	// Returns the number of entities in this range as unsigned
	[[nodiscard]] constexpr size_t ucount() const {
		return static_cast<size_t>(last_ - first_ + 1);
	}

	// Returns true if the ranges are identical
	[[nodiscard]] constexpr bool equals(entity_range const& other) const {
		return first_ == other.first() && last_ == other.last();
	}

	// Returns true if the entity is contained in this range
	[[nodiscard]] constexpr bool contains(entity_id const& ent) const {
		return ent >= first_ && ent <= last_;
	}

	// Returns true if the range is contained in this range
	[[nodiscard]] constexpr bool contains(entity_range const& range) const {
		return range.first() >= first_ && range.last() <= last_;
	}

	// Returns true if the ranges are adjacent to each other
	[[nodiscard]] constexpr bool adjacent(entity_range const& range) const {
		return first_ - 1 == range.last() || last_ + 1 == range.first();
	}

	// Returns the offset of an entity into this range
	// Pre: 'ent' must be in the range
	[[nodiscard]] constexpr detail::entity_offset offset(entity_id const ent) const {
		Pre(contains(ent), "entity must exist in the range");
		return static_cast<detail::entity_offset>(ent - first_);
	}

	// Returns the entity id at the specified offset
	// Pre: 'offset' is in the range
	[[nodiscard]] entity_id at(detail::entity_offset const offset) const {
		entity_id const id = static_cast<detail::entity_type>(static_cast<detail::entity_offset>(first()) + offset);
		Pre(id <= last(), "offset is out of bounds of the range");
		return id;
	}

	// Returns true if the two ranges touches each other
	[[nodiscard]] constexpr bool overlaps(entity_range const& other) const {
		return first_ <= other.last_ && other.first_ <= last_;
	}

	// Removes a range from another range.
	// If the range was split by the remove, it returns two ranges.
	// Pre: 'other' must overlap 'range', but must not be equal to it
	[[nodiscard]] constexpr static std::pair<entity_range, std::optional<entity_range>> remove(entity_range const& range,
																							   entity_range const& other) {
		Pre(range.overlaps(other), "the two ranges must overlap");
		Pre(!range.equals(other), "the two ranges can not be equal");

		// Remove from the front
		if (other.first() == range.first()) {
			auto const [min, max] = std::minmax({range.last(), other.last()});
			return {entity_range{min + 1, max}, std::nullopt};
		}

		// Remove from the back
		if (other.last() == range.last()) {
			auto const [min, max] = std::minmax({range.first(), other.first()});
			return {entity_range{min, max - 1}, std::nullopt};
		}

		if (range.contains(other)) {
			// Remove from the middle
			return {entity_range{range.first(), other.first() - 1}, entity_range{other.last() + 1, range.last()}};
		} else {
			// Remove overlaps
			if (range.first() < other.first())
				return {entity_range{range.first(), other.first() - 1}, std::nullopt};
			else
				return {entity_range{other.last() + 1, range.last()}, std::nullopt};
		}
	}

	// Combines two ranges into one
	// Pre: r1 and r2 must be adjacent ranges
	[[nodiscard]] constexpr static entity_range merge(entity_range const& r1, entity_range const& r2) {
		Pre(r1.adjacent(r2), "can not merge two ranges that are not adjacent to each other");
		if (r1 < r2)
			return entity_range{r1.first(), r2.last()};
		else
			return entity_range{r2.first(), r1.last()};
	}

	// Returns the intersection of two ranges
	// Pre: The ranges must overlap
	[[nodiscard]] constexpr static entity_range intersect(entity_range const& range, entity_range const& other) {
		Pre(range.overlaps(other), "ranges must overlap in order to intersect");

		entity_id const first{std::max(range.first(), other.first())};
		entity_id const last{std::min(range.last(), other.last())};

		return entity_range{first, last};
	}

	// Returns a range that overlaps the two ranges
	[[nodiscard]] constexpr static entity_range overlapping(entity_range const& r1, entity_range const& r2) {
		entity_id const first{std::min(r1.first(), r2.first())};
		entity_id const last{std::max(r1.last(), r2.last())};

		return entity_range{first, last};
	}
};

// The view of a collection of ranges
using entity_range_view = std::span<entity_range const>;

} // namespace ecs

#endif // !ECS_ENTITTY_RANGE_H
#ifndef ECS_DETAIL_PARENT_H
#define ECS_DETAIL_PARENT_H


namespace ecs::detail {

// The parent type stored internally in component pools
ECS_EXPORT struct parent_id : entity_id {
	constexpr parent_id(detail::entity_type _id) noexcept : entity_id(_id) {}
};

} // namespace ecs::detail

#endif // !ECS_DETAIL_PARENT_H
#ifndef ECS_DETAIL_VARIANT_H
#define ECS_DETAIL_VARIANT_H


namespace ecs::detail {
	template <typename T>
	concept has_variant_alias = requires { typename T::variant_of; };

	template <has_variant_alias T>
	using variant_t = typename T::variant_of;

	template <typename A, typename B>
	consteval bool is_variant_of() {
		if constexpr (!has_variant_alias<A> && !has_variant_alias<B>) {
			return false;
		}

		if constexpr (has_variant_alias<A>) {
			static_assert(!std::same_as<A, typename A::variant_of>, "Types can not be variant with themselves");
			if (std::same_as<typename A::variant_of, B>)
				return true;
			if (has_variant_alias<typename A::variant_of>)
				return is_variant_of<typename A::variant_of, B>();
		}

		if constexpr (has_variant_alias<B>) {
			static_assert(!std::same_as<B, typename B::variant_of>, "Types can not be variant with themselves");
			if (std::same_as<typename B::variant_of, A>)
				return true;
			if (has_variant_alias<typename B::variant_of>)
				return is_variant_of<typename B::variant_of, A>();
		}

		return false;
	};

	// Returns false if any types passed are variants of each other
	template <typename First, typename... T>
	consteval bool is_variant_of_pack() {
		if constexpr ((is_variant_of<First, T>() || ...))
			return true;
		else {
			if constexpr (sizeof...(T) > 1)
				return is_variant_of_pack<T...>();
			else
				return false;
		}
	}

	template<typename T, typename V>
	consteval bool check_for_recursive_variant() {
		if constexpr (std::same_as<T, V>)
			return true;
		else if constexpr (!has_variant_alias<V>)
			return false;
		else
			return check_for_recursive_variant<T, variant_t<V>>();
	}

	template <has_variant_alias T>
	consteval bool not_recursive_variant() {
		return !check_for_recursive_variant<T, variant_t<T>>();
	}
	template <typename T>
	consteval bool not_recursive_variant() {
		return true;
	}
} // namespace ecs::detail

#endif
#ifndef ECS_FLAGS_H
#define ECS_FLAGS_H

namespace ecs {
ECS_EXPORT enum ComponentFlags {
	// Add this in a component to mark it as tag.
	// Uses O(1) memory instead of O(n).
	// Mutually exclusive with 'share' and 'global'
	tag = 1 << 0,

	// Add this in a component to mark it as transient.
	// The component will only exist on an entity for one cycle,
	// and then be automatically removed.
	// Mutually exclusive with 'global'
	transient = 1 << 1,

	// Add this in a component to mark it as constant.
	// A compile-time error will be raised if a system tries to
	// write to the component through a reference.
	immutable = 1 << 2,

	// Add this in a component to mark it as global.
	// Global components can be referenced from systems without
	// having been added to any entities.
	// Uses O(1) memory instead of O(n).
	// Mutually exclusive with 'tag', 'share', and 'transient'
	global = 1 << 3
};

ECS_EXPORT template <ComponentFlags... Flags>
struct flags {
	static constexpr int val = (Flags | ...);
};
}

// Some helper concepts/struct to detect flags
namespace ecs::detail {

// Strip a type of all its modifiers
template <typename T>
using stripped_t = std::remove_pointer_t<std::remove_cvref_t<T>>;


template <typename T>
concept tagged = ComponentFlags::tag == (stripped_t<T>::ecs_flags::val & ComponentFlags::tag);

template <typename T>
concept transient = ComponentFlags::transient == (stripped_t<T>::ecs_flags::val & ComponentFlags::transient);

template <typename T>
concept immutable = ComponentFlags::immutable == (stripped_t<T>::ecs_flags::val & ComponentFlags::immutable);

template <typename T>
concept global = ComponentFlags::global == (stripped_t<T>::ecs_flags::val & ComponentFlags::global);

template <typename T>
concept local = !global<T>;

template <typename T>
concept persistent = !transient<T>;

template <typename T>
concept unbound = (tagged<T> || global<T>); // component is not bound to a specific entity (ie static)

//
template <typename T>
struct is_tagged : std::bool_constant<tagged<T>> {};

template <typename T>
struct is_transient : std::bool_constant<transient<T>> {};

template <typename T>
struct is_immutable : std::bool_constant<immutable<T>> {};

template <typename T>
struct is_global : std::bool_constant<global<T>> {};

template <typename T>
struct is_local : std::bool_constant<!global<T>> {};

template <typename T>
struct is_persistent : std::bool_constant<!transient<T>> {};

template <typename T>
struct is_unbound : std::bool_constant<tagged<T> || global<T>> {};

// Returns true if a type is read-only
template <typename T>
constexpr bool is_read_only_v = detail::immutable<T> || detail::tagged<T> || std::is_const_v<std::remove_reference_t<T>>;
} // namespace ecs::detail


// Unittest
namespace {
	struct test_tag {
		using ecs_flags = ecs::flags<ecs::tag>;
	};
	static_assert(ecs::detail::tagged<test_tag>);

	struct test_transient {
		using ecs_flags = ecs::flags<ecs::transient>;
	};
	static_assert(ecs::detail::transient<test_transient>);

	struct test_immutable {
		using ecs_flags = ecs::flags<ecs::immutable>;
	};
	static_assert(ecs::detail::immutable<test_immutable>);
	static_assert(ecs::detail::immutable<test_immutable&>);
	static_assert(ecs::detail::immutable<test_immutable const&>);
	static_assert(ecs::detail::immutable<test_immutable*>);

	struct test_global {
		using ecs_flags = ecs::flags<ecs::global>;
	};
	static_assert(ecs::detail::global<test_global>);
} // namespace
#endif // !ECS_FLAGS_H
#ifndef ECS_DETAIL_STRIDE_VIEW_H
#define ECS_DETAIL_STRIDE_VIEW_H


namespace ecs::detail {

// 
template <std::size_t Stride, typename T>
class stride_view {
	char const* first{nullptr};
	char const* curr{nullptr};
	char const* last{nullptr};

public:
	stride_view() noexcept = default;
	stride_view(T const* first_, std::size_t count_) noexcept
		: first{reinterpret_cast<char const*>(first_)}
		, curr {reinterpret_cast<char const*>(first_)}
		, last {reinterpret_cast<char const*>(first_) + Stride*count_} {
		Pre(first_ != nullptr, "input pointer can not be null");
	}

	consteval std::size_t stride_size() const {
		return Stride;
	}

	T const* current() const noexcept {
		return reinterpret_cast<T const*>(curr);
	}

	bool done() const noexcept {
		return (first==nullptr) || (curr >= last);
	}

	void next() noexcept {
		if (!done())
			curr += Stride;
	}
};

}

#endif //!ECS_DETAIL_STRIDE_VIEW_H
#ifndef ECS_DETAIL_COMPONENT_POOL_BASE_H
#define ECS_DETAIL_COMPONENT_POOL_BASE_H

namespace ecs::detail {
// The baseclass of typed component pools
class component_pool_base {
public:
	component_pool_base() = default;
	component_pool_base(component_pool_base const&) = delete;
	component_pool_base(component_pool_base&&) = delete;
	component_pool_base& operator=(component_pool_base const&) = delete;
	component_pool_base& operator=(component_pool_base&&) = delete;
	virtual ~component_pool_base() = default;

	virtual void process_changes() = 0;
	virtual void clear_flags() = 0;
	virtual void clear() = 0;

	// facilitate variant implementation.
	// Called from other component pools.
	virtual void remove_variant(class entity_range const& range) = 0;
};
} // namespace ecs::detail

#endif // !ECS_DETAIL_COMPONENT_POOL_BASE_H
#ifndef ECS_DETAIL_COMPONENT_POOL_H
#define ECS_DETAIL_COMPONENT_POOL_H





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

		// Skip list stuff
		std::array<chunk*, 2> next;
	};
	static_assert(sizeof(chunk) == 32);

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

	array_scatter_allocator<T> as_alloc;
	array_scatter_allocator<chunk> chunk_alloc;
	std::vector<chunk> chunks;
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
#ifndef ECS_DETAIL_SYSTEM_DEFS_H
#define ECS_DETAIL_SYSTEM_DEFS_H

// Contains definitions that are used by the systems classes

namespace ecs::detail {
template <typename T>
constexpr static bool is_entity = std::is_same_v<std::remove_cvref_t<T>, entity_id>;

// If given a parent, convert to detail::parent_id, otherwise do nothing
template <typename T>
using reduce_parent_t =
	std::conditional_t<std::is_pointer_v<T>,
		std::conditional_t<is_parent<std::remove_pointer_t<T>>::value, parent_id*, T>,
		std::conditional_t<is_parent<T>::value, parent_id, T>>;

// Given a component type, return the naked type without any modifiers.
// Also converts ecs::parent into ecs::detail::parent_id.
template <typename T>
using naked_component_t = std::remove_pointer_t<std::remove_cvref_t<reduce_parent_t<T>>>;

// Alias for stored pools
template <typename T>
using pool = component_pool<naked_component_t<T>>* const;

// Returns true if a type is read-only
template <typename T>
constexpr bool is_read_only() {
	return detail::immutable<T> || detail::tagged<T> || std::is_const_v<std::remove_reference_t<T>>;
}

// Helper to extract the parent types
template <typename T>
struct parent_type_list; // primary template

template <>
struct parent_type_list<void> {
	using type = void;
}; // partial specialization for void

template <template <typename...> typename Parent, typename... ParentComponents> // partial specialization
struct parent_type_list<Parent<ParentComponents...>> {
	using type = type_list<ParentComponents...>;
};
template <typename T>
using parent_type_list_t = typename parent_type_list<std::remove_cvref_t<T>>::type;

// Helper to extract the parent pool types
template <typename T>
struct parent_pool_detect; // primary template

template <template <typename...> typename Parent, typename... ParentComponents> // partial specialization
struct parent_pool_detect<Parent<ParentComponents...>> {
	static_assert(!(is_parent<ParentComponents>::value || ...), "parents in parents not supported");
	using type = std::tuple<pool<ParentComponents>...>;
};
template <typename T>
using parent_pool_tuple_t = typename parent_pool_detect<T>::type;


// The type of a single component argument
template <typename Component>
using component_argument = std::conditional_t<is_parent<std::remove_cvref_t<Component>>::value,
											  std::remove_cvref_t<reduce_parent_t<Component>>*,	// parent components are stored as copies
											  std::remove_cvref_t<Component>*>; // rest are pointers
} // namespace ecs::detail

#endif // !ECS_DETAIL_SYSTEM_DEFS_H
#ifndef ECS_DETAIL_COMPONENT_POOLS_H
#define ECS_DETAIL_COMPONENT_POOLS_H


namespace ecs::detail {

// Forward decls
class component_pool_base;
template <typename, typename> class component_pool;

// Holds a bunch of component pools that can be looked up based on a type.
// The types in the type_list must be naked, so no 'int&' or 'char*' allowed.
template <impl::TypeList ComponentsList>
	requires(std::is_same_v<ComponentsList, transform_type<ComponentsList, naked_component_t>>)
struct component_pools {
	explicit constexpr component_pools(auto... pools) noexcept : base_pools{pools...} {
		Pre(((pools != nullptr) && ...), "pools can not be null");
	}

	// Get arguments corresponding component pool.
	// The type must be naked, ie. 'int' and not 'int&'.
	// This is to prevent the compiler from creating a bajillion
	// different instances that all do the same thing.
	template <typename Component>
		requires(std::is_same_v<Component, naked_component_t<Component>>)
	constexpr auto& get() const noexcept {
		constexpr int index = index_of<Component, ComponentsList>();
		return *static_cast<component_pool<Component>*>(base_pools[index]);
	}

	constexpr bool has_component_count_changed() const {
		return any_of_type<ComponentsList>([this]<typename T>() {
			return this->get<T>().has_component_count_changed();
		});
	}

private:
	component_pool_base* base_pools[type_list_size<ComponentsList>];
};




// Get an entities component from a component pool
template <typename Component, impl::TypeList PoolsList>
[[nodiscard]] auto get_component(entity_id const entity, component_pools<PoolsList> const& pools) {
	using T = std::remove_cvref_t<Component>;

	if constexpr (std::is_pointer_v<T>) {
		// Filter: return a nullptr
		static_cast<void>(entity);
		return static_cast<T*>(nullptr);
	} else if constexpr (tagged<T>) {
		// Tag: return a pointer to some dummy storage
		thread_local char dummy_arr[sizeof(T)];
		return reinterpret_cast<T*>(dummy_arr);
	} else if constexpr (global<T>) {
		// Global: return the shared component
		return &pools.template get<T>().get_shared_component();
	} else if constexpr (std::is_same_v<reduce_parent_t<T>, parent_id>) {
		return pools.template get<parent_id>().find_component_data(entity);
	} else {
		// Standard: return the component from the pool
		return pools.template get<T>().find_component_data(entity);
	}
}

// Extracts a component argument from a pointer+offset
template <typename Component>
decltype(auto) extract_arg_lambda(auto& cmp, [[maybe_unused]] ptrdiff_t offset, [[maybe_unused]] auto pools = std::ptrdiff_t{0}) {
	using T = std::remove_cvref_t<Component>;

	if constexpr (std::is_pointer_v<T>) {
		return static_cast<T>(nullptr);
	} else if constexpr (detail::unbound<T>) {
		T* ptr = cmp;
		return *ptr;
	} else if constexpr (detail::is_parent<T>::value) {
		parent_id const pid = *(cmp + offset);

		// TODO store this in separate container in system_hierarchy? might not be
		//      needed after O(1) pool lookup implementation
		return for_all_types<parent_type_list_t<T>>([&]<typename... ParentTypes>() {
			return T{pid, get_component<ParentTypes>(pid, pools)...};
		});
	} else {
		T* ptr = cmp;
		return *(ptr + offset);
	}
}

}

#endif //!ECS_DETAIL_COMPONENT_POOLS_H
#ifndef ECS_OPTIONS_H
#define ECS_OPTIONS_H

ECS_EXPORT namespace ecs::opts {
	template <int Milliseconds, int Microseconds = 0>
	struct interval {
		static_assert(Milliseconds >= 0, "time values can not be negative");
		static_assert(Microseconds >= 0, "time values can not be negative");
		static_assert(Microseconds <= 999, "microseconds must be in the range 0-999");

		static constexpr int ms = Milliseconds;
		static constexpr int us = Microseconds;
	};

	struct manual_update {};

	struct not_parallel {};
	// struct not_concurrent {};

} // namespace ecs::opts

#endif // !ECS_OPTIONS_H
#ifndef ECS_PARENT_H
#define ECS_PARENT_H


// forward decls
namespace ecs::detail {
	//template <typename Pools> struct pool_entity_walker;
	//template <typename Pools> struct pool_range_walker;

	template<std::size_t Size>
	struct void_ptr_storage {
		void* te_ptrs[Size];
	};
	struct empty_storage {
	};
}

namespace ecs {
// Special component that allows parent/child relationships
ECS_EXPORT template <typename... ParentTypes>
struct parent : entity_id, private std::conditional_t<(sizeof...(ParentTypes) > 0), detail::void_ptr_storage<sizeof...(ParentTypes)>, detail::empty_storage> {
	static_assert((!detail::global<ParentTypes> && ...), "global components are not allowed in parents");
	static_assert((!detail::is_parent<ParentTypes>::value && ...), "parents in parents is not supported");

	explicit parent(entity_id id) : entity_id(id) {}

	parent(parent const&) = default;
	parent& operator=(parent const&) = default;

	entity_id id() const {
		return static_cast<entity_id>(*this);
	}

	template <typename T>
	[[nodiscard]] T& get() const {
		static_assert((std::is_same_v<T, ParentTypes> || ...), "T is not specified in the parent component");
		return *static_cast<T*>(this->te_ptrs[detail::index_of<T, parent_type_list>()]);
	}

	// used internally by detectors
	struct _ecs_parent {};

private:
	using parent_type_list = detail::type_list<ParentTypes...>;

	template <typename Component>
	friend decltype(auto) detail::extract_arg_lambda(auto& cmp, ptrdiff_t offset, auto pools);

	//template <typename Pools> friend struct detail::pool_entity_walker;
	//template <typename Pools> friend struct detail::pool_range_walker;

	parent(entity_id id, ParentTypes*... pt)
		requires(sizeof...(ParentTypes) > 0)
		: entity_id(id) {
		((this->te_ptrs[detail::index_of<ParentTypes, parent_type_list>()] = static_cast<void*>(pt)), ...);
	}
};
} // namespace ecs

#endif // !ECS_PARENT_H
#ifndef ECS_DETAIL_INTERVAL_LIMITER_H
#define ECS_DETAIL_INTERVAL_LIMITER_H


namespace ecs::detail {

template <int milliseconds, int microseconds>
struct interval_limiter {
	bool can_run() {
		using namespace std::chrono_literals;
		static constexpr std::chrono::nanoseconds interval_size = 1ms * milliseconds + 1us * microseconds;

		auto const now = std::chrono::high_resolution_clock::now();
		auto const diff = now - time;
		if (diff >= interval_size) {
			time = now;
			return true;
		} else {
			return false;
		}
	}

private:
	std::chrono::high_resolution_clock::time_point time = std::chrono::high_resolution_clock::now();
};

template <>
struct interval_limiter<0, 0> {
	static constexpr bool can_run() {
		return true;
	}
};

} // namespace ecs::detail

#endif // !ECS_DETAIL_INTERVAL_LIMITER_H
#ifndef ECS_DETAIL_VERIFICATION_H
#define ECS_DETAIL_VERIFICATION_H



namespace ecs::detail {

// Given a type T, if it is callable with an entity argument,
// resolve to the return type of the callable. Otherwise assume the type T.
template <typename T>
using get_type_t = std::conditional_t<std::invocable<T, entity_type>,
	std::invoke_result<T, entity_type>,
	std::type_identity<T>>;


// Returns true if all types passed are unique
template <typename First, typename... T>
constexpr bool is_unique_type_args() {
	if constexpr ((std::is_same_v<First, T> || ...))
		return false;
	else {
		if constexpr (sizeof...(T) == 0)
			return true;
		else
			return is_unique_type_args<T...>();
	}
}


// Find the types a sorting predicate takes
template <typename R, typename T>
constexpr std::remove_cvref_t<T> get_sorter_type(R (*)(T, T)) { return T{}; }			// Standard function

template <typename R, typename C, typename T>
constexpr std::remove_cvref_t<T> get_sorter_type(R (C::*)(T, T) const) {return T{}; }	// const member function
template <typename R, typename C, typename T>
constexpr std::remove_cvref_t<T> get_sorter_type(R (C::*)(T, T) ) {return T{}; }			// mutable member function


template <typename Pred>
constexpr auto get_sorter_type() {
	// Verify predicate
	static_assert(
		requires {
			{ Pred{}({}, {}) } -> std::same_as<bool>;
		},
		"predicates must take two arguments and return a bool");

	if constexpr (requires { &Pred::operator(); }) {
		return get_sorter_type(&Pred::operator());
	} else {
		return get_sorter_type(Pred{});
	}
}

template <typename Pred>
using sorter_predicate_type_t = decltype(get_sorter_type<Pred>());


// Implement the requirements for ecs::parent components
template <typename C>
constexpr void verify_parent_component() {
	if constexpr (detail::is_parent<C>::value) {
		using parent_subtypes = parent_type_list_t<C>;
		size_t const total_subtypes = type_list_size<parent_subtypes>;

		if constexpr (total_subtypes > 0) {
			// Count all the filters in the parent type
			size_t const num_subtype_filters = count_type_if<parent_subtypes, std::is_pointer>();

			// Count all the types minus filters in the parent type
			size_t const num_parent_subtypes = total_subtypes - num_subtype_filters;

			// If there is one-or-more sub-components,
			// then the parent must be passed as a const reference or value
			if constexpr (num_parent_subtypes > 0) {
				if constexpr (std::is_reference_v<C>) {
					static_assert(std::is_const_v<std::remove_reference_t<C>>, "parent with non-filter sub-components must be passed as a value or const&");
				}
			}
		}
	}
}

// Implement the requirements for tagged components
template <typename C>
constexpr void verify_tagged_component() {
	if constexpr (!std::is_pointer_v<C> && detail::tagged<C>)
		static_assert(!std::is_reference_v<C> && (sizeof(C) == 1), "components flagged as 'tag' must not be references");
}

// Implement the requirements for global components
template <typename C>
constexpr void verify_global_component() {
	if constexpr (detail::global<C>)
		static_assert(!detail::tagged<C> && !detail::transient<C>, "components flagged as 'global' must not be 'tag's or 'transient'");
}

// Implement the requirements for immutable components
template <typename C>
constexpr void verify_immutable_component() {
	if constexpr (detail::immutable<C>) {
		static_assert(std::is_const_v<std::remove_reference_t<C>>, "components flagged as 'immutable' must also be const");
	}
}

template <typename R, typename FirstArg, typename... Args>
constexpr void system_verifier() {
	static_assert(std::is_same_v<R, void>, "systems can not have return values");

	static_assert(is_unique_type_args < std::remove_cvref_t<FirstArg>, std::remove_cvref_t<Args>...>(),
				  "component parameter types can only be specified once");

	if constexpr (is_entity<FirstArg>) {
		static_assert(sizeof...(Args) > 0, "systems must take at least one component argument");

		// Make sure the first entity is not passed as a reference
		static_assert(!std::is_reference_v<FirstArg>, "ecs::entity_id must not be passed as a reference");
	} else {
		verify_immutable_component<FirstArg>();
		verify_global_component<FirstArg>();
		verify_tagged_component<FirstArg>();
		verify_parent_component<FirstArg>();
	}

	(verify_immutable_component<Args>(), ...);
	(verify_global_component<Args>(), ...);
	(verify_tagged_component<Args>(), ...);
	(verify_parent_component<Args>(), ...);
}

// A small bridge to allow the Lambda to activate the system verifier
template <typename R, typename C, typename FirstArg, typename... Args>
constexpr void system_to_lambda_bridge(R (C::*)(FirstArg, Args...)) { system_verifier<R, FirstArg, Args...>(); }
template <typename R, typename C, typename FirstArg, typename... Args>
constexpr void system_to_lambda_bridge(R (C::*)(FirstArg, Args...) const) { system_verifier<R, FirstArg, Args...>(); }
template <typename R, typename C, typename FirstArg, typename... Args>
constexpr void system_to_lambda_bridge(R (C::*)(FirstArg, Args...) noexcept) { system_verifier<R, FirstArg, Args...>(); }
template <typename R, typename C, typename FirstArg, typename... Args>
constexpr void system_to_lambda_bridge(R (C::*)(FirstArg, Args...) const noexcept) { system_verifier<R, FirstArg, Args...>(); }

// A small bridge to allow the function to activate the system verifier
template <typename R, typename FirstArg, typename... Args>
constexpr void system_to_func_bridge(R (*)(FirstArg, Args...)) { system_verifier<R, FirstArg, Args...>(); }
template <typename R, typename FirstArg, typename... Args>
constexpr void system_to_func_bridge(R (*)(FirstArg, Args...) noexcept) { system_verifier<R, FirstArg, Args...>(); }

template <typename T>
concept type_is_lambda = requires {
	&T::operator();
};

template <typename T>
concept type_is_function = requires(T t) {
	system_to_func_bridge(t);
};

template <typename OptionsTypeList, typename SystemFunc, typename SortFunc>
constexpr bool make_system_parameter_verifier() {
	bool constexpr is_lambda = type_is_lambda<SystemFunc>;
	bool constexpr is_func = type_is_function<SystemFunc>;

	static_assert(is_lambda || is_func, "Systems can only be created from lambdas or free-standing functions");

	// verify the system function
	if constexpr (is_lambda) {
		system_to_lambda_bridge(&SystemFunc::operator());
	} else if constexpr (is_func) {
		system_to_func_bridge(SystemFunc{});
	}

	// verify the sort function
	if constexpr (!std::is_same_v<std::nullptr_t, SortFunc>) {
		bool constexpr is_sort_lambda = type_is_lambda<SortFunc>;
		bool constexpr is_sort_func = type_is_function<SortFunc>;

		static_assert(is_sort_lambda || is_sort_func, "invalid sorting function");

		using sort_types = sorter_predicate_type_t<SortFunc>;
		static_assert(std::predicate<SortFunc, sort_types, sort_types>, "Sorting function is not a predicate");
	}

	return true;
}

} // namespace ecs::detail

#endif // !ECS_DETAIL_VERIFICATION_H
#ifndef ECS_DETAIL_ENTITY_RANGE_H
#define ECS_DETAIL_ENTITY_RANGE_H


// Find the intersections between two sets of ranges
namespace ecs::detail {
template <typename Iter>
struct iter_pair {
	Iter curr;
	Iter end;
};

template <typename Iter1, typename Iter2>
std::vector<entity_range> intersect_ranges_iter(iter_pair<Iter1> it_a, iter_pair<Iter2> it_b) {
	std::vector<entity_range> result;

	while (it_a.curr != it_a.end && it_b.curr != it_b.end) {
		if (it_a.curr->overlaps(*it_b.curr)) {
			result.push_back(entity_range::intersect(*it_a.curr, *it_b.curr));
		}

		if (it_a.curr->last() < it_b.curr->last()) { // range a is inside range b, move to
													 // the next range in a
			++it_a.curr;
		} else if (it_b.curr->last() < it_a.curr->last()) { // range b is inside range a,
															// move to the next range in b
			++it_b.curr;
		} else { // ranges are equal, move to next ones
			++it_a.curr;
			++it_b.curr;
		}
	}

	return result;
}

// Find the intersectsions between two sets of ranges
/* inline std::vector<entity_range> intersect_ranges(entity_range_view view_a, entity_range_view view_b) {
	std::vector<entity_range> result;

	if (view_a.empty() || view_b.empty()) {
		return result;
	}

	auto it_a = view_a.begin();
	auto it_b = view_b.begin();

	while (it_a != view_a.end() && it_b != view_b.end()) {
		if (it_a->overlaps(*it_b)) {
			result.push_back(entity_range::intersect(*it_a, *it_b));
		}

		if (it_a->last() < it_b->last()) { // range a is inside range b, move to
										   // the next range in a
			++it_a;
		} else if (it_b->last() < it_a->last()) { // range b is inside range a,
												  // move to the next range in b
			++it_b;
		} else { // ranges are equal, move to next ones
			++it_a;
			++it_b;
		}
	}

	return result;
}*/

// Merges a range into the last range in the vector, or adds a new range
inline void merge_or_add(std::vector<entity_range>& v, entity_range r) {
	if (!v.empty() && v.back().adjacent(r))
		v.back() = entity_range::merge(v.back(), r);
	else
		v.push_back(r);
}

// Find the difference between two sets of ranges.
// Removes ranges in b from a.
inline std::vector<entity_range> difference_ranges(entity_range_view view_a, entity_range_view view_b) {
	if (view_a.empty())
		return {};
	if (view_b.empty())
		return {view_a.begin(), view_a.end()};

	std::vector<entity_range> result;
	auto it_a = view_a.begin();
	auto it_b = view_b.begin();

	auto range_a = *it_a;
	while (it_a != view_a.end()) {
		if (it_b == view_b.end()) {
			result.push_back(range_a);
			if (++it_a != view_a.end())
				range_a = *it_a;
		} else if (it_b->contains(range_a)) {
			// Range 'a' is contained entirely in range 'b',
			// which means that 'a' will not be added.
			if (++it_a != view_a.end())
				range_a = *it_a;
		} else if (range_a < *it_b) {
			// The whole 'a' range is before 'b', so add range 'a'
			result.push_back(range_a);

			if (++it_a != view_a.end())
				range_a = *it_a;
		} else if (*it_b < range_a) {
			// The whole 'b' range is before 'a', so move ahead
			++it_b;
		} else {
			// The two ranges overlap

			auto const res = entity_range::remove(range_a, *it_b);

			if (res.second) {
				// Range 'a' was split in two by range 'b'. Add the first range and update
				// range 'a' with the second range
				result.push_back(res.first);
				range_a = *res.second;

				++it_b;
			} else {
				// Range 'b' removes some of range 'a'

				if (range_a.first() >= it_b->first()) {
					// The result is an endpiece, so update the current range.
					// The next 'b' might remove more from the current 'a'
					range_a = res.first;

					++it_b;
				} else {
					// Add the range
					result.push_back(res.first);

					if (++it_a != view_a.end())
						range_a = *it_a;
				}
			}
		}
	}

	return result;
}
} // namespace ecs::detail

#endif // !ECS_DETAIL_ENTITY_RANGE_H
#ifndef ECS_FIND_ENTITY_POOL_INTERSECTIONS_H
#define ECS_FIND_ENTITY_POOL_INTERSECTIONS_H


namespace ecs::detail {

// Given a list of components, return an array containing the corresponding component pools
template <typename ComponentsList, typename PoolsList>
	requires(std::is_same_v<ComponentsList, transform_type<ComponentsList, naked_component_t>>)
auto get_pool_iterators([[maybe_unused]] component_pools<PoolsList> const& pools) {
	if constexpr (type_list_is_empty<ComponentsList>) {
		return std::array<stride_view<0, char const>, 0>{};
	} else {
		// Verify that the component list passed has a corresponding pool
		for_each_type<ComponentsList>([]<typename T>() {
			static_assert(contains_type<T, PoolsList>(), "A component is missing its corresponding component pool");
		});

		return for_all_types<ComponentsList>([&]<typename... Components>() {
			return std::to_array({pools.template get<Components>().get_entities()...});
		});
	}
}


// Find the intersection of the sets of entities in the specified pools
template <typename InputList, typename PoolsList, typename F>
void find_entity_pool_intersections_cb(component_pools<PoolsList> const& pools, F&& callback) {
	static_assert(not type_list_is_empty<InputList>, "Empty component list supplied");

	// Split the type_list into filters and non-filters (regular components).
	using FilterComponentPairList = split_types_if<InputList, std::is_pointer>;
	using FilterList = transform_type<typename FilterComponentPairList::first, naked_component_t>;
	using ComponentList = typename FilterComponentPairList::second;

	// Filter local components.
	// Global components are available for all entities
	// so don't bother wasting cycles on testing them.
	using LocalComponentList = transform_type<filter_types_if<ComponentList, detail::is_local>, naked_component_t>;
	auto iter_components = get_pool_iterators<LocalComponentList>(pools);

	// If any of the pools are empty, bail
	bool const any_emtpy_pools = std::ranges::any_of(iter_components, [](auto p) {
		return p.current() == nullptr;
	});
	if (any_emtpy_pools)
		return;

	// Get the iterators for the filters
	auto iter_filters = get_pool_iterators<FilterList>(pools);

	// Sort the filters
	std::sort(iter_filters.begin(), iter_filters.end(), [](auto const& a, auto const& b) {
		if (a.current() && b.current())
			return *a.current() < *b.current();
		else
			return nullptr != a.current();
	});

	// helper lambda to test if an iterator has reached its end
	auto const done = [](auto const& it) {
		return it.done();
	};

	while (!std::any_of(iter_components.begin(), iter_components.end(), done)) {
		// Get the starting range to test other ranges against
		entity_range curr_range = *iter_components[0].current();

		// Find all intersections
		if constexpr (type_list_size<LocalComponentList> == 1) {
			iter_components[0].next();
		} else {
			bool intersection_found = false;
			for (size_t i = 1; i < iter_components.size(); ++i) {
				auto& it_a = iter_components[i - 1];
				auto& it_b = iter_components[i];

				if (curr_range.overlaps(*it_b.current())) {
					curr_range = entity_range::intersect(curr_range, *it_b.current());
					intersection_found = true;
				}

				if (it_a.current()->last() < it_b.current()->last()) {
					// range a is inside range b, move to
					// the next range in a
					it_a.next();
					if (it_a.done())
						break;

				} else if (it_b.current()->last() < it_a.current()->last()) {
					// range b is inside range a,
					// move to the next range in b
					it_b.next();
				} else {
					// ranges are equal, move to next ones
					it_a.next();
					if (it_a.done())
						break;

					it_b.next();
				}
			}

			if (!intersection_found)
				continue;
		}

		// Filter the range, if needed
		if constexpr (type_list_size<FilterList> > 0) {
			bool completely_filtered = false;
			for (auto& it : iter_filters) {
				while(!done(it)) {
					if (it.current()->contains(curr_range)) {
						// 'curr_range' is contained entirely in filter range,
						// which means that it will not be sent to the callback
						completely_filtered = true;
						break;
					} else if (curr_range < *it.current()) {
						// The whole 'curr_range' is before the filter, so don't touch it
						break;
					} else if (*it.current() < curr_range) {
						// The filter precedes the range, so advance it and restart
						it.next();
						continue;
					} else {
						// The two ranges overlap
						auto const res = entity_range::remove(curr_range, *it.current());

						if (res.second) {
							// 'curr_range' was split in two by the filter.
							// Send the first range and update
							// 'curr_range' to be the second range
							callback(res.first);
							curr_range = *res.second;

							it.next();
						} else {
							// The result is an endpiece, so update the current range.
							curr_range = res.first;

							// The next filter might remove more from 'curr_range', so don't just
							// skip past it without checking
							if (curr_range.first() >= it.current()->first()) {
								it.next();
							}
						}
					}
				}
			}

			if (!completely_filtered)
				callback(curr_range);
		} else {
			// No filters on this range, so send
			callback(curr_range);
		}
	}
}

} // namespace ecs::detail

#endif // !ECS_FIND_ENTITY_POOL_INTERSECTIONS_H
#ifndef ECS_DETAIL_SYSTEM_BASE_H
#define ECS_DETAIL_SYSTEM_BASE_H


namespace ecs::detail {
class context;

class system_base {
public:
	system_base() = default;
	virtual ~system_base() = default;
	system_base(system_base const&) = delete;
	system_base(system_base&&) = default;
	system_base& operator=(system_base const&) = delete;
	system_base& operator=(system_base&&) = default;

	// Run this system on all of its associated components
	virtual void run() = 0;

	// Enables this system for updates and runs
	void enable() {
		set_enable(true);
	}

	// Prevent this system from being updated or run
	void disable() {
		set_enable(false);
	}

	// Sets wheter the system is enabled or disabled
	void set_enable(bool is_enabled) {
		enabled = is_enabled;
		if (is_enabled) {
			process_changes(true);
		}
	}

	// Returns true if this system is enabled
	[[nodiscard]] bool is_enabled() const {
		return enabled;
	}

	// Get the hashes of types used by the system with const/reference qualifiers removed
	[[nodiscard]] virtual std::span<detail::type_hash const> get_type_hashes() const noexcept = 0;

	// Returns true if this system uses the type
	[[nodiscard]] virtual bool has_component(detail::type_hash hash) const noexcept = 0;

	// Returns true if this system has a dependency on another system
	[[nodiscard]] virtual bool depends_on(system_base const*) const noexcept = 0;

	// Returns true if this system writes data to a specific component
	[[nodiscard]] virtual bool writes_to_component(detail::type_hash hash) const noexcept = 0;

private:
	// Only allow the context class to call 'process_changes'
	friend class detail::context;

	// Process changes to component layouts
	virtual void process_changes(bool force_rebuild = false) = 0;

	// Whether this system is enabled or disabled. Disabled systems are neither updated nor run.
	bool enabled = true;
};
} // namespace ecs::detail

#endif // !ECS_DETAIL_SYSTEM_BASE_H
#ifndef ECS_DETAIL_SYSTEM_H
#define ECS_DETAIL_SYSTEM_H



namespace ecs::detail {

// The implementation of a system specialized on its components
template <typename Options, typename UpdateFn, bool FirstIsEntity, typename ComponentsList, typename PoolsList>
class system : public system_base {
	virtual void do_run() = 0;
	virtual void do_build() = 0;

public:
	system(UpdateFn func, component_pools<PoolsList>&& in_pools)
		: update_func{func}, pools{std::forward<component_pools<PoolsList>>(in_pools)} {
	}

	void run() override {
		if (!is_enabled()) {
			return;
		}

		if (!interval_checker.can_run()) {
			return;
		}

		do_run();

		// Notify pools if data was written to them
		for_each_type<ComponentsList>([this]<typename T>() {
			this->notify_pool_modifed<T>();
		});
	}

	template <typename T>
	void notify_pool_modifed() {
		if constexpr (detail::is_parent<T>::value && !is_read_only<T>()) { // writeable parent
			// Recurse into the parent types
			for_each_type<parent_type_list_t<T>>([this]<typename... ParentTypes>() {
				(this->notify_pool_modifed<ParentTypes>(), ...);
			});
		} else if constexpr (std::is_reference_v<T> && !is_read_only<T>() && !std::is_pointer_v<T>) {
			pools.template get<std::remove_reference_t<T>>().notify_components_modified();
		}
	}

	constexpr std::span<detail::type_hash const> get_type_hashes() const noexcept override {
		return type_hashes;
	}

	constexpr bool has_component(detail::type_hash hash) const noexcept override {
		auto const check_hash = [hash]<typename T>() {
			return get_type_hash<T>() == hash;
		};

		if (any_of_type<stripped_component_list>(check_hash))
			return true;

		if constexpr (has_parent_types) {
			return any_of_type<parent_component_list>(check_hash);
		} else {
			return false;
		}
	}

	constexpr bool depends_on(system_base const* other) const noexcept override {
		return any_of_type<stripped_component_list>([this, other]<typename T>() {
			constexpr auto hash = get_type_hash<T>();

			// If the other system doesn't touch the same component,
			// then there can be no dependecy
			if (!other->has_component(hash))
				return false;

			bool const other_writes = other->writes_to_component(hash);
			if (other_writes) {
				// The other system writes to the component,
				// so there is a strong dependency here.
				// Order is preserved.
				return true;
			} else { // 'other' reads component
				bool const this_writes = writes_to_component(hash);
				if (this_writes) {
					// This system writes to the component,
					// so there is a strong dependency here.
					// Order is preserved.
					return true;
				} else {
					// These systems have a weak read/read dependency
					// and can be scheduled concurrently
					// Order does not need to be preserved.
					return false;
				}
			}
		});
	}

	constexpr bool writes_to_component(detail::type_hash hash) const noexcept override {
		auto const check_writes = [hash]<typename T>() {
			return get_type_hash<std::remove_cvref_t<T>>() == hash && !is_read_only<T>();
		};

		if (any_of_type<ComponentsList>(check_writes))
			return true;

		if constexpr (has_parent_types) {
			return any_of_type<parent_component_list>(check_writes);
		} else {
			return false;
		}
	}

protected:
	// Handle changes when the component pools change
	void process_changes(bool force_rebuild) override {
		if (force_rebuild) {
			do_build();
			return;
		}

		if (!is_enabled()) {
			return;
		}

		if (pools.has_component_count_changed()) {
			do_build();
		}
	}

protected:
	// Number of components
	static constexpr size_t num_components = type_list_size<ComponentsList>;

	// List of components used, with all modifiers stripped
	using stripped_component_list = transform_type<ComponentsList, std::remove_cvref_t>;

	using user_interval = test_option_type_or<is_interval, Options, opts::interval<0, 0>>;
	using interval_type = interval_limiter<user_interval::ms, user_interval::us>;

	//
	// ecs::parent related stuff

	// The parent type, or void
	using full_parent_type = test_option_type_or<is_parent, stripped_component_list, void>;
	using stripped_parent_type = std::remove_pointer_t<std::remove_cvref_t<full_parent_type>>;
	using parent_component_list = parent_type_list_t<stripped_parent_type>;
	static constexpr bool has_parent_types = !std::is_same_v<full_parent_type, void>;


	// Number of filters
	static constexpr size_t num_filters = count_type_if<ComponentsList, std::is_pointer>();
	static_assert(num_components-num_filters > 0, "systems must have at least one non-filter component");

	// Hashes of stripped types used by this system ('int' instead of 'int const&')
	static constexpr std::array<detail::type_hash, num_components> type_hashes = get_type_hashes_array<stripped_component_list>();

	// The user supplied system
	UpdateFn update_func;

	// Fully typed component pools used by this system
	component_pools<PoolsList> const pools;

	interval_type interval_checker;
};
} // namespace ecs::detail

#endif // !ECS_DETAIL_SYSTEM_H
#ifndef ECS_DETAIL_SYSTEM_HIERARCHY_H
#define ECS_DETAIL_SYSTEM_HIERARCHY_H


namespace ecs::detail {
template <typename Options, typename UpdateFn, bool FirstIsEntity, typename ComponentsList, typename CombinedList, typename PoolsList>
class system_hierarchy final : public system<Options, UpdateFn, FirstIsEntity, CombinedList, PoolsList> {
	using base = system<Options, UpdateFn, FirstIsEntity, CombinedList, PoolsList>;

	// Is parallel execution wanted
	static constexpr bool is_parallel = !ecs::detail::has_option<opts::not_parallel, Options>();

	struct location {
		std::uint32_t index;
		entity_offset offset;
		auto operator<=>(location const&) const = default;
	};
	struct entity_info {
		std::uint32_t parent_count;
		entity_type root_id;
		location l;
		auto operator<=>(entity_info const&) const = default;
	};
	struct hierarchy_span {
		unsigned offset, count;
	};

public:
	system_hierarchy(UpdateFn func, component_pools<PoolsList>&& in_pools)
		: base{func, std::forward<component_pools<PoolsList>>(in_pools)} {
		pool_parent_id = &this->pools.template get<parent_id>();
		this->process_changes(true);
	}

private:
	void do_run() override {
		auto const& this_pools = this->pools;

		if constexpr (is_parallel) {
			std::for_each(std::execution::par, info_spans.begin(), info_spans.end(), [&](hierarchy_span span) {
				auto const ei_span = std::span<entity_info>{infos.data() + span.offset, span.count};
				for (entity_info const& info : ei_span) {
					arguments[info.l.index](this->update_func, info.l.offset, this_pools);
				}
			});
		} else {
			for (entity_info const& info : infos) {
				arguments[info.l.index](this->update_func, info.l.offset, this->pools);
			}
		}
	}

	// Convert a set of entities into arguments that can be passed to the system
	void do_build() override {
		ranges.clear();
		ents_to_remove.clear();

		// Find the entities
		find_entity_pool_intersections_cb<ComponentsList>(this->pools, [&](entity_range const range) {
			ranges.push_back(range);

			// Get the parent ids in the range
			parent_id const* pid_ptr = pool_parent_id->find_component_data(range.first());

			// the ranges to remove
			for_each_type<parent_component_list>([&]<typename T>() {
				// Get the pool of the parent sub-component
				using X = std::remove_pointer_t<T>;
				component_pool<X> const& sub_pool = this->pools.template get<X>();

				size_t pid_index = 0;
				for (entity_id const ent : range) {
					parent_id const pid = pid_ptr[pid_index];
					pid_index += 1;

					// Does tests on the parent sub-components to see they satisfy the constraints
					// ie. a 'parent<int*, float>' will return false if the parent does not have a float or
					// has an int.
					if (std::is_pointer_v<T> == sub_pool.has_entity(pid)) {
						// Above 'if' statement is the same as
						//   if (!pointer && !has_entity)	// The type is a filter, so the parent is _not_ allowed to have this component
						//   if (pointer && has_entity)		// The parent must have this component
						merge_or_add(ents_to_remove, entity_range{ent, ent});
					}
				}
			});
		});

		// Remove entities from the result
		ranges = difference_ranges(ranges, ents_to_remove);

		// Clear the arguments
		arguments.clear();
		infos.clear();
		info_spans.clear();

		if (ranges.empty()) {
			return;
		}

		// Build the arguments for the ranges
		for_all_types<ComponentsList>([&]<typename... T>() {
			for (unsigned index = 0; entity_range const range : ranges) {
				arguments.push_back(make_argument<T...>(range, get_component<T>(range.first(), this->pools)...));

				for (entity_id const id : range) {
					infos.push_back({0, *(pool_parent_id->find_component_data(id)), {index, range.offset(id)}});
				}

				index += 1;
			}
		});

		// partition the roots
		auto it = std::partition(infos.begin(), infos.end(), [&](entity_info const& info) {
			return false == pool_parent_id->has_entity(info.root_id);
		});

		// Keep partitioning if there are more levels in the hierarchies
		if (it != infos.begin()) {
			// data needed by the partition lambda
			auto prev_it = infos.begin();
			unsigned hierarchy_level = 1;

			// The lambda used to partition non-root entities
			const auto parter = [&](entity_info& info) {
				// update the parent count while we are here anyway
				info.parent_count = hierarchy_level;

				// Look for the parent in the previous partition
				auto const parent_it = std::find_if(prev_it, it, [&](entity_info const& parent_info) {
					entity_id const parent_id = ranges[parent_info.l.index].at(parent_info.l.offset);
					return parent_id == info.root_id;
				});

				if (it != parent_it) {
					// Propagate the root id downwards to its children
					info.root_id = parent_it->root_id;

					// A parent was found in the previous partition
					return true;
				}
				return false;
			};

			// partition the levels below the roots
			while (it != infos.end()) {
				auto next_it = std::partition(it, infos.end(), parter);

				// Nothing was partitioned, so leave
				if (next_it == it)
					break;

				// Update the partition iterators
				prev_it = it;
				it = next_it;

				hierarchy_level += 1;
			}
		}

		// Do the topological sort of the arguments
		std::sort(infos.begin(), infos.end());

		// The spans are only needed for parallel execution
		if constexpr (is_parallel) {
			// Create the spans
			auto current_root = infos.front().root_id;
			unsigned count = 1;
			unsigned offset = 0;

			for (size_t i = 1; i < infos.size(); i++) {
				entity_info const& info = infos[i];
				if (current_root != info.root_id) {
					info_spans.push_back({offset, count});

					current_root = info.root_id;
					offset += count;
					count = 0;
				}

				// TODO compress 'infos' into 'location's inplace

				count += 1;
			}
			info_spans.push_back({offset, static_cast<unsigned>(infos.size() - offset)});
		}
	}

	template <typename... Ts>
	static auto make_argument(entity_range range, auto... args) noexcept {
		return [=](auto update_func, entity_offset offset, component_pools<PoolsList> const& pools) mutable {
			entity_id const ent = static_cast<entity_type>(static_cast<entity_offset>(range.first()) + offset);
			if constexpr (FirstIsEntity) {
				update_func(ent, extract_arg_lambda<Ts>(args, offset, pools)...);
			} else {
				update_func(/**/ extract_arg_lambda<Ts>(args, offset, pools)...);
			}
		};
	}

private:
	using base::has_parent_types;
	using typename base::full_parent_type;
	using typename base::parent_component_list;
	using typename base::stripped_component_list;
	using typename base::stripped_parent_type;

	// The argument for parameter to pass to system func
	using base_argument_ptr = decltype(for_all_types<ComponentsList>([]<typename... Types>() {
		return make_argument<Types...>(entity_range{0, 0}, component_argument<Types>{0}...);
	}));
	using base_argument = std::remove_const_t<base_argument_ptr>;

	// Ensure we have a parent type
	static_assert(has_parent_types, "no parent component found");

	// The vector of entity/parent info
	std::vector<entity_info> infos;

	// The vector of unrolled arguments
	std::vector<base_argument> arguments;

	// The spans over each tree in the argument vector.
	// Only used for parallel execution
	std::vector<hierarchy_span> info_spans;

	std::vector<entity_range> ranges;
	std::vector<entity_range> ents_to_remove;

	// The pool that holds 'parent_id's
	component_pool<parent_id> const* pool_parent_id;
};
} // namespace ecs::detail

#endif // !ECS_DETAIL_SYSTEM_HIERARCHY_H
#ifndef ECS_DETAIL_SYSTEM_SORTED_H
#define ECS_DETAIL_SYSTEM_SORTED_H


namespace ecs::detail {
// Manages sorted arguments. Neither cache- nor storage space friendly, but arguments
// will be passed to the user supplied lambda in a sorted manner
template <typename Options, typename UpdateFn, typename SortFunc, bool FirstIsEntity, typename ComponentsList, typename PoolsList>
struct system_sorted final : public system<Options, UpdateFn, FirstIsEntity, ComponentsList, PoolsList> {
	using base = system<Options, UpdateFn, FirstIsEntity, ComponentsList, PoolsList>;

	// Determine the execution policy from the options (or lack thereof)
	using execution_policy = std::conditional_t<ecs::detail::has_option<opts::not_parallel, Options>(), std::execution::sequenced_policy,
												std::execution::parallel_policy>;

public:
	system_sorted(UpdateFn func, SortFunc sort, component_pools<PoolsList>&& in_pools)
		: base{func, std::forward<component_pools<PoolsList>>(in_pools)}, sort_func{sort} {
		this->process_changes(true);
	}

private:
	void do_run() override {
		// Sort the arguments if the component data has been modified
		if (needs_sorting || this->pools.template get<sort_types>().has_components_been_modified()) {
			auto const e_p = execution_policy{}; // cannot pass 'execution_policy{}' directly to for_each in gcc
			std::sort(e_p, sorted_args.begin(), sorted_args.end(), [this](sort_help const& l, sort_help const& r) {
				return sort_func(*l.sort_val_ptr, *r.sort_val_ptr);
			});

			needs_sorting = false;
		}

		for (sort_help const& sh : sorted_args) {
			lambda_arguments[sh.arg_index](this->update_func, sh.offset);
		}
	}

	// Convert a set of entities into arguments that can be passed to the system
	void do_build() override {
		sorted_args.clear();
		lambda_arguments.clear();

		for_all_types<ComponentsList>([&]<typename... Types>() {
			find_entity_pool_intersections_cb<ComponentsList>(this->pools, [this, index = 0u](entity_range range) mutable {
				lambda_arguments.push_back(make_argument<Types...>(range, get_component<Types>(range.first(), this->pools)...));

				for (entity_id const entity : range) {
					entity_offset const offset = range.offset(entity);
					sorted_args.push_back({index, offset, get_component<sort_types>(entity, this->pools)});
				}

				index += 1;
			});
		});

		needs_sorting = true;
	}

	template <typename... Ts>
	static auto make_argument(entity_range range, auto... args) {
		return [=](auto update_func, entity_offset offset) {
			entity_id const ent = static_cast<entity_type>(static_cast<entity_offset>(range.first()) + offset);
			if constexpr (FirstIsEntity) {
				update_func(ent, extract_arg_lambda<Ts>(args, offset, 0)...);
			} else {
				update_func(/**/ extract_arg_lambda<Ts>(args, offset, 0)...);
			}
		};
	}

private:
	// The user supplied sorting function
	SortFunc sort_func;

	// The type used for sorting
	using sort_types = sorter_predicate_type_t<SortFunc>;

	// True if the data needs to be sorted
	bool needs_sorting = false;

	struct sort_help {
		unsigned arg_index;
		entity_offset offset;
		sort_types* sort_val_ptr;
	};
	std::vector<sort_help> sorted_args;

	using base_argument = decltype(for_all_types<ComponentsList>([]<typename... Types>() {
			return make_argument<Types...>(entity_range{0,0}, component_argument<Types>{}...);
		}));
	
	std::vector<std::remove_const_t<base_argument>> lambda_arguments;
};
} // namespace ecs::detail

#endif // !ECS_DETAIL_SYSTEM_SORTED_H
#ifndef ECS_DETAIL_SYSTEM_RANGED_H
#define ECS_DETAIL_SYSTEM_RANGED_H


namespace ecs::detail {
// Manages arguments using ranges. Very fast linear traversal and minimal storage overhead.
template <typename Options, typename UpdateFn, bool FirstIsEntity, typename ComponentsList, typename PoolsList>
class system_ranged final : public system<Options, UpdateFn, FirstIsEntity, ComponentsList, PoolsList> {
	using base = system<Options, UpdateFn, FirstIsEntity, ComponentsList, PoolsList>;

	// Determine the execution policy from the options (or lack thereof)
	using execution_policy = std::conditional_t<ecs::detail::has_option<opts::not_parallel, Options>(), std::execution::sequenced_policy,
												std::execution::parallel_policy>;

public:
	system_ranged(UpdateFn func, component_pools<PoolsList>&& in_pools)
		: base{func, std::forward<component_pools<PoolsList>>(in_pools)} {
		this->process_changes(true);
	}

private:
	void do_run() override {
		// Call the system for all the components that match the system signature
		for (auto& argument : lambda_arguments) {
			argument(this->update_func);
		}
	}

	// Convert a set of entities into arguments that can be passed to the system
	void do_build() override {
		// Clear current arguments
		lambda_arguments.clear();

		for_all_types<ComponentsList>([&]<typename... Type>() {
			find_entity_pool_intersections_cb<ComponentsList>(this->pools, [this](entity_range found_range) {
				lambda_arguments.push_back(make_argument<Type...>(found_range, get_component<Type>(found_range.first(), this->pools)...));
			});
		});
	}

	template <typename... Ts>
	static auto make_argument(entity_range const range, auto... args) {
		return [=](auto update_func) noexcept {
			auto constexpr e_p = execution_policy{}; // cannot pass 'execution_policy{}' directly to for_each in gcc
			std::for_each(e_p, range.begin(), range.end(), [=](entity_id const ent) mutable noexcept {
				auto const offset = ent - range.first();

				if constexpr (FirstIsEntity) {
					update_func(ent, extract_arg_lambda<Ts>(args, offset, 0)...);
				} else {
					update_func(/**/ extract_arg_lambda<Ts>(args, offset, 0)...);
				}
			});
		};
	}

private:
	/// XXX
	using base_argument = decltype(for_all_types<ComponentsList>([]<typename... Types>() {
			return make_argument<Types...>(entity_range{0,0}, component_argument<Types>{}...);
		}));
	
	std::vector<std::remove_const_t<base_argument>> lambda_arguments;

};
} // namespace ecs::detail

#endif // !ECS_DETAIL_SYSTEM_RANGED_H
#ifndef ECS_DETAIL_SYSTEM_GLOBAL_H
#define ECS_DETAIL_SYSTEM_GLOBAL_H


namespace ecs::detail {
// The implementation of a system specialized on its components
template <typename Options, typename UpdateFn, bool FirstIsEntity, typename ComponentsList, typename PoolsList>
class system_global final : public system<Options, UpdateFn, FirstIsEntity, ComponentsList, PoolsList> {
	using base = system<Options, UpdateFn, FirstIsEntity, ComponentsList, PoolsList>;

public:
	system_global(UpdateFn func, component_pools<PoolsList>&& in_pools)
		: base{func, std::forward<component_pools<PoolsList>>(in_pools)} {
		this->process_changes(true);
	  }

private:
	void do_run() override {
		for_all_types<PoolsList>([&]<typename... Types>() {
			this->update_func(this->pools.template get<Types>().get_shared_component()...);
		});
	}

	void do_build() override {
	}
};
} // namespace ecs::detail

#endif // !ECS_DETAIL_SYSTEM_GLOBAL_H
#ifndef ECS_DETAIL_SCHEDULER_H
#define ECS_DETAIL_SCHEDULER_H



namespace ecs::detail {

// Describes a node in the scheduler execution graph
struct scheduler_node final {
	// Construct a node from a system.
	// The system can not be null
	scheduler_node(detail::system_base* _sys) : sys(_sys), dependents{}, unfinished_dependencies{0}, dependencies{0} {
		Pre(sys != nullptr, "system can not be null");
	}

	scheduler_node(scheduler_node const& other) {
		sys = other.sys;
		dependents = other.dependents;
		dependencies = other.dependencies;
		unfinished_dependencies = other.unfinished_dependencies.load();
	}

	scheduler_node& operator=(scheduler_node const& other) {
		sys = other.sys;
		dependents = other.dependents;
		dependencies = other.dependencies;
		unfinished_dependencies = other.unfinished_dependencies.load();
		return *this;
	}

	detail::system_base* get_system() const noexcept {
		return sys;
	}

	// Add a dependant to this system. This system has to run to
	// completion before the dependents can run.
	void add_dependent(size_t node_index) {
		dependents.push_back(node_index);
	}

	// Increase the dependency counter of this system. These dependencies has to
	// run to completion before this system can run.
	void increase_dependency_count() {
		Pre(dependencies != std::numeric_limits<int16_t>::max(), "system has too many dependencies (>32k)");
		dependencies += 1;
	}

	// Resets the unfinished dependencies to the total number of dependencies.
	void reset_unfinished_dependencies() {
		unfinished_dependencies = dependencies;
	}

	// Called from systems we depend on when they have run to completion.
	void dependency_done() {
		unfinished_dependencies.fetch_sub(1, std::memory_order_release);
	}

	void run(std::vector<struct scheduler_node>& nodes) {
		// If we are not the last node here, leave
		if (unfinished_dependencies.load(std::memory_order_acquire) != 0)
			return;

		// Run the system
		sys->run();

		// Notify the dependents that we are done
		for (size_t const node : dependents)
			nodes[node].dependency_done();

		// Run the dependents in parallel
		std::for_each(std::execution::par, dependents.begin(), dependents.end(), [&nodes](size_t node) {
			nodes[node].run(nodes);
		});
	}

private:
	// The system to execute
	detail::system_base* sys{};

	// The systems that depend on this
	std::vector<size_t> dependents{};

	// The number of systems this depends on
	std::atomic<int> unfinished_dependencies = 0;
	int dependencies = 0;
};

// Schedules systems for concurrent execution based on their components.
class scheduler final {
	std::vector<scheduler_node> all_nodes;
	std::vector<std::size_t> entry_nodes{};

public:
	scheduler() {}

	void insert(detail::system_base* sys) {
		// Create a new node with the system
		size_t const node_index = all_nodes.size();
		scheduler_node& node = all_nodes.emplace_back(sys);

		// Find a dependant system for each component
		bool inserted = false;
		auto const end = all_nodes.rend();
		for (auto const hash : sys->get_type_hashes()) {
			auto it = std::next(all_nodes.rbegin()); // 'next' to skip the newly added system
			while (it != end) {
				scheduler_node& dep_node = *it;
				// If the other system doesn't touch the same component,
				// then there can be no dependency
				if (dep_node.get_system()->has_component(hash)) {
					if (dep_node.get_system()->writes_to_component(hash) || sys->writes_to_component(hash)) {
						// The system writes to the component,
						// so there is a strong dependency here.
						inserted = true;
						dep_node.add_dependent(node_index);
						node.increase_dependency_count();
						break;
					} else { // 'other' reads component
							 // These systems have a weak read/read dependency
							 // and can be scheduled concurrently
					}
				}

				++it;
			}
		}

		// The system has no dependencies, so make it an entry node
		if (!inserted) {
			entry_nodes.push_back(node_index);
		}
	}

	// Clears all the schedulers data
	void clear() {
		all_nodes.clear();
		entry_nodes.clear();
	}

	void run() {
		// Reset the execution data
		for (auto& node : all_nodes)
			node.reset_unfinished_dependencies();

		// Run the nodes concurrently
		std::for_each(std::execution::par, entry_nodes.begin(), entry_nodes.end(), [this](size_t node_id) {
			all_nodes[node_id].run(all_nodes);
		});
	}
};

} // namespace ecs::detail

#endif // !ECS_DETAIL_SCHEDULER_H
#ifndef ECS_DETAIL_CONTEXT_H
#define ECS_DETAIL_CONTEXT_H




namespace ecs::detail {
// The central class of the ecs implementation. Maintains the state of the system.
class context final {
	// The values that make up the ecs core.
	std::vector<std::unique_ptr<system_base>> systems;
	std::vector<std::unique_ptr<component_pool_base>> component_pools;
	std::vector<type_hash> pool_type_hash;
	scheduler sched;

	mutable std::shared_mutex system_mutex;
	mutable std::recursive_mutex component_pool_mutex;

	bool commit_in_progress = false;
	bool run_in_progress = false;

	using cache_type = tls::cache<type_hash, component_pool_base*, get_type_hash<void>()>;
	tls::split<cache_type> type_caches;

public:
	~context() {
		reset();
	}

	// Commits the changes to the entities.
	void commit_changes() {
		Pre(!commit_in_progress, "a commit is already in progress");
		Pre(!run_in_progress, "can not commit changes while systems are running");

		// Prevent other threads from
		//  adding components
		//  registering new component types
		//  adding new systems
		std::shared_lock system_lock(system_mutex, std::defer_lock);
		std::unique_lock component_pool_lock(component_pool_mutex, std::defer_lock);
		std::lock(system_lock, component_pool_lock); // lock both without deadlock

		commit_in_progress = true;

		static constexpr auto process_changes = [](auto const& inst) {
			inst->process_changes();
		};

		// Let the component pools handle pending add/remove requests for components
		std::for_each(std::execution::par, component_pools.begin(), component_pools.end(), process_changes);

		// Let the systems respond to any changes in the component pools
		std::for_each(std::execution::par, systems.begin(), systems.end(), process_changes);

		// Reset any dirty flags on pools
		for (auto const& pool : component_pools) {
			pool->clear_flags();
		}

		commit_in_progress = false;
	}

	// Calls the 'update' function on all the systems in the order they were added.
	void run_systems() {
		Pre(!commit_in_progress, "can not run systems while changes are being committed");
		Pre(!run_in_progress, "systems are already running");

		// Prevent other threads from adding new systems during the run
		std::shared_lock system_lock(system_mutex);
		run_in_progress = true;

		// Run all the systems
		sched.run();

		run_in_progress = false;
	}

	// Returns true if a pool for the type exists
	template <typename T>
	bool has_component_pool() const {
		// Prevent other threads from registering new component types
		std::shared_lock component_pool_lock(component_pool_mutex);

		static constexpr auto hash = get_type_hash<T>();
		return std::ranges::find(pool_type_hash, hash) != pool_type_hash.end();
	}

	// Resets the runtime state. Removes all systems, empties component pools
	void reset() {
		Pre(!commit_in_progress, "a commit is already in progress");
		Pre(!run_in_progress, "can not commit changes while systems are running");

		std::unique_lock system_lock(system_mutex, std::defer_lock);
		std::unique_lock component_pool_lock(component_pool_mutex, std::defer_lock);
		std::lock(system_lock, component_pool_lock); // lock both without deadlock

		systems.clear();
		sched.clear();
		pool_type_hash.clear();
		component_pools.clear();
		type_caches.clear();
	}

	// Returns a reference to a components pool.
	// If a pool doesn't exist, one will be created.
	template <typename T>
	auto& get_component_pool() {
		// This assert is here to prevent calls like get_component_pool<T> and get_component_pool<T&>,
		// which will produce the exact same code. It should help a bit with compilation times
		// and prevent the compiler from generating duplicated code.
		static_assert(std::is_same_v<T, std::remove_pointer_t<std::remove_cvref_t<T>>>,
					  "This function only takes naked types, like 'int', and not 'int const&' or 'int*'");

		// Don't call this when a commit is in progress
		Pre(!commit_in_progress, "can not get a component pool while a commit is in progress");

		auto& cache = type_caches.local();

		static constexpr auto hash = get_type_hash<T>();
		auto pool = cache.get_or(hash, [this](type_hash _hash) {
			// A new pool might be created, so take a unique lock
			std::unique_lock component_pool_lock(component_pool_mutex);

			// Look in the pool for the type
			auto const it = std::ranges::find(pool_type_hash, _hash);
			if (it == pool_type_hash.end()) {
				// The pool wasn't found so create it.
				return create_component_pool<T>();
			} else {
				return component_pools[std::distance(pool_type_hash.begin(), it)].get();
			}
		});

		return *static_cast<component_pool<T>*>(pool);
	}

	// Regular function
	template <typename Options, typename UpdateFn, typename SortFn, typename R, typename FirstArg, typename... Args>
	decltype(auto) create_system(UpdateFn update_func, SortFn sort_func, R(FirstArg, Args...)) {
		return create_system<Options, UpdateFn, SortFn, FirstArg, Args...>(update_func, sort_func);
	}

	// Const lambda with sort
	template <typename Options, typename UpdateFn, typename SortFn, typename R, typename C, typename FirstArg, typename... Args>
	decltype(auto) create_system(UpdateFn update_func, SortFn sort_func, R (C::*)(FirstArg, Args...) const) {
		return create_system<Options, UpdateFn, SortFn, FirstArg, Args...>(update_func, sort_func);
	}

	// Mutable lambda with sort
	template <typename Options, typename UpdateFn, typename SortFn, typename R, typename C, typename FirstArg, typename... Args>
	decltype(auto) create_system(UpdateFn update_func, SortFn sort_func, R (C::*)(FirstArg, Args...)) {
		return create_system<Options, UpdateFn, SortFn, FirstArg, Args...>(update_func, sort_func);
	}

private:
	template <impl::TypeList ComponentList>
	auto make_pools() {
		using NakedComponentList = transform_type<ComponentList, naked_component_t>;

		return for_all_types<NakedComponentList>([this]<typename... Types>() {
			return detail::component_pools<NakedComponentList>{
				&this->get_component_pool<Types>()...};
		});
	}

	template <typename Options, typename UpdateFn, typename SortFn, typename FirstComponent, typename... Components>
	decltype(auto) create_system(UpdateFn update_func, SortFn sort_func) {
		Pre(!commit_in_progress, "can not create systems while changes are being committed");
		Pre(!run_in_progress, "can not create systems while systems are running");

		// Is the first component an entity_id?
		static constexpr bool first_is_entity = is_entity<FirstComponent>;

		// The type_list of components
		using component_list = std::conditional_t<first_is_entity, type_list<Components...>, type_list<FirstComponent, Components...>>;
	
		// Find potential parent type
		using parent_type = test_option_type_or<is_parent, component_list, void>;

		// Do some checks on the systems
		static bool constexpr has_sort_func = !std::is_same_v<SortFn, std::nullptr_t>;
		static bool constexpr has_parent = !std::is_same_v<void, parent_type>;
		static bool constexpr is_global_sys = for_all_types<component_list>([]<typename... Types>() {
				return (detail::global<Types> && ...);
			});

		static_assert(!(is_global_sys == has_sort_func && is_global_sys), "Global systems can not be sorted");
		static_assert(!(has_sort_func == has_parent && has_parent == true), "Systems can not both be hierarchial and sorted");

		// Helper-lambda to insert system
		auto const insert_system = [this](auto& system) -> decltype(auto) {
			std::unique_lock system_lock(system_mutex);

			[[maybe_unused]] auto sys_ptr = system.get();
			systems.push_back(std::move(system));

			// -vv-  MSVC shenanigans
			[[maybe_unused]] static bool constexpr request_manual_update = has_option<opts::manual_update, Options>();
			if constexpr (!request_manual_update) {
				detail::system_base* ptr_system = systems.back().get();
				sched.insert(ptr_system);
			} else {
				return (*sys_ptr);
			}
		};

		// Create the system instance
		if constexpr (has_parent) {
			using combined_list = detail::merge_type_lists<component_list, parent_type_list_t<parent_type>>;
			using typed_system = system_hierarchy<Options, UpdateFn, first_is_entity, component_list, combined_list, transform_type<combined_list, naked_component_t>>;
			auto sys = std::make_unique<typed_system>(update_func, make_pools<combined_list>());
			return insert_system(sys);
		} else if constexpr (is_global_sys) {
			using typed_system = system_global<Options, UpdateFn, first_is_entity, component_list, transform_type<component_list, naked_component_t>>;
			auto sys = std::make_unique<typed_system>(update_func, make_pools<component_list>());
			return insert_system(sys);
		} else if constexpr (has_sort_func) {
			using typed_system = system_sorted<Options, UpdateFn, SortFn, first_is_entity, component_list, transform_type<component_list, naked_component_t>>;
			auto sys = std::make_unique<typed_system>(update_func, sort_func, make_pools<component_list>());
			return insert_system(sys);
		} else {
			using typed_system = system_ranged<Options, UpdateFn, first_is_entity, component_list, transform_type<component_list, naked_component_t>>;
			auto sys = std::make_unique<typed_system>(update_func, make_pools<component_list>());
			return insert_system(sys);
		}
	}

	template<typename T, typename V>
	void setup_variant_pool(component_pool<T>& pool, component_pool<V>& variant_pool) {
		if constexpr (std::same_as<T, V>) {
			return;
		} else {
			pool.add_variant(&variant_pool);
			variant_pool.add_variant(&pool);
			if constexpr (has_variant_alias<V> && !std::same_as<T, V>) {
				setup_variant_pool(pool, get_component_pool<variant_t<V>>());
			}
		}
	}

	// Create a component pool for a new type
	template <typename T>
	component_pool_base* create_component_pool() {
		static_assert(not_recursive_variant<T>(), "variant chain/tree is recursive");

		// Create a new pool
		auto pool = std::make_unique<component_pool<T>>();

		// Set up variants
		if constexpr (ecs::detail::has_variant_alias<T>) {
			setup_variant_pool(*pool.get(), get_component_pool<variant_t<T>>());
		}

		// Store the pool and its type hash
		static constexpr auto hash = get_type_hash<T>();
		pool_type_hash.push_back(hash);
		component_pools.push_back(std::move(pool));

		return component_pools.back().get();
	}
};
} // namespace ecs::detail

#endif // !ECS_DETAIL_CONTEXT_H
#ifndef ECS_RUNTIME_H
#define ECS_RUNTIME_H



namespace ecs {
	ECS_EXPORT class runtime {
	public:
		// Add several components to a range of entities. Will not be added until 'commit_changes()' is called.
		// Pre: entity does not already have the component, or have it in queue to be added
		template <typename... T>
		constexpr void add_component(entity_range const range, T&&... vals) {
			static_assert(detail::is_unique_type_args<T...>(), "the same component type was specified more than once");
			static_assert((!detail::global<T> && ...), "can not add global components to entities");
			static_assert((!std::is_pointer_v<std::remove_cvref_t<T>> && ...), "can not add pointers to entities; wrap them in a struct");
			static_assert((!detail::is_variant_of_pack<T...>()), "Can not add more than one component from the same variant");

			auto const adder = [this, range]<typename Type>(Type&& val) {
				// Add it to the component pool
				if constexpr (detail::is_parent<Type>::value) {
					detail::component_pool<detail::parent_id>& pool = ctx.get_component_pool<detail::parent_id>();
					Pre(!pool.has_entity(range), "one- or more entities in the range already has this type");
					pool.add(range, detail::parent_id{val.id()});
				} else if constexpr (std::is_reference_v<Type>) {
					using DerefT = std::remove_cvref_t<Type>;
					static_assert(std::copyable<DerefT>, "Type must be copyable");

					detail::component_pool<DerefT>& pool = ctx.get_component_pool<DerefT>();
					Pre(!pool.has_entity(range), "one- or more entities in the range already has this type");
					pool.add(range, val);
				} else {
					static_assert(std::copyable<Type>, "Type must be copyable");

					detail::component_pool<Type>& pool = ctx.get_component_pool<Type>();
					Pre(!pool.has_entity(range), "one- or more entities in the range already has this type");
					pool.add(range, std::forward<Type>(val));
				}
			};

			(adder(std::forward<T>(vals)), ...);
		}

		// Adds a span of components to a range of entities. Will not be added until 'commit_changes()' is called.
		// Pre: entity does not already have the component, or have it in queue to be added
		// Pre: range and span must be same size
		void add_component_span(entity_range const range, std::ranges::contiguous_range auto const& vals) {
			static_assert(std::ranges::sized_range<decltype(vals)>, "Size of span is needed.");
			using T = std::remove_cvref_t<decltype(vals[0])>;
			static_assert(!detail::global<T>, "can not add global components to entities");
			static_assert(!std::is_pointer_v<std::remove_cvref_t<T>>, "can not add pointers to entities; wrap them in a struct");
			// static_assert(!detail::is_parent<std::remove_cvref_t<T>>::value, "adding spans of parents is not (yet?) supported"); //
			// should work
			static_assert(std::copyable<T>, "Type must be copyable");

			Pre(range.ucount() == std::size(vals), "range and span must be same size");

			// Add it to the component pool
			if constexpr (detail::is_parent<T>::value) {
				detail::component_pool<detail::parent_id>& pool = ctx.get_component_pool<detail::parent_id>();
				PreAudit(!pool.has_entity(range), "one- or more entities in the range already has this type");
				pool.add_span(range, vals | std::views::transform([](T v) {
										 return detail::parent_id{v.id()};
									 }));
			} else {
				detail::component_pool<T>& pool = ctx.get_component_pool<T>();
				PreAudit(!pool.has_entity(range), "one- or more entities in the range already has this type");
				pool.add_span(range, std::span{vals});
			}
		}

		template <typename Fn>
		void add_component_generator(entity_range const range, Fn&& gen) {
			// Return type of 'func'
			using ComponentType = decltype(std::declval<Fn>()(entity_id{0}));
			static_assert(!std::is_same_v<ComponentType, void>, "Initializer functions must return a component");

			if constexpr (detail::is_parent<std::remove_cvref_t<ComponentType>>::value) {
				auto const converter = [gen = std::forward<Fn>(gen)](entity_id id) {
					return detail::parent_id{gen(id).id()};
				};

				auto& pool = ctx.get_component_pool<detail::parent_id>();
				PreAudit(!pool.has_entity(range), "one- or more entities in the range already has this type");
				pool.add_generator(range, converter);
			} else {
				auto& pool = ctx.get_component_pool<ComponentType>();
				PreAudit(!pool.has_entity(range), "one- or more entities in the range already has this type");
				pool.add_generator(range, std::forward<Fn>(gen));
			}
		}

		// Add several components to an entity. Will not be added until 'commit_changes()' is called.
		// Pre: entity does not already have the component, or have it in queue to be added
		template <typename... T>
		void add_component(entity_id const id, T&&... vals) {
			add_component(entity_range{id, id}, std::forward<T>(vals)...);
		}

		// Removes a component from a range of entities.
		// Will not be removed until 'commit_changes()' is called.
		// Pre: entity has the component
		template <detail::persistent T>
		void remove_component(entity_range const range, T const& = T{}) {
			static_assert(!detail::global<T>, "can not remove or add global components to entities");

			// Remove the entities from the components pool
			detail::component_pool<T>& pool = ctx.get_component_pool<T>();
			Pre(pool.has_entity(range), "component pool does not contain some- or all of the entities in the range");
			pool.remove(range);
		}

		// Removes a component from an entity. Will not be removed until 'commit_changes()' is called.
		// Pre: entity has the component
		template <typename T>
		void remove_component(entity_id const id, T const& = T{}) {
			remove_component<T>({id, id});
		}

		// Returns a global component.
		template <detail::global T>
		T& get_global_component() {
			return ctx.get_component_pool<T>().get_shared_component();
		}

		// Returns the component from an entity, or nullptr if the entity is not found
		// NOTE: Pointers to components are only guaranteed to be valid
		//       until the next call to 'runtime::commit_changes' or 'runtime::update',
		//       after which the component might be reallocated.
		template <detail::local T>
		T* get_component(entity_id const id) {
			// Get the component pool
			detail::component_pool<T>& pool = ctx.get_component_pool<T>();
			return pool.find_component_data(id);
		}

		// Returns the components from an entity range, or an empty span if the entities are not found
		// or does not contain the component.
		// NOTE: Pointers to components are only guaranteed to be valid
		//       until the next call to 'runtime::commit_changes' or 'runtime::update',
		//       after which the component might be reallocated.
		template <detail::local T>
		std::span<T> get_components(entity_range const range) {
			if (!has_component<T>(range))
				return {};

			// Get the component pool
			detail::component_pool<T>& pool = ctx.get_component_pool<T>();
			return {pool.find_component_data(range.first()), range.ucount()};
		}

		// Returns the number of active components for a specific type of components
		template <typename T>
		ptrdiff_t get_component_count() {
			// Get the component pool
			detail::component_pool<T> const& pool = ctx.get_component_pool<T>();
			return pool.num_components();
		}

		// Returns the number of entities that has the component.
		template <typename T>
		ptrdiff_t get_entity_count() {
			// Get the component pool
			detail::component_pool<T> const& pool = ctx.get_component_pool<T>();
			return pool.num_entities();
		}

		// Return true if an entity contains the component
		template <typename T>
		bool has_component(entity_id const id) {
			detail::component_pool<T> const& pool = ctx.get_component_pool<T>();
			return pool.has_entity(id);
		}

		// Returns true if all entities in a range has the component.
		template <typename T>
		bool has_component(entity_range const range) {
			detail::component_pool<T>& pool = ctx.get_component_pool<T>();
			return pool.has_entity(range);
		}

		// Commits the changes to the entities.
		inline void commit_changes() {
			ctx.commit_changes();
		}

		// Calls the 'update' function on all the systems in the order they were added.
		inline void run_systems() {
			ctx.run_systems();
		}

		// Commits all changes and calls the 'update' function on all the systems in the order they were
		// added. Same as calling commit_changes() and run_systems().
		inline void update() {
			commit_changes();
			run_systems();
		}

		// Make a new system
		template <typename... Options, typename SystemFunc, typename SortFn = std::nullptr_t>
		decltype(auto) make_system(SystemFunc sys_func, SortFn sort_func = nullptr) {
			using opts = detail::type_list<Options...>;

			// verify the input
			constexpr static bool dummy_for_clang_13 = detail::make_system_parameter_verifier<opts, SystemFunc, SortFn>();
			(void)dummy_for_clang_13;

			if constexpr (ecs::detail::type_is_function<SystemFunc>) {
				// Build from regular function
				return ctx.create_system<opts, SystemFunc, SortFn>(sys_func, sort_func, sys_func);
			} else if constexpr (ecs::detail::type_is_lambda<SystemFunc>) {
				// Build from lambda
				return ctx.create_system<opts, SystemFunc, SortFn>(sys_func, sort_func, &SystemFunc::operator());
			} else {
				(void)sys_func;
				(void)sort_func;
				struct _invalid_system_type {
				} invalid_system_type;
				return invalid_system_type;
			}
		}

		// Set the memory resource to use to store a specific type of component
		/*template <typename Component>
		void set_memory_resource(std::pmr::memory_resource* resource) {
			auto& pool = ctx.get_component_pool<Component>();
			pool.set_memory_resource(resource);
		}

		// Returns the memory resource used to store a specific type of component
		template <typename Component>
		std::pmr::memory_resource* get_memory_resource() {
			auto& pool = ctx.get_component_pool<Component>();
			return pool.get_memory_resource();
		}

		// Resets the memory resource to the default
		template <typename Component>
		void reset_memory_resource() {
			auto& pool = ctx.get_component_pool<Component>();
			pool.set_memory_resource(std::pmr::get_default_resource());
		}*/

	private:
		detail::context ctx;
	};

} // namespace ecs

#endif // !ECS_RUNTIME_H
