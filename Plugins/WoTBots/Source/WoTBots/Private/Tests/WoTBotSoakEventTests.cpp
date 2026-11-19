#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotBehaviourFixture.h"
#include "Testing/WoTSoakEvents.h"
#include "UObject/UnrealType.h"
#include "WoTBotSoakServer.h"
namespace WoTBotTest
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSoakServerSpec, "WoT.Bots.SoakDiagnostics.ServerSemantics", Flags)
bool FBotSoakServerSpec::RunTest(const FString&)
{
	UWorld* W = World();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	FSoakServer Server(W);
	TestTrue(TEXT("worker runs dedicated gameplay semantics"), W->GetNetMode() == NM_DedicatedServer);
	AWoTInventory* Item = W->SpawnActor<AWoTInventory>(W->GetAuthGameMode<AWoTGameMode>()->DefaultAngreal);
	if (!TestNotNull(TEXT("concrete pickup exists"), Item)) return false;
	Item->RespawnTime = 10;
	TestTrue(TEXT("ordinary Arena pickups replenish"), W->GetAuthGameMode<AWoTGameMode>()->ShouldRespawn(Item));
	WoTTest::TickWorld(W, .1f, 1.0f / 30.0f);
	TestTrue(TEXT("server semantics survive world ticks"), W->GetNetMode() == NM_DedicatedServer);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSoakVelocitySpec, "WoT.Bots.SoakDiagnostics.SimulationVelocity", Flags)
bool FBotSoakVelocitySpec::RunTest(const FString&)
{
	UWorld* W = World();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	UClass* FragmentClass = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTFragment"));
	AActor* Fragment = W->SpawnActor<AActor>(FragmentClass, FVector(0,0,100), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("fragment exists"), Fragment)) return false;
	FStructProperty* Property = FindFProperty<FStructProperty>(FragmentClass, TEXT("Velocity"));
	if (!TestNotNull(TEXT("fragment simulation velocity"), Property)) return false;
	FVector& Velocity = *Property->ContainerPtrToValuePtr<FVector>(Fragment);
	Velocity = FVector(1000000,0,0);
	TestEqual(TEXT("custom movement is sampled even when root velocity is zero"), WoTSoakEvents::SimulationVelocity(Fragment), Velocity);
	Velocity = FVector(std::numeric_limits<double>::infinity(),0,0);
	TestFalse(TEXT("non-finite custom movement remains detectable"), FMath::IsFinite(WoTSoakEvents::SimulationVelocity(Fragment).X));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSoakEventsSpec, "WoT.Bots.SoakDiagnostics.SynchronousEvents", Flags)
bool FBotSoakEventsSpec::RunTest(const FString&)
{
	UWorld* W = World();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	TMap<WoTSoakEvents::EKind, int32> Counts;
	const FDelegateHandle Handle = WoTSoakEvents::Events().AddLambda(
		[&](WoTSoakEvents::EKind Kind, AActor* Subject, AActor*)
		{ if (Subject && Subject->GetWorld() == W) ++Counts.FindOrAdd(Kind); });
	ON_SCOPE_EXIT { WoTSoakEvents::Events().Remove(Handle); };
	AWoTAngrealInventory* Item = Kit(B);
	TestNotNull(TEXT("ordinary starter artifact"), Item);
	if (!Item) return false;
	Item->UseCharge();
	TestEqual(TEXT("charge use captured synchronously"), Counts.FindRef(WoTSoakEvents::EKind::Cast), 1);
	AWoTAngrealSpawnPool* P = Pool(W, FVector(500, 0, 50), TEXT("/Script/WoT.WoTAngrealInvDart"));
	TestEqual(TEXT("initial stock captured before next tick"), Counts.FindRef(WoTSoakEvents::EKind::PoolStock), 1);
	P->OnPickupOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
	TestEqual(TEXT("pool collection captured even when destroyed"), Counts.FindRef(WoTSoakEvents::EKind::PoolCollect), 1);
	TestEqual(TEXT("duplicate charge pickup counted once"), Counts.FindRef(WoTSoakEvents::EKind::Pickup), 1);
	B->SilentRespawn();
	TestEqual(TEXT("silent respawn captured before pawn destruction"), Counts.FindRef(WoTSoakEvents::EKind::SilentRespawn), 1);
	return !HasAnyErrors();
}
} // namespace WoTBotTest
#endif

