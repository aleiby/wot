#include "WoTHandTracking.h"

#include "Dom/JsonValue.h"
#include "Features/IModularFeatures.h"
#include "HeadMountedDisplayTypes.h"
#include "IHandTracker.h"
#include "InputCoreTypes.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace WoTFirstPerson::Tracking
{
static_assert(JointCount == EHandKeypointCount, "the joint set must match the engine's hand keypoints");

namespace
{
const TCHAR* const TrackBoneNames[JointCount] = {
	TEXT("palm_r"), TEXT("wrist_r"),
	TEXT("thumb_metacarpal_r"), TEXT("thumb_proximal_r"), TEXT("thumb_distal_r"), TEXT("thumb_tip_r"),
	TEXT("index_metacarpal_r"), TEXT("index_proximal_r"), TEXT("index_intermediate_r"), TEXT("index_distal_r"), TEXT("index_tip_r"),
	TEXT("middle_metacarpal_r"), TEXT("middle_proximal_r"), TEXT("middle_intermediate_r"), TEXT("middle_distal_r"), TEXT("middle_tip_r"),
	TEXT("ring_metacarpal_r"), TEXT("ring_proximal_r"), TEXT("ring_intermediate_r"), TEXT("ring_distal_r"), TEXT("ring_tip_r"),
	TEXT("little_metacarpal_r"), TEXT("little_proximal_r"), TEXT("little_intermediate_r"), TEXT("little_distal_r"), TEXT("little_tip_r"),
};

// The synthetic hand: each joint's offset from its parent joint in the parent's
// frame (cm), its rest turn about the parent's Z (the thumb's splay), and how
// far it flexes at a full curl.
struct FTrackSyntheticJoint
{
	FVector Offset;
	double RestYaw;
	double FullCurl;
};
const FTrackSyntheticJoint TrackSyntheticHand[JointCount] = {
	{FVector(4.5, -0.5, 0.0), 0.0, 0.0},    // palm
	{FVector::ZeroVector, 0.0, 0.0},        // wrist
	{FVector(1.5, -2.0, -0.8), -40.0, 0.0}, // thumb
	{FVector(4.0, 0.0, 0.0), 0.0, 50.0},
	{FVector(3.2, 0.0, 0.0), 0.0, 50.0},
	{FVector(2.6, 0.0, 0.0), 0.0, 0.0},
	{FVector(0.5, -1.8, 0.0), 0.0, 0.0},    // index
	{FVector(8.0, -0.4, 0.0), 0.0, 80.0},
	{FVector(4.2, 0.0, 0.0), 0.0, 80.0},
	{FVector(2.6, 0.0, 0.0), 0.0, 80.0},
	{FVector(2.2, 0.0, 0.0), 0.0, 0.0},
	{FVector(0.5, -0.6, 0.0), 0.0, 0.0},    // middle
	{FVector(8.0, 0.0, 0.0), 0.0, 80.0},
	{FVector(4.6, 0.0, 0.0), 0.0, 80.0},
	{FVector(2.8, 0.0, 0.0), 0.0, 80.0},
	{FVector(2.3, 0.0, 0.0), 0.0, 0.0},
	{FVector(0.5, 0.6, 0.0), 0.0, 0.0},     // ring
	{FVector(7.6, 0.3, 0.0), 0.0, 80.0},
	{FVector(4.3, 0.0, 0.0), 0.0, 80.0},
	{FVector(2.7, 0.0, 0.0), 0.0, 80.0},
	{FVector(2.2, 0.0, 0.0), 0.0, 0.0},
	{FVector(0.4, 1.7, 0.0), 0.0, 0.0},     // little
	{FVector(7.0, 0.7, 0.0), 0.0, 80.0},
	{FVector(3.4, 0.0, 0.0), 0.0, 80.0},
	{FVector(2.2, 0.0, 0.0), 0.0, 80.0},
	{FVector(2.0, 0.0, 0.0), 0.0, 0.0},
};

FQuat TrackAbout(const FVector& Axis, double Degrees)
{
	return FQuat(Axis, FMath::DegreesToRadians(Degrees));
}

TFunction<bool(FJoints&)>& TrackLiveOverride()
{
	static TFunction<bool(FJoints&)> Source;
	return Source;
}

TArray<TSharedPtr<FJsonValue>> TrackNumbers(const FVector& Value)
{
	return {MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)};
}

bool TrackReadNumbers(const TSharedPtr<FJsonValue>& Value, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Value.IsValid() || !Value->TryGetArray(Items) || Items->Num() != 3) return false;
	double Read[3] = {0, 0, 0};
	for (int32 Index = 0; Index < 3; ++Index)
		if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetNumber(Read[Index]) || !FMath::IsFinite(Read[Index])) return false;
	Out = FVector(Read[0], Read[1], Read[2]);
	return true;
}
}

