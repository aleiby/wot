#include "WoTCharacter.h"
#include "WoTRandom.h"
#include "Assets/WoTRuntimeAssets.h"
#include "Assets/WoTRetailIdentity.h"

#include "WoTGameMode.h"
#include "WoTPlayerController.h"
#include "WoTPlayerState.h"
#include "WoTPreferencesSubsystem.h"
#include "WoTCitadelResources.h"
#include "WoTHUD.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Inventory/WoTHandInfo.h"
#include "Inventory/WoTHandSet.h"
#include "WoTPayloadSubsystem.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeBSPRenderer.h"
#include "World/WoTRuntimeWorldProperties.h"
#if !WOT_GUARDED_BOOTSTRAP
#include "RetailData/WoTDevRetailFamilies.h"
#endif
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTAngrealInvAirBurst.h"
#include "Inventory/WoTAngrealInvSpecial.h"
#include "Inventory/WoTBagHolding.h"
#include "Inventory/WoTSeal.h"
#include "Inventory/WoTLeech.h"
#include "Inventory/WoTZoneLeeches.h"
#include "Inventory/WoTReflector.h"
#include "Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "Projectiles/WoTAngrealProjectile.h"
#include "Projectiles/WoTGenericProjectile.h"
#include "Projectiles/WoTSeekingProjectile.h"
#include "WoTUE1Names.h"
#include "WoTAnimTable.h"
#include "WoTBloodFX.h"
#include "WoTAmbientSound.h"
#include "WoTBubbles.h"
#include "WoTSound.h"
#include "WoTSoundTable.h"
#include "WoTRetailText.h"
#include "WoTCarcass.h"
#include "WoTMeshLight.h"
#include "WoTMeshModulate.h"
#include "WoTVertexAnimComponent.h"
#include "AI/WoTNPCController.h"
#include "AI/WoTNPCCharacter.h"
#include "AI/WoTCasterComponent.h"
#include "AI/WoTCreatureData.h"
#include "AI/WoTPathNodeIterator.h"

#include "WoTCharacterMovement.h"
#include "WoTFootsteps.h"
#include "WoTWeaveFX.h"
#include "WoTZoneVolume.h"
#include "Traps/WoTPit.h"

#include "Components/CapsuleComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/DataTable.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"      // SetControlRotation (server-side aim stamp)
#include "Kismet/GameplayStatics.h"        // PlaySound2D (owner-local selection feedback)
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "Sound/SoundBase.h"

namespace
{
	// A held movement key's UE1 axis value: UInput adds Speed*DeltaTime per held
	// tick and ReadInput scales every axis by 20/DeltaTime,
	// so the default Speed=300 bindings (User.ini MoveForward, StrafeLeft,
	// Jump's aUp) read 6000.
	constexpr float RetailHeldKeyAxis = 20.0f * 300.0f;
	// PlayerSwimming.PlayerMove (engine PlayerPawn; WOTPlayer's
	// PlayerSwimming does not override it): aForward*0.2 along the full
	// ViewRotation, aStrafe*0.1, aUp*0.1 straight up. Walking's aForward*0.4
	// (2400) saturates AccelRate 2048, so walking input is full scale; in water
	// these stay below AccelRate and set the acceleration magnitude.
	constexpr float RetailSwimForwardAccel = 0.2f * RetailHeldKeyAxis;   // 1200
	constexpr float RetailSwimSideAccel = 0.1f * RetailHeldKeyAxis;      // 600

	FName WoTStatusIconTextureName(const TSoftObjectPtr<UTexture2D>& Texture)
	{
		return Texture.IsNull() ? NAME_None : FName(*Texture.GetAssetName());
	}

	// Retail STY_Modulated uses the raw skin as an unlit 2x framebuffer multiplier:
	// neutral gray is identity, and no vertex-light or fog contribution is applied.
	UMaterialInterface* WoTNeutralGhostMaster()
	{
		return WoTMeshModulate::GetMaster();
	}
}

bool WoTDecodePlayerHealthDefault(const FWoTRuntimeClassDefaults& Defaults,
	int32& OutHealth, FString& OutError)
{
	const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(
		TEXT("WOT.WOTPlayer"), TEXT("Health"));
	if (Property == nullptr)
	{
		OutError = TEXT("WOT.WOTPlayer has no inherited Health default");
		return false;
	}
	if (!WoTRuntimeWorldPropertyInteger(*Property, OutHealth) || OutHealth <= 0)
	{
		OutError = TEXT("WOT.WOTPlayer inherited Health is not a positive integer");
		return false;
	}
	return true;
}

bool AWoTCharacter::ApplyRetailPlayerHealthDefaults(
	const FWoTRuntimeClassDefaults& Defaults, FString& OutError)
{
	int32 DefaultHealth = 0;
	if (!WoTDecodePlayerHealthDefault(Defaults, DefaultHealth, OutError))
	{
		return false;
	}
	MaxHealth = DefaultHealth;
	HitReferenceHealth = static_cast<float>(DefaultHealth);
	// Health replicates and the authority owns it. A client runs BeginPlay
	// after the initial replicated Health arrives, so resetting it here would
	// show a late joiner an already-damaged pawn at full health.
	if (HasAuthority())
	{
		Health = DefaultHealth;
	}
	return true;
}

bool AWoTCharacter::PlayerHitQualifies(
	const int32 Damage, const float MomentumZ) const
{
	return Damage > 0
		&& (Damage > 0.25f * HitReferenceHealth || MomentumZ > 300.0f);
}

AWoTCharacter::AWoTCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer
		.SetDefaultSubobjectClass<UWoTCharacterMovement>(
			ACharacter::CharacterMovementComponentName)
		// The pawn mesh IS the UE1 vertex-anim surface: OnRep_Anim's replay and every
		// creature/faction mesh bind land on this component.
		.SetDefaultSubobjectClass<UWoTVertexAnimComponent>(ACharacter::MeshComponentName))
{
	// The minimal locomotion anim driver runs in Tick (authority-only, state-change).
	PrimaryActorTick.bCanEverTick = true;

	InventoryComponent =
		CreateDefaultSubobject<UWoTInventoryComponent>(TEXT("InventoryComponent"));

	// WOTPlayer.DefaultAngrealInventory — the MP faction pawns' shared AngrealInvAirBurst
	// default (WoTPawns/AesSedai.uc:88 et al.; this one character stands in for all four).
	DefaultAngrealInventory = AWoTAngrealInvAirBurst::StaticClass();

	// UE1 cylinder 17 x 46 (half-height) → capsule; same units.
	GetCapsuleComponent()->InitCapsuleSize(17.0f, 46.0f);
	BaseEyeHeight = 42.0f;

	// Runtime actor lighting: opt the pawn mesh into lighting
	// channel 1. The world BSP is lit by the channel-0 baked-atlas material; the
	// placed WoT PointLights are channel-1-ONLY (import_map place_lights). A pawn
	// mesh left on the default channel-0 receives NONE of those lights and renders
	// black. Decorations/movers already opt into channel 0+1; match them.
	if (USkeletalMeshComponent* Body = GetMesh())
	{
		Body->LightingChannels.bChannel1 = true;
		// Retail first person never draws the viewer's own pawn (the renderer
		// skips the view target unless behind-view, and WoT MP has no
		// behind-view). Without this the owner sees their own body mesh from
		// inside (a full body in first person), and the
		// giant hand/arm silhouette photobombing shot-harness poses whenever
		// the capture camera visits the spawn PlayerStart. Other clients still
		// see this mesh. There is NO retail 3D first-person hand/held-angreal
		// overlay to add on top (zero PlayerViewMesh= authors in
		// the whole shipped script, WOTPlayer.uc:3530 Weapon=None, no
		// RenderOverlays overrides, no HUD DrawActor; Engine BecomeItem keeps
		// held items bHidden) — the visible retail "hands" are the 2D HUD hand
		// interface, ported with the HUD.
		Body->SetOwnerNoSee(true);
	}

	// Spawn EXACTLY at the requested transform — never let the engine "adjust" us. The APawn
	// default (AdjustIfPossibleButDontSpawnIfColliding) runs FindTeleportSpot when SpawnActor's
	// overlap test trips, and that test is UNRELIABLE against the maps' complex-as-simple BSP
	// trimesh (no defined interior): at some PlayerStarts it false-positives an overlap, then
	// "adjusts" the pawn to the nearest clear spot — which it finds ~17,650uu DOWN in the void,
	// so the pawn spawns below the world and free-falls forever. Diagnosed via a same-box
	// two-client repro + a post-SpawnActor location probe.
	// AlwaysSpawn seats us at the PlayerStart like retail (fall + land); any true residual
	// overlap is resolved by CharacterMovement's CLAMPED depenetration, not a void teleport.
	SpawnCollisionHandlingMethod = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->MaxWalkSpeed = 400.0f;        // GroundSpeed
	Move->MaxFlySpeed = 400.0f;         // AirSpeed
	Move->MaxSwimSpeed = 200.0f;        // WaterSpeed
	Move->MaxAcceleration = 2048.0f;    // AccelRate
	Move->JumpZVelocity = 325.0f;       // JumpZ
	Move->AirControl = 1.0f;            // AirControl (full — signature WoT)
	Move->MaxStepHeight = 25.0f;
	// UE1 calls a hit a sloped wall only below Normal.Z 0.7 (its
	// physWalking bHitSlopedWall; >= 0.7 is floor). UE5's default 0.71
	// (44.765 deg) turned retail's exact-45-degree chamfered steps into walls.
	Move->SetWalkableFloorZ(0.7f);
	Move->Mass = 175.0f;
	// WOTPlayer.uc:7289-7290. Our retail PhysSwimming divides pawn buoyancy
	// by Mass; this is a mass quantity, not UE5's immersion multiplier.
	Move->Buoyancy = 150.0f;
	// Disguise copies speeds only (WOTPlayer.uc:1926-1931), not these values.

	// Gravity: UE1 ZoneGravity = -950 uu/s^2 (Engine ZoneInfo default; the
	// Arena/Battle maps don't override it). UE5's world gravity defaults to
	// -980, so scale to reproduce -950 exactly. VALIDATED against the reference
	// footage (exports/reference/feel): standing jump airtime = 41 frames @60fps
	// => g = 2*JumpZ/airtime = 2*325/0.6833 = 951 uu/s^2. See measurements.json.
	Move->GravityScale = 950.0f / 980.0f;   // -> effective -950

	// Braking: UE1 walking has NO constant deceleration term — a stopping pawn
	// decays purely by exponential friction. The UE1 walking integrator, with
	// zero Acceleration, applies:
	//     Velocity -= (2 * Velocity) * dt * friction        // rate 2*friction
	// sub-stepped at 0.03s, snapping to zero below speed 10. UE5's CMC braking
	// reproduces this EXACTLY: GroundFriction*BrakingFrictionFactor = 8*2 = 16/s
	// matches UE1's 2*friction (friction=8), and UE5's BRAKE_TO_STOP_VELOCITY=10
	// + 1/33s substep already match UE1's speed<10 stop + 0.03s substep. The
	// only correction vs a naive port is dropping the constant braking-decel
	// term (was 2048, which stopped the pawn far more crisply than 1999).
	// bUseSeparateBrakingFriction stays false so GroundFriction feeds braking.
	// NOTE: BrakingDeceleration must be a hair above zero — CMC only applies its
	// BRAKE_TO_STOP_VELOCITY=10 snap when !bZeroBraking (CharacterMovementComponent
	// .cpp:4423); at exactly 0 the pawn creeps exponentially to ~0 instead of
	// snapping like UE1's speed<10 stop. 0.01 uu/s^2 adds
	// no perceptible decel but re-arms the snap.
	Move->GroundFriction = 8.0f;             // = UE1 ZoneGroundFriction
	Move->BrakingFrictionFactor = 2.0f;      // UE5 default; = UE1's "2*friction"
	Move->BrakingDecelerationWalking = 0.01f; // UE1 has no constant braking term

	bUseControllerRotationYaw = true;
}

void AWoTCharacter::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	// WOTPlayer.SelectedStr/TerAngrealStr are localized class defaults. Load
	// them from the authenticated WoT.int resource (or the purchaser's retail
	// file in development) exactly like UE1 UObject::LoadLocalized; there is no
	// compiled retail-text fallback.
	FWoTRetailText::ApplyLocalizedDefaults(this);
}

float AWoTCharacter::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
                                AController* EventInstigator, AActor* DamageCauser)
{
	// Pawn.TakeDamage is authority-only.
	if (!HasAuthority())
	{
		return 0.0f;
	}
	// UE's god flag (CheatManager::God, the playthrough walker): no damage and
	// none of its effects. Ordinary pawns always keep it set.
	if (!CanBeDamaged())
	{
		return 0.0f;
	}

	// Recover the original WoT damage-type name (AEFWS) if the causer carried it.
	FName WoTDamageType = TEXT("hurt");
	FVector HitLocation = GetActorLocation();
	FVector UE1Momentum = FVector::ZeroVector;
	if (DamageEvent.IsOfType(FWoTDamageEvent::ClassID))
	{
		const FWoTDamageEvent& Ev = static_cast<const FWoTDamageEvent&>(DamageEvent);
		WoTDamageType = Ev.WoTDamageType;
		UE1Momentum = Ev.UE1Momentum;
		if (!Ev.HitInfo.ImpactPoint.IsZero())
		{
			HitLocation = Ev.HitInfo.ImpactPoint;
		}
	}
	// WOTPlayer.uc:1579-1601 enters these states before the ordinary damage
	// reflector/health chain. NPCs have their own Minion/Warder behavior and do
	// not use the player movement-state machine.
	if (HasAuthority() && !IsNPCPawn())
	{
		if (WoTDamageType == TEXT("SkewRip"))
		{
			SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::Impaled);
		}
		else if (WoTDamageType == TEXT("Grabbed"))
		{
			SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::Grabbed);
		}
	}

	// UnrealScript's float->int coercion truncates toward zero,
	// so fractional splash damage rounds DOWN at the TakeDamage call boundary.
	return ApplyDamageChain(FMath::TruncToInt(DamageAmount), WoTDamageType,
	                        EventInstigator, DamageCauser, HitLocation, UE1Momentum);
}

void AWoTCharacter::OnJumped_Implementation()
{
	Super::OnJumped_Implementation();
	// WOTPlayer.PlayInAir (WOTPlayer.uc:2256-2264): a real jump holds the
	// authored Jump pose for the whole airborne state. OnMovementModeChanged may
	// have observed MOVE_Falling first; this edge deliberately replaces Fall.
	if (!IsNPCPawn() && IsAlive())
	{
		BaseEyeHeight = 0.7f * RetailStandingBaseEyeHeight;
		PlayPlayerTweenPose(FName(TEXT("Jump")));
	}
	if (HasAuthority() && IsAlive())
	{
		PlayVoiceSlot(FName(TEXT("Jump")));   // PlayInAir (Grunt.uc:1044)
	}
}

void AWoTCharacter::Landed(const FHitResult& Hit)
{
	if (!IsNPCPawn()
		&& GetEffectivePlayerMovementConstraint()
			!= EWoTPlayerMovementConstraint::LandingFreeze)
	{
		ExitPlayerMovementConstraint();
	}
	// WOTPlayer.Landed (WOTPlayer.uc:2362-2368): the engine's jump-onto-
	// ladder fix calls Landed with the vertical wall normal. Players take the
	// ladder-only hook and skip every normal thump/noise/damage branch. WOTPawn
	// has no corresponding normal gate, so NPCs continue through their inherited
	// landing path even though CheckSurfaces itself runs for every pawn.
	if (!IsNPCPawn() && Hit.Normal.Z <= LadderLandedHitNormalZ)
	{
		LandedOnLadder();
		return;
	}
	// PlayLanded (Grunt.uc:1056-1070): LandHard under -1200, LandSoft under -600
	// (thresholds :1563-1564), living pawns only. Read the impact velocity before
	// the CMC resets it.
	const float ImpactVz = GetCharacterMovement() ? GetCharacterMovement()->Velocity.Z : 0.0f;
	LastLandingImpactVelocityForTest = ImpactVz;
	// WOTPlayer.Landed applies falling damage before its Health > 0 / PlayLanded
	// branch (WOTPlayer.uc:2372-2405). This ordering is observable: a fall-damage
	// TakeHit must finish before PlayerLanded stamps Land, rather than replacing
	// the Land animation and shortening the newly entered freeze.
	TakeFallingDamage(ImpactVz);
	// WOTPlayer.PlayLanded enters PlayerLanded at the authored
	// SpeedPlayAnimAfterLanding=1100 threshold. Landed executes on the autonomous
	// proxy as well as the authority, which preserves UE1's predicted zero-input
	// PlayerMove while the server confirms the state over replication.
	if (!IsNPCPawn() && IsAlive() && ImpactVz <= -1100.0f)
	{
		BeginHardLanding();
	}
	else if (!IsNPCPawn() && IsAlive())
	{
		// WOTPlayer.PlayLanded (WOTPlayer.uc:2331-2341): even a minor
		// landing tweens to the Land pose so the model does not remain in Fall.
		PlayPlayerTweenPose(FName(TEXT("Land")));
	}
	if (HasAuthority() && IsAlive())
	{
		if (ImpactVz <= -1200.0f)
		{
			PlayVoiceSlot(FName(TEXT("LandHard")));
		}
		else if (ImpactVz <= -600.0f)
		{
			PlayVoiceSlot(FName(TEXT("LandSoft")));
		}
	}
	// The landing thud (HandleLandedOnTexture -> HandleLandingOnTexture,
	// GenericAssetsHelper.uc:38-43 -> GenericTextureHelper.uc:578-592): the
	// SG_Landed wave at volume 0.005 * Mass * (Vz/JumpZ)^2 — the CMC Mass
	// carries the shipped WOTPlayer 175 (ctor). UE1 deferred one walking tick
	// only because its texture cache was stale on impact.
	if (HasAuthority() && IsAlive() && GetCharacterMovement())
	{
		const float JumpZ = FMath::Max(GetCharacterMovement()->JumpZVelocity, 1.0f);
		const float ThudVolume = 0.005f * GetCharacterMovement()->Mass *
		                         FMath::Square(ImpactVz / JumpZ);
		PlaySurfaceSound(EWoTFootstepGroup::Landed, FMath::Min(ThudVolume, 2.0f));
	}
	Super::Landed(Hit);
}

void AWoTCharacter::BeginHardLanding()
{
	if (IsNPCPawn() || IsHardLanding() || !GetWorld())
	{
		return;
	}
	// PlayerLanded.BeginState (WOTPlayer.uc:3270).
	BaseEyeHeight = 0.1f * RetailStandingBaseEyeHeight;
	// The owning proxy predicts presentation independently because RepAnim skips
	// it.  Clear the local fire latch on the same edge as authority so Attack
	// cannot resume after the landing freeze while the physical key stays held.
	if (HasAuthority() || IsLocallyControlled())
	{
		bPlayerAnimFireHeld = false;
		DrivenLocomotionAnim = NAME_None;
	}
	if (HasAuthority())
	{
		// WOTPlayer.PlayerLanded.BeginState (WOTPlayer.uc:3227): release the
		// currently used angreal before entering the authoritative freeze.
		if (InventoryComponent)
		{
			InventoryComponent->CeaseUsingAngreal();
		}
		SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::LandingFreeze);
	}
	else
	{
		SetPredictedPlayerMovementConstraint(
			EWoTPlayerMovementConstraint::LandingFreeze);
	}
}

void AWoTCharacter::StartHardLandingPresentation()
{
	++HardLandingEnterCountForTest;
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
	}

	const FWoTAnimTableEntry* Pick =
		WoTAnimTable::PickSlotAnim(AnimTableName, FName(TEXT("Land")), this);
	float CompletionSeconds = 1.0f;
	if (Pick)
	{
		HardLandingSequence = Pick->AnimName;
		const float Rate = WoTAnimTable::ResolveRate(Pick);
		const float TweenTime = WoTAnimTable::ResolveTweenTime(Pick);
		const float Duration = SequenceDurationSeconds(Pick->AnimName, Rate);
		CompletionSeconds = TweenTime + (Duration > 0.0f ? Duration : 1.0f);
		HoldLocomotionFor(CompletionSeconds);
		// RepAnim deliberately skips the owning connection, and authority never
		// receives its own OnRep.  Drive both through the component exactly once;
		// its authority seam stamps the replicated intent.  This also animates a
		// remote player's authority pawn in a listen-server viewport.
		if (HasAuthority() || IsLocallyControlled())
		{
			if (UWoTVertexAnimComponent* Anim =
				FindComponentByClass<UWoTVertexAnimComponent>())
			{
				if (Anim->FindSequence(Pick->AnimName))
				{
					Anim->TweenPlayAnim(Pick->AnimName, Rate, TweenTime);
					Anim->OnAnimEnd.AddUniqueDynamic(
						this, &AWoTCharacter::HandleHardLandingAnimEnd);
				}
				else if (HasAuthority() && Anim->Sequences.IsEmpty())
				{
					SetTweenAnimIntent(Pick->AnimName, Rate, TweenTime, false);
				}
			}
			else if (HasAuthority())
			{
				SetTweenAnimIntent(Pick->AnimName, Rate, TweenTime, false);
			}
		}
	}
	LastHardLandingCompletionSecondsForTest = CompletionSeconds;
	GetWorldTimerManager().SetTimer(HardLandingTimer, this,
		&AWoTCharacter::EndHardLanding, CompletionSeconds, false);
}

void AWoTCharacter::HandleHardLandingAnimEnd(FName)
{
	// PlayerLanded.AnimEnd leaves the state even when a TakeHit animation replaced
	// Land, so the sequence identity is intentionally not filtered here.
	EndHardLanding();
}

void AWoTCharacter::EndHardLanding()
{
	if (!IsHardLanding())
	{
		return;
	}
	if (HasAuthority())
	{
		SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	}
	else
	{
		SetPredictedPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	}
}

void AWoTCharacter::StopHardLandingPresentation()
{
	GetWorldTimerManager().ClearTimer(HardLandingTimer);
	if (UWoTVertexAnimComponent* Anim =
		FindComponentByClass<UWoTVertexAnimComponent>())
	{
		Anim->OnAnimEnd.RemoveDynamic(this,
			&AWoTCharacter::HandleHardLandingAnimEnd);
	}
	HardLandingSequence = NAME_None;
	BaseEyeHeight = RetailStandingBaseEyeHeight;
}

void AWoTCharacter::TrackAuthorityLandingAnimation(FName Sequence,
	float Rate, float TweenTime, bool bTweenOnly, bool bLoop)
{
	if (!HasAuthority() || !GetWorld()
		|| PlayerMovementConstraint
			!= EWoTPlayerMovementConstraint::LandingFreeze)
	{
		return;
	}

	HardLandingSequence = Sequence;
	GetWorldTimerManager().ClearTimer(HardLandingTimer);
	if (bLoop)
	{
		// UE1 PlayerLanded leaves on AnimEnd. A loop replacement has no AnimEnd.
		LastHardLandingCompletionSecondsForTest = 0.0f;
		return;
	}

	float CompletionSeconds = FMath::Max(0.0f, TweenTime);
	if (!bTweenOnly)
	{
		const float Duration = SequenceDurationSeconds(Sequence, Rate);
		CompletionSeconds += Duration > 0.0f ? Duration : 1.0f;
	}
	CompletionSeconds = FMath::Max(CompletionSeconds, 0.01f);
	LastHardLandingCompletionSecondsForTest = CompletionSeconds;
	GetWorldTimerManager().SetTimer(HardLandingTimer, this,
		&AWoTCharacter::EndHardLanding, CompletionSeconds, false);
}

void AWoTCharacter::SetPlayerMovementConstraint(
	EWoTPlayerMovementConstraint NewState)
{
	if (!ensureMsgf(HasAuthority(),
		TEXT("PlayerMovementConstraint is authority-owned; client writes are rejected")))
	{
		return;
	}
	if (IsNPCPawn() || PlayerMovementConstraint == NewState)
	{
		return;
	}
	const EWoTPlayerMovementConstraint OldState = PlayerMovementConstraint;
	PlayerMovementConstraint = NewState;
	EffectivePlayerMovementConstraint = NewState;
	if (NewState == EWoTPlayerMovementConstraint::Impaled)
	{
		++ImpaledConstraintApplyCountForTest;
	}
	ApplyPlayerMovementConstraintTransition(OldState, NewState);
	ForceNetUpdate();
}

void AWoTCharacter::OnRep_PlayerMovementConstraint()
{
	const EWoTPlayerMovementConstraint OldState =
		EffectivePlayerMovementConstraint;
	EffectivePlayerMovementConstraint = PlayerMovementConstraint;
	if (PlayerMovementConstraint == EWoTPlayerMovementConstraint::Impaled)
	{
		++ImpaledConstraintApplyCountForTest;
	}
	ApplyPlayerMovementConstraintTransition(OldState, PlayerMovementConstraint);
}

void AWoTCharacter::SetPredictedPlayerMovementConstraint(
	EWoTPlayerMovementConstraint NewState)
{
	if (GetLocalRole() != ROLE_AutonomousProxy
		|| EffectivePlayerMovementConstraint == NewState)
	{
		return;
	}
	const EWoTPlayerMovementConstraint OldState =
		EffectivePlayerMovementConstraint;
	EffectivePlayerMovementConstraint = NewState;
	ApplyPlayerMovementConstraintTransition(OldState, NewState);
}

void AWoTCharacter::ExitPlayerMovementConstraint()
{
	if (HasAuthority())
	{
		SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	}
	else
	{
		SetPredictedPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	}
}

void AWoTCharacter::ApplyPlayerMovementConstraintTransition(
	EWoTPlayerMovementConstraint OldState,
	EWoTPlayerMovementConstraint NewState)
{
	if (OldState == EWoTPlayerMovementConstraint::LandingFreeze
		&& NewState != EWoTPlayerMovementConstraint::LandingFreeze)
	{
		StopHardLandingPresentation();
	}
	ApplyPlayerMovementConstraint(NewState);
	if (NewState == EWoTPlayerMovementConstraint::LandingFreeze
		&& OldState != EWoTPlayerMovementConstraint::LandingFreeze)
	{
		StartHardLandingPresentation();
	}
	if (NewState == EWoTPlayerMovementConstraint::Impaled)
	{
		// Impaled.UpdateRotation owns actor pitch. A BAT pulse which was live
		// immediately before this edge is consumed, never deferred until release.
		CreaturePresentationPitch = 0.0f;
		CreaturePresentationPitchUpdatesRemaining = 0;
	}
}

void AWoTCharacter::ApplyPlayerMovementConstraint(
	EWoTPlayerMovementConstraint State)
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (!Move || State == EWoTPlayerMovementConstraint::None)
	{
		return;
	}
	if (State == EWoTPlayerMovementConstraint::LandingFreeze)
	{
		Move->StopMovementImmediately();
		return;
	}
	// Immobilized/Impaled BeginState zero both vectors. Grabbed's own BeginState
	// omits that reset because the Minion applies its pull impulse immediately.
	if (State != EWoTPlayerMovementConstraint::Grabbed)
	{
		Move->StopMovementImmediately();
	}
	// Immobilized.BeginState immediately leaves for PlayerSwimming when the
	// pawn is already in water. Do not manufacture a Falling frame first.
	if (Move->IsSwimming())
	{
		ExitPlayerMovementConstraint();
		return;
	}
	Move->SetMovementMode(MOVE_Falling);
}

void AWoTCharacter::ReconcileEffectiveConstraintFromServerCorrection()
{
	if (GetLocalRole() != ROLE_AutonomousProxy)
	{
		return;
	}
	const EWoTPlayerMovementConstraint OldState =
		EffectivePlayerMovementConstraint;
	// A position/velocity correction is not a second state transition. Re-entering
	// an already-active Impaled/Immobilized state calls StopMovementImmediately(),
	// erasing the authoritative external velocity that the correction just
	// delivered. Only reconcile when prediction and replicated authority differ.
	if (OldState == PlayerMovementConstraint)
	{
		return;
	}
	EffectivePlayerMovementConstraint = PlayerMovementConstraint;
	ApplyPlayerMovementConstraintTransition(OldState, PlayerMovementConstraint);
}

