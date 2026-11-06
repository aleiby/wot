#include "WoTPortcullis.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTSoundTable.h"
#include "../WoTUE1Collision.h"


#include "Components/CapsuleComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

// --------------------------- PortcullisMover ------------------------------- //

AWoTPortcullisMover::AWoTPortcullisMover()
{
	// InitialState TriggerToggle so the base Tick keeps the raised (Open) phase put (it does not
	// auto-close); the portcullis drives the drop via StartClose and the raise via HideMover.
	// bDamageTriggered is OFF — we override TakeDamage (the base's "open on any hit" would defeat
	// WithstandDamage). The shipped Ignore/1 encroach defaults make the falling gate chip every
	// overlapping pawn without reversing.
	InitialState = EWoTMoverState::TriggerToggle;
	bDamageTriggered = false;
	EncroachType = EWoTMoverEncroachType::IgnoreWhenEncroach;
	EncroachDamage = 1;
	// PortcullisMover.uc:93-96 MoverSounds defaults. The Open pair is authored for
	// parity but never audible — ShouldPlayPhaseEdgeSound gates the Open-family edges
	// off (the retail raise is the quiet InterpolateTo(1,0) snap; see the override).
	OpeningSoundPath = FName(TEXT("WoTTraps/OpenPT"));
	OpenedSoundPath = FName(TEXT("WoTTraps/OpenPT"));
	ClosingSoundPath = FName(TEXT("WoTTraps/ClosePT"));
	ClosedSoundPath = FName(TEXT("WoTTraps/ClosedPT"));
}

void AWoTPortcullisMover::BeginPlay()
{
	Super::BeginPlay();   // caches BaseLoc/BaseRot, registers the tag, ApplyAlpha(0)
	HideMover();          // PortcullisMover.PreBeginPlay: start raised (open)
}

void AWoTPortcullisMover::HideMover()
{
	// InterpolateTo(1,0): jump to the raised keyframe instantly. Phase=Open replicates → clients snap
	// to alpha 1 via OnRep_Phase. Retail's snap is SILENT ("goes to open state quietly",
	// PortcullisMover.uc:27): InterpolateTo never plays a sound and the Open-label code that would
	// (DoOpen/FinishedOpening) never runs — ShouldPlayPhaseEdgeSound keeps the presentation pass
	// below (and the client OnRep pass) from sounding the Open edge; it still syncs the
	// SoundPhase dedup + the (unset) move-ambient loop.
	Phase = EWoTMoverPhase::Open;
	PhaseTime = 0.0f;
	ApplyAlpha(1.0f);
	ApplyPhaseSoundPresentation();
}

void AWoTPortcullisMover::Activate()
{
	WithstandDamage = GetDefault<AWoTPortcullisMover>()->WithstandDamage;   // default 120
	bActive = true;
	bSpent = false;   // re-enable the once-trigger (Disable/Enable('Trigger'))
	HideMover();
}

void AWoTPortcullisMover::Deactivate()
{
	bActive = false;
}

void AWoTPortcullisMover::WoTTrigger(AActor* /*Other*/, APawn* /*EventInstigator*/)
{
	// TriggerToggle.Trigger: drop the gate once. Authority-only (world-state change).
	if (!HasAuthority() || !bActive || bSpent || OwnerInactive())
	{
		return;
	}
	StartClose();     // Phase=Closing → the base Tick interpolates 1→0 to Closed (down, blocking)
	bSpent = true;    // Disable('Trigger') — the drop fires once
}

float AWoTPortcullisMover::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
                                      AController* EventInstigator, AActor* /*DamageCauser*/)
{
	// TriggerToggle.TakeDamage: WithstandDamage accounting; at <=0 the gate re-raises + deactivates
	// (the mover actor is NOT destroyed — parity). Authority-only.
	if (!HasAuthority() || !bActive || OwnerInactive())
	{
		return 0.0f;
	}

	FVector HitLoc = GetActorLocation();
	if (DamageEvent.IsOfType(FPointDamageEvent::ClassID))
	{
		HitLoc = static_cast<const FPointDamageEvent&>(DamageEvent).HitInfo.ImpactPoint;
	}
	WoTTrapHelpers::SpawnTrapBurst(*GetWorld(), HitLoc);
	// No damage-hit sound: DamageSoundName ships un-defaulted (see the destroyed
	// branch's record below).

	WithstandDamage -= FMath::RoundToInt(DamageAmount);
	if (WithstandDamage <= 0)
	{
		if (AWoTTrap* OwnerTrap = Cast<AWoTTrap>(GetOwner()))
		{
			OwnerTrap->DestroyedTrigger(this, EventInstigator ? EventInstigator->GetPawn() : nullptr);
		}
		HideMover();       // re-raise (open) — the visible "destroyed" is the gate lifting
		bActive = false;   // GotoState('Inactive')
		// DestroyedSound (PortcullisMover.uc:80 -> WOTTraps.Portcullis.DestroyedPT).
		// The Damage-hit sound stays silent: DamageSoundName ships un-defaulted.
		WoTSound::PlayReplicated(*GetWorld(), FName(TEXT("WoTTraps/DestroyedPT")), GetActorLocation(), 1600.0f);
	}
	return DamageAmount;
}

bool AWoTPortcullisMover::OwnerInactive() const
{
	const AWoTTrap* OwnerTrap = Cast<AWoTTrap>(GetOwner());
	return OwnerTrap && OwnerTrap->IsInactive(nullptr);
}

