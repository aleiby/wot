#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraTypes.h"
#include "Engine/EngineTypes.h"
#include "InputCoreTypes.h"
#include "WoTHUDSnapshot.h"

class AActor;
class APawn;
class APlayerController;
class SWidget;
class UGameViewportClient;
class UInputComponent;
class ULocalPlayer;
class UWorld;
struct FKey;

// Per-local-player providers for input and presentation: an optional consumer
// attaches to a local player, independent of its controller and pawn, and the
// game module routes its aim, movement, view, HUD, UI and command paths
// through the slots the consumer fills. A separate concern from the process-wide menu and
// asset seams (WoTOnboardingSeams.h). Plain engine types only; game thread only.
// Nothing is allocated until the first Attach, and with nothing attached every
// path behaves exactly as without these hooks.

/** Lifecycle notifications. Attached is always a consumer's first event and
 * Detached its last. Callbacks may attach or detach consumers. */
enum class EWoTLocalPlayerEvent : uint8
{
	Attached,
	Detached,
	/** The local player's controller changed: replaced, or ended (none). */
	ControllerChanged,
	/** A pawn became the gameplay anchor (also sent on attach while possessed). */
	Possessed,
	Unpossessed,
	/** The current world is being left (travel, disconnect or shutdown). */
	Travel,
	/** The pawnless front-end controller is current (also sent on attach there). */
	FrontEnd,
	Death,
	/** The player re-entered play after death; the new pawn arrives as Possessed. */
	Respawn,
	MatchEnded,
	/** The possessed pawn was relocated discontinuously: moved in one step by
	 * more than FWoTLocalPlayerHooks::TeleportDistance, either by a gameplay
	 * relocation on this machine (a teleporter, a place swap, a pit, the citadel
	 * editor's return) or by an authoritative correction on an owning client,
	 * which is how a remote client learns of the server's relocations. The same
	 * rule holds on a standalone or listen host and on a remote client.
	 * Continuous motion never qualifies, scripted or not: the spinning
	 * artifact's lift and knockback move the pawn far less than that between two
	 * observations. Sent once, after the whole relocation (including any heading
	 * it sets), before the aim is sampled again and at the latest at the start
	 * of the next player tick. */
	Teleported,
	/** Held input must be released: a modal opened, the input context changed
	 * or input focus was lost. */
	InputContextChanged,
	/** Gameplay started turning the view by force (the spinning artifact's
	 * lift): the aim keeps turning, and the gameplay yaw keeps growing, until
	 * ForcedTurnEnded. */
	ForcedTurnStarted,
	ForcedTurnEnded
};

struct FWoTLocalPlayerContext
{
	TWeakObjectPtr<ULocalPlayer> Player;
	TWeakObjectPtr<APlayerController> Controller;
	TWeakObjectPtr<APawn> Pawn;
	TWeakObjectPtr<UWorld> World;
};

struct FWoTLocalViewPolicy
{
	/** Removes synthetic camera motion from the view: walk, swim and landing bob,
	 * eye-height smoothing, vertical shake and roll shake. Their state keeps
	 * running, and every gameplay effect (damage, impulses, forced rotation, fog,
	 * flash, field of view) still applies. */
	bool bSuppressSyntheticMotion = false;
	/** Runs on the final view, after the camera and its modifiers. Called several
	 * times per frame, once for every view the engine builds (the scene view and
	 * each projection, so at least once per eye in stereo): it must be free of
	 * side effects and give the same pose for the same frame. Only the pose is
	 * writable; the view keeps its field of view, post-process (fog, flash) and
	 * special-view state for the consumer to read. */
	TFunction<void(const FWoTLocalPlayerContext&, const FMinimalViewInfo&,
		FVector& /*Location*/, FRotator& /*Rotation*/)> Compose;
	/** Positive near-plane distance in world units, applied to every view's
	 * projection, including projections a stereo device supplies. Called as often
	 * as Compose; it must be free of side effects too. */
	TFunction<float()> NearClip;
};

