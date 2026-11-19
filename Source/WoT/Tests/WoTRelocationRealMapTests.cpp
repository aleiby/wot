#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

// The swap, shift and spinning artifacts on real maps. Each map is materialized
// the way a release build does it (the runtime level built from the purchaser's
// files, then every placed actor projected), so the level geometry, its solid
// space and the map's own LevelInfo are the real ones. The server and client
// specs project a map's actors into both worlds of a dedicated server and its
// connected client, as each machine does when it loads the map.

#include "WoTTestBattleClasses.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../Inventory/WoTAngrealInvShift.h"
#include "../Inventory/WoTAngrealInvSwapPlaces.h"
#include "../Inventory/WoTIceLeech.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTWhirlwindLeech.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../Projectiles/WoTSwapPlacesProjectile.h"
#include "../WoTActorRotator.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeMapSky.h"
#include "../World/WoTRuntimeWorldProperties.h"
#include "../World/WoTWorldMaterializer.h"
#include "../World/WoTWorldProjector.h"
#include "../WoTBSPPayload.h"
#include "../WoTBounceableDecoration.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTCharacter.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTDecoration.h"
#include "../WoTGameMode.h"
#include "../WoTLeafMap.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshFX.h"
#include "../WoTMeshLight.h"
#include "../WoTMover.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTRetailText.h"
#include "../WoTSkyBox.h"
#include "../WoTUE1Collision.h"

#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#if WITH_EDITOR
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Tests/NetTestHelpers.h"
#endif

namespace WoTRelocationRealMap
{
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
constexpr float Step = 1.0f / 30.0f;

// Maps whose level geometry these specs build; the rest keep only their records.
const TCHAR* const LevelMaps[] = {TEXT("Arena_01"), TEXT("Arena_09")};

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
				EWoTBootstrapError::SizeLimit, TEXT("relocation retail writer overflow")));
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

bool DecodeWorld(FAutomationTestBase& Test, FFixture& Fixture,
	const FWoTWorldDescriptor& Descriptor, FWoTRuntimeWorldResource& Resource)
{
	const TArray<uint8>* WorldBytes = Fixture.Writer.Objects.Find(Descriptor.WorldLogicalId);
	if (!Test.TestNotNull(Descriptor.MapIdentity + TEXT(" world payload"), WorldBytes))
		return false;
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(*WorldBytes,
			TEXT("ue1-world-records-le-v6"), TEXT("world"));
	if (!Test.TestFalse(Descriptor.MapIdentity + TEXT(" world frame decodes"),
		Decoded.IsError())) return false;
	return Test.TestFalse(Descriptor.MapIdentity + TEXT(" world payload decodes"),
		Resource.LoadObjectForTesting(MoveTemp(Decoded).GetValue()).IsError());
}

void FillProjectionSource(const FWoTRuntimeWorldResource& Resource,
	FWoTWorldProjectionSource& Source)
{
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

	AWoTLeafMap* LeafMap() const
	{
		TActorIterator<AWoTLeafMap> It(World);
		return It ? *It : nullptr;
	}
};