void AWoTCharacter::SetCreaturePresentationPitch(float PitchDegrees)
{
	if (!ensureMsgf(HasAuthority(),
		TEXT("Creature presentation pitch is authority-owned")))
	{
		return;
	}
	MulticastCreaturePresentationPitch(PitchDegrees);
	ForceNetUpdate();
}

void AWoTCharacter::MulticastCreaturePresentationPitch_Implementation(
	float PitchDegrees)
{
#if WITH_AUTOMATION_TESTS
	LastCreaturePresentationPitchForTest = PitchDegrees;
	++CreaturePresentationPitchObserveCountForTest;
#endif
	// While Impaled the clamp owns rotation, so HeadToss is discarded rather than
	// left stale. Otherwise retain it through exactly one normal presentation
	// update; the following update restores the ordinary actor pitch.
	CreaturePresentationPitchUpdatesRemaining = 0;
	if (GetEffectivePlayerMovementConstraint()
		== EWoTPlayerMovementConstraint::Impaled)
	{
		CreaturePresentationPitch = 0.0f;
		return;
	}
	CreaturePresentationPitch = PitchDegrees;
	CreaturePresentationPitchUpdatesRemaining = 1;
	ApplyCreaturePresentationPitch();
}

void AWoTCharacter::ClearCreaturePresentationPitch()
{
	if (FMath::IsNearlyZero(CreaturePresentationPitch))
	{
		return;
	}
	if (CreaturePresentationPitchUpdatesRemaining > 0)
	{
		--CreaturePresentationPitchUpdatesRemaining;
#if WITH_AUTOMATION_TESTS
		LastCreaturePresentationRetainedActorPitchForTest =
			GetActorRotation().Pitch;
		++CreaturePresentationPitchRetainCountForTest;
#endif
		return;
	}
	CreaturePresentationPitch = 0.0f;
	ApplyCreaturePresentationPitch();
#if WITH_AUTOMATION_TESTS
	LastCreaturePresentationClearedActorPitchForTest =
		GetActorRotation().Pitch;
	++CreaturePresentationPitchClearCountForTest;
#endif
	if (HasAuthority())
	{
		// Actor rotation is replicated independently. Do not leave a stale pitch
		// in the next movement bunch after the local presentation pulse clears.
		ForceNetUpdate();
	}
}

void AWoTCharacter::ApplyCreaturePresentationPitch()
{
	if (GetEffectivePlayerMovementConstraint()
		== EWoTPlayerMovementConstraint::Impaled)
	{
		return;
	}
	FRotator Rotation = GetActorRotation();
	Rotation.Pitch = CreaturePresentationPitch;
	SetActorRotation(Rotation);
}

void AWoTCharacter::ClearCreaturePitchAfterNormalViewUpdate()
{
	if (GetEffectivePlayerMovementConstraint()
		== EWoTPlayerMovementConstraint::Impaled)
	{
		return;
	}
	ClearCreaturePresentationPitch();
}

void AWoTCharacter::OnMovementModeChanged(EMovementMode PrevMovementMode,
	uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PrevMovementMode, PreviousCustomMode);
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	const bool bCanDrivePlayerTransition = HasAuthority() || IsLocallyControlled();
	if (!IsNPCPawn() && Move)
	{
		if (Move->IsFalling() && PrevMovementMode != MOVE_Falling
			&& !IsHardLanding())
		{
			if (PrevMovementMode != MOVE_Swimming)
			{
				RetailStandingBaseEyeHeight = BaseEyeHeight;
			}
			BaseEyeHeight = 0.7f * RetailStandingBaseEyeHeight;
			// WOTPlayer.Falling (WOTPlayer.uc:2268-2286) is the walk-off-
			// ledge path. During a jump, bPressedJump/bWasJumping is live and
			// OnJumped_Implementation supplies Jump instead, avoiding a spurious
			// Fall replication edge before it.
			if (bCanDrivePlayerTransition && !bPressedJump && !bWasJumping)
			{
				PlayPlayerTweenPose(FName(TEXT("Fall")));
			}
		}
		else if ((Move->IsMovingOnGround() || Move->IsSwimming())
			&& !IsHardLanding())
		{
			if (Move->IsSwimming())
			{
				BaseEyeHeight = 0.8f * RetailStandingBaseEyeHeight;
				if (bCanDrivePlayerTransition)
				{
					const FWoTAnimTableEntry* Pick = WoTAnimTable::PickSlotAnim(
						AnimTableName, FName(TEXT("Swim")), this);
					RetailSwimTweenSequence = Pick
						? Pick->AnimName : FName(TEXT("Swim"));
					RetailSwimLoopRate = WoTAnimTable::ResolveRate(Pick);
					LastSwimmingTweenSecondsForTest =
						WoTAnimTable::ResolveTweenTime(Pick);
					if (UWoTVertexAnimComponent* Anim =
						FindComponentByClass<UWoTVertexAnimComponent>())
					{
						Anim->OnAnimEnd.RemoveDynamic(this,
							&AWoTCharacter::HandleSwimmingTweenAnimEnd);
						Anim->OnAnimEnd.AddUniqueDynamic(this,
							&AWoTCharacter::HandleSwimmingTweenAnimEnd);
						Anim->TweenAnim(RetailSwimTweenSequence,
							LastSwimmingTweenSecondsForTest);
					}
					else if (HasAuthority())
					{
						SetTweenAnimIntent(RetailSwimTweenSequence, 0.0f,
							LastSwimmingTweenSecondsForTest, true);
					}
					HoldLocomotionFor(LastSwimmingTweenSecondsForTest);
				}
			}
			else
			{
				BaseEyeHeight = RetailStandingBaseEyeHeight;
				RetailSwimTweenSequence = NAME_None;
				if (UWoTVertexAnimComponent* Anim =
					FindComponentByClass<UWoTVertexAnimComponent>())
				{
					Anim->OnAnimEnd.RemoveDynamic(this,
						&AWoTCharacter::HandleSwimmingTweenAnimEnd);
				}
			}
		}
	}
	if (!IsNPCPawn()
		&& GetEffectivePlayerMovementConstraint()
			!= EWoTPlayerMovementConstraint::LandingFreeze
		&& IsPlayerMovementConstrained() && Move
		&& (Move->IsMovingOnGround() || Move->IsSwimming()))
	{
		ExitPlayerMovementConstraint();
	}
}

void AWoTCharacter::HandleSwimmingTweenAnimEnd(FName Sequence)
{
	if (!HasAuthority() && !IsLocallyControlled())
	{
		return;
	}
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	if (!Move || !Move->IsSwimming() || Sequence != RetailSwimTweenSequence)
	{
		return;
	}
	if (UWoTVertexAnimComponent* Anim =
		FindComponentByClass<UWoTVertexAnimComponent>())
	{
		Anim->OnAnimEnd.RemoveDynamic(this,
			&AWoTCharacter::HandleSwimmingTweenAnimEnd);
		Anim->LoopAnim(RetailSwimTweenSequence, RetailSwimLoopRate);
	}
	if (HasAuthority() || IsLocallyControlled())
	{
		DrivenLocomotionAnim = FName(TEXT("Swim"));
		DrivenLocomotionRateScale = 1.0f;
	}
	BaseEyeHeight = 0.5f * RetailStandingBaseEyeHeight;
}

void AWoTCharacter::TeleportSucceeded(bool bIsATest)
{
	Super::TeleportSucceeded(bIsATest);
	if (!bIsATest && GetWorld())
	{
		if (UWoTZoneIndex* Zones = GetWorld()->GetSubsystem<UWoTZoneIndex>())
		{
			Zones->UpdateActorZones(this);
		}
	}
	if (!bIsATest && !IsNPCPawn())
	{
		ExitPlayerMovementConstraint();
	}
}

bool AWoTCharacter::ShouldMakeLadderLandingNoise(
	const FVector& LandingVelocity, float NoiseVelocity)
{
	// Shipped code wins over LadderLandingNoiseVelocity's misleading comment:
	// WOTPlayer.uc:2350 uses VSize(Velocity) < threshold, not >.
	return LandingVelocity.Size() < NoiseVelocity;
}

void AWoTCharacter::LandedOnLadder()
{
	const FVector LandingVelocity = GetCharacterMovement()
		? GetCharacterMovement()->Velocity : FVector::ZeroVector;
	if (HasAuthority() && ShouldMakeLadderLandingNoise(
		LandingVelocity, LadderLandingNoiseVelocity))
	{
		EmitNoise(1.0f);
	}
	TweenToRunning(0.1f);
}

void AWoTCharacter::TweenToRunning(float TweenTime)
{
	const FWoTAnimTableEntry* Pick =
		WoTAnimTable::PickSlotAnim(AnimTableName, FName(TEXT("Run")));
	const FName Sequence = Pick ? Pick->AnimName : FName(TEXT("Run"));
	const float Rate = WoTAnimTable::ResolveRate(Pick);
	UWoTVertexAnimComponent* Anim =
		FindComponentByClass<UWoTVertexAnimComponent>();
	if (Anim && Anim->FindSequence(Sequence))
	{
		// TweenPlaySlotAnim's 0.1-second entry half. The locomotion driver is
		// held for the same interval, then stamps the normal looping Run intent.
		Anim->TweenAnim(Sequence, TweenTime);
	}
	else
	{
		// Headless/no-bound-mesh equivalent keeps the hook observable in specs.
		SetAnimIntent(Sequence, Rate, false);
	}
	if (HasAuthority() && GetWorld())
	{
		LocomotionHoldUntil = FMath::Max(
			LocomotionHoldUntil, GetWorld()->GetTimeSeconds() + TweenTime);
		DrivenLocomotionAnim = NAME_None;
	}
}

void AWoTCharacter::NotifyWalkSurface(int32 NodeIndex, float Friction,
	const FVector& HitLocation, const FVector& HitNormal)
{
	LastWalkSurfaceNode = NodeIndex;
	LastWalkSurfaceFriction = Friction;
	LastWalkSurfaceLocation = HitLocation;
	LastWalkSurfaceNormal = HitNormal;
}

void AWoTCharacter::TakeFallingDamage(const float VelocityZ)
{
	// WOTPlayer.Landed (WOTPlayer.uc:2362-2390), the normal-floor damage core.
	const float JumpZ = GetCharacterMovement()->JumpZVelocity;
	if (VelocityZ < -1.4f * JumpZ)
	{
		EmitNoise(-0.5f * VelocityZ / FMath::Max(JumpZ, 150.0f));
		if (HasAuthority() && VelocityZ <= -1100.0f)
		{
			float Damage;
			if (VelocityZ < -2000.0f)
			{
				// Guaranteed gib. (The shipped ReducedDamageType != 'All' bypass has no port
				// seam — divergence in PORT_NOTES; the reflector chain still reduces this.)
				Damage = Health + 250.0f;
			}
			else
			{
				Damage = -0.15f * (VelocityZ + 1050.0f);
			}
			FWoTDamageEvent Ev;
			Ev.WoTDamageType = TEXT("fell");
			Ev.UE1Momentum = FVector::ZeroVector;   // WOTPlayer.uc:2387
			Ev.HitInfo.ImpactPoint = GetActorLocation();
			TakeDamage(Damage, Ev, nullptr, nullptr);
		}
	}
}

void AWoTCharacter::AddUE1Velocity(FVector NewVelocity)
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (!IsValid(Move))
	{
		return;
	}

	// Engine Pawn.AddVelocity. LaunchCharacter(V,false,false)
	// would add the current velocity, but it also forces every movement mode to
	// Falling. Retail changes mode only from PHYS_Walking, so update the CMC
	// velocity directly and make just that transition explicit.
	if (Move->MovementMode == MOVE_Walking || Move->MovementMode == MOVE_NavWalking)
	{
		Move->SetMovementMode(MOVE_Falling);
	}
	if (Move->Velocity.Z > 380.0f && NewVelocity.Z > 0.0f)
	{
		NewVelocity.Z *= 0.5f;
	}
	Move->Velocity += NewVelocity;
}

int32 AWoTCharacter::ApplyDamageChain(int32 Damage, FName WoTDamageType,
                                      AController* EventInstigator, AActor* DamageCauser,
                                      const FVector& HitLocation,
                                      const FVector& UE1Momentum)
{
	// The WOTPlayer TakeDamage reflector chain, folded in priority order
	// (WOTPlayer.uc:1597 routes TakeDamage into CurrentReflector):
	//   element eat (255) -> shield split (225) -> damage notifier (64)
	//   -> DefaultTakeDamageReflector (0): game-mode ReduceDamage, then health.
	// Balefire's direct Invoke enters here too: UE's god flag holds for it.
	if (!CanBeDamaged())
	{
		return 0;
	}
	const bool bAlreadyDead = Health <= 0;
	// WOTPlayer.TakeDamage/DamageShake (uc:1593-1595,1643-1645) happens
	// before the reflector chain, so even a subsequently absorbed hit can shake
	// the owning player's view. PlayHit adds its damage-scaled shake below.
	if (Damage > 5)
	{
		if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(GetController()))
		{
			PC->ClientShakeView(0.2f, 2500.0f, -50.0f);
		}
	}

	bool bChainStopped = false;
	const int32 AfterFold = ReduceDamage(
		Damage, WoTDamageType, HitLocation, &bChainStopped);
	// IgnoreElementReflector and a fully absorbing ShieldReflector do not call
	// Super.TakeDamage in retail (IgnoreElementReflector.uc:75-86;
	// ShieldReflector.uc:78-131). Before momentum existed in this port their
	// continue-with-zero fold had the same health/blood/PlayHit outcome. The
	// explicit stop is what prevents the newly ported momentum from leaking past.
	if (bChainStopped)
	{
		return 0;
	}

	// LightningDamageNotifyer (Priority 64) sits BELOW the shields in the chain: a
	// hit the shields fully absorb never reaches it (ShieldReflector.uc:128 passes
	// down only if PlayerDamage > 0), and it fires BEFORE the chain bottom applies
	// the game-mode reduction. Attacker = the instigating pawn, else the causer.
	if (AfterFold > 0)
	{
		APawn* Attacker = Cast<APawn>(DamageCauser);
		if (EventInstigator && EventInstigator->GetPawn())
		{
			Attacker = EventInstigator->GetPawn();
		}
		if (Attacker && InventoryComponent)
		{
			TArray<TObjectPtr<AWoTInventory>> Items = InventoryComponent->Items;
			for (const TObjectPtr<AWoTInventory>& Item : Items)
			{
				if (AWoTAngrealInventory* Ang = Cast<AWoTAngrealInventory>(Item))
				{
					Ang->NotifyOwnerDamaged(Attacker);
				}
			}
		}
	}

	// Engine Pawn.TakeDamage, shipped bytecode probe (GUARDED),
	// structure matching the original engine: momentum happens after the reflector
	// chain reaches the bottom but BEFORE game-mode ReduceDamage/Health. UE1's
	// PHYS_None -> SetMovementPhysics line has no seam here: every AWoTCharacter
	// always owns a CharacterMovement mode.
	FVector Momentum = UE1Momentum;
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		if (Move->IsMovingOnGround())
		{
			Momentum.Z = FMath::Max(Momentum.Z, 0.4f * Momentum.Size());
		}
		if (EventInstigator && EventInstigator->GetPawn() == this)
		{
			Momentum *= 0.6f;
		}
		if (Move->Mass > 0.0f)
		{
			Momentum /= Move->Mass;
		}
		else
		{
			Momentum = FVector::ZeroVector;   // invalid-Mass tolerance
		}
		if (!Momentum.IsZero())
		{
			AddUE1Velocity(Momentum);
		}
	}

	// DefaultTakeDamageReflector -> Pawn.TakeDamage: the
	// game-mode reduction — the Battle mode's friendly-fire / neutral /
	// editing-phase rules — runs at the BOTTOM of the chain, AFTER the pawn's own
	// shields (retail drains a shield pool by the raw damage, then scales what's
	// left). Base mode returns Damage unchanged.
	int32 Actual = AfterFold;
	if (AWoTGameMode* Game = GetWorld() ? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr)
	{
		APawn* Inst = EventInstigator ? EventInstigator->GetPawn() : nullptr;
		Actual = Game->ReduceDamage(Actual, WoTDamageType, this, Inst);
	}

	Health -= Actual;
	// Engine.Pawn.TakeDamage: a killing blow larger than Mass records the full
	// overkill as negative health before Died/Gibbed evaluates its thresholds.
	if (Health <= 0 && Actual > 0)
	{
		const UCharacterMovementComponent* Move = GetCharacterMovement();
		if (Move && static_cast<float>(Actual) > Move->Mass)
		{
			Health = -Actual;
		}
	}
	if (Actual > 0 && Health > 0)
	{
		if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(GetController()))
		{
			const float Rnd = FMath::Clamp(static_cast<float>(Actual), 20.0f, 40.0f);
			if (!WoTDamageType.ToString().Contains(TEXT("NoFlash"), ESearchCase::IgnoreCase))
			{
				float FlashScale = -0.019f * Rnd;
				FVector FlashFog = Rnd * FVector(26.5f, 4.5f, 4.5f);
				if (AWoTAngrealInventory::DamageTypeContains(
					WoTDamageType, TEXT("Fire")))
				{
					FlashScale = -0.009375f * Rnd;
					FlashFog = Rnd * FVector(16.41f, 11.719f, 4.6875f);
				}
				else if (AWoTAngrealInventory::DamageTypeContains(
					WoTDamageType, TEXT("Air")))
				{
					FlashScale = -0.390f;
					FlashFog = FVector(312.5f, 468.75f, 468.75f);
				}
				else if (AWoTAngrealInventory::DamageTypeContains(
					WoTDamageType, TEXT("Earth")))
				{
					FlashScale = -0.01171875f * Rnd;
					FlashFog = Rnd * FVector(9.375f, 14.0625f, 4.6875f);
				}
				else if (AWoTAngrealInventory::DamageTypeContains(
						WoTDamageType, TEXT("Water"))
					|| AWoTAngrealInventory::DamageTypeContains(
						WoTDamageType, TEXT("Spirit")))
				{
					FlashScale = -0.390f;
					FlashFog = FVector(312.5f, 468.75f, 468.75f);
				}
				PC->ClientFlash(FlashScale, FlashFog);
			}
			PC->ClientShakeView(0.15f + 0.005f * Actual,
				static_cast<float>(Actual) * 30.0f, 0.3f * Actual);
		}
	}

	// Dying.TakeDamage (WOTPawn.uc:892-915): enough overkill mid-death-anim gibs
	// INSTEAD of playing the damage effect (the shipped else-branch split).
	const bool bOverkillGib =
	    bAlreadyDead && bDead && !bCarcassSpawned && Actual > 0 &&
	    Health < (40.0f /*WOTPawn BaseGibDamage*/ / Actual) * GibForSureFinalHealth;

	// The damage blood spurt (GenericDamageHelper.DoDamageEffects, uc:100-127):
	// spray 45-degrees-up along center->hit, spawned halfway out toward the hit,
	// volume scaled by clamped damage / BaseDamageLevel 25. Every damaging hit
	// bleeds (surviving, lethal, and post-death carcass-window hits alike) —
	// except Drowned, which maps to NO blood effect (GenericDamageHelper.uc:247;
	// retail spawns a BubbleSpawner instead when the victim's head is underwater —
	// bubbles deferred, PORT_NOTES).
	if (Actual > 0 && !bOverkillGib && WoTDamageType != FName(TEXT("Drowned")) &&
	    GetWorld() && !GetWorld()->bIsTearingDown)
	{
		const FVector EffectVector = HitLocation - GetActorLocation();
		FVector Dir = EffectVector.GetSafeNormal();
		if (Dir.IsNearlyZero())
		{
			Dir = -GetActorForwardVector();   // no hit location: bleed toward the front
		}
		FVector SprayDir = Dir;
		SprayDir.Z = 0.707f;   // bias the burst upward by 45 degrees (uc:106)
		WoTBloodFX::SpawnBurstMarker(
		    *GetWorld(),
		    FName(bBlackBlood ? TEXT("BloodDamageBlack1") : TEXT("BloodDamageRed1")),
		    GetActorLocation() + 0.5f * EffectVector.Size() * Dir,
		    SprayDir.Rotation(),
		    FMath::Clamp(static_cast<float>(Actual), 0.0f, 500.0f) / 25.0f);
	}

	// GenericDamageHelper.uc:131-134 (DoDamageEffects tail): ANY damaging hit
	// with the victim's head underwater bubbles from the mouth — in ADDITION to
	// blood (Drowned just sheds none); the overkill-gib branch skips the damage
	// effects entirely.
	if (Actual > 0 && !bOverkillGib && GetWorld() && !GetWorld()->bIsTearingDown &&
	    AWoTZoneVolume::IsPointInWater(
	        GetWorld(), GetActorLocation() + FVector(0.0f, 0.0f, BaseEyeHeight)))
	{
		const FVector Mouth = GetActorLocation() +
		                      GetActorForwardVector() * GetSimpleCollisionRadius() +
		                      FVector(0.0f, 0.0f, BaseEyeHeight);
		WoTBubbles::SpawnBurst(*GetWorld(), Mouth, GetActorRotation());
	}

	if (Health <= 0 && !bAlreadyDead)
	{
		Died(EventInstigator, WoTDamageType, HitLocation);
	}
	else if (Health > 0)
	{
		// Pawn.TakeDamage plays the survivor's hit reaction after health lands
		// even for Damage=0 (DecreaseHealthLeech's pain-only DamageEffect).
		// WOTPawn.PlayHit receives the post-Mass-divide Momentum.Z (uc:1019).
		PlayHitReactAnim(Actual, WoTDamageType, Momentum.Z,
			/*bEnforceProductionEligibility*/ true);
	}
	else if (bOverkillGib)
	{
		SpawnCarcassNow(/*bGibbed*/ true);
	}

	return Actual;
}

int32 AWoTCharacter::TakeRawDamage(int32 Damage, FName WoTDamageType,
                                   AController* EventInstigator, const FVector& HitLocation,
                                   const FVector& UE1Momentum)
{
	// Balefire "breaks all rules": the original calls DamageEffect.Invoke()
	// directly, skipping ONLY ProcessEffect — the delivery-side interception
	// (anti-magic dome, element blocks, Absorb). Invoke() is a full TakeDamage
	// (DamageEffect.uc:56-62), so the whole reflector chain — Personal Shield
	// split, element eat by damage-type letters, notifier, game-mode reduction —
	// still applies (AngrealInvBalefire.uc:316-322).
	if (!HasAuthority())
	{
		return 0;
	}
	return ApplyDamageChain(
		Damage, WoTDamageType, EventInstigator, nullptr, HitLocation, UE1Momentum);
}

void AWoTCharacter::IncreaseHealth(int32 Amount)
{
	// DefaultHealthReflector: only heal the living, clamp to the cap.
	if (Health > 0)
	{
		Health = FMath::Min(Health + Amount, MaxHealth);
	}
}

void AWoTCharacter::GetOrderedReflectors(
	TArray<UWoTReflector*>& OutReflectors) const
{
	OutReflectors.Reset();
	GetComponents<UWoTReflector>(OutReflectors);
	// Owned != installed: a NewObject'd reflector sits in OwnedComponents before
	// Install() ever runs (UActorComponent::PostInitProperties adds it). Retail's
	// chain only contains INSTALLED reflectors — without the registration filter
	// an uninstalled Absorb could consume a ProcessEffect.
	OutReflectors.RemoveAll([](const UWoTReflector* Reflector)
	{
		return !IsValid(Reflector) || !Reflector->IsRegistered();
	});
	OutReflectors.Sort([](const UWoTReflector& A, const UWoTReflector& B)
	{
		if (A.Priority != B.Priority)
		{
			return A.Priority > B.Priority;
		}
		return A.InstallSeq > B.InstallSeq;
	});
}

int32 AWoTCharacter::ReduceDamage(int32 Damage, FName WoTDamageType,
	                              const FVector& /*HitLocation*/,
	                              bool* bOutChainStopped)
{
	// Fold the reflector chain's TakeDamage interception: each installed
	// reflector may cut the damage (element shields zero their element; the
	// hit-point shield absorbs a share). Highest Priority runs first.
	TArray<UWoTReflector*> Reflectors;
	GetOrderedReflectors(Reflectors);
	if (bOutChainStopped)
	{
		*bOutChainStopped = false;
	}
	for (UWoTReflector* R : Reflectors)
	{
		if (IsValid(R))
		{
			Damage = R->ModifyDamage(Damage, WoTDamageType);
			if (R->StopsDamageChain(Damage))
			{
				if (bOutChainStopped)
				{
					*bOutChainStopped = true;
				}
				break;
			}
		}
	}
	return Damage;
}

bool AWoTCharacter::IsEffectBlocked(const AWoTAngrealInventory* Source, bool bDeleterious,
	bool bEmitFeedback)
{
	TArray<UWoTReflector*> Reflectors;
	GetOrderedReflectors(Reflectors);
	for (UWoTReflector* R : Reflectors)
	{
		if (IsValid(R) && R->IgnoresEffect(Source, bDeleterious))
		{
			if (bEmitFeedback)
			{
				R->OnEffectIgnoredFeedback();
			}
			return true;
		}
	}
	return false;
}

int32 AWoTCharacter::ProcessEffect(const FWoTDamageEffectSpec& Effect)
{
	if (!HasAuthority())
	{
		return 0;
	}

	// Retail is one CurrentReflector walk: each reflector blocks, consumes, or
	// passes before DefaultProcessEffectReflector invokes DamageEffect.TakeDamage.
	// The shipped 255 blockers remain above Absorb 170, so this is observably
	// equivalent to the former blockers-only sweep for all existing reflectors
	// while giving Absorb its priority-correct consume seat.
	TArray<UWoTReflector*> Reflectors;
	GetOrderedReflectors(Reflectors);
	for (UWoTReflector* Reflector : Reflectors)
	{
		if (!IsValid(Reflector))
		{
			continue;
		}
		if (Reflector->IgnoresEffect(Effect.SourceAngreal, Effect.bDeleterious))
		{
			Reflector->OnEffectIgnoredFeedback();
			return 0;
		}
		if (Reflector->ConsumesEffect(Effect))
		{
			return 0;
		}
	}

	FWoTDamageEvent Event;
	Event.WoTDamageType = Effect.DamageType;
	Event.UE1Momentum = Effect.UE1Momentum;
	Event.HitInfo.ImpactPoint = Effect.HitLocation;
	Event.Damage = static_cast<float>(Effect.Damage);
	AController* EffectInstigator = IsValid(Effect.InstigatorPawn)
		? Effect.InstigatorPawn->GetController() : nullptr;
	AActor* DamageCauser = nullptr;
	if (IsValid(Effect.SourceProjectile))
	{
		DamageCauser = Effect.SourceProjectile;
	}
	else if (IsValid(Effect.SourceAngreal))
	{
		DamageCauser = Effect.SourceAngreal;
	}
	return FMath::TruncToInt(TakeDamage(
		static_cast<float>(Effect.Damage), Event, EffectInstigator, DamageCauser));
}

bool AWoTCharacter::IsCastingBlocked() const
{
	TArray<UWoTReflector*> Reflectors;
	GetOrderedReflectors(Reflectors);
	for (const UWoTReflector* R : Reflectors)
	{
		if (IsValid(R) && R->BlocksCasting())
		{
			return true;
		}
	}
	return false;
}

void AWoTCharacter::NotifyChargeUsed(AWoTAngrealInventory* Ang)
{
	// AngrealUser.ChargeUsed runs before the inherited reflector fan-out. Keep
	// the NPC adapter callback synchronous with the real inventory expenditure.
	if (IsNPCPawn())
	{
		if (UWoTCasterComponent* Caster = FindComponentByClass<UWoTCasterComponent>())
		{
			Caster->NotifyChargeUsed(Ang);
		}
	}
	TArray<UWoTReflector*> Reflectors;
	GetOrderedReflectors(Reflectors);
	for (UWoTReflector* R : Reflectors)
	{
		if (IsValid(R))
		{
			R->OnChargeUsed(Ang);
		}
	}
}

