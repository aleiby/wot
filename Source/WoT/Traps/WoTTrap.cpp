#include "WoTTrap.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../WoTCarcass.h"
#include "../WoTMeshLight.h"
#include "../WoTVertexAnimComponent.h"
#include "Engine/SkeletalMesh.h"

#include "../AI/WoTNPCCharacter.h"
#include "../WoTEvents.h"
#include "../WoTMover.h"
#include "../WoTPlayerState.h"
#include "WoTTrapBurst.h"

#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "CollisionQueryParams.h"
#include "EngineUtils.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

namespace WoTTrapHelpers
{
	AWoTTrapBurst* SpawnTrapBurst(UWorld& World, const FVector& Location)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World.SpawnActor<AWoTTrapBurst>(AWoTTrapBurst::StaticClass(),
			Location, FRotator::ZeroRotator, SpawnParameters);
	}

	AWoTMover* ResolveTaggedMover(
		UWorld* World, FName Tag, const UClass* MoverClass)
	{
		if (!World || !MoverClass)
		{
			return nullptr;
		}
		for (TActorIterator<AWoTMover> It(World); It; ++It)
		{
			if (It->IsA(MoverClass) && It->TagName == Tag)
			{
				return *It;
			}
		}
		return nullptr;
	}
}

AWoTTrap::AWoTTrap()
{
	PrimaryActorTick.bCanEverTick = false;

	// UE1 Trap defaults bCollideActors=True; the trigger cylinder is the touch volume, resized per
	// subclass (CollisionRadius/Height in each .uc's defaultproperties).
	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	SetRootComponent(Capsule);
	Capsule->InitCapsuleSize(34.0f, 34.0f);
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);

}

AController* AWoTTrap::ResolveDamageInstigatorController() const
{
	if (bDamageInstigatorCleared)
	{
		return nullptr;
	}
	if (AController* Controller = DamageInstigatorController.Get())
	{
		// A disconnected player's controller can remain addressable until the
		// actor-destruction/GC tail completes.  UE1 lost the Instigator's PRI at
		// logout, so never award credit through a controller already leaving play.
		if (!Controller->IsActorBeingDestroyed())
		{
			return Controller;
		}
	}
	return GetInstigatorController();
}

void AWoTTrap::Destroyed()
{
	ResetPreviewChildren();
	Super::Destroyed();
}

bool AWoTTrap::IsInactive(const AActor* Other) const
{
	// Trap.IsInactive (Trap.uc:265-280): inactive while editing, or for a pawn on this trap's team
	// (traps don't fire on teammates). The original reads Pawn(Other).PlayerReplicationInfo.Team —
	// the REAL team, never a disguise's ApparentTeam (a disguised infiltrator still springs the
	// defenders' traps). A retail NPC mirrored AssignedTeam into its PRI (WOTPawn.uc:429), so an
	// NPC's AssignedTeam is that same read; a player carries it on PlayerState.
	if (bPreviewMode || bEditing)
	{
		return true;
	}
	if (const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(Other))
	{
		return Team == NPC->AssignedTeam;
	}
	if (const APawn* Pawn = Cast<APawn>(Other))
	{
		const AWoTPlayerState* PS = Pawn->GetPlayerState<AWoTPlayerState>();
		const uint8 OtherTeam = PS ? PS->Team : uint8(255);
		return Team == OtherTeam;
	}
	return false;
}

