#include "WoTZoneVolume.h"

#include "Projectiles/WoTGenericProjectile.h"
#include "Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "WoTCharacter.h"
#include "WoTCarcass.h"
#include "WoTCreatureChunk.h"
#include "AI/WoTNPCController.h"
#include "WoTMeshLight.h"
#include "WoTMapLightTable.h"
#include "WoTGameMode.h"
#include "WoTEvents.h"
#include "WoTLocalPlayerHooks.h"
#include "WoTSoundTable.h"
#include "Payload/WoTPayloadActivation.h"
#include "Inventory/WoTAngrealInvLightning.h"
#include "Inventory/WoTLeech.h"
#include "Inventory/WoTZoneLeeches.h"

#include "Components/BoxComponent.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/DamageType.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UObjectGlobals.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	// A body whose falling physics is running this frame. The movement
	// component simulates a pawn only while something controls it, so an
	// uncontrolled body that merely holds the falling mode is not falling.
	bool IsRunningFallingPhysics(const AWoTCharacter& Body)
	{
		const UCharacterMovementComponent* Move = Body.GetCharacterMovement();
		return Move && Move->IsActive() && Move->MovementMode == MOVE_Falling
			&& (Body.GetController() != nullptr || Move->bRunPhysicsWithNoController);
	}
}

void UWoTZoneIndex::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UWorld* World = GetWorld();
	if (World == nullptr || !IsWoTBootstrapWorldReady())
	{
		return;
	}
	for (auto It = ActorZones.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			// A normal pawn teardown calls RemoveActor while the object is still
			// usable. This is the last-resort disconnect/GC path: the retained bit
			// still owns exactly one authority count even though no pawn remains.
			if (It.Value().bCountedPlayer)
			{
				if (AWoTZoneVolume* Region = It.Value().Region.Get())
				{
					Region->RemoveCountedPlayer(nullptr);
				}
			}
			It.RemoveCurrent();
		}
	}
#if WITH_AUTOMATION_TESTS
	for (auto It = UpdateCountsForTest.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid()) It.RemoveCurrent();
	}
#endif
	// Tickable world subsystems run after actor movement. Retail calls
	// SetActorZone after every MoveActor; this is the one authoritative ordinary
	// post-movement pass. Explicit SetLocation/FarMove seams refresh immediately.
	//
	// The original's falling physics begins every step by checking the zone the
	// last move left the actor in, and a pawn falling outside the level dies
	// there. The server makes that check here, as soon as the move that took the
	// body out has been refreshed. The deaths run after the walk, since they
	// spawn and destroy actors.
	TArray<AWoTCharacter*, TInlineAllocator<4>> FellOut;
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		if (!IsValid(*It) || It->IsActorBeingDestroyed()) continue;
		UpdateActorZones(*It);
		AWoTCharacter* Body = Cast<AWoTCharacter>(*It);
		if (Body && Body->HasAuthority() && !Body->bDead && IsOutsideWorld(Body)
			&& IsRunningFallingPhysics(*Body))
		{
			FellOut.Add(Body);
		}
	}
	for (AWoTCharacter* Body : FellOut)
	{
		if (IsValid(Body) && !Body->IsActorBeingDestroyed())
		{
			Body->FellOutOfWorld(*GetDefault<UDamageType>());
		}
	}
	for (TActorIterator<AWoTCarcass> It(World); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed()) UpdateActorZones(*It);
	}
	for (TActorIterator<AWoTCreatureChunk> It(World); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed()) UpdateActorZones(*It);
	}
	// Tickable objects run after every actor/component tick group, so each weave
	// has finished this frame's move. UpdateWaterZone may destroy the weave
	// (Fireball, Fireworks, Explosive Ward), so collect first.
	TArray<AWoTGenericProjectile*, TInlineAllocator<32>> Weaves;
	for (TActorIterator<AWoTGenericProjectile> It(World); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed())
		{
			Weaves.Add(*It);
		}
	}
	for (AWoTGenericProjectile* Weave : Weaves)
	{
		if (IsValid(Weave))
		{
			Weave->UpdateWaterZone();
		}
	}
}

AWoTZoneVolume* UWoTZoneIndex::GetRegion(const AActor* Actor) const
{
	if (const FActorZones* State = ActorZones.Find(Actor)) return State->Region.Get();
	return nullptr;
}