FJoints::FJoints()
{
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
	{
		Positions[Joint] = FVector::ZeroVector;
		Rotations[Joint] = FQuat::Identity;
	}
}

FFixes::FFixes()
{
	for (FQuat& Each : Fix) Each = FQuat::Identity;
}

int32 ParentJoint(int32 Joint)
{
	if (Joint == WristJoint || Joint < 0 || Joint >= JointCount) return INDEX_NONE;
	if (Joint == PalmJoint || Joint == 2) return WristJoint;
	if (Joint <= 5) return Joint - 1;
	return (Joint - 6) % 5 == 0 ? WristJoint : Joint - 1;
}

FName BoneForJoint(int32 Joint)
{
	return Joint >= 0 && Joint < JointCount ? FName(TrackBoneNames[Joint]) : NAME_None;
}

int32 JointForBone(FName Bone)
{
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
		if (Bone == FName(TrackBoneNames[Joint])) return Joint;
	return INDEX_NONE;
}

bool IsTipJoint(int32 Joint)
{
	return Joint >= 5 && Joint < JointCount && Joint % 5 == 0;
}

FQuat LocalRotation(const FJoints& Joints, const FFixes& Fixes, int32 Parent, int32 Joint)
{
	const FQuat ParentFrame = Joints.Rotations[Parent] * Fixes.Fix[Parent];
	const FQuat JointFrame = Joints.Rotations[Joint] * Fixes.Fix[Joint];
	return (ParentFrame.Inverse() * JointFrame).GetNormalized();
}

void LocalRotations(const FJoints& Joints, const FFixes& Fixes, FQuat OutLocal[JointCount])
{
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
	{
		const int32 Parent = ParentJoint(Joint);
		OutLocal[Joint] = Parent == INDEX_NONE ? FQuat::Identity : LocalRotation(Joints, Fixes, Parent, Joint);
	}
}

double ChainLength(const FVector Positions[JointCount])
{
	double Length = FVector::Dist(Positions[WristJoint], Positions[11]);
	for (int32 Joint = 12; Joint <= 15; ++Joint) Length += FVector::Dist(Positions[Joint - 1], Positions[Joint]);
	return Length;
}

FTransform ActorPlacement(const FJoints& Joints, const FFixes& Fixes, const FTransform& WristInComponent, double Scale)
{
	const FQuat Wrist = (Joints.Rotations[WristJoint] * Fixes.Fix[WristJoint]).GetNormalized();
	const FQuat Rotation = (Wrist * WristInComponent.GetRotation().Inverse()).GetNormalized();
	const FVector Location = Joints.Positions[WristJoint] - Rotation.RotateVector(WristInComponent.GetLocation() * Scale);
	return FTransform(Rotation, Location, FVector(Scale));
}

