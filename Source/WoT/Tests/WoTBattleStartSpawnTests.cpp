#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

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
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTPlayerStart.h"
#include "../WoTSkyBox.h"

#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

namespace
{
constexpr EAutomationTestFlags WoTBattleStartSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

class FWoTBattleStartObjectWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("battle-start retail writer overflow")));
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

class FWoTBattleStartProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};

// The authenticated multiplayer world set and class defaults, imported once
// from the purchaser's retail files.
struct FWoTBattleStartRetailFixture
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
		TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
			WoTLoadProductionRetailContract();
		if (LoadedContract.IsError())
		{
			Error = LoadedContract.GetError().ToString();
			Test.AddError(Error);
			return false;
		}
		FWoTRetailContract WorldContract;
		FWoTRetailContract DefaultsContract;
		TArray<FWoTWorldPackageBytesForTesting> WorldSources;
		TArray<FWoTRemainingSourceBytesForTesting> DefaultSources;
		TArray<FString> DefaultLogicalIds;
		for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
		{
			const bool bMultiplayerWorld =
				(Fact.Domain == EWoTFactDomain::World || Fact.Domain == EWoTFactDomain::BSP)
				&& !Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"),
					ESearchCase::IgnoreCase);
			if (bMultiplayerWorld) WorldContract.Facts.Add(Fact);
			if (bMultiplayerWorld && Fact.Domain == EWoTFactDomain::World)
			{
				FWoTWorldPackageBytesForTesting& Source = WorldSources.AddDefaulted_GetRef();
				Source.SourcePath = Fact.SourcePath;
				if (!FFileHelper::LoadFileToArray(Source.Bytes,
					*(RetailRoot / Fact.SourcePath)))
				{
					Error = Fact.SourcePath + TEXT(" failed to load");
					Test.AddError(Error);
					return false;
				}
			}
			if (Fact.Domain == EWoTFactDomain::ClassDefault)
			{
				DefaultsContract.Facts.Add(Fact);
				FWoTRemainingSourceBytesForTesting& Source =
					DefaultSources.AddDefaulted_GetRef();
				Source.SourcePath = Fact.SourcePath;
				if (!FFileHelper::LoadFileToArray(Source.Bytes,
					*(RetailRoot / Fact.SourcePath)))
				{
					Error = Fact.SourcePath + TEXT(" failed to load");
					Test.AddError(Error);
					return false;
				}
				if (Fact.Outputs.Num() == 1)
					DefaultLogicalIds.Add(Fact.Outputs[0].LogicalId);
			}
		}
		FWoTBattleStartProgress Progress;
		TWoTBootstrapResult<FWoTWorldSetResult> Imported =
			ImportRequiredWorldSetPackageBytesForTesting(
				WorldSources, WorldContract, Writer, Progress);
		if (Imported.IsError())
		{
			Error = Imported.GetError().ToString();
			Test.AddError(Error);
			return false;
		}
		Worlds = Imported.GetValue().Worlds;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportedDefaults =
			ImportRegistryLocalizationDefaultsSourcesForTesting(
				DefaultSources, DefaultsContract, Writer, Progress);
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
			if (Bytes == nullptr)
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

	bool bAttempted = false;
	bool bLoaded = false;
	FString Error;
	FWoTBattleStartObjectWriter Writer;
	TArray<FWoTWorldDescriptor> Worlds;
	TUniquePtr<FWoTRuntimeClassDefaults> Defaults;
};

FWoTBattleStartRetailFixture& WoTBattleStartFixture()
{
	static FWoTBattleStartRetailFixture Fixture;
	return Fixture;
}

// One Battle map materialized the way the production host does before play:
// the runtime BSP (with its collision-only invisible solids), every projected
// actor, then the Battle game mode's InitGame/BeginPlay.
struct FWoTBattleStartWorld
{
	UWorld* World = nullptr;
	FWoTWorldProjectionResult Projection;
	FWoTWorldProjectionSource Source;
	TArray<FWoTRuntimeWorldActor> Records;

	~FWoTBattleStartWorld()
	{
		if (World) WoTTest::DestroySpecWorld(World);
	}

