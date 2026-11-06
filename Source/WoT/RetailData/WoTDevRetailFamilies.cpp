#include "WoTDevRetailFamilies.h"

#if !WOT_GUARDED_BOOTSTRAP

#include "WoTRetailFamilies.h"
#include "WoTRetailFamilyPins.h"
#include "../Assets/WoTRetailIdentity.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../World/WoTRuntimeClassDefaults.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "WoTRetailReader/DevelopmentClassDefaults.h"
#include "WoTRetailReader/FontProcedural.h"
#include "WoTRetailReader/MeshAnimation.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#include "WoTRetailReader/TextureSound.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTRetailData, Log, All);

namespace
{
bool GWoTDevRetailFamiliesAttempted = false;
bool GWoTDevRetailFamiliesDecodeSucceeded = false;
int32 GWoTDevRetailFamiliesDecodedObjectCount = 0;
int32 GWoTDevRetailFamiliesCacheHitCount = 0;
double GWoTDevRetailFamiliesDecodeSeconds = 0.0;
TUniquePtr<FWoTRuntimeClassDefaults> GWoTDevRetailClassDefaults;
TUniquePtr<FWoTRetailBuildContext> GWoTDevRetailBuildContext;

struct FWoTDevRetailBuildState
{
	FWoTRetailContract Contract;
	TSharedPtr<FWoTDevelopmentPackageCache> SourceCache;
	TMap<FString, TUniquePtr<FWoTDevelopmentTexturePackage>> TexturePackages;
	TSet<FString> FailedTexturePackages;
	TMap<FString, TUniquePtr<FWoTDevelopmentProceduralPackage>>
		ProceduralPackages;
	TSet<FString> FailedProceduralPackages;
	TMap<FString, TUniquePtr<FWoTDevelopmentMeshPackage>> MeshPackages;
	TSet<FString> FailedMeshPackages;
	TMap<FString, FWoTRetailTextureFacts> TextureFacts;
	TSet<FString> MissingTextures;
	TMap<FString, TArray<FWoTVertexMeshSequence>> MeshSequences;
	TSet<FString> MissingMeshes;

	const FWoTRequiredFact* FindFact(
		EWoTFactDomain Domain, FStringView Identity) const
	{
		return Contract.Facts.FindByPredicate(
			[Domain, Identity](const FWoTRequiredFact& Fact)
			{
				return Fact.Domain == Domain
					&& (WoTRetailSourceExportMatches(
							Fact.SourceExport, Identity)
						// Class-default object references own the exact collision
						// suffix, while the signed production contract predates it.
						|| WoTRetailSourceExportMatches(
							Identity, Fact.SourceExport));
			});
	}

