#include "WoTLocalPlayerHooks.h"

#include "WoTCharacter.h"
#include "WoTFrontEndPlayerController.h"
#include "WoTGameViewportClient.h"
#include "WoTHUD.h"
#include "WoTPlayerController.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTHandInfo.h"
#include "Inventory/WoTHandSet.h"
#include "UI/WoTControllerUIInput.h"
#include "UI/WoTTextEntry.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Input/Events.h"
#include "Input/HittestGrid.h"
#include "Layout/WidgetPath.h"
#include "Widgets/SViewport.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTLocalPlayerHooks, Log, All);

namespace
{
struct FWoTHookConsumerState
{
	FWoTLocalPlayerConsumer Callbacks;
	bool bActive = true;
	bool bFireHeld = false;
	bool bJumpHeld = false;
	bool bScoresChanged = false;
	/** The movement owner holds the body still (HoldMovement). */
	bool bMovementHeld = false;
	/** While this consumer supplies the aim: the yaw changes gameplay made to it
	 * (PeekGameplayYaw), and the control yaw the hooks last left. */
	double GameplayYaw = 0.0;
	double AimYaw = 0.0;
	bool bHasAimYaw = false;
	TWeakObjectPtr<AWoTPlayerController> CommandController;
	/** H6, while this consumer hosts UI: the hosted surface's place in the
	 * controller UI input scope, and the screens it took (with their z-orders,
	 * in the order taken) from the viewport they return to. */
	TSharedPtr<FWoTControllerUIInputRegistration> UIScope;
	TArray<TPair<TSharedPtr<SWidget>, int32>> Hosted;
	TWeakObjectPtr<UGameViewportClient> HostViewport;
	/** H6: where the host's pointer last was, and whether its button is down. */
	FVector2D PointerLast = FVector2D::ZeroVector;
	bool bPointerDown = false;
	/** H6: the keys the host's routes left pressed and the sticks they left
	 * deflected, released when the host detaches. */
	TSet<FKey> RoutedKeys;
	TSet<FKey> RoutedAxes;
};

struct FWoTHookPlayerState
{
	FWoTLocalPlayerContext Context;
	TMap<FName, TSharedPtr<FWoTHookConsumerState>> Consumers;
	/** The pawn a relocation over the teleport distance moved since the last
	 * reconcile. */
	TWeakObjectPtr<APawn> RelocatedPawn;
	/** Where the possessed pawn was when last seen (each reconcile, relocation
	 * and correction): the start of the next relocation's jump. */
	TWeakObjectPtr<APawn> AnchorPawn;
	FVector AnchorLocation = FVector::ZeroVector;
	/** Travel was already announced for the current controller and world. */
	bool bTravelNotified = false;
	/** Inside the controller's rotation update, where the yaw is the hooks' own. */
	int32 AimUpdateDepth = 0;
};

// Weak keys: a registration never keeps a local player alive.
TMap<TWeakObjectPtr<ULocalPlayer>, TSharedPtr<FWoTHookPlayerState>> GWoTLocalPlayerHooks;

TSharedPtr<FWoTHookPlayerState> WoTHooksFind(const ULocalPlayer* Player)
{
	if (!Player || GWoTLocalPlayerHooks.IsEmpty())
	{
		return nullptr;
	}
	return GWoTLocalPlayerHooks.FindRef(const_cast<ULocalPlayer*>(Player));
}

/** The registration of the local player whose current controller this is. A
 * replaced controller that still ticks never speaks for the player. */
TSharedPtr<FWoTHookPlayerState> WoTHooksFind(const APlayerController* Controller)
{
	if (!Controller || GWoTLocalPlayerHooks.IsEmpty())
	{
		return nullptr;
	}
	const ULocalPlayer* Player = Controller->GetLocalPlayer();
	return Player && Player->PlayerController == Controller ? WoTHooksFind(Player) : nullptr;
}

/** A snapshot, so callbacks may attach or detach while it is walked. */
TArray<TSharedPtr<FWoTHookConsumerState>> WoTHooksConsumers(
	const TSharedPtr<FWoTHookPlayerState>& Hooks)
{
	TArray<TSharedPtr<FWoTHookConsumerState>> Result;
	if (Hooks)
	{
		Hooks->Consumers.GenerateValueArray(Result);
	}
	return Result;
}

TSharedPtr<FWoTHookConsumerState> WoTHooksAimOwner(const TSharedPtr<FWoTHookPlayerState>& Hooks)
{
	if (Hooks)
	{
		for (const TPair<FName, TSharedPtr<FWoTHookConsumerState>>& Pair : Hooks->Consumers)
		{
			if (Pair.Value->bActive && Pair.Value->Callbacks.AimRotation)
			{
				return Pair.Value;
			}
		}
	}
	return nullptr;
}

TSharedPtr<FWoTHookConsumerState> WoTHooksMovementOwner(const TSharedPtr<FWoTHookPlayerState>& Hooks)
{
	if (Hooks)
	{
		for (const TPair<FName, TSharedPtr<FWoTHookConsumerState>>& Pair : Hooks->Consumers)
		{
			const FWoTLocalPlayerConsumer& Callbacks = Pair.Value->Callbacks;
			if (Pair.Value->bActive && (Callbacks.GroundMovementYaw || Callbacks.ExtraMovement))
			{
				return Pair.Value;
			}
		}
	}
	return nullptr;
}

TSharedPtr<FWoTHookConsumerState> WoTHooksViewOwner(const TSharedPtr<FWoTHookPlayerState>& Hooks)
{
	if (Hooks)
	{
		for (const TPair<FName, TSharedPtr<FWoTHookConsumerState>>& Pair : Hooks->Consumers)
		{
			if (Pair.Value->bActive && Pair.Value->Callbacks.View.IsSet())
			{
				return Pair.Value;
			}
		}
	}
	return nullptr;
}

TSharedPtr<FWoTHookConsumerState> WoTHooksHUDDestinationOwner(
	const TSharedPtr<FWoTHookPlayerState>& Hooks)
{
	if (Hooks)
	{
		for (const TPair<FName, TSharedPtr<FWoTHookConsumerState>>& Pair : Hooks->Consumers)
		{
			if (Pair.Value->bActive && Pair.Value->Callbacks.HUDDestination)
			{
				return Pair.Value;
			}
		}
	}
	return nullptr;
}

TSharedPtr<FWoTHookConsumerState> WoTHooksUIHostOwner(const TSharedPtr<FWoTHookPlayerState>& Hooks)
{
	if (Hooks)
	{
		for (const TPair<FName, TSharedPtr<FWoTHookConsumerState>>& Pair : Hooks->Consumers)
		{
			if (Pair.Value->bActive && Pair.Value->Callbacks.UIHost.IsSet())
			{
				return Pair.Value;
			}
		}
	}
	return nullptr;
}

/** The Slate user of the host pointer event being routed, if any. */
int32 GWoTRoutingUIPointerUser = INDEX_NONE;

bool WoTHooksIsInside(TSharedPtr<SWidget> Widget, const SWidget* Ancestor)
{
	for (; Widget.IsValid() && Ancestor; Widget = Widget->GetParentWidget())
	{
		if (Widget.Get() == Ancestor)
		{
			return true;
		}
	}
	return false;
}

TSharedPtr<SWidget> WoTHooksUserFocus(const ULocalPlayer& Player)
{
	return FSlateApplication::IsInitialized()
		? FSlateApplication::Get().GetUserFocusedWidget(FWoTLocalPlayerHooks::UISlateUser(Player))
		: nullptr;
}

/** Focus inside a screen that moved between the viewport and a host keeps
 * its widget: the user's focus path is generated again where it now lives. */
void WoTHooksRefocus(const ULocalPlayer& Player, const TSharedPtr<SWidget>& Focused)
{
	if (Focused.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetUserFocus(FWoTLocalPlayerHooks::UISlateUser(Player),
			Focused, EFocusCause::SetDirectly);
	}
}

/** Offers Screen to Consumer's host: true when it took it. */
bool WoTHooksShowHosted(FWoTHookConsumerState& Consumer, UGameViewportClient& Viewport,
	const TSharedRef<SWidget>& Screen, int32 ZOrder)
{
	const FWoTUIHost& Host = Consumer.Callbacks.UIHost.GetValue();
	if (!Consumer.bActive || !Host.Show
		|| !Host.Show(Screen, ZOrder, FWoTLocalPlayerHooks::ClassifyUIContent(*Screen)))
	{
		return false;
	}
	if (!Consumer.bActive)
	{
		// Show detached this host (re-entrantly): it has already let go of the
		// screens it held, so it must not keep this one either. The caller puts
		// the screen on the viewport, or offers it to whoever hosts now.
		if (Host.Hide)
		{
			Host.Hide(Screen);
		}
		return false;
	}
	Consumer.Hosted.Emplace(Screen, ZOrder);
	Consumer.HostViewport = &Viewport;
	return true;
}

/** The host's pointer lets go of whatever it captured (a pressed button, a
 * dragged slider) without coming up over anything, so letting go is never a
 * click. The desktop mouse's own capture, on its own pointer index, stays. */
void WoTHooksReleasePointerCapture(FSlateApplication& Slate, int32 User)
{
	if (const TSharedPtr<FSlateUser> SlateUser = Slate.GetUser(User))
	{
		SlateUser->ReleaseCapture(FWoTLocalPlayerHooks::UIPointerIndex);
	}
}

/** The host's pointer leaves every widget it hovered. Slate clears a widget's
 * hover only on a move of the same pointer that no longer finds it
 * (FSlateApplication::RoutePointerMoveEvent diffs the widgets last under that
 * pointer index): this is that move, synthetic, to a point outside every
 * window with nothing under it, so the widgets hear their leave and none hears
 * a move. The desktop mouse's own hover, on its own pointer index, stays. */
void WoTHooksSendPointerLeave(int32 User)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const FVector2D Away(-1.0e6, -1.0e6);
	const FPointerEvent Event(User, FWoTLocalPlayerHooks::UIPointerIndex, Away, Away,
		TSet<FKey>(), EKeys::Invalid, 0.0f, Slate.GetModifierKeys());
	TGuardValue<int32> Routing(GWoTRoutingUIPointerUser, User);
	Slate.RoutePointerMoveEvent(FWidgetPath(), Event, /*bIsSynthetic*/ true);
}

