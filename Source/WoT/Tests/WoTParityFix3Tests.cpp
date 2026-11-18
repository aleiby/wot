#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTRandom.h"
#include "WoTTestWorld.h"
#include "WoTTestAIClasses.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTNPCWeapon.h"
#include "../Inventory/WoTIceLeech.h"
#include "../Inventory/WoTAngrealInvIce.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Projectiles/WoTArcherArrow.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerState.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTPayloadSubsystem.h"
#include "../RetailData/WoTDevRetailFamilies.h"
#include "../World/WoTRuntimeClassDefaults.h"

#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
	constexpr auto Fix3_Flags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter;

	AWoTNPCCharacter* Fix3_Creature(UWorld* World, const FName Row,
		const FVector& At = FVector(0.0, 0.0, 100.0))
	{
		AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(
			*World, Row, At, 1, true, AWoTTestNPCCharacter::StaticClass());
		if (NPC)
		{
			NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		}
		return NPC;
	}

	AWoTNPCController* Fix3_Controller(AWoTNPCCharacter* NPC)
	{
		return NPC ? Cast<AWoTNPCController>(NPC->GetController()) : nullptr;
	}

	AWoTCharacter* Fix3_Player(UWorld* World, const FVector& At,
		const uint8 Team = 2)
	{
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		APlayerController* PC = World->SpawnActor<APlayerController>();
		PC->InitPlayerState();
		if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>())
		{
			PS->Team = Team;
		}
		AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(
			At, FRotator::ZeroRotator, Spawn);
		PC->Possess(Player);
		Player->Health = 100000;
		Player->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		return Player;
	}

	bool Fix3_Until(UWorld* World, const float Seconds,
		TFunctionRef<bool()> Predicate)
	{
		for (int32 Frame = 0; Frame < FMath::CeilToInt(Seconds * 60.0f); ++Frame)
		{
			if (Predicate())
			{
				return true;
			}
			WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
		}
		return Predicate();
	}

	int32 Fix3_CountArcherArrows(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTArcherArrow> It(World); It; ++It)
		{
			Count += IsValid(*It) && !It->IsActorBeingDestroyed() ? 1 : 0;
		}
		return Count;
	}

	UWoTIceLeech* Fix3_FreezeWithRealIce(UWorld* World,
		AWoTCharacter* Caster, AWoTNPCCharacter* Victim,
		const float Timeout = 1.5f)
	{
		if (!World || !Caster || !Victim || !Caster->GetController()) return nullptr;
		Caster->GetController()->SetControlRotation(
			(Victim->GetActorLocation() - Caster->GetActorLocation()).Rotation());
		const FTransform At(Caster->GetActorLocation());
		AWoTAngrealInvIce* Ice = World->SpawnActorDeferred<AWoTAngrealInvIce>(
			AWoTAngrealInvIce::StaticClass(), At, Caster, Caster,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Ice) return nullptr;
		Ice->bHeldItem = true;
		Ice->FinishSpawning(At);
		Ice->CurCharges = 10;
		Ice->GiveTo(Caster);
		Caster->InventoryComponent->SelectItem(Ice);
		if (World->GetTimeSeconds() <= 0.0f)
		{
			WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
		}
		Caster->InventoryComponent->UseAngreal();
		UWoTIceLeech* Accepted = nullptr;
		Fix3_Until(World, Timeout, [&]()
		{
			Accepted = Victim->FindComponentByClass<UWoTIceLeech>();
			return Accepted != nullptr;
		});
		return Accepted;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFix3RandomFixedSequence,
	"WoT.Parity.Fix3.Random.FixedSequence", Fix3_Flags)