/** The client screens a UI host can show (H6). */
enum class EWoTUIContent : uint8
{
	/** The front end: its menus, the server browser, class and team choice,
	 * settings. */
	FrontEnd,
	/** The startup and progress screen before the front end. */
	Startup,
	/** The in-game menu and its pages (settings, controls, chat and taunts). */
	InGameMenu,
	Help,
	/** The selected item's information window. */
	ItemInfo,
	/** The chat line and its keyboard panel. */
	Chat,
	/** A notice over any screen. */
	Notice,
	/** A screen an optional module added: the onboarding guide, its cards and
	 * recaps. */
	Extension
};

/** Shows the game's Slate screens somewhere other than the game viewport
 * (H6), such as an offscreen surface. Screens keep their own input, focus
 * and window rules; only where they draw changes. */
struct FWoTUIHost
{
	/** Take a screen: called on attach for every screen already up (bottom
	 * first) and as each one opens. Return false to leave it on the viewport. */
	TFunction<bool(const TSharedRef<SWidget>& /*Screen*/, int32 /*ZOrder*/,
		EWoTUIContent)> Show;
	/** A taken screen closed, or the host is detaching: let it go. On detach
	 * each screen returns to the viewport at its z-order. */
	TFunction<void(const TSharedRef<SWidget>& /*Screen*/)> Hide;
	/** The window the taken screens live in: an SWindow Slate knows (an
	 * offscreen one registered with FSlateApplication::RegisterVirtualWindow),
	 * so focus paths and the hit test reach the screens. While attached it is
	 * part of the controller UI input scope (navigation repeat, last input,
	 * the cursor lease); focus anywhere else keeps the scope's own rules. */
	TFunction<TSharedPtr<SWidget>()> Scope;
	/** The size, in pixels, of the surface the taken screens lay out on. A
	 * screen that opens while this host is attached is sized for it from its
	 * first frame, as it would be for the viewport's own size
	 * (FWoTLocalPlayerHooks::UIScreenSize). Unset, or zero, leaves screens
	 * sized for the viewport. */
	TFunction<FIntPoint()> ScreenSize;
	/** The host offers text entry of its own; false keeps every entry on the
	 * game's keyboard panel. */
	bool bRuntimeTextEntry = false;
};

enum class EWoTUIPointerAction : uint8
{
	Move,
	Down,
	Up,
	DoubleClick,
	/** The pointer leaves the host's window (FWoTLocalPlayerHooks::RoutePointer):
	 * every widget it hovered hears its leave. */
	Leave
};

