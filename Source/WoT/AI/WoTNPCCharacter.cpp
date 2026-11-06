#include "WoTNPCCharacter.h"
#include "../WoTRandom.h"

#include "WoTBrainComponent.h"
#include "WoTCasterComponent.h"
#include "WoTNPCController.h"
#include "WoTCreatureBehaviors.h"
#include "WoTCreatureData.h"
#include "WoTNPCWeapon.h"
#include "WoTNPCWeaponData.h"
#include "../WoTMeshLight.h"
#include "../WoTMeshModulate.h"
#include "../WoTPlayerState.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTWeaveFX.h"
#include "../WoTParticleSprayer.h"
#include "../WoTActorRotator.h"
#include "../WoTEvents.h"
#include "../WoTSoundTable.h"
#include "../Assets/WoTRetailIdentity.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTAngrealInvDisguise.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "../Projectiles/WoTIceProjectile.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	constexpr int32 WoTNPCWarderFadeSkinCount = 12;

	// Warder is the only retail consumer of this authored FadeSkins sequence, so keep
	// its soft paths local to the NPC presentation implementation rather than global data.
	const FSoftObjectPath WoTNPCWarderFadeSkinPaths[WoTNPCWarderFadeSkinCount] = {
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/WCamo_11.WCamo_11")),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/WCamo_10.WCamo_10")),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/WCamo_09.WCamo_09")),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/WCamo_08.WCamo_08")),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/WCamo_07.WCamo_07")),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/WCamo_06.WCamo_06")),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/WCamo_05.WCamo_05")),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/WCamo_04.WCamo_04")),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/WCamo_03.WCamo_03")),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/WCamo_02.WCamo_02")),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/WCamo_01.WCamo_01")),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/JWarder1.JWarder1"))
	};
}

AWoTNPCCharacter::AWoTNPCCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Brain = CreateDefaultSubobject<UWoTBrainComponent>(TEXT("Brain"));

	// AWoTCharacter hides the body from its owner so a first-person player never sees
	// their own pawn. UE applies that to indirect owners too, and a citadel troop is
	// owned by the player who placed it (WOTPlayer.uc:4917 Spawn(SpawnClass, Self)), so
	// the placer saw only the troop's weapon. Retail skips only the view target; an
	// NPC is never one.
	if (USkeletalMeshComponent* Body = GetMesh())
	{
		Body->SetOwnerNoSee(false);
	}

	AIControllerClass = AWoTNPCController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	// NPC cylinder: radius 16 (LegendPawn.uc:2647), half-height 48
	// (WOTPawn.uc:1580 — UE1 CollisionHeight is the half-height).
	GetCapsuleComponent()->SetCapsuleSize(16.0f, 48.0f);
	BaseEyeHeight = 42.0f;   // WOTPawn.uc:1576
	UnderWaterTime = 20.0f;  // WOTPawn.uc:1577 (players stay 30, WOTPlayer.uc:7283)

	// WOTPawn movement defaults (WOTPawn.uc:1572,1575). GroundSpeed's CDO is
	// 600, but PreBeginPlay immediately re-rolls it from the 400/400 band —
	// BeginPlay below reproduces that, so placed pawns run 400 like retail.
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->MaxWalkSpeed = 600.0f;
	Move->MaxAcceleration = 1024.0f;
	Move->MaxSwimSpeed = 400.0f; // WOTPawn.uc:1573; rows replace it with WaterSpeed.
	// Every supported creature row inherits Grunt's Buoyancy=150 (Grunt.uc:2434);
	// none overrides it, so it is a class constant rather than a row field.
	// Retail swimming divides it by Mass, which ConfigureNPC takes from the row.
	Move->Buoyancy = 150.0f;

	// NPCs turn by steering toward the controller's desired rotation at
	// RotationRate, the UE1 physicsRotation model — not by snapping to the
	// controller yaw the way the player pawn does.
	bUseControllerRotationYaw = false;
	Move->bUseControllerDesiredRotation = true;
	Move->bOrientRotationToMovement = false;

	// The Trolloc swing pair (Trolloc.uc:100-101; notify fractions :11,:14) as the
	// working default until creature rows land (slice 5).
	FWoTMeleeAttack Run;
	Run.Damage = 15.0f;
	Run.HeightModifier = 0.0f;
	Run.DamageTypeName = TEXT("Cutup");
	Run.AnimName = TEXT("AttackRun");
	Run.NotifyFraction = 0.75f;
	FWoTMeleeAttack RunB;
	RunB.Damage = 10.0f;
	RunB.HeightModifier = -0.5f;
	RunB.DamageTypeName = TEXT("Sliced");
	RunB.AnimName = TEXT("ATTACKRUNB");
	RunB.NotifyFraction = 0.70f;
	MeleeAttacks = {Run, RunB};
}