bool FWoTFix3RandomFixedSequence::RunTest(const FString& Parameters)
{
	(void)Parameters;
	WoTRandom::Seed(0x7051);
	const uint32 Expected[] = {
		0x73BD6830u,
		0xBF98B55Bu,
		0x55DADB42u,
		0xFA9CD015u,
		0x598A4BC4u,
		0x74D822FFu,
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Expected); ++Index)
	{
		TestEqual(FString::Printf(TEXT("stable draw %d"), Index),
			WoTRandom::GetUnsignedInt(), Expected[Index]);
	}
	// Retail appFrand grid: (draw >> 17) / 32767, i.e. 14814, 24524, 10989,
	// 32078, 11461 and 14956 out of RAND_MAX = 32767.
	const float ExpectedInclusiveFractions[] = {
		0.452101201f,
		0.748435915f,
		0.335367888f,
		0.978972733f,
		0.349772632f,
		0.456434816f,
	};
	WoTRandom::Seed(0x7051);
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(ExpectedInclusiveFractions); ++Index)
	{
		TestEqual(FString::Printf(TEXT("stable inclusive fraction %d"), Index),
			WoTRandom::FRand(), ExpectedInclusiveFractions[Index]);
	}
	// The maximum is as common as in retail (1 in 32768), not 1 in 2^32: a draw
	// whose top 15 bits are all set already yields exactly 1.0.
	{
		int32 Ones = 0;
		WoTRandom::Seed(0x7051);
		for (int32 Index = 0; Index < 32768 * 64; ++Index)
		{
			Ones += WoTRandom::FRand() == 1.0f ? 1 : 0;
		}
		TestTrue(FString::Printf(
			TEXT("inclusive 1.0 occurs at retail frequency (%d in %d draws, expect about 64)"),
			Ones, 32768 * 64), Ones >= 32 && Ones <= 112);
	}
	WoTRandom::Seed(0x7051);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFix3IceHeldFrameReplicates,
	"WoT.Parity.Fix3.Ice.HeldFrameReplicates", Fix3_Flags)

bool FWoTFix3IceHeldFrameReplicates::RunTest(const FString& Parameters)
{
	(void)Parameters;
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("Fix3 iced NPC held-frame replication"));
	if (!TestTrue(TEXT("dedicated server loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Worlds.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
		FVector(-8000.0, 0.0, 5000.0), FRotator::ZeroRotator, Spawn);
	if (!TestTrue(TEXT("presentation client connects"),
		Worlds.CreateAndConnectClient()))
	{
		return false;
	}

	AWoTNPCCharacter* ServerNPC = WoTCreatureData::SpawnCreature(
		*Worlds.Server.GetWorld(), TEXT("Trolloc"),
		FVector(0.0, 0.0, 5000.0), 1, true,
		AWoTNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("authority Trolloc"), ServerNPC))
	{
		return false;
	}
	ServerNPC->bAlwaysRelevant = true;
	ServerNPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	if (AController* Controller = ServerNPC->GetController())
	{
		Controller->SetActorTickEnabled(false);
	}
	UWoTVertexAnimComponent* ServerAnim =
		Cast<UWoTVertexAnimComponent>(ServerNPC->GetMesh());
	if (!TestNotNull(TEXT("authority vertex animation"), ServerAnim))
	{
		return false;
	}
	ServerNPC->ForceNetUpdate();

	AWoTNPCCharacter* ClientNPC = nullptr;
	UWoTVertexAnimComponent* ClientAnim = nullptr;
	TestTrue(TEXT("simulated proxy receives the Trolloc"),
		Worlds.TickUntil([&]()
		{
			for (TActorIterator<AWoTNPCCharacter> It(
				Worlds.Clients[0].GetWorld()); It; ++It)
			{
				if (It->CreatureRowName == TEXT("Trolloc"))
				{
					ClientNPC = *It;
					break;
				}
			}
			ClientAnim = ClientNPC
				? Cast<UWoTVertexAnimComponent>(ClientNPC->GetMesh()) : nullptr;
			return ClientNPC && ClientAnim
				&& ClientNPC->GetLocalRole() == ROLE_SimulatedProxy;
		}, 180));
	if (!ClientAnim)
	{
		return false;
	}
	FWoTAnimSequence FreezeSequence;
	FreezeSequence.Name = TEXT("Fix3Freeze");
	FreezeSequence.NumFrames = 30;
	FreezeSequence.Rate = 15.0f;
	ServerAnim->Sequences.Add(FreezeSequence);
	ClientAnim->Sequences.Add(FreezeSequence);
	ServerAnim->LoopAnim(FreezeSequence.Name, 1.0f);
	ServerNPC->ForceNetUpdate();
	TestTrue(TEXT("replicated test sequence advances on both roles"),
		Worlds.TickUntil([&]()
		{
			return ClientAnim->GetCurrentSequence() == FreezeSequence.Name
				&& ServerAnim->GetAnimFrame() > 0.15f
				&& ClientAnim->GetAnimFrame() > 0.05f;
		}, 180));

	UWoTIceLeech* Ice = NewObject<UWoTIceLeech>(ServerNPC);
	Ice->AttachTo(ServerNPC);
	ServerNPC->ForceNetUpdate();
	const float ServerHeldFrame = ServerAnim->GetAnimFrame();
	const FName HeldSequence = ServerAnim->GetCurrentSequence();
	const bool bClientReceivedHeldPose = Worlds.TickUntil([&]()
	{
		return ClientAnim->GetCurrentSequence() == HeldSequence
			&& ClientAnim->GetAnimFrame() > 0.05f
			&& FMath::IsNearlyEqual(
				ClientAnim->GetAnimFrame(), ServerHeldFrame, 1.0f / 2000.0f);
	}, 120);
	TestTrue(TEXT("client proxy receives the authority's nonzero frozen frame"),
		bClientReceivedHeldPose);
	const float ClientHeldFrame = ClientAnim->GetAnimFrame();
	Worlds.TickAll(30);
	TestTrue(TEXT("client proxy holds the replicated frozen frame"),
		FMath::IsNearlyEqual(ClientAnim->GetAnimFrame(), ClientHeldFrame,
			1.0f / 2000.0f));

	Ice->Unattach();
	ServerNPC->ForceNetUpdate();
	TestTrue(TEXT("client animation resumes after thaw"),
		Worlds.TickUntil([&]()
		{
			return !FMath::IsNearlyEqual(
				ClientAnim->GetAnimFrame(), ClientHeldFrame, 0.02f);
		}, 180));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFix3ArcherLostTargetRearm,
	"WoT.Parity.Fix3.Archer.LostTargetRearmsAfterSwordNotify", Fix3_Flags)

