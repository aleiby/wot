// Specs for hashing, domain-separated Ed25519 verification, and key rings.
//
// Provenance of the fixtures, because it decides what a failure means:
//
//   * The Ed25519 vectors are RFC 8032 section 7.1 TEST 1/2/3 — external truth,
//     not something this code produced. All three public keys were independently
//     re-derived from their seeds with OpenSSL 3.0.13, and the TEST 2/TEST 3
//     signatures were independently re-produced byte for byte. TEST 1's signature
//     could not be re-produced externally (the openssl CLI refuses to sign a
//     zero-length message), so if TEST 1 alone ever fails while 2 and 3 pass,
//     suspect the transcribed vector before the code.
//   * The synthetic ring/manifest signatures were produced by OpenSSL over the
//     framing this module defines. Only public keys, messages, and signatures are
//     stored: no private key exists in the repository, and this module cannot sign.
#include "Misc/AutomationTest.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Ed25519.h"
#include "WoTBootstrapCore/Generated/BootstrapTrust.generated.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/KeyRing.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTCryptoSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

// RFC 8032 section 7.1.
const TCHAR* const RfcPublicKey1 = TEXT("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
const TCHAR* const RfcSignature1 = TEXT("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
const TCHAR* const RfcPublicKey2 = TEXT("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
const TCHAR* const RfcSignature2 = TEXT("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
const TCHAR* const RfcPublicKey3 = TEXT("fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025");
const TCHAR* const RfcSignature3 = TEXT("6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a");

// Synthetic, verify-only fixtures over this module's own framing.
const TCHAR* const SynthRootPublicKey = TEXT("66117dc0c9c70f2c230832ee1b1a09576b26453596decf52707f5c432bd566a2");
const TCHAR* const SynthKeyAPublicKey = TEXT("c3b9d1a779db1bf4da0e21c1967a8ef4f6dbf15bc0cb497f9c05de7cf22aad04");
const TCHAR* const SynthKeyBPublicKey = TEXT("be90d1d8ffad3ed9f7b32084fafd1970e048835bfad47749f6f98d4eb917601e");
const TCHAR* const SynthRingJson = TEXT("{\"epoch\":7,\"format\":\"wot-release-key-ring-v1\",\"keys\":[{\"first_sequence\":1,\"key_id\":\"release-2026a\",\"last_sequence\":100,\"public_key\":\"c3b9d1a779db1bf4da0e21c1967a8ef4f6dbf15bc0cb497f9c05de7cf22aad04\"},{\"first_sequence\":101,\"key_id\":\"release-2026b\",\"last_sequence\":200,\"public_key\":\"be90d1d8ffad3ed9f7b32084fafd1970e048835bfad47749f6f98d4eb917601e\"}],\"revoked_key_ids\":[\"release-2025z\"]}");
const TCHAR* const SynthRingSignature = TEXT("709c67053bd38ab2c742e6b3a8d9973ed5b6030cc02471c0836795a9cf6f884647df0d2ccc8f5a2233286d2e8e3fadf9c4defa355f515ec9af8e503ceba81308");
const TCHAR* const SynthManifestJson = TEXT("{\"format\":\"wot-release-manifest-v2\",\"release_sequence\":42}");
const TCHAR* const SynthManifestSignature = TEXT("8842ec6dac0584473cd42b992dfff24c7d6e0fe6ea28c03e490a51c29878a681d5888e709ecfe8b4092e5eccfc89e2a72d63ed01cc49d58160adaeb11ab8d905");
// The same manifest bytes signed under AnnouncementV1 instead of ReleaseManifestV2.
const TCHAR* const SynthWrongDomainSignature = TEXT("d5ea3b010ccc532ed952678333b7b6f17a907fd4b5cdcac89a707b78ca6028e2d6881136d6cf950f22f68815ee90944d038ae2c05d30f70bbbd7da884da26803");

TArray<uint8> WoTCryptoSpecAsciiBytes(FStringView Ascii)
{
	TArray<uint8> Bytes;
	Bytes.Reserve(Ascii.Len());
	for (const TCHAR Character : Ascii)
	{
		Bytes.Add(static_cast<uint8>(Character));
	}
	return Bytes;
}

TArray<uint8> WoTCryptoSpecHexBytes(FStringView Hex)
{
	TArray<uint8> Bytes;
	Bytes.Reserve(Hex.Len() / 2);
	const auto Nibble = [](TCHAR Character) -> uint8
	{
		if (Character >= TEXT('0') && Character <= TEXT('9')) return static_cast<uint8>(Character - TEXT('0'));
		return static_cast<uint8>(10 + (Character - TEXT('a')));
	};
	for (int32 Index = 0; Index + 1 < Hex.Len(); Index += 2)
	{
		Bytes.Add(static_cast<uint8>((Nibble(Hex[Index]) << 4) | Nibble(Hex[Index + 1])));
	}
	return Bytes;
}

FWoTEd25519PublicKey WoTCryptoSpecKey(FAutomationTestBase& Test, const TCHAR* Hex)
{
	TWoTBootstrapResult<FWoTEd25519PublicKey> Parsed = FWoTEd25519PublicKey::FromHex(Hex);
	if (Parsed.IsError())
	{
		Test.AddError(FString::Printf(TEXT("fixture key %s: %s"), Hex,
			*Parsed.GetError().ToString()));
		return FWoTEd25519PublicKey();
	}
	return Parsed.GetValue();
}

FWoTEd25519Signature WoTCryptoSpecSignature(FAutomationTestBase& Test, const TCHAR* Hex)
{
	TWoTBootstrapResult<FWoTEd25519Signature> Parsed = FWoTEd25519Signature::FromHex(Hex);
	if (Parsed.IsError())
	{
		Test.AddError(FString::Printf(TEXT("fixture signature: %s"),
			*Parsed.GetError().ToString()));
		return FWoTEd25519Signature();
	}
	return Parsed.GetValue();
}

/** Verify a raw RFC vector by framing nothing: the vector message IS the input. */
bool WoTCryptoSpecVerifyFramed(FAutomationTestBase& Test, const TCHAR* KeyHex,
	EWoTSignatureContext Context, TConstArrayView<uint8> Payload, const TCHAR* SignatureHex)
{
	const FWoTEd25519PublicKey Key = WoTCryptoSpecKey(Test, KeyHex);
	const FWoTEd25519Signature Signature = WoTCryptoSpecSignature(Test, SignatureHex);
	TWoTBootstrapResult<bool> Verified = VerifyEd25519(Key, Context, Payload, Signature);
	if (Verified.IsError())
	{
		Test.AddError(FString::Printf(TEXT("verification could not run: %s"),
			*Verified.GetError().ToString()));
		return false;
	}
	return Verified.GetValue();
}

TWoTBootstrapResult<FWoTKeyRing> WoTCryptoSpecParseRing(FStringView Json,
	FStringView ExpectedFormat = WoTReleaseKeyRingFormat)
{
	const TArray<uint8> Bytes = WoTCryptoSpecAsciiBytes(Json);
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Document = ParseCanonicalJson(Bytes);
	if (Document.IsError())
	{
		return TWoTBootstrapResult<FWoTKeyRing>::Error(MoveTemp(Document).GetError());
	}
	return ParseKeyRing(Document.GetValue(), ExpectedFormat);
}

// A synthetic RELEASE ring signed by the PRODUCTION offline root key
// `offline-root-a`. The release key inside is made up: the point is that the root
// from the real ceremony can vouch for a ring, which is the one thing a public-only
// export cannot demonstrate on its own.
const TCHAR* const ProductionSignedReleaseRingJson = TEXT("{\"epoch\":1,\"format\":\"wot-release-key-ring-v1\",\"keys\":[{\"first_sequence\":1,\"key_id\":\"release-synthetic\",\"last_sequence\":1000,\"public_key\":\"0000000000000000000000000000000000000000000000000000000000000001\"}],\"revoked_key_ids\":[]}");
const TCHAR* const ProductionRootASignature = TEXT("a8cddc4a3983c5bb5f2dc7d957938d746fd15c602b2f9e078512d1cfb7fa3e8cf43e86a9a1007a8c9dba9b542abe52c10fbb07dd0d0d42799bcb6873c8118b02");
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCryptoHashSpec,
	"WoT.BootstrapCore.Crypto.Hash", WoTCryptoSpecFlags)

bool FWoTCryptoHashSpec::RunTest(const FString&)
{
	// Published SHA-256 values, re-derived with OpenSSL 3.0.13.
	TestEqual(TEXT("empty input"), Sha256(TArray<uint8>()).ToHex(),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	TestEqual(TEXT("\"abc\""), Sha256(WoTCryptoSpecAsciiBytes(TEXT("abc"))).ToHex(),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));

	// Streaming in arbitrary pieces must equal the one-shot digest: the installer
	// and release archives are hashed in chunks and must not depend on chunking.
	{
		const TArray<uint8> Whole = WoTCryptoSpecAsciiBytes(
			TEXT("the wheel weaves as the wheel wills"));
		for (const int32 ChunkSize : {1, 2, 7, 16, 1024})
		{
			FWoTSha256Hasher Hasher;
			for (int32 Offset = 0; Offset < Whole.Num(); Offset += ChunkSize)
			{
				const int32 Count = FMath::Min(ChunkSize, Whole.Num() - Offset);
				Hasher.Update(TConstArrayView<uint8>(Whole.GetData() + Offset, Count));
			}
			TestEqual(FString::Printf(TEXT("streaming in %d-byte chunks"), ChunkSize),
				Hasher.Finish().ToHex(), Sha256(Whole).ToHex());
		}
	}

	// Hex round trip, and the canonicality rules for hex in artifacts.
	{
		const FWoTSha256 Digest = Sha256(WoTCryptoSpecAsciiBytes(TEXT("abc")));
		TWoTBootstrapResult<FWoTSha256> Parsed = FWoTSha256::FromHex(Digest.ToHex());
		if (TestFalse(TEXT("a digest round trips through hex"), Parsed.IsError()))
		{
			TestTrue(TEXT("round trip is byte-identical"), Parsed.GetValue() == Digest);
		}
		TestTrue(TEXT("upper-case hex is refused"),
			FWoTSha256::FromHex(TEXT("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD")).IsError());
		TestTrue(TEXT("a short digest is refused"),
			FWoTSha256::FromHex(TEXT("ba7816bf")).IsError());
		TestTrue(TEXT("an over-long digest is refused"),
			FWoTSha256::FromHex(FString(Digest.ToHex()) + TEXT("00")).IsError());
		TestTrue(TEXT("non-hex characters are refused"),
			FWoTSha256::FromHex(TEXT("zzzz816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")).IsError());
	}

	// A one-bit change must change the digest.
	{
		TArray<uint8> Payload = WoTCryptoSpecAsciiBytes(TEXT("abc"));
		const FWoTSha256 Before = Sha256(Payload);
		Payload[0] ^= 0x01;
		TestTrue(TEXT("a flipped bit changes the digest"), Sha256(Payload) != Before);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCryptoFramingSpec,
	"WoT.BootstrapCore.Crypto.Framing", WoTCryptoSpecFlags)

bool FWoTCryptoFramingSpec::RunTest(const FString&)
{
	// The framing is signed, so its exact bytes are pinned here rather than merely
	// described: the Python release signer must reproduce this layout.
	const TArray<uint8> Payload = WoTCryptoSpecAsciiBytes(TEXT("{}"));
	const TArray<uint8> Framed = FrameSignatureInput(
		EWoTSignatureContext::ReleaseManifestV2, Payload);

	TestEqual(TEXT("framed length is prefix plus payload"), Framed.Num(),
		WoTSignatureFramePrefixBytes + Payload.Num());

	const TArray<uint8> ExpectedTag = WoTCryptoSpecAsciiBytes(TEXT("WOT-SIGNATURE"));
	for (int32 Index = 0; Index < ExpectedTag.Num(); ++Index)
	{
		TestEqual(FString::Printf(TEXT("tag byte %d"), Index),
			static_cast<int32>(Framed[Index]), static_cast<int32>(ExpectedTag[Index]));
	}
	TestEqual(TEXT("tag is NUL terminated"), static_cast<int32>(Framed[13]), 0);

	// uint16 little-endian context: ReleaseManifestV2 is ordinal 5.
	TestEqual(TEXT("context low byte"), static_cast<int32>(Framed[14]), 5);
	TestEqual(TEXT("context high byte"), static_cast<int32>(Framed[15]), 0);

	// uint64 little-endian canonical size.
	TestEqual(TEXT("size low byte"), static_cast<int32>(Framed[16]), Payload.Num());
	for (int32 Index = 17; Index < 24; ++Index)
	{
		TestEqual(FString::Printf(TEXT("size byte %d is zero"), Index),
			static_cast<int32>(Framed[Index]), 0);
	}
	TestEqual(TEXT("payload follows the prefix"),
		static_cast<int32>(Framed[24]), static_cast<int32>('{'));

	// Every context produces a distinct framed input for identical payload bytes —
	// that is the whole mechanism preventing cross-domain signature replay.
	{
		const EWoTSignatureContext Contexts[] = {
			EWoTSignatureContext::AnnouncementV1, EWoTSignatureContext::ComponentIdentityV1,
			EWoTSignatureContext::ReleaseKeyRingV1, EWoTSignatureContext::FullPackageV1,
			EWoTSignatureContext::RecoveryDomainV1, EWoTSignatureContext::ReleaseManifestV2,
			EWoTSignatureContext::InstalledTreeV1, EWoTSignatureContext::CacheContractV3,
			EWoTSignatureContext::CacheLineageV1,
		};
		TSet<FString> Distinct;
		TSet<FString> Names;
		for (const EWoTSignatureContext Context : Contexts)
		{
			const TArray<uint8> Bytes = FrameSignatureInput(Context, Payload);
			Distinct.Add(BytesToHex(Bytes.GetData(), Bytes.Num()));
			Names.Add(LexToString(Context));
		}
		TestEqual(TEXT("all nine contexts frame differently"), Distinct.Num(), 9);
		TestEqual(TEXT("all nine contexts are named uniquely"), Names.Num(), 9);
	}

	// Length is framed explicitly, so two payloads cannot be spliced into one input.
	{
		const TArray<uint8> Split = FrameSignatureInput(
			EWoTSignatureContext::AnnouncementV1, WoTCryptoSpecAsciiBytes(TEXT("ab")));
		const TArray<uint8> Joined = FrameSignatureInput(
			EWoTSignatureContext::AnnouncementV1, WoTCryptoSpecAsciiBytes(TEXT("a")));
		TestTrue(TEXT("payload length changes the framed input"),
			Split.Num() != Joined.Num());
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCryptoEd25519Spec,
	"WoT.BootstrapCore.Crypto.Ed25519", WoTCryptoSpecFlags)

bool FWoTCryptoEd25519Spec::RunTest(const FString&)
{
	// RFC 8032 vectors sign the raw message. Passing the vector message as the
	// "canonical bytes" of a context would frame it, so these run through the raw
	// path exposed for exactly this purpose.
	struct FVector
	{
		const TCHAR* Key;
		const TCHAR* Message;
		const TCHAR* Signature;
	};
	const FVector Vectors[] = {
		{RfcPublicKey1, TEXT(""),     RfcSignature1},
		{RfcPublicKey2, TEXT("72"),   RfcSignature2},
		{RfcPublicKey3, TEXT("af82"), RfcSignature3},
	};

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Vectors); ++Index)
	{
		const FVector& Vector = Vectors[Index];
		const FWoTEd25519PublicKey Key = WoTCryptoSpecKey(*this, Vector.Key);
		const FWoTEd25519Signature Signature = WoTCryptoSpecSignature(*this, Vector.Signature);
		const TArray<uint8> Message = WoTCryptoSpecHexBytes(Vector.Message);
		TWoTBootstrapResult<bool> Verified =
			WoTVerifyEd25519RawForTesting(Key, Message, Signature);
		if (Verified.IsError())
		{
			AddError(FString::Printf(TEXT("RFC 8032 TEST %d could not run: %s"),
				Index + 1, *Verified.GetError().ToString()));
			continue;
		}
		TestTrue(FString::Printf(TEXT("RFC 8032 TEST %d verifies"), Index + 1),
			Verified.GetValue());
	}

	// A flipped bit anywhere in the signature must fail — cleanly, not as an error.
	{
		const FWoTEd25519PublicKey Key = WoTCryptoSpecKey(*this, RfcPublicKey3);
		FWoTEd25519Signature Signature = WoTCryptoSpecSignature(*this, RfcSignature3);
		const TArray<uint8> Message = WoTCryptoSpecHexBytes(TEXT("af82"));
		for (const int32 ByteIndex : {0, 31, 32, 63})
		{
			FWoTEd25519Signature Tampered = Signature;
			Tampered.Bytes[ByteIndex] ^= 0x01;
			TWoTBootstrapResult<bool> Verified =
				WoTVerifyEd25519RawForTesting(Key, Message, Tampered);
			if (TestFalse(FString::Printf(TEXT("tampered signature byte %d does not error"),
					ByteIndex), Verified.IsError()))
			{
				TestFalse(FString::Printf(TEXT("tampered signature byte %d is rejected"),
					ByteIndex), Verified.GetValue());
			}
		}

		// A flipped message bit must fail too.
		TArray<uint8> Tampered = Message;
		Tampered[0] ^= 0x01;
		TWoTBootstrapResult<bool> Verified =
			WoTVerifyEd25519RawForTesting(Key, Tampered, Signature);
		if (TestFalse(TEXT("tampered message does not error"), Verified.IsError()))
		{
			TestFalse(TEXT("tampered message is rejected"), Verified.GetValue());
		}

		// The wrong key must fail.
		const FWoTEd25519PublicKey Other = WoTCryptoSpecKey(*this, RfcPublicKey2);
		TWoTBootstrapResult<bool> WrongKey =
			WoTVerifyEd25519RawForTesting(Other, Message, Signature);
		if (TestFalse(TEXT("wrong key does not error"), WrongKey.IsError()))
		{
			TestFalse(TEXT("wrong key is rejected"), WrongKey.GetValue());
		}
	}

	// Domain separation, over this module's framing.
	{
		const TArray<uint8> Manifest = WoTCryptoSpecAsciiBytes(SynthManifestJson);
		TestTrue(TEXT("a correctly framed signature verifies"),
			WoTCryptoSpecVerifyFramed(*this, SynthKeyAPublicKey,
				EWoTSignatureContext::ReleaseManifestV2, Manifest, SynthManifestSignature));

		// Same key, same bytes, same signature algorithm — different domain.
		TestFalse(TEXT("a signature framed for another domain is rejected"),
			WoTCryptoSpecVerifyFramed(*this, SynthKeyAPublicKey,
				EWoTSignatureContext::ReleaseManifestV2, Manifest, SynthWrongDomainSignature));

		// And the converse: the announcement-framed signature verifies as an
		// announcement, proving the fixture is a real signature and the rejection
		// above came from the domain rather than from a corrupt fixture.
		TestTrue(TEXT("the other-domain fixture is itself a valid signature"),
			WoTCryptoSpecVerifyFramed(*this, SynthKeyAPublicKey,
				EWoTSignatureContext::AnnouncementV1, Manifest, SynthWrongDomainSignature));

		// A different key in the same ring must not verify.
		TestFalse(TEXT("another release key is rejected"),
			WoTCryptoSpecVerifyFramed(*this, SynthKeyBPublicKey,
				EWoTSignatureContext::ReleaseManifestV2, Manifest, SynthManifestSignature));

		// One byte of payload changes the framed input.
		TArray<uint8> Tampered = Manifest;
		Tampered[Tampered.Num() - 2] = static_cast<uint8>('3');
		TestFalse(TEXT("tampered canonical bytes are rejected"),
			WoTCryptoSpecVerifyFramed(*this, SynthKeyAPublicKey,
				EWoTSignatureContext::ReleaseManifestV2, Tampered, SynthManifestSignature));
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCryptoKeyRingSpec,
	"WoT.BootstrapCore.Crypto.KeyRing", WoTCryptoSpecFlags)

bool FWoTCryptoKeyRingSpec::RunTest(const FString&)
{
	TWoTBootstrapResult<FWoTKeyRing> Parsed = WoTCryptoSpecParseRing(SynthRingJson);
	if (!TestFalse(TEXT("the synthetic ring parses"), Parsed.IsError()))
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	const FWoTKeyRing& Ring = Parsed.GetValue();
	TestEqual(TEXT("format"), Ring.Format, FString(WoTReleaseKeyRingFormat));
	TestEqual(TEXT("epoch"), static_cast<int64>(Ring.Epoch), static_cast<int64>(7));
	TestEqual(TEXT("key count"), Ring.Keys.Num(), 2);
	TestEqual(TEXT("revoked count"), Ring.RevokedKeyIds.Num(), 1);

	// The ring itself is only trusted once a root signature over it verifies.
	{
		const TArray<uint8> RingBytes = WoTCryptoSpecAsciiBytes(SynthRingJson);
		TestTrue(TEXT("the root signature over the ring verifies"),
			WoTCryptoSpecVerifyFramed(*this, SynthRootPublicKey,
				EWoTSignatureContext::ReleaseKeyRingV1, RingBytes, SynthRingSignature));
		// A release key must not be able to vouch for the ring that contains it.
		TestFalse(TEXT("a release key cannot sign the ring"),
			WoTCryptoSpecVerifyFramed(*this, SynthKeyAPublicKey,
				EWoTSignatureContext::ReleaseKeyRingV1, RingBytes, SynthRingSignature));
	}

	// Sequence windows.
	{
		TestFalse(TEXT("the first key covers its own range"),
			SelectReleaseKey(Ring, TEXT("release-2026a"), 1).IsError());
		TestFalse(TEXT("inclusive upper bound"),
			SelectReleaseKey(Ring, TEXT("release-2026a"), 100).IsError());
		TestTrue(TEXT("one past the upper bound is refused"),
			SelectReleaseKey(Ring, TEXT("release-2026a"), 101).IsError());
		TestTrue(TEXT("below the lower bound is refused"),
			SelectReleaseKey(Ring, TEXT("release-2026a"), 0).IsError());
		TestFalse(TEXT("the successor key covers the next range"),
			SelectReleaseKey(Ring, TEXT("release-2026b"), 101).IsError());
		TestTrue(TEXT("an unknown key id is refused"),
			SelectReleaseKey(Ring, TEXT("release-nope"), 1).IsError());
		TestTrue(TEXT("a revoked key id is refused"),
			SelectReleaseKey(Ring, TEXT("release-2025z"), 1).IsError());

		TWoTBootstrapResult<FWoTReleaseKey> Selected =
			SelectReleaseKey(Ring, TEXT("release-2026a"), 42);
		if (TestFalse(TEXT("selection succeeds"), Selected.IsError()))
		{
			TestEqual(TEXT("selection returns the right key bytes"),
				Selected.GetValue().PublicKey.ToHex(), FString(SynthKeyAPublicKey));
		}
	}

	// A revoked id still listed among the keys must not be selectable: revocation
	// wins over presence, or revoking a key would require rewriting the ring.
	{
		const FString Revoked = FString(SynthRingJson).Replace(
			TEXT("\"revoked_key_ids\":[\"release-2025z\"]"),
			TEXT("\"revoked_key_ids\":[\"release-2026a\"]"));
		TWoTBootstrapResult<FWoTKeyRing> WithRevocation = WoTCryptoSpecParseRing(Revoked);
		if (TestFalse(TEXT("a ring revoking a listed key parses"), WithRevocation.IsError()))
		{
			TestTrue(TEXT("revocation beats presence"),
				SelectReleaseKey(WithRevocation.GetValue(), TEXT("release-2026a"), 1).IsError());
		}
	}

	// Epoch policy: equal is acceptable, older is a rollback.
	{
		TestFalse(TEXT("the current epoch is accepted"),
			RequireRingEpochAtLeast(Ring, 7).IsError());
		TestFalse(TEXT("a newer epoch is accepted"),
			RequireRingEpochAtLeast(Ring, 6).IsError());
		FWoTBootstrapStatus Stale = RequireRingEpochAtLeast(Ring, 8);
		if (TestTrue(TEXT("a stale epoch is refused"), Stale.IsError()))
		{
			TestEqual(TEXT("a stale epoch reports Rollback"),
				Stale.GetError().Code, EWoTBootstrapError::Rollback);
		}
	}

	// Closed schema and structural rules.
	{
		struct FCase
		{
			const TCHAR* Json;
			EWoTBootstrapError Expected;
			const TCHAR* Why;
		};
		const FCase Cases[] = {
			{TEXT("{\"epoch\":1,\"format\":\"wot-release-key-ring-v1\",\"keys\":[],\"revoked_key_ids\":[],\"surprise\":1}"),
			 EWoTBootstrapError::UnknownField, TEXT("an unknown ring field")},
			{TEXT("{\"epoch\":1,\"format\":\"wot-release-key-ring-v1\",\"keys\":[{\"first_sequence\":1,\"key_id\":\"k\",\"last_sequence\":2,\"public_key\":\"c3b9d1a779db1bf4da0e21c1967a8ef4f6dbf15bc0cb497f9c05de7cf22aad04\",\"surprise\":1}],\"revoked_key_ids\":[]}"),
			 EWoTBootstrapError::UnknownField, TEXT("an unknown key field")},
			{TEXT("{\"epoch\":1,\"format\":\"wot-release-key-ring-v1\",\"keys\":[]}"),
			 EWoTBootstrapError::UnknownField, TEXT("a missing required field")},
			{TEXT("{\"epoch\":1,\"format\":\"wot-release-key-ring-v2\",\"keys\":[],\"revoked_key_ids\":[]}"),
			 EWoTBootstrapError::UnsupportedProtocol, TEXT("an unknown format")},
			{TEXT("{\"epoch\":1,\"format\":\"wot-release-key-ring-v1\",\"keys\":[{\"first_sequence\":1,\"key_id\":\"dup\",\"last_sequence\":2,\"public_key\":\"c3b9d1a779db1bf4da0e21c1967a8ef4f6dbf15bc0cb497f9c05de7cf22aad04\"},{\"first_sequence\":3,\"key_id\":\"dup\",\"last_sequence\":4,\"public_key\":\"be90d1d8ffad3ed9f7b32084fafd1970e048835bfad47749f6f98d4eb917601e\"}],\"revoked_key_ids\":[]}"),
			 EWoTBootstrapError::DuplicateField, TEXT("a duplicate key id")},
			{TEXT("{\"epoch\":1,\"format\":\"wot-release-key-ring-v1\",\"keys\":[{\"first_sequence\":1,\"key_id\":\"k\",\"last_sequence\":2,\"public_key\":\"c3b9\"}],\"revoked_key_ids\":[]}"),
			 EWoTBootstrapError::Crypto, TEXT("a short public key")},
			{TEXT("{\"epoch\":1,\"format\":\"wot-release-key-ring-v1\",\"keys\":[{\"first_sequence\":5,\"key_id\":\"k\",\"last_sequence\":2,\"public_key\":\"c3b9d1a779db1bf4da0e21c1967a8ef4f6dbf15bc0cb497f9c05de7cf22aad04\"}],\"revoked_key_ids\":[]}"),
			 EWoTBootstrapError::Crypto, TEXT("an inverted sequence range")},
			{TEXT("{\"epoch\":1,\"format\":\"wot-release-key-ring-v1\",\"keys\":[{\"first_sequence\":-1,\"key_id\":\"k\",\"last_sequence\":2,\"public_key\":\"c3b9d1a779db1bf4da0e21c1967a8ef4f6dbf15bc0cb497f9c05de7cf22aad04\"}],\"revoked_key_ids\":[]}"),
			 EWoTBootstrapError::InvalidNumber, TEXT("a negative sequence")},
			{TEXT("{\"epoch\":1,\"format\":\"wot-release-key-ring-v1\",\"keys\":[{\"first_sequence\":1,\"key_id\":\"\",\"last_sequence\":2,\"public_key\":\"c3b9d1a779db1bf4da0e21c1967a8ef4f6dbf15bc0cb497f9c05de7cf22aad04\"}],\"revoked_key_ids\":[]}"),
			 EWoTBootstrapError::Crypto, TEXT("an empty key id")},
			{TEXT("{\"epoch\":1,\"format\":\"wot-release-key-ring-v1\",\"keys\":{},\"revoked_key_ids\":[]}"),
			 EWoTBootstrapError::UnknownField, TEXT("keys that are not an array")},
		};

		for (const FCase& Case : Cases)
		{
			TWoTBootstrapResult<FWoTKeyRing> Rejected = WoTCryptoSpecParseRing(Case.Json);
			if (!Rejected.IsError())
			{
				AddError(FString::Printf(TEXT("%s was accepted"), Case.Why));
				continue;
			}
			TestEqual(FString::Printf(TEXT("%s is rejected with the right code"), Case.Why),
				Rejected.GetError().Code, Case.Expected);
		}
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCryptoGeneratedTrustSpec,
	"WoT.BootstrapCore.Crypto.GeneratedTrust", WoTCryptoSpecFlags)

bool FWoTCryptoGeneratedTrustSpec::RunTest(const FString&)
{
	using namespace WoT::Bootstrap::Trust;

	// This is the platform probe. It runs in a binary compiled for exactly one
	// platform, so it proves what that link actually contains — not what the header
	// source happens to say. Running it on Mac and on Windows covers both branches.
	{
		const FString Url = AnnouncementUrl;
#if PLATFORM_WINDOWS
		const FString Expected = TEXT("https://github.com/aleiby/wot/releases/latest/download/WoT-Win64-announcement-v1.json");
		const FString Opposite = TEXT("https://github.com/aleiby/wot/releases/latest/download/WoT-Mac-announcement-v1.json");
#elif PLATFORM_MAC
		const FString Expected = TEXT("https://github.com/aleiby/wot/releases/latest/download/WoT-Mac-announcement-v1.json");
		const FString Opposite = TEXT("https://github.com/aleiby/wot/releases/latest/download/WoT-Win64-announcement-v1.json");
#else
#error "the generated trust probe is defined for Win64 and Mac only"
#endif
		TestEqual(TEXT("this platform selects its own announcement URL"), Url, Expected);
		TestNotEqual(TEXT("the opposite platform URL is not what was selected"),
			Url, Opposite);
		TestTrue(TEXT("the announcement URL is HTTPS"),
			Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive));

		// The recovery pointer is a separate, explicitly-invoked constant and must
		// never be confusable with an announcement source.
		const FString Recovery = RootKeyRingRecoveryUrl;
		TestEqual(TEXT("the recovery URL is the fixed constant"), Recovery,
			FString(TEXT("https://github.com/aleiby/wot/releases/latest/download/WoT-root-key-ring-v1.json")));
		TestNotEqual(TEXT("recovery is not the announcement object"), Recovery, Url);
	}

	// The embedded root ring must be usable with no file on disk: that is the whole
	// reason the bytes are compiled in rather than referenced by path.
	const TConstArrayView<uint8> EmbeddedRing(
		OfflineRootKeyRingCanonicalBytes, OfflineRootKeyRingSize);
	TestEqual(TEXT("the embedded ring matches its recorded size"),
		static_cast<int32>(sizeof(OfflineRootKeyRingCanonicalBytes)),
		OfflineRootKeyRingSize);
	TestEqual(TEXT("the embedded ring matches its recorded digest"),
		Sha256(EmbeddedRing).ToHex(), FString(OfflineRootKeyRingSha256Hex));

	TWoTBootstrapResult<FWoTCanonicalJsonValue> Document = ParseCanonicalJson(EmbeddedRing);
	if (!TestFalse(TEXT("the embedded ring is canonical JSON"), Document.IsError()))
	{
		AddError(Document.GetError().ToString());
		return false;
	}

	TWoTBootstrapResult<FWoTKeyRing> RootRing =
		ParseKeyRing(Document.GetValue(), WoTOfflineRootRingFormat);
	if (!TestFalse(TEXT("the embedded ring parses as an offline-root ring"),
			RootRing.IsError()))
	{
		AddError(RootRing.GetError().ToString());
		return false;
	}
	const FWoTKeyRing& Roots = RootRing.GetValue();
	TestTrue(TEXT("the root ring holds more than one key, so losing one is survivable"),
		Roots.Keys.Num() >= 2);

	// The same bytes must NOT be accepted where a release ring is expected.
	TestTrue(TEXT("a root ring is refused as a release ring"),
		ParseKeyRing(Document.GetValue(), WoTReleaseKeyRingFormat).IsError());

	// Role separation, re-checked in the binary rather than only in the generator.
	{
		TWoTBootstrapResult<FWoTEd25519PublicKey> Announcement =
			FWoTEd25519PublicKey::FromHex(AnnouncementPublicKeyHex);
		if (TestFalse(TEXT("the embedded announcement key is well formed"),
				Announcement.IsError()))
		{
			for (const FWoTReleaseKey& Root : Roots.Keys)
			{
				TestTrue(FString::Printf(
					TEXT("the announcement key differs from root '%s'"), *Root.KeyId),
					Root.PublicKey != Announcement.GetValue());
			}
		}
	}

	// The production root can actually vouch for a release ring. A public-only
	// export cannot show this; a signature made by the offline private half can.
	{
		TWoTBootstrapResult<FWoTReleaseKey> RootA =
			SelectReleaseKey(Roots, TEXT("offline-root-a"), 1);
		if (TestFalse(TEXT("offline-root-a is selectable from the embedded ring"),
				RootA.IsError()))
		{
			const TArray<uint8> SignedRing =
				WoTCryptoSpecAsciiBytes(ProductionSignedReleaseRingJson);
			const FWoTEd25519Signature Signature =
				WoTCryptoSpecSignature(*this, ProductionRootASignature);

			TWoTBootstrapResult<bool> Verified = VerifyEd25519(RootA.GetValue().PublicKey,
				EWoTSignatureContext::ReleaseKeyRingV1, SignedRing, Signature);
			if (TestFalse(TEXT("root verification runs"), Verified.IsError()))
			{
				TestTrue(TEXT("the production root's signature over a release ring verifies"),
					Verified.GetValue());
			}

			// Domain separation holds for the production key too.
			TWoTBootstrapResult<bool> WrongDomain = VerifyEd25519(RootA.GetValue().PublicKey,
				EWoTSignatureContext::ReleaseManifestV2, SignedRing, Signature);
			if (TestFalse(TEXT("cross-domain check runs"), WrongDomain.IsError()))
			{
				TestFalse(TEXT("the root signature does not verify in another domain"),
					WrongDomain.GetValue());
			}

			// And the other root must not verify a signature it did not make.
			TWoTBootstrapResult<FWoTReleaseKey> RootB =
				SelectReleaseKey(Roots, TEXT("offline-root-b"), 1);
			if (TestFalse(TEXT("offline-root-b is selectable"), RootB.IsError()))
			{
				TWoTBootstrapResult<bool> Other = VerifyEd25519(RootB.GetValue().PublicKey,
					EWoTSignatureContext::ReleaseKeyRingV1, SignedRing, Signature);
				if (TestFalse(TEXT("second-root check runs"), Other.IsError()))
				{
					TestFalse(TEXT("the backup root did not sign this ring"),
						Other.GetValue());
				}
			}

			// The signed ring parses as a release ring, so the signature covers a
			// document this build can actually consume.
			TestFalse(TEXT("the root-signed ring is a usable release ring"),
				WoTCryptoSpecParseRing(ProductionSignedReleaseRingJson).IsError());
		}
	}

	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
