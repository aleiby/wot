#include "WoTHUD.h"
#include "AI/WoTPathNodeIterator.h"

#include "Assets/WoTRetailIdentity.h"
#include "Assets/WoTRuntimeAssets.h"
#include "WoTCanvasScale.h"
#include "WoTCharacter.h"
#include "WoTHUDFonts.h"
#include "WoTHUDTextures.h"
#include "WoTPlayerController.h"
#include "WoTPlayerState.h"
#include "WoTPreferencesSubsystem.h"
#include "WoTRetailText.h"
#include "WoTGameState.h"
#include "WoTScoreboard.h"
#include "WoTCitadelTeamOverview.h"
#include "WoTCitadelGameState.h"
#include "WoTBattleInfo.h"
#include "WoTSound.h"
#include "Inventory/WoTInventory.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTHandInfo.h"
#include "Inventory/WoTHandSet.h"
#include "World/WoTMapDisplay.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "CanvasItem.h"
#include "CollisionQueryParams.h"
#include "HAL/IConsoleManager.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Math/ScaleMatrix.h"
#include "Sound/SoundBase.h"

#define LOCTEXT_NAMESPACE "WoTHUD"

namespace
{
	constexpr int32 WoTHUD_IconWidth = 64;
	constexpr int32 WoTHUD_IconHeight = 64;
	constexpr int32 WoTHUD_NotchWidth = 8;

	const AWoTInventory* WoTHUD_FindInventoryDefault(FName OriginalClassName)
	{
		// The shared name-cached resolver: this is called per populated hand per
		// frame (LayoutHands/LayoutCurrentHand/LayoutAddedItems), and the raw
		// TObjectIterator<UClass> scan it replaced was HUD-frame hot.
		return AWoTInventory::FindDefaultByOriginalName(OriginalClassName);
	}

	const AWoTInventory* WoTHUD_ItemOrDefault(
		const UWoTHandInfo* Hand, int32 Slot)
	{
		if (!Hand)
		{
			return nullptr;
		}
		if (const AWoTInventory* Item = Hand->GetItem(Slot))
		{
			return Item;
		}
		return WoTHUD_FindInventoryDefault(Hand->GetClassName(Slot));
	}

	int32 WoTHUD_DisplaySlot(const UWoTHandInfo* Hand)
	{
		if (!Hand)
		{
			return INDEX_NONE;
		}
		if (!Hand->GetSelectedClassName().IsNone())
		{
			return Hand->Selected;
		}
		// BaseHUD.DrawMinimizedHands temporarily SelectsForward when the current
		// slot is empty. Resolve the same display slot without mutating gameplay.
		for (int32 Slot = 0; Slot < UWoTHandInfo::NumSlots; ++Slot)
		{
			if (!Hand->GetClassName(Slot).IsNone())
			{
				return Slot;
			}
		}
		return INDEX_NONE;
	}

	void WoTHUD_SetTexture(FWoTHUDDrawCmd& Command,
		const TSoftObjectPtr<UTexture2D>& Texture)
	{
		Command.Texture = Texture;
		Command.TexId = Texture.IsNull()
			? NAME_None : FName(*Texture.GetAssetName());
	}

	void WoTHUD_SetMessage(FWoTHUDMessageData& Entry, const FString& MessageText,
		float Duration, float DefaultDuration)
	{
		Entry.Message = MessageText;
		Entry.LifeSpan = Duration > 0.0f ? Duration : DefaultDuration;
	}

	void WoTHUD_AdvanceMessage(FWoTHUDMessageData& Entry, float DeltaTime)
	{
		if (Entry.LifeSpan > 0.0f)
		{
			Entry.LifeSpan = FMath::Max(Entry.LifeSpan - DeltaTime, 0.0f);
		}
	}

	void WoTHUD_AdvanceGenericMessage(FWoTHUDGenericMessageData& Entry,
		float DeltaTime)
	{
		if (Entry.LifeSpan > 0.0f)
		{
			Entry.LifeSpan = FMath::Max(Entry.LifeSpan - DeltaTime, 0.0f);
		}
	}

	FName WoTHUD_SoundSubPath(const FString& UE1SoundName)
	{
		// import_sounds.py flattens Sound'Pkg.Group.Name' to Pkg/Name; a name
		// that collides inside its package keeps its groups when the guarded
		// registry holds that exact identity (WoTRetailIdentity.h).
		FWoTRetailObjectPath Path;
		if (!WoTParseRetailObjectPath(UE1SoundName, Path))
		{
			return NAME_None;
		}
		return FName(*WoTRetailAssetSubPath(EWoTNeutralResourceKind::Sound, Path));
	}

	struct FWoTHUDIntegerUpscaleGuard
	{
		explicit FWoTHUDIntegerUpscaleGuard(
			UCanvas& InCanvasObject, int32 InUpscale)
			: CanvasObject(InCanvasObject)
			, SavedDimensions(FWoTCanvasScale::ApplyLogicalDimensions(
				&InCanvasObject, InUpscale))
		{
			if (InUpscale > 1 && CanvasObject.Canvas)
			{
				CanvasObject.Canvas->PushRelativeTransform(FScaleMatrix(
					FVector(InUpscale, InUpscale, 1)));
				bPushedTransform = true;
			}
		}

		~FWoTHUDIntegerUpscaleGuard()
		{
			if (bPushedTransform)
			{
				CanvasObject.Canvas->PopTransform();
			}
			FWoTCanvasScale::RestoreDimensions(&CanvasObject, SavedDimensions);
		}

		UCanvas& CanvasObject;
		FWoTCanvasDimensions SavedDimensions;
		bool bPushedTransform = false;
	};
}

FString AWoTHUD::TeamSelectPrompt(uint8 PlayerTeam, int32 NumTeams,
	int32 JoinTeamTimeout, bool bControllerHint) const
{
	if (PlayerTeam != 255 || NumTeams <= 0)
	{
		return FString();
	}
	FText Prompt = FText::Format(
		LOCTEXT("TeamSelectBoundFormat", "{0} (1-{1})"),
		SelectTeamStr, FText::AsNumber(NumTeams));
	if (bControllerHint)
	{
		// The D-pad order of the controller's TeamSelect context: teams 1-4.
		static const TCHAR* const Buttons[] = {
			TEXT("Left"), TEXT("Left/Right"), TEXT("Left/Right/Up"), TEXT("Left/Right/Up/Down")};
		Prompt = FText::Format(
			LOCTEXT("TeamSelectControllerFormat", "{0} (1-{1} or D-pad {2})"),
			SelectTeamStr, FText::AsNumber(NumTeams),
			FText::FromString(Buttons[FMath::Clamp(NumTeams, 1, 4) - 1]));
	}
	if (JoinTeamTimeout > 0)
	{
		Prompt = FText::Format(
			LOCTEXT("TeamSelectTimeoutFormat", "{0}{1}{2}{3}"),
			Prompt, TimeLimitStr, FText::AsNumber(JoinTeamTimeout), SecondsStr);
	}
	return Prompt.ToString();
}

bool AWoTHUD::ShouldInterceptTeamDigit(uint8 PlayerTeam, int32 NumTeams,
	int32 TeamIndex)
{
	// BattleHUD.KeyEvent only handles IK_1..IK_4 even if invalid map data
	// advertises more teams.
	return PlayerTeam == 255 && TeamIndex >= 0 &&
		TeamIndex < FMath::Min(NumTeams, 4);
}

int32 AWoTHUD::BattleTeamCount() const
{
	if (const UWorld* HUDWorld = GetWorld())
	{
		for (TActorIterator<AWoTBattleInfo> It(HUDWorld); It; ++It)
		{
			if (IsValid(*It))
			{
				return It->NumTeams;
			}
		}
	}
	return 0;
}

bool AWoTHUD::IsChoosingTeam() const
{
	const APlayerController* Owner = GetOwningPlayerController();
	const AWoTPlayerState* WoTState = Owner ? Owner->GetPlayerState<AWoTPlayerState>() : nullptr;
	return WoTState && WoTState->Team == 255 && BattleTeamCount() > 0;
}

bool AWoTHUD::HandleTeamSelectDigit(int32 TeamIndex)
{
	AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetOwningPlayerController());
	const AWoTPlayerState* WoTState = WoTController
		? WoTController->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!WoTController || !WoTState ||
		!ShouldInterceptTeamDigit(WoTState->Team, BattleTeamCount(), TeamIndex))
	{
		return false;
	}

	// BattleHUD.KeyEvent (BattleHUD.uc:63-64) sends both requests in this order. The
	// restart must be the WoT RPC: the engine's retained ServerRestartPlayer no-ops for
	// a live pawn, while PlayerPawn.ServerRestartPlayer restarts unconditionally — the
	// Battle mode then spawns the faction pawn class at a team start.
	WoTController->ServerChangeTeam(TeamIndex);
	WoTController->ServerRestartPlayerWoT();
	return true;
}

FString AWoTHUD::PlayerIDName(const APawn* HitPawn)
{
	if (!HitPawn || HitPawn->IsHidden())
	{
		return FString();
	}
	// Remote PlayerControllers do not exist on unrelated clients, so
	// IsPlayerControlled() would reject valid network players. PlayerState is the
	// replicated UE5 equivalent of the shipped bIsPlayer + PRI test.
	const APlayerState* HitState = HitPawn->GetPlayerState();
	return HitState ? HitState->GetPlayerName() : FString();
}

EWoTTurtleFlash AWoTHUD::ClassifyTurtle(float DeltaTime, bool bStandalone,
	float SlowThresholdSP, float SlowThresholdMP,
	float DeadThresholdSP, float DeadThresholdMP)
{
	const float SlowThreshold = bStandalone ? SlowThresholdSP : SlowThresholdMP;
	const float DeadThreshold = bStandalone ? DeadThresholdSP : DeadThresholdMP;
	if (DeltaTime > DeadThreshold)
	{
		return EWoTTurtleFlash::TurtleDead;
	}
	if (DeltaTime > SlowThreshold)
	{
		return EWoTTurtleFlash::Turtle;
	}
	return EWoTTurtleFlash::None;
}

bool AWoTHUD::TimedExtraActive(float TriggerTime, float InTimeOut,
	float CurrentTime)
{
	return CurrentTime - TriggerTime < InTimeOut;
}

float AWoTHUD::FlashFade(float RemainingTime, float TotalTime)
{
	return TotalTime > 0.0f
		? FMath::Clamp(RemainingTime / TotalTime, 0.0f, 1.0f) : 0.0f;
}

int32 AWoTHUD::ScaledViewportWidth(int32 ViewportWidth)
{
	return ViewportWidth >= 640 ? ViewportWidth : 640;
}

float AWoTHUD::FlashTextureX(int32 ViewportWidth)
{
	return static_cast<float>(ScaledViewportWidth(ViewportWidth)) - 64.0f;
}

float AWoTHUD::WarningTextureX(int32 ViewportWidth)
{
	return (static_cast<float>(ScaledViewportWidth(ViewportWidth)) - 16.0f) * 0.5f;
}

FVector2D AWoTHUD::TeamSelectPosition(float ViewportWidth, float TextWidth,
	float TextHeight)
{
	return FVector2D((ViewportWidth - TextWidth) * 0.5f, 3.0f * TextHeight);
}

FLinearColor AWoTHUD::FadedHUDColor(float Opacity)
{
	// UE1 STY_Translucent treated black as no contribution. UE5's translucent
	// canvas path is source-alpha blending, so preserve white and fade alpha.
	return FLinearColor(1.0f, 1.0f, 1.0f, FMath::Clamp(Opacity, 0.0f, 1.0f));
}

static TAutoConsoleVariable<int32> CVarWoTHUDDebug(
	TEXT("wot.HUDDebug"),
	0,
	TEXT("Draw the legacy text HUD readout over the art HUD."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarWoTHUDScale(
	TEXT("wot.HUDScale"),
	0,
	TEXT("Integer HUD scale: 0 auto, 1 original 1999 size, N forced (clamped to 640x480)."),
	ECVF_Default);

AWoTHUD::AWoTHUD()
{
	HUDTextures = TSoftObjectPtr<UWoTHUDTextures>(FSoftObjectPath(
		TEXT("/Game/WoT/UI/DA_WoTHUDTextures.DA_WoTHUDTextures")));
#if !WOT_GUARDED_BOOTSTRAP
	HUDFonts = TSoftObjectPtr<UWoTHUDFonts>(FSoftObjectPath(
		TEXT("/Game/WoT/UI/Fonts/DA_WoTHUDFonts.DA_WoTHUDFonts")));
#endif
	EditorCursorTexture = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		TEXT("/Game/WoT/Textures/WOT/UI_CECursor.UI_CECursor")));
	BeginEditorSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/WOT/BeginEdit.BeginEdit")));
	EndEditorSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/WOT/EndEdit.EndEdit")));
	SelectTeamStr = LOCTEXT("SelectTeamStr", "Choose a side:");
	TimeLimitStr = LOCTEXT("TimeLimitStr", " within ");
	SecondsStr = LOCTEXT("SecondsStr", " sec");
	CitadelOverviewTeamStr = LOCTEXT("CitadelOverviewTeamStr", "Side");
	CitadelOverviewPlayersStr = LOCTEXT(
		"CitadelOverviewPlayersStr", "Members (limit)");
	CitadelOverviewStatusStr = LOCTEXT("CitadelOverviewStatusStr", "State");
	CitadelOverviewBeginBattleStr = LOCTEXT(
		"CitadelOverviewBeginBattleStr", "Use @Fire@ when ready");
}