bool BuildMap(FAutomationTestBase& Test, FFixture& Fixture, FStringView Map,
	FBuiltMap& Out)
{
	const FWoTWorldDescriptor* Descriptor = Fixture.Find(Map);
	if (!Test.TestNotNull(FString(Map) + TEXT(" is in the world set"), Descriptor))
		return false;
	FWoTRuntimeWorldResource Resource;
	if (!DecodeWorld(Test, Fixture, *Descriptor, Resource)) return false;
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
	Out.Records = Resource.Actors();
	FWoTWorldProjectionSource Source;
	FillProjectionSource(Resource, Source);
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

// The retail record index a projected actor came from ("WoTProjected_<index>_...").
int32 RecordIndexOf(const AActor* Actor)
{
	const FString Name = Actor ? Actor->GetName() : FString();
	const FString Prefix = TEXT("WoTProjected_");
	return Name.StartsWith(Prefix) ? FCString::Atoi(*Name.Mid(Prefix.Len())) : INDEX_NONE;
}

FString ClassLeaf(const FString& ClassIdentity)
{
	int32 Dot = INDEX_NONE;
	return ClassIdentity.FindLastChar(TEXT('.'), Dot) ? ClassIdentity.Mid(Dot + 1)
		: ClassIdentity;
}

// A player standing on the map, holding the shift artifact.
struct FShifter
{
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Pawn = nullptr;
	AWoTAngrealInvShift* Shift = nullptr;
};

bool StandShifter(FAutomationTestBase& Test, UWorld& World, const FVector& At,
	FShifter& Out)
{
	Out.Controller = World.SpawnActor<AWoTPlayerController>(AlwaysSpawn());
	Out.Pawn = World.SpawnActor<AWoTCharacter>(At, FRotator::ZeroRotator, AlwaysSpawn());
	if (!Test.TestNotNull(TEXT("shifter controller"), Out.Controller)
		|| !Test.TestNotNull(TEXT("shifter"), Out.Pawn)) return false;
	Out.Controller->SetAsLocalPlayerController();
	Out.Controller->Possess(Out.Pawn);
	Out.Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	WoTTest::TickWorld(&World, 1.0f, Step);
	if (!Test.TestFalse(FString::Printf(TEXT("the shifter stands on the floor (at %s)"),
		*Out.Pawn->GetActorLocation().ToString()),
		Out.Pawn->GetCharacterMovement()->IsFalling())) return false;
	if (AWoTGameMode* Game = World.GetAuthGameMode<AWoTGameMode>())
	{
		Game->GivePawnAngreal(Out.Pawn, AWoTAngrealInvShift::StaticClass(), 5);
	}
	Out.Shift = Cast<AWoTAngrealInvShift>(Out.Pawn->InventoryComponent->FindInventoryType(
		AWoTAngrealInvShift::StaticClass()));
	return Test.TestNotNull(TEXT("the shifter holds the shift artifact"), Out.Shift);
}

// A body standing at At with no controller of its own.
AWoTCharacter* StandBody(FAutomationTestBase& Test, UWorld& World, const FVector& At)
{
	AWoTCharacter* Body = World.SpawnActor<AWoTCharacter>(At, FRotator::ZeroRotator, AlwaysSpawn());
	if (!Test.TestNotNull(TEXT("body"), Body)) return nullptr;
	Body->GetCharacterMovement()->bRunPhysicsWithNoController = true;
	Body->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	WoTTest::TickWorld(&World, 1.0f, Step);
	if (!Test.TestFalse(FString::Printf(TEXT("the body stands on the floor (at %s)"),
		*Body->GetActorLocation().ToString()), Body->GetCharacterMovement()->IsFalling()))
		return nullptr;
	return Body;
}

// The swap artifact's projectile, cast by Caster, detonating on Target.
bool CastSwapOnto(FAutomationTestBase& Test, UWorld& World, APawn& Caster, AActor& Target)
{
	AWoTSwapPlacesProjectile* Projectile = World.SpawnActor<AWoTSwapPlacesProjectile>(
		AWoTSwapPlacesProjectile::StaticClass(), Target.GetActorLocation() + FVector(0, 0, 120),
		FRotator::ZeroRotator, AlwaysSpawn());
	if (!Test.TestNotNull(TEXT("swap projectile"), Projectile)) return false;
	Projectile->InitiatingPawn = &Caster;
	Projectile->SetIgnoredPawn(&Caster);
	Projectile->SetDestination(&Target);
	Projectile->Explode(Projectile->GetActorLocation(), FVector::UpVector);
	return true;
}

bool SamePose(const AActor& A, const AActor& B, double Tolerance = 0.05)
{
	return A.GetActorLocation().Equals(B.GetActorLocation(), Tolerance)
		&& A.GetActorRotation().Equals(B.GetActorRotation(), 0.05);
}

#if WITH_EDITOR
// Payload activation as a release world has it, published for the test worlds
// of a dedicated server and its clients.
class FScopedReadyWorlds
{
public:
	FScopedReadyWorlds()
	{
		if (!IsWoTPayloadBootstrapActive())
		{
			bOwns = !PublishWoTPayloadBootstrapActivationForTesting().IsError()
				&& !PublishWoTBootstrapWorldReadyForTesting().IsError();
		}
	}
	~FScopedReadyWorlds()
	{
		if (bOwns) ResetWoTPayloadActivationForTesting();
	}

private:
	bool bOwns = false;
};

// What a level does for its actors that have not finished spawning when the
// world is set up for play (ULevel::InitializeNetworkActors, not exported):
// each becomes a startup actor, known by its name on every machine, and on a
// client a replicated one becomes the proxy of the server's actor.
void InitializeAsLevelActor(AActor& Actor)
{
	if (Actor.IsActorInitialized()) return;
	if (Actor.bNetLoadOnClient)
	{
		Actor.bNetStartup = true;
		for (UActorComponent* Component : Actor.GetComponents())
		{
			if (Component) Component->SetIsNetStartupComponent(true);
		}
	}
	if (Actor.GetWorld() && Actor.GetWorld()->IsNetMode(NM_Client))
	{
		if (!Actor.bNetLoadOnClient) Actor.Destroy(true);
		else Actor.ExchangeNetRoles(true);
	}
}

// Each projected actor set up as a level's network actor, then finished and
// begun play, in that order.
void FinishProjectedActors(const FWoTWorldProjectionResult& Out)
{
	for (AActor* Actor : Out.DeferredActors)
	{
		if (IsValid(Actor)) InitializeAsLevelActor(*Actor);
	}
	for (AActor* Actor : Out.DeferredActors)
	{
		if (IsValid(Actor) && !Actor->IsActorInitialized())
			Actor->FinishSpawning(Actor->GetActorTransform(), true);
	}
	for (AActor* Actor : Out.DeferredActors)
	{
		if (IsValid(Actor) && !Actor->HasActorBegunPlay()) Actor->DispatchBeginPlay();
	}
}

// A dedicated server and one connected client.
bool Connect(FAutomationTestBase& Test, UE::Net::FTestWorlds& Worlds)
{
	return Test.TestTrue(TEXT("the server loads"), Worlds.Server.IsLoaded())
		&& Test.TestTrue(TEXT("a client connects"), Worlds.CreateAndConnectClient());
}

// The map's placed actors projected into a world already in play, the way a
// machine projects them while it loads the map: every actor is spawned and
// configured first, then the level's network actors are set up (on a client
// the replicated ones become proxies of the server's actors of the same name),
// then they finish spawning and begin play.
bool ProjectIntoPlayingWorld(FAutomationTestBase& Test, FFixture& Fixture, FStringView Map,
	UWorld& World, FWoTWorldProjectionResult& Out,
	TArray<TStrongObjectPtr<UTexture2D>>& Textures)
{
	const FWoTWorldDescriptor* Descriptor = Fixture.Find(Map);
	if (!Test.TestNotNull(FString(Map) + TEXT(" is in the world set"), Descriptor))
		return false;
	FWoTRuntimeWorldResource Resource;
	if (!DecodeWorld(Test, Fixture, *Descriptor, Resource)) return false;
	FWoTWorldProjectionSource Source;
	FillProjectionSource(Resource, Source);
	// The movers' light-map atlas and its responses, made from the world
	// payload as the materializer makes them.
	TArray<UTexture2D*> Responses;
	if (Resource.MoverAtlasWidth() != 0 || Resource.MoverAtlasHeight() != 0
		|| !Resource.MoverAtlasBGRA().IsEmpty())
	{
		UTexture2D* Lightmap = WoTRuntimeBSPMaterializeLightmap(Resource.MoverAtlasWidth(),
			Resource.MoverAtlasHeight(), Resource.MoverAtlasBGRA());
		if (!Test.TestNotNull(FString(Map) + TEXT(" mover light-map"), Lightmap)) return false;
		Textures.Emplace(Lightmap);
		Source.MoverLightmap = Lightmap;
		for (const FWoTRuntimeMoverAnimationTerm& Term : Resource.MoverAnimationTerms())
		{
			UTexture2D* Response = WoTRuntimeBSPMaterializeLightmap(Resource.MoverAtlasWidth(),
				Resource.MoverAtlasHeight(), Term.ResponseBGRA);
			if (!Test.TestNotNull(FString(Map) + TEXT(" mover light-map response"), Response))
				return false;
			Textures.Emplace(Response);
			Responses.Add(Response);
		}
		if (!Responses.IsEmpty())
		{
			UTexture2D* Black = WoTRuntimeBSPMaterializeSolidBlackTexture();
			if (!Test.TestNotNull(TEXT("black light-map response"), Black)) return false;
			Textures.Emplace(Black);
			Source.MoverBlackLightResponse = Black;
		}
		Source.MoverLightResponses = Responses;
	}
	FString Error;
	if (!Test.TestTrue(FString(Map) + TEXT(" projects into ") + World.GetName(),
		FWoTWorldProjector::ProjectDeferred(World, Source, *Fixture.Defaults, Out, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	for (const FString& Skipped : Out.SkippedSamples)
	{
		Test.AddInfo(FString(Map) + TEXT(" skipped ") + Skipped);
	}
	FinishProjectedActors(Out);
	return true;
}

// Only the records of Map that Keep selects, projected into a world already in
// play as ProjectIntoPlayingWorld projects a whole map. OutRecords are the kept
// records, in projection order (RecordIndexOf indexes them).
bool ProjectRecordsIntoPlayingWorld(FAutomationTestBase& Test, FFixture& Fixture,
	FStringView Map, TFunctionRef<bool(const FWoTRuntimeWorldActor&)> Keep, UWorld& World,
	FWoTWorldProjectionResult& Out, TArray<FWoTRuntimeWorldActor>& OutRecords)
{
	const FWoTWorldDescriptor* Descriptor = Fixture.Find(Map);
	if (!Test.TestNotNull(FString(Map) + TEXT(" is in the world set"), Descriptor))
		return false;
	FWoTRuntimeWorldResource Resource;
	if (!DecodeWorld(Test, Fixture, *Descriptor, Resource)) return false;
	OutRecords.Reset();
	for (const FWoTRuntimeWorldActor& Record : Resource.Actors())
	{
		if (Keep(Record)) OutRecords.Add(Record);
	}
	FWoTWorldProjectionSource Source;
	FillProjectionSource(Resource, Source);
	Source.Records = OutRecords;
	// The leaves' lights name light records left out here.
	Source.Leaves = {};
	Source.ModelLights = {};
	FString Error;
	if (!Test.TestTrue(FString(Map) + TEXT(" records project into ") + World.GetName(),
		FWoTWorldProjector::ProjectDeferred(World, Source, *Fixture.Defaults, Out, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	FinishProjectedActors(Out);
	return true;
}

// A name property the record itself carries ("" when it has none).
FString RecordName(const FWoTRuntimeWorldActor& Record, const TCHAR* Property)
{
	const FWoTRuntimeWorldProperty* Value = WoTRuntimeWorldFindProperty(Record, Property);
	return Value && Value->Type == EWoTRuntimeWorldPropertyType::Name
		? Value->NameValue : FString();
}

// The projected decorations of one class, in record order.
TArray<AWoTDecoration*> DecorationsOfClass(const FWoTWorldProjectionResult& Projection,
	const TArray<FWoTRuntimeWorldActor>& Records, const TCHAR* Leaf)
{
	TArray<AWoTDecoration*> Found;
	for (AActor* Actor : Projection.DeferredActors)
	{
		AWoTDecoration* Decoration = Cast<AWoTDecoration>(Actor);
		const int32 Index = RecordIndexOf(Actor);
		if (IsValid(Decoration) && Records.IsValidIndex(Index)
			&& ClassLeaf(Records[Index].ClassIdentity).Equals(Leaf, ESearchCase::IgnoreCase))
		{
			Found.Add(Decoration);
		}
	}
	return Found;
}

template <typename T>
T* ClientCopy(UWorld& ClientWorld, const AActor& ServerActor)
{
	return FindObject<T>(ClientWorld.PersistentLevel, *ServerActor.GetName());
}

AWoTCharacter* FlyingBody(UWorld& World, const FVector& At, float Yaw)
{
	AWoTCharacter* Body = World.SpawnActor<AWoTCharacter>(At, FRotator(0, Yaw, 0), AlwaysSpawn());
	if (Body) Body->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	return Body;
}
#endif
}

// A body standing on Arena_09's floor (top at -7914) and looking about 42
// degrees down: its shift destination 160 units along the view lies wholly
// under that floor, outside the level. The original places the body
// with SetLocation (ShiftEffect.uc:136-141), which cannot fit it in solid space,
// so the cast fails and keeps its charge (AngrealInvShift.uc:45-53); it never
// leaves the body under the floor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTShiftRealMapAimedDownSpec,
	"WoT.Inventory.RelocationRealMap.ShiftAimedIntoTheFloor",
	WoTRelocationRealMap::Flags)

bool FWoTShiftRealMapAimedDownSpec::RunTest(const FString&)
{
	using namespace WoTRelocationRealMap;
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: the real-map shift is not verified"));
		return true;
	}
	FFixture& Fixture = TheFixture();
	if (!Fixture.Load(*this)) return false;
	FBuiltMap Built;
	if (!BuildMap(*this, Fixture, TEXT("Arena_09"), Built)) return false;
	UWorld& World = *Built.World;
	AWoTLeafMap* LeafMap = Built.LeafMap();
	if (!TestNotNull(TEXT("Arena_09 carries its solid-space classification"), LeafMap))
		return false;

	FShifter Shifter;
	if (!StandShifter(*this, World, FVector(-4796, -11821, -7866), Shifter)) return false;
	Shifter.Controller->SetControlRotation(FRotator(-42.0f, 55.7f, 0.0f));
	const FVector Before = Shifter.Pawn->GetActorLocation();
	const FVector Aimed = Before
		+ Shifter.Pawn->GetViewRotation().Vector() * Shifter.Shift->ShiftDistance;
	TestTrue(FString::Printf(TEXT("the aimed destination %s lies in solid space under the floor"),
		*Aimed.ToString()), LeafMap->IsPointInSolid(Aimed));
	const int32 Charges = Shifter.Shift->CurCharges;
	Shifter.Shift->CastArtifact();
	const FVector After = Shifter.Pawn->GetActorLocation();
	const bool bShifted = Shifter.Shift->CurCharges != Charges;
	AddInfo(FString::Printf(TEXT("cast %s: %s -> %s"), bShifted ? TEXT("succeeded")
		: TEXT("failed"), *Before.ToString(), *After.ToString()));
	if (!bShifted)
	{
		TestTrue(TEXT("a refused shift leaves the body where it stood"),
			After.Equals(Before, 1.0));
	}
	TestFalse(FString::Printf(TEXT("the body is not in solid space (at %s)"), *After.ToString()),
		LeafMap->IsPointInSolid(After));
	WoTTest::TickWorld(&World, 1.0f, Step);
	const FVector Settled = Shifter.Pawn->GetActorLocation();
	TestFalse(FString::Printf(TEXT("a second later the body stands on the level (at %s)"),
		*Settled.ToString()), Shifter.Pawn->GetCharacterMovement()->IsFalling());
	TestTrue(FString::Printf(TEXT("on the floor it stood on (z %.1f, was %.1f)"),
		Settled.Z, Before.Z), FMath::Abs(Settled.Z - Before.Z) < 20.0);
	return !HasAnyErrors();
}

// A world swap on real maps. The original's level is the map's LevelInfo: on
// Arena_09 it sits in solid space, so the caster cannot be moved there and is
// told it does not fit at the target's location (SwapPlacesEffect.uc:164-176);
// on Arena_01 the caster fits there, the static LevelInfo cannot move, and the
// caster is put back and told the target does not fit (uc:178-191). Nothing else
// moves or turns.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapRealMapLevelSpec,
	"WoT.Inventory.RelocationRealMap.SwapWithTheLevel",
	WoTRelocationRealMap::Flags)

bool FWoTSwapRealMapLevelSpec::RunTest(const FString&)
{
	using namespace WoTRelocationRealMap;
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: the real-map swap is not verified"));
		return true;
	}
	FFixture& Fixture = TheFixture();
	if (!Fixture.Load(*this)) return false;
	const bool bText = FWoTRetailText::EnsureLoaded();
	if (!bText)
	{
		AddWarning(TEXT("retail localization is unavailable; the refusal text is not checked"));
	}
	struct FCase
	{
		const TCHAR* Map;
		const TCHAR* MessageKey;
	};
	const FCase Cases[] = {
		{TEXT("Arena_09"), TEXT("SourceFailMessage")},
		{TEXT("Arena_01"), TEXT("DestinationFailMessage")},
	};
	for (const FCase& Case : Cases)
	{
		FBuiltMap Built;
		if (!BuildMap(*this, Fixture, Case.Map, Built)) continue;
		UWorld& World = *Built.World;
		const FString Map(Case.Map);
		const UWoTMapLightTable* Table = UWoTMeshLight::FindMapLightTable(&World);
		if (!TestTrue(Map + TEXT(" records where its LevelInfo stands"),
			Table && Table->bHasLevelInfoLocation)) continue;
		AddInfo(FString::Printf(TEXT("%s LevelInfo at %s"), Case.Map,
			*Table->LevelInfoLocation.ToString()));

		APlayerStart* Start = nullptr;
		for (TActorIterator<APlayerStart> It(&World); It && !Start; ++It) Start = *It;
		if (!TestNotNull(Map + TEXT(" has a player start"), Start)) continue;
		AWoTTestSealNotifyController* Controller =
			World.SpawnActor<AWoTTestSealNotifyController>(AlwaysSpawn());
		AWoTCharacter* Caster = World.SpawnActor<AWoTCharacter>(
			Start->GetActorLocation(), FRotator(0, 30, 0), AlwaysSpawn());
		AWoTAngrealInvSwapPlaces* Artifact = World.SpawnActor<AWoTAngrealInvSwapPlaces>(
			Start->GetActorLocation(), FRotator::ZeroRotator, AlwaysSpawn());
		if (!TestNotNull(Map + TEXT(" caster"), Caster)
			|| !TestNotNull(Map + TEXT(" controller"), Controller)
			|| !TestNotNull(Map + TEXT(" artifact"), Artifact)) continue;
		Controller->SetAsLocalPlayerController();
		Controller->Possess(Caster);
		Controller->SetControlRotation(FRotator(0, 30, 0));
		Caster->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		Artifact->GiveTo(Caster);
		WoTTest::TickWorld(&World, 1.0f, Step);
		if (!TestFalse(Map + TEXT(" caster stands at its start"),
			Caster->GetCharacterMovement()->IsFalling())) continue;

		TArray<FVector> PartsBefore;
		for (const UStaticMeshComponent* Part : Built.Level->GeometryComponents)
			PartsBefore.Add(Part ? Part->GetComponentLocation() : FVector::ZeroVector);
		const FTransform LevelBefore = Built.Level->GetActorTransform();
		const FVector CasterBefore = Caster->GetActorLocation();
		const FRotator CasterRotationBefore = Caster->GetActorRotation();
		UPrimitiveComponent* Wall = Built.Level->GeometryComponents.IsEmpty() ? nullptr
			: Built.Level->GeometryComponents[0].Get();
		AWoTSwapPlacesProjectile* Swap = World.SpawnActor<AWoTSwapPlacesProjectile>(
			AWoTSwapPlacesProjectile::StaticClass(), CasterBefore + FVector(0, 0, 200),
			FRotator::ZeroRotator, AlwaysSpawn());
		if (!TestNotNull(Map + TEXT(" swap projectile"), Swap)) continue;
		Swap->SetSourceAngreal(Artifact);
		Swap->InitiatingPawn = Caster;
		Swap->SetIgnoredPawn(Caster);
		const FVector Impact = Swap->GetActorLocation() + FVector(0, 0, 6);
		const FHitResult Hit(Built.Level, Wall, Impact, -FVector::UpVector);
		Swap->NotifyHit(nullptr, Built.Level, Wall, true, Impact, -FVector::UpVector,
			FVector::ZeroVector, Hit);

		TestTrue(Map + TEXT(": the level stays put and keeps its rotation"),
			Built.Level->GetActorTransform().Equals(LevelBefore, 0.001));
		bool bPartsStayed = true;
		for (int32 Index = 0; Index < PartsBefore.Num(); ++Index)
		{
			const UStaticMeshComponent* Part = Built.Level->GeometryComponents[Index];
			bPartsStayed &= !Part || Part->GetComponentLocation().Equals(PartsBefore[Index], 0.001);
		}
		TestTrue(Map + TEXT(": every level part stays put"), bPartsStayed);
		TestTrue(FString::Printf(TEXT("%s: the caster stays put (now %s, was %s)"), Case.Map,
			*Caster->GetActorLocation().ToString(), *CasterBefore.ToString()),
			Caster->GetActorLocation().Equals(CasterBefore, 1.0));
		TestTrue(Map + TEXT(": the caster keeps its facing"),
			Caster->GetActorRotation().Equals(CasterRotationBefore, 0.01));
		TestEqual(Map + TEXT(": a refused swap shows no flashes"), Swap->LastSwapFlashCount, 0);
		if (bText)
		{
			const FString* Suffix = FWoTRetailText::FindSectionValue(
				TEXT("SwapPlacesEffect"), Case.MessageKey);
			const FString Got = Controller->CenterMessages.IsEmpty() ? FString()
				: Controller->CenterMessages.Last();
			TestEqual(Map + TEXT(": the caster is told which end did not fit"), Got,
				Suffix ? Artifact->Title + TEXT(" ") + *Suffix : FString());
		}
		WoTTest::TickWorld(&World, 1.0f, Step);
		TestFalse(Map + TEXT(": the caster still stands a second later"),
			Caster->GetCharacterMovement()->IsFalling());
	}
	return !HasAnyErrors();
}

// Map-placed blockers and decorations carry their own static flag into the
// port, so the swap artifact can refuse to move the ones the original could
// not. Blockers are static; a decoration is static unless its class or the map
// says otherwise: hanging lights are not static by class
// (HangingLight.uc:20), except the four Battle2_05 sets.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTProjectedStaticFlagsSpec,
	"WoT.World.Projector.RealRetailStaticFlags",
	WoTRelocationRealMap::Flags)

bool FWoTProjectedStaticFlagsSpec::RunTest(const FString&)
{
	using namespace WoTRelocationRealMap;
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: projected static flags are not verified"));
		return true;
	}
	FFixture& Fixture = TheFixture();
	if (!Fixture.Load(*this)) return false;
	TMap<FString, int32> StaticByClass;
	TMap<FString, int32> MovableByClass;
	int32 Blockers = 0;
	int32 StaticBlockers = 0;
	for (const FWoTWorldDescriptor& Descriptor : Fixture.Worlds)
	{
		FWoTRuntimeWorldResource Resource;
		if (!DecodeWorld(*this, Fixture, Descriptor, Resource)) continue;
		WoTTest::FSpecWorldOptions Options;
		Options.bStartPlay = false;
		UWorld* World = WoTTest::CreateSpecWorld(Options);
		if (!TestNotNull(TEXT("spec world"), World)) continue;
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		FWoTWorldProjectionSource Source;
		FillProjectionSource(Resource, Source);
		FWoTWorldProjectionResult Projection;
		FString Error;
		if (!TestTrue(Descriptor.MapIdentity + TEXT(" projects"),
			FWoTWorldProjector::ProjectDeferred(*World, Source, *Fixture.Defaults,
				Projection, Error)))
		{
			AddError(Error);
			continue;
		}
		const TArray<FWoTRuntimeWorldActor>& Records = Resource.Actors();
		for (AActor* Actor : Projection.DeferredActors)
		{
			if (!IsValid(Actor)) continue;
			const int32 Index = RecordIndexOf(Actor);
			const FString Class = Records.IsValidIndex(Index)
				? ClassLeaf(Records[Index].ClassIdentity) : FString();
			if (const AWoTCollisionBlocker* Blocker = Cast<AWoTCollisionBlocker>(Actor))
			{
				++Blockers;
				StaticBlockers += Blocker->bStatic ? 1 : 0;
			}
			else if (const AWoTDecoration* Decoration = Cast<AWoTDecoration>(Actor))
			{
				const FString Key = Descriptor.MapIdentity + TEXT(".") + Class;
				(Decoration->bStatic ? StaticByClass : MovableByClass).FindOrAdd(Key)++;
			}
			Actor->FinishSpawning(Actor->GetActorTransform(), true);
		}
	}
	AddInfo(FString::Printf(TEXT("%d blockers, %d static and %d movable decoration groups"),
		Blockers, StaticByClass.Num(), MovableByClass.Num()));
	TestTrue(TEXT("the maps place blockers"), Blockers > 0);
	TestEqual(TEXT("every placed blocker is static"), StaticBlockers, Blockers);
	auto CountOf = [](const TMap<FString, int32>& Counts, const FString& Suffix)
	{
		int32 Total = 0;
		for (const TPair<FString, int32>& Entry : Counts)
			if (Entry.Key.EndsWith(Suffix)) Total += Entry.Value;
		return Total;
	};
	TestTrue(TEXT("gothic lamps are static"), CountOf(StaticByClass, TEXT(".GothicLamp")) > 0);
	TestEqual(TEXT("no gothic lamp is movable"), CountOf(MovableByClass, TEXT(".GothicLamp")), 0);
	TestTrue(TEXT("hanging light chains are not static (HangingLightChain.uc:20)"),
		CountOf(MovableByClass, TEXT(".HangingLightChain")) > 0);
	TestEqual(TEXT("no hanging light chain is static"),
		CountOf(StaticByClass, TEXT(".HangingLightChain")), 0);
	TestTrue(TEXT("Arena_03's hanging lights keep their class's flag: not static"),
		MovableByClass.FindRef(TEXT("Arena_03.HangingLight")) > 0
		&& StaticByClass.FindRef(TEXT("Arena_03.HangingLight")) == 0);
	TestEqual(TEXT("Battle2_05 makes four of its hanging lights static"),
		StaticByClass.FindRef(TEXT("Battle2_05.HangingLight")), 4);
	TestEqual(TEXT("and leaves its other two as their class has them"),
		MovableByClass.FindRef(TEXT("Battle2_05.HangingLight")), 2);
	return !HasAnyErrors();
}

