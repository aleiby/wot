#include "WoTMenuStyle.h"

#include "Async/Async.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/AudioComponent.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/UserInterfaceSettings.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformProcess.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "WoTBootstrapHeartbeat.h"
#include "WoTHUDFonts.h"
#include "WoTMenuStrings.h"
#include "WoTPayloadSoundWave.h"
#include "WoTPayloadSubsystem.h"
#include "WoTPreferencesSubsystem.h"
#include "Payload/WoTFrontEndFontOwner.h"
#include "Payload/WoTRuntimeLocalizationDefaults.h"
#include "Payload/WoTRuntimeRegistry.h"
#include "UI/WoTControllerUIInput.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeWorldProperties.h"
#include "World/WoTWorldMaterializer.h"


namespace
{
struct FWoTIntegrityRecoveryPresentationSnapshot
{
	FString Identity;
	FSlateFontInfo Title;
	FSlateFontInfo List;
	TMap<FName, FString> Localization;
	TStrongObjectPtr<UObject> TitleOwner;
	TStrongObjectPtr<UObject> ListOwner;
	bool bValid = false;
};

FWoTIntegrityRecoveryPresentationSnapshot& WoTIntegrityRecoverySnapshot()
{
	// Process-lifetime retention owns the already verified font objects across
	// the failed travel. A normal startup replaces the snapshot before use.
	static FWoTIntegrityRecoveryPresentationSnapshot* Snapshot =
		new FWoTIntegrityRecoveryPresentationSnapshot();
	return *Snapshot;
}
}

void FWoTBrowserSortState::Select(EWoTBrowserSortColumn NewColumn)
{
	if (Column == NewColumn)
	{
		bDescending = !bDescending;
	}
	else
	{
		Column = NewColumn;
		bDescending = false;
	}
}

int32 FWoTBrowserSortState::CompareText(const FString& A, const FString& B)
{
	// UBrowserServerGrid.uc:352-359,381-388 delegates to appStrcmp.
	return A.Compare(B, ESearchCase::CaseSensitive);
}

float FWoTMenuLayout::CalculateSpacing(
	int32 MenuLength, float ItemHeight, float AvailableHeight, float SpacingRatio)
{
	if (MenuLength <= 1)
	{
		return 0.0f;
	}

	// menuWOT.uc:149-179. If all rows fit, the requested gap is YL*ratio.
	// Otherwise MenuListSpaceY remains the available height and the same final
	// division deliberately returns a negative overlap.
	const float RowsHeight = MenuLength * ItemHeight;
	// menuWOT declares MenuListSpaceY and MenuListSpacingY as int. UnrealScript
	// float-to-int assignment truncates toward zero at both assignments.
	int32 MenuListSpace = static_cast<int32>(AvailableHeight);
	if (RowsHeight <= MenuListSpace)
	{
		const float IdealSpace =
			RowsHeight + (MenuLength - 1) * ItemHeight * SpacingRatio;
		MenuListSpace = static_cast<int32>(
			RowsHeight <= IdealSpace ? IdealSpace : RowsHeight);
	}
	return static_cast<float>(static_cast<int32>(
		(MenuListSpace - RowsHeight) / (MenuLength - 1)));
}

float FWoTMenuLayout::CalculateListHeight(
	int32 MenuLength, float ItemHeight, float Spacing)
{
	return MenuLength <= 0
		? 0.0f
		: MenuLength * ItemHeight + (MenuLength - 1) * Spacing;
}

FWoTRetailMenuLayout FWoTMenuLayout::CalculateRetailMainLayout(
	FVector2D ViewportSize, TConstArrayView<FVector2D> MeasuredLabels)
{
	ViewportSize.X = FMath::Max(0.0f, ViewportSize.X);
	ViewportSize.Y = FMath::Max(0.0f, ViewportSize.Y);
	FWoTRetailMenuLayout Result;
	const float SafeWidth = FMath::Min(
		ViewportSize.X, ViewportSize.Y * 4.0f / 3.0f);
	const float SafeLeft = (ViewportSize.X - SafeWidth) * 0.5f;
	Result.SafeRegion = FSlateRect(
		SafeLeft, 0.0f, SafeLeft + SafeWidth, ViewportSize.Y);
	for (const FVector2D& Measurement : MeasuredLabels)
	{
		Result.ListSize.X = FMath::Max(Result.ListSize.X, Measurement.X);
		Result.ItemHeight = FMath::Max(Result.ItemHeight, Measurement.Y);
	}
	Result.Spacing = CalculateSpacing(
		MeasuredLabels.Num(), Result.ItemHeight, ViewportSize.Y);
	Result.ListSize.Y = CalculateListHeight(
		MeasuredLabels.Num(), Result.ItemHeight, Result.Spacing);
	Result.ListPosition = FVector2D(
		SafeLeft + (SafeWidth - Result.ListSize.X) * 0.5f,
		(ViewportSize.Y - Result.ListSize.Y) * 0.5f);
	return Result;
}

FVector2D FWoTMenuLayout::MeasureText(
	const FText& Text, const FSlateFontInfo& Font)
{
	if (FSlateApplication::IsInitialized())
	{
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			return Renderer->GetFontMeasureService()->Measure(Text, Font);
		}
	}
	// Main-menu placement is authenticated-font measurement, not an estimate.
	// A missing Slate renderer is therefore unavailable state, not permission to
	// synthesize advances from string length.
	return FVector2D::ZeroVector;
}

float FWoTMenuLayout::MeasureFontHeight(const FSlateFontInfo& Font)
{
	if (FSlateApplication::IsInitialized())
	{
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			const TSharedRef<FSlateFontMeasure> Measure =
				Renderer->GetFontMeasureService();
			return Measure->Measure(TEXT("X"), Font).Y;
		}
	}
	return static_cast<float>(Font.Size);
}

float FWoTMenuLayout::ScaleX(float Value, float ViewportWidth)
{
	// LegendCanvas.uc:18-30 scales down below the 640-wide base, never up.
	return ViewportWidth >= 1.0f && ViewportWidth < BaseWidth
		? Value * ViewportWidth / BaseWidth
		: Value;
}

float FWoTMenuLayout::LeftColumnStart(float ViewportWidth)
{
	return ViewportWidth * 0.5f - ScaleX(LeftColumnOffset, ViewportWidth);
}

float FWoTMenuLayout::RightColumnStart(float ViewportWidth)
{
	return ViewportWidth * 0.5f + ScaleX(RightColumnOffset, ViewportWidth);
}

const TCHAR* FWoTFrontEndFeedbackContract::MoveSelectorLogicalId()
{
	return TEXT("cache/sound/WOT__MoveSelector");
}

const TCHAR* FWoTFrontEndFeedbackContract::SelectLogicalId()
{
	return TEXT("cache/sound/WOT__Select");
}

bool FWoTFrontEndFeedbackContract::ShouldPlay(
	float AuthenticatedBootstrapVolume, float AppVolumeMultiplier)
{
	return AuthenticatedBootstrapVolume > 0.0f
		&& AppVolumeMultiplier > 0.0f;
}

bool FWoTFrontEndMenuSelection::MoveTo(int32 NewIndex)
{
	if (NewIndex < 0 || NewIndex >= EntryCount || NewIndex == SelectedIndex)
	{
		return false;
	}
	SelectedIndex = NewIndex;
	Feedback->MoveSelector();
	return true;
}

bool FWoTFrontEndMenuSelection::Activate(
	int32 Index, TFunctionRef<void()> Action)
{
	if (Index < 0 || Index >= EntryCount || !Feedback->IsReady())
	{
		return false;
	}
	MoveTo(Index);
	Feedback->Select();
	Action();
	return true;
}

FSlateFontInfo FWoTMenuFontSelection::Select(
	UFont* RetailFont, float Size, FName FallbackTypeface)
{
	if (RetailFont)
	{
		return FSlateFontInfo(RetailFont, Size, FallbackTypeface);
	}
	return FCoreStyle::GetDefaultFontStyle(FallbackTypeface, Size);
}

void FWoTRequiredMenuFontGeneration::BindWidget(
	const TSharedRef<SWidget>& Widget)
{
	if (!bValid)
	{
		Widget->SetVisibility(EVisibility::Collapsed);
		return;
	}
	Widgets.Add(Widget);
}

void FWoTRequiredMenuFontGeneration::Revoke()
{
	bValid = false;
	for (const TWeakPtr<SWidget>& WeakWidget : Widgets)
	{
		if (const TSharedPtr<SWidget> Widget = WeakWidget.Pin())
			Widget->SetVisibility(EVisibility::Collapsed);
	}
	Widgets.Empty();
}

TSharedRef<FWoTRequiredMenuFontGeneration>
FWoTRequiredMenuFontGeneration::CreateForIntegrityRecovery()
{
	TSharedRef<FWoTRequiredMenuFontGeneration> Result =
		MakeShared<FWoTRequiredMenuFontGeneration>();
	Result->bValid = true;
	return Result;
}

TSharedPtr<FWoTFrontEndPresentationAuthority>
FWoTFrontEndPresentationAuthority::Create(
	APlayerController* Controller)
{
	if (Controller == nullptr || !IsInGameThread()) return nullptr;
	TSharedRef<FWoTFrontEndPresentationAuthority> Result =
		MakeShareable(new FWoTFrontEndPresentationAuthority());
	if (!Result->Initialize(*Controller)
		&& !Result->InitializeIntegrityRecovery(*Controller)) return nullptr;
	return Result;
}

FWoTFrontEndPresentationAuthority::FWoTFrontEndPresentationAuthority() = default;

void FWoTFrontEndPresentationAuthority::ShutdownIntegrityRecoverySnapshot()
{
	check(IsInGameThread());
	WoTIntegrityRecoverySnapshot() = FWoTIntegrityRecoveryPresentationSnapshot();
}

FWoTFrontEndPresentationAuthority::~FWoTFrontEndPresentationAuthority()
{
	ReleaseRevocationSubscription();
	if (IsInGameThread())
	{
		RevokeOnGameThread();
		return;
	}
	FEvent* Completed = FPlatformProcess::GetSynchEventFromPool(true);
	AsyncTask(ENamedThreads::GameThread, [this, Completed]()
	{
		RevokeOnGameThread();
		Completed->Trigger();
	});
	Completed->Wait();
	FPlatformProcess::ReturnSynchEventToPool(Completed);
}

bool FWoTFrontEndPresentationAuthority::Initialize(
	APlayerController& Controller)
{
	UGameInstance* Instance = Controller.GetGameInstance();
	UWoTPayloadSubsystem* Payload = Instance
		? Instance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	const UWoTPreferencesSubsystem* Preferences = Instance
		? Instance->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
	const FWoTFrontEndFontOwner* FontOwner =
		Payload ? Payload->FrontEndFontOwnerForScene() : nullptr;
	if (Payload == nullptr || Preferences == nullptr || FontOwner == nullptr
		|| !Preferences->HasAdoptedAuthenticatedEarlyUI()) return false;

	PinnedLease = FontOwner->RetainPresentationLease();
	if (!PinnedLease.IsValid() || !PinnedLease->CanPublishPresentation()
		|| !Payload->SharesSnapshotWith(*PinnedLease)
		|| !FontOwner->SharesSnapshotWith(*PinnedLease)) return false;
	CapturedSnapshotIdentity = PinnedLease->GetManifestSha256().ToHex();
	PlaybackContext = &Controller;
	PayloadOwner = Payload;
	bValid = true;
	const TWeakPtr<FWoTFrontEndPresentationAuthority> WeakThis = AsShared();
	RevocationSubscription = PinnedLease->SubscribeRevocation([WeakThis]()
	{
		auto Teardown = [WeakThis]()
		{
			if (const TSharedPtr<FWoTFrontEndPresentationAuthority> Authority =
				WeakThis.Pin()) Authority->RevokeOnGameThread();
		};
		if (IsInGameThread())
		{
			Teardown();
			return true;
		}
		FEvent* Completed = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread,
			[Teardown = MoveTemp(Teardown), Completed]() mutable
			{
				Teardown();
				Completed->Trigger();
			});
		Completed->Wait();
		FPlatformProcess::ReturnSynchEventToPool(Completed);
		return true;
	});
	if (RevocationSubscription == 0)
	{
		bValid = false;
		return false;
	}

	const TOptional<FWoTRequiredMenuFonts> Resolved =
		FWoTMenuStyle::Get().ResolveRequiredFonts();
	if (!Resolved.IsSet())
	{
		RevokeOnGameThread();
		return false;
	}
	Fonts = Resolved.GetValue();
	FWoTRuntimeRegistry& RuntimeRegistry = FWoTRuntimeRegistry::Get();
	if (!RuntimeRegistry.SharesSnapshotWith(*PinnedLease)
		|| !FWoTMenuStyle::Get().LoadAuthenticatedMainMenuLocalization(
			Fonts, Localization)
		|| !RuntimeRegistry.SharesSnapshotWith(*PinnedLease))
	{
		RevokeOnGameThread();
		return false;
	}
	LocalizationSnapshotIdentity = CapturedSnapshotIdentity;
	bAudioEnabled = FWoTFrontEndFeedbackContract::ShouldPlay(
		Preferences->GetPreferences().EarlyUI.BootstrapVolume,
		FApp::GetVolumeMultiplier());
	if (!BindWave(*Payload,
		FWoTFrontEndFeedbackContract::MoveSelectorLogicalId(),
		MoveSelectorWave, MoveSelectorPayload)
		|| !BindWave(*Payload, FWoTFrontEndFeedbackContract::SelectLogicalId(),
			SelectWave, SelectPayload)
		|| !ValidateCapturedIdentity())
	{
		RevokeOnGameThread();
		return false;
	}
	FWoTIntegrityRecoveryPresentationSnapshot& Recovery =
		WoTIntegrityRecoverySnapshot();
	Recovery.Identity = CapturedSnapshotIdentity;
	Recovery.Title = Fonts.Title;
	Recovery.List = Fonts.List;
	Recovery.Localization = Localization;
	Recovery.TitleOwner.Reset(
		const_cast<UObject*>(Fonts.Title.FontObject.Get()));
	Recovery.ListOwner.Reset(
		const_cast<UObject*>(Fonts.List.FontObject.Get()));
	Recovery.bValid = Recovery.TitleOwner.IsValid()
		&& Recovery.ListOwner.IsValid();
	return true;
}

bool FWoTFrontEndPresentationAuthority::InitializeIntegrityRecovery(
	APlayerController& Controller)
{
	if (!IsWoTFrontEndIntegrityRecoveryAuthorized()) return false;
	FWoTIntegrityRecoveryPresentationSnapshot& Recovery =
		WoTIntegrityRecoverySnapshot();
	if (!Recovery.bValid || Recovery.Identity.IsEmpty()
		|| !Recovery.TitleOwner.IsValid() || !Recovery.ListOwner.IsValid()
		|| Recovery.Localization.Num() < 4) return false;
	CapturedSnapshotIdentity = Recovery.Identity;
	LocalizationSnapshotIdentity = Recovery.Identity;
	Fonts.Title = Recovery.Title;
	Fonts.List = Recovery.List;
	Fonts.Generation =
		FWoTRequiredMenuFontGeneration::CreateForIntegrityRecovery();
	Localization = Recovery.Localization;
	PlaybackContext = &Controller;
	bIntegrityRecovery = true;
	bAudioEnabled = false;
	bValid = Fonts.IsValid();
	return bValid;
}

