#include "WoTAngrealInvBalefire.h"
#include "Engine/DamageEvents.h"
#include "../WoTBeamEffect.h"
#include "../WoTBalefireFX.h"
#include "../WoTCarcass.h"
#include "../WoTDecoration.h"
#include "../WoTDecal.h"
#include "../WoTRainCyd.h"
#include "../WoTSoundTable.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../Projectiles/WoTLegionProjectile.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTWall.h"

#include "../WoTCharacter.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

AWoTAngrealInvBalefire::AWoTAngrealInvBalefire()
{
	BindStatusIcons(TEXT("I_Balefire"), TEXT("M_Balefire"));
	// AngrealInvBalefire defaultproperties (simulation subset).
	BalefireDamage = 400.0f;
	PrimeTime = 2.0f;
	StreakLength = 8000.0f;
	Kickback = 1000.0f;
	VisualOffset = 50.0f;

	bElementFire = true;
	bElementSpirit = true;
	bRare = true;
	bOffensive = true;
	bCombat = true;
	MinInitialCharges = 3;
	MaxInitialCharges = 5;
	MaxCharges = 10;
	Priority = 10.0f;
	MaxChargesInGroup = 4;
	MinChargeGroupInterval = 5.0f;
	MissOdds = 1.0f;
	InventoryGroup = 51;

	// Angreal.ActivateBF / Angreal.LaunchBF — the base plays these as the
	// activate/deactivate sounds (soft, bound by the sound pass).
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateBF.ActivateBF"));
	DeactivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchBF.LaunchBF"));
}

void AWoTAngrealInvBalefire::CastArtifact()
{
	// FinishCharging.Cast() (AngrealInvBalefire.uc:258-266): a re-press while the
	// auto-finish is pending only cancels the pending shot — GotoState('') — and
	// the charge keeps accruing on the ORIGINAL CastTime. The global Cast() does
	// not re-run (no new ActivateSound, no CastTime reset).
	if (UWorld* W = GetWorld())
	{
		if (W->GetTimerManager().IsTimerActive(FinishChargingTimer))
		{
			W->GetTimerManager().ClearTimer(FinishChargingTimer);
			return;
		}
	}

	Super::CastArtifact();   // ActivateSound (gather begins)

	if (UWorld* W = GetWorld())
	{
		CastTime = W->GetTimeSeconds();
	}
	bCasting = true;
	ReconcileCastingPresentation();
	// Replicated bCasting reconciles the local BalefireCorona gather effect.
}

void AWoTAngrealInvBalefire::UnCastArtifact()
{
	if (!bCasting)
	{
		return;
	}

	UWorld* W = GetWorld();
	const float Now = W ? W->GetTimeSeconds() : 0.0f;
	AWoTCharacter* Caster = Cast<AWoTCharacter>(GetOwner());

	// Killed before fully charged -> auto-prime to half so the shot still fires.
	if (Caster && !Caster->IsAlive() && Now < CastTime + PrimeTime / 2.0f)
	{
		CastTime = Now - PrimeTime / 2.0f;
	}

	// Released before half-prime: finish charging first, then fire (the original's
	// FinishCharging state Sleeps the remainder and calls UnCast() again).
	if (Now < CastTime + PrimeTime / 2.0f)
	{
		const float Remaining = (CastTime + PrimeTime / 2.0f) - Now;
		if (W)
		{
			W->GetTimerManager().SetTimer(FinishChargingTimer, this,
				&AWoTAngrealInvBalefire::FinishCharging, FMath::Max(Remaining, 0.01f), false);
		}
		return;
	}

	Super::UnCastArtifact();   // DeactivateSound (LaunchBF)
	bCasting = false;
	ReconcileCastingPresentation();

	const float Scalar = FMath::Min((Now - CastTime) / PrimeTime, 1.0f);
	Fire(Scalar);
	UseCharge();
}

void AWoTAngrealInvBalefire::FinishCharging()
{
	// FinishCharging state: the prime elapsed, so re-entering UnCast now fires.
	UnCastArtifact();
}