void UWoTZoneIndex::RefreshAfterRelocation(AActor* Actor)
{
	if (!IsValid(Actor) || !Actor->GetWorld()) return;
	if (UWoTZoneIndex* Zones = Actor->GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		Zones->UpdateActorZones(Actor);
	}
	// Every gameplay relocation (teleporters, swaps, pits) passes here; a local
	// player's pawn reports it to the local-player hooks, which announce a jump
	// beyond their teleport distance (inert without them).
	FWoTLocalPlayerHooks::PawnRelocated(Actor);
}

AWoTZoneVolume* UWoTZoneIndex::GetFootRegion(const APawn* Pawn) const
{
	if (const FActorZones* State = ActorZones.Find(Pawn)) return State->FootRegion.Get();
	return nullptr;
}

AWoTZoneVolume* UWoTZoneIndex::GetHeadRegion(const APawn* Pawn) const
{
	if (const FActorZones* State = ActorZones.Find(Pawn)) return State->HeadRegion.Get();
	return nullptr;
}

int32 UWoTZoneIndex::ZoneNumberAt(const UWorld* World, const FVector& Point)
{
	int32 Zone = 0;
	int32 Leaf = INDEX_NONE;
	return UWoTMeshLight::ClassifyRegion(UWoTMeshLight::FindMapLightTable(World), Point,
		Zone, Leaf) ? Zone : INDEX_NONE;
}

int32 UWoTZoneIndex::GetZoneNumber(const AActor* Actor) const
{
	if (const FActorZones* State = ActorZones.Find(Actor)) return State->ZoneNumber;
	return INDEX_NONE;
}

void UWoTZoneIndex::UpdateActorZones(AActor* Actor, bool bForceRefresh)
{
	if (!IsValid(Actor) || Actor->IsActorBeingDestroyed()) return;
	if (bForceRefresh && ActorZones.Contains(Actor))
	{
		RemoveActor(Actor);
	}
#if WITH_AUTOMATION_TESTS
	++UpdateCountsForTest.FindOrAdd(Actor);
#endif
	FActorZones& State = ActorZones.FindOrAdd(Actor);
	if (bForceRefresh || !State.bInitialized)
	{
		State.Region.Reset(); State.FootRegion.Reset(); State.HeadRegion.Reset();
		State.bInitialized = true;
	}
	State.ZoneNumber = ZoneNumberAt(GetWorld(), Actor->GetActorLocation());

	AWoTZoneVolume* NewRegion = AWoTZoneVolume::ZoneAt(GetWorld(), Actor->GetActorLocation());
	AWoTZoneVolume* OldRegion = State.Region.Get();
	if (NewRegion != OldRegion)
	{
		// UE1's ULevel::SetActorZone: old leaving, actor callback,
		// assignment, then new entered.
		if (OldRegion) OldRegion->ActorLeaving(Actor);
		State.bCountedPlayer = false;
		if (AWoTCarcass* Carcass = Cast<AWoTCarcass>(Actor))
		{
			Carcass->HandleZoneChange(NewRegion, OldRegion);
		}
		else if (AWoTCreatureChunk* Chunk = Cast<AWoTCreatureChunk>(Actor))
		{
			Chunk->HandleZoneChange(NewRegion, OldRegion);
		}
		if (APawn* Pawn = Cast<APawn>(Actor); Pawn && Pawn->HasAuthority()
			&& NewRegion && NewRegion->bWaterZone)
		{
			if (AWoTNPCController* NPCController = Cast<AWoTNPCController>(Pawn->GetController()))
			{
				NPCController->HandleFallingSplash();
			}
		}
		State.Region = NewRegion;
		if (NewRegion)
		{
			NewRegion->ActorEntered(Actor);
			const AWoTCharacter* Player = Cast<AWoTCharacter>(Actor);
			State.bCountedPlayer = Player && !Player->IsNPCPawn()
				&& Player->HasAuthority();
		}
	}

	APawn* Pawn = Cast<APawn>(Actor);
	if (!Pawn) return;
	float Radius = 0.0f, HalfHeight = 0.0f;
	Pawn->GetSimpleCollisionCylinder(Radius, HalfHeight);
	const FVector Location = Pawn->GetActorLocation();
	AWoTZoneVolume* NewFoot = AWoTZoneVolume::ZoneAt(
		GetWorld(), Location - FVector(0, 0, HalfHeight));
	if (NewFoot != State.FootRegion.Get())
	{
		// Pawn.FootZoneChange is not simulated: authority owns gameplay.
		if (Pawn->HasAuthority())
		{
			AWoTAngrealInvLightning::NotifyFootZoneChange(Pawn, NewFoot);
		}
		State.FootRegion = NewFoot;
	}
	const float EyeHeight = Pawn->BaseEyeHeight;
	AWoTZoneVolume* NewHead = AWoTZoneVolume::ZoneAt(
		GetWorld(), Location + FVector(0, 0, EyeHeight));
	if (NewHead != State.HeadRegion.Get())
	{
		// No shipped HeadZoneChange override exists; retain the exact region
		// state for breath/view consumers and assign after the (empty) event.
		State.HeadRegion = NewHead;
	}
}

