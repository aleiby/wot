#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTResourceSpawnerInv.generated.h"

class AWoTNPCCharacter;

// Port of Angreal.ResourceSpawner (abstract) — the summon ter'angreal base: casting
// it puts a creature helper (Minion / Guardian / Champion) in front of the caster,
// one at a time, on the caster's team. Spec: exports/script/Angreal/ResourceSpawner.uc.
//
// The original's GetResourceClass() (a class<WOTPawn> troop lookup) becomes
// GetResourceRow() returning a creature-registry ROW NAME, because the port models
// the 12 creatures as data rows (WoTCreatureData), not pawn subclasses — every summon
// is one AWoTNPCCharacter configured from its row. Spawn goes through
// WoTCreatureData::SpawnCreature; the caster's faction (which troop table to read)
// comes from its actual faction class identity through AWoTCharacter::GetSummonFaction.
UCLASS(Abstract)
class AWoTResourceSpawnerInv : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTResourceSpawnerInv();

	// The Minion/Guardian/Champion currently helping the caster (ResourceSpawner.uc:16).
	// Weak: the summon is a world actor that can die/despawn independently of the artifact.
	UPROPERTY() TWeakObjectPtr<AWoTNPCCharacter> CurrentArenaHelper;

	// "You already have one <helper> under your control." (ResourceSpawner.uc:19).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal") FString LimitWarning;

	// Separation between the caster and the summon (ResourceSpawner.uc:22, CDO 50).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal") float SpawnSeparation = 50.0f;

	// The summon AppearEffect sparkle pair (ResourceSpawner.uc:24, AE at uc:58-61);
	// each concrete spawner authors its corona textures (AngrealInv{Minion,
	// Guardian,Champion}.uc defaultproperties).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal")
	FName TopSparkle = TEXT("CyanCorona");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal")
	FName BottomSparkle = TEXT("PurpleCorona");

	// Cast(): spawn a helper if we don't already have one (ResourceSpawner.uc:34).
	virtual void CastArtifact() override;

	// The troop tier this spawner summons ('Grunt'/'Captain'/'Champion') — the subclass sets it.
	virtual FName GetTierKeyword() const { return NAME_None; }

	// GetResourceClass(): the creature row for the caster's faction + this tier
	// (WOTPlayer.GetTroop). NAME_None => nothing to summon (subclass may also veto).
	virtual FName GetResourceRow() const;

	// HaveHelper(): a live, same-team helper of this exact type already ours
	// (ResourceSpawner.uc:76). The one-at-a-time gate.
	bool HaveHelper() const;
};
