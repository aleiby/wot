#include "WoTVRPresentation.h"

#include "WoTLocalPlayerHooks.h"
#include "WoTVRLog.h"
#include "WoTVRModule.h"
#include "WoTVRRig.h"
#include "WoTVRTuning.h"

#include "Engine/LocalPlayer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/NavigationConfig.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "IStereoLayers.h"
#include "Input/Events.h"
#include "Input/HittestGrid.h"
#include "Layout/ArrangedWidget.h"
#include "Widgets/SVirtualWindow.h"

const FName FWoTVRPresentation::Owner(TEXT("WoTVRPresentation"));

/** Sees Slate's input before any widget while the presentation is attached,
 * and hands it to the presentation, which consumes only what it routes. */
class FWoTVRUIInputProcessor final : public IInputProcessor
{
public:
	explicit FWoTVRUIInputProcessor(const TSharedRef<FWoTVRPresentation>& InOwner) : Owner(InOwner) {}

	virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event) override
	{
		const TSharedPtr<FWoTVRPresentation> Presentation = Owner.Pin();
		return Presentation && Presentation->HandleKey(Event, true);
	}

	virtual bool HandleKeyUpEvent(FSlateApplication&, const FKeyEvent& Event) override
	{
		const TSharedPtr<FWoTVRPresentation> Presentation = Owner.Pin();
		return Presentation && Presentation->HandleKey(Event, false);
	}

	virtual bool HandleAnalogInputEvent(FSlateApplication&, const FAnalogInputEvent& Event) override
	{
		const TSharedPtr<FWoTVRPresentation> Presentation = Owner.Pin();
		return Presentation && Presentation->HandleAnalog(Event);
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication&, const FPointerEvent& Event) override
	{
		const TSharedPtr<FWoTVRPresentation> Presentation = Owner.Pin();
		return Presentation && Presentation->HandleMouseButton(Event, true, false);
	}

	virtual bool HandleMouseButtonUpEvent(FSlateApplication&, const FPointerEvent& Event) override
	{
		const TSharedPtr<FWoTVRPresentation> Presentation = Owner.Pin();
		return Presentation && Presentation->HandleMouseButton(Event, false, false);
	}

	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication&, const FPointerEvent& Event) override
	{
		const TSharedPtr<FWoTVRPresentation> Presentation = Owner.Pin();
		return Presentation && Presentation->HandleMouseButton(Event, true, true);
	}

	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication&, const FPointerEvent& Event,
		const FPointerEvent*) override
	{
		const TSharedPtr<FWoTVRPresentation> Presentation = Owner.Pin();
		return Presentation && Presentation->HandleWheel(Event);
	}

	virtual const TCHAR* GetDebugName() const override { return TEXT("WoTVRPresentation"); }

private:
	TWeakPtr<FWoTVRPresentation> Owner;
};

namespace
{
IWoTVRSession* WoTVRPresentationSession()
{
	return FWoTVRModule::IsAvailable() ? &FWoTVRModule::Get().GetSession() : nullptr;
}

IStereoLayers* WoTVRPresentationLayers()
{
	const IWoTVRSession* Session = WoTVRPresentationSession();
	return Session ? Session->GetStereoLayers() : nullptr;
}

/** The confirm keys: the gamepad's A and the keyboard's Enter (the left mouse
 * button arrives as a mouse event). */
bool WoTVRIsConfirmKey(const FKey& Key)
{
	return Key == EKeys::Gamepad_FaceButton_Bottom || Key == EKeys::Enter
		|| Key == EKeys::Virtual_Accept;
}

/** The gamepad's navigation and back: the D-pad, the left stick's
 * directions and B. */
bool WoTVRIsNavigationKey(const FKey& Key)
{
	return Key == EKeys::Gamepad_DPad_Up || Key == EKeys::Gamepad_DPad_Down
		|| Key == EKeys::Gamepad_DPad_Left || Key == EKeys::Gamepad_DPad_Right
		|| Key == EKeys::Gamepad_LeftStick_Up || Key == EKeys::Gamepad_LeftStick_Down
		|| Key == EKeys::Gamepad_LeftStick_Left || Key == EKeys::Gamepad_LeftStick_Right
		|| Key == EKeys::Gamepad_FaceButton_Right || Key == EKeys::Virtual_Back;
}

/** The navigation keys that move focus: the D-pad and the left stick's
 * directions. */
bool WoTVRIsDirectionKey(const FKey& Key)
{
	return Key == EKeys::Gamepad_DPad_Up || Key == EKeys::Gamepad_DPad_Down
		|| Key == EKeys::Gamepad_DPad_Left || Key == EKeys::Gamepad_DPad_Right
		|| Key == EKeys::Gamepad_LeftStick_Up || Key == EKeys::Gamepad_LeftStick_Down
		|| Key == EKeys::Gamepad_LeftStick_Left || Key == EKeys::Gamepad_LeftStick_Right;
}

/** The left stick's axes, which navigate past the threshold. */
bool WoTVRIsNavigationAxis(const FKey& Key)
{
	return Key == EKeys::Gamepad_LeftX || Key == EKeys::Gamepad_LeftY;
}

/** A passive screen (an onboarding card, the online notice) is a notice or an
 * add-on's screen that takes no focus and no pointer: it shows beside the HUD
 * panel and never takes input. The game's own windows never are. */
bool WoTVRIsPassiveScreen(const SWidget& Screen, EWoTUIContent Kind)
{
	return (Kind == EWoTUIContent::Notice || Kind == EWoTUIContent::Extension)
		&& !Screen.SupportsKeyboardFocus() && Screen.GetVisibility() == EVisibility::HitTestInvisible;
}

/** Whether the player's focus is in editable text, which keeps Enter. */
bool WoTVRFocusEditsText(int32 User)
{
	const TSharedPtr<SWidget> Focused = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().GetUserFocusedWidget(User) : nullptr;
	if (!Focused.IsValid())
	{
		return false;
	}
	static const FName EditableText(TEXT("SEditableText"));
	static const FName MultiLineText(TEXT("SMultiLineEditableText"));
	const FName Type = Focused->GetType();
	return Type == EditableText || Type == MultiLineText;
}
}

