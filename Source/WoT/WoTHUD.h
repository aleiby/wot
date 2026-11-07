#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "WoTCitadelTeamOverview.h"
#include "WoTHUDDrawCmd.h"
#include "WoTScoreboard.h"
#include "WoTHUD.generated.h"

class UCanvas;
class UFont;
class USoundBase;
class UWoTHUDFonts;
class UWoTHUDTextures;
class UWoTHandSet;
class APawn;
class AWoTPlayerController;
struct FWoTStatusIconEntry;

UENUM()
enum class EWoTHUDMode : uint8
{
	Normal,
	CitadelEditor
};

// uiConsole.InterfaceLevel / BaseHUD's synchronized presentation level
// (uiConsole.uc:63-64,397-443; BaseHUD.uc:32-35,57,229).
UENUM()
enum class EWoTInterfaceLevel : uint8
{
	Normal = 0,
	Minimized = 1,
	UltraMinimized = 2
};

// Pure census result for the shipped BaseHUD/MainHUD consumers. InterfaceLevel
// changes hand layouts and the added-item transient only. A WOTWindow hides the
// four bHide* surfaces while leaving hands/progress/crosshair drawing live.
struct FWoTHUDSurfaceVisibility
{
	bool bHealth = true;
	bool bStatusIcons = true;
	bool bKeys = true;
	bool bMessages = true;
	bool bHands = true;
	bool bProgress = true;
	bool bAddedItems = true;
};

struct FWoTEditorCursorGeometry
{
	FVector2D CursorPoint = FVector2D::ZeroVector;
	FVector2D TopLeft = FVector2D::ZeroVector;
	FIntPoint Size = FIntPoint::ZeroValue;
	FIntPoint Hotspot = FIntPoint::ZeroValue;
};

enum class EWoTTurtleFlash : uint8
{
	None,
	Turtle,
	TurtleDead
};

USTRUCT()
struct FWoTHUDMessageData
{
	GENERATED_BODY()

	UPROPERTY()
	FString Message;

	UPROPERTY()
	float LifeSpan = 0.0f;
};

USTRUCT()
struct FWoTHUDGenericMessageData
{
	GENERATED_BODY()

	UPROPERTY()
	FString Message;

	UPROPERTY()
	int32 X = 0;

	UPROPERTY()
	int32 Y = 0;

	UPROPERTY()
	bool bCenter = false;

	UPROPERTY()
	uint8 Intensity = 0;

	UPROPERTY()
	TObjectPtr<UFont> Font = nullptr;

	UPROPERTY()
	float LifeSpan = 0.0f;
};

// A pure snapshot of everything the debug HUD draws, assembled from the local
// player's already-replicated state (health, selected artifact/charges, score,
// match clock, seal counts, ping, match-over). Split out from DrawHUD so the data
// assembly is unit-testable headless — there is nothing to assert about pixels, so
// the spec exercises this instead. Every field here READS local state only; the HUD
// adds no replication (all of it is already on the wire).
USTRUCT()
struct FWoTHUDReadout
{
	GENERATED_BODY()

	// Local pawn (AWoTCharacter). bHasPawn is false between death and respawn.
	bool bHasPawn = false;
	int32 Health = 0;
	int32 MaxHealth = 0;

	// Breath (owner-only BreathDisplay mirror): -1 = head dry (no line), N = whole
	// seconds of breath left, 0 = drowning (banner).
	int32 Breath = -1;

	// Selected artifact display name ("" = no selection) and its charges
	// (-1 = the selection isn't a charged angreal, so no count is shown).
	FString Artifact;
	int32 Charges = -1;

	// Score / team / ping from the owning AWoTPlayerState.
	bool bHasScore = false;
	int32 Kills = 0;
	int32 Deaths = 0;
	int32 Team = 255;
	int32 Ping = 0;

	// Match clock pre-formatted "M:SS" ("" = no GameState yet). bDisplayRemaining
	// selects the countdown vs the elapsed clock (mirrors GameState.bDisplayRemaining).
	FString Clock;
	bool bDisplayRemaining = false;

	// Match decided (the owner's WOTPlayer GameEnded latch) — the HUD shows a banner.
	bool bMatchEnded = false;

	// Battle/citadel only: seals held per team (0..3) + the per-team win target.
	bool bHasSeals = false;
	int32 NumTeams = 0;
	int32 SealGoal = 0;
	int32 Seals[4] = { 0, 0, 0, 0 };

