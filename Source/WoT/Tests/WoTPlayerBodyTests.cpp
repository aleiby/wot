#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

// A player body works the world the same whoever drives it. The original's
// player test (Pawn.bIsPlayer, or a WOTPlayer cast) belongs to the body, so a
// player body driven by an AI (a bot) opens player doors, starts lifts, fires
// player-proximity triggers, is crushed by crushing movers, sounds the heal zone
// and banks seals like a person's, while an NPC body does none of these
// (WoTIsPlayerBody, WoTCharacter.h). An AAIController holding a bot's player
// state stands in for a bot, so IsPlayerControlled() reads false for it.
//
// The real-map specs materialize Arena_02, Arena_04 and Arena_11 the way a
// release build does (the runtime level built from the purchaser's files, then
// every placed actor projected), so the trigger, door, lift and updraft are the
// maps' own.

#include "WoTTestAIClasses.h"
#include "WoTTestBattleClasses.h"
#include "WoTTestSoundDispatcher.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTAngrealInvShields.h"
#include "../Inventory/WoTIgnoreElementReflector.h"
#include "../Inventory/WoTAngrealSpawnPool.h"
#include "../Inventory/WoTSeal.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeMapSky.h"
#include "../World/WoTRuntimeWorldProperties.h"
#include "../World/WoTWorldMaterializer.h"
#include "../World/WoTWorldProjector.h"
#include "../WoTBSPPayload.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTCharacterMovement.h"
#include "../WoTMeshLight.h"
#include "../WoTEvents.h"
#include "../WoTGameMode.h"
#include "../WoTMover.h"
#include "../WoTPlayerState.h"
#include "../WoTSealAltar.h"
#include "../WoTSkyBox.h"
#include "../WoTSoundTable.h"
#include "../WoTZoneVolume.h"

#include "AIController.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"
#include "../Payload/WoTPayloadActivation.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

namespace WoTPlayerBodySpec
{
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
constexpr float Step = 1.0f / 30.0f;

// Maps whose level geometry these specs build; the rest keep only their records.
const TCHAR* const LevelMaps[] = {TEXT("Arena_02"), TEXT("Arena_04"), TEXT("Arena_11")};

FActorSpawnParameters AlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

// A bot's stand-in: an ordinary player body possessed by an AI controller that
// holds a player state (an AI controller's player state marks itself a bot).
AWoTCharacter* SpawnAIDrivenBody(FAutomationTestBase& Test, UWorld& World,
	const FVector& Location, uint8 Team = 255)
{
	AWoTCharacter* Body = World.SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), Location, FRotator::ZeroRotator, AlwaysSpawn());
	AAIController* Driver = World.SpawnActor<AAIController>(AlwaysSpawn());
	if (!Test.TestNotNull(TEXT("player body"), Body)
		|| !Test.TestNotNull(TEXT("AI controller"), Driver))
	{
		return nullptr;
	}
	Driver->InitPlayerState();
	if (AWoTPlayerState* State = Driver->GetPlayerState<AWoTPlayerState>())
	{
		State->Team = Team;
	}
	Driver->Possess(Body);
	Test.TestTrue(TEXT("the AI-driven body is a bot's, not a person's"),
		Body->IsBotControlled() && !Body->IsPlayerControlled());
	return Body;
}

AWoTCharacter* SpawnNPCBody(FAutomationTestBase& Test, UWorld& World,
	const FVector& Location)
{
	AWoTCharacter* NPC = World.SpawnActor<AWoTTestNPCCharacter>(
		AWoTTestNPCCharacter::StaticClass(), Location, FRotator::ZeroRotator,
		AlwaysSpawn());
	Test.TestNotNull(TEXT("NPC body"), NPC);
	return NPC;
}

// One touch delivered from inside the trigger's flat cylinder; the toucher
// then returns to where it was.
void TouchTrigger(AWoTTrigger& Trigger, APawn& Toucher)
{
	const FVector Home = Toucher.GetActorLocation();
	WoTTest::PlaceWithoutOverlapEvents(&Toucher, Trigger.GetActorLocation());
	Trigger.OnOverlap(nullptr, &Toucher, nullptr, 0, false, FHitResult());
	WoTTest::PlaceWithoutOverlapEvents(&Toucher, Home);
}

void Bump(AWoTMover& Mover, APawn& Bumper)
{
	Mover.NotifyHit(nullptr, &Bumper, nullptr, false, FVector::ZeroVector,
		FVector::ZeroVector, FVector::ZeroVector, FHitResult());
}

int32 CountSoundMarkers(UWorld& World, FName SubPath)
{
	int32 Count = 0;
	for (TActorIterator<AWoTSoundBurst> It(&World); It; ++It)
	{
		Count += IsValid(*It) && It->SoundSubPath == SubPath ? 1 : 0;
	}
	return Count;
}

// ---------------------------------------------------------------------------
// The real maps.

class FObjectWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("player-body retail writer overflow")));
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
		// Keep the records of every map but the level geometry only of the maps
		// these specs build.
		for (const FWoTWorldDescriptor& Descriptor : Worlds)
		{
			bool bKeep = false;
			for (const TCHAR* Map : LevelMaps) bKeep |= Descriptor.MapIdentity == Map;
			if (bKeep) continue;
			for (const FWoTBSPPartDescriptor& Part : Descriptor.BSPParts)
			{
				Writer.Objects.Remove(Part.LogicalId);
			}
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
	TArray<FWoTRuntimeWorldActor> Records;

	~FBuiltMap()
	{
		if (World) WoTTest::DestroySpecWorld(World);
	}

	// The projected actor of class T made from the retail record Identity
	// ("Arena_02.Trigger0").
	template <typename T>
	T* Projected(FStringView Identity) const
	{
		const int32 Index = Records.IndexOfByPredicate(
			[Identity](const FWoTRuntimeWorldActor& Record)
			{
				return FStringView(Record.Identity).Equals(Identity, ESearchCase::CaseSensitive);
			});
		if (Index == INDEX_NONE) return nullptr;
		const FString Prefix = FString::Printf(TEXT("WoTProjected_%d_"), Index);
		for (TActorIterator<T> It(World); It; ++It)
		{
			if (It->GetName().StartsWith(Prefix)) return *It;
		}
		return nullptr;
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
	// The movers' light-map atlas and its responses, made from the world payload
	// as the materializer makes them (a mover with baked light needs them to
	// project) and kept alive by the runtime level.
	FWoTWorldProjectionSource Source;
	TArray<UTexture2D*> Responses;
	if (Resource.MoverAtlasWidth() != 0 || Resource.MoverAtlasHeight() != 0
		|| !Resource.MoverAtlasBGRA().IsEmpty())
	{
		Source.MoverLightmap = WoTRuntimeBSPMaterializeLightmap(Resource.MoverAtlasWidth(),
			Resource.MoverAtlasHeight(), Resource.MoverAtlasBGRA());
		if (!Test.TestNotNull(Descriptor->MapIdentity + TEXT(" mover light-map"),
			Source.MoverLightmap)) return false;
		Out.Level->RuntimeTextures.Add(Source.MoverLightmap);
		for (const FWoTRuntimeMoverAnimationTerm& Term : Resource.MoverAnimationTerms())
		{
			UTexture2D* Response = WoTRuntimeBSPMaterializeLightmap(Resource.MoverAtlasWidth(),
				Resource.MoverAtlasHeight(), Term.ResponseBGRA);
			if (!Test.TestNotNull(Descriptor->MapIdentity + TEXT(" mover light-map response"),
				Response)) return false;
			Out.Level->RuntimeTextures.Add(Response);
			Responses.Add(Response);
		}
		Source.MoverLightResponses = Responses;
		if (!Responses.IsEmpty())
		{
			Source.MoverBlackLightResponse = WoTRuntimeBSPMaterializeSolidBlackTexture();
			if (!Test.TestNotNull(TEXT("black light-map response"),
				Source.MoverBlackLightResponse)) return false;
			Out.Level->RuntimeTextures.Add(Source.MoverBlackLightResponse);
		}
	}
	Out.Records = Resource.Actors();
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

// Builds Map, or warns and reports false when the reference tree is absent.
bool LoadAndBuild(FAutomationTestBase& Test, FStringView Map, FBuiltMap& Built)
{
	if (!WoTReferenceTreeExists())
	{
		Test.AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return false;
	}
	FFixture& Fixture = TheFixture();
	return Fixture.Load(Test) && BuildMap(Test, Fixture, Map, Built);
}

// A spot inside the level well away from Point: the farthest player start.
FVector FarSpot(UWorld& World, const FVector& Point)
{
	FVector Farthest = Point;
	double FarthestDistance = -1.0;
	for (TActorIterator<APlayerStart> It(&World); It; ++It)
	{
		const double Distance = FVector::DistSquared(It->GetActorLocation(), Point);
		if (Distance > FarthestDistance)
		{
			FarthestDistance = Distance;
			Farthest = It->GetActorLocation();
		}
	}
	return Farthest;
}

// Ticks until the mover's phase is Wanted, at most Seconds.
bool TickUntilPhase(UWorld& World, const AWoTMover& Mover, EWoTMoverPhase Wanted,
	float Seconds)
{
	for (float Elapsed = 0.0f; Elapsed < Seconds; Elapsed += Step)
	{
		if (Mover.GetMoverPhase() == Wanted) return true;
		WoTTest::TickWorld(&World, 1, Step);
	}
	return Mover.GetMoverPhase() == Wanted;
}

// Where a body can stand on the lift's floor: Arena_04's lifts are a platform
// with a post rising from its middle, so the spot is found by tracing down
// through the mover's own collision a third of the way in from its -X edge.
bool LiftFloorSpot(AWoTMover& Mover, FVector& OutSpot)
{
	const FBox Box = Mover.GetComponentsBoundingBox(true);
	if (!Box.IsValid) return false;
	const FVector Center = Box.GetCenter();
	const double X = Center.X - Box.GetExtent().X * 2.0 / 3.0;
	const FVector Start(X, Center.Y, Box.Max.Z + 10.0);
	const FVector End(X, Center.Y, Box.Min.Z - 10.0);
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTPlayerBodyLiftFloor), true);
	bool bFound = false;
	TInlineComponentArray<UPrimitiveComponent*> Components;
	Mover.GetComponents(Components);
	for (UPrimitiveComponent* Component : Components)
	{
		FHitResult Hit;
		if (IsValid(Component) && Component->IsQueryCollisionEnabled()
			&& Component->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block
			&& Component->LineTraceComponent(Hit, Start, End, Params)
			&& (!bFound || Hit.ImpactPoint.Z > OutSpot.Z))
		{
			OutSpot = Hit.ImpactPoint;
			bFound = true;
		}
	}
	return bFound;
}

