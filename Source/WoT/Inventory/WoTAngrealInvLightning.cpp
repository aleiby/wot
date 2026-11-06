#include "WoTAngrealInvLightning.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../WoTBeamEffect.h"
#include "../WoTMeshFX.h"

#include "WoTDecreaseHealthLeech.h"
#include "WoTIceLeech.h"
#include "WoTReflector.h"
#include "WoTInventoryComponent.h"
#include "WoTAutoTargetReflector.h"
#include "../WoTCharacter.h"
#include "../WoTZoneVolume.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "Net/UnrealNetwork.h"

AWoTAngrealInvLightning::AWoTAngrealInvLightning()
{
	BindStatusIcons(TEXT("I_LightningStrike"), TEXT("M_LightningStrike"));
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// AngrealInvLightning defaultproperties (simulation subset).
	AttachmentRange = 180.0f;
	MaxCollisionRadius = 250.0f;
	MaxEffectiveRange = 900.0f;
	MaxDamageRate = 25.0f;
	MaxWaterDamage = 50.0f;
	WaterDamageRadius = 1000.0f;
	PercentStrain = 0.9f;
	LatchDelay = 0.3f;

	DurationType = EWoTDurationType::DT_Charge;   // AngrealInvLightning.uc:595
	bElementFire = true;
	bElementWater = true;
	bElementAir = true;
	bUncommon = true;
	bOffensive = true;
	MinInitialCharges = 10;
	MaxInitialCharges = 20;
	MaxCharges = 30;
	Priority = 8.0f;
	MaxChargesInGroup = 30;
	MinChargesInGroup = 10;
	MaxChargeUsedInterval = 1.0f;
	MinChargeGroupInterval = 3.0f;
	InventoryGroup = 62;

	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchLS.LaunchLS"));
	DeactivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/EndLS.EndLS"));
}

bool AWoTAngrealInvLightning::IsUsingLightning(const APawn* Pawn)
{
	const UWoTInventoryComponent* Inventory = Pawn
		? Pawn->FindComponentByClass<UWoTInventoryComponent>() : nullptr;
	const AWoTAngrealInvLightning* Lightning = Inventory
		? Cast<AWoTAngrealInvLightning>(
			Inventory->FindInventoryType(StaticClass())) : nullptr;
	return Lightning && Lightning->bCasting;
}

void AWoTAngrealInvLightning::NotifyFootZoneChange(
	APawn* WetPawn, const AWoTZoneVolume* NewFootZone)
{
	if (!IsValid(WetPawn) || !WetPawn->HasAuthority()
		|| !IsValid(NewFootZone) || !NewFootZone->bWaterZone) return;
	TSet<AWoTAngrealInvLightning*> Sources;
	if (UWoTInventoryComponent* Inventory =
		WetPawn->FindComponentByClass<UWoTInventoryComponent>())
	{
		if (AWoTAngrealInvLightning* Held = Cast<AWoTAngrealInvLightning>(
			Inventory->FindInventoryType(StaticClass())))
		{
			if (Held->bCasting) Sources.Add(Held);
		}
	}
	TArray<UWoTDecreaseHealthLeech*> HealthLeeches;
	WetPawn->GetComponents<UWoTDecreaseHealthLeech>(HealthLeeches);
	for (UWoTDecreaseHealthLeech* Leech : HealthLeeches)
	{
		if (IsValid(Leech))
		{
			if (AWoTAngrealInvLightning* Source =
				Cast<AWoTAngrealInvLightning>(Leech->SourceAngreal))
			{
				if (Source->bCasting) Sources.Add(Source);
			}
		}
	}
	for (AWoTAngrealInvLightning* Source : Sources)
	{
		if (IsValid(Source)) Source->NotifyPawnEnteredWater(WetPawn);
	}
}