	bool operator==(const FWoTHUDReadout& Other) const
	{
		return bHasPawn == Other.bHasPawn && Health == Other.Health
			&& MaxHealth == Other.MaxHealth && Breath == Other.Breath
			&& Artifact == Other.Artifact && Charges == Other.Charges
			&& bHasScore == Other.bHasScore && Kills == Other.Kills
			&& Deaths == Other.Deaths && Team == Other.Team && Ping == Other.Ping
			&& Clock == Other.Clock && bDisplayRemaining == Other.bDisplayRemaining
			&& bMatchEnded == Other.bMatchEnded && bHasSeals == Other.bHasSeals
			&& NumTeams == Other.NumTeams && SealGoal == Other.SealGoal
			&& Seals[0] == Other.Seals[0] && Seals[1] == Other.Seals[1]
			&& Seals[2] == Other.Seals[2] && Seals[3] == Other.Seals[3];
	}
};

// The native art HUD. It draws health + crosshair and retains the debug text
// readout behind wot.HUDDebug; the other HUD surfaces build on this same AHUD.
UCLASS()
class AWoTHUD : public AHUD
{
	GENERATED_BODY()

public:
	AWoTHUD();

	// Shipped out-of-box default (original/System/DefUser.ini:215 [Engine.HUD]
	// Crosshair=13 -> the Crosshair14 art). Still user-config, as in retail.
	UPROPERTY(config)
	int32 Crosshair = 13;
	// Deliberately persistent (config) on the HUD. Shipped uiConsole
	// owns the value but does not mark it config; PORT_NOTES records that mismatch.
	UPROPERTY(config)
	EWoTInterfaceLevel InterfaceLevel = EWoTInterfaceLevel::Normal;

	static constexpr int32 NumCrossHairs = 21;
	static constexpr int32 NumLeftMessages = 10;
	static constexpr int32 NumRightMessages = 10;
	static constexpr int32 NumCenterMessages = 2;
	static constexpr int32 NumGenericMessages = 16;

	UPROPERTY()
	FWoTHUDMessageData HandMessage;

	UPROPERTY()
	FWoTHUDMessageData LeftMessages[10];

	UPROPERTY()
	FWoTHUDMessageData RightMessages[10];

	UPROPERTY()
	FWoTHUDMessageData CenterMessages[2];

	UPROPERTY()
	FWoTHUDMessageData SubtitleMessage;

	UPROPERTY()
	FWoTHUDGenericMessageData GenericMessages[16];

	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float HandMessageDuration = 1.5f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float LeftMessageDuration = 10.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float RightMessageDuration = 10.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float CenterMessageDuration = 5.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float SubtitleMessageDuration = 5.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float GenericMessageDuration = 5.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float HandMessageFadePercent = 0.5f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float LeftMessageFadePercent = 0.2f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float RightMessageFadePercent = 0.2f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float CenterMessageFadePercent = 0.25f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float SubtitleMessageFadePercent = 0.25f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	float GenericMessageFadePercent = 0.25f;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	uint8 HandMessageIntensity = 255;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	uint8 LeftMessageIntensity = 255;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	uint8 RightMessageIntensity = 255;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	uint8 CenterMessageIntensity = 255;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	uint8 SubtitleMessageIntensity = 255;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	uint8 GenericMessageIntensity = 255;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	TObjectPtr<USoundBase> HandMessageSound = nullptr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	TObjectPtr<USoundBase> CenterMessageSound = nullptr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	TObjectPtr<USoundBase> GenericMessageSound = nullptr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	FString LeftMessageName = TEXT("WOT.Interface.LeftNotify");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	FString RightMessageName = TEXT("WOT.Interface.RightNotify");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	TObjectPtr<USoundBase> LeftMessageSound = nullptr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Messages")
	TObjectPtr<USoundBase> RightMessageSound = nullptr;

	UPROPERTY(EditAnywhere, Category = "WoT|HUD|Messages")
	bool bHideMessages = false;
	// BaseHUD.bHideStatusIcons is not authored in defaultproperties, so False.
	UPROPERTY(EditAnywhere, Category = "WoT|HUD")
	bool bHideStatusIcons = false;

	// BaseHUD's final native extras. Values mirror BaseHUD.defaultproperties.
	UPROPERTY(config, EditDefaultsOnly, Category = "WoT|HUD|Extras")
	bool bTurtleEnabled = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Extras")
	float TurtleThresholdSP = 0.05f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Extras")
	float TurtleThresholdMP = 0.05f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Extras")
	float DeadTurtleThresholdSP = 0.083333f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Extras")
	float DeadTurtleThresholdMP = 0.066666f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Extras")
	float TurtleFadeTime = 1.0f;

