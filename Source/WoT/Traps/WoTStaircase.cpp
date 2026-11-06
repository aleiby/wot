#include "WoTStaircase.h"
#include "../WoTSoundTable.h"
#include "../WoTUE1Collision.h"
#include "../WoTCollisionBlocker.h"

#include "../WoTMover.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"

AWoTStaircase::AWoTStaircase()
{
	// Staircase defaults: bHidden (an invisible trigger), collision 32x32.
	SetActorHiddenInGame(true);
	bReplicates = true;
	if (Capsule)
	{
		SetTriggerSize(32.0f, 32.0f);
		Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
		Capsule->OnComponentBeginOverlap.AddDynamic(this, &AWoTStaircase::OnStaircaseOverlap);
	}
	CursorTraceCylinder = CreateDefaultSubobject<UWoTFlatCylinderComponent>(
		TEXT("CursorTraceCylinder"));
	CursorTraceCylinder->SetupAttachment(Capsule);
	CursorTraceCylinder->SetCylinderSize(UE1CollisionRadius,
		UE1CollisionHalfHeight);
	CursorTraceCylinder->SetAnalyticCursorTrace(true);
	CursorTraceCylinder->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CursorTraceCylinder->SetCollisionObjectType(ECC_WorldDynamic);
	CursorTraceCylinder->SetCollisionResponseToAllChannels(ECR_Ignore);
	CursorTraceCylinder->SetGenerateOverlapEvents(false);
	TrapType = TEXT("Staircase");
}

void AWoTStaircase::SetTriggerSize(float Radius, float HalfHeight)
{
	UE1CollisionRadius = FMath::Max(0.0f, Radius);
	UE1CollisionHalfHeight = FMath::Max(0.0f, HalfHeight);
	if (Capsule)
	{
		Capsule->SetCapsuleSize(UE1CollisionRadius,
			WoTUE1Collision::ContainingCapsuleHalfHeight(
				UE1CollisionRadius, UE1CollisionHalfHeight), false);
	}
	if (CursorTraceCylinder)
	{
		CursorTraceCylinder->SetCylinderSize(
			UE1CollisionRadius, UE1CollisionHalfHeight);
	}
}

void AWoTStaircase::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTStaircase, State);
}

FName AWoTStaircase::GetCitadelEditorMarkerTexture(
	bool bActiveState, bool bHighlight) const
{
	return FName(bActiveState
		? (bHighlight ? TEXT("StairActiveHighlight") : TEXT("StairActive"))
		: (bHighlight ? TEXT("StairInactiveHighlight") : TEXT("StairInactive")));
}

void AWoTStaircase::ApplyEditingCollision(bool bEditingNow)
{
	// Staircase.uc:40-66: BeginEditingResource drops ALL collision — an inactive trigger
	// is untraceable while editing (clicks pass through to the geometry behind,
	// EditorHUD.uc:266-273) — until BeginSelectResource (its class selected on the
	// palette) Show()s it; EndSelectResource re-hides an INACTIVE trigger while an
	// ACTIVE one keeps collision (Super(Actor).Hide() is sprite-only), so an active
	// staircase is always traceable for the Deactivate fall-through. The block drops at
	// play. The pawn-overlap trigger volume persists throughout.
	const bool bTraceable = bEditingNow
		&& (IsCitadelSelectedForTrace() || IsActive());
	if (CursorTraceCylinder)
	{
		CursorTraceCylinder->SetCollisionResponseToChannel(
			ECC_Visibility, bTraceable ? ECR_Block : ECR_Ignore);
	}
}

void AWoTStaircase::OnRep_State()
{
	// Re-derive the local cursor collision from the replicated active state (an owner's
	// deactivate click may land while the palette is elsewhere — the trigger must go
	// untraceable without waiting for a palette change).
	ApplyEditingCollision(bEditing);
}

