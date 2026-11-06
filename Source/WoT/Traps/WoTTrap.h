#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTTrap.generated.h"

class UCapsuleComponent;
class AWoTMover;
class AWoTTrapBurst;
class AController;

namespace WoTTrapHelpers
{
	AWoTTrapBurst* SpawnTrapBurst(UWorld& World, const FVector& Location);
	AWoTMover* ResolveTaggedMover(
		UWorld* World, FName Tag, const UClass* MoverClass);
}

enum class EWoTCitadelResourcePresentation : uint8
{
	Hidden,
	Inactive,
	Active,
	InactiveHighlight,
	ActiveHighlight,
};

// Port of WoTBase/Trap.uc — the abstract base for the citadel/siege defensive traps (barriers,
// hazards, alarm). The editor placement primitives (Trap.CalcLocation / DeployResource /
// BeginEditingResource) and runtime trap-actor spine share this base: the
// team gate, collision Hide/Show, destroyed-event routing, and multi-actor identity. Trap.uc has
// NO replication{} block — tunables are CDO/level data; each subclass sets its own RemoteRole and
// any damage-staging replication.
UCLASS(Abstract)
class AWoTTrap : public AActor
{
	GENERATED_BODY()

public:
	// Bind an imported trap mesh onto a vertex-anim visual component (6b: the
	// class-default visual — no placed trap overrides one). Quiet miss headless.
	static void BindTrapVisual(class UWoTVertexAnimComponent* Visual, FName MeshName,
	                           const FVector& Scale);

public:
	AWoTTrap();
	virtual void Destroyed() override;

