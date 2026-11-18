// Automation specs for P34 — the Disguise visible morph (AngrealInvDisguise,
// exports/script/Angreal/AngrealInvDisguise.uc:179-458). Covers the live-look copy
// onto the caster (CopyDisguisePawnProperties), the exact snapshot revert
// (RestoreDisguisePawnProperties), the won't-fit collision abort with no charge
// drain (uc:317-321), the replicated display driver round-trip (the visible morph
// reaches every client), and the P02 personal-skin / faction-appearance interaction.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCharacter.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTAngrealInvDisguise.h"
#include "WoTTestBattleClasses.h"   // AWoTTestTeamAlpha (a distinctive source class)

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
	// Minimal ticking Standalone world (shared test-file convention; helper names
	// kept file-unique for the unity build).
	template <typename T>
	T* P34SpawnPawn(UWorld* World, const FVector& At)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		T* Pawn = World->SpawnActor<T>(At, FRotator::ZeroRotator, Params);
		if (Pawn)
		{
			Pawn->Health = 100;   // alive (GetClosestPawn skips dead pawns)
			Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		}
		return Pawn;
	}

	// Stamp distinctive LIVE look onto a source pawn so the copy is observable
	// without imported content: capsule, mesh scale, eye height, anim table, speeds.
	void P34MakeDistinctive(AWoTCharacter* Src)
	{
		Src->GetCapsuleComponent()->SetCapsuleSize(25.0f, 70.0f);
		Src->GetMesh()->SetRelativeScale3D(FVector(1.5f));
		Src->BaseEyeHeight = 63.0f;
		Src->AnimTableName = FName(TEXT("P34MimicTable"));
		UCharacterMovementComponent* Move = Src->GetCharacterMovement();
		Move->MaxWalkSpeed = 250.0f;
		Move->MaxSwimSpeed = 111.0f;
		Move->MaxAcceleration = 512.0f;
		Move->AirControl = 0.3f;
	}

	// A blocking slab (blocks the Pawn trace channel) for the won't-fit abort.
	AActor* P34SpawnBlocker(UWorld* World, const FVector& At, const FVector& Extent)
	{
		AActor* Blocker = World->SpawnActor<AActor>(At, FRotator::ZeroRotator);
		UBoxComponent* Box = NewObject<UBoxComponent>(Blocker, TEXT("Blocker"));
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Blocker->SetRootComponent(Box);
		Box->RegisterComponent();
		Blocker->SetActorLocation(At);
		return Blocker;
	}
}