// The shift artifact moves its caster a few paces through any obstacle, but
// only onto a spot the original's SetLocation can fit (ShiftEffect.uc:136-141).
// On Arena_09, a body 30 units from the north outer wall and facing it aims into
// the void behind the wall: the cast fails, keeps its charge and leaves the body
// where it stood (AngrealInvShift.uc:45-53). A body facing the thin inner wall
// nearby goes through it to the floor beyond.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTShiftRealMapThinWallsSpec,
	"WoT.Inventory.RelocationRealMap.ShiftThroughThinWalls",
	WoTRelocationRealMap::Flags)

bool FWoTShiftRealMapThinWallsSpec::RunTest(const FString&)
{
	using namespace WoTRelocationRealMap;
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: the real-map shift is not verified"));
		return true;
	}
	FFixture& Fixture = TheFixture();
	if (!Fixture.Load(*this)) return false;
	FBuiltMap Built;
	if (!BuildMap(*this, Fixture, TEXT("Arena_09"), Built)) return false;
	UWorld& World = *Built.World;
	AWoTLeafMap* LeafMap = Built.LeafMap();
	if (!TestNotNull(TEXT("Arena_09 carries its solid-space classification"), LeafMap))
		return false;

	struct FCase
	{
		const TCHAR* Label;
		FVector StandAt;
		float Yaw;
		bool bPassesThrough;
	};
	const FCase Cases[] = {
		{TEXT("into the outer wall"), FVector(-5750.212, -11695.335, -7865.85), 110.0f, false},
		{TEXT("through the inner wall"), FVector(-4830.253, -12004.217, -7865.85), 70.0f, true},
	};
	for (const FCase& Case : Cases)
	{
		const FString Label(Case.Label);
		FShifter Shifter;
		if (!StandShifter(*this, World, Case.StandAt, Shifter)) continue;
		Shifter.Controller->SetControlRotation(FRotator(0.0f, Case.Yaw, 0.0f));
		const FVector Before = Shifter.Pawn->GetActorLocation();
		const FVector Aimed = Before
			+ Shifter.Pawn->GetViewRotation().Vector() * Shifter.Shift->ShiftDistance;
		FHitResult Wall;
		TestTrue(Label + TEXT(": a wall stands between the body and its destination"),
			WoTVisBlockingTrace(&World, Before, Aimed, Shifter.Pawn, Wall));
		TestEqual(Label + TEXT(": the destination is in solid space only behind the outer wall"),
			LeafMap->IsPointInSolid(Aimed), !Case.bPassesThrough);
		const int32 Charges = Shifter.Shift->CurCharges;
		Shifter.Shift->CastArtifact();
		const FVector After = Shifter.Pawn->GetActorLocation();
		AddInfo(FString::Printf(TEXT("%s: %s -> %s"), Case.Label, *Before.ToString(),
			*After.ToString()));
		if (Case.bPassesThrough)
		{
			TestEqual(Label + TEXT(": the cast spends a charge"),
				Shifter.Shift->CurCharges, Charges - 1);
			TestTrue(FString::Printf(TEXT("%s: the body is past the wall (at %s)"), Case.Label,
				*After.ToString()), FVector::Dist2D(After, Before) > 140.0
				&& (After - Before).Dot(Aimed - Before) > 0.0);
		}
		else
		{
			TestEqual(Label + TEXT(": the refused cast keeps its charge"),
				Shifter.Shift->CurCharges, Charges);
			TestTrue(FString::Printf(TEXT("%s: the body stays where it stood (at %s)"),
				Case.Label, *After.ToString()), After.Equals(Before, 1.0));
		}
		TestFalse(FString::Printf(TEXT("%s: the body is not in solid space (at %s)"),
			Case.Label, *After.ToString()), LeafMap->IsPointInSolid(After));
		WoTTest::TickWorld(&World, 1.0f, Step);
		const FVector Settled = Shifter.Pawn->GetActorLocation();
		TestFalse(FString::Printf(TEXT("%s: a second later the body stands on the level (at %s)"),
			Case.Label, *Settled.ToString()), Shifter.Pawn->GetCharacterMovement()->IsFalling());
		TestTrue(FString::Printf(TEXT("%s: on the floor it stood on (z %.1f, was %.1f)"),
			Case.Label, Settled.Z, Before.Z), FMath::Abs(Settled.Z - Before.Z) < 20.0);
	}
	return !HasAnyErrors();
}