	bool FindTexture(FStringView Identity, FWoTRetailTextureFacts& Out)
	{
		Out = FWoTRetailTextureFacts{};
		const FString Key = FString(Identity).ToLower();
		if (const FWoTRetailTextureFacts* Existing = TextureFacts.Find(Key))
		{
			Out = *Existing;
			return true;
		}
		if (MissingTextures.Contains(Key) || !SourceCache.IsValid())
		{
			return false;
		}
		const FWoTRequiredFact* Fact =
			FindFact(EWoTFactDomain::Texture, Identity);
		const bool bProcedural = Fact == nullptr;
		if (bProcedural)
		{
			Fact = FindFact(EWoTFactDomain::Procedural, Identity);
		}
		if (Fact == nullptr)
		{
			MissingTextures.Add(Key);
			return false;
		}
		FWoTRetailTextureFacts Decoded;
		Decoded.bProcedural = bProcedural;
		bool bFound = false;
		if (bProcedural)
		{
			FWoTDevelopmentProceduralPackage* Package = nullptr;
			if (TUniquePtr<FWoTDevelopmentProceduralPackage>* Existing =
				ProceduralPackages.Find(Fact->SourcePath))
			{
				Package = Existing->Get();
			}
			else if (!FailedProceduralPackages.Contains(Fact->SourcePath))
			{
				auto Source = SourceCache->Load(Fact->SourcePath);
				if (Source.IsError())
				{
					FailedProceduralPackages.Add(Fact->SourcePath);
				}
				else
				{
					auto Parsed = FWoTDevelopmentProceduralPackage::Parse(
						TArray<uint8>(*Source.GetValue()), Fact->SourcePath);
					if (Parsed.IsError())
					{
						FailedProceduralPackages.Add(Fact->SourcePath);
					}
					else
					{
						Package = Parsed.GetValue().Get();
						ProceduralPackages.Add(
							Fact->SourcePath, MoveTemp(Parsed).GetValue());
					}
				}
			}
			bFound = Package != nullptr
				&& Package->FindTexture(Fact->SourceExport, Decoded.USize,
					Decoded.VSize, Decoded.Format);
		}
		else
		{
			FWoTDevelopmentTexturePackage* Package = nullptr;
			if (TUniquePtr<FWoTDevelopmentTexturePackage>* Existing =
				TexturePackages.Find(Fact->SourcePath))
			{
				Package = Existing->Get();
			}
			else if (!FailedTexturePackages.Contains(Fact->SourcePath))
			{
				auto Source = SourceCache->Load(Fact->SourcePath);
				if (Source.IsError())
				{
					FailedTexturePackages.Add(Fact->SourcePath);
				}
				else
				{
					auto Parsed = FWoTDevelopmentTexturePackage::Parse(
						TArray<uint8>(*Source.GetValue()), Fact->SourcePath);
					if (Parsed.IsError())
					{
						FailedTexturePackages.Add(Fact->SourcePath);
					}
					else
					{
						Package = Parsed.GetValue().Get();
						TexturePackages.Add(
							Fact->SourcePath, MoveTemp(Parsed).GetValue());
					}
				}
			}
			bFound = Package != nullptr
				&& Package->FindTexture(Fact->SourceExport, Decoded.USize,
					Decoded.VSize, Decoded.Format, Decoded.AnimNext,
					Decoded.MinFrameRate, Decoded.MaxFrameRate, Decoded.PrimeCount);
		}
		if (!bFound)
		{
			MissingTextures.Add(Key);
			return false;
		}
		TextureFacts.Add(Key, Decoded);
		Out = Decoded;
		return true;
	}

	bool FindMeshSequences(FStringView Identity,
		TArray<FWoTVertexMeshSequence>& Out)
	{
		Out.Reset();
		const FString Key = FString(Identity).ToLower();
		if (const TArray<FWoTVertexMeshSequence>* Existing =
			MeshSequences.Find(Key))
		{
			Out = *Existing;
			return true;
		}
		if (MissingMeshes.Contains(Key) || !SourceCache.IsValid())
		{
			return false;
		}
		const FWoTRequiredFact* Fact =
			FindFact(EWoTFactDomain::Mesh, Identity);
		if (Fact == nullptr)
		{
			MissingMeshes.Add(Key);
			return false;
		}
		FWoTDevelopmentMeshPackage* Package = nullptr;
		if (TUniquePtr<FWoTDevelopmentMeshPackage>* Existing =
			MeshPackages.Find(Fact->SourcePath))
		{
			Package = Existing->Get();
		}
		else if (FailedMeshPackages.Contains(Fact->SourcePath))
		{
			MissingMeshes.Add(Key);
			return false;
		}
		else
		{
			auto Source = SourceCache->Load(Fact->SourcePath);
			if (Source.IsError())
			{
				FailedMeshPackages.Add(Fact->SourcePath);
				MissingMeshes.Add(Key);
				return false;
			}
			auto Parsed = FWoTDevelopmentMeshPackage::Parse(
				*Source.GetValue(), Fact->SourcePath);
			if (Parsed.IsError())
			{
				FailedMeshPackages.Add(Fact->SourcePath);
				MissingMeshes.Add(Key);
				return false;
			}
			Package = Parsed.GetValue().Get();
			MeshPackages.Add(Fact->SourcePath, MoveTemp(Parsed).GetValue());
		}
		TArray<FWoTVertexMeshSequence> Decoded;
		if (Package == nullptr
			|| !Package->FindMeshSequences(Fact->SourceExport, Decoded))
		{
			MissingMeshes.Add(Key);
			return false;
		}
		MeshSequences.Add(Key, Decoded);
		Out = MoveTemp(Decoded);
		return true;
	}
};

TSharedPtr<FWoTDevRetailBuildState> GWoTDevRetailBuildState;
#if WITH_AUTOMATION_TESTS
TOptional<FString> GWoTDevRetailFamiliesCacheRootForTesting;
#endif
}

