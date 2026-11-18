#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotMapFixture.h"
#include "WoTBotSoakReport.h"
#include "WoTBotSoakServer.h"
#include "Testing/WoTSoakEvents.h"
#include "EngineUtils.h"
#include "GameFramework/PhysicsVolume.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Crc.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"
namespace WoTBotSoak
{
// Scoped to this namespace, so the shared test helpers don't leak into
// the rest of the plugin's unity blob.
using namespace WoTBotTest;

namespace
{
constexpr float Step = 1.0f / 30.0f;
int32 MapSeed(const FString& Map) { return int32(FCrc::StrCrc32(*Map) & 0x7fffffff); }
FObject NewReport(const FString& Map, int32 Fill, double Seconds)
{
	FObject R = Object();
	R->SetStringField(TEXT("map"), Map);
	R->SetStringField(TEXT("result"), TEXT("INCOMPLETE"));
	R->SetNumberField(TEXT("seed"), MapSeed(Map));
	R->SetNumberField(TEXT("fill"), Fill);
	R->SetNumberField(TEXT("requested_seconds"), Seconds);
	R->SetNumberField(TEXT("simulated_seconds"), 0);
	R->SetBoolField(TEXT("frag_limit_end"), false);
	R->SetBoolField(TEXT("restart_requested"), false);
	R->SetBoolField(TEXT("reload_tested"), false);
	R->SetBoolField(TEXT("completed"), false);
	return R;
}

struct FObserver
{
	UWorld* World;
	FObject Report;
	FLogCapture& Logs;
	TMap<TWeakObjectPtr<AWoTBotController>, FObject> Bots;
	TMap<TWeakObjectPtr<AWoTAngrealSpawnPool>, FObject> Pools;
	TSet<TWeakObjectPtr<AActor>> NonFiniteActors, RunawayActors;
	TArray<TSharedPtr<FJsonValue>> Anomalies, Recoveries;
	TArray<double> TickTimes;
	FDelegateHandle Handle;
	double Time = 0;
	double LastSampleTime = 0;
	int32 LastClusterSecond = 0;
	TSet<TWeakObjectPtr<AWoTAngrealSpawnPool>> VisitedPools;
	TArray<double> PairDistances;
	FObserver(UWorld* W, const FObject& R, FLogCapture& L) : World(W), Report(R), Logs(L)
	{
		Handle = WoTSoakEvents::Events().AddLambda([this](WoTSoakEvents::EKind Kind, AActor* Subject, AActor* Participant)
		{
			if (!Subject || Subject->GetWorld() != World) return;
			AWoTBotController* B = Cast<AWoTBotController>(Subject);
			if (!B) if (const APawn* P = Cast<APawn>(Participant)) B = Cast<AWoTBotController>(P->GetController());
			if (AWoTAngrealSpawnPool* P = Cast<AWoTAngrealSpawnPool>(Subject))
			{
				if (Kind == WoTSoakEvents::EKind::PoolStock) Increment(Pool(P), TEXT("stocked"));
				if (Kind == WoTSoakEvents::EKind::PoolCollect) Increment(Pool(P), TEXT("collected"));
			}
			if (!B) return;
			FObject Row = Bot(B);
			if (Kind == WoTSoakEvents::EKind::Pickup) Increment(Row, TEXT("pickups"));
			if (Kind == WoTSoakEvents::EKind::Cast)
			{
				const auto Casts = Row->GetObjectField(TEXT("casts"));
				Increment(Casts.ToSharedRef(), *Subject->GetClass()->GetPathName());
			}
			if (Kind == WoTSoakEvents::EKind::Recovery || Kind == WoTSoakEvents::EKind::SilentRespawn)
			{
				const TCHAR* Key = Kind == WoTSoakEvents::EKind::Recovery ? TEXT("recoveries") : TEXT("silent_respawns");
				Increment(Row, Key); Increment(Report, Key);
				FObject Event = Object();
				Event->SetStringField(TEXT("bot"), B->GetName());
				Event->SetStringField(TEXT("kind"), Key);
				Event->SetNumberField(TEXT("seconds"), Time);
				Event->SetStringField(TEXT("position"), Position(Participant ? Participant->GetActorLocation() : Subject->GetActorLocation()));
				Recoveries.Add(Value(Event));
			}
		});
	}
	~FObserver() { WoTSoakEvents::Events().Remove(Handle); }
	FObject Bot(AWoTBotController* B)
	{
		if (const FObject* Found = Bots.Find(B)) return *Found;
		FObject R = Object(); R->SetStringField(TEXT("id"), B->GetName());
		R->SetObjectField(TEXT("casts"), Object());
		for (const TCHAR* Key : { TEXT("kills"), TEXT("deaths"), TEXT("suicides"), TEXT("pickups"), TEXT("recoveries"), TEXT("silent_respawns") }) R->SetNumberField(Key, 0);
		Bots.Add(B, R); return R;
	}
	FObject Pool(AWoTAngrealSpawnPool* P)
	{
		if (const FObject* Found = Pools.Find(P)) return *Found;
		FObject R = Object(); R->SetStringField(TEXT("id"), P->GetName());
		R->SetStringField(TEXT("position"), Position(P->GetActorLocation()));
		R->SetNumberField(TEXT("stocked"), 0); R->SetNumberField(TEXT("collected"), 0);
		Pools.Add(P, R); return R;
	}
	void Sample()
	{
		const double Delta = Time - LastSampleTime;
		LastSampleTime = Time;
		TArray<FVector> Living;
		for (TActorIterator<AWoTBotController> It(World); It; ++It)
		{
			FObject R = Bot(*It);
			static const TCHAR* States[] = { TEXT("roam_seconds"), TEXT("seek_seconds"), TEXT("engage_seconds"), TEXT("dead_seconds"), TEXT("frozen_seconds") };
			Increment(R, States[int32(It->GetBehaviourState())], Delta);
			Increment(Report, States[int32(It->GetBehaviourState())], Delta);
			Increment(Report, TEXT("bot_seconds"), Delta);
			if (const AWoTCharacter* Body = Cast<AWoTCharacter>(It->GetPawn()); Body && Body->IsAlive())
			{
				// A body falling through a gap in a floor can be far below the arena;
				// count its time, but keep it out of the arena's spacing samples.
				if (Body->GetCharacterMovement()->IsFalling())
				{
					Increment(R, TEXT("falling_seconds"), Delta);
					Increment(Report, TEXT("falling_seconds"), Delta);
				}
				else Living.Add(Body->GetActorLocation());
				for (const auto& P : Pools)
					if (P.Key.IsValid() && FVector::DistSquared(Body->GetActorLocation(), P.Key->GetActorLocation()) <= FMath::Square(120.0f))
						VisitedPools.Add(P.Key);
			}
			if (const AWoTPlayerState* S = It->GetPlayerState<AWoTPlayerState>())
			{
				R->SetNumberField(TEXT("kills"), S->Kills);
				R->SetNumberField(TEXT("deaths"), S->Deaths);
				R->SetNumberField(TEXT("suicides"), S->Suicides);
			}
		}
		if (FMath::FloorToInt(Time) > LastClusterSecond)
		{
			LastClusterSecond = FMath::FloorToInt(Time);
			bool Clustered = false;
			for (int32 A = 0; A < Living.Num(); ++A)
				for (int32 B = A + 1; B < Living.Num(); ++B)
				{
					PairDistances.Add(FVector::Dist(Living[A], Living[B]));
					Increment(Report, TEXT("pair_distance_sum"), PairDistances.Last());
					Increment(Report, TEXT("pair_samples"));
					for (int32 C = B + 1; C < Living.Num(); ++C)
						Clustered |= FVector::DistSquared(Living[A], Living[B]) <= 360000
							&& FVector::DistSquared(Living[A], Living[C]) <= 360000
							&& FVector::DistSquared(Living[B], Living[C]) <= 360000;
				}
			Increment(Report, TEXT("cluster_samples"));
			Increment(Report, TEXT("clustered_samples"), Clustered ? 1 : 0);
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			const FVector Location = A->GetActorLocation(), EngineVelocity = A->GetVelocity();
			FVector Velocity = WoTSoakEvents::SimulationVelocity(A);
			if (Finite(Velocity) && (!Finite(EngineVelocity) || EngineVelocity.SizeSquared() > Velocity.SizeSquared())) Velocity = EngineVelocity;
			const bool bFinite = Finite(Location) && Finite(Velocity);
			const APhysicsVolume* Zone = A->GetPhysicsVolume();
			const double Bound = 10.0 * FMath::Max(1.0f, Zone ? Zone->TerminalVelocity : 4000.0f);
			if (bFinite && Velocity.Size() <= Bound) continue;
			auto& Seen = bFinite ? RunawayActors : NonFiniteActors;
			if (Seen.Contains(A)) continue;
			Seen.Add(A);
			Increment(Report, bFinite ? TEXT("runaway") : TEXT("non_finite"));
			FObject Event = Object();
			Event->SetStringField(TEXT("actor"), A->GetName()); Event->SetStringField(TEXT("class"), A->GetClass()->GetPathName());
			Event->SetStringField(TEXT("kind"), bFinite ? TEXT("runaway") : TEXT("non_finite"));
			Event->SetNumberField(TEXT("first_seconds"), Time);
			Event->SetStringField(TEXT("position"), Location.ToString()); Event->SetStringField(TEXT("velocity"), Velocity.ToString());
			Event->SetNumberField(TEXT("speed_bound"), Bound);
			Anomalies.Add(Value(Event));
		}
		const AWoTGameMode* G = World->GetAuthGameMode<AWoTGameMode>();
		if (G && G->bGameEnded && !Report->GetBoolField(TEXT("frag_limit_end")))
		{
			bool bReached = false;
			for (const auto& Pair : Bots) bReached |= G->FragLimit > 0 && Number(Pair.Value,TEXT("kills")) - Number(Pair.Value,TEXT("suicides")) >= G->FragLimit;
			Report->SetBoolField(TEXT("frag_limit_end"), bReached);
			Report->SetNumberField(TEXT("match_end_seconds"), Time);
		}
		if (G && !G->PendingMapURL.IsEmpty() && !Report->GetBoolField(TEXT("restart_requested")))
		{
			Report->SetBoolField(TEXT("restart_requested"), true);
			Report->SetNumberField(TEXT("restart_request_seconds"), Time);
			Report->SetStringField(TEXT("restart_url"), G->PendingMapURL);
		}
	}
	void Snapshot()
	{
		Report->SetNumberField(TEXT("simulated_seconds"), Time);
		int32 Kills = 0, Deaths = 0, Unvisited = 0;
		TArray<TSharedPtr<FJsonValue>> BotRows, PoolRows;
		for (const auto& P : Bots) { BotRows.Add(Value(P.Value)); Kills += int32(Number(P.Value,TEXT("kills"))); Deaths += int32(Number(P.Value,TEXT("deaths"))); }
		for (const auto& P : Pools) { PoolRows.Add(Value(P.Value)); Unvisited += Number(P.Value,TEXT("collected")) == 0; }
		Report->SetArrayField(TEXT("bots"), BotRows); Report->SetArrayField(TEXT("pools"), PoolRows);
		Report->SetArrayField(TEXT("actor_anomalies"), Anomalies); Report->SetArrayField(TEXT("recovery_events"), Recoveries);
		Report->SetNumberField(TEXT("kills"), Kills); Report->SetNumberField(TEXT("deaths"), Deaths);
		Report->SetNumberField(TEXT("unvisited_pools"), Unvisited);
		double Pickups = 0;
		for (const auto& P : Bots) Pickups += Number(P.Value, TEXT("pickups"));
		Report->SetNumberField(TEXT("pickups_per_bot_minute"), Pickups * 60 / FMath::Max(1.0, Number(Report, TEXT("bot_seconds"))));
		Report->SetNumberField(TEXT("unique_pools_visited"), VisitedPools.Num());
		Report->SetNumberField(TEXT("unique_pools_collected"), Pools.Num() - Unvisited);
		Report->SetNumberField(TEXT("cluster_share"), Number(Report, TEXT("clustered_samples")) / FMath::Max(1.0, Number(Report, TEXT("cluster_samples"))));
		Report->SetNumberField(TEXT("mean_pair_distance"), Number(Report, TEXT("pair_distance_sum")) / FMath::Max(1.0, Number(Report, TEXT("pair_samples"))));
		if (!PairDistances.IsEmpty())
		{
			// A few bodies far from the others dominate the mean; the median is robust to them.
			TArray<double> Sorted = PairDistances; Sorted.Sort();
			Report->SetNumberField(TEXT("median_pair_distance"), Sorted[Sorted.Num() / 2]);
		}
		if (!TickTimes.IsEmpty())
		{
			TArray<double> Sorted = TickTimes; Sorted.Sort(); double Total = 0; int32 Slow = 0;
			for (double T : Sorted) { Total += T; Slow += T > 1000.0 / 30.0; }
			Report->SetNumberField(TEXT("tick_avg_ms"), Total / Sorted.Num());
			Report->SetNumberField(TEXT("tick_p95_ms"), Sorted[FMath::CeilToInt(.95 * Sorted.Num()) - 1]);
			Report->SetNumberField(TEXT("tick_max_ms"), Sorted.Last());
			Report->SetNumberField(TEXT("slow_ticks"), Slow);
		}
		Logs.Snapshot(Report);
	}
};

bool RunMap(const FObject& Report, const FString& Output, FString& Error)
{
	FLogCapture Logs;
	const double Started = FPlatformTime::Seconds();
	ON_SCOPE_EXIT { Logs.Snapshot(Report); Report->SetNumberField(TEXT("worker_wall_seconds"), FPlatformTime::Seconds()-Started); Save(Report, Output); };
	FMapSet Maps;
	const FString Map = Report->GetStringField(TEXT("map"));
	if (!Maps.Load(Error)) return false;
	const FWoTWorldDescriptor* D = Maps.Descriptors.FindByPredicate([&](const auto& Item) { return Item.MapIdentity == Map; });
	if (!D) { Error = TEXT("Requested Arena map is absent from imported contract"); return false; }
	WoTTest::FSpecWorldOptions Options; Options.bStartPlay = false; Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	UWorld* W = WoTTest::CreateSpecWorld(Options);
	if (!W) { Error = TEXT("Could not create world"); return false; }
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); CollectGarbage(RF_NoFlags); };
	FSoakServer Server(W);
	if (W->GetNetMode() != NM_DedicatedServer) { Error = TEXT("Worker requires dedicated server gameplay semantics"); return false; }
	Report->SetStringField(TEXT("net_mode"), TEXT("dedicated_server"));
	WoTRandom::Seed(int32(Number(Report,TEXT("seed"))));
	FObserver Observer(W, Report, Logs);
	if (!Materialize(W, Maps, *D, Error)) return false;
	UNavigationSystemConfig* Config = NewObject<UNavigationSystemConfig>(W->GetWorldSettings());
	Config->NavigationSystemClass = FSoftClassPath(UWoTBotNavigationSystem::StaticClass());
	W->GetWorldSettings()->SetNavigationSystemConfigOverride(Config);
	FNavigationSystem::AddNavigationSystemToWorld(*W, FNavigationSystemRunMode::GameMode, Config);
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
	if (!Nav) { Error = TEXT("Navigation system unavailable"); return false; }
	const double NavStart = FPlatformTime::Seconds();
	Nav->Build();
	int32 Agents = 0;
	TArray<TSharedPtr<FJsonValue>> Meshes;
	for (ANavigationData* Data : Nav->NavDataSet)
	{
		if (!Data) continue;
		Data->EnsureBuildCompletion(); ++Agents;
		const ARecastNavMesh* Mesh = Cast<ARecastNavMesh>(Data);
		int32 Populated = 0, Polygons = 0;
		if (Mesh)
		{
			TArray<FNavTileRef> Tiles; Mesh->GetAllNavMeshTiles(Tiles);
			for (FNavTileRef Tile : Tiles)
			{
				TArray<FNavPoly> Polys; Mesh->GetPolysInTile(Tile,Polys);
				Populated += !Polys.IsEmpty(); Polygons += Polys.Num();
			}
		}
		FObject Row = Object(); Row->SetStringField(TEXT("class"), Data->GetClass()->GetPathName());
		Row->SetNumberField(TEXT("populated_tiles"), Populated); Row->SetNumberField(TEXT("polygons"), Polygons);
		Meshes.Add(Value(Row));
		if (!Populated) { Error = TEXT("Navigation agent has no populated tiles"); return false; }
	}
	Report->SetArrayField(TEXT("navigation_meshes"), Meshes);
	Report->SetNumberField(TEXT("navigation_agents"), Agents);
	Report->SetNumberField(TEXT("navigation_build_ms"), 1000 * (FPlatformTime::Seconds() - NavStart));
	if (Agents != 2) { Error = TEXT("Expected both navigation agents"); return false; }
	W->GetWorldSettings()->DefaultGameMode = AWoTGameMode::StaticClass();
	const FString URLText = FString::Printf(TEXT("/Engine/Maps/Entry?BotFill=%d"), int32(Number(Report,TEXT("fill"))));
	FURL URL(nullptr, *URLText, TRAVEL_Absolute);
	if (!W->SetGameMode(URL)) { Error = TEXT("Could not create Arena rules"); return false; }
	AWoTGameMode* Game = W->GetAuthGameMode<AWoTGameMode>();
	Game->SetInstalledMapsForTesting({Map});
	for (TActorIterator<AWoTAngrealSpawnPool> It(W); It; ++It) Observer.Pool(*It);
	W->InitializeActorsForPlay(URL);
	W->BeginPlay();
	Observer.Sample();
	const int32 Steps = FMath::CeilToInt(Number(Report,TEXT("requested_seconds")) * 30.0);
	for (int32 Index = 0; Index < Steps; ++Index)
	{
		Observer.Time = (Index + 1) / 30.0; Logs.Time.Store(Observer.Time);
		const double TickStart = FPlatformTime::Seconds();
		W->Tick(LEVELTICK_All, Step); ++GFrameCounter;
		Observer.TickTimes.Add((FPlatformTime::Seconds() - TickStart) * 1000);
		Observer.Sample();
		if ((Index + 1) % 900 == 0)
		{
			Observer.Snapshot();
			if (!Save(Report, Output)) { Error = TEXT("Could not persist worker checkpoint"); return false; }
			UE_LOG(LogTemp, Display, TEXT("BOT_SOAK_PROGRESS map=%s seconds=%.0f kills=%.0f"), *Map, Observer.Time, Number(Report,TEXT("kills")));
		}
	}
	Observer.Snapshot();
	Report->SetBoolField(TEXT("completed"), true);
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSoakPolicySpec, "WoT.Bots.SoakDiagnostics.FailurePolicy", Flags)
bool FBotSoakPolicySpec::RunTest(const FString&)
{
	TestFalse(TEXT("healthy map passes"), Failed(true,0,0,0,1));
	TestTrue(TEXT("zero kills fails"), Failed(true,0,0,0,0));
	TestTrue(TEXT("ensure fails"), Failed(true,1,0,0,1));
	TestTrue(TEXT("error fails"), Failed(true,0,1,0,1));
	TestTrue(TEXT("bad actor fails"), Failed(true,0,0,1,1));
	TestTrue(TEXT("crash/incomplete fails"), Failed(false,0,0,0,1));
	TestFalse(TEXT("infinity rejected"), Finite(FVector(std::numeric_limits<double>::infinity(),0,0)));
	TestFalse(TEXT("NaN rejected"), Finite(FVector(0,std::numeric_limits<double>::quiet_NaN(),0)));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSoakEscalationSpec, "WoT.Bots.SoakDiagnostics.RunawayThenNonFinite", Flags)
