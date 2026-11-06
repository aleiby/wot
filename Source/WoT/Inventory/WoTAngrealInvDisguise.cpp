#include "WoTAngrealInvDisguise.h"

#include "../WoTAppearEffect.h"
#include "../WoTPlayerColorFX.h"
#include "../WoTCharacter.h"
#include "../WoTUE1Names.h"
#include "../AI/WoTNPCCharacter.h"   // TeamOf (the mimicked pawn's apparent team)

#include "EngineUtils.h"
#include "GameFramework/Pawn.h"

AWoTAngrealInvDisguise::AWoTAngrealInvDisguise()
{
	BindStatusIcons(TEXT("I_Disguise"), TEXT("M_Disguise"));
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// AngrealInvDisguise defaultproperties.
	DurationType = EWoTDurationType::DT_Charge;
	bElementSpirit = true;
	bRare = true;
	bCombat = true;
	MinInitialCharges = 15;
	MaxInitialCharges = 20;
	MaxCharges = 60;
	MaxChargesInGroup = 20;
	MinChargesInGroup = 10;
	MaxChargeUsedInterval = 1.0f;
	MinChargeGroupInterval = 1.0f;
	InventoryGroup = 67;

	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateDG.ActivateDG"));
	DeactivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/DeActivateDG.DeActivateDG"));
}

APawn* AWoTAngrealInvDisguise::GetClosestPawn() const
{
	const AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return nullptr;
	}
	const FVector Loc = OwnerActor->GetActorLocation();
	APawn* Closest = nullptr;
	float Best = TNumericLimits<float>::Max();
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		APawn* P = *It;
		if (P == OwnerActor || !IsValid(P))
		{
			continue;
		}
		if (WoTUE1Names::ActorIsA(P, TEXT("NavigationProxyPawn"))
			|| WoTUE1Names::ActorIsA(P, TEXT("MashadarFocusPawn")))
		{
			continue;
		}
		const AWoTCharacter* Ch = Cast<AWoTCharacter>(P);
		if (Ch && !Ch->IsAlive())
		{
			continue;
		}
		const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(P);
		if (NPC && NPC->CreatureRowName == TEXT("MashadarFocusPawn"))
		{
			continue;
		}
		const float Dist = FVector::DistSquared(Loc, P->GetActorLocation());
		if (Dist < Best)
		{
			Best = Dist;
			Closest = P;
		}
	}
	return Closest;
}

void AWoTAngrealInvDisguise::CastArtifact()
{
	if (bCasting)
	{
		return;
	}
	APawn* LookLike = GetClosestPawn();
	if (!LookLike)
	{
		Failed();   // no one to mimic
		return;
	}
	AWoTCharacter* Self = Cast<AWoTCharacter>(GetOwner());
	// AcquireAppearance + CopyDisguisePawnProperties (uc:179-384): the visible morph.
	// Authority applies it and stamps the replicated driver that rebuilds the look on
	// every client. The won't-fit collision abort (uc:317-321) Fails the cast BEFORE
	// any state change — no ApparentTeam/ApparentClass stamp, no charge drain, no flash.
	if (Self && Self->HasAuthority())
	{
		if (!Self->ApplyDisguiseFrom(LookLike))
		{
			Failed();
			return;
		}
	}
	bCasting = true;
	DisguisedAs = LookLike;
	// Take on the mimicked pawn's team (CopyDisguisePawnProperties:364/373 — a player's
	// GetApparentTeam or an NPC's team). TeamOf yields exactly that: the NPC's
	// AssignedTeam, or another player's own apparent team.
	ApparentTeam = static_cast<int32>(AWoTNPCCharacter::TeamOf(LookLike));
	// Stamp the owner-only ApparentClass surface on the caster (AngrealInvDisguise.uc:346/364:
	// DestPlayer.ApparentClass = ...). Owner-local anim/movement/HUD read it; the visible
	// mesh/skin swap that fools others rides the replicated Disguise driver (above).
	if (Self)
	{
		Self->ApparentClass = LookLike->GetClass();
	}
	// AngrealInvDisguise.uc:119-123: the cast flash — SetColors(PlayerColor) rings
	// around the caster (bFadeIn=false; the port never fades). The AcquireAppearance
	// morph half stays P34.
	if (APawn* OwnerPawn = Cast<APawn>(GetOwner()))
	{
		if (UWorld* World = GetWorld())
		{
			const FName Color = WoTPlayerColorFX::Resolve(OwnerPawn, TEXT("Blue"));
			AWoTAppearEffect::SpawnColored(*World, OwnerPawn, Color, Color);
		}
	}
	// No ChargeTimer reset — retail never resets it (AngrealInvDisguise.uc:104-134):
	// it starts at 0 (first tick after the first cast drains a charge) and a re-cast
	// carries the previous session's remainder.
	Super::CastArtifact();   // ActivateSound; AcquireAppearance stays P34
	SetActorTickEnabled(true);
}

void AWoTAngrealInvDisguise::UnCastArtifact()
{
	if (!bCasting)
	{
		return;
	}
	bCasting = false;
	DisguisedAs = nullptr;
	ApparentTeam = -1;       // back to the real team (RestoreDisguisePawnProperties:450)
	if (AWoTCharacter* Self = Cast<AWoTCharacter>(GetOwner()))
	{
		// RevertAppearance (uc:197-200): restore the snapshot + clear the replicated
		// driver so every client reverts. Clears ApparentIcon (uc:452) via the driver.
		Self->RevertDisguise();
		Self->ApparentClass = nullptr;   // RestoreDisguisePawnProperties:450 clears it
	}
	Super::UnCastArtifact();   // DeactivateSound
	SetActorTickEnabled(false);
}

void AWoTAngrealInvDisguise::GoEmpty()
{
	UnCastArtifact();
	Super::GoEmpty();
}

void AWoTAngrealInvDisguise::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bCasting)
	{
		return;
	}
	// Suck up charges while held — count-DOWN from 0 (AngrealInvDisguise.uc:83-88),
	// so the first charge drains on the first tick after cast (cf. Levitate).
	ChargeTimer -= DeltaTime;
	while (bCasting && ChargeTimer < 0.0f && RoundsPerMinute > 0.0f)
	{
		ChargeTimer += (60.0f / RoundsPerMinute);
		UseCharge();
	}
}
