#include "WoTAngrealInventory.h"
#include "../WoTRandom.h"

#include "../WoTBattleGameMode.h"
#include "../WoTPlayerState.h"
#include "WoTInventoryComponent.h"
#include "WoTLatentGiveToLeech.h"
#include "WoTReflector.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTSoundTable.h"
#include "../WoTSound.h"
#include "../Projectiles/WoTProjectile.h"
#include "../Projectiles/WoTAngrealProjectile.h"
#include "../Projectiles/WoTAngrealIllusionProjectile.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../WoTAppearEffect.h"
#include "../WoTUE1Names.h"

#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "NavigationSystem.h"

namespace
{
	FName SoundSubPath(const FSoftObjectPath& SoundPath)
	{
		if (SoundPath.IsNull())
		{
			return NAME_None;
		}
		FString PackagePath = SoundPath.GetLongPackageName();
		if (!PackagePath.RemoveFromStart(TEXT("/Game/WoT/Sounds/"))
			|| PackagePath.IsEmpty())
		{
			return NAME_None;
		}
		return FName(*PackagePath);
	}

	bool IsRetailTargetCandidate(const AActor* Candidate)
	{
		if (!IsValid(Candidate))
		{
			return false;
		}
		// DefaultTargetingReflector.uc:122-126. WOTPawn/WOTPlayer collapse to
		// APawn in the port; the three non-pawn names retain retail-name matching.
		if (Candidate->IsA<APawn>())
		{
			return true;
		}
		if (WoTUE1Names::ActorIsA(
				Candidate, FName(TEXT("AngrealIllusionProjectile")))
			|| WoTUE1Names::ActorIsA(
				Candidate, FName(TEXT("LegionProjectile"))))
		{
			return true;
		}
		// Retail MashadarTrailer is the shared tendril body family. The port split
		// that family into the homing guide and its IK body segments; keep this
		// alias local to targeting rather than widening generic UE1-name rules.
		return WoTUE1Names::ActorIsA(
				Candidate, FName(TEXT("MashadarTrailer")))
			|| Candidate->IsA<AWoTMashadarGuide>()
			|| Candidate->IsA<AWoTMashadarSegment>();
	}

	bool CanRetailAutoTargetReach(UWorld* World, const AActor* QueryOwner,
		const AActor* IgnoredArtifact, const FVector& Start,
		const AActor* Candidate)
	{
		if (!IsValid(World) || !IsValid(Candidate))
		{
			return false;
		}
		// AutoTargetReflector.uc:141 Trace(..., false): pawns/projectiles do
		// not force path fallback; only level/movers obstruct direct reach.
		if (!WoTVisBlockingTraceHits(
			World, Start, Candidate->GetActorLocation(), IgnoredArtifact))
		{
			return true;
		}
		if (WoTSeekNavigationUsesPathNodes())
		{
			// AutoTargetReflector.uc:110-130: no desperate endpoint fallback.
			FWoTPathNodeIterator Iterator;
			return Iterator.BuildPath(
				World, Start, Candidate->GetActorLocation(), false);
		}

		// Retained NavMesh A/B arm from before the PathNode port. No navigation
		// data means no path.
		UNavigationSystemV1* Nav =
			FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		const ANavigationData* NavData = Nav
			? Nav->GetDefaultNavDataInstance(FNavigationSystem::DontCreate) : nullptr;
		if (!IsValid(NavData))
		{
			return false;
		}
		const FPathFindingQuery Query(QueryOwner ? QueryOwner : IgnoredArtifact,
			*NavData, Start, Candidate->GetActorLocation());
		return Nav->TestPathSync(Query);
	}
}

#if WITH_AUTOMATION_TESTS
bool AWoTAngrealInventory::CanRetailAutoTargetReachForTest(UWorld* World,
	const AActor* QueryOwner, const AActor* IgnoredArtifact,
	const FVector& Start, const AActor* Candidate)
{
	return CanRetailAutoTargetReach(
		World, QueryOwner, IgnoredArtifact, Start, Candidate);
}
#endif

AWoTAngrealInventory::AWoTAngrealInventory()
{
	// AngrealInventory defaultproperties
	InventoryGroup = 1;
	MaxDesireability = 0.5f;
	// PickupSound=Sound'WOT.Player.Pickup', TaintedSoundName/FailSoundName
	// lazy loads → soft paths (deterministic import paths)
	PickupSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WOT/Pickup.Pickup"));
	TaintedSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WOT/Tainted.Tainted"));
	FailSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WOT/AngrealFail.AngrealFail"));
}