/** The host's pointer event at Position in its window, routed as the
 * player's Slate user to the widgets the window's hit test finds there (or to
 * the widget holding the pointer's capture). A release where nothing is hit
 * still ends the press. Leave unhovers, unless the press is held. False when
 * the host has no window, for a Leave while the press is held, or for any
 * other action when nothing is under Position. */
bool WoTHooksRoutePointer(const ULocalPlayer& Player, FWoTHookConsumerState& Consumer,
	const FVector2D& Position, EWoTUIPointerAction Action)
{
	if (!FSlateApplication::IsInitialized() || !Consumer.Callbacks.UIHost.IsSet())
	{
		return false;
	}
	// Only the host's own window is hit, the way Slate hits any window: the
	// pointer cannot reach the game viewport or any other window, so it has
	// no world-use or collision authority.
	const FWoTUIHost& Host = Consumer.Callbacks.UIHost.GetValue();
	const TSharedPtr<SWidget> Scope = Host.Scope ? Host.Scope() : nullptr;
	if (!Scope.IsValid() || !Scope->Advanced_IsWindow())
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const int32 User = FWoTLocalPlayerHooks::UISlateUser(Player);
	if (Action == EWoTUIPointerAction::Leave)
	{
		// Refused while the press is held: the move would reach the widget
		// holding the capture (a captured slider would jump to the far point),
		// and a pressed button keeps its hover until the press ends.
		if (Consumer.bPointerDown)
		{
			return false;
		}
		WoTHooksSendPointerLeave(User);
		return true;
	}
	TArray<FWidgetAndPointer> Bubble = StaticCastSharedPtr<SWindow>(Scope)->GetHittestGrid()
		.GetBubblePath(Position, 0.0f, /*bIgnoreEnabledStatus*/ false, User);
	if (Bubble.IsEmpty())
	{
		if (Action != EWoTUIPointerAction::Up)
		{
			return false;
		}
		// Released where nothing is hit: no widget hears it, and the press
		// ends all the same, as one that ends off a button does. Nothing stays
		// captured or pressed, and the next move carries no held button.
		Consumer.bPointerDown = false;
		Consumer.PointerLast = Position;
		WoTHooksReleasePointerCapture(Slate, User);
		return true;
	}
	const FWidgetPath Path(Bubble);
	TSet<FKey> Pressed;
	FKey Effecting = EKeys::LeftMouseButton;
	switch (Action)
	{
	case EWoTUIPointerAction::Down:
	case EWoTUIPointerAction::DoubleClick:
		Consumer.bPointerDown = true;
		Pressed.Add(EKeys::LeftMouseButton);
		break;
	case EWoTUIPointerAction::Up:
		Consumer.bPointerDown = false;
		break;
	case EWoTUIPointerAction::Move:
		Effecting = EKeys::Invalid;
		if (Consumer.bPointerDown)
		{
			Pressed.Add(EKeys::LeftMouseButton);
		}
		break;
	case EWoTUIPointerAction::Leave:
		// Sent above.
		return false;
	}
	// Its own pointer index: the desktop mouse's moves and presses never reach
	// this pointer's hover, capture or press, nor this pointer the mouse's.
	const FPointerEvent Event(User, FWoTLocalPlayerHooks::UIPointerIndex, Position,
		Consumer.PointerLast, Pressed, Effecting, 0.0f, Slate.GetModifierKeys());
	Consumer.PointerLast = Position;
	TGuardValue<int32> Routing(GWoTRoutingUIPointerUser, User);
	switch (Action)
	{
	case EWoTUIPointerAction::Move:
		Slate.RoutePointerMoveEvent(Path, Event, /*bIsSynthetic*/ false);
		break;
	case EWoTUIPointerAction::Down:
		Slate.RoutePointerDownEvent(Path, Event);
		break;
	case EWoTUIPointerAction::Up:
		Slate.RoutePointerUpEvent(Path, Event);
		break;
	case EWoTUIPointerAction::DoubleClick:
		Slate.RoutePointerDoubleClickEvent(Path, Event);
		break;
	case EWoTUIPointerAction::Leave:
		break;
	}
	return true;
}

/** Everything the host's routes still hold is let go, as the player's Slate
 * user, while its screens are still in its window, so nothing stays held once
 * they are back on the viewport (the controller UI navigation repeat, a
 * pressed button, a captured slider): a pressed pointer gives up its capture
 * without coming up over anything, as a press that ends elsewhere does, so
 * releasing it is never a click, and then leaves every widget it hovered;
 * each pressed key is released through Slate's own route, as a key lifted
 * after the switch would be; and each deflected stick returns to rest. */
void WoTHooksSendRelease(int32 User, const TSet<FKey>& Keys, const TSet<FKey>& Axes)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	// The pointer gave up its capture already: nothing hears this move but the
	// widgets it hovered, which hear their leave.
	WoTHooksSendPointerLeave(User);
	for (const FKey& Key : Keys)
	{
		Slate.ProcessKeyUpEvent(FKeyEvent(Key, Slate.GetModifierKeys(), User, false, 0, 0));
	}
	for (const FKey& Axis : Axes)
	{
		Slate.ProcessAnalogInputEvent(FAnalogInputEvent(Axis, Slate.GetModifierKeys(), User,
			/*bIsRepeat*/ false, 0, 0, 0.0f));
	}
}

