#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class WOTBOTS_API FWoTBotsModule final : public IModuleInterface
{
public:
	static FName HostOptionOwner();

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