void AWoTCharacter::RefreshNPCLocomotionAnimation()
{
	if (!IsNPCPawn())
	{
		return;
	}
	LocomotionHoldUntil = 0.0f;
	DrivenLocomotionAnim = NAME_None;
}

void AWoTCharacter::SuspendNPCAnimationForIce()
{
	if (!IsNPCPawn() || bNPCAnimationSuspendedForIce)
	{
		return;
	}
	bNPCAnimationSuspendedForIce = true;
	DrivenLocomotionAnim = NAME_None;
	if (UWoTVertexAnimComponent* Anim = Cast<UWoTVertexAnimComponent>(GetMesh()))
	{
		const FName Sequence = Anim->GetCurrentSequence().IsNone()
			? RepAnim.Sequence : Anim->GetCurrentSequence();
		if (!Sequence.IsNone())
		{
			const float HeldFrame = FMath::Clamp(Anim->GetAnimFrame(), 0.0f, 1.0f);
			Anim->SetFrameAndStop(Sequence, HeldFrame);
			SetAnimIntent(Sequence, 0.0f, false);
			RepAnim.HeldFrame = static_cast<uint16>(FMath::RoundToInt(
				HeldFrame * static_cast<float>(MAX_uint16)));
			RepAnim.bHoldFrame = 1;
		}
	}
}

void AWoTCharacter::ResumeNPCAnimationAfterIce()
{
	if (!bNPCAnimationSuspendedForIce)
	{
		return;
	}
	bNPCAnimationSuspendedForIce = false;
	RefreshNPCLocomotionAnimation();
}

bool AWoTCharacter::HasTargetingOverride() const
{
	TArray<UWoTReflector*> Reflectors;
	GetOrderedReflectors(Reflectors);
	for (const UWoTReflector* R : Reflectors)
	{
		if (IsValid(R) && R->OverridesTargeting())
		{
			return true;
		}
	}
	return false;
}

bool AWoTCharacter::ReflectsProjectiles() const
{
	TArray<UWoTReflector*> Reflectors;
	GetOrderedReflectors(Reflectors);
	for (const UWoTReflector* R : Reflectors)
	{
		if (IsValid(R) && R->ReflectsProjectiles())
		{
			return true;
		}
	}
	return false;
}

bool AWoTCharacter::ForksProjectiles() const
{
	TArray<UWoTReflector*> Reflectors;
	GetOrderedReflectors(Reflectors);
	for (const UWoTReflector* R : Reflectors)
	{
		if (IsValid(R) && R->ForksProjectiles())
		{
			return true;
		}
	}
	return false;
}

bool AWoTCharacter::AbsorbProjectile(AWoTAngrealProjectile* Proj)
{
	TArray<UWoTReflector*> Reflectors;
	GetOrderedReflectors(Reflectors);
	for (UWoTReflector* R : Reflectors)
	{
		if (IsValid(R) && R->IsProjectileAffectable(Proj)
			&& R->OnHitByProjectile(Proj))
		{
			return true;   // absorbed
		}
	}
	return false;
}

void AWoTCharacter::Died(AController* Killer, FName WoTDamageType,
                         const FVector& /*HitLocation*/)
{
	if (bDead || !HasAuthority())
	{
		return;
	}
	SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	bDead = true;
	bGibbedDeath = Health < GibForSureFinalHealth
		|| (Health < GibSometimesFinalHealth && WoTRandom::FRand() < 0.5f);

	// WOTPlayer.Died -> CancelAngrealEffects (WOTPlayer.uc:3480-3489, 3865-3912):
	// Reset() every held angreal — a sustained cast ends with its caster (a dead
	// Chain Lightning caster's aura stops; a charging Balefire never auto-fires) —
	// and strip every removable leech and reflector from the corpse.
	if (InventoryComponent)
	{
		TArray<TObjectPtr<AWoTInventory>> Items = InventoryComponent->Items;
		for (const TObjectPtr<AWoTInventory>& Item : Items)
		{
			if (AWoTAngrealInventory* Ang = Cast<AWoTAngrealInventory>(Item))
			{
				Ang->Reset();
			}
		}
	}
	TArray<UWoTLeech*> Leeches;
	GetComponents<UWoTLeech>(Leeches);
	for (UWoTLeech* L : Leeches)
	{
		if (IsValid(L) && L->bRemovable)
		{
			L->Unattach();
		}
	}
	TArray<UWoTReflector*> DeathReflectors;
	GetComponents<UWoTReflector>(DeathReflectors);
	for (UWoTReflector* R : DeathReflectors)
	{
		if (IsValid(R) && R->bRemovable)
		{
			R->Uninstall();
		}
	}

	// WOTPlayer.Died: seals leave the pawn before seeker reset and before the
	// remaining inventory is detached into one multiplayer bag.
	DropAllSeals();
	SeekerCount = 0;

	if (InventoryComponent)
	{
		AWoTBagHolding* Bag = nullptr;
		const bool bDropBag = GetNetMode() != NM_Standalone;
		const TArray<TObjectPtr<AWoTInventory>> RemainingItems =
			InventoryComponent->Items;
		for (AWoTInventory* Item : RemainingItems)
		{
			if (!IsValid(Item))
			{
				continue;
			}
			InventoryComponent->DeleteInventory(Item);
			const bool bBagEligible = bDropBag
				&& Item->GetClass() != DefaultAngrealInventory
				&& !Item->IsA<AWoTAngrealInvSpecial>();
			if (!bBagEligible)
			{
				Item->Destroy();
				continue;
			}
			if (AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Item))
			{
				Angreal->CurCharges = FMath::Min(
					Angreal->CurCharges, Angreal->MaxInitialCharges);
			}
			if (!Bag)
			{
				const FTransform BagTransform(
					FRotator::ZeroRotator, GetActorLocation());
				Bag = GetWorld()->SpawnActorDeferred<AWoTBagHolding>(
					AWoTBagHolding::StaticClass(), BagTransform, nullptr, nullptr,
					ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
				if (Bag)
				{
					// UE1's Spawn does not generate an intermediate Touch at the
					// carrier's origin. Finish this temporary bag held so its live
					// pickup collision begins only when DropFrom moves it forward.
					Bag->bHeldItem = true;
					Bag->FinishSpawning(BagTransform);
				}
			}
			if (!Bag || !Bag->AddItem(Item))
			{
				Item->Destroy();
			}
		}
		if (Bag)
		{
			const float Distance = Bag->GetPickupCollisionRadius()
				+ GetCapsuleComponent()->GetScaledCapsuleRadius();
			Bag->DropFrom(GetActorLocation() + GetActorForwardVector() * Distance);
		}
		InventoryComponent->SelectedItem = nullptr;
	}
	// Stop the corpse; the death animation and ragdoll/gib land with the death-anim
	// pass. This half runs client-side too, via OnRep_bDead.
	ShutdownCorpsePresentation();
	if (bGibbedDeath)
	{
		SpawnCarcassNow(/*bGibbed*/ true);
	}

	// Pawn.Died -> Level.Game.Killed: hand the kill to the rules for scoring + respawn.
	if (AWoTGameMode* Game = GetWorld()->GetAuthGameMode<AWoTGameMode>())
	{
		Game->PlayerKilled(Killer, this, WoTDamageType);
	}
	if (bGibbedDeath && IsNPCPawn() && !IsPendingKillPending())
	{
		Destroy();
	}
}

int32 AWoTCharacter::HealthIconIndex(int32 InHealth, int32 InDefaultHealth)
{
	check(InDefaultHealth != 0);
	return 3 - FMath::Clamp(4 * InHealth / InDefaultHealth, 0, 3);
}

UTexture2D* AWoTCharacter::GetHealthIcon() const
{
	// WOTPlayer.GetHealthIcon (uc:2969-2977): while disguised the health icon is the
	// mimicked class's DisguiseIcon (ApparentIcon = GetDisguiseIcon, uc:366/375). The
	// driver already carries the source class on every machine, so derive it here — no
	// separate ApparentIcon field. Missing DisguiseIcon content falls through to the
	// real health set. The HUD consumes this result directly in its health-icon pass.
	if (Disguise.bActive)
	{
		FSoftObjectPath IconPath = Disguise.SourceDisguiseIcon;
		// SourceDisguiseIcon is the replicated live-NPC path. Retain the class
		// fallback for class-authored player disguises and older saved/net state.
		if (IconPath.IsNull() && Disguise.SourceClass)
		{
			IconPath = Disguise.SourceClass->GetDefaultObject<AWoTCharacter>()
				->DisguiseIcon.ToSoftObjectPath();
		}
		if (UTexture2D* Icon = UWoTRuntimeAssets::ResolveTexturePath(IconPath))
		{
			return Icon;
		}
	}
	// MaxHealth is the authenticated inherited WOTPlayer default stamped on this
	// instance before Super::BeginPlay. The native CDO intentionally remains zero:
	// consulting it here would reintroduce the removed literal fallback.
	const int32 HealthIconSlot = HealthIconIndex(Health, MaxHealth);
	return UWoTRuntimeAssets::ResolveTexturePath(
		HealthIcons[HealthIconSlot].ToSoftObjectPath());
}

void AWoTCharacter::AddIconInfo(UObject* Source, bool bGood, FName IconID)
{
	if (!HasAuthority())
	{
		return;
	}
	if (!IsValid(Source))
	{
		UE_LOG(LogTemp, Warning, TEXT("Status icon source is invalid."));
		return;
	}

	FName IconTexture = NAME_None;
	float InitialDuration = 0.0f;
	EStatusIconSourceKind SourceKind = EStatusIconSourceKind::Angreal;
	if (UWoTLeech* Leech = Cast<UWoTLeech>(Source))
	{
		SourceKind = EStatusIconSourceKind::Leech;
		if (!Leech->StatusIconFrame.IsNull())
		{
			IconTexture = WoTStatusIconTextureName(Leech->StatusIconFrame);
		}
		else if (IsValid(Leech->SourceAngreal))
		{
			IconTexture = WoTStatusIconTextureName(Leech->SourceAngreal->StatusIconFrame);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT(
				"Attached effect has no icon or originating artifact; skipping its status icon."));
			return;
		}
		InitialDuration = Leech->GetInitialDuration();
	}
	else if (UWoTReflector* Reflector = Cast<UWoTReflector>(Source))
	{
		SourceKind = EStatusIconSourceKind::Reflector;
		if (!Reflector->StatusIconFrame.IsNull())
		{
			IconTexture = WoTStatusIconTextureName(Reflector->StatusIconFrame);
		}
		else if (IsValid(Reflector->SourceAngreal))
		{
			IconTexture = WoTStatusIconTextureName(
				Reflector->SourceAngreal->StatusIconFrame);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT(
				"Reflective effect has no icon or originating artifact; skipping its status icon."));
			return;
		}
		InitialDuration = Reflector->GetInitialDuration();
	}
	else if (AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Source))
	{
		SourceKind = EStatusIconSourceKind::Angreal;
		IconTexture = WoTStatusIconTextureName(Angreal->StatusIconFrame);
		if (IconTexture.IsNone())
		{
			// Retail warns here but, unlike the leech/reflector branches, does not
			// return (WOTPlayer.uc:1703-1715).
			UE_LOG(LogTemp, Warning,
				TEXT("Artifact status icon request has no icon frame."));
		}
		InitialDuration = Angreal->GetDuration();
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Status icon source type is unsupported."));
		return;
	}

	if (IconID.IsNone())
	{
		UE_LOG(LogTemp, Warning, TEXT("Status icon request has no identifier."));
		return;
	}

	const int32 ExistingIndex = StatusIcons.IndexOfByPredicate(
		[IconID](const FWoTStatusIconEntry& Entry)
		{
			return Entry.IconID == IconID;
		});
	FWoTStatusIconEntry Entry;
	Entry.IconID = IconID;
	Entry.IconTexture = IconTexture;
	Entry.bGood = bGood;
	Entry.InitialDuration = InitialDuration;
	Entry.RemainingDuration = InitialDuration;
	FStatusIconSourceRef SourceRef;
	SourceRef.Source = Source;
	SourceRef.Kind = SourceKind;
	if (ExistingIndex == INDEX_NONE)
	{
		StatusIcons.Insert(Entry, 0);
		StatusIconSources.Insert(SourceRef, 0);
	}
	else
	{
		StatusIcons[ExistingIndex] = Entry;
		if (StatusIconSources.IsValidIndex(ExistingIndex))
		{
			StatusIconSources[ExistingIndex] = SourceRef;
		}
		else
		{
			StatusIconSources.SetNum(StatusIcons.Num());
			StatusIconSources[ExistingIndex] = SourceRef;
		}
	}
	ForceNetUpdate();
}

void AWoTCharacter::RemoveIconInfo(FName IconID)
{
	if (!HasAuthority())
	{
		return;
	}
	const int32 Index = StatusIcons.IndexOfByPredicate(
		[IconID](const FWoTStatusIconEntry& Entry)
		{
			return Entry.IconID == IconID;
		});
	if (Index == INDEX_NONE)
	{
		return;
	}
	StatusIcons.RemoveAt(Index);
	if (StatusIconSources.IsValidIndex(Index))
	{
		StatusIconSources.RemoveAt(Index);
	}
	ForceNetUpdate();
}

void AWoTCharacter::UpdateStatusIconDurations()
{
	for (int32 Index = 0; Index < StatusIcons.Num(); ++Index)
	{
		float RemainingDuration = 0.0f;
		if (StatusIconSources.IsValidIndex(Index))
		{
			const FStatusIconSourceRef& Ref = StatusIconSources[Index];
			UObject* Source = Ref.Source.Get();
			if (IsValid(Source))
			{
				switch (Ref.Kind)
				{
				case EStatusIconSourceKind::Angreal:
					if (const AWoTAngrealInventory* Angreal =
						Cast<AWoTAngrealInventory>(Source))
					{
						RemainingDuration = Angreal->GetDuration();
					}
					break;
				case EStatusIconSourceKind::Leech:
					if (const UWoTLeech* Leech = Cast<UWoTLeech>(Source))
					{
						RemainingDuration = Leech->GetDuration();
					}
					break;
				case EStatusIconSourceKind::Reflector:
					if (const UWoTReflector* Reflector = Cast<UWoTReflector>(Source))
					{
						RemainingDuration = Reflector->GetDuration();
					}
					break;
				}
			}
		}
		StatusIcons[Index].RemainingDuration = RemainingDuration;
	}
}

void AWoTCharacter::IncrementSeekerCount()
{
	if (!HasAuthority())
	{
		return;
	}
	++SeekerCount;
}

void AWoTCharacter::DecrementSeekerCount()
{
	if (!HasAuthority())
	{
		return;
	}
	SeekerCount = FMath::Max(0, SeekerCount - 1);
}

void AWoTCharacter::ShutdownCorpsePresentation()
{
	// The client-safe half of Died(): halt the corpse and drop its collision. Runs
	// authority-side (from Died) and on simulated proxies (from OnRep_bDead), so a
	// killed pawn stops being a colliding, mobile corpse on every machine. The
	// death-instant velocity is captured first — the carcass inherits the tumble the
	// halt erases (the corpse-halt divergence closes at the carcass).
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		DeathVelocity = Move->Velocity;
		Move->StopMovementImmediately();
		Move->DisableMovement();
	}
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	// The hum hands off to the carcass (which inherits the sound; retail let the
	// hidden dead pawn keep humming beside it for its linger window — PORT_NOTES).
	if (AmbientHum)
	{
		AmbientHum->Stop();
	}
	if (bGibbedDeath)
	{
		return;
	}
	// Death anim (mirrors PlayDeathHit, WOTPawn.uc:812-816): the authority picks the
	// 'Death' slot variant — weighted odds, geometry-filtered by AnimEndVector so a
	// corpse doesn't fall into a wall — and plays it; proxies replay the concrete pick
	// via OnRep_Anim. Without a table (bare pawns / headless) the literal "Death"
	// stand-in stays. RepAnim skips the owning connection, so a dying player's own
	// client picks and plays its own variant (presentation only; the pick may differ
	// from the one the others see). Other proxies do nothing here.
	const bool bOwningClient = !HasAuthority() && IsLocallyControlled();
	if (HasAuthority() || bOwningClient)
	{
		// PlayDeathHit's cry (WOTPawn.uc:812-830): DieHard on an exceptional death
		// (Health < -0.25 x default, ExceptionalDeathHealthRatio :1558) else DieSoft;
		// a gib-worthy death (WOTGibbed, WOTUtil.uc:493-507) dies silent in MP.
		if (HasAuthority())
		{
			PlayVoiceSlot(Health < -0.25f * HitReferenceHealth ? FName(TEXT("DieHard"))
			                                                   : FName(TEXT("DieSoft")));
		}

		FName DeathSlot(TEXT("Death"));
		bool bPlayDeathWithoutTween = false;
		if (const AWoTNPCController* NPCController =
			Cast<AWoTNPCController>(GetController());
			NPCController && NPCController->IsArcherKneeling())
		{
			// PerformProjectileAttack.PlayDeathHit always uses the kneeling variant
			// (Archer.uc:138-141), even if the ordinary geometry-filtered death pick
			// would have selected a standing fall.
			DeathSlot = FName(TEXT("DeathKneel"));
			bPlayDeathWithoutTween = true; // Archer.uc:138-141 uses PlaySlotAnim
		}
		const FWoTAnimTableEntry* Pick = WoTAnimTable::PickSlotAnim(
		    AnimTableName, DeathSlot, this);
		const FName DeathAnim = Pick ? Pick->AnimName : FName(TEXT("Death"));
		const float DeathRate = WoTAnimTable::ResolveRate(Pick);
		const float DeathTweenTime = bPlayDeathWithoutTween
			? 0.0f : WoTAnimTable::ResolveTweenTime(Pick);
		// Play the pick on the vertex-anim component, whose authority seam stamps the
		// replicated intent (as StartHardLandingPresentation does): authority never
		// receives its own OnRep_Anim, so stamping the intent alone left the corpse
		// frozen in its last pose on a listen host or standalone game until the carcass
		// swap snapped it to the floor.
		UWoTVertexAnimComponent* DeathBody = FindComponentByClass<UWoTVertexAnimComponent>();
		if (DeathBody && DeathBody->FindSequence(DeathAnim))
		{
			if (bPlayDeathWithoutTween)
			{
				DeathBody->PlayAnim(DeathAnim, DeathRate);
			}
			else
			{
				DeathBody->TweenPlayAnim(DeathAnim, DeathRate, DeathTweenTime);
			}
		}
		else if (bPlayDeathWithoutTween)
		{
			SetAnimIntent(DeathAnim, DeathRate, false);
		}
		else
		{
			SetTweenAnimIntent(DeathAnim, DeathRate, DeathTweenTime, false);
		}
		if (bOwningClient)
		{
			return;
		}

		// Carcass swap: fire at the death sequence's carcass notify —
		// TransitionToCarcassNotification's authored fraction — with the AnimEnd
		// fallback when the sequence has none (WOTPawn.uc:854-927). A pawn with no
		// bound death sequence (bare spec pawns) never swaps: there is no display
		// to copy, and the hidden actor would be all that remained (PORT_NOTES).
		const float Duration = SequenceDurationSeconds(DeathAnim, DeathRate);
		DeathAnimStartTime = GetWorld()->GetTimeSeconds() + DeathTweenTime;
		DeathAnimDuration = Duration;
		if (Duration > 0.0f && !bCarcassSpawned)
		{
			float Fraction = 1.0f;   // AnimEnd fallback
			if (const UWoTVertexAnimComponent* Anim =
			        Cast<UWoTVertexAnimComponent>(GetMesh()))
			{
				const float Authored = Anim->FindNotifyFraction(
				    DeathAnim, FName(TEXT("TransitionToCarcassNotification")));
				if (Authored >= 0.0f)
				{
					Fraction = Authored;
				}
			}
			GetWorldTimerManager().SetTimer(
			    CarcassSwapTimer,
			    FTimerDelegate::CreateWeakLambda(this, [this] { SpawnCarcassNow(); }),
			    FMath::Max(DeathTweenTime + Fraction * Duration, 0.01f), false);
		}
	}
}

float AWoTCharacter::GetDeathPoseFraction() const
{
	if (DeathAnimDuration <= 0.0f || !GetWorld())
	{
		return 1.0f;
	}
	return FMath::Clamp(
	    (GetWorld()->GetTimeSeconds() - DeathAnimStartTime) / DeathAnimDuration, 0.0f, 1.0f);
}

void AWoTCharacter::SpawnCarcassNow(bool bGibbed)
{
	// The teardown guard matters: Destroyed() runs for every dead pawn when a test
	// world unwinds — spawning into a dying world would leak/crash. A pawn with no
	// bound body mesh has no display to copy — no carcass (bare spec pawns).
	const UWoTVertexAnimComponent* Display =
		Cast<UWoTVertexAnimComponent>(GetMesh());
	if (!HasAuthority() || bCarcassSpawned || !GetWorld() || GetWorld()->bIsTearingDown ||
	    !Display || !Display->HasDisplayMesh())
	{
		return;
	}
	bCarcassSpawned = true;
	GetWorldTimerManager().ClearTimer(CarcassSwapTimer);

	// DoSpawnCarcass (WOTPawn.uc:1148-1167) + PawnToCarcassTransition (:864-881):
	// spawn, copy display/physics, chunk immediately on the overkill path, then hide
	// this pawn (bHidden replicates; collision/movement are already off). The hidden
	// actor lingers DestroyDelaySecs for NPCs (the dying-sound window) — a player is
	// destroyed when its eligible Fire consumes the respawn gate.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
	    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	FRotator YawOnly = GetActorRotation();
	YawOnly.Roll = 0.0f;
	YawOnly.Pitch = 0.0f;
	if (AWoTCarcass* Carc = GetWorld()->SpawnActor<AWoTCarcass>(
	        GetActorLocation(), YawOnly, Params))
	{
		Carc->InitFor(*this);
		if (bGibbed)
		{
			Carc->ChunkUp(FMath::Max(0, -Health));
		}
	}
	SetActorHiddenInGame(true);

	if (IsNPCPawn() && !IsPendingKillPending())
	{
		GetWorldTimerManager().SetTimer(
		    CorpseDestroyTimer,
		    FTimerDelegate::CreateWeakLambda(this, [this] { Destroy(); }),
		    FMath::Max(DestroyDelaySecs, 0.01f), false);
	}
}

void AWoTCharacter::Destroyed()
{
	// The port destroys/replaces player pawns where retail moved its persistent
	// PlayerPawn. Leave the region while both pawn and controller are still usable
	// so owner-camera and authority ZonePlayerEvent teardown run exactly once.
	TeardownOwnerLocalZoneFOV();
	if (UWorld* World = GetWorld())
	{
		if (UWoTZoneIndex* Zones = World->GetSubsystem<UWoTZoneIndex>())
		{
			Zones->RemoveActor(this);
		}
	}
	if (HasAuthority())
	{
		// Retail destruction is broader than the ordinary Died path. WOTPlayer
		// drops seals first, then CancelAngrealEffects + RemoveAllReflectorsFrom;
		// Engine.Pawn::Destroyed finally destroys the inventory chain. UE5 Owner
		// does not own actor lifetime, so reproduce that chain explicitly.
		// Destruction is a lifecycle event, not interactive input. Retail always
		// drops carried seals here even if a Citadel palette/edit state is active.
		DropAllSeals(/*bIgnoreInteractionGuards*/ true);
		if (InventoryComponent)
		{
			const TArray<TObjectPtr<AWoTInventory>> Items =
				InventoryComponent->Items;
			for (AWoTInventory* Item : Items)
			{
				if (AWoTAngrealInventory* Angreal =
					Cast<AWoTAngrealInventory>(Item))
				{
					Angreal->Reset();
				}
			}
		}

		TArray<UWoTLeech*> Leeches;
		GetComponents<UWoTLeech>(Leeches);
		for (UWoTLeech* Leech : Leeches)
		{
			if (IsValid(Leech) && Leech->bRemovable)
			{
				Leech->Unattach();
			}
		}
		TArray<UWoTReflector*> Reflectors;
		GetComponents<UWoTReflector>(Reflectors);
		for (UWoTReflector* Reflector : Reflectors)
		{
			if (IsValid(Reflector))
			{
				// WOTUtil.RemoveAllReflectorsFrom intentionally includes defaults
				// whose bRemovable is false when a pawn is being destroyed.
				Reflector->Uninstall();
			}
		}

		if (InventoryComponent)
		{
			const TArray<TObjectPtr<AWoTInventory>> RemainingItems =
				InventoryComponent->Items;
			for (AWoTInventory* Item : RemainingItems)
			{
				if (IsValid(Item))
				{
					Item->Destroy();
				}
			}
		}
	}

	if (HasAuthority() && bDead)
	{
		// The pawn actor is going away before its carcass notify fired (an early player
		// respawn can beat the authored fraction): swap now so a corpse always exists.
		SpawnCarcassNow();
		// A dead NPC's AI controller has nothing left to drive (the player respawn
		// path never touches NPCs — WoTGameMode branches them off); tear it down with
		// the pawn.
		if (AController* C = GetController(); C && !C->IsA<APlayerController>())
		{
			C->UnPossess();
			C->Destroy();
		}
	}
	Super::Destroyed();
}

void AWoTCharacter::OnRep_bDead()
{
	// Died() is authority-only; the simulated proxy learns of the death through
	// this replicated flag and runs the client-side shutdown locally.
	if (bDead)
	{
		ShutdownCorpsePresentation();
	}
}

void AWoTCharacter::SetAnimIntent(FName Sequence, float Rate, bool bLoop)
{
	// Authority stamps the intent; the owning client self-animates (COND_SkipOwner)
	// and non-owners replay it via OnRep_Anim. The bumped token re-fires the same
	// sequence even when Sequence is unchanged.
	if (!HasAuthority())
	{
		return;
	}
	if (Disguise.bActive)
	{
		Sequence = SubstituteDisguiseAnimName(Sequence);
	}
	RepAnim.Sequence = Sequence;
	RepAnim.Rate = Rate;
	RepAnim.bLoop = bLoop ? 1 : 0;
	RepAnim.TweenTime = 0.0f;
	RepAnim.bTweenOnly = 0;
	RepAnim.HeldFrame = 0;
	RepAnim.bHoldFrame = 0;
	++RepAnim.PlayToken;
	TrackAuthorityLandingAnimation(Sequence, Rate, 0.0f,
		/*bTweenOnly*/ false, bLoop);
}

void AWoTCharacter::RefreshTouchReflectorDescriptions(
	const UWoTReflector* Excluded)
{
	if (!HasAuthority()) return;
	TArray<UWoTReflector*> Ordered;
	GetOrderedReflectors(Ordered);
	TArray<FWoTTouchReflectorDescription> Updated;
	for (const UWoTReflector* Reflector : Ordered)
	{
		if (!IsValid(Reflector) || Reflector == Excluded) continue;
		FWoTTouchReflectorDescription Description;
		if (Reflector->BuildTouchReflectorDescription(Description))
			Updated.Add(MoveTemp(Description));
	}
	TouchReflectorDescriptions = MoveTemp(Updated);
	ForceNetUpdate();
}

bool AWoTCharacter::ShouldSilentlyRetireProjectile(
	const AWoTGenericProjectile* Projectile) const
{
	if (!IsValid(Projectile)) return false;
	const AWoTSeekingProjectile* Seeking =
		Cast<AWoTSeekingProjectile>(Projectile);
	const bool bAfterThisPawn = !Seeking || !Seeking->bSeeking
		|| Seeking->Destination == this;
	for (const FWoTTouchReflectorDescription& Description
		: TouchReflectorDescriptions)
	{
		bool bAffectable = true;
		for (const FName Type : Description.UnaffectedProjectileTypes)
		{
			if (WoTUE1Names::ActorIsA(Projectile, Type))
			{
				bAffectable = false;
				break;
			}
		}
		if (!bAffectable) continue;
		if (Description.Kind == EWoTTouchReflectorKind::Absorb)
			return true;
		if (Description.Kind == EWoTTouchReflectorKind::Reflect
			&& bAfterThisPawn)
			return true;
		// Fork leaves the incoming local copy unchanged and ordered dispatch
		// continues, just as authority continues to any lower-priority reflector.
	}
	return false;
}

