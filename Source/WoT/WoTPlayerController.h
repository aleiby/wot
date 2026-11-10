#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "WoTCitadelResources.h"
#include "WoTFrontEnd.h"
#include "WoTHUDDrawCmd.h"
#include "WoTInGameMenu.h"
#include "WoTPlayerController.generated.h"

class AWoTCharacter;
class AWoTInventory;
class FWoTFrontEndPresentationAuthority;
class FWoTChatInputPreProcessor;
class SWoTInGameMenuOverlay;
class SWoTChatInputOverlay;
class SWoTItemInfoOverlay;
class SWoTHelpOverlay;
class UGameViewportClient;

UENUM()
enum class EWoTCitadelEditCameraMode : uint8
{
	Inactive,
	Roam,
	Orbit
};

// Shared-enum port of WOTPlayer's input-relevant states. The controller owns this
// FSM because it survives the port's fresh-pawn respawns and can receive Fire while
// it is unpossessed.
UENUM()
enum class EWoTPlayerStateMachineState : uint8
{
	Playing,
	Dying,
	WaitingForSpawn,
	GameEnded
};

USTRUCT()
struct FWoTCitadelBudgetCount
{
	GENERATED_BODY()

	UPROPERTY() FName AdapterName;
	UPROPERTY() int32 ItemCount = 0;
};

// (bEditing, Team) — plain C++ multicast; the non-dynamic macro takes types only, no names.
DECLARE_MULTICAST_DELEGATE_TwoParams(FWoTCitadelEditSessionChanged, bool, uint8);

// Controller half of WOTPlayer's input FSM. It survives fresh-pawn respawns, so Fire
// can dispatch live casting, Dying respawn, and GameEnded match restart distinctly.
UCLASS()
class AWoTPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	virtual void UpdateRotation(float DeltaTime) override;
	/** The legacy yaw/pitch input scales AddYaw/PitchInput multiply by (BaseGame.ini
	 * 2.5 / -2.5), or 1 when legacy input scales are off. */
	FVector2D GetLegacyLookInputScale() const;
	/** The Joystick Enabled option (a test override first). */
	bool IsJoystickInputEnabled() const;
#if WITH_AUTOMATION_TESTS
	void ApplyPitchRotationForTest(float PitchInputDegrees);
	EWoTPlayerStateMachineState PlayerStateForTest() const
	{
		return PlayerStateMachineState;
	}
	class UWoTGamepadLayer* GetGamepadLayerForTest() const { return GamepadLayer; }
	/** Stands in for the Joystick Enabled preference (spec worlds have no preferences). */
	void SetJoystickEnabledOverrideForTest(TOptional<bool> bEnabled) { JoystickEnabledOverrideForTest = bEnabled; }
	/** Sets the owner's edit-session mirror IsOwnerEditingCitadel reads. */
	void SetOwnerEditSessionTeamForTest(uint8 Team) { OwnerEditSessionTeam = Team; }
