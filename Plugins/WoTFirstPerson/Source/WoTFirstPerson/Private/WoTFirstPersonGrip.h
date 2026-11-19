#pragma once

#include "CoreMinimal.h"

// Authored hand poses and how each artifact sits in the hand
// (Tools/import/generate_hand_poses.py compiles Data/hand_poses.json).

/** One bone of a pose: a local rotation applied on top of the reference pose. */
struct FWoTHandBoneRotation
{
	const TCHAR* Bone;
	FQuat Rotation;
};

/** A hand pose; a grip pose also places the hand and the held item. */
struct FWoTHandPoseRow
{
	const TCHAR* Name;
	const FWoTHandBoneRotation* Bones;
	int32 NumBones;
	bool bGrip;
	// The hand's camera-space placement.
	FVector Offset;
	FRotator Rotation;
	float Scale;
	// In grip_r's frame: the line the item lies on, the side turned toward the
	// viewer, the item's centre, a roll about the line, its default half-size
	// and how far its centre moves along the line per cm of extra half-size.
	FVector ItemAxis;
	FVector ItemFront;
	FVector ItemPoint;
	float ItemRoll;
	float ItemHalfLength;
	float ItemLead;
};

/** How one artifact sits in its grip. */
struct FWoTHeldItemGrip
{
	const TCHAR* Class;
	const TCHAR* Grip;
	// The local axis that leads along the grip line and the one turned toward the viewer.
	FVector Axis;
	FVector Front;
	// Half-size in the hand (cm), a slide along the line (cm; positive holds it
	// lower) and an offset (item-local cm) from its bounds centre to the point
	// the grip places.
	float Size;
	float Slide;
	FVector Offset;
};

namespace WoTFirstPerson
{
/** Dev adjustments on top of the authored grip (all zero when shipped). */
struct FHeldItemNudge
{
	double GripAngle = 0;
	double Slide = 0;
	double Roll = 0;
	double SizeDelta = 0;
	bool bFlip = false;
};

const FWoTHandPoseRow* FindHandPose(FName Name);
TConstArrayView<FWoTHandPoseRow> HandPoses();
const FWoTHeldItemGrip* FindHeldItemGrip(FName Class);
TConstArrayView<FWoTHeldItemGrip> HeldItemGrips();

/** The listed grip, else rod for a long item (long axis at least twice the next) and orb otherwise. */
FName GripForItem(FName Class, const FBox& Bounds);

/** The rotation taking one (axis, front) pair onto another; front is kept across the axis. */
FQuat AlignAxes(const FVector& FromAxis, const FVector& FromFront, const FVector& ToAxis, const FVector& ToFront);

/** The held item's transform in grip_r's frame for a grip pose. */
FTransform HeldItemTransform(const FWoTHandPoseRow& Row, FName Class, const FBox& Bounds,
	double ScaleMultiplier, const FHeldItemNudge& Nudge);
}