struct FWoTLocalPlayerConsumer
{
	TFunction<void(EWoTLocalPlayerEvent, const FWoTLocalPlayerContext&)> Lifecycle;
	/** Called on attach (with a null component when no controller exists yet) and
	 * whenever a controller of this player builds its input component, before
	 * the controller binds its own actions. */
	TFunction<void(ULocalPlayer&, UInputComponent*)> RegisterInput;
	/** Exclusive. Absolute yaw and pitch for the simulation aim (roll is ignored),
	 * sampled before every cast, launch and rotation update. The aim is the
	 * supplied yaw plus the gameplay yaw (FWoTLocalPlayerHooks::PeekGameplayYaw)
	 * and the supplied pitch, constrained like mouse aim (view limits, gameplay
	 * pitch constraints). Mouse and stick look are ignored while attached. The
	 * citadel editor's orbit view is not aim: it never samples the provider, and
	 * look input keeps turning its camera. Return false to hold the current aim. */
	TFunction<bool(FRotator&)> AimRotation;
	/** Exclusive (the movement slot). The yaw that ground movement input uses
	 * instead of the control yaw; swimming keeps the view direction. */
	TFunction<bool(double&)> GroundMovementYaw;
	/** Exclusive (the movement slot). A world-space movement input, where a
	 * length of 1 is a full stick deflection. It is scaled like the forward stick
	 * in the current movement state, summed with the stick and keyboard input and
	 * limited to the larger of their own input and a full forward stick, then
	 * follows the ordinary movement path and transport. No position, velocity,
	 * speed or teleport authority. Ignored whenever the stick is: while movement
	 * input is ignored, while a window holds UI-only input (the in-game menu, help,
	 * item information, chat, an onboarding window), after the match ends, in the
	 * citadel orbit view and for a dead or constrained body. */
	TFunction<FVector()> ExtraMovement;
	/** Exclusive. View ownership. */
	TOptional<FWoTLocalViewPolicy> View;
	/** H5. Called once per HUD update (once per local-player frame while the
	 * HUD draws, wherever it is presented) with what the HUD shows. Read-only;
	 * copy what is needed later. Observers coexist. */
	TFunction<void(const FWoTHUDSnapshot&)> HUDObserver;
	/** H5, exclusive (the HUD destination). While attached the flat Canvas HUD
	 * is not drawn: this receives each update's snapshot to present, arranged
	 * by FWoTHUDLayout and drawn on a canvas of the consumer's own (one texture
	 * for both eyes), never through the HUD's Canvas. Detaching restores the
	 * flat HUD at its next draw. */
	TFunction<void(const FWoTHUDSnapshot&)> HUDDestination;
	/** H6, exclusive (the UI host). */
	TOptional<FWoTUIHost> UIHost;
};

enum class EWoTLocalPlayerCommand : uint8
{
	/** Press and release the Fire binding (casts, respawns, restarts). */
	Fire, CeaseFire,
	/** Press and release the Jump binding (jump, held swim-up, respawn). */
	Jump, StopJumping,
	NextItem, PreviousItem, NextGroup, PreviousGroup,
	/** Index: the group (hand) whose key to press, 0 for the first. A neutral
	 * player's key chooses a team while the team prompt is up, as the hand keys do. */
	SelectGroup,
	/** Index: the slot to select within the selected group; only held items. */
	SelectSlot,
	/** Hold the scoreboard; HideScores restores the visibility from before the hold. */
	ShowScores, HideScores,
	/** The in-game menu key. Opening the menu releases this consumer's held commands. */
	Menu
};

struct FWoTSelectionSlot
{
	int32 Group = INDEX_NONE;
	int32 Slot = INDEX_NONE;
	FName ClassName;
	int32 Charges = 0;
	bool bOwned = false;
	bool bSelected = false;
};

struct FWoTSelectionSnapshot
{
	int32 SelectedGroup = INDEX_NONE;
	int32 SelectedSlot = INDEX_NONE;
	/** Authored slots in group/slot order, including unavailable entries. */
	TArray<FWoTSelectionSlot> Slots;
};

struct WOT_API FWoTLocalPlayerHooks
{
	/** A relocation or correction that moves the possessed pawn farther than
	 * this (world units; about 7.3 m at 52.5 units per metre) reports Teleported.
	 * It is the distance beyond which the engine stops smoothing a remote
	 * character and snaps it to the server's location (the default
	 * UCharacterMovementComponent::NetworkNoSmoothUpdateDistance). Continuous
	 * motion stays well below it between two observations: a lift tick or a
	 * knockback correction moves a pawn a few units, a lift correction on a
	 * remote client a few tens. The shortest teleporter pair in the shipped
	 * maps is 629 units apart and pit landings are farther. A shorter
	 * relocation (a place swap with a near neighbour) carries the view like any
	 * other movement, and the heading it sets still reaches the aim as gameplay
	 * yaw. */
	static constexpr double TeleportDistance = 384.0;

