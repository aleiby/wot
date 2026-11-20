#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTAngrealInvLevitate.h"
#include "../Inventory/WoTZoneLeeches.h"
#include "../WoTAmbientSound.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"
#include "../WoTSound.h"
#include "../WoTZoneVolume.h"

#include "AIController.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Sound/SoundWave.h"

namespace WoTLocalHumanEffectsSpec
{
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters AlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

struct FFixture
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Bot = nullptr;
	AWoTCharacter* Human = nullptr;
	AAIController* AI = nullptr;
	AWoTPlayerController* Player = nullptr;

	~FFixture() { WoTTest::DestroySpecWorld(World); }

	bool Initialize(FAutomationTestBase& Test)
	{
		if (!Test.TestNotNull(TEXT("standalone effects world"), World)) return false;
		Test.TestEqual(TEXT("fixture uses standalone authority"), World->GetNetMode(), NM_Standalone);
		Bot = World->SpawnActor<AWoTCharacter>(
			FVector(-200, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
		Human = World->SpawnActor<AWoTCharacter>(
			FVector(200, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
		AI = World->SpawnActor<AAIController>(AlwaysSpawn());
		Player = World->SpawnActor<AWoTPlayerController>(AlwaysSpawn());
		if (!Test.TestNotNull(TEXT("bot player body"), Bot)
			|| !Test.TestNotNull(TEXT("human player body"), Human)
			|| !Test.TestNotNull(TEXT("real AI controller"), AI)
			|| !Test.TestNotNull(TEXT("local player controller"), Player)) return false;
		AI->Possess(Bot);
		Player->SetAsLocalPlayerController();
		Player->Possess(Human);
		Test.TestTrue(TEXT("AI body reproduces the locally controlled bot condition"),
			Bot->IsLocallyControlled() && !Bot->IsPlayerControlled());
		Test.TestTrue(TEXT("human body has local player control"),
			Human->IsLocallyControlled() && Human->IsPlayerControlled());
		return true;
	}
};

int32 CountSmoothLeeches(const AWoTCharacter* Body)
{
	TArray<UWoTSmoothRandFOVLeech*> Leeches;
	Body->GetComponents<UWoTSmoothRandFOVLeech>(Leeches);
	int32 Count = 0;
	for (const UWoTSmoothRandFOVLeech* Leech : Leeches)
	{
		if (IsValid(Leech) && Leech->IsRegistered()) ++Count;
	}
	return Count;
}

// A derived scope may form a pointer to its protected base member. Read that
// member on the real production component; no derived UObject is instantiated.
struct FSmoothFOVState : UWoTSmoothRandFOVLeech
{
	static float ReadCurrent(const UWoTSmoothRandFOVLeech& Leech)
	{
		const float UWoTSmoothRandFOVLeech::* Field = &FSmoothFOVState::CurrentFOV;
		return Leech.*Field;
	}
	static float ReadNext(const UWoTSmoothRandFOVLeech& Leech)
	{
		const float UWoTSmoothRandFOVLeech::* Field = &FSmoothFOVState::NextFOV;
		return Leech.*Field;
	}
	static bool ReadGrowing(const UWoTSmoothRandFOVLeech& Leech)
	{
		const bool UWoTSmoothRandFOVLeech::* Field = &FSmoothFOVState::bGrowing;
		return Leech.*Field;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalHumanZoneEffectsSpec,
	"WoT.LocalHuman.Effects.ZoneFOV", WoTLocalHumanEffectsSpec::Flags)

bool FWoTLocalHumanZoneEffectsSpec::RunTest(const FString&)
{
	using namespace WoTLocalHumanEffectsSpec;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	AWoTZoneVolume* Zone = Fixture.World->SpawnActor<AWoTZoneVolume>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("view effects zone"), Zone)) return false;
	Zone->Collision->SetBoxExtent(FVector(500));
	Zone->bFOVZone = true;
	Zone->ZoneFOV = 120.0f;
	Zone->bLeechZone = true;
	Zone->LeechClasses.Add(UWoTSmoothRandFOVLeech::StaticClass());
	const float BotFOV = Fixture.Bot->GetZoneFOVForLeech();
	const float HumanFOV = Fixture.Player->GetCameraFOVForTest();

	Zone->ReconcileOwnerLocalFOV(Fixture.Bot);
	Zone->ReconcileOwnerLocalFOV(Fixture.Human);
	TestEqual(TEXT("bot does not acquire pending zone FOV"),
		Fixture.Bot->GetZoneFOVForLeech(), BotFOV);
	TestEqual(TEXT("bot does not acquire a local view leech"), CountSmoothLeeches(Fixture.Bot), 0);
	TestEqual(TEXT("human receives authored zone FOV"),
		Fixture.Player->GetCameraFOVForTest(), 120.0f);
	TestEqual(TEXT("human receives one local view leech"), CountSmoothLeeches(Fixture.Human), 1);
	Zone->ReconcileOwnerLocalFOV(Fixture.Human);
	TestEqual(TEXT("human reconciliation remains singular"), CountSmoothLeeches(Fixture.Human), 1);
	Zone->DetachOwnerLocalFOV(Fixture.Human);
	TestEqual(TEXT("human exit restores camera FOV"),
		Fixture.Player->GetCameraFOVForTest(), HumanFOV);
	TestEqual(TEXT("human exit removes the view leech"), CountSmoothLeeches(Fixture.Human), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalHumanSmoothFOVTickSpec,
	"WoT.LocalHuman.Effects.SmoothFOVTick", WoTLocalHumanEffectsSpec::Flags)

bool FWoTLocalHumanSmoothFOVTickSpec::RunTest(const FString&)
{
	using namespace WoTLocalHumanEffectsSpec;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	UWoTSmoothRandFOVLeech* BotLeech = NewObject<UWoTSmoothRandFOVLeech>(Fixture.Bot);
	UWoTSmoothRandFOVLeech* HumanLeech = NewObject<UWoTSmoothRandFOVLeech>(Fixture.Human);
	BotLeech->AttachTo(Fixture.Bot);
	HumanLeech->AttachTo(Fixture.Human);
	if (!TestTrue(TEXT("bot fixture has a preexisting attached view leech"), BotLeech->IsRegistered())
		|| !TestTrue(TEXT("human view leech is attached"), HumanLeech->IsRegistered())) return false;
	const float BotCurrent = FSmoothFOVState::ReadCurrent(*BotLeech);
	const float BotNext = FSmoothFOVState::ReadNext(*BotLeech);
	const bool bBotGrowing = FSmoothFOVState::ReadGrowing(*BotLeech);
	const float HumanCurrent = FSmoothFOVState::ReadCurrent(*HumanLeech);

	BotLeech->TickComponent(0.25f, LEVELTICK_All, nullptr);
	HumanLeech->TickComponent(0.25f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("bot tick leaves interpolated FOV unchanged"),
		FSmoothFOVState::ReadCurrent(*BotLeech), BotCurrent);
	TestEqual(TEXT("bot tick leaves target FOV unchanged"),
		FSmoothFOVState::ReadNext(*BotLeech), BotNext);
	TestEqual(TEXT("bot tick leaves interpolation direction unchanged"),
		FSmoothFOVState::ReadGrowing(*BotLeech), bBotGrowing);
	TestTrue(TEXT("human tick advances FOV at the authored rate"),
		FMath::IsNearlyEqual(FMath::Abs(FSmoothFOVState::ReadCurrent(*HumanLeech) - HumanCurrent),
			HumanLeech->FOVRate * 0.25f));
	TestEqual(TEXT("human tick delivers interpolated FOV to the camera"),
		Fixture.Player->GetCameraFOVForTest(), FSmoothFOVState::ReadCurrent(*HumanLeech));
	BotLeech->Unattach();
	HumanLeech->Unattach();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalHumanLiftLoopSpec,
	"WoT.LocalHuman.Effects.LiftLoop", WoTLocalHumanEffectsSpec::Flags)

bool FWoTLocalHumanLiftLoopSpec::RunTest(const FString&)
{
	using namespace WoTLocalHumanEffectsSpec;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	AStaticMeshActor* Floor = Fixture.World->SpawnActor<AStaticMeshActor>(
		FVector(0, 0, -40), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("lift ground fixture"), Floor)) return false;
	Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	UBoxComponent* FloorBox = NewObject<UBoxComponent>(Floor);
	FloorBox->SetupAttachment(Floor->GetRootComponent());
	FloorBox->SetMobility(EComponentMobility::Movable);
	Floor->AddInstanceComponent(FloorBox);
	FloorBox->SetBoxExtent(FVector(1000, 1000, 40));
	FloorBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	FloorBox->SetCollisionObjectType(ECC_WorldStatic);
	FloorBox->SetCollisionResponseToAllChannels(ECR_Block);
	FloorBox->RegisterComponent();

	AWoTNPCCharacter* NPC = Fixture.World->SpawnActor<AWoTNPCCharacter>(
		FVector(0, 400, 100), FRotator::ZeroRotator, AlwaysSpawn());
	AAIController* NPCController = Fixture.World->SpawnActor<AAIController>(AlwaysSpawn());
	if (!TestNotNull(TEXT("standalone NPC lift control"), NPC)
		|| !TestNotNull(TEXT("NPC AI controller"), NPCController)) return false;
	NPCController->Possess(NPC);
	TestTrue(TEXT("NPC fixture retains AI possession and its distinct body type"),
		NPC->GetController() == NPCController && NPC->IsLocallyControlled() && NPC->IsNPCPawn());

	// Inject a resident sound through the item's existing soft reference so the
	// component assertion does not depend on imported audio or a playback device.
	USoundWave* Loop = NewObject<USoundWave>(Fixture.World);
	Loop->Duration = 1.0f;
	Loop->bLooping = true;
	for (AWoTCharacter* Body : {Fixture.Bot, Fixture.Human, static_cast<AWoTCharacter*>(NPC)})
	{
		const bool bHuman = Body == Fixture.Human;
		const bool bNPC = Body == NPC;
		const FString Label = bHuman ? TEXT("human") : (bNPC ? TEXT("NPC") : TEXT("bot"));
		FVector Location = Body->GetActorLocation();
		Location.Z = Body->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 5.0f;
		Body->SetActorLocation(Location);
		Body->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		AWoTAngrealInvLevitate* Item = Fixture.World->SpawnActor<AWoTAngrealInvLevitate>(
			FVector(0, 0, 300), FRotator::ZeroRotator, AlwaysSpawn());
		if (!TestNotNull(Label + TEXT(" lift item"), Item)) return false;
		Item->SetOwner(Body);
		Item->CurCharges = 10;
		Item->ActivateSound = Loop;
		Item->DeactivateSound.Reset();
		if (!TestEqual(Label + TEXT(" loop sound resolves headlessly"),
			WoTSound::Resolve(Item->ActivateSound, true), static_cast<USoundBase*>(Loop))) return false;
		Item->CastArtifact();
		if (bHuman || bNPC)
		{
			TestNotNull(Label + TEXT(" cast starts its owner loop component"), Item->GetLevitateLoopForTest());
		}
		else
		{
			TestNull(TEXT("bot cast creates no owner loop component"), Item->GetLevitateLoopForTest());
		}
		Item->Tick(0.01f);
		TestEqual(Label + TEXT(" lift still changes the physical movement mode"),
			Body->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
		TestEqual(Label + TEXT(" lift still raises the body above traced ground"),
			Body->GetCharacterMovement()->Velocity.Z, static_cast<double>(Item->LevitateSpeed));
		TestEqual(Label + TEXT(" lift still consumes a charge"), Item->CurCharges, 9);
		Item->UnCastArtifact();
		TestNull(Label + TEXT(" release leaves no owner loop component"), Item->GetLevitateLoopForTest());
		TestEqual(Label + TEXT(" release restores the original movement mode"),
			Body->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Falling);
	}
	return !HasAnyErrors();
}

#endif
