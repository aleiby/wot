#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotMapFixture.h"
#include "WoTBotSoakReport.h"
#include "WoTBotNavProbe.h"
#include "WoTBotSoakServer.h"
#include "Testing/WoTSoakEvents.h"
#include "WoTRainCyd.h"
#include "WoTMover.h"
#include "PhysicsEngine/BodySetup.h"
#include "EngineUtils.h"
#include "GameFramework/PhysicsVolume.h"
#include "GameFramework/PlayerStart.h"
#include "Components/CapsuleComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "AI/WoTPathNode.h"
#include "Algo/BinarySearch.h"
#include "WoTBotRoster.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Crc.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"
namespace WoTBotSoak
{
// Scoped to this namespace, so the shared test helpers don't leak into
// the rest of the plugin's unity blob.
using namespace WoTBotTest;

namespace
{
constexpr float Step = 1.0f / 30.0f;
int32 MapSeed(const FString& Map) { return int32(FCrc::StrCrc32(*Map) & 0x7fffffff); }
FObject NewReport(const FString& Map, int32 Fill, double Seconds)
{
	FObject R = Object();
	R->SetStringField(TEXT("map"), Map);
	R->SetStringField(TEXT("result"), TEXT("INCOMPLETE"));
	R->SetNumberField(TEXT("seed"), MapSeed(Map));
	R->SetNumberField(TEXT("fill"), Fill);
	R->SetNumberField(TEXT("requested_seconds"), Seconds);
	R->SetNumberField(TEXT("simulated_seconds"), 0);
	R->SetBoolField(TEXT("frag_limit_end"), false);
	R->SetBoolField(TEXT("restart_requested"), false);
	R->SetBoolField(TEXT("reload_tested"), false);
	R->SetBoolField(TEXT("completed"), false);
	return R;
}

struct FObserver
{
	struct FBodyHistory
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		FDelegateHandle TransformHandle;
		FVector LastLocation = FVector::ZeroVector;
		float HalfHeight = 0, Radius = 0;
		bool bInside = false, bFloor = false, bPenetrating = false;
		TSharedPtr<FJsonObject> ActiveEvent;
	};
	TMap<TWeakObjectPtr<AWoTCharacter>, FBodyHistory> BodyHistories;
	TArray<TSharedPtr<FJsonValue>> FloorEvents;
	TArray<TSharedPtr<FJsonValue>> SwapEvents;
	TSet<FString> ObservedSwaps;
	FDelegateHandle SpawnHandle, BodyHandle;
	UWorld* World;
	FObject Report;
	FLogCapture& Logs;
	TMap<TWeakObjectPtr<AWoTBotController>, FObject> Bots;
	TMap<TWeakObjectPtr<AWoTAngrealSpawnPool>, FObject> Pools;
	TSet<TWeakObjectPtr<AActor>> NonFiniteActors, RunawayActors;
	TArray<TSharedPtr<FJsonValue>> Anomalies, Recoveries;
	TArray<double> TickTimes;
	FDelegateHandle Handle;
	double Time = 0;
	double LastSampleTime = 0;
	int32 LastClusterSecond = 0;
	TSet<TWeakObjectPtr<AWoTAngrealSpawnPool>> VisitedPools;
	TMap<TWeakObjectPtr<AWoTAngrealSpawnPool>, double> ClosestApproach;
	TArray<double> PairDistances;
	struct FGoalChange { double Time; FString Pool; EWoTBotState State; };
	TMap<TWeakObjectPtr<AWoTBotController>, TArray<FGoalChange>> GoalHistory;
	void RecordGoal(AWoTBotController* B)
	{
		auto& History = GoalHistory.FindOrAdd(B);
		const FString Goal = B->GetSeekingPool() ? B->GetSeekingPool()->GetName() : TEXT("none");
		if (History.IsEmpty() || History.Last().Pool != Goal || History.Last().State != B->GetBehaviourState())
			History.Add({Time, Goal, B->GetBehaviourState()});
		while (History.Num() > 1 && History[1].Time < Time - 5.0) History.RemoveAt(0);
	}
	FObserver(UWorld* W, const FObject& R, FLogCapture& L) : World(W), Report(R), Logs(L)
	{
		Report->SetNumberField(TEXT("inside_floor_count"), 0);
		Report->SetNumberField(TEXT("geometry_penetration_count"), 0);
		Report->SetObjectField(TEXT("swap_targets_by_class"), Object());
		Report->SetObjectField(TEXT("non_pawn_swap_targets_by_class"), Object());
		SpawnHandle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([this](AActor* A)
		{
			if (AWoTCharacter* Body = Cast<AWoTCharacter>(A)) Remember(Body, TEXT("spawn"));
		}));
		BodyHandle = WoTSoakEvents::BodyEvents().AddLambda([this](AActor* A, const TCHAR* Source)
		{
			if (A && A->GetWorld() == World)
			{
				// Existing swap scopes name the actual far end, including lost-lock
				// impacts. Count once even when a failed placement rolls back.
				if (FCString::Strcmp(Source, TEXT("swap source placed or rolled back")) == 0)
				{
					const FString Scope = WoTSoakEvents::BodySource();
					FString Prefix, TargetAndRotation, TargetName, Rotation;
					if (Scope.Split(TEXT("target="), &Prefix, &TargetAndRotation,
						ESearchCase::CaseSensitive, ESearchDir::FromEnd)
						&& TargetAndRotation.Split(TEXT(" rotation="), &TargetName, &Rotation))
					{
						const FString Key = FString::Printf(TEXT("%.6f:%s:%s"), World->GetTimeSeconds(), *A->GetName(), *TargetName);
						if (!ObservedSwaps.Contains(Key))
						{
							ObservedSwaps.Add(Key);
							for (TActorIterator<AActor> It(World); It; ++It)
								if (It->GetName() == TargetName)
								{
									const FString Class = It->GetClass()->GetName();
									Increment(Report->GetObjectField(TEXT("swap_targets_by_class")).ToSharedRef(), *Class);
									if (!It->IsA<APawn>()) Increment(Report->GetObjectField(TEXT("non_pawn_swap_targets_by_class")).ToSharedRef(), *Class);
									FObject Row = Object();
									Row->SetNumberField(TEXT("seconds"), Time);
									Row->SetStringField(TEXT("source"), A->GetName());
									Row->SetStringField(TEXT("target"), TargetName);
									Row->SetStringField(TEXT("target_class"), Class);
									Row->SetBoolField(TEXT("target_is_pawn"), It->IsA<APawn>());
									SwapEvents.Add(Value(Row));
									Report->SetArrayField(TEXT("swap_events"), SwapEvents);
									break;
								}
						}
					}
				}
				if (AWoTCharacter* Body = Cast<AWoTCharacter>(A)) Remember(Body, Source);
			}
		});
		Handle = WoTSoakEvents::Events().AddLambda([this](WoTSoakEvents::EKind Kind, AActor* Subject, AActor* Participant)
		{
			if (!Subject || Subject->GetWorld() != World) return;
			AWoTBotController* B = Cast<AWoTBotController>(Subject);
			if (!B) if (const APawn* P = Cast<APawn>(Participant)) B = Cast<AWoTBotController>(P->GetController());
			if (AWoTAngrealSpawnPool* P = Cast<AWoTAngrealSpawnPool>(Subject))
			{
				if (Kind == WoTSoakEvents::EKind::PoolStock) Increment(Pool(P), TEXT("stocked"));
				if (Kind == WoTSoakEvents::EKind::PoolCollect) Increment(Pool(P), TEXT("collected"));
			}
			if (!B) return;
			FObject Row = Bot(B);
			if (Kind == WoTSoakEvents::EKind::Pickup) Increment(Row, TEXT("pickups"));
			if (Kind == WoTSoakEvents::EKind::Cast)
			{
				const auto Casts = Row->GetObjectField(TEXT("casts"));
				Increment(Casts.ToSharedRef(), *Subject->GetClass()->GetPathName());
			}
			if (Kind == WoTSoakEvents::EKind::Recovery || Kind == WoTSoakEvents::EKind::SilentRespawn)
			{
				const TCHAR* Key = Kind == WoTSoakEvents::EKind::Recovery ? TEXT("recoveries") : TEXT("silent_respawns");
				Increment(Row, Key); Increment(Report, Key);
				FObject Event = Object();
				Event->SetStringField(TEXT("bot"), B->GetName());
				Event->SetStringField(TEXT("kind"), Key);
				Event->SetNumberField(TEXT("seconds"), Time);
				Event->SetStringField(TEXT("position"), Position(Participant ? Participant->GetActorLocation() : Subject->GetActorLocation()));
				DescribeStuck(Event, B);
				Recoveries.Add(Value(Event));
			}
		});
	}
	~FObserver()
	{
		WoTSoakEvents::Events().Remove(Handle);
		WoTSoakEvents::BodyEvents().Remove(BodyHandle);
		World->RemoveOnActorSpawnedHandler(SpawnHandle);
		for (const auto& Pair : BodyHistories)
			if (Pair.Key.IsValid()) Pair.Key->GetCapsuleComponent()->TransformUpdated.Remove(Pair.Value.TransformHandle);
	}
	static FVector CollisionAxis(UCapsuleComponent* Capsule)
	{
		const UBodySetup* Setup = Capsule->GetBodySetup();
		const FQuat Local = Setup && Setup->AggGeom.SphylElems.Num() == 1
			? Setup->AggGeom.SphylElems[0].Rotation.Quaternion() : FQuat::Identity;
		return Capsule->GetComponentQuat().RotateVector(Local.GetAxisZ());
	}
	void Remember(AWoTCharacter* Body, const TCHAR* Event)
	{
		UCapsuleComponent* Capsule = Body->GetCapsuleComponent();
		if (!Capsule) return;
		FBodyHistory& History = BodyHistories.FindOrAdd(Body);
		if (!History.TransformHandle.IsValid())
		{
			History.LastLocation = Body->GetActorLocation();
			History.TransformHandle = Capsule->TransformUpdated.AddLambda(
				[this, Body](USceneComponent*, EUpdateTransformFlags, ETeleportType)
				{ Remember(Body, TEXT("transform")); });
		}
		FObject Row = Object();
		Row->SetNumberField(TEXT("seconds"), Time);
		Row->SetStringField(TEXT("event"), Event);
		FString Source = WoTSoakEvents::BodySource();
		if (Source.IsEmpty() && Cast<UPrimitiveComponent>(Body->GetMovementBaseObject()) && Cast<UPrimitiveComponent>(Body->GetMovementBaseObject())->GetOwner()->IsA<AWoTMover>())
			Source = TEXT("movement on mover base");
		Row->SetStringField(TEXT("source"), Source.IsEmpty() ? TEXT("movement/unscoped") : Source);
		Row->SetStringField(TEXT("from"), History.LastLocation.ToString());
		Row->SetStringField(TEXT("position"), Body->GetActorLocation().ToString());
		Row->SetStringField(TEXT("velocity"), Body->GetVelocity().ToString());
		Row->SetStringField(TEXT("movement"), Body->GetCharacterMovement()->GetMovementName());
		Row->SetBoolField(TEXT("crouched"), Body->bIsCrouched);
		Row->SetStringField(TEXT("rotation"), Body->GetActorRotation().ToString());
		Row->SetStringField(TEXT("capsule_up"), CollisionAxis(Capsule).ToString());
		Row->SetNumberField(TEXT("capsule_vertical_extent"), Capsule->GetScaledCapsuleRadius() + (Capsule->GetScaledCapsuleHalfHeight() - Capsule->GetScaledCapsuleRadius()) * FMath::Abs(CollisionAxis(Capsule).Z));
		Row->SetNumberField(TEXT("half_height"), Capsule->GetScaledCapsuleHalfHeight());
		Row->SetNumberField(TEXT("radius"), Capsule->GetScaledCapsuleRadius());
		Row->SetBoolField(TEXT("collision"), Capsule->IsQueryCollisionEnabled());
		if (const UPrimitiveComponent* Base = Cast<UPrimitiveComponent>(Body->GetMovementBaseObject()))
		{
			Row->SetStringField(TEXT("base"), Base->GetPathName());
			Row->SetStringField(TEXT("base_position"), Base->GetComponentLocation().ToString());
		}
		History.LastLocation = Body->GetActorLocation();
		History.HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		History.Radius = Capsule->GetScaledCapsuleRadius();
		History.Rows.RemoveAll([this](const auto& Old) { return Old->AsObject()->GetNumberField(TEXT("seconds")) < Time - 2.0; });
		History.Rows.Add(Value(Row));
	}
	void ProbeBodies(double Delta)
	{
		for (TActorIterator<AWoTCharacter> It(World); It; ++It)
		{
			AWoTCharacter* Body = *It;
			if (!Body->IsAlive()) continue;
			Remember(Body, TEXT("tick"));
			FBodyHistory& History = BodyHistories.FindChecked(Body);
			UCapsuleComponent* Capsule = Body->GetCapsuleComponent();
			const FVector Here = Body->GetActorLocation();
			const FVector Feet = Here - FVector(0, 0, Capsule->GetScaledCapsuleHalfHeight());
			FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTSoakBodyGeometry), false, Body);
			FCollisionObjectQueryParams Objects;
			Objects.AddObjectTypesToQuery(ECC_WorldStatic);
			Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
			TArray<FHitResult> Floors, Contacts;
			World->LineTraceMultiByObjectType(Floors, Here, Feet - FVector(0, 0, 2), Objects, Params);
			World->SweepMultiByObjectType(Contacts, Here, Here, FQuat::FindBetweenNormals(FVector::UpVector, CollisionAxis(Capsule)), Objects,
				Capsule->GetCollisionShape(), Params);
			bool bFloor = false, bPenetrating = false;
			TArray<TSharedPtr<FJsonValue>> Geometry;
			const auto RecordHit = [&](const FHitResult& Hit, const TCHAR* Kind)
			{
				FObject Row = Object(); Row->SetStringField(TEXT("kind"), Kind);
				Row->SetStringField(TEXT("actor"), Hit.GetActor()->GetName());
				Row->SetStringField(TEXT("component"), Hit.GetComponent() ? Hit.GetComponent()->GetName() : TEXT("none"));
				Row->SetStringField(TEXT("point"), Hit.ImpactPoint.ToString());
				Row->SetStringField(TEXT("normal"), Hit.ImpactNormal.ToString());
				Row->SetNumberField(TEXT("floor_above_feet"), Hit.ImpactPoint.Z - Feet.Z);
				const double Extent = Capsule->GetScaledCapsuleRadius() + (Capsule->GetScaledCapsuleHalfHeight() - Capsule->GetScaledCapsuleRadius()) * FMath::Abs(CollisionAxis(Capsule).Z);
				Row->SetNumberField(TEXT("rotated_bottom_clearance"), Here.Z - Extent - Hit.ImpactPoint.Z);
				Row->SetNumberField(TEXT("penetration_depth"), Hit.PenetrationDepth);
				Geometry.Add(Value(Row));
			};
			for (const FHitResult& Hit : Floors)
				if (WoTRainCydIsVisBlockingActor(Hit.GetActor()))
				{
					RecordHit(Hit, TEXT("floor_ray"));
					bFloor |= Body->GetCharacterMovement()->IsWalkable(Hit) && Hit.ImpactPoint.Z > Feet.Z + UE_KINDA_SMALL_NUMBER;
				}
			for (const FHitResult& Hit : Contacts)
				if (Hit.bStartPenetrating && Hit.PenetrationDepth > UE_KINDA_SMALL_NUMBER
					&& WoTRainCydIsVisBlockingActor(Hit.GetActor()))
				{ RecordHit(Hit, TEXT("capsule_penetration")); bPenetrating = true; }
			const bool bInside = bFloor || bPenetrating;
			Increment(Report, TEXT("living_body_seconds"), Delta);
			if (bFloor && !History.bFloor) Increment(Report, TEXT("inside_floor_count"));
			if (bPenetrating && !History.bPenetrating) Increment(Report, TEXT("geometry_penetration_count"));
			if (bInside && !History.bInside)
			{
				FObject Event = Object();
				Event->SetStringField(TEXT("body"), Body->GetName());
				Event->SetStringField(TEXT("controller"), GetNameSafe(Body->GetController()));
				Event->SetNumberField(TEXT("first_seconds"), Time);
				Event->SetNumberField(TEXT("last_seconds"), Time);
				Event->SetBoolField(TEXT("inside_floor"), bFloor);
				Event->SetBoolField(TEXT("penetrating"), bPenetrating);
				Event->SetStringField(TEXT("position"), Here.ToString());
				Event->SetStringField(TEXT("feet"), Feet.ToString());
				Event->SetStringField(TEXT("movement"), Body->GetCharacterMovement()->GetMovementName());
				Event->SetArrayField(TEXT("history"), History.Rows);
				Event->SetArrayField(TEXT("geometry"), Geometry);
				FloorEvents.Add(Value(Event)); History.ActiveEvent = Event;
				UE_LOG(LogTemp, Warning, TEXT("BOT_SOAK_BODY_INSIDE map=%s seconds=%.3f body=%s floor=%d penetration=%d position=%s"),
					*Report->GetStringField(TEXT("map")), Time, *Body->GetName(), bFloor, bPenetrating, *Here.ToString());
			}
			if (bInside && History.ActiveEvent)
			{
				History.ActiveEvent->SetNumberField(TEXT("last_seconds"), Time);
				History.ActiveEvent->SetObjectField(TEXT("last_state"), History.Rows.Last()->AsObject());
				History.ActiveEvent->SetArrayField(TEXT("last_geometry"), Geometry);
				History.ActiveEvent->SetBoolField(TEXT("inside_floor"), History.ActiveEvent->GetBoolField(TEXT("inside_floor")) || bFloor);
				History.ActiveEvent->SetBoolField(TEXT("penetrating"), History.ActiveEvent->GetBoolField(TEXT("penetrating")) || bPenetrating);
			}
			History.bInside = bInside; History.bFloor = bFloor; History.bPenetrating = bPenetrating;
		}
		for (auto It = BodyHistories.CreateIterator(); It; ++It) if (!It.Key().IsValid()) It.RemoveCurrent();
	}
	static const ANavigationData* BotNavigation(UWorld* W, UNavigationSystemV1*& Nav)
	{
		Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
		if (!Nav) return nullptr;
		for (ANavigationData* Data : Nav->NavDataSet) if (Data && Data->IsA<AWoTBotNavMesh>()) return Data;
		return nullptr;
	}
	static bool CompletePath(UNavigationSystemV1* Nav, const ANavigationData* Data, const FVector& From, const FVector& To, double* Length = nullptr)
	{
		FPathFindingQuery Query(nullptr, *Data, From, To);
		Query.SetAllowPartialPaths(false);
		const FPathFindingResult Path = Nav->FindPathSync(Data->GetConfig(), Query);
		if (!Path.IsSuccessful() || !Path.Path.IsValid() || Path.Path->IsPartial()) return false;
		if (Length) *Length = Path.Path->GetLength();
		return true;
	}
	// Why a recovery fired: where the route led, what the body stands on, and what
	// blocks a step along the current path segment.
	void DescribeStuck(const FObject& Event, AWoTBotController* B)
	{
		static const TCHAR* StateNames[] = { TEXT("roam"), TEXT("seek"), TEXT("engage"), TEXT("dead"), TEXT("frozen") };
		RecordGoal(B);
		TArray<TSharedPtr<FJsonValue>> HistoryRows;
		const auto& History = GoalHistory.FindChecked(B);
		for (const auto& Change : History)
		{
			FObject Row = Object();
			Row->SetNumberField(TEXT("seconds"), Change.Time);
			Row->SetStringField(TEXT("pool"), Change.Pool);
			Row->SetStringField(TEXT("state"), StateNames[int32(Change.State)]);
			HistoryRows.Add(Value(Row));
		}
		Event->SetArrayField(TEXT("recent_goals"), HistoryRows);
		Event->SetNumberField(TEXT("goal_age_seconds"), Time - History.Last().Time);
		Event->SetStringField(TEXT("state"), StateNames[int32(B->GetBehaviourState())]);
		const AWoTCharacter* Body = Cast<AWoTCharacter>(B->GetPawn());
		if (!Body) return;
		Event->SetStringField(TEXT("body"), Body->GetName());
		const float HalfHeight = Body->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector Here = Body->GetActorLocation(), Feet = Here - FVector(0, 0, HalfHeight);
		const FVector Goal = B->GetDestination();
		Event->SetStringField(TEXT("destination"), Position(Goal));
		Event->SetNumberField(TEXT("destination_2d"), FVector::Dist2D(Here, Goal));
		Event->SetNumberField(TEXT("destination_dz"), Goal.Z - Feet.Z);
		Event->SetBoolField(TEXT("navigation_unavailable"), B->IsNavigationUnavailable());
		Event->SetNumberField(TEXT("since_jump"), World->GetTimeSeconds() - B->GetLastJumpTime());
		if (const AWoTAngrealSpawnPool* P = B->GetSeekingPool())
		{
			Event->SetStringField(TEXT("pool"), P->GetName());
			Event->SetStringField(TEXT("pool_position"), Position(P->GetActorLocation()));
			Event->SetBoolField(TEXT("pool_stocked"), P->PickupItem != nullptr);
			Event->SetNumberField(TEXT("pool_2d"), FVector::Dist2D(Here, P->GetActorLocation()));
			Event->SetNumberField(TEXT("pool_dz"), P->GetActorLocation().Z - Feet.Z);
		}
		const UCharacterMovementComponent* Move = Body->GetCharacterMovement();
		Event->SetStringField(TEXT("movement"), Move->GetMovementName());
		Event->SetNumberField(TEXT("speed"), Body->GetVelocity().Size());
		if (const AActor* Floor = Move->CurrentFloor.HitResult.GetActor()) Event->SetStringField(TEXT("floor"), Floor->GetName());
		const UPathFollowingComponent* Follow = B->GetPathFollowingComponent();
		Event->SetNumberField(TEXT("follow_status"), int32(Follow->GetStatus()));
		FVector Next = Goal;
		if (Follow->HasValidPath())
		{
			const FNavPathSharedPtr Path = Follow->GetPath();
			const int32 Index = Follow->GetCurrentPathIndex();
			Event->SetNumberField(TEXT("path_points"), Path->GetPathPoints().Num());
			Event->SetNumberField(TEXT("path_index"), Index);
			Event->SetBoolField(TEXT("path_partial"), Path->IsPartial());
			if (Path->GetPathPoints().IsValidIndex(Index + 1)) Next = Path->GetPathPoints()[Index + 1].Location;
			Event->SetStringField(TEXT("path_next"), Position(Next));
			Event->SetNumberField(TEXT("path_next_dz"), Next.Z - Feet.Z);
			// A route that continues well above the feet: what holds that mesh up?
			if (Next.Z - Feet.Z > 20) Event->SetStringField(TEXT("path_next_below"), Below(World, Next));
		}
		UNavigationSystemV1* Nav = nullptr;
		if (const ANavigationData* Data = BotNavigation(World, Nav))
		{
			FNavLocation OnMesh;
			const bool bOnMesh = Nav->ProjectPointToNavigation(Body->GetNavAgentLocation(), OnMesh, FVector(50, 50, 100), Data);
			Event->SetBoolField(TEXT("on_navigation"), bOnMesh);
			if (bOnMesh)
			{
				Event->SetNumberField(TEXT("navigation_offset_2d"), FVector::Dist2D(OnMesh.Location, Here));
				Event->SetBoolField(TEXT("route_complete"), CompletePath(Nav, Data, OnMesh.Location, Goal));
			}
		}
		const FVector Direction = (Next - Here).GetSafeNormal2D();
		if (!Direction.IsNearlyZero())
		{
			FHitResult Hit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTBotSoakStuck), false, Body);
			if (World->SweepSingleByChannel(Hit, Here, Here + Direction * 40.0f, FQuat::Identity,
				Body->GetCapsuleComponent()->GetCollisionObjectType(), Body->GetCapsuleComponent()->GetCollisionShape(), Params))
			{
				Event->SetStringField(TEXT("blocked_by"), Hit.GetActor() ? Hit.GetActor()->GetName() : TEXT("none"));
				Event->SetStringField(TEXT("blocked_class"), Hit.GetActor() ? Hit.GetActor()->GetClass()->GetName() : TEXT("none"));
				// Distinguish a body's own ice from another body's shell or map collision.
				const AActor* BlockedOwner = Hit.GetActor() ? Hit.GetActor()->GetOwner() : nullptr;
				Event->SetStringField(TEXT("blocked_owner"), BlockedOwner ? BlockedOwner->GetName() : TEXT("none"));
				Event->SetNumberField(TEXT("blocked_impact_dz"), Hit.ImpactPoint.Z - Feet.Z);
				Event->SetNumberField(TEXT("blocked_normal_z"), Hit.ImpactNormal.Z);
				Event->SetNumberField(TEXT("blocked_distance"), Hit.Distance);
			}
		}
	}
	// Per pool: where it projects on the bots' mesh, whether standing there touches
	// its pickup, and from how many player starts a complete route leads there.
	void DescribeReachability()
	{
		UNavigationSystemV1* Nav = nullptr;
		const ANavigationData* Data = BotNavigation(World, Nav);
		if (!Data) return;
		TArray<TPair<FString, FVector>> Starts;
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			FNavLocation Start;
			if (Nav->ProjectPointToNavigation(It->GetActorLocation(), Start, FVector(50, 50, 150), Data)) Starts.Emplace(It->GetName(), Start.Location);
			else Starts.Emplace(It->GetName(), FVector(std::numeric_limits<double>::quiet_NaN()));
		}
		TMap<FString, int32> StartReach;
		int32 Projected = 0;
		for (const auto& Pair : Pools)
		{
			const AWoTAngrealSpawnPool* P = Pair.Key.Get();
			if (!P) continue;
			const FObject Row = Pair.Value;
			FNavLocation End;
			if (!Nav->ProjectPointToNavigation(P->GetActorLocation(), End, FVector(80, 80, 160), Data))
			{ Row->SetBoolField(TEXT("on_navigation"), false); continue; }
			++Projected;
			Row->SetBoolField(TEXT("on_navigation"), true);
			Row->SetStringField(TEXT("navigation_point"), Position(End.Location));
			Row->SetNumberField(TEXT("navigation_offset_2d"), FVector::Dist2D(End.Location, P->GetActorLocation()));
			Row->SetNumberField(TEXT("navigation_offset_z"), P->GetActorLocation().Z - End.Location.Z);
			// A player body (17 x 46) standing at the point, against the 30-unit pickup sphere.
			const FVector Bottom = End.Location + FVector(0, 0, 17), Top = End.Location + FVector(0, 0, 92 - 17);
			const FVector Closest = FMath::ClosestPointOnSegment(P->GetActorLocation(), Bottom, Top);
			Row->SetBoolField(TEXT("touch_from_navigation"), FVector::Dist(Closest, P->GetActorLocation()) < 17 + P->GetPickupCollisionRadius());
			int32 Reaching = 0;
			for (const auto& Start : Starts)
				if (!Start.Value.ContainsNaN() && CompletePath(Nav, Data, Start.Value, End.Location)) { ++Reaching; ++StartReach.FindOrAdd(Start.Key); }
			Row->SetNumberField(TEXT("starts_reaching"), Reaching);
			Row->SetNumberField(TEXT("starts"), Starts.Num());
		}
		TArray<TSharedPtr<FJsonValue>> StartRows;
		for (const auto& Start : Starts)
		{
			FObject Row = Object();
			Row->SetStringField(TEXT("id"), Start.Key);
			Row->SetBoolField(TEXT("on_navigation"), !Start.Value.ContainsNaN());
			if (!Start.Value.ContainsNaN()) Row->SetStringField(TEXT("navigation_point"), Position(Start.Value));
			Row->SetNumberField(TEXT("pools_reached"), StartReach.FindRef(Start.Key));
			Row->SetNumberField(TEXT("pools_on_navigation"), Projected);
			StartRows.Add(Value(Row));
		}
		Report->SetArrayField(TEXT("player_starts"), StartRows);
	}
	// Opt-in (WOT_BOT_SOAK_NAVDUMP=1): every bot-mesh polygon with its connected
	// island, plus the retail path nodes, for offline plotting.
	void DumpNavigation(const FString& Path)
	{
		UNavigationSystemV1* Nav = nullptr;
		const ARecastNavMesh* Mesh = Cast<ARecastNavMesh>(BotNavigation(World, Nav));
		if (!Mesh) return;
		TArray<FNavTileRef> Tiles; Mesh->GetAllNavMeshTiles(Tiles);
		TArray<NavNodeRef> Polys;
		TMap<NavNodeRef, int32> Index;
		for (FNavTileRef Tile : Tiles)
		{
			TArray<FNavPoly> InTile; Mesh->GetPolysInTile(Tile, InTile);
			for (const FNavPoly& P : InTile) { Index.Add(P.Ref, Polys.Num()); Polys.Add(P.Ref); }
		}
		TArray<int32> Parent; Parent.SetNum(Polys.Num());
		for (int32 I = 0; I < Parent.Num(); ++I) Parent[I] = I;
		const auto Find = [&Parent](int32 I) { while (Parent[I] != I) I = Parent[I] = Parent[Parent[I]]; return I; };
		for (int32 I = 0; I < Polys.Num(); ++I)
		{
			TArray<NavNodeRef> Neighbours; Mesh->GetPolyNeighbors(Polys[I], Neighbours);
			for (NavNodeRef N : Neighbours) if (const int32* J = Index.Find(N)) Parent[Find(I)] = Find(*J);
		}
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (int32 I = 0; I < Polys.Num(); ++I)
		{
			TArray<FVector> Verts; Mesh->GetPolyVerts(Polys[I], Verts);
			TArray<TSharedPtr<FJsonValue>> Flat;
			for (const FVector& V : Verts) for (double C : { V.X, V.Y, V.Z }) Flat.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(C)));
			FObject Row = Object(); Row->SetNumberField(TEXT("island"), Find(I)); Row->SetArrayField(TEXT("v"), Flat);
			Rows.Add(Value(Row));
		}
		FObject Dump = Object(); Dump->SetArrayField(TEXT("polys"), Rows);
		const auto IslandAt = [&](const FVector& Point, FVector Extent) -> int32
		{
			FNavLocation Projected;
			if (!Nav->ProjectPointToNavigation(Point, Projected, Extent, Mesh)) return -1;
			const int32* I = Index.Find(Projected.NodeRef);
			return I ? Find(*I) : -1;
		};
		TArray<TSharedPtr<FJsonValue>> PoolRows;
		for (TActorIterator<AWoTAngrealSpawnPool> It(World); It; ++It)
		{
			FObject Row = Object(); Row->SetStringField(TEXT("id"), It->GetName());
			Row->SetStringField(TEXT("position"), Position(It->GetActorLocation()));
			Row->SetNumberField(TEXT("island"), IslandAt(It->GetActorLocation(), FVector(80, 80, 160)));
			PoolRows.Add(Value(Row));
		}
		Dump->SetArrayField(TEXT("pools"), PoolRows);
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (TActorIterator<AWoTPathNode> It(World); It; ++It)
		{
			FObject Row = Object(); Row->SetStringField(TEXT("id"), It->GetName());
			Row->SetNumberField(TEXT("island"), IslandAt(It->GetActorLocation(), FVector(50, 50, 150)));
			Row->SetStringField(TEXT("class"), It->SourceClass.ToString());
			Row->SetStringField(TEXT("position"), Position(It->GetActorLocation()));
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FWoTPathEdge& E : It->OutEdges) if (E.Other) Out.Add(MakeShared<FJsonValueString>(E.Other->GetName()));
			Row->SetArrayField(TEXT("out"), Out);
			Nodes.Add(Value(Row));
		}
		Dump->SetArrayField(TEXT("nodes"), Nodes);
		Save(Dump, Path);
	}
	// The same cluster rule for as many bodies placed uniformly at random on the
	// mesh island holding most player starts: what the map's size alone yields.
	void RandomClusterBaseline()
	{
		const int32 Bodies = FMath::RoundToInt(Number(Report, TEXT("cluster_living_sum")) / FMath::Max(1.0, Number(Report, TEXT("cluster_samples"))));
		UNavigationSystemV1* Nav = nullptr;
		const ARecastNavMesh* Mesh = Cast<ARecastNavMesh>(BotNavigation(World, Nav));
		if (!Mesh || Bodies < 3) return;
		TArray<FNavTileRef> Tiles; Mesh->GetAllNavMeshTiles(Tiles);
		TArray<NavNodeRef> Polys; TMap<NavNodeRef, int32> Index;
		for (FNavTileRef Tile : Tiles)
		{
			TArray<FNavPoly> InTile; Mesh->GetPolysInTile(Tile, InTile);
			for (const FNavPoly& P : InTile) { Index.Add(P.Ref, Polys.Num()); Polys.Add(P.Ref); }
		}
		TArray<int32> Parent; Parent.SetNum(Polys.Num());
		for (int32 I = 0; I < Parent.Num(); ++I) Parent[I] = I;
		const auto Find = [&Parent](int32 I) { while (Parent[I] != I) I = Parent[I] = Parent[Parent[I]]; return I; };
		for (int32 I = 0; I < Polys.Num(); ++I)
		{
			TArray<NavNodeRef> Neighbours; Mesh->GetPolyNeighbors(Polys[I], Neighbours);
			for (NavNodeRef N : Neighbours) if (const int32* J = Index.Find(N)) Parent[Find(I)] = Find(*J);
		}
		TMap<int32, int32> Starts;
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			FNavLocation At;
			if (Nav->ProjectPointToNavigation(It->GetActorLocation(), At, FVector(50, 50, 150), Mesh))
				if (const int32* I = Index.Find(At.NodeRef)) ++Starts.FindOrAdd(Find(*I));
		}
		int32 Main = INDEX_NONE, Most = 0;
		for (const auto& Pair : Starts) if (Pair.Value > Most) { Most = Pair.Value; Main = Pair.Key; }
		if (Main == INDEX_NONE) return;
		struct FTriangle { FVector A, B, C; };
		TArray<FTriangle> Triangles; TArray<double> Cumulative; double Area = 0;
		for (int32 I = 0; I < Polys.Num(); ++I)
		{
			if (Find(I) != Main) continue;
			TArray<FVector> Verts; Mesh->GetPolyVerts(Polys[I], Verts);
			for (int32 V = 2; V < Verts.Num(); ++V)
			{
				Area += .5 * FVector::CrossProduct(Verts[V - 1] - Verts[0], Verts[V] - Verts[0]).Size();
				Triangles.Add({ Verts[0], Verts[V - 1], Verts[V] }); Cumulative.Add(Area);
			}
		}
		if (Triangles.IsEmpty() || Area <= 0) return;
		FRandomStream Rng(MapSeed(Report->GetStringField(TEXT("map"))));
		const auto RandomPoint = [&]()
		{
			const int32 T = FMath::Min(Algo::LowerBound(Cumulative, Rng.FRand() * Area), Triangles.Num() - 1);
			float U = Rng.FRand(), V = Rng.FRand();
			if (U + V > 1) { U = 1 - U; V = 1 - V; }
			return Triangles[T].A + U * (Triangles[T].B - Triangles[T].A) + V * (Triangles[T].C - Triangles[T].A);
		};
		int32 Clustered = 0; double PairSum = 0; int32 Pairs = 0;
		constexpr int32 Trials = 2000;
		TArray<FVector> Points;
		for (int32 Trial = 0; Trial < Trials; ++Trial)
		{
			Points.Reset();
			for (int32 I = 0; I < Bodies; ++I) Points.Add(RandomPoint());
			bool bClustered = false;
			for (int32 A = 0; A < Points.Num(); ++A)
				for (int32 B = A + 1; B < Points.Num(); ++B)
				{
					PairSum += FVector::Dist(Points[A], Points[B]); ++Pairs;
					for (int32 C = B + 1; C < Points.Num() && !bClustered; ++C)
						bClustered = FVector::DistSquared(Points[A], Points[B]) <= 360000
							&& FVector::DistSquared(Points[A], Points[C]) <= 360000
							&& FVector::DistSquared(Points[B], Points[C]) <= 360000;
				}
			Clustered += bClustered;
		}
		Report->SetNumberField(TEXT("cluster_bodies"), Bodies);
		Report->SetNumberField(TEXT("cluster_share_random"), double(Clustered) / Trials);
		Report->SetNumberField(TEXT("mean_pair_distance_random"), PairSum / FMath::Max(1, Pairs));
		Report->SetNumberField(TEXT("main_island_area"), Area);
	}
	// What a downward trace finds below a point, by every primitive type.
	static FString Below(UWorld* W, const FVector& Point)
	{
		TArray<FHitResult> Hits;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTBotSoakBelow), true);
		W->LineTraceMultiByObjectType(Hits, Point + FVector(0, 0, 20), Point - FVector(0, 0, 60), FCollisionObjectQueryParams::AllObjects, Params);
		FString Text;
		for (const FHitResult& Hit : Hits)
		{
			const UPrimitiveComponent* C = Hit.GetComponent();
			Text += FString::Printf(TEXT("%s/%s z=%.0f pawn=%d nav=%d; "), Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("-"),
				C ? *C->GetName() : TEXT("-"), Hit.ImpactPoint.Z, C ? int32(C->GetCollisionResponseToChannel(ECC_Pawn)) : -1,
				C ? int32(C->IsNavigationRelevant()) : -1);
		}
		return Text;
	}
	FObject Bot(AWoTBotController* B)
	{
		if (const FObject* Found = Bots.Find(B)) return *Found;
		FObject R = Object(); R->SetStringField(TEXT("id"), B->GetName());
		R->SetObjectField(TEXT("casts"), Object());
		for (const TCHAR* Key : { TEXT("kills"), TEXT("deaths"), TEXT("suicides"), TEXT("pickups"), TEXT("recoveries"), TEXT("silent_respawns") }) R->SetNumberField(Key, 0);
		Bots.Add(B, R); return R;
	}
	FObject Pool(AWoTAngrealSpawnPool* P)
	{
		if (const FObject* Found = Pools.Find(P)) return *Found;
		FObject R = Object(); R->SetStringField(TEXT("id"), P->GetName());
		R->SetStringField(TEXT("position"), Position(P->GetActorLocation()));
		R->SetNumberField(TEXT("stocked"), 0); R->SetNumberField(TEXT("collected"), 0);
		Pools.Add(P, R); return R;
	}
	void Sample()
	{
		const double Delta = Time - LastSampleTime;
		LastSampleTime = Time;
		ProbeBodies(Delta);
		TArray<FVector> Living;
		for (TActorIterator<AWoTBotController> It(World); It; ++It)
		{
			RecordGoal(*It);
			FObject R = Bot(*It);
			static const TCHAR* States[] = { TEXT("roam_seconds"), TEXT("seek_seconds"), TEXT("engage_seconds"), TEXT("dead_seconds"), TEXT("frozen_seconds") };
			Increment(R, States[int32(It->GetBehaviourState())], Delta);
			Increment(Report, States[int32(It->GetBehaviourState())], Delta);
			Increment(Report, TEXT("bot_seconds"), Delta);
			if (const AWoTCharacter* Body = Cast<AWoTCharacter>(It->GetPawn()); Body && Body->IsAlive())
			{
				// A body falling through a gap in a floor can be far below the arena;
				// count its time, but keep it out of the arena's spacing samples.
				if (Body->GetCharacterMovement()->IsFalling())
				{
					Increment(R, TEXT("falling_seconds"), Delta);
					Increment(Report, TEXT("falling_seconds"), Delta);
				}
				else Living.Add(Body->GetActorLocation());
				for (const auto& P : Pools)
				{
					if (!P.Key.IsValid()) continue;
					const double Distance = FVector::Dist(Body->GetActorLocation(), P.Key->GetActorLocation());
					if (Distance <= 120.0) VisitedPools.Add(P.Key);
					double& Closest = ClosestApproach.FindOrAdd(P.Key, TNumericLimits<double>::Max());
					Closest = FMath::Min(Closest, Distance);
				}
				if (AWoTAngrealSpawnPool* Goal = It->GetSeekingPool())
					Increment(Pool(Goal), It->GetBehaviourState() == EWoTBotState::SeekItem ? TEXT("sought_seconds") : TEXT("roam_goal_seconds"), Delta);
			}
			R->SetNumberField(TEXT("climb_jumps"), It->GetClimbJumpCount());
			R->SetNumberField(TEXT("hops"), It->GetHopCount());
			if (const AWoTPlayerState* S = It->GetPlayerState<AWoTPlayerState>())
			{
				R->SetNumberField(TEXT("kills"), S->Kills);
				R->SetNumberField(TEXT("deaths"), S->Deaths);
				R->SetNumberField(TEXT("suicides"), S->Suicides);
			}
		}
		if (FMath::FloorToInt(Time) > LastClusterSecond)
		{
			LastClusterSecond = FMath::FloorToInt(Time);
			bool Clustered = false;
			for (int32 A = 0; A < Living.Num(); ++A)
				for (int32 B = A + 1; B < Living.Num(); ++B)
				{
					PairDistances.Add(FVector::Dist(Living[A], Living[B]));
					Increment(Report, TEXT("pair_distance_sum"), PairDistances.Last());
					Increment(Report, TEXT("pair_samples"));
					for (int32 C = B + 1; C < Living.Num(); ++C)
						Clustered |= FVector::DistSquared(Living[A], Living[B]) <= 360000
							&& FVector::DistSquared(Living[A], Living[C]) <= 360000
							&& FVector::DistSquared(Living[B], Living[C]) <= 360000;
				}
			Increment(Report, TEXT("cluster_samples"));
			Increment(Report, TEXT("clustered_samples"), Clustered ? 1 : 0);
			Increment(Report, TEXT("cluster_living_sum"), Living.Num());
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			const FVector Location = A->GetActorLocation(), EngineVelocity = A->GetVelocity();
			FVector Velocity = WoTSoakEvents::SimulationVelocity(A);
			if (Finite(Velocity) && (!Finite(EngineVelocity) || EngineVelocity.SizeSquared() > Velocity.SizeSquared())) Velocity = EngineVelocity;
			const bool bFinite = Finite(Location) && Finite(Velocity);
			const APhysicsVolume* Zone = A->GetPhysicsVolume();
			const double Bound = 10.0 * FMath::Max(1.0f, Zone ? Zone->TerminalVelocity : 4000.0f);
			if (bFinite && Velocity.Size() <= Bound) continue;
			auto& Seen = bFinite ? RunawayActors : NonFiniteActors;
			if (Seen.Contains(A)) continue;
			Seen.Add(A);
			Increment(Report, bFinite ? TEXT("runaway") : TEXT("non_finite"));
			FObject Event = Object();
			Event->SetStringField(TEXT("actor"), A->GetName()); Event->SetStringField(TEXT("class"), A->GetClass()->GetPathName());
			Event->SetStringField(TEXT("kind"), bFinite ? TEXT("runaway") : TEXT("non_finite"));
			Event->SetNumberField(TEXT("first_seconds"), Time);
			Event->SetStringField(TEXT("position"), Location.ToString()); Event->SetStringField(TEXT("velocity"), Velocity.ToString());
			Event->SetNumberField(TEXT("speed_bound"), Bound);
			Anomalies.Add(Value(Event));
		}
		const AWoTGameMode* G = World->GetAuthGameMode<AWoTGameMode>();
		if (G && G->bGameEnded && !Report->GetBoolField(TEXT("frag_limit_end")))
		{
			bool bReached = false;
			for (const auto& Pair : Bots) bReached |= G->FragLimit > 0 && Number(Pair.Value,TEXT("kills")) - Number(Pair.Value,TEXT("suicides")) >= G->FragLimit;
			Report->SetBoolField(TEXT("frag_limit_end"), bReached);
			Report->SetNumberField(TEXT("match_end_seconds"), Time);
		}
		if (G && !G->PendingMapURL.IsEmpty() && !Report->GetBoolField(TEXT("restart_requested")))
		{
			Report->SetBoolField(TEXT("restart_requested"), true);
			Report->SetNumberField(TEXT("restart_request_seconds"), Time);
			Report->SetStringField(TEXT("restart_url"), G->PendingMapURL);
		}
	}
	void Snapshot()
	{
		Report->SetNumberField(TEXT("simulated_seconds"), Time);
		Report->SetArrayField(TEXT("body_geometry_events"), FloorEvents);
		int32 Kills = 0, Deaths = 0, Unvisited = 0;
		TArray<TSharedPtr<FJsonValue>> BotRows, PoolRows;
		for (const auto& P : Bots) { BotRows.Add(Value(P.Value)); Kills += int32(Number(P.Value,TEXT("kills"))); Deaths += int32(Number(P.Value,TEXT("deaths"))); }
		for (const auto& P : Pools)
		{
			if (const double* Closest = ClosestApproach.Find(P.Key)) P.Value->SetNumberField(TEXT("closest_approach"), *Closest);
			PoolRows.Add(Value(P.Value)); Unvisited += Number(P.Value,TEXT("collected")) == 0;
		}
		Report->SetArrayField(TEXT("bots"), BotRows); Report->SetArrayField(TEXT("pools"), PoolRows);
		Report->SetArrayField(TEXT("actor_anomalies"), Anomalies); Report->SetArrayField(TEXT("recovery_events"), Recoveries);
		Report->SetNumberField(TEXT("kills"), Kills); Report->SetNumberField(TEXT("deaths"), Deaths);
		Report->SetNumberField(TEXT("unvisited_pools"), Unvisited);
		double Pickups = 0;
		for (const auto& P : Bots) Pickups += Number(P.Value, TEXT("pickups"));
		Report->SetNumberField(TEXT("pickups_per_bot_minute"), Pickups * 60 / FMath::Max(1.0, Number(Report, TEXT("bot_seconds"))));
		Report->SetNumberField(TEXT("unique_pools_visited"), VisitedPools.Num());
		if (const UWoTBotRoster* Roster = World->GetSubsystem<UWoTBotRoster>()) Report->SetNumberField(TEXT("navigation_links"), Roster->GetLinkCount());
		// Tiles the bot mesh built or rebuilt; after the first checkpoint (links in
		// place), further rebuilds come from movers and other runtime changes.
		UNavigationSystemV1* Nav = nullptr;
		if (const AWoTBotNavMesh* Mesh = Cast<AWoTBotNavMesh>(BotNavigation(World, Nav)))
		{
			if (!Report->HasField(TEXT("navigation_tile_updates_first_checkpoint")))
				Report->SetNumberField(TEXT("navigation_tile_updates_first_checkpoint"), Mesh->GetTileUpdateCount());
			Report->SetNumberField(TEXT("navigation_tile_updates"), Mesh->GetTileUpdateCount());
		}
		Report->SetNumberField(TEXT("unique_pools_collected"), Pools.Num() - Unvisited);
		Report->SetNumberField(TEXT("cluster_share"), Number(Report, TEXT("clustered_samples")) / FMath::Max(1.0, Number(Report, TEXT("cluster_samples"))));
		RandomClusterBaseline();
		Report->SetNumberField(TEXT("mean_pair_distance"), Number(Report, TEXT("pair_distance_sum")) / FMath::Max(1.0, Number(Report, TEXT("pair_samples"))));
		if (!PairDistances.IsEmpty())
		{
			// A few bodies far from the others dominate the mean; the median is robust to them.
			TArray<double> Sorted = PairDistances; Sorted.Sort();
			Report->SetNumberField(TEXT("median_pair_distance"), Sorted[Sorted.Num() / 2]);
		}
		if (!TickTimes.IsEmpty())
		{
			TArray<double> Sorted = TickTimes; Sorted.Sort(); double Total = 0; int32 Slow = 0;
			for (double T : Sorted) { Total += T; Slow += T > 1000.0 / 30.0; }
			Report->SetNumberField(TEXT("tick_avg_ms"), Total / Sorted.Num());
			Report->SetNumberField(TEXT("tick_p95_ms"), Sorted[FMath::CeilToInt(.95 * Sorted.Num()) - 1]);
			Report->SetNumberField(TEXT("tick_max_ms"), Sorted.Last());
			Report->SetNumberField(TEXT("slow_ticks"), Slow);
		}
		Logs.Snapshot(Report);
	}
};

