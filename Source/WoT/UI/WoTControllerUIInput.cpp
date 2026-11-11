#include "UI/WoTControllerUIInput.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Input/Events.h"
#include "Input/Reply.h"
#include "Layout/WidgetPath.h"
#include "UI/WoTBackStack.h"
#include "Widgets/SWindow.h"

namespace
{
// Process-wide: the last input, the leases and the registered preprocessor.
// Widgets read the mode without knowing which game instance registered.
struct FWoTControllerUIGlobals
{
	EWoTLastInput LastInput = EWoTLastInput::Pointer;
	FWoTLastInputChangedEvent Changed;
	bool bTextEntryActive = false;
	int32 LeaseCount = 0;
	bool bCursorSuppressed = false;
	bool bSyntheticMovesBefore = true;
	TOptional<FVector2D> PointerAnchor;
	TSharedPtr<IWoTCursorPolicy> PolicyOverride;
	TSharedPtr<IWoTCursorPolicy> ProductionPolicy;
	TSharedPtr<FWoTControllerUIInput> Registered;
	uint64 NextScopeId = 1;
};

FWoTControllerUIGlobals& WoTControllerUIGlobals()
{
	static FWoTControllerUIGlobals Globals;
	return Globals;
}

IWoTCursorPolicy& WoTControllerUIPolicy()
{
	FWoTControllerUIGlobals& Globals = WoTControllerUIGlobals();
	if (Globals.PolicyOverride.IsValid())
	{
		return *Globals.PolicyOverride;
	}
	if (!Globals.ProductionPolicy.IsValid())
	{
		Globals.ProductionPolicy = MakeShared<FWoTSlateCursorPolicy>();
	}
	return *Globals.ProductionPolicy;
}

IConsoleVariable* WoTSyntheticCursorMovesVariable()
{
	return IConsoleManager::Get().FindConsoleVariable(
		TEXT("Slate.EnableSyntheticCursorMoves"));
}

bool WoTIsWindowInFamily(TSharedPtr<SWindow> Window, const TSharedPtr<SWindow>& Family)
{
	for (; Window.IsValid(); Window = Window->GetParentWindow())
	{
		if (Window == Family)
		{
			return true;
		}
	}
	return false;
}

EUINavigation WoTDPadDirection(const FKey& Key)
{
	if (Key == EKeys::Gamepad_DPad_Up) return EUINavigation::Up;
	if (Key == EKeys::Gamepad_DPad_Down) return EUINavigation::Down;
	if (Key == EKeys::Gamepad_DPad_Left) return EUINavigation::Left;
	if (Key == EKeys::Gamepad_DPad_Right) return EUINavigation::Right;
	return EUINavigation::Invalid;
}

bool WoTIsStickAxis(const FKey& Key)
{
	return Key == EKeys::Gamepad_LeftX || Key == EKeys::Gamepad_LeftY
		|| Key == EKeys::Gamepad_RightX || Key == EKeys::Gamepad_RightY;
}
}

// ---------------------------------------------------------------------------
// Cursor policies

void FWoTSlateCursorPolicy::SetCursorVisible(bool bVisible)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	if (const TSharedPtr<FSlateUser> CursorUser =
		FSlateApplication::Get().GetCursorUser())
	{
		CursorUser->SetCursorVisibility(bVisible);
	}
}

bool FWoTSlateCursorPolicy::AreSyntheticCursorMovesEnabled() const
{
	const IConsoleVariable* Variable = WoTSyntheticCursorMovesVariable();
	return Variable == nullptr || Variable->GetBool();
}

void FWoTSlateCursorPolicy::SetSyntheticCursorMovesEnabled(bool bEnabled)
{
	if (IConsoleVariable* Variable = WoTSyntheticCursorMovesVariable())
	{
		Variable->Set(bEnabled, ECVF_SetByCode);
	}
}

void FWoTSlateCursorPolicy::ClearHover()
{
	SendOffWindowMouseMove();
}

TOptional<FVector2D> FWoTSlateCursorPolicy::GetCursorPosition() const
{
	if (!FSlateApplication::IsInitialized())
	{
		return {};
	}
	return FVector2D(FSlateApplication::Get().GetCursorPos());
}

