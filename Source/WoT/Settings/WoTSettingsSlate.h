#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"
#include "UI/SWoTButtonPrompts.h"
#include "WoTGameSettings.h"
#include "Widgets/SCompoundWidget.h"

class FWoTBackStackEntryHandle;
class FWoTFrontEndPresentationAuthority;
class SCheckBox;
class SScrollBox;
class STextBlock;
class UWoTPreferencesSubsystem;
class UWoTInstallerSharingSubsystem;

enum class EWoTSettingsPage : uint8
{
	Controls,
	Hardware
};

/** Resolves an authenticated retail localization id to display text. */
DECLARE_DELEGATE_RetVal_OneParam(FText, FWoTSettingsTextResolver, FName);

/** The one editable-binding slot a capture targets. */
enum class EWoTBindingSlot : uint8
{
	Primary,
	Secondary
};

// One Controls or Hardware screen. The widget owns the draft, the key capture
// and the 15-second video confirmation; UWoTPreferencesSubsystem stays the only
// persistence owner. Nothing here reads a command, CVar or path from a draft.
class SWoTSettingsScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTSettingsScreen)
		: _Page(EWoTSettingsPage::Controls)
		, _Preferences(nullptr)
	{}
		SLATE_ARGUMENT(EWoTSettingsPage, Page)
		SLATE_ARGUMENT(UWoTPreferencesSubsystem*, Preferences)
		SLATE_ARGUMENT(TSharedPtr<FWoTFrontEndPresentationAuthority>,
			PresentationAuthority)
		SLATE_ATTRIBUTE(FSlateFontInfo, HeadingFont)
		SLATE_ATTRIBUTE(FSlateFontInfo, BodyFont)
		SLATE_EVENT(FWoTSettingsTextResolver, TextResolver)
		/** Back or a completed Save: the caller navigates. */
		SLATE_EVENT(FSimpleDelegate, OnBack)
		SLATE_EVENT(FSimpleDelegate, OnSaved)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SWoTSettingsScreen() override;

	/** The back action (B, View or Escape): cancels a live key capture first;
	 * closes an open "Save changes?" prompt, back to editing; with the video
	 * Keep/Revert strip up, reverts the preview and leaves, as before; with
	 * unsaved changes, opens the "Save changes?" prompt (the owner's Steam
	 * Deck session, 2026-11-09); else leaves the screen. Always handled. The
	 * Back button keeps leaving at once and dropping the draft. */
	bool HandleBack();
	bool IsCapturingBinding() const { return CaptureAction.IsSet(); }
	/** The draft (the Steam keyboard choice included) differs from what is saved. */
	bool HasUnsavedChanges() const;
	/** The "Save changes?" prompt is open. */
	bool IsConfirmingUnsaved() const { return bConfirmingUnsaved; }
	/** The first control of the page: initial focus in Controller mode. */
	TSharedPtr<SWidget> InitialControllerFocusWidget() const;
	/** The prompt strip's set for this page now (spec r3 section 1a): the
	 * settings set, the video Keep/Revert strip's, or a live key capture's. */
	TArray<FWoTButtonPrompt> ControllerPrompts() const;

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnPreviewKeyDown(
		const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyDown(
		const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnPreviewMouseButtonDown(
		const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseWheel(
		const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FNavigationReply OnNavigation(
		const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent) override;
	virtual void Tick(
		const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;

	static constexpr double VideoConfirmationSeconds = 15.0;

#if WITH_AUTOMATION_TESTS
	/** The Mouse Sensitivity slider's eight steps (its labels are "%gx"). */
	static TArray<float> SensitivityStepsForTesting();
	static TArray<float> ControllerSensitivityStepsForTesting();
	const FWoTGameSettings& DraftForTesting() const { return Draft; }
	void SetDraftForTesting(const FWoTGameSettings& NewDraft) { Draft = NewDraft; }
	void BeginCaptureForTesting(EWoTManagedBinding Action, EWoTBindingSlot Slot)
		{ BeginCapture(Action, Slot); }
	FString ErrorTextForTesting() const { return ErrorText.ToString(); }
	FString StatusTextForTesting() const { return StatusText.ToString(); }
	bool IsAwaitingVideoConfirmationForTesting() const { return VideoConfirmDeadline > 0.0; }
	double VideoConfirmDeadlineForTesting() const { return VideoConfirmDeadline; }
	FReply SaveForTesting() { return Save(); }
	/** The Back button's click: leaves at once, dropping the draft. */
	FReply BackForTesting() { return Back(); }
	TSharedPtr<SWidget> ConfirmSaveButtonForTesting() const { return ConfirmSaveButton; }
	TSharedPtr<SWidget> ConfirmDiscardButtonForTesting() const { return ConfirmDiscardButton; }
	TSharedPtr<SWidget> ConfirmPromptForTesting() const { return ConfirmPrompt; }
	FReply KeepVideoForTesting() { return KeepVideo(); }
	FReply RevertVideoForTesting() { return RevertVideo(); }
	FReply ResetBindingsForTesting() { return ResetBindings(); }
	TSharedPtr<SWidget> SaveButtonForTesting() const { return SaveButton; }
	TSharedPtr<SWidget> BackButtonForTesting() const { return BackButton; }
	TSharedPtr<SWidget> KeepButtonForTesting() const { return KeepButton; }
	TSharedPtr<SWidget> RevertButtonForTesting() const { return RevertButton; }
	/** Every focusable row of the body, top to bottom, then the footer. */
	const TArray<TArray<TSharedPtr<SWidget>>>& NavigationRowsForTesting() const
		{ return NavigationRows; }
	TSharedPtr<SWidget> BodyScrollForTesting() const { return BodyScroll; }
	bool IsAvailableForTesting() const { return Preferences.IsValid(); }
	FText ResolvedRetailTextForTesting(const TCHAR* Key) const
		{ return Retail(Key); }
	FText ActionLabelForTesting(EWoTManagedBinding Action) const
		{ return ActionLabel(Action); }
	float VSyncGlyphSizeForTesting() const { return BodyCheckBoxGlyphSize; }
	TSharedPtr<SWidget> WindowModeLabelForTesting() const
		{ return WindowModeLabel; }
	TSharedPtr<SWidget> WindowModeControlForTesting() const
		{ return WindowModeControl; }
	TSharedPtr<SWidget> ControllerSensitivitySliderForTesting() const
		{ return ControllerSensitivitySlider; }
	TSharedPtr<SCheckBox> ControllerInvertCheckBoxForTesting() const
		{ return ControllerInvertCheckBox; }
	/** The Steam keyboard toggle; null where Steam's keyboard cannot open. */
	TSharedPtr<SCheckBox> SteamKeyboardCheckBoxForTesting() const
		{ return SteamKeyboardCheckBox; }
	bool SteamKeyboardDraftForTesting() const { return bSteamKeyboardDraft; }
	bool SharingDraftForTesting() const { return bSharingDraft; }
	bool SharingConfirmedForTesting() const { return bSharingConfirmed; }
	TSharedPtr<SCheckBox> SharingCheckBoxForTesting() const
		{ return SharingCheckBox; }
	bool IsSharingHintVisibleForTesting() const { return IsSharingHintVisible(); }
	TSharedPtr<STextBlock> SharingHintForTesting() const { return SharingHint; }
	/** The game instance's sharing subsystem, for a screen built outside one. */
	void SetSharingSubsystemForTesting(UWoTInstallerSharingSubsystem* Sharing);
#endif

private:
	/** Fills BodyScroll, one scroll-box slot per row (spec r3 [I5]). */
	void BuildBody();
	void BuildControlsBody();
	void BuildHardwareBody();
	TSharedRef<SWidget> BuildBindingRow(EWoTManagedBinding Action);
	TSharedRef<SWidget> BuildBindingButton(EWoTManagedBinding Action, EWoTBindingSlot Slot);
	/** After Keep/Revert closes the strip, focus returns to Save. */
	void FocusSaveAfterStrip(bool bAlways);
	TSharedRef<SWidget> BuildLabel(const FText& Text);
	TSharedRef<SWidget> BuildChoiceRow(const FText& Label,
		TSharedRef<SWidget> Control, TSharedPtr<SWidget>* LabelCapture = nullptr);

	FText Retail(const TCHAR* Key) const;
	FText ActionLabel(EWoTManagedBinding Action) const;
	FText BindingText(EWoTManagedBinding Action, EWoTBindingSlot Slot) const;
	FSlateFontInfo Body() const { return BodyFont.Get(FSlateFontInfo()); }
	FSlateFontInfo Heading() const { return HeadingFont.Get(FSlateFontInfo()); }

	void BeginCapture(EWoTManagedBinding Action, EWoTBindingSlot Slot);
	void CancelCapture();
	bool CommitCapturedKey(const FKey& Key);
	FReply ResetBindings();
	FReply Save();
	FReply Back();
	/** Drops the draft (restoring any live preview) and leaves the screen. */
	void LeaveDiscardingDraft();
	void OpenSaveConfirm();
	/** Closes the prompt; with bRestoreFocus, focus returns where it was. */
	void CloseSaveConfirm(bool bRestoreFocus);
	/** Shows or hides the prompt and disables or enables the page behind it. */
	void SetConfirmShown(bool bShown);
	/** The prompt's Save: exactly the Save button. */
	FReply ConfirmSave();
	/** The prompt's Discard: exactly the Back button. */
	FReply ConfirmDiscard();
	FReply KeepVideo();
	FReply RevertVideo();
	bool CommitDraft();
	/** Persists a changed sharing switch and tells the sharer. */
	bool CommitSharingDraft();
	UWoTInstallerSharingSubsystem* ResolveSharing() const;
	bool IsSharingHintVisible() const;
	bool VideoDiffersFromConfirmed() const;
	void RevertDraftVideoToConfirmed();
	void SetError(const FText& Text);
	void SetStatus(const FText& Text);

	EWoTSettingsPage Page = EWoTSettingsPage::Controls;
	TWeakObjectPtr<UWoTPreferencesSubsystem> Preferences;
	TSharedPtr<FWoTFrontEndPresentationAuthority> PresentationAuthority;
	TAttribute<FSlateFontInfo> HeadingFont;
	TAttribute<FSlateFontInfo> BodyFont;
	FWoTSettingsTextResolver TextResolver;
	FSimpleDelegate OnBack;
	FSimpleDelegate OnSaved;

	FWoTGameSettings Confirmed;
	FWoTGameSettings Draft;
	FText ErrorText;
	FText StatusText;
	TOptional<EWoTManagedBinding> CaptureAction;
	EWoTBindingSlot CaptureSlot = EWoTBindingSlot::Primary;
	double VideoConfirmDeadline = 0.0;
	double LastTickTime = 0.0;
	bool bVideoPreviewLive = false;
	/** "Share the original installer on this network" (Hardware > NETWORK):
	 * a preferences side file, saved with the rest of the page. */
	bool bSharingConfirmed = true;
	bool bSharingDraft = true;

	TSharedPtr<SWidget> SaveButton;
	TSharedPtr<SWidget> BackButton;
	TSharedPtr<SWidget> KeepButton;
	TSharedPtr<SWidget> RevertButton;
	TSharedPtr<SScrollBox> BodyScroll;
	/** The focus graph's rows: body rows, then Save and Back. */
	TArray<TArray<TSharedPtr<SWidget>>> NavigationRows;
	/** B cancels a live capture; B on the video strip reverts; B on the
	 * "Save changes?" prompt closes it. */
	TSharedPtr<FWoTBackStackEntryHandle> CaptureBackEntry;
	TSharedPtr<FWoTBackStackEntryHandle> VideoStripBackEntry;
	TSharedPtr<FWoTBackStackEntryHandle> ConfirmBackEntry;
	/** The "Save changes?" prompt over the page, its two buttons, and the
	 * page's host, disabled while the prompt is open. */
	TSharedPtr<SWidget> ConfirmPrompt;
	TSharedPtr<SWidget> PageHost;
	TSharedPtr<SWidget> ConfirmSaveButton;
	TSharedPtr<SWidget> ConfirmDiscardButton;
	/** Where focus was when the prompt opened; it returns there on B. */
	TWeakPtr<SWidget> FocusBeforeConfirm;
	bool bConfirmingUnsaved = false;
	TSharedPtr<SWidget> WindowModeLabel;
	TSharedPtr<SWidget> WindowModeControl;
	TSharedPtr<SWidget> ControllerSensitivitySlider;
	TSharedPtr<SCheckBox> ControllerInvertCheckBox;
	/** Steam's keyboard for text fields: its own side file, not the settings. */
	TSharedPtr<SCheckBox> SteamKeyboardCheckBox;
	bool bSteamKeyboardConfirmed = false;
	bool bSteamKeyboardDraft = false;
	TSharedPtr<SCheckBox> SharingCheckBox;
	TSharedPtr<STextBlock> SharingHint;
#if WITH_AUTOMATION_TESTS
	TWeakObjectPtr<UWoTInstallerSharingSubsystem> SharingForTesting;
#endif
	FCheckBoxStyle BodyCheckBoxStyle;
	float BodyCheckBoxGlyphSize = 0.0f;
	TArray<TSharedPtr<FString>> ResolutionItems;
	TArray<TSharedPtr<FString>> WindowModeItems;
	TArray<TSharedPtr<FString>> QualityItems;
	TArray<TSharedPtr<FString>> FrameRateItems;
	TArray<TSharedPtr<FString>> BrightnessItems;
};