	// --- Trap.uc tunables (var()) — CDO/level data (a subclass may route these through a
	// DataTable per iron rule 2 when its values are placement-tuned). ---
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") int32 DamageAmount = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") int32 HeightAboveFloor = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") int32 SeparationDistance = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") int32 TrapRadius = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") int32 TraceDistance = 1600;
	// Trap.TrapType — the identifier the TrapDetect artifact scans for (UWoTTrapDetectLeech;
	// names tabled in TrapDetectLeech.uc:286-291). NO shipped .uc sets it — GetTrapID falls back
	// to the class name, which our ctor-set values stand in for (UE5 class names differ). Per-
	// INSTANCE map data can override it (Mission_07a's Staircase0 is tagged TrapType=Pit), hence
	// EditAnywhere.
	UPROPERTY(EditAnywhere, Category = "WoT|Trap") FName TrapType;
	// Trap.DestroyedEvent — fired through the Tag/Event subsystem when the trap is destroyed.
	UPROPERTY(EditAnywhere, Category = "WoT|Trap") FName DestroyedEvent;

	// --- runtime state ---
	// Trap.Team (default 255 = neutral). Traps do not fire on same-team pawns (IsInactive).
	UPROPERTY(EditAnywhere, Category = "WoT|Trap") uint8 Team = 255;
	// Trap.bEditing — set while a player is placing/editing. IsInactive returns true while
	// editing, so a trap never fires mid-placement. REPLICATED: UE1 got the edit-phase
	// collision state to clients for free (bCollideActors replicates), and the editor cursor
	// (EditorTrace) runs its ECC_Visibility trace on the LOCAL client — so the collision half
	// of Begin/EndEditingResource must reach net proxies. OnRep re-applies it there.
	UPROPERTY(ReplicatedUsing = OnRep_Editing) bool bEditing = false;
	// Public so a role-explicit spec can drive the client half directly (a real second
	// connection is out of scope for the headless suite).
	UFUNCTION()
	void OnRep_Editing();
	// Trap.bLocked — tutorial-only removal inhibit (ported inert; the SP tutorial path is deferred).
	bool bLocked = false;
	// Trap.uc:71,76 — the "being edited" brightness cue: BeginEditingResource sets
	// AmbientGlow=64, EndEditingResource clears it. Named exactly AmbientGlow so the
	// trap visual's runtime UWoTMeshLight picks it up through its reflection read
	// (ActorNumericProperty; Ambient += AmbientGlow/255 per channel — the UE1 formula).
	// Derived from replicated bEditing on every machine, never replicated itself.
	UPROPERTY(Transient) float AmbientGlow = 0.0f;

	// Trap.IsInactive — the gate every trap consults before firing on a toucher: true while editing,
	// or when Other is a pawn on this trap's Team (players via PlayerState.Team; NPC AssignedTeam is
	// folded when traps meet NPCs in Battle). Non-pawns read as active (false). Virtual: Pit adds a
	// Seal exclusion.
	virtual bool IsInactive(const AActor* Other) const;

	// Trap.BeginEditingResource / EndEditingResource (Trap.uc:67-77): resources stay dormant
	// during the citadel build, then go live when their team's edit session ends.
	virtual void BeginEditingResource(uint8 PlacedByTeam);
	virtual void EndEditingResource();

	// Trap.GetDeploySound / GetRemoveSound (WoTBase/Trap.uc:57-65): the citadel editor's
	// owner-only create/remove/toggle feedback pair, dispatched virtually off the resource
	// (retail ClientPlaySound sites WOTPlayer.uc:4938/4965/5014/5043 — the port delivers
	// via AWoTPlayerController::ClientPlayUISound). Subpaths under /Game/WoT/Sounds.
	virtual FName GetDeploySoundPath() const { return FName(TEXT("WoTBase/DeployTrap")); }
	virtual FName GetRemoveSoundPath() const { return FName(TEXT("WoTBase/RemoveTrap")); }

	// Begin/EndSelectResource (Portcullis.uc:62-77, Staircase.uc:52-66): while the palette
	// has this resource's class selected, an INACTIVE toggle resource becomes traceable
	// (Show()); deselected, it hides from the cursor again (an active one keeps its
	// collision — EndSelectResource's Super(Actor).Hide() is sprite-only). The stamp is
	// per-machine presentation state driven by the owner-local palette (the trace runs on
	// the owner's machine); subclasses fold it into ApplyEditingCollision. Base traps
	// ignore it — only the two preplaced toggle resources have select hooks.
	void SetCitadelSelectTraceability(bool bSelected);
	bool IsCitadelSelectedForTrace() const { return bCitadelSelectedForTrace; }
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	int32 GetBeginEditingResourceCallCount() const
	{
		return BeginEditingResourceCallCount;
	}
	int32 GetEndEditingResourceCallCount() const
	{
		return EndEditingResourceCallCount;
	}

	// Citadel placement primitives (Trap.uc:80-165, 212). DeployResource is false in the
	// abstract base; spatial trap subclasses opt in with their own placement validation.
	virtual bool CalcLocation(FVector& Loc, FVector& Normal);
	virtual AActor* AnyActorsInArea(const FVector& Loc, float Radius) const;
	virtual bool DeployResource(FVector Loc, FVector Normal) { return false; }
	virtual bool RemoveResource() { return true; }

	// UE1 keeps the placing PlayerPawn (and its PRI/controller identity) across
	// RestartPlayer. UE5 replaces the pawn, so persistent traps retain the
	// controller separately; a disconnected/destroyed controller resolves null,
	// matching a retail instigator whose PRI is gone.
	void SetDamageInstigatorController(AController* Controller)
	{
		DamageInstigatorController = Controller;
		bDamageInstigatorCleared = false;
	}
	void ClearDamageInstigatorController(AController* Controller)
	{
		if (DamageInstigatorController.Get() == Controller)
		{
			DamageInstigatorController.Reset();
			bDamageInstigatorCleared = true;
		}
	}
	AController* ResolveDamageInstigatorController() const;

	// Slice 2f: a real resource class may be used as an owner-local placement ghost, but it
	// must remain pure presentation even in standalone worlds where a local spawn has authority.
	// Set this on a deferred actor before FinishSpawning so subclass BeginPlay can suppress all
	// gameplay setup. Preview state is deliberately runtime-only and never replicated.
	void SetPreviewMode(bool bEnabled);
	bool IsPreviewMode() const { return bPreviewMode; }
	void ConfigurePreviewChild(AActor* Child);
	void ResetPreviewChildren();
	void SetPreviewVisible(bool bVisible);
	bool IsPreviewPlacementVisible() const { return bPreviewPlacementVisible; }

	// Trap.DestroyedTrigger — fire DestroyedEvent through UWoTEventSubsystem (authority only).
	void DestroyedTrigger(AActor* Other, APawn* EventInstigator);

	// Trap.GetBaseResource / SameLogicalActor — identity for multi-actor traps (a Wall's slabs share
	// the manager as their base). Slab/opening children override GetBaseResource to return their owner.
	virtual AActor* GetBaseResource() { return this; }
	bool SameLogicalActor(AActor* Other) const;

	// Trap.Hide / Show — UE1 coupled visibility with collision. Named *Trap to avoid the AActor
	// Hidden accessors.
	void HideTrap();
	void ShowTrap();

	// Trap.DamageActor — empty in the base; Spear / FireWall override to hurt the toucher.
	virtual void DamageActor(AActor* Other) {}

	// The trap's touch handler (UE1 Actor.Touch on the trap). Empty in the base; Spear overrides it
	// to begin activating. AWoTTrapBlocker's non-damage overlap forwards here (mirroring the
	// original NoDamage state's `Owner.Touch(Other)`), so the collision proxy can drive the owner.
	virtual void TrapTouch(AActor* Other) {}

	// Trap.IsTrapDeployed — previews are presentation-only and must be invisible to external
	// trap observers. Portcullis / Staircase additionally require their Active state.
	virtual bool IsTrapDeployed() const { return !IsPreviewMode(); }

	// Owner-local Slice-2c marker identifier. The local controller renders it through a
	// transient non-colliding marker actor; it never calls HideTrap/ShowTrap or mutates
	// gameplay state.
	void SetCitadelEditorPresentation(EWoTCitadelResourcePresentation Presentation);
	EWoTCitadelResourcePresentation GetCitadelEditorPresentation() const
	{
		return CitadelEditorPresentation;
	}
	FName ResolveCitadelEditorMarkerTexture(bool bActive, bool bHighlight) const
	{
		return GetCitadelEditorMarkerTexture(bActive, bHighlight);
	}