void FWoTSlateCursorPolicy::SendOffWindowMouseMove()
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FSlateApplication& SlateApp = FSlateApplication::Get();
	// Far outside every window: Slate finds no widget there, so each hovered
	// widget receives its mouse leave. Synthetic, so no preprocessor sees it.
	const FVector2D Away(-1.0e6, -1.0e6);
	const FPointerEvent Event(SlateApp.GetUserIndexForMouse(),
		FSlateApplication::CursorPointerIndex, Away, Away, TSet<FKey>(),
		EKeys::Invalid, 0.0f, SlateApp.GetModifierKeys());
	SlateApp.ProcessMouseMoveEvent(Event, /*bIsSynthetic*/ true);
}

#if WITH_AUTOMATION_TESTS
FWoTRecordingCursorPolicy::~FWoTRecordingCursorPolicy()
{
	if (RealSyntheticMovesBefore.IsSet())
	{
		if (IConsoleVariable* Variable = WoTSyntheticCursorMovesVariable())
		{
			Variable->Set(RealSyntheticMovesBefore.GetValue(), ECVF_SetByCode);
		}
	}
}

void FWoTRecordingCursorPolicy::SetCursorVisible(bool bVisible)
{
	bCursorVisible = bVisible;
	Calls.Add(bVisible ? TEXT("ShowCursor") : TEXT("HideCursor"));
}

bool FWoTRecordingCursorPolicy::AreSyntheticCursorMovesEnabled() const
{
	return bSyntheticMoves;
}

void FWoTRecordingCursorPolicy::SetSyntheticCursorMovesEnabled(bool bEnabled)
{
	bSyntheticMoves = bEnabled;
	Calls.Add(bEnabled ? TEXT("SyntheticMovesOn") : TEXT("SyntheticMovesOff"));
	if (bApplySlateSideEffects)
	{
		if (IConsoleVariable* Variable = WoTSyntheticCursorMovesVariable())
		{
			if (!RealSyntheticMovesBefore.IsSet())
			{
				RealSyntheticMovesBefore = Variable->GetBool();
			}
			Variable->Set(bEnabled, ECVF_SetByCode);
		}
	}
}

void FWoTRecordingCursorPolicy::ClearHover()
{
	++ClearHoverCount;
	Calls.Add(TEXT("ClearHover"));
	if (bApplySlateSideEffects)
	{
		FWoTSlateCursorPolicy::SendOffWindowMouseMove();
	}
}

TOptional<FVector2D> FWoTRecordingCursorPolicy::GetCursorPosition() const
{
	return CursorPosition;
}
#endif

// ---------------------------------------------------------------------------
// Lease and registration handles

FWoTCursorLease::~FWoTCursorLease()
{
	FWoTControllerUIInput::ReleaseCursorLease();
}

FWoTControllerUIInputRegistration::~FWoTControllerUIInputRegistration()
{
	FWoTControllerUIInput::Unregister(Id);
}

// ---------------------------------------------------------------------------
// Navigation config

FWoTNavigationConfig::FWoTNavigationConfig()
{
	// The engine's defaults: the left stick and the D-pad, 0.5 thresholds.
	AnalogNavigationHorizontalThreshold = 0.5f;
	AnalogNavigationVerticalThreshold = 0.5f;
}

bool FWoTNavigationConfig::IsNavigationAxis(const FKey& Key) const
{
	return IsAnalogHorizontalKey(Key) || IsAnalogVerticalKey(Key);
}

EUINavigation FWoTNavigationConfig::DirectionForAnalog(const FKey& Key, float Value) const
{
	if (!bAnalogNavigation)
	{
		return EUINavigation::Invalid;
	}
	if (IsAnalogHorizontalKey(Key))
	{
		if (Value < -AnalogNavigationHorizontalThreshold) return EUINavigation::Left;
		if (Value > AnalogNavigationHorizontalThreshold) return EUINavigation::Right;
	}
	else if (IsAnalogVerticalKey(Key))
	{
		if (Value > AnalogNavigationVerticalThreshold) return EUINavigation::Up;
		if (Value < -AnalogNavigationVerticalThreshold) return EUINavigation::Down;
	}
	return EUINavigation::Invalid;
}

EUINavigation FWoTNavigationConfig::ObserveAnalog(int32 UserIndex, const FKey& Key,
	float Value)
{
	bObserved = true;
	if (!IsNavigationAxis(Key))
	{
		return EUINavigation::Invalid;
	}
	FAxisState& Axis = Axes.FindOrAdd(TPair<int32, FName>(UserIndex, Key.GetFName()));
	const EUINavigation Desired = DirectionForAnalog(Key, Value);
	Axis.bFresh = Desired != EUINavigation::Invalid && Desired != Axis.Held;
	Axis.Held = Desired;
	return Axis.bFresh ? Desired : EUINavigation::Invalid;
}