void AWoTCharacter::SetTweenAnimIntent(FName Sequence, float Rate,
	float TweenTime, bool bTweenOnly)
{
	if (!HasAuthority())
	{
		return;
	}
	if (Disguise.bActive)
	{
		Sequence = SubstituteDisguiseAnimName(Sequence);
	}
	RepAnim.Sequence = Sequence;
	RepAnim.Rate = Rate;
	RepAnim.bLoop = 0;
	RepAnim.TweenTime = FMath::Max(0.0f, TweenTime);
	RepAnim.bTweenOnly = bTweenOnly ? 1 : 0;
	RepAnim.HeldFrame = 0;
	RepAnim.bHoldFrame = 0;
	++RepAnim.PlayToken;
	TrackAuthorityLandingAnimation(Sequence, Rate, RepAnim.TweenTime,
		bTweenOnly, /*bLoop*/ false);
}

FName AWoTCharacter::SubstituteDisguiseAnimName(FName PlayerAnim)
{
	if (PlayerAnim == TEXT("Run") || PlayerAnim == TEXT("RunL")
		|| PlayerAnim == TEXT("RunR") || PlayerAnim == TEXT("AttackRun")
		|| PlayerAnim == TEXT("AttackRunL") || PlayerAnim == TEXT("AttackRunR"))
		return TEXT("Run");
	if (PlayerAnim == TEXT("Walk") || PlayerAnim == TEXT("WalkL")
		|| PlayerAnim == TEXT("WalkR") || PlayerAnim == TEXT("Swim")
		|| PlayerAnim == TEXT("AttackWalk")) return TEXT("Walk");
	if (PlayerAnim == TEXT("Attack")) return TEXT("Breath");
	static const FName Preserved[] = {TEXT("Breath"),TEXT("Fall"),TEXT("Landed"),
		TEXT("Jump"),TEXT("HitB"),TEXT("HitBHard"),TEXT("HitF"),
		TEXT("HitFHard"),TEXT("Look")};
	for (FName Name : Preserved) if (PlayerAnim == Name) return PlayerAnim;
	return TEXT("Walk");
}

void AWoTCharacter::OnRep_Anim()
{
	// Replay the replicated intent on the pawn's vertex-anim component if one is
	// present. Headless / pre-mesh-bind pawns have none — the intent still reads off
	// RepAnim for the probe, so anim stays observable. On a proxy the component's own
	// authority-gated re-stamp is skipped, so this never loops back through SetAnimIntent.
	if (UWoTVertexAnimComponent* Anim = FindComponentByClass<UWoTVertexAnimComponent>())
	{
		if (RepAnim.bHoldFrame)
		{
			Anim->SetFrameAndStop(RepAnim.Sequence,
				static_cast<float>(RepAnim.HeldFrame)
				/ static_cast<float>(MAX_uint16));
		}
		else if (RepAnim.bTweenOnly)
		{
			Anim->TweenAnim(RepAnim.Sequence, RepAnim.TweenTime);
		}
		else if (RepAnim.TweenTime > 0.0f)
		{
			Anim->TweenPlayAnim(RepAnim.Sequence, RepAnim.Rate, RepAnim.TweenTime);
		}
		else if (RepAnim.bLoop)
		{
			Anim->LoopAnim(RepAnim.Sequence, RepAnim.Rate);
		}
		else
		{
			Anim->PlayAnim(RepAnim.Sequence, RepAnim.Rate);
		}
	}
}

void AWoTCharacter::UpdateBreath(float DeltaSeconds)
{
	// The engine PainTime countdown (authority-only)
	// + HeadZoneChange's arm/reset edges + PainTimer's drown
	// branch, folded to one authority update. Dying pawns
	// ignore it (Dying.PainTimer).
	if (!HasAuthority() || bDead || !GetWorld())
	{
		return;
	}
	const FVector Loc = GetActorLocation();
	const FVector FootPoint = Loc - FVector(0.0f, 0.0f, GetSimpleCollisionHalfHeight());
	const bool bHeadInWater = AWoTZoneVolume::IsPointInWater(
	    GetWorld(), Loc + FVector(0.0f, 0.0f, BaseEyeHeight));
	const bool bFeetInPainZone = AWoTZoneVolume::IsPointInPainZone(GetWorld(), FootPoint);

	// FootZoneChange's splash half: entering a water
	// zone plays its EntrySound at 2*splashSize — unless something solid lies
	// within ~40uu below the feet (the shipped WaterStep fallback is None
	// everywhere, so shallow entries are SILENT); exiting plays the OLD zone's
	// ExitSound. One 0.25s throttle per pawn (SplashTime), shared across zones.
	// Retail's EntryActor/ExitActor visual chain (WaterImpact->WaterRing) shipped
	// mesh-less and draws nothing — sound-only is parity (PORT_NOTES).
	AWoTZoneVolume* FootWater = AWoTZoneVolume::WaterZoneAt(GetWorld(), FootPoint);
	const bool bFeetInWater = FootWater != nullptr;
	if (bFeetInWater != bWasFeetInWater &&
	    GetWorld()->GetTimeSeconds() - LastSplashTime > 0.25f)
	{
		LastSplashTime = GetWorld()->GetTimeSeconds();
		// splashSize: mass-scaled entry speed, clamped 1-4.
		const float Mass = GetCharacterMovement() ? GetCharacterMovement()->Mass : 175.0f;
		const float SplashSize = FMath::Clamp(
		    0.000025f * Mass *
		        (300.0f - 0.5f * FMath::Max(-500.0f, static_cast<float>(GetVelocity().Z))),
		    1.0f, 4.0f);
		const AWoTZoneVolume* Zone = bFeetInWater ? FootWater : LastFootWaterZone.Get();
		const FName Sub = Zone ? (bFeetInWater ? Zone->EntrySoundSubPath
		                                       : Zone->ExitSoundSubPath)
		                       : FName();
		bool bShallowSilent = false;
		if (bFeetInWater)
		{
			FHitResult FloorHit;
			bShallowSilent = WoTVisBlockingTrace(GetWorld(), FootPoint,
				FootPoint - FVector(0.0f, 0.0f, 40.0f), this, FloorHit);
		}
		if (!Sub.IsNone() && !bShallowSilent)
		{
			WoTSound::PlayReplicated(*GetWorld(), Sub, FootPoint, 1600.0f,
			                         2.0f * SplashSize);
		}
	}
	bWasFeetInWater = bFeetInWater;
	if (bFeetInWater)
	{
		LastFootWaterZone = FootWater;
	}

	if (bHeadInWater && !bWasHeadInWater)
	{
		// Head submerged: arm the full breath — unless the feet sit in a pain zone,
		// whose own cadence owns the shared timer (Pawn.HeadZoneChange).
		if (!bFeetInPainZone)
		{
			BreathTime = UnderWaterTime;
		}
		bDrowning = false;
	}
	else if (!bHeadInWater && bWasHeadInWater)
	{
		// Head surfaced (Pawn.HeadZoneChange). Gasp window:
		// players only, under 8s of breath left at the surfacing moment (a
		// drowning pawn's 0-2s cycle also matches).
		if (!IsNPCPawn() && BreathTime > 0.0f && BreathTime < 8.0f)
		{
			Gasp();
		}
		if (!bFeetInPainZone)
		{
			BreathTime = -1.0f;
		}
		bDrowning = false;
	}
	// FootZoneChange arms PainTime = 0.01 on pain-zone entry —
	// clobbering any running breath: transit a pain zone underwater and the breath
	// is spent.
	if (bFeetInPainZone && !bWasFeetInPainZone && BreathTime > 0.0f)
	{
		BreathTime = 0.01f;
	}
	// While submerged in a pain zone the shared timer is always cycling (the zone's
	// pain branch re-arms 1.0 each fire) — pin it so leaving the pain zone underwater
	// drowns within a second, as in UE1.
	if (bFeetInPainZone && bHeadInWater && BreathTime <= 0.0f)
	{
		BreathTime = 1.0f;
	}
	bWasHeadInWater = bHeadInWater;
	bWasFeetInPainZone = bFeetInPainZone;

	if (BreathTime > 0.0f)
	{
		BreathTime -= DeltaSeconds;
		if (BreathTime < 0.001f)
		{
			BreathTime = 0.0f;
			if (bFeetInPainZone)
			{
				// Pain-branch priority: the ZONE deals its own damage on its own
				// occupant timer; the shared timer just re-arms (Pawn.PainTimer).
				BreathTime = 1.0f;
			}
			else if (bHeadInWater)
			{
				// 5 HP, no instigator, 'Drowned', throat-high hit, zero momentum
				// (Pawn.PainTimer) — the killer-less suicide path, like 'fell'.
				bDrowning = true;
				FWoTDamageEvent Ev;
				Ev.WoTDamageType = TEXT("Drowned");
				Ev.UE1Momentum = FVector::ZeroVector;
				Ev.HitInfo.ImpactPoint =
				    Loc + FVector(0.0f, 0.0f, 0.5f * GetSimpleCollisionHalfHeight());
				TakeDamage(5.0f, Ev, nullptr, nullptr);
				if (Health > 0)
				{
					BreathTime = 2.0f;
				}
			}
		}
	}

	// HUD mirror (owner-only replicated): whole seconds during the countdown,
	// 0 while the drown loop cycles, -1 dry/disarmed.
	BreathDisplay = (bHeadInWater && BreathTime > 0.0f)
	                    ? (bDrowning ? 0 : FMath::CeilToInt(BreathTime))
	                    : -1;
}

void AWoTCharacter::UpdateAmbientHum()
{
	// The looping AmbientSound hum (Legion's Leg_Ambient), attached lazily on
	// every machine — the replicated path/bytes land after creation on clients
	// (the leech UpdateAmbientHum shape, WoTLeech.cpp).
	if (AmbientHumPath.IsNone() || AmbientHum || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	if (USoundBase* Wave = WoTSound::LoadBySubPath(AmbientHumPath, true))
	{
		AmbientHum = NewObject<UWoTAmbientSoundComponent>(this);
		AmbientHum->SetupAttachment(GetRootComponent());
		AmbientHum->RegisterComponent();
		AmbientHum->ConfigureUE1Ambient(Wave, AmbientHumRadius, AmbientHumVolume,
		                                AmbientHumPitch);
		AmbientHum->Play();
	}
	else
	{
		AmbientHumPath = NAME_None;   // unresolved: don't retry every tick
	}
}

FName AWoTCharacter::SelectPlayerLocomotionSlot(
	const UCharacterMovementComponent* Move, float& OutRateScale) const
{
	OutRateScale = 1.0f;
	if (!Move)
	{
		return FName(TEXT("Wait"));
	}
	if (Move->IsSwimming())
	{
		return FName(TEXT("Swim"));
	}

	const float HorizontalVelocity = Move->Velocity.Size2D();
	if (HorizontalVelocity <= 10.0f)
	{
		return FName(TEXT("Wait"));
	}

	const FVector Motion = Move->GetCurrentAcceleration();
	if (Motion.SizeSquared2D() <= FMath::Square(KINDA_SMALL_NUMBER))
	{
		// SetMotionDirection(0) -> MD_None -> PlayWaiting, even while residual
		// velocity is decaying (WOTPlayer.uc:1959-1968,2444-2449).
		return FName(TEXT("Wait"));
	}

	// Pawn.GroundSpeed is the movement setting, not instantaneous velocity.
	// WOTPlayer scales grounded loops by GroundSpeed divided by the apparent
	// class default (uc:2455-2481,2555-2559). Data-driven NPC disguises do not
	// have one concrete UE class per creature, so their replicated state carries
	// the row's authored class-default GroundSpeed explicitly.
	float DefaultGroundSpeed = 400.0f;
	if (Disguise.bActive && Disguise.SourceDefaultWalkSpeed > KINDA_SMALL_NUMBER)
	{
		DefaultGroundSpeed = Disguise.SourceDefaultWalkSpeed;
	}
	else if (const TSubclassOf<AWoTCharacter> ApparentCharacterClass =
		GetEffectiveAppearanceClass())
	{
		const AWoTCharacter* ApparentCDO =
			ApparentCharacterClass->GetDefaultObject<AWoTCharacter>();
		if (const UCharacterMovementComponent* ApparentMove =
			ApparentCDO ? ApparentCDO->GetCharacterMovement() : nullptr)
		{
			DefaultGroundSpeed = ApparentMove->MaxWalkSpeed;
		}
	}
	OutRateScale = Move->MaxWalkSpeed / FMath::Max(DefaultGroundSpeed, 1.0f);
	const UWoTCharacterMovement* RetailMove = Cast<UWoTCharacterMovement>(Move);
	if (RetailMove && RetailMove->IsRetailWalking())
	{
		return bPlayerAnimFireHeld
			? FName(TEXT("AttackWalk")) : FName(TEXT("Walk"));
	}

	// WOTPlayer.SetMotionDirection compares ViewRotation to the motion vector
	// in UE1 yaw units. The authored +/-8192 and +/-24576 boundaries are
	// exactly +/-45 and +/-135 degrees (WOTPlayer.uc:1971-1993).
	const float RelativeYaw = FRotator::NormalizeAxis(
		GetViewRotation().Yaw - Motion.Rotation().Yaw);
	if (RelativeYaw > 45.0f && RelativeYaw <= 135.0f)
	{
		return bPlayerAnimFireHeld
			? FName(TEXT("AttackRunL")) : FName(TEXT("RunL"));
	}
	if (RelativeYaw < -45.0f && RelativeYaw >= -135.0f)
	{
		return bPlayerAnimFireHeld
			? FName(TEXT("AttackRunR")) : FName(TEXT("RunR"));
	}
	// Forward and backward intentionally share Run/AttackRun; the shipped
	// player tables expose no separate backpedal sequence.
	return bPlayerAnimFireHeld
		? FName(TEXT("AttackRun")) : FName(TEXT("Run"));
}

void AWoTCharacter::PlayPlayerTweenPose(FName SlotName, float CallerTween)
{
	if (IsNPCPawn() || bDead || (!HasAuthority() && !IsLocallyControlled()))
	{
		return;
	}
	const FWoTAnimTableEntry* Pick =
		WoTAnimTable::PickSlotAnim(AnimTableName, SlotName, this);
	FName Sequence = Pick ? Pick->AnimName : SlotName;
	if (Disguise.bActive)
	{
		Sequence = SubstituteDisguiseAnimName(Sequence);
	}
	const float TweenTime =
		WoTAnimTable::ResolveTweenTime(Pick, CallerTween);

	UWoTVertexAnimComponent* Anim =
		FindComponentByClass<UWoTVertexAnimComponent>();
	const bool bBoundSequence = Anim && Anim->FindSequence(Sequence);
	if (bBoundSequence)
	{
		// TweenAnim stamps the authority-owned replicated intent through the
		// component's existing owner seam and also gives an autonomous/listen
		// owner the immediate local pose that COND_SkipOwner requires.
		Anim->TweenAnim(Sequence, TweenTime);
	}
	else if (HasAuthority() && (!Anim || Anim->Sequences.Num() == 0))
	{
		// Headless/pre-bind worlds keep the state observable. Once a real mesh
		// exposes a sequence table, never ask it to play a missing sequence.
		SetTweenAnimIntent(Sequence, 0.0f, TweenTime, true);
	}
	if (HasAuthority() || IsLocallyControlled())
	{
		HoldLocomotionFor(TweenTime);
		// PlayerPawn.PlayerLanded.AnimEnd routes a minor Land through
		// TweenToWaiting(0.2). Keep the source pose explicit so the next local
		// driver tick takes that edge instead of treating the hold as startup.
		if (SlotName == FName(TEXT("Land")))
		{
			DrivenLocomotionAnim = SlotName;
		}
	}
}

void AWoTCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!IsLocallyControlled())
	{
		ClearCreaturePresentationPitch();
	}
	UpdateAmbientHum();
	UpdateBreath(DeltaSeconds);
	if (HasAuthority())
	{
		UpdateStatusIconDurations();
	}
	if (IsLocallyControlled())
	{
		if (const UWoTPreferencesSubsystem* Preferences = GetGameInstance()
			? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr)
		{
			bAlwaysMouseLook = Preferences->GetGameSettings().bAlwaysMouseLook;
			bLookUpStairs = Preferences->GetGameSettings().bAutoSlopeLook;
			bSnapToLevel = Preferences->GetGameSettings().bLookSpring;
		}
		if (bCenterViewHeld && !bFreeLookHeld) { bKeyboardLook = false; }
		const UCharacterMovementComponent* Movement = GetCharacterMovement();
		const bool bExplicitLook = bExplicitLookInputThisFrame
			|| bFreeLookHeld || IsLookActive();
		if (!bCenterViewHeld)
		{
			if (bExplicitLook)
			{
				bCenterView = false;
			}
			else if (bSnapToLevel && !bAlwaysMouseLook)
			{
				bCenterView = true;
				bKeyboardLook = false;
			}
		}
		if (Controller && bLookUpStairs && !bAlwaysMouseLook && !bExplicitLook
			&& Movement && (Movement->MovementMode == MOVE_Walking
				|| Movement->MovementMode == MOVE_NavWalking))
		{
			FRotator View = Controller->GetControlRotation();
			View.Pitch = CalculateStairLookPitch(DeltaSeconds);
			Controller->SetControlRotation(View);
		}
		else if (Controller && bCenterView && !bExplicitLook
			&& Movement && (Movement->MovementMode == MOVE_Walking
				|| Movement->MovementMode == MOVE_NavWalking))
		{
			FRotator View = Controller->GetControlRotation();
			View.Pitch = DecayCenterViewPitch(
				static_cast<float>(FMath::UnwindDegrees(View.Pitch)),
				DeltaSeconds);
			Controller->SetControlRotation(View);
		}
		bExplicitLookInputThisFrame = false;
	}

	// Player selection runs both on authority (which stamps RepAnim) and on the
	// autonomous owner (COND_SkipOwner requires immediate local playback). NPCs
	// remain authority-driven. Both paths play only on a state edge and respect
	// death/one-shot holds.
	const bool bLocalPlayerDriver = !IsNPCPawn() && IsLocallyControlled();
	if ((HasAuthority() || bLocalPlayerDriver) && !bDead)
	{
		// PlayerLanded owns the animation channel until AnimEnd. On the exact
		// frame LocomotionHoldUntil expires, actor ticks precede timer dispatch;
		// allowing Breath/Run here would stamp a loop, cancel the landing release
		// timer in TrackAuthorityLandingAnimation, and freeze the player forever.
		if (IsHardLanding())
		{
			return;
		}
		if (IsNPCPawn())
		{
			const AWoTNPCController* NPCController = Cast<AWoTNPCController>(Controller);
			if (NPCController && NPCController->GetBrainState() == EWoTBrainState::Falling)
			{
				return; // FallingState owns Fall and Land through FinishAnim.
			}
			if (NPCController && NPCController->IsWaitingCowering())
			{
				return; // WaitingCower owns ReactP then the ReactPLoop channel.
			}
			if (bNPCAnimationSuspendedForIce)
			{
				return; // accepted Ice owns a frozen pose until thaw
			}
		}
		if (GetWorld()->GetTimeSeconds() < LocomotionHoldUntil)
		{
			return;
		}
		const UCharacterMovementComponent* Move = GetCharacterMovement();
		if (!IsNPCPawn() && Move && Move->IsFalling())
		{
			// Jump/Fall owns the pose until a landing or water transition.
			return;
		}
		float RateScale = 1.0f;
		FName Desired;
		bool bNPCIdleCycle = false;
		if (!IsNPCPawn())
		{
			Desired = SelectPlayerLocomotionSlot(Move, RateScale);
			BaseEyeHeight = Move && Move->IsSwimming()
				? 0.5f * RetailStandingBaseEyeHeight
				: RetailStandingBaseEyeHeight;
		}
		else
		{
			const UWoTCasterComponent* Caster = FindComponentByClass<UWoTCasterComponent>();
			if (Caster && Caster->IsContinuousCasting())
			{
				// AngrealUser.LoopMovementAnim/PlayInactiveAnimation route both
				// moving and stationary continuous casts through AttackRun.
				Desired = FName(TEXT("AttackRun"));
			}
			else
			{
				const float ActualSpeed = Move ? Move->Velocity.Size2D() : 0.0f;
				const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(this);
				const AWoTNPCController* NPCController =
					Cast<AWoTNPCController>(Controller);
				float Intended = NPCController
					? NPCController->GetIntendedMovementSpeedForAnimation()
					: ActualSpeed;
				const bool bHasMovementIntent = NPCController
					&& NPCController->HasActiveMovementIntentForAnimation()
					&& Intended > KINDA_SMALL_NUMBER;
				if (ActualSpeed <= 10.0f && !bHasMovementIntent)
				{
					Desired = FName(TEXT("Wait"));
					if (DrivenLocomotionAnim == Desired)
					{
						NPCIdleAnimRemaining = FMath::Max(0.0f,
							NPCIdleAnimRemaining - DeltaSeconds);
						bNPCIdleCycle = NPCIdleAnimRemaining <= KINDA_SMALL_NUMBER;
					}
				}
				else
				{
					if (Intended <= KINDA_SMALL_NUMBER) Intended = ActualSpeed;
					const float Ground = NPC
						? NPC->AnimationGroundSpeed : FMath::Max(Intended, 1.0f);
					const float Walking = NPC
						? NPC->AnimationBaseWalkingSpeed : FMath::Max(Intended, 1.0f);
					const float WalkPct = NPC ? NPC->AnimationWalkSpeedPct : 50.0f;
					const float WalkThreshold = Walking + WalkPct / 100.0f
						* (Ground - Walking);
					if (Intended <= WalkThreshold)
					{
						Desired = FName(TEXT("Walk"));
						RateScale = Intended / FMath::Max(Walking, 1.0f);
					}
					else
					{
						Desired = FName(TEXT("Run"));
						RateScale = Intended / FMath::Max(Ground, 1.0f);
					}
				}
			}
		}
		const bool bPlayerLocomotion = !IsNPCPawn();
		const FName PreviousDriven = DrivenLocomotionAnim;
		const bool bStateEdge = Desired != PreviousDriven;
		bool bTransitionCompleted = false;
		if (bPlayerLocomotion && !bStateEdge
			&& PlayerLocomotionTransitionRemaining > 0.0f)
		{
			PlayerLocomotionTransitionRemaining = FMath::Max(
				0.0f, PlayerLocomotionTransitionRemaining - DeltaSeconds);
			bTransitionCompleted =
				PlayerLocomotionTransitionRemaining <= KINDA_SMALL_NUMBER;
		}
		const bool bRateEdge = !bStateEdge && !bNPCIdleCycle
			&& PlayerLocomotionTransitionRemaining <= KINDA_SMALL_NUMBER
			&& !FMath::IsNearlyEqual(
				RateScale, DrivenLocomotionRateScale, KINDA_SMALL_NUMBER);
		if (bStateEdge || bTransitionCompleted || bRateEdge || bNPCIdleCycle)
		{
			if (bNPCIdleCycle && HasAuthority())
			{
				PlayVoiceSlot(FName(TEXT("WaitingRandom")));
			}
			if (bStateEdge)
			{
				// A new retail state supersedes any pending AnimEnd completion.
				PlayerLocomotionTransitionRemaining = 0.0f;
			}
			// PlayBreathing performs the shipped 90/10 Breath/Look pick. The
			// imported Wait rows encode exactly those odds and concrete sequences.
			const bool bWaiting = Desired == FName(TEXT("Wait"));
			const bool bStationaryAttack = bPlayerLocomotion && bWaiting
				&& bPlayerAnimFireHeld && !Disguise.bActive;
			const bool bTweenToWaiting = bPlayerLocomotion && bStateEdge
				&& bWaiting && !PreviousDriven.IsNone()
				&& PreviousDriven != FName(TEXT("Wait"));
			FName PickSlot = Desired;
			if (bStationaryAttack)
			{
				// TweenToBreathing holds Attack for as long as bFire remains set.
				PickSlot = FName(TEXT("Attack"));
			}
			else if (bTweenToWaiting)
			{
				PickSlot = FName(TEXT("Breath"));
			}
			const FWoTAnimTableEntry* Pick = WoTAnimTable::PickSlotAnim(
				AnimTableName, PickSlot, this);
			if (!Pick && bWaiting)
			{
				Pick = WoTAnimTable::PickSlotAnim(
					AnimTableName, FName(TEXT("Breath")), this);
			}
			FName LoopAnim = Pick ? Pick->AnimName
				: (bWaiting ? FName(TEXT("Breath")) : Desired);
			if (Disguise.bActive)
			{
				LoopAnim = SubstituteDisguiseAnimName(LoopAnim);
			}
			if (const UWoTVertexAnimComponent* Anim =
				Cast<UWoTVertexAnimComponent>(GetMesh());
				Anim && Anim->Sequences.Num() > 0 && !Anim->FindSequence(LoopAnim))
			{
				// A bound production mesh is authoritative about its sequence set.
				// Preserve the prior valid animation instead of issuing a bogus name.
				return;
			}
			const float LoopRate =
				WoTAnimTable::ResolveRate(Pick) * RateScale;
			if (!bPlayerLocomotion && bWaiting)
			{
				const float Duration = SequenceDurationSeconds(LoopAnim, LoopRate);
				NPCIdleAnimRemaining = Duration > KINDA_SMALL_NUMBER
					? Duration : 1.0f;
			}
			DrivenLocomotionAnim = Desired;
			DrivenLocomotionRateScale = RateScale;
			const bool bGroundedMovement = bPlayerLocomotion && !bWaiting
				&& Desired != FName(TEXT("Swim"));
			const bool bTweenIntoMovement = bStateEdge && bGroundedMovement
				&& (PreviousDriven.IsNone()
					|| PreviousDriven == FName(TEXT("Wait")));
			const bool bStampTransition =
				bTweenToWaiting || bTweenIntoMovement
				|| (bStationaryAttack && bStateEdge);
			UWoTVertexAnimComponent* Anim =
				Cast<UWoTVertexAnimComponent>(GetMesh());
			const bool bCanPlay = Anim && Anim->FindSequence(LoopAnim);
			if (bStationaryAttack && bStateEdge)
			{
				const float TweenTime = bTweenToWaiting ? 0.2f
					: WoTAnimTable::ResolveTweenTime(Pick);
				if (bCanPlay)
				{
					Anim->TweenAnim(LoopAnim, TweenTime);
				}
				else if (HasAuthority() && (!Anim || Anim->Sequences.IsEmpty()))
				{
					SetTweenAnimIntent(LoopAnim, 0.0f, TweenTime, true);
				}
				// TweenAnim ends on the Attack pose. It has no follow-on idle loop
				// until the local/server fire-release edge invalidates this state.
				PlayerLocomotionTransitionRemaining = 0.0f;
			}
			else if (bTweenToWaiting)
			{
				if (bCanPlay)
				{
					Anim->TweenAnim(LoopAnim, 0.2f);
				}
				else if (HasAuthority() && (!Anim || Anim->Sequences.IsEmpty()))
				{
					SetTweenAnimIntent(LoopAnim, 0.0f, 0.2f, true);
				}
				PlayerLocomotionTransitionRemaining = 0.2f;
			}
			else if (bTweenIntoMovement)
			{
				if (bCanPlay)
				{
					Anim->TweenPlayAnim(LoopAnim, LoopRate, 0.1f);
				}
				else if (HasAuthority() && (!Anim || Anim->Sequences.IsEmpty()))
				{
					SetTweenAnimIntent(LoopAnim, LoopRate, 0.1f, false);
				}
				const float Duration = SequenceDurationSeconds(LoopAnim, LoopRate);
				PlayerLocomotionTransitionRemaining = 0.1f
					+ (Duration > 0.0f ? Duration : 1.0f);
			}
			else
			{
				if (bCanPlay)
				{
					Anim->LoopAnim(LoopAnim, LoopRate);
				}
				else if (HasAuthority() && (!Anim || Anim->Sequences.IsEmpty()))
				{
					SetAnimIntent(LoopAnim, LoopRate, true);
				}
				PlayerLocomotionTransitionRemaining = 0.0f;
			}

			// Cache the loop's sound notifies (sound 5d): footstep + idle-bark rows
			// fire cyclically below at their authored fractions.
			LoopNotifies.Reset();
			LoopDuration = 0.0f;
			LoopElapsed = 0.0f;
			LoopSequence = LoopAnim;
			if (!bStampTransition)
			if (const UWoTVertexAnimComponent* CurrentAnim =
			        Cast<UWoTVertexAnimComponent>(GetMesh()))
			{
				if (const FWoTAnimSequence* Seq = CurrentAnim->FindSequence(LoopAnim))
				{
					LoopDuration = LoopSequenceDurationSeconds(LoopAnim, LoopRate);
					for (const FWoTAnimNotify& N : Seq->Notifies)
					{
						if (N.Function == FName(TEXT("PlayMovementSound")) ||
						    N.Function == FName(TEXT("PlayAnimSound")))
						{
							LoopNotifies.Add(N);
						}
					}
				}
			}
		}

		// The cyclic loop-notify clock (Grunt's anim-notify sounds, authority-driven
		// because a headless server never plays the anim locally).
		if (HasAuthority() && LoopDuration > 0.0f && LoopNotifies.Num() > 0)
		{
			const float Prev = LoopElapsed;
			LoopElapsed += DeltaSeconds;
			const float PrevPhase = FMath::Fmod(Prev, LoopDuration) / LoopDuration;
			float NewPhase = FMath::Fmod(LoopElapsed, LoopDuration) / LoopDuration;
			const bool bWrapped = LoopElapsed >= Prev + LoopDuration ||
			                      NewPhase < PrevPhase;
			for (const FWoTAnimNotify& N : LoopNotifies)
			{
				const bool bCrossed = bWrapped
				    ? (N.Fraction > PrevPhase || N.Fraction <= NewPhase)
				    : (N.Fraction > PrevPhase && N.Fraction <= NewPhase);
				if (bCrossed)
				{
					FireLoopNotify(N.Function);
				}
			}
		}
	}
}

