#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTActorRotator.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTJumper.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTSealAltar.h"
#include "../WoTZoneVolume.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/DefaultPawn.h"
#include "Math/RotationMatrix.h"

namespace
{
constexpr EAutomationTestFlags RotationEntryFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters RotationAlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

void CheckBody(FAutomationTestBase& Test, const ACharacter& Body, double Yaw)
{
	Test.TestTrue(TEXT("character body receives yaw only"),
		Body.GetActorRotation().Equals(FRotator(0.0, Yaw, 0.0), 0.01));
	Test.TestTrue(TEXT("character capsule axis remains vertical"),
		Body.GetCapsuleComponent()->GetUpVector().Equals(FVector::UpVector, 0.001));
}

struct FRotationEditorFixture
{
	UWorld* World = nullptr;
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Body = nullptr;

	bool Initialize()
	{
		World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
		if (!World) return false;
		AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
			AWoTBattleInfo::StaticClass(), FTransform::Identity);
		Battle->NumTeams = 2;
		Battle->TeamClass[0] = AWoTCharacter::StaticClass();
		Battle->TeamClass[1] = AWoTCharacter::StaticClass();
		Battle->NumPlayers[1] = 1;
		Battle->bTeleportersDisabled = true;
		Battle->FinishSpawning(FTransform::Identity);
		World->GetAuthGameMode<AWoTBattleGameMode>()->Battle = Battle;
		AWoTZoneVolume* Zone = World->SpawnActorDeferred<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), FTransform::Identity);
		Zone->EditTeam = 1;
		Zone->bAllowEditing = true;
		Zone->Collision->SetBoxExtent(FVector(3000.0));
		Zone->FinishSpawning(FTransform::Identity);
		AWoTSealAltar* Altar = World->SpawnActor<AWoTSealAltar>(RotationAlwaysSpawn());
		Altar->Team = 1;
		AWoTBudgetInfo* Budget = World->SpawnActor<AWoTBudgetInfo>(RotationAlwaysSpawn());
		Budget->Team = 1;
		FWoTBudgetResource& Resource = Budget->Budget.AddDefaulted_GetRef();
		Resource.ResourcePackageName = TEXT("WoTPawns");
		Resource.ResourceAdapterName = TEXT("SoldierInventory");
		Resource.ItemCount = 2;
		Controller = World->SpawnActor<AWoTPlayerController>(RotationAlwaysSpawn());
		Controller->SetAsLocalPlayerController();
		Controller->InitPlayerState();
		Controller->GetPlayerState<AWoTPlayerState>()->Team = 1;
		Body = World->SpawnActor<AWoTCharacter>(FVector(320, -160, 240),
			FRotator::ZeroRotator, RotationAlwaysSpawn());
		Controller->Possess(Body);
		return true;
	}

	~FRotationEditorFixture() { WoTTest::DestroySpecWorld(World); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBodyRotationJumperSpec,
	"WoT.Movement.BodyRotation.JumperLaunch", RotationEntryFlags)
bool FWoTBodyRotationJumperSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FRotator Incoming(23, 71, 37);
	for (bool bCharacter : {true, false})
	{
		const FVector Location(bCharacter ? 1000 : 2000, 0, 200);
		APawn* Pawn = bCharacter
			? static_cast<APawn*>(World->SpawnActor<AWoTCharacter>(Location,
				FRotator::ZeroRotator, RotationAlwaysSpawn()))
			: static_cast<APawn*>(World->SpawnActor<ADefaultPawn>(Location,
				FRotator::ZeroRotator, RotationAlwaysSpawn()));
		AWoTJumper* Jumper = World->SpawnActor<AWoTJumper>(Location, Incoming,
			RotationAlwaysSpawn());
		Jumper->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
		WoTTest::TickWorld(World, 0.02f, 0.005f);
		if (ACharacter* Body = Cast<ACharacter>(Pawn)) CheckBody(*this, *Body, Incoming.Yaw);
		else TestTrue(TEXT("non-character launch keeps full authored rotation"),
			Pawn->GetActorRotation().Equals(Incoming, 0.01));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBodyRotationOrbitSpec,
	"WoT.Movement.BodyRotation.OrbitDriver", RotationEntryFlags)
bool FWoTBodyRotationOrbitSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Body = World->SpawnActor<AWoTCharacter>(FVector(1000, 0, 200),
		FRotator::ZeroRotator, RotationAlwaysSpawn());
	ADefaultPawn* Other = World->SpawnActor<ADefaultPawn>(FVector(2000, 0, 200),
		FRotator::ZeroRotator, RotationAlwaysSpawn());
	const FTransform Transform(FRotator(24, 62, 0), FVector(1000, 0, 200));
	AWoTActorRotator* Driver = World->SpawnActorDeferred<AWoTActorRotator>(
		AWoTActorRotator::StaticClass(), Transform);
	Driver->DrivenActors = {TSoftObjectPtr<AActor>(Body), TSoftObjectPtr<AActor>(Other)};
	Driver->MinRadius = Driver->MaxRadius = 100;
	Driver->MinHeight = Driver->MaxHeight = 25;
	Driver->MinRotationRate = Driver->MaxRotationRate = 16384;
	Driver->ActorRotationOffset = FRotator(4096, 8192, 6144);
	Driver->FinishSpawning(Transform);
	for (int32 Mode = 0; Mode < 3; ++Mode)
	{
		Driver->bFaceIn = Mode == 1;
		Driver->bFaceOut = Mode == 2;
		Driver->bUpdateActorRotation = true;
		Driver->Tick(0.125f);
		const FRotationMatrix Frame(Driver->GetActorRotation());
		FRotator Expected = (-Frame.GetUnitAxis(EAxis::Z)).Rotation()
			+ Driver->ActorRotationOffset * (360.0 / 65536.0);
		if (Mode != 0)
		{
			FVector Direction = Driver->GetActorLocation() + 25 * Frame.GetUnitAxis(EAxis::X)
				- Body->GetActorLocation();
			Expected = (Mode == 1 ? Direction : -Direction).Rotation();
		}
		CheckBody(*this, *Body, Expected.Yaw);
		TestTrue(TEXT("non-character orbit retains full driven rotation"),
			Other->GetActorRotation().Equals(Expected, 0.01));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBodyRotationEditorRestoreSpec,
	"WoT.Movement.BodyRotation.EditorRestore", RotationEntryFlags)
bool FWoTBodyRotationEditorRestoreSpec::RunTest(const FString&)
{
	FRotationEditorFixture Fixture;
	if (!TestTrue(TEXT("editor fixture initializes"), Fixture.Initialize())) return false;
	const FRotator Incoming(23, 71, 37);
	Fixture.Controller->SetControlRotation(Incoming);
	Fixture.Controller->CitadelBeginEdit(1);
	TestTrue(TEXT("orbit editing starts"), Fixture.Controller->IsEditingCitadel());
	Fixture.Body->SetActorLocation(FVector(900, 800, 700));
	Fixture.Controller->CitadelEndEdit();
	CheckBody(*this, *Fixture.Body, Incoming.Yaw);
	TestTrue(TEXT("authority restore preserves full view rotation"),
		Fixture.Controller->GetControlRotation().Equals(Incoming, 0.01));
	struct FRestoreParameters { FVector SavedLocation; FRotator SavedRotation; };
	FRestoreParameters Parameters{FVector(320, -160, 240), FRotator(-31, 113, -27)};
	UFunction* Restore = Fixture.Controller->FindFunction(TEXT("ClientRestoreCitadelEditTransform"));
	if (TestNotNull(TEXT("client restore RPC exists"), Restore))
	{
		Fixture.Controller->ProcessEvent(Restore, &Parameters);
		CheckBody(*this, *Fixture.Body, Parameters.SavedRotation.Yaw);
		TestTrue(TEXT("client restore preserves full view rotation"),
			Fixture.Controller->GetControlRotation().Equals(Parameters.SavedRotation, 0.01));
		ADefaultPawn* Other = Fixture.World->SpawnActor<ADefaultPawn>(
			FVector(2000, 0, 200), FRotator::ZeroRotator, RotationAlwaysSpawn());
		Fixture.Controller->Possess(Other);
		Parameters.SavedLocation = FVector(2000, 0, 240);
		Fixture.Controller->ProcessEvent(Restore, &Parameters);
		TestTrue(TEXT("non-character client restore preserves full body rotation"),
			Other->GetActorRotation().Equals(Parameters.SavedRotation, 0.01));
	}
	return !HasAnyErrors();
}

#endif
