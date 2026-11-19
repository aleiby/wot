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
// The real-map specs materialize Arena_02 and Arena_04 the way a release build
// does (the runtime level built from the purchaser's files, then every placed
// actor projected), so the trigger, door and lift are the maps' own.

#include "WoTTestAIClasses.h"
#include "WoTTestBattleClasses.h"
#include "WoTTestSoundDispatcher.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeMapSky.h"
#include "../World/WoTWorldMaterializer.h"
#include "../World/WoTWorldProjector.h"
#include "../WoTBSPPayload.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
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
const TCHAR* const LevelMaps[] = {TEXT("Arena_02"), TEXT("Arena_04")};

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

#endif // WITH_AUTOMATION_TESTS