bool RunMap(const FObject& Report, const FString& Output, FString& Error)
{
	FLogCapture Logs;
	const double Started = FPlatformTime::Seconds();
	ON_SCOPE_EXIT { Logs.Snapshot(Report); Report->SetNumberField(TEXT("worker_wall_seconds"), FPlatformTime::Seconds()-Started); Save(Report, Output); };
	FMapSet Maps;
	const FString Map = Report->GetStringField(TEXT("map"));
	if (!Maps.Load(Error)) return false;
	const FWoTWorldDescriptor* D = Maps.Descriptors.FindByPredicate([&](const auto& Item) { return Item.MapIdentity == Map; });
	if (!D) { Error = TEXT("Requested Arena map is absent from imported contract"); return false; }
	WoTTest::FSpecWorldOptions Options; Options.bStartPlay = false; Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	UWorld* W = WoTTest::CreateSpecWorld(Options);
	if (!W) { Error = TEXT("Could not create world"); return false; }
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); CollectGarbage(RF_NoFlags); };
	FSoakServer Server(W);
	FNavProbe NavProbe(W);
	ON_SCOPE_EXIT { NavProbe.Snapshot(Report, Output); };
	if (W->GetNetMode() != NM_DedicatedServer) { Error = TEXT("Worker requires dedicated server gameplay semantics"); return false; }
	Report->SetStringField(TEXT("net_mode"), TEXT("dedicated_server"));
	WoTRandom::Seed(int32(Number(Report,TEXT("seed"))));
	FObserver Observer(W, Report, Logs);
	if (!Materialize(W, Maps, *D, Error)) return false;
	UNavigationSystemConfig* Config = NewObject<UNavigationSystemConfig>(W->GetWorldSettings());
	Config->NavigationSystemClass = FSoftClassPath(UWoTBotNavigationSystem::StaticClass());
	W->GetWorldSettings()->SetNavigationSystemConfigOverride(Config);
	FNavigationSystem::AddNavigationSystemToWorld(*W, FNavigationSystemRunMode::GameMode, Config);
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
	if (!Nav) { Error = TEXT("Navigation system unavailable"); return false; }
	const double NavStart = FPlatformTime::Seconds();
	Nav->Build();
	int32 Agents = 0;
	TArray<TSharedPtr<FJsonValue>> Meshes;
	for (ANavigationData* Data : Nav->NavDataSet)
	{
		if (!Data) continue;
		Data->EnsureBuildCompletion(); ++Agents;
		const ARecastNavMesh* Mesh = Cast<ARecastNavMesh>(Data);
		int32 Populated = 0, Polygons = 0;
		if (Mesh)
		{
			TArray<FNavTileRef> Tiles; Mesh->GetAllNavMeshTiles(Tiles);
			for (FNavTileRef Tile : Tiles)
			{
				TArray<FNavPoly> Polys; Mesh->GetPolysInTile(Tile,Polys);
				Populated += !Polys.IsEmpty(); Polygons += Polys.Num();
			}
		}
		FObject Row = Object(); Row->SetStringField(TEXT("class"), Data->GetClass()->GetPathName());
		Row->SetNumberField(TEXT("populated_tiles"), Populated); Row->SetNumberField(TEXT("polygons"), Polygons);
		Meshes.Add(Value(Row));
		if (!Populated) { Error = TEXT("Navigation agent has no populated tiles"); return false; }
	}
	Report->SetArrayField(TEXT("navigation_meshes"), Meshes);
	Report->SetNumberField(TEXT("navigation_agents"), Agents);
	Report->SetNumberField(TEXT("navigation_build_ms"), 1000 * (FPlatformTime::Seconds() - NavStart));
	if (Agents != 2) { Error = TEXT("Expected both navigation agents"); return false; }
	W->GetWorldSettings()->DefaultGameMode = AWoTGameMode::StaticClass();
	const FString URLText = FString::Printf(TEXT("/Engine/Maps/Entry?BotFill=%d"), int32(Number(Report,TEXT("fill"))));
	FURL URL(nullptr, *URLText, TRAVEL_Absolute);
	if (!W->SetGameMode(URL)) { Error = TEXT("Could not create Arena rules"); return false; }
	AWoTGameMode* Game = W->GetAuthGameMode<AWoTGameMode>();
	Game->SetInstalledMapsForTesting({Map});
	for (TActorIterator<AWoTAngrealSpawnPool> It(W); It; ++It) Observer.Pool(*It);
	Observer.DescribeReachability();
	if (FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_BOT_SOAK_NAVDUMP")) == TEXT("1"))
		Observer.DumpNavigation(FPaths::GetPath(Output) / (TEXT("NavDump-") + FPaths::GetCleanFilename(Output)));
	W->InitializeActorsForPlay(URL);
	W->BeginPlay();
	// A connected spectator keeps this unattended gameplay measurement active without taking a fill slot.
	APlayerState* ObserverState = W->SpawnActor<APlayerState>(Game->PlayerStateClass);
	ObserverState->SetIsSpectator(true);
	ObserverState->SetIsOnlyASpectator(true);
	W->GetSubsystem<UWoTBotRoster>()->Evaluate();
	Observer.Sample();
	const int32 Steps = FMath::CeilToInt(Number(Report,TEXT("requested_seconds")) * 30.0);
	for (int32 Index = 0; Index < Steps; ++Index)
	{
		Observer.Time = (Index + 1) / 30.0; Logs.Time.Store(Observer.Time);
		const double TickStart = FPlatformTime::Seconds();
		W->Tick(LEVELTICK_All, Step); ++GFrameCounter;
		Observer.TickTimes.Add((FPlatformTime::Seconds() - TickStart) * 1000);
		Observer.Sample();
		if ((Index + 1) % 900 == 0)
		{
			Observer.Snapshot();
			if (!Save(Report, Output)) { Error = TEXT("Could not persist worker checkpoint"); return false; }
			UE_LOG(LogTemp, Display, TEXT("BOT_SOAK_PROGRESS map=%s seconds=%.0f kills=%.0f"), *Map, Observer.Time, Number(Report,TEXT("kills")));
		}
	}
	Observer.Snapshot();
	Report->SetBoolField(TEXT("completed"), true);
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotArenaNavChurnSpec, "WoT.Bots.Navigation.Churn.ArenaMatch", Flags)
bool FBotArenaNavChurnSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::ProjectSavedDir() / TEXT("BotSoak/NavChurnSpec");
	IFileManager::Get().MakeDirectory(*Directory, true);
	FObject R = NewReport(TEXT("Arena_01"), 8, 60);
	FString Error;
	if (!RunMap(R, Directory / TEXT("Arena_01.json"), Error)) { AddError(Error); return false; }
	TestEqual(TEXT("eight bots in the real map match"), R->GetArrayField(TEXT("bots")).Num(), 8);
	TestEqual(TEXT("complete sixty second match sample"), Number(R,TEXT("simulated_seconds")), 60.0);
	const double Rate = (Number(R,TEXT("navigation_tile_updates"))
		- Number(R,TEXT("navigation_tile_updates_first_checkpoint"))) / 30.0;
	AddInfo(FString::Printf(TEXT("NAV_CHURN map=Arena_01 steady_tiles_per_second=%.3f"), Rate));
	// Baseline: 55.56 updates/s, exclusively projectile sources on this map.
	// Allow 30 late setup updates over the steady 30-second window, rather than
	// depending on exact asynchronous build completion ordering.
	TestTrue(TEXT("steady match stays below one tile update per second"), Rate < 1.0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSoakPolicySpec, "WoT.Bots.SoakDiagnostics.FailurePolicy", Flags)