void AWoTNPCCharacter::PostInitializeComponents()
{
	if (HasAuthority())
	{
		// Pawn.PreBeginPlay rolls health before the controller can evaluate the pawn.
		// APawn::PostInitializeComponents auto-possesses during Super, so seed from
		// the authored creature band first; otherwise the controller observes the
		// deliberately-zero player base default as a dead NPC.
		Health = FMath::RoundToInt(WoTRandom::FRandRange(HealthMPMin, HealthMPMax));
	}
	Super::PostInitializeComponents();
}

void AWoTNPCCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		// Spawn-time rolls (WOTPawn.uc:383-386; MP path — the port is MP-only).
		RetailGroundSpeed = WoTRandom::FRandRange(GroundSpeedMin, GroundSpeedMax);
		GetCharacterMovement()->MaxWalkSpeed = RetailGroundSpeed;
	}
}

void AWoTNPCCharacter::GoalInitialized(EWoTGoalIndex GoalIndex)
{
	if (GoalIndex != EWoTGoalIndex::Threat || !HasAuthority()
		|| InitThreatGoalEvent.IsNone())
	{
		return;
	}
	if (UWoTEventSubsystem* Events = GetWorld()
		? GetWorld()->GetSubsystem<UWoTEventSubsystem>() : nullptr)
	{
		Events->FireEvent(InitThreatGoalEvent, this, this);
	}
}

float AWoTNPCCharacter::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
                                  AController* EventInstigator, AActor* DamageCauser)
{
	// TeleporterReached's TakeDamage stub (Myrddraal.uc:361): the Myrddraal is
	// INVULNERABLE for the whole blink — swallow the hit before the reflector/health
	// chain runs, and skip the TakeHit/behavior notifications the state would suppress.
	if (const AWoTNPCController* NPCController = Cast<AWoTNPCController>(GetController());
	    NPCController && NPCController->IsTeleporting())
	{
		return 0.0f;
	}
	// Grunt.TakeDamage fires this authored hook before sighting and before the
	// inherited health/reflector chain. The once flag clears the event afterward.
	if (HasAuthority() && !TakeDamageEvent.IsNone())
	{
		if (UWoTEventSubsystem* Events = GetWorld()
			? GetWorld()->GetSubsystem<UWoTEventSubsystem>() : nullptr)
		{
			Events->FireEvent(TakeDamageEvent, this, this);
		}
		if (bTakeDamageEventOnce)
		{
			TakeDamageEvent = NAME_None;
		}
	}
	const AWoTNPCController* NPCController =
		Cast<AWoTNPCController>(GetController());
	const bool bSuspendBehaviorCallback = NPCController
		&& NPCController->IsSuspendedForIceFreeze();
	if (HasAuthority())
	{
		if (AWoTNPCController* MutableNPCController =
			Cast<AWoTNPCController>(GetController()))
		{
			MutableNPCController->HandleGruntTakeDamage(DamageAmount,
				EventInstigator ? EventInstigator->GetPawn() : nullptr);
		}
	}
	// Super runs the full reflector/health chain first (BATrolloc.uc:85 calls
	// Super.TakeDamage before its recovery check, which then reads the post-hit
	// Health). Then hand the RAW incoming amount to the behavior — the original's
	// TakeDamage threshold is on the unreduced Damage param.
	const float Applied = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	if (HasAuthority() && Health <= 0)
	{
		DropWeaponsOnDeath();
	}
	if (!bSuspendBehaviorCallback)
	{
		if (UWoTCreatureBehavior* CreatureBehavior =
			FindComponentByClass<UWoTCreatureBehavior>())
		{
			CreatureBehavior->NotifyTookDamage(FMath::RoundToInt(DamageAmount));
		}
	}
	return Applied;
}

void AWoTNPCCharacter::Died(AController* Killer, FName WoTDamageType,
	const FVector& HitLocation)
{
	if (bDead || !HasAuthority())
	{
		return;
	}
	// Warder.Died disables the camouflage before Super can take the immediate
	// gib-and-destroy path.  The controller tick is too late for a gibbed NPC.
	if (UWoTWarderFadeCloakBehavior* Fade =
		FindComponentByClass<UWoTWarderFadeCloakBehavior>())
	{
		Fade->SetFadeLevel(1000.0f);
	}
	// WOTPawn death drops its equipped weapons before Engine.Pawn may destroy a
	// non-player gib victim.  This is idempotent with the ordinary controller path.
	DropWeaponsOnDeath();
	Super::Died(Killer, WoTDamageType, HitLocation);
}