#endif
	// Authority-only citadel edit grant. This is deliberately not replicated; edit RPCs bind to
	// the team and exact budget that granted the session instead of trusting the team shadow.
	UPROPERTY() TObjectPtr<class AWoTBudgetInfo> EditSessionBudget = nullptr;
	uint8 EditSessionTeam = 255;

	// Owner-client mirror of the authority grant. Team 255 is the exit state. BudgetInfo
	// remains server-only; only the rows needed by the local palette cross the wire.
	UPROPERTY(ReplicatedUsing = OnRep_OwnerEditSessionTeam)
	uint8 OwnerEditSessionTeam = 255;
	UPROPERTY(ReplicatedUsing = OnRep_OwnerEditBudget)
	TArray<FWoTCitadelBudgetCount> OwnerEditBudget;
	// Retail's per-player SealInventory.Count, independent of the team budget.
	// Starts at one on each fresh controller and is never reset intra-match.
	UPROPERTY(Replicated)
	int32 OwnerSealCardCount = 1;
	// Controller/session replacement for the two UnrealScript editor states. The zone is
	// retained because an orbit view may lie outside the volume that selected it.
	UPROPERTY(ReplicatedUsing = OnRep_CitadelEditCamera)
	EWoTCitadelEditCameraMode CitadelEditCameraMode =
		EWoTCitadelEditCameraMode::Inactive;
	UPROPERTY(ReplicatedUsing = OnRep_CitadelEditCamera)
	TObjectPtr<class AWoTZoneVolume> CitadelEditZone = nullptr;
	// WOTPlayer.ViewOffset: look input's offset from the zone's authored orbit view.
	// Local only; cleared whenever a zone is selected (SetZoneNumber) or editing ends.
	FRotator CitadelOrbitViewOffset = FRotator::ZeroRotator;

	FWoTCitadelEditSessionChanged OnCitadelEditSessionChanged;

	bool IsEditingCitadel() const { return EditSessionTeam != 255; }
	bool IsOwnerEditingCitadel() const { return OwnerEditSessionTeam != 255; }
	bool CanChangeTeamNow() const { return !IsEditingCitadel(); }
	int32 GetOwnerCitadelBudgetCount(FName AdapterName) const;
	// Client-safe twin of BudgetInfo.GetResourceAdapterForTier: scans the replicated
	// owner mirror in budget order and applies the shared registry tier predicate.
	FName GetOwnerCitadelTroopAdapter(FName TierKeyword) const;
	// Authority-side snapshot after a successful budget mutation. Public because future
	// server budget grant paths must use the same owner mirror, not replicate BudgetInfo.
	void RefreshOwnerCitadelBudgetMirror();
	static constexpr ELifetimeCondition CitadelOwnerReplicationCondition()
	{
		return COND_OwnerOnly;
	}
	UFUNCTION() void OnRep_OwnerEditSessionTeam();
	UFUNCTION() void OnRep_OwnerEditBudget();
	UFUNCTION() void OnRep_CitadelEditCamera();
	FVector GetCitadelViewerLocation() const;
	EWoTCitadelEditCameraMode GetCitadelEditCameraMode() const
	{
		return CitadelEditCameraMode;
	}
	FVector GetCitadelEditStartLocation() const { return EditStartLoc; }
	FRotator GetCitadelEditStartRotation() const { return EditStartRot; }
	AWoTZoneVolume* GetCitadelEditZone() const { return CitadelEditZone.Get(); }
	/** The look offset from the zone's authored orbit view (WOTPlayer ViewOffset). */
	FRotator GetCitadelOrbitViewOffset() const { return CitadelOrbitViewOffset; }
	static float ClampCitadelEditViewDistanceFeet(float DistanceFeet);
	static float CitadelEditViewDistanceFeetToUU(float DistanceFeet);
	static float AdjustCitadelEditViewDistanceFeet(float CurrentFeet,
		float AForward, float DeltaTime);
	static void CalculateCitadelOrbitView(const FVector& ZoneLocation,
		const FRotator& EditViewAngle, float EditViewDistanceFeet,
		FVector& OutLocation, FRotator& OutRotation,
		const FRotator& ViewOffset = FRotator::ZeroRotator);
	void AdjustCitadelOrbitDistance(float AForward, float DeltaTime);
	/** EditViewAngle after a decelerated UE1 aStrafe for DeltaTime (PlayerTickEdit). */
	static FRotator AdjustCitadelEditViewAngle(const FRotator& EditViewAngle,
		float AStrafe, float DeltaTime);
	void RotateCitadelOrbit(float AStrafe, float DeltaTime);
	static constexpr float TRACE_DISTANCE = 3200.0f;
	// WOTPlayer.EditorTrace + EditorHUD cursor start/direction: one canonical visible
	// world/actor trace from the current view. Preview-owned child actors are ignored too.
	bool EditorTrace(FWoTCitadelEditorTraceResult& OutHit,
		AActor* PreviewActor = nullptr) const;

	// WOTPlayer.uc:311 bProhibitEditorExit: tutorial scripting may block a
	// voluntary editor exit and post the localized refusal (uc:5146-5155). No
	// shipped MP map/mode sets it; the
	// surface lands ahead of any SP content that will. Owner-replicated so the
	// refused exit skips the owner-local pre-RPC selection cleanup.
	UPROPERTY(EditAnywhere, Replicated, Category = "WoT|Battle")
	bool bProhibitEditorExit = false;
	// Shipped WoT.int WOTPlayer.CantLeaveEditorStr.
	static FString CantLeaveEditorStr();

	UFUNCTION(Exec) void CitadelBeginEdit(int32 Mode = 0);
	// WOTPlayer.ServerEditCitadel (uc:4448-4458): F10 enters only when the
	// Battle mode permits editing; all other modes show CantEditLevelStr.
	UFUNCTION(Exec) void ServerEditCitadel(int32 Mode = 0);
	bool CanRequestCitadelEdit() const;
	// Input-independent seam. Real Fire and the one retained debug EXEC both
	// converge here; tests call it directly without fabricating hardware input.
	EWoTCitadelEditorAction HandleCitadelEditorClick();
	UFUNCTION(Exec) void CitadelClick();
	UFUNCTION(Exec) void CitadelEndEdit();

	UFUNCTION(Server, Reliable)
	void ServerCreateResource(FName Adapter, FVector Loc, FVector Normal);
	UFUNCTION(Server, Reliable)
	void ServerRemoveResource(AActor* Selected);
	UFUNCTION(Server, Reliable)
	void ServerToggleResource(class AWoTTrap* Selected);

	// Owner-local presentation. Palette/hover calls never cross the network;
	// persistent activation remains ServerToggleResource.
	void SetCitadelPaletteSelection(FName AdapterName);
	void SetHoveredCitadelResource(AActor* Candidate);
	void ClearCitadelSelectionPresentation();
	void RefreshCitadelSelectionPresentation();
	FName GetCitadelPaletteSelection() const { return SelectedCitadelAdapter; }
	class AWoTTrap* GetHoveredCitadelResource() const
	{
		return HoveredCitadelResource.Get();
	}

	// Owner-local transient placement ghost. Reconcile is separated from trace/sync so
	// headless specs can drive lifecycle and placement hits without fabricating a viewport.
	void ReconcileCitadelGhostPreview();
	void SyncCitadelGhostPreview(const FWoTCitadelEditorTraceResult& Hit);
	class AWoTTrap* GetCitadelGhostPreview() const
	{
		return CitadelGhostPreview.Get();
	}
	AActor* GetCitadelAnyGhostPreview() const
	{
		return CitadelGhostPreviewActor.Get();
	}
	bool IsCitadelGhostPlacementValid() const
	{
		return bCitadelGhostPlacementValid;
	}

	// Shared, idempotent normal-exit/disconnect teardown. Public so BattleGameMode::Logout can
	// invoke it before Super::Logout invalidates the controller's PlayerState.
	void EndEditSession();

	virtual void BeginPlay() override;
	virtual void ClientReturnToMainMenuWithTextReason_Implementation(
		const FText& ReturnReason) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Destroyed() override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void SetPawn(APawn* InPawn) override;
	virtual void OnRep_Pawn() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;
	virtual bool InputKey(const FInputKeyEventArgs& Params) override;