EWoTInterfaceLevel AWoTHUD::NextInterfaceLevel(EWoTInterfaceLevel Level)
{
	const int32 Value = FMath::Clamp(static_cast<int32>(Level) + 1,
		static_cast<int32>(EWoTInterfaceLevel::Normal),
		static_cast<int32>(EWoTInterfaceLevel::UltraMinimized));
	return static_cast<EWoTInterfaceLevel>(Value);
}

EWoTInterfaceLevel AWoTHUD::PreviousInterfaceLevel(EWoTInterfaceLevel Level)
{
	const int32 Value = FMath::Clamp(static_cast<int32>(Level) - 1,
		static_cast<int32>(EWoTInterfaceLevel::Normal),
		static_cast<int32>(EWoTInterfaceLevel::UltraMinimized));
	return static_cast<EWoTInterfaceLevel>(Value);
}

FWoTHUDSurfaceVisibility AWoTHUD::SurfaceVisibility(
	EWoTInterfaceLevel Level, bool bItemInfoOpen)
{
	FWoTHUDSurfaceVisibility Visibility;
	Visibility.bAddedItems = Level == EWoTInterfaceLevel::Normal;
	if (bItemInfoOpen)
	{
		// WOTWindow.PreBeginPlay calls HideUIExceptForHands(true): health,
		// status, keys, and ordinary messages hide; hands/crosshair/progress live.
		Visibility.bHealth = false;
		Visibility.bStatusIcons = false;
		Visibility.bKeys = false;
		Visibility.bMessages = false;
	}
	return Visibility;
}

bool AWoTHUD::ShouldDrawHUD(bool bHUDEnabled, bool bInSeamlessTravel,
	bool bOwningMenuOpen)
{
	return bHUDEnabled && !bInSeamlessTravel && !bOwningMenuOpen;
}

void AWoTHUD::SetInterfaceLevel(EWoTInterfaceLevel NewLevel, bool bSave)
{
	const int32 Value = FMath::Clamp(static_cast<int32>(NewLevel),
		static_cast<int32>(EWoTInterfaceLevel::Normal),
		static_cast<int32>(EWoTInterfaceLevel::UltraMinimized));
	const EWoTInterfaceLevel Clamped = static_cast<EWoTInterfaceLevel>(Value);
	const bool bChanged = InterfaceLevel != Clamped;
	InterfaceLevel = Clamped;
	if (bSave && bChanged)
	{
		// Persist through the class default so a HUD recreated by later travel in
		// the same process inherits the new presentation level immediately.
		AWoTHUD* Defaults = GetMutableDefault<AWoTHUD>();
		Defaults->InterfaceLevel = Clamped;
		Defaults->SaveConfig();
	}
}

void AWoTHUD::BeginPlay()
{
	Super::BeginPlay();
	if (FWoTRetailText::EnsureLoaded())
	{
		auto ApplyRetailText = [](FText& Target, const TCHAR* Section, const TCHAR* Key)
		{
			if (const FString* Value = FWoTRetailText::FindSectionValue(Section, Key))
			{
				Target = FText::FromString(*Value);
			}
		};
		ApplyRetailText(SelectTeamStr, TEXT("BattleHUD"), TEXT("SelectTeamStr"));
		ApplyRetailText(TimeLimitStr, TEXT("BattleHUD"), TEXT("TimeLimitStr"));
		ApplyRetailText(SecondsStr, TEXT("BattleHUD"), TEXT("SecondsStr"));
		ApplyRetailText(CitadelOverviewTeamStr,
			TEXT("CitadelTeamOverview"), TEXT("Description[0]"));
		ApplyRetailText(CitadelOverviewPlayersStr,
			TEXT("CitadelTeamOverview"), TEXT("Description[2]"));
		ApplyRetailText(CitadelOverviewStatusStr,
			TEXT("CitadelTeamOverview"), TEXT("Description[3]"));
		ApplyRetailText(CitadelOverviewBeginBattleStr,
			TEXT("CitadelTeamOverview"), TEXT("BeginBattleStr"));
	}
	SetInterfaceLevel(InterfaceLevel, false);
	BindEditorModeController();
}

void AWoTHUD::BindEditorModeController()
{
	UnbindEditorModeController();
	AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetOwningPlayerController());
	if (!IsValid(WoTController) || !WoTController->IsLocalController())
	{
		SetHUDMode(EWoTHUDMode::Normal, false);
		return;
	}

	BoundEditorModeController = WoTController;
	EditorModeDelegateHandle = WoTController->OnCitadelEditSessionChanged.AddUObject(
		this, &AWoTHUD::HandleCitadelEditSessionChanged);
	SetHUDMode(WoTController->IsOwnerEditingCitadel()
		? EWoTHUDMode::CitadelEditor : EWoTHUDMode::Normal, false);
}

void AWoTHUD::UnbindEditorModeController()
{
	if (AWoTPlayerController* WoTController = BoundEditorModeController.Get())
	{
		WoTController->OnCitadelEditSessionChanged.Remove(EditorModeDelegateHandle);
	}
	BoundEditorModeController.Reset();
	EditorModeDelegateHandle.Reset();
}

void AWoTHUD::HandleCitadelEditSessionChanged(bool bEditing, uint8 /*Team*/)
{
	SetHUDMode(bEditing ? EWoTHUDMode::CitadelEditor : EWoTHUDMode::Normal, true);
}