bool FWoTFix3ArcherLostTargetRearm::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTNPCCharacter* NPC = Fix3_Creature(World, TEXT("Archer"));
	AWoTCharacter* CloseTarget = Fix3_Player(World, FVector(900.0, 0.0, 100.0));
	AWoTNPCController* Controller = Fix3_Controller(NPC);
	UWoTArcherBowBehavior* BowBehavior = NPC
		? NPC->FindComponentByClass<UWoTArcherBowBehavior>() : nullptr;
	AWoTNPCWeapon* Sword = NPC ? NPC->GetMeleeWeapon() : nullptr;
	if (TestNotNull(TEXT("Archer controller"), Controller)
		&& TestNotNull(TEXT("Archer bow behavior"), BowBehavior)
		&& TestNotNull(TEXT("Archer sword"), Sword))
	{
		TestTrue(TEXT("Archer enters its real bow episode"),
			Fix3_Until(World, 4.0f, [&]()
			{
				return Controller->GetBrainState()
					== EWoTBrainState::PerformProjectile;
			}));
		WoTTest::TickWorld(World, 320, 1.0f / 60.0f);
		CloseTarget->SetActorLocation(
			NPC->GetActorLocation() + FVector(80.0, 0.0, 0.0));
		TestTrue(TEXT("real post-shot melee commit draws the sword"),
			Fix3_Until(World, 6.0f, [&]()
			{
				return BowBehavior->IsCommittedToMelee()
					&& NPC->GetHeldWeapon() == Sword;
			}));

		CloseTarget->Destroy();
		WoTTest::TickWorld(World, 10, 1.0f / 60.0f);
		WoTTest::TickWorld(World, 310, 1.0f / 60.0f);
		const int32 ArrowsBeforeReacquisition = Fix3_CountArcherArrows(World);
		Fix3_Player(World,
			NPC->GetActorLocation() + FVector(900.0, 0.0, 0.0));

		bool bSawSwordHeldBeforeNotify = false;
		bool bSawSwordRemovedByNotify = false;
		bool bArrowBeforeSwordRemoval = false;
		bool bArrowAfterSwordRemoval = false;
		for (int32 Frame = 0; Frame < 600; ++Frame)
		{
			WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
			if (NPC->GetReplicatedAnimName() == TEXT("DRAWSWRD"))
			{
				bSawSwordHeldBeforeNotify |= NPC->GetHeldWeapon() == Sword;
				bSawSwordRemovedByNotify |= NPC->GetHeldWeapon() != Sword;
			}
			if (Fix3_CountArcherArrows(World) > ArrowsBeforeReacquisition)
			{
				bArrowBeforeSwordRemoval |= !bSawSwordRemovedByNotify;
				bArrowAfterSwordRemoval |= bSawSwordRemovedByNotify;
			}
			if (bSawSwordHeldBeforeNotify && bSawSwordRemovedByNotify
				&& bArrowAfterSwordRemoval)
			{
				break;
			}
		}
		TestTrue(TEXT("distant reacquisition prepares the held sword"),
			bSawSwordHeldBeforeNotify);
		TestTrue(TEXT("DRAWSWRD notify removes the sword"),
			bSawSwordRemovedByNotify);
		TestFalse(TEXT("no arrow fires before the sword-removal notify"),
			bArrowBeforeSwordRemoval);
		TestTrue(TEXT("the component bow fires after the sword is removed"),
			bArrowAfterSwordRemoval);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFix3HearingRelationshipCache,
	"WoT.Parity.Fix3.Hearing.RelationshipResolutionCached", Fix3_Flags)