void AWoTAngrealInvLightning::CastArtifact()
{
	if (bCasting)
	{
		return;
	}
	// A fresh cast owns a fresh link set. This is idempotent in the normal
	// UnCast path and prevents an externally interrupted cast from double-starting
	// a victim loop or leaving an old bolt alive.
	RemoveAll();
	Super::CastArtifact();   // ActivateSound; replicated state creates the belt
	bCasting = true;
	ReconcileCastingPresentation();
	LatchTimer = 0.0f;
	WaterPollTimer = 0.0f;
	SetActorTickEnabled(true);
}

void AWoTAngrealInvLightning::UnCastArtifact()
{
	if (!bCasting)
	{
		return;
	}
	RemoveAll();
	bCasting = false;
	ReconcileCastingPresentation();
	Super::UnCastArtifact();   // DeactivateSound
	SetActorTickEnabled(false);
}

void AWoTAngrealInvLightning::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Inventory removal/travel can bypass the input-release UnCast path. Retail's
	// LightningLeech actors died with the artifact/host and took their loops and
	// streaks with them; make that ownership explicit for the folded components.
	RemoveAll();
	bCasting = false;
	ReconcileCastingPresentation();
	Super::EndPlay(EndPlayReason);
}

void AWoTAngrealInvLightning::OnRep_Casting()
{
	ReconcileCastingPresentation();
}

void AWoTAngrealInvLightning::ReconcileCastingPresentation()
{
	if (bCasting)
	{
		if (!IsValid(PlayerEffect) && GetWorld())
		{
			PlayerEffect = WoTMeshFX::SpawnPawnOverlayLocal(*GetWorld(),
				TEXT("LightningSkinEffect"), Cast<APawn>(GetOwner()));
		}
	}
	else if (IsValid(PlayerEffect))
	{
		PlayerEffect->Destroy();
		PlayerEffect = nullptr;
	}
}

void AWoTAngrealInvLightning::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTAngrealInvLightning, bCasting);
}

void AWoTAngrealInvLightning::NotifyOwnerDamaged(APawn* Attacker)
{
	// NotifyDamagedBy: self-inflicted damage drops the aura; a victim who fights
	// back snaps their own link.
	if (!bCasting || !IsValid(Attacker))
	{
		return;
	}
	if (Attacker == GetOwner())
	{
		UnCastArtifact();
	}
	else
	{
		RemoveVictim(Attacker);
	}
}

void AWoTAngrealInvLightning::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bCasting || !HasAuthority())
	{
		return;
	}

	// Suck up charges while casting.
	ChargeTimer += DeltaTime;
	const float ChargeInterval = (RoundsPerMinute > 0.0f) ? 60.0f / RoundsPerMinute : 1.0f;
	if (ChargeTimer >= ChargeInterval)
	{
		ChargeTimer -= ChargeInterval;
		UseCharge();
	}
	if (!bCasting)
	{
		return;   // UseCharge -> GoEmpty -> UnCast can drop us mid-tick
	}

	// Latch onto fresh victims on a cadence.
	LatchTimer -= DeltaTime;
	if (LatchTimer < 0.0f)
	{
		LatchTimer += LatchDelay;
		AttemptLatch();
	}

	UpdateLeeches();

	// Water poll (NotifyInWaterReflector PollingTime=0.5): shock anyone standing in
	// water — the caster or a victim. The original's poll is STATELESS: it re-fires
	// NotifyPawnEnteredWater every 0.5s while the foot is in water, not just on the
	// dry->wet transition. A victim's link is snapped on the first shock (RemoveVictim),
	// so they drop out of Leeches and are naturally one-shot; the caster keeps polling
	// (an unshielded caster UnCasts on the first shock; a shielded one re-zaps the pool).
	WaterPollTimer += DeltaTime;
	if (WaterPollTimer >= 0.5f)
	{
		WaterPollTimer = 0.0f;
		TArray<APawn*> ToNotify;
		if (APawn* Caster = Cast<APawn>(GetOwner()))
		{
			if (IsInWater(Caster)) { ToNotify.Add(Caster); }
		}
		for (const TObjectPtr<UWoTDecreaseHealthLeech>& L : Leeches)
		{
			APawn* V = IsValid(L) ? Cast<APawn>(L->GetOwner()) : nullptr;
			if (V && IsInWater(V)) { ToNotify.Add(V); }
		}
		for (APawn* P : ToNotify)
		{
			NotifyPawnEnteredWater(P);
			if (!bCasting) { return; }   // caster shock can drop the aura
		}
	}
}

