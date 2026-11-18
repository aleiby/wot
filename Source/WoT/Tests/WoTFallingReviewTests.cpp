#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCWeapon.h"
#include "../AI/WoTNPCWeaponData.h"
#include "../WoTCarcass.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTMover.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTVertexAnimComponent.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeWorldProperties.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "UObject/UnrealType.h"
#if !WOT_GUARDED_BOOTSTRAP
#include "../RetailData/WoTDevRetailFamilies.h"
#endif

namespace
{
constexpr EAutomationTestFlags FallingReviewFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

void AddFallingReviewFloor(UWorld* World, bool bMover = false)
{
	AActor* Floor = bMover ? World->SpawnActor<AWoTMover>() : World->SpawnActor<AActor>();
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
	Box->SetBoxExtent(FVector(1000.0, 1000.0, 50.0));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Box->SetCollisionObjectType(bMover ? ECC_WorldDynamic : ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Floor->SetRootComponent(Box);
	Box->RegisterComponent();
	Floor->SetActorLocation(FVector(0.0, 0.0, -50.0));
}

TArray<FName> FallingReviewWeaponTypes()
{
	TArray<FName> Types;
	WoTNPCWeaponData::GetAllWeaponTypes(Types);
	Types.RemoveAll([](FName Type)
	{
		return WoTNPCWeaponData::Find(Type)->TossedDecorationType.IsNone();
	});
	Types.Sort(FNameLexicalLess());
	return Types;
}

float FallingReviewDefault(const FWoTRuntimeClassDefaults& Defaults,
	const FString& Class, const TCHAR* Name, float Fallback)
{
	if (const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(Class, Name))
	{
		WoTRuntimeWorldPropertyFloat(*Property, Fallback);
	}
	return Fallback;
}

struct FFallingDeathObservation
{
	TArray<FVector> WeaponPositions;
	FVector CarcassLocation = FVector::ZeroVector;
};

FFallingDeathObservation ObserveFallingDeath(FAutomationTestBase& Test, bool bSeparateDrops)
{
	FFallingDeathObservation Result;
	UWorld* World = WoTTest::CreateSpecWorld();
	AddFallingReviewFloor(World);
	TArray<FName> Rows;
	WoTCreatureData::GetAllRowNames(Rows);
	Rows.Sort(FNameLexicalLess());
	FName TwoWeaponRow;
	for (FName Name : Rows)
	{
		const FWoTCreatureRow* Row = WoTCreatureData::Find(Name);
		if (Row && Row->MeleeWeaponType != Row->RangedWeaponType
			&& WoTNPCWeaponData::Find(Row->MeleeWeaponType)
			&& WoTNPCWeaponData::Find(Row->RangedWeaponType))
		{
			TwoWeaponRow = Name;
			break;
		}
	}
	AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(
		*World, TwoWeaponRow, FVector(0.0, 0.0, 100.0), 1);
	if (!Test.TestNotNull(TEXT("shipped two-weapon NPC"), NPC))
	{
		WoTTest::DestroySpecWorld(World);
		return Result;
	}
	NPC->Health = 0;
	NPC->Died(nullptr, TEXT("hurt"), NPC->GetActorLocation());
	TArray<AWoTTossedWeapon*> Drops;
	for (TActorIterator<AWoTTossedWeapon> It(World); It; ++It) Drops.Add(*It);
	Drops.Sort([](const AWoTTossedWeapon& A, const AWoTTossedWeapon& B)
	{
		return A.WeaponTypeName.LexicalLess(B.WeaponTypeName);
	});
	Test.TestEqual(TEXT("ordinary death creates both shipped tossed weapons"), Drops.Num(), 2);
	for (int32 Index = 0; Index < Drops.Num(); ++Index)
	{
		// Identical launch vectors hold the death drops together. The control uses
		// separate columns over the same floor, preserving all collision settings.
		Drops[Index]->Movement->Velocity = FVector(0.0, 0.0, 260.0);
		if (bSeparateDrops) Drops[Index]->AddActorWorldOffset(FVector(300.0 * (Index + 1), 0.0, 0.0));
	}
	AWoTCarcass* Carcass = nullptr;
	for (int32 Frame = 0; Frame < 420; ++Frame)
	{
		WoTTest::TickWorld(World, 1);
		for (int32 Index = 0; Index < Drops.Num(); ++Index)
		{
			Result.WeaponPositions.Add(Drops[Index]->GetActorLocation()
				- FVector(bSeparateDrops ? 300.0 * (Index + 1) : 0.0, 0.0, 0.0));
		}
		for (TActorIterator<AWoTCarcass> It(World); It; ++It) Carcass = *It;
	}
	for (AWoTTossedWeapon* Drop : Drops)
	{
		Test.TestTrue(TEXT("each death drop settles on the floor"),
			Drop->Movement->Velocity.IsNearlyZero()
			&& FMath::IsNearlyEqual(Drop->GetActorLocation().Z,
				(double)Drop->Collision->GetUnscaledCapsuleHalfHeight(), 0.2));
	}
	if (Test.TestNotNull(TEXT("death animation produces its real carcass"), Carcass))
	{
		Test.TestTrue(TEXT("the real carcass settles"), Carcass->IsLyingStill());
		Result.CarcassLocation = Carcass->GetActorLocation();
	}
	WoTTest::DestroySpecWorld(World);
	return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingDeathDropsSpec,
	"WoT.Falling.Review.DeathDropsDoNotBlockActors", FallingReviewFlags)
bool FWoTFallingDeathDropsSpec::RunTest(const FString&)
{
	const FFallingDeathObservation Separate = ObserveFallingDeath(*this, true);
	const FFallingDeathObservation Together = ObserveFallingDeath(*this, false);
	if (TestEqual(TEXT("both ticking deaths were observed"),
		Together.WeaponPositions.Num(), Separate.WeaponPositions.Num()))
	{
		double MaxDisplacement = 0.0;
		for (int32 Index = 0; Index < Together.WeaponPositions.Num(); ++Index)
		{
			MaxDisplacement = FMath::Max(MaxDisplacement, FVector::Distance(
				Together.WeaponPositions[Index], Separate.WeaponPositions[Index]));
		}
		TestTrue(FString::Printf(TEXT("overlapping death drops follow independent trajectories (drift %.3f)"),
			MaxDisplacement), MaxDisplacement < 0.2);
	}
	TestTrue(FString::Printf(TEXT("the carcass reaches the same floor with both drops underneath (height difference %.3f)"),
		Together.CarcassLocation.Z - Separate.CarcassLocation.Z),
		Together.CarcassLocation.Equals(Separate.CarcassLocation, 0.2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingTossedMoverSpec,
	"WoT.Falling.Review.TossedWeaponStillBlocksMovers", FallingReviewFlags)
bool FWoTFallingTossedMoverSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AddFallingReviewFloor(World, true);
	AWoTNPCWeapon* Source = World->SpawnActor<AWoTNPCWeapon>();
	const TArray<FName> Types = FallingReviewWeaponTypes();
	if (!TestTrue(TEXT("shipped weapon registry is available"), !Types.IsEmpty()))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Source->ConfigureFromType(Types[0]);
	AWoTTossedWeapon* Drop = World->SpawnActor<AWoTTossedWeapon>(FVector(0.0, 0.0, 80.0), FRotator::ZeroRotator);
	Drop->InitFromWeapon(*Source);
	WoTTest::TickWorld(World, 4.0f);
	TestTrue(TEXT("a shipped weapon rests on a dynamic mover"),
		Drop->Movement->Velocity.IsNearlyZero() && FMath::IsNearlyEqual(
			Drop->GetActorLocation().Z, (double)Drop->Collision->GetUnscaledCapsuleHalfHeight(), 0.2));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FWoTFallingAuthoredLandingSpec,
	"WoT.Falling.Review.AuthoredLanding", FallingReviewFlags)
void FWoTFallingAuthoredLandingSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	// One case for every shipped death-toss decoration. Resolve identities from
	// retail data at runtime so public test names need no setting-specific names.
	for (int32 Index = 0; Index < 7; ++Index)
	{
		Names.Add(FString::Printf(TEXT("Class%02d"), Index + 1));
		Commands.Add(FString::FromInt(Index));
	}
}

bool FWoTFallingAuthoredLandingSpec::RunTest(const FString& Parameters)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AddFallingReviewFloor(World);
	const TArray<FName> Types = FallingReviewWeaponTypes();
	FString Error;
	const FWoTRuntimeClassDefaults* Defaults = UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(Error);
#if !WOT_GUARDED_BOOTSTRAP
	if (!Defaults) Defaults = WoTDevRetailClassDefaults();
#endif
	const int32 Index = FCString::Atoi(*Parameters);
	if (!TestEqual(TEXT("all seven shipped death-toss classes have individual cases"), Types.Num(), 7)
		|| !TestNotNull(TEXT("active shipped class defaults"), Defaults) || !Types.IsValidIndex(Index))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTNPCWeapon* Source = World->SpawnActor<AWoTNPCWeapon>();
	Source->ConfigureFromType(Types[Index]);
	const FString Decoration = Source->TossedDecorationType.ToString();
	const FRotator Authored(
		FallingReviewDefault(*Defaults, Decoration, TEXT("LandedPitch"), -1.0f),
		FallingReviewDefault(*Defaults, Decoration, TEXT("LandedYaw"), -1.0f),
		FallingReviewDefault(*Defaults, Decoration, TEXT("LandedRoll"), -1.0f));
	TestTrue(TEXT("selected shipped decoration authors landing angles"),
		Authored.Pitch >= 0.0 || Authored.Yaw >= 0.0 || Authored.Roll >= 0.0);
	for (const FRotator Initial : {FRotator(25.0, 40.0, 35.0), FRotator(-25.0, 220.0, -145.0)})
	{
		AWoTTossedWeapon* Drop = World->SpawnActor<AWoTTossedWeapon>(FVector(0.0, 0.0, 80.0), Initial);
		Drop->InitFromWeapon(*Source);
		const FRotator BeforeLanding = Drop->TossedVisualRotation;
		// Util.uc:187-205 chooses the nearer authored angle or its opposite.
		auto ExpectedAxis = [](double Current, double Desired)
		{
			if (Desired < 0.0) return Current;
			const double Degrees = Desired * 360.0 / 65536.0;
			return FMath::Abs(FMath::FindDeltaAngleDegrees(Current, Degrees)) < 90.0
				? Degrees : Degrees + 180.0;
		};
		const FRotator Expected(ExpectedAxis(BeforeLanding.Pitch, Authored.Pitch),
			ExpectedAxis(BeforeLanding.Yaw, Authored.Yaw), ExpectedAxis(BeforeLanding.Roll, Authored.Roll));
		WoTTest::TickWorld(World, 1);
		TestTrue(TEXT("launch preserves held presentation until floor contact"),
			Drop->DisplayMesh->GetComponentQuat().Equals(BeforeLanding.Quaternion(), 0.001));
		WoTTest::TickWorld(World, 4.0f);
		TestTrue(TEXT("shipped collision height is used at rest"),
			FMath::IsNearlyEqual(Drop->GetActorLocation().Z,
				(double)FallingReviewDefault(*Defaults, Decoration, TEXT("CollisionHeight"), 4.0f), 0.2));
		TestTrue(TEXT("collision stays world-up after authored landing"), Drop->GetActorUpVector().Equals(FVector::UpVector));
		TestTrue(TEXT("floor contact applies the selected decoration's nearest landing angles"),
			Drop->DisplayMesh->GetComponentQuat().Equals(Expected.Quaternion(), 0.001));
		TestTrue(TEXT("settled rotation is stored in the replicated payload"),
			Drop->TossedVisualRotation.Quaternion().Equals(Expected.Quaternion(), 0.001));

		// Exercise the actual receive notification on a simulated proxy, including
		// a late presentation refresh. This is a replication seam, not a socket test.
		AWoTTossedWeapon* Proxy = World->SpawnActor<AWoTTossedWeapon>(FVector(500.0, 0.0, 80.0), Initial);
		Proxy->SetRole(ROLE_SimulatedProxy);
		Proxy->Movement->Deactivate();
		Proxy->WeaponTypeName = Drop->WeaponTypeName;
		Proxy->TossedVisualRotation = Drop->TossedVisualRotation;
		FProperty* RotationProperty = FindFProperty<FProperty>(Proxy->GetClass(), TEXT("TossedVisualRotation"));
		if (TestNotNull(TEXT("visual rotation property"), RotationProperty))
		{
			TestTrue(TEXT("visual rotation is a replicated property"),
				RotationProperty->HasAnyPropertyFlags(CPF_Net));
			UFunction* Notify = Proxy->FindFunction(RotationProperty->RepNotifyFunc);
			if (TestNotNull(TEXT("visual rotation receive notification"), Notify))
			{
				Proxy->ProcessEvent(Notify, nullptr);
				Proxy->SetActorRotation(FRotator(0.0, 125.0, 0.0));
				WoTTest::TickWorld(World, 1);
				TestTrue(TEXT("a later movement snapshot does not rotate the settled visual child"),
					Proxy->DisplayMesh->GetComponentQuat().Equals(Expected.Quaternion(), 0.001));
				Proxy->ProcessEvent(Notify, nullptr);
				TestTrue(TEXT("a receiving client retains the settled visual rotation"),
					Proxy->DisplayMesh->GetComponentQuat().Equals(Expected.Quaternion(), 0.001));
			}
		}
		Proxy->Destroy();
		Drop->Destroy();
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
