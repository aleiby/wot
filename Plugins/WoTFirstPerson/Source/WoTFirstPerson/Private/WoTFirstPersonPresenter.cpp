#include "WoTFirstPersonPresenter.h"

#include "WoTExtensionSeams.h"
#include "WoTFirstPersonSettings.h"
#include "WoTFirstPersonTuning.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Math/RotationMatrix.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "String/LexFromString.h"

namespace
{
APlayerController* PrimaryController(UWorld* World)
{
	if (UGameInstance* Instance = World->GetGameInstance())
		if (APlayerController* Controller = Instance->GetFirstLocalPlayerController(World)) return Controller;
	// Headless worlds have local controllers but no ULocalPlayer.
	for (auto It = World->GetPlayerControllerIterator(); It; ++It)
		if (It->IsValid() && It->Get()->IsLocalController()) return It->Get();
	return nullptr;
}

double SwitchFraction(double Time)
{
	using namespace WoTFirstPerson::Tuning;
	// Normalized critically damped step, completing at the authored duration.
	const auto Response = [](double T)
	{
		const double Phase = SpringFrequency * T;
		return 1.0 - (1.0 + Phase) * FMath::Exp(-Phase);
	};
	return Response(FMath::Clamp(Time, 0.0, SwitchSeconds)) / Response(SwitchSeconds);
}

// Tracked hands render as ordinary world geometry: no first-person field of
// view or scale. Only changes a component that differs, so it can run every frame.
void SetPresentationPrimitiveType(AActor* Actor, EFirstPersonPrimitiveType Type)
{
	if (!Actor) return;
	TArray<UPrimitiveComponent*> Primitives;
	Actor->GetComponents(Primitives);
	for (UPrimitiveComponent* Primitive : Primitives)
		if (Primitive->FirstPersonPrimitiveType != Type) Primitive->SetFirstPersonPrimitiveType(Type);
}

#if !UE_BUILD_SHIPPING
FString TrackingFile(const TCHAR* Name)
{
	return FPaths::ProjectSavedDir() / TEXT("WoTFirstPerson") / Name;
}

bool SaveTrackingFile(const TCHAR* Name, const TSharedRef<FJsonObject>& Object)
{
	const FString Path = TrackingFile(Name);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	return FFileHelper::SaveStringToFile(WoTFirstPerson::Tracking::JsonToString(Object), *Path);
}

TSharedPtr<FJsonObject> LoadTrackingFile(const TCHAR* Name)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *TrackingFile(Name))) return nullptr;
	return WoTFirstPerson::Tracking::JsonFromString(Text);
}

const TCHAR* const TrackFixesFile = TEXT("hand_tracking_fixes.json");
const TCHAR* const CapturedPosesFile = TEXT("captured_poses.json");
#endif

}

FWoTFirstPersonPresenter::FWoTFirstPersonPresenter()
{
	// Camera-relative hand placement before the clearance pull (wrist ahead,
	// right and below the eye), and the held item's frame at grip_r.
	HandPose = {TEXT("hand"), FVector(30, 24, -10), FRotator::ZeroRotator, 1.4f};
	SelectGripPose(TEXT("rod"));
	FWoTExtensionSeams& Seams = FWoTExtensionSeams::Get();
	FirePressedHandle = Seams.LocalFirePressed.AddRaw(this, &FWoTFirstPersonPresenter::HandleFirePressed);
	FireReleasedHandle = Seams.LocalFireReleased.AddRaw(this, &FWoTFirstPersonPresenter::HandleFireReleased);
	TickHandle = FWorldDelegates::OnWorldPreActorTick.AddRaw(this, &FWoTFirstPersonPresenter::BeforeActors);
	CleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &FWoTFirstPersonPresenter::WorldCleanup);
	TearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddRaw(this, &FWoTFirstPersonPresenter::WorldTearingDown);
#if !UE_BUILD_SHIPPING
	PoseCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("wot.fp.pose"),
		TEXT("Nudge selected pose: dX dY dZ dPitch dYaw dRoll dScale; prints a CSV row."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FWoTFirstPersonPresenter::NudgePose), ECVF_Default);
	HandCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("wot.fp.hand"),
		TEXT("Hand override: <channeler|darklord|hound|zealot|auto> [colour 0-4, -1 = the player's]."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FWoTFirstPersonPresenter::SetHandOverride), ECVF_Default);
	HandPoseCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("wot.fp.handpose"),
		TEXT("Nudge the hand: dX dY dZ dPitch dYaw dRoll dScale [dItemSlide dItemRoll flipItem dItemSize]; prints the values."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FWoTFirstPersonPresenter::NudgeHandPose), ECVF_Default);
	HandDumpCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("wot.fp.handdump"),
		TEXT("Log the hand's placement and its bones in camera space (cm), for offline pose matching."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FWoTFirstPersonPresenter::DumpHand), ECVF_Default);
	HandLightCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("wot.fp.handlight"),
		TEXT("Hand key light: X Y Z (camera space, cm), intensity (0 turns it off) [R G B tint]."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FWoTFirstPersonPresenter::SetHandLight), ECVF_Default);
	HandGlowCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("wot.fp.handglow"),
		TEXT("Respawn the hand with this ambient glow (0-255) [and ScaleGlow]; -1 restores the defaults."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FWoTFirstPersonPresenter::SetHandGlow), ECVF_Default);
	GripAngleCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("wot.fp.gripangle"),
		TEXT("Nudge the held item's line about grip_r's Z, in degrees."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FWoTFirstPersonPresenter::NudgeGripAngle), ECVF_Default);
	TrackCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("wot.fp.track"),
		TEXT("Tracked hand: 0 (off), 1 (live joints from the headset runtime) or sim [curl 0-1] (a synthetic hand)."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FWoTFirstPersonPresenter::SetTrackMode), ECVF_Default);
	TrackFixCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("wot.fp.trackfix"),
		TEXT("Tracked hand joint fix: <bone> <pitch> <yaw> <roll>; all (reset every fix); save; load."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FWoTFirstPersonPresenter::SetTrackFix), ECVF_Default);
	TrackDumpCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("wot.fp.trackdump"),
		TEXT("Log the hand trackers, every tracked joint (in view space) and the bone rotation taken from it."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FWoTFirstPersonPresenter::DumpTracking), ECVF_Default);
	PoseCaptureCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("wot.fp.posecapture"),
		TEXT("Save the tracked hand's current pose under <name> in Saved/WoTFirstPerson/captured_poses.json."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FWoTFirstPersonPresenter::CaptureTrackedPose), ECVF_Default);
	// Saved fixes apply from the start; -WoTHandTracking turns live tracking on at launch.
	WoTFirstPerson::Tracking::FixesFromJson(LoadTrackingFile(TrackFixesFile), TrackFixes);
	if (FParse::Param(FCommandLine::Get(), TEXT("WoTHandTracking"))) TrackMode = ETrackMode::Live;