#if WITH_AUTOMATION_TESTS
	void TickRetailCameraForTest(float DeltaTime) { TickRetailCamera(DeltaTime); }
	const FVector& RetailWalkBobForTest() const { return UE1WalkBob; }
	float RetailEyeHeightForTest() const { return UE1EyeHeight; }
	const FVector& RetailPreviousPawnLocationForTest() const
	{
		return UE1PreviousPawnLocation;
	}
	EMovementMode RetailPreviousMovementModeForTest() const
	{
		return UE1PreviousMovementMode;
	}
	bool IsJoystickInputEnabledForTest() const;
#endif
	// The item-info selection poll, split from PlayerTick so specs can drive it
	// on controllers without an input stack (Super::PlayerTick asserts).
	void TickItemInfoLifecycle();
	// The citadel edit-session phase reconcile + ghost/selection presentation
	// (the original PlayerTick tail; split alongside TickItemInfoLifecycle).
	void TickCitadelEditPresentation();
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;
	virtual void PreClientTravel(const FString& PendingURL, ETravelType TravelType,
		bool bIsSeamlessTravel) override;
	virtual void PostSeamlessTravel() override;
	virtual void NotifyLoadedWorld(FName WorldPackageName, bool bFinalDest) override;

	// Owner-only UI-ish sounds (UE1 PlayerPawn.ClientPlaySound): the TrapDetect
	// heartbeat + leech expiry play on the owning client alone, non-spatialized.
	UFUNCTION(Client, Unreliable)
	void ClientPlayUISound(FName SoundSubPath, float Volume);

	// PlayerPawn.ClientAdjustGlow: additive owner-view color scale/fog used by
	// Freeze. Calling with the negated pair restores the prior view exactly.
	UFUNCTION(Client, Reliable)
	void ClientAdjustGlow(float Scale, FVector Fog);
	// PlayerPawn.ClientFlash: transient owner-only damage color treatment.
	UFUNCTION(Client, Unreliable)
	void ClientFlash(float Scale, FVector Fog);
	UPROPERTY(Transient) float ConstantGlowScale = 0.0f;
	UPROPERTY(Transient) FVector ConstantGlowFog = FVector::ZeroVector;
	UPROPERTY(Transient) float DesiredFlashScale = 0.0f;
	UPROPERTY(Transient) FVector DesiredFlashFog = FVector::ZeroVector;
	UPROPERTY(Transient) FVector CurrentViewColorScale = FVector::OneVector;
	UPROPERTY(Transient) float CurrentViewFlashScale = 1.0f;
	UPROPERTY(Transient) FVector CurrentViewFlashFog = FVector::ZeroVector;
	// DistantEyeLeech's owner-client FOV surface. The client saves once and
	// restores idempotently across every component teardown path.
	UFUNCTION(Client, Reliable)
	void ClientSetCameraFOV(float NewFOV);
	UFUNCTION(Client, Reliable)
	void ClientRestoreCameraFOV();
	float GetCameraFOVForTest() const;

	// WhirlwindLeech's WOTPlayer.bForceRotation/ForcedRotationRate owner seam.
	UFUNCTION(Client, Reliable)
	void ClientSetForcedViewYaw(bool bEnable, float YawRateUnitsPerSecond);
	int32 GetForcedViewApplyCountForTest() const { return ForcedViewApplyCountForTest; }
	UPROPERTY(Transient) bool bForcedViewYaw = false;
	UPROPERTY(Transient) float ForcedViewYawRateUnits = 0.0f;
	UPROPERTY(Transient) int32 ForcedViewApplyCountForTest = 0;

	// Engine.PlayerPawn ShakeView/ViewShake owner-client seam used by Earth Tremor.
	UFUNCTION(Client, Unreliable)
	void ClientShakeView(float ShakeTime, float RollMag, float VertMag);
	int32 GetShakeRequestCountForTest() const { return ShakeRequestCountForTest; }
	UPROPERTY(Transient) int32 ShakeRequestCountForTest = 0;
	UPROPERTY(Transient) float UE1ShakeTimer = 0.0f;
	UPROPERTY(Transient) float UE1ShakeMag = 0.0f;
	UPROPERTY(Transient) float UE1MaxShakeVert = 0.0f;
	UPROPERTY(Transient) float UE1VertTimer = 0.0f;
	UPROPERTY(Transient) float UE1ShakeVert = 0.0f;
	UPROPERTY(Transient) bool bUE1ShakeDir = false;

	// --- MP session console verbs (UWoTSessionSubsystem wrappers).
	// Console: WoTHost [lan] / WoTFind [lan] / WoTJoin <index> / WoTLeave.
	UFUNCTION(Exec) void WoTHost(bool bLan = false);
	UFUNCTION(Exec) void WoTFind(bool bLan = false);
	UFUNCTION(Exec) void WoTJoin(int32 Index = 0);
	UFUNCTION(Exec) void WoTLeave();

	// Gameplay Escape owns only this overlay. Retail's CitadelEditor overrides
	// ShowMenu to leave edit mode, so an editing owner consumes the first Escape
	// through CitadelEndEdit and does not open the overlay until a later press.
	void ToggleInGameMenu();
	void ResumeFromInGameMenu();
	void LeaveToFrontEnd();
	bool IsInGameMenuOpen() const { return InGameMenu.IsOpen(); }
	bool IsLeavingForMenu() const { return InGameMenu.IsLeaving(); }

	// Engine.Console typing actions. Type starts empty;
	// Talk/TeamTalk start with the exact commands the retail console executed.
	static constexpr int32 MaxChatMessageLength = 128;
	UFUNCTION(Exec) void Type();
	UFUNCTION(Exec) void Talk();
	UFUNCTION(Exec) void TeamTalk();
	UFUNCTION(Server, Reliable, WithValidation) void Say(const FString& Message);
	UFUNCTION(Server, Reliable, WithValidation) void TeamSay(const FString& Message);
	UFUNCTION(Exec) void Vote(const FString& Request);
	UFUNCTION(Server, Reliable, WithValidation) void ServerVote(const FString& Request);
	UFUNCTION(Exec) void Admin(const FString& Command);
	UFUNCTION(Exec) void AdminLogin(const FString& Password);
	UFUNCTION(Exec) void AdminLogout();
	UFUNCTION(Exec) void Kick(const FString& PlayerName);
	UFUNCTION(Exec) void KickBan(const FString& PlayerName);
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerAdminCommand(const FString& Command);
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerAdminLogin(const FString& Password);
	UFUNCTION(Server, Reliable) void ServerAdminLogout();
	// The authority's view of a chat payload: capped at Console.TextMsgSize and
	// flattened to one line (control and line-break characters become spaces).
	static FString SanitizeChatMessage(const FString& Message);
	bool IsChatTyping() const { return bChatTyping; }
	void CancelChatInput();
	void SubmitChatInput(const FString& Line);
