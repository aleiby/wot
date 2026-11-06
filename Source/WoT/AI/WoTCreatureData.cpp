#include "WoTCreatureData.h"
#include "../WoTRandom.h"

#include "WoTBrainComponent.h"
#include "WoTCasterComponent.h"
#include "WoTCreatureBehaviors.h"
#include "WoTNPCCharacter.h"
#include "WoTNPCController.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../WoTMeshLight.h"
#include "../WoTRetailText.h"
#include "../WoTVertexAnimComponent.h"
#include "../Assets/WoTRetailIdentity.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"

namespace
{
	TUniquePtr<FWoTCreatureSet>& InstalledCreatureRegistry()
	{
		static TUniquePtr<FWoTCreatureSet> Set;
		return Set;
	}

	const FWoTCreatureSet* ActiveCreatureRegistry()
	{
		if (const TUniquePtr<FWoTCreatureSet>& Installed = InstalledCreatureRegistry())
		{
			return Installed.Get();
		}
		return nullptr;
	}

}

void FWoTCreatureDataBuilder::Install(FWoTCreatureSet&& Set)
{
	InstalledCreatureRegistry() = MakeUnique<FWoTCreatureSet>(MoveTemp(Set));
}

#if WITH_AUTOMATION_TESTS
void FWoTCreatureDataBuilder::Reset()
{
	InstalledCreatureRegistry().Reset();
}
#endif

