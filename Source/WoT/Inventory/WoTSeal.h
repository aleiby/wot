#pragma once

#include "CoreMinimal.h"
#include "WoTInventory.h"
#include "WoTSeal.generated.h"

class AController;
class AWoTBattleInfo;
class AWoTParticleSprayer;

// Port of Seal (exports/script/WOT/Seal.uc): the carriable citadel objective. The win-economy
// half (team seal-count credit/debit through BattleInfo) landed with citadel step 4; the G13
// wave adds the carry loop — the possession-change/capture notifications, the per-machine
// carry-indicator sprayer (CarrySealSprayer at the retail PrePivot Z=75; its Nimbus child
// ships Particles(0)=None — an invisible emitter, binary-CDO verified, not ported), the
// replicated controlling-faction skin/icon swap, DropFrom's closest-pathnode fallback (UE5
// NavMesh projection; no nav system keeps the location and warns, the retail warn path),
// and the pawn Trigger fan-out (Util.TriggerAllInstances →
// every GuardSeal captain re-plans; Grunt.Trigger ignores seals). DropFrom→Landed uses
// the retail flat-cylinder blocking window, including moving-out initial-overlap semantics.
// G34 lands the editor half: full retail node-stamp placement validation, collision-off
// ground presentation, replicated editing glow, preview isolation, and edit-end credit.
UCLASS()
class AWoTSeal : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTSeal();

	// Seal.Team: the team that placed the seal. Seal.ControllingTeam: the team currently
	// credited with the capture (255 = none). Both default 255 (Seal.uc:38-39,428-429).
	UPROPERTY(BlueprintReadOnly, Category = "WoT|Seal") uint8 Team = 255;
	UPROPERTY(BlueprintReadOnly, Category = "WoT|Seal") uint8 ControllingTeam = 255;

	// The crediting player's faction (Seal.uc:88-110 switch on PlayerClass.Name):
	// drives the per-machine skin/icon swap. NAME_None until first credited.
	UPROPERTY(ReplicatedUsing = OnRep_ControllingFactionName, BlueprintReadOnly,
	          Category = "WoT|Seal")
	FName ControllingFactionName;

	// The carrier's resolved indicator color, stamped by the AUTHORITY at GiveTo
	// (retail SetColor ran server-side and the sprayer actor replicated). Proxies
	// must not re-derive it from the pawn — Owner, PlayerState, and
	// AppearanceClass replicate independently, so a proxy-side Resolve can race
	// to the Green fallback (retro-review). NAME_None = not carried.
	UPROPERTY(ReplicatedUsing = OnRep_CarryColorName, BlueprintReadOnly,
	          Category = "WoT|Seal")
	FName CarryColorName;

	// True while banked on an altar (RestOnAltar -> hand-back/GiveTo): the
	// Pickup-state presentation must NEVER enable the touch cylinder — retail
	// left collision off through placement, and an enabled overlap update at
	// EITHER the stale pre-move spot or the altar re-grants the seal to a
	// standing pawn mid-placement (retro-review; replicated so proxy
	// presentation skips the enable too).
	UPROPERTY(ReplicatedUsing = OnRep_PlacementState, BlueprintReadOnly,
		Category = "WoT|Seal")
	bool bRestingOnAltar = false;

	// Ground-deployed seals use the same frozen, visible, collision-off Pickup
	// presentation as altar seals and are therefore untraceable/uncapturable.
	UPROPERTY(ReplicatedUsing = OnRep_PlacementState, BlueprintReadOnly,
		Category = "WoT|Seal")
	bool bGroundPlaced = false;

	// Owner-local TempResource ghost. Never replicated and excluded from every
	// editing/crediting gameplay sweep.
	UPROPERTY(Transient)
	bool bPreviewSeal = false;

	// Seal.uc:129-138 editing cue. The glow is derived from the replicated
	// editing flag on every machine, matching AWoTTrap.
	UPROPERTY(ReplicatedUsing = OnRep_EditingGlow)
	bool bEditingGlow = false;

	// Seal.uc:44-45 (const editconst): the pawn fan-out event names.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seal")
	FName DestroyedEvent = TEXT("SealDestroyed");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seal")
	FName OwnershipChangedEvent = TEXT("SealOwnershipChanged");

	// Seal.uc:47-48: the editor-deploy pathnode gates (G15 consumes them; DropFrom's
	// fallback uses the NavMesh projection).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seal") float PathNodeDeltaH = 8.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seal") float PathNodeDeltaR = 512.0f;

	// Seal.uc:42 (localized): loaded from the retail WoT.int [Seal] (FWoTRetailText).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seal")
	FString PossesionChangeStr;

	// Seal.SetControllingTeam (Seal.uc:71-113): credit the placer's team — debit the
	// previously-credited team (if any) and credit the new one via the Battle mode's BattleInfo
	// NumSeals — and adopt the placer's faction skin/icon identity. No-op if the team is
	// unchanged, or off a Battle game. Player is the banking player's controller.
	void SetControllingTeam(AController* Player);
	// Seal.ClearControllingTeam (Seal.uc:55-69): debit the credited team, reset to none.
	void ClearControllingTeam();

	// Seal.Begin/EndEditingResource (Seal.uc:129-138): stamp the placing team and
	// drive AmbientGlow 64/0 through the runtime mesh-light bake.
	void BeginEditingResource(int32 PlacedByTeam);
	void EndEditingResource();

	// Seal.DeployResource / AnyActorsInArea (Seal.uc:209-300).
	virtual bool DeployResource(const FVector& StartLocation,
		const FVector& StartNormal);
	AActor* AnyActorsInArea(const FVector& SearchLocation,
		float SearchDistance) const;

	// Seal.GiveTo (Seal.uc:366-390): retire any indicator, Super, then on success the
	// cross-team take notification + a fresh carry indicator in the carrier's color.
	virtual void GiveTo(APawn* Other) override;
	// Seal.DropFrom (Seal.uc:336-349): blocked drop spots fall back to the closest
	// pathnode location; drops fan the ownership-changed pawn event out.
	virtual void DropFrom(FVector StartLocation) override;
	virtual void Landed(const FVector& HitNormal) override;
	// Seal.BecomePickup (Seal.uc:403-408): a grounded seal retires the indicator.
	virtual void BecomePickup() override;
	virtual void BecomeItem() override;
	// Seal.Destroyed (Seal.uc:327-334): indicator + team credit + the destroyed fan-out.
	virtual void Destroyed() override;

	// Seal.PickupFunction (Seal.uc:360-364): every successful pickup fans the
	// ownership-changed pawn event (AWoTInventory::GiveTo is the port's completion point).
	// Seal.SendTakeNotification (Seal.uc:410-422): possession-change sound + center message
	// to every player on the losing team.
	void SendTakeNotification(uint8 NotifyTeam);

	// Util.TriggerAllInstances(Self, class'Pawn', Instigator, EventName): the pawn
	// fan-out — every GuardSeal captain re-plans (Captain.uc:108). Public for the altar.
	void NotifyPawnsSealEvent(FName SealEventName, APawn* EventInstigator);

	// SealAltar.PlaceSeal's on-altar half (SealAltar.uc:118-124): visible, no spin
	// (state Pickup at the Dropped label), frozen at the banked spot, collision OFF
	// (the altar is the touch surface), position replicated (retail RemoteRole
	// DumbProxy), altar-owned, never respawning.
	void RestOnAltar(const FVector& Spot, const FRotator& Rot, AActor* Altar);

	AWoTParticleSprayer* GetIndicatorForTest() const { return Indicator; }
