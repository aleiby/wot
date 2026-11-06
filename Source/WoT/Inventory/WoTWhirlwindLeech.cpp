#include "WoTWhirlwindLeech.h"
#include "../AI/WoTPathNodeIterator.h"

#include "WoTAngrealInvLevitate.h"
#include "WoTInventoryComponent.h"
#include "../WoTCharacter.h"
#include "../WoTUE1Collision.h"
#include "../WoTMover.h"
#include "../WoTParticleSprayer.h"
#include "../WoTPlayerController.h"
#include "../WoTWeaveFX.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTWhirlwindFunnel.h"
#include "../WoTZoneVolume.h"
#include "../World/WoTRuntimeBSPRenderer.h"

#include "Components/PointLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

UWoTWhirlwindLeech::UWoTWhirlwindLeech()
{
	// WhirlwindLeech defaultproperties (simulation subset).
	AlignmentSpeed = 2.0f;
	RotationPerSecondDeg = 659.18f;   // UE1 120000 units/s
	AffectResolution = 0.2f;
	bDeleterious = true;
	bDisplayIcon = true;
	bSingular = true;                 // one whirlwind per victim

	// WhirlwindLeech.uc: AmbientSound=LoopWW, SoundRadius/Volume 255/255 — the
	// roar rides the spun victim.
	AmbientSoundPath = FName(TEXT("Angreal/LoopWW"));
	AmbientSoundRadius = 255;
	AmbientSoundVolume = 255;
}

void UWoTWhirlwindLeech::DisableLevitate(APawn* Host)
{
	// A victim can't levitate and be whirlwinded at once — cancel any Levitate.
	if (UWoTInventoryComponent* Inv = Host->FindComponentByClass<UWoTInventoryComponent>())
	{
		for (AWoTInventory* Item : Inv->Items)
		{
			if (AWoTAngrealInvLevitate* Lev = Cast<AWoTAngrealInvLevitate>(Item))
			{
				Lev->UnCastArtifact();
			}
		}
	}
}

void UWoTWhirlwindLeech::AttachTo(APawn* NewHost)
{
	Super::AttachTo(NewHost);

	if (!IsRegistered())
	{
		return;   // bSingular refused (victim already whirlwinded)
	}

	APawn* Host = Cast<APawn>(GetOwner());
	if (!Host)
	{
		Unattach();
		return;
	}

	DisableLevitate(Host);
	DesiredLocation = Host->GetActorLocation();

	// Lift + hold: disable movement (UE1 PHYS_None), restored on Unattach.
	if (ACharacter* Char = Cast<ACharacter>(Host))
	{
		if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
		{
			OldMovementMode = Move->MovementMode;
			OldCustomMovementMode = Move->CustomMovementMode;
			Move->StopMovementImmediately();
			Move->DisableMovement();
		}
	}
	bLifting = true;
	if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Host->GetController()))
	{
		PC->ClientSetForcedViewYaw(true, 120000.0f);
	}
}

void UWoTWhirlwindLeech::OnHostDetached(APawn* OldHost)
{
	// WhirlwindLeech.uc:83-97 (UnAttach): restore the host's physics + stop the
	// lift. Called from Unattach AND from a Swap Places transfer before we're
	// reparented — retail UnAttaches the old host first (SwapPlacesEffect.uc:294),
	// so the freed victim isn't left permanently movement-disabled.
	if (!bLifting)
	{
		return;
	}
	bLifting = false;
	if (AWoTPlayerController* PC = OldHost
		? Cast<AWoTPlayerController>(OldHost->GetController()) : nullptr)
	{
		PC->ClientSetForcedViewYaw(false, 120000.0f);
	}
	if (ACharacter* Char = Cast<ACharacter>(OldHost))
	{
		if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
		{
			Move->SetMovementMode(OldMovementMode, OldCustomMovementMode);
		}
	}
}

void UWoTWhirlwindLeech::Unattach()
{
	OnHostDetached(Cast<APawn>(GetOwner()));
	DestroyPresentation();
	Super::Unattach();
}

void UWoTWhirlwindLeech::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	OnHostDetached(Cast<APawn>(GetOwner()));
	DestroyPresentation();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

namespace
{
	bool BlocksActors(const AActor* Actor)
	{
		if (!Actor) { return false; }
		TArray<UPrimitiveComponent*> Components;
		Actor->GetComponents<UPrimitiveComponent>(Components);
		for (const UPrimitiveComponent* Primitive : Components)
		{
			if (Primitive && Primitive->GetCollisionEnabled() != ECollisionEnabled::NoCollision
				&& (Primitive->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block
					|| Primitive->GetCollisionResponseToChannel(ECC_WorldDynamic) == ECR_Block))
			{
				return true;
			}
		}
		return false;
	}

