#include "WoTCreatureBehaviors.h"
#include "../WoTRandom.h"
#include "WoTPathNodeIterator.h"

#include "WoTNPCCharacter.h"
#include "WoTNPCWeapon.h"
#include "WoTNPCController.h"
#include "WoTBrainComponent.h"
#include "WoTSight.h"
#include "WoTWeaponComponent.h"
#include "../WoTAnimTable.h"
#include "../WoTCharacter.h"
#include "../WoTUE1Collision.h"
#include "../WoTPlayerController.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTZoneVolume.h"
#include "../Inventory/WoTShieldReturnToSenderReflector.h"
#include "../Inventory/WoTDecreaseHealthLeech.h"
#include "../Inventory/WoTReflector.h"
#include "../Inventory/WoTLeech.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTMyrddraalSwordAngreal.h"
#include "WoTPathNode.h"
#include "WoTLegionStompVolcano.h"
#include "../Projectiles/WoTSeekingProjectile.h"
#include "../Projectiles/WoTArcherArrow.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "Components/CapsuleComponent.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "EngineUtils.h"

// ============================================================ base

UWoTCreatureBehavior* UWoTCreatureBehavior::Create(AWoTNPCCharacter& NPC, FName BehaviorName)
{
	// Map the row's BehaviorClass name to a concrete hook. Names without a ported hook
	// (MashadarTendrilFocus) fall through to null — those rows keep their marker for a
	// later batch.
	UClass* Cls = nullptr;
	if (BehaviorName == FName(TEXT("ShieldDeflect")))
	{
		Cls = UWoTShieldDeflectBehavior::StaticClass();
	}
	else if (BehaviorName == FName(TEXT("WarderFadeCloak")))
	{
		Cls = UWoTWarderFadeCloakBehavior::StaticClass();
	}
	else if (BehaviorName == FName(TEXT("MinionStealth")))
	{
		Cls = UWoTMinionStealthBehavior::StaticClass();
	}
	else if (BehaviorName == FName(TEXT("MyrddraalTeleport")))
	{
		Cls = UWoTMyrddraalTeleportBehavior::StaticClass();
	}
	else if (BehaviorName == FName(TEXT("BATrollocRecovery")))
	{
		Cls = UWoTBATrollocRecoveryBehavior::StaticClass();
	}
	else if (BehaviorName == FName(TEXT("LegionSpiritsStomp")))
	{
		Cls = UWoTLegionSpiritsStompBehavior::StaticClass();
	}
	else if (BehaviorName == FName(TEXT("ArcherBow")))
	{
		Cls = UWoTArcherBowBehavior::StaticClass();
	}
	if (!Cls)
	{
		return nullptr;
	}

	UWoTCreatureBehavior* Behavior = NewObject<UWoTCreatureBehavior>(&NPC, Cls);
	Behavior->RegisterComponent();
	return Behavior;
}

void UWoTCreatureBehavior::OnPossessedBy(AWoTNPCController* Controller)
{
	OwningController = Controller;
}

AWoTNPCCharacter* UWoTCreatureBehavior::GetNPC() const
{
	return Cast<AWoTNPCCharacter>(GetOwner());
}

AWoTNPCController* UWoTCreatureBehavior::GetController() const
{
	return OwningController.Get();
}

UWoTBrainComponent* UWoTCreatureBehavior::GetBrain() const
{
	const AWoTNPCCharacter* NPC = GetNPC();
	return NPC ? NPC->Brain : nullptr;
}

// ============================================================ defensive scan

AWoTProjectile* WoTDefensiveScan::FindIncomingOffender(const APawn* OwnerPawn,
	float CollectionRadius, float FovCos, float MinTime, float MaxTime,
	bool bRejectWeaponProjectiles, bool bRejectSeekers)
{
	// DefensiveDetector CheckForOffending folded to a scan (DefensiveDetector.uc +
	// LegendPawn.uc:1349-1362): the nearest AWoTProjectile inside the CollectionRadius
	// sphere, in the FOV cone, not an explicitly-rejected class, moving toward the pawn
	// with an arrival time in the response window.
	const UWorld* World = OwnerPawn ? OwnerPawn->GetWorld() : nullptr;
	if (!OwnerPawn || !World)
	{
		return nullptr;
	}
	const FVector Origin = OwnerPawn->GetActorLocation();
	const FVector Forward = OwnerPawn->GetActorForwardVector();
	const float RadiusSq = FMath::Square(CollectionRadius);

	AWoTProjectile* Best = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();
	for (TActorIterator<AWoTProjectile> It(World); It; ++It)
	{
		AWoTProjectile* Proj = *It;
		if (!IsValid(Proj))
		{
			continue;
		}
		// ExplicitClassRejections: WotWeaponProjectile leaves + seekers are un-deflectable
		// (shield only). The dodge chain rejects neither — it collects the whole family.
		if (bRejectWeaponProjectiles && Proj->IsA<AWoTWeaponProjectile>())
		{
			continue;
		}
		if (bRejectSeekers && Proj->IsA<AWoTSeekingProjectile>())
		{
			continue;
		}
		const FVector Delta = Proj->GetActorLocation() - Origin;
		const float DistSq = Delta.SizeSquared();
		if (DistSq > RadiusSq)
		{
			continue;
		}
		// FOV cone (the offender within the pawn's peripheral vision).
		if (FVector::DotProduct(Delta.GetSafeNormal(), Forward) < FovCos)
		{
			continue;
		}
		// offender.Velocity: read the movement component (current the frame it is set;
		// the root ComponentVelocity lags a tick).
		FVector Velocity = Proj->GetVelocity();
		if (Proj->ProjectileMovement)
		{
			Velocity = Proj->ProjectileMovement->Velocity;
		}
		const float Speed = Velocity.Size();
		if (Speed <= 0.0f)
		{
			continue;   // not moving → uncomputable arrival, not incoming
		}
		// Moving toward us (native detector rule: a unit step must close distance).
		const float DistNow = FMath::Sqrt(DistSq);
		if (FVector::Dist(Proj->GetActorLocation() + Velocity.GetSafeNormal(), Origin) >= DistNow)
		{
			continue;
		}
		// Arrival time inside the response window [Min,Max].
		const float Arrival = DistNow / Speed;
		if (Arrival < MinTime || Arrival > MaxTime)
		{
			continue;
		}
		if (DistSq < BestDistSq)
		{
			Best = Proj;
			BestDistSq = DistSq;
		}
	}
	return Best;
}

// ============================================================ ShieldDeflect

AWoTProjectile* UWoTShieldDeflectBehavior::DetectIncoming() const
{
	// The active DefensiveDetectorSoldier/Questioner poll: the nearest deflectable
	// incoming projectile in the perimeter, arriving within [ResponseMinTime,MaxTime].
	// bRejectSeekers is the per-creature seeker axis (Soldier rejects, Questioner keeps).
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	float FovCos = 0.0f;
	if (const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(OwnerPawn))
	{
		FovCos = NPC->PeripheralVision;
	}
	return WoTDefensiveScan::FindIncomingOffender(OwnerPawn, CollectionRadius, FovCos,
		ResponseMinTime, ResponseMaxTime, /*bRejectWeaponProjectiles*/ true, bRejectSeekers);
}

AWoTNPCWeapon* UWoTShieldDeflectBehavior::GetShieldWeapon() const
{
	// The shield lives in the melee slot for both creatures (Soldier/Questioner rows:
	// MeleeWeaponType = SoldierShield/QuestionerShield). Retail's SelectedItem is that
	// same inventory proxy (ShieldReflectorInventoryProxyClass, ShieldUser.uc:19).
	const AWoTNPCCharacter* NPC = GetNPC();
	return NPC ? NPC->GetMeleeWeapon() : nullptr;
}

bool UWoTShieldDeflectBehavior::HasShieldAvailable() const
{
	// UseShield's ShieldReflectorInventoryProxy gate (uc:143,173): the shield must be
	// in inventory. The Questioner's throwable is absent while its projectile flies
	// (WotThrowableWeapon.uc:21-24 -> IsInFlight), and a battered shield is destroyed.
	const AWoTNPCWeapon* Shield = GetShieldWeapon();
	return IsValid(Shield) && !Shield->IsInFlight();
}

