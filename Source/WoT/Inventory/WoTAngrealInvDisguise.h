#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvDisguise.generated.h"

// Port of Angreal.AngrealInvDisguise (expands AngrealInventory): while active,
// the Disguise ter'angreal mimics the closest creature or person
// (AngrealInvDisguise.uc:7-8).
//
// SIM subset: hold-to-cast (press->CastArtifact, release->UnCastArtifact) draining
// charges over time; on cast it picks the nearest living pawn (GetClosestPawn) and
// records it as the disguise target, failing if there's no one to mimic; GoEmpty
// drops it at zero charges.
//
// AcquireAppearance/RevertAppearance now land the visible morph (P34): the cast
// calls AWoTCharacter::ApplyDisguiseFrom (mesh/skins/drawscale/style/capsule/eye/
// movement-speed copy off the LIVE target, replicated to every client) and the
// uncast/GoEmpty call RevertDisguise. The ApparentTeam AI-visibility half and the
// owner-only ApparentClass surface were already done. Still deferred: the SP-only
// physics copy (uc:340-353, NM_Standalone-gated — out of MP scope).
UCLASS()
class AWoTAngrealInvDisguise : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvDisguise();

	// The pawn we're currently mimicking (null = not disguised).
	UPROPERTY() TObjectPtr<APawn> DisguisedAs;

	// WOTPlayer.ApparentTeam / GetApparentTeam (WOTPlayer.uc:1912): the team the
	// disguising player presents to faction checks while mimicking someone, or -1
	// ("not disguised") for their real team. The AI-visibility half of the disguise
	// (the rest — mesh/skin/collision copy — is still deferred presentation): set on
	// cast to the mimicked pawn's team, cleared on uncast. AWoTNPCCharacter::TeamOf
	// reads it so an NPC treats a disguised player as its apparent team.
	int32 GetApparentTeam() const { return ApparentTeam; }

	virtual void CastArtifact() override;    // Cast(): take the nearest pawn's form
	virtual void UnCastArtifact() override;  // UnCast(): revert
	virtual void GoEmpty() override;         // out of charges -> revert
	virtual void Tick(float DeltaTime) override;

private:
	APawn* GetClosestPawn() const;

	bool bCasting = false;
	float ChargeTimer = 0.0f;
	// -1 = not disguised (WOTPlayer.ApparentTeam == -1 sentinel, WOTPlayer.uc:1914).
	int32 ApparentTeam = -1;
};
