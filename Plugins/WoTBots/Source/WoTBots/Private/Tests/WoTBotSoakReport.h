#pragma once
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/CoreDelegates.h"
#include "HAL/PlatformStackWalk.h"

namespace WoTBotSoak
{
using FObject = TSharedRef<FJsonObject>;
inline FObject Object() { return MakeShared<FJsonObject>(); }
inline TSharedPtr<FJsonValue> Value(const FObject& O) { return MakeShared<FJsonValueObject>(O); }
inline void Increment(const FObject& O, const TCHAR* Key, double Amount = 1)
{
	double Previous = 0; O->TryGetNumberField(Key, Previous); O->SetNumberField(Key, Previous + Amount);
}
inline double Number(const FObject& O, const TCHAR* Key)
{
	double Result = 0; O->TryGetNumberField(Key, Result); return Result;
}
inline FString Encode(const FObject& O)
{
	FString Text; FJsonSerializer::Serialize(O, TJsonWriterFactory<>::Create(&Text)); return Text;
}
inline bool Save(const FObject& O, const FString& Path)
{
	return FFileHelper::SaveStringToFile(Encode(O), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
inline FString Position(const FVector& P)
{
	return FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), P.X, P.Y, P.Z);
}
inline bool Finite(const FVector& V)
{
	return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
}
inline bool Failed(bool bComplete, int32 Ensures, int32 Errors, int32 BadActors, int32 Kills)
{
	return !bComplete || Ensures > 0 || Errors > 0 || BadActors > 0 || Kills == 0;
}

struct FLogCapture : FOutputDevice
{
	FCriticalSection Mutex;
	TAtomic<double> Time{0};
	TMap<FString, FObject> Groups;
	int32 Ensures = 0, Errors = 0, Warnings = 0;
	FDelegateHandle EnsureHandle;
	FLogCapture()
	{
		GLog->AddOutputDevice(this);
		EnsureHandle = FCoreDelegates::OnEnsureFailed.AddLambda(
			[this](const ANSICHAR*, const ANSICHAR*, int32, const TCHAR*, const TCHAR* Combined)
			{
				FScopeLock Lock(&Mutex);
				++Ensures;
				const FString Key = FString(TEXT("ensure:")) + Combined;
				if (FObject* Existing = Groups.Find(Key)) { Increment(*Existing, TEXT("count")); return; }
				FObject Entry = Object();
				Entry->SetStringField(TEXT("severity"), TEXT("ensure"));
				Entry->SetStringField(TEXT("message"), Combined);
				Entry->SetNumberField(TEXT("first_seconds"), Time.Load());
				Entry->SetNumberField(TEXT("count"), 1);
				ANSICHAR Stack[65536] = {};
				FPlatformStackWalk::StackWalkAndDump(Stack, UE_ARRAY_COUNT(Stack), 2);
				Entry->SetStringField(TEXT("first_callstack"), ANSI_TO_TCHAR(Stack));
				Groups.Add(Key, Entry);
			});
	}
	~FLogCapture()
	{
		FCoreDelegates::OnEnsureFailed.Remove(EnsureHandle);
		GLog->RemoveOutputDevice(this);
	}
	bool CanBeUsedOnAnyThread() const override { return true; }
	void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		const auto Level = Verbosity & ELogVerbosity::VerbosityMask;
		if (Level > ELogVerbosity::Warning || Level == ELogVerbosity::NoLogging) return;
		FScopeLock Lock(&Mutex);
		if (Level <= ELogVerbosity::Error) ++Errors; else ++Warnings;
		const FString Key = FString::Printf(TEXT("%d:%s:%s"), int32(Level), *Category.ToString(), Message);
		if (FObject* Existing = Groups.Find(Key)) { Increment(*Existing, TEXT("count")); return; }
		FObject Entry = Object();
		Entry->SetStringField(TEXT("severity"), Level <= ELogVerbosity::Error ? TEXT("error") : TEXT("warning"));
		Entry->SetStringField(TEXT("category"), Category.ToString());
		Entry->SetStringField(TEXT("message"), Message);
		Entry->SetNumberField(TEXT("first_seconds"), Time.Load());
		Entry->SetNumberField(TEXT("count"), 1);
		Groups.Add(Key, Entry);
	}
	void Snapshot(const FObject& Report)
	{
		FScopeLock Lock(&Mutex);
		TArray<TSharedPtr<FJsonValue>> Entries;
		for (const auto& Pair : Groups) Entries.Add(Value(Pair.Value));
		Report->SetArrayField(TEXT("logs"), Entries);
		Report->SetNumberField(TEXT("ensures"), Ensures);
		Report->SetNumberField(TEXT("errors"), Errors);
		Report->SetNumberField(TEXT("warnings"), Warnings);
	}
};