void UWoTZoneIndex::RemoveActor(AActor* Actor)
{
	if (!Actor) return;
	FActorZones* Found = ActorZones.Find(Actor);
	if (!Found) return;
	const FActorZones State = *Found;
	AWoTZoneVolume* OldRegion = State.Region.Get();
	if (OldRegion)
	{
		OldRegion->ActorLeaving(Actor);
	}
	if (AWoTCarcass* Carcass = Cast<AWoTCarcass>(Actor))
	{
		Carcass->HandleZoneChange(nullptr, OldRegion);
	}
	else if (AWoTCreatureChunk* Chunk = Cast<AWoTCreatureChunk>(Actor))
	{
		Chunk->HandleZoneChange(nullptr, OldRegion);
	}
	if (APawn* Pawn = Cast<APawn>(Actor); Pawn && Pawn->HasAuthority()
		&& State.FootRegion.IsValid())
	{
		AWoTAngrealInvLightning::NotifyFootZoneChange(Pawn, nullptr);
	}
	ActorZones.Remove(Actor);
#if WITH_AUTOMATION_TESTS
	UpdateCountsForTest.Remove(Actor);
#endif
}

AWoTZoneVolume::AWoTZoneVolume()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	Collision = CreateDefaultSubobject<UBoxComponent>(TEXT("Collision"));
	Collision->InitBoxExtent(FVector(100.0f));
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);   // just the extent
	RootComponent = Collision;
}

void AWoTZoneVolume::BeginPlay()
{
	Super::BeginPlay();
	if (!IsWoTBootstrapWorldReady())
	{
		return;
	}
	if (UWoTZoneIndex* Index = GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		Index->Zones.Add(this);
	}
	if (bTrigZoneVelo2)
	{
		if (GetNetMode() == NM_Client)
		{
			// A client's own copy of the map zone follows the server. A copy made
			// after its world began play never passed the level's network role
			// exchange, so it still claims authority here.
			if (GetLocalRole() == ROLE_Authority) SetRole(ROLE_SimulatedProxy);
		}
		else if (HasAuthority())
		{
			// Only the on/off state replicates (TriggeredCurrent); every machine
			// derives the same ZoneVelocity from it and its own authored settings.
			SetReplicates(true);
			SetReplicateMovement(false);
			bAlwaysRelevant = true;
			GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
		}
		// WOTZoneInfo.uc:78-92 PreBeginPlay starts in the authored state. A client
		// that already received the server's state keeps it.
		if (TriggeredCurrent == EWoTTriggeredCurrent::Unset)
		{
			TriggeredCurrent = bTrigZoneVelo2InitiallyOn
				? EWoTTriggeredCurrent::On : EWoTTriggeredCurrent::Off;
		}
		ApplyTriggeredCurrent();
	}
	SetActorTickEnabled(true);
}

void AWoTZoneVolume::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTZoneVolume, TriggeredCurrent);
}

FVector AWoTZoneVolume::SteadyTriggeredVelocity(const FVector& Max, const FVector& Min,
	float TransitionTime)
{
	// WOTZoneInfo.uc:104-128 adds VeloPerSec2 = (Max - Min) / TransitionTime2
	// once per server tick, without DeltaTime, and an "on" zone already at Max
	// takes the unguarded else branch one step back toward Min. So an "on" zone
	// repeats Max, Max - Step, where Step is VeloPerSec2 clamped to the whole
	// range (Util.uc:958 VectorAproxEqual detects the overshoot). Bodies moved
	// with that cycle's average. A step spans the range when TransitionTime2 is
	// at most 1 (every shipped triggered zone: Arena_11's .5), giving
	// (Max + Min) / 2. A longer transition gives Max - (Max - Min) / (2 *
	// TransitionTime2). A non-positive time is outside the authored contract
	// (WOTZoneInfo.uc:56 "greater than zero") and counts as spanning.
	const double StepFraction = TransitionTime > 1.0f ? 1.0 / TransitionTime : 1.0;
	return Max - (Max - Min) * (0.5 * StepFraction);
}