bool FBotSoakPolicySpec::RunTest(const FString&)
{
	TestFalse(TEXT("healthy map passes"), Failed(true,0,0,0,1));
	TestTrue(TEXT("zero kills fails"), Failed(true,0,0,0,0));
	TestTrue(TEXT("ensure fails"), Failed(true,1,0,0,1));
	TestTrue(TEXT("error fails"), Failed(true,0,1,0,1));
	TestTrue(TEXT("bad actor fails"), Failed(true,0,0,1,1));
	TestTrue(TEXT("crash/incomplete fails"), Failed(false,0,0,0,1));
	TestFalse(TEXT("infinity rejected"), Finite(FVector(std::numeric_limits<double>::infinity(),0,0)));
	TestFalse(TEXT("NaN rejected"), Finite(FVector(0,std::numeric_limits<double>::quiet_NaN(),0)));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSoakEscalationSpec, "WoT.Bots.SoakDiagnostics.RunawayThenNonFinite", Flags)
bool FBotSoakEscalationSpec::RunTest(const FString&)
{
	UWorld* W = World();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	UClass* FragmentClass = FindObject<UClass>(nullptr,TEXT("/Script/WoT.WoTFragment"));
	AActor* Fragment = W->SpawnActor<AActor>(FragmentClass,FVector(0,0,100),FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("sampled fragment"),Fragment)) return false;
	FStructProperty* Property = FindFProperty<FStructProperty>(FragmentClass,TEXT("Velocity"));
	if (!TestNotNull(TEXT("custom velocity"),Property)) return false;
	FVector& Velocity = *Property->ContainerPtrToValuePtr<FVector>(Fragment);
	FObject R = NewReport(TEXT("diagnostics"),8,180);
	FLogCapture Logs; FObserver Observer(W,R,Logs);
	Velocity = FVector(1000000,0,0); Observer.Time = 1; Observer.Sample();
	Observer.Sample();
	TestEqual(TEXT("runaway recorded once"),int32(Number(R,TEXT("runaway"))),1);
	Velocity.X = std::numeric_limits<double>::infinity(); Observer.Time = 2; Observer.Sample();
	Observer.Sample(); Observer.Snapshot();
	TestEqual(TEXT("later non-finite transition is still sampled"),int32(Number(R,TEXT("non_finite"))),1);
	TestEqual(TEXT("each anomaly kind recorded once"),R->GetArrayField(TEXT("actor_anomalies")).Num(),2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSoakSpec, "WoT.Bots.Soak.AllArenaMaps", Flags)