bool FWoTFrontEndPresentationAuthority::BindWave(
	UWoTPayloadSubsystem& Payload, const TCHAR* LogicalId,
	TStrongObjectPtr<UWoTPayloadSoundWave>& OutWave,
	TSharedPtr<const FWoTSoundPayloadData>& OutPayload)
{
	OutPayload = Payload.LoadSoundPayload(LogicalId);
	if (!OutPayload.IsValid()) return false;
	UWoTPayloadSoundWave* Wave = NewObject<UWoTPayloadSoundWave>(
		GetTransientPackage(), NAME_None, RF_Transient);
	if (Wave == nullptr) return false;
	Wave->PayloadKey = LogicalId;
	Wave->PayloadChannels = OutPayload->Channels;
	Wave->PayloadSampleRate = OutPayload->SampleRate;
	Wave->PayloadBitsPerSample = OutPayload->BitsPerSample;
	Wave->PayloadFrameCount = static_cast<int32>(FMath::Min<uint64>(
		OutPayload->FrameCount, MAX_int32));
	Wave->bLooping = false;
	if (!Payload.InjectSound(Wave, LogicalId)
		|| !Payload.IsInjectedSoundBound(Wave, OutPayload)) return false;
	OutWave.Reset(Wave);
	return true;
}

bool FWoTFrontEndPresentationAuthority::ValidateCapturedIdentity() const
{
	if (bIntegrityRecovery)
	{
		return bValid && IsWoTFrontEndIntegrityRecoveryAuthorized()
			&& !CapturedSnapshotIdentity.IsEmpty()
			&& LocalizationSnapshotIdentity == CapturedSnapshotIdentity
			&& Localization.Num() >= 4 && Fonts.IsValid()
			&& PlaybackContext.IsValid();
	}
	if (!bValid || !PinnedLease.IsValid()
		|| !PinnedLease->CanPublishPresentation()
		|| CapturedSnapshotIdentity.IsEmpty()
		|| LocalizationSnapshotIdentity != CapturedSnapshotIdentity
		|| Localization.Num() < 4 || !Fonts.IsValid()
		|| !PlaybackContext.IsValid() || !PayloadOwner.IsValid()) return false;
	const FWoTFrontEndFontOwner* FontOwner =
		FWoTFrontEndFontOwner::GetActive();
	const UWoTPayloadSubsystem* Payload = PayloadOwner.Get();
	return FontOwner != nullptr && FontOwner->SharesSnapshotWith(*PinnedLease)
		&& Payload->SharesSnapshotWith(*PinnedLease)
		&& FWoTRuntimeRegistry::Get().SharesSnapshotWith(*PinnedLease)
		&& MoveSelectorWave.IsValid()
		&& MoveSelectorWave->PayloadKey
			== FWoTFrontEndFeedbackContract::MoveSelectorLogicalId()
		&& SelectWave.IsValid()
		&& SelectWave->PayloadKey
			== FWoTFrontEndFeedbackContract::SelectLogicalId()
		&& Payload->IsInjectedSoundBound(
			MoveSelectorWave.Get(), MoveSelectorPayload)
		&& Payload->IsInjectedSoundBound(SelectWave.Get(), SelectPayload);
}

bool FWoTFrontEndPresentationAuthority::IsReady() const
{
	if (bTestingAuthority)
	{
		return bValid && !CapturedSnapshotIdentity.IsEmpty()
			&& LocalizationSnapshotIdentity == CapturedSnapshotIdentity
			&& Localization.Num() >= 4 && Fonts.Title.HasValidFont()
			&& Fonts.List.HasValidFont() && TestingFeedback.IsValid()
			&& TestingFeedback->IsReady();
	}
	return ValidateCapturedIdentity();
}

const FWoTRequiredMenuFonts*
FWoTFrontEndPresentationAuthority::RequiredFonts() const
{
	return IsReady() ? &Fonts : nullptr;
}

FSlateFontInfo FWoTFrontEndPresentationAuthority::MultiplayerFont(FName Face, float ViewportScale,
	int32 MinimumPixelScale) const
{
	if (!IsReady() || !FMath::IsFinite(ViewportScale) || ViewportScale <= 0.0f)
		return FSlateFontInfo();
	const int32 PixelScale = FMath::Clamp(
		FMath::Max(FMath::RoundToInt(ViewportScale), MinimumPixelScale), 1, 2);
	FSlateFontInfo Font;
#if WITH_AUTOMATION_TESTS
	if (bTestingAuthority || bIntegrityRecovery)
	{
		Font = Face == FName(TEXT("F_WOTReg30")) ? Fonts.Title : Fonts.List;
		Font.Size *= PixelScale;
	}
	else
#endif
	{
#if !WITH_AUTOMATION_TESTS
		if (bIntegrityRecovery)
		{
			Font = Face == FName(TEXT("F_WOTReg30")) ? Fonts.Title : Fonts.List;
			Font.Size *= PixelScale;
		}
		else
#endif
		{
		UWoTPayloadSubsystem* Payload = PayloadOwner.Get();
		FWoTFrontEndFontOwner* Owner = Payload ? Payload->FrontEndFontOwnerForScene() : nullptr;
		if (!Owner || !PinnedLease.IsValid() || !Owner->SharesSnapshotWith(*PinnedLease))
			return FSlateFontInfo();
		auto Resolved = Owner->ResolveIntegerScaled(Face, PixelScale);
		// A failed optional larger strike must not select Slate's engine-font
		// fallback; retain the authenticated native face at its original size.
		if (Resolved.IsError() && PixelScale > 1) Resolved = Owner->ResolveIntegerScaled(Face, 1);
		// Even if both optional pixel-perfect variants are rejected, keep the
		// already validated original face; empty font info selects Slate fallback.
		if (Resolved.IsError()) Resolved = Owner->Resolve(Face);
		if (Resolved.IsError() || !IsReady()) return FSlateFontInfo();
		Font = Resolved.GetValue();
		}
	}
	// Owner sizes are bitmap pixels, not points. Integer strikes are expanded
	// before entering Slate, so its alpha atlas is sampled at 1:1 on screen.
	Font.Size *= 72.0f / (FontConstants::RenderDPI * ViewportScale);
	return Font;
}

FText FWoTFrontEndPresentationAuthority::Text(FName Id) const
{
	if (!IsReady()) return FText::GetEmpty();
	const FString* Value = Localization.Find(Id);
	return Value ? FText::FromString(*Value) : FText::GetEmpty();
}

bool FWoTFrontEndPresentationAuthority::PrepareBrowserResources()
{
	if (!IsInGameThread() || !IsReady()) return false;
#if WITH_AUTOMATION_TESTS
	if (bBrowserResourcesUnavailableForTesting) return false;
#endif
	if (BrowserResources.IsValid()) return true;
	auto Fail = [this](const FString& Cause)
	{
		BrowserFailureMessage = FText::FromString(TEXT("Multiplayer resources are unavailable."));
		WoTRecordBootstrapHeartbeat(TEXT("browser-resource-failure"),
			EWoTBootstrapPhase::Ready, 0, {}, {}, Cause.Left(512));
		return false;
	};
#if WITH_AUTOMATION_TESTS
	if (bTestingAuthority)
	{
		// Explicit injected presentation fixtures do not have a payload lease.
		BrowserResources = FWoTRetailBrowserStyle::CreateForTesting(Fonts.List);
		BrowserFailureMessage = FText::GetEmpty();
		return true;
	}
#endif
	if (bIntegrityRecovery)
		return Fail(TEXT("Multiplayer resources are unavailable during cache recovery"));
	static const TCHAR* RequiredLabels[] = {
		TEXT("menuStartArenaServer.MenuTitle"), TEXT("menuStartArenaServer.MenuList[1]"),
		TEXT("menuStartBattleServer.MenuTitle"), TEXT("menuStartBattleServer.MenuList[6]"),
		TEXT("menuStartBattleServer.MenuList[7]"),
		TEXT("menuStartArenaServer.MenuList[2]"), TEXT("menuStartArenaServer.MenuList[3]"),
		TEXT("menuStartArenaServer.MenuList[5]"), TEXT("menuStartArenaServer.MenuList[7]"),
		TEXT("menuMultiPlayer.HelpMessage[1]")};
	for (const TCHAR* Label : RequiredLabels)
		if (Text(FName(Label)).IsEmpty())
			return Fail(FString(TEXT("Authenticated multiplayer label is unavailable: ")) + Label);
	UWoTPayloadSubsystem* Payload = PayloadOwner.Get();
	FWoTFrontEndFontOwner* Owner = Payload ? Payload->FrontEndFontOwnerForScene() : nullptr;
	if (!Owner || !PinnedLease.IsValid() || !Owner->SharesSnapshotWith(*PinnedLease))
		return Fail(TEXT("Browser font owner does not share the captured presentation lease"));
	auto Loaded = Owner->EnsureBrowserFont(*Payload);
	if (Loaded.IsError()) return Fail(Loaded.GetError().ToString());
	auto Font = Owner->Resolve(FName(TEXT("Tahoma20")));
	if (Font.IsError()) return Fail(Font.GetError().ToString());
	FString Failure;
	// The multiplayer menus use
	// the port's dark game skin, never the retail Win95 UWindow look
	// (PORT_NOTES "Menu skin, fonts and sliders"). The authenticated face and
	// captured lease still supply the text.
	TSharedPtr<FWoTRetailBrowserStyle> Prepared = FWoTRetailBrowserStyle::CreateAuthenticated(
		*Payload, *PinnedLease, Font.GetValue(), Failure, true);
	if (!Prepared.IsValid()) return Fail(Failure);
	if (!IsReady()) return Fail(TEXT("Browser resource publication lost its captured presentation lease"));
	BrowserResources = MoveTemp(Prepared);
	BrowserFailureMessage = FText::GetEmpty();
	return true;
}

const FWoTRetailBrowserStyle* FWoTFrontEndPresentationAuthority::BrowserStyle() const
{
	return IsReady() ? BrowserResources.Get() : nullptr;
}

FText FWoTFrontEndPresentationAuthority::BrowserResourceError() const
{
	return IsReady() ? BrowserFailureMessage : FText::GetEmpty();
}

void FWoTFrontEndPresentationAuthority::BindWidget(
	const TSharedRef<SWidget>& Widget)
{
	if (!IsReady())
	{
		Widget->SetVisibility(EVisibility::Collapsed);
		return;
	}
	BoundWidgets.RemoveAll(
		[&Widget](const TWeakPtr<SWidget>& Existing)
		{
			const TSharedPtr<SWidget> Pinned = Existing.Pin();
			return !Pinned.IsValid() || Pinned == Widget;
		});
	BoundWidgets.Add(Widget);
}

void FWoTFrontEndPresentationAuthority::MoveSelector()
{
	if (!IsReady()) return;
	if (TestingFeedback.IsValid()) TestingFeedback->MoveSelector();
	else Play(MoveSelectorWave.Get());
}

void FWoTFrontEndPresentationAuthority::Select()
{
	if (!IsReady()) return;
	if (TestingFeedback.IsValid()) TestingFeedback->Select();
	else Play(SelectWave.Get());
}

void FWoTFrontEndPresentationAuthority::Play(UWoTPayloadSoundWave* Wave)
{
	if (!bAudioEnabled || !IsReady() || Wave == nullptr) return;
	ActiveFeedbackComponents.RemoveAll(
		[](const TWeakObjectPtr<UAudioComponent>& Component)
		{
			return !Component.IsValid();
		});
	if (UAudioComponent* Component = UGameplayStatics::SpawnSound2D(
		PlaybackContext.Get(), Wave, 1.0f, 1.0f, 0.0f, nullptr,
		false, true))
	{
		ActiveFeedbackComponents.Add(Component);
	}
}

void FWoTFrontEndPresentationAuthority::RevokeOnGameThread()
{
	check(IsInGameThread());
	// Invalidate first. No callback, visibility attribute, or input event may
	// observe a partially torn-down presentation.
	bValid = false;
#if WITH_AUTOMATION_TESTS
	bRevocationBarrierRan = true;
#endif
	for (const TWeakPtr<SWidget>& WeakWidget : BoundWidgets)
	{
		if (const TSharedPtr<SWidget> Widget = WeakWidget.Pin())
			Widget->SetVisibility(EVisibility::Collapsed);
	}
	BoundWidgets.Empty();
	if (BrowserResources.IsValid()) BrowserResources->ReleaseAuthenticatedResources();
	BrowserFailureMessage = FText::GetEmpty();
	for (const TWeakObjectPtr<UAudioComponent>& WeakComponent
		: ActiveFeedbackComponents)
	{
		if (UAudioComponent* Component = WeakComponent.Get())
		{
			Component->Stop();
			Component->DestroyComponent();
		}
	}
	ActiveFeedbackComponents.Empty();
	if (UWoTPayloadSubsystem* Payload = PayloadOwner.Get())
	{
		if (MoveSelectorWave.IsValid())
			Payload->RemoveInjectedSound(MoveSelectorWave.Get());
		if (SelectWave.IsValid())
			Payload->RemoveInjectedSound(SelectWave.Get());
	}
	MoveSelectorWave.Reset();
	SelectWave.Reset();
	MoveSelectorPayload.Reset();
	SelectPayload.Reset();
	Localization.Empty();
	LocalizationSnapshotIdentity.Empty();
	Fonts = FWoTRequiredMenuFonts{};
	TestingFeedback.Reset();
}

void FWoTFrontEndPresentationAuthority::ReleaseRevocationSubscription()
{
	if (PinnedLease.IsValid() && RevocationSubscription != 0)
	{
		PinnedLease->UnsubscribeRevocation(RevocationSubscription);
		RevocationSubscription = 0;
	}
}

#if WITH_AUTOMATION_TESTS
TSharedRef<FWoTFrontEndPresentationAuthority>
FWoTFrontEndPresentationAuthority::CreateForTesting(
	FString SnapshotIdentity, FSlateFontInfo Title, FSlateFontInfo List,
	TMap<FName, FString>&& InLocalization,
	TSharedRef<IWoTFrontEndFeedbackSink> Feedback)
{
	TSharedRef<FWoTFrontEndPresentationAuthority> Result =
		MakeShareable(new FWoTFrontEndPresentationAuthority());
	Result->bTestingAuthority = true;
	Result->bValid = true;
	Result->CapturedSnapshotIdentity = MoveTemp(SnapshotIdentity);
	Result->LocalizationSnapshotIdentity = Result->CapturedSnapshotIdentity;
	Result->Fonts.Title = MoveTemp(Title);
	Result->Fonts.List = MoveTemp(List);
	Result->Localization = MoveTemp(InLocalization);
	Result->TestingFeedback = MoveTemp(Feedback);
	return Result;
}

