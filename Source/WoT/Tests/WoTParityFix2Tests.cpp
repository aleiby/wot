#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTZoneLeeches.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTWorldProjector.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTCreatureChunk.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTRandom.h"
#include "../WoTSoundTable.h"
#include "../WoTSpecialTriggers.h"
#include "../WoTTeleporter.h"
#include "../WoTZoneVolume.h"

#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif
#include "WoTRetailReader/BSPWorld.h"

namespace
{
constexpr EAutomationTestFlags Fix2Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters Fix2Spawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

AWoTZoneVolume* Fix2Zone(UWorld& World, const FVector& Center,
	const FVector& Extent)
{
	AWoTZoneVolume* Zone = World.SpawnActor<AWoTZoneVolume>(
		Center, FRotator::ZeroRotator, Fix2Spawn());
	if (Zone) Zone->Collision->SetBoxExtent(Extent);
	return Zone;
}

AWoTCharacter* Fix2Player(UWorld& World, const FVector& Location,
	AWoTPlayerController*& OutController)
{
	OutController = World.SpawnActor<AWoTPlayerController>(Fix2Spawn());
	AWoTCharacter* Player = World.SpawnActor<AWoTCharacter>(
		Location, FRotator::ZeroRotator, Fix2Spawn());
	if (OutController && Player)
	{
		OutController->SetAsLocalPlayerController();
		OutController->Possess(Player);
	}
	return Player;
}

template <typename T>
FWoTRuntimeWorldProperty Fix2Value(const TCHAR* Name,
	EWoTRuntimeWorldPropertyType Type, const T& Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = Type;
	Property.Value.SetNumUninitialized(sizeof(T));
	FMemory::Memcpy(Property.Value.GetData(), &Value, sizeof(T));
	return Property;
}

FWoTRuntimeWorldProperty Fix2Name(const TCHAR* Name, const TCHAR* Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Name;
	Property.NameValue = Value;
	return Property;
}

FWoTRuntimeWorldProperty Fix2Object(const TCHAR* Name, uint32 ArrayIndex,
	const TCHAR* Identity)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Object;
	Property.ArrayIndex = ArrayIndex;
	Property.ObjectIdentity = Identity;
	Property.ObjectClassIdentity = TEXT("Core.Class");
	return Property;
}

struct FFix2WorldFixture
{
	FWoTWorldProjectionSource Source;
	TArray<FWoTRuntimeWorldActor> Records;
	TArray<FVector3f> Points;
	TArray<FWoTRuntimeWorldVertex> Vertices;
	TArray<FWoTRuntimeWorldNode> Nodes;
	TArray<FWoTRuntimeWorldZone> Zones;

	void Bind()
	{
		Source.Records = Records;
		Source.Points = Points;
		Source.Vertices = Vertices;
		Source.Nodes = Nodes;
		Source.Zones = Zones;
	}
};

