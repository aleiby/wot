#include "WoTSwapPlacesProjectile.h"
#include "../WoTMeshFX.h"
#include "../WoTAppearEffect.h"
#include "../WoTPlayerColorFX.h"
#include "../WoTRetailText.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTWeaveFX.h"
#include "../WoTParticleSprayer.h"

#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"
#include "../WoTZoneVolume.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTLeech.h"
#include "../Inventory/WoTReflector.h"

#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"

namespace
{
// Move an existing effect component (leech/reflector) between hosts WITHOUT
// destroying it — the original's UnAttach+AttachTo keeps the object; our
// Unattach() destroys, so we reparent instead so identity survives (a Lightning
// aura keeps tracking its moved leech). The caller re-runs AttachTo/Install to
// re-register + honor the new host's shields.
void ReparentComponent(UActorComponent* C, AActor* NewOwner)
{
	AActor* Old = C->GetOwner();
	if (C->IsRegistered())
	{
		C->UnregisterComponent();
	}
	if (Old)
	{
		Old->RemoveOwnedComponent(C);
	}
	C->Rename(nullptr, NewOwner,
	          REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
	NewOwner->AddOwnedComponent(C);
}
}

AWoTSwapPlacesProjectile::AWoTSwapPlacesProjectile()
{
	Sprite = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);
	// AngrealSwapPlacesProjectile defaultproperties (simulation subset).
	Speed = 750.0f;
	MaxSpeed = 1000.0f;
	Damage = 0.0f;         // no impact damage — the payload is the swap
	DamageRadius = 0.0f;   // no splash
	Collision->InitSphereRadius(6.0f);   // CollisionRadius=6

	// Sound'Angreal.SwapPlaces.*' — bound by the sound pass (soft, safe if absent).
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchSP.LaunchSP"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitSP.HitSP"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LoopSP.LoopSP"));
	SoundRadius = 160;             // AmbientSound (LoopSP)
	SoundVolume = 100;
}

void AWoTSwapPlacesProjectile::SetSourceAngreal(AWoTAngrealInventory* Source)
{
	Super::SetSourceAngreal(Source);
	// Store the firing pawn where a later reflection (which rewrites SourcePawn)
	// can't clobber it.
	if (!InitiatingPawn)
	{
		InitiatingPawn = GetInstigator();
	}
}

void AWoTSwapPlacesProjectile::Explode(FVector HitLocation, FVector HitNormal)
{
	if (HasAuthority())
	{
		// Prefer the locked destination; fall back to whatever we struck.
		AActor* Target = Destination ? Destination.Get() : HitActor.Get();
		AWoTCharacter* TargetCharacter = Cast<AWoTCharacter>(Target);
		if (!TargetCharacter || !TargetCharacter->IsEffectBlocked(
			SourceAngreal, /*bEffectDeleterious*/ true, /*bEmitFeedback*/ true))
		{
			PerformSwap(InitiatingPawn, Target);
		}
	}
	Super::Explode(HitLocation, HitNormal);   // impact sound + destroy (no splash)
}

bool AWoTSwapPlacesProjectile::ActorFitsAt(AActor* A, const FVector& Loc, AActor* Ignore) const
{
	const ACharacter* Ch = Cast<ACharacter>(A);
	if (!Ch)
	{
		return true;   // only capsule pawns get a fit test
	}
	const UCapsuleComponent* Cap = Ch->GetCapsuleComponent();
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(
		Cap->GetScaledCapsuleRadius(), Cap->GetScaledCapsuleHalfHeight());
	FCollisionObjectQueryParams ObjTypes;
	ObjTypes.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjTypes.AddObjectTypesToQuery(ECC_WorldDynamic);
	ObjTypes.AddObjectTypesToQuery(ECC_Pawn);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SwapFits), false, A);
	Params.AddIgnoredActor(Ignore);
	return !GetWorld()->OverlapAnyTestByObjectType(Loc, FQuat::Identity, ObjTypes, Shape, Params);
}