void WoTDevRetailFamiliesEnsureActivated()
{
	check(IsInGameThread());
	if (GWoTDevRetailFamiliesAttempted)
	{
		return;
	}
	GWoTDevRetailFamiliesAttempted = true;
	GWoTDevRetailFamiliesDecodeSucceeded = false;
	GWoTDevRetailFamiliesDecodedObjectCount = 0;
	GWoTDevRetailFamiliesCacheHitCount = 0;
	GWoTDevRetailFamiliesDecodeSeconds = 0.0;

	const FString SystemRoot =
		WoTReferencePath(TEXT("original/System"));
	if (!IFileManager::Get().DirectoryExists(*SystemRoot))
	{
		UE_LOG(LogWoTRetailData, Warning,
			TEXT("Retail family development activation skipped: WOT_REFERENCE_ROOT does not contain original/System"));
		return;
	}

	const double StartedAt = FPlatformTime::Seconds();
	TWoTBootstrapResult<FWoTRetailContract> ContractResult =
		WoTLoadProductionRetailContract();
	if (ContractResult.IsError())
	{
		UE_LOG(LogWoTRetailData, Error,
			TEXT("Retail family development contract failed: %s"),
			*ContractResult.GetError().ToString());
		return;
	}
	TSharedPtr<FWoTDevRetailBuildState> BuildState =
		MakeShared<FWoTDevRetailBuildState>();
	BuildState->Contract = MoveTemp(ContractResult).GetValue();
	BuildState->SourceCache = MakeShared<FWoTDevelopmentPackageCache>(
		WoTReferencePath(TEXT("original")));
	FString CacheRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("WoTRetailFamilies/ClassDefaults"));
#if WITH_AUTOMATION_TESTS
	if (GWoTDevRetailFamiliesCacheRootForTesting.IsSet())
	{
		CacheRoot = GWoTDevRetailFamiliesCacheRootForTesting.GetValue();
	}
#endif
	TUniquePtr<FWoTRuntimeClassDefaults> Candidate;
	FString Error;
	for (int32 Attempt = 0; Attempt < 2 && !Candidate.IsValid(); ++Attempt)
	{
		TWoTBootstrapResult<TArray<FWoTDevelopmentClassDefaultObject>> Built =
			WoTBuildClassDefaultObjectsForDevelopment(
				BuildState->Contract, *BuildState->SourceCache,
				CacheRoot, Attempt > 0);
		if (Built.IsError())
		{
			UE_LOG(LogWoTRetailData, Error,
				TEXT("Retail family development decode failed: %s"),
				*Built.GetError().ToString());
			return;
		}
		TArray<FWoTDevelopmentClassDefaultObject> BuiltObjects =
			MoveTemp(Built).GetValue();
		if (BuiltObjects.Num() != 20)
		{
			UE_LOG(LogWoTRetailData, Error,
				TEXT("Retail family development decode produced %d class-default objects, expected 20"),
				BuiltObjects.Num());
			return;
		}
		int32 CacheHits = 0;
		for (const FWoTDevelopmentClassDefaultObject& BuiltObject : BuiltObjects)
		{
			CacheHits += BuiltObject.bLoadedFromCache ? 1 : 0;
		}

		TArray<FWoTRuntimeVerifiedObject> RuntimeObjects;
		RuntimeObjects.Reserve(BuiltObjects.Num());
		Error.Reset();
		for (FWoTDevelopmentClassDefaultObject& BuiltObject : BuiltObjects)
		{
			TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
				FWoTRuntimeRegistry::DecodeObjectBytesForDevelopment(
					MoveTemp(BuiltObject.ObjectBytes),
					TEXT("ue1-class-defaults-v2"),
					TEXT("class_default"));
			if (Decoded.IsError())
			{
				Error = FString::Printf(
					TEXT("object %s failed runtime validation: %s"),
					*BuiltObject.LogicalId,
					*Decoded.GetError().ToString());
				break;
			}
			RuntimeObjects.Add(MoveTemp(Decoded).GetValue());
		}
		TUniquePtr<FWoTRuntimeClassDefaults> AttemptedCandidate;
		if (Error.IsEmpty())
		{
			AttemptedCandidate = MakeUnique<FWoTRuntimeClassDefaults>();
			if (!AttemptedCandidate->LoadObjectsForDevelopment(
				MoveTemp(RuntimeObjects), Error))
			{
				AttemptedCandidate.Reset();
			}
		}
		if (AttemptedCandidate.IsValid())
		{
			GWoTDevRetailFamiliesCacheHitCount = CacheHits;
			GWoTDevRetailFamiliesDecodedObjectCount = BuiltObjects.Num();
			Candidate = MoveTemp(AttemptedCandidate);
			break;
		}
		if (Attempt == 0 && CacheHits > 0)
		{
			UE_LOG(LogWoTRetailData, Warning,
				TEXT("Retail family development cache failed runtime validation and will be rebuilt: %s"),
				*Error);
			continue;
		}
		UE_LOG(LogWoTRetailData, Error,
			TEXT("Retail family development defaults failed runtime validation: %s"),
			*Error);
		return;
	}
	GWoTDevRetailFamiliesDecodeSeconds =
		FPlatformTime::Seconds() - StartedAt;
	GWoTDevRetailFamiliesDecodeSucceeded = true;
	UE_LOG(LogWoTRetailData, Display,
		TEXT("Decoded %d retail class-default packages in %.3f seconds (%d cache hits)"),
		GWoTDevRetailFamiliesDecodedObjectCount,
		GWoTDevRetailFamiliesDecodeSeconds,
		GWoTDevRetailFamiliesCacheHitCount);

	TUniquePtr<FWoTRetailBuildContext> BuildContext =
		MakeUnique<FWoTRetailBuildContext>(FWoTRetailBuildContext{
			*Candidate,
			[BuildState](FStringView Identity, FWoTRetailTextureFacts& Out)
			{
				return BuildState->FindTexture(Identity, Out);
			},
			[BuildState](FStringView Identity,
				TArray<FWoTVertexMeshSequence>& Out)
			{
				return BuildState->FindMeshSequences(Identity, Out);
			}});
	if (!WoTActivateRetailFamilies(
		*BuildContext, WoTRetailFamilies(), WoTRetailFamilyPins(), Error))
	{
		UE_LOG(LogWoTRetailData, Error,
			TEXT("Retail family development activation failed: %s"), *Error);
		return;
	}
	GWoTDevRetailClassDefaults = MoveTemp(Candidate);
	GWoTDevRetailBuildContext = MoveTemp(BuildContext);
	GWoTDevRetailBuildState = MoveTemp(BuildState);
}

