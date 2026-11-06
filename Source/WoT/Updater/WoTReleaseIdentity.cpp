#include "WoTReleaseIdentity.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace WoTReleaseIdentity
{
	namespace
	{
		constexpr int64 MaximumIdentityBytes = 4 * 1024;
		constexpr TCHAR IdentityFormat[] = TEXT("wot-release-identity-v1");
		constexpr uint32 IdentityVersion = 1;

#if WITH_AUTOMATION_TESTS
		TOptional<FString> PathForTesting;
#endif

		FString IdentityPath()
		{
#if WITH_AUTOMATION_TESTS
			if (PathForTesting.IsSet())
			{
				return PathForTesting.GetValue();
			}
#endif
			return FPaths::ProjectDir()
				/ TEXT("Build/WoTRelease/release-identity.json");
		}
	}

	TWoTBootstrapResult<FIdentity> ParseCanonical(TConstArrayView<uint8> Bytes)
	{
		using FResult = TWoTBootstrapResult<FIdentity>;
		FWoTCanonicalJsonLimits Limits;
		Limits.MaxBytes = static_cast<uint32>(MaximumIdentityBytes);
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
			ParseCanonicalJson(Bytes, Limits);
		if (Parsed.IsError())
		{
			return FResult::Error(MoveTemp(Parsed).GetError());
		}

		const FWoTCanonicalJsonValue& Root = Parsed.GetValue();
		static const TCHAR* const Fields[] = {
			TEXT("format"), TEXT("release_commit"),
			TEXT("release_sequence"), TEXT("version")};
		if (TOptional<FWoTBootstrapError> Error = RequireExactMembers(
				Root, Fields, TEXT("release identity")); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}

		FString Format;
		if (TOptional<FWoTBootstrapError> Error = ReadStringField(
				Root, TEXT("format"), TEXT("release identity"), Format);
			Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (!Format.Equals(IdentityFormat, ESearchCase::CaseSensitive))
		{
			return FResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsupportedProtocol,
				TEXT("release identity format is unsupported")));
		}

		uint32 Version = 0;
		if (TOptional<FWoTBootstrapError> Error = ReadUnsigned32Field(
				Root, TEXT("version"), TEXT("release identity"), Version);
			Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (Version != IdentityVersion)
		{
			return FResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsupportedProtocol,
				TEXT("release identity version is unsupported")));
		}

		FIdentity Identity;
		if (TOptional<FWoTBootstrapError> Error = ReadStringField(
				Root, TEXT("release_commit"), TEXT("release identity"),
				Identity.ReleaseCommit); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ReadUnsignedField(
				Root, TEXT("release_sequence"), TEXT("release identity"),
				Identity.ReleaseSequence); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (Identity.ReleaseCommit.IsEmpty())
		{
			return FResult::Success(FIdentity());
		}
		Identity.bStamped = true;
		return FResult::Success(MoveTemp(Identity));
	}

	FIdentity LoadRunning()
	{
		const FString Path = IdentityPath();
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size < 0 || Size > MaximumIdentityBytes)
		{
			return {};
		}
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path)
			|| Bytes.Num() != Size || Bytes.Num() > MaximumIdentityBytes)
		{
			return {};
		}
		TWoTBootstrapResult<FIdentity> Parsed = ParseCanonical(Bytes);
		return Parsed.IsError() ? FIdentity{} : MoveTemp(Parsed).GetValue();
	}

#if WITH_AUTOMATION_TESTS
	FScopedPathOverrideForTesting::FScopedPathOverrideForTesting(FString InPath)
		: PreviousPath(PathForTesting)
	{
		PathForTesting = MoveTemp(InPath);
	}

	FScopedPathOverrideForTesting::~FScopedPathOverrideForTesting()
	{
		PathForTesting = MoveTemp(PreviousPath);
	}
#endif
}