void AWoTPortcullisMover::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);   // Phase
	DOREPLIFETIME(AWoTPortcullisMover, WithstandDamage);
}

// ----------------------------- Portcullis ---------------------------------- //

AWoTPortcullis::AWoTPortcullis()
{
	// Portcullis defaults: bHidden (invisible archway trigger), collision 64 x 160.
	SetActorHiddenInGame(true);
	bReplicates = true;
	if (Capsule)
	{
		SetTriggerSize(64.0f, 160.0f);
		Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
		Capsule->OnComponentBeginOverlap.AddDynamic(this, &AWoTPortcullis::OnPortcullisOverlap);
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
	TrapType = TEXT("Portcullis");
}

void AWoTPortcullis::SetTriggerSize(float Radius, float HalfHeight)
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

void AWoTPortcullis::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTPortcullis, State);
}

FName AWoTPortcullis::GetCitadelEditorMarkerTexture(
	bool bActiveState, bool bHighlight) const
{
	return FName(bActiveState
		? (bHighlight ? TEXT("PortcullisActiveHighlight") : TEXT("PortcullisActive"))
		: (bHighlight ? TEXT("PortcullisInactiveHighlight") : TEXT("PortcullisInactive")));
}

void AWoTPortcullis::ApplyEditingCollision(bool bEditingNow)
{
	// Portcullis.uc:50-77: BeginEditingResource drops ALL collision — an inactive archway
	// is untraceable while editing (clicks pass through to the geometry behind,
	// EditorHUD.uc:266-273) — until BeginSelectResource (its class selected on the
	// palette) Show()s it; EndSelectResource re-hides an INACTIVE gate while an ACTIVE
	// one keeps collision (Super(Actor).Hide() is sprite-only), so an active gate is
	// always traceable for the Deactivate fall-through. The invisible archway must not
	// block weapon-aim / visibility traces at play. The pawn-overlap trigger volume
	// persists throughout.
	const bool bTraceable = bEditingNow
		&& (IsCitadelSelectedForTrace() || IsActive());
	if (CursorTraceCylinder)
	{
		CursorTraceCylinder->SetCollisionResponseToChannel(
			ECC_Visibility, bTraceable ? ECR_Block : ECR_Ignore);
	}
}

void AWoTPortcullis::OnRep_State()
{
	// Re-derive the local cursor collision from the replicated active state (an owner's
	// deactivate click may land while the palette is elsewhere — the gate must go
	// untraceable without waiting for a palette change).
	ApplyEditingCollision(bEditing);
}

void AWoTPortcullis::BeginPlay()
{
	Super::BeginPlay();
	if (IsPreviewMode())
	{
		return;
	}

	// Portcullis.PreBeginPlay: find the PortcullisMover tagged MoverTag and take ownership of it (so
	// the mover's TakeDamage/WoTTrigger can consult this trap's IsInactive).
	Mover = Cast<AWoTPortcullisMover>(WoTTrapHelpers::ResolveTaggedMover(
		GetWorld(), MoverTag, AWoTPortcullisMover::StaticClass()));
	if (Mover)
	{
		Mover->SetOwner(this);
	}
}

void AWoTPortcullis::Activate()
{
	// ActivateResource(bChangeState): Inactive → Active + arm the mover.
	State = EWoTPortcullisState::Active;
	if (Mover)
	{
		Mover->Activate();
	}
	ApplyEditingCollision(bEditing);   // authority-local mirror of OnRep_State
}

void AWoTPortcullis::Deactivate()
{
	// DeactivateResource(bChangeState): Active → Inactive + disarm the mover.
	State = EWoTPortcullisState::Inactive;
	if (Mover)
	{
		Mover->Deactivate();
	}
	ApplyEditingCollision(bEditing);   // authority-local mirror of OnRep_State
}

void AWoTPortcullis::OnPortcullisOverlap(UPrimitiveComponent* /*Comp*/, AActor* Other,
                                         UPrimitiveComponent* /*OtherComp*/, int32 /*BodyIndex*/,
                                         bool /*bFromSweep*/, const FHitResult& /*Hit*/)
{
	// Active.Touch (Portcullis.uc:160): a pawn walking under the arch drops the gate. Authority-only.
	if (!HasAuthority() || State != EWoTPortcullisState::Active || IsInactive(Other))
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
			{ OnPortcullisOverlap(Capsule, Candidate, nullptr, 0, false, FHitResult()); });
		return;
	}
	if (APawn* P = Cast<APawn>(Other))
	{
		if (Mover)
		{
			Mover->WoTTrigger(this, P);
		}
	}
}

// -------------------------- PortcullisInventory ---------------------------- //

AWoTPortcullisInventory::AWoTPortcullisInventory()
{
	// PortcullisInventory.uc defaultproperties (pre-placed → SpawnTempResource
	// false, like the staircase).
	ResourceClass = AWoTPortcullis::StaticClass();
	SpawnTempResource = false;
	Count = 2;
	// PortcullisInventory.uc:20,24: StatusIconFrame=Texture'WoTTraps.UI.M_SPortcullis' /
	// StatusIcon=Texture'WoTTraps.UI.I_SPortcullis'.
	BindStatusIcons(TEXT("WoTTraps"), TEXT("I_SPortcullis"), TEXT("M_SPortcullis"));
}