void FWoTCreatureRow::ConfigureNPC(AWoTNPCCharacter& NPC, AWoTNPCController* Controller) const
{
	if (RowName == TEXT("MashadarFocusPawn"))
	{
		if (FWoTRetailText::EnsureLoaded())
		{
			if (const FString* Name = FWoTRetailText::FindSectionValue(
				TEXT("MashadarInventory"), TEXT("Title")))
			{
				NPC.SummonDisplayName = *Name;
			}
		}
	}
	// --- Stats (the MP band drives the spawn roll — WOTPawn.uc:383) ---
	NPC.HealthMPMin = HealthMPMin;
	NPC.HealthMPMax = HealthMPMax;
	NPC.MaxHealth = FMath::RoundToInt(HealthMPMax);
	// Roll now so a direct (post-BeginPlay) apply takes effect; the deferred-spawn
	// path re-rolls the same band in BeginPlay (identical since Min==Max here).
	NPC.Health = FMath::RoundToInt(WoTRandom::FRandRange(HealthMPMin, HealthMPMax));
	NPC.BrokenMoralePct = BrokenMoralePct;

	// --- Movement ---
	NPC.GroundSpeedMin = GroundSpeedMin;
	NPC.GroundSpeedMax = GroundSpeedMax;
	NPC.AnimationGroundSpeed = GroundSpeed;
	NPC.AnimationBaseWalkingSpeed = BaseWalkingSpeed;
	if (UCharacterMovementComponent* Move = NPC.GetCharacterMovement())
	{
		const float Rolled = WoTRandom::FRandRange(GroundSpeedMin, GroundSpeedMax);
		NPC.RetailGroundSpeed = Rolled;
		Move->MaxWalkSpeed = Rolled;
		Move->MaxFlySpeed = Rolled;
		Move->MaxSwimSpeed = WaterSpeed;
		if (RotationRateYaw > 0)
		{
			Move->RotationRate = FRotator(0.0f, static_cast<float>(RotationRateYaw), 0.0f);
		}
		// The row's authored Mass (review-caught P28 D1: this was never plumbed, so
		// the Legion carcass read the 175 ctor default and the >=500 LandThump gate
		// never fired). Also parity for incoming knockback — the damage chain
		// divides momentum by this mass (Legion 3000 = near-immovable, Legion.uc:528).
		if (Mass > 0.0f)
		{
			Move->Mass = Mass;
		}
	}

	// The per-goal chase/wait suggested speeds (LegendPawn GoalSuggestedSpeeds fold).
	// They survive InitGoal (WoTBrainComponent.cpp:135) so writing them at spawn holds.
	if (UWoTBrainComponent* Brain = NPC.Brain)
	{
		for (EWoTGoalIndex Idx : {EWoTGoalIndex::Threat, EWoTGoalIndex::Refuge,
		                          EWoTGoalIndex::Guarding, EWoTGoalIndex::Colleague,
		                          EWoTGoalIndex::Intermediate})
		{
			Brain->GetGoal(Idx).SuggestedSpeed = ChaseSuggestedSpeed;
		}
		Brain->GetGoal(EWoTGoalIndex::Waiting).SuggestedSpeed = WaitSuggestedSpeed;
	}

	// --- Combat ---
	NPC.MeleeRange = MeleeRange;
	NPC.MeleeAttacks = MeleeAttacks;
	// Combat-anim binding (presentation Slice 3): the shipped AnimationTableClass=
	// ref, the effective melee slot, and the hit-react gates. The hit ratios
	// reference the authored default.health (WOTPawn.uc:834-844) = HealthPlaced,
	// not the rolled MP band.
	NPC.AnimTableName = AnimTable;
	NPC.MeleeAnimSlot = MeleeAnimSlot;
	// Defensive dodge arming (DefensiveDetector*.uc): only Warder/Trolloc/Minion set it.
	NPC.bDefensiveDodge = bDefensiveDodge;
	NPC.DodgeResponseMinTime = DodgeResponseMinTime;
	NPC.DodgeResponseMaxTime = DodgeResponseMaxTime;
	NPC.DodgeCollectionRadius = DodgeCollectionRadius;
	NPC.ChanceOfHitAnim = ChanceOfHitAnim;
	NPC.HitSoftHealthRatio = HitSoftHealthRatio;
	NPC.HitReferenceHealth = static_cast<float>(HealthPlaced);
	// Carcass swap data (Slice 4a): the shipped CarcassType ref + gib thresholds.
	NPC.CarcassTypeName = CarcassType;
	NPC.GibForSureFinalHealth = GibForSureFinalHealth;
	NPC.GibSometimesFinalHealth = GibSometimesFinalHealth;
	NPC.bBlackBlood = bBlackBlood;
	// The voice table (sound Slice 5): "WoTPawns.SoundTableTrolloc" -> "Trolloc".
	{
		FString Voice = SoundTableClass.ToString();
		int32 Dot;
		if (Voice.FindLastChar(TEXT('.'), Dot))
		{
			Voice = Voice.RightChop(Dot + 1);
		}
		Voice.RemoveFromStart(TEXT("SoundTable"));
		NPC.SoundTableName = FName(*Voice);
	}
	// The looping hum (Legion's Leg_Ambient) — replicated so clients attach it.
	NPC.AmbientHumPath = AmbientSoundPath;
	NPC.AmbientHumRadius = AmbientSoundRadius;
	NPC.AmbientHumVolume = AmbientSoundVolume;
	NPC.AmbientHumPitch = AmbientSoundPitch;

	// --- Collision / perception ---
	if (UCapsuleComponent* Cap = NPC.GetCapsuleComponent())
	{
		Cap->SetCapsuleSize(CollisionRadius, CollisionHeight);
	}
	WoTCreatureData::ApplyBlockingColumns(NPC, *this);
	NPC.BaseEyeHeight = BaseEyeHeight;
	NPC.Visibility = Visibility;
	if (!DisguiseIcon.IsNone())
	{
		FWoTRetailObjectPath Path;
		if (WoTParseRetailObjectPath(DisguiseIcon.ToString(), Path))
		{
			NPC.DisguiseIcon = FSoftObjectPath(FString::Printf(
				TEXT("/Game/WoT/Textures/%s.%s"),
				*WoTRetailTextureSubPath(Path), *Path.Name));
		}
	}

	// --- Body mesh (WOTPawn Mesh=LodMesh'WoTPawns.X') ---
	// The replicated row identity: clients re-derive the body from the static
	// registry via OnRep_CreatureRow (before this, NOTHING row-shaped replicated
	// and net clients had no way to bind a creature's mesh — PORT_NOTES).
	NPC.CreatureRowName = RowName;
	WoTCreatureData::BindCreatureBody(NPC, RowName);

	// --- Orders / leadership flags (the controller reads these at OnPossess) ---
	NPC.bIsCaptain = bIsCaptain;
	NPC.AssignedOrders = AssignedOrders;
	NPC.bChampionOrders = bChampionOrders;

	// LegionTargetingReflector.uc:8: the Legion's weaves exclude the spirit family from
	// their targetable set (the reflector-install seam, folded to a pawn flag).
	NPC.bExcludesLegionProjectileTarget = bExcludesLegionProjectileTarget;

	// --- Caster loadout (AngrealUser family) ---
	if (Weaves.Num() > 0)
	{
		UWoTCasterComponent* Caster = NPC.FindComponentByClass<UWoTCasterComponent>();
		if (!Caster)
		{
			Caster = NewObject<UWoTCasterComponent>(&NPC);
			Caster->RegisterComponent();
		}
		Caster->HoldRange = HoldRange;
		Caster->MinOffensiveUsageInterval = MinOffensiveAngrealUsageInterval;
		Caster->MinDefensiveUsageInterval = MinDefensiveAngrealUsageInterval;
		Caster->MinProjectileRange = AdapterMinProjectileRange;
		Caster->MaxProjectileRange = AdapterMaxProjectileRange;
		Caster->bRequireTargetVisibility = bAdapterRequireTargetVisibility;
		Caster->Weaves.Reset();
		for (const FWoTCreatureWeave& W : Weaves)
		{
			if (!W.WeaveClass)
			{
				continue;
			}
			FWoTWeaveSlot Slot;
			Slot.WeaveClass = W.WeaveClass;
			Slot.Priority = W.Priority;
			Slot.MinChargesInGroup = W.MinChargesInGroup;
			Slot.MaxChargesInGroup = W.MaxChargesInGroup;
			Slot.MinChargeGroupInterval = W.MinChargeGroupInterval;
			Caster->Weaves.Add(Slot);
		}
	}

	// --- Behavior hook (slice 5b): instantiate the per-creature behavior the row
	// names. Create() binds the ported hooks (ShieldDeflect / WarderFadeCloak /
	// MinionStealth) and returns null for the not-yet-ported names (later batches).
	if (BehaviorClass != NAME_None)
	{
		UWoTCreatureBehavior* Behavior = UWoTCreatureBehavior::Create(NPC, BehaviorClass);
		if (UWoTShieldDeflectBehavior* Shield = Cast<UWoTShieldDeflectBehavior>(Behavior))
		{
			// The per-creature reflector config (ShieldUser.PostBeginPlay copies these,
			// uc:42-46). Soldier: bShieldReflectorDeflect=True (Soldier.uc:50) +
			// ShieldReflectorSoundName (Soldier.uc:48). Questioner: lead-reflect
			// (bShieldReflectorLeadWhenReflecting=True, Questioner.uc:35) and its active
			// detector keeps seeker rejection OFF (DefensiveDetectorQuestioner omits the
			// SeekingProjectile rejection), so it raises its shield against seekers too.
			if (RowName == TEXT("Soldier"))
			{
				Shield->bReflectorDeflect = true;
				// Sounds\Weapon\Shield\Shield_Deflect1.wav (Soldier.uc:40) imported
				// under /Game/WoT/Sounds; the exact subpath is confirmed at bake time
				// (PORT_NOTES). NAME_None would keep the base "WOT/Reflected".
				Shield->ReflectSoundOverride = TEXT("Weapon/Shield/Shield_Deflect1");
			}
			else if (RowName == TEXT("Questioner"))
			{
				Shield->bReflectorDeflect = false;
				Shield->bReflectorLeadWhenReflecting = true;
				Shield->bRejectSeekers = false;
			}
		}
	}

	// --- Shipped initial state (bridged by SpawnCreature's bCombatReady) ---
	if (Controller)
	{
		Controller->InitialState = InitialState;
	}
}