#endif
}

FWoTFirstPersonPresenter::~FWoTFirstPersonPresenter()
{
	FWoTExtensionSeams& Seams = FWoTExtensionSeams::Get();
	Seams.LocalFirePressed.Remove(FirePressedHandle);
	Seams.LocalFireReleased.Remove(FireReleasedHandle);
	FWorldDelegates::OnWorldPreActorTick.Remove(TickHandle);
	FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
	FWorldDelegates::OnWorldBeginTearDown.Remove(TearDownHandle);
#if !UE_BUILD_SHIPPING
	if (PoseCommand) IConsoleManager::Get().UnregisterConsoleObject(PoseCommand, false);
	if (HandCommand) IConsoleManager::Get().UnregisterConsoleObject(HandCommand, false);
	if (HandPoseCommand) IConsoleManager::Get().UnregisterConsoleObject(HandPoseCommand, false);
	if (GripAngleCommand) IConsoleManager::Get().UnregisterConsoleObject(GripAngleCommand, false);
	if (HandGlowCommand) IConsoleManager::Get().UnregisterConsoleObject(HandGlowCommand, false);
	if (HandLightCommand) IConsoleManager::Get().UnregisterConsoleObject(HandLightCommand, false);
	if (HandDumpCommand) IConsoleManager::Get().UnregisterConsoleObject(HandDumpCommand, false);
	if (TrackCommand) IConsoleManager::Get().UnregisterConsoleObject(TrackCommand, false);
	if (TrackFixCommand) IConsoleManager::Get().UnregisterConsoleObject(TrackFixCommand, false);
	if (TrackDumpCommand) IConsoleManager::Get().UnregisterConsoleObject(TrackDumpCommand, false);
	if (PoseCaptureCommand) IConsoleManager::Get().UnregisterConsoleObject(PoseCaptureCommand, false);
#endif
	Clear();
}

void FWoTFirstPersonPresenter::BeforeActors(UWorld* World, ELevelTick, float)
{
	if (World && World->IsGameWorld() && World->GetNetMode() != NM_DedicatedServer) Reconcile(World);
}

void FWoTFirstPersonPresenter::WorldCleanup(UWorld* World, bool, bool)
{
	WorldTearingDown(World);
}

void FWoTFirstPersonPresenter::WorldTearingDown(UWorld* World)
{
	if (ActiveWorld.Get() == World) Clear();
}

void FWoTFirstPersonPresenter::DestroyModel()
{
	if (AActor* Actor = Model.Get()) Actor->Destroy();
	Model.Reset();
	ShownClass = NAME_None;
}

void FWoTFirstPersonPresenter::DestroyHand()
{
	if (AActor* Actor = HandActor.Get()) Actor->Destroy();
	HandActor.Reset();
	HandMesh.Reset();
	HandShownClass = NAME_None;
	HandShownColor = -1;
	HandBounds = FBox(ForceInit);
	HandPoses.Reset();
	PosedBones.Reset();
	HandRefPose.Reset();
	TrackedBones.Reset();
	TrackedMesh.Reset();
	bTracked = false;
}

void FWoTFirstPersonPresenter::Clear()
{
	FlatMode.Hide();
	DestroyModel();
	DestroyHand();
	Pawn.Reset();
	Camera.Reset();
	ActiveWorld.Reset();
	for (auto& Anchor : Anchors) Anchor = FWoTFirstPersonAnchor();
	DesiredClass = NAME_None;
	HeldClass = NAME_None;
	Switch = ESwitch::Still;
	SwitchTime = 0;
	LowerStart = 0;
	Motion = FWoTFirstPersonMotion();
}

void FWoTFirstPersonPresenter::Reconcile(UWorld* World)
{
	if (!World || World->bIsTearingDown) { Clear(); return; }
	APlayerController* Controller = PrimaryController(World);
	if (!Controller)
	{
		if (ActiveWorld.Get() == World) Clear();
		return;
	}
	APawn* CurrentPawn = Controller->GetPawn();
	APlayerCameraManager* Manager = Controller->PlayerCameraManager;
	auto& Seams = FWoTExtensionSeams::Get();
	const FName Selected = Seams.SelectedItemClass ? Seams.SelectedItemClass() : NAME_None;
	if (!UWoTFirstPersonSettings::Get().bShowHeldArtifact || !IsValid(CurrentPawn) || !IsValid(Manager)
		|| !Seams.IsFirstPersonViewActive || !Seams.IsFirstPersonViewActive() || Selected.IsNone()
		|| (Manager->PendingViewTarget.Target && Manager->BlendTimeToGo > 0))
	{
		Clear();
		return;
	}
	if (Pawn.Get() != CurrentPawn || Camera.Get() != Manager || ActiveWorld.Get() != World)
	{
		Clear();
		Pawn = CurrentPawn;
		Camera = Manager;
		ActiveWorld = World;
		Anchors[static_cast<uint8>(EWoTFirstPersonHand::Primary)] = FlatMode.Anchor(Manager, EWoTFirstPersonHand::Primary);
	}
	EnsureHand();
	DesiredClass = Selected;
	if (!HeldClass.IsNone() && HeldClass != Selected) HandleFireReleased();
	if (!Model.IsValid())
	{
		if (!SpawnModel(Selected)) { FlatMode.Hide(); return; }
		Switch = ESwitch::Raise;
		SwitchTime = 0;
	}
	else if (Selected != ShownClass && Switch != ESwitch::Lower)
	{
		LowerStart = SwitchLowering();
		Switch = ESwitch::Lower;
		SwitchTime = 0;
	}
	if (!CurrentFit().bVisible) { Clear(); return; }
	FlatMode.Show(Manager, *this);
}

