#include "WoTTrapDetectLeech.h"
#include "../AI/WoTPathNodeIterator.h"

#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Traps/WoTTrap.h"
#include "../WoTMeshFX.h"
#include "../WoTParticleSprayer.h"
#include "../WoTPlayerController.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTWeaveFX.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

UWoTTrapDetectLeech::UWoTTrapDetectLeech()
{
	AffectResolution = 1.0f;
	bDisplayIcon = true;
	bRemoveExisting = true;
	DetectableTrapTypes = {
		TEXT("WallSlab"), TEXT("Pit"), TEXT("Portcullis"), TEXT("Spear"),
		TEXT("Staircase"), TEXT("AngrealExpWardProjectile"),
	};
	TrapIconInfos = {
		{TEXT("WallSlab"), TEXT("TrapIconWallSlab"), TEXT("TrapDetectWallSlab"), .3f},
		{TEXT("Pit"), TEXT("TrapIconPit"), TEXT("TrapDetectPit"), .3f},
		{TEXT("Portcullis"), TEXT("TrapIconPortcullis"), TEXT("TrapDetectPortcullis"), .3f},
		{TEXT("Spear"), TEXT("TrapIconSpear"), TEXT("TrapDetectSpear"), .4f},
		{TEXT("Staircase"), TEXT("TrapIconStaircase"), TEXT("TrapDetectStaircase"), .3f},
		{TEXT("AngrealExpWardProjectile"), TEXT("TrapIconAngrealExpWardProjectile"),
			TEXT("TrapDetectAngrealExpWardProjectile"), .3f},
	};
}

FName UWoTTrapDetectLeech::GetTrapID(const AActor* Trap)
{
	if (const AWoTTrap* T = Cast<AWoTTrap>(Trap))
	{
		if (!T->TrapType.IsNone()) { return T->TrapType; }
	}
	if (Cast<AWoTExpWardProjectile>(Trap))
	{
		return FName(TEXT("AngrealExpWardProjectile"));
	}
	return Trap ? Trap->GetClass()->GetFName() : NAME_None;
}

bool UWoTTrapDetectLeech::IsTrapRelevant(const AActor* Trap) const
{
	bool bDetectableState = false;
	if (const AWoTTrap* T = Cast<AWoTTrap>(Trap))
	{
		bDetectableState = T->IsTrapDeployed();
	}
	else if (const AWoTExpWardProjectile* Ward = Cast<AWoTExpWardProjectile>(Trap))
	{
		bDetectableState = Ward->bArmed;
	}
	return bDetectableState && DetectableTrapTypes.Contains(GetTrapID(Trap));
}

bool UWoTTrapDetectLeech::ShouldRunClientOutput() const
{
	// TrapDetectLeech.uc: NM_Standalone || Role < ROLE_Authority — the retail
	// gate is NETMODE-based, not possession-based (standalone role IS
	// authority; a possession requirement would also kill it there). In
	// networked play the leech runs on clients; the owner-only component
	// flags + the local-control check in PlayHeartbeat keep the output
	// private to the owning client.
	const ENetMode Mode = GetNetMode();
	if (Mode == NM_DedicatedServer) { return false; }
	return Mode == NM_Standalone || GetOwnerRole() != ROLE_Authority;
}

FVector UWoTTrapDetectLeech::ClosestCollisionPoint(const AActor* Trap,
	const FVector& From) const
{
	const FVector Center = Trap->GetActorLocation();
	// Legend.Util.uc:220-390 CalcClosestCollisionPoint clips against the
	// actor's collision cylinder. false excludes non-colliding visual meshes;
	// their bounds must not change Trap Detect's aim point.
	const FBox CollisionBounds = Trap->GetComponentsBoundingBox(
		false /* bNonColliding */);
	if (!CollisionBounds.IsValid) { return Center; }

	const FVector Extent = CollisionBounds.GetExtent();
	const float Radius = FMath::Max(Extent.X, Extent.Y);
	const float HalfHeight = Extent.Z;
	const FVector Offset = From - Center;
	FVector Closest = From;
	Closest.Z = Center.Z + FMath::Clamp(Offset.Z, -HalfHeight, HalfHeight);
	const float RadialDistance = FVector2D(Offset.X, Offset.Y).Size();
	if (RadialDistance > Radius && RadialDistance > SMALL_NUMBER)
	{
		const float RadialScale = Radius / RadialDistance;
		Closest.X = Center.X + Offset.X * RadialScale;
		Closest.Y = Center.Y + Offset.Y * RadialScale;
	}
	return Closest;
}