void AWoTHUD::SetHUDMode(EWoTHUDMode NewMode, bool bPlaySound)
{
	if (HUDMode == NewMode)
	{
		return;
	}
	HUDMode = NewMode;
	if (!bPlaySound || !GetWorld() || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	USoundBase* EdgeSound = HUDMode == EWoTHUDMode::CitadelEditor
		? WoTSound::Resolve(BeginEditorSound) : WoTSound::Resolve(EndEditorSound);
	if (IsValid(EdgeSound))
	{
		UGameplayStatics::PlaySound2D(this, EdgeSound);
	}
}

float AWoTHUD::MessageFadeScale(float LifeSpan, float Duration, float FadePercent)
{
	const float FadeTime = Duration * FadePercent;
	if (LifeSpan <= 0.0f)
	{
		return 0.0f;
	}
	if (FadeTime > 0.0f && LifeSpan < FadeTime)
	{
		return FMath::Clamp(LifeSpan / FadeTime, 0.0f, 1.0f);
	}
	return 1.0f;
}

float AWoTHUD::GenericMessageCenterX(float ScaledX, float CanvasWidth)
{
	return 0.5f * (ScaledX + CanvasWidth);
}

TArray<FString> AWoTHUD::WrapMessageLines(const FString& SourceText, float MaxWidth,
	TFunctionRef<float(const FString&)> MeasureText)
{
	TArray<FString> Lines;
	FString CurrentLine;

	auto FlushLine = [&Lines, &CurrentLine]()
	{
		Lines.Add(CurrentLine);
		CurrentLine.Reset();
	};
	auto AppendRun = [&Lines, &CurrentLine, MaxWidth, &MeasureText](
		const FString& Run, bool bWhitespaceRun)
	{
		int32 RunOffset = 0;
		while (RunOffset < Run.Len())
		{
			const FString Remainder = Run.Mid(RunOffset);
			const FString Candidate = CurrentLine + Remainder;
			if (MeasureText(Candidate) <= MaxWidth)
			{
				CurrentLine = Candidate;
				break;
			}

			if (!CurrentLine.IsEmpty())
			{
				Lines.Add(CurrentLine);
				CurrentLine.Reset();
				// UE1's wrap consumes the whitespace straddling a wrap boundary: a
				// continuation line never begins with the spaces that forced the wrap
				// (interior whitespace that fits mid-line is untouched above).
				if (bWhitespaceRun)
				{
					return;
				}
				continue;
			}

			int32 FittingLength = 0;
			for (int32 Length = 1; Length <= Remainder.Len(); ++Length)
			{
				if (MeasureText(Remainder.Left(Length)) > MaxWidth)
				{
					break;
				}
				FittingLength = Length;
			}
			// A subtitle region is much wider than one glyph in practice. Retaining
			// one character here also guarantees progress for a degenerate width.
			FittingLength = FMath::Max(FittingLength, 1);
			CurrentLine = Remainder.Left(FittingLength);
			RunOffset += FittingLength;
			if (RunOffset < Run.Len())
			{
				Lines.Add(CurrentLine);
				CurrentLine.Reset();
			}
		}
	};

	int32 Offset = 0;
	while (Offset < SourceText.Len())
	{
		if (SourceText[Offset] == TEXT('\n') || SourceText[Offset] == TEXT('\r'))
		{
			FlushLine();
			if (SourceText[Offset] == TEXT('\r') && Offset + 1 < SourceText.Len() &&
				SourceText[Offset + 1] == TEXT('\n'))
			{
				++Offset;
			}
			++Offset;
			continue;
		}

		const bool bWhitespace = FChar::IsWhitespace(SourceText[Offset]);
		int32 End = Offset + 1;
		while (End < SourceText.Len() && SourceText[End] != TEXT('\n') &&
			SourceText[End] != TEXT('\r') &&
			FChar::IsWhitespace(SourceText[End]) == bWhitespace)
		{
			++End;
		}
		AppendRun(SourceText.Mid(Offset, End - Offset), bWhitespace);
		Offset = End;
	}
	Lines.Add(CurrentLine);
	return Lines;
}

void AWoTHUD::TickMessages(float DeltaTime)
{
	WoTHUD_AdvanceMessage(HandMessage, DeltaTime);
	for (FWoTHUDMessageData& Entry : LeftMessages)
	{
		WoTHUD_AdvanceMessage(Entry, DeltaTime);
	}
	for (FWoTHUDMessageData& Entry : RightMessages)
	{
		WoTHUD_AdvanceMessage(Entry, DeltaTime);
	}
	for (FWoTHUDMessageData& Entry : CenterMessages)
	{
		WoTHUD_AdvanceMessage(Entry, DeltaTime);
	}
	WoTHUD_AdvanceMessage(SubtitleMessage, DeltaTime);
	for (FWoTHUDGenericMessageData& Entry : GenericMessages)
	{
		WoTHUD_AdvanceGenericMessage(Entry, DeltaTime);
	}
}

bool AWoTHUD::ShouldDrawMessages(bool bHUDVisible) const
{
	return bHUDVisible && !bHideMessages;
}

float AWoTHUD::UpdateMessageRenderTime(bool bHUDVisible, float CurrentTimeSeconds)
{
	if (!bHUDVisible)
	{
		return 0.0f;
	}
	const float DeltaTime = CurrentTimeSeconds - LastMessageRenderTime;
	LastMessageRenderTime = CurrentTimeSeconds;
	return DeltaTime;
}

void AWoTHUD::AddHandMessage(const FString& MessageText, float Duration)
{
	if (HandMessageSound && GetWorld())
	{
		UGameplayStatics::PlaySound2D(this, HandMessageSound);
	}
	WoTHUD_SetMessage(HandMessage, MessageText, Duration, HandMessageDuration);
}

void AWoTHUD::AddLeftMessage(const FString& MessageText, float Duration)
{
	USoundBase* NotifySound = LeftMessageSound;
	if (!NotifySound && !LeftMessageName.IsEmpty())
	{
		NotifySound = WoTSound::LoadBySubPath(WoTHUD_SoundSubPath(LeftMessageName));
		LeftMessageSound = NotifySound;
	}
	if (NotifySound && GetWorld())
	{
		UGameplayStatics::PlaySound2D(this, NotifySound);
	}
	for (int32 Index = NumLeftMessages - 1; Index > 0; --Index)
	{
		LeftMessages[Index] = LeftMessages[Index - 1];
	}
	WoTHUD_SetMessage(LeftMessages[0], MessageText, Duration, LeftMessageDuration);
	if (APlayerController* Controller = GetOwningPlayerController())
	{
		Controller->ClientMessage(MessageText, FName(TEXT("Message")));
	}
}

void AWoTHUD::AddRightMessage(const FString& MessageText, float Duration)
{
	USoundBase* NotifySound = RightMessageSound;
	if (!NotifySound && !RightMessageName.IsEmpty())
	{
		NotifySound = WoTSound::LoadBySubPath(WoTHUD_SoundSubPath(RightMessageName));
		RightMessageSound = NotifySound;
	}
	if (NotifySound && GetWorld())
	{
		UGameplayStatics::PlaySound2D(this, NotifySound);
	}
	for (int32 Index = NumRightMessages - 1; Index > 0; --Index)
	{
		RightMessages[Index] = RightMessages[Index - 1];
	}
	WoTHUD_SetMessage(RightMessages[0], MessageText, Duration, RightMessageDuration);
	if (APlayerController* Controller = GetOwningPlayerController())
	{
		Controller->ClientMessage(MessageText, FName(TEXT("Message")));
	}
}

void AWoTHUD::AddCenterMessage(const FString& MessageText, float Duration, bool bEcho)
{
	if (CenterMessageSound && GetWorld())
	{
		UGameplayStatics::PlaySound2D(this, CenterMessageSound);
	}
	for (int32 Index = NumCenterMessages - 1; Index > 0; --Index)
	{
		CenterMessages[Index] = CenterMessages[Index - 1];
	}
	WoTHUD_SetMessage(CenterMessages[0], MessageText, Duration, CenterMessageDuration);
	if (bEcho)
	{
		if (APlayerController* Controller = GetOwningPlayerController())
		{
			Controller->ClientMessage(MessageText, FName(TEXT("Message")));
		}
	}
}

void AWoTHUD::AddSubtitleMessage(const FString& MessageText, float Duration, bool bEcho)
{
	WoTHUD_SetMessage(SubtitleMessage, MessageText, Duration, CenterMessageDuration);
	if (bEcho)
	{
		if (APlayerController* Controller = GetOwningPlayerController())
		{
			Controller->ClientMessage(MessageText, FName(TEXT("Message")));
		}
	}
}

void AWoTHUD::AddGenericMessage(const FString& MessageText, int32 MessageX,
	int32 MessageY, bool bCenter, uint8 Intensity, UFont* MessageFont, float Duration)
{
	if (GenericMessageSound && GetWorld())
	{
		UGameplayStatics::PlaySound2D(this, GenericMessageSound);
	}
	for (int32 Index = NumGenericMessages - 1; Index > 0; --Index)
	{
		GenericMessages[Index] = GenericMessages[Index - 1];
	}
	FWoTHUDGenericMessageData& Entry = GenericMessages[0];
	Entry.Message = MessageText;
	Entry.X = MessageX;
	Entry.Y = MessageY;
	Entry.bCenter = bCenter;
	Entry.Intensity = Intensity;
	if (!MessageFont)
	{
		// Resolve the default font against the live viewport width, matching the
		// draw-time paths' Canvas->SizeX resolution — a sub-512 window must pick the
		// small variant here too, not a hardcoded 640 assumption. Headless/pre-viewport
		// callers keep the 640 default.
		int32 ViewportX = 640;
		int32 ViewportY = 0;
		if (APlayerController* Controller = GetOwningPlayerController())
		{
			int32 MeasuredX = 0;
			Controller->GetViewportSize(MeasuredX, ViewportY);
			if (MeasuredX > 0)
			{
				const int32 Upscale = FWoTCanvasScale::ComputeUpscale(
					MeasuredX, ViewportY,
					CVarWoTHUDScale.GetValueOnGameThread());
				ViewportX = MeasuredX / Upscale;
			}
		}
		MessageFont = ResolveFont(FName(TEXT("F_WOTReg14")), ViewportX);
	}
	Entry.Font = MessageFont;
	Entry.LifeSpan = Duration > 0.0f ? Duration : GenericMessageDuration;
	if (APlayerController* Controller = GetOwningPlayerController())
	{
		Controller->ClientMessage(MessageText, FName(TEXT("Message")));
	}
}

UFont* AWoTHUD::ResolveFont(FName Id, int32 ViewportWidth) const
{
#if WOT_GUARDED_BOOTSTRAP
	if (ViewportWidth < 512)
	{
		const FName SmallId(*FString::Printf(TEXT("%s_S"), *Id.ToString()));
		if (UFont* SmallFont = UWoTHUDFonts::ResolveCanvasRuntime(SmallId))
		{
			return SmallFont;
		}
	}
	return UWoTHUDFonts::ResolveCanvasRuntime(Id);
#else
	UWoTHUDFonts* FontRegistry = HUDFonts.LoadSynchronous();
	if (!FontRegistry)
	{
		return nullptr;
	}
	if (ViewportWidth < 512)
	{
		const FName SmallId(*FString::Printf(TEXT("%s_S"), *Id.ToString()));
		if (UFont* SmallFont = FontRegistry->Resolve(SmallId))
		{
			return SmallFont;
		}
	}
	return FontRegistry->Resolve(Id);
#endif
}

FSoftObjectPath AWoTHUD::RuntimeHUDTexturePath(FName Id)
{
	if (Id.IsNone())
	{
		return FSoftObjectPath(); // an icon-less command resolves nothing
	}
	const FString Name = Id.ToString();
	return FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/UI/%s.%s"), *Name, *Name));
}

FSoftObjectPath AWoTHUD::CommandTexturePath(const FWoTHUDDrawCmd& Command)
{
	return Command.Texture.IsNull()
		? RuntimeHUDTexturePath(Command.TexId)
		: Command.Texture.ToSoftObjectPath();
}

UTexture2D* AWoTHUD::ResolveHUDTexture(FName Id) const
{
#if WOT_GUARDED_BOOTSTRAP
	const FSoftObjectPath Path = RuntimeHUDTexturePath(Id);
	return Path.IsNull() ? nullptr : UWoTRuntimeAssets::ResolveTexturePath(Path);
#else
	UWoTHUDTextures* TextureRegistry = HUDTextures.LoadSynchronous();
	return TextureRegistry ? TextureRegistry->Resolve(Id) : nullptr;
#endif
}

void AWoTHUD::ChangeCrosshair(int32 Delta)
{
	Crosshair += Delta;
	if (Crosshair >= NumCrossHairs)
	{
		Crosshair = 0;
	}
	else if (Crosshair < 0)
	{
		Crosshair = NumCrossHairs - 1;
	}
}

TArray<FWoTHUDDrawCmd> AWoTHUD::LayoutCrosshair(int32 Index, int32 ClipX, int32 ClipY)
{
	TArray<FWoTHUDDrawCmd> Commands;
	if (Index < 0 || Index >= NumCrossHairs - 1)
	{
		return Commands;
	}

	FWoTHUDDrawCmd& Command = Commands.AddDefaulted_GetRef();
	Command.TexId = FName(*FString::Printf(TEXT("Crosshair%d"), Index + 1));
	// MainHUD.DrawCursor assigns 0.5*ClipX-8 to an int StartX (MainHUD.uc:114-118),
	// truncating the final coordinate; TruncToFloat matches (0.5*801-8 = 392.5 -> 392).
	Command.X = FMath::TruncToFloat(0.5f * static_cast<float>(ClipX) - 8.0f);
	Command.Y = FMath::TruncToFloat(0.5f * static_cast<float>(ClipY) - 8.0f);
	Command.Style = EWoTHUDStyle::Masked;
	return Commands;
}

FWoTEditorCursorGeometry AWoTHUD::EditorCursorGeometry(int32 ClipX, int32 ClipY)
{
	FWoTEditorCursorGeometry Geometry;
	Geometry.CursorPoint = FVector2D(ClipX / 2, ClipY / 2);
	Geometry.Hotspot = FIntPoint(EditorCursorHotspot, EditorCursorHotspot);
	Geometry.Size = FIntPoint(EditorCursorSize, EditorCursorSize);
	Geometry.TopLeft = Geometry.CursorPoint - FVector2D(
		static_cast<float>(Geometry.Hotspot.X),
		static_cast<float>(Geometry.Hotspot.Y));
	return Geometry;
}

TArray<FWoTHUDDrawCmd> AWoTHUD::LayoutEditorCursor(int32 ClipX, int32 ClipY)
{
	TArray<FWoTHUDDrawCmd> Commands;
	const FWoTEditorCursorGeometry Geometry = EditorCursorGeometry(ClipX, ClipY);
	FWoTHUDDrawCmd& Command = Commands.AddDefaulted_GetRef();
	Command.TexId = TEXT("UI_CECursor");
	Command.X = Geometry.TopLeft.X;
	Command.Y = Geometry.TopLeft.Y;
	Command.Style = EWoTHUDStyle::Masked;
	return Commands;
}

TArray<FWoTHUDDrawCmd> AWoTHUD::LayoutHealth(
	const FWoTHUDReadout& Readout, FName HealthIconId, bool bSeeker)
{
	TArray<FWoTHUDDrawCmd> Commands;
	if (!Readout.bHasPawn)
	{
		return Commands;
	}

	FWoTHUDDrawCmd& Icon = Commands.AddDefaulted_GetRef();
	Icon.TexId = HealthIconId;
	Icon.X = 8.0f;
	Icon.Y = 8.0f;

	if (bSeeker)
	{
		FWoTHUDDrawCmd& Frame = Commands.AddDefaulted_GetRef();
		Frame.TexId = FName(TEXT("SeekerFrame"));
		Frame.X = 8.0f;
		Frame.Y = 8.0f;
		Frame.Style = EWoTHUDStyle::Masked;
	}

	FWoTHUDDrawCmd& Number = Commands.AddDefaulted_GetRef();
	Number.Text = FString::FromInt(FMath::Max(0, Readout.Health));
	Number.FontId = FName(TEXT("F_Charge"));
	Number.X = 24.0f;
	Number.Y = 54.0f;
	Number.bCenterX = true;
	return Commands;
}

int32 AWoTHUD::StatusTimerIndex(float InitialDuration, float RemainingDuration)
{
	if (InitialDuration == 0.0f)
	{
		return 0;
	}
	return FMath::Clamp(FMath::TruncToInt(
		18.0f * (1.0f - RemainingDuration / InitialDuration)), 0, 18);
}

TArray<FWoTHUDDrawCmd> AWoTHUD::LayoutStatusIcons(
	const TArray<FWoTStatusIconEntry>& Entries, int32 ScaledSizeX)
{
	// MainHUD.uc:55-59,122-164: 32px icons at y=8. The good stack starts
	// immediately to the right of DrawHealth's 32px icon; bad icons grow left.
	constexpr int32 IconSpacing = 8;
	constexpr int32 IconWidth = 32;
	int32 GoodX = IconSpacing + IconWidth + IconSpacing;
	int32 BadX = ScaledSizeX - IconWidth - IconSpacing;
	constexpr int32 Y = IconSpacing;

	TArray<FWoTHUDDrawCmd> Commands;
	Commands.Reserve(Entries.Num() * 2);
	for (const FWoTStatusIconEntry& Entry : Entries)
	{
		const int32 X = Entry.bGood ? GoodX : BadX;
		if (Entry.bGood)
		{
			GoodX += IconWidth + IconSpacing;
		}
		else
		{
			BadX -= IconWidth + IconSpacing;
		}

		FWoTHUDDrawCmd& Icon = Commands.AddDefaulted_GetRef();
		Icon.TexId = Entry.IconTexture;
		Icon.X = static_cast<float>(X);
		Icon.Y = static_cast<float>(Y);
		Icon.Scale = 0.5f;

		const int32 TimerIndex = StatusTimerIndex(
			Entry.InitialDuration, Entry.RemainingDuration);
		FWoTHUDDrawCmd& Timer = Commands.AddDefaulted_GetRef();
		Timer.TexId = FName(*FString::Printf(TEXT("T32_%02d"), TimerIndex + 1));
		Timer.X = static_cast<float>(X);
		Timer.Y = static_cast<float>(Y);
		Timer.Style = EWoTHUDStyle::Masked;
	}
	return Commands;
}

TArray<FWoTHUDDrawCmd> AWoTHUD::LayoutHands(
	const UWoTHandSet* HandSet, int32 ScaledSizeX, int32 ScaledSizeY)
{
	TArray<FWoTHUDDrawCmd> Commands;
	if (!HandSet)
	{
		return Commands;
	}

	const int32 StartX = (ScaledSizeX - UWoTHandSet::NumHands * WoTHUD_IconWidth) / 2;
	const int32 Y = ScaledSizeY - WoTHUD_IconHeight;
	for (int32 HandIndex = 0; HandIndex < UWoTHandSet::NumHands; ++HandIndex)
	{
		const UWoTHandInfo* Hand = HandSet->GetHand(HandIndex);
		if (!Hand || Hand->IsEmpty())
		{
			continue;
		}
		int32 DisplaySlot = Hand->Selected;
		if (Hand->GetSelectedClassName().IsNone())
		{
			// BaseHUD.uc:269 temporarily advances to the first occupied slot for
			// drawing, then restores Hand.Selected.
			for (int32 Slot = 0; Slot < UWoTHandInfo::NumSlots; ++Slot)
			{
				if (!Hand->GetClassName(Slot).IsNone())
				{
					DisplaySlot = Slot;
					break;
				}
			}
		}

		const int32 IconX = StartX + WoTHUD_IconWidth * HandIndex;
		const AWoTInventory* LiveItem = Hand->GetItem(DisplaySlot);
		const AWoTInventory* DisplayDefault =
			WoTHUD_FindInventoryDefault(Hand->GetClassName(DisplaySlot));
		const AWoTInventory* IconSource = LiveItem ? LiveItem : DisplayDefault;
		FWoTHUDDrawCmd& Icon = Commands.AddDefaulted_GetRef();
		if (IconSource)
		{
			WoTHUD_SetTexture(Icon, IconSource->StatusIcon);
		}
		Icon.X = static_cast<float>(IconX);
		Icon.Y = static_cast<float>(Y);
		if (const AWoTAngrealInventory* LiveAngreal =
			Cast<AWoTAngrealInventory>(LiveItem); LiveAngreal && LiveAngreal->bTainted)
		{
			FWoTHUDDrawCmd& Tainted = Commands.AddDefaulted_GetRef();
			Tainted.TexId = FName(TEXT("I_Tainted"));
			Tainted.X = static_cast<float>(IconX);
			Tainted.Y = static_cast<float>(Y);
			Tainted.Style = EWoTHUDStyle::Masked;
		}

		const bool bSelectedHand = HandIndex == HandSet->Selected &&
			!Hand->GetSelectedClassName().IsNone();
		if (bSelectedHand)
		{
			FWoTHUDDrawCmd& Highlight = Commands.AddDefaulted_GetRef();
			Highlight.TexId = FName(TEXT("StatusIconSelected"));
			Highlight.X = static_cast<float>(IconX);
			Highlight.Y = static_cast<float>(Y);
			Highlight.Style = EWoTHUDStyle::Masked;
		}

		FWoTHUDDrawCmd& HandNumber = Commands.AddDefaulted_GetRef();
		HandNumber.Text = FString::FromInt((HandIndex + 1) % 10);
		HandNumber.FontId = bSelectedHand
			? FName(TEXT("F_KeySelected")) : FName(TEXT("F_Key"));
		HandNumber.X = static_cast<float>(IconX + 1);
		HandNumber.Y = static_cast<float>(Y + WoTHUD_IconHeight / 2 - 12);

		FWoTHUDDrawCmd& Charge = Commands.AddDefaulted_GetRef();
		if (const AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(LiveItem))
		{
			Charge.Text = Angreal->ChargeCost == 0
				? FString(TEXT(":")) : FString::FromInt(Angreal->CurCharges);
		}
		else
		{
			const int32 DisplayCount = Hand->GetDisplayCount(DisplaySlot);
			Charge.Text = FString::FromInt(DisplayCount != INDEX_NONE
				? DisplayCount : (LiveItem ? LiveItem->Count : 0));
		}
		Charge.FontId = FName(TEXT("F_Charge"));
		Charge.X = static_cast<float>(IconX + WoTHUD_IconWidth / 2 + 2);
		Charge.Y = static_cast<float>(Y + 3 * WoTHUD_IconHeight / 4 + 3);
		Charge.bCenterX = true;

		int32 ItemCount = 0;
		int32 SelectedIndex = 0;
		for (int32 Slot = 0; Slot < UWoTHandInfo::NumSlots; ++Slot)
		{
			if (!Hand->GetClassName(Slot).IsNone())
			{
				if (Slot == Hand->Selected)
				{
					SelectedIndex = ItemCount;
				}
				++ItemCount;
			}
		}
		for (int32 ItemIndex = 0; ItemIndex < ItemCount; ++ItemIndex)
		{
			FWoTHUDDrawCmd& Notch = Commands.AddDefaulted_GetRef();
			Notch.TexId = ItemIndex == SelectedIndex
				? FName(TEXT("I_Notch")) : FName(TEXT("D_Notch"));
			Notch.X = static_cast<float>(StartX +
				WoTHUD_IconWidth * (2 * HandIndex + 1) / 2 +
				WoTHUD_NotchWidth * (2 * ItemIndex - ItemCount) / 2);
			Notch.Y = static_cast<float>(Y);
			Notch.Style = EWoTHUDStyle::Masked;
		}
	}
	return Commands;
}

TArray<FWoTHUDDrawCmd> AWoTHUD::LayoutMinimizedHands(
	const UWoTHandSet* HandSet, int32 ScaledSizeX, int32 ScaledSizeY,
	EWoTInterfaceLevel Level, bool bCurrentHandDisplayActive,
	float TimeSeconds, float TriggerTime, float InTimeOut)
{
	TArray<FWoTHUDDrawCmd> Commands;
	if (!HandSet)
	{
		return Commands;
	}

	// BaseHUD.uc:242-319: the selected hand keeps its full status icon,
	// number, and charge/count readout at bottom center at both reduced levels.
	const UWoTHandInfo* SelectedHand = HandSet->GetSelectedHand();
	if (SelectedHand && !SelectedHand->IsEmpty())
	{
		const int32 Slot = WoTHUD_DisplaySlot(SelectedHand);
		if (Slot != INDEX_NONE)
		{
			const int32 IconX = (ScaledSizeX - WoTHUD_IconWidth) / 2;
			const int32 IconY = ScaledSizeY - WoTHUD_IconHeight;
			const AWoTInventory* LiveItem = SelectedHand->GetItem(Slot);
			const AWoTInventory* IconSource = WoTHUD_ItemOrDefault(SelectedHand, Slot);

			FWoTHUDDrawCmd& Icon = Commands.AddDefaulted_GetRef();
			if (IconSource)
			{
				WoTHUD_SetTexture(Icon, IconSource->StatusIcon);
			}
			Icon.X = static_cast<float>(IconX);
			Icon.Y = static_cast<float>(IconY);

			if (const AWoTAngrealInventory* Angreal =
				Cast<AWoTAngrealInventory>(LiveItem); Angreal && Angreal->bTainted)
			{
				FWoTHUDDrawCmd& Tainted = Commands.AddDefaulted_GetRef();
				Tainted.TexId = FName(TEXT("I_Tainted"));
				Tainted.X = static_cast<float>(IconX);
				Tainted.Y = static_cast<float>(IconY);
				Tainted.Style = EWoTHUDStyle::Masked;
			}

			FWoTHUDDrawCmd& HandNumber = Commands.AddDefaulted_GetRef();
			HandNumber.Text = FString::FromInt((HandSet->Selected + 1) % 10);
			HandNumber.FontId = FName(TEXT("F_KeySelected"));
			HandNumber.X = static_cast<float>(IconX + 1);
			HandNumber.Y = static_cast<float>(IconY + 20);

			FWoTHUDDrawCmd& Charge = Commands.AddDefaulted_GetRef();
			if (const AWoTAngrealInventory* Angreal =
				Cast<AWoTAngrealInventory>(LiveItem))
			{
				Charge.Text = Angreal->ChargeCost == 0
					? FString(TEXT(":")) : FString::FromInt(Angreal->CurCharges);
			}
			else
			{
				const int32 DisplayCount = SelectedHand->GetDisplayCount(Slot);
				Charge.Text = FString::FromInt(DisplayCount != INDEX_NONE
					? DisplayCount : (LiveItem ? LiveItem->Count : 0));
			}
			Charge.FontId = FName(TEXT("F_Charge"));
			Charge.X = static_cast<float>(IconX + 34);
			Charge.Y = static_cast<float>(IconY + 51);
			Charge.bCenterX = true;
		}
	}

	// BaseHUD.uc:320-329: Minimized always shows every nonempty hand's frame;
	// Ultra shows them only during the selected-hand transient, sliding downward.
	if (Level == EWoTInterfaceLevel::Minimized || bCurrentHandDisplayActive)
	{
		float FrameY = static_cast<float>(ScaledSizeY - 32);
		if (Level == EWoTInterfaceLevel::UltraMinimized && InTimeOut > 0.0f)
		{
			FrameY += 32.0f * (TimeSeconds - TriggerTime) / InTimeOut;
		}
		for (int32 HandIndex = 0; HandIndex < UWoTHandSet::NumHands; ++HandIndex)
		{
			const UWoTHandInfo* Hand = HandSet->GetHand(HandIndex);
			if (!Hand || Hand->GetSelectedClassName().IsNone())
			{
				continue;
			}
			const int32 Slot = Hand->Selected;
			const int32 HalfHands = UWoTHandSet::NumHands / 2;
			const int32 FrameX = HandIndex < HalfHands
				? (ScaledSizeX - WoTHUD_IconWidth) / 2
					+ (HandIndex - HalfHands) * WoTHUD_IconWidth / 2
				: (ScaledSizeX + WoTHUD_IconWidth) / 2
					+ (HandIndex - HalfHands) * WoTHUD_IconWidth / 2;
			FWoTHUDDrawCmd& Frame = Commands.AddDefaulted_GetRef();
			if (const AWoTInventory* Item = WoTHUD_ItemOrDefault(Hand, Slot))
			{
				WoTHUD_SetTexture(Frame, Item->StatusIconFrame);
			}
			Frame.X = static_cast<float>(FrameX);
			Frame.Y = FrameY;
			Frame.Scale = 0.5f;
		}
	}
	return Commands;
}

TArray<FWoTHUDDrawCmd> AWoTHUD::LayoutCurrentHand(
	const UWoTHandSet* HandSet, int32 ScaledSizeX, int32 ScaledSizeY,
	EWoTInterfaceLevel Level)
{
	TArray<FWoTHUDDrawCmd> Commands;
	if (!HandSet)
	{
		return Commands;
	}
	const UWoTHandInfo* Hand = HandSet->GetSelectedHand();
	if (!Hand)
	{
		return Commands;
	}

	const int32 ItemCount = Hand->GetItemCount();
	int32 X = 0;
	int32 Y = ScaledSizeY - WoTHUD_IconHeight - WoTHUD_IconHeight / 2;
	if (Level != EWoTInterfaceLevel::Normal)
	{
		// BaseHUD.uc:516-539 centers the current-hand fan and raises it when
		// InterfaceLevel is Minimized or UltraMinimized.
		X = ScaledSizeX / 2 - ItemCount * WoTHUD_IconWidth / 4;
		Y = ScaledSizeY - 96;
	}
	else
	{
		X = (ScaledSizeX - UWoTHandSet::NumHands * WoTHUD_IconWidth) / 2;
		X += (2 * HandSet->Selected + 1) * WoTHUD_IconWidth / 2;
		if (X - ItemCount * WoTHUD_IconWidth / 4 < 0)
		{
			X = 0;
		}
		else if (X + ItemCount * WoTHUD_IconWidth / 4 > ScaledSizeX)
		{
			X = ScaledSizeX - ItemCount * WoTHUD_IconWidth / 2;
		}
		else
		{
			X -= ItemCount * WoTHUD_IconWidth / 4;
		}
	}

	int32 DenseIndex = 0;
	for (int32 Slot = 0; Slot < UWoTHandInfo::NumSlots; ++Slot)
	{
		if (Hand->GetClassName(Slot).IsNone())
		{
			continue;
		}
		const float ItemX = static_cast<float>(X + DenseIndex * WoTHUD_IconWidth / 2);
		FWoTHUDDrawCmd& Frame = Commands.AddDefaulted_GetRef();
		if (const AWoTInventory* Item = WoTHUD_ItemOrDefault(Hand, Slot))
		{
			WoTHUD_SetTexture(Frame, Item->StatusIconFrame);
		}
		Frame.X = ItemX;
		Frame.Y = static_cast<float>(Y);
		Frame.Scale = 0.5f;
		if (Slot == Hand->Selected)
		{
			FWoTHUDDrawCmd& Highlight = Commands.AddDefaulted_GetRef();
			Highlight.TexId = FName(TEXT("StatusIconFrame"));
			Highlight.X = ItemX;
			Highlight.Y = static_cast<float>(Y);
			Highlight.Scale = 0.5f;
			Highlight.Style = EWoTHUDStyle::Masked;
		}
		++DenseIndex;
	}
	return Commands;
}

TArray<FWoTHUDDrawCmd> AWoTHUD::LayoutProgressMessages(
	const TArray<FWoTProgressEntry>& Entries, int32 ClipX, int32 ClipY,
	float FontLineHeight)
{
	TArray<FWoTHUDDrawCmd> Commands;
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (Entries[Index].Message.IsEmpty())
		{
			continue;
		}
		FWoTHUDDrawCmd& Command = Commands.AddDefaulted_GetRef();
		Command.Text = Entries[Index].Message;
		Command.FontId = FName(TEXT("F_WOTReg14"));
		Command.X = static_cast<float>(ClipX) * 0.5f;
		Command.Y = static_cast<float>(ClipY) * 0.25f
			+ static_cast<float>(Index) * (FontLineHeight + 1.0f);
		Command.bCenterX = true;
		Command.Tint = Entries[Index].Color;
	}
	return Commands;
}