bool BuildBattle202ZoneFixture(FWoTRuntimeClassDefaults& Defaults,
	FFix2WorldFixture& Fixture, FString& Error)
{
	TArray<FWoTRuntimeClassDefaultRecord> Classes;
	auto AddClass = [&Classes](const TCHAR* Identity, const TCHAR* Super)
	{
		FWoTRuntimeClassDefaultRecord& Record = Classes.AddDefaulted_GetRef();
		Record.Identity = Identity;
		Record.SuperclassIdentity = Super;
	};
	AddClass(TEXT("Engine.Actor"), TEXT(""));
	AddClass(TEXT("Engine.ZoneInfo"), TEXT("Engine.Actor"));
	AddClass(TEXT("WOT.WOTZoneInfo"), TEXT("Engine.ZoneInfo"));
	AddClass(TEXT("Engine.Light"), TEXT("Engine.Actor"));
	AddClass(TEXT("Engine.TriggerLight"), TEXT("Engine.Light"));
	if (!Defaults.LoadFixture(MoveTemp(Classes), Error)) return false;

	FWoTRuntimeWorldActor Zone;
	Zone.Identity = TEXT("Battle2_02.WOTZoneInfo13");
	Zone.ClassIdentity = TEXT("WOT.WOTZoneInfo");
	Zone.Properties.Add(Fix2Value(TEXT("bHealZone"),
		EWoTRuntimeWorldPropertyType::Boolean, uint8(1)));
	Zone.Properties.Add(Fix2Value(TEXT("HealPerSec"),
		EWoTRuntimeWorldPropertyType::Integer, int32(10)));
	Zone.Properties.Add(Fix2Value(TEXT("bFOVZone"),
		EWoTRuntimeWorldPropertyType::Boolean, uint8(1)));
	Zone.Properties.Add(Fix2Value(TEXT("ZoneFOV"),
		EWoTRuntimeWorldPropertyType::Float, 120.0f));
	Zone.Properties.Add(Fix2Value(TEXT("bLeechZone"),
		EWoTRuntimeWorldPropertyType::Boolean, uint8(1)));
	Zone.Properties.Add(Fix2Object(TEXT("LeechClasses"), 0,
		TEXT("WOT.SmoothRandFOVLeech")));
	Zone.Properties.Add(Fix2Name(TEXT("ZonePlayerEvent"),
		TEXT("healing_effects")));

	FWoTRuntimeWorldActor Light;
	Light.Identity = TEXT("Battle2_02.TriggerLight16");
	Light.ClassIdentity = TEXT("Engine.TriggerLight");
	Light.Properties.Add(Fix2Name(TEXT("Tag"), TEXT("healing_effects")));
	Light.Properties.Add(Fix2Name(TEXT("InitialState"), TEXT("TriggerControl")));
	Light.Properties.Add(Fix2Value(TEXT("ChangeTime"),
		EWoTRuntimeWorldPropertyType::Float, 3.0f));
	Light.Properties.Add(Fix2Value(TEXT("LightBrightness"),
		EWoTRuntimeWorldPropertyType::Byte, uint8(255)));

	Fixture.Source = FWoTWorldProjectionSource();
	Fixture.Source.MapIdentity = TEXT("Battle2_02");
	Fixture.Records = {MoveTemp(Zone), MoveTemp(Light)};
	Fixture.Points = {
		FVector3f(-200, -200, -200), FVector3f(200, -200, -200),
		FVector3f(200, 200, 200), FVector3f(-200, 200, 200)};
	for (int32 Index = 0; Index < Fixture.Points.Num(); ++Index)
	{
		FWoTRuntimeWorldVertex& Vertex = Fixture.Vertices.AddDefaulted_GetRef();
		Vertex.Point = Index;
	}
	FWoTRuntimeWorldNode& Node = Fixture.Nodes.AddDefaulted_GetRef();
	Node.VertexPool = 0;
	Node.VertexCount = Fixture.Vertices.Num();
	Node.Zones[0] = 1;
	Node.Zones[1] = 0;
	Node.Plane[2] = 1.0f;
	Fixture.Zones.SetNum(2);
	Fixture.Zones[1].ActorIdentity = TEXT("Battle2_02.WOTZoneInfo13");
	Fixture.Bind();
	return true;
}

int32 CountSmoothFOVLeeches(const AWoTCharacter* Character,
	UWoTSmoothRandFOVLeech** OutFirst = nullptr)
{
	if (OutFirst) *OutFirst = nullptr;
	if (!Character) return 0;
	TArray<UWoTSmoothRandFOVLeech*> Leeches;
	Character->GetComponents<UWoTSmoothRandFOVLeech>(Leeches);
	int32 Count = 0;
	for (UWoTSmoothRandFOVLeech* Leech : Leeches)
	{
		if (!IsValid(Leech)) continue;
		if (OutFirst && !*OutFirst) *OutFirst = Leech;
		++Count;
	}
	return Count;
}

struct FFix4Battle202Projected
{
	UWorld* World = nullptr;
	AWoTZoneVolume* Zone = nullptr;
	AWoTTriggerLight* Light = nullptr;
};