	/** Atomic: a duplicate owner, or a second consumer for an occupied aim,
	 * movement, view, HUD destination or UI host slot, rejects the whole
	 * registration with a warning, whatever the order of attachment. Consumers
	 * that fill no exclusive slot coexist freely. Detach before the consumer's
	 * module shuts down. */
	static bool Attach(ULocalPlayer& Player, FName Owner, const FWoTLocalPlayerConsumer& Consumer);
	/** Releases the consumer's held commands, then sends Detached. The aim stays
	 * where it is, so flat look continues from it. */
	static void Detach(ULocalPlayer& Player, FName Owner);
	static void DetachAll(ULocalPlayer& Player);
	static bool HasConsumers(const ULocalPlayer* Player);

	/** The gameplay yaw in degrees, (-180, 180]: every change to the aim's yaw
	 * that the hooks did not make while the provider owned the aim. A forced
	 * turn, a teleporter's exit heading, a respawn or possession facing, a place
	 * swap, a rotating base and the match-end view keep applying to the aim, as
	 * they would to a mouse player's: the aim is the provider's yaw plus this
	 * offset. Zero without an aim provider. */
	static double PeekGameplayYaw(const ULocalPlayer& Player);
	/** For the aim owner: returns the gameplay yaw and zeroes it (zero for any
	 * other owner). Adding the result to the provider's own heading leaves the
	 * aim where it is and brings the view back into line with it. During a
	 * forced turn, leave it: the rendered head stays unrotated while the aim
	 * shows the turn, and the offset is taken once ForcedTurnEnded arrives. */
	static double TakeGameplayYaw(ULocalPlayer& Player, FName Owner);
	/** Whether gameplay is turning this player's view by force (between
	 * ForcedTurnStarted and ForcedTurnEnded). */
	static bool IsForcedTurn(const ULocalPlayer& Player);
	/** For the movement owner: holds the local player's body still (bHold) or
	 * lets it go. While held, movement input is ignored as when the controller
	 * ignores move input, for whichever controller and pawn the player has, so a
	 * respawn's input reset does not lift it; and the controller's own ignore
	 * count is never touched, so lifting it never lifts the game's own freezes
	 * (the match end). Detach lifts it. Ignored for any other owner. */
	static void HoldMovement(ULocalPlayer& Player, FName Owner, bool bHold);
	/** Whether the movement owner holds this controller's local player still. */
	static bool IsMovementHeld(const APlayerController* Controller);
	/** Whether gameplay keys and sticks are taken from this local player's
	 * controller right now: a window holds UI-only input (the in-game menu,
	 * help, item information, an onboarding window) or chat is being typed
	 * (which still passes stick values through). A consumer's own gameplay
	 * input, such as turning, follows the same gates. False without a
	 * controller of a local player. */
	static bool IsGameInputBlocked(const APlayerController* Controller);

	// Queries the game module uses on its own paths.
	static bool OwnsAim(const APlayerController* Controller);
	/** Starts a rotation update of Controller: delivers a pending relocation,
	 * then adds the yaw change made since the last update to the gameplay yaw.
	 * True when a provider owns the aim. */
	static bool BeginAimUpdate(APlayerController& Controller);
	/** The provider's aim (its pitch, its yaw plus the gameplay yaw), keeping
	 * Rotation's roll. False to hold the current aim. */
	static bool SampleAim(APlayerController& Controller, FRotator& Rotation);
	/** Ends a rotation update: records the control yaw the hooks leave, after view
	 * limits, pawn facing and gameplay constraints, so those never read as
	 * gameplay yaw. */
	static void EndAimUpdate(APlayerController& Controller);
	static double GroundYaw(const APlayerController* Controller, double Fallback);
	/** The raw extra movement input for a locally controlled player pawn, or zero
	 * when nothing supplies one or the stick would be ignored. */
	static FVector ExtraMovement(const APawn& Pawn);
	static bool SuppressSyntheticMotion(const APlayerController* Controller);
	static void ComposeView(const ULocalPlayer& Player, FMinimalViewInfo& View);
	static TOptional<float> NearClip(const ULocalPlayer& Player);
	/** Apply an owned near plane to an engine reversed-Z perspective projection,
	 * retaining its eye asymmetry, field of view and finite/infinite far plane. */
	static void ApplyNearClip(const ULocalPlayer& Player, FMatrix& Projection);

