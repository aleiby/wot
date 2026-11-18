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
		constexpr TCHAR HumanVersionFormat[] = TEXT("wot-release-version-v1");
		constexpr uint32 HumanVersionDocumentVersion = 1;
		constexpr TCHAR HumanVersionLeaf[] = TEXT("release-version.json");
		constexpr int32 MaximumHumanVersionLength = 32;
		constexpr int32 DevelopmentCommitLength = 9;

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

		/** The bounded stamp bytes at Path; false when absent, oversize or unreadable. */
		bool LoadStampBytes(const FString& Path, TArray<uint8>& OutBytes)
		{
			const int64 Size = IFileManager::Get().FileSize(*Path);
			if (Size < 0 || Size > MaximumIdentityBytes)
			{
				return false;
			}
			return FFileHelper::LoadFileToArray(OutBytes, *Path)
				&& OutBytes.Num() == Size && OutBytes.Num() <= MaximumIdentityBytes;
		}

		FString LoadHumanVersionAt(const FString& Path)
		{
			TArray<uint8> Bytes;
			if (!LoadStampBytes(Path, Bytes))
			{
				return FString();
			}
			TWoTBootstrapResult<FString> Parsed = ParseHumanVersion(Bytes);
			return Parsed.IsError() ? FString() : MoveTemp(Parsed).GetValue();
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
		TArray<uint8> Bytes;
		if (!LoadStampBytes(IdentityPath(), Bytes))
		{
			return {};
		}
		TWoTBootstrapResult<FIdentity> Parsed = ParseCanonical(Bytes);
		return Parsed.IsError() ? FIdentity{} : MoveTemp(Parsed).GetValue();
	}

	bool IsDisplayableHumanVersion(FStringView HumanVersion)
	{
		if (HumanVersion.IsEmpty())
		{
			return true;
		}
		if (HumanVersion.Len() > MaximumHumanVersionLength
			|| !FChar::IsDigit(HumanVersion[0]))
		{
			return false;
		}
		for (const TCHAR Character : HumanVersion)
		{
			const bool bAsciiAlphanumeric = (Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('A') && Character <= TEXT('Z'))
				|| (Character >= TEXT('a') && Character <= TEXT('z'));
			if (!bAsciiAlphanumeric && Character != TEXT('.') && Character != TEXT('+')
				&& Character != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}

	TWoTBootstrapResult<FString> ParseHumanVersion(TConstArrayView<uint8> Bytes)
	{
		using FResult = TWoTBootstrapResult<FString>;
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
			TEXT("format"), TEXT("human_version"), TEXT("version")};
		if (TOptional<FWoTBootstrapError> Error = RequireExactMembers(
				Root, Fields, TEXT("release version")); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		FString Format;
		if (TOptional<FWoTBootstrapError> Error = ReadStringField(
				Root, TEXT("format"), TEXT("release version"), Format);
			Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		uint32 Version = 0;
		if (TOptional<FWoTBootstrapError> Error = ReadUnsigned32Field(
				Root, TEXT("version"), TEXT("release version"), Version);
			Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (!Format.Equals(HumanVersionFormat, ESearchCase::CaseSensitive)
			|| Version != HumanVersionDocumentVersion)
		{
			return FResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsupportedProtocol,
				TEXT("release version format is unsupported")));
		}
		FString HumanVersion;
		if (TOptional<FWoTBootstrapError> Error = ReadStringField(
				Root, TEXT("human_version"), TEXT("release version"), HumanVersion);
			Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (!IsDisplayableHumanVersion(HumanVersion))
		{
			return FResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnknownField,
				TEXT("release version is not a short version string")));
		}
		return FResult::Success(MoveTemp(HumanVersion));
	}

	FString LoadRunningHumanVersion()
	{
		return LoadHumanVersionAt(FPaths::GetPath(IdentityPath()) / HumanVersionLeaf);
	}

	FString LoadStagedHumanVersion(const FString& StagedTreeDir)
	{
		if (StagedTreeDir.IsEmpty())
		{
			return FString();
		}
#if PLATFORM_MAC
		const TCHAR* const Relative = TEXT("Contents/UE/WoT/Build/WoTRelease");
#else
		const TCHAR* const Relative = TEXT("WoT/Build/WoTRelease");
#endif
		return LoadHumanVersionAt(StagedTreeDir / Relative / HumanVersionLeaf);
	}

	FString DisplayLabel(const FIdentity& Identity, const FString& HumanVersion)
	{
		if (!HumanVersion.IsEmpty() && IsDisplayableHumanVersion(HumanVersion))
		{
			return TEXT("v") + HumanVersion;
		}
		if (Identity.bStamped)
		{
			return TEXT("dev ") + Identity.ReleaseCommit.Left(DevelopmentCommitLength);
		}
		return TEXT("dev");
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
