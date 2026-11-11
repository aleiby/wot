#pragma once

#include "CoreMinimal.h"
#include "Framework/SlateDelegates.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SCompoundWidget.h"
#include "WoTPayloadSoundWave.h"
#include "WoTMenuStrings.h"

class UFont;
class UAudioComponent;
class UWoTPayloadSubsystem;
class UTexture2D;
class UWoTHUDFonts;
class FWoTRuntimeClassDefaults;
class APlayerController;
class FWoTCacheGenerationLease;
class FWoTRetailBrowserStyle;
struct FWoTSoundPayloadData;

struct FWoTBrowserRegion
{
	int32 X = 0;
	int32 Y = 0;
	int32 W = 0;
	int32 H = 0;
};

// Complete typed input consumed by the browser skin. Authenticated retail
// instances are decoded from UWindowWin95LookAndFeel (including inherited
// defaults); pre-cache instances use an intentionally non-retail neutral row.
struct FWoTBrowserLookAndFeel
{
	TMap<FName, FWoTBrowserRegion> Regions;
	FLinearColor FrameActiveTitleColor = FLinearColor(0.82f, 0.86f, 0.9f);
	FLinearColor FrameInactiveTitleColor = FLinearColor(0.52f, 0.56f, 0.6f);
	FLinearColor HeadingActiveTitleColor = FLinearColor::Black;
	FLinearColor HeadingInActiveTitleColor = FLinearColor::Black;
	FLinearColor EditBoxTextColor = FLinearColor::Black;
	float ScrollbarWidth = 14.0f;
	float ColumnHeadingHeight = 13.0f;
};

enum class EWoTBrowserSortColumn : uint8
{
	Server,
	Ping,
	Map,
	Players
};

// UBrowserServerGrid.uc:39-47,126-135: Ping starts ascending; selecting the
// current column reverses it, while selecting another resets descending.
struct FWoTBrowserSortState
{
	EWoTBrowserSortColumn Column = EWoTBrowserSortColumn::Ping;
	bool bDescending = false;

	void Select(EWoTBrowserSortColumn NewColumn);
	static int32 CompareText(const FString& A, const FString& B);
};

struct FWoTRetailMenuLayout
{
	FSlateRect SafeRegion;
	FVector2D ListPosition = FVector2D::ZeroVector;
	FVector2D ListSize = FVector2D::ZeroVector;
	float ItemHeight = 0.0f;
	float Spacing = 0.0f;
};

// Pure menuWOT.DetermineMenuListLayout parity math.
struct FWoTMenuLayout
{
	static constexpr float BaseWidth = 640.0f;
	static constexpr float HighResolutionSpacingRatio = 1.0f / 3.0f;
	static constexpr float LeftColumnOffset = 160.0f;
	static constexpr float RightColumnOffset = 80.0f;

	static float CalculateSpacing(
		int32 MenuLength, float ItemHeight, float AvailableHeight,
		float SpacingRatio = HighResolutionSpacingRatio);
	static float CalculateListHeight(
		int32 MenuLength, float ItemHeight, float Spacing);
	static FWoTRetailMenuLayout CalculateRetailMainLayout(
		FVector2D ViewportSize, TConstArrayView<FVector2D> MeasuredLabels);
	static FVector2D MeasureText(const FText& Text, const FSlateFontInfo& Font);
	static float MeasureFontHeight(const FSlateFontInfo& Font);
	static float ScaleX(float Value, float ViewportWidth);
	static float LeftColumnStart(float ViewportWidth);
	static float RightColumnStart(float ViewportWidth);
};

class IWoTFrontEndFeedbackSink
{
public:
	virtual ~IWoTFrontEndFeedbackSink() = default;
	virtual bool IsReady() const { return true; }
	virtual void MoveSelector() = 0;
	virtual void Select() = 0;
};

// Pure, reviewable contract consumed by the authenticated production sink.
// Keeping identifiers and typed volume gating here lets automation detect a
// production-routing typo without weakening the payload boundary.
struct FWoTFrontEndFeedbackContract
{
	static const TCHAR* MoveSelectorLogicalId();
	static const TCHAR* SelectLogicalId();
	static bool ShouldPlay(
		float AuthenticatedBootstrapVolume, float AppVolumeMultiplier);
};

