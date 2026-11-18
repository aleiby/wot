#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../World/WoTItemPresentation.h"
#include "../WoTExtensionSeams.h"
#include "../World/WoTRuntimeActorVisuals.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTVertexAnimComponent.h"
#include "../Inventory/WoTInventory.h"
#include "../RetailData/WoTDevRetailFamilies.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "WoTFrontEndTestData.h"
#include "WoTTestWorld.h"
#include "Engine/World.h"

namespace
{
constexpr auto ItemPresentationFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

struct FItemPresentationWorld
{
	UWorld* World = WoTTest::CreateSpecWorld();
	~FItemPresentationWorld() { WoTTest::DestroySpecWorld(World); }
};

FWoTRuntimeActorVisualPresentation SyntheticVisual()
{
	FWoTRuntimeActorVisualPresentation Visual;
	Visual.MeshPackage = TEXT("Environment");
	Visual.MeshName = TEXT("RainCyd");
	Visual.DrawScale = 0.75f;
	Visual.ScaleGlow = 0.5f;
	Visual.AmbientGlow = 32;
	Visual.Style = 3;
	Visual.bStyleTranslucent = true;
	Visual.bUnlit = true;
	Visual.Fatness = 144;
	return Visual;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTItemPresentationMissingSpec,
	"WoT.ItemPresentation.RejectsMissingWorldClassAndVisual", ItemPresentationFlags)

bool FWoTItemPresentationMissingSpec::RunTest(const FString&)
{
	FItemPresentationWorld Fixture;
	const auto& Factory = FWoTExtensionSeams::Get().SpawnItemPresentation;
	if (!TestTrue(TEXT("game startup installs the presentation factory"), static_cast<bool>(Factory))) return false;
	TestNull(TEXT("registered factory rejects an unknown class"),
		Factory(Fixture.World, TEXT("MissingPresentationSpecClass"), FTransform::Identity));
	TestNull(TEXT("null world returns no presentation"),
		AWoTItemPresentation::Spawn(nullptr, TEXT("Inventory"), FTransform::Identity));
	TestNull(TEXT("empty selection returns no presentation"),
		AWoTItemPresentation::Spawn(Fixture.World, NAME_None, FTransform::Identity));
	TestNull(TEXT("unknown class returns no presentation"),
		AWoTItemPresentation::Spawn(Fixture.World,
			TEXT("MissingPresentationSpecClass"), FTransform::Identity));
	TestNull(TEXT("known inventory without a mesh returns no presentation"),
		AWoTItemPresentation::Spawn(Fixture.World, TEXT("Inventory"), FTransform::Identity));
	TestNull(TEXT("meshless presentation returns no actor"),
		AWoTItemPresentation::SpawnFromPresentation(Fixture.World,
			FWoTRuntimeActorVisualPresentation(), FTransform::Identity));
	return !HasAnyErrors();
}

#if !WOT_GUARDED_BOOTSTRAP
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTItemPresentationClassSpec,
	"WoT.ItemPresentation.ResolvesInventoryClassVisual", ItemPresentationFlags)