// The spinning artifact eases the body it lifts a step each frame toward the
// point its caster aims at, and moves it there with SetLocation when no
// blocking actor is in the way (WhirlwindLeech.uc:127-138). SetLocation fits
// the body to the level: it nudges it out of a floor or wall it grazes, and a
// step into solid space fits nowhere, so the body holds where it was. On
// Arena_09, a body next to the north outer wall pulled toward a point far
// beyond it, and a body on the floor pulled toward a point far below it, never
// leave the level. One step at the server's 30 frames a second covers a
// sixtieth of the distance, more than the body's width.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRelocationWhirlwindSpec,
	"WoT.Relocation.WhirlwindLiftStaysInTheLevel",
	WoTRelocationRealMap::Flags)

bool FWoTRelocationWhirlwindSpec::RunTest(const FString&)
{
	using namespace WoTRelocationRealMap;
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: the real-map lift is not verified"));
		return true;
	}
	FFixture& Fixture = TheFixture();
	if (!Fixture.Load(*this)) return false;
	FBuiltMap Built;
	if (!BuildMap(*this, Fixture, TEXT("Arena_09"), Built)) return false;
	UWorld& World = *Built.World;
	AWoTLeafMap* LeafMap = Built.LeafMap();
	if (!TestNotNull(TEXT("Arena_09 carries its solid-space classification"), LeafMap))
		return false;

	struct FCase
	{
		const TCHAR* Label;
		FVector StandAt;
		FVector Pull;   // from where the body stands to where the caster aims
	};
	const FCase Cases[] = {
		{TEXT("next to the outer wall, pulled beyond it"),
			FVector(-5750.212, -11695.335, -7865.85),
			FRotator(0.0f, 110.0f, 0.0f).Vector() * 4000.0f},
		{TEXT("on the floor, pulled far below it"),
			FVector(-4796, -11821, -7866), FVector(0, 0, -4000)},
	};
	for (const FCase& Case : Cases)
	{
		AWoTCharacter* Body = StandBody(*this, World, Case.StandAt);
		if (!Body) continue;
		const FVector Start = Body->GetActorLocation();
		TestFalse(FString::Printf(TEXT("%s: the body starts in the level"), Case.Label),
			LeafMap->IsPointInSolid(Start));
		UWoTWhirlwindLeech* Lift = NewObject<UWoTWhirlwindLeech>(Body);
		Lift->AttachTo(Body);
		Lift->SetDesiredLocation(Start + Case.Pull);
		int32 FirstOutside = INDEX_NONE;
		FVector Outside = FVector::ZeroVector;
		for (int32 Frame = 0; Frame < 60 && FirstOutside == INDEX_NONE; ++Frame)
		{
			WoTTest::TickWorld(&World, 1, Step);
			if (LeafMap->IsPointInSolid(Body->GetActorLocation()))
			{
				FirstOutside = Frame;
				Outside = Body->GetActorLocation();
			}
		}
		AddInfo(FString::Printf(TEXT("%s: %s -> %s"), Case.Label, *Start.ToString(),
			*Body->GetActorLocation().ToString()));
		TestEqual(FString::Printf(TEXT("%s: the body never leaves the level "
			"(the frame it did, it was at %s)"), Case.Label, *Outside.ToString()),
			FirstOutside, INDEX_NONE);
		Lift->Unattach();
		Body->Destroy();
	}
	return !HasAnyErrors();
}

