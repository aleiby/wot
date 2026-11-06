#include "WoTSpear.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTSoundTable.h"

#include "WoTTrapBlocker.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

AWoTSpear::AWoTSpear()
{
	PrimaryActorTick.bCanEverTick = true;
	VisualMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("VisualMesh"));
	VisualMesh->SetupAttachment(RootComponent);
	VisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	bReplicates = true;
	TrapType = TEXT("Spear");
	// Spear.uc tunables (base AWoTTrap fields).
	DamageAmount = 5;
	HeightAboveFloor = 48;
	SeparationDistance = 32;
	TrapRadius = 10;
	if (Capsule)
	{
		Capsule->InitCapsuleSize(10.0f, 10.0f);
		Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	}
}

void AWoTSpear::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Phase != EWoTSpearPhase::Activating || ActivationOccupants.IsEmpty())
	{
		return;
	}
	TArray<TWeakObjectPtr<AActor>> Remove;
	for (const TPair<TWeakObjectPtr<AActor>, FVector>& Entry : ActivationOccupants)
	{
		AActor* Other = Entry.Key.Get();
		if (!IsValid(Other))
		{
			Remove.Add(Entry.Key);
			continue;
		}
		if (!Other->GetActorLocation().Equals(Entry.Value, KINDA_SMALL_NUMBER))
		{
			Remove.Add(Entry.Key);
			if (!Blockers.IsEmpty() && IsValid(Blockers.Last())
				&& Blockers.Last()->IsOverlappingActor(Other))
			{
				Impale(Other);
			}
		}
	}
	for (const TWeakObjectPtr<AActor>& Other : Remove)
	{
		ActivationOccupants.Remove(Other);
	}
}

void AWoTSpear::BeginPlay()
{
	Super::BeginPlay();
	BindTrapVisual(VisualMesh, FName(TEXT("Spear")), FVector(1.0f));
	ApplyPhasePresentation();
	if (HasAuthority() && !IsPreviewMode())
	{
		RebuildBlockersAtCurrentTransform();
	}
}

void AWoTSpear::Destroyed()
{
	// Spear.Destroyed: tear down the blockers with the spear.
	DestroyBlockers();
	Super::Destroyed();
}

void AWoTSpear::DestroyBlockers()
{
	for (TObjectPtr<AWoTTrapBlocker>& B : Blockers)
	{
		if (IsValid(B))
		{
			B->Destroy();
		}
	}
	Blockers.Reset();
}

void AWoTSpear::RebuildBlockersAtCurrentTransform()
{
	DestroyBlockers();
	InitBlockers();
}

void AWoTSpear::InitBlockers()
{
	// Spear.InitBlockers: 4 blockers spaced along +X over ImpaleRange (Increment = range / (N+1)).
	if (Blockers.Num() > 0)
	{
		return;
	}
	const int32 Count = 4;
	const float Increment = ImpaleRange / (Count + 1);
	const FVector Forward = GetActorForwardVector();
	for (int32 i = 0; i < Count; ++i)
	{
		const FVector Loc = GetActorLocation() + Increment * (i + 1) * Forward;
		FActorSpawnParameters SP;
		SP.Owner = this;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTrapBlocker* B = GetWorld()->SpawnActor<AWoTTrapBlocker>(
			AWoTTrapBlocker::StaticClass(), Loc, FRotator::ZeroRotator, SP);
		Blockers.Add(B);
	}
	SetBlockers(true, false, false, TriggerRadius, TriggerRadius);
}

bool AWoTSpear::DeployResource(FVector Loc, FVector Normal)
{
	if (!CalcLocation(Loc, Normal))
	{
		return false;
	}
	if (AnyActorsInArea(Loc, SeparationDistance) != nullptr)
	{
		return false;
	}
	SetActorLocationAndRotation(Loc, Normal.Rotation());
	// Spear.uc:65 returns ValidateTrap(): Trap.ValidateTrap (Trap.uc:185-204) does
	// SetOwner(None) on every final placement (owner drives net relevancy — retail cleared
	// it for exactly that reason; the tutorial-trigger poll is deferred, as in Pit).
	if (!IsPreviewMode())
	{
		SetOwner(nullptr);
	}
	return true;
}