void AWoTAngrealInvBalefire::Fire(float Scalar)
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	UWorld* W = GetWorld();
	if (!OwnerPawn || !W || !HasAuthority())
	{
		return;
	}

	const FRotator ViewRot = OwnerPawn->GetViewRotation();
	const FVector Dir = ViewRot.Vector();
	FVector Start = OwnerPawn->GetActorLocation();
	Start.Z += OwnerPawn->BaseEyeHeight;
	const FRotator BeamRot = Dir.Rotation();
	AController* InstigatorCtrl = OwnerPawn->GetController();

	// Kickback: the caster is shoved back along the beam, scaled by charge.
	if (AWoTCharacter* CasterCh = Cast<AWoTCharacter>(OwnerPawn))
	{
		CasterCh->AddUE1Velocity(-Dir * Kickback * Scalar);
	}

	// The visual line (BalefireLine: Sparks01 segments fading 1.2s) + the
	// end-scorch decals traced 500uu-stepwise BOTH directions along the beam
	// (AngrealInvBalefire.uc:116-121 + PlaceDecals). The beam replicates;
	// decals spawn server-side through the replicated decal path.
	const FVector VisualStart = Start + ViewRot.RotateVector(FVector(50.0f, 0.0f, -30.0f));
	const FVector BeamEnd = Start + Dir * StreakLength;
	const FTransform BeamTransform(BeamRot, (VisualStart + BeamEnd) * 0.5f);
	if (AWoTBeamEffect* Line = W->SpawnActorDeferred<AWoTBeamEffect>(
		AWoTBeamEffect::StaticClass(), BeamTransform, OwnerPawn, OwnerPawn,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
	{
		Line->Config = FName(TEXT("BalefireLine"));
		Line->Start = VisualStart;
		Line->End = BeamEnd;
		// Initial replication sees a complete beam at its relevancy midpoint; a
		// dedicated server never runs the cosmetic Tick that used to move it there.
		Line->FinishSpawning(BeamTransform);
		Line->SetLifeSpan(1.2f);   // BalefireLine.uc LifeSpan
	}
	// Both passes walk the VISIBLE line (AngrealInvBalefire.uc:119-120):
	// PlaceDecals(VisualStart, Normal(End - VisualStart)) and
	// PlaceDecals(End, Normal(VisualStart - End)). The return pass used -Dir
	// (the eye ray, up to 30uu off the drawn beam), so exit-side scorches
	// missed where the beam visibly pierced the wall.
	PlaceBeamDecals(VisualStart, (BeamEnd - VisualStart).GetSafeNormal(), StreakLength);
	PlaceBeamDecals(BeamEnd, (VisualStart - BeamEnd).GetSafeNormal(), StreakLength);

	// Strike every balefireable pawn whose body lies within the beam cylinder.
	// The cylinder test (DidIndeedHit) has no far cap: the beam pierces everything.
	for (TActorIterator<AWoTCharacter> It(W); It; ++It)
	{
		AWoTCharacter* Victim = *It;
		if (Victim == OwnerPawn || !IsValid(Victim))
		{
			continue;
		}

		// Transform into the beam's local frame (X forward). In front of the
		// muzzle and within the victim's own capsule half-extents = a hit.
		const FVector Rel = BeamRot.UnrotateVector(Victim->GetActorLocation() - Start);
		const float HalfHeight = Victim->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const float Radius = Victim->GetCapsuleComponent()->GetScaledCapsuleRadius();
		if (Rel.X < -VisualOffset || FMath::Abs(Rel.Z) > HalfHeight
			|| FMath::Abs(Rel.Y) > Radius)
		{
			continue;
		}

		const FVector HitLocation = Start + BeamRot.RotateVector(FVector(Rel.X, 0.0f, 0.0f));
		// Breaks all rules = skips only ProcessEffect (dome/element interception);
		// DE.Invoke() is a full TakeDamage, so the Personal Shield and the game
		// rules still apply. UnrealScript float->int truncates `Damage * Scalar`.
		// Retail's singular Actor.Tag maps to the port's Tags array. Preserve the
		// complete array, replace it with the death marker for damage/carcass spawn,
		// and restore it only when the pawn survives.
		const TArray<FName> PrevTags = Victim->Tags;
		Victim->Tags.Reset();
		Victim->Tags.Add(TEXT("Balefired"));
		Victim->TakeRawDamage(FMath::TruncToInt(BalefireDamage * Scalar),
			TEXT("Balefire"), InstigatorCtrl, HitLocation,
			// AngrealInvBalefire.uc:319: HitNormal * Kickback * -1 * Scalar.
			// DidIndeedHit stores HitNormal=Dir at uc:231, so the victim is
			// shoved opposite the beam.
			-Dir * Kickback * Scalar);
		WoTSound::PlayReplicated(*W, TEXT("Angreal/HitBF"), HitLocation, 1600.0f);
		if (Victim->IsAlive())
		{
			Victim->Tags = PrevTags;
		}
		else
		{
			// WOTCarcass.InitFor copies this marker; Tick performs retail's
			// AllActors(class'Carcass', ..., 'Balefired') removal sweep.
			bCheckForCarcasses = true;
			SetActorTickEnabled(true);
		}
	}

	// Existing RemoveFromTimeline non-pawn branches, using the same unbounded
	// forward-cylinder test. Bounds stand in for the pawn capsule half-extents.
	for (TActorIterator<AActor> It(W); It; ++It)
	{
		AActor* Other = *It;
		if (!IsValid(Other) || Other == this || Other == OwnerPawn || Other->IsA<APawn>()
			|| Other->IsA<AWoTBalefireRemovalMarker>())
		{
			continue;
		}
		const bool bDecoration = Other->IsA<AWoTDecoration>();
		const bool bCarcass = Other->IsA<AWoTCarcass>();
		const bool bWall = Other->IsA<AWoTWallSlab>();
		const bool bGate = Other->IsA<AWoTPortcullisMover>();
		const bool bSegment = Other->IsA<AWoTMashadarSegment>();
		const bool bLegion = Other->IsA<AWoTLegionProjectile>();
		// Retail Decoration includes Carcass; the port split carcasses into their
		// own actor family, so include that folded subtype explicitly.
		if (!(bDecoration || bCarcass || bWall || bGate || bSegment || bLegion))
		{
			continue;
		}
		FVector TestOrigin;
		float TestRadius = 1.0f;
		float TestHalfHeight = 1.0f;
		// A landed carcass uses its live shifted capsule exactly. Other port
		// decorations have no authored collision component, so their retained
		// approximation stays the visual bounds.
		if (const AWoTCarcass* Carcass = Cast<AWoTCarcass>(Other))
		{
			TestOrigin = Carcass->Capsule->GetComponentLocation();
			TestRadius = Carcass->Capsule->GetScaledCapsuleRadius();
			TestHalfHeight = Carcass->Capsule->GetScaledCapsuleHalfHeight();
		}
		else
		{
			FVector Extent;
			Other->GetActorBounds(false, TestOrigin, Extent);
			TestRadius = FMath::Max(Extent.Y, 1.0f);
			TestHalfHeight = FMath::Max(Extent.Z, 1.0f);
		}
		const FVector Rel = BeamRot.UnrotateVector(TestOrigin - Start);
		if (Rel.X < -VisualOffset || FMath::Abs(Rel.Z) > TestHalfHeight
			|| FMath::Abs(Rel.Y) > TestRadius)
		{
			continue;
		}
		const FVector Hit = Start + BeamRot.RotateVector(FVector(Rel.X, 0, 0));
		WoTSound::PlayReplicated(*W, TEXT("Angreal/HitBF"), Hit, 1600.0f);
		if (bDecoration || bCarcass || bWall || bSegment || bLegion)
		{
			WoTBalefireFX::SpawnRemoval(*W, Other, Hit, true);
		}
		else
		{
			FDamageEvent DamageEvent;
			Other->TakeDamage(BalefireDamage * Scalar, DamageEvent, InstigatorCtrl, this);
			const AWoTPortcullisMover* Gate = Cast<AWoTPortcullisMover>(Other);
			if (!Gate || Gate->WithstandDamage <= 0)
			{
				WoTBalefireFX::SpawnRemoval(*W, Other, Hit, true);
			}
		}
	}
}

void AWoTAngrealInvBalefire::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (HasAuthority() && bCheckForCarcasses)
	{
		SweepBalefiredCarcasses();
	}
}

