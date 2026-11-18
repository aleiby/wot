#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"

/** Sees keys ahead of every widget, so the mode switch and recentre shortcuts
 * work in menus, the front end, chat and while dead. It consumes only the two
 * shortcut keys, and only when a local player's VR rig handled them. */
class FWoTVRInputProcessor final : public IInputProcessor
{
public:
	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp,
		TSharedRef<ICursor> Cursor) override;
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual const TCHAR* GetDebugName() const override { return TEXT("WoTVRShortcuts"); }
};