	/** Invoke a command through the same controller and pawn handlers as its key
	 * binding, with that binding's gates. A press does nothing while a window
	 * holds UI-only input, and while the game is paused only Jump (whose handler
	 * turns a paused press into an unpause request) and Menu run, as only their
	 * bindings run when paused. Behind one of the game's own windows (the in-game
	 * menu, help, item information, chat) Menu closes it, as the key does there;
	 * behind any other window it does nothing. Releases (CeaseFire, StopJumping,
	 * HideScores) always run, as every release binding does. False only when
	 * Owner is not attached or no gameplay controller exists; a command that a
	 * gate or its handler ignores still returns true. */
	static bool Command(ULocalPlayer& Player, FName Owner, EWoTLocalPlayerCommand Command, int32 Index = 0);
	/** Release Owner's held Fire, Jump and scoreboard hold, once each. */
	static void ReleaseCommands(ULocalPlayer& Player, FName Owner);
	/** The selected pawn's groups and slots in selection order, read-only. */
	static FWoTSelectionSnapshot Selection(ULocalPlayer& Player);

	// H5: the HUD as data.
	/** The player's HUD after its last update, or null (no HUD yet, or a
	 * HUD that has not drawn). Read it after the update: from HUDObserver or
	 * HUDDestination, or later in the frame. */
	static const FWoTHUDSnapshot* LatestHUD(const ULocalPlayer& Player);

	// H6: the UI host's input. One Slate user (UISlateUser) carries the
	// player's pointer, focus, navigation, confirm, back and text entry.
	/** The Slate user index of the player's UI. */
	static int32 UISlateUser(const ULocalPlayer& Player);
	/** The Slate pointer index of a host's pointer (RoutePointer): past every
	 * index the platform gives a finger (ETouchIndex Touch1 to Touch10) or the
	 * mouse (FSlateApplication::CursorPointerIndex). The player's one Slate
	 * user keeps the desktop mouse and the host's pointer apart: each has its
	 * own hover, capture and press, so moving the mouse over the desktop
	 * window neither unhovers what the host's pointer is on nor reaches a
	 * press it holds. */
	static constexpr uint32 UIPointerIndex = ETouchIndex::MAX_TOUCHES;
	/** The host's pointer (a controller's laser) at Position in the host's
	 * window (its Scope, an SWindow such as a virtual window; its own
	 * coordinates, as it last painted). The window's hit test finds the
	 * widgets under it, and the event is routed to them as the player's Slate
	 * user, on UIPointerIndex, with the left button as its button. Only the
	 * host's window is ever hit: the pointer has no world-use or collision
	 * authority. A primary press on a text field starts that field's
	 * controller text entry. A release always ends the press: where nothing is
	 * hit it reaches no widget and the pointer gives up its capture, so it is
	 * never a click. False unless Owner hosts UI with a window scope.
	 *
	 * Hover is per pointer: Slate unhovers a widget only when a move of the
	 * same pointer no longer finds it (FSlateApplication::RoutePointerMoveEvent).
	 * Leave (Position is ignored) is that move for a pointer that has left the
	 * window, such as a laser or gaze gone off the surface: a synthetic move of
	 * UIPointerIndex to a point outside every window, where nothing is hit, so
	 * every widget the pointer hovered hears its leave and none hears a move or
	 * a click. While the pointer's press is held (a Down or DoubleClick not yet
	 * released) Leave is refused: it returns false and sends nothing, as a move
	 * would reach the widget holding the capture (a captured slider would jump
	 * to the far point). End the press first (an Up where nothing is hit ends
	 * it without a click), then Leave. Detaching the host sends the same leave
	 * once the pointer has given up its capture, after the current input when
	 * the host detaches from inside Slate's input handling. */
	static bool RoutePointer(ULocalPlayer& Player, FName Owner, const FVector2D& Position,
		EWoTUIPointerAction Action);
	/** The host's buttons (confirm, back, the D-pad's navigation) through
	 * Slate's own key route as the player's Slate user, while the user's focus
	 * is inside the host's scope; false (nothing sent) otherwise. A key the
	 * host leaves pressed is released when the host detaches. */
	static bool RouteKey(ULocalPlayer& Player, FName Owner, const FKey& Key, bool bPressed,
		bool bRepeat = false);
	/** The host's sticks (navigation past the threshold) through Slate's own
	 * analog route, under RouteKey's focus rule. A stick the host leaves
	 * deflected returns to rest when the host detaches, and a pointer it leaves
	 * pressed comes up and gives up its capture. */
	static bool RouteAnalog(ULocalPlayer& Player, FName Owner, const FKey& Key, float Value);
	/** Whether a host's pointer event for this Slate user is being routed now. */
	static bool IsRoutingUIPointer(int32 SlateUserIndex);
	/** Whether text entry must use the game's keyboard panel: a host without
	 * text entry of its own is attached. */
	static bool UsesPanelTextEntry();
	/** Whether a text entry runs (the keyboard panel or a runtime keyboard). */
	static bool IsTextEntryActive();
	/** The size, in pixels, a screen opening on Viewport lays out for: the
	 * surface of a UI host attached for a player of that viewport, when the
	 * host gives one (FWoTUIHost::ScreenSize), else the viewport's own size
	 * (UGameViewportClient::GetViewportSize). */
	static FVector2D UIScreenSize(const UGameViewportClient& Viewport);