// Drops Body just above the lift's floor and lets it fall; true when it comes
// to stand on the mover.
bool DropOnto(FAutomationTestBase& Test, UWorld& World, AWoTMover& Mover,
	ACharacter& Body)
{
	FVector Floor;
	if (!Test.TestTrue(TEXT("the lift has a floor to stand on"), LiftFloorSpot(Mover, Floor)))
		return false;
	const float HalfHeight = Body.GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	Body.SetActorLocation(Floor + FVector(0.0, 0.0, HalfHeight + 24.0f),
		false, nullptr, ETeleportType::TeleportPhysics);
	Body.GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	for (float Elapsed = 0.0f; Elapsed < 1.0f; Elapsed += Step)
	{
		WoTTest::TickWorld(&World, 1, Step);
		const UPrimitiveComponent* Base = Cast<UPrimitiveComponent>(Body.GetMovementBaseObject());
		if (Base && Base->GetOwner() == &Mover) return true;
	}
	const UPrimitiveComponent* Base = Cast<UPrimitiveComponent>(Body.GetMovementBaseObject());
	Test.AddWarning(FString::Printf(TEXT("the dropped body rests at %s on %s"),
		*Body.GetActorLocation().ToString(),
		Base && Base->GetOwner() ? *Base->GetOwner()->GetName() : TEXT("nothing")));
	return false;
}
}

// A player-proximity trigger (the engine Trigger's class default) fires for an
// AI-driven player body and for a player body nobody drives (a dead or
// unpossessed player's body: bIsPlayer belongs to the body), as for a person's,
// and never for an NPC body.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerBodyProximityTriggerSpec,
	"WoT.Events.PlayerBody.ProximityTriggerFiresForAIDrivenBody",
	WoTPlayerBodySpec::Flags)

bool FWoTPlayerBodyProximityTriggerSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	AWoTTestSoundDispatcherEventSink* Sink =
		World->SpawnActorDeferred<AWoTTestSoundDispatcherEventSink>(
			AWoTTestSoundDispatcherEventSink::StaticClass(), FTransform::Identity);
	Sink->TagName = TEXT("PlayerBodyTrigger");
	Sink->FinishSpawning(FTransform::Identity);
	const FTransform At(FRotator::ZeroRotator, FVector(2000, 0, 0));
	AWoTTrigger* Trigger = World->SpawnActorDeferred<AWoTTrigger>(
		AWoTTrigger::StaticClass(), At);
	Trigger->EventName = TEXT("PlayerBodyTrigger");
	Trigger->SetUE1CollisionSize(100.0f, 100.0f);
	Trigger->FinishSpawning(At);
	TestEqual(TEXT("a trigger defaults to player proximity"),
		Trigger->TriggerType, EWoTTriggerType::PlayerProximity);

	AWoTCharacter* NPC = PB::SpawnNPCBody(*this, *World, FVector(0, 1000, 0));
	AWoTCharacter* Bot = PB::SpawnAIDrivenBody(*this, *World, FVector(0, 2000, 0));
	AWoTCharacter* Unpossessed = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(0, 3000, 0), FRotator::ZeroRotator,
		PB::AlwaysSpawn());
	AWoTCharacter* Person = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(0, 4000, 0), FRotator::ZeroRotator,
		PB::AlwaysSpawn());
	APlayerController* PersonController =
		World->SpawnActor<APlayerController>(PB::AlwaysSpawn());
	if (!NPC || !Bot || !TestNotNull(TEXT("unpossessed body"), Unpossessed)
		|| !TestNotNull(TEXT("person's body"), Person)
		|| !TestNotNull(TEXT("person's controller"), PersonController))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PersonController->Possess(Person);

	PB::TouchTrigger(*Trigger, *NPC);
	TestEqual(TEXT("an NPC body does not fire a player-proximity trigger"),
		Sink->TriggerCount, 0);
	PB::TouchTrigger(*Trigger, *Bot);
	TestEqual(TEXT("an AI-driven player body fires it"), Sink->TriggerCount, 1);
	TestEqual(TEXT("the AI-driven body is the event instigator"),
		Sink->LastInstigator.Get(), static_cast<APawn*>(Bot));
	PB::TouchTrigger(*Trigger, *Unpossessed);
	TestEqual(TEXT("a player body nobody drives fires it"), Sink->TriggerCount, 2);
	PB::TouchTrigger(*Trigger, *Person);
	TestEqual(TEXT("a person's player body fires it"), Sink->TriggerCount, 3);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// A lift with the mover defaults (bump-open, player bump) starts when an
// AI-driven player body bumps it, and stays shut for an NPC body.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerBodyBumpLiftSpec,
	"WoT.Mover.PlayerBody.BumpLiftStartsForAIDrivenBody",
	WoTPlayerBodySpec::Flags)

bool FWoTPlayerBodyBumpLiftSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	const FTransform At(FRotator::ZeroRotator, FVector(1000, 0, 0));
	AWoTMover* Lift = World->SpawnActorDeferred<AWoTMover>(AWoTMover::StaticClass(), At);
	Lift->KeyPos = {FVector::ZeroVector, FVector(0, 0, 200)};
	Lift->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Lift->MoveTime = 0.5f;
	Lift->StayOpenTime = 1.0f;
	Lift->FinishSpawning(At);
	TestEqual(TEXT("a mover defaults to bump-open"),
		Lift->InitialState, EWoTMoverState::BumpOpenTimed);
	TestEqual(TEXT("a mover defaults to player bump"),
		Lift->BumpType, EWoTMoverBumpType::PlayerBump);
	const double BaseZ = Lift->GetActorLocation().Z;

	AWoTCharacter* NPC = PB::SpawnNPCBody(*this, *World, FVector(0, 1000, 0));
	AWoTCharacter* Bot = PB::SpawnAIDrivenBody(*this, *World, FVector(0, 2000, 0));
	if (!NPC || !Bot)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PB::Bump(*Lift, *NPC);
	TestEqual(TEXT("an NPC body's bump leaves the lift closed"),
		Lift->GetMoverPhase(), EWoTMoverPhase::Closed);
	PB::Bump(*Lift, *Bot);
	TestEqual(TEXT("an AI-driven player body's bump starts the lift"),
		Lift->GetMoverPhase(), EWoTMoverPhase::Opening);
	WoTTest::TickWorld(World, 0.6f);
	TestTrue(TEXT("the lift rises toward its open key"),
		Lift->GetActorLocation().Z > BaseZ + 150.0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// A crushing mover kills a living AI-driven player body as it does a person's
// (the player body's KilledBy), and leaves an NPC body and a dying player's
// body alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerBodyCrushSpec,
	"WoT.Mover.PlayerBody.CrushKillsAIDrivenBody",
	WoTPlayerBodySpec::Flags)

bool FWoTPlayerBodyCrushSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	AWoTMover* Crusher = World->SpawnActorDeferred<AWoTMover>(
		AWoTMover::StaticClass(), FTransform::Identity);
	Crusher->KeyPos = {FVector::ZeroVector, FVector(0, 0, 200)};
	Crusher->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Crusher->EncroachType = EWoTMoverEncroachType::CrushWhenEncroach;
	Crusher->FinishSpawning(FTransform::Identity);

	AWoTCharacter* NPC = PB::SpawnNPCBody(*this, *World, FVector(0, 1000, 0));
	AWoTCharacter* Bot = PB::SpawnAIDrivenBody(*this, *World, FVector(0, 2000, 0));
	if (!NPC || !Bot)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const int32 NPCHealth = NPC->Health;
	TestFalse(TEXT("the crush lets the mover proceed past an AI-driven body"),
		Crusher->EncroachingOn(Bot));
	TestTrue(TEXT("an AI-driven player body is crushed"), Bot->Health <= 0);
	TestFalse(TEXT("the crush lets the mover proceed past an NPC body"),
		Crusher->EncroachingOn(NPC));
	TestEqual(TEXT("an NPC body is not crushed"), NPC->Health, NPCHealth);
	// A dying player's body ignores the crush (its KilledBy), driven or not.
	AWoTCharacter* Dying = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(0, 3000, 0), FRotator::ZeroRotator,
		PB::AlwaysSpawn());
	if (TestNotNull(TEXT("dying player body"), Dying))
	{
		Dying->Health = 0;
		Crusher->EncroachingOn(Dying);
		TestEqual(TEXT("a dying player body is not crushed again"), Dying->Health, 0);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// A heavy corpse coming to rest throws the walking player bodies near it. The
// original walks every pawn and throws each player body (WOTCarcass.uc:219-235,
// PlayerPawn(Thrown)), so an AI-driven player body takes the upward kick and an
// NPC body is not thrown.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerBodyCarcassThrowSpec,
	"WoT.P28.LandThumpThrowsAIDrivenBody",
	WoTPlayerBodySpec::Flags)

bool FWoTPlayerBodyCarcassThrowSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	constexpr float Frame = 1.0f / 60.0f;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	// A wide floor whose top is at Z = 0.
	AActor* Floor = World->SpawnActor<AActor>(FVector(0, 0, -50), FRotator::ZeroRotator);
	UBoxComponent* Slab = NewObject<UBoxComponent>(Floor, TEXT("PlayerBodyFloor"));
	Slab->SetBoxExtent(FVector(6000, 6000, 50));
	Slab->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Slab->SetCollisionObjectType(ECC_WorldStatic);
	Slab->SetCollisionResponseToAllChannels(ECR_Block);
	Floor->SetRootComponent(Slab);
	Slab->RegisterComponent();
	Floor->SetActorLocation(FVector(0, 0, -50));

	AWoTCharacter* Bot = PB::SpawnAIDrivenBody(*this, *World, FVector(400, 0, 100));
	AWoTCharacter* NPC = PB::SpawnNPCBody(*this, *World, FVector(-400, 0, 100));
	if (!Bot || !NPC)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	for (AWoTCharacter* Body : {Bot, NPC})
	{
		// A bare spawn starts at Health 0, and a dead body never walks.
		Body->Health = 100;
		Body->SetActorLocation(FVector(Body->GetActorLocation().X, 0.0,
			Body->GetSimpleCollisionHalfHeight() + 2.0f));
		Body->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	}
	WoTTest::TickWorld(World, 0.2f);
	TestTrue(TEXT("the AI-driven body walks on the floor"),
		Bot->GetCharacterMovement()->MovementMode == MOVE_Walking);
	TestTrue(TEXT("the NPC body walks on the floor"),
		NPC->GetCharacterMovement()->MovementMode == MOVE_Walking);

	AWoTCarcass* Corpse = World->SpawnActor<AWoTCarcass>(FVector(0, 0, 300),
		FRotator::ZeroRotator, PB::AlwaysSpawn());
	if (!TestNotNull(TEXT("heavy corpse"), Corpse))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Corpse->Mass = 3000.0f;   // a heavy (500 or more) corpse throws others
	Corpse->SetVelocityForTest(FVector(0, 0, -200));
	float BotMaxVz = 0.0f;
	float NPCMaxVz = 0.0f;
	bool bLanded = false;
	for (float Elapsed = 0.0f; Elapsed < 3.0f && !bLanded; Elapsed += Frame)
	{
		WoTTest::TickWorld(World, 1, Frame);
		BotMaxVz = FMath::Max(BotMaxVz, Bot->GetCharacterMovement()->Velocity.Z);
		NPCMaxVz = FMath::Max(NPCMaxVz, NPC->GetCharacterMovement()->Velocity.Z);
		bLanded = Corpse->IsLyingStill();
	}
	// One more frame carries a kick applied late in the landing frame.
	WoTTest::TickWorld(World, 1, Frame);
	BotMaxVz = FMath::Max(BotMaxVz, Bot->GetCharacterMovement()->Velocity.Z);
	NPCMaxVz = FMath::Max(NPCMaxVz, NPC->GetCharacterMovement()->Velocity.Z);
	TestTrue(TEXT("the heavy corpse landed"), bLanded);

	// Momentum.Z = 7000000 / ((0.4 * dist + 350) * mass).
	const UCharacterMovementComponent* BotMove = Bot->GetCharacterMovement();
	const float BotMass = BotMove->Mass > 0.0f ? BotMove->Mass : 175.0f;
	const float Dist = FVector::Dist(Corpse->GetActorLocation(), Bot->GetActorLocation());
	const float ExpectedZ = 7000000.0f / ((0.4f * Dist + 350.0f) * BotMass);
	TestTrue(FString::Printf(TEXT("the AI-driven player body takes the kick (%.1f of %.1f)"),
		BotMaxVz, ExpectedZ), BotMaxVz > 0.4f * ExpectedZ && BotMaxVz < 1.2f * ExpectedZ);
	TestTrue(FString::Printf(TEXT("the NPC body is not thrown (%.1f)"), NPCMaxVz),
		NPCMaxVz < 10.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The heal zone's sound plays for an AI-driven player body as for a person's
// (an NPC body heals silently: WoT.Sound.ZoneHeal).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerBodyHealSoundSpec,
	"WoT.Zone.PlayerBody.HealSoundForAIDrivenBody",
	WoTPlayerBodySpec::Flags)

bool FWoTPlayerBodyHealSoundSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	AWoTZoneVolume* Zone = World->SpawnActorDeferred<AWoTZoneVolume>(
		AWoTZoneVolume::StaticClass(), FTransform::Identity, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	Zone->bHealZone = true;
	Zone->HealPerSec = 5;
	Zone->HealSoundPath = FName(TEXT("Spec/HealSound"));
	Zone->Collision->SetBoxExtent(FVector(200.0f), false);
	Zone->FinishSpawning(FTransform::Identity);

	AWoTCharacter* Bot = PB::SpawnAIDrivenBody(*this, *World, FVector(0, 0, 100));
	if (!Bot)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Bot->Health = 50;
	Bot->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 2, 0.02f);
	TestTrue(TEXT("the AI-driven player body heals"), Bot->Health > 50);
	TestEqual(TEXT("its heal plays the zone's heal sound"),
		PB::CountSoundMarkers(*World, Zone->HealSoundPath), 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The seal altar serves an AI-driven player body as a person's: it banks the
// body's seal and later hands it back, while an NPC touch changes nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerBodySealAltarSpec,
	"WoT.Seal.PlayerBody.AltarServesAIDrivenBody",
	WoTPlayerBodySpec::Flags)

bool FWoTPlayerBodySealAltarSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	Battle->NumTeams = 2;
	Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	Battle->FinishSpawning(FTransform::Identity);
	if (AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>())
	{
		Mode->Battle = Battle;
	}
	const FVector AltarSpot(500, 0, 0);
	const FTransform AltarAt(FRotator::ZeroRotator, AltarSpot);
	AWoTSealAltar* Altar = World->SpawnActorDeferred<AWoTSealAltar>(
		AWoTSealAltar::StaticClass(), AltarAt);
	Altar->Team = 0;
	Altar->SealGoal = 4;
	Altar->FinishSpawning(AltarAt);

	AWoTCharacter* Bot = PB::SpawnAIDrivenBody(*this, *World, AltarSpot, 0);
	if (!Bot)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Bot->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	// Spawned clear of the body, then given through the pickup path.
	AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(AWoTSeal::StaticClass(),
		AltarSpot + FVector(0, 0, 500), FRotator::ZeroRotator, PB::AlwaysSpawn());
	if (!TestNotNull(TEXT("seal"), Seal)
		|| !TestTrue(TEXT("the AI-driven body picks up the seal"), Seal->TryPickupBy(Bot)))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Altar->TouchedByPawn(Bot);
	TestTrue(TEXT("the altar banks the AI-driven body's seal"), Altar->FindSeal() == Seal);
	TestFalse(TEXT("the banked seal left the body's inventory"),
		Bot->InventoryComponent->Items.Contains(Seal));
	WoTTest::TickWorld(World, 1.1f);

	AWoTCharacter* NPC = PB::SpawnNPCBody(*this, *World, AltarSpot + FVector(0, 0, 300));
	if (NPC)
	{
		Altar->TouchedByPawn(NPC);
		TestTrue(TEXT("an NPC touch leaves the seal banked"), Altar->FindSeal() == Seal);
		TestFalse(TEXT("an NPC touch leaves the altar active"), Altar->bInactive);
	}

	Altar->TouchedByPawn(Bot);
	TestTrue(TEXT("the altar hands the seal back to the AI-driven body"),
		Seal->GetOwner() == Bot && Bot->InventoryComponent->Items.Contains(Seal));
	TestNull(TEXT("the altar slot is clear"), Altar->FindSeal());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Arena_02's door (Mover0, TriggerControl) opens when a player body enters
// Arena_02's door trigger (Trigger0, whose Event is the door's Tag; the Trigger
// class default player proximity): an AI-driven player body opens it, an NPC
// body does not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerBodyRealArena02DoorSpec,
	"WoT.World.PlayerBody.RealArena02TriggerOpensDoorForAIDrivenBody",
	WoTPlayerBodySpec::Flags)

