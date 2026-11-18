// The volumetric fog sphere (AWoTFogLight, cache-projected binding) must fog
// the view from outside the sphere AND from inside it. Arena_05's fog once
// vanished as the camera walked into it: the sphere drew only
// its outer faces, so from inside nothing was drawn.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTFogLight.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "RHI.h"
#include "RenderingThread.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFogLightInsideSphereSpec,
	"WoT.Fog.InsideSphere",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFogLightInsideSphereSpec::RunTest(const FString&)
{
	if (GUsingNullRHI || !FApp::CanEverRender())
	{
		AddInfo(TEXT("null/non-rendering RHI: render-backed fog sphere skipped"));
		return true;
	}
	WoTTest::FSpecWorldOptions WorldOptions;
	WorldOptions.bStartPlay = false;
	WorldOptions.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
	if (!TestNotNull(TEXT("fog sphere world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (World->Scene == nullptr)
	{
		AddInfo(TEXT("spec world has no rendering scene: fog sphere skipped"));
		return true;
	}

	AWoTFogLight* Fog = World->SpawnActor<AWoTFogLight>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("fog light"), Fog)) return false;
	Fog->Radius = 325.0f;
	Fog->FogColor = FLinearColor(1.0f, 1.0f, 1.0f);
	Fog->VolBrightness = 1.0f;
	Fog->bRuntimeProjected = true;
	if (!TestTrue(TEXT("the runtime sphere binds"), Fog->BindRuntimeSphere())) return false;
	Fog->SetActorHiddenInGame(false);
	TestTrue(TEXT("the fog sphere draws its inner faces"),
		Fog->SphereAdd->bReverseCulling && Fog->SphereMod->bReverseCulling);
	if (UMaterialInterface* Material = Fog->SphereAdd->GetMaterial(0))
	{
		Material->EnsureIsComplete();
	}
	World->UpdateWorldComponents(false, false);
	WoTTest::TickWorld(World, 2);
	FlushRenderingCommands();

	constexpr int32 TargetSize = 32;
	auto CaptureCenter = [this, World, Fog](const TCHAR* Name, const FVector& From,
		FColor& Out)
	{
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(
			World, *FString::Printf(TEXT("%sTarget"), Name), RF_Transient);
		Target->ClearColor = FLinearColor::Black;
		Target->bAutoGenerateMips = false;
		Target->InitCustomFormat(TargetSize, TargetSize, PF_B8G8R8A8, true);
		Target->UpdateResourceImmediate(true);
		ASceneCapture2D* Actor = World->SpawnActor<ASceneCapture2D>(
			From, FRotator::ZeroRotator);
		USceneCaptureComponent2D* Capture =
			Actor != nullptr ? Actor->GetCaptureComponent2D() : nullptr;
		if (Capture == nullptr) return false;
		Capture->TextureTarget = Target;
		Capture->FOVAngle = 20.0f;
		Capture->CaptureSource = SCS_FinalColorLDR;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->PrimitiveRenderMode =
			ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		Capture->ShowOnlyComponent(Fog->SphereAdd);
		World->UpdateWorldComponents(false, false);
		WoTTest::TickWorld(World, 2);
		FlushRenderingCommands();
		Capture->CaptureScene();
		FlushRenderingCommands();
		TArray<FColor> Pixels;
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		if (Resource == nullptr || !Resource->ReadPixels(Pixels)
			|| Pixels.Num() != TargetSize * TargetSize) return false;
		Out = Pixels[(TargetSize / 2) * TargetSize + TargetSize / 2];
		AddInfo(FString::Printf(TEXT("%s: center %u,%u,%u"), Name, Out.R, Out.G, Out.B));
		return true;
	};
	FColor Outside, Inside;
	if (!TestTrue(TEXT("outside capture reads"),
			CaptureCenter(TEXT("Outside"), FVector(-1000.0, 0.0, 0.0), Outside))
		|| !TestTrue(TEXT("inside capture reads"),
			CaptureCenter(TEXT("Inside"), FVector(-100.0, 0.0, 0.0), Inside)))
		return false;
	TestTrue(TEXT("the sphere fogs the view from outside"), Outside.R > 8);
	TestTrue(TEXT("the sphere still fogs the view from inside"), Inside.R > 8);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