bool UWoTShieldDeflectBehavior::WantsShield() const
{
	// PendingOffenderCollision → UseShield (ShieldUser.uc:137-145): raise the shield
	// only when the active detector reports a deflectable incoming projectile. This is
	// the ONLY UseShield caller in the shipped code, so the shield is purely reactive —
	// NOT raised merely from seeing an enemy (the earlier FSM proxy). A dying pawn never
	// shields, and a pawn without its shield (thrown/destroyed) dodges instead (item 4).
	const AWoTNPCController* Controller = GetController();
	if (!Controller || Controller->GetBrainState() == EWoTBrainState::Dying)
	{
		return false;
	}
	if (!HasShieldAvailable())
	{
		return false;
	}
	return DetectIncoming() != nullptr;
}

bool UWoTShieldDeflectBehavior::InAttackState() const
{
	const AWoTNPCController* Controller = GetController();
	if (!Controller)
	{
		return false;
	}
	switch (Controller->GetBrainState())
	{
	case EWoTBrainState::AttemptAttack:
	case EWoTBrainState::PerformMelee:
	case EWoTBrainState::PerformMeleeWeapon:
		return true;
	default:
		return false;
	}
}

void UWoTShieldDeflectBehavior::OnPossessedBy(AWoTNPCController* Controller)
{
	Super::OnPossessedBy(Controller);
	// DeleteInventory (uc:65-74): the shield weapon disappearing forces DisableDeflect.
	// Bind its destroyed delegate now — the melee slot is materialized in OnPossess
	// (InitializeWeaponSlots) before the behavior pump, so it exists here.
	if (AWoTNPCWeapon* Shield = GetShieldWeapon())
	{
		BoundShieldWeapon = Shield;
		Shield->OnWeaponDestroyed.AddUObject(
			this, &UWoTShieldDeflectBehavior::OnShieldWeaponDestroyed);
	}
}

void UWoTShieldDeflectBehavior::OnShieldWeaponDestroyed(AWoTNPCWeapon* /*Weapon*/)
{
	// The shield battered to 0 Health while deflecting: force the drop immediately
	// (DeleteInventory -> DisableDeflect, uc:65-74). No 0.5s deferral.
	BoundShieldWeapon = nullptr;
	DisableDeflect();
}

void UWoTShieldDeflectBehavior::EnableDeflect()
{
	// UseShield -> EnableDeflect (uc:168/91): install the shield reflector so incoming
	// weaves deflect (ReflectsProjectiles), select the shield weapon, crouch, and hold.
	AWoTNPCCharacter* NPC = GetNPC();
	if (!NPC || bDeflectEnabled)
	{
		return;
	}
	UWoTShieldReturnToSenderReflector* Reflector =
		NewObject<UWoTShieldReturnToSenderReflector>(NPC);
	// ShieldUser.PostBeginPlay copies the creature's config into the reflector
	// (ShieldUser.uc:40-46): the Soldier authors IrreleventProjectileType=
	// 'SeekingProjectile' (Soldier.uc:49) so seekers pass through un-deflected;
	// bShieldReflectorDeflect drives the deflect-away math; ShieldReflectorSoundName
	// overrides the reflect sound. The Questioner leads and keeps the base sound.
	Reflector->IrreleventProjectileType =
		bRejectSeekers ? FName(TEXT("SeekingProjectile")) : NAME_None;
	Reflector->bDeflect = bReflectorDeflect;
	Reflector->bLeadWhenReflecting = bReflectorLeadWhenReflecting;
	Reflector->ReflectSoundOverride = ReflectSoundOverride;
	Reflector->Install(NPC);
	ShieldReflector = Reflector;
	bDeflectEnabled = true;
	bUninstallPending = false;

	// UseShield's SelectedItem = shield proxy (uc:175): make the held weapon the shield
	// (item 3) — item 2's damage routing keys off the held shield. It is already the
	// default weapon for both, so this is a re-assert (and re-equips after a throw).
	NPC->EquipMeleeWeapon();
	// TweenPlaySlotAnim(CrouchAnimSlot) (uc:179) — the crouch-down tween (item 6).
	NPC->StampSlotAnim(FName(TEXT("Crouch")));
	if (AWoTNPCController* Controller = GetController())
	{
		Controller->StopMovement();             // InterruptMovement (uc:181)
		Controller->SetForcedStationary(true);  // ForceStationary (uc:96)
	}
}

void UWoTShieldDeflectBehavior::DisableDeflect()
{
	// DisableDeflect (uc:110-118): uninstall the reflector, clear the deflect state,
	// and release the stationary hold (UnforceStationary, uc:116) — on drop AND on
	// unpossess, so the gate never leaks.
	if (ShieldReflector.IsValid())
	{
		ShieldReflector->Uninstall();
	}
	ShieldReflector = nullptr;
	bDeflectEnabled = false;
	bUninstallPending = false;
	if (AWoTNPCController* Controller = GetController())
	{
		Controller->SetForcedStationary(false);
	}
}

void UWoTShieldDeflectBehavior::TickBehavior(float DeltaSeconds)
{
	// The shield left the hand (thrown-in-flight, or destroyed between ticks) mid-
	// deflect: force the drop at once (DeleteInventory + Tick's uninstall, uc:56-74) —
	// no 0.5s deferral, and the pawn falls back to the dodge path next Defensive pulse.
	if (bDeflectEnabled && !HasShieldAvailable())
	{
		DisableDeflect();
		return;
	}
	if (bDeflectEnabled)
	{
		if (AWoTNPCCharacter* NPC = GetNPC();
			NPC && NPC->GetReplicatedAnimName() != TEXT("Crouching"))
		{
			NPC->LoopSlotAnim(TEXT("Crouching"));
		}
	}
	if (WantsShield())
	{
		bUninstallPending = false;
		if (!bDeflectEnabled)
		{
			EnableDeflect();
		}
		return;
	}
	if (!bDeflectEnabled)
	{
		return;
	}
	// Shield no longer wanted. Committing to an attack drops it at once (the pawn is
	// striking, not defending); a merely-cleared perimeter drops it after the
	// UnistallShieldReflectorDelay (DontUseShield's deferred DisableDeflect, uc:155-163
	// + Tick uc:56-60).
	if (InAttackState())
	{
		DisableDeflect();
		return;
	}
	if (!bUninstallPending)
	{
		bUninstallPending = true;
		UninstallTimer = UninstallShieldReflectorDelay;
		// DontUseShield (uc:155-163): stand back up (Walk tween) the moment the
		// perimeter clears, then uninstall after the delay.
		if (AWoTNPCCharacter* NPC = GetNPC())
		{
			NPC->TweenSlotAnim(FName(TEXT("Walk")));
		}
	}
	UninstallTimer -= DeltaSeconds;
	if (UninstallTimer <= 0.0f)
	{
		DisableDeflect();
	}
}

void UWoTShieldDeflectBehavior::OnUnpossessed()
{
	// CancelAngrealEffects (uc:78): drop the shield when the brain lets go of the pawn.
	if (BoundShieldWeapon.IsValid())
	{
		BoundShieldWeapon->OnWeaponDestroyed.RemoveAll(this);
	}
	BoundShieldWeapon = nullptr;
	DisableDeflect();
}

namespace
{
	constexpr int32 WoTWarderFadeAnimSkinCount = 12;
	constexpr float WoTWarderCloakVolumeReductionPct = 0.10f;
}

// ============================================================ WarderFadeCloak

void UWoTWarderFadeCloakBehavior::OnPossessedBy(AWoTNPCController* Controller)
{
	// A behavior can be re-used across possession changes. Never let a prior
	// WaitingIdle window become the next possession's authored baseline.
	RestoreWaitingSoundVolume();
	// And never let it look ALREADY-ENTERED: with a stale LastBrainState, a
	// repossessed pawn whose new controller is also Waiting would skip
	// StartWaitingIdleAnim and run the old timer un-ducked (review-caught).
	LastBrainState = 255;
	Super::OnPossessedBy(Controller);
	// Capture the fully-visible byte (default.Visibility) and start fully visible
	// (PreBeginPlay: FadeLevel = FadeLevels, uc:108).
	if (const AWoTNPCCharacter* NPC = GetNPC())
	{
		DefaultVisibility = static_cast<float>(NPC->Visibility);
	}
	FadeLevel = FadeLevels;
	CurrentFadeInChangeTime = FadeInChangeTime;
	CurrentFadeOutChangeTime = FadeOutChangeTime;
}

