// Specs for the frozen signed announcement, ring, and manifest envelopes.
#include "Misc/AutomationTest.h"
#include "WoTBootstrapCore/Announcement.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/KeyRing.h"
#include "WoTBootstrapCore/ReleaseManifest.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTOrderingSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

const TCHAR* const OrderDomain = TEXT("com.aleiby.wot.release.v1");
const TCHAR* const OrderPlatform = TEXT("Mac");
const TCHAR* const OrderAnnouncementKeyHex = TEXT("ed54a59fb1ac3a51239351362941b868e85a60e3d7b2485d828821dc7a69c279");
const TCHAR* const OrderAnnouncementJson = TEXT("{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"format\":\"wot-announcement-v1\",\"key_id\":\"release-2026a\",\"key_ring\":{\"sha256\":\"920035d41d3a4e09ec1ea800719aeaee3bae4747230dc3f2ff7a92aacf65e786\",\"size\":222,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/key-ring.json\"},\"key_ring_epoch\":3,\"manifest\":{\"sha256\":\"3bf676702b83085b489e9cb8c0be83c24f341892f8b81f010e4c56095206db41\",\"size\":1171,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/manifest.json\"},\"minimum_gate_protocol\":1,\"platform\":\"Mac\",\"release_sequence\":7}");
const TCHAR* const OrderAnnouncementSignature = TEXT("c6c8ac1f94ecf00e3f9a29ce4f02efba558cd3f543ebf1ec4b4fa2cf4da345dd0708c668383fe5da49a1a056ed8b4a4c4c14f5ca889377921c0fe4a0a59d7d09");
const TCHAR* const OrderRingJson = TEXT("{\"epoch\":3,\"format\":\"wot-release-key-ring-v1\",\"keys\":[{\"first_sequence\":1,\"key_id\":\"release-2026a\",\"last_sequence\":100,\"public_key\":\"14c70c7e0c4c7712756ebbdfd33317be8fdf76358824e636098912ced81c1fb1\"}],\"revoked_key_ids\":[]}");
const TCHAR* const OrderManifestJson = TEXT("{\"announcement_domain\":\"com.aleiby.wot.release.v1\",\"archive\":{\"name\":\"WoT-Mac-7.zip\",\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"size\":4096,\"url\":\"https://github.com/aleiby/wot/releases/download/v7/WoT-Mac-7.zip\"},\"archive_root_id\":\"wot-archive-root-v1\",\"build_commit\":\"abc123456\",\"cache_contract_id\":\"wot-cache-contract-1\",\"cache_contract_version\":3,\"format\":\"wot-release-manifest-v2\",\"generated_utc\":\"2026-07-25T00:00:00+00:00\",\"human_version\":\"0.7.0\",\"installed_tree_root\":{\"name\":\"WoT.app\",\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\",\"size\":1024},\"installed_tree_version\":1,\"key_id\":\"release-2026a\",\"main_executable\":{\"name\":\"WoT\",\"sha256\":\"3333333333333333333333333333333333333333333333333333333333333333\",\"size\":2048},\"manifest_version\":2,\"minimum_commit_service_protocol\":1,\"minimum_gate_protocol\":1,\"minimum_updater_protocol\":1,\"platform\":\"Mac\",\"release_sequence\":7,\"stripped_audit_verdict\":\"pass\",\"target_attestation\":{\"configuration\":\"Shipping\",\"dedicated_server_startup_available\":false,\"launch_module\":\"WoTLaunch\",\"listen_server_present\":true,\"release_gate_enabled\":false,\"target_type\":\"Game\"}}");
const TCHAR* const OrderManifestSignature = TEXT("ac351f105834d84d7a1ed58c2db094e461e45154e9c8c8417dd46ec493c319c43b351558211bed77e7918c2005d3f0d71ef4cdc087a852845c9b985f4a5c580a");

TArray<uint8> WoTOrderingSpecBytes(FStringView Ascii)
{
	TArray<uint8> Bytes;
	Bytes.Reserve(Ascii.Len());
	for (const TCHAR Character : Ascii)
	{
		Bytes.Add(static_cast<uint8>(Character));
	}
	return Bytes;
}

FWoTEd25519PublicKey WoTOrderingSpecKey(const TCHAR* Hex)
{
	TWoTBootstrapResult<FWoTEd25519PublicKey> Parsed = FWoTEd25519PublicKey::FromHex(Hex);
	return Parsed.IsError() ? FWoTEd25519PublicKey() : Parsed.GetValue();
}

