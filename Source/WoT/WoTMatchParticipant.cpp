#include "WoTMatchParticipant.h"

#include "AIController.h"
#include "AI/WoTNPCController.h"
#include "WoTPlayerState.h"

bool IsWoTBotParticipant(const AController* Controller)
{
	const AWoTPlayerState* State = Controller
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	return Controller && Controller->IsA<AAIController>()
		&& !Controller->IsA<AWoTNPCController>() && State && State->IsABot();
}
