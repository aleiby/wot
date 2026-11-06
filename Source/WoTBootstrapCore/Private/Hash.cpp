#include "WoTBootstrapCore/Hash.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif

#if WITH_AUTOMATION_TESTS
namespace
{
// Intentional process-lifetime storage: a static or cross-TU token may drain
// during teardown, after ordinary namespace statics would have been destroyed.
FCriticalSection& GWoTSha256ObserverGuard = *new FCriticalSection();
TArray<TSharedPtr<TFunction<void(TConstArrayView<uint8>)>, ESPMode::ThreadSafe>>
	& GWoTSha256Observers =
		*new TArray<TSharedPtr<TFunction<void(TConstArrayView<uint8>)>,
			ESPMode::ThreadSafe>>();
TSet<const void*>& GWoTActiveSha256Observers = *new TSet<const void*>();
TMap<const void*, int32>& GWoTSha256ObserverInFlight =
	*new TMap<const void*, int32>();
TMap<const void*, TMap<uint32, int32>>& GWoTSha256ObserverThreads =
	*new TMap<const void*, TMap<uint32, int32>>();
TMap<const void*, FEvent*>& GWoTSha256ObserverDrainedEvents =
	*new TMap<const void*, FEvent*>();
}

TSharedPtr<uint8, ESPMode::ThreadSafe> WoTObserveSha256ForTesting(
	TFunction<void(TConstArrayView<uint8>)> Observer,
	TFunction<void()> OnDrainEntered,
	TFunction<void(FEvent*)> WaitForDrain)
{
	return WoTRegisterAutomationObserverForTesting(
		GWoTSha256ObserverGuard, GWoTSha256Observers,
		GWoTActiveSha256Observers, GWoTSha256ObserverInFlight,
		GWoTSha256ObserverThreads, GWoTSha256ObserverDrainedEvents,
		MoveTemp(Observer), MoveTemp(OnDrainEntered), MoveTemp(WaitForDrain));
}
#endif
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace WoT::Bootstrap::Detail
{
FString FixedBytesToHex(const uint8* Bytes, int32 Count)
{
	static const TCHAR* const Hex = TEXT("0123456789abcdef");
	FString Text;
	Text.Reserve(Count * 2);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Text.AppendChar(Hex[(Bytes[Index] >> 4) & 0xF]);
		Text.AppendChar(Hex[Bytes[Index] & 0xF]);
	}
	return Text;
}

FWoTBootstrapStatus FixedBytesFromHex(FStringView Hex, uint8* OutBytes, int32 Count)
{
	if (Hex.Len() != Count * 2)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Crypto,
			FString::Printf(TEXT("expected %d hex characters, got %d"),
				Count * 2, Hex.Len())));
	}

	for (int32 Index = 0; Index < Count; ++Index)
	{
		uint8 Value = 0;
		for (int32 Half = 0; Half < 2; ++Half)
		{
			const TCHAR Character = Hex[Index * 2 + Half];
			uint8 Nibble = 0;
			if (Character >= TEXT('0') && Character <= TEXT('9'))
			{
				Nibble = static_cast<uint8>(Character - TEXT('0'));
			}
			else if (Character >= TEXT('a') && Character <= TEXT('f'))
			{
				Nibble = static_cast<uint8>(10 + (Character - TEXT('a')));
			}
			else if (Character >= TEXT('A') && Character <= TEXT('F'))
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Crypto,
					TEXT("hex fields are lower case in every artifact")));
			}
			else
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Crypto,
					FString::Printf(TEXT("'%c' is not a hex digit"), Character)));
			}
			Value = static_cast<uint8>((Value << 4) | Nibble);
		}
		OutBytes[Index] = Value;
	}
	return FWoTBootstrapStatus::Success();
}
}  // namespace WoT::Bootstrap::Detail

FString FWoTSha256::ToHex() const
{
	return WoT::Bootstrap::Detail::FixedBytesToHex(Bytes, ByteCount);
}

