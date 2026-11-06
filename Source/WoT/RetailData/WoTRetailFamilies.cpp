#include "WoTRetailFamilies.h"
#include "WoTRetailFamilyPins.h"

#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCWeaponData.h"
#include "../Traps/WoTFireWall.h"
#include "../WoTAnimTable.h"
#include "../WoTBloodFX.h"
#include "../WoTCreatureChunk.h"
#include "../WoTFootsteps.h"
#include "../WoTDecal.h"
#include "../WoTMeshFX.h"
#include "../WoTSoundTable.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTWeaveFX.h"
#include "../World/WoTRuntimeClassDefaults.h"

namespace
{
enum class EWoTRetailFamilyActivationState : uint8
{
	NeverAttempted,
	InProgress,
	Succeeded,
	Failed,
};

EWoTRetailFamilyActivationState GWoTRetailFamilyActivationState =
	EWoTRetailFamilyActivationState::NeverAttempted;
FString GWoTRetailFamilyActivationError;
#if WITH_AUTOMATION_TESTS
TOptional<TArray<FWoTRetailFamily>> GWoTRetailFamiliesForTesting;
TOptional<TMap<FString, FString>> GWoTRetailFamilyPinsForTesting;
TMap<FString, FString> GWoTRetailFamilyObservedDigestsForTesting;
#endif
}

