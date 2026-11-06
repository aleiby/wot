// Slice 2c: canonical selectable-resource state and owner-local presentation invariants.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCitadelResources.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTPlayerController.h"
#include "../WoTZoneVolume.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTStaircase.h"
#include "../Traps/WoTTrap.h"
#include "../Traps/WoTWall.h"

#include "Components/BoxComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
	FActorSpawnParameters EditorSelectSpecAlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	AWoTZoneVolume* EditorSelectSpecSpawnZone(UWorld* World,
		const FVector& Center, const FVector& Extent, uint8 Team,
		bool bAllowEditing = true)
	{
		AWoTZoneVolume* Zone = World->SpawnActor<AWoTZoneVolume>(
			Center, FRotator::ZeroRotator, EditorSelectSpecAlwaysSpawnParams());
		if (IsValid(Zone))
		{
			Zone->Collision->SetBoxExtent(Extent);
			Zone->EditTeam = Team;
			Zone->bAllowEditing = bAllowEditing;
		}
		return Zone;
	}

	AWoTPlayerController* EditorSelectSpecSpawnLocalController(
		UWorld* World, const FVector& ViewerLocation)
	{
		AWoTPlayerController* Controller =
			World->SpawnActor<AWoTPlayerController>(
				EditorSelectSpecAlwaysSpawnParams());
		if (!IsValid(Controller))
		{
			return nullptr;
		}
		Controller->SetAsLocalPlayerController();
		USceneComponent* ViewerRoot = Controller->GetRootComponent();
		if (!ViewerRoot)
		{
			ViewerRoot = NewObject<USceneComponent>(
				Controller, TEXT("EditorSelectViewerRoot"));
			Controller->SetRootComponent(ViewerRoot);
			ViewerRoot->RegisterComponent();
		}
		ViewerRoot->SetWorldLocation(ViewerLocation);
		return Controller;
	}

	int32 EditorSelectSpecPresentationId(const AWoTTrap* Resource)
	{
		return Resource
			? static_cast<int32>(Resource->GetCitadelEditorPresentation())
			: INDEX_NONE;
	}

	AActor* EditorSelectSpecSpawnWorldBox(
		UWorld* World, const FVector& Center, const FVector& Extent)
	{
		AActor* BoxActor = World->SpawnActor<AActor>(
			Center, FRotator::ZeroRotator, EditorSelectSpecAlwaysSpawnParams());
		if (!IsValid(BoxActor))
		{
			return nullptr;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(BoxActor);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		BoxActor->SetRootComponent(Box);
		Box->RegisterComponent();
		BoxActor->SetActorLocation(Center);
		return BoxActor;
	}

	ECollisionResponse EditorSelectSpecCursorResponse(const AWoTTrap* Resource)
	{
		const UPrimitiveComponent* Cursor = nullptr;
		if (const AWoTPortcullis* Gate = Cast<AWoTPortcullis>(Resource))
		{
			Cursor = Gate->GetCursorTraceCylinder();
		}
		else if (const AWoTStaircase* Stairs = Cast<AWoTStaircase>(Resource))
		{
			Cursor = Stairs->GetCursorTraceCylinder();
		}
		else if (IsValid(Resource))
		{
			Cursor = Cast<UPrimitiveComponent>(Resource->GetRootComponent());
		}
		return Cursor
			? Cursor->GetCollisionResponseToChannel(ECC_Visibility) : ECR_MAX;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorSelectQuerySpec,
	"WoT.Citadel.EditorSelect.QueryFiltering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorSelectQuerySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	EditorSelectSpecSpawnZone(World, FVector::ZeroVector, FVector(300), 1);
	EditorSelectSpecSpawnZone(World, FVector(1000, 0, 0), FVector(200), 2);
	AWoTPortcullis* Matching = World->SpawnActor<AWoTPortcullis>(
		FVector::ZeroVector, FRotator::ZeroRotator,
		EditorSelectSpecAlwaysSpawnParams());
	AWoTStaircase* WrongClass = World->SpawnActor<AWoTStaircase>(
		FVector(50, 0, 0), FRotator::ZeroRotator,
		EditorSelectSpecAlwaysSpawnParams());
	AWoTPortcullis* Foreign = World->SpawnActor<AWoTPortcullis>(
		FVector(1000, 0, 0), FRotator::ZeroRotator,
		EditorSelectSpecAlwaysSpawnParams());

	const FWoTCitadelSelectableResource Accepted =
		WoTCitadelResources::QuerySelectableResource(
			Matching, TEXT("PortcullisInventory"), 1);
	TestTrue(TEXT("matching preplaced resource is selectable"), Accepted.IsSelectable());
	TestEqual(TEXT("inactive state identifier"), static_cast<int32>(Accepted.State),
		static_cast<int32>(EWoTCitadelSelectableState::Inactive));

	Matching->Activate();
	const FWoTCitadelSelectableResource Active =
		WoTCitadelResources::QuerySelectableResource(
			Matching, TEXT("PortcullisInventory"), 1);
	TestEqual(TEXT("active state identifier"), static_cast<int32>(Active.State),
		static_cast<int32>(EWoTCitadelSelectableState::Active));

	const FWoTCitadelSelectableResource Wrong =
		WoTCitadelResources::QuerySelectableResource(
			WrongClass, TEXT("PortcullisInventory"), 1);
	TestEqual(TEXT("wrong class rejected"), static_cast<int32>(Wrong.Rejection),
		static_cast<int32>(EWoTCitadelSelectableRejection::WrongResourceClass));
	const FWoTCitadelSelectableResource ForeignResult =
		WoTCitadelResources::QuerySelectableResource(
			Foreign, TEXT("PortcullisInventory"), 1);
	TestEqual(TEXT("foreign edit zone rejected"),
		static_cast<int32>(ForeignResult.Rejection),
		static_cast<int32>(EWoTCitadelSelectableRejection::ForeignEditZone));
	const FWoTCitadelSelectableResource Unknown =
		WoTCitadelResources::QuerySelectableResource(Matching, TEXT("BogusInventory"), 1);
	TestEqual(TEXT("unknown palette descriptor rejected"),
		static_cast<int32>(Unknown.Rejection),
		static_cast<int32>(EWoTCitadelSelectableRejection::UnknownSelection));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorSelectLogicalBaseSpec,
	"WoT.Citadel.EditorSelect.LogicalBaseAndToggleStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorSelectLogicalBaseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	EditorSelectSpecSpawnZone(World, FVector::ZeroVector, FVector(300), 1);
	AWoTWall* Wall = World->SpawnActor<AWoTWall>(
		FVector::ZeroVector, FRotator::ZeroRotator,
		EditorSelectSpecAlwaysSpawnParams());
	FActorSpawnParameters SlabParams = EditorSelectSpecAlwaysSpawnParams();
	SlabParams.Owner = Wall;
	AWoTWallSlab* Slab = World->SpawnActor<AWoTWallSlab>(
		FVector(20, 0, 0), FRotator::ZeroRotator, SlabParams);

	TestTrue(TEXT("Wall slab resolves to owning Wall"),
		WoTCitadelResources::ResolveLogicalResource(Slab) == Wall);
	const FWoTCitadelSelectableResource WallResult =
		WoTCitadelResources::QuerySelectableResource(
			Slab, TEXT("WallInventory"), 1);
	TestTrue(TEXT("query reports the Wall logical base"),
		WallResult.LogicalResource == Wall);
	TestEqual(TEXT("spawn-temp Wall is not a toggle selection"),
		static_cast<int32>(WallResult.Rejection),
		static_cast<int32>(EWoTCitadelSelectableRejection::CreateResource));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Portcullis.uc:50-77 / Staircase.uc:40-66: the toggle resources' edit-time cursor
// traceability follows the PALETTE — BeginEditingResource makes an inactive gate
// untraceable (clicks pass through to the geometry behind, EditorHUD.uc:266-273),
// BeginSelectResource (its class selected) makes it traceable, EndSelectResource
// re-hides an inactive gate while an ACTIVE one keeps its cursor collision
// (sprite-only hide).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorSelectTraceabilityMatrixSpec,
	"WoT.Citadel.EditorSelect.ToggleTraceabilityMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorSelectTraceabilityMatrixSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	EditorSelectSpecSpawnZone(World, FVector::ZeroVector, FVector(600), 1);
	AWoTPortcullis* Gate = World->SpawnActor<AWoTPortcullis>(
		FVector(200, 0, 0), FRotator::ZeroRotator,
		EditorSelectSpecAlwaysSpawnParams());
	AWoTStaircase* Stairs = World->SpawnActor<AWoTStaircase>(
		FVector(200, 300, 0), FRotator::ZeroRotator,
		EditorSelectSpecAlwaysSpawnParams());
	AActor* Backstop = EditorSelectSpecSpawnWorldBox(
		World, FVector(450, 0, 0), FVector(40));
	AWoTPlayerController* Controller =
		EditorSelectSpecSpawnLocalController(World, FVector(-200, 0, 0));
	if (!IsValid(Gate) || !IsValid(Stairs) || !IsValid(Backstop)
		|| !IsValid(Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// Aim the pawnless viewer down +X through the gate at the backstop wall
	// (AController hides the actor-transform setters; drive the root directly —
	// headless GetPlayerViewPoint reads the view target's actor transform).
	Controller->GetRootComponent()->SetWorldRotation(
		FVector::ForwardVector.Rotation());
	Controller->OwnerEditSessionTeam = 1;
	Controller->OnRep_OwnerEditSessionTeam();
	Gate->BeginEditingResource(1);
	Stairs->BeginEditingResource(1);

	// Editing, nothing selected: both inactive triggers are untraceable and the
	// cursor passes through to the world geometry behind.
	TestEqual(TEXT("editing unselected inactive gate is untraceable"),
		EditorSelectSpecCursorResponse(Gate), ECR_Ignore);
	TestEqual(TEXT("editing unselected inactive staircase is untraceable"),
		EditorSelectSpecCursorResponse(Stairs), ECR_Ignore);
	FWoTCitadelEditorTraceResult PassThroughHit;
	TestTrue(TEXT("cursor trace reaches behind the unselected gate"),
		Controller->EditorTrace(PassThroughHit));
	TestEqual(TEXT("unselected inactive gate lets the click pass to the wall"),
		PassThroughHit.HitActor, Backstop);

	// BeginSelectResource: the palette lands on the gate's class.
	Controller->SetCitadelPaletteSelection(TEXT("PortcullisInventory"));
	TestEqual(TEXT("palette-selected gate is traceable"),
		EditorSelectSpecCursorResponse(Gate), ECR_Block);
	TestEqual(TEXT("other toggle class stays untraceable"),
		EditorSelectSpecCursorResponse(Stairs), ECR_Ignore);
	FWoTCitadelEditorTraceResult SelectedHit;
	TestTrue(TEXT("cursor trace hits the selected gate"),
		Controller->EditorTrace(SelectedHit));
	TestEqual(TEXT("selected gate is the cursor hit"),
		SelectedHit.HitActor, static_cast<AActor*>(Gate));

	// EndSelectResource: palette moves on — the inactive gate hides again, the
	// selected staircase shows.
	Controller->SetCitadelPaletteSelection(TEXT("StaircaseInventory"));
	TestEqual(TEXT("deselected inactive gate is untraceable again"),
		EditorSelectSpecCursorResponse(Gate), ECR_Ignore);
	TestEqual(TEXT("palette-selected staircase is traceable"),
		EditorSelectSpecCursorResponse(Stairs), ECR_Block);

	// An ACTIVE gate keeps its cursor collision even deselected (the Deactivate
	// fall-through must be able to hit it).
	Gate->Activate();
	TestEqual(TEXT("active gate stays traceable while deselected"),
		EditorSelectSpecCursorResponse(Gate), ECR_Block);
	FWoTCitadelEditorTraceResult ActiveHit;
	TestTrue(TEXT("cursor trace hits the deselected ACTIVE gate"),
		Controller->EditorTrace(ActiveHit));
	TestEqual(TEXT("active gate is the cursor hit"),
		ActiveHit.HitActor, static_cast<AActor*>(Gate));
	Gate->Deactivate();
	TestEqual(TEXT("deactivated deselected gate goes untraceable"),
		EditorSelectSpecCursorResponse(Gate), ECR_Ignore);

	// Leaving the editable zone EndSelects (EditorHUD's IsInvalidEditZone sync).
	Controller->SetCitadelPaletteSelection(TEXT("PortcullisInventory"));
	TestEqual(TEXT("re-selected gate is traceable"),
		EditorSelectSpecCursorResponse(Gate), ECR_Block);
	Controller->GetRootComponent()->SetWorldLocation(FVector(2000, 0, 0));
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("leaving the edit zone ends the selection"),
		EditorSelectSpecCursorResponse(Gate), ECR_Ignore);
	Controller->GetRootComponent()->SetWorldLocation(FVector(-200, 0, 0));
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("returning to the edit zone re-selects"),
		EditorSelectSpecCursorResponse(Gate), ECR_Block);

	// Session exit clears the selection stamp; EndEditingResource restores play
	// state (the invisible archway never blocks aim traces at play).
	Controller->OwnerEditSessionTeam = 255;
	Controller->OnRep_OwnerEditSessionTeam();
	TestEqual(TEXT("edit exit deselects the gate"),
		EditorSelectSpecCursorResponse(Gate), ECR_Ignore);
	Gate->Activate();
	TestEqual(TEXT("active gate remains traceable while still editing"),
		EditorSelectSpecCursorResponse(Gate), ECR_Block);
	Gate->EndEditingResource();
	Stairs->EndEditingResource();
	TestEqual(TEXT("gate ignores the cursor at play even while active"),
		EditorSelectSpecCursorResponse(Gate), ECR_Ignore);
	TestEqual(TEXT("staircase ignores the cursor at play"),
		EditorSelectSpecCursorResponse(Stairs), ECR_Ignore);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorSelectLocalPresentationSpec,
	"WoT.Citadel.EditorSelect.LocalPresentationNoSimulation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorSelectLocalPresentationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	EditorSelectSpecSpawnZone(World, FVector::ZeroVector, FVector(300), 1);
	AWoTPortcullis* Resource = World->SpawnActor<AWoTPortcullis>(
		FVector::ZeroVector, FRotator::ZeroRotator,
		EditorSelectSpecAlwaysSpawnParams());
	AWoTPlayerController* Controller =
		EditorSelectSpecSpawnLocalController(World, FVector::ZeroVector);
	if (!IsValid(Resource) || !IsValid(Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FWoTCitadelBudgetCount& BudgetRow =
		Controller->OwnerEditBudget.AddDefaulted_GetRef();
	BudgetRow.AdapterName = TEXT("PortcullisInventory");
	BudgetRow.ItemCount = 2;
	Controller->OwnerEditSessionTeam = 1;
	Controller->OnRep_OwnerEditSessionTeam();

	const int32 BudgetBefore = Controller->GetOwnerCitadelBudgetCount(
		TEXT("PortcullisInventory"));
	const bool bActiveBefore = Resource->IsActive();
	const bool bCollisionBefore = Resource->GetActorEnableCollision();
	const bool bEditingBefore = Resource->bEditing;
	Controller->SetCitadelPaletteSelection(TEXT("PortcullisInventory"));
	TestEqual(TEXT("begin-select reveals inactive marker"),
		EditorSelectSpecPresentationId(Resource),
		static_cast<int32>(EWoTCitadelResourcePresentation::Inactive));
	Controller->SetHoveredCitadelResource(Resource);
	TestTrue(TEXT("hover tracks logical resource"),
		Controller->GetHoveredCitadelResource() == Resource);
	TestEqual(TEXT("hover swaps to inactive highlight identifier"),
		EditorSelectSpecPresentationId(Resource),
		static_cast<int32>(EWoTCitadelResourcePresentation::InactiveHighlight));
	Controller->SetHoveredCitadelResource(nullptr);
	TestEqual(TEXT("unhover restores inactive identifier"),
		EditorSelectSpecPresentationId(Resource),
		static_cast<int32>(EWoTCitadelResourcePresentation::Inactive));

	USceneComponent* ViewerRoot = Controller->GetRootComponent();
	ViewerRoot->SetWorldLocation(FVector(1000, 0, 0));
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("invalid viewer zone clears reveal"),
		EditorSelectSpecPresentationId(Resource),
		static_cast<int32>(EWoTCitadelResourcePresentation::Hidden));
	ViewerRoot->SetWorldLocation(FVector::ZeroVector);
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("returning to valid zone refreshes selected reveal"),
		EditorSelectSpecPresentationId(Resource),
		static_cast<int32>(EWoTCitadelResourcePresentation::Inactive));

	Controller->SetCitadelPaletteSelection(TEXT("StaircaseInventory"));
	TestEqual(TEXT("palette change ends old reveal"),
		EditorSelectSpecPresentationId(Resource),
		static_cast<int32>(EWoTCitadelResourcePresentation::Hidden));
	Controller->SetCitadelPaletteSelection(TEXT("PortcullisInventory"));
	Controller->OwnerEditSessionTeam = 255;
	Controller->OnRep_OwnerEditSessionTeam();
	TestEqual(TEXT("edit exit clears reveal"),
		EditorSelectSpecPresentationId(Resource),
		static_cast<int32>(EWoTCitadelResourcePresentation::Hidden));

	TestEqual(TEXT("local presentation never changes budget"),
		Controller->GetOwnerCitadelBudgetCount(TEXT("PortcullisInventory")), BudgetBefore);
	TestEqual(TEXT("local presentation never changes active state"),
		Resource->IsActive(), bActiveBefore);
	TestEqual(TEXT("local presentation never changes collision"),
		Resource->GetActorEnableCollision(), bCollisionBefore);
	TestEqual(TEXT("local presentation never changes edit simulation state"),
		Resource->bEditing, bEditingBefore);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
