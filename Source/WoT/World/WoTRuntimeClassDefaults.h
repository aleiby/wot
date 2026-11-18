#pragma once

#include "CoreMinimal.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"

struct WOT_API FWoTRuntimeClassDefaultRecord
{
	FString Identity;
	uint32 ExportIndex = 0;
	FString SuperclassIdentity;
	bool bPlaceholder = false;
	TArray<FWoTRuntimeWorldProperty> Properties;
};

/** Generation-owned decoded WOTDEF3 class-default graph. */
class WOT_API FWoTRuntimeClassDefaults final
{
public:
	FWoTRuntimeClassDefaults() = default;
	FWoTRuntimeClassDefaults(const FWoTRuntimeClassDefaults&) = delete;
	FWoTRuntimeClassDefaults& operator=(const FWoTRuntimeClassDefaults&) = delete;

	bool LoadActiveGeneration(FString& OutError);
	bool LoadFixture(TArray<FWoTRuntimeClassDefaultRecord> Records,
		FString& OutError);
#if !WOT_GUARDED_BOOTSTRAP
	bool LoadObjectsForDevelopment(
		TArray<FWoTRuntimeVerifiedObject> Objects,
		FString& OutError);
#endif
#if WITH_AUTOMATION_TESTS
	bool LoadObjectsForTesting(TArray<FWoTRuntimeVerifiedObject> Objects,
		FString& OutError);
	bool SetClassStructIntegerForTesting(FStringView ClassIdentity,
		FStringView PropertyName, uint32 ArrayIndex, FStringView MemberName,
		int32 Value);
	void ResetRelationshipResolutionCountForTesting() const
	{
		RelationshipResolutionCountForTesting = 0;
		RelatedClassLeavesCache.Reset();
	}
	int32 GetRelationshipResolutionCountForTesting() const
	{
		return RelationshipResolutionCountForTesting;
	}
#endif
	bool IsLoaded() const { return bLoaded; }

	const FWoTRuntimeWorldProperty* FindProperty(
		const FWoTRuntimeWorldActor& Actor, FStringView Name,
		uint32 ArrayIndex = 0) const;
	const FWoTRuntimeWorldProperty* FindClassProperty(
		FStringView ClassIdentity, FStringView Name,
		uint32 ArrayIndex = 0) const;
	/** A property serialized on this class itself, without walking its parents. */
	const FWoTRuntimeWorldProperty* FindOwnClassProperty(
		FStringView ClassIdentity, FStringView Name,
		uint32 ArrayIndex = 0) const;
	bool ClassIsA(FStringView ClassIdentity, FStringView BaseIdentity) const;
	bool GetClassChain(FStringView ClassIdentity,
		TArray<FString>& OutMostSpecificFirst) const;
	/** Every decoded class equal to, or derived from, BaseIdentity. */
	void GetDerivedClassIdentities(FStringView BaseIdentity,
		TArray<FString>& OutIdentities) const;
	/** Resolve a native port leaf (e.g. AngrealInvDart) only when unique. */
	bool FindClassIdentityByLeaf(FStringView Leaf, FString& OutIdentity) const;
	/** Symmetric ancestry relationship, cached by the native creature row pair. */
	bool ClassesAreRelatedByLeaf(FName FirstLeaf, FName SecondLeaf) const;
	int32 NumClasses() const { return RecordsByFoldedIdentity.Num(); }

private:
	bool LoadObjects(TArray<FWoTRuntimeVerifiedObject> Objects,
		const TCHAR* InvalidInputError, FString& OutError);
	bool AdoptRecords(TArray<FWoTRuntimeClassDefaultRecord> Records,
		FString& OutError);

	// The builder disambiguates a Class export whose Pkg.Name collides with
	// another export (e.g. Angreal's EarthTremor class vs its EarthTremor
	// sound group) as `Pkg.Name#export-NNNNNNNN`, while cross-package imports
	// (WoTPawns.LegionEarthTremor's superclass) and world actor class refs
	// carry the plain `Pkg.Name`. Unique disambiguated classes are aliased.
	const FWoTRuntimeClassDefaultRecord* FindRecord(FStringView Identity) const;
	static FString PublicIdentity(const FString& Identity);

	TMap<FString, FWoTRuntimeClassDefaultRecord> RecordsByFoldedIdentity;
	TMap<FString, FString> FoldedAliasToFoldedIdentity;
	TMap<FName, FString> PublicIdentityByLeaf;
	TMap<FString, TSet<FString>> FoldedAncestorsByFoldedIdentity;
	mutable TMap<FName, TMap<FName, bool>> RelatedClassLeavesCache;
	TArray<FWoTRuntimeVerifiedObject> VerifiedObjects;
	bool bLoaded = false;
#if WITH_AUTOMATION_TESTS
	mutable int32 RelationshipResolutionCountForTesting = 0;
#endif
};
