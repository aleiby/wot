#pragma once

#include "CoreMinimal.h"

class FWoTRuntimeWorldResource;

// Small, widget-free representation used to keep the Asset Registry filtering
// policy independently testable. The runtime query converts FAssetData into
// these candidates before applying the same filter as the specs.
struct FWoTMapAssetCandidate
{
	FString PackageName;
	FString AssetName;
	bool bIsWorld = false;
	bool bIsTopLevel = false;
};

struct FWoTFrontEndMapEntry
{
	FString PackageName;
	FString ShortName;
	bool bIsBattle = false;
	FString DisplayName;
};

class FWoTFrontEndMapCatalog
{
public:
	// Guarded builds: the adopted world authority's authenticated descriptor
	// identities (hosting routes each through its neutral shell). Otherwise
	// query all assets below /Game/WoT/Maps; only top-level UWorld assets whose
	// packages pass FWoTFrontEndModel::IsPlayableMap survive.
	static TArray<FWoTFrontEndMapEntry> Enumerate();

	// Pure seam: short identities ("Arena_01") -> the same filtered catalog the
	// cooked-asset path produces, under the retail package convention.
	static TArray<FWoTFrontEndMapEntry> FromIdentities(
		TConstArrayView<FString> Identities);

	// Read Engine.LevelInfo.Title from the authenticated retail world payload.
	// Falls back to the cache identity when the payload/title is unavailable.
	static FString ResolveRetailTitle(const FString& MapIdentity);
	/** Last authenticated title resolved while the front end held a live
	 * payload lease; safe to use after a map-read failure revoked that lease. */
	static FString CachedRetailTitle(const FString& MapIdentity);
	/** Same production decoder after the authenticated world object is loaded. */
	static FString ResolveRetailTitleFromAuthenticatedWorld(
		const FWoTRuntimeWorldResource& Resource, const FString& MapIdentity);

	// Pure filtering/sorting seam for headless automation.
	static TArray<FWoTFrontEndMapEntry> FilterAndSort(
		const TArray<FWoTMapAssetCandidate>& Candidates);
};