void AWoTAngrealInventory::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// replication { reliable if(Role==ROLE_Authority && bNetOwner) CurCharges, bTainted }
	DOREPLIFETIME_CONDITION(AWoTAngrealInventory, CurCharges, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AWoTAngrealInventory, bTainted, COND_OwnerOnly);
}

void AWoTAngrealInventory::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	// BeginPlay(): set initial charges (a *copy* is given on pickup, so the
	// ground item's charges never match the held one's — as the original)
	if (HasAuthority())
	{
		SetCharges();
	}
}

void AWoTAngrealInventory::BecomePickup()
{
	Super::BecomePickup();
	// AngrealInventory.BecomePickup (uc:632-646): grow the rarity-colored appear
	// rings around a VISIBLE pickup. The AE is a replicated actor — authority
	// only (proxies re-run this from OnRep_ItemState; their AE arrives on its
	// own channel). The Sleeping presentation also routes through here before
	// hiding — no rings on a hidden respawn-wait pickup.
	if (!HasAuthority() || GetItemState() == EWoTInventoryState::Sleeping)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const bool bWantsEffect = World->GetNetMode() == NM_Standalone
		? bSPAppearEffect : bMPAppearEffect;
	// uc:639: reuse only a live effect (AE == None || AE.bDeleteMe respawns it).
	if (!bWantsEffect || IsValid(AppearEffect))
	{
		return;
	}
	FName Top = NAME_None, Bottom = NAME_None;
	if (bCommon) { Top = CommonTop; Bottom = CommonBottom; }
	else if (bUncommon) { Top = UnCommonTop; Bottom = UnCommonBottom; }
	else if (bRare) { Top = RareTop; Bottom = RareBottom; }
	if (Top.IsNone())
	{
		// No rarity flag -> the AppearEffect class-default sprites (uc:301-302:
		// CyanCorona top / PurpleCorona bottom — the Common pair).
		Top = TEXT("CyanCorona");
		Bottom = TEXT("PurpleCorona");
	}
	AppearEffect = AWoTAppearEffect::SpawnWithSprites(*World, this, Top, Bottom);
}

void AWoTAngrealInventory::BecomeItem()
{
	Super::BecomeItem();
	// uc:667-671: taking the item destroys the rings (the effect lingers its
	// particles out on every machine).
	if (HasAuthority() && IsValid(AppearEffect))
	{
		AppearEffect->Destroy();
		AppearEffect = nullptr;
	}
}

void AWoTAngrealInventory::CastArtifact()
{
	UWorld* World = GetWorld();
	const FName SubPath = SoundSubPath(ActivateSound.ToSoftObjectPath());
	if (IsValid(World) && !SubPath.IsNone())
	{
		AActor* SoundOwner = GetOwner();
		// Owner slot (UE1 Owner.PlaySound SLOT_Misc): bounds looping activate
		// waves (LaunchLS has an smpl loop chunk) — the deactivate sound
		// replaces it, and pawn teardown stops it (no stuck hum).
		WoTSound::PlayReplicated(*World, SubPath,
			IsValid(SoundOwner) ? SoundOwner->GetActorLocation() : GetActorLocation(),
			1600.0f, 1.0f, 1.0f, SoundOwner);
	}
	// AngrealInventory.uc:207-210: the gate deliberately reads StatusIcon,
	// while AddIconInfo displays StatusIconFrame.
	if (bDisplayIcon && !StatusIcon.IsNull())
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner());
			IsValid(Character) && !Character->IsNPCPawn())
		{
			Character->AddIconInfo(this, true, GetFName());
		}
	}
}

void AWoTAngrealInventory::UnCastArtifact()
{
	UWorld* World = GetWorld();
	const FName SubPath = SoundSubPath(DeactivateSound.ToSoftObjectPath());
	if (IsValid(World) && !SubPath.IsNone())
	{
		AActor* SoundOwner = GetOwner();
		// Same owner slot as the activate sound — this replacement is what
		// retail relied on to END the looping cast hum.
		WoTSound::PlayReplicated(*World, SubPath,
			IsValid(SoundOwner) ? SoundOwner->GetActorLocation() : GetActorLocation(),
			1600.0f, 1.0f, 1.0f, SoundOwner);
	}
	if (bDisplayIcon && !StatusIcon.IsNull())
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner());
			IsValid(Character) && !Character->IsNPCPawn())
		{
			Character->RemoveIconInfo(GetFName());
		}
	}
}

