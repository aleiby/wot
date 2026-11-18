#include "WoTBootstrapGameMode.h"

#include "WoTFrontEndPlayerController.h"

AWoTBootstrapGameMode::AWoTBootstrapGameMode()
{
	DefaultPawnClass = nullptr;
	HUDClass = nullptr;
	PlayerControllerClass = AWoTFrontEndPlayerController::StaticClass();
	bStartPlayersAsSpectators = true;
}
