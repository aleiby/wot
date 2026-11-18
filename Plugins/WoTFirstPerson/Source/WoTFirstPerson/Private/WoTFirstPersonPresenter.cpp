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
#include "HAL/IConsoleManager.h"
#include "Math/RotationMatrix.h"
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
#endif