// (a) The cast copies the mimicked pawn's LIVE class/scale/capsule/eye/anim/speeds
//     onto the caster and stamps the replicated driver.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP34CopiesLiveLook,
	"WoT.P34.CopiesLiveLook",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTP34CopiesLiveLook::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Caster = P34SpawnPawn<AWoTCharacter>(World, FVector(0, 0, 300));
	// A distinctive source of a different class (so SourceClass is observable).
	AWoTTestTeamAlpha* Mimic =
		P34SpawnPawn<AWoTTestTeamAlpha>(World, FVector(400, 0, 300));
	P34MakeDistinctive(Mimic);
	Mimic->PersonalSkin = FName(TEXT("Aes_Red"));

	const bool bOk = Caster->ApplyDisguiseFrom(Mimic);
	TestTrue(TEXT("morph succeeds (fits)"), bOk);
	TestTrue(TEXT("caster is now disguised"), Caster->IsDisguised());

	// The replicated driver carries the source's effective class + live scalars.
	TestTrue(TEXT("driver source class = the mimic's class"),
		Caster->Disguise.SourceClass == AWoTTestTeamAlpha::StaticClass());
	TestEqual(TEXT("driver source skin = the mimic's live skin"),
		Caster->Disguise.SourcePersonalSkin, FName(TEXT("Aes_Red")));

	// The caster's live look adopts the source's.
	TestEqual(TEXT("capsule half-height copied"),
		Caster->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), 70.0f);
	TestEqual(TEXT("capsule radius copied"),
		Caster->GetCapsuleComponent()->GetScaledCapsuleRadius(), 25.0f);
	TestEqual(TEXT("mesh scale copied"),
		Caster->GetMesh()->GetRelativeScale3D().X, 1.5);
	TestEqual(TEXT("base eye height copied"), Caster->BaseEyeHeight, 63.0f);
	TestEqual(TEXT("anim table copied (uc:369)"),
		Caster->AnimTableName, FName(TEXT("P34MimicTable")));
	TestEqual(TEXT("walk speed scaled to apparent class (uc:1928)"),
		Caster->GetCharacterMovement()->MaxWalkSpeed, 250.0f);
	TestEqual(TEXT("air control scaled to apparent class (uc:1930)"),
		Caster->GetCharacterMovement()->AirControl, 0.3f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (b) Revert restores the EXACT pre-disguise snapshot (uc:389-458).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP34RevertRestoresSnapshot,
	"WoT.P34.RevertRestoresSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTP34RevertRestoresSnapshot::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Caster = P34SpawnPawn<AWoTCharacter>(World, FVector(0, 0, 300));
	AWoTTestTeamAlpha* Mimic =
		P34SpawnPawn<AWoTTestTeamAlpha>(World, FVector(400, 0, 300));
	P34MakeDistinctive(Mimic);

	// The caster's own baseline (defaults: 17/46, eye 42, walk 400).
	const float BaseRadius = Caster->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float BaseHalfHeight = Caster->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float BaseEye = Caster->BaseEyeHeight;
	const float BaseWalk = Caster->GetCharacterMovement()->MaxWalkSpeed;
	const FVector BaseScale = Caster->GetMesh()->GetRelativeScale3D();

	Caster->ApplyDisguiseFrom(Mimic);
	Caster->RevertDisguise();

	TestFalse(TEXT("no longer disguised"), Caster->IsDisguised());
	TestEqual(TEXT("radius restored"),
		Caster->GetCapsuleComponent()->GetScaledCapsuleRadius(), BaseRadius);
	TestEqual(TEXT("half-height restored"),
		Caster->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), BaseHalfHeight);
	TestEqual(TEXT("eye height restored"), Caster->BaseEyeHeight, BaseEye);
	TestEqual(TEXT("walk speed restored"),
		Caster->GetCharacterMovement()->MaxWalkSpeed, BaseWalk);
	TestEqual(TEXT("mesh scale restored"),
		Caster->GetMesh()->GetRelativeScale3D().X, BaseScale.X);
	TestEqual(TEXT("anim table restored"), Caster->AnimTableName, FName(NAME_None));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (c) The won't-fit collision abort Fails the cast with NO charge drain (uc:317-321).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP34WontFitAbort,
	"WoT.P34.WontFitAbort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTP34WontFitAbort::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Caster = P34SpawnPawn<AWoTCharacter>(World, FVector(0, 0, 100));
	// A source whose taller capsule would lift the caster into the ceiling: with the
	// caster at Z=100 (half-height 46) the morph to half-height 120 shifts it up +74,
	// so its new top reaches ~Z=294 — into the blocker below.
	AWoTTestTeamAlpha* Mimic =
		P34SpawnPawn<AWoTTestTeamAlpha>(World, FVector(400, 0, 100));
	Mimic->GetCapsuleComponent()->SetCapsuleSize(17.0f, 120.0f);
	P34SpawnBlocker(World, FVector(0, 0, 250), FVector(100, 100, 50));   // Z 200..300

	// Direct API: the abort returns false and leaves the caster un-morphed.
	const float BaseHalfHeight = Caster->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const bool bOk = Caster->ApplyDisguiseFrom(Mimic);
	TestFalse(TEXT("won't-fit morph is rejected"), bOk);
	TestFalse(TEXT("caster is NOT disguised after the abort"), Caster->IsDisguised());
	TestEqual(TEXT("capsule restored after the abort"),
		Caster->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), BaseHalfHeight);

	// Artifact path: cast Fails and drains no charge (bCasting never arms the Tick).
	World->SpawnActor<AWoTAngrealInvDisguise>(FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	AWoTAngrealInvDisguise* Held =
		Cast<AWoTAngrealInvDisguise>(Caster->InventoryComponent->SelectedItem.Get());
	if (!TestNotNull(TEXT("disguise picked up"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const int32 Charges0 = Held->CurCharges;
	Caster->InventoryComponent->UseAngreal();
	TestFalse(TEXT("cast aborted -> not disguised"), Caster->IsDisguised());
	TestNull(TEXT("cast aborted -> no mimic recorded"), Held->DisguisedAs.Get());
	WoTTest::TickWorld(World, 1.2f);
	TestEqual(TEXT("won't-fit cast drains NO charge"), Held->CurCharges, Charges0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (d) The replicated driver round-trips: copying the driver struct onto a fresh
//     pawn and firing OnRep_Disguise reproduces (and reverts) the look — the
//     visible morph reaches simulated proxies without any owner/PIE dependency.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP34DriverReplicationRoundTrip,
	"WoT.P34.DriverReplicationRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTP34DriverReplicationRoundTrip::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Caster = P34SpawnPawn<AWoTCharacter>(World, FVector(0, 0, 300));
	AWoTTestTeamAlpha* Mimic =
		P34SpawnPawn<AWoTTestTeamAlpha>(World, FVector(400, 0, 300));
	P34MakeDistinctive(Mimic);
	Caster->ApplyDisguiseFrom(Mimic);

	// A stand-in remote proxy: receives ONLY the replicated driver, then runs OnRep.
	AWoTCharacter* Proxy = P34SpawnPawn<AWoTCharacter>(World, FVector(0, 0, 600));
	const float ProxyBaseHalf =
		Proxy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	Proxy->Disguise = Caster->Disguise;   // the replicated state arrives
	Proxy->OnRep_Disguise();
	TestTrue(TEXT("proxy applies the morph from the replicated driver"),
		Proxy->IsDisguised());
	TestEqual(TEXT("proxy capsule half-height matches the driver"),
		Proxy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), 70.0f);
	TestEqual(TEXT("proxy eye height matches the driver"),
		Proxy->BaseEyeHeight, 63.0f);
	TestEqual(TEXT("proxy anim table matches the driver"),
		Proxy->AnimTableName, FName(TEXT("P34MimicTable")));

	// Clearing the driver (bActive=0) reverts the proxy's local look.
	Proxy->Disguise = FWoTDisguiseState();
	Proxy->OnRep_Disguise();
	TestFalse(TEXT("proxy reverts when the driver clears"), Proxy->IsDisguised());
	TestEqual(TEXT("proxy capsule restored on revert"),
		Proxy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), ProxyBaseHalf);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (e) Disguise is orthogonal to the P02 personal-skin / faction-appearance state:
//     a disguised-then-reverted pawn keeps its own PersonalSkin and AppearanceClass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP34PersonalSkinIntact,
	"WoT.P34.PersonalSkinIntact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTP34PersonalSkinIntact::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Caster = P34SpawnPawn<AWoTCharacter>(World, FVector(0, 0, 300));
	Caster->PersonalSkin = FName(TEXT("Aes_Blue"));
	Caster->AppearanceClass = AWoTTestTeamAlpha::StaticClass();

	AWoTTestTeamAlpha* Mimic =
		P34SpawnPawn<AWoTTestTeamAlpha>(World, FVector(400, 0, 300));
	P34MakeDistinctive(Mimic);
	Mimic->PersonalSkin = FName(TEXT("Aes_Red"));

	Caster->ApplyDisguiseFrom(Mimic);
	// While disguised the effective skin is the SOURCE's; the caster's own is untouched.
	TestEqual(TEXT("effective skin while disguised = source's"),
		Caster->GetEffectivePersonalSkin(), FName(TEXT("Aes_Red")));
	TestEqual(TEXT("caster's own PersonalSkin untouched by the morph"),
		Caster->PersonalSkin, FName(TEXT("Aes_Blue")));

	Caster->RevertDisguise();
	TestEqual(TEXT("PersonalSkin intact after revert"),
		Caster->PersonalSkin, FName(TEXT("Aes_Blue")));
	TestTrue(TEXT("AppearanceClass intact after revert"),
		Caster->AppearanceClass == AWoTTestTeamAlpha::StaticClass());
	TestEqual(TEXT("effective skin back to the caster's own"),
		Caster->GetEffectivePersonalSkin(), FName(TEXT("Aes_Blue")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