bool AWoTAngrealInvLightning::IsAlreadyAffected(APawn* Other) const
{
	for (const TObjectPtr<UWoTDecreaseHealthLeech>& L : Leeches)
	{
		if (IsValid(L) && L->GetOwner() == Other)
		{
			return true;
		}
	}
	return false;
}

bool AWoTAngrealInvLightning::IsAttachable(APawn* Other, APawn* Caster) const
{
	// Frozen victims latch regardless of line of sight (a trace into ice fails).
	if (Other->FindComponentByClass<UWoTIceLeech>())
	{
		return true;
	}
	// AutoTarget on the caster ignores line of sight too.
	if (Caster->FindComponentByClass<UWoTAutoTargetReflector>())
	{
		return true;
	}
	// Otherwise require clear line of sight to the victim's CENTER, TOP, or BOTTOM —
	// any one clear point latches (AngrealInvLightning.uc:395-399). Trace against
	// world AND pawns from the caster's eye: an intervening actor blocks, matching
	// the original's bTraceActors trace whose first hit must BE the victim.
	FVector Start = Caster->GetActorLocation();
	Start.Z += Caster->BaseEyeHeight;
	float Radius = 0.0f, HalfHeight = 0.0f;
	Other->GetSimpleCollisionCylinder(Radius, HalfHeight);
	const FVector Base = Other->GetActorLocation();
	const FVector Ends[3] = { Base,
	                          Base + FVector(0.0f, 0.0f, HalfHeight),
	                          Base - FVector(0.0f, 0.0f, HalfHeight) };
	for (const FVector& End : Ends)
	{
		FHitResult Hit;
		// AngrealInvLightning.uc:396-398 Trace(..., true)==Other. Apply the
		// actor acceptance mask of UE1's ULevel::SingleLineCheck.
		if (WoTRetailActorTrace(GetWorld(), Start, End, Caster, Hit)
			&& Hit.GetActor() == Other)
		{
			return true;
		}
	}
	return false;
}

void AWoTAngrealInvLightning::AttemptLatch()
{
	APawn* Caster = Cast<APawn>(GetOwner());
	if (!Caster)
	{
		return;
	}
	const FVector Origin = Caster->GetActorLocation();
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		APawn* Other = *It;
		if (Other == Caster || !IsValid(Other))
		{
			continue;
		}
		float Radius = 0.0f, HalfHeight = 0.0f;
		Other->GetSimpleCollisionCylinder(Radius, HalfHeight);
		const float Surface = FVector::Dist(Origin, Other->GetActorLocation()) - Radius;
		if (Surface <= AttachmentRange && !IsAlreadyAffected(Other)
			&& IsAttachable(Other, Caster))
		{
			AddVictim(Other);
			break;   // one at a time
		}
	}
}

