#pragma once
#include "CoreMinimal.h"

class AActor;

// Synchronous observation only; no storage or callbacks in non-automation builds.
#if WITH_AUTOMATION_TESTS
namespace WoTSoakEvents
{
enum class EKind : uint8 { Cast, Pickup, PoolStock, PoolCollect, Recovery, SilentRespawn };
DECLARE_MULTICAST_DELEGATE_ThreeParams(FEvent, EKind, AActor*, AActor*);
WOT_API FEvent& Events();
WOT_API FVector SimulationVelocity(const AActor* Actor);
}
#define WOT_SOAK_EVENT(Kind, Subject, Participant) WoTSoakEvents::Events().Broadcast(WoTSoakEvents::EKind::Kind, Subject, Participant)
#else
#define WOT_SOAK_EVENT(Kind, Subject, Participant)
#endif
