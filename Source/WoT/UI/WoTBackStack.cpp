#include "UI/WoTBackStack.h"

#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Input/Events.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace
{
// The window a widget is shown in, or null when it or any ancestor is not
// visible (a collapsed overlay, a screen torn down but still referenced).
TSharedPtr<SWindow> WoTBackStack_ShownWindow(const TSharedPtr<SWidget>& Widget)
{
	for (TSharedPtr<SWidget> Current = Widget; Current.IsValid();
		Current = Current->GetParentWidget())
	{
		if (!Current->GetVisibility().IsVisible())
		{
			return nullptr;
		}
		if (Current->Advanced_IsWindow())
		{
			return StaticCastSharedPtr<SWindow>(Current);
		}
	}
	return nullptr;
}

bool WoTBackStack_IsInFamily(TSharedPtr<SWindow> Window,
	const TSharedPtr<SWindow>& Family)
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

// Routes B and View to the stack. Game priority: the controller UI input
// processor (PreGame) sees the press first for the last-input mode.
class FWoTBackStackInputProcessor final : public IInputProcessor
{
public:
	virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override
	{
	}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp,
		const FKeyEvent& InKeyEvent) override
	{
		return FWoTBackStack::Get().HandleKeyDown(SlateApp, InKeyEvent);
	}

	virtual const TCHAR* GetDebugName() const override
	{
		return TEXT("WoTBackStack");
	}
};
}

FWoTBackStackEntryHandle::~FWoTBackStackEntryHandle()
{
	FWoTBackStack::Get().Unregister(Id);
}

FWoTBackStack& FWoTBackStack::Get()
{
	static FWoTBackStack Stack;
	return Stack;
}

bool FWoTBackStack::IsBackKey(const FKey& Key)
{
	return Key == EKeys::Gamepad_FaceButton_Right
		|| Key == EKeys::Gamepad_Special_Left;
}

TSharedRef<FWoTBackStackEntryHandle> FWoTBackStack::Register(
	const TSharedRef<SWidget>& Owner, TFunction<bool()> IsActive,
	TFunction<bool()> OnBack)
{
	FEntry& Entry = Entries.AddDefaulted_GetRef();
	Entry.Id = NextId++;
	Entry.Owner = Owner;
	Entry.IsActive = MoveTemp(IsActive);
	Entry.OnBack = MoveTemp(OnBack);
	SyncInputProcessor();
	return MakeShareable(new FWoTBackStackEntryHandle(Entry.Id));
}

void FWoTBackStack::Unregister(uint64 Id)
{
	Entries.RemoveAll([Id](const FEntry& Entry) { return Entry.Id == Id; });
	SyncInputProcessor();
}

void FWoTBackStack::Prune()
{
	const int32 Removed = Entries.RemoveAll([](const FEntry& Entry)
	{
		return !Entry.Owner.IsValid();
	});
	if (Removed > 0)
	{
		SyncInputProcessor();
	}
}

int32 FWoTBackStack::NumEntries()
{
	Prune();
	return Entries.Num();
}

void FWoTBackStack::SyncInputProcessor()
{
	if (!FSlateApplication::IsInitialized())
	{
		InputProcessor.Reset();
		return;
	}
	FSlateApplication& SlateApp = FSlateApplication::Get();
	if (!Entries.IsEmpty() && !InputProcessor.IsValid())
	{
		InputProcessor = MakeShared<FWoTBackStackInputProcessor>();
		SlateApp.RegisterInputPreProcessor(InputProcessor,
			EInputPreProcessorType::Game);
	}
	else if (Entries.IsEmpty() && InputProcessor.IsValid())
	{
		// Removal during an event is deferred by Slate until the event ends.
		SlateApp.UnregisterInputPreProcessor(InputProcessor);
		InputProcessor.Reset();
	}
}

int32 FWoTBackStack::FindHandlerIndex(FSlateApplication& SlateApp,
	int32 UserIndex)
{
	Prune();
	TSharedPtr<SWindow> FocusWindow;
	if (const TSharedPtr<FSlateUser> User = SlateApp.GetUser(UserIndex))
	{
		if (const TSharedPtr<SWidget> Focused = User->GetFocusedWidget())
		{
			FocusWindow = SlateApp.FindWidgetWindow(Focused.ToSharedRef());
		}
	}
	if (!FocusWindow.IsValid())
	{
		FocusWindow = SlateApp.GetActiveTopLevelWindow();
	}
	if (!FocusWindow.IsValid())
	{
		return INDEX_NONE;
	}
	for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
	{
		const FEntry& Entry = Entries[Index];
		const TSharedPtr<SWidget> EntryOwner = Entry.Owner.Pin();
		const TSharedPtr<SWindow> OwnerWindow = WoTBackStack_ShownWindow(EntryOwner);
		if (!OwnerWindow.IsValid()
			|| !WoTBackStack_IsInFamily(FocusWindow, OwnerWindow))
		{
			continue;
		}
		if (Entry.IsActive && !Entry.IsActive())
		{
			continue;
		}
		return Index;
	}
	return INDEX_NONE;
}

uint64 FWoTBackStack::FindHandlerId(FSlateApplication& SlateApp, int32 UserIndex)
{
	const int32 Index = FindHandlerIndex(SlateApp, UserIndex);
	return Entries.IsValidIndex(Index) ? Entries[Index].Id : 0;
}

bool FWoTBackStack::HandleKeyDown(FSlateApplication& SlateApp,
	const FKeyEvent& KeyEvent)
{
	if (!IsBackKey(KeyEvent.GetKey()))
	{
		return false;
	}
	const int32 Index = FindHandlerIndex(SlateApp, KeyEvent.GetUserIndex());
	if (!Entries.IsValidIndex(Index))
	{
		return false;
	}
	if (KeyEvent.IsRepeat())
	{
		// A held B backs exactly one level.
		return true;
	}
	// Copy the action: OnBack may destroy its owner and drop this entry.
	const TFunction<bool()> OnBack = Entries[Index].OnBack;
	return OnBack ? OnBack() : true;
}
