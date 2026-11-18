#include "WoTBotRoster.h"
#include "WoTBotController.h"
#include "WoTBotFactionProfile.h"
#include "WoTBotSettings.h"
#include "WoTCharacter.h"
#include "WoTBattleGameMode.h"
#include "WoTFrontEnd.h"
#include "WoTGameMode.h"
#include "WoTPlayerState.h"
#include "WoTSessionSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "WoTRandom.h"
#include "Inventory/WoTAngrealSpawnPool.h"
#include "EngineUtils.h"

void UWoTBotRoster::ReleasePool(AWoTBotController* Bot)
{
	PoolClaims.Remove(Bot);
}

void UWoTBotRoster::ClaimPool(AWoTBotController* Bot, AWoTAngrealSpawnPool* Pool)
{
	for (auto It = PoolClaims.CreateIterator(); It; ++It)
		if (!It.Key().IsValid() || !It.Value().IsValid()) It.RemoveCurrent();
	ReleasePool(Bot);
	if (Bot && Pool) PoolClaims.Add(Bot, Pool);
}

bool UWoTBotRoster::IsPoolClaimed(const AWoTAngrealSpawnPool* Pool, const AWoTBotController* Except) const
{
	for (const auto& Claim : PoolClaims)
		if (Claim.Key.IsValid() && Claim.Key.Get() != Except && Claim.Value.Get() == Pool) return true;
	return false;
}

void UWoTBotRoster::CountFighters(const AWoTBotController* Except, TMap<const AWoTCharacter*, int32>& Counts) const
{
	Counts.Reset();
	// Every bot controller in the world, including any not added by this roster.
	for (TActorIterator<AWoTBotController> It(GetWorld()); It; ++It)
		if (*It != Except && !It->IsDeparturePending() && It->GetBehaviourState() == EWoTBotState::Engage
			&& It->GetTarget() && It->GetPawn() && CastChecked<AWoTCharacter>(It->GetPawn())->IsAlive())
			++Counts.FindOrAdd(It->GetTarget());
}

int32 UWoTBotRoster::FightersFor(const AWoTCharacter* Target, const AWoTBotController* Except) const
{
	TMap<const AWoTCharacter*, int32> Counts;
	CountFighters(Except, Counts);
	return Counts.FindRef(Target);
}

void UWoTBotRoster::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LoginHandle = FGameModeEvents::GameModePostLoginEvent.AddUObject(this, &ThisClass::PostLogin);
	LogoutHandle = FGameModeEvents::GameModeLogoutEvent.AddUObject(this, &ThisClass::Logout);
	MatchHandle = FGameModeEvents::GameModeMatchStateSetEvent.AddUObject(this, &ThisClass::MatchStateChanged);
}

void UWoTBotRoster::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (InWorld.GetNetMode() == NM_Client) return;
	AWoTGameMode* Game = InWorld.GetAuthGameMode<AWoTGameMode>();
	bActive = Game && !Game->IsA<AWoTBattleGameMode>();
	PublishCount();
	if (!bActive) return;
	FillTarget = UGameplayStatics::HasOption(Game->OptionsString, TEXT("BotFill"))
		? UGameplayStatics::GetIntOption(Game->OptionsString, TEXT("BotFill"), 0)
		: GetDefault<UWoTBotSettings>()->DefaultBotFill;
	FillTarget = FMath::Clamp(FillTarget, 0, 16);
	NextAddTime = InWorld.GetTimeSeconds() + FMath::Max(.01f, GetDefault<UWoTBotSettings>()->AddIntervalSeconds);
	InWorld.GetTimerManager().SetTimer(EvaluationTimer, this, &ThisClass::Evaluate, 2.0f, true);
	Evaluate();
}

void UWoTBotRoster::Deinitialize()
{
	bTearingDown = true;
	FGameModeEvents::GameModePostLoginEvent.Remove(LoginHandle);
	FGameModeEvents::GameModeLogoutEvent.Remove(LogoutHandle);
	FGameModeEvents::GameModeMatchStateSetEvent.Remove(MatchHandle);
	if (GetWorld()) GetWorld()->GetTimerManager().ClearAllTimersForObject(this);
	Bots.Reset();
	PoolClaims.Reset();
	Departures.Reset();
	Super::Deinitialize();
}

int32 UWoTBotRoster::HumanCount() const
{
	int32 Count = 0;
	if (const AGameStateBase* State = GetWorld()->GetGameState())
	{
		for (const APlayerState* Player : State->PlayerArray)
			if (IsValid(Player) && !Player->IsABot() && !Player->IsInactive() && !ExitingStates.Contains(Player)) ++Count;
	}
	return Count;
}

int32 UWoTBotRoster::GetBotCount() const
{
	int32 Count = 0;
	for (const auto& Bot : Bots) if (Bot.IsValid()) ++Count;
	return Count;
}

int32 UWoTBotRoster::GetDesiredBotCount() const
{
	return bActive ? FMath::Max(0, FillTarget - HumanCount()) : 0;
}

