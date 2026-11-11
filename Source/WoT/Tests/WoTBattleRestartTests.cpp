// Restarts never strand a player (rated-start fallback, the live pawn kept until
// its replacement exists), the listen host's pre-battle team select, and the
// retail citadel edit refusals.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTGameMode.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerState.h"
#include "../WoTRetailText.h"
#include "../WoTZoneVolume.h"
#include "WoTTestBattleClasses.h"

#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

namespace
{
constexpr EAutomationTestFlags WoTBattleRestartSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters WoTBattleRestartAlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

// Level geometry stands in as a StaticMeshActor, as the editor maps' BSP meshes
// are: FindSpot traces and fits against the level only.
AStaticMeshActor* WoTBattleRestartSpawnFloor(UWorld& World)
{
	AStaticMeshActor* Floor = World.SpawnActor<AStaticMeshActor>(
		FVector(0, 0, -10), FRotator::ZeroRotator, WoTBattleRestartAlwaysSpawn());
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("RestartFloorBox"));
	Box->SetBoxExtent(FVector(6000, 6000, 10));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->SetupAttachment(Floor->GetRootComponent());
	Box->RegisterComponent();
	return Floor;
}

AWoTPlayerStart* WoTBattleRestartSpawnStart(UWorld& World, const FVector& Location,
	uint8 Team)
{
	AWoTPlayerStart* Start = World.SpawnActor<AWoTPlayerStart>(
		Location, FRotator::ZeroRotator, WoTBattleRestartAlwaysSpawn());
	if (Start) Start->TeamNumber = Team;
	return Start;
}

// A blocking actor seated on a start: the placed pawn would encroach it.
AWoTCollisionBlocker* WoTBattleRestartBlockStart(UWorld& World, const AActor* Start)
{
	const FTransform Transform(Start->GetActorLocation());
	AWoTCollisionBlocker* Blocker = World.SpawnActorDeferred<AWoTCollisionBlocker>(
		AWoTCollisionBlocker::StaticClass(), Transform);
	Blocker->Configure(EWoTCollisionBlockerMode::Actor, 40.0f, 60.0f);
	Blocker->FinishSpawning(Transform);
	return Blocker;
}

AWoTPlayerController* WoTBattleRestartSpawnPlayer(UWorld& World, uint8 Team)
{
	AWoTPlayerController* Controller =
		World.SpawnActor<AWoTPlayerController>(WoTBattleRestartAlwaysSpawn());
	if (!Controller) return nullptr;
	Controller->InitPlayerState();
	if (AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>())
		State->Team = Team;
	return Controller;
}

bool WoTBattleRestartSeatedAt(const APawn* Pawn, const AActor* Start)
{
	return Pawn && Start
		&& FVector::Dist2D(Pawn->GetActorLocation(), Start->GetActorLocation()) < 1.0;
}
}

// GameInfo.RestartPlayer keeps retail's first choice; when that start refuses the
// pawn, the next rated start is tried, for a login and for a death respawn alike.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRestartTriesRatedStartsSpec,
	"WoT.GameMode.RestartTriesRatedStarts", WoTBattleRestartSpecFlags)

bool FWoTRestartTriesRatedStartsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!TestNotNull(TEXT("game mode"), Game)) return false;
	Game->bSpawnInTeamArea = true;
	WoTBattleRestartSpawnFloor(*World);
	// The player's own team start always out-rates the enemy one (-20000).
	AWoTPlayerStart* Own = WoTBattleRestartSpawnStart(*World, FVector(0, 0, 48), 0);
	AWoTPlayerStart* Enemy = WoTBattleRestartSpawnStart(*World, FVector(2500, 0, 48), 1);
	AWoTCollisionBlocker* OwnBlocker = WoTBattleRestartBlockStart(*World, Own);
	AWoTPlayerController* Controller = WoTBattleRestartSpawnPlayer(*World, 0);
	if (!TestNotNull(TEXT("player"), Controller) || !TestNotNull(TEXT("starts"), Own)
		|| !TestNotNull(TEXT("enemy start"), Enemy)) return false;

	TArray<AActor*> Ranked;
	bool bAboveFloor = false;
	Game->RankPlayerStarts(Controller, Ranked, &bAboveFloor);
	TestTrue(TEXT("both starts are rated, own team first"),
		Ranked.Num() == 2 && Ranked[0] == Own && Ranked[1] == Enemy && bAboveFloor);
	TestEqual(TEXT("retail's first choice is unchanged"),
		Game->ChoosePlayerStart(Controller), static_cast<AActor*>(Own));

	FWoTStartSpawnResult Refused;
	APawn* Probe = AWoTGameMode::SpawnPawnAtRetailStart(*World,
		AWoTCharacter::StaticClass(), FTransform(Own->GetActorLocation()), nullptr,
		true, &Refused);
	TestNull(TEXT("the blocked start refuses the pawn"), Probe);
	TestEqual(TEXT("it is refused for encroachment"),
		FString(WoTStartSpawnRejectionName(Refused.Rejection)),
		FString(WoTStartSpawnRejectionName(EWoTStartSpawnRejection::Encroaches)));
	TestEqual(TEXT("the encroached actor is named"), Refused.Blocker, OwnBlocker->GetName());

	// Login: the refused first choice falls back to the next rated start.
	Game->RestartPlayer(Controller);
	APawn* Joined = Controller->GetPawn();
	TestTrue(TEXT("a login whose first start refuses seats at the next rated start"),
		WoTBattleRestartSeatedAt(Joined, Enemy));

	// Death respawn (PlayerPawn.ServerReStartPlayer) falls back the same way.
	Controller->UnPossess();
	if (Joined) Joined->Destroy();
	Controller->ArmRespawnFire(nullptr, 0.0f);
	Controller->ServerRespawnPlayer_Implementation();
	APawn* Respawned = Controller->GetPawn();
	TestTrue(TEXT("a death respawn whose first start refuses seats at the next start"),
		WoTBattleRestartSeatedAt(Respawned, Enemy));
	TestEqual(TEXT("the respawned player is playing"),
		static_cast<int32>(Controller->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::Playing));

	// Only when every start refuses does the respawn fail (and Fire retries it).
	Controller->UnPossess();
	if (Respawned) Respawned->Destroy();
	WoTBattleRestartBlockStart(*World, Enemy);
	Controller->ArmRespawnFire(nullptr, 0.0f);
	Controller->ServerRespawnPlayer_Implementation();
	TestNull(TEXT("with every start refusing there is no pawn"), Controller->GetPawn());
	TestEqual(TEXT("the player waits to retry the spawn"),
		static_cast<int32>(Controller->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::WaitingForSpawn));
	return !HasAnyErrors();
}

// PlayerPawn.ServerReStartPlayer on a live player (the team-select restart):
// retail moves the one pawn, and "Restartplayer failed" leaves it where it stood.
// The port keeps the live pawn until a replacement exists, and the hidden,
// collisionless live pawn never blocks its own replacement.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReStartKeepsLivePawnSpec,
	"WoT.GameMode.ReStartKeepsLivePawnUntilReplaced", WoTBattleRestartSpecFlags)

bool FWoTReStartKeepsLivePawnSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!TestNotNull(TEXT("game mode"), Game)) return false;
	Game->bSpawnInTeamArea = true;
	WoTBattleRestartSpawnFloor(*World);
	AWoTPlayerStart* Own = WoTBattleRestartSpawnStart(*World, FVector(0, 0, 48), 0);
	AWoTPlayerStart* Enemy = WoTBattleRestartSpawnStart(*World, FVector(2500, 0, 48), 1);
	WoTBattleRestartBlockStart(*World, Own);
	AWoTCollisionBlocker* EnemyBlocker = WoTBattleRestartBlockStart(*World, Enemy);
	AWoTPlayerController* Controller = WoTBattleRestartSpawnPlayer(*World, 0);
	AWoTCharacter* Live = World->SpawnActor<AWoTCharacter>(
		FVector(1200, 0, 48), FRotator::ZeroRotator, WoTBattleRestartAlwaysSpawn());
	if (!TestNotNull(TEXT("player"), Controller) || !TestNotNull(TEXT("live pawn"), Live))
		return false;
	Controller->Possess(Live);
	const FVector StoodAt = Live->GetActorLocation();

	Controller->ServerRestartPlayerWoT_Implementation();
	TestTrue(TEXT("every start refusing keeps the live pawn possessed"),
		Controller->GetPawn() == Live);
	TestTrue(TEXT("the kept pawn is where it stood"),
		IsValid(Live) && Live->GetActorLocation().Equals(StoodAt, 0.01));
	TestTrue(TEXT("the kept pawn is visible again"), IsValid(Live) && !Live->IsHidden());
	TestTrue(TEXT("the kept pawn collides again"),
		IsValid(Live) && Live->GetActorEnableCollision());
	TestEqual(TEXT("the player keeps playing (no respawn wait)"),
		static_cast<int32>(Controller->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::Playing));

	// Free the enemy start and stand the live pawn on it: hidden and collisionless
	// during the attempt, it must not refuse its own replacement.
	EnemyBlocker->Destroy();
	Live->SetActorLocation(Enemy->GetActorLocation() + FVector(0, 0, 2));
	Controller->ServerRestartPlayerWoT_Implementation();
	APawn* Replacement = Controller->GetPawn();
	TestTrue(TEXT("a free start replaces the live pawn"),
		Replacement && Replacement != Live);
	TestTrue(TEXT("the replacement is seated at the start the old pawn stood on"),
		WoTBattleRestartSeatedAt(Replacement, Enemy));
	TestTrue(TEXT("the old pawn is destroyed once the replacement exists"),
		!IsValid(Live) || Live->IsActorBeingDestroyed());
	TestTrue(TEXT("the replacement is visible"), Replacement && !Replacement->IsHidden());
	return !HasAnyErrors();
}

