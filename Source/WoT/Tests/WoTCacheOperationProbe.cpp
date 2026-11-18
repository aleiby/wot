// One-shot, environment-gated throughput probe for the real pinned installer.
#if WITH_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WoTRetailReader/Package.h"
#include "WoTRetailReader/Reader.h"

#include "../Payload/WoTCacheStore.h"
#include "../Updater/WoTCacheBootstrapOperation.h"
#include "../Updater/WoTInstallerAcquisition.h"

namespace
{
constexpr double WoTCacheOperationProbeCapSeconds = 30.0 * 60.0;

const TCHAR* WoTCacheOperationProbeDomain(EWoTFactDomain Domain)
{
	switch (Domain)
	{
	case EWoTFactDomain::Texture: return TEXT("texture");
	case EWoTFactDomain::Sound: return TEXT("sound");
	case EWoTFactDomain::Music: return TEXT("music");
	case EWoTFactDomain::Mesh: return TEXT("mesh");
	case EWoTFactDomain::Animation: return TEXT("animation");
	case EWoTFactDomain::BSP: return TEXT("bsp");
	case EWoTFactDomain::World: return TEXT("world");
	case EWoTFactDomain::Font: return TEXT("font");
	case EWoTFactDomain::Procedural: return TEXT("procedural");
	case EWoTFactDomain::Registry: return TEXT("registry");
	case EWoTFactDomain::Localization: return TEXT("localization");
	case EWoTFactDomain::ClassDefault: return TEXT("class_default");
	}
	return TEXT("unknown");
}

struct FWoTCacheOperationProbeStage
{
	struct FPoint
	{
		uint64 FactsDone = 0;
		double ElapsedSeconds = 0.0;
	};

	FString Name;
	double StartedElapsedSeconds = 0.0;
	double EndedElapsedSeconds = 0.0;
	uint64 FactsDoneStart = 0;
	uint64 FactsDoneEnd = 0;
	TArray<FPoint> Points;
};

class FWoTCacheOperationProbeProgress final : public IWoTProgressSink
{
public:
	explicit FWoTCacheOperationProbeProgress(FString InProgressPath)
		: ProgressPath(MoveTemp(InProgressPath))
		, StartedAt(FPlatformTime::Seconds())
	{
	}

	bool Append(const TSharedRef<FJsonObject>& Object)
	{
		FString Json;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		if (!FJsonSerializer::Serialize(Object, Writer))
		{
			return false;
		}
		Json.AppendChar(TEXT('\n'));
		// SaveStringToFile opens, writes, flushes, and closes for every line.
		return FFileHelper::SaveStringToFile(Json, *ProgressPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(), FILEWRITE_Append);
	}