bool FBotSoakEscalationSpec::RunTest(const FString&)
{
	UWorld* W = World();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	UClass* FragmentClass = FindObject<UClass>(nullptr,TEXT("/Script/WoT.WoTFragment"));
	AActor* Fragment = W->SpawnActor<AActor>(FragmentClass,FVector(0,0,100),FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("sampled fragment"),Fragment)) return false;
	FStructProperty* Property = FindFProperty<FStructProperty>(FragmentClass,TEXT("Velocity"));
	if (!TestNotNull(TEXT("custom velocity"),Property)) return false;
	FVector& Velocity = *Property->ContainerPtrToValuePtr<FVector>(Fragment);
	FObject R = NewReport(TEXT("diagnostics"),8,180);
	FLogCapture Logs; FObserver Observer(W,R,Logs);
	Velocity = FVector(1000000,0,0); Observer.Time = 1; Observer.Sample();
	Observer.Sample();
	TestEqual(TEXT("runaway recorded once"),int32(Number(R,TEXT("runaway"))),1);
	Velocity.X = std::numeric_limits<double>::infinity(); Observer.Time = 2; Observer.Sample();
	Observer.Sample(); Observer.Snapshot();
	TestEqual(TEXT("later non-finite transition is still sampled"),int32(Number(R,TEXT("non_finite"))),1);
	TestEqual(TEXT("each anomaly kind recorded once"),R->GetArrayField(TEXT("actor_anomalies")).Num(),2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSoakSpec, "WoT.Bots.Soak.AllArenaMaps", Flags)
