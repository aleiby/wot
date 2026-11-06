#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../FrontEnd/WoTFrontEndSceneMaterializer.h"
#include "../Payload/WoTFrontEndFontOwner.h"
#include "../Payload/WoTCacheLease.h"
#include "../Payload/WoTVerifiedBlobReader.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTParticleSprayer.h"
#include "../WoTRainCyd.h"
#include "../WoTSkyBox.h"
#include "../WoTBootstrapSubsystem.h"
#include "../WoTFrontEndPlayerController.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTGameInstance.h"
#include "../WoTSessionSubsystem.h"
#include "../World/WoTWorldMaterializer.h"
#include "WoTFrontEndTestData.h"
#include "WoTTestWorld.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponentCube.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Async/TaskGraphInterfaces.h"
#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/Hash.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
constexpr EAutomationTestFlags WoTFrontEndSceneFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FWoTFrontEndDescriptor MakeSceneDescriptor()
{
	FWoTFrontEndDescriptor Descriptor;
	Descriptor.MapName = TEXT("SyntheticEntry");
	Descriptor.BspLogicalId = TEXT("cache/bsp/SyntheticEntry/SyntheticEntry");
	Descriptor.WorldLogicalId = TEXT("cache/world/SyntheticEntry");
	Descriptor.CameraLocation = FVector(125.5, -64.25, 42.0);
	Descriptor.CameraRotation = FRotator(11.25, 73.5, 0.0);
	Descriptor.HorizontalFov = 90.0f;
	Descriptor.Census.Lights = 39;
	Descriptor.Census.AmbientSounds = 5;
	Descriptor.Census.SplashEmitters = 4;
	Descriptor.Census.RainCylinders = 6;
	Descriptor.Census.LightningActors = 5;
	Descriptor.Census.RandomLightningControllers = 1;
	return Descriptor;
}

FWoTFrontEndSceneTestFixture MakeSceneFixture()
{
	FWoTFrontEndSceneTestFixture Fixture;
	Fixture.SnapshotIdentity = TEXT("generation-a");
	Fixture.FontSnapshotIdentity = TEXT("generation-a");
	Fixture.BSPSurfaceCount = 8;
	Fixture.WaterSurfaceCount = 1;
	Fixture.PositionalAmbienceCount = 5;
	return Fixture;
}

bool IsZeroScene(const FWoTFrontEndSceneObservation& Seen)
{
	return Seen.ObservableActors == 0
		&& Seen.VisibleComponents == 0
		&& Seen.TickingActors == 0
		&& Seen.CollidingComponents == 0
		&& Seen.AudibleComponents == 0
		&& Seen.RegisteredComponents == 0
		&& Seen.PendingDestroyActors == 0
		&& Seen.PendingDestroyComponents == 0;
}

UStaticMeshComponent* FindCommittedBSPMesh(UWorld& World)
{
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor == nullptr
			|| !Actor->ActorHasTag(TEXT("WoTAuthenticatedFrontEndScene"))) continue;
		TInlineComponentArray<UStaticMeshComponent*> Meshes;
		Actor->GetComponents(Meshes);
		for (UStaticMeshComponent* Mesh : Meshes)
			if (Mesh != nullptr
				&& Mesh->GetFName() == TEXT("AuthenticatedBSPMesh")) return Mesh;
	}
	return nullptr;
}

TArray<uint8> ReadTextureMip(UTexture2D* Texture, int32 Level)
{
	TArray<uint8> Result;
	const FTexturePlatformData* PlatformData = Texture
		? Texture->GetPlatformData() : nullptr;
	if (PlatformData == nullptr || !PlatformData->Mips.IsValidIndex(Level)) return Result;
	FTexture2DMipMap& Mip = const_cast<FTexture2DMipMap&>(PlatformData->Mips[Level]);
	const int64 Size = Mip.BulkData.GetBulkDataSize();
	const uint8* Bytes = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
	if (Bytes != nullptr && Size >= 0 && Size <= MAX_int32)
		Result.Append(Bytes, static_cast<int32>(Size));
	Mip.BulkData.Unlock();
	return Result;
}

TArray<uint8> ReadTextureMipZero(UTexture2D* Texture)
{
	return ReadTextureMip(Texture, 0);
}