TConstArrayView<FWoTRetailFamily> WoTRetailFamilies()
{
#if WITH_AUTOMATION_TESTS
	if (GWoTRetailFamiliesForTesting.IsSet())
	{
		return GWoTRetailFamiliesForTesting.GetValue();
	}
#endif
	static const TArray<FWoTRetailFamily> Families = {
		{FWoTAnimTableBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTAnimSet> Set = MakeShared<FWoTAnimSet>();
				if (!FWoTAnimTableBuilder::Build(Context, *Set, OutError))
					return false;
				OutDigest = FWoTAnimTableBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTAnimTableBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
		{FWoTSoundTableBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTSoundSet> Set = MakeShared<FWoTSoundSet>();
				if (!FWoTSoundTableBuilder::Build(Context, *Set, OutError))
					return false;
				OutDigest = FWoTSoundTableBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTSoundTableBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
		{FWoTUE1LightRegistryBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTUE1LightSet> Set = MakeShared<FWoTUE1LightSet>();
				if (!FWoTUE1LightRegistryBuilder::Build(
					Context, *Set, OutError)) return false;
				OutDigest = FWoTUE1LightRegistryBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTUE1LightRegistryBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
		{FWoTNPCWeaponDataBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTNPCWeaponSet> Set = MakeShared<FWoTNPCWeaponSet>();
				if (!FWoTNPCWeaponDataBuilder::Build(Context, *Set, OutError))
					return false;
				OutDigest = FWoTNPCWeaponDataBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTNPCWeaponDataBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
		{FWoTCreatureChunkBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTCreatureChunkSet> Set =
					MakeShared<FWoTCreatureChunkSet>();
				if (!FWoTCreatureChunkBuilder::Build(Context, *Set, OutError))
					return false;
				OutDigest = FWoTCreatureChunkBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTCreatureChunkBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
		{FWoTCreatureDataBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTCreatureSet> Set = MakeShared<FWoTCreatureSet>();
				if (!FWoTCreatureDataBuilder::Build(Context, *Set, OutError))
					return false;
				OutDigest = FWoTCreatureDataBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTCreatureDataBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
		{FWoTBloodFXBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTBloodSet> Set = MakeShared<FWoTBloodSet>();
				if (!FWoTBloodFXBuilder::Build(Context, *Set, OutError))
					return false;
				OutDigest = FWoTBloodFXBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTBloodFXBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
		{FWoTFireWallBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTFireWallSet> Set = MakeShared<FWoTFireWallSet>();
				if (!FWoTFireWallBuilder::Build(Context, *Set, OutError))
					return false;
				OutDigest = FWoTFireWallBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTFireWallBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
		{FWoTFootstepsBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTFootstepSet> Set = MakeShared<FWoTFootstepSet>();
				if (!FWoTFootstepsBuilder::Build(Context, *Set, OutError))
					return false;
				OutDigest = FWoTFootstepsBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTFootstepsBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
		{FWoTWeaveFXBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTWeaveFXSet> Set = MakeShared<FWoTWeaveFXSet>();
				if (!FWoTWeaveFXBuilder::Build(Context, *Set, OutError))
					return false;
				OutDigest = FWoTWeaveFXBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTWeaveFXBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
		{FWoTMeshFXBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTMeshFXSet> Set = MakeShared<FWoTMeshFXSet>();
				if (!FWoTMeshFXBuilder::Build(Context, *Set, OutError))
					return false;
				OutDigest = FWoTMeshFXBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTMeshFXBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
		{FWoTDecalBuilder::FamilyName,
			[](const FWoTRetailBuildContext& Context, FString& OutDigest,
				TFunction<void()>& OutInstall, FString& OutError)
			{
				TSharedRef<FWoTDecalSet> Set = MakeShared<FWoTDecalSet>();
				if (!FWoTDecalBuilder::Build(Context, *Set, OutError))
					return false;
				OutDigest = FWoTDecalBuilder::CanonicalDigest(*Set);
				OutInstall = [Set]() mutable
				{
					FWoTDecalBuilder::Install(MoveTemp(*Set));
				};
				return true;
			}},
	};
	return Families;
}

const TMap<FString, FString>& WoTRetailFamilyPins()
{
#if WITH_AUTOMATION_TESTS
	if (GWoTRetailFamilyPinsForTesting.IsSet())
	{
		return GWoTRetailFamilyPinsForTesting.GetValue();
	}
#endif
	// Canonical SHA-256 of each family built from the pinned retail installer's
	// class defaults (a publishable fact, not retail data). Recorded from
	// WoT.RetailData.RealRetailFamilyDigests and identical on MSVC and Apple
	// Clang, which proves the canonical serialization is compiler-independent.
	// A rebuilt table that differs fails payload activation.
	static const TMap<FString, FString> Pins = {
		{TEXT("anim-tables"),
			TEXT("fe9dfe7f9a9bbd03190b6c81cc8448f80115b63fb74f55503c6f88ecb91d377d")},
		{TEXT("sound-tables"),
			TEXT("17e4f832c2d2febbcea3e92c3201fd1dc2b21ff18bc87421a4c118d82fff510b")},
		{TEXT("ue1-lights"),
			TEXT("af79d568856c043129b937aa16fe092c3feeadc93ead83502a3eea0ec6d87022")},
		{TEXT("npc-weapons"),
			TEXT("9dd0c2947406c4ab0395d2e2909d2bd3c443f82eee7e2528e66784a971a18187")},
		{TEXT("creature-chunks"),
			TEXT("fe149d9336f39920bcda6e8d0d20a07bc56fe07375e4b8602ae046d5b56f8301")},
		{TEXT("creature-data"),
			TEXT("584784d962e48f39aa5eab06f4891f39c4a8551da4d0e1e02915b9acb6770ed2")},
		{TEXT("blood-fx"),
			TEXT("fd2e81115297f46323d13070e9c840edf1c1987b9cf3a431403eb03cfda3641b")},
		{TEXT("fire-wall"),
			TEXT("5d5c61571d988d5edb00aac0b9570b3d85bd471e2305e8acf6c04aa803038042")},
		{TEXT("footsteps"),
			TEXT("55c8b97dcc1e002d61f8e401b6c1613fcd66111e77470c3f57ac244e725832e6")},
		{TEXT("weave-fx"),
			TEXT("e4bb13bfd79fd3041fb07c26b84eed537a1bf8c94be9139faa881714e9349814")},
		{TEXT("mesh-fx"),
			TEXT("bd665de0bfa1004691aeafc5d18d94e55512f9b7a3bdf2620ba8765276f6aed6")},
		{TEXT("decal"),
			TEXT("a290edf1b57986977477d3f003f16c24827eece02fafef73aa92752e7f151a40")},
	};
	return Pins;
}

bool WoTActivateRetailFamilies(
	const FWoTRetailBuildContext& Context,
	TConstArrayView<FWoTRetailFamily> Families,
	const TMap<FString, FString>& Pins,
	FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (GWoTRetailFamilyActivationState
		== EWoTRetailFamilyActivationState::Succeeded)
	{
		return true;
	}
	if (GWoTRetailFamilyActivationState
		== EWoTRetailFamilyActivationState::Failed)
	{
		OutError = GWoTRetailFamilyActivationError;
		return false;
	}
	if (GWoTRetailFamilyActivationState
		== EWoTRetailFamilyActivationState::InProgress)
	{
		OutError = TEXT("retail-family activation was re-entered");
		return false;
	}

	GWoTRetailFamilyActivationState =
		EWoTRetailFamilyActivationState::InProgress;
#if WITH_AUTOMATION_TESTS
	GWoTRetailFamilyObservedDigestsForTesting.Reset();
#endif
	TArray<TFunction<void()>> Installs;
	Installs.Reserve(Families.Num());
	TArray<FString> Digests;
	Digests.Reserve(Families.Num());
	// Every failure is kept: a packaged build can only report this text.
	TArray<FString> Failures;
	TMap<FString, FWoTRetailTextureFacts> TextureFacts;
	TSet<FString> MissingTextures;
	TMap<FString, TArray<FWoTVertexMeshSequence>> MeshSequences;
	TSet<FString> MissingMeshes;
	const FWoTRetailBuildContext MemoizedContext{
		Context.ClassDefaults,
		[&Context, &TextureFacts, &MissingTextures](
			FStringView Identity, FWoTRetailTextureFacts& Out)
		{
			const FString Key = FString(Identity).ToLower();
			if (const FWoTRetailTextureFacts* Existing = TextureFacts.Find(Key))
			{
				Out = *Existing;
				return true;
			}
			if (MissingTextures.Contains(Key) || !Context.FindTexture)
			{
				return false;
			}
			FWoTRetailTextureFacts Decoded;
			if (!Context.FindTexture(Identity, Decoded))
			{
				MissingTextures.Add(Key);
				return false;
			}
			TextureFacts.Add(Key, Decoded);
			Out = Decoded;
			return true;
		},
		[&Context, &MeshSequences, &MissingMeshes](FStringView Identity,
			TArray<FWoTVertexMeshSequence>& Out)
		{
			const FString Key = FString(Identity).ToLower();
			if (const TArray<FWoTVertexMeshSequence>* Existing =
				MeshSequences.Find(Key))
			{
				Out = *Existing;
				return true;
			}
			if (MissingMeshes.Contains(Key) || !Context.FindMeshSequences)
			{
				return false;
			}
			TArray<FWoTVertexMeshSequence> Decoded;
			if (!Context.FindMeshSequences(Identity, Decoded))
			{
				MissingMeshes.Add(Key);
				return false;
			}
			MeshSequences.Add(Key, Decoded);
			Out = MoveTemp(Decoded);
			return true;
		}};

	for (const FWoTRetailFamily& Family : Families)
	{
		const FString FamilyName = Family.Name != nullptr
			? FString(Family.Name) : FString();
		FString Digest;
		FString BuildError;
		TFunction<void()> Install;
		if (FamilyName.IsEmpty() || !Family.Build)
		{
			Failures.Add(FamilyName.IsEmpty()
				? TEXT("a retail family has no name")
				: FString::Printf(
					TEXT("retail family '%s' has no builder"), *FamilyName));
		}
		else if (!Family.Build(
			MemoizedContext, Digest, Install, BuildError))
		{
			Failures.Add(FString::Printf(
				TEXT("retail family '%s' failed to build: %s"),
				*FamilyName,
				BuildError.IsEmpty() ? TEXT("no detail") : *BuildError));
		}
		else if (!Install)
		{
			Failures.Add(FString::Printf(
				TEXT("retail family '%s' produced no install closure"),
				*FamilyName));
		}
#if WITH_AUTOMATION_TESTS
		if (!FamilyName.IsEmpty() && !Digest.IsEmpty())
		{
			GWoTRetailFamilyObservedDigestsForTesting.Add(
				FamilyName, Digest);
		}
#endif
		Digests.Add(MoveTemp(Digest));
		Installs.Add(MoveTemp(Install));
	}

	for (int32 Index = 0; Index < Families.Num(); ++Index)
	{
		const FWoTRetailFamily& Family = Families[Index];
		const FString FamilyName = Family.Name != nullptr
			? FString(Family.Name) : FString();
		const FString* Expected = Pins.Find(FamilyName);
		if (Expected == nullptr)
		{
			Failures.Add(FString::Printf(
				TEXT("retail family '%s' has no pinned digest; observed '%s'"),
				*FamilyName, *Digests[Index]));
		}
		else if (!Digests[Index].Equals(*Expected, ESearchCase::CaseSensitive))
		{
			Failures.Add(FString::Printf(
				TEXT("retail family '%s' digest mismatch: expected '%s', observed '%s'"),
				*FamilyName, **Expected, *Digests[Index]));
		}
	}

	if (!Failures.IsEmpty())
	{
		GWoTRetailFamilyActivationError = FString::Join(Failures, TEXT("; "));
		GWoTRetailFamilyActivationState =
			EWoTRetailFamilyActivationState::Failed;
		OutError = GWoTRetailFamilyActivationError;
		return false;
	}

	for (TFunction<void()>& Install : Installs)
	{
		Install();
	}
	GWoTRetailFamilyActivationError.Reset();
	GWoTRetailFamilyActivationState =
		EWoTRetailFamilyActivationState::Succeeded;
	return true;
}

#if WITH_AUTOMATION_TESTS
void WoTRetailFamiliesSetActivationInputsForTesting(
	TArray<FWoTRetailFamily> Families,
	TMap<FString, FString> Pins)
{
	check(IsInGameThread());
	GWoTRetailFamiliesForTesting = MoveTemp(Families);
	GWoTRetailFamilyPinsForTesting = MoveTemp(Pins);
}

bool WoTRetailFamiliesOverriddenForTesting()
{
	return GWoTRetailFamiliesForTesting.IsSet();
}

void WoTRetailFamiliesResetActivationForTesting()
{
	check(IsInGameThread());
	GWoTRetailFamilyActivationState =
		EWoTRetailFamilyActivationState::NeverAttempted;
	GWoTRetailFamilyActivationError.Reset();
	GWoTRetailFamiliesForTesting.Reset();
	GWoTRetailFamilyPinsForTesting.Reset();
	GWoTRetailFamilyObservedDigestsForTesting.Reset();
}

const TMap<FString, FString>&
WoTRetailFamilyObservedDigestsForTesting()
{
	return GWoTRetailFamilyObservedDigestsForTesting;
}
#endif
