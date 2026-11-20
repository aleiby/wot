#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

// Actors that leave the level, on real maps. Each map is materialized the way a
// release build does it (the runtime level built from the purchaser's files,
// then every placed actor projected), so the level geometry and the map's own
// zone classification are the real ones.

#include "WoTTestFellOut.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTNPCWeapon.h"
#include "../AI/WoTNPCWeaponData.h"
#include "../AI/WoTPathNode.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTBagHolding.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTWhirlwindLeech.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../Projectiles/WoTBurningChunk.h"
#include "../Projectiles/WoTEarthTremorFX.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Projectiles/WoTProjectile.h"
#include "../Traps/WoTPit.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeMapSky.h"
#include "../World/WoTWorldMaterializer.h"
#include "../World/WoTWorldProjector.h"
#include "../WoTBSPPayload.h"
#include "../WoTBounceableDecoration.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTCreatureChunk.h"
#include "../WoTDecal.h"
#include "../WoTDecoration.h"
#include "../WoTExplodingMover.h"
#include "../WoTEvents.h"
#include "../WoTFragment.h"
#include "../WoTGameMode.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTMover.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTSkyBox.h"
#include "../WoTUE1Collision.h"
#include "../WoTWorldFX.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
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

namespace WoTFellOutRealMap
{
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
constexpr float Step = 1.0f / 30.0f;

// Maps whose level geometry these specs build; the rest keep only their records.
const TCHAR* const LevelMaps[] = {TEXT("Arena_01"), TEXT("Arena_09"), TEXT("Battle2_02")};

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
				EWoTBootstrapError::SizeLimit, TEXT("fell-out retail writer overflow")));
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

	// The projected actor made from retail record Index, if any.
	AActor* Projected(int32 Index) const
	{
		const FString Prefix = FString::Printf(TEXT("WoTProjected_%d_"), Index);
		for (TActorIterator<AActor> It(World); It; ++It)
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
	Out.Records = Resource.Actors();
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

namespace WoTFellOutRealMap
{
// Arena_01's lowest floor lies at Z -1072, and the void outside the level lies
// under it. The bot soak's runaway decoration (record 946) rests on that floor,
// and PlayerStart 214 stands over it.
constexpr int32 SoakDecorationRecord = 946;
constexpr int32 LowFloorStartRecord = 214;
constexpr double TerminalSpeed = 2500.0;

bool LoadAndBuild(FAutomationTestBase& Test, FStringView Map, FBuiltMap& Built)
{
	if (!WoTReferenceTreeExists())
	{
		Test.AddWarning(TEXT("real retail data absent: the real-map spec is not verified"));
		return false;
	}
	FFixture& Fixture = TheFixture();
	return Fixture.Load(Test) && BuildMap(Test, Fixture, Map, Built);
}

AWoTBounceableDecoration* SoakDecoration(FAutomationTestBase& Test, const FBuiltMap& Built)
{
	AWoTBounceableDecoration* Decoration =
		Cast<AWoTBounceableDecoration>(Built.Projected(SoakDecorationRecord));
	Test.TestNotNull(TEXT("Arena_01 places the soak's bounceable decoration"), Decoration);
	return Decoration;
}

void Knock(AActor& Victim, const FVector& Momentum)
{
	FWoTDamageEvent Hit;
	Hit.Damage = 20.0f;
	Hit.WoTDamageType = TEXT("exploded");
	Hit.UE1Momentum = Momentum;
	Victim.TakeDamage(20.0f, Hit, nullptr, nullptr);
}

// A player standing on the map: a real controller with its player state,
// possessing Body at Location.
template <typename TBody>
TBody* StandPlayer(FAutomationTestBase& Test, UWorld& World, const FVector& Location,
	AWoTPlayerController*& OutController)
{
	OutController = World.SpawnActor<AWoTPlayerController>(AlwaysSpawn());
	TBody* Body = World.SpawnActor<TBody>(Location, FRotator::ZeroRotator, AlwaysSpawn());
	if (!Test.TestNotNull(TEXT("player controller"), OutController)
		|| !Test.TestNotNull(TEXT("player body"), Body)) return nullptr;
	OutController->InitPlayerState();
	if (AWoTGameMode* Game = World.GetAuthGameMode<AWoTGameMode>())
	{
		Game->ChangeTeam(OutController, 255);   // a unique free-for-all team
	}
	// A locally controlled body runs its own movement, as the host's player does.
	OutController->SetAsLocalPlayerController();
	OutController->Possess(Body);
	Body->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	WoTTest::TickWorld(&World, 1.0f, Step);
	Test.TestFalse(FString::Printf(TEXT("the player stands on the floor (at %s)"),
		*Body->GetActorLocation().ToString()), Body->GetCharacterMovement()->IsFalling());
	return Body;
}

// Moves Body to To as a relocation would: no sweep, physics teleported.
void Relocate(AActor& Body, const FVector& To)
{
	Body.SetActorLocation(To, false, nullptr, ETeleportType::TeleportPhysics);
}

bool IsNearPlayerStart(UWorld& World, const FVector& Location)
{
	for (TActorIterator<APlayerStart> It(&World); It; ++It)
	{
		if (FVector::Dist2D(It->GetActorLocation(), Location) < 1.0) return true;
	}
	return false;
}
}

// The bot soak's runaway decoration, knocked as the soak saw it (178 and -1328
// units per second sideways). BounceableDecoration.uc collides with the world
// (bCollideWorld) and bounces back off walls and floors (:48-86), so it comes to
// rest inside the level and never approaches the terminal speed. The port lost
// the decoration's world collision when it finished spawning, so the knock
// carried it through the floor and it fell for minutes, past 40,000 units per
// second.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutKnockedDecorationSpec,
	"WoT.Falling.OutsideLevel.KnockedDecorationStaysCapped", WoTFellOutRealMap::Flags)

bool FWoTFellOutKnockedDecorationSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	FBuiltMap Built;
	if (!LoadAndBuild(*this, TEXT("Arena_01"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	AWoTBounceableDecoration* Decoration = SoakDecoration(*this, Built);
	if (!Decoration) return false;
	WoTTest::TickWorld(&World, 0.2f, Step);
	const FVector Rest = Decoration->GetActorLocation();
	TestTrue(TEXT("the decoration rests inside the level"), ZoneAt(World, Rest) > 0);

	Knock(*Decoration, FVector(8900.0, -66400.0, 0.0));
	TestTrue(TEXT("the knock sets it bouncing at the soak's sideways speed"),
		Decoration->IsBouncing()
		&& FMath::IsNearlyEqual(Decoration->GetBounceVelocity().Size2D(),
			FVector2D(178.0, -1328.0).Size(), 1.0));
	double PeakSpeed = 0.0;
	double LowestZ = Rest.Z;
	int32 OutsideTicks = 0;
	for (int32 Tick = 0; Tick < 180 && IsValid(Decoration); ++Tick)
	{
		WoTTest::TickWorld(&World, 1, Step);
		if (!IsValid(Decoration)) break;
		PeakSpeed = FMath::Max(PeakSpeed, Decoration->GetBounceVelocity().Size());
		LowestZ = FMath::Min(LowestZ, Decoration->GetActorLocation().Z);
		if (ZoneAt(World, Decoration->GetActorLocation()) == 0) ++OutsideTicks;
	}
	if (!TestTrue(TEXT("the knocked decoration still exists"), IsValid(Decoration)))
		return false;
	AddInfo(FString::Printf(TEXT("knocked from %s: peak speed %.1f, lowest Z %.1f, now at %s"),
		*Rest.ToString(), PeakSpeed, LowestZ, *Decoration->GetActorLocation().ToString()));
	TestTrue(FString::Printf(TEXT("its speed stays within the terminal speed (peak %.1f)"),
		PeakSpeed), PeakSpeed <= TerminalSpeed + 0.01);
	TestEqual(TEXT("it never leaves the level"), OutsideTicks, 0);
	TestTrue(FString::Printf(TEXT("it never sinks through its floor (lowest %.1f, rest %.1f)"),
		LowestZ, Rest.Z), LowestZ > Rest.Z - 2.0);
	const FVector Settled = Decoration->GetActorLocation();
	WoTTest::TickWorld(&World, 0.5f, Step);
	TestTrue(FString::Printf(TEXT("it comes to rest on a floor (at %s)"), *Settled.ToString()),
		Decoration->GetBounceVelocity().IsNearlyZero()
		&& Decoration->GetActorLocation().Equals(Settled, 0.01));
	TestTrue(TEXT("it rests inside the level"),
		ZoneAt(World, Decoration->GetActorLocation()) > 0);
	return !HasAnyErrors();
}

// A decoration that is outside the level when a falling step begins is removed:
// the original stops a falling non-pawn actor outside the level and destroys
// it. One that rests outside the level is not falling and is left alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutDecorationRemovedSpec,
	"WoT.Falling.OutsideLevel.DecorationOutsideIsRemoved", WoTFellOutRealMap::Flags)

bool FWoTFellOutDecorationRemovedSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	FBuiltMap Built;
	if (!LoadAndBuild(*this, TEXT("Arena_01"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	AWoTBounceableDecoration* Decoration = SoakDecoration(*this, Built);
	if (!Decoration) return false;
	WoTTest::TickWorld(&World, 0.2f, Step);

	// Off the level: under its own floor, in the void.
	const FVector Outside = Decoration->GetActorLocation() - FVector(0.0, 0.0, 300.0);
	TestEqual(TEXT("the spot under the floor is outside the level"), ZoneAt(World, Outside), 0);
	Relocate(*Decoration, Outside);
	WoTTest::TickWorld(&World, 1.0f, Step);
	TestTrue(TEXT("a decoration resting outside the level is left alone"),
		IsValid(Decoration) && Decoration->GetActorLocation().Equals(Outside, 0.01));

	// Pushed, it starts falling outside the level.
	const TWeakObjectPtr<AWoTBounceableDecoration> Pushed(Decoration);
	Knock(*Decoration, FVector(0.0, -20000.0, 0.0));
	FVector LastSeen = Outside;
	double PeakSpeed = 0.0;
	int32 Ticks = 0;
	for (; Ticks < 60 && Pushed.IsValid(); ++Ticks)
	{
		WoTTest::TickWorld(&World, 1, Step);
		if (Pushed.IsValid())
		{
			LastSeen = Pushed->GetActorLocation();
			PeakSpeed = FMath::Max(PeakSpeed, Pushed->GetBounceVelocity().Size());
		}
	}
	AddInfo(FString::Printf(TEXT("pushed outside: %s after %d ticks, last seen at %s"),
		Pushed.IsValid() ? TEXT("still present") : TEXT("removed"), Ticks, *LastSeen.ToString()));
	TestFalse(FString::Printf(TEXT("the pushed decoration is removed (last seen at %s, peak speed %.1f)"),
		*LastSeen.ToString(), PeakSpeed), Pushed.IsValid());
	TestTrue(FString::Printf(TEXT("on its first falling steps (%d ticks)"), Ticks), Ticks <= 2);
	return !HasAnyErrors();
}

// Every other kind of falling actor that is outside the level is removed on its
// next falling step too: the bodies and gore a death outside the level leaves,
// dropped items, broken decorations' debris, thrown decals, dropped weapons and
// lobbed projectiles.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutEveryKindRemovedSpec,
	"WoT.Falling.OutsideLevel.EveryFallingKindIsRemoved", WoTFellOutRealMap::Flags)

bool FWoTFellOutEveryKindRemovedSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	FBuiltMap Built;
	if (!LoadAndBuild(*this, TEXT("Arena_01"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	const FVector Void(-4291.277, 4592.466, -1400.0);
	TestEqual(TEXT("the spot under Arena_01's lowest floor is outside the level"),
		ZoneAt(World, Void), 0);

	// Projected actors outside the level that are not falling are left alone.
	TArray<TWeakObjectPtr<AActor>> PlacedOutside;
	TArray<FString> PlacedOutsideClasses;
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		if (It->GetName().StartsWith(TEXT("WoTProjected_"))
			&& ZoneAt(World, It->GetActorLocation()) == 0)
		{
			PlacedOutside.Add(*It);
			PlacedOutsideClasses.Add(It->GetClass()->GetName());
		}
	}
	AddInfo(FString::Printf(TEXT("%d projected actors stand outside the level: %s"),
		PlacedOutside.Num(), *FString::Join(PlacedOutsideClasses, TEXT(", "))));

	struct FFaller
	{
		FString Kind;
		TWeakObjectPtr<AActor> Actor;
	};
	TArray<FFaller> Fallers;
	auto Spot = [&Void](int32 Index) { return Void + FVector(40.0 * Index, 0.0, 0.0); };
	if (AWoTCarcass* Carcass = World.SpawnActor<AWoTCarcass>(Spot(0), FRotator::ZeroRotator,
		AlwaysSpawn()))
	{
		Carcass->SetVelocityForTest(FVector(0.0, 0.0, -100.0));
		Fallers.Add({TEXT("carcass"), Carcass});
	}
	if (AWoTCreatureChunk* Chunk = World.SpawnActor<AWoTCreatureChunk>(Spot(1),
		FRotator::ZeroRotator, AlwaysSpawn()))
	{
		Chunk->SetVelocityForTest(FVector(0.0, 0.0, -100.0));
		Fallers.Add({TEXT("gib chunk"), Chunk});
	}
	if (AWoTAngrealInvFireball* Item = World.SpawnActor<AWoTAngrealInvFireball>(
		Spot(2), FRotator::ZeroRotator, AlwaysSpawn()))
	{
		// A drop has to fit the level, so it is made inside the level, over the low
		// floor; the falling item is then carried outside.
		const AActor* LowStart = Built.Projected(LowFloorStartRecord);
		Item->DropFrom(LowStart ? LowStart->GetActorLocation() : Spot(2));
		TestTrue(TEXT("the dropped item falls"), Item->IsFalling());
		Relocate(*Item, Spot(2));
		Fallers.Add({TEXT("dropped item"), Item});
	}
	if (AWoTBreakableDecoration* Breakable = World.SpawnActor<AWoTBreakableDecoration>(
		Spot(3), FRotator::ZeroRotator, AlwaysSpawn()))
	{
		Breakable->StartFalling(FVector::ZeroVector);
		Fallers.Add({TEXT("breakable decoration"), Breakable});
	}
	if (AWoTTossedWeapon* Weapon = World.SpawnActor<AWoTTossedWeapon>(Spot(4),
		FRotator::ZeroRotator, AlwaysSpawn()))
	{
		Fallers.Add({TEXT("tossed weapon"), Weapon});
	}
	if (AWoTFragment* Fragment = World.SpawnActor<AWoTFragment>(Spot(5),
		FRotator::ZeroRotator, AlwaysSpawn()))
	{
		Fragment->SetFragmentVelocity(FVector(0.0, 0.0, -100.0));
		Fallers.Add({TEXT("debris fragment"), Fragment});
	}
	{
		const FTransform Transform(FRotator::ZeroRotator, Spot(6));
		AWoTSpewChunk* Spew = World.SpawnActorDeferred<AWoTSpewChunk>(
			AWoTSpewChunk::StaticClass(), Transform);
		if (Spew)
		{
			Spew->bCollision = true;
			Spew->bSilent = true;
			Spew->CollisionRadius = 5.0f;
			Spew->CollisionHeight = 5.0f;
			Spew->InitialVelocity = FVector(0.0, 0.0, -100.0);
			Spew->MinBounceSpeed = 10.0f;
			Spew->SpeedDampeningRatio = 0.33f;
			Spew->FinishSpawning(Transform);
			Fallers.Add({TEXT("spewed chunk"), Spew});
		}
	}
	if (AWoTDecal* Decal = World.SpawnActor<AWoTDecal>(Spot(7), FRotator::ZeroRotator,
		AlwaysSpawn()))
	{
		Decal->bFlying = true;
		Decal->Velocity = FVector(0.0, 0.0, -100.0);
		Decal->SetActorTickEnabled(true);
		Fallers.Add({TEXT("thrown decal"), Decal});
	}
	// A lobbed projectile has falling physics (AngrealEarthTremorProjectile.uc).
	if (AWoTEarthTremorProjectile* Lob = World.SpawnActor<AWoTEarthTremorProjectile>(
		Spot(8), FRotator::ZeroRotator, AlwaysSpawn()))
	{
		Fallers.Add({TEXT("lobbed projectile"), Lob});
	}
	TestEqual(TEXT("every falling kind was spawned"), Fallers.Num(), 9);
	WoTTest::TickWorld(&World, 0.2f, Step);
	for (const FFaller& Faller : Fallers)
	{
		TestFalse(FString::Printf(TEXT("a falling %s outside the level is removed%s"), *Faller.Kind,
			Faller.Actor.IsValid() ? *FString::Printf(TEXT(" (still at %s)"),
				*Faller.Actor->GetActorLocation().ToString()) : TEXT("")),
			Faller.Actor.IsValid());
	}
	WoTTest::TickWorld(&World, 1.0f, Step);
	int32 Survivors = 0;
	for (const TWeakObjectPtr<AActor>& Placed : PlacedOutside) Survivors += Placed.IsValid() ? 1 : 0;
	TestEqual(TEXT("placed actors outside the level that do not fall stay"), Survivors,
		PlacedOutside.Num());
	return !HasAnyErrors();
}

// A player whose body ends up outside the level, as the shift and swap defects
// left bodies: the original's falling physics kills a pawn outside the level
// through the ordinary death path, with health below zero, no killer and the
// fell damage type. The death scores as an accident (giWOT.uc:110-138), or as a
// kill for whoever displaced the body moments before; the player then respawns
// at a start as after any death.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutPawnSpec,
	"WoT.Falling.OutsideLevel.PawnDiesAndRespawns", WoTFellOutRealMap::Flags)

bool FWoTFellOutPawnSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	FBuiltMap Built;
	if (!LoadAndBuild(*this, TEXT("Arena_01"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	AWoTGameMode* Game = World.GetAuthGameMode<AWoTGameMode>();
	const AActor* Start = Built.Projected(LowFloorStartRecord);
	if (!TestNotNull(TEXT("game mode"), Game)
		|| !TestNotNull(TEXT("Arena_01's low-floor player start"), Start)) return false;

	AWoTPlayerController* Controller = nullptr;
	AWoTTestFellRecorderCharacter* Body = StandPlayer<AWoTTestFellRecorderCharacter>(
		*this, World, Start->GetActorLocation(), Controller);
	if (!Body) return false;
	AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("player state"), State)) return false;
	// It carries a seal: Seal.uc:338-344 drops a seal that cannot be placed where
	// its carrier stands at the nearest path node instead.
	AWoTSeal* Seal = World.SpawnActor<AWoTSeal>(Body->GetActorLocation()
		+ FVector(200.0, 0.0, 0.0), FRotator::ZeroRotator, AlwaysSpawn());
	TestTrue(TEXT("the player carries a seal"), Seal && Seal->TryPickupBy(Body));

	const FVector Outside = Body->GetActorLocation() - FVector(0.0, 0.0, 300.0);
	TestEqual(TEXT("the spot under the floor is outside the level"), ZoneAt(World, Outside), 0);
	Relocate(*Body, Outside);
	int32 Ticks = 0;
	for (; Ticks < 90 && Body->DiedCalls == 0; ++Ticks) WoTTest::TickWorld(&World, 1, Step);
	AddInfo(FString::Printf(TEXT("moved to %s: %s after %d ticks, at %s, damage type %s"),
		*Outside.ToString(), Body->DiedCalls ? TEXT("died") : TEXT("still alive"), Ticks,
		*Body->GetActorLocation().ToString(), *Body->DiedDamageType.ToString()));
	TestEqual(FString::Printf(TEXT("the body outside the level dies (after %d ticks, at %s)"),
		Ticks, *Body->GetActorLocation().ToString()), Body->DiedCalls, 1);
	TestTrue(TEXT("on its first falling steps"), Ticks <= 3);
	TestEqual(TEXT("with the fell damage type"), Body->DiedDamageType, FName(TEXT("fell")));
	TestFalse(TEXT("and no killer"), Body->bDiedWithKiller);
	TestEqual(TEXT("its health is below zero"), Body->HealthAtDeath, -1);
	TestTrue(TEXT("its fall stops"), Body->bDead
		&& Body->GetCharacterMovement()->MovementMode == MOVE_None);
	TestEqual(TEXT("an accident counts as a suicide"), State->Suicides, 1);
	TestEqual(TEXT("not as a death"), State->Deaths, 0);
	TestEqual(TEXT("the player waits to respawn"),
		static_cast<int32>(Controller->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::Dying));
	WoTTest::TickWorld(&World, 0.5f, Step);
	TestEqual(TEXT("the body dies only once"), Body->DiedCalls, 1);
	if (IsValid(Seal))
	{
		double NearestNode = TNumericLimits<double>::Max();
		for (TActorIterator<AWoTPathNode> It(&World); It; ++It)
		{
			NearestNode = FMath::Min(NearestNode,
				FVector::Dist2D(It->GetActorLocation(), Seal->GetActorLocation()));
		}
		AddInfo(FString::Printf(TEXT("the seal lies at %s, %.1f from the nearest path node"),
			*Seal->GetActorLocation().ToString(), NearestNode));
		TestTrue(FString::Printf(TEXT("the seal is dropped at a path node inside the level (at %s, %.1f from a node)"),
			*Seal->GetActorLocation().ToString(), NearestNode),
			!Seal->bHeldItem && Seal->GetOwnerInventory() == nullptr
			&& ZoneAt(World, Seal->GetActorLocation()) > 0 && NearestNode < 1.0);
	}
	else
	{
		AddError(TEXT("the dropped seal survives"));
	}

	WoTTest::TickWorld(&World, Game->RespawnDelay + 0.1f, Step);
	Controller->Fire();
	AWoTCharacter* Respawned = Cast<AWoTCharacter>(Controller->GetPawn());
	if (!TestNotNull(TEXT("fire respawns the player"), Respawned)) return false;
	TestTrue(TEXT("in a new body"), Respawned != Body);
	WoTTest::TickWorld(&World, 1.0f, Step);
	TestTrue(FString::Printf(TEXT("the respawned player stands at a start (at %s)"),
		*Respawned->GetActorLocation().ToString()),
		IsNearPlayerStart(World, Respawned->GetActorLocation())
		&& !Respawned->GetCharacterMovement()->IsFalling());
	TestTrue(TEXT("inside the level"), ZoneAt(World, Respawned->GetActorLocation()) > 0);
	TestTrue(TEXT("alive"), Respawned->IsAlive());

	// Displaced by another player moments before: the fall is that player's kill.
	AWoTPlayerController* CasterController = nullptr;
	AWoTCharacter* Caster = StandPlayer<AWoTCharacter>(*this, World,
		Built.Projected(50) ? Built.Projected(50)->GetActorLocation() : Start->GetActorLocation(),
		CasterController);
	if (!Caster) return false;
	AWoTPlayerState* CasterState = CasterController->GetPlayerState<AWoTPlayerState>();
	Respawned->SetSuicideInstigator(Caster);
	Relocate(*Respawned, Outside);
	for (int32 Tick = 0; Tick < 90 && Respawned->IsAlive(); ++Tick)
		WoTTest::TickWorld(&World, 1, Step);
	TestFalse(TEXT("the displaced body dies outside the level"), Respawned->IsAlive());
	TestEqual(TEXT("the displacing player is credited the kill"),
		CasterState ? CasterState->Kills : -1, 1);
	TestEqual(TEXT("the victim takes a death"), State->Deaths, 1);
	TestEqual(TEXT("not another suicide"), State->Suicides, 1);
	return !HasAnyErrors();
}

// A creature outside the level dies the same way. The original clears its held
// weapon before the death path, so nothing is tossed: its weapons go with the
// rest of its inventory.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutCreatureSpec,
	"WoT.Falling.OutsideLevel.CreatureDiesWithoutTossingWeapons", WoTFellOutRealMap::Flags)

bool FWoTFellOutCreatureSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	FBuiltMap Built;
	if (!LoadAndBuild(*this, TEXT("Arena_01"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	const AActor* Start = Built.Projected(LowFloorStartRecord);
	if (!TestNotNull(TEXT("Arena_01's low-floor player start"), Start)) return false;
	TArray<FName> Rows;
	WoTCreatureData::GetAllRowNames(Rows);
	Rows.Sort(FNameLexicalLess());
	FName TwoWeaponRow;
	for (FName Name : Rows)
	{
		const FWoTCreatureRow* Row = WoTCreatureData::Find(Name);
		if (Row && Row->MeleeWeaponType != Row->RangedWeaponType
			&& WoTNPCWeaponData::Find(Row->MeleeWeaponType)
			&& WoTNPCWeaponData::Find(Row->RangedWeaponType))
		{
			TwoWeaponRow = Name;
			break;
		}
	}
	AWoTNPCCharacter* Creature = WoTCreatureData::SpawnCreature(World, TwoWeaponRow,
		Start->GetActorLocation(), 1);
	if (!TestNotNull(TEXT("a shipped two-weapon creature"), Creature)) return false;
	WoTTest::TickWorld(&World, 1.0f, Step);
	const TWeakObjectPtr<AWoTNPCWeapon> Melee(Creature->MeleeWeapon);
	const TWeakObjectPtr<AWoTNPCWeapon> Ranged(Creature->RangedWeapon);
	TestTrue(TEXT("the creature holds both weapons"), Melee.IsValid() && Ranged.IsValid());
	TestTrue(TEXT("and stands inside the level"),
		ZoneAt(World, Creature->GetActorLocation()) > 0);

	const TWeakObjectPtr<AWoTNPCCharacter> Weak(Creature);
	Relocate(*Creature, Creature->GetActorLocation() - FVector(0.0, 0.0, 300.0));
	for (int32 Tick = 0; Tick < 90 && Weak.IsValid() && Weak->IsAlive(); ++Tick)
		WoTTest::TickWorld(&World, 1, Step);
	TestTrue(TEXT("the creature outside the level dies"),
		!Weak.IsValid() || !Weak->IsAlive());
	WoTTest::TickWorld(&World, 0.5f, Step);
	int32 Tossed = 0;
	for (TActorIterator<AWoTTossedWeapon> It(&World); It; ++It) ++Tossed;
	TestEqual(TEXT("no weapon is tossed"), Tossed, 0);
	TestFalse(TEXT("its weapons are gone"), Melee.IsValid() || Ranged.IsValid());
	return !HasAnyErrors();
}

// A body that falls inside the level, into a pit with a floor, is unaffected: it
// lands and lives. Arena_09's lowest floor (Z -8170) lies 256 units under the
// floor at PlayerStart 822, and the void begins right under it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutPitSpec,
	"WoT.Falling.OutsideLevel.FallIntoPitIsUnaffected", WoTFellOutRealMap::Flags)

bool FWoTFellOutPitSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	FBuiltMap Built;
	if (!LoadAndBuild(*this, TEXT("Arena_09"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	struct FPit
	{
		int32 StartRecord;
		FVector Offset;
		double PitFloor;
	};
	const FPit Pits[] = {
		{822, FVector(0.0, 625.0, 0.0), -8170.0},   // into the level's lowest pit
		{840, FVector(-300.0, 0.0, 0.0), -7914.0},  // off the raised ledge
	};
	for (const FPit& Pit : Pits)
	{
		const AActor* Start = Built.Projected(Pit.StartRecord);
		if (!TestNotNull(TEXT("Arena_09 player start"), Start)) continue;
		AWoTPlayerController* Controller = nullptr;
		AWoTTestFellRecorderCharacter* Body = StandPlayer<AWoTTestFellRecorderCharacter>(
			*this, World, Start->GetActorLocation(), Controller);
		if (!Body) continue;
		const FVector Over = Body->GetActorLocation() + Pit.Offset;
		Relocate(*Body, Over);
		Body->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		bool bFell = false;
		int32 OutsideTicks = 0;
		for (int32 Tick = 0; Tick < 90; ++Tick)
		{
			WoTTest::TickWorld(&World, 1, Step);
			bFell |= Body->GetCharacterMovement()->IsFalling();
			OutsideTicks += ZoneAt(World, Body->GetActorLocation()) == 0 ? 1 : 0;
		}
		const FVector Landed = Body->GetActorLocation();
		TestTrue(TEXT("the body falls"), bFell);
		TestTrue(FString::Printf(TEXT("and lands on the pit's floor (at %s)"), *Landed.ToString()),
			!Body->GetCharacterMovement()->IsFalling()
			&& FMath::Abs(Landed.Z - Body->GetSimpleCollisionHalfHeight() - Pit.PitFloor) < 4.0);
		TestEqual(TEXT("it is never outside the level"), OutsideTicks, 0);
		TestEqual(TEXT("it does not die"), Body->DiedCalls, 0);
		TestTrue(TEXT("it is alive"), Body->IsAlive());
		Controller->UnPossess();
		Body->Destroy();
	}
	return !HasAnyErrors();
}

// UE5's own kill height ends the same way: a body below it dies through the
// original's out-of-level death, exactly once, whichever check sees it first,
// instead of being silently destroyed. On a real map with the kill height just
// under the level both checks see the body; on a map without the level's zone
// classification only the kill height can.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutKillHeightSpec,
	"WoT.Falling.OutsideLevel.KillHeightDiesOnce", WoTFellOutRealMap::Flags)

bool FWoTFellOutKillHeightSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	auto FallBelowKillHeight = [this](UWorld& World, const FVector& Standing,
		const TCHAR* Case)
	{
		World.GetWorldSettings()->KillZ = Standing.Z - 200.0f;
		AWoTPlayerController* Controller = nullptr;
		AWoTTestFellRecorderCharacter* Body = StandPlayer<AWoTTestFellRecorderCharacter>(
			*this, World, Standing, Controller);
		if (!Body) return;
		const TWeakObjectPtr<AWoTTestFellRecorderCharacter> Weak(Body);
		Relocate(*Body, Body->GetActorLocation() - FVector(0.0, 0.0, 400.0));
		WoTTest::TickWorld(&World, 0.5f, Step);
		if (!TestTrue(FString::Printf(TEXT("%s: the body is kept, not silently destroyed"), Case),
			Weak.IsValid())) return;
		TestEqual(FString::Printf(TEXT("%s: it dies exactly once"), Case), Body->DiedCalls, 1);
		TestEqual(FString::Printf(TEXT("%s: with the fell damage type"), Case),
			Body->DiedDamageType, FName(TEXT("fell")));
		TestEqual(FString::Printf(TEXT("%s: the player waits to respawn"), Case),
			static_cast<int32>(Controller->GetPlayerStateMachineState()),
			static_cast<int32>(EWoTPlayerStateMachineState::Dying));
	};

	{
		FBuiltMap Built;
		if (LoadAndBuild(*this, TEXT("Arena_01"), Built))
		{
			const AActor* Start = Built.Projected(LowFloorStartRecord);
			if (TestNotNull(TEXT("Arena_01's low-floor player start"), Start))
			{
				FallBelowKillHeight(*Built.World, Start->GetActorLocation(), TEXT("real map"));
			}
		}
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	AActor* Floor = World->SpawnActor<AActor>(AlwaysSpawn());
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
	Box->SetBoxExtent(FVector(2000.0, 2000.0, 50.0));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Floor->SetRootComponent(Box);
	Box->RegisterComponent();
	Floor->SetActorLocation(FVector(0.0, 0.0, -50.0));
	World->SpawnActor<APlayerStart>(FVector(500.0, 0.0, 100.0), FRotator::ZeroRotator,
		AlwaysSpawn());
	TestFalse(TEXT("the spec world has no zone classification"),
		WoTIsPointOutsideWorld(World, FVector(0.0, 0.0, -100000.0)));
	FallBelowKillHeight(*World, FVector(0.0, 0.0, 100.0), TEXT("no zone classification"));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// The outside-the-level test works for the imported editor maps as well as the
// maps a release build makes at runtime: the imported map's own zone table
// agrees with the runtime level's at every player start and decoration and over
// a grid around the whole level.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutImportedMapSpec,
	"WoT.Falling.OutsideLevel.ImportedMapAgreesWithRuntime", WoTFellOutRealMap::Flags)

bool FWoTFellOutImportedMapSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	const UWoTMapLightTable* Imported = LoadObject<UWoTMapLightTable>(nullptr,
		TEXT("/Game/WoT/Maps/LightTables/LT_Arena_01.LT_Arena_01"));
	if (!Imported)
	{
		AddWarning(TEXT("the imported editor map is absent: its zone table is not verified"));
		return true;
	}
	FBuiltMap Built;
	if (!LoadAndBuild(*this, TEXT("Arena_01"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	const UWoTMapLightTable* Runtime = UWoTMeshLight::FindMapLightTable(&World);
	if (!TestNotNull(TEXT("the runtime level registers its zone table"), Runtime)) return false;

	TArray<FVector> Points;
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		if (It->IsA<APlayerStart>() || It->IsA<AWoTDecoration>()) Points.Add(It->GetActorLocation());
	}
	const FBox Bounds = Built.Level->GetComponentsBoundingBox(true).ExpandBy(500.0);
	constexpr int32 Steps = 24;
	for (int32 X = 0; X <= Steps; ++X)
	for (int32 Y = 0; Y <= Steps; ++Y)
	for (int32 Z = 0; Z <= Steps; ++Z)
	{
		Points.Add(Bounds.Min + (Bounds.Max - Bounds.Min)
			* FVector(X, Y, Z) / static_cast<double>(Steps));
	}
	int32 Inside = 0;
	int32 Outside = 0;
	int32 Disagreements = 0;
	FString FirstDisagreement;
	for (const FVector& Point : Points)
	{
		int32 RuntimeZone = INDEX_NONE;
		int32 ImportedZone = INDEX_NONE;
		int32 Leaf = INDEX_NONE;
		const bool bRuntime = UWoTMeshLight::ClassifyRegion(Runtime, Point, RuntimeZone, Leaf);
		const bool bImported = UWoTMeshLight::ClassifyRegion(Imported, Point, ImportedZone, Leaf);
		if (!bRuntime || !bImported || (RuntimeZone == 0) != (ImportedZone == 0))
		{
			if (Disagreements++ == 0)
			{
				FirstDisagreement = FString::Printf(TEXT("%s runtime %d imported %d"),
					*Point.ToString(), RuntimeZone, ImportedZone);
			}
			continue;
		}
		(RuntimeZone == 0 ? Outside : Inside) += 1;
	}
	TestEqual(FString::Printf(TEXT("both tables place every point the same side of the level (first difference: %s)"),
		*FirstDisagreement), Disagreements, 0);
	TestTrue(FString::Printf(TEXT("the samples cover both sides (%d inside, %d outside)"),
		Inside, Outside), Inside > 100 && Outside > 100);
	return !HasAnyErrors();
}

// BounceableDecoration.uc:33-34 bounces with falling physics. The original's
// falling step adds only half the zone's gravity while an actor bounces, and the
// full gravity, bounded by the terminal speed, once it has stopped bouncing (the
// same rule the breakable decoration follows). The port added UE5's full world
// gravity throughout, so a bouncing decoration fell twice as fast.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutBounceGravitySpec,
	"WoT.Falling.Bounceable.HalfGravityWhileBouncing", WoTFellOutRealMap::Flags)

bool FWoTFellOutBounceGravitySpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	AWoTBounceableDecoration* Decoration = World->SpawnActor<AWoTBounceableDecoration>(
		FVector(0.0, 0.0, 10000.0), FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("bounceable decoration"), Decoration))
	{
		Decoration->ConfigureRuntimeCollision(true, false, false, 5.0f, 5.0f);
		Decoration->SetupBounce(FVector(0.0, 0.0, 10.0));
		WoTTest::TickWorld(World, 20, 0.05f);
		TestTrue(FString::Printf(TEXT("a second of bouncing fall gains half the zone's 950 (velocity %.2f)"),
			Decoration->GetBounceVelocity().Z),
			FMath::IsNearlyEqual(Decoration->GetBounceVelocity().Z, 10.0 - 475.0, 0.01));
		TestTrue(TEXT("with no contact it is still bouncing"), Decoration->IsBouncing());
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Placements that fit the level as the original's do, a zone walk at the
// original's precision, and a bouncing decoration that collides as the
// original's does. Each of these, done otherwise, ended in a removal or a death
// once actors outside the level were removed.
// ---------------------------------------------------------------------------

namespace WoTFellOutRealMap
{
// A finding the spec reports, also written to the log.
void Note(FAutomationTestBase& Test, const FString& Text)
{
	Test.AddInfo(Text);
	UE_LOG(LogTemp, Display, TEXT("FELLOUT: %s"), *Text);
}

// The floor under Point within Depth (the level or a mover).
bool FloorUnder(UWorld& World, const FVector& Point, double Depth, FHitResult& OutHit)
{
	return WoTVisBlockingTrace(&World, Point, Point - FVector(0.0, 0.0, Depth), nullptr,
		OutHit) && OutHit.ImpactNormal.Z > 0.7;
}

// Every player start and path node of the map, in a stable order.
TArray<FVector> NavigationSpots(UWorld& World)
{
	TArray<FVector> Spots;
	for (TActorIterator<APlayerStart> It(&World); It; ++It) Spots.Add(It->GetActorLocation());
	for (TActorIterator<AWoTPathNode> It(&World); It; ++It) Spots.Add(It->GetActorLocation());
	Spots.Sort([](const FVector& A, const FVector& B)
	{
		return A.X != B.X ? A.X < B.X : A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z;
	});
	return Spots;
}

FVector CapsuleExtent(const ACharacter& Body)
{
	const UCapsuleComponent* Capsule = Body.GetCapsuleComponent();
	return FVector(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleRadius(),
		Capsule->GetScaledCapsuleHalfHeight());
}

// The body's cylinder fits the level where it stands: it crosses no level
// geometry and is not in solid space.
bool FitsLevel(UWorld& World, const ACharacter& Body)
{
	return WoTUE1Collision::LevelPointCheckFits(World, Body.GetActorLocation(),
		CapsuleExtent(Body));
}

// A player body placed at Location, without touching anything on the way, whose
// controller has a team of its own.
AWoTCharacter* PlacePlayer(UWorld& World, const FVector& Location)
{
	AWoTPlayerController* Controller = World.SpawnActor<AWoTPlayerController>(AlwaysSpawn());
	AWoTCharacter* Body = World.SpawnActor<AWoTCharacter>(Location + FVector(0.0, 0.0, 5000.0),
		FRotator::ZeroRotator, AlwaysSpawn());
	if (!Controller || !Body) return nullptr;
	Controller->InitPlayerState();
	if (AWoTGameMode* Game = World.GetAuthGameMode<AWoTGameMode>())
	{
		Game->ChangeTeam(Controller, 255);
	}
	Controller->SetAsLocalPlayerController();
	Controller->Possess(Body);
	WoTTest::PlaceWithoutOverlapEvents(Body, Location);
	return Body;
}
}

// EarthTremorRock.uc:108-131: a rock traces down to its floor and stands at the
// trace's hit location, which the original backs off 0.5 units toward the trace's
// start. Its lava chunks (:237-240) spawn there and fall. A rock standing on the
// floor plane itself spawned chunks whose first falling step could begin on the
// solid side of the floor, where they were removed as outside the level.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutTremorRockSpec,
	"WoT.Falling.Placement.TremorRockStandsOffItsFloor", WoTFellOutRealMap::Flags)

bool FWoTFellOutTremorRockSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	const TCHAR* const Maps[] = {TEXT("Arena_01"), TEXT("Arena_09")};
	for (const TCHAR* Map : Maps)
	{
		FBuiltMap Built;
		if (!LoadAndBuild(*this, Map, Built)) return !HasAnyErrors();
		UWorld& World = *Built.World;
		// BurningChunk.uc Touch and GenericProjectile.uc:195-202 consume chunks
		// on shoot targets. This spec isolates floor placement, so exclude those
		// contacts while retaining all level and mover collision.
		for (TActorIterator<AWoTTrigger> It(&World); It; ++It)
		{
			if (It->IsRetailProjectileTarget())
				It->Capsule->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Ignore);
		}
		int32 Rocks = 0;
		int32 OffTheFloor = 0;
		FString FirstOnTheFloor;
		TSet<const AWoTEarthTremorChunk*> Seen;
		TArray<TWeakObjectPtr<AWoTEarthTremorChunk>> Chunks;
		for (const FVector& Spot : NavigationSpots(World))
		{
			if (Rocks >= 40) break;
			FHitResult Floor;
			if (!FloorUnder(World, Spot, 200.0, Floor)) continue;
			const FVector Up = Floor.ImpactNormal.GetSafeNormal();
			// Raised a little over its floor, as a tremor's centre or ring slot is.
			const FTransform At(FRotationMatrix::MakeFromZ(Up).Rotator(),
				Floor.ImpactPoint + Up * 10.0);
			AWoTEarthTremorRock* Rock = World.SpawnActorDeferred<AWoTEarthTremorRock>(
				AWoTEarthTremorRock::StaticClass(), At, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Rock) continue;
			Rock->InitializeRock(false, true, 1.0f, nullptr);
			Rock->FinishSpawning(At);
			if (!IsValid(Rock)) continue;
			++Rocks;
			if (Rock->GetActorLocation().Equals(Floor.ImpactPoint + Up * 0.5, 0.05))
			{
				++OffTheFloor;
			}
			else if (FirstOnTheFloor.IsEmpty())
			{
				FirstOnTheFloor = FString::Printf(TEXT("rock at %s, floor at %s"),
					*Rock->GetActorLocation().ToString(), *Floor.ImpactPoint.ToString());
			}
			// The rock's spew of lava chunks (EarthTremorRock.uc:237-240).
			AWoTBurningChunk::SpawnFan(World, AWoTEarthTremorChunk::StaticClass(),
				Rock->GetActorLocation(), Rock->RelativeUp, Rock);
			for (TActorIterator<AWoTEarthTremorChunk> It(&World); It; ++It)
			{
				if (!Seen.Contains(*It))
				{
					Seen.Add(*It);
					Chunks.Add(*It);
				}
			}
		}
		const int32 Spewed = Chunks.Num();
		WoTTest::TickWorld(&World, 0.2f, Step);
		int32 Survivors = 0;
		for (const TWeakObjectPtr<AWoTEarthTremorChunk>& Chunk : Chunks)
		{
			Survivors += Chunk.IsValid() ? 1 : 0;
		}
		Note(*this, FString::Printf(TEXT("%s: %d rocks, %d off the floor, %d of %d chunks survive"),
			Map, Rocks, OffTheFloor, Survivors, Spewed));
		TestTrue(FString::Printf(TEXT("%s: rocks stand on many floors (%d)"), Map, Rocks),
			Rocks >= 10);
		TestEqual(FString::Printf(TEXT("%s: every rock stands 0.5 units off its floor (first on it: %s)"),
			Map, *FirstOnTheFloor), OffTheFloor, Rocks);
		TestEqual(FString::Printf(TEXT("%s: five chunks per rock"), Map), Spewed, 5 * Rocks);
		TestEqual(FString::Printf(TEXT("%s: every lava chunk survives its first falling steps"),
			Map), Survivors, Spewed);
	}
	return !HasAnyErrors();
}