// One index and one feedback boundary for keyboard/controller focus,
// programmatic focus, hover, and click. Calls never emit on invalid/no-op
// moves, and Select is invoked in the same frame immediately before Action.
class FWoTFrontEndMenuSelection final
{
public:
	explicit FWoTFrontEndMenuSelection(
		TSharedRef<IWoTFrontEndFeedbackSink> InFeedback)
		: Feedback(MoveTemp(InFeedback))
	{
	}

	int32 GetSelectedIndex() const { return SelectedIndex; }
	bool MoveTo(int32 NewIndex);
	bool Activate(int32 Index, TFunctionRef<void()> Action);

private:
	static constexpr int32 EntryCount = 4;
	TSharedRef<IWoTFrontEndFeedbackSink> Feedback;
	int32 SelectedIndex = 0;
};

struct FWoTMenuFontSelection
{
	static FSlateFontInfo Select(
		UFont* RetailFont, float Size, FName FallbackTypeface);
};

class FWoTFrontEndFontOwner;

/** Revocable identity for one authenticated owner publication. */
class WOT_API FWoTRequiredMenuFontGeneration final
{
public:
	bool IsValid() const { return bValid; }
	void BindWidget(const TSharedRef<SWidget>& Widget);
#if WITH_AUTOMATION_TESTS
	static TSharedRef<FWoTRequiredMenuFontGeneration> CreateForTesting()
	{
		TSharedRef<FWoTRequiredMenuFontGeneration> Result =
			MakeShared<FWoTRequiredMenuFontGeneration>();
		Result->bValid = true;
		return Result;
	}
	void RevokeForTesting() { Revoke(); }
#endif

private:
	static TSharedRef<FWoTRequiredMenuFontGeneration>
		CreateForIntegrityRecovery();
	void Revoke();
	bool bValid = false;
	TArray<TWeakPtr<SWidget>> Widgets;
	friend class FWoTFrontEndFontOwner;
	friend class FWoTFrontEndPresentationAuthority;
};

/** The two authenticated faces required before guarded menu text may shape. */
struct FWoTRequiredMenuFonts
{
	FSlateFontInfo Title;
	FSlateFontInfo List;
	TWeakPtr<FWoTRequiredMenuFontGeneration> Generation;

	bool IsValid() const
	{
		const TSharedPtr<const FWoTRequiredMenuFontGeneration> Pinned =
			Generation.Pin();
		return Pinned.IsValid() && Pinned->IsValid()
			&& Title.HasValidFont() && List.HasValidFont();
	}

	void BindWidget(const TSharedRef<SWidget>& Widget) const
	{
		if (const TSharedPtr<FWoTRequiredMenuFontGeneration> Pinned =
			Generation.Pin())
		{
			Pinned->BindWidget(Widget);
		}
		else
		{
			Widget->SetVisibility(EVisibility::Collapsed);
		}
	}
};

/** One immutable, revocable presentation capability for a single verified
 * cache snapshot. Fonts, the bounded main-menu localization projection, and
 * the two feedback waves are admitted and revoked as one unit. */