void FWoTFirstPersonPresenter::EnsureHand()
{
	auto& Seams = FWoTExtensionSeams::Get();
	FName WantClass = NAME_None;
	int32 WantColor = 0;
	const bool bStyled = Seams.HandStyleForPawn && Seams.SpawnHandPresentation
		&& Seams.HandStyleForPawn(Pawn.Get(), WantClass, WantColor);
	if (!bStyled)
	{
		if (HandActor.IsValid()) { DestroyHand(); DestroyModel(); }
		return;
	}
	if (!HandClassOverride.IsNone()) WantClass = HandClassOverride;
	if (HandColorOverride >= 0) WantColor = HandColorOverride;
	// One attempt per style: hand art that is unavailable stays unavailable.
	if (HandShownClass == WantClass && HandShownColor == WantColor) return;
	const bool bHadHand = HandActor.IsValid();
	DestroyHand();
	// The held item re-spawns under the new hand (or the camera, without one).
	if (bHadHand || Model.IsValid()) DestroyModel();
	HandShownClass = WantClass;
	HandShownColor = WantColor;
	const auto& Anchor = Anchors[static_cast<uint8>(EWoTFirstPersonHand::Primary)];
	if (!Anchor.Parent.IsValid()) return;
	AActor* Actor = Seams.SpawnHandPresentation(ActiveWorld.Get(), WantClass, WantColor,
		Anchor.Parent->GetComponentTransform());
	if (!Actor) return;
#if !UE_BUILD_SHIPPING
	// wot.fp.handglow: the mesh-light baker's next re-bake picks these up.
	if (FByteProperty* Glow = FindFProperty<FByteProperty>(Actor->GetClass(), TEXT("AmbientGlow")); Glow && HandGlowOverride >= 0)
		Glow->SetPropertyValue_InContainer(Actor, static_cast<uint8>(HandGlowOverride));
	if (FFloatProperty* Scale = FindFProperty<FFloatProperty>(Actor->GetClass(), TEXT("ScaleGlow")); Scale && HandScaleGlowOverride >= 0)
		Scale->SetPropertyValue_InContainer(Actor, static_cast<float>(HandScaleGlowOverride));
#endif
	UPoseableMeshComponent* Mesh = Actor->FindComponentByClass<UPoseableMeshComponent>();
	if (!Mesh || !Mesh->GetSkinnedAsset()
		|| !Actor->AttachToComponent(Anchor.Parent.Get(), FAttachmentTransformRules::KeepRelativeTransform, Anchor.Socket))
	{
		Actor->Destroy();
		return;
	}
	TArray<UPrimitiveComponent*> Primitives;
	Actor->GetComponents(Primitives);
	for (UPrimitiveComponent* Primitive : Primitives)
	{
		Primitive->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
		Primitive->SetHiddenInSceneCapture(true);
		Primitive->SetCastShadow(false);
		Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Primitive->SetGenerateOverlapEvents(false);
	}
	HandActor = Actor;
	HandMesh = Mesh;
	HandBounds = Mesh->GetSkinnedAsset()->GetBounds().GetBox();
	const FReferenceSkeleton& Skeleton = Mesh->GetSkinnedAsset()->GetRefSkeleton();
	HandRefPose = Skeleton.GetRefBonePose();
	HandPoses.Reset();
	PosedBones.Reset();
	for (const FWoTHandPoseRow& Row : WoTFirstPerson::HandPoses())
	{
		FResolvedHandPose& Resolved = HandPoses.Add(FName(Row.Name));
		for (int32 Index = 0; Index < Row.NumBones; ++Index)
		{
			if (!Row.Bones[Index].Bone) continue;
			const int32 Bone = Skeleton.FindBoneIndex(FName(Row.Bones[Index].Bone));
			if (Bone == INDEX_NONE) continue;
			Resolved.Bones.Add(Bone);
			Resolved.Rotations.Add(Row.Bones[Index].Rotation);
			PosedBones.AddUnique(Bone);
		}
	}
}

void FWoTFirstPersonPresenter::SelectGripPose(FName Name)
{
	const FWoTHandPoseRow* Row = WoTFirstPerson::FindHandPose(Name);
	if (!Row || !Row->bGrip || GripPoseName == Name) return;
	GripPoseName = Name;
	HandPose.Offset = Row->Offset;
	HandPose.ScaleMultiplier = Row->Scale;
	WandPitch = Row->Rotation.Pitch;
	WandYaw = Row->Rotation.Yaw;
	HandRoll = Row->Rotation.Roll;
	ItemNudge = {};
	UpdateHandRotation();
}

FQuat FWoTFirstPersonPresenter::HandPoseRotation(FName PoseName, int32 Bone) const
{
	if (const FResolvedHandPose* Resolved = HandPoses.Find(PoseName))
	{
		const int32 Index = Resolved->Bones.IndexOfByKey(Bone);
		if (Index != INDEX_NONE) return Resolved->Rotations[Index];
	}
	return FQuat::Identity;
}

bool FWoTFirstPersonPresenter::SpawnModel(FName ClassName)
{
	DestroyModel();
	auto& Seams = FWoTExtensionSeams::Get();
	const auto& Anchor = Anchors[static_cast<uint8>(EWoTFirstPersonHand::Primary)];
	if (!Anchor.Parent.IsValid() || !Seams.SpawnItemPresentation) return false;
	// The factory bakes lighting at the camera's current zone/leaf.
	AActor* Actor = Seams.SpawnItemPresentation(ActiveWorld.Get(), ClassName, Anchor.Parent->GetComponentTransform());
	if (!Actor) return false;
	Model = Actor;
	// The actor answers with the box of what it draws, which a development and a
	// packaged build agree on. A primitive's own bounds are a culling volume and
	// differ between them, so nothing here is sized from those.
	Bounds = Actor->CalculateComponentsBoundingBoxInLocalSpace(true);
	TArray<UPrimitiveComponent*> Primitives;
	Actor->GetComponents(Primitives);
	for (UPrimitiveComponent* Primitive : Primitives)
	{
		Primitive->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
		Primitive->SetHiddenInSceneCapture(true);
		Primitive->SetCastShadow(false);
		Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Primitive->SetGenerateOverlapEvents(false);
	}
	Pose = PoseOverrides.Contains(ClassName) ? PoseOverrides[ClassName] : WoTFirstPerson::FindPose(ClassName);
	const bool bAttached = HandMesh.IsValid()
		? Actor->AttachToComponent(HandMesh.Get(), FAttachmentTransformRules::KeepRelativeTransform, TEXT("grip_r"))
		: Actor->AttachToComponent(Anchor.Parent.Get(), FAttachmentTransformRules::KeepRelativeTransform, Anchor.Socket);
	if (!Bounds.IsValid || !bAttached)
	{
		DestroyModel();
		return false;
	}
	ShownClass = ClassName;
	if (HandMesh.IsValid()) PlaceHeldItemInHand();
	return true;
}