// A decoration that is not static can be moved during play, by the swap
// artifact among others, and the original sends it to clients as its network
// role says, its class's own or the map's: one whose role is ROLE_None is never
// sent (Fish.uc:65, and the dragonflies and hanging lights Battle2_01 sets so),
// a temporary one is sent once, when it first appears (SeaweedA.uc:24-25,
// Crow.uc:29-30, Rat.uc:27-28, Spider.uc:24-25), and the rest are replicated
// actors (HangingLight.uc:20, HangingLightChain.uc:20). The projection keeps a
// static decoration, and one whose role is ROLE_None, local to each machine; a
// temporary one replicates once, awake from the start; every other one
// replicates its movement, asleep until something moves it, and is known to
// every client by name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRelocationProjectedMovableDecorationsSpec,
	"WoT.Relocation.ProjectedMovableDecorationsReplicate",
	WoTRelocationRealMap::Flags)

bool FWoTRelocationProjectedMovableDecorationsSpec::RunTest(const FString&)
{
	using namespace WoTRelocationRealMap;
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: projected decorations are not verified"));
		return true;
	}
	FFixture& Fixture = TheFixture();
	if (!Fixture.Load(*this)) return false;
	// UE1 ENetRole: ROLE_None is 0.
	constexpr uint8 RoleNone = 0;
	auto IsOneOf = [](const FString& Class, std::initializer_list<const TCHAR*> Classes)
	{
		for (const TCHAR* Candidate : Classes)
		{
			if (Class.Equals(Candidate, ESearchCase::IgnoreCase)) return true;
		}
		return false;
	};
	int32 Movable = 0;
	int32 Static = 0;
	int32 NeverSent = 0;
	int32 SentOnce = 0;
	TMap<FString, FString> RolesByClass;
	TArray<FString> Wrong;
	for (const TCHAR* Map : {TEXT("Arena_12"), TEXT("Battle2_01"), TEXT("Battle2_03"),
		TEXT("Battle2_05")})
	{
		const FWoTWorldDescriptor* Descriptor = Fixture.Find(Map);
		if (!TestNotNull(FString(Map) + TEXT(" is in the world set"), Descriptor)) continue;
		FWoTRuntimeWorldResource Resource;
		if (!DecodeWorld(*this, Fixture, *Descriptor, Resource)) continue;
		WoTTest::FSpecWorldOptions Options;
		Options.bStartPlay = false;
		UWorld* World = WoTTest::CreateSpecWorld(Options);
		if (!TestNotNull(TEXT("spec world"), World)) continue;
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		FWoTWorldProjectionSource Source;
		FillProjectionSource(Resource, Source);
		FWoTWorldProjectionResult Projection;
		FString Error;
		if (!TestTrue(FString(Map) + TEXT(" projects"),
			FWoTWorldProjector::ProjectDeferred(*World, Source, *Fixture.Defaults,
				Projection, Error)))
		{
			AddError(Error);
			continue;
		}
		const TArray<FWoTRuntimeWorldActor>& Records = Resource.Actors();
		for (AActor* Actor : Projection.DeferredActors)
		{
			const AWoTDecoration* Decoration = Cast<AWoTDecoration>(Actor);
			const int32 Index = RecordIndexOf(Actor);
			if (IsValid(Decoration) && Records.IsValidIndex(Index))
			{
				const FWoTRuntimeWorldActor& Record = Records[Index];
				const FString Class = ClassLeaf(Record.ClassIdentity);
				const FString Name = FString(Map) + TEXT(".") + Decoration->GetName()
					+ TEXT(" (") + Class + TEXT(")");
				// What the class defaults say, for the record.
				uint8 ClassRole = 255;
				bool bClassTemporary = false;
				if (const FWoTRuntimeWorldProperty* Role = Fixture.Defaults->FindClassProperty(
					Record.ClassIdentity, TEXT("RemoteRole")))
				{
					WoTRuntimeWorldPropertyByte(*Role, ClassRole);
				}
				if (const FWoTRuntimeWorldProperty* Temporary = Fixture.Defaults->FindClassProperty(
					Record.ClassIdentity, TEXT("bNetTemporary")))
				{
					WoTRuntimeWorldPropertyBool(*Temporary, bClassTemporary);
				}
				RolesByClass.FindOrAdd(Class) = FString::Printf(TEXT("%s RemoteRole=%d%s"),
					*Class, ClassRole, bClassTemporary ? TEXT(" bNetTemporary") : TEXT(""));
				// What the shipped scripts and the map say.
				uint8 PlacedRole = 1;
				bool bPlacedTemporary = false;
				const FWoTRuntimeWorldProperty* RoleOverride =
					WoTRuntimeWorldFindProperty(Record, TEXT("RemoteRole"));
				const FWoTRuntimeWorldProperty* TemporaryOverride =
					WoTRuntimeWorldFindProperty(Record, TEXT("bNetTemporary"));
				const bool bNone = RoleOverride
					? WoTRuntimeWorldPropertyByte(*RoleOverride, PlacedRole) && PlacedRole == RoleNone
					: IsOneOf(Class, {TEXT("Fish")});
				const bool bOnce = !bNone && (TemporaryOverride
					? WoTRuntimeWorldPropertyBool(*TemporaryOverride, bPlacedTemporary)
						&& bPlacedTemporary
					: IsOneOf(Class, {TEXT("SeaweedA"), TEXT("SeaweedB"), TEXT("SeaweedC"),
						TEXT("Crow"), TEXT("Rat"), TEXT("Spider")}));
				if (Decoration->bStatic)
				{
					++Static;
					if (Decoration->GetIsReplicated()) Wrong.Add(Name + TEXT(" static, replicated"));
				}
				else if (bNone)
				{
					++NeverSent;
					if (Decoration->GetIsReplicated()) Wrong.Add(Name + TEXT(" ROLE_None, replicated"));
				}
				else
				{
					++Movable;
					SentOnce += bOnce ? 1 : 0;
					const bool bLoose = Decoration->IsReplicatingMovement()
						&& Decoration->GetIsReplicated() && Decoration->IsNameStableForNetworking();
					if (!bLoose) Wrong.Add(Name + TEXT(" movable, does not replicate its movement"));
					else if (Decoration->bNetTemporary != bOnce)
						Wrong.Add(Name + (bOnce ? TEXT(" temporary, updated after it first appears")
							: TEXT(" sent only when it first appears")));
					else if (Decoration->NetDormancy
						!= (bOnce ? DORM_Awake : DORM_Initial)
						&& !Cast<AWoTBreakableDecoration>(Decoration)
						&& !Cast<AWoTBounceableDecoration>(Decoration))
						Wrong.Add(Name + (bOnce ? TEXT(" temporary, asleep")
							: TEXT(" awake before anything moves it")));
				}
			}
			if (IsValid(Actor)) Actor->FinishSpawning(Actor->GetActorTransform(), true);
		}
	}
	TArray<FString> Roles;
	RolesByClass.GenerateValueArray(Roles);
	Roles.Sort();
	const FString Census = FString::Printf(TEXT("%d replicated (%d of them sent once), "
		"%d never sent and %d static decorations; class defaults: %s"), Movable, SentOnce,
		NeverSent, Static, *FString::Join(Roles, TEXT(", ")));
	AddInfo(Census);
	UE_LOG(LogTemp, Display, TEXT("ProjectedMovableDecorationsReplicate: %s"), *Census);
	TestTrue(TEXT("the maps place replicated decorations"), Movable > SentOnce);
	TestTrue(TEXT("decorations sent once"), SentOnce > 0);
	TestTrue(TEXT("decorations never sent"), NeverSent > 0);
	TestTrue(TEXT("and static ones"), Static > 0);
	TestEqual(FString::Printf(TEXT("each replicates as its network role says (first: %s)"),
		Wrong.IsEmpty() ? TEXT("none") : *Wrong[0]), Wrong.Num(), 0);
	for (int32 Index = 1; Index < FMath::Min(Wrong.Num(), 12); ++Index)
	{
		AddError(TEXT("also: ") + Wrong[Index]);
	}
	return !HasAnyErrors();
}