const FWoTCreatureRow* WoTCreatureData::Find(FName RowName)
{
	const FWoTCreatureSet* Set = ActiveCreatureRegistry();
	return Set == nullptr ? nullptr : Set->Rows.Find(RowName);
}

float WoTCreatureData::TracerSpinAccelerationRollDegPerSec2()
{
	const FWoTCreatureSet* Set = ActiveCreatureRegistry();
	return Set ? Set->TracerSpinAccelerationRollDegPerSec2 : 0.0f;
}

void WoTCreatureData::ApplyBlockingColumns(
	AWoTNPCCharacter& NPC, const FWoTCreatureRow& Row)
{
	if (UCapsuleComponent* Capsule = NPC.GetCapsuleComponent())
	{
		// Retail has three flags, but every shipped row has them agreeing and the
		// port routes pawn-vs-pawn and projectile-vs-pawn queries through ECC_Pawn.
		const bool bBlocksPawnChannel =
			Row.bBlockActors || Row.bBlockPlayers || Row.bProjTarget;
		Capsule->SetCollisionResponseToChannel(
			ECC_Pawn, bBlocksPawnChannel ? ECR_Block : ECR_Ignore);
	}
}

bool WoTCreatureData::BindCreatureBody(AWoTNPCCharacter& NPC, FName RowName)
{
	// DrawScale is the UE1 mesh-only multiplier (collision stays the unscaled
	// capsule), so it applies to the mesh component, not the actor.
	const FWoTCreatureRow* Row = Find(RowName);
	UWoTVertexAnimComponent* Body =
		Cast<UWoTVertexAnimComponent>(NPC.GetMesh());
	if (!Row || !Body)
	{
		return false;
	}
	const bool bReapplyFadeSkin =
		NPC.GetFadeSkinIndex() != AWoTNPCCharacter::VisibleFadeSkinIndex;
	if (bReapplyFadeSkin)
	{
		NPC.PrepareFadeSkinBodyRebind();
	}
	const bool bMeshBound = Body->SetVertexMesh(Row->Mesh);
	if (bMeshBound)
	{
		Body->EmptyOverrideMaterials();
	}
	Body->SetRelativeScale3D(FVector(Row->DrawScale));
	// Retail leaf MultiSkins decoded with their exact indices.
	for (const TPair<int32, FName>& Pair : Row->MaterialSkinIdentities)
	{
		FWoTRetailObjectPath Path;
		if (WoTParseRetailObjectPath(Pair.Value.ToString(), Path))
		{
			Body->SetSkinOverride(Pair.Key, FSoftObjectPath(FString::Printf(
				TEXT("/Game/WoT/Textures/%s.%s"),
				*WoTRetailTextureSubPath(Path), *Path.Name)));
		}
	}
	NPC.SetScaleGlowPresentation(Row->ScaleGlow);
	if (bReapplyFadeSkin)
	{
		NPC.FinishFadeSkinBodyRebind();
	}
	// A post-spawn row change exposes fresh authored materials after the one-shot
	// bake may already be spent. This is safe while faded: the modulated root has no
	// MeshLit parameters and is left untouched; reveal re-bakes restored originals.
	if (bMeshBound && NPC.GetNetMode() != NM_DedicatedServer)
	{
		if (UWoTMeshLight* MeshLight = NPC.FindComponentByClass<UWoTMeshLight>())
		{
			MeshLight->ForceRuntimeBake();
		}
	}
	return bMeshBound;
}