void AWoTAngrealInventory::Reset()
{
	UnCastArtifact();
	LastChargeUsedTime = 0.0f;
	ChargesUsedInGroup = 0;
}

bool AWoTAngrealInventory::HaveEnoughCharges() const
{
	return (CurCharges - ChargeCost) >= 0;
}

void AWoTAngrealInventory::UseCharge()
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());

	// Taint turns an activation into rarity-scaled self-damage (AngrealInventory.uc:292).
	if (bTainted)
	{
		int32 TaintDamage = 0;
		if      (bCommon)   { TaintDamage = TaintCommonDamage; }
		else if (bUncommon) { TaintDamage = TaintUncommonDamage; }
		else if (bRare)     { TaintDamage = TaintRareDamage; }

		if (TaintDamage > 0 && OwnerPawn)
		{
			// Taint is a DamageEffect routed through ProcessEffect (AngrealInventory
			// .uc:287-331): a matching element shield ignores it — except an effect
			// from the shield's OWN angreal ("don't ignore things we do to ourself",
			// IgnoreElementReflector.uc:97), which IsEffectBlocked's self-exemption
			// already models.
			AWoTCharacter* OwnerCh = Cast<AWoTCharacter>(OwnerPawn);
			if (!OwnerCh || !OwnerCh->IsEffectBlocked(this, /*bDeleterious*/ true,
				/*bEmitFeedback*/ true))
			{
				FWoTDamageEvent Ev;
				Ev.WoTDamageType = FName(TEXT("SelfInflicted"));
				Ev.UE1Momentum = FVector::ZeroVector;   // AngrealInventory.uc:315
				Ev.HitInfo.ImpactPoint = OwnerPawn->GetActorLocation();
				Ev.Damage = TaintDamage;
				OwnerPawn->TakeDamage(TaintDamage, Ev,
					TaintInstigator ? TaintInstigator->GetController() : nullptr, this);
			}
		}

		// Owner.PlaySound(TaintedSound) — natively replicated (uc:321-329).
		if (UWorld* World = GetWorld())
		{
			const FName SubPath = SoundSubPath(TaintedSound.ToSoftObjectPath());
			if (!SubPath.IsNone())
			{
				WoTSound::PlayReplicated(*World, SubPath,
					OwnerPawn ? OwnerPawn->GetActorLocation() : GetActorLocation(),
					1600.0f);
			}
		}
	}

	CurCharges -= ChargeCost;
	// A use happened — the caster's volley-tick signal, independent of ChargeCost (a
	// 0-cost launcher spends no charge but still fired one projectile).
	++CastUseCount;

	// Audio feedback that charges are being consumed (Owner.PlaySound, uc:336-338;
	// no shipped class or map authors UseChargeSound — retail-dead, kept for parity).
	if (UWorld* World = GetWorld())
	{
		const FName SubPath = SoundSubPath(UseChargeSound.ToSoftObjectPath());
		if (!SubPath.IsNone())
		{
			WoTSound::PlayReplicated(*World, SubPath,
				OwnerPawn ? OwnerPawn->GetActorLocation() : GetActorLocation(),
				1600.0f);
		}
	}

	// Record the expenditure for the holder's charge-group logic (AngrealInventory.uc:341).
	LastChargeUsedTime = GetWorld()->GetTimeSeconds();
	ChargesUsedInGroup++;
	if (UWoTInventoryComponent* Inv = GetOwnerInventory())
	{
		Inv->ChargeUsed(this);
	}

	if (CurCharges <= 0)
	{
		UnCastArtifact();
		GoEmpty();
	}
}

void AWoTAngrealInventory::AddCharges(int32 NumCharges)
{
	CurCharges = FMath::Min(CurCharges + NumCharges, MaxCharges);
}

void AWoTAngrealInventory::Failed()
{
	// AngrealInventory.uc:386 — typed 'FailedMessage' lands in the hand-message slot.
	ClientMessageToOwner(Title + TEXT(" ") + FailMessage + TEXT("."),
		FName(TEXT("FailedMessage")));

	// Owner.PlaySound(FailSound) — natively replicated (uc:389-397).
	if (UWorld* World = GetWorld())
	{
		const FName SubPath = SoundSubPath(FailSound.ToSoftObjectPath());
		if (!SubPath.IsNone())
		{
			const AActor* SoundOwner = GetOwner();
			WoTSound::PlayReplicated(*World, SubPath,
				IsValid(SoundOwner) ? SoundOwner->GetActorLocation() : GetActorLocation(),
				1600.0f);
		}
	}

	if (UWoTInventoryComponent* Inv = GetOwnerInventory())
	{
		Inv->NotifyCastFailed(this);
	}
}