bool ReadLive(FJoints& Out)
{
	Out = FJoints();
	if (TrackLiveOverride())
	{
		Out.bTracked = TrackLiveOverride()(Out) && Out.bTracked;
		return Out.bTracked;
	}
	IModularFeatures& Features = IModularFeatures::Get();
	const TArray<IHandTracker*> Trackers =
		Features.GetModularFeatureImplementations<IHandTracker>(IHandTracker::GetModularFeatureName());
	for (IHandTracker* Tracker : Trackers)
	{
		if (!Tracker || !Tracker->IsHandTrackingStateValid()) continue;
		TArray<FVector> Positions;
		TArray<FQuat> Rotations;
		TArray<float> Radii;
		bool bIsTracked = false;
		if (!Tracker->GetAllKeypointStates(EControllerHand::Right, Positions, Rotations, Radii, bIsTracked) || !bIsTracked
			|| Positions.Num() != JointCount || Rotations.Num() != JointCount) continue;
		for (int32 Joint = 0; Joint < JointCount; ++Joint)
		{
			Out.Positions[Joint] = Positions[Joint];
			Out.Rotations[Joint] = Rotations[Joint].GetNormalized();
		}
		Out.bTracked = true;
		return true;
	}
	return false;
}

void SetLiveSourceOverride(TFunction<bool(FJoints&)> Source)
{
	TrackLiveOverride() = MoveTemp(Source);
}

FString DescribeTrackers()
{
	const TArray<IHandTracker*> Trackers =
		IModularFeatures::Get().GetModularFeatureImplementations<IHandTracker>(IHandTracker::GetModularFeatureName());
	FString Text = FString::Printf(TEXT("%d hand tracker(s)"), Trackers.Num());
	for (const IHandTracker* Tracker : Trackers)
		if (Tracker)
			Text += FString::Printf(TEXT("; %s valid=%d"), *Tracker->GetHandTrackerDeviceTypeName().ToString(), Tracker->IsHandTrackingStateValid() ? 1 : 0);
	return Text;
}

FJoints Synthetic(const FTransform& Wrist, double Curl)
{
	FJoints Joints;
	const double Closed = FMath::Clamp(Curl, 0.0, 1.0);
	Joints.Positions[WristJoint] = Wrist.GetLocation();
	Joints.Rotations[WristJoint] = Wrist.GetRotation().GetNormalized();
	// The wrist first, then the palm, then every chain in order: a parent is
	// always placed before its children.
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
	{
		if (Joint == WristJoint) continue;
		const int32 Parent = ParentJoint(Joint);
		const FTrackSyntheticJoint& Spec = TrackSyntheticHand[Joint];
		const FQuat Local = TrackAbout(FVector::ZAxisVector, Spec.RestYaw) * TrackAbout(FVector::YAxisVector, Spec.FullCurl * Closed);
		Joints.Positions[Joint] = Joints.Positions[Parent] + Joints.Rotations[Parent].RotateVector(Spec.Offset);
		Joints.Rotations[Joint] = (Joints.Rotations[Parent] * Local).GetNormalized();
	}
	Joints.bTracked = true;
	return Joints;
}

FVector FlexSpreadTwist(const FQuat& Relative)
{
	// Relative = Rx(twist) * Rz(spread) * Ry(flex); read the angles back from
	// the rotated axes.
	const FQuat Q = Relative.GetNormalized();
	const FVector AxisX = Q.RotateVector(FVector::XAxisVector);
	const FVector AxisY = Q.RotateVector(FVector::YAxisVector);
	const FVector AxisZ = Q.RotateVector(FVector::ZAxisVector);
	const double Spread = FMath::Asin(FMath::Clamp(-AxisY.X, -1.0, 1.0));
	const double Flex = FMath::Atan2(AxisZ.X, AxisX.X);
	const double Twist = FMath::Atan2(AxisY.Z, AxisY.Y);
	return FVector(FMath::RadiansToDegrees(Flex), FMath::RadiansToDegrees(Spread), FMath::RadiansToDegrees(Twist));
}

