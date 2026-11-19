#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"

/**
 * Tracked hands: the hand skeleton driven by the headset runtime's hand joints
 * (docs/specs/hands/2026-11-18-tracked-hands.md).
 *
 * Joints follow the engine's EHandKeypoint order (Palm, Wrist, the thumb's four
 * joints, then five joints for each finger). The hand models use the same joint
 * set and the same frame convention as a tracked joint: local +X runs toward
 * the next joint, local +Z out of the nail side. So a bone's local rotation is
 * the rotation between its parent joint and its own joint.
 */
namespace WoTFirstPerson::Tracking
{
inline constexpr int32 JointCount = 26;
inline constexpr int32 PalmJoint = 0;
inline constexpr int32 WristJoint = 1;

/** The joint a joint hangs from: the wrist for the palm and every metacarpal,
 *  the previous joint of its chain otherwise; INDEX_NONE for the wrist. */
int32 ParentJoint(int32 Joint);
/** The hand skeleton's bone for a joint (wrist_r, palm_r, index_proximal_r ...). */
FName BoneForJoint(int32 Joint);
/** The joint a bone stands for, or INDEX_NONE. */
int32 JointForBone(FName Bone);
/** True for the five tip joints (they end a chain and carry no rotation of their own). */
bool IsTipJoint(int32 Joint);

/** One hand's joints in world space. */
struct FJoints
{
	bool bTracked = false;
	FVector Positions[JointCount];
	FQuat Rotations[JointCount];
	FJoints();
};

/** A constant rotation per joint, applied on the joint's own side of the
 *  mapping; identity by default. It absorbs differences between a runtime's
 *  joint frames and a model's bone frames, and is what the developer tunes. */
struct FFixes
{
	FQuat Fix[JointCount];
	FFixes();
};

/** The local rotation of the bone for Joint under the bone for Parent:
 *  inverse(Rotations[Parent] * Fix[Parent]) * (Rotations[Joint] * Fix[Joint]). */
FQuat LocalRotation(const FJoints& Joints, const FFixes& Fixes, int32 Parent, int32 Joint);
/** LocalRotation for every joint under its ParentJoint; identity for the wrist. */
void LocalRotations(const FJoints& Joints, const FFixes& Fixes, FQuat OutLocal[JointCount]);

/** Wrist to middle fingertip, along the joints (the wrist, the middle finger's
 *  five joints): it does not change as the finger bends. */
double ChainLength(const FVector Positions[JointCount]);

/** The hand mesh's world transform, uniformly scaled by Scale, that puts its
 *  wrist bone (at WristInComponent in the mesh's component space) on the
 *  tracked wrist joint, turned by the wrist's fix. */
FTransform ActorPlacement(const FJoints& Joints, const FFixes& Fixes, const FTransform& WristInComponent, double Scale);

/** The right hand's live joints from the first registered hand tracker that
 *  reports them; false (and Out.bTracked false) when none does. */
bool ReadLive(FJoints& Out);
/** Replaces the hand trackers in ReadLive (specs); an empty function restores them. */
void SetLiveSourceOverride(TFunction<bool(FJoints&)> Source);
/** The registered hand trackers, for the developer dump. */
FString DescribeTrackers();

/** A canonical right hand (fingers along +X, thumb toward -Y, back of the hand
 *  +Z, centimetres) whose fingers and thumb close by Curl (0 open, 1 closed),
 *  placed at Wrist. Positions agree with the rotations. */
FJoints Synthetic(const FTransform& Wrist, double Curl);

/** A bone rotation relative to its reference pose as [flex, spread, twist]
 *  degrees, the form Data/hand_poses.json uses: Relative = Rx(twist) * Rz(spread) * Ry(flex). */
FVector FlexSpreadTwist(const FQuat& Relative);
FQuat FromFlexSpreadTwist(const FVector& FlexSpreadTwistDegrees);

/** A captured hand pose: per bone, its rotation relative to the reference pose
 *  as [flex, spread, twist]; the tracked wrist relative to the view; the scale. */
struct FCapturedPose
{
	TMap<FName, FVector> Bones;
	FVector WristLocation = FVector::ZeroVector;
	FRotator WristRotation = FRotator::ZeroRotator;
	double Scale = 1.0;
};

TSharedRef<FJsonObject> FixesToJson(const FFixes& Fixes);
bool FixesFromJson(const TSharedPtr<FJsonObject>& Object, FFixes& Out);
TSharedRef<FJsonObject> PoseToJson(const FCapturedPose& Pose);
bool PoseFromJson(const TSharedPtr<FJsonObject>& Object, FCapturedPose& Out);
FString JsonToString(const TSharedRef<FJsonObject>& Object);
TSharedPtr<FJsonObject> JsonFromString(const FString& Text);
}
