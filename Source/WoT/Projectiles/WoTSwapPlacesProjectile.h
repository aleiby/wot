#pragma once

#include "CoreMinimal.h"
#include "WoTSeekingProjectile.h"
#include "WoTSwapPlacesProjectile.generated.h"

// Port of Angreal.AngrealSwapPlacesProjectile (expands SeekingProjectile), the
// swap artifact's projectile: it seeks the caster's target and, on reaching it,
// switches the caster and the target. Spec:
// exports/script/Angreal/AngrealSwapPlacesProjectile.uc + SwapPlacesEffect.uc.
//
// The SwapPlacesEffect is folded into the projectile's Explode (the effects
// framework isn't ported). On impact it moves
// InitiatingPawn (the caster) to the target and the target to where the caster
// stood, both or neither (SwapPlacesEffect.uc:120-191):
// - when the two differ in size, the larger must clear the blocking actors
//   where the smaller stands (Util.ActorFits);
// - the caster moves only to a spot that fits the level there;
// - the target then moves where the caster stood, fitted to the level there
//   when it collides with the world (a pawn, or a decoration that can be
//   knocked loose); if it cannot, the caster is put back. A target the original
//   could not move (its level, which a wall hit names, map-placed blockers,
//   static decorations) never moves.
// A refused swap tells a player caster which end did not fit and does nothing
// else. A completed one swaps rotations and view rotation, redirects every
// seeking projectile (those tracking the caster now track the target and vice
// versa, with instigator and ignored pawn exchanged), trades harmful lasting
// effects, credits the caster with the target's suicide, and flashes both ends.
// Clients see a mover, decoration or ice it moved where the server put it.
//
// InitiatingPawn is captured once (SetSourceAngreal) and never overwritten, so a
// reflection may replace SourcePawn without losing the original caster.
//
// PerformSwap trades harmful removable non-projectile leeches and reflectors
// between the pawns (WOTUtil.uc:117-123), including the case where the other pawn
// is sustaining an effect and must stop casting. Effects are REPARENTED (not
// Unattach+re-AttachTo, which our Unattach() would destroy), so a live aura keeps
// tracking its moved leech.
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
	// SwapPlacesEffect.ActorFitsRadius (SwapPlacesEffect.uc:441): the search
	// radius of the blocking-actor fit tests.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Swap") float ActorFitsRadius = 1024.0f;

	// The pawn that fired us — preserved across reflection (SourcePawn is not).
	UPROPERTY() TObjectPtr<APawn> InitiatingPawn;

	virtual void SetSourceAngreal(AWoTAngrealInventory* Source) override;
	virtual void Explode(FVector HitLocation, FVector HitNormal) override;

	// Swap Source (caster) and Dest (target): position, facing, seeker locks and
	// lasting effects, or nothing at all when either end cannot move.
	void PerformSwap(APawn* Source, AActor* Dest);

private:
	// Retarget a seeker onto NewDest and hand its ownership to NewOwner.
	void RedirectSeeker(AWoTSeekingProjectile* Seeker, AActor* NewDest, APawn* NewOwner);
};