void UWoTWarderFadeCloakBehavior::OnUnpossessed()
{
	RestoreWaitingSoundVolume();
	Super::OnUnpossessed();
}

void UWoTWarderFadeCloakBehavior::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RestoreWaitingSoundVolume();
	Super::EndPlay(EndPlayReason);
}

void UWoTWarderFadeCloakBehavior::SetFadeLevel(float NewFadeLevel)
{
	// SetFadeLevel (uc:182-193): clamp, then link Visibility and the authored skin.
	FadeLevel = FMath::Clamp(NewFadeLevel, 0.0f, FadeLevels);
	AWoTNPCCharacter* NPC = GetNPC();
	if (!NPC)
	{
		return;
	}
	const float Vis = FadeLevel / FadeLevels * (DefaultVisibility - MinVisibility) + MinVisibility;
	NPC->Visibility = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Vis), 0, 255));
	// Warder.uc:120-132,186-190: the held sword mirrors the cloak through
	// translucent style + ScaleGlow (the camouflage textures remain body-only).
	if (AWoTNPCWeapon* Weapon = NPC->GetHeldWeapon())
	{
		Weapon->ScaleGlow = FadeLevel / FadeLevels * (MaxScaleGlow - MinScaleGlow) + MinScaleGlow;
		Weapon->bStyleTranslucent = FadeLevel < FadeLevels;
		Weapon->ApplyFadePresentation();
	}

	uint8 EffectiveSkinIndex = AWoTNPCCharacter::VisibleFadeSkinIndex;
	if (FadeLevel < FadeLevels)
	{
		const int32 RetailSkinIndex = FMath::Min(
			static_cast<int32>(FadeLevel / FadeLevels * WoTWarderFadeAnimSkinCount),
			WoTWarderFadeAnimSkinCount - 1);
		EffectiveSkinIndex = static_cast<uint8>(RetailSkinIndex);
	}
	// Retail guards SetFadeSkins behind a SkinIndex change; keep the same boundary so
	// repeated ticks within one authored band do no material or replication work.
	if (NPC->GetFadeSkinIndex() != EffectiveSkinIndex)
	{
		NPC->SetFadeSkinIndex(EffectiveSkinIndex);
	}
}

void UWoTWarderFadeCloakBehavior::BeginFadeIn(bool bSlow)
{
	// WaitingIdle.DisableFade restores default FadeInChangeTime; the Look/RubNeck
	// IncreaseVisibilitySlow notifier substitutes the 10s rate (Warder.uc:374-399).
	CurrentFadeInChangeTime = bSlow ? FadeInSlowChangeTime : FadeInChangeTime;
	bFadeOut = false;
	bFadeNotifierEnabled = true;
}

void UWoTWarderFadeCloakBehavior::BeginFadeOut(bool bSlow)
{
	// WaitingIdle.EnableFade restores default FadeOutChangeTime; the matching idle
	// notifier can substitute FadeOutSlowChangeTime (Warder.uc:380-405).
	CurrentFadeOutChangeTime = bSlow ? FadeOutSlowChangeTime : FadeOutChangeTime;
	bFadeOut = true;
	bFadeNotifierEnabled = true;
}

void UWoTWarderFadeCloakBehavior::IncreaseVisibilitySlow()
{
	BeginFadeIn(true);
}

void UWoTWarderFadeCloakBehavior::DecreaseVisibilitySlow()
{
	BeginFadeOut(true);
}

void UWoTWarderFadeCloakBehavior::RestoreWaitingSoundVolume()
{
	if (!bWaitingSoundDucked)
	{
		return;
	}
	if (AWoTNPCCharacter* NPC = GetNPC())
	{
		NPC->SoundVolumeMultiplier = SavedWaitingSoundVolumeMultiplier;
	}
	bWaitingSoundDucked = false;
}

void UWoTWarderFadeCloakBehavior::StartWaitingIdleAnim()
{
	// FinishAnim restores before the loop re-enters. Doing the same at the top makes
	// direct/re-entrant starts idempotent and preserves the original authored value.
	RestoreWaitingSoundVolume();
	AWoTNPCCharacter* NPC = GetNPC();
	UWoTBrainComponent* Brain = GetBrain();
	if (!NPC || !Brain)
	{
		return;
	}
	const bool bThreatWaiting = Brain->IsGoalValid(EWoTGoalIndex::Threat);
	const FWoTAnimTableEntry* Pick = NPC->StampSlotAnim(
		bThreatWaiting ? FName(TEXT("WaitAcquired")) : FName(TEXT("Wait")));
	WaitingAnimElapsed = 0.0f;
	WaitingAnimDuration = 0.25f;
	WaitingFadeInNotifyTime = -1.0f;
	WaitingFadeOutNotifyTime = -1.0f;
	if (!Pick)
	{
		return;
	}
	if (bThreatWaiting)
	{
		const AWoTNPCCharacter* ClassDefaults =
			NPC->GetClass()->GetDefaultObject<AWoTNPCCharacter>();
		if (ClassDefaults)
		{
			SavedWaitingSoundVolumeMultiplier = NPC->SoundVolumeMultiplier;
			NPC->SoundVolumeMultiplier = WoTWarderCloakVolumeReductionPct *
				ClassDefaults->SoundVolumeMultiplier;
			bWaitingSoundDucked = true;
		}
	}
	const float TweenTime = WoTAnimTable::ResolveTweenTime(Pick);
	const float SequenceTime = NPC->SequenceDurationSeconds(
		Pick->AnimName, WoTAnimTable::ResolveRate(Pick));
	WaitingAnimDuration = TweenTime + FMath::Max(SequenceTime, 0.01f);
	// IncreaseVisibilitySlow / DecreaseVisibilitySlow exist only in WaitingIdle;
	// the authored Look/RubNeck notifies are 0.05 and 0.94 respectively
	// (Warder.uc:21-24,382-393). Only the ordinary Wait slot can pick those
	// inactivity sequences; the threat-held WaitAcquired slot is Breath.
	if (!bThreatWaiting)
	{
		if (const UWoTVertexAnimComponent* Anim =
			Cast<UWoTVertexAnimComponent>(NPC->GetMesh()))
		{
			const float InFraction = Anim->FindNotifyFraction(
				Pick->AnimName, FName(TEXT("IncreaseVisibilitySlow")));
			const float OutFraction = Anim->FindNotifyFraction(
				Pick->AnimName, FName(TEXT("DecreaseVisibilitySlow")));
			if (InFraction >= 0.0f) { WaitingFadeInNotifyTime = TweenTime + InFraction * SequenceTime; }
			if (OutFraction >= 0.0f) { WaitingFadeOutNotifyTime = TweenTime + OutFraction * SequenceTime; }
		}
	}
}

void UWoTWarderFadeCloakBehavior::TickWaitingIdleAnim(float DeltaSeconds)
{
	const float Previous = WaitingAnimElapsed;
	WaitingAnimElapsed += DeltaSeconds;
	if (WaitingFadeInNotifyTime >= 0.0f && Previous < WaitingFadeInNotifyTime &&
		WaitingAnimElapsed >= WaitingFadeInNotifyTime)
	{
		IncreaseVisibilitySlow();
	}
	if (WaitingFadeOutNotifyTime >= 0.0f && Previous < WaitingFadeOutNotifyTime &&
		WaitingAnimElapsed >= WaitingFadeOutNotifyTime)
	{
		DecreaseVisibilitySlow();
	}
	if (WaitingAnimElapsed >= WaitingAnimDuration)
	{
		// FinishAnim ends this retail duck window before the loop chooses its next pick.
		RestoreWaitingSoundVolume();
		StartWaitingIdleAnim();
	}
}

