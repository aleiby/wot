#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestSoundDispatcher.h"

// Projectile launches on real maps. Each map is materialized the way a
// release build does it (the runtime level built from the purchaser's files,
// then every placed actor projected), so the level geometry and the map's own
// zone classification are the real ones.

#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../Projectiles/WoTGenericProjectile.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeMapSky.h"
#include "../World/WoTWorldMaterializer.h"
#include "../World/WoTWorldProjector.h"
#include "../WoTBSPPayload.h"
#include "../WoTCharacter.h"
#include "../WoTEvents.h"
#include "../WoTGameMode.h"
#include "../WoTMeshLight.h"
#include "../WoTPlayerController.h"
#include "../WoTSkyBox.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/FileHelper.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

namespace WoTProjectileRealMap
{
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
constexpr float Step = 1.0f / 30.0f;

FActorSpawnParameters AlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

class FObjectWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("projectile retail writer overflow")));
		}
		if (Schema == TEXT("wot-world-v3") || Schema == TEXT("wot-bsp-v3")
			|| Schema == TEXT("wot-class-default-v3"))
		{
			TArray<uint8>& Copy = Objects.Add(FString(LogicalId));
			Copy.Append(Bytes.GetData(), Bytes.Num());
		}
		return FWoTBootstrapStatus::Success();
	}
	TMap<FString, TArray<uint8>> Objects;
};

class FProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};

// The multiplayer world set and class defaults, imported once from the
// purchaser's retail files.
struct FFixture
{
	bool Load(FAutomationTestBase& Test)
	{
		if (bAttempted)
		{
			if (!bLoaded) Test.AddError(Error);
			return bLoaded;
		}
		bAttempted = true;
		const FString RetailRoot = WoTReferencePath(TEXT("original"));
		TWoTBootstrapResult<FWoTRetailContract> Contract = WoTLoadProductionRetailContract();
		if (Contract.IsError())
		{
			Error = Contract.GetError().ToString();
			Test.AddError(Error);
			return false;
		}
		FWoTRetailContract WorldContract;
		FWoTRetailContract DefaultsContract;
		TArray<FWoTWorldPackageBytesForTesting> WorldSources;
		TArray<FWoTRemainingSourceBytesForTesting> DefaultSources;
		TArray<FString> DefaultLogicalIds;
		for (const FWoTRequiredFact& Fact : Contract.GetValue().Facts)
		{
			const bool bMultiplayerWorld =
				(Fact.Domain == EWoTFactDomain::World || Fact.Domain == EWoTFactDomain::BSP)
				&& !Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase);
			if (bMultiplayerWorld) WorldContract.Facts.Add(Fact);
			if (bMultiplayerWorld && Fact.Domain == EWoTFactDomain::World)
			{
				FWoTWorldPackageBytesForTesting& Source = WorldSources.AddDefaulted_GetRef();
				Source.SourcePath = Fact.SourcePath;
				if (!FFileHelper::LoadFileToArray(Source.Bytes, *(RetailRoot / Fact.SourcePath)))
				{
					Error = Fact.SourcePath + TEXT(" failed to load");
					Test.AddError(Error);
					return false;
				}
			}
			if (Fact.Domain == EWoTFactDomain::ClassDefault)
			{
				DefaultsContract.Facts.Add(Fact);
				FWoTRemainingSourceBytesForTesting& Source = DefaultSources.AddDefaulted_GetRef();
				Source.SourcePath = Fact.SourcePath;
				if (!FFileHelper::LoadFileToArray(Source.Bytes, *(RetailRoot / Fact.SourcePath)))
				{
					Error = Fact.SourcePath + TEXT(" failed to load");
					Test.AddError(Error);
					return false;
				}
				if (Fact.Outputs.Num() == 1) DefaultLogicalIds.Add(Fact.Outputs[0].LogicalId);
			}
		}
		FProgress Progress;
		TWoTBootstrapResult<FWoTWorldSetResult> Imported =
			ImportRequiredWorldSetPackageBytesForTesting(WorldSources, WorldContract, Writer,
				Progress);
		if (Imported.IsError())
		{
			Error = Imported.GetError().ToString();
			Test.AddError(Error);
			return false;
		}
		Worlds = Imported.GetValue().Worlds;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportedDefaults =
			ImportRegistryLocalizationDefaultsSourcesForTesting(DefaultSources,
				DefaultsContract, Writer, Progress);
		if (ImportedDefaults.IsError())
		{
			Error = ImportedDefaults.GetError().ToString();
			Test.AddError(Error);
			return false;
		}
		TArray<FWoTRuntimeVerifiedObject> DefaultObjects;
		for (const FString& LogicalId : DefaultLogicalIds)
		{
			const TArray<uint8>* Bytes = Writer.Objects.Find(LogicalId);
			if (!Bytes)
			{
				Error = LogicalId + TEXT(" was not imported");
				Test.AddError(Error);
				return false;
			}
			TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
				FWoTRuntimeRegistry::DecodeObjectBytesForTesting(*Bytes,
					TEXT("ue1-class-defaults-v2"), TEXT("class_default"));
			if (Decoded.IsError())
			{
				Error = Decoded.GetError().ToString();
				Test.AddError(Error);
				return false;
			}
			DefaultObjects.Add(MoveTemp(Decoded).GetValue());
		}
		Defaults = MakeUnique<FWoTRuntimeClassDefaults>();
		if (!Defaults->LoadObjectsForTesting(MoveTemp(DefaultObjects), Error))
		{
			Test.AddError(Error);
			return false;
		}
		bLoaded = true;
		return true;
	}

	const FWoTWorldDescriptor* Find(FStringView Map) const
	{
		return Worlds.FindByPredicate([Map](const FWoTWorldDescriptor& Item)
		{
			return FStringView(Item.MapIdentity).Equals(Map, ESearchCase::CaseSensitive);
		});
	}

	bool bAttempted = false;
	bool bLoaded = false;
	FString Error;
	FObjectWriter Writer;
	TArray<FWoTWorldDescriptor> Worlds;
	TUniquePtr<FWoTRuntimeClassDefaults> Defaults;
};

