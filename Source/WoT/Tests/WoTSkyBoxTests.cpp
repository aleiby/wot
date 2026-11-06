// Specs for the AWoTSkyBox collision contract.
//
// The camera-locked sky mesh envelops the entire world every frame, so it must
// NEVER be query-collidable: a placed skybox with collision on makes every
// pawn's CMC FindFloor start-penetrating (FloorDist == -CapsuleRadius,
// bWalkableFloor=false) and zeroes all walking velocity on the map.
//
// The trap being pinned here: constructor SetCollisionEnabled(NoCollision)
// alone is NOT honored for map-PLACED instances — UPrimitiveComponent::PostLoad
// runs FBodyInstance::LoadProfileData(false), which reapplies the serialized
// collision PROFILE template (StaticMeshActor default "BlockAllDynamic" →
// QueryAndPhysics), clobbering the constructor value. The class must therefore
// set the NoCollision *profile* (so the reapplication is a no-op) and re-assert
// NoCollision at PostInitializeComponents (so stale umaps that already
// serialized a clobbered state heal at runtime).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTSkyBox.h"
#include "../World/WoTRuntimeBSPRenderer.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneCaptureComponentCube.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "RHI.h"
#include "RenderingThread.h"

namespace
{
}

// The CDO contract: the profile NAME must be NoCollision, not just the enabled
// flag — the profile is what PostLoad reapplies over placed instances.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSkyBoxNoCollisionProfileSpec,
	"WoT.SkyBox.NoCollisionProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSkyBoxNoCollisionProfileSpec::RunTest(const FString&)
{
	const AWoTSkyBox* CDO = GetDefault<AWoTSkyBox>();
	const UStaticMeshComponent* SMC = CDO->GetStaticMeshComponent();
	TestNotNull(TEXT("CDO mesh component"), SMC);
	if (SMC)
	{
		TestEqual(TEXT("CDO collision profile"),
			SMC->GetCollisionProfileName(),
			UCollisionProfile::NoCollision_ProfileName);
		TestEqual(TEXT("CDO collision enabled"),
			(int32)SMC->GetCollisionEnabled(),
			(int32)ECollisionEnabled::NoCollision);
		TestEqual(TEXT("camera-centered sky forces the real LOD0"), SMC->ForcedLodModel, 1);
		TestEqual(TEXT("camera-locked sky can move"), SMC->Mobility,
			TEnumAsByte<EComponentMobility::Type>(EComponentMobility::Movable));
	}
	return true;
}