FWoTVRPresentation::FWoTVRPresentation(ULocalPlayer& InPlayer)
	: Player(&InPlayer)
	, RaySource(MakeUnique<FWoTVRHeadGazeRay>())
{
}

FWoTVRPresentation::~FWoTVRPresentation()
{
	if (bAttached)
	{
		// The owner releases first; this is the fallback. The hooks' callbacks
		// cannot reach an object being destroyed, so the surfaces let their
		// screens go before the hooks put them back on the viewport.
		if (Menu)
		{
			Menu->Clear();
		}
		if (Cards)
		{
			Cards->Clear();
		}
		Detach();
	}
	ReleaseSurfaces();
}

UWorld* FWoTVRPresentation::GetWorld() const
{
	const ULocalPlayer* LocalPlayer = Player.Get();
	return LocalPlayer ? LocalPlayer->GetWorld() : nullptr;
}

void FWoTVRPresentation::SetRaySource(TUniquePtr<IWoTVRRaySource> Source)
{
	if (Source.IsValid())
	{
		RaySource = MoveTemp(Source);
	}
}

bool FWoTVRPresentation::Attach(const FWoTVRHeadPose& Head)
{
	ULocalPlayer* LocalPlayer = Player.Get();
	if (bAttached || !LocalPlayer || !FSlateApplication::IsInitialized())
	{
		return bAttached;
	}
	if (!WoTVRPresentationLayers())
	{
		UE_LOG(LogWoTVR, Warning,
			TEXT("The headset's stereo device offers no stereo layers: the HUD and menus stay on the desktop window"));
		ReleaseSurfaces();
		return false;
	}
	// Made on the first attach of a VR session; a later one (after travel, a
	// lost session or the runtime's dashboard) finds them kept by the detach.
	if (!HUD)
	{
		HUD = MakeUnique<FWoTVRCanvasSurface>(FIntPoint(HUDPixelsX, HUDPixelsY), TEXT("WoTVRHUD"));
	}
	if (!Reading)
	{
		Reading = MakeUnique<FWoTVRCanvasSurface>(FIntPoint(ReadingPixelsX, ReadingPixelsY), TEXT("WoTVRReading"));
	}
	if (!Menu)
	{
		Menu = MakeUnique<FWoTVRSlateSurface>(FIntPoint(MenuPixelsX, MenuPixelsY), /*bWithReticle*/ true);
	}
	if (!Cards)
	{
		Cards = MakeUnique<FWoTVRSlateSurface>(FIntPoint(CardPixelsX, CardPixelsY), /*bWithReticle*/ false);
	}
	Menu->SetRegistered(true);
	Cards->SetRegistered(true);
	LastHead = Head;
	HUDYaw = FWoTVRRig::ForwardYaw(Head.Orientation, 0.0);
	bHUDFollowing = false;
	bHUDWanted = false;
	bReadingWanted = false;
	bSnapshotSinceTick = false;
	bPlaceReading = false;
	bPlaceMenu = true;
	ReadingPage = 0;
	ReadingPageCount = 1;
	bPointerOnMenu = false;
	LastRoutedMove.Reset();
	bPointerHovering = false;
	bPreferFocus = false;
	NavigationControl.Reset();
	NavigationGaze.Reset();
	Held.Reset();

	const TWeakPtr<FWoTVRPresentation> Weak = AsShared();
	FWoTLocalPlayerConsumer Consumer;
	Consumer.Lifecycle = [Weak](EWoTLocalPlayerEvent Event, const FWoTLocalPlayerContext&)
	{
		const TSharedPtr<FWoTVRPresentation> Self = Weak.Pin();
		if (Self && Event == EWoTLocalPlayerEvent::Detached)
		{
			Self->HandleDetachedByGame();
		}
	};
	Consumer.HUDDestination = [Weak](const FWoTHUDSnapshot& Snapshot)
	{
		if (const TSharedPtr<FWoTVRPresentation> Self = Weak.Pin())
		{
			Self->PresentHUD(Snapshot);
		}
	};
	FWoTUIHost& Host = Consumer.UIHost.Emplace();
	Host.Show = [Weak](const TSharedRef<SWidget>& Screen, int32 ZOrder, EWoTUIContent Kind)
	{
		const TSharedPtr<FWoTVRPresentation> Self = Weak.Pin();
		return Self && Self->ShowScreen(Screen, ZOrder, Kind);
	};
	Host.Hide = [Weak](const TSharedRef<SWidget>& Screen)
	{
		if (const TSharedPtr<FWoTVRPresentation> Self = Weak.Pin())
		{
			Self->HideScreen(Screen);
		}
	};
	Host.Scope = [Weak]() -> TSharedPtr<SWidget>
	{
		const TSharedPtr<FWoTVRPresentation> Self = Weak.Pin();
		return Self && Self->Menu ? TSharedPtr<SWidget>(Self->Menu->GetWindow()) : nullptr;
	};
	Host.ScreenSize = []() { return FIntPoint(MenuPixelsX, MenuPixelsY); };
	// No runtime keyboard reaches the headset: every entry uses the game's panel.
	Host.bRuntimeTextEntry = false;
	// Attaching moves the screens already open onto the surfaces.
	bAttached = true;
	if (!FWoTLocalPlayerHooks::Attach(*LocalPlayer, Owner, Consumer))
	{
		bAttached = false;
		UE_LOG(LogWoTVR, Warning,
			TEXT("Another consumer owns this player's HUD destination or UI host: the HUD and menus stay on the desktop window"));
		ReleaseSurfaces();
		return false;
	}
	Processor = MakeShared<FWoTVRUIInputProcessor>(AsShared());
	// Ahead of every other preprocessor (the game's controller UI input among
	// them), which then sees only what the presentation routes.
	FInputPreprocessorRegistrationKey First;
	First.Type = EInputPreProcessorType::PreGame;
	First.Priority = 0;
	FSlateApplication::Get().RegisterInputPreProcessor(Processor, First);
	UE_LOG(LogWoTVR, Log, TEXT("Headset HUD and menus attached (%d screens moved onto the surfaces)"),
		Menu->Num() + Cards->Num());
	return true;
}

