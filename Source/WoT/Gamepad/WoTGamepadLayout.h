// Controller (gamepad) layout: the generated table from ControllerLayout.csv,
// the stick and timing tuning from ControllerTuning.csv, and the gamepad keys
// each input name stands for. The names must match the CSVs exactly
// (Tools/build/generate_controller_layout.py validates them).
#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

/** TeamSelect overlays Gameplay while a Battle player has no team. */
enum class EWoTGamepadContext : uint8 { Gameplay, CitadelEditor, TeamSelect, Count };

enum class EWoTGamepadInput : uint8
{
	LeftStick, RightStick, LeftTrigger, RightTrigger,
	FaceBottom, FaceRight, FaceLeft, FaceTop,
	LeftShoulder, RightShoulder,
	DPadUp, DPadDown, DPadLeft, DPadRight,
	LeftThumb, RightThumb, View, Menu,
};

enum class EWoTGamepadAction : uint8
{
	Move, Look, Fire, Jump, EditCitadel, Drop, DropTainted, ItemInfo,
	PrevItem, NextItem, PrevHand, NextHand, ViewUp, ViewDown,
	RandomTaunt, WalkToggle, CenterView, Scoreboard, Menu, TurnAround,
	JoinTeam1, JoinTeam2, JoinTeam3, JoinTeam4,
	Count
};

/** Press fires on the press edge; Tap on a quick release; Hold once held long enough. */
enum class EWoTGamepadTrigger : uint8 { Press, Tap, Hold };

struct FWoTGamepadBinding
{
	EWoTGamepadContext Context;
	EWoTGamepadInput Input;
	EWoTGamepadAction Action;
	EWoTGamepadTrigger Trigger;
};

struct FWoTGamepadTuning
{
	float OuterZone;       // deflection treated as full tilt
	float MoveGain;        // movement input per unit of shaped deflection
	float YawRate;         // degrees per second at full tilt
	float PitchRate;       // degrees per second at full tilt
	float LookExponent;    // look response curve
	float DropHoldSeconds; // FaceLeft tap/hold split
	float TurnAroundSeconds; // LeftTrigger quick 180-degree turn
};

/** Every row of ControllerLayout.csv, in file order. */
TConstArrayView<FWoTGamepadBinding> WoTGamepadLayout();
/** ControllerTuning.csv. */
const FWoTGamepadTuning& WoTGamepadTuning();
/** The key an input stands for; a stick's X axis for the sticks. */
FKey WoTGamepadKey(EWoTGamepadInput Input);
/** A stick's Y axis; an invalid key for every other input. */
FKey WoTGamepadStickYKey(EWoTGamepadInput Input);
/** Move and Look carry a 2D stick value; every other action is a button. */
bool WoTGamepadActionIsStick(EWoTGamepadAction Action);
const TCHAR* WoTGamepadActionName(EWoTGamepadAction Action);
/** The trigger the table gives this action (one per action); Press when unbound. */
EWoTGamepadTrigger WoTGamepadActionTrigger(EWoTGamepadAction Action);