TArray<FWoTHUDDrawCmd> AWoTHUD::LayoutAddedItems(
	const UWoTHandSet* HandSet, int32 ScaledSizeX, int32 ScaledSizeY,
	float TimeSeconds, float InTimeOut)
{
	TArray<FWoTHUDDrawCmd> Commands;
	if (!HandSet || InTimeOut <= 0.0f)
	{
		return Commands;
	}
	for (int32 HandIndex = 0; HandIndex < UWoTHandSet::NumHands; ++HandIndex)
	{
		const UWoTHandInfo* Hand = HandSet->GetHand(HandIndex);
		if (!Hand || Hand->ItemAdded.IsNone())
		{
			continue;
		}
		const float Elapsed = TimeSeconds - Hand->ItemAddedTime;
		if (Elapsed < 0.0f || Elapsed > InTimeOut)
		{
			continue;
		}
		const AWoTInventory* AddedItem = nullptr;
		for (int32 Slot = 0; Slot < UWoTHandInfo::NumSlots; ++Slot)
		{
			if (Hand->GetClassName(Slot) == Hand->ItemAdded)
			{
				AddedItem = WoTHUD_ItemOrDefault(Hand, Slot);
				break;
			}
		}
		if (!AddedItem)
		{
			AddedItem = WoTHUD_FindInventoryDefault(Hand->ItemAdded);
		}
		FWoTHUDDrawCmd& Added = Commands.AddDefaulted_GetRef();
		if (AddedItem)
		{
			WoTHUD_SetTexture(Added, AddedItem->StatusIconFrame);
		}
		const int32 StartX = (ScaledSizeX - UWoTHandSet::NumHands * WoTHUD_IconWidth) / 2;
		Added.X = static_cast<float>(StartX +
			(4 * HandIndex + 1) * WoTHUD_IconWidth / 4);
		Added.Y = static_cast<float>(ScaledSizeY - WoTHUD_IconHeight -
			WoTHUD_IconHeight / 2 + FMath::TruncToInt(
				(WoTHUD_IconHeight / 2) * Elapsed / InTimeOut));
		Added.Scale = 0.5f;
	}
	return Commands;
}