// The original keeps an actor's location and the zone tree's planes in single
// precision, so a point that rounds onto a plane lies on its front. A walk at
// double precision put a point a hair under a floor (a trace's impact on it)
// behind the floor's plane, in solid space: outside the level.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutZonePrecisionSpec,
	"WoT.Falling.ZonePrecision.LocationsClassifyAsStored", WoTFellOutRealMap::Flags)

bool FWoTFellOutZonePrecisionSpec::RunTest(const FString&)
{
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>();
	// A floor at Z 100: zone 1 above it, solid space (zone 0) under it.
	Table->ZoneBspPlanes = {FVector4(0.0, 0.0, 1.0, 100.0)};
	Table->ZoneBspFront = {-2};
	Table->ZoneBspBack = {-1};
	auto ZoneOf = [Table](const FVector& Point)
	{
		int32 Zone = INDEX_NONE;
		int32 Leaf = INDEX_NONE;
		return UWoTMeshLight::ClassifyRegion(Table, Point, Zone, Leaf) ? Zone : INDEX_NONE;
	};
	TestEqual(TEXT("a point on the floor lies inside"), ZoneOf(FVector(0.0, 0.0, 100.0)), 1);
	TestEqual(TEXT("a point a hair under the floor rounds onto it and lies inside"),
		ZoneOf(FVector(0.0, 0.0, 100.0 - 1e-9)), 1);
	TestEqual(TEXT("so does one far from the origin"),
		ZoneOf(FVector(12345.678, -23456.5, 100.0 - 1e-6)), 1);
	TestEqual(TEXT("a point further under the floor lies outside"),
		ZoneOf(FVector(0.0, 0.0, 99.999)), 0);
	// A wall at X 30000, where single precision resolves 1/512 of a unit.
	Table->ZoneBspPlanes = {FVector4(1.0, 0.0, 0.0, 30000.0)};
	TestEqual(TEXT("a point that rounds onto a distant wall lies on its front"),
		ZoneOf(FVector(30000.0 - 0.0009, 0.0, 0.0)), 1);
	TestEqual(TEXT("one that rounds behind it lies behind it"),
		ZoneOf(FVector(30000.0 - 0.003, 0.0, 0.0)), 0);
	return !HasAnyErrors();
}