AActor* UWoTTrapDetectLeech::GetClosestTrap(float& TrapDistance,
	FVector& ClosestPoint) const
{
	APawn* Host = Cast<APawn>(GetOwner());
	if (!Host) { return nullptr; }
	const FVector Eye = Host->GetActorLocation() + FVector(0, 0, Host->BaseEyeHeight);
	const FVector View = Host->GetViewRotation().Vector();
	FHitResult Direct;
	// TrapDetectLeech.uc:161 TraceRecursive(..., true): the first qualifying
	// retail actor hit on the view ray wins (ULevel::SingleLineCheck rules).
	if (WoTRetailActorTraceRecursive(GetWorld(), Eye,
		View, DetectRadius, Host, Direct)
		&& IsTrapRelevant(Direct.GetActor()))
	{
		ClosestPoint = ClosestCollisionPoint(Direct.GetActor(), Host->GetActorLocation());
		TrapDistance = FVector::Dist(ClosestPoint, Eye);
		return Direct.GetActor();
	}

	AActor* Best = nullptr;
	float BestRatio = 0.0f;
	auto Consider = [&](AActor* Candidate)
	{
		if (!IsValid(Candidate) || !IsTrapRelevant(Candidate)) { return; }
		const FVector Point = ClosestCollisionPoint(Candidate, Host->GetActorLocation());
		const FVector ToPoint = Point - Eye;
		const float LocDist = ToPoint.Size();
		if (LocDist > DetectRadius || LocDist <= SMALL_NUMBER) { return; }
		const float Ratio = FVector::DotProduct(ToPoint, View) / LocDist;
		if (!Best || Ratio > BestRatio)
		{
			Best = Candidate;
			BestRatio = Ratio;
			TrapDistance = LocDist;
			ClosestPoint = Point;
		}
	};
	for (TActorIterator<AWoTTrap> It(GetWorld()); It; ++It) { Consider(*It); }
	for (TActorIterator<AWoTExpWardProjectile> It(GetWorld()); It; ++It) { Consider(*It); }
	return Best;
}

bool UWoTTrapDetectLeech::InitializeVisualsFor(AActor* Trap)
{
	const FName TrapID = GetTrapID(Trap);
	const FTrapIconInfo* Info = TrapIconInfos.FindByPredicate(
		[TrapID](const FTrapIconInfo& Row) { return Row.TrapType == TrapID; });
	if (!Info) { return false; }
	if (CurrentVisualTrapID != TrapID)
	{
		DestroyVisuals();
		CurrentVisualTrapID = TrapID;
	}
	APawn* Host = Cast<APawn>(GetOwner());
	if (!TrapIcon)
	{
		TrapIcon = WoTMeshFX::SpawnSpriteLocal(*GetWorld(), Info->IconConfig,
			Trap->GetActorLocation());
		if (TrapIcon)
		{
			TrapIcon->SetOwner(Host);
			TrapIcon->Sprite->SetOnlyOwnerSee(true);
		}
	}
	if (!Indicator)
	{
		Indicator = WoTWeaveFX::SpawnLocal(*GetWorld(), Info->SprayerConfig,
			Trap->GetActorLocation(), FRotator::ZeroRotator);
		if (Indicator)
		{
			Indicator->SetOwner(Host);
			Indicator->SetOnlyOwnerSee(true);
		}
	}
	return IsValid(TrapIcon) && IsValid(Indicator);
}

void UWoTTrapDetectLeech::HideVisuals()
{
	if (TrapIcon) { TrapIcon->SetActorHiddenInGame(true); }
	if (Indicator) { Indicator->bOn = false; }
}

void UWoTTrapDetectLeech::DestroyVisuals()
{
	if (IsValid(TrapIcon)) { TrapIcon->Destroy(); }
	if (IsValid(Indicator))
	{
		Indicator->bOn = false;
		Indicator->SetLifeSpan(60.0f);
	}
	TrapIcon = nullptr;
	Indicator = nullptr;
}

void UWoTTrapDetectLeech::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	APawn* Host = Cast<APawn>(GetOwner());
	if (Host && ShouldRunClientOutput())
	{
		float TrapDistance = 0.0f;
		FVector ClosestPoint = FVector::ZeroVector;
		ClosestTrap = GetClosestTrap(TrapDistance, ClosestPoint);
		if (IsValid(ClosestTrap) && InitializeVisualsFor(ClosestTrap))
		{
			const FVector Eye = Host->GetActorLocation()
				+ FVector(0, 0, Host->BaseEyeHeight);
			const FRotator Facing = (Eye - ClosestPoint).Rotation();
			TrapIcon->SetActorLocationAndRotation(ClosestPoint, Facing);
			Indicator->SetActorLocationAndRotation(ClosestPoint, Facing);
			Indicator->bOn = true;
			TrapIcon->SetActorHiddenInGame(false);
			const float Scalar = FMath::Clamp(TrapDistance / DetectRadius, 0.0f, 1.0f);
			AffectResolution = MinBeatTime + (MaxBeatTime - MinBeatTime) * Scalar;
		}
		else
		{
			ClosestTrap = nullptr;
			HideVisuals();
		}
	}

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	// TrapDetectLeech.uc:60-70's simulated client leg needs its own timer, but
	// standalone authority is already dispatched by WoTLeech::TickComponent.
	// Running both produced two PlayHeartbeat calls per AffectResolution.
	if (GetOwnerRole() != ROLE_Authority && ShouldRunClientOutput()
		&& AffectResolution > 0.0f)
	{
		ClientBeatTimer += DeltaTime;
		if (ClientBeatTimer >= AffectResolution)
		{
			ClientBeatTimer = FMath::Fmod(ClientBeatTimer, AffectResolution);
			PlayHeartbeat();
		}
	}
}

void UWoTTrapDetectLeech::PlayHeartbeat()
{
	APawn* Host = Cast<APawn>(GetOwner());
	if (IsValid(ClosestTrap) && Host)
	{
		if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Host->GetController()))
		{
			++HeartbeatRequestCountForTest;
			PC->ClientPlayUISound(FName(TEXT("Angreal/ActiveTD")), 1.0f);
		}
	}
}

void UWoTTrapDetectLeech::AffectHost(int32 /*Iterations*/)
{
	if (ShouldRunClientOutput()) { PlayHeartbeat(); }
}

void UWoTTrapDetectLeech::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	DestroyVisuals();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}
