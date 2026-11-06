#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTCreatureData.h"
#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCasterComponent.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTLegionStompVolcano.h"
#include "../Inventory/WoTAMAReflectors.h"
#include "../Inventory/WoTAngrealInvAMA.h"
#include "../Inventory/WoTAngrealInvAirBurst.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvDisguise.h"
#include "../Inventory/WoTAngrealInvDistantEye.h"
#include "../Inventory/WoTAngrealInvEarthTremor.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvLevitate.h"
#include "../Inventory/WoTAngrealInvLightning.h"
#include "../Inventory/WoTAngrealInvRemoveCurse.h"
#include "../Inventory/WoTAngrealInvShields.h"
#include "../Inventory/WoTAutoTargetReflector.h"
#include "../Inventory/WoTDecreaseChargesLeech.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTReturnToSenderReflector.h"
#include "../Inventory/WoTShieldReflector.h"
#include "../Projectiles/WoTAMAPearl.h"
#include "../Projectiles/WoTArcherArrow.h"
#include "../Projectiles/WoTAngrealAMAProjectile.h"
#include "../Projectiles/WoTDistantEyeProjectile.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTLegionProjectile.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../Projectiles/WoTWeaponProjectile.h"
#include "../WoTBeamEffect.h"
#include "../WoTBloodFX.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTDecal.h"
#include "../WoTGameMode.h"
#include "../WoTMeshFX.h"
#include "../WoTMover.h"
#include "../WoTParticleSprayer.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTPlayerStart.h"
#include "../WoTSoundTable.h"
#include "../WoTTeleporter.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTWeaveFX.h"
#include "WoTTestAIClasses.h"

#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
constexpr EAutomationTestFlags P1A_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters P1A_AlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

template <typename ActorType>
int32 P1A_CountConfig(UWorld* World, FName Config)
{
	int32 Count = 0;
	for (TActorIterator<ActorType> It(World); It; ++It)
	{
		if (IsValid(*It) && It->ConfigName == Config) ++Count;
	}
	return Count;
}

int32 P1A_CountDecals(UWorld* World, FName Config)
{
	int32 Count = 0;
	for (TActorIterator<AWoTDecal> It(World); It; ++It)
	{
		if (IsValid(*It) && It->DecalType == Config) ++Count;
	}
	return Count;
}

AWoTMover* P1A_SpawnSupportMover(UWorld& World)
{
	AWoTMover* Mover = World.SpawnActor<AWoTMover>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	if (!Mover) return nullptr;
	UBoxComponent* Support = NewObject<UBoxComponent>(Mover, TEXT("P1A_Support"));
	Support->SetupAttachment(Mover->GetRootComponent());
	Support->SetBoxExtent(FVector(1000.0f, 1000.0f, 10.0f));
	Support->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Support->SetCollisionObjectType(ECC_WorldDynamic);
	Support->SetCollisionResponseToAllChannels(ECR_Block);
	Support->RegisterComponent();
	return Mover;
}

AStaticMeshActor* P1A_SpawnWall(UWorld& World, const FVector& Location)
{
	AStaticMeshActor* Wall = World.SpawnActor<AStaticMeshActor>(
		Location, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	if (!Wall) return nullptr;
	UBoxComponent* Face = NewObject<UBoxComponent>(Wall, TEXT("P1A_WallFace"));
	Face->SetupAttachment(Wall->GetRootComponent());
	Face->SetBoxExtent(FVector(20.0f, 300.0f, 300.0f));
	Face->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Face->SetCollisionObjectType(ECC_WorldStatic);
	Face->SetCollisionResponseToAllChannels(ECR_Block);
	Face->RegisterComponent();
	return Wall;
}

template <typename T>
T* P1A_GiveHeld(UWorld& World, AWoTCharacter* Owner, int32 Charges = 20)
{
	if (!Owner) return nullptr;
	const FTransform At(Owner->GetActorRotation(), Owner->GetActorLocation());
	T* Item = World.SpawnActorDeferred<T>(T::StaticClass(), At, Owner, Owner,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Item) return nullptr;
	Item->bHeldItem = true;
	Item->FinishSpawning(At);
	Item->CurCharges = Charges;
	Item->GiveTo(Owner);
	return Item;
}

int32 P1A_CountWeaveBursts(UWorld* World, FName Config)
{
	int32 Count = 0;
	for (TActorIterator<AWoTWeaveBurst> It(World); It; ++It)
	{
		if (IsValid(*It) && It->Config == Config) ++Count;
	}
	return Count;
}

int32 P1A_CountSounds(UWorld* World, FName SubPath)
{
	int32 Count = 0;
	for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
	{
		if (IsValid(*It) && It->SoundSubPath == SubPath) ++Count;
	}
	return Count;
}

AWoTTestCasterNPC* P1A_SpawnCaster(UWorld& World, const FVector& Location)
{
	AWoTTestCasterNPC* NPC = World.SpawnActor<AWoTTestCasterNPC>(
		Location, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	if (NPC)
	{
		NPC->Health = 100;
		NPC->AssignedTeam = 1;
		NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
	WoTTest::TickWorld(&World, 0.05f);
	return NPC;
}

AStaticMeshActor* P1A_SpawnFloor(UWorld& World, const FVector& Location,
	const FVector& Extent = FVector(1000.0f, 1000.0f, 20.0f))
{
	AStaticMeshActor* Floor = World.SpawnActor<AStaticMeshActor>(
		Location, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	if (!Floor) return nullptr;
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("P1A_FloorBox"));
	Box->SetupAttachment(Floor->GetRootComponent());
	Box->SetBoxExtent(Extent);
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->RegisterComponent();
	return Floor;
}

bool P1A_VerifyChargeLeech(FAutomationTestBase& Test,
	TSubclassOf<UWoTDecreaseChargesLeech> LeechClass,
	const TArray<TSubclassOf<AWoTAngrealInventory>>& ItemClasses)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Host = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	TArray<AWoTAngrealInventory*> Items;
	for (TSubclassOf<AWoTAngrealInventory> ItemClass : ItemClasses)
	{
		const FTransform At(Host->GetActorLocation());
		AWoTAngrealInventory* Item = World->SpawnActorDeferred<AWoTAngrealInventory>(
			ItemClass, At, Host, Host, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Item)
		{
			Item->bHeldItem = true;
			Item->FinishSpawning(At);
			Item->GiveTo(Host);
			Items.Add(Item);
		}
	}
	bool bOK = Test.TestEqual(TEXT("three real held artifacts enter the fixture"),
		Items.Num(), 3);
	if (Items.Num() == 3)
	{
		// GiveTo inserts at the head, so Items[2] is the first entry visited.
		// Empty that entry to exercise the in-loop removal on every rarity.
		Items[0]->CurCharges = 2;
		Items[1]->CurCharges = 2;
		Items[2]->CurCharges = 1;
		UWoTDecreaseChargesLeech* Leech = NewObject<UWoTDecreaseChargesLeech>(
			Host, LeechClass);
		Leech->AttachTo(Host);
		WoTTest::TickWorld(World, Leech->AffectResolution + 0.05f);
		bOK &= Test.TestFalse(TEXT("the artifact emptied first is unlinked"),
			Host->InventoryComponent->Items.Contains(Items[2]));
		bOK &= Test.TestEqual(TEXT("the following artifact drains exactly once"),
			Items[1]->CurCharges, 1);
		bOK &= Test.TestEqual(TEXT("the final artifact drains exactly once"),
			Items[0]->CurCharges, 1);
	}
	WoTTest::DestroySpecWorld(World);
	return bOK;
}

#if WITH_EDITOR
UE::Net::FTestWorldInstance P1A_CreateListenServer()
{
	using namespace UE::Net;
	FTestWorldInstance Instance(/*bDelayedInit*/ true);
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->AddToRoot();
	FGameInstancePIEParameters Parameters;
	Parameters.bSimulateInEditor = false;
	Parameters.bAnyBlueprintErrors = false;
	Parameters.bStartInSpectatorMode = false;
	Parameters.bRunAsDedicated = false;
	Parameters.bIsPrimaryPIEClient = false;
	Parameters.WorldFeatureLevel = ERHIFeatureLevel::Num;
	Parameters.EditorPlaySettings = nullptr;
	Parameters.NetMode = PIE_ListenServer;
	Parameters.OverrideMapURL = TEXT("/Engine/Maps/Entry");
	int32 PIEInstance = INDEX_NONE;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		PIEInstance = FMath::Max(PIEInstance, Context.PIEInstance);
	}
	++PIEInstance;
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		const FGameInstancePIEResult Initialized =
			GameInstance->InitializeForPlayInEditor(PIEInstance, Parameters);
		if (!Initialized.IsSuccess())
		{
			GameInstance->Shutdown();
			GameInstance->RemoveFromRoot();
			return Instance;
		}
	}
	Instance.GameInstance = GameInstance;
	FWorldContext* WorldContext = GameInstance->GetWorldContext();
	UGameViewportClient* ViewportClient = NewObject<UGameViewportClient>(
		GEngine, GEngine->GameViewportClientClass);
	ViewportClient->Init(*WorldContext, GameInstance);
	WorldContext->GameViewport = ViewportClient;
	FString CreatePlayerError;
	ViewportClient->SetupInitialLocalPlayer(CreatePlayerError);
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		FURL URL(nullptr,
			TEXT("/Engine/Maps/Entry?listen?Game=/Script/WoT.WoTGameMode"),
			TRAVEL_Absolute);
		FString Error;
		Instance.bIsLoaded = GEngine->Browse(
			*GameInstance->GetWorldContext(), URL, Error)
			== EBrowseReturnVal::Success;
	}
	return Instance;
}

