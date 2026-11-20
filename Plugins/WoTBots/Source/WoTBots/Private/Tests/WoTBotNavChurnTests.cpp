#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotBehaviourFixture.h"
#include "WoTBotNavProbe.h"
#include "WoTBotSoakServer.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/PlatformProcess.h"

namespace WoTBotTest
{
namespace
{
void FlushNavigation(UNavigationSystemV1* Nav)
{
	Nav->Tick(1.f / 30.f);
	// UE 5.8 EnsureBuildCompletion consumes tile results without publishing
	// OnNavMeshTilesUpdated. Drain through the ordinary tick for this probe.
	const double Deadline = FPlatformTime::Seconds() + 5;
	while ((Nav->GetNumRemainingBuildTasks() > 0 || Nav->HasDirtyAreasQueued())
		&& FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(.001f); ++GFrameCounter;
		Nav->Tick(1.f / 30.f);
	}
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotProjectileNavChurnSpec,
	"WoT.Bots.Navigation.Churn.ProjectileMovement", Flags)
void FBotProjectileNavChurnSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	// Concrete source classes observed in the Arena dirty-area probe.
	for (const TCHAR* Name : {TEXT("WoTDartProjectile"), TEXT("WoTSeekerProjectile"),
		TEXT("WoTFireballProjectile"), TEXT("WoTDecayProjectile"), TEXT("WoTTaintProjectile"),
		TEXT("WoTSoulBarbProjectile"), TEXT("WoTSwapPlacesProjectile"), TEXT("WoTIceProjectile"),
		TEXT("WoTEarthTremorProjectile"), TEXT("WoTBATHalberdProjectile"), TEXT("WoTMashadarGuide")})
	{
		for (const TCHAR* Lifecycle : {TEXT("Existing"), TEXT("Spawned"), TEXT("Deferred")})
		{
			Names.Add(FString(Name) + TEXT(".") + Lifecycle);
			Commands.Add(FString(Name) + TEXT("|") + Lifecycle);
		}
	}
}
bool FBotProjectileNavChurnSpec::RunTest(const FString& Parameters)
{
	FString ClassName, Lifecycle; Parameters.Split(TEXT("|"), &ClassName, &Lifecycle);
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	FSoakServer Server(W);
	UClass* Class = FindObject<UClass>(nullptr, *(TEXT("/Script/WoT.") + ClassName));
	if (!TestNotNull(TEXT("concrete projectile class"), Class)) return false;
	auto Spawn = [&]()
	{
		FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.bDeferConstruction = Lifecycle == TEXT("Deferred");
		AActor* Result = W->SpawnActor<AActor>(Class, FVector(-800,0,100), FRotator::ZeroRotator, Params);
		if (Result && Params.bDeferConstruction) Result->FinishSpawning(Result->GetActorTransform());
		return Result;
	};
	AActor* Actor = Lifecycle == TEXT("Existing") ? Spawn() : nullptr;
	UNavigationSystemV1* Nav = BuildNav(W);
	if (!TestNotNull(TEXT("bot navigation exists"), Nav)) return false;
	FlushNavigation(Nav);
	WoTBotSoak::FNavProbe Probe(W);
	if (!Actor) Actor = Spawn();
	if (!TestNotNull(TEXT("projectile spawned"), Actor)) return false;
	USphereComponent* Sphere = Actor->FindComponentByClass<USphereComponent>();
	if (!TestNotNull(TEXT("measured source sphere"), Sphere)) return false;
	TestEqual(TEXT("projectile keeps its vehicle blocking response"), Sphere->GetCollisionResponseToChannel(ECC_Vehicle), ECR_Block);
	TestEqual(TEXT("projectile keeps its pawn overlap response"), Sphere->GetCollisionResponseToChannel(ECC_Pawn), ECR_Overlap);
	TestTrue(TEXT("gameplay collision remains enabled"), Sphere->IsQueryCollisionEnabled());
	for (int32 Step = 0; Step < 24; ++Step)
	{
		Actor->SetActorLocation(FVector(-800 + Step * 70, 0, 100));
		FlushNavigation(Nav);
	}
	TestTrue(TEXT("actor traversed the mesh"), Actor->GetActorLocation().X > 700);
	Actor->Destroy(); FlushNavigation(Nav);
	AddInfo(FString::Printf(TEXT("NAV_SOURCE class=%s dirty_areas=%d tile_updates=%d"), *ClassName, Probe.Areas, Probe.Tiles));
	TestEqual(TEXT("spawn, movement and destruction never dirty navigation"), Probe.Areas, 0);
	TestEqual(TEXT("projectile lifecycle never rebuilds a bot tile"), Probe.Tiles, 0);
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotNonSourceNavControlSpec,
	"WoT.Bots.Navigation.Churn.AlreadyIrrelevant", Flags)
void FBotNonSourceNavControlSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : {TEXT("WoTCharacter"), TEXT("WoTCarcass"), TEXT("WoTFragment"),
		TEXT("WoTAngrealInvDart"), TEXT("WoTParticleSprayer"), TEXT("WoTDecoration")})
	{
		Names.Add(Name); Commands.Add(Name);
	}
	Names.Add(TEXT("TossedInventory")); Commands.Add(TEXT("WoTAngrealInvDart:Tossed"));
}
bool FBotNonSourceNavControlSpec::RunTest(const FString& Parameters)
{
	FString ClassName = Parameters, Mode; Parameters.Split(TEXT(":"), &ClassName, &Mode);
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	FSoakServer Server(W);
	UNavigationSystemV1* Nav = BuildNav(W);
	if (!TestNotNull(TEXT("navigation built"), Nav)) return false;
	FlushNavigation(Nav);
	UClass* Class = FindObject<UClass>(nullptr, *(TEXT("/Script/WoT.") + ClassName));
	if (!TestNotNull(TEXT("source candidate class"), Class)) return false;
	WoTBotSoak::FNavProbe Probe(W);
	AActor* Actor = W->SpawnActor<AActor>(Class, FVector(-500,0,100), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("candidate spawned"), Actor)) return false;
	if (Mode == TEXT("Tossed")) CastChecked<AWoTInventory>(Actor)->DropFrom(Actor->GetActorLocation());
	for (int32 Step = 0; Step < 20; ++Step)
	{
		Actor->SetActorLocation(FVector(-500 + Step * 50, 0, 100));
		FlushNavigation(Nav);
	}
	TestTrue(TEXT("candidate moved through navigation bounds"), Actor->GetActorLocation().X > 400);
	Actor->Destroy(); FlushNavigation(Nav);
	TestEqual(TEXT("candidate lifecycle has no dirty areas"), Probe.Areas, 0);
	TestEqual(TEXT("candidate lifecycle has no tile updates"), Probe.Tiles, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotNoNavigationControlSpec,
	"WoT.Bots.Navigation.Churn.BotsOffDoesNotCreateNavigation", Flags)
