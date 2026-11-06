#pragma once

#include "CoreMinimal.h"

// Widget-free state for the gameplay Escape overlay. Slate projects this state,
// while automation can exercise every transition without initializing a viewport.
enum class EWoTInGameMenuState : uint8
{
	Closed,
	Open,
	Leaving
};

class FWoTInGameMenuModel
{
public:
	EWoTInGameMenuState GetState() const { return State; }
	bool IsOpen() const { return State != EWoTInGameMenuState::Closed; }
	bool IsLeaving() const { return State == EWoTInGameMenuState::Leaving; }

	bool Open();
	bool Resume();
	bool BeginLeave();
	bool CancelLeave();
	void Reset();

private:
	EWoTInGameMenuState State = EWoTInGameMenuState::Closed;
};