bool AWoTSpear::RemoveResource()
{
	if (bLocked)
	{
		return false;
	}
	DestroyBlockers();
	return Super::RemoveResource();
}

void AWoTSpear::BeginEditingResource(uint8 PlacedByTeam)
{
	Super::BeginEditingResource(PlacedByTeam);   // sets bEditing + ApplyEditingCollision(true)
	if (HasAuthority() && !IsPreviewMode())
	{
		// Spear.BeginEditingResource (Spear.uc:89-95): InitBlockers(); HideBlockers() —
		// the blockers exist at the final transform but carry NO collision for the whole
		// edit phase (so they never occupy placement area nor spring).
		RebuildBlockersAtCurrentTransform();
		HideBlockers();
	}
}

void AWoTSpear::EndEditingResource()
{
	Super::EndEditingResource();   // clears bEditing + ApplyEditingCollision(false)
	// Spear.EndEditingResource (Spear.uc:97-102): the manager stops being traceable at play
	// (collision handled in ApplyEditingCollision) and the blockers re-arm —
	// SetBlockers(true,false,false,TriggerRadius,TriggerRadius).
	SetBlockers(true, false, false, TriggerRadius, TriggerRadius);
}

void AWoTSpear::ApplyEditingCollision(bool bEditingNow)
{
	if (!Capsule)
	{
		return;
	}
	if (bEditingNow)
	{
		// UE1 bCollideActors made the manager selectable without making it block players.
		// The canonical editor cursor uses Visibility, so mirror Pit's query-only response.
		Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	}
	else
	{
		// Spear.EndEditingResource (Spear.uc:100): SetCollision(false,false,false) — the
		// manager stops being traceable at play (its TrapBlockers carry runtime sensing).
		// This also clears the edit-time ECC_Visibility block so it does not leak into
		// runtime.
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void AWoTSpear::HideBlockers()
{
	// Spear.HideBlockers (Spear.uc:141-149): every blocker — including the always-collide
	// first/last that SetBlockers pins — drops collision at TriggerRadius size.
	for (TObjectPtr<AWoTTrapBlocker>& B : Blockers)
	{
		if (IsValid(B))
		{
			B->SetBlocking(false, false, false, TriggerRadius, TriggerRadius);
		}
	}
}

void AWoTSpear::SetBlockers(bool bCollide, bool bBlock, bool bDamage,
                            float NewRadius, float NewHeight)
{
	// Spear.SetBlockers: the FIRST blocker always collides (the trigger); the LAST always damages (the
	// tip); the middle take the passed flags.
	if (Blockers.Num() == 0)
	{
		return;
	}
	Blockers[0]->SetBlocking(true, bBlock, bDamage, NewRadius, NewHeight);
	for (int32 i = 1; i < Blockers.Num() - 1; ++i)
	{
		Blockers[i]->SetBlocking(bCollide, bBlock, bDamage, NewRadius, NewHeight);
	}
	Blockers.Last()->SetBlocking(true, bBlock, true, NewRadius, NewHeight);
}

void AWoTSpear::TrapTouch(AActor* Other)
{
	// Waiting.Touch (via a non-damage blocker): spring the spear.
	if (Phase == EWoTSpearPhase::Waiting)
	{
		TryActivate(Other);
	}
}

void AWoTSpear::DamageActor(AActor* Other)
{
	// A damage blocker forwarded a touch. Waiting.DamageActor -> Touch (spring); Activating.DamageActor
	// -> the actual impale. Other phases have no handler (no damage).
	if (bEnablingActivationBlockers)
	{
		return;   // SetCollision itself is not a retail Touch event.
	}
	if (const FVector* ActivationLocation = ActivationOccupants.Find(Other))
	{
		if (Other->GetActorLocation().Equals(*ActivationLocation, KINDA_SMALL_NUMBER))
		{
			return;
		}
		ActivationOccupants.Remove(Other);
	}
	if (Phase == EWoTSpearPhase::Waiting)
	{
		TryActivate(Other);
	}
	else if (Phase == EWoTSpearPhase::Activating)
	{
		Impale(Other);
	}
}

void AWoTSpear::TryActivate(AActor* Other)
{
	if (!HasAuthority() || IsInactive(Other))
	{
		return;
	}
	if (Cast<APawn>(Other))
	{
		EnterActivating(Other);
	}
}

void AWoTSpear::Impale(AActor* Other)
{
	APawn* P = Cast<APawn>(Other);
	if (!P || !HasAuthority())
	{
		return;
	}
	bool bAlready = false;
	ImpaledThisCycle.Add(P, &bAlready);
	if (bAlready)
	{
		return;   // one impale per contact per arming (see the .h note)
	}
	const FVector Dir = (P->GetActorLocation() - GetActorLocation()).GetSafeNormal();

	// Momentum shove now rides the shared AWoTCharacter::TakeDamage fold.
	// The live-pawn path is engine Pawn.TakeDamage: a WALKING
	// pawn's momentum.Z is floored at 0.4*|momentum| (the upward pop that lifts the victim off the
	// floor), THEN momentum/Mass is ADDED (AddVelocity — 175-mass player: 20000/175 ≈ 114 uu/s
	// along the shove + ≈ 46 up), applied BEFORE the damage. (A LETHAL impale still loses the
	// shove: ShutdownCorpsePresentation's StopMovementImmediately/DisableMovement zeroes it — the
	// corpse-halt presentation divergence, not a Spear behavior; PORT_NOTES.)
	// Spear damages with the 'killed' type (a killer-less environment hit; a displacement instigator,
	// if any, is credited by AWoTGameMode::ScoreKill's SuicideInstigator path).
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("killed");
	// Spear.uc:252 passes Direction * MomentumConstant; Pawn.TakeDamage owns the
	// walking floor and Mass divide (retail-bytecode probe verdict GUARDED).
	Ev.UE1Momentum = Dir * MomentumConstant;
	Ev.HitInfo.ImpactPoint = P->GetActorLocation();
	P->TakeDamage(DamageAmount, Ev, nullptr, this);
}

void AWoTSpear::EnterActivating(AActor* Activator)
{
	// Spear.uc toggles the blockers, but SetCollision does not synthesize Touch
	// for a pair already touching. A later movement event owns the impale.
	Phase = EWoTSpearPhase::Activating;
	ApplyPhasePresentation();
	ImpaledThisCycle.Reset();
	ActivationOccupants.Reset();
	if (IsValid(Activator))
	{
		ActivationOccupants.Add(Activator, Activator->GetActorLocation());
	}
	if (!Blockers.IsEmpty() && IsValid(Blockers.Last()))
	{
		TArray<AActor*> Overlapping;
		Blockers.Last()->GetOverlappingActors(Overlapping, APawn::StaticClass());
		for (AActor* Other : Overlapping)
		{
			ActivationOccupants.FindOrAdd(Other) = Other->GetActorLocation();
		}
	}
	SetBlockers(false, false, false, DamageRadius, TriggerRadius);
	bEnablingActivationBlockers = true;
	SetBlockers(true, true, false, DamageRadius, TriggerRadius);
	bEnablingActivationBlockers = false;
	WoTSound::PlayReplicated(*GetWorld(), FName(TEXT("WoTTraps/ActivateSP")), GetActorLocation(), 1600.0f);   // Spear.uc:192
	GetWorldTimerManager().SetTimer(PhaseTimer, this, &AWoTSpear::EnterExtended, ThrustTime, false);
}

void AWoTSpear::EnterExtended()
{
	// Extended: held out for 2s (no damage window), then retract.
	Phase = EWoTSpearPhase::Extended;
	ApplyPhasePresentation();
	GetWorldTimerManager().SetTimer(PhaseTimer, this, &AWoTSpear::EnterReseting, ExtendedTime, false);
}

void AWoTSpear::EnterReseting()
{
	// Reseting.begin: retract; after 1s go back to Waiting. The shipped Reseting.Timer re-impale scan
	// (Spear.uc:210-223) is retail-dead in MP: it reads the MANAGER's Touching[], but EndEditingResource's
	// SetCollision(false,false,false) (uc:100) removed the manager from the UE1 collision hash
	// in the original engine, so victims only ever touch the BLOCKERS — intentionally not ported
	// (PORT_NOTES "P27+G30"); the trigger re-arms in Waiting.
	Phase = EWoTSpearPhase::Reseting;
	ApplyPhasePresentation();
	SetBlockers(false, false, false, TriggerRadius, TriggerRadius);
	WoTSound::PlayReplicated(*GetWorld(), FName(TEXT("WoTTraps/RetractSP")), GetActorLocation(), 1600.0f);   // Spear.uc:226
	GetWorldTimerManager().SetTimer(PhaseTimer, this, &AWoTSpear::EnterWaiting, ResetTime, false);
}

void AWoTSpear::EnterWaiting()
{
	// Waiting: re-armed, retracted.
	Phase = EWoTSpearPhase::Waiting;
	ActivationOccupants.Reset();
	ApplyPhasePresentation();
	SetBlockers(true, false, false, TriggerRadius, TriggerRadius);
}

bool AWoTSpear::HasActivePhaseTimerForTest() const
{
	return GetWorld() && GetWorld()->GetTimerManager().IsTimerActive(PhaseTimer);
}

void AWoTSpear::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTSpear, Phase);
}