void FWoTFirstPersonPresenter::UpdateHandRotation()
{
	// The hand's bind frame is the forearm's: X runs from the wrist toward the
	// fingers and Z out of the back of the hand. Aim the forearm, then roll
	// (0 = back of the hand up, +90 = back of the hand to the right).
	HandPose.Rotation = FRotator(WandPitch, WandYaw, HandRoll);
}

void FWoTFirstPersonPresenter::PlaceHeldItemInHand()
{
	AActor* Item = Model.Get();
	if (!Item || !Bounds.IsValid) return;
	// Each artifact names its grip and how it sits in it (Data/hand_poses.json).
	SelectGripPose(WoTFirstPerson::GripForItem(ShownClass, Bounds));
	if (const FWoTHandPoseRow* Row = WoTFirstPerson::FindHandPose(GripPoseName))
		Item->SetActorRelativeTransform(WoTFirstPerson::HeldItemTransform(*Row, ShownClass, Bounds, Pose.ScaleMultiplier, ItemNudge));
}

void FWoTFirstPersonPresenter::PoseFingers(double Grip)
{
	UPoseableMeshComponent* Mesh = HandMesh.Get();
	if (!Mesh || Mesh->BoneSpaceTransforms.Num() != HandRefPose.Num()) return;
	// Each posed bone eases from the relaxed hand toward the grip as the item rises.
	static const FName Relaxed(TEXT("relaxed"));
	for (const int32 Bone : PosedBones)
	{
		FTransform Local = HandRefPose[Bone];
		Local.SetRotation(Local.GetRotation()
			* FQuat::Slerp(HandPoseRotation(Relaxed, Bone), HandPoseRotation(GripPoseName, Bone), Grip));
		Mesh->BoneSpaceTransforms[Bone] = Local;
	}
	Mesh->MarkRefreshTransformDirty();
}

bool FWoTFirstPersonPresenter::ReadTrackedJoints(const FMinimalViewInfo& View, WoTFirstPerson::Tracking::FJoints& Out) const
{
	if (TrackMode == ETrackMode::Synthetic)
	{
		// Ahead of the eye, a little right and below, fingers forward.
		const FTransform Wrist(View.Rotation, View.Location + View.Rotation.RotateVector(FVector(40, 8, -12)));
		Out = WoTFirstPerson::Tracking::Synthetic(Wrist, SyntheticCurl);
		return true;
	}
	return TrackMode == ETrackMode::Live && WoTFirstPerson::Tracking::ReadLive(Out);
}

bool FWoTFirstPersonPresenter::ResolveTrackedBones()
{
	using namespace WoTFirstPerson::Tracking;
	UPoseableMeshComponent* Mesh = HandMesh.Get();
	if (!Mesh || !Mesh->GetSkinnedAsset() || Mesh->BoneSpaceTransforms.Num() != HandRefPose.Num()) return false;
	if (TrackedMesh.Get() == Mesh) return TrackedBones.Num() > 0 && ModelChainLength > 0;
	TrackedMesh = Mesh;
	TrackedBones.Reset();
	ModelChainLength = 0;
	const FReferenceSkeleton& Skeleton = Mesh->GetSkinnedAsset()->GetRefSkeleton();
	const int32 NumBones = Skeleton.GetNum();
	if (NumBones != HandRefPose.Num()) return false;
	TArray<int32> JointOfBone;
	JointOfBone.Init(INDEX_NONE, NumBones);
	TArray<FTransform> ComponentPose;
	ComponentPose.SetNum(NumBones);
	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		JointOfBone[Bone] = JointForBone(Skeleton.GetBoneName(Bone));
		const int32 ParentBone = Skeleton.GetParentIndex(Bone);
		ComponentPose[Bone] = ParentBone == INDEX_NONE ? HandRefPose[Bone] : HandRefPose[Bone] * ComponentPose[ParentBone];
	}
	const int32 WristBone = Skeleton.FindBoneIndex(BoneForJoint(WristJoint));
	if (WristBone == INDEX_NONE) return false;
	WristInComponent = ComponentPose[WristBone];
	FVector Positions[JointCount];
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
	{
		const int32 Bone = Skeleton.FindBoneIndex(BoneForJoint(Joint));
		Positions[Joint] = Bone == INDEX_NONE ? FVector::ZeroVector : ComponentPose[Bone].GetLocation();
	}
	ModelChainLength = ChainLength(Positions);
	// Tips end their chains and keep the reference pose; the wrist is placed,
	// not rotated. A bone is driven only when its parent bone has a joint too.
	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		const int32 Joint = JointOfBone[Bone];
		const int32 ParentBone = Skeleton.GetParentIndex(Bone);
		if (Joint == INDEX_NONE || Joint == WristJoint || IsTipJoint(Joint) || ParentBone == INDEX_NONE
			|| JointOfBone[ParentBone] == INDEX_NONE) continue;
		TrackedBones.Add({Bone, Joint, JointOfBone[ParentBone]});
	}
	return TrackedBones.Num() > 0 && ModelChainLength > 0;
}

