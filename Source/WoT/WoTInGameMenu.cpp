#include "WoTInGameMenu.h"

bool FWoTInGameMenuModel::Open()
{
	if (State != EWoTInGameMenuState::Closed)
	{
		return false;
	}
	State = EWoTInGameMenuState::Open;
	return true;
}

bool FWoTInGameMenuModel::Resume()
{
	if (State != EWoTInGameMenuState::Open)
	{
		return false;
	}
	State = EWoTInGameMenuState::Closed;
	return true;
}

bool FWoTInGameMenuModel::BeginLeave()
{
	if (State != EWoTInGameMenuState::Open)
	{
		return false;
	}
	State = EWoTInGameMenuState::Leaving;
	return true;
}

bool FWoTInGameMenuModel::CancelLeave()
{
	if (State != EWoTInGameMenuState::Leaving)
	{
		return false;
	}
	State = EWoTInGameMenuState::Open;
	return true;
}

void FWoTInGameMenuModel::Reset()
{
	State = EWoTInGameMenuState::Closed;
}