bool OverwriteHeldSceneObject(const FString& AbsolutePath,
	TConstArrayView<uint8> Bytes)
{
#if PLATFORM_WINDOWS
	const HANDLE Handle = ::CreateFileW(*AbsolutePath, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (Handle == INVALID_HANDLE_VALUE) return false;
	DWORD Written = 0;
	const BOOL bOk = ::WriteFile(Handle, Bytes.GetData(),
		static_cast<DWORD>(Bytes.Num()), &Written, nullptr);
	::CloseHandle(Handle);
	return bOk != 0 && Written == static_cast<DWORD>(Bytes.Num());
#else
	return FFileHelper::SaveArrayToFile(
		TArrayView64<const uint8>(Bytes.GetData(), Bytes.Num()), *AbsolutePath);
#endif
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneRuntimeColorSpec,
	"WoT.FrontEnd.Scene.RuntimeColorBoundaryAndParticleSampling", WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneRuntimeColorSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FWoTFrontEndWeatherFixtureOptions Options;
	Options.bUseSplash02ParticleTexture = true;
	FString Error;
	if (!TestTrue(TEXT("authenticated Splash02 weather fixture activates"),
		WoTFrontEndTest_BuildWeatherStageFixture(Fixture, Options, Error)))
	{
		AddError(Error); return false;
	}
	FWoTFrontEndSceneMaterializer Owner;
	auto Stage = Owner.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
		*Fixture.Payload, *Fixture.Payload->FrontEndFontOwnerForScene(),
		{}, [] { return false; });
	if (!TestFalse(TEXT("production Splash02 scene stages"), Stage.IsError()))
	{
		AddError(Stage.GetError().ToString()); return false;
	}
	if (!TestFalse(TEXT("production Splash02 scene commits"),
		MoveTemp(Stage).GetValue().Commit().IsError())) return false;

	UCameraComponent* Camera = nullptr;
	for (TActorIterator<AActor> It(Fixture.World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor->ActorHasTag(TEXT("WoTAuthenticatedFrontEndScene"))) continue;
		TInlineComponentArray<UCameraComponent*> Cameras;
		Actor->GetComponents(Cameras);
		for (UCameraComponent* Candidate : Cameras)
			if (Candidate && Candidate->GetFName() == TEXT("AuthenticatedFrontEndCamera"))
				Camera = Candidate;
	}
	if (TestNotNull(TEXT("committed scene owns the front-end view camera"), Camera))
	{
		TestEqual(TEXT("front-end camera enables post processing"),
			Camera->PostProcessBlendWeight, 1.0f);
		TestTrue(TEXT("front-end camera overrides the filmic tone curve"),
			Camera->PostProcessSettings.bOverride_ToneCurveAmount);
		TestEqual(TEXT("front-end camera disables the filmic tone curve"),
			Camera->PostProcessSettings.ToneCurveAmount, 0.0f);
		TestTrue(TEXT("front-end camera overrides gamut expansion"),
			Camera->PostProcessSettings.bOverride_ExpandGamut);
		TestEqual(TEXT("front-end camera disables gamut expansion"),
			Camera->PostProcessSettings.ExpandGamut, 0.0f);
		const TArray<FWeightedBlendable>& Blendables =
			Camera->PostProcessSettings.WeightedBlendables.Array;
		TestEqual(TEXT("front-end camera has only the neutral gamma boundary"),
			Blendables.Num(), 1);
		if (Blendables.Num() == 1)
		{
			TestEqual(TEXT("neutral gamma boundary has full weight"),
				Blendables[0].Weight, 1.0f);
			TestEqual(TEXT("front-end camera uses the established gamma boundary"),
				Blendables[0].Object
					? Blendables[0].Object->GetPathName() : FString(),
				FString(TEXT("/Game/WoT/Materials/Instances/MI_GammaBoundary.MI_GammaBoundary")));
		}
	}

	int32 SprayerCount = 0;
	UTexture2D* SharedParticleTexture = nullptr;
	for (TActorIterator<AWoTParticleSprayer> It(Fixture.World); It; ++It)
	{
		AWoTParticleSprayer* Sprayer = *It;
		++SprayerCount;
		TestEqual(TEXT("authored sprayer has one additive material"),
			Sprayer->AddMaterials.Num(), 1);
		TestEqual(TEXT("authored sprayer has one modulated material"),
			Sprayer->ModMaterials.Num(), 1);
		for (UMaterialInterface* Interface : {
			Sprayer->AddMaterials.IsEmpty() ? nullptr : Sprayer->AddMaterials[0].Get(),
			Sprayer->ModMaterials.IsEmpty() ? nullptr : Sprayer->ModMaterials[0].Get()})
		{
			UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Interface);
			if (!TestNotNull(TEXT("sprayer binds an actual particle MID"), MID)) continue;
			UTexture* Bound = nullptr;
			TestTrue(TEXT("particle MID has an authenticated Tex binding"),
				MID->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), Bound));
			UTexture2D* Texture = Cast<UTexture2D>(Bound);
			if (!TestNotNull(TEXT("Splash02 binding is a transient texture"), Texture)) continue;
			if (SharedParticleTexture == nullptr) SharedParticleTexture = Texture;
			TestEqual(TEXT("all sprayer materials share authenticated Splash02"),
				Texture, SharedParticleTexture);
			TestTrue(TEXT("particle sampler sees raw palette bytes"),
				!Texture->SRGB && Texture->Filter == TF_Nearest && Texture->NeverStream);
		}
	}
	TestEqual(TEXT("all four authored sprayers bind Splash02"), SprayerCount, 4);
	Owner.Shutdown();
	TestTrue(TEXT("shutdown revokes the scene camera and particle bindings"),
		IsZeroScene(Owner.ObserveForTesting()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneWeatherBindingsSpec,
	"WoT.FrontEnd.Scene.WeatherProductionBindings", WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneWeatherBindingsSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FString Error;
	if (!TestTrue(TEXT("canonical weather authority activates"),
		WoTFrontEndTest_BuildWeatherStageFixture(Fixture, {}, Error)))
	{
		AddError(Error); return false;
	}
	FWoTFrontEndSceneMaterializer Owner;
	FWoTFrontEndScenePreferences Preferences;
	Preferences.AudioVolume = 0;
	auto Stage = Owner.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
		*Fixture.Payload, *Fixture.Payload->FrontEndFontOwnerForScene(), Preferences,
		[] { return false; });
	if (!TestFalse(TEXT("production Stage accepts the authenticated weather fixture"), Stage.IsError()))
	{
		AddError(Stage.GetError().ToString()); return false;
	}
	TestTrue(TEXT("weather is unpublished while staged"), IsZeroScene(Owner.ObserveForTesting()));
	auto Commit = MoveTemp(Stage).GetValue().Commit();
	if (!TestFalse(TEXT("weather commits"), Commit.IsError()))
	{
		AddError(Commit.GetError().ToString()); return false;
	}
	UStaticMeshComponent* WorldMesh = FindCommittedBSPMesh(*Fixture.World);
	UTexture2D* Generic = Owner.GenericTextureForTesting(TEXT("WOT.F_WOTReg30.Texture10"));
	TestTrue(TEXT("generic non-world font texture retains sRGB nearest single-mip policy"),
		Generic && Generic->SRGB && Generic->Filter == TF_Nearest && Generic->GetPlatformData()->Mips.Num() == 1);
	AWoTSkyBox* Sky = nullptr;
	for (TActorIterator<AWoTSkyBox> It(Fixture.World); It; ++It) Sky = *It;
	TestNotNull(TEXT("production commit creates the sky actor"), Sky);
	UTexture* SharedAtlas = nullptr;
	if (TestNotNull(TEXT("production world renderer"), WorldMesh))
	{
		const FStaticMeshLODResources& LOD = WorldMesh->GetStaticMesh()->GetRenderData()->LODResources[0];
		TestEqual(TEXT("world excludes sky and FakeBackdrop indices"), LOD.IndexBuffer.GetNumIndices(), 6u);
		for (int32 Slot = 0; Slot < WorldMesh->GetNumMaterials(); ++Slot)
		{
			UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(WorldMesh->GetMaterial(Slot));
			if (!TestNotNull(TEXT("actual world MID"), MID)) continue;
			UTexture* Atlas = nullptr;
			MID->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Lightmap")), Atlas);
			TestNotNull(TEXT("world receiver binds working atlas"), Atlas);
			if (!SharedAtlas) SharedAtlas = Atlas;
			TestEqual(TEXT("world receivers share atlas"), Atlas, SharedAtlas);
			TestEqual(TEXT("procedural water keeps its existing parent"), MID->Parent->GetName(),
				FString(TEXT("M_WoT_WorldWaveTranslucent")));
		}
		TestTrue(TEXT("lit wall UV1 is preserved"),
			LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(0, 1).Equals(FVector2f(.01f, .02f), .0001f));
	}
	if (Sky)
	{
		UStaticMeshComponent* Mesh = Sky->GetStaticMeshComponent();
		TestFalse(TEXT("front-end sky remains directly visible"),
			Mesh->bVisibleInSceneCaptureOnly);
		TestNull(TEXT("front-end sky does not allocate a runtime cube"),
			Sky->GetSkyCaptureTarget());
		TestTrue(TEXT("front-end sky capture stays dormant"),
			Sky->GetSkyCapture() != nullptr
			&& !Sky->GetSkyCapture()->bCaptureEveryFrame
			&& !Sky->GetSkyCapture()->IsActive());
		TestEqual(TEXT("sky is camera-positioned before first tick"), Sky->GetActorLocation(), Fixture.Descriptor.CameraLocation);
		TestTrue(TEXT("sky rotation is staged before first tick"), Sky->GetActorRotation().Equals(FRotator(0, 90, 0)));
		TestTrue(TEXT("sky scale is staged before first tick"), Sky->GetActorScale3D().Equals(FVector(Sky->SkyScale)) && Sky->SkyScale >= 12);
		TestEqual(TEXT("sky has no collision"), Mesh->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
		TestEqual(TEXT("sky forces LOD0"), Mesh->ForcedLodModel, 1);
		const FStaticMeshLODResources& LOD = Mesh->GetStaticMesh()->GetRenderData()->LODResources[0];
		TestEqual(TEXT("sky has only sky indices"), LOD.IndexBuffer.GetNumIndices(), 3u);
		TestEqual(TEXT("sky render section uses real slot zero"), LOD.Sections[0].MaterialIndex, 0);
		UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0));
		if (TestNotNull(TEXT("actual sky MID"), MID))
		{
			TestEqual(TEXT("masked rain texture uses the authored two-sided sky parent"), MID->Parent->GetName(), FString(TEXT("M_WoT_EntryMasked_TwoSided")));
			float PanU = 0, PanV = 0;
			MID->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("PanU")), PanU);
			MID->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("PanV")), PanV);
			TestEqual(TEXT("actual sky PanU"), PanU, .01f);
			TestEqual(TEXT("actual sky PanV"), PanV, -.02f);
			UTexture* Atlas = nullptr;
			MID->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Lightmap")), Atlas);
			TestEqual(TEXT("sky and water bind the same working atlas"), Atlas, SharedAtlas);
		}
		TestTrue(TEXT("sky valid UV1 is not replaced to brighten it"),
			LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(0, 1).Equals(FVector2f(.11f, .12f), .0001f));
	}
	int32 RainCount = 0;
	UTexture2D* RainTexture = nullptr;
	for (TActorIterator<AWoTRainCyd> It(Fixture.World); It; ++It)
	{
		AWoTRainCyd* Rain = *It;
		++RainCount;
		TestTrue(TEXT("rain is grounded with one half-unit pullback before BeginPlay"), FMath::IsNearlyEqual(Rain->GetActorLocation().Z, -71.5, .001));
		TInlineComponentArray<UStaticMeshComponent*> Meshes;
		Rain->GetComponents(Meshes);
		for (UStaticMeshComponent* Mesh : Meshes)
		{
			UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0));
			if (!TestNotNull(TEXT("rain segment has actual MID"), MID)) continue;
			FLinearColor Ambient;
			MID->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("MLAmbient")), Ambient);
			const float ExpectedGlow = FMath::IsNearlyEqual(Rain->GetActorLocation().X, 10.0) ? .10f : .05f;
			TestTrue(TEXT("per-rain authored ambient survives onto every segment"), Ambient.Equals(FLinearColor(ExpectedGlow, ExpectedGlow, ExpectedGlow, 1), .00001f));
			float UTile = 0;
			MID->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("UTile")), UTile);
			TestEqual(TEXT("rain retains default UTile one"), UTile, 1.f);
			UTexture* Texture = nullptr;
			MID->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), Texture);
			if (!RainTexture) RainTexture = Cast<UTexture2D>(Texture);
			TestEqual(TEXT("all rain segments share semantic skin texture"), Texture, static_cast<UTexture*>(RainTexture));
		}
	}
	TestEqual(TEXT("all six rain actors publish"), RainCount, 6);
	if (TestNotNull(TEXT("actual MID-bound rain skin"), RainTexture))
	{
		const FTexturePlatformData* Data = RainTexture->GetPlatformData();
		TestTrue(TEXT("rain owns one dynamically populated texture slice"), Data && Data->GetNumSlices() == 1 && RainTexture->bNotOfflineProcessed);
		TestTrue(TEXT("rain raw linear bilinear wrap sampling"), !RainTexture->SRGB && RainTexture->Filter == TF_Bilinear && RainTexture->AddressX == TA_Wrap && RainTexture->AddressY == TA_Wrap && RainTexture->NeverStream);
		if (TestTrue(TEXT("rain exposes Glide source levels 1 through 9"), Data && Data->SizeX == 256 && Data->SizeY == 256 && Data->Mips.Num() == 9))
			for (int32 Level = 0; Level < 9; ++Level)
			{
				const int32 Side = 256 >> Level;
				TestEqual(TEXT("authored mip dimension"), Data->Mips[Level].SizeX, Side);
				TArray<uint8> Expected;
				const uint8 Index = static_cast<uint8>(Level + 2);
				for (int32 Pixel = 0; Pixel < Side * Side; ++Pixel)
					if (Pixel == 0 && Side > 1) Expected.Append({0,0,0,0});
					else Expected.Append({static_cast<uint8>(Index * 3), static_cast<uint8>(Index * 2), Index, 255});
				TestTrue(TEXT("every rain mip is exact source palette expansion without dilation"), ReadTextureMip(RainTexture, Level) == Expected);
			}
	}
	Owner.Shutdown();
	TestTrue(TEXT("weather shutdown leaves no scene"), IsZeroScene(Owner.ObserveForTesting()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneV4EntrySpec,
	"WoT.FrontEnd.Scene.AcceptsIdentityBearingV4Entry", WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneV4EntrySpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FWoTFrontEndWeatherFixtureOptions Options;
	Options.bBSPV4 = true;
	FString Error;
	if (!TestTrue(TEXT("v4 weather authority activates"),
		WoTFrontEndTest_BuildWeatherStageFixture(Fixture, Options, Error)))
	{
		AddError(Error);
		return false;
	}
	FWoTFrontEndSceneMaterializer Owner;
	FWoTFrontEndScenePreferences Preferences;
	Preferences.AudioVolume = 0;
	auto Stage = Owner.Stage(*Fixture.World, Fixture.Descriptor,
		*Fixture.WorldLease, *Fixture.Payload,
		*Fixture.Payload->FrontEndFontOwnerForScene(), Preferences,
		[] { return false; });
	if (!TestFalse(TEXT("production Stage accepts the v4 entry payload"),
		Stage.IsError()))
	{
		AddError(Stage.GetError().ToString());
		return false;
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneTextureMaskSpec,
	"WoT.FrontEnd.Scene.TextureAuthoredMaskMIDParents", WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneTextureMaskSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 2);
	for (const bool bMasked : {false, true})
	{
		const FString Phase = bMasked ? TEXT("masked") : TEXT("opaque");
		const FString TwoSidedParent = bMasked
			? TEXT("M_WoT_EntryMasked_TwoSided")
			: TEXT("M_WoT_EntryWorld_TwoSided");
		FWoTFrontEndStageProductionFixture Fixture;
		FWoTFrontEndWeatherFixtureOptions Options;
		Options.bMaskedSurfaceTexture = bMasked;
		Options.bWorldSkyBlendCoverage = true;
		FString Error;
		if (!TestTrue(Phase + TEXT(" authenticated surface fixture activates"),
			WoTFrontEndTest_BuildWeatherStageFixture(Fixture, Options, Error)))
		{
			AddError(Error); return false;
		}
		FWoTFrontEndSceneMaterializer Owner;
		auto Stage = Owner.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
			*Fixture.Payload, *Fixture.Payload->FrontEndFontOwnerForScene(),
			{}, [] { return false; });
		if (!TestFalse(Phase + TEXT(" surface fixture stages"), Stage.IsError()))
		{
			AddError(Stage.GetError().ToString()); return false;
		}
		if (!TestFalse(Phase + TEXT(" surface fixture commits"),
			MoveTemp(Stage).GetValue().Commit().IsError())) return false;

		UStaticMeshComponent* WorldMesh = FindCommittedBSPMesh(*Fixture.World);
		if (TestNotNull(Phase + TEXT(" fixture actual world mesh"), WorldMesh))
		{
			const FStaticMeshLODResources& LOD =
				WorldMesh->GetStaticMesh()->GetRenderData()->LODResources[0];
			TestEqual(TEXT("water and two authored world styles keep separate sections"),
				LOD.Sections.Num(), 3);
			const FString ExpectedParents[] = {
				TEXT("M_WoT_WorldWaveTranslucent"),
				TwoSidedParent,
				TEXT("M_WoT_EntryModulated")};
			TestEqual(Phase + TEXT(" actual world MID count"),
				WorldMesh->GetNumMaterials(), 3);
			for (int32 Slot = 0; Slot < WorldMesh->GetNumMaterials()
				&& Slot < UE_ARRAY_COUNT(ExpectedParents); ++Slot)
			{
				UMaterialInstanceDynamic* MID =
					Cast<UMaterialInstanceDynamic>(WorldMesh->GetMaterial(Slot));
				if (!TestNotNull(TEXT("actual world slot MID"), MID)) continue;
				TestEqual(FString::Printf(TEXT("%s world slot %d parent"), *Phase, Slot),
					MID->Parent->GetName(), ExpectedParents[Slot]);
				if (LOD.Sections.IsValidIndex(Slot))
					TestEqual(TEXT("world render section uses its MID slot"),
						LOD.Sections[Slot].MaterialIndex, Slot);
			}
		}
		AWoTSkyBox* Sky = nullptr;
		for (TActorIterator<AWoTSkyBox> It(Fixture.World); It; ++It) Sky = *It;
		if (TestNotNull(Phase + TEXT(" fixture actual sky actor"), Sky))
		{
			UStaticMeshComponent* SkyMesh = Sky->GetStaticMeshComponent();
			TestEqual(TEXT("mountain-like sky keeps one authored section"),
				SkyMesh->GetNumMaterials(), 1);
			UMaterialInstanceDynamic* MID =
				Cast<UMaterialInstanceDynamic>(SkyMesh->GetMaterial(0));
			if (TestNotNull(TEXT("actual mountain-like sky MID"), MID))
				TestEqual(Phase + TEXT(" sky parent follows texture-authored mask"),
					MID->Parent->GetName(), TwoSidedParent);
		}
		Owner.Shutdown();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneWeatherLifetimeSpec,
	"WoT.FrontEnd.Scene.WeatherUploadReentry", WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneWeatherLifetimeSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FString Error;
	if (!TestTrue(TEXT("weather reentry fixture activates"), WoTFrontEndTest_BuildWeatherStageFixture(Fixture, {}, Error)))
	{ AddError(Error); return false; }
	TArray<TSharedRef<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe>> Jobs;
	TArray<TFunction<void()>> Completions;
	FWoTFrontEndSceneMaterializer Owner;
	Owner.SetAtlasUploaderForTesting([&](TSharedRef<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe> Job, TFunction<void()> Complete)
	{
		Jobs.Add(Job); Completions.Add(MoveTemp(Complete)); return true;
	});
	FWoTFrontEndScenePreferences Preferences; Preferences.AudioVolume = 0;
	auto Stage = Owner.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
		*Fixture.Payload, *Fixture.Payload->FrontEndFontOwnerForScene(), Preferences, [] { return false; });
	if (!TestFalse(TEXT("weather lifetime stages"), Stage.IsError())) { AddError(Stage.GetError().ToString()); return false; }
	if (!TestFalse(TEXT("weather lifetime commits"), MoveTemp(Stage).GetValue().Commit().IsError())) return false;
	TSharedPtr<FWoTFrontEndSceneController> OldController = Owner.ControllerForTesting();
	if (!TestTrue(TEXT("committed controller runs"), OldController && OldController->IsRunning())) return false;
	OldController->TickForTesting(21.f);
	OldController->TickForTesting(.01f);
	TestEqual(TEXT("muted scene still submits two visual lightning updates"), Jobs.Num(), 2);
	TestEqual(TEXT("muted weather has no audio"), Owner.ObserveForTesting().AudibleComponents, 0u);
	int32 VisibleLights = 0;
	for (TActorIterator<AActor> It(Fixture.World); It; ++It)
	{
		TInlineComponentArray<UPointLightComponent*> Lights; It->GetComponents(Lights);
		for (const UPointLightComponent* Light : Lights) VisibleLights += Light->IsVisible() ? 1 : 0;
	}
	TestEqual(TEXT("atlas lightning does not double-brighten with five point lights"), VisibleLights, 34);
	UTexture* OldAtlas = nullptr;
	CastChecked<UMaterialInstanceDynamic>(FindCommittedBSPMesh(*Fixture.World)->GetMaterial(0))->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Lightmap")), OldAtlas);
	Owner.SuspendBeforeTravel();
	TestFalse(TEXT("suspended controller no longer ticks"), OldController->IsRunning());
	OldController.Reset();
	TestTrue(TEXT("suspension detaches every actor"), IsZeroScene(Owner.ObserveForTesting()));
	if (!TestFalse(TEXT("retained plan reenters"), Owner.RematerializeForTesting(*Fixture.World).IsError())) return false;
	UTexture* NewAtlas = nullptr;
	CastChecked<UMaterialInstanceDynamic>(FindCommittedBSPMesh(*Fixture.World)->GetMaterial(0))->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Lightmap")), NewAtlas);
	TestNotEqual(TEXT("reentry allocates a fresh atlas while old callbacks remain held"), NewAtlas, OldAtlas);
	TArray<uint8> Steady = ReadTextureMipZero(Cast<UTexture2D>(NewAtlas));
	for (TActorIterator<AWoTSkyBox> It(Fixture.World); It; ++It)
	{
		UTexture* SkyAtlas = nullptr;
		CastChecked<UMaterialInstanceDynamic>(It->GetStaticMeshComponent()->GetMaterial(0))->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Lightmap")), SkyAtlas);
		TestEqual(TEXT("reentry rebinds retained sky receivers"), SkyAtlas, NewAtlas);
	}
	for (const auto& Job : Jobs)
	{
		TestEqual(TEXT("accepted callbacks own only old atlas"), Job->Texture.Get(), Cast<UTexture2D>(OldAtlas));
		TestTrue(TEXT("accepted callback owns complete atlas bytes"), Job->Bytes.Num() == 64 && Job->Region.Width == 4 && Job->Region.Height == 4);
	}
	Async(EAsyncExecution::ThreadPool, [&Completions]
	{
		for (auto& Complete : Completions) Complete();
		Completions.Reset();
	}).Get();
	Jobs.Reset();
	TestTrue(TEXT("late retired callbacks cannot change fresh steady bytes"), ReadTextureMipZero(Cast<UTexture2D>(NewAtlas)) == Steady);
	TSharedPtr<FWoTFrontEndSceneController> NewController = Owner.ControllerForTesting();
	NewController->TickForTesting(21.f); NewController->TickForTesting(.01f);
	Owner.RevokeAuthorityForTesting();
	Owner.Shutdown();
	TestFalse(TEXT("revocation terminates replacement controller"), NewController->IsRunning());
	NewController.Reset();
	CollectGarbage(RF_NoFlags);
	TestTrue(TEXT("revocation destroys scene while two uploads are held"), IsZeroScene(Owner.ObserveForTesting()));
	Async(EAsyncExecution::ThreadPool, [&Completions]
	{
		for (auto& Complete : Completions) Complete();
		Completions.Reset();
	}).Get();
	Jobs.Reset();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneWeatherRejectSpec,
	"WoT.FrontEnd.Scene.WeatherRejectsLegacyAndMissingGround", WoTFrontEndSceneFlags)