EUINavigation FWoTNavigationConfig::GetNavigationDirectionFromAnalog(
	const FAnalogInputEvent& InAnalogEvent)
{
	if (!bAnalogNavigation)
	{
		return EUINavigation::Invalid;
	}
	const TPair<int32, FName> AxisKey(InAnalogEvent.GetUserIndex(),
		InAnalogEvent.GetKey().GetFName());
	if (!bObserved)
	{
		// Without the preprocessor in front, track crossings here.
		FAxisState& Axis = Axes.FindOrAdd(AxisKey);
		const EUINavigation Desired =
			DirectionForAnalog(InAnalogEvent.GetKey(), InAnalogEvent.GetAnalogValue());
		Axis.bFresh = Desired != EUINavigation::Invalid && Desired != Axis.Held;
		Axis.Held = Desired;
	}
	// Only the crossing navigates; a held stick never repeats here (several
	// widgets along the bubble path may ask about the same event).
	if (FAxisState* Axis = Axes.Find(AxisKey); Axis && Axis->bFresh)
	{
		Axis->bFresh = false;
		return Axis->Held;
	}
	return EUINavigation::Invalid;
}

// ---------------------------------------------------------------------------
// Process-wide state

EWoTLastInput FWoTControllerUIInput::GetLastInput()
{
	return WoTControllerUIGlobals().LastInput;
}

FWoTLastInputChangedEvent& FWoTControllerUIInput::OnLastInputChanged()
{
	return WoTControllerUIGlobals().Changed;
}

void FWoTControllerUIInput::SetTextEntryActive(bool bActive)
{
	WoTControllerUIGlobals().bTextEntryActive = bActive;
}

bool FWoTControllerUIInput::IsTextEntryActive()
{
	return WoTControllerUIGlobals().bTextEntryActive;
}

double FWoTControllerUIInput::CurrentTime()
{
	const TSharedPtr<FWoTControllerUIInput>& Registered = WoTControllerUIGlobals().Registered;
	return Registered.IsValid() ? Registered->Now() : FPlatformTime::Seconds();
}

TSharedRef<FWoTCursorLease> FWoTControllerUIInput::AcquireCursorLease()
{
	++WoTControllerUIGlobals().LeaseCount;
	ReconcileCursor();
	return MakeShareable(new FWoTCursorLease());
}

int32 FWoTControllerUIInput::GetCursorLeaseCount()
{
	return WoTControllerUIGlobals().LeaseCount;
}

bool FWoTControllerUIInput::IsCursorSuppressed()
{
	return WoTControllerUIGlobals().bCursorSuppressed;
}

void FWoTControllerUIInput::ReleaseCursorLease()
{
	FWoTControllerUIGlobals& Globals = WoTControllerUIGlobals();
	Globals.LeaseCount = FMath::Max(0, Globals.LeaseCount - 1);
	ReconcileCursor();
}

void FWoTControllerUIInput::SetLastInput(EWoTLastInput Input)
{
	FWoTControllerUIGlobals& Globals = WoTControllerUIGlobals();
	if (Globals.LastInput == Input)
	{
		return;
	}
	Globals.LastInput = Input;
	if (Input == EWoTLastInput::Controller)
	{
		// The 4 px rule measures from where Controller mode began.
		Globals.PointerAnchor = WoTControllerUIPolicy().GetCursorPosition();
	}
	else
	{
		Globals.PointerAnchor.Reset();
	}
	ReconcileCursor();
	Globals.Changed.Broadcast(Input);
}

void FWoTControllerUIInput::ReconcileCursor()
{
	FWoTControllerUIGlobals& Globals = WoTControllerUIGlobals();
	const bool bSuppress = Globals.LeaseCount > 0
		&& Globals.LastInput == EWoTLastInput::Controller;
	if (bSuppress == Globals.bCursorSuppressed)
	{
		return;
	}
	IWoTCursorPolicy& Policy = WoTControllerUIPolicy();
	Globals.bCursorSuppressed = bSuppress;
	if (bSuppress)
	{
		Globals.bSyntheticMovesBefore = Policy.AreSyntheticCursorMovesEnabled();
		Policy.SetCursorVisible(false);
		// Every tick otherwise queues a synthetic move at the real OS cursor,
		// which would hover whatever sits under it within a frame or two.
		Policy.SetSyntheticCursorMovesEnabled(false);
		Policy.ClearHover();
	}
	else
	{
		Policy.SetCursorVisible(true);
		Policy.SetSyntheticCursorMovesEnabled(Globals.bSyntheticMovesBefore);
	}
}

