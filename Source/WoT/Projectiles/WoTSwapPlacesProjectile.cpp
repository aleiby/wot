#include "WoTSwapPlacesProjectile.h"
#include "../WoTMeshFX.h"
#include "../WoTAppearEffect.h"
#include "../WoTPlayerColorFX.h"
#include "../WoTRetailText.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTWeaveFX.h"
#include "../WoTParticleSprayer.h"

#include "../WoTBounceableDecoration.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTCharacter.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTDecoration.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTMover.h"
#include "../WoTPlayerController.h"
#include "../WoTRainCyd.h"
#include "../WoTUE1Collision.h"
#include "../WoTZoneVolume.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTIceLeech.h"
#include "../Inventory/WoTLeech.h"
#include "../Inventory/WoTReflector.h"
#include "../Inventory/WoTWhirlwindLeech.h"

#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"

namespace
{
// The original's LevelInfo keeps the default actor collision cylinder.
constexpr float LevelInfoCollisionSize = 22.0f;

// The port's level geometry: the runtime level actor, or an imported editor
// map's static mesh parts. A projectile that hits it has hit the original's
// level, whose actor is the static LevelInfo (GenericProjectile.uc:216-222).
bool IsLevelGeometry(const AActor& Actor)
{
	return WoTRainCydIsVisBlockingActor(&Actor) && !Actor.IsA<AWoTMover>();
}

// Where the original's LevelInfo stands. The runtime world projection records
// it; an imported editor map has no record, so the level actor's own origin
// stands in for it.
FVector WhereLevelInfoStands(const UWorld& World, const AActor& Level)
{
	const UWoTMapLightTable* Table = UWoTMeshLight::FindMapLightTable(&World);
	return Table && Table->bHasLevelInfoLocation ? Table->LevelInfoLocation
		: Level.GetActorLocation();
}

// A far end whose move always fails in the original because it is a static
// actor: the level, the blockers a map places, static decorations. Anything
// the engine cannot move is treated the same way.
bool CannotMove(const AActor& Actor)
{
	if (IsLevelGeometry(Actor) || !Actor.IsRootComponentMovable()) return true;
	if (const AWoTCollisionBlocker* Blocker = Cast<AWoTCollisionBlocker>(&Actor))
	{
		return Blocker->bStatic;
	}
	if (const AWoTDecoration* Decoration = Cast<AWoTDecoration>(&Actor))
	{
		return Decoration->bStatic;
	}
	return false;
}

// The actor's collision cylinder in the original's terms (CollisionRadius,
// CollisionHeight), read while the actor still collides.
void RetailCylinder(const AActor& Actor, float& Radius, float& Height)
{
	if (const UWoTFlatCylinderComponent* Cylinder =
		Cast<UWoTFlatCylinderComponent>(Actor.GetRootComponent()))
	{
		Radius = Cylinder->GetUnscaledCapsuleRadius();
		Height = Cylinder->GetUnscaledCapsuleHalfHeight();
		return;
	}
	Actor.GetSimpleCollisionCylinder(Radius, Height);
}

// The original's SetLocation for an actor that collides with the world: it has
// to fit the level at the spot, which may nudge it out of a wall or floor, and
// while it still collides with actors, no mover may be where it lands (the one
// actor that refuses the move of a player). Nothing moves; InOutSpot becomes
// where it would stand.
bool FindRetailMoveSpot(const AActor& Actor, const FVector& Extent, FVector& InOutSpot,
	bool bCheckActors, const AActor* Ignoring)
{
	UWorld* World = Actor.GetWorld();
	return World
		&& WoTUE1Collision::FindSpotLevelOnly(*World, Extent, InOutSpot, /*bAssumeFit*/ false)
		&& (!bCheckActors
			|| !WoTUE1Collision::FindEncroachedMoverAt(Actor, InOutSpot, Ignoring));
}

// The far ends the original fits to the level where the caster stood, because
// they collide with the world: pawns, and the decorations that can be knocked
// loose (BreakableDecoration.uc:125, BounceableDecoration.uc:187). A breakable
// one is wider than a pawn, so against the wall the caster stood by it is
// nudged out or fails to fit. Projectiles collide with the world too, but are
// small enough to fit wherever a pawn stood.
bool CollidesWithWorld(const AActor& Actor)
{
	return Actor.IsA<APawn>() || Actor.IsA<AWoTBreakableDecoration>()
		|| Actor.IsA<AWoTBounceableDecoration>();
}

// The original's movers, most of its decorations that are not static and a
// frozen pawn's ice are replicated actors, so its clients saw the far end of a
// swap where the server had put it. The port shows them the way each kind
// replicates: a mover holds the pose as part of its replicated state, a
// decoration that sleeps until it is moved wakes to send where it now stands,
// and the ice's place is part of its frozen pawn's replicated effect. A
// decoration the original never sends (Fish.uc:65) or sends only when it first
// appears (SeaweedA.uc:24-25) is moved on the server alone.
void ShowClientsWhereItStands(AActor& Moved)
{
	if (AWoTMover* Mover = Cast<AWoTMover>(&Moved))
	{
		Mover->HoldDisplacedPose();
	}
	else if (AWoTCollisionBlocker* Blocker = Cast<AWoTCollisionBlocker>(&Moved))
	{
		UWoTIceLeech::NoteIceBlockMoved(*Blocker);
	}
	if (Moved.GetIsReplicated())
	{
		Moved.ForceNetUpdate();
	}
}

// SwapPlacesEffect.uc:160 and :208-210: the far end does not collide while the
// two ends trade places, so the caster arriving where it stands touches nothing
// of it; it collides again once both have moved and turned.
class FScopedCollisionOff
{
public:
	explicit FScopedCollisionOff(AActor& InActor)
		: Actor(&InActor), bWasColliding(InActor.GetActorEnableCollision())
	{
		if (bWasColliding)
		{
			InActor.SetActorEnableCollision(false);
		}
	}
	~FScopedCollisionOff() { Restore(); }
	void Restore()
	{
		if (bWasColliding && Actor.IsValid())
		{
			Actor->SetActorEnableCollision(true);
		}
		bWasColliding = false;
	}

private:
	TWeakObjectPtr<AActor> Actor;
	bool bWasColliding = false;
};

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