void AWoTZoneVolume::ApplyTriggeredCurrent()
{
	ZoneVelocity = TriggeredCurrent == EWoTTriggeredCurrent::On
		? SteadyTriggeredVelocity(MaxZoneVelocity2, MinZoneVelocity2, TransitionTime2)
		: MinZoneVelocity2;
}

void AWoTZoneVolume::OnRep_TriggeredCurrent()
{
	if (bTrigZoneVelo2) ApplyTriggeredCurrent();
}

void AWoTZoneVolume::SetTriggeredCurrent(bool bOn)
{
	// The original switched within one server tick of the trigger (the shipped
	// step spans the whole range); the steady current switches at once.
	TriggeredCurrent = bOn ? EWoTTriggeredCurrent::On : EWoTTriggeredCurrent::Off;
	ApplyTriggeredCurrent();
	ForceNetUpdate();
}

void AWoTZoneVolume::RestoreTriggeredCurrent()
{
	if (HasAuthority() && bTrigZoneVelo2 && IsWoTBootstrapWorldReady())
		SetTriggeredCurrent(bTrigZoneVelo2InitiallyOn);
}

void AWoTZoneVolume::WoTTrigger(AActor*, APawn*)
{
	if (!HasAuthority() || !bTrigZoneVelo2 || !IsWoTBootstrapWorldReady()) return;
	// WOTZoneInfo.uc:160-222: Toggle flips; Control and Timed select the
	// opposite of the authored initial state, and Timed (re)arms its timer.
	// In any other state the zone ignores the trigger.
	if (InitialState == TEXT("TriggerToggle"))
		SetTriggeredCurrent(TriggeredCurrent != EWoTTriggeredCurrent::On);
	else if (InitialState == TEXT("TriggerControl") || InitialState == TEXT("TriggerTimed"))
	{
		SetTriggeredCurrent(!bTrigZoneVelo2InitiallyOn);
		if (InitialState == TEXT("TriggerTimed"))
			GetWorldTimerManager().SetTimer(TriggerCurrentTimer, this,
				&AWoTZoneVolume::RestoreTriggeredCurrent, TriggerDuration, false);
	}
}

void AWoTZoneVolume::WoTUnTrigger(AActor*, APawn*)
{
	// WOTZoneInfo.uc:195-201: only TriggerControl restores on UnTrigger.
	if (InitialState == TEXT("TriggerControl")) RestoreTriggeredCurrent();
}

void AWoTZoneVolume::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(TriggerCurrentTimer);
		if (UWoTEventSubsystem* Events = World->GetSubsystem<UWoTEventSubsystem>())
			Events->UnregisterActor(this);
		if (UWoTZoneIndex* Index = World->GetSubsystem<UWoTZoneIndex>())
		{
			Index->Zones.Remove(this);
		}
	}
	Super::EndPlay(Reason);
}

bool AWoTZoneVolume::ContainsPoint(const FVector& Point) const
{
	if (!Collision)
	{
		return false;
	}
	// Compare in the box's UNSCALED local space: InverseTransformPosition already divides
	// out the component scale, so the extent must be unscaled too (comparing against the
	// SCALED extent would double-count scale). Use the box's own component transform.
	const FVector Ext = Collision->GetUnscaledBoxExtent();
	const FVector Rel =
		Collision->GetComponentTransform().InverseTransformPosition(Point);
	if (FMath::Abs(Rel.X) > Ext.X || FMath::Abs(Rel.Y) > Ext.Y
		|| FMath::Abs(Rel.Z) > Ext.Z) return false;
	if (ProjectedZoneIndex == INDEX_NONE) return true;
	UWoTMapLightTable* Table = UWoTMeshLight::FindMapLightTable(GetWorld());
	int32 Zone = 0, Leaf = INDEX_NONE;
	// Missing/malformed tables occur in editor-imported maps: preserve their
	// historical box behavior rather than making every volume empty.
	return !UWoTMeshLight::ClassifyRegion(Table, Point, Zone, Leaf)
		|| Zone == ProjectedZoneIndex;
}

