// Compile-time contract for the public headers.
//
// TWoTBootstrapResult and FWoTBootstrapStatus exist to make one class of bug
// impossible: reading a value that was never produced, or silently copying a
// payload that must be unique (an open native handle, a durable record, a signed
// artifact). Most of that contract is enforced by the type system, so most of
// this file is static_assert — the assertions below fail the build rather than a
// test run, which is the point. The runtime spec covers what only construction
// and destruction can show: that a move-only payload survives the result type
// without being copied, and that nothing leaks on either path.
#include <type_traits>

#include "Misc/AutomationTest.h"
#include "Templates/UnrealTemplate.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Error.h"

namespace WoTBootstrapCoreHeaderContract
{
/** A payload that can be moved but never copied, and that counts its instances. */
struct FMoveOnlySentinel
{
	static int32 LiveCount;
	static int32 MoveCount;

	int32 Value = 0;

	explicit FMoveOnlySentinel(int32 InValue) : Value(InValue) { ++LiveCount; }
	~FMoveOnlySentinel() { --LiveCount; }

	FMoveOnlySentinel(const FMoveOnlySentinel&) = delete;
	FMoveOnlySentinel& operator=(const FMoveOnlySentinel&) = delete;

	FMoveOnlySentinel(FMoveOnlySentinel&& Other) : Value(Other.Value)
	{
		Other.Value = 0;
		++LiveCount;
		++MoveCount;
	}

	FMoveOnlySentinel& operator=(FMoveOnlySentinel&& Other)
	{
		Value = Other.Value;
		Other.Value = 0;
		++MoveCount;
		return *this;
	}
};

int32 FMoveOnlySentinel::LiveCount = 0;
int32 FMoveOnlySentinel::MoveCount = 0;

using FSentinelResult = TWoTBootstrapResult<FMoveOnlySentinel>;
using FIntegerResult = TWoTBootstrapResult<int32>;

// A result is never copied — not even one holding a trivially copyable value.
static_assert(!std::is_copy_constructible_v<FIntegerResult>,
	"a result must not be copy constructible");
static_assert(!std::is_copy_assignable_v<FIntegerResult>,
	"a result must not be copy assignable");
static_assert(std::is_move_constructible_v<FIntegerResult>,
	"a result must be move constructible");
static_assert(std::is_move_assignable_v<FIntegerResult>,
	"a result must be move assignable");

static_assert(!std::is_copy_constructible_v<FSentinelResult>,
	"a move-only payload must not become copyable through the result");
static_assert(std::is_move_constructible_v<FSentinelResult>,
	"a move-only payload must remain movable through the result");

// The same rules for the no-value form.
static_assert(!std::is_copy_constructible_v<FWoTBootstrapStatus>,
	"a status must not be copy constructible");
static_assert(!std::is_copy_assignable_v<FWoTBootstrapStatus>,
	"a status must not be copy assignable");
static_assert(std::is_move_constructible_v<FWoTBootstrapStatus>,
	"a status must be move constructible");

// Accessors are ref-qualified: an lvalue yields a reference, an rvalue yields an
// rvalue reference, and neither silently produces a copy.
static_assert(std::is_same_v<decltype(DeclVal<FSentinelResult&>().GetValue()),
	FMoveOnlySentinel&>, "lvalue GetValue must return a mutable reference");
static_assert(std::is_same_v<decltype(DeclVal<const FSentinelResult&>().GetValue()),
	const FMoveOnlySentinel&>, "const lvalue GetValue must return a const reference");
static_assert(std::is_same_v<decltype(DeclVal<FSentinelResult&&>().GetValue()),
	FMoveOnlySentinel&&>, "rvalue GetValue must return an rvalue reference");
static_assert(std::is_same_v<decltype(DeclVal<FSentinelResult&>().GetError()),
	const FWoTBootstrapError&>, "lvalue GetError must return a const reference");
static_assert(std::is_same_v<decltype(DeclVal<FSentinelResult&&>().GetError()),
	FWoTBootstrapError&&>, "rvalue GetError must return an rvalue reference");

// A canonical JSON value owns its children, so it is move-only for the same reason.
static_assert(!std::is_copy_constructible_v<FWoTCanonicalJsonValue>,
	"a canonical JSON value must not be copy constructible");
static_assert(std::is_move_constructible_v<FWoTCanonicalJsonValue>,
	"a canonical JSON value must be move constructible");

// The error code is a fixed-width value on the wire.
static_assert(sizeof(EWoTBootstrapError) == sizeof(uint16),
	"the error code's wire width is part of the protocol");

// The protocol constants are compile-time, so no runtime configuration can move
// them. Their values are pinned by the protocol contract.
static_assert(WoT::Bootstrap::GateProtocol == 1, "gate protocol is pinned");
static_assert(WoT::Bootstrap::CommitServiceProtocol == 1, "commit protocol is pinned");
static_assert(WoT::Bootstrap::UpdaterProtocol == 1, "updater protocol is pinned");
static_assert(WoT::Bootstrap::CacheContractVersion == 3, "cache contract is v3");
static_assert(WoT::Bootstrap::ReleaseManifestVersion == 2, "release manifest is v2");
static_assert(WoT::Bootstrap::InstallerSize == 1029509080, "installer size is pinned");
}  // namespace WoTBootstrapCoreHeaderContract

