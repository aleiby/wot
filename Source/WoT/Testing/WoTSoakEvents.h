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
// Nested operation names let an opt-in observer attribute synchronous transform
// changes, including relocations that happen inside a movement overlap callback.
struct WOT_API FBodyScope
{
	explicit FBodyScope(const TCHAR* Source);
	~FBodyScope();
private:
	bool bActive;
};
WOT_API FString BodySource();
DECLARE_MULTICAST_DELEGATE_TwoParams(FBodyEvent, AActor*, const TCHAR*);
WOT_API FBodyEvent& BodyEvents();
}
#define WOT_SOAK_EVENT(Kind, Subject, Participant) WoTSoakEvents::Events().Broadcast(WoTSoakEvents::EKind::Kind, Subject, Participant)
#define WOT_SOAK_BODY_SCOPE(Source) WoTSoakEvents::FBodyScope WoTBodyScope(TEXT(Source))
#define WOT_SOAK_BODY_EVENT(Actor, Source) WoTSoakEvents::BodyEvents().Broadcast(Actor, TEXT(Source))
#else
#define WOT_SOAK_EVENT(Kind, Subject, Participant)
#define WOT_SOAK_BODY_SCOPE(Source)
#define WOT_SOAK_BODY_EVENT(Actor, Source)
#endif
