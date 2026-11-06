// Headless specs for the M2 runtime mesh-light table selection and material bind.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTMover.h"
#include "../WoTBeamEffect.h"
#include "../WoTVertexAnimTestActor.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTExpWardProjectile.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UnrealType.h"

namespace
{
FActorSpawnParameters MeshLightSpawnParams()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

FWoTLightRec MakeLight(const FVector& Pos, float Radius, const FLinearColor& Color,
	uint8 Brightness = 64, bool bSpecialLit = false, bool bEmits = true,
	int32 WaveGroup = INDEX_NONE)
{
	FWoTLightRec Light;
	Light.Pos = Pos;
	Light.InvRadius = 1.0f / Radius;
	Light.Radius = Radius;
	Light.Color = Color;
	Light.WaveGroup = WaveGroup;
	Light.Brightness = Brightness;
	Light.bSpecialLit = bSpecialLit;
	Light.bEmits = bEmits;
	return Light;
}

void BindAllLightsToLeafZero(UWoTMapLightTable* Table)
{
	// A non-CSG splitter leaves RootOutside unchanged, so every line is clear
	// and both terminal sides classify as zone zero / leaf zero.
	Table->ZoneBspPlanes = {FVector4(1.0, 0.0, 0.0, 0.0)};
	Table->ZoneBspFront = {-1};
	Table->ZoneBspBack = {-1};
	Table->LeafBspLeaves = {0, 0};
	Table->LeafBspCsg = {0};
	Table->bBspRootOutside = true;
	Table->LeafLightOffsets = {0, Table->Lights.Num()};
	Table->LeafLightIndices.Reset(Table->Lights.Num());
	for (int32 LightIndex = 0; LightIndex < Table->Lights.Num(); ++LightIndex)
	{
		Table->LeafLightIndices.Add(LightIndex);
	}
}

FWoTZoneAmbient MakeMeshLightZoneAmbient(int32 ZoneNumber,
	const FLinearColor& Color, bool bDefault = false,
	const FVector& BoundsMin = FVector::ZeroVector,
	const FVector& BoundsMax = FVector::ZeroVector)
{
	FWoTZoneAmbient AmbientRec;
	AmbientRec.Zone = ZoneNumber;
	AmbientRec.Color = Color;
	AmbientRec.bDefault = bDefault;
	AmbientRec.BoundsMin = BoundsMin;
	AmbientRec.BoundsMax = BoundsMax;
	return AmbientRec;
}

bool ParentChainContains(const UMaterialInterface* Material,
	const UMaterialInterface* ExpectedParent)
{
	const UMaterialInterface* Current = Material;
	while (IsValid(Current))
	{
		if (Current == ExpectedParent)
		{
			return true;
		}
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current);
		Current = Instance ? Instance->Parent : nullptr;
	}
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightZoneBspSpec,
	"WoT.MeshLight.ZoneBsp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightZoneBspSpec::RunTest(const FString&)
{
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>();
	const FLinearColor DefaultColor(0.01f, 0.02f, 0.03f, 1.0f);
	// Zone 1 is authored black and must still beat the lit LevelInfo default.
	const FLinearColor BackColor(0.0f, 0.0f, 0.0f, 1.0f);
	const FLinearColor FrontColor(0.0f, 0.0f, 0.2f, 1.0f);
	Table->ZoneAmbients = {
		MakeMeshLightZoneAmbient(0, DefaultColor, true),
		MakeMeshLightZoneAmbient(1, BackColor),
		MakeMeshLightZoneAmbient(2, FrontColor)
	};
	// X=0 split: coplanar and positive points are front. Leaf marker = -(zone+1).
	Table->ZoneBspPlanes = {FVector4(1.0, 0.0, 0.0, 0.0)};
	Table->ZoneBspFront = {-3};
	Table->ZoneBspBack = {-2};

	TestEqual(TEXT("negative side resolves the back zone"),
		UWoTMeshLight::ZoneAmbientAt(Table, FVector(-1.0, 0.0, 0.0)), BackColor);
	TestEqual(TEXT("positive side resolves the front zone"),
		UWoTMeshLight::ZoneAmbientAt(Table, FVector(1.0, 0.0, 0.0)), FrontColor);
	TestEqual(TEXT("coplanar points resolve to the front side"),
		UWoTMeshLight::ZoneAmbientAt(Table, FVector::ZeroVector), FrontColor);

	Table->ZoneBspFront[0] = -1;
	TestEqual(TEXT("zone zero resolves the LevelInfo default"),
		UWoTMeshLight::ZoneAmbientAt(Table, FVector(1.0, 0.0, 0.0)), DefaultColor);
	Table->ZoneBspFront[0] = -8;
	TestEqual(TEXT("an absent classified zone resolves the LevelInfo default"),
		UWoTMeshLight::ZoneAmbientAt(Table, FVector(1.0, 0.0, 0.0)), DefaultColor);

	Table->ZoneBspPlanes.Reset();
	Table->ZoneBspFront.Reset();
	Table->ZoneBspBack.Reset();
	Table->ZoneAmbients = {
		MakeMeshLightZoneAmbient(0, DefaultColor, true),
		MakeMeshLightZoneAmbient(1, BackColor, false,
			FVector(-10.0), FVector(10.0)),
		MakeMeshLightZoneAmbient(2, FrontColor, false,
			FVector(-2.0), FVector(2.0))
	};
	TestEqual(TEXT("tree-less tables retain smallest-AABB fallback"),
		UWoTMeshLight::ZoneAmbientAt(Table, FVector::ZeroVector), FrontColor);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightLeafBspLineCheckSpec,
	"WoT.MeshLight.LeafBspLineCheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightLeafBspLineCheckSpec::RunTest(const FString&)
{
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>();
	Table->ZoneBspPlanes = {FVector4(1.0, 0.0, 0.0, 0.0)};
	Table->ZoneBspFront = {-3};
	Table->ZoneBspBack = {-2};
	Table->LeafBspLeaves = {11, 22};
	Table->LeafBspCsg = {1};
	Table->bBspRootOutside = false;

	TestEqual(TEXT("negative side resolves the back compiled leaf"),
		UWoTMeshLight::LeafAt(Table, FVector(-1.0, 0.0, 0.0)), 11);
	TestEqual(TEXT("positive side resolves the front compiled leaf"),
		UWoTMeshLight::LeafAt(Table, FVector(1.0, 0.0, 0.0)), 22);
	TestEqual(TEXT("coplanar point resolves to the front compiled leaf"),
		UWoTMeshLight::LeafAt(Table, FVector::ZeroVector), 22);
	TestTrue(TEXT("the outside/front portion of a CSG node is clear"),
		UWoTMeshLight::BspLineCheck(Table,
			FVector(2.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0)));
	TestFalse(TEXT("crossing into the solid/back portion blocks"),
		UWoTMeshLight::BspLineCheck(Table,
			FVector(1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0)));

	Table->LeafBspCsg[0] = 0;
	Table->bBspRootOutside = true;
	TestTrue(TEXT("a non-CSG sheet does not block the solid-hull check"),
		UWoTMeshLight::BspLineCheck(Table,
			FVector(1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0)));

	Table->LeafBspCsg[0] = 1;
	Table->bBspRootOutside = false;
	TestTrue(TEXT("the retail +/-0.001 overlap takes the front side"),
		UWoTMeshLight::BspLineCheck(Table,
			FVector(-0.0005, 0.0, 0.0), FVector(0.0005, 0.0, 0.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightFirstHitNodeSpec,
	"WoT.MeshLight.FirstHitNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightFirstHitNodeSpec::RunTest(const FString&)
{
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>();
	// Root X=0 is a non-CSG partition. Its front child is a CSG X=2
	// boundary: front is outside and back is solid. A +X-to-X=1 trace must
	// report child node 1, matching UModel::LineCheck's Hit.Item.
	Table->ZoneBspPlanes = {
		FVector4(1.0, 0.0, 0.0, 0.0),
		FVector4(1.0, 0.0, 0.0, 2.0)
	};
	Table->ZoneBspFront = {1, -1};
	Table->ZoneBspBack = {-1, -1};
	Table->LeafBspCsg = {0, 1};
	Table->bBspRootOutside = true;
	Table->FrictionNodeIndices = {1};
	Table->FrictionNodeValues = {10.0f};

	TestEqual(TEXT("start-side-first traversal returns the first blocking node"),
		UWoTMeshLight::FirstHitNode(Table,
			FVector(3.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0)), 1);
	TestEqual(TEXT("the hit node resolves authored climb friction"),
		UWoTMeshLight::FrictionForNode(Table, 1), 10.0f);
	TestEqual(TEXT("an unlisted node resolves Texture's default friction"),
		UWoTMeshLight::FrictionForNode(Table, 0), 1.0f);
	TestEqual(TEXT("a clear trace has no hit node"),
		UWoTMeshLight::FirstHitNode(Table,
			FVector(3.0, 0.0, 0.0), FVector(4.0, 0.0, 0.0)), INDEX_NONE);

	Table->FrictionNodeValues.Reset();
	TestEqual(TEXT("a malformed sparse channel safely uses the default"),
		UWoTMeshLight::FrictionForNode(Table, 1), 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightProjectileGlowDefaultsSpec,
	"WoT.MeshLight.ProjectileGlowDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightProjectileGlowDefaultsSpec::RunTest(const FString&)
{
	const AWoTEarthTremorProjectile* EarthTremor =
		GetDefault<AWoTEarthTremorProjectile>();
	TestEqual(TEXT("Earth Tremor AmbientGlow matches its uc"),
		EarthTremor->AmbientGlow, 211.0f);
	TestTrue(TEXT("Earth Tremor bUnlit matches its uc"), EarthTremor->bUnlit);

	const AWoTFireballProjectile* Fireball = GetDefault<AWoTFireballProjectile>();
	TestEqual(TEXT("Fireball ScaleGlow matches its uc"), Fireball->ScaleGlow, 5.0f);
	TestEqual(TEXT("Fireball AmbientGlow matches its uc"), Fireball->AmbientGlow, 60.0f);

	const AWoTBeamEffect* Beam = GetDefault<AWoTBeamEffect>();
	TestEqual(TEXT("LightningStreak02 beam AmbientGlow matches its uc"),
		Beam->AmbientGlow, 250.0f);
	TestTrue(TEXT("LightningStreak02 beam bUnlit matches its uc"), Beam->bUnlit);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightSelectionSpec,
	"WoT.MeshLight.Selection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightSelectionSpec::RunTest(const FString&)
{
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>();
	Table->Lights = {
		MakeLight(FVector(0, 100, 0), 1000.0f, FLinearColor::Red, 40),
		MakeLight(FVector(0, 200, 0), 1000.0f, FLinearColor::Green, 90),
		MakeLight(FVector(100, 0, 0), 1000.0f, FLinearColor::Blue, 100),
		MakeLight(FVector(2000, 0, 0), 1000.0f, FLinearColor::White, 255),
		MakeLight(FVector(0, -50, 0), 1000.0f, FLinearColor::Yellow, 20),
		MakeLight(FVector(0, 10, 0), 1000.0f, FLinearColor::White, 255, true),
		MakeLight(FVector(0, -50, 0), 1000.0f, FLinearColor::White, 2)
	};
	BindAllLightsToLeafZero(Table);
	TArray<int32> Selected = UWoTMeshLight::SelectLeafLights(Table,
		FVector::ZeroVector, 0, 6, false,
		[](int32 LightIndex) { return LightIndex != 2; });
	TestEqual(TEXT("radius, special-lit, LineCheck, and threshold filters apply"),
		Selected.Num(), 3);
	if (Selected.Num() == 3)
	{
		TestEqual(TEXT("highest visible raw-brightness priority first"), Selected[0], 1);
		TestEqual(TEXT("second visible priority"), Selected[1], 0);
		TestEqual(TEXT("third visible priority"), Selected[2], 4);
	}

	Table->Lights.Reset();
	for (int32 LightIndex = 0; LightIndex < 8; ++LightIndex)
	{
		const float Angle = 2.0f * PI * static_cast<float>(LightIndex) / 8.0f;
		Table->Lights.Add(MakeLight(
			FVector(FMath::Cos(Angle) * 100.0f, FMath::Sin(Angle) * 100.0f, 50.0f),
			1000.0f, FLinearColor::White, 64));
	}
	BindAllLightsToLeafZero(Table);
	Selected = UWoTMeshLight::SelectLeafLights(Table, FVector::ZeroVector,
		0, 6, false, [](int32) { return true; });
	TestEqual(TEXT("selection is capped at the zone maximum"), Selected.Num(), 6);
	for (int32 Index = 0; Index < Selected.Num(); ++Index)
	{
		TestEqual(TEXT("stable equal-priority order follows the shipped run"),
			Selected[Index], Index);
	}

	Table->Lights = {
		MakeLight(FVector(10, 0, 0), 100.0f, FLinearColor::White, 128),
		MakeLight(FVector(10, 0, 0), 100.0f, FLinearColor::White, 100,
			false, false),
		MakeLight(FVector(10, 0, 0), 100.0f, FLinearColor::White, 16)
	};
	BindAllLightsToLeafZero(Table);
	Selected = UWoTMeshLight::SelectLeafLights(Table, FVector::ZeroVector,
		0, 2, false, [](int32) { return true; });
	TestEqual(TEXT("LT_None-visible candidate consumes the cap without emitting"),
		Selected.Num(), 1);
	if (Selected.Num() == 1)
	{
		TestEqual(TEXT("only the bright emitting candidate reaches materials"),
			Selected[0], 0);
	}

	const FWoTLightRec ArenaWitness = MakeLight(
		FVector(-4915.652344, -11787.421875, -7664.330566),
		625.0f, FLinearColor::White, 96);
	TestEqual(TEXT("Arena_09 WotPathNode30 Light114 SqrtApprox tag"),
		UWoTMeshLight::LightingTagAt(ArenaWitness,
			FVector(-4969.651855, -11269.277344, -7863.899414)), 10584);
	const FWoTLightRec BattleWitness = MakeLight(
		FVector(-16602.650391, -7637.359375, 21302.890625),
		1225.0f, FLinearColor::White, 32);
	TestEqual(TEXT("Battle2_02 PathNode261 Light87 crosses threshold 1762"),
		UWoTMeshLight::LightingTagAt(BattleWitness,
			FVector(-17688.714844, -7321.080078, 21048.101563)), 1763);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverSurfaceLightSelectionSpec,
	"WoT.MeshLight.MoverSurfaceSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMoverSurfaceLightSelectionSpec::RunTest(const FString&)
{
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>();
	TestEqual(TEXT("unstamped light rows are steady"),
		FWoTLightRec().WaveGroup, INDEX_NONE);
	TestEqual(TEXT("unstamped tables use identity waveform means"),
		Table->WaveMeans, FVector4(1.0, 1.0, 1.0, 1.0));
	TestEqual(TEXT("unstamped tables use zero waveform amplitudes"),
		Table->WaveAmplitudes, FVector4(0.0, 0.0, 0.0, 0.0));
	// Radius=1 and raw Brightness=1 make the first row both far and dim. Retail
	// SetupMoverSurface still selects it because neither property is a gate.
	Table->Lights = {
		MakeLight(FVector(5000, 0, 0), 1.0f, FLinearColor::Red, 1),
		MakeLight(FVector(-100, 0, 0), 1.0f, FLinearColor::Green, 1),
		MakeLight(FVector(200, 0, 0), 1.0f, FLinearColor::Blue, 1, true),
		MakeLight(FVector(300, 0, 0), 1.0f, FLinearColor::White, 1,
			false, false),
		// bEmits deliberately stays true: retail AddLight independently rejects
		// LightBrightness==0 after the facing/special-lit consider gates.
		MakeLight(FVector(400, 0, 0), 1.0f, FLinearColor::Yellow, 0),
	};
	BindAllLightsToLeafZero(Table);

	TArray<int32> Front = UWoTMeshLight::SelectMoverSurfaceLights(
		Table, 0, FVector::ZeroVector, FVector::ForwardVector, false);
	TArray<int32> Back = UWoTMeshLight::SelectMoverSurfaceLights(
		Table, 0, FVector::ZeroVector, -FVector::ForwardVector, false);
	TArray<int32> Special = UWoTMeshLight::SelectMoverSurfaceLights(
		Table, 0, FVector::ZeroVector, FVector::ForwardVector, true);
	TestEqual(TEXT("front face selects the far dim in-front light"), Front.Num(), 1);
	if (Front.Num() == 1)
	{
		TestEqual(TEXT("front face keeps permeating order and ignores radius"),
			Front[0], 0);
	}
	TestEqual(TEXT("opposite face selects a different light"), Back.Num(), 1);
	if (Back.Num() == 1)
	{
		TestEqual(TEXT("behind/front divergence follows oriented plane"), Back[0], 1);
	}
	TestEqual(TEXT("bSpecialLit equality admits only the special light"),
		Special.Num(), 1);
	if (Special.Num() == 1)
	{
		TestEqual(TEXT("special-lit row"), Special[0], 2);
	}

	Table->Lights.Reset();
	for (int32 LightIndex = 0; LightIndex < 23; ++LightIndex)
	{
		Table->Lights.Add(MakeLight(
			FVector(10000.0f + LightIndex, 0, 0), 1.0f,
			FLinearColor::White, 1));
	}
	BindAllLightsToLeafZero(Table);
	const TArray<int32> Uncapped = UWoTMeshLight::SelectMoverSurfaceLights(
		Table, 0, FVector::ZeroVector, FVector::ForwardVector, false);
	TestEqual(TEXT("retail selection itself has no material/count cap"),
		Uncapped.Num(), 23);
	TestEqual(TEXT("presentation contract is measured 17 plus three guard slots"),
		UWoTMeshLight::MaxMoverSurfaceLights, 20);

	const AWoTMover* MoverDefault = GetDefault<AWoTMover>();
	TestFalse(TEXT("Actor.uc bSpecialLit default is imported on movers"),
		MoverDefault->bSpecialLit);
	TestNotNull(TEXT("AWoTMover exposes bSpecialLit to import/runtime reflection"),
		FindFProperty<FBoolProperty>(AWoTMover::StaticClass(), TEXT("bSpecialLit")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverSurfaceRuntimeStampSpec,
	"WoT.MeshLight.MoverSurfaceRuntimeStamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMoverSurfaceRuntimeStampSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* Mover = World->SpawnActor<AWoTMover>(
		AWoTMover::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator,
		MeshLightSpawnParams());
	if (!TestNotNull(TEXT("mover spawned"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	Table->Lights = {
		MakeLight(FVector(100, 0, 0), 1.0f, FLinearColor::Red, 1,
			false, true, 0),
		MakeLight(FVector(-100, 0, 0), 1.0f, FLinearColor::Green, 1),
	};
	Table->WaveMeans = FVector4(0.6, 1.0, 1.0, 1.0);
	Table->WaveAmplitudes = FVector4(0.39, 0.0, 0.0, 0.0);
	BindAllLightsToLeafZero(Table);
	FWoTMoverSurfaceLight& Front = Mover->MeshLight->MoverSurfaces.AddDefaulted_GetRef();
	Front.MaterialSlot = TEXT("Front");
	Front.LocalNormal = FVector::ForwardVector;
	FWoTMoverSurfaceLight& Back = Mover->MeshLight->MoverSurfaces.AddDefaulted_GetRef();
	Back.MaterialSlot = TEXT("Back");
	Back.LocalNormal = -FVector::ForwardVector;
	Mover->MeshLight->StartRuntimeBake(Table);
	TestEqual(TEXT("front runtime row gets its own light"),
		Mover->MeshLight->MoverSurfaces[0].LightPosR.Num(), 1);
	TestEqual(TEXT("back runtime row gets its own light"),
		Mover->MeshLight->MoverSurfaces[1].LightPosR.Num(), 1);
	if (TestEqual(TEXT("front runtime row retains one waveform group"),
		Mover->MeshLight->MoverSurfaces[0].LightWaveGroup.Num(), 1))
	{
		TestEqual(TEXT("front runtime row selects Wave0"),
			Mover->MeshLight->MoverSurfaces[0].LightWaveGroup[0], 0);
	}
	if (TestEqual(TEXT("back runtime row retains one steady selector"),
		Mover->MeshLight->MoverSurfaces[1].LightWaveGroup.Num(), 1))
	{
		TestEqual(TEXT("back runtime row retains the steady sentinel"),
			Mover->MeshLight->MoverSurfaces[1].LightWaveGroup[0], INDEX_NONE);
	}
	TestEqual(TEXT("runtime component copies map waveform means"),
		Mover->MeshLight->WaveMeans, Table->WaveMeans);
	TestEqual(TEXT("runtime component copies map waveform amplitudes"),
		Mover->MeshLight->WaveAmplitudes, Table->WaveAmplitudes);
	if (Mover->MeshLight->MoverSurfaces[0].LightPosR.Num() == 1
		&& Mover->MeshLight->MoverSurfaces[1].LightPosR.Num() == 1)
	{
		TestEqual(TEXT("front row light"),
			FVector(Mover->MeshLight->MoverSurfaces[0].LightPosR[0]),
			FVector(100, 0, 0));
		TestEqual(TEXT("back row light"),
			FVector(Mover->MeshLight->MoverSurfaces[1].LightPosR[0]),
			FVector(-100, 0, 0));
	}

	// Consume the post-spawn timer bake, then prove a pure keyframe-style
	// rotation reselects even when origin leaf/location do not change.
	WoTTest::TickWorld(World, 20);
	Mover->SetActorRotation(FRotator(0.0f, 180.0f, 0.0f));
	WoTTest::TickWorld(World, 20);
	if (TestEqual(TEXT("rotation cadence keeps one light per plane"),
		Mover->MeshLight->MoverSurfaces[0].LightPosR.Num(), 1))
	{
		TestEqual(TEXT("rotated front plane now selects the negative-X light"),
			FVector(Mover->MeshLight->MoverSurfaces[0].LightPosR[0]),
			FVector(-100, 0, 0));
		if (TestEqual(TEXT("surface reselection retains one selector"),
			Mover->MeshLight->MoverSurfaces[0].LightWaveGroup.Num(), 1))
		{
			TestEqual(TEXT("surface reselection replaces the animated selector"),
				Mover->MeshLight->MoverSurfaces[0].LightWaveGroup[0], INDEX_NONE);
		}
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverSurfaceSubThresholdTranslationSpec,
	"WoT.MeshLight.MoverSurfaceSubThresholdTranslation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMoverSurfaceSubThresholdTranslationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FVector BaseLocation(-1130.0, -2624.0, -16848.0);
	AWoTMover* Mover = World->SpawnActor<AWoTMover>(
		AWoTMover::StaticClass(), BaseLocation, FRotator::ZeroRotator,
		MeshLightSpawnParams());
	if (!TestNotNull(TEXT("Battle2_05 Mover11 witness spawned"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	// Battle2_05 Mover11 moves -81 Z without leaving leaf 1611. Light162
	// crosses from Model579's -Z plane to its +Z plane over that keyframe.
	Table->Lights = {
		MakeLight(FVector(-1129.900146, -2623.902100, -16914.201172),
			425.0f, FLinearColor::White, 44),
	};
	BindAllLightsToLeafZero(Table);
	FWoTMoverSurfaceLight& Down =
		Mover->MeshLight->MoverSurfaces.AddDefaulted_GetRef();
	Down.MaterialSlot = TEXT("WhiteCloakT__DirtGrnd3692drk__MP0006");
	Down.LocalOrigin = FVector(326.0, 464.0, -59.999996);
	Down.LocalNormal = -FVector::UpVector;
	FWoTMoverSurfaceLight& Up =
		Mover->MeshLight->MoverSurfaces.AddDefaulted_GetRef();
	Up.MaterialSlot = TEXT("WhiteCloakT__DirtGrnd3692drk__MP0007");
	Up.LocalOrigin = FVector(326.0, 464.0, -27.999996);
	Up.LocalNormal = FVector::UpVector;
	Mover->MeshLight->StartRuntimeBake(Table);
	TestEqual(TEXT("base pose selects Light162 on the -Z plane"),
		Mover->MeshLight->MoverSurfaces[0].LightPosR.Num(), 1);
	TestEqual(TEXT("base pose excludes Light162 from the +Z plane"),
		Mover->MeshLight->MoverSurfaces[1].LightPosR.Num(), 0);

	// Consume the unconditional post-spawn bake, then move less than the
	// actor-wide M2 128-UU threshold while staying in the same synthetic leaf.
	WoTTest::TickWorld(World, 20);
	Mover->SetActorLocation(BaseLocation + FVector(0.0, 0.0, -81.0));
	WoTTest::TickWorld(World, 20);
	TestEqual(TEXT("81-UU move removes Light162 from the -Z plane"),
		Mover->MeshLight->MoverSurfaces[0].LightPosR.Num(), 0);
	TestEqual(TEXT("81-UU move transfers Light162 to the +Z plane"),
		Mover->MeshLight->MoverSurfaces[1].LightPosR.Num(), 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightBattle203FixtureSpec,
	"WoT.MeshLight.Battle203Fixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightBattle203FixtureSpec::RunTest(const FString&)
{
	struct FFixtureLight
	{
		const TCHAR* Name;
		FVector Pos;
		float Radius;
		uint8 Brightness;
		bool bLineClear;
		int32 ExpectedTag;
	};
	// Pinned from the shipped Battle2_03 model's leaf permeating runs. The
	// clear/block values come from the exact zero-extent solid-hull port; in
	// particular dominant Light136 legitimately crosses a solid terminal slab.
	const TArray<FFixtureLight> StatueC = {
		{TEXT("Light229"), FVector(-7419.380371, 5103.744629, -576.906311), 1625.0f, 32, false, 5867},
		{TEXT("Light220"), FVector(-8796.862305, 4615.893066, -672.898315), 825.0f, 10, false, 6077},
		{TEXT("Light219"), FVector(-7940.300781, 4593.405762, -672.898315), 825.0f, 10, false, 59},
		{TEXT("Light204"), FVector(-7590.617676, 5094.079102, -526.427185), 1625.0f, 24, false, 7006},
		{TEXT("Light137"), FVector(-8336.474609, 5184.077148, -53.632111), 1625.0f, 40, true, 24568}
	};
	const TArray<FFixtureLight> StatueB = {
		{TEXT("Light229"), FVector(-7419.380371, 5103.744629, -576.906311), 1625.0f, 32, false, 16630},
		{TEXT("Light220"), FVector(-8796.862305, 4615.893066, -672.898315), 825.0f, 10, true, INDEX_NONE},
		{TEXT("Light219"), FVector(-7940.300781, 4593.405762, -672.898315), 825.0f, 10, false, 4322},
		{TEXT("Light205"), FVector(-9195.616211, 5096.770508, -526.427185), 1625.0f, 24, false, 5770},
		{TEXT("Light204"), FVector(-7590.617676, 5094.079102, -526.427185), 1625.0f, 24, false, 14807},
		{TEXT("Light137"), FVector(-8336.474609, 5184.077148, -53.632111), 1625.0f, 40, true, 26787},
		{TEXT("Light136"), FVector(-8330.525391, 4803.815430, 1572.703125), 6400.0f, 120, false, 87800}
	};

	auto CheckFixture = [this](const TCHAR* Label, const FVector& ActorPos,
		int32 LeafIndex, const TArray<FFixtureLight>& Fixture)
	{
		UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>();
		Table->Lights.Reserve(Fixture.Num());
		Table->LeafLightOffsets.SetNumZeroed(LeafIndex + 2);
		for (int32 Index = 0; Index < Fixture.Num(); ++Index)
		{
			const FFixtureLight& Row = Fixture[Index];
			Table->Lights.Add(MakeLight(
				Row.Pos, Row.Radius, FLinearColor::White, Row.Brightness));
			Table->LeafLightIndices.Add(Index);
			if (Row.ExpectedTag != INDEX_NONE)
			{
				TestEqual(FString::Printf(TEXT("%s %s LightingTag"), Label, Row.Name),
					UWoTMeshLight::LightingTagAt(Table->Lights[Index], ActorPos),
					Row.ExpectedTag);
			}
		}
		Table->LeafLightOffsets[LeafIndex + 1] = Fixture.Num();
		const TArray<int32> Selected = UWoTMeshLight::SelectLeafLights(
			Table, ActorPos, LeafIndex, 6, false,
			[&Fixture](int32 LightIndex)
			{
				return Fixture[LightIndex].bLineClear;
			});
		TestEqual(FString::Printf(TEXT("%s steady-state count"), Label),
			Selected.Num(), 1);
		if (Selected.Num() == 1)
		{
			TestEqual(FString::Printf(TEXT("%s selected light"), Label),
				FString(Fixture[Selected[0]].Name), FString(TEXT("Light137")));
		}
	};

	CheckFixture(TEXT("statueC leaf 638"),
		FVector(-8691.858398, 4751.094238, -384.356262), 638, StatueC);
	CheckFixture(TEXT("statueB leaf 594"),
		FVector(-8041.774414, 4738.958496, -230.332489), 594, StatueB);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightMovementRebakeSpec,
	"WoT.MeshLight.MovementRebake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightMovementRebakeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTVertexAnimTestActor* Actor = World->SpawnActor<AWoTVertexAnimTestActor>(
		FVector::ZeroVector, FRotator::ZeroRotator, MeshLightSpawnParams());
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	Table->Lights = {
		MakeLight(FVector(0, 0, 100), 200.0f, FLinearColor::White,
			64, false, true, 2),
		MakeLight(FVector(400, 0, 100), 200.0f, FLinearColor::White)
	};
	BindAllLightsToLeafZero(Table);
	UWoTMeshLight* MeshLight = UWoTMeshLight::AttachRuntime(Actor, Table);
	if (!TestNotNull(TEXT("runtime mesh-light component"), MeshLight))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	if (TestEqual(TEXT("initial location selects one light"), MeshLight->LightPosR.Num(), 1))
	{
		TestEqual(TEXT("initial location selects the first light"),
			FVector(MeshLight->LightPosR[0]), FVector(0, 0, 100));
		if (TestEqual(TEXT("ordinary runtime array retains one waveform group"),
			MeshLight->LightWaveGroup.Num(), 1))
		{
			TestEqual(TEXT("ordinary runtime array selects Wave2"),
				MeshLight->LightWaveGroup[0], 2);
		}
	}

	Actor->SetActorLocation(FVector(400, 0, 0));
	WoTTest::TickWorld(World, 20);
	if (TestEqual(TEXT("moved location selects one light"), MeshLight->LightPosR.Num(), 1))
	{
		TestEqual(TEXT("moving more than 128uu rebakes on the 0.25s timer"),
			FVector(MeshLight->LightPosR[0]), FVector(400, 0, 100));
	}

	Table->Lights = {
		MakeLight(FVector(464, 0, 25), 50.0f, FLinearColor::White)
	};
	BindAllLightsToLeafZero(Table);
	Actor->SetActorLocation(FVector(464, 0, 0), false, nullptr,
		ETeleportType::TeleportPhysics);
	if (TestEqual(TEXT("teleported location selects one light"), MeshLight->LightPosR.Num(), 1))
	{
		TestEqual(TEXT("a sub-threshold teleport rebakes immediately"),
			FVector(MeshLight->LightPosR[0]), FVector(464, 0, 25));
	}

	// A 16uu move crosses a compiled leaf boundary. Distance alone is below
	// the 128uu cadence threshold, but the shipped permeating run changes.
	Table->Lights = {
		MakeLight(FVector(464, 0, 5), 8.0f, FLinearColor::Red),
		MakeLight(FVector(480, 0, 5), 8.0f, FLinearColor::Green)
	};
	Table->ZoneBspPlanes = {FVector4(1.0, 0.0, 0.0, 472.0)};
	Table->ZoneBspFront = {-1};
	Table->ZoneBspBack = {-1};
	Table->LeafBspLeaves = {0, 1};
	Table->LeafBspCsg = {0};
	Table->bBspRootOutside = true;
	Table->LeafLightOffsets = {0, 1, 2};
	Table->LeafLightIndices = {0, 1};
	MeshLight->ForceRuntimeBake();
	if (TestEqual(TEXT("back leaf selects its own run"), MeshLight->LightPosR.Num(), 1))
	{
		TestEqual(TEXT("back-leaf light"), FVector(MeshLight->LightPosR[0]),
			FVector(464, 0, 5));
	}
	Actor->SetActorLocation(FVector(480, 0, 0));
	WoTTest::TickWorld(World, 20);
	if (TestEqual(TEXT("leaf crossing selects one light"), MeshLight->LightPosR.Num(), 1))
	{
		TestEqual(TEXT("a sub-128uu leaf crossing rebakes on the timer"),
			FVector(MeshLight->LightPosR[0]), FVector(480, 0, 5));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightReparentSpec,
	"WoT.MeshLight.Reparent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightReparentSpec::RunTest(const FString&)
{
	UMaterialInterface* PlainMaster = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/WoT/Materials/M_WoT_Master.M_WoT_Master"));
	UMaterialInterface* MeshLitMaster = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/WoT/Materials/M_WoT_MeshLit.M_WoT_MeshLit"));
	USkeletalMesh* TestMesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/WoT/Meshes/Trolloc/Trolloc/SkeletalMeshes/Trolloc.Trolloc"));
	UTexture* Texture = LoadObject<UTexture>(
		nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	if (!TestNotNull(TEXT("plain master"), PlainMaster)
		|| !TestNotNull(TEXT("MeshLit master"), MeshLitMaster)
		|| !TestNotNull(TEXT("test skeletal mesh"), TestMesh)
		|| !TestNotNull(TEXT("test texture"), Texture))
	{
		return false;
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTVertexAnimTestActor* Actor = World->SpawnActor<AWoTVertexAnimTestActor>(
		FVector::ZeroVector, FRotator::ZeroRotator, MeshLightSpawnParams());
	Actor->Mesh->SetSkeletalMeshAsset(TestMesh);
	UMaterialInstanceDynamic* PlainMid = UMaterialInstanceDynamic::Create(PlainMaster, Actor);
	PlainMid->SetTextureParameterValue(TEXT("Tex"), Texture);
	const FLinearColor ExpectedKeyColor(42.0f / 255.0f, 42.0f / 255.0f,
		42.0f / 255.0f, 1.0f);
	PlainMid->SetVectorParameterValue(TEXT("KeyColor"), ExpectedKeyColor);
	Actor->Mesh->SetMaterial(0, PlainMid);
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	UWoTMeshLight::AttachRuntime(Actor, Table);

	UMaterialInterface* Result = Actor->Mesh->GetMaterial(0);
	TestTrue(TEXT("runtime material parent chain reaches M_WoT_MeshLit"),
		ParentChainContains(Result, MeshLitMaster));
	UTexture* ResultTexture = nullptr;
	const bool bHasTexture = Result && Result->GetTextureParameterValue(
		FHashedMaterialParameterInfo(TEXT("Tex")), ResultTexture);
	TestTrue(TEXT("Tex parameter remains present"), bHasTexture);
	TestEqual(TEXT("Tex parameter value is carried across"), ResultTexture, Texture);
	FLinearColor ResultKeyColor = FLinearColor::Black;
	const bool bHasKeyColor = Result && Result->GetVectorParameterValue(
		FHashedMaterialParameterInfo(TEXT("KeyColor")), ResultKeyColor);
	TestTrue(TEXT("KeyColor parameter remains present"), bHasKeyColor);
	TestTrue(TEXT("KeyColor is carried across runtime MID reparenting"),
		ResultKeyColor.Equals(ExpectedKeyColor, KINDA_SMALL_NUMBER));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightProjectileStyleRoutingSpec,
	"WoT.MeshLight.ProjectileStyleRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightProjectileStyleRoutingSpec::RunTest(const FString&)
{
	UMaterialInterface* TranslucentMaster = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/WoT/Materials/M_WoT_MeshTranslucent.M_WoT_MeshTranslucent"));
	UMaterialInterface* MeshLitMaster = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/WoT/Materials/M_WoT_MeshLit.M_WoT_MeshLit"));
	// The dart mesh (xboxdart) is ALL MTT_NormalTwoSided tris, so post the
	// per-type slot split its opaque route is the TWO-SIDED lit master.
	UMaterialInterface* MeshLit2SMaster = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/WoT/Materials/M_WoT_MeshLit2S.M_WoT_MeshLit2S"));
	if (!TestNotNull(TEXT("MeshTranslucent master"), TranslucentMaster)
		|| !TestNotNull(TEXT("MeshLit master"), MeshLitMaster)
		|| !TestNotNull(TEXT("MeshLit2S master"), MeshLit2SMaster))
	{
		return false;
	}

	const AWoTDartProjectile* DartDefault = GetDefault<AWoTDartProjectile>();
	const AWoTExpWardProjectile* WardDefault = GetDefault<AWoTExpWardProjectile>();
	// AngrealDartProjectile.uc:121-127 and AngrealExpWardProjectile.uc:281-285.
	TestTrue(TEXT("Dart CDO stamps STY_Translucent"),
		DartDefault->bStyleTranslucent);
	TestTrue(TEXT("ExpWard CDO stamps STY_Translucent"),
		WardDefault->bStyleTranslucent);
	TestTrue(TEXT("Dart CDO stamps bUnlit"), DartDefault->bUnlit);
	TestTrue(TEXT("ExpWard CDO stamps bUnlit"), WardDefault->bUnlit);
	TestEqual(TEXT("Dart CDO ScaleGlow is 4"), DartDefault->ScaleGlow, 4.0f);
	TestEqual(TEXT("Dart CDO AmbientGlow is 211"), DartDefault->AmbientGlow, 211.0f);
	TestEqual(TEXT("ExpWard CDO ScaleGlow is 1"), WardDefault->ScaleGlow, 1.0f);
	TestEqual(TEXT("ExpWard CDO AmbientGlow keeps Actor default zero"),
		WardDefault->AmbientGlow, 0.0f);

	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(0.0f, 0.0f, 100.0f),
		FRotator::ZeroRotator, MeshLightSpawnParams());
	AWoTExpWardProjectile* Ward = World->SpawnActor<AWoTExpWardProjectile>(
		AWoTExpWardProjectile::StaticClass(), FVector(500.0f, 0.0f, 100.0f),
		FRotator::ZeroRotator, MeshLightSpawnParams());
	if (!TestNotNull(TEXT("Dart spawned"), Dart)
		|| !TestNotNull(TEXT("ExpWard spawned"), Ward))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	UWoTMeshLight* DartLight = UWoTMeshLight::AttachRuntime(Dart, Table);
	UWoTMeshLight* WardLight = UWoTMeshLight::AttachRuntime(Ward, Table);
	if (!TestNotNull(TEXT("Dart runtime mesh-light"), DartLight)
		|| !TestNotNull(TEXT("ExpWard runtime mesh-light"), WardLight))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	auto RootMaterial = [](const UMaterialInterface* Material)
	{
		const UMaterialInterface* Current = Material;
		while (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current))
		{
			if (!IsValid(Instance->Parent))
			{
				break;
			}
			Current = Instance->Parent;
		}
		return Current;
	};
	auto CheckTranslucentSlots = [this, TranslucentMaster, MeshLitMaster,
		&RootMaterial](UWoTVertexAnimComponent* Mesh, const TCHAR* Label,
		const FLinearColor& ExpectedAmbient)
	{
		TestTrue(FString::Printf(TEXT("%s has at least one material slot"), Label),
			Mesh && Mesh->GetNumMaterials() > 0);
		if (!Mesh)
		{
			return;
		}
		for (int32 SlotIndex = 0; SlotIndex < Mesh->GetNumMaterials(); ++SlotIndex)
		{
			UMaterialInterface* Material = Mesh->GetMaterial(SlotIndex);
			TestEqual(FString::Printf(TEXT("%s slot %d roots at MeshTranslucent"),
				Label, SlotIndex), RootMaterial(Material),
				static_cast<const UMaterialInterface*>(TranslucentMaster));
			TestFalse(FString::Printf(TEXT("%s slot %d does not route through MeshLit"),
				Label, SlotIndex), ParentChainContains(Material, MeshLitMaster));
			FLinearColor AmbientValue = FLinearColor::Black;
			const bool bHasAmbient = Material && Material->GetVectorParameterValue(
				FHashedMaterialParameterInfo(TEXT("MLAmbient")), AmbientValue);
			TestTrue(FString::Printf(TEXT("%s slot %d carries MLAmbient"),
				Label, SlotIndex), bHasAmbient);
			TestTrue(FString::Printf(TEXT("%s slot %d MLAmbient matches GUnlitColor"),
				Label, SlotIndex), AmbientValue.Equals(ExpectedAmbient, KINDA_SMALL_NUMBER));
		}
	};

	CheckTranslucentSlots(Dart->DisplayMesh, TEXT("Dart"),
		FLinearColor(1.0f, 1.0f, 1.0f, 1.0f));
	CheckTranslucentSlots(Ward->DisplayMesh, TEXT("ExpWard"),
		FLinearColor(0.5f, 0.5f, 0.5f, 1.0f));
	TestTrue(TEXT("Dart DrawScale=3 reaches its bound visual"),
		Dart->DisplayMesh->GetRelativeScale3D().Equals(FVector(3.0f),
			KINDA_SMALL_NUMBER));

	const FTransform OpaqueTransform(
		FRotator::ZeroRotator, FVector(1000.0f, 0.0f, 100.0f));
	AWoTDartProjectile* OpaqueProjectile =
		World->SpawnActorDeferred<AWoTDartProjectile>(
			AWoTDartProjectile::StaticClass(), OpaqueTransform);
	OpaqueProjectile->bStyleTranslucent = false;
	OpaqueProjectile->FinishSpawning(OpaqueTransform);
	UWoTMeshLight* OpaqueLight =
		UWoTMeshLight::AttachRuntime(OpaqueProjectile, Table);
	TestNotNull(TEXT("non-translucent projectile runtime mesh-light"), OpaqueLight);
	for (int32 SlotIndex = 0;
		SlotIndex < OpaqueProjectile->DisplayMesh->GetNumMaterials(); ++SlotIndex)
	{
		UMaterialInterface* Material =
			OpaqueProjectile->DisplayMesh->GetMaterial(SlotIndex);
		// xboxdart's tris are MTT_NormalTwoSided: the typed slot routes to
		// the two-sided lit master (retail drew the dart two-sided).
		TestTrue(FString::Printf(
			TEXT("non-translucent projectile slot %d routes to MeshLit2S"), SlotIndex),
			ParentChainContains(Material, MeshLit2SMaster));
		TestFalse(FString::Printf(
			TEXT("non-translucent projectile slot %d avoids MeshTranslucent"), SlotIndex),
			ParentChainContains(Material, TranslucentMaster));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightTypedMastersSpec,
	"WoT.MeshLight.TypedMasters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightTypedMastersSpec::RunTest(const FString&)
{
	struct FExpectedMaster
	{
		const TCHAR* Name;
		EBlendMode BlendMode;
	};
	const FExpectedMaster Expected[] = {
		{TEXT("M_WoT_MeshLit2S"), BLEND_Opaque},
		{TEXT("M_WoT_MeshLitMasked"), BLEND_Masked},
		{TEXT("M_WoT_MeshLitT"), BLEND_AlphaComposite},
		{TEXT("M_WoT_MeshModulate"), BLEND_Modulate},
	};
	for (const FExpectedMaster& Item : Expected)
	{
		const FString Path = FString::Printf(
			TEXT("/Game/WoT/Materials/%s.%s"), Item.Name, Item.Name);
		UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, *Path);
		if (!TestNotNull(FString::Printf(TEXT("%s loads"), Item.Name), Master))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s keeps its blend mode"), Item.Name),
			Master->GetBlendMode(), Item.BlendMode);
		TestTrue(FString::Printf(TEXT("%s is two-sided like its James type"),
			Item.Name), Master->IsTwoSided());
	}
	for (const TCHAR* OpaqueName : {TEXT("M_WoT_MeshLit"), TEXT("M_WoT_MeshLit2S")})
	{
		const FString Path = FString::Printf(
			TEXT("/Game/WoT/Materials/%s.%s"), OpaqueName, OpaqueName);
		UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, *Path);
		if (!TestNotNull(FString::Printf(TEXT("%s loads"), OpaqueName), Master))
		{
			continue;
		}
		FLinearColor DefaultKeyColor = FLinearColor::White;
		const bool bHasKeyColor = Master->GetVectorParameterValue(
			FHashedMaterialParameterInfo(TEXT("KeyColor")), DefaultKeyColor);
		TestTrue(FString::Printf(TEXT("%s exposes KeyColor"), OpaqueName),
			bHasKeyColor);
		TestTrue(FString::Printf(TEXT("%s defaults KeyColor to black"), OpaqueName),
			DefaultKeyColor.Equals(FLinearColor::Black, KINDA_SMALL_NUMBER));
	}

	auto CheckFixtureSlot = [this](const TCHAR* MeshName, const TCHAR* SlotName,
		const TCHAR* TextureName, const FLinearColor& KeyColor)
	{
		const FString MeshPath = FString::Printf(
			TEXT("/Game/WoT/Meshes/%s/%s/SkeletalMeshes/%s.%s"),
			MeshName, MeshName, MeshName, MeshName);
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
		if (!TestNotNull(FString::Printf(TEXT("%s fixture mesh loads"), MeshName), Mesh))
		{
			return;
		}
		const FSkeletalMaterial* Found = Mesh->GetMaterials().FindByPredicate(
			[SlotName](const FSkeletalMaterial& Material)
			{
				return Material.MaterialSlotName == FName(SlotName);
			});
		if (!TestNotNull(FString::Printf(TEXT("%s fixture slot %s"), MeshName, SlotName),
			Found) || !Found)
		{
			return;
		}
		UMaterialInterface* Material = Found->MaterialInterface;
		if (!TestNotNull(FString::Printf(TEXT("%s slot material"), MeshName), Material))
		{
			return;
		}
		UTexture* Texture = nullptr;
		const bool bHasTexture = Material->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), Texture);
		TestTrue(FString::Printf(TEXT("%s slot exposes Tex"), MeshName), bHasTexture);
		TestEqual(FString::Printf(TEXT("%s slot binds %s"), MeshName, TextureName),
			Texture ? Texture->GetName() : FString(), FString(TextureName));
		FLinearColor ActualKeyColor = FLinearColor::Black;
		const bool bHasKeyColor = Material->GetVectorParameterValue(
			FHashedMaterialParameterInfo(TEXT("KeyColor")), ActualKeyColor);
		TestTrue(FString::Printf(TEXT("%s slot exposes KeyColor"), MeshName), bHasKeyColor);
		TestTrue(FString::Printf(TEXT("%s slot stamps palette zero"), MeshName),
			ActualKeyColor.Equals(KeyColor, KINDA_SMALL_NUMBER));
	};
	CheckFixtureSlot(TEXT("BoxyLamp"), TEXT("JBoxyLamp1_M"),
		TEXT("JBoxyLamp1_key0"), FLinearColor(1.0f, 0.0f, 1.0f, 1.0f));
	CheckFixtureSlot(TEXT("Minion"), TEXT("JMinion2_M"),
		TEXT("JMinion2_key0"), FLinearColor::Black);
	CheckFixtureSlot(TEXT("HangingLight"), TEXT("JHangingLight1"),
		TEXT("JHangingLight1"), FLinearColor(1.0f, 0.0f, 1.0f, 1.0f));

	USkeletalMesh* TestMesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/WoT/Meshes/Trolloc/Trolloc/SkeletalMeshes/Trolloc.Trolloc"));
	UTexture* Texture = LoadObject<UTexture>(
		nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	if (!TestNotNull(TEXT("typed-routing skeletal mesh"), TestMesh)
		|| !TestNotNull(TEXT("typed-routing texture"), Texture))
	{
		return false;
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	const TCHAR* PreservedRoots[] = {
		TEXT("M_WoT_MeshLit2S"),
		TEXT("M_WoT_MeshLitMasked"),
		TEXT("M_WoT_MeshLitT"),
	};
	for (const TCHAR* RootName : PreservedRoots)
	{
		const FString Path = FString::Printf(
			TEXT("/Game/WoT/Materials/%s.%s"), RootName, RootName);
		UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, *Path);
		if (!Master)
		{
			continue; // The load assertion above already reports the missing asset.
		}
		AWoTVertexAnimTestActor* Actor = World->SpawnActor<AWoTVertexAnimTestActor>(
			FVector::ZeroVector, FRotator::ZeroRotator, MeshLightSpawnParams());
		Actor->Mesh->SetSkeletalMeshAsset(TestMesh);
		UMaterialInstanceDynamic* Mid =
			UMaterialInstanceDynamic::Create(Master, Actor);
		Mid->SetTextureParameterValue(TEXT("Tex"), Texture);
		Actor->Mesh->SetMaterial(0, Mid);
		TestNotNull(FString::Printf(TEXT("%s attaches mesh lighting"), RootName),
			UWoTMeshLight::AttachRuntime(Actor, Table));
		TestTrue(FString::Printf(TEXT("%s survives runtime routing"), RootName),
			ParentChainContains(Actor->Mesh->GetMaterial(0), Master));
	}

	// Actor Style is ExtraFlags in the retail draw call and therefore wins over
	// an imported James slot type. Pin that override using a real translucent
	// projectile whose source default sets bStyleTranslucent.
	UMaterialInterface* TwoSidedMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshLit2S.M_WoT_MeshLit2S"));
	UMaterialInterface* TranslucentMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshTranslucent.M_WoT_MeshTranslucent"));
	if (TwoSidedMaster && TestNotNull(TEXT("Style target master"), TranslucentMaster))
	{
		AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
			AWoTDartProjectile::StaticClass(), FVector::ZeroVector,
			FRotator::ZeroRotator, MeshLightSpawnParams());
		Dart->DisplayMesh->SetSkeletalMeshAsset(TestMesh);
		UMaterialInstanceDynamic* Mid =
			UMaterialInstanceDynamic::Create(TwoSidedMaster, Dart);
		Mid->SetTextureParameterValue(TEXT("Tex"), Texture);
		Dart->DisplayMesh->SetMaterial(0, Mid);
		TestNotNull(TEXT("Style override attaches mesh lighting"),
			UWoTMeshLight::AttachRuntime(Dart, Table));
		TestTrue(TEXT("Style overrides typed 2S with MeshTranslucent"),
			ParentChainContains(Dart->DisplayMesh->GetMaterial(0),
				TranslucentMaster));
	}

	// An actor's bUnlit makes every poly unlit whatever its Style
	// in the original renderer. LightningStreak02 is STY_Normal and bUnlit; its
	// LSbolt slots are translucent by poly type, so the typed lit-translucent
	// root must move to the GUnlitColor master.
	UMaterialInterface* LitTranslucentMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshLitT.M_WoT_MeshLitT"));
	if (TestNotNull(TEXT("lit translucent master"), LitTranslucentMaster)
		&& TranslucentMaster)
	{
		AWoTBeamEffect* Beam = World->SpawnActor<AWoTBeamEffect>(
			AWoTBeamEffect::StaticClass(), FVector::ZeroVector,
			FRotator::ZeroRotator, MeshLightSpawnParams());
		USkeletalMeshComponent* Segment = NewObject<USkeletalMeshComponent>(Beam);
		Segment->SetupAttachment(Beam->GetRootComponent());
		Segment->SetSkeletalMeshAsset(TestMesh);
		UMaterialInstanceDynamic* Mid =
			UMaterialInstanceDynamic::Create(LitTranslucentMaster, Beam);
		Mid->SetTextureParameterValue(TEXT("Tex"), Texture);
		Segment->SetMaterial(0, Mid);
		Segment->RegisterComponent();
		TestNotNull(TEXT("bUnlit beam attaches mesh lighting"),
			UWoTMeshLight::AttachRuntime(Beam, Table));
		TestTrue(TEXT("bUnlit STY_Normal actor moves MeshLitT to MeshTranslucent"),
			ParentChainContains(Segment->GetMaterial(0), TranslucentMaster));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightModulatePreservedSpec,
	"WoT.MeshLight.ModulatePreserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightModulatePreservedSpec::RunTest(const FString&)
{
	UMaterialInterface* ModulateMaster = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/WoT/Materials/M_WoT_MeshModulate.M_WoT_MeshModulate"));
	USkeletalMesh* TestMesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/WoT/Meshes/Trolloc/Trolloc/SkeletalMeshes/Trolloc.Trolloc"));
	UTexture* Texture = LoadObject<UTexture>(
		nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	if (!TestNotNull(TEXT("committed modulate master loads"), ModulateMaster)
		|| !TestNotNull(TEXT("test skeletal mesh"), TestMesh)
		|| !TestNotNull(TEXT("test texture"), Texture))
	{
		return false;
	}
	// Retail modulated skips the fog pass; UE5 defaults translucency vertex fog
	// ON, which would fade the 2x multiplier toward identity in fog. Pin the
	// COMMITTED asset's flag, not just the generator script.
	if (const UMaterial* MasterBase = ModulateMaster->GetMaterial())
	{
		TestFalse(TEXT("modulate master has translucency vertex fog disabled"),
			MasterBase->bUseTranslucencyVertexFog);
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTVertexAnimTestActor* Actor = World->SpawnActor<AWoTVertexAnimTestActor>(
		FVector::ZeroVector, FRotator::ZeroRotator, MeshLightSpawnParams());
	Actor->Mesh->SetSkeletalMeshAsset(TestMesh);
	UMaterialInstanceDynamic* ModulateMid =
		UMaterialInstanceDynamic::Create(ModulateMaster, Actor);
	ModulateMid->SetTextureParameterValue(TEXT("Tex"), Texture);
	Actor->Mesh->SetMaterial(0, ModulateMid);
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	UWoTMeshLight* MeshLight = UWoTMeshLight::AttachRuntime(Actor, Table);
	TestNotNull(TEXT("runtime mesh-light component"), MeshLight);

	UMaterialInterface* Result = Actor->Mesh->GetMaterial(0);
	TestEqual(TEXT("modulated slot is not replaced by the MeshLit family"),
		Result, static_cast<UMaterialInterface*>(ModulateMid));
	TestTrue(TEXT("modulated MID keeps the same root parent after reparent push"),
		ModulateMid->Parent == ModulateMaster);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshLightInertSpec,
	"WoT.MeshLight.Inert",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMeshLightInertSpec::RunTest(const FString&)
{
	UMaterialInterface* PlainMaster = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/WoT/Materials/M_WoT_Master.M_WoT_Master"));
	USkeletalMesh* TestMesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/WoT/Meshes/Trolloc/Trolloc/SkeletalMeshes/Trolloc.Trolloc"));
	if (!TestNotNull(TEXT("plain master"), PlainMaster)
		|| !TestNotNull(TEXT("test skeletal mesh"), TestMesh))
	{
		return false;
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTMeshLight::ResetMapLightTableCacheForTesting();
	AWoTVertexAnimTestActor* Actor = World->SpawnActor<AWoTVertexAnimTestActor>(
		FVector::ZeroVector, FRotator::ZeroRotator, MeshLightSpawnParams());
	Actor->Mesh->SetSkeletalMeshAsset(TestMesh);
	Actor->Mesh->SetMaterial(0, PlainMaster);
	UMaterialInterface* Before = Actor->Mesh->GetMaterial(0);
	UWoTMeshLight* MeshLight = UWoTMeshLight::AttachRuntime(Actor);

	TestNull(TEXT("no LT_<Map> asset creates no runtime component"), MeshLight);
	TestEqual(TEXT("missing table leaves the material untouched"),
		Actor->Mesh->GetMaterial(0), Before);

	UWoTMeshLight* Existing = NewObject<UWoTMeshLight>(Actor, TEXT("UnbakedMeshLight"));
	Existing->LightPosR.Add(FVector4(1, 2, 3, 4));
	Existing->Ambient = FLinearColor(0.1f, 0.2f, 0.3f);
	Actor->AddInstanceComponent(Existing);
	Existing->RegisterComponent();
	MeshLight = UWoTMeshLight::AttachRuntime(Actor);
	TestEqual(TEXT("a missing LT_<Map> asset is loaded only once per world"),
		UWoTMeshLight::MapLightTableLoadAttemptsForTesting(), 1);
	TestEqual(TEXT("an existing unbaked component is reused without mutation"),
		MeshLight, Existing);
	TestFalse(TEXT("missing table does not mark the component baked"), Existing->bBaked);
	if (TestEqual(TEXT("missing table preserves the component light count"),
		Existing->LightPosR.Num(), 1))
	{
		TestEqual(TEXT("missing table preserves the component light data"),
			Existing->LightPosR[0], FVector4(1, 2, 3, 4));
	}
	TestEqual(TEXT("missing table preserves component ambient"),
		Existing->Ambient, FLinearColor(0.1f, 0.2f, 0.3f));
	TestEqual(TEXT("existing-component inert path still leaves material untouched"),
		Actor->Mesh->GetMaterial(0), Before);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
