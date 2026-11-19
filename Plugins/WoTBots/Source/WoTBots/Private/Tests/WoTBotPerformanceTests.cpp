#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS
#include "WoTBotBehaviourFixture.h"
#include "WoTBotMetrics.h"
namespace WoTBotTest
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotRosterCostSpec, "WoT.Bots.Measurements.FifteenBotsGameThread", Flags)
bool FBotRosterCostSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTBotMetrics::bEnabled = false; WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	Pool(W, FVector(-2300, 1000, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	Pool(W, FVector(2300, -1000, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	double Baseline = 0;
	for (int32 I = 0; I < 300; ++I)
	{
		const double Begin = FPlatformTime::Seconds();
		WoTTest::TickWorld(W, 1.0f / 60.0f);
		Baseline += FPlatformTime::Seconds() - Begin;
	}
	TArray<AWoTBotController*> Bots;
	for (int32 I = 0; I < 15; ++I)
	{
		AWoTBotController* B = Bot(W, FVector((I % 5) * 800 - 1600, (I / 5) * 800 - 800, 50), I);
		B->GetPawn()->SetCanBeDamaged(false); // Keep fifteen live participants for every sample.
		Bots.Add(B);
	}
	WoTTest::TickWorld(W, 1.0f);
	WoTBotMetrics::bEnabled = true;
	double Total = 0, Worst = 0, WorldTotal = 0, WorldWorst = 0;
	constexpr int32 Frames = 900;
	for (int32 I = 0; I < Frames; ++I)
	{
		WoTBotMetrics::Seconds = 0;
		const double Begin = FPlatformTime::Seconds();
		WoTTest::TickWorld(W, 1.0f / 60.0f);
		const double Frame = FPlatformTime::Seconds() - Begin;
		WorldTotal += Frame; WorldWorst = FMath::Max(WorldWorst, Frame);
		Total += WoTBotMetrics::Seconds; Worst = FMath::Max(Worst, WoTBotMetrics::Seconds);
	}
	for (AWoTBotController* B : Bots) TestTrue(TEXT("all fifteen thought throughout measurement"), B->GetThinkCount() >= 140);
	AddInfo(FString::Printf(TEXT("BOT_COST frames=%d hz=60 controllers=15 behaviour_mean_ms=%.6f behaviour_max_ms=%.6f world_mean_ms=%.6f world_max_ms=%.6f empty_world_mean_ms=%.6f incremental_world_mean_ms=%.6f incremental_world_max_estimate_ms=%.6f"),
		Frames, Total * 1000 / Frames, Worst * 1000, WorldTotal * 1000 / Frames,
		WorldWorst * 1000, Baseline * 1000 / 300, (WorldTotal / Frames - Baseline / 300) * 1000,
		(WorldWorst - Baseline / 300) * 1000));
	return !HasAnyErrors();
}
} // namespace WoTBotTest
#endif