void FWoTFirstPersonPresenter::ApplyTracked(const WoTFirstPerson::Tracking::FJoints& Joints, const FMinimalViewInfo& View)
{
	using namespace WoTFirstPerson::Tracking;
	UPoseableMeshComponent* Mesh = HandMesh.Get();
	AActor* Hand = HandActor.Get();
	if (!Mesh || !Hand) return;
	for (const FTrackedBone& Tracked : TrackedBones)
	{
		FTransform Local = HandRefPose[Tracked.Bone];
		Local.SetRotation(LocalRotation(Joints, TrackFixes, Tracked.Parent, Tracked.Joint));
		Mesh->BoneSpaceTransforms[Tracked.Bone] = Local;
	}
	Mesh->MarkRefreshTransformDirty();
	// The player's hand size: tracked wrist-to-fingertip over the model's.
	const double TrackScale = ChainLength(Joints.Positions) / ModelChainLength;
	const FTransform MeshWorld = ActorPlacement(Joints, TrackFixes, WristInComponent, TrackScale > UE_SMALL_NUMBER ? TrackScale : 1.0);
	// The mesh sits under the actor's root: place the actor so the mesh lands.
	const FTransform HandWorld = Mesh == Hand->GetRootComponent() ? MeshWorld : Mesh->GetRelativeTransform().Inverse() * MeshWorld;
	// The hand rides the camera anchor, which reaches this frame's view only
	// after the camera modifiers run: place it relative to the view, not to
	// where the anchor is now.
	Hand->SetActorRelativeTransform(HandWorld.GetRelativeTransform(FTransform(View.Rotation, View.Location)));
	Hand->SetActorHiddenInGame(false);
	SetPresentationPrimitiveType(Hand, EFirstPersonPrimitiveType::None);
	if (AActor* Item = Model.Get())
	{
		Item->SetActorHiddenInGame(false);
		SetPresentationPrimitiveType(Item, EFirstPersonPrimitiveType::None);
		PlaceHeldItemInHand();
	}
	LightHand(View);
	LastJoints = Joints;
	LastView = FTransform(View.Rotation, View.Location);
	LastTrackScale = TrackScale;
	bTracked = true;
}

void FWoTFirstPersonPresenter::EndTracking()
{
	// Back to the flat hand: reference bones (the grip pose goes on top this
	// frame), first-person rendering; the flat path places the actor again.
	if (UPoseableMeshComponent* Mesh = HandMesh.Get(); Mesh && Mesh->BoneSpaceTransforms.Num() == HandRefPose.Num())
	{
		for (const FTrackedBone& Tracked : TrackedBones) Mesh->BoneSpaceTransforms[Tracked.Bone] = HandRefPose[Tracked.Bone];
		Mesh->MarkRefreshTransformDirty();
	}
	SetPresentationPrimitiveType(HandActor.Get(), EFirstPersonPrimitiveType::FirstPerson);
	SetPresentationPrimitiveType(Model.Get(), EFirstPersonPrimitiveType::FirstPerson);
	bTracked = false;
}

void FWoTFirstPersonPresenter::LightHand(const FMinimalViewInfo& View)
{
	UPoseableMeshComponent* Mesh = HandMesh.Get();
	if (!Mesh) return;
	// The mesh-lit material's last light slot carries the key light. The world
	// bake rewrites its slots on a timer, before the camera update that calls
	// this, so the key light holds every frame.
	const FVector Key = View.Location + View.Rotation.RotateVector(HandKeyOffset);
	const FLinearColor Colour(HandKeyIntensity * HandKeyTint.R, HandKeyIntensity * HandKeyTint.G, HandKeyIntensity * HandKeyTint.B, 1.0f);
	for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
	{
		UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(Slot));
		if (!Mid) continue;
		Mid->SetVectorParameterValue(TEXT("MLPos5"), FLinearColor(Key.X, Key.Y, Key.Z, 0.0f));
		Mid->SetScalarParameterValue(TEXT("MLRad5"), HandKeyIntensity > 0 ? 1.0f / WoTFirstPerson::Tuning::HandKeyRadius : 0.0f);
		Mid->SetVectorParameterValue(TEXT("MLCol5"), Colour);
		Mid->SetScalarParameterValue(TEXT("MLWaveGroup5"), -1.0f);
	}
}

double FWoTFirstPersonPresenter::SwitchLowering() const
{
	if (Switch == ESwitch::Lower) return FMath::Lerp(LowerStart, 1.0, SwitchFraction(SwitchTime));
	if (Switch == ESwitch::Raise) return 1.0 - SwitchFraction(SwitchTime);
	return 0;
}

WoTFirstPerson::FFit FWoTFirstPersonPresenter::CurrentFit() const
{
	const APawn* CurrentPawn = Pawn.Get();
	const UCapsuleComponent* Capsule = CurrentPawn ? CurrentPawn->FindComponentByClass<UCapsuleComponent>() : nullptr;
	const bool bHand = HandActor.IsValid();
	return WoTFirstPerson::Fit(bHand ? HandBounds : Bounds, bHand ? HandPose : Pose,
		Capsule ? Capsule->GetScaledCapsuleRadius() : 0,
		WoTFirstPerson::ShownNearPlane(), UWoTFirstPersonSettings::Get().Motion);
}

void FWoTFirstPersonPresenter::AdvanceSwitch(double DeltaTime)
{
	using namespace WoTFirstPerson::Tuning;
	if (UWoTFirstPersonSettings::Get().Motion == 0)
	{
		if (DesiredClass != ShownClass) SpawnModel(DesiredClass);
		Switch = ESwitch::Still;
		return;
	}
	while (Switch != ESwitch::Still && DeltaTime > 0)
	{
		const double Step = FMath::Min(DeltaTime, SwitchSeconds - SwitchTime);
		SwitchTime += Step;
		DeltaTime -= Step;
		if (SwitchTime + UE_SMALL_NUMBER < SwitchSeconds) break;
		if (Switch == ESwitch::Lower)
		{
			SpawnModel(DesiredClass);
			Switch = ESwitch::Raise;
		}
		else Switch = ESwitch::Still;
		SwitchTime = 0;
	}
}

