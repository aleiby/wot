#pragma once

#include "CoreMinimal.h"
#include "WoTFirstPersonCameraModifier.h"
#include "WoTFirstPersonFraming.h"
#include "WoTFirstPersonGrip.h"
#include "WoTFirstPersonMotion.h"
#include "WoTFirstPersonTuning.h"
#include "WoTHandTracking.h"

class APawn;
class IConsoleObject;
class UPoseableMeshComponent;
class UWorld;

/** One primary local player's cosmetic model; owns all transient resources. */
class FWoTFirstPersonPresenter final
{
public:
	FWoTFirstPersonPresenter();
	~FWoTFirstPersonPresenter();

	FWoTFirstPersonPresenter(const FWoTFirstPersonPresenter&) = delete;
	FWoTFirstPersonPresenter& operator=(const FWoTFirstPersonPresenter&) = delete;
	TOptional<float> UpdateModel(float DeltaTime, const FMinimalViewInfo& View);

private:
	void BeforeActors(UWorld* World, ELevelTick TickType, float DeltaTime);
	void WorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	void WorldTearingDown(UWorld* World);
	void Reconcile(UWorld* World);
	void Clear();
	void DestroyModel();
	bool SpawnModel(FName ClassName);
	void AdvanceSwitch(double DeltaTime);
	double SwitchLowering() const;
	WoTFirstPerson::FFit CurrentFit() const;
	void HandleFirePressed(FName ClassName);
	void HandleFireReleased();
	// Hands: an optional hand-and-forearm the held item rides in (grip_r).
	void EnsureHand();
	void DestroyHand();
	void PoseFingers(double Grip);
	void LightHand(const FMinimalViewInfo& View);
	// The hand's view-fixed key light (WoTFirstPersonTuning.h; wot.fp.handlight tunes it).
	FVector HandKeyOffset = FVector(WoTFirstPerson::Tuning::HandKeyX, WoTFirstPerson::Tuning::HandKeyY, WoTFirstPerson::Tuning::HandKeyZ);
	double HandKeyIntensity = WoTFirstPerson::Tuning::HandKeyIntensity;
	FLinearColor HandKeyTint = FLinearColor(WoTFirstPerson::Tuning::HandKeyTintR, WoTFirstPerson::Tuning::HandKeyTintG, WoTFirstPerson::Tuning::HandKeyTintB, 1.0f);
	void PlaceHeldItemInHand();
#if !UE_BUILD_SHIPPING
	void NudgePose(const TArray<FString>& Args);
	void SetHandOverride(const TArray<FString>& Args);
	void NudgeHandPose(const TArray<FString>& Args);
	void NudgeGripAngle(const TArray<FString>& Args);
	void DumpHand(const TArray<FString>& Args);
	void SetHandGlow(const TArray<FString>& Args);
	void SetHandLight(const TArray<FString>& Args);
	IConsoleObject* HandLightCommand = nullptr;
	int32 HandGlowOverride = -1;
	double HandScaleGlowOverride = -1;
	IConsoleObject* HandGlowCommand = nullptr;
	IConsoleObject* HandDumpCommand = nullptr;
	IConsoleObject* PoseCommand = nullptr;
	IConsoleObject* HandCommand = nullptr;
	IConsoleObject* HandPoseCommand = nullptr;
	IConsoleObject* GripAngleCommand = nullptr;
	void SetTrackMode(const TArray<FString>& Args);
	void SetTrackFix(const TArray<FString>& Args);
	void DumpTracking(const TArray<FString>& Args);
	void CaptureTrackedPose(const TArray<FString>& Args);
	IConsoleObject* TrackCommand = nullptr;
	IConsoleObject* TrackFixCommand = nullptr;
	IConsoleObject* TrackDumpCommand = nullptr;
	IConsoleObject* PoseCaptureCommand = nullptr;
#endif