	void CylinderSize(const AActor* Actor, float& Radius, float& Height)
	{
		Radius = Actor ? Actor->GetSimpleCollisionRadius() : 0.0f;
		Height = Actor ? Actor->GetSimpleCollisionHalfHeight() : 0.0f;
	}
}

bool UWoTWhirlwindLeech::ActorFits(AActor* MovingActor,
	const FVector& NewLocation, float SearchRadius)
{
	if (!IsValid(MovingActor) || !MovingActor->GetWorld()) { return false; }
	if (!BlocksActors(MovingActor)) { return true; }

	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WhirlwindActorFits), false,
		MovingActor);
	MovingActor->GetWorld()->OverlapMultiByObjectType(Overlaps, NewLocation,
		FQuat::Identity, FCollisionObjectQueryParams::AllObjects,
		FCollisionShape::MakeSphere(SearchRadius), Params);
	float MovingRadius = 0.0f, MovingHeight = 0.0f;
	CylinderSize(MovingActor, MovingRadius, MovingHeight);
	for (const FOverlapResult& Result : Overlaps)
	{
		AActor* Other = Result.GetActor();
		// RadiusActors walks ACTORS: the level's BSP is not one. The port's level
		// geometry is actors (dev: StaticMeshActor world meshes; guarded: the
		// locally projected AWoTRuntimeBSPWorld) whose bounding "cylinder" spans
		// the map, so counting them made every destination fail (Shift aimed a
		// few degrees down, and a Whirlwind victim that could never rise).
		if (!IsValid(Other) || Other == MovingActor || Other->IsA<AWoTMover>()
			|| Other->IsA<AStaticMeshActor>() || Other->IsA<AWoTRuntimeBSPWorld>()
			|| !BlocksActors(Other))
		{
			continue;
		}
		FVector Diff = Other->GetActorLocation() - NewLocation;
		const float HeightDiff = Diff.Z; // SIGNED in shipped Util.ActorFits.
		Diff.Z = 0.0f;
		float OtherRadius = 0.0f, OtherHeight = 0.0f;
		CylinderSize(Other, OtherRadius, OtherHeight);
		if (OtherRadius + MovingRadius >= Diff.Size()
			&& OtherHeight + MovingHeight >= HeightDiff)
		{
			return false;
		}
	}
	return true;
}

void UWoTWhirlwindLeech::DestroyPresentation()
{
	if (IsValid(Funnel)) { Funnel->Destroy(); }
	if (IsValid(Dust)) { Dust->Destroy(); }
	if (WhirlwindLight) { WhirlwindLight->DestroyComponent(); }
	Funnel = nullptr;
	Dust = nullptr;
	WhirlwindLight = nullptr;
	LastTraceLocation = FVector::ZeroVector;
}

void UWoTWhirlwindLeech::TickPresentation(APawn* Host, float DeltaTime)
{
	if (!IsValid(Host) || GetNetMode() == NM_DedicatedServer || !GetWorld()) { return; }
	FHitResult Ground;
	const FVector HostLocation = Host->GetActorLocation();
	// WhirlwindLeech.uc:147 TraceRecursive(..., false): funnel ground is the
	// first level/mover hit, never a pawn or blocker cylinder.
	WoTVisBlockingTraceRecursive(GetWorld(), HostLocation,
		FVector(0, 0, -1), 0.0f, Host, Ground);
	const FVector TraceLocation = Ground.bBlockingHit ? Ground.ImpactPoint : HostLocation;
	if (!IsValid(Funnel))
	{
		FActorSpawnParameters P;
		P.Owner = Host;
		P.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Funnel = GetWorld()->SpawnActor<AWoTWhirlwindFunnel>(
			AWoTWhirlwindFunnel::StaticClass(), TraceLocation,
			FVector::UpVector.Rotation(), P);
	}
	if (!IsValid(Funnel)) { return; }

	WWRotation.Yaw += 64000.0f * 360.0f / 65536.0f * DeltaTime;
	FVector DesiredBase = TraceLocation + WWRotation.RotateVector(FVector(10, 0, 0));
	if (DesiredBase.Z < Funnel->GetActorLocation().Z)
	{
		DesiredBase.Z = FMath::Min(Funnel->GetActorLocation().Z - 60.0f * DeltaTime,
			HostLocation.Z - Host->GetSimpleCollisionHalfHeight());
	}
	if (HostLocation.Z - DesiredBase.Z > 500.0f)
	{
		DesiredBase.Z = HostLocation.Z - 500.0f;
	}
	Funnel->SetActorLocation(DesiredBase);
	// WhirlwindLeech.uc:164 measures the funnel to DesiredLocation, not the
	// victim's eased HostLocation, so the visual reaches the caster's target.
	Funnel->SetHeight(FVector::Dist(DesiredLocation, Funnel->GetActorLocation()) + 250.0f);
	FVector GroundXY = TraceLocation;
	GroundXY.Z = DesiredBase.Z;
	if (!LastTraceLocation.IsNearlyZero())
	{
		Funnel->ShiftParticles(GroundXY - LastTraceLocation);
	}
	LastTraceLocation = GroundXY;

	if (!IsValid(Dust))
	{
		Dust = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("WhirlwindDust"),
			Funnel->GetActorLocation(), FVector::UpVector.Rotation());
		if (Dust) { Dust->SetOwner(Host); }
	}
	if (IsValid(Dust))
	{
		Dust->SetActorLocation(Funnel->GetActorLocation());
		Dust->SetActorRotation(FVector::UpVector.Rotation());
		Dust->bOn = FMath::Abs(Funnel->GetActorLocation().Z - TraceLocation.Z) < 10.0f;
	}
	if (!WhirlwindLight)
	{
		WhirlwindLight = NewObject<UPointLightComponent>(Host, TEXT("WhirlwindLight"));
		if (WhirlwindLight)
		{
			WhirlwindLight->SetupAttachment(Host->GetRootComponent());
			WhirlwindLight->RegisterComponent();
			WoTUE1LightRegistry::Configure(WhirlwindLight,
				TEXT("WhirlwindLeech"));
		}
	}
}