protected:
	// UE1 Trap defaults bCollideActors=True; the trigger cylinder is the trap's touch volume,
	// resized per subclass (CollisionRadius/Height in the .uc defaults).
	UPROPERTY(VisibleAnywhere, Category = "WoT|Trap")
	TObjectPtr<UCapsuleComponent> Capsule;

	// The COLLISION half of Begin/EndEditingResource, applied on the authority inside those
	// calls and on net clients from OnRep_Editing (the editor cursor traces ECC_Visibility
	// locally, so edit-time selectability must exist on every machine). Subclasses put ONLY
	// collision-response state here; blockers/child actors/authority world state stay in
	// Begin/EndEditingResource. Base Trap.uc changes no collision while editing (AmbientGlow
	// only) — no-op here.
	virtual void ApplyEditingCollision(bool bEditingNow) {}

	// Texture basename for this trap's inactive/active editor marker. Only the two
	// preplaced toggle resources override it.
	virtual FName GetCitadelEditorMarkerTexture(bool bActive, bool bHighlight) const
	{
		return NAME_None;
	}

private:
	// The GLOW half of Begin/EndEditingResource (Trap.uc:71,76): derive AmbientGlow from
	// bEditing and re-bake the visual's mesh light so the cue lands immediately. Called on
	// the authority inside Begin/EndEditingResource and on net clients from OnRep_Editing.
	void ApplyEditingGlowPresentation();
	// Lifecycle acceptance counters. They also make repeated teardown diagnosable without
	// changing BeginEditingResource/EndEditingResource's exported public contract.
	int32 BeginEditingResourceCallCount = 0;
	int32 EndEditingResourceCallCount = 0;
	// The palette-selection half of the toggle-resource traceability matrix (see
	// SetCitadelSelectTraceability). Local-only — never replicated.
	bool bCitadelSelectedForTrace = false;
	bool bPreviewMode = false;
	bool bPreviewPlacementVisible = false;
	TArray<TWeakObjectPtr<AActor>> PreviewChildren;
	EWoTCitadelResourcePresentation CitadelEditorPresentation =
		EWoTCitadelResourcePresentation::Hidden;
	TWeakObjectPtr<AController> DamageInstigatorController;
	bool bDamageInstigatorCleared = false;
};