void FWoTVRPresentation::Detach()
{
	if (!bAttached)
	{
		return;
	}
	// From here the processor lets every event pass, and the hooks' own
	// release of what the routes hold reaches Slate unchanged.
	bAttached = false;
	bDetaching = true;
	Held.Reset();
	if (ULocalPlayer* LocalPlayer = Player.Get())
	{
		// Every screen goes back to the viewport with its focus, and the flat
		// HUD draws again from its next frame.
		FWoTLocalPlayerHooks::Detach(*LocalPlayer, Owner);
	}
	bDetaching = false;
	// Unregistered after the hooks let go: a switch made inside Slate's input
	// handling changes the processors only once that handling is done.
	if (Processor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(Processor);
	}
	Processor.Reset();
	Suspend();
	UE_LOG(LogWoTVR, Log, TEXT("Headset HUD and menus detached: the HUD and screens are back on the desktop window"));
}

void FWoTVRPresentation::Release()
{
	Detach();
	ReleaseSurfaces();
}

void FWoTVRPresentation::HandleDetachedByGame()
{
	if (bDetaching || !bAttached)
	{
		return;
	}
	// The game let the consumer go (the local player is leaving): the hooks
	// have already returned the screens.
	bAttached = false;
	if (Processor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(Processor);
	}
	Processor.Reset();
	Held.Reset();
	Suspend();
}

void FWoTVRPresentation::Suspend()
{
	// Hidden once and kept: a detach costs the runtime one hide per layer, and
	// the next attach shows the same layers, textures and windows again. The
	// windows leave Slate meanwhile, empty (the hooks returned every screen).
	IStereoLayers* Layers = WoTVRPresentationLayers();
	HUDPanel.Hide(Layers);
	CardPanel.Hide(Layers);
	ReadingPanel.Hide(Layers);
	MenuPanel.Hide(Layers);
	if (Menu)
	{
		Menu->Clear();
		Menu->SetReticle(TOptional<FVector2D>());
		Menu->SetRegistered(false);
	}
	if (Cards)
	{
		Cards->Clear();
		Cards->SetRegistered(false);
	}
	bHUDWanted = false;
	bReadingWanted = false;
	bSnapshotSinceTick = false;
	bPointerOnMenu = false;
	LastRoutedMove.Reset();
	// The hooks' detach sent the pointer's leave.
	bPointerHovering = false;
	bPreferFocus = false;
	NavigationControl.Reset();
	NavigationGaze.Reset();
}

void FWoTVRPresentation::ReleaseSurfaces()
{
	IStereoLayers* Layers = WoTVRPresentationLayers();
	HUDPanel.Destroy(Layers);
	CardPanel.Destroy(Layers);
	ReadingPanel.Destroy(Layers);
	MenuPanel.Destroy(Layers);
	Menu.Reset();
	Cards.Reset();
	HUD.Reset();
	Reading.Reset();
	bHUDWanted = false;
	bReadingWanted = false;
	bSnapshotSinceTick = false;
	bPointerOnMenu = false;
	bPointerHovering = false;
}

