#include "WoTPit.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../WoTMeshModulate.h"
#include "../WoTMover.h"
#include "../WoTPlayerController.h"
#include "../WoTUE1Collision.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTZoneVolume.h"

#include "../Inventory/WoTSeal.h"

#include "Components/CapsuleComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	// PitTrap.Accept / PitOpening.Accept: the receiver lands the pawn at itself, shifted by the
	// pawn's horizontal offset from the side it left (the .uc zeroes DeltaLocation.Z). View
	// rotation rides the base Accept — bChangesYaw stays false.
	bool PitAccept(AWoTTeleporter& Receiver, APawn& Toucher, const AActor& Source)
	{
		FVector Offset = Toucher.GetActorLocation() - Source.GetActorLocation();
		Offset.Z = 0.0;
		if (!Receiver.Accept(&Toucher))
		{
			return false;
		}
		if (Toucher.SetActorLocation(Receiver.GetActorLocation() + Offset, false,
			nullptr, ETeleportType::TeleportPhysics))
		{
			UWoTZoneIndex::RefreshAfterRelocation(&Toucher);
		}
		return true;
	}
}

AWoTPitTrap::AWoTPitTrap()
{
	// PitTrap.uc defaults: CollisionRadius=50, CollisionHeight=8. URL stays empty — the explicit
	// pointer link replaces the URL="PitOpening"/Tag machinery.
	SetUE1CollisionSize(50.0f, 8.0f);
}

void AWoTPitTrap::HideTrap()
{
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AWoTPitTrap::ShowTrap()
{
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
}

void AWoTPitTrap::NotifyActorBeginOverlap(AActor* Other)
{
	Super::NotifyActorBeginOverlap(Other);   // base Touch early-outs (URL empty)
	APawn* P = Cast<APawn>(Other);
	if (!HasAuthority() || !P || P == JustAccepted.Get() || !IsValid(ThePitOpening))
	{
		return;
	}
	if (!AdmitUE1Touch(P))
	{
		return;
	}
	// Waiting.Touch gate 1: Pit(Owner).IsInactive — team/editing + the Seal exclusion. A missing
	// owner reads active (the .uc's tolerated Accessed None).
	const AWoTPit* Pit = Cast<AWoTPit>(GetOwner());
	if (Pit && Pit->IsInactive(Other))
	{
		return;
	}
	// Waiting.Touch gate 2: a seal resting on the cover prevents the inbound
	// teleporter from firing (PitTrap.uc:46). UE1 read the engine contact list (Touching[]); the
	// port tests the collision cylinders directly, since inventory items don't overlap the
	// pawn-only teleporter pad channel.
	for (TActorIterator<AWoTSeal> It(GetWorld()); It; ++It)
	{
		if (!IsValid(*It))
		{
			continue;
		}
		if (WoTUE1Collision::CylindersOverlap(GetActorLocation(),
			UE1CollisionRadius, UE1CollisionHalfHeight, *It))
		{
			return;
		}
	}
	PitAccept(*ThePitOpening, *P, *this);
}

void AWoTPitTrap::Destroyed()
{
	// PitTrap.Destroyed: release the opening for the next pit.
	if (IsValid(ThePitOpening))
	{
		ThePitOpening->ThePitTrap = nullptr;
	}
	ThePitOpening = nullptr;
	Super::Destroyed();
}

AWoTPitOpening::AWoTPitOpening()
{
	// PitOpening.uc defaults: CollisionRadius=128 (height inherits the Teleporter 40).
	SetUE1CollisionSize(128.0f, 40.0f);
}

void AWoTPitOpening::NotifyActorBeginOverlap(AActor* Other)
{
	Super::NotifyActorBeginOverlap(Other);   // base Touch early-outs (URL empty)
	APawn* P = Cast<APawn>(Other);
	if (!HasAuthority() || !P || P == JustAccepted.Get() || !IsValid(ThePitTrap))
	{
		return;
	}
	if (!AdmitUE1Touch(P))
	{
		return;
	}
	PitAccept(*ThePitTrap, *P, *this);
}

AWoTPit::AWoTPit()
{
	VisualMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("VisualMesh"));
	VisualMesh->SetupAttachment(RootComponent);
	VisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	bReplicates = true;   // the false-floor look is client-facing
	TrapType = TEXT("Pit");
	// Pit.uc tunables (base AWoTTrap fields) + collision defaults.
	SeparationDistance = 100;
	TrapRadius = 48;
	if (Capsule)
	{
		Capsule->InitCapsuleSize(50.0f, 12.0f);
		Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	}
}