class WOT_API FWoTFrontEndPresentationAuthority final
	: public IWoTFrontEndFeedbackSink
	, public TSharedFromThis<FWoTFrontEndPresentationAuthority>
{
public:
	static TSharedPtr<FWoTFrontEndPresentationAuthority> Create(
		APlayerController* Controller);
	/** Release process-retained recovery objects when their owning game-instance
	 * payload session is shutting down. */
	static void ShutdownIntegrityRecoverySnapshot();
	~FWoTFrontEndPresentationAuthority();

	virtual bool IsReady() const override;
	bool IsReadyForFeedback() const { return IsReady(); }
	const FWoTRequiredMenuFonts* RequiredFonts() const;
	FSlateFontInfo MultiplayerFont(FName Face, float ViewportScale,
		int32 MinimumPixelScale = 1) const;
	FText Text(FName Id) const;
	bool PrepareBrowserResources();
	const FWoTRetailBrowserStyle* BrowserStyle() const;
	FText BrowserResourceError() const;
	const FString& SnapshotIdentity() const { return CapturedSnapshotIdentity; }
	void BindWidget(const TSharedRef<SWidget>& Widget);

	virtual void MoveSelector() override;
	virtual void Select() override;

#if WITH_AUTOMATION_TESTS
	static TSharedRef<FWoTFrontEndPresentationAuthority> CreateForTesting(
		FString SnapshotIdentity, FSlateFontInfo Title, FSlateFontInfo List,
		TMap<FName, FString>&& Localization,
		TSharedRef<IWoTFrontEndFeedbackSink> Feedback);
	void SetBrowserLookAndFeelForTesting(
		const FWoTBrowserLookAndFeel& LookAndFeel);
	void SetBrowserResourcesUnavailableForTesting(bool bUnavailable);
	void RevokeForTesting();
	bool DidRunRevocationBarrierForTesting() const;
	bool HasCapturedFeedbackBindingsForTesting() const;
	bool HasLiveLeaseForTesting() const;
	int32 ActiveFeedbackComponentCountForTesting() const;
	int32 BoundWidgetCountForTesting() const { return BoundWidgets.Num(); }
#endif

private:
	FWoTFrontEndPresentationAuthority();
	bool Initialize(APlayerController& Controller);
	bool InitializeIntegrityRecovery(APlayerController& Controller);
	bool BindWave(UWoTPayloadSubsystem& Payload, const TCHAR* LogicalId,
		TStrongObjectPtr<UWoTPayloadSoundWave>& OutWave,
		TSharedPtr<const FWoTSoundPayloadData>& OutPayload);
	bool ValidateCapturedIdentity() const;
	void Play(UWoTPayloadSoundWave* Wave);
	void RevokeOnGameThread();
	void ReleaseRevocationSubscription();

	TUniquePtr<FWoTCacheGenerationLease> PinnedLease;
	TWeakObjectPtr<APlayerController> PlaybackContext;
	TWeakObjectPtr<UWoTPayloadSubsystem> PayloadOwner;
	FWoTRequiredMenuFonts Fonts;
	TMap<FName, FString> Localization;
	FString CapturedSnapshotIdentity;
	FString LocalizationSnapshotIdentity;
	TStrongObjectPtr<UWoTPayloadSoundWave> MoveSelectorWave;
	TStrongObjectPtr<UWoTPayloadSoundWave> SelectWave;
	TSharedPtr<const FWoTSoundPayloadData> MoveSelectorPayload;
	TSharedPtr<const FWoTSoundPayloadData> SelectPayload;
	TArray<TWeakObjectPtr<UAudioComponent>> ActiveFeedbackComponents;
	TArray<TWeakPtr<SWidget>> BoundWidgets;
	TSharedPtr<IWoTFrontEndFeedbackSink> TestingFeedback;
	TSharedPtr<FWoTRetailBrowserStyle> BrowserResources;
	FText BrowserFailureMessage;
	uint64 RevocationSubscription = 0;
	bool bAudioEnabled = false;
	bool bValid = false;
	bool bIntegrityRecovery = false;
	bool bTestingAuthority = false;
#if WITH_AUTOMATION_TESTS
	bool bRevocationBarrierRan = false;
	bool bBrowserResourcesUnavailableForTesting = false;
#endif
};

class FWoTMenuStyle
{
public:
	static FWoTMenuStyle& Get();

	/** Returns no value while either guarded face is unavailable or revoked.
	 * Callers must hide/drop dependent widgets instead of constructing an empty
	 * FSlateFontInfo, because pinned Slate maps empty infos to last resort. */
	TOptional<FWoTRequiredMenuFonts> ResolveRequiredFonts() const;
	/** The base small face at its retail size 8. Its one user, the item-info
	 * element gems, draws it through LegibleSmallFont instead. */
	const FSlateFontInfo& SmallFont() const { return Small; }
	const FSlateFontInfo& ItalicFont() const { return Italic; }

	/** Steam Deck verification, Display > Text Legibility: no on-screen
	 * character under 9 physical pixels at 1280x800. */
	static constexpr float MinimumLegiblePixels = 9.0f;
	/** The layout scale the game viewport applies at PhysicalSize pixels: the
	 * engine's UI DPI curve (UUserInterfaceSettings, ShortestSide). */
	static float ViewportDpiScale(const FVector2D& PhysicalSize);
	/** Physical pixels of Slate text of Size points under LayoutScale:
	 * Size x 96/72 x LayoutScale. */
	static float PhysicalTextPixels(float Size, float LayoutScale);
	/** The smallest whole point size, never below Size, whose text is at least
	 * MinimumLegiblePixels tall under LayoutScale. */
	static float LegibleFontSize(float Size, float LayoutScale);
	/** SmallFont raised to the legibility floor under LayoutScale (10 at
	 * 1280x800, 11 at 1280x720, unchanged 8 from 1080p up). The style is built
	 * once, so callers pass their live layout scale on every draw and a resize
	 * never leaves a stale size. */
	FSlateFontInfo LegibleSmallFont(float LayoutScale) const;