bool FWoTFix3HearingRelationshipCache::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTNPCCharacter* Maker = Fix3_Creature(
		World, TEXT("Trolloc"), FVector(500.0, 0.0, 100.0));
	if (!TestNotNull(TEXT("noise maker"), Maker))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Maker->AssignedTeam = 2;
	TArray<AWoTNPCCharacter*> RelatedListeners;
	for (int32 Index = 0; Index < 12; ++Index)
	{
		AWoTNPCCharacter* Listener = Fix3_Creature(World, TEXT("TrollocStrong"),
			FVector(0.0, Index * 20.0, 100.0));
		if (Listener)
		{
			Listener->AssignedTeam = 1;
			RelatedListeners.Add(Listener);
		}
	}
	// These unrelated rows must be rejected by the cheap gates before any class
	// relationship lookup: four are out of range and four are not hearing-live.
	for (int32 Index = 0; Index < 4; ++Index)
	{
		AWoTNPCCharacter* Far = Fix3_Creature(World, TEXT("Soldier"),
			FVector(10000.0, Index * 20.0, 100.0));
		if (Far) Far->AssignedTeam = 1;
		AWoTNPCCharacter* Dormant = Fix3_Creature(World, TEXT("Soldier"),
			FVector(0.0, 500.0 + Index * 20.0, 100.0));
		if (Dormant)
		{
			Dormant->AssignedTeam = 1;
			if (AWoTNPCController* Ctl = Fix3_Controller(Dormant))
			{
				Ctl->ForceDormant();
			}
		}
	}

	FString Error;
	const FWoTRuntimeClassDefaults* Defaults =
		UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(Error);
#if !WOT_GUARDED_BOOTSTRAP
	if (!Defaults) Defaults = WoTDevRetailClassDefaults();