void AWoTPit::BeginPlay()
{
	Super::BeginPlay();
	BindTrapVisual(VisualMesh, FName(TEXT("Pit")), FVector(1.0f));
	if (GetNetMode() != NM_DedicatedServer && IsValid(VisualMesh))
	{
		const int32 MaterialCount = VisualMesh->GetNumMaterials();
		for (int32 Slot = 0; Slot < MaterialCount; ++Slot)
		{
			UMaterialInterface* OriginalMaterial = VisualMesh->GetMaterial(Slot);
			if (UMaterialInstanceDynamic* ModulatedMaterial =
				WoTMeshModulate::CreateInstance(this, OriginalMaterial))
			{
				VisualMesh->SetMaterial(Slot, ModulatedMaterial);
			}
		}
	}
	if (!HasAuthority())
	{
		return;
	}
	// Pit.PreBeginPlay: spawn the surface hidden (the deploy/editing flow shows it), then claim
	// the first free opening; both ends link back and the pit owns both actors.
	const FTransform PitTrapTransform(GetActorRotation(), GetActorLocation());
	ThePitTrap = GetWorld()->SpawnActorDeferred<AWoTPitTrap>(
		AWoTPitTrap::StaticClass(), PitTrapTransform, this, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!ThePitTrap)
	{
		return;
	}
	if (IsPreviewMode())
	{
		ConfigurePreviewChild(ThePitTrap);
	}
	ThePitTrap->FinishSpawning(PitTrapTransform);
	if (IsPreviewMode())
	{
		ConfigurePreviewChild(ThePitTrap);
	}
	ThePitTrap->HideTrap();
	if (IsPreviewMode())
	{
		return;
	}
	for (TActorIterator<AWoTPitOpening> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It) && !It->ThePitTrap)
		{
			It->ThePitTrap = ThePitTrap;
			ThePitTrap->ThePitOpening = *It;
			It->SetOwner(this);
			break;
		}
	}
	if (!ThePitTrap->ThePitOpening)
	{
		// Pit.uc:42-47: when every PitOpening is claimed, remove the surface and
		// notify the player who placed it.
		// MP-LIVE: the citadel editor spawns with Owner=Self (WOTPlayer.uc:4917) before
		// ValidateTrap later clears the owner; the port's create funnel spawns with the
		// controller as owner. Typeless, matching retail's unset optional Type (the plain
		// console route). Owner-only RPC, authority side (this branch is authority-gated).
		AWoTPlayerController* PlacingPlayer =
			Cast<AWoTPlayerController>(GetOwner());
		if (!PlacingPlayer)
		{
			if (const APawn* OwnerPawn = Cast<APawn>(GetOwner()))
			{
				PlacingPlayer =
					Cast<AWoTPlayerController>(OwnerPawn->GetController());
			}
		}
		if (IsValid(PlacingPlayer))
		{
			PlacingPlayer->ClientTypedMessage(
				TEXT("No pit opening is free in this level."), NAME_None);
		}
		UE_LOG(LogTemp, Warning, TEXT("AWoTPit %s: no free PitOpening in the level."), *GetName());
		ThePitTrap->Destroy();
		ThePitTrap = nullptr;
	}
}

void AWoTPit::Destroyed()
{
	// Pit.Destroyed: the surface goes with the pit (its own Destroyed frees the opening).
	if (IsValid(ThePitTrap))
	{
		ThePitTrap->Destroy();
	}
	ThePitTrap = nullptr;
	Super::Destroyed();
}

bool AWoTPit::IsInactive(const AActor* Other) const
{
	// Pit.IsInactive: Seals never spring the pit.
	if (Other && Other->IsA<AWoTSeal>())
	{
		return true;
	}
	return Super::IsInactive(Other);
}