void AWoTAngrealInvBalefire::SweepBalefiredCarcasses()
{
	UWorld* W = GetWorld();
	if (!W)
	{
		return;
	}
	bool bFound = false;
	for (TActorIterator<AWoTCarcass> It(W); It; ++It)
	{
		AWoTCarcass* Carcass = *It;
		if (IsValid(Carcass) && Carcass->ActorHasTag(TEXT("Balefired")))
		{
			bFound = true;
			WoTBalefireFX::SpawnRemoval(*W, Carcass,
				Carcass->GetActorLocation(), true);
		}
	}
	if (bFound)
	{
		bCheckForCarcasses = false;
		if (!bCasting && GetItemState() == EWoTInventoryState::Held)
		{
			SetActorTickEnabled(false);
		}
	}
}

void AWoTAngrealInvBalefire::Reset()
{
	Super::Reset();
	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().ClearTimer(FinishChargingTimer);
	}
	CastTime = 0.0f;
	bCheckForCarcasses = false;
	bCasting = false;
	ReconcileCastingPresentation();
}

void AWoTAngrealInvBalefire::Failed()
{
	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().ClearTimer(FinishChargingTimer);
	}
	bCasting = false;
	ReconcileCastingPresentation();
	// ReconcileCastingPresentation released the gather effect above.
	Super::Failed();
}