TOptional<float> FWoTFirstPersonPresenter::UpdateModel(float DeltaTime, const FMinimalViewInfo& View)
{
	// Catch selection, possession and visibility changes made during actor ticks.
	Reconcile(ActiveWorld.Get());
	if (!Model.IsValid()) return {};
	AdvanceSwitch(FMath::Max(0.0f, DeltaTime));
	AActor* Item = Model.Get();
	APawn* CurrentPawn = Pawn.Get();
	if (!Item || !CurrentPawn) { FlatMode.Hide(); return {}; }
	const WoTFirstPerson::FFit Fit = CurrentFit();
	if (!Fit.bVisible) { Clear(); return {}; }
	if (HandActor.IsValid() && TrackMode != ETrackMode::Off)
	{
		WoTFirstPerson::Tracking::FJoints Joints;
		if (ReadTrackedJoints(View, Joints) && ResolveTrackedBones())
		{
			ApplyTracked(Joints, View);
			return 1.0f;
		}
	}
	if (bTracked) EndTracking();
	// With a hand, the hand is what the camera frames and the item rides in it.
	AActor* Placed = HandActor.IsValid() ? HandActor.Get() : Item;
	const FBox& PlacedBounds = HandActor.IsValid() ? HandBounds : Bounds;
	const WoTFirstPerson::FPose& PlacedPose = HandActor.IsValid() ? HandPose : Pose;
	const auto& Settings = UWoTFirstPersonSettings::Get();
	const UPawnMovementComponent* Movement = CurrentPawn->GetMovementComponent();
	const double GroundSpeed = Movement && Movement->IsMovingOnGround() ? CurrentPawn->GetVelocity().Size2D() : 0;
	Motion.Step(DeltaTime, View.Rotation, GroundSpeed, Settings.Motion);
	FVector Offset = Motion.Offset() * Fit.Shrink;
	Offset.Z -= WoTFirstPerson::Tuning::SwitchDrop * SwitchLowering() * Settings.Motion;
	const FQuat Rotation = Motion.Rotation().Quaternion() * PlacedPose.Rotation.Quaternion();
	const FVector Location = PlacedPose.Offset + Offset - Rotation.RotateVector(PlacedBounds.GetCenter() * Fit.ModelScale);
	FTransform Relative = FTransform(Rotation, Location, FVector(Fit.ModelScale))
		* Anchors[static_cast<uint8>(EWoTFirstPersonHand::Primary)].Grip;
	// Pull the actual model into the eye's clear space so CPU lighting samples
	// its real position. Only the FOV correction remains in the rendering warp.
	Relative.ScaleTranslation(Fit.Pull);
	Relative.SetScale3D(Relative.GetScale3D() * Fit.Pull);

	const double FOVCorrection = FMath::Tan(FMath::DegreesToRadians(View.FOV * 0.5))
		/ FMath::Tan(FMath::DegreesToRadians(Settings.ModelFieldOfView * 0.5));
	const FVector Centre = Relative.TransformPosition(PlacedBounds.GetCenter()) * FVector(1, FOVCorrection, FOVCorrection);
	const double Radius = PlacedBounds.GetExtent().Size() * Relative.GetScale3D().GetAbsMax() * FMath::Max(1.0, FOVCorrection);
	const double CentreDistance = Centre.Size();
	bool bHidden = false;
	FHitResult Hit;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(WoTFirstPersonClearance), false, CurrentPawn);
	Query.AddIgnoredActor(Item);
	Query.AddIgnoredActor(Placed);
	// The camera anchor still carries last frame's transform during modifiers.
	// Sweep from this frame's POV; attachment follows it after modifiers finish.
	if (ActiveWorld->SweepSingleByChannel(Hit, View.Location, View.Location + View.Rotation.RotateVector(Centre),
		FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(static_cast<float>(Radius + WoTFirstPerson::Tuning::SweepMargin)), Query))
	{
		const double Pull = CentreDistance > UE_SMALL_NUMBER
			? FMath::Clamp(static_cast<double>(Hit.Distance) / CentreDistance, 0.0, 1.0) : 0.0;
		Relative.ScaleTranslation(Pull);
		Relative.SetScale3D(Relative.GetScale3D() * Pull);
		bHidden = (Centre.X - Radius) * Pull <= WoTFirstPerson::ShownNearPlane() + WoTFirstPerson::Tuning::NearMargin;
	}
	Placed->SetActorRelativeTransform(Relative);
	Placed->SetActorHiddenInGame(bHidden);
	if (Placed != Item)
	{
		Item->SetActorHiddenInGame(bHidden);
		PlaceHeldItemInHand();
		PoseFingers(1.0 - 0.7 * SwitchLowering());
		LightHand(View);
	}
	return 1.0f;
}

void FWoTFirstPersonPresenter::HandleFirePressed(FName ClassName)
{
	const auto& Seams = FWoTExtensionSeams::Get();
	if (Model.IsValid() && !ClassName.IsNone() && Seams.SelectedItemClass
		&& Seams.SelectedItemClass() == ClassName)
	{
		// Motion belongs to the hand, so a press during a model transition kicks
		// immediately and its held tremble survives the eventual model swap.
		HeldClass = ClassName;
		Motion.FirePressed();
	}
}

void FWoTFirstPersonPresenter::HandleFireReleased()
{
	Motion.FireReleased();
	HeldClass = NAME_None;
}

#if !UE_BUILD_SHIPPING
void FWoTFirstPersonPresenter::NudgePose(const TArray<FString>& Args)
{
	if (!Model.IsValid() || Args.Num() != 7) return;
	double Values[7];
	for (int32 Index = 0; Index < 7; ++Index)
		if (!LexTryParseString(Values[Index], *Args[Index]) || !FMath::IsFinite(Values[Index])) return;
	auto Updated = Pose;
	Updated.Offset += FVector(Values[0], Values[1], Values[2]);
	Updated.Rotation += FRotator(Values[3], Values[4], Values[5]);
	Updated.ScaleMultiplier += static_cast<float>(Values[6]);
	if (Updated.Offset.X <= 0 || Updated.ScaleMultiplier <= 0) return;
	Pose = Updated;
	PoseOverrides.Add(ShownClass, Pose);
	UE_LOG(LogTemp, Display, TEXT("%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f"),
		*ShownClass.ToString(), Pose.Offset.X, Pose.Offset.Y, Pose.Offset.Z,
		Pose.Rotation.Pitch, Pose.Rotation.Yaw, Pose.Rotation.Roll, Pose.ScaleMultiplier);
}

void FWoTFirstPersonPresenter::SetHandOverride(const TArray<FString>& Args)
{
	if (Args.IsEmpty()) return;
	HandClassOverride = Args[0].Equals(TEXT("auto"), ESearchCase::IgnoreCase) ? NAME_None : FName(*Args[0].ToLower());
	int32 Color = -1;
	if (Args.Num() > 1 && LexTryParseString(Color, *Args[1])) HandColorOverride = FMath::Clamp(Color, -1, 4);
	UE_LOG(LogTemp, Display, TEXT("wot.fp.hand: class %s colour %d"),
		HandClassOverride.IsNone() ? TEXT("auto") : *HandClassOverride.ToString(), HandColorOverride);
}

