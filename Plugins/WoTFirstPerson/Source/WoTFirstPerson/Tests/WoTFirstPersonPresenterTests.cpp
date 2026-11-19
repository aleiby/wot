#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP

#include "WoTExtensionSeams.h"
#include "WoTFirstPersonModule.h"
#include "WoTFirstPersonSettings.h"
#include "WoTFirstPersonCameraModifier.h"
#include "WoTFirstPersonFraming.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"

namespace
{
constexpr auto PresenterFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

// Resolve real game classes without making the runtime plugin link game classes.
struct FPresenterWorld
{
	FWoTExtensionSeams Saved;
	bool bSavedShow = UWoTFirstPersonSettings::Get().bShowHeldArtifact;
	float SavedMotion = UWoTFirstPersonSettings::Get().Motion;
	float SavedFOV = UWoTFirstPersonSettings::Get().ModelFieldOfView;
	UGameInstance* Instance = nullptr;
	UWorld* World = nullptr;
	APlayerController* Controller = nullptr;
	ACharacter* Pawn = nullptr;
	FName Selection = TEXT("AngrealInvDart");
	bool bViewActive = true;

	FPresenterWorld()
	{
		UWoTFirstPersonSettings::Get().SetShowHeldArtifact(false);
		Saved = FWoTExtensionSeams::Get();
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
		Pawn = World->SpawnActor<ACharacter>(FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTCharacter")),
			FVector(0, 0, 100), FRotator::ZeroRotator, Params);
		Controller->Possess(Pawn);
		Controller->SetViewTarget(Pawn);
		Controller->PlayerCameraManager->bUseClientSideCameraUpdates = false;
		FWoTExtensionSeams::Get().SelectedItemClass = [this]() { return Selection; };
		FWoTExtensionSeams::Get().IsFirstPersonViewActive = [this]() { return bViewActive; };
		UWoTFirstPersonSettings::Get().SetMotion(1);
		UWoTFirstPersonSettings::Get().SetShowHeldArtifact(true);
	}

	~FPresenterWorld()
	{
		UWoTFirstPersonSettings::Get().SetShowHeldArtifact(false);
		if (!World->bIsTearingDown) World->BeginTearingDown();
		World->EndPlay(EEndPlayReason::Quit);
		Instance->Shutdown();
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		FWoTExtensionSeams::Get() = MoveTemp(Saved);
		UWoTFirstPersonSettings::Get().SetMotion(SavedMotion);
		UWoTFirstPersonSettings::Get().SetModelFieldOfView(SavedFOV);
		UWoTFirstPersonSettings::Get().SetShowHeldArtifact(bSavedShow);
	}

	void Step(float DeltaTime = 1.0f / 60.0f)
	{
		FWorldDelegates::OnWorldPreActorTick.Broadcast(World, LEVELTICK_All, DeltaTime);
		Controller->PlayerCameraManager->UpdateCamera(DeltaTime);
	}

	TArray<AActor*> Models() const
	{
		TArray<AActor*> Result;
		for (TActorIterator<AActor> It(World); It; ++It)
			if (It->GetClass()->GetFName() == TEXT("WoTItemPresentation")) Result.Add(*It);
		return Result;
	}

	UCameraModifier* Modifier() const
	{
		return Controller->PlayerCameraManager->FindCameraModifierByClass(UWoTFirstPersonCameraModifier::StaticClass());
	}