	const FSlateBrush* Background() const { return &BackgroundBrush; }
	const FSlateBrush* Blackout() const { return &BlackoutBrush; }
	const FSlateBrush* Frame() const { return &FrameBrush; }
	const FSlateBrush* SliderLeftCap() const { return &Slide1Brush; }
	const FSlateBrush* SliderKnob() const { return &Slide2Brush; }
	const FSlateBrush* SliderRightCap() const { return &Slide3Brush; }
	const FSlateBrush* SliderEmptyCell() const { return &Slide4Brush; }
	const FSlateBrush* Separator() const { return &SeparatorBrush; }
	const FButtonStyle* ButtonStyle() const { return &MenuButtonStyle; }
	const FEditableTextBoxStyle* EditableTextBoxStyle() const
		{ return &MenuEditableTextBoxStyle; }
	const FComboBoxStyle* ComboBoxStyle() const { return &MenuComboBoxStyle; }
	const FCheckBoxStyle* CheckBoxStyle() const { return &MenuCheckBoxStyle; }
	/** The check box square for labels in Font: the font's size in Slate units
	 * (0.75 of its bitmap pixel height on screen at every viewport scale),
	 * never under the base 12. */
	static float CheckGlyphSizeFor(const FSlateFontInfo& Font);
	/** Sizes all nine check images (states share one square, so hovering or
	 * pressing never resizes the box). */
	static void SizeCheckBoxImages(FCheckBoxStyle& Style, float GlyphSize);
	const FTableRowStyle* TableRowStyle() const { return &MenuTableRowStyle; }
	const FTableViewStyle* TableViewStyle() const { return &MenuTableViewStyle; }
	const FScrollBarStyle* ScrollBarStyle() const { return &MenuScrollBarStyle; }
	const FScrollBoxStyle* ScrollBoxStyle() const { return &MenuScrollBoxStyle; }

	FText RetailText(FName Id) const;
	bool BindAuthenticatedMainMenuLocalization(
		const FWoTRequiredMenuFonts& Fonts);
	bool LoadAuthenticatedMainMenuLocalization(
		const FWoTRequiredMenuFonts& Fonts,
		TMap<FName, FString>& OutStrings) const;
	static bool ParseAuthenticatedMainMenuLocalization(
		TConstArrayView<uint8> Bytes, TMap<FName, FString>& OutStrings);

	static FLinearColor SelectedColor();
	static FLinearColor UnselectedColor();
	static FLinearColor DisabledColor();
	static FSlateColor MenuColor(bool bSelected);

public:
	// Re-apply every image brush against its texture's CURRENT platform data.
	// The style's own LoadTexture calls are what first bring the stripped UI
	// textures into memory, so ConfigureImageBrush necessarily runs against
	// PLACEHOLDER dimensions and resolves a placeholder Slate resource;
	// payload injection swaps the real mips in afterwards and the drawn brush
	// never saw them - the packaged menu rendered black. Call after a payload
	// sweep. Idempotent.
	/** True only when every recorded menu image brush was refreshed against
	 * its still-live texture. */
	bool RefreshTextureBrushes();
	/** Exact texture set that the root will present after handoff. Null entries
	 * are preserved so missing loads fail the payload gate explicitly. */
	TArray<UTexture2D*> RequiredTexturePayloads() const;
	/** Atomically root and wire the closed six authenticated menu consumers. */
	bool BindTextureConsumers(TConstArrayView<UTexture2D*> Textures);
	/** Synchronously remove all texture consumers and their brush references. */
	void UnbindTextureConsumers();
#if WITH_AUTOMATION_TESTS
	/** Remove constructor-time editor textures so guarded absence and multiple
	 * payload lifecycles can be exercised in one editor process. */
	void ClearTextureConsumersForTesting();
	/** Hide the editor-only DA_WoTMenuStrings fallback, as in the neutral
	 * package (which ships no menu strings), so specs see what players see. */
	void SetEditorMenuStringsHiddenForTesting(bool bHidden)
		{ bEditorMenuStringsHiddenForTesting = bHidden; }
	/** The editor's small face is its offline F_WOTReg08 asset, which Slate
	 * cannot shape (it draws LastResort boxes); specs that measure layout swap
	 * in the face a guarded build resolves. */
	void SetSmallFontForTesting(const FSlateFontInfo& Font) { Small = Font; }
#endif

private:
	FWoTMenuStyle();

