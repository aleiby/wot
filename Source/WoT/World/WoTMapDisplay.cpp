#include "WoTMapDisplay.h"

#include "WoTMaterializedWorldHost.h"
#include "WoTWorldMaterializer.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/Package.h"

namespace
{
const AWoTMaterializedWorldRoot* WoTMapDisplayRoot(const UWorld* World)
{
	if (!IsValid(World)) return nullptr;
	for (TActorIterator<AWoTMaterializedWorldRoot> It(World); It; ++It)
	{
		if (IsValid(*It)) return *It;
	}
	return nullptr;
}
}

FString WoTWorldMapIdentity(const UWorld* World)
{
	if (!IsValid(World)) return FString();
	if (const AWoTMaterializedWorldRoot* Root = WoTMapDisplayRoot(World);
		Root != nullptr && !Root->MapIdentity.IsEmpty())
	{
		return Root->MapIdentity;
	}
	if (const UPackage* Package = World->GetOutermost())
	{
		if (const TOptional<FString> Adopted =
				WoTMapIdentityForGuardedShellPackage(Package->GetName());
			Adopted.IsSet())
		{
			return Adopted.GetValue();
		}
	}
	return UWorld::RemovePIEPrefix(World->GetMapName());
}

bool WoTWorldHasRetailLevelTitle(const UWorld* World)
{
	return WoTMapDisplayRoot(World) != nullptr;
}

FString WoTWorldDisplayTitle(const UWorld* World)
{
	if (const AWoTMaterializedWorldRoot* Root = WoTMapDisplayRoot(World))
	{
		return Root->LevelTitle;
	}
	return WoTWorldMapIdentity(World);
}

FString WoTDisplayMapForTravelURLMap(const FString& UrlMap)
{
	if (UrlMap.IsEmpty()) return UrlMap;
	const TOptional<FString> Adopted = WoTMapIdentityForGuardedShellPackage(UrlMap);
	return Adopted.IsSet() ? Adopted.GetValue() : UrlMap;
}
