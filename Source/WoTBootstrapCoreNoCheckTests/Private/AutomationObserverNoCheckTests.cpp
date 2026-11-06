#include "Misc/AutomationTest.h"
#include "WoTBootstrapCore/Hash.h"

#if WITH_AUTOMATION_TESTS
namespace
{
constexpr EAutomationTestFlags WoTObserverNoCheckSpecFlags =
	EAutomationTestFlags::ProgramContext
	| EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTObserverNoCheckCleanupSpec,
	"WoT.BootstrapCore.Observer.NoCheckCleanup",
	WoTObserverNoCheckSpecFlags)

bool FWoTObserverNoCheckCleanupSpec::RunTest(const FString&)
{
	TestEqual(TEXT("the forced observer test compiles with checks elided"),
		DO_CHECK, 0);
	FCriticalSection Guard;
	TArray<TSharedPtr<TFunction<void(int32)>, ESPMode::ThreadSafe>> Entries;
	TSet<const void*> ActiveEntries;
	TMap<const void*, int32> InFlight;
	TMap<const void*, TMap<uint32, int32>> ActiveThreads;
	TMap<const void*, FEvent*> DrainedEvents;
	TAtomic<int32> Calls = 0;
	auto Token = WoTRegisterAutomationObserverForTesting(
		Guard, Entries, ActiveEntries, InFlight, ActiveThreads, DrainedEvents,
		TFunction<void(int32)>([&](int32 Value)
		{
			if (Value == 17)
			{
				++Calls;
			}
		}));
	WoTBroadcastAutomationObserverForTesting(
		Guard, Entries, ActiveEntries, InFlight, ActiveThreads, DrainedEvents,
		17);
	Token.Reset();
	TestEqual(TEXT("the observer runs in the forced no-check configuration"),
		Calls.Load(), 1);
	TestTrue(TEXT("no-check unregister removes the callback entry"),
		Entries.IsEmpty());
	TestTrue(TEXT("no-check unregister removes active membership"),
		ActiveEntries.IsEmpty());
	TestTrue(TEXT("no-check unregister removes in-flight bookkeeping"),
		InFlight.IsEmpty());
	TestTrue(TEXT("no-check unregister removes thread bookkeeping"),
		ActiveThreads.IsEmpty());
	TestTrue(TEXT("no-check unregister removes drain-event bookkeeping"),
		DrainedEvents.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTObserverNoCheckSelfUnregisterSpec,
	"WoT.BootstrapCore.Observer.NoCheckSelfUnregister",
	WoTObserverNoCheckSpecFlags)

bool FWoTObserverNoCheckSelfUnregisterSpec::RunTest(const FString&)
{
	FCriticalSection Guard;
	TArray<TSharedPtr<TFunction<void(int32)>, ESPMode::ThreadSafe>> Entries;
	TSet<const void*> ActiveEntries;
	TMap<const void*, int32> InFlight;
	TMap<const void*, TMap<uint32, int32>> ActiveThreads;
	TMap<const void*, FEvent*> DrainedEvents;
	TAtomic<int32> Calls = 0;
	TSharedPtr<uint8, ESPMode::ThreadSafe> Token;
	Token = WoTRegisterAutomationObserverForTesting(
		Guard, Entries, ActiveEntries, InFlight, ActiveThreads, DrainedEvents,
		TFunction<void(int32)>([&](int32 Value)
		{
			if (Value == 23)
			{
				++Calls;
				Token.Reset();
			}
		}));
	WoTBroadcastAutomationObserverForTesting(
		Guard, Entries, ActiveEntries, InFlight, ActiveThreads, DrainedEvents,
		23);
	WoTBroadcastAutomationObserverForTesting(
		Guard, Entries, ActiveEntries, InFlight, ActiveThreads, DrainedEvents,
		23);
	TestEqual(TEXT("self-unregister runs exactly once without deadlock"),
		Calls.Load(), 1);
	TestFalse(TEXT("self-unregister releases the final token"), Token.IsValid());
	TestTrue(TEXT("self-unregister removes the callback entry"),
		Entries.IsEmpty());
	TestTrue(TEXT("self-unregister removes active membership"),
		ActiveEntries.IsEmpty());
	TestTrue(TEXT("self-unregister removes in-flight bookkeeping"),
		InFlight.IsEmpty());
	TestTrue(TEXT("self-unregister removes thread bookkeeping"),
		ActiveThreads.IsEmpty());
	TestTrue(TEXT("self-unregister removes drain-event bookkeeping"),
		DrainedEvents.IsEmpty());
	return !HasAnyErrors();
}
#endif