bool FWoTPlayerBodyRealArena02DoorSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	PB::FBuiltMap Built;
	if (!PB::LoadAndBuild(*this, TEXT("Arena_02"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	AWoTTrigger* Trigger = Built.Projected<AWoTTrigger>(TEXT("Arena_02.Trigger0"));
	AWoTMover* Door = Built.Projected<AWoTMover>(TEXT("Arena_02.Mover0"));
	if (!TestNotNull(TEXT("Arena_02 projects Trigger0"), Trigger)
		|| !TestNotNull(TEXT("Arena_02 projects Mover0"), Door))
		return false;
	TestEqual(TEXT("Trigger0 is a player-proximity trigger"),
		Trigger->TriggerType, EWoTTriggerType::PlayerProximity);
	TestFalse(TEXT("Arena_02's door has a tag"), Door->TagName.IsNone());
	TestEqual(TEXT("Arena_02's door trigger fires the door's tag"),
		Trigger->EventName, Door->TagName);
	TestEqual(TEXT("Mover0 is trigger-controlled"),
		Door->InitialState, EWoTMoverState::TriggerControl);
	const double ClosedZ = Door->GetActorLocation().Z;
	const FVector Away = PB::FarSpot(World, Trigger->GetActorLocation());
	TestTrue(TEXT("a spot well outside Trigger0"),
		FVector::Dist(Away, Trigger->GetActorLocation()) > 1000.0);

	// The bodies hover where they are put, so each stays inside the trigger
	// until it is moved out.
	AWoTCharacter* NPC = PB::SpawnNPCBody(*this, World, Away);
	if (!NPC) return false;
	NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	NPC->SetActorLocation(Trigger->GetActorLocation(), false, nullptr,
		ETeleportType::TeleportPhysics);
	WoTTest::TickWorld(&World, 6, PB::Step);
	TestEqual(TEXT("an NPC body in Trigger0 leaves the door shut"),
		Door->GetMoverPhase(), EWoTMoverPhase::Closed);
	NPC->Destroy();
	WoTTest::TickWorld(&World, 1, PB::Step);

	AWoTCharacter* Bot = PB::SpawnAIDrivenBody(*this, World, Away);
	if (!Bot) return false;
	Bot->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Bot->SetActorLocation(Trigger->GetActorLocation(), false, nullptr,
		ETeleportType::TeleportPhysics);
	WoTTest::TickWorld(&World, 1, PB::Step);
	TestTrue(TEXT("an AI-driven player body in Trigger0 starts the door"),
		Door->GetMoverPhase() == EWoTMoverPhase::Opening
		|| Door->GetMoverPhase() == EWoTMoverPhase::Open);
	TestTrue(TEXT("the door opens fully while the body stays"),
		PB::TickUntilPhase(World, *Door, EWoTMoverPhase::Open, 3.0f));
	TestTrue(TEXT("the open door stands 195 units up (KeyPos 1)"),
		FMath::IsNearlyEqual(Door->GetActorLocation().Z, ClosedZ + 195.0, 1.0));
	return !HasAnyErrors();
}

// Arena_04's lift (Mover2: no authored InitialState or BumpType, so the mover
// defaults bump-open and player bump) stays down for an NPC body and starts when
// an AI-driven player body bumps it, or lands on it and stands there.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerBodyRealArena04LiftSpec,
	"WoT.World.PlayerBody.RealArena04LiftStartsForAIDrivenBody",
	WoTPlayerBodySpec::Flags)

bool FWoTPlayerBodyRealArena04LiftSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	PB::FBuiltMap Built;
	if (!PB::LoadAndBuild(*this, TEXT("Arena_04"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	AWoTMover* Lift = Built.Projected<AWoTMover>(TEXT("Arena_04.Mover2"));
	if (!TestNotNull(TEXT("Arena_04 projects Mover2"), Lift)) return false;
	TestEqual(TEXT("Mover2 is bump-open"), Lift->InitialState, EWoTMoverState::BumpOpenTimed);
	TestEqual(TEXT("Mover2 takes a player bump"), Lift->BumpType, EWoTMoverBumpType::PlayerBump);
	const FVector Away = PB::FarSpot(World, Lift->GetActorLocation());
	TestTrue(TEXT("a spot well away from Mover2"),
		FVector::Dist(Away, Lift->GetActorLocation()) > 1000.0);

	// Bumping it. The bodies hover where they are parked.
	AWoTCharacter* NPC = PB::SpawnNPCBody(*this, World, Away);
	AWoTCharacter* Bot = PB::SpawnAIDrivenBody(*this, World, Away + FVector(0, 0, 120));
	if (!NPC || !Bot) return false;
	Bot->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	PB::Bump(*Lift, *NPC);
	TestEqual(TEXT("an NPC body's bump leaves Mover2 down"),
		Lift->GetMoverPhase(), EWoTMoverPhase::Closed);
	NPC->Destroy();
	PB::Bump(*Lift, *Bot);
	TestEqual(TEXT("an AI-driven player body's bump starts Mover2"),
		Lift->GetMoverPhase(), EWoTMoverPhase::Opening);
	if (Lift->GetMoverPhase() != EWoTMoverPhase::Closed
		&& !TestTrue(TEXT("Mover2 rises, waits and comes back down"),
			PB::TickUntilPhase(World, *Lift, EWoTMoverPhase::Open, 3.0f)
			&& PB::TickUntilPhase(World, *Lift, EWoTMoverPhase::Closed, 4.0f)))
		return false;

	// Standing on it.
	AWoTCharacter* StandingNPC = PB::SpawnNPCBody(*this, World, Away + FVector(0, 0, 240));
	if (!StandingNPC) return false;
	TestTrue(TEXT("an NPC body stands on Mover2"),
		PB::DropOnto(*this, World, *Lift, *StandingNPC));
	TestEqual(TEXT("an NPC body standing on Mover2 leaves it down"),
		Lift->GetMoverPhase(), EWoTMoverPhase::Closed);
	StandingNPC->Destroy();
	WoTTest::TickWorld(&World, 1, PB::Step);
	TestTrue(TEXT("an AI-driven player body stands on Mover2"),
		PB::DropOnto(*this, World, *Lift, *Bot));
	TestTrue(TEXT("an AI-driven player body standing on Mover2 starts it"),
		Lift->GetMoverPhase() == EWoTMoverPhase::Opening
		|| Lift->GetMoverPhase() == EWoTMoverPhase::Open);
	return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Arena_11's updraft: a triggered zone current (WOTZoneInfo.uc:52-60, 104-128,
// 160-222). Its zone, WOTZoneInfo1, authors MaxZoneVelocity2=(-256,0,768),
// TransitionTime2=.5, initially on and TriggerControl, with the tag the air
// shield's reflector triggers on install and untriggers on uninstall
// (IgnoreElementReflector.uc:37-70; IgnoreAirElementReflector.uc
// TriggerEvent=ElementalAirTriggered). While on, the zone applies the steady
// average of the shipped per-tick cycle; while off, MinZoneVelocity2 (PORT_NOTES
// "A triggered zone current is steady"). The specs reach the zone's trigger
// settings and state by reflection, so they also build against the zone actor
// before this fix, where each one fails.

namespace WoTTriggeredCurrentSpec
{
namespace PB = WoTPlayerBodySpec;

const TCHAR* const Updraft = TEXT("Arena_11.WOTZoneInfo1");
// (Max + Min) / 2: Arena_11's per-tick step spans the whole range.
const FVector SteadyUpdraft(-128.0, 0.0, 384.0);
constexpr float Frame = 1.0f / 60.0f;

// The zone's on/off state by name ("On", "Off", "Unset"), or "absent" for a
// zone without one.
FString StateOf(const AWoTZoneVolume& Zone)
{
	const FEnumProperty* Property =
		FindFProperty<FEnumProperty>(Zone.GetClass(), TEXT("TriggeredCurrent"));
	if (!Property) return TEXT("absent");
	const int64 Value = Property->GetUnderlyingProperty()->GetSignedIntPropertyValue(
		Property->ContainerPtrToValuePtr<void>(&Zone));
	return Property->GetEnum()->GetNameStringByValue(Value);
}

bool SetZoneProperty(FAutomationTestBase& Test, AWoTZoneVolume& Zone,
	const TCHAR* Name, const TCHAR* Value)
{
	FProperty* Property = Zone.GetClass()->FindPropertyByName(Name);
	if (!Test.TestNotNull(FString(TEXT("zone property ")) + Name, Property)) return false;
	return Property->ImportText_Direct(Value, Property->ContainerPtrToValuePtr<void>(&Zone),
		&Zone, PPF_None) != nullptr;
}

// Gives Body a real air shield artifact and uses it as a player does: selected,
// then cast through the inventory's use path. It installs its reflector for
// the artifact's Duration.
AWoTAngrealInvAirShield* UseAirShield(FAutomationTestBase& Test, AWoTCharacter& Body)
{
	UWorld* World = Body.GetWorld();
	AWoTGameMode* Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	UWoTInventoryComponent* Inventory = Body.FindComponentByClass<UWoTInventoryComponent>();
	if (!Test.TestNotNull(TEXT("the server's game mode"), Game)
		|| !Test.TestNotNull(TEXT("the body's inventory"), Inventory)) return nullptr;
	Game->GivePawnAngreal(&Body, AWoTAngrealInvAirShield::StaticClass(), 3);
	AWoTAngrealInvAirShield* Shield = Cast<AWoTAngrealInvAirShield>(
		Inventory->FindInventoryType(AWoTAngrealInvAirShield::StaticClass()));
	if (!Test.TestNotNull(TEXT("a real air shield artifact"), Shield)) return nullptr;
	Inventory->SelectItem(Shield);
	Test.TestTrue(TEXT("the air shield casts"), Inventory->UseAngreal());
	Test.TestNotNull(TEXT("the air shield installs its reflector"),
		Body.FindComponentByClass<UWoTIgnoreAirElementReflector>());
	return Shield;
}

// The floor height under From, swept against the level with a small sphere: a
// bare line slips through narrow gaps in the shaft floor's collision that no
// body can.
bool TraceFloor(UWorld& World, const FVector& From, double& OutZ)
{
	FHitResult Hit;
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTTriggeredCurrentFloor), true);
	if (!World.SweepSingleByChannel(Hit, From, From - FVector(0, 0, 600), FQuat::Identity,
		ECC_Pawn, FCollisionShape::MakeSphere(12.0f), Params)) return false;
	OutZ = Hit.ImpactPoint.Z;
	return true;
}

// A human-controlled player body.
AWoTCharacter* SpawnPlayer(FAutomationTestBase& Test, UWorld& World, const FVector& Location)
{
	AWoTCharacter* Body = World.SpawnActor<AWoTCharacter>(
		Location, FRotator::ZeroRotator, PB::AlwaysSpawn());
	APlayerController* Controller = World.SpawnActor<APlayerController>();
	if (!Test.TestNotNull(TEXT("player body"), Body)
		|| !Test.TestNotNull(TEXT("player controller"), Controller)) return nullptr;
	Controller->SetAsLocalPlayerController();
	Controller->Possess(Body);
	Test.TestTrue(TEXT("a human-controlled body"), Body->IsPlayerControlled());
	return Body;
}

// One real map's zones and one of its player starts, decoded from the map's
// world payload and projected into a world as the release materializer projects
// a map before play. Every machine of a network spec projects the same records,
// so the server's zone and each client's zone are the same map actor.
struct FMapZones
{
	bool Load(FAutomationTestBase& Test, PB::FFixture& Fixture, FStringView Map,
		FStringView AwayFrom)
	{
		Defaults = Fixture.Defaults.Get();
		const FWoTWorldDescriptor* Descriptor = Fixture.Find(Map);
		if (!Test.TestNotNull(FString(Map) + TEXT(" is in the world set"), Descriptor))
			return false;
		const TArray<uint8>* Bytes = Fixture.Writer.Objects.Find(Descriptor->WorldLogicalId);
		if (!Test.TestNotNull(FString(Map) + TEXT(" world payload"), Bytes)) return false;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(*Bytes,
				TEXT("ue1-world-records-le-v6"), TEXT("world"));
		if (!Test.TestFalse(FString(Map) + TEXT(" world frame decodes"), Decoded.IsError())
			|| !Test.TestFalse(FString(Map) + TEXT(" world payload decodes"),
				Resource.LoadObjectForTesting(MoveTemp(Decoded).GetValue()).IsError()))
			return false;
		FVector Away = FVector::ZeroVector;
		for (const FWoTRuntimeWorldActor& Record : Resource.Actors())
			if (FStringView(Record.Identity).Equals(AwayFrom, ESearchCase::CaseSensitive))
				if (const FWoTRuntimeWorldProperty* P = WoTRuntimeWorldFindProperty(Record, TEXT("Location")))
					WoTRuntimeWorldPropertyVector(*P, Away);
		// The zone classes the projector materializes, and the player start
		// farthest from AwayFrom for the server's game mode to use.
		const FWoTRuntimeWorldActor* Start = nullptr;
		double Farthest = -1.0;
		for (const FWoTRuntimeWorldActor& Record : Resource.Actors())
		{
			const FStringView Class(Record.ClassIdentity);
			if (Class.Equals(TEXT("Engine.ZoneInfo"), ESearchCase::IgnoreCase)
				|| Class.Equals(TEXT("WOT.WOTZoneInfo"), ESearchCase::IgnoreCase)
				|| Class.Equals(TEXT("WOT.WaterZone"), ESearchCase::IgnoreCase))
			{
				Records.Add(Record);
				continue;
			}
			FVector Location;
			const FWoTRuntimeWorldProperty* P = WoTRuntimeWorldFindProperty(Record, TEXT("Location"));
			if ((Class.Equals(TEXT("Engine.PlayerStart"), ESearchCase::IgnoreCase)
					|| Defaults->ClassIsA(Class, TEXT("Engine.PlayerStart")))
				&& P && WoTRuntimeWorldPropertyVector(*P, Location)
				&& FVector::DistSquared(Location, Away) > Farthest)
			{
				Farthest = FVector::DistSquared(Location, Away);
				Start = &Record;
				StartLocation = Location;
			}
		}
		if (!Test.TestNotNull(FString(Map) + TEXT(" has a player start"), Start)) return false;
		Records.Add(*Start);
		return Test.TestTrue(FString(Map) + TEXT(" has zones"), Records.Num() > 1);
	}

	bool Project(UWorld& World, FString& Error) const
	{
		FWoTWorldProjectionSource Source;
		Source.MapIdentity = Resource.MapIdentity();
		Source.Records = Records;
		Source.Points = Resource.Points();
		Source.Nodes = Resource.Nodes();
		Source.Vertices = Resource.Vertices();
		Source.Zones = Resource.Zones();
		Source.Surfaces = Resource.Surfaces();
		Source.Leaves = Resource.Leaves();
		Source.bBSPRootOutside = Resource.BSPRootOutside();
		FWoTWorldProjectionResult Out;
		if (!FWoTWorldProjector::ProjectDeferred(World, Source, *Defaults, Out, Error)) return false;
		for (AActor* Actor : Out.DeferredActors)
			if (Actor) Actor->FinishSpawning(Actor->GetActorTransform(), true);
		// These network worlds have no level geometry. A floor under the start
		// keeps the players standing inside the map's zones: a body that falls
		// out of them dies (UWoTZoneIndex), as in the original.
		AActor* Floor = World.SpawnActor<AActor>(AActor::StaticClass(),
			FTransform(StartLocation - FVector(0, 0, 60)), PB::AlwaysSpawn());
		if (!Floor) { Error = TEXT("the spec floor could not be spawned"); return false; }
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
		Box->SetBoxExtent(FVector(600, 600, 10));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Floor->SetRootComponent(Box);
		Box->RegisterComponent();
		Floor->SetActorLocation(StartLocation - FVector(0, 0, 60));
		return true;
	}

	// The projected actor made from Identity's record in World.
	template <typename T>
	T* Find(UWorld* World, FStringView Identity) const
	{
		const int32 Index = Records.IndexOfByPredicate([Identity](const FWoTRuntimeWorldActor& Record)
		{
			return FStringView(Record.Identity).Equals(Identity, ESearchCase::CaseSensitive);
		});
		if (!World || Index == INDEX_NONE) return nullptr;
		const FString Prefix = FString::Printf(TEXT("WoTProjected_%d_"), Index);
		for (TActorIterator<T> It(World); It; ++It)
			if (It->GetName().StartsWith(Prefix)) return *It;
		return nullptr;
	}

	FWoTRuntimeWorldResource Resource;
	TArray<FWoTRuntimeWorldActor> Records;
	FVector StartLocation = FVector::ZeroVector;
	const FWoTRuntimeClassDefaults* Defaults = nullptr;
};
}

// Arena_11's updraft at runtime: on from the start with the steady current, off
// at once while a real air shield is up, on again when the shield expires. The
// current holds from tick to tick (no per-tick alternation).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FZoneVelocityRuntimeShieldSpec,
	"WoT.Zone.TriggeredVelocity.RuntimeShield", WoTPlayerBodySpec::Flags)
bool FZoneVelocityRuntimeShieldSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	namespace TC = WoTTriggeredCurrentSpec;
	PB::FBuiltMap Map;
	if (!PB::LoadAndBuild(*this, TEXT("Arena_11"), Map)) return !HasAnyErrors();
	UWorld& World = *Map.World;
	AWoTZoneVolume* Zone = Map.Projected<AWoTZoneVolume>(TC::Updraft);
	if (!TestNotNull(TEXT("Arena_11's updraft zone"), Zone)) return false;
	TestEqual(TEXT("the updraft starts on"), TC::StateOf(*Zone), FString(TEXT("On")));
	TestTrue(FString::Printf(TEXT("the updraft starts with the steady current: %s"),
		*Zone->ZoneVelocity.ToString()), Zone->ZoneVelocity.Equals(TC::SteadyUpdraft, 0.001));
	AWoTCharacter* Body = TC::SpawnPlayer(*this, World, PB::FarSpot(World, Zone->GetActorLocation()));
	if (!Body) return false;
	Body->GetCharacterMovement()->DisableMovement();
	bool bHeld = true;
	for (int32 Tick = 0; Tick < 8; ++Tick)
	{
		WoTTest::TickWorld(&World, 1, TC::Frame);
		bHeld &= Zone->ZoneVelocity.Equals(TC::SteadyUpdraft, 0.001);
	}
	TestTrue(TEXT("an on updraft applies the same current every tick"), bHeld);

	const double CastTime = World.GetTimeSeconds();
	if (!TC::UseAirShield(*this, *Body)) return false;
	TestEqual(TEXT("the air shield turns the updraft off"), TC::StateOf(*Zone), FString(TEXT("Off")));
	TestTrue(FString::Printf(TEXT("the current stops at once: %s"), *Zone->ZoneVelocity.ToString()),
		Zone->ZoneVelocity.IsNearlyZero());
	bHeld = true;
	for (int32 Tick = 0; Tick < 8; ++Tick)
	{
		WoTTest::TickWorld(&World, 1, TC::Frame);
		bHeld &= Zone->ZoneVelocity.IsNearlyZero();
	}
	TestTrue(TEXT("the updraft stays off every tick while the shield is up"), bHeld);

	// The shield lasts the artifact's authored Duration (20 s),
	// then uninstalls its reflector, which untriggers the zone.
	double OffUntil = CastTime;
	for (int32 Tick = 0; Tick < 300 && Body->FindComponentByClass<UWoTIgnoreAirElementReflector>(); ++Tick)
	{
		bHeld &= Zone->ZoneVelocity.IsNearlyZero();
		WoTTest::TickWorld(&World, 1, 0.1f);
		OffUntil = World.GetTimeSeconds();
	}
	TestTrue(TEXT("the updraft stays off for the whole shield"), bHeld);
	TestFalse(TEXT("the shield expires"), Body->FindComponentByClass<UWoTIgnoreAirElementReflector>() != nullptr);
	TestTrue(FString::Printf(TEXT("the shield lasted 20 s: %.2f"), OffUntil - CastTime),
		FMath::IsNearlyEqual(OffUntil - CastTime, 20.0, 0.2));
	TestEqual(TEXT("the expired shield turns the updraft back on"), TC::StateOf(*Zone), FString(TEXT("On")));
	TestTrue(FString::Printf(TEXT("the steady current returns: %s"), *Zone->ZoneVelocity.ToString()),
		Zone->ZoneVelocity.Equals(TC::SteadyUpdraft, 0.001));
	return !HasAnyErrors();
}