inline FString SummaryHeader()
{
	return TEXT("| Map | Result | Kills | Deaths | Ensures/errors | Invalid/runaway | Recoveries/respawns | Unvisited pools | Tick avg/p95/max ms | Frag end/restart request |\n|---|---|---:|---:|---|---|---|---:|---|---|\n");
}
inline FString SummaryRow(const FObject& M)
{
	return FString::Printf(TEXT("| %s | %s | %.0f | %.0f | %.0f/%.0f | %.0f/%.0f | %.0f/%.0f | %.0f | %.2f/%.2f/%.2f | %s/%s |\n"),
		*M->GetStringField(TEXT("map")), *M->GetStringField(TEXT("result")), Number(M,TEXT("kills")), Number(M,TEXT("deaths")),
		Number(M,TEXT("ensures")), Number(M,TEXT("errors")), Number(M,TEXT("non_finite")), Number(M,TEXT("runaway")),
		Number(M,TEXT("recoveries")), Number(M,TEXT("silent_respawns")), Number(M,TEXT("unvisited_pools")),
		Number(M,TEXT("tick_avg_ms")), Number(M,TEXT("tick_p95_ms")), Number(M,TEXT("tick_max_ms")),
		M->GetBoolField(TEXT("frag_limit_end")) ? TEXT("yes") : TEXT("no"),
		M->GetBoolField(TEXT("restart_requested")) ? TEXT("yes") : TEXT("no"));
}
inline FString Markdown(const TArray<FObject>& Maps, double WallSeconds)
{
	FString Text = FString::Printf(TEXT("# Bot soak\n\nWall time: %.2f seconds. Fixed simulation step: 1/30 second.\n\n"), WallSeconds);
	Text += TEXT("Each map runs in its own headless process with the ordinary Arena rules and roster. Seeds are per map. Recast internals can still depend on engine scheduling. Tick timings measure World::Tick only; monitoring and loading are excluded. Slow ticks exceed 33.33 ms.\n\n");
	Text += TEXT("A socketless test driver selects dedicated-server gameplay semantics. Network transport ticks are disabled; this is a gameplay soak, not a network throughput measurement. Synchronous event and log capture overhead is included in tick timing. An actor can appear once in each anomaly category if a runaway velocity later becomes non-finite.\n\n");
	Text += TEXT("H4 records the natural frag-limit end and unattended restart request. Existing automation suppresses ServerTravel: completed reload is **not tested**. No forced score or shortened frag limit is used.\n\n");
	Text += SummaryHeader();
	for (const FObject& M : Maps) Text += SummaryRow(M);
	Text += TEXT("\nVisits mean a living bot within 120 units; unvisited in the original table means never collected. Pickups include loose items and bags. Rates and state shares use observed controller seconds (including dead/frozen). Falling is the share of those seconds a living body spent falling, in any state. Clustering samples once per simulated second, over living bodies that are not falling: any three with all pair distances <= 600. Pair distances cover every such pair in those samples; the mean is sensitive to a few bodies far from the rest, the median is not.\n\n");
	Text += TEXT("| Map | Pool count | Pickups/bot/min | Unique pools visited/collected | Roam/Seek/Engage/Dead/Frozen % | Falling % | Cluster % | Median/mean pair distance |\n|---|---:|---:|---|---|---:|---:|---|\n");
	for (const FObject& M : Maps)
	{
		const double Scale = 100 / FMath::Max(1.0, Number(M, TEXT("bot_seconds")));
		const TArray<TSharedPtr<FJsonValue>>* PoolRows = nullptr;
		Text += FString::Printf(TEXT("| %s | %d | %.2f | %.0f/%.0f | %.1f/%.1f/%.1f/%.1f/%.1f | %.1f | %.1f | %.0f/%.0f |\n"),
			*M->GetStringField(TEXT("map")), M->TryGetArrayField(TEXT("pools"), PoolRows) ? PoolRows->Num() : 0,
			Number(M,TEXT("pickups_per_bot_minute")), Number(M,TEXT("unique_pools_visited")), Number(M,TEXT("unique_pools_collected")),
			Scale*Number(M,TEXT("roam_seconds")), Scale*Number(M,TEXT("seek_seconds")), Scale*Number(M,TEXT("engage_seconds")), Scale*Number(M,TEXT("dead_seconds")), Scale*Number(M,TEXT("frozen_seconds")),
			Scale*Number(M,TEXT("falling_seconds")), 100*Number(M,TEXT("cluster_share")), Number(M,TEXT("median_pair_distance")), Number(M,TEXT("mean_pair_distance")));
	}
	for (const FObject& M : Maps)
	{
		Text += FString::Printf(TEXT("\n## %s\n\nSeed: %.0f; requested duration: %.2f s; completed simulation: %.2f s; fill: %.0f.\n\n"),
			*M->GetStringField(TEXT("map")), Number(M,TEXT("seed")), Number(M,TEXT("requested_seconds")), Number(M,TEXT("simulated_seconds")), Number(M,TEXT("fill")));
		Text += TEXT("Minimal reproduction: set WOT_BOT_SOAK=1 and WOT_REFERENCE_ROOT, run WoT.Bots.Soak.AllArenaMaps with ");
		Text += FString::Printf(TEXT("`-WotBotSoakMap=%s -WotBotSoakOutput=<report.json>` using the same fill and seconds. The map seed is deterministic. Raw worker log is adjacent to the report.\n\n"), *M->GetStringField(TEXT("map")));
		Text += TEXT("Warnings include recoveries, silent respawns, unvisited pools and slow ticks; these do not independently fail the map. Full per-bot, per-pool, grouped log and first-event evidence follows.\n\n```json\n");
		Text += Encode(M) + TEXT("\n```\n");
	}
	return Text;
}
}
#endif