#if WITH_AUTOMATION_TESTS
void FWoTControllerUIInput::SetCursorPolicyForTesting(TSharedPtr<IWoTCursorPolicy> Policy)
{
	FWoTControllerUIGlobals& Globals = WoTControllerUIGlobals();
	if (Globals.bCursorSuppressed)
	{
		// Restore through the policy that applied it.
		IWoTCursorPolicy& Previous = WoTControllerUIPolicy();
		Previous.SetCursorVisible(true);
		Previous.SetSyntheticCursorMovesEnabled(Globals.bSyntheticMovesBefore);
		Globals.bCursorSuppressed = false;
	}
	Globals.PolicyOverride = MoveTemp(Policy);
	ReconcileCursor();
}

void FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput Input)
{
	SetLastInput(Input);
}

void FWoTControllerUIInput::ResetForTesting()
{
	SetLastInput(EWoTLastInput::Pointer);
	SetTextEntryActive(false);
	SetCursorPolicyForTesting(nullptr);
}
#endif

// ---------------------------------------------------------------------------
// Registration

bool FWoTControllerUIInput::ShouldRegisterForGameInstance(bool bDedicatedServer,
	bool bSlateInitialized, bool bAutomationTesting)
{
	return !bDedicatedServer && bSlateInitialized && !bAutomationTesting;
}

TSharedPtr<FWoTControllerUIInputRegistration> FWoTControllerUIInput::Register(
	FScopeResolver Scope, FClock InClock)
{
	if (!FSlateApplication::IsInitialized() || !Scope)
	{
		return nullptr;
	}
	FWoTControllerUIGlobals& Globals = WoTControllerUIGlobals();
	if (!Globals.Registered.IsValid())
	{
		const TSharedRef<FWoTControllerUIInput> Input = MakeShared<FWoTControllerUIInput>();
		FSlateApplication& SlateApp = FSlateApplication::Get();
		Input->PreviousNavigationConfig = SlateApp.GetNavigationConfig();
		SlateApp.SetNavigationConfig(Input->NavigationConfig);
		SlateApp.RegisterInputPreProcessor(Input, EInputPreProcessorType::PreGame);
		Globals.Registered = Input;
	}
	if (InClock)
	{
		Globals.Registered->Clock = MoveTemp(InClock);
	}
	const uint64 Id = Globals.NextScopeId++;
	Globals.Registered->Scopes.Emplace(Id, MoveTemp(Scope));
	return MakeShareable(new FWoTControllerUIInputRegistration(Id));
}

void FWoTControllerUIInput::Unregister(uint64 Id)
{
	FWoTControllerUIGlobals& Globals = WoTControllerUIGlobals();
	const TSharedPtr<FWoTControllerUIInput> Input = Globals.Registered;
	if (!Input.IsValid())
	{
		return;
	}
	Input->Scopes.RemoveAll([Id](const TPair<uint64, FScopeResolver>& Entry)
	{
		return Entry.Key == Id;
	});
	if (!Input->Scopes.IsEmpty())
	{
		return;
	}
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication& SlateApp = FSlateApplication::Get();
		SlateApp.UnregisterInputPreProcessor(Input);
		const FNavigationConfig* Installed = &SlateApp.GetNavigationConfig().Get();
		if (Installed == static_cast<const FNavigationConfig*>(&Input->NavigationConfig.Get()))
		{
			SlateApp.SetNavigationConfig(Input->PreviousNavigationConfig.IsValid()
				? Input->PreviousNavigationConfig.ToSharedRef()
				: MakeShared<FNavigationConfig>());
		}
	}
	Globals.Registered.Reset();
}

TSharedPtr<FWoTControllerUIInput> FWoTControllerUIInput::GetRegistered()
{
	return WoTControllerUIGlobals().Registered;
}

// ---------------------------------------------------------------------------
// The preprocessor

FWoTControllerUIInput::FWoTControllerUIInput()
	: NavigationConfig(MakeShared<FWoTNavigationConfig>())
{
}

FWoTControllerUIInput::~FWoTControllerUIInput() = default;

