#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "WoTTestWorld.h"
#include "WoTPayloadActivationTestSupport.h"
#include "../Assets/WoTRuntimeAssets.h"
#include "../Inventory/WoTReturnToSenderReflector.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Projectiles/WoTSeekerProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTFractalSubsystem.h"
#include "../WoTGameInstance.h"
#include "../WoTMeshFX.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTSpriteBillboard.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "WoTRetailReader/FontProcedural.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#include "WoTRetailReader/TextureSound.h"

namespace
{
constexpr EAutomationTestFlags WoTSeekerVisualFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
const FName SeekerCoreQualifiedName(TEXT("Angreal.Seeker.SLightning01"));

UInstancedStaticMeshComponent* SeekerQuad(UWoTSpriteBillboardComponent* Sprite,
	const TCHAR* Pass)
{
	if (!Sprite || !Sprite->GetOwner()) return nullptr;
	TInlineComponentArray<UInstancedStaticMeshComponent*> Quads(Sprite->GetOwner());
	for (UInstancedStaticMeshComponent* Quad : Quads)
	{
		if (Quad->GetAttachParent() == Sprite
			&& Quad->GetName().EndsWith(FString(TEXT("_")) + Pass)) return Quad;
	}
	return nullptr;
}

UTexture* SeekerQuadTexture(UInstancedStaticMeshComponent* Quad)
{
	UTexture* Texture = nullptr;
	if (Quad && Quad->GetMaterial(0))
		Quad->GetMaterial(0)->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), Texture);
	return Texture;
}

// Capture authenticated importer output, never copy retail bytes into the spec.
class FSeekerVisualObjectWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("Seeker spec object overflow")));
		FWoTPayloadActivationTestObject& Object = Objects.AddDefaulted_GetRef();
		Object.LogicalId = FString(LogicalId);
		Object.Schema = FString(Schema);
		Object.Bytes.Append(Bytes.GetData(), Bytes.Num());
		return FWoTBootstrapStatus::Success();
	}
	TArray<FWoTPayloadActivationTestObject> Objects;
};

class FSeekerVisualProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSeekerSpriteCentersSpec,
	"WoT.Seeker.SpriteCenters", WoTSeekerVisualFlags)

bool FWoTSeekerSpriteCentersSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	AWoTSeekerProjectile* Seeker = World->SpawnActor<AWoTSeekerProjectile>(
		FVector(100, 200, 300), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("real Seeker"), Seeker)
		|| !TestNotNull(TEXT("real globe"), Seeker->Globe.Get())) return false;

	auto CheckCenters = [this, Seeker](const TCHAR* Phase)
	{
		for (UWoTSpriteBillboardComponent* Sprite :
			{Seeker->Sprite.Get(), Seeker->Globe->Sprite.Get()})
		{
			const bool bCore = Sprite == Seeker->Sprite;
			for (const TCHAR* Pass : {TEXT("Mod"), TEXT("Add")})
			{
				const FString Label = FString::Printf(TEXT("%s %s %s"),
					Phase, bCore ? TEXT("core") : TEXT("globe"), Pass);
				UInstancedStaticMeshComponent* Quad = SeekerQuad(Sprite, Pass);
				if (!TestNotNull(Label + TEXT(" quad"), Quad)) continue;
				FTransform Instance;
				if (!TestTrue(Label + TEXT(" world instance"),
					Quad->GetInstanceTransform(0, Instance, true))) continue;
				TestTrue(Label + TEXT(" centered on projectile"),
					Instance.GetLocation().Equals(Seeker->GetActorLocation(), 0.01f));
				// Retail 256x256 core and 128x128 globe at DrawScale .75,
				// measured from the rendered 100uu plane's world dimensions.
				const double ExpectedSize = bCore ? 256.0 : 96.0;
				TestTrue(Label + TEXT(" rendered width"),
					FMath::IsNearlyEqual(Instance.GetScale3D().X * 100.0, ExpectedSize, 0.01));
				TestTrue(Label + TEXT(" rendered height"),
					FMath::IsNearlyEqual(Instance.GetScale3D().Y * 100.0, ExpectedSize, 0.01));
			}
		}
	};
	WoTTest::TickWorld(World, 1);
	CheckCenters(TEXT("after movement"));
	TestTrue(TEXT("projectile actually moved"),
		Seeker->GetActorLocation().X > 100.0);
	WoTTest::TickWorld(World, 5);
	CheckCenters(TEXT("after continued flight"));
	Seeker->SetActorLocation(FVector(900, -600, 700), false, nullptr,
		ETeleportType::TeleportPhysics);
	// Refresh the draw transforms in the teleport's frame, before the globe's
	// independent actor tick can conceal a missing retail SetBase relationship.
	Seeker->Sprite->TickComponent(0, LEVELTICK_All, nullptr);
	Seeker->Globe->Sprite->TickComponent(0, LEVELTICK_All, nullptr);
	CheckCenters(TEXT("teleport frame"));
	WoTTest::TickWorld(World, 1);
	CheckCenters(TEXT("after teleport flight"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSeekerReflectedVisibleTexturesSpec,
	"WoT.Seeker.ReflectedVisibleTexturesAdvance", WoTSeekerVisualFlags)

bool FWoTSeekerReflectedVisibleTexturesSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	auto Contract = WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production contract loads"), Contract.IsError())) return false;
	// Import only this real projectile's procedural core and globe frame chain.
	Contract.GetValue().Facts.RemoveAll([](const FWoTRequiredFact& Fact)
	{
		return Fact.SourcePath != TEXT("System/Angreal.u")
			|| !((Fact.Domain == EWoTFactDomain::Procedural
					&& Fact.SourceExport == TEXT("Angreal.Seeker.SLightning01"))
				|| (Fact.Domain == EWoTFactDomain::Texture
					&& Fact.SourceExport.StartsWith(TEXT("Angreal.Seeker.SeekerGlobe.SG_A"))));
	});
	TArray<uint8> PackageBytes;
	if (!TestTrue(TEXT("retail Angreal package loads"), FFileHelper::LoadFileToArray(
		PackageBytes, *WoTReferencePath(TEXT("original/System/Angreal.u"))))) return false;
	FSeekerVisualObjectWriter Writer;
	FSeekerVisualProgress Progress;
	auto Procedurals = ImportFontProceduralPackageBytesForTesting(PackageBytes,
		TEXT("System/Angreal.u"), Contract.GetValue(), Writer, Progress);
	auto Textures = ImportTexturePackageBytesForTesting(PackageBytes,
		TEXT("System/Angreal.u"), Contract.GetValue(), Writer, Progress);
	if (!TestFalse(TEXT("real procedural import"), Procedurals.IsError())
		|| !TestFalse(TEXT("real texture import"), Textures.IsError())) return false;
	const FString CacheRoot = FPaths::ConvertRelativePathToFull(
		FPaths::AutomationTransientDir() / (TEXT("SeekerVisual_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)) / TEXT("ContentCache"));
	FString Error;
	if (!TestTrue(TEXT("authenticated generation publishes"),
		WoTPayloadActivationTest_PublishObjects(CacheRoot, Writer.Objects, Error)))
	{
		AddError(Error);
		return false;
	}
	ResetWoTPayloadActivationForTesting();
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, true);
	UWoTRuntimeAssets::SetPayloadTextureResolutionForTesting(true);
	ON_SCOPE_EXIT
	{
		UWoTRuntimeAssets::SetPayloadTextureResolutionForTesting(false);
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
		ResetWoTPayloadActivationForTesting();
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(CacheRoot), false, true);
	};
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bActivatePayload = false;
	Options.BeforeInitializeStandalone = [&CacheRoot](UGameInstance* GameInstance)
	{
		CastChecked<UWoTGameInstance>(GameInstance)->SetPayloadCacheDir(CacheRoot, false);
	};
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!TestNotNull(TEXT("guarded spec world"), World)) return false;
	UWoTPayloadSubsystem* Payload = World->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>();
	if (!TestTrue(TEXT("real payload activates"),
		WoTPayloadActivationTest_ActivatePublished(Payload, CacheRoot, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("world readiness publishes"), PublishWoTBootstrapWorldReady().IsError());
	UWoTFractalSubsystem* Fractals = World->GetSubsystem<UWoTFractalSubsystem>();
	Fractals->ActivateGuardedCatalogForTesting();
	AWoTCharacter* Host = World->SpawnActor<AWoTCharacter>(FVector(2000, 0, 300), FRotator::ZeroRotator);
	AWoTCharacter* Sender = World->SpawnActor<AWoTCharacter>(FVector(-2000, 0, 300), FRotator::ZeroRotator);
	NewObject<UWoTReturnToSenderReflector>(Host)->Install(Host);
	FActorSpawnParameters Spawn;
	Spawn.Instigator = Sender;
	AWoTSeekerProjectile* Incoming = World->SpawnActor<AWoTSeekerProjectile>(
		FVector(0, 0, 300), FRotator::ZeroRotator, Spawn);
	if (!TestNotNull(TEXT("incoming real Seeker"), Incoming)) return false;
	Incoming->SourcePawn = Sender;
	Incoming->SetDestination(Host);
	WoTTest::TickWorld(World, 1);
	UTexture2D* Live = Fractals->FindLiveTexture(SeekerCoreQualifiedName);
	if (!TestNotNull(TEXT("real core live texture"), Live)) return false;
	UMaterialInterface* CachedCoreMaterials[2] = {};
	int32 CachedPassIndex = 0;
	for (const TCHAR* Pass : {TEXT("Mod"), TEXT("Add")})
	{
		UInstancedStaticMeshComponent* Quad = SeekerQuad(Incoming->Sprite, Pass);
		const FString MaterialName = FString::Printf(
			TEXT("MI_PS%sFB_Angreal__SLightning01"), Pass);
		CachedCoreMaterials[CachedPassIndex] = UWoTRuntimeAssets::ResolveMaterialPath(
			FSoftObjectPath(FString::Printf(TEXT("/Game/WoT/Materials/Instances/%s.%s"),
				*MaterialName, *MaterialName)));
		TestNotNull(FString(Pass) + TEXT(" guarded cache returns MID"),
			Cast<UMaterialInstanceDynamic>(CachedCoreMaterials[CachedPassIndex]));
		TestTrue(FString(Pass) + TEXT(" incoming uses actual cached MID"),
			Quad && Quad->GetMaterial(0) == CachedCoreMaterials[CachedPassIndex]);
		++CachedPassIndex;
		TestTrue(FString(Pass) + TEXT(" incoming pass samples live texture"),
			SeekerQuadTexture(Quad) == Live);
		// This is the actual cache-owned MID, already rebound by the incoming
		// consumer. Inference deliberately treats WoTLive_* as already bound.
		TestNull(FString(Pass) + TEXT(" cached live MID bypasses inference"),
			Fractals->FindRecordForMaterialForTesting(Quad ? Quad->GetMaterial(0) : nullptr));
	}
	Incoming->ProcessTouch(Host, Host->GetActorLocation());
	TestTrue(TEXT("Reflect retires incoming actor"), Incoming->IsActorBeingDestroyed());
	AWoTSeekerProjectile* Returned = nullptr;
	for (TActorIterator<AWoTSeekerProjectile> It(World); It; ++It)
		if (*It != Incoming && !It->IsActorBeingDestroyed()) Returned = *It;
	if (!TestNotNull(TEXT("Reflect spawned replacement Seeker"), Returned)
		|| !TestNotNull(TEXT("replacement globe"), Returned->Globe.Get())) return false;
	TestTrue(TEXT("replacement belongs to reflector"), Returned->SourcePawn == Host);
	// Age out all old draw evidence. No primitive receives synthetic render
	// evidence until the replacement is the only possible visible consumer.
	WoTTest::TickWorld(World, 15);
	UWoTSpriteBillboardComponent* GlobeSprite = Returned->Globe->Sprite;
	TestTrue(TEXT("real globe frames resolve from guarded payload"), GlobeSprite->HasResolvedFrames());
	const int32 GlobeBefore = GlobeSprite->GetCurrentFrameForTesting();
	UTexture* GlobeTextureBefore = GlobeSprite->GetBoundTextureForTesting();
	TestNotNull(TEXT("replacement globe has a visible texture"), GlobeTextureBefore);
	const int32 StepsBefore = Fractals->GetStepCount(SeekerCoreQualifiedName);
	CachedPassIndex = 0;
	for (const TCHAR* Pass : {TEXT("Mod"), TEXT("Add")})
	{
		UInstancedStaticMeshComponent* Quad = SeekerQuad(Returned->Sprite, Pass);
		if (!TestNotNull(FString(Pass) + TEXT(" replacement quad"), Quad)) continue;
		TestTrue(FString(Pass) + TEXT(" replacement reuses guarded cached MID"),
			Quad->GetMaterial(0) == CachedCoreMaterials[CachedPassIndex++]);
		TestTrue(FString(Pass) + TEXT(" replacement samples live texture"),
			SeekerQuadTexture(Quad) == Live);
		const int32 BeforePass = Fractals->GetStepCount(SeekerCoreQualifiedName);
		Quad->GetSceneData().SetLastRenderTimeOnScreen(World->GetTimeSeconds());
		GlobeSprite->MarkDrawnForTesting();
		WoTTest::TickWorld(World, 1);
		TestTrue(FString(Pass) + TEXT(" replacement alone advances fractal"),
			Fractals->GetStepCount(SeekerCoreQualifiedName) > BeforePass);
		WoTTest::TickWorld(World, 15);
	}
	TestTrue(TEXT("replacement visible core keeps advancing"),
		Fractals->GetStepCount(SeekerCoreQualifiedName) > StepsBefore);
	TestTrue(TEXT("replacement globe advances animated texture"),
		GlobeSprite->GetCurrentFrameForTesting() != GlobeBefore);
	TestTrue(TEXT("replacement globe changes sampled texture"),
		GlobeSprite->GetBoundTextureForTesting() != GlobeTextureBefore);
	const int32 HiddenSteps = Fractals->GetStepCount(SeekerCoreQualifiedName);
	WoTTest::TickWorld(World, 15);
	TestEqual(TEXT("undrawn replacement stops advancing after render grace"),
		Fractals->GetStepCount(SeekerCoreQualifiedName), HiddenSteps);
	return true;
}

#endif
