#include "WoTRetailReader/CacheManifest.h"

#include "CanonicalObjectBuilder.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTRetailReader/WorldDescriptorSidecar.h"

namespace
{
FWoTBootstrapError CacheManifestError(EWoTBootstrapError Code,
	const TCHAR* Message, FString Path = FString())
{
	return MakeWoTBootstrapError(Code, Message, MoveTemp(Path));
}
}  // namespace

TWoTBootstrapResult<TArray<uint8>> WriteCanonicalCacheManifest(
	const FWoTCacheManifestV3& Manifest)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	if (Manifest.Format.IsEmpty() || Manifest.HashAlgorithm.IsEmpty())
	{
		return FResult::Error(CacheManifestError(EWoTBootstrapError::UnknownField,
			TEXT("the cache manifest format and hash algorithm must not be empty")));
	}

	TArray<int32> OrderedIndices;
	OrderedIndices.Reserve(Manifest.Blobs.Num());
	for (int32 Index = 0; Index < Manifest.Blobs.Num(); ++Index)
	{
		OrderedIndices.Add(Index);
	}
	OrderedIndices.Sort([&Manifest](int32 Left, int32 Right)
	{
		return Manifest.Blobs[Left].LogicalId < Manifest.Blobs[Right].LogicalId;
	});

	FWoTCanonicalJsonValue Blobs = FWoTCanonicalJsonValue::MakeArray();
	FString PreviousLogicalId;
	for (const int32 Index : OrderedIndices)
	{
		const FWoTCacheBlob& Blob = Manifest.Blobs[Index];
		if (Blob.LogicalId.IsEmpty() || Blob.Schema.IsEmpty() || Blob.Satisfies.IsEmpty()
			|| Blob.Size > static_cast<uint64>(TNumericLimits<int64>::Max())
			|| (!PreviousLogicalId.IsEmpty() && PreviousLogicalId == Blob.LogicalId))
		{
			return FResult::Error(CacheManifestError(EWoTBootstrapError::UnknownField,
				TEXT("the cache manifest blob is not representable"), Blob.LogicalId));
		}
		PreviousLogicalId = Blob.LogicalId;

		TArray<FString> FactIds;
		FactIds.Reserve(Blob.Satisfies.Num());
		for (const FWoTRequiredFactId& FactId : Blob.Satisfies)
		{
			if (FactId.Value.IsEmpty())
			{
				return FResult::Error(CacheManifestError(
					EWoTBootstrapError::UnknownField,
					TEXT("a cache manifest fact id must not be empty"), Blob.LogicalId));
			}
			FactIds.Add(FactId.Value);
		}
		FactIds.Sort();
		FWoTCanonicalJsonValue Satisfies = FWoTCanonicalJsonValue::MakeArray();
		FString PreviousFactId;
		for (const FString& FactId : FactIds)
		{
			if (!PreviousFactId.IsEmpty() && PreviousFactId == FactId)
			{
				return FResult::Error(CacheManifestError(
					EWoTBootstrapError::DuplicateField,
					TEXT("a cache manifest blob contains a duplicate fact id"), FactId));
			}
			PreviousFactId = FactId;
			Satisfies.AddElement(FWoTCanonicalJsonValue::MakeString(FactId));
		}

		FWoTCanonicalObjectBuilder RecordBuilder;
		RecordBuilder.Add(TEXT("logical_id"),
			FWoTCanonicalJsonValue::MakeString(Blob.LogicalId));
		RecordBuilder.Add(TEXT("satisfies"), MoveTemp(Satisfies));
		RecordBuilder.Add(TEXT("schema"),
			FWoTCanonicalJsonValue::MakeString(Blob.Schema));
		RecordBuilder.Add(TEXT("sha256"),
			FWoTCanonicalJsonValue::MakeString(Blob.Sha256.ToHex()));
		RecordBuilder.Add(TEXT("size"),
			FWoTCanonicalJsonValue::MakeInteger(static_cast<int64>(Blob.Size)));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltRecord =
			MoveTemp(RecordBuilder).Build(TEXT("cache manifest blob"));
		if (BuiltRecord.IsError())
		{
			return FResult::Error(MoveTemp(BuiltRecord).GetError());
		}
		Blobs.AddElement(MoveTemp(BuiltRecord).GetValue());
	}

	FWoTCanonicalObjectBuilder RootBuilder;
	RootBuilder.Add(TEXT("blobs"), MoveTemp(Blobs));
	RootBuilder.Add(TEXT("cache_version"),
		FWoTCanonicalJsonValue::MakeInteger(Manifest.CacheVersion));
	if (Manifest.bHasBuildIdentity)
	{
		RootBuilder.Add(TEXT("contract_sha256"),
			FWoTCanonicalJsonValue::MakeString(Manifest.ContractSha256.ToHex()));
	}
	RootBuilder.Add(TEXT("format"),
		FWoTCanonicalJsonValue::MakeString(Manifest.Format));
	if (Manifest.bHasBuildIdentity)
	{
		RootBuilder.Add(TEXT("front_end_descriptor_root"),
			FWoTCanonicalJsonValue::MakeString(Manifest.FrontEndDescriptorRoot.ToHex()));
	}
	RootBuilder.Add(TEXT("hash_algorithm"),
		FWoTCanonicalJsonValue::MakeString(Manifest.HashAlgorithm));
	if (Manifest.bHasBuildIdentity)
	{
		RootBuilder.Add(TEXT("installer_sha256"),
			FWoTCanonicalJsonValue::MakeString(Manifest.InstallerSha256.ToHex()));
		RootBuilder.Add(TEXT("production_descriptor_sha256"),
			FWoTCanonicalJsonValue::MakeString(
				Manifest.ProductionDescriptorSha256.ToHex()));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Worlds =
			WriteCacheWorldDescriptors(Manifest.WorldDescriptors);
		if (Worlds.IsError())
		{
			return FResult::Error(MoveTemp(Worlds).GetError());
		}
		RootBuilder.Add(TEXT("world_descriptors"), MoveTemp(Worlds).GetValue());
	}
	TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltRoot =
		MoveTemp(RootBuilder).Build(TEXT("cache manifest root"));
	if (BuiltRoot.IsError())
	{
		return FResult::Error(MoveTemp(BuiltRoot).GetError());
	}
	return WriteCanonicalJson(BuiltRoot.GetValue());
}