FWoTHUDReadout AWoTHUD::GatherReadout() const
{
	FWoTHUDReadout R;

	const APlayerController* PC = GetOwningPlayerController();
	if (!PC)
	{
		return R;
	}

	// --- Local pawn: health + selected artifact / charges ---
	if (const AWoTCharacter* Pawn = Cast<AWoTCharacter>(PC->GetPawn()))
	{
		R.bHasPawn = true;
		R.Health = Pawn->Health;
		R.MaxHealth = Pawn->MaxHealth;
		R.Breath = Pawn->BreathDisplay;   // owner-only mirror; -1 when dry
		if (const UWoTInventoryComponent* Inv =
			Pawn->FindComponentByClass<UWoTInventoryComponent>())
		{
			// SelectedItem replicates COND_OwnerOnly, so a client only ever reads its
			// OWN selection here — exactly the local-player readout the HUD wants.
			if (const AWoTInventory* Sel = Inv->SelectedItem)
			{
				R.Artifact = Sel->GetClass()->GetName();
				if (const AWoTAngrealInventory* Ang = Cast<AWoTAngrealInventory>(Sel))
				{
					R.Charges = Ang->CurCharges;
				}
			}
		}
	}

	// --- Score / team / ping (AWoTPlayerState) ---
	if (const AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>())
	{
		R.bHasScore = true;
		R.Kills = PS->Kills;
		R.Deaths = PS->Deaths;
		R.Team = PS->Team;
		R.Ping = PS->GetPingInMilliseconds();
	}

	// --- Match clock (AWoTGameState; the citadel state adds the seal goal) ---
	if (const UWorld* World = GetWorld())
	{
		if (const AWoTGameState* GS = World->GetGameState<AWoTGameState>())
		{
			R.bDisplayRemaining = GS->bDisplayRemaining;
			const float Seconds = FMath::Max(0.0f,
				GS->bDisplayRemaining ? GS->RemainingTime : GS->ElapsedTime);
			const int32 Mins = FMath::FloorToInt(Seconds / 60.0f);
			const int32 Secs = FMath::FloorToInt(Seconds) % 60;
			R.Clock = FString::Printf(TEXT("%d:%02d"), Mins, Secs);
			if (const AWoTCitadelGameState* CGS = Cast<AWoTCitadelGameState>(GS))
			{
				R.SealGoal = CGS->SealGoal;
			}
		}

		// --- Battle seals: the first placed BattleInfo (Battle maps only) ---
		for (TActorIterator<AWoTBattleInfo> It(World); It; ++It)
		{
			const AWoTBattleInfo* BI = *It;
			if (!IsValid(BI))
			{
				continue;
			}
			R.bHasSeals = true;
			R.NumTeams = BI->NumTeams;
			for (int32 i = 0; i < 4; ++i)
			{
				R.Seals[i] = BI->GetTeamSealCount(static_cast<uint8>(i));
			}
			break;
		}
	}

	// --- Match over (owner-only bMatchEnded latch) ---
	if (const AWoTPlayerController* WPC = Cast<AWoTPlayerController>(PC))
	{
		R.bMatchEnded = WPC->IsMatchEnded();
	}

	return R;
}

TArray<FString> AWoTHUD::FormatLines(const FWoTHUDReadout& Readout)
{
	TArray<FString> Lines;

	if (Readout.bMatchEnded)
	{
		Lines.Add(TEXT("-- MATCH OVER --"));
	}

	if (Readout.bHasPawn)
	{
		Lines.Add(FString::Printf(TEXT("Health: %d/%d"), Readout.Health, Readout.MaxHealth));
		if (Readout.Breath == 0)
		{
			Lines.Add(TEXT("Breath: DROWNING"));
		}
		else if (Readout.Breath > 0)
		{
			Lines.Add(FString::Printf(TEXT("Breath: %d"), Readout.Breath));
		}

		FString ArtLine = Readout.Artifact.IsEmpty()
			? FString(TEXT("Artifact: (none)"))
			: FString::Printf(TEXT("Artifact: %s"), *Readout.Artifact);
		if (Readout.Charges >= 0)
		{
			ArtLine += FString::Printf(TEXT("  x%d"), Readout.Charges);
		}
		Lines.Add(ArtLine);
	}
	else
	{
		Lines.Add(TEXT("(awaiting respawn)"));
	}

	if (Readout.bHasScore)
	{
		Lines.Add(FString::Printf(TEXT("Score: %d / %d   Team: %d   Ping: %dms"),
			Readout.Kills, Readout.Deaths, Readout.Team, Readout.Ping));
	}

	if (!Readout.Clock.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("%s: %s"),
			Readout.bDisplayRemaining ? TEXT("Time left") : TEXT("Elapsed"), *Readout.Clock));
	}

	if (Readout.bHasSeals)
	{
		FString SealLine = TEXT("Seals:");
		const int32 Teams = FMath::Clamp(Readout.NumTeams, 0, 4);
		for (int32 i = 0; i < Teams; ++i)
		{
			SealLine += FString::Printf(TEXT(" T%d=%d"), i, Readout.Seals[i]);
		}
		if (Readout.SealGoal > 0)
		{
			SealLine += FString::Printf(TEXT("  (goal %d)"), Readout.SealGoal);
		}
		Lines.Add(SealLine);
	}

	return Lines;
}

bool AWoTHUD::ShouldDrawWoTHUD() const
{
	// MainHUD/BaseHUD.PostRender gate on !bHide && !bShowMenu and the ordinary
	// LevelAction state (MainHUD.uc:227-233; BaseHUD.uc:703-714). bShowHUD is
	// !bHide, seamless travel is the retained LevelAction proxy, and the owning
	// controller's open gameplay-menu model is WOTPlayer.bShowMenu.
	const UWorld* HUDWorld = GetWorld();
	const AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetOwningPlayerController());
	return HUDWorld && ShouldDrawHUD(bShowHUD, HUDWorld->IsInSeamlessTravel(),
		WoTController && (WoTController->IsInGameMenuOpen()
			|| WoTController->IsHelpOpen()));
}

FWoTHUDSurfaceVisibility AWoTHUD::CurrentSurfaceVisibility() const
{
	const AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetOwningPlayerController());
	return SurfaceVisibility(InterfaceLevel,
		WoTController && WoTController->IsItemInfoOpen());
}

FWoTScoreboardSnapshot AWoTHUD::GatherArenaScoreboard() const
{
	FWoTScoreboardSnapshot Snapshot;
	const UWorld* HUDWorld = GetWorld();
	const AWoTPlayerController* Controller =
		Cast<AWoTPlayerController>(GetOwningPlayerController());
	const AWoTPlayerState* LocalState = Controller
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!HUDWorld)
	{
		return Snapshot;
	}

	// ArenaScoreBoard.DrawHeader prints Level.Title (uc:315-318). Guarded
	// worlds are neutral shells (WoTMapShellNN): the title comes from the
	// materialized world record, never from the shell package name.
	Snapshot.MapName = WoTWorldDisplayTitle(HUDWorld);
	const AWoTGameState* GameState = HUDWorld->GetGameState<AWoTGameState>();
	if (!GameState)
	{
		return Snapshot;
	}
	Snapshot.bDisplayRemaining = GameState->bDisplayRemaining;
	Snapshot.RemainingTime = FMath::TruncToInt(GameState->RemainingTime);
	Snapshot.ElapsedTime = FMath::TruncToInt(GameState->ElapsedTime);

	for (APlayerState* State : GameState->PlayerArray)
	{
		const AWoTPlayerState* WoTState = Cast<AWoTPlayerState>(State);
		if (!WoTState)
		{
			continue;
		}
		FWoTScoreboardPlayer& Player = Snapshot.Players.AddDefaulted_GetRef();
		Player.Name = WoTState->GetPlayerName();
		Player.Ping = FMath::TruncToInt(WoTState->GetPingInMilliseconds());
		Player.PacketLoss = static_cast<int32>(WoTState->PacketLoss);
		Player.Kills = WoTState->Kills;
		Player.Deaths = WoTState->Deaths;
		Player.Suicides = WoTState->Suicides;
		Player.Score = WoTState->CalcScore();
		Player.TimeOnServer = FMath::Max(0,
			Snapshot.ElapsedTime - WoTState->GetStartTime());
		Player.Color = WoTState->Color;
		// The login/morph seam does maintain retail PlayerType for the faction head layer.
		Player.PlayerType = WoTState->PlayerType;
		Player.bHighlight = WoTState == LocalState;
		Player.Team = WoTState->Team;
		if (Snapshot.Players.Num() >= FWoTArenaScoreboard::MaxPlayers)
		{
			break;
		}
	}

	// SB-D3 (G12): the citadel board's team economy — per-row TeamSealCount/TeamScore
	// (CitadelScoreBoard.InitializePlayer uc:121-134, TeamScore = SealCount*10), the
	// SealGoal header, and the BattleInfo team descriptions. Arena worlds place no
	// BattleInfo, so the arena snapshot is untouched.
	const AWoTBattleInfo* Battle = nullptr;
	if (TActorIterator<AWoTBattleInfo> It(const_cast<UWorld*>(HUDWorld)); It)
	{
		Battle = *It;
	}
	if (Battle)
	{
		if (const AWoTCitadelGameState* Citadel =
			Cast<AWoTCitadelGameState>(GameState))
		{
			Snapshot.SealGoal = Citadel->SealGoal;
		}
		for (FWoTScoreboardPlayer& Player : Snapshot.Players)
		{
			if (Player.Team != 255)
			{
				Player.TeamSealCount = Battle->GetTeamSealCount(Player.Team);
				Player.TeamScore = Player.TeamSealCount * 10;
				if (!Snapshot.TeamDescriptions.Contains(Player.Team))
				{
					Snapshot.TeamDescriptions.Add(Player.Team,
						Battle->GetTeamDescription(Player.Team));
				}
			}
		}
	}
	return Snapshot;
}