#if WITH_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapPublicHeaderSpec,
	"WoT.BootstrapCore.PublicHeaders.ResultContract",
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FWoTBootstrapPublicHeaderSpec::RunTest(const FString&)
{
	using namespace WoTBootstrapCoreHeaderContract;

	FMoveOnlySentinel::LiveCount = 0;
	FMoveOnlySentinel::MoveCount = 0;

	{
		// Success constructs the payload in place: no temporary to move from.
		FSentinelResult Result = FSentinelResult::Success(42);
		TestFalse(TEXT("a success result is not an error"), Result.IsError());
		TestEqual(TEXT("in-place construction made exactly one instance"),
			FMoveOnlySentinel::LiveCount, 1);
		TestEqual(TEXT("in-place construction moved nothing"),
			FMoveOnlySentinel::MoveCount, 0);
		TestEqual(TEXT("lvalue access reads the payload"),
			Result.GetValue().Value, 42);

		// Ownership transfers only through an explicit MoveTemp.
		FMoveOnlySentinel Taken = MoveTemp(Result).GetValue();
		TestEqual(TEXT("extraction moved once"), FMoveOnlySentinel::MoveCount, 1);
		TestEqual(TEXT("extraction carried the value"), Taken.Value, 42);
	}
	TestEqual(TEXT("nothing leaks on the success path"),
		FMoveOnlySentinel::LiveCount, 0);

	{
		FSentinelResult Result = FSentinelResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Crypto, TEXT("bad signature"), TEXT("manifest.sig")));
		TestTrue(TEXT("an error result reports an error"), Result.IsError());
		TestEqual(TEXT("no payload exists on the error path"),
			FMoveOnlySentinel::LiveCount, 0);
		TestEqual(TEXT("the error code survives"),
			Result.GetError().Code, EWoTBootstrapError::Crypto);
		TestEqual(TEXT("the error renders code, message, and path"),
			Result.GetError().ToString(),
			FString(TEXT("Crypto: bad signature (manifest.sig)")));
	}

	{
		FWoTBootstrapStatus Success = FWoTBootstrapStatus::Success();
		TestFalse(TEXT("a success status reports no error"), Success.IsError());

		FWoTBootstrapStatus Failure = FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Durability, TEXT("fsync refused")));
		TestTrue(TEXT("a failed status reports an error"), Failure.IsError());
		TestEqual(TEXT("a pathless error renders code and message only"),
			Failure.GetError().ToString(),
			FString(TEXT("Durability: fsync refused")));

		FWoTBootstrapError Taken = MoveTemp(Failure).GetError();
		TestEqual(TEXT("the error moves out intact"),
			Taken.Code, EWoTBootstrapError::Durability);
	}

	// Every code has a stable, unique spelling: tooling compares these strings.
	{
		const EWoTBootstrapError Codes[] = {
			EWoTBootstrapError::InvalidUtf8, EWoTBootstrapError::DuplicateField,
			EWoTBootstrapError::UnknownField, EWoTBootstrapError::InvalidNumber,
			EWoTBootstrapError::NonCanonicalJson, EWoTBootstrapError::SizeLimit,
			EWoTBootstrapError::UnsupportedProtocol, EWoTBootstrapError::Io,
			EWoTBootstrapError::Crypto, EWoTBootstrapError::Rollback,
			EWoTBootstrapError::UnsafePath, EWoTBootstrapError::Durability,
			EWoTBootstrapError::ServiceUnavailable,
			EWoTBootstrapError::CorruptInstallation,
		};

		TSet<FString> Names;
		for (const EWoTBootstrapError Code : Codes)
		{
			const FString Name = LexToString(Code);
			TestFalse(FString::Printf(TEXT("%s has a name"), *Name), Name.IsEmpty());
			TestNotEqual(TEXT("no code falls through to the unknown name"),
				Name, FString(TEXT("UnknownErrorCode")));
			Names.Add(Name);
		}
		TestEqual(TEXT("every error name is unique"),
			Names.Num(), static_cast<int32>(UE_ARRAY_COUNT(Codes)));
	}

	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