void AWoTAngrealInvLightning::AddVictim(APawn* Victim)
{
	UWoTDecreaseHealthLeech* L = NewObject<UWoTDecreaseHealthLeech>(Victim);
	L->SetSourceAngreal(this);
	L->DamagePerHit = 1.0f;
	L->AffectResolution = (MaxDamageRate > 0.0f) ? 1.0f / MaxDamageRate : 1.0f;
	L->bDeleterious = true;
	// The folded LightningLeech wrapper carried the victim crackle
	// (LightningLeech.uc: AmbientSound=LoopLS, 64/255) — stamp it on the health
	// leech standing in for it (instance values replicate).
	L->AmbientSoundPath = FName(TEXT("Angreal/LoopLS"));
	L->AmbientSoundRadius = 64;
	L->AmbientSoundVolume = 255;
	L->AttachTo(Victim);
	// AttachTo destroys itself if a shield blocks the effect — only track survivors.
	if (IsValid(L) && L->IsRegistered())
	{
		Leeches.Add(L);
		// The latch bolt (LightningLeech.uc PathStreak): caster -> victim while
		// the latch holds. Replicated; segments lay locally per machine.
		APawn* Caster = Cast<APawn>(GetOwner());
		const FVector Start = Caster
			? Caster->GetActorLocation() + FVector(0.0f, 0.0f, Caster->BaseEyeHeight)
			: GetActorLocation();
		const FTransform SpawnTransform(FRotator::ZeroRotator, Start);
		if (AWoTBeamEffect* Bolt = GetWorld()->SpawnActorDeferred<AWoTBeamEffect>(
			AWoTBeamEffect::StaticClass(), SpawnTransform, Caster, Caster,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
		{
			Bolt->Config = FName(TEXT("LightningStreak02"));
			Bolt->bAlwaysRelevant = true;   // LightningLeech.uc:280-285
			Bolt->Start = Start;
			Bolt->End = Victim->GetActorLocation();
			Bolt->StartActor = Caster;
			Bolt->EndActor = Victim;
			Bolt->FinishSpawning(SpawnTransform);
			if (TObjectPtr<AWoTBeamEffect>* OldBolt = Bolts.Find(Victim))
			{
				if (IsValid(*OldBolt))
				{
					(*OldBolt)->Destroy();
				}
			}
			Bolts.Add(Victim, Bolt);
		}
	}
}

void AWoTAngrealInvLightning::RemoveVictim(APawn* Victim)
{
	for (int32 i = Leeches.Num() - 1; i >= 0; --i)
	{
		UWoTDecreaseHealthLeech* L = Leeches[i];
		if (!IsValid(L) || L->GetOwner() == Victim)
		{
			if (IsValid(L))
			{
				L->Unattach();
			}
			Leeches.RemoveAt(i);
		}
	}
	if (TObjectPtr<AWoTBeamEffect>* Bolt = Bolts.Find(Victim))
	{
		if (IsValid(*Bolt))
		{
			(*Bolt)->Destroy();
		}
		Bolts.Remove(Victim);
	}
}

void AWoTAngrealInvLightning::RemoveAll()
{
	for (auto& Pair : Bolts)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	Bolts.Empty();
	for (UWoTDecreaseHealthLeech* L : Leeches)
	{
		if (IsValid(L))
		{
			L->Unattach();
		}
	}
	Leeches.Empty();
}

void AWoTAngrealInvLightning::UpdateLeeches()
{
	APawn* Caster = Cast<APawn>(GetOwner());
	if (!Caster)
	{
		return;
	}
	// Prune dead/detached leeches first so NumVictims (strain) is accurate. Do
	// not discard the pointer through RemoveAll alone: a still-valid detached
	// component must explicitly stop its victim loop.
	for (int32 Index = Leeches.Num() - 1; Index >= 0; --Index)
	{
		UWoTDecreaseHealthLeech* L = Leeches[Index];
		if (!IsValid(L) || !L->IsRegistered())
		{
			if (IsValid(L))
			{
				L->Unattach();
			}
			Leeches.RemoveAt(Index);
		}
	}
	// A victim death/cleanse can destroy its leech outside RemoveVictim. Tear
	// down the corresponding replicated beam instead of leaving it to stack on
	// the next latch/recast.
	TSet<APawn*> ActiveVictims;
	for (const TObjectPtr<UWoTDecreaseHealthLeech>& L : Leeches)
	{
		if (IsValid(L))
		{
			if (APawn* Victim = Cast<APawn>(L->GetOwner()))
			{
				ActiveVictims.Add(Victim);
			}
		}
	}
	for (auto It = Bolts.CreateIterator(); It; ++It)
	{
		APawn* Victim = It.Key().Get();
		AWoTBeamEffect* Bolt = It.Value().Get();
		if (!IsValid(Victim) || !ActiveVictims.Contains(Victim))
		{
			if (IsValid(Bolt))
			{
				Bolt->Destroy();
			}
			It.RemoveCurrent();
		}
	}
	const int32 NumVictims = Leeches.Num();
	const float Strain = FMath::Pow(PercentStrain, (float)NumVictims);
	const FVector Origin = Caster->GetActorLocation();

	// Copy — RemoveVictim mutates Leeches.
	TArray<TObjectPtr<UWoTDecreaseHealthLeech>> Current = Leeches;
	for (UWoTDecreaseHealthLeech* L : Current)
	{
		APawn* V = IsValid(L) ? Cast<APawn>(L->GetOwner()) : nullptr;
		if (!V)
		{
			continue;
		}
		const float Dist = FVector::Dist(Origin, V->GetActorLocation());
		if (Dist < MaxEffectiveRange)
		{
			const float Scalar = 1.0f - (Dist / MaxEffectiveRange);
			const float DPS = MaxDamageRate * Strain * Scalar;
			L->AffectResolution = (DPS > 0.01f) ? 1.0f / DPS : 100.0f;
		}
		else
		{
			RemoveVictim(V);   // out of range — snap the link
		}
	}
}

void AWoTAngrealInvLightning::DamagePawn(APawn* Victim, float Damage)
{
	if (!IsValid(Victim) || Damage <= 0.0f)
	{
		return;
	}
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = AWoTAngrealInventory::GetDamageType(this);
	Ev.UE1Momentum = FVector::ZeroVector;   // AngrealInvLightning.uc:210,224
	Ev.HitInfo.ImpactPoint = Victim->GetActorLocation();
	APawn* Caster = Cast<APawn>(GetOwner());
	Victim->TakeDamage(Damage, Ev, Caster ? Caster->GetController() : nullptr, this);
}

void AWoTAngrealInvLightning::ElectrocutePawn(APawn* Source)
{
	if (!IsValid(Source))
	{
		return;
	}
	const FVector Origin = Source->GetActorLocation();
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		APawn* P = *It;
		if (P == Source || !IsValid(P) || !IsInWater(P))
		{
			continue;
		}
		const float Dist = FVector::Dist(Origin, P->GetActorLocation());
		if (Dist < WaterDamageRadius)
		{
			DamagePawn(P, MaxWaterDamage * (1.0f - Dist / WaterDamageRadius));
		}
	}
}

void AWoTAngrealInvLightning::NotifyPawnEnteredWater(APawn* WetPawn)
{
	if (!bCasting || !IsValid(WetPawn))
	{
		return;
	}
	APawn* Caster = Cast<APawn>(GetOwner());
	const AWoTCharacter* CasterCh = Cast<AWoTCharacter>(Caster);
	const int32 CasterHP0 = CasterCh ? CasterCh->Health : 0;

	if (WetPawn != Caster)
	{
		RemoveVictim(WetPawn);   // a victim who wades in snaps free (but still shocked)
	}
	DamagePawn(WetPawn, MaxWaterDamage);
	ElectrocutePawn(WetPawn);

	// You can't hold lightning while being electrocuted underwater.
	if (WetPawn == Caster && CasterCh && CasterCh->Health < CasterHP0)
	{
		UnCastArtifact();
	}
}

bool AWoTAngrealInvLightning::IsInWater(const APawn* Pawn)
{
	if (!IsValid(Pawn))
	{
		return false;
	}
	// Retail tests FootRegion — the zone at the cylinder BOTTOM (NotifyInWater
	// Reflector.uc:34, AngrealInvLightning.uc:172): wading is enough to conduct.
	// Probing the actor center missed knee-deep water. (The 0.5s poll's up-to-0.5s
	// entry lag vs retail's instant FootZoneChange event is a recorded
	// approximation — PORT_NOTES.)
	float Radius = 0.0f, HalfHeight = 0.0f;
	Pawn->GetSimpleCollisionCylinder(Radius, HalfHeight);
	const FVector Foot = Pawn->GetActorLocation() - FVector(0.0f, 0.0f, HalfHeight);
	return AWoTZoneVolume::IsPointInWater(Pawn->GetWorld(), Foot);
}