FFixture& TheFixture()
{
	static FFixture Fixture;
	return Fixture;
}

// One map, materialized as the production host does before play: the runtime
// level (with its collision-only invisible solids), every projected actor, then
// the game mode's InitGame/BeginPlay.
struct FBuiltMap
{
	UWorld* World = nullptr;
	AWoTRuntimeBSPWorld* Level = nullptr;
	FWoTWorldProjectionResult Projection;

	~FBuiltMap()
	{
		if (World) WoTTest::DestroySpecWorld(World);
	}

};

bool BuildMap(FAutomationTestBase& Test, FFixture& Fixture, FStringView Map,
	FBuiltMap& Out)
{
	const FWoTWorldDescriptor* Descriptor = Fixture.Find(Map);
	if (!Test.TestNotNull(FString(Map) + TEXT(" is in the world set"), Descriptor))
		return false;
	const TArray<uint8>* WorldBytes = Fixture.Writer.Objects.Find(Descriptor->WorldLogicalId);
	if (!Test.TestNotNull(Descriptor->MapIdentity + TEXT(" world payload"), WorldBytes))
		return false;
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedWorld =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(*WorldBytes,
			TEXT("ue1-world-records-le-v6"), TEXT("world"));
	if (!Test.TestFalse(Descriptor->MapIdentity + TEXT(" world frame decodes"),
		DecodedWorld.IsError())) return false;
	FWoTRuntimeWorldResource Resource;
	if (!Test.TestFalse(Descriptor->MapIdentity + TEXT(" world payload decodes"),
		Resource.LoadObjectForTesting(MoveTemp(DecodedWorld).GetValue()).IsError()))
		return false;
	TArray<FWoTBSPPayloadData> Parts;
	for (const FWoTBSPPartDescriptor& PartDescriptor : Descriptor->BSPParts)
	{
		const TArray<uint8>* PartBytes = Fixture.Writer.Objects.Find(PartDescriptor.LogicalId);
		if (!Test.TestNotNull(PartDescriptor.PartIdentity + TEXT(" BSP payload"), PartBytes))
			return false;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedPart =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(*PartBytes,
				TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
		FString PartError;
		FWoTBSPPayloadData& Part = Parts.AddDefaulted_GetRef();
		if (DecodedPart.IsError() || !UWoTBSPPayloadLibrary::DecodePayload(
			DecodedPart.GetValue().Payload(), Part, PartError))
		{
			Test.AddError(Descriptor->MapIdentity + TEXT(" BSP decode: ")
				+ (DecodedPart.IsError() ? DecodedPart.GetError().ToString() : PartError));
			return false;
		}
	}
	FWoTRuntimeBSPCollisionGeometryView CollisionGeometry;
	CollisionGeometry.Vectors = Resource.Vectors();
	CollisionGeometry.Points = Resource.Points();
	CollisionGeometry.Nodes = Resource.Nodes();
	CollisionGeometry.Surfaces = Resource.Surfaces();
	CollisionGeometry.Vertices = Resource.Vertices();
	FWoTRuntimeInvisibleBSPCollisionData InvisibleCollision;
	FString Error;
	if (!Test.TestTrue(Descriptor->MapIdentity + TEXT(" collision-only solids build"),
		WoTWorldBuildInvisibleSolidBSPCollision(CollisionGeometry, InvisibleCollision, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	TArray<uint32> SurfaceFlags;
	for (const FWoTRuntimeWorldSurface& Surface : Resource.Surfaces())
		SurfaceFlags.Add(Surface.Flags);
	FWoTRetailSkyWorldView SkyView;
	SkyView.Actors = Resource.Actors();
	SkyView.Zones = Resource.Zones();
	SkyView.Nodes = Resource.Nodes();
	SkyView.Surfaces = Resource.Surfaces();

	WoTTest::FSpecWorldOptions WorldOptions;
	WorldOptions.bStartPlay = false;
	Out.World = WoTTest::CreateSpecWorld(WorldOptions);
	if (!Test.TestNotNull(Descriptor->MapIdentity + TEXT(" spec world"), Out.World))
		return false;
	UWorld& World = *Out.World;
	uint32 FallbackParts = 0;
	FString SkyError;
	if (!Test.TestTrue(Descriptor->MapIdentity + TEXT(" runtime level builds"),
		WoTWorldBuildRuntimeBSPForTesting(World, *Descriptor, Parts, Out.Level,
			FallbackParts, Error, {}, nullptr, nullptr, &SkyView, &SkyError,
			EWoTRuntimeSkyStageFault::None, nullptr, SurfaceFlags, &InvisibleCollision))
		|| !Test.TestNotNull(Descriptor->MapIdentity + TEXT(" runtime level actor"), Out.Level))
	{
		Test.AddError(Error);
		return false;
	}
	UTexture2D* MoverLightmap = nullptr;
	TArray<UTexture2D*> MoverLightResponses;
	UTexture2D* MoverBlackLightResponse = nullptr;
	if (Resource.MoverAtlasWidth() || Resource.MoverAtlasHeight()
		|| !Resource.MoverAtlasBGRA().IsEmpty())
	{
		MoverLightmap = WoTRuntimeBSPMaterializeLightmap(Resource.MoverAtlasWidth(),
			Resource.MoverAtlasHeight(), Resource.MoverAtlasBGRA());
		if (!Test.TestNotNull(TEXT("mover lightmap"), MoverLightmap)) return false;
		Out.Level->RuntimeTextures.Add(MoverLightmap);
		for (const FWoTRuntimeMoverAnimationTerm& Term : Resource.MoverAnimationTerms())
		{
			UTexture2D* Response = WoTRuntimeBSPMaterializeLightmap(Resource.MoverAtlasWidth(),
				Resource.MoverAtlasHeight(), Term.ResponseBGRA);
			if (!Test.TestNotNull(TEXT("mover light response"), Response)) return false;
			Out.Level->RuntimeTextures.Add(Response);
			MoverLightResponses.Add(Response);
		}
		if (!MoverLightResponses.IsEmpty())
		{
			MoverBlackLightResponse = WoTRuntimeBSPMaterializeSolidBlackTexture();
			if (!Test.TestNotNull(TEXT("mover black response"), MoverBlackLightResponse)) return false;
			Out.Level->RuntimeTextures.Add(MoverBlackLightResponse);
		}
	}
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = Resource.MapIdentity();
	Source.Records = Resource.Actors();
	Source.Points = Resource.Points();
	Source.Nodes = Resource.Nodes();
	Source.Vertices = Resource.Vertices();
	Source.Zones = Resource.Zones();
	Source.ReachSpecs = Resource.ReachSpecs();
	Source.Surfaces = Resource.Surfaces();
	Source.Leaves = Resource.Leaves();
	Source.ModelLights = Resource.ModelLights();
	Source.MoverModels = Resource.MoverModels();
	Source.bHasDedicatedMoverCollision = Resource.FormatVersion() >= 4;
	Source.MoverAnimationGroups = Resource.MoverAnimationGroups();
	Source.MoverAnimationTerms = Resource.MoverAnimationTerms();
	Source.MoverLightmap = MoverLightmap;
	Source.MoverLightResponses = MoverLightResponses;
	Source.MoverBlackLightResponse = MoverBlackLightResponse;
	Source.SemanticObjects = Resource.SemanticObjects();
	Source.bBSPRootOutside = Resource.BSPRootOutside();
	if (!Test.TestTrue(Descriptor->MapIdentity + TEXT(" actors project"),
		FWoTWorldProjector::ProjectDeferred(World, Source, *Fixture.Defaults,
			Out.Projection, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	Out.Level->FinishSpawning(FTransform::Identity, true);
	for (AWoTSkyBox* SkyBox : Out.Level->SkyBoxes)
		if (SkyBox) SkyBox->FinishSpawning(FTransform::Identity, true);
	for (AActor* Actor : Out.Projection.DeferredActors)
		if (Actor) Actor->FinishSpawning(Actor->GetActorTransform(), true);
	World.UpdateWorldComponents(false, false);
	World.GetWorldSettings()->DefaultGameMode = AWoTGameMode::StaticClass();
	FURL URL;
	World.SetGameMode(URL);
	World.InitializeActorsForPlay(URL);
	World.BeginPlay();
	return true;
}

int32 ZoneAt(UWorld& World, const FVector& Point)
{
	int32 Zone = INDEX_NONE;
	int32 Leaf = INDEX_NONE;
	return UWoTMeshLight::ClassifyRegion(UWoTMeshLight::FindMapLightTable(&World), Point,
		Zone, Leaf) ? Zone : INDEX_NONE;
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FWoTProjectileRealMapLaunchSpec,
	"WoT.Projectile.RealMapLaunch", WoTProjectileRealMap::Flags)

void FWoTProjectileRealMapLaunchSpec::GetTests(TArray<FString>& Names,
	TArray<FString>& Commands) const
{
	const TCHAR* Maps[] = {TEXT("Arena_01"), TEXT("Arena_02"), TEXT("Arena_03"),
		TEXT("Arena_04"), TEXT("Arena_05"), TEXT("Arena_06"), TEXT("Arena_07"),
		TEXT("Arena_08"), TEXT("Arena_09"), TEXT("Arena_11"), TEXT("Arena_12"),
		TEXT("Battle2_01"), TEXT("Battle2_02"), TEXT("Battle2_03"), TEXT("Battle2_04"),
		TEXT("Battle2_05"), TEXT("Battle3_01"), TEXT("Battle3_02"), TEXT("Battle4_01")};
	for (const TCHAR* Map : Maps)
	{
		for (const TCHAR* Kind : {TEXT("Dart"), TEXT("Fireball")})
		{
			const FString Case = FString(Map) + TEXT(".") + Kind;
			Names.Add(Case);
			Commands.Add(Case);
		}
	}
}

bool FWoTProjectileRealMapLaunchSpec::RunTest(const FString& Parameters)
{
	using namespace WoTProjectileRealMap;
	if (!TestTrue(TEXT("retail map inputs are available"), WoTReferenceTreeExists())) return false;
	FString Map, Kind;
	Parameters.Split(TEXT("."), &Map, &Kind);
	FFixture& Fixture = TheFixture();
	FBuiltMap Built;
	if (!Fixture.Load(*this) || !BuildMap(*this, Fixture, Map, Built)) return false;
	UWorld& World = *Built.World;
	AWoTGameMode* Game = World.GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("game mode"), Game)) return false;
	TArray<APlayerStart*> Starts;
	for (TActorIterator<APlayerStart> It(&World); It; ++It) Starts.Add(*It);
	Starts.Sort([](const APlayerStart& A, const APlayerStart& B)
	{
		return A.GetName() < B.GetName();
	});
	if (!TestTrue(TEXT("map has player starts"), !Starts.IsEmpty())) return false;
	int32 Shots = 0;
	int32 Displaced = 0;
	int32 Rejected = 0;
	int32 Impacts = 0;
	int32 WaterRetirements = 0;
	double MaxDisplacement = 0.0;
	for (APlayerStart* Start : Starts)
	{
		for (float Pitch : {0.0f, -45.0f})
		{
			AWoTPlayerController* Controller = World.SpawnActor<AWoTPlayerController>(AlwaysSpawn());
			if (!TestNotNull(TEXT("controller"), Controller)) return false;
			FWoTStartSpawnResult Placement;
			AWoTCharacter* Body = Cast<AWoTCharacter>(AWoTGameMode::SpawnPawnAtRetailStart(
				World, AWoTCharacter::StaticClass(), Start->GetActorTransform(), nullptr, true, &Placement));
			if (!Body)
			{
				++Rejected;
				UE_LOG(LogTemp, Display, TEXT("PROJECTILE_REJECTED %s %s reason=%s"),
					*Parameters, *Start->GetName(), WoTStartSpawnRejectionName(Placement.Rejection));
				Controller->Destroy();
				continue;
			}
			if (!TestNotNull(TEXT("controller"), Controller) || !TestNotNull(TEXT("body"), Body)) return false;
			Controller->InitPlayerState();
			Controller->SetAsLocalPlayerController();
			Controller->Possess(Body);
			Game->SetPlayerDefaults(Body);
			Body->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
			WoTTest::TickWorld(&World, 1.0f, Step);
			TestTrue(TEXT("the host body is alive inside the map"), Body->IsAlive()
				&& ZoneAt(World, Body->GetActorLocation()) > 0);
			TestFalse(TEXT("the host body stands before firing"), Body->GetCharacterMovement()->IsFalling());
			const FRotator Aim(Pitch, Start->GetActorRotation().Yaw, 0.0f);
			Controller->SetControlRotation(Aim);
			if (Kind == TEXT("Fireball"))
				Game->GivePawnAngreal(Body, AWoTAngrealInvFireball::StaticClass(), 25);
			AWoTProjectileLauncher* Launcher = nullptr;
			for (AWoTInventory* Item : Body->InventoryComponent->Items)
			{
				if ((Kind == TEXT("Dart") && Item->IsA<AWoTAngrealInvDart>())
					|| (Kind == TEXT("Fireball") && Item->IsA<AWoTAngrealInvFireball>()))
					Launcher = Cast<AWoTProjectileLauncher>(Item);
			}
			if (!TestNotNull(TEXT("granted launcher"), Launcher)) return false;
			Body->InventoryComponent->SelectItem(Launcher);
			const FVector Eye = Body->GetActorLocation() + FVector(0, 0, Body->BaseEyeHeight);
			const FVector Requested = Eye + Body->GetActorRotation().RotateVector(
				Launcher->ProjectileClass.GetDefaultObject()->LaunchOffset);
			const int32 Charges = Launcher->CurCharges;
			const AWoTGenericProjectile* Defaults = Launcher->ProjectileClass.GetDefaultObject();
			AWoTGenericProjectile* Projectile = nullptr;
			bool bImpact = false;
			bool bTriggerImpact = false;
			FVector ImpactLocation = FVector::ZeroVector;
			const FDelegateHandle ImpactHandle = AWoTGenericProjectile::OnImpactProbe.AddLambda(
				[&](AActor* Actor, const FHitResult& Hit)
				{
					if (Actor->GetInstigator() != Body || !Actor->IsA(Launcher->ProjectileClass)) return;
					bImpact = true;
					ImpactLocation = Actor->GetActorLocation();
					const AWoTTrigger* Trigger = Cast<AWoTTrigger>(Hit.GetActor());
					bTriggerImpact |= Trigger && !Trigger->IsRetailProjectileTarget();
				});
			FVector Spawn = FVector::ZeroVector;
			const FDelegateHandle Handle = World.AddOnActorSpawnedHandler(
				FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor)
				{
					if (Actor->IsA(Launcher->ProjectileClass))
					{
						Projectile = Cast<AWoTGenericProjectile>(Actor);
						Spawn = Actor->GetActorLocation();
					}
				}));
			Controller->Fire();
			Controller->CeaseFire();
			World.RemoveOnActorSpawnedHandler(Handle);
			++Shots;
			const FString Label = FString::Printf(TEXT("%s %s pitch %.0f"), *Parameters,
				*Start->GetName(), Pitch);
			TestEqual(Label + TEXT(" spends one charge"), Launcher->CurCharges, Charges - 1);
			if (TestNotNull(Label + TEXT(" creates a projectile"), Projectile))
			{
				const double Distance = FVector::Dist(Spawn, Requested);
				MaxDisplacement = FMath::Max(MaxDisplacement, Distance);
				Displaced += Distance > 30.0 ? 1 : 0;
				TestTrue(FString::Printf(TEXT("%s starts at the eye within a local adjustment: eye %s, spawn %s, distance %.3f"),
					*Label, *Eye.ToString(), *Spawn.ToString(), Distance), Distance < 30.0);
				int32 Ticks = 0;
				double MaxStep = 0.0;
				for (int32 Tick = 0; Tick < 90 && IsValid(Projectile); ++Tick)
				{
					const FVector Before = Projectile->GetActorLocation();
					WoTTest::TickWorld(&World, 1, Step);
					++Ticks;
					MaxStep = FMath::Max(MaxStep, FVector::Dist(Before, Projectile->GetActorLocation()));

				}
				TestFalse(Label + TEXT(" never strikes a non-target trigger volume"), bTriggerImpact);
				TestTrue(Label + TEXT(" moves continuously at its launch speed"),
					MaxStep <= Defaults->Speed * Step + 1.0);
				const bool bWaterRetirement = Kind == TEXT("Fireball")
					&& Projectile->bHitWater && !bImpact;
				TestTrue(Label + TEXT(" flies or ends at an impact or water"),
					IsValid(Projectile) || bImpact || bWaterRetirement);
				if (IsValid(Projectile))
					TestTrue(Label + TEXT(" advances along the aimed flight path"),
						FVector::Dist(Projectile->GetActorLocation(), Spawn + Aim.Vector() * Defaults->Speed * Ticks * Step) < 2.0);
				if (bImpact)
				{
					++Impacts;
					TestTrue(Label + TEXT(" impacts within its flight distance of the eye"),
						FVector::Dist(ImpactLocation, Requested) <= Defaults->Speed * Ticks * Step + 30.0);
				}
				if (!IsValid(Projectile) && bWaterRetirement) ++WaterRetirements;
				if (IsValid(Projectile)) Projectile->Destroy();
			}
			AWoTGenericProjectile::OnImpactProbe.Remove(ImpactHandle);
			Controller->UnPossess();
			Body->Destroy();
			Controller->Destroy();
		}
	}
	TestTrue(TEXT("at least one valid start was fired from"), Shots > 0);
	UE_LOG(LogTemp, Display, TEXT("PROJECTILE_SWEEP %s starts=%d shots=%d rejected=%d displaced=%d max=%.3f impacts=%d water=%d"),
		*Parameters, Starts.Num(), Shots, Rejected, Displaced, MaxDisplacement, Impacts, WaterRetirements);
	return !HasAnyErrors();
}

// Trigger volumes deliver touches without acting as walls. A shoot target still
// consumes the projectile through the normal target filter and ProcessTouch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTProjectileTriggerTouchSpec,
	"WoT.Projectile.TriggerTouches", WoTProjectileRealMap::Flags)

bool FWoTProjectileTriggerTouchSpec::RunTest(const FString&)
{
	using namespace WoTProjectileRealMap;
	for (const EWoTTriggerType Type : {EWoTTriggerType::PlayerProximity,
		EWoTTriggerType::AnyProximity, EWoTTriggerType::ClassProximity, EWoTTriggerType::Shoot})
	{
		FBuiltMap Built;
		Built.World = WoTTest::CreateSpecWorld();
		if (!TestNotNull(TEXT("touch world"), Built.World)) return false;
		UWorld& World = *Built.World;
		AWoTTestSoundDispatcherEventSink* Sink = World.SpawnActorDeferred<AWoTTestSoundDispatcherEventSink>(
			AWoTTestSoundDispatcherEventSink::StaticClass(), FTransform::Identity);
		Sink->TagName = TEXT("ProjectileTouch");
		Sink->FinishSpawning(FTransform::Identity);
		const FTransform At(FVector(300, 0, 1000));
		AWoTTrigger* Trigger = World.SpawnActorDeferred<AWoTTrigger>(AWoTTrigger::StaticClass(), At);
		Trigger->EventName = Sink->TagName;
		Trigger->TriggerType = Type;
		Trigger->ClassProximityType = AWoTDartProjectile::StaticClass();
		Trigger->SetUE1CollisionSize(64.0f, 64.0f);
		Trigger->FinishSpawning(At);
		AWoTDartProjectile* Projectile = World.SpawnActor<AWoTDartProjectile>(
			FVector(0, 0, 1000), FRotator::ZeroRotator, AlwaysSpawn());
		if (!TestNotNull(TEXT("touch projectile"), Projectile)) return false;
		WoTTest::TickWorld(&World, 12, 1.0f / 60.0f);
		const FString Label = FString::Printf(TEXT("trigger type %d"), int32(Type));
		const bool bShoot = Type == EWoTTriggerType::Shoot;
		TestEqual(Label + TEXT(" consumes only a projectile hitting a shoot target"), IsValid(Projectile), !bShoot);
		const bool bTouchEvent = Type == EWoTTriggerType::AnyProximity || Type == EWoTTriggerType::ClassProximity;
		// A shot shoot-trigger's own event (its damage threshold) is not ported
		// yet; this spec covers only the collision response.
		if (!bShoot)
			TestEqual(Label + TEXT(" receives its relevant touch"), Sink->TriggerCount, bTouchEvent ? 1 : 0);
		if (bTouchEvent) TestEqual(Label + TEXT(" preserves the touching actor"), Sink->LastOther.Get(), static_cast<AActor*>(Projectile));
		if (IsValid(Projectile))
			TestTrue(Label + TEXT(" lets flight continue through the volume"),
				FVector::Dist(Projectile->GetActorLocation(), FVector(400, 0, 1000)) < 1.0);
	}
	return !HasAnyErrors();
}

#endif

