#pragma once
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotNavigation.h"
#include "WoTBotSoakReport.h"
#include "AI/Navigation/NavigationDirtyArea.h"
#include "AI/Navigation/NavigationElement.h"
#include "Components/ActorComponent.h"
#include "NavMesh/RecastNavMeshGenerator.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"

namespace WoTBotSoak
{
// UE 5.8 dispatches this queue to both meshes. Record each area once, before
// dispatch, and correlate completed bot tiles with outstanding source bounds.
// Correlation is inclusive: overlapping sources share credit, not exclusive cost.
struct FNavProbe
{
	UWorld* World;
	FDelegateHandle AreasHandle, TilesHandle;
	TMap<FString, FObject> Sources;
	TMap<FIntPoint, TSet<FString>> Pending;
	TMap<FNavTileRef, FIntPoint> TileLocations;
	FString Events = TEXT("seconds\tsource\tclass\towner_class\tflags\tmin\tmax\n");
	int32 Areas = 0, Tiles = 0, Unattributed = 0;
	double CallbackSeconds = 0;
	explicit FNavProbe(UWorld* W) : World(W)
	{
		AreasHandle = WoTBotNavigation::DirtyAreasObserved.AddLambda([this](UWorld* W, const TArray<FNavigationDirtyArea>& Batch)
		{
			if (W != World) return;
			const double Started = FPlatformTime::Seconds();
			ON_SCOPE_EXIT { CallbackSeconds += FPlatformTime::Seconds() - Started; };
			auto* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
			auto* Mesh = Nav ? Cast<AWoTBotNavMesh>(Nav->GetNavDataForAgentName(TEXT("ArenaBot"))) : nullptr;
			if (Mesh && TileLocations.IsEmpty())
			{
				// The soak's initial synchronous build does not emit tile callbacks.
				TArray<FNavTileRef> Existing; Mesh->GetAllNavMeshTiles(Existing);
				for (FNavTileRef Tile : Existing)
				{
					int32 X, Y, Layer;
					if (Mesh->GetNavMeshTileXY(Tile, X, Y, Layer)) TileLocations.Add(Tile, FIntPoint(X,Y));
				}
			}
			for (const auto& Area : Batch)
			{
				const UObject* Source = Area.OptionalSourceElement ? Area.OptionalSourceElement->GetWeakUObject().Get(true) : nullptr;
				const UActorComponent* Component = Cast<UActorComponent>(Source);
				const AActor* Owner = Component ? Component->GetOwner() : Cast<AActor>(Source);
				const FString Name = Source ? Source->GetPathName() : Area.GetSourceDescription();
				const FString Class = Source ? Source->GetClass()->GetPathName() : TEXT("unknown");
				const FString OwnerClass = Owner ? Owner->GetClass()->GetPathName() : TEXT("none");
				FObject* Found = Sources.Find(Name);
				if (!Found)
				{
					FObject Row = Object(); Row->SetStringField(TEXT("source"), Name);
					Row->SetStringField(TEXT("class"), Class); Row->SetStringField(TEXT("owner_class"), OwnerClass);
					Sources.Add(Name, Row); Found = Sources.Find(Name);
				}
				Increment(*Found, TEXT("dirty_areas")); ++Areas;
				if (W->GetTimeSeconds() >= 30) Increment(*Found, TEXT("steady_dirty_areas"));
				Events += FString::Printf(TEXT("%.3f\t%s\t%s\t%s\t%d\t%s\t%s\n"), W->GetTimeSeconds(),
					*Name, *Class, *OwnerClass, int32(Area.Flags), *Area.Bounds.Min.ToString(), *Area.Bounds.Max.ToString());
				if (!Mesh || !Mesh->GetGenerator()) continue;
				const auto* Generator = static_cast<const FRecastNavMeshGenerator*>(Mesh->GetGenerator());
				const auto& Config = Generator->GetConfig();
				FBox Bounds = Area.Bounds;
				if (!Area.HasFlag(ENavigationDirtyFlag::NavigationBounds))
				{
					if (!Bounds.Intersect(Generator->GetTotalBounds())) continue;
					Bounds = Bounds.Overlap(Generator->GetTotalBounds());
					// Match UE 5.8 MarkDirtyTiles/GrowDirtyBounds, including Recast's
					// inverted axes and optional legacy doubled border expansion.
					const bool Tight = IConsoleManager::Get().FindConsoleVariable(TEXT("ai.nav.UseTightBoundExpansion"))->GetBool();
					const double Low = Config.borderSize.high * Config.cs * (Tight ? 1 : 2);
					const double High = Config.borderSize.low * Config.cs * (Tight ? 1 : 2);
					Bounds.Min -= FVector(High, High, Tight ? Config.cs : High);
					Bounds.Max += FVector(Low, Low, Tight ? Config.cs : Low);
					if (Area.HasFlag(ENavigationDirtyFlag::UseAgentHeight)) Bounds.Min.Z -= Config.AgentHeight;
				}
				int32 X0, Y0, X1, Y1;
				if (!Mesh->GetNavMeshTileXY(Bounds.Min, X0, Y0) || !Mesh->GetNavMeshTileXY(Bounds.Max, X1, Y1)) continue;
				for (int32 X = FMath::Min(X0, X1); X <= FMath::Max(X0, X1); ++X)
					for (int32 Y = FMath::Min(Y0, Y1); Y <= FMath::Max(Y0, Y1); ++Y)
						Pending.FindOrAdd(FIntPoint(X,Y)).Add(Name);
			}
		});
		TilesHandle = WoTBotNavigation::TilesObserved.AddLambda([this](AWoTBotNavMesh* Mesh, const TArray<FNavTileRef>& Changed)
		{
			if (Mesh->GetWorld() != World) return;
			const double Started = FPlatformTime::Seconds();
			ON_SCOPE_EXIT { CallbackSeconds += FPlatformTime::Seconds() - Started; };
			TSet<FIntPoint> Completed;
			for (FNavTileRef Tile : Changed)
			{
				++Tiles; int32 X, Y, Layer;
				FIntPoint Key;
				if (Mesh->GetNavMeshTileXY(Tile, X, Y, Layer))
				{
					Key = FIntPoint(X,Y); TileLocations.Add(Tile, Key);
				}
				else if (const FIntPoint* Previous = TileLocations.Find(Tile))
				{
					Key = *Previous; TileLocations.Remove(Tile);
				}
				else { ++Unattributed; continue; }
				if (const TSet<FString>* Names = Pending.Find(Key))
					for (const FString& Name : *Names)
					{
						Increment(Sources.FindChecked(Name), TEXT("correlated_tile_updates"));
						if (World->GetTimeSeconds() >= 30) Increment(Sources.FindChecked(Name), TEXT("steady_correlated_tile_updates"));
					}
				else ++Unattributed;
				Completed.Add(Key);
			}
			// A rebuild reports removed refs plus replacements and all vertical
			// layers. Retain the contributors until the whole callback is counted.
			for (const FIntPoint& Key : Completed) Pending.Remove(Key);
		});
	}
	~FNavProbe()
	{
		WoTBotNavigation::DirtyAreasObserved.Remove(AreasHandle);
		WoTBotNavigation::TilesObserved.Remove(TilesHandle);
	}
	void Snapshot(const FObject& Report, const FString& Output)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const auto& Pair : Sources) Rows.Add(Value(Pair.Value));
		Report->SetArrayField(TEXT("navigation_dirty_sources"), Rows);
		Report->SetNumberField(TEXT("navigation_dirty_areas"), Areas);
		Report->SetNumberField(TEXT("navigation_observed_tile_updates"), Tiles);
		Report->SetNumberField(TEXT("navigation_unattributed_tile_updates"), Unattributed);
		Report->SetNumberField(TEXT("navigation_probe_callback_ms"), CallbackSeconds * 1000);
		FFileHelper::SaveStringToFile(Events, *(Output + TEXT(".dirty.tsv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
};
}
#endif
