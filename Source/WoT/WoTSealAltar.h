#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTUE1Collision.h"
#include "WoTSealAltar.generated.h"

class AWoTBattleInfo;
class AWoTSeal;
class UCapsuleComponent;

// Port of SealAltar (exports/script/WOT/SealAltar.uc): the placed KeyPoint where a team banks
// seals to win. Citadel step 4 landed the win detection (Reset / PlaceSeal's bank half /
// CheckForWinner / AnnounceWinningTeam); the G13 wave adds the live Touch state machine
// (Active / the 1s Inactive debounce / GameEnded), the full on-altar placement physics
// (centered slots, PHYS_None, collision off, altar-owned, replicated position), the
// place/capture sounds + notifications, and the Event/WinEvent fan-out. The retail
// 48x1 KeyPoint cylinder becomes a 48-radius capsule (UE minimum half-height = radius;
// only airborne pawns see the taller window — PORT_NOTES rider).
UCLASS()
class AWoTSealAltar : public AActor
{
	GENERATED_BODY()

public:
	AWoTSealAltar();

	// SealAltar.Team (255 = any): which team may bank here. SealAltar.SealGoal (4): seals to win.
	UPROPERTY(EditAnywhere, Category = "WoT|Battle") uint8 Team = 255;
	UPROPERTY(EditAnywhere, Category = "WoT|Battle") int32 SealGoal = 4;
	// Actor.Event: fired (tag fan-out) on EVERY carried-seal touch, before the team gate.
	UPROPERTY(EditAnywhere, Category = "WoT|Battle") FName EventName;
	// SealAltar.WinEvent: actor event fired when this altar wins.
	UPROPERTY(EditAnywhere, Category = "WoT|Battle") FName WinEvent;
	// SealAltar.CaptureNotificationStr (localized; shipped WoT.int).
	UPROPERTY(EditAnywhere, Category = "WoT|Battle")
	FString CaptureNotificationStr = TEXT(" captured a seal.");

	// The placed BattleInfo (located at BeginPlay).
	UPROPERTY(BlueprintReadOnly, Category = "WoT|Battle") TObjectPtr<AWoTBattleInfo> Battle;

	// The retail KeyPoint touch cylinder (48x1). Touch is a containing capsule;
	// overlap callbacks are filtered against the exact flat UE1 cylinder.
	UPROPERTY(VisibleAnywhere, Category = "WoT|Battle")
	TObjectPtr<UCapsuleComponent> Touch;
	void SetUE1CollisionSize(float Radius, float HalfHeight);

	// SealAltar.Reset (:49-65): clear banked seals, reset this team's count, set the goal.
	// (un-hide AActor::Reset() — the seal-goal overload shares its name, not its purpose.)
	using AActor::Reset;
	void Reset(int32 Count);
	// SealAltar.CheckForWinner (:67-88): the team's banked count (via BattleInfo), or the
	// placed-seal count fallback off a Battle game, >= SealGoal.
	bool CheckForWinner() const;
	// SealAltar.PlaceSeal (:90-141): bank a seal for its owner's team — team-access gated unless
	// bForce — pulling it from the placer's inventory, centering it on the altar
	// (LayoutBankedSeals), freezing it (no spin, collision off, altar-owned,
	// position replicated), playing the place sound (own team) or the capture notification
	// (steal), and crediting it via Seal.SetControllingTeam.
	void PlaceSeal(AWoTSeal* Seal, bool bForce = false);
	// Offset along the altar's yaw of the Rank-th of Count banked seals: centred on the
	// seals actually banked (retail centres the whole SealGoal row, see the .cpp), retail
	// Seal.CollisionRadius spacing, compressed when the row would overhang the altar.
	float BankedSealOffset(int32 Rank, int32 Count) const;
	// Re-place every banked seal: centred by rank, resting on the visible altar top.
	void LayoutBankedSeals();
	// SealAltar.FindSeal (:157-172): the LAST banked seal. RemoveSeal (:174-186): clear its slot.
	AWoTSeal* FindSeal() const;
	void RemoveSeal(AWoTSeal* Seal);
	// SealAltar.AnnounceWinningTeam (:188-199): end every altar, record the mode's WinningTeam,
	// EndGame("AltarFilled").
	void AnnounceWinningTeam();

	// state Active.Touch (:208-270), public like AWoTTrigger::OnOverlap (a headless overlap
	// is awkward to synthesize; the bound handler routes here).
	void TouchedByPawn(APawn* Other);

	// bEnded: the GameEnded state. bInactive: the 1s debounce state.
	UPROPERTY(BlueprintReadOnly, Category = "WoT|Battle") bool bEnded = false;
	UPROPERTY(BlueprintReadOnly, Category = "WoT|Battle") bool bInactive = false;

protected:
	// SealAltar.PostBeginPlay: locate the placed BattleInfo.
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnAltarOverlap(UPrimitiveComponent* Comp, AActor* Other,
	                    UPrimitiveComponent* OtherComp, int32 BodyIndex,
	                    bool bFromSweep, const FHitResult& Hit);

private:
	// SealAltar.SendCaptureNotification (:143-155): every player hears the capture and reads
	// "<TeamDescription> captured a seal.".
	void SendCaptureNotification(AController* Placer);
	void StartInactive();
	void EndInactive();

	// SealAltar.Seals[4]: the banked seals (fixed slots, filled left to right).
	UPROPERTY() TObjectPtr<AWoTSeal> Seals[4];
	FTimerHandle InactiveTimer;
	float UE1CollisionRadius = 48.0f;
	float UE1CollisionHalfHeight = 1.0f;
	WoTUE1Collision::FTouchTracker UE1Touches;
};