AWoTZoneVolume* AWoTZoneVolume::ZoneAt(const UWorld* World, const FVector& Point)
{
	if (!World || !IsWoTBootstrapWorldReady()) return nullptr;
	const UWoTZoneIndex* Index = World->GetSubsystem<UWoTZoneIndex>();
	if (!Index) return nullptr;
	for (const TObjectPtr<AWoTZoneVolume>& Zone : Index->Zones)
	{
		if (IsValid(Zone) && Zone->ContainsPoint(Point)) return Zone;
	}
	return nullptr;
}

void AWoTZoneVolume::ActorEntered(AActor* Other)
{
	AWoTCharacter* Player = Cast<AWoTCharacter>(Other);
	if (!Player || Player->IsNPCPawn()) return;
	ReconcileOwnerLocalFOV(Player);
	if (bLeechZone)
	{
		for (const TSubclassOf<UWoTLeech>& LeechClass : LeechClasses)
		{
			if (!LeechClass) continue;
			if (LeechClass->IsChildOf(UWoTSmoothRandFOVLeech::StaticClass())) continue;
			if (!Player->HasAuthority()) continue;
			UWoTLeech* Leech = NewObject<UWoTLeech>(Player, LeechClass,
				MakeUniqueObjectName(Player, LeechClass, GetFName()));
			Leech->ComponentTags.AddUnique(GetFName());
			Leech->bTransferring = true; Leech->AttachTo(Player); Leech->bTransferring = false;
		}
	}
	if (!Player->HasAuthority()) return;
	const AWoTGameMode* Game = Cast<AWoTGameMode>(UGameplayStatics::GetGameMode(this));
	const int32 Difficulty = FMath::Clamp(Game ? Game->Difficulty : 1, 0, 2);
	const float Duration = TimerLeechDuration.IsValidIndex(Difficulty)
		? TimerLeechDuration[Difficulty] : 0.0f;
	if (Duration > 0.0f && (!bAttachedTimer || !bOnlyTimeOnce))
	{
		UWoTTimerLeech* Timer = NewObject<UWoTTimerLeech>(Player,
			MakeUniqueObjectName(Player, UWoTTimerLeech::StaticClass(), GetFName()));
		Timer->ComponentTags.AddUnique(GetFName()); Timer->Lifespan = Duration;
		Timer->StatusIconFrame = TimerLeechIcon; Timer->bSingular = bPersistentTimer;
		Timer->bDeleterious = bTimerOnRight;
		Timer->bTransferring = true; Timer->AttachTo(Player); Timer->bTransferring = false;
		bAttachedTimer = true;
	}
	++ZonePlayerCount;
	if (ZonePlayerCount == 1 && !ZonePlayerEvent.IsNone())
	{
		if (UWoTEventSubsystem* Events =
			GetWorld()->GetSubsystem<UWoTEventSubsystem>())
		{
			Events->FireEvent(ZonePlayerEvent, this, Player);
		}
	}
}

void AWoTZoneVolume::ActorLeaving(AActor* Other)
{
	AWoTCharacter* Player = Cast<AWoTCharacter>(Other);
	if (!Player || Player->IsNPCPawn()) return;
	DetachOwnerLocalFOV(Player);
	TArray<UWoTLeech*> Leeches; Player->GetComponents<UWoTLeech>(Leeches);
	for (UWoTLeech* Leech : Leeches)
	{
		if (!IsValid(Leech) || !Leech->ComponentHasTag(GetFName())) continue;
		if (Leech->IsA<UWoTSmoothRandFOVLeech>()) continue;
		if (Leech->IsA<UWoTTimerLeech>() && !bRemoveTimerOnExit) continue;
		Leech->Unattach();
	}
	if (Player->HasAuthority()) RemoveCountedPlayer(Player);
}