void AWoTSwapPlacesProjectile::RedirectSeeker(AWoTSeekingProjectile* Seeker,
                                              AActor* NewDest, APawn* NewOwner)
{
	Seeker->SetDestination(NewDest);
	Seeker->SetInstigator(NewOwner);
	Seeker->SourcePawn = NewOwner;
	if (!Seeker->bHurtsOwner)
	{
		Seeker->SetIgnoredPawn(NewOwner);
	}
}

void AWoTSwapPlacesProjectile::PerformSwap(APawn* Source, AActor* Dest)
{
	// Don't swap with yourself; both ends must exist.
	if (!IsValid(Source) || !IsValid(Dest) || Source == Dest)
	{
		return;
	}
	UWorld* W = GetWorld();
	if (!W)
	{
		return;
	}

	const FVector SrcLoc = Source->GetActorLocation();
	const FVector DstLoc = Dest->GetActorLocation();
	const FRotator SrcRot = Source->GetActorRotation();
	const FRotator DstRot = Dest->GetActorRotation();
	auto ReportFitFailure = [this, Source](const TCHAR* LocalizedKey)
	{
		if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Source->GetController());
			PC && FWoTRetailText::EnsureLoaded())
		{
			const FString* Suffix = FWoTRetailText::FindSectionValue(
				TEXT("SwapPlacesEffect"), LocalizedKey);
			if (Suffix && SourceAngreal)
			{
				// SwapPlacesEffect.uc:130/149: SourceAngreal.Title $ " " $
				// the purchaser-localized effect failure fragment.
				PC->ClientCenterMessage(SourceAngreal->Title + TEXT(" ") + *Suffix, true);
			}
		}
	};

	// Telefrag safety (verbatim): only test a fit when the pawns differ in size —
	// the larger must clear where the smaller stood, else the swap is cancelled.
	// Inert while every pawn shares one capsule; correct once NPC sizes vary.
	const ACharacter* SrcCh = Cast<ACharacter>(Source);
	const ACharacter* DstCh = Cast<ACharacter>(Dest);
	if (SrcCh && DstCh)
	{
		const float SrcH = SrcCh->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const float SrcR = SrcCh->GetCapsuleComponent()->GetScaledCapsuleRadius();
		const float DstH = DstCh->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const float DstR = DstCh->GetCapsuleComponent()->GetScaledCapsuleRadius();
		if ((DstH < SrcH || DstR < SrcR) && !ActorFitsAt(Source, DstLoc, Dest))
		{
			ReportFitFailure(TEXT("SourceFailMessage"));
			return;
		}
		if ((SrcH < DstH || SrcR < DstR) && !ActorFitsAt(Dest, SrcLoc, Source))
		{
			ReportFitFailure(TEXT("DestinationFailMessage"));
			return;
		}
	}

	// Swap positions — teleport, straight through anything between them.
	if (Source->SetActorLocation(DstLoc, false, nullptr,
		ETeleportType::TeleportPhysics))
	{
		UWoTZoneIndex::RefreshAfterRelocation(Source);
	}
	if (Dest->SetActorLocation(SrcLoc, false, nullptr,
		ETeleportType::TeleportPhysics))
	{
		UWoTZoneIndex::RefreshAfterRelocation(Dest);
	}

	// Swap facing (actor + control/view rotation).
	Source->SetActorRotation(DstRot);
	if (AController* C = Source->GetController())
	{
		C->SetControlRotation(DstRot);
	}
	Dest->SetActorRotation(SrcRot);
	if (APawn* DestPawn = Cast<APawn>(Dest))
	{
		if (AController* C = DestPawn->GetController())
		{
			C->SetControlRotation(SrcRot);
		}
	}

	// Swap seeking weaves: those after the target now chase the caster and vice
	// versa (two-phase so a just-redirected seeker isn't caught again). Never
	// touch our own projectile.
	APawn* DestPawn = Cast<APawn>(Dest);
	TArray<AWoTSeekingProjectile*> ToSource;
	for (TActorIterator<AWoTSeekingProjectile> It(W); It; ++It)
	{
		AWoTSeekingProjectile* Seeker = *It;
		if (Seeker == this || !IsValid(Seeker))
		{
			continue;
		}
		if (Seeker->Destination == Dest)
		{
			ToSource.Add(Seeker);   // -> retarget the caster (phase 2)
		}
		else if (Seeker->Destination == Source)
		{
			RedirectSeeker(Seeker, Dest, Source);   // now chases the target
		}
	}
	for (AWoTSeekingProjectile* Seeker : ToSource)
	{
		RedirectSeeker(Seeker, Source, DestPawn);   // now chases the caster
	}

	// Exchange harmful removable effects that did not arrive by projectile, plus
	// reflectors, between the two pawns (WOTUtil.uc:117-123). If the opposite pawn
	// is sustaining one of those effects, stop that cast rather than move it
	// (AngrealInvSwapPlaces.uc:43).
	// Retail DETACHES both sides fully before re-attaching either, so re-attach's
	// bSingular/bRemoveExisting can't destroy an un-moved opposite-side effect
	// mid-swap; we mirror that with a detach-all (reparent) then attach-all pass.
	if (APawn* DestPawnEff = Cast<APawn>(Dest))
	{
		auto IsBad = [](const auto* E)
		{
			return IsValid(E) && E->bRemovable && E->bDeleterious && !E->bFromProjectile;
		};

		// --- leeches (SwapPlacesEffect.uc:255-322) --- the instigator "stop
		// casting" case is on the DESTINATION leech loop.
		TArray<UWoTLeech*> SrcAll, DstAll, SrcMove, DstMove;
		Source->GetComponents<UWoTLeech>(SrcAll);
		DestPawnEff->GetComponents<UWoTLeech>(DstAll);
		for (UWoTLeech* L : SrcAll) { if (IsBad(L)) { SrcMove.Add(L); } }
		for (UWoTLeech* L : DstAll)
		{
			if (!IsBad(L)) { continue; }
			if (L->LeechInstigator == Dest && L->SourceAngreal)
			{
				L->SourceAngreal->UnCastArtifact();   // make them stop casting
			}
			else { DstMove.Add(L); }
		}
		// Detach all (retail L.UnAttach() first, SwapPlacesEffect.uc:294):
		// OnHostDetached restores host state the leech changed (Whirlwind re-enables
		// the old host's movement, WhirlwindLeech.uc:83-97) without destroying it.
		for (UWoTLeech* L : SrcMove)
		{
			L->RemoveStatusIconFromCurrentHost();
			L->OnHostDetached(Source);
			ReparentComponent(L, Dest);
		}
		for (UWoTLeech* L : DstMove)
		{
			L->RemoveStatusIconFromCurrentHost();
			L->OnHostDetached(DestPawnEff);
			ReparentComponent(L, Source);
		}
		for (UWoTLeech* L : SrcMove)                                      // re-attach all
		{
			L->bTransferring = true; L->AttachTo(DestPawnEff); L->bTransferring = false;
		}
		for (UWoTLeech* L : DstMove)
		{
			L->bTransferring = true; L->AttachTo(Source); L->bTransferring = false;
		}

		// --- reflectors (SwapPlacesEffect.uc:324-394) --- NOTE: the instigator
		// "stop casting" case is on the SOURCE reflector loop here (the opposite
		// side from the leech section above — verbatim with the .uc).
		TArray<UWoTReflector*> SrcR, DstR, SrcRMove, DstRMove;
		Source->GetComponents<UWoTReflector>(SrcR);
		DestPawnEff->GetComponents<UWoTReflector>(DstR);
		for (UWoTReflector* R : SrcR)
		{
			if (!IsBad(R)) { continue; }
			if (R->ReflectorInstigator == Dest && R->SourceAngreal)
			{
				R->SourceAngreal->UnCastArtifact();
			}
			else { SrcRMove.Add(R); }
		}
		for (UWoTReflector* R : DstR) { if (IsBad(R)) { DstRMove.Add(R); } }
		for (UWoTReflector* R : SrcRMove)
		{
			R->RemoveStatusIconFromCurrentHost();
			ReparentComponent(R, Dest);
		}
		for (UWoTReflector* R : DstRMove)
		{
			R->RemoveStatusIconFromCurrentHost();
			ReparentComponent(R, Source);
		}
		for (UWoTReflector* R : SrcRMove)
		{
			R->bTransferring = true; R->Install(DestPawnEff); R->bTransferring = false;
		}
		for (UWoTReflector* R : DstRMove)
		{
			R->bTransferring = true; R->Install(Source); R->bTransferring = false;
		}
	}

	// SwapPlacesEffect.uc:396-404: the target now stands where the caster was — if that spot
	// is a hazard, credit the caster with the kill if the target dies soon.
	if (AWoTCharacter* DestChar = Cast<AWoTCharacter>(Dest))
	{
		DestChar->SetSuicideInstigator(Source, SourceAngreal);
	}

	// SwapPlacesEffect.uc:408-434: an AppearEffect on EACH swapped pawn, colored
	// by the retail four-case rule — both players: each their own PlayerColor;
	// one player: BOTH take that player's; neither: Blue source / Green dest.
	// (bFadeIn=false is the retail SP fade opt-out; the port never fades.)
	LastSwapFlashCount = 0;
	if (HasAuthority())
	{
		const bool bSourcePlayer = Source->GetPlayerState() != nullptr
			|| Source->IsPlayerControlled();
		const bool bDestPlayer = DestPawn && (DestPawn->GetPlayerState() != nullptr
			|| DestPawn->IsPlayerControlled());
		FName SourceColor, DestColor;
		if (bSourcePlayer && bDestPlayer)
		{
			SourceColor = WoTPlayerColorFX::Resolve(Source, TEXT("Blue"));
			DestColor = WoTPlayerColorFX::Resolve(DestPawn, TEXT("Blue"));
		}
		else if (bSourcePlayer)
		{
			SourceColor = DestColor = WoTPlayerColorFX::Resolve(Source, TEXT("Blue"));
		}
		else if (bDestPlayer)
		{
			SourceColor = DestColor = WoTPlayerColorFX::Resolve(DestPawn, TEXT("Blue"));
		}
		else
		{
			SourceColor = TEXT("Blue");
			DestColor = TEXT("Green");
		}
		LastSwapFlashCount += AWoTAppearEffect::SpawnColored(*W, Source,
			SourceColor, SourceColor) ? 1 : 0;
		LastSwapFlashCount += AWoTAppearEffect::SpawnColored(*W, Dest,
			DestColor, DestColor) ? 1 : 0;
	}
}

void AWoTSwapPlacesProjectile::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() != NM_DedicatedServer)
	{
		WoTMeshFX::ConfigureClassSprite(*Sprite,
			TEXT("AngrealSwapPlacesProjectile"), 30.0f);
	}
	Trail = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("Firework04"),
		GetActorLocation(), GetActorRotation(), this, true);
	if (Trail)
	{
		// AngrealSwapPlacesProjectile.uc:78-80 overrides only the spawned
		// Firework04 visual's radius/effect. Firework04 already registered its
		// authored 255/100/60 bytes; update the live radius component in place.
		Trail->Light->SetAttenuationRadius((6.0f + 1.0f) * 25.0f);
	}
}

void AWoTSwapPlacesProjectile::Destroyed()
{
	WoTWeaveFX::FadeOut(Trail, 5.0f);
	Super::Destroyed();
}