bool Fix4CreateBattle202Projected(FFix4Battle202Projected& Out, FString& Error)
{
	FWoTRuntimeClassDefaults Defaults;
	FFix2WorldFixture Fixture;
	if (!BuildBattle202ZoneFixture(Defaults, Fixture, Error)) return false;
	Out.World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	if (!Out.World)
	{
		Error = TEXT("could not create the Battle2_02 lifecycle world");
		return false;
	}
	FWoTWorldProjectionResult Projection;
	if (!FWoTWorldProjector::ProjectDeferred(
		*Out.World, Fixture.Source, Defaults, Projection, Error))
	{
		WoTTest::DestroySpecWorld(Out.World);
		Out.World = nullptr;
		return false;
	}
	for (AActor* Actor : Projection.DeferredActors)
	{
		if (!Out.Zone) Out.Zone = Cast<AWoTZoneVolume>(Actor);
		if (!Out.Light) Out.Light = Cast<AWoTTriggerLight>(Actor);
		if (Actor) Actor->FinishSpawning(Actor->GetActorTransform(), true);
	}
	Out.World->UpdateWorldComponents(false, false);
	if (!Out.Zone || !Out.Light)
	{
		Error = TEXT("Battle2_02 lifecycle projection omitted WOTZoneInfo13 or TriggerLight16");
		WoTTest::DestroySpecWorld(Out.World);
		Out = FFix4Battle202Projected();
		return false;
	}
	Out.Zone->ProjectedZoneIndex = INDEX_NONE;
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix2SmoothRandFOVOwnerClientSpec,
	"WoT.Parity.Fix2.SmoothRandFOVOwnerClient", Fix2Flags)
bool FFix2SmoothRandFOVOwnerClientSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	bool bOwnsPayloadActivation = false;
	if (!IsWoTPayloadBootstrapActive())
	{
		bOwnsPayloadActivation =
			!PublishWoTPayloadBootstrapActivationForTesting().IsError()
			&& !PublishWoTBootstrapWorldReadyForTesting().IsError();
	}
	ON_SCOPE_EXIT
	{
		if (bOwnsPayloadActivation) ResetWoTPayloadActivationForTesting();
	};
	if (!TestTrue(TEXT("network worlds have authorized payload gameplay"),
		IsWoTBootstrapWorldReady())) return false;

	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("Fix2 SmoothRandFOV owner-client replication"));
	if (!TestTrue(TEXT("real dedicated server loads"), Worlds.Server.IsLoaded()))
		return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	ServerWorld->SpawnActor<AWoTPlayerStart>(
		FVector(0, 0, 5000), FRotator::ZeroRotator, Fix2Spawn());
	if (!TestTrue(TEXT("real owner client connects"),
		Worlds.CreateAndConnectClient())) return false;
	AWoTCharacter* ServerPawn = Cast<AWoTCharacter>(
		Worlds.GetServerPlayerControllerOfClient(0)->GetPawn());
	UWorld* ClientWorld = Worlds.Clients[0].GetWorld();
	AWoTPlayerController* ClientPC = Cast<AWoTPlayerController>(
		ClientWorld->GetFirstPlayerController());
	AWoTCharacter* ClientPawn = ClientPC
		? Cast<AWoTCharacter>(ClientPC->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("authority pawn"), ServerPawn)
		|| !TestNotNull(TEXT("autonomous pawn"), ClientPawn)) return false;

	auto AddBattle202Zone = [](UWorld& World, const FVector& At)
	{
		// Keep the falling network pawns enclosed for the whole replication window.
		AWoTZoneVolume* Zone = Fix2Zone(World, At, FVector(100000));
		Zone->bFOVZone = true;
		Zone->ZoneFOV = 120.0f;
		Zone->bLeechZone = true;
		Zone->LeechClasses.Add(UWoTSmoothRandFOVLeech::StaticClass());
		return Zone;
	};
	AWoTZoneVolume* ServerZone =
		AddBattle202Zone(*ServerWorld, ServerPawn->GetActorLocation());
	AWoTZoneVolume* ClientZone =
		AddBattle202Zone(*ClientWorld, ClientPawn->GetActorLocation());
	Worlds.TickAll(120);
	TestTrue(TEXT("client pawn is locally controlled"), ClientPawn->IsLocallyControlled());
	TestFalse(TEXT("client pawn is a player, not an NPC"), ClientPawn->IsNPCPawn());
	TestEqual(TEXT("server tracker enters the authored FOV zone"),
		ServerWorld->GetSubsystem<UWoTZoneIndex>()->GetRegion(ServerPawn), ServerZone);
	TestEqual(TEXT("owner tracker enters the authored FOV zone"),
		ClientWorld->GetSubsystem<UWoTZoneIndex>()->GetRegion(ClientPawn), ClientZone);

	UWoTSmoothRandFOVLeech* OwnerLeech = nullptr;
	TestEqual(TEXT("dedicated authority owns no camera-only FOV leech"),
		CountSmoothFOVLeeches(ServerPawn), 0);
	TestEqual(TEXT("owning client has exactly one active FOV leech"),
		CountSmoothFOVLeeches(ClientPawn, &OwnerLeech), 1);
	if (TestNotNull(TEXT("owning client FOV leech"), OwnerLeech))
	{
		TestFalse(TEXT("owner-local FOV leech is not replicated"),
			OwnerLeech->GetIsReplicated());
	}
	const float FOV = ClientPC->GetCameraFOVForTest();
	TestTrue(FString::Printf(TEXT("Battle2_02 120 +/- 3 range is honored (%.3f)"), FOV),
		FOV >= 117.0f && FOV <= 123.0f);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix2Battle202ZonePlayerEventSpec,
	"WoT.Parity.Fix2.Battle202ZonePlayerEvent", Fix2Flags)
