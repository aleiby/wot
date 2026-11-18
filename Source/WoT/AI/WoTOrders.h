#pragma once

#include "CoreMinimal.h"
#include "WoTOrders.generated.h"

// Captain order types (Captain.uc:24 EOrdersType). Declaration order matches the
// original enum so MapOrders / the CDO default carry over verbatim. A Captain is
// modelled as "a Grunt with orders + ranged" (policy-port-map.md §2): the pawn
// carries a bIsCaptain flag + an AssignedOrders default, and this enum drives the
// controller's orders block. AssignedOrders is what the map/GUI sets; the AI
// reads WorkingOrders (Captain.uc:42-45).
UENUM()
enum class EWoTOrders : uint8
{
	Guard,          // OT_Guard: guard the current location
	KillIntruder,   // OT_KillIntruder: acts as a Grunt (plus the idle heal)
	GuardSeal,      // OT_GuardSeal: guard the nearest unowned seal
	SoundAlarm,     // OT_SoundAlarm: navigate to and ring the nearest alarm
	GetHelp,        // OT_GetHelp: navigate to a reinforcement and alert nearby pawns
	Automatic       // OT_Automatic: escalate from alarm to reinforcements to attack (Captain.uc:31).
};