// giMPBattle.PostBeginPlay -> ResetBattle ran before every Login, so a listen
// host's team select never armed JoinTeamTimeout before the battle began.
// Once EnableTeleporters opens the battle, Timer counts an unteamed player down
// once per second and joins them to the smallest team at 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTListenHostJoinCountdownSpec,
	"WoT.Battle.ListenHostJoinCountdownWaitsForBattle", WoTBattleRestartSpecFlags)

bool FWoTListenHostJoinCountdownSpec::RunTest(const FString&)
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTBattleGameMode::StaticClass();
	Options.bBeginPlay = false;   // LoadMap logs the listen host in before BeginPlay.
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTBattleGameMode* Mode = World ? World->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	if (!TestNotNull(TEXT("Battle mode"), Mode)) return false;
	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	Battle->NumTeams = 2;
	Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	Battle->FinishSpawning(FTransform::Identity);
	TestFalse(TEXT("an unprepared BattleInfo still reads as begun"),
		Battle->bTeleportersDisabled);
	WoTBattleRestartSpawnFloor(*World);
	WoTBattleRestartSpawnStart(*World, FVector(0, 0, 48), 255);

	AWoTPlayerController* Host =
		World->SpawnActor<AWoTPlayerController>(WoTBattleRestartAlwaysSpawn());
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>(WoTBattleRestartAlwaysSpawn());
	if (!TestNotNull(TEXT("host"), Host) || !TestNotNull(TEXT("HUD"), HUD)) return false;
	Host->Player = NewObject<ULocalPlayer>(GEngine);
	Host->InitPlayerState();
	HUD->PlayerOwner = Host;
	Mode->PostLogin(Host);
	const AWoTPlayerState* State = Host->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("host player state"), State)) return false;
	TestEqual(TEXT("the host logs in neutral"), static_cast<int32>(State->Team), 255);
	TestTrue(TEXT("the login runs retail's ResetBattle first: the editing phase is open"),
		Battle->bTeleportersDisabled && !Mode->CanTeleport());
	TestEqual(TEXT("no join countdown is armed before the battle"), Host->JoinTeamTimeout, 0);
	TestEqual(TEXT("the team select shows no time limit"),
		HUD->TeamSelectPrompt(State->Team, Battle->NumTeams, Host->JoinTeamTimeout),
		HUD->TeamSelectPrompt(255, Battle->NumTeams, 0));

	World->BeginPlay();
	for (int32 Second = 0; Second < 5; ++Second) Mode->Timer();
	TestEqual(TEXT("the editing phase never counts the host down"), Host->JoinTeamTimeout, 0);
	TestEqual(TEXT("BeginPlay does not reset the battle a second time"),
		static_cast<int32>(State->Team), 255);

	Mode->EnableTeleporters();
	TestEqual(TEXT("the battle opening arms MaxSpectatorPeriod"),
		Host->JoinTeamTimeout, Mode->MaxSpectatorPeriod);
	TestEqual(TEXT("the team select now shows the time limit"),
		HUD->TeamSelectPrompt(State->Team, Battle->NumTeams, Host->JoinTeamTimeout),
		HUD->TeamSelectPrompt(255, Battle->NumTeams, Mode->MaxSpectatorPeriod));
	for (int32 Second = 1; Second < Mode->MaxSpectatorPeriod; ++Second) Mode->Timer();
	TestEqual(TEXT("one second remains"), Host->JoinTeamTimeout, 1);
	TestEqual(TEXT("still neutral at one second"), static_cast<int32>(State->Team), 255);
	Mode->Timer();
	TestEqual(TEXT("the countdown reaches zero"), Host->JoinTeamTimeout, 0);
	TestEqual(TEXT("zero joins the smallest team"), static_cast<int32>(State->Team), 0);
	return !HasAnyErrors();
}

// WOTPlayer.ServerEditCitadel / EditCitadel (WOTPlayer.uc:4448-4507): each
// refusal tells the player why with the shipped center message.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditRefusalMessagesSpec,
	"WoT.Citadel.EditRefusalMessages", WoTBattleRestartSpecFlags)

