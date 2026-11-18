#include "WoTNetTestSubsystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
#include "WoTNetTestChannel.h"
#include "WoTNetTestSnapshot.h"
#include "WoTNetTestActions.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#endif // WOT_WITH_BOOTSTRAP_TEST_HARNESSES

bool UWoTNetTestSubsystem::ShouldCreateSubsystem(UObject* /*Outer*/) const
{
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	// Only when explicitly requested on the command line (the harness passes it).
	return FParse::Param(FCommandLine::Get(), TEXT("wotnettest"));
#else
	return false;
#endif
}

void UWoTNetTestSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	int32 Port = 17777;
	FParse::Value(FCommandLine::Get(), TEXT("wotnettestport="), Port);
	FString Bind = TEXT("127.0.0.1");
	FParse::Value(FCommandLine::Get(), TEXT("wotnettestbind="), Bind);
	FString Tok = TEXT("wotnet");
	FParse::Value(FCommandLine::Get(), TEXT("wotnettesttoken="), Tok);
	FParse::Value(FCommandLine::Get(), TEXT("wotnettestlog="), ProbeLogPath);

	Channel = new FWoTNetTestChannel();
	if (Channel->Start((uint16)Port, Bind, Tok))
	{
		UE_LOG(LogTemp, Display, TEXT("WoTNetTest: channel listening on %s:%u (token gated)"),
			*Bind, Channel->GetBoundPort());
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("WoTNetTest: channel FAILED to bind %s:%d"), *Bind, Port);
	}

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UWoTNetTestSubsystem::Tick), 0.0f);
#endif
}

void UWoTNetTestSubsystem::Deinitialize()
{
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	if (Channel)
	{
		Channel->Stop();
		delete Channel;
		Channel = nullptr;
	}
#endif
	Super::Deinitialize();
}

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
bool UWoTNetTestSubsystem::Tick(float /*DeltaTime*/)
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (Channel && World)
	{
		Channel->PumpOnGameThread(World);
		for (const TPair<int32, WoTNetTest::FMoveState>& Move : Channel->HeldMoves)
		{
			WoTNetTest::ApplyMoveTick(World, Move.Key, Move.Value);
		}
		Channel->TickDirector(World);
		if (!ProbeLogPath.IsEmpty() && FPlatformTime::Seconds() - LastProbeTime > 0.25)
		{
			FFileHelper::SaveStringToFile(WoTNetTest::BuildSnapshotJson(World) + TEXT("\n"),
				*ProbeLogPath, FFileHelper::EEncodingOptions::AutoDetect,
				&IFileManager::Get(), EFileWrite::FILEWRITE_Append);
			LastProbeTime = FPlatformTime::Seconds();
		}
	}
	return true;   // keep ticking
}
#endif // WOT_WITH_BOOTSTRAP_TEST_HARNESSES