void WoTCreatureData::GetAllRowNames(TArray<FName>& OutNames)
{
	if (const FWoTCreatureSet* Set = ActiveCreatureRegistry())
	{
		Set->Rows.GetKeys(OutNames);
	}
	else
	{
		OutNames.Reset();
	}
}

FName WoTCreatureData::GetTroopRow(FName FactionName, FName TierKeyword)
{
	// GetTroop (WOTPlayer.uc:4076): linear scan for the (Player, Troop) tuple.
	const FWoTCreatureSet* Set = ActiveCreatureRegistry();
	if (Set == nullptr) return NAME_None;
	for (const FWoTPlayerTroopRow& Entry : Set->PlayerTroops)
	{
		if (Entry.Faction == FactionName && Entry.Tier == TierKeyword)
		{
			return Entry.RowName;
		}
	}
	return NAME_None;
}

AWoTNPCCharacter* WoTCreatureData::SpawnCreature(UWorld& World, FName RowName, const FVector& Location,
                                                 uint8 Team, bool bCombatReady,
                                                 TSubclassOf<AWoTNPCCharacter> PawnClass)
{
	const FWoTCreatureRow* Row = Find(RowName);
	if (!Row)
	{
		return nullptr;
	}
	UClass* SpawnClass = PawnClass ? PawnClass.Get() : AWoTNPCCharacter::StaticClass();
	const FTransform Xform(FRotator::ZeroRotator, Location);

	// Deferred so the row's stats land BEFORE the BeginPlay health/speed roll
	// (WoTNPCCharacter.cpp:55), and so team is set before the first perception sample.
	AWoTNPCCharacter* NPC = World.SpawnActorDeferred<AWoTNPCCharacter>(
		SpawnClass, Xform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!NPC)
	{
		return nullptr;
	}
	NPC->AssignedTeam = Team;
	// Manual possession so the controller's InitialState is set before OnPossess runs.
	NPC->AutoPossessAI = EAutoPossessAI::Disabled;
	Row->ConfigureNPC(*NPC, nullptr);
	NPC->FinishSpawning(Xform);
	// FinishSpawning registers the display component. Rebind once at the same
	// post-registration seam used by replicated row changes so leaf MultiSkins
	// (TrollocStrong/Sitter) are retained even by developer carriers that expose
	// no material slots while the actor is still deferred.
	WoTCreatureData::BindCreatureBody(*NPC, RowName);

	AWoTNPCController* Controller = World.SpawnActor<AWoTNPCController>();
	if (Controller)
	{
		// bCombatReady selects the spawn disposition: specs/summons want the pawn live
		// now (Waiting); a placed citadel troop passes false to take the shipped
		// InitialState (Inactive/Dormant), which now sticks and wakes on proximity via
		// the controller's activity-LOD poll (AWoTNPCController::TickActivityLOD).
		Controller->InitialState = bCombatReady ? EWoTBrainState::Waiting : Row->InitialState;
		Controller->Possess(NPC);
	}
	return NPC;
}