void AWoTZoneVolume::ReconcileOwnerLocalFOV(AWoTCharacter* Player)
{
	if (!IsValid(Player) || Player->IsNPCPawn()) return;
	TArray<UWoTSmoothRandFOVLeech*> Existing;
	Player->GetComponents<UWoTSmoothRandFOVLeech>(Existing);
	if (!WoTIsLocalHumanPawn(Player))
	{
		for (UWoTSmoothRandFOVLeech* Leech : Existing)
		{
			if (IsValid(Leech) && Leech->ComponentHasTag(GetFName())) Leech->Unattach();
		}
		return;
	}
	if (bFOVZone) Player->SetZoneFOV(ZoneFOV);

	TSet<UClass*> Desired;
	if (bLeechZone)
	{
		for (const TSubclassOf<UWoTLeech>& LeechClass : LeechClasses)
		{
			if (LeechClass
				&& LeechClass->IsChildOf(UWoTSmoothRandFOVLeech::StaticClass()))
			{
				Desired.Add(LeechClass.Get());
			}
		}
	}
	TSet<UClass*> Satisfied;
	for (UWoTSmoothRandFOVLeech* Leech : Existing)
	{
		if (!IsValid(Leech) || !Leech->ComponentHasTag(GetFName())) continue;
		UClass* Class = Leech->GetClass();
		if (!Desired.Contains(Class) || Satisfied.Contains(Class))
		{
			Leech->Unattach();
		}
		else
		{
			Satisfied.Add(Class);
		}
	}
	for (UClass* Class : Desired)
	{
		if (Satisfied.Contains(Class)) continue;
		UWoTLeech* Leech = NewObject<UWoTLeech>(Player, Class,
			MakeUniqueObjectName(Player, Class, GetFName()));
		Leech->ComponentTags.AddUnique(GetFName());
		Leech->bTransferring = true;
		Leech->AttachTo(Player);
		Leech->bTransferring = false;
	}
}

void AWoTZoneVolume::DetachOwnerLocalFOV(AWoTCharacter* Player)
{
	if (!Player) return;
	TArray<UWoTSmoothRandFOVLeech*> Leeches;
	Player->GetComponents<UWoTSmoothRandFOVLeech>(Leeches);
	for (UWoTSmoothRandFOVLeech* Leech : Leeches)
	{
		if (IsValid(Leech) && Leech->ComponentHasTag(GetFName())) Leech->Unattach();
	}
	if (bFOVZone) Player->ClearZoneFOV();
}

void AWoTZoneVolume::RemoveCountedPlayer(AWoTCharacter* Player)
{
	if (ZonePlayerCount <= 0)
	{
		ZonePlayerCount = 0;
		return;
	}
	--ZonePlayerCount;
	if (ZonePlayerCount == 0 && !ZonePlayerEvent.IsNone())
	{
		if (UWoTEventSubsystem* Events =
			GetWorld()->GetSubsystem<UWoTEventSubsystem>())
		{
			Events->FireUnEvent(ZonePlayerEvent, this, Player);
		}
	}
}

bool AWoTZoneVolume::IsPointInWater(const UWorld* World, const FVector& Point)
{
	if (!IsWoTBootstrapWorldReady())
	{
		return false;
	}
	const UWoTZoneIndex* Index = World ? World->GetSubsystem<UWoTZoneIndex>() : nullptr;
	if (!Index)
	{
		return false;
	}
	for (const TObjectPtr<AWoTZoneVolume>& Z : Index->Zones)
	{
		if (IsValid(Z) && Z->bWaterZone && Z->ContainsPoint(Point))
		{
			return true;
		}
	}
	return false;
}

AWoTZoneVolume* AWoTZoneVolume::WaterZoneAt(const UWorld* World, const FVector& Point)
{
	if (!IsWoTBootstrapWorldReady())
	{
		return nullptr;
	}
	const UWoTZoneIndex* Index = World ? World->GetSubsystem<UWoTZoneIndex>() : nullptr;
	if (!Index)
	{
		return nullptr;
	}
	for (const TObjectPtr<AWoTZoneVolume>& Z : Index->Zones)
	{
		if (IsValid(Z) && Z->bWaterZone && Z->ContainsPoint(Point))
		{
			return Z;
		}
	}
	return nullptr;
}

AWoTZoneVolume* AWoTZoneVolume::PainZoneAt(const UWorld* World, const FVector& Point)
{
	if (!IsWoTBootstrapWorldReady())
	{
		return nullptr;
	}
	const UWoTZoneIndex* Index = World ? World->GetSubsystem<UWoTZoneIndex>() : nullptr;
	if (!Index)
	{
		return nullptr;
	}
	for (const TObjectPtr<AWoTZoneVolume>& Z : Index->Zones)
	{
		if (IsValid(Z) && Z->bPainZone && Z->ContainsPoint(Point))
		{
			return Z;
		}
	}
	return nullptr;
}

AWoTZoneVolume* AWoTZoneVolume::PitZoneAt(const UWorld* World, const FVector& Point)
{
	if (!IsWoTBootstrapWorldReady())
	{
		return nullptr;
	}
	const UWoTZoneIndex* Index = World ? World->GetSubsystem<UWoTZoneIndex>() : nullptr;
	if (!Index)
	{
		return nullptr;
	}
	for (const TObjectPtr<AWoTZoneVolume>& Zone : Index->Zones)
	{
		if (IsValid(Zone) && Zone->bPitZone && Zone->ContainsPoint(Point))
		{
			return Zone;
		}
	}
	return nullptr;
}

