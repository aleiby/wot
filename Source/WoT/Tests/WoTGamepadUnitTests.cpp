// Gamepad layout table and stick shaping: pure specs, no world.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Gamepad/WoTGamepadLayout.h"

namespace
{
constexpr EAutomationTestFlags Gamepad_UnitFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

bool Gamepad_ContextHas(EWoTGamepadContext Context, EWoTGamepadAction Action)
{
	for (const FWoTGamepadBinding& Row : WoTGamepadLayout())
	{
		if (Row.Context == Context && Row.Action == Action) { return true; }
	}
	return false;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadLayoutTableSpec,
	"WoT.Input.Gamepad.LayoutTable", Gamepad_UnitFlags)

bool FWoTGamepadLayoutTableSpec::RunTest(const FString&)
{
	TestTrue(TEXT("the generated table has rows"), WoTGamepadLayout().Num() > 0);
	for (EWoTGamepadContext Context : {EWoTGamepadContext::Gameplay, EWoTGamepadContext::CitadelEditor})
	{
		for (EWoTGamepadAction Required : {EWoTGamepadAction::Move, EWoTGamepadAction::Look,
			EWoTGamepadAction::Menu, EWoTGamepadAction::Fire})
		{
			TestTrue(FString::Printf(TEXT("context %d carries %s"), static_cast<int32>(Context),
				WoTGamepadActionName(Required)), Gamepad_ContextHas(Context, Required));
		}
	}
	TSet<TPair<int32, int32>> InputTriggerSeen;
	for (const FWoTGamepadBinding& Row : WoTGamepadLayout())
	{
		const FKey Key = WoTGamepadKey(Row.Input);
		TestTrue(FString::Printf(TEXT("%s maps to a gamepad key"), WoTGamepadActionName(Row.Action)),
			Key.IsValid() && Key.IsGamepadKey());
		const bool bStick = WoTGamepadActionIsStick(Row.Action);
		TestEqual(FString::Printf(TEXT("%s has a Y key only on a stick"), WoTGamepadActionName(Row.Action)),
			WoTGamepadStickYKey(Row.Input).IsValid(), bStick);
		const TPair<int32, int32> Slot(static_cast<int32>(Row.Context) * 256 + static_cast<int32>(Row.Input),
			static_cast<int32>(Row.Trigger));
		TestFalse(TEXT("an input and trigger appear once per context"), InputTriggerSeen.Contains(Slot));
		InputTriggerSeen.Add(Slot);
	}
	TestTrue(TEXT("FaceLeft taps drop"),
		WoTGamepadActionTrigger(EWoTGamepadAction::Drop) == EWoTGamepadTrigger::Tap);
	TestTrue(TEXT("FaceLeft holds drop tainted"),
		WoTGamepadActionTrigger(EWoTGamepadAction::DropTainted) == EWoTGamepadTrigger::Hold);
	TestTrue(TEXT("Fire is a plain press"),
		WoTGamepadActionTrigger(EWoTGamepadAction::Fire) == EWoTGamepadTrigger::Press);
	TestTrue(TEXT("the left stick is the X axis plus the Y axis"),
		WoTGamepadKey(EWoTGamepadInput::LeftStick) == EKeys::Gamepad_LeftX
		&& WoTGamepadStickYKey(EWoTGamepadInput::LeftStick) == EKeys::Gamepad_LeftY);
	TestTrue(TEXT("View and Menu are the special buttons"),
		WoTGamepadKey(EWoTGamepadInput::View) == EKeys::Gamepad_Special_Left
		&& WoTGamepadKey(EWoTGamepadInput::Menu) == EKeys::Gamepad_Special_Right);
	const FWoTGamepadTuning& Tuning = WoTGamepadTuning();
	TestTrue(TEXT("tuning came from the CSV"), Tuning.MoveGain > 1.0f && Tuning.YawRate > 0.0f
		&& Tuning.OuterZone > 0.35f && Tuning.OuterZone <= 1.0f);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