void AWoTAngrealInventory::ResetCharges()
{
	CurCharges = 0;
}

void AWoTAngrealInventory::SetCharges()
{
	ResetCharges();

	if (GetNetMode() == NM_Standalone)
	{
		const AWoTGameMode* Game = GetWorld()->GetAuthGameMode<AWoTGameMode>();
		const int32 Difficulty = Game ? FMath::Clamp(Game->Difficulty, 0, 2) : 0;
		const int32 ChargesToAdd = SPInitialCharges[Difficulty] > 0
			? SPInitialCharges[Difficulty]
			: Difficulty == 2
				? MinInitialCharges
				: Difficulty == 1
					? (MinInitialCharges + MaxInitialCharges) / 2
					: MaxInitialCharges;
		AddCharges(ChargesToAdd);
	}
	else
	{
		// UE1 RandRange is float; the int conversion truncates (Max is rare)
		AddCharges(static_cast<int32>(WoTRandom::FRandRange(
			static_cast<float>(MinInitialCharges), static_cast<float>(MaxInitialCharges))));
	}
}

bool AWoTAngrealInventory::HandlePickupQuery(AWoTInventory* Item)
{
	// Merge a duplicate pickup into this copy, including its charges and taint
	// (AngrealInventory.uc:525-536).
	if (Item->GetClass() == GetClass())
	{
		AWoTAngrealInventory* Angreal = CastChecked<AWoTAngrealInventory>(Item);
		AddCharges(Angreal->CurCharges);
		Angreal->SetCharges();   // prepare its next world pickup (AngrealInventory.uc:533)
		if (!bTainted)
		{
			bTainted = Angreal->bTainted;
		}
		ClientMessageToOwner(Item->PickupMessage, FName(TEXT("Pickup")));
		// AngrealInventory.uc:539: while the notify-flash is still armed on the held
		// copy, the ordinary pickup sound stays silent (the notify presentation owns it).
		if (NotifyNewPickupFadeTime == 0.0f)
		{
			// Item.PlaySound(Item.PickupSound): a server PlaySound at the item.
			WoTSound::PlayReplicatedAsset(*GetWorld(), Item->PickupSound,
				Item->GetActorLocation(), WoTSound::DefaultPlaySoundRadius);
		}
		Item->SetRespawn();
		if (Angreal->bPuzzleRespawn)
		{
			AddPuzzleRespawn(Angreal);
		}
		return true;
	}
	return false;
}

void AWoTAngrealInventory::InitializeCopy(AWoTInventory* Copy)
{
	Super::InitializeCopy(Copy);
	CastChecked<AWoTAngrealInventory>(Copy)->CurCharges = CurCharges;
	SetCharges();   // prepare this world copy for its next pickup (AngrealInventory.uc:562)
}

void AWoTAngrealInventory::AddPuzzleRespawn(AWoTAngrealInventory* Item)
{
	if (PuzzleRespawns.Num() < 5)
	{
		PuzzleRespawns.Add(Item);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("PuzzleRespawns capacity overflow."));
	}
}

void AWoTAngrealInventory::SetRespawn()
{
	if (bPuzzleRespawn)
	{
		BecomeItem();
		CurCharges = 0;
	}
	else
	{
		Super::SetRespawn();
	}
}

void AWoTAngrealInventory::GiveTo(APawn* Other)
{
	Super::GiveTo(Other);
	LastOwner = GetOwner();
	// AngrealInventory.uc:604-618 GiveTo: the FIRST pickup of a new artifact type (a
	// duplicate never reaches GiveTo — HandlePickupQuery absorbs it) plays the notify
	// sound and flashes I_NotifyPickup on the owning client, then disarms itself
	// ("Don't do this again").
	if (NotifyNewPickupFadeTime > 0.0f && Other && Other == GetOwner())
	{
		if (AWoTPlayerController* PC =
			Cast<AWoTPlayerController>(Other->GetController()))
		{
			PC->ClientNotifyNewPickup(NotifyNewPickupTexture, NotifyNewPickupFadeTime,
				NotifyNewPickupSound);
			NotifyNewPickupFadeTime = 0.0f;
		}
	}
}