bool P1A_StartListenRemote(FAutomationTestBase& Test, UE::Net::FTestWorlds& Worlds)
{
	Worlds.Server = P1A_CreateListenServer();
	Worlds.SetDescription(TEXT("P1A replicated weapon impact"));
	if (!Test.TestTrue(TEXT("real listen server loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	Test.TestEqual(TEXT("authority fixture is a listen server"),
		Worlds.Server.GetWorld()->GetNetMode(), NM_ListenServer);
	Worlds.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
		FVector(10000.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		P1A_AlwaysSpawn());
	return Test.TestTrue(TEXT("real remote client connects"),
		Worlds.CreateAndConnectClient());
}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AF2ArrowRemoteImpactSpec,
	"WoT.Parity.P1A.F2Review.ArcherArrowRemoteImpactOnce", P1A_Flags)

bool FWoTP1AF2ArrowRemoteImpactSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds;
	if (!P1A_StartListenRemote(*this, Worlds)) return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	UWorld* ClientWorld = Worlds.Clients[0].GetWorld();
	APlayerController* RemoteController = Worlds.GetServerPlayerControllerOfClient(0);
	AWoTCharacter* Target = RemoteController
		? Cast<AWoTCharacter>(RemoteController->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("remote client's real authority pawn"), Target)) return false;
	Target->SetActorLocation(FVector(5000.0f, 5000.0f, 100.0f));
	Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Target->ForceNetUpdate();
	AWoTArcherArrow* Arrow = ServerWorld->SpawnActor<AWoTArcherArrow>(
		FVector(0.0f, 5000.0f, 100.0f), FRotator::ZeroRotator,
		P1A_AlwaysSpawn());
	if (!TestNotNull(TEXT("real Archer arrow launches"), Arrow)) return false;
	Arrow->ForceNetUpdate();
	TestTrue(TEXT("remote client sees one ArrowTrail at the real pawn impact"),
		Worlds.TickUntil([&]()
		{
			return P1A_CountConfig<AWoTMeshEffect>(ClientWorld,
				TEXT("ArrowTrail")) == 1;
		}, 240));
	TestEqual(TEXT("remote impact bundle is emitted exactly once"),
		P1A_CountConfig<AWoTMeshEffect>(ClientWorld, TEXT("ArrowTrail")), 1);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AF2MyrddraalRemoteWallImpactSpec,
	"WoT.Parity.P1A.F2Review.MyrddraalRemoteWallImpactOnce", P1A_Flags)

bool FWoTP1AF2MyrddraalRemoteWallImpactSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds;
	if (!P1A_StartListenRemote(*this, Worlds)) return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	UWorld* ClientWorld = Worlds.Clients[0].GetWorld();
	const FVector WallLocation(2500.0f, 5000.0f, 100.0f);
	AStaticMeshActor* ServerWall = P1A_SpawnWall(*ServerWorld, WallLocation);
	AStaticMeshActor* ClientWall = P1A_SpawnWall(*ClientWorld, WallLocation);
	if (!TestNotNull(TEXT("authority world wall"), ServerWall)
		|| !TestNotNull(TEXT("remote client's matching map wall"), ClientWall))
	{
		return false;
	}
	AWoTMyrddraalBolt* ServerBolt = ServerWorld->SpawnActor<AWoTMyrddraalBolt>(
		FVector(0.0f, 5000.0f, 100.0f), FRotator::ZeroRotator,
		P1A_AlwaysSpawn());
	if (!TestNotNull(TEXT("real Myrddraal bolt launches"), ServerBolt)) return false;
	ServerBolt->ForceNetUpdate();
	AWoTMyrddraalBolt* ClientBolt = nullptr;
	TestTrue(TEXT("remote client receives the live bolt and both flight trails"),
		Worlds.TickUntil([&]()
		{
			for (TActorIterator<AWoTMyrddraalBolt> It(ClientWorld); It; ++It)
			{
				if (IsValid(*It)) ClientBolt = *It;
			}
			int32 LiveTrails = 0;
			for (TActorIterator<AWoTParticleSprayer> It(ClientWorld); It; ++It)
			{
				if (It->FollowActor.Get() == ClientBolt
					&& (It->ConfigName == TEXT("MyrdSprayerA")
						|| It->ConfigName == TEXT("MyrdSprayerB")) && It->bOn)
				{
					++LiveTrails;
				}
			}
			return ClientBolt && LiveTrails == 2;
		}, 120));
	TestTrue(TEXT("remote wall impact immediately creates one complete bundle"),
		Worlds.TickUntil([&]()
		{
			return P1A_CountConfig<AWoTParticleSprayer>(ClientWorld,
					TEXT("MyrdSprayerExp")) == 1
				&& P1A_CountConfig<AWoTExplosionEffect>(ClientWorld,
					TEXT("MyrddraalElectric")) == 1
				&& P1A_CountDecals(ClientWorld, TEXT("CrackDecal")) == 1;
		}, 240));
	int32 StoppedTrails = 0;
	for (TActorIterator<AWoTParticleSprayer> It(ClientWorld); It; ++It)
	{
		if (It->FollowActor.Get() == ClientBolt
			&& (It->ConfigName == TEXT("MyrdSprayerA")
				|| It->ConfigName == TEXT("MyrdSprayerB")) && !It->bOn)
		{
			++StoppedTrails;
		}
	}
	TestEqual(TEXT("both remote flight trails stop at impact"), StoppedTrails, 2);
	TestEqual(TEXT("remote impact burst is emitted exactly once"),
		P1A_CountConfig<AWoTParticleSprayer>(ClientWorld, TEXT("MyrdSprayerExp")), 1);
	TestEqual(TEXT("remote electric effect is emitted exactly once"),
		P1A_CountConfig<AWoTExplosionEffect>(ClientWorld, TEXT("MyrddraalElectric")), 1);
	TestEqual(TEXT("remote crack is emitted exactly once"),
		P1A_CountDecals(ClientWorld, TEXT("CrackDecal")), 1);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CWeaponLateJoinStuckSpec,
	"WoT.Parity.P1C.P1AReview.WeaponProjectileLateJoinStuckPresentation", P1A_Flags)