void WoTHooksReleaseRouted(const ULocalPlayer* Player, FWoTHookConsumerState& Consumer)
{
	const TSet<FKey> Keys = MoveTemp(Consumer.RoutedKeys);
	const TSet<FKey> Axes = MoveTemp(Consumer.RoutedAxes);
	Consumer.RoutedKeys.Reset();
	Consumer.RoutedAxes.Reset();
	Consumer.bPointerDown = false;
	if (!Player || !FSlateApplication::IsInitialized())
	{
		return;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const int32 User = FWoTLocalPlayerHooks::UISlateUser(*Player);
	// Only the host's pointer: the desktop mouse's capture is its own.
	WoTHooksReleasePointerCapture(Slate, User);
	// The pointer's leave always follows, with or without keys to release.
	if (!Slate.IsProcessingInput())
	{
		WoTHooksSendRelease(User, Keys, Axes);
		return;
	}
	// Detached from inside Slate's own input handling (a key handler, a
	// shortcut's input processor, a pointer event): the leave and the releases
	// follow once this frame's input is done, never nested inside it, where
	// Slate may be adding or removing input processors or routing this
	// pointer's own event.
	const TSharedRef<FDelegateHandle> Handle = MakeShared<FDelegateHandle>();
	*Handle = Slate.OnPostTick().AddLambda([User, Keys, Axes, Handle](float)
	{
		// Removing the binding destroys this lambda: what it holds is copied
		// out first.
		const int32 ReleaseUser = User;
		const TSet<FKey> ReleaseKeys = Keys;
		const TSet<FKey> ReleaseAxes = Axes;
		const FDelegateHandle Binding = *Handle;
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().OnPostTick().Remove(Binding);
			WoTHooksSendRelease(ReleaseUser, ReleaseKeys, ReleaseAxes);
		}
	});
}

/** A UI host attached: its surface joins the controller UI input scope and
 * takes the screens already on the viewport, bottom first. */
void WoTHooksBeginHosting(ULocalPlayer& Player, FWoTHookConsumerState& Consumer)
{
	const FWoTUIHost& Host = Consumer.Callbacks.UIHost.GetValue();
	if (Host.Scope)
	{
		Consumer.UIScope = FWoTControllerUIInput::Register(Host.Scope);
	}
	UWoTGameViewportClient* Viewport = Cast<UWoTGameViewportClient>(Player.ViewportClient);
	if (!Viewport)
	{
		return;
	}
	const TSharedPtr<SWidget> Focused = WoTHooksUserFocus(Player);
	bool bFocusMoved = false;
	for (const TPair<TSharedRef<SWidget>, int32>& Screen : Viewport->GetOverlayContent())
	{
		if (!Consumer.bActive)
		{
			break;
		}
		Viewport->RemoveOverlayContent(Screen.Key);
		if (WoTHooksShowHosted(Consumer, *Viewport, Screen.Key, Screen.Value))
		{
			bFocusMoved |= WoTHooksIsInside(Focused, &Screen.Key.Get());
		}
		else
		{
			Viewport->AddOverlayContent(Screen.Key, Screen.Value);
		}
	}
	if (bFocusMoved)
	{
		WoTHooksRefocus(Player, Focused);
	}
}

/** A UI host detaching: every screen it took goes back to the viewport at its
 * z-order, and its surface leaves the controller UI input scope. */
void WoTHooksEndHosting(const ULocalPlayer* Player, FWoTHookConsumerState& Consumer)
{
	WoTHooksReleaseRouted(Player, Consumer);
	const TArray<TPair<TSharedPtr<SWidget>, int32>> Hosted = MoveTemp(Consumer.Hosted);
	Consumer.Hosted.Reset();
	UWoTGameViewportClient* Viewport = Cast<UWoTGameViewportClient>(Consumer.HostViewport.Get());
	const TSharedPtr<SWidget> Focused = Player ? WoTHooksUserFocus(*Player) : nullptr;
	bool bFocusMoved = false;
	for (const TPair<TSharedPtr<SWidget>, int32>& Screen : Hosted)
	{
		if (!Screen.Key.IsValid())
		{
			continue;
		}
		const FWoTUIHost& Host = Consumer.Callbacks.UIHost.GetValue();
		if (Host.Hide)
		{
			Host.Hide(Screen.Key.ToSharedRef());
		}
		if (Viewport)
		{
			Viewport->AddOverlayContent(Screen.Key.ToSharedRef(), Screen.Value);
			bFocusMoved |= WoTHooksIsInside(Focused, Screen.Key.Get());
		}
	}
	Consumer.HostViewport.Reset();
	Consumer.UIScope.Reset();
	if (bFocusMoved && Player)
	{
		WoTHooksRefocus(*Player, Focused);
	}
}

/** Owner's consumer when it hosts UI for Player. */
TSharedPtr<FWoTHookConsumerState> WoTHooksHostOf(ULocalPlayer& Player, FName Owner)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Player);
	const TSharedPtr<FWoTHookConsumerState> Consumer =
		Hooks ? Hooks->Consumers.FindRef(Owner) : nullptr;
	return Consumer && Consumer->bActive && Consumer->Callbacks.UIHost.IsSet()
		? Consumer : nullptr;
}

/** Whether Player's Slate user has focus inside Consumer's hosted scope. */
bool WoTHooksFocusInScope(const ULocalPlayer& Player, const FWoTHookConsumerState& Consumer)
{
	const FWoTUIHost& Host = Consumer.Callbacks.UIHost.GetValue();
	const TSharedPtr<SWidget> Scope = Host.Scope ? Host.Scope() : nullptr;
	return Scope.IsValid() && WoTHooksIsInside(WoTHooksUserFocus(Player), Scope.Get());
}

/** Any change to the control yaw since the hooks last left it is gameplay's:
 * add it to the aim owner's gameplay yaw. */
void WoTHooksFoldGameplayYaw(FWoTHookConsumerState& Owner, const APlayerController& Controller)
{
	const double Yaw = Controller.GetControlRotation().Yaw;
	if (Owner.bHasAimYaw)
	{
		Owner.GameplayYaw = FMath::UnwindDegrees(
			Owner.GameplayYaw + FMath::FindDeltaAngleDegrees(Owner.AimYaw, Yaw));
	}
	Owner.AimYaw = Yaw;
	Owner.bHasAimYaw = true;
}

/** The aim owner with its gameplay yaw brought up to date, outside the
 * controller's own rotation update (whose writes are the hooks' own). */
TSharedPtr<FWoTHookConsumerState> WoTHooksCurrentAimOwner(const ULocalPlayer& Player)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Player);
	const TSharedPtr<FWoTHookConsumerState> Owner = WoTHooksAimOwner(Hooks);
	const AWoTPlayerController* Controller = Cast<AWoTPlayerController>(Player.PlayerController);
	if (Owner && Controller && Hooks->AimUpdateDepth == 0)
	{
		WoTHooksFoldGameplayYaw(*Owner, *Controller);
	}
	return Owner;
}

/** Whether the game viewport ignores input: a window holds UI-only input (the
 * in-game menu, help, chat, an onboarding window), so no key reaches a binding
 * and no stick moves the body. */
bool WoTHooksViewportIgnoresInput(const APlayerController& Controller)
{
	const ULocalPlayer* Player = Controller.GetLocalPlayer();
	UGameViewportClient* Viewport = Player ? Player->ViewportClient.Get() : nullptr;
	return Viewport && Viewport->IgnoreInput();
}

void WoTHooksSetAnchor(FWoTHookPlayerState& Hooks, APawn* Pawn)
{
	Hooks.AnchorPawn = Pawn;
	Hooks.AnchorLocation = Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
}

/** Release each held command exactly once, through the binding's own release
 * handler (WoTGamepadLayer.cpp's ReleaseHeldEdges discipline). */
void WoTHooksRelease(FWoTHookConsumerState& Consumer)
{
	const bool bFire = Consumer.bFireHeld;
	const bool bJump = Consumer.bJumpHeld;
	const bool bScores = Consumer.bScoresChanged;
	Consumer.bFireHeld = false;
	Consumer.bJumpHeld = false;
	Consumer.bScoresChanged = false;
	AWoTPlayerController* Controller = Consumer.CommandController.Get();
	if (!Controller)
	{
		return;
	}
	if (bFire)
	{
		Controller->CeaseFire();
	}
	if (bJump)
	{
		Controller->StopJumping();
	}
	if (bScores && Controller->bShowScores && !Controller->IsMatchEnded()
		&& !Controller->IsDeathScoreboardAutoShowActive())
	{
		Controller->ShowScores();
	}
}