void AWoTAngrealInventory::DropFrom(FVector StartLocation)
{
	UnCastArtifact();   // always safe to call
	Super::DropFrom(StartLocation);
}

void AWoTAngrealInventory::GoEmpty()
{
	// Restore every puzzle pickup accumulated by this artifact (AngrealInventory.uc:692).
	for (AWoTAngrealInventory* Item : PuzzleRespawns)
	{
		if (Item)
		{
			Item->PuzzleRespawn();
		}
	}
	PuzzleRespawns.Empty();

	// Capture the last holder before DeleteInventory. Reset() leaves LastOwner
	// intact, but retail reads LastOwner right after the delete, so hold it in a
	// local to make that ordering explicit.
	AActor* Holder = LastOwner ? LastOwner.Get() : GetOwner();

	if (UWoTInventoryComponent* Inv = GetOwnerInventory())
	{
		Inv->DeleteInventory(this);
	}

	// An NPC's exhausted artifact returns later through a respawn leech
	// (AngrealInventory.uc:704-712). The retail gate is LegendPawn(LastOwner) !=
	// None: the player pawn is a PlayerPawn, not a LegendPawn, so a player's
	// emptied weave never respawns — only an NPC's does. IsNPCPawn() is that gate.
	// Mutually exclusive with the puzzle/cleanup arms below (retail if/else-if/else).
	AWoTCharacter* NPCHolder = Cast<AWoTCharacter>(Holder);
	if (NPCHolder && NPCHolder->IsNPCPawn() && NPCRespawnTime > 0.0f)
	{
		// Reroll our charges (AngrealInventory.uc:707); RandRange is float and the
		// int assignment truncates, matching SetCharges' MP arm above.
		CurCharges = static_cast<int32>(WoTRandom::FRandRange(
			static_cast<float>(MinInitialCharges),
			static_cast<float>(MaxInitialCharges)));

		UWoTLatentGiveToLeech* RespawnLeech =
			NewObject<UWoTLatentGiveToLeech>(NPCHolder);
		RespawnLeech->Item = this;
		RespawnLeech->AffectResolution = NPCRespawnTime;
		RespawnLeech->AttachTo(NPCHolder);
	}
	// Puzzle-bound artifacts return to their spawn point (AngrealInventory.uc:714-717).
	else if (bPuzzleRespawn)
	{
		PuzzleRespawn();
	}
	// Otherwise grant a short grace period before destroying the empty artifact
	// (AngrealInventory.uc:718-722).
	else
	{
		// clean up in 1.5 min — projectiles that reference us are gone by then
		SetLifeSpan(90.0f);
	}
}

void AWoTAngrealInventory::PuzzleRespawn()
{
	// AngrealInventory.uc:732 PlaySound(RespawnSound) — server, replicated.
	WoTSound::PlayReplicatedAsset(*GetWorld(), RespawnSound, GetActorLocation(),
		WoTSound::DefaultPlaySoundRadius);
	SetCharges();
	bHeldItem = false;
	GotoItemState(EWoTInventoryState::Pickup);
}

float AWoTAngrealInventory::GetDuration() const
{
	switch (DurationType)
	{
	case EWoTDurationType::DT_Charge:
		return CurCharges;
	case EWoTDurationType::DT_Shield:
		// ShieldHitPoints arrives with the pawn port
		return GetLifeSpan();
	case EWoTDurationType::DT_Lifespan:
	default:
		return GetLifeSpan();
	}
}

bool AWoTAngrealInventory::IsNotTargetable(const AActor* Other) const
{
	// UE1 IsA(name): walk the class chain matching original script names —
	// ported classes carry the WoT prefix (AngrealProjectile → WoTAngrealProjectile)
	for (const FName& TypeName : NonTargetableTypes)
	{
		if (WoTUE1Names::ActorIsA(Other, TypeName))
		{
			return true;
		}
		if (TypeName == FName(TEXT("MashadarTrailer"))
			&& (Other->IsA<AWoTMashadarGuide>()
				|| Other->IsA<AWoTMashadarSegment>()))
		{
			return true;
		}
	}

	// AngrealInventory.uc:815-824: Level.Game exists only on authority, so a
	// client GetAuthGameMode() miss reproduces UE1's Accessed-None-to-false skip.
	if (!bTargetsFriendlies && GetWorld()
		&& GetWorld()->GetAuthGameMode<AWoTBattleGameMode>())
	{
		const APawn* OwnerPawn = Cast<APawn>(GetOwner());
		const AWoTPlayerState* OwnerPS = OwnerPawn
			? OwnerPawn->GetPlayerState<AWoTPlayerState>() : nullptr;
		const APawn* OtherPawn = Cast<APawn>(Other);
		const AWoTPlayerState* OtherPS = OtherPawn
			? OtherPawn->GetPlayerState<AWoTPlayerState>() : nullptr;
		// This branch explicitly guards both PRIs in retail, unlike giMPBattle's
		// TeamOfPawn Accessed-None-to-0 fold. Compare raw bytes: neutral 255 == 255.
		const AWoTAngrealIllusionProjectile* Illusion =
			Cast<AWoTAngrealIllusionProjectile>(Other);
		if (OwnerPS && ((OtherPS && OtherPS->Team == OwnerPS->Team)
			|| (Illusion && Illusion->Team == OwnerPS->Team)))
		{
			return true;
		}
	}
	return false;
}