void AWoTTrap::SetPreviewMode(bool bEnabled)
{
	bPreviewMode = bEnabled;
	if (!bPreviewMode)
	{
		return;
	}

	bEditing = true;
	SetFlags(RF_Transient);
	SetReplicates(false);
	SetReplicateMovement(false);
	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
	for (UActorComponent* Component : GetComponents())
	{
		if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
		{
			Primitive->SetOnlyOwnerSee(true);
			Primitive->SetGenerateOverlapEvents(false);
			Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}
}

void AWoTTrap::ConfigurePreviewChild(AActor* Child)
{
	if (!bPreviewMode || !IsValid(Child))
	{
		return;
	}
	Child->SetFlags(RF_Transient);
	Child->SetReplicates(false);
	Child->SetReplicateMovement(false);
	Child->SetActorEnableCollision(false);
	Child->SetActorHiddenInGame(!bPreviewPlacementVisible);
	for (UActorComponent* Component : Child->GetComponents())
	{
		if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
		{
			Primitive->SetOnlyOwnerSee(true);
			Primitive->SetGenerateOverlapEvents(false);
			Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}
	PreviewChildren.AddUnique(Child);
}

void AWoTTrap::ResetPreviewChildren()
{
	if (!bPreviewMode)
	{
		return;
	}
	for (const TWeakObjectPtr<AActor>& ChildPtr : PreviewChildren)
	{
		if (AActor* Child = ChildPtr.Get())
		{
			Child->Destroy();
		}
	}
	PreviewChildren.Reset();
}

void AWoTTrap::SetPreviewVisible(bool bVisible)
{
	if (!bPreviewMode)
	{
		return;
	}
	bPreviewPlacementVisible = bVisible;
	SetActorHiddenInGame(!bVisible);
	SetActorEnableCollision(false);
	for (const TWeakObjectPtr<AActor>& ChildPtr : PreviewChildren)
	{
		if (AActor* Child = ChildPtr.Get())
		{
			Child->SetActorHiddenInGame(!bVisible);
			Child->SetActorEnableCollision(false);
		}
	}
}

void AWoTTrap::BeginEditingResource(uint8 PlacedByTeam)
{
	if (!HasAuthority())
	{
		return;
	}
	++BeginEditingResourceCallCount;
	Team = PlacedByTeam;
	bEditing = true;
	// The collision half is shared with net clients: the authority applies it here; a
	// remote proxy applies it from OnRep_Editing (bEditing replicates).
	ApplyEditingCollision(true);
	ApplyEditingGlowPresentation();   // Trap.uc:71 AmbientGlow=64
}

void AWoTTrap::OnRep_Editing()
{
	// UE1 replicated bCollideActors, so a client's edit-phase collision tracked the server
	// for free; here the replicated edit flag re-applies the collision half locally.
	// UE1 also replicated AmbientGlow natively — the glow half re-derives here too.
	ApplyEditingCollision(bEditing);
	ApplyEditingGlowPresentation();
}

void AWoTTrap::ApplyEditingGlowPresentation()
{
	// Trap.uc:71,76: BeginEditingResource sets AmbientGlow=64; EndEditingResource sets 0 —
	// the trap brightens while being placed/edited. Reuses the port's existing UE1
	// lighting formula: the visual's runtime UWoTMeshLight reads the owner's AmbientGlow
	// byte via reflection and adds AmbientGlow/255 (64/255 ~ 0.25) to each ambient
	// channel (WoTMeshLight.cpp BakeRuntime; the bUnlit branch folds it as
	// ScaleGlow*0.5 + AmbientGlow/256). Force a re-bake so the cue flips immediately
	// instead of waiting for the movement/leaf re-bake timer; worlds without a light
	// table (headless specs, dedicated servers) still carry the property value.
	AmbientGlow = bEditing ? 64.0f : 0.0f;
	if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
	{
		MeshLight->ForceRuntimeBake();
	}
}

void AWoTTrap::SetCitadelSelectTraceability(bool bSelected)
{
	// Begin/EndSelectResource (Portcullis.uc:62-77, Staircase.uc:52-66) as a local stamp:
	// the owner's controller drives it from the palette; the collision consequence is
	// whatever the subclass's ApplyEditingCollision derives from it.
	if (bCitadelSelectedForTrace == bSelected)
	{
		return;
	}
	bCitadelSelectedForTrace = bSelected;
	ApplyEditingCollision(bEditing);
}

void AWoTTrap::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTTrap, bEditing);
}

bool AWoTTrap::CalcLocation(FVector& Loc, FVector& Normal)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Trap.uc:122-130: the down-trace hit must be `Level` (BSP) — anything else (a pawn, a
	// decoration, another trap under the cursor) fails placement. Pawns BLOCK the WorldStatic
	// trace channel, so a bare blocking-hit test would let a pawn stand in as the floor;
	// require the hit component's collision OBJECT TYPE be WorldStatic (the AWoTWall check).
	// The later hanging/side traces accept any hit, per the .uc (:144 `Other == None`, :161).
	FHitResult DownHit;
	const FVector DownEnd = Loc + FVector(0.0f, 0.0f, -1.0f) * TraceDistance;
	if (!WoTRetailActorTrace(World, Loc, DownEnd, this, DownHit)
		|| !DownHit.bBlockingHit
		|| !DownHit.GetComponent()
		|| DownHit.GetComponent()->GetCollisionObjectType() != ECC_WorldStatic)
	{
		return false;
	}
	// UE1 Trace returns the hit backed off 0.5 uu along the trace,
	// so a trap's StartLocation sits just off the surfaces it was traced to.
	Loc = DownHit.ImpactPoint + FVector(0.0f, 0.0f, 0.5f);

	const FVector RotatedUp =
		FRotationMatrix(Normal.Rotation()).GetScaledAxis(EAxis::Z);
	if (RotatedUp.Z > 0.5f)
	{
		Loc.Z += HeightAboveFloor;
	}

	FHitResult BackHit;
	if (!WoTRetailActorTrace(World, Loc,
		Loc + (-Normal) * TraceDistance, this, BackHit)
		|| !BackHit.bBlockingHit)
	{
		return false;
	}
	if (!FMath::IsNearlyEqual(BackHit.ImpactPoint.X, Loc.X, 1.0f)
		|| !FMath::IsNearlyEqual(BackHit.ImpactPoint.Y, Loc.Y, 1.0f))
	{
		return false;
	}
	// Retail accepts only when StartLocation equals that backed-off hanging hit, so the
	// placed trap hangs 0.5 uu in front of its wall, not on the face. A sight ray to a
	// point on the face ends inside the BSP and is blocked (a captain could never see
	// its alarm).
	Loc.X = BackHit.ImpactPoint.X + 0.5f * Normal.X;
	Loc.Y = BackHit.ImpactPoint.Y + 0.5f * Normal.Y;

	const FVector TempLocation = Loc + 32.0f * (-Normal);
	const FVector Up(0.0f, 0.0f, 1.0f);
	const FVector TestPoint = TempLocation + 64.0f * Normal;
	const FVector SidePoints[] = {
		TempLocation + (Normal ^ Up) * TrapRadius,
		TempLocation + (Normal ^ -Up) * TrapRadius,
	};
	for (const FVector& SidePoint : SidePoints)
	{
		FHitResult SideHit;
		if (!WoTRetailActorTrace(World, TestPoint, SidePoint, this, SideHit)
			|| !SideHit.bBlockingHit || (SideHit.ImpactNormal | Normal) < 0.99f)
		{
			return false;
		}
	}
	return true;
}

