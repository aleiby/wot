#pragma once

#include "CoreMinimal.h"
#include "WoTTrap.h"
#include "../WoTTeleporter.h"
#include "../Inventory/WoTInventory.h"
#include "WoTPit.generated.h"

class AWoTPit;
class AWoTPitOpening;

// Port of WoTTraps/Pit{,Trap,Opening}.uc — the false-floor pit. Three actors: AWoTPit (the Trap
// manager + the visible false-floor mesh) spawns an AWoTPitTrap (the touch
// surface, an Engine.Teleporter leaf) and links it to a free pre-placed AWoTPitOpening (the shaft
// mouth elsewhere in the map). Falling in: touching the trap teleports the victim to the opening,
// preserving their horizontal offset from the trap center and their view rotation; the deep shaft
// below the opening + fall damage (AWoTCharacter::Landed) do the hurting. Climbing out: touching
// the opening (reached up the vine walls) teleports back to the trap the same way. The explicit
// linked pointers replace the .uc's URL/Tag fallback (URL stays empty, so the AWoTTeleporter
// random-destination Touch never runs). The citadel-editor placement surface is ported here;
// the single-player InitSinglePlayer path remains deferred.
UCLASS()
class AWoTPitTrap : public AWoTTeleporter
{
	GENERATED_BODY()

public:
	AWoTPitTrap();

	// PitTrap.TrapRadius — the placement footprint.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") int32 TrapRadius = 56;

	UPROPERTY() TObjectPtr<AWoTPitOpening> ThePitOpening;

	// PitTrap.Hide/Show — collision only (the pit's look is the AWoTPit mesh).
	void HideTrap();
	void ShowTrap();

	// PitTrap Waiting.Touch/Bump (Bump folds into the overlap): gate via the owning Pit's
	// IsInactive + the resting-Seal hold, then send the toucher DOWN — the opening accepts with
	// the offset preserved.
	virtual void NotifyActorBeginOverlap(AActor* Other) override;

	// PitTrap.Destroyed: unlink so the opening frees up for the next pit.
	virtual void Destroyed() override;
};

UCLASS()
class AWoTPitOpening : public AWoTTeleporter
{
	GENERATED_BODY()

public:
	AWoTPitOpening();

	UPROPERTY() TObjectPtr<AWoTPitTrap> ThePitTrap;

	// PitOpening.Touch: climbing out — no gates (any pawn escapes); the trap surface accepts
	// with the offset preserved.
	virtual void NotifyActorBeginOverlap(AActor* Other) override;
};

UCLASS()
class AWoTPit : public AWoTTrap
{
	GENERATED_BODY()

public:
	// The illusory false floor (Pit.uc:201-205: DT_Mesh 'WoTTraps.Pit' (meshmap scale baked; DrawScale 1.0),
	// STY_Modulated — every skin slot is rebound to the shared 2x framebuffer-modulate master).
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTVertexAnimComponent> VisualMesh;

	AWoTPit();

	UPROPERTY() TObjectPtr<AWoTPitTrap> ThePitTrap;

	// Pit.PreBeginPlay: spawn the trap surface and claim a PitOpening; if none is
	// free, remove the surface and notify the placing owner (Pit.uc:42-47).
	virtual void BeginPlay() override;
	virtual void Destroyed() override;

	// Pit.IsInactive: Seals never spring the pit (a seal can't be pitted).
	virtual bool IsInactive(const AActor* Other) const override;

	// Pit.uc editor placement surface (Slice 1e).
	virtual bool CalcLocation(FVector& HitLocation, FVector& HitNormal) override;
	virtual AActor* AnyActorsInArea(const FVector& Loc, float Radius) const override;
	virtual bool DeployResource(FVector HitLocation, FVector HitNormal) override;
	virtual void BeginEditingResource(uint8 PlacedByTeam) override;
	virtual void EndEditingResource() override;

protected:
	// The edit-phase collision split (see AWoTTrap::ApplyEditingCollision): the manager is
	// cursor-traceable while editing, collisionless at play (the false floor is illusory).
	virtual void ApplyEditingCollision(bool bEditingNow) override;
};

// Citadel-editor placement adapter (PitInventory.uc). Data-only leaf.
UCLASS()
class AWoTPitInventory : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTPitInventory();
};