	// Game integration. None of these allocates a registration.
	static void PrepareInput(APlayerController& Controller, UInputComponent* Input);
	static void Reconcile(APlayerController& Controller);
	static void Notify(APlayerController& Controller, EWoTLocalPlayerEvent Event);
	/** Before a possession change: release held commands toward the pawn that is
	 * still possessed (the gamepad layer's possession rule). */
	static void PawnChanging(APlayerController& Controller);
	/** A gameplay relocation of Actor on this machine, after the move. A local
	 * player's pawn that moved farther than TeleportDistance from where the hooks
	 * last saw it reports Teleported. */
	static void PawnRelocated(AActor* Actor);
	/** An owning client's authoritative correction: the client's own location
	 * for the corrected move and the server's. */
	static void PawnCorrected(APawn& Pawn, const FVector& ClientLocation,
		const FVector& ServerLocation);
	static void ControllerEnded(APlayerController& Controller, EEndPlayReason::Type Reason);
	/** The local player object is being destroyed: forget it without callbacks. */
	static void PlayerDestroyed(const ULocalPlayer& Player);
	/** H5: whether a consumer owns this controller's HUD destination (the flat
	 * Canvas HUD is then not drawn). */
	static bool OwnsHUDDestination(const APlayerController* Controller);
	/** H5: the HUD updated: observers and the destination read the snapshot. */
	static void HUDUpdated(APlayerController& Controller, const FWoTHUDSnapshot& Snapshot);
	/** H6: a screen is going onto Viewport: true when a UI host took it. */
	static bool HostUIContent(UGameViewportClient& Viewport, const TSharedRef<SWidget>& Screen,
		int32 ZOrder);
	/** H6: a screen is coming off Viewport: true when it was hosted (and the
	 * host let it go). */
	static bool UnhostUIContent(UGameViewportClient& Viewport, const TSharedRef<SWidget>& Screen);
	/** H6: what kind of screen this is. */
	static EWoTUIContent ClassifyUIContent(const SWidget& Screen);
};
