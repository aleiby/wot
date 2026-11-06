#include "WoTMyrddraalSwordAngreal.h"

AWoTMyrddraalSwordAngreal::AWoTMyrddraalSwordAngreal()
{
	// MyrddraalSwordAngreal defaultproperties (MyrddraalSwordAngreal.uc:18-30):
	// a Spirit-element source carrier, hidden, non-collide, never a pickup goal.
	bElementSpirit = true;                // element identity for the leech
	// StatusIconFrame=Texture'WoTPawns.UI.M_SMyrddraal' — the lingering-damage HUD
	// icon carried by the spawned leech.
	StatusIconFrame = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		TEXT("/Game/WoT/Textures/WoTPawns/M_SMyrddraal.M_SMyrddraal")));
	// bHidden / bCanTeleport=False / bIsItemGoal=False / bCollideActors=False —
	// it exists only as a data object, never interacts with the world.
	SetHidden(true);
	SetActorEnableCollision(false);
	// No appear rings on either netmode; it is never a visible pickup.
	bSPAppearEffect = false;
	bMPAppearEffect = false;
}