bool FFix2Battle202ZonePlayerEventSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaults Defaults;
	FFix2WorldFixture Fixture;
	FString Error;
	if (!TestTrue(TEXT("exact Battle2_02 actor fixture builds"),
		BuildBattle202ZoneFixture(Defaults, Fixture, Error)))
	{
		AddError(Error);
		return false;
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	FWoTWorldProjectionResult Projection;
	if (!TestTrue(TEXT("production world projector accepts Battle2_02 linkage"),
		FWoTWorldProjector::ProjectDeferred(
			*World, Fixture.Source, Defaults, Projection, Error)))
	{
		AddError(Error);
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTZoneVolume* Zone = nullptr;
	AWoTTriggerLight* Light = nullptr;
	for (AActor* Actor : Projection.DeferredActors)
	{
		if (!Zone) Zone = Cast<AWoTZoneVolume>(Actor);
		if (!Light) Light = Cast<AWoTTriggerLight>(Actor);
		if (Actor) Actor->FinishSpawning(Actor->GetActorTransform(), true);
	}
	World->UpdateWorldComponents(false, false);
	if (!TestNotNull(TEXT("Battle2_02.WOTZoneInfo13 projects"), Zone)
		|| !TestNotNull(TEXT("Battle2_02.TriggerLight16 projects"), Light))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("projected ZonePlayerEvent retains healing_effects"),
		Zone->ZonePlayerEvent, FName(TEXT("healing_effects")));
	TestEqual(TEXT("projected TriggerLight16 registers matching tag"),
		Light->TagName, FName(TEXT("healing_effects")));
	// The source is an exact authored-property/identity fixture; bypass only the
	// synthetic one-plane BSP classifier so movement exercises the real zone index.
	Zone->ProjectedZoneIndex = INDEX_NONE;

	AWoTPlayerController* FirstPC = nullptr;
	AWoTPlayerController* SecondPC = nullptr;
	AWoTCharacter* First = Fix2Player(*World, FVector(1000, 0, 0), FirstPC);
	AWoTCharacter* Second = Fix2Player(*World, FVector(1200, 0, 0), SecondPC);
	WoTTest::TickWorld(World, 1);
	First->SetActorLocation(FVector::ZeroVector);
	WoTTest::TickWorld(World, 60);
	TestTrue(TEXT("first player entry fires healing_effects into TriggerLight16"),
		Light->Light->Intensity > 0.0f);
	Second->SetActorLocation(FVector(20, 0, 0));
	WoTTest::TickWorld(World, 10);
	First->SetActorLocation(FVector(1000, 0, 0));
	WoTTest::TickWorld(World, 10);
	TestTrue(TEXT("first departure leaves light on while a player remains"),
		Light->Light->Intensity > 0.0f);
	Second->SetActorLocation(FVector(1200, 0, 0));
	WoTTest::TickWorld(World, 240);
	TestTrue(TEXT("last departure untriggers TriggerLight16"),
		Light->Light->Intensity < 0.01f);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix4ZonePlayerEventLifecycleSpec,
	"WoT.Parity.Fix4.ZonePlayerEvent.PlayerLifecycle", Fix2Flags)