FWoTCitadelTeamOverviewSnapshot AWoTHUD::GatherCitadelTeamOverview() const
{
	FWoTCitadelTeamOverviewSnapshot Snapshot;
	Snapshot.TeamLegend = CitadelOverviewTeamStr.ToString();
	Snapshot.PlayersLegend = CitadelOverviewPlayersStr.ToString();
	Snapshot.StatusLegend = CitadelOverviewStatusStr.ToString();
	Snapshot.BeginBattleText = CitadelOverviewBeginBattleStr.ToString();

	UWorld* HUDWorld = GetWorld();
	if (!IsValid(HUDWorld))
	{
		return Snapshot;
	}
	// CitadelTeamOverview inherits ArenaScoreBoard.DrawHeader's Level.Title.
	Snapshot.MapName = WoTWorldDisplayTitle(HUDWorld);
	if (const AWoTGameState* GameState = HUDWorld->GetGameState<AWoTGameState>())
	{
		Snapshot.bDisplayRemaining = GameState->bDisplayRemaining;
		Snapshot.RemainingTime = FMath::TruncToInt(GameState->RemainingTime);
		Snapshot.ElapsedTime = FMath::TruncToInt(GameState->ElapsedTime);
	}

	const AWoTBattleInfo* Battle = nullptr;
	for (TActorIterator<AWoTBattleInfo> It(HUDWorld); It; ++It)
	{
		if (IsValid(*It))
		{
			Battle = *It;
			break;
		}
	}
	if (!IsValid(Battle))
	{
		return Snapshot;
	}

	const AWoTPlayerController* Controller =
		Cast<AWoTPlayerController>(GetOwningPlayerController());
	const AWoTPlayerState* LocalState = IsValid(Controller)
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	const int32 TeamCount = FMath::Clamp(Battle->NumTeams, 0,
		FWoTCitadelTeamOverview::MaxTeams);
	Snapshot.bAllTeamsReady = TeamCount > 0;
	Snapshot.Teams.Reserve(TeamCount);
	for (int32 Team = 0; Team < TeamCount; ++Team)
	{
		FWoTCitadelTeamOverviewRow& Row = Snapshot.Teams.AddDefaulted_GetRef();
		Row.Description = Battle->GetTeamDescription(static_cast<uint8>(Team));
		Row.NumPlayers = Battle->NumPlayers[Team];
		Row.MaxTeamSize = Battle->MaxTeamSize;
		Row.Status = Battle->GetTeamStatus(Team);
		Row.bHighlight = IsValid(LocalState) && LocalState->Team == Team;
		Snapshot.bAllTeamsReady &= Row.Status == Battle->ReadyStr;
	}
	return Snapshot;
}

bool AWoTHUD::ShouldDrawArenaScoreboard() const
{
	if (!ShouldDrawWoTHUD())
	{
		return false;
	}
	const AWoTPlayerController* Controller =
		Cast<AWoTPlayerController>(GetOwningPlayerController());
	if (!Controller || Controller->IsItemInfoOpen())
	{
		return false;
	}
	// Match end is NOT a forced term here: GameEnded.BeginState sets bShowScores once
	// (PlayerPawn.GameEnded.BeginState) and the ShowScores exec still toggles
	// afterward — the controller's GameEnded entry seeds bShowScores, the toggle owns it.
	if (Controller->bShowScores || Controller->IsDeathScoreboardAutoShowActive())
	{
		return true;
	}
	// MenuHUD.uc:91-97: bShowOverview is a distinct CitadelTeamOverview branch.
	// If its separately replicated BattleInfo actor has not arrived yet, that branch
	// draws nothing; it never falls through to the Arena scoreboard.
	return false;
}

bool AWoTHUD::ShouldDrawCitadelTeamOverview() const
{
	if (!ShouldDrawWoTHUD())
	{
		return false;
	}
	const AWoTPlayerController* Controller =
		Cast<AWoTPlayerController>(GetOwningPlayerController());
	if (!IsValid(Controller) || Controller->IsItemInfoOpen() ||
		!Controller->bShowOverview ||
		Controller->bShowScores || Controller->IsDeathScoreboardAutoShowActive() ||
		Controller->IsOwnerEditingCitadel())
	{
		return false;
	}

	UWorld* HUDWorld = GetWorld();
	if (!IsValid(HUDWorld))
	{
		return false;
	}
	for (TActorIterator<AWoTBattleInfo> It(HUDWorld); It; ++It)
	{
		if (IsValid(*It))
		{
			return true;
		}
	}
	return false;
}

void AWoTHUD::DrawArenaScoreboard(UCanvas* CanvasObject)
{
	if (!CanvasObject || !ShouldDrawArenaScoreboard())
	{
		return;
	}
	UFont* Font = ResolveFont(FName(TEXT("F_WOTReg14")), CanvasObject->SizeX);
	if (!Font)
	{
		return;
	}
	float TestWidth = 0.0f;
	float FontLineHeight = 0.0f;
	CanvasObject->StrLen(Font, TEXT("TEST"), TestWidth, FontLineHeight);
	(void)TestWidth;

	FWoTCanvasScale CanvasScale;
	CanvasScale.ComputeFactors(CanvasObject->SizeX, CanvasObject->SizeY);
	TMap<FName, UTexture2D*> IconTextures;
	bool bIconsAvailable = true;
	for (const FName TextureId : FWoTArenaScoreboard::AllIconTextureIds())
	{
		UTexture2D* Texture = ResolveHUDTexture(TextureId);
		IconTextures.Add(TextureId, Texture);
		bIconsAvailable &= Texture != nullptr;
	}
	const bool bStandalone = GetWorld() &&
		GetWorld()->GetNetMode() == NM_Standalone;
	// giMPBattle.ScoreBoardType=CitadelScoreBoard vs giMPArena's ArenaScoreBoard
	// (giMPBattle.uc:533 / giMPArena.uc:23): the citadel game state selects the board.
	const bool bCitadel = GetWorld()
		&& Cast<AWoTCitadelGameState>(GetWorld()->GetGameState()) != nullptr;
	const FWoTArenaScoreboard ArenaBoard;
	const FWoTCitadelScoreboard CitadelBoard;
	const FWoTArenaScoreboard& Scoreboard = bCitadel
		? static_cast<const FWoTArenaScoreboard&>(CitadelBoard) : ArenaBoard;
	const TArray<FWoTHUDDrawCmd> Commands = Scoreboard.Layout(
		GatherArenaScoreboard(), FMath::Max(640, CanvasObject->SizeX),
		FMath::Max(480, CanvasObject->SizeY), FontLineHeight, bStandalone,
		bIconsAvailable);
	for (const FWoTHUDDrawCmd& Command : Commands)
	{
		if (!Command.Text.IsEmpty())
		{
			CanvasScale.DrawScaledTextAt(CanvasObject, Command.X, Command.Y,
				Command.Text, Font, Command.bCenterX, Command.Tint,
				Command.TextAlignment);
		}
		else if (!Command.TexId.IsNone())
		{
			UTexture2D* const* Texture = IconTextures.Find(Command.TexId);
			CanvasScale.DrawIconAt(CanvasObject, Texture ? *Texture : nullptr,
				Command.X, Command.Y, Command.Scale, Command.Style, Command.Tint);
		}
	}
}

void AWoTHUD::DrawCitadelTeamOverview(UCanvas* CanvasObject)
{
	if (!IsValid(CanvasObject) || !ShouldDrawCitadelTeamOverview())
	{
		return;
	}
	UFont* Font = ResolveFont(FName(TEXT("F_WOTReg14")), CanvasObject->SizeX);
	if (!IsValid(Font))
	{
		return;
	}
	float TestWidth = 0.0f;
	float FontLineHeight = 0.0f;
	CanvasObject->StrLen(Font, TEXT("TEST"), TestWidth, FontLineHeight);
	(void)TestWidth;

	FWoTCanvasScale CanvasScale;
	CanvasScale.ComputeFactors(CanvasObject->SizeX, CanvasObject->SizeY);
	const FWoTCitadelTeamOverview Overview;
	const TArray<FWoTHUDDrawCmd> Commands = Overview.Layout(
		GatherCitadelTeamOverview(), FMath::Max(640, CanvasObject->SizeX),
		FMath::Max(480, CanvasObject->SizeY), FontLineHeight);
	for (const FWoTHUDDrawCmd& Command : Commands)
	{
		if (!Command.Text.IsEmpty())
		{
			CanvasScale.DrawScaledTextAt(CanvasObject, Command.X, Command.Y,
				Command.Text, Font, Command.bCenterX, Command.Tint,
				Command.TextAlignment);
		}
	}
}

void AWoTHUD::DrawHealth(UCanvas* CanvasObject)
{
	if (!CanvasObject || !ShouldDrawWoTHUD() ||
		!CurrentSurfaceVisibility().bHealth)
	{
		return;
	}

	const APlayerController* OwningController = GetOwningPlayerController();
	const AWoTCharacter* Pawn = OwningController
		? Cast<AWoTCharacter>(OwningController->GetPawn()) : nullptr;
	if (!Pawn)
	{
		return;
	}

	UTexture2D* HealthTexture = Pawn->GetHealthIcon();
	if (!HealthTexture)
	{
		return;
	}

	FWoTCanvasScale CanvasScale;
	CanvasScale.ComputeFactors(CanvasObject->SizeX, CanvasObject->SizeY);
	const TArray<FWoTHUDDrawCmd> Commands = LayoutHealth(
		GatherReadout(), HealthTexture->GetFName(), Pawn->SeekerCount != 0);
	for (const FWoTHUDDrawCmd& Command : Commands)
	{
		if (!Command.Text.IsEmpty())
		{
			UFont* CommandFont = ResolveFont(
				Command.FontId, static_cast<int32>(CanvasObject->SizeX));
			CanvasScale.DrawScaledTextAt(CanvasObject, Command.X, Command.Y,
				Command.Text, CommandFont, Command.bCenterX, Command.Tint,
				Command.TextAlignment);
			continue;
		}

		UTexture2D* Texture = Command.TexId == HealthTexture->GetFName()
			? HealthTexture
			: ResolveHUDTexture(Command.TexId);
		CanvasScale.DrawIconAt(CanvasObject, Texture, Command.X, Command.Y,
			Command.Scale, Command.Style, Command.Tint);
	}
}

void AWoTHUD::DrawStatusIcons(UCanvas* CanvasObject)
{
	if (!CanvasObject || !ShouldDrawWoTHUD() || bHideStatusIcons ||
		!CurrentSurfaceVisibility().bStatusIcons)
	{
		return;
	}
	const APlayerController* OwningController = GetOwningPlayerController();
	const AWoTCharacter* Pawn = OwningController
		? Cast<AWoTCharacter>(OwningController->GetPawn()) : nullptr;
	if (!IsValid(Pawn))
	{
		return;
	}
	FWoTCanvasScale CanvasScale;
	CanvasScale.ComputeFactors(CanvasObject->SizeX, CanvasObject->SizeY);
	for (const FWoTHUDDrawCmd& Command : LayoutStatusIcons(
		Pawn->StatusIcons, ScaledViewportWidth(CanvasObject->SizeX)))
	{
		CanvasScale.DrawIconAt(CanvasObject, ResolveHUDTexture(Command.TexId),
			Command.X, Command.Y, Command.Scale, Command.Style, Command.Tint);
	}
}

void AWoTHUD::DrawCrosshair(UCanvas* CanvasObject)
{
	if (!CanvasObject || !ShouldDrawWoTHUD())
	{
		return;
	}

	// MainHUD.DrawCursor uses the live ClipX/ClipY and Canvas.DrawIcon directly,
	// unlike the health path's LegendCanvas scaling. Unit factors preserve that.
	const FWoTCanvasScale CanvasScale;
	for (const FWoTHUDDrawCmd& Command :
		LayoutCrosshair(Crosshair, static_cast<int32>(CanvasObject->ClipX),
			static_cast<int32>(CanvasObject->ClipY)))
	{
		CanvasScale.DrawIconAt(CanvasObject, ResolveHUDTexture(Command.TexId),
			Command.X, Command.Y, Command.Scale, Command.Style, Command.Tint);
	}
}

void AWoTHUD::DrawEditorCursor(UCanvas* CanvasObject)
{
	if (!CanvasObject || !ShouldDrawWoTHUD())
	{
		return;
	}
	UTexture2D* CursorTexture = UWoTRuntimeAssets::ResolveTexturePath(
		EditorCursorTexture.ToSoftObjectPath());
	if (!IsValid(CursorTexture))
	{
		return;
	}
	const FWoTCanvasScale CanvasScale;
	for (const FWoTHUDDrawCmd& Command : LayoutEditorCursor(
		static_cast<int32>(CanvasObject->ClipX),
		static_cast<int32>(CanvasObject->ClipY)))
	{
		CanvasScale.DrawIconAt(CanvasObject, CursorTexture, Command.X, Command.Y,
			Command.Scale, Command.Style, Command.Tint);
	}
}