FWoTEd25519Signature WoTOrderingSpecSignature(const TCHAR* Hex)
{
	TWoTBootstrapResult<FWoTEd25519Signature> Parsed = FWoTEd25519Signature::FromHex(Hex);
	return Parsed.IsError() ? FWoTEd25519Signature() : Parsed.GetValue();
}

TWoTBootstrapResult<FWoTAnnouncementV1> WoTOrderingSpecAnnouncement(
	FStringView Json = OrderAnnouncementJson,
	const TCHAR* SignatureHex = OrderAnnouncementSignature,
	FStringView Platform = OrderPlatform, FStringView Domain = OrderDomain)
{
	return VerifyAnnouncementV1(WoTOrderingSpecBytes(Json),
		WoTOrderingSpecSignature(SignatureHex), Platform, Domain,
		WoTOrderingSpecKey(OrderAnnouncementKeyHex));
}

TWoTBootstrapResult<FWoTKeyRing> WoTOrderingSpecRing(FStringView Json = OrderRingJson)
{
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Document =
		ParseCanonicalJson(WoTOrderingSpecBytes(Json));
	if (Document.IsError())
	{
		return TWoTBootstrapResult<FWoTKeyRing>::Error(MoveTemp(Document).GetError());
	}
	return ParseKeyRing(Document.GetValue());
}

}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOrderingAnnouncementSpec,
	"WoT.BootstrapCore.Ordering.Announcement", WoTOrderingSpecFlags)

