// Real signed-metadata specs for announcement replay and best-effort state.
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/KeyRing.h"
#include "WoTUpdaterCore/AnnouncementPolicy.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTAnnouncementPolicySpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

constexpr TCHAR WoTAnnouncementPolicySpecPublicKeyHex[] =
	TEXT("03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8");

#if PLATFORM_WINDOWS
constexpr TCHAR WoTAnnouncementPolicySpecValidJson[] = TEXT(
	"{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"format\":\"wot-announcement-v1\",\"key_id\":\"release-policy-test\",\"key_ring\":{\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"size\":222,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/key-ring.json\"},\"key_ring_epoch\":3,\"manifest\":{\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\",\"size\":1316,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/manifest.json\"},\"minimum_gate_protocol\":1,\"platform\":\"Win64\",\"release_sequence\":7}");
constexpr TCHAR WoTAnnouncementPolicySpecValidSignatureHex[] = TEXT(
	"7f57b5a38fdfbdb7bc5cc9c9e63e1ff38ea4abaf7aeae31a93c9cbbb10012bd66e3b80e529de00e7318031fb1a8e62660bbea4c526cdcd49817178efb826970d");
constexpr TCHAR WoTAnnouncementPolicySpecRunningJson[] = TEXT(
	"{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"format\":\"wot-announcement-v1\",\"key_id\":\"release-policy-test\",\"key_ring\":{\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"size\":222,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/key-ring.json\"},\"key_ring_epoch\":3,\"manifest\":{\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\",\"size\":1316,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/manifest.json\"},\"minimum_gate_protocol\":1,\"platform\":\"Win64\",\"release_sequence\":5}");
constexpr TCHAR WoTAnnouncementPolicySpecRunningSignatureHex[] = TEXT(
	"3a2f963eaf4c0ee3a4403727df0cc1dc57a8ea1cc79f8410d78e0049598a1fe4d04b90b74f6129094ab55de781a14b2e67aec91da302483542e64ce84e0a1102");
constexpr TCHAR WoTAnnouncementPolicySpecUnknownJson[] = TEXT(
	"{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"format\":\"wot-announcement-v1\",\"key_id\":\"release-policy-test\",\"key_ring\":{\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"size\":222,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/key-ring.json\"},\"key_ring_epoch\":3,\"manifest\":{\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\",\"size\":1316,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/manifest.json\"},\"minimum_gate_protocol\":1,\"platform\":\"Win64\",\"release_sequence\":7,\"unknown_policy\":true}");
constexpr TCHAR WoTAnnouncementPolicySpecUnknownSignatureHex[] = TEXT(
	"67352ba0c4acc3bf6338ce692af5f380720c38cce7c8697c788aa7d9b74d22b11880d01d31cfca1847d86d45876166d79eb77b1a03aa2313a72333a2c471f60d");
constexpr TCHAR WoTAnnouncementPolicySpecAbsentManifestJson[] = TEXT(
	"{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"format\":\"wot-announcement-v1\",\"key_id\":\"release-policy-test\",\"key_ring\":{\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"size\":222,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/key-ring.json\"},\"key_ring_epoch\":3,\"minimum_gate_protocol\":1,\"platform\":\"Win64\",\"release_sequence\":7}");
constexpr TCHAR WoTAnnouncementPolicySpecAbsentManifestSignatureHex[] = TEXT(
	"c7b035c222b25f088f3a2e0ff3ab69dc3cbf624fa408157f193278a39ba661ea508091d3e6b85b26d9e35695f23f6b21d6900ee80de97ed47ea2150e7e769405");
constexpr TCHAR WoTAnnouncementPolicySpecInvalidManifestJson[] = TEXT(
	"{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"format\":\"wot-announcement-v1\",\"key_id\":\"release-policy-test\",\"key_ring\":{\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"size\":222,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/key-ring.json\"},\"key_ring_epoch\":3,\"manifest\":{},\"minimum_gate_protocol\":1,\"platform\":\"Win64\",\"release_sequence\":7}");