double FWoTControllerUIInput::Now() const
{
	return Clock ? Clock() : FPlatformTime::Seconds();
}

bool FWoTControllerUIInput::IsInScope(FSlateApplication& SlateApp, int32 UserIndex) const
{
	const TSharedPtr<FSlateUser> User = SlateApp.GetUser(UserIndex);
	TSharedPtr<SWindow> Window;
	if (User.IsValid())
	{
		if (const TSharedPtr<SWidget> Focused = User->GetFocusedWidget())
		{
			Window = SlateApp.FindWidgetWindow(Focused.ToSharedRef());
		}
	}
	if (!Window.IsValid())
	{
		Window = SlateApp.GetActiveTopLevelWindow();
	}
	for (const TPair<uint64, FScopeResolver>& Scope : Scopes)
	{
		const TSharedPtr<SWidget> ScopeWidget = Scope.Value ? Scope.Value() : nullptr;
		if (!ScopeWidget.IsValid())
		{
			continue;
		}
		if (ScopeWidget->Advanced_IsWindow())
		{
			if (WoTIsWindowInFamily(Window, StaticCastSharedPtr<SWindow>(ScopeWidget)))
			{
				return true;
			}
		}
		else if (User.IsValid() && User->IsWidgetInFocusPath(ScopeWidget))
		{
			return true;
		}
	}
	return false;
}

void FWoTControllerUIInput::Arm(int32 UserIndex, EUINavigation Direction,
	const FKey& Key, uint64 TopEntryId)
{
	FRepeat& Repeat = Repeats.FindOrAdd(UserIndex);
	Repeat.Direction = Direction;
	Repeat.Key = Key;
	Repeat.NextFireTime = Now() + FirstRepeatDelay;
	Repeat.TopEntryId = TopEntryId;
}

void FWoTControllerUIInput::Cancel(int32 UserIndex)
{
	Repeats.Remove(UserIndex);
}

bool FWoTControllerUIInput::IsRepeatArmed(int32 UserIndex) const
{
	return Repeats.Contains(UserIndex);
}

bool FWoTControllerUIInput::HandleKeyDownEvent(FSlateApplication& SlateApp,
	const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	const int32 UserIndex = InKeyEvent.GetUserIndex();
	if (!IsInScope(SlateApp, UserIndex))
	{
		return false;
	}
	if (Key.IsGamepadKey())
	{
		SetLastInput(EWoTLastInput::Controller);
	}
	else if (!Key.IsMouseButton() && !Key.IsTouch() && !IsTextEntryActive())
	{
		SetLastInput(EWoTLastInput::Pointer);
	}
	const EUINavigation Direction = WoTDPadDirection(Key);
	if (Direction == EUINavigation::Invalid)
	{
		return false;
	}
	const uint64 TopEntryId = FWoTBackStack::Get().FindHandlerId(SlateApp, UserIndex);
	if (InKeyEvent.IsRepeat())
	{
		// XInput repeats held buttons itself; in a menu ours replaces it.
		return TopEntryId != 0;
	}
	if (TopEntryId != 0)
	{
		Arm(UserIndex, Direction, Key, TopEntryId);
	}
	else
	{
		Cancel(UserIndex);
	}
	// The press itself navigates through Slate's ordinary key path.
	return false;
}

bool FWoTControllerUIInput::HandleKeyUpEvent(FSlateApplication&,
	const FKeyEvent& InKeyEvent)
{
	if (const FRepeat* Repeat = Repeats.Find(InKeyEvent.GetUserIndex());
		Repeat && Repeat->Key == InKeyEvent.GetKey())
	{
		Cancel(InKeyEvent.GetUserIndex());
	}
	return false;
}