void UWoTWarderFadeCloakBehavior::TickBehavior(float DeltaSeconds)
{
	const AWoTNPCController* Controller = GetController();
	AWoTNPCCharacter* NPC = GetNPC();
	if (!Controller || !NPC)
	{
		RestoreWaitingSoundVolume();
		return;
	}
	const EWoTBrainState State = Controller->GetBrainState();
	if (NPC->Health <= 0 || State == EWoTBrainState::Dying)
	{
		RestoreWaitingSoundVolume();
		// Died() restores body and weapon style before the corpse path
		// (Warder.uc:113-160).
		bFadeNotifierEnabled = false;
		SetFadeLevel(FadeLevels);
		LastBrainState = static_cast<uint8>(State);
		return;
	}
	if (LastBrainState != static_cast<uint8>(State))
	{
		if (State == EWoTBrainState::Waiting)
		{
			bLastWaitingCower = Controller->IsWaitingCowering();
			if (bLastWaitingCower)
			{
				RestoreWaitingSoundVolume();
				BeginFadeIn(false); // WaitingCower.DisableFade
			}
			else
			{
				BeginFadeOut(false); // WaitingIdle.EnableFade (uc:398-405)
				StartWaitingIdleAnim();
			}
		}
		else if (LastBrainState == static_cast<uint8>(EWoTBrainState::Waiting))
		{
			RestoreWaitingSoundVolume();
			BeginFadeIn(false); // WaitingIdle.EndState -> DisableFade (uc:318-324)
		}
		LastBrainState = static_cast<uint8>(State);
	}
	if (State == EWoTBrainState::Waiting)
	{
		const bool bCower = Controller->IsWaitingCowering();
		if (bCower != bLastWaitingCower)
		{
			RestoreWaitingSoundVolume();
			if (bCower)
			{
				BeginFadeIn(false); // Warder WaitingCower disables camouflage.
			}
			else
			{
				BeginFadeOut(false);
				StartWaitingIdleAnim();
			}
			bLastWaitingCower = bCower;
		}
		if (!bCower)
		{
			TickWaitingIdleAnim(DeltaSeconds);
		}
	}
	if (!bFadeNotifierEnabled)
	{
		return;
	}
	const float ChangeTime = bFadeOut ? CurrentFadeOutChangeTime : CurrentFadeInChangeTime;
	if (ChangeTime <= KINDA_SMALL_NUMBER)
	{
		return;
	}
	const float DeltaLevel = FadeLevels * DeltaSeconds / ChangeTime;
	const float NewLevel = FadeLevel + (bFadeOut ? -DeltaLevel : DeltaLevel);
	SetFadeLevel(NewLevel);
	if ((bFadeOut && FadeLevel <= 0.0f) || (!bFadeOut && FadeLevel >= FadeLevels))
	{
		bFadeNotifierEnabled = false; // DNI_Misc2.DisableNotifier at either endpoint
	}
}

float UWoTWarderFadeCloakBehavior::GetMeleeDamageMultiplier()
{
	// GetStrengthMultiplier (uc:230-243): 2x when conscripted by a guarding Sister.
	// The shipped check is a LIVE FindLeader() per swing — side effects included
	// (Warder.uc:230 → Grunt.uc:1143 re-scans the leadership radius and copies or
	// invalidates the guard goal) — so leadership changes between the slice-4
	// InvokeWotPawnFindLeader pings are reflected on the very next swing.
	AWoTNPCController* Controller = GetController();
	if (Controller && Controller->FindLeader())
	{
		return ConscriptedStrengthMultiplier;
	}
	return 1.0f;
}

// ============================================================ MinionStealth

bool UWoTMinionStealthBehavior::IsWatchedByThreat() const
{
	// Minion.GetPriorityGoalIndex watch test (uc:171-174): the current Threat goal is
	// an enemy Pawn, and PawnCanSeeActor(threat, self, HoldingAngleCos, true) — the
	// threat's script FOV test sees the Minion. Keep this on the separate
	// PawnCanSeeActor path rather than native APawn::LineOfSightTo.
	AWoTNPCCharacter* NPC = GetNPC();
	UWoTBrainComponent* Brain = GetBrain();
	if (!NPC || !Brain || !Brain->IsGoalValid(EWoTGoalIndex::Threat))
	{
		return false;
	}
	const FWoTGoal& Threat = Brain->GetGoal(EWoTGoalIndex::Threat);
	if (!Threat.HasActor())
	{
		return false;
	}
	APawn* Watcher = Cast<APawn>(Threat.GoalActor.Get());
	if (!Watcher)
	{
		return false;
	}
	return WoTSight::PawnCanSeeActor(Watcher, NPC, HoldingAngleCos, true);
}

void UWoTMinionStealthBehavior::SetFrozen(bool bNewFrozen)
{
	if (bNewFrozen == bFrozen)
	{
		return;
	}
	bFrozen = bNewFrozen;
	if (AWoTNPCController* Controller = GetController())
	{
		// ForceStationary / UnForceStationary (uc:176/180): hold position + withhold
		// the attack while watched.
		Controller->SetForcedStationary(bFrozen);
	}
}

void UWoTMinionStealthBehavior::TickBehavior(float /*DeltaSeconds*/)
{
	const AWoTNPCController* Controller = GetController();
	const AWoTNPCCharacter* NPC = GetNPC();
	if (!Controller || !NPC)
	{
		return;
	}
	const UWoTBrainComponent* Brain = GetBrain();
	const float Now = GetWorld()->GetTimeSeconds();

	// The stare latch (Minion.uc:148-161): armed once when the first Threat goal
	// appears (threshold = now + MaxStareTime, set only while threshold == 0,
	// never re-armed), and stealth shuts off for good when the window passes.
	// Taking damage also clears it (:188/:196 — the defensive-dodge clear rides
	// the same flag; dodges are unported). At the shipped MaxStareTime (1e-6)
	// the freeze therefore lives for at most one frame — see the header note.
	if (LastSeenHealth == -1)
	{
		LastSeenHealth = NPC->Health;
	}
	if (NPC->Health < LastSeenHealth)
	{
		bStealthAttacks = false;
	}
	LastSeenHealth = NPC->Health;
	if (bStealthAttacks && Brain)
	{
		if (StareTimeThreshold == 0.0f && Brain->IsGoalValid(EWoTGoalIndex::Threat))
		{
			StareTimeThreshold = Now + MaxStareTime;
		}
		if (StareTimeThreshold != 0.0f && Now > StareTimeThreshold)
		{
			bStealthAttacks = false;
		}
	}

	// A dying Minion never freezes (its threat callbacks no-op in Dying); the
	// freeze only exists while the stealth latch is armed (uc:171).
	const bool bWatched = bStealthAttacks &&
		Controller->GetBrainState() != EWoTBrainState::Dying && IsWatchedByThreat();
	SetFrozen(bWatched);
}

void UWoTMinionStealthBehavior::OnUnpossessed()
{
	SetFrozen(false);
}

void UWoTMinionStealthBehavior::ApplyGrabDrawIn(AActor* Victim, float GrabDistance)
{
	// TentacleGrabAttackDamage1 (Minion.uc:417-441): reel the victim toward the Minion.
	// Only a PLAYER pawn is drawn in (Minion.uc:429); an NPC victim keeps the grab damage
	// but is not moved (parity-locked, PORT_NOTES).
	AWoTCharacter* Player = Cast<AWoTCharacter>(Victim);
	AWoTNPCCharacter* Self = GetNPC();
	if (!Player || !Self || Player->IsNPCPawn() || !Self->HasAuthority())
	{
		return;
	}
	UCharacterMovementComponent* Move = Player->GetCharacterMovement();
	const float Mass = Move ? FMath::Max(Move->Mass, 1.0f) : 100.0f;
	const FVector Dir =
		(Self->GetActorLocation() - Player->GetActorLocation()).GetSafeNormal();
	// DrawInVelocity * Normal(MinionLoc - VictimLoc) / VictimMass * GrabDistance /
	// GrabDistanceDivisor — toward the Minion, scaled by how far the reach stretched.
	const FVector Delta = DrawInVelocity * Dir / Mass * GrabDistance / GrabDistanceDivisor;
	if (Move)
	{
		// PlayerPawnGoal.Velocity/Acceleration = 0 before the impulse (Minion.uc:433-434):
		// AddUE1Velocity adds, so zeroing first makes the result exactly the draw-in
		// delta. StopMovementImmediately zeroes Velocity and clears pending input-derived
		// acceleration (the member itself is protected — rebuilt from input each tick).
		Move->StopMovementImmediately();
	}
	Player->RecordAuthorityCreatureImpulseForTest(Delta);
	Player->AddUE1Velocity(Delta);
	// Simulation velocity remains authority-only; CharacterMovement correction
	// carries it to the owning copy. The view shake is presentation-only.
	if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Player->GetController()))
	{
		PC->ClientShakeView(0.2f, 2000.0f, -10.0f);
	}
}