AWoTBotController* UWoTBotRoster::RetirementCandidate(bool bUnmarkedOnly) const
{
	AWoTBotController* Best = nullptr;
	for (const auto& Entry : Bots)
	{
		AWoTBotController* Bot = Entry.Get();
		if (!Bot || (bUnmarkedOnly && Bot->IsDeparturePending())) continue;
		const auto Score = [](AWoTBotController* Controller)
		{
			const AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>();
			return State ? State->CalcScore() : 0;
		};
		if (!Best || (Best->GetPawn() && !Bot->GetPawn())
			|| (!!Best->GetPawn() == !!Bot->GetPawn() && Score(Bot) < Score(Best))) Best = Bot;
	}
	return Best;
}

void UWoTBotRoster::Evaluate()
{
	if (!bActive || bFrozen || bTearingDown || bEvaluating) return;
	AWoTGameMode* Game = GetWorld()->GetAuthGameMode<AWoTGameMode>();
	if (!Game || Game->bGameEnded) { Freeze(); return; }
	TGuardValue<bool> Guard(bEvaluating, true);
	Bots.RemoveAll([](const auto& Bot) { return !Bot.IsValid(); });
	for (auto It = ExitingStates.CreateIterator(); It; ++It)
	{
		if (!It->IsValid() || !GetWorld()->GetGameState()->PlayerArray.Contains(It->Get())) It.RemoveCurrent();
	}
	while (GetBotCount() + HumanCount() > 16)
	{
		AWoTBotController* Bot = RetirementCandidate(false);
		if (!Bot) break;
		Retire(Bot);
	}
	ReconcileDepartures();
	FTimerManager& Timers = GetWorld()->GetTimerManager();
	if (GetBotCount() < GetDesiredBotCount())
	{
		if (!Timers.IsTimerActive(AddTimer))
			Timers.SetTimer(AddTimer, this, &ThisClass::AddBot,
				FMath::Max(.001f, float(NextAddTime - GetWorld()->GetTimeSeconds())));
	}
	else Timers.ClearTimer(AddTimer);
	QueueCount();
}

void UWoTBotRoster::ReconcileDepartures()
{
	Departures.RemoveAll([](const FDeparture& Departure) { return !Departure.Bot.IsValid(); });
	int32 Excess = FMath::Max(0, GetBotCount() - GetDesiredBotCount());
	while (Departures.Num() > Excess)
	{
		Departures.Last().Bot->SetDeparturePending(false);
		Departures.Pop();
	}
	// A death or a join may have made an already marked participant removable.
	for (int32 Index = Departures.Num() - 1; Index >= 0; --Index)
	{
		AWoTBotController* Bot = Departures[Index].Bot.Get();
		if (Bot && !Bot->GetPawn()) { Retire(Bot); --Excess; }
	}
	while (Excess > Departures.Num())
	{
		AWoTBotController* Bot = RetirementCandidate(true);
		if (!Bot) break;
		if (!Bot->GetPawn()) { Retire(Bot); --Excess; }
		else
		{
			Bot->SetDeparturePending(true);
			Departures.Add({Bot, GetWorld()->GetTimeSeconds() + FMath::Max(.01f, GetDefault<UWoTBotSettings>()->LeaveTimeoutSeconds)});
		}
	}
	ArmDepartureTimer();
}

void UWoTBotRoster::ArmDepartureTimer()
{
	FTimerManager& Timers = GetWorld()->GetTimerManager();
	Timers.ClearTimer(DepartureTimer);
	if (Departures.IsEmpty() || bFrozen) return;
	double Earliest = Departures[0].Deadline;
	for (const FDeparture& Departure : Departures) Earliest = FMath::Min(Earliest, Departure.Deadline);
	Timers.SetTimer(DepartureTimer, this, &ThisClass::DepartureDue,
		FMath::Max(.001f, float(Earliest - GetWorld()->GetTimeSeconds())));
}

void UWoTBotRoster::DepartureDue()
{
	Evaluate(); // Revalidate human count and cancel newest marks before any departure.
	if (bFrozen || bTearingDown) return;
	const auto Pending = Departures;
	for (const FDeparture& Departure : Pending)
	{
		if (Departure.Bot.IsValid() && Departure.Bot->IsDeparturePending()
			&& Departure.Deadline <= GetWorld()->GetTimeSeconds()
			&& GetBotCount() > GetDesiredBotCount()) Retire(Departure.Bot.Get());
	}
	Evaluate();
}

void UWoTBotRoster::ParticipantDied(AWoTBotController*)
{
	Evaluate();
}

void UWoTBotRoster::Retire(AWoTBotController* Bot)
{
	if (!IsValid(Bot) || bFrozen || bTearingDown) return;
	Bots.Remove(Bot);
	Departures.RemoveAll([Bot](const FDeparture& Departure) { return Departure.Bot == Bot; });
	Bot->Retire();
	QueueCount();
}