#if WITH_AUTOMATION_TESTS
	FString GetChatTextForTesting() const;
	void SetChatTextForTesting(const FString& Text);
	void SubmitChatForTesting();
	void EnableChatWidgetForTesting() { bEnableChatWidgetForTesting = true; }
#endif

	// Engine.PlayerPawn SShot/ToggleFullscreen direct features plus
	// WOTPlayer.ShowHelp's retail localized WOTHelpWindow toggle.
	UFUNCTION(Exec) void SShot();
	UFUNCTION(Exec) void ToggleFullscreen();
	UFUNCTION(Exec) void ShowHelp();
	UFUNCTION(Exec) void PlayTaunt(int32 TauntIndex);
	UFUNCTION(Server, Reliable) void ServerPlayTaunt(int32 TauntIndex);
	static FName TauntSlotForIndex(int32 TauntIndex);
	UFUNCTION(Exec) void Brightness();
	virtual void Pause() override;
	/** Bound pause criterion: true permits an ordinary resume; binding it to
	 * this controller also lets engine disconnect cleanup remove its pause. */
	bool CanUnpauseOwnedMatch() { return true; }
	UFUNCTION(Server, Reliable) void ServerRequestPause();
#if WITH_AUTOMATION_TESTS
	FName GetLastTauntSlotForTesting() const { return LastTauntSlotForTesting; }
	void HandlePauseRequestForTesting(ENetMode NetMode) { HandlePauseRequest(NetMode); }
