// The one error vocabulary and the one success-or-error return shape for every
// bootstrap, update, and commit-service operation.
//
// Public APIs in this module never return a bare bool, never return a sentinel
// value, and never throw: they return TWoTBootstrapResult<T> or
// FWoTBootstrapStatus so that a caller cannot read a value that was never
// produced. Both types are move-only by design — a signed artifact, an open
// native handle, or a durable record must not be silently duplicated.
#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "Misc/AssertionMacros.h"
#include "Misc/Optional.h"
#include "Misc/TVariant.h"

WOTBOOTSTRAPCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogWoTBootstrap, Log, All);

/**
 * The closed set of failure reasons. These names are stable: they cross the
 * round-trip wire protocol, appear in release evidence, and are compared by
 * tooling, so entries are never renamed, reordered, or removed.
 */
enum class EWoTBootstrapError : uint16
{
	InvalidUtf8,
	DuplicateField,
	UnknownField,
	InvalidNumber,
	NonCanonicalJson,
	SizeLimit,
	UnsupportedProtocol,
	Io,
	Crypto,
	Rollback,
	UnsafePath,
	Durability,
	ServiceUnavailable,
	CorruptInstallation,
};

/** The stable ASCII spelling of a code, as it appears on the wire and in evidence. */
WOTBOOTSTRAPCORE_API const TCHAR* LexToString(EWoTBootstrapError Code);

struct FWoTBootstrapError
{
	EWoTBootstrapError Code = EWoTBootstrapError::Io;

	/** Human-readable detail. Never a substitute for Code in a decision. */
	FString Message;

	/** Where the failure was found: a field path, a byte offset, or a file path. */
	FString Path;

	/** Platform error code for local classification only; never serialized. */
	uint32 NativeCode = 0;

	/** "<Code>: <Message>[ (<Path>)]" — the exact form the round-trip driver matches. */
	WOTBOOTSTRAPCORE_API FString ToString() const;
};

inline FWoTBootstrapError MakeWoTBootstrapError(
	EWoTBootstrapError Code, FString Message, FString Path = FString())
{
	return FWoTBootstrapError{Code, MoveTemp(Message), MoveTemp(Path), 0};
}

/**
 * A move-only value-or-error result. Concrete type, never an alias.
 *
 * Success constructs T in place by perfect forwarding, so a move-only payload
 * needs no intermediate copy. Accessors are ref-qualified: lvalue access never
 * copies, and taking ownership requires MoveTemp(Result).GetValue(), which makes
 * an accidental copy of a move-only payload a compile error rather than a
 * runtime surprise. Reading the wrong alternative is a check(), not UB.
 */
template<class T>
class TWoTBootstrapResult final
{
public:
	template<class... Args>
	static TWoTBootstrapResult Success(Args&&... ValueArgs)
	{
		return TWoTBootstrapResult(FSuccessTag{}, Forward<Args>(ValueArgs)...);
	}

	static TWoTBootstrapResult Error(FWoTBootstrapError&& ErrorValue)
	{
		return TWoTBootstrapResult(FErrorTag{}, MoveTemp(ErrorValue));
	}

	bool IsError() const { return Storage.template IsType<FWoTBootstrapError>(); }

	const FWoTBootstrapError& GetError() const &
	{
		check(IsError());
		return Storage.template Get<FWoTBootstrapError>();
	}

	FWoTBootstrapError&& GetError() &&
	{
		check(IsError());
		return MoveTemp(Storage.template Get<FWoTBootstrapError>());
	}

	T& GetValue() & { check(!IsError()); return Storage.template Get<T>(); }
	const T& GetValue() const & { check(!IsError()); return Storage.template Get<T>(); }
	T&& GetValue() && { check(!IsError()); return MoveTemp(Storage.template Get<T>()); }

	/** Deleted: a const rvalue result cannot surrender ownership of its value. */
	const T&& GetValue() const && = delete;

	TWoTBootstrapResult(const TWoTBootstrapResult&) = delete;
	TWoTBootstrapResult& operator=(const TWoTBootstrapResult&) = delete;
	TWoTBootstrapResult(TWoTBootstrapResult&&) = default;
	TWoTBootstrapResult& operator=(TWoTBootstrapResult&&) = default;

private:
	struct FSuccessTag {};
	struct FErrorTag {};

	template<class... Args>
	explicit TWoTBootstrapResult(FSuccessTag, Args&&... ValueArgs)
		: Storage(TInPlaceType<T>(), Forward<Args>(ValueArgs)...)
	{
	}

	explicit TWoTBootstrapResult(FErrorTag, FWoTBootstrapError&& InError)
		: Storage(TInPlaceType<FWoTBootstrapError>(), MoveTemp(InError))
	{
	}

	TVariant<T, FWoTBootstrapError> Storage;
};

/**
 * The success-or-error type for operations that produce no value.
 *
 * Deliberately not TWoTBootstrapResult<void> or a unit alternative: an operation
 * with nothing to return should not force callers through GetValue().
 */
class FWoTBootstrapStatus final
{
public:
	static FWoTBootstrapStatus Success() { return FWoTBootstrapStatus(); }

	static FWoTBootstrapStatus Error(FWoTBootstrapError&& ErrorValue)
	{
		return FWoTBootstrapStatus(MoveTemp(ErrorValue));
	}

	bool IsError() const { return ErrorValue.IsSet(); }

	const FWoTBootstrapError& GetError() const &
	{
		check(IsError());
		return ErrorValue.GetValue();
	}

	FWoTBootstrapError&& GetError() &&
	{
		check(IsError());
		return MoveTemp(ErrorValue.GetValue());
	}

	FWoTBootstrapStatus(const FWoTBootstrapStatus&) = delete;
	FWoTBootstrapStatus& operator=(const FWoTBootstrapStatus&) = delete;
	FWoTBootstrapStatus(FWoTBootstrapStatus&&) = default;
	FWoTBootstrapStatus& operator=(FWoTBootstrapStatus&&) = default;

private:
	FWoTBootstrapStatus() = default;

	explicit FWoTBootstrapStatus(FWoTBootstrapError&& InError)
		: ErrorValue(MoveTemp(InError))
	{
	}

	TOptional<FWoTBootstrapError> ErrorValue;
};
