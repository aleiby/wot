#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS
#include "WoTFirstPersonFraming.h"
#include "WoTFirstPersonGrip.h"
#include "WoTFirstPersonMotion.h"

namespace
{
constexpr auto MathFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPPoseFallbackSpec,
	"WoT.FirstPerson.PoseRowsAndUnknownFallback", MathFlags)
bool FWoTFPPoseFallbackSpec::RunTest(const FString&)
{
	const auto& Pose = WoTFirstPerson::FindPose(TEXT("UnlistedSpecArtifact"));
	TestTrue(TEXT("unknown class uses default offset"), Pose.Offset.Equals(FVector(60, 24, -20)));
	TestTrue(TEXT("generated default rotation parses"), Pose.Rotation.Equals(FRotator(0, -90, 0)));
	TestEqual(TEXT("generated multiplier parses"), Pose.ScaleMultiplier, 1.0f);
	TestTrue(TEXT("default key resolves the same row"), &Pose == &WoTFirstPerson::FindPose(TEXT("*")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPClearanceSpec,
	"WoT.FirstPerson.ClearanceAcrossBoundsAndMotionExtremes", MathFlags)
bool FWoTFPClearanceSpec::RunTest(const FString&)
{
	// Off-center and differently sized pickup bounds must fit around their center.
	for (const FBox Bounds : {FBox(FVector(-1), FVector(1)),
		FBox(FVector(10, -70, -2), FVector(30, 50, 8))})
	for (const double NearPlane : {2.0, 10.0})
	{
		for (const double Radius : {17.0, NearPlane + 4.0, NearPlane + 3.1, NearPlane + 3.01})
		{
			const auto& Pose = WoTFirstPerson::FindPose(NAME_None);
			const auto Fit = WoTFirstPerson::Fit(Bounds, Pose, Radius, NearPlane, 1.0);
			TestTrue(TEXT("positive clearance interval stays visible"), Fit.bVisible);
			TestTrue(TEXT("near extreme is beyond near plane plus one"), Fit.MinDepth * Fit.Pull > NearPlane + 1.0);
			TestTrue(TEXT("far extreme is inside capsule minus bob margin"), Fit.MaxDepth * Fit.Pull < Radius - 2.0);
			if (Radius < 17) TestTrue(TEXT("small capsule shrinks the model"), Fit.Shrink < 1.0);
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector Vertex((Corner & 1) ? Bounds.Max.X : Bounds.Min.X,
					(Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y, (Corner & 4) ? Bounds.Max.Z : Bounds.Min.Z);
				for (const FRotator Rotation : {FRotator::ZeroRotator, FRotator(80, 110, -40), FRotator(-170, 5, 90)})
					for (const double MotionX : {-1.5, 1.5})
					{
						const double Depth = (Pose.Offset.X + Rotation.RotateVector(Vertex - Bounds.GetCenter()).X * Fit.ModelScale
							+ MotionX * Fit.Shrink) * Fit.Pull;
						TestTrue(TEXT("independent transformed vertex clears both limits"), Depth > NearPlane + 1 && Depth < Radius - 2);
					}
			}
		}
	}
	for (const double NearPlane : {2.0, 10.0})
	{
		const auto Impossible = WoTFirstPerson::Fit(FBox(FVector(-1), FVector(1)),
			WoTFirstPerson::FindPose(NAME_None), NearPlane + 3.0, NearPlane, 1.0);
		TestFalse(TEXT("empty physical interval cannot display a model"), Impossible.bVisible);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPPullIdentitySpec,
	"WoT.FirstPerson.RealPullMatchesFirstPersonWarp", MathFlags)
bool FWoFPPullIdentitySpec::RunTest(const FString&)
{
	const auto& Pose = WoTFirstPerson::FindPose(NAME_None);
	const FBox Bounds(FVector(10, -70, -2), FVector(30, 50, 8));
	const auto Fit = WoTFirstPerson::Fit(Bounds, Pose, 17, 2, 1);
	const FQuat Rotation = FRotator(12, -30, 8).Quaternion() * Pose.Rotation.Quaternion();
	// Include motion and a partial switch drop, with off-center local bounds.
	const FVector Location = Pose.Offset + FVector(-1, 0.3, -20)
		- Rotation.RotateVector(Bounds.GetCenter() * Fit.ModelScale);
	const FTransform OldPose(Rotation, Location, FVector(Fit.ModelScale));
	const FTransform PulledPose(Rotation, Location * Fit.Pull, FVector(Fit.ModelScale * Fit.Pull));
	for (const double ModelFOV : {90.0, 60.0})
	{
		const double FOVCorrection = FMath::Tan(FMath::DegreesToRadians(90.0 / 2))
			/ FMath::Tan(FMath::DegreesToRadians(ModelFOV / 2));
		for (const FVector Point : {Bounds.Min, Bounds.Max, Bounds.GetCenter(), FVector(15, -20, 0)})
		{
			const FVector OldRendered = OldPose.TransformPosition(Point)
				* FVector(Fit.Pull, FOVCorrection * Fit.Pull, FOVCorrection * Fit.Pull);
			const FVector NewRendered = PulledPose.TransformPosition(Point) * FVector(1, FOVCorrection, FOVCorrection);
			TestTrue(TEXT("real translation and scale pull preserves every rendered sample"), OldRendered.Equals(NewRendered, 1.e-9));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPMotionRateSpec,
	"WoT.FirstPerson.MotionAt30And144Hz", MathFlags)
bool FWoTFPMotionRateSpec::RunTest(const FString&)
{
	FWoTFirstPersonMotion Slow, Fast;
	auto Run = [](FWoTFirstPersonMotion& Motion, int32 Rate)
	{
		Motion.Step(0, FRotator::ZeroRotator, 120, 1);
		Motion.FirePressed();
		for (int32 Index = 1; Index <= Rate; ++Index)
			Motion.Step(1.0 / Rate, FRotator(0, 30.0 * Index / Rate, 0), 120, 1);
		Motion.FireReleased();
		for (int32 Index = 1; Index <= Rate; ++Index)
			Motion.Step(1.0 / Rate, FRotator(0, 30 + 30.0 * Index / Rate, 0), 120, 1);
	};
	Run(Slow, 30);
	Run(Fast, 144);
	TestTrue(TEXT("translation is independent of render rate"), Slow.Offset().Equals(Fast.Offset(), 0.005));
	TestTrue(TEXT("view lag is independent of render rate"), Slow.Rotation().Equals(Fast.Rotation(), 0.005));
	TestTrue(TEXT("distance advances walking phase"), !Slow.Offset().IsNearlyZero());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPMotionZeroSpec,
	"WoT.FirstPerson.MotionZeroRemainsStill", MathFlags)
bool FWoTFPMotionZeroSpec::RunTest(const FString&)
{
	FWoTFirstPersonMotion Motion;
	Motion.FirePressed();
	for (int32 Index = 0; Index < 60; ++Index)
	{
		Motion.Step(1.0 / 60, FRotator(Index * 5, Index * 8, 0), 200, 0);
		TestTrue(TEXT("zero motion suppresses all translations"), Motion.Offset().IsZero());
		TestTrue(TEXT("zero motion suppresses all rotations"), Motion.Rotation().IsZero());
	}
	Motion.FireReleased();
	Motion.Step(0.1, FRotator::ZeroRotator, 0, 0);
	TestTrue(TEXT("release also stays still"), Motion.Offset().IsZero());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPMotionFireSpec,
	"WoT.FirstPerson.FireKickHoldAndRelease", MathFlags)
bool FWoTFPMotionFireSpec::RunTest(const FString&)
{
	FWoTFirstPersonMotion Motion;
	Motion.FirePressed();
	Motion.Step(0.04, FRotator::ZeroRotator, 0, 1);
	TestTrue(TEXT("press causes a backward kick"), Motion.Offset().X < -0.5);
	Motion.Step(1.0, FRotator::ZeroRotator, 0, 1);
	TestTrue(TEXT("holding does not repeat the kick"), FMath::Abs(Motion.Offset().X) < 0.01);
	Motion.FireReleased();
	Motion.Step(0.04, FRotator::ZeroRotator, 0, 1);
	TestTrue(TEXT("release produces a small forward settle"), Motion.Offset().X > 0.01);
	Motion.FirePressed();
	Motion.Step(0.04, FRotator::ZeroRotator, 0, 1);
	TestTrue(TEXT("next press produces exactly a new kick"), Motion.Offset().X < -0.5);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPHandPoseTableSpec,
	"WoT.FirstPerson.HandPoseTableResolves", MathFlags)
bool FWoTFPHandPoseTableSpec::RunTest(const FString&)
{
	const FWoTHandPoseRow* Relaxed = WoTFirstPerson::FindHandPose(TEXT("relaxed"));
	TestTrue(TEXT("relaxed pose exists without a grip"), Relaxed && !Relaxed->bGrip && Relaxed->NumBones > 0);
	for (const TCHAR* Name : {TEXT("rod"), TEXT("figurine"), TEXT("orb"), TEXT("disc")})
	{
		const FWoTHandPoseRow* Row = WoTFirstPerson::FindHandPose(Name);
		if (!TestTrue(FString::Printf(TEXT("%s grip exists"), Name), Row && Row->bGrip)) continue;
		TestTrue(TEXT("grip line is a unit vector"), FMath::IsNearlyEqual(Row->ItemAxis.Size(), 1.0, 1e-4));
		TestTrue(TEXT("grip front is a unit vector across the line"),
			FMath::IsNearlyEqual(Row->ItemFront.Size(), 1.0, 1e-4) && FMath::Abs(Row->ItemAxis | Row->ItemFront) < 1e-4);
		TestTrue(TEXT("grip places the hand in front of the camera"), Row->Offset.X > 0 && Row->Scale > 0);
	}
	int32 Items = 0;
	for (const FWoTHeldItemGrip& Item : WoTFirstPerson::HeldItemGrips())
	{
		++Items;
		const FWoTHandPoseRow* Row = WoTFirstPerson::FindHandPose(Item.Grip);
		TestTrue(FString::Printf(TEXT("%s names a grip pose"), Item.Class), Row && Row->bGrip);
		TestTrue(FString::Printf(TEXT("%s has a positive size"), Item.Class), Item.Size > 0);
	}
	TestTrue(TEXT("artifacts carry grip rows"), Items >= 40);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPHeldItemAlignSpec,
	"WoT.FirstPerson.HeldItemAlignsLeadingAxisAndFront", MathFlags)
bool FWoTFPHeldItemAlignSpec::RunTest(const FString&)
{
	// A listed item's leading axis lies on its grip's line and its front turns to the grip's front.
	const FBox Bounds(FVector(-3, -5, -7), FVector(5, 3, 9));
	for (const TCHAR* Class : {TEXT("AngrealInvFireball"), TEXT("AngrealInvLightGlobe"), TEXT("AngrealInvDart")})
	{
		const FWoTHeldItemGrip* Item = WoTFirstPerson::FindHeldItemGrip(Class);
		if (!TestNotNull(FString::Printf(TEXT("%s is listed"), Class), Item)) continue;
		const FName Grip = WoTFirstPerson::GripForItem(Class, Bounds);
		TestEqual(TEXT("listed grip wins"), Grip, FName(Item->Grip));
		const FWoTHandPoseRow* Row = WoTFirstPerson::FindHandPose(Grip);
		const FTransform Placed = WoTFirstPerson::HeldItemTransform(*Row, Class, Bounds, 1.0, {});
		const FQuat Rotation = Placed.GetRotation();
		TestTrue(TEXT("leading axis lies on the grip line"), Rotation.RotateVector(Item->Axis.GetSafeNormal()).Equals(Row->ItemAxis, 1e-3));
		const FVector Front = Item->Front - Item->Axis.GetSafeNormal() * (Item->Front | Item->Axis.GetSafeNormal());
		TestTrue(TEXT("front turns to the grip front"), Rotation.RotateVector(Front.GetSafeNormal()).Equals(Row->ItemFront, 1e-3));
		TestTrue(TEXT("size sets the largest half-extent"),
			FMath::IsNearlyEqual(Placed.GetScale3D().X * Bounds.GetExtent().GetMax(), double(Item->Size), 1e-3));
		// The anchor (bounds centre plus offset) lands on the pose point, moved by lead and slide.
		const double Along = Row->ItemLead * (Item->Size - Row->ItemHalfLength) + Item->Slide;
		TestTrue(TEXT("anchor lands on the grip point"),
			Placed.TransformPosition(Bounds.GetCenter() + Item->Offset).Equals(Row->ItemPoint + Row->ItemAxis * Along, 1e-3));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPHeldItemNudgeSpec,
	"WoT.FirstPerson.HeldItemGrowsAlongItsLine", MathFlags)
bool FWoTFPHeldItemNudgeSpec::RunTest(const FString&)
{
	const FBox Bounds(FVector(-6), FVector(6));
	const TCHAR* Class = TEXT("AngrealInvFireShield");
	const FWoTHandPoseRow* Row = WoTFirstPerson::FindHandPose(WoTFirstPerson::GripForItem(Class, Bounds));
	if (!TestNotNull(TEXT("orb grip"), Row)) return false;
	WoTFirstPerson::FHeldItemNudge Bigger;
	Bigger.SizeDelta = 2.0;
	const FVector Rest = WoTFirstPerson::HeldItemTransform(*Row, Class, Bounds, 1.0, {}).TransformPosition(Bounds.GetCenter());
	const FVector Grown = WoTFirstPerson::HeldItemTransform(*Row, Class, Bounds, 1.0, Bigger).TransformPosition(Bounds.GetCenter());
	TestTrue(TEXT("a larger item grows away from the hand by its lead"), (Grown - Rest).Equals(Row->ItemAxis * Row->ItemLead * 2.0, 1e-3));
	WoTFirstPerson::FHeldItemNudge Flipped;
	Flipped.bFlip = true;
	const FWoTHeldItemGrip* Item = WoTFirstPerson::FindHeldItemGrip(Class);
	TestTrue(TEXT("flip reverses which end leads"), WoTFirstPerson::HeldItemTransform(*Row, Class, Bounds, 1.0, Flipped)
		.GetRotation().RotateVector(-Item->Axis.GetSafeNormal()).Equals(Row->ItemAxis, 1e-3));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPUnlistedGripSpec,
	"WoT.FirstPerson.UnlistedItemsPickRodOrOrb", MathFlags)
bool FWoTFPUnlistedGripSpec::RunTest(const FString&)
{
	TestEqual(TEXT("a long unlisted item takes the rod grip"),
		WoTFirstPerson::GripForItem(TEXT("UnlistedSpecArtifact"), FBox(FVector(-2, -9, -2), FVector(2, 9, 2))), FName(TEXT("rod")));
	TestEqual(TEXT("a compact unlisted item takes the orb grip"),
		WoTFirstPerson::GripForItem(TEXT("UnlistedSpecArtifact"), FBox(FVector(-4, -6, -5), FVector(4, 6, 5))), FName(TEXT("orb")));
	const FBox Long(FVector(-2, -9, -2), FVector(2, 9, 2));
	const FWoTHandPoseRow* Row = WoTFirstPerson::FindHandPose(TEXT("rod"));
	const FTransform Placed = WoTFirstPerson::HeldItemTransform(*Row, TEXT("UnlistedSpecArtifact"), Long, 1.0, {});
	TestTrue(TEXT("its long axis lies on the line"), Placed.GetRotation().RotateVector(FVector::YAxisVector).Equals(Row->ItemAxis, 1e-3));
	TestTrue(TEXT("it takes the grip's own size"),
		FMath::IsNearlyEqual(Placed.GetScale3D().X * 9.0, double(Row->ItemHalfLength), 1e-3));
	return !HasAnyErrors();
}
#endif
