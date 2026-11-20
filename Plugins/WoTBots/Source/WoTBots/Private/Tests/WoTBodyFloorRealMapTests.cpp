#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotMapFixture.h"
#include "WoTPlayerController.h"
#include "WoTTeleporter.h"
#include "Testing/WoTSoakEvents.h"
#include "Components/CapsuleComponent.h"

namespace WoTBodyFloorRealMap
{
using namespace WoTBotTest;

// A human-controlled body walks into the projected pad on the actual level.
// The arrival is observed inside the real relocation callback, before movement
// can hide a bad placement with its next penetration recovery.
bool WalkThroughPad(FAutomationTestBase& Test, const TCHAR* Map,
	const FVector& Start, const FVector& Direction, const FVector& Destination)
{
	FMapSet Maps; FString Error;
	if (!Maps.Load(Error)) { Test.AddError(Error); return false; }
	const FWoTWorldDescriptor* Descriptor = Maps.Descriptors.FindByPredicate(
		[Map](const auto& D) { return D.MapIdentity == Map; });
	if (!Test.TestNotNull(TEXT("real map descriptor"), Descriptor)) return false;
	WoTTest::FSpecWorldOptions Options; Options.bStartPlay = false;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	UWorld* W = WoTTest::CreateSpecWorld(Options);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	if (!Materialize(W, Maps, *Descriptor, Error)) { Test.AddError(Error); return false; }
	W->GetWorldSettings()->DefaultGameMode = AWoTGameMode::StaticClass();
	FURL URL(nullptr, TEXT("/Engine/Maps/Entry?BotFill=0?FragLimit=0?TimeLimit=0"), TRAVEL_Absolute);
	if (!Test.TestTrue(TEXT("real game mode"), W->SetGameMode(URL))) return false;
	W->InitializeActorsForPlay(URL); W->BeginPlay();
	AWoTPlayerController* PC = W->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Body = Cast<AWoTCharacter>(AWoTGameMode::SpawnPawnAtRetailStart(
		*W, AWoTCharacter::StaticClass(), FTransform(Start), nullptr, true));
	if (!Test.TestNotNull(TEXT("player body"), Body) || !Test.TestNotNull(TEXT("player controller"), PC)) return false;
	PC->SetAsLocalPlayerController();
	PC->Possess(Body); PC->SetControlRotation(Direction.Rotation());
	Body->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	WoTTest::TickWorld(W, 1.0f, 1.0f / 30.0f);
	Test.AddInfo(FString::Printf(TEXT("approach=%s movement=%s"), *Body->GetActorLocation().ToString(), *Body->GetCharacterMovement()->GetMovementName()));
	if (!Test.TestTrue(TEXT("player stands before walking into the pad"), Body->GetCharacterMovement()->IsMovingOnGround())) return false;
	bool bArrived = false;
	FVector Arrival;
	const FDelegateHandle Handle = WoTSoakEvents::BodyEvents().AddLambda(
		[&](AActor* Actor, const TCHAR* Event)
		{
			if (Actor != Body || FString(Event) != TEXT("teleporter placed")) return;
			if (FVector::Dist2D(Body->GetActorLocation(), Destination) > 1.0) return;
			bArrived = true; Arrival = Body->GetActorLocation();
		});
	ON_SCOPE_EXIT { WoTSoakEvents::BodyEvents().Remove(Handle); };
	for (int32 Tick = 0; Tick < 120 && !bArrived; ++Tick)
	{
		Body->AddMovementInput(Direction);
		W->Tick(LEVELTICK_All, 1.0f / 30.0f); ++GFrameCounter;
	}
	if (!Test.TestTrue(TEXT("walking into the real pad invokes its real relocation"), bArrived)) return false;
	FHitResult Floor;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTRealPadFloor), false, Body);
	const bool bFloor = W->LineTraceSingleByChannel(Floor, Arrival,
		Arrival - FVector(0, 0, 200), ECC_Visibility, Params);
	if (!Test.TestTrue(TEXT("walkable destination floor"), bFloor && Body->GetCharacterMovement()->IsWalkable(Floor))) return false;
	const double Feet = Arrival.Z - Body->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	Test.AddInfo(FString::Printf(TEXT("%s arrival=%s floor=%.6f feet=%.6f"), Map, *Arrival.ToString(), Floor.ImpactPoint.Z, Feet));
	Test.TestTrue(TEXT("teleport placement keeps the capsule above the floor before recovery"), Feet >= Floor.ImpactPoint.Z - UE_KINDA_SMALL_NUMBER);
	WoTTest::TickWorld(W, 1.0f, 1.0f / 30.0f);
	Test.TestTrue(TEXT("the human body settles on the destination floor"), Body->GetCharacterMovement()->IsMovingOnGround());
	return !Test.HasAnyErrors();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBodyFloorTeleporterSpec,
	"WoT.Teleporter.RealMap.ArrivalFitsArena06Floor", WoTBotTest::Flags)
bool FBodyFloorTeleporterSpec::RunTest(const FString&)
{
	return WoTBodyFloorRealMap::WalkThroughPad(*this, TEXT("Arena_06"),
		FVector(-700, -124.25, 380), FVector(-1, 0, 0), FVector(2404.973, -163.671, 40.103));
}
#endif