AWoTZoneVolume* AWoTZoneVolume::EditZoneAt(const UWorld* World, const FVector& Point)
{
	if (!IsWoTBootstrapWorldReady())
	{
		return nullptr;
	}
	const UWoTZoneIndex* Index = World ? World->GetSubsystem<UWoTZoneIndex>() : nullptr;
	if (!Index)
	{
		return nullptr;
	}

	AWoTZoneVolume* BestZone = nullptr;
	double BestVolume = TNumericLimits<double>::Max();
	for (const TObjectPtr<AWoTZoneVolume>& Zone : Index->Zones)
	{
		if (!IsValid(Zone) || !IsValid(Zone->Collision) || !Zone->ContainsPoint(Point))
		{
			continue;
		}
		const FVector Extent = Zone->Collision->GetScaledBoxExtent();
		const double Volume = 8.0 * static_cast<double>(Extent.X)
			* static_cast<double>(Extent.Y) * static_cast<double>(Extent.Z);
		if (Volume < BestVolume)
		{
			BestZone = Zone;
			BestVolume = Volume;
		}
	}
	return BestZone;
}

bool AWoTZoneVolume::IsPointInPainZone(const UWorld* World, const FVector& Point)
{
	if (!IsWoTBootstrapWorldReady())
	{
		return false;
	}
	const UWoTZoneIndex* Index = World ? World->GetSubsystem<UWoTZoneIndex>() : nullptr;
	if (!Index)
	{
		return false;
	}
	for (const TObjectPtr<AWoTZoneVolume>& Z : Index->Zones)
	{
		if (IsValid(Z) && Z->bPainZone && Z->ContainsPoint(Point))
		{
			return true;
		}
	}
	return false;
}

bool AWoTZoneVolume::GetMovementEffectAt(const UWorld* World, const FVector& Point,
                                         FWoTZoneMovement& Out)
{
	if (!IsWoTBootstrapWorldReady())
	{
		return false;
	}
	const UWoTZoneIndex* Index = World ? World->GetSubsystem<UWoTZoneIndex>() : nullptr;
	if (!Index)
	{
		return false;
	}
	bool bAny = false;
	for (const TObjectPtr<AWoTZoneVolume>& Z : Index->Zones)
	{
		if (!IsValid(Z) || !Z->ContainsPoint(Point))
		{
			continue;
		}
		bAny = true;
		Out.Current += Z->ZoneVelocity;
		Out.GroundFriction = Z->ZoneGroundFriction;   // single-zone: this zone's; multi: last
		Out.FluidFriction = Z->ZoneFluidFriction;     // single-zone: this zone's; multi: last
		if (Z->bOverrideGravity)
		{
			Out.bOverrideGravity = true;
			Out.Gravity += Z->ZoneGravity;
		}
		if (Z->bWaterZone)
		{
			Out.bWater = true;
		}
	}
	return bAny;
}

FVector AWoTZoneVolume::GetGravityAt(const UWorld* World, const FVector& Point)
{
	FWoTZoneMovement Effect;
	return GetMovementEffectAt(World, Point, Effect) && Effect.bOverrideGravity
		? Effect.Gravity : FVector(0.0f, 0.0f, -950.0f);
}