// --- the HUD (H5) ------------------------------------------------------------

void FWoTVRPresentation::PresentHUD(const FWoTHUDSnapshot& Snapshot)
{
	if (!bAttached || !HUD || !Reading)
	{
		return;
	}
	++HUDUpdates;
	bSnapshotSinceTick = true;
	UWorld* World = GetWorld();
	// The compact panel: the HUD's essentials, urgent warnings and the flash
	// included, once per update.
	FWoTHUDLayoutResult Compact;
	if (Snapshot.bVisible)
	{
		FWoTHUDLayoutOptions Options;
		Options.Layout = EWoTHUDLayout::Compact;
		Options.Size = HUD->GetPixels();
		Compact = FWoTHUDLayout::Arrange(Snapshot, Options);
	}
	bHUDWanted = !Compact.Sections.IsEmpty();
	if (bHUDWanted)
	{
		HUD->Draw(World, Compact.Sections, Compact.Extent);
	}
	// The reading panel: the scoreboard (or the citadel overview) while it
	// shows, held or shown by the game, a page at a time.
	const bool bBoard = Snapshot.bVisible && (Snapshot.bScoreboard || Snapshot.bTeamOverview);
	if (bBoard)
	{
		FWoTHUDLayoutOptions Options;
		Options.Layout = EWoTHUDLayout::Reading;
		Options.Size = Reading->GetPixels();
		Options.Page = ReadingPage;
		const FWoTHUDLayoutResult Board = FWoTHUDLayout::Arrange(Snapshot, Options);
		ReadingPage = Board.Page;
		ReadingPageCount = FMath::Max(Board.PageCount, 1);
		Reading->Draw(World, Board.Sections, Board.Extent);
		if (!bReadingWanted)
		{
			bPlaceReading = true;
		}
	}
	else
	{
		ReadingPage = 0;
		ReadingPageCount = 1;
	}
	bReadingWanted = bBoard;
}

// --- the screens (H6) --------------------------------------------------------

bool FWoTVRPresentation::ShowScreen(const TSharedRef<SWidget>& Screen, int32 ZOrder, EWoTUIContent Kind)
{
	if (!bAttached || !Menu || !Cards)
	{
		return false;
	}
	if (WoTVRIsPassiveScreen(*Screen, Kind))
	{
		Cards->Add(Screen, ZOrder);
		return true;
	}
	const bool bFirst = Menu->Num() == 0;
	Menu->Add(Screen, ZOrder);
	if (bFirst)
	{
		// World-locked in front of the view where the head is as it opens.
		MenuPose = PlaceInFront(LastHead, WoTVRTuning::MenuDistanceMeters(), WoTVRTuning::MenuWidthMeters(),
			Menu->GetPixels());
		bPlaceMenu = false;
		LastRoutedMove.Reset();
	}
	UE_LOG(LogWoTVR, Verbose, TEXT("Hosting %s (kind %d) on the headset menu surface"),
		*Screen->GetTypeAsString(), static_cast<int32>(Kind));
	return true;
}

void FWoTVRPresentation::HideScreen(const TSharedRef<SWidget>& Screen)
{
	if (Menu && Menu->Remove(Screen))
	{
		if (Menu->Num() == 0)
		{
			bPointerOnMenu = false;
			Menu->SetReticle(TOptional<FVector2D>());
			bPreferFocus = false;
			NavigationControl.Reset();
			NavigationGaze.Reset();
		}
		return;
	}
	if (Cards)
	{
		Cards->Remove(Screen);
	}
}

// --- per frame ---------------------------------------------------------------

FWoTVRPanelPose FWoTVRPresentation::PlaceInFront(const FWoTVRHeadPose& Head, float Distance, float Width,
	const FIntPoint& Pixels) const
{
	const double Yaw = FWoTVRRig::ForwardYaw(Head.Orientation, HUDYaw);
	return FWoTVRPanelPose::Facing(Head.PositionMeters, Yaw, 0.0, Distance, Width, Pixels);
}

void FWoTVRPresentation::UpdateHUDFollow(const FWoTVRHeadPose& Head, float DeltaSeconds)
{
	// Stable within the dead zone, then a gentle follow that settles on the
	// head's yaw; never the head's pitch or roll.
	const double HeadYaw = FWoTVRRig::ForwardYaw(Head.Orientation, HUDYaw);
	const double Gap = FMath::FindDeltaAngleDegrees(HUDYaw, HeadYaw);
	if (FMath::Abs(Gap) > WoTVRTuning::HUDFollowDeadZoneDegrees())
	{
		bHUDFollowing = true;
	}
	if (bHUDFollowing)
	{
		const double Step = Gap * (1.0 - FMath::Exp(-WoTVRTuning::HUDFollowRate() * FMath::Max(DeltaSeconds, 0.0f)));
		HUDYaw = FMath::UnwindDegrees(HUDYaw + Step);
		if (FMath::Abs(FMath::FindDeltaAngleDegrees(HUDYaw, HeadYaw)) < 1.0)
		{
			bHUDFollowing = false;
		}
	}
}