void AWoTNPCCharacter::ApplyMinionEyeState(bool bEyesOn)
{
	const FWoTCreatureRow* Row = WoTCreatureData::Find(CreatureRowName);
	if (!Row || !Row->bMinionEyesEnabled) return;
	constexpr int32 MinionEyeSlot = 4;
	bMinionEyesOn = bEyesOn;
	if (UWoTVertexAnimComponent* Body = Cast<UWoTVertexAnimComponent>(GetMesh()))
	{
		FWoTRetailObjectPath Path;
		const FName Identity = bEyesOn
			? Row->MinionEyesTexture
			: Row->MaterialSkinIdentities.FindRef(MinionEyeSlot);
		if (WoTParseRetailObjectPath(Identity.ToString(), Path))
		{
			Body->SetSkinOverride(MinionEyeSlot,
				FSoftObjectPath(FString::Printf(TEXT("/Game/WoT/Textures/%s.%s"),
					*WoTRetailTextureSubPath(Path), *Path.Name)));
		}
		else if (!bEyesOn)
		{
			// A null SetSkinOverride is intentionally a no-op on legacy developer
			// carriers. Reapply the map without slot 4 so both those carriers and
			// authenticated runtime meshes restore their authored default material.
			TMap<int32, FSoftObjectPath> Overrides = Body->GetRuntimeSkinOverrides();
			Overrides.Remove(MinionEyeSlot);
			Body->SetRuntimeSkinOverrides(Overrides);
		}
	}
}

void AWoTNPCCharacter::ShutdownCorpsePresentation()
{
	// Minion.Died restores MultiSkins[4] before the death pose and before
	// InitFor copies overrides to the carcass. This virtual path also runs on
	// simulated proxies when bDead replicates.
	ApplyMinionEyeState(false);
	MinionEyeToggleAt = TNumericLimits<float>::Max();
	Super::ShutdownCorpsePresentation();
}

void AWoTNPCCharacter::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
	UPrimitiveComponent* OtherComp, bool bSelfMoved, FVector HitLocation,
	FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
	Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation,
		HitNormal, NormalImpulse, Hit);
	if (HasAuthority())
	{
		if (AWoTNPCController* NPCController =
			Cast<AWoTNPCController>(GetController()))
		{
			NPCController->HandleGruntBump(Cast<APawn>(Other));
		}
	}
	EnsureLegionSpirits();
}

void AWoTNPCCharacter::NotifyHitByAngrealProjectile(
	AWoTAngrealProjectile* Projectile)
{
	// AngrealUser only specializes Ice. This callback is intentionally before
	// the projectile reflector walk, so reflect/absorb still interrupts a group.
	if (!HasAuthority() || !Cast<AWoTIceProjectile>(Projectile))
	{
		return;
	}
	if (UWoTCasterComponent* Caster = FindComponentByClass<UWoTCasterComponent>())
	{
		Caster->InterruptForIceContact();
	}
}

void AWoTNPCCharacter::EnsureLegionSpirits()
{
	if (CreatureRowName != TEXT("Legion") || GetNetMode() == NM_DedicatedServer
		|| !LegionSpirits.IsEmpty()) return;
	for (int32 Index=0; Index<3; ++Index)
	{
		UWoTVertexAnimComponent* Spirit=NewObject<UWoTVertexAnimComponent>(this,
			*FString::Printf(TEXT("LegionSpirit%d"),Index));
		if (!Spirit) continue;
		Spirit->SetupAttachment(GetRootComponent());
		Spirit->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Spirit->RegisterComponent();
		Spirit->SetVertexMesh(TEXT("LegionSpiritHead"),TEXT("WoTPawns"));
		LegionSpirits.Add(Spirit);
	}
}

void AWoTNPCCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	EnsureLegionSpirits();
	if (!LegionSpirits.IsEmpty())
	{
		LegionSpiritClock += DeltaSeconds;
		const FVector Seats[3] = {FVector::ZeroVector,FVector(-50,0,0),FVector(50,0,50)};
		for (int32 Index=0;Index<LegionSpirits.Num();++Index)
		{
			const float Direction=Index==1?-1.0f:1.0f;
			const float Angle=LegionSpiritClock*Direction*(10000.0f+Index*4000.0f)*360.0f/65536.0f;
			const float Radius=60.0f+20.0f*FMath::Sin(LegionSpiritClock*(2.0f+Index));
			const float Height=90.0f*FMath::Sin(LegionSpiritClock*(1.3f+Index*0.2f));
			LegionSpirits[Index]->SetRelativeLocation(Seats[Index]+FRotator(0,Angle,0).Vector()*Radius+FVector(0,0,Height));
			LegionSpirits[Index]->SetRelativeRotation(Index==1?FRotator(45,0,0):FRotator::ZeroRotator);
		}
	}
	const FWoTCreatureRow* CreatureRow = WoTCreatureData::Find(CreatureRowName);
	if (CreatureRow && CreatureRow->bMinionEyesEnabled && Health > 0
		&& GetWorld()->GetTimeSeconds() >= MinionEyeToggleAt)
	{
		// Minion.uc:104-129 ToggleEyes writes the function-body literal
		// MultiSkins[4]. It is behavior policy, not decoded class-default data.
		ApplyMinionEyeState(!bMinionEyesOn);
		MinionEyeToggleAt = GetWorld()->GetTimeSeconds() + (bMinionEyesOn
			? WoTRandom::FRandRange(CreatureRow->MinionEyeOnMin,
				CreatureRow->MinionEyeOnMax)
			: WoTRandom::FRandRange(CreatureRow->MinionEyeOffMin,
				CreatureRow->MinionEyeOffMax));
	}
}

void AWoTNPCCharacter::OnWalkingOffLedge_Implementation(
	const FVector& PreviousFloorImpactNormal, const FVector& PreviousFloorContactNormal,
	const FVector& PreviousLocation, float TimeDelta)
{
	Super::OnWalkingOffLedge_Implementation(PreviousFloorImpactNormal,
		PreviousFloorContactNormal, PreviousLocation, TimeDelta);
	// CheckFall dispatches this while still walking, before StartFalling. Damage
	// AddUE1Velocity, dodge launches and teleports only change movement mode.
	if (HasAuthority() && GetCharacterMovement()->IsMovingOnGround())
	{
		if (AWoTNPCController* NPCController = Cast<AWoTNPCController>(GetController()))
		{
			NPCController->HandleWalkingOffLedge();
		}
	}
}

void AWoTNPCCharacter::Landed(const FHitResult& Hit)
{
	const float ImpactVelocityZ = GetCharacterMovement()->Velocity.Z;
	AWoTNPCController* NPCController = Cast<AWoTNPCController>(GetController());
	const bool bOriginatedInFalling = NPCController
		&& NPCController->GetBrainState() == EWoTBrainState::Falling;
	Super::Landed(Hit); // WOTPawn damage and death precede FallingState's Land pose.
	if (IsValid(NPCController) && NPCController->GetPawn() == this)
	{
		NPCController->HandleFallingLanded(ImpactVelocityZ, Hit, bOriginatedInFalling);
	}
}

void AWoTNPCCharacter::TakeFallingDamage(const float VelocityZ)
{
	// WOTPawn.Landed (WOTPawn.uc:1074-1096): the same -0.15*(VZ+1050) core as the player's
	// WOTPlayer.Landed, but gated at MinLandedDamageVelocity-850 (default -1700, vs the
	// player's -1100) and gibbed below FatalLandedDamageVelocity. Land sounds/texture
	// handling are presentation/deferred.
	const float JumpZ = GetCharacterMovement()->JumpZVelocity;
	if (VelocityZ < -1.4f * JumpZ)
	{
		EmitNoise(-0.5f * VelocityZ / FMath::Max(JumpZ, 150.0f));
		if (HasAuthority() && VelocityZ <= MinLandedDamageVelocity - 850.0f)
		{
			float Damage;
			if (VelocityZ < FatalLandedDamageVelocity)
			{
				// Guaranteed gib. (The shipped ReducedDamageType != 'All' bypass has no port
				// seam — the same logged divergence as the player path.)
				Damage = Health + 250.0f;
			}
			else
			{
				Damage = -0.15f * (VelocityZ + 1050.0f);
			}
			FWoTDamageEvent Ev;
			Ev.WoTDamageType = TEXT("fell");
			Ev.UE1Momentum = FVector::ZeroVector;   // WOTPawn.uc:1093
			Ev.HitInfo.ImpactPoint = GetActorLocation();
			TakeDamage(Damage, Ev, nullptr, nullptr);
		}
	}
}