/** The transitions after which held input must be pressed afresh. A teleport,
 * a respawn, a forced turn or a new possession alone does not interrupt a hold. */
bool WoTHooksEventReleasesCommands(EWoTLocalPlayerEvent Event)
{
	switch (Event)
	{
	case EWoTLocalPlayerEvent::ControllerChanged:
	case EWoTLocalPlayerEvent::Unpossessed:
	case EWoTLocalPlayerEvent::Travel:
	case EWoTLocalPlayerEvent::FrontEnd:
	case EWoTLocalPlayerEvent::Death:
	case EWoTLocalPlayerEvent::InputContextChanged:
		return true;
	default:
		return false;
	}
}

void WoTHooksEmit(const TSharedPtr<FWoTHookPlayerState>& Hooks, EWoTLocalPlayerEvent Event)
{
	if (!Hooks)
	{
		return;
	}
	const FWoTLocalPlayerContext Context = Hooks->Context;
	const bool bRelease = WoTHooksEventReleasesCommands(Event);
	for (const TSharedPtr<FWoTHookConsumerState>& Consumer : WoTHooksConsumers(Hooks))
	{
		if (!Consumer->bActive)
		{
			continue;
		}
		if (bRelease)
		{
			WoTHooksRelease(*Consumer);
		}
		if (Consumer->bActive && Consumer->Callbacks.Lifecycle)
		{
			Consumer->Callbacks.Lifecycle(Event, Context);
		}
	}
}

void WoTHooksReleaseAll(const TSharedPtr<FWoTHookPlayerState>& Hooks)
{
	for (const TSharedPtr<FWoTHookConsumerState>& Consumer : WoTHooksConsumers(Hooks))
	{
		if (Consumer->bActive)
		{
			WoTHooksRelease(*Consumer);
		}
	}
}
}

bool FWoTLocalPlayerHooks::Attach(ULocalPlayer& Player, FName Owner,
	const FWoTLocalPlayerConsumer& Consumer)
{
	check(IsInGameThread());
	for (auto It = GWoTLocalPlayerHooks.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Player);
	bool bConflict = Owner.IsNone() || (Hooks && Hooks->Consumers.Contains(Owner));
	if (Hooks)
	{
		for (const TPair<FName, TSharedPtr<FWoTHookConsumerState>>& Pair : Hooks->Consumers)
		{
			const FWoTLocalPlayerConsumer& Existing = Pair.Value->Callbacks;
			const bool bAim = Consumer.AimRotation && Existing.AimRotation;
			const bool bMovement = (Consumer.GroundMovementYaw || Consumer.ExtraMovement)
				&& (Existing.GroundMovementYaw || Existing.ExtraMovement);
			const bool bView = Consumer.View.IsSet() && Existing.View.IsSet();
			const bool bHUD = Consumer.HUDDestination && Existing.HUDDestination;
			const bool bUI = Consumer.UIHost.IsSet() && Existing.UIHost.IsSet();
			bConflict |= bAim || bMovement || bView || bHUD || bUI;
		}
	}
	if (bConflict)
	{
		UE_LOG(LogWoTLocalPlayerHooks, Warning,
			TEXT("Rejected %s: duplicate owner or exclusive local-player slot already occupied"),
			*Owner.ToString());
		return false;
	}
	if (!Hooks)
	{
		Hooks = MakeShared<FWoTHookPlayerState>();
		Hooks->Context.Player = &Player;
		GWoTLocalPlayerHooks.Add(&Player, Hooks);
	}
	const TSharedPtr<FWoTHookConsumerState> State = MakeShared<FWoTHookConsumerState>();
	State->Callbacks = Consumer;
	State->bActive = false;
	Hooks->Consumers.Add(Owner, State);
	// Bring the shared context up to date first; only consumers already attached
	// hear about changes that happened before this one arrived.
	APlayerController* Controller = Player.PlayerController;
	if (Controller)
	{
		Reconcile(*Controller);
	}
	State->bActive = true;
	const FWoTLocalPlayerContext Context = Hooks->Context;
	if (State->Callbacks.Lifecycle)
	{
		State->Callbacks.Lifecycle(EWoTLocalPlayerEvent::Attached, Context);
		// Then the current state, to the new consumer alone.
		if (State->bActive && Context.Controller.IsValid()
			&& Context.Controller->IsA<AWoTFrontEndPlayerController>())
		{
			State->Callbacks.Lifecycle(EWoTLocalPlayerEvent::FrontEnd, Context);
		}
		if (State->bActive && Context.Pawn.IsValid())
		{
			State->Callbacks.Lifecycle(EWoTLocalPlayerEvent::Possessed, Context);
		}
	}
	if (State->bActive && State->Callbacks.RegisterInput)
	{
		State->Callbacks.RegisterInput(Player,
			Controller ? Controller->InputComponent.Get() : nullptr);
	}
	if (State->bActive && State->Callbacks.UIHost.IsSet())
	{
		WoTHooksBeginHosting(Player, *State);
	}
	return true;
}

void FWoTLocalPlayerHooks::Detach(ULocalPlayer& Player, FName Owner)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Player);
	const TSharedPtr<FWoTHookConsumerState> Consumer =
		Hooks ? Hooks->Consumers.FindRef(Owner) : nullptr;
	if (!Consumer)
	{
		return;
	}
	Hooks->Consumers.Remove(Owner);
	Consumer->bActive = false;
	WoTHooksRelease(*Consumer);
	if (Consumer->Callbacks.UIHost.IsSet())
	{
		WoTHooksEndHosting(&Player, *Consumer);
	}
	if (Hooks->Consumers.IsEmpty())
	{
		GWoTLocalPlayerHooks.Remove(&Player);
	}
	if (Consumer->Callbacks.Lifecycle)
	{
		Consumer->Callbacks.Lifecycle(EWoTLocalPlayerEvent::Detached, Hooks->Context);
	}
}

void FWoTLocalPlayerHooks::DetachAll(ULocalPlayer& Player)
{
	if (const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Player))
	{
		TArray<FName> Owners;
		Hooks->Consumers.GetKeys(Owners);
		for (const FName Owner : Owners)
		{
			Detach(Player, Owner);
		}
	}
}

bool FWoTLocalPlayerHooks::HasConsumers(const ULocalPlayer* Player)
{
	return WoTHooksFind(Player).IsValid();
}

double FWoTLocalPlayerHooks::PeekGameplayYaw(const ULocalPlayer& Player)
{
	const TSharedPtr<FWoTHookConsumerState> Owner = WoTHooksCurrentAimOwner(Player);
	return Owner ? Owner->GameplayYaw : 0.0;
}

double FWoTLocalPlayerHooks::TakeGameplayYaw(ULocalPlayer& Player, FName Owner)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Player);
	const TSharedPtr<FWoTHookConsumerState> Consumer =
		Hooks ? Hooks->Consumers.FindRef(Owner) : nullptr;
	const TSharedPtr<FWoTHookConsumerState> AimOwner = WoTHooksCurrentAimOwner(Player);
	if (!Consumer || Consumer != AimOwner)
	{
		return 0.0;
	}
	const double Taken = Consumer->GameplayYaw;
	Consumer->GameplayYaw = 0.0;
	return Taken;
}

bool FWoTLocalPlayerHooks::IsForcedTurn(const ULocalPlayer& Player)
{
	const AWoTPlayerController* Controller = Cast<AWoTPlayerController>(Player.PlayerController);
	return Controller && Controller->bForcedViewYaw;
}