void FWoTVRPresentation::Tick(float DeltaSeconds, const FWoTVRHeadPose& Head)
{
	if (!bAttached || !HUD || !Reading || !Menu || !Cards)
	{
		return;
	}
	if (Head.bValid)
	{
		LastHead = Head;
		UpdateHUDFollow(Head, DeltaSeconds);
	}
	// The HUD updates once for each frame it draws (H5). A frame that brought
	// no snapshot means it stopped drawing (the HUD hidden, or no HUD actor):
	// its panels hide rather than hold their last frame, and come back with
	// the next snapshot.
	if (!bSnapshotSinceTick)
	{
		bHUDWanted = false;
		bReadingWanted = false;
		ReadingPage = 0;
		ReadingPageCount = 1;
	}
	bSnapshotSinceTick = false;
	IWoTVRSession* Session = WoTVRPresentationSession();
	IStereoLayers* Layers = Session ? Session->GetStereoLayers() : nullptr;
	const float Scale = Session ? Session->GetWorldToMetersScale() : static_cast<float>(WoTVR::UnitsPerMeter);

	// The HUD panel, soft-attached below the forward view.
	const double Distance = WoTVRTuning::HUDDistanceMeters();
	const double Width = 2.0 * Distance * FMath::Tan(FMath::DegreesToRadians(WoTVRTuning::HUDWidthDegrees() * 0.5));
	const FWoTVRPanelPose HUDPose = FWoTVRPanelPose::Facing(LastHead.PositionMeters, HUDYaw,
		WoTVRTuning::HUDDropDegrees(), Distance, Width, HUD->GetPixels());
	HUDPanel.Update(Layers, HUD->GetTarget(), HUDPose, Scale, bHUDWanted);

	// Passive cards beside it, on the right.
	FWoTVRPanelPose CardPose = HUDPose;
	const double CardWidth = WoTVRTuning::CardWidthMeters();
	CardPose.Size = FVector2D(CardWidth, CardWidth * Cards->GetPixels().Y / FMath::Max(Cards->GetPixels().X, 1));
	CardPose.Center = HUDPose.Center + HUDPose.Orientation.GetRightVector()
		* (HUDPose.Size.X * 0.5 + WoTVRTuning::CardGapMeters() + CardWidth * 0.5);
	const bool bCards = Cards->Num() > 0;
	if (bCards)
	{
		Cards->Paint(DeltaSeconds);
	}
	CardPanel.Update(Layers, Cards->GetTarget(), CardPose, Scale, bCards);

	// The board, world-locked where the view was when it began to show.
	if (bReadingWanted && bPlaceReading)
	{
		ReadingPose = PlaceInFront(LastHead, WoTVRTuning::ReadingDistanceMeters(),
			WoTVRTuning::ReadingWidthMeters(), Reading->GetPixels());
		bPlaceReading = false;
	}
	ReadingPanel.Update(Layers, Reading->GetTarget(), ReadingPose, Scale, bReadingWanted);

	// The menus, world-locked where the view was when the first one opened.
	const bool bMenus = Menu->Num() > 0;
	if (bMenus && bPlaceMenu)
	{
		MenuPose = PlaceInFront(LastHead, WoTVRTuning::MenuDistanceMeters(), WoTVRTuning::MenuWidthMeters(),
			Menu->GetPixels());
		bPlaceMenu = false;
	}
	MenuPanel.Update(Layers, Menu->GetTarget(), MenuPose, Scale, bMenus);
	// The pointer moves over what was painted last, then the surface paints
	// with this frame's hover and reticle.
	UpdatePointer();
	if (bMenus)
	{
		Menu->Paint(DeltaSeconds);
	}
}

void FWoTVRPresentation::Recentre(const FWoTVRHeadPose& Head)
{
	if (!bAttached || !Head.bValid)
	{
		return;
	}
	LastHead = Head;
	HUDYaw = FWoTVRRig::ForwardYaw(Head.Orientation, HUDYaw);
	bHUDFollowing = false;
	if (Menu && Menu->Num() > 0)
	{
		MenuPose = PlaceInFront(Head, WoTVRTuning::MenuDistanceMeters(), WoTVRTuning::MenuWidthMeters(),
			Menu->GetPixels());
		LastRoutedMove.Reset();
	}
	if (bReadingWanted && Reading)
	{
		ReadingPose = PlaceInFront(Head, WoTVRTuning::ReadingDistanceMeters(),
			WoTVRTuning::ReadingWidthMeters(), Reading->GetPixels());
	}
}

// --- the pointer -------------------------------------------------------------