void UWoTMinionStealthBehavior::ReleaseGrabVictim(AActor* Victim)
{
	// TentacleGrabAttackDamage2 (Minion.uc:443-464): the @0.50 notify. It deals NO damage
	// (the "nail him again" comment is aspirational, dead) and restores only a PLAYER
	// that has reached walking/swimming, matching the two shipped Physics branches.
	AWoTCharacter* Player = Cast<AWoTCharacter>(Victim);
	const UCharacterMovementComponent* Move = Player
		? Player->GetCharacterMovement() : nullptr;
	if (Player && Player->HasAuthority() && !Player->IsNPCPawn() && Move
		&& (Move->IsMovingOnGround() || Move->IsSwimming()))
	{
		Player->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	}
}

// ============================================================ MyrddraalTeleport

void UWoTMyrddraalTeleportBehavior::OnPossessedBy(AWoTNPCController* Controller)
{
	Super::OnPossessedBy(Controller);
	LastTeleportPosition = FVector(FLT_MAX);
	bHasLastTeleportPosition = false;
	bHasPendingDestination = false;
	TeleportCount = 0;

	// PreBeginPlay (uc:83): spawn the hidden Spirit-element source angreal once, as a
	// bare SourceAngreal carried by no one (the CreateProxy shape). Stamped onto the
	// melee leech (uc:573) so the drain carries the sword's element identity.
	if (AWoTNPCCharacter* Self = GetNPC(); Self && !SourceAngreal.IsValid())
	{
		SourceAngreal = Cast<AWoTMyrddraalSwordAngreal>(AWoTAngrealInventory::CreateProxy(
			Self, AWoTMyrddraalSwordAngreal::StaticClass()));
	}
}

void UWoTMyrddraalTeleportBehavior::OnUnpossessed()
{
	// The sword-drain is a component on the VICTIM, not us — leave it to run out its
	// own lifespan (the original never yanks it when the Myrddraal lets go).
	CurrentLeech = nullptr;
	// The source angreal (uc:89-95 Destroyed): tear it down with the pawn.
	if (SourceAngreal.IsValid())
	{
		SourceAngreal->Destroy();
	}
	SourceAngreal = nullptr;
}

void UWoTMyrddraalTeleportBehavior::TickBehavior(float /*DeltaSeconds*/)
{
	// Trigger 2 — the PASSIVE defensive perimeter (DefensivePerimiterCompromisedBy
	// Offender, uc:121-129): a SeekingProjectile whose Destination is this pawn that
	// has breached the perimeter proxy (1.5x the collision cylinder, DefensiveDetector
	// .uc:278). Folded to a poll of the seeker set gated on the perimeter distance —
	// the ranged case is the active detector (trigger 3, controller Defensive pulse).
	// A dying Myrddraal no-ops its perception callbacks (Dying), and an episode already
	// underway is never re-triggered.
	AWoTNPCController* Controller = GetController();
	AWoTNPCCharacter* Self = GetNPC();
	if (!Self || !Controller || Controller->GetBrainState() == EWoTBrainState::Dying ||
	    Controller->IsTeleportEpisodeActive())
	{
		return;
	}
	float SelfRadius = 0.0f;
	float SelfHalfHeight = 0.0f;
	Self->GetSimpleCollisionCylinder(SelfRadius, SelfHalfHeight);
	for (TActorIterator<AWoTSeekingProjectile> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It) && It->Destination == Self &&
			WoTUE1Collision::CylindersOverlap(Self->GetActorLocation(),
				SelfRadius * DefensivePerimeterRadiusFactor,
				SelfHalfHeight * DefensivePerimeterRadiusFactor, *It))
		{
			Controller->BeginTeleportEpisode();   // PerformTeleportation (uc:127)
			return;
		}
	}
}

void UWoTMyrddraalTeleportBehavior::OnMeleeHit(AActor* Victim)
{
	// SetupDelayedDamageToGoal (uc:554-578): PerformTeleportation FIRST (uc:557 order),
	// then drop a decaying health-drain leech on the pawn we just struck (replacing any
	// live one). The leech lands AT THE HIT, before the pawn walks off to blink.
	if (AWoTNPCController* Controller = GetController())
	{
		Controller->BeginTeleportEpisode();
	}

	APawn* PawnVictim = Cast<APawn>(Victim);
	if (!PawnVictim)
	{
		return;
	}
	if (CurrentLeech.IsValid())
	{
		// Remove the prior delayed-damage leech before installing its replacement
		// (Myrddraal.uc:562-564).
		CurrentLeech->Unattach();
		CurrentLeech = nullptr;
	}
	UWoTDecreaseHealthLeech* Leech = NewObject<UWoTDecreaseHealthLeech>(PawnVictim);
	Leech->AffectResolution = DelayedDamageRate;         // uc:568
	Leech->AffectResolutionScale = DelayedDamageRateScale; // uc:571
	Leech->Lifespan = DelayedDamageLifeSpanSecs;         // uc:572
	// SetSourceAngreal (uc:573): the MyrddraalSwordAngreal gives the drain its Spirit
	// element identity (IgnoresEffect/reflector matching) + its HUD status icon.
	if (SourceAngreal.IsValid())
	{
		Leech->SetSourceAngreal(SourceAngreal.Get());
	}
	Leech->LeechInstigator = GetNPC();
	Leech->AttachTo(PawnVictim);                         // uc:576
	CurrentLeech = Leech;
}

void UWoTMyrddraalTeleportBehavior::CommitReappear()
{
	// ExitMyrddraalTeleporting (uc:508-522): land at the chosen node (retail lands at
	// the effect-rotator's wandered location — folded to the chosen node position,
	// PORT_NOTES), record it, count the blink, and face the threat.
	AWoTNPCCharacter* Self = GetNPC();
	if (!Self)
	{
		return;
	}
	const FVector Destination = bHasPendingDestination
		? PendingDestination : Self->GetActorLocation();
	if (Self->SetActorLocation(Destination, /*bSweep*/ false))
	{
		UWoTZoneIndex::RefreshAfterRelocation(Self);
	}
	// LastTeleportPosition is stamped at SELECTION in FindTeleportPosition (uc:283), not
	// here — retail's ExitMyrddraalTeleporting never re-stamps it, and stamping at pick
	// time is what lets a beacon dropped before arrival be excluded on the retry.
	bHasPendingDestination = false;
	++TeleportCount;

	// SetRotation(Rotator(ReturnThreatLocation() - Location)) (uc:512).
	if (const UWoTBrainComponent* Brain = GetBrain())
	{
		const FWoTGoal& Threat = Brain->GetGoal(EWoTGoalIndex::Threat);
		if (Threat.HasActor())
		{
			const FVector ToThreat = Threat.GoalActor->GetActorLocation() - Destination;
			if (!ToThreat.IsNearlyZero())
			{
				Self->SetActorRotation(ToThreat.GetSafeNormal2D().Rotation());
			}
		}
	}
}

void UWoTMyrddraalTeleportBehavior::DestroySeekers()
{
	// DestroySeekers (uc:465-478): drop the lock of every seeker homing on us, then
	// ShiftOutOfLeechesAndReflectors(Self).
	ClearSeekersTargetingSelf();
	ShiftOutOfLeechesAndReflectors();
}

bool UWoTMyrddraalTeleportBehavior::DetectActiveTeleportOffender() const
{
	// Trigger 3 — the ACTIVE DefensiveDetectorMyrddraal (Myrddraal.uc:618): a Seeking
	// Projectile whose Destination is this pawn (RejectOffender, DefensiveDetector
	// Myrddraal.uc), inside CollectionRadius 2048, within the pawn's FOV cone, whose
	// straight-line arrival time falls in the response window [1.0,7.5]s. CollectClass
	// is SeekingProjectile only, so the shared WotProjectile scan (which returns the
	// nearest of ANY class) is not reused — this walks only the seeker set.
	const AWoTNPCCharacter* Self = GetNPC();
	const UWorld* World = Self ? Self->GetWorld() : nullptr;
	if (!Self || !World)
	{
		return false;
	}
	const FVector Origin = Self->GetActorLocation();
	const float RadiusSq = FMath::Square(ActiveDetectorRadius);
	for (TActorIterator<AWoTSeekingProjectile> It(World); It; ++It)
	{
		AWoTSeekingProjectile* Seeker = *It;
		if (!IsValid(Seeker) || Seeker->Destination != Self)   // RejectOffender: Destination==self
		{
			continue;
		}
		const FVector Delta = Seeker->GetActorLocation() - Origin;
		const float DistSq = Delta.SizeSquared();
		if (DistSq > RadiusSq)
		{
			continue;
		}
		return true;
	}
	return false;
}

