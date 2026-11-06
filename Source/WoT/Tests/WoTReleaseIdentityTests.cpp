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

#endif  // WITH_AUTOMATION_TESTS
