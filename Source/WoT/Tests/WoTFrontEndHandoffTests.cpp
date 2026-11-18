#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTBootstrapHeartbeat.h"
#include "../WoTBootstrapViewModel.h"
#include "../WoTFrontEndPlayerController.h"
#include "WoTTestWorld.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
constexpr EAutomationTestFlags WoTFrontEndHandoffSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

TArray<TSharedPtr<FJsonObject>> WoTFrontEndHandoffReadRecords(
	const FString& Path)
{
	FString Contents;
	TArray<TSharedPtr<FJsonObject>> Result;
	if (!FFileHelper::LoadFileToString(Contents, *Path))
	{
		return Result;
	}
	TArray<FString> Lines;
	Contents.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(Line), Object) || !Object.IsValid())
		{
			return {};
		}
		Result.Add(MoveTemp(Object));
	}
	return Result;
}

TArray<FString> WoTFrontEndHandoffEventsForEpoch(
	const TArray<TSharedPtr<FJsonObject>>& Records, int64 Epoch)
{
	TArray<FString> Result;
	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		if (Record->GetIntegerField(TEXT("epoch")) == Epoch)
		{
			Result.Add(Record->GetStringField(TEXT("event")));
		}
	}
	return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndHandoffPipelineSpec,
	"WoT.FrontEnd.HandoffPipeline",
	WoTFrontEndHandoffSpecFlags)

