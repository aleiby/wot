#include "WoTGamepadLayout.h"

#include "Generated/WoTControllerLayout.inl"

TConstArrayView<FWoTGamepadBinding> WoTGamepadLayout()
{
	return MakeArrayView(WoTControllerLayoutGenerated::Bindings);
}

const FWoTGamepadTuning& WoTGamepadTuning()
{
	return WoTControllerLayoutGenerated::Tuning;
}

FKey WoTGamepadKey(EWoTGamepadInput Input)
{
	switch (Input)
	{
	case EWoTGamepadInput::LeftStick: return EKeys::Gamepad_LeftX;
	case EWoTGamepadInput::RightStick: return EKeys::Gamepad_RightX;
	case EWoTGamepadInput::LeftTrigger: return EKeys::Gamepad_LeftTrigger;
	case EWoTGamepadInput::RightTrigger: return EKeys::Gamepad_RightTrigger;
	case EWoTGamepadInput::FaceBottom: return EKeys::Gamepad_FaceButton_Bottom;
	case EWoTGamepadInput::FaceRight: return EKeys::Gamepad_FaceButton_Right;
	case EWoTGamepadInput::FaceLeft: return EKeys::Gamepad_FaceButton_Left;
	case EWoTGamepadInput::FaceTop: return EKeys::Gamepad_FaceButton_Top;
	case EWoTGamepadInput::LeftShoulder: return EKeys::Gamepad_LeftShoulder;
	case EWoTGamepadInput::RightShoulder: return EKeys::Gamepad_RightShoulder;
	case EWoTGamepadInput::DPadUp: return EKeys::Gamepad_DPad_Up;
	case EWoTGamepadInput::DPadDown: return EKeys::Gamepad_DPad_Down;
	case EWoTGamepadInput::DPadLeft: return EKeys::Gamepad_DPad_Left;
	case EWoTGamepadInput::DPadRight: return EKeys::Gamepad_DPad_Right;
	case EWoTGamepadInput::LeftThumb: return EKeys::Gamepad_LeftThumbstick;
	case EWoTGamepadInput::RightThumb: return EKeys::Gamepad_RightThumbstick;
	case EWoTGamepadInput::View: return EKeys::Gamepad_Special_Left;
	case EWoTGamepadInput::Menu: return EKeys::Gamepad_Special_Right;
	}
	return FKey();
}

FKey WoTGamepadStickYKey(EWoTGamepadInput Input)
{
	switch (Input)
	{
	case EWoTGamepadInput::LeftStick: return EKeys::Gamepad_LeftY;
	case EWoTGamepadInput::RightStick: return EKeys::Gamepad_RightY;
	default: return FKey();
	}
}

bool WoTGamepadActionIsStick(EWoTGamepadAction Action)
{
	return Action == EWoTGamepadAction::Move || Action == EWoTGamepadAction::Look;
}

const TCHAR* WoTGamepadActionName(EWoTGamepadAction Action)
{
	static const TCHAR* const Names[] = {
		TEXT("Move"), TEXT("Look"), TEXT("Fire"), TEXT("Jump"), TEXT("EditCitadel"),
		TEXT("Drop"), TEXT("DropTainted"), TEXT("ItemInfo"), TEXT("PrevItem"),
		TEXT("NextItem"), TEXT("PrevHand"), TEXT("NextHand"), TEXT("ViewUp"),
		TEXT("ViewDown"), TEXT("RandomTaunt"), TEXT("WalkToggle"), TEXT("CenterView"),
		TEXT("Scoreboard"), TEXT("Menu"), TEXT("TurnAround"),
		TEXT("JoinTeam1"), TEXT("JoinTeam2"), TEXT("JoinTeam3"), TEXT("JoinTeam4"),
		TEXT("Chat"), TEXT("TeamChat")};
	static_assert(UE_ARRAY_COUNT(Names) == static_cast<int32>(EWoTGamepadAction::Count),
		"one name per gamepad action");
	const int32 Index = static_cast<int32>(Action);
	return Index >= 0 && Index < UE_ARRAY_COUNT(Names) ? Names[Index] : TEXT("?");
}

EWoTGamepadTrigger WoTGamepadActionTrigger(EWoTGamepadAction Action)
{
	for (const FWoTGamepadBinding& Row : WoTGamepadLayout())
	{
		if (Row.Action == Action) { return Row.Trigger; }
	}
	return EWoTGamepadTrigger::Press;
}