// BounceableDecoration.uc:187-189: a bounceable decoration collides with the
// world, and so with movers, but blocks neither actors nor players. Falling, it
// passes through bodies, gore, debris and other decorations to the floor under
// them, and they pass through it; the spinning artifact's fit test
// (Util.uc:400-443) does not count it. Its touch still hurts (:96-102).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutBounceableCollisionSpec,
	"WoT.Falling.BounceableCollision.WorldAndMoversOnly", WoTFellOutRealMap::Flags)

bool FWoTFellOutBounceableCollisionSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	UStaticMesh* Cube = Cast<UStaticMesh>(
		FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")).TryLoad());
	// The level's floor, its top at Z 0.
	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), FVector(0.0, 0.0, -50.0), FRotator::ZeroRotator,
		AlwaysSpawn());
	if (!TestNotNull(TEXT("floor"), Floor) || !TestNotNull(TEXT("cube mesh"), Cube))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Floor->SetMobility(EComponentMobility::Movable);
	Floor->GetStaticMeshComponent()->SetStaticMesh(Cube);
	Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
	Floor->SetActorScale3D(FVector(80.0, 80.0, 1.0));
	auto Bounceable = [World](const FVector& At)
	{
		AWoTBounceableDecoration* Decoration = World->SpawnActor<AWoTBounceableDecoration>(
			At, FRotator::ZeroRotator, AlwaysSpawn());
		if (Decoration) Decoration->ConfigureRuntimeCollision(true, false, false, 5.0f, 5.0f);
		return Decoration;
	};

	// A body, gore, debris, a breakable decoration and another bounceable one
	// resting on the floor, and a mover platform whose top is at Z 50.
	struct FObstacle
	{
		FString Kind;
		double X;
		bool bBlocks;
	};
	const TArray<FObstacle> Obstacles = {
		{TEXT("carcass"), 0.0, false},
		{TEXT("gib chunk"), 200.0, false},
		{TEXT("debris"), 400.0, false},
		{TEXT("breakable decoration"), 600.0, false},
		{TEXT("bounceable decoration"), 800.0, false},
		{TEXT("mover"), 1000.0, true},
	};
	World->SpawnActor<AWoTCarcass>(FVector(0.0, 0.0, 60.0), FRotator::ZeroRotator, AlwaysSpawn());
	World->SpawnActor<AWoTCreatureChunk>(FVector(200.0, 0.0, 9.0), FRotator::ZeroRotator,
		AlwaysSpawn());
	AWoTBreakableDecoration* Breakable = World->SpawnActor<AWoTBreakableDecoration>(
		FVector(600.0, 0.0, 29.0), FRotator::ZeroRotator, AlwaysSpawn());
	Bounceable(FVector(800.0, 0.0, 5.0));
	if (AWoTMover* Platform = World->SpawnActor<AWoTMover>(FVector(1000.0, 0.0, 25.0),
		FRotator::ZeroRotator, AlwaysSpawn()))
	{
		Platform->Mesh->SetStaticMesh(Cube);
		Platform->SetActorScale3D(FVector(1.0, 1.0, 0.5));
	}
	WoTTest::TickWorld(World, 0.5f, Step);
	// Resting debris is cleaned up within seconds, so it is laid down last.
	World->SpawnActor<AWoTFragment>(FVector(400.0, 0.0, 4.5), FRotator::ZeroRotator,
		AlwaysSpawn());

	// A bounceable decoration knocked loose over each.
	TArray<AWoTBounceableDecoration*> Droppers;
	for (const FObstacle& Obstacle : Obstacles)
	{
		AWoTBounceableDecoration* Dropper = Bounceable(FVector(Obstacle.X, 0.0, 200.0));
		if (Dropper) Dropper->SetupBounce(FVector::ZeroVector);
		Droppers.Add(Dropper);
	}
	// Debris and a body falling onto bounceable decorations resting on the floor.
	Bounceable(FVector(1400.0, 0.0, 5.0));
	AWoTFragment* FallingDebris = World->SpawnActor<AWoTFragment>(FVector(1400.0, 0.0, 120.0),
		FRotator::ZeroRotator, AlwaysSpawn());
	if (FallingDebris) FallingDebris->SetFragmentVelocity(FVector(0.0, 0.0, -10.0));
	Bounceable(FVector(1600.0, 0.0, 5.0));
	AWoTCarcass* FallingBody = World->SpawnActor<AWoTCarcass>(FVector(1600.0, 0.0, 160.0),
		FRotator::ZeroRotator, AlwaysSpawn());
	if (FallingBody) FallingBody->SetVelocityForTest(FVector(0.0, 0.0, -10.0));
	// The lowest each one reaches: the floor when nothing but the world stops it.
	// A dropper's first landing comes within 1.2 seconds, before resting debris is
	// cleaned up from under it.
	TArray<double> DropperLowest;
	DropperLowest.Init(TNumericLimits<double>::Max(), Droppers.Num());
	double DebrisLowest = TNumericLimits<double>::Max();
	double BodyLowest = TNumericLimits<double>::Max();
	const double DebrisHalfHeight = FallingDebris ? FallingDebris->GetSimpleCollisionHalfHeight() : 0.0;
	const double BodyHalfHeight = FallingBody ? FallingBody->GetSimpleCollisionHalfHeight() : 0.0;
	for (int32 Tick = 0; Tick < 90; ++Tick)
	{
		WoTTest::TickWorld(World, 1, Step);
		for (int32 Index = 0; Index < Droppers.Num() && Tick < 36; ++Index)
		{
			if (IsValid(Droppers[Index]))
			{
				DropperLowest[Index] = FMath::Min(DropperLowest[Index],
					Droppers[Index]->GetActorLocation().Z);
			}
		}
		if (IsValid(FallingDebris))
		{
			DebrisLowest = FMath::Min(DebrisLowest, FallingDebris->GetActorLocation().Z);
		}
		if (IsValid(FallingBody))
		{
			BodyLowest = FMath::Min(BodyLowest, FallingBody->GetActorLocation().Z);
		}
	}

	for (int32 Index = 0; Index < Obstacles.Num(); ++Index)
	{
		const FObstacle& Obstacle = Obstacles[Index];
		AWoTBounceableDecoration* Dropper = Droppers[Index];
		if (!TestTrue(FString::Printf(TEXT("the decoration over the %s exists"), *Obstacle.Kind),
			IsValid(Dropper))) continue;
		// Its first landing stops a few units short of where it comes to rest; every
		// obstacle's top stands at least 8 units over the floor.
		const double RestZ = Obstacle.bBlocks ? 55.0 : 5.0;
		TestTrue(FString::Printf(TEXT("dropped on the %s, it comes down %s (lowest Z %.2f)"),
			*Obstacle.Kind, Obstacle.bBlocks ? TEXT("on it") : TEXT("to the floor under it"),
			DropperLowest[Index]), DropperLowest[Index] > RestZ - 1.0
			&& DropperLowest[Index] < RestZ + 4.0);
		// Its cylinder turns with it as it tumbles, so it may come to rest tilted, up
		// to two units higher.
		TestTrue(FString::Printf(TEXT("and rests there (at Z %.2f, bouncing %d)"),
			Dropper->GetActorLocation().Z, Dropper->IsBouncing() ? 1 : 0),
			Dropper->GetActorLocation().Z > RestZ - 1.0
			&& Dropper->GetActorLocation().Z < RestZ + 3.0);
	}
	// A resting bounceable decoration's top is 10 units over the floor.
	TestTrue(FString::Printf(TEXT("debris falls through a resting bounceable decoration to the floor (lowest Z %.2f)"),
		DebrisLowest), FallingDebris && DebrisLowest - DebrisHalfHeight < 3.0);
	// A carcass flattens its cylinder as it lands; its landed height counts.
	const double LandedBodyHalfHeight = IsValid(FallingBody)
		? FallingBody->GetSimpleCollisionHalfHeight() : BodyHalfHeight;
	TestTrue(FString::Printf(TEXT("so does a body (lowest Z %.2f, half-height %.2f)"),
		BodyLowest, LandedBodyHalfHeight),
		FallingBody && BodyLowest - LandedBodyHalfHeight < 3.0);

	// The spinning artifact's fit test counts the breakable decoration, which
	// blocks, but not a bounceable one.
	AWoTCharacter* Fitter = World->SpawnActor<AWoTCharacter>(FVector(3000.0, 0.0, 100.0),
		FRotator::ZeroRotator, AlwaysSpawn());
	AWoTBounceableDecoration* Lone = Bounceable(FVector(2000.0, 0.0, 5.0));
	if (TestNotNull(TEXT("a body to fit"), Fitter) && TestNotNull(TEXT("a lone decoration"), Lone))
	{
		TestTrue(TEXT("a bounceable decoration does not block the fit"),
			UWoTWhirlwindLeech::ActorFits(Fitter, FVector(2000.0, 0.0, 46.0), 128.0f));
		TestFalse(TEXT("a breakable decoration does"),
			IsValid(Breakable)
			&& UWoTWhirlwindLeech::ActorFits(Fitter, FVector(600.0, 0.0, 46.0), 128.0f));
	}

	// A burning decoration hurts whoever touches it (Coal: Damage 1, burned).
	AWoTBounceableDecoration* Coal = Bounceable(FVector(2400.0, 0.0, 5.0));
	AWoTCharacter* Walker = World->SpawnActor<AWoTCharacter>(FVector(2400.0, 400.0, 48.0),
		FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("coal"), Coal) && TestNotNull(TEXT("walker"), Walker))
	{
		Coal->Damage = 1.0f;
		Coal->DamageType = TEXT("burned");
		const int32 HealthBefore = Walker->Health;
		Walker->SetActorLocation(FVector(2400.0, 0.0, 48.0));
		TestEqual(TEXT("touching it costs the walker its damage"), Walker->Health,
			HealthBefore - 1);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// Seal.uc:336-346: a seal that does not fit where its carrier drops it goes to
// the nearest path node instead, at the node's base plus the seal's height
// (:152-182), and the inherited drop places it there with SetLocation, which
// fits the seal to the level: a seal on a node that stands low over its floor is
// lifted out of that floor. Placed there unfitted, it began its fall in the floor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutSealFallbackSpec,
	"WoT.Falling.Placement.SealFallbackFitsAtItsNode", WoTFellOutRealMap::Flags)

bool FWoTFellOutSealFallbackSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	const FVector SealExtent(17.0, 17.0, 3.0);
	const TCHAR* const Maps[] = {TEXT("Battle2_02"), TEXT("Arena_01"), TEXT("Arena_09")};
	for (const TCHAR* Map : Maps)
	{
		FBuiltMap Built;
		if (!LoadAndBuild(*this, Map, Built)) return !HasAnyErrors();
		UWorld& World = *Built.World;
		// The nodes whose seal spot does not fit the level as it stands but does once
		// fitted; a spot in solid space first.
		struct FCandidate
		{
			const AWoTPathNode* Node;
			FVector Spot;
			bool bInSolid;
		};
		TArray<FCandidate> Candidates;
		int32 Nodes = 0;
		for (TActorIterator<AWoTPathNode> It(&World); It; ++It)
		{
			++Nodes;
			FVector Spot = It->GetActorLocation();
			Spot.Z += 3.0 - It->NodeCollisionHeight;
			FVector Fitted = Spot;
			if (WoTUE1Collision::LevelPointCheckFits(World, Spot, SealExtent)
				|| !WoTUE1Collision::FindSpotLevelOnly(World, SealExtent, Fitted, false))
			{
				continue;
			}
			Candidates.Add({*It, Spot, ZoneAt(World, Spot) == 0});
		}
		Candidates.Sort([](const FCandidate& A, const FCandidate& B)
		{
			if (A.bInSolid != B.bInSolid) return A.bInSolid;
			return A.Spot.X != B.Spot.X ? A.Spot.X < B.Spot.X : A.Spot.Y < B.Spot.Y;
		});
		int32 InSolid = 0;
		for (const FCandidate& Candidate : Candidates) InSolid += Candidate.bInSolid ? 1 : 0;
		Note(*this, FString::Printf(TEXT("%s: %d of %d path nodes need their seal spot fitted, %d of them in solid space"),
			Map, Candidates.Num(), Nodes, InSolid));
		for (const FCandidate& Candidate : Candidates)
		{
			// A drop spot deep in the floor under the node, which cannot be fitted and
			// whose nearest node (by Seal.uc:152-182's rule) is this one.
			const FVector Start = Candidate.Spot - FVector(0.0, 0.0, 60.0);
			FVector Probe = Start;
			if (WoTUE1Collision::FindSpotLevelOnly(World, SealExtent, Probe, false)) continue;
			const AWoTPathNode* Nearest = nullptr;
			double NearestDistance = 999999.0;
			for (TActorIterator<AWoTPathNode> It(&World); It; ++It)
			{
				const double Distance = FVector::Dist(Start, It->GetActorLocation());
				if (Distance < NearestDistance)
				{
					NearestDistance = Distance;
					Nearest = *It;
				}
			}
			if (Nearest != Candidate.Node) continue;

			AWoTSeal* Seal = World.SpawnActor<AWoTSeal>(Candidate.Spot + FVector(0.0, 0.0, 300.0),
				FRotator::ZeroRotator, AlwaysSpawn());
			if (!TestNotNull(TEXT("seal"), Seal)) return false;
			Seal->DropFrom(Start);
			const FVector Dropped = Seal->GetActorLocation();
			Note(*this, FString::Printf(TEXT("%s: dropped at %s under the node, the seal goes to %s (node spot %s%s)"),
				Map, *Start.ToString(), *Dropped.ToString(), *Candidate.Spot.ToString(),
				Candidate.bInSolid ? TEXT(", in solid space") : TEXT("")));
			TestTrue(TEXT("the seal goes to the nearest node's spot"),
				FVector::Dist2D(Dropped, Candidate.Spot) < SealExtent.X
				&& FMath::Abs(Dropped.Z - Candidate.Spot.Z) < 20.0);
			TestTrue(TEXT("fitted to the level there"),
				WoTUE1Collision::LevelPointCheckFits(World, Dropped, SealExtent));
			const TWeakObjectPtr<AWoTSeal> Weak(Seal);
			WoTTest::TickWorld(&World, 1.0f, Step);
			if (!TestTrue(TEXT("the seal survives its fall"), Weak.IsValid())) return false;
			TestTrue(FString::Printf(TEXT("and rests on the node's floor, inside the level (at %s)"),
				*Seal->GetActorLocation().ToString()),
				ZoneAt(World, Seal->GetActorLocation()) > 0 && !Seal->IsFalling()
				&& FVector::Dist2D(Seal->GetActorLocation(), Candidate.Spot) < SealExtent.X);
			return !HasAnyErrors();
		}
	}
	AddError(TEXT("no map has a path node whose seal spot needs fitting"));
	return false;
}