bool FBotSoakSpec::RunTest(const FString&)
{
	if (FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_BOT_SOAK")) != TEXT("1"))
	{
		AddInfo(TEXT("SKIPPED: opt-in bot soak; set WOT_BOT_SOAK=1 to run all Arena maps (default 180 seconds/map, BotFill=8)."));
		return true;
	}
	int32 Fill = 8; double Seconds = 180;
	const FString FillText = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_BOT_SOAK_FILL"));
	const FString SecondsText = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_BOT_SOAK_SECONDS"));
	if ((!FillText.IsEmpty() && !LexTryParseString(Fill,*FillText)) || Fill < 1 || Fill > 16
		|| (!SecondsText.IsEmpty() && !LexTryParseString(Seconds,*SecondsText)) || !FMath::IsFinite(Seconds) || Seconds <= 0 || Seconds > 86400)
	{ AddError(TEXT("Invalid soak settings: fill must be 1..16 and seconds must be finite in (0,86400].")); return false; }
	FString WorkerMap, WorkerOutput;
	if (FParse::Value(FCommandLine::Get(), TEXT("WotBotSoakMap="), WorkerMap))
	{
		if (!FParse::Value(FCommandLine::Get(), TEXT("WotBotSoakOutput="), WorkerOutput)) { AddError(TEXT("Worker requires WotBotSoakOutput")); return false; }
		FObject R = NewReport(WorkerMap, Fill, Seconds); FString Error;
		if (!Save(R,WorkerOutput)) { AddError(TEXT("Cannot create worker report")); return false; }
		const bool bComplete = RunMap(R,WorkerOutput,Error);
		const bool bFailed = Failed(bComplete,int32(Number(R,TEXT("ensures"))),int32(Number(R,TEXT("errors"))),int32(Number(R,TEXT("non_finite"))+Number(R,TEXT("runaway"))),int32(Number(R,TEXT("kills"))));
		R->SetStringField(TEXT("result"), bFailed ? TEXT("FAIL") : TEXT("PASS"));
		R->SetStringField(TEXT("setup_error"), Error);
		if (!Save(R,WorkerOutput)) AddError(TEXT("Cannot save worker report"));
		if (bFailed) AddError(FString::Printf(TEXT("Soak map failed: %s; %s; see %s"), *WorkerMap,*Error,*WorkerOutput));
		return !bFailed && !HasAnyErrors();
	}
	const double Started = FPlatformTime::Seconds();
	FString Directory;
	if (!FParse::Value(FCommandLine::Get(), TEXT("WotBotSoakDirectory="), Directory))
		Directory = FPaths::ProjectSavedDir() / TEXT("BotSoak") / FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
	Directory = FPaths::ConvertRelativePathToFull(Directory);
	IFileManager::Get().MakeDirectory(*Directory,true);
	AddInfo(FString::Printf(TEXT("Bot soak reports: %s"), *Directory));
	const auto Contract = WoTLoadProductionRetailContract();
	if (Contract.IsError()) { AddError(Contract.GetError().ToString()); return false; }
	TArray<FString> Names;
	for (const auto& Fact : Contract.GetValue().Facts)
		if (Fact.Domain == EWoTFactDomain::World && FPaths::GetBaseFilename(Fact.SourcePath).StartsWith(TEXT("Arena_"))) Names.AddUnique(FPaths::GetBaseFilename(Fact.SourcePath));
	Names.Sort();
	if (Names.IsEmpty()) { AddError(TEXT("No Arena maps in production contract")); return false; }
	// Optional subset for iterating on one map: WOT_BOT_SOAK_MAPS=Arena_06,Arena_11.
	TArray<FString> Subset;
	FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_BOT_SOAK_MAPS")).ParseIntoArray(Subset, TEXT(","));
	if (!Subset.IsEmpty())
	{
		Names.RemoveAll([&Subset](const FString& Map) { return !Subset.Contains(Map); });
		if (Names.IsEmpty()) { AddError(TEXT("WOT_BOT_SOAK_MAPS names no Arena map")); return false; }
	}
	// Workers inherit the parent's core limit.
	int32 CoreLimit = 0;
	const FString WorkerLimit = FParse::Value(FCommandLine::Get(), TEXT("corelimit="), CoreLimit) && CoreLimit > 0
		? FString::Printf(TEXT(" -corelimit=%d"), CoreLimit) : FString();
	TArray<FObject> Reports;
	for (const FString& Map : Names)
	{
		const FString Output = Directory / (Map + TEXT(".json"));
		const FString Log = Directory / (Map + TEXT(".log"));
		const FString Args = FString::Printf(TEXT("\"%s\" \"-ExecCmds=Automation RunTests WoT.Bots.Soak.AllArenaMaps; Quit\" -unattended -nullrhi -nosplash -nopause -nocrashreports -stdout -abslog=\"%s\" -WotBotSoakMap=\"%s\" -WotBotSoakOutput=\"%s\"%s"),
			*FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), *Log, *Map, *Output, *WorkerLimit);
		UE_LOG(LogTemp, Display, TEXT("BOT_SOAK_START map=%s seed=%d log=%s"), *Map, MapSeed(Map), *Log);
		FProcHandle Process = FPlatformProcess::CreateProc(FPlatformProcess::ExecutablePath(), *Args, false, true, true, nullptr, 0, nullptr, nullptr);
		int32 ExitCode = -1; bool bTimedOut = false;
		const double WorkerStart = FPlatformTime::Seconds();
		if (Process.IsValid())
		{
			while (FPlatformProcess::IsProcRunning(Process))
			{
				if (FPlatformTime::Seconds() - WorkerStart > FMath::Max(1200.0, Seconds*10))
				{ bTimedOut = true; FPlatformProcess::TerminateProc(Process,true); break; }
				FPlatformProcess::Sleep(.1f);
			}
			FPlatformProcess::WaitForProc(Process);
			FPlatformProcess::GetProcReturnCode(Process,&ExitCode);
			FPlatformProcess::CloseProc(Process);
		}
		FObject R = NewReport(Map,Fill,Seconds); FString Json;
		TSharedPtr<FJsonObject> Loaded;
		if (FFileHelper::LoadFileToString(Json,*Output) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),Loaded) && Loaded.IsValid()) R = Loaded.ToSharedRef();
		R->SetNumberField(TEXT("exit_code"),ExitCode); R->SetBoolField(TEXT("timed_out"),bTimedOut);
		R->SetStringField(TEXT("worker_log"),Log);
		R->SetNumberField(TEXT("process_wall_seconds"),FPlatformTime::Seconds()-WorkerStart);
		if (ExitCode != 0 || bTimedOut || R->GetStringField(TEXT("result")) != TEXT("PASS")) R->SetStringField(TEXT("result"),TEXT("FAIL"));
		Reports.Add(R);
		FObject Combined = Object(); TArray<TSharedPtr<FJsonValue>> Rows;
		for (const auto& Item : Reports) Rows.Add(Value(Item));
		Combined->SetArrayField(TEXT("maps"),Rows);
		Combined->SetNumberField(TEXT("wall_seconds"),FPlatformTime::Seconds()-Started);
		if (!Save(Combined,Directory/TEXT("report.json")) || !FFileHelper::SaveStringToFile(Markdown(Reports,FPlatformTime::Seconds()-Started),*(Directory/TEXT("report.md")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			AddError(TEXT("Could not write aggregate soak report"));
		AddInfo(SummaryRow(R));
	}
	for (const auto& R : Reports) if (R->GetStringField(TEXT("result")) != TEXT("PASS")) AddError(FString::Printf(TEXT("Soak failed on %s; evidence: %s"),*R->GetStringField(TEXT("map")),*Directory));
	AddInfo(FString::Printf(TEXT("Bot soak finished in %.2f seconds; %s"),FPlatformTime::Seconds()-Started,*(Directory/TEXT("report.md"))));
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotNestedSwapCounterSpec,
	"WoT.Bots.Soak.NestedSwapTargetCounting", WoTBotTest::Flags)
bool FBotNestedSwapCounterSpec::RunTest(const FString&)
{
	UWorld* W = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTCharacter* Source = W->SpawnActor<AWoTCharacter>();
	APawn* OuterTarget = W->SpawnActor<APawn>();
	AActor* InnerTarget = W->SpawnActor<AActor>();
	FObject Report = NewReport(TEXT("observer fixture"), 0, 0);
	FLogCapture Logs;
	FObserver Observer(W, Report, Logs);
	{
		WoTSoakEvents::FBodyScope Outer(*FString::Printf(TEXT("target=%s rotation=outer"), *OuterTarget->GetName()));
		WoTSoakEvents::FBodyScope Inner(*FString::Printf(TEXT("target=%s rotation=inner"), *InnerTarget->GetName()));
		WoTSoakEvents::BodyEvents().Broadcast(Source, TEXT("swap source placed or rolled back"));
		// The rollback callback is a second observation of the same attempt.
		WoTSoakEvents::BodyEvents().Broadcast(Source, TEXT("swap source placed or rolled back"));
	}
	const auto Counts = Report->GetObjectField(TEXT("non_pawn_swap_targets_by_class")).ToSharedRef();
	TestEqual(TEXT("nested attempt counts its actual non-pawn far end once"), Number(Counts, TEXT("Actor")), 1.0);
	if (TestEqual(TEXT("rollback does not duplicate the attempt"), Observer.SwapEvents.Num(), 1))
		TestEqual(TEXT("record names the inner target"), Observer.SwapEvents[0]->AsObject()->GetStringField(TEXT("target")), InnerTarget->GetName());
	return !HasAnyErrors();
}
} // namespace WoTBotSoak
#endif