#endif
	bool IsHelpOpen() const { return bHelpOpen; }

	// Equals/Minus run both retail commands: interface level outside the editor,
	// next/previous team-visible zone while CitadelOrbitMode owns the view.
	UFUNCTION(Exec) void ViewUp();
	UFUNCTION(Exec) void ViewDown();
	UFUNCTION(Exec) void NextView();
	UFUNCTION(Exec) void PrevView();
	UFUNCTION(Exec) void SwitchView(int32 Index);

	// WOTPlayer.ShowInventoryInfo + InventoryInfoWindow: controller-owned state
	// with a code-only Slate projection when a viewport exists.
	UFUNCTION(Exec) void ShowInventoryInfo();
	bool IsItemInfoOpen() const { return bItemInfoOpen; }
	const AWoTInventory* GetItemInfoItem() const { return ItemInfoItem.Get(); }

	// Engine.PlayerPawn's eight local progress slots and timeout. Direct setters
	// serve engine-local producers; Client* twins preserve the retail reliable
	// server-to-owner API shape for future gameplay producers.
	static constexpr int32 NumProgressMessages = 8;
	void ClearProgressMessages();
	void SetProgressMessage(const FString& Message, int32 Index);
	void SetProgressColor(const FLinearColor& Color, int32 Index);
	void SetProgressTime(float Duration);
	UFUNCTION(Client, Reliable) void ClientClearProgressMessages();
	UFUNCTION(Client, Reliable) void ClientSetProgressMessage(
		const FString& Message, int32 Index);
	UFUNCTION(Client, Reliable) void ClientSetProgressColor(
		FLinearColor Color, int32 Index);
	UFUNCTION(Client, Reliable) void ClientSetProgressTime(float Duration);
	FString GetProgressMessage(int32 Index) const;
	FLinearColor GetProgressColor(int32 Index) const;
	float GetProgressTimeOut() const { return ProgressTimeOut; }
	bool AreProgressMessagesActive(float TimeSeconds) const;
	TArray<FWoTProgressEntry> GetProgressEntries() const;
	void PopulateNetworkFailureProgress(const FString& FailureType,
		const FString& ErrorString);
	static FString GetTravelDestinationDisplay(const FString& PendingURL);

	// WOTPlayer.PlayerRestartGameDelay (WOTPlayer.uc:7251 default = 15s): the GameEnded
	// freeze before a player's fire may restart / rotate the match.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float PlayerRestartGameDelay = 15.0f;

	// End camera — GameInfo.SetEndCams → PlayerPawn.GameEnded.BeginState.
	// FindGoodView runs before bBehindView: normal self-view
	// therefore fixes pitch at 56000 UU (-52.4°) but retains yaw, then the per-frame
	// self branch uses CalcBehindView(.., 150). Only an already-external ViewTarget
	// makes the 16 yaw trials exercise CalcBehindView(.., 180) and retain the farthest.
	// Both arms trace Dist+30 and stand off 30 from a hit.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float EndCamDistance = 150.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float ExternalEndCamDistance = 180.0f;
	FRotator FindGoodEndView(const FVector& Anchor) const;
	FVector CalcEndCamLocation(const FVector& Anchor, const FRotator& View,
		float Distance) const;
	void ApplyEndCamera();

	// GameInfo.EndGame → the player enters WOTPlayer's GameEnded state. Server-side: latch
	// the ended flag and arm the restart-delay timer (GameEnded.BeginState). Idempotent.
	// AWoTGameMode::EndGame calls this on every player controller.
	void NotifyMatchEnded();

	// WOTPlayer.Dying.BeginState / Fire. PlayerKilled arms this authority-side before
	// leaving the controller unpossessed. Fire is controller-owned so the client can
	// still send the respawn request after its dead pawn has been detached.
	void ArmRespawnFire(AWoTCharacter* Corpse, float RespawnDelaySecs);
	void Fire();
	// Fire release (exec CeaseUsingAngreal, WOTPlayer.uc:1215). Must be bound on the
	// SAME input component as the press: UPlayerInput consumes a bound action's key on
	// both edges per component, so a pawn-side release below the controller's press
	// binding would be starved and the weave would never cease.
	void CeaseFire();

	// Jump lives beside Fire on the controller for the same two reasons: the Dying
	// state has no pawn to receive input, and a controller binding consumes the key on
	// both edges for the pawn below (binding it here only-while-dying would need
	// fragile bind/unbind bookkeeping on every FSM edge, and binding it on BOTH
	// components would starve one side — so the controller owns it permanently and
	// forwards live presses to ACharacter::Jump/StopJumping). While Dying, a jump
	// press routes into Fire — retail respawns on the jump key through the same
	// OKToFireTime gate (PlayerPawn's Dying.PlayerMove fires on bPressedJump;
	// its Dying.ServerMove fires on the jump-status edge; WOTPlayer keeps both).
	void Jump();
	void StopJumping();

	// Arena scoreboard presentation is client-local: both shipped keys toggle the
	// same flag, while match-end visibility remains forced independently.
	UFUNCTION()
	void ShowScores();
	UPROPERTY(Transient, BlueprintReadOnly, Category = "WoT|Scoreboard")
	bool bShowScores = false;
	bool IsDeathScoreboardAutoShowActive() const
	{
		return bDeathScoreboardAutoShowActive;
	}
	EWoTPlayerStateMachineState GetPlayerStateMachineState() const
	{
		return PlayerStateMachineState;
	}

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRespawnPlayer();

	// PlayerPawn.ServerRestartPlayer: Level.Game.RestartPlayer(self) unconditionally.
	// BattleHUD's team select (BattleHUD.uc:63-64) depends on it restarting a LIVE
	// player at a team start; the engine's retained ServerRestartPlayer RPC no-ops for
	// a live pawn, so this WoT RPC replaces the pawn (the port replaces pawns instead
	// of moving them — same divergence as ServerRespawnPlayer). The live pawn is kept
	// until its replacement exists, and kept outright when every start refuses.
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRestartPlayerWoT();

	// PlayerPawn.ServerReStartGame → Level.Game.RestartGame(). A fire in the GameEnded state,
	// once the freeze has lifted, restarts (and for a FragLimit match, rotates) the match.
	// Server RPC: the client's fire routed to the authority.
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRestartGame();

	// WOTPlayer.ServerChangeTeam (WOTPlayer.uc:4225-4235, replicated server fn :411): a
	// client-initiated team switch routed to the authority game mode's ChangeTeam (the range
	// clamp / smallest-team resolution lives there). This is the seam the deferred team-select UI
	// and the Battle spectator auto-timeout (giMPBattle.Timer) both drive. An active citadel edit
	// grant rejects the request, matching the original bEditing guard.
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerChangeTeam(int32 Team);

	// WOTPlayer.JoinTeamTimeout. The Battle mode owns the 1 Hz countdown; owner-only
	// replication feeds BattleHUD's optional "in N seconds" prompt suffix.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Battle")
	int32 JoinTeamTimeout = 0;

	// PlayerPawn.ServerChangeSkin (a replicated server function):
	// the in-game menu path (menuPlayer.SaveConfigs). Gates on Game.bCanChangeSkin, then
	// hands the string to the pawn's SetMultiSkin fold. FaceName/TeamNum ride the retail
	// signature but WOTPlayer.SetMultiSkin ignores both (uc:4208-4220).
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerChangeSkin(const FString& SkinName, const FString& FaceName, uint8 TeamNum);
	// Battle team assignment runs on authority, but CitadelSkin is an owning-client
	// preference. The authority names the new faction; the owner reads its own game
	// instance and submits the ordinary validated ServerChangeSkin request.
	UFUNCTION(Client, Reliable)
	void ClientApplyCitadelSkin(EWoTPlayerFaction TeamFaction);

	// The player's current personal skin selection (GameInfo.Login's ?Skin= option,
	// updated by ServerChangeSkin). Server-side state: UE1 kept the applied skin on the
	// PERSISTENT PlayerPawn across respawns; the UE5 split respawns pawns, so the
	// controller carries the string and every possession re-applies it.
	FString PersonalSkinURL;

	// WOTPlayer.bTeleportingDisabled (WOTPlayer.uc:97, owner-only repl :335): per-player
	// teleport/cast freeze. The Battle mode drives it — true during the citadel-editing
	// phase and for neutral players even after play begins; JoinTeam folds in the current
	// phase. Owner-only so the owning client's UseAngreal gate runs without a round trip.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Battle")
	bool bTeleportingDisabled = false;
	// WOTPlayer.PreTeleport: true consumes a blocked teleporter touch and posts
	// the throttled localized reason to the owning player's center queue.
	bool PreTeleport();

	// WOTPlayer.bShowOverview (WOTPlayer.uc:106, owner-only repl :337): auto-show the
	// overview scoreboard — set for neutral players and pre-battle team joins, cleared
	// when real play starts. The HUD routes it to CitadelTeamOverview when replicated
	// BattleInfo is present; bShowScores retains MenuHUD's higher precedence.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Battle")
	bool bShowOverview = false;

	// WOTPlayer.ServerAssessBattlefield (fired from the UseAngreal gate while
	// bTeleportingDisabled and on a team): ask the authority whether the battlefield is
	// ready and open it if so. Routes to AWoTBattleGameMode::AssessBattlefield, which
	// re-checks budgets/editing server-side.
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerAssessBattlefield();

	// UE1 PlayerPawn.CenterMessage over the wire: the server pushes a center-screen HUD
	// message to the owning client (giMPBattle's BattleBeginStr etc.). bEcho is the
	// retail third parameter (BaseHUD.uc:933): also copy the text to the plain
	// console queue — the seal possession/capture notifications pass true.
	UFUNCTION(Client, Reliable)
	void ClientCenterMessage(const FString& Text, bool bEcho = false);

	// WOTPlayer.LeftMessage over the wire (WOTPlayer.uc:1787-1794 → BaseHUD.AddLeftMessage):
	// the left notification queue — giMPBattle's team-change rejects and the join/leave
	// broadcasts (giMPBattle.uc:330/337/357/362) ride this.
	UFUNCTION(Client, Reliable)
	void ClientLeftMessage(const FString& Text);

	// WOTPlayer.HandMessage: owner-only lock/unlock presentation with the
	// authored lifetime preserved.
	UFUNCTION(Client, Reliable)
	void ClientHandMessage(const FString& Message, float Duration);

	// WOTPlayer.CantChangeTeamsStr (WOTPlayer.uc:313; original/System/WoT.int:48):
	// the ServerChangeTeam while-editing refusal message.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle")
	FString CantChangeTeamsStr;

	// WOTPlayer.ClientMessage (WOTPlayer.uc:1862-1890) over the wire: a typed message the
	// owning client routes to the matching HUD channel — 'Pickup'/'FailedMessage' → hand
	// message, 'WOTDeathMessage' → right queue (MP only), default → console.
	UFUNCTION(Client, Reliable)
	void ClientTypedMessage(const FString& S, FName Type);
	// The routing body, split out with an explicit standalone flag so the headless spec
	// can exercise the MP-only death-message gate (a test world is always standalone).
	void RouteTypedMessage(const FString& S, FName Type, bool bStandalone);

	// AngrealInventory.uc:615-616 GiveTo: the first-new-artifact owner presentation —
	// ClientPlaySound(NotifyNewPickupSoundName) + BaseHUD.SetFlashTexture(texture, fade).
	UFUNCTION(Client, Reliable)
	void ClientNotifyNewPickup(FName FlashTextureId, float FadeTime, FName SoundSubPath);

	// In the GameEnded state (match decided).
	bool IsMatchEnded() const { return bMatchEnded; }
	// The PlayerRestartGameDelay freeze has lifted; a fire will now restart the match
	// (WOTPlayer clears bFrozen in the GameEnded-state Timer).
	bool IsRestartAllowed() const { return bRestartAllowed; }

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	friend class UWoTGamepadLayer;
#if WITH_AUTOMATION_TESTS
	TOptional<bool> JoystickEnabledOverrideForTest;