bool FFix4ZonePlayerEventLifecycleSpec::RunTest(const FString&)
{
	auto WithFixture = [this](const TCHAR* Label,
		TFunctionRef<void(FFix4Battle202Projected&)> Exercise)
	{
		FFix4Battle202Projected Fixture;
		FString Error;
		if (!TestTrue(FString::Printf(TEXT("%s uses the real Battle2_02 linkage"), Label),
			Fix4CreateBattle202Projected(Fixture, Error)))
		{
			AddError(Error);
			return;
		}
		Exercise(Fixture);
		WoTTest::DestroySpecWorld(Fixture.World);
	};

	WithFixture(TEXT("two-player leave"), [this](FFix4Battle202Projected& Fixture)
	{
		AWoTPlayerController* FirstPC = nullptr;
		AWoTPlayerController* SecondPC = nullptr;
		AWoTCharacter* First = Fix2Player(*Fixture.World, FVector::ZeroVector, FirstPC);
		AWoTCharacter* Second = Fix2Player(*Fixture.World, FVector(20, 0, 0), SecondPC);
		First->GetCharacterMovement()->DisableMovement();
		Second->GetCharacterMovement()->DisableMovement();
		WoTTest::TickWorld(Fixture.World, 20);
		TestEqual(TEXT("two players are counted exactly once"),
			Fixture.Zone->ZonePlayerCountForTest(), 2);
		First->Destroy();
		WoTTest::TickWorld(Fixture.World, 10);
		TestEqual(TEXT("destroying one of two players decrements once"),
			Fixture.Zone->ZonePlayerCountForTest(), 1);
		TestTrue(TEXT("TriggerLight16 stays on while the second player remains"),
			Fixture.Light->Light->Intensity > 0.0f);
		Second->Destroy();
		WoTTest::TickWorld(Fixture.World, 240);
		TestEqual(TEXT("destroying the last player clears the count"),
			Fixture.Zone->ZonePlayerCountForTest(), 0);
		TestTrue(TEXT("last destroyed player untriggers TriggerLight16"),
			Fixture.Light->Light->Intensity < 0.01f);
	});

	WithFixture(TEXT("direct destruction"), [this](FFix4Battle202Projected& Fixture)
	{
		AWoTPlayerController* PC = nullptr;
		AWoTCharacter* Pawn = Fix2Player(*Fixture.World, FVector::ZeroVector, PC);
		Pawn->GetCharacterMovement()->DisableMovement();
		WoTTest::TickWorld(Fixture.World, 20);
		Pawn->Destroy();
		Pawn->Destroy();
		WoTTest::TickWorld(Fixture.World, 240);
		TestEqual(TEXT("idempotent direct destruction never makes the count negative"),
			Fixture.Zone->ZonePlayerCountForTest(), 0);
		TestTrue(TEXT("direct destruction untriggers TriggerLight16"),
			Fixture.Light->Light->Intensity < 0.01f);
	});

	WithFixture(TEXT("death and respawn elsewhere"), [this](FFix4Battle202Projected& Fixture)
	{
		AWoTPlayerStart* OutsideStart = Fixture.World->SpawnActor<AWoTPlayerStart>(
			FVector(1200, 0, 0), FRotator::ZeroRotator, Fix2Spawn());
		AWoTPlayerController* PC = nullptr;
		AWoTCharacter* Corpse = Fix2Player(*Fixture.World, FVector::ZeroVector, PC);
		Corpse->GetCharacterMovement()->DisableMovement();
		WoTTest::TickWorld(Fixture.World, 20);
		Corpse->TakeRawDamage(5000, TEXT("hurt"), nullptr,
			Corpse->GetActorLocation());
		PC->ArmRespawnFire(Corpse, 0.0f);
		PC->ServerRespawnPlayer();
		WoTTest::TickWorld(Fixture.World, 240);
		TestTrue(TEXT("production death/respawn replaces the in-zone pawn"),
			OutsideStart && PC->GetPawn() && PC->GetPawn() != Corpse);
		TestEqual(TEXT("death/respawn elsewhere clears the old zone count"),
			Fixture.Zone->ZonePlayerCountForTest(), 0);
		TestTrue(TEXT("death/respawn elsewhere untriggers TriggerLight16"),
			Fixture.Light->Light->Intensity < 0.01f);
	});

	WithFixture(TEXT("disconnect"), [this](FFix4Battle202Projected& Fixture)
	{
		AWoTPlayerController* PC = nullptr;
		AWoTCharacter* Pawn = Fix2Player(*Fixture.World, FVector::ZeroVector, PC);
		Pawn->GetCharacterMovement()->DisableMovement();
		WoTTest::TickWorld(Fixture.World, 20);
		// A production logout unpossesses and destroys this pawn/controller pair.
		PC->UnPossess();
		Pawn->Destroy();
		PC->Destroy();
		WoTTest::TickWorld(Fixture.World, 240);
		TestEqual(TEXT("disconnect teardown clears the counted occupant"),
			Fixture.Zone->ZonePlayerCountForTest(), 0);
		TestTrue(TEXT("disconnect untriggers TriggerLight16"),
			Fixture.Light->Light->Intensity < 0.01f);
	});

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix2CreatureChunkZoneBehaviorSpec,
	"WoT.Parity.Fix2.CreatureChunkZoneBehavior", Fix2Flags)
bool FFix2CreatureChunkZoneBehaviorSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* Water = Fix2Zone(*World,
		FVector(1000, 0, 0), FVector(400, 1200, 400));
	Water->bWaterZone = true;
	Water->ZoneFluidFriction = 1.2f;
	Water->EntrySoundSubPath = TEXT("AmbientA/DIVE");
	AWoTZoneVolume* Destructive = Fix2Zone(*World,
		FVector(3000, 0, 0), FVector(250));
	Destructive->bPainZone = true;
	Destructive->bDestructive = true;
	Destructive->DamageType = TEXT("Burned");

	const TArray<FWoTBodyPart>* Recipe = WoTGoreData::FindRecipe(NAME_None);
	if (!TestTrue(TEXT("production base ChunkUp recipe is installed"),
		Recipe && !Recipe->IsEmpty()))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	auto ChunkUpReplacement = [&](const FVector& Location, int32 Seed,
		TArray<AWoTCreatureChunk*>& OutCreated) -> AWoTCreatureChunk*
	{
		TSet<TWeakObjectPtr<AWoTCreatureChunk>> Before;
		for (TActorIterator<AWoTCreatureChunk> It(World); It; ++It)
		{
			if (IsValid(*It)) Before.Add(*It);
		}
		AWoTCarcass* Source = World->SpawnActor<AWoTCarcass>(
			Location, FRotator::ZeroRotator, Fix2Spawn());
		if (!Source) return nullptr;
		Source->CarcassTypeName = NAME_None;
		WoTRandom::Seed(Seed);
		Source->ChunkUp(100.0f);
		AWoTCreatureChunk* Replacement = nullptr;
		for (TActorIterator<AWoTCreatureChunk> It(World); It; ++It)
		{
			if (!IsValid(*It) || Before.Contains(*It)) continue;
			OutCreated.Add(*It);
			if (!Replacement || It->Mass > Replacement->Mass)
			{
				Replacement = *It;
			}
		}
		return Replacement;
	};

	TArray<AWoTCreatureChunk*> DryCreated;
	TArray<AWoTCreatureChunk*> WetCreated;
	AWoTCreatureChunk* Dry = ChunkUpReplacement(
		FVector(-1000, 0, 0), 0x2442, DryCreated);
	AWoTCreatureChunk* Wet = ChunkUpReplacement(
		FVector(1000, 0, 0), 0x2442, WetCreated);
	if (!TestNotNull(TEXT("dry production ChunkUp creates its replacement"), Dry)
		|| !TestNotNull(TEXT("underwater production ChunkUp creates its replacement"), Wet))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("underwater InitFor halves the complete seeded launch velocity"),
		Wet->GetVelocityForTest().Equals(0.5 * Dry->GetVelocityForTest(), 0.001));
	for (AWoTCreatureChunk* Chunk : DryCreated)
	{
		if (IsValid(Chunk)) Chunk->Destroy();
	}

	AWoTCreatureChunk* Floating = nullptr;
	AWoTCreatureChunk* Sinking = nullptr;
	auto ClassifyBranch = [&](AWoTCreatureChunk* Chunk)
	{
		if (!Chunk || Chunk->Mass <= 0.0f) return;
		const float Ratio = Chunk->Buoyancy / Chunk->Mass;
		if (FMath::IsNearlyEqual(Ratio, 1.06f, 0.0001f) && !Floating)
		{
			Floating = Chunk;
		}
		else if (FMath::IsNearlyEqual(Ratio, 0.94f, 0.0001f) && !Sinking)
		{
			Sinking = Chunk;
		}
	};
	for (AWoTCreatureChunk* Chunk : WetCreated)
	{
		ClassifyBranch(Chunk);
	}
	for (int32 Seed = 1; Seed <= 32 && (!Floating || !Sinking); ++Seed)
	{
		TArray<AWoTCreatureChunk*> Created;
		AWoTCreatureChunk* Replacement = ChunkUpReplacement(
			FVector(1000, Seed * 20.0, 0), Seed, Created);
		ClassifyBranch(Replacement);
		for (AWoTCreatureChunk* Chunk : Created)
		{
			ClassifyBranch(Chunk);
		}
		for (AWoTCreatureChunk* Chunk : Created)
		{
			if (Chunk != Floating && Chunk != Sinking && IsValid(Chunk))
			{
				Chunk->Destroy();
			}
		}
	}
	if (!TestNotNull(TEXT("seeded real ChunkUp reaches the 1.06*Mass float branch"),
		Floating) || !TestNotNull(TEXT("seeded real ChunkUp reaches the 0.94*Mass sink branch"),
		Sinking))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("floating branch receives carcass-base collision flattening"),
		Floating->WasWaterFlattenedForTest());

	Floating->SetActorLocation(FVector(1000, -300, 0), false, nullptr,
		ETeleportType::TeleportPhysics);
	Sinking->SetActorLocation(FVector(1000, 300, 0), false, nullptr,
		ETeleportType::TeleportPhysics);
	UWoTZoneIndex::RefreshAfterRelocation(Floating);
	UWoTZoneIndex::RefreshAfterRelocation(Sinking);
	Floating->SetVelocityForTest(FVector(100, 0, 0));
	Sinking->SetVelocityForTest(FVector(100, 0, 0));
	WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
	const float ExpectedDampedX = 100.0f
		* (1.0f - 2.0f * Water->ZoneFluidFriction / 60.0f);
	TestTrue(TEXT("water falling applies the zone fluid-friction factor"),
		FMath::IsNearlyEqual(Floating->GetVelocityForTest().X,
			ExpectedDampedX, 0.01f));
	TestTrue(TEXT("1.06*Mass buoyancy reverses water gravity upward"),
		Floating->GetVelocityForTest().Z > 0.0f);
	TestTrue(TEXT("0.94*Mass buoyancy retains water gravity downward"),
		Sinking->GetVelocityForTest().Z < 0.0f);

	Sinking->SetActorLocation(FVector(3000, 0, 0), false, nullptr,
		ETeleportType::TeleportPhysics);
	WoTTest::TickWorld(World, 1);
	TestTrue(TEXT("destructive-zone CheckZone immediately destroys the chunk"),
		!IsValid(Sinking) || Sinking->IsActorBeingDestroyed());
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix2TeleporterImmediateZoneRefreshSpec,
	"WoT.Parity.Fix2.TeleporterImmediateZoneRefresh", Fix2Flags)
bool FFix2TeleporterImmediateZoneRefreshSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* SourceZone = Fix2Zone(*World, FVector::ZeroVector, FVector(300));
	AWoTZoneVolume* DestinationZone = Fix2Zone(*World,
		FVector(2000, 0, 0), FVector(300));

	AWoTTeleporter* Destination = World->SpawnActorDeferred<AWoTTeleporter>(
		AWoTTeleporter::StaticClass(), FTransform(FVector(2000, 0, 0)),
		nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	Destination->TeleTag = TEXT("fix2_exit");
	Destination->FinishSpawning(Destination->GetActorTransform());
	AWoTTeleporter* Source = World->SpawnActorDeferred<AWoTTeleporter>(
		AWoTTeleporter::StaticClass(), FTransform::Identity,
		nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	Source->URL = TEXT("fix2_exit");
	Source->FinishSpawning(Source->GetActorTransform());

	AWoTPlayerController* PC = nullptr;
	AWoTCharacter* Player = Fix2Player(*World, FVector(-1000, 0, 0), PC);
	WoTTest::TickWorld(World, 1);
	// Enter through the real overlap path while statically typed as AActor: this
	// is the production call shape that the non-virtual character shadow misses.
	static_cast<AActor*>(Player)->SetActorLocation(FVector::ZeroVector, false, nullptr,
		ETeleportType::TeleportPhysics);
	TestTrue(TEXT("real teleporter overlap lands the player at its destination"),
		Player->GetActorLocation().Equals(Destination->GetActorLocation(), 1.0f));
	UWoTZoneIndex* Index = World->GetSubsystem<UWoTZoneIndex>();
	TestEqual(TEXT("destination zone is refreshed in the teleport frame"),
		Index->GetRegion(Player), DestinationZone);
	TestNotEqual(TEXT("the source region is not left stale"),
		Index->GetRegion(Player), SourceZone);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix2ZoneTrackerPrunesInvalidActorsSpec,
	"WoT.Parity.Fix2.ZoneTrackerPrunesInvalidActors", Fix2Flags)
bool FFix2ZoneTrackerPrunesInvalidActorsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTZoneIndex* Index = World->GetSubsystem<UWoTZoneIndex>();
	AActor* Actor = World->SpawnActor<AActor>(Fix2Spawn());
	Index->UpdateActorZones(Actor);
	TestEqual(TEXT("fixture inserts one tracker entry"),
		Index->TrackedActorCountForTest(), 1);
	Actor->Destroy();
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("post-movement tracker tick prunes invalid weak keys"),
		Index->TrackedActorCountForTest(), 0);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix2SinglePostMovementZoneRefreshSpec,
	"WoT.Parity.Fix2.SinglePostMovementZoneRefresh", Fix2Flags)
bool FFix2SinglePostMovementZoneRefreshSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTZoneIndex* Index = World->GetSubsystem<UWoTZoneIndex>();
	AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 500), FRotator::ZeroRotator, Fix2Spawn());
	AWoTCarcass* Carcass = World->SpawnActor<AWoTCarcass>(
		FVector(1000, 0, 500), FRotator::ZeroRotator, Fix2Spawn());
	AWoTCreatureChunk* Chunk = World->SpawnActor<AWoTCreatureChunk>(
		FVector(2000, 0, 500), FRotator::ZeroRotator, Fix2Spawn());
	Index->ResetUpdateCountsForTest();
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("player has one authoritative post-movement refresh"),
		Index->UpdateCountForTest(Player), 1);
	TestEqual(TEXT("carcass has one authoritative post-movement refresh"),
		Index->UpdateCountForTest(Carcass), 1);
	TestEqual(TEXT("creature chunk has one authoritative post-movement refresh"),
		Index->UpdateCountForTest(Chunk), 1);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