	UPROPERTY(config, EditDefaultsOnly, Category = "WoT|HUD|Player ID")
	bool bPlayerIDEnabled = true;
	UPROPERTY(config, EditDefaultsOnly, Category = "WoT|HUD|Player ID")
	float PlayerIDTraceLen = 32767.0f;
	UPROPERTY(config, EditDefaultsOnly, Category = "WoT|HUD|Player ID",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PlayerIDYOffsetRatio = 0.667f;

	UPROPERTY(EditAnywhere, Category = "WoT|HUD|Extras")
	bool bWarningEnabled = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Extras")
	float TimeOut = 1.5f;

	// BattleHUD's shipped localized fragments. Keeping these as FText preserves
	// the original per-fragment localization surface while the formatter below
	// supplies the dynamic team bound and timeout.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Team Select")
	FText SelectTeamStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Team Select")
	FText TimeLimitStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Team Select")
	FText SecondsStr;

	// CitadelTeamOverview localized strings (original/System/WoT.int:35-42).
	// The dynamic team descriptions/status values remain on replicated BattleInfo.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Scoreboard")
	FText CitadelOverviewTeamStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Scoreboard")
	FText CitadelOverviewPlayersStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Scoreboard")
	FText CitadelOverviewStatusStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Scoreboard")
	FText CitadelOverviewBeginBattleStr;

	UPROPERTY(Transient)
	FString IdentifyTarget;
	UPROPERTY(Transient)
	float IdentifyFadeTime = 0.0f;
	UPROPERTY(Transient)
	FName FlashTextureId;
	UPROPERTY(Transient)
	float FlashTextureTime = 0.0f;
	UPROPERTY(Transient)
	float FlashTextureTimer = 0.0f;

	UFUNCTION(BlueprintCallable, Category = "WoT|HUD|Messages")
	void AddHandMessage(const FString& MessageText, float Duration = 0.0f);
	UFUNCTION(BlueprintCallable, Category = "WoT|HUD|Messages")
	void AddLeftMessage(const FString& MessageText, float Duration = 0.0f);
	UFUNCTION(BlueprintCallable, Category = "WoT|HUD|Messages")
	void AddRightMessage(const FString& MessageText, float Duration = 0.0f);
	UFUNCTION(BlueprintCallable, Category = "WoT|HUD|Messages")
	void AddCenterMessage(const FString& MessageText, float Duration = 0.0f,
		bool bEcho = false);
	UFUNCTION(BlueprintCallable, Category = "WoT|HUD|Messages")
	void AddSubtitleMessage(const FString& MessageText, float Duration = 0.0f,
		bool bEcho = false);
	UFUNCTION(BlueprintCallable, Category = "WoT|HUD|Messages")
	void AddGenericMessage(const FString& MessageText, int32 MessageX, int32 MessageY,
		bool bCenter, uint8 Intensity, UFont* MessageFont = nullptr,
		float Duration = 0.0f);

	// Pure/state-only seams used by DrawMessages and the headless message spec.
	static float MessageFadeScale(float LifeSpan, float Duration, float FadePercent);
	static float GenericMessageCenterX(float ScaledX, float CanvasWidth);
	static TArray<FString> WrapMessageLines(const FString& SourceText, float MaxWidth,
		TFunctionRef<float(const FString&)> MeasureText);
	void TickMessages(float DeltaTime);
	bool ShouldDrawMessages(bool bHUDVisible) const;
	float UpdateMessageRenderTime(bool bHUDVisible, float CurrentTimeSeconds);

	UFUNCTION(Exec)
	void ChangeCrosshair(int32 Delta);

	static TArray<FWoTHUDDrawCmd> LayoutCrosshair(int32 Index, int32 ClipX, int32 ClipY);
	static TArray<FWoTHUDDrawCmd> LayoutHealth(
		const FWoTHUDReadout& Readout, FName HealthIconId, bool bSeeker);
	static int32 StatusTimerIndex(float InitialDuration, float RemainingDuration);
	static TArray<FWoTHUDDrawCmd> LayoutStatusIcons(
		const TArray<FWoTStatusIconEntry>& Entries, int32 ScaledSizeX);
	static TArray<FWoTHUDDrawCmd> LayoutHands(
		const UWoTHandSet* HandSet, int32 ScaledSizeX, int32 ScaledSizeY);
	static TArray<FWoTHUDDrawCmd> LayoutMinimizedHands(
		const UWoTHandSet* HandSet, int32 ScaledSizeX, int32 ScaledSizeY,
		EWoTInterfaceLevel Level, bool bCurrentHandDisplayActive,
		float TimeSeconds, float TriggerTime, float InTimeOut = 1.5f);
	static TArray<FWoTHUDDrawCmd> LayoutCurrentHand(
		const UWoTHandSet* HandSet, int32 ScaledSizeX, int32 ScaledSizeY,
		EWoTInterfaceLevel Level = EWoTInterfaceLevel::Normal);
	static TArray<FWoTHUDDrawCmd> LayoutAddedItems(
		const UWoTHandSet* HandSet, int32 ScaledSizeX, int32 ScaledSizeY,
		float TimeSeconds, float InTimeOut = 1.5f);
	static TArray<FWoTHUDDrawCmd> LayoutProgressMessages(
		const TArray<FWoTProgressEntry>& Entries, int32 ClipX, int32 ClipY,
		float FontLineHeight);
	static EWoTInterfaceLevel NextInterfaceLevel(EWoTInterfaceLevel Level);
	static EWoTInterfaceLevel PreviousInterfaceLevel(EWoTInterfaceLevel Level);
	static FWoTHUDSurfaceVisibility SurfaceVisibility(
		EWoTInterfaceLevel Level, bool bItemInfoOpen);
	static bool ShouldDrawHUD(bool bHUDEnabled, bool bInSeamlessTravel,
		bool bOwningMenuOpen);
	void SetInterfaceLevel(EWoTInterfaceLevel NewLevel, bool bSave = true);
	EWoTInterfaceLevel GetInterfaceLevel() const { return InterfaceLevel; }
	static constexpr int32 EditorCursorSize = 16;
	static constexpr int32 EditorCursorHotspot = 7;
	static FWoTEditorCursorGeometry EditorCursorGeometry(int32 ClipX, int32 ClipY);
	static TArray<FWoTHUDDrawCmd> LayoutEditorCursor(int32 ClipX, int32 ClipY);
	static bool ShouldDrawHealth(EWoTHUDMode Mode)
	{
		return Mode == EWoTHUDMode::Normal;
	}
	EWoTHUDMode GetHUDMode() const { return HUDMode; }
	bool IsEditorMode() const { return HUDMode == EWoTHUDMode::CitadelEditor; }

	// Pure decisions. Runtime drawing/input are thin consumers of these seams.
	/** bControllerHint also names the D-pad buttons that choose each team. */
	FString TeamSelectPrompt(uint8 PlayerTeam, int32 NumTeams,
		int32 JoinTeamTimeout, bool bControllerHint = false) const;
	static bool ShouldInterceptTeamDigit(uint8 PlayerTeam, int32 NumTeams,
		int32 TeamIndex);
	bool HandleTeamSelectDigit(int32 TeamIndex);
	/** True while the owner is on a Battle map without a team (the 1-N prompt is up). */
	bool IsChoosingTeam() const;
	/** The Battle map's team count; zero off Battle maps. */
	int32 BattleTeamCount() const;
	static FString PlayerIDName(const APawn* HitPawn);
	// Public: the PlayerID spec drives the real trace (object-type pawn find + world
	// occlusion) headlessly with a null canvas and asserts IdentifyTarget/FadeTime.
	void UpdatePlayerID(UCanvas* CanvasObject, float DeltaTime);
	static EWoTTurtleFlash ClassifyTurtle(float DeltaTime, bool bStandalone,
		float SlowThresholdSP, float SlowThresholdMP,
		float DeadThresholdSP, float DeadThresholdMP);
	static bool TimedExtraActive(float TriggerTime, float InTimeOut,
		float CurrentTime);
	static float FlashFade(float RemainingTime, float TotalTime);
	static int32 ScaledViewportWidth(int32 ViewportWidth);
	static float FlashTextureX(int32 ViewportWidth);
	static float WarningTextureX(int32 ViewportWidth);
	static FVector2D TeamSelectPosition(float ViewportWidth, float TextWidth,
		float TextHeight);
	static FLinearColor FadedHUDColor(float Opacity);

	UFUNCTION(BlueprintCallable, Category = "WoT|HUD|Extras")
	void SetWarning();
	UFUNCTION(BlueprintCallable, Category = "WoT|HUD|Extras")
	void ClientWarn();
	UFUNCTION(BlueprintCallable, Category = "WoT|HUD|Extras")
	void SetFlashTexture(FName TextureId, float Duration = 0.0f);
	UFUNCTION(Exec)
	void GetRidOfTurtle();
	UFUNCTION(Exec)
	void BringBackTurtle();
	UFUNCTION(Exec)
	void SetTurtleFadeTime(float InFadeTime);
	UFont* ResolveFont(FName Id, int32 ViewportWidth) const;
	UTexture2D* ResolveHUDTexture(FName Id) const;
	/** Guarded HUD-registry route of a texture id: /Game/WoT/UI/<Id>.<Id>,
	 * which the runtime resolver maps through its retail UI aliases. */
	static FSoftObjectPath RuntimeHUDTexturePath(FName Id);
	/** The single texture a draw command resolves in guarded builds: the bound
	 * item texture (StatusIcon/StatusIconFrame soft path) when present, else
	 * the HUD-registry id route. Never both. */
	static FSoftObjectPath CommandTexturePath(const FWoTHUDDrawCmd& Command);
	void NotifyHandSelection();
	bool IsCurrentHandDisplayActive(float CurrentTimeSeconds) const;

	// Assemble the read-out from the owning player's local (replicated) state.
	// Pure + side-effect-free so the spec can assert its values without a canvas.
	FWoTHUDReadout GatherReadout() const;

	// Scoreboard adapter seams. Gathering/visibility are exposed so their
	// world-facing mapping can be covered without a canvas.
	FWoTScoreboardSnapshot GatherArenaScoreboard() const;
	bool ShouldDrawArenaScoreboard() const;
	FWoTCitadelTeamOverviewSnapshot GatherCitadelTeamOverview() const;
	bool ShouldDrawCitadelTeamOverview() const;

	// Turn a read-out into the display lines DrawHUD paints (one string per row).
	static TArray<FString> FormatLines(const FWoTHUDReadout& Readout);

	virtual void DrawHUD() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

protected:
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD")
	TSoftObjectPtr<UWoTHUDTextures> HUDTextures;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD")
	TSoftObjectPtr<UWoTHUDFonts> HUDFonts;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Citadel Editor")
	TSoftObjectPtr<UTexture2D> EditorCursorTexture;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Citadel Editor")
	TSoftObjectPtr<USoundBase> BeginEditorSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD|Citadel Editor")
	TSoftObjectPtr<USoundBase> EndEditorSound;

	void DrawHealth(UCanvas* CanvasObject);
	void DrawStatusIcons(UCanvas* CanvasObject);
	void DrawCrosshair(UCanvas* CanvasObject);
	void DrawEditorCursor(UCanvas* CanvasObject);
	void DrawHands(UCanvas* CanvasObject);
	void DrawCurrentHand(UCanvas* CanvasObject);
	void DrawAddedItems(UCanvas* CanvasObject);
	void DrawProgressMessages(UCanvas* CanvasObject);
	void DrawMessages(UCanvas* CanvasObject, float DeltaTime);
	void DrawTeamSelect(UCanvas* CanvasObject);
	void UpdateTurtle(float DeltaTime);
	void DrawFlashTexture(UCanvas* CanvasObject, float DeltaTime);
	void DrawWarning(UCanvas* CanvasObject);
	void DrawArenaScoreboard(UCanvas* CanvasObject);
	void DrawCitadelTeamOverview(UCanvas* CanvasObject);
	void ExecuteHandCommands(UCanvas* CanvasObject,
		const TArray<FWoTHUDDrawCmd>& Commands);
	void DrawDebugReadout(UCanvas* CanvasObject);
	const TArray<FString>& GetCachedDebugReadoutLines();
	bool ShouldDrawWoTHUD() const;
	FWoTHUDSurfaceVisibility CurrentSurfaceVisibility() const;
	void BindEditorModeController();
	void UnbindEditorModeController();
	void HandleCitadelEditSessionChanged(bool bEditing, uint8 Team);
	void SetHUDMode(EWoTHUDMode NewMode, bool bPlaySound);

	static constexpr float HandDisplayTime = 1.5f;
	float HandSelectionTime = -2.0f * HandDisplayTime;
	float LastMessageRenderTime = 0.0f;
	float TriggerWarningTime = 0.0f;
	UPROPERTY(Transient)
	EWoTHUDMode HUDMode = EWoTHUDMode::Normal;
	bool bDebugReadoutCacheValid = false;
	FWoTHUDReadout CachedDebugReadout;
	TArray<FString> CachedDebugReadoutLines;
	TWeakObjectPtr<AWoTPlayerController> BoundEditorModeController;
	FDelegateHandle EditorModeDelegateHandle;
};