	UFont* ResolveFont(UWoTHUDFonts* Registry, FName Id);
	UTexture2D* LoadTexture(const TCHAR* AssetPath);
	void ConfigureImageBrush(
		FSlateBrush& Brush, UTexture2D* Texture, bool bTile = false);
	struct FConfiguredImageBrush
	{
		FSlateBrush* Brush = nullptr;
		TWeakObjectPtr<UTexture2D> Texture;
		bool bTile = false;
	};
	TArray<FConfiguredImageBrush> ConfiguredImageBrushes;
	TArray<TStrongObjectPtr<UObject>> LoadedObjects;
	TStrongObjectPtr<UWoTMenuStrings> MenuStrings;
#if WITH_AUTOMATION_TESTS
	bool bEditorMenuStringsHiddenForTesting = false;
#endif
	FSlateFontInfo Small;
	FSlateFontInfo Italic;
	FSlateBrush BackgroundBrush;
	FSlateBrush BlackoutBrush;
	FSlateBrush FrameBrush;
	FSlateBrush Slide1Brush;
	FSlateBrush Slide2Brush;
	FSlateBrush Slide3Brush;
	FSlateBrush Slide4Brush;
	FSlateBrush SeparatorBrush;
	FButtonStyle MenuButtonStyle;
	FEditableTextBoxStyle MenuEditableTextBoxStyle;
	FComboBoxStyle MenuComboBoxStyle;
	FCheckBoxStyle MenuCheckBoxStyle;
	FTableRowStyle MenuTableRowStyle;
	FTableViewStyle MenuTableViewStyle;
	FScrollBarStyle MenuScrollBarStyle;
	FScrollBoxStyle MenuScrollBoxStyle;
};

class FWoTRetailBrowserStyle
{
public:
	static FWoTRetailBrowserStyle& Get();
	static TSharedPtr<FWoTRetailBrowserStyle> CreateAuthenticated(
		UWoTPayloadSubsystem& Payload, const FWoTCacheGenerationLease& Lease,
		const FSlateFontInfo& Font, FString& OutError, bool bGameSkin = false);
	void ReleaseAuthenticatedResources();
#if WITH_AUTOMATION_TESTS
	static TSharedRef<FWoTRetailBrowserStyle> CreateForTesting(
		const FSlateFontInfo& Font,
		const FWoTBrowserLookAndFeel* DecodedLookAndFeel = nullptr);
	static bool DecodeLookAndFeelForTesting(
		const FWoTRuntimeClassDefaults& Defaults,
		FWoTBrowserLookAndFeel& Out, FString& OutError);
	bool IsGameSkinForTesting() const { return bGameSkin; }
#endif

	const FSlateFontInfo& NormalFont() const { return Normal; }
	const FSlateFontInfo& BoldFont() const { return Bold; }
	const FSlateFontInfo& LargeFont() const { return Large; }
	const FSlateFontInfo& LargeBoldFont() const { return LargeBold; }

	const FSlateBrush* WindowFrame() const { return &WindowFrameBrush; }
	const FSlateBrush* RaisedBevel() const { return &RaisedBevelBrush; }
	const FSlateBrush* SunkenBevel() const { return &SunkenBevelBrush; }
	const FSlateBrush* ClientBackground() const { return &ClientBrush; }
	const FSlateBrush* FaceBackground() const { return &FaceBrush; }
	const FButtonStyle* ButtonStyle() const { return &BrowserButtonStyle; }
	const FButtonStyle* HeadingStyle() const { return &BrowserHeadingStyle; }
	const FEditableTextBoxStyle* EditableTextBoxStyle() const
		{ return &BrowserEditableTextBoxStyle; }
	const FTableRowStyle* TableRowStyle() const { return &BrowserTableRowStyle; }
	const FTableViewStyle* TableViewStyle() const
		{ return &BrowserTableViewStyle; }
	const FScrollBarStyle* ScrollBarStyle() const
		{ return &BrowserScrollBarStyle; }
	const FScrollBoxStyle* ScrollBoxStyle() const
		{ return &BrowserScrollBoxStyle; }