// Pits (PitTrap.uc:61-82, PitOpening.uc:19-40): a pawn that falls into a pit is
// moved to the pit's shaft, and one that climbs out is moved back to the trap,
// each keeping its horizontal offset from the side it left, with SetLocation,
// which fits the pawn's cylinder to the level. The trap lies on its floor, so a
// pawn climbing out lands standing on that floor; placed unfitted at the trap's
// height, its body stood half in the floor and fell out of the level.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutPitTransferSpec,
	"WoT.Falling.Placement.PitTransfersFitTheLevel", WoTFellOutRealMap::Flags)

bool FWoTFellOutPitTransferSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	FBuiltMap Built;
	if (!LoadAndBuild(*this, TEXT("Battle2_02"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	AWoTPit* Pit = World.SpawnActor<AWoTPit>(FVector::ZeroVector, FRotator::ZeroRotator,
		AlwaysSpawn());
	AWoTPitTrap* Trap = Pit ? Pit->ThePitTrap.Get() : nullptr;
	AWoTPitOpening* Opening = Trap ? Trap->ThePitOpening.Get() : nullptr;
	if (!TestNotNull(TEXT("the pit's trap"), Trap)
		|| !TestNotNull(TEXT("a shaft opening the map places"), Opening)) return false;
	Pit->Team = 1;
	// Placed where the citadel editor's cursor trace stops, 0.5 units over a floor
	// the players stand on.
	TArray<FVector> Starts;
	for (TActorIterator<APlayerStart> It(&World); It; ++It) Starts.Add(It->GetActorLocation());
	Starts.Sort([](const FVector& A, const FVector& B)
	{
		return A.X != B.X ? A.X < B.X : A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z;
	});
	FVector TrapSpot = FVector::ZeroVector;
	bool bPlaced = false;
	for (const FVector& Spot : Starts)
	{
		FHitResult Floor;
		if (!FloorUnder(World, Spot, 200.0, Floor) || Floor.ImpactNormal.Z < 0.99) continue;
		FVector Location = Floor.ImpactPoint + FVector(0.0, 0.0, 0.5);
		FVector Normal = Floor.ImpactNormal;
		if (!Pit->CalcLocation(Location, Normal)) continue;
		TrapSpot = Location;
		bPlaced = true;
		break;
	}
	if (!TestTrue(TEXT("the pit has a floor to lie on"), bPlaced)) return false;
	Pit->SetActorLocation(TrapSpot);
	Trap->SetActorLocation(TrapSpot);
	const double FloorZ = TrapSpot.Z - 0.5;

	// Falling in: a player standing on the trap is moved to the shaft.
	AWoTPlayerController* Controller = nullptr;
	const FVector Offset(20.0, -10.0, 0.0);
	AWoTCharacter* Victim = StandPlayer<AWoTCharacter>(*this, World,
		TrapSpot + Offset + FVector(0.0, 0.0, 60.0), Controller);
	if (!Victim) return false;
	Trap->ShowTrap();
	Trap->NotifyActorBeginOverlap(Victim);
	Note(*this, FString::Printf(TEXT("trap at %s, shaft opening at %s, the victim lands at %s"),
		*TrapSpot.ToString(), *Opening->GetActorLocation().ToString(),
		*Victim->GetActorLocation().ToString()));
	TestTrue(TEXT("falling in lands in the shaft, offset kept"),
		FVector::Dist2D(Victim->GetActorLocation(), Opening->GetActorLocation() + Offset) < 20.0);
	TestTrue(TEXT("fitted to the level there"), FitsLevel(World, *Victim));
	int32 OutsideTicks = 0;
	for (int32 Tick = 0; Tick < 90 && IsValid(Victim); ++Tick)
	{
		WoTTest::TickWorld(&World, 1, Step);
		if (IsValid(Victim)) OutsideTicks += ZoneAt(World, Victim->GetActorLocation()) == 0 ? 1 : 0;
	}
	TestEqual(TEXT("down the shaft, it never leaves the level"), OutsideTicks, 0);

	// Climbing out: a player reaching the shaft's opening is moved back to the trap.
	const FVector ClimbOffset(30.0, 15.0, 0.0);
	AWoTCharacter* Climber = PlacePlayer(World, Opening->GetActorLocation() + ClimbOffset);
	if (!TestNotNull(TEXT("climber"), Climber)) return false;
	Opening->NotifyActorBeginOverlap(Climber);
	const FVector Arrived = Climber->GetActorLocation();
	Note(*this, FString::Printf(TEXT("the climber arrives at %s (floor at Z %.2f)"),
		*Arrived.ToString(), FloorZ));
	TestTrue(TEXT("climbing out lands at the trap, offset kept"),
		FVector::Dist2D(Arrived, TrapSpot + ClimbOffset) < 2.0);
	TestTrue(FString::Printf(TEXT("standing on the trap's floor, not in it (Z %.2f)"), Arrived.Z),
		Arrived.Z - Climber->GetSimpleCollisionHalfHeight() > FloorZ - 0.01
		&& Arrived.Z - Climber->GetSimpleCollisionHalfHeight() < FloorZ + 3.0);
	TestTrue(TEXT("fitted to the level there"), FitsLevel(World, *Climber));
	WoTTest::TickWorld(&World, 1.0f, Step);
	TestTrue(FString::Printf(TEXT("it stays on the floor, alive and inside the level (at %s)"),
		*Climber->GetActorLocation().ToString()),
		Climber->IsAlive() && ZoneAt(World, Climber->GetActorLocation()) > 0
		&& !Climber->GetCharacterMovement()->IsFalling()
		&& FMath::Abs(Climber->GetActorLocation().Z - Arrived.Z) < 3.0);
	return !HasAnyErrors();
}

// Dropped bags (WOTPlayer.uc:1302 and :3525): Inventory.DropFrom places the bag
// ahead of its dropper with SetLocation, which fits it to the level. A body that
// dies facing a wall, or a player who drops an artifact there, drops the bag at
// a spot behind the wall; the original refuses that spot, and the bag stays a
// pickup where it was made, at the body. UE5's TeleportTo put it behind the wall,
// outside the level, where it was removed with its contents as it fell.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutDropFacingWallSpec,
	"WoT.Falling.Placement.DropFacingAWallKeepsTheBag", WoTFellOutRealMap::Flags)

bool FWoTFellOutDropFacingWallSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	FBuiltMap Built;
	if (!LoadAndBuild(*this, TEXT("Arena_01"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	const AActor* Start = Built.Projected(LowFloorStartRecord);
	if (!TestNotNull(TEXT("Arena_01's low-floor player start"), Start)) return false;
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Body = StandPlayer<AWoTCharacter>(*this, World, Start->GetActorLocation(),
		Controller);
	if (!Body) return false;
	const double Radius = Body->GetCapsuleComponent()->GetScaledCapsuleRadius();
	constexpr double BagRadius = 30.0;
	// A wall near the body, with floor up to it, and the spot an arm's length from
	// it where a body faces it; the bag's drop spot then lies behind the wall.
	FVector Standing = FVector::ZeroVector;
	FVector Facing = FVector::ZeroVector;
	bool bWall = false;
	for (int32 Index = 0; Index < 16 && !bWall; ++Index)
	{
		const FVector Direction = FRotator(0.0, 22.5 * Index, 0.0).Vector();
		FHitResult Wall;
		if (!WoTVisBlockingTrace(&World, Body->GetActorLocation(),
			Body->GetActorLocation() + Direction * 800.0, Body, Wall)
			|| FMath::Abs(Wall.ImpactNormal.Z) > 0.1) continue;
		const FVector Normal = Wall.ImpactNormal.GetSafeNormal2D();
		FVector Spot = Wall.ImpactPoint + Normal * (Radius + 3.0);
		Spot.Z = Body->GetActorLocation().Z;
		FHitResult Under;
		if (!FloorUnder(World, Spot, Body->GetSimpleCollisionHalfHeight() + 8.0, Under)
			|| ZoneAt(World, Spot - Normal * (BagRadius + Radius)) != 0) continue;
		Standing = Spot;
		Facing = -Normal;
		bWall = true;
	}
	if (!TestTrue(TEXT("a wall stands near the body"), bWall)) return false;
	Relocate(*Body, Standing);
	Controller->SetControlRotation(Facing.Rotation());
	Body->SetActorRotation(Facing.Rotation());
	WoTTest::TickWorld(&World, 0.3f, Step);
	TestTrue(FString::Printf(TEXT("the body faces the wall from %s"), *Body->GetActorLocation().ToString()),
		FVector::DotProduct(Body->GetActorForwardVector(), Facing) > 0.99
		&& FVector::Dist2D(Body->GetActorLocation(), Standing) < 2.0);

	// A player drops an artifact there: it goes in a bag dropped ahead of them.
	AWoTAngrealInvFireball* Tossed = World.SpawnActor<AWoTAngrealInvFireball>(
		Standing + FVector(0.0, 0.0, 3000.0), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("an artifact to drop"), Tossed)) return false;
	Tossed->GiveTo(Body);
	Body->InventoryComponent->SelectItem(Tossed);
	Body->Drop();
	WoTTest::TickWorld(&World, 1.0f, Step);
	AWoTInventory* const TossedItem = Tossed;
	bool bKept = IsValid(Tossed) && Body->InventoryComponent->Items.Contains(TossedItem);
	for (TActorIterator<AWoTBagHolding> It(&World); It; ++It)
	{
		bKept |= IsValid(Tossed) && It->GetItems().Contains(TossedItem)
			&& It->GetItemState() == EWoTInventoryState::Pickup
			&& ZoneAt(World, It->GetActorLocation()) > 0;
	}
	TestTrue(TEXT("the dropped artifact is never lost: the player keeps it, or a bag holds it inside the level"),
		bKept);

	// The body dies there: its bag is made at the body and dropped ahead of it.
	AWoTAngrealInvFireball* Carried = World.SpawnActor<AWoTAngrealInvFireball>(
		Standing + FVector(0.0, 0.0, 3200.0), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("an artifact the body carries"), Carried)) return false;
	Body->Health = 0;
	const FTransform BagTransform(FRotator::ZeroRotator, Body->GetActorLocation());
	AWoTBagHolding* Bag = World.SpawnActorDeferred<AWoTBagHolding>(
		AWoTBagHolding::StaticClass(), BagTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("death bag"), Bag)) return false;
	Bag->bHeldItem = true;
	Bag->FinishSpawning(BagTransform);
	TestTrue(TEXT("the bag takes the artifact"), Bag->AddItem(Carried));
	const FVector DropSpot = Body->GetActorLocation()
		+ Body->GetActorForwardVector() * (Bag->GetPickupCollisionRadius() + Radius);
	TestEqual(TEXT("its drop spot lies behind the wall, outside the level"),
		ZoneAt(World, DropSpot), 0);
	const TWeakObjectPtr<AWoTBagHolding> WeakBag(Bag);
	Bag->DropFrom(DropSpot);
	WoTTest::TickWorld(&World, 2.0f, Step);
	if (!TestTrue(TEXT("the bag is kept"), WeakBag.IsValid() && IsValid(Carried))) return false;
	Note(*this, FString::Printf(TEXT("the body at %s drops its bag toward %s; it lies at %s"),
		*Standing.ToString(), *DropSpot.ToString(), *Bag->GetActorLocation().ToString()));
	TestEqual(TEXT("as a pickup"), static_cast<int32>(Bag->GetItemState()),
		static_cast<int32>(EWoTInventoryState::Pickup));
	TestTrue(TEXT("holding the artifact"),
		Bag->GetItems().Contains(static_cast<AWoTInventory*>(Carried)));
	TestTrue(TEXT("inside the level, at rest"),
		ZoneAt(World, Bag->GetActorLocation()) > 0 && !Bag->IsFalling());
	return !HasAnyErrors();
}