void FWoTVRPresentation::UpdatePointer()
{
	bPointerOnMenu = false;
	if (Menu && Menu->Num() > 0 && RaySource.IsValid())
	{
		FVector Origin = FVector::ZeroVector;
		FVector Direction = FVector::ForwardVector;
		FVector2D Pixel = FVector2D::ZeroVector;
		bool bInside = false;
		if (RaySource->GetRay(Origin, Direction)
			&& MenuPose.Intersect(Origin, Direction.GetSafeNormal(), Menu->GetPixels(), Pixel, bInside)
			&& bInside)
		{
			PointerPixel = Pixel;
			bPointerOnMenu = true;
		}
	}
	if (Menu)
	{
		Menu->SetReticle(bPointerOnMenu ? TOptional<FVector2D>(PointerPixel) : TOptional<FVector2D>());
	}
	if (bPointerOnMenu && (!LastRoutedMove.IsSet() || FVector2D::Distance(*LastRoutedMove, PointerPixel) > 0.5))
	{
		RoutePointer(EWoTUIPointerAction::Move);
	}
	else if (!bPointerOnMenu && bPointerHovering && !IsPointerPressHeld())
	{
		// The gaze went off the surface (or its last menu closed): the control
		// it was on is no longer under it. A held press keeps its control
		// hovered until the press ends (ReleaseRoute).
		LeavePanel();
	}
	UpdateFocusPreference();
}

void FWoTVRPresentation::LeavePanel()
{
	bPointerHovering = false;
	// Back on the surface the pointer moves again, even onto the same pixel.
	LastRoutedMove.Reset();
	RoutePointer(EWoTUIPointerAction::Leave);
}

TSharedPtr<SWidget> FWoTVRPresentation::ControlUnderPointer() const
{
	const ULocalPlayer* LocalPlayer = Player.Get();
	if (!bPointerOnMenu || !Menu || !LocalPlayer)
	{
		return nullptr;
	}
	// The hit test the pointer's own events use (the last paint's).
	const TArray<FWidgetAndPointer> Bubble = Menu->GetWindow()->GetHittestGrid().GetBubblePath(
		PointerPixel, 0.0f, /*bIgnoreEnabledStatus*/ false, FWoTLocalPlayerHooks::UISlateUser(*LocalPlayer));
	for (int32 Index = Bubble.Num() - 1; Index >= 0; --Index)
	{
		const TSharedRef<SWidget>& Widget = Bubble[Index].Widget;
		if (!Widget->Advanced_IsWindow() && (Widget->IsInteractable() || Widget->SupportsKeyboardFocus()))
		{
			return Widget;
		}
	}
	return nullptr;
}

void FWoTVRPresentation::NoteNavigation()
{
	bPreferFocus = true;
	NavigationControl = ControlUnderPointer();
	FVector Origin = FVector::ZeroVector;
	FVector Direction = FVector::ForwardVector;
	NavigationGaze.Reset();
	if (RaySource.IsValid() && RaySource->GetRay(Origin, Direction))
	{
		NavigationGaze = Direction.GetSafeNormal();
	}
}

void FWoTVRPresentation::UpdateFocusPreference()
{
	if (!bPreferFocus)
	{
		return;
	}
	// Over no control (a label, the background, off the panel), the gaze has
	// not chosen anything: reading the panel keeps the focused control.
	const TSharedPtr<SWidget> Control = ControlUnderPointer();
	if (!Control.IsValid())
	{
		return;
	}
	// The control the gaze was on, or one holding it (its row, its screen):
	// still where the player navigated from.
	const TSharedPtr<SWidget> Recorded = NavigationControl.Pin();
	for (TSharedPtr<SWidget> Widget = Recorded; Widget.IsValid(); Widget = Widget->GetParentWidget())
	{
		if (Widget == Control)
		{
			return;
		}
	}
	// A screen that changed under a still gaze is not the gaze moving.
	FVector Origin = FVector::ZeroVector;
	FVector Direction = FVector::ForwardVector;
	if (NavigationGaze.IsSet() && RaySource.IsValid() && RaySource->GetRay(Origin, Direction)
		&& FVector::DotProduct(Direction.GetSafeNormal(), *NavigationGaze)
			>= FMath::Cos(FMath::DegreesToRadians(WoTVRTuning::GazeMoveDegrees())))
	{
		return;
	}
	// The gaze moved onto another control: confirm presses at the reticle again.
	bPreferFocus = false;
	NavigationControl.Reset();
	NavigationGaze.Reset();
}

bool FWoTVRPresentation::IsPointerPressHeld() const
{
	for (const TPair<FKey, ERoute>& Entry : Held)
	{
		if (Entry.Value == ERoute::Pointer)
		{
			return true;
		}
	}
	return false;
}

bool FWoTVRPresentation::RoutePointer(EWoTUIPointerAction Action)
{
	ULocalPlayer* LocalPlayer = Player.Get();
	if (!LocalPlayer)
	{
		return false;
	}
	// On the surface, where the ray meets it. Off it (a press released after
	// the gaze strayed), at a point outside the window: the hit test finds
	// nothing there, so the release ends the press without reaching any
	// control (never a click, even on one at the surface's edge), and a
	// dragged slider keeps the value it had.
	const FVector2D Position = bPointerOnMenu ? PointerPixel : FVector2D(-1.0e6, -1.0e6);
	TGuardValue<bool> Routing(bRouting, true);
	const bool bRouted = FWoTLocalPlayerHooks::RoutePointer(*LocalPlayer, Owner, Position, Action);
	if (bRouted && Action == EWoTUIPointerAction::Move)
	{
		LastRoutedMove = Position;
		bPointerHovering = true;
	}
	return bRouted;
}