// A swap whose far end is a door or lift moves it to where the caster stood
// and turns it to the caster's facing (SwapPlacesEffect.uc:178-206); a mover is
// not static. The original's movers were replicated actors, so its clients saw
// a mover where the server had put it. On a dedicated server running Arena_09
// with a client connected, the boiler lift a swap moves stands in the same
// place on the client, stays there while it rests, and when it next runs it
// runs along its own path on both.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRelocationMoverNetSpec,
	"WoT.Relocation.MoverMovedBySwapMatchesOnClient",
	WoTRelocationRealMap::Flags)

bool FWoTRelocationMoverNetSpec::RunTest(const FString&)
{
	using namespace WoTRelocationRealMap;
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: the swapped mover is not verified on a client"));
		return true;
	}
#if WITH_EDITOR
	FFixture& Fixture = TheFixture();
	if (!Fixture.Load(*this)) return false;
	FScopedReadyWorlds Ready;
	UE::Net::FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"), TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("a swap moves a mover on a dedicated server"));
	if (!Connect(*this, Worlds)) return false;
	UWorld& ServerWorld = *Worlds.Server.GetWorld();
	UWorld& ClientWorld = *Worlds.Clients[0].GetWorld();
	// The client has the map's actors before the server's reach it.
	FWoTWorldProjectionResult ClientProjection;
	FWoTWorldProjectionResult ServerProjection;
	TArray<TStrongObjectPtr<UTexture2D>> Textures;
	if (!ProjectIntoPlayingWorld(*this, Fixture, TEXT("Arena_09"), ClientWorld,
			ClientProjection, Textures)
		|| !ProjectIntoPlayingWorld(*this, Fixture, TEXT("Arena_09"), ServerWorld,
			ServerProjection, Textures)) return false;
	AWoTMover* Lift = nullptr;
	for (AActor* Actor : ServerProjection.DeferredActors)
	{
		AWoTMover* Mover = Cast<AWoTMover>(Actor);
		if (IsValid(Mover) && Mover->TagName == FName(TEXT("Boiler"))) Lift = Mover;
	}
	if (!TestNotNull(TEXT("Arena_09's boiler lift"), Lift)) return false;
	AWoTMover* ClientLift = ClientCopy<AWoTMover>(ClientWorld, *Lift);
	if (!TestNotNull(TEXT("the client's boiler lift"), ClientLift)) return false;
	if (!TestTrue(TEXT("the lift reaches the client"), Worlds.TickUntil([&Worlds, Lift, ClientLift]()
		{
			return Worlds.IsServerObjectReplicated(Lift)
				&& ClientLift->GetLocalRole() == ROLE_SimulatedProxy;
		}, 120))) return false;
	TestTrue(TEXT("as the client's own lift of the same name"),
		Worlds.FindReplicatedObjectOnClient(Lift) == ClientLift);
	TestTrue(TEXT("both lifts rest at their base"), SamePose(*Lift, *ClientLift));

	// The caster stands at a player start where the lift clears every blocking
	// actor (SwapPlacesEffect.uc:140-157), and it fits where the lift is.
	const FVector CasterExtent(17.0f, 17.0f, 46.0f);
	FVector LiftSpot = Lift->GetActorLocation();
	TestTrue(FString::Printf(TEXT("a caster fits where the lift is (%s)"), *LiftSpot.ToString()),
		WoTUE1Collision::FindSpotLevelOnly(ServerWorld, CasterExtent, LiftSpot, false));
	const AWoTPlayerStart* Start = nullptr;
	for (AActor* Actor : ServerProjection.DeferredActors)
	{
		const AWoTPlayerStart* Candidate = Cast<AWoTPlayerStart>(Actor);
		if (!Start && IsValid(Candidate)
			&& WoTUE1Collision::LevelPointCheckFits(ServerWorld, Candidate->GetActorLocation(),
				CasterExtent)
			&& UWoTWhirlwindLeech::ActorFits(Lift, Candidate->GetActorLocation(), 1024.0f))
		{
			Start = Candidate;
		}
	}
	if (!TestNotNull(TEXT("a player start the lift fits at"), Start)) return false;
	AWoTCharacter* Caster = FlyingBody(ServerWorld, Start->GetActorLocation(), 37.0f);
	if (!TestNotNull(TEXT("caster"), Caster)) return false;
	const FVector CasterBefore = Caster->GetActorLocation();
	if (!CastSwapOnto(*this, ServerWorld, *Caster, *Lift)) return false;
	if (!TestTrue(FString::Printf(TEXT("the server's lift takes the caster's place "
		"(now %s, the caster stood at %s)"), *Lift->GetActorLocation().ToString(),
		*CasterBefore.ToString()), Lift->GetActorLocation().Equals(CasterBefore, 1.0)))
		return false;
	TestTrue(FString::Printf(TEXT("and turns to the caster's facing (yaw %.2f)"),
		Lift->GetActorRotation().Yaw),
		FMath::IsNearlyEqual(Lift->GetActorRotation().Yaw, 37.0, 0.01));

	const bool bArrived = Worlds.TickUntil(
		[Lift, ClientLift]() { return SamePose(*Lift, *ClientLift); }, 120);
	TestTrue(FString::Printf(TEXT("the client's lift stands where the server's does "
		"(client %s %s, server %s %s)"), *ClientLift->GetActorLocation().ToString(),
		*ClientLift->GetActorRotation().ToString(), *Lift->GetActorLocation().ToString(),
		*Lift->GetActorRotation().ToString()), bArrived);
	Worlds.TickAll(60);
	TestTrue(TEXT("and stays there while the lift rests"), SamePose(*Lift, *ClientLift));

	// When the lift runs again it goes back onto its own path, on both machines.
	Caster->Destroy();
	Lift->WoTTrigger(nullptr, nullptr);
	Worlds.TickAll(240);
	TestTrue(FString::Printf(TEXT("the server's lift has run back onto its path and opened "
		"(at %s, base %s)"), *Lift->GetActorLocation().ToString(),
		*Lift->GetBasePosition().ToString()),
		Lift->GetMoverPhase() == EWoTMoverPhase::Open
		&& FVector::Dist(Lift->GetActorLocation(), Lift->GetBasePosition()) < 300.0);
	TestTrue(FString::Printf(TEXT("and the client's with it (client %s, server %s)"),
		*ClientLift->GetActorLocation().ToString(), *Lift->GetActorLocation().ToString()),
		SamePose(*Lift, *ClientLift, 0.5));
#endif
	return !HasAnyErrors();
}

// The same for a decoration on a dedicated server with a client connected: a
// hanging light the swap artifact moves (HangingLight.uc:20, not static) stands
// in the same place on the client, which was sent nothing about it before.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRelocationDecorationNetSpec,
	"WoT.Relocation.DecorationMovedBySwapMatchesOnClient",
	WoTRelocationRealMap::Flags)

bool FWoTRelocationDecorationNetSpec::RunTest(const FString&)
{
	using namespace WoTRelocationRealMap;
#if WITH_EDITOR
	FScopedReadyWorlds Ready;
	UE::Net::FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"), TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("a swap moves a decoration on a dedicated server"));
	if (!Connect(*this, Worlds)) return false;
	UWorld& ServerWorld = *Worlds.Server.GetWorld();
	UWorld& ClientWorld = *Worlds.Clients[0].GetWorld();
	// Each machine places the light under one name and configures it as the
	// map projection does, before play.
	auto PlaceLight = [](UWorld& World) -> AWoTDecoration*
	{
		FActorSpawnParameters Params = AlwaysSpawn();
		Params.OverrideLevel = World.PersistentLevel;
		Params.bDeferConstruction = true;
		Params.Name = FName(TEXT("WoTProjected_7001_Decoration"));
		const FTransform At(FVector(0, 0, 3200));
		AWoTDecoration* Light = World.SpawnActor<AWoTDecoration>(
			AWoTDecoration::StaticClass(), At, Params);
		if (!Light) return nullptr;
		Light->ConfigureRuntimeCollision(true, true, true, 22.0f, 17.0f);
		Light->bStatic = false;
		// A hanging light keeps Actor's network role (HangingLight.uc sets none).
		Light->ConfigureRuntimeReplication(AWoTDecoration::UE1RoleDumbProxy, false);
		InitializeAsLevelActor(*Light);
		Light->FinishSpawning(At, true);
		if (!Light->HasActorBegunPlay()) Light->DispatchBeginPlay();
		return Light;
	};
	AWoTDecoration* ClientLight = PlaceLight(ClientWorld);
	AWoTDecoration* Light = PlaceLight(ServerWorld);
	if (!TestNotNull(TEXT("the client's light"), ClientLight)
		|| !TestNotNull(TEXT("the server's light"), Light)) return false;
	Worlds.TickAll(30);
	TestFalse(TEXT("the server sends nothing about a light nothing has moved"),
		Worlds.IsServerObjectReplicated(Light));

	AWoTCharacter* Caster = FlyingBody(ServerWorld, FVector(500, 300, 3000), 45.0f);
	if (!TestNotNull(TEXT("caster"), Caster)) return false;
	const FVector CasterBefore = Caster->GetActorLocation();
	if (!CastSwapOnto(*this, ServerWorld, *Caster, *Light)) return false;
	if (!TestTrue(FString::Printf(TEXT("the server's light takes the caster's place (now %s)"),
		*Light->GetActorLocation().ToString()),
		Light->GetActorLocation().Equals(CasterBefore, 1.0))) return false;
	const bool bArrived = Worlds.TickUntil(
		[Light, ClientLight]() { return SamePose(*Light, *ClientLight); }, 120);
	TestTrue(FString::Printf(TEXT("the client's light stands where the server's does "
		"(client %s %s, server %s %s)"), *ClientLight->GetActorLocation().ToString(),
		*ClientLight->GetActorRotation().ToString(), *Light->GetActorLocation().ToString(),
		*Light->GetActorRotation().ToString()), bArrived);
	Worlds.TickAll(60);
	TestTrue(TEXT("and stays there"), SamePose(*Light, *ClientLight));
#endif
	return !HasAnyErrors();
}