void FWoTFrontEndPresentationAuthority::SetBrowserLookAndFeelForTesting(
	const FWoTBrowserLookAndFeel& LookAndFeel)
{
	BrowserResources = FWoTRetailBrowserStyle::CreateForTesting(
		Fonts.List, &LookAndFeel);
	BrowserFailureMessage = FText::GetEmpty();
	bBrowserResourcesUnavailableForTesting = false;
}

void FWoTFrontEndPresentationAuthority::SetBrowserResourcesUnavailableForTesting(
	bool bUnavailable)
{
	bBrowserResourcesUnavailableForTesting = bUnavailable;
	if (bUnavailable)
	{
		BrowserResources.Reset();
	}
}

void FWoTFrontEndPresentationAuthority::RevokeForTesting()
{
	RevokeOnGameThread();
}

bool FWoTFrontEndPresentationAuthority::DidRunRevocationBarrierForTesting() const
{
	return bRevocationBarrierRan;
}

bool FWoTFrontEndPresentationAuthority::HasCapturedFeedbackBindingsForTesting() const
{
	const UWoTPayloadSubsystem* Payload = PayloadOwner.Get();
	return Payload != nullptr && MoveSelectorWave.IsValid()
		&& SelectWave.IsValid()
		&& Payload->IsInjectedSoundBound(MoveSelectorWave.Get(), MoveSelectorPayload)
		&& Payload->IsInjectedSoundBound(SelectWave.Get(), SelectPayload);
}

bool FWoTFrontEndPresentationAuthority::HasLiveLeaseForTesting() const
{
	return PinnedLease.IsValid() && PinnedLease->CanPublishPresentation();
}

int32 FWoTFrontEndPresentationAuthority::ActiveFeedbackComponentCountForTesting() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<UAudioComponent>& Component
		: ActiveFeedbackComponents)
	{
		Count += Component.IsValid() ? 1 : 0;
	}
	return Count;
}
#endif

FWoTMenuStyle& FWoTMenuStyle::Get()
{
	static FWoTMenuStyle Style;
	return Style;
}

TOptional<FWoTRequiredMenuFonts> FWoTMenuStyle::ResolveRequiredFonts() const
{
	const FWoTFrontEndFontOwner* Owner = FWoTFrontEndFontOwner::GetActive();
	if (Owner == nullptr)
	{
		return {};
	}
	TWoTBootstrapResult<FSlateFontInfo> Title = Owner->Resolve(
		FName(TEXT("F_WOTReg30")));
	TWoTBootstrapResult<FSlateFontInfo> List = Owner->Resolve(
		FName(TEXT("F_WOTReg14")));
	if (Title.IsError() || List.IsError()) return {};
	FWoTRequiredMenuFonts Result{
		MoveTemp(Title).GetValue(), MoveTemp(List).GetValue(),
		Owner->FontGeneration()};
	return Result.IsValid() ? TOptional<FWoTRequiredMenuFonts>(MoveTemp(Result))
		: TOptional<FWoTRequiredMenuFonts>();
}

float FWoTMenuStyle::ViewportDpiScale(const FVector2D& PhysicalSize)
{
	// SGameLayerManager scales the game layer by this curve value for the
	// viewport's pixel size (the platform DPI it divides out is reapplied by the
	// window), so it is the layout scale of every viewport overlay.
	// Rounded: a widget's local size times its layout scale can land a hair
	// under the viewport's whole pixel count.
	return GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(FIntPoint(
		FMath::RoundToInt32(PhysicalSize.X), FMath::RoundToInt32(PhysicalSize.Y)));
}

float FWoTMenuStyle::PhysicalTextPixels(float Size, float LayoutScale)
{
	return Size * FontConstants::RenderDPI / 72.0f * LayoutScale;
}

float FWoTMenuStyle::LegibleFontSize(float Size, float LayoutScale)
{
	const float PixelsPerPoint =
		PhysicalTextPixels(1.0f, FMath::Max(LayoutScale, 0.01f));
	// The tolerance keeps a size that lands exactly on the floor from rounding
	// up a whole point through float error.
	const float Floor = FMath::CeilToFloat(
		MinimumLegiblePixels / PixelsPerPoint - UE_KINDA_SMALL_NUMBER);
	return FMath::Max(Size, Floor);
}

FSlateFontInfo FWoTMenuStyle::LegibleSmallFont(float LayoutScale) const
{
	FSlateFontInfo Font = Small;
	Font.Size = LegibleFontSize(Small.Size, LayoutScale);
	return Font;
}

FWoTRetailBrowserStyle& FWoTRetailBrowserStyle::Get()
{
	static FWoTRetailBrowserStyle Style;
	return Style;
}

namespace
{
bool WoTBrowserReadRegion(const FWoTRuntimeClassDefaults& Defaults,
	FStringView Name, FWoTBrowserRegion& Out, uint32 ArrayIndex = 0)
{
	const FWoTRuntimeWorldProperty* Region = Defaults.FindClassProperty(
		TEXT("UWindow.UWindowWin95LookAndFeel"), Name, ArrayIndex);
	if (!Region || Region->Type != EWoTRuntimeWorldPropertyType::Struct)
	{
		return false;
	}
	auto ReadMember = [Region](const TCHAR* MemberName, int32& Value)
	{
		const FWoTRuntimeWorldProperty* Member =
			WoTRuntimeWorldStructMember(*Region, MemberName);
		if (!Member)
		{
			Value = 0;
			return true;
		}
		if (!WoTRuntimeWorldPropertyInteger(*Member, Value)) return false;
		return Value >= 0;
	};
	return ReadMember(TEXT("X"), Out.X) && ReadMember(TEXT("Y"), Out.Y)
		&& ReadMember(TEXT("W"), Out.W) && ReadMember(TEXT("H"), Out.H)
		&& Out.W > 0 && Out.H > 0;
}

bool WoTBrowserReadColor(const FWoTRuntimeClassDefaults& Defaults,
	FStringView Name, FLinearColor& Out)
{
	const FWoTRuntimeWorldProperty* Color = Defaults.FindClassProperty(
		TEXT("UWindow.UWindowWin95LookAndFeel"), Name);
	if (!Color || Color->Type != EWoTRuntimeWorldPropertyType::Struct)
	{
		return false;
	}
	uint8 R = 0, G = 0, B = 0;
	auto Read = [Color](const TCHAR* MemberName, uint8& Value)
	{
		const FWoTRuntimeWorldProperty* Member =
			WoTRuntimeWorldStructMember(*Color, MemberName);
		return Member && WoTRuntimeWorldPropertyByte(*Member, Value);
	};
	if (!Read(TEXT("R"), R) || !Read(TEXT("G"), G)
		|| !Read(TEXT("B"), B)) return false;
	Out = FLinearColor(FColor(R, G, B, 255));
	return true;
}

FWoTBrowserLookAndFeel WoTBrowserNeutralLookAndFeel()
{
	// Deliberately the port's slate-gray fallback, not copied retail data.
	FWoTBrowserLookAndFeel Result;
	Result.ScrollbarWidth = 14.0f;
	Result.FrameActiveTitleColor = FLinearColor(0.82f, 0.86f, 0.9f);
	Result.FrameInactiveTitleColor = FLinearColor(0.52f, 0.56f, 0.6f);
	Result.HeadingActiveTitleColor = FLinearColor::Black;
	Result.HeadingInActiveTitleColor = FLinearColor::Black;
	Result.EditBoxTextColor = FLinearColor::Black;
	Result.ColumnHeadingHeight = 13.0f;
	Result.Regions.Add(TEXT("FrameT"), FWoTBrowserRegion{0, 0, 1, 18});
	Result.Regions.Add(TEXT("FrameSB"), FWoTBrowserRegion{0, 0, 1, 14});
	return Result;
}

bool WoTBrowserDecodeLookAndFeel(const FWoTRuntimeClassDefaults& Defaults,
	FWoTBrowserLookAndFeel& Out, FString& OutError)
{
	Out = FWoTBrowserLookAndFeel();
	struct FRequiredRegion { const TCHAR* Name; uint32 Index; };
	const FRequiredRegion Required[] = {
		{TEXT("BevelUpTL"),0},{TEXT("BevelUpT"),0},{TEXT("BevelUpTR"),0},
		{TEXT("BevelUpL"),0},{TEXT("BevelUpArea"),0},{TEXT("BevelUpR"),0},
		{TEXT("BevelUpBL"),0},{TEXT("BevelUpB"),0},{TEXT("BevelUpBR"),0},
		{TEXT("FrameTL"),0},{TEXT("FrameT"),0},{TEXT("FrameTR"),0},
		{TEXT("FrameL"),0},{TEXT("FrameR"),0},{TEXT("FrameSBL"),0},
		{TEXT("FrameSB"),0},{TEXT("FrameSBR"),0},
		{TEXT("MiscBevelTL"),1},{TEXT("MiscBevelT"),1},
		{TEXT("MiscBevelTR"),1},{TEXT("MiscBevelL"),1},
		{TEXT("MiscBevelArea"),1},{TEXT("MiscBevelR"),1},
		{TEXT("MiscBevelBL"),1},{TEXT("MiscBevelB"),1},
		{TEXT("MiscBevelBR"),1},{TEXT("MiscBevelArea"),2},
		{TEXT("MiscBevelTL"),2},{TEXT("SBUpUp"),0},
		{TEXT("SBDownUp"),0},{TEXT("SBLeftUp"),0},
		{TEXT("SBRightUp"),0},{TEXT("SBBackground"),0}};
	for (const FRequiredRegion& RequiredRegion : Required)
	{
		FWoTBrowserRegion Region;
		if (!WoTBrowserReadRegion(Defaults, RequiredRegion.Name, Region,
			RequiredRegion.Index))
		{
			OutError = FString::Printf(TEXT(
				"Authenticated UWindowWin95LookAndFeel region %s[%u] is unavailable"),
				RequiredRegion.Name, RequiredRegion.Index);
			return false;
		}
		const FName Key(RequiredRegion.Index == 0 ? RequiredRegion.Name
			: *FString::Printf(TEXT("%s[%u]"), RequiredRegion.Name,
				RequiredRegion.Index));
		Out.Regions.Add(Key, Region);
	}
	auto ReadImplicitZeroColor = [&Defaults](const TCHAR* Name,
		FLinearColor& Value)
	{
		// UWindowLookAndFeel.uc:24,59 declare these consumed colors without
		// defaultproperties assignments. Absence from the serialized CDO is the
		// authenticated implicit-zero value; an authored mutation still decodes.
		if (!Defaults.FindClassProperty(
			TEXT("UWindow.UWindowWin95LookAndFeel"), Name))
		{
			Value = FLinearColor::Black;
			return true;
		}
		return WoTBrowserReadColor(Defaults, Name, Value);
	};
	if (!WoTBrowserReadColor(Defaults, TEXT("FrameActiveTitleColor"),
			Out.FrameActiveTitleColor)
		|| !WoTBrowserReadColor(Defaults, TEXT("FrameInactiveTitleColor"),
			Out.FrameInactiveTitleColor)
		|| !WoTBrowserReadColor(Defaults, TEXT("HeadingActiveTitleColor"),
			Out.HeadingActiveTitleColor)
		|| !ReadImplicitZeroColor(TEXT("HeadingInActiveTitleColor"),
			Out.HeadingInActiveTitleColor)
		|| !ReadImplicitZeroColor(TEXT("EditBoxTextColor"),
			Out.EditBoxTextColor))
	{
		OutError = TEXT("Authenticated UWindowWin95LookAndFeel title color is unavailable");
		return false;
	}
	const FWoTRuntimeWorldProperty* Width = Defaults.FindClassProperty(
		TEXT("UWindow.UWindowWin95LookAndFeel"), TEXT("Size_ScrollbarWidth"));
	const FWoTRuntimeWorldProperty* HeadingHeight = Defaults.FindClassProperty(
		TEXT("UWindow.UWindowWin95LookAndFeel"), TEXT("ColumnHeadingHeight"));
	int32 HeadingHeightInteger = 0;
	if (!Width || !WoTRuntimeWorldPropertyFloat(*Width, Out.ScrollbarWidth)
		|| !FMath::IsFinite(Out.ScrollbarWidth) || Out.ScrollbarWidth <= 0.0f
		|| !HeadingHeight || !WoTRuntimeWorldPropertyInteger(
			*HeadingHeight, HeadingHeightInteger)
		|| HeadingHeightInteger <= 0)
	{
		OutError = TEXT("Authenticated UWindowWin95LookAndFeel consumed size is unavailable");
		return false;
	}
	Out.ColumnHeadingHeight = static_cast<float>(HeadingHeightInteger);
	OutError.Empty();
	return true;
}

bool WoTBrowserDecodeAtlas(UWoTPayloadSubsystem& Payload, const TCHAR* LogicalId,
	const TCHAR* SourceExport, int32 Side, TArray<FColor>& Pixels)
{
	const TSharedPtr<const FWoTTexturePayloadData> Atlas = Payload.LoadTexturePayload(LogicalId);
	if (!Atlas.IsValid() || Atlas->SourceExport != SourceExport || Atlas->Mips.Num() != 1
		|| Atlas->Palette.Num() != 256 || Atlas->Mips[0].Width != Side || Atlas->Mips[0].Height != Side)
		return false;
	const FWoTTexturePayloadMip& Mip = Atlas->Mips[0];
	const TConstArrayView<uint8> Bytes = Atlas->PayloadBytes();
	if (Mip.StoredByteLength != static_cast<uint64>(Side * Side)
		|| Mip.DataOffset > static_cast<uint64>(Bytes.Num())
		|| Mip.StoredByteLength > static_cast<uint64>(Bytes.Num()) - Mip.DataOffset) return false;
	Pixels.SetNumUninitialized(Side * Side);
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		const uint8 PaletteIndex = Bytes[static_cast<int32>(Mip.DataOffset) + Index];
		Pixels[Index] = Atlas->Palette[PaletteIndex];
		Pixels[Index].A = Atlas->bMasked && PaletteIndex == 0 ? 0 : 255;
	}
	return true;
}

void WoTBrowserCopyRegion(TArray<FColor>& Out, int32 OutWidth,
	TConstArrayView<FColor> Source, int32 SourceWidth,
	int32 X, int32 Y, int32 Width, int32 Height,
	int32 DestinationX, int32 DestinationY, int32 DestinationWidth, int32 DestinationHeight)
{
	for (int32 Row = 0; Row < DestinationHeight; ++Row)
		for (int32 Column = 0; Column < DestinationWidth; ++Column)
			Out[(DestinationY + Row) * OutWidth + DestinationX + Column] =
				Source[(Y + Row % Height) * SourceWidth + X + Column % Width];
}

UTexture2D* WoTBrowserMaterializePixels(int32 Width, int32 Height, TConstArrayView<FColor> Pixels)
{
	if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height) return nullptr;
	UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	if (!Texture) return nullptr;
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Destination = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Destination, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
	Mip.BulkData.Unlock();
	Texture->SRGB = true;
	Texture->Filter = TF_Nearest;
	Texture->NeverStream = true;
	Texture->UpdateResource();
	return Texture;
}
}