void AWoTHUD::NotifyHandSelection()
{
	HandSelectionTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

bool AWoTHUD::IsCurrentHandDisplayActive(float CurrentTimeSeconds) const
{
	return CurrentTimeSeconds - HandSelectionTime <= HandDisplayTime &&
		CurrentTimeSeconds >= HandSelectionTime;
}

void AWoTHUD::ExecuteHandCommands(UCanvas* CanvasObject,
	const TArray<FWoTHUDDrawCmd>& Commands)
{
	if (!CanvasObject)
	{
		return;
	}
	FWoTCanvasScale CanvasScale;
	CanvasScale.ComputeFactors(CanvasObject->SizeX, CanvasObject->SizeY);
	for (const FWoTHUDDrawCmd& Command : Commands)
	{
		if (!Command.Text.IsEmpty())
		{
			CanvasScale.DrawScaledTextAt(CanvasObject, Command.X, Command.Y,
				Command.Text, ResolveFont(Command.FontId, CanvasObject->SizeX),
				Command.bCenterX, Command.Tint, Command.TextAlignment);
			continue;
		}
		// A command bound to an item's authored texture (StatusIcon/Frame, e.g.
		// Texture'WOT.UI.I_SSeal') resolves ONLY that path. Its TexId is just
		// the asset leaf, which is not a HUD registry id: resolving it first
		// asked guarded builds for /Game/WoT/UI/I_SSeal, a path outside every
		// retail identity, and every citadel hand frame recorded a failed
		// texture (T6-2 assets-resolved failed_textures=20 host / 90-180 client).
#if WOT_GUARDED_BOOTSTRAP
		const FSoftObjectPath TexturePath = CommandTexturePath(Command);
		UTexture2D* Texture = TexturePath.IsNull()
			? nullptr : UWoTRuntimeAssets::ResolveTexturePath(TexturePath);
#else
		UTexture2D* Texture = Command.Texture.IsNull()
			? ResolveHUDTexture(Command.TexId)
			: UWoTRuntimeAssets::ResolveTexturePath(
				Command.Texture.ToSoftObjectPath());
#endif
		CanvasScale.DrawIconAt(CanvasObject, Texture, Command.X, Command.Y,
			Command.Scale, Command.Style, Command.Tint);
	}
}

void AWoTHUD::DrawHands(UCanvas* CanvasObject)
{
	const APlayerController* Controller = GetOwningPlayerController();
	const AWoTCharacter* Pawn = Controller
		? Cast<AWoTCharacter>(Controller->GetPawn()) : nullptr;
	if (!Pawn || !ShouldDrawWoTHUD())
	{
		return;
	}
	if (!CurrentSurfaceVisibility().bHands)
	{
		return;
	}
	const int32 ScaledX = FMath::Max(640, CanvasObject->SizeX);
	const int32 ScaledY = FMath::Max(480, CanvasObject->SizeY);
	if (InterfaceLevel == EWoTInterfaceLevel::Normal)
	{
		ExecuteHandCommands(CanvasObject,
			LayoutHands(Pawn->CurrentHandSet, ScaledX, ScaledY));
		return;
	}
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	ExecuteHandCommands(CanvasObject, LayoutMinimizedHands(Pawn->CurrentHandSet,
		ScaledX, ScaledY, InterfaceLevel, IsCurrentHandDisplayActive(Now),
		Now, HandSelectionTime, HandDisplayTime));
}

void AWoTHUD::DrawCurrentHand(UCanvas* CanvasObject)
{
	const APlayerController* Controller = GetOwningPlayerController();
	const AWoTCharacter* Pawn = Controller
		? Cast<AWoTCharacter>(Controller->GetPawn()) : nullptr;
	if (!Pawn || !ShouldDrawWoTHUD())
	{
		return;
	}
	ExecuteHandCommands(CanvasObject, LayoutCurrentHand(Pawn->CurrentHandSet,
		FMath::Max(640, CanvasObject->SizeX), FMath::Max(480, CanvasObject->SizeY),
		InterfaceLevel));
}

void AWoTHUD::DrawAddedItems(UCanvas* CanvasObject)
{
	APlayerController* Controller = GetOwningPlayerController();
	AWoTCharacter* Pawn = Controller
		? Cast<AWoTCharacter>(Controller->GetPawn()) : nullptr;
	if (!Pawn || !ShouldDrawWoTHUD() ||
		!CurrentSurfaceVisibility().bAddedItems)
	{
		return;
	}
	// BaseHUD.uc:588 reads WOTPlayer(Owner).CurrentHandSet — the added-items slide (and
	// its expiry sweep below) follows the ACTIVE hand set, exactly like the other two
	// hand draw paths, not the angreal set specifically.
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	const TArray<FWoTHUDDrawCmd> Commands = LayoutAddedItems(Pawn->CurrentHandSet,
		FMath::Max(640, CanvasObject->SizeX), FMath::Max(480, CanvasObject->SizeY),
		Now, HandDisplayTime);
	ExecuteHandCommands(CanvasObject, Commands);
	// DrawAddedItems clears stale notification state after the pure layout snapshot
	// has been assembled (BaseHUD.uc:596-600), including level-time rollback.
	if (Pawn->CurrentHandSet)
	{
		for (int32 HandIndex = 0; HandIndex < UWoTHandSet::NumHands; ++HandIndex)
		{
			UWoTHandInfo* Hand = Pawn->CurrentHandSet->GetHand(HandIndex);
			if (Hand && !Hand->ItemAdded.IsNone() &&
				(Now - Hand->ItemAddedTime > HandDisplayTime || Now < Hand->ItemAddedTime))
			{
				Hand->ItemAdded = NAME_None;
			}
		}
	}
}

void AWoTHUD::DrawProgressMessages(UCanvas* CanvasObject)
{
	const AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetOwningPlayerController());
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (!CanvasObject || !WoTController || !ShouldDrawWoTHUD() ||
		!CurrentSurfaceVisibility().bProgress ||
		!WoTController->AreProgressMessagesActive(Now))
	{
		return;
	}
	UFont* Font = ResolveFont(FName(TEXT("F_WOTReg14")), CanvasObject->SizeX);
	if (!Font)
	{
		return;
	}
	float TestWidth = 0.0f;
	float FontLineHeight = 0.0f;
	CanvasObject->StrLen(Font, TEXT("TEST"), TestWidth, FontLineHeight);
	(void)TestWidth;
	const FWoTCanvasScale CanvasScale;
	for (const FWoTHUDDrawCmd& Command : LayoutProgressMessages(
		WoTController->GetProgressEntries(), CanvasObject->SizeX,
		CanvasObject->SizeY, FontLineHeight))
	{
		CanvasScale.DrawScaledTextAt(CanvasObject, Command.X, Command.Y,
			Command.Text, Font, Command.bCenterX, Command.Tint,
			Command.TextAlignment);
	}
}

void AWoTHUD::DrawMessages(UCanvas* CanvasObject, float DeltaTime)
{
	if (!CanvasObject)
	{
		return;
	}

	// BaseHUD updates the queues only while DrawMessages is allowed to run, so
	// bHideMessages intentionally pauses rather than silently expires messages.
	TickMessages(DeltaTime);
	UFont* RegularFont = ResolveFont(FName(TEXT("F_WOTReg14")), CanvasObject->SizeX);
	if (!RegularFont)
	{
		return;
	}

	FWoTCanvasScale CanvasScale;
	CanvasScale.ComputeFactors(CanvasObject->SizeX, CanvasObject->SizeY);
	auto TextSize = [CanvasObject](UFont* Font, const FString& Text)
	{
		FVector2D Size(0.0f, 0.0f);
		CanvasObject->StrLen(Font, Text, Size.X, Size.Y);
		return Size;
	};
	auto DrawMessageText = [CanvasObject](const FString& Text, UFont* Font, float X,
		float Y, uint8 Intensity, float FadeScale, bool bCenterX,
		float WrapWidth = 0.0f)
	{
		if (!Font || Intensity == 0 || FadeScale <= 0.0f)
		{
			return;
		}
		const float Opacity = static_cast<float>(Intensity) * FadeScale / 255.0f;
		if (WrapWidth > 0.0f)
		{
			const TArray<FString> Lines = WrapMessageLines(Text, WrapWidth,
				[CanvasObject, Font](const FString& Line)
				{
					float Width = 0.0f;
					float Height = 0.0f;
					CanvasObject->StrLen(Font, Line, Width, Height);
					return Width;
				});
			const bool bCenterLine = bCenterX && Lines.Num() == 1;
			float LineY = Y;
			for (const FString& Line : Lines)
			{
				FCanvasTextItem LineItem(FVector2D(X, LineY), FText::FromString(Line),
					Font, FadedHUDColor(Opacity));
				LineItem.BlendMode = SE_BLEND_Translucent;
				LineItem.bCentreX = bCenterLine;
				CanvasObject->DrawItem(LineItem);

				float LineWidth = 0.0f;
				float LineHeight = 0.0f;
				CanvasObject->StrLen(Font, Line.IsEmpty() ? TEXT(" ") : Line,
					LineWidth, LineHeight);
				LineY += LineHeight;
			}
			return;
		}

		FCanvasTextItem TextItem(FVector2D(X, Y), FText::FromString(Text), Font,
			FadedHUDColor(Opacity));
		TextItem.BlendMode = SE_BLEND_Translucent;
		TextItem.bCentreX = bCenterX;
		CanvasObject->DrawItem(TextItem);
	};

	if (HandMessage.LifeSpan > 0.0f && HandMessageIntensity > 0)
	{
		const FVector2D Size = TextSize(RegularFont, HandMessage.Message);
		DrawMessageText(HandMessage.Message, RegularFont,
			(static_cast<float>(CanvasObject->SizeX) - Size.X) * 0.5f,
			static_cast<float>(CanvasObject->SizeY) -
				(CanvasScale.ScaleValY(96.0f) + Size.Y),
			HandMessageIntensity, MessageFadeScale(HandMessage.LifeSpan,
				HandMessageDuration, HandMessageFadePercent), false);
	}

	if (LeftMessageIntensity > 0)
	{
		float Y = CanvasScale.ScaleValY(80.0f);
		for (const FWoTHUDMessageData& Entry : LeftMessages)
		{
			if (Entry.LifeSpan > 0.0f)
			{
				const FVector2D Size = TextSize(RegularFont, Entry.Message);
				DrawMessageText(Entry.Message, RegularFont, 0.0f, Y, LeftMessageIntensity,
					MessageFadeScale(Entry.LifeSpan, LeftMessageDuration,
						LeftMessageFadePercent), false);
				Y += Size.Y;
			}
		}
	}

	if (RightMessageIntensity > 0)
	{
		float Y = CanvasScale.ScaleValY(80.0f);
		for (const FWoTHUDMessageData& Entry : RightMessages)
		{
			if (Entry.LifeSpan > 0.0f)
			{
				const FVector2D Size = TextSize(RegularFont, Entry.Message);
				DrawMessageText(Entry.Message, RegularFont,
					static_cast<float>(CanvasObject->SizeX) - Size.X - 1.0f, Y,
					RightMessageIntensity, MessageFadeScale(Entry.LifeSpan,
						RightMessageDuration, RightMessageFadePercent), false);
				Y += Size.Y;
			}
		}
	}

	if (CenterMessageIntensity > 0)
	{
		float Y = TextSize(RegularFont, TEXT(" ")).Y;
		for (const FWoTHUDMessageData& Entry : CenterMessages)
		{
			if (Entry.LifeSpan > 0.0f)
			{
				const FVector2D Size = TextSize(RegularFont, Entry.Message);
				DrawMessageText(Entry.Message, RegularFont,
					(static_cast<float>(CanvasObject->SizeX) - Size.X) * 0.5f, Y,
					CenterMessageIntensity, MessageFadeScale(Entry.LifeSpan,
						CenterMessageDuration, CenterMessageFadePercent), false);
				Y += Size.Y;
			}
		}
	}

	if (SubtitleMessageIntensity > 0 && SubtitleMessage.LifeSpan > 0.0f)
	{
		const float SubtitleWidth = 0.8f * static_cast<float>(CanvasObject->SizeX);
		const bool bHasForcedBreak = SubtitleMessage.Message.Contains(TEXT("\n")) ||
			SubtitleMessage.Message.Contains(TEXT("\r"));
		const bool bOneLine = !bHasForcedBreak &&
			TextSize(RegularFont, SubtitleMessage.Message).X <= SubtitleWidth;
		// The shipped OrgX/ClipX window is the middle 8/10ths. Canvas.bCenter only
		// centers a one-line subtitle; wrapped subtitles remain left justified.
		DrawMessageText(SubtitleMessage.Message, RegularFont,
			bOneLine ? 0.5f * static_cast<float>(CanvasObject->SizeX)
				: 0.1f * static_cast<float>(CanvasObject->SizeX),
			0.0f, SubtitleMessageIntensity,
			MessageFadeScale(SubtitleMessage.LifeSpan, SubtitleMessageDuration,
				SubtitleMessageFadePercent), bOneLine, SubtitleWidth);
	}

	// The outer intensity is a shipped master gate. A per-message zero intensity
	// falls back to it; non-zero values override it.
	if (GenericMessageIntensity > 0)
	{
		for (const FWoTHUDGenericMessageData& Entry : GenericMessages)
		{
			if (Entry.LifeSpan <= 0.0f)
			{
				continue;
			}
			UFont* EntryFont = Entry.Font ? Entry.Font.Get() : RegularFont;
			const uint8 EntryIntensity = Entry.Intensity > 0
				? Entry.Intensity : GenericMessageIntensity;
			float X = CanvasScale.ScaleValX(static_cast<float>(Entry.X));
			if (Entry.bCenter)
			{
				// UE1 centers within the remaining clip region from CurX to the right.
				X = GenericMessageCenterX(X,
					static_cast<float>(CanvasObject->SizeX));
			}
			DrawMessageText(Entry.Message, EntryFont, X,
				CanvasScale.ScaleValY(static_cast<float>(Entry.Y)), EntryIntensity,
				MessageFadeScale(Entry.LifeSpan, GenericMessageDuration,
					GenericMessageFadePercent), Entry.bCenter);
		}
	}
}