bool FBotNoNavigationControlSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	TestNull(TEXT("bots-off match begins without navigation"), W->GetNavigationSystem());
	UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTDartProjectile"));
	if (!TestNotNull(TEXT("projectile class"), Class)) return false;
	AActor* Actor = W->SpawnActor<AActor>(Class, FVector(-500,0,100), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("projectile spawned"), Actor)) return false;
	WoTTest::TickWorld(W, .2f);
	TestNull(TEXT("projectile activity does not create a navigation system"), W->GetNavigationSystem());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotMoverNavControlSpec,
	"WoT.Bots.Navigation.Churn.MoverStillChangesRoutes", Flags)
bool FBotMoverNavControlSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	// A real mover blocks the only corridor; its geometry must remain relevant.
	Box(W, FVector(0,-1100,150), FVector(100,1000,150));
	Box(W, FVector(0,1100,150), FVector(100,1000,150));
	UClass* MoverClass = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTMover"));
	if (!TestNotNull(TEXT("mover class"), MoverClass)) return false;
	AActor* Mover = W->SpawnActor<AActor>(MoverClass, FVector(0,0,100), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("real mover"), Mover)) return false;
	UStaticMeshComponent* Mesh = Mover->FindComponentByClass<UStaticMeshComponent>();
	if (!TestNotNull(TEXT("mover collision mesh"), Mesh)) return false;
	Mover->SetActorTickEnabled(false);
	Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	Mover->SetActorScale3D(FVector(1,2,3));
	UNavigationSystemV1* Nav = BuildNav(W);
	if (!TestNotNull(TEXT("navigation built"), Nav)) return false;
	FlushNavigation(Nav);
	const ANavigationData* Data = Nav->GetNavDataForAgentName(TEXT("ArenaBot"));
	if (!TestNotNull(TEXT("bot mesh built"), Data)) return false;
	auto Crosses = [&]()
	{
		FPathFindingQuery Query(nullptr, *Data, FVector(-500,0,0), FVector(500,0,0));
		Query.SetAllowPartialPaths(false);
		return Nav->FindPathSync(Data->GetConfig(), Query).IsSuccessful();
	};
	TestFalse(TEXT("closed mover blocks the corridor"), Crosses());
	WoTBotSoak::FNavProbe Probe(W);
	Mover->SetActorLocation(FVector(0,0,500)); FlushNavigation(Nav);
	TestTrue(TEXT("opening mover rebuilds navigation"), Probe.Areas > 0 && Probe.Tiles > 0);
	TestTrue(TEXT("open mover permits the route"), Crosses());
	Mover->SetActorLocation(FVector(0,0,100)); FlushNavigation(Nav);
	TestFalse(TEXT("closing mover blocks the route again"), Crosses());
	TestEqual(TEXT("mover still blocks players"), Mesh->GetCollisionResponseToChannel(ECC_Pawn), ECR_Block);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotPluginOffNavControlSpec,
	"WoT.Bots.Navigation.Churn.PluginOffPreservesSourceRelevance", Flags)
bool FBotPluginOffNavControlSpec::RunTest(const FString&)
{
	WoTBotNavigation::Unregister(); ON_SCOPE_EXIT { WoTBotNavigation::Register(); };
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	UNavigationSystemV1* Nav = BuildNav(W);
	if (!TestNotNull(TEXT("ordinary navigation exists"), Nav)) return false;
	TestFalse(TEXT("plugin-off world uses ordinary navigation"), Nav->IsA<UWoTBotNavigationSystem>());
	UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTDartProjectile"));
	if (!TestNotNull(TEXT("projectile class"), Class)) return false;
	AActor* Actor = W->SpawnActor<AActor>(Class, FVector(-500,0,100), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("projectile spawned"), Actor)) return false;
	const USphereComponent* Sphere = Actor->FindComponentByClass<USphereComponent>();
	if (!TestNotNull(TEXT("projectile sphere"), Sphere)) return false;
	TestTrue(TEXT("plugin-off relevance is unchanged"), Sphere->CanEverAffectNavigation() && Sphere->IsNavigationRelevant());
	return !HasAnyErrors();
}
}
#endif