constexpr TCHAR WoTAnnouncementPolicySpecInvalidManifestSignatureHex[] = TEXT(
	"fc9ea190852200f60533345abc140b6a7f363b0edcc68936b26105e6614602f356ecb6db91038b9d8a8eed9e95f78895abef5b479367386d341f4bc6d1164308");
#elif PLATFORM_MAC
constexpr TCHAR WoTAnnouncementPolicySpecValidJson[] = TEXT(
	"{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"format\":\"wot-announcement-v1\",\"key_id\":\"release-policy-test\",\"key_ring\":{\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"size\":222,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/key-ring.json\"},\"key_ring_epoch\":3,\"manifest\":{\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\",\"size\":1316,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/manifest.json\"},\"minimum_gate_protocol\":1,\"platform\":\"Mac\",\"release_sequence\":7}");
constexpr TCHAR WoTAnnouncementPolicySpecValidSignatureHex[] = TEXT(
	"021c2ca4071ea20b552d4dad1aedf8b36a41269fb41b55c2dd82598c88c00f4414dc934b000c1751c0dc461b05f5bbab11cbe3a55cd096e9b3d6b1a06c7f2003");
constexpr TCHAR WoTAnnouncementPolicySpecRunningJson[] = TEXT(
	"{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"format\":\"wot-announcement-v1\",\"key_id\":\"release-policy-test\",\"key_ring\":{\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"size\":222,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/key-ring.json\"},\"key_ring_epoch\":3,\"manifest\":{\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\",\"size\":1316,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/manifest.json\"},\"minimum_gate_protocol\":1,\"platform\":\"Mac\",\"release_sequence\":5}");
constexpr TCHAR WoTAnnouncementPolicySpecRunningSignatureHex[] = TEXT(
	"a7530ccef6654ef41d307a6dcaba0db89d9af1e9303c902d851cec7df90190579cd25b9acf5364cf025c6d099a67fe8d85ede876ee44fdd1357b74b9decbe002");
constexpr TCHAR WoTAnnouncementPolicySpecUnknownJson[] = TEXT(
	"{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"format\":\"wot-announcement-v1\",\"key_id\":\"release-policy-test\",\"key_ring\":{\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"size\":222,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/key-ring.json\"},\"key_ring_epoch\":3,\"manifest\":{\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\",\"size\":1316,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/manifest.json\"},\"minimum_gate_protocol\":1,\"platform\":\"Mac\",\"release_sequence\":7,\"unknown_policy\":true}");
constexpr TCHAR WoTAnnouncementPolicySpecUnknownSignatureHex[] = TEXT(
	"ed86dda68ea3c76033f516960f3ec6641f2b92d66bb1dd9dd75ed8dc033e994105d1901685902f167a5b6c1d8092fba97b4af88daad6336247c2fc761a51c402");
constexpr TCHAR WoTAnnouncementPolicySpecAbsentManifestJson[] = TEXT(
	"{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"format\":\"wot-announcement-v1\",\"key_id\":\"release-policy-test\",\"key_ring\":{\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"size\":222,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/key-ring.json\"},\"key_ring_epoch\":3,\"minimum_gate_protocol\":1,\"platform\":\"Mac\",\"release_sequence\":7}");
constexpr TCHAR WoTAnnouncementPolicySpecAbsentManifestSignatureHex[] = TEXT(
	"4164f3995ed63b6e3b1eb14829542eda02e7eaa93139cf90c11b3e282a1551395ea72839e304f165a220203a3e369976477137651bd007228e2f09e587532b03");
constexpr TCHAR WoTAnnouncementPolicySpecInvalidManifestJson[] = TEXT(
	"{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"format\":\"wot-announcement-v1\",\"key_id\":\"release-policy-test\",\"key_ring\":{\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"size\":222,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/key-ring.json\"},\"key_ring_epoch\":3,\"manifest\":{},\"minimum_gate_protocol\":1,\"platform\":\"Mac\",\"release_sequence\":7}");
constexpr TCHAR WoTAnnouncementPolicySpecInvalidManifestSignatureHex[] = TEXT(
	"1d580c3f97b846d73a1a732e01d86121c515e79871b7caaad4e14d8c3e2d9a183e80455ef2fbbdf72eb587190e5184275e4312de6fd64fb48bb0f77b7dba6807");
