#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Updater/WoTReleaseIdentity.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
	constexpr EAutomationTestFlags WoTReleaseIdentitySpecFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::ProductFilter;

	bool WoTReleaseIdentitySpecWriteAscii(const FString& Path, FStringView Text)
	{
		FTCHARToUTF8 Converted(Text.GetData(), Text.Len());
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		return FFileHelper::SaveArrayToFile(Bytes, *Path);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseIdentityLoaderSpec,
	"WoT.Update.Identity.Loader", WoTReleaseIdentitySpecFlags)

bool FWoTReleaseIdentityLoaderSpec::RunTest(const FString&)
{
	FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::AutomationTransientDir(), TEXT("wot-release-identity-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower()));
	FPaths::NormalizeDirectoryName(Directory);
	IFileManager::Get().MakeDirectory(*Directory, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};

	const FString IdentityPath = Directory / TEXT("release-identity.json");
	auto LoadAt = [](const FString& Path)
	{
		WoTReleaseIdentity::FScopedPathOverrideForTesting Override(Path);
		return WoTReleaseIdentity::LoadRunning();
	};
	auto TestUnstamped = [this](const TCHAR* Label,
		const WoTReleaseIdentity::FIdentity& Identity)
	{
		TestEqual(FString::Printf(TEXT("%s sequence"), Label),
			Identity.ReleaseSequence, uint64(0));
		TestTrue(FString::Printf(TEXT("%s commit"), Label),
			Identity.ReleaseCommit.IsEmpty());
		TestFalse(FString::Printf(TEXT("%s stamped"), Label),
			Identity.bStamped);
	};

	TestTrue(TEXT("valid stamped fixture writes"), WoTReleaseIdentitySpecWriteAscii(
		IdentityPath,
		TEXTVIEW("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"abc123456\",\"release_sequence\":5,\"version\":1}")));
	const WoTReleaseIdentity::FIdentity Stamped = LoadAt(IdentityPath);
	TestEqual(TEXT("valid stamp sequence"), Stamped.ReleaseSequence, uint64(5));
	TestEqual(TEXT("valid stamp commit"), Stamped.ReleaseCommit,
		FString(TEXT("abc123456")));
	TestTrue(TEXT("valid non-empty identity is stamped"), Stamped.bStamped);

	TestTrue(TEXT("development fixture writes"), WoTReleaseIdentitySpecWriteAscii(
		IdentityPath,
		TEXTVIEW("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"\",\"release_sequence\":0,\"version\":1}")));
	TestUnstamped(TEXT("development identity"), LoadAt(IdentityPath));
	TestTrue(TEXT("empty-commit positive-sequence fixture writes"),
		WoTReleaseIdentitySpecWriteAscii(
			IdentityPath,
			TEXTVIEW("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"\",\"release_sequence\":9,\"version\":1}")));
	TestUnstamped(TEXT("empty-commit positive-sequence identity"),
		LoadAt(IdentityPath));
	TestUnstamped(TEXT("missing identity"), LoadAt(Directory / TEXT("missing.json")));

	const TPair<const TCHAR*, const TCHAR*> InvalidCases[] = {
		{TEXT("malformed"), TEXT("{")},
		{TEXT("wrong format"), TEXT("{\"format\":\"wrong\",\"release_commit\":\"abc123456\",\"release_sequence\":5,\"version\":1}")},
		{TEXT("wrong version"), TEXT("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"abc123456\",\"release_sequence\":5,\"version\":2}")},
		{TEXT("extra field"), TEXT("{\"extra\":true,\"format\":\"wot-release-identity-v1\",\"release_commit\":\"abc123456\",\"release_sequence\":5,\"version\":1}")},
		{TEXT("negative sequence"), TEXT("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"abc123456\",\"release_sequence\":-1,\"version\":1}")},
		{TEXT("sequence above int64"), TEXT("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"abc123456\",\"release_sequence\":9223372036854775808,\"version\":1}")},
		{TEXT("non-integer sequence"), TEXT("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"abc123456\",\"release_sequence\":1.5,\"version\":1}")},
		{TEXT("reordered keys"), TEXT("{\"release_commit\":\"abc123456\",\"format\":\"wot-release-identity-v1\",\"release_sequence\":5,\"version\":1}")},
		{TEXT("inner whitespace"), TEXT("{\"format\": \"wot-release-identity-v1\",\"release_commit\":\"abc123456\",\"release_sequence\":5,\"version\":1}")},
		{TEXT("trailing newline"), TEXT("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"abc123456\",\"release_sequence\":5,\"version\":1}\n")},
	};
	for (const TPair<const TCHAR*, const TCHAR*>& Case : InvalidCases)
	{
		TestTrue(FString::Printf(TEXT("%s fixture writes"), Case.Key),
			WoTReleaseIdentitySpecWriteAscii(IdentityPath, Case.Value));
		TestUnstamped(Case.Key, LoadAt(IdentityPath));
	}

	TArray<uint8> Oversize;
	Oversize.Init(static_cast<uint8>('x'), 4 * 1024 + 1);
	TestTrue(TEXT("oversize fixture writes"),
		FFileHelper::SaveArrayToFile(Oversize, *IdentityPath));
	TestUnstamped(TEXT("oversize identity"), LoadAt(IdentityPath));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseIdentityHumanVersionSpec,
	"WoT.Update.Identity.HumanVersion", WoTReleaseIdentitySpecFlags)

bool FWoTReleaseIdentityHumanVersionSpec::RunTest(const FString&)
{
	FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::AutomationTransientDir(), TEXT("wot-release-version-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower()));
	FPaths::NormalizeDirectoryName(Directory);
	IFileManager::Get().MakeDirectory(*Directory, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	auto Parse = [](FStringView Text)
	{
		FTCHARToUTF8 Converted(Text.GetData(), Text.Len());
		return WoTReleaseIdentity::ParseHumanVersion(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length()));
	};
	for (const TCHAR* Valid : {TEXT(""), TEXT("0.1.2"), TEXT("0.2.0-rc1"),
		TEXT("1.0.0+mac"), TEXT("01234567890123456789012345678901")})
	{
		TWoTBootstrapResult<FString> Parsed = Parse(FString::Printf(
			TEXT("{\"format\":\"wot-release-version-v1\",\"human_version\":\"%s\",\"version\":1}"),
			Valid));
		TestTrue(FString::Printf(TEXT("valid version parses: [%s]"), Valid),
			!Parsed.IsError() && Parsed.GetValue() == Valid);
	}
	const TPair<const TCHAR*, const TCHAR*> InvalidCases[] = {
		{TEXT("leading v"), TEXT("{\"format\":\"wot-release-version-v1\",\"human_version\":\"v0.1.2\",\"version\":1}")},
		{TEXT("too long"), TEXT("{\"format\":\"wot-release-version-v1\",\"human_version\":\"012345678901234567890123456789012\",\"version\":1}")},
		{TEXT("space"), TEXT("{\"format\":\"wot-release-version-v1\",\"human_version\":\"0.1 2\",\"version\":1}")},
		{TEXT("wrong format"), TEXT("{\"format\":\"wot-release-identity-v1\",\"human_version\":\"0.1.2\",\"version\":1}")},
		{TEXT("wrong version"), TEXT("{\"format\":\"wot-release-version-v1\",\"human_version\":\"0.1.2\",\"version\":2}")},
		{TEXT("extra field"), TEXT("{\"extra\":1,\"format\":\"wot-release-version-v1\",\"human_version\":\"0.1.2\",\"version\":1}")},
		{TEXT("missing field"), TEXT("{\"format\":\"wot-release-version-v1\",\"version\":1}")},
		{TEXT("trailing newline"), TEXT("{\"format\":\"wot-release-version-v1\",\"human_version\":\"0.1.2\",\"version\":1}\n")},
	};
	for (const TPair<const TCHAR*, const TCHAR*>& Case : InvalidCases)
	{
		TestTrue(FString::Printf(TEXT("invalid version is refused: %s"), Case.Key),
			Parse(Case.Value).IsError());
	}

	// The running file sits beside the identity, so one override moves both.
	const FString IdentityPath = Directory / TEXT("release-identity.json");
	TestTrue(TEXT("stamped identity writes"), WoTReleaseIdentitySpecWriteAscii(IdentityPath,
		TEXTVIEW("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"60de31709abcdef\",\"release_sequence\":2,\"version\":1}")));
	WoTReleaseIdentity::FScopedPathOverrideForTesting Override(IdentityPath);
	TestEqual(TEXT("absent version file is unversioned"),
		WoTReleaseIdentity::LoadRunningHumanVersion(), FString());
	TestEqual(TEXT("stamped unversioned package shows dev and its commit"),
		WoTReleaseIdentity::DisplayLabel(WoTReleaseIdentity::LoadRunning(),
			WoTReleaseIdentity::LoadRunningHumanVersion()),
		FString(TEXT("dev 60de31709")));
	TestTrue(TEXT("running version writes"), WoTReleaseIdentitySpecWriteAscii(
		Directory / TEXT("release-version.json"),
		TEXTVIEW("{\"format\":\"wot-release-version-v1\",\"human_version\":\"0.1.2\",\"version\":1}")));
	TestEqual(TEXT("running version loads"),
		WoTReleaseIdentity::LoadRunningHumanVersion(), FString(TEXT("0.1.2")));
	TestEqual(TEXT("versioned package shows v and its version"),
		WoTReleaseIdentity::DisplayLabel(WoTReleaseIdentity::LoadRunning(),
			WoTReleaseIdentity::LoadRunningHumanVersion()),
		FString(TEXT("v0.1.2")));
	TestEqual(TEXT("an unstamped build shows dev"),
		WoTReleaseIdentity::DisplayLabel(WoTReleaseIdentity::FIdentity(), FString()),
		FString(TEXT("dev")));
	TestEqual(TEXT("an undisplayable version never reaches the label"),
		WoTReleaseIdentity::DisplayLabel(WoTReleaseIdentity::FIdentity(),
			TEXT("0.1.2 <b>")),
		FString(TEXT("dev")));

	// A staged release tree carries its stamp where the platform's tree does.
	const FString StagedTree = Directory / TEXT("staged-tree");
#if PLATFORM_MAC
	const FString StagedStamp = StagedTree
		/ TEXT("Contents/UE/WoT/Build/WoTRelease/release-version.json");
#else
	const FString StagedStamp = StagedTree
		/ TEXT("WoT/Build/WoTRelease/release-version.json");
#endif
	TestEqual(TEXT("a staged tree without a stamp is unversioned"),
		WoTReleaseIdentity::LoadStagedHumanVersion(StagedTree), FString());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(StagedStamp), true);
	TestTrue(TEXT("staged version writes"), WoTReleaseIdentitySpecWriteAscii(StagedStamp,
		TEXTVIEW("{\"format\":\"wot-release-version-v1\",\"human_version\":\"0.1.3\",\"version\":1}")));
	TestEqual(TEXT("staged version loads"),
		WoTReleaseIdentity::LoadStagedHumanVersion(StagedTree), FString(TEXT("0.1.3")));
	TestEqual(TEXT("an empty staged path is unversioned"),
		WoTReleaseIdentity::LoadStagedHumanVersion(FString()), FString());
	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