uint8 AWoTNPCCharacter::TeamOf(const APawn* Other)
{
	if (const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(Other))
	{
		return NPC->AssignedTeam;
	}
	// Players present their (possibly disguised) APPARENT team — WOTPawn.IsFriendly
	// reads GetApparentTeam (WOTPlayer.uc:1912/747/777). A held, active Disguise
	// substitutes the mimicked pawn's team (AngrealInvDisguise.CopyDisguisePawnProperties:
	// 364/373); otherwise the real PlayerState team.
	if (const AWoTCharacter* Player = Cast<AWoTCharacter>(Other))
	{
		if (const UWoTInventoryComponent* Inv = Player->InventoryComponent)
		{
			for (const TObjectPtr<AWoTInventory>& Item : Inv->Items)
			{
				if (const AWoTAngrealInvDisguise* Disguise = Cast<AWoTAngrealInvDisguise>(Item))
				{
					const int32 Apparent = Disguise->GetApparentTeam();
					if (Apparent != -1)   // GetApparentTeam's "not disguised" sentinel
					{
						return static_cast<uint8>(Apparent);
					}
					break;   // at most one Disguise per inventory
				}
			}
		}
	}
	if (const AWoTPlayerState* PS = Other ? Other->GetPlayerState<AWoTPlayerState>() : nullptr)
	{
		return PS->Team;
	}
	return WoTTeam::None;
}

bool AWoTNPCCharacter::IsFriendly(const AActor* Other) const
{
	// WOTPawn.IsFriendly (WOTPawn.uc:728): non-pawns are friendly; pawns
	// classify by this pawn's team byte. The apparent-team clauses fold into
	// TeamOf (above).
	const APawn* OtherPawn = Cast<APawn>(Other);
	if (!OtherPawn)
	{
		return true;
	}
	const bool bOtherIsPlayer = Cast<AWoTNPCCharacter>(OtherPawn) == nullptr;
	const uint8 OtherTeam = TeamOf(OtherPawn);
	switch (AssignedTeam)
	{
	case WoTTeam::NPC:
		// Friendly to other teamless NPCs and to (apparently) teamless players.
		return OtherTeam == WoTTeam::NPC;
	case WoTTeam::PC:
		return bOtherIsPlayer;
	case WoTTeam::None:
		return false;
	case WoTTeam::Any:
		return true;
	default:
		return OtherTeam == AssignedTeam;
	}
}

EWoTFaction AWoTNPCCharacter::GetActorFaction(const AActor* Other) const
{
	// Grunt.GetActorFaction (Grunt.uc:1167): everything resolves to friendly
	// or enemy; only a null actor stays unknown.
	if (!Other)
	{
		return EWoTFaction::Unknown;
	}
	return IsFriendly(Other) ? EWoTFaction::Friendly : EWoTFaction::Enemy;
}

AWoTNPCWeapon* AWoTNPCCharacter::SpawnWeapon(FName WeaponType)
{
	if (!HasAuthority() || WeaponType.IsNone() || !GetWorld())
	{
		return nullptr;
	}
	const FWoTNPCWeaponConfig* Config = WoTNPCWeaponData::Find(WeaponType);
	if (!Config)
	{
		// AngrealWeaponAdapter / ArcherBowProxy are component-backed adapters, not
		// rendered WotWeapon leaves. Absence from this registry is intentional.
		return nullptr;
	}
	UClass* WeaponClass = Config->bThrowable
		? AWoTNPCThrowableWeapon::StaticClass() : AWoTNPCWeapon::StaticClass();
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.Instigator = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTNPCWeapon* Weapon = GetWorld()->SpawnActor<AWoTNPCWeapon>(
		WeaponClass, GetActorLocation(), GetActorRotation(), Params);
	if (Weapon)
	{
		Weapon->ConfigureFromType(WeaponType);
		Weapon->OnWeaponDestroyed.AddUObject(this, &AWoTNPCCharacter::OnOwnedWeaponDestroyed);
		Weapon->Unequip();
	}
	return Weapon;
}

void AWoTNPCCharacter::InitializeWeaponSlots(FName DefaultWeaponType,
	FName MeleeWeaponType, FName RangedWeaponType)
{
	if (!HasAuthority() || MeleeWeapon || RangedWeapon)
	{
		return;
	}
	MeleeWeapon = SpawnWeapon(MeleeWeaponType);
	if (!RangedWeaponType.IsNone() && RangedWeaponType == MeleeWeaponType)
	{
		RangedWeapon = MeleeWeapon; // same actor in both slots (retail inventory)
	}
	else
	{
		RangedWeapon = SpawnWeapon(RangedWeaponType);
	}
	if (!DefaultWeaponType.IsNone() && DefaultWeaponType == RangedWeaponType)
	{
		EquipRangedWeapon();
	}
	else if (MeleeWeapon)
	{
		EquipMeleeWeapon();
	}
}