bool UWoTMyrddraalTeleportBehavior::FindTeleportPosition(FVector& OutDestination)
{
	// FindTeleportPosition (uc:229-306): collect AWoTPathNode actors within the
	// collection radius of the anchor, reject null / == LastTeleportPosition (uc:297)
	// / non-dark (bOnlyCollectShadowNodes=True, uc:301-304), sort by distance from the
	// anchor (uc:253), and pick the CLOSEST that is reachable (ActorReachable /
	// FindPathToward, uc:274-276 → the NavMesh path test). The anchor is the threat
	// goal actor if it is a valid Pawn, else self (uc:431-434).
	const AWoTNPCCharacter* Self = GetNPC();
	const UWorld* World = Self ? Self->GetWorld() : nullptr;
	if (!Self || !World)
	{
		return false;
	}
	FVector AnchorLoc = Self->GetActorLocation();
	if (const UWoTBrainComponent* Brain = GetBrain())
	{
		const FWoTGoal& Threat = Brain->GetGoal(EWoTGoalIndex::Threat);
		if (Threat.HasActor() && Threat.GoalActor->IsA<APawn>())
		{
			AnchorLoc = Threat.GoalActor->GetActorLocation();
		}
	}

	const UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	const float CollectSq = FMath::Square(TeleportPositionCollectionRadius);
	const AWoTPathNode* Closest = nullptr;
	float ClosestDistSq = TNumericLimits<float>::Max();
	for (TActorIterator<AWoTPathNode> It(World); It; ++It)
	{
		const AWoTPathNode* Node = *It;
		if (!IsValid(Node) || !Node->GetDarkFlag())   // RejectTeleportPosition: dark only
		{
			continue;
		}
		const FVector NodeLoc = Node->GetActorLocation();
		if (bHasLastTeleportPosition &&
		    FVector::DistSquared(NodeLoc, LastTeleportPosition) < 1.0f)   // never the same spot twice
		{
			continue;
		}
		const float DistSq = FVector::DistSquared(NodeLoc, AnchorLoc);
		if (DistSq > CollectSq)
		{
			continue;   // outside CollectRadiusItems (uc:240)
		}
		// Only a REACHABLE node is a candidate, so a closer-but-unreachable node yields to
		// the next-closest reachable one (uc:274-276 walks accepted items in sorted order,
		// taking the first reachable). A real path test in production; accept-all headless.
		if (!IsTeleportNodeReachable(Self, Nav, NodeLoc))
		{
			continue;
		}
		if (DistSq < ClosestDistSq)
		{
			ClosestDistSq = DistSq;
			Closest = Node;
		}
	}
	if (Closest)
	{
		OutDestination = Closest->GetActorLocation();
		LastTeleportPosition = OutDestination;   // uc:283: stamp the pick at selection time
		bHasLastTeleportPosition = true;
		return true;
	}

	// Fallback (no shadow node qualifies): the synthetic away-from-threat point — a
	// documented divergence for node-less worlds (the retail maps author dark nodes,
	// but the headless specs place none). PORT_NOTES.
	// Retail simply refuses the episode when no collected dark node is usable.
	return false;
}

bool UWoTMyrddraalTeleportBehavior::IsTeleportNodeReachable(const AWoTNPCCharacter* Self,
	const UNavigationSystemV1* Nav, const FVector& NodeLoc) const
{
#if WITH_AUTOMATION_TESTS
	// Spec seam: a headless world builds no navmesh, so a spec marks specific node
	// positions unreachable to exercise the real "closest reachable, else next-closest"
	// pick (the DodgeBlockedDirsForTest precedent).
	for (const FVector& Blocked : UnreachableNodesForTest)
	{
		if (FVector::DistSquared(Blocked, NodeLoc) < 1.0f)
		{
			return false;
		}
	}
#endif
	if (!Nav || !Self)
	{
		// No nav system (headless): forced-open, the convention the AI specs already use.
		return true;
	}
	// Cheap pre-filter: the node must sit on the navmesh at all.
	FNavLocation Projected;
	if (!Nav->ProjectPointToNavigation(NodeLoc, Projected, FVector(200.0f, 200.0f, 400.0f)))
	{
		return false;
	}
	// ActorReachable / FindPathToward (uc:274-276): a real path from the pawn to the node
	// (the UWoTBrainComponent::IsGoalReachable path-sync precedent).
	UNavigationSystemV1* NavSys =
		FNavigationSystem::GetCurrent<UNavigationSystemV1>(Self->GetWorld());
	const ANavigationData* NavData = NavSys ? NavSys->GetDefaultNavDataInstance() : nullptr;
	if (!NavSys || !NavData)
	{
		return true;   // projected but no nav-data instance to path against — accept it
	}
	FPathFindingQuery Query(Self, *NavData, Self->GetActorLocation(), Projected.Location);
	Query.SetAllowPartialPaths(false);
	const FPathFindingResult Result = NavSys->FindPathSync(Query);
	return Result.IsSuccessful() && Result.Path.IsValid();
}

void UWoTMyrddraalTeleportBehavior::ClearSeekersTargetingSelf()
{
	const AWoTNPCCharacter* Self = GetNPC();
	if (!Self)
	{
		return;
	}
	for (TActorIterator<AWoTSeekingProjectile> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It) && It->Destination == Self)
		{
			It->SetDestination(nullptr);   // uc:474
		}
	}
}

void UWoTMyrddraalTeleportBehavior::ShiftOutOfLeechesAndReflectors()
{
	// ShiftOutOfLeeches + ShiftOutOfReflector (WOTUtil.uc:130-173): shed every leech
	// and reflector on the pawn that is bRemovable && bDeleterious && !bFromProjectile.
	// The port's leeches/reflectors are UActorComponents on the host; Unattach() /
	// Uninstall() destroy them (== UE1 UnAttach/UnInstall + Destroy).
	AWoTNPCCharacter* Self = GetNPC();
	if (!Self)
	{
		return;
	}
	TArray<UWoTLeech*> Leeches;
	Self->GetComponents<UWoTLeech>(Leeches);
	for (UWoTLeech* L : Leeches)
	{
		if (IsValid(L) && L->bRemovable && L->bDeleterious && !L->bFromProjectile)
		{
			L->Unattach();   // WOTUtil.uc:142-143
		}
	}
	TArray<UWoTReflector*> Reflectors;
	Self->GetComponents<UWoTReflector>(Reflectors);
	for (UWoTReflector* R : Reflectors)
	{
		if (IsValid(R) && R->bRemovable && R->bDeleterious && !R->bFromProjectile)
		{
			R->Uninstall();   // WOTUtil.uc:167-168
		}
	}
}

// ============================================================ BATrollocRecovery

void UWoTBATrollocRecoveryBehavior::OnPossessedBy(AWoTNPCController* Controller)
{
	Super::OnPossessedBy(Controller);
	// Default.CollisionRadius — the configured cylinder radius to restore to (uc:123).
	if (const AWoTNPCCharacter* Self = GetNPC())
	{
		if (const UCapsuleComponent* Cap = Self->GetCapsuleComponent())
		{
			DefaultCollisionRadius = Cap->GetUnscaledCapsuleRadius();
		}
	}
	bRecovering = false;
	LastRecoveryTime = 0.0f;
	CollisionAdjustTimer = 0.0f;
}

void UWoTBATrollocRecoveryBehavior::OnUnpossessed()
{
	if (bRecovering)
	{
		if (AWoTNPCController* Controller = GetController())
		{
			Controller->SetForcedStationary(false);
		}
		bRecovering = false;
	}
}

void UWoTBATrollocRecoveryBehavior::NotifyTookDamage(int32 Damage)
{
	// BATrolloc.TakeDamage (uc:83-97): a hit harder than the threshold staggers the
	// champion, at most once per RecoveryInterval. Super has already applied the hit,
	// so Health reflects the post-damage value (the uc reads Health after Super too).
	const AWoTNPCCharacter* Self = GetNPC();
	if (!Self)
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	if (Damage > RecoveryDamageAmount && Self->Health > 0 &&
	    (LastRecoveryTime + RecoveryInterval) < Now)
	{
		if (LastRecoveryTime == 0.0f && bNoInitialRecovery)
		{
			// Arm, don't stagger, on the very first qualifying hit (uc:88-91).
			LastRecoveryTime = Now + RecoveryInterval;
		}
		else
		{
			EnterRecovering();
		}
	}
}

