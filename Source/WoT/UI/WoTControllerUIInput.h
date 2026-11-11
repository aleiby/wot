#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/NavigationConfig.h"

class FSlateApplication;
class SWidget;
class SWindow;

/**
 * Which device the player last used in the menus (spec r3 §1, "Last input").
 * Controller: a gamepad button, or a stick past 0.35. Pointer: a real mouse
 * move of more than 4 px from where Controller mode began, a mouse button,
 * touch, or any keyboard key (except while a text entry is active).
 */
enum class EWoTLastInput : uint8
{
	Pointer,
	Controller
};

DECLARE_MULTICAST_DELEGATE_OneParam(FWoTLastInputChangedEvent, EWoTLastInput);

/**
 * The cursor side effects of the lease, behind one seam so specs never touch
 * the real cursor (spec r3 §1, "Cursor lease").
 */
class WOT_API IWoTCursorPolicy
{
public:
	virtual ~IWoTCursorPolicy() = default;
	/** FSlateUser::SetCursorVisibility on the cursor user. */
	virtual void SetCursorVisible(bool bVisible) = 0;
	virtual bool AreSyntheticCursorMovesEnabled() const = 0;
	/** The Slate.EnableSyntheticCursorMoves console variable. */
	virtual void SetSyntheticCursorMovesEnabled(bool bEnabled) = 0;
	/** One synthetic off-window mouse move, so every widget leaves its hover.
	 * The OS cursor does not move. */
	virtual void ClearHover() = 0;
	/** Where the pointer is now, when known. */
	virtual TOptional<FVector2D> GetCursorPosition() const = 0;
};

/** The production policy: the real Slate cursor user and console variable. */
class WOT_API FWoTSlateCursorPolicy final : public IWoTCursorPolicy
{
public:
	virtual void SetCursorVisible(bool bVisible) override;
	virtual bool AreSyntheticCursorMovesEnabled() const override;
	virtual void SetSyntheticCursorMovesEnabled(bool bEnabled) override;
	virtual void ClearHover() override;
	virtual TOptional<FVector2D> GetCursorPosition() const override;

	/** The synthetic off-window move ClearHover sends. */
	static void SendOffWindowMouseMove();
};

#if WITH_AUTOMATION_TESTS
/**
 * Records what the lease asked for. With bApplySlateSideEffects it also sends
 * the off-window move and sets the synthetic-moves console variable (never
 * the OS cursor), so a spec can tick real Slate frames.
 */
class WOT_API FWoTRecordingCursorPolicy final : public IWoTCursorPolicy
{
public:
	virtual ~FWoTRecordingCursorPolicy() override;
	virtual void SetCursorVisible(bool bVisible) override;
	virtual bool AreSyntheticCursorMovesEnabled() const override;
	virtual void SetSyntheticCursorMovesEnabled(bool bEnabled) override;
	virtual void ClearHover() override;
	virtual TOptional<FVector2D> GetCursorPosition() const override;

	bool bCursorVisible = true;
	bool bSyntheticMoves = true;
	int32 ClearHoverCount = 0;
	TOptional<FVector2D> CursorPosition;
	bool bApplySlateSideEffects = false;
	TArray<FString> Calls;

private:
	TOptional<bool> RealSyntheticMovesBefore;
};
#endif

/**
 * Held by every modal UI surface that wants a pointer: the front end, the
 * in-game menu and its pages, notices and the bootstrap screen. While any
 * lease is held and the last input is Controller, the cursor is hidden,
 * synthetic cursor moves are off, sub-4px real mouse moves are consumed and
 * hover is cleared once. Pointer mode, or the last lease ending, restores it.
 */
class WOT_API FWoTCursorLease : public FNoncopyable
{
public:
	~FWoTCursorLease();

private:
	friend class FWoTControllerUIInput;
	FWoTCursorLease() = default;
};

/** Keeps one game viewport scope registered (see FWoTControllerUIInput::Register). */
class WOT_API FWoTControllerUIInputRegistration : public FNoncopyable
{
public:
	~FWoTControllerUIInputRegistration();

private:
	friend class FWoTControllerUIInput;
	explicit FWoTControllerUIInputRegistration(uint64 InId) : Id(InId) {}
	uint64 Id = 0;
};

/**
 * Analog navigation that fires only when a stick crosses the 0.5 threshold.
 * The engine's analog repeat is therefore off; FWoTControllerUIInput repeats
 * on its own clock (spec r3 §1, "Navigation repeat").
 */
class WOT_API FWoTNavigationConfig : public FNavigationConfig
{
public:
	FWoTNavigationConfig();
	virtual EUINavigation GetNavigationDirectionFromAnalog(
		const FAnalogInputEvent& InAnalogEvent) override;

	/**
	 * Fed every analog event before Slate routes it. Returns the direction
	 * when this event newly crosses the threshold, else Invalid. A crossing
	 * nobody navigated on (a press that began in gameplay) is never replayed.
	 */
	EUINavigation ObserveAnalog(int32 UserIndex, const FKey& Key, float Value);

	/** The stick direction past the threshold for this axis value, or Invalid. */
	EUINavigation DirectionForAnalog(const FKey& Key, float Value) const;
	bool IsNavigationAxis(const FKey& Key) const;

private:
	struct FAxisState
	{
		EUINavigation Held = EUINavigation::Invalid;
		bool bFresh = false;
	};
	TMap<TPair<int32, FName>, FAxisState> Axes;
	bool bObserved = false;
};