TSharedPtr<FWoTRetailBrowserStyle> FWoTRetailBrowserStyle::CreateAuthenticated(
	UWoTPayloadSubsystem& Payload, const FWoTCacheGenerationLease& Lease,
	const FSlateFontInfo& Font, FString& OutError, bool bUseGameSkin)
{
	OutError = TEXT("Browser style requires an authenticated font and the captured payload lease");
	if (!IsInGameThread() || !Lease.IsValid() || !Payload.SharesSnapshotWith(Lease)
		|| !Font.FontObject.Get() || Font.FontFallback != EFontFallback::FF_NoFallback) return nullptr;
	if (bUseGameSkin)
	{
		// Procedural chrome needs no texture import or extra payload dependency.
		// Text still uses the authenticated face and captured presentation lease.
		TSharedRef<FWoTRetailBrowserStyle> Result = MakeShareable(new FWoTRetailBrowserStyle(false));
		Result->Normal = Font;
		Result->LoadedObjects.Emplace(const_cast<UObject*>(Font.FontObject.Get()));
		Result->ConfigureGameSkin();
		OutError.Empty();
		return Result;
	}
	TArray<FColor> Misc, Frame, Highlight;
	FString DefaultsError;
	const FWoTRuntimeClassDefaults* Defaults = Payload.RuntimeClassDefaults(
		DefaultsError);
	OutError = DefaultsError.IsEmpty()
		? TEXT("Authenticated UWindowWin95LookAndFeel defaults are unavailable")
		: DefaultsError;
	if (!Defaults) return nullptr;
	FWoTBrowserLookAndFeel DecodedLook;
	if (!WoTBrowserDecodeLookAndFeel(*Defaults, DecodedLook, OutError))
	{
		return nullptr;
	}
	auto ReadRegion = [&DecodedLook](const TCHAR* Name,
		FWoTBrowserRegion& Out, uint32 Index = 0)
	{
		const FName Key(Index == 0 ? Name
			: *FString::Printf(TEXT("%s[%u]"), Name, Index));
		if (const FWoTBrowserRegion* Found = DecodedLook.Regions.Find(Key))
		{
			Out = *Found;
			return true;
		}
		return false;
	};
	OutError = TEXT("Browser atlas is unavailable or invalid: cache/texture/UWindow__Misc");
	if (!WoTBrowserDecodeAtlas(Payload, TEXT("cache/texture/UWindow__Misc"), TEXT("UWindow.Icons.Misc"), 128, Misc))
		return nullptr;
	OutError = TEXT("Browser atlas is unavailable or invalid: cache/texture/UWindow__ActiveFrameS");
	if (!WoTBrowserDecodeAtlas(Payload, TEXT("cache/texture/UWindow__ActiveFrameS"), TEXT("UWindow.Icons.ActiveFrameS"), 128, Frame))
		return nullptr;
	OutError = TEXT("Browser atlas is unavailable or invalid: cache/texture/UBrowser__Highlight");
	if (!WoTBrowserDecodeAtlas(Payload, TEXT("cache/texture/UBrowser__Highlight"), TEXT("UBrowser.Icons.Highlight"), 16, Highlight))
		return nullptr;
	// Both region-bearing authenticated atlases are 128x128. Prove every
	// decoded rectangle fits before the first pixel sample or brush allocation.
	// Subtraction after the signed range checks is overflow-safe, unlike X+W.
	for (const TPair<FName, FWoTBrowserRegion>& Pair : DecodedLook.Regions)
	{
		const FWoTBrowserRegion& Region = Pair.Value;
		if (Region.X < 0 || Region.Y < 0 || Region.W <= 0 || Region.H <= 0
			|| Region.W > 128 || Region.H > 128
			|| Region.X > 128 - Region.W || Region.Y > 128 - Region.H)
		{
			OutError = FString::Printf(TEXT(
				"Authenticated UWindowWin95LookAndFeel region %s is outside its 128x128 atlas"),
				*Pair.Key.ToString());
			return nullptr;
		}
	}
	TSharedRef<FWoTRetailBrowserStyle> Result = MakeShareable(new FWoTRetailBrowserStyle(false));
	Result->Normal = Font;
	// The current browser consumes only the normal face. Other style faces
	// remain unset rather than silently substituting a different authenticated face.
	Result->LoadedObjects.Emplace(const_cast<UObject*>(Font.FontObject.Get()));
	auto Bind = [&Result](FSlateBrush& Brush, TConstArrayView<FColor> Pixels,
		int32 Width, int32 Height, ESlateBrushDrawType::Type DrawAs, FMargin Margin = FMargin(0.0f))
	{
		// Slate boxes compute fixed borders from ResourceProxy::ActualSize,
		// ignoring brush ImageSize. Replicate authored texels exactly so the
		// texture borders and GUIScale-two widget geometry use the same units.
		const int32 PixelScale = DrawAs == ESlateBrushDrawType::Box ? 2 : 1;
		TArray<FColor> Expanded;
		if (PixelScale == 2)
		{
			Expanded.SetNumUninitialized(Width * Height * 4);
			for (int32 Y = 0; Y < Height * 2; ++Y)
				for (int32 X = 0; X < Width * 2; ++X)
					Expanded[Y * Width * 2 + X] = Pixels[(Y / 2) * Width + X / 2];
			Pixels = Expanded;
		}
		UTexture2D* Texture = WoTBrowserMaterializePixels(
			Width * PixelScale, Height * PixelScale, Pixels);
		if (!Texture) return false;
		Result->LoadedObjects.Emplace(Texture);
		Result->ConfigureRegionBrush(Brush, Texture, FVector2f::ZeroVector,
			FVector2f(Width * PixelScale, Height * PixelScale), DrawAs, Margin);
		// Full-texture UVs above include all replicated pixels. ImageSize is
		// still twice the authored size, never twice the expanded texture.
		Brush.SetImageSize(FVector2D(Width * GUIScale, Height * GUIScale));
		return true;
	};
	auto CropMisc = [&Bind, &Misc](FSlateBrush& Brush, int32 X, int32 Y,
		int32 Width, int32 Height, ESlateBrushDrawType::Type DrawAs, FMargin Margin = FMargin(0.0f))
	{
		TArray<FColor> Pixels;
		Pixels.SetNumZeroed(Width * Height);
		WoTBrowserCopyRegion(Pixels, Width, Misc, 128, X, Y, Width, Height, 0, 0, Width, Height);
		return Bind(Brush, Pixels, Width, Height, DrawAs, Margin);
	};
	// Exact decoded UWindowWin95LookAndFeel regions. Both the pixels and their
	// atlas coordinates come from the captured authenticated generation.
	FWoTBrowserRegion BevelTL, BevelT, BevelTR, BevelL, BevelArea, BevelR,
		BevelBL, BevelB, BevelBR;
	if (!ReadRegion(TEXT("BevelUpTL"), BevelTL)
		|| !ReadRegion(TEXT("BevelUpT"), BevelT)
		|| !ReadRegion(TEXT("BevelUpTR"), BevelTR)
		|| !ReadRegion(TEXT("BevelUpL"), BevelL)
		|| !ReadRegion(TEXT("BevelUpArea"), BevelArea)
		|| !ReadRegion(TEXT("BevelUpR"), BevelR)
		|| !ReadRegion(TEXT("BevelUpBL"), BevelBL)
		|| !ReadRegion(TEXT("BevelUpB"), BevelB)
		|| !ReadRegion(TEXT("BevelUpBR"), BevelBR)) return nullptr;
	// The stock Win95 face (#C0C0C0 in the shipped atlas) is authenticated
	// texture data, sampled from the decoded BevelUpArea rather than transcribed.
	Result->FaceBrush = FSlateColorBrush(FLinearColor(
		Misc[BevelArea.Y * 128 + BevelArea.X]));
	const int32 RaisedWidth = BevelTL.W + BevelT.W + BevelTR.W;
	const int32 RaisedHeight = BevelTL.H + BevelL.H + BevelBL.H;
	TArray<FColor> Raised;
	Raised.SetNumZeroed(RaisedWidth * RaisedHeight);
	const FWoTBrowserRegion* RaisedRegions[] = {&BevelTL, &BevelT, &BevelTR,
		&BevelL, &BevelArea, &BevelR, &BevelBL, &BevelB, &BevelBR};
	const int32 RaisedX[] = {0, BevelTL.W, BevelTL.W + BevelT.W,
		0, BevelL.W, BevelL.W + BevelArea.W,
		0, BevelBL.W, BevelBL.W + BevelB.W};
	const int32 RaisedY[] = {0, 0, 0, BevelTL.H, BevelT.H, BevelTR.H,
		BevelTL.H + BevelL.H, BevelT.H + BevelArea.H,
		BevelTR.H + BevelR.H};
	for (int32 Index = 0; Index < 9; ++Index)
	{
		const FWoTBrowserRegion& Region = *RaisedRegions[Index];
		WoTBrowserCopyRegion(Raised, RaisedWidth, Misc, 128, Region.X, Region.Y,
			Region.W, Region.H, RaisedX[Index], RaisedY[Index], Region.W, Region.H);
	}

	FWoTBrowserRegion FrameTL, FrameT, FrameTR, FrameL, FrameR,
		FrameSBL, FrameSB, FrameSBR;
	if (!ReadRegion(TEXT("FrameTL"), FrameTL)
		|| !ReadRegion(TEXT("FrameT"), FrameT)
		|| !ReadRegion(TEXT("FrameTR"), FrameTR)
		|| !ReadRegion(TEXT("FrameL"), FrameL)
		|| !ReadRegion(TEXT("FrameR"), FrameR)
		|| !ReadRegion(TEXT("FrameSBL"), FrameSBL)
		|| !ReadRegion(TEXT("FrameSB"), FrameSB)
		|| !ReadRegion(TEXT("FrameSBR"), FrameSBR)) return nullptr;
	const int32 WindowWidth = FrameSBL.W + FrameSB.W + FrameSBR.W;
	const int32 WindowHeight = FrameTL.H + FrameL.H + FrameSBL.H;
	TArray<FColor> Window;
	Window.SetNumZeroed(WindowWidth * WindowHeight);
	auto CopyFrame = [&Window, WindowWidth, &Frame](const FWoTBrowserRegion& Region,
		int32 X, int32 Y, int32 W, int32 H)
	{
		WoTBrowserCopyRegion(Window, WindowWidth, Frame, 128, Region.X, Region.Y,
			Region.W, Region.H, X, Y, W, H);
	};
	const int32 TopMiddleWidth = WindowWidth - FrameTL.W - FrameTR.W;
	CopyFrame(FrameTL, 0, 0, FrameTL.W, FrameTL.H);
	CopyFrame(FrameT, FrameTL.W, 0, TopMiddleWidth, FrameT.H);
	CopyFrame(FrameTR, WindowWidth - FrameTR.W, 0, FrameTR.W, FrameTR.H);
	CopyFrame(FrameL, 0, FrameTL.H, FrameL.W, FrameL.H);
	FWoTBrowserRegion FrameArea{
		FrameT.X, FrameL.Y, FrameT.W, FrameL.H};
	CopyFrame(FrameArea, FrameL.W, FrameTL.H,
		WindowWidth - FrameL.W - FrameR.W, FrameL.H);
	CopyFrame(FrameR, WindowWidth - FrameR.W, FrameTR.H, FrameR.W, FrameR.H);
	CopyFrame(FrameSBL, 0, WindowHeight - FrameSBL.H,
		FrameSBL.W, FrameSBL.H);
	CopyFrame(FrameSB, FrameSBL.W, WindowHeight - FrameSB.H,
		FrameSB.W, FrameSB.H);
	CopyFrame(FrameSBR, WindowWidth - FrameSBR.W, WindowHeight - FrameSBR.H,
		FrameSBR.W, FrameSBR.H);

	FWoTBrowserRegion RecessedTL, RecessedT, RecessedTR, RecessedL,
		RecessedArea, RecessedR, RecessedBL, RecessedB, RecessedBR,
		Sunken, SunkenBorder, ScrollUp, ScrollDown, ScrollLeft,
		ScrollRight, ScrollBackground;
	if (!ReadRegion(TEXT("MiscBevelTL"), RecessedTL, 1)
		|| !ReadRegion(TEXT("MiscBevelT"), RecessedT, 1)
		|| !ReadRegion(TEXT("MiscBevelTR"), RecessedTR, 1)
		|| !ReadRegion(TEXT("MiscBevelL"), RecessedL, 1)
		|| !ReadRegion(TEXT("MiscBevelArea"), RecessedArea, 1)
		|| !ReadRegion(TEXT("MiscBevelR"), RecessedR, 1)
		|| !ReadRegion(TEXT("MiscBevelBL"), RecessedBL, 1)
		|| !ReadRegion(TEXT("MiscBevelB"), RecessedB, 1)
		|| !ReadRegion(TEXT("MiscBevelBR"), RecessedBR, 1)
		|| !ReadRegion(TEXT("MiscBevelArea"), Sunken, 2)
		|| !ReadRegion(TEXT("MiscBevelTL"), SunkenBorder, 2)
		|| !ReadRegion(TEXT("SBUpUp"), ScrollUp)
		|| !ReadRegion(TEXT("SBDownUp"), ScrollDown)
		|| !ReadRegion(TEXT("SBLeftUp"), ScrollLeft)
		|| !ReadRegion(TEXT("SBRightUp"), ScrollRight)
		|| !ReadRegion(TEXT("SBBackground"), ScrollBackground)) return nullptr;
	const int32 RecessedWidth = RecessedTL.W + RecessedT.W + RecessedTR.W;
	const int32 RecessedHeight = RecessedTL.H + RecessedL.H + RecessedBL.H;
	TArray<FColor> RecessedPixels;
	RecessedPixels.SetNumZeroed(RecessedWidth * RecessedHeight);
	const FWoTBrowserRegion* RecessedRegions[] = {&RecessedTL, &RecessedT,
		&RecessedTR, &RecessedL, &RecessedArea, &RecessedR,
		&RecessedBL, &RecessedB, &RecessedBR};
	const int32 RecessedX[] = {0, RecessedTL.W, RecessedTL.W + RecessedT.W,
		0, RecessedL.W, RecessedL.W + RecessedArea.W,
		0, RecessedBL.W, RecessedBL.W + RecessedB.W};
	const int32 RecessedY[] = {0, 0, 0, RecessedTL.H, RecessedT.H,
		RecessedTR.H, RecessedTL.H + RecessedL.H,
		RecessedT.H + RecessedArea.H, RecessedTR.H + RecessedR.H};
	for (int32 Index = 0; Index < 9; ++Index)
	{
		const FWoTBrowserRegion& Region = *RecessedRegions[Index];
		WoTBrowserCopyRegion(RecessedPixels, RecessedWidth, Misc, 128,
			Region.X, Region.Y, Region.W, Region.H, RecessedX[Index],
			RecessedY[Index], Region.W, Region.H);
	}
	OutError = TEXT("Authenticated browser brush materialization failed");
	if (!Bind(Result->RaisedBevelBrush, Raised, RaisedWidth, RaisedHeight,
			ESlateBrushDrawType::Box,
			FMargin(static_cast<float>(BevelTL.W) / RaisedWidth))
		|| !Bind(Result->WindowFrameBrush, Window, WindowWidth, WindowHeight,
			ESlateBrushDrawType::Box, FMargin(
				static_cast<float>(FrameTL.W) / WindowWidth,
				static_cast<float>(FrameTL.H) / WindowHeight,
				static_cast<float>(FrameSBR.W) / WindowWidth,
				static_cast<float>(FrameSBR.H) / WindowHeight))
		|| !Bind(Result->SelectionBrush, Highlight, 16, 16, ESlateBrushDrawType::Image)
		|| !Bind(Result->RecessedBevelBrush, RecessedPixels,
			RecessedWidth, RecessedHeight, ESlateBrushDrawType::Box,
			FMargin(static_cast<float>(RecessedTL.W) / RecessedWidth,
				static_cast<float>(RecessedTL.H) / RecessedHeight))
		|| !CropMisc(Result->SunkenBevelBrush, Sunken.X, Sunken.Y,
			Sunken.W, Sunken.H, ESlateBrushDrawType::Box,
			FMargin(static_cast<float>(SunkenBorder.W) / Sunken.W))
		|| !CropMisc(Result->ScrollUpBrush, ScrollUp.X, ScrollUp.Y,
			ScrollUp.W, ScrollUp.H, ESlateBrushDrawType::Image)
		|| !CropMisc(Result->ScrollDownBrush, ScrollDown.X, ScrollDown.Y,
			ScrollDown.W, ScrollDown.H, ESlateBrushDrawType::Image)
		|| !CropMisc(Result->ScrollLeftBrush, ScrollLeft.X, ScrollLeft.Y,
			ScrollLeft.W, ScrollLeft.H, ESlateBrushDrawType::Image)
		|| !CropMisc(Result->ScrollRightBrush, ScrollRight.X, ScrollRight.Y,
			ScrollRight.W, ScrollRight.H, ESlateBrushDrawType::Image)
		|| !CropMisc(Result->ScrollBackgroundBrush, ScrollBackground.X,
			ScrollBackground.Y, ScrollBackground.W, ScrollBackground.H,
			ESlateBrushDrawType::Image)) return nullptr;
	OutError = TEXT("Browser atlas materialization lost the captured payload lease");
	if (!Lease.IsValid() || !Payload.SharesSnapshotWith(Lease)) return nullptr;
	Result->ConfigureWidgetStyles(DecodedLook);
	OutError.Empty();
	return Result;
}