bool FWoTOrderingAnnouncementSpec::RunTest(const FString&)
{
	TWoTBootstrapResult<FWoTAnnouncementV1> Parsed = WoTOrderingSpecAnnouncement();
	if (!TestFalse(TEXT("the signed announcement verifies and parses"), Parsed.IsError()))
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	const FWoTAnnouncementV1& Announcement = Parsed.GetValue();
	TestEqual(TEXT("format"), Announcement.Format, FString(WoTAnnouncementV1Format));
	TestEqual(TEXT("platform"), Announcement.Platform, FString(OrderPlatform));
	TestEqual(TEXT("domain"), Announcement.AnnouncementDomain, FString(OrderDomain));
	TestEqual(TEXT("release sequence"),
		static_cast<int64>(Announcement.ReleaseSequence), static_cast<int64>(7));
	TestEqual(TEXT("key ring epoch"),
		static_cast<int64>(Announcement.KeyRingEpoch), static_cast<int64>(3));
	TestEqual(TEXT("minimum gate protocol"),
		static_cast<int64>(Announcement.MinimumGateProtocol), static_cast<int64>(1));
	TestEqual(TEXT("key id"), Announcement.KeyId, FString(TEXT("release-2026a")));
	TestEqual(TEXT("manifest size"),
		static_cast<int64>(Announcement.Manifest.Size), static_cast<int64>(1171));
	TestTrue(TEXT("the key ring url is HTTPS"),
		Announcement.KeyRing.Url.StartsWith(TEXT("https://")));

	// The signature is checked before any field is believed, so a tampered byte
	// fails as a signature failure rather than as a parse quirk.
	{
		FString Tampered = OrderAnnouncementJson;
		Tampered.ReplaceInline(TEXT("\"release_sequence\":7"), TEXT("\"release_sequence\":8"));
		TWoTBootstrapResult<FWoTAnnouncementV1> Rejected =
			WoTOrderingSpecAnnouncement(Tampered);
		if (TestTrue(TEXT("a tampered announcement is refused"), Rejected.IsError()))
		{
			TestEqual(TEXT("tampering reports Crypto"), Rejected.GetError().Code,
				EWoTBootstrapError::Crypto);
		}
	}

	// Wrong platform and wrong domain: valid signatures over something not ours.
	{
		TWoTBootstrapResult<FWoTAnnouncementV1> WrongPlatform =
			WoTOrderingSpecAnnouncement(OrderAnnouncementJson, OrderAnnouncementSignature,
				TEXT("Win64"), OrderDomain);
		TestTrue(TEXT("an announcement for the other platform is refused"),
			WrongPlatform.IsError());

		TWoTBootstrapResult<FWoTAnnouncementV1> WrongDomain =
			WoTOrderingSpecAnnouncement(OrderAnnouncementJson, OrderAnnouncementSignature,
				OrderPlatform, TEXT("com.aleiby.wot.release.v2"));
		if (TestTrue(TEXT("a different domain is never adopted from the network"),
				WrongDomain.IsError()))
		{
			TestEqual(TEXT("a foreign domain reports UnsupportedProtocol"),
				WrongDomain.GetError().Code, EWoTBootstrapError::UnsupportedProtocol);
		}
	}

	// The wrong key must not authenticate it.
	{
		TWoTBootstrapResult<FWoTAnnouncementV1> WrongKey = VerifyAnnouncementV1(
			WoTOrderingSpecBytes(OrderAnnouncementJson),
			WoTOrderingSpecSignature(OrderAnnouncementSignature), OrderPlatform,
			OrderDomain, WoTOrderingSpecKey(
				TEXT("63e201504407a45299acd5db1d16e9489a22badbb9e21159c9a255633f4ea81a")));
		TestTrue(TEXT("the release key cannot authenticate an announcement"),
			WrongKey.IsError());
	}

	// The envelope schema is closed and permanent.
	{
		FString Extra = OrderAnnouncementJson;
		Extra.ReplaceInline(TEXT("\"format\":"), TEXT("\"extra\":1,\"format\":"));
		TestTrue(TEXT("an unknown announcement field is refused"),
			WoTOrderingSpecAnnouncement(Extra).IsError());

		// Plaintext references are refused even though the object is hashed.
		FString Plaintext = OrderAnnouncementJson;
		Plaintext.ReplaceInline(TEXT("https://github.com/aleiby/wot/releases/download/v7/key-ring.json"),
			TEXT("http://github.com/aleiby/wot/releases/download/v7/key-ring.json"));
		TestTrue(TEXT("a non-HTTPS reference is refused"),
			WoTOrderingSpecAnnouncement(Plaintext).IsError());
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOrderingManifestSpec,
	"WoT.BootstrapCore.Ordering.Manifest", WoTOrderingSpecFlags)

bool FWoTOrderingManifestSpec::RunTest(const FString&)
{
	TWoTBootstrapResult<FWoTAnnouncementV1> Announcement = WoTOrderingSpecAnnouncement();
	TWoTBootstrapResult<FWoTKeyRing> Ring = WoTOrderingSpecRing();
	if (!TestFalse(TEXT("fixtures load"), Announcement.IsError() || Ring.IsError()))
	{
		return false;
	}

	TWoTBootstrapResult<FWoTReleaseManifestV2> Parsed = VerifyReleaseManifestV2(
		WoTOrderingSpecBytes(OrderManifestJson),
		WoTOrderingSpecSignature(OrderManifestSignature),
		Announcement.GetValue(), Ring.GetValue());
	if (!TestFalse(TEXT("the signed manifest verifies and parses"), Parsed.IsError()))
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	const FWoTReleaseManifestV2& Manifest = Parsed.GetValue();
	TestEqual(TEXT("manifest version"),
		static_cast<int64>(Manifest.ManifestVersion), static_cast<int64>(2));
	TestEqual(TEXT("human version"), Manifest.HumanVersion, FString(TEXT("0.7.0")));
	TestEqual(TEXT("archive name"), Manifest.ArchiveName, FString(TEXT("WoT-Mac-7.zip")));
	TestEqual(TEXT("archive size"),
		static_cast<int64>(Manifest.Archive.Size), static_cast<int64>(4096));
	TestEqual(TEXT("attested configuration"), Manifest.Attestation.Configuration,
		FString(TEXT("Shipping")));
	TestEqual(TEXT("attested launch module"), Manifest.Attestation.LaunchModule,
		FString(TEXT("WoTLaunch")));
	TestTrue(TEXT("listen server present"), Manifest.Attestation.bListenServerPresent);
	TestFalse(TEXT("dedicated server startup unavailable"),
		Manifest.Attestation.bDedicatedServerStartupAvailable);
	TestTrue(TEXT("this build supports the manifest's protocols"),
		SupportsManifestProtocols(Manifest));

	// A manifest that disagrees with the envelope is a substituted release.
	{
		FString Swapped = OrderManifestJson;
		Swapped.ReplaceInline(TEXT("\"release_sequence\":7"), TEXT("\"release_sequence\":8"));
		TWoTBootstrapResult<FWoTReleaseManifestV2> Mismatched = VerifyReleaseManifestV2(
			WoTOrderingSpecBytes(Swapped),
			WoTOrderingSpecSignature(OrderManifestSignature),
			Announcement.GetValue(), Ring.GetValue());
		// The edit breaks the signature first, which is the stronger rejection.
		TestTrue(TEXT("an edited manifest is refused"), Mismatched.IsError());
	}

	// A revoked or out-of-window key cannot sign, even with a valid signature.
	{
		FString Revoked = OrderRingJson;
		Revoked.ReplaceInline(TEXT("\"revoked_key_ids\":[]"),
			TEXT("\"revoked_key_ids\":[\"release-2026a\"]"));
		TWoTBootstrapResult<FWoTKeyRing> RevokedRing = WoTOrderingSpecRing(Revoked);
		if (TestFalse(TEXT("the revoking ring parses"), RevokedRing.IsError()))
		{
			TestTrue(TEXT("a revoked key cannot sign a manifest"),
				VerifyReleaseManifestV2(WoTOrderingSpecBytes(OrderManifestJson),
					WoTOrderingSpecSignature(OrderManifestSignature),
					Announcement.GetValue(), RevokedRing.GetValue()).IsError());
		}

		FString Narrow = OrderRingJson;
		Narrow.ReplaceInline(TEXT("\"last_sequence\":100"), TEXT("\"last_sequence\":6"));
		TWoTBootstrapResult<FWoTKeyRing> NarrowRing = WoTOrderingSpecRing(Narrow);
		if (TestFalse(TEXT("the narrowed ring parses"), NarrowRing.IsError()))
		{
			TestTrue(TEXT("a retired key cannot sign a later release"),
				VerifyReleaseManifestV2(WoTOrderingSpecBytes(OrderManifestJson),
					WoTOrderingSpecSignature(OrderManifestSignature),
					Announcement.GetValue(), NarrowRing.GetValue()).IsError());
		}
	}

	// Manifest v1 is a developer oracle and is never accepted in production.
	{
		FString V1 = OrderManifestJson;
		V1.ReplaceInline(TEXT("\"manifest_version\":2"), TEXT("\"manifest_version\":1"));
		TestTrue(TEXT("release-manifest-v1 is refused by the production parser"),
			VerifyReleaseManifestV2(WoTOrderingSpecBytes(V1),
				WoTOrderingSpecSignature(OrderManifestSignature),
				Announcement.GetValue(), Ring.GetValue()).IsError());
	}

	// A signed manifest is not permission to write outside the staging root.
	{
		FString Escape = OrderManifestJson;
		Escape.ReplaceInline(TEXT("\"name\":\"WoT-Mac-7.zip\""),
			TEXT("\"name\":\"../../escape.zip\""));
		TestTrue(TEXT("a traversing archive name is refused"),
			VerifyReleaseManifestV2(WoTOrderingSpecBytes(Escape),
				WoTOrderingSpecSignature(OrderManifestSignature),
				Announcement.GetValue(), Ring.GetValue()).IsError());
	}

	// Protocol support is a separate question from validity.
	{
		FWoTReleaseManifestV2 TooNew;
		TooNew.MinimumGateProtocol = WoT::Bootstrap::GateProtocol + 1;
		TestFalse(TEXT("a newer gate protocol is unsupported"),
			SupportsManifestProtocols(TooNew));

		FWoTReleaseManifestV2 NewerCache;
		NewerCache.CacheContractVersion = WoT::Bootstrap::CacheContractVersion + 1;
		TestFalse(TEXT("a newer cache contract is unsupported"),
			SupportsManifestProtocols(NewerCache));

		FWoTReleaseManifestV2 NewerUpdater;
		NewerUpdater.MinimumUpdaterProtocol = WoT::Bootstrap::UpdaterProtocol + 1;
		TestFalse(TEXT("a newer updater protocol is unsupported"),
			SupportsManifestProtocols(NewerUpdater));

		FWoTReleaseManifestV2 NewerService;
		NewerService.MinimumCommitServiceProtocol =
			WoT::Bootstrap::CommitServiceProtocol + 1;
		TestFalse(TEXT("a newer commit service protocol is unsupported"),
			SupportsManifestProtocols(NewerService));
	}

	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