	static FLinearColor ClientColor();
	static FLinearColor BodyTextColor();
	static FLinearColor CellTextColor();
	static FLinearColor DisabledColor();
	// Deliberate port skin colors, not retail UWindow data.
	static FLinearColor GameTextColor() { return FLinearColor(0.88f, 0.84f, 0.73f); }
	static FLinearColor GameAccentColor() { return FLinearColor(0.48f, 0.34f, 0.13f); }
	FLinearColor BodyForeground() const { return bGameSkin ? GameTextColor() : BodyTextColor(); }
	FLinearColor HeadingForeground(bool bActive = true) const
	{
		return bGameSkin ? GameTextColor()
			: bActive ? LookAndFeel.HeadingActiveTitleColor
				: LookAndFeel.HeadingInActiveTitleColor;
	}
	FLinearColor EditForeground() const
		{ return bGameSkin ? GameTextColor() : LookAndFeel.EditBoxTextColor; }
	FLinearColor CellForeground() const { return bGameSkin ? GameTextColor() : CellTextColor(); }
	FLinearColor TitleForeground() const
	{
		return bGameSkin ? GameTextColor() : LookAndFeel.FrameActiveTitleColor;
	}
	FLinearColor InactiveTitleForeground() const
	{
		return bGameSkin ? GameTextColor() : LookAndFeel.FrameInactiveTitleColor;
	}

	static constexpr float GUIScale = 2.0f;
	// UBrowserServerGrid.uc:20-30 Created() authors RowHeight=12.
	static constexpr float RowHeight = 12.0f * GUIScale;
	float HeadingHeight() const { return LookAndFeel.ColumnHeadingHeight * GUIScale; }
	float TitleBarHeight() const;
	float StatusBarHeight() const;
	float ScrollBarReservation() const
	{
		return LookAndFeel.ScrollbarWidth * GUIScale + 4.0f;
	}

private:
	explicit FWoTRetailBrowserStyle(bool bLoadLegacyResources = true);
	void ConfigureWidgetStyles(const FWoTBrowserLookAndFeel& InLookAndFeel);
	void ConfigureGameSkin();
	bool bGameSkin = false;
	FWoTBrowserLookAndFeel LookAndFeel;

	UFont* ResolveFont(UWoTHUDFonts* Registry, FName Id);
	UTexture2D* LoadTexture(const TCHAR* AssetPath);
	void ConfigureRegionBrush(
		FSlateBrush& Brush, UTexture2D* Texture, FVector2f PixelPosition,
		FVector2f PixelSize, ESlateBrushDrawType::Type DrawAs,
		FMargin Margin = FMargin(0.0f));

	TArray<TStrongObjectPtr<UObject>> LoadedObjects;
	FSlateFontInfo Normal;
	FSlateFontInfo Bold;
	FSlateFontInfo Large;
	FSlateFontInfo LargeBold;
	FSlateBrush WindowFrameBrush;
	FSlateBrush RaisedBevelBrush;
	FSlateBrush RecessedBevelBrush;
	FSlateBrush SunkenBevelBrush;
	FSlateBrush ScrollBackgroundBrush;
	FSlateBrush ScrollUpBrush;
	FSlateBrush ScrollDownBrush;
	FSlateBrush ScrollLeftBrush;
	FSlateBrush ScrollRightBrush;
	FSlateBrush SelectionBrush;
	FSlateBrush ClientBrush;
	FSlateBrush FaceBrush;
	FButtonStyle BrowserButtonStyle;
	FButtonStyle BrowserHeadingStyle;
	FEditableTextBoxStyle BrowserEditableTextBoxStyle;
	FTableRowStyle BrowserTableRowStyle;
	FTableViewStyle BrowserTableViewStyle;
	FScrollBarStyle BrowserScrollBarStyle;
	FScrollBoxStyle BrowserScrollBoxStyle;
};

/** Keys that back out of an in-game menu or window: Escape, and B or View on a controller. */
bool WoTIsMenuBackKey(const FKey& Key);