// A falling body in the shaft rises while the updraft is on and falls while a
// real air shield holds it off.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FZoneVelocityShaftBodySpec,
	"WoT.Zone.TriggeredVelocity.ShaftBody", WoTPlayerBodySpec::Flags)
bool FZoneVelocityShaftBodySpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	namespace TC = WoTTriggeredCurrentSpec;
	PB::FBuiltMap Map;
	if (!PB::LoadAndBuild(*this, TEXT("Arena_11"), Map)) return !HasAnyErrors();
	AWoTZoneVolume* Zone = Map.Projected<AWoTZoneVolume>(TC::Updraft);
	if (!TestNotNull(TEXT("shaft zone"), Zone)) return false;
	const FVector Start(1920, -64, -500);
	TestTrue(TEXT("body starts inside compiled shaft zone"), Zone->ContainsPoint(Start));
	AWoTCharacter* Player = TC::SpawnPlayer(*this, *Map.World, Start);
	if (!Player) return false;
	auto Fall = [&]()
	{
		Player->SetActorLocation(Start, false, nullptr, ETeleportType::TeleportPhysics);
		Player->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		Player->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		WoTTest::TickWorld(Map.World, 6, TC::Frame);
		return Player->GetActorLocation().Z - Start.Z;
	};
	const double OnRise = Fall();
	TestTrue(FString::Printf(TEXT("falling body rises while on: %.3f"), OnRise), OnRise > 10);
	if (!TC::UseAirShield(*this, *Player)) return false;
	const double OffRise = Fall();
	TestTrue(FString::Printf(TEXT("body falls while the air shield holds the updraft off: %.3f"), OffRise),
		OffRise < -1);
	if (UWoTIgnoreAirElementReflector* Reflector = Player->FindComponentByClass<UWoTIgnoreAirElementReflector>())
		Reflector->Uninstall();
	const double RestoredRise = Fall();
	TestTrue(FString::Printf(TEXT("body rises again once the shield is gone: %.3f"), RestoredRise),
		RestoredRise > 10);
	return !HasAnyErrors();
}

// The updraft was tuned so that a jump from the floor of Arena_11's shaft just
// clears the ring. A body standing on that pedestal jumps and steers toward the
// ring with ordinary input and air control: while the updraft is on it rises to
// ring height and walks out onto the ring; while a real air shield holds the
// updraft off it cannot. (Jump 325 and gravity 950 give about 56 units of rise on
// their own and about 265 with the steady current.) The shaft's -X wall is a
// ladder (a climbable surface), which a body can climb out on whatever the
// current, so the jump steers toward the +X side, an ordinary wall topped by
// the ledge and steps that rise 224, 240 and 256 units above the pedestal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FZoneVelocityPedestalJumpSpec,
	"WoT.Movement.TriggeredVelocity.PedestalJump", WoTPlayerBodySpec::Flags)
bool FZoneVelocityPedestalJumpSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	namespace TC = WoTTriggeredCurrentSpec;
	PB::FBuiltMap Map;
	if (!PB::LoadAndBuild(*this, TEXT("Arena_11"), Map)) return !HasAnyErrors();
	UWorld& World = *Map.World;
	AWoTZoneVolume* Zone = Map.Projected<AWoTZoneVolume>(TC::Updraft);
	if (!TestNotNull(TEXT("Arena_11's updraft zone"), Zone)) return false;
	// The shaft is centred on Brush73 (Arena_11.t3d: 1920,-64).
	double Pedestal = 0, Ring = 0;
	if (!TestTrue(TEXT("the shaft has a floor"), TC::TraceFloor(World, FVector(1960, -64, -600), Pedestal))
		|| !TestTrue(TEXT("the ring has a floor"), TC::TraceFloor(World, FVector(2060, -64, -300), Ring)))
		return false;
	AddInfo(FString::Printf(TEXT("pedestal floor %.2f, ring floor %.2f"), Pedestal, Ring));
	TestTrue(FString::Printf(TEXT("the ring stands 256 units above the pedestal: %.2f"), Ring - Pedestal),
		FMath::IsNearlyEqual(Ring - Pedestal, 256.0, 1.0));
	FWoTWalkSurfaceHit Ladder, Wall;
	const UWoTMapLightTable* Surfaces = UWoTMeshLight::FindMapLightTable(&World);
	const FVector Middle(1920, -64, -600);
	TestTrue(TEXT("the shaft's -X wall is climbable"), UWoTCharacterMovement::TraceWalkSurface(
		Surfaces, &World, nullptr, Middle, Middle - FVector(400, 0, 0), Ladder) && Ladder.Friction == 10.0f);
	TestTrue(TEXT("the shaft's +X wall is an ordinary wall"), UWoTCharacterMovement::TraceWalkSurface(
		Surfaces, &World, nullptr, Middle, Middle + FVector(400, 0, 0), Wall) && Wall.Friction == 1.0f);
	const FVector Stand(1960, -64, Pedestal + 60);
	AWoTCharacter* Body = TC::SpawnPlayer(*this, World, Stand);
	if (!Body) return false;
	UCharacterMovementComponent* Move = Body->GetCharacterMovement();
	const double HalfHeight = Body->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	struct FJump { double Rise = 0; bool bStood = false; FVector End; };
	auto JumpFromPedestal = [&](const TCHAR* Label)
	{
		FJump Result;
		Body->SetActorLocation(FVector(Stand.X, Stand.Y, Pedestal + HalfHeight + 2.0), false, nullptr,
			ETeleportType::TeleportPhysics);
		Move->Velocity = FVector::ZeroVector;
		Move->SetMovementMode(MOVE_Walking);
		WoTTest::TickWorld(&World, 6, TC::Frame);
		Result.bStood = Move->IsMovingOnGround() && Zone->ContainsPoint(Body->GetActorLocation());
		Body->Jump();
		for (int32 Tick = 0; Tick < 150; ++Tick)
		{
			Body->AddMovementInput(FVector(1, 0, 0));
			WoTTest::TickWorld(&World, 1, TC::Frame);
			Body->StopJumping();
			Result.Rise = FMath::Max(Result.Rise, Body->GetActorLocation().Z - HalfHeight - Pedestal);
		}
		Result.End = Body->GetActorLocation();
		AddInfo(FString::Printf(TEXT("jump from the pedestal, %s: rise %.2f, end %s, walking %d"),
			Label, Result.Rise, *Result.End.ToString(), Move->IsMovingOnGround() ? 1 : 0));
		return Result;
	};

	TestEqual(TEXT("the updraft is on"), TC::StateOf(*Zone), FString(TEXT("On")));
	const FJump On = JumpFromPedestal(TEXT("updraft on"));
	TestTrue(TEXT("the body stands on the pedestal inside the updraft"), On.bStood);
	TestTrue(TEXT("with the updraft on, the jump reaches ring height"), On.Rise >= 240.0);
	TestTrue(TEXT("with the updraft on, the body walks out onto the ring"),
		Move->IsMovingOnGround() && On.End.X > 2000.0
		&& FMath::IsNearlyEqual(On.End.Z - HalfHeight, Ring, 4.0));

	WoTTest::TickWorld(&World, 1, TC::Frame);
	if (!TC::UseAirShield(*this, *Body)) return false;
	TestEqual(TEXT("the air shield turns the updraft off"), TC::StateOf(*Zone), FString(TEXT("Off")));
	const FJump Off = JumpFromPedestal(TEXT("air shield up"));
	TestTrue(TEXT("the body stands on the pedestal again"), Off.bStood);
	TestTrue(TEXT("with the updraft off, the jump stays far below the ring"), Off.Rise < 100.0);
	TestTrue(TEXT("with the updraft off, the body lands back on the pedestal"),
		Move->IsMovingOnGround() && Off.End.X < 2000.0
		&& FMath::IsNearlyEqual(Off.End.Z - HalfHeight, Pedestal, 4.0));
	return !HasAnyErrors();
}

// A person's way through Arena_11's shaft, with the path kept in the log for
// walking proposals: from the ring, a body that steps into the updraft rides it
// back out onto the ring; with an air shield up it drops to the bottom and walks
// onto the artifact waiting there (the drop ends against the shaft's -X wall,
// beyond the pool's touch, so it walks over as a person would); with the
// updraft on again, a body standing at the bottom stays there (walking transport
// is horizontal) and jumps toward the -X wall, a ladder, out onto the ring.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FZoneVelocityWalkingProbeSpec,
	"WoT.Movement.TriggeredVelocity.ShaftTraversal", WoTPlayerBodySpec::Flags)
bool FZoneVelocityWalkingProbeSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	namespace TC = WoTTriggeredCurrentSpec;
	PB::FBuiltMap Map;
	if (!PB::LoadAndBuild(*this, TEXT("Arena_11"), Map)) return !HasAnyErrors();
	AWoTZoneVolume* Zone = Map.Projected<AWoTZoneVolume>(TC::Updraft);
	if (!TestNotNull(TEXT("shaft zone"), Zone)) return false;
	AWoTAngrealSpawnPool* BottomPool = Map.Projected<AWoTAngrealSpawnPool>(TEXT("Arena_11.RareAngrealSpawnPool7"));
	if (!TestNotNull(TEXT("bottom pickup pool"), BottomPool)
		|| !TestNotNull(TEXT("bottom artifact"), BottomPool->PickupItem.Get())) return false;
	AWoTInventory* BottomItem = BottomPool->PickupItem;
	const FVector PoolSpot = BottomPool->GetActorLocation();
	AWoTCharacter* Player = TC::SpawnPlayer(*this, *Map.World, FVector(2112,-64,-396));
	if (!Player) return false;
	UCharacterMovementComponent* Move = Player->GetCharacterMovement();
	UWoTInventoryComponent* Inventory = Player->FindComponentByClass<UWoTInventoryComponent>();
	auto WalkIn = [&](const TCHAR* Label)
	{
		Player->SetActorLocation(FVector(2112,-64,-396), false, nullptr, ETeleportType::TeleportPhysics);
		Move->Velocity = FVector::ZeroVector;
		Move->SetMovementMode(MOVE_Walking);
		bool Entered = false, Rose = false, Bottom = false;
		double MinZ = Player->GetActorLocation().Z, MaxZ = MinZ, Closest = TNumericLimits<double>::Max();
		for (int32 Frame = 0; Frame < 240; ++Frame)
		{
			const FVector Before = Player->GetActorLocation();
			if (Before.Z < -620)
			{
				// Once down, walk onto the artifact, as a person would: the
				// fall's carried momentum can leave the body beside it.
				const double Offset = PoolSpot.X - Before.X;
				if (FMath::Abs(Offset) > 8) Player->AddMovementInput(FVector(FMath::Sign(Offset),0,0));
			}
			else if (Before.X > 1930) Player->AddMovementInput(FVector(-1,0,0));
			WoTTest::TickWorld(Map.World, 1, TC::Frame);
			const FVector At = Player->GetActorLocation();
			Entered |= Zone->ContainsPoint(At);
			MinZ = FMath::Min(MinZ, At.Z); MaxZ = FMath::Max(MaxZ, At.Z);
			Rose |= Entered && At.Z > -390;
			Bottom |= At.Z < -620 && FVector::Dist2D(At, PoolSpot) < 100;
			Closest = FMath::Min(Closest, FVector::Dist(At, PoolSpot));
		}
		AddInfo(FString::Printf(TEXT("shaft traversal %s: entered=%d rose=%d bottom=%d z=[%.2f,%.2f] end=%s mode=%d closest-to-artifact=%.2f"),
			Label, Entered, Rose, Bottom, MinZ, MaxZ, *Player->GetActorLocation().ToString(),
			int32(Move->MovementMode), Closest));
		TestTrue(FString(Label) + TEXT(" can step from ring into shaft"), Entered);
		if (FString(Label) == TEXT("on"))
			TestTrue(TEXT("current lifts a body stepping off the ring back out"),
				Rose && Player->GetActorLocation().Z > -410);
		return Bottom;
	};
	WalkIn(TEXT("on"));
	WoTTest::TickWorld(Map.World, 1, TC::Frame);
	if (!TC::UseAirShield(*this, *Player)) return false;
	TestTrue(TEXT("shield permits descent to the bottom pickup"), WalkIn(TEXT("shield off")));
	FString Held;
	for (const AWoTInventory* Item : Inventory->Items) Held += (Item ? Item->GetName() : FString(TEXT("null"))) + TEXT(" ");
	AddInfo(FString::Printf(TEXT("after the descent: health=%d inventory=[%s] pool item=%s"),
		Player->Health, *Held, *GetNameSafe(BottomPool->PickupItem.Get())));
	TestTrue(TEXT("human player receives the bottom artifact"), Inventory->Items.Contains(BottomItem));
	if (UWoTIgnoreAirElementReflector* Reflector = Player->FindComponentByClass<UWoTIgnoreAirElementReflector>())
		Reflector->Uninstall();
	TestEqual(TEXT("removing the shield turns the updraft back on"), TC::StateOf(*Zone), FString(TEXT("On")));
	Player->SetActorLocation(FVector(1920,-64,-655), false, nullptr, ETeleportType::TeleportPhysics);
	Move->Velocity = FVector::ZeroVector; Move->SetMovementMode(MOVE_Walking);
	WoTTest::TickWorld(Map.World, 12, TC::Frame);
	AddInfo(FString::Printf(TEXT("grounded bottom with current on: %s mode=%d"),
		*Player->GetActorLocation().ToString(), int32(Move->MovementMode)));
	TestTrue(TEXT("current does not auto-lift a grounded walker"), Player->GetActorLocation().Z < -640);
	Player->Jump();
	double JumpMaxZ = Player->GetActorLocation().Z;
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		Player->AddMovementInput(FVector(-1,0,0));
		WoTTest::TickWorld(Map.World, 1, TC::Frame);
		Player->StopJumping();
		JumpMaxZ = FMath::Max(JumpMaxZ, Player->GetActorLocation().Z);
	}
	AddInfo(FString::Printf(TEXT("jump from bottom: maxZ=%.2f end=%s mode=%d"),
		JumpMaxZ, *Player->GetActorLocation().ToString(), int32(Move->MovementMode)));
	TestTrue(TEXT("jumping from bottom permits an exit onto the ring"),
		JumpMaxZ > -410 && Move->IsMovingOnGround() && Player->GetActorLocation().X < 1792);
	return !HasAnyErrors();
}