	virtual FWoTBootstrapStatus ReportProgress(FStringView Stage,
		uint64 Completed, uint64 Total) override
	{
		const double Elapsed = ElapsedSeconds();
		const FString OwnedStage(Stage);
		const bool bStageTransition = OwnedStage != LastStage;
		if (bStageTransition)
		{
			if (!LastStage.IsEmpty())
			{
				Stages.Last().EndedElapsedSeconds = Elapsed;
				Stages.Last().FactsDoneEnd = LastCompleted;
			}
			FWoTCacheOperationProbeStage& NewStage = Stages.AddDefaulted_GetRef();
			NewStage.Name = OwnedStage;
			NewStage.StartedElapsedSeconds = Elapsed;
			NewStage.FactsDoneStart = Completed;
		}
		FWoTCacheOperationProbeStage& CurrentStage = Stages.Last();
		CurrentStage.Points.Add({Completed, Elapsed});
		LastStage = OwnedStage;
		LastCompleted = Completed;
		LastTotal = Total;

		const bool bFactInterval = Completed >= LastEmittedCompleted
			&& Completed - LastEmittedCompleted >= 25;
		if (bStageTransition || bFactInterval)
		{
			TSharedRef<FJsonObject> Line = MakeShared<FJsonObject>();
			Line->SetStringField(TEXT("event"), TEXT("progress"));
			Line->SetStringField(TEXT("stage"), OwnedStage);
			Line->SetNumberField(TEXT("facts_done"), static_cast<double>(Completed));
			Line->SetNumberField(TEXT("facts_total"), static_cast<double>(Total));
			Line->SetNumberField(TEXT("elapsed_s"), Elapsed);
			Line->SetField(TEXT("bytes_read_from_archive"),
				MakeShared<FJsonValueNull>());
			if (!Append(Line))
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("the cache-operation probe could not append progress"),
					ProgressPath));
			}
			LastEmittedCompleted = Completed;
		}
		return FWoTBootstrapStatus::Success();
	}

	void SetEntryBufferHighWater(uint64 Bytes)
	{
		EntryBufferHighWater = FMath::Max(EntryBufferHighWater, Bytes);
	}

	virtual bool IsCancellationRequested() const override
	{
		if (ElapsedSeconds() >= WoTCacheOperationProbeCapSeconds)
		{
			bReachedCap = true;
		}
		return bReachedCap;
	}

	double ElapsedSeconds() const
	{
		return FPlatformTime::Seconds() - StartedAt;
	}

	void AppendFinalSummary(bool bCompleted, const FString& OperationError)
	{
		const double Elapsed = ElapsedSeconds();
		if (!Stages.IsEmpty())
		{
			Stages.Last().EndedElapsedSeconds = Elapsed;
			Stages.Last().FactsDoneEnd = LastCompleted;
		}
		const double ElapsedMinutes = Elapsed / 60.0;
		const double Rate = ElapsedMinutes > 0.0
			? static_cast<double>(LastCompleted) / ElapsedMinutes : 0.0;

		TSharedRef<FJsonObject> Line = MakeShared<FJsonObject>();
		Line->SetStringField(TEXT("event"), TEXT("summary"));
		Line->SetStringField(TEXT("verdict"), bCompleted
			? TEXT("completed")
			: (bReachedCap ? TEXT("cancelled-at-cap") : TEXT("failed")));
		Line->SetStringField(TEXT("stage"), LastStage);
		Line->SetNumberField(TEXT("facts_done"), static_cast<double>(LastCompleted));
		Line->SetNumberField(TEXT("facts_total"), static_cast<double>(LastTotal));
		Line->SetNumberField(TEXT("elapsed_s"), Elapsed);
		Line->SetField(TEXT("bytes_read_from_archive"),
			MakeShared<FJsonValueNull>());
		Line->SetNumberField(TEXT("entry_buffer_high_water_bytes"),
			static_cast<double>(EntryBufferHighWater));
		Line->SetNumberField(TEXT("rate_facts_per_min"), Rate);
		if (Rate > 0.0)
		{
			Line->SetNumberField(TEXT("extrapolated_total_min"),
				static_cast<double>(LastTotal) / Rate);
		}
		else
		{
			Line->SetField(TEXT("extrapolated_total_min"),
				MakeShared<FJsonValueNull>());
		}
		if (!OperationError.IsEmpty())
		{
			Line->SetStringField(TEXT("operation_error"), OperationError);
		}
		TArray<TSharedPtr<FJsonValue>> StageValues;
		for (const FWoTCacheOperationProbeStage& Stage : Stages)
		{
			TSharedRef<FJsonObject> StageObject = MakeShared<FJsonObject>();
			StageObject->SetStringField(TEXT("stage"), Stage.Name);
			StageObject->SetNumberField(TEXT("started_elapsed_s"),
				Stage.StartedElapsedSeconds);
			StageObject->SetNumberField(TEXT("ended_elapsed_s"),
				Stage.EndedElapsedSeconds);
			StageObject->SetNumberField(TEXT("facts_done_start"),
				static_cast<double>(Stage.FactsDoneStart));
			StageObject->SetNumberField(TEXT("facts_done_end"),
				static_cast<double>(Stage.FactsDoneEnd));
			const uint64 Span = Stage.FactsDoneEnd >= Stage.FactsDoneStart
				? Stage.FactsDoneEnd - Stage.FactsDoneStart : 0;
			if (Span >= 1000 && !Stage.Points.IsEmpty())
			{
				const auto ElapsedAtOrAfter = [&Stage](uint64 Facts)
				{
					for (const FWoTCacheOperationProbeStage::FPoint& Point :
						Stage.Points)
					{
						if (Point.FactsDone >= Facts)
						{
							return Point.ElapsedSeconds;
						}
					}
					return Stage.Points.Last().ElapsedSeconds;
				};
				const double FirstStart =
					ElapsedAtOrAfter(Stage.FactsDoneStart);
				const double FirstEnd =
					ElapsedAtOrAfter(Stage.FactsDoneStart + 500);
				const double LastStart =
					ElapsedAtOrAfter(Stage.FactsDoneEnd - 500);
				const double LastEnd =
					ElapsedAtOrAfter(Stage.FactsDoneEnd);
				const double FirstRate = FirstEnd > FirstStart
					? 500.0 * 60.0 / (FirstEnd - FirstStart) : 0.0;
				const double LastRate = LastEnd > LastStart
					? 500.0 * 60.0 / (LastEnd - LastStart) : 0.0;
				StageObject->SetNumberField(TEXT("first_500_facts_per_min"),
					FirstRate);
				StageObject->SetNumberField(TEXT("last_500_facts_per_min"),
					LastRate);
				StageObject->SetNumberField(TEXT("last_to_first_rate_ratio"),
					FirstRate > 0.0 ? LastRate / FirstRate : 0.0);
			}
			StageValues.Add(MakeShared<FJsonValueObject>(StageObject));
		}
		Line->SetArrayField(TEXT("stage_breakdown"), MoveTemp(StageValues));
		Append(Line);
	}

	FString ProgressPath;
	FString LastStage;
	uint64 LastCompleted = 0;
	uint64 LastTotal = 0;
	mutable bool bReachedCap = false;