#endif
	/** The controller (gamepad) layer; local controllers only. */
	UPROPERTY(Transient)
	TObjectPtr<class UWoTGamepadLayer> GamepadLayer;
	void HandlePauseRequest(ENetMode NetMode);
	float SavedCameraFOV = 90.0f;
	float CameraFOVForTest = 90.0f;
	bool bHasSavedCameraFOV = false;
	TWeakObjectPtr<class AWoTCharacter> OwnerLocalZoneFOVPawn;
	void ReconcileOwnerLocalZoneFOVPawn(APawn* NewPawn);
	float LastTeleportProhibitedMessageTime = 0.0f;

	void TickUE1ViewShake(float DeltaTime);
	void TickUE1ViewFlash(float DeltaTime);
	void TickRetailCamera(float DeltaTime);
	void ResetRetailCameraState(APawn* NewPawn);
	FVector UE1WalkBob = FVector::ZeroVector;
	float UE1BobTime = 0.0f;
	float UE1LandBob = 0.0f;
	float UE1AppliedBob = 0.0f;
	float UE1EyeHeight = 42.0f;
	FVector UE1PreviousPawnLocation = FVector::ZeroVector;
	TEnumAsByte<EMovementMode> UE1PreviousMovementMode = MOVE_None;
	TWeakObjectPtr<APawn> UE1CameraPawn;
	void CreateInGameMenuWidget();
	void RemoveInGameMenuWidget(bool bRestoreGameInput);
	void BeginChatInput(const FString& InitialText);
	void CreateChatInputWidget();
	void RemoveChatInputWidget(bool bRestoreGameInput);
	void BroadcastSay(const FString& Message);
	void ServerEditCitadelInput();
	void CreateHelpWidget(const struct FWoTHelpContent& Content);
	void RemoveHelpWidget(bool bRestoreGameInput = true);
	AWoTInventory* GetSelectedItemForInfo() const;
	void CreateItemInfoWidget(AWoTInventory* Item);
	void RemoveItemInfoWidget(bool bCloseWindow = true);
	void BindProgressFailureDelegates();
	void UnbindProgressFailureDelegates();
	void ClearTravelProgressOnArrival();

	void EnterPlayerState(EWoTPlayerStateMachineState NewState);
	void ExitPlayerState(EWoTPlayerStateMachineState OldState);
	void TickPlayerState(float DeltaTime);
	void ApplyPlayerStateEntryPresentation(EWoTPlayerStateMachineState NewState);
	void OnDeathScoreboardAutoShowElapsed();
	UFUNCTION()
	void OnRep_PlayerStateMachineState(EWoTPlayerStateMachineState OldState);

	void ApplyOwnerCitadelEditState();
	void ApplyCitadelEditCameraState();
	void CaptureCitadelEditStart(EWoTCitadelEditCameraMode Mode);
	void RestoreCitadelEditStart();
	void ClearLocalCitadelEditStateForTeardown();
	bool HasValidLocalCitadelCreateZone() const;
	bool HasValidLocalCitadelPresentationZone() const;
	void ClearCitadelPresentationActors();
	// Owner-local ServerBegin/EndSelect (WOTPlayer.uc:5059-5091, driven per HUD tick by
	// EditorHUD.SyncTempResourceWithCursor): stamp every toggle-resource root in the
	// session team's zones as palette-selected (traceable) or not. Runs on the machine
	// that traces — the owner; the server's action gates never trace.
	void RefreshCitadelSelectTraceability();
	void DestroyCitadelGhostPreview();
	void CreateCitadelGhostPreview(const FWoTResourceDescriptor& Descriptor);
	void ApplyCitadelResourcePresentation(class AWoTTrap* Resource,
		bool bActive, bool bHighlight);

	FName SelectedCitadelAdapter = NAME_None;
	TArray<TWeakObjectPtr<class AWoTTrap>> RevealedCitadelResources;
	TWeakObjectPtr<class AWoTTrap> HoveredCitadelResource;
	TMap<TWeakObjectPtr<class AWoTTrap>, TWeakObjectPtr<AActor>> CitadelResourceMarkers;
	bool bCitadelPresentationZoneValid = false;
	TWeakObjectPtr<AActor> CitadelGhostPreviewActor;
	TWeakObjectPtr<class AWoTTrap> CitadelGhostPreview;
	TWeakObjectPtr<UClass> CitadelGhostResourceClass;
	FName CitadelGhostCreatureRow = NAME_None;
	bool bCitadelGhostPlacementValid = false;
	FVector EditStartLoc = FVector::ZeroVector;
	FRotator EditStartRot = FRotator::ZeroRotator;
	UFUNCTION(Server, Reliable)
	void ServerBeginCitadelEdit(int32 Mode);
	UFUNCTION(Server, Reliable)
	void ServerRequestCitadelEdit(int32 Mode);
	UFUNCTION(Server, Reliable)
	void ServerEndCitadelEdit();
	UFUNCTION(Client, Reliable)
	void ClientRestoreCitadelEditTransform(FVector SavedLocation,
		FRotator SavedRotation);

	// GameEnded-state freeze lift (WOTPlayer clears bFrozen here).
	void OnRestartDelayElapsed();

	// WOTPlayer.GameEnded.BeginState sets bFrozen — freeze the owning client's movement
	// input while the match is decided (the fire→restart re-route lives on AWoTCharacter).
	// Applied on the authority in NotifyMatchEnded and on remote clients via OnRep_MatchEnded
	// (the port drives it off the replicated flag instead of a dedicated ClientGameEnded RPC).
	// Look input stays free so a frozen player can still look around (minor divergence).
	void ApplyMatchEndedInput();
	// PlayerPawn.GameEnded.BeginState collision/PHYS_None body, shared by the authority
	// transition and the owning client's replicated ClientGameEnded-equivalent path.
	void FreezeMatchEndedPawn();
	// Owner-side reaction to the replicated ended flag (freeze input / scoreboard).
	UFUNCTION()
	void OnRep_MatchEnded();

	// Replicated so the client can gate the restart-on-fire input / show the scoreboard.
	UPROPERTY(ReplicatedUsing = OnRep_MatchEnded) bool bMatchEnded = false;
	TWeakObjectPtr<AActor> EndCameraViewTarget;
	bool bEndCameraUsesExternalViewTarget = false;
	// Owner-only state mirror: unlike the pawn, the controller remains available for input
	// between death and respawn.
	UPROPERTY(ReplicatedUsing = OnRep_PlayerStateMachineState)
	EWoTPlayerStateMachineState PlayerStateMachineState =
		EWoTPlayerStateMachineState::Playing;
	// Authority-only WOTPlayer.OKToFireTime and the body replaced by RestartPlayer.
	float OKToFireTime = 0.0f;
	TWeakObjectPtr<AWoTCharacter> RespawnCorpse;
	// Server-authoritative: the fire-to-restart gate. Not replicated (the server RPC is the
	// authority on whether a restart is allowed).
	bool bRestartAllowed = false;
	bool bDeathScoreboardAutoShowActive = false;
	FWoTInGameMenuModel InGameMenu;
	TSharedPtr<FWoTFrontEndPresentationAuthority>
		InGameMenuPresentationAuthority;
	TSharedPtr<SWoTInGameMenuOverlay> InGameMenuWidget;
	TWeakObjectPtr<UGameViewportClient> InGameMenuViewport;
	TSharedPtr<SWoTChatInputOverlay> ChatInputWidget;
	TWeakObjectPtr<UGameViewportClient> ChatInputViewport;
	FString ChatInputText;
	bool bChatTyping = false;