// The teleporting creature's blink (its script, uc:491-512): it vanishes with its
// actor collision off but its world collision on, so it keeps standing on its
// floor while hidden, and reappears with SetLocation, which fits it to the level
// at its destination node. It used to reappear at the node unfitted, half in the
// floor under a node that stands low over it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutCreatureTeleportSpec,
	"WoT.Falling.Placement.TeleportingCreatureKeepsItsFloor", WoTFellOutRealMap::Flags)

bool FWoTFellOutCreatureTeleportSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	FBuiltMap Built;
	if (!LoadAndBuild(*this, TEXT("Arena_01"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	const AActor* Start = Built.Projected(LowFloorStartRecord);
	if (!TestNotNull(TEXT("Arena_01's low-floor player start"), Start)) return false;
	// The creature kind that blinks.
	AWoTNPCCharacter* Creature = nullptr;
	UWoTMyrddraalTeleportBehavior* Teleport = nullptr;
	TArray<FName> Rows;
	WoTCreatureData::GetAllRowNames(Rows);
	Rows.Sort(FNameLexicalLess());
	for (FName Name : Rows)
	{
		const FWoTCreatureRow* Row = WoTCreatureData::Find(Name);
		if (!Row || Row->BehaviorClass.IsNone()) continue;
		AWoTNPCCharacter* Candidate = WoTCreatureData::SpawnCreature(World, Name,
			Start->GetActorLocation(), 1);
		if (!Candidate) continue;
		Teleport = Candidate->FindComponentByClass<UWoTMyrddraalTeleportBehavior>();
		if (Teleport)
		{
			Creature = Candidate;
			break;
		}
		if (AController* Brain = Candidate->GetController()) Brain->Destroy();
		Candidate->Destroy();
	}
	if (!TestNotNull(TEXT("a creature that blinks"), Teleport)) return false;
	AWoTNPCController* Controller = Cast<AWoTNPCController>(Creature->GetController());
	if (!TestNotNull(TEXT("its controller"), Controller)) return false;
	WoTTest::TickWorld(&World, 1.0f, Step);
	TestFalse(TEXT("it stands on its floor"), Creature->GetCharacterMovement()->IsFalling());
	// Its destination: the nearest path node over 150 units away that stands lower
	// over its floor than the creature's half-height, so that the creature fits
	// the level there only once lifted out of the floor.
	const FVector Extent = CapsuleExtent(*Creature);
	const AWoTPathNode* Destination = nullptr;
	double Nearest = TNumericLimits<double>::Max();
	for (TActorIterator<AWoTPathNode> It(&World); It; ++It)
	{
		const FVector NodeSpot = It->GetActorLocation();
		const double Distance = FVector::Dist(NodeSpot, Creature->GetActorLocation());
		FVector Fitted = NodeSpot;
		if (Distance > 150.0 && Distance < Nearest && ZoneAt(World, NodeSpot) > 0
			&& !WoTUE1Collision::LevelPointCheckFits(World, NodeSpot, Extent)
			&& WoTUE1Collision::FindSpotLevelOnly(World, Extent, Fitted, false))
		{
			Nearest = Distance;
			Destination = *It;
		}
	}
	if (!TestNotNull(TEXT("a destination node low over its floor"), Destination)) return false;
	Teleport->SetPendingDestination(Destination->GetActorLocation());
	Controller->SetBrainStateForG20Test(EWoTBrainState::Teleporting);
	TestTrue(TEXT("the blink begins"), Controller->IsTeleporting());
	bool bHidden = false;
	bool bDied = false;
	int32 OutsideTicks = 0;
	FVector VanishedAt = FVector::ZeroVector;
	double HiddenDrift = 0.0;
	for (int32 Tick = 0; Tick < 30 * 15 && Controller->IsTeleporting(); ++Tick)
	{
		WoTTest::TickWorld(&World, 1, Step);
		if (!IsValid(Creature) || !Creature->IsAlive())
		{
			bDied = true;
			break;
		}
		if (Creature->IsHidden())
		{
			if (!bHidden) VanishedAt = Creature->GetActorLocation();
			bHidden = true;
			HiddenDrift = FMath::Max(HiddenDrift,
				FVector::Dist(Creature->GetActorLocation(), VanishedAt));
		}
		OutsideTicks += ZoneAt(World, Creature->GetActorLocation()) == 0 ? 1 : 0;
	}
	TestFalse(TEXT("it never dies"), bDied);
	if (bDied) return false;
	TestTrue(TEXT("it vanishes"), bHidden);
	TestTrue(FString::Printf(TEXT("while hidden it stays where it vanished (drift %.2f)"),
		HiddenDrift), HiddenDrift < 1.0);
	TestEqual(TEXT("it never leaves the level"), OutsideTicks, 0);
	TestFalse(TEXT("the blink completes"), Controller->IsTeleporting());
	Note(*this, FString::Printf(TEXT("blinked from the low floor to node %s, reappearing at %s"),
		*Destination->GetActorLocation().ToString(), *Creature->GetActorLocation().ToString()));
	TestTrue(TEXT("it reappears at its destination"),
		FVector::Dist2D(Creature->GetActorLocation(), Destination->GetActorLocation()) < 20.0);
	TestTrue(TEXT("fitted to the level there"), FitsLevel(World, *Creature));
	WoTTest::TickWorld(&World, 0.5f, Step);
	TestTrue(FString::Printf(TEXT("standing on the floor, alive and inside the level (at %s)"),
		*Creature->GetActorLocation().ToString()),
		Creature->IsAlive() && ZoneAt(World, Creature->GetActorLocation()) > 0
		&& !Creature->GetCharacterMovement()->IsFalling());

	// The reappearance itself, before any movement can push the body out of a floor
	// it was placed in: its collision still off, it lands at the node fitted.
	Creature->SetActorEnableCollision(false);
	Teleport->SetPendingDestination(Destination->GetActorLocation());
	Teleport->CommitReappear();
	Creature->SetActorEnableCollision(true);
	Note(*this, FString::Printf(TEXT("placed directly at node %s, it stands at %s"),
		*Destination->GetActorLocation().ToString(), *Creature->GetActorLocation().ToString()));
	TestTrue(TEXT("reappearing, it is placed at its node fitted to the level"),
		FitsLevel(World, *Creature)
		&& FVector::Dist2D(Creature->GetActorLocation(), Destination->GetActorLocation()) < 20.0);
	return !HasAnyErrors();
}

// ExplodingMover.uc:81-115: an exploding mover throws its debris from random spots
// around itself. Debris collides with the world, so a piece spawns only where it
// fits the level, nudged clear of a wall or floor it grazes; a spot in solid
// space spawns nothing (s == None). Spawned there anyway, a piece in the floor was
// removed as outside the level on its first falling step.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFellOutDebrisSpec,
	"WoT.Falling.Placement.DebrisSpawnsWhereItFits", WoTFellOutRealMap::Flags)

bool FWoTFellOutDebrisSpec::RunTest(const FString&)
{
	using namespace WoTFellOutRealMap;
	FBuiltMap Built;
	if (!LoadAndBuild(*this, TEXT("Arena_09"), Built)) return !HasAnyErrors();
	UWorld& World = *Built.World;
	// Arena_09's exploding cave wall (its ExplodingMover0: ExplosionSize 100,
	// ExplosionDimensions 60, no wood), its origin set 20 units over the cave's
	// floor, so that many of its debris spots lie in the floor. This fixture does
	// not build movers' geometry; the wall is the explosion's origin only.
	FHitResult CaveFloor;
	if (!TestTrue(TEXT("the cave has a floor"), FloorUnder(World,
		FVector(-6531.0, -13215.0, -7822.0), 300.0, CaveFloor))) return false;
	AWoTExplodingMover* Mover = World.SpawnActor<AWoTExplodingMover>(
		CaveFloor.ImpactPoint + FVector(0.0, 0.0, 20.0), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("Arena_09's exploding cave wall"), Mover)) return false;
	Mover->ExplosionSize = 100.0f;
	Mover->ExplosionDimensions = 60.0f;
	Mover->NumWoodChunks = 0;
	TSet<const AWoTFragment*> Before;
	for (TActorIterator<AWoTFragment> It(&World); It; ++It) Before.Add(*It);
	Mover->Explode(nullptr, FVector::ZeroVector);
	TArray<TWeakObjectPtr<AWoTFragment>> Pieces;
	int32 Unfit = 0;
	FString FirstUnfit;
	const EWoTFragmentType Families[] = {
		EWoTFragmentType::Wall, EWoTFragmentType::Wood, EWoTFragmentType::Glass};
	for (TActorIterator<AWoTFragment> It(&World); It; ++It)
	{
		if (Before.Contains(*It)) continue;
		Pieces.Add(*It);
		// The piece's own cylinder: its family's, which its capsule keeps the radius
		// of (a UE5 capsule is never shorter than it is wide).
		const float Radius = It->Capsule->GetUnscaledCapsuleRadius();
		FVector Extent(Radius, Radius, Radius);
		for (const EWoTFragmentType Family : Families)
		{
			const FWoTFragmentRecipe* Recipe = WoTFragmentData::FindRecipe(Family);
			if (Recipe && FMath::IsNearlyEqual(Recipe->CollisionRadius, Radius))
			{
				Extent.Z = Recipe->CollisionHeight;
			}
		}
		if (!WoTUE1Collision::LevelPointCheckFits(World, It->GetActorLocation(), Extent))
		{
			if (Unfit++ == 0) FirstUnfit = It->GetActorLocation().ToString();
		}
	}
	TestTrue(TEXT("the mover throws debris"), Pieces.Num() > 0);
	TestEqual(FString::Printf(TEXT("every piece spawns where it fits the level (first unfit at %s)"),
		*FirstUnfit), Unfit, 0);
	WoTTest::TickWorld(&World, 0.1f, Step);
	int32 Removed = 0;
	for (const TWeakObjectPtr<AWoTFragment>& Piece : Pieces) Removed += Piece.IsValid() ? 0 : 1;
	Note(*this, FString::Printf(TEXT("%d pieces spawned at %s, %d unfit, %d removed on their first steps"),
		Pieces.Num(), *Mover->GetActorLocation().ToString(), Unfit, Removed));
	TestEqual(TEXT("no piece is removed on its first falling steps"), Removed, 0);
	return !HasAnyErrors();
}

#endif