void AWoTCharacter::SetSuicideInstigator(APawn* Other)
{
	// WOTPawn.SetSuicideInstigator: remember who displaced us and when, so ScoreKill can
	// credit them if we die in the next few seconds. Store the controller (the stable player
	// identity across a respawn) rather than the pawn, which the port replaces on death.
	SuicideInstigator = Other ? Other->GetController() : nullptr;
	SuicideInstigationTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void AWoTCharacter::BeginPlay()
{
	if (!IsA<AWoTNPCCharacter>())
	{
		FString DefaultsError;
		const FWoTRuntimeClassDefaults* Defaults =
			UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(DefaultsError);
#if !WOT_GUARDED_BOOTSTRAP
		if (Defaults == nullptr)
		{
			WoTDevRetailFamiliesEnsureActivated();
			Defaults = WoTDevRetailClassDefaults();
			if (Defaults != nullptr) DefaultsError.Reset();
		}
#endif
		if (Defaults != nullptr)
		{
			if (!ApplyRetailPlayerHealthDefaults(*Defaults, DefaultsError))
			{
				UE_LOG(LogTemp, Error, TEXT("Player health defaults: %s"),
					*DefaultsError);
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Player health defaults unavailable: %s"),
				*DefaultsError);
		}
	}
	Super::BeginPlay();
	// Pawn.PreBeginPlay: Skill is the class-authored base
	// plus the current game difficulty, clamped before any sight checks run.
	const AWoTGameMode* Game = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr;
	Skill = FMath::Clamp(Skill + (Game ? Game->Difficulty : 0), 0.0f, 3.0f);
	// Cosmetic, every role: a fresh spawn binds its own class's appearance; a late joiner
	// with a replicated morph binds the override (OnRep may also have run — idempotent).
	// A disguise is not idempotent with that bind. A client first receiving an already
	// disguised pawn runs OnRep_Disguise before BeginPlay (the actor channel calls
	// RepNotifies, then PostNetInit), so the disguise look predates the base bind and
	// SavedDisguise snapshotted an unbound body. Peel it, bind the base, and re-layer
	// the disguise so the look survives and a later revert restores the real base.
	const bool bReapplyDisguise = bDisguiseLookActive;
	if (bReapplyDisguise)
	{
		RestoreDisguiseLook();
	}
	ApplyFactionAppearance();
	WoTSoundTable::InitializeSlotTimers(*this);
	if (bReapplyDisguise && Disguise.bActive)
	{
		ApplyDisguiseLook();
	}
	EnsureAngrealHands();
	if (UWoTZoneIndex* Zones = GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		Zones->UpdateActorZones(this, true);
	}
}

void AWoTCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	if (HasAuthority())
	{
		SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	}
	ResetWalkingInputState();
	// GameInfo.Login applied ?Skin= once to UE1's PERSISTENT PlayerPawn; the UE5 split
	// respawns a fresh pawn, so the controller carries the selection (login-parsed and
	// ServerChangeSkin-updated) and every possession re-applies it.
	if (const AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(NewController))
	{
		if (!WoTController->PersonalSkinURL.IsEmpty())
		{
			ServerSetPersonalSkin(WoTController->PersonalSkinURL);
		}
	}
	EnsureAngrealHands();
	ApplyPendingZoneFOV();
	ReconcileOwnerLocalZoneFOV();
}

void AWoTCharacter::OnRep_Controller()
{
	Super::OnRep_Controller();
	ResetWalkingInputState();
	EnsureAngrealHands();
	ApplyPendingZoneFOV();
	if (GetController()) ReconcileOwnerLocalZoneFOV();
}

void AWoTCharacter::SetZoneFOV(float NewFOV)
{
	bHasZoneFOV = true;
	PendingZoneFOV = FMath::Clamp(NewFOV, 1.0f, 170.0f);
	ApplyPendingZoneFOV();
}

void AWoTCharacter::ClearZoneFOV()
{
	bHasZoneFOV = false;
	if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(GetController()))
	{
		PC->ClientRestoreCameraFOV();
	}
}

float AWoTCharacter::GetZoneFOVForLeech() const
{
	if (const AWoTPlayerController* PC = Cast<AWoTPlayerController>(GetController()))
	{
		return PC->GetCameraFOVForTest();
	}
	return bHasZoneFOV ? PendingZoneFOV : 90.0f;
}

void AWoTCharacter::ApplyPendingZoneFOV()
{
	if (!bHasZoneFOV) return;
	if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(GetController()))
	{
		PC->ClientSetCameraFOV(PendingZoneFOV);
	}
}

void AWoTCharacter::ReconcileOwnerLocalZoneFOV()
{
	if (!IsLocallyControlled() || !GetWorld()) return;
	if (const UWoTZoneIndex* Zones = GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		if (AWoTZoneVolume* Region = Zones->GetRegion(this))
		{
			Region->ReconcileOwnerLocalFOV(this);
		}
	}
}

void AWoTCharacter::TeardownOwnerLocalZoneFOV()
{
	bool bDetachedThroughRegion = false;
	if (GetWorld())
	{
		if (const UWoTZoneIndex* Zones = GetWorld()->GetSubsystem<UWoTZoneIndex>())
		{
			if (AWoTZoneVolume* Region = Zones->GetRegion(this))
			{
				Region->DetachOwnerLocalFOV(this);
				bDetachedThroughRegion = true;
			}
		}
	}
	if (!bDetachedThroughRegion)
	{
		TArray<UWoTSmoothRandFOVLeech*> Leeches;
		GetComponents<UWoTSmoothRandFOVLeech>(Leeches);
		for (UWoTSmoothRandFOVLeech* Leech : Leeches)
		{
			if (IsValid(Leech)) Leech->Unattach();
		}
		if (bHasZoneFOV) ClearZoneFOV();
	}
}

void AWoTCharacter::UnPossessed()
{
	TeardownOwnerLocalZoneFOV();
	ExitPlayerMovementConstraint();
	ResetWalkingInputState();
	Super::UnPossessed();
}

void AWoTCharacter::EnsureAngrealHands()
{
	if (!IsLocallyControlled())
	{
		// Keep the long-standing headless/manual view-model seam: a caller may install an
		// AngrealHandSet without a local controller. Real simulated proxies install neither.
		if (!CurrentHandSet && AngrealHandSet)
		{
			CurrentHandSet = AngrealHandSet;
		}
		return;
	}
	ReconcileHandSet();
	const AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetController());
	if (IsValid(WoTController) && WoTController->IsOwnerEditingCitadel())
	{
		bCitadelEditorInputActive = true;
		ReconcileCitadelEditorHandSet();
		CurrentHandSet = CitadelEditorHandSet;
	}
	else
	{
		bCitadelEditorInputActive = false;
		CurrentHandSet = AngrealHandSet;
	}
}

void AWoTCharacter::ReconcileHandSet()
{
	if (!IsLocallyControlled())
	{
		return;
	}
	if (!AngrealHandSet)
	{
		AngrealHandSet = NewObject<UWoTHandSet>(this);
		AngrealHandSet->Init(this);
		// Guarded builds cook no /Game/WoT/Data; LoadObject would log a
		// missing-package error there, so only the dev build asks for the table.
		const UDataTable* HandTable = WOT_GUARDED_BOOTSTRAP ? nullptr
			: LoadObject<UDataTable>(nullptr,
				TEXT("/Game/WoT/Data/DT_WoTAngrealHands.DT_WoTAngrealHands"));
		if (HandTable != nullptr)
		{
			AngrealHandSet->PopulateFromTable(HandTable);
		}
		else
		{
			FString DefaultsError;
			if (const FWoTRuntimeClassDefaults* Defaults =
				UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(DefaultsError))
			{
				AngrealHandSet->PopulateFromRuntimeClassDefaults(*Defaults);
			}
		}
	}
	if (!InventoryComponent)
	{
		return;
	}

	// HandInfo.Update only re-resolves entries already marked as held. Reconcile
	// removals as well so a late owner-only array update cannot leave a stale flag.
	for (int32 HandIndex = 0; HandIndex < UWoTHandSet::NumHands; ++HandIndex)
	{
		UWoTHandInfo* HandInfo = AngrealHandSet->GetHand(HandIndex);
		for (int32 SlotIndex = 0; SlotIndex < UWoTHandInfo::NumSlots; ++SlotIndex)
		{
			const FName PlannedClass = HandInfo->ClassName[SlotIndex];
			if (!PlannedClass.IsNone() && HandInfo->bHaveThis[SlotIndex] &&
				!InventoryComponent->FindInventoryName(PlannedClass))
			{
				HandInfo->RemoveItem(PlannedClass);
			}
		}
	}
	// Owner-only replication can replace Items without executing AddInventory on
	// the receiving client. Fold any such newly visible entries into the model in
	// original acquisition order, without replaying entries already marked held.
	for (int32 ItemIndex = InventoryComponent->Items.Num() - 1; ItemIndex >= 0;
		--ItemIndex)
	{
		AWoTInventory* InventoryItem = InventoryComponent->Items[ItemIndex];
		if (!InventoryItem)
		{
			continue;
		}
		const FName OriginalClassName =
			UWoTInventoryComponent::GetHandSetItemName(InventoryItem);
		bool bAlreadyHeld = false;
		for (int32 HandIndex = 0; HandIndex < UWoTHandSet::NumHands; ++HandIndex)
		{
			if (AngrealHandSet->GetHand(HandIndex)->IsIn(OriginalClassName))
			{
				bAlreadyHeld = true;
				break;
			}
		}
		if (!bAlreadyHeld)
		{
			AngrealHandSet->AddItem(OriginalClassName, InventoryComponent);
		}
	}
	AngrealHandSet->Update(InventoryComponent);
	if (InventoryComponent->SelectedItem)
	{
		AngrealHandSet->SelectItem(UWoTInventoryComponent::GetHandSetItemName(
			InventoryComponent->SelectedItem));
	}
}

void AWoTCharacter::EnsureCitadelEditorHands()
{
	if (!IsLocallyControlled() || CitadelEditorHandSet)
	{
		return;
	}

	CitadelEditorHandSet = NewObject<UWoTHandSet>(this);
	CitadelEditorHandSet->Init(this);
	// CreateCitadelEditorHands (WOTPlayer.uc:4734). The seal/troop cards at
	// CE_SEALS/GRUNT/CAPTAIN (indices 0..2) are stamped by Reconcile, because the
	// troop adapter names are faction-specific and arrive in the budget mirror.
	static const FName PortedAdapters[UWoTHandSet::NumHands] = {
		NAME_None, NAME_None, NAME_None,
		TEXT("AlarmInventory"), TEXT("SpearInventory"),
		TEXT("FireWallInventory"), TEXT("WallInventory"), TEXT("PitInventory"),
		TEXT("StaircaseInventory"), TEXT("PortcullisInventory")};
	for (int32 HandIndex = 0; HandIndex < UWoTHandSet::NumHands; ++HandIndex)
	{
		const FName AdapterName = PortedAdapters[HandIndex];
		if (AdapterName.IsNone()
			|| !WoTCitadelResources::FindByAdapterName(AdapterName).IsValidDescriptor())
		{
			continue;
		}
		UWoTHandInfo* HandInfo = CitadelEditorHandSet->GetHand(HandIndex);
		HandInfo->AddClassName(AdapterName, 0);
		// Editor adapters are palette cards, not held/spawned inventory actors. Mark the
		// planned class visible and let the budget mirror supply its display count.
		HandInfo->AddItem(AdapterName, false, nullptr);
	}
	const int32 FirstPopulatedHand = CitadelEditorHandSet->GetNext();
	CitadelEditorHandSet->Select(FirstPopulatedHand);
}

void AWoTCharacter::ReconcileCitadelEditorHandSet()
{
	if (!IsLocallyControlled())
	{
		return;
	}
	EnsureCitadelEditorHands();
	AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetController());
	if (!CitadelEditorHandSet || !IsValid(WoTController))
	{
		return;
	}

	// Ensure selected the first populated static hand (indices 3+). Remember that
	// exact default so a late budget mirror may promote the newly complete palette
	// to retail's first hand without pulling a user away from a later choice.
	int32 EnsureDefaultHand = INDEX_NONE;
	for (int32 HandIndex = 3; HandIndex < UWoTHandSet::NumHands; ++HandIndex)
	{
		if (UWoTHandInfo* HandInfo = CitadelEditorHandSet->GetHand(HandIndex);
			HandInfo && !HandInfo->IsEmpty())
		{
			EnsureDefaultHand = HandIndex;
			break;
		}
	}
	const bool bSelectionStillEnsureDefault =
		CitadelEditorHandSet->Selected == EnsureDefaultHand;
	bool bStampedCard = false;
	auto StampCard = [this, &bStampedCard](int32 HandIndex, int32 SlotIndex,
		FName AdapterName)
	{
		if (AdapterName.IsNone()
			|| !WoTCitadelResources::FindByAdapterName(AdapterName).IsValidDescriptor())
		{
			return;
		}
		UWoTHandInfo* HandInfo = CitadelEditorHandSet->GetHand(HandIndex);
		if (!HandInfo || !HandInfo->ClassName[SlotIndex].IsNone())
		{
			return;
		}
		HandInfo->AddClassName(AdapterName, SlotIndex);
		HandInfo->AddItem(AdapterName, false, nullptr);
		bStampedCard = true;
	};

	// CreateCitadelEditorHands (WOTPlayer.uc:4734-4788): CE_SEALS is constant;
	// troop cards come from Budget.GetResourceType and are therefore per-faction.
	StampCard(0, 0, TEXT("SealInventory"));
	StampCard(1, 0, WoTController->GetOwnerCitadelTroopAdapter(TEXT("Grunt")));
	StampCard(2, 0, WoTController->GetOwnerCitadelTroopAdapter(TEXT("Captain")));
	StampCard(2, 1, WoTController->GetOwnerCitadelTroopAdapter(TEXT("Champion")));

	if (bStampedCard && bSelectionStillEnsureDefault)
	{
		// Re-run Ensure's GetNext/Select shape from just before hand zero. This makes
		// CE_SEALS the initial retail selection while preserving later user choices.
		CitadelEditorHandSet->Selected = UWoTHandSet::NumHands - 1;
		const int32 FirstPopulatedHand = CitadelEditorHandSet->GetNext();
		CitadelEditorHandSet->Select(FirstPopulatedHand);
	}
	for (int32 HandIndex = 0; HandIndex < UWoTHandSet::NumHands; ++HandIndex)
	{
		UWoTHandInfo* HandInfo = CitadelEditorHandSet->GetHand(HandIndex);
		if (!HandInfo)
		{
			continue;
		}
		for (int32 SlotIndex = 0; SlotIndex < UWoTHandInfo::NumSlots; ++SlotIndex)
		{
			const FName AdapterName = HandInfo->ClassName[SlotIndex];
			if (!AdapterName.IsNone())
			{
				HandInfo->SetDisplayCount(AdapterName,
					WoTController->GetOwnerCitadelBudgetCount(AdapterName));
			}
		}
	}
}

void AWoTCharacter::HandleCitadelEditStateChanged(bool bEditing)
{
	if (!IsLocallyControlled())
	{
		return;
	}
	if (bEditing)
	{
		bCitadelEditorInputActive = true;
		ReconcileCitadelEditorHandSet();
		CurrentHandSet = CitadelEditorHandSet;
	}
	else
	{
		bCitadelEditorInputActive = false;
		ReconcileHandSet();
		CurrentHandSet = AngrealHandSet;
	}
}

void AWoTCharacter::NotifyItemAdded(FName /*ItemClassName*/)
{
	ReconcileHandSet();
}

void AWoTCharacter::NotifyHandSelected()
{
	ReconcileHandSet();
	if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
	{
		if (AWoTHUD* WoTHUD = Cast<AWoTHUD>(PlayerController->GetHUD()))
		{
			WoTHUD->NotifyHandSelection();
		}
	}
}

void AWoTCharacter::ApplyFactionAppearance()
{
	// ChangeClassModelStuff (WOTPlayer.uc:4109): copy the class-default Mesh + skin + scale
	// onto this pawn. Source = the morph override when set, else our own class. The imported
	// mesh carries its *_Base skin materials, and the vertex-anim component self-feeds its
	// sequence table on bind — so one mesh set does mesh+skin+anims.
	// A neutral pawn can change faction appearance immediately before JoinTeam restores
	// STY_Normal. Drop the old mesh's overrides before rebinding, then regenerate the
	// ghost slots from the new mesh if the replicated style is still active.
	const bool bReapplyNeutralGhost = bNeutralGhost;
	if (bReapplyNeutralGhost)
	{
		RestoreNeutralGhostMaterials();
	}

	const UClass* SourceClass = AppearanceClass ? *AppearanceClass : GetClass();
	const AWoTCharacter* CDO = SourceClass->GetDefaultObject<AWoTCharacter>();
	if (CDO->BodyMeshPath.IsValid())
	{
		if (UWoTVertexAnimComponent* Body =
			Cast<UWoTVertexAnimComponent>(GetMesh());
			Body && Body->SetVertexMeshPath(CDO->BodyMeshPath))
		{
			// UE1 ChangeClassModelStuff copies both Mesh and Skin. Material overrides
			// belong to the previous mesh (including a just-restored ghost slot), so
			// clear them and expose the newly bound asset's authored faction skins.
			Body->EmptyOverrideMaterials();
			// UE1 DrawScale scaled the MESH only — collision stays the capsule.
			Body->SetRelativeScale3D(FVector(CDO->BodyDrawScale));
		}
	}
	TeamDescription = CDO->TeamDescription;
	bIsFemale = CDO->bIsFemale;
	// WOTPlayer.GetTroop keys the summon table from PlayerClass.Name. The enum is the
	// data-registry equivalent, so a Battle morph must copy it with the other class CDO
	// identity instead of retaining the pawn's original AesSedai fallback.
	SummonFaction = CDO->SummonFaction;
	// The class's shipped AnimationTableClass= ref rides the same morph (combat-anim
	// picks follow the appearance). Keep our own when the source class carries none.
	if (!CDO->AnimTableName.IsNone())
	{
		AnimTableName = CDO->AnimTableName;
	}
	if (!CDO->SoundTableName.IsNone())
	{
		SoundTableName = CDO->SoundTableName;
	}
	if (bReapplyNeutralGhost)
	{
		ApplyNeutralGhostLook();
	}
	// The rebind exposed the mesh's RAW authored materials (EmptyOverrideMaterials) —
	// re-layer the personal skin variant before any ghost reapply caches the slots.
	ApplyPersonalSkin();
	// The unconditional post-spawn re-bake may already be spent — without a
	// fresh bake the slots stay off the MeshLit path until the pawn moves 128uu
	// (indefinitely during a frozen Battle edit phase). No-op on modulated ghost
	// slots (no ML* params) and before the component's own BeginPlay bake arms.
	if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
	{
		MeshLight->ForceRuntimeBake();
	}
}

uint8 AWoTCharacter::ClassifySkinColor(const FString& SkinTextureName)
{
	// WOTPlayer.SetSkin (uc:2752-2757): InStr on the texture name, first match wins.
	// UnrealScript InStr is case-sensitive and the classification always runs on the
	// LOADED object's authored-case name (DynamicLoadObject is case-insensitive; the
	// resolved object carries authored case), so the port classifies resolved names too.
	if (SkinTextureName.Contains(TEXT("Black"), ESearchCase::CaseSensitive)) { return 1; }
	if (SkinTextureName.Contains(TEXT("Blue"), ESearchCase::CaseSensitive)) { return 2; }
	if (SkinTextureName.Contains(TEXT("Red"), ESearchCase::CaseSensitive)) { return 3; }
	if (SkinTextureName.Contains(TEXT("Green"), ESearchCase::CaseSensitive)) { return 4; }
	return 0;
}

void AWoTCharacter::ServerSetPersonalSkin(const FString& SkinName)
{
	if (!HasAuthority())
	{
		return;
	}
	// PlayerPawn.ServerChangeSkin + WOTPlayer.SetMultiSkin both gate on
	// Level.Game.bCanChangeSkin (WOTPlayer.uc:4216). The uc:4216
	// !giMission clause is vacuous in the MP port (no mission modes exist).
	const AWoTGameMode* Game = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!Game || !Game->bCanChangeSkin)
	{
		return;
	}
	// MeshName = GetItemName(string(Mesh)) (uc:4212) — the converter preserves the UE1
	// mesh name as the imported body-mesh asset name. The morph override's mesh governs,
	// exactly as retail reads the LIVE Mesh the last ChangeClassModelStuff installed.
	const UClass* SourceClass = AppearanceClass ? *AppearanceClass : GetClass();
	const AWoTCharacter* CDO = SourceClass->GetDefaultObject<AWoTCharacter>();
	const FString MeshName = CDO->BodyMeshPath.GetAssetName();
	if (MeshName.IsEmpty()
		|| !SkinName.StartsWith(MeshName, ESearchCase::IgnoreCase))
	{
		// Failing the package-prefix gate keeps the CURRENT skin (SetMultiSkin no-ops).
		return;
	}
	// DynamicLoadObject uses the COMPLETE supplied object name (WOTPlayer.uc:4212-4218).
	// Validate the converted /Textures/<Pkg>/<Name> asset before deriving the convention
	// MIC; otherwise a missing package can alias an unrelated same-leaf MI. A miss is
	// retail SetSkin(None), which restores the class default (uc:2738-2740).
	FString PackageName;
	FString ObjectName;
	int32 FirstSeparatorIndex = INDEX_NONE;
	int32 LastSeparatorIndex = INDEX_NONE;
	if (SkinName.FindChar(TEXT('.'), FirstSeparatorIndex)
		&& SkinName.FindLastChar(TEXT('.'), LastSeparatorIndex))
	{
		PackageName = SkinName.Left(FirstSeparatorIndex);
		ObjectName = SkinName.Mid(LastSeparatorIndex + 1);
	}
	FName ResolvedSkin = NAME_None;
	uint8 ResolvedColor = 0;
	if (!PackageName.IsEmpty() && !ObjectName.IsEmpty())
	{
		const FString TextureAssetPath = FString::Printf(
			TEXT("/Game/WoT/Textures/%s/%s.%s"),
			*PackageName, *ObjectName, *ObjectName);
		FWoTRuntimeTextureOptions TextureOptions;
		TextureOptions.bLoadMips = false;
		if (const UTexture* Texture = UWoTRuntimeAssets::ResolveTexturePath(
			FSoftObjectPath(TextureAssetPath), TextureOptions))
		{
			// Take the LOADED texture's authored-case name, as retail classified the
			// resolved texture object rather than the caller's spelling.
			const FString AuthoredName = Texture->GetName();
			const FString VariantMaterialPath = FString::Printf(
				TEXT("/Game/WoT/Materials/Instances/MI_%s.MI_%s"),
				*AuthoredName, *AuthoredName);
			if (
#if WOT_GUARDED_BOOTSTRAP
				true
#else
				FSoftObjectPath(VariantMaterialPath).TryLoad() != nullptr
#endif
			)
			{
				ResolvedSkin = FName(*AuthoredName);
				ResolvedColor = ClassifySkinColor(AuthoredName);
			}
		}
	}
	// else: SetSkin(None) — the four shipped *_Base defaults contain none of the four
	// substrings, so the class-default reset is Color 0 (uc:2740 + 2752-2757).
	PersonalSkin = ResolvedSkin;
	if (AWoTPlayerState* PS = GetPlayerState<AWoTPlayerState>())
	{
		PS->Color = ResolvedColor;
	}
	ApplyPersonalSkin();
}

void AWoTCharacter::OnRep_PersonalSkin()
{
	ApplyPersonalSkin();
}

void AWoTCharacter::ApplyPersonalSkin()
{
	ApplySkinVariant(PersonalSkin);
}

void AWoTCharacter::ApplySkinVariant(FName SkinName)
{
	UWoTVertexAnimComponent* Body =
		Cast<UWoTVertexAnimComponent>(GetMesh());
	if (!Body || !Body->HasDisplayMesh())
	{
		return;
	}
	// The ghost look owns every slot while active; rebind beneath it and re-cache, the
	// ApplyFactionAppearance bracket pattern (retail ghosting is a Style override — the
	// skin texture rides under it and must survive the unghost restore).
	const bool bReapplyNeutralGhost = bNeutralGhost
		&& NeutralGhostOriginalMaterials.Num() > 0;
	if (bReapplyNeutralGhost)
	{
		RestoreNeutralGhostMaterials();
	}
	if (SkinName.IsNone())
	{
		Body->ClearSkinOverrides();
	}
	else
	{
		const FString TexturePackage =
			Body->GetDisplayMeshName().ToString() + TEXT("Skins");
		Body->SetAllSkinOverrides(FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Textures/%s/%s.%s"), *TexturePackage,
			*SkinName.ToString(), *SkinName.ToString())));
	}
	if (bReapplyNeutralGhost)
	{
		ApplyNeutralGhostLook();
	}
}