#endif
	if (TestNotNull(TEXT("runtime class defaults"), Defaults))
	{
		Defaults->ResetRelationshipResolutionCountForTesting();
		Maker->EmitNoise(1.0f);
		TestEqual(TEXT("one row pair resolves once outside the observer fanout"),
			Defaults->GetRelationshipResolutionCountForTesting(), 1);
		if (TestTrue(TEXT("all related listeners spawned"),
			RelatedListeners.Num() == 12))
		{
			TestEqual(TEXT("related listener still hears the maker"),
				RelatedListeners[0]->Brain->GetGoal(EWoTGoalIndex::Threat)
					.GoalActor.Get(), static_cast<AActor*>(Maker));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFix3FallingWalkOffAdjustJump,
	"WoT.Parity.Fix3.Falling.WalkOffAdjustsJumpArc", Fix3_Flags)

bool FWoTFix3FallingWalkOffAdjustJump::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestProjectileCollisionTarget* Ledge =
		World->SpawnActor<AWoTTestProjectileCollisionTarget>(
			FVector(0.0, 0.0, 790.0), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("finite ledge"), Ledge))
	{
		Ledge->Box->SetBoxExtent(FVector(120.0, 200.0, 10.0));
		Ledge->Box->SetCollisionObjectType(ECC_WorldStatic);
		Ledge->Box->SetCollisionResponseToAllChannels(ECR_Block);
	}
	AWoTTestNPCCharacter* Guard = Cast<AWoTTestNPCCharacter>(
		Fix3_Creature(World, TEXT("Trolloc"), FVector(100.0, 0.0, 850.0)));
	AWoTCharacter* Target = Fix3_Player(World, FVector(2000.0, 0.0, 850.0));
	if (TestNotNull(TEXT("walk-off guard"), Guard)
		&& TestNotNull(TEXT("real pursuit target"), Target))
	{
		if (AWoTNPCController* Controller = Fix3_Controller(Guard))
		{
			Controller->ClearMovementPatternsForFix4Test();
			Guard->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
			Controller->TransitionOnGoalPriority(true);
		}
		UCharacterMovementComponent* Movement = Guard->GetCharacterMovement();
		Movement->SetMovementMode(MOVE_Walking);
		Guard->bCaptureWalkOffVelocityForFix3 = true;
		WoTTest::TickWorld(World, 0.1f);
		TestTrue(TEXT("real movement walks the guard off the finite ledge"),
			Fix3_Until(World, 4.0f, [&]()
			{
				return Guard->bFix3CapturedWalkOffVelocity;
			}));
		if (Guard->bFix3CapturedWalkOffVelocity)
		{
			TestTrue(FString::Printf(TEXT("AdjustJump restores full authored GroundSpeed (actual %.1f, retail %.1f, band %.1f..%.1f)"),
				Guard->Fix3WalkOffVelocity.Size2D(), Guard->RetailGroundSpeed,
				Guard->GroundSpeedMin, Guard->GroundSpeedMax),
				FMath::IsNearlyEqual(
					Guard->Fix3WalkOffVelocity.Size2D(), Guard->RetailGroundSpeed, 2.0f));
			TestTrue(TEXT("AdjustJump adds the retail JumpZ launch"),
				FMath::IsNearlyEqual(Guard->Fix3WalkOffVelocity.Z,
					Movement->JumpZVelocity, 1.0f));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFix4FallingWalkOffTargetsPursuit,
	"WoT.Parity.Fix4.NPCPursuit.WalkOffTargetsActiveMoveDestination", Fix3_Flags)

bool FWoTFix4FallingWalkOffTargetsPursuit::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestProjectileCollisionTarget* Ledge =
		World->SpawnActor<AWoTTestProjectileCollisionTarget>(
			FVector(0.0, 0.0, 790.0), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("finite ledge"), Ledge))
	{
		Ledge->Box->SetBoxExtent(FVector(120.0, 200.0, 10.0));
		Ledge->Box->SetCollisionObjectType(ECC_WorldStatic);
		Ledge->Box->SetCollisionResponseToAllChannels(ECR_Block);
	}
	AWoTTestNPCCharacter* Guard = Cast<AWoTTestNPCCharacter>(
		Fix3_Creature(World, TEXT("Trolloc"), FVector(100.0, 0.0, 850.0)));
	AWoTCharacter* Target = Fix3_Player(World, FVector(2000.0, 0.0, 850.0));
	if (TestNotNull(TEXT("walk-off guard"), Guard)
		&& TestNotNull(TEXT("real pursuit target"), Target))
	{
		if (AWoTNPCController* Controller = Fix3_Controller(Guard))
		{
			Controller->ClearMovementPatternsForFix4Test();
			Guard->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
			Controller->TransitionOnGoalPriority(true);
		}
		Guard->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		Guard->bCaptureWalkOffVelocityForFix3 = true;
		WoTTest::TickWorld(World, 0.1f);
		TestTrue(TEXT("ordinary pursuit walks the guard off the finite ledge"),
			Fix3_Until(World, 4.0f, [&]()
			{
				return Guard->bFix3CapturedWalkOffVelocity;
			}));
		if (Guard->bFix3CapturedWalkOffVelocity)
		{
			const FVector ExpectedDirection =
				(Target->GetActorLocation() - Guard->GetActorLocation())
				.GetSafeNormal2D();
			const FVector ActualDirection =
				Guard->Fix3WalkOffVelocity.GetSafeNormal2D();
			TestTrue(FString::Printf(
				TEXT("AdjustJump heads toward the live pursuit target (dot %.3f)"),
				FVector::DotProduct(ExpectedDirection, ActualDirection)),
				FVector::DotProduct(ExpectedDirection, ActualDirection) > 0.9f);
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFix3ArcherIceInterruptedRearm,
	"WoT.Parity.Fix3.Archer.IceInterruptedRearmRetries", Fix3_Flags)

bool FWoTFix3ArcherIceInterruptedRearm::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTNPCCharacter* NPC = Fix3_Creature(World, TEXT("Archer"));
	AWoTCharacter* CloseTarget = Fix3_Player(World, FVector(900.0, 0.0, 100.0));
	AWoTNPCController* Controller = Fix3_Controller(NPC);
	UWoTArcherBowBehavior* BowBehavior = NPC
		? NPC->FindComponentByClass<UWoTArcherBowBehavior>() : nullptr;
	AWoTNPCWeapon* Sword = NPC ? NPC->GetMeleeWeapon() : nullptr;
	if (TestNotNull(TEXT("Archer controller"), Controller)
		&& TestNotNull(TEXT("Archer bow behavior"), BowBehavior)
		&& TestNotNull(TEXT("Archer sword"), Sword))
	{
		TestTrue(TEXT("Archer enters its real bow episode"),
			Fix3_Until(World, 4.0f, [&]()
			{
				return Controller->GetBrainState()
					== EWoTBrainState::PerformProjectile;
			}));
		WoTTest::TickWorld(World, 320, 1.0f / 60.0f);
		CloseTarget->SetActorLocation(
			NPC->GetActorLocation() + FVector(80.0, 0.0, 0.0));
		TestTrue(TEXT("post-shot melee commit holds the sword"),
			Fix3_Until(World, 6.0f, [&]()
			{
				return BowBehavior->IsCommittedToMelee()
					&& NPC->GetHeldWeapon() == Sword;
			}));

		CloseTarget->Destroy();
		WoTTest::TickWorld(World, 320, 1.0f / 60.0f);
		AWoTCharacter* RangedTarget = Fix3_Player(World,
			NPC->GetActorLocation() + FVector(500.0, 0.0, 0.0));
		TestTrue(TEXT("ranged rearm enters DRAWSWRD before its notify"),
			Fix3_Until(World, 4.0f, [&]()
			{
				return Controller->GetBrainState() == EWoTBrainState::PrepareWeapon
					&& NPC->GetReplicatedAnimName() == TEXT("DRAWSWRD")
					&& NPC->GetHeldWeapon() == Sword;
			}));

		RangedTarget->SetActorLocation(
			NPC->GetActorLocation() + FVector(100.0, 0.0, 0.0));
		UWoTIceLeech* Ice = Fix3_FreezeWithRealIce(
			World, RangedTarget, NPC);
		const uint8 FrozenToken = NPC->RepAnim.PlayToken;
		TestNotNull(TEXT("real Ice projectile installs its leech during DRAWSWRD"), Ice);
		TestTrue(TEXT("Ice suspends the preparation controller"),
			Controller->IsSuspendedForIceFreeze());
		TestEqual(TEXT("interrupted notify leaves the sword held"),
			NPC->GetHeldWeapon(), Sword);
		RangedTarget->SetActorLocation(
			NPC->GetActorLocation() + FVector(500.0, 0.0, 0.0));
		const int32 ArrowsBeforeThaw = Fix3_CountArcherArrows(World);
		if (Ice) Ice->Unattach();

		bool bSawRetriedPreparation = false;
		bool bSawRetriedRemoval = false;
		bool bArrowBeforeRemoval = false;
		bool bArrowAfterRemoval = false;
		for (int32 Frame = 0; Frame < 600; ++Frame)
		{
			WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
			const bool bNewDraw = NPC->RepAnim.PlayToken != FrozenToken
				&& NPC->GetReplicatedAnimName() == TEXT("DRAWSWRD");
			bSawRetriedPreparation |= bNewDraw && NPC->GetHeldWeapon() == Sword;
			bSawRetriedRemoval |= bSawRetriedPreparation && bNewDraw
				&& NPC->GetHeldWeapon() != Sword;
			if (Fix3_CountArcherArrows(World) > ArrowsBeforeThaw)
			{
				bArrowBeforeRemoval |= !bSawRetriedRemoval;
				bArrowAfterRemoval |= bSawRetriedRemoval;
			}
			if (bSawRetriedRemoval && bArrowAfterRemoval) break;
		}
		TestTrue(TEXT("thaw retries DRAWSWRD from its beginning"),
			bSawRetriedPreparation);
		TestTrue(TEXT("retried notify removes the sword"), bSawRetriedRemoval);
		TestFalse(TEXT("no arrow fires before the retried notify"),
			bArrowBeforeRemoval);
		TestTrue(TEXT("bow fire resumes after the retried removal"),
			bArrowAfterRemoval);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFix4ArcherIceClearsPostShotContinuation,
	"WoT.Parity.Fix4.Archer.IceInterruptClearsPostShotContinuation", Fix3_Flags)

bool FWoTFix4ArcherIceClearsPostShotContinuation::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTNPCCharacter* NPC = Fix3_Creature(World, TEXT("Archer"));
	AWoTCharacter* Target = Fix3_Player(World, FVector(900.0, 0.0, 100.0));
	AWoTNPCController* Controller = Fix3_Controller(NPC);
	UWoTArcherBowBehavior* BowBehavior = NPC
		? NPC->FindComponentByClass<UWoTArcherBowBehavior>() : nullptr;
	if (TestNotNull(TEXT("Archer controller"), Controller)
		&& TestNotNull(TEXT("Archer bow behavior"), BowBehavior)
		&& TestNotNull(TEXT("real Ice caster target"), Target))
	{
		TestTrue(TEXT("Archer enters its real bow episode"),
			Fix3_Until(World, 4.0f, [&]()
			{
				return Controller->GetBrainState()
					== EWoTBrainState::PerformProjectile;
			}));
		// Cross the authored five-second minimum attack-state interval, then move
		// close so the completed release takes the real post-shot sword branch.
		WoTTest::TickWorld(World, 320, 1.0f / 60.0f);
		Target->SetActorLocation(
			NPC->GetActorLocation() + FVector(80.0, 0.0, 0.0));
		TestTrue(TEXT("post-shot continuation enters its DRAWSWRD preparation"),
			Fix3_Until(World, 6.0f, [&]()
			{
				return BowBehavior->IsCommittedToMelee()
					&& Controller->GetBrainState() == EWoTBrainState::PrepareWeapon
					&& NPC->GetReplicatedAnimName() == TEXT("DRAWSWRD");
			}));

		UWoTIceLeech* Ice = Fix3_FreezeWithRealIce(World, Target, NPC);
		TestNotNull(TEXT("real Ice projectile interrupts post-shot DRAWSWRD"), Ice);
		TestTrue(TEXT("Ice suspends the post-shot preparation"),
			Controller->IsSuspendedForIceFreeze());
		const int32 ArrowsBeforeThaw = Fix3_CountArcherArrows(World);
		Target->Destroy();
		if (Ice)
		{
			Ice->Unattach();
		}
		// Losing the close target follows the shipped no-switch arm. Once the
		// five-second weapon-switch gate expires, a new distant threat makes the
		// component bow the real best weapon and starts the episode under test.
		WoTTest::TickWorld(World, 320, 1.0f / 60.0f);
		Fix3_Player(World, NPC->GetActorLocation() + FVector(900.0, 0.0, 0.0));

		bool bSawFirstRangedEpisode = false;
		bool bFirstEpisodeStartedAtKneel = false;
		bool bFirstEpisodeFired = false;
		for (int32 Frame = 0; Frame < 900; ++Frame)
		{
			WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
			if (!bSawFirstRangedEpisode
				&& Controller->GetBrainState() == EWoTBrainState::PerformProjectile)
			{
				bSawFirstRangedEpisode = true;
				bFirstEpisodeStartedAtKneel =
					Controller->GetArcherAttackPhaseName() == TEXT("Kneel");
			}
			if (bSawFirstRangedEpisode
				&& Fix3_CountArcherArrows(World) > ArrowsBeforeThaw)
			{
				bFirstEpisodeFired = true;
				break;
			}
			if (bSawFirstRangedEpisode
				&& Controller->GetBrainState() != EWoTBrainState::PerformProjectile)
			{
				break;
			}
		}
		TestTrue(TEXT("thaw reaches a new ranged episode"), bSawFirstRangedEpisode);
		TestTrue(TEXT("the first ranged episode starts at Kneel rather than stale GetUp"),
			bFirstEpisodeStartedAtKneel);
		TestTrue(TEXT("the first ranged episode after thaw fires its arrow"),
			bFirstEpisodeFired);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
