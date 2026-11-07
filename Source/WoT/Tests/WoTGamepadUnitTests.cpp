// Gamepad layout table and stick shaping: pure specs, no world.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Gamepad/WoTGamepadLayout.h"
#include "../Gamepad/WoTStickShaping.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadStickShapingSpec,
	"WoT.Input.Gamepad.StickShaping", Gamepad_UnitFlags)

bool FWoTGamepadStickShapingSpec::RunTest(const FString&)
{
	using S = FWoTStickShaping;
	const double Tol = 1.0e-4;
	TestTrue(TEXT("inside the dead zone is zero"),
		S::RadialDeadZone(FVector2D(0.19f, 0.0f), 0.2f, 0.95f).IsZero());
	TestTrue(TEXT("the dead-zone edge is zero"),
		S::RadialDeadZone(FVector2D(0.0f, 0.2f), 0.2f, 0.95f).IsZero());
	const FVector2D Mid = S::RadialDeadZone(FVector2D(0.575f, 0.0f), 0.2f, 0.95f);
	TestEqual(TEXT("halfway between the zones rescales to 0.5"), Mid.X, 0.5, Tol);
	const FVector2D Diag = S::RadialDeadZone(FVector2D(1.0f, 1.0f), 0.2f, 0.95f);
	TestEqual(TEXT("a corner clamps to magnitude 1"), Diag.Size(), 1.0, Tol);
	TestEqual(TEXT("direction is kept"), Diag.X, Diag.Y, Tol);
	TestTrue(TEXT("a dead zone at or past the outer zone gives nothing"),
		S::RadialDeadZone(FVector2D(0.9f, 0.0f), 0.95f, 0.95f).IsZero());

	// MoveGain 4.7: full input from 1/4.7 = 21.3% past the dead zone.
	const FVector2D Saturated = S::Move(FVector2D(0.0f, 0.2f + 0.75f * 0.22f), 0.2f, 0.95f, 4.7f);
	TestEqual(TEXT("22% past the dead zone saturates"), Saturated.Y, 1.0, Tol);
	const FVector2D Partial = S::Move(FVector2D(0.0f, 0.2f + 0.75f * 0.1f), 0.2f, 0.95f, 4.7f);
	TestEqual(TEXT("10% past the dead zone is partial"), Partial.Y, 0.47, Tol);
	TestEqual(TEXT("a saturated diagonal is not faster than straight"),
		S::Move(FVector2D(0.7f, 0.7f), 0.2f, 0.95f, 4.7f).Size(), 1.0, Tol);

	const float Dt = 1.0f / 60.0f;
	const FVector2D FullRight = S::LookDegrees(FVector2D(1.0f, 0.0f), 0.2f, 0.95f, 2.0f,
		220.0f, 150.0f, 1.0f, false, 1.0f, Dt);
	TestEqual(TEXT("full right yaws YawRate * dt"), FullRight.X, double(220.0f * Dt), Tol);
	TestEqual(TEXT("no pitch from pure yaw"), FullRight.Y, 0.0, Tol);
	const FVector2D HalfUp = S::LookDegrees(FVector2D(0.0f, 0.575f), 0.2f, 0.95f, 2.0f,
		220.0f, 150.0f, 1.0f, false, 1.0f, Dt);
	TestEqual(TEXT("half tilt up follows the curve (0.5^2)"), HalfUp.Y, double(0.25f * 150.0f * Dt), Tol);
	const FVector2D Inverted = S::LookDegrees(FVector2D(0.0f, 0.575f), 0.2f, 0.95f, 2.0f,
		220.0f, 150.0f, 1.0f, true, 1.0f, Dt);
	TestEqual(TEXT("invert flips pitch"), Inverted.Y, -HalfUp.Y, Tol);
	const FVector2D Scaled = S::LookDegrees(FVector2D(1.0f, 0.0f), 0.2f, 0.95f, 2.0f,
		220.0f, 150.0f, 2.0f, false, 0.5f, Dt);
	TestEqual(TEXT("sensitivity and FOV scale multiply"), Scaled.X, FullRight.X, Tol);
	return !HasAnyErrors();
}


#endif // WITH_AUTOMATION_TESTS