	// A wall hit names the original's level, its static LevelInfo: the caster
	// is sent to where that actor stands, and it never moves.
	const bool bLevel = IsLevelGeometry(*Dest);
	const FVector SrcLoc = Source->GetActorLocation();
	const FVector DstLoc = bLevel ? WhereLevelInfoStands(*W, *Dest) : Dest->GetActorLocation();
	const FRotator SrcRot = Source->GetActorRotation();
	const FRotator DstRot = Dest->GetActorRotation();
	// Both ends' sizes are read now: an actor that stops colliding reports none.
	float SourceRadius = 0.0f, SourceHeight = 0.0f;
	float DestRadius = LevelInfoCollisionSize, DestHeight = LevelInfoCollisionSize;
	RetailCylinder(*Source, SourceRadius, SourceHeight);
	if (!bLevel)
	{
		RetailCylinder(*Dest, DestRadius, DestHeight);
	}
	const FVector SourceExtent = Source->GetSimpleCollisionCylinderExtent();
	// The far end fits the level as the original's cylinder (a decoration's
	// 29-unit radius, not the corners of the box around it).
	const FVector DestExtent(DestRadius, DestRadius, DestHeight);
	auto ReportFitFailure = [this, Source](const TCHAR* LocalizedKey)
	{
		if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Source->GetController());
			PC && FWoTRetailText::EnsureLoaded())
		{
			const FString* Suffix = FWoTRetailText::FindSectionValue(
				TEXT("SwapPlacesEffect"), LocalizedKey);
			if (Suffix && SourceAngreal)
			{
				// SwapPlacesEffect.uc:130/149/168/182: SourceAngreal.Title $ " " $
				// the purchaser-localized effect failure fragment.
				PC->ClientCenterMessage(SourceAngreal->Title + TEXT(" ") + *Suffix, true);
			}
		}
	};

	// SwapPlacesEffect.uc:120-138: a far end smaller than the caster first needs
	// the caster to clear every blocking actor where the far end stands (the far
	// end itself not counted).
	if ((DestHeight < SourceHeight || DestRadius < SourceRadius)
		&& !UWoTWhirlwindLeech::ActorFits(Source, DstLoc, ActorFitsRadius, Dest))
	{
		ReportFitFailure(TEXT("SourceFailMessage"));
		return;
	}
	// uc:140-157: a caster smaller than the far end needs the far end to clear
	// them where the caster stands. The caster stops colliding for that test and
	// stays so through its own move, which then checks no actors. The level's
	// LevelInfo blocks nothing, so it always clears them.
	const bool bSourceSmaller = SourceHeight < DestHeight || SourceRadius < DestRadius;
	if (bSourceSmaller && !bLevel
		&& !UWoTWhirlwindLeech::ActorFits(Dest, SrcLoc, ActorFitsRadius, Source))
	{
		ReportFitFailure(TEXT("DestinationFailMessage"));
		return;
	}

	// uc:164-176: the caster moves first, and only to a spot that fits the
	// level there (in solid space it fits nowhere).
	FVector SourceSpot = DstLoc;
	if (!FindRetailMoveSpot(*Source, SourceExtent, SourceSpot, !bSourceSmaller, Dest))
	{
		ReportFitFailure(TEXT("SourceFailMessage"));
		return;
	}
	// uc:178-191: then the far end moves to where the caster stood; when it
	// cannot, the caster is put back. A far end the original could never move
	// (the level, static actors) is known before anything moves, so the caster
	// simply stays.
	if (CannotMove(*Dest))
	{
		ReportFitFailure(TEXT("DestinationFailMessage"));
		return;
	}
	FScopedCollisionOff DestCollision(*Dest);
	if (!Source->SetActorLocation(SourceSpot, false, nullptr,
		ETeleportType::TeleportPhysics))
	{
		ReportFitFailure(TEXT("SourceFailMessage"));
		return;
	}
	UWoTZoneIndex::RefreshAfterRelocation(Source);
	// A far end that collides with the world has to fit the level where the
	// caster stood (uc:178), which may nudge it out of a wall the caster stood
	// against, or fail.
	FVector DestSpot = SrcLoc;
	const bool bDestMoved = IsValid(Dest)
		&& (!CollidesWithWorld(*Dest) || FindRetailMoveSpot(*Dest, DestExtent, DestSpot,
			/*bCheckActors*/ false, nullptr))
		&& Dest->SetActorLocation(DestSpot, false, nullptr, ETeleportType::TeleportPhysics);
	if (!bDestMoved)
	{
		if (Source->SetActorLocation(SrcLoc, false, nullptr,
			ETeleportType::TeleportPhysics))
		{
			UWoTZoneIndex::RefreshAfterRelocation(Source);
		}
		ReportFitFailure(TEXT("DestinationFailMessage"));
		return;
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
	// uc:208-210: the far end collides again. Its zones are taken now: with its
	// collision off it reports no size, and its feet would be read at its middle.
	DestCollision.Restore();
	UWoTZoneIndex::RefreshAfterRelocation(Dest);
	ShowClientsWhereItStands(*Dest);

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