void UWoTBATrollocRecoveryBehavior::EnterRecovering()
{
	// state Recovering (uc:197-208): stamp the cooldown, interrupt, hold for the
	// Recover anim. The suspend is the ForcedStationary gate (hold position + withhold
	// the attack) — the same controller gate the Minion freeze uses.
	LastRecoveryTime = GetWorld()->GetTimeSeconds();   // uc:203
	bRecovering = true;
	RecoverTimer = RecoverDuration;
	// PlayAnim 'Recover' + FinishAnim (uc:205-206): play the stagger and hold for the
	// sequence's real length where the table + sheet are bound (retires the 1.0
	// stand-in duration); RecoverDuration stays the table-less fallback.
	if (AWoTNPCCharacter* Self = GetNPC())
	{
		if (const FWoTAnimTableEntry* Pick = Self->StampSlotAnim(FName(TEXT("Recover"))))
		{
			const float Real = Self->SequenceDurationSeconds(
			    Pick->AnimName, WoTAnimTable::ResolveRate(Pick));
			if (Real > 0.0f)
			{
				RecoverTimer = WoTAnimTable::ResolveTweenTime(Pick) + Real;
			}
		}
	}
	if (AWoTNPCController* Controller = GetController())
	{
		Controller->SetForcedStationary(true);
	}
}

void UWoTBATrollocRecoveryBehavior::TickBehavior(float DeltaSeconds)
{
	if (bRecovering)
	{
		RecoverTimer -= DeltaSeconds;
		if (RecoverTimer <= 0.0f)
		{
			bRecovering = false;
			if (AWoTNPCController* Controller = GetController())
			{
				Controller->SetForcedStationary(false);
			}
		}
	}

	// OnAdjustCollisionNotification on its 0.5s cadence (DNI_Misc2, uc:265).
	CollisionAdjustTimer -= DeltaSeconds;
	if (CollisionAdjustTimer <= 0.0f)
	{
		CollisionAdjustTimer = CollisionAdjustInterval;
		AdjustCollision();
	}
}

void UWoTBATrollocRecoveryBehavior::AdjustCollision()
{
	// OnAdjustCollisionNotification (uc:109-125): grow to BiggerAssRadius while the
	// current goal is the threat and it is inside AdjustCollisionThreshold; otherwise
	// restore the default cylinder radius (height unchanged — SetCollisionSize keeps it).
	AWoTNPCCharacter* Self = GetNPC();
	const UWoTBrainComponent* Brain = GetBrain();
	if (!Self)
	{
		return;
	}
	bool bBigger = false;
	float Gap = 0.0f;
	if (Brain && Brain->CurrentGoalIndex == EWoTGoalIndex::Threat &&
	    Brain->GetGoalDistance(EWoTGoalIndex::Threat, Gap) && Gap < AdjustCollisionThreshold)
	{
		bBigger = true;
	}
	if (UCapsuleComponent* Cap = Self->GetCapsuleComponent())
	{
		Cap->SetCapsuleRadius(bBigger ? BiggerAssRadius : DefaultCollisionRadius);
	}
}

// ============================================================ LegionSpiritsStomp

float UWoTLegionSpiritsStompBehavior::StompKnockbackZ(float Distance, float Mass)
{
	// ShakeRadius (uc:297): Momentum.Z = 7000000 / (0.75*(Distance+500)*Mass).
	const float Denom = 0.75f * (Distance + 500.0f) * FMath::Max(Mass, 1.0f);
	return Denom > 0.0f ? 7000000.0f / Denom : 0.0f;
}

void UWoTLegionSpiritsStompBehavior::TickBehavior(float DeltaSeconds)
{
	if (bStompPending)
	{
		const AWoTNPCController* Controller = GetController();
		const AWoTNPCCharacter* Self = GetNPC();
		if (!Controller || !Self || Self->Health <= 0 ||
			Controller->GetBrainState() != EWoTBrainState::Tracking)
		{
			// The damage wave is a mesh notify: replacing the stomp animation (a
			// TakeHit detour or death) discards the pending notify just as retail does.
			bStompPending = false;
			bStompDamageFired = false;
			PendingStompTarget = nullptr;
			if (AWoTNPCController* MutableController = GetController())
			{
				MutableController->SetForcedStationary(false);
			}
			return;
		}
		PendingStompElapsed += DeltaSeconds;
		if (!bStompDamageFired && PendingStompElapsed >= PendingStompNotifyTime)
		{
			bStompDamageFired = true;
			if (PendingStompTarget.IsValid())
			{
				ApplyStompAt(ResolvePendingStompImpact(), PendingStompTarget.Get());
			}
		}
		if (PendingStompElapsed >= PendingStompEndTime)
		{
			bStompPending = false;
			PendingStompTarget = nullptr;
			if (AWoTNPCController* MutableController = GetController())
			{
				MutableController->SetForcedStationary(false);
			}
		}
		return;
	}
	// Drive the stomp on the offensive cadence: the Legion has no Grunt swing set
	// (MeleeRange 500 is the reach), so the AttemptAttack→PerformMeleeAttack chain
	// (which gates on MeleeAttacks) never fires it — the behavior does, once the
	// threat is inside range and the pawn is holding (Tracking, not stationary).
	StompTimer -= DeltaSeconds;
	if (StompTimer > 0.0f)
	{
		return;
	}
	StompTimer = StompInterval;

	const AWoTNPCController* Controller = GetController();
	UWoTBrainComponent* Brain = GetBrain();
	if (!Controller || Controller->GetBrainState() != EWoTBrainState::Tracking ||
	    Controller->IsForcedStationary() || !Brain ||
	    Brain->CurrentGoalIndex != EWoTGoalIndex::Threat ||
	    !Brain->IsGoalValid(EWoTGoalIndex::Threat))
	{
		return;
	}
	const FWoTGoal& G = Brain->GetGoal(EWoTGoalIndex::Threat);
	float Gap = 0.0f;
	// WithinMaxMeleeDistance (Legion.uc:193-206): the stomp is a MELEE attack, so it
	// requires the threat to be VISIBLE as well as inside the stomp reach. A not-visible
	// threat (even at melee distance) gets a seeker instead, never a blind stomp; lack
	// of sight leaves its intermittent seeker attack enabled (Legion.uc:193-206).
	// IsGoalVisible stamps the goal cache (non-const) — a server-side gameplay query,
	// as in the offensive pulse.
	if (!G.HasActor() || !Brain->GetGoalDistance(EWoTGoalIndex::Threat, Gap) ||
	    Gap > StompRange || !Brain->IsGoalVisible(EWoTGoalIndex::Threat))
	{
		return;
	}

	// The behavior cadence starts the anim; SpawnDamageWave1 itself waits for the
	// picked sequence's authored mesh notify (ATTACKSTOMP1 0.65, ATTACKSTOMP2 0.4;
	// Legion.uc:21-29 and DT_WoTAnimSequences). DisableMovement at 0.01 roots the
	// Legion for this animation rather than merely suppressing its locomotion stamp.
	if (AWoTNPCCharacter* Self = GetNPC())
	{
		const FWoTAnimTableEntry* Pick = Self->StampSlotAnim(Self->MeleeAnimSlot);
		if (!Pick)
		{
			return;
		}
		const float TweenTime = WoTAnimTable::ResolveTweenTime(Pick);
		float SequenceTime = Self->SequenceDurationSeconds(
			Pick->AnimName, WoTAnimTable::ResolveRate(Pick));
		if (SequenceTime <= 0.0f) { SequenceTime = 1.0f; }
		float NotifyFraction = Pick->AnimName == FName(TEXT("ATTACKSTOMP2")) ? 0.4f : 0.65f;
		if (const UWoTVertexAnimComponent* Anim =
			Cast<UWoTVertexAnimComponent>(Self->GetMesh()))
		{
			const float Authored = Anim->FindNotifyFraction(
				Pick->AnimName, FName(TEXT("SpawnDamageWave1")));
			if (Authored >= 0.0f) { NotifyFraction = Authored; }
		}
		PendingStompTarget = G.GoalActor;
		PendingStompElapsed = 0.0f;
		PendingStompNotifyTime = TweenTime + NotifyFraction * SequenceTime;
		PendingStompEndTime = TweenTime + SequenceTime;
		bStompDamageFired = false;
		bStompPending = true;
		if (AWoTNPCController* MutableController = GetController())
		{
			MutableController->StopMovement();
			MutableController->SetForcedStationary(true);
		}
		if (UCharacterMovementComponent* Move = Self->GetCharacterMovement())
		{
			Move->StopMovementImmediately();
		}
	}
}