// The load-path contract: after the exact PostLoad profile reapplication that
// clobbered placed instances (LoadProfileData(false)) AND with a stale umap's
// serialized state (profile "BlockAllDynamic" + QueryAndPhysics, what the
// an older map re-save captured), a spawned+initialized skybox must still
// end up query-disabled.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSkyBoxCollisionSurvivesLoadSpec,
	"WoT.SkyBox.CollisionSurvivesProfileReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSkyBoxCollisionSurvivesLoadSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Fresh spawn + the PostLoad reapplication: with the NoCollision profile the
	// template reapply must be a no-op.
	{
		AWoTSkyBox* Box = World->SpawnActor<AWoTSkyBox>();
		UStaticMeshComponent* SMC = Box->GetStaticMeshComponent();
		SMC->GetBodyInstance()->LoadProfileData(false);
		TestEqual(TEXT("collision after profile reload"),
			(int32)SMC->GetCollisionEnabled(),
			(int32)ECollisionEnabled::NoCollision);
	}

	// Stale-umap shape: instance deserialized with the clobbered blocking state
	// (as the re-saved maps carry). Deferred spawn stamps it before
	// PostInitializeComponents, which must heal it back to NoCollision.
	{
		AWoTSkyBox* Box = World->SpawnActorDeferred<AWoTSkyBox>(
			AWoTSkyBox::StaticClass(), FTransform::Identity);
		UStaticMeshComponent* SMC = Box->GetStaticMeshComponent();
		SMC->SetCollisionProfileName(
			UCollisionProfile::BlockAllDynamic_ProfileName);
		SMC->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->FinishSpawning(FTransform::Identity);
		TestEqual(TEXT("collision healed at init"),
			(int32)SMC->GetCollisionEnabled(),
			(int32)ECollisionEnabled::NoCollision);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Render the same -X direction once from the child mesh itself and once
// through the captured cube/window master. Both targets are raw BGRA8 scene
// colour, so an unchanged display-space transport differs by at most two
// quantization levels after the extra capture/sample boundary.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSkyWindowColorTransportSpec,
	"WoT.Sky.ColorTransport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSkyWindowColorTransportSpec::RunTest(const FString&)
{
	if (GUsingNullRHI || !FApp::CanEverRender())
	{
		AddInfo(TEXT("null/non-rendering RHI: render-backed color transport skipped"));
		return true;
	}
	WoTTest::FSpecWorldOptions WorldOptions;
	WorldOptions.bStartPlay = false;
	WorldOptions.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
	if (!TestNotNull(TEXT("color transport world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (World->Scene == nullptr)
	{
		AddInfo(TEXT("spec world has no rendering scene: color transport skipped"));
		return true;
	}

	UStaticMesh* Cube = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* Master = LoadObject<UMaterialInterface>(
		nullptr, WoTRuntimeSkyWindowMasterPath);
	UMaterialInterface* SourceMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/Instances/MI_Swatch_128.MI_Swatch_128"));
	if (!TestNotNull(TEXT("engine cube"), Cube)
		|| !TestNotNull(TEXT("generated sky-window master"), Master)
		|| !TestNotNull(TEXT("constant-emissive source master"), SourceMaster))
		return false;
	// Newly generated or first-loaded editor materials may still expose the
	// default fallback proxy until their shader maps finish. The pixel oracle
	// must sample the intended masters, not a transient black fallback.
	Master->EnsureIsComplete();
	SourceMaster->EnsureIsComplete();

	AWoTSkyBox* Sky = World->SpawnActor<AWoTSkyBox>();
	if (!TestNotNull(TEXT("source child sky"), Sky)) return false;
	Sky->SetActorTickEnabled(false);
	Sky->SetActorScale3D(FVector(100.0));
	Sky->GetStaticMeshComponent()->SetStaticMesh(Cube);
	// The engine test cube is wound outward; render its interior as the child
	// sky without requiring a special test-only material asset.
	Sky->GetStaticMeshComponent()->SetReverseCulling(true);
	Sky->GetStaticMeshComponent()->SetMaterial(0, SourceMaster);
	if (!TestTrue(TEXT("source child sky initializes its cube capture"),
		Sky->InitializeSkyWindowCapture())) return false;

	AStaticMeshActor* Window = World->SpawnActor<AStaticMeshActor>(
		FVector(-100.0, 0.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("test window"), Window)) return false;
	UStaticMeshComponent* WindowMesh = Window->GetStaticMeshComponent();
	WindowMesh->SetMobility(EComponentMobility::Movable);
	WindowMesh->SetStaticMesh(Cube);
	WindowMesh->SetWorldScale3D(FVector(0.01, 2.0, 2.0));
	WindowMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WindowMesh->SetCastShadow(false);
	UMaterialInstanceDynamic* WindowMaterial =
		UMaterialInstanceDynamic::Create(Master, Window);
	WindowMaterial->SetTextureParameterValue(WoTRuntimeSkyWindowCubeParameter,
		Sky->GetSkyCaptureTarget());
	WindowMesh->SetMaterial(0, WindowMaterial);
	World->UpdateWorldComponents(false, false);
	// Let the standalone test world publish the new proxies and finish the
	// first texture/material update before issuing synchronous manual probes.
	WoTTest::TickWorld(World, 2);
	FlushRenderingCommands();

	Sky->GetSkyCapture()->bCaptureEveryFrame = false;
	Sky->GetSkyCapture()->CaptureScene();
	FlushRenderingCommands();
	Sky->GetSkyCapture()->bCaptureEveryFrame = true;

	constexpr int32 TargetSize = 64;
	auto MakeProbe = [World](const TCHAR* Name, UPrimitiveComponent* Only,
		UTextureRenderTarget2D*& OutTarget)
	{
		OutTarget = NewObject<UTextureRenderTarget2D>(
			World, *FString::Printf(TEXT("%sTarget"), Name), RF_Transient);
		OutTarget->ClearColor = FLinearColor::Black;
		OutTarget->bAutoGenerateMips = false;
		OutTarget->InitCustomFormat(TargetSize, TargetSize, PF_B8G8R8A8, true);
		OutTarget->UpdateResourceImmediate(true);
		ASceneCapture2D* Actor = World->SpawnActor<ASceneCapture2D>(
			FVector::ZeroVector, FRotator(0.0, 180.0, 0.0));
		USceneCaptureComponent2D* Capture = Actor != nullptr
			? Actor->GetCaptureComponent2D() : nullptr;
		if (Capture == nullptr) return static_cast<USceneCaptureComponent2D*>(nullptr);
		Capture->TextureTarget = OutTarget;
		Capture->FOVAngle = 20.0f;
		Capture->CaptureSource = SCS_FinalColorLDR;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->bAlwaysPersistRenderingState = false;
		Capture->PrimitiveRenderMode =
			ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		Capture->ShowOnlyComponent(Only);
		return Capture;
	};
	UTextureRenderTarget2D* DirectTarget = nullptr;
	UTextureRenderTarget2D* WindowTarget = nullptr;
	USceneCaptureComponent2D* Direct = MakeProbe(
		TEXT("DirectSky"), Sky->GetStaticMeshComponent(), DirectTarget);
	USceneCaptureComponent2D* ThroughWindow = MakeProbe(
		TEXT("ThroughWindow"), WindowMesh, WindowTarget);
	if (!TestNotNull(TEXT("direct sky probe"), Direct)
		|| !TestNotNull(TEXT("window probe"), ThroughWindow)) return false;
	World->UpdateWorldComponents(false, false);
	WoTTest::TickWorld(World, 2);
	FlushRenderingCommands();
	Direct->CaptureScene();
	ThroughWindow->CaptureScene();
	FlushRenderingCommands();

	auto CenterPixel = [this](const TCHAR* Name,
		UTextureRenderTarget2D* Target, FColor& Out)
	{
		TArray<FColor> Pixels;
		FTextureRenderTargetResource* Resource =
			Target != nullptr ? Target->GameThread_GetRenderTargetResource() : nullptr;
		if (Resource == nullptr || !Resource->ReadPixels(Pixels)
			|| Pixels.Num() != TargetSize * TargetSize) return false;
		Out = Pixels[(TargetSize / 2) * TargetSize + TargetSize / 2];
		int32 NonBlack = 0;
		uint8 MaxChannel = 0;
		for (const FColor Pixel : Pixels)
		{
			if (Pixel.R != 0 || Pixel.G != 0 || Pixel.B != 0) ++NonBlack;
			MaxChannel = FMath::Max3(MaxChannel, Pixel.R,
				FMath::Max(Pixel.G, Pixel.B));
		}
		AddInfo(FString::Printf(TEXT("%s target: nonblack=%d/%d max=%u center=%u,%u,%u"),
			Name, NonBlack, Pixels.Num(), MaxChannel, Out.R, Out.G, Out.B));
		return true;
	};
	FColor DirectColor;
	FColor WindowColor;
	if (!TestTrue(TEXT("direct sky sample reads"),
		CenterPixel(TEXT("direct"), DirectTarget, DirectColor))
		|| !TestTrue(TEXT("window sample reads"),
			CenterPixel(TEXT("window"), WindowTarget, WindowColor))) return false;
	TestTrue(TEXT("direct sky sample is meaningful"),
		DirectColor.R != 0 || DirectColor.G != 0 || DirectColor.B != 0);
	const int32 MaxDelta = FMath::Max3(
		FMath::Abs(static_cast<int32>(DirectColor.R) - WindowColor.R),
		FMath::Abs(static_cast<int32>(DirectColor.G) - WindowColor.G),
		FMath::Abs(static_cast<int32>(DirectColor.B) - WindowColor.B));
	AddInfo(FString::Printf(TEXT("raw color transport: direct %u,%u,%u; "
		"window %u,%u,%u; delta %d"),
		DirectColor.R, DirectColor.G, DirectColor.B,
		WindowColor.R, WindowColor.G, WindowColor.B, MaxDelta));
	TestTrue(FString::Printf(TEXT("raw display-space RGB transport is within 2/255 "
		"(direct %u,%u,%u; window %u,%u,%u; delta %d)"),
		DirectColor.R, DirectColor.G, DirectColor.B,
		WindowColor.R, WindowColor.G, WindowColor.B, MaxDelta), MaxDelta <= 2);
	return !HasAnyErrors();
}

// Two overlapping translucent child-sky layers must stack: each draws as
// BLEND_Modulate by M*(1-Tex) then BLEND_Additive Tex*M, in far-to-near
// priority order (sky_layer_materials.py, WoTRuntimeMapSkyLayerOrder). The old
// SceneColor material read the pre-translucency scene, so a nearer layer
// replaced the farther one (Arena_02's sepia sky). Each layer applies the same
// affine map v -> a*v + c, so with background v0, one layer v1 and both v2:
// v2 - v1 = a*(v1 - v0), a = M*(1-Tex). An overwrite would give v2 == v1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSkyLayerStackingSpec,
	"WoT.Sky.LayerStacking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSkyLayerStackingSpec::RunTest(const FString&)
{
	if (GUsingNullRHI || !FApp::CanEverRender())
	{
		AddInfo(TEXT("null/non-rendering RHI: render-backed layer stacking skipped"));
		return true;
	}
	WoTTest::FSpecWorldOptions WorldOptions;
	WorldOptions.bStartPlay = false;
	WorldOptions.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
	if (!TestNotNull(TEXT("layer stacking world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (World->Scene == nullptr)
	{
		AddInfo(TEXT("spec world has no rendering scene: layer stacking skipped"));
		return true;
	}
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Background = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/Instances/MI_Swatch_128.MI_Swatch_128"));
	UMaterialInterface* ModulateMaster = LoadObject<UMaterialInterface>(
		nullptr, WoTRuntimeSkyModulateMasterPath);
	UMaterialInterface* AddMaster = LoadObject<UMaterialInterface>(
		nullptr, WoTRuntimeSkyAddMasterPath);
	if (!TestNotNull(TEXT("engine cube"), Cube) || !TestNotNull(TEXT("engine plane"), Plane)
		|| !TestNotNull(TEXT("constant-emissive background"), Background)
		|| !TestNotNull(TEXT("generated sky modulate master"), ModulateMaster)
		|| !TestNotNull(TEXT("generated sky add master"), AddMaster))
		return false;
	Background->EnsureIsComplete();
	ModulateMaster->EnsureIsComplete();
	AddMaster->EnsureIsComplete();

	AWoTSkyBox* Sky = World->SpawnActor<AWoTSkyBox>();
	if (!TestNotNull(TEXT("child sky"), Sky)) return false;
	Sky->SetActorTickEnabled(false);
	Sky->SetActorScale3D(FVector(100.0));
	Sky->GetStaticMeshComponent()->SetStaticMesh(Cube);
	Sky->GetStaticMeshComponent()->SetReverseCulling(true);
	Sky->GetStaticMeshComponent()->SetMaterial(0, Background);
	// Runtime layers always carry the slot's Tex and Lightmap. Bind mid-gray
	// (128) to both: Tex ~0.5 and M = 2*Lightmap ~1, so a = M*(1-Tex) ~0.5.
	UTexture2D* Gray = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
	if (!TestNotNull(TEXT("transient gray texture"), Gray)) return false;
	Gray->SRGB = false;
	{
		FTexture2DMipMap& Mip = Gray->GetPlatformData()->Mips[0];
		FColor* Texel = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
		*Texel = FColor(128, 128, 128, 255);
		Mip.BulkData.Unlock();
	}
	Gray->UpdateResource();
	UMaterialInstanceDynamic* Modulate = UMaterialInstanceDynamic::Create(ModulateMaster, Sky);
	UMaterialInstanceDynamic* Add = UMaterialInstanceDynamic::Create(AddMaster, Sky);
	for (UMaterialInstanceDynamic* Pass : {Modulate, Add})
	{
		Pass->SetTextureParameterValue(TEXT("Tex"), Gray);
		Pass->SetTextureParameterValue(TEXT("Lightmap"), Gray);
		Pass->SetScalarParameterValue(TEXT("DynLightEnable"), 0.0f);
	}
	TArray<UStaticMeshComponent*> Layers;
	for (const double Distance : {600.0, 300.0})   // far layer first, then near
	{
		const int32 Rank = Layers.Num() / 2;
		for (UMaterialInterface* Pass : {static_cast<UMaterialInterface*>(Modulate),
			static_cast<UMaterialInterface*>(Add)})
		{
			UStaticMeshComponent* Layer = Sky->AddSkyLayer(Plane, Pass,
				2 * Rank + (Pass == Add ? 1 : 0));
			if (!TestNotNull(TEXT("sky layer component"), Layer)) return false;
			// A vertical plane across the probe's view, Distance along -X.
			Layer->SetWorldLocationAndRotation(FVector(-Distance, 0.0, 0.0),
				FRotator(90.0, 0.0, 0.0));
			Layer->SetWorldScale3D(FVector(8.0));
			Layers.Add(Layer);
		}
	}
	World->UpdateWorldComponents(false, false);
	WoTTest::TickWorld(World, 2);
	FlushRenderingCommands();

	constexpr int32 TargetSize = 32;
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(
		World, TEXT("LayerStackingTarget"), RF_Transient);
	Target->ClearColor = FLinearColor::Black;
	Target->bAutoGenerateMips = false;
	Target->InitCustomFormat(TargetSize, TargetSize, PF_FloatRGBA, true);
	Target->UpdateResourceImmediate(true);
	ASceneCapture2D* ProbeActor = World->SpawnActor<ASceneCapture2D>(
		FVector::ZeroVector, FRotator(0.0, 180.0, 0.0));
	USceneCaptureComponent2D* Probe = ProbeActor != nullptr
		? ProbeActor->GetCaptureComponent2D() : nullptr;
	if (!TestNotNull(TEXT("stacking probe"), Probe)) return false;
	Probe->TextureTarget = Target;
	Probe->FOVAngle = 20.0f;
	Probe->CaptureSource = SCS_SceneColorHDR;
	Probe->bCaptureEveryFrame = false;
	Probe->bCaptureOnMovement = false;
	Probe->bAlwaysPersistRenderingState = false;
	Probe->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	World->UpdateWorldComponents(false, false);

	// Capture with the first ShownLayers layer components visible.
	auto Capture = [&](int32 ShownLayers, FLinearColor& Out)
	{
		Probe->ClearShowOnlyComponents();
		Probe->ShowOnlyComponent(Sky->GetStaticMeshComponent());
		for (int32 Index = 0; Index < ShownLayers; ++Index)
			Probe->ShowOnlyComponent(Layers[Layers.Num() - ShownLayers + Index]);
		WoTTest::TickWorld(World, 1);
		FlushRenderingCommands();
		Probe->CaptureScene();
		FlushRenderingCommands();
		TArray<FLinearColor> Pixels;
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		if (Resource == nullptr || !Resource->ReadLinearColorPixels(Pixels)
			|| Pixels.Num() != TargetSize * TargetSize) return false;
		Out = Pixels[(TargetSize / 2) * TargetSize + TargetSize / 2];
		return true;
	};
	FLinearColor V0, V1, V2;
	// 0 layers, the near layer's two passes, then all four (far then near).
	if (!TestTrue(TEXT("background capture"), Capture(0, V0))
		|| !TestTrue(TEXT("one-layer capture"), Capture(2, V1))
		|| !TestTrue(TEXT("two-layer capture"), Capture(4, V2))) return false;
	// Each pass alone over the background: the modulate pass scales the scene
	// by M*(1-Tex) ~0.5; the add pass adds Tex*M ~0.5.
	auto CaptureOnly = [&](UStaticMeshComponent* Only, FLinearColor& Out)
	{
		Probe->ClearShowOnlyComponents();
		Probe->ShowOnlyComponent(Sky->GetStaticMeshComponent());
		Probe->ShowOnlyComponent(Only);
		WoTTest::TickWorld(World, 1);
		FlushRenderingCommands();
		Probe->CaptureScene();
		FlushRenderingCommands();
		TArray<FLinearColor> Pixels;
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		if (Resource == nullptr || !Resource->ReadLinearColorPixels(Pixels)
			|| Pixels.Num() != TargetSize * TargetSize) return false;
		Out = Pixels[(TargetSize / 2) * TargetSize + TargetSize / 2];
		return true;
	};
	FLinearColor ModulateOnly, AddOnly;
	if (TestTrue(TEXT("modulate-only capture"), CaptureOnly(Layers[2], ModulateOnly))
		&& TestTrue(TEXT("add-only capture"), CaptureOnly(Layers[3], AddOnly)))
	{
		AddInfo(FString::Printf(TEXT("modulate only %.4f, add only %.4f"),
			ModulateOnly.R, AddOnly.R));
		TestTrue(TEXT("the modulate pass multiplies the scene by M*(1-Tex)"),
			FMath::IsNearlyEqual(ModulateOnly.R, V0.R * 0.5f, 0.02f));
		TestTrue(TEXT("the add pass adds Tex*M"),
			FMath::IsNearlyEqual(AddOnly.R - V0.R, 0.5f, 0.02f));
	}
	AddInfo(FString::Printf(TEXT("layer stacking: background %.4f, one layer %.4f, two layers %.4f"),
		V0.R, V1.R, V2.R));
	TestTrue(TEXT("the background renders"), V0.R > 0.05f);
	TestTrue(TEXT("one layer changes the background"), FMath::Abs(V1.R - V0.R) > 0.02f);
	const float Ratio = FMath::Abs(V1.R - V0.R) > KINDA_SMALL_NUMBER
		? (V2.R - V1.R) / (V1.R - V0.R) : 0.0f;
	TestTrue(FString::Printf(TEXT("the second layer composites over the first "
		"(v2-v1 = a*(v1-v0), a=%.3f, expected ~0.5)"), Ratio),
		Ratio > 0.4f && Ratio < 0.6f);
	TestTrue(TEXT("channels stack alike"), FMath::IsNearlyEqual(V2.R, V2.G, 0.02f)
		&& FMath::IsNearlyEqual(V2.R, V2.B, 0.02f));
	return !HasAnyErrors();
}

#endif
