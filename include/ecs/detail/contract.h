#ifndef ECS_DETAIL_CONTRACT_H
#define ECS_DETAIL_CONTRACT_H

#include <concepts>
#include <iostream>
#include <utility>
#if __has_include(<stacktrace>)
#include <stacktrace>
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
	[[noreturn]] inline void do_assertion_failed(char const* what, char const* how) {
		ecs::detail::contract_violation_interface auto& cvi = ecs::contract_violation_handler<DummyArgs...>;
		cvi.assertion_failed(what, how);
	}

	template <typename... DummyArgs>
		requires(sizeof...(DummyArgs) == 0)
	[[noreturn]] inline void do_precondition_violation(char const* what, char const* how) {
		ecs::detail::contract_violation_interface auto& cvi = ecs::contract_violation_handler<DummyArgs...>;
		cvi.precondition_violation(what, how);
	}

	template <typename... DummyArgs>
		requires(sizeof...(DummyArgs) == 0)
	[[noreturn]] inline void do_postcondition_violation(char const* what, char const* how) {
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