	// The retail actor identity ("Battle2_02.PlayerStart10") of a projected actor.
	FString RecordIdentity(const AActor* Actor) const
	{
		const FString Name = Actor ? Actor->GetName() : FString();
		const FString Prefix = TEXT("WoTProjected_");
		if (!Name.StartsWith(Prefix)) return Name;
		const int32 Index = FCString::Atoi(*Name.Mid(Prefix.Len()));
		return Records.IsValidIndex(Index) ? Records[Index].Identity : Name;
	}
};

bool WoTBattleStartBuildWorld(FAutomationTestBase& Test,
	FWoTBattleStartRetailFixture& Fixture, const FWoTWorldDescriptor& Descriptor,
	FWoTBattleStartWorld& Out, UClass* GameModeClass = AWoTBattleGameMode::StaticClass())
{
	const TArray<uint8>* WorldBytes = Fixture.Writer.Objects.Find(Descriptor.WorldLogicalId);
	if (!Test.TestNotNull(Descriptor.MapIdentity + TEXT(" world payload"), WorldBytes))
		return false;
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedWorld =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(*WorldBytes,
			TEXT("ue1-world-records-le-v6"), TEXT("world"));
	if (!Test.TestFalse(Descriptor.MapIdentity + TEXT(" world frame decodes"),
		DecodedWorld.IsError())) return false;
	FWoTRuntimeWorldResource Resource;
	if (!Test.TestFalse(Descriptor.MapIdentity + TEXT(" world payload decodes"),
		Resource.LoadObjectForTesting(MoveTemp(DecodedWorld).GetValue()).IsError()))
		return false;

	TArray<FWoTBSPPayloadData> Parts;
	for (const FWoTBSPPartDescriptor& PartDescriptor : Descriptor.BSPParts)
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
			Test.AddError(Descriptor.MapIdentity + TEXT(" BSP decode: ")
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
	if (!Test.TestTrue(Descriptor.MapIdentity + TEXT(" collision-only solids build"),
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
	if (!Test.TestNotNull(Descriptor.MapIdentity + TEXT(" spec world"), Out.World))
		return false;
	UWorld& World = *Out.World;
	AWoTRuntimeBSPWorld* RuntimeBSP = nullptr;
	uint32 FallbackParts = 0;
	FString SkyError;
	if (!Test.TestTrue(Descriptor.MapIdentity + TEXT(" runtime BSP materializes"),
		WoTWorldBuildRuntimeBSPForTesting(World, Descriptor, Parts, RuntimeBSP,
			FallbackParts, Error, {}, nullptr, nullptr, &SkyView, &SkyError,
			EWoTRuntimeSkyStageFault::None, nullptr, SurfaceFlags, &InvisibleCollision))
		|| !Test.TestNotNull(Descriptor.MapIdentity + TEXT(" runtime BSP actor"), RuntimeBSP))
	{
		Test.AddError(Error);
		return false;
	}

	Out.Records = Resource.Actors();
	FWoTWorldProjectionSource& Source = Out.Source;
	Source.MapIdentity = Resource.MapIdentity();
	Source.Records = Out.Records;
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
	if (!Test.TestTrue(Descriptor.MapIdentity + TEXT(" actors project"),
		FWoTWorldProjector::ProjectDeferred(World, Source, *Fixture.Defaults,
			Out.Projection, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	RuntimeBSP->FinishSpawning(FTransform::Identity, true);
	for (AWoTSkyBox* SkyBox : RuntimeBSP->SkyBoxes)
		if (SkyBox) SkyBox->FinishSpawning(FTransform::Identity, true);
	for (AActor* Actor : Out.Projection.DeferredActors)
		if (Actor) Actor->FinishSpawning(Actor->GetActorTransform(), true);
	World.UpdateWorldComponents(false, false);
	World.GetWorldSettings()->DefaultGameMode = GameModeClass;
	FURL URL;
	World.SetGameMode(URL);
	World.InitializeActorsForPlay(URL);
	World.BeginPlay();
	return true;
}

// Everything a pawn capsule overlaps at Location, for the matrix's evidence.
FString WoTBattleStartOverlaps(UWorld& World, const FVector& Location,
	float Radius, float HalfHeight)
{
	TArray<FOverlapResult> Overlaps;
	FCollisionObjectQueryParams Objects(FCollisionObjectQueryParams::AllObjects);
	World.OverlapMultiByObjectType(Overlaps, Location, FQuat::Identity, Objects,
		FCollisionShape::MakeCapsule(Radius, HalfHeight));
	TArray<FString> Names;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		const UPrimitiveComponent* Component = Overlap.Component.Get();
		const AActor* Actor = Overlap.GetActor();
		if (!Component || !Actor || Actor->IsA<APawn>()) continue;
		Names.AddUnique(FString::Printf(TEXT("%s/%s[%d,pawn=%d]"), *Actor->GetName(),
			*Component->GetName(), static_cast<int32>(Component->GetCollisionObjectType()),
			static_cast<int32>(Component->GetCollisionResponseToChannel(ECC_Pawn))));
	}
	return Names.IsEmpty() ? TEXT("-") : FString::Join(Names, TEXT(" "));
}
}

// Every team pawn class at every PlayerStart of every Battle map, through the
// production start placement (UE1 FindSpot against the level, then actor
// encroachment), both as GameInfo.Login spawns it (SpawnActor) and as
// GameInfo.RestartPlayer moves it (FarMoveActor). The owner's Battle2_02 Hound
// joins were stranded at PlayerStart10/11/13/16: the floor there is flat at
// z=20998 under starts authored 13.7 uu above it, and UE5's FindTeleportSpot
// took one minimum-translation step sideways off a floor triangle's edge
// (onto the next coplanar triangle) instead of lifting the cylinder 34.6 uu
// as UE1's AdjustSpot traces do. Only Battle3_01's two starts buried under a
// semisolid floor are refused, as in retail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBattleRealRetailTeamStartsSpec,
	"WoT.Battle.RealRetailTeamStarts", WoTBattleStartSpecFlags)

bool FWoTBattleRealRetailTeamStartsSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: Battle team starts not verified"));
		return true;
	}
	FWoTBattleStartRetailFixture& Fixture = WoTBattleStartFixture();
	if (!Fixture.Load(*this)) return false;

	auto RetailRefuses = [](const FString& Identity)
	{
		return Identity == TEXT("Battle3_01.PlayerStart27")
			|| Identity == TEXT("Battle3_01.PlayerStart28");
	};
	int32 BattleMaps = 0;
	int32 Placements = 0;
	int32 Refusals = 0;
	for (const FWoTWorldDescriptor& Descriptor : Fixture.Worlds)
	{
		if (!Descriptor.MapIdentity.StartsWith(TEXT("Battle"))) continue;
		FWoTBattleStartWorld Built;
		if (!WoTBattleStartBuildWorld(*this, Fixture, Descriptor, Built)) continue;
		++BattleMaps;
		UWorld& World = *Built.World;
		AWoTBattleGameMode* Mode = World.GetAuthGameMode<AWoTBattleGameMode>();
		if (!TestNotNull(Descriptor.MapIdentity + TEXT(" Battle mode located its BattleInfo"),
			Mode ? Mode->Battle.Get() : nullptr)) continue;
		TArray<UClass*> Classes;
		for (int32 Team = 0; Team < Mode->Battle->NumTeams && Team < 4; ++Team)
			if (Mode->Battle->TeamClass[Team])
				Classes.AddUnique(Mode->Battle->TeamClass[Team].Get());
		TestTrue(Descriptor.MapIdentity + TEXT(" has team pawn classes"), !Classes.IsEmpty());
		TMap<FString, FVector> TeamZeroResolved;
		for (TActorIterator<AWoTPlayerStart> It(&World); It; ++It)
		{
			const AWoTPlayerStart* Start = *It;
			const FString Identity = Built.RecordIdentity(Start);
			const FVector At = Start->GetActorLocation();
			const FTransform Transform(FRotator(0.0f, Start->GetActorRotation().Yaw, 0.0f), At);
			for (UClass* PawnClass : Classes)
			{
				for (const bool bAssumeFit : {true, false})
				{
					FWoTStartSpawnResult Result;
					APawn* Pawn = AWoTGameMode::SpawnPawnAtRetailStart(World, PawnClass,
						Transform, nullptr, bAssumeFit, &Result);
					const AWoTCharacter* CDO = Cast<AWoTCharacter>(PawnClass->GetDefaultObject());
					const UCapsuleComponent* Capsule = CDO ? CDO->GetCapsuleComponent() : nullptr;
					const float Radius = Capsule ? Capsule->GetUnscaledCapsuleRadius() : 17.0f;
					const float HalfHeight =
						Capsule ? Capsule->GetUnscaledCapsuleHalfHeight() : 46.0f;
					UE_LOG(LogTemp, Display,
						TEXT("BATTLE_START map=%s start=%s team=%d at=(%.1f,%.1f,%.1f) class=%s path=%s result=%s resolved=(%.1f,%.1f,%.1f) blocker=%s overlaps=%s"),
						*Descriptor.MapIdentity, *Identity, static_cast<int32>(Start->TeamNumber),
						At.X, At.Y, At.Z, *PawnClass->GetName(),
						bAssumeFit ? TEXT("login") : TEXT("restart"),
						WoTStartSpawnRejectionName(Result.Rejection),
						Result.ResolvedLocation.X, Result.ResolvedLocation.Y,
						Result.ResolvedLocation.Z,
						Result.Blocker.IsEmpty() ? TEXT("-") : *Result.Blocker,
						*WoTBattleStartOverlaps(World, At, Radius, HalfHeight));
					const FString Label = FString::Printf(TEXT("%s %s %s"), *Identity,
						*PawnClass->GetName(), bAssumeFit ? TEXT("login") : TEXT("restart"));
					++Placements;
					if (RetailRefuses(Identity))
					{
						++Refusals;
						TestEqual(Label + TEXT(" is refused by FindSpot like retail"),
							FString(WoTStartSpawnRejectionName(Result.Rejection)),
							FString(WoTStartSpawnRejectionName(EWoTStartSpawnRejection::NoFreeSpot)));
					}
					else
					{
						TestEqual(Label + TEXT(" seats the pawn"),
							FString(WoTStartSpawnRejectionName(Result.Rejection)),
							FString(WoTStartSpawnRejectionName(EWoTStartSpawnRejection::None)));
					}
					if (Descriptor.MapIdentity == TEXT("Battle2_02") && Start->TeamNumber == 0)
						TeamZeroResolved.Add(Label, Result.ResolvedLocation);
					if (Pawn) Pawn->Destroy();
				}
			}
		}
		if (Descriptor.MapIdentity != TEXT("Battle2_02")) continue;

		// Retail's FindSpot at the team-0 citadel: the downward AdjustSpot probe
		// meets the flat floor 13.6875 uu below each start (Time 0.29755) and
		// lifts the box by (1.05 - Time) * 46 = 34.6125 uu.
		TestEqual(TEXT("Battle2_02 has eight team-0 starts for each probe"),
			TeamZeroResolved.Num(), 8 * Classes.Num() * 2);
		for (const TPair<FString, FVector>& Resolved : TeamZeroResolved)
		{
			TestEqual(Resolved.Key + TEXT(" lands at retail's FindSpot lift"),
				Resolved.Value.Z, 21011.6875 + 34.6125, 0.01);
		}

		// The production join: a neutral host presses "1" sixteen times (leaving
		// the team in between). Every join must possess a Hound at a team-0 start.
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTPlayerController* Controller = World.SpawnActor<AWoTPlayerController>(Spawn);
		AWoTHUD* HUD = World.SpawnActor<AWoTHUD>(Spawn);
		if (!TestNotNull(TEXT("Battle2_02 host controller"), Controller)
			|| !TestNotNull(TEXT("Battle2_02 host HUD"), HUD)) continue;
		Controller->Player = NewObject<ULocalPlayer>(GEngine);
		Controller->InitPlayerState();
		Mode->PostLogin(Controller);
		HUD->PlayerOwner = Controller;
		TArray<FVector> TeamZeroStarts;
		for (TActorIterator<AWoTPlayerStart> It(&World); It; ++It)
			if (It->TeamNumber == 0) TeamZeroStarts.Add(It->GetActorLocation());
		UClass* HoundClass = Mode->Battle->TeamClass[0].Get();
		int32 Joins = 0;
		for (int32 Attempt = 0; Attempt < 16; ++Attempt)
		{
			const AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>();
			if (!State || State->Team != 255) Mode->ChangeTeam(Controller, 255);
			if (!TestTrue(TEXT("Battle2_02 digit 1 is the team-select join"),
				HUD->HandleTeamSelectDigit(0))) break;
			const APawn* Joined = Controller->GetPawn();
			const bool bAtTeamStart = Joined && TeamZeroStarts.ContainsByPredicate(
				[Joined](const FVector& StartAt)
				{
					return FVector::Dist2D(StartAt, Joined->GetActorLocation()) < 1.0;
				});
			TestTrue(FString::Printf(TEXT("Battle2_02 join %d possesses a Hound at a team-0 start"),
				Attempt), Joined && Joined->IsA(HoundClass) && bAtTeamStart
				&& !Joined->IsHidden());
			Joins += Joined ? 1 : 0;
		}
		TestEqual(TEXT("every Battle2_02 Hound join seats the player"), Joins, 16);
	}
	TestEqual(TEXT("all eight Battle maps were checked"), BattleMaps, 8);
	TestEqual(TEXT("only Battle3_01's buried starts refuse (2 starts x 3 classes x 2 paths)"),
		Refusals, 12);
	TestTrue(TEXT("every team class was placed at every start"), Placements > Refusals);
	return !HasAnyErrors();
}

// The same placement at every PlayerStart of every Arena map, for each of the
// four faction pawns a joining player may pick: every start seats the pawn.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTArenaRealRetailStartsSpec,
	"WoT.GameMode.RealRetailArenaStarts", WoTBattleStartSpecFlags)

bool FWoTArenaRealRetailStartsSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: Arena starts not verified"));
		return true;
	}
	FWoTBattleStartRetailFixture& Fixture = WoTBattleStartFixture();
	if (!Fixture.Load(*this)) return false;
	UClass* const Classes[] = {AWoTAesSedai::StaticClass(), AWoTForsaken::StaticClass(),
		AWoTHound::StaticClass(), AWoTWhitecloak::StaticClass()};
	int32 ArenaMaps = 0;
	int32 Starts = 0;
	for (const FWoTWorldDescriptor& Descriptor : Fixture.Worlds)
	{
		if (!Descriptor.MapIdentity.StartsWith(TEXT("Arena"))) continue;
		FWoTBattleStartWorld Built;
		if (!WoTBattleStartBuildWorld(*this, Fixture, Descriptor, Built,
			AWoTGameMode::StaticClass())) continue;
		++ArenaMaps;
		UWorld& World = *Built.World;
		for (TActorIterator<APlayerStart> It(&World); It; ++It)
		{
			const APlayerStart* Start = *It;
			const FString Identity = Built.RecordIdentity(Start);
			const FTransform Transform(
				FRotator(0.0f, Start->GetActorRotation().Yaw, 0.0f), Start->GetActorLocation());
			++Starts;
			for (UClass* PawnClass : Classes)
			{
				for (const bool bAssumeFit : {true, false})
				{
					FWoTStartSpawnResult Result;
					APawn* Pawn = AWoTGameMode::SpawnPawnAtRetailStart(World, PawnClass,
						Transform, nullptr, bAssumeFit, &Result);
					TestEqual(FString::Printf(TEXT("%s %s %s seats the pawn"), *Identity,
						*PawnClass->GetName(), bAssumeFit ? TEXT("login") : TEXT("restart")),
						FString(WoTStartSpawnRejectionName(Result.Rejection)),
						FString(WoTStartSpawnRejectionName(EWoTStartSpawnRejection::None)));
					if (Pawn) Pawn->Destroy();
				}
			}
		}
	}
	TestEqual(TEXT("all eleven Arena maps were checked"), ArenaMaps, 11);
	TestTrue(TEXT("Arena starts were placed"), Starts > 0);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