// SButton only applies its hovered foreground automatically. This small wrapper
// also treats keyboard focus as retail Selection and refreshes after key/mouse
// transitions so focused menu text remains pure white.
class SWoTMenuButton : public SButton
{
public:
	SLATE_BEGIN_ARGS(SWoTMenuButton)
		: _IsEnabled(true)
		, _ContentPadding(FMargin(4.0f, 2.0f))
		, _HAlign(HAlign_Fill)
	{}
		SLATE_ATTRIBUTE(bool, IsEnabled)
		SLATE_ATTRIBUTE(bool, IsSelected)
		SLATE_EVENT(FSimpleDelegate, OnSelected)
		SLATE_EVENT(FOnClicked, OnClicked)
		SLATE_ARGUMENT(FMargin, ContentPadding)
		SLATE_ARGUMENT(EHorizontalAlignment, HAlign)
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual FReply OnFocusReceived(
		const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override;
	virtual void OnMouseEnter(
		const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void Tick(
		const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;

private:
	TAttribute<bool> ExternalSelection;
	FSimpleDelegate OnSelected;
	bool bUsesExternalSelection = false;
};

class SWoTMenuText : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTMenuText)
		: _Large(false)
		, _Justification(ETextJustify::Left)
	{}
		SLATE_ATTRIBUTE(FText, Text)
		SLATE_ARGUMENT(bool, Large)
		SLATE_ARGUMENT(ETextJustify::Type, Justification)
		SLATE_ARGUMENT(TSharedPtr<FWoTFrontEndPresentationAuthority>,
			PresentationAuthority)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

class SWoTRetailSlider : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTRetailSlider)
		: _Value(0.0f)
		, _MinValue(0.0f)
		, _StepSize(0.125f)
		, _Positions(8)
	{}
		SLATE_ATTRIBUTE(float, Value)
		SLATE_ARGUMENT(float, MinValue)
		SLATE_ARGUMENT(float, StepSize)
		// Knob stops across the full MinValue..MinValue+8*StepSize range. The
		// retail menuWOT.DrawSlider has 8 (one per cell); a value the retail
		// menu stepped one unit at a time (time limit 0-60) uses one stop per unit.
		SLATE_ARGUMENT(int32, Positions)
		SLATE_EVENT(FOnFloatValueChanged, OnValueChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual bool SupportsKeyboardFocus() const override { return true; }
	// The knob's left edge for the current value: flush with the left cap at
	// the first stop and flush with the right cap at the last.
	float KnobLeft(float AllottedWidth) const;
	// All four arrows, Home and End step it for the keyboard; D-pad Left and
	// Right step it too, and D-pad Up/Down move focus (spec r3 section 2).
	virtual FReply OnKeyDown(
		const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	// Left/Right navigation from a controller (the stick, held-D-pad repeats)
	// steps the slider instead of moving focus.
	virtual FNavigationReply OnNavigation(
		const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent) override;
	virtual FReply OnMouseButtonDown(
		const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(
		const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(
		const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	FSlateColor SelectionColor() const;
	int32 ValueCell() const;
	int32 CellFromMouse(const FGeometry& MyGeometry, const FVector2D& ScreenPosition) const;
	void CommitCell(int32 CellIndex);

	TAttribute<float> Value;
	FOnFloatValueChanged OnValueChanged;
	float MinValue = 0.0f;
	float StepSize = 0.125f;
	int32 Positions = 8;
	bool bDragging = false;
};

/**
 * The value shown beside a menu slider, at one width shared by every menu
 * slider: the widest value any of them can show, in the label's font. Sliders
 * in a panel therefore end at the same x and never resize while dragging.
 */
class SWoTSliderValueLabel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTSliderValueLabel) {}
		SLATE_ATTRIBUTE(FText, Text)
		SLATE_ATTRIBUTE(FSlateFontInfo, Font)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Every value a menu slider can show: the host panel's whole numbers
	 * (Kills 0-100, Time 0-60, Players 2-16), Settings Master Volume 0%-100%,
	 * and the Settings Mouse Sensitivity steps (WoT.Menu.Slider.ValueLabels
	 * checks those against the screen's own steps). */
	static const TArray<FString>& AllValueLabels();
	/** The widest of AllValueLabels in LabelFont, or 0 when text cannot be
	 * measured (the label then sizes to its text). */
	static float SharedWidth(const FSlateFontInfo& LabelFont);

private:
	float CachedWidth(const FSlateFontInfo& LabelFont) const;

	TAttribute<FSlateFontInfo> Font;
	mutable FSlateFontInfo MeasuredFont;
	mutable float MeasuredWidth = 0.0f;
};