bool AWoTPit::CalcLocation(FVector& HitLocation, FVector& HitNormal)
{
	// Pit.CalcLocation: unlike base Trap, does NOT retrace to the floor — it trusts
	// the caller's floor aim and only validates PitRadius clearance in the plane.
	if (HitNormal.Z < 0.95f)
	{
		return false;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const float PitRadius = static_cast<float>(
		GetDefault<AWoTPitTrap>()->TrapRadius);
	FVector AxisX, AxisY, AxisZ;
	FRotationMatrix(HitNormal.Rotation()).GetScaledAxes(AxisX, AxisY, AxisZ);
	(void)AxisX;
	// 2 UU off the surface so a plane-parallel trace doesn't self-graze the floor
	// (matches WoTWall.cpp:174).
	const FVector TraceOrigin = HitLocation + HitNormal.GetSafeNormal() * 2.0f;
	for (int32 i = -1; i <= 1; ++i)
	{
		for (int32 j = -1; j <= 1; ++j)
		{
			// Pit.uc:132-137 rejects ONLY when the clearance trace hits `Level` (BSP) —
			// a pawn/decoration first-hit does NOT reject there. Pawns block the
			// WorldStatic trace channel, so require the hit's collision OBJECT TYPE be
			// WorldStatic (level geometry) for the reject, as in base CalcLocation.
			const FVector TraceEnd = TraceOrigin + PitRadius * (AxisY * i + AxisZ * j);
			FHitResult Hit;
			if (WoTRetailActorTrace(World, TraceOrigin, TraceEnd, this, Hit)
				&& Hit.bBlockingHit && Hit.GetComponent()
				&& Hit.GetComponent()->GetCollisionObjectType() == ECC_WorldStatic)
			{
				return false;
			}
		}
	}
	return true;
}

AActor* AWoTPit::AnyActorsInArea(const FVector& Loc, float Radius) const
{
	// Pit.AnyActorsInArea (Pit.uc:81-97): reject placement near a mover, then fall through
	// to base occupancy. Ported to UE1 RadiusActors semantics like the FireWall teleporter
	// pass: a collision-agnostic actor walk whose radius test is
	// `dist < 512 + Mover.CollisionRadius` — the mover mesh's bounds sphere radius stands
	// in for the UE1 collision cylinder radius — followed by the explicit horizontal
	// `VSize(Delta) < 120` gate against Mover.RealPosition (the mover's REST pose, not a
	// mid-travel keyframe location).
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AWoTMover> It(World); It; ++It)
		{
			AWoTMover* Mover = *It;
			if (!IsValid(Mover))
			{
				continue;
			}
			const float MoverRadius = Mover->GetRootComponent()
				? static_cast<float>(Mover->GetRootComponent()->Bounds.SphereRadius)
				: 0.0f;
			if ((Mover->GetActorLocation() - Loc).Size() < 512.0f + MoverRadius)
			{
				FVector HorizontalDelta = Loc - Mover->GetBasePosition();
				HorizontalDelta.Z = 0.0f;
				if (HorizontalDelta.Size() < 120.0f)
				{
					return Mover;
				}
			}
		}
	}
	return Super::AnyActorsInArea(Loc, Radius);
}

bool AWoTPit::DeployResource(FVector HitLocation, FVector HitNormal)
{
	// Pit.DeployResource: needs a claimed opening, then clearance + separation.
	if (!IsValid(ThePitTrap))
	{
		return false;
	}
	if (IsPreviewMode())
	{
		bool bHasFreeOpening = false;
		for (TActorIterator<AWoTPitOpening> It(GetWorld()); It; ++It)
		{
			if (IsValid(*It) && !It->ThePitTrap)
			{
				bHasFreeOpening = true;
				break;
			}
		}
		if (!bHasFreeOpening)
		{
			return false;
		}
	}
	if (!CalcLocation(HitLocation, HitNormal))
	{
		return false;
	}
	if (AnyActorsInArea(HitLocation, static_cast<float>(SeparationDistance)) != nullptr)
	{
		return false;
	}
	const FRotator Rot = HitNormal.Rotation();
	ThePitTrap->SetActorLocationAndRotation(HitLocation, Rot);
	SetActorLocationAndRotation(HitLocation, Rot);
	// Trap.ValidateTrap final-placement cleanup (tutorial poll deferred): detach the
	// finished pit from the placing controller.
	if (!IsPreviewMode())
	{
		SetOwner(nullptr);
	}
	return true;
}

void AWoTPit::BeginEditingResource(uint8 PlacedByTeam)
{
	Super::BeginEditingResource(PlacedByTeam);   // sets bEditing + ApplyEditingCollision(true)
	// Pit.BeginEditingResource: hide the trap surface so it can't spring while editing
	// (authority world state; the surface senses on the authority only).
	if (IsValid(ThePitTrap))
	{
		ThePitTrap->HideTrap();
	}
}

void AWoTPit::EndEditingResource()
{
	Super::EndEditingResource();   // clears bEditing + ApplyEditingCollision(false)
	// Pit.EndEditingResource: the trap surface goes live.
	if (IsValid(ThePitTrap))
	{
		ThePitTrap->ShowTrap();
	}
}

void AWoTPit::ApplyEditingCollision(bool bEditingNow)
{
	if (!Capsule)
	{
		return;
	}
	if (bEditingNow)
	{
		// Pit.BeginEditingResource: SetCollision(true,false,false) — the placed pit is
		// targetable/occupancy-testable during edit. The editor cursor
		// (CitadelRemove/CitadelToggle) traces on ECC_Visibility, so the manager capsule
		// must BLOCK that channel to be selectable (UE1's bCollideActors made the placement
		// Trace hit it); pawns stay unblocked, matching bBlockPlayers=false.
		Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	}
	else
	{
		// Pit.EndEditingResource: SetCollision(false,false,false) — the false floor is
		// illusory at play.
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

AWoTPitInventory::AWoTPitInventory()
{
	// PitInventory.uc defaultproperties (player-spawned).
	ResourceClass = AWoTPit::StaticClass();
	Count = 2;
	// PitInventory.uc:20,23: StatusIconFrame=Texture'WoTTraps.UI.M_SPit' /
	// StatusIcon=Texture'WoTTraps.UI.I_SPit'.
	BindStatusIcons(TEXT("WoTTraps"), TEXT("I_SPit"), TEXT("M_SPit"));
}