void UWoTWhirlwindLeech::AffectHost(int32 /*Iterations*/)
{
	// Dead/gone host -> let go (original gates on Pawn(Owner).Health <= 0).
	const AWoTCharacter* Host = Cast<AWoTCharacter>(GetOwner());
	if (!IsValid(Host) || !Host->IsAlive())
	{
		Unattach();
	}
}

void UWoTWhirlwindLeech::TickComponent(float DeltaTime, ELevelTick TickType,
                                       FActorComponentTickFunction* ThisTickFunction)
{
	APawn* Host = Cast<APawn>(GetOwner());
	if (bLifting && GetOwnerRole() == ROLE_Authority)
	{
		if (Host)
		{
			// Ease toward DesiredLocation at rate 1/AlignmentSpeed (UE1:
			// NewVelocity = (Desired - Loc)/AlignmentSpeed; Loc += NewVelocity*dt).
			const FVector Loc = Host->GetActorLocation();
			const FVector NewLoc = (AlignmentSpeed > 0.0f)
				? Loc + ((DesiredLocation - Loc) / AlignmentSpeed) * DeltaTime
				: DesiredLocation;
			// Shipped comment said "telefrag", but a no-fit tick merely holds.
			// Owner.SetLocation (WhirlwindLeech.uc:134-138) is UE1 FarMoveActor:
			// FindSpot keeps the cylinder out of world geometry and a spot that
			// cannot fit leaves the victim where it is. UWorld::FindTeleportSpot is
			// UE5's FindSpot, so a desired point below the floor (the caster's view
			// pitched down, AngrealInvWhirlwind.uc:149) drags the victim along the
			// floor instead of sinking it. (Not TeleportTo: its CMC teleport
			// notification would drop the PHYS_None hold into falling.)
			FVector Spot = NewLoc;
			if (ActorFits(Host, NewLoc, 1024.0f) && GetWorld()
				&& WoTUE1Collision::FindTeleportSpotWorldOnly(
					*GetWorld(), *Host, Spot, Host->GetActorRotation()))
			{
				if (Host->SetActorLocation(Spot, false, nullptr,
					ETeleportType::TeleportPhysics))
				{
					UWoTZoneIndex::RefreshAfterRelocation(Host);
				}
			}

			// Spin the victim.
			FRotator Rot = Host->GetActorRotation();
			const int32 DeltaUnits = FMath::TruncToInt(120000.0f * DeltaTime) + 1;
			Rot.Yaw += DeltaUnits * 360.0f / 65536.0f;
			Host->SetActorRotation(Rot);

			// WhirlwindLeech.uc:181-189: mark the caster as the victim's SuicideInstigator
			// each tick — if the whirlwind flings them into a hazard, the caster gets the kill.
			if (AWoTCharacter* HostChar = Cast<AWoTCharacter>(Host))
			{
				HostChar->SetSuicideInstigator(LeechInstigator);
			}
		}
	}
	if (IsValid(Host))
	{
		// WhirlwindLeech.uc:127-153 moves/spins the victim before tracing the
		// funnel ground from the new authoritative location.
		TickPresentation(Host, DeltaTime);
	}
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UWoTWhirlwindLeech::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// Original: unreliable if Role==Authority DesiredLocation.
	DOREPLIFETIME(UWoTWhirlwindLeech, DesiredLocation);
}
