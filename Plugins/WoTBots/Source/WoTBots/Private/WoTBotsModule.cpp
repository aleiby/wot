#include "WoTBotsModule.h"

#include "Modules/ModuleManager.h"
#include "WoTFrontEnd.h"
#include "WoTBotNavigation.h"

FName FWoTBotsModule::HostOptionOwner()
{
	return FName(TEXT("WoTBots"));
}

void FWoTBotsModule::StartupModule()
{
	WoTBotNavigation::Register();
	FWoTHostOptionRow Row;
	Row.Owner = HostOptionOwner();
	Row.URLKey = TEXT("BotFill");
	Row.Label = []() { return NSLOCTEXT("WoTBots", "HostFillLabel", "Fill with bots to"); };
	Row.Min = 0;
	Row.Max = [](const FWoTHostRequest& Request) { return Request.MaxPlayers; };
	Row.DefaultValue = 0;
	FWoTFrontEndModel::RegisterHostOption(MoveTemp(Row));
}

void FWoTBotsModule::ShutdownModule()
{
	WoTBotNavigation::Unregister();
	FWoTFrontEndModel::UnregisterHostOptions(HostOptionOwner());
}

IMPLEMENT_MODULE(FWoTBotsModule, WoTBots)