void AWoTAngrealInvBalefire::OnRep_Casting()
{
	ReconcileCastingPresentation();
}

void AWoTAngrealInvBalefire::ReconcileCastingPresentation()
{
	if (bCasting && !IsValid(GatherEffect))
	{
		if (UWorld* W = GetWorld())
		{
			if (APawn* Pawn = Cast<APawn>(GetOwner()))
			{
				GatherEffect = WoTBalefireFX::SpawnGather(*W, Pawn);
			}
		}
	}
	else if (!bCasting && IsValid(GatherEffect))
	{
		GatherEffect->Destroy();
		GatherEffect = nullptr;
	}
}

void AWoTAngrealInvBalefire::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bCasting = false;
	ReconcileCastingPresentation();
	Super::EndPlay(EndPlayReason);
}

void AWoTAngrealInvBalefire::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTAngrealInvBalefire, bCasting);
}

void AWoTAngrealInvBalefire::PlaceBeamDecals(FVector From, const FVector& Direction,
                                             float Limit)
{
	// AngrealInvBalefire.PlaceDecals: walk the beam in 500uu traces, scorching
	// every wall hit (stepping 100uu past each) until the length is spent.
	UWorld* W = GetWorld();
	float TraceInterval = 500.0f;
	while (Limit > 0.0f && W)
	{
		TraceInterval = FMath::Min(TraceInterval, Limit);
		const FVector To = From + TraceInterval * Direction;
		// Trace(..., bTraceActors=False) => TRACE_VisBlocking: level geometry
		// and movers only (UE1's native Trace), the same
		// rule as the RainCyd ground trace. Projected blockers, decorations and
		// other world-static actors are skipped and the segment re-traced.
		FHitResult Hit;
		const bool bHit = WoTVisBlockingTrace(W, From, To, this, Hit);
		if (bHit)
		{
			WoTDecalFX::SpawnAligned(*W, TEXT("BalefireDecal"), Hit.ImpactPoint,
			                         Hit.ImpactNormal, /*bReplicated*/true);
			const FVector Stepped = Hit.ImpactPoint + 100.0f * Direction;
			Limit -= FVector::Dist(Stepped, From);
			From = Stepped;
		}
		else
		{
			From = To;
			Limit -= TraceInterval;
		}
	}
}
