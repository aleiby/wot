#include "WoTAngrealInvGuardian.h"

#include "../Projectiles/WoTMashadarGuide.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerState.h"
#include "../WoTDecal.h"
#include "../WoTAppearEffect.h"
#include "../AI/WoTPathNodeIterator.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"

AWoTAngrealInvGuardian::AWoTAngrealInvGuardian()
{
	BindStatusIcons(TEXT("I_Guardian"), TEXT("M_Guardian"));
	// AngrealInvGuardian defaultproperties (AngrealInvGuardian.uc:81-100).
	// AngrealInvGuardian.uc:84-85 summon sparkles.
	TopSparkle = TEXT("APurpleCorona");
	BottomSparkle = TEXT("AWhiteCorona");
	bElementSpirit = true;
	bRare = true;
	MaxCharges = 5;
	MinChargeGroupInterval = 10.0f;
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateGN.ActivateGN"));
}

bool AWoTAngrealInvGuardian::IsMashadarSummon() const
{
	// The raw faction+tier troop lookup (before the veto below) — Hound-Captain resolves
	// to the MashadarFocusPawn row.
	return AWoTResourceSpawnerInv::GetResourceRow() == TEXT("MashadarFocusPawn");
}

FName AWoTAngrealInvGuardian::GetResourceRow() const
{
	// Keep the Mashadar row OUT of the base creature flow — the Mashadar cast path
	// (CastMashadar) handles it. (GetResourceClass == None on the Hound path,
	// AngrealInvGuardian.uc:38-42.)
	const FName Row = Super::GetResourceRow();
	return (Row == TEXT("MashadarFocusPawn")) ? NAME_None : Row;
}

void AWoTAngrealInvGuardian::CastArtifact()
{
	// AngrealInvGuardian.Cast (uc:34): the Hound-Captain troop is the Mashadar tendril;
	// everything else is the base captain summon.
	if (IsMashadarSummon())
	{
		CastMashadar();
		return;
	}
	Super::CastArtifact();
}

void AWoTAngrealInvGuardian::CastMashadar()
{
	// HaveHelper (uc:60-79): one live tendril per caster. Warn + no charge if we already
	// have one.
	if (CurrentGuide.IsValid())
	{
		ClientMessageToOwner(LimitWarning);
		return;
	}

	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	UWorld* World = GetWorld();
	if (!OwnerPawn || !World)
	{
		return;
	}

	// MashadarArenaSpawner.uc:33-52: try a 100uu forward geometry trace,
	// otherwise trace down from the authored forward offset; both the crack and
	// guide use the resulting surface point/normal.
	const FVector Forward = OwnerPawn->GetActorForwardVector().GetSafeNormal();
	const FVector OffsetLocation = OwnerPawn->GetActorLocation()
		+ Forward * (OwnerPawn->GetSimpleCollisionRadius() + SpawnSeparation + 50.0f);
	FHitResult PlacementHit;
	bool bHit = WoTVisBlockingTraceRecursive(World, OwnerPawn->GetActorLocation(),
		Forward, 100.0f, OwnerPawn, PlacementHit);
	if (!bHit)
	{
		bHit = WoTVisBlockingTraceRecursive(World, OffsetLocation,
			FVector(0.0f, 0.0f, -1.0f), 0.0f, OwnerPawn, PlacementHit);
	}
	if (!bHit)
	{
		Failed();
		return;
	}
	const FVector SpawnLocation = PlacementHit.ImpactPoint + PlacementHit.ImpactNormal;
	const FWoTDecalConfig* CrackConfig = WoTDecalFX::Find(TEXT("MashadarCrack"));
	FVector CrackLocation;
	FRotator CrackRotation;
	float CrackScale = CrackConfig ? CrackConfig->DrawScale : 0.0f;
	if (!CrackConfig || !WoTDecalFX::FindAlignedFit(*World, SpawnLocation,
		PlacementHit.ImpactNormal, CrackScale, CrackLocation, CrackRotation,
		CrackScale, OwnerPawn))
	{
		Failed();
		return;
	}
#if WITH_AUTOMATION_TESTS
	LastValidatedCrackTransformForTest = FTransform(CrackRotation, CrackLocation);
	LastValidatedCrackScaleForTest = CrackScale;
#endif
	// Presentation is allowed to be absent (notably NM_DedicatedServer). The
	// authoritative trace/four-corner fit above, not cosmetic allocation, gates
	// the guide and charge.
	AWoTDecal* Crack = WoTDecalFX::SpawnAlignedFit(*World, TEXT("MashadarCrack"),
		CrackLocation, CrackRotation, CrackScale, PlacementHit.ImpactNormal,
		/*bReplicated*/ true);
#if WITH_AUTOMATION_TESTS
	LastSpawnedCrackForTest = Crack;
#endif

	const FTransform GuideTransform(PlacementHit.ImpactNormal.Rotation(), SpawnLocation);
	AWoTMashadarGuide* Guide = World->SpawnActorDeferred<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), GuideTransform, OwnerPawn, OwnerPawn,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Guide)
	{
		if (IsValid(Crack))
		{
			Crack->Destroy();
		}
		Failed();   // AngrealInvGuardian.uc:71/109
		return;
	}
	Guide->SearchRadius = 1000.0f;
	Guide->TriggerRadius = 1000.0f;
	Guide->Speed = 200.0f;
	// ArenaSpawner authors 300 before Initialize; MashadarGuide.Initialize then
	// replaces it with MPMaxSpeed=400 in every network game.
	Guide->MaxSpeed = World->GetNetMode() == NM_Standalone ? 300.0f : 400.0f;
	Guide->Acceleration = 15.0f;
	Guide->FinishSpawning(GuideTransform);

	// Team copied from the caster's real PRI team (MashadarArenaSpawner.uc:61 reads
	// PlayerReplicationInfo.Team, not the disguised apparent team) + owner for credit.
	uint8 OwnerTeam = 255;
	if (const AWoTPlayerState* PS = OwnerPawn->GetPlayerState<AWoTPlayerState>())
	{
		OwnerTeam = PS->Team;
	}
	Guide->Team = OwnerTeam;
	Guide->SummonOwner = Cast<AWoTCharacter>(OwnerPawn);
	Guide->SetInstigator(OwnerPawn);
	Guide->HidingPlaceLocation = SpawnLocation;
	Guide->Deploy();
	CurrentGuide = Guide;

	// SuperCast() — the grandparent AngrealInventory.Cast (activate sound), skipping the
	// base ResourceSpawner creature flow (AngrealInvGuardian.uc:52-53).
	AWoTAngrealInventory::CastArtifact();
	UseCharge();   // AngrealInvGuardian.uc:77
}
