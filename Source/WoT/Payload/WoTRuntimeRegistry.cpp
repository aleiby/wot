#include "WoTRuntimeRegistry.h"

#include "Misc/ScopeLock.h"
#include "WoTCacheLease.h"
#include "WoTPayloadActivation.h"

namespace
{
FWoTBootstrapStatus WoTRuntimeRegistryStatus(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

template <typename T>
TWoTBootstrapResult<T> WoTRuntimeRegistryError(const TCHAR* Message)
{
	return TWoTBootstrapResult<T>::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}
}

FWoTRuntimeRegistry& FWoTRuntimeRegistry::Get()
{
	static FWoTRuntimeRegistry Registry;
	return Registry;
}

FWoTBootstrapStatus FWoTRuntimeRegistry::BindVerifiedGeneration(
	const FWoTCacheGenerationLease& Lease)
{
	if (!IsInGameThread() || !Lease.IsValid())
	{
		return WoTRuntimeRegistryStatus(
			TEXT("runtime resources require a game-thread activated verified generation"));
	}
	FScopeLock ReadAdmission(&Lease.GetVerifiedReadGuard());
	if (!Lease.MayBeginVerifiedReadUnderVerifiedReadGuard())
	{
		return WoTRuntimeRegistryStatus(
			TEXT("runtime resource binding was denied during cache revocation"));
	}
	FScopeLock Lock(&Guard);
	if (ActiveLease != nullptr)
	{
		return WoTRuntimeRegistryStatus(
			TEXT("a runtime generation is already bound"));
	}
	ActiveLease = &Lease;
	for (const TPair<FString, FWoTCacheLeaseManifestEntry>& Pair
		: Lease.GetManifestTable())
	{
		FWoTCacheBlobDescriptor Descriptor;
		Descriptor.LogicalId = Pair.Value.LogicalId;
		Descriptor.Schema = Pair.Value.Schema;
		Descriptor.Size = Pair.Value.Size;
		Descriptor.Sha256 = Pair.Value.Sha256;
		Descriptors.Add(Pair.Key, MoveTemp(Descriptor));
	}
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus FWoTRuntimeRegistry::RegisterDescriptor(
	const FWoTNeutralResourceId& Resource,
	const FWoTCacheBlobDescriptor& Descriptor)
{
	if (!IsInGameThread() || !Resource.IsValid()
		|| Resource.LogicalId != Descriptor.LogicalId
		|| Descriptor.Schema != WoTNeutralResourceSchema(Resource.Kind)
		|| Descriptor.Size == 0)
	{
		return WoTRuntimeRegistryStatus(
			TEXT("runtime descriptor does not match its neutral semantic id"));
	}
	FScopeLock Lock(&Guard);
	if (ActiveLease == nullptr)
	{
		return WoTRuntimeRegistryStatus(
			TEXT("runtime descriptor has no active lease"));
	}
	const FWoTCacheBlobDescriptor* Authorized =
		Descriptors.Find(Resource.LogicalId);
	if (Authorized == nullptr
		|| Authorized->Schema != Descriptor.Schema
		|| Authorized->Size != Descriptor.Size
		|| Authorized->Sha256 != Descriptor.Sha256)
	{
		return WoTRuntimeRegistryStatus(
			TEXT("runtime descriptor is absent from the active lease"));
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<FWoTImmutableVerifiedBlob> FWoTRuntimeRegistry::Read(
	const FWoTNeutralResourceId& Resource,
	uint64 MaximumDecodedBytes) const
{
	const FWoTCacheGenerationLease* Lease = nullptr;
	FWoTCacheBlobDescriptor Descriptor;
	if (!IsInGameThread())
	{
		return WoTRuntimeRegistryError<FWoTImmutableVerifiedBlob>(
			TEXT("runtime resources may be read only on the owning game thread"));
	}
	{
		FScopeLock Lock(&Guard);
		Lease = ActiveLease;
		const FWoTCacheBlobDescriptor* Found =
			Descriptors.Find(Resource.LogicalId);
		if (!IsWoTPayloadBootstrapActive()
			|| Lease == nullptr || Found == nullptr || !Resource.IsValid()
			|| Found->Schema != WoTNeutralResourceSchema(Resource.Kind))
		{
			return WoTRuntimeRegistryError<FWoTImmutableVerifiedBlob>(
				TEXT("runtime resource is not registered in the active generation"));
		}
		Descriptor = *Found;
	}
	FWoTCacheBlobReadLimits Limits;
	Limits.MaximumBlobBytes = MaximumDecodedBytes;
	Limits.MaximumAllocationBytes = MaximumDecodedBytes;
	return ReadVerifiedCacheBlob(*Lease, Descriptor, Limits);
}

TWoTBootstrapResult<FWoTRuntimeVerifiedObject>
FWoTRuntimeRegistry::ReadObject(
	const FWoTNeutralResourceId& Resource,
	uint64 MaximumDecodedBytes, FStringView ExpectedEncoding,
	FStringView ExpectedKind, FStringView RebuildOnlyEncoding,
	FStringView AlternateExpectedEncoding,
	FStringView SecondAlternateExpectedEncoding,
	FStringView ThirdAlternateExpectedEncoding,
	FStringView FourthAlternateExpectedEncoding) const
{
	using FResult = TWoTBootstrapResult<FWoTRuntimeVerifiedObject>;
	TWoTBootstrapResult<FWoTImmutableVerifiedBlob> ReadResult =
		Read(Resource, MaximumDecodedBytes);
	if (ReadResult.IsError())
	{
		return FResult::Error(MoveTemp(ReadResult).GetError());
	}
	FWoTImmutableVerifiedBlob Blob =
		MoveTemp(ReadResult).GetValue();
	const TConstArrayView<uint8> Bytes = Blob.Bytes();
	if (Bytes.Num() < 12
		|| FMemory::Memcmp(Bytes.GetData(), "WOTOBJ3", 8) != 0)
	{
		return WoTRuntimeRegistryError<FWoTRuntimeVerifiedObject>(
			TEXT("runtime object frame has invalid magic or is truncated"));
	}
	const uint32 HeaderBytes = static_cast<uint32>(Bytes[8])
		| (static_cast<uint32>(Bytes[9]) << 8)
		| (static_cast<uint32>(Bytes[10]) << 16)
		| (static_cast<uint32>(Bytes[11]) << 24);
	if (HeaderBytes > static_cast<uint32>(Bytes.Num() - 12)
		|| HeaderBytes > 4u * 1024u * 1024u)
	{
		return WoTRuntimeRegistryError<FWoTRuntimeVerifiedObject>(
			TEXT("runtime object canonical header is out of bounds"));
	}
	FWoTCanonicalJsonLimits JsonLimits;
	JsonLimits.MaxBytes = 4u * 1024u * 1024u;
	JsonLimits.MaxMembers = 1000000;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
		ParseCanonicalJson(TConstArrayView<uint8>(
			Bytes.GetData() + 12, static_cast<int32>(HeaderBytes)), JsonLimits);
	if (Parsed.IsError())
	{
		return FResult::Error(MoveTemp(Parsed).GetError());
	}
	FWoTCanonicalJsonValue Header =
		MoveTemp(Parsed).GetValue();
	FString Encoding;
	FString Kind;
	uint64 PayloadBytes = 0;
	TOptional<FWoTBootstrapError> HeaderError =
		ReadStringField(Header, TEXT("encoding"), TEXT("runtime object"), Encoding);
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadStringField(
			Header, TEXT("kind"), TEXT("runtime object"), Kind);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadUnsignedField(
			Header, TEXT("payload_byte_length"),
			TEXT("runtime object"), PayloadBytes);
	}
	const int32 PayloadOffset = 12 + static_cast<int32>(HeaderBytes);
	const bool bEncodingAccepted = Encoding == ExpectedEncoding
		|| (!AlternateExpectedEncoding.IsEmpty()
			&& Encoding == AlternateExpectedEncoding)
		|| (!SecondAlternateExpectedEncoding.IsEmpty()
			&& Encoding == SecondAlternateExpectedEncoding)
		|| (!ThirdAlternateExpectedEncoding.IsEmpty()
			&& Encoding == ThirdAlternateExpectedEncoding)
		|| (!FourthAlternateExpectedEncoding.IsEmpty()
			&& Encoding == FourthAlternateExpectedEncoding);
	if (HeaderError.IsSet()
		|| !bEncodingAccepted || Kind != ExpectedKind
		|| PayloadBytes != static_cast<uint64>(Bytes.Num() - PayloadOffset))
	{
		if (!RebuildOnlyEncoding.IsEmpty() && Encoding == RebuildOnlyEncoding
			&& Kind == ExpectedKind)
		{
			return WoTRuntimeRegistryError<FWoTRuntimeVerifiedObject>(
				TEXT("runtime object uses a diagnostic-only legacy encoding and the cache must be rebuilt"));
		}
		return WoTRuntimeRegistryError<FWoTRuntimeVerifiedObject>(
			TEXT("runtime object header kind, encoding, or payload size mismatched"));
	}
	FWoTRuntimeVerifiedObject Result;
	Result.Blob = MoveTemp(Blob);
	Result.ParsedHeader = MoveTemp(Header);
	Result.PayloadOffset = PayloadOffset;
	return FResult::Success(MoveTemp(Result));
}

#if WITH_AUTOMATION_TESTS || !WOT_GUARDED_BOOTSTRAP
TWoTBootstrapResult<FWoTRuntimeVerifiedObject>
FWoTRuntimeRegistry::DecodeUntrustedObjectBytes(TArray<uint8> InputBytes,
	FStringView ExpectedEncoding, FStringView ExpectedKind,
	FStringView RebuildOnlyEncoding,
	FStringView AlternateExpectedEncoding)
{
	using FResult = TWoTBootstrapResult<FWoTRuntimeVerifiedObject>;
	FWoTImmutableVerifiedBlob Blob;
	Blob.Buffer = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(
		MoveTemp(InputBytes));
	const TConstArrayView<uint8> Bytes = Blob.Bytes();
	if (Bytes.Num() < 12
		|| FMemory::Memcmp(Bytes.GetData(), "WOTOBJ3", 8) != 0)
	{
		return WoTRuntimeRegistryError<FWoTRuntimeVerifiedObject>(
			TEXT("untrusted object frame has invalid magic or is truncated"));
	}
	const uint32 HeaderBytes = static_cast<uint32>(Bytes[8])
		| (static_cast<uint32>(Bytes[9]) << 8)
		| (static_cast<uint32>(Bytes[10]) << 16)
		| (static_cast<uint32>(Bytes[11]) << 24);
	if (HeaderBytes > static_cast<uint32>(Bytes.Num() - 12)
		|| HeaderBytes > 4u * 1024u * 1024u)
	{
		return WoTRuntimeRegistryError<FWoTRuntimeVerifiedObject>(
			TEXT("untrusted object canonical header is out of bounds"));
	}
	FWoTCanonicalJsonLimits JsonLimits;
	JsonLimits.MaxBytes = 4u * 1024u * 1024u;
	JsonLimits.MaxMembers = 1000000;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(
		TConstArrayView<uint8>(Bytes.GetData() + 12,
			static_cast<int32>(HeaderBytes)), JsonLimits);
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	FWoTCanonicalJsonValue Header = MoveTemp(Parsed).GetValue();
	FString Encoding, Kind;
	uint64 PayloadBytes = 0;
	TOptional<FWoTBootstrapError> HeaderError = ReadStringField(
		Header, TEXT("encoding"), TEXT("untrusted object"), Encoding);
	if (!HeaderError.IsSet()) HeaderError = ReadStringField(
		Header, TEXT("kind"), TEXT("untrusted object"), Kind);
	if (!HeaderError.IsSet()) HeaderError = ReadUnsignedField(
		Header, TEXT("payload_byte_length"), TEXT("untrusted object"), PayloadBytes);
	const int32 PayloadOffset = 12 + static_cast<int32>(HeaderBytes);
	const bool bEncodingAccepted = Encoding == ExpectedEncoding
		|| (!AlternateExpectedEncoding.IsEmpty()
			&& Encoding == AlternateExpectedEncoding);
	if (HeaderError.IsSet() || !bEncodingAccepted || Kind != ExpectedKind
		|| PayloadBytes != static_cast<uint64>(Bytes.Num() - PayloadOffset))
	{
		if (!RebuildOnlyEncoding.IsEmpty() && Encoding == RebuildOnlyEncoding
			&& Kind == ExpectedKind)
		{
			return WoTRuntimeRegistryError<FWoTRuntimeVerifiedObject>(
				TEXT("untrusted object uses a diagnostic-only legacy encoding and the cache must be rebuilt"));
		}
		return WoTRuntimeRegistryError<FWoTRuntimeVerifiedObject>(
			TEXT("untrusted object header kind, encoding, or payload size mismatched"));
	}
	FWoTRuntimeVerifiedObject Result;
	Result.Blob = MoveTemp(Blob);
	Result.ParsedHeader = MoveTemp(Header);
	Result.PayloadOffset = PayloadOffset;
	return FResult::Success(MoveTemp(Result));
}
#endif

#if !WOT_GUARDED_BOOTSTRAP
TWoTBootstrapResult<FWoTRuntimeVerifiedObject>
FWoTRuntimeRegistry::DecodeObjectBytesForDevelopment(TArray<uint8> Bytes,
	FStringView ExpectedEncoding, FStringView ExpectedKind,
	FStringView RebuildOnlyEncoding,
	FStringView AlternateExpectedEncoding)
{
	return DecodeUntrustedObjectBytes(MoveTemp(Bytes), ExpectedEncoding,
		ExpectedKind, RebuildOnlyEncoding, AlternateExpectedEncoding);
}
#endif

#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<FWoTRuntimeVerifiedObject>
FWoTRuntimeRegistry::DecodeObjectBytesForTesting(TArray<uint8> Bytes,
	FStringView ExpectedEncoding, FStringView ExpectedKind,
	FStringView RebuildOnlyEncoding,
	FStringView AlternateExpectedEncoding)
{
	return DecodeUntrustedObjectBytes(MoveTemp(Bytes), ExpectedEncoding,
		ExpectedKind, RebuildOnlyEncoding, AlternateExpectedEncoding);
}
#endif

TArray<FWoTNeutralResourceId> FWoTRuntimeRegistry::ListResources(
	EWoTNeutralResourceKind Kind) const
{
	TArray<FWoTNeutralResourceId> Result;
	if (!IsInGameThread())
	{
		return Result;
	}
	const FString ExpectedSchema = WoTNeutralResourceSchema(Kind);
	{
		FScopeLock Lock(&Guard);
		if (!IsWoTPayloadBootstrapActive() || ActiveLease == nullptr)
		{
			return Result;
		}
		for (const TPair<FString, FWoTCacheBlobDescriptor>& Pair : Descriptors)
		{
			if (Pair.Value.Schema == ExpectedSchema)
			{
				Result.Add(FWoTNeutralResourceId{Pair.Key, Kind});
			}
		}
	}
	Result.Sort([](
		const FWoTNeutralResourceId& Left,
		const FWoTNeutralResourceId& Right)
	{
		return Left.LogicalId < Right.LogicalId;
	});
	return Result;
}

TOptional<FWoTCacheBlobDescriptor> FWoTRuntimeRegistry::FindDescriptor(
	const FWoTNeutralResourceId& Resource) const
{
	if (!IsInGameThread() || !Resource.IsValid())
	{
		return {};
	}
	FScopeLock Lock(&Guard);
	if (!IsWoTPayloadBootstrapActive() || ActiveLease == nullptr)
	{
		return {};
	}
	const FWoTCacheBlobDescriptor* Found = Descriptors.Find(Resource.LogicalId);
	if (Found == nullptr
		|| Found->Schema != WoTNeutralResourceSchema(Resource.Kind))
	{
		return {};
	}
	return *Found;
}

bool FWoTRuntimeRegistry::SharesSnapshotWith(
	const FWoTCacheGenerationLease& Lease) const
{
	FScopeLock Lock(&Guard);
	return ActiveLease != nullptr && ActiveLease->IsValid()
		&& ActiveLease->SharesSnapshotWith(Lease);
}

void FWoTRuntimeRegistry::Reset()
{
	check(IsInGameThread());
	FScopeLock Lock(&Guard);
	Descriptors.Empty();
	ActiveLease = nullptr;
}