AWoTSpearInventory::AWoTSpearInventory()
{
	// SpearInventory.uc defaultproperties (player-spawned).
	ResourceClass = AWoTSpear::StaticClass();
	Count = 2;
	// SpearInventory.uc:20,23: StatusIconFrame=Texture'WoTTraps.UI.M_SSpear' /
	// StatusIcon=Texture'WoTTraps.UI.I_SSpear'.
	BindStatusIcons(TEXT("WoTTraps"), TEXT("I_SSpear"), TEXT("M_SSpear"));
}

void AWoTSpear::ApplyPhasePresentation()
{
	// Waiting holds f0; Activating plays the 19-frame thrust; Extended holds the tip
	// out; Reseting tweens back to the retracted pose over the shipped 0.25s.
	if (!VisualMesh || !VisualMesh->HasDisplayMesh())
	{
		return;
	}
	switch (Phase)
	{
		case EWoTSpearPhase::Activating:
		{
			// Rate fitted so the thrust fills the sim window exactly: duration =
			// (NumFrames-1)/(Rate x PlayRate) = ThrustTime.
			float ThrustRate = 1.0f;
			if (const FWoTAnimSequence* Seq =
			        VisualMesh->FindSequence(FName(TEXT("Activating"))))
			{
				ThrustRate = (Seq->NumFrames - 1) / (Seq->Rate * ThrustTime);
			}
			VisualMesh->PlayAnim(FName(TEXT("Activating")), ThrustRate);
			break;
		}
		case EWoTSpearPhase::Extended:
			VisualMesh->SetFrameAndStop(FName(TEXT("Activating")), 1.0f);
			break;
		case EWoTSpearPhase::Reseting:
			VisualMesh->TweenAnim(FName(TEXT("Waiting")), 0.25f);
			break;
		default:
			VisualMesh->SetFrameAndStop(FName(TEXT("Waiting")), 0.0f);
			break;
	}
}