// A fish is never sent to clients (Fish.uc:65, RemoteRole=ROLE_None): every
// machine swims its own, so a swap moves a fish on the server alone. On a
// dedicated server with a client connected, both running Battle2_05's school of
// fish and the rotator that swims it (ActorRotator.uc), the fish a swap strikes
// takes the caster's place on the server, and the server sends nothing about
// it, then or once the rotator has carried it on round its circle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRelocationSwappedFishSpec,
	"WoT.Relocation.SwappedFishStaysOnTheServer",
	WoTRelocationRealMap::Flags)

bool FWoTRelocationSwappedFishSpec::RunTest(const FString&)
{
	using namespace WoTRelocationRealMap;
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: the swapped fish is not verified"));
		return true;
	}
#if WITH_EDITOR
	FFixture& Fixture = TheFixture();
	if (!Fixture.Load(*this)) return false;
	FScopedReadyWorlds Ready;
	UE::Net::FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"), TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("a swap moves a fish on a dedicated server"));
	if (!Connect(*this, Worlds)) return false;
	UWorld& ServerWorld = *Worlds.Server.GetWorld();
	UWorld& ClientWorld = *Worlds.Clients[0].GetWorld();
	// Battle2_05's school of fish and the rotator that swims it, on each machine.
	auto SchoolOfFish = [](const FWoTRuntimeWorldActor& Record)
	{
		const FString Class = ClassLeaf(Record.ClassIdentity);
		return Class.Equals(TEXT("Fish"), ESearchCase::IgnoreCase)
			|| (Class.Equals(TEXT("ActorRotator"), ESearchCase::IgnoreCase)
				&& RecordName(Record, TEXT("Event")).Equals(TEXT("SchoolOfFish01"),
					ESearchCase::IgnoreCase));
	};
	FWoTWorldProjectionResult ClientProjection;
	FWoTWorldProjectionResult ServerProjection;
	TArray<FWoTRuntimeWorldActor> ClientRecords;
	TArray<FWoTRuntimeWorldActor> ServerRecords;
	if (!ProjectRecordsIntoPlayingWorld(*this, Fixture, TEXT("Battle2_05"), SchoolOfFish,
			ClientWorld, ClientProjection, ClientRecords)
		|| !ProjectRecordsIntoPlayingWorld(*this, Fixture, TEXT("Battle2_05"), SchoolOfFish,
			ServerWorld, ServerProjection, ServerRecords)) return false;
	const TArray<AWoTDecoration*> School =
		DecorationsOfClass(ServerProjection, ServerRecords, TEXT("Fish"));
	if (!TestEqual(TEXT("Battle2_05's school has six fish"), School.Num(), 6)) return false;
	Worlds.TickAll(30);
	for (AWoTDecoration* Fish : School)
	{
		TestFalse(Fish->GetName() + TEXT(" is not a replicated actor (Fish.uc:65)"),
			Fish->GetIsReplicated());
		TestFalse(TEXT("the server sends nothing about ") + Fish->GetName(),
			Worlds.IsServerObjectReplicated(Fish));
	}
	// The fish standing clearest of the others, so the caster fits where it is.
	AWoTDecoration* Fish = nullptr;
	double Clearance = -1.0;
	for (AWoTDecoration* Candidate : School)
	{
		double Nearest = TNumericLimits<double>::Max();
		for (AWoTDecoration* Other : School)
		{
			if (Other != Candidate)
			{
				Nearest = FMath::Min(Nearest, FVector::Dist2D(
					Candidate->GetActorLocation(), Other->GetActorLocation()));
			}
		}
		if (Nearest > Clearance)
		{
			Clearance = Nearest;
			Fish = Candidate;
		}
	}
	if (!TestNotNull(TEXT("a fish to swap with"), Fish)) return false;
	AWoTDecoration* ClientFish = ClientCopy<AWoTDecoration>(ClientWorld, *Fish);
	if (!TestNotNull(TEXT("the client swims its own fish of the same name"), ClientFish))
		return false;
	bool bSwum = false;
	for (TActorIterator<AWoTActorRotator> It(&ServerWorld); It; ++It)
	{
		for (const TSoftObjectPtr<AActor>& Driven : It->DrivenActors)
		{
			bSwum |= Driven.Get() == Fish;
		}
	}
	TestTrue(TEXT("a rotator swims the fish"), bSwum);

	AWoTCharacter* Caster = FlyingBody(ServerWorld,
		Fish->GetActorLocation() + FVector(0, 0, 400), 0.0f);
	if (!TestNotNull(TEXT("caster"), Caster)) return false;
	const FVector CasterBefore = Caster->GetActorLocation();
	if (!CastSwapOnto(*this, ServerWorld, *Caster, *Fish)) return false;
	if (!TestTrue(FString::Printf(TEXT("the server's fish takes the caster's place (now %s)"),
		*Fish->GetActorLocation().ToString()),
		Fish->GetActorLocation().Equals(CasterBefore, 1.0))) return false;
	// The swap projectile's replicated destination names the fish (by its name,
	// which every machine shares); the fish itself is never sent: the server
	// opens no channel for it, while the swap lands or after.
	UNetDriver* Driver = ServerWorld.GetNetDriver();
	UNetConnection* Connection = Driver && !Driver->ClientConnections.IsEmpty()
		? Driver->ClientConnections[0].Get() : nullptr;
	if (!TestNotNull(TEXT("the server's connection to its client"), Connection)) return false;
	if (!TestFalse(TEXT("the server replicates through actor channels"),
		Driver->IsUsingIrisReplication())) return false;
	int32 TicksWithChannel = 0;
	for (int32 Tick = 0; Tick < 60; ++Tick)
	{
		Worlds.TickAll();
		TicksWithChannel += Connection->FindActorChannelRef(Fish) ? 1 : 0;
	}
	TestFalse(FString::Printf(TEXT("the rotator has carried the fish on (now %s)"),
		*Fish->GetActorLocation().ToString()),
		Fish->GetActorLocation().Equals(CasterBefore, 1.0));
	TestFalse(TEXT("the fish is still not a replicated actor"), Fish->GetIsReplicated());
	TestEqual(TEXT("ticks the server had a channel open for the fish"), TicksWithChannel, 0);
#endif
	return !HasAnyErrors();
}

// A seaweed is sent to clients once, when it first appears, and never updated
// after (SeaweedA.uc:24-25, bNetTemporary with ROLE_SimulatedProxy). On a
// dedicated server with a client connected, both running Battle2_05's weeds,
// the client takes a weed where the server has it when the weed first reaches
// it, and a swap after that moves the weed on the server alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRelocationSwappedSeaweedSpec,
	"WoT.Relocation.SwappedSeaweedStaysWhereClientsFirstSawIt",
	WoTRelocationRealMap::Flags)

bool FWoTRelocationSwappedSeaweedSpec::RunTest(const FString&)
{
	using namespace WoTRelocationRealMap;
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: the swapped seaweed is not verified"));
		return true;
	}