FQuat FromFlexSpreadTwist(const FVector& FlexSpreadTwistDegrees)
{
	return (TrackAbout(FVector::XAxisVector, FlexSpreadTwistDegrees.Z) * TrackAbout(FVector::ZAxisVector, FlexSpreadTwistDegrees.Y)
		* TrackAbout(FVector::YAxisVector, FlexSpreadTwistDegrees.X)).GetNormalized();
}

TSharedRef<FJsonObject> FixesToJson(const FFixes& Fixes)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
	{
		if (Fixes.Fix[Joint].AngularDistance(FQuat::Identity) < UE_KINDA_SMALL_NUMBER) continue;
		const FRotator Turn = Fixes.Fix[Joint].Rotator();
		Object->SetArrayField(TrackBoneNames[Joint], TrackNumbers(FVector(Turn.Pitch, Turn.Yaw, Turn.Roll)));
	}
	return Object;
}

bool FixesFromJson(const TSharedPtr<FJsonObject>& Object, FFixes& Out)
{
	if (!Object.IsValid()) return false;
	FFixes Read;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
	{
		const int32 Joint = JointForBone(FName(*Field.Key));
		FVector Turn = FVector::ZeroVector;
		if (Joint == INDEX_NONE || !TrackReadNumbers(Field.Value, Turn)) return false;
		Read.Fix[Joint] = FRotator(Turn.X, Turn.Y, Turn.Z).Quaternion();
	}
	Out = Read;
	return true;
}

TSharedRef<FJsonObject> PoseToJson(const FCapturedPose& Pose)
{
	TSharedRef<FJsonObject> Bones = MakeShared<FJsonObject>();
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
		if (const FVector* Angles = Pose.Bones.Find(BoneForJoint(Joint)))
			Bones->SetArrayField(TrackBoneNames[Joint], TrackNumbers(*Angles));
	TSharedRef<FJsonObject> Wrist = MakeShared<FJsonObject>();
	Wrist->SetArrayField(TEXT("location"), TrackNumbers(Pose.WristLocation));
	Wrist->SetArrayField(TEXT("rotation"), TrackNumbers(FVector(Pose.WristRotation.Pitch, Pose.WristRotation.Yaw, Pose.WristRotation.Roll)));
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("bones"), Bones);
	Object->SetObjectField(TEXT("wrist_in_view"), Wrist);
	Object->SetNumberField(TEXT("scale"), Pose.Scale);
	return Object;
}

bool PoseFromJson(const TSharedPtr<FJsonObject>& Object, FCapturedPose& Out)
{
	const TSharedPtr<FJsonObject>* Bones = nullptr;
	const TSharedPtr<FJsonObject>* Wrist = nullptr;
	double Scale = 0;
	if (!Object.IsValid() || !Object->TryGetObjectField(TEXT("bones"), Bones) || !Object->TryGetObjectField(TEXT("wrist_in_view"), Wrist)
		|| !Object->TryGetNumberField(TEXT("scale"), Scale) || !FMath::IsFinite(Scale)) return false;
	FCapturedPose Read;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Bones)->Values)
	{
		FVector Angles = FVector::ZeroVector;
		if (JointForBone(FName(*Field.Key)) == INDEX_NONE || !TrackReadNumbers(Field.Value, Angles)) return false;
		Read.Bones.Add(FName(*Field.Key), Angles);
	}
	FVector Turn = FVector::ZeroVector;
	if (!TrackReadNumbers((*Wrist)->TryGetField(TEXT("location")), Read.WristLocation)
		|| !TrackReadNumbers((*Wrist)->TryGetField(TEXT("rotation")), Turn)) return false;
	Read.WristRotation = FRotator(Turn.X, Turn.Y, Turn.Z);
	Read.Scale = Scale;
	Out = MoveTemp(Read);
	return true;
}

FString JsonToString(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Object, Writer);
	return Text;
}

TSharedPtr<FJsonObject> JsonFromString(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object)) return nullptr;
	return Object;
}
}