void FWoTLocalPlayerHooks::HoldMovement(ULocalPlayer& Player, FName Owner, bool bHold)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Player);
	const TSharedPtr<FWoTHookConsumerState> Consumer =
		Hooks ? Hooks->Consumers.FindRef(Owner) : nullptr;
	if (Consumer && Consumer == WoTHooksMovementOwner(Hooks))
	{
		Consumer->bMovementHeld = bHold;
	}
}

bool FWoTLocalPlayerHooks::IsMovementHeld(const APlayerController* Controller)
{
	const TSharedPtr<FWoTHookConsumerState> Owner = WoTHooksMovementOwner(WoTHooksFind(Controller));
	return Owner && Owner->bMovementHeld;
}

bool FWoTLocalPlayerHooks::IsGameInputBlocked(const APlayerController* Controller)
{
	if (!Controller || !Controller->GetLocalPlayer())
	{
		return false;
	}
	if (WoTHooksViewportIgnoresInput(*Controller))
	{
		return true;
	}
	const AWoTPlayerController* WoTController = Cast<AWoTPlayerController>(Controller);
	return WoTController && (WoTController->IsChatTyping() || WoTController->IsInGameMenuOpen()
		|| WoTController->IsHelpOpen() || WoTController->IsItemInfoOpen());
}

bool FWoTLocalPlayerHooks::OwnsAim(const APlayerController* Controller)
{
	return WoTHooksAimOwner(WoTHooksFind(Controller)).IsValid();
}

bool FWoTLocalPlayerHooks::BeginAimUpdate(APlayerController& Controller)
{
	if (!WoTHooksAimOwner(WoTHooksFind(&Controller)))
	{
		return false;
	}
	// A pending relocation reaches consumers before the aim is sampled again, so
	// they read the relocation's own heading.
	Reconcile(Controller);
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Controller);
	const TSharedPtr<FWoTHookConsumerState> Owner = WoTHooksAimOwner(Hooks);
	if (!Owner)
	{
		return false;
	}
	// A nested update (a cast or launch inside one) adds nothing: the yaw is
	// already the hooks' own there.
	if (Hooks->AimUpdateDepth++ == 0)
	{
		WoTHooksFoldGameplayYaw(*Owner, Controller);
	}
	return true;
}

bool FWoTLocalPlayerHooks::SampleAim(APlayerController& Controller, FRotator& Rotation)
{
	const TSharedPtr<FWoTHookConsumerState> Owner = WoTHooksAimOwner(WoTHooksFind(&Controller));
	if (!Owner)
	{
		return false;
	}
	FRotator Supplied = Rotation;
	if (!Owner->Callbacks.AimRotation(Supplied) || !Owner->bActive || Supplied.ContainsNaN())
	{
		return false;
	}
	// Yaw and pitch only: roll stays the controller's own. Gameplay's yaw changes
	// keep applying on top of the provider's.
	Rotation = FRotator(Supplied.Pitch, Supplied.Yaw + Owner->GameplayYaw, Rotation.Roll)
		.GetNormalized();
	return true;
}

void FWoTLocalPlayerHooks::EndAimUpdate(APlayerController& Controller)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Controller);
	if (!Hooks || Hooks->AimUpdateDepth == 0 || --Hooks->AimUpdateDepth > 0)
	{
		return;
	}
	if (const TSharedPtr<FWoTHookConsumerState> Owner = WoTHooksAimOwner(Hooks))
	{
		Owner->AimYaw = Controller.GetControlRotation().Yaw;
		Owner->bHasAimYaw = true;
	}
}

double FWoTLocalPlayerHooks::GroundYaw(const APlayerController* Controller, double Fallback)
{
	const TSharedPtr<FWoTHookConsumerState> Owner = WoTHooksMovementOwner(WoTHooksFind(Controller));
	if (!Owner || !Owner->Callbacks.GroundMovementYaw)
	{
		return Fallback;
	}
	double Yaw = Fallback;
	return Owner->Callbacks.GroundMovementYaw(Yaw) && Owner->bActive && FMath::IsFinite(Yaw)
		? Yaw : Fallback;
}

FVector FWoTLocalPlayerHooks::ExtraMovement(const APawn& Pawn)
{
	if (GWoTLocalPlayerHooks.IsEmpty() || !Pawn.IsLocallyControlled())
	{
		return FVector::ZeroVector;
	}
	const AWoTPlayerController* Controller = Cast<AWoTPlayerController>(Pawn.GetController());
	const TSharedPtr<FWoTHookConsumerState> Owner = WoTHooksMovementOwner(WoTHooksFind(Controller));
	if (!Owner || !Owner->Callbacks.ExtraMovement)
	{
		return FVector::ZeroVector;
	}
	// The stick's own gates: no movement input while it is ignored, while a
	// window holds UI-only input (no key or stick reaches the game then), typed
	// into chat, behind a retail window, frozen by the match end or consumed by
	// the orbit camera, nor for a dead or constrained body.
	const AWoTCharacter* Character = Cast<AWoTCharacter>(&Pawn);
	if (Controller->IsMoveInputIgnored() || WoTHooksViewportIgnoresInput(*Controller)
		|| Controller->IsChatTyping()
		|| Controller->IsInGameMenuOpen() || Controller->IsHelpOpen()
		|| Controller->IsItemInfoOpen() || Controller->IsMatchEnded()
		|| Controller->GetCitadelEditCameraMode() == EWoTCitadelEditCameraMode::Orbit
		|| (Character && (Character->bDead || Character->IsPlayerMovementConstrained())))
	{
		return FVector::ZeroVector;
	}
	const FVector Extra = Owner->Callbacks.ExtraMovement();
	return Owner->bActive && !Extra.ContainsNaN() ? Extra : FVector::ZeroVector;
}

bool FWoTLocalPlayerHooks::SuppressSyntheticMotion(const APlayerController* Controller)
{
	const TSharedPtr<FWoTHookConsumerState> Owner = WoTHooksViewOwner(WoTHooksFind(Controller));
	return Owner && Owner->Callbacks.View->bSuppressSyntheticMotion;
}

void FWoTLocalPlayerHooks::ComposeView(const ULocalPlayer& Player, FMinimalViewInfo& View)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Player);
	const TSharedPtr<FWoTHookConsumerState> Owner = WoTHooksViewOwner(Hooks);
	if (!Owner)
	{
		return;
	}
	if (Owner->Callbacks.View->Compose)
	{
		FVector Location = View.Location;
		FRotator Rotation = View.Rotation;
		Owner->Callbacks.View->Compose(Hooks->Context, View, Location, Rotation);
		if (!Location.ContainsNaN() && !Rotation.ContainsNaN())
		{
			View.Location = Location;
			View.Rotation = Rotation;
		}
	}
	if (const TOptional<float> Near = NearClip(Player))
	{
		View.PerspectiveNearClipPlane = *Near;
	}
}

TOptional<float> FWoTLocalPlayerHooks::NearClip(const ULocalPlayer& Player)
{
	const TSharedPtr<FWoTHookConsumerState> Owner = WoTHooksViewOwner(WoTHooksFind(&Player));
	if (Owner && Owner->Callbacks.View->NearClip)
	{
		const float Near = Owner->Callbacks.View->NearClip();
		if (FMath::IsFinite(Near) && Near > 0.0f)
		{
			return Near;
		}
	}
	return {};
}

void FWoTLocalPlayerHooks::ApplyNearClip(const ULocalPlayer& Player, FMatrix& Projection)
{
	const TOptional<float> Near = NearClip(Player);
	// Only a perspective projection (w = z); an orthographic one keeps its planes.
	if (!Near || !FMath::IsNearlyEqual(Projection.M[2][3], 1.0)
		|| !FMath::IsNearlyZero(Projection.M[3][3]))
	{
		return;
	}
	const double Depth = Projection.M[2][2];
	if (FMath::IsNearlyZero(Depth))
	{
		// Infinite far plane: depth = Near / z.
		Projection.M[3][2] = *Near;
	}
	else
	{
		const double Far = -Projection.M[3][2] / Depth;
		if (Far > *Near)
		{
			Projection.M[2][2] = *Near / (*Near - Far);
			Projection.M[3][2] = -Far * Projection.M[2][2];
		}
	}
}