bool FWoTItemPresentationClassSpec::RunTest(const FString&)
{
	FItemPresentationWorld Fixture;
	const FName ItemClass(TEXT("AngrealInvDart"));
	if (!TestNotNull(TEXT("native item class resolves"),
		AWoTInventory::FindDefaultByOriginalName(ItemClass))) return false;
	const FWoTRuntimeClassDefaults* Defaults = WoTDevRetailClassDefaults();
	if (!TestNotNull(TEXT("development class defaults are available"), Defaults)) return false;
	FString ClassIdentity;
	FString Error;
	FWoTRuntimeActorVisualPresentation Expected;
	if (!TestTrue(TEXT("item class identity resolves"),
		Defaults->FindClassIdentityByLeaf(ItemClass.ToString(), ClassIdentity))
		|| !TestTrue(TEXT("world pickup presentation decodes"),
			WoTRuntimeReadClassVisualPresentation(*Defaults, ClassIdentity, Expected, Error)))
		return false;
	AWoTItemPresentation* Actor = AWoTItemPresentation::Spawn(
		Fixture.World, ItemClass, FTransform(FVector(100.0, 200.0, 300.0)));
	if (!TestNotNull(TEXT("class factory creates the selected pickup visual"), Actor)) return false;
	UWoTVertexAnimComponent* Mesh = Actor->FindComponentByClass<UWoTVertexAnimComponent>();
	if (!TestNotNull(TEXT("selected pickup has a mesh component"), Mesh)) return false;
	TestEqual(TEXT("selected pickup mesh is used"), Mesh->GetDisplayMeshName(), Expected.MeshName);
	TestEqual(TEXT("pickup lighting scale is carried"), Actor->ScaleGlow, Expected.ScaleGlow);
	TestEqual(TEXT("pickup ambient is carried"), Actor->AmbientGlow, Expected.AmbientGlow);
	TestEqual(TEXT("pickup style is carried"), Actor->Style, Expected.Style);
	TestEqual(TEXT("pickup unlit state is carried"), Actor->bUnlit, Expected.bUnlit);
	TestEqual(TEXT("pickup translucency is carried"), Actor->bStyleTranslucent, Expected.bStyleTranslucent);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTItemPresentationDrawnBoxSpec,
	"WoT.ItemPresentation.BoxIsWhatTheItemDraws", ItemPresentationFlags)

bool FWoTItemPresentationDrawnBoxSpec::RunTest(const FString&)
{
	FItemPresentationWorld Fixture;
	AWoTItemPresentation* Actor = AWoTItemPresentation::Spawn(
		Fixture.World, FName(TEXT("AngrealInvDart")), FTransform(FVector(100.0, 200.0, 300.0)));
	if (!TestNotNull(TEXT("the dart's visual spawns"), Actor)) return false;
	const FBox Box = Actor->CalculateComponentsBoundingBoxInLocalSpace(true);
	if (!TestTrue(TEXT("the visual reports a box"), Box.IsValid != 0)) return false;
	// The dart's own vertices at its draw scale, about the actor and not the
	// world. Its culling bounds are a wider and shorter volume.
	TestTrue(FString::Printf(TEXT("box extent is the dart's vertices (%s)"),
		*Box.GetExtent().ToString()),
		Box.GetExtent().Equals(FVector(1.14, 19.23, 6.0), 0.01));
	TestTrue(FString::Printf(TEXT("box centre is the dart's vertices (%s)"),
		*Box.GetCenter().ToString()),
		Box.GetCenter().Equals(FVector(0.0, -0.03, 0.0), 0.01));
	TestFalse(TEXT("a collision-only query still finds nothing"),
		Actor->CalculateComponentsBoundingBoxInLocalSpace(false).IsValid != 0);
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTItemPresentationVisualSpec,
	"WoT.ItemPresentation.VisualLightingTransformAndIsolation", ItemPresentationFlags)

bool FWoTItemPresentationVisualSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FString Error;
	if (!TestTrue(TEXT("authenticated synthetic visual cache activates"),
		WoTFrontEndTest_BuildProductionStageFixture(Fixture, false, Error)))
	{
		AddError(Error);
		return false;
	}
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(Fixture.World);
	UWoTMeshLight::RegisterRuntimeMapLightTable(Fixture.World, Table);
	const FTransform Transform(FRotator(12.0, 34.0, 56.0),
		FVector(160.0, -240.0, 96.0), FVector(1.25));
	AWoTItemPresentation* Actor = AWoTItemPresentation::SpawnFromPresentation(
		Fixture.World, SyntheticVisual(), Transform);
	if (!TestNotNull(TEXT("presentation actor spawns"), Actor)) return false;
	TestTrue(TEXT("actor preserves the caller transform"),
		Actor->GetActorTransform().Equals(Transform));
	TestEqual(TEXT("owner scale glow"), Actor->ScaleGlow, 0.5f);
	TestEqual(TEXT("owner ambient byte"), Actor->AmbientGlow, uint8(32));
	TestEqual(TEXT("owner style"), Actor->Style, uint8(3));
	TestTrue(TEXT("owner unlit flag"), Actor->bUnlit);
	TestTrue(TEXT("owner translucency flag"), Actor->bStyleTranslucent);
	TestFalse(TEXT("actor never replicates"), Actor->GetIsReplicated());
	TestFalse(TEXT("movement never replicates"), Actor->IsReplicatingMovement());
	TestFalse(TEXT("actor has no collision"), Actor->GetActorEnableCollision());
	TestFalse(TEXT("actor needs no tick"), Actor->PrimaryActorTick.bCanEverTick);
	UWoTVertexAnimComponent* Mesh = Actor->FindComponentByClass<UWoTVertexAnimComponent>();
	if (!TestNotNull(TEXT("visual component exists"), Mesh)) return false;
	TestTrue(TEXT("shared visual path binds a display mesh"), Mesh->HasDisplayMesh());
