#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP

#include "WoTHandTracking.h"
#include "WoTExtensionSeams.h"
#include "WoTFirstPersonSettings.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "ReferenceSkeleton.h"

namespace
{
constexpr auto TrackSpecFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

// Two rotations are the same when their quaternions agree up to sign.
bool TrackSpecSameRotation(const FQuat& A, const FQuat& B, double Tolerance)
{
	const FQuat Na = A.GetNormalized();
	const FQuat Nb = B.GetNormalized();
	const FQuat Flipped(-Nb.X, -Nb.Y, -Nb.Z, -Nb.W);
	return Na.Equals(Nb, Tolerance) || Na.Equals(Flipped, Tolerance);
}

FQuat TrackSpecTurn(double Pitch, double Yaw, double Roll)
{
	return FRotator(Pitch, Yaw, Roll).Quaternion();
}

// The world rotations of a hand whose bones have the given local rotations
// (identity where none is given), composed down each chain from Wrist.
void TrackSpecCompose(const FQuat& Wrist, const TMap<int32, FQuat>& Locals, WoTFirstPerson::Tracking::FJoints& Out)
{
	// Parents, written out: Palm and every chain's first joint hang from the
	// wrist; the rest from the joint before them.
	static const int32 Parents[WoTFirstPerson::Tracking::JointCount] = {1, INDEX_NONE, 1, 2, 3, 4, 1, 6, 7, 8, 9, 1, 11, 12, 13, 14, 1, 16, 17, 18, 19, 1, 21, 22, 23, 24};
	Out.Rotations[1] = Wrist;
	for (int32 Joint = 0; Joint < WoTFirstPerson::Tracking::JointCount; ++Joint)
	{
		if (Joint == 1) continue;
		const FQuat* Local = Locals.Find(Joint);
		Out.Rotations[Joint] = Out.Rotations[Parents[Joint]] * (Local ? *Local : FQuat::Identity);
	}
	// Each joint three units along its parent's X, so the hand has a size.
	Out.Positions[0] = Out.Positions[1] + Out.Rotations[1].RotateVector(FVector(3, 0, 0));
	for (int32 Joint = 2; Joint < WoTFirstPerson::Tracking::JointCount; ++Joint)
		Out.Positions[Joint] = Out.Positions[Parents[Joint]] + Out.Rotations[Parents[Joint]].RotateVector(FVector(3, 0, 0));
	Out.bTracked = true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPTrackTopologySpec, "WoT.FirstPerson.Tracking.Topology", TrackSpecFlags)
bool FWoTFPTrackTopologySpec::RunTest(const FString&)
{
	using namespace WoTFirstPerson::Tracking;
	struct FRow { int32 Joint; int32 Parent; const TCHAR* Bone; };
	const FRow Rows[] = {
		{0, 1, TEXT("palm_r")}, {1, INDEX_NONE, TEXT("wrist_r")},
		{2, 1, TEXT("thumb_metacarpal_r")}, {3, 2, TEXT("thumb_proximal_r")}, {4, 3, TEXT("thumb_distal_r")}, {5, 4, TEXT("thumb_tip_r")},
		{6, 1, TEXT("index_metacarpal_r")}, {7, 6, TEXT("index_proximal_r")}, {8, 7, TEXT("index_intermediate_r")},
		{9, 8, TEXT("index_distal_r")}, {10, 9, TEXT("index_tip_r")},
		{11, 1, TEXT("middle_metacarpal_r")}, {12, 11, TEXT("middle_proximal_r")}, {13, 12, TEXT("middle_intermediate_r")},
		{14, 13, TEXT("middle_distal_r")}, {15, 14, TEXT("middle_tip_r")},
		{16, 1, TEXT("ring_metacarpal_r")}, {17, 16, TEXT("ring_proximal_r")}, {18, 17, TEXT("ring_intermediate_r")},
		{19, 18, TEXT("ring_distal_r")}, {20, 19, TEXT("ring_tip_r")},
		{21, 1, TEXT("little_metacarpal_r")}, {22, 21, TEXT("little_proximal_r")}, {23, 22, TEXT("little_intermediate_r")},
		{24, 23, TEXT("little_distal_r")}, {25, 24, TEXT("little_tip_r")},
	};
	TestEqual(TEXT("one row per joint"), static_cast<int32>(UE_ARRAY_COUNT(Rows)), JointCount);
	for (const FRow& Row : Rows)
	{
		TestEqual(*FString::Printf(TEXT("parent of joint %d"), Row.Joint), ParentJoint(Row.Joint), Row.Parent);
		TestEqual(*FString::Printf(TEXT("bone of joint %d"), Row.Joint), BoneForJoint(Row.Joint), FName(Row.Bone));
		TestEqual(*FString::Printf(TEXT("joint of bone %s"), Row.Bone), JointForBone(FName(Row.Bone)), Row.Joint);
		TestEqual(*FString::Printf(TEXT("tip flag of joint %d"), Row.Joint), IsTipJoint(Row.Joint),
			Row.Joint == 5 || Row.Joint == 10 || Row.Joint == 15 || Row.Joint == 20 || Row.Joint == 25);
	}
	TestEqual(TEXT("an unknown bone has no joint"), JointForBone(TEXT("forearm_r")), INDEX_NONE);
	TestEqual(TEXT("out of range has no parent"), ParentJoint(JointCount), INDEX_NONE);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPTrackIdentitySpec, "WoT.FirstPerson.Tracking.IdentityJointsGiveIdentityLocals", TrackSpecFlags)
bool FWoTFPTrackIdentitySpec::RunTest(const FString&)
{
	using namespace WoTFirstPerson::Tracking;
	FJoints Joints;
	FQuat Locals[JointCount];
	LocalRotations(Joints, FFixes(), Locals);
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
		TestTrue(*FString::Printf(TEXT("joint %d local is identity"), Joint), TrackSpecSameRotation(Locals[Joint], FQuat::Identity, 1.e-6));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPTrackChainRoundTripSpec, "WoT.FirstPerson.Tracking.ChainRoundTrip", TrackSpecFlags)
bool FWoTFPTrackChainRoundTripSpec::RunTest(const FString&)
{
	using namespace WoTFirstPerson::Tracking;
	TMap<int32, FQuat> Chosen;
	Chosen.Add(6, TrackSpecTurn(4, -6, 2));    // index metacarpal
	Chosen.Add(7, TrackSpecTurn(-35, 9, -4));  // index proximal
	Chosen.Add(8, TrackSpecTurn(-50, 0, 3));   // index intermediate
	Chosen.Add(9, TrackSpecTurn(-25, -2, 0));  // index distal
	Chosen.Add(10, TrackSpecTurn(0, 0, 0));    // index tip
	Chosen.Add(2, TrackSpecTurn(-12, -38, 50)); // thumb metacarpal
	Chosen.Add(3, TrackSpecTurn(-20, 6, -8));  // thumb proximal
	Chosen.Add(4, TrackSpecTurn(-30, 0, 5));   // thumb distal
	FJoints Joints;
	TrackSpecCompose(TrackSpecTurn(20, 130, -35), Chosen, Joints);
	FQuat Locals[JointCount];
	LocalRotations(Joints, FFixes(), Locals);
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
	{
		if (Joint == 1) continue;
		const FQuat* Expected = Chosen.Find(Joint);
		TestTrue(*FString::Printf(TEXT("joint %d local comes back"), Joint),
			TrackSpecSameRotation(Locals[Joint], Expected ? *Expected : FQuat::Identity, 1.e-4));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPTrackFixSidesSpec, "WoT.FirstPerson.Tracking.FixOnBothSides", TrackSpecFlags)
bool FWoTFPTrackFixSidesSpec::RunTest(const FString&)
{
	using namespace WoTFirstPerson::Tracking;
	const FQuat A = TrackSpecTurn(15, -70, 30);  // the wrist
	const FQuat B = TrackSpecTurn(-5, -55, 40);  // the index metacarpal
	const FQuat C = TrackSpecTurn(10, -80, 20);  // the middle metacarpal
	const FQuat Fp = TrackSpecTurn(0, 0, 90);
	const FQuat Fc = TrackSpecTurn(12, -7, 0);
	FJoints Joints;
	Joints.Rotations[1] = A;
	Joints.Rotations[6] = B;
	Joints.Rotations[11] = C;
	FFixes Fixes;
	Fixes.Fix[1] = Fp;
	Fixes.Fix[6] = Fc;
	FQuat Locals[JointCount];
	LocalRotations(Joints, Fixes, Locals);
	TestTrue(TEXT("both fixes apply"), TrackSpecSameRotation(Locals[6], (A * Fp).Inverse() * (B * Fc), 1.e-6));
	TestTrue(TEXT("the parent's fix reaches a sibling too"), TrackSpecSameRotation(Locals[11], (A * Fp).Inverse() * C, 1.e-6));
	TestTrue(TEXT("one pair on its own"), TrackSpecSameRotation(LocalRotation(Joints, Fixes, 1, 6), (A * Fp).Inverse() * (B * Fc), 1.e-6));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPTrackChainLengthSpec, "WoT.FirstPerson.Tracking.ChainLengthIgnoresBending", TrackSpecFlags)
bool FWoTFPTrackChainLengthSpec::RunTest(const FString&)
{
	using namespace WoTFirstPerson::Tracking;
	// Wrist, then the middle finger's metacarpal, proximal, intermediate, distal, tip.
	const double Segments[5] = {7.0, 4.5, 3.0, 2.5, 2.0};
	const int32 Chain[6] = {1, 11, 12, 13, 14, 15};
	FVector Straight[JointCount];
	FVector Bent[JointCount];
	for (int32 Joint = 0; Joint < JointCount; ++Joint) Straight[Joint] = Bent[Joint] = FVector(50, 50, 50);
	FVector Direction(1, 0, 0);
	for (int32 Step = 0; Step < 5; ++Step)
	{
		Straight[Chain[Step + 1]] = Straight[Chain[Step]] + FVector(Segments[Step], 0, 0);
		Bent[Chain[Step + 1]] = Bent[Chain[Step]] + Direction * Segments[Step];
		Direction = FQuat(FVector(0.2, 1, 0).GetSafeNormal(), FMath::DegreesToRadians(-40.0)).RotateVector(Direction);
	}
	TestEqual(TEXT("a straight chain"), ChainLength(Straight), 19.0, 1.e-3);
	TestEqual(TEXT("the same chain bent"), ChainLength(Bent), 19.0, 1.e-3);
	FVector Doubled[JointCount];
	for (int32 Joint = 0; Joint < JointCount; ++Joint) Doubled[Joint] = Bent[Joint] * 2.0;
	TestEqual(TEXT("twice the size"), ChainLength(Doubled), 38.0, 1.e-3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPTrackPlacementSpec, "WoT.FirstPerson.Tracking.PlacementPutsWristOnJoint", TrackSpecFlags)
bool FWoTFPTrackPlacementSpec::RunTest(const FString&)
{
	using namespace WoTFirstPerson::Tracking;
	FJoints Joints;
	Joints.Positions[1] = FVector(12, -30, 95);
	Joints.Rotations[1] = TrackSpecTurn(10, -60, 25);
	FFixes Fixes;
	Fixes.Fix[1] = TrackSpecTurn(0, 0, 90);
	const FTransform WristInComponent(FRotator(5, 15, -80), FVector(3, -2, 7));
	const FTransform Placed = ActorPlacement(Joints, Fixes, WristInComponent, 0.6);
	TestTrue(TEXT("the wrist bone lands on the tracked wrist"),
		Placed.TransformPosition(WristInComponent.GetLocation()).Equals(Joints.Positions[1], 1.e-3));
	TestTrue(TEXT("the wrist bone turns with the tracked wrist and its fix"),
		TrackSpecSameRotation(Placed.GetRotation() * WristInComponent.GetRotation(), Joints.Rotations[1] * Fixes.Fix[1], 1.e-5));
	TestTrue(TEXT("uniform scale"), Placed.GetScale3D().Equals(FVector(0.6), 1.e-6));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPTrackSyntheticSpec, "WoT.FirstPerson.Tracking.SyntheticIsSelfConsistent", TrackSpecFlags)
bool FWoTFPTrackSyntheticSpec::RunTest(const FString&)
{
	using namespace WoTFirstPerson::Tracking;
	const FTransform Wrist(FRotator(15, 40, -20), FVector(10, 20, 30));
	const FJoints Open = Synthetic(Wrist, 0.0);
	const FJoints Half = Synthetic(Wrist, 0.5);
	const FJoints Closed = Synthetic(Wrist, 1.0);
	for (const FJoints* Hand : {&Open, &Half, &Closed})
	{
		TestTrue(TEXT("synthetic joints are tracked"), Hand->bTracked);
		TestTrue(TEXT("the wrist is where it was put"), Hand->Positions[1].Equals(Wrist.GetLocation(), 1.e-6));
	}
	// Each joint sits at a fixed offset in its parent's frame, however far the hand closes.
	const int32 Parents[JointCount] = {1, INDEX_NONE, 1, 2, 3, 4, 1, 6, 7, 8, 9, 1, 11, 12, 13, 14, 1, 16, 17, 18, 19, 1, 21, 22, 23, 24};
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
	{
		if (Joint == 1) continue;
		const int32 Parent = Parents[Joint];
		const FVector OpenOffset = Open.Rotations[Parent].UnrotateVector(Open.Positions[Joint] - Open.Positions[Parent]);
		const FVector HalfOffset = Half.Rotations[Parent].UnrotateVector(Half.Positions[Joint] - Half.Positions[Parent]);
		const FVector ClosedOffset = Closed.Rotations[Parent].UnrotateVector(Closed.Positions[Joint] - Closed.Positions[Parent]);
		TestTrue(*FString::Printf(TEXT("joint %d has a length"), Joint), OpenOffset.Size() > 0.1);
		TestTrue(*FString::Printf(TEXT("joint %d offset at half curl"), Joint), HalfOffset.Equals(OpenOffset, 1.e-3));
		TestTrue(*FString::Printf(TEXT("joint %d offset at full curl"), Joint), ClosedOffset.Equals(OpenOffset, 1.e-3));
	}
	TestTrue(TEXT("a full curl closes the index finger"),
		FMath::RadiansToDegrees(Open.Rotations[8].AngularDistance(Closed.Rotations[8])) > 60.0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPTrackAnglesSpec, "WoT.FirstPerson.Tracking.FlexSpreadTwistRoundTrip", TrackSpecFlags)
bool FWoTFPTrackAnglesSpec::RunTest(const FString&)
{
	using namespace WoTFirstPerson::Tracking;
	const FVector Angles[] = {FVector(30, 10, 5), FVector(-20, -8, 12), FVector(75, -25, -40), FVector::ZeroVector, FVector(110, 5, 0)};
	for (const FVector& Each : Angles)
	{
		const FQuat Built = FromFlexSpreadTwist(Each);
		TestTrue(*FString::Printf(TEXT("%s reads back"), *Each.ToString()), FlexSpreadTwist(Built).Equals(Each, 1.e-3));
		TestTrue(*FString::Printf(TEXT("%s rebuilds"), *Each.ToString()),
			TrackSpecSameRotation(FromFlexSpreadTwist(FlexSpreadTwist(Built)), Built, 1.e-6));
	}
	// The pose generator's own formula: Rx(twist) * Rz(spread) * Ry(flex), each a
	// half-angle quaternion about its axis.
	auto Half = [](double Degrees) { return FMath::DegreesToRadians(Degrees) * 0.5; };
	TestTrue(TEXT("flex 40 is a turn about Y"), TrackSpecSameRotation(FromFlexSpreadTwist(FVector(40, 0, 0)),
		FQuat(0, FMath::Sin(Half(40)), 0, FMath::Cos(Half(40))), 1.e-6));
	TestTrue(TEXT("spread 10 is a turn about Z"), TrackSpecSameRotation(FromFlexSpreadTwist(FVector(0, 10, 0)),
		FQuat(0, 0, FMath::Sin(Half(10)), FMath::Cos(Half(10))), 1.e-6));
	TestTrue(TEXT("twist 15 is a turn about X"), TrackSpecSameRotation(FromFlexSpreadTwist(FVector(0, 0, 15)),
		FQuat(FMath::Sin(Half(15)), 0, 0, FMath::Cos(Half(15))), 1.e-6));
	// Two compound rotations as the generator writes them into Data/hand_poses.json.
	TestTrue(TEXT("[30, 10, 5] as the generator writes it"), TrackSpecSameRotation(FromFlexSpreadTwist(FVector(30, 10, 5)),
		FQuat(0.019436667, 0.253916619, 0.095352425, 0.962318285), 1.e-6));
	TestTrue(TEXT("[-20, -8, 12] as the generator writes it"), TrackSpecSameRotation(FromFlexSpreadTwist(FVector(-20, -8, 12)),
		FQuat(0.090642955, -0.165095472, -0.086427350, 0.978293235), 1.e-6));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPTrackFixesJsonSpec, "WoT.FirstPerson.Tracking.FixesJsonRoundTrip", TrackSpecFlags)
bool FWoTFPTrackFixesJsonSpec::RunTest(const FString&)
{
	using namespace WoTFirstPerson::Tracking;
	FFixes Fixes;
	Fixes.Fix[1] = TrackSpecTurn(0, 0, 90);
	Fixes.Fix[7] = TrackSpecTurn(-12.5, 4, 0);
	Fixes.Fix[25] = TrackSpecTurn(3, -170, 45);
	const TSharedRef<FJsonObject> Written = FixesToJson(Fixes);
	TestEqual(TEXT("only the fixes that turn are written"), Written->Values.Num(), 3);
	TestTrue(TEXT("keyed by bone"), Written->HasField(TEXT("index_proximal_r")));
	FFixes Read;
	if (!TestTrue(TEXT("reads back"), FixesFromJson(JsonFromString(JsonToString(Written)), Read))) return false;
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
		TestTrue(*FString::Printf(TEXT("joint %d fix"), Joint),
			FMath::RadiansToDegrees(Read.Fix[Joint].AngularDistance(Fixes.Fix[Joint])) < 0.01);
	FFixes Untouched;
	Untouched.Fix[3] = TrackSpecTurn(1, 2, 3);
	TestFalse(TEXT("an unknown bone is refused"),
		FixesFromJson(JsonFromString(TEXT("{\"forearm_r\": [0, 0, 90]}")), Untouched));
	TestFalse(TEXT("a short entry is refused"),
		FixesFromJson(JsonFromString(TEXT("{\"wrist_r\": [0, 90]}")), Untouched));
	TestTrue(TEXT("a refused file changes nothing"), TrackSpecSameRotation(Untouched.Fix[3], TrackSpecTurn(1, 2, 3), 1.e-9));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPTrackPoseJsonSpec, "WoT.FirstPerson.Tracking.CapturedPoseJsonRoundTrip", TrackSpecFlags)
bool FWoTFPTrackPoseJsonSpec::RunTest(const FString&)
{
	using namespace WoTFirstPerson::Tracking;
	FCapturedPose Pose;
	Pose.Bones.Add(TEXT("index_proximal_r"), FVector(42.5, -3, 1.25));
	Pose.Bones.Add(TEXT("thumb_metacarpal_r"), FVector(-10, 35, -60));
	Pose.Bones.Add(TEXT("little_distal_r"), FVector(70, 0, 0));
	Pose.WristLocation = FVector(31.5, 6.25, -11);
	Pose.WristRotation = FRotator(-20, 15, 80);
	Pose.Scale = 0.57;
	FCapturedPose Read;
	if (!TestTrue(TEXT("reads back"), PoseFromJson(JsonFromString(JsonToString(PoseToJson(Pose))), Read))) return false;
	TestEqual(TEXT("every bone"), Read.Bones.Num(), Pose.Bones.Num());
	for (const TPair<FName, FVector>& Bone : Pose.Bones)
	{
		const FVector* Angles = Read.Bones.Find(Bone.Key);
		TestTrue(*FString::Printf(TEXT("%s angles"), *Bone.Key.ToString()), Angles && Angles->Equals(Bone.Value, 0.01));
	}
	TestTrue(TEXT("wrist location"), Read.WristLocation.Equals(Pose.WristLocation, 0.01));
	TestTrue(TEXT("wrist rotation"), Read.WristRotation.Equals(Pose.WristRotation, 0.01));
	TestEqual(TEXT("scale"), Read.Scale, Pose.Scale, 1.e-6);
	return !HasAnyErrors();
}

namespace
{
// A local player holding the dart in first person, with the game's own hand.
struct FTrackPresenterWorld
{
	FWoTExtensionSeams SavedSeams;
	bool bSavedShow = UWoTFirstPersonSettings::Get().bShowHeldArtifact;
	float SavedMotion = UWoTFirstPersonSettings::Get().Motion;
	UGameInstance* Instance = nullptr;
	UWorld* World = nullptr;
	APlayerController* Controller = nullptr;

	FTrackPresenterWorld()
	{
		UWoTFirstPersonSettings::Get().SetShowHeldArtifact(false);
		SavedSeams = FWoTExtensionSeams::Get();
		Instance = NewObject<UGameInstance>(GEngine);
		Instance->InitializeStandalone();
		World = Instance->GetWorld();
		World->SetGameMode(FURL());
		World->InitializeActorsForPlay(FURL());
		World->BeginPlay();
		UClass* ControllerClass = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTPlayerController"));
		Controller = World->SpawnActorDeferred<APlayerController>(ControllerClass, FTransform::Identity);
		Controller->SetAsLocalPlayerController();
		Controller->FinishSpawning(FTransform::Identity);
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACharacter* Pawn = World->SpawnActor<ACharacter>(FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTCharacter")),
			FVector(0, 0, 100), FRotator::ZeroRotator, Params);
		Controller->Possess(Pawn);
		Controller->SetViewTarget(Pawn);
		Controller->PlayerCameraManager->bUseClientSideCameraUpdates = false;
		FWoTExtensionSeams::Get().SelectedItemClass = []() { return FName(TEXT("AngrealInvDart")); };
		FWoTExtensionSeams::Get().IsFirstPersonViewActive = []() { return true; };
		// The game gives hands to the class pawns only; this plain pawn gets one too.
		FWoTExtensionSeams::Get().HandStyleForPawn = [](const APawn*, FName& OutHandClass, int32& OutColor)
		{
			OutHandClass = TEXT("channeler");
			OutColor = 0;
			return true;
		};
		UWoTFirstPersonSettings::Get().SetMotion(0);
		UWoTFirstPersonSettings::Get().SetShowHeldArtifact(true);
		Command(TEXT("wot.fp.trackfix all"));
	}

	~FTrackPresenterWorld()
	{
		Command(TEXT("wot.fp.track 0"));
		WoTFirstPerson::Tracking::SetLiveSourceOverride(nullptr);
		// Saved fixes, if this machine has any, come back.
		Command(TEXT("wot.fp.trackfix load"));
		UWoTFirstPersonSettings::Get().SetShowHeldArtifact(false);
		if (!World->bIsTearingDown) World->BeginTearingDown();
		World->EndPlay(EEndPlayReason::Quit);
		Instance->Shutdown();
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		FWoTExtensionSeams::Get() = MoveTemp(SavedSeams);
		UWoTFirstPersonSettings::Get().SetMotion(SavedMotion);
		UWoTFirstPersonSettings::Get().SetShowHeldArtifact(bSavedShow);
	}

	void Command(const TCHAR* Text) const
	{
		IConsoleManager::Get().ProcessUserConsoleInput(Text, *GLog, World);
	}

	void Step() const
	{
		FWorldDelegates::OnWorldPreActorTick.Broadcast(World, LEVELTICK_All, 1.0f / 60.0f);
		Controller->PlayerCameraManager->UpdateCamera(1.0f / 60.0f);
	}

	AActor* Find(const TCHAR* ClassName) const
	{
		for (TActorIterator<AActor> It(World); It; ++It)
			if (It->GetClass()->GetFName() == ClassName) return *It;
		return nullptr;
	}
};

bool TrackSpecAllPrimitivesAre(AActor* Actor, EFirstPersonPrimitiveType Type)
{
	if (!Actor) return false;
	TArray<UPrimitiveComponent*> Primitives;
	Actor->GetComponents(Primitives);
	for (const UPrimitiveComponent* Primitive : Primitives)
		if (Primitive->FirstPersonPrimitiveType != Type) return false;
	return Primitives.Num() > 0;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPTrackPresenterSpec, "WoT.FirstPerson.Tracking.PresenterFollowsJointsThenReturns", TrackSpecFlags)
bool FWoTFPTrackPresenterSpec::RunTest(const FString&)
{
	using namespace WoTFirstPerson::Tracking;
	FTrackPresenterWorld Fixture;
	Fixture.Step();
	Fixture.Step();
	AActor* Hand = Fixture.Find(TEXT("WoTHandPresentation"));
	AActor* Item = Fixture.Find(TEXT("WoTItemPresentation"));
	UPoseableMeshComponent* Mesh = Hand ? Hand->FindComponentByClass<UPoseableMeshComponent>() : nullptr;
	if (!Hand || !Item || !Mesh || !Mesh->GetSkinnedAsset())
	{
		AddInfo(TEXT("No hand art in this tree: the tracked-hand presenter check is skipped."));
		return true;
	}
	const FReferenceSkeleton& Skeleton = Mesh->GetSkinnedAsset()->GetRefSkeleton();
	const int32 WristBone = Skeleton.FindBoneIndex(TEXT("wrist_r"));
	const int32 ProximalBone = Skeleton.FindBoneIndex(TEXT("index_proximal_r"));
	if (!TestTrue(TEXT("the hand has the tracked bones"), WristBone != INDEX_NONE && ProximalBone != INDEX_NONE)) return false;
	// The flat hand, to compare with after tracking ends.
	const TArray<FTransform> FlatBones = Mesh->BoneSpaceTransforms;
	const FTransform FlatPlacement = Hand->GetRootComponent()->GetRelativeTransform();
	TestTrue(TEXT("the flat hand renders in first person"), TrackSpecAllPrimitivesAre(Hand, EFirstPersonPrimitiveType::FirstPerson));

	// Tracked joints: the wrist somewhere in front of the player, the index
	// proximal bent by a chosen local rotation, everything else straight.
	const FQuat ChosenLocal = TrackSpecTurn(-30, 8, 4);
	TMap<int32, FQuat> Chosen;
	Chosen.Add(7, ChosenLocal);
	FJoints Joints;
	Joints.Positions[1] = FVector(40, 6, 150);
	TrackSpecCompose(TrackSpecTurn(10, 20, -15), Chosen, Joints);
	bool bHandPresent = true;
	SetLiveSourceOverride([&Joints, &bHandPresent](FJoints& Out)
	{
		if (!bHandPresent) return false;
		Out = Joints;
		return true;
	});
	// The source refers to these locals: drop it before they go.
	ON_SCOPE_EXIT { SetLiveSourceOverride(nullptr); };
	Fixture.Command(TEXT("wot.fp.track 1"));
	Fixture.Step();
	TestTrue(TEXT("the index proximal takes the tracked local rotation"),
		TrackSpecSameRotation(Mesh->BoneSpaceTransforms[ProximalBone].GetRotation(), ChosenLocal, 1.e-4));
	TestTrue(TEXT("bone translations stay the reference pose's"),
		Mesh->BoneSpaceTransforms[ProximalBone].GetLocation().Equals(Skeleton.GetRefBonePose()[ProximalBone].GetLocation(), 1.e-4));
	// The wrist bone's world position, from the reference skeleton and the mesh's placement.
	FTransform WristInComponent = Skeleton.GetRefBonePose()[WristBone];
	for (int32 Parent = Skeleton.GetParentIndex(WristBone); Parent != INDEX_NONE; Parent = Skeleton.GetParentIndex(Parent))
		WristInComponent = WristInComponent * Skeleton.GetRefBonePose()[Parent];
	const FVector WristWorld = Mesh->GetComponentTransform().TransformPosition(WristInComponent.GetLocation());
	TestTrue(*FString::Printf(TEXT("the wrist bone is on the tracked wrist (%s vs %s)"), *WristWorld.ToString(), *Joints.Positions[1].ToString()),
		WristWorld.Equals(Joints.Positions[1], 1.e-2));
	TestTrue(TEXT("the tracked hand renders as world geometry"), TrackSpecAllPrimitivesAre(Hand, EFirstPersonPrimitiveType::None));
	TestTrue(TEXT("so does the item in it"), TrackSpecAllPrimitivesAre(Item, EFirstPersonPrimitiveType::None));
	TestFalse(TEXT("the tracked hand shows"), Hand->IsHidden());

	// The runtime loses the hand: the flat hand comes back on the next update.
	bHandPresent = false;
	Fixture.Step();
	if (!TestEqual(TEXT("same bone count"), Mesh->BoneSpaceTransforms.Num(), FlatBones.Num())) return false;
	for (int32 Bone = 0; Bone < FlatBones.Num(); ++Bone)
		TestTrue(*FString::Printf(TEXT("bone %s back to the flat pose"), *Skeleton.GetBoneName(Bone).ToString()),
			Mesh->BoneSpaceTransforms[Bone].Equals(FlatBones[Bone], 1.e-4));
	TestTrue(TEXT("the flat placement is back"), Hand->GetRootComponent()->GetRelativeTransform().Equals(FlatPlacement, 1.e-3));
	TestTrue(TEXT("the hand renders in first person again"), TrackSpecAllPrimitivesAre(Hand, EFirstPersonPrimitiveType::FirstPerson));
	TestTrue(TEXT("and the item"), TrackSpecAllPrimitivesAre(Item, EFirstPersonPrimitiveType::FirstPerson));
	return !HasAnyErrors();
}
#endif
