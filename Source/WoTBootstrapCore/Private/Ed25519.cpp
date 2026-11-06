#include "WoTBootstrapCore/Ed25519.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
/** The framing tag, including its terminating NUL: 14 bytes. */
const ANSICHAR SignatureTag[] = "WOT-SIGNATURE";
constexpr int32 SignatureTagBytes = 14;

static_assert(SignatureTagBytes + sizeof(uint16) + sizeof(uint64) == WoTSignatureFramePrefixBytes,
	"the framing prefix width is part of the signed bytes");

void AppendLittleEndian16(TArray<uint8>& Out, uint16 Value)
{
	Out.Add(static_cast<uint8>(Value & 0xFF));
	Out.Add(static_cast<uint8>((Value >> 8) & 0xFF));
}

void AppendLittleEndian64(TArray<uint8>& Out, uint64 Value)
{
	for (int32 Shift = 0; Shift < 64; Shift += 8)
	{
		Out.Add(static_cast<uint8>((Value >> Shift) & 0xFF));
	}
}
}  // namespace

const TCHAR* LexToString(EWoTSignatureContext Context)
{
	switch (Context)
	{
	case EWoTSignatureContext::AnnouncementV1:      return TEXT("AnnouncementV1");
	case EWoTSignatureContext::ComponentIdentityV1: return TEXT("ComponentIdentityV1");
	case EWoTSignatureContext::ReleaseKeyRingV1:    return TEXT("ReleaseKeyRingV1");
	case EWoTSignatureContext::FullPackageV1:       return TEXT("FullPackageV1");
	case EWoTSignatureContext::RecoveryDomainV1:    return TEXT("RecoveryDomainV1");
	case EWoTSignatureContext::ReleaseManifestV2:   return TEXT("ReleaseManifestV2");
	case EWoTSignatureContext::InstalledTreeV1:     return TEXT("InstalledTreeV1");
	case EWoTSignatureContext::CacheContractV3:     return TEXT("CacheContractV3");
	case EWoTSignatureContext::CacheLineageV1:      return TEXT("CacheLineageV1");
	}
	return TEXT("UnknownSignatureContext");
}

FString FWoTEd25519PublicKey::ToHex() const
{
	return WoT::Bootstrap::Detail::FixedBytesToHex(Bytes, ByteCount);
}

TWoTBootstrapResult<FWoTEd25519PublicKey> FWoTEd25519PublicKey::FromHex(FStringView Hex)
{
	FWoTEd25519PublicKey Key;
	if (FWoTBootstrapStatus Status =
			WoT::Bootstrap::Detail::FixedBytesFromHex(Hex, Key.Bytes, ByteCount);
		Status.IsError())
	{
		return TWoTBootstrapResult<FWoTEd25519PublicKey>::Error(MoveTemp(Status).GetError());
	}
	return TWoTBootstrapResult<FWoTEd25519PublicKey>::Success(Key);
}

bool FWoTEd25519PublicKey::operator==(const FWoTEd25519PublicKey& Other) const
{
	uint8 Difference = 0;
	for (int32 Index = 0; Index < ByteCount; ++Index)
	{
		Difference |= static_cast<uint8>(Bytes[Index] ^ Other.Bytes[Index]);
	}
	return Difference == 0;
}

FString FWoTEd25519Signature::ToHex() const
{
	return WoT::Bootstrap::Detail::FixedBytesToHex(Bytes, ByteCount);
}

TWoTBootstrapResult<FWoTEd25519Signature> FWoTEd25519Signature::FromHex(FStringView Hex)
{
	FWoTEd25519Signature Signature;
	if (FWoTBootstrapStatus Status =
			WoT::Bootstrap::Detail::FixedBytesFromHex(Hex, Signature.Bytes, ByteCount);
		Status.IsError())
	{
		return TWoTBootstrapResult<FWoTEd25519Signature>::Error(MoveTemp(Status).GetError());
	}
	return TWoTBootstrapResult<FWoTEd25519Signature>::Success(Signature);
}

bool FWoTEd25519Signature::operator==(const FWoTEd25519Signature& Other) const
{
	uint8 Difference = 0;
	for (int32 Index = 0; Index < ByteCount; ++Index)
	{
		Difference |= static_cast<uint8>(Bytes[Index] ^ Other.Bytes[Index]);
	}
	return Difference == 0;
}

TArray<uint8> FrameSignatureInput(EWoTSignatureContext Context,
	TConstArrayView<uint8> CanonicalBytes)
{
	TArray<uint8> Framed;
	Framed.Reserve(WoTSignatureFramePrefixBytes + CanonicalBytes.Num());

	// Tag including its NUL, so no artifact byte can ever be read as part of it.
	for (int32 Index = 0; Index < SignatureTagBytes; ++Index)
	{
		Framed.Add(static_cast<uint8>(SignatureTag[Index]));
	}
	AppendLittleEndian16(Framed, static_cast<uint16>(Context));
	AppendLittleEndian64(Framed, static_cast<uint64>(CanonicalBytes.Num()));
	Framed.Append(CanonicalBytes.GetData(), CanonicalBytes.Num());
	return Framed;
}

TWoTBootstrapResult<bool> WoTVerifyEd25519RawForTesting(const FWoTEd25519PublicKey& Key,
	TConstArrayView<uint8> Message, const FWoTEd25519Signature& Signature)
{
	using FResult = TWoTBootstrapResult<bool>;

	EVP_PKEY* const PublicKey = EVP_PKEY_new_raw_public_key(
		EVP_PKEY_ED25519, nullptr, Key.GetData(), static_cast<size_t>(Key.Num()));
	if (PublicKey == nullptr)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
			TEXT("OpenSSL rejected the raw Ed25519 public key")));
	}

	EVP_MD_CTX* const Context = EVP_MD_CTX_new();
	if (Context == nullptr)
	{
		EVP_PKEY_free(PublicKey);
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
			TEXT("OpenSSL could not create a digest context")));
	}

	// Ed25519 is a one-shot algorithm: no digest is supplied, and the whole message
	// is passed to EVP_DigestVerify in a single call.
	bool bMachineryFailed = false;
	bool bVerified = false;
	if (EVP_DigestVerifyInit(Context, nullptr, nullptr, nullptr, PublicKey) != 1)
	{
		bMachineryFailed = true;
	}
	else
	{
		const int32 Result = EVP_DigestVerify(Context, Signature.GetData(),
			static_cast<size_t>(Signature.Num()),
			Message.Num() > 0 ? Message.GetData() : reinterpret_cast<const uint8*>(""),
			static_cast<size_t>(Message.Num()));
		if (Result == 1)
		{
			bVerified = true;
		}
		else if (Result != 0)
		{
			// Negative means the question could not be asked. A caller must never
			// treat that as a clean "no".
			bMachineryFailed = true;
		}
	}

	EVP_MD_CTX_free(Context);
	EVP_PKEY_free(PublicKey);

	if (bMachineryFailed)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
			TEXT("OpenSSL could not complete Ed25519 verification")));
	}
	return FResult::Success(bVerified);
}

TWoTBootstrapResult<bool> VerifyEd25519(const FWoTEd25519PublicKey& Key,
	EWoTSignatureContext Context, TConstArrayView<uint8> CanonicalBytes,
	const FWoTEd25519Signature& Signature)
{
	const TArray<uint8> Framed = FrameSignatureInput(Context, CanonicalBytes);
	return WoTVerifyEd25519RawForTesting(Key, Framed, Signature);
}