void AWoTCharacter::FireLoopNotify(FName Function)
{
	if (Function == FName(TEXT("PlayMovementSound")))
	{
		// PlayMovementSound -> HandleMovingOnTexture (WOTPawn.uc:1117-1125 /
		// WOTPlayer.uc:2428-2434 -> GenericTextureHelper.uc:596-609): the floor
		// texture keys the wave. NPCs skip entirely with their feet in a water
		// zone (WOTPawn.uc:1118); players keep wading — their trace hits the
		// floor beneath the sheet.
		UWorld* World = GetWorld();
		if (!World)
		{
			return;
		}
		if (IsNPCPawn() &&
		    AWoTZoneVolume::IsPointInWater(
		        World, GetActorLocation() -
		                   FVector(0.0f, 0.0f, GetSimpleCollisionHalfHeight())))
		{
			return;
		}
		const bool bNeverQuiet =
		    WoTFootsteps::HelperForSoundTable(SoundTableName).bNeverWalkQuiet;
		PlaySurfaceSound(EWoTFootstepGroup::RunWalk,
		                 WoTFootsteps::VolumeMultForSpeed(GetVelocity().Size(),
		                                                  bNeverQuiet));
		return;
	}
	// PlayAnimSound (Grunt.uc:144-177): the sequence -> voice-slot switch.
	static const TMap<FName, FName> SequenceSlots = {
	    {FName(TEXT("Breath")), FName(TEXT("Breath"))},
	    {FName(TEXT("Listen")), FName(TEXT("AcceptOrders"))},
	    {FName(TEXT("Look")), FName(TEXT("Look"))},
	    {FName(TEXT("SeeEnemy")), FName(TEXT("SeeEnemy"))},
	    {FName(TEXT("ReactP")), FName(TEXT("ShowRespect"))},
	    {FName(TEXT("ReactPLoop")), FName(TEXT("ShowRespect"))},
	};
	if (const FName* Slot = SequenceSlots.Find(LoopSequence))
	{
		PlayVoiceSlot(*Slot);
	}
}

void AWoTCharacter::PlaySurfaceSound(EWoTFootstepGroup Group, float VolumeMult)
{
	// DetermineTextureEffect + DoTextureEffect (GenericTextureHelper.uc:535-574,
	// :242-253): resolve the floor surface, pick a row uniformly, perturb pitch
	// +-30% / volume +-50% (Util.PerturbFloatPercent), play at the engine-default
	// 1600uu radius (PlaySoundRadius 0), MakeNoise for AI hearing.
	UWorld* World = GetWorld();
	if (!World || !HasAuthority())
	{
		return;
	}
	const FWoTFootstepHelper& Helper =
	    WoTFootsteps::HelperForSoundTable(SoundTableName);

	// UE1 traced straight down CollisionHeight + 2*CollisionRadius from the
	// origin and resolved BSP textures only —
	// the port resolves ANY hit component's imported material name (movers and
	// meshes key their real surface; PORT_NOTES divergence).
	float CapsuleRadius = 0.0f, CapsuleHalfHeight = 0.0f;
	GetSimpleCollisionCylinder(CapsuleRadius, CapsuleHalfHeight);
	const FVector Start = GetActorLocation();
	const FVector End =
	    Start - FVector(0.0f, 0.0f, CapsuleHalfHeight + 2.0f * CapsuleRadius);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTFootstep),
	                             /*bTraceComplex*/ true, this);
	Params.bReturnFaceIndex = true;
	FHitResult Hit;
	EWoTSurface Surface = EWoTSurface::None;
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params) &&
	    Hit.Component.IsValid())
	{
		if (const AWoTRuntimeBSPWorld* BSPWorld =
			Cast<AWoTRuntimeBSPWorld>(Hit.GetActor()))
		{
			uint32 SurfaceIndex = MAX_uint32;
			FString TextureIdentity;
			if (BSPWorld->ResolveInvisibleSolidCollisionFace(Hit.GetComponent(),
				Hit.FaceIndex, SurfaceIndex, TextureIdentity))
			{
				Surface = WoTFootsteps::SurfaceFromTextureIdentity(TextureIdentity);
			}
		}
		UMaterialInterface* Mat = nullptr;
		if (Surface == EWoTSurface::None && Hit.FaceIndex != INDEX_NONE)
		{
			int32 SectionIndex = 0;
			Mat = Hit.Component->GetMaterialFromCollisionFaceIndex(Hit.FaceIndex,
			                                                       SectionIndex);
		}
		if (Surface == EWoTSurface::None && !Mat)
		{
			Mat = Hit.Component->GetMaterial(0);
		}
		if (Surface == EWoTSurface::None && Mat)
		{
			Surface = WoTFootsteps::SurfaceFromTextureName(
			    WoTFootsteps::TextureNameFromMaterialName(Mat->GetName()));
		}
	}
	if (Surface == EWoTSurface::None)
	{
		Surface = EWoTSurface::Stone;   // ConvertTexture fallback (:445-449)
	}

	const FName TableName =
	    Helper.OwnTable.IsNone() ? FName(TEXT("Generic")) : Helper.OwnTable;
	const FWoTFootstepRow* Row = WoTFootsteps::PickRow(TableName, Group, Surface);
	if (!Row)
	{
		return;
	}
	const float Volume = WoTRandom::FRandRange(0.5f, 1.5f) * Row->Volume *
	                     Helper.BaseVolume * VolumeMult;
	const float Pitch =
	    WoTRandom::FRandRange(0.7f, 1.3f) * Row->Pitch * Helper.BasePitch;
	WoTSound::PlayReplicated(*World, Row->SoundSubPath, GetActorLocation(),
	                         1600.0f, Volume, Pitch);
	// The row's effect (Puddle -> the WaterSplash droplet burst) spawns at the
	// feet (DoTextureEffect: Location - CollisionHeight*z, :242-253); the burst
	// itself sprays straight up regardless of rotation (WaterSplash.uc:10-15).
	if (!Row->EffectName.IsNone())
	{
		WoTWeaveFX::SpawnBurstMarker(
		    *World, Row->EffectName,
		    GetActorLocation() - FVector(0.0f, 0.0f, CapsuleHalfHeight),
		    FRotator(90.0f, 0.0f, 0.0f));
	}
	EmitNoise(FMath::Clamp(Volume, 0.0f, 1.0f));
}

void AWoTCharacter::EmitNoise(float Loudness)
{
	// Keep the engine noise (harmless with no emitter component today; faithful to
	// the shipped MakeNoise sites and ready for any future PawnSensing listener).
	MakeNoise(Loudness, this);
	// The AI-hearing fan-out is authority state (Grunt.OnHearNoise runs server-side);
	// the landing sites call unconditionally on server + client, so gate it here.
	if (HasAuthority())
	{
		AWoTNPCController::BroadcastNoise(GetWorld(), this, Loudness, this);
	}
}

void AWoTCharacter::Gasp()
{
	if (HasAuthority())
	{
		PlayVoiceSlot(FName(TEXT("Gasp")));
	}
}

bool AWoTCharacter::PlayVoiceSlot(FName Slot, float VolumeMult)
{
	return WoTSoundTable::PlaySlotSound(*this, Slot,
	                                    VolumeMult * SoundVolumeMultiplier,
	                                    SoundRadiusMultiplier, SoundPitchMultiplier);
}

float AWoTCharacter::SequenceDurationSeconds(FName Sequence, float RateMultiplier) const
{
	const UWoTVertexAnimComponent* Anim = Cast<UWoTVertexAnimComponent>(GetMesh());
	if (!Anim || RateMultiplier <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}
	for (const FWoTAnimSequence& S : Anim->Sequences)
	{
		if (S.Name == Sequence)
		{
			// The component clamps at NumFrames-1 (its non-loop end), so that is the
			// playback clock this mirrors.
			return (S.NumFrames > 1 && S.Rate > KINDA_SMALL_NUMBER)
			           ? (S.NumFrames - 1) / (S.Rate * RateMultiplier)
			           : 0.0f;
		}
	}
	return 0.0f;
}

float AWoTCharacter::LoopSequenceDurationSeconds(FName Sequence,
	float RateMultiplier) const
{
	const UWoTVertexAnimComponent* Anim = Cast<UWoTVertexAnimComponent>(GetMesh());
	if (!Anim || RateMultiplier <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}
	for (const FWoTAnimSequence& S : Anim->Sequences)
	{
		if (S.Name == Sequence)
		{
			// UWoTVertexAnimComponent loop playback wraps LocalFrame with
			// fmod(LocalFrame, NumFrames), so every authored frame contributes
			// one interval. One-shot playback intentionally uses NumFrames-1.
			return (S.NumFrames > 0 && S.Rate > KINDA_SMALL_NUMBER)
				? S.NumFrames / (S.Rate * RateMultiplier)
				: 0.0f;
		}
	}
	return 0.0f;
}

void AWoTCharacter::HoldLocomotionFor(float Seconds)
{
	if (Seconds <= 0.0f || !GetWorld())
	{
		return;
	}
	LocomotionHoldUntil =
	    FMath::Max(LocomotionHoldUntil, GetWorld()->GetTimeSeconds() + Seconds);
	// Force a locomotion re-stamp when the hold expires even if the state (Run/Breath)
	// didn't change — the one-shot replaced the looping anim on every machine.
	DrivenLocomotionAnim = NAME_None;
}

const FWoTAnimTableEntry* AWoTCharacter::StampSlotAnim(FName SlotName)
{
	if (!HasAuthority())
	{
		return nullptr;
	}
	const FWoTAnimTableEntry* Pick =
	    WoTAnimTable::PickSlotAnim(AnimTableName, SlotName, this);
	if (!Pick)
	{
		return nullptr;
	}
	const float Rate = WoTAnimTable::ResolveRate(Pick);
	const float TweenTime = WoTAnimTable::ResolveTweenTime(Pick);
	SetTweenAnimIntent(Pick->AnimName, Rate, TweenTime, false);
	// The native clock enters the sequence only after its negative-frame tween
	// reaches zero, so hold across both intervals.
	const float Duration = SequenceDurationSeconds(Pick->AnimName, Rate);
	const UWoTVertexAnimComponent* Anim = Cast<UWoTVertexAnimComponent>(GetMesh());
	const bool bKnownSequence = Anim && Anim->FindSequence(Pick->AnimName);
	HoldLocomotionFor(TweenTime + (bKnownSequence ? Duration
		: (Duration > 0.0f ? Duration : 1.0f)));
	return Pick;
}

const FWoTAnimTableEntry* AWoTCharacter::LoopSlotAnim(FName SlotName)
{
	if (!HasAuthority()) return nullptr;
	const FWoTAnimTableEntry* Pick =
		WoTAnimTable::PickSlotAnim(AnimTableName, SlotName, this);
	if (!Pick) return nullptr;
	SetAnimIntent(Pick->AnimName, WoTAnimTable::ResolveRate(Pick), true);
	return Pick;
}

const FWoTAnimTableEntry* AWoTCharacter::TweenSlotAnim(FName SlotName)
{
	if (!HasAuthority())
	{
		return nullptr;
	}
	const FWoTAnimTableEntry* Pick =
		WoTAnimTable::PickSlotAnim(AnimTableName, SlotName, this);
	if (!Pick)
	{
		return nullptr;
	}
	const float TweenTime = WoTAnimTable::ResolveTweenTime(Pick);
	SetTweenAnimIntent(Pick->AnimName, 0.0f, TweenTime, true);
	HoldLocomotionFor(TweenTime);
	return Pick;
}

void AWoTCharacter::PlayTakeHitVoice(float Damage, FName WoTDamageType)
{
	// WOTPlayer.PlayTakeHitSound suppresses fall-damage pain entirely before it
	// advances the 1-3 second voice window (WOTPlayer.uc:2197-2220). NPCs inherit
	// WOTPawn's ordinary HitSoft/HitHard sound for the same damage type.
	if (!IsNPCPawn() && WoTDamageType == FName(TEXT("fell")))
	{
		return;
	}
	if (!HasAuthority() || !GetWorld() ||
		GetWorld()->GetTimeSeconds() < NextPainSoundTime)
	{
		return;
	}
	NextPainSoundTime = GetWorld()->GetTimeSeconds() + WoTRandom::FRandRange(1.0f, 3.0f);
	if (WoTDamageType == FName(TEXT("Drowned")))
	{
		PlayVoiceSlot(FName(TEXT("Drown")));
	}
	else
	{
		PlayVoiceSlot(Damage >= HitHardHealthRatio * HitReferenceHealth
			? FName(TEXT("HitHard")) : FName(TEXT("HitSoft")));
	}
}

void AWoTCharacter::PlayHitReactAnim(int32 Damage, FName WoTDamageType,
	float MomentumZ, bool bEnforceProductionEligibility)
{
	const bool bPlayerPawn = !IsNPCPawn();
	// WOTPlayer.PlayHit returns before its sound for non-positive damage. NPCs keep
	// WOTPawn's pain-sound-first behavior, including zero-damage pain effects.
	if (bPlayerPawn && Damage <= 0)
	{
		return;
	}
	// PlayTakeHitSound (WOTPawn.uc:996-1015) runs first, on its own 1-3s window:
	// 'drowned' short-circuits to the Drown slot (WOTPawn.uc:1002-1005 /
	// WOTPlayer.uc:2205-2209) before the HitHard/HitSoft split. Authority;
	// networked via the voice path. (FName comparison is case-insensitive, so the
	// engine-emitted 'Drowned' matches the script's 'drowned'.)
	PlayTakeHitVoice(static_cast<float>(Damage), WoTDamageType);

	if (!HasAuthority() || bDead || Damage <= 0)
	{
		return;
	}
	// WOTPlayer.PlayHit has no ChanceOfHitAnim gate: production damage animates
	// only above the strict 25%-health threshold or for MomentumZ > 300, then
	// PlayTakeHit deterministically selects hard through IsHardHit's authored
	// ratio, otherwise soft. Direct calls deliberately bypass only the eligibility
	// test so state specs can drive the presentation seam with authored damage sizes.
	if (bPlayerPawn)
	{
		const bool bHardHit = Damage >= HitHardHealthRatio * HitReferenceHealth;
		if (bEnforceProductionEligibility && !PlayerHitQualifies(Damage, MomentumZ))
		{
			return;
		}
		if (bHardHit)
		{
			StampSlotAnim(FName(TEXT("HitHard")));
		}
		else
		{
			TweenSlotAnim(FName(TEXT("Hit")));
		}
		return;
	}

	// WOTPawn.PlayHit: only a ChanceOfHitAnim fraction of surviving NPC hits
	// flinch. Slot by damage size against authored default health; HitF-vs-HitB
	// remains the table's own weighted choice rather than hit direction.
	FName Slot;
	const bool bHardHit = Damage >= HitHardHealthRatio * HitReferenceHealth;
	if (bHardHit)
	{
		Slot = FName(TEXT("HitHard"));
	}
	else if (Damage >= HitSoftHealthRatio * HitReferenceHealth)
	{
		Slot = FName(TEXT("Hit"));
	}
	else
	{
		return;
	}
	if (WoTRandom::FRand() < ChanceOfHitAnim)
	{
		if (AWoTNPCController* NPCController = Cast<AWoTNPCController>(GetController()))
		{
			// Archer's PerformProjectileAttack state overrides GetHitAnimSeqName while
			// kneeling (Archer.uc:186-196). All state suppression and stored return
			// handling belongs to the controller's TakeHit detour.
			if (NPCController->IsArcherKneeling())
			{
				Slot = bHardHit ? FName(TEXT("HitHardKneel")) : FName(TEXT("HitKneel"));
			}
			NPCController->TryEnterTakeHit(Slot, bHardHit);
		}
		else if (bHardHit)
		{
			StampSlotAnim(Slot);
		}
		else
		{
			TweenSlotAnim(Slot);
		}
	}
}

void AWoTCharacter::SetAppearanceClass(TSubclassOf<AWoTCharacter> NewClass)
{
	if (!HasAuthority())
	{
		return;
	}
	// ServerChangeClass updates PlayerReplicationInfo.PlayerType in the same operation as
	// the model morph (WOTPlayer.uc:4161-4202). Keep replicated identity synchronized even
	// if the requested appearance is already active.
	if (AWoTPlayerState* PS = GetPlayerState<AWoTPlayerState>())
	{
		PS->SetFactionClass(NewClass);
	}
	if (AppearanceClass == NewClass)
	{
		return;
	}
	AppearanceClass = NewClass;
	// ChangeClassModelStuff ends with SetSkin(NewClass.default.MultiSkins[1]) (uc:4123):
	// a real class morph RESETS the personal skin to the new class's Base default. UE1's
	// pawn was also the persistent selection store (WOTPlayer.uc:4109-4123); clear the
	// split controller store in the same authority operation so possession cannot revive it.
	PersonalSkin = NAME_None;
	if (AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetController()))
	{
		WoTController->PersonalSkinURL.Reset();
	}
	if (AWoTPlayerState* PS = GetPlayerState<AWoTPlayerState>())
	{
		PS->Color = 0;
	}
	ApplyFactionAppearance();
}

void AWoTCharacter::OnRep_AppearanceClass()
{
	ApplyFactionAppearance();
}

// --- Disguise visible morph (AngrealInvDisguise, uc:179-458) ---

TSubclassOf<AWoTCharacter> AWoTCharacter::GetEffectiveAppearanceClass() const
{
	// uc:281-283: disguising as someone who is themselves disguised copies THEIR
	// current look — chase the source's own driver first, then any faction morph.
	if (Disguise.bActive && Disguise.SourceClass)
	{
		return Disguise.SourceClass;
	}
	if (AppearanceClass)
	{
		return AppearanceClass;
	}
	return GetClass();
}

FName AWoTCharacter::GetEffectivePersonalSkin() const
{
	return Disguise.bActive ? Disguise.SourcePersonalSkin : PersonalSkin;
}

bool AWoTCharacter::ApplyDisguiseFrom(APawn* Source)
{
	// Authority builds the driver from the source's LIVE look; clients follow the
	// replicated driver through OnRep_Disguise (raw mesh/scale/materials don't
	// replicate natively). uc:229-384 read the live Source, not a class CDO.
	AWoTCharacter* SourceCh = Cast<AWoTCharacter>(Source);
	if (!HasAuthority() || !IsValid(SourceCh))
	{
		// "disguised NPCs not supported" (uc:275/380) — every port pawn is an
		// AWoTCharacter, so a non-character source is simply un-mimickable.
		return false;
	}

	FWoTDisguiseState NewState;
	NewState.bActive = 1;
	NewState.SourceClass = SourceCh->GetEffectiveAppearanceClass();
	NewState.SourcePersonalSkin = SourceCh->GetEffectivePersonalSkin();
	if (const USkeletalMeshComponent* SourceBody = SourceCh->GetMesh())
	{
		NewState.SourceMeshScale = SourceBody->GetRelativeScale3D();
	}
	if (const UCapsuleComponent* SourceCapsule = SourceCh->GetCapsuleComponent())
	{
		NewState.SourceCapsuleRadius = SourceCapsule->GetScaledCapsuleRadius();
		NewState.SourceCapsuleHalfHeight = SourceCapsule->GetScaledCapsuleHalfHeight();
	}
	NewState.SourceBaseEyeHeight = SourceCh->BaseEyeHeight;
	NewState.SourceAnimTableName = SourceCh->AnimTableName;
	NewState.SourceVisibility = SourceCh->Visibility;
	if (const UWoTVertexAnimComponent* SourceBody =
		Cast<UWoTVertexAnimComponent>(SourceCh->GetMesh()))
	{
		NewState.SourceRuntimeMeshLogicalId = SourceBody->GetVertexMeshLogicalId();
		NewState.SourceRuntimeAnimationLogicalId = SourceBody->GetVertexAnimationLogicalId();
		for (const TPair<int32, FSoftObjectPath>& Pair :
			SourceBody->GetRuntimeSkinOverrides())
		{
			FWoTDisguiseSkinIdentity& Skin =
				NewState.SourceRuntimeSkins.AddDefaulted_GetRef();
			Skin.Slot = Pair.Key;
			Skin.Texture = FSoftObjectPath(Pair.Value.GetAssetPathString());
		}
		NewState.SourceRuntimeSkins.Sort([](const FWoTDisguiseSkinIdentity& A,
			const FWoTDisguiseSkinIdentity& B) { return A.Slot < B.Slot; });
	}
	NewState.SourceAmbientGlow = SourceCh->AmbientGlow;
	NewState.SourceScaleGlow = SourceCh->ScaleGlow;
	NewState.SourceStyle = SourceCh->IsNeutralGhost() ? 1 : 0;
	if (const AWoTNPCCharacter* SourceNPC = Cast<AWoTNPCCharacter>(SourceCh))
	{
		if (SourceNPC->GetFadeSkinIndex()
			!= AWoTNPCCharacter::VisibleFadeSkinIndex)
		{
			NewState.SourceStyle = 2;
		}
		if (const FWoTCreatureRow* Row = WoTCreatureData::Find(SourceNPC->CreatureRowName))
		{
			NewState.SourceDefaultWalkSpeed = Row->GroundSpeed;
			FWoTRetailObjectPath Path;
			if (WoTParseRetailObjectPath(Row->DisguiseIcon.ToString(), Path))
				NewState.SourceDisguiseIcon = FSoftObjectPath(FString::Printf(
					TEXT("/Game/WoT/Textures/%s.%s"),
					*WoTRetailTextureSubPath(Path), *Path.Name));
		}
	}
	else if (NewState.SourceClass)
	{
		NewState.SourceDisguiseIcon = NewState.SourceClass
			->GetDefaultObject<AWoTCharacter>()->DisguiseIcon.ToSoftObjectPath();
		if (const UCharacterMovementComponent* DefaultMove = NewState.SourceClass
			->GetDefaultObject<AWoTCharacter>()->GetCharacterMovement())
		{
			NewState.SourceDefaultWalkSpeed = DefaultMove->MaxWalkSpeed;
		}
	}
	if (const UCharacterMovementComponent* SourceMove = SourceCh->GetCharacterMovement())
	{
		NewState.SourceWalkSpeed = SourceMove->MaxWalkSpeed;
		NewState.SourceSwimSpeed = SourceMove->MaxSwimSpeed;
		NewState.SourceAcceleration = SourceMove->MaxAcceleration;
		NewState.SourceAirControl = SourceMove->AirControl;
	}
	NewState.bSourceNeutralGhost = SourceCh->IsNeutralGhost() ? 1 : 0;

	// Commit tentatively so ApplyDisguiseLook reads it; ApplyDisguiseLook captures
	// our current look into SavedDisguise, applies the source look, and reports the
	// won't-fit collision abort (uc:317-321).
	const FWoTDisguiseState Prev = Disguise;
	Disguise = NewState;
	if (!ApplyDisguiseLook())
	{
		// Won't fit — restore the snapshot and roll the driver back; the artifact
		// then Fails the cast and drains no charge.
		RestoreDisguiseLook();
		Disguise = Prev;
		return false;
	}
	return true;
}

void AWoTCharacter::RevertDisguise()
{
	if (!HasAuthority())
	{
		return;
	}
	RestoreDisguiseLook();               // authority restores locally (OnRep fires only on clients)
	Disguise = FWoTDisguiseState();      // bActive=0 replicates -> clients OnRep restore
}

void AWoTCharacter::OnRep_Disguise()
{
	if (Disguise.bActive)
	{
		// A coalesced active->active transition (uncast + recast-different-source
		// inside one net update) would early-return on the bDisguiseLookActive
		// guard and strand the OLD look (a latent desync): restore to
		// the true base first, then apply the new driver.
		if (bDisguiseLookActive)
		{
			RestoreDisguiseLook();
		}
		ApplyDisguiseLook();
	}
	else
	{
		RestoreDisguiseLook();
	}
}

bool AWoTCharacter::ApplyDisguiseLook()
{
	if (bDisguiseLookActive)
	{
		return true;   // already morphed (idempotent OnRep / re-entry guard)
	}
	// SaveDisguisePawnProperties (uc:229-277): snapshot the current look before it
	// is overwritten, so RevertDisguise can restore it locally on each machine.
	FWoTSavedDisguise& S = SavedDisguise;
	S = FWoTSavedDisguise();
	S.bValid = 1;
	if (UWoTVertexAnimComponent* Body =
		Cast<UWoTVertexAnimComponent>(GetMesh()))
	{
		S.Mesh = Body->GetSkeletalMeshAsset();
		S.RuntimeMeshLogicalId = Body->GetVertexMeshLogicalId();
		S.RuntimeAnimationLogicalId = Body->GetVertexAnimationLogicalId();
		S.RuntimeSkinOverrides = Body->GetRuntimeSkinOverrides();
		S.OverrideMaterials = Body->OverrideMaterials;
		S.MeshScale = Body->GetRelativeScale3D();
	}
	if (const UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		S.CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		S.CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	}
	S.BaseEyeHeight = BaseEyeHeight;
	S.AnimTableName = AnimTableName;
	S.Visibility = Visibility;
	S.AmbientGlow = AmbientGlow;
	S.ScaleGlow = ScaleGlow;
	if (const UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		S.WalkSpeed = Move->MaxWalkSpeed;
		S.SwimSpeed = Move->MaxSwimSpeed;
		S.Acceleration = Move->MaxAcceleration;
		S.AirControl = Move->AirControl;
	}
	bDisguiseLookActive = true;

	// CopyDisguisePawnProperties (uc:285-384): rebuild the source look on top.
	UWoTVertexAnimComponent* Body =
		Cast<UWoTVertexAnimComponent>(GetMesh());
	if (Body && Disguise.SourceClass)
	{
		const AWoTCharacter* CDO = Disguise.SourceClass->GetDefaultObject<AWoTCharacter>();
		if (CDO->BodyMeshPath.IsValid())
		{
			if (Body->SetVertexMeshPath(CDO->BodyMeshPath))
			{
				Body->EmptyOverrideMaterials();
			}
		}
	}
	if (Body && !Disguise.SourceRuntimeMeshLogicalId.IsEmpty())
	{
		Body->SetVertexMeshLogicalIds(Disguise.SourceRuntimeMeshLogicalId,
			Disguise.SourceRuntimeAnimationLogicalId);
	}
	if (Body)
	{
		Body->SetRelativeScale3D(Disguise.SourceMeshScale);   // uc:293 DrawScale
	}
	// The source's live skin variant rides the mesh (uc:323 MultiSkins copy).
	// uc:329-330's Style/Visibility copy: disguising as a ghost source carries
	// the translucent ghost material, while reverting restores the saved look.
	RestoreNeutralGhostMaterials();
	ApplySkinVariant(Disguise.SourcePersonalSkin);
	if (Body && !Disguise.SourceRuntimeSkins.IsEmpty())
	{
		TMap<int32, FSoftObjectPath> RuntimeSkins;
		for (const FWoTDisguiseSkinIdentity& Skin : Disguise.SourceRuntimeSkins)
		{
			if (Skin.Slot >= 0 && !Skin.Texture.IsNull())
				RuntimeSkins.Add(Skin.Slot, Skin.Texture);
		}
		Body->SetRuntimeSkinOverrides(RuntimeSkins);
		if (Disguise.SourceStyle == 2 && GetNetMode() != NM_DedicatedServer)
		{
			for (int32 Slot = 0; Slot < Body->GetNumMaterials(); ++Slot)
			{
				if (UMaterialInstanceDynamic* Modulated =
					WoTMeshModulate::CreateInstance(this, Body->GetMaterial(Slot)))
				{
					Body->SetMaterial(Slot, Modulated);
				}
			}
		}
	}
	Visibility = Disguise.SourceVisibility;
	AmbientGlow = Disguise.SourceAmbientGlow;
	SetScaleGlowPresentation(Disguise.SourceScaleGlow);
	ApplyNeutralGhostLook();
	AnimTableName = Disguise.SourceAnimTableName;            // uc:365/369
	BaseEyeHeight = Disguise.SourceBaseEyeHeight;            // uc:332
	RetailStandingBaseEyeHeight = BaseEyeHeight;
	// AngrealInvDisguise.uc:339-351: movement traits are part of a disguise only
	// in NM_Standalone. Multiplayer changes the look and apparent team, never the
	// server-authoritative movement envelope.
	if (GetNetMode() == NM_Standalone)
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->MaxWalkSpeed = Disguise.SourceWalkSpeed;      // ScaleSpeedSettings(1.0)
		Move->MaxSwimSpeed = Disguise.SourceSwimSpeed;      //   uc:1926-1931
		Move->MaxAcceleration = Disguise.SourceAcceleration;
		Move->AirControl = Disguise.SourceAirControl;
	}
	// Capsule + feet-on-ground last, so the won't-fit abort can bail cleanly.
	return AdjustCapsuleForDisguise(
		Disguise.SourceCapsuleRadius, Disguise.SourceCapsuleHalfHeight);
}