	FDelegateHandle FirePressedHandle;
	FDelegateHandle FireReleasedHandle;
	FDelegateHandle TickHandle;
	FDelegateHandle CleanupHandle;
	FDelegateHandle TearDownHandle;
	TWeakObjectPtr<UWorld> ActiveWorld;
	TWeakObjectPtr<APawn> Pawn;
	TWeakObjectPtr<APlayerCameraManager> Camera;
	TWeakObjectPtr<AActor> Model;
	FWoTFirstPersonAnchor Anchors[static_cast<uint8>(EWoTFirstPersonHand::Count)];
	FWoTFirstPersonFlatMode FlatMode;
	FWoTFirstPersonMotion Motion;
	FBox Bounds = FBox(ForceInit);
	WoTFirstPerson::FPose Pose = WoTFirstPerson::FindPose(NAME_None);
	TMap<FName, WoTFirstPerson::FPose> PoseOverrides;
	FName ShownClass;
	FName DesiredClass;
	FName HeldClass;
	enum class ESwitch : uint8 { Still, Lower, Raise };
	ESwitch Switch = ESwitch::Still;
	double SwitchTime = 0;
	double LowerStart = 0;

	TWeakObjectPtr<AActor> HandActor;
	TWeakObjectPtr<UPoseableMeshComponent> HandMesh;
	FName HandShownClass;
	int32 HandShownColor = -1;
	FName HandClassOverride;
	int32 HandColorOverride = -1;
	FBox HandBounds = FBox(ForceInit);
	WoTFirstPerson::FPose HandPose;
	// The hand's camera-space aim (pitch, yaw, roll), loaded from the selected
	// grip pose (Data/hand_poses.json) and adjustable with wot.fp.handpose.
	double WandPitch = 0;
	double WandYaw = 0;
	double HandRoll = 0;
	// Dev adjustments to how the held item sits in the grip (wot.fp.handpose,
	// wot.fp.gripangle); reset when the grip changes, zero when shipped.
	WoTFirstPerson::FHeldItemNudge ItemNudge;
	void UpdateHandRotation();
	// Authored hand poses (generated from Data/hand_poses.json): per pose, the
	// bones it rotates and the local rotation applied on top of the reference
	// pose. The grip pose follows the held item (rod, figurine, orb or disc).
	struct FResolvedHandPose { TArray<int32> Bones; TArray<FQuat> Rotations; };
	TMap<FName, FResolvedHandPose> HandPoses;
	TArray<int32> PosedBones;
	FName GripPoseName;
	void SelectGripPose(FName Name);
	FQuat HandPoseRotation(FName PoseName, int32 Bone) const;
	TArray<FTransform> HandRefPose;

	// Tracked hands (docs/specs/hands/2026-11-18-tracked-hands.md): with a
	// tracking source the hand follows the tracked joints in the world, and
	// none of the flat framing applies. Off unless a developer turns it on.
	enum class ETrackMode : uint8 { Off, Live, Synthetic };
	ETrackMode TrackMode = ETrackMode::Off;
	double SyntheticCurl = 0.5;
	WoTFirstPerson::Tracking::FFixes TrackFixes;
	WoTFirstPerson::Tracking::FJoints LastJoints;
	FTransform LastView;
	double LastTrackScale = 1.0;
	bool bTracked = false;
	// Every hand bone driven by a joint, with the joint of its parent bone.
	struct FTrackedBone { int32 Bone = INDEX_NONE; int32 Joint = INDEX_NONE; int32 Parent = INDEX_NONE; };
	TArray<FTrackedBone> TrackedBones;
	TWeakObjectPtr<UPoseableMeshComponent> TrackedMesh;
	FTransform WristInComponent;
	double ModelChainLength = 0;
	bool ReadTrackedJoints(const FMinimalViewInfo& View, WoTFirstPerson::Tracking::FJoints& Out) const;
	bool ResolveTrackedBones();
	void ApplyTracked(const WoTFirstPerson::Tracking::FJoints& Joints, const FMinimalViewInfo& View);
	void EndTracking();
};