bool FWoTVRPresentation::RouteKey(const FKey& Key, bool bPressed, bool bRepeat)
{
	ULocalPlayer* LocalPlayer = Player.Get();
	if (!LocalPlayer)
	{
		return false;
	}
	TGuardValue<bool> Routing(bRouting, true);
	return FWoTLocalPlayerHooks::RouteKey(*LocalPlayer, Owner, Key, bPressed, bRepeat);
}

bool FWoTVRPresentation::IsMenuInputActive() const
{
	const ULocalPlayer* LocalPlayer = Player.Get();
	if (!bAttached || !Menu || Menu->Num() == 0 || !LocalPlayer)
	{
		return false;
	}
	// The same gates as the game's own windows: a window holds UI-only input
	// (the front end, the in-game menu, help, item information, chat), or a
	// text entry runs. A passive card holds neither, so play carries on.
	return FWoTLocalPlayerHooks::IsGameInputBlocked(LocalPlayer->PlayerController)
		|| FWoTLocalPlayerHooks::IsTextEntryActive();
}

void FWoTVRPresentation::ReleaseRoute(const FKey& Key, ERoute Route)
{
	switch (Route)
	{
	case ERoute::Pointer:
		RoutePointer(EWoTUIPointerAction::Up);
		if (!bPointerOnMenu)
		{
			// Released off the surface: the press is over, and the control it
			// held is no longer under the gaze.
			LeavePanel();
		}
		break;
	case ERoute::Key:
		RouteKey(Key, false, false);
		break;
	case ERoute::Dropped:
	default:
		break;
	}
}

bool FWoTVRPresentation::IsPagingKey(const FKey& Key, int32& OutDirection) const
{
	// The bumpers and whatever the player binds to the item selection.
	if (Key == EKeys::Gamepad_LeftShoulder)
	{
		OutDirection = -1;
		return true;
	}
	if (Key == EKeys::Gamepad_RightShoulder)
	{
		OutDirection = 1;
		return true;
	}
	const ULocalPlayer* LocalPlayer = Player.Get();
	const APlayerController* Controller = LocalPlayer ? LocalPlayer->PlayerController.Get() : nullptr;
	const UPlayerInput* Input = Controller ? Controller->PlayerInput.Get() : nullptr;
	if (!Input)
	{
		return false;
	}
	for (const FInputActionKeyMapping& Mapping : Input->ActionMappings)
	{
		if (Mapping.Key == Key && (Mapping.ActionName == FName(TEXT("NextItem"))
			|| Mapping.ActionName == FName(TEXT("PrevItem"))))
		{
			OutDirection = Mapping.ActionName == FName(TEXT("NextItem")) ? 1 : -1;
			return true;
		}
	}
	return false;
}

void FWoTVRPresentation::PageBy(int32 Direction)
{
	// Shown from the next update; the layout clamps to the pages there are.
	ReadingPage = FMath::Clamp(ReadingPage + Direction, 0, FMath::Max(ReadingPageCount - 1, 0));
}

// --- input -------------------------------------------------------------------

bool FWoTVRPresentation::HandleKey(const FKeyEvent& Event, bool bPressed)
{
	if (bRouting || !bAttached)
	{
		return false;
	}
	const FKey Key = Event.GetKey();
	if (!bPressed)
	{
		// A release follows its press wherever the press went.
		ERoute Route = ERoute::Dropped;
		if (!Held.RemoveAndCopyValue(Key, Route))
		{
			return false;
		}
		ReleaseRoute(Key, Route);
		return true;
	}
	if (Held.Contains(Key))
	{
		// A held key's repeats go where its press went.
		if (const ERoute* Route = Held.Find(Key); Route && *Route == ERoute::Key)
		{
			RouteKey(Key, true, true);
		}
		return true;
	}
	// The board's pages turn with the bumpers and the selection keys while it
	// shows more than one.
	int32 Direction = 0;
	if (bReadingWanted && ReadingPageCount > 1 && IsPagingKey(Key, Direction))
	{
		PageBy(Direction);
		Held.Add(Key, ERoute::Dropped);
		return true;
	}
	if (!IsMenuInputActive())
	{
		return false;
	}
	ULocalPlayer* LocalPlayer = Player.Get();
	if (WoTVRIsConfirmKey(Key))
	{
		if (Key == EKeys::Enter && (FWoTLocalPlayerHooks::IsTextEntryActive() || (LocalPlayer
			&& WoTVRFocusEditsText(FWoTLocalPlayerHooks::UISlateUser(*LocalPlayer)))))
		{
			// Typing: Enter is the field's, and on the keyboard panel it
			// finishes the entry, wherever the gaze is.
			return false;
		}
		if (IsPointerPressHeld())
		{
			// The pointer is already pressed (by another confirm, or the
			// desktop mouse): this press neither joins nor ends it.
			Held.Add(Key, ERoute::Dropped);
			return true;
		}
		if (bPointerOnMenu && !bPreferFocus)
		{
			// Confirm presses where the pointer is.
			RoutePointer(EWoTUIPointerAction::Down);
			Held.Add(Key, ERoute::Pointer);
			return true;
		}
		// Off the panel, or after D-pad or stick navigation until the gaze
		// moves onto another control: the focused control.
		if (RouteKey(Key, true, Event.IsRepeat()))
		{
			Held.Add(Key, ERoute::Key);
			return true;
		}
		return false;
	}
	if (WoTVRIsNavigationKey(Key) && RouteKey(Key, true, Event.IsRepeat()))
	{
		Held.Add(Key, ERoute::Key);
		if (WoTVRIsDirectionKey(Key))
		{
			// Focus moved by D-pad or stick: confirm follows it.
			NoteNavigation();
		}
		return true;
	}
	return false;
}

