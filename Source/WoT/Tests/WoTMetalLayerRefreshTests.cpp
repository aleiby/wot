#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTMetalLayerRefresh.h"

#include <initializer_list>
#include <limits>

namespace
{
constexpr EAutomationTestFlags WoTMetalLayerRefreshSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

using WoTMetalLayerRefreshSpecReason = EWoTMetalLayerRefreshReason;
using WoTMetalLayerRefreshSpecResult = EWoTMetalLayerRefreshResult;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshFirstSizeSpec,
	"WoT.MetalLayerRefresh.FirstSizeIsBaseline", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshFirstSizeSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	TestEqual(TEXT("first size is only a baseline"), Policy.Tick(0.0, 0.0, {800, 600}, true),
		WoTMetalLayerRefreshSpecReason::None);
	TestEqual(TEXT("an unchanged baseline never needs a resize refresh"),
		Policy.Tick(100.0, 0.01, {800, 600}, true), WoTMetalLayerRefreshSpecReason::None);
	TestFalse(TEXT("no request is pending"), Policy.IsRequestPending());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshZeroSizeSpec,
	"WoT.MetalLayerRefresh.ZeroSizeIgnored", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshZeroSizeSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	const FIntPoint InvalidSizes[] = {{0, 600}, {800, 0}, {-1, 600}, {800, -1}};
	double NowSeconds = 0.0;
	for (const FIntPoint InvalidSize : InvalidSizes)
	{
		TestEqual(TEXT("invalid initial size is ignored"),
			Policy.Tick(NowSeconds++, 0.01, InvalidSize, false), WoTMetalLayerRefreshSpecReason::None);
	}
	Policy.Tick(NowSeconds++, 0.01, {800, 600}, false);
	for (const FIntPoint InvalidSize : InvalidSizes)
	{
		TestEqual(TEXT("invalid size does not change the baseline"),
			Policy.Tick(NowSeconds++, 0.01, InvalidSize, false), WoTMetalLayerRefreshSpecReason::None);
	}
	TestEqual(TEXT("returning to the valid size is not a resize"),
		Policy.Tick(NowSeconds++, 0.01, {800, 600}, false), WoTMetalLayerRefreshSpecReason::None);
	Policy.Tick(NowSeconds, 0.01, {900, 600}, false);
	Policy.Tick(NowSeconds + 0.125, 0.01, {0, 0}, false);
	TestEqual(TEXT("invalid observations do not restart the settle timer"),
		Policy.Tick(NowSeconds + 0.25, 0.01, {900, 600}, false),
		WoTMetalLayerRefreshSpecReason::ResizeSettled);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshSettleSpec,
	"WoT.MetalLayerRefresh.SettledResizeRequestsOnce", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshSettleSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	Policy.Tick(0.0, 0.0, {800, 600}, false);
	TestEqual(TEXT("a changed size must settle"), Policy.Tick(1.0, 0.01, {900, 600}, false),
		WoTMetalLayerRefreshSpecReason::None);
	TestEqual(TEXT("before the settle boundary"), Policy.Tick(1.249, 0.01, {900, 600}, false),
		WoTMetalLayerRefreshSpecReason::None);
	TestEqual(TEXT("at the settle boundary"), Policy.Tick(1.25, 0.01, {900, 600}, false),
		WoTMetalLayerRefreshSpecReason::ResizeSettled);
	TestTrue(TEXT("the request remains pending"), Policy.IsRequestPending());
	TestEqual(TEXT("no repeat without an answer"), Policy.Tick(2.0, 0.01, {900, 600}, false),
		WoTMetalLayerRefreshSpecReason::None);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshMovingSizeSpec,
	"WoT.MetalLayerRefresh.MovingSizeNeverSettles", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshMovingSizeSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	Policy.Tick(0.0, 0.0, {800, 600}, true);
	for (int32 StepIndex = 1; StepIndex <= 20; ++StepIndex)
	{
		TestEqual(TEXT("each change restarts settling and suppresses stall recovery"),
			Policy.Tick(static_cast<double>(StepIndex), 1.0, {800 + StepIndex, 600}, true),
			WoTMetalLayerRefreshSpecReason::None);
	}
	TestEqual(TEXT("final size can settle"), Policy.Tick(20.25, 0.25, {820, 600}, true),
		WoTMetalLayerRefreshSpecReason::ResizeSettled);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshRoundTripSpec,
	"WoT.MetalLayerRefresh.ReturnToOriginalSizeStillRefreshes", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshRoundTripSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	Policy.Tick(0.0, 0.0, {800, 600}, false);
	Policy.Tick(1.0, 0.01, {900, 600}, false);
	Policy.Tick(1.125, 0.01, {800, 600}, false);
	TestEqual(TEXT("returning to the original size restarts settling"),
		Policy.Tick(1.25, 0.01, {800, 600}, false), WoTMetalLayerRefreshSpecReason::None);
	TestEqual(TEXT("the intervening resize still needs recovery"),
		Policy.Tick(1.375, 0.01, {800, 600}, false), WoTMetalLayerRefreshSpecReason::ResizeSettled);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshPendingSpec,
	"WoT.MetalLayerRefresh.PendingRequestBlocksRepeats", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshPendingSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	Policy.Tick(0.0, 0.0, {800, 600}, true);
	Policy.Tick(1.0, 0.01, {900, 600}, true);
	Policy.Tick(1.25, 0.01, {900, 600}, true);
	for (int32 StepIndex = 1; StepIndex <= 10; ++StepIndex)
	{
		TestEqual(TEXT("neither elapsed time, a new resize nor a stall duplicates the request"),
			Policy.Tick(1000.0 * StepIndex, 1.0, {1000, 700}, true),
			WoTMetalLayerRefreshSpecReason::None);
	}
	TestTrue(TEXT("there is no request timeout"), Policy.IsRequestPending());
	Policy.OnResult(10000.0, WoTMetalLayerRefreshSpecResult::Deferred);
	TestFalse(TEXT("an answer ends the in-flight request"), Policy.IsRequestPending());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshDeferredSpec,
	"WoT.MetalLayerRefresh.DeferredAnswerRetriesLater", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshDeferredSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	Policy.Tick(0.0, 0.0, {800, 600}, false);
	Policy.Tick(1.0, 0.01, {900, 600}, false);
	Policy.Tick(1.25, 0.01, {900, 600}, false);
	Policy.OnResult(2.0, WoTMetalLayerRefreshSpecResult::Deferred);
	TestEqual(TEXT("retry interval starts at the answer"), Policy.Tick(2.249, 0.01, {900, 600}, false),
		WoTMetalLayerRefreshSpecReason::None);
	TestEqual(TEXT("a settled resize retries at the boundary"), Policy.Tick(2.25, 0.01, {900, 600}, false),
		WoTMetalLayerRefreshSpecReason::ResizeSettled);
	Policy.OnResult(2.25, WoTMetalLayerRefreshSpecResult::Deferred);
	Policy.Tick(2.375, 0.01, {1000, 600}, false);
	TestEqual(TEXT("a new size still needs to settle after retry time passes"),
		Policy.Tick(2.5, 0.01, {1000, 600}, false), WoTMetalLayerRefreshSpecReason::None);
	TestEqual(TEXT("retry follows the new settle boundary"),
		Policy.Tick(2.625, 0.01, {1000, 600}, false), WoTMetalLayerRefreshSpecReason::ResizeSettled);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshDeferredStallSpec,
	"WoT.MetalLayerRefresh.DeferredStallKeepsItsNeed", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshDeferredStallSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	for (int32 StepIndex = 1; StepIndex <= 4; ++StepIndex)
	{
		Policy.Tick(static_cast<double>(StepIndex), 1.0, {800, 600}, true);
	}
	Policy.OnResult(4.0, WoTMetalLayerRefreshSpecResult::Deferred);
	TestEqual(TEXT("fast frames do not discard a deferred need"),
		Policy.Tick(4.125, 0.01, {800, 600}, true), WoTMetalLayerRefreshSpecReason::None);
	TestEqual(TEXT("a deferred stall retries without four more slow frames"),
		Policy.Tick(4.25, 0.01, {800, 600}, true), WoTMetalLayerRefreshSpecReason::FrameStall);
	Policy.OnResult(4.25, WoTMetalLayerRefreshSpecResult::Deferred);
	TestEqual(TEXT("a hidden window cannot retry a stall"),
		Policy.Tick(5.0, 1.0, {800, 600}, false), WoTMetalLayerRefreshSpecReason::None);
	TestEqual(TEXT("the deferred need survives until watching resumes"),
		Policy.Tick(6.0, 0.01, {800, 600}, true), WoTMetalLayerRefreshSpecReason::FrameStall);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshRefreshedSpec,
	"WoT.MetalLayerRefresh.RefreshedAnswerClearsResize", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshRefreshedSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	Policy.Tick(0.0, 0.0, {800, 600}, true);
	Policy.Tick(1.0, 1.0, {900, 600}, true);
	Policy.Tick(1.125, 1.0, {900, 600}, true);
	Policy.Tick(1.25, 1.0, {900, 600}, true);
	Policy.OnResult(1.25, WoTMetalLayerRefreshSpecResult::Refreshed);
	TestFalse(TEXT("refresh answers the pending request"), Policy.IsRequestPending());
	for (int32 StepIndex = 1; StepIndex <= 3; ++StepIndex)
	{
		TestEqual(TEXT("resize and old slow frames were cleared"),
			Policy.Tick(1.25 + StepIndex, 1.0, {900, 600}, true), WoTMetalLayerRefreshSpecReason::None);
	}
	TestEqual(TEXT("resize recovery does not impose a stall cooldown"),
		Policy.Tick(5.25, 1.0, {900, 600}, true), WoTMetalLayerRefreshSpecReason::FrameStall);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshUnavailableSpec,
	"WoT.MetalLayerRefresh.UnavailableAnswerDropsRequest", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshUnavailableSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	Policy.Tick(0.0, 0.0, {800, 600}, false);
	Policy.Tick(1.0, 0.01, {900, 600}, false);
	Policy.Tick(1.25, 0.01, {900, 600}, false);
	Policy.OnResult(1.25, WoTMetalLayerRefreshSpecResult::Unavailable);
	TestFalse(TEXT("unavailable answers the request"), Policy.IsRequestPending());
	TestEqual(TEXT("unavailable drops the resize instead of retrying"),
		Policy.Tick(100.0, 0.01, {900, 600}, false), WoTMetalLayerRefreshSpecReason::None);
	Policy.Tick(101.0, 0.01, {1000, 600}, false);
	TestEqual(TEXT("a later resize can try again"), Policy.Tick(101.25, 0.01, {1000, 600}, false),
		WoTMetalLayerRefreshSpecReason::ResizeSettled);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshConsecutiveSpec,
	"WoT.MetalLayerRefresh.StallNeedsConsecutiveSlowFrames", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshConsecutiveSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	for (int32 StepIndex = 1; StepIndex <= 3; ++StepIndex)
	{
		TestEqual(TEXT("three slow frames are not enough"),
			Policy.Tick(StepIndex * 0.25, 0.25, {800, 600}, true), WoTMetalLayerRefreshSpecReason::None);
	}
	Policy.Tick(1.0, 0.249, {800, 600}, true);
	for (int32 StepIndex = 1; StepIndex <= 3; ++StepIndex)
	{
		TestEqual(TEXT("a faster frame resets the consecutive count"),
			Policy.Tick(1.0 + StepIndex * 0.25, 0.25, {800, 600}, true), WoTMetalLayerRefreshSpecReason::None);
	}
	TestEqual(TEXT("four frames exactly at the slow threshold request recovery"),
		Policy.Tick(2.0, 0.25, {800, 600}, true), WoTMetalLayerRefreshSpecReason::FrameStall);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshCooldownSpec,
	"WoT.MetalLayerRefresh.StallWaitsForCooldown", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshCooldownSpec::RunTest(const FString&)
{
	for (const WoTMetalLayerRefreshSpecResult Answer : {
		WoTMetalLayerRefreshSpecResult::Refreshed, WoTMetalLayerRefreshSpecResult::Unavailable})
	{
		FWoTMetalLayerRefreshPolicy Policy;
		for (int32 StepIndex = 1; StepIndex <= 4; ++StepIndex)
		{
			Policy.Tick(static_cast<double>(StepIndex), 1.0, {800, 600}, true);
		}
		Policy.OnResult(10.0, Answer);
		TestFalse(TEXT("terminal stall answer ends the request"), Policy.IsRequestPending());
		for (int32 StepIndex = 1; StepIndex <= 4; ++StepIndex)
		{
			TestEqual(TEXT("both terminal answers apply the initial cooldown"),
				Policy.Tick(10.0 + StepIndex, 1.0, {800, 600}, true), WoTMetalLayerRefreshSpecReason::None);
		}
		TestEqual(TEXT("cooldown includes its boundary"), Policy.Tick(15.0, 1.0, {800, 600}, true),
			WoTMetalLayerRefreshSpecReason::FrameStall);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshBackoffSpec,
	"WoT.MetalLayerRefresh.StallBackoffDoublesAndCaps", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshBackoffSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshTuning Tuning;
	Tuning.StallFrameCount = 1;
	for (const WoTMetalLayerRefreshSpecResult Answer : {
		WoTMetalLayerRefreshSpecResult::Refreshed, WoTMetalLayerRefreshSpecResult::Unavailable})
	{
		FWoTMetalLayerRefreshPolicy Policy(Tuning);
		double AnswerSeconds = 1.0;
		Policy.Tick(AnswerSeconds, 1.0, {800, 600}, true);
		for (const double ExpectedCooldown : {5.0, 10.0, 20.0, 40.0, 60.0, 60.0})
		{
			Policy.OnResult(AnswerSeconds, Answer);
			TestEqual(TEXT("backoff holds until its deadline"),
				Policy.Tick(AnswerSeconds + ExpectedCooldown - 0.125, 1.0, {800, 600}, true),
				WoTMetalLayerRefreshSpecReason::None);
			AnswerSeconds += ExpectedCooldown;
			TestEqual(TEXT("backoff doubles then stays capped"),
				Policy.Tick(AnswerSeconds, 1.0, {800, 600}, true), WoTMetalLayerRefreshSpecReason::FrameStall);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshHealthySpec,
	"WoT.MetalLayerRefresh.HealthyFramesResetBackoff", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshHealthySpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshTuning Tuning;
	Tuning.StallFrameCount = 1;
	FWoTMetalLayerRefreshPolicy Policy(Tuning);
	Policy.Tick(0.0, 1.0, {800, 600}, true);
	Policy.OnResult(0.0, WoTMetalLayerRefreshSpecResult::Refreshed);
	Policy.Tick(5.0, 1.0, {800, 600}, true);
	Policy.OnResult(5.0, WoTMetalLayerRefreshSpecResult::Refreshed);
	for (int32 StepIndex = 1; StepIndex <= 32; ++StepIndex)
	{
		Policy.Tick(5.0 + StepIndex * 0.0625, 0.0625, {800, 600}, true);
	}
	TestEqual(TEXT("healthy recovery does not shorten a cooldown already in progress"),
		Policy.Tick(14.875, 1.0, {800, 600}, true), WoTMetalLayerRefreshSpecReason::None);
	Policy.Tick(15.0, 1.0, {800, 600}, true);
	Policy.OnResult(15.0, WoTMetalLayerRefreshSpecResult::Refreshed);
	TestEqual(TEXT("the next cooldown uses the initial duration"),
		Policy.Tick(19.875, 1.0, {800, 600}, true), WoTMetalLayerRefreshSpecReason::None);
	TestEqual(TEXT("two seconds of healthy frame time reset backoff"),
		Policy.Tick(20.0, 1.0, {800, 600}, true), WoTMetalLayerRefreshSpecReason::FrameStall);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshInterruptedHealthSpec,
	"WoT.MetalLayerRefresh.HealthyRunMustBeConsecutive", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshInterruptedHealthSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshTuning Tuning;
	Tuning.StallFrameCount = 1;
	FWoTMetalLayerRefreshPolicy Policy(Tuning);
	Policy.Tick(0.0, 1.0, {800, 600}, true);
	Policy.OnResult(0.0, WoTMetalLayerRefreshSpecResult::Refreshed);
	for (int32 StepIndex = 1; StepIndex <= 24; ++StepIndex)
	{
		Policy.Tick(StepIndex * 0.0625, 0.0625, {800, 600}, true);
	}
	Policy.Tick(1.6, 0.1, {800, 600}, true);
	for (int32 StepIndex = 1; StepIndex <= 24; ++StepIndex)
	{
		Policy.Tick(2.0 + StepIndex * 0.0625, 0.0625, {800, 600}, true);
	}
	Policy.Tick(5.0, 1.0, {800, 600}, true);
	Policy.OnResult(5.0, WoTMetalLayerRefreshSpecResult::Refreshed);
	TestEqual(TEXT("exactly the healthy threshold breaks the run"),
		Policy.Tick(10.0, 1.0, {800, 600}, true), WoTMetalLayerRefreshSpecReason::None);
	TestEqual(TEXT("separate healthy runs do not reset the doubled cooldown"),
		Policy.Tick(15.0, 1.0, {800, 600}, true), WoTMetalLayerRefreshSpecReason::FrameStall);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshNotWatchedSpec,
	"WoT.MetalLayerRefresh.StallIgnoredWhenNotWatched", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshNotWatchedSpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	for (int32 StepIndex = 1; StepIndex <= 3; ++StepIndex)
	{
		Policy.Tick(static_cast<double>(StepIndex), 1.0, {800, 600}, true);
	}
	for (int32 StepIndex = 4; StepIndex <= 10; ++StepIndex)
	{
		TestEqual(TEXT("unwatched slow frames never trigger recovery"),
			Policy.Tick(static_cast<double>(StepIndex), 1.0, {800, 600}, false),
			WoTMetalLayerRefreshSpecReason::None);
	}
	for (int32 StepIndex = 11; StepIndex <= 13; ++StepIndex)
	{
		TestEqual(TEXT("watching resumes with a fresh slow-frame count"),
			Policy.Tick(static_cast<double>(StepIndex), 1.0, {800, 600}, true),
			WoTMetalLayerRefreshSpecReason::None);
	}
	TestEqual(TEXT("four new watched frames can trigger recovery"),
		Policy.Tick(14.0, 1.0, {800, 600}, true), WoTMetalLayerRefreshSpecReason::FrameStall);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshPrioritySpec,
	"WoT.MetalLayerRefresh.ResizeWinsOverStall", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshPrioritySpec::RunTest(const FString&)
{
	FWoTMetalLayerRefreshPolicy Policy;
	Policy.Tick(0.0, 0.25, {800, 600}, true);
	Policy.Tick(0.25, 0.25, {800, 600}, true);
	Policy.Tick(0.5, 0.25, {900, 600}, true);
	TestEqual(TEXT("a settled resize wins when both reasons become due"),
		Policy.Tick(0.75, 0.25, {900, 600}, true), WoTMetalLayerRefreshSpecReason::ResizeSettled);
	Policy.OnResult(0.75, WoTMetalLayerRefreshSpecResult::Refreshed);
	for (int32 StepIndex = 1; StepIndex <= 4; ++StepIndex)
	{
		Policy.Tick(0.75 + StepIndex, 1.0, {900, 600}, true);
	}
	Policy.OnResult(4.75, WoTMetalLayerRefreshSpecResult::Refreshed);
	Policy.Tick(5.0, 0.01, {1000, 600}, false);
	TestEqual(TEXT("stall cooldown and watch gating do not delay settled resizes"),
		Policy.Tick(5.25, 0.01, {1000, 600}, false), WoTMetalLayerRefreshSpecReason::ResizeSettled);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshBadTimesSpec,
	"WoT.MetalLayerRefresh.BadFrameTimesCountAsZero", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshBadTimesSpec::RunTest(const FString&)
{
	const double BadTimes[] = {-1.0, std::numeric_limits<double>::quiet_NaN(),
		std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
	for (const double BadTime : BadTimes)
	{
		FWoTMetalLayerRefreshPolicy Policy;
		for (int32 StepIndex = 1; StepIndex <= 3; ++StepIndex)
		{
			Policy.Tick(static_cast<double>(StepIndex), 1.0, {800, 600}, true);
		}
		TestEqual(TEXT("invalid frame time resets the slow count"),
			Policy.Tick(4.0, BadTime, {800, 600}, true), WoTMetalLayerRefreshSpecReason::None);
		for (int32 StepIndex = 5; StepIndex <= 7; ++StepIndex)
		{
			TestEqual(TEXT("slow frames must accumulate again"),
				Policy.Tick(static_cast<double>(StepIndex), 1.0, {800, 600}, true),
				WoTMetalLayerRefreshSpecReason::None);
		}
		TestEqual(TEXT("four valid slow frames still trigger recovery"),
			Policy.Tick(8.0, 1.0, {800, 600}, true), WoTMetalLayerRefreshSpecReason::FrameStall);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMetalLayerRefreshBadHealthyTimesSpec,
	"WoT.MetalLayerRefresh.InvalidTimesPreserveHealthyProgress", WoTMetalLayerRefreshSpecFlags)
bool FWoTMetalLayerRefreshBadHealthyTimesSpec::RunTest(const FString&)
{
	const double BadTimes[] = {-1.0, std::numeric_limits<double>::quiet_NaN(),
		std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
	for (const double BadTime : BadTimes)
	{
		FWoTMetalLayerRefreshTuning Tuning;
		Tuning.StallFrameCount = 1;
		FWoTMetalLayerRefreshPolicy Policy(Tuning);
		Policy.Tick(0.0, 1.0, {800, 600}, true);
		Policy.OnResult(0.0, WoTMetalLayerRefreshSpecResult::Refreshed);
		for (int32 StepIndex = 1; StepIndex <= 31; ++StepIndex)
		{
			Policy.Tick(StepIndex * 0.0625, 0.0625, {800, 600}, true);
		}
		Policy.Tick(2.0, BadTime, {800, 600}, true);
		FWoTMetalLayerRefreshPolicy IncompletePolicy = Policy;
		IncompletePolicy.Tick(5.0, 1.0, {800, 600}, true);
		IncompletePolicy.OnResult(5.0, WoTMetalLayerRefreshSpecResult::Refreshed);
		TestEqual(TEXT("invalid time contributes no healthy duration"),
			IncompletePolicy.Tick(10.0, 1.0, {800, 600}, true), WoTMetalLayerRefreshSpecReason::None);
		Policy.Tick(2.0625, 0.0625, {800, 600}, true);
		Policy.Tick(5.0, 1.0, {800, 600}, true);
		Policy.OnResult(5.0, WoTMetalLayerRefreshSpecResult::Refreshed);
		TestEqual(TEXT("zero duration preserves the consecutive healthy run"),
			Policy.Tick(10.0, 1.0, {800, 600}, true), WoTMetalLayerRefreshSpecReason::FrameStall);
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