void FWoTRetailBrowserStyle::ReleaseAuthenticatedResources()
{
	// Widget styles contain brush copies; clear those before dropping the
	// strongly owned textures, keeping their addresses valid for retained widgets.
	Normal = FSlateFontInfo();
	Bold = FSlateFontInfo();
	Large = FSlateFontInfo();
	LargeBold = FSlateFontInfo();
	FSlateBrush* Brushes[] = {&WindowFrameBrush, &RaisedBevelBrush, &RecessedBevelBrush,
		&SunkenBevelBrush, &ScrollBackgroundBrush, &ScrollUpBrush, &ScrollDownBrush,
		&ScrollLeftBrush, &ScrollRightBrush, &SelectionBrush};
	for (FSlateBrush* Brush : Brushes) Brush->SetResourceObject(nullptr);
	ConfigureWidgetStyles(LookAndFeel);
	LoadedObjects.Empty();
}

#if WITH_AUTOMATION_TESTS
TSharedRef<FWoTRetailBrowserStyle> FWoTRetailBrowserStyle::CreateForTesting(
	const FSlateFontInfo& Font,
	const FWoTBrowserLookAndFeel* DecodedLookAndFeel)
{
	TSharedRef<FWoTRetailBrowserStyle> Result = MakeShareable(new FWoTRetailBrowserStyle(false));
	Result->Normal = Font;
	Result->ConfigureWidgetStyles(DecodedLookAndFeel
		? *DecodedLookAndFeel : WoTBrowserNeutralLookAndFeel());
	return Result;
}

bool FWoTRetailBrowserStyle::DecodeLookAndFeelForTesting(
	const FWoTRuntimeClassDefaults& Defaults,
	FWoTBrowserLookAndFeel& Out, FString& OutError)
{
	return WoTBrowserDecodeLookAndFeel(Defaults, Out, OutError);
}
#endif

FWoTRetailBrowserStyle::FWoTRetailBrowserStyle(bool bLoadLegacyResources)
{
	LookAndFeel = WoTBrowserNeutralLookAndFeel();
	ClientBrush = FSlateColorBrush(ClientColor());
	// Deliberate port-neutral surface used before/after retail look publication.
	FaceBrush = FSlateColorBrush(FLinearColor(0.75f, 0.75f, 0.75f, 1.0f));
	if (!bLoadLegacyResources) return;
#if WOT_GUARDED_BOOTSTRAP
	UWoTHUDFonts* Registry = nullptr;
#else
	UWoTHUDFonts* Registry = LoadObject<UWoTHUDFonts>(
		nullptr, TEXT("/Game/WoT/UI/Fonts/DA_WoTHUDFonts.DA_WoTHUDFonts"));
	if (Registry)
	{
		LoadedObjects.Emplace(Registry);
	}
#endif

	// UWindowRootWindow.SetupFonts, GUIScale==2 branch: lines 341-346.
	Normal = FWoTMenuFontSelection::Select(
		ResolveFont(Registry, FName(TEXT("Tahoma20"))), 20.0f,
		FName(TEXT("Regular")));
	Bold = FWoTMenuFontSelection::Select(
		ResolveFont(Registry, FName(TEXT("TahomaB20"))), 20.0f,
		FName(TEXT("Bold")));
	Large = FWoTMenuFontSelection::Select(
		ResolveFont(Registry, FName(TEXT("Tahoma30"))), 30.0f,
		FName(TEXT("Regular")));
	LargeBold = FWoTMenuFontSelection::Select(
		ResolveFont(Registry, FName(TEXT("TahomaB30"))), 30.0f,
		FName(TEXT("Bold")));

	// Before authenticated defaults exist, expose only the port's neutral skin.
	// Retail atlases, geometry and colors are never reconstructed from literals.
	ConfigureGameSkin();
}

void FWoTRetailBrowserStyle::ConfigureGameSkin()
{
	bGameSkin = true;
	const FLinearColor Border(0.14f, 0.115f, 0.07f);
	const FLinearColor Panel(0.018f, 0.022f, 0.025f, 0.96f);
	WindowFrameBrush = FSlateRoundedBoxBrush(Panel, 2.0f, Border, 1.0f);
	RaisedBevelBrush = FSlateRoundedBoxBrush(FLinearColor(0.045f, 0.05f, 0.052f), 2.0f, Border, 1.0f);
	RecessedBevelBrush = FSlateRoundedBoxBrush(FLinearColor(0.025f, 0.029f, 0.032f), 2.0f, GameAccentColor(), 1.0f);
	SunkenBevelBrush = FSlateRoundedBoxBrush(FLinearColor(0.008f, 0.012f, 0.015f), 2.0f, Border, 1.0f);
	ClientBrush = FSlateColorBrush(FLinearColor(0.009f, 0.013f, 0.017f, 0.94f));
	FaceBrush = FSlateColorBrush(Panel);
	SelectionBrush = FSlateColorBrush(FLinearColor(0.17f, 0.115f, 0.035f));
	ScrollBackgroundBrush = FSlateColorBrush(Panel);
	ScrollUpBrush = FSlateNoResource();
	ScrollDownBrush = FSlateNoResource();
	ScrollLeftBrush = FSlateNoResource();
	ScrollRightBrush = FSlateNoResource();
	ConfigureWidgetStyles(WoTBrowserNeutralLookAndFeel());
}

void FWoTRetailBrowserStyle::ConfigureWidgetStyles(
	const FWoTBrowserLookAndFeel& InLookAndFeel)
{
	LookAndFeel = InLookAndFeel;
	BrowserButtonStyle
		.SetNormal(RaisedBevelBrush)
		.SetHovered(RaisedBevelBrush)
		.SetPressed(RecessedBevelBrush)
		.SetDisabled(RaisedBevelBrush)
		.SetNormalForeground(FSlateColor(BodyForeground()))
		.SetHoveredForeground(FSlateColor(bGameSkin ? FLinearColor::White : BodyTextColor()))
		.SetPressedForeground(FSlateColor(BodyForeground()))
		.SetDisabledForeground(FSlateColor(DisabledColor()));
	BrowserHeadingStyle = BrowserButtonStyle;
	BrowserHeadingStyle
		.SetNormalForeground(FSlateColor(HeadingForeground(false)))
		.SetHoveredForeground(FSlateColor(bGameSkin ? FLinearColor::White : HeadingForeground()))
		.SetPressedForeground(FSlateColor(HeadingForeground()));

	BrowserScrollBarStyle
		.SetHorizontalBackgroundImage(ScrollBackgroundBrush)
		.SetVerticalBackgroundImage(ScrollBackgroundBrush)
		.SetVerticalTopSlotImage(ScrollUpBrush)
		.SetVerticalBottomSlotImage(ScrollDownBrush)
		.SetHorizontalTopSlotImage(ScrollLeftBrush)
		.SetHorizontalBottomSlotImage(ScrollRightBrush)
		.SetNormalThumbImage(RaisedBevelBrush)
		.SetHoveredThumbImage(RaisedBevelBrush)
		.SetDraggedThumbImage(RecessedBevelBrush)
		.SetThickness(LookAndFeel.ScrollbarWidth * GUIScale);
	BrowserScrollBoxStyle
		.SetBarThickness(LookAndFeel.ScrollbarWidth * GUIScale)
		.SetTopShadowBrush(FSlateNoResource())
		.SetBottomShadowBrush(FSlateNoResource())
		.SetLeftShadowBrush(FSlateNoResource())
		.SetRightShadowBrush(FSlateNoResource());

	BrowserEditableTextBoxStyle
		.SetBackgroundImageNormal(SunkenBevelBrush)
		.SetBackgroundImageHovered(SunkenBevelBrush)
		.SetBackgroundImageFocused(SunkenBevelBrush)
		.SetBackgroundImageReadOnly(SunkenBevelBrush)
		.SetPadding(FMargin(5.0f * GUIScale, 2.0f * GUIScale))
		.SetFont(Normal)
		.SetForegroundColor(FSlateColor(EditForeground()))
		.SetFocusedForegroundColor(FSlateColor(EditForeground()))
		.SetReadOnlyForegroundColor(FSlateColor(DisabledColor()))
		.SetBackgroundColor(FSlateColor(FLinearColor::White))
		.SetScrollBarStyle(BrowserScrollBarStyle);

	const FSlateNoResource NoResource;
	BrowserTableRowStyle
		.SetSelectorFocusedBrush(SelectionBrush)
		.SetActiveHoveredBrush(SelectionBrush)
		.SetActiveBrush(SelectionBrush)
		.SetInactiveHoveredBrush(SelectionBrush)
		.SetInactiveBrush(SelectionBrush)
		.SetEvenRowBackgroundHoveredBrush(NoResource)
		.SetEvenRowBackgroundBrush(NoResource)
		.SetOddRowBackgroundHoveredBrush(NoResource)
		.SetOddRowBackgroundBrush(NoResource)
		.SetTextColor(FSlateColor(CellForeground()))
		.SetSelectedTextColor(FSlateColor(CellForeground()));
	if (bGameSkin)
	{
		const FSlateRoundedBoxBrush Hover(FLinearColor(0.07f, 0.073f, 0.07f), 2.0f, GameAccentColor(), 1.0f);
		BrowserButtonStyle.SetHovered(Hover);
		BrowserHeadingStyle.SetHovered(Hover);
		BrowserEditableTextBoxStyle.SetBackgroundImageFocused(RecessedBevelBrush);
		BrowserTableRowStyle
			.SetEvenRowBackgroundHoveredBrush(SelectionBrush)
			.SetOddRowBackgroundHoveredBrush(SelectionBrush)
			.SetOddRowBackgroundBrush(FSlateColorBrush(FLinearColor(0.018f, 0.023f, 0.028f)));
	}
	BrowserTableViewStyle.SetBackgroundBrush(ClientBrush);
}

UFont* FWoTRetailBrowserStyle::ResolveFont(
	UWoTHUDFonts* Registry, FName Id)
{
#if WOT_GUARDED_BOOTSTRAP
	(void)Registry;
	UFont* Font = UWoTHUDFonts::ResolveRuntime(Id);
#else
	UFont* Font = Registry ? Registry->Resolve(Id) : nullptr;
#endif
	if (Font)
	{
#if !WOT_GUARDED_BOOTSTRAP
		LoadedObjects.Emplace(Font);
#endif
	}
	return Font;
}

UTexture2D* FWoTRetailBrowserStyle::LoadTexture(const TCHAR* AssetPath)
{
	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, AssetPath);
	// Diagnostic-only: a null load OR a load whose stripped pixels are never
	// injected both render black with no trace in packaged Test builds (the
	// front-end menu, 2026-08-06). The style loads ~20 textures once.
	WoTRecordBootstrapHeartbeat(TEXT("menu-texture-load"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("%s -> %s"), AssetPath,
			Texture ? TEXT("loaded") : TEXT("NULL")));
	if (Texture)
	{
		LoadedObjects.Emplace(Texture);
	}
	return Texture;
}

void FWoTRetailBrowserStyle::ConfigureRegionBrush(
	FSlateBrush& Brush, UTexture2D* Texture, FVector2f PixelPosition,
	FVector2f PixelSize, ESlateBrushDrawType::Type DrawAs, FMargin Margin)
{
	Brush.DrawAs = DrawAs;
	Brush.Tiling = ESlateBrushTileType::NoTile;
	Brush.Margin = Margin;
	Brush.SetResourceObject(Texture);
	Brush.SetImageSize(FVector2D(
		PixelSize.X * GUIScale, PixelSize.Y * GUIScale));
	if (Texture)
	{
		const FVector2f TextureSize(
			static_cast<float>(Texture->GetSurfaceWidth()),
			static_cast<float>(Texture->GetSurfaceHeight()));
		Brush.SetUVRegion(FBox2f(
			PixelPosition / TextureSize,
			(PixelPosition + PixelSize) / TextureSize));
	}
}

FLinearColor FWoTRetailBrowserStyle::ClientColor()
{
	// UBrowserMainClientWindow.uc:117-120 overrides the stock gray client.
	return FLinearColor::Black;
}

FLinearColor FWoTRetailBrowserStyle::BodyTextColor()
{
	return FLinearColor::Black;
}

FLinearColor FWoTRetailBrowserStyle::CellTextColor()
{
	// UWindowWindow.PaintClients:468-472 resets each child to white. Then
	// UBrowserServerGrid.DrawCell:49-66 clips text without changing DrawColor.
	return FLinearColor::White;
}