#if WITH_AUTOMATION_TESTS
	void ApplyControllingFactionForTesting() { ApplyControllingFaction(); }
#endif

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual bool TraceFallingMovement(const FVector& Start, const FVector& End,
		FHitResult& OutHit) const override;
	UFUNCTION()
	void OnRep_ControllingFactionName();
	UFUNCTION()
	void OnRep_CarryColorName();
	UFUNCTION()
	void OnRep_PlacementState();
	UFUNCTION()
	void OnRep_EditingGlow();
	// Owner vs ItemState replication order is arbitrary — re-derive the carry
	// indicator when the owner arrives on a proxy.
	virtual void OnRep_Owner() override;

private:
	bool TryFindDropSpot(FVector& InOutLocation) const;
	// giMPBattle(Level.Game).Battle — the placed BattleInfo, or null off a Battle game / before
	// PostBeginPlay locates it.
	AWoTBattleInfo* GetBattleInfo() const;
	// Seal.GetClosestPathNodeLocation (Seal.uc:152-182): the closest stamped
	// NavigationPoint (AWoTPathNode), floor-adjusted. False (no nodes) leaves the
	// location untouched; the caller warns like retail.
	bool GetClosestPathNodeLocation(FVector& NodeLocation) const;
	// Retire the carry indicator gracefully (bOn=false + LifeSpan 1 — Seal.uc:369-373).
	void ClearIndicator();
	// Per-machine: spawn/retire the indicator from the replicated carry state
	// (Held + pawn owner). The retail CarrySealSprayer replicated as an actor; the
	// port follows the replicated-identity + local-cosmetics convention.
	void UpdateIndicator();
	// Apply the faction skin (mesh slot 1 MI_S_<Faction>Seal) + status icons from
	// ControllingFactionName on this machine.
	void ApplyControllingFaction();
	void ApplyEditingGlowPresentation();
	void ApplyPlacedPresentation();

	// Per-machine cosmetic (the retail Indicator).
	UPROPERTY() TObjectPtr<AWoTParticleSprayer> Indicator;
};

// SealInventory.uc: the hidden, one-count citadel palette card for AWoTSeal. It is
// intentionally distinct from the carried seal itself (D19 selection checks both).
UCLASS()
class AWoTSealInventory : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTSealInventory();
};