bool FWoTCitadelEditRefusalMessagesSpec::RunTest(const FString&)
{
	if (!FWoTRetailText::EnsureLoaded())
	{
		AddWarning(TEXT("retail localization is unavailable; edit refusals not verified"));
		return true;
	}
	auto Retail = [this](const TCHAR* Key) -> FString
	{
		const FString* Value = FWoTRetailText::FindSectionValue(TEXT("WOTPlayer"), Key);
		TestNotNull(FString(Key) + TEXT(" is loaded"), Value);
		return Value ? *Value : FString();
	};
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTBattleGameMode* Mode = World ? World->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	if (!TestNotNull(TEXT("Battle mode"), Mode)) return false;
	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	Battle->NumTeams = 3;
	Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	Battle->TeamClass[2] = AWoTTestTeamAlpha::StaticClass();
	Battle->FinishSpawning(FTransform::Identity);
	Battle->bTeleportersDisabled = true;
	Mode->Battle = Battle;
	// Team 1 has a zone and a budget; team 2 has a zone but no budget.
	auto SpawnZone = [World](const FVector& Center, uint8 Team)
	{
		const FTransform Transform(Center);
		AWoTZoneVolume* Zone = World->SpawnActorDeferred<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), Transform);
		Zone->EditTeam = Team;
		if (IsValid(Zone->Collision)) Zone->Collision->SetBoxExtent(FVector(200));
		Zone->FinishSpawning(Transform);
		return Zone;
	};
	SpawnZone(FVector(0, 0, 0), 1);
	SpawnZone(FVector(5000, 0, 0), 2);
	AWoTBudgetInfo* Budget = World->SpawnActor<AWoTBudgetInfo>(
		FVector::ZeroVector, FRotator::ZeroRotator, WoTBattleRestartAlwaysSpawn());
	Budget->Team = 1;
	FWoTBudgetResource Seals;
	Seals.ResourcePackageName = TEXT("WoTTraps");
	Seals.ResourceAdapterName = TEXT("SealInventory");
	Seals.ItemCount = 1;
	Budget->Budget.Add(Seals);

	auto Player = [World](uint8 Team, const FVector& At)
	{
		AWoTTestSealNotifyController* Controller =
			World->SpawnActor<AWoTTestSealNotifyController>(WoTBattleRestartAlwaysSpawn());
		// The pawnless viewer position is the controller's own root.
		if (USceneComponent* Root = Controller->GetRootComponent())
			Root->SetWorldLocation(At);
		Controller->InitPlayerState();
		if (AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>())
			State->Team = Team;
		return Controller;
	};
	auto Refused = [this](AWoTTestSealNotifyController* Controller,
		const FString& Expected, const TCHAR* Label)
	{
		Controller->CenterMessages.Reset();
		Controller->ServerEditCitadel(/*Mode=*/1);
		TestFalse(FString(Label) + TEXT(": no edit session opens"),
			Controller->IsEditingCitadel());
		TestTrue(FString(Label) + TEXT(": the retail refusal is shown"),
			Controller->CenterMessages.Num() == 1
			&& Controller->CenterMessages[0] == Expected);
	};

	Refused(Player(1, FVector(2500, 0, 0)), Retail(TEXT("CantEditZoneStr")),
		TEXT("outside every edit zone"));
	Refused(Player(1, FVector(5000, 0, 0)), Retail(TEXT("CantEditAreaStr")),
		TEXT("in another team's zone"));
	Refused(Player(255, FVector(0, 0, 0)), Retail(TEXT("CantEditAreaStr")),
		TEXT("a neutral player in a team zone"));
	Refused(Player(2, FVector(5000, 0, 0)), Retail(TEXT("CantEditBudgetStr")),
		TEXT("a team without a budget"));
	AWoTTestSealNotifyController* First = Player(1, FVector::ZeroVector);
	First->ServerEditCitadel(/*Mode=*/1);
	TestTrue(TEXT("the first editor of a team is granted"), First->IsEditingCitadel());
	TestTrue(TEXT("a granted edit shows no refusal"), First->CenterMessages.IsEmpty());
	Refused(Player(1, FVector::ZeroVector), Retail(TEXT("CantEditLimitStr")),
		TEXT("past the team editing limit"));
	First->ServerEditCitadel();
	TestFalse(TEXT("the editor key leaves the editor"), First->IsEditingCitadel());
	Battle->bTeleportersDisabled = false;
	Refused(Player(1, FVector::ZeroVector), Retail(TEXT("CantEditNowStr")),
		TEXT("once the battle has begun"));

	UWorld* ArenaWorld = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(ArenaWorld); };
	AWoTTestSealNotifyController* ArenaPlayer =
		ArenaWorld->SpawnActor<AWoTTestSealNotifyController>(WoTBattleRestartAlwaysSpawn());
	Refused(ArenaPlayer, Retail(TEXT("CantEditLevelStr")), TEXT("a mode without citadels"));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