void FWoTFirstPersonPresenter::NudgeHandPose(const TArray<FString>& Args)
{
	// dX dY dZ dWandPitch dWandYaw dHandRoll dScale [dItemSlide dItemRoll flipItem dItemSize]
	if (Args.Num() != 7 && Args.Num() != 11) return;
	double Values[11] = {};
	for (int32 Index = 0; Index < Args.Num(); ++Index)
		if (!LexTryParseString(Values[Index], *Args[Index]) || !FMath::IsFinite(Values[Index])) return;
	const FVector Offset = HandPose.Offset + FVector(Values[0], Values[1], Values[2]);
	const double Scale = HandPose.ScaleMultiplier + Values[6];
	if (Offset.X <= 0 || Scale <= 0) return;
	HandPose.Offset = Offset;
	HandPose.ScaleMultiplier = static_cast<float>(Scale);
	WandPitch += Values[3];
	WandYaw += Values[4];
	HandRoll += Values[5];
	ItemNudge.Slide += Values[7];
	ItemNudge.Roll += Values[8];
	if (Values[9] != 0) ItemNudge.bFlip = !ItemNudge.bFlip;
	ItemNudge.SizeDelta += Values[10];
	UpdateHandRotation();
	UE_LOG(LogTemp, Display, TEXT("wot.fp.handpose: offset %.2f %.2f %.2f forearm %.1f %.1f roll %.1f scale %.3f slide %.2f itemroll %.1f flip %d size %+.2f gripangle %.1f"),
		HandPose.Offset.X, HandPose.Offset.Y, HandPose.Offset.Z, WandPitch, WandYaw, HandRoll,
		HandPose.ScaleMultiplier, ItemNudge.Slide, ItemNudge.Roll, ItemNudge.bFlip ? 1 : 0, ItemNudge.SizeDelta, ItemNudge.GripAngle);
}

void FWoTFirstPersonPresenter::DumpHand(const TArray<FString>& Args)
{
	UPoseableMeshComponent* Mesh = HandMesh.Get();
	const auto& Anchor = Anchors[static_cast<uint8>(EWoTFirstPersonHand::Primary)];
	if (!Mesh || !Anchor.Parent.IsValid())
	{
		UE_LOG(LogTemp, Display, TEXT("WoTHandDump: no hand"));
		return;
	}
	const FTransform View = Anchor.Parent->GetComponentTransform();
	const FTransform HandInView = Mesh->GetOwner()->GetActorTransform().GetRelativeTransform(View);
	UE_LOG(LogTemp, Display, TEXT("WoTHandDump: item %s grip %s bounds centre %s extent %s"),
		*ShownClass.ToString(), *GripPoseName.ToString(),
		Bounds.IsValid ? *Bounds.GetCenter().ToString() : TEXT("-"), Bounds.IsValid ? *Bounds.GetExtent().ToString() : TEXT("-"));
	const FRotator Rot = HandPose.Rotation;
	UE_LOG(LogTemp, Display, TEXT("WoTHandDump: pose offset %s rotator %s scale %.4f; actor-in-camera loc %s rot %s scale %s; bounds centre %s extent %s"),
		*HandPose.Offset.ToString(), *Rot.ToString(), HandPose.ScaleMultiplier, *HandInView.GetLocation().ToString(),
		*HandInView.Rotator().ToString(), *HandInView.GetScale3D().ToString(), *HandBounds.GetCenter().ToString(), *HandBounds.GetExtent().ToString());
	const FReferenceSkeleton& Skeleton = Mesh->GetSkinnedAsset()->GetRefSkeleton();
	for (int32 Bone = 0; Bone < Skeleton.GetNum(); ++Bone)
	{
		const FTransform BoneWorld = Mesh->GetBoneTransformByName(Skeleton.GetBoneName(Bone), EBoneSpaces::WorldSpace);
		const FTransform InView = BoneWorld.GetRelativeTransform(View);
		UE_LOG(LogTemp, Display, TEXT("WoTHandDump: %s %s"), *Skeleton.GetBoneName(Bone).ToString(), *InView.GetLocation().ToString());
	}
}

void FWoTFirstPersonPresenter::SetHandLight(const TArray<FString>& Args)
{
	// X Y Z intensity [R G B tint]
	double Values[7] = {0, 0, 0, 0, 1, 1, 1};
	if (Args.Num() != 4 && Args.Num() != 7) return;
	for (int32 Index = 0; Index < Args.Num(); ++Index)
		if (!LexTryParseString(Values[Index], *Args[Index]) || !FMath::IsFinite(Values[Index])) return;
	HandKeyOffset = FVector(Values[0], Values[1], Values[2]);
	HandKeyIntensity = FMath::Max(Values[3], 0.0);
	HandKeyTint = FLinearColor(Values[4], Values[5], Values[6], 1.0f);
	UE_LOG(LogTemp, Display, TEXT("wot.fp.handlight: %s intensity %.3f tint %s"), *HandKeyOffset.ToString(), HandKeyIntensity, *HandKeyTint.ToString());
}

void FWoTFirstPersonPresenter::SetHandGlow(const TArray<FString>& Args)
{
	int32 Value = -1;
	double Scale = -1;
	if (Args.Num() < 1 || Args.Num() > 2 || !LexTryParseString(Value, *Args[0])) return;
	if (Args.Num() == 2 && (!LexTryParseString(Scale, *Args[1]) || !FMath::IsFinite(Scale))) return;
	HandGlowOverride = FMath::Clamp(Value, -1, 255);
	HandScaleGlowOverride = Scale < 0 ? -1.0 : Scale;
	// The next reconcile respawns the hand (and its held item) with the new glow.
	HandShownColor = -1;
	UE_LOG(LogTemp, Display, TEXT("wot.fp.handglow: %d scaleglow %.2f"), HandGlowOverride, HandScaleGlowOverride);
}

void FWoTFirstPersonPresenter::NudgeGripAngle(const TArray<FString>& Args)
{
	double Delta = 0;
	if (Args.Num() != 1 || !LexTryParseString(Delta, *Args[0]) || !FMath::IsFinite(Delta)) return;
	ItemNudge.GripAngle += Delta;
	UE_LOG(LogTemp, Display, TEXT("wot.fp.gripangle: %.1f"), ItemNudge.GripAngle);
}