void AWoTCharacter::RestoreDisguiseLook()
{
	if (!bDisguiseLookActive)
	{
		return;
	}
	bDisguiseLookActive = false;
	const FWoTSavedDisguise& S = SavedDisguise;
	if (S.bValid)
	{
		if (UWoTVertexAnimComponent* Body =
			Cast<UWoTVertexAnimComponent>(GetMesh()))
		{
			if (!S.RuntimeMeshLogicalId.IsEmpty())
			{
				if (Body->SetVertexMeshLogicalIds(S.RuntimeMeshLogicalId,
					S.RuntimeAnimationLogicalId))
				{
					Body->SetRuntimeSkinOverrides(S.RuntimeSkinOverrides);
				}
			}
			else
			{
				Body->SetSkeletalMesh(S.Mesh);
			}
			Body->EmptyOverrideMaterials();
			for (int32 Slot = 0; Slot < S.OverrideMaterials.Num(); ++Slot)
			{
				if (S.OverrideMaterials[Slot])
				{
					Body->SetMaterial(Slot, S.OverrideMaterials[Slot]);
				}
			}
			Body->SetRelativeScale3D(S.MeshScale);
		}
		AnimTableName = S.AnimTableName;
		Visibility = S.Visibility;
		AmbientGlow = S.AmbientGlow;
		SetScaleGlowPresentation(S.ScaleGlow);
		BaseEyeHeight = S.BaseEyeHeight;
		RetailStandingBaseEyeHeight = BaseEyeHeight;
		if (UCharacterMovementComponent* Move = GetCharacterMovement())
		{
			Move->MaxWalkSpeed = S.WalkSpeed;
			Move->MaxSwimSpeed = S.SwimSpeed;
			Move->MaxAcceleration = S.Acceleration;
			Move->AirControl = S.AirControl;
		}
		// AdjustToNewCollisionHeight might fail here in rare cases -- so what (uc:420).
		AdjustCapsuleForDisguise(S.CapsuleRadius, S.CapsuleHalfHeight);
	}
	SavedDisguise = FWoTSavedDisguise();
	ApplyNeutralGhostLook();
}

bool AWoTCharacter::AdjustCapsuleForDisguise(float NewRadius, float NewHalfHeight)
{
	UCapsuleComponent* Capsule = GetCapsuleComponent();
	if (!Capsule)
	{
		return true;
	}
	const float OldHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	// Set collision size before moving Z, or SetLocation might fail (uc:312-314).
	Capsule->SetCapsuleSize(NewRadius, NewHalfHeight);

	// Move feet-on-ground: shift by the half-height delta (uc:210-211).
	FVector NewLocation = GetActorLocation();
	NewLocation.Z += (NewHalfHeight - OldHalfHeight);

	// Util.ActorFits (uc:214): does the new capsule fit at the shifted location?
	bool bFits = true;
	if (const UWorld* World = GetWorld())
	{
		FCollisionQueryParams Params(FName(TEXT("DisguiseFit")), false, this);
		bFits = !World->OverlapBlockingTestByChannel(
			NewLocation, FQuat::Identity, Capsule->GetCollisionObjectType(),
			FCollisionShape::MakeCapsule(NewRadius, NewHalfHeight), Params);
	}
	if (bFits)
	{
		if (SetActorLocation(NewLocation))
		{
			UWoTZoneIndex::RefreshAfterRelocation(this);
		}
	}
	return bFits;
}

void AWoTCharacter::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTCharacter, Health);
	DOREPLIFETIME(AWoTCharacter, ScaleGlow);
	DOREPLIFETIME(AWoTCharacter, bDead);
	DOREPLIFETIME(AWoTCharacter, bGibbedDeath);
	DOREPLIFETIME(AWoTCharacter, PlayerMovementConstraint);
	// The mid-match faction morph (UE1 replicated Mesh/Skin natively; the port replicates
	// the class whose CDO carries them).
	DOREPLIFETIME(AWoTCharacter, AppearanceClass);
	// The personal ?Skin= variant (UE1 replicated Actor.Skin/MultiSkins natively; the
	// port replicates the accepted texture name and rebinds convention materials).
	DOREPLIFETIME(AWoTCharacter, PersonalSkin);
	// Owner-gated: non-owners get server-driven anim (UE1 !bNetOwner); the owning
	// client self-animates and the server doesn't stomp it.
	DOREPLIFETIME_CONDITION(AWoTCharacter, RepAnim, COND_SkipOwner);
	// Owner-only (WOTPlayer.uc `Role==ROLE_Authority && bNetOwner`).
	DOREPLIFETIME_CONDITION(AWoTCharacter, ApparentClass, COND_OwnerOnly);
	// The visible disguise morph must fool EVERY client (COND_None), unlike the
	// owner-only ApparentClass anim/HUD surface. UE1 replicated the raw Mesh/skins
	// natively; the port replicates the source-appearance inputs and rebuilds locally.
	DOREPLIFETIME(AWoTCharacter, Disguise);
	DOREPLIFETIME_CONDITION(AWoTCharacter, StatusIcons,
		StatusIconsReplicationCondition());
	DOREPLIFETIME_CONDITION(AWoTCharacter, SeekerCount, COND_OwnerOnly);
	// Owner-only breath mirror for the debug HUD (gasp/drowning is the owner's
	// local concern; nobody else reads another pawn's breath).
	DOREPLIFETIME_CONDITION(AWoTCharacter, BreathDisplay, COND_OwnerOnly);
	// The looping hum (Legion) — clients attach the local loop off these.
	DOREPLIFETIME(AWoTCharacter, AmbientHumPath);
	DOREPLIFETIME(AWoTCharacter, AmbientHumRadius);
	DOREPLIFETIME(AWoTCharacter, AmbientHumVolume);
	DOREPLIFETIME(AWoTCharacter, AmbientHumPitch);
	// The orbit-editor body ghost (WOTPlayer CitadelOrbitMode): every machine needs the
	// collision half locally (UE1 replicated bCollideActors; UE5 does not).
	DOREPLIFETIME(AWoTCharacter, bCitadelOrbitGhosted);
	// Actor.Style replicated without an owner condition in UE1. The explicit COND_None
	// keeps the neutral ghost visible to every client, not merely the owning player.
	DOREPLIFETIME_CONDITION(AWoTCharacter, bNeutralGhost, COND_None);
	DOREPLIFETIME_CONDITION(AWoTCharacter, TouchReflectorDescriptions, COND_None);
}

void AWoTCharacter::SetupPlayerInputComponent(UInputComponent* Input)
{
	Super::SetupPlayerInputComponent(Input);
	// The project currently uses legacy action/axis names from DefaultInput.ini
	// for movement, fire, and the shipped hand controls alike; no mapping context
	// is installed on this pawn.
	Input->BindAxis("MoveForward", this, &AWoTCharacter::MoveForward);
	Input->BindAxis("MoveRight", this, &AWoTCharacter::MoveRight);
	Input->BindAxis("CitadelOrbitZoom", this, &AWoTCharacter::CitadelOrbitZoom);
	Input->BindAxis("Turn", this, &AWoTCharacter::TurnInput);
	Input->BindAxis("LookUp", this, &AWoTCharacter::KeyboardLookInput);
	Input->BindAxis("MouseX", this, &AWoTCharacter::MouseXInput);
	Input->BindAxis("MouseY", this, &AWoTCharacter::MouseYInput);
	Input->BindAction("Walking", IE_Pressed, this, &AWoTCharacter::WalkingPressed);
	Input->BindAction("Walking", IE_Released,
		this, &AWoTCharacter::WalkingReleased).bExecuteWhenPaused = true;
	Input->BindAction("Look", IE_Pressed, this, &AWoTCharacter::LookPressed);
	Input->BindAction("Look", IE_Released,
		this, &AWoTCharacter::LookReleased).bExecuteWhenPaused = true;
	Input->BindAction("LookToggle", IE_Pressed, this, &AWoTCharacter::LookTogglePressed);
	Input->BindAction("FreeLook", IE_Pressed, this, &AWoTCharacter::FreeLookPressed);
	Input->BindAction("FreeLook", IE_Released,
		this, &AWoTCharacter::FreeLookReleased).bExecuteWhenPaused = true;
	Input->BindAction("CenterView", IE_Pressed, this, &AWoTCharacter::CenterViewPressed);
	Input->BindAction("CenterView", IE_Released,
		this, &AWoTCharacter::CenterViewReleased).bExecuteWhenPaused = true;
	Input->BindAction("Strafe", IE_Pressed, this, &AWoTCharacter::StrafePressed);
	Input->BindAction("Strafe", IE_Released,
		this, &AWoTCharacter::StrafeReleased).bExecuteWhenPaused = true;
	// Fire AND Jump (both edges each) are controller-owned: the controller survives the
	// unpossessed Dying state (retail respawns on the jump key there too — engine
	// PlayerPawn Dying.PlayerMove), and a controller-level binding consumes the key
	// on both edges for everything below it in the input stack — a pawn-side binding
	// here would either starve or (for Jump) steal the dead player's respawn press.
	// The controller forwards live presses to ACharacter::Jump/StopJumping.
	Input->BindAction("NextItem", IE_Pressed, this, &AWoTCharacter::NextItem);
	Input->BindAction("PrevItem", IE_Pressed, this, &AWoTCharacter::PrevItem);
	Input->BindAction("NextHand", IE_Pressed, this, &AWoTCharacter::NextHand);
	Input->BindAction("PrevHand", IE_Pressed, this, &AWoTCharacter::PrevHand);
	Input->BindAction("Drop", IE_Pressed, this, &AWoTCharacter::Drop);
	Input->BindAction("DropTainted", IE_Pressed, this, &AWoTCharacter::DropTainted);
	for (int32 HandIndex = 0; HandIndex < UWoTHandSet::NumHands; ++HandIndex)
	{
		const int32 DisplayDigit = (HandIndex + 1) % UWoTHandSet::NumHands;
		FInputActionBinding Binding(
			FName(*FString::Printf(TEXT("SelectHand%d"), DisplayDigit)), IE_Pressed);
		Binding.ActionDelegate.GetDelegateForManualSet().BindWeakLambda(
			this, [this, HandIndex]()
			{
				const AWoTPlayerController* WoTController =
					Cast<AWoTPlayerController>(GetController());
				AWoTHUD* WoTHUD = WoTController
					? Cast<AWoTHUD>(WoTController->GetHUD()) : nullptr;
				if (!WoTHUD || !WoTHUD->HandleTeamSelectDigit(HandIndex))
				{
					SelectHand(HandIndex);
				}
			});
		Input->AddActionBinding(MoveTemp(Binding));
	}
}

void AWoTCharacter::UseAngreal()
{
	if (IsHardLanding())
	{
		return;
	}
	AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetController());
	if (bCitadelEditorInputActive && IsValid(WoTController)
		&& WoTController->IsOwnerEditingCitadel())
	{
		bCitadelEditorFireHeld = true;
		WoTController->HandleCitadelEditorClick();
		return;
	}
	bCitadelEditorFireHeld = false;
	// WOTPlayer.UseAngreal (WOTPlayer.uc:1088-1098): before Citadel play begins,
	// bTeleportingDisabled suppresses casting; a teamed player's input asks the
	// authority to assess readiness. The server re-checks budgets/editing and answers with the
	// Waiting* message or EnableTeleporters).
	if (IsValid(WoTController) && WoTController->bTeleportingDisabled)
	{
		const AWoTPlayerState* PS = GetPlayerState<AWoTPlayerState>();
		if (PS && PS->Team != 255)
		{
			WoTController->ServerAssessBattlefield();
		}
		return;
	}
	// WOTPlayer.GameEnded.Fire: once the match is decided, fire no longer casts — it becomes
	// the restart request (ServerReStartGame, gated by the PlayerRestartGameDelay freeze).
	// Checked on the owning client (bMatchEnded replicates owner-only), so a frozen player's
	// weave input never leaves the box.
	if (IsValid(WoTController))
	{
		if (WoTController->IsMatchEnded())
		{
			WoTController->ServerRestartGame();
			return;
		}
	}
	// WOTPlayer.UseAngreal's empty-slot branch: the press is consumed by an
	// auto-selection (same hand first, otherwise the next populated hand).  It
	// deliberately does not enter the fire animation or cast on this press.
	EnsureAngrealHands();
	if (CurrentHandSet == AngrealHandSet && CurrentHandSet)
	{
		UWoTHandInfo* Hand = CurrentHandSet->GetSelectedHand();
		if (Hand && Hand->GetSelectedClassName().IsNone()
			&& (!InventoryComponent || !IsValid(InventoryComponent->SelectedItem)))
		{
			if (Hand->IsEmpty()) CurrentHandSet->Select(CurrentHandSet->GetNext());
			else Hand->SelectFirst();
			Hand = CurrentHandSet->GetSelectedHand();
			const FName SelectedName = Hand ? Hand->GetSelectedClassName() : NAME_None;
			if (!SelectedName.IsNone())
			{
				AWoTInventory* Selected = InventoryComponent
					? InventoryComponent->FindInventoryName(SelectedName) : nullptr;
				if (!HasAuthority() && InventoryComponent) InventoryComponent->SelectItem(Selected);
				ServerSelectAngreal(SelectedName);
				SendSelectItemHandMessage(SelectedName);
				if (APlayerController* PC = Cast<APlayerController>(GetController()))
					if (AWoTHUD* HUD = Cast<AWoTHUD>(PC->GetHUD())) HUD->NotifyHandSelection();
				if (IsLocallyControlled())
					if (USoundBase* Sound = WoTSound::LoadBySubPath(TEXT("WOT/SelectNewItem")))
						UGameplayStatics::PlaySound2D(this, Sound);
			}
			return;
		}
	}
	// A selected Seal is placed rather than cast. Do not start the autonomous
	// owner's firing prediction; the authoritative path drops it immediately.
	if (InventoryComponent && Cast<AWoTSeal>(InventoryComponent->SelectedItem))
	{
		ServerUseAngreal(GetControlRotation());
		return;
	}
	// COND_SkipOwner also skips the server's animation intent, so predict the
	// presentation latch and stationary Attack pose on the autonomous owner.
	if (!HasAuthority())
	{
		const FName PreviousAnimState = DrivenLocomotionAnim;
		bPlayerAnimFireHeld = true;
		if (PreviousAnimState.IsNone()
			|| PreviousAnimState == FName(TEXT("Wait")))
		{
			PlayPlayerTweenPose(FName(TEXT("Attack")));
			DrivenLocomotionAnim = FName(TEXT("Wait"));
		}
	}
	// Carry the caster's client-side aim (yaw+pitch) to the server: the launcher fires
	// along SourcePawn->GetViewRotation() on the authority, but a Character replicates
	// control yaw (bUseControllerRotationYaw) and not pitch — so without this the server
	// fire is flat and an up/down aim misses over the wire.
	ServerUseAngreal(GetControlRotation());
}

void AWoTCharacter::CeaseUsingAngreal()
{
	// Swallow ONLY the release of a press the editor consumed (bCitadelEditorFireHeld). A
	// release whose press was a normal angreal use — even if editing began while it was held —
	// must still cease the angreal: WOTPlayer's CitadelEditor state ignores Fire/UseAngreal
	// but NOT CeaseUsingAngreal (WOTPlayer.uc:5100).
	if (bCitadelEditorFireHeld)
	{
		bCitadelEditorFireHeld = false;
		return;
	}
	if (!HasAuthority())
	{
		bPlayerAnimFireHeld = false;
		DrivenLocomotionAnim = NAME_None;
	}
	ServerCeaseUsingAngreal();
}

void AWoTCharacter::NextInventory()
{
	ServerNextInventory();
}

void AWoTCharacter::PrevInventory()
{
	ServerPrevInventory();
}

void AWoTCharacter::Drop()
{
#if WITH_AUTOMATION_TESTS
	++DropRequestCountForTest;
#endif
	ServerDrop();
}

void AWoTCharacter::DropTainted()
{
#if WITH_AUTOMATION_TESTS
	++DropTaintedRequestCountForTest;
#endif
	ServerDropTainted();
}

void AWoTCharacter::DropSeals()
{
	ServerDropSeals();
}

void AWoTCharacter::SelectHand(int32 Index)
{
	EnsureAngrealHands();
	if (!CurrentHandSet || Index < 0 || Index >= UWoTHandSet::NumHands)
	{
		return;
	}
	// WOTPlayer.SelectHand (WOTPlayer.uc:998-1006): bNewHand is computed BEFORE the
	// select — it picks the SelectHand vs SelectArtifact feedback sound.
	const bool bNewHand = Index != CurrentHandSet->Selected;
	CurrentHandSet->Select(Index);
	DriveHandSelection(bNewHand);
}

void AWoTCharacter::NextHand()
{
	EnsureAngrealHands();
	if (CurrentHandSet)
	{
		SelectHand(CurrentHandSet->GetNext());
	}
}

void AWoTCharacter::PrevHand()
{
	EnsureAngrealHands();
	if (CurrentHandSet)
	{
		SelectHand(CurrentHandSet->GetPrevious());
	}
}

void AWoTCharacter::NextItem()
{
	if (const AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetController());
		IsValid(WoTController)
		&& WoTController->GetCitadelEditCameraMode()
			== EWoTCitadelEditCameraMode::Orbit)
	{
		// Mouse wheel is the orbit distance axis in this mode; do not also move the palette.
		return;
	}
	EnsureAngrealHands();
	if (!CurrentHandSet)
	{
		return;
	}
	UWoTHandInfo* CurrentHand = CurrentHandSet->GetSelectedHand();
	const int32 PreviousSlot = CurrentHand->Selected;
	CurrentHand->SelectNext();
	if (CurrentHand->Selected <= PreviousSlot)
	{
		NextHand();
	}
	else
	{
		DriveHandSelection(/*bNewHand*/ false);
	}
}

void AWoTCharacter::PrevItem()
{
	if (const AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetController());
		IsValid(WoTController)
		&& WoTController->GetCitadelEditCameraMode()
			== EWoTCitadelEditCameraMode::Orbit)
	{
		return;
	}
	EnsureAngrealHands();
	if (!CurrentHandSet)
	{
		return;
	}
	UWoTHandInfo* CurrentHand = CurrentHandSet->GetSelectedHand();
	const int32 PreviousSlot = CurrentHand->Selected;
	CurrentHand->SelectPrevious();
	if (CurrentHand->Selected >= PreviousSlot)
	{
		PrevHand();
	}
	else
	{
		DriveHandSelection(/*bNewHand*/ false);
	}
}

namespace
{
	// WOTPlayer.uc:7163-7203 ArtifactNames — the shipped cheat grant list,
	// verbatim order including the authored gaps (10/29 unassigned) and
	// entries whose classes are not yet ported (resolution skips them).
	const TCHAR* GWoTArtifactNameSuffixes[] = {
		TEXT("LightGlobe"), TEXT("Dart"), TEXT("Fireball"), TEXT("Seeker"),
		TEXT("Target"), TEXT("Heal"), TEXT("Fork"), TEXT("Reflect"),
		TEXT("Absorb"), TEXT("Decay"), TEXT("Minion"), TEXT("Guardian"),
		TEXT("Champion"), TEXT("EarthTremor"), TEXT("AirShield"),
		TEXT("EarthShield"), TEXT("FireShield"), TEXT("SpiritShield"),
		TEXT("WaterShield"), TEXT("Shield"), TEXT("Shift"), TEXT("Taint"),
		TEXT("SoulBarb"), TEXT("AMA"), TEXT("RemoveCurse"), TEXT("Lightning"),
		TEXT("Balefire"), TEXT("Whirlwind"), TEXT("ExpWard"), TEXT("Tracer"),
		TEXT("WallOfAir"), TEXT("DistantEye"), TEXT("Levitate"),
		TEXT("SwapPlaces"), TEXT("TrapDetect"), TEXT("Illusion"),
		TEXT("Disguise"), TEXT("Special"), TEXT("Ice"), TEXT("AirBurst")};

	UClass* ResolveAngrealClass(const FString& Suffix)
	{
		const FString Path =
			FString::Printf(TEXT("/Script/WoT.WoTAngrealInv%s"), *Suffix);
		return FindObject<UClass>(nullptr, *Path);
	}
}

bool AWoTCharacter::OKToCheat() const
{
	// WOTPlayer.uc:5413-5422: any time in SP, bAdmin-only in MP.
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	if (World->GetNetMode() == NM_Standalone)
	{
		return true;
	}
	const AController* OwningController = GetController();
	const AWoTPlayerState* State = OwningController
		? OwningController->GetPlayerState<AWoTPlayerState>() : nullptr;
	return State && State->bAdmin;
}

void AWoTCharacter::AllAngreal(bool bInfiniteCharges)
{
	if (!HasAuthority())
	{
		ServerAllAngreal(bInfiniteCharges);
		return;
	}
	if (OKToCheat())
	{
		GrantAllAngreal(bInfiniteCharges);
	}
}

void AWoTCharacter::ServerAllAngreal_Implementation(bool bInfiniteCharges)
{
	// Never trust the replicated client-side admin bit. The owning controller's
	// authoritative PlayerState is the multiplayer permission check.
	if (OKToCheat())
	{
		GrantAllAngreal(bInfiniteCharges);
	}
}

void AWoTCharacter::GrantAllAngreal(bool bInfiniteCharges)
{
	if (!IsValid(InventoryComponent)) return;
	for (const TCHAR* Suffix : GWoTArtifactNameSuffixes)
	{
		GiveAngreal(Suffix);
	}
	if (bInfiniteCharges)
	{
		// WOTPlayer.uc:5975-5980: sweep EXISTING items too.
		for (AWoTInventory* Item : InventoryComponent->Items)
		{
			if (AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Item))
			{
				Angreal->CurCharges = 999;
				Angreal->ChargeCost = 0;
			}
		}
	}
}

void AWoTCharacter::AllAmmo()
{
	// WOTPlayer.uc:5985-5993.
	AllAngreal(true);
}

void AWoTCharacter::GiveAngreal(const FString& Name)
{
	if (!HasAuthority())
	{
		ServerGiveAngreal(Name);
		return;
	}
	if (!OKToCheat() || !IsValid(InventoryComponent))
	{
		return;
	}
	FString Suffix = Name;
	Suffix.RemoveFromStart(TEXT("WoT"));
	Suffix.RemoveFromStart(TEXT("AngrealInv"));
	UClass* AngrealClass = ResolveAngrealClass(Suffix);
	UWorld* World = GetWorld();
	if (!AngrealClass || !World)
	{
		return;
	}
	// Deferred spawn with bHeldItem set BEFORE FinishSpawning, exactly like
	// SpawnCopy: a live-state spawn at the owner's feet self-touches, and the
	// duplicate charge-steal then consumes the grant (the first cut of this
	// cheat flashed the pickup icon but never landed in the hands).
	const FTransform Xform(GetActorRotation(), GetActorLocation());
	AWoTAngrealInventory* Angreal =
		World->SpawnActorDeferred<AWoTAngrealInventory>(AngrealClass, Xform,
			this, this, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Angreal)
	{
		return;
	}
	Angreal->bHeldItem = true;
	Angreal->FinishSpawning(Xform);
	const AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (Game && !Game->PickupQuery(this, Angreal))
	{
		Angreal->Destroy();
		return;
	}
	// WOTPlayer.uc:5966-5967: silent grant — no pickup sound or message.
	Angreal->PickupSound.Reset();
	Angreal->PickupMessage.Empty();
	Angreal->GiveTo(this);
}

void AWoTCharacter::ServerGiveAngreal_Implementation(const FString& Name)
{
	GiveAngreal(Name);
}

void AWoTCharacter::SpawnAngreal(const FString& Name)
{
	// Retail 'Summon'-style placement cheat: drop the GENERATED PICKUP BP (the
	// leaf that binds the mesh/skin) ~150uu in front, in the normal pickup
	// state — for eyeballing pickup presentation (e.g. the Dart masking
	// finding) without hunting a map placement.
	if (!HasAuthority())
	{
		ServerSpawnAngreal(Name);
		return;
	}
	if (!OKToCheat())
	{
		return;
	}
	FString Suffix = Name;
	Suffix.RemoveFromStart(TEXT("WoT"));
	Suffix.RemoveFromStart(TEXT("AngrealInv"));
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const FString BPPath = FString::Printf(
		TEXT("/Game/WoT/Blueprints/Pickups/BP_WoTAngrealInv%s.BP_WoTAngrealInv%s_C"),
		*Suffix, *Suffix);
	UClass* PickupClass = LoadClass<AWoTInventory>(nullptr, *BPPath);
	if (!PickupClass)
	{
		PickupClass = ResolveAngrealClass(Suffix);   // logic-only fallback
	}
	if (!PickupClass)
	{
		return;
	}
	const FVector Forward = GetActorForwardVector();
	const FVector At = GetActorLocation() + Forward * 150.0f;
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	World->SpawnActor<AWoTInventory>(PickupClass, At,
		FRotator(0.0f, GetActorRotation().Yaw, 0.0f), Params);
}

void AWoTCharacter::ServerSpawnAngreal_Implementation(const FString& Name)
{
	SpawnAngreal(Name);
}

void AWoTCharacter::DriveHandSelection(bool bNewHand)
{
	if (CurrentHandSet)
	{
		if (CurrentHandSet == AngrealHandSet)
		{
			ServerSelectAngreal(
				AngrealHandSet->GetSelectedHand()->GetSelectedClassName());
		}
		// WOTPlayer.PerformSelection (WOTPlayer.uc:1013-1023): with a live selection, a
		// hand change plays WOT.UI.SelectHand, an item change WOT.UI.SelectArtifact —
		// owner-local ClientPlaySound, checked AFTER the selection RPC like the original.
		if (IsLocallyControlled() && InventoryComponent
			&& InventoryComponent->SelectedItem)
		{
			if (USoundBase* SelectSound = WoTSound::LoadBySubPath(FName(bNewHand
				? TEXT("WOT/SelectHand") : TEXT("WOT/SelectArtifact"))))
			{
				UGameplayStatics::PlaySound2D(this, SelectSound);
			}
		}
		if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
		{
			if (CurrentHandSet == CitadelEditorHandSet
				&& IsCitadelEditorInputActive())
			{
				if (AWoTPlayerController* WoTController =
					Cast<AWoTPlayerController>(PlayerController);
					IsValid(WoTController)
					&& WoTController->IsOwnerEditingCitadel())
				{
					UWoTHandInfo* SelectedHand = CurrentHandSet->GetSelectedHand();
					WoTController->SetCitadelPaletteSelection(SelectedHand
						? SelectedHand->GetSelectedClassName() : NAME_None);
				}
			}
			if (AWoTHUD* HUD = Cast<AWoTHUD>(PlayerController->GetHUD()))
			{
				HUD->NotifyHandSelection();
			}
		}
		// WOTPlayer.PerformSelection ends with ClientSelectItemMessage(new selection).
		if (IsLocallyControlled())
		{
			const UWoTHandInfo* SelectedHand = CurrentHandSet->GetSelectedHand();
			SendSelectItemHandMessage(SelectedHand
				? SelectedHand->GetSelectedClassName() : NAME_None);
		}
	}
}

void AWoTCharacter::SendSelectItemHandMessage(FName ItemName)
{
	// WOTPlayer.ClientSelectItemMessage (WOTPlayer.uc:1176-1210): an angreal selection
	// announces "Selected the <Title> ter'angreal " on the hand channel (SelectedStr /
	// TerAngrealStr, WoT.int:45-46); a plain WOTInventory drops the ter'angreal suffix;
	// an unresolvable class falls back to the raw item name. Empty selections are silent.
	if (ItemName.IsNone())
	{
		return;
	}
	const APlayerController* PlayerController = Cast<APlayerController>(GetController());
	AWoTHUD* HUD = PlayerController
		? Cast<AWoTHUD>(PlayerController->GetHUD()) : nullptr;
	if (!HUD)
	{
		return;
	}
	const AWoTInventory* Default = AWoTInventory::FindDefaultByOriginalName(ItemName);
	FString Message = SelectedStr;
	if (const AWoTAngrealInventory* AngrealDefault =
		Cast<AWoTAngrealInventory>(Default))
	{
		Message += AngrealDefault->Title + TerAngrealStr;
	}
	else if (Default)
	{
		Message += Default->Title;
	}
	else
	{
		Message += ItemName.ToString();
	}
	HUD->AddHandMessage(Message);
}