float FWoTRetailBrowserStyle::TitleBarHeight() const
{
	if (const FWoTBrowserRegion* Region = LookAndFeel.Regions.Find(TEXT("FrameT")))
	{
		return Region->H * GUIScale;
	}
	return 36.0f;
}

float FWoTRetailBrowserStyle::StatusBarHeight() const
{
	if (const FWoTBrowserRegion* Region = LookAndFeel.Regions.Find(TEXT("FrameSB")))
	{
		return Region->H * GUIScale;
	}
	return 28.0f;
}

FLinearColor FWoTRetailBrowserStyle::DisabledColor()
{
	// UWindowWin95LookAndFeel.uc:765-771 authors this disabled gray in code.
	constexpr float Disabled = 96.0f / 255.0f;
	return FLinearColor(Disabled, Disabled, Disabled, 1.0f);
}

FWoTMenuStyle::FWoTMenuStyle()
{
#if WOT_GUARDED_BOOTSTRAP
	UWoTHUDFonts* Registry = nullptr;
#else
	UWoTHUDFonts* Registry = LoadObject<UWoTHUDFonts>(
		nullptr, TEXT("/Game/WoT/UI/Fonts/DA_WoTHUDFonts.DA_WoTHUDFonts"));
	if (Registry)
	{
		LoadedObjects.Emplace(Registry);
	}
#endif

	Small = FWoTMenuFontSelection::Select(
		ResolveFont(Registry, FName(TEXT("F_WOTReg08"))), 8.0f,
		FName(TEXT("Regular")));
	Italic = FWoTMenuFontSelection::Select(
		ResolveFont(Registry, FName(TEXT("F_WOTIta14"))), 14.0f,
		FName(TEXT("Italic")));

	MenuStrings.Reset(LoadObject<UWoTMenuStrings>(
		nullptr, TEXT("/Game/WoT/UI/Menus/DA_WoTMenuStrings.DA_WoTMenuStrings")));

	UTexture2D* Console = LoadTexture(
		TEXT("/Game/WoT/Textures/WOT/WOTConsole.WOTConsole"));
	UTexture2D* Border = LoadTexture(
		TEXT("/Game/WoT/Textures/WOT/WOTBorder.WOTBorder"));
	ConfigureImageBrush(BackgroundBrush, Console, true);
	ConfigureImageBrush(BlackoutBrush, Border, true);
	// uiConsole.DrawBackGroundPattern uses Canvas.DrawPattern for WOTBorder:
	// the texture tiles across the complete region; it is not a nine-slice.
	ConfigureImageBrush(FrameBrush, Border, true);

	ConfigureImageBrush(Slide1Brush, LoadTexture(
		TEXT("/Game/WoT/Textures/WOT/Slide1.Slide1")));
	ConfigureImageBrush(Slide2Brush, LoadTexture(
		TEXT("/Game/WoT/Textures/WOT/Slide2.Slide2")));
	ConfigureImageBrush(Slide3Brush, LoadTexture(
		TEXT("/Game/WoT/Textures/WOT/Slide3.Slide3")));
	ConfigureImageBrush(Slide4Brush, LoadTexture(
		TEXT("/Game/WoT/Textures/WOT/Slide4.Slide4")));

	const FSlateNoResource NoResource;
	MenuButtonStyle
		.SetNormal(NoResource)
		.SetHovered(NoResource)
		.SetPressed(NoResource)
		.SetDisabled(NoResource)
		.SetNormalForeground(MenuColor(false))
		.SetHoveredForeground(MenuColor(true))
		.SetPressedForeground(MenuColor(true))
		.SetDisabledForeground(FSlateColor(DisabledColor()));

	const FSlateColorBrush ControlNormal(FLinearColor(0.035f, 0.035f, 0.035f, 1.0f));
	const FSlateColorBrush ControlHovered(FLinearColor(0.16f, 0.16f, 0.16f, 1.0f));
	const FSlateColorBrush ControlFocused(FLinearColor(0.28f, 0.28f, 0.28f, 1.0f));
	// Unchecked boxes keep the dark fill but draw a 1px outline in the menu
	// text colour: a fill-only near-black box vanished on the dark panels, so
	// the relay privacy checkbox and the LAN/Internet choice read as plain text.
	FSlateRoundedBoxBrush CheckOff(FLinearColor(0.04f, 0.04f, 0.04f, 1.0f),
		0.0f, UnselectedColor(), 1.0f, FVector2D(12.0f, 12.0f));
	FSlateColorBrush CheckOn(UnselectedColor());
	CheckOn.SetImageSize(FVector2D(12.0f, 12.0f));
	SeparatorBrush = FSlateColorBrush(UnselectedColor());
	SeparatorBrush.SetImageSize(FVector2D(1.0f, 1.0f));

	MenuScrollBarStyle
		.SetHorizontalBackgroundImage(ControlNormal)
		.SetVerticalBackgroundImage(ControlNormal)
		.SetVerticalTopSlotImage(NoResource)
		.SetHorizontalTopSlotImage(NoResource)
		.SetVerticalBottomSlotImage(NoResource)
		.SetHorizontalBottomSlotImage(NoResource)
		.SetNormalThumbImage(ControlHovered)
		.SetHoveredThumbImage(ControlFocused)
		.SetDraggedThumbImage(FSlateColorBrush(SelectedColor()))
		.SetThickness(8.0f);
	MenuScrollBoxStyle
		.SetBarThickness(8.0f)
		.SetTopShadowBrush(NoResource)
		.SetBottomShadowBrush(NoResource)
		.SetLeftShadowBrush(NoResource)
		.SetRightShadowBrush(NoResource);

	MenuEditableTextBoxStyle
		.SetBackgroundImageNormal(ControlNormal)
		.SetBackgroundImageHovered(ControlHovered)
		.SetBackgroundImageFocused(ControlFocused)
		.SetBackgroundImageReadOnly(ControlNormal)
		.SetPadding(FMargin(5.0f, 3.0f))
		.SetForegroundColor(MenuColor(false))
		.SetFocusedForegroundColor(MenuColor(true))
		.SetReadOnlyForegroundColor(FSlateColor(DisabledColor()))
		.SetBackgroundColor(FSlateColor(FLinearColor::White))
		.SetScrollBarStyle(MenuScrollBarStyle);

	FButtonStyle ComboButtonBase = MenuButtonStyle;
	ComboButtonBase
		.SetNormal(ControlNormal)
		.SetHovered(ControlHovered)
		.SetPressed(ControlFocused);
	FComboButtonStyle ComboButton;
	ComboButton
		.SetButtonStyle(ComboButtonBase)
		.SetDownArrowImage(NoResource)
		.SetMenuBorderBrush(FrameBrush)
		.SetMenuBorderPadding(FMargin(2.0f))
		.SetContentPadding(FMargin(5.0f, 3.0f));
	MenuComboBoxStyle
		.SetComboButtonStyle(ComboButton)
		.SetContentPadding(FMargin(5.0f, 3.0f))
		.SetMenuRowPadding(FMargin(4.0f, 2.0f));

	MenuCheckBoxStyle
		.SetCheckBoxType(ESlateCheckBoxType::CheckBox)
		.SetUncheckedImage(CheckOff)
		.SetUncheckedHoveredImage(ControlHovered)
		.SetUncheckedPressedImage(ControlFocused)
		.SetCheckedImage(CheckOn)
		.SetCheckedHoveredImage(FSlateColorBrush(SelectedColor()))
		.SetCheckedPressedImage(FSlateColorBrush(SelectedColor()))
		.SetUndeterminedImage(ControlHovered)
		.SetUndeterminedHoveredImage(ControlFocused)
		.SetUndeterminedPressedImage(ControlFocused)
		.SetPadding(FMargin(4.0f))
		.SetForegroundColor(MenuColor(false))
		.SetHoveredForegroundColor(MenuColor(true))
		.SetPressedForegroundColor(MenuColor(true))
		.SetCheckedForegroundColor(MenuColor(false))
		.SetCheckedHoveredForegroundColor(MenuColor(true))
		.SetCheckedPressedForegroundColor(MenuColor(true));

	MenuTableRowStyle
		.SetSelectorFocusedBrush(ControlFocused)
		.SetActiveHoveredBrush(ControlFocused)
		.SetActiveBrush(ControlHovered)
		.SetInactiveHoveredBrush(ControlFocused)
		.SetInactiveBrush(ControlHovered)
		.SetEvenRowBackgroundHoveredBrush(ControlHovered)
		.SetEvenRowBackgroundBrush(NoResource)
		.SetOddRowBackgroundHoveredBrush(ControlHovered)
		.SetOddRowBackgroundBrush(NoResource)
		.SetTextColor(MenuColor(false))
		.SetSelectedTextColor(MenuColor(true));
	MenuTableViewStyle.SetBackgroundBrush(NoResource);
}

UFont* FWoTMenuStyle::ResolveFont(UWoTHUDFonts* Registry, FName Id)
{
#if WOT_GUARDED_BOOTSTRAP
	(void)Registry;
	UFont* Font = UWoTHUDFonts::ResolveRuntime(Id);
#else
	UFont* Font = Registry ? Registry->Resolve(Id) : nullptr;
#endif
	if (Font)
	{
#if !WOT_GUARDED_BOOTSTRAP
		LoadedObjects.Emplace(Font);
#endif
	}
	return Font;
}

UTexture2D* FWoTMenuStyle::LoadTexture(const TCHAR* AssetPath)
{
	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, AssetPath);
	// Diagnostic-only: a null load OR a load whose stripped pixels are never
	// injected both render black with no trace in packaged Test builds (the
	// front-end menu, 2026-08-06). The style loads ~20 textures once.
	WoTRecordBootstrapHeartbeat(TEXT("menu-texture-load"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("%s -> %s"), AssetPath,
			Texture ? TEXT("loaded") : TEXT("NULL")));
	if (Texture)
	{
		LoadedObjects.Emplace(Texture);
	}
	return Texture;
}

void FWoTMenuStyle::ConfigureImageBrush(
	FSlateBrush& Brush, UTexture2D* Texture, bool bTile)
{
	Brush.DrawAs = ESlateBrushDrawType::Image;
	Brush.Tiling = bTile
		? ESlateBrushTileType::Both : ESlateBrushTileType::NoTile;
	Brush.SetResourceObject(Texture);
	if (Texture)
	{
		Brush.SetImageSize(FVector2D(
			static_cast<float>(Texture->GetSurfaceWidth()),
			static_cast<float>(Texture->GetSurfaceHeight())));
	}
	// Remember the triple so RefreshTextureBrushes can re-apply it once the
	// payload injection has replaced the placeholder platform data.
	const bool bAlreadyRecorded = ConfiguredImageBrushes.ContainsByPredicate(
		[&Brush](const FConfiguredImageBrush& Recorded)
		{
			return Recorded.Brush == &Brush;
		});
	if (!bAlreadyRecorded)
	{
		ConfiguredImageBrushes.Add(
			FConfiguredImageBrush{&Brush, Texture, bTile});
	}
}

bool FWoTMenuStyle::RefreshTextureBrushes()
{
	int32 Refreshed = 0;
	for (const FConfiguredImageBrush& Recorded : ConfiguredImageBrushes)
	{
		UTexture2D* Texture = Recorded.Texture.Get();
		if (Recorded.Brush == nullptr || Texture == nullptr)
		{
			continue;
		}
		// SetResourceObject with the same object still forces Slate to drop
		// its cached rendering resource for this brush, which is the half
		// that matters after a platform-data swap; the image size is
		// re-read from the now-real surface.
		Recorded.Brush->SetResourceObject(nullptr);
		Recorded.Brush->SetResourceObject(Texture);
		Recorded.Brush->SetImageSize(FVector2D(
			static_cast<float>(Texture->GetSurfaceWidth()),
			static_cast<float>(Texture->GetSurfaceHeight())));
		++Refreshed;
	}
	WoTRecordBootstrapHeartbeat(TEXT("menu-brushes-refreshed"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("%d brushes"), Refreshed));
	return Refreshed > 0 && Refreshed == ConfiguredImageBrushes.Num();
}

TArray<UTexture2D*> FWoTMenuStyle::RequiredTexturePayloads() const
{
	TArray<UTexture2D*> Result;
	Result.Reserve(ConfiguredImageBrushes.Num());
	for (const FConfiguredImageBrush& Recorded : ConfiguredImageBrushes)
	{
		Result.Add(Recorded.Texture.Get());
	}
	return Result;
}

void FWoTMenuStyle::UnbindTextureConsumers()
{
	ConfiguredImageBrushes.Empty();
	BackgroundBrush.SetResourceObject(nullptr);
	BlackoutBrush.SetResourceObject(nullptr);
	FrameBrush.SetResourceObject(nullptr);
	Slide1Brush.SetResourceObject(nullptr);
	Slide2Brush.SetResourceObject(nullptr);
	Slide3Brush.SetResourceObject(nullptr);
	Slide4Brush.SetResourceObject(nullptr);
	LoadedObjects.RemoveAllSwap([](const TStrongObjectPtr<UObject>& Object)
	{
		return Object.IsValid() && Object->IsA<UTexture2D>();
	});
}

bool FWoTMenuStyle::BindTextureConsumers(
	TConstArrayView<UTexture2D*> Textures)
{
	static const TCHAR* ExpectedPaths[] = {
		TEXT("/Game/WoT/Textures/WOT/WOTConsole.WOTConsole"),
		TEXT("/Game/WoT/Textures/WOT/WOTBorder.WOTBorder"),
		TEXT("/Game/WoT/Textures/WOT/Slide1.Slide1"),
		TEXT("/Game/WoT/Textures/WOT/Slide2.Slide2"),
		TEXT("/Game/WoT/Textures/WOT/Slide3.Slide3"),
		TEXT("/Game/WoT/Textures/WOT/Slide4.Slide4"),
	};
	if (Textures.Num() != UE_ARRAY_COUNT(ExpectedPaths))
	{
		return false;
	}
	for (int32 Index = 0; Index < Textures.Num(); ++Index)
	{
		if (!IsValid(Textures[Index])
			|| Textures[Index]->GetPathName() != ExpectedPaths[Index])
		{
			return false;
		}
	}

	UnbindTextureConsumers();
	for (UTexture2D* Texture : Textures)
	{
		LoadedObjects.Emplace(Texture);
	}
	ConfigureImageBrush(BackgroundBrush, Textures[0], true);
	ConfigureImageBrush(BlackoutBrush, Textures[1], true);
	ConfigureImageBrush(FrameBrush, Textures[1], true);
	ConfigureImageBrush(Slide1Brush, Textures[2]);
	ConfigureImageBrush(Slide2Brush, Textures[3]);
	ConfigureImageBrush(Slide3Brush, Textures[4]);
	ConfigureImageBrush(Slide4Brush, Textures[5]);
	return ConfiguredImageBrushes.Num() == 7;
}

#if WITH_AUTOMATION_TESTS
void FWoTMenuStyle::ClearTextureConsumersForTesting()
{
	UnbindTextureConsumers();
}
#endif

