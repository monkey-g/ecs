#ifndef ECS_DETAIL_TYPE_LIST_H
#define ECS_DETAIL_TYPE_LIST_H

#include <concepts>
#include <type_traits>
#include <utility>

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
		constexpr decltype(auto) with_all_types(F&& f, type_list<Types...>*) {
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
		constexpr type_list<Types...>* skip_first_type(type_list<First, Types...>*) ECS_NULLBODY;

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
		constexpr type_list<Types..., T>* add_type(type_list<Types...>*) ECS_NULLBODY;

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

		// Reduce a collection of type_lists
		template <template <typename L1, typename L2> typename Op>
		struct reducer {
			template <typename Dest, typename First, typename... Lists>
			consteval static auto helper() {
				if constexpr (sizeof...(Lists) == 0)
					return null_tlist<Op<Dest, First>>();
				else
					return reducer::helper<Op<Dest, First>, Lists...>();
			}
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

	// Applies the functor F once with all types in the type list.
	// Takes lambdas of the form '[]<typename ...T>() {}'
	template <impl::TypeList TL, typename F>
	constexpr decltype(auto) with_all_types(F&& f) {
		return impl::with_all_types(f, static_cast<TL*>(nullptr));
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

	// Reduce a collection of type_lists
	template <template <typename L1, typename L2> typename Op, impl::TypeList... Lists>
	using reduce_lists = std::remove_pointer_t<
		decltype(
			impl::reducer<Op>::template helper<type_list<>, Lists...>()
		)
	>;

} // namespace ecs::detail
#endif // !ECS_DETAIL_TYPE_LIST_H
