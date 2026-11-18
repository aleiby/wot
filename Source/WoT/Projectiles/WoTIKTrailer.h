#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTIKTrailer.generated.h"

// Inverse-kinematics trailer joint: one link in a chain that follows a head actor.
// Each joint snaps to a fixed Dist behind its parent, but its bend away from the
// parent's backward direction is clamped to AngleExt degrees, so the chain trails
// like a jointed tail rather than a rigid pole. Reposition walks the chain from the
// head outward (each joint becomes the next joint's parent). Subclass to bind a mesh
// / collision (see AWoTMashadarSegment). Spec: exports/script/Legend/IKTrailer.uc.
//
// UE1 -> UE5: the original solved this in 16-bit rotator units (UnrealDegreesFactor
// = 65536/360); UE5 FRotator is float degrees, so the joint-bend clamp is a plain
// degree-space NormalizeAxis/ClampAxis with no conversion factor. The tag-based
// editor AutoLink path is omitted — Mashadar builds its chain programmatically.
UCLASS(Abstract)
class AWoTIKTrailer : public AActor
{
	GENERATED_BODY()

public:
	AWoTIKTrailer();

	// How far a joint may bend away from straight-behind-the-parent, in degrees
	// (rolled per joint from [Min,Max] at BeginPlay). Small = stiff, large = floppy.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|IKTrailer") float MinAngleExt = 15.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|IKTrailer") float MaxAngleExt = 45.0f;
	UPROPERTY() float AngleExt = 0.0f;

	// Fixed spacing between two joints.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|IKTrailer") float Dist = 40.0f;

	// The master joint follows HeadActor directly; the rest follow their parent joint.
	UPROPERTY() bool bMaster = false;
	UPROPERTY() TObjectPtr<AActor> HeadActor = nullptr;
	UPROPERTY() TObjectPtr<AWoTIKTrailer> ChildTrailer = nullptr;

	// Reposition this joint (and, recursively, the rest of the chain) relative to Parent.
	void Reposition(AActor* Parent);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

protected:
	// Snap this joint Dist behind Parent along a bend-clamped direction, then face Parent.
	void AngleExtents2(AActor* Parent);
	// Clamp DeltaAngle so it sits within AngleExt degrees of NormalAngle; returns the
	// clamped angle wrapped to [0,360).
	float AdjustAngle(float DeltaAngle, float NormalAngle) const;

	// Called on the master when it has a bMaster flag but no live HeadActor.
	virtual void NotifyLostHeadActor() {}

	// Change monitors, so we only re-solve when the head has actually moved.
	FVector LastHALocation = FVector::ZeroVector;
	FRotator LastHARotation = FRotator::ZeroRotator;
};