FText FWoTMenuStyle::RetailText(FName Id) const
{
	bool bUseEditorStrings = MenuStrings.IsValid();
#if WITH_AUTOMATION_TESTS
	bUseEditorStrings = bUseEditorStrings && !bEditorMenuStringsHiddenForTesting;
#endif
	if (bUseEditorStrings)
	{
		const FText Resolved = MenuStrings->Resolve(Id);
		if (!Resolved.IsEmpty() && Resolved.ToString() != Id.ToString())
		{
			return Resolved;
		}
	}
	struct FFallback
	{
		const TCHAR* Key;
		const TCHAR* Value;
	};
	static const FFallback SettingsFallbacks[] = {
		{TEXT("menuOptions.MenuTitle"), TEXT("CONTROLS")},
		{TEXT("menuOptions.MenuList[1]"), TEXT("Customize Controls")},
		{TEXT("menuOptions.MenuList[2]"), TEXT("Invert Mouse")},
		{TEXT("menuOptions.MenuList[3]"), TEXT("Mouse Sensitivity")},
		{TEXT("menuKeyboard.MenuList[1]"), TEXT("Fire")},
		{TEXT("menuKeyboard.MenuList[2]"), TEXT("Jump/Up")},
		{TEXT("menuKeyboard.MenuList[3]"), TEXT("Move Forward")},
		{TEXT("menuKeyboard.MenuList[4]"), TEXT("Move Backward")},
		{TEXT("menuKeyboard.MenuList[5]"), TEXT("Strafe Left")},
		{TEXT("menuKeyboard.MenuList[6]"), TEXT("Strafe Right")},
		{TEXT("menuKeyboard.MenuList[7]"), TEXT("Turn Left")},
		{TEXT("menuKeyboard.MenuList[8]"), TEXT("Turn Right")},
		{TEXT("menuKeyboard.MenuList[9]"), TEXT("Next Hand")},
		{TEXT("menuKeyboard.MenuList[10]"), TEXT("Previous Hand")},
		{TEXT("menuKeyboard.MenuList[13]"), TEXT("Look Up")},
		{TEXT("menuKeyboard.MenuList[14]"), TEXT("Look Down")},
		{TEXT("menuKeyboard.MenuList[18]"), TEXT("RESET TO DEFAULTS")},
		{TEXT("menuConfiguration.MenuTitle"), TEXT("HARDWARE")},
		{TEXT("menuConfiguration.MenuList[2]"), TEXT("Toggle Fullscreen Mode")},
		{TEXT("menuConfiguration.MenuList[3]"), TEXT("Select Resolution")},
		{TEXT("menuConfiguration.MenuList[5]"), TEXT("VSync")},
		{TEXT("menuConfiguration.MenuList[6]"), TEXT("Master Detail Control")},
		{TEXT("menuConfiguration.MenuList[15]"), TEXT("Master Volume")},
	};
	for (const FFallback& Fallback : SettingsFallbacks)
	{
		if (Id == FName(Fallback.Key))
		{
			return FText::FromString(Fallback.Value);
		}
	}
	return FText::FromName(Id);
}

bool FWoTMenuStyle::BindAuthenticatedMainMenuLocalization(
	const FWoTRequiredMenuFonts& Fonts)
{
	TMap<FName, FString> Discarded;
	return LoadAuthenticatedMainMenuLocalization(Fonts, Discarded);
}

bool FWoTMenuStyle::LoadAuthenticatedMainMenuLocalization(
	const FWoTRequiredMenuFonts& Fonts,
	TMap<FName, FString>& OutStrings) const
{
	OutStrings.Empty();
	if (!Fonts.IsValid())
	{
		return false;
	}

	FWoTRuntimeLocalizationDefaults Defaults;
	FWoTBootstrapStatus Status = Defaults.LoadLocalizationFirstUse(
		FWoTNeutralResourceId{
			TEXT("cache/localization/WoT.int"),
			EWoTNeutralResourceKind::Localization});
	if (Status.IsError()
		|| !ParseAuthenticatedMainMenuLocalization(
			Defaults.LocalizationBytes(), OutStrings))
	{
		OutStrings.Empty();
		return false;
	}
	return Fonts.IsValid();
}

bool FWoTMenuStyle::ParseAuthenticatedMainMenuLocalization(
	TConstArrayView<uint8> Bytes, TMap<FName, FString>& OutStrings)
{
	OutStrings.Empty();
	if (Bytes.IsEmpty())
	{
		return false;
	}
	FString Text;
	Text.Reserve(Bytes.Num());
	for (const uint8 Byte : Bytes)
	{
		if (Byte == 0)
		{
			return false;
		}
		Text.AppendChar(static_cast<TCHAR>(Byte));
	}

	struct FRequiredEntry
	{
		const TCHAR* Section;
		const TCHAR* Key;
		const TCHAR* Id;
	};
	static const FRequiredEntry Required[] = {
		{TEXT("menuMain"), TEXT("MenuList[2]"), TEXT("menuMain.MenuList[2]")},
		{TEXT("menuMain"), TEXT("MenuList[3]"), TEXT("menuMain.MenuList[3]")},
		{TEXT("menuMain"), TEXT("MenuList[4]"), TEXT("menuMain.MenuList[4]")},
		{TEXT("menuMain"), TEXT("MenuList[7]"), TEXT("menuMain.MenuList[7]")},
		{TEXT("menuStartArenaServer"), TEXT("MenuTitle"), TEXT("menuStartArenaServer.MenuTitle")},
		{TEXT("menuStartBattleServer"), TEXT("MenuTitle"), TEXT("menuStartBattleServer.MenuTitle")},
		{TEXT("menuStartBattleServer"), TEXT("MenuList[6]"), TEXT("menuStartBattleServer.MenuList[6]")},
		{TEXT("menuStartBattleServer"), TEXT("MenuList[7]"), TEXT("menuStartBattleServer.MenuList[7]")},
		{TEXT("menuStartArenaServer"), TEXT("MenuList[1]"), TEXT("menuStartArenaServer.MenuList[1]")},
		{TEXT("menuStartArenaServer"), TEXT("MenuList[2]"), TEXT("menuStartArenaServer.MenuList[2]")},
		{TEXT("menuStartArenaServer"), TEXT("MenuList[3]"), TEXT("menuStartArenaServer.MenuList[3]")},
		{TEXT("menuStartArenaServer"), TEXT("MenuList[5]"), TEXT("menuStartArenaServer.MenuList[5]")},
		{TEXT("menuStartArenaServer"), TEXT("MenuList[7]"), TEXT("menuStartArenaServer.MenuList[7]")},
		{TEXT("menuMultiPlayer"), TEXT("HelpMessage[1]"), TEXT("menuMultiPlayer.HelpMessage[1]")},
		// Player Config under Multiplayer, plus the real Controls/Hardware pages.
		{TEXT("menuMultiPlayer"), TEXT("MenuList[2]"), TEXT("menuMultiPlayer.MenuList[2]")},
		{TEXT("menuPlayer"), TEXT("MenuTitle"), TEXT("menuPlayer.MenuTitle")},
		{TEXT("menuPlayer"), TEXT("MenuList[1]"), TEXT("menuPlayer.MenuList[1]")},
		{TEXT("menuPlayer"), TEXT("HelpMessage[1]"), TEXT("menuPlayer.HelpMessage[1]")},
		{TEXT("menuPlayer"), TEXT("HelpMessage[2]"), TEXT("menuPlayer.HelpMessage[2]")},
		{TEXT("menuOptions"), TEXT("MenuTitle"), TEXT("menuOptions.MenuTitle")},
		{TEXT("menuOptions"), TEXT("MenuList[1]"), TEXT("menuOptions.MenuList[1]")},
		{TEXT("menuOptions"), TEXT("MenuList[2]"), TEXT("menuOptions.MenuList[2]")},
		{TEXT("menuOptions"), TEXT("MenuList[3]"), TEXT("menuOptions.MenuList[3]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[1]"), TEXT("menuKeyboard.MenuList[1]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[2]"), TEXT("menuKeyboard.MenuList[2]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[3]"), TEXT("menuKeyboard.MenuList[3]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[4]"), TEXT("menuKeyboard.MenuList[4]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[5]"), TEXT("menuKeyboard.MenuList[5]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[6]"), TEXT("menuKeyboard.MenuList[6]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[7]"), TEXT("menuKeyboard.MenuList[7]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[8]"), TEXT("menuKeyboard.MenuList[8]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[9]"), TEXT("menuKeyboard.MenuList[9]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[10]"), TEXT("menuKeyboard.MenuList[10]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[13]"), TEXT("menuKeyboard.MenuList[13]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[14]"), TEXT("menuKeyboard.MenuList[14]")},
		{TEXT("menuKeyboard"), TEXT("MenuList[18]"), TEXT("menuKeyboard.MenuList[18]")},
		{TEXT("menuConfiguration"), TEXT("MenuTitle"), TEXT("menuConfiguration.MenuTitle")},
		{TEXT("menuConfiguration"), TEXT("MenuList[2]"), TEXT("menuConfiguration.MenuList[2]")},
		{TEXT("menuConfiguration"), TEXT("MenuList[3]"), TEXT("menuConfiguration.MenuList[3]")},
		{TEXT("menuConfiguration"), TEXT("MenuList[5]"), TEXT("menuConfiguration.MenuList[5]")},
		{TEXT("menuConfiguration"), TEXT("MenuList[6]"), TEXT("menuConfiguration.MenuList[6]")},
		{TEXT("menuConfiguration"), TEXT("MenuList[15]"), TEXT("menuConfiguration.MenuList[15]")}
	};

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	FString Section;
	for (FString Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
		{
			Section = Line.Mid(1, Line.Len() - 2);
			continue;
		}
		if (Section.IsEmpty() || Line.IsEmpty()
			|| Line.StartsWith(TEXT(";")))
		{
			continue;
		}
		FString Key;
		FString Value;
		if (!Line.Split(TEXT("="), &Key, &Value))
		{
			continue;
		}
		Key.TrimStartAndEndInline();
		Value.TrimStartAndEndInline();
		// The engine's localization reader drops one pair of enclosing quotes
		// (retail quotes values such as menuKeyboard.MenuList[12]).
		if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
		{
			Value = Value.Mid(1, Value.Len() - 2);
		}
		bool bPrintable = !Value.IsEmpty();
		for (const TCHAR Character : Value)
		{
			bPrintable = bPrintable && Character >= 0x20 && Character <= 0x7f;
		}
		// Only the four opening-scene labels gate admission; every other label
		// (the rest of the table included) is optional below, so one damaged
		// label cannot take the whole menu down.
		bool bRequired = false;
		for (const FRequiredEntry& Entry : MakeArrayView(Required, 4))
		{
			if (!Section.Equals(Entry.Section, ESearchCase::IgnoreCase)
				|| !Key.Equals(Entry.Key, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FName Id(Entry.Id);
			if (!bPrintable || OutStrings.Contains(Id))
			{
				OutStrings.Empty();
				return false;
			}
			OutStrings.Add(Id, MoveTemp(Value));
			bRequired = true;
			break;
		}
		// Every other label of the retail menu sections, so each screen's
		// MenuList/HelpMessage rows resolve from the player's own copy (the
		// neutral package ships no menu strings; a fixed list here left the
		// host panel's framerate, password and cycle rows as raw keys). An
		// unprintable or empty optional label is skipped, never fatal; the
		// first occurrence wins, as in the engine's reader.
		if (!bRequired && bPrintable
			&& Section.StartsWith(TEXT("menu"), ESearchCase::IgnoreCase))
		{
			const FName Id(*(Section + TEXT(".") + Key));
			if (!OutStrings.Contains(Id))
			{
				OutStrings.Add(Id, MoveTemp(Value));
			}
		}
	}
	// Keep opening-scene admission bounded to its original four labels.
	// Multiplayer checks its additional labels at first use.
	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (!OutStrings.Contains(FName(Required[Index].Id)))
		{
			OutStrings.Empty();
			return false;
		}
	}
	return true;
}

FLinearColor FWoTMenuStyle::SelectedColor()
{
	// Engine Menu.SetFontBrightness(bBright): white.
	return FLinearColor::White;
}

FLinearColor FWoTMenuStyle::UnselectedColor()
{
	// Menu.SetFontBrightness restores Canvas.Default.DrawColor; UE1's Canvas
	// authors that default as (127,127,127).
	constexpr float RetailDefault = 127.0f / 255.0f;
	return FLinearColor(RetailDefault, RetailDefault, RetailDefault, 1.0f);
}

FLinearColor FWoTMenuStyle::DisabledColor()
{
	// menuWOT.uc:43,418.
	constexpr float RetailDisabled = 25.0f / 255.0f;
	return FLinearColor(
		RetailDisabled, RetailDisabled, RetailDisabled, 1.0f);
}

FSlateColor FWoTMenuStyle::MenuColor(bool bSelected)
{
	return FSlateColor(bSelected ? SelectedColor() : UnselectedColor());
}

float FWoTMenuStyle::CheckGlyphSizeFor(const FSlateFontInfo& Font)
{
	// MultiplayerFont sizes are bitmap pixels * 0.75 / viewport scale, so the
	// square tracks the label at every scale. A fixed 12 shrank to 8px beside
	// 20px capitals at 720p.
	return FMath::Max(12.0f, static_cast<float>(Font.Size));
}

void FWoTMenuStyle::SizeCheckBoxImages(FCheckBoxStyle& Style, float GlyphSize)
{
	const FVector2D Size(GlyphSize);
	for (FSlateBrush* Brush : {&Style.UncheckedImage, &Style.UncheckedHoveredImage,
		&Style.UncheckedPressedImage, &Style.CheckedImage, &Style.CheckedHoveredImage,
		&Style.CheckedPressedImage, &Style.UndeterminedImage,
		&Style.UndeterminedHoveredImage, &Style.UndeterminedPressedImage})
	{
		Brush->SetImageSize(Size);
	}
}

bool WoTIsMenuBackKey(const FKey& Key)
{
	return Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right
		|| Key == EKeys::Gamepad_Special_Left;
}

void SWoTMenuButton::Construct(const FArguments& InArgs)
{
	ExternalSelection = InArgs._IsSelected;
	OnSelected = InArgs._OnSelected;
	bUsesExternalSelection = ExternalSelection.IsBound();
	SButton::Construct(
		SButton::FArguments()
		.ButtonStyle(FWoTMenuStyle::Get().ButtonStyle())
		.ContentPadding(InArgs._ContentPadding)
		.HAlign(InArgs._HAlign)
		.IsEnabled(InArgs._IsEnabled)
		.OnClicked(InArgs._OnClicked)
		[
			InArgs._Content.Widget
		]);
}

FReply SWoTMenuButton::OnFocusReceived(
	const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	if (bUsesExternalSelection)
	{
		OnSelected.ExecuteIfBound();
	}
	return SButton::OnFocusReceived(MyGeometry, InFocusEvent);
}

void SWoTMenuButton::OnMouseEnter(
	const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	SButton::OnMouseEnter(MyGeometry, MouseEvent);
	// In Controller mode the pointer never selects: the D-pad owns the one
	// highlight, and a hidden cursor must not steal it (spec r3 section 1).
	if (bUsesExternalSelection && !FWoTControllerUIInput::IsControllerMode())
	{
		OnSelected.ExecuteIfBound();
		// Hover is a real retail selection change. Keep Slate's navigation origin
		// on that same entry so the next keyboard/controller move cannot resume
		// from a stale, differently focused button.
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetKeyboardFocus(
				AsShared(), EFocusCause::Mouse);
		}
	}
}