// The projection reads each trigger setting from the record or the class
// default, and the projected zone starts in the authored state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FZoneVelocityProjectionPropertiesSpec,
	"WoT.World.TriggeredVelocity.ProjectionProperties", WoTPlayerBodySpec::Flags)
bool FZoneVelocityProjectionPropertiesSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	namespace TC = WoTTriggeredCurrentSpec;
	if (!WoTReferenceTreeExists()) { AddWarning(WoTReferenceMissingWarning(TEXT("original"))); return true; }
	PB::FFixture& Fixture = PB::TheFixture();
	if (!Fixture.Load(*this)) return false;
	for (bool Authored : {false, true})
	{
		FWoTRuntimeWorldActor Record;
		Record.Identity = TEXT("VelocitySpec.Zone");
		Record.ClassIdentity = TEXT("WOT.WOTZoneInfo");
		auto Add = [&](const TCHAR* Name, EWoTRuntimeWorldPropertyType Type,
			const void* Data, int32 Size, const TCHAR* NameValue = TEXT(""))
		{
			FWoTRuntimeWorldProperty& P = Record.Properties.AddDefaulted_GetRef();
			P.Name = Name; P.Type = Type; P.NameValue = NameValue;
			if (Type == EWoTRuntimeWorldPropertyType::Struct) P.StructName = TEXT("Vector");
			if (Size) P.Value.Append(static_cast<const uint8*>(Data), Size);
		};
		if (Authored)
		{
			const uint8 True = 1;
			const float Maximum[3] = {60, -120, 240}, Minimum[3] = {-20, 40, -80};
			const float Transition = 0.75f, Duration = 0.25f;
			Add(TEXT("bTrigZoneVelo2"), EWoTRuntimeWorldPropertyType::Boolean, &True, 1);
			Add(TEXT("bTrigZoneVelo2InitiallyOn"), EWoTRuntimeWorldPropertyType::Boolean, &True, 1);
			Add(TEXT("MaxZoneVelocity2"), EWoTRuntimeWorldPropertyType::Struct, Maximum, sizeof(Maximum));
			Add(TEXT("MinZoneVelocity2"), EWoTRuntimeWorldPropertyType::Struct, Minimum, sizeof(Minimum));
			Add(TEXT("TransitionTime2"), EWoTRuntimeWorldPropertyType::Float, &Transition, sizeof(Transition));
			Add(TEXT("TriggerDuration"), EWoTRuntimeWorldPropertyType::Float, &Duration, sizeof(Duration));
			Add(TEXT("InitialState"), EWoTRuntimeWorldPropertyType::Name, nullptr, 0, TEXT("TriggerControl"));
			Add(TEXT("Tag"), EWoTRuntimeWorldPropertyType::Name, nullptr, 0, TEXT("VelocityEvent"));
		}
		TArray<FWoTRuntimeWorldActor> Records = {Record};
		TArray<FVector3f> Points = {FVector3f(-100), FVector3f(100)};
		TArray<FWoTRuntimeWorldVertex> Vertices;
		Vertices.AddDefaulted_GetRef().Point = 0;
		Vertices.AddDefaulted_GetRef().Point = 1;
		TArray<FWoTRuntimeWorldNode> Nodes;
		FWoTRuntimeWorldNode& Node = Nodes.AddDefaulted_GetRef();
		Node.VertexPool = 0; Node.VertexCount = 2; Node.Zones[0] = 1;
		TArray<FWoTRuntimeWorldZone> Zones; Zones.SetNum(2); Zones[1].ActorIdentity = Record.Identity;
		FWoTWorldProjectionSource Source;
		Source.MapIdentity = TEXT("VelocitySpec"); Source.Records = Records;
		Source.Points = Points; Source.Vertices = Vertices; Source.Nodes = Nodes; Source.Zones = Zones;
		UWorld* World = WoTTest::CreateSpecWorld();
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		FWoTWorldProjectionResult Out; FString Error;
		if (!TestTrue(TEXT("zone projects"), FWoTWorldProjector::ProjectDeferred(
			*World, Source, *Fixture.Defaults, Out, Error))) { AddError(Error); return false; }
		AWoTZoneVolume* Zone = nullptr;
		for (AActor* Actor : Out.DeferredActors)
		{
			if (AWoTZoneVolume* Candidate = Cast<AWoTZoneVolume>(Actor)) Zone = Candidate;
			Actor->FinishSpawning(Actor->GetActorTransform(), true);
		}
		if (!TestNotNull(TEXT("projected zone"), Zone)) return false;
		for (const TCHAR* Name : {TEXT("bTrigZoneVelo2"), TEXT("bTrigZoneVelo2InitiallyOn"),
			TEXT("MaxZoneVelocity2"), TEXT("MinZoneVelocity2"), TEXT("TransitionTime2"),
			TEXT("TriggerDuration"), TEXT("InitialState"), TEXT("TagName")})
			TestNotNull(FString(TEXT("projected configuration exposes ")) + Name, Zone->GetClass()->FindPropertyByName(Name));
		if (Authored)
		{
			// A .75 transition steps past the whole range each tick, so "on" is
			// the midpoint of Max and Min.
			const FVector Steady(20, -40, 80), Minimum(-20, 40, -80);
			TestEqual(TEXT("initially on starts on"), TC::StateOf(*Zone), FString(TEXT("On")));
			TestTrue(FString::Printf(TEXT("initially on applies the steady current: %s"),
				*Zone->ZoneVelocity.ToString()), Zone->ZoneVelocity.Equals(Steady, 0.001));
			World->GetSubsystem<UWoTEventSubsystem>()->FireEvent(TEXT("VelocityEvent"), nullptr, nullptr);
			TestTrue(FString::Printf(TEXT("the tag's trigger switches to the minimum at once: %s"),
				*Zone->ZoneVelocity.ToString()), Zone->ZoneVelocity.Equals(Minimum, 0.001));
			Zone->Tick(0.2f);
			TestTrue(TEXT("off holds the minimum"), Zone->ZoneVelocity.Equals(Minimum, 0.001));
			World->GetSubsystem<UWoTEventSubsystem>()->FireUnEvent(TEXT("VelocityEvent"), nullptr, nullptr);
			TestTrue(FString::Printf(TEXT("the untrigger restores the steady current at once: %s"),
				*Zone->ZoneVelocity.ToString()), Zone->ZoneVelocity.Equals(Steady, 0.001));
			Zone->Tick(0.001f);
			TestTrue(TEXT("on holds the steady current"), Zone->ZoneVelocity.Equals(Steady, 0.001));
		}
		else TestTrue(TEXT("omitted settings leave the default current at zero"), Zone->ZoneVelocity.IsZero());
		for (const TCHAR* Name : {TEXT("bTrigZoneVelo2"), TEXT("bTrigZoneVelo2InitiallyOn")})
			if (FBoolProperty* P = FindFProperty<FBoolProperty>(Zone->GetClass(), Name))
				TestEqual(FString(Name) + TEXT(" resolves instance or class default"), P->GetPropertyValue_InContainer(Zone), Authored);
		for (const TCHAR* Name : {TEXT("TransitionTime2"), TEXT("TriggerDuration")})
			if (FFloatProperty* P = FindFProperty<FFloatProperty>(Zone->GetClass(), Name))
				TestEqual(FString(Name) + TEXT(" resolves instance or class default"), P->GetPropertyValue_InContainer(Zone),
					Authored ? (FString(Name) == TEXT("TransitionTime2") ? 0.75f : 0.25f) : 0.0f);
		for (const TCHAR* Name : {TEXT("MaxZoneVelocity2"), TEXT("MinZoneVelocity2")})
			if (FStructProperty* P = FindFProperty<FStructProperty>(Zone->GetClass(), Name))
				TestEqual(FString(Name) + TEXT(" resolves instance or class default"), *P->ContainerPtrToValuePtr<FVector>(Zone),
					Authored ? (FString(Name) == TEXT("MaxZoneVelocity2") ? FVector(60,-120,240) : FVector(-20,40,-80)) : FVector::ZeroVector);
		if (FNameProperty* P = FindFProperty<FNameProperty>(Zone->GetClass(), TEXT("InitialState")))
			TestEqual(TEXT("state resolves instance or class default"), P->GetPropertyValue_InContainer(Zone),
				Authored ? FName(TEXT("TriggerControl")) : NAME_None);
		if (FNameProperty* P = FindFProperty<FNameProperty>(Zone->GetClass(), TEXT("TagName")); P && Authored)
			TestEqual(TEXT("the tag comes from the record"), P->GetPropertyValue_InContainer(Zone),
				FName(TEXT("VelocityEvent")));
	}
	return !HasAnyErrors();
}

// The trigger states as WOTZoneInfo.uc:160-222 ships them, on a zone whose
// transition (4) is longer than one step: TriggerControl turns on while
// triggered and restores on UnTrigger, TriggerToggle flips on each Trigger and
// ignores UnTrigger, TriggerTimed turns on and restores when its timer (re-armed
// by each Trigger) runs out, and a zone in no trigger state ignores the event.
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FZoneVelocityStatesSpec,
	"WoT.Zone.TriggeredVelocity.States", WoTPlayerBodySpec::Flags)
void FZoneVelocityStatesSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* State : {TEXT("TriggerControl"), TEXT("TriggerToggle"), TEXT("TriggerTimed"), TEXT("None")})
	{
		Names.Add(State); Commands.Add(State);
	}
}
bool FZoneVelocityStatesSpec::RunTest(const FString& State)
{
	namespace TC = WoTTriggeredCurrentSpec;
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	FActorSpawnParameters Params = WoTPlayerBodySpec::AlwaysSpawn(); Params.bDeferConstruction = true;
	AWoTZoneVolume* Zone = World->SpawnActor<AWoTZoneVolume>(Params);
	bool Configured = TC::SetZoneProperty(*this, *Zone, TEXT("bTrigZoneVelo2"), TEXT("True"));
	Configured &= TC::SetZoneProperty(*this, *Zone, TEXT("MaxZoneVelocity2"), TEXT("(X=80,Y=-40,Z=120)"));
	Configured &= TC::SetZoneProperty(*this, *Zone, TEXT("TransitionTime2"), TEXT("4"));
	Configured &= TC::SetZoneProperty(*this, *Zone, TEXT("TriggerDuration"), TEXT("0.1"));
	Configured &= TC::SetZoneProperty(*this, *Zone, TEXT("InitialState"), *State);
	Configured &= TC::SetZoneProperty(*this, *Zone, TEXT("TagName"), TEXT("VelocityEvent"));
	Zone->FinishSpawning(FTransform::Identity, true);
	if (!Configured) return false;
	// A step of a quarter of the range: the shipped "on" cycle is Max, Max - Max/4,
	// whose average is Max - Max/8.
	const FVector On(70, -35, 105), Off = FVector::ZeroVector;
	TestEqual(TEXT("initially off applies the minimum"), Zone->ZoneVelocity, Off);
	UWoTEventSubsystem* Events = World->GetSubsystem<UWoTEventSubsystem>();
	Events->FireEvent(TEXT("VelocityEvent"), nullptr, nullptr);
	TestEqual(TEXT("only the trigger states turn on, at once"), Zone->ZoneVelocity,
		State == TEXT("None") ? Off : On);
	WoTTest::TickWorld(World, 3);
	TestEqual(TEXT("the switched current holds across ticks"), Zone->ZoneVelocity,
		State == TEXT("None") ? Off : On);
	if (State == TEXT("TriggerControl"))
	{
		Events->FireEvent(TEXT("VelocityEvent"), nullptr, nullptr);
		TestEqual(TEXT("repeated control trigger does not toggle"), Zone->ZoneVelocity, On);
		Events->FireUnEvent(TEXT("VelocityEvent"), nullptr, nullptr);
		TestEqual(TEXT("control untrigger restores initially off"), Zone->ZoneVelocity, Off);
	}
	else if (State == TEXT("TriggerToggle"))
	{
		Events->FireUnEvent(TEXT("VelocityEvent"), nullptr, nullptr);
		TestEqual(TEXT("toggle ignores untrigger"), Zone->ZoneVelocity, On);
		Events->FireEvent(TEXT("VelocityEvent"), nullptr, nullptr);
		TestEqual(TEXT("second trigger turns it off"), Zone->ZoneVelocity, Off);
	}
	else if (State == TEXT("TriggerTimed"))
	{
		Events->FireUnEvent(TEXT("VelocityEvent"), nullptr, nullptr);
		TestEqual(TEXT("timed ignores untrigger"), Zone->ZoneVelocity, On);
		Events->FireEvent(TEXT("VelocityEvent"), nullptr, nullptr);
		WoTTest::TickWorld(World, 0.07f);
		TestEqual(TEXT("timed retrigger rearms duration"), Zone->ZoneVelocity, On);
		WoTTest::TickWorld(World, 0.07f);
		TestEqual(TEXT("timer restores initial state"), Zone->ZoneVelocity, Off);
	}
	return !HasAnyErrors();
}

#if WITH_EDITOR
// Arena_11's updraft over the network. Each machine materializes the map's
// zones before play, as a release build does, so the server's updraft and each
// client's are the same map actor, and only the on/off state replicates. A
// connected client sees a real air shield on the remote player turn the updraft
// off; a client that joins while it is off sees it off; when the shield expires
// both see the steady current again, and every machine holds the same current
// from tick to tick.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FZoneVelocityNetShieldSpec,
	"WoT.Net.TriggeredVelocity.ShieldState", WoTPlayerBodySpec::Flags)
bool FZoneVelocityNetShieldSpec::RunTest(const FString&)
{
	namespace PB = WoTPlayerBodySpec;
	namespace TC = WoTTriggeredCurrentSpec;
	if (!WoTReferenceTreeExists()) { AddWarning(WoTReferenceMissingWarning(TEXT("original"))); return true; }
	PB::FFixture& Fixture = PB::TheFixture();
	if (!Fixture.Load(*this)) return false;
	TC::FMapZones Arena;
	if (!Arena.Load(*this, Fixture, TEXT("Arena_11"), TC::Updraft)) return false;
	bool bOwnsActivation = false;
	if (!IsWoTPayloadBootstrapActive())
	{
		bOwnsActivation = !PublishWoTPayloadBootstrapActivationForTesting().IsError()
			&& !PublishWoTBootstrapWorldReadyForTesting().IsError();
	}
	ON_SCOPE_EXIT { if (bOwnsActivation) ResetWoTPayloadActivationForTesting(); };
	TArray<TWeakObjectPtr<UWorld>> Materialized;
	FString MaterializeError;
	const FDelegateHandle Hook = FWorldDelegates::OnPostWorldInitialization.AddLambda(
		[&](UWorld* World, const UWorld::InitializationValues)
		{
			FString Error;
			if (World && World->IsGameWorld())
			{
				if (Arena.Project(*World, Error)) Materialized.Add(World);
				else MaterializeError += Error;
			}
		});
	ON_SCOPE_EXIT { FWorldDelegates::OnPostWorldInitialization.Remove(Hook); };

	UE::Net::FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"), TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("a triggered zone current reaches connected and late-joining clients"));
	if (!TestTrue(TEXT("server loads"), Worlds.Server.IsLoaded())) return false;
	if (!TestTrue(TEXT("client connects"), Worlds.CreateAndConnectClient())) return false;
	TestTrue(FString(TEXT("each world materializes the map's zones ")) + MaterializeError,
		MaterializeError.IsEmpty() && Materialized.Contains(Worlds.Server.GetWorld())
		&& Materialized.Contains(Worlds.Clients[0].GetWorld()));
	AWoTZoneVolume* ServerZone = Arena.Find<AWoTZoneVolume>(Worlds.Server.GetWorld(), TC::Updraft);
	AWoTZoneVolume* FirstZone = Arena.Find<AWoTZoneVolume>(Worlds.Clients[0].GetWorld(), TC::Updraft);
	if (!TestNotNull(TEXT("the server's updraft"), ServerZone)
		|| !TestNotNull(TEXT("the connected client's updraft"), FirstZone)) return false;
	auto Matches = [&](const AWoTZoneVolume* Client, const TCHAR* State, const FVector& Current)
	{
		return TC::StateOf(*ServerZone) == State && TC::StateOf(*Client) == State
			&& ServerZone->ZoneVelocity.Equals(Current, 0.001)
			&& Client->ZoneVelocity.Equals(Current, 0.001);
	};
	auto Describe = [&](const AWoTZoneVolume* Client)
	{
		return FString::Printf(TEXT("server %s %s, client %s %s"),
			*TC::StateOf(*ServerZone), *ServerZone->ZoneVelocity.ToString(),
			*TC::StateOf(*Client), *Client->ZoneVelocity.ToString());
	};
	TestTrue(TEXT("both machines start with the updraft on"), Worlds.TickUntil([&]()
	{
		return Matches(FirstZone, TEXT("On"), TC::SteadyUpdraft);
	}, 60));
	TestTrue(TEXT("the client's own map zone is the server's"),
		Worlds.FindReplicatedObjectOnClient(ServerZone, 0) == FirstZone);
	TestFalse(TEXT("the client's zone follows the server"), FirstZone->HasAuthority());

	APlayerController* ServerController = Worlds.GetServerPlayerControllerOfClient(0);
	AWoTCharacter* Body = ServerController ? Cast<AWoTCharacter>(ServerController->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("the remote player's body"), Body)) return false;
	if (!TC::UseAirShield(*this, *Body)) return false;
	TestEqual(TEXT("the server turns the updraft off at once"), TC::StateOf(*ServerZone), FString(TEXT("Off")));
	TestTrue(TEXT("the connected client sees the updraft off"), Worlds.TickUntil([&]()
	{
		return Matches(FirstZone, TEXT("Off"), FVector::ZeroVector);
	}, 60));
	AddInfo(TEXT("connected client, shield up: ") + Describe(FirstZone));

	// Free the start for the second player, then join while the updraft is off.
	Body->SetActorLocation(Body->GetActorLocation() + FVector(0, 250, 0));
	if (!TestTrue(TEXT("a second client joins while the updraft is off"), Worlds.CreateAndConnectClient()))
		return false;
	TestTrue(TEXT("the late client's world materializes the map's zones"),
		Materialized.Contains(Worlds.Clients[1].GetWorld()));
	AWoTZoneVolume* LateZone = Arena.Find<AWoTZoneVolume>(Worlds.Clients[1].GetWorld(), TC::Updraft);
	if (!TestNotNull(TEXT("the late client's updraft"), LateZone)) return false;
	TestTrue(TEXT("the late client sees the updraft off"), Worlds.TickUntil([&]()
	{
		return Matches(LateZone, TEXT("Off"), FVector::ZeroVector)
			&& Matches(FirstZone, TEXT("Off"), FVector::ZeroVector);
	}, 60));
	AddInfo(TEXT("late client, shield up: ") + Describe(LateZone));
	TestTrue(TEXT("the late client's own map zone is the server's"),
		Worlds.FindReplicatedObjectOnClient(ServerZone, 1) == LateZone);
	bool bHeld = true;
	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		Worlds.TickAll();
		bHeld &= Matches(FirstZone, TEXT("Off"), FVector::ZeroVector)
			&& Matches(LateZone, TEXT("Off"), FVector::ZeroVector);
	}
	TestTrue(TEXT("every machine holds the updraft off"), bHeld);

	TestTrue(TEXT("when the shield expires, both clients see the steady current"), Worlds.TickAllUntil([&]()
	{
		return !Body->FindComponentByClass<UWoTIgnoreAirElementReflector>()
			&& Matches(FirstZone, TEXT("On"), TC::SteadyUpdraft)
			&& Matches(LateZone, TEXT("On"), TC::SteadyUpdraft);
	}, 0.25f, 120));
	AddInfo(TEXT("after the shield: ") + Describe(FirstZone) + TEXT("; late ") + Describe(LateZone));
	bHeld = true;
	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		Worlds.TickAll();
		bHeld &= Matches(FirstZone, TEXT("On"), TC::SteadyUpdraft)
			&& Matches(LateZone, TEXT("On"), TC::SteadyUpdraft);
	}
	TestTrue(TEXT("every machine holds the steady current"), bHeld);
	return !HasAnyErrors();
}
#endif

#endif // WITH_AUTOMATION_TESTS