bool FWoTP1CWeaponLateJoinStuckSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds;
	Worlds.Server = P1A_CreateListenServer();
	Worlds.SetDescription(TEXT("P1C late-join stuck weapon projectile"));
	if (!TestTrue(TEXT("real listen server loads"), Worlds.Server.IsLoaded())) return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	const FVector WallLocation(500.0f, 6500.0f, 100.0f);
	if (!TestNotNull(TEXT("authority wall"), P1A_SpawnWall(*ServerWorld, WallLocation)))
	{
		return false;
	}
	AWoTMyrddraalBolt* ServerBolt = ServerWorld->SpawnActor<AWoTMyrddraalBolt>(
		FVector(0.0f, 6500.0f, 100.0f), FRotator::ZeroRotator,
		P1A_AlwaysSpawn());
	if (!TestNotNull(TEXT("real Myrddraal bolt launches"), ServerBolt)) return false;
	TestTrue(TEXT("bolt reaches the real wall before the remote client joins"),
		Worlds.TickUntil([&]()
		{
			return IsValid(ServerBolt) && ServerBolt->ProjectileMovement
				&& !ServerBolt->ProjectileMovement->IsActive();
		}, 120));
	ServerWorld->SpawnActor<AWoTPlayerStart>(
		FVector(10000.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		P1A_AlwaysSpawn());
	if (!TestTrue(TEXT("late remote client connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	UWorld* ClientWorld = Worlds.Clients[0].GetWorld();
	AWoTMyrddraalBolt* ClientBolt = nullptr;
	TestTrue(TEXT("late client receives the already-stuck bolt"),
		Worlds.TickUntil([&]()
		{
			for (TActorIterator<AWoTMyrddraalBolt> It(ClientWorld); It; ++It)
			{
				if (IsValid(*It)) ClientBolt = *It;
			}
			return ClientBolt != nullptr;
		}, 120));
	int32 LiveTrails = 0;
	for (TActorIterator<AWoTParticleSprayer> It(ClientWorld); It; ++It)
	{
		if (It->FollowActor.Get() == ClientBolt
			&& (It->ConfigName == TEXT("MyrdSprayerA")
				|| It->ConfigName == TEXT("MyrdSprayerB")) && It->bOn)
		{
			++LiveTrails;
		}
	}
	TestEqual(TEXT("stuck initial state never creates flight sprayers"), LiveTrails, 0);
	TestEqual(TEXT("a historical impact never bursts for a late joiner"),
		P1A_CountConfig<AWoTParticleSprayer>(ClientWorld, TEXT("MyrdSprayerExp")), 0);
	TestEqual(TEXT("a historical impact never creates late electric FX"),
		P1A_CountConfig<AWoTExplosionEffect>(ClientWorld, TEXT("MyrddraalElectric")), 0);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CWeaponListenImpactOnceSpec,
	"WoT.Parity.P1C.P1AReview.WeaponProjectileListenHostImpactOnce", P1A_Flags)

bool FWoTP1CWeaponListenImpactOnceSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds;
	Worlds.Server = P1A_CreateListenServer();
	if (!TestTrue(TEXT("real listen server loads"), Worlds.Server.IsLoaded())) return false;
	UWorld* World = Worlds.Server.GetWorld();
	const FVector WallLocation(500.0f, 7000.0f, 100.0f);
	P1A_SpawnWall(*World, WallLocation);
	AWoTMyrddraalBolt* Bolt = World->SpawnActor<AWoTMyrddraalBolt>(
		FVector(0.0f, 7000.0f, 100.0f), FRotator::ZeroRotator,
		P1A_AlwaysSpawn());
	if (!TestNotNull(TEXT("real bolt launches on the host"), Bolt)) return false;
	bool bImpacted = false;
	for (int32 Frame = 0; Frame < 120 && !bImpacted; ++Frame)
	{
		Worlds.TickAll();
		bImpacted = P1A_CountConfig<AWoTParticleSprayer>(World,
			TEXT("MyrdSprayerExp")) == 1;
	}
	TestTrue(TEXT("listen host observes the real wall impact"), bImpacted);
	TestEqual(TEXT("listen host impact burst appears exactly once"),
		P1A_CountConfig<AWoTParticleSprayer>(World, TEXT("MyrdSprayerExp")), 1);
	TestEqual(TEXT("listen host electric impact appears exactly once"),
		P1A_CountConfig<AWoTExplosionEffect>(World, TEXT("MyrddraalElectric")), 1);
	TestEqual(TEXT("listen host crack appears exactly once"),
		P1A_CountDecals(World, TEXT("CrackDecal")), 1);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CWeaponTemporaryChannelSpec,
	"WoT.Parity.P1C.P1AReview.WeaponProjectileChannelClosesAfterInitialBunch", P1A_Flags)

bool FWoTP1CWeaponTemporaryChannelSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds;
	if (!P1A_StartListenRemote(*this, Worlds)) return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	UWorld* ClientWorld = Worlds.Clients[0].GetWorld();
	AWoTMyrddraalBolt* ServerBolt = ServerWorld->SpawnActor<AWoTMyrddraalBolt>(
		FVector(0.0f, 8500.0f, 100.0f), FRotator::ZeroRotator,
		P1A_AlwaysSpawn());
	if (!TestNotNull(TEXT("real temporary bolt launches"), ServerBolt)) return false;
	ServerBolt->ForceNetUpdate();
	TestTrue(TEXT("client receives the initial weapon-projectile bunch"),
		Worlds.TickUntil([&]()
		{
			for (TActorIterator<AWoTMyrddraalBolt> It(ClientWorld); It; ++It)
			{
				if (IsValid(*It)) return true;
			}
			return false;
		}, 120));
	UNetDriver* Driver = ServerWorld->GetNetDriver();
	UNetConnection* Connection = Driver && !Driver->ClientConnections.IsEmpty()
		? Driver->ClientConnections[0] : nullptr;
	if (!TestNotNull(TEXT("real remote net connection"), Connection)) return false;
	TestTrue(TEXT("temporary actor channel closes after the initial bunch"),
		Worlds.TickUntil([&]()
		{
			return Connection->FindActorChannelRef(ServerBolt) == nullptr;
		}, 120));
	TestTrue(TEXT("weapon projectile retains engine temporary-actor semantics"),
		ServerBolt->bNetTemporary);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AF2LegionDestroyedSupportSpec,
	"WoT.Parity.P1A.F2Review.LegionDestroyedSupportFalls", P1A_Flags)

bool FWoTP1AF2LegionDestroyedSupportSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* Support = P1A_SpawnSupportMover(*World);
	AWoTNPCCharacter* Legion = WoTCreatureData::SpawnCreature(*World, TEXT("Legion"),
		FVector(0.0f, 0.0f, 100.0f), 1, true);
	if (Legion && Legion->GetCharacterMovement())
	{
		Legion->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
	if (!TestNotNull(TEXT("real mover support"), Support)
		|| !TestNotNull(TEXT("real Legion"), Legion))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Legion->Health = 10;
	FWoTDamageEvent Damage;
	Damage.WoTDamageType = TEXT("Sliced");
	Legion->TakeDamage(20.0f, Damage, nullptr, Legion);
	AWoTCarcass* Carcass = nullptr;
	for (int32 Frame = 0; Frame < 720 && (!Carcass
		|| !Carcass->IsSinkingForTest()); ++Frame)
	{
		for (TActorIterator<AWoTCarcass> It(World); It; ++It)
		{
			if (It->CarcassTypeName.ToString().Contains(TEXT("Legion"))) Carcass = *It;
		}
		WoTTest::TickWorld(World, 1, 1.0f / 120.0f);
	}
	if (TestTrue(TEXT("real Legion death lands and sinks on the mover"),
		Carcass && Carcass->IsSinkingForTest()
			&& Carcass->GetSinkingBaseForTest() == Support))
	{
		const float BeforeZ = Carcass->GetActorLocation().Z;
		Support->Destroy();
		WoTTest::TickWorld(World, 30, 1.0f / 120.0f);
		TestFalse(TEXT("destroyed support sends the sinking carcass into falling physics"),
			Carcass->IsLyingStill());
		TestTrue(TEXT("falling and sinking move the corpse below an unsupported hover"),
			Carcass->GetActorLocation().Z < BeforeZ - 20.0f);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AAMAPearlProjectileListSpec,
	"WoT.Parity.P1A.AMAPearl.ProjectileListIgnoresCollision", P1A_Flags)

bool FWoTP1AAMAPearlProjectileListSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* EyeCaster = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	EyeCaster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvDistantEye* EyeItem = P1A_GiveHeld<AWoTAngrealInvDistantEye>(
		*World, EyeCaster);
	EyeCaster->InventoryComponent->SelectItem(EyeItem);
	WoTTest::TickWorld(World, 0.05f);
	EyeCaster->InventoryComponent->UseAngreal();
	AWoTDistantEyeProjectile* Eye = EyeItem ? EyeItem->GetEye() : nullptr;
	if (!TestNotNull(TEXT("real Distant Eye cast creates its collision-ignoring projectile"), Eye))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	AWoTCharacter* Unraveler = World->SpawnActor<AWoTCharacter>(
		Eye->GetActorLocation(), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Unraveler->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvRemoveCurse* Remove =
		P1A_GiveHeld<AWoTAngrealInvRemoveCurse>(*World, Unraveler);
	Unraveler->InventoryComponent->SelectItem(Remove);
	const int32 SparkBefore = P1A_CountWeaveBursts(World, TEXT("WOTSparks"));
	WoTTest::TickWorld(World, 0.05f);
	Unraveler->InventoryComponent->UseAngreal();
	TWeakObjectPtr<AWoTDistantEyeProjectile> EyePtr = Eye;
	WoTTest::TickWorld(World, 0.25f);
	TestFalse(TEXT("a real Unravel dome absorbs Distant Eye despite its Pawn-channel ignores"),
		EyePtr.IsValid());
	TestEqual(TEXT("the collision-independent absorption emits one WOTSparks bundle"),
		P1A_CountWeaveBursts(World, TEXT("WOTSparks")), SparkBefore + 1);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AAMABoxAbsorbedSpec,
	"WoT.Parity.P1A.AMA.TriggeredBoxIsAbsorbed", P1A_Flags)

bool FWoTP1AAMABoxAbsorbedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AStaticMeshActor* Floor = P1A_SpawnFloor(*World, FVector(0, 0, -20));
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 180), FRotator(-90, 0, 0), P1A_AlwaysSpawn());
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvAMA* AMA = P1A_GiveHeld<AWoTAngrealInvAMA>(*World, Caster);
	Caster->InventoryComponent->SelectItem(AMA);
	Caster->SetActorRotation(FRotator(-90, 0, 0));
	WoTTest::TickWorld(World, 0.05f);
	TestTrue(TEXT("the real AMA artifact accepts the cast"),
		Caster->InventoryComponent->UseAngreal());
	AWoTAngrealAMAProjectile* Mine = nullptr;
	for (TActorIterator<AWoTAngrealAMAProjectile> It(World); It; ++It) Mine = *It;
	if (!TestNotNull(TEXT("the real AMA cast launches its box"), Mine))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FHitResult Landing;
	Landing.Location = Mine->GetActorLocation();
	Landing.ImpactPoint = Landing.Location;
	Landing.ImpactNormal = FVector::UpVector;
	Mine->NotifyHit(nullptr, Floor, nullptr, true, Landing.Location,
		Landing.ImpactNormal, FVector::ZeroVector, Landing);
	if (!TestTrue(TEXT("the real AMA cast lands and places its box"),
		Mine && Mine->IsPlaced()))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTCharacter* Trigger = World->SpawnActor<AWoTCharacter>(
		Mine->GetActorLocation() + FVector(200, 0, 0), FRotator::ZeroRotator,
		P1A_AlwaysSpawn());
	Trigger->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	const int32 SparkBefore = P1A_CountWeaveBursts(World, TEXT("WOTSparks"));
	TWeakObjectPtr<AWoTAngrealAMAProjectile> MinePtr = Mine;
	Trigger->SetActorLocation(Mine->GetActorLocation(), false, nullptr,
		ETeleportType::TeleportPhysics);
	WoTTest::TickWorld(World, 0.1f);
	AWoTAMAPearl* Pearl = nullptr;
	for (TActorIterator<AWoTAMAPearl> It(World); It; ++It) Pearl = *It;
	TestFalse(TEXT("the newly planted dome immediately absorbs its triggering box"),
		MinePtr.IsValid());
	TestTrue(TEXT("the real pearl records exactly one box absorption"),
		Pearl && Pearl->GetAbsorptionCountForTest() == 1);
	TestEqual(TEXT("box absorption emits the retail WOTSparks cue exactly once"),
		P1A_CountWeaveBursts(World, TEXT("WOTSparks")), SparkBefore + 1);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AEarthTrajectorySpec,
	"WoT.Parity.P1A.EarthTremor.NPCLobTrajectory", P1A_Flags)

