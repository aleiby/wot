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
	Row.Values = [](const FWoTHostRequest& Request)
	{
		TArray<int32> Values{0};
		for (int32 Count = 2; Count <= Request.MaxPlayers; ++Count) Values.Add(Count);
		return Values;
	};
	Row.ValueText = [](int32 Value, const FWoTHostRequest&)
	{
		if (Value == 0) return NSLOCTEXT("WoTBots", "HostFillOff", "Off");
		return FText::Format(NSLOCTEXT("WoTBots", "HostFillCount", "{0} (you + {1} {1}|plural(one=bot,other=bots))"),
			FText::AsNumber(Value), Value - 1);
	};
	FWoTFrontEndModel::RegisterHostOption(MoveTemp(Row));
}

void FWoTBotsModule::ShutdownModule()
{
	WoTBotNavigation::Unregister();
	FWoTFrontEndModel::UnregisterHostOptions(HostOptionOwner());
}

IMPLEMENT_MODULE(FWoTBotsModule, WoTBots)