#if WITH_EDITOR
	FFixture& Fixture = TheFixture();
	if (!Fixture.Load(*this)) return false;
	FScopedReadyWorlds Ready;
	UE::Net::FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"), TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("a swap moves a seaweed on a dedicated server"));
	if (!Connect(*this, Worlds)) return false;
	UWorld& ServerWorld = *Worlds.Server.GetWorld();
	UWorld& ClientWorld = *Worlds.Clients[0].GetWorld();
	auto Seaweed = [](const FWoTRuntimeWorldActor& Record)
	{
		return ClassLeaf(Record.ClassIdentity).StartsWith(TEXT("Seaweed"),
			ESearchCase::IgnoreCase);
	};
	FWoTWorldProjectionResult ClientProjection;
	FWoTWorldProjectionResult ServerProjection;
	TArray<FWoTRuntimeWorldActor> ClientRecords;
	TArray<FWoTRuntimeWorldActor> ServerRecords;
	if (!ProjectRecordsIntoPlayingWorld(*this, Fixture, TEXT("Battle2_05"), Seaweed,
			ClientWorld, ClientProjection, ClientRecords)
		|| !ProjectRecordsIntoPlayingWorld(*this, Fixture, TEXT("Battle2_05"), Seaweed,
			ServerWorld, ServerProjection, ServerRecords)) return false;
	TArray<AWoTDecoration*> Weeds;
	for (const TCHAR* Class : {TEXT("SeaweedA"), TEXT("SeaweedB"), TEXT("SeaweedC")})
	{
		Weeds.Append(DecorationsOfClass(ServerProjection, ServerRecords, Class));
	}
	if (!TestEqual(TEXT("Battle2_05 places nine weeds"), Weeds.Num(), 9)) return false;
	// The weed standing clearest of the others, so the caster fits where it is.
	AWoTDecoration* Weed = nullptr;
	double Clearance = -1.0;
	for (AWoTDecoration* Candidate : Weeds)
	{
		double Nearest = TNumericLimits<double>::Max();
		for (AWoTDecoration* Other : Weeds)
		{
			if (Other != Candidate)
			{
				Nearest = FMath::Min(Nearest, FVector::Dist2D(
					Candidate->GetActorLocation(), Other->GetActorLocation()));
			}
		}
		if (Nearest > Clearance)
		{
			Clearance = Nearest;
			Weed = Candidate;
		}
	}
	if (!TestNotNull(TEXT("a weed to swap with"), Weed)) return false;
	AWoTDecoration* ClientWeed = ClientCopy<AWoTDecoration>(ClientWorld, *Weed);
	if (!TestNotNull(TEXT("the client's weed of the same name"), ClientWeed)) return false;
	TestTrue(TEXT("a weed is a replicated actor"), Weed->GetIsReplicated());
	UNetDriver* Driver = ServerWorld.GetNetDriver();
	UNetConnection* Connection = Driver && !Driver->ClientConnections.IsEmpty()
		? Driver->ClientConnections[0].Get() : nullptr;
	if (!TestNotNull(TEXT("the server's connection to its client"), Connection)) return false;
	const bool bLegacy = !Driver->IsUsingIrisReplication();

	// The server has the weed a little above its place when it first reaches
	// the client, which takes it there.
	const FVector Placed = ClientWeed->GetActorLocation();
	Weed->SetActorLocation(Placed + FVector(0, 0, 40));
	const FVector FirstSeen = Weed->GetActorLocation();
	const bool bReached = Worlds.TickUntil([ClientWeed, &FirstSeen]()
	{
		return ClientWeed->GetActorLocation().Equals(FirstSeen, 0.05);
	}, 120);
	TestTrue(FString::Printf(TEXT("the client takes the weed where the server has it "
		"(client %s, server %s)"), *ClientWeed->GetActorLocation().ToString(),
		*FirstSeen.ToString()), bReached);
	if (bLegacy)
	{
		TestTrue(TEXT("sent to the client as a temporary actor"),
			Connection->SentTemporaries.Contains(static_cast<AActor*>(Weed)));
	}

	AWoTCharacter* Caster = FlyingBody(ServerWorld, FirstSeen + FVector(0, 0, 400), 0.0f);
	if (!TestNotNull(TEXT("caster"), Caster)) return false;
	const FVector CasterBefore = Caster->GetActorLocation();
	if (!CastSwapOnto(*this, ServerWorld, *Caster, *Weed)) return false;
	if (!TestTrue(FString::Printf(TEXT("the server's weed takes the caster's place (now %s)"),
		*Weed->GetActorLocation().ToString()),
		Weed->GetActorLocation().Equals(CasterBefore, 1.0))) return false;
	Worlds.TickAll(60);
	TestTrue(FString::Printf(TEXT("the client's weed stays where it first saw it "
		"(client %s, first seen %s, server %s)"), *ClientWeed->GetActorLocation().ToString(),
		*FirstSeen.ToString(), *Weed->GetActorLocation().ToString()),
		ClientWeed->GetActorLocation().Equals(FirstSeen, 0.05));
	if (bLegacy)
	{
		TestNull(TEXT("the server keeps no channel open for it"),
			Connection->FindActorChannelRef(Weed));
	}
#endif
	return !HasAnyErrors();
}

// A frozen pawn's ice is an actor of its own in the original (IceLeech.uc:
// 205-236, a mesh with its own collision cylinder), and a swap that strikes the
// ice trades places with it, leaving the frozen pawn held where it was frozen
// (IceLeech.uc:193). On a dedicated server with a client connected, the ice a
// swap moves stands in the same place on the client, and its look moves with
// it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRelocationIceNetSpec,
	"WoT.Relocation.IceMovedBySwapMatchesOnClient",
	WoTRelocationRealMap::Flags)

bool FWoTRelocationIceNetSpec::RunTest(const FString&)
{
	using namespace WoTRelocationRealMap;
#if WITH_EDITOR
	FScopedReadyWorlds Ready;
	UE::Net::FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"), TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("a swap moves a frozen pawn's ice on a dedicated server"));
	if (!Connect(*this, Worlds)) return false;
	UWorld& ServerWorld = *Worlds.Server.GetWorld();
	AWoTCharacter* Frozen = FlyingBody(ServerWorld, FVector(0, 0, 3000), 0.0f);
	AWoTCharacter* Caster = FlyingBody(ServerWorld, FVector(400, 0, 3000), 90.0f);
	if (!TestNotNull(TEXT("frozen pawn"), Frozen) || !TestNotNull(TEXT("caster"), Caster))
		return false;
	UWoTIceLeech* Ice = NewObject<UWoTIceLeech>(Frozen);
	Ice->AttachTo(Frozen);
	if (!TestTrue(TEXT("the pawn is frozen in a block of ice"), IsValid(Ice->IceBlock.Get())))
		return false;
	AWoTCharacter* ClientFrozen = nullptr;
	UWoTIceLeech* ClientIce = nullptr;
	const bool bFrozenOnClient = Worlds.TickUntil([&]()
	{
		ClientFrozen = Worlds.IsServerObjectReplicated(Frozen)
			? Worlds.FindReplicatedObjectOnClient(Frozen) : nullptr;
		ClientIce = ClientFrozen ? ClientFrozen->FindComponentByClass<UWoTIceLeech>() : nullptr;
		return ClientIce && IsValid(ClientIce->IceBlock.Get())
			&& IsValid(ClientIce->FrozenShell.Get());
	}, 180);
	if (!TestTrue(TEXT("the client sees the pawn frozen in its ice"), bFrozenOnClient))
		return false;

	const FVector FrozenAt = Frozen->GetActorLocation();
	const FVector CasterBefore = Caster->GetActorLocation();
	if (!CastSwapOnto(*this, ServerWorld, *Caster, *Ice->IceBlock)) return false;
	if (!TestTrue(FString::Printf(TEXT("the server's ice takes the caster's place (now %s)"),
		*Ice->IceBlock->GetActorLocation().ToString()),
		Ice->IceBlock->GetActorLocation().Equals(CasterBefore, 1.0))) return false;
	TestTrue(TEXT("the caster takes the ice's place"),
		Caster->GetActorLocation().Equals(FrozenAt, 1.0));
	const bool bArrived = Worlds.TickUntil([Ice, ClientIce]()
	{
		return IsValid(ClientIce) && IsValid(ClientIce->IceBlock.Get())
			&& SamePose(*Ice->IceBlock, *ClientIce->IceBlock);
	}, 120);
	TestTrue(FString::Printf(TEXT("the client's ice stands where the server's does "
		"(client %s, server %s)"),
		IsValid(ClientIce) && ClientIce->IceBlock
			? *ClientIce->IceBlock->GetActorLocation().ToString() : TEXT("none"),
		*Ice->IceBlock->GetActorLocation().ToString()), bArrived);
	TestTrue(TEXT("and so does its look"), IsValid(ClientIce) && ClientIce->FrozenShell
		&& ClientIce->FrozenShell->GetActorLocation().Equals(
			Ice->IceBlock->GetActorLocation(), 0.5));
	Worlds.TickAll(30);
	TestTrue(TEXT("both stay there"), IsValid(ClientIce) && ClientIce->IceBlock
		&& SamePose(*Ice->IceBlock, *ClientIce->IceBlock)
		&& ClientIce->FrozenShell && ClientIce->FrozenShell->GetActorLocation().Equals(
			Ice->IceBlock->GetActorLocation(), 0.5));
	TestTrue(TEXT("the frozen pawn stays where it was frozen, on both machines"),
		Frozen->GetActorLocation().Equals(FrozenAt, 1.0)
		&& IsValid(ClientFrozen) && ClientFrozen->GetActorLocation().Equals(FrozenAt, 1.0));
#endif
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