void AWoTHUD::DrawTeamSelect(UCanvas* CanvasObject)
{
	if (!CanvasObject || !ShouldDrawWoTHUD())
	{
		return;
	}
	const AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetOwningPlayerController());
	if (WoTController && WoTController->IsItemInfoOpen())
	{
		// BattleHUD.PostRender suppresses the team prompt while a WOTWindow lives.
		return;
	}
	const AWoTPlayerState* WoTState = WoTController
		? WoTController->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!WoTController || !WoTState)
	{
		return;
	}

	const FString Prompt = TeamSelectPrompt(WoTState->Team, BattleTeamCount(),
		WoTController->JoinTeamTimeout, WoTController->IsJoystickInputEnabled());
	UFont* PromptFont = ResolveFont(FName(TEXT("F_WOTReg14")), CanvasObject->SizeX);
	if (Prompt.IsEmpty() || !PromptFont)
	{
		return;
	}

	float TextHeight = 0.0f;
	float TextWidth = 0.0f;
	CanvasObject->StrLen(PromptFont, Prompt, TextWidth, TextHeight);
	FCanvasTextItem PromptItem(
		TeamSelectPosition(static_cast<float>(CanvasObject->SizeX), TextWidth, TextHeight),
		FText::FromString(Prompt), PromptFont, FLinearColor::White);
	PromptItem.BlendMode = SE_BLEND_Masked;
	CanvasObject->DrawItem(PromptItem);
}

void AWoTHUD::UpdatePlayerID(UCanvas* CanvasObject, float DeltaTime)
{
	APlayerController* OwningController = GetOwningPlayerController();
	if (const UWoTPreferencesSubsystem* Preferences = OwningController
		&& OwningController->GetGameInstance()
		? OwningController->GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>()
		: nullptr)
	{
		bPlayerIDEnabled = Preferences->GetGameSettings().bPlayerIDEnabled;
	}
	if (!bPlayerIDEnabled)
	{
		IdentifyTarget.Reset();
		IdentifyFadeTime = 0.0f;
		return;
	}
	IdentifyFadeTime = FMath::Max(0.0f, IdentifyFadeTime - DeltaTime * 85.0f);
	APawn* OwningPawn = OwningController ? OwningController->GetPawn() : nullptr;
	UWorld* HUDWorld = GetWorld();
	const float TraceLength = FMath::Clamp(PlayerIDTraceLen, 0.0f, 65535.0f);
	if (OwningPawn && HUDWorld && TraceLength > 0.0f)
	{
		const FVector StartTrace = OwningPawn->GetActorLocation() +
			FVector(0.0f, 0.0f, OwningPawn->BaseEyeHeight);
		const FVector TraceDirection =
			OwningController->GetControlRotation().Vector();
		// BaseHUD.uc:1394 TraceRecursive(..., true): actor and world candidates
		// share the retail recursive query and ordering.
		FHitResult PawnHit;
		if (WoTRetailActorTraceRecursive(HUDWorld, StartTrace, TraceDirection,
			TraceLength, OwningPawn, PawnHit))
		{
			const FString HitName = PlayerIDName(Cast<APawn>(PawnHit.GetActor()));
			if (!HitName.IsEmpty())
			{
				IdentifyTarget = HitName;
				IdentifyFadeTime = 350.0f;
			}
		}
	}

	if (!CanvasObject || IdentifyFadeTime <= 0.0f || IdentifyTarget.IsEmpty())
	{
		return;
	}
	UFont* IDFont = ResolveFont(FName(TEXT("F_WOTReg14")), CanvasObject->SizeX);
	if (!IDFont)
	{
		return;
	}
	const float Intensity = FMath::Clamp(IdentifyFadeTime * 0.5f / 255.0f,
		0.0f, 1.0f);
	FCanvasTextItem NameItem(
		FVector2D(0.5f * CanvasObject->SizeX,
			CanvasObject->SizeY * FMath::Clamp(PlayerIDYOffsetRatio, 0.0f, 1.0f)),
		FText::FromString(IdentifyTarget), IDFont,
		FadedHUDColor(Intensity));
	NameItem.BlendMode = SE_BLEND_Translucent;
	NameItem.bCentreX = true;
	CanvasObject->DrawItem(NameItem);
}

void AWoTHUD::SetFlashTexture(FName TextureId, float Duration)
{
	FlashTextureTime = Duration > 0.0f ? Duration : 1.0f;
	FlashTextureId = TextureId;
	FlashTextureTimer = FlashTextureTime;
}

void AWoTHUD::UpdateTurtle(float DeltaTime)
{
	const bool bStandalone = GetNetMode() == NM_Standalone;
	switch (ClassifyTurtle(DeltaTime, bStandalone,
		TurtleThresholdSP, TurtleThresholdMP,
		DeadTurtleThresholdSP, DeadTurtleThresholdMP))
	{
	case EWoTTurtleFlash::TurtleDead:
		SetFlashTexture(FName(TEXT("TurtleDead")), TurtleFadeTime);
		break;
	case EWoTTurtleFlash::Turtle:
		SetFlashTexture(FName(TEXT("Turtle")), TurtleFadeTime);
		break;
	case EWoTTurtleFlash::None:
	default:
		break;
	}
}

void AWoTHUD::GetRidOfTurtle()
{
	bTurtleEnabled = false;
}

void AWoTHUD::BringBackTurtle()
{
	bTurtleEnabled = true;
}

void AWoTHUD::SetTurtleFadeTime(float InFadeTime)
{
	TurtleFadeTime = InFadeTime;
}

void AWoTHUD::DrawFlashTexture(UCanvas* CanvasObject, float DeltaTime)
{
	if (!CanvasObject || FlashTextureTimer <= 0.0f)
	{
		return;
	}
	UTexture2D* Texture = ResolveHUDTexture(FlashTextureId);
	if (Texture)
	{
		const float Fade = FlashFade(FlashTextureTimer, FlashTextureTime);
		FWoTCanvasScale CanvasScale;
		CanvasScale.ComputeFactors(CanvasObject->SizeX, CanvasObject->SizeY);
		CanvasScale.DrawIconAt(CanvasObject, Texture,
			FlashTextureX(CanvasObject->SizeX), 64.0f, 1.0f,
			EWoTHUDStyle::Translucent, FadedHUDColor(Fade));
	}
	FlashTextureTimer -= DeltaTime;
}

void AWoTHUD::SetWarning()
{
	TriggerWarningTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void AWoTHUD::ClientWarn()
{
	// BaseHUD.ClientWarn enabled the warning before using SetWarning's ordinary
	// timestamp-only behavior.
	bWarningEnabled = true;
	SetWarning();
}

void AWoTHUD::DrawWarning(UCanvas* CanvasObject)
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (!CanvasObject || !bWarningEnabled ||
		!TimedExtraActive(TriggerWarningTime, TimeOut, Now))
	{
		return;
	}
	FWoTCanvasScale CanvasScale;
	CanvasScale.ComputeFactors(CanvasObject->SizeX, CanvasObject->SizeY);
	CanvasScale.DrawIconAt(CanvasObject,
		ResolveHUDTexture(FName(TEXT("Warn"))),
		WarningTextureX(CanvasObject->SizeX), 0.0f, 1.0f,
		EWoTHUDStyle::Normal);
}

void AWoTHUD::DrawDebugReadout(UCanvas* CanvasObject)
{
	if (!CanvasObject)
	{
		return;
	}

	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;

	float Y = 16.0f;
	for (const FString& Line : GetCachedDebugReadoutLines())
	{
		FCanvasTextItem Text(FVector2D(16.0f, Y), FText::FromString(Line),
			Font, FLinearColor::White);
		Text.EnableShadow(FLinearColor::Black);
		CanvasObject->DrawItem(Text);
		Y += 16.0f;
	}
}

const TArray<FString>& AWoTHUD::GetCachedDebugReadoutLines()
{
	const FWoTHUDReadout Readout = GatherReadout();
	if (!bDebugReadoutCacheValid || !(Readout == CachedDebugReadout))
	{
		CachedDebugReadout = Readout;
		CachedDebugReadoutLines = FormatLines(Readout);
		bDebugReadoutCacheValid = true;
	}
	return CachedDebugReadoutLines;
}

#undef LOCTEXT_NAMESPACE

void AWoTHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const int32 HUDUpscale = FWoTCanvasScale::ComputeUpscale(
		Canvas->SizeX, Canvas->SizeY,
		CVarWoTHUDScale.GetValueOnGameThread());
	{
		// The retail HUD draws on a logical canvas scaled by an integer factor; the
		// debug readout below uses an engine runtime font and stays at native size.
		const FWoTHUDIntegerUpscaleGuard UpscaleGuard(*Canvas, HUDUpscale);

		if (ShouldDrawHealth(HUDMode))
		{
			DrawHealth(Canvas);
		}
		DrawStatusIcons(Canvas);
		if (IsEditorMode())
		{
			DrawEditorCursor(Canvas);
		}
		else
		{
			DrawCrosshair(Canvas);
		}
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		if (IsCurrentHandDisplayActive(Now))
		{
			DrawCurrentHand(Canvas);
		}
		else
		{
			DrawAddedItems(Canvas);
		}
		DrawHands(Canvas);
		DrawProgressMessages(Canvas);
		DrawTeamSelect(Canvas);
		const bool bHUDVisible = ShouldDrawWoTHUD();
		const FWoTHUDSurfaceVisibility Surface = CurrentSurfaceVisibility();
		const bool bMessagesVisible = bHUDVisible && Surface.bMessages;
		// BaseHUD updates LastTimeSeconds inside the overall HUD gate even when a
		// WOTWindow's bHideMessages flag suppresses DrawMessages.
		const float MessageDeltaTime = UpdateMessageRenderTime(bHUDVisible, Now);
		if (ShouldDrawMessages(bMessagesVisible))
		{
			DrawMessages(Canvas, MessageDeltaTime);
		}
		if (bHUDVisible)
		{
			if (bTurtleEnabled)
			{
				UpdateTurtle(MessageDeltaTime);
			}
			if (bPlayerIDEnabled)
			{
				UpdatePlayerID(Canvas, MessageDeltaTime);
			}
			if (FlashTextureTimer > 0.0f)
			{
				DrawFlashTexture(Canvas, MessageDeltaTime);
			}
			DrawWarning(Canvas);
		}
		// Scoreboard overlays every ordinary HUD element and remains immediately before
		// the opt-in developer readout.
		DrawArenaScoreboard(Canvas);
		DrawCitadelTeamOverview(Canvas);
	}
	if (CVarWoTHUDDebug.GetValueOnGameThread() != 0)
	{
		DrawDebugReadout(Canvas);
	}
}

void AWoTHUD::EndPlay(const EEndPlayReason::Type Reason)
{
	UnbindEditorModeController();
	SetHUDMode(EWoTHUDMode::Normal, false);
	AWoTPlayerController* WoTController =
		Cast<AWoTPlayerController>(GetOwningPlayerController());
	if (IsValid(WoTController))
	{
		WoTController->ClearCitadelSelectionPresentation();
	}
	Super::EndPlay(Reason);
}