bool FWoTFrontEndSceneWeatherRejectSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 6);
	for (int32 Variant = 0; Variant < 6; ++Variant)
	{
		FWoTFrontEndWeatherFixtureOptions Options;
		Options.bLegacyV2 = Variant == 0; Options.bMissingGround = Variant == 1;
		Options.bMalformedRainScaleGlow = Variant == 2;
		Options.bMalformedRainAmbientGlow = Variant == 3;
		Options.bMalformedLightningLength = Variant == 4;
		Options.bMismatchedLightMetadata = Variant == 5;
		FWoTFrontEndStageProductionFixture Fixture;
		FString Error;
		if (!TestTrue(TEXT("negative fixture authenticates before Stage"), WoTFrontEndTest_BuildWeatherStageFixture(Fixture, Options, Error)))
		{ AddError(Error); continue; }
		FWoTFrontEndSceneMaterializer Owner;
		auto Stage = Owner.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
			*Fixture.Payload, *Fixture.Payload->FrontEndFontOwnerForScene(), {}, [] { return false; });
		TestTrue(FString::Printf(TEXT("production entry rejects invalid weather variant %d"), Variant), Stage.IsError());
		TestTrue(TEXT("invalid weather publishes no scene"), IsZeroScene(Owner.ObserveForTesting()));
	}
	FWoTFrontEndStageProductionFixture Invalid;
	FWoTFrontEndWeatherFixtureOptions Options; Options.RainScaleGlow.SetNum(5);
	FString Error;
	TestFalse(TEXT("weather builder rejects arrays other than six"), WoTFrontEndTest_BuildWeatherStageFixture(Invalid, Options, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneDirectStagedRematerializeSpec,
	"WoT.FrontEnd.Scene.WeatherDirectStagedRematerialize", WoTFrontEndSceneFlags)
bool FWoTFrontEndSceneDirectStagedRematerializeSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FString Error;
	if (!TestTrue(TEXT("direct reentry fixture activates"), WoTFrontEndTest_BuildWeatherStageFixture(Fixture, {}, Error)))
	{ AddError(Error); return false; }
	TArray<TSharedRef<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe>> Jobs;
	FWoTFrontEndSceneMaterializer Owner;
	Owner.SetAtlasUploaderForTesting([&](TSharedRef<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe> Job, TFunction<void()> Complete)
	{
		Jobs.Add(Job); Complete(); return true;
	});
	auto Stage = Owner.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
		*Fixture.Payload, *Fixture.Payload->FrontEndFontOwnerForScene(), {}, [] { return false; });
	if (!TestFalse(TEXT("direct reentry production Stage succeeds"), Stage.IsError())) { AddError(Stage.GetError().ToString()); return false; }
	TestTrue(TEXT("staged plan is still inert before direct Rematerialize"), IsZeroScene(Owner.ObserveForTesting()));
	if (!TestFalse(TEXT("public Rematerialize accepts its valid uncommitted plan"), Owner.Rematerialize(*Fixture.World).IsError())) return false;
	UStaticMeshComponent* Mesh = FindCommittedBSPMesh(*Fixture.World);
	if (!TestNotNull(TEXT("direct reentry publishes actual world mesh"), Mesh)) return false;
	UTexture* CurrentAtlas = nullptr;
	CastChecked<UMaterialInstanceDynamic>(Mesh->GetMaterial(0))->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Lightmap")), CurrentAtlas);
	Owner.ControllerForTesting()->TickForTesting(21.f);
	if (TestEqual(TEXT("direct reentry submits visual lightning"), Jobs.Num(), 1))
		TestEqual(TEXT("direct staged reentry controller targets actual published atlas"),
			Jobs[0]->Texture.Get(), Cast<UTexture2D>(CurrentAtlas));
	Owner.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneWeatherReducedSpec,
	"WoT.FrontEnd.Scene.WeatherReducedMotionAndBoundaries", WoTFrontEndSceneFlags)
bool FWoTFrontEndSceneWeatherReducedSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FWoTFrontEndWeatherFixtureOptions Options; Options.bDefaultRainGlow = true;
	FString Error;
	if (!TestTrue(TEXT("reduced weather fixture activates"), WoTFrontEndTest_BuildWeatherStageFixture(Fixture, Options, Error)))
	{ AddError(Error); return false; }
	FWoTFrontEndSceneMaterializer Owner;
	const FWoTFrontEndFontOwner& Fonts = *Fixture.Payload->FrontEndFontOwnerForScene();
	for (int32 Boundary = 0; Boundary < 4; ++Boundary)
	{
		bool bCancel = false;
		auto Cancelled = Owner.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
			*Fixture.Payload, Fonts, {}, [&] { return bCancel; },
			[&](const FWoTFrontEndSceneProgress& Progress) { if (static_cast<int32>(Progress.CompletedUnits) == Boundary + 1) bCancel = true; });
		TestTrue(FString::Printf(TEXT("weather cancels at completed stage boundary %d"), Boundary), Cancelled.IsError());
		if (!Cancelled.IsError()) MoveTemp(Cancelled).GetValue().Rollback();
		TestTrue(TEXT("cancelled weather has no publication"), IsZeroScene(Owner.ObserveForTesting()));
	}
	FWoTFrontEndScenePreferences Preferences; Preferences.bReducedMotion = true;
	auto Stage = Owner.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
		*Fixture.Payload, Fonts, Preferences, [] { return false; });
	if (!TestFalse(TEXT("reduced weather stages"), Stage.IsError())) { AddError(Stage.GetError().ToString()); return false; }
	if (!TestFalse(TEXT("reduced weather commits"), MoveTemp(Stage).GetValue().Commit().IsError())) return false;
	TSharedPtr<FWoTFrontEndSceneController> Controller = Owner.ControllerForTesting();
	TArray<TPair<AWoTRainCyd*, FVector>> Rains;
	for (TActorIterator<AWoTRainCyd> It(Fixture.World); It; ++It)
	{
		Rains.Add({*It, It->GetActorLocation()});
		TestFalse(TEXT("actual rain actor tick disabled by reduced motion"), It->IsActorTickEnabled());
		TInlineComponentArray<UStaticMeshComponent*> Meshes; It->GetComponents(Meshes);
		if (Meshes.Num())
		{
			FLinearColor Ambient;
			CastChecked<UMaterialInstanceDynamic>(Meshes[0]->GetMaterial(0))->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("MLAmbient")), Ambient);
			TestTrue(TEXT("omitted typed glow uses ScaleGlow one and AmbientGlow zero"), Ambient.Equals(FLinearColor(.5f,.5f,.5f,1.f)));
		}
	}
	Controller->TickForTesting(21.f);
	Fixture.World->Tick(LEVELTICK_All, .1f);
	for (const auto& Rain : Rains) TestEqual(TEXT("reduced actor ticks leave rain anchored"), Rain.Key->GetActorLocation(), Rain.Value);
	TestEqual(TEXT("reduced motion holds atlas at steady state"), Controller->AcceptedUploadsForTesting(), uint64(0));
	TestEqual(TEXT("reduced motion holds controlled sky time"), Controller->SceneTimeForTesting(), 0.f);
	for (TActorIterator<AWoTSkyBox> It(Fixture.World); It; ++It)
	{
		float Time = -1;
		CastChecked<UMaterialInstanceDynamic>(It->GetStaticMeshComponent()->GetMaterial(0))->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("WoTSceneTime")), Time);
		TestEqual(TEXT("actual reduced sky MID time is frozen"), Time, 0.f);
	}
	Owner.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneWeatherStyleSpec,
	"WoT.FrontEnd.Scene.WeatherSectionStyles", WoTFrontEndSceneFlags)
bool FWoTFrontEndSceneWeatherStyleSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FWoTFrontEndWeatherFixtureOptions Options; Options.bSkyStyleCoverage = true;
	Options.bMaskedSurfaceTexture = true;
	FString Error;
	if (!TestTrue(TEXT("style fixture activates"), WoTFrontEndTest_BuildWeatherStageFixture(Fixture, Options, Error)))
	{ AddError(Error); return false; }
	FWoTFrontEndSceneMaterializer Owner;
	auto Stage = Owner.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
		*Fixture.Payload, *Fixture.Payload->FrontEndFontOwnerForScene(), {}, [] { return false; });
	if (!TestFalse(TEXT("style fixture stages"), Stage.IsError())) { AddError(Stage.GetError().ToString()); return false; }
	if (!TestFalse(TEXT("style fixture commits"), MoveTemp(Stage).GetValue().Commit().IsError())) return false;
	AWoTSkyBox* Sky = nullptr;
	for (TActorIterator<AWoTSkyBox> It(Fixture.World); It; ++It) Sky = *It;
	if (TestNotNull(TEXT("style fixture actual sky"), Sky))
	{
		UStaticMeshComponent* Mesh = Sky->GetStaticMeshComponent();
		const FStaticMeshLODResources& LOD = Mesh->GetStaticMesh()->GetRenderData()->LODResources[0];
		TestEqual(TEXT("same material with different pan rates gets two actual sections"), LOD.Sections.Num(), 2);
		TestTrue(TEXT("sky vertices are recentered around nonzero source origin"),
			LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(0).Equals(FVector3f(-110,-120,70), .001f));
		for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
		{
			UMaterialInstanceDynamic* MID = CastChecked<UMaterialInstanceDynamic>(Mesh->GetMaterial(Slot));
			TestEqual(TEXT("translucent precedence over modulated survives onto actual neutral parent"), MID->Parent->GetName(), FString(TEXT("M_WoT_EntryTranslucent_TwoSided")));
			float Pan = 0;
			MID->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("PanU")), Pan);
			TestEqual(TEXT("per-section pan stays distinct despite common texture"), Pan, Slot == 0 ? .01f : .03f);
			TestEqual(TEXT("render section uses matching real MID slot"), LOD.Sections[Slot].MaterialIndex, Slot);
		}
		for (int32 Vertex = 0; Vertex < LOD.GetNumVertices(); ++Vertex)
			TestTrue(TEXT("unlit surface preserves its existing neutral UV1 mapping"), LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Vertex, 1).Equals(FVector2f(.875f,.875f), .0001f));
		Owner.ControllerForTesting()->TickForTesting(.25f);
		float SceneTime = 0;
		CastChecked<UMaterialInstanceDynamic>(Mesh->GetMaterial(0))->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("WoTSceneTime")), SceneTime);
		TestEqual(TEXT("actual sky MID receives controller time"), SceneTime, .25f);
	}
	Owner.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneRealWeatherSpec,
	"WoT.FrontEnd.Scene.WeatherRealEntryAnchors", WoTFrontEndSceneFlags)
bool FWoTFrontEndSceneRealWeatherSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{ AddWarning(WoTReferenceMissingWarning(TEXT("original"))); return true; }
	if (!FPaths::FileExists(WoTReferencePath(TEXT("original/Maps/WheelOfTime.wot"))))
	{ AddError(TEXT("required original/Maps/WheelOfTime.wot is missing")); return false; }
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FString Error;
	if (!TestTrue(TEXT("real entry imports through authenticated world and v3 decoders"), WoTFrontEndTest_BuildRealWeatherStageFixture(Fixture, Error)))
	{ AddError(Error); return false; }
	FWoTFrontEndSceneMaterializer Owner;
	auto Stage = Owner.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
		*Fixture.Payload, *Fixture.Payload->FrontEndFontOwnerForScene(), {}, [] { return false; });
	if (!TestFalse(TEXT("real entry production Stage validates source metadata"), Stage.IsError())) { AddError(Stage.GetError().ToString()); return false; }
	if (!TestFalse(TEXT("real entry weather commits"), MoveTemp(Stage).GetValue().Commit().IsError())) return false;
	int32 RainCount = 0;
	for (TActorIterator<AWoTRainCyd> It(Fixture.World); It; ++It)
	{
		++RainCount;
		TestTrue(TEXT("real rain anchor includes original trace pullback exactly once"), FMath::IsNearlyEqual(It->GetActorLocation().Z, 360.5, .001));
	}
	TestEqual(TEXT("all six actual entry rains grounded before first tick"), RainCount, 6);
	TestEqual(TEXT("all five actual entry lights routed in payload order"), Owner.ControllerForTesting()->LightningSamplesForTesting().Num(), 5);
	Owner.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneProductionStageSpec,
	"WoT.FrontEnd.Scene.ProductionStageAuthenticatedFixture",
	WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneProductionStageSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 18); // Thirteen retained fixtures plus five converter-slot fixtures.
	FWoTFrontEndStageProductionFixture Good;
	FString Error;
	if (!TestTrue(TEXT("exact attested 48-object fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(Good, false, Error)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}
	const FWoTFrontEndFontOwner* FontOwner =
		Good.Payload->FrontEndFontOwnerForScene();
	TestNotNull(TEXT("Task3 owner shares the retained fixture"), FontOwner);
	TArray<FWoTFrontEndSceneProgress> Progress;
	TMap<EWoTFactDomain, int32> ConsumedResourceStages;
	TArray<int32> ConsumedCountsAtBoundary;
	FWoTFrontEndSceneMaterializer::SetResourceStageObserverForTesting(
		[&ConsumedResourceStages](EWoTFactDomain Domain, int32 Stage)
			{ ConsumedResourceStages.FindOrAdd(Domain) = Stage; });
	FWoTFrontEndScenePreferences ProductionPreferences;
	ProductionPreferences.AudioVolume = 0.5f;
	const float PreviousApplicationVolume = FApp::GetVolumeMultiplier();
	FApp::SetVolumeMultiplier(0.5f);
	ON_SCOPE_EXIT { FApp::SetVolumeMultiplier(PreviousApplicationVolume); };
	FWoTFrontEndSceneMaterializer Materializer;
	auto VerifyCommittedLightmap = [this](UWorld& World,
		const TCHAR* Phase) -> UTexture2D*
	{
		UStaticMeshComponent* BSPMesh = FindCommittedBSPMesh(World);
		TestNotNull(FString::Printf(TEXT("%s publishes the real BSP component"), Phase),
			BSPMesh);
		if (BSPMesh == nullptr) return nullptr;
		UTexture2D* Atlas = nullptr;
		for (int32 Slot = 0; Slot < BSPMesh->GetNumMaterials(); ++Slot)
		{
			UMaterialInstanceDynamic* MID =
				Cast<UMaterialInstanceDynamic>(BSPMesh->GetMaterial(Slot));
			TestNotNull(FString::Printf(TEXT("%s BSP slot %d uses a MID"), Phase, Slot),
				MID);
			UTexture* Bound = nullptr;
			const bool bHasLightmap = MID != nullptr
				&& MID->GetTextureParameterValue(
					FHashedMaterialParameterInfo(TEXT("Lightmap")), Bound);
			TestTrue(FString::Printf(TEXT("%s BSP slot %d exposes Lightmap"),
				Phase, Slot), bHasLightmap);
			UTexture2D* BoundAtlas = Cast<UTexture2D>(Bound);
			TestNotNull(FString::Printf(TEXT("%s BSP slot %d binds an atlas"),
				Phase, Slot), BoundAtlas);
			if (Atlas == nullptr) Atlas = BoundAtlas;
			else TestEqual(FString::Printf(
				TEXT("%s BSP slots share one atlas"), Phase), BoundAtlas, Atlas);
		}
		TestNotNull(FString::Printf(TEXT("%s has a bound atlas"), Phase), Atlas);
		if (Atlas == nullptr) return nullptr;
		const FTexturePlatformData* PlatformData = Atlas->GetPlatformData();
		TestTrue(FString::Printf(TEXT("%s atlas has exact dimensions and one mip"),
			Phase), PlatformData != nullptr && PlatformData->SizeX == 4
			&& PlatformData->SizeY == 4 && PlatformData->Mips.Num() == 1);
		TestTrue(FString::Printf(TEXT("%s atlas samples raw linear bytes"), Phase),
			!Atlas->SRGB && Atlas->Filter == TF_Bilinear
			&& Atlas->AddressX == TA_Clamp && Atlas->AddressY == TA_Clamp
			&& Atlas->NeverStream);
		TArray<uint8> ExpectedPixels;
		for (int32 Pixel = 0; Pixel < 16; ++Pixel) ExpectedPixels.Append({20,40,60,255});
		TestTrue(FString::Printf(TEXT("%s atlas owns the decoded BGRA bytes"), Phase),
			ReadTextureMipZero(Atlas) == ExpectedPixels);
		return Atlas;
	};
	auto Staged = FontOwner && Good.WorldLease.IsValid()
		? Materializer.Stage(*Good.World, Good.Descriptor, *Good.WorldLease,
			*Good.Payload, *FontOwner, ProductionPreferences, []() { return false; },
			[&](const FWoTFrontEndSceneProgress& Value)
			{
				Progress.Add(Value);
				int32 Completed = 0;
				for (const auto& Pair : ConsumedResourceStages)
					Completed += Pair.Value < static_cast<int32>(Value.CompletedUnits);
				ConsumedCountsAtBoundary.Add(Completed);
			})
		: TWoTBootstrapResult<FWoTFrontEndSceneStaging>::Error(
			MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
				TEXT("production fixture prerequisites are absent")));
	if (!TestFalse(TEXT("actual production Stage accepts retained objects"),
		Staged.IsError()))
	{
		FWoTFrontEndSceneMaterializer::SetResourceStageObserverForTesting({});
		if (Staged.IsError()) AddError(Staged.GetError().ToString());
		return false;
	}
	FWoTFrontEndSceneMaterializer::SetResourceStageObserverForTesting({});
	TestTrue(TEXT("actual Stage reports typed completed substages"),
		Progress.Num() == 4
			&& Progress[0].Stage == EWoTFrontEndSceneProgressStage::WorldAndBSP
			&& Progress[1].Stage == EWoTFrontEndSceneProgressStage::SurfacesAndLightmaps
			&& Progress[2].Stage == EWoTFrontEndSceneProgressStage::WeatherAndAmbience
			&& Progress[3].Stage == EWoTFrontEndSceneProgressStage::FontsAndLocalization
			&& Progress[0].CompletedUnits == 1
			&& Progress.Last().CompletedUnits == 4);
	TestTrue(TEXT("typed boundaries follow completed domain consumers"),
		ConsumedResourceStages.FindRef(EWoTFactDomain::BSP) == 0
			&& ConsumedResourceStages.FindRef(EWoTFactDomain::Texture) == 1
			&& ConsumedResourceStages.FindRef(EWoTFactDomain::Sound) == 2
			&& ConsumedResourceStages.FindRef(EWoTFactDomain::Font) == 3
			&& ConsumedCountsAtBoundary.Num() == 4
			&& ConsumedCountsAtBoundary[0] < ConsumedCountsAtBoundary[1]
			&& ConsumedCountsAtBoundary[1] < ConsumedCountsAtBoundary[2]
			&& ConsumedCountsAtBoundary[2] < ConsumedCountsAtBoundary[3]);
	TestTrue(TEXT("actual Stage publishes zero world state"),
		IsZeroScene(Materializer.ObserveForTesting()));
	APlayerController* ViewOwner = Good.World->SpawnActor<APlayerController>();
	AActor* PreviousView = Good.World->SpawnActor<AActor>();
	TestNotNull(TEXT("production scene view owner"), ViewOwner);
	TestNotNull(TEXT("production scene previous view"), PreviousView);
	if (ViewOwner != nullptr && PreviousView != nullptr)
		ViewOwner->SetViewTarget(PreviousView);
	if (!Staged.IsError())
	{
		FWoTBootstrapStatus Committed =
			MoveTemp(Staged).GetValue().Commit(ViewOwner);
		if (!TestFalse(TEXT("actual retained production plan commits"),
			Committed.IsError()) && Committed.IsError())
			AddError(Committed.GetError().ToString());
	}
	if (ViewOwner != nullptr && PreviousView != nullptr)
		TestNotEqual(TEXT("commit publishes the authenticated scene view"),
			ViewOwner->GetViewTarget(), PreviousView);
	TestTrue(TEXT("actual commit binds BSP, water, RainCyd, and splash renderers"),
		Materializer.ObserveForTesting().AuthenticatedBSPRenderers == 1
			&& Materializer.ObserveForTesting().BoundBSPMaterialSections == 1
			&& Materializer.ObserveForTesting().BoundWaterMaterials == 1
			&& Materializer.ObserveForTesting().AuthenticatedRainRenderers == 6
			&& Materializer.ObserveForTesting().AuthoredSplashRenderers == 4);
	UTexture2D* FirstCommittedAtlas = VerifyCommittedLightmap(
		*Good.World, TEXT("initial commit"));
	bool bSawRainSkinTexture = false;
	for (TActorIterator<AWoTRainCyd> It(Good.World); It; ++It)
	{
		TInlineComponentArray<UStaticMeshComponent*> RainMeshes;
		It->GetComponents(RainMeshes);
		for (UStaticMeshComponent* RainMesh : RainMeshes)
		{
			for (int32 Slot = 0; RainMesh != nullptr
				&& Slot < RainMesh->GetNumMaterials(); ++Slot)
			{
				UMaterialInstanceDynamic* MID =
					Cast<UMaterialInstanceDynamic>(RainMesh->GetMaterial(Slot));
				UTexture* Bound = nullptr;
				if (MID == nullptr || !MID->GetTextureParameterValue(
					FHashedMaterialParameterInfo(TEXT("Tex")), Bound)) continue;
				UTexture2D* RainTexture = Cast<UTexture2D>(Bound);
				bSawRainSkinTexture = bSawRainSkinTexture || RainTexture != nullptr;
				TestTrue(TEXT("rain semantic skin uses the verified raw bilinear authored-mip path"),
					RainTexture != nullptr && !RainTexture->SRGB && RainTexture->Filter == TF_Bilinear
					&& RainTexture->GetPlatformData()->Mips.Num() == 9);
			}
		}
	}
	TestTrue(TEXT("committed RainCyd exposes its real skin texture"),
		bSawRainSkinTexture);
	TestEqual(TEXT("commit is the fifth real progress boundary"),
		Progress.Num(), 5);
	TestTrue(TEXT("commit progress is emitted only after success"),
		Progress.Last().Stage == EWoTFrontEndSceneProgressStage::Commit
			&& Progress.Last().CompletedUnits == 5);
	TestEqual(TEXT("all five authored lightning endpoints survive staging"),
		Materializer.ObserveForTesting().LightningEndpointCount, 5u);
	TestEqual(TEXT("every endpoint preserves tag/light-tag/radius/volume"),
		Materializer.ObserveForTesting().PreservedLightningEndpointProperties, 5u);
	TestEqual(TEXT("authenticated initial lightning minimum is preserved"),
		Materializer.ObserveForTesting().MinLightningSeconds, 10.0f);
	TestEqual(TEXT("authenticated initial lightning maximum is preserved"),
		Materializer.ObserveForTesting().MaxLightningSeconds, 20.0f);
	TestTrue(TEXT("deterministic initial strike stays inside authenticated bounds"),
		Materializer.ObserveForTesting().NextLightningSeconds >= 10.0f
			&& Materializer.ObserveForTesting().NextLightningSeconds <= 20.0f);
	TestEqual(TEXT("zero-delay thunder maps to the nearest authored sound"),
		Materializer.ObserveForTesting().NearThunderSoundIndex, 0);
	TestEqual(TEXT("maximum-delay thunder maps to the farthest effective sound"),
		Materializer.ObserveForTesting().FarThunderSoundIndex, 9);
	TestEqual(TEXT("nearest delay preserves authored thunder identity ordering"),
		Materializer.ObserveForTesting().NearThunderSoundIdentity,
		FString(TEXT("environment.thunder.thunder9")));
	TestEqual(TEXT("farthest delay preserves effective thunder identity ordering"),
		Materializer.ObserveForTesting().FarThunderSoundIdentity,
		FString(TEXT("environment.thunder.thunder10")));
	Good.World->Tick(LEVELTICK_All, 2.0f);
	TestEqual(TEXT("all authored ambience remains playing past source duration"),
		Materializer.ObserveForTesting().PlayingAmbientLoops, 5u);
	TestEqual(TEXT("component preserves the authored ambient multiplier"),
		Materializer.ObserveForTesting().FirstAmbientVolume, 380.0f / 255.0f);
	TestEqual(TEXT("application volume times component volume applies .5 once"),
		FApp::GetVolumeMultiplier()
			* Materializer.ObserveForTesting().FirstAmbientVolume,
		190.0f / 255.0f);
	Materializer.SuspendBeforeTravel();
	FWoTBootstrapStatus Rematerialized =
		Materializer.RematerializeForTesting(*Good.World);
	if (!TestFalse(TEXT("retained production plan rematerializes"),
		Rematerialized.IsError()) && Rematerialized.IsError())
		AddError(Rematerialized.GetError().ToString());
	UTexture2D* RematerializedAtlas = VerifyCommittedLightmap(
		*Good.World, TEXT("rematerialization"));
	TestNotEqual(TEXT("rematerialization owns a fresh steady atlas"),
		RematerializedAtlas, FirstCommittedAtlas);
	Materializer.Shutdown();
	if (ViewOwner != nullptr && PreviousView != nullptr)
		TestEqual(TEXT("shutdown restores the previous view immediately"),
			ViewOwner->GetViewTarget(), PreviousView);

	float FaultYaw = 0.0f;
	float FaultFallRate = 0.0f;
	WoTSetRainCydStagedMotionObserverForTesting(
		[&](float InitialYaw, float InitialFallRate)
		{
			if (FaultFallRate == 0.0f)
			{
				FaultYaw = InitialYaw;
				FaultFallRate = InitialFallRate;
			}
		});
	FMath::RandInit(0x5241494e);
	const int32 ExpectedNextGlobalRandom = FMath::Rand();
	FMath::RandInit(0x5241494e);
	auto FaultedRainStage = Materializer.Stage(*Good.World, Good.Descriptor,
		*Good.WorldLease, *Good.Payload, *FontOwner, ProductionPreferences,
		[]() { return false; });
	TestFalse(TEXT("deterministic-rain fault fixture stages"),
		FaultedRainStage.IsError());
	FWoTFrontEndSceneMaterializer::FailAfterHiddenConstructionForTesting();
	if (!FaultedRainStage.IsError())
		TestTrue(TEXT("hidden-construction rain fault rejects commit"),
			MoveTemp(FaultedRainStage).GetValue().Commit().IsError());
	auto CancelledRainStage = Materializer.Stage(*Good.World, Good.Descriptor,
		*Good.WorldLease, *Good.Payload, *FontOwner, ProductionPreferences,
		[]() { return false; });
	TestFalse(TEXT("deterministic-rain cancellation fixture stages"),
		CancelledRainStage.IsError());
	FWoTFrontEndSceneMaterializer::CancelNextCommitForTesting();
	if (!CancelledRainStage.IsError())
		TestTrue(TEXT("cancelled rain attempt rejects commit"),
			MoveTemp(CancelledRainStage).GetValue().Commit().IsError());
	auto RetriedRainStage = Materializer.Stage(*Good.World, Good.Descriptor,
		*Good.WorldLease, *Good.Payload, *FontOwner, ProductionPreferences,
		[]() { return false; });
	TestFalse(TEXT("deterministic-rain retry stages"), RetriedRainStage.IsError());
	if (!RetriedRainStage.IsError())
		TestFalse(TEXT("deterministic-rain retry commits"),
			MoveTemp(RetriedRainStage).GetValue().Commit().IsError());
	const FWoTFrontEndSceneObservation RetriedRain =
		Materializer.ObserveForTesting();
	TestEqual(TEXT("fault and retry preserve the staged RainCyd yaw"),
		RetriedRain.FirstRainInitialYaw, FaultYaw);
	TestEqual(TEXT("fault and retry preserve the staged RainCyd fall rate"),
		RetriedRain.FirstRainFallRate, FaultFallRate);
	TestEqual(TEXT("hidden fault, cancellation, and retry do not advance global RNG"),
		FMath::Rand(), ExpectedNextGlobalRandom);
	WoTSetRainCydStagedMotionObserverForTesting({});
	Materializer.Shutdown();
	WoTFrontEndTest_DestroyProductionStageFixture(Good);

	FWoTFrontEndStageProductionFixture ClassDefaultSkin;
	Error.Empty();
	if (!TestTrue(TEXT("authenticated class-default RainCyd fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(ClassDefaultSkin, false, Error,
			false, false, false, false, false, {},
			EWoTFrontEndRainSkinFixture::ClassDefaultOneSection)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}
	FontOwner = ClassDefaultSkin.Payload->FrontEndFontOwnerForScene();
	FWoTFrontEndSceneMaterializer ClassDefaultSkinMaterializer;
	auto ClassDefaultSkinStage = FontOwner && ClassDefaultSkin.WorldLease.IsValid()
		? ClassDefaultSkinMaterializer.Stage(*ClassDefaultSkin.World,
			ClassDefaultSkin.Descriptor, *ClassDefaultSkin.WorldLease,
			*ClassDefaultSkin.Payload, *FontOwner, {}, []() { return false; })
		: TWoTBootstrapResult<FWoTFrontEndSceneStaging>::Error(
			MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
				TEXT("class-default RainCyd fixture prerequisites are absent")));
	if (!TestFalse(
		TEXT("actual Stage resolves every RainCyd section to the authenticated class-default skin"),
		ClassDefaultSkinStage.IsError()) && ClassDefaultSkinStage.IsError())
		AddError(ClassDefaultSkinStage.GetError().ToString());
	ClassDefaultSkinMaterializer.Rollback();
	WoTFrontEndTest_DestroyProductionStageFixture(ClassDefaultSkin);

	struct FRainSkinAttack
	{
		EWoTFrontEndRainSkinFixture Fixture;
		const TCHAR* Label;
	};
	for (const FRainSkinAttack& Attack : {
		FRainSkinAttack{EWoTFrontEndRainSkinFixture::ClassDefaultMultiSlot,
			TEXT("multiple semantic slots")},
		FRainSkinAttack{EWoTFrontEndRainSkinFixture::ClassDefaultWrongIndex,
			TEXT("out-of-range section skin index")},
		FRainSkinAttack{EWoTFrontEndRainSkinFixture::ClassDefaultWrongIdentity,
			TEXT("wrong mesh source identity")},
		FRainSkinAttack{EWoTFrontEndRainSkinFixture::ClassDefaultExtraSection,
			TEXT("extra mesh section")}})
	{
		FWoTFrontEndStageProductionFixture HostileSkin;
		Error.Empty();
		if (!TestTrue(TEXT("hostile RainCyd skin fixture activates"),
			WoTFrontEndTest_BuildProductionStageFixture(HostileSkin, false, Error,
				false, false, false, false, false, {}, Attack.Fixture)))
		{
			if (!Error.IsEmpty()) AddError(Error);
			return false;
		}
		FontOwner = HostileSkin.Payload->FrontEndFontOwnerForScene();
		FWoTFrontEndSceneMaterializer HostileSkinMaterializer;
		auto HostileSkinStage = FontOwner && HostileSkin.WorldLease.IsValid()
			? HostileSkinMaterializer.Stage(*HostileSkin.World,
				HostileSkin.Descriptor, *HostileSkin.WorldLease,
				*HostileSkin.Payload, *FontOwner, {}, []() { return false; })
			: TWoTBootstrapResult<FWoTFrontEndSceneStaging>::Error(
				MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
					TEXT("hostile RainCyd skin fixture prerequisites are absent")));
		TestTrue(FString::Printf(TEXT("actual Stage rejects RainCyd %s"),
			Attack.Label), HostileSkinStage.IsError());
		TestTrue(TEXT("hostile RainCyd skin leaves zero world state"),
			IsZeroScene(HostileSkinMaterializer.ObserveForTesting()));
		HostileSkinMaterializer.Rollback();
		WoTFrontEndTest_DestroyProductionStageFixture(HostileSkin);
	}

	FWoTFrontEndStageProductionFixture ConverterSlot;
	Error.Empty();
	if (!TestTrue(TEXT("converter-slot fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(ConverterSlot, false, Error,
			false, false, false, false, false,
			TEXT("Environment__RainDropsA_T"),
			EWoTFrontEndRainSkinFixture::MeshLocal, false, true)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}
	FontOwner = ConverterSlot.Payload->FrontEndFontOwnerForScene();
	FWoTFrontEndSceneMaterializer ConverterSlotMaterializer;
	auto ConverterSlotStage = FontOwner && ConverterSlot.WorldLease.IsValid()
		? ConverterSlotMaterializer.Stage(*ConverterSlot.World,
			ConverterSlot.Descriptor, *ConverterSlot.WorldLease,
			*ConverterSlot.Payload, *FontOwner, {}, []() { return false; })
		: TWoTBootstrapResult<FWoTFrontEndSceneStaging>::Error(
			MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
				TEXT("converter-slot fixture prerequisites are absent")));
	if (!TestFalse(
		TEXT("actual Stage binds the authenticated converter slot to its source"),
		ConverterSlotStage.IsError()) && ConverterSlotStage.IsError())
	{
		AddError(ConverterSlotStage.GetError().ToString());
	}
	if (!ConverterSlotStage.IsError())
	{
		FWoTBootstrapStatus ConverterCommit =
			MoveTemp(ConverterSlotStage).GetValue().Commit();
		if (!TestFalse(TEXT("converter-slot production plan commits"),
			ConverterCommit.IsError()) && ConverterCommit.IsError())
			AddError(ConverterCommit.GetError().ToString());
	}
	VerifyCommittedLightmap(*ConverterSlot.World,
		TEXT("opaque and water converter-slot commit"));
	UStaticMeshComponent* ConverterMesh =
		FindCommittedBSPMesh(*ConverterSlot.World);
	int32 RawLinearTextures = 0;
	int32 PreservedSRGBTextures = 0;
	for (int32 Slot = 0; ConverterMesh != nullptr
		&& Slot < ConverterMesh->GetNumMaterials(); ++Slot)
	{
		UMaterialInstanceDynamic* MID =
			Cast<UMaterialInstanceDynamic>(ConverterMesh->GetMaterial(Slot));
		UTexture* Bound = nullptr;
		if (MID == nullptr || !MID->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), Bound)) continue;
		const UTexture2D* Texture = Cast<UTexture2D>(Bound);
		RawLinearTextures += Texture != nullptr && !Texture->SRGB ? 1 : 0;
		PreservedSRGBTextures += Texture != nullptr && Texture->SRGB ? 1 : 0;
	}
	TestEqual(TEXT("authenticated procedural and literal world textures remain raw linear"),
		RawLinearTextures, 2);
	TestEqual(TEXT("both actual world-surface receivers use world texture policy"),
		PreservedSRGBTextures, 0);
	ConverterSlotMaterializer.Shutdown();
	WoTFrontEndTest_DestroyProductionStageFixture(ConverterSlot);
	for (const TCHAR* RejectedSlot : {
		TEXT("WarrenT__rain2_2S"), TEXT("WarrenT__Rain2_SL_M"),
		TEXT("WarrenT__Rain2_2S_2S"), TEXT("WarrenT__Rain2_Extra")})
	{
		FWoTFrontEndStageProductionFixture HostileSlot;
		Error.Empty();
		if (!TestTrue(TEXT("hostile converter-slot fixture activates"),
			WoTFrontEndTest_BuildProductionStageFixture(HostileSlot, false, Error,
				false, false, false, false, false, RejectedSlot)))
		{
			if (!Error.IsEmpty()) AddError(Error);
			return false;
		}
		FontOwner = HostileSlot.Payload->FrontEndFontOwnerForScene();
		FWoTFrontEndSceneMaterializer HostileSlotMaterializer;
		auto HostileStage = FontOwner && HostileSlot.WorldLease.IsValid()
			? HostileSlotMaterializer.Stage(*HostileSlot.World,
				HostileSlot.Descriptor, *HostileSlot.WorldLease,
				*HostileSlot.Payload, *FontOwner, {}, []() { return false; })
			: TWoTBootstrapResult<FWoTFrontEndSceneStaging>::Error(
				MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
					TEXT("hostile converter-slot fixture prerequisites are absent")));
		TestTrue(FString::Printf(
			TEXT("actual Stage rejects hostile converter slot: %s"), RejectedSlot),
			HostileStage.IsError());
		TestTrue(TEXT("hostile converter slot leaves zero world state"),
			IsZeroScene(HostileSlotMaterializer.ObserveForTesting()));
		HostileSlotMaterializer.Rollback();
		WoTFrontEndTest_DestroyProductionStageFixture(HostileSlot);
	}

	FWoTFrontEndStageProductionFixture Corrupt;
	Error.Empty();
	if (!TestTrue(TEXT("attested object-corruption fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(Corrupt, true, Error)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}
	FontOwner = Corrupt.Payload->FrontEndFontOwnerForScene();
	FWoTFrontEndSceneMaterializer Rejected;
	auto CorruptStage = FontOwner && Corrupt.WorldLease.IsValid()
		? Rejected.Stage(*Corrupt.World, Corrupt.Descriptor, *Corrupt.WorldLease,
			*Corrupt.Payload, *FontOwner, {}, []() { return false; })
		: TWoTBootstrapResult<FWoTFrontEndSceneStaging>::Error(
			MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
				TEXT("corrupt production fixture prerequisites are absent")));
	TestTrue(TEXT("actual production Stage rejects corrupt retained BSP"),
		CorruptStage.IsError());
	TestTrue(TEXT("production object corruption leaves zero world state"),
		IsZeroScene(Rejected.ObserveForTesting()));
	WoTFrontEndTest_DestroyProductionStageFixture(Corrupt);

	FWoTFrontEndStageProductionFixture MissingLightmap;
	Error.Empty();
	if (!TestTrue(TEXT("missing-lightmap fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(MissingLightmap, false, Error,
			false, false, false, false, false, {},
			EWoTFrontEndRainSkinFixture::MeshLocal, true)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}
	FontOwner = MissingLightmap.Payload->FrontEndFontOwnerForScene();
	FWoTFrontEndSceneMaterializer MissingLightmapRejected;
	auto MissingLightmapStage = FontOwner && MissingLightmap.WorldLease.IsValid()
		? MissingLightmapRejected.Stage(*MissingLightmap.World,
			MissingLightmap.Descriptor, *MissingLightmap.WorldLease,
			*MissingLightmap.Payload, *FontOwner, {}, []() { return false; })
		: TWoTBootstrapResult<FWoTFrontEndSceneStaging>::Error(
			MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
				TEXT("missing-lightmap fixture prerequisites are absent")));
	TestTrue(TEXT("required entry staging rejects a version-1 BSP without atlas"),
		MissingLightmapStage.IsError());
	TestTrue(TEXT("missing entry atlas leaves zero world state"),
		IsZeroScene(MissingLightmapRejected.ObserveForTesting()));
	WoTFrontEndTest_DestroyProductionStageFixture(MissingLightmap);

	for (int32 HostileIndex = 0; HostileIndex < 5; ++HostileIndex)
	{
		const bool bDuplicateRoute = HostileIndex == 0;
		const bool bThunderGap = HostileIndex == 1;
		const bool bThunderMismatch = HostileIndex == 2;
		const bool bThunderRootMismatch = HostileIndex == 3;
		const bool bThunderCaseMismatch = HostileIndex == 4;
		FWoTFrontEndStageProductionFixture MalformedRoute;
		Error.Empty();
		if (!TestTrue(TEXT("malformed lightning fixture activates"),
			WoTFrontEndTest_BuildProductionStageFixture(MalformedRoute, false,
				Error, bDuplicateRoute, bThunderGap, bThunderMismatch,
				bThunderRootMismatch, bThunderCaseMismatch)))
		{
			if (!Error.IsEmpty()) AddError(Error);
			return false;
		}
		FontOwner = MalformedRoute.Payload->FrontEndFontOwnerForScene();
		FWoTFrontEndSceneMaterializer RouteRejected;
		auto RouteStage = RouteRejected.Stage(*MalformedRoute.World,
			MalformedRoute.Descriptor, *MalformedRoute.WorldLease,
			*MalformedRoute.Payload, *FontOwner, {}, []() { return false; });
		TestTrue(bDuplicateRoute
			? TEXT("actual Stage rejects duplicate endpoint-to-light route")
			: bThunderGap
				? TEXT("actual Stage rejects gapped thunder ordering")
				: bThunderMismatch
					? TEXT("actual Stage rejects thunder override mismatch")
					: bThunderRootMismatch
						? TEXT("actual Stage rejects thunder source-root mismatch")
						: TEXT("actual Stage rejects thunder source-case mismatch"),
			RouteStage.IsError());
		TestTrue(TEXT("malformed lightning route leaves zero world state"),
			IsZeroScene(RouteRejected.ObserveForTesting()));
		WoTFrontEndTest_DestroyProductionStageFixture(MalformedRoute);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneAtomicCommitSpec,
	"WoT.FrontEnd.Scene.TransactionalAtomicCommit", WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneAtomicCommitSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TestNotNull(TEXT("spec world"), World);
	if (World == nullptr) return false;
	const IConsoleVariable* DelayedSpawn = IConsoleManager::Get().FindConsoleVariable(
		TEXT("s.DelayOnActorSpawnedUntilFinishedSpawning"));
	TestTrue(TEXT("publication contract requires finished-spawn notification"),
		DelayedSpawn != nullptr && DelayedSpawn->GetInt() != 0);

	FWoTFrontEndSceneMaterializer Materializer;
	auto Staged = Materializer.StageForTesting(
		*World, MakeSceneDescriptor(), MakeSceneFixture(), {});
	TestFalse(TEXT("valid synthetic scene stages"), Staged.IsError());
	TestTrue(TEXT("stage publishes no actor, component, tick, collision, or audio"),
		IsZeroScene(Materializer.ObserveForTesting()));
	if (!Staged.IsError())
	{
		const FWoTFrontEndSceneStagedCensus Census =
			Staged.GetValue().StagedCensusForTesting();
		TestEqual(TEXT("all BSP surfaces are staged"), Census.BSPSurfaces, 8u);
		TestEqual(TEXT("all lights are staged"), Census.Lights, 39u);
		TestEqual(TEXT("all ambience is staged"), Census.AmbientSounds, 5u);
		TestEqual(TEXT("all splashes are staged"), Census.SplashEmitters, 4u);
		TestEqual(TEXT("all rain cylinders are staged"), Census.RainCylinders, 6u);
		TestEqual(TEXT("all lightning actors are staged"), Census.LightningActors, 5u);
		TestEqual(TEXT("one lightning controller is staged"),
			Census.RandomLightningControllers, 1u);

		FWoTBootstrapStatus Committed = MoveTemp(Staged).GetValue().Commit();
		TestFalse(TEXT("complete stage commits"), Committed.IsError());
		const FWoTFrontEndSceneObservation Seen = Materializer.ObserveForTesting();
		TestEqual(TEXT("commit atomically exposes root plus authored rain/splash actors"),
			Seen.ObservableActors, 11u);
		TestTrue(TEXT("commit exposes real registered render/audio/light components"),
			Seen.VisibleComponents >= 45u);
		TestEqual(TEXT("controller plus authored weather tick only after commit"),
			Seen.TickingActors, 11u);
		TestEqual(TEXT("front-end scene never enables collision"),
			Seen.CollidingComponents, 0u);
		TestEqual(TEXT("all positional ambience starts together"),
			Seen.AudibleComponents, 5u);
		TestEqual(TEXT("every RainCyd has authenticated runtime topology"),
			Seen.AuthenticatedRainRenderers, 6u);
		TestEqual(TEXT("every splash has authored templates and material arrays"),
			Seen.AuthoredSplashRenderers, 4u);
		TestEqual(TEXT("authored water is bound to a BSP material section"),
			Seen.BoundWaterMaterials, 1u);
	}

	Materializer.Rollback();
	TestTrue(TEXT("rollback removes every committed observable"),
		IsZeroScene(Materializer.ObserveForTesting()));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneConverterSurfaceSlotSpec,
	"WoT.FrontEnd.Scene.AuthenticatedConverterSurfaceSlots",
	WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneConverterSurfaceSlotSpec::RunTest(const FString&)
{
	const TArray<FString> ProductionShaped = {
		TEXT("ForsakenT.DetailArchitect.MarbBalc2720"),
		TEXT("MordethT.DetailArchitect.MetlGril4002v02"),
		TEXT("MordethT.SurfaceFloor.StonFlor4900"),
		TEXT("WarrenT.Rain2")};
	auto Resolve = [&ProductionShaped](const TCHAR* Slot)
	{
		return FWoTFrontEndSceneMaterializer::
			ResolveConverterSurfaceSlotForTesting(Slot, ProductionShaped);
	};
	TestEqual(TEXT("grouped texture resolves through exact package and leaf"),
		Resolve(TEXT("ForsakenT__MarbBalc2720")), 0);
	TestEqual(TEXT("ordered masked and special-lit flags preserve authority"),
		Resolve(TEXT("MordethT__MetlGril4002v02_M_SL")), 1);
	TestEqual(TEXT("plain repeated identity resolves"),
		Resolve(TEXT("MordethT__StonFlor4900")), 2);
	TestEqual(TEXT("differently flagged repeated identity resolves"),
		Resolve(TEXT("MordethT__StonFlor4900_SL")), 2);
	TestEqual(TEXT("procedural identity resolves with two-sided flag"),
		Resolve(TEXT("WarrenT__Rain2_2S")), 3);
	TestEqual(TEXT("complete ordered converter grammar resolves"),
		FWoTFrontEndSceneMaterializer::ResolveConverterSurfaceSlotForTesting(
			TEXT("Pkg__Leaf_M_T_Mod_2S_U_Sky_SL__SkyZone"),
			TArray<FString>{TEXT("Pkg.Group.Leaf")}), 0);

	for (const TCHAR* Rejected : {
		TEXT("Unknown__Rain2"),
		TEXT("WarrenT__Rain2_SL_M"),
		TEXT("WarrenT__Rain2_2S_2S"),
		TEXT("WarrenT__Rain2_Extra"),
		TEXT("WarrenT__Rain20"),
		TEXT("WarrenT__rain2_2S"),
		TEXT("WarrenT__Rain2__SkyZone__SkyZone")})
	{
		TestEqual(FString::Printf(TEXT("hostile slot is rejected: %s"), Rejected),
			Resolve(Rejected), INDEX_NONE);
	}
	TestEqual(TEXT("two authenticated groups with one converter base are ambiguous"),
		FWoTFrontEndSceneMaterializer::ResolveConverterSurfaceSlotForTesting(
			TEXT("Pkg__Leaf"),
			TArray<FString>{TEXT("Pkg.GroupA.Leaf"), TEXT("Pkg.GroupB.Leaf")}),
		INDEX_NONE);
	TestEqual(TEXT("longest authenticated leaf outranks converter flag grammar"),
		FWoTFrontEndSceneMaterializer::ResolveConverterSurfaceSlotForTesting(
			TEXT("Pkg__Leaf_M"),
			TArray<FString>{TEXT("Pkg.Group.Leaf"), TEXT("Pkg.Group.Leaf_M")}),
		1);
	for (const FString& Malformed : {
		FString(TEXT("NoDot")), FString(TEXT(".Leaf")), FString(TEXT("Pkg.")),
		FString(TEXT("Pkg..Leaf")), FString(TEXT("Pkg.Group.Bad__Leaf"))})
	{
		TestEqual(FString::Printf(TEXT("malformed source is rejected: %s"), *Malformed),
			FWoTFrontEndSceneMaterializer::ResolveConverterSurfaceSlotForTesting(
				TEXT("Pkg__Leaf"), TArray<FString>{Malformed}), INDEX_NONE);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneAsyncTravelFailureSpec,
	"WoT.FrontEnd.Scene.AsyncTravelFailureRematerializes",
	WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneAsyncTravelFailureSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	UWoTBootstrapSubsystem::SetCreationEnabledForTesting(true);
	FWoTFrontEndStageProductionFixture Fixture;
	FString Error;
	if (!TestTrue(TEXT("async-travel fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(Fixture, false, Error)))
	{
		UWoTBootstrapSubsystem::SetCreationEnabledForTesting(false);
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}
	UWoTBootstrapSubsystem* Bootstrap = Fixture.World->GetGameInstance()
		? Fixture.World->GetGameInstance()->GetSubsystem<UWoTBootstrapSubsystem>()
		: nullptr;
	UWoTSessionSubsystem* Sessions = Fixture.World->GetGameInstance()
		? Fixture.World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>()
		: nullptr;
	TestNotNull(TEXT("bootstrap owns async-travel recovery"), Bootstrap);
	TestNotNull(TEXT("session installed travel-failure subscription"), Sessions);
	if (Sessions != nullptr)
		TestTrue(TEXT("session adopts fixture world authority"),
			Sessions->ActivateForAdoptedWorldAuthority());
	TUniquePtr<FWoTFrontEndSceneMaterializer> Scene =
		MakeUnique<FWoTFrontEndSceneMaterializer>();
	auto Staged = Scene->Stage(*Fixture.World, Fixture.Descriptor,
		*Fixture.WorldLease, *Fixture.Payload,
		*Fixture.Payload->FrontEndFontOwnerForScene(), {},
		[]() { return false; });
	TestFalse(TEXT("async-travel scene stages"), Staged.IsError());
	if (!Staged.IsError())
		TestFalse(TEXT("async-travel scene first commits"),
			MoveTemp(Staged).GetValue().Commit().IsError());
	if (Bootstrap != nullptr)
	{
		Bootstrap->AdoptFrontEndSceneForTesting(MoveTemp(Scene));
		Bootstrap->SuspendFrontEndSceneBeforeTravel();
		TestFalse(TEXT("host travel suspension makes scene inert"),
			Bootstrap->IsFrontEndSceneCommitted());
		GEngine->OnTravelFailure().Broadcast(Fixture.World,
			ETravelFailure::ServerTravelFailure,
			TEXT("synthetic asynchronous host travel failure"));
		TestTrue(TEXT("owned asynchronous failure rematerializes immediately"),
			Bootstrap->IsFrontEndSceneCommitted());
	}
	WoTFrontEndTest_DestroyProductionStageFixture(Fixture);
	UWoTBootstrapSubsystem::SetCreationEnabledForTesting(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneFaultRollbackSpec,
	"WoT.FrontEnd.Scene.FaultRollback", WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneFaultRollbackSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TestNotNull(TEXT("spec world"), World);
	if (World == nullptr) return false;

	struct FFaultCase
	{
		const TCHAR* Name;
		TFunction<void(FWoTFrontEndSceneTestFixture&)> Mutate;
	};
	const FFaultCase Cases[] = {
		{TEXT("malformed BSP"), [](auto& F) { F.bMalformedBSP = true; }},
		{TEXT("missing surface"), [](auto& F) { F.bMissingSurface = true; }},
		{TEXT("corrupt sound"), [](auto& F) { F.bCorruptSound = true; }},
		{TEXT("corrupt font"), [](auto& F) { F.bCorruptFont = true; }},
		{TEXT("cancelled"), [](auto& F) { F.bCancelled = true; }},
		{TEXT("stale font generation"),
			[](auto& F) { F.FontSnapshotIdentity = TEXT("generation-b"); }},
	};

	for (const FFaultCase& Case : Cases)
	{
		FWoTFrontEndSceneMaterializer Materializer;
		FWoTFrontEndSceneTestFixture Fixture = MakeSceneFixture();
		Case.Mutate(Fixture);
		auto Result = Materializer.StageForTesting(
			*World, MakeSceneDescriptor(), Fixture, {});
		TestTrue(Case.Name, Result.IsError());
		TestTrue(*FString::Printf(TEXT("%s leaves zero scene state"), Case.Name),
			IsZeroScene(Materializer.ObserveForTesting()));
	}

	FWoTFrontEndSceneMaterializer PreCommit;
	auto Staged = PreCommit.StageForTesting(
		*World, MakeSceneDescriptor(), MakeSceneFixture(), {});
	TestFalse(TEXT("precommit fixture stages"), Staged.IsError());
	FWoTFrontEndSceneMaterializer::FailNextCommitForTesting();
	if (!Staged.IsError())
	{
		TestTrue(TEXT("immediate precommit fault is rejected"),
			MoveTemp(Staged).GetValue().Commit().IsError());
	}
	TestTrue(TEXT("immediate precommit fault rolls back zero scene state"),
		IsZeroScene(PreCommit.ObserveForTesting()));

	FWoTFrontEndSceneMaterializer MidPublication;
	auto HiddenStage = MidPublication.StageForTesting(
		*World, MakeSceneDescriptor(), MakeSceneFixture(), {});
	TestFalse(TEXT("hidden-construction fixture stages"), HiddenStage.IsError());
	FWoTFrontEndSceneMaterializer::FailAfterHiddenConstructionForTesting();
	if (!HiddenStage.IsError())
	{
		TestTrue(TEXT("failure after hidden construction is rejected"),
			MoveTemp(HiddenStage).GetValue().Commit().IsError());
	}
	TestTrue(TEXT("hidden-construction fault leaves no tagged world actor/component"),
		IsZeroScene(MidPublication.ObserveForTesting()));

	FWoTFrontEndSceneMaterializer PreparedController;
	auto PreparedStage = PreparedController.StageForTesting(
		*World, MakeSceneDescriptor(), MakeSceneFixture(), {});
	TestFalse(TEXT("inert-controller fixture stages"), PreparedStage.IsError());
	FWoTFrontEndSceneMaterializer::FailAfterControllerPreparationForTesting();
	if (!PreparedStage.IsError())
	{
		TestTrue(TEXT("failure after controller preparation is rejected"),
			MoveTemp(PreparedStage).GetValue().Commit().IsError());
	}
	TestTrue(TEXT("prepared controller publishes neither tick nor audio"),
		IsZeroScene(PreparedController.ObserveForTesting()));

	FWoTFrontEndSceneMaterializer PreCommitCancel;
	auto CancelledStage = PreCommitCancel.StageForTesting(
		*World, MakeSceneDescriptor(), MakeSceneFixture(), {});
	TestFalse(TEXT("precommit cancellation fixture stages"),
		CancelledStage.IsError());
	FWoTFrontEndSceneMaterializer::CancelNextCommitForTesting();
	if (!CancelledStage.IsError())
	{
		TestTrue(TEXT("cancellation immediately before commit is rejected"),
			MoveTemp(CancelledStage).GetValue().Commit().IsError());
	}
	TestTrue(TEXT("immediate precommit cancellation rolls back zero scene state"),
		IsZeroScene(PreCommitCancel.ObserveForTesting()));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndScenePublicationObserverSpec,
	"WoT.FrontEnd.Scene.PublicationBoundaryObservers", WoTFrontEndSceneFlags)

bool FWoTFrontEndScenePublicationObserverSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TestNotNull(TEXT("spec world"), World);
	if (World == nullptr) return false;
	int32 RegistrationCallbacks = 0;
	int32 SpawnCallbacks = 0;
	int32 ActualBeginPlayCallbacks = 0;
	int32 ActualOnRegisterCallbacks = 0;
	bool bEveryBoundaryInert = true;
	WoTSetParticleSprayerLifecycleObserverForTesting(
		[&](const TCHAR* Boundary, const AActor& Actor,
			const UActorComponent* Component)
		{
			const bool bBeginPlay = FCString::Strcmp(Boundary, TEXT("BeginPlay")) == 0;
			ActualBeginPlayCallbacks += bBeginPlay ? 1 : 0;
			ActualOnRegisterCallbacks += bBeginPlay ? 0 : 1;
			bEveryBoundaryInert &=
				FWoTFrontEndSceneMaterializer::IsPublicationGuardActiveForTesting()
				&& Actor.IsHidden() && !Actor.IsActorTickEnabled();
			if (const UPrimitiveComponent* Primitive =
				Cast<UPrimitiveComponent>(Component))
				bEveryBoundaryInert &= !Primitive->IsRegistered()
					&& Primitive->GetCollisionEnabled()
						== ECollisionEnabled::NoCollision;
		});
	FWoTFrontEndSceneMaterializer::SetPublicationBoundaryObserverForTesting(
		[&](const TCHAR*, const FWoTFrontEndSceneObservation& Seen)
		{
			++RegistrationCallbacks;
			bEveryBoundaryInert &=
				FWoTFrontEndSceneMaterializer::IsPublicationGuardActiveForTesting()
				&& Seen.ObservableActors == 0 && Seen.TickingActors == 0
				&& Seen.CollidingComponents == 0 && Seen.AudibleComponents == 0;
		});
	const FDelegateHandle SpawnHandle = World->AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor)
		{
			if (Actor == nullptr
				|| !Actor->ActorHasTag(TEXT("WoTAuthenticatedFrontEndScene"))) return;
			++SpawnCallbacks;
			bEveryBoundaryInert &=
				FWoTFrontEndSceneMaterializer::IsPublicationGuardActiveForTesting()
				&& Actor->IsHidden() && !Actor->IsActorTickEnabled();
			TInlineComponentArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (const UActorComponent* Component : Components)
			{
				if (const UPrimitiveComponent* Primitive =
					Cast<UPrimitiveComponent>(Component))
					bEveryBoundaryInert &= Primitive->GetCollisionEnabled()
						== ECollisionEnabled::NoCollision;
				if (const UAudioComponent* Audio = Cast<UAudioComponent>(Component))
					bEveryBoundaryInert &= !Audio->IsPlaying();
			}
		}));

	FWoTFrontEndSceneMaterializer Materializer;
	auto Staged = Materializer.StageForTesting(
		*World, MakeSceneDescriptor(), MakeSceneFixture(), {});
	TestFalse(TEXT("observer fixture stages"), Staged.IsError());
	if (!Staged.IsError())
		TestFalse(TEXT("observer fixture commits"),
			MoveTemp(Staged).GetValue().Commit().IsError());
	TestTrue(TEXT("component registration and BeginPlay boundaries were observed"),
		RegistrationCallbacks > 40 && SpawnCallbacks == 11
			&& ActualBeginPlayCallbacks == 4 && ActualOnRegisterCallbacks == 8);
	TestTrue(TEXT("every external callback sees only guarded inert state"),
		bEveryBoundaryInert);
	World->RemoveOnActorSpawnedHandler(SpawnHandle);
	FWoTFrontEndSceneMaterializer::SetPublicationBoundaryObserverForTesting({});
	WoTSetParticleSprayerLifecycleObserverForTesting({});
	Materializer.Rollback();
	TestTrue(TEXT("rollback census includes no pending-destroy object"),
		IsZeroScene(Materializer.ObserveForTesting()));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneAuthorityRevocationSpec,
	"WoT.FrontEnd.Scene.AuthorityRevocationBoundaries", WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneAuthorityRevocationSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 9);
	enum class ESeam : uint8
	{
		Hidden, Prepared, Publication, CommitProgress, PostCommit
	};
	for (const ESeam Seam : {ESeam::Hidden, ESeam::Prepared,
		ESeam::Publication, ESeam::CommitProgress, ESeam::PostCommit})
	{
		FWoTFrontEndStageProductionFixture Fixture;
		FString Error;
		if (!TestTrue(TEXT("production authority fixture activates"),
			WoTFrontEndTest_BuildProductionStageFixture(Fixture, false, Error)))
		{
			if (!Error.IsEmpty()) AddError(Error);
			return false;
		}
		const FWoTFrontEndFontOwner* FontOwner =
			Fixture.Payload->FrontEndFontOwnerForScene();
		FWoTFrontEndSceneMaterializer Materializer;
		APlayerController* ViewOwner = Fixture.World->SpawnActor<APlayerController>();
		AActor* PreviousView = Fixture.World->SpawnActor<AActor>();
		if (ViewOwner != nullptr && PreviousView != nullptr)
			ViewOwner->SetViewTarget(PreviousView);
		bool bReentrantCallbackPreservedAuthority = false;
		auto Staged = Materializer.Stage(*Fixture.World, Fixture.Descriptor,
			*Fixture.WorldLease, *Fixture.Payload, *FontOwner, {},
			[]() { return false; },
			[&](const FWoTFrontEndSceneProgress& Progress)
			{
				if (Seam == ESeam::CommitProgress
					&& Progress.Stage == EWoTFrontEndSceneProgressStage::Commit)
				{
					FWoTFrontEndSceneMaterializer::RevokeLeaseForTesting(
						*Fixture.WorldLease);
					const FWoTFrontEndSceneObservation DuringCallback =
						Materializer.ObserveForTesting();
					bReentrantCallbackPreservedAuthority =
						Fixture.WorldLease->IsValid()
						&& DuringCallback.ObservableActors > 0
						&& DuringCallback.AudibleComponents > 0;
				}
			});
		TestFalse(TEXT("production authority scene stages"), Staged.IsError());
		if (Staged.IsError())
		{
			AddError(Staged.GetError().ToString());
			WoTFrontEndTest_DestroyProductionStageFixture(Fixture);
			return false;
		}
		if (Seam == ESeam::Hidden)
			FWoTFrontEndSceneMaterializer::RevokeAfterHiddenConstructionForTesting();
		else if (Seam == ESeam::Prepared)
			FWoTFrontEndSceneMaterializer::RevokeAfterControllerPreparationForTesting();
		else if (Seam == ESeam::Publication)
			FWoTFrontEndSceneMaterializer::RevokeDuringPublicationForTesting();
		FWoTBootstrapStatus Committed =
			MoveTemp(Staged).GetValue().Commit(ViewOwner);
		if (Seam == ESeam::PostCommit)
		{
			TestFalse(TEXT("postcommit seam first commits"), Committed.IsError());
			Materializer.RevokeAuthorityForTesting();
			TestTrue(TEXT("postcommit revoke tears down before returning"),
				IsZeroScene(Materializer.ObserveForTesting()));
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		}
		else TestTrue(TEXT("revocation before return rejects commit"),
			Committed.IsError());
		if (Seam == ESeam::CommitProgress)
			TestTrue(TEXT("reentrant callback never observes invalid live scene"),
				bReentrantCallbackPreservedAuthority);
		TestTrue(TEXT("revocation leaves no view/tick/collision/audio authority"),
			IsZeroScene(Materializer.ObserveForTesting()));
		if (ViewOwner != nullptr && PreviousView != nullptr)
			TestEqual(TEXT("revocation restores the previous view before returning"),
				ViewOwner->GetViewTarget(), PreviousView);
		WoTFrontEndTest_DestroyProductionStageFixture(Fixture);
	}

	FWoTFrontEndStageProductionFixture RevokingRead;
	FString RevokingReadError;
	if (!TestTrue(TEXT("generic-read revocation fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(
			RevokingRead, false, RevokingReadError))) return false;
	FWoTFrontEndSceneMaterializer RevokingReadMaterializer;
	auto RevokingReadStage = RevokingReadMaterializer.Stage(*RevokingRead.World,
		RevokingRead.Descriptor, *RevokingRead.WorldLease, *RevokingRead.Payload,
		*RevokingRead.Payload->FrontEndFontOwnerForScene(), {},
		[]() { return false; });
	TestFalse(TEXT("generic-read revocation scene stages"),
		RevokingReadStage.IsError());
	const FWoTCacheLeaseManifestEntry* RevokingEntry =
		RevokingRead.WorldLease->FindManifestEntryForTesting(
			RevokingRead.Descriptor.BspLogicalId);
	if (!TestNotNull(TEXT("generic-read object is retained"), RevokingEntry))
		return false;
	FWoTCacheBlobDescriptor RevokingDescriptor{RevokingEntry->LogicalId,
		RevokingEntry->Schema, RevokingEntry->Size, RevokingEntry->Sha256};
	FEvent* RevocationStarted = FPlatformProcess::GetSynchEventFromPool(true);
	WoTSetCacheRevocationStartedObserverForTesting(
		[RevocationStarted]() { RevocationStarted->Trigger(); });
	int32 PhysicalReadsDuringRevocation = 0;
	TSharedPtr<uint8, ESPMode::ThreadSafe> ReadObserver =
		WoTObserveCacheSnapshotPhysicalReadsForTesting(
			[&](FStringView) { ++PhysicalReadsDuringRevocation; });
	auto RevocationFuture = Async(EAsyncExecution::Thread, [&]()
	{
		FWoTFrontEndSceneMaterializer::RevokeLeaseForTesting(
			*RevokingRead.WorldLease);
	});
	RevocationStarted->Wait();
	auto SecondRead = ReadVerifiedCacheBlob(*RevokingRead.WorldLease,
		RevokingDescriptor, FWoTCacheBlobReadLimits());
	auto DescriptorDuringRevocation =
		WoTDeriveFrontEndDescriptorFromActiveGeneration(*RevokingRead.WorldLease);
	auto SidecarDuringRevocation =
		WoTDeriveWorldDescriptorsFromActiveGeneration(*RevokingRead.WorldLease);
	FWoTFrontEndSceneMaterializer RetentionMaterializer;
	auto RetentionDuringRevocation = RetentionMaterializer.Stage(
		*RevokingRead.World, RevokingRead.Descriptor, *RevokingRead.WorldLease,
		*RevokingRead.Payload,
		*RevokingRead.Payload->FrontEndFontOwnerForScene(), {},
		[]() { return false; });
	TestTrue(TEXT("a second verified read is rejected during teardown"),
		SecondRead.IsError());
	TestTrue(TEXT("memoized descriptor access is rejected during teardown"),
		DescriptorDuringRevocation.IsError());
	TestTrue(TEXT("memoized sidecar access is rejected during teardown"),
		SidecarDuringRevocation.IsError());
	TestTrue(TEXT("snapshot retention is rejected during teardown"),
		RetentionDuringRevocation.IsError());
	TestEqual(TEXT("rejected teardown reads neither reopen nor repopulate bytes"),
		PhysicalReadsDuringRevocation, 0);
	ReadObserver.Reset();
	WoTSetCacheRevocationStartedObserverForTesting({});
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	RevocationFuture.Wait();
	FPlatformProcess::ReturnSynchEventToPool(RevocationStarted);
	TestTrue(TEXT("generic-read revocation tears down staged presentation"),
		IsZeroScene(RevokingReadMaterializer.ObserveForTesting()));
	WoTFrontEndTest_DestroyProductionStageFixture(RevokingRead);
	FWoTFrontEndStageProductionFixture CorruptRead;
	FString Error;
	if (!TestTrue(TEXT("concurrent-corruption fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(CorruptRead, false, Error)))
		return false;
	FWoTFrontEndSceneMaterializer CorruptReadMaterializer;
	auto CorruptReadStage = CorruptReadMaterializer.Stage(*CorruptRead.World,
		CorruptRead.Descriptor, *CorruptRead.WorldLease, *CorruptRead.Payload,
		*CorruptRead.Payload->FrontEndFontOwnerForScene(), {},
		[]() { return false; });
	TestFalse(TEXT("concurrent-corruption scene stages"), CorruptReadStage.IsError());
	AWoTFrontEndPlayerController* CorruptMenuController =
		CorruptRead.World->SpawnActor<AWoTFrontEndPlayerController>();
	TSharedPtr<FWoTFrontEndPresentationAuthority> CorruptPresentation =
		FWoTFrontEndPresentationAuthority::Create(CorruptMenuController);
	TSharedPtr<SWoTFrontEndRoot> CorruptRoot;
	if (TestTrue(TEXT("concurrent-corruption production presentation binds"),
		CorruptPresentation.IsValid() && CorruptPresentation->IsReady()))
	{
		CorruptRoot = SNew(SWoTFrontEndRoot)
			.PlayerController(CorruptMenuController)
			.PresentationAuthorityForTesting(CorruptPresentation);
	}
	const FString CorruptLogicalId = CorruptRead.Descriptor.BspLogicalId;
	const FWoTCacheLeaseManifestEntry* CorruptEntry =
		CorruptRead.WorldLease->FindManifestEntryForTesting(CorruptLogicalId);
	if (!TestNotNull(TEXT("concurrent-corruption object is retained"), CorruptEntry))
		return false;
	FWoTCacheBlobDescriptor CorruptDescriptor{CorruptEntry->LogicalId,
		CorruptEntry->Schema, CorruptEntry->Size, CorruptEntry->Sha256};
	const FTCHARToUTF8 LogicalUtf8(*CorruptLogicalId);
	const FString ObjectPath = FPaths::Combine(
		CorruptRead.WorldLease->GetCanonicalRoot(),
		TEXT("current"), TEXT("blobs"),
		Sha256(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(
			LogicalUtf8.Get()), LogicalUtf8.Length())).ToHex() + TEXT(".blob"));
	FEvent* ReaderHasLock = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* MutateNow = FPlatformProcess::GetSynchEventFromPool(true);
	TAtomic<bool> bMutationWritten{false};
	FWoTCacheBlobReadLimits CorruptLimits;
	CorruptLimits.ImmediatelyBeforeSingleReadForTesting = [&]()
	{
		ReaderHasLock->Trigger();
		MutateNow->Wait();
		TArray<uint8> Bytes;
		if (FFileHelper::LoadFileToArray(Bytes, *ObjectPath) && !Bytes.IsEmpty())
		{
			Bytes[0] ^= 0xff;
			bMutationWritten.Store(OverwriteHeldSceneObject(ObjectPath, Bytes));
		}
	};
	auto CorruptFuture = Async(EAsyncExecution::Thread, [&]()
		{ return ReadVerifiedCacheBlob(*CorruptRead.WorldLease,
			CorruptDescriptor, CorruptLimits); });
	ReaderHasLock->Wait();
	auto ReleaseMutation = Async(EAsyncExecution::Thread, [MutateNow]()
	{
		FPlatformProcess::Sleep(0.05f);
		MutateNow->Trigger();
	});
	FWoTBootstrapStatus CorruptCommit = CorruptReadStage.IsError()
		? FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation, TEXT("stage failed")))
		: MoveTemp(CorruptReadStage).GetValue().Commit();
	ReleaseMutation.Wait();
	for (int32 Spin = 0; Spin < 5000 && !CorruptFuture.IsReady(); ++Spin)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::Sleep(0.001f);
	}
	TestTrue(TEXT("corrupt read mutation reached the held object"),
		bMutationWritten.Load());
	TestTrue(TEXT("concurrent corrupt read prevents publication"),
		CorruptCommit.IsError());
	TestTrue(TEXT("corruption teardown completes without reader/GT lock cycle"),
		CorruptFuture.IsReady());
	if (CorruptFuture.IsReady())
		TestTrue(TEXT("corrupt verified read is rejected"),
			CorruptFuture.Get().IsError());
	TestTrue(TEXT("corruption race leaves immediate zero presentation"),
		IsZeroScene(CorruptReadMaterializer.ObserveForTesting()));
	TestFalse(TEXT("corrupt real lease invalidates menu authority synchronously"),
		CorruptPresentation.IsValid() && CorruptPresentation->IsReady());
	TestTrue(TEXT("corrupt real lease removes menu localization synchronously"),
		!CorruptPresentation.IsValid()
			|| CorruptPresentation->Text(
				FName(TEXT("menuMain.MenuList[2]"))).IsEmpty());
	TestTrue(TEXT("corrupt real lease collapses the captured root synchronously"),
		!CorruptRoot.IsValid()
			|| CorruptRoot->GetVisibility() == EVisibility::Collapsed);
	CorruptReadMaterializer.Shutdown();
	FPlatformProcess::ReturnSynchEventToPool(ReaderHasLock);
	FPlatformProcess::ReturnSynchEventToPool(MutateNow);
	WoTFrontEndTest_DestroyProductionStageFixture(CorruptRead);
	FWoTFrontEndStageProductionFixture FailedA;
	FWoTFrontEndStageProductionFixture LiveB;
	Error.Empty();
	if (!TestTrue(TEXT("failed-A fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(FailedA, false, Error)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}
	FWoTFrontEndSceneMaterializer SerialMaterializer;
	int32 CancellationReads = 0;
	auto FailedStage = SerialMaterializer.Stage(*FailedA.World,
		FailedA.Descriptor, *FailedA.WorldLease, *FailedA.Payload,
		*FailedA.Payload->FrontEndFontOwnerForScene(), {},
		[&CancellationReads]() { return ++CancellationReads > 1; });
	TestTrue(TEXT("A fails after installing its subscription"), FailedStage.IsError());
	TUniquePtr<FWoTCacheGenerationLease> StaleALease =
		MoveTemp(FailedA.WorldLease);
	WoTFrontEndTest_DestroyProductionStageFixture(FailedA);
	Error.Empty();
	if (!TestTrue(TEXT("live-B fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(LiveB, false, Error)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}
	auto LiveStage = SerialMaterializer.Stage(*LiveB.World, LiveB.Descriptor,
		*LiveB.WorldLease, *LiveB.Payload,
		*LiveB.Payload->FrontEndFontOwnerForScene(), {}, []() { return false; });
	TestFalse(TEXT("B stages after failed A"), LiveStage.IsError());
	FWoTFrontEndSceneMaterializer::RevokeLeaseForTesting(*StaleALease);
	if (!LiveStage.IsError())
	{
		TestEqual(TEXT("revoking failed A cannot consume staged B"),
			LiveStage.GetValue().StagedCensusForTesting().Lights, 39u);
		TestFalse(TEXT("B commits after stale A revocation"),
			MoveTemp(LiveStage).GetValue().Commit().IsError());
	}
	SerialMaterializer.Shutdown();
	StaleALease.Reset();
	WoTFrontEndTest_DestroyProductionStageFixture(LiveB);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneCameraWeatherLifecycleSpec,
	"WoT.FrontEnd.Scene.CameraWeatherAudioAndLifecycle", WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneCameraWeatherLifecycleSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TestNotNull(TEXT("spec world"), World);
	if (World == nullptr) return false;

	FWoTFrontEndScenePreferences Preferences;
	Preferences.AudioVolume = 0.0f;
	Preferences.bReducedMotion = true;
	FWoTFrontEndSceneMaterializer Materializer;
	const FWoTFrontEndDescriptor Descriptor = MakeSceneDescriptor();
	auto Staged = Materializer.StageForTesting(
		*World, Descriptor, MakeSceneFixture(), Preferences);
	TestFalse(TEXT("preference-gated fixture stages"), Staged.IsError());
	if (!Staged.IsError())
	{
		TestFalse(TEXT("preference-gated fixture commits"),
			MoveTemp(Staged).GetValue().Commit().IsError());
	}
	FWoTFrontEndSceneObservation Seen = Materializer.ObserveForTesting();
	TestEqual(TEXT("camera uses authenticated descriptor location"),
		Seen.CameraLocation, Descriptor.CameraLocation);
	TestEqual(TEXT("camera uses authenticated descriptor rotation"),
		Seen.CameraRotation, Descriptor.CameraRotation);
	TestEqual(TEXT("horizontal FOV is 90"), Seen.HorizontalFov, 90.0f);
	TestTrue(TEXT("widescreen expands horizontally without aspect bars"),
		Seen.bMaintainHorizontalFov && !Seen.bConstrainAspectRatio);
	TestEqual(TEXT("zero typed volume keeps ambience silent"),
		Seen.AudibleComponents, 0u);
	TestEqual(TEXT("reduced motion keeps rain and water animation unticked"),
		Seen.AnimatedWeatherComponents, 0u);
	TestEqual(TEXT("no music component exists"), Seen.MusicComponents, 0u);

	Materializer.SuspendBeforeTravel();
	TestTrue(TEXT("host/join suspension removes scene before travel"),
		IsZeroScene(Materializer.ObserveForTesting()));
	UWorld* ReplacementWorld = WoTTest::CreateSpecWorld();
	TestNotNull(TEXT("replacement front-end world exists"), ReplacementWorld);
	TestFalse(TEXT("leave rematerializes from the retained valid snapshot"),
		ReplacementWorld == nullptr
			|| Materializer.RematerializeForTesting(*ReplacementWorld).IsError());
	Seen = Materializer.ObserveForTesting();
	TestEqual(TEXT("rematerialization restores the complete actor census"),
		Seen.ObservableActors, 11u);
	TestEqual(TEXT("rematerialization preserves generation"),
		Seen.SnapshotIdentity, FString(TEXT("generation-a")));
	if (ReplacementWorld != nullptr)
	{
		WoTTest::DestroySpecWorld(ReplacementWorld);
		ReplacementWorld = nullptr;
	}
	TestTrue(TEXT("unexpected world cleanup stops scene presentation"),
		IsZeroScene(Materializer.ObserveForTesting()));
	UWorld* RecoveryWorld = WoTTest::CreateSpecWorld();
	TestFalse(TEXT("replacement/disconnect cleanup rematerializes"),
		RecoveryWorld == nullptr
			|| Materializer.RematerializeForTesting(*RecoveryWorld).IsError());

	Materializer.Shutdown();
	TestTrue(TEXT("shutdown deterministically leaves zero scene state"),
		IsZeroScene(Materializer.ObserveForTesting()));
	TestTrue(TEXT("shutdown releases retained generation"),
		Materializer.ObserveForTesting().SnapshotIdentity.IsEmpty());
	if (RecoveryWorld != nullptr) WoTTest::DestroySpecWorld(RecoveryWorld);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneStagingGenerationSpec,
	"WoT.FrontEnd.Scene.StagingGenerationSafety", WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneStagingGenerationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TestNotNull(TEXT("spec world"), World);
	if (World == nullptr) return false;
	FWoTFrontEndSceneMaterializer Materializer;
	auto AResult = Materializer.StageForTesting(
		*World, MakeSceneDescriptor(), MakeSceneFixture(), {});
	TestFalse(TEXT("generation A stages"), AResult.IsError());
	FWoTFrontEndSceneStaging A = MoveTemp(AResult).GetValue();
	FWoTFrontEndSceneTestFixture BFixture = MakeSceneFixture();
	BFixture.SnapshotIdentity = TEXT("generation-b");
	BFixture.FontSnapshotIdentity = TEXT("generation-b");
	auto BResult = Materializer.StageForTesting(
		*World, MakeSceneDescriptor(), BFixture, {});
	TestFalse(TEXT("generation B supersedes A"), BResult.IsError());
	A.Rollback();
	TestEqual(TEXT("stale A cannot roll back B"),
		BResult.GetValue().StagedCensusForTesting().Lights, 39u);
	if (!BResult.IsError()) TestFalse(TEXT("B remains committable"),
		MoveTemp(BResult).GetValue().Commit().IsError());
	Materializer.Rollback();
	auto WorkerResult = Materializer.StageForTesting(
		*World, MakeSceneDescriptor(), MakeSceneFixture(), {});
	TestFalse(TEXT("worker-rollback fixture stages"), WorkerResult.IsError());
	if (!WorkerResult.IsError())
	{
		TFuture<void> Worker = Async(EAsyncExecution::Thread,
			[Capability = MoveTemp(WorkerResult).GetValue()]() mutable
			{
				Capability.Rollback();
			});
		Worker.Wait();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		TestTrue(TEXT("worker capability destruction marshals rollback to game thread"),
			IsZeroScene(Materializer.ObserveForTesting())
				&& Materializer.ObserveForTesting().SnapshotIdentity.IsEmpty());
	}

	TOptional<FWoTFrontEndSceneStaging> Orphan;
	{
		auto* ShortLived = new FWoTFrontEndSceneMaterializer();
		auto Result = ShortLived->StageForTesting(
			*World, MakeSceneDescriptor(), MakeSceneFixture(), {});
		TestFalse(TEXT("short-lived owner stages"), Result.IsError());
		if (!Result.IsError()) Orphan.Emplace(MoveTemp(Result).GetValue());
		delete ShortLived;
	}
	Orphan.Reset();
	TestTrue(TEXT("destroyed owner leaves staging capability harmless"), true);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSceneProductionWorkerStageSpec,
	"WoT.FrontEnd.Scene.ProductionStageRejectsWorkerThread",
	WoTFrontEndSceneFlags)

bool FWoTFrontEndSceneProductionWorkerStageSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FString Error;
	if (!TestTrue(TEXT("worker-stage production fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(Fixture, false, Error)))
		return false;
	FWoTFrontEndSceneMaterializer Materializer;
	const FWoTFrontEndFontOwner* FontOwner =
		Fixture.Payload->FrontEndFontOwnerForScene();
	auto Worker = Async(EAsyncExecution::Thread, [&]()
	{
		return Materializer.Stage(*Fixture.World, Fixture.Descriptor,
			*Fixture.WorldLease, *Fixture.Payload, *FontOwner, {},
			[]() { return false; });
	});
	auto WorkerResult = Worker.Consume();
	TestTrue(TEXT("actual production Stage rejects a worker thread"),
		WorkerResult.IsError());
	TestTrue(TEXT("worker-thread rejection leaves materializer state untouched"),
		IsZeroScene(Materializer.ObserveForTesting())
			&& Fixture.WorldLease->IsValid());
	WoTFrontEndTest_DestroyProductionStageFixture(Fixture);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