bool FWoTP1AEarthTrajectorySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestCasterNPC* NPC = P1A_SpawnCaster(*World, FVector::ZeroVector);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 0), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvEarthTremor* Earth = P1A_GiveHeld<AWoTAngrealInvEarthTremor>(
		*World, NPC, 20);
	if (!TestNotNull(TEXT("real caster NPC controller"), NPC ? NPC->GetController() : nullptr)
		|| !TestNotNull(TEXT("Earth Tremor in the NPC inventory"), Earth))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Earth->SetVictim(Victim);
	Earth->InitChargeGroup();
	NPC->Caster->BeginCast(Earth);
	AWoTEarthTremorProjectile* Projectile = nullptr;
	for (TActorIterator<AWoTEarthTremorProjectile> It(World); It; ++It)
	{
		Projectile = *It;
	}
	if (TestNotNull(TEXT("real NPC cast launches Earth Tremor"), Projectile))
	{
		const FVector V = Projectile->ProjectileMovement->Velocity;
		TestTrue(FString::Printf(TEXT("NPC pre-lob plus BeginPlay boost gives retail rise (%.3f)"),
			V.Z / FMath::Max(FMath::Abs(V.X), 1.0f)),
			V.Z / FMath::Max(FMath::Abs(V.X), 1.0f) > 0.55f);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CEarthReturnToSenderLobSpec,
	"WoT.Parity.P1C.P1AReview.EarthTremorReturnToSenderLob", P1A_Flags)

bool FWoTP1CEarthReturnToSenderLobSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Sender = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	AWoTCharacter* ReflectorHost = World->SpawnActor<AWoTCharacter>(
		FVector(1000.0f, 0.0f, 0.0f), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	UWoTReturnToSenderReflector* Reflector =
		NewObject<UWoTReturnToSenderReflector>(ReflectorHost);
	Reflector->Install(ReflectorHost);

	FActorSpawnParameters ProjectileParams = P1A_AlwaysSpawn();
	ProjectileParams.Owner = Sender;
	ProjectileParams.Instigator = Sender;
	AWoTEarthTremorProjectile* Incoming = World->SpawnActor<AWoTEarthTremorProjectile>(
		FVector(900.0f, 0.0f, 0.0f), FRotator::ZeroRotator, ProjectileParams);
	if (!TestNotNull(TEXT("real Earth Tremor projectile"), Incoming))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Incoming->SourcePawn = Sender;
	Incoming->ProcessTouch(ReflectorHost, ReflectorHost->GetActorLocation());

	AWoTEarthTremorProjectile* Returned = nullptr;
	for (TActorIterator<AWoTEarthTremorProjectile> It(World); It; ++It)
	{
		if (*It != Incoming && It->GetInstigator() == ReflectorHost)
		{
			Returned = *It;
			break;
		}
	}
	if (TestNotNull(TEXT("Return-to-Sender creates a reflected Earth Tremor"), Returned))
	{
		const FVector Velocity = Returned->ProjectileMovement->Velocity;
		TestTrue(TEXT("the reflected projectile travels back toward its sender"),
			Velocity.X < 0.0f);
		TestTrue(FString::Printf(TEXT("the reflected projectile keeps the Earth Tremor lob (%.3f)"),
			Velocity.Z / FMath::Max(Velocity.Size2D(), 1.0f)),
			Velocity.Z / FMath::Max(Velocity.Size2D(), 1.0f) > 0.3f);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AEarthMinRangeSpec,
	"WoT.Parity.P1A.EarthTremor.NPCMinRange300", P1A_Flags)

bool FWoTP1AEarthMinRangeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestCasterNPC* NPC = P1A_SpawnCaster(*World, FVector::ZeroVector);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(290, 0, 0), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvEarthTremor* Earth = P1A_GiveHeld<AWoTAngrealInvEarthTremor>(
		*World, NPC, 20);
	Earth->Priority = 100.0f;
	Earth->InitChargeGroup();
	WoTTest::TickWorld(World, 1.0f);
	AWoTAngrealInventory* TooClose = NPC->Caster->SelectOffensiveWeave(Victim);
	TestTrue(TEXT("a real caster refuses Earth Tremor below the authored 300 range"),
		TooClose != Earth);
	Victim->SetActorLocation(FVector(310, 0, 0));
	AWoTAngrealInventory* InRange = NPC->Caster->SelectOffensiveWeave(Victim);
	TestEqual(TEXT("the same caster selects Earth Tremor once beyond 300"),
		InRange, static_cast<AWoTAngrealInventory*>(Earth));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AAirBurstMaxRangeSpec,
	"WoT.Parity.P1A.AirBurst.NPCMaxRange120", P1A_Flags)