#else
#error "WoT announcement policy tests are defined for Win64 and Mac only"
#endif

TArray<uint8> WoTAnnouncementPolicySpecAscii(FStringView Text)
{
	TArray<uint8> Bytes;
	Bytes.Reserve(Text.Len());
	for (const TCHAR Character : Text) Bytes.Add(static_cast<uint8>(Character));
	return Bytes;
}

TArray<uint8> WoTAnnouncementPolicySpecSignature(const TCHAR* Hex)
{
	TArray<uint8> Bytes;
	TWoTBootstrapResult<FWoTEd25519Signature> Parsed = FWoTEd25519Signature::FromHex(Hex);
	if (!Parsed.IsError()) Bytes.Append(Parsed.GetValue().Bytes, FWoTEd25519Signature::ByteCount);
	return Bytes;
}

FWoTKeyRing WoTAnnouncementPolicySpecTrustedRing()
{
	FWoTKeyRing Ring;
	Ring.Format = WoTReleaseKeyRingFormat;
	Ring.Epoch = 3;
	FWoTReleaseKey Key;
	Key.KeyId = TEXT("dedicated-announcement-policy-test");
	TWoTBootstrapResult<FWoTEd25519PublicKey> Parsed =
		FWoTEd25519PublicKey::FromHex(WoTAnnouncementPolicySpecPublicKeyHex);
	if (!Parsed.IsError()) Key.PublicKey = Parsed.GetValue();
	Key.FirstSequence = 0;
	Key.LastSequence = MAX_uint64;
	Ring.Keys.Add(MoveTemp(Key));
	return Ring;
}

class FWoTAnnouncementPolicySpecRoot final
{
public:
	FWoTAnnouncementPolicySpecRoot()
	{
		Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
			TEXT("AnnouncementPolicy"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeDirectoryName(Path);
		IFileManager::Get().MakeDirectory(*Path, true);
	}
	~FWoTAnnouncementPolicySpecRoot()
	{
		IFileManager::Get().DeleteDirectory(*Path, false, true);
	}
	FWoTAnnouncementPolicySpecRoot(const FWoTAnnouncementPolicySpecRoot&) = delete;
	FWoTAnnouncementPolicySpecRoot& operator=(const FWoTAnnouncementPolicySpecRoot&) = delete;
	FString Path;
};

bool WoTAnnouncementPolicySpecResolve(FAutomationTestBase& Test,
	const FWoTAnnouncementPolicySpecRoot& Fixture, FWoTUpdaterStateRoot& OutRoot)
{
	TWoTBootstrapResult<FWoTUpdaterStateRoot> Resolved = ResolveUpdaterUserStateRoot(Fixture.Path);
	if (!Test.TestFalse(TEXT("the updater state root resolves"), Resolved.IsError()))
	{
		if (Resolved.IsError()) Test.AddError(Resolved.GetError().ToString());
		return false;
	}
	OutRoot = MoveTemp(Resolved).GetValue();
	return true;
}

class FWoTAnnouncementPolicySpecTransport final : public IWoTAnnouncementTransport
{
public:
	FWoTAnnouncementPolicySpecTransport(TArray<uint8> InAnnouncement,
		TArray<uint8> InSignature, bool bInUnavailable = false)
		: Announcement(MoveTemp(InAnnouncement)), Signature(MoveTemp(InSignature)),
		  bUnavailable(bInUnavailable) {}
	virtual FWoTBootstrapStatus FetchPair(TArray<uint8>& OutAnnouncement,
		TArray<uint8>& OutSignature) override
	{
		OutAnnouncement.Reset();
		OutSignature.Reset();
		if (bUnavailable)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("the scripted announcement service is unavailable")));
		}
		OutAnnouncement = Announcement;
		OutSignature = Signature;
		return FWoTBootstrapStatus::Success();
	}
private:
	TArray<uint8> Announcement;
	TArray<uint8> Signature;
	bool bUnavailable = false;
};

