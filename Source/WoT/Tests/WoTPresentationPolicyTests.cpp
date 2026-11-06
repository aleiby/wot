// Presentation-only animation is skipped on dedicated servers: the animated
// light-map owner, the light-wave MPC driver, the fractal simulation and the
// fog spheres all ask WoTPresentationAnimationEnabled.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "WoTTestWorld.h"

#include "../WoTBSPPayload.h"
#include "../WoTFogLight.h"
#include "../WoTFractalSubsystem.h"
#include "../WoTLightWave.h"
#include "../WoTPresentationPolicy.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FWoTBSPPayloadData MakeAnimatedAtlasData()
	{
		FWoTBSPPayloadData Data;
		Data.FormatVersion = 5;
		Data.LightmapWidth = 8;
		Data.LightmapHeight = 8;
		for (int32 Pixel = 0; Pixel < 64; ++Pixel)
		{
			Data.LightmapBGRA.Add(100); Data.LightmapBGRA.Add(120);
			Data.LightmapBGRA.Add(140); Data.LightmapBGRA.Add(255);
		}
		FWoTBSPAnimationSurface& Surface = Data.AnimationSurfaces.AddDefaulted_GetRef();
		Surface.X = 1; Surface.Y = 1; Surface.W = 2; Surface.H = 2;
		FWoTBSPAnimationGroup& Group = Data.AnimationGroups.AddDefaulted_GetRef();
		Group.Type = 2; Group.Period = 64; Group.Phase = 0; Group.LightCount = 1;
		FWoTBSPAnimationTerm& Term = Data.AnimationTerms.AddDefaulted_GetRef();
		Term.Kind = 0; Term.Group = 0; Term.Effect = 0; Term.LightCount = 1;
		FWoTBSPAnimationRect& Rect = Term.Rects.AddDefaulted_GetRef();
		Rect.SurfaceIndex = 0;
		Rect.RGB.Init(10, 12);
		return Data;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPresentationDedicatedServerSpec,
	"WoT.Presentation.DedicatedServerSkipsAnimation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPresentationDedicatedServerSpec::RunTest(const FString&)
{
	WoTSetPresentationDedicatedServerOverrideForTesting(true);
	ON_SCOPE_EXIT
	{
		WoTSetPresentationDedicatedServerOverrideForTesting(TOptional<bool>());
	};
	TestFalse(TEXT("the predicate reports a dedicated server"),
		WoTPresentationAnimationEnabled(nullptr));

	// A ready world (payload + world receipts granted by the fixture).
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;

	// 1. The runtime BSP owner refuses the animated trailer: no base copy, no
	//    staging, no tick.
	const FWoTBSPPayloadData Animated = MakeAnimatedAtlasData();
	AWoTRuntimeBSPWorld* Owner = World->SpawnActor<AWoTRuntimeBSPWorld>();
	UTexture2D* Lightmap = WoTRuntimeBSPMaterializeLightmap(Animated);
	if (TestNotNull(TEXT("owner"), Owner) && TestNotNull(TEXT("lightmap"), Lightmap))
	{
		Owner->SetLightmapAnimation(Animated, Lightmap);
		TestFalse(TEXT("dedicated: the trailer is not adopted"), Owner->HasLightmapAnimation());
		TestFalse(TEXT("dedicated: no tick duty"), Owner->ShouldTick());
		TestFalse(TEXT("dedicated: tick stays off"), Owner->IsActorTickEnabled());
		Owner->RecomposeLightmap(1.0f, false);
		TestEqual(TEXT("dedicated: nothing is staged"), Owner->GetLastUploadBatchCount(), 0);
		TestEqual(TEXT("dedicated: no staging"), Owner->GetUploadStagingBytes(), 0ull);
	}

	// 2. The light-wave driver disables itself and leaves the collection at
	//    its frozen defaults.
	AWoTLightWave* Wave = World->SpawnActor<AWoTLightWave>();
	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(
		nullptr, TEXT("/Game/WoT/Materials/MPC_WoTLightWave.MPC_WoTLightWave"));
	if (TestNotNull(TEXT("light wave"), Wave) && TestNotNull(TEXT("collection"), Collection))
	{
		FWoTLightWaveGroup& Group = Wave->Groups.AddDefaulted_GetRef();
		Group.Period = 64;
		Group.Phase = 0;
		Wave->Tick(0.016f);
		TestFalse(TEXT("dedicated: the driver stops ticking"), Wave->IsActorTickEnabled());
		TestEqual(TEXT("dedicated: Freeze keeps the collection default"),
			UKismetMaterialLibrary::GetScalarParameterValue(World, Collection, TEXT("Freeze")),
			1.0f);
		TestEqual(TEXT("dedicated: Wave0 keeps the collection default"),
			UKismetMaterialLibrary::GetScalarParameterValue(World, Collection, TEXT("Wave0")),
			0.5f);
	}

	// 3. A fog light stays dormant at BeginPlay (both spheres hidden).
	AWoTFogLight* Fog = World->SpawnActor<AWoTFogLight>();
	if (TestNotNull(TEXT("fog light"), Fog))
	{
		TestFalse(TEXT("dedicated: fog modulate sphere hidden"),
			Fog->SphereMod && Fog->SphereMod->IsVisible());
		TestFalse(TEXT("dedicated: fog additive sphere hidden"),
			Fog->SphereAdd && Fog->SphereAdd->IsVisible());
	}

	// 4. The fractal subsystem registered nothing and never ticks.
	if (UWoTFractalSubsystem* Fractal = World->GetSubsystem<UWoTFractalSubsystem>())
	{
		TestEqual(TEXT("dedicated: no fractal records"), Fractal->GetRegisteredRecordCount(), 0);
		TestFalse(TEXT("dedicated: fractal subsystem is not tickable"), Fractal->IsTickable());
	}
	else
	{
		AddError(TEXT("fractal subsystem is absent"));
	}

	// Not vacuous: the same owner adopts the trailer once the world is a client.
	WoTSetPresentationDedicatedServerOverrideForTesting(false);
	if (Owner && Lightmap)
	{
		Owner->SetLightmapAnimation(Animated, Lightmap);
		TestTrue(TEXT("client: the trailer is adopted"), Owner->HasLightmapAnimation());
		TestTrue(TEXT("client: tick duty"), Owner->ShouldTick());
	}
	if (Wave)
	{
		Wave->SetActorTickEnabled(true);
		Wave->Tick(0.016f);
		TestTrue(TEXT("client: the driver keeps ticking"), Wave->IsActorTickEnabled());
		TestEqual(TEXT("client: the driver writes Freeze=0"),
			UKismetMaterialLibrary::GetScalarParameterValue(World, Collection, TEXT("Freeze")),
			0.0f);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
