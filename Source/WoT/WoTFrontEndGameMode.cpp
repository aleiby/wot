#include "WoTFrontEndGameMode.h"

#include "WoTFrontEndPlayerController.h"

AWoTFrontEndGameMode::AWoTFrontEndGameMode()
{
	DefaultPawnClass = nullptr;
	HUDClass = nullptr;
	PlayerControllerClass = AWoTFrontEndPlayerController::StaticClass();
	// The controller projects bootstrap Slate until the subsystem proves Ready;
	// this mode never creates a pawn/HUD that could bypass that gate.
}