AWoTAngrealInventory* AWoTAngrealInventory::CreateProxy(
	AActor* InOwner, TSubclassOf<AWoTAngrealInventory> Type)
{
	// a dummy angreal — usable as a SourceAngreal but carried by no one
	const FTransform Xform(FRotator::ZeroRotator, InOwner->GetActorLocation());
	AWoTAngrealInventory* Proxy =
		InOwner->GetWorld()->SpawnActorDeferred<AWoTAngrealInventory>(Type, Xform,
			InOwner, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	Proxy->bHeldItem = true;   // keep it out of the Pickup state
	Proxy->FinishSpawning(Xform);
	Proxy->BecomeItem();
	return Proxy;
}

bool AWoTAngrealInventory::DamageTypeContains(FName DamageType, FName Element)
{
	// Equivalent to the original's exhaustive 32-combo switch: a valid combo
	// name has exactly the pattern {A|x}{E|x}{F|x}{W|x}{S|x}, and contains the
	// element iff its letter is present at its position. Invalid names
	// ('hurt', 'SelfInflicted'...) contain nothing, as in the original.
	const FString S = DamageType.ToString();
	if (S.Len() != 5)
	{
		return false;
	}
	static const TCHAR Letters[5] = {'A', 'E', 'F', 'W', 'S'};
	for (int32 i = 0; i < 5; ++i)
	{
		if (S[i] != Letters[i] && S[i] != 'x')
		{
			return false;
		}
	}
	int32 Pos;
	if      (Element == FName(TEXT("Air")))    { Pos = 0; }
	else if (Element == FName(TEXT("Earth")))  { Pos = 1; }
	else if (Element == FName(TEXT("Fire")))   { Pos = 2; }
	else if (Element == FName(TEXT("Water")))  { Pos = 3; }
	else if (Element == FName(TEXT("Spirit"))) { Pos = 4; }
	else { return false; }
	return S[Pos] == Letters[Pos];
}

FName AWoTAngrealInventory::GetDamageType(const AWoTAngrealInventory* Item)
{
	// Compose the AEFWS combo from the element flags ('xxxxx' when none).
	if (!Item)
	{
		return FName(TEXT("xxxxx"));
	}
	TCHAR Name[6] = {'x', 'x', 'x', 'x', 'x', 0};
	if (Item->bElementAir)    { Name[0] = 'A'; }
	if (Item->bElementEarth)  { Name[1] = 'E'; }
	if (Item->bElementFire)   { Name[2] = 'F'; }
	if (Item->bElementWater)  { Name[3] = 'W'; }
	if (Item->bElementSpirit) { Name[4] = 'S'; }
	return FName(Name);
}

// ---- AI support ----

bool AWoTAngrealInventory::ShouldCast() const
{
	return HaveEnoughCharges() && (ChargesUsedInGroup < IntendedChargesInGroup);
}

void AWoTAngrealInventory::InitChargeGroup()
{
	const float ChargeScale = (ChargeCost > 0) ? (1.0f / ChargeCost) : 1.0f;
	IntendedChargesInGroup = FMath::Min<float>(CurCharges,
		WoTRandom::FRandRange(static_cast<float>(MinChargesInGroup),
		                  static_cast<float>(MaxChargesInGroup))) * ChargeScale;
}

void AWoTAngrealInventory::GetEffectiveRange(float& MinRange, float& MaxRange)
{
	MinRange = GetMinRange();
	MaxRange = GetMaxRange();
}

float AWoTAngrealInventory::GetMinRange()
{
	return 0.0f;
}

float AWoTAngrealInventory::GetMaxRange()
{
	return static_cast<float>(MAX_int32);
}

bool AWoTAngrealInventory::ShouldUncast() const
{
	return (MaxChargeUsedInterval != 0.0f) || !ShouldCast();
}

bool AWoTAngrealInventory::GetCastDuration(float& CastDuration)
{
	if (ShouldCast())
	{
		ensure(RoundsPerMinute != 0.0f);
		CastDuration = (60.0f / RoundsPerMinute) + 0.05f;
		if (MaxChargeUsedInterval == 0.0f)
		{
			// automatic-weapon style: hold for the whole charge group
			CastDuration *= IntendedChargesInGroup - ChargesUsedInGroup;
		}
		return true;
	}
	return false;
}

float AWoTAngrealInventory::GetPriority() const
{
	const float ChargeUseTimeDelta = GetWorld()->GetTimeSeconds() - LastChargeUsedTime;
	ensure(RoundsPerMinute != 0.0f);
	const float MinChargeUseTimeDelta =
		(RoundsPerMinute == 0.0f) ? 1.0f : (60.0f / RoundsPerMinute);

	if (ChargeUseTimeDelta >= MinChargeUseTimeDelta)
	{
		if (ShouldCast())
		{
			return Priority;
		}
		if (HaveEnoughCharges() && ChargeUseTimeDelta >= MinChargeGroupInterval)
		{
			const_cast<AWoTAngrealInventory*>(this)->ChargesUsedInGroup = 0;
			return Priority;
		}
	}
	return PriorityUnusable;
}

bool AWoTAngrealInventory::GetEffectivePriority(float& EffectivePriority)
{
	if (!Target || !GetOwner())
	{
		return false;
	}
	const float TargetDistance =
		FVector::Dist(Target->GetActorLocation(), GetOwner()->GetActorLocation());
	float MinRange, MaxRange;
	GetEffectiveRange(MinRange, MaxRange);
	if (TargetDistance < MinRange || TargetDistance > MaxRange)
	{
		return false;
	}
	if (AWoTAngrealProjectile* Proj = Cast<AWoTAngrealProjectile>(Target.Get()))
	{
		return GetDefensivePriority(EffectivePriority, Proj);
	}
	return GetOffensivePriority(EffectivePriority, Target);
}

bool AWoTAngrealInventory::GetDefensivePriority(
	float& DefensivePriority, AWoTAngrealProjectile* DefensiveTarget) const
{
	if (!bDefensive || !DefensiveTarget->SourceAngreal)
	{
		return false;
	}
	const AWoTAngrealInventory* Src = DefensiveTarget->SourceAngreal;
	const bool bEffective =
		(Src->bElementFire && bElementFire) || Src->bElementWater ||
		Src->bElementAir || Src->bElementEarth || Src->bElementSpirit;
	if (bEffective)
	{
		DefensivePriority = GetPriority();
		return DefensivePriority != PriorityUnusable;
	}
	return false;
}

bool AWoTAngrealInventory::GetOffensivePriority(float& OffensivePriority,
                                                AActor*) const
{
	OffensivePriority = GetPriority();
	return bOffensive && (OffensivePriority != PriorityUnusable);
}

FVector AWoTAngrealInventory::GetTrajectorySource() const
{
	FVector SourceLocation = FVector::ZeroVector;
	if (const AActor* O = GetOwner())
	{
		SourceLocation = O->GetActorLocation();
		if (const APawn* P = Cast<APawn>(O))
		{
			SourceLocation.Z += P->BaseEyeHeight;
		}
	}
	return SourceLocation;
}

bool AWoTAngrealInventory::IsTargetable(const AActor* A) const
{
	if (!IsValid(A))
	{
		return false;
	}
	if (IsNotTargetable(A))
	{
		return false;
	}
	// Exclude the caster and hidden candidates (DefaultTargetingReflector.uc:39-40).
	const AActor* Source = LastOwner ? LastOwner.Get() : GetOwner();
	// LegionTargetingReflector.uc:8 (TargetableTypes(4)=None): a pawn whose targetable set
	// excludes the LegionProjectile spirit family never picks one (the Legion ignoring its
	// own — and other Legions' — spirits). Every other caster keeps the base list, so a
	// player-owned weave still targets a spirit.
	if (const AWoTCharacter* SourceCh = Cast<AWoTCharacter>(Source);
	    SourceCh && SourceCh->bExcludesLegionProjectileTarget &&
	    WoTUE1Names::ActorIsA(A, FName(TEXT("LegionProjectile"))))
	{
		return false;
	}
	return A != Source && A != this && !A->IsHidden();
}

AActor* AWoTAngrealInventory::FindBestTarget(FVector Loc, FRotator ViewRot,
                                             float MaxAngleInDegrees)
{
	AActor* Source = LastOwner ? LastOwner.Get() : GetOwner();

	// AutoTargetReflector (Find Target) override: the nearest reachable retail
	// candidate, ignoring FOV and LOS. Reachability tries direct world geometry,
	// then the live-selected retail-path or NavMesh backend.
	if (const AWoTCharacter* Ch = Cast<AWoTCharacter>(GetOwner()))
	{
		if (Ch->HasTargetingOverride())
		{
			AActor* Best = nullptr;
			float BestDist = 0.0f;
			const FVector ReachStart = IsValid(Source)
				? Source->GetActorLocation() : Loc;
			for (TActorIterator<AActor> It(GetWorld()); It; ++It)
			{
				AActor* Candidate = *It;
				if (!IsRetailTargetCandidate(Candidate) || !IsTargetable(Candidate))
				{
					continue;
				}
				if (!CanRetailAutoTargetReach(GetWorld(), Source, this,
					ReachStart, Candidate))
				{
					continue;
				}
				const float Dist = FVector::Dist(
					Candidate->GetActorLocation(), ReachStart);
				if (!Best || Dist < BestDist)
				{
					Best = Candidate;
					BestDist = Dist;
				}
			}
			return Best;
		}
	}

	// Prefer the first qualified actor hit by the view trace (TrapDetectLeech.uc:161).
	const FVector ViewVec = ViewRot.Vector();
	FHitResult Hit;
	// DefaultTargetingReflector.uc:87-89 TraceRecursive(..., true). The shared
	// helper returns the first accepted actor in distance order with one query.
	if (WoTRetailActorTraceRecursive(GetWorld(), Loc, ViewVec, 0.0f,
		this, Hit)
	    && IsRetailTargetCandidate(Hit.GetActor())
	    && IsTargetable(Hit.GetActor()))
	{
		return Hit.GetActor();
	}

	// else the targetable retail candidate closest to view center within the FOV
	const float FOVCos = FMath::Cos(FMath::DegreesToRadians(MaxAngleInDegrees));
	FVector SightStart = Loc;
	if (const APawn* SourcePawn = Cast<APawn>(Source))
	{
		SightStart = SourcePawn->GetActorLocation()
			+ FVector(0, 0, SourcePawn->BaseEyeHeight);
	}
	AActor* Best = nullptr;
	float BestRatio = 0.0f;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Candidate = *It;
		if (!IsRetailTargetCandidate(Candidate) || !IsTargetable(Candidate))
		{
			continue;
		}
		const FVector Dir = Candidate->GetActorLocation() - Loc;
		const float LocDist = Dir.Size();
		if (LocDist <= UE_KINDA_SMALL_NUMBER)
		{
			continue;
		}
		// DistRatio = distance down the view axis over straight-line distance
		// (the cosine of the angle off view center)
		const float DistRatio = FVector::DotProduct(Dir, ViewVec) / LocDist;
		if (DistRatio < FOVCos)
		{
			continue;
		}
		// Util.uc:1243-1295 PawnCanSeeActor checks eye-to-center/top/bottom;
		// any clear TRACE_VisBlocking ray is visible.
		float CandidateRadius = 0.0f;
		float CandidateHalfHeight = 0.0f;
		Candidate->GetSimpleCollisionCylinder(
			CandidateRadius, CandidateHalfHeight);
		const FVector CandidateCenter = Candidate->GetActorLocation();
		const FVector SightPoints[3] = {
			CandidateCenter,
			CandidateCenter + FVector(0, 0, CandidateHalfHeight),
			CandidateCenter - FVector(0, 0, CandidateHalfHeight),
		};
		bool bVisible = false;
		for (const FVector& SightPoint : SightPoints)
		{
			if (!WoTVisBlockingTraceHits(
				GetWorld(), SightStart, SightPoint, this))
			{
				bVisible = true;
				break;
			}
		}
		if (!bVisible)
		{
			continue;
		}
		if (!Best || DistRatio > BestRatio)
		{
			Best = Candidate;
			BestRatio = DistRatio;
		}
	}
	return Best;
}