AActor* AWoTTrap::AnyActorsInArea(const FVector& Loc, float Radius) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	TArray<FOverlapResult> Overlaps;
	const FCollisionShape Sphere = FCollisionShape::MakeSphere(Radius);
	World->OverlapMultiByObjectType(Overlaps, Loc, FQuat::Identity,
		FCollisionObjectQueryParams::AllObjects, Sphere);
	for (const FOverlapResult& Overlap : Overlaps)
	{
		// The static world (floor/walls) is the UE equivalent of UE1's `Level`, which
		// RadiusActors never returned — so it is NOT an occupant. Only dynamic actors
		// (other deployed traps, pawns) count against separation.
		const UPrimitiveComponent* Comp = Overlap.GetComponent();
		if (Comp && Comp->GetCollisionObjectType() == ECC_WorldStatic)
		{
			continue;
		}
		AActor* Candidate = Overlap.GetActor();
		if (Candidate && !SameLogicalActor(Candidate)
			&& !Candidate->IsHidden() && Candidate->GetActorEnableCollision())
		{
			return Candidate;
		}
	}
	return nullptr;
}

void AWoTTrap::EndEditingResource()
{
	if (HasAuthority())
	{
		++EndEditingResourceCallCount;
		bEditing = false;
		ApplyEditingCollision(false);   // clients mirror via OnRep_Editing
		ApplyEditingGlowPresentation(); // Trap.uc:76 AmbientGlow=0
	}
}

void AWoTTrap::DestroyedTrigger(AActor* Other, APawn* EventInstigator)
{
	// Trap.DestroyedTrigger (Trap.uc:40-51): fire DestroyedEvent through the Tag/Event subsystem.
	// World-state change → authority only (matching the N4 event-authority scoping).
	if (bPreviewMode || DestroyedEvent.IsNone() || !HasAuthority())
	{
		return;
	}
	if (UWoTEventSubsystem* Events = GetWorld() ? GetWorld()->GetSubsystem<UWoTEventSubsystem>() : nullptr)
	{
		Events->FireEvent(DestroyedEvent, Other, EventInstigator);
	}
}

bool AWoTTrap::SameLogicalActor(AActor* Other) const
{
	// Trap.SameLogicalActor (Trap.uc:235-248): identical, this owns Other, or same base resource.
	if (this == Other)
	{
		return true;
	}
	if (Other && Other->GetOwner() == this)
	{
		return true;
	}
	if (AWoTTrap* OtherTrap = Cast<AWoTTrap>(Other))
	{
		return OtherTrap->GetBaseResource()
			== const_cast<AWoTTrap*>(this)->GetBaseResource();
	}
	return false;
}

void AWoTTrap::HideTrap()
{
	// Trap.Hide (Trap.uc:282-286): UE1 coupled visibility with collision.
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
}

void AWoTTrap::ShowTrap()
{
	// Trap.Show (Trap.uc:288-292).
	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
}

void AWoTTrap::SetCitadelEditorPresentation(
	EWoTCitadelResourcePresentation Presentation)
{
	CitadelEditorPresentation = Presentation;
}

void AWoTTrap::BindTrapVisual(UWoTVertexAnimComponent* Visual, FName MeshName,
                              const FVector& Scale)
{
	if (!Visual || MeshName.IsNone())
	{
		return;
	}
	if (Visual->SetVertexMesh(MeshName))
	{
		Visual->SetRelativeScale3D(Scale);
	}
}