void SWoTMenuButton::Tick(
	const FGeometry& AllottedGeometry, const double InCurrentTime,
	const float InDeltaTime)
{
	SButton::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// Hover draws the highlight only for the pointer; keyboard focus always
	// does (the existing keyboard cue, spec r3 [N3]).
	const bool bHoverLook = IsHovered() && !FWoTControllerUIInput::IsControllerMode();
	const bool bSelected = bUsesExternalSelection
		? ExternalSelection.Get(false)
		: bHoverLook || HasKeyboardFocus();
	SetForegroundColor(IsEnabled()
		? FWoTMenuStyle::MenuColor(bSelected)
		: FSlateColor(FWoTMenuStyle::DisabledColor()));
}

void SWoTMenuText::Construct(const FArguments& InArgs)
{
	if (InArgs._PresentationAuthority.IsValid())
	{
		const TSharedPtr<FWoTFrontEndPresentationAuthority> Authority =
			InArgs._PresentationAuthority;
		const FWoTRequiredMenuFonts* Fonts = Authority->RequiredFonts();
		if (Fonts == nullptr)
		{
			SetVisibility(EVisibility::Collapsed);
			ChildSlot[SNullWidget::NullWidget];
			return;
		}
		Authority->BindWidget(AsShared());
		SetVisibility(TAttribute<EVisibility>::CreateLambda([Authority]()
		{
			return Authority->IsReady()
				? EVisibility::Visible : EVisibility::Collapsed;
		}));
		ChildSlot
		[
			SNew(STextBlock)
			.Text(InArgs._Text)
			.Font(InArgs._Large ? Fonts->Title : Fonts->List)
			.ColorAndOpacity(FSlateColor::UseForeground())
			.Justification(InArgs._Justification)
		];
		return;
	}
	const FWoTMenuStyle& Style = FWoTMenuStyle::Get();
	const TOptional<FWoTRequiredMenuFonts> Fonts = Style.ResolveRequiredFonts();
	if (!Fonts.IsSet())
	{
		SetVisibility(EVisibility::Collapsed);
		ChildSlot[SNullWidget::NullWidget];
		return;
	}
	const FWoTRequiredMenuFonts CapturedFonts = Fonts.GetValue();
	CapturedFonts.BindWidget(AsShared());
	const FSlateFontInfo& Face = InArgs._Large
		? CapturedFonts.Title : CapturedFonts.List;
	SetVisibility(TAttribute<EVisibility>::CreateLambda([CapturedFonts]()
	{
		return CapturedFonts.IsValid()
			? EVisibility::Visible : EVisibility::Collapsed;
	}));

	ChildSlot
	[
		SNew(STextBlock)
		.Text(InArgs._Text)
		.Font(Face)
		.ColorAndOpacity(FSlateColor::UseForeground())
		.Justification(InArgs._Justification)
	];
}

namespace
{
// Retail sprites are 8x8. At the 720p viewport scale (2/3), this keeps
// each source pixel 2x2 on screen instead of shrinking the control to ~5px.
constexpr float WoTRetailSliderArtScale = 3.0f;
}

void SWoTRetailSlider::Construct(const FArguments& InArgs)
{
	const FWoTMenuStyle& Style = FWoTMenuStyle::Get();
	Value = InArgs._Value;
	OnValueChanged = InArgs._OnValueChanged;
	MinValue = InArgs._MinValue;
	StepSize = FMath::Max(InArgs._StepSize, UE_SMALL_NUMBER);
	Positions = FMath::Max(InArgs._Positions, 2);
	SetCanTick(false);

	TSharedRef<SHorizontalBox> Strip = SNew(SHorizontalBox);
	Strip->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(SImage)
		.Image(Style.SliderLeftCap())
		.DesiredSizeOverride(Style.SliderLeftCap()->GetImageSize() * WoTRetailSliderArtScale)
		.ColorAndOpacity(this, &SWoTRetailSlider::SelectionColor)
	];
	for (int32 Cell = 0; Cell < 8; ++Cell)
	{
		Strip->AddSlot().FillWidth(1.0f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot().VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(Style.SliderEmptyCell())
				.DesiredSizeOverride(Style.SliderEmptyCell()->GetImageSize() * WoTRetailSliderArtScale)
				.ColorAndOpacity(this, &SWoTRetailSlider::SelectionColor)
			]
		];
	}
	Strip->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(SImage)
		.Image(Style.SliderRightCap())
		.DesiredSizeOverride(Style.SliderRightCap()->GetImageSize() * WoTRetailSliderArtScale)
		.ColorAndOpacity(this, &SWoTRetailSlider::SelectionColor)
	];
	ChildSlot
	[
		Strip
	];
}

FSlateColor SWoTRetailSlider::SelectionColor() const
{
	return FWoTMenuStyle::MenuColor(IsHovered() || HasKeyboardFocus());
}

int32 SWoTRetailSlider::ValueCell() const
{
	const float CurrentValue = Value.Get(MinValue);
	if (Positions == 8)
	{
		// menuWOT.DrawSlider: the knob takes the first cell whose upper bound
		// (StepSize * i + sMin) exceeds the value, else the last.
		for (int32 CellIndex = 1; CellIndex <= 8; ++CellIndex)
		{
			if (StepSize * CellIndex + MinValue > CurrentValue || CellIndex == 8)
			{
				return CellIndex;
			}
		}
		return 8;
	}
	const float Fraction = (CurrentValue - MinValue) / (StepSize * 8.0f);
	return FMath::Clamp(
		FMath::RoundToInt(Fraction * static_cast<float>(Positions - 1)) + 1, 1, Positions);
}

namespace
{
// Slide1/Slide3 are 8px sprites: 4 transparent columns, the 1px end border,
// then 3 columns of track interior (Slide3 mirrored). The visible track ends
// at that border, so the knob's travel runs 3 source columns into each cap.
constexpr float WoTRetailSliderCapTrackColumns = 3.0f;

struct FWoTRetailSliderMetrics
{
	float LeftCap = 0.0f;
	float RightCap = 0.0f;
	// The visible track's ends: the inner side of each cap's end border.
	float TrackStart = 0.0f;
	float TrackEndInset = 0.0f;
	FVector2D Knob = FVector2D::ZeroVector;
};

FWoTRetailSliderMetrics WoTRetailSliderMetrics()
{
	const FWoTMenuStyle& Style = FWoTMenuStyle::Get();
	FWoTRetailSliderMetrics Metrics;
	Metrics.LeftCap = Style.SliderLeftCap()->GetImageSize().X * WoTRetailSliderArtScale;
	Metrics.RightCap = Style.SliderRightCap()->GetImageSize().X * WoTRetailSliderArtScale;
	const float CapTrack = WoTRetailSliderCapTrackColumns * WoTRetailSliderArtScale;
	Metrics.TrackStart = FMath::Max(Metrics.LeftCap - CapTrack, 0.0f);
	Metrics.TrackEndInset = FMath::Max(Metrics.RightCap - CapTrack, 0.0f);
	Metrics.Knob = Style.SliderKnob()->GetImageSize() * WoTRetailSliderArtScale;
	return Metrics;
}
}

float SWoTRetailSlider::KnobLeft(float AllottedWidth) const
{
	// Retail drew the knob as one of eight unstretched 8px cells after the
	// whole Slide1 sprite, so 3px of track showed between the end border and
	// the knob at either extreme, which reads as unreachable values. The port
	// stretches the track to the row and lets the knob travel to the end
	// borders themselves (PORT_NOTES "Menu slider travel and host-setting stops").
	const FWoTRetailSliderMetrics Metrics = WoTRetailSliderMetrics();
	const float Travel = FMath::Max(AllottedWidth - Metrics.TrackStart
		- Metrics.TrackEndInset - Metrics.Knob.X, 0.0f);
	const float Fraction = static_cast<float>(ValueCell() - 1)
		/ static_cast<float>(Positions - 1);
	return Metrics.TrackStart + Travel * Fraction;
}

int32 SWoTRetailSlider::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 TrackLayer = SCompoundWidget::OnPaint(Args, AllottedGeometry,
		MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	const FWoTRetailSliderMetrics Metrics = WoTRetailSliderMetrics();
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	const FVector2D KnobPosition(
		KnobLeft(Size.X), (Size.Y - Metrics.Knob.Y) * 0.5f);
	const bool bEnabled = ShouldBeEnabled(bParentEnabled);
	FSlateDrawElement::MakeBox(OutDrawElements, TrackLayer + 1,
		AllottedGeometry.ToPaintGeometry(Metrics.Knob, FSlateLayoutTransform(KnobPosition)),
		FWoTMenuStyle::Get().SliderKnob(),
		bEnabled ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect,
		InWidgetStyle.GetColorAndOpacityTint()
			* SelectionColor().GetColor(InWidgetStyle));
	return TrackLayer + 1;
}

int32 SWoTRetailSlider::CellFromMouse(
	const FGeometry& MyGeometry, const FVector2D& ScreenPosition) const
{
	// The stop whose knob centre is nearest the pointer.
	const FWoTRetailSliderMetrics Metrics = WoTRetailSliderMetrics();
	const float Travel = FMath::Max(MyGeometry.GetLocalSize().X
		- Metrics.TrackStart - Metrics.TrackEndInset - Metrics.Knob.X, 1.0f);
	const float LocalX = MyGeometry.AbsoluteToLocal(ScreenPosition).X;
	const float Fraction = FMath::Clamp(
		(LocalX - Metrics.TrackStart - Metrics.Knob.X * 0.5f) / Travel, 0.0f, 1.0f);
	return FMath::RoundToInt(Fraction * static_cast<float>(Positions - 1)) + 1;
}

void SWoTRetailSlider::CommitCell(int32 CellIndex)
{
	CellIndex = FMath::Clamp(CellIndex, 1, Positions);
	// The stops span the complete authored eight-step range.
	const float NewValue = MinValue + StepSize * 8.0f
		* static_cast<float>(CellIndex - 1) / static_cast<float>(Positions - 1);
	OnValueChanged.ExecuteIfBound(NewValue);
	Invalidate(EInvalidateWidgetReason::Paint);
}

FReply SWoTRetailSlider::OnKeyDown(
	const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Left || Key == EKeys::Down)
	{
		CommitCell(ValueCell() - 1);
		return FReply::Handled();
	}
	if (Key == EKeys::Right || Key == EKeys::Up)
	{
		CommitCell(ValueCell() + 1);
		return FReply::Handled();
	}
	if (Key == EKeys::Home)
	{
		CommitCell(1);
		return FReply::Handled();
	}
	if (Key == EKeys::End)
	{
		CommitCell(Positions);
		return FReply::Handled();
	}
	// D-pad Left/Right step by the keyboard's step, one stop. On a two-column
	// screen they would otherwise switch columns (spec r3 section 2).
	if (Key == EKeys::Gamepad_DPad_Left)
	{
		CommitCell(ValueCell() - 1);
		return FReply::Handled();
	}
	if (Key == EKeys::Gamepad_DPad_Right)
	{
		CommitCell(ValueCell() + 1);
		return FReply::Handled();
	}
	// D-pad Up/Down navigate as usual.
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

FNavigationReply SWoTRetailSlider::OnNavigation(
	const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent)
{
	if (InNavigationEvent.GetNavigationGenesis() == ENavigationGenesis::Controller)
	{
		const EUINavigation Direction = InNavigationEvent.GetNavigationType();
		if (Direction == EUINavigation::Left || Direction == EUINavigation::Right)
		{
			CommitCell(ValueCell() + (Direction == EUINavigation::Right ? 1 : -1));
			return FNavigationReply::Stop();
		}
	}
	return SCompoundWidget::OnNavigation(MyGeometry, InNavigationEvent);
}

FReply SWoTRetailSlider::OnMouseButtonDown(
	const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	bDragging = true;
	CommitCell(CellFromMouse(MyGeometry, MouseEvent.GetScreenSpacePosition()));
	return FReply::Handled()
		.CaptureMouse(SharedThis(this))
		.SetUserFocus(SharedThis(this), EFocusCause::Mouse);
}

FReply SWoTRetailSlider::OnMouseButtonUp(
	const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && bDragging)
	{
		bDragging = false;
		CommitCell(CellFromMouse(MyGeometry, MouseEvent.GetScreenSpacePosition()));
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

FReply SWoTRetailSlider::OnMouseMove(
	const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (bDragging && HasMouseCapture())
	{
		CommitCell(CellFromMouse(MyGeometry, MouseEvent.GetScreenSpacePosition()));
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

void SWoTSliderValueLabel::Construct(const FArguments& InArgs)
{
	Font = InArgs._Font;
	ChildSlot
	[
		SNew(SBox)
		.WidthOverride_Lambda([this]()
		{
			const float Width = CachedWidth(Font.Get(FSlateFontInfo()));
			return Width > 0.0f ? FOptionalSize(Width) : FOptionalSize();
		})
		[
			SNew(STextBlock)
			.Text(InArgs._Text)
			.Font(InArgs._Font)
		]
	];
}

const TArray<FString>& SWoTSliderValueLabel::AllValueLabels()
{
	// Every value is measured: digit advances differ in proportional faces, so
	// "88" can outmeasure "100" and "0.75x" can outmeasure "0.25x".
	static const TArray<FString> Labels = []()
	{
		TArray<FString> Result;
		for (int32 Value = 0; Value <= 100; ++Value)
		{
			Result.Add(FString::FromInt(Value));
			Result.Add(FString::Printf(TEXT("%d%%"), Value));
		}
		// The mouse and controller sensitivity steps (WoTSettingsSlate.cpp).
		for (const float Step : {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 7.0f, 10.0f})
		{
			Result.Add(FString::Printf(TEXT("%gx"), Step));
		}
		return Result;
	}();
	return Labels;
}

float SWoTSliderValueLabel::CachedWidth(const FSlateFontInfo& LabelFont) const
{
	if (MeasuredWidth <= 0.0f || !(MeasuredFont == LabelFont))
	{
		MeasuredFont = LabelFont;
		MeasuredWidth = SharedWidth(LabelFont);
	}
	return MeasuredWidth;
}

float SWoTSliderValueLabel::SharedWidth(const FSlateFontInfo& LabelFont)
{
	float Width = 0.0f;
	for (const FString& Sample : AllValueLabels())
	{
		Width = FMath::Max(Width, static_cast<float>(
			FWoTMenuLayout::MeasureText(FText::FromString(Sample), LabelFont).X));
	}
	return Width > 0.0f ? FMath::CeilToFloat(Width) : 0.0f;
}
