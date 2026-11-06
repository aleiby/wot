#pragma once

#include "CoreMinimal.h"
#include "../Payload/WoTNeutralResources.h"
#include "Templates/Function.h"

/**
 * Exact cache identity of a retail object reference. Mirrors
 * Tools/dist/retail_identity.py, which the production contract generator uses:
 *
 *  - a name UNIQUE (case-folded) among one package's exports of a domain keeps
 *    the plain identity "<Package>__<Name>";
 *  - exports that share a name inside one package (retail: AesSedaiT.Ornament
 *    vs .SurfaceWall.PaprWall1441, AmbientA.Nature vs .Bush.Rustle3) each get
 *    their exact identity "<Package>.<Group>[.<Group>...]__<Name>"; a top-level
 *    colliding export's exact identity is the plain form (its UE1 path).
 *
 * Consumers resolve exact-first: the grouped exact identity when the
 * authenticated set holds it, else the plain one. UE1 names are FNames and
 * therefore case-insensitive in the original engine; a map's
 * import table may spell a package differently from the package's own export
 * table (Arena_06 imports "Naturet.Sky.Sky_340a"), so every source-export
 * comparison uses WoTRetailIdentityEquals.
 */
struct FWoTRetailObjectPath
{
	FString Package;
	TArray<FString> Groups;
	FString Name;
};

/** Parse "Pkg[.Group...].Name" dropping a trailing "#export-N"/"#import-N"
 * builder disambiguator. False for fewer than two or any empty component. */
WOT_API bool WoTParseRetailObjectPath(FStringView ObjectPath,
	FWoTRetailObjectPath& Out);

/** "Pkg__Name". */
WOT_API FString WoTRetailPlainLogicalBase(const FWoTRetailObjectPath& Path);

/** "Pkg.Group1.Group2__Name" (the plain base when there is no group). Group
 * names are escaped for the logical-id grammar: every byte outside
 * [A-Za-z0-9_] becomes "-XX" (FootStubA.(All).METL -> FootStubA.-28All-29__METL). */
WOT_API FString WoTRetailExactLogicalBase(const FWoTRetailObjectPath& Path);

/** The reversible logical-id spelling of one group name (see above). */
WOT_API FString WoTRetailEncodeGroup(FStringView Group);

/** "cache/texture/", "cache/sound/", "cache/procedural/", ... ("" otherwise). */
WOT_API FString WoTRetailLogicalIdPrefix(EWoTNeutralResourceKind Kind);

/** Exact-first resolution against an existence predicate over complete
 * logical ids ("cache/<domain>/<base>"). */
WOT_API FString WoTRetailResolveLogicalBase(EWoTNeutralResourceKind Kind,
	const FWoTRetailObjectPath& Path,
	TFunctionRef<bool(const FString& LogicalId)> Exists);

/** Exact-first resolution against the active authenticated registry. Without
 * an active lease (development builds, automation without a registry) this is
 * the plain base, so development behaviour is unchanged. */
WOT_API FString WoTRetailResolveLogicalBase(EWoTNeutralResourceKind Kind,
	const FWoTRetailObjectPath& Path);

/** The "/Game/WoT/<Sounds|Textures>/" sub path of a retail object: "Pkg/Name",
 * or "Pkg/Group/.../Name" when the exact grouped identity is the resolved one.
 * UWoTRuntimeAssets::ResolveCacheId maps both back to the same logical id. */
WOT_API FString WoTRetailAssetSubPath(EWoTNeutralResourceKind Kind,
	const FWoTRetailObjectPath& Path,
	TFunctionRef<bool(const FString& LogicalId)> Exists);
WOT_API FString WoTRetailAssetSubPath(EWoTNeutralResourceKind Kind,
	const FWoTRetailObjectPath& Path);

/** The texture-gateway sub path of a surface/skin texture that may be static
 * or procedural (UWoTRuntimeAssets serves both under /Game/WoT/Textures/):
 * grouped when either authenticated texture domain holds the exact identity. */
WOT_API FString WoTRetailTextureSubPath(const FWoTRetailObjectPath& Path);

/** True while an authenticated lease is active and the registry holds this
 * exact logical id (FWoTRuntimeRegistry::FindDescriptor, case-insensitive). */
WOT_API bool WoTRetailRegistryHolds(EWoTNeutralResourceKind Kind,
	const FString& LogicalId);

/** "Pkg/Name" -> "Pkg__Name"; "Pkg/G1/G2/Name" -> "Pkg.G1.G2__Name". */
WOT_API bool WoTRetailLogicalBaseFromSubPath(FStringView SubPath, FString& OutBase);

/** UE1 FName equality of two identities (case-insensitive). */
WOT_API bool WoTRetailIdentityEquals(FStringView Left, FStringView Right);

/** An authenticated object's source export (which may carry the builder's
 * "#export-N" path disambiguator) against a retail reference path: equal
 * under FName case rules once that disambiguator is dropped. */
WOT_API bool WoTRetailSourceExportMatches(FStringView CachedSourceExport,
	FStringView ReferencePath);

/** An object-name-safe spelling of a logical base: every character outside
 * [A-Za-z0-9_] becomes '_' (plain Pkg__Name bases are unchanged). */
WOT_API FString WoTRetailObjectNameSafe(FStringView LogicalBase);

/** The reversible texture suffix of a derived material (MI_PSAdd_<suffix>,
 * MI_PSMod_<suffix>, ...): the plain "Pkg__Name", or for a grouped texture
 * sub path the exact logical base with each '.' spelled "-2E" (the group
 * escape of '.'), which keeps it an object-name-safe, unambiguous spelling:
 * ParticleSystems.Blood.Blood01 -> "ParticleSystems-2EBlood__Blood01". */
WOT_API FString WoTRetailDerivedMaterialSuffix(const FWoTRetailObjectPath& Path,
	bool bGrouped);

/** Inverse of WoTRetailDerivedMaterialSuffix: the texture-gateway sub path
 * ("Pkg/Name" or "Pkg/Group/.../Name", groups decoded). False when malformed. */
WOT_API bool WoTRetailTextureSubPathFromDerivedSuffix(FStringView Suffix,
	FString& OutSubPath);
