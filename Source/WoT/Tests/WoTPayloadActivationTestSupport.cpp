#include "WoTPayloadActivationTestSupport.h"
#include "WoTCacheTestSupport.h"

#if WITH_AUTOMATION_TESTS

#include "../Payload/WoTCacheStore.h"
#include "../WoTPayloadSubsystem.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTRetailReader/CacheManifest.h"
#include "Misc/Paths.h"

namespace
{
void WoTPayloadActivationTestSupport_AppendU32(
	TArray<uint8>& Bytes, uint32 Value)
{
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Bytes.Add(static_cast<uint8>((Value >> (Index * 8)) & 0xff));
	}
}

bool WoTPayloadActivationTestSupport_Error(
	FWoTBootstrapError&& Error, FString& OutError)
{
	OutError = Error.ToString();
	return false;
}
} // namespace

TArray<uint8> WoTPayloadActivationTest_FrameObject(
	FStringView CanonicalHeader, TConstArrayView<uint8> Payload)
{
	TArray<uint8> Result;
	const uint8 Magic[8] = {'W', 'O', 'T', 'O', 'B', 'J', '3', 0};
	Result.Append(Magic, UE_ARRAY_COUNT(Magic));
	const FString HeaderString(CanonicalHeader);
	const FTCHARToUTF8 HeaderUtf8(*HeaderString);
	WoTPayloadActivationTestSupport_AppendU32(
		Result, static_cast<uint32>(HeaderUtf8.Length()));
	Result.Append(
		reinterpret_cast<const uint8*>(HeaderUtf8.Get()), HeaderUtf8.Length());
	Result.Append(Payload.GetData(), Payload.Num());
	return Result;
}

bool WoTPayloadActivationTest_PublishObject(
	const FString& Root, const FString& LogicalId, const FString& Schema,
	TConstArrayView<uint8> ObjectBytes, FString& OutError)
{
	FWoTPayloadActivationTestObject Object;
	Object.LogicalId = LogicalId;
	Object.Schema = Schema;
	Object.Bytes.Append(ObjectBytes.GetData(), ObjectBytes.Num());
	return WoTPayloadActivationTest_PublishObjects(Root, MakeArrayView(&Object, 1), OutError);
}

bool WoTPayloadActivationTest_PublishObjects(
	const FString& Root, TConstArrayView<FWoTPayloadActivationTestObject> Objects,
	FString& OutError)
{
	TWoTBootstrapResult<FWoTCacheStore> Created =
		FWoTCacheStore::ForTesting(Root);
	if (Created.IsError())
	{
		return WoTPayloadActivationTestSupport_Error(
			MoveTemp(Created).GetError(), OutError);
	}
	FWoTCacheStore Store = MoveTemp(Created).GetValue();
	TWoTBootstrapResult<FWoTProfileSession> SessionResult =
		AcquireWoTProfileSessionShared(FPaths::GetPath(Root));
	if (SessionResult.IsError())
	{
		return WoTPayloadActivationTestSupport_Error(
			MoveTemp(SessionResult).GetError(), OutError);
	}
	FWoTProfileSession Session = MoveTemp(SessionResult).GetValue();
	TWoTBootstrapResult<bool> Exclusive = Session.TryAcquireExclusive();
	if (Exclusive.IsError())
	{
		return WoTPayloadActivationTestSupport_Error(
			MoveTemp(Exclusive).GetError(), OutError);
	}
	if (!Exclusive.GetValue())
	{
		OutError = TEXT("the test profile session is contended");
		return false;
	}
	FWoTBootstrapStatus Permit =
		Store.BeginBuild(Session);
	if (Permit.IsError())
	{
		return WoTPayloadActivationTestSupport_Error(
			MoveTemp(Permit).GetError(), OutError);
	}
	FWoTCacheBuildResult Build;
	for (const FWoTPayloadActivationTestObject& Object : Objects)
	{
		FWoTBootstrapStatus Written = Store.WriteObject(
			Object.LogicalId, Object.Schema, Object.Bytes, Sha256(Object.Bytes),
			1024ull * 1024ull * 1024ull);
		if (Written.IsError())
		{
			return WoTPayloadActivationTestSupport_Error(
				MoveTemp(Written).GetError(), OutError);
		}
		FWoTCacheBlob& Blob = Build.Manifest.Blobs.AddDefaulted_GetRef();
		Blob.LogicalId = Object.LogicalId;
		Blob.Schema = Object.Schema;
		Blob.Sha256 = Sha256(Object.Bytes);
		Blob.Size = static_cast<uint64>(Object.Bytes.Num());
		Blob.Satisfies.Add(FWoTRequiredFactId{TEXT("spec.") + Object.LogicalId});
	}
	TWoTBootstrapResult<TArray<uint8>> Manifest =
		WriteCanonicalCacheManifest(Build.Manifest);
	if (Manifest.IsError())
	{
		return WoTPayloadActivationTestSupport_Error(
			MoveTemp(Manifest).GetError(), OutError);
	}
	Build.CanonicalManifest = MoveTemp(Manifest).GetValue();
	Build.ManifestSha256 = Sha256(Build.CanonicalManifest);
	FWoTBootstrapStatus Published =
		Store.Publish(Build);
	if (Published.IsError())
	{
		return WoTPayloadActivationTestSupport_Error(
			MoveTemp(Published).GetError(), OutError);
	}
	FWoTBootstrapStatus Restored = Session.RestoreShared();
	if (Restored.IsError())
	{
		return WoTPayloadActivationTestSupport_Error(
			MoveTemp(Restored).GetError(), OutError);
	}
	return true;
}

bool WoTPayloadActivationTest_ActivatePublished(
	UWoTPayloadSubsystem* Subsystem, const FString& Root, FString& OutError)
{
	FWoTBootstrapStatus Activated =
		WoTPayloadActivationTest_ActivatePublishedForWorldAuthorization(
			Subsystem, Root);
	if (Activated.IsError())
	{
		return WoTPayloadActivationTestSupport_Error(
			MoveTemp(Activated).GetError(), OutError);
	}
	return true;
}

FWoTBootstrapStatus
WoTPayloadActivationTest_ActivatePublishedForWorldAuthorization(
	UWoTPayloadSubsystem* Subsystem, const FString& Root)
{
	if (Subsystem == nullptr)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the payload subsystem fixture is null")));
	}
	TWoTBootstrapResult<FWoTCacheStore> Created =
		FWoTCacheStore::ForTesting(Root);
	if (Created.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Created).GetError());
	}
	FWoTCacheStore Store = MoveTemp(Created).GetValue();
	TWoTBootstrapResult<FWoTCacheGenerationLease> Acquired =
		WoTAcquireCurrentForTesting(Store);
	if (Acquired.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Acquired).GetError());
	}
	return WoTPayloadActivationTest_ActivateLeaseForWorldAuthorization(
		Subsystem, Acquired.GetValue());
}

FWoTBootstrapStatus
WoTPayloadActivationTest_ActivateLeaseForWorldAuthorization(
	UWoTPayloadSubsystem* Subsystem, const FWoTCacheGenerationLease& Lease)
{
	if (Subsystem == nullptr)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the payload subsystem fixture is null")));
	}
	return Subsystem->ActivateVerifiedCache(Lease);
}

#endif // WITH_AUTOMATION_TESTS
