#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTMyrddraalSwordAngreal.generated.h"

// Port of WoTPawns.MyrddraalSwordAngreal (expands AngrealInventory) — the hidden
// non-pickup source artifact the Myrddraal stamps onto its melee-hit health-drain
// leech, giving that leech its element identity (Spirit) for IgnoresEffect /
// reflector matching and its HUD status icon (M_SMyrddraal). Spec:
// exports/script/WoTPawns/MyrddraalSwordAngreal.uc. Never a world pickup: retail
// stubs BecomePickup() empty and flags bHidden / bCanTeleport=False / !bCollide.
// It is spawned once per Myrddraal (PreBeginPlay, Myrddraal.uc:83) as a bare
// SourceAngreal carried by no one — the AWoTAngrealInventory::CreateProxy shape.
UCLASS()
class AWoTMyrddraalSwordAngreal : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTMyrddraalSwordAngreal();

	// "never want this visible, pickupable" (MyrddraalSwordAngreal.uc:11-13):
	// the source artifact never grows appear-effect rings.
	virtual void BecomePickup() override {}
};