void FWoTLocalPlayerHooks::Reconcile(APlayerController& Controller)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Controller);
	if (!Hooks)
	{
		return;
	}
	if (Hooks->Context.Controller.Get() != &Controller)
	{
		if (!Hooks->Context.Pawn.IsExplicitlyNull())
		{
			WoTHooksEmit(Hooks, EWoTLocalPlayerEvent::Unpossessed);
		}
		Hooks->Context.Controller = &Controller;
		Hooks->Context.Pawn.Reset();
		Hooks->Context.World = Controller.GetWorld();
		Hooks->RelocatedPawn.Reset();
		Hooks->AnchorPawn.Reset();
		Hooks->bTravelNotified = false;
		WoTHooksEmit(Hooks, EWoTLocalPlayerEvent::ControllerChanged);
		if (Controller.IsA<AWoTFrontEndPlayerController>())
		{
			WoTHooksEmit(Hooks, EWoTLocalPlayerEvent::FrontEnd);
		}
	}
	else if (Hooks->Context.World.Get() != Controller.GetWorld())
	{
		// The same controller in another world (a seamless travel keeps it): the
		// context names the new world, and the next departure is announced again.
		Hooks->Context.World = Controller.GetWorld();
		Hooks->RelocatedPawn.Reset();
		Hooks->AnchorPawn.Reset();
		Hooks->bTravelNotified = false;
	}
	APawn* const Current = Controller.GetPawn();
	// A destroyed pawn reads as null through its weak pointer; it still counts as
	// a pawn that was possessed until an unpossession has been announced.
	if (Hooks->Context.Pawn.Get() != Current
		|| (!Current && !Hooks->Context.Pawn.IsExplicitlyNull()))
	{
		if (!Hooks->Context.Pawn.IsExplicitlyNull())
		{
			WoTHooksEmit(Hooks, EWoTLocalPlayerEvent::Unpossessed);
		}
		Hooks->Context.Pawn = Current;
		Hooks->RelocatedPawn.Reset();
		if (Current)
		{
			WoTHooksEmit(Hooks, EWoTLocalPlayerEvent::Possessed);
		}
	}
	else if (Current && Hooks->RelocatedPawn.Get() == Current)
	{
		Hooks->RelocatedPawn.Reset();
		WoTHooksEmit(Hooks, EWoTLocalPlayerEvent::Teleported);
	}
	// Later relocations are measured from here.
	WoTHooksSetAnchor(*Hooks, Controller.GetPawn());
}

void FWoTLocalPlayerHooks::PrepareInput(APlayerController& Controller, UInputComponent* Input)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Controller);
	if (!Hooks)
	{
		return;
	}
	Reconcile(Controller);
	ULocalPlayer* const Player = Controller.GetLocalPlayer();
	for (const TSharedPtr<FWoTHookConsumerState>& Consumer : WoTHooksConsumers(Hooks))
	{
		if (Consumer->bActive && Consumer->Callbacks.RegisterInput)
		{
			Consumer->Callbacks.RegisterInput(*Player, Input);
		}
	}
}

void FWoTLocalPlayerHooks::Notify(APlayerController& Controller, EWoTLocalPlayerEvent Event)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Controller);
	if (!Hooks)
	{
		return;
	}
	Reconcile(Controller);
	if (Event == EWoTLocalPlayerEvent::Travel)
	{
		Hooks->bTravelNotified = true;
	}
	WoTHooksEmit(Hooks, Event);
}

void FWoTLocalPlayerHooks::PawnChanging(APlayerController& Controller)
{
	WoTHooksReleaseAll(WoTHooksFind(&Controller));
}

void FWoTLocalPlayerHooks::PawnRelocated(AActor* Actor)
{
	if (GWoTLocalPlayerHooks.IsEmpty())
	{
		return;
	}
	APawn* const Relocated = Cast<APawn>(Actor);
	const TSharedPtr<FWoTHookPlayerState> Hooks = Relocated
		? WoTHooksFind(Cast<APlayerController>(Relocated->GetController())) : nullptr;
	if (!Hooks)
	{
		return;
	}
	// The jump from where the hooks last saw the pawn: the relocation itself plus
	// at most the ordinary movement of the frame it happened in.
	const FVector Location = Relocated->GetActorLocation();
	if (Hooks->AnchorPawn.Get() == Relocated
		&& FVector::DistSquared(Location, Hooks->AnchorLocation) > FMath::Square(TeleportDistance))
	{
		Hooks->RelocatedPawn = Relocated;
	}
	WoTHooksSetAnchor(*Hooks, Relocated);
}

void FWoTLocalPlayerHooks::PawnCorrected(APawn& Pawn, const FVector& ClientLocation,
	const FVector& ServerLocation)
{
	if (GWoTLocalPlayerHooks.IsEmpty())
	{
		return;
	}
	const TSharedPtr<FWoTHookPlayerState> Hooks =
		WoTHooksFind(Cast<APlayerController>(Pawn.GetController()));
	if (!Hooks)
	{
		return;
	}
	// The jump the correction makes: where the server has the pawn against
	// where the client had it for the same move.
	if (FVector::DistSquared(ServerLocation, ClientLocation) > FMath::Square(TeleportDistance))
	{
		Hooks->RelocatedPawn = &Pawn;
	}
	Hooks->AnchorPawn = &Pawn;
	Hooks->AnchorLocation = ServerLocation;
}

void FWoTLocalPlayerHooks::ControllerEnded(APlayerController& Controller,
	EEndPlayReason::Type Reason)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Controller);
	if (!Hooks || Hooks->Context.Controller.Get() != &Controller)
	{
		return;
	}
	// A controller destroyed in a world that carries on is a replacement; any
	// other end leaves the world (the engine destroys the local controller
	// while tearing the old world down for travel).
	const UWorld* World = Controller.GetWorld();
	const bool bLeavingWorld = Reason != EEndPlayReason::Destroyed
		|| (World && World->bIsTearingDown);
	if (bLeavingWorld && !Hooks->bTravelNotified)
	{
		Hooks->bTravelNotified = true;
		WoTHooksEmit(Hooks, EWoTLocalPlayerEvent::Travel);
	}
	if (!Hooks->Context.Pawn.IsExplicitlyNull())
	{
		WoTHooksEmit(Hooks, EWoTLocalPlayerEvent::Unpossessed);
	}
	Hooks->Context.Controller.Reset();
	Hooks->Context.Pawn.Reset();
	Hooks->Context.World.Reset();
	Hooks->RelocatedPawn.Reset();
	Hooks->AnchorPawn.Reset();
	WoTHooksEmit(Hooks, EWoTLocalPlayerEvent::ControllerChanged);
}

void FWoTLocalPlayerHooks::PlayerDestroyed(const ULocalPlayer& Player)
{
	// No callbacks: the player may be going away inside garbage collection.
	for (auto It = GWoTLocalPlayerHooks.CreateIterator(); It; ++It)
	{
		if (It.Key().GetEvenIfUnreachable() == &Player || !It.Key().IsValid())
		{
			for (const TPair<FName, TSharedPtr<FWoTHookConsumerState>>& Pair : It.Value()->Consumers)
			{
				Pair.Value->bActive = false;
				Pair.Value->UIScope.Reset();
				Pair.Value->Hosted.Reset();
			}
			It.RemoveCurrent();
		}
	}
}

void FWoTLocalPlayerHooks::ReleaseCommands(ULocalPlayer& Player, FName Owner)
{
	if (const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Player))
	{
		if (const TSharedPtr<FWoTHookConsumerState> Consumer = Hooks->Consumers.FindRef(Owner))
		{
			WoTHooksRelease(*Consumer);
		}
	}
}