bool FWoTP1AAirBurstMaxRangeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestCasterNPC* NPC = P1A_SpawnCaster(*World, FVector::ZeroVector);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 0), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvAirBurst* Air = P1A_GiveHeld<AWoTAngrealInvAirBurst>(
		*World, NPC, 20);
	Air->Priority = 100.0f;
	Air->InitChargeGroup();
	WoTTest::TickWorld(World, 1.0f);
	TestTrue(TEXT("a real caster does not select Air Burst across the room"),
		NPC->Caster->SelectOffensiveWeave(Victim) != Air);
	Victim->SetActorLocation(FVector(100, 0, 0));
	TestEqual(TEXT("Air Burst remains selectable inside its 120-unit reach"),
		NPC->Caster->SelectOffensiveWeave(Victim),
		static_cast<AWoTAngrealInventory*>(Air));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1ADisguiseFocusExclusionSpec,
	"WoT.Parity.P1A.Disguise.ExcludesMashadarFocus", P1A_Flags)

bool FWoTP1ADisguiseFocusExclusionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Player->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTNPCCharacter* Focus = WoTCreatureData::SpawnCreature(*World,
		TEXT("MashadarFocusPawn"), FVector(30, 0, 0), 2, true);
	AWoTNPCCharacter* Trolloc = WoTCreatureData::SpawnCreature(*World,
		TEXT("Trolloc"), FVector(60, 0, 0), 2, true);
	AWoTAngrealInvDisguise* Disguise =
		P1A_GiveHeld<AWoTAngrealInvDisguise>(*World, Player, 20);
	if (!TestNotNull(TEXT("real Mashadar focus pawn"), Focus)
		|| !TestNotNull(TEXT("nearest real creature"), Trolloc))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Player->InventoryComponent->SelectItem(Disguise);
	WoTTest::TickWorld(World, 0.05f);
	Player->InventoryComponent->UseAngreal();
	TestEqual(TEXT("the real Disguise cast skips the nearer focus proxy"),
		Disguise->DisguisedAs.Get(), static_cast<APawn*>(Trolloc));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1ALevitateTimerSpec,
	"WoT.Parity.P1A.Levitate.ChargeRemainderAcrossCasts", P1A_Flags)

bool FWoTP1ALevitateTimerSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	AWoTAngrealInvLevitate* Levitate =
		P1A_GiveHeld<AWoTAngrealInvLevitate>(*World, Player, 10);
	Levitate->CastArtifact();
	Levitate->Tick(0.25f);
	const int32 AfterFirstTap = Levitate->CurCharges;
	Levitate->UnCastArtifact();
	Levitate->CastArtifact();
	Levitate->Tick(0.25f);
	TestEqual(TEXT("two short real activations share one drain interval"),
		Levitate->CurCharges, AfterFirstTap);
	Levitate->UnCastArtifact();
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AShieldPrioritySpec,
	"WoT.Parity.P1C.P1AReview.ShieldPriorityUsesHitPoints", P1A_Flags)

bool FWoTP1AShieldPrioritySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestCasterNPC* NPC = P1A_SpawnCaster(*World, FVector::ZeroVector);
	AWoTAngrealInvShield* ShieldItem =
		P1A_GiveHeld<AWoTAngrealInvShield>(*World, NPC, 10);
	NPC->InventoryComponent->SelectItem(ShieldItem);
	NPC->InventoryComponent->UseAngreal();
	NPC->InventoryComponent->CeaseUsingAngreal();
	UWoTShieldReflector* Shield = NPC->FindComponentByClass<UWoTShieldReflector>();
	if (TestNotNull(TEXT("real shield cast installs the shield"), Shield))
	{
		TestEqual(TEXT("installed Personal Shield has zero AI priority"),
			ShieldItem->GetPriority(), 0.0f);
		Shield->ShieldHitPoints = 0;
		TestEqual(TEXT("an installed but exhausted shield restores authored priority"),
			ShieldItem->GetPriority(), ShieldItem->Priority);
		Shield->ShieldHitPoints = 25;
		Shield->Uninstall();
		TestEqual(TEXT("an absent shield immediately restores authored priority without cooldown"),
			ShieldItem->GetPriority(), ShieldItem->Priority);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CSisterAdaptedShieldSpec,
	"WoT.Parity.P1C.P1AReview.SisterDoesNotRecastAdaptedShield", P1A_Flags)

bool FWoTP1CSisterAdaptedShieldSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTNPCCharacter* Sister = WoTCreatureData::SpawnCreature(*World,
		TEXT("Sister"), FVector(0, 0, 100), 1, true);
	if (Sister) Sister->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	APlayerController* EnemyController = World->SpawnActor<APlayerController>();
	EnemyController->InitPlayerState();
	if (AWoTPlayerState* State = EnemyController->GetPlayerState<AWoTPlayerState>())
	{
		State->Team = 2;
	}
	AWoTCharacter* Enemy = World->SpawnActor<AWoTCharacter>(
		FVector(600, 0, 100), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	EnemyController->Possess(Enemy);
	Enemy->Health = 10000;
	Enemy->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	if (Sister && Sister->GetController())
	{
		AController* Previous = Sister->GetController();
		Previous->UnPossess();
		Previous->Destroy();
	}
	AWoTP1CDefensivePulseController* SisterController =
		World->SpawnActor<AWoTP1CDefensivePulseController>(
			FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	if (SisterController && Sister)
	{
		SisterController->Possess(Sister);
		SisterController->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	}
	UWoTCasterComponent* Caster = Sister
		? Sister->FindComponentByClass<UWoTCasterComponent>() : nullptr;
	AWoTAngrealInvShield* ShieldItem = Sister ? Cast<AWoTAngrealInvShield>(
		Sister->InventoryComponent->FindInventoryType(
			AWoTAngrealInvShield::StaticClass())) : nullptr;
	AWoTAngrealInvAirBurst* Source = World->SpawnActor<AWoTAngrealInvAirBurst>(
		FVector(9000), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	if (!TestNotNull(TEXT("real Sister NPC controller"), SisterController)
		|| !TestNotNull(TEXT("real Sister caster adapter"), Caster)
		|| !TestNotNull(TEXT("real Sister personal shield"), ShieldItem)
		|| !TestNotNull(TEXT("incoming weave source"), Source))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	for (AWoTInventory* Item : Sister->InventoryComponent->Items)
	{
		if (AWoTAngrealInventory* Weave = Cast<AWoTAngrealInventory>(Item))
		{
			// Keep the real controller in Tracking so this fixture isolates its
			// defensive detector/selection path from unrelated offensive volleys.
			Weave->bOffensive = false;
		}
	}
	Caster->MinDefensiveUsageInterval = 0.0f;
	ShieldItem->MinChargeGroupInterval = 0.0f;
	ShieldItem->MinChargesInGroup = 5;
	ShieldItem->MaxChargesInGroup = 5;
	ShieldItem->RoundsPerMinute = 6000.0f;
	WoTTest::TickWorld(World, 0.25f);
	auto SpawnIncoming = [&]()
	{
		AWoTTestSeeker* Offender = World->SpawnActor<AWoTTestSeeker>(
			Sister->GetActorLocation() + FVector(100, 0, 0),
			FRotator(0, 180, 0), P1A_AlwaysSpawn());
		if (Offender)
		{
			Offender->SetSourceAngreal(Source);
			Offender->Destination = Sister;
			Offender->ProjectileMovement->Velocity = FVector(-40, 0, 0);
			Offender->ProjectileMovement->InitialSpeed = 40.0f;
			Offender->ProjectileMovement->MaxSpeed = 40.0f;
		}
		return Offender;
	};
	AWoTTestSeeker* FirstOffender = SpawnIncoming();
	if (!TestNotNull(TEXT("first real incoming projectile"), FirstOffender))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	SisterController->PulseDefensiveForP1C();
	UWoTShieldReflector* InstalledShield = nullptr;
	for (int32 Frame = 0; Frame < 180 && !InstalledShield; ++Frame)
	{
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
		InstalledShield = Sister->FindComponentByClass<UWoTShieldReflector>();
	}
	if (!TestNotNull(TEXT("controller selects and casts the real personal shield"),
		InstalledShield))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FirstOffender->Destroy();
	InstalledShield->ShieldHitPoints = 25;
	for (int32 Frame = 0; Frame < 120 && Caster->IsCasting(); ++Frame)
	{
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
	}
	WoTTest::TickWorld(World, 0.25f);
	const int32 ChargesBeforeSecondDetection = ShieldItem->CurCharges;
	AWoTTestSeeker* SecondOffender = SpawnIncoming();
	TestNotNull(TEXT("second real incoming projectile"), SecondOffender);
	AActor* DetectedSecond = Caster->FindIncomingWeave();
	TestEqual(TEXT("the real defensive detector sees the second projectile"),
		DetectedSecond, static_cast<AActor*>(SecondOffender));
	TestNull(TEXT("the adapted personal shield is excluded from second selection"),
		Caster->SelectDefensiveWeave(DetectedSecond));
	SisterController->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	SisterController->PulseDefensiveForP1C();
	WoTTest::TickWorld(World, 0.25f, 1.0f / 60.0f);
	TestEqual(TEXT("second defensive detection does not spend another shield charge"),
		ShieldItem->CurCharges, ChargesBeforeSecondDetection);
	TestEqual(TEXT("second defensive detection keeps the worn reflector"),
		Sister->FindComponentByClass<UWoTShieldReflector>(), InstalledShield);
	TestEqual(TEXT("second defensive detection preserves the worn hit-point pool"),
		InstalledShield->ShieldHitPoints, 25);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CSisterAdaptedOffenseSpec,
	"WoT.Parity.P1C.P1AReview.SisterExcludesAdaptedOffensiveWeave", P1A_Flags)

bool FWoTP1CSisterAdaptedOffenseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTNPCCharacter* Sister = WoTCreatureData::SpawnCreature(*World,
		TEXT("Sister"), FVector(0, 0, 100), 1, true);
	UWoTCasterComponent* Caster = Sister
		? Sister->FindComponentByClass<UWoTCasterComponent>() : nullptr;
	AWoTAngrealInvDart* Dart = Sister ? Cast<AWoTAngrealInvDart>(
		Sister->InventoryComponent->FindInventoryType(
			AWoTAngrealInvDart::StaticClass())) : nullptr;
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	if (TestNotNull(TEXT("real Sister caster adapter"), Caster)
		&& TestNotNull(TEXT("real Sister offensive Dart"), Dart)
		&& TestNotNull(TEXT("real offensive victim"), Victim))
	{
		for (AWoTInventory* Item : Sister->InventoryComponent->Items)
		{
			if (AWoTAngrealInventory* Weave = Cast<AWoTAngrealInventory>(Item))
			{
				Weave->bOffensive = Weave == Dart;
			}
		}
		Dart->MinChargeGroupInterval = 0.0f;
		Dart->RoundsPerMinute = 6000.0f;
		Caster->MinOffensiveUsageInterval = 0.0f;
		WoTTest::TickWorld(World, 0.1f);
		AWoTAngrealInventory* InitialSelection =
			Caster->SelectOffensiveWeave(Victim);
		TestEqual(TEXT("real offensive selection initially chooses Dart"),
			InitialSelection, static_cast<AWoTAngrealInventory*>(Dart));
		Caster->BeginCast(InitialSelection);
		Caster->Release();
		WoTTest::TickWorld(World, 0.1f);
		TestNull(TEXT("button release preserves and excludes the adapted offensive weave"),
			Caster->SelectOffensiveWeave(Victim));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1ACommonChargeLeechSpec,
	"WoT.Parity.P1A.Decay.CommonDrainsEachArtifactOnce", P1A_Flags)

bool FWoTP1ACommonChargeLeechSpec::RunTest(const FString&)
{
	P1A_VerifyChargeLeech(*this, UWoTDecreaseCommonChargesLeech::StaticClass(),
		{AWoTAngrealInvFireball::StaticClass(), AWoTAngrealInvDart::StaticClass(),
		 AWoTAngrealInvShield::StaticClass()});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AUncommonChargeLeechSpec,
	"WoT.Parity.P1A.Decay.UncommonDrainsEachArtifactOnce", P1A_Flags)

bool FWoTP1AUncommonChargeLeechSpec::RunTest(const FString&)
{
	P1A_VerifyChargeLeech(*this, UWoTDecreaseUncommonChargesLeech::StaticClass(),
		{AWoTAngrealInvEarthTremor::StaticClass(), AWoTAngrealInvLevitate::StaticClass(),
		 AWoTAngrealInvLightning::StaticClass()});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1ARareChargeLeechSpec,
	"WoT.Parity.P1A.Decay.RareDrainsEachArtifactOnce", P1A_Flags)

bool FWoTP1ARareChargeLeechSpec::RunTest(const FString&)
{
	P1A_VerifyChargeLeech(*this, UWoTDecreaseRareChargesLeech::StaticClass(),
		{AWoTAngrealInvDisguise::StaticClass(), AWoTAngrealInvRemoveCurse::StaticClass(),
		 AWoTAngrealInvFireShield::StaticClass()});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AImpactNoiseSpec,
	"WoT.Parity.P1A.GenericProjectile.PawnImpactMakesNoise", P1A_Flags)

bool FWoTP1AImpactNoiseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(-500, 0, 100), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTTestNPCCharacter* Victim = World->SpawnActor<AWoTTestNPCCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Victim->AssignedTeam = 2;
	Victim->Health = 100;
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTTestNPCCharacter* Listener = World->SpawnActor<AWoTTestNPCCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Listener->AssignedTeam = 1;
	Listener->Health = 100;
	Listener->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 0.05f);
	AWoTNPCController* ListenerController = Cast<AWoTNPCController>(Listener->GetController());
	if (!TestNotNull(TEXT("real listening NPC controller"), ListenerController))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	if (UWoTTestBrainComponent* Brain = Cast<UWoTTestBrainComponent>(Listener->Brain))
	{
		Brain->bForceVisible = false;
	}
	AWoTAngrealInvFireball* Fireball =
		P1A_GiveHeld<AWoTAngrealInvFireball>(*World, Caster, 10);
	Caster->InventoryComponent->SelectItem(Fireball);
	Caster->SetActorRotation(FRotator::ZeroRotator);
	Caster->InventoryComponent->UseAngreal();
	Listener->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
	const int32 HealthBefore = Victim->Health;
	for (int32 Frame = 0; Frame < 240 && Victim->Health == HealthBefore; ++Frame)
	{
		WoTTest::TickWorld(World, 1, 1.0f / 120.0f);
	}
	TestTrue(TEXT("the real Fireball reaches and damages the victim"),
		Victim->Health < HealthBefore);
	TestTrue(TEXT("the nearby NPC acquires the struck pawn from impact noise"),
		Listener->Brain->IsGoalValid(EWoTGoalIndex::Threat));
	TestEqual(TEXT("the impact stimulus identifies the struck pawn as noisemaker"),
		Listener->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
		static_cast<AActor*>(Victim));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1ALightningEndpointSpec,
	"WoT.Parity.P1C.P1AReview.LightningEndpointPlacement", P1A_Flags)