void AWoTNPCCharacter::EquipWeapon(AWoTNPCWeapon* Weapon)
{
	if (HeldWeapon == Weapon && (!Weapon || Weapon->bEquipped))
	{
		return;
	}
	AWoTNPCWeapon* Previous=HeldWeapon;
	const bool bCueWeapons=CreatureRowName==TEXT("Archer")||CreatureRowName==TEXT("Myrddraal");
	if(bCueWeapons&&Previous&&Previous!=Weapon)
		PlayVoiceSlot(Previous==RangedWeapon?TEXT("WeaponRangedSheath"):TEXT("WeaponMeleeSheath"));
	if (MeleeWeapon && MeleeWeapon != Weapon)
	{
		MeleeWeapon->Unequip();
	}
	if (RangedWeapon && RangedWeapon != Weapon && RangedWeapon != MeleeWeapon)
	{
		RangedWeapon->Unequip();
	}
	HeldWeapon = Weapon;
	if (Weapon)
	{
		Weapon->Equip(this);
		if(bCueWeapons&&Previous!=Weapon)
			PlayVoiceSlot(Weapon==RangedWeapon?TEXT("WeaponRangedDraw"):TEXT("WeaponMeleeDraw"));
	}
}

void AWoTNPCCharacter::EquipMeleeWeapon()
{
	EquipWeapon(MeleeWeapon);
}

void AWoTNPCCharacter::EquipRangedWeapon()
{
	EquipWeapon(RangedWeapon);
}

void AWoTNPCCharacter::EquipBodyAttack()
{
	// LegendPawn.GetAttackActor falls back to Self within melee distance when no
	// Weapon is held (LegendPawn.uc:1687); Trolloc/Minion keep body attacks.
	EquipWeapon(nullptr);
}

FName AWoTNPCCharacter::GetEffectiveMeleeAnimSlot() const
{
	// PerformMeleeWeaponAttack is valid only while Pawn.Weapon still resolves to
	// the inventory weapon; a thrown/destroyed weapon falls back to Self/body
	// attack (Grunt.uc:1333-1382; WotThrowableWeapon.uc:10-24).
	if (MeleeAnimSlot != FName(TEXT("MeleeWeaponAttack")))
	{
		return MeleeAnimSlot;
	}
	const AWoTNPCWeapon* Weapon = GetMeleeWeapon();
	return IsValid(Weapon) && HeldWeapon == Weapon && !Weapon->IsInFlight()
		? FName(TEXT("MeleeWeaponAttack")) : FName(TEXT("MeleeAttack"));
}

void AWoTNPCCharacter::OnOwnedWeaponDestroyed(AWoTNPCWeapon* Weapon)
{
	if (MeleeWeapon == Weapon)  { MeleeWeapon = nullptr; }
	if (RangedWeapon == Weapon) { RangedWeapon = nullptr; }
	if (HeldWeapon == Weapon)   { HeldWeapon = nullptr; }
}

void AWoTNPCCharacter::DropWeaponsOnDeath()
{
	if (!HasAuthority() || bDroppedDeathWeapons)
	{
		return;
	}
	bDroppedDeathWeapons = true;
	AWoTNPCWeapon* First = MeleeWeapon;
	AWoTNPCWeapon* Second = RangedWeapon;
	if (IsValid(First))
	{
		// A thrown weapon is absent from retail inventory while its projectile flies
		// (WotThrowableWeapon.uc:21-24), so death has no held item to corpse-toss.
		if (First->IsInFlight()) { First->Destroy(); }
		else { First->DropFrom(GetActorLocation()); }
	}
	if (IsValid(Second) && Second != First)
	{
		if (Second->IsInFlight()) { Second->Destroy(); }
		else { Second->DropFrom(GetActorLocation()); }
	}
}

void AWoTNPCCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTNPCCharacter, AssignedTeam);
	DOREPLIFETIME(AWoTNPCCharacter, MeleeWeapon);
	DOREPLIFETIME(AWoTNPCCharacter, RangedWeapon);
	DOREPLIFETIME(AWoTNPCCharacter, HeldWeapon);
	// The creature-row identity — clients re-derive the body from the registry.
	DOREPLIFETIME(AWoTNPCCharacter, CreatureRowName);
	// UE1 Actor.Style/MultiSkins replicated to every client without an owner condition.
	DOREPLIFETIME_CONDITION(AWoTNPCCharacter, FadeSkinIndex, COND_None);
	DOREPLIFETIME(AWoTNPCCharacter, TeleportEnterEffectLocation);
	DOREPLIFETIME(AWoTNPCCharacter, TeleportExitEffectLocation);
	DOREPLIFETIME(AWoTNPCCharacter, TeleportPresentationPhase);
	DOREPLIFETIME(AWoTNPCCharacter, TeleportPresentationSerial);
}