void AWoTZoneVolume::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!IsWoTBootstrapWorldReady())
	{
		SetActorTickEnabled(false);
		return;
	}
	// Pawn pain/heal occupancy is authority-only. Weave water entry no longer
	// lives here: UWoTZoneIndex::Tick tracks each weave's Location point on every
	// machine (an overlap poll missed weaves with collision off, such as a placed
	// Explosive Ward, and fired a frame late or on the sphere's edge).
	if (!HasAuthority())
	{
		return;
	}

	// Poll for occupants inside the box (robust headless — same pattern as the
	// AngrealProjectile ProjTouch poll).
	TArray<FOverlapResult> Overlaps;
	FCollisionObjectQueryParams ObjTypes;
	ObjTypes.AddObjectTypesToQuery(ECC_Pawn);
	ObjTypes.AddObjectTypesToQuery(ECC_WorldDynamic);
	ObjTypes.AddObjectTypesToQuery(ECC_PhysicsBody);
	const FCollisionShape Box = FCollisionShape::MakeBox(Collision->GetScaledBoxExtent());
	GetWorld()->OverlapMultiByObjectType(Overlaps, GetActorLocation(),
	                                     GetActorQuat(), ObjTypes, Box);

	TSet<TWeakObjectPtr<APawn>> Now;
	for (const FOverlapResult& O : Overlaps)
	{
		AActor* A = O.GetActor();
		if (!IsValid(A) || A == this)
		{
			continue;
		}

		APawn* Pawn = Cast<APawn>(A);
		if (!Pawn)
		{
			continue;
		}
		Now.Add(Pawn);
		if (!Occupants.Contains(Pawn))
		{
			// Entered. ALL movement effects — current, zone gravity, ground friction, and
			// swim-mode entry — are applied by UWoTCharacterMovement's predicted path now
			// not written here; this map only tracks occupancy for the
			// pain/heal cadence below.
			Occupants.Add(Pawn);
		}
	}

	// Left the volume: drop the occupant's pain/heal state. Every movement effect (current,
	// gravity, friction, swim) reverts itself in the CMC once the pawn is outside every zone —
	// no CMC write here.
	for (auto It = Occupants.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid() || !Now.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}

	const bool bPain = bPainZone && DamagePerSec > 0.0f;
	const bool bHeal = bHealZone && HealPerSec > 0;

	for (TPair<TWeakObjectPtr<APawn>, FOccupantState>& Pair : Occupants)
	{
		APawn* Pawn = Pair.Key.Get();
		if (!IsValid(Pawn))
		{
			continue;
		}
		FOccupantState& St = Pair.Value;

		// Pain/heal cadence (UE1 Pawn FootZoneChange/PainTimer, WOTPlayer.uc PainTimer):
		// the per-pawn timer arms at 0.01s when the FEET enter the zone, fires,
		// then re-arms at PainInterval (UE1 PainTime = 1.0); feet out disarms.
		if (bPain || bHeal)
		{
			const FVector Loc = Pawn->GetActorLocation();
			const bool bFootIn = ContainsPoint(
				Loc - FVector(0, 0, Pawn->GetSimpleCollisionHalfHeight()));
			if (bFootIn && !St.bFootIn)
			{
				St.PainTime = 0.01f;   // FootZoneChange -> PainTime = 0.01
			}
			St.bFootIn = bFootIn;
			if (bFootIn)
			{
				St.PainTime -= DeltaTime;
				if (St.PainTime <= 0.0f)
				{
					St.PainTime = PainInterval;
					if (bPain)
					{
						// Damage scales with immersion: 0.4 feet + 0.4 body +
						// 0.2 head (Pawn.PainTimer), truncated to int as the
						// original's int() cast.
						float Depth = 0.4f;
						if (ContainsPoint(Loc))
						{
							Depth += 0.4f;
						}
						if (ContainsPoint(Loc + FVector(0, 0, Pawn->BaseEyeHeight)))
						{
							Depth += 0.2f;
						}
						FWoTDamageEvent Ev;
						Ev.WoTDamageType = DamageType;
						Ev.UE1Momentum = FVector::ZeroVector;   // Pawn.PainTimer: zero momentum
						Ev.HitInfo.ImpactPoint = Loc;
						Pawn->TakeDamage(FMath::TruncToFloat(DamagePerSec * Depth),
						                 Ev, nullptr, this);
					}
					if (bHeal)
					{
						// WOTPlayer.uc:3095-3103 — +HealPerSec, capped at full
						// health (IncreaseHealth clamps); NPCs stay silent.
						if (AWoTCharacter* C = Cast<AWoTCharacter>(Pawn))
						{
							if (C->Health > 0 && C->Health < 100)
							{
								UWorld* World = GetWorld();
								// The player body plays the heal sound
								// (WOTPlayer.uc:3091-3105) whoever drives it;
								// an NPC heals silently (WOTPawn.uc:505-517).
								if (!HealSoundPath.IsNone() && IsValid(World)
									&& WoTIsPlayerBody(C))
								{
									// WOTPlayer.uc PainTimer: the player's SLOT_Interact.
									WoTSound::PlayReplicatedFrom(*C, HealSoundPath,
										WoTSound::ESlot::Interact);
								}
								C->Health = FMath::Min(C->Health + HealPerSec, 100);
							}
						}
					}
				}
			}
		}

		// (No movement write here — current / gravity / friction / swim are all applied by
		// UWoTCharacterMovement in the predicted path. This loop is pain/heal only.)
	}
}