#if WOT_GUARDED_BOOTSTRAP
	TestEqual(TEXT("guarded path binds the authenticated logical mesh"),
		Mesh->GetVertexMeshLogicalId(), FString(TEXT("cache/mesh/Environment__RainCyd")));
	TestNull(TEXT("guarded visual needs no imported skeletal asset"), Mesh->GetSkeletalMeshAsset());
#endif
	TestTrue(TEXT("draw scale stays on the mesh"),
		Mesh->GetRelativeScale3D().Equals(FVector(0.75)));
	TestEqual(TEXT("visual fatness"), Mesh->Fatness, uint8(144));
	TestEqual(TEXT("visual style"), Mesh->Style, uint8(3));
	TestFalse(TEXT("rest frame starts no animation"), Mesh->IsAnimating());
	TestFalse(TEXT("mesh does not tick"), Mesh->IsComponentTickEnabled());
	TestTrue(TEXT("mesh is hidden from scene captures"), Mesh->bHiddenInSceneCapture);
	TestFalse(TEXT("mesh casts no shadows"), Mesh->CastShadow);
	TestFalse(TEXT("mesh generates no overlaps"), Mesh->GetGenerateOverlapEvents());
	TestEqual(TEXT("mesh has no collision"), Mesh->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	UWoTMeshLight* Light = Actor->FindComponentByClass<UWoTMeshLight>();
	if (TestNotNull(TEXT("lighting bakes before the factory returns"), Light))
	{
		TestTrue(TEXT("lighting is already baked"), Light->bBaked);
		TestEqual(TEXT("lighting reader finds the owner scale glow"), Light->ScaleGlow, 0.5f);
		TestTrue(TEXT("initial bake uses owner unlit and ambient properties"),
			Light->Ambient.Equals(FLinearColor(0.375f, 0.375f, 0.375f, 1.0f)));
	}
	WoTTest::TickWorld(Fixture.World, 0.1f);
	TestTrue(TEXT("rest presentation does not rotate or drift"),
		Actor->GetActorTransform().Equals(Transform));
	TestFalse(TEXT("rest frame remains stopped"), Mesh->IsAnimating());
	FWoTZoneAmbient Zone;
	Zone.BoundsMin = Transform.GetLocation() - FVector(10.0);
	Zone.BoundsMax = Transform.GetLocation() + FVector(10.0);
	Zone.Color = FLinearColor(0.125f, 0.25f, 0.5f, 1.0f);
	Table->ZoneAmbients.Add(Zone);
	FWoTRuntimeActorVisualPresentation LitVisual = SyntheticVisual();
	LitVisual.bUnlit = false;
	LitVisual.AmbientGlow = 0;
	AWoTItemPresentation* LitActor = AWoTItemPresentation::SpawnFromPresentation(
		Fixture.World, LitVisual, Transform);
	if (TestNotNull(TEXT("lit presentation spawns"), LitActor))
	{
		const UWoTMeshLight* LitLight = LitActor->FindComponentByClass<UWoTMeshLight>();
		if (TestNotNull(TEXT("lit presentation bakes immediately"), LitLight))
			TestTrue(TEXT("first bake selects the zone at the requested transform"),
				LitLight->Ambient.Equals(FLinearColor(0.125f, 0.25f, 0.5f, 1.0f)));
	}
	return !HasAnyErrors();
}

#endif