bool FBotSoakSpec::RunTest(const FString&)
{
	if (FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_BOT_SOAK")) != TEXT("1"))
	{
		AddInfo(TEXT("SKIPPED: opt-in bot soak; set WOT_BOT_SOAK=1 to run all Arena maps (default 180 seconds/map, BotFill=8)."));
		return true;
	}
	int32 Fill = 8; double Seconds = 180;
	const FString FillText = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_BOT_SOAK_FILL"));
	const FString SecondsText = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_BOT_SOAK_SECONDS"));
	if ((!FillText.IsEmpty() && !LexTryParseString(Fill,*FillText)) || Fill < 1 || Fill > 16
		|| (!SecondsText.IsEmpty() && !LexTryParseString(Seconds,*SecondsText)) || !FMath::IsFinite(Seconds) || Seconds <= 0 || Seconds > 86400)
	{ AddError(TEXT("Invalid soak settings: fill must be 1..16 and seconds must be finite in (0,86400].")); return false; }
	FString WorkerMap, WorkerOutput;
	if (FParse::Value(FCommandLine::Get(), TEXT("WotBotSoakMap="), WorkerMap))
	{
		if (!FParse::Value(FCommandLine::Get(), TEXT("WotBotSoakOutput="), WorkerOutput)) { AddError(TEXT("Worker requires WotBotSoakOutput")); return false; }
		FObject R = NewReport(WorkerMap, Fill, Seconds); FString Error;
		if (!Save(R,WorkerOutput)) { AddError(TEXT("Cannot create worker report")); return false; }
		const bool bComplete = RunMap(R,WorkerOutput,Error);
		const bool bFailed = Failed(bComplete,int32(Number(R,TEXT("ensures"))),int32(Number(R,TEXT("errors"))),int32(Number(R,TEXT("non_finite"))+Number(R,TEXT("runaway"))),int32(Number(R,TEXT("kills"))));
		R->SetStringField(TEXT("result"), bFailed ? TEXT("FAIL") : TEXT("PASS"));
		R->SetStringField(TEXT("setup_error"), Error);
		if (!Save(R,WorkerOutput)) AddError(TEXT("Cannot save worker report"));
		if (bFailed) AddError(FString::Printf(TEXT("Soak map failed: %s; %s; see %s"), *WorkerMap,*Error,*WorkerOutput));
		return !bFailed && !HasAnyErrors();
	}
	const double Started = FPlatformTime::Seconds();
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("BotSoak") / FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")));
	IFileManager::Get().MakeDirectory(*Directory,true);
	AddInfo(FString::Printf(TEXT("Bot soak reports: %s"), *Directory));
	const auto Contract = WoTLoadProductionRetailContract();
	if (Contract.IsError()) { AddError(Contract.GetError().ToString()); return false; }
	TArray<FString> Names;
	for (const auto& Fact : Contract.GetValue().Facts)
		if (Fact.Domain == EWoTFactDomain::World && FPaths::GetBaseFilename(Fact.SourcePath).StartsWith(TEXT("Arena_"))) Names.AddUnique(FPaths::GetBaseFilename(Fact.SourcePath));
	Names.Sort();
	if (Names.IsEmpty()) { AddError(TEXT("No Arena maps in production contract")); return false; }
	TArray<FObject> Reports;
	for (const FString& Map : Names)
	{
		const FString Output = Directory / (Map + TEXT(".json"));
		const FString Log = Directory / (Map + TEXT(".log"));
		const FString Args = FString::Printf(TEXT("\"%s\" \"-ExecCmds=Automation RunTests WoT.Bots.Soak.AllArenaMaps; Quit\" -unattended -nullrhi -nosplash -nopause -nocrashreports -stdout -abslog=\"%s\" -WotBotSoakMap=\"%s\" -WotBotSoakOutput=\"%s\""),
			*FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), *Log, *Map, *Output);
		UE_LOG(LogTemp, Display, TEXT("BOT_SOAK_START map=%s seed=%d log=%s"), *Map, MapSeed(Map), *Log);
		FProcHandle Process = FPlatformProcess::CreateProc(FPlatformProcess::ExecutablePath(), *Args, false, true, true, nullptr, 0, nullptr, nullptr);
		int32 ExitCode = -1; bool bTimedOut = false;
		const double WorkerStart = FPlatformTime::Seconds();
		if (Process.IsValid())
		{
			while (FPlatformProcess::IsProcRunning(Process))
			{
				if (FPlatformTime::Seconds() - WorkerStart > FMath::Max(1200.0, Seconds*10))
				{ bTimedOut = true; FPlatformProcess::TerminateProc(Process,true); break; }
				FPlatformProcess::Sleep(.1f);
			}
			FPlatformProcess::WaitForProc(Process);
			FPlatformProcess::GetProcReturnCode(Process,&ExitCode);
			FPlatformProcess::CloseProc(Process);
		}
		FObject R = NewReport(Map,Fill,Seconds); FString Json;
		TSharedPtr<FJsonObject> Loaded;
		if (FFileHelper::LoadFileToString(Json,*Output) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),Loaded) && Loaded.IsValid()) R = Loaded.ToSharedRef();
		R->SetNumberField(TEXT("exit_code"),ExitCode); R->SetBoolField(TEXT("timed_out"),bTimedOut);
		R->SetStringField(TEXT("worker_log"),Log);
		R->SetNumberField(TEXT("process_wall_seconds"),FPlatformTime::Seconds()-WorkerStart);
		if (ExitCode != 0 || bTimedOut || R->GetStringField(TEXT("result")) != TEXT("PASS")) R->SetStringField(TEXT("result"),TEXT("FAIL"));
		Reports.Add(R);
		FObject Combined = Object(); TArray<TSharedPtr<FJsonValue>> Rows;
		for (const auto& Item : Reports) Rows.Add(Value(Item));
		Combined->SetArrayField(TEXT("maps"),Rows);
		Combined->SetNumberField(TEXT("wall_seconds"),FPlatformTime::Seconds()-Started);
		if (!Save(Combined,Directory/TEXT("report.json")) || !FFileHelper::SaveStringToFile(Markdown(Reports,FPlatformTime::Seconds()-Started),*(Directory/TEXT("report.md")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			AddError(TEXT("Could not write aggregate soak report"));
		AddInfo(SummaryRow(R));
	}
	for (const auto& R : Reports) if (R->GetStringField(TEXT("result")) != TEXT("PASS")) AddError(FString::Printf(TEXT("Soak failed on %s; evidence: %s"),*R->GetStringField(TEXT("map")),*Directory));
	AddInfo(FString::Printf(TEXT("Bot soak finished in %.2f seconds; %s"),FPlatformTime::Seconds()-Started,*(Directory/TEXT("report.md"))));
	return !HasAnyErrors();
}
} // namespace WoTBotSoak
#endif