bool FWoTP1ALightningEndpointSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(150, 0, 0), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Caster->Health = 100;
	Victim->Health = 100;
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvLightning* Lightning =
		P1A_GiveHeld<AWoTAngrealInvLightning>(*World, Caster, 20);
	UWoTAutoTargetReflector* AutoTarget = NewObject<UWoTAutoTargetReflector>(Caster);
	AutoTarget->Install(Caster);
	Lightning->CastArtifact();
	for (int32 Frame = 0; Frame < 120 && !Lightning->Bolts.Contains(Victim); ++Frame)
	{
		WoTTest::TickWorld(World, 1, 1.0f / 120.0f);
	}
	AWoTParticleSprayer* Sparks = nullptr;
	AWoTMeshEffect* Impact = nullptr;
	AWoTBeamEffect* Streak = nullptr;
	for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
	{
		if (It->ConfigName == TEXT("LightningSparks")) Sparks = *It;
	}
	for (TActorIterator<AWoTMeshEffect> It(World); It; ++It)
	{
		if (It->ConfigName == TEXT("LSImpact")) Impact = *It;
	}
	for (TActorIterator<AWoTBeamEffect> It(World); It; ++It)
	{
		if (It->Config == TEXT("LightningStreak02")) Streak = *It;
	}
	if (TestNotNull(TEXT("real Lightning cast creates the path streak"), Streak)
		&& TestNotNull(TEXT("real Lightning cast creates endpoint sparks"), Sparks)
		&& TestNotNull(TEXT("real Lightning cast creates LSImpact"), Impact))
	{
		float Radius = 0.0f, HalfHeight = 0.0f;
		Victim->GetSimpleCollisionCylinder(Radius, HalfHeight);
		const FVector Offset = Sparks->GetActorLocation() - Victim->GetActorLocation();
		TestTrue(FString::Printf(TEXT("sparks sit on the victim cylinder (offset %.2f radius %.2f)"),
			Offset.Size2D(), Radius), FMath::IsNearlyEqual(Offset.Size2D(), Radius, 2.0f));
		TestTrue(TEXT("the endpoint is on the near side toward the caster"),
			FVector::DotProduct(Offset.GetSafeNormal2D(),
				(Caster->GetActorLocation() - Victim->GetActorLocation()).GetSafeNormal2D())
				> 0.98f);
		TestTrue(TEXT("LSImpact remains at the victim center"),
			Impact->GetActorLocation().Equals(Victim->GetActorLocation(), 1.0f));
		const FVector ExpectedStreakMidpoint =
			(Caster->GetActorLocation() + Sparks->GetActorLocation()) * 0.5f;
		TestTrue(TEXT("the streak terminates with the sparks at the cylinder edge"),
			Streak->GetActorLocation().Equals(ExpectedStreakMidpoint, 2.0f));
	}
	Lightning->UnCastArtifact();
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1ANetworkFaderSpec,
	"WoT.Parity.P1A.Fader.NetworkProjectilesStayLitThenPop", P1A_Flags)

bool FWoTP1ANetworkFaderSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds;
	Worlds.Server = P1A_CreateListenServer();
	if (!TestTrue(TEXT("real listen server loads"), Worlds.Server.IsLoaded())) return false;
	UWorld* World = Worlds.Server.GetWorld();
	P1A_SpawnWall(*World, FVector(200, 0, 100));
	AWoTMyrddraalBolt* Bolt = World->SpawnActor<AWoTMyrddraalBolt>(
		FVector(0, 0, 100), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	if (!TestNotNull(TEXT("real network Myrddraal bolt"), Bolt)) return false;
	TestTrue(TEXT("the bolt reaches the real wall and sticks"),
		Worlds.TickUntil([&]()
		{
			return IsValid(Bolt) && Bolt->ProjectileMovement
				&& !Bolt->ProjectileMovement->IsActive();
		}, 120));
	Bolt->Tick(Bolt->StayStuckTime + 0.5f * Bolt->FadeAwayTime);
	TestTrue(TEXT("network Fader retains full authored glow during the would-be fade"),
		FMath::IsNearlyEqual(Bolt->ScaleGlow, Bolt->FadeStartGlow));
	TWeakObjectPtr<AWoTMyrddraalBolt> BoltPtr = Bolt;
	Bolt->Tick(0.6f * Bolt->FadeAwayTime);
	TestFalse(TEXT("network Fader pops after StayStuckTime plus FadeAwayTime"),
		BoltPtr.IsValid());
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1ABloodPrimeSpec,
	"WoT.Parity.P1A.BloodSpray.AuthoredPrimeCount", P1A_Flags)

bool FWoTP1ABloodPrimeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCarcass* Carcass = World->SpawnActor<AWoTCarcass>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	const int32 SprayersBefore = P1A_CountConfig<AWoTParticleSprayer>(World, NAME_None);
	FDamageEvent Damage;
	Carcass->TakeDamage(1.0f, Damage, nullptr, Carcass);
	AWoTParticleSprayer* Spray = nullptr;
	for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
	{
		if (FMath::IsNearlyEqual(It->PrimeCount, 3.0f)) Spray = *It;
	}
	if (TestNotNull(TEXT("real corpse damage creates BloodSpray02-derived spray"), Spray))
	{
		TestTrue(TEXT("the authored three-tick prime emits particles before first render"),
			Spray->GetLiveParticleCountForTesting() > 0);
	}
	(void)SprayersBefore;
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1ANoCastSelectionSpec,
	"WoT.Parity.P1A.Caster.NoCastBlocksSelection", P1A_Flags)

bool FWoTP1ANoCastSelectionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestCasterNPC* NPC = P1A_SpawnCaster(*World, FVector::ZeroVector);
	// Advance the fresh artifact cooldown before a target exists, so the NPC's
	// autonomous caster cannot consume the test weave during setup.
	WoTTest::TickWorld(World, 0.25f);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 0), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvDart* Dart = P1A_GiveHeld<AWoTAngrealInvDart>(*World, NPC, 50);
	Dart->Priority = 100.0f;
	Dart->InitChargeGroup();
	UWoTNoCastReflector* NoCast = NewObject<UWoTNoCastReflector>(NPC);
	NoCast->Install(NPC);
	TestNull(TEXT("a real caster inside anti-magic selects no offensive weave"),
		NPC->Caster->SelectOffensiveWeave(Victim));
	NoCast->Uninstall();
	WoTTest::TickWorld(World, 1, 1.0f / 120.0f);
	TestEqual(TEXT("selection resumes immediately when the anti-magic seal leaves"),
		NPC->Caster->SelectOffensiveWeave(Victim),
		static_cast<AWoTAngrealInventory*>(Dart));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AWeaponTeleporterSpec,
	"WoT.Parity.P1A.WeaponProjectile.Teleporter", P1A_Flags)