	AActor* BlockingBox(const FVector& Location, const FVector& Extent)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
		Actor->SetRootComponent(Box);
		Actor->AddInstanceComponent(Box);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Box->SetCollisionResponseToAllChannels(ECR_Ignore);
		Box->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		Box->RegisterComponent();
		Actor->SetActorLocation(Location);
		return Actor;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPPresenterAttachmentSpec,
	"WoT.FirstPerson.PresenterAttachmentAndPrimitiveFlags", PresenterFlags)
bool FWoTFPPresenterAttachmentSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step();
	const auto Models = Fixture.Models();
	if (!TestEqual(TEXT("selection creates exactly one factory presentation"), Models.Num(), 1)) return false;
	TestEqual(TEXT("flat anchor is camera transform component"), Models[0]->GetRootComponent()->GetAttachParent(),
		Fixture.Controller->PlayerCameraManager->GetTransformComponent());
	TArray<UPrimitiveComponent*> Primitives;
	Models[0]->GetComponents(Primitives);
	TestTrue(TEXT("real inventory visual has primitives"), Primitives.Num() > 0);
	for (const UPrimitiveComponent* Primitive : Primitives)
	{
		TestTrue(TEXT("primitive uses first-person rendering"), Primitive->FirstPersonPrimitiveType == EFirstPersonPrimitiveType::FirstPerson);
		TestTrue(TEXT("mirror excludes presentation"), Primitive->bHiddenInSceneCapture);
		TestFalse(TEXT("presentation casts no shadows"), Primitive->CastShadow);
		TestEqual(TEXT("presentation has no collision"), Primitive->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	}
	Fixture.Step();
	TestEqual(TEXT("polling the same selection keeps one actor"), Fixture.Models().Num(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPItemSizedFromDrawnBoxSpec,
	"WoT.FirstPerson.ItemIsSizedFromWhatItDraws", PresenterFlags)
bool FWoTFPItemSizedFromDrawnBoxSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	// No hand: the item alone is fitted to the view, from its own box.
	FWoTExtensionSeams::Get().HandStyleForPawn = nullptr;
	Fixture.Step();
	const auto Models = Fixture.Models();
	if (!TestEqual(TEXT("the selected item shows"), Models.Num(), 1)) return false;
	// The dart's vertices at its draw scale. Its culling bounds are a different
	// volume in a development and a packaged build, so the fit must not use them.
	const FBox DartBox(FVector(-1.14, -19.26, -6.0), FVector(1.14, 19.2, 6.0));
	const WoTFirstPerson::FFit Fit = WoTFirstPerson::Fit(DartBox,
		WoTFirstPerson::FindPose(TEXT("AngrealInvDart")),
		Fixture.Pawn->GetCapsuleComponent()->GetScaledCapsuleRadius(),
		WoTFirstPerson::ShownNearPlane(), 1.0);
	if (!TestTrue(TEXT("the dart fits the view"), Fit.bVisible)) return false;
	TestEqual(TEXT("the item is scaled from its drawn box"),
		Models[0]->GetRootComponent()->GetRelativeScale3D().X,
		Fit.ModelScale * Fit.Pull, 1.e-4);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFPSwitchSpec,
	"WoT.FirstPerson.SelectionLowersSwapsRaisesAndClears", PresenterFlags)
bool FWoTFPSwitchSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step(0.13f);
	if (!TestEqual(TEXT("initial model"), Fixture.Models().Num(), 1)) return false;
	TWeakObjectPtr<AActor> Old = Fixture.Models()[0];
	const double StartZ = Old->GetRootComponent()->GetRelativeLocation().Z;
	Fixture.Selection = TEXT("AngrealInvFireball");
	Fixture.Step(0.06f);
	TestTrue(TEXT("old model survives the lowering half"), Old.IsValid());
	TestTrue(TEXT("old model lowers before swap"), Old->GetRootComponent()->GetRelativeLocation().Z < StartZ - 0.75);
	Fixture.Step(0.061f);
	TestFalse(TEXT("old actor destroyed at swap"), Old.IsValid());
	if (!TestEqual(TEXT("swap leaves exactly one actor"), Fixture.Models().Num(), 1)) return false;
	TWeakObjectPtr<AActor> New = Fixture.Models()[0];
	const double LoweredZ = New->GetRootComponent()->GetRelativeLocation().Z;
	Fixture.Step(0.12f);
	TestTrue(TEXT("new model raises in about 0.12 seconds"), New->GetRootComponent()->GetRelativeLocation().Z > LoweredZ + 3);
	Fixture.Selection = NAME_None;
	Fixture.Step(0);
	TestFalse(TEXT("empty selection destroys immediately without a switch delay"), New.IsValid());
	TestEqual(TEXT("empty hand has no model"), Fixture.Models().Num(), 0);
	TestNull(TEXT("empty hand removes modifier"), Fixture.Modifier());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPInactiveSpec,
	"WoT.FirstPerson.ViewInactiveDuringActorTickHidesImmediately", PresenterFlags)
bool FWoFPInactiveSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step();
	if (!TestEqual(TEXT("visible positive control"), Fixture.Models().Num(), 1)) return false;
	Fixture.bViewActive = false;
	Fixture.Controller->PlayerCameraManager->UpdateCamera(0.016f);
	TestEqual(TEXT("late inactive view destroys actor"), Fixture.Models().Num(), 0);
	TestNull(TEXT("late inactive view removes modifier"), Fixture.Modifier());
	TestFalse(TEXT("inactive cache has no first-person override"), Fixture.Controller->PlayerCameraManager->GetCameraCacheView().bUseFirstPersonParameters);
	Fixture.bViewActive = true;
	Fixture.Step();
	TestEqual(TEXT("return to first person restores selection"), Fixture.Models().Num(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPBlendSpec,
	"WoT.FirstPerson.ViewTargetBlendSuppressesModel", PresenterFlags)
bool FWoFPBlendSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step();
	if (!TestEqual(TEXT("visible before blend"), Fixture.Models().Num(), 1)) return false;
	AActor* Target = Fixture.World->SpawnActor<AActor>();
	Fixture.Controller->SetViewTargetWithBlend(Target, 0.2f);
	TestTrue(TEXT("engine starts pending target blend"), Fixture.Controller->PlayerCameraManager->BlendTimeToGo > 0);
	Fixture.Step(0.05f);
	TestEqual(TEXT("blend hides even when view seam is true"), Fixture.Models().Num(), 0);
	TestNull(TEXT("blend removes modifier"), Fixture.Modifier());
	Fixture.Controller->SetViewTarget(Fixture.Pawn);
	Fixture.Step();
	TestEqual(TEXT("cut back restores model"), Fixture.Models().Num(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPToggleModelSpec,
	"WoT.FirstPerson.SwitchOffRemovesModelAndModifierAndRestores", PresenterFlags)
bool FWoFPToggleModelSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step();
	if (!TestEqual(TEXT("visible before disabling"), Fixture.Models().Num(), 1)) return false;
	UWoTFirstPersonSettings::Get().SetShowHeldArtifact(false);
	TestEqual(TEXT("off destroys without waiting for a tick"), Fixture.Models().Num(), 0);
	TestNull(TEXT("off removes modifier without a tick"), Fixture.Modifier());
	Fixture.Step();
	TestEqual(TEXT("off stays empty"), Fixture.Models().Num(), 0);
	UWoTFirstPersonSettings::Get().SetShowHeldArtifact(true);
	Fixture.Step();
	TestEqual(TEXT("on restores selected model"), Fixture.Models().Num(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPCameraFrameSpec,
	"WoT.FirstPerson.CameraCacheAndAttachmentUseSameFrame", PresenterFlags)
bool FWoFPCameraFrameSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	UWoTFirstPersonSettings::Get().SetModelFieldOfView(85);
	Fixture.Step();
	if (!TestEqual(TEXT("camera fixture model"), Fixture.Models().Num(), 1)) return false;
	Fixture.Pawn->SetActorLocation(FVector(180, 50, 120));
	Fixture.Controller->SetControlRotation(FRotator(15, 70, 0));
	Fixture.Step();
	const APlayerCameraManager* Camera = Fixture.Controller->PlayerCameraManager;
	const auto& POV = Camera->GetCameraCacheView();
	TestTrue(TEXT("camera cache enables first-person parameters"), POV.bUseFirstPersonParameters);
	TestEqual(TEXT("camera cache carries live model FOV"), POV.FirstPersonFOV, 85.0f);
	TestEqual(TEXT("camera warp leaves real pull unchanged"), POV.FirstPersonScale, 1.0f);
	TestEqual(TEXT("modifier runs last"), Fixture.Modifier()->Priority, uint8(255));
	const AActor* Model = Fixture.Models()[0];
	TestTrue(TEXT("lighting origin stays within capsule radius of the eye"),
		FVector::Distance(Model->GetActorLocation(), POV.Location) < Fixture.Pawn->GetCapsuleComponent()->GetScaledCapsuleRadius());
	const FTransform Expected = Model->GetRootComponent()->GetRelativeTransform() * FTransform(POV.Rotation, POV.Location);
	TestTrue(TEXT("world transform matches this frame camera and relative pose"), Model->GetActorTransform().Equals(Expected, 0.001));
	TestTrue(TEXT("anchor itself has the final camera location"), Camera->GetTransformComponent()->GetComponentLocation().Equals(POV.Location));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPCeilingClearanceSpec,
	"WoT.FirstPerson.CeilingSweepPullsOrHidesAndRestoresSameActor", PresenterFlags)
bool FWoFPCeilingClearanceSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	UWoTFirstPersonSettings::Get().SetMotion(0);
	APlayerCameraManager* Camera = Fixture.Controller->PlayerCameraManager;
	Camera->ViewPitchMax = 90;
	for (const float ModelFOV : {90.0f, 60.0f})
	{
		Camera->SetFOV(90);
		UWoTFirstPersonSettings::Get().SetModelFieldOfView(ModelFOV);
		Fixture.Controller->SetControlRotation(FRotator(90, 0, 0));
		Fixture.Step();
		if (!TestEqual(TEXT("ceiling positive control has one model"), Fixture.Models().Num(), 1)) return false;
		AActor* Model = Fixture.Models()[0];
		TestFalse(TEXT("clear upward view shows model"), Model->IsHidden());
		const FTransform ClearPose = Model->GetRootComponent()->GetRelativeTransform();
		const FVector Eye = Camera->GetCameraCacheView().Location;
		const double ClearDistance = FVector::Distance(Model->GetActorLocation(), Eye);
		TestTrue(TEXT("ceiling fixture looks straight up"), Camera->GetCameraCacheView().Rotation.Vector().Z > 0.999);
		AActor* Ceiling = Fixture.BlockingBox(Eye + FVector(0, 0, 5), FVector(100, 100, 1));
		// Turn away first: the next sweep must use the incoming view, not the old anchor rotation.
		Fixture.Controller->SetControlRotation(FRotator::ZeroRotator);
		Fixture.Step();
		Fixture.Controller->SetControlRotation(FRotator(90, 0, 0));
		Fixture.Step();
		TestTrue(TEXT("ceiling forces extra pull or hiding"), Model->IsHidden()
			|| FVector::Distance(Model->GetActorLocation(), Eye) < ClearDistance - 0.01);
		Fixture.Step();
		TestTrue(TEXT("ceiling clearance retains the same actor"), Fixture.Models().Num() == 1 && Fixture.Models()[0] == Model);
		Ceiling->Destroy();
		Fixture.Step();
		TestFalse(TEXT("removing ceiling unhides model"), Model->IsHidden());
		TestTrue(TEXT("removing ceiling restores baseline pose"), Model->GetRootComponent()->GetRelativeTransform().Equals(ClearPose, 0.001));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPWallClearanceSpec,
	"WoT.FirstPerson.WallSweepPullsOrHidesAndRestoresSameActor", PresenterFlags)
bool FWoFPWallClearanceSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	UWoTFirstPersonSettings::Get().SetMotion(0);
	Fixture.Controller->PlayerCameraManager->SetFOV(90);
	for (const float ModelFOV : {90.0f, 60.0f})
	{
		UWoTFirstPersonSettings::Get().SetModelFieldOfView(ModelFOV);
		Fixture.Step();
		if (!TestEqual(TEXT("wall positive control has one model"), Fixture.Models().Num(), 1)) return false;
		AActor* Model = Fixture.Models()[0];
		TestFalse(TEXT("clear forward view shows model"), Model->IsHidden());
		const FTransform ClearPose = Model->GetRootComponent()->GetRelativeTransform();
		// Move the eye before updating: querying last frame's camera would miss this wall.
		const FVector Move(200, 0, 0);
		const FVector Eye = Fixture.Controller->PlayerCameraManager->GetCameraCacheView().Location + Move;
		Fixture.Pawn->SetActorLocation(Fixture.Pawn->GetActorLocation() + Move);
		const double ClearDistance = ClearPose.GetTranslation().Size();
		// The clear sphere ends before 12 at equal FOV; 10 intersects its sweep.
		for (const double Distance : {10.0, 4.0, 0.0})
		{
			AActor* Wall = Fixture.BlockingBox(Eye + FVector(Distance + 1, 0, 0), FVector(1, 100, 100));
			Fixture.Step();
			TestTrue(FString::Printf(TEXT("wall at %.1f with model FOV %.1f forces extra pull or hiding (distance %.3f, clear %.3f)"),
				Distance, ModelFOV, FVector::Distance(Model->GetActorLocation(), Eye), ClearDistance), Model->IsHidden()
				|| FVector::Distance(Model->GetActorLocation(), Eye) < ClearDistance - 0.01);
			if (Distance == 10 && ModelFOV == 90)
			{
				TestFalse(TEXT("moderate obstruction keeps the pulled model visible"), Model->IsHidden());
				const FTransform PulledPose = Model->GetRootComponent()->GetRelativeTransform();
				const double Pull = PulledPose.GetTranslation().Size() / ClearDistance;
				TestTrue(TEXT("extra pull shrinks scale and translation together"),
					PulledPose.GetScale3D().Equals(ClearPose.GetScale3D() * Pull, 1.e-6));
			}
			if (Distance <= 4) TestTrue(TEXT("near-plane violation or initial overlap hides model"), Model->IsHidden());
			Fixture.Step();
			TestTrue(TEXT("wall clearance retains the same actor"), Fixture.Models().Num() == 1 && Fixture.Models()[0] == Model);
			Wall->Destroy();
			Fixture.Step();
			TestFalse(TEXT("removing wall unhides model"), Model->IsHidden());
			TestTrue(TEXT("removing wall restores baseline pose"), Model->GetRootComponent()->GetRelativeTransform().Equals(ClearPose, 0.001));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPPawnReplacementSpec,
	"WoT.FirstPerson.PawnReplacementWithSameClassRecreatesModel", PresenterFlags)
bool FWoFPPawnReplacementSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step();
	if (!TestEqual(TEXT("initial pawn model"), Fixture.Models().Num(), 1)) return false;
	TWeakObjectPtr<AActor> Old = Fixture.Models()[0];
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACharacter* Replacement = Fixture.World->SpawnActor<ACharacter>(Fixture.Pawn->GetClass(), FVector(100, 0, 100), FRotator::ZeroRotator, Params);
	Fixture.Controller->Possess(Replacement);
	Fixture.Controller->SetViewTarget(Replacement);
	Fixture.Step();
	TestFalse(TEXT("pawn change destroys the old presentation despite same selection"), Old.IsValid());
	TestEqual(TEXT("new pawn has exactly one model"), Fixture.Models().Num(), 1);
	Replacement->Destroy();
	Fixture.Step();
	TestEqual(TEXT("lost pawn destroys actor"), Fixture.Models().Num(), 0);
	TestNull(TEXT("lost pawn removes modifier"), Fixture.Modifier());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPWorldTeardownSpec,
	"WoT.FirstPerson.WorldTeardownReleasesPresentation", PresenterFlags)
bool FWoFPWorldTeardownSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step();
	if (!TestEqual(TEXT("live world model"), Fixture.Models().Num(), 1)) return false;
	Fixture.World->BeginTearingDown();
	TestEqual(TEXT("teardown immediately destroys model"), Fixture.Models().Num(), 0);
	TestNull(TEXT("teardown immediately removes modifier"), Fixture.Modifier());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPShutdownModelSpec,
	"WoT.FirstPerson.ModuleShutdownReleasesLivePresentation", PresenterFlags)
bool FWoFPShutdownModelSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step();
	if (!TestEqual(TEXT("live module model"), Fixture.Models().Num(), 1)) return false;
	auto& Module = FModuleManager::GetModuleChecked<FWoTFirstPersonModule>(TEXT("WoTFirstPerson"));
	Module.ShutdownModule();
	TestEqual(TEXT("shutdown destroys model"), Fixture.Models().Num(), 0);
	TestNull(TEXT("shutdown removes modifier"), Fixture.Modifier());
	Module.StartupModule();
	Fixture.Step();
	TestEqual(TEXT("startup restores model"), Fixture.Models().Num(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPLateFactorySpec,
	"WoT.FirstPerson.LateVisualResolutionRetriesRealFactory", PresenterFlags)
bool FWoFPLateFactorySpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	const FName ValidSelection = Fixture.Selection;
	Fixture.Selection = TEXT("MissingFirstPersonVisual");
	Fixture.Step();
	TestEqual(TEXT("unresolved class creates nothing"), Fixture.Models().Num(), 0);
	TestNull(TEXT("unresolved visual has no modifier"), Fixture.Modifier());
	Fixture.Selection = ValidSelection;
	Fixture.Step();
	TestEqual(TEXT("resolved selection creates real model"), Fixture.Models().Num(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPSmallCapsuleSpec,
	"WoT.FirstPerson.LiveCapsuleShrinkRefitsAndRecovers", PresenterFlags)
bool FWoFPSmallCapsuleSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step();
	if (!TestEqual(TEXT("normal capsule model"), Fixture.Models().Num(), 1)) return false;
	const FVector InitialScale = Fixture.Models()[0]->GetActorScale3D();
	// Near plane 2 + 1 margin and a 2-unit wall margin: radius 6 leaves a
	// positive but narrow depth band (3..4); radius 4.5 leaves none.
	Fixture.Pawn->GetCapsuleComponent()->SetCapsuleSize(6, 40);
	Fixture.Step();
	if (!TestEqual(TEXT("small capsule keeps fitted model"), Fixture.Models().Num(), 1)) return false;
	TestTrue(TEXT("model shrinks for narrow clearance interval"), Fixture.Models()[0]->GetActorScale3D().X < InitialScale.X);
	Fixture.Pawn->GetCapsuleComponent()->SetCapsuleSize(4.5f, 40);
	Fixture.Step();
	TestEqual(TEXT("impossible interval hides model"), Fixture.Models().Num(), 0);
	TestNull(TEXT("impossible interval removes modifier"), Fixture.Modifier());
	Fixture.Pawn->GetCapsuleComponent()->SetCapsuleSize(17, 40);
	Fixture.Step();
	TestEqual(TEXT("normal radius restores presentation"), Fixture.Models().Num(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPFireSeamSpec,
	"WoT.FirstPerson.FireSeamsKickOnceAndMotionZeroFreezesPose", PresenterFlags)
bool FWoFPFireSeamSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step(0.13f);
	if (!TestEqual(TEXT("fire fixture model"), Fixture.Models().Num(), 1)) return false;
	AActor* Model = Fixture.Models()[0];
	const double RestX = Model->GetRootComponent()->GetRelativeLocation().X;
	auto& Seams = FWoTExtensionSeams::Get();
	Seams.LocalFirePressed.Broadcast(Fixture.Selection);
	Fixture.Step(0.04f);
	TestTrue(TEXT("broadcast causes one kick"), Model->GetRootComponent()->GetRelativeLocation().X < RestX - 0.075);
	Fixture.Step(1.0f);
	TestTrue(TEXT("held fire does not create another kick"), FMath::Abs(Model->GetRootComponent()->GetRelativeLocation().X - RestX) < 0.0015);
	Seams.LocalFireReleased.Broadcast();
	Fixture.Step(0.04f);
	TestTrue(TEXT("release seam settles"), Model->GetRootComponent()->GetRelativeLocation().X > RestX);
	UWoTFirstPersonSettings::Get().SetMotion(0);
	Fixture.Step();
	const FTransform Still = Model->GetRootComponent()->GetRelativeTransform();
	Seams.LocalFirePressed.Broadcast(Fixture.Selection);
	Fixture.Controller->SetControlRotation(FRotator(30, 90, 0));
	Fixture.Step(0.08f);
	TestTrue(TEXT("zero motion freezes relative pose even while firing and turning"), Model->GetRootComponent()->GetRelativeTransform().Equals(Still));
	Seams.LocalFireReleased.Broadcast();
	Fixture.Step();
	TestTrue(TEXT("zero motion release stays still"), Model->GetRootComponent()->GetRelativeTransform().Equals(Still));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPRapidSwitchSpec,
	"WoT.FirstPerson.RapidSelectionReversesWithoutPoseJump", PresenterFlags)
bool FWoFPRapidSwitchSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step(0.04f);
	if (!TestEqual(TEXT("model is partway through initial raise"), Fixture.Models().Num(), 1)) return false;
	AActor* Old = Fixture.Models()[0];
	const double PreviousZ = Old->GetRootComponent()->GetRelativeLocation().Z;
	Fixture.Selection = TEXT("AngrealInvFireball");
	Fixture.Step(0.001f);
	TestTrue(TEXT("reversing raise starts at its current displacement"),
		FMath::Abs(Old->GetRootComponent()->GetRelativeLocation().Z - PreviousZ) < 0.15);
	Fixture.Step(0.12f);
	TestEqual(TEXT("rapid retarget still leaves one model"), Fixture.Models().Num(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPFireDuringSwitchSpec,
	"WoT.FirstPerson.FireDuringSelectionTransitionKeepsFeedback", PresenterFlags)
bool FWoFPFireDuringSwitchSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	Fixture.Step(0.13f);
	if (!TestEqual(TEXT("initial fire-transition model"), Fixture.Models().Num(), 1)) return false;
	AActor* Old = Fixture.Models()[0];
	const double OldRestX = Old->GetRootComponent()->GetRelativeLocation().X;
	auto& Seams = FWoTExtensionSeams::Get();
	Fixture.Selection = TEXT("AngrealInvFireball");
	// Input can arrive before the presenter's next selection poll.
	Seams.LocalFirePressed.Broadcast(Fixture.Selection);
	Fixture.Step(0.04f);
	TestTrue(TEXT("new selection press kicks the visible hand immediately"), Old->GetRootComponent()->GetRelativeLocation().X < OldRestX - 0.075);
	Fixture.Step(0.09f);
	if (!TestEqual(TEXT("new model has replaced the old one"), Fixture.Models().Num(), 1)) return false;
	AActor* New = Fixture.Models()[0];
	Fixture.Step(1.0f);
	const double NewRestX = New->GetRootComponent()->GetRelativeLocation().X;
	const double HeldY = New->GetRootComponent()->GetRelativeLocation().Y;
	Fixture.Step(0.02f);
	TestTrue(TEXT("held tremble survives the model swap"), FMath::Abs(New->GetRootComponent()->GetRelativeLocation().Y - HeldY) > 0.0001);
	Seams.LocalFireReleased.Broadcast();
	Fixture.Step(0.04f);
	TestTrue(TEXT("release after swap still settles the hand"), New->GetRootComponent()->GetRelativeLocation().X > NewRestX + 0.0015);
	Fixture.Step(1.0f);
	Seams.LocalFirePressed.Broadcast(TEXT("AngrealInvDart"));
	Fixture.Step(0.04f);
	TestTrue(TEXT("stale class press cannot kick new selection"), FMath::Abs(New->GetRootComponent()->GetRelativeLocation().X - NewRestX) < 0.0015);
	return !HasAnyErrors();
}

#if !UE_BUILD_SHIPPING
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoFPPoseCommandSpec,
	"WoT.FirstPerson.PoseCommandNudgesAndKeepsClassOverride", PresenterFlags)
bool FWoFPPoseCommandSpec::RunTest(const FString&)
{
	FPresenterWorld Fixture;
	UWoTFirstPersonSettings::Get().SetMotion(0);
	Fixture.Step();
	if (!TestEqual(TEXT("tuning fixture model"), Fixture.Models().Num(), 1)) return false;
	const double OldY = Fixture.Models()[0]->GetRootComponent()->GetRelativeLocation().Y;
	IConsoleManager::Get().ProcessUserConsoleInput(TEXT("wot.fp.pose 0 3 0 0 0 0 0"), *GLog, Fixture.World);
	Fixture.Step();
	TestTrue(TEXT("command adds the requested offset"), FMath::IsNearlyEqual(Fixture.Models()[0]->GetRootComponent()->GetRelativeLocation().Y, OldY + 0.45));
	const FName Tuned = Fixture.Selection;
	Fixture.Selection = TEXT("AngrealInvFireball");
	Fixture.Step();
	Fixture.Selection = Tuned;
	Fixture.Step();
	TestTrue(TEXT("class override survives switching away and back"), FMath::IsNearlyEqual(Fixture.Models()[0]->GetRootComponent()->GetRelativeLocation().Y, OldY + 0.45));
	return !HasAnyErrors();
}
#endif
#endif