bool FWoTLocalPlayerHooks::Command(ULocalPlayer& Player, FName Owner,
	EWoTLocalPlayerCommand Command, int32 Index)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Player);
	const TSharedPtr<FWoTHookConsumerState> Consumer =
		Hooks ? Hooks->Consumers.FindRef(Owner) : nullptr;
	AWoTPlayerController* Controller = Cast<AWoTPlayerController>(Player.PlayerController);
	if (!Consumer || !Consumer->bActive || !Controller)
	{
		return false;
	}
	if (Consumer->CommandController.Get() != Controller)
	{
		WoTHooksRelease(*Consumer);
		Consumer->CommandController = Controller;
	}
	AWoTCharacter* const Character = Cast<AWoTCharacter>(Controller->GetPawn());
	// The gates of the key bindings these commands copy
	// (AWoTPlayerController::SetupInputComponent,
	// AWoTCharacter::SetupPlayerInputComponent): no key reaches a binding while a
	// window holds UI-only input, and a press binding that does not run when
	// paused does nothing while paused. Every release binding runs regardless.
	const UWorld* World = Controller->GetWorld();
	const bool bPaused = World && World->IsPaused();
	const bool bUIOnly = WoTHooksViewportIgnoresInput(*Controller);
	const bool bPress = !bUIOnly && !bPaused;
	switch (Command)
	{
	case EWoTLocalPlayerCommand::Fire:
		if (!Consumer->bFireHeld && bPress)
		{
			Consumer->bFireHeld = true;
			Controller->Fire();
		}
		break;
	case EWoTLocalPlayerCommand::CeaseFire:
		if (Consumer->bFireHeld)
		{
			Consumer->bFireHeld = false;
			Controller->CeaseFire();
		}
		break;
	case EWoTLocalPlayerCommand::Jump:
		// Runs when paused, like its binding: the handler turns a paused press
		// into an unpause request.
		if (!Consumer->bJumpHeld && !bUIOnly)
		{
			Consumer->bJumpHeld = true;
			Controller->Jump();
		}
		break;
	case EWoTLocalPlayerCommand::StopJumping:
		if (Consumer->bJumpHeld)
		{
			Consumer->bJumpHeld = false;
			Controller->StopJumping();
		}
		break;
	case EWoTLocalPlayerCommand::ShowScores:
		if (!Controller->bShowScores && bPress)
		{
			Consumer->bScoresChanged = true;
			Controller->ShowScores();
		}
		break;
	case EWoTLocalPlayerCommand::HideScores:
		if (Consumer->bScoresChanged && Controller->bShowScores && !Controller->IsMatchEnded()
			&& !Controller->IsDeathScoreboardAutoShowActive())
		{
			Controller->ShowScores();
		}
		Consumer->bScoresChanged = false;
		break;
	case EWoTLocalPlayerCommand::Menu:
	{
		// Runs when paused, like its binding. Behind a window that holds UI-only
		// input the key is the window's: the game's own windows (the in-game menu,
		// help, item information, chat) close on it, as the key's handler closes
		// them; any other window keeps it.
		if (bUIOnly && !Controller->IsInGameMenuOpen() && !Controller->IsHelpOpen()
			&& !Controller->IsItemInfoOpen() && !Controller->IsChatTyping())
		{
			break;
		}
		const bool bWasOpen = Controller->IsInGameMenuOpen();
		Controller->ToggleInGameMenu();
		// Opening the menu is a modal entry: held commands are released, as the
		// menu's own key flush releases held keys.
		if (!bWasOpen && Controller->IsInGameMenuOpen())
		{
			WoTHooksReleaseAll(Hooks);
		}
		break;
	}
	case EWoTLocalPlayerCommand::NextItem:
		if (Character && bPress) { Character->NextItem(); }
		break;
	case EWoTLocalPlayerCommand::PreviousItem:
		if (Character && bPress) { Character->PrevItem(); }
		break;
	case EWoTLocalPlayerCommand::NextGroup:
		if (Character && bPress) { Character->NextHand(); }
		break;
	case EWoTLocalPlayerCommand::PreviousGroup:
		if (Character && bPress) { Character->PrevHand(); }
		break;
	case EWoTLocalPlayerCommand::SelectGroup:
		// The hand key's own handler, team prompt first; only the hands have keys.
		if (Character && bPress && Index >= 0 && Index < UWoTHandSet::NumHands)
		{
			Character->SelectHandPressed(Index);
		}
		break;
	case EWoTLocalPlayerCommand::SelectSlot:
		if (Character && bPress) { Character->SelectSlot(Index); }
		break;
	}
	return true;
}

FWoTSelectionSnapshot FWoTLocalPlayerHooks::Selection(ULocalPlayer& Player)
{
	FWoTSelectionSnapshot Result;
	const AWoTCharacter* Character = Player.PlayerController
		? Cast<AWoTCharacter>(Player.PlayerController->GetPawn()) : nullptr;
	const UWoTHandSet* Set = Character ? Character->CurrentHandSet.Get() : nullptr;
	if (!Set)
	{
		return Result;
	}
	Result.SelectedGroup = Set->Selected;
	if (const UWoTHandInfo* Selected = Set->GetSelectedHand())
	{
		Result.SelectedSlot = Selected->Selected;
	}
	for (int32 Group = 0; Group < UWoTHandSet::NumHands; ++Group)
	{
		const UWoTHandInfo* Hand = Set->GetHand(Group);
		if (!Hand)
		{
			continue;
		}
		for (int32 Slot = 0; Slot < UWoTHandInfo::NumSlots; ++Slot)
		{
			if (Hand->ClassName[Slot].IsNone())
			{
				continue;
			}
			FWoTSelectionSlot Row;
			Row.Group = Group;
			Row.Slot = Slot;
			Row.ClassName = Hand->ClassName[Slot];
			Row.bOwned = Hand->bHaveThis[Slot];
			Row.bSelected = Group == Result.SelectedGroup && Slot == Result.SelectedSlot;
			// The editor palette carries budget counts; the live hands read the item.
			Row.Charges = Hand->GetDisplayCount(Slot);
			if (Row.Charges == INDEX_NONE)
			{
				const AWoTAngrealInventory* Item = Cast<AWoTAngrealInventory>(Hand->GetItem(Slot));
				Row.Charges = Item ? Item->CurCharges : 0;
			}
			Result.Slots.Add(Row);
		}
	}
	return Result;
}

const FWoTHUDSnapshot* FWoTLocalPlayerHooks::LatestHUD(const ULocalPlayer& Player)
{
	const APlayerController* Controller = Player.PlayerController;
	const AWoTHUD* HUD = Controller ? Cast<AWoTHUD>(Controller->GetHUD()) : nullptr;
	return HUD && HUD->GetSnapshot().Serial > 0 ? &HUD->GetSnapshot() : nullptr;
}

bool FWoTLocalPlayerHooks::OwnsHUDDestination(const APlayerController* Controller)
{
	return WoTHooksHUDDestinationOwner(WoTHooksFind(Controller)).IsValid();
}

void FWoTLocalPlayerHooks::HUDUpdated(APlayerController& Controller,
	const FWoTHUDSnapshot& Snapshot)
{
	const TSharedPtr<FWoTHookPlayerState> Hooks = WoTHooksFind(&Controller);
	for (const TSharedPtr<FWoTHookConsumerState>& Consumer : WoTHooksConsumers(Hooks))
	{
		if (Consumer->bActive && Consumer->Callbacks.HUDObserver)
		{
			Consumer->Callbacks.HUDObserver(Snapshot);
		}
		if (Consumer->bActive && Consumer->Callbacks.HUDDestination)
		{
			Consumer->Callbacks.HUDDestination(Snapshot);
		}
	}
}

int32 FWoTLocalPlayerHooks::UISlateUser(const ULocalPlayer& Player)
{
	if (const TSharedPtr<const FSlateUser> User = Player.GetSlateUser())
	{
		return User->GetUserIndex();
	}
	return FSlateApplication::IsInitialized()
		? FSlateApplication::Get().GetUserIndexForKeyboard() : 0;
}