#if WITH_AUTOMATION_TESTS
	bool bEnableChatWidgetForTesting = false;
#endif
	TSharedPtr<FWoTChatInputPreProcessor> ChatInputPreProcessor;
	TSharedPtr<FWoTFrontEndPresentationAuthority> HelpPresentationAuthority;
	TSharedPtr<SWoTHelpOverlay> HelpWidget;
	TWeakObjectPtr<UGameViewportClient> HelpViewport;
	bool bHelpOpen = false;
	float NextTauntTime = 0.0f;
	FName LastTauntSlotForTesting = NAME_None;
	TWeakObjectPtr<AWoTInventory> ItemInfoItem;
	TSharedPtr<SWoTItemInfoOverlay> ItemInfoWidget;
	TWeakObjectPtr<UGameViewportClient> ItemInfoViewport;
	bool bItemInfoOpen = false;

	UPROPERTY(Transient)
	FString ProgressMessage[8];
	UPROPERTY(Transient)
	FLinearColor ProgressColor[8];
	UPROPERTY(Transient)
	float ProgressTimeOut = 0.0f;
	FDelegateHandle ProgressNetworkFailureHandle;
	FDelegateHandle ProgressTravelFailureHandle;

	FTimerHandle RestartDelayTimer;
	FTimerHandle DeathScoreboardAutoShowTimer;
};
