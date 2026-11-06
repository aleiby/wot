#pragma once

#include "CoreMinimal.h"
#include "WoTSeekingProjectile.h"
#include "WoTSwapPlacesProjectile.generated.h"

// Port of Angreal.AngrealSwapPlacesProjectile (expands SeekingProjectile) — the
// Swap Places ter'angreal's weave: seeks the caster's target and, on reaching it,
// switches the two pawns' positions. Spec:
// exports/script/Angreal/AngrealSwapPlacesProjectile.uc + SwapPlacesEffect.uc.
//
// The SwapPlacesEffect is folded into the projectile's Explode (the effects
// framework isn't ported — same fold as Air Pulse / Balefire). On impact it swaps
// InitiatingPawn (the caster) and the target: locations (teleport, through
// everything), rotations + view/control rotation, and redirects every seeking
// weave — those tracking the caster now track the target and vice versa, with
// instigator/ignored-pawn ownership exchanged (AngrealInvSwapPlaces.uc:43).
// Telefrag-safe: when the pawns differ in size it first
// checks the larger fits where the smaller stood (ActorFits), else it bails
// (inert while pawns are uniform-sized).
//
// InitiatingPawn is captured once (SetSourceAngreal) and never overwritten, so a
// reflection may replace SourcePawn without losing the original caster.
//
// PerformSwap also trades harmful removable non-projectile leeches and reflectors
// between the pawns (WOTUtil.uc:117-123), including the case where the other pawn
// is sustaining an effect and must stop casting. Effects are REPARENTED (not
// Unattach+re-AttachTo, which our Unattach() would destroy), so a live aura keeps
// tracking its moved leech.
//
// Deferred (presentation): the AppearEffect swap flashes, the CenterMessage
// fail text, and SetSuicideInstigator. The ParticleSprayer trail and decoded
// DT_Sprite billboard are live.
UCLASS()
class AWoTSwapPlacesProjectile : public AWoTSeekingProjectile
{
	GENERATED_BODY()

public:

	// The decoded DT_Sprite animation.
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTSpriteBillboardComponent> Sprite;
	virtual void BeginPlay() override;
	virtual void Destroyed() override;
	AWoTSwapPlacesProjectile();
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> Trail;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Swap") int32 LastSwapFlashCount = 0;

	// The pawn that fired us — preserved across reflection (SourcePawn is not).
	UPROPERTY() TObjectPtr<APawn> InitiatingPawn;

	virtual void SetSourceAngreal(AWoTAngrealInventory* Source) override;
	virtual void Explode(FVector HitLocation, FVector HitNormal) override;

	// Swap Source (caster) and Dest (target): position, facing, and seeker locks.
	void PerformSwap(APawn* Source, AActor* Dest);

private:
	// Retarget a seeker onto NewDest and hand its ownership to NewOwner.
	void RedirectSeeker(AWoTSeekingProjectile* Seeker, AActor* NewDest, APawn* NewOwner);
	// UE1 Util.ActorFits: A's capsule clears Loc (ignoring the two swap pawns).
	bool ActorFitsAt(AActor* A, const FVector& Loc, AActor* Ignore) const;
};