bool FWoTFrontEndHandoffPipelineSpec::RunTest(const FString&)
{
	FString Parent = FPaths::Combine(
		FPaths::AutomationTransientDir(), TEXT("wot-frontend-handoff-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Parent = FPaths::ConvertRelativePathToFull(Parent);
	FPaths::NormalizeDirectoryName(Parent);
	const FString InstallRoot = FPaths::Combine(Parent, TEXT("WoT"));
	if (!TestTrue(TEXT("handoff fixture parent is created"),
		IFileManager::Get().MakeDirectory(*Parent, true)))
	{
		return false;
	}
	WoTTest::FSpecWorldOptions WorldOptions;
	WorldOptions.bStartPlay = false;
	UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
	if (!TestNotNull(TEXT("handoff fixture world exists"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		SetWoTFrontEndHandoffTestAdapter(nullptr);
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		WoTTest::DestroySpecWorld(World);
		IFileManager::Get().DeleteDirectory(*Parent, false, true);
	};
	SetWoTBootstrapHeartbeatRootForTesting(InstallRoot);

	TArray<FString> Trace;
	FWoTFrontEndHandoffTestAdapter Adapter;
	Adapter.bPayloadSubsystemAvailable = true;
	Adapter.PrepareTextureConsumers = [&Trace]
	{
		Trace.Add(TEXT("texture-consumers-prepared"));
		return true;
	};
	Adapter.BindTextureConsumers = [&Trace]
	{
		Trace.Add(TEXT("texture-consumers-bound"));
		return true;
	};
	Adapter.PrepareWidget = [&Trace](EWoTFrontEndHandoffPath Path)
	{
		Trace.Add(Path == EWoTFrontEndHandoffPath::DirectAuthorized
			? TEXT("widget-constructed") : TEXT("widget-swapped"));
		return true;
	};
	Adapter.RebindLoadedPayloads = [] { return true; };
	Adapter.RefreshTextureBrushes = [] { return true; };
	Adapter.ObserveEvent = [&Trace](FStringView Event)
	{
		Trace.Add(FString(Event));
	};
	SetWoTFrontEndHandoffTestAdapter(&Adapter);

	auto RunSuccessPath = [this, World, &Trace](
		EWoTFrontEndHandoffPath Path, uint64 Epoch,
		const TArray<FString>& ExpectedTrace)
	{
		AWoTFrontEndPlayerController* Controller =
			World->SpawnActor<AWoTFrontEndPlayerController>();
		if (!TestNotNull(TEXT("handoff controller exists"), Controller))
		{
			return;
		}
		FWoTBootstrapSnapshot Ready;
		Ready.Phase = EWoTBootstrapPhase::Ready;
		Ready.OperationEpoch = Epoch;
		TestTrue(TEXT("scene span begin is durable"),
			WoTRecordBootstrapHeartbeat(TEXT("span-begin"), Ready.Phase,
				Ready.OperationEpoch, {}, {},
				TEXT("name=scene-preparation-handoff")));
		Trace.Reset();
		TestTrue(TEXT("handoff path succeeds"),
			Controller->RunFrontEndHandoffForTesting(Path, Ready));
		TestEqual(TEXT("handoff pipeline stage order"), Trace, ExpectedTrace);
		const int32 TraceCount = Trace.Num();
		TestTrue(TEXT("completed handoff is idempotent"),
			Controller->RunFrontEndHandoffForTesting(Path, Ready));
		TestEqual(TEXT("completed handoff emits no duplicate stages"),
			Trace.Num(), TraceCount);
	};

	RunSuccessPath(EWoTFrontEndHandoffPath::DirectAuthorized, 71,
		{TEXT("texture-consumers-prepared"), TEXT("texture-consumers-bound"),
		 TEXT("widget-constructed"), TEXT("payload-rebind"),
		 TEXT("brush-refresh"), TEXT("success-heartbeat")});
	RunSuccessPath(EWoTFrontEndHandoffPath::BootstrapSwap, 72,
		{TEXT("texture-consumers-prepared"), TEXT("texture-consumers-bound"),
		 TEXT("widget-swapped"), TEXT("payload-rebind"),
		 TEXT("brush-refresh"), TEXT("success-heartbeat")});

	auto RunRefusalPath = [this, World, &Trace, &Adapter](uint64 Epoch,
		bool bPayloadAvailable, bool bRebindSucceeds,
		bool bRefreshSucceeds, const TArray<FString>& ExpectedTrace)
	{
		AWoTFrontEndPlayerController* Refused =
			World->SpawnActor<AWoTFrontEndPlayerController>();
		if (!TestNotNull(TEXT("refusal controller exists"), Refused))
		{
			return;
		}
		FWoTBootstrapSnapshot Ready;
		Ready.Phase = EWoTBootstrapPhase::Ready;
		Ready.OperationEpoch = Epoch;
		TestTrue(TEXT("refusal scene span begin is durable"),
			WoTRecordBootstrapHeartbeat(TEXT("span-begin"), Ready.Phase,
				Ready.OperationEpoch, {}, {},
				TEXT("name=scene-preparation-handoff")));
		Adapter.bPayloadSubsystemAvailable = bPayloadAvailable;
		Adapter.RebindLoadedPayloads =
			[bRebindSucceeds] { return bRebindSucceeds; };
		Adapter.RefreshTextureBrushes =
			[bRefreshSucceeds] { return bRefreshSucceeds; };
		Trace.Reset();
		TestFalse(TEXT("post-widget-load failure refuses handoff"),
			Refused->RunFrontEndHandoffForTesting(
				EWoTFrontEndHandoffPath::DirectAuthorized, Ready));
		TestEqual(TEXT("refusal stops at the failed stage"),
			Trace, ExpectedTrace);
	};

	RunRefusalPath(73, false, true, true,
		{TEXT("texture-consumers-prepared"), TEXT("texture-consumers-bound"),
		 TEXT("widget-constructed"), TEXT("refused-heartbeat")});
	RunRefusalPath(74, true, false, true,
		{TEXT("texture-consumers-prepared"), TEXT("texture-consumers-bound"),
		 TEXT("widget-constructed"), TEXT("refused-heartbeat")});
	RunRefusalPath(75, true, true, false,
		{TEXT("texture-consumers-prepared"), TEXT("texture-consumers-bound"),
		 TEXT("widget-constructed"), TEXT("payload-rebind"),
		 TEXT("refused-heartbeat")});

	const FString HeartbeatPath = FPaths::Combine(InstallRoot,
		TEXT("UpdaterState"), WoTBootstrapHeartbeatFileName());
	const TArray<TSharedPtr<FJsonObject>> Records =
		WoTFrontEndHandoffReadRecords(HeartbeatPath);
	if (!TestTrue(TEXT("all handoff records parse as JSON"),
		Records.Num() > 0))
	{
		return false;
	}
	TestEqual(TEXT("direct path has one paired successful handoff"),
		WoTFrontEndHandoffEventsForEpoch(Records, 71),
		TArray<FString>{TEXT("span-begin"), TEXT("span-end"),
			TEXT("frontend-handoff")});
	TestEqual(TEXT("normal swap has one paired successful handoff"),
		WoTFrontEndHandoffEventsForEpoch(Records, 72),
		TArray<FString>{TEXT("span-begin"), TEXT("span-end"),
			TEXT("frontend-handoff")});
	TestEqual(TEXT("missing payload produces only a non-gating refusal"),
		WoTFrontEndHandoffEventsForEpoch(Records, 73),
		TArray<FString>{TEXT("span-begin"), TEXT("span-end"),
			TEXT("frontend-handoff-refused")});
	TestEqual(TEXT("failed rebind produces only a non-gating refusal"),
		WoTFrontEndHandoffEventsForEpoch(Records, 74),
		TArray<FString>{TEXT("span-begin"), TEXT("span-end"),
			TEXT("frontend-handoff-refused")});
	TestEqual(TEXT("failed brush refresh produces only a non-gating refusal"),
		WoTFrontEndHandoffEventsForEpoch(Records, 75),
		TArray<FString>{TEXT("span-begin"), TEXT("span-end"),
			TEXT("frontend-handoff-refused")});
	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		const int64 Epoch = Record->GetIntegerField(TEXT("epoch"));
		if (Epoch < 71 || Epoch > 75)
		{
			continue;
		}
		TestEqual(TEXT("every handoff pipeline record is phase Ready"),
			Record->GetStringField(TEXT("phase")), FString(TEXT("Ready")));
		const FString Event = Record->GetStringField(TEXT("event"));
		if (Event == TEXT("span-begin"))
		{
			TestEqual(TEXT("scene span begin names the aggregate"),
				Record->GetStringField(TEXT("detail")),
				FString(TEXT("name=scene-preparation-handoff")));
		}
		else if (Event == TEXT("span-end"))
		{
			const FString ExpectedDetail = Epoch <= 72
				? TEXT("name=scene-preparation-handoff result=success")
				: TEXT("name=scene-preparation-handoff result=failure");
			TestEqual(TEXT("scene span end names and closes the aggregate"),
				Record->GetStringField(TEXT("detail")), ExpectedDetail);
		}
	}
	// A failed diagnostic append must not strand a fully initialized menu.
	Adapter.bPayloadSubsystemAvailable = true;
	Adapter.RebindLoadedPayloads = [] { return true; };
	Adapter.RefreshTextureBrushes = [] { return true; };
	FailNextWoTBootstrapHeartbeatOpenForTesting();
	FWoTBootstrapSnapshot ReadyWithoutLog;
	ReadyWithoutLog.Phase = EWoTBootstrapPhase::Ready;
	ReadyWithoutLog.OperationEpoch = 76;
	AWoTFrontEndPlayerController* WithoutLog =
		World->SpawnActor<AWoTFrontEndPlayerController>();
	if (!TestNotNull(TEXT("controller without diagnostics exists"), WithoutLog))
		return false;
	TestTrue(TEXT("failed heartbeat does not gate the menu"),
		WithoutLog->RunFrontEndHandoffForTesting(
			EWoTFrontEndHandoffPath::DirectAuthorized, ReadyWithoutLog));
	return true;
}

#endif