void AWoTNPCCharacter::EmitTeleportPresentation(
	const uint8 Phase, const FVector& Location)
{
	if (!HasAuthority()) return;
	TeleportPresentationPhase = Phase;
	if (Phase == 1) TeleportEnterEffectLocation = Location;
	else if (Phase == 2) TeleportExitEffectLocation = Location;
	++TeleportPresentationSerial;
	ApplyTeleportPresentation();
	if (UWorld* World = GetWorld())
	{
		const FName Sound = Phase == 3
			? FName(TEXT("WoTPawns/Myrddraal_Reappear"))
			: FName(TEXT("WoTPawns/Myrddraal_Teleport"));
		WoTSound::PlayReplicated(*World, Sound, Location, 2048.0f);
	}
	ForceNetUpdate();
}

void AWoTNPCCharacter::OnRep_TeleportPresentation()
{
	ApplyTeleportPresentation();
}

void AWoTNPCCharacter::ApplyTeleportPresentation()
{
	if (!GetWorld() || GetNetMode() == NM_DedicatedServer
		|| (TeleportPresentationPhase != 1 && TeleportPresentationPhase != 2))
	{
		return;
	}
	const bool bExit = TeleportPresentationPhase == 2;
	const FVector Location = bExit
		? FVector(TeleportExitEffectLocation) : FVector(TeleportEnterEffectLocation);
	TObjectPtr<AWoTParticleSprayer>& Slot =
		bExit ? TeleportExitSprayer : TeleportEnterSprayer;
	TObjectPtr<AWoTActorRotator>& Rotator =
		bExit ? TeleportExitSprayerRotator : TeleportEnterSprayerRotator;
	if (!IsValid(Slot))
	{
		Slot = WoTWeaveFX::SpawnLocal(*GetWorld(),
			TEXT("MyrddraalTeleportSprayer2"), Location,
			FRotator(90.0f, 0.0f, 0.0f));
	}
	if (!IsValid(Rotator) && IsValid(Slot))
	{
		Rotator = GetWorld()->SpawnActor<AWoTActorRotator>(
			AWoTActorRotator::StaticClass(), Location, FRotator(90.0f, 0.0f, 0.0f));
		if (IsValid(Rotator))
		{
			Rotator->bBounceHeight = true;
			Rotator->HeightShiftRate = 80.0f;
			Rotator->MaxHeight = 72.0f;
			Rotator->MaxRadius = 24.0f;
			Rotator->MaxRotationRate = 100000.0f;
			Rotator->MinHeight = -30.0f;
			Rotator->MinRadius = 5.0f;
			Rotator->MinRotationRate = 80000.0f;
			Rotator->RadiusShiftRate = 8.0f;
			Rotator->RotationShiftRate = 1000.0f;
			Rotator->DrivenActors.Add(TSoftObjectPtr<AActor>(Slot));
			Rotator->SetActorTickEnabled(true);
			Rotator->Initialize();
		}
	}
	if (IsValid(Slot))
	{
		Slot->SetActorLocation(Location);
		Slot->WoTTrigger(this, this);
	}
	if (IsValid(Rotator))
	{
		Rotator->SetActorLocation(Location);
		Rotator->Initialize();
	}
}

void AWoTNPCCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Myrddraal.uc:89-116 Destroyed() tears down every local teleport helper.
	// These actors are renderer-local, so cleanup must run on every machine.
	for (AActor* PresentationActor : {
		static_cast<AActor*>(TeleportEnterSprayer.Get()),
		static_cast<AActor*>(TeleportExitSprayer.Get()),
		static_cast<AActor*>(TeleportEnterSprayerRotator.Get()),
		static_cast<AActor*>(TeleportExitSprayerRotator.Get())})
	{
		if (IsValid(PresentationActor)) PresentationActor->Destroy();
	}
	TeleportEnterSprayer = nullptr;
	TeleportExitSprayer = nullptr;
	TeleportEnterSprayerRotator = nullptr;
	TeleportExitSprayerRotator = nullptr;
	Super::EndPlay(EndPlayReason);
}