FWoTAnnouncementPolicySpecTransport WoTAnnouncementPolicySpecSequence7()
{
	return FWoTAnnouncementPolicySpecTransport(
		WoTAnnouncementPolicySpecAscii(WoTAnnouncementPolicySpecValidJson),
		WoTAnnouncementPolicySpecSignature(WoTAnnouncementPolicySpecValidSignatureHex));
}
FWoTAnnouncementPolicySpecTransport WoTAnnouncementPolicySpecSequence5()
{
	return FWoTAnnouncementPolicySpecTransport(
		WoTAnnouncementPolicySpecAscii(WoTAnnouncementPolicySpecRunningJson),
		WoTAnnouncementPolicySpecSignature(WoTAnnouncementPolicySpecRunningSignatureHex));
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAnnouncementPolicyAuthenticationSpec,
	"WoT.UpdaterCore.Announcement.AuthenticationBeforeParse", WoTAnnouncementPolicySpecFlags)
bool FWoTAnnouncementPolicyAuthenticationSpec::RunTest(const FString&)
{
	const FWoTKeyRing Ring = WoTAnnouncementPolicySpecTrustedRing();
	{
		FWoTAnnouncementPolicySpecRoot Fixture;
		FWoTUpdaterStateRoot Root;
		if (!WoTAnnouncementPolicySpecResolve(*this, Fixture, Root)) return false;
		FWoTUpdateStateRecord State;
		FWoTAnnouncementPolicySpecTransport Transport(
			WoTAnnouncementPolicySpecAscii(TEXT("{")),
			WoTAnnouncementPolicySpecSignature(WoTAnnouncementPolicySpecValidSignatureHex));
		auto Assessed = AssessAnnouncement(Transport, Ring, State, Root, 0);
		if (TestTrue(TEXT("a signature for different bytes is refused"), Assessed.IsError()))
			TestEqual(TEXT("signature refusal happens before JSON parsing"),
				Assessed.GetError().Code, EWoTBootstrapError::Crypto);
		TestEqual(TEXT("unauthenticated bytes do not change memory"),
			State.NewestAnnouncementSequence, uint64(0));
	}
	{
		FWoTAnnouncementPolicySpecRoot Fixture;
		FWoTUpdaterStateRoot Root;
		if (!WoTAnnouncementPolicySpecResolve(*this, Fixture, Root)) return false;
		FWoTUpdateStateRecord State;
		FWoTAnnouncementPolicySpecTransport Transport(
			WoTAnnouncementPolicySpecAscii(WoTAnnouncementPolicySpecUnknownJson),
			WoTAnnouncementPolicySpecSignature(WoTAnnouncementPolicySpecUnknownSignatureHex));
		auto Assessed = AssessAnnouncement(Transport, Ring, State, Root, 0);
		if (TestTrue(TEXT("a signed unknown field is refused"), Assessed.IsError()))
			TestEqual(TEXT("the closed schema reports UnknownField"),
				Assessed.GetError().Code, EWoTBootstrapError::UnknownField);
		TestEqual(TEXT("a refused signed schema does not change memory"),
			State.NewestAnnouncementSequence, uint64(0));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAnnouncementPolicyReplaySpec,
	"WoT.UpdaterCore.Announcement.ReplayFloorAndEqualRetry", WoTAnnouncementPolicySpecFlags)
bool FWoTAnnouncementPolicyReplaySpec::RunTest(const FString&)
{
	const FWoTKeyRing Ring = WoTAnnouncementPolicySpecTrustedRing();
	{
		FWoTAnnouncementPolicySpecRoot Fixture;
		FWoTUpdaterStateRoot Root;
		if (!WoTAnnouncementPolicySpecResolve(*this, Fixture, Root)) return false;
		FWoTUpdateStateRecord State;
		State.NewestAnnouncementSequence = 7;
		FWoTAnnouncementPolicySpecTransport Transport = WoTAnnouncementPolicySpecSequence5();
		auto Assessed = AssessAnnouncement(Transport, Ring, State, Root, 3);
		if (!TestFalse(TEXT("the signed replay is assessed"), Assessed.IsError())) return false;
		TestEqual(TEXT("an announcement below the persisted floor is ignored"),
			Assessed.GetValue().Decision, EWoTUpdateDecision::Current);
		TestEqual(TEXT("the replay cannot lower memory"), State.NewestAnnouncementSequence, uint64(7));
	}
	{
		FWoTAnnouncementPolicySpecRoot Fixture;
		FWoTUpdaterStateRoot Root;
		if (!WoTAnnouncementPolicySpecResolve(*this, Fixture, Root)) return false;
		FWoTUpdateStateRecord State;
		State.NewestAnnouncementSequence = 5;
		FWoTAnnouncementPolicySpecTransport Transport = WoTAnnouncementPolicySpecSequence5();
		auto Assessed = AssessAnnouncement(Transport, Ring, State, Root, 3);
		if (!TestFalse(TEXT("the equal signed announcement is assessed"), Assessed.IsError())) return false;
		TestEqual(TEXT("an equal unfinished announcement may retry"),
			Assessed.GetValue().Decision, EWoTUpdateDecision::MustStage);
	}
	{
		FWoTAnnouncementPolicySpecRoot Fixture;
		FWoTUpdaterStateRoot Root;
		if (!WoTAnnouncementPolicySpecResolve(*this, Fixture, Root)) return false;
		FWoTUpdateStateRecord State;
		FWoTAnnouncementPolicySpecTransport Transport = WoTAnnouncementPolicySpecSequence7();
		auto Assessed = AssessAnnouncement(Transport, Ring, State, Root, 0);
		if (!TestFalse(TEXT("the newer signed announcement is assessed"), Assessed.IsError())) return false;
		TestEqual(TEXT("new signed metadata stages"), Assessed.GetValue().Decision,
			EWoTUpdateDecision::MustStage);
		TestEqual(TEXT("the authenticated sequence raises memory"),
			State.NewestAnnouncementSequence, uint64(7));
		TestEqual(TEXT("an announcement claim never raises the root-verified floor"),
			State.KeyRevocationFloor, uint64(0));
		const FWoTUpdateStateRecordReadResult Read = ReadWoTUpdateState(Root);
		TestEqual(TEXT("the authenticated sequence is persisted"),
			Read.State.NewestAnnouncementSequence, uint64(7));
		TestEqual(TEXT("the claimed ring epoch is not persisted"),
			Read.State.KeyRevocationFloor, uint64(0));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAnnouncementPolicyUnusableManifestSpec,
	"WoT.UpdaterCore.Announcement.SignedUnusableManifestRecordsSequence",
	WoTAnnouncementPolicySpecFlags)
bool FWoTAnnouncementPolicyUnusableManifestSpec::RunTest(const FString&)
{
	struct FCase
	{
		const TCHAR* Label;
		const TCHAR* Json;
		const TCHAR* Signature;
	};
	const FCase Cases[] = {
		{TEXT("absent"), WoTAnnouncementPolicySpecAbsentManifestJson,
			WoTAnnouncementPolicySpecAbsentManifestSignatureHex},
		{TEXT("invalid"), WoTAnnouncementPolicySpecInvalidManifestJson,
			WoTAnnouncementPolicySpecInvalidManifestSignatureHex},
	};
	const FWoTKeyRing Ring = WoTAnnouncementPolicySpecTrustedRing();
	for (const FCase& Case : Cases)
	{
		FWoTAnnouncementPolicySpecRoot Fixture;
		FWoTUpdaterStateRoot Root;
		if (!WoTAnnouncementPolicySpecResolve(*this, Fixture, Root)) return false;
		FWoTUpdateStateRecord State;
		FWoTAnnouncementPolicySpecTransport Transport(
			WoTAnnouncementPolicySpecAscii(Case.Json),
			WoTAnnouncementPolicySpecSignature(Case.Signature));
		auto Assessed = AssessAnnouncement(Transport, Ring, State, Root, 0);
		if (!TestFalse(FString::Printf(TEXT("the signed %s manifest is assessed"),
				Case.Label), Assessed.IsError())) return false;
		TestEqual(FString::Printf(TEXT("the %s manifest cannot stage"), Case.Label),
			Assessed.GetValue().Decision, EWoTUpdateDecision::StayBlocked);
		TestEqual(TEXT("authenticated unusable metadata still raises memory"),
			State.NewestAnnouncementSequence, uint64(7));
		TestEqual(TEXT("authenticated unusable metadata is persisted"),
			ReadWoTUpdateState(Root).State.NewestAnnouncementSequence, uint64(7));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAnnouncementPolicyBestEffortSpec,
	"WoT.UpdaterCore.Announcement.CorruptAndUnwritableStateStayPlayable",
	WoTAnnouncementPolicySpecFlags)
bool FWoTAnnouncementPolicyBestEffortSpec::RunTest(const FString&)
{
	const FWoTKeyRing Ring = WoTAnnouncementPolicySpecTrustedRing();
	{
		FWoTAnnouncementPolicySpecRoot Fixture;
		FWoTUpdaterStateRoot Root;
		if (!WoTAnnouncementPolicySpecResolve(*this, Fixture, Root)) return false;
		IFileManager::Get().MakeDirectory(*Root.StateDir, true);
		TestTrue(TEXT("the corrupt real-path record is written"),
			FFileHelper::SaveStringToFile(TEXT("not-json"),
				*FPaths::Combine(Root.StateDir, TEXT("update-state.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		FWoTUpdateStateRecord State = ReadWoTUpdateState(Root).State;
		FWoTAnnouncementPolicySpecTransport Transport = WoTAnnouncementPolicySpecSequence7();
		auto Assessed = AssessAnnouncement(Transport, Ring, State, Root, 0);
		if (!TestFalse(TEXT("corrupt state does not reject signed metadata"), Assessed.IsError())) return false;
		TestEqual(TEXT("gameplay can proceed to staging after corrupt state"),
			Assessed.GetValue().Decision, EWoTUpdateDecision::MustStage);
		TestFalse(TEXT("corrupt state produces a diagnostic"),
			Assessed.GetValue().NotifyOnlyDetail.IsEmpty());
	}
	{
		FWoTAnnouncementPolicySpecRoot Fixture;
		FWoTUpdaterStateRoot Root;
		if (!WoTAnnouncementPolicySpecResolve(*this, Fixture, Root)) return false;
		IFileManager::Get().MakeDirectory(*Root.StateDir, true);
		TestTrue(TEXT("a directory blocks record replacement"),
			IFileManager::Get().MakeDirectory(
				*FPaths::Combine(Root.StateDir, TEXT("update-state.json")), false));
		FWoTUpdateStateRecord State;
		FWoTAnnouncementPolicySpecTransport Transport = WoTAnnouncementPolicySpecSequence7();
		auto Assessed = AssessAnnouncement(Transport, Ring, State, Root, 0);
		if (!TestFalse(TEXT("unwritable state does not reject signed metadata"), Assessed.IsError())) return false;
		TestEqual(TEXT("gameplay can proceed to staging after write failure"),
			Assessed.GetValue().Decision, EWoTUpdateDecision::MustStage);
		TestEqual(TEXT("write failure retains authenticated memory"),
			State.NewestAnnouncementSequence, uint64(7));
		TestFalse(TEXT("write failure produces a diagnostic"),
			Assessed.GetValue().NotifyOnlyDetail.IsEmpty());
	}
	{
		FWoTAnnouncementPolicySpecRoot Fixture;
		FWoTUpdaterStateRoot Root;
		if (!WoTAnnouncementPolicySpecResolve(*this, Fixture, Root)) return false;
		FWoTUpdateStateRecord State;
		FWoTAnnouncementPolicySpecTransport Offline({}, {}, true);
		auto Assessed = AssessAnnouncement(Offline, Ring, State, Root, 0);
		if (!TestFalse(TEXT("an unavailable service becomes a decision"), Assessed.IsError())) return false;
		TestEqual(TEXT("an unavailable service does not gate gameplay"),
			Assessed.GetValue().Decision, EWoTUpdateDecision::ContinueCurrentAndLogUnavailable);
	}
	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