bool FWoTP1AWeaponTeleporterSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTeleporter* Entry = World->SpawnActor<AWoTTeleporter>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	AWoTTeleporter* Exit = World->SpawnActor<AWoTTeleporter>(
		FVector(1000, 0, 100), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Entry->URL = TEXT("p1a_weapon_exit");
	Exit->TeleTag = TEXT("p1a_weapon_exit");
	AWoTArcherArrow* Arrow = World->SpawnActor<AWoTArcherArrow>(
		FVector(-500, 0, 100), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	if (!TestNotNull(TEXT("real ArcherArrow projectile"), Arrow))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Arrow->ProjectileMovement->Deactivate();
	Arrow->SetActorEnableCollision(false);
	Arrow->SetActorLocation(Entry->GetActorLocation(), false, nullptr,
		ETeleportType::TeleportPhysics);
	TWeakObjectPtr<AWoTArcherArrow> ArrowPtr = Arrow;
	Entry->NotifyActorBeginOverlap(Arrow);
	TestTrue(TEXT("the real teleporter accepts a WotWeaponProjectile"),
		ArrowPtr.IsValid()
		&& ArrowPtr->GetActorLocation().Equals(Exit->GetActorLocation(), 2.0f));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1ACasterKilledTargetSpec,
	"WoT.Parity.P1A.Caster.KilledTargetStopsVolley", P1A_Flags)

bool FWoTP1ACasterKilledTargetSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestCasterNPC* NPC = P1A_SpawnCaster(*World, FVector::ZeroVector);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 0), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvDart* Dart = P1A_GiveHeld<AWoTAngrealInvDart>(*World, NPC, 50);
	Dart->SetVictim(Victim);
	Dart->InitChargeGroup();
	NPC->Caster->BeginCast(Dart);
	if (!TestTrue(TEXT("real caster begins a sustained Dart volley"),
		NPC->Caster->IsCasting()))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FWoTDamageEvent Fatal;
	Fatal.WoTDamageType = TEXT("P1AKill");
	Victim->TakeDamage(1000.0f, Fatal, NPC->GetController(), NPC);
	TestFalse(TEXT("the real death dispatch stops every volley targeting the victim"),
		NPC->Caster->IsCasting());
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AVolcanoAttributionSpec,
	"WoT.Parity.P1A.Legion.VolcanoSelfAttributionAndOwnerDamage", P1A_Flags)

bool FWoTP1AVolcanoAttributionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTPlayerController* VictimPC = World->SpawnActor<AWoTPlayerController>(
		P1A_AlwaysSpawn());
	VictimPC->InitPlayerState();
	if (Game) Game->ChangeTeam(VictimPC, 255);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Victim->Health = 5;
	VictimPC->Possess(Victim);
	FActorSpawnParameters VictimOwned = P1A_AlwaysSpawn();
	VictimOwned.Owner = Victim;
	VictimOwned.Instigator = Victim;
	World->SpawnActor<AWoTLegionStompVolcano>(AWoTLegionStompVolcano::StaticClass(),
		Victim->GetActorLocation(), FRotator::ZeroRotator, VictimOwned);
	AWoTPlayerState* VictimState = VictimPC->GetPlayerState<AWoTPlayerState>();
	TestTrue(TEXT("target-owned volcano damage is scored as the target's suicide"),
		VictimState && VictimState->Suicides == 1);

	AWoTTestNPCCharacter* Legion = World->SpawnActor<AWoTTestNPCCharacter>(
		FVector(1000, 0, 100), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Legion->Health = 100;
	Legion->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	FActorSpawnParameters LegionOwned = P1A_AlwaysSpawn();
	LegionOwned.Owner = Legion;
	LegionOwned.Instigator = Legion;
	World->SpawnActor<AWoTLegionStompVolcano>(AWoTLegionStompVolcano::StaticClass(),
		Legion->GetActorLocation(), FRotator::ZeroRotator, LegionOwned);
	TestEqual(TEXT("Touch does not exempt a Legion that is inside a volcano"),
		Legion->Health, 90);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1ALegionWallSilentSpec,
	"WoT.Parity.P1A.LegionProjectile.WallAndNonPawnAreSilent", P1A_Flags)

bool FWoTP1ALegionWallSilentSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AStaticMeshActor* Wall = P1A_SpawnWall(*World, FVector(100, 0, 100));
	const int32 Before = P1A_CountSounds(World, TEXT("WoTPawns/Spirit_HitWall1"));
	AWoTLegionProjectile* TouchSpirit = World->SpawnActor<AWoTLegionProjectile>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	TouchSpirit->ProcessTouch(Wall, Wall->GetActorLocation());
	TestEqual(TEXT("non-pawn ProcessTouch destroys silently"),
		P1A_CountSounds(World, TEXT("WoTPawns/Spirit_HitWall1")), Before);

	AWoTLegionProjectile* WallSpirit = World->SpawnActor<AWoTLegionProjectile>(
		FVector(0, 200, 100), FRotator::ZeroRotator, P1A_AlwaysSpawn());
	WallSpirit->SetDestination(nullptr);
	FHitResult Hit;
	Hit.ImpactPoint = Wall->GetActorLocation();
	Hit.ImpactNormal = FVector(-1, 0, 0);
	WallSpirit->NotifyHit(WallSpirit->Collision.Get(), Wall,
		Cast<UPrimitiveComponent>(Wall->GetRootComponent()), true,
		Hit.ImpactPoint, Hit.ImpactNormal, FVector::ZeroVector, Hit);
	TestEqual(TEXT("straight-flying HitWall destroys silently"),
		P1A_CountSounds(World, TEXT("WoTPawns/Spirit_HitWall1")), Before);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AMashadarNetworkFadeSpec,
	"WoT.Parity.P1A.Mashadar.NetworkSegmentsAppearAtFullGlow", P1A_Flags)

bool FWoTP1AMashadarNetworkFadeSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds;
	Worlds.Server = P1A_CreateListenServer();
	if (!TestTrue(TEXT("real listen server loads"), Worlds.Server.IsLoaded())) return false;
	AWoTMashadarSegment* Segment = Worlds.Server.GetWorld()->SpawnActor<AWoTMashadarSegment>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Worlds.TickAll(1);
	TestTrue(FString::Printf(TEXT("network segment starts at desired glow (%.3f)"),
		Segment ? Segment->GetCurrentScaleGlow() : -1.0f),
		Segment && Segment->GetCurrentScaleGlow() >= 0.2f
			&& Segment->GetCurrentScaleGlow() <= 0.4f);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1AQuestShieldMotionBlurSpec,
	"WoT.Parity.P1C.P1AReview.MotionBlurQuestShieldGhostTrail", P1A_Flags)

bool FWoTP1AQuestShieldMotionBlurSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTQuestShieldProjectile* Shield = World->SpawnActor<AWoTQuestShieldProjectile>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1A_AlwaysSpawn());
	Shield->SetActorEnableCollision(false);
	if (Shield->ProjectileMovement) Shield->ProjectileMovement->Deactivate();
	for (int32 Frame = 0; Frame < 12; ++Frame)
	{
		Shield->SetActorLocation(FVector(Frame * 20.0f, 0, 100));
		Shield->Tick(1.0f / 60.0f);
	}
	TArray<UWoTVertexAnimComponent*> Visuals;
	Shield->GetComponents<UWoTVertexAnimComponent>(Visuals);
	TArray<UWoTVertexAnimComponent*> Ghosts;
	for (UWoTVertexAnimComponent* Visual : Visuals)
	{
		if (Visual && Visual->GetName().StartsWith(TEXT("MotionBlurGhost")))
		{
			Ghosts.Add(Visual);
		}
	}
	Ghosts.Sort([](const UWoTVertexAnimComponent& A,
		const UWoTVertexAnimComponent& B)
	{
		return A.GetName() < B.GetName();
	});
	TestEqual(TEXT("MaxItems ten includes the current copy and exactly nine ghosts"),
		Ghosts.Num(), 9);
	for (int32 Index = 0; Index < Ghosts.Num(); ++Index)
	{
		UWoTVertexAnimComponent* Ghost = Ghosts[Index];
		TestEqual(FString::Printf(TEXT("ghost %d uses translucent Style 3"), Index),
			Ghost->Style, uint8(3));
		float Glow = -1.0f;
		if (UMaterialInstanceDynamic* MID =
			Cast<UMaterialInstanceDynamic>(Ghost->GetMaterial(0)))
		{
			MID->GetScalarParameterValue(
				FHashedMaterialParameterInfo(TEXT("MLScaleGlow")), Glow);
		}
		const float ExpectedGlow = 0.9f - 0.1f * Index;
		TestTrue(FString::Printf(TEXT("ghost %d glow is %.1f"),
			Index, ExpectedGlow), FMath::IsNearlyEqual(Glow, ExpectedGlow, 0.001f));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