const FWoTRuntimeClassDefaults* WoTDevRetailClassDefaults()
{
	return GWoTDevRetailClassDefaults.Get();
}

#if WITH_AUTOMATION_TESTS
void WoTDevRetailFamiliesResetForTesting()
{
	check(IsInGameThread());
	GWoTDevRetailClassDefaults.Reset();
	GWoTDevRetailBuildContext.Reset();
	GWoTDevRetailBuildState.Reset();
	GWoTDevRetailFamiliesAttempted = false;
	GWoTDevRetailFamiliesDecodeSucceeded = false;
	GWoTDevRetailFamiliesDecodedObjectCount = 0;
	GWoTDevRetailFamiliesCacheHitCount = 0;
	GWoTDevRetailFamiliesDecodeSeconds = 0.0;
	GWoTDevRetailFamiliesCacheRootForTesting.Reset();
}

void WoTDevRetailFamiliesSetCacheRootForTesting(FString CacheRoot)
{
	check(IsInGameThread());
	GWoTDevRetailFamiliesCacheRootForTesting = MoveTemp(CacheRoot);
}

bool WoTDevRetailFamiliesLastDecodeSucceededForTesting()
{
	return GWoTDevRetailFamiliesDecodeSucceeded;
}

int32 WoTDevRetailFamiliesLastDecodedObjectCountForTesting()
{
	return GWoTDevRetailFamiliesDecodedObjectCount;
}

int32 WoTDevRetailFamiliesLastCacheHitCountForTesting()
{
	return GWoTDevRetailFamiliesCacheHitCount;
}

double WoTDevRetailFamiliesLastDecodeSecondsForTesting()
{
	return GWoTDevRetailFamiliesDecodeSeconds;
}

bool WoTDevRetailFamiliesFindTextureForTesting(
	FStringView Identity, FWoTRetailTextureFacts& Out)
{
	return GWoTDevRetailBuildContext.IsValid()
		&& GWoTDevRetailBuildContext->FindTexture(Identity, Out);
}

bool WoTDevRetailFamiliesFindMeshSequencesForTesting(
	FStringView Identity, TArray<FWoTVertexMeshSequence>& Out)
{
	return GWoTDevRetailBuildContext.IsValid()
		&& GWoTDevRetailBuildContext->FindMeshSequences(Identity, Out);
}
#endif

#endif // !WOT_GUARDED_BOOTSTRAP