FVector UWoTLegionSpiritsStompBehavior::ResolvePendingStompImpact() const
{
	AActor* Target = PendingStompTarget.Get();
	if (!IsValid(Target))
	{
		return GetNPC() ? GetNPC()->GetActorLocation() : FVector::ZeroVector;
	}
	// SpawnDamageWave1 computes VolcanoLocation at notify time, including the
	// target's then-current 0.3s lead (Legion.uc:246-274).
	FVector Impact = Target->GetActorLocation() + Target->GetVelocity() * TargetLead;
	FHitResult Floor;
	// Legion.uc:272 Trace(..., false): a target capsule containing the trace
	// start is ignored and the volcano is placed on level/mover geometry.
	if (WoTVisBlockingTrace(GetWorld(), Impact,
		Impact - FVector(0, 0, 1000.0f), GetNPC(), Floor))
	{
		Impact = Floor.Location;
	}
	return Impact;
}

void UWoTLegionSpiritsStompBehavior::ApplyStompAt(
	const FVector& ImpactPoint, AActor* Target)
{
	AWoTNPCCharacter* Self = GetNPC();
	if (!Self || !Self->HasAuthority())
	{
		return;
	}
	const FVector LegionLoc = Self->GetActorLocation();
	// SpawnDamageWave1 calls ShakeRadius BEFORE spawning the volcano (uc:268-270).
	// Pawns within ShakeRange are shoved with a distance-falloff momentum and a
	// player-only owning-view shake.
	for (TActorIterator<AWoTCharacter> It(GetWorld()); It; ++It)
	{
		AWoTCharacter* Victim = *It;
		if (Victim == Self || !IsValid(Victim) || Victim->Health <= 0)
		{
			continue;
		}
		const float Distance = FVector::Dist(LegionLoc, Victim->GetActorLocation());
		if (Distance > ShakeRange)
		{
			continue;
		}
		if (!Victim->GetCharacterMovement()->IsMovingOnGround()
			|| !FMath::IsNearlyZero(Victim->GetVelocity().Z))
		{
			continue;
		}
		const float Mass = FMath::Max(1.0f, Victim->GetCharacterMovement()->Mass);
		if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Victim->GetController()))
		{
			const float ShakeMag = FMath::Max(750.0f, 2400.0f - Distance);
			PC->ClientShakeView(FMath::Max(0.0f, 0.35f - Distance / 25000.0f),
				ShakeMag, ShakeMag * 0.015f);
		}
		FVector Momentum = -0.75f * Victim->GetVelocity() + 100.0f * WoTRandom::VRand();
		Momentum.Z = StompKnockbackZ(Distance, Mass);
		Victim->AddUE1Velocity(Momentum);
	}

	// The live Effects actor's initial encroachment and later re-entry drive Touch.
	// A pawn tossed above has already entered Falling, so its Touch takes damage
	// and shakes but does not also receive the walking-only +256 volcano launch.
	if (UWorld* World = GetWorld())
	{
		FActorSpawnParameters Params;
		Params.Owner = IsValid(Target) ? Target : Self;
		Params.Instigator = Cast<APawn>(Params.Owner);
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		World->SpawnActor<AWoTLegionStompVolcano>(
			AWoTLegionStompVolcano::StaticClass(), ImpactPoint,
			FRotator::ZeroRotator, Params);
	}
}

// ============================================================ ArcherBow

void UWoTArcherBowBehavior::OnPossessedBy(AWoTNPCController* Controller)
{
	Super::OnPossessedBy(Controller);
	// Equip the bow before BuildTrackingBands runs (same OnPossess pass), so the
	// controller detects the weapon component and hands out the projectile-fighter
	// ladder (WoTNPCController::BuildTrackingBands).
	EquipBow();
}

void UWoTArcherBowBehavior::EquipBow()
{
	AWoTNPCCharacter* NPC = GetNPC();
	if (!NPC || Bow.IsValid())
	{
		return;
	}
	UWoTWeaponComponent* Weapon = NewObject<UWoTWeaponComponent>(NPC);
	// ArcherBowProxy.uc:11-19 + WotWeapon CDO (bRequireTargetVisibility True): a
	// projectile-only weapon (no melee window — the Archer's melee is its MeleeAttacks
	// set, not the sword weapon) firing the ArcherArrow.
	Weapon->MinProjectileRange = MinProjectileRange;
	Weapon->MaxProjectileRange = MaxProjectileRange;
	Weapon->MinMeleeRange = 0.0f;
	Weapon->MaxMeleeRange = 0.0f;
	Weapon->bRequireTargetVisibility = true;
	Weapon->MissOdds = MissOdds;
	Weapon->ProjectileClass = AWoTArcherArrow::StaticClass();
	Weapon->RegisterComponent();
	Bow = Weapon;
}

void UWoTArcherBowBehavior::TickBehavior(float DeltaSeconds)
{
	(void)DeltaSeconds;
	AWoTNPCController* Controller = GetController();
	UWoTBrainComponent* Brain = GetBrain();
	if (!Controller || !Brain)
	{
		return;
	}

	// CheckForMeleeSwitch (Archer.uc:167) folded onto the prefer-visibility toggle.
	// The target is the current goal (GetGoal(CurrentGoalIdx), uc:173).
	const EWoTGoalIndex Goal = Brain->CurrentGoalIndex;
	float Gap = 0.0f;
	const bool bHasTarget = Brain->IsGoalValid(Goal) && Brain->GetGoal(Goal).HasActor() &&
	                        Brain->GetGoalDistance(Goal, Gap);
	if (!bHasTarget)
	{
		// Losing a target does not perform SwitchToBestWeapon. Keep the melee
		// commitment (and any held sword) until a later ranged target makes the
		// component-backed bow the real best-weapon choice.
		return;
	}

	if (!bBowArmed)
	{
		// Committed to melee: re-arm the bow when the target opens the range back up
		// past the switch distance (the shipped SwitchToBestWeapon re-picks the bow by
		// range/desireability once melee is no longer the better option) — but only
		// once WOTPawn's switch gate has elapsed (WOTPawn.uc:172:
		// TimeSeconds - LastWeaponSwitchTime >= MinTimeBetweenWeaponSwitch; Archer
		// CDO 5.0). Re-arming also clears the held sword: the bow is
		// component-backed, so the held-actor seam goes to the body/none state
		// while shooting.
		if (Gap >= SwitchToMeleeDistance &&
			GetWorld()->GetTimeSeconds() - LastWeaponSwitchTime >= MinTimeBetweenWeaponSwitch &&
			Controller->GetBrainState() == EWoTBrainState::Tracking)
		{
			AWoTNPCCharacter* NPC = GetNPC();
			if (NPC && NPC->GetHeldWeapon() == NPC->GetMeleeWeapon())
			{
				// The bow is component-backed, but DownWeapon.TweenDown still asks the
				// held ArcherSword to prepare its owner. Do not publish the bow latch
				// until DRAWSWRD's real switch notify has cleared the sword.
				Controller->RouteToComponentBackedRangedWeapon();
				return;
			}
			bBowArmed = true;
			LastWeaponSwitchTime = GetWorld()->GetTimeSeconds();
			Controller->SetTrackingPreferVisibility(true);
		}
	}
}

void UWoTArcherBowBehavior::CommitToMelee()
{
	if (AWoTNPCController* Controller = GetController())
	{
		// The melee commit is itself a successful switch: stamp the gate so the
		// bow cannot re-arm for MinTimeBetweenWeaponSwitch (WOTPawn.uc:172).
		bBowArmed = false;
		LastWeaponSwitchTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		Controller->SetTrackingPreferVisibility(false);
	}
}