TWoTBootstrapResult<FWoTSha256> FWoTSha256::FromHex(FStringView Hex)
{
	FWoTSha256 Digest;
	if (FWoTBootstrapStatus Status =
			WoT::Bootstrap::Detail::FixedBytesFromHex(Hex, Digest.Bytes, ByteCount);
		Status.IsError())
	{
		return TWoTBootstrapResult<FWoTSha256>::Error(MoveTemp(Status).GetError());
	}
	return TWoTBootstrapResult<FWoTSha256>::Success(Digest);
}

bool FWoTSha256::operator==(const FWoTSha256& Other) const
{
	// Digest comparison is not a secret-dependent operation, but keeping it
	// constant time costs nothing and removes the question entirely.
	uint8 Difference = 0;
	for (int32 Index = 0; Index < ByteCount; ++Index)
	{
		Difference |= static_cast<uint8>(Bytes[Index] ^ Other.Bytes[Index]);
	}
	return Difference == 0;
}

struct FWoTSha256Hasher::FImpl
{
	EVP_MD_CTX* Context = nullptr;
	bool bFinished = false;
};

FWoTSha256Hasher::FWoTSha256Hasher()
	: Impl(MakeUnique<FImpl>())
{
	Impl->Context = EVP_MD_CTX_new();
	if (Impl->Context != nullptr)
	{
		if (EVP_DigestInit_ex(Impl->Context, EVP_sha256(), nullptr) != 1)
		{
			EVP_MD_CTX_free(Impl->Context);
			Impl->Context = nullptr;
		}
	}
	// A null context is carried rather than thrown: Update/Finish stay callable and
	// Finish returns a zero digest, which never matches a real one.
	checkf(Impl->Context != nullptr, TEXT("OpenSSL could not create a SHA-256 context"));
}

FWoTSha256Hasher::~FWoTSha256Hasher()
{
	if (Impl.IsValid() && Impl->Context != nullptr)
	{
		EVP_MD_CTX_free(Impl->Context);
		Impl->Context = nullptr;
	}
}

FWoTSha256Hasher::FWoTSha256Hasher(FWoTSha256Hasher&& Other) = default;
FWoTSha256Hasher& FWoTSha256Hasher::operator=(FWoTSha256Hasher&& Other) = default;

void FWoTSha256Hasher::Update(TConstArrayView<uint8> Bytes)
{
	check(Impl.IsValid() && !Impl->bFinished);
	if (Bytes.Num() == 0)
	{
		return;
	}
	const int32 Result = EVP_DigestUpdate(Impl->Context, Bytes.GetData(),
		static_cast<size_t>(Bytes.Num()));
	checkf(Result == 1, TEXT("OpenSSL SHA-256 update failed"));
}

FWoTSha256 FWoTSha256Hasher::Finish()
{
	check(Impl.IsValid() && !Impl->bFinished);
	FWoTSha256 Digest;
	unsigned int Length = 0;
	const int32 Result = EVP_DigestFinal_ex(Impl->Context, Digest.Bytes, &Length);
	Impl->bFinished = true;
	checkf(Result == 1 && Length == static_cast<unsigned int>(FWoTSha256::ByteCount),
		TEXT("OpenSSL SHA-256 finalization failed"));
	return Digest;
}

FWoTSha256 Sha256(TConstArrayView<uint8> Bytes)
{
	#if WITH_AUTOMATION_TESTS
	WoTBroadcastAutomationObserverForTesting(
		GWoTSha256ObserverGuard, GWoTSha256Observers,
		GWoTActiveSha256Observers, GWoTSha256ObserverInFlight,
		GWoTSha256ObserverThreads, GWoTSha256ObserverDrainedEvents, Bytes);
	#endif
	FWoTSha256Hasher Hasher;
	Hasher.Update(Bytes);
	return Hasher.Finish();
}