bool FWoTVRPresentation::HandleAnalog(const FAnalogInputEvent& Event)
{
	if (bRouting || !bAttached || !WoTVRIsNavigationAxis(Event.GetKey()) || !IsMenuInputActive())
	{
		return false;
	}
	ULocalPlayer* LocalPlayer = Player.Get();
	if (!LocalPlayer)
	{
		return false;
	}
	bool bRouted = false;
	{
		TGuardValue<bool> Routing(bRouting, true);
		bRouted = FWoTLocalPlayerHooks::RouteAnalog(*LocalPlayer, Owner, Event.GetKey(), Event.GetAnalogValue());
	}
	if (bRouted && FSlateApplication::IsInitialized())
	{
		// Past Slate's navigation threshold the stick moves focus: confirm
		// follows it.
		const TSharedRef<FNavigationConfig> Config = FSlateApplication::Get().GetNavigationConfig();
		const float Threshold = Event.GetKey() == EKeys::Gamepad_LeftX
			? Config->AnalogNavigationHorizontalThreshold : Config->AnalogNavigationVerticalThreshold;
		if (FMath::Abs(Event.GetAnalogValue()) > Threshold)
		{
			NoteNavigation();
		}
	}
	return bRouted;
}

bool FWoTVRPresentation::HandleMouseButton(const FPointerEvent& Event, bool bPressed, bool bDoubleClick)
{
	if (bRouting || !bAttached)
	{
		return false;
	}
	const FKey Button = Event.GetEffectingButton();
	if (!bPressed)
	{
		ERoute Route = ERoute::Dropped;
		if (!Held.RemoveAndCopyValue(Button, Route))
		{
			return false;
		}
		ReleaseRoute(Button, Route);
		return true;
	}
	if (!IsMenuInputActive())
	{
		return false;
	}
	if (Button == EKeys::LeftMouseButton && bPointerOnMenu && !IsPointerPressHeld())
	{
		RoutePointer(bDoubleClick ? EWoTUIPointerAction::DoubleClick : EWoTUIPointerAction::Down);
		Held.Add(Button, ERoute::Pointer);
		return true;
	}
	// With a menu up in the headset, a click on the desktop window must not
	// take the player's focus from it.
	Held.Add(Button, ERoute::Dropped);
	return true;
}

bool FWoTVRPresentation::HandleWheel(const FPointerEvent& Event)
{
	if (bRouting || !bAttached || !bReadingWanted || ReadingPageCount <= 1)
	{
		return false;
	}
	// The wheel selects items in play (MouseScrollUp is the previous one): it
	// turns the board's pages while the board shows more than one.
	const float Delta = Event.GetWheelDelta();
	if (FMath::IsNearlyZero(Delta))
	{
		return false;
	}
	PageBy(Delta > 0.0f ? -1 : 1);
	return true;
}

// --- diagnostics ---------------------------------------------------------------

FString FWoTVRPresentation::Describe() const
{
	if (!bAttached)
	{
		return TEXT("headset HUD and menus detached");
	}
	return FString::Printf(
		TEXT("headset HUD %s (%d updates), board %s (page %d of %d), menus %d on %s, cards %d, pointer (%s) %s"),
		HUDPanel.IsShown() ? TEXT("shown") : TEXT("hidden"), HUDUpdates,
		ReadingPanel.IsShown() ? TEXT("shown") : TEXT("hidden"), ReadingPage + 1, ReadingPageCount,
		Menu ? Menu->Num() : 0, MenuPanel.IsShown() ? TEXT("a world-locked panel") : TEXT("no panel"),
		Cards ? Cards->Num() : 0, RaySource ? RaySource->GetName() : TEXT("none"),
		bPointerOnMenu ? *FString::Printf(TEXT("at %.0f,%.0f"), PointerPixel.X, PointerPixel.Y) : TEXT("off the menu"));
}

void FWoTVRPresentation::AddReferencedObjects(FReferenceCollector& Collector)
{
	if (HUD)
	{
		HUD->AddReferencedObjects(Collector);
	}
	if (Reading)
	{
		Reading->AddReferencedObjects(Collector);
	}
	if (Menu)
	{
		Menu->AddReferencedObjects(Collector);
	}
	if (Cards)
	{
		Cards->AddReferencedObjects(Collector);
	}
}
