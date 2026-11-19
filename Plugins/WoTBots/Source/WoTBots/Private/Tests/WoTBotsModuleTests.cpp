#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTBotsModule.h"
#include "WoTBotSettings.h"
#include "WoTBotNavigation.h"
#include "Tests/WoTTestWorld.h"
#include "AI/NavigationSystemConfig.h"
#include "GameFramework/WorldSettings.h"
#include "Engine/World.h"
#include "WoTFrontEnd.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotsHostModuleLifecycleSpec,
	"WoT.Bots.Host.ModuleLifecycleAndURL",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBotsHostModuleLifecycleSpec::RunTest(const FString&)
{
	FWoTBotsModule& Module = FModuleManager::LoadModuleChecked<FWoTBotsModule>(TEXT("WoTBots"));
	ON_SCOPE_EXIT { Module.StartupModule(); };
	const FName Owner = FWoTBotsModule::HostOptionOwner();
	TestNotNull(TEXT("startup registered the host row"),
		FWoTFrontEndModel::FindHostOption(Owner, TEXT("BotFill")));
	Module.ShutdownModule();
	TestNull(TEXT("shutdown removes the host row"),
		FWoTFrontEndModel::FindHostOption(Owner, TEXT("BotFill")));
	FWoTHostRequest Request;
	Request.MapPackage = TEXT("/Game/WoT/Maps/Arena_01");
	Request.MaxPlayers = 12;
	Request.ExtraValues.Add(TEXT("BotFill"), 8);
	const EWoTPlayerFaction ClassChoice = FWoTFrontEndModel::FactionChoices()[0].Faction;
	TestFalse(TEXT("disabled module contributes no URL option"),
		FWoTFrontEndModel::BuildHostTravelURL(Request, ClassChoice).Contains(TEXT("?BotFill=")));
	Module.StartupModule();
	Module.StartupModule();
	const FWoTHostOptionRow* Row = FWoTFrontEndModel::FindHostOption(Owner, TEXT("BotFill"));
	if (!TestNotNull(TEXT("startup restores the host row"), Row)) return false;
	TestEqual(TEXT("host label"), Row->Label().ToString(), FString(TEXT("Fill with bots to")));
	int32 OwnedRows = 0;
	for (const FWoTHostOptionRow& Registered : FWoTFrontEndModel::HostOptions())
	{
		if (Registered.Owner == Owner) ++OwnedRows;
	}
	TestEqual(TEXT("repeated startup keeps one row"), OwnedRows, 1);
	TestTrue(TEXT("selected fill travels"),
		FWoTFrontEndModel::BuildHostTravelURL(Request, ClassChoice).Contains(TEXT("?BotFill=8")));
	Request.MaxPlayers = 4;
	TestTrue(TEXT("fill follows the current Max Players"),
		FWoTFrontEndModel::BuildHostTravelURL(Request, ClassChoice).Contains(TEXT("?BotFill=4")));
	Request.ExtraValues.Add(TEXT("BotFill"), -1);
	TestTrue(TEXT("fill cannot be negative"),
		FWoTFrontEndModel::BuildHostTravelURL(Request, ClassChoice).Contains(TEXT("?BotFill=0")));
	UWoTBotSettings* Settings = GetMutableDefault<UWoTBotSettings>();
	const int32 PreviousDefault = Settings->DefaultBotFill;
	ON_SCOPE_EXIT { Settings->DefaultBotFill = PreviousDefault; };
	Settings->DefaultBotFill = 11;
	Request.ExtraValues.Reset();
	TestTrue(TEXT("host defaults off independently of the dedicated-server setting"),
		FWoTFrontEndModel::BuildHostTravelURL(Request, ClassChoice).Contains(TEXT("?BotFill=0")));
	Module.ShutdownModule();
	WoTTest::FSpecWorldOptions Options; Options.bStartPlay = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	TestTrue(TEXT("shutdown removes every navigation callback after repeated startup"),
		World->GetWorldSettings()->GetNavigationSystemConfig()->NavigationSystemClass
			== FSoftClassPath(UNavigationSystemV1::StaticClass()));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
