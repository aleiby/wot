// Authenticated description of the retail WheelOfTime entry world.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/Contract.h"
#include "WoTRetailReader/Package.h"

struct FWoTFrontEndCensus
{
	uint32 Lights = 0;
	uint32 AmbientSounds = 0;
	uint32 SplashEmitters = 0;
	uint32 RainCylinders = 0;
	uint32 LightningActors = 0;
	uint32 RandomLightningControllers = 0;
};

/** One exact typed edge from retail source identity to cache output. */
struct FWoTFrontEndResourceRequirement
{
	EWoTFactDomain Domain = EWoTFactDomain::Texture;
	FString SourcePath;
	FString SourceExport;
	FString LogicalId;
};

/** A separate entry-world record. It is deliberately not a multiplayer slot. */
struct FWoTFrontEndDescriptor
{
	FString MapName;
	FString BspLogicalId;
	FString WorldLogicalId;
	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	float HorizontalFov = 90.0f;
	FWoTFrontEndCensus Census;
	/** Exact effective UE1 RandomLightning String[10] order after applying the
	 * entry actor's authenticated instance overrides to authenticated defaults. */
	TArray<FString> ThunderSoundSourceExports;
	TArray<FWoTFrontEndResourceRequirement> RequiredResources;
	TArray<FString> RequiredLogicalIds;
	FWoTSha256 SourceRoot;
	FWoTSha256 DescriptorRoot;
};

/** Read and quarantine the entry map and its contract-authorized dependencies. */
WOTRETAILREADER_API TWoTBootstrapResult<FWoTFrontEndDescriptor>
BuildFrontEndDescriptor(const FWoTRetailPackage& Package,
	const FWoTRetailContract& Contract, IWoTProgressSink& Progress);

/** Validate every pinned field and recompute the canonical descriptor root. */
WOTRETAILREADER_API FWoTBootstrapStatus ValidateFrontEndDescriptor(
	const FWoTFrontEndDescriptor& Descriptor);

WOTRETAILREADER_API TWoTBootstrapResult<FWoTSha256>
ComputeFrontEndDescriptorRoot(const FWoTFrontEndDescriptor& Descriptor);

#if WITH_AUTOMATION_TESTS
struct FWoTFrontEndSourceBytesForTesting
{
	FString SourcePath;
	TArray<uint8> Bytes;
};

enum class EWoTFrontEndPackageAttackForTesting : uint8
{
	DuplicateLevelActorReference,
	LevelModelTargetsNonModel,
	UnclassifiedInternalReference,
	ThunderSourceRootMismatch,
	ThunderSourceCaseMismatch,
};

/** Produce one structurally valid hostile package-byte fixture. */
WOTRETAILREADER_API TWoTBootstrapResult<TArray<uint8>>
MakeHostileFrontEndPackageBytesForTesting(TConstArrayView<uint8> PackageBytes,
	EWoTFrontEndPackageAttackForTesting Attack);

/** Synthetic closed semantic grammar. No retail byte or retail hash is stored. */
WOTRETAILREADER_API TWoTBootstrapResult<FWoTFrontEndDescriptor>
BuildFrontEndDescriptorSemanticFixtureForTesting(
	TConstArrayView<uint8> CanonicalBytes, const FWoTRetailContract& Contract);

/** The production UE1 decoder with a source-only byte seam. */
WOTRETAILREADER_API TWoTBootstrapResult<FWoTFrontEndDescriptor>
BuildFrontEndDescriptorPackageBytesForTesting(
	TConstArrayView<FWoTFrontEndSourceBytesForTesting> Sources,
	const FWoTRetailContract& Contract);
#endif