/**
 * The controller UI input foundation (spec r3 §1): one Slate input
 * preprocessor registered at PreGame for the game instance's viewport window
 * family. It tracks the last input, applies the cursor lease, and runs the
 * per-user navigation repeat for the D-pad and stick: first repeat after
 * 0.35 s, then every 0.08 s, armed only by a press that starts while a
 * back-stack surface has focus, cancelled on release and whenever the
 * back-stack top changes. Native repeats of D-pad keys are dropped. Repeats
 * are dispatched as Controller-genesis navigation through
 * ProcessExternalReply, so every widget's OnNavigation runs. It never
 * consumes a keyboard key, so keyboard play and chat are untouched.
 */
class WOT_API FWoTControllerUIInput final : public IInputProcessor
{
public:
	using FClock = TFunction<double()>;
	/** A window (its family is in scope) or any other widget (in scope while
	 * it is in the focus path: the editor's game viewport, which shares the
	 * editor's window during PIE). */
	using FScopeResolver = TFunction<TSharedPtr<SWidget>()>;

	static constexpr float StickControllerThreshold = 0.35f;
	static constexpr float PointerMoveThreshold = 4.0f;
	static constexpr double FirstRepeatDelay = 0.35;
	static constexpr double RepeatInterval = 0.08;

	// Process-wide last-input state: widgets read it for hover looks and
	// initial focus.
	static EWoTLastInput GetLastInput();
	static bool IsControllerMode() { return GetLastInput() == EWoTLastInput::Controller; }
	static FWoTLastInputChangedEvent& OnLastInputChanged();

	/**
	 * Stub for the PR 3 text entry: while an entry is active, keyboard keys
	 * (physical typing into the panel, Steam's keyboard) do not flip the
	 * last input to Pointer.
	 */
	static void SetTextEntryActive(bool bActive);
	static bool IsTextEntryActive();

	/** Hold for as long as a modal UI surface is shown. */
	[[nodiscard]] static TSharedRef<FWoTCursorLease> AcquireCursorLease();
	static int32 GetCursorLeaseCount();
	/** True while a lease is held in Controller mode (the cursor is hidden). */
	static bool IsCursorSuppressed();

	/** Game instances register outside dedicated servers and automation runs
	 * (automation constructs no menus and must keep the editor's input). */
	static bool ShouldRegisterForGameInstance(bool bDedicatedServer,
		bool bSlateInitialized, bool bAutomationTesting);

	/**
	 * Registers one scope: the preprocessor handles events whose focus is in
	 * that scope. The first registration installs the
	 * preprocessor and FWoTNavigationConfig; dropping the last one removes
	 * them and restores the previous navigation config. A clock replaces the
	 * repeat clock (specs inject one).
	 */
	[[nodiscard]] static TSharedPtr<FWoTControllerUIInputRegistration> Register(
		FScopeResolver Scope, FClock Clock = FClock());
	/** The registered preprocessor, if any. */
	static TSharedPtr<FWoTControllerUIInput> GetRegistered();

	FWoTControllerUIInput();
	virtual ~FWoTControllerUIInput() override;

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp,
		TSharedRef<ICursor> Cursor) override;
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp,
		const FKeyEvent& InKeyEvent) override;
	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp,
		const FKeyEvent& InKeyEvent) override;
	virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp,
		const FAnalogInputEvent& InAnalogInputEvent) override;
	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp,
		const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp,
		const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp,
		const FPointerEvent& MouseEvent) override;
	virtual const TCHAR* GetDebugName() const override
	{
		return TEXT("WoTControllerUIInput");
	}

	/** Runs the repeat state machine against the clock (Tick calls it). */
	void TickRepeat(FSlateApplication& SlateApp);
	bool IsRepeatArmed(int32 UserIndex) const;
	TSharedRef<FWoTNavigationConfig> GetNavigationConfig() const { return NavigationConfig; }

#if WITH_AUTOMATION_TESTS
	/** Replaces the cursor policy (null restores production). */
	static void SetCursorPolicyForTesting(TSharedPtr<IWoTCursorPolicy> Policy);
	static void SetLastInputForTesting(EWoTLastInput Input);
	/** Pointer mode, no text entry, production policy. */
	static void ResetForTesting();
#endif

private:
	friend class FWoTCursorLease;
	friend class FWoTControllerUIInputRegistration;

	struct FRepeat
	{
		EUINavigation Direction = EUINavigation::Invalid;
		FKey Key;
		double NextFireTime = 0.0;
		uint64 TopEntryId = 0;
	};

	static void SetLastInput(EWoTLastInput Input);
	static void ReconcileCursor();
	static void ReleaseCursorLease();
	static void Unregister(uint64 Id);

	bool IsInScope(FSlateApplication& SlateApp, int32 UserIndex) const;
	double Now() const;
	void Arm(int32 UserIndex, EUINavigation Direction, const FKey& Key, uint64 TopEntryId);
	void Cancel(int32 UserIndex);

	TArray<TPair<uint64, FScopeResolver>> Scopes;
	FClock Clock;
	TSharedRef<FWoTNavigationConfig> NavigationConfig;
	TSharedPtr<FNavigationConfig> PreviousNavigationConfig;
	TMap<int32, FRepeat> Repeats;
};