bool FWoTLocalPlayerHooks::RoutePointer(ULocalPlayer& Player, FName Owner,
	const FVector2D& Position, EWoTUIPointerAction Action)
{
	const TSharedPtr<FWoTHookConsumerState> Consumer = WoTHooksHostOf(Player, Owner);
	return Consumer && WoTHooksRoutePointer(Player, *Consumer, Position, Action);
}

bool FWoTLocalPlayerHooks::RouteKey(ULocalPlayer& Player, FName Owner, const FKey& Key,
	bool bPressed, bool bRepeat)
{
	const TSharedPtr<FWoTHookConsumerState> Consumer = WoTHooksHostOf(Player, Owner);
	if (!Consumer || !FSlateApplication::IsInitialized()
		|| !WoTHooksFocusInScope(Player, *Consumer))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const FKeyEvent Event(Key, Slate.GetModifierKeys(), UISlateUser(Player), bRepeat, 0, 0);
	if (bPressed)
	{
		Consumer->RoutedKeys.Add(Key);
		Slate.ProcessKeyDownEvent(Event);
	}
	else
	{
		Consumer->RoutedKeys.Remove(Key);
		Slate.ProcessKeyUpEvent(Event);
	}
	return true;
}

bool FWoTLocalPlayerHooks::RouteAnalog(ULocalPlayer& Player, FName Owner, const FKey& Key,
	float Value)
{
	const TSharedPtr<FWoTHookConsumerState> Consumer = WoTHooksHostOf(Player, Owner);
	if (!Consumer || !FSlateApplication::IsInitialized()
		|| !WoTHooksFocusInScope(Player, *Consumer))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const FAnalogInputEvent Event(Key, Slate.GetModifierKeys(), UISlateUser(Player),
		/*bIsRepeat*/ false, 0, 0, Value);
	if (FMath::IsNearlyZero(Value))
	{
		Consumer->RoutedAxes.Remove(Key);
	}
	else
	{
		Consumer->RoutedAxes.Add(Key);
	}
	Slate.ProcessAnalogInputEvent(Event);
	return true;
}

bool FWoTLocalPlayerHooks::IsRoutingUIPointer(int32 SlateUserIndex)
{
	return GWoTRoutingUIPointerUser != INDEX_NONE && GWoTRoutingUIPointerUser == SlateUserIndex;
}

bool FWoTLocalPlayerHooks::UsesPanelTextEntry()
{
	for (const TPair<TWeakObjectPtr<ULocalPlayer>, TSharedPtr<FWoTHookPlayerState>>& Entry
		: GWoTLocalPlayerHooks)
	{
		const TSharedPtr<FWoTHookConsumerState> Host = WoTHooksUIHostOwner(Entry.Value);
		if (Host && !Host->Callbacks.UIHost->bRuntimeTextEntry)
		{
			return true;
		}
	}
	return false;
}

bool FWoTLocalPlayerHooks::IsTextEntryActive()
{
	return FWoTTextEntry::IsActive();
}

FVector2D FWoTLocalPlayerHooks::UIScreenSize(const UGameViewportClient& Viewport)
{
	FVector2D Size = FVector2D::ZeroVector;
	Viewport.GetViewportSize(Size);
	for (const TPair<TWeakObjectPtr<ULocalPlayer>, TSharedPtr<FWoTHookPlayerState>>& Entry
		: GWoTLocalPlayerHooks)
	{
		const ULocalPlayer* Player = Entry.Key.Get();
		if (!Player || Player->ViewportClient != &Viewport)
		{
			continue;
		}
		const TSharedPtr<FWoTHookConsumerState> Host = WoTHooksUIHostOwner(Entry.Value);
		if (Host && Host->Callbacks.UIHost->ScreenSize)
		{
			const FIntPoint Surface = Host->Callbacks.UIHost->ScreenSize();
			if (Surface.X > 0 && Surface.Y > 0)
			{
				return FVector2D(Surface);
			}
		}
	}
	return Size;
}

bool FWoTLocalPlayerHooks::HostUIContent(UGameViewportClient& Viewport,
	const TSharedRef<SWidget>& Screen, int32 ZOrder)
{
	if (GWoTLocalPlayerHooks.IsEmpty())
	{
		return false;
	}
	// A copy: the host's callback may attach or detach consumers.
	TArray<TPair<TWeakObjectPtr<ULocalPlayer>, TSharedPtr<FWoTHookPlayerState>>> Players;
	for (const TPair<TWeakObjectPtr<ULocalPlayer>, TSharedPtr<FWoTHookPlayerState>>& Entry
		: GWoTLocalPlayerHooks)
	{
		Players.Add(Entry);
	}
	for (const TPair<TWeakObjectPtr<ULocalPlayer>, TSharedPtr<FWoTHookPlayerState>>& Entry : Players)
	{
		const ULocalPlayer* Player = Entry.Key.Get();
		if (!Player || Player->ViewportClient != &Viewport)
		{
			continue;
		}
		// A host that detached inside its own Show let the screen go; whoever
		// hosts for the player afterwards is offered it once.
		TSharedPtr<FWoTHookConsumerState> Tried;
		for (int32 Attempt = 0; Attempt < 2; ++Attempt)
		{
			const TSharedPtr<FWoTHookConsumerState> Host = WoTHooksUIHostOwner(WoTHooksFind(Player));
			if (!Host || Host == Tried)
			{
				break;
			}
			if (WoTHooksShowHosted(*Host, Viewport, Screen, ZOrder))
			{
				return true;
			}
			if (Host->bActive)
			{
				break;
			}
			Tried = Host;
		}
	}
	return false;
}

bool FWoTLocalPlayerHooks::UnhostUIContent(UGameViewportClient& Viewport,
	const TSharedRef<SWidget>& Screen)
{
	if (GWoTLocalPlayerHooks.IsEmpty())
	{
		return false;
	}
	for (const TPair<TWeakObjectPtr<ULocalPlayer>, TSharedPtr<FWoTHookPlayerState>>& Entry
		: GWoTLocalPlayerHooks)
	{
		for (const TSharedPtr<FWoTHookConsumerState>& Consumer : WoTHooksConsumers(Entry.Value))
		{
			const int32 Index = Consumer->Hosted.IndexOfByPredicate(
				[&Screen](const TPair<TSharedPtr<SWidget>, int32>& Hosted)
				{
					return Hosted.Key == Screen;
				});
			if (Index == INDEX_NONE || Consumer->HostViewport.Get() != &Viewport)
			{
				continue;
			}
			Consumer->Hosted.RemoveAt(Index);
			if (Consumer->Callbacks.UIHost.IsSet() && Consumer->Callbacks.UIHost->Hide)
			{
				Consumer->Callbacks.UIHost->Hide(Screen);
			}
			return true;
		}
	}
	return false;
}

EWoTUIContent FWoTLocalPlayerHooks::ClassifyUIContent(const SWidget& Screen)
{
	// The game's own screens by widget type; anything else was added by an
	// optional module.
	static const TPair<FName, EWoTUIContent> Kinds[] =
	{
		{FName(TEXT("SWoTFrontEndRoot")), EWoTUIContent::FrontEnd},
		{FName(TEXT("SWoTBootstrapScreen")), EWoTUIContent::Startup},
		{FName(TEXT("SWoTInGameMenuOverlay")), EWoTUIContent::InGameMenu},
		{FName(TEXT("SWoTHelpOverlay")), EWoTUIContent::Help},
		{FName(TEXT("SWoTItemInfoOverlay")), EWoTUIContent::ItemInfo},
		{FName(TEXT("SWoTChatInputOverlay")), EWoTUIContent::Chat},
		{FName(TEXT("SWoTSharingNotice")), EWoTUIContent::Notice},
	};
	const FName Type = Screen.GetType();
	for (const TPair<FName, EWoTUIContent>& Kind : Kinds)
	{
		if (Kind.Key == Type)
		{
			return Kind.Value;
		}
	}
	return EWoTUIContent::Extension;
}