private:
	double StartedAt = 0.0;
	uint64 LastEmittedCompleted = 0;
	uint64 EntryBufferHighWater = 0;
	TArray<FWoTCacheOperationProbeStage> Stages;
};

FWoTBootstrapStatus WoTCacheOperationProbeRecordOrdering(
	FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	FWoTCacheOperationProbeProgress& Progress)
{
	static constexpr EWoTFactDomain Domains[] = {
		EWoTFactDomain::Texture, EWoTFactDomain::Sound, EWoTFactDomain::Music,
		EWoTFactDomain::Mesh, EWoTFactDomain::Animation, EWoTFactDomain::BSP,
		EWoTFactDomain::World, EWoTFactDomain::Font,
		EWoTFactDomain::Procedural, EWoTFactDomain::Registry,
		EWoTFactDomain::Localization, EWoTFactDomain::ClassDefault};
	for (EWoTFactDomain Domain : Domains)
	{
		TArray<TSharedPtr<FJsonValue>> Pairs;
		uint32 PreviousIndex = 0;
		bool bHavePrevious = false;
		bool bNondecreasing = true;
		TSet<uint32> DistinctIndices;
		for (const FWoTRequiredFact& Fact : Contract.Facts)
		{
			if (Fact.Domain != Domain || Pairs.Num() >= 50) continue;
			TWoTBootstrapResult<FWoTPackageEntry> Entry =
				Package.FindExact(Fact.SourcePath);
			if (Entry.IsError())
			{
				return FWoTBootstrapStatus::Error(MoveTemp(Entry).GetError());
			}
			const uint32 Index = Entry.GetValue().AdapterIndex;
			if (bHavePrevious && Index < PreviousIndex) bNondecreasing = false;
			PreviousIndex = Index;
			bHavePrevious = true;
			DistinctIndices.Add(Index);
			TSharedRef<FJsonObject> Pair = MakeShared<FJsonObject>();
			Pair->SetStringField(TEXT("fact_id"), Fact.FactId);
			Pair->SetNumberField(TEXT("archive_entry_index"),
				static_cast<double>(Index));
			Pairs.Add(MakeShared<FJsonValueObject>(Pair));
		}
		if (Pairs.IsEmpty()) continue;
		TSharedRef<FJsonObject> Line = MakeShared<FJsonObject>();
		Line->SetStringField(TEXT("event"), TEXT("ordering_evidence"));
		Line->SetStringField(TEXT("domain"), WoTCacheOperationProbeDomain(Domain));
		Line->SetNumberField(TEXT("facts_sampled"), Pairs.Num());
		Line->SetNumberField(TEXT("distinct_archive_entries"),
			DistinctIndices.Num());
		Line->SetBoolField(TEXT("archive_indices_nondecreasing"), bNondecreasing);
		Line->SetArrayField(TEXT("pairs"), MoveTemp(Pairs));
		if (!Progress.Append(Line))
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::Io,
				TEXT("the cache-operation probe could not append ordering evidence"),
				Progress.ProgressPath));
		}
	}
	return FWoTBootstrapStatus::Success();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheOperationProbe,
	"WoT.Bootstrap.CacheOperation.Probe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCacheOperationProbe::RunTest(const FString&)
{
	const FString InstallerPath = FPlatformMisc::GetEnvironmentVariable(
		TEXT("WOT_CACHE_OPERATION_PROBE_INSTALLER")).TrimStartAndEnd();
	const FString OutputDir = FPlatformMisc::GetEnvironmentVariable(
		TEXT("WOT_CACHE_OPERATION_PROBE_OUTPUT_DIR")).TrimStartAndEnd();
	if (InstallerPath.IsEmpty() && OutputDir.IsEmpty())
	{
		AddInfo(TEXT("cache-operation probe skipped; environment gate is absent"));
		return true;
	}
	if (InstallerPath.IsEmpty() || OutputDir.IsEmpty()
		|| FPaths::IsRelative(InstallerPath) || FPaths::IsRelative(OutputDir))
	{
		AddError(TEXT("cache-operation probe paths must both be absolute"));
		return false;
	}

	const FString ProgressPath = OutputDir / TEXT("progress.jsonl");
	const FString StoreRoot = OutputDir / TEXT("cache-store");
	if (IFileManager::Get().FileExists(*ProgressPath)
		|| IFileManager::Get().DirectoryExists(*StoreRoot))
	{
		AddError(TEXT("cache-operation probe refuses non-fresh output paths"));
		return false;
	}
	if (!IFileManager::Get().MakeDirectory(*OutputDir, true))
	{
		AddError(FString::Printf(TEXT("could not create probe output: %s"),
			*OutputDir));
		return false;
	}

	TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult> Acquired =
		WoTTryAcquirePinnedAdjacentInstaller(InstallerPath);
	if (Acquired.IsError())
	{
		AddError(Acquired.GetError().ToString());
		return false;
	}
	if (Acquired.GetValue().State
		!= EWoTPinnedAdjacentInstallerProbe::Valid
		|| !Acquired.GetValue().Verified.IsValid())
	{
		AddError(TEXT("cache-operation probe requires the pinned installer"));
		return false;
	}
	FWoTVerifiedInstaller Installer =
		MoveTemp(*Acquired.GetValue().Verified);
	TWoTBootstrapResult<FWoTCacheStore> OpenedStore =
		FWoTCacheStore::ForTesting(StoreRoot);
	if (OpenedStore.IsError())
	{
		AddError(OpenedStore.GetError().ToString());
		return false;
	}
	FWoTCacheStore Store = MoveTemp(OpenedStore).GetValue();
	FWoTCacheOperationProbeProgress Progress(ProgressPath);

	FWoTCacheBootstrapOperation Operation(
		[&Progress](FWoTRetailPackage& Package,
			const FWoTRetailContract& Contract, IWoTObjectWriter& Writer, IWoTProgressSink& BuildProgress)
		{
			using FResult = TWoTBootstrapResult<FWoTCacheBuildResult>;
			FWoTBootstrapStatus Ordering = WoTCacheOperationProbeRecordOrdering(
				Package, Contract, Progress);
			if (Ordering.IsError())
			{
				return FResult::Error(MoveTemp(Ordering).GetError());
			}
			FWoTRetailReader Reader;
			FResult Built =
				Reader.Build(Package, Contract, Writer, BuildProgress);
			Progress.SetEntryBufferHighWater(
				Package.EntryBufferHighWaterBytes());
			return Built;
		});
	TWoTBootstrapResult<FWoTPublishedCacheReceipt> Published =
		Operation.BuildAndPublish(MoveTemp(Installer), Store, Progress);
	const bool bCompleted = !Published.IsError();
	const FString OperationError = Published.IsError()
		? Published.GetError().ToString() : FString();
	Progress.AppendFinalSummary(bCompleted, OperationError);

	if (bCompleted)
	{
		TestEqual(TEXT("completed fact count"),
			Published.GetValue().CompletedProgressUnits,
			Published.GetValue().TotalProgressUnits);
		return true;
	}
	if (!Progress.bReachedCap)
	{
		AddError(OperationError);
		return false;
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