UClass* UWoTBotRoster::DealClass()
{
	if (ClassDeck.IsEmpty())
	{
		for (const FWoTFactionChoice& Choice : FWoTFrontEndModel::FactionChoices())
		{
			UClass* Class = AWoTGameMode::ResolvePlayerClassOption(TEXT("?Class=") + Choice.ClassOption);
			if (Class && !WoTBotProfiles::DisplayName(Class).IsEmpty()) ClassDeck.Add(Class);
		}
		for (int32 Index = ClassDeck.Num() - 1; Index > 0; --Index) ClassDeck.Swap(Index, WoTRandom::RandRange(0, Index));
	}
	return ClassDeck.IsEmpty() ? nullptr : ClassDeck.Pop().Get();
}

FString UWoTBotRoster::NextName(UClass* PlayerClass) const
{
	const FString Base = WoTBotProfiles::DisplayName(PlayerClass);
	for (int32 Number = 1; ; ++Number)
	{
		const FString Candidate = FString::Printf(TEXT("%s %d"), *Base, Number);
		bool bTaken = false;
		if (const AGameStateBase* State = GetWorld()->GetGameState())
		{
			for (const APlayerState* Player : State->PlayerArray)
				if (Player && Player->GetPlayerName().Equals(Candidate, ESearchCase::IgnoreCase)) { bTaken = true; break; }
		}
		if (!bTaken) return Candidate;
	}
}

void UWoTBotRoster::AddBot()
{
	// A one-shot timer remains active during its callback. Allow Evaluate to arm
	// the next paced addition immediately instead of waiting for its 2 s poll.
	GetWorld()->GetTimerManager().ClearTimer(AddTimer);
	if (!bActive || bFrozen || bTearingDown) return;
	AWoTGameMode* Game = GetWorld()->GetAuthGameMode<AWoTGameMode>();
	if (!Game || Game->bGameEnded) { Freeze(); return; }
	NextAddTime = GetWorld()->GetTimeSeconds() + FMath::Max(.01f, GetDefault<UWoTBotSettings>()->AddIntervalSeconds);
	if (GetBotCount() < GetDesiredBotCount())
	{
		if (UClass* Class = DealClass())
		{
			AWoTBotController* Bot = GetWorld()->SpawnActor<AWoTBotController>();
			if (Bot && Bot->GetPlayerState<AWoTPlayerState>())
			{
				Bot->InitializeIdentity(this, Class, NextName(Class));
				Bots.Add(Bot);
				Game->ChangeTeam(Bot, 255);
				Bot->Restart();
			}
			else if (Bot) Bot->Destroy();
		}
	}
	Evaluate();
}

void UWoTBotRoster::PostLogin(AGameModeBase* GameMode, APlayerController*)
{
	if (GameMode && GameMode->GetWorld() == GetWorld()) Evaluate();
}

void UWoTBotRoster::Logout(AGameModeBase* GameMode, AController* Exiting)
{
	if (!GameMode || GameMode->GetWorld() != GetWorld() || bTearingDown) return;
	// The event precedes player-state removal, so do not count the exiting human.
	if (Exiting && Exiting->PlayerState) ExitingStates.Add(Exiting->PlayerState.Get());
	if (AWoTBotController* Bot = Cast<AWoTBotController>(Exiting))
	{
		Bots.Remove(Bot);
		Departures.RemoveAll([Bot](const FDeparture& Departure) { return Departure.Bot == Bot; });
	}
	Evaluate();
}

void UWoTBotRoster::MatchStateChanged(FName)
{
	Evaluate();
}

void UWoTBotRoster::Freeze()
{
	if (bFrozen) return;
	bFrozen = true;
	FTimerManager& Timers = GetWorld()->GetTimerManager();
	Timers.ClearTimer(EvaluationTimer);
	Timers.ClearTimer(AddTimer);
	Timers.ClearTimer(DepartureTimer);
	for (const auto& Bot : Bots) if (Bot.IsValid()) Bot->OnMatchEnded();
}

void UWoTBotRoster::QueueCount()
{
	if (LastPublishedCount == GetBotCount() || bTearingDown) return;
	FTimerManager& Timers = GetWorld()->GetTimerManager();
	if (Timers.IsTimerActive(PublishTimer)) return;
	const float Delay = float(LastPublishTime + 2.0 - GetWorld()->GetTimeSeconds());
	if (Delay <= 0) PublishCount();
	else Timers.SetTimer(PublishTimer, this, &ThisClass::PublishCount, Delay);
}

void UWoTBotRoster::PublishCount()
{
	if (bTearingDown || GetWorld()->GetNetMode() == NM_Client) return;
	UGameInstance* Instance = GetWorld()->GetGameInstance();
	// A surviving timer from an outgoing world cannot overwrite its successor.
	if (!Instance || Instance->GetWorld() != GetWorld()) return;
	if (UWoTSessionSubsystem* Sessions = Instance->GetSubsystem<UWoTSessionSubsystem>())
	{
		const int32 Count = bActive ? GetBotCount() : 0;
		if (Count != LastPublishedCount)
		{
			Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), Count);
			LastPublishedCount = Count;
			LastPublishTime = GetWorld()->GetTimeSeconds();
		}
	}
}
