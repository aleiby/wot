#include "WoTRetailReader/ProductionContract.h"

#include "Containers/StringConv.h"
#include "Misc/ScopeLock.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/KeyRing.h"
#include "WoTRetailReader/Generated/CacheContractProduction.generated.h"

namespace
{
#if WITH_AUTOMATION_TESTS
FCriticalSection GWoTProductionContractTestMutex;
TOptional<FWoTRetailContract> GWoTProductionContractForTesting;
#endif
template <typename T>
TWoTBootstrapResult<T> WoTProductionContractError(const TCHAR* Message,
	const TCHAR* Path)
{
	return TWoTBootstrapResult<T>::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::Crypto, Message, Path));
}

void WoTProductionContractAppendAscii(TArray<uint8>& Out, const ANSICHAR* Text,
	uint32 Length)
{
	Out.Append(reinterpret_cast<const uint8*>(Text), static_cast<int32>(Length));
}
} // namespace

TWoTBootstrapResult<FWoTRetailContract> WoTLoadProductionRetailContract()
{
	using FResult = TWoTBootstrapResult<FWoTRetailContract>;
	using namespace WoT::Retail::Generated;

#if WITH_AUTOMATION_TESTS
	{
		FScopeLock Scope(&GWoTProductionContractTestMutex);
		if (GWoTProductionContractForTesting.IsSet())
		{
			return FResult::Success(GWoTProductionContractForTesting.GetValue());
		}
	}
#endif

	if (FStringView(ProductionSignatureContext)
			!= WoT::Bootstrap::CacheContractSignatureContext
		|| ProductionPayloadSize == 0
		|| UE_ARRAY_COUNT(ProductionPublicKey) != FWoTEd25519PublicKey::ByteCount
		|| UE_ARRAY_COUNT(ProductionSignature) != FWoTEd25519Signature::ByteCount)
	{
		return WoTProductionContractError<FWoTRetailContract>(
			TEXT("the compiled production cache-contract material is malformed"),
			TEXT("contract.payload"));
	}
	TArray<uint8> Payload;
	Payload.Reserve(static_cast<int32>(ProductionPayloadSize));
	for (const FProductionPayloadFragment& Fragment : ProductionPayloadFragments)
	{
		WoTProductionContractAppendAscii(Payload, Fragment.Data, Fragment.Size);
	}
	if (Payload.Num() != static_cast<int32>(ProductionPayloadSize))
	{
		return WoTProductionContractError<FWoTRetailContract>(
			TEXT("the compiled production cache-contract fragments are malformed"),
			TEXT("contract.payload"));
	}
	TWoTBootstrapResult<FWoTSha256> ExpectedPayloadSha =
		FWoTSha256::FromHex(ProductionPayloadSha256);
	if (ExpectedPayloadSha.IsError()
		|| Sha256(Payload) != ExpectedPayloadSha.GetValue())
	{
		return WoTProductionContractError<FWoTRetailContract>(
			TEXT("the compiled production cache-contract payload digest drifted"),
			TEXT("contract.payload"));
	}

	FWoTKeyRing Ring;
	Ring.Format = WoTReleaseKeyRingFormat;
	Ring.Epoch = 1;
	FWoTReleaseKey& Key = Ring.Keys.AddDefaulted_GetRef();
	Key.KeyId = ProductionKeyId;
	Key.FirstSequence = ProductionReleaseSequence;
	Key.LastSequence = ProductionReleaseSequence;
	FMemory::Memcpy(Key.PublicKey.Bytes, ProductionPublicKey,
		FWoTEd25519PublicKey::ByteCount);

	const FString SignatureHex = WoT::Bootstrap::Detail::FixedBytesToHex(
		ProductionSignature, UE_ARRAY_COUNT(ProductionSignature));
	const FTCHARToUTF8 SignatureUtf8(*SignatureHex);
	TArray<uint8> Document;
	Document.Reserve(Payload.Num() + SignatureUtf8.Length() + 32);
	static constexpr ANSICHAR DocumentPrefix[] = "{\"payload\":";
	static constexpr ANSICHAR SignaturePrefix[] = ",\"signature\":\"";
	static constexpr ANSICHAR DocumentSuffix[] = "\"}";
	WoTProductionContractAppendAscii(
		Document, DocumentPrefix, UE_ARRAY_COUNT(DocumentPrefix) - 1);
	Document.Append(Payload.GetData(), Payload.Num());
	WoTProductionContractAppendAscii(
		Document, SignaturePrefix, UE_ARRAY_COUNT(SignaturePrefix) - 1);
	Document.Append(reinterpret_cast<const uint8*>(SignatureUtf8.Get()),
		SignatureUtf8.Length());
	WoTProductionContractAppendAscii(
		Document, DocumentSuffix, UE_ARRAY_COUNT(DocumentSuffix) - 1);
	return ParseRetailContract(Document, Ring);
}

#if WITH_AUTOMATION_TESTS
void WoTSetProductionRetailContractForTesting(const FWoTRetailContract* Contract)
{
	FScopeLock Scope(&GWoTProductionContractTestMutex);
	if (Contract == nullptr)
	{
		GWoTProductionContractForTesting.Reset();
	}
	else
	{
		GWoTProductionContractForTesting = *Contract;
	}
}
#endif