void AWoTNPCCharacter::OnRep_CreatureRow()
{
	// Bind the row-derived client state from the static registry. Authority already
	// applied both halves in ConfigureNPC; OnRep is the remote-client entry point.
	WoTCreatureData::BindCreatureBody(*this, CreatureRowName);
	if (const FWoTCreatureRow* Row = WoTCreatureData::Find(CreatureRowName))
	{
		WoTCreatureData::ApplyBlockingColumns(*this, *Row);
	}
}

void AWoTNPCCharacter::SetFadeSkinIndex(const uint8 NewIndex)
{
	if (!HasAuthority())
	{
		return;
	}
	const uint8 EffectiveIndex = NewIndex < WoTNPCWarderFadeSkinCount
		? NewIndex : VisibleFadeSkinIndex;
	if (FadeSkinIndex == EffectiveIndex)
	{
		return;
	}
	FadeSkinIndex = EffectiveIndex;
	ApplyFadeSkinLook();
	ForceNetUpdate();
}

void AWoTNPCCharacter::OnRep_FadeSkinIndex()
{
	ApplyFadeSkinLook();
}

void AWoTNPCCharacter::RestoreFadeSkinMaterials(const bool bForceRuntimeBake)
{
	USkeletalMeshComponent* Body = GetMesh();
	if (IsValid(Body))
	{
		if (UWoTVertexAnimComponent* Display = Cast<UWoTVertexAnimComponent>(Body))
		{
			Display->ClearSkinOverrides();
		}
		for (int32 Slot = 0; Slot < FadeSkinOriginalMaterials.Num(); ++Slot)
		{
			Body->SetMaterial(Slot, FadeSkinOriginalMaterials[Slot]);
		}
	}
	FadeSkinMaterials.Reset();
	FadeSkinOriginalMaterials.Reset();

	// The one-shot post-spawn bake may have run while the modulated slots were active.
	// Restoring the saved originals must re-arm their MeshLit runtime parameters.
	if (bForceRuntimeBake)
	{
		if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
		{
			MeshLight->ForceRuntimeBake();
		}
	}
}

void AWoTNPCCharacter::ApplyFadeSkinLook()
{
	USkeletalMeshComponent* Body = GetMesh();
	if (!IsValid(Body))
	{
		return;
	}
	UWoTVertexAnimComponent* Display = Cast<UWoTVertexAnimComponent>(Body);
	if (FadeSkinIndex == VisibleFadeSkinIndex)
	{
		if (GetNetMode() == NM_DedicatedServer)
		{
			if (Display) Display->ClearSkinOverrides();
			return;
		}
		RestoreFadeSkinMaterials(true);
		return;
	}

	if (!Display)
	{
		return;
	}

	if (FadeSkinOriginalMaterials.Num() == 0)
	{
		const int32 MaterialCount = Body->GetNumMaterials();
		FadeSkinOriginalMaterials.Reserve(MaterialCount);
		for (int32 Slot = 0; Slot < MaterialCount; ++Slot)
		{
			FadeSkinOriginalMaterials.Add(Body->GetMaterial(Slot));
		}
	}

	// Skin= is a B3 texture override first; the modulate wrapper is then rebuilt
	// from the newly routed slots so every fade frame works identically in a
	// cooked development build and from authenticated runtime bytes.
	if (!Display->SetAllSkinOverrides(WoTNPCWarderFadeSkinPaths[FadeSkinIndex]))
	{
		return;
	}
	// The dedicated server keeps the normalized live skin identities so a
	// disguise can copy the source's CURRENT fade frame into its replicated
	// appearance driver. It never creates or renders material instances.
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	FadeSkinMaterials.Reset();
	FadeSkinMaterials.Reserve(Body->GetNumMaterials());
	for (int32 Slot = 0; Slot < Body->GetNumMaterials(); ++Slot)
	{
		UMaterialInstanceDynamic* FadeMaterial =
			WoTMeshModulate::CreateInstance(this, Body->GetMaterial(Slot));
		FadeSkinMaterials.Add(FadeMaterial);
		if (IsValid(FadeMaterial))
		{
			Body->SetMaterial(Slot, FadeMaterial);
		}
	}
	EnsureLegionSpirits();
}

void AWoTNPCCharacter::PrepareFadeSkinBodyRebind()
{
	if (FadeSkinIndex != VisibleFadeSkinIndex)
	{
		RestoreFadeSkinMaterials(false);
	}
}

void AWoTNPCCharacter::FinishFadeSkinBodyRebind()
{
	if (FadeSkinIndex != VisibleFadeSkinIndex)
	{
		ApplyFadeSkinLook();
	}
}
