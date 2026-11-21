#pragma once

#include "CoreMinimal.h"
#include "NavLinkCustomComponent.h"
#include "Navigation/NavLinkProxy.h"
#include "WoTBotMoverLink.generated.h"

class AWoTBotController;
class AWoTMover;
class AWoTTrigger;
class ANavigationData;
class UNavigationSystemV1;
#if WITH_AUTOMATION_TESTS
namespace WoTBotTest { struct FWoTBotMoverAccess; }
#endif

// The link's planning filter. UE asks it per querier while searching a route
// (INavLinkCustomInterface::IsLinkPathfindingAllowed); a bot that has just
// failed this traversal is given routes without it until its cooldown ends.
UCLASS(Transient)
class UWoTBotMoverLinkComponent : public UNavLinkCustomComponent
{
	GENERATED_BODY()
public:
	virtual bool IsLinkPathfindingAllowed(const UObject* Querier) const override;
};

// A fixed, one-way navigation action. Only ordinary pawn movement operates the
// mover; its geometry stays relevant to both navigation meshes.
UCLASS(Transient, NotPlaceable)
class AWoTBotMoverLink : public ANavLinkProxy
{
	GENERATED_BODY()
public:
	AWoTBotMoverLink(const FObjectInitializer& ObjectInitializer);
	// Holds are waiting spots for bots contending for a lift: stationary floor
	// beside the entry, outside the deck's footprint.
	void Configure(AWoTMover* InMover, AWoTTrigger* InTrigger, int32 Agent,
		const FVector& Entry, const FVector& Exit, const FVector& Deck, const TArray<FVector>& InHolds);
	void UpdateTraversal(AWoTBotController* Bot, float DeltaSeconds);
	void Cancel(AWoTBotController* Bot);
	bool IsLiftLink() const { return bLift; }
	FVector GetEntryLocation() const { return EntryFeet; }
	FVector GetExitLocation() const { return ExitFeet; }
	const TArray<FVector>& GetHoldLocations() const { return Holds; }
	int32 GetLiftRides() const { return LiftRides; }
	int32 GetDoorExits() const { return DoorExits; }
	int32 GetAbandons() const { return Abandons; }
	AWoTMover* GetMover() const { return Mover.Get(); }
protected:
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
#if WITH_AUTOMATION_TESTS
	friend struct WoTBotTest::FWoTBotMoverAccess;
#endif
	UFUNCTION() void Reached(AActor* Actor, const FVector& Destination);
	enum class EStage : uint8 { Wait, Board, Ride, Exit, Retreat, Contended };
	struct FTraversal
	{
		EStage Stage = EStage::Wait;
		float Started = 0, StageStarted = 0, LastMotion = 0;
		// When a rider on the deck first saw it resting at the bottom; -1 otherwise.
		float BottomSince = -1;
		FVector LastPosition = FVector::ZeroVector;
		// Where this bot waits: its hold while contended, else the entry.
		FVector Hold = FVector::ZeroVector;
		TWeakObjectPtr<APawn> Pawn;
	};
	void Finish(AWoTBotController* Bot, bool bSuccess);
	FVector HoldFor(const AWoTBotController* Bot, const FVector& From) const;
	bool InFootprint(const FVector& Point, float Radius) const;
	TWeakObjectPtr<AWoTMover> Mover;
	TWeakObjectPtr<AWoTTrigger> Trigger;
	TWeakObjectPtr<AWoTBotController> Rider;
	TMap<TWeakObjectPtr<AWoTBotController>, FTraversal> Traversals;
	FVector EntryFeet, ExitFeet, DeckOffset, BottomPosition, TopPosition;
	TArray<FVector> Holds;
	FVector2D FootprintMin = FVector2D::ZeroVector, FootprintMax = FVector2D::ZeroVector;
	float RetreatReach = 0;
	bool bLift = false;
	int32 LiftRides = 0, DoorExits = 0, Abandons = 0;
};

namespace WoTBotMoverLinks
{
int32 Build(UWorld& World, UNavigationSystemV1& Nav, const ANavigationData& Data, int32 Agent);
}