void AWoTStaircase::BeginPlay()
{
	Super::BeginPlay();
	if (IsPreviewMode())
	{
		return;
	}

	// Staircase.PreBeginPlay: find the Mover tagged MoverTag — the tilting stairs this trigger fires —
	// and hold it for six seconds once sprung (StayOpenTime = 6.0).
	TheStaircase = WoTTrapHelpers::ResolveTaggedMover(
		GetWorld(), MoverTag, AWoTMover::StaticClass());
	if (TheStaircase)
	{
		TheStaircase->StayOpenTime = 6.0f;
	}
}

void AWoTStaircase::Activate()
{
	// ActivateResource(bChangeState): Inactive → Active (the staircase can now spring).
	State = EWoTStaircaseState::Active;
	bArmed = true;
	ApplyEditingCollision(bEditing);   // authority-local mirror of OnRep_State
}

void AWoTStaircase::Deactivate()
{
	// DeactivateResource(bChangeState): Active → Inactive.
	State = EWoTStaircaseState::Inactive;
	bArmed = false;
	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().ClearTimer(ReArmTimer);
	}
	ApplyEditingCollision(bEditing);   // authority-local mirror of OnRep_State
}

void AWoTStaircase::OnStaircaseOverlap(UPrimitiveComponent* /*Comp*/, AActor* Other,
                                       UPrimitiveComponent* /*OtherComp*/, int32 /*BodyIndex*/,
                                       bool /*bFromSweep*/, const FHitResult& /*Hit*/)
{
	// Active.Touch (Staircase.uc:147-156): fire the mover once, arm the re-arm timer, disable Touch.
	// Authority-only — firing the mover is a world-state change (matching the N4 event-authority
	// scoping); clients see the stairs move via the mover's own Phase replication. A same-team /
	// editing toucher is inactive.
	if (!HasAuthority() || State != EWoTStaircaseState::Active || !bArmed
		|| IsInactive(Other))
	{
		return;
	}
	const auto Inside = [this](const AActor* Candidate)
		{
			return WoTUE1Collision::CylindersOverlap(GetActorLocation(),
				UE1CollisionRadius, UE1CollisionHalfHeight, Candidate);
		};
	if (!Inside(Other))
	{
		UE1Touches.DeferOutside(*this, Capsule, Other, Inside,
			[this](AActor* Candidate)
			{ OnStaircaseOverlap(Capsule, Candidate, nullptr, 0, false, FHitResult()); });
		return;
	}
	bArmed = false;
	if (TheStaircase)
	{
		GetWorldTimerManager().SetTimer(ReArmTimer, this, &AWoTStaircase::ReArm,
			FMath::Max(TheStaircase->StayOpenTime, 0.01f), /*loop*/ false);
		TheStaircase->WoTTrigger(this, Cast<APawn>(Other));
	}
	WoTSound::PlayReplicated(*GetWorld(), FName(TEXT("WoTTraps/RetractSC")), GetActorLocation(), 1600.0f);   // Staircase.uc:154
}

void AWoTStaircase::ReArm()
{
	// Active.Timer: the stairs have reset — Touch can spring them again.
	bArmed = true;
	WoTSound::PlayReplicated(*GetWorld(), FName(TEXT("WoTTraps/ResetSC")), GetActorLocation(), 1600.0f);   // Staircase.uc:161
}

AWoTStaircaseInventory::AWoTStaircaseInventory()
{
	// StaircaseInventory.uc defaultproperties.
	ResourceClass = AWoTStaircase::StaticClass();
	SpawnTempResource = false;
	Count = 2;
	// StaircaseInventory.uc:20,24: StatusIconFrame=Texture'WoTTraps.UI.M_SStaircase' /
	// StatusIcon=Texture'WoTTraps.UI.I_SStaircase'.
	BindStatusIcons(TEXT("WoTTraps"), TEXT("I_SStaircase"), TEXT("M_SStaircase"));
}