bool FWoTControllerUIInput::HandleAnalogInputEvent(FSlateApplication& SlateApp,
	const FAnalogInputEvent& InAnalogInputEvent)
{
	const FKey Key = InAnalogInputEvent.GetKey();
	const float Value = InAnalogInputEvent.GetAnalogValue();
	const int32 UserIndex = InAnalogInputEvent.GetUserIndex();
	// Observed for every event, in or out of a menu, so a stick pushed during
	// gameplay is never mistaken for a fresh crossing once a menu opens.
	const EUINavigation Crossing = NavigationConfig->ObserveAnalog(UserIndex, Key, Value);
	if (!IsInScope(SlateApp, UserIndex))
	{
		return false;
	}
	if (WoTIsStickAxis(Key) && FMath::Abs(Value) > StickControllerThreshold)
	{
		SetLastInput(EWoTLastInput::Controller);
	}
	if (!NavigationConfig->IsNavigationAxis(Key))
	{
		return false;
	}
	if (Crossing != EUINavigation::Invalid)
	{
		const uint64 TopEntryId = FWoTBackStack::Get().FindHandlerId(SlateApp, UserIndex);
		if (TopEntryId != 0)
		{
			Arm(UserIndex, Crossing, Key, TopEntryId);
		}
		else
		{
			Cancel(UserIndex);
		}
	}
	else if (NavigationConfig->DirectionForAnalog(Key, Value) == EUINavigation::Invalid)
	{
		if (const FRepeat* Repeat = Repeats.Find(UserIndex); Repeat && Repeat->Key == Key)
		{
			Cancel(UserIndex);
		}
	}
	return false;
}

bool FWoTControllerUIInput::HandleMouseMoveEvent(FSlateApplication& SlateApp,
	const FPointerEvent& MouseEvent)
{
	FWoTControllerUIGlobals& Globals = WoTControllerUIGlobals();
	if (Globals.LastInput != EWoTLastInput::Controller
		|| !IsInScope(SlateApp, MouseEvent.GetUserIndex()))
	{
		return false;
	}
	const FVector2D Position(MouseEvent.GetScreenSpacePosition());
	if (!Globals.PointerAnchor.IsSet())
	{
		Globals.PointerAnchor = Position;
	}
	if (FVector2D::Distance(Position, Globals.PointerAnchor.GetValue()) > PointerMoveThreshold)
	{
		SetLastInput(EWoTLastInput::Pointer);
		// This move is the one that resumes hover.
		return false;
	}
	// Zero-distance OS moves must not hover anything under a hidden cursor.
	return Globals.bCursorSuppressed;
}

bool FWoTControllerUIInput::HandleMouseButtonDownEvent(FSlateApplication& SlateApp,
	const FPointerEvent& MouseEvent)
{
	// A mouse button, or touch (Slate routes a touch start here).
	if (IsInScope(SlateApp, MouseEvent.GetUserIndex()))
	{
		SetLastInput(EWoTLastInput::Pointer);
	}
	return false;
}

bool FWoTControllerUIInput::HandleMouseButtonDoubleClickEvent(
	FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	return HandleMouseButtonDownEvent(SlateApp, MouseEvent);
}

void FWoTControllerUIInput::Tick(const float, FSlateApplication& SlateApp,
	TSharedRef<ICursor>)
{
	TickRepeat(SlateApp);
}

void FWoTControllerUIInput::TickRepeat(FSlateApplication& SlateApp)
{
	if (Repeats.IsEmpty())
	{
		return;
	}
	TArray<int32> Users;
	Repeats.GetKeys(Users);
	for (const int32 UserIndex : Users)
	{
		const uint64 TopEntryId = FWoTBackStack::Get().FindHandlerId(SlateApp, UserIndex);
		FRepeat* Repeat = Repeats.Find(UserIndex);
		if (Repeat == nullptr)
		{
			continue;
		}
		if (TopEntryId == 0 || TopEntryId != Repeat->TopEntryId
			|| !IsInScope(SlateApp, UserIndex))
		{
			// The surface that armed the repeat closed or was covered.
			Cancel(UserIndex);
			continue;
		}
		const double Time = Now();
		if (Time < Repeat->NextFireTime)
		{
			continue;
		}
		Repeat->NextFireTime += RepeatInterval;
		if (Repeat->NextFireTime <= Time)
		{
			Repeat->NextFireTime = Time + RepeatInterval;
		}
		const EUINavigation Direction = Repeat->Direction;
		const TSharedPtr<FSlateUser> User = SlateApp.GetUser(UserIndex);
		const FWidgetPath FocusPath = User.IsValid()
			? User->GetWeakFocusPath().ToWidgetPath() : FWidgetPath();
		if (!FocusPath.IsValid())
		{
			Cancel(UserIndex);
			continue;
		}
		// Controller genesis through the reply path: every widget's
		// OnNavigation runs (wrap links, list rows, the in-game menu's wrap).
		SlateApp.ProcessExternalReply(FocusPath,
			FReply::Handled().SetNavigation(Direction, ENavigationGenesis::Controller),
			UserIndex, INDEX_NONE);
	}
}