bool AWoTCharacter::ServerUseAngreal_Validate(FRotator AimRotation)
{
	// Reject a malformed aim from a compromised client (NaN/Inf would poison the fire
	// rotation and any downstream trace). A well-behaved client always sends a finite
	// control rotation.
	return !AimRotation.ContainsNaN();
}

bool AWoTCharacter::ServerCeaseUsingAngreal_Validate() { return true; }
bool AWoTCharacter::ServerNextInventory_Validate() { return true; }
bool AWoTCharacter::ServerPrevInventory_Validate() { return true; }
bool AWoTCharacter::ServerSelectAngreal_Validate(FName) { return true; }
bool AWoTCharacter::ServerDrop_Validate() { return true; }
bool AWoTCharacter::ServerDropTainted_Validate() { return true; }
bool AWoTCharacter::ServerDropSeals_Validate() { return true; }

void AWoTCharacter::ServerUseAngreal_Implementation(FRotator AimRotation)
{
	// Never trust only the autonomous proxy's predicted gate: reject a forged or
	// late cast RPC throughout the authoritative PlayerLanded interval too.
	if (IsHardLanding())
	{
		return;
	}
	// Authority guard: a decided match casts nothing, even if a stale/modified client still
	// sends the cast RPC (the well-behaved client re-routes it to a restart in UseAngreal).
	if (const AWoTPlayerController* WPC = Cast<AWoTPlayerController>(GetController()))
	{
		if (WPC->IsMatchEnded())
		{
			return;
		}
	}
	const bool bSelectedSeal = InventoryComponent
		&& Cast<AWoTSeal>(InventoryComponent->SelectedItem) != nullptr;
	// Stamp the controller with the caster's replicated-in aim before the launcher reads
	// GetViewRotation(). UE1's ServerMove carried the full view, so the authority already
	// fired along the client's true aim; UE5 gives us only yaw for free, so the cast RPC
	// delivers the rest. Yaw is a no-op re-set (already replicated); pitch is the fix.
	if (AController* C = GetController())
	{
		C->SetControlRotation(AimRotation);
	}
	// Pawn.bFire is the animation selector in WOTPlayer.PlayFiring/
	// PlayRunning/PlayWalking (WOTPlayer.uc:2137-2159,2440-2567). Keep the
	// latch authority-owned with RepAnim; changing the selected slot creates the
	// movement edge without discarding the previous retail animation group.
	if (!bSelectedSeal)
	{
		const FName PreviousAnimState = DrivenLocomotionAnim;
		bPlayerAnimFireHeld = true;
		if (PreviousAnimState.IsNone()
			|| PreviousAnimState == FName(TEXT("Wait")))
		{
			// PlayFiring's non-movement branch is a held Attack tween pose.
			PlayPlayerTweenPose(FName(TEXT("Attack")));
			DrivenLocomotionAnim = FName(TEXT("Wait"));
		}
	}
	InventoryComponent->UseAngreal();
}

void AWoTCharacter::ServerCeaseUsingAngreal_Implementation()
{
	bPlayerAnimFireHeld = false;
	DrivenLocomotionAnim = NAME_None;
	InventoryComponent->CeaseUsingAngreal();
}

void AWoTCharacter::ServerNextInventory_Implementation()
{
	InventoryComponent->NextInventory();
}

void AWoTCharacter::ServerPrevInventory_Implementation()
{
	InventoryComponent->PrevInventory();
}

void AWoTCharacter::ServerSelectAngreal_Implementation(FName ClassName)
{
	InventoryComponent->SelectItem(InventoryComponent->FindInventoryName(ClassName));
}

FVector AWoTCharacter::GetPitDropLocation() const
{
	AWoTPitOpening* NearestOpening = nullptr;
	float NearestDistance = 1600.0f;
	for (TActorIterator<AWoTPitOpening> It(GetWorld()); It; ++It)
	{
		if (!IsValid(*It))
		{
			continue;
		}
		FVector Delta = It->GetActorLocation() - GetActorLocation();
		Delta.Z = 0.0f;
		const float Distance = Delta.Size();
		if (Distance < NearestDistance)
		{
			NearestDistance = Distance;
			NearestOpening = *It;
		}
	}
	if (NearestOpening && IsValid(NearestOpening->ThePitTrap))
	{
		if (const AWoTPit* Pit = Cast<AWoTPit>(
			NearestOpening->ThePitTrap->GetOwner()))
		{
			return Pit->GetActorLocation();
		}
	}
	return GetActorLocation();
}

void AWoTCharacter::DropSelectedInventory(bool bIgnoreInteractionGuards)
{
	if (!HasAuthority() || !InventoryComponent)
	{
		return;
	}
	const AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetController());
	if (!bIgnoreInteractionGuards
		&& ((CurrentHandSet && CurrentHandSet != AngrealHandSet)
			|| (WoTController && WoTController->IsOwnerEditingCitadel())))
	{
		return;
	}

	AWoTInventory* Item = InventoryComponent->SelectedItem;
	if (!IsValid(Item) || Item->GetClass() == DefaultAngrealInventory
		|| Item->IsA<AWoTAngrealInvSpecial>())
	{
		return;
	}

	if (AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Item))
	{
		AWoTBagHolding* Bag = nullptr;
		for (TActorIterator<AWoTBagHolding> It(GetWorld()); It; ++It)
		{
			if (IsValid(*It) && !It->IsDestroyingBag()
				&& FVector::Dist(GetActorLocation(), It->GetActorLocation()) <= 160.0f)
			{
				Bag = *It;
				break;
			}
		}
		if (!Bag)
		{
			const FTransform BagTransform(
				FRotator::ZeroRotator, GetActorLocation());
			Bag = GetWorld()->SpawnActorDeferred<AWoTBagHolding>(
				AWoTBagHolding::StaticClass(), BagTransform, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (Bag)
			{
				Bag->bHeldItem = true;
				Bag->FinishSpawning(BagTransform);
			}
		}
		InventoryComponent->DeleteInventory(Item);
		Angreal->CurCharges = FMath::Min(
			Angreal->CurCharges, Angreal->MaxInitialCharges);
		if (!Bag || !Bag->AddItem(Item))
		{
			Item->Destroy();
			return;
		}
		Item = Bag;
	}

	FVector DropLocation;
	if (Item->IsA<AWoTSeal>()
		&& AWoTZoneVolume::PitZoneAt(GetWorld(), GetActorLocation()))
	{
		DropLocation = GetPitDropLocation() + FVector(0.0f, 0.0f, 16.0f);
	}
	else
	{
		const float PawnRadius = GetCapsuleComponent()->GetScaledCapsuleRadius();
		const float Distance = Item->IsA<AWoTSeal>()
			? PawnRadius
			: Item->GetPickupCollisionRadius() + PawnRadius + 1.0f;
		DropLocation = GetActorLocation() + GetActorForwardVector() * Distance;
	}
	Item->DropFrom(DropLocation);
}

void AWoTCharacter::SetScaleGlowPresentation(const float NewScaleGlow)
{
	ScaleGlow = NewScaleGlow;
	OnRep_ScaleGlow();
	if (HasAuthority()) ForceNetUpdate();
}

void AWoTCharacter::OnRep_ScaleGlow()
{
	// MeshLight owns the authenticated body material instances. Re-baking here
	// immediately pushes MLScaleGlow into those live instances; a stationary
	// teleporting pawn must not wait for the movement/zone polling path.
	if (GetNetMode() != NM_DedicatedServer)
	{
		// A stationary proxy can receive this edge before MeshLight has a map
		// light table (Entry/headless and late relevance are both real cases).
		// Push the actor parameter directly into the live body MIDs as retail's
		// replicated ScaleGlow did; a later light bake preserves the same value.
		if (USkeletalMeshComponent* Body = GetMesh())
		{
			for (int32 Slot = 0; Slot < Body->GetNumMaterials(); ++Slot)
			{
				UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(
					Body->GetMaterial(Slot));
				if (!MID) MID = Body->CreateDynamicMaterialInstance(Slot);
				if (MID) MID->SetScalarParameterValue(TEXT("MLScaleGlow"), ScaleGlow);
			}
		}
		if (UWoTMeshLight* Light = FindComponentByClass<UWoTMeshLight>())
		{
			Light->ForceRuntimeBake();
		}
	}
}

// A dead player's inventory already left with Died (retail had nothing left to
// drop); the port also refuses the RPCs outright, since a dead pawn can stay
// possessed (match end) and must not drop anything granted to it afterwards.
void AWoTCharacter::ServerDrop_Implementation()
{
	if (bDead)
	{
		return;
	}
	DropSelectedInventory();
}

void AWoTCharacter::ServerDropTainted_Implementation()
{
	if (!HasAuthority() || !InventoryComponent || bDead)
	{
		return;
	}
	const TArray<TObjectPtr<AWoTInventory>> InventoryItems = InventoryComponent->Items;
	for (AWoTInventory* Item : InventoryItems)
	{
		if (AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Item);
			Angreal && Angreal->bTainted && InventoryComponent->Items.Contains(Item))
		{
			InventoryComponent->SelectItem(Item);
			DropSelectedInventory();
		}
	}
}

void AWoTCharacter::ServerDropSeals_Implementation()
{
	if (bDead)
	{
		return;
	}
	DropAllSeals();
}

void AWoTCharacter::DropAllSeals(bool bIgnoreInteractionGuards)
{
	if (!HasAuthority() || !InventoryComponent)
	{
		return;
	}
	const TArray<TObjectPtr<AWoTInventory>> InventoryItems = InventoryComponent->Items;
	for (AWoTInventory* Item : InventoryItems)
	{
		if (IsValid(Item) && Item->IsA<AWoTSeal>()
			&& InventoryComponent->Items.Contains(Item))
		{
			InventoryComponent->SelectItem(Item);
			DropSelectedInventory(bIgnoreInteractionGuards);
		}
	}
}

bool AWoTCharacter::IsCitadelOrbitInput() const
{
	const AWoTPlayerController* WoTController = Cast<AWoTPlayerController>(GetController());
	return IsValid(WoTController)
		&& WoTController->GetCitadelEditCameraMode() == EWoTCitadelEditCameraMode::Orbit;
}

void AWoTCharacter::MoveForward(float Value)
{
	// MoveForward runs every frame (an axis binding), so it also carries the
	// held-Jump swim-up input. Orbit mode consumes movement before that.
	if (!IsCitadelOrbitInput())
	{
		AddSwimVerticalInput();
	}
	ApplyForwardAxis(Value);
}

void AWoTCharacter::ApplyForwardAxis(float Value)
{
	if (IsCitadelOrbitInput())
	{
		if (AWoTPlayerController* WoTController = Cast<AWoTPlayerController>(GetController()))
		{
			WoTController->AdjustCitadelOrbitDistance(
				Value, GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.0f);
		}
		return;
	}
	if (bFreeLookHeld) { AddPitchInput(Value * 0.5f * RetailFOVScale(), true); }
	else { AddForwardInput(Value); }
}

void AWoTCharacter::CitadelOrbitZoom(float Value)
{
	AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetController());
	if (IsValid(WoTController))
	{
		WoTController->AdjustCitadelOrbitDistance(
			Value, GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.0f);
	}
}

void AWoTCharacter::MoveRight(float Value)
{
	ApplyStrafeAxis(Value);
}

void AWoTCharacter::ApplyStrafeAxis(float Value)
{
	if (IsCitadelOrbitInput())
	{
		// CitadelOrbitMode consumes movement input: a strafe must not drag the ghosted
		// body (the orbit camera is zone-driven, not pawn-driven — WOTPlayer.uc:5262).
		return;
	}
	AddStrafeInput(Value);
}

void AWoTCharacter::ApplyStickLook(float YawDegrees, float PitchDegrees)
{
	// The same calls the mouse makes, so look-spring, center-view and the
	// Impaled clamp apply unchanged. The controller multiplies them by its
	// legacy input scales; divide those out so the stick turns exactly the
	// degrees asked for.
	const AWoTPlayerController* WoTController = Cast<AWoTPlayerController>(GetController());
	const FVector2D Scale = WoTController
		? WoTController->GetLegacyLookInputScale() : FVector2D(1.0, 1.0);
	if (!FMath::IsNearlyZero(YawDegrees) && !FMath::IsNearlyZero(Scale.X))
	{
		AddControllerYawInput(YawDegrees / static_cast<float>(Scale.X));
	}
	if (!FMath::IsNearlyZero(Scale.Y))
	{
		AddPitchInput(PitchDegrees / static_cast<float>(Scale.Y), /*bFromLookDevice*/ true);
	}
}

float AWoTCharacter::DecayCenterViewPitch(float PitchDegrees, float DeltaSeconds)
{
	// PlayerPawn.UpdateRotation.
	constexpr float SnapDegrees = 1000.0f * 360.0f / 65536.0f;
	const float Decayed = PitchDegrees
		* (1.0f - 12.0f * FMath::Min(0.0833f, FMath::Max(0.0f, DeltaSeconds)));
	return FMath::Abs(Decayed) < SnapDegrees ? 0.0f : Decayed;
}

float AWoTCharacter::CalculateStairLookPitch(float DeltaSeconds) const
{
	if (!Controller || !GetWorld()) return 0.0f;
	const float CurrentPitch = FMath::UnwindDegrees(
		Controller->GetControlRotation().Pitch);
	if (DeltaSeconds > 0.33f) return CurrentPitch;
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	if (!Capsule) return CurrentPitch;

	const float Height = Capsule->GetScaledCapsuleHalfHeight() + BaseEyeHeight;
	if (Height <= UE_SMALL_NUMBER) return CurrentPitch;
	FRotator LookRotation = Controller->GetControlRotation();
	LookRotation.Pitch = 0.0f;
	const FVector Direction = LookRotation.Vector().GetSafeNormal2D();
	const FVector Eye = GetActorLocation() + FVector(0.0f, 0.0f, BaseEyeHeight);
	const FCollisionShape Slice = FCollisionShape::MakeBox(FVector(
		Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleRadius(), 1.0f));
	auto SweepFraction = [this, &Slice](const FVector& Start, const FVector& End)
	{
		FHitResult Hit;
		return WoTVisBlockingSweep(GetWorld(), Start, End, FQuat::Identity,
			Slice, this, Hit) ? Hit.Time : 1.0f;
	};

	const float Distance = 2.0f * Height * SweepFraction(
		Eye, Eye + 2.0f * Height * Direction);
	float TargetPitch = 0.0f;
	if (Distance > 0.8f * Height)
	{
		const float Down = 3.0f * Height;
		FVector Spot = Eye + 0.5f * Distance * Direction;
		const float FirstDown = Down * SweepFraction(
			Spot, Spot - FVector(0.0f, 0.0f, Down));
		if (FirstDown < 0.7f * Height - 6.0f)
		{
			Spot = Eye + Distance * Direction;
			const float SecondDown = Down * SweepFraction(
				Spot, Spot - FVector(0.0f, 0.0f, Down));
			TargetPitch = FMath::Max(0.0f, CurrentPitch);
			if (SecondDown < FirstDown - 10.0f)
			{
				TargetPitch = 5400.0f * 360.0f / 65536.0f;
			}
		}
		else if (FirstDown > 0.7f * Height + 6.0f)
		{
			FHitResult BodyHit;
			const bool bBodyBlocked = WoTVisBlockingTrace(GetWorld(),
				GetActorLocation(), GetActorLocation() + 0.9f * Distance * Direction,
				this, BodyHit);
			if (!bBodyBlocked)
			{
				Spot = Eye + Distance * Direction;
				const float SecondDown = Down * SweepFraction(
					Spot, Spot - FVector(0.0f, 0.0f, Down));
				TargetPitch = FMath::Min(0.0f, CurrentPitch);
				if (SecondDown > FirstDown + 10.0f)
				{
					TargetPitch = -5000.0f * 360.0f / 65536.0f;
				}
			}
		}
	}

	const float DifferenceUnits = FMath::Abs(CurrentPitch - TargetPitch)
		* 65536.0f / 360.0f;
	if (DifferenceUnits <= UE_SMALL_NUMBER) return TargetPitch;
	const float RotationRate = DifferenceUnits < 1000.0f
		? 8000.0f / DifferenceUnits : 8.0f;
	return FMath::Lerp(CurrentPitch, TargetPitch,
		FMath::Min(1.0f, RotationRate * DeltaSeconds));
}

bool AWoTCharacter::IsSwimmingForInput() const
{
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	return Move != nullptr && Move->IsSwimming() && Move->GetMaxAcceleration() > 0.0f;
}

void AWoTCharacter::AddForwardInput(float Value)
{
	if (!IsPlayerMovementConstrained() && !FMath::IsNearlyZero(Value))
	{
		if (IsSwimmingForInput())
		{
			// Swimming follows the full view: look down to dive, up to rise.
			AddMovementInput(GetControlRotation().Vector(),
				Value * RetailSwimForwardAccel
					/ GetCharacterMovement()->GetMaxAcceleration());
			return;
		}
		AddMovementInput(FRotator(0, GetControlRotation().Yaw, 0).Vector(), Value);
	}
}

void AWoTCharacter::AddStrafeInput(float Value)
{
	if (!IsPlayerMovementConstrained() && !FMath::IsNearlyZero(Value))
	{
		// The ViewRotation Y axis is horizontal (no roll), as in walking.
		const FVector Right = FRotationMatrix(FRotator(0, GetControlRotation().Yaw, 0))
			.GetScaledAxis(EAxis::Y);
		const float Scale = IsSwimmingForInput()
			? RetailSwimSideAccel / GetCharacterMovement()->GetMaxAcceleration()
			: 1.0f;
		AddMovementInput(Right, Value * Scale);
	}
}

void AWoTCharacter::AddSwimVerticalInput()
{
	// aUp: Jump held while swimming pushes straight up (UE5's own Jump does
	// nothing in water). Retail binds no key to Duck (aUp < 0) by default.
	if (bSwimUpHeld && !IsPlayerMovementConstrained() && IsSwimmingForInput())
	{
#if WITH_AUTOMATION_TESTS
		++SwimVerticalInputCountForTest;
#endif
		AddMovementInput(FVector::UpVector,
			RetailSwimSideAccel / GetCharacterMovement()->GetMaxAcceleration());
	}
}

#if WITH_AUTOMATION_TESTS
void AWoTCharacter::AddForwardInputForTest(float Value)
{
	AddForwardInput(Value);
}
#endif

void AWoTCharacter::AddPitchInput(float Value, bool bFromLookDevice)
{
	if (FMath::IsNearlyZero(Value)) { return; }
	if (bFromLookDevice)
	{
		bKeyboardLook = true;
		bExplicitLookInputThisFrame = true;
		if (!bCenterViewHeld) { bCenterView = false; }
	}
	AddControllerPitchInput(Value);
}

void AWoTCharacter::ApplyImpaledRotationAfterViewUpdate(float AppliedPitchInput)
{
	// Impaled.PlayerMove (WOTPlayer.uc:3370-3400): the view may cross the
	// horizon, but not linger in the authored 3000..8000-unit forbidden band.
	float ViewPitchDegrees = 0.0f;
	if (Controller)
	{
		FRotator View = Controller->GetControlRotation();
		float Units = FMath::Fmod(View.Pitch * 65536.0f / 360.0f, 65536.0f);
		if (Units < 0.0f)
		{
			Units += 65536.0f;
		}
		if (Units > 3000.0f && Units < 8000.0f)
		{
			Units = AppliedPitchInput > 0.0f ? 3000.0f : 8000.0f;
			View.Pitch = Units * 360.0f / 65536.0f;
			Controller->SetControlRotation(View);
		}
		ViewPitchDegrees = View.Pitch;
	}

	FRotator ActorRotation = GetActorRotation();
	const float MaxPitchDegrees = 15000.0f * 360.0f / 65536.0f;
	ActorRotation.Pitch = FMath::Clamp(
		FMath::UnwindDegrees(ViewPitchDegrees),
		-MaxPitchDegrees, MaxPitchDegrees);
	SetActorRotation(ActorRotation);
}

float AWoTCharacter::RetailFOVScale() const
{
	const APlayerController* Player = Cast<APlayerController>(Controller);
	return Player && Player->PlayerCameraManager
		? Player->PlayerCameraManager->GetFOVAngle() * 0.01111f : 1.0f;
}

void AWoTCharacter::TurnInput(float Value)
{
	if (bStrafeHeld) { AddStrafeInput(Value); }
	else if (!FMath::IsNearlyZero(Value))
	{
		AddControllerYawInput(Value * RetailFOVScale());
	}
}

void AWoTCharacter::KeyboardLookInput(float Value)
{
	AddPitchInput(Value * RetailFOVScale(), true);
}

void AWoTCharacter::MouseXInput(float Value)
{
	if (bStrafeHeld) { AddStrafeInput(Value); }
	else if (!FMath::IsNearlyZero(Value)) { AddControllerYawInput(Value); }
}

void AWoTCharacter::MouseYInput(float Value)
{
	bool bInvertMouseY = false;
	if (const APlayerController* OwningPlayerController =
		Cast<APlayerController>(GetController());
		OwningPlayerController && OwningPlayerController->PlayerInput)
	{
		FInputAxisProperties Properties;
		if (OwningPlayerController->PlayerInput->GetAxisProperties(
			EKeys::MouseY, Properties))
		{
			bInvertMouseY = Properties.bInvert;
		}
	}
	const float ForwardValue = bInvertMouseY ? -Value : Value;
	if (bStrafeHeld) { AddForwardInput(ForwardValue); }
	else if (bAlwaysMouseLook || IsLookActive()) { AddPitchInput(Value, true); }
	else { AddForwardInput(ForwardValue); }
}

void AWoTCharacter::WalkingPressed()
{
	++WalkingPressCount;
	if (UWoTCharacterMovement* Movement =
		Cast<UWoTCharacterMovement>(GetCharacterMovement()))
	{
		Movement->SetWalkingRequested(true);
	}
}

void AWoTCharacter::WalkingReleased()
{
	WalkingPressCount = FMath::Max(0, WalkingPressCount - 1);
	if (UWoTCharacterMovement* Movement =
		Cast<UWoTCharacterMovement>(GetCharacterMovement()))
	{
		Movement->SetWalkingRequested(WalkingPressCount > 0);
	}
}

void AWoTCharacter::ResetWalkingInputState()
{
	WalkingPressCount = 0;
	if (UWoTCharacterMovement* Movement =
		Cast<UWoTCharacterMovement>(GetCharacterMovement()))
	{
		Movement->SetWalkingRequested(false);
	}
}

void AWoTCharacter::LookPressed() { bLookHeld = true; }
void AWoTCharacter::LookReleased() { bLookHeld = false; }
void AWoTCharacter::LookTogglePressed() { bLookToggled = !bLookToggled; }
void AWoTCharacter::FreeLookPressed()
{
	bFreeLookHeld = true;
	bKeyboardLook = true;
}
void AWoTCharacter::FreeLookReleased() { bFreeLookHeld = false; }
void AWoTCharacter::CenterViewPressed()
{
	bCenterView = true;
	bCenterViewHeld = true;
	bKeyboardLook = false;
}
void AWoTCharacter::CenterViewReleased() { bCenterViewHeld = false; }
void AWoTCharacter::StrafePressed() { bStrafeHeld = true; }
void AWoTCharacter::StrafeReleased() { bStrafeHeld = false; }

void AWoTCharacter::SetCitadelOrbitGhost(bool bGhosted)
{
	// CitadelOrbitMode.BeginState (WOTPlayer.uc:5262-5272): the Ghost() entry code —
	// SetCollision(false,false,false), bHidden, PHYS_None, UnderWaterTime=-1. EndState
	// (WOTPlayer.uc:5277-5289) restores collision/visibility, PHYS_Falling, and the
	// default UnderWaterTime. Authority-only: bHidden replicates natively; the
	// collision/movement halves reach clients via the OnRep (collision enable does not
	// replicate — the AWoTTrap::bEditing pattern).
	if (!HasAuthority() || bCitadelOrbitGhosted == bGhosted)
	{
		return;
	}
	bCitadelOrbitGhosted = bGhosted;
	SetActorHiddenInGame(bGhosted);
	UnderWaterTime = bGhosted
		? -1.0f
		: GetClass()->GetDefaultObject<AWoTCharacter>()->UnderWaterTime;
	ApplyCitadelOrbitGhost();
}

void AWoTCharacter::OnRep_CitadelOrbitGhosted()
{
	ApplyCitadelOrbitGhost();
}

void AWoTCharacter::SetNeutralGhost(bool bGhosted)
{
	// giMPBattle mutates Actor.Style on the authority; the replicated bool is the
	// UE5 presentation channel replacing UE1's native replicated Style property.
	if (!HasAuthority() || bNeutralGhost == bGhosted)
	{
		return;
	}
	bNeutralGhost = bGhosted;
	ApplyNeutralGhostLook();
	ForceNetUpdate();
}

void AWoTCharacter::OnRep_NeutralGhost()
{
	ApplyNeutralGhostLook();
}

void AWoTCharacter::RestoreNeutralGhostMaterials()
{
	USkeletalMeshComponent* Body = GetMesh();
	if (IsValid(Body))
	{
		for (int32 Slot = 0; Slot < NeutralGhostOriginalMaterials.Num(); ++Slot)
		{
			Body->SetMaterial(Slot, NeutralGhostOriginalMaterials[Slot]);
		}
	}
	NeutralGhostMaterials.Reset();
	NeutralGhostOriginalMaterials.Reset();
}

void AWoTCharacter::ApplyNeutralGhostLook()
{
	USkeletalMeshComponent* Body = GetMesh();
	if (!IsValid(Body))
	{
		return;
	}
	const bool bEffectiveGhost = bDisguiseLookActive
		? Disguise.bSourceNeutralGhost != 0 : bNeutralGhost;
	if (!bEffectiveGhost)
	{
		RestoreNeutralGhostMaterials();
		// A neutral login ghosts BEFORE the post-spawn bake fires, so the saved
		// originals are the raw plain-master materials; restoring them after that
		// bake was consumed leaves the slots un-lit until a 128uu move. Re-bake.
		if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
		{
			MeshLight->ForceRuntimeBake();
		}
		return;
	}
	// Cosmetics do not need material instances on a headless dedicated server. The
	// replicated state remains set so clients still receive and apply it.
	if (GetNetMode() == NM_DedicatedServer || NeutralGhostOriginalMaterials.Num() > 0)
	{
		return;
	}

	UMaterialInterface* GhostMaster = WoTNeutralGhostMaster();
	if (!IsValid(GhostMaster))
	{
		return;
	}
	const int32 MaterialCount = Body->GetNumMaterials();
	NeutralGhostOriginalMaterials.Reserve(MaterialCount);
	NeutralGhostMaterials.Reserve(MaterialCount);
	for (int32 Slot = 0; Slot < MaterialCount; ++Slot)
	{
		UMaterialInterface* Original = Body->GetMaterial(Slot);
		NeutralGhostOriginalMaterials.Add(Original);
		UMaterialInstanceDynamic* Ghost =
			WoTMeshModulate::CreateInstance(this, Original);
		NeutralGhostMaterials.Add(Ghost);
		if (!IsValid(Ghost))
		{
			continue;
		}
		Body->SetMaterial(Slot, Ghost);
	}
}

void AWoTCharacter::ApplyCitadelOrbitGhost()
{
	SetActorEnableCollision(!bCitadelOrbitGhosted);
	// The movement halt runs where movement is simulated: the authority and the owning
	// client (its autonomous proxy predicts its own moves). Simulated proxies follow the
	// replicated movement stream and only need the collision half above.
	if (!HasAuthority() && !IsLocallyControlled())
	{
		return;
	}
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (!Move)
	{
		return;
	}
	if (bCitadelOrbitGhosted)
	{
		Move->StopMovementImmediately();
		Move->DisableMovement();   // SetPhysics(PHYS_None)
	}
	else
	{
		Move->SetMovementMode(MOVE_Falling);   // SetPhysics(PHYS_Falling)
	}
}