void FWoTFirstPersonPresenter::SetTrackMode(const TArray<FString>& Args)
{
	if (Args.Num() >= 1)
	{
		const FString& Mode = Args[0];
		double Curl = SyntheticCurl;
		if (Mode == TEXT("0") || Mode.Equals(TEXT("off"), ESearchCase::IgnoreCase)) TrackMode = ETrackMode::Off;
		else if (Mode == TEXT("1") || Mode.Equals(TEXT("live"), ESearchCase::IgnoreCase)) TrackMode = ETrackMode::Live;
		else if (Mode.Equals(TEXT("sim"), ESearchCase::IgnoreCase)
			&& (Args.Num() == 1 || (LexTryParseString(Curl, *Args[1]) && FMath::IsFinite(Curl))))
		{
			TrackMode = ETrackMode::Synthetic;
			SyntheticCurl = FMath::Clamp(Curl, 0.0, 1.0);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("wot.fp.track: expected 0, 1 or sim [curl]"));
			return;
		}
	}
	static const TCHAR* const Names[] = {TEXT("off"), TEXT("live"), TEXT("sim")};
	UE_LOG(LogTemp, Display, TEXT("wot.fp.track: %s (curl %.2f); %s"), Names[static_cast<uint8>(TrackMode)], SyntheticCurl,
		*WoTFirstPerson::Tracking::DescribeTrackers());
}

void FWoTFirstPersonPresenter::SetTrackFix(const TArray<FString>& Args)
{
	using namespace WoTFirstPerson::Tracking;
	if (Args.Num() == 1 && Args[0] == TEXT("save"))
	{
		const bool bSaved = SaveTrackingFile(TrackFixesFile, FixesToJson(TrackFixes));
		UE_LOG(LogTemp, Display, TEXT("wot.fp.trackfix: %s %s"), bSaved ? TEXT("saved") : TEXT("could not save"), *TrackingFile(TrackFixesFile));
		return;
	}
	if (Args.Num() == 1 && Args[0] == TEXT("load"))
	{
		const bool bLoaded = FixesFromJson(LoadTrackingFile(TrackFixesFile), TrackFixes);
		UE_LOG(LogTemp, Display, TEXT("wot.fp.trackfix: %s %s"), bLoaded ? TEXT("loaded") : TEXT("could not load"), *TrackingFile(TrackFixesFile));
		return;
	}
	if (Args.Num() == 1 && Args[0] == TEXT("all"))
	{
		TrackFixes = FFixes();
		UE_LOG(LogTemp, Display, TEXT("wot.fp.trackfix: every fix reset"));
		return;
	}
	double Turn[3] = {0, 0, 0};
	const int32 Joint = Args.Num() == 4 ? JointForBone(FName(*Args[0])) : INDEX_NONE;
	for (int32 Index = 0; Joint != INDEX_NONE && Index < 3; ++Index)
	{
		if (!LexTryParseString(Turn[Index], *Args[Index + 1]) || !FMath::IsFinite(Turn[Index]))
		{
			UE_LOG(LogTemp, Warning, TEXT("wot.fp.trackfix: %s is not a number"), *Args[Index + 1]);
			return;
		}
	}
	if (Joint == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("wot.fp.trackfix: expected <bone> <pitch> <yaw> <roll>, all, save or load"));
		return;
	}
	TrackFixes.Fix[Joint] = FRotator(Turn[0], Turn[1], Turn[2]).Quaternion();
	UE_LOG(LogTemp, Display, TEXT("wot.fp.trackfix: %s %s"), *BoneForJoint(Joint).ToString(), *TrackFixes.Fix[Joint].Rotator().ToString());
}

void FWoTFirstPersonPresenter::DumpTracking(const TArray<FString>& Args)
{
	using namespace WoTFirstPerson::Tracking;
	UE_LOG(LogTemp, Display, TEXT("WoTHandTrack: tracked %d scale %.4f model chain %.2f; %s"), bTracked ? 1 : 0, LastTrackScale,
		ModelChainLength, *DescribeTrackers());
	if (!bTracked) return;
	UPoseableMeshComponent* Mesh = HandMesh.Get();
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
	{
		const FVector InView = LastView.InverseTransformPosition(LastJoints.Positions[Joint]);
		FString Bone = TEXT("-");
		for (const FTrackedBone& Tracked : TrackedBones)
			if (Tracked.Joint == Joint && Mesh)
				Bone = FlexSpreadTwist(HandRefPose[Tracked.Bone].GetRotation().Inverse() * Mesh->BoneSpaceTransforms[Tracked.Bone].GetRotation()).ToString();
		UE_LOG(LogTemp, Display, TEXT("WoTHandTrack: %s at %s; flex/spread/twist %s"), *BoneForJoint(Joint).ToString(), *InView.ToString(), *Bone);
	}
}

void FWoTFirstPersonPresenter::CaptureTrackedPose(const TArray<FString>& Args)
{
	using namespace WoTFirstPerson::Tracking;
	UPoseableMeshComponent* Mesh = HandMesh.Get();
	if (Args.Num() != 1 || !bTracked || !Mesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("wot.fp.posecapture: needs a name and a tracked hand"));
		return;
	}
	FCapturedPose Captured;
	for (const FTrackedBone& Tracked : TrackedBones)
		Captured.Bones.Add(BoneForJoint(Tracked.Joint),
			FlexSpreadTwist(HandRefPose[Tracked.Bone].GetRotation().Inverse() * Mesh->BoneSpaceTransforms[Tracked.Bone].GetRotation()));
	const FTransform Wrist((LastJoints.Rotations[WristJoint] * TrackFixes.Fix[WristJoint]).GetNormalized(), LastJoints.Positions[WristJoint]);
	const FTransform WristInView = Wrist.GetRelativeTransform(LastView);
	Captured.WristLocation = WristInView.GetLocation();
	Captured.WristRotation = WristInView.Rotator();
	Captured.Scale = LastTrackScale;
	TSharedPtr<FJsonObject> All = LoadTrackingFile(CapturedPosesFile);
	if (!All.IsValid()) All = MakeShared<FJsonObject>();
	All->SetObjectField(Args[0], PoseToJson(Captured));
	const bool bSaved = SaveTrackingFile(CapturedPosesFile, All.ToSharedRef());
	UE_LOG(LogTemp, Display, TEXT("wot.fp.posecapture: %s %s in %s"), *Args[0], bSaved ? TEXT("saved") : TEXT("could not be saved"),
		*TrackingFile(CapturedPosesFile));
}
#endif
