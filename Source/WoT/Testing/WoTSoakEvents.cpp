#include "WoTSoakEvents.h"
#if WITH_AUTOMATION_TESTS
#include "GameFramework/Actor.h"
#include "Inventory/WoTInventory.h"
#include "WoTFragment.h"
#include "WoTBreakableDecoration.h"
#include "WoTBounceableDecoration.h"
#include "WoTCreatureChunk.h"
#include "WoTCarcass.h"
#include "WoTDecal.h"
#include "WoTWorldFX.h"
namespace
{
thread_local TArray<FString> BodySources;
}
WoTSoakEvents::FBodyScope::FBodyScope(const TCHAR* Source) : bActive(BodyEvents().IsBound())
{
	if (bActive) BodySources.Add(Source);
}
WoTSoakEvents::FBodyScope::~FBodyScope() { if (bActive) BodySources.Pop(); }
FString WoTSoakEvents::BodySource() { return FString::Join(BodySources, TEXT(" > ")); }
WoTSoakEvents::FBodyEvent& WoTSoakEvents::BodyEvents()
{
	static FBodyEvent Event;
	return Event;
}
FVector WoTSoakEvents::SimulationVelocity(const AActor* Actor)
{
	if (const auto* A = Cast<AWoTInventory>(Actor)) return A->GetFallingVelocity();
	if (const auto* A = Cast<AWoTFragment>(Actor)) return A->GetFragmentVelocity();
	if (const auto* A = Cast<AWoTBreakableDecoration>(Actor)) return A->GetFallVelocity();
	if (const auto* A = Cast<AWoTBounceableDecoration>(Actor)) return A->GetBounceVelocity();
	if (const auto* A = Cast<AWoTCreatureChunk>(Actor)) return A->GetVelocityForTest();
	if (const auto* A = Cast<AWoTCarcass>(Actor)) return A->GetVelocityForTest();
	if (const auto* A = Cast<AWoTDecal>(Actor)) return A->Velocity;
	if (const auto* A = Cast<AWoTSpewChunk>(Actor)) return A->GetVelocityForTest();
	return Actor->GetVelocity();
}
WoTSoakEvents::FEvent& WoTSoakEvents::Events()
{
	static FEvent Event;
	return Event;
}
#endif
