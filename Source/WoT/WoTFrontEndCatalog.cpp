#include "WoTFrontEndCatalog.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "HAL/PlatformProperties.h"
#include "Modules/ModuleManager.h"
#include "Payload/WoTRuntimeWorldResource.h"
#include "WoTFrontEnd.h"
#include "World/WoTRuntimeWorldProperties.h"
#include "World/WoTWorldMaterializer.h"

namespace
{
TMap<FString, FString>& WoTRetailTitleCache()
{
	static TMap<FString, FString>* Cache = new TMap<FString, FString>();
	return *Cache;
}
}

TArray<FWoTFrontEndMapEntry> FWoTFrontEndMapCatalog::Enumerate()
{
	// The neutral cook ships no /Game/WoT/Maps packages; the playable set is
	// the authenticated descriptor set adopted from the player's own cache.
	const TArray<FString> Adopted = WoTAdoptedWorldMapIdentities();
	if (!Adopted.IsEmpty())
	{
		return FromIdentities(Adopted);
	}
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	if (!FPlatformProperties::RequiresCookedData())
	{
		// Uncooked -game can reach Play before the asynchronous registry gatherer
		// has visited content. This bounded path scan makes that first query ready;
		// cooked builds use the registry serialized into the cooked package.
		AssetRegistry.ScanPathsSynchronous({TEXT("/Game/WoT/Maps")});
	}
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssetsByPath(
		FName(TEXT("/Game/WoT/Maps")), Assets, /*bRecursive*/ true);

	TArray<FWoTMapAssetCandidate> Candidates;
	Candidates.Reserve(Assets.Num());
	const FTopLevelAssetPath WorldClassPath = UWorld::StaticClass()->GetClassPathName();
	for (const FAssetData& Asset : Assets)
	{
		FWoTMapAssetCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.PackageName = Asset.PackageName.ToString();
		Candidate.AssetName = Asset.AssetName.ToString();
		Candidate.bIsWorld = Asset.AssetClassPath == WorldClassPath;
		Candidate.bIsTopLevel = Asset.IsTopLevelAsset();
	}
	return FilterAndSort(Candidates);
}

TArray<FWoTFrontEndMapEntry> FWoTFrontEndMapCatalog::FromIdentities(
	TConstArrayView<FString> Identities)
{
	TArray<FWoTMapAssetCandidate> Candidates;
	Candidates.Reserve(Identities.Num());
	for (const FString& Identity : Identities)
	{
		FWoTMapAssetCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.PackageName = TEXT("/Game/WoT/Maps/") + Identity;
		Candidate.AssetName = Identity;
		Candidate.bIsWorld = true;
		Candidate.bIsTopLevel = true;
	}
	TArray<FWoTFrontEndMapEntry> Result = FilterAndSort(Candidates);
	for (FWoTFrontEndMapEntry& Entry : Result)
	{
		Entry.DisplayName = ResolveRetailTitle(Entry.ShortName);
	}
	return Result;
}

FString FWoTFrontEndMapCatalog::ResolveRetailTitle(const FString& MapIdentity)
{
	FWoTRuntimeWorldResource Resource;
	const FWoTNeutralResourceId Id{
		FString::Printf(TEXT("cache/world/%s"), *MapIdentity),
		EWoTNeutralResourceKind::World};
	if (Resource.LoadFirstUse(Id).IsError())
	{
		return MapIdentity;
	}
	const FString Title =
		ResolveRetailTitleFromAuthenticatedWorld(Resource, MapIdentity);
	WoTRetailTitleCache().Add(MapIdentity, Title);
	return Title;
}

FString FWoTFrontEndMapCatalog::CachedRetailTitle(const FString& MapIdentity)
{
	const FString* Title = WoTRetailTitleCache().Find(MapIdentity);
	return Title != nullptr && !Title->IsEmpty() ? *Title : MapIdentity;
}

FString FWoTFrontEndMapCatalog::ResolveRetailTitleFromAuthenticatedWorld(
	const FWoTRuntimeWorldResource& Resource, const FString& MapIdentity)
{
	for (const FWoTRuntimeWorldActor& Actor : Resource.Actors())
	{
		if (!Actor.ClassIdentity.Equals(
			TEXT("Engine.LevelInfo"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		for (const FWoTRuntimeWorldProperty& Property : Actor.Properties)
		{
			FString Title;
			if (Property.Name.Equals(TEXT("Title"), ESearchCase::IgnoreCase)
				&& WoTRuntimeWorldPropertyUE1String(Property, Title))
			{
				const FString Resolved = Title.IsEmpty() ? MapIdentity : Title;
				WoTRetailTitleCache().Add(MapIdentity, Resolved);
				return Resolved;
			}
		}
	}
	WoTRetailTitleCache().Add(MapIdentity, MapIdentity);
	return MapIdentity;
}

TArray<FWoTFrontEndMapEntry> FWoTFrontEndMapCatalog::FilterAndSort(
	const TArray<FWoTMapAssetCandidate>& Candidates)
{
	TArray<FWoTFrontEndMapEntry> Result;
	TSet<FString> SeenPackages;
	for (const FWoTMapAssetCandidate& Candidate : Candidates)
	{
		if (!Candidate.bIsWorld || !Candidate.bIsTopLevel
			|| !FWoTFrontEndModel::IsPlayableMap(Candidate.PackageName)
			|| Candidate.AssetName !=
				FWoTFrontEndModel::ShortMapName(Candidate.PackageName)
			|| SeenPackages.Contains(Candidate.PackageName))
		{
			continue;
		}

		SeenPackages.Add(Candidate.PackageName);
		FWoTFrontEndMapEntry& Entry = Result.AddDefaulted_GetRef();
		Entry.PackageName = Candidate.PackageName;
		Entry.ShortName = Candidate.AssetName;
		Entry.DisplayName = Candidate.AssetName;
		Entry.bIsBattle = FWoTFrontEndModel::IsBattleMap(Candidate.PackageName);
	}

	Result.Sort([](const FWoTFrontEndMapEntry& A, const FWoTFrontEndMapEntry& B)
	{
		const int32 NameOrder = A.ShortName.Compare(B.ShortName, ESearchCase::CaseSensitive);
		return NameOrder == 0
			? A.PackageName.Compare(B.PackageName, ESearchCase::CaseSensitive) < 0
			: NameOrder < 0;
	});
	return Result;
}
