// Slice 2d: canonical editor cursor trace, pure click resolver, and removable-resource targeting.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCitadelResources.h"
#include "../WoTPlayerController.h"
#include "../WoTZoneVolume.h"
#include "../Traps/WoTAlarm.h"
#include "../Traps/WoTFireWall.h"
#include "../Traps/WoTPit.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTSpear.h"
#include "../Traps/WoTStaircase.h"
#include "../Traps/WoTWall.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

namespace
{
	FActorSpawnParameters EditorActionSpecAlwaysSpawnParams(AActor* Owner = nullptr)
	{
		FActorSpawnParameters Params;
		Params.Owner = Owner;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	AActor* EditorActionSpecSpawnBox(UWorld* World, const FVector& Center,
		const FVector& Extent, ECollisionChannel ObjectType = ECC_WorldStatic,
		AActor* Owner = nullptr)
	{
		AActor* BoxActor = World->SpawnActor<AActor>(
			Center, FRotator::ZeroRotator,
			EditorActionSpecAlwaysSpawnParams(Owner));
		if (!IsValid(BoxActor))
		{
			return nullptr;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(BoxActor);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ObjectType);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		BoxActor->SetRootComponent(Box);
		Box->RegisterComponent();
		BoxActor->SetActorLocation(Center);
		return BoxActor;
	}

	AWoTPlayerController* EditorActionSpecSpawnViewer(UWorld* World)
	{
		AWoTPlayerController* Controller =
			World->SpawnActor<AWoTPlayerController>(
				EditorActionSpecAlwaysSpawnParams());
		APawn* Pawn = World->SpawnActor<APawn>(
			FVector::ZeroVector, FRotator::ZeroRotator,
			EditorActionSpecAlwaysSpawnParams());
		if (!IsValid(Controller) || !IsValid(Pawn))
		{
			return nullptr;
		}
		Controller->SetAsLocalPlayerController();
		UBoxComponent* PawnBox = NewObject<UBoxComponent>(Pawn);
		PawnBox->SetBoxExtent(FVector(30.0f));
		PawnBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		PawnBox->SetCollisionObjectType(ECC_Pawn);
		PawnBox->SetCollisionResponseToAllChannels(ECR_Block);
		Pawn->SetRootComponent(PawnBox);
		PawnBox->RegisterComponent();
		Pawn->SetActorLocation(FVector::ZeroVector);
		Controller->Possess(Pawn);
		Controller->SetControlRotation(FRotator::ZeroRotator);
		return Controller;
	}

	void EditorActionSpecSetView(AWoTPlayerController* Controller,
		const FVector& ViewOrigin, const FVector& Direction = FVector::ForwardVector)
	{
		if (!IsValid(Controller))
		{
			return;
		}
		if (APawn* Pawn = Controller->GetPawn(); IsValid(Pawn))
		{
			Pawn->SetActorLocation(ViewOrigin);
		}
		Controller->SetControlRotation(Direction.Rotation());
	}

	FWoTCitadelEditorSelection EditorActionSpecSelection(
		UClass* ResourceClass, int32 Count, bool bSpawnTempResource)
	{
		FWoTCitadelEditorSelection Selection;
		Selection.ResourceClass = ResourceClass;
		Selection.Count = Count;
		Selection.bSpawnTempResource = bSpawnTempResource;
		return Selection;
	}

	int32 EditorActionSpecActionId(EWoTCitadelEditorAction Action)
	{
		return static_cast<int32>(Action);
	}

	FWoTCitadelEditorTraceResult EditorActionSpecActorHit(AActor* Actor)
	{
		FWoTCitadelEditorTraceResult Hit;
		Hit.HitActor = Actor;
		Hit.bBlockingHit = IsValid(Actor);
		return Hit;
	}

	UPrimitiveComponent* EditorActionSpecRootPrimitive(AActor* Actor)
	{
		return IsValid(Actor)
			? Cast<UPrimitiveComponent>(Actor->GetRootComponent()) : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorActionResolverSpec,
	"WoT.Citadel.EditorAction.ResolverMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorActionResolverSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTSpear* Spear = World->SpawnActor<AWoTSpear>(
		EditorActionSpecAlwaysSpawnParams());
	AWoTPortcullis* Portcullis = World->SpawnActor<AWoTPortcullis>(
		EditorActionSpecAlwaysSpawnParams());
	AWoTStaircase* Staircase = World->SpawnActor<AWoTStaircase>(
		EditorActionSpecAlwaysSpawnParams());

	FWoTCitadelEditorTraceResult Miss;
	FWoTCitadelEditorTraceResult WorldHit;
	WorldHit.bBlockingHit = true;
	WorldHit.bHitWorldGeometry = true;
	FWoTCitadelEditorTraceResult NonResourceActorHit;
	NonResourceActorHit.bBlockingHit = true;
	const FWoTCitadelEditorSelection SpearAvailable = EditorActionSpecSelection(
		AWoTSpear::StaticClass(), 2, true);
	const FWoTCitadelEditorSelection SpearEmpty = EditorActionSpecSelection(
		AWoTSpear::StaticClass(), 0, true);
	const FWoTCitadelEditorSelection PortcullisAvailable = EditorActionSpecSelection(
		AWoTPortcullis::StaticClass(), 1, false);
	const FWoTCitadelEditorSelection PortcullisEmpty = EditorActionSpecSelection(
		AWoTPortcullis::StaticClass(), 0, false);
	const FWoTCitadelEditorSelection StaircaseAvailable = EditorActionSpecSelection(
		AWoTStaircase::StaticClass(), 1, false);
	const FWoTCitadelEditorSelection NoSelection;

	auto Resolve = [](const FWoTCitadelEditorTraceResult& Hit,
		const FWoTCitadelEditorSelection& Selection)
	{
		return EditorActionSpecActionId(
			WoTCitadelResources::ResolveEditorAction(Hit, Selection));
	};
	TestEqual(TEXT("miss resolves None"), Resolve(Miss, SpearAvailable),
		EditorActionSpecActionId(EWoTCitadelEditorAction::None));
	TestEqual(TEXT("world plus spawn-temp count resolves Create"),
		Resolve(WorldHit, SpearAvailable),
		EditorActionSpecActionId(EWoTCitadelEditorAction::Create));
	TestEqual(TEXT("world with zero count resolves None"), Resolve(WorldHit, SpearEmpty),
		EditorActionSpecActionId(EWoTCitadelEditorAction::None));
	TestEqual(TEXT("world with no selection resolves None"), Resolve(WorldHit, NoSelection),
		EditorActionSpecActionId(EWoTCitadelEditorAction::None));
	TestEqual(TEXT("non-world non-resource hit resolves None"),
		Resolve(NonResourceActorHit, SpearAvailable),
		EditorActionSpecActionId(EWoTCitadelEditorAction::None));
	TestEqual(TEXT("ordinary resource resolves Remove"),
		Resolve(EditorActionSpecActorHit(Spear), NoSelection),
		EditorActionSpecActionId(EWoTCitadelEditorAction::Remove));
	TestEqual(TEXT("matching Portcullis with count resolves Toggle"),
		Resolve(EditorActionSpecActorHit(Portcullis), PortcullisAvailable),
		EditorActionSpecActionId(EWoTCitadelEditorAction::Toggle));
	TestEqual(TEXT("matching Staircase with count resolves Toggle"),
		Resolve(EditorActionSpecActorHit(Staircase), StaircaseAvailable),
		EditorActionSpecActionId(EWoTCitadelEditorAction::Toggle));
	TestEqual(TEXT("matching toggle with zero count falls through to Deactivate"),
		Resolve(EditorActionSpecActorHit(Portcullis), PortcullisEmpty),
		EditorActionSpecActionId(EWoTCitadelEditorAction::Deactivate));
	TestEqual(TEXT("mismatched toggle class resolves Deactivate"),
		Resolve(EditorActionSpecActorHit(Portcullis), StaircaseAvailable),
		EditorActionSpecActionId(EWoTCitadelEditorAction::Deactivate));
	// EditorHUD.LeftMouseDown falls through to Deactivate for ANY non-matching toggle hit,
	// including no selection (EditorHUD.uc:282-292) — not None.
	TestEqual(TEXT("toggle resource with no selection resolves Deactivate"),
		Resolve(EditorActionSpecActorHit(Portcullis), NoSelection),
		EditorActionSpecActionId(EWoTCitadelEditorAction::Deactivate));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorTraceGeometrySpec,
	"WoT.Citadel.EditorAction.TraceGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorTraceGeometrySpec::RunTest(const FString&)
{
	TestEqual(TEXT("canonical trace distance is the shipped 3200 units"),
		AWoTPlayerController::TRACE_DISTANCE, 3200.0f);
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = EditorActionSpecSpawnViewer(World);
	if (!IsValid(Controller))
	{
		AddError(TEXT("trace geometry viewer failed to spawn"));
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AActor* NearBox = EditorActionSpecSpawnBox(
		World, FVector(500, 0, 0), FVector(50));
	EditorActionSpecSpawnBox(World, FVector(900, 0, 0), FVector(50));
	WoTTest::TickWorld(World, 1);

	FWoTCitadelEditorTraceResult Hit;
	TestTrue(TEXT("canonical trace hits world geometry"),
		IsValid(Controller) && Controller->EditorTrace(Hit));
	TestEqual(TEXT("canonical trace returns nearest actor"), Hit.HitActor, NearBox);
	TestTrue(TEXT("world hit is classified as world geometry"), Hit.bHitWorldGeometry);
	// TraceVisible returns the hit backed off 0.5 uu along the view, as in the original engine.
	TestTrue(TEXT("hit location is the impact backed off 0.5 uu along the view"),
		Hit.HitLocation.Equals(FVector(449.5, 0, 0), 0.01f));
	TestTrue(TEXT("impact normal is returned"),
		Hit.HitNormal.Equals(-FVector::ForwardVector, 0.01f));

	EditorActionSpecSetView(Controller, FVector(0, 200, 0));
	AActor* AtCutoff = EditorActionSpecSpawnBox(
		World, FVector(3190, 200, 0), FVector(10));
	WoTTest::TickWorld(World, 1);
	FWoTCitadelEditorTraceResult CutoffHit;
	TestTrue(TEXT("geometry whose near face is inside 3200 is hit"),
		Controller->EditorTrace(CutoffHit));
	TestEqual(TEXT("3200-range hit returns actor"), CutoffHit.HitActor, AtCutoff);

	EditorActionSpecSetView(Controller, FVector(0, 400, 0));
	EditorActionSpecSpawnBox(World, FVector(3220, 400, 0), FVector(10));
	WoTTest::TickWorld(World, 1);
	FWoTCitadelEditorTraceResult BeyondHit;
	TestFalse(TEXT("geometry wholly beyond 3200 is not hit"),
		Controller->EditorTrace(BeyondHit));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Retail's native TraceVisible skips hits inside zones invisible to the viewer
// (WOTZoneInfo.IsZoneVisible: Team==255 || Team==playerTeam) and re-traces 16 uu
// past the hit until the 3200-unit budget is spent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorTraceZoneVisibilitySpec,
	"WoT.Citadel.EditorAction.TraceSkipsWrongTeamZones",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorTraceZoneVisibilitySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = EditorActionSpecSpawnViewer(World);
	if (!IsValid(Controller))
	{
		AddError(TEXT("zone visibility viewer failed to spawn"));
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// The trace keys IsZoneVisible off the edit session's owner team mirror (retail:
	// PlayerReplicationInfo.Team — identical while a session is live).
	Controller->OwnerEditSessionTeam = 1;
	// A team-tagged zone wrapping the NEAR geometry; the far box sits in open space.
	AWoTZoneVolume* Zone = World->SpawnActor<AWoTZoneVolume>(
		FVector(500, 0, 0), FRotator::ZeroRotator,
		EditorActionSpecAlwaysSpawnParams());
	if (IsValid(Zone))
	{
		Zone->Collision->SetBoxExtent(FVector(150.0f));
		Zone->EditTeam = 2;
	}
	AActor* NearBox = EditorActionSpecSpawnBox(World, FVector(500, 0, 0), FVector(50));
	AActor* FarBox = EditorActionSpecSpawnBox(World, FVector(900, 0, 0), FVector(50));
	WoTTest::TickWorld(World, 1);

	FWoTCitadelEditorTraceResult EnemyHit;
	TestTrue(TEXT("trace continues past enemy-zone geometry"),
		Controller->EditorTrace(EnemyHit));
	TestEqual(TEXT("enemy-zone hit is skipped to the visible geometry behind"),
		EnemyHit.HitActor, FarBox);

	Zone->EditTeam = 1;
	FWoTCitadelEditorTraceResult OwnHit;
	TestTrue(TEXT("own-team zone geometry is hit"), Controller->EditorTrace(OwnHit));
	TestEqual(TEXT("own-team zone returns the near geometry"), OwnHit.HitActor, NearBox);

	Zone->EditTeam = 255;
	FWoTCitadelEditorTraceResult NeutralHit;
	TestTrue(TEXT("neutral zone geometry is hit"), Controller->EditorTrace(NeutralHit));
	TestEqual(TEXT("neutral zone returns the near geometry"),
		NeutralHit.HitActor, NearBox);

	// The skip is bounded by the trace distance: an all-wrong-team corridor ends as a
	// clean miss (retail "missed section"), never a hang.
	Zone->EditTeam = 2;
	Zone->Collision->SetBoxExtent(FVector(4000.0f));
	FWoTCitadelEditorTraceResult MissHit;
	TestFalse(TEXT("wholly wrong-team corridor reports a miss"),
		Controller->EditorTrace(MissHit));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorTraceIgnoreAndLogicalBaseSpec,
	"WoT.Citadel.EditorAction.TraceIgnoreAndLogicalBase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorTraceIgnoreAndLogicalBaseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = EditorActionSpecSpawnViewer(World);
	if (!IsValid(Controller))
	{
		AddError(TEXT("trace ignore viewer failed to spawn"));
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AActor* Preview = EditorActionSpecSpawnBox(
		World, FVector(120, 0, 0), FVector(20), ECC_WorldDynamic);
	EditorActionSpecSpawnBox(World, FVector(180, 0, 0), FVector(20),
		ECC_WorldDynamic, Preview);
	AActor* Target = EditorActionSpecSpawnBox(
		World, FVector(500, 0, 0), FVector(30));
	WoTTest::TickWorld(World, 1);

	FWoTCitadelEditorTraceResult IgnoredHit;
	TestTrue(TEXT("trace passes through pawn, preview, and preview child"),
		IsValid(Controller) && Controller->EditorTrace(IgnoredHit, Preview));
	TestEqual(TEXT("ignored blockers expose the target"), IgnoredHit.HitActor, Target);

	EditorActionSpecSetView(Controller, FVector(0, 300, 0));
	AWoTWall* Wall = World->SpawnActor<AWoTWall>(
		FVector(500, 300, 0), FRotator::ZeroRotator,
		EditorActionSpecAlwaysSpawnParams());
	AWoTWallSlab* Slab = World->SpawnActor<AWoTWallSlab>(
		FVector(500, 300, 0), FRotator::ZeroRotator,
		EditorActionSpecAlwaysSpawnParams(Wall));
	WoTTest::TickWorld(World, 1);
	FWoTCitadelEditorTraceResult WallHit;
	TestTrue(TEXT("visibility trace hits Wall child"), Controller->EditorTrace(WallHit));
	TestEqual(TEXT("Wall child resolves to logical Wall manager"),
		WallHit.HitActor, static_cast<AActor*>(Wall));
	TestTrue(TEXT("Wall child surface is not classified as world"),
		!WallHit.bHitWorldGeometry);
	TestTrue(TEXT("Wall slab fixture remains the physical blocker"), IsValid(Slab));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorTraceRemovableResourcesSpec,
	"WoT.Citadel.EditorAction.TraceAllRemovableResources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorTraceRemovableResourcesSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = EditorActionSpecSpawnViewer(World);
	if (!IsValid(Controller))
	{
		AddError(TEXT("resource targeting viewer failed to spawn"));
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTSpear* Spear = World->SpawnActor<AWoTSpear>(
		FVector(500, 0, 0), FRotator::ZeroRotator,
		EditorActionSpecAlwaysSpawnParams());
	AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(
		FVector(500, 200, 0), FRotator::ZeroRotator,
		EditorActionSpecAlwaysSpawnParams());
	AWoTPit* Pit = World->SpawnActor<AWoTPit>(
		FVector(500, 400, 0), FRotator::ZeroRotator,
		EditorActionSpecAlwaysSpawnParams());
	AWoTWall* Wall = World->SpawnActor<AWoTWall>(
		FVector(500, 600, 0), FRotator::ZeroRotator,
		EditorActionSpecAlwaysSpawnParams());
	World->SpawnActor<AWoTWallSlab>(
		FVector(500, 600, 0), FRotator::ZeroRotator,
		EditorActionSpecAlwaysSpawnParams(Wall));
	AWoTFireWall* FireWall = World->SpawnActor<AWoTFireWall>(
		FVector(500, 800, 0), FRotator::ZeroRotator,
		EditorActionSpecAlwaysSpawnParams());
	World->SpawnActor<AWoTFireWallSlab>(
		FVector(500, 800, 0), FRotator::ZeroRotator,
		EditorActionSpecAlwaysSpawnParams(FireWall));
	WoTTest::TickWorld(World, 1);

	struct FTargetCase
	{
		const TCHAR* Label;
		float ViewY;
		AWoTTrap* Expected;
	};
	const TArray<FTargetCase> Cases = {
		{TEXT("Spear"), 0.0f, Spear},
		{TEXT("Alarm"), 200.0f, Alarm},
		{TEXT("Pit"), 400.0f, Pit},
		{TEXT("Wall slab to manager"), 600.0f, Wall},
		{TEXT("FireWall slab to manager"), 800.0f, FireWall},
	};
	for (const FTargetCase& TargetCase : Cases)
	{
		if (!IsValid(TargetCase.Expected))
		{
			AddError(FString::Printf(TEXT("%s fixture failed to spawn"), TargetCase.Label));
			continue;
		}
		TargetCase.Expected->BeginEditingResource(1);
		EditorActionSpecSetView(Controller, FVector(0, TargetCase.ViewY, 0));
		WoTTest::TickWorld(World, 1);
		FWoTCitadelEditorTraceResult Hit;
		const bool bHit = Controller->EditorTrace(Hit);
		TestTrue(FString::Printf(TEXT("real cursor trace hits %s"), TargetCase.Label), bHit);
		TestEqual(FString::Printf(TEXT("real cursor trace selects %s logical base"),
			TargetCase.Label), Hit.HitActor, static_cast<AActor*>(TargetCase.Expected));
		TestEqual(FString::Printf(TEXT("%s click resolves Remove"), TargetCase.Label),
			EditorActionSpecActionId(WoTCitadelResources::ResolveEditorAction(
				Hit, FWoTCitadelEditorSelection())),
			EditorActionSpecActionId(EWoTCitadelEditorAction::Remove));
	}

	UPrimitiveComponent* SpearRoot = EditorActionSpecRootPrimitive(Spear);
	UPrimitiveComponent* AlarmRoot = EditorActionSpecRootPrimitive(Alarm);
	UPrimitiveComponent* PitRoot = EditorActionSpecRootPrimitive(Pit);
	TestTrue(TEXT("Spear stays query-only while targetable"), IsValid(SpearRoot)
		&& SpearRoot->GetCollisionEnabled() == ECollisionEnabled::QueryOnly);
	TestTrue(TEXT("Alarm stays query-only while targetable"), IsValid(AlarmRoot)
		&& AlarmRoot->GetCollisionEnabled() == ECollisionEnabled::QueryOnly);
	TestTrue(TEXT("Pit stays query-only while targetable"), IsValid(PitRoot)
		&& PitRoot->GetCollisionEnabled() == ECollisionEnabled::QueryOnly);
	TestTrue(TEXT("Spear blocks the cursor visibility channel"), IsValid(SpearRoot)
		&& SpearRoot->GetCollisionResponseToChannel(ECC_Visibility) == ECR_Block);
	TestTrue(TEXT("Alarm blocks the cursor visibility channel"), IsValid(AlarmRoot)
		&& AlarmRoot->GetCollisionResponseToChannel(ECC_Visibility) == ECR_Block);
	TestTrue(TEXT("Pit blocks the cursor visibility channel"), IsValid(PitRoot)
		&& PitRoot->GetCollisionResponseToChannel(ECC_Visibility) == ECR_Block);
	TestTrue(TEXT("Spear does not block pawns while targetable"), IsValid(SpearRoot)
		&& SpearRoot->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block);
	TestTrue(TEXT("Alarm does not block pawns while targetable"), IsValid(AlarmRoot)
		&& AlarmRoot->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block);
	TestTrue(TEXT("Pit does not block pawns while targetable"), IsValid(PitRoot)
		&& PitRoot->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block);

	// End editing: the edit-time Visibility targetability must NOT leak into runtime.
	Spear->EndEditingResource();
	Alarm->EndEditingResource();
	Pit->EndEditingResource();
	TestTrue(TEXT("Spear manager collision is off at play"), IsValid(SpearRoot)
		&& SpearRoot->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	TestTrue(TEXT("Pit manager collision is off at play"), IsValid(PitRoot)
		&& PitRoot->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	TestTrue(TEXT("Alarm stops blocking the cursor visibility channel at play"),
		IsValid(AlarmRoot)
		&& AlarmRoot->GetCollisionResponseToChannel(ECC_Visibility) != ECR_Block);
	TestTrue(TEXT("Alarm keeps its query-only pawn touch volume at play"),
		IsValid(AlarmRoot)
		&& AlarmRoot->GetCollisionEnabled() == ECollisionEnabled::QueryOnly
		&& AlarmRoot->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
