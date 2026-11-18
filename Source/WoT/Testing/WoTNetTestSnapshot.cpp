#include "WoTNetTestSnapshot.h"
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES

#include "EngineUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Components/CapsuleComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/OverlapResult.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Info.h"
#include "Camera/PlayerCameraManager.h"
#include "../WoTBattleInfo.h"
#include "../WoTBootstrapHeartbeat.h"
#include "../WoTBootstrapSubsystem.h"
#include "../WoTSessionSubsystem.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTGameState.h"
#include "../WoTSealAltar.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTVertexAnimComponent.h"
#include "../Projectiles/WoTProjectile.h"
#include "../Projectiles/WoTSeekingProjectile.h"
#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTPathNode.h"
#include "../Traps/WoTAlarm.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTHandInfo.h"
#include "../Inventory/WoTHandSet.h"
#include "../Inventory/WoTInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../World/WoTMapDisplay.h"
#include "../Inventory/WoTLeech.h"
#include "../Inventory/WoTReflector.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace
{
	double R1(double V) { return FMath::RoundToDouble(V * 10.0) / 10.0; }

	void SetVec(const TSharedPtr<FJsonObject>& O, const FVector& L, const FVector& V)
	{
		O->SetNumberField(TEXT("x"), R1(L.X));
		O->SetNumberField(TEXT("y"), R1(L.Y));
		O->SetNumberField(TEXT("z"), R1(L.Z));
		O->SetNumberField(TEXT("vx"), R1(V.X));
		O->SetNumberField(TEXT("vy"), R1(V.Y));
		O->SetNumberField(TEXT("vz"), R1(V.Z));
	}

	const TCHAR* ItemStateName(EWoTInventoryState State)
	{
		switch (State)
		{
		case EWoTInventoryState::Pickup:   return TEXT("Pickup");
		case EWoTInventoryState::Sleeping: return TEXT("Sleeping");
		case EWoTInventoryState::Held:     return TEXT("Held");
		default:                           return TEXT("?");
		}
	}

	TArray<TSharedPtr<FJsonValue>> ToStringArray(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In) { Out.Add(MakeShared<FJsonValueString>(S)); }
		return Out;
	}
}

TSharedPtr<FJsonObject> WoTNetTest::BuildSnapshot(UWorld* World)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	if (!IsValid(World))
	{
		return Root;
	}
	Root->SetNumberField(TEXT("netmode"), (int32)World->GetNetMode());
	Root->SetStringField(TEXT("map"), World->GetMapName().Replace(TEXT("UEDPIE_0_"), TEXT("")));
	// The map sweep's name/title surface: `map` stays the raw world leaf (a
	// guarded shell reports WoTMapShellNN); these are what the UI must show.
	Root->SetStringField(TEXT("map_identity"), WoTWorldMapIdentity(World));
	Root->SetStringField(TEXT("map_title"), WoTWorldDisplayTitle(World));
	Root->SetBoolField(TEXT("map_title_retail"), WoTWorldHasRetailLevelTitle(World));
	Root->SetNumberField(TEXT("time"), World->GetTimeSeconds());

	// Live bootstrap phase surface: the packaged stall diagnosis depends on
	// interrogating a wedged instance over this channel.
	if (const UGameInstance* GameInstance = World->GetGameInstance())
	{
		if (const UWoTBootstrapSubsystem* Bootstrap =
			GameInstance->GetSubsystem<UWoTBootstrapSubsystem>())
		{
			const FWoTBootstrapSnapshot State = Bootstrap->Snapshot();
			TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
			B->SetStringField(TEXT("phase"), WoTBootstrapPhaseName(State.Phase));
			B->SetNumberField(TEXT("epoch"), (double)State.OperationEpoch);
			B->SetNumberField(TEXT("completed"), (double)State.CompletedUnits);
			B->SetNumberField(TEXT("total"), (double)State.TotalUnits);
			B->SetBoolField(TEXT("started"), Bootstrap->HasStarted());
			B->SetStringField(TEXT("diagnostic"),
				FWoTBootstrapViewModel::DiagnosticCodeText(
					State.DiagnosticCode).ToString());
			Root->SetObjectField(TEXT("bootstrap"), B);
		}
		if (const UWoTSessionSubsystem* Session =
			GameInstance->GetSubsystem<UWoTSessionSubsystem>())
		{
			// Host-path gate readout: the packaged guarded client is log-less,
			// so host refusals are only diagnosable through this surface.
			Root->SetStringField(TEXT("session"), Session->HostDiagnostics());
		}
	}

	TArray<TSharedPtr<FJsonValue>> Pawns, Projs, Pickups;

	for (TActorIterator<APawn> It(World); It; ++It)
	{
		APawn* P = *It;
		if (!IsValid(P)) { continue; }
		TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("name"), P->GetName());
		J->SetStringField(TEXT("cls"), P->GetClass()->GetName());
		J->SetNumberField(TEXT("role"), (int32)P->GetLocalRole());
		J->SetBoolField(TEXT("ctrl"), P->GetController() != nullptr);
		// On a LISTEN server every player's pawn has a controller, so `ctrl`
		// cannot name the host's own pawn; `local` can (a client's own pawn is
		// both). The two-instance seal-match harness keys on it.
		J->SetBoolField(TEXT("local"), P->IsLocallyControlled());
		if (const UGameInstance* GameInstance = World->GetGameInstance())
		{
			for (int32 LocalIndex = 0; LocalIndex < GameInstance->GetNumLocalPlayers(); ++LocalIndex)
			{
				const ULocalPlayer* LocalPlayer = GameInstance->GetLocalPlayerByIndex(LocalIndex);
				if (LocalPlayer && LocalPlayer->PlayerController == P->GetController())
				{
					J->SetNumberField(TEXT("lp"), LocalIndex);
					break;
				}
			}
		}
		if (const APlayerState* PS = P->GetPlayerState())
		{
			J->SetStringField(TEXT("player"), PS->GetPlayerName());
			// PlayerState.Team replicates unconditionally (all clients see every team). Read it
			// off the pawn so a position-matched pawn (pawn_on) carries a robust team identity —
			// the players[] array keys on the shared-nickname player name, which collides.
			if (const AWoTPlayerState* WPS = Cast<AWoTPlayerState>(PS))
			{
				J->SetNumberField(TEXT("team"), WPS->Team);
			}
		}
		SetVec(J, P->GetActorLocation(), P->GetVelocity());
		if (const AWoTCharacter* C = Cast<AWoTCharacter>(P))
		{
			J->SetNumberField(TEXT("health"), C->Health);
			J->SetBoolField(TEXT("dead"), C->bDead);
			// The death-shutdown observable: a killed proxy drops its capsule
			// collision (ShutdownCorpsePresentation), so a false value on a dead
			// pawn proves the client-side reaction ran.
			J->SetBoolField(TEXT("colliding"),
				C->GetCapsuleComponent()->GetCollisionEnabled() != ECollisionEnabled::NoCollision);
			// Overlap-path diagnostics: the engine only generates overlaps for a
			// begun-play owner with a live physics body; `overlaps` is what the
			// capsule has registered, `query` what a fresh WorldDynamic overlap
			// query at the capsule finds (separates "no event" from "no body").
			{
				const UCapsuleComponent* Capsule = C->GetCapsuleComponent();
				J->SetBoolField(TEXT("begunplay"), C->HasActorBegunPlay());
				J->SetBoolField(TEXT("initialized"), C->IsActorInitialized());
				J->SetBoolField(TEXT("physics_state"), Capsule->IsPhysicsStateCreated());
				J->SetBoolField(TEXT("gen_overlaps"), Capsule->GetGenerateOverlapEvents());
				TArray<FString> Registered;
				for (const FOverlapInfo& Info : Capsule->GetOverlapInfos())
				{
					if (const UPrimitiveComponent* Other = Info.OverlapInfo.GetComponent())
					{
						Registered.Add(GetNameSafe(Other->GetOwner()) + TEXT(".") + Other->GetName());
					}
				}
				J->SetArrayField(TEXT("overlaps"), ToStringArray(Registered));
				TArray<FOverlapResult> Hits;
				FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTNetTestOverlapProbe), false, C);
				World->OverlapMultiByObjectType(Hits, C->GetActorLocation(), FQuat::Identity,
					FCollisionObjectQueryParams(ECC_WorldDynamic),
					FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),
						Capsule->GetScaledCapsuleHalfHeight()), Params);
				TArray<FString> Queried;
				for (const FOverlapResult& Hit : Hits)
				{
					const UPrimitiveComponent* Other = Hit.GetComponent();
					const AActor* Owner = Hit.GetActor();
					Queried.Add(FString::Printf(TEXT("%s.%s begun=%d init=%d gen=%d"),
						*GetNameSafe(Owner), *GetNameSafe(Other),
						Owner && Owner->HasActorBegunPlay() ? 1 : 0,
						Owner && Owner->IsActorInitialized() ? 1 : 0,
						Other && Other->GetGenerateOverlapEvents() ? 1 : 0));
				}
				J->SetArrayField(TEXT("query"), ToStringArray(Queried));
			}
			// Movement diagnostics: MovementMode (MOVE_None=0/Walking=1/Falling=3/...)
			// and whether the controller ignores move input — to distinguish a frozen
			// controller from a stuck/None movement component.
			if (const UCharacterMovementComponent* CM = C->GetCharacterMovement())
			{
				J->SetNumberField(TEXT("movemode"), (int32)CM->MovementMode.GetValue());
				// Floor diagnostics: the full identity of what the
				// CMC's FindFloor hit, so a bad floor (bWalkableFloor=false, FloorDist=
				// -CapsuleRadius) can be attributed to an actual actor/component instead
				// of guessed at. CurrentFloor refreshes every walking tick.
				{
					const FFindFloorResult& F = CM->CurrentFloor;
					TSharedPtr<FJsonObject> FJ = MakeShared<FJsonObject>();
					FJ->SetBoolField(TEXT("walkable"), F.bWalkableFloor);
					FJ->SetBoolField(TEXT("blocking"), F.bBlockingHit);
					FJ->SetBoolField(TEXT("line"), F.bLineTrace);
					FJ->SetNumberField(TEXT("dist"), F.FloorDist);
					FJ->SetNumberField(TEXT("linedist"), F.LineDist);
					const FHitResult& H = F.HitResult;
					FJ->SetBoolField(TEXT("startpen"), H.bStartPenetrating);
					FJ->SetNumberField(TEXT("time"), H.Time);
					FJ->SetNumberField(TEXT("pendepth"), H.PenetrationDepth);
					FJ->SetStringField(TEXT("actor"), GetNameSafe(H.GetActor()));
					FJ->SetStringField(TEXT("comp"), GetNameSafe(H.GetComponent()));
					FJ->SetNumberField(TEXT("impactz"), H.ImpactPoint.Z);
					FJ->SetNumberField(TEXT("normalz"), H.ImpactNormal.Z);
					J->SetObjectField(TEXT("floor"), FJ);
				}
			}
			if (const AController* Ctrl = P->GetController())
			{
				J->SetBoolField(TEXT("moveignored"), Ctrl->IsMoveInputIgnored());
				const FRotator Control = Ctrl->GetControlRotation();
				J->SetNumberField(TEXT("control_yaw"), R1(Control.Yaw));
				J->SetNumberField(TEXT("control_pitch"), R1(Control.Pitch));
			}
			// Anim = the replicated intent (stable sequence NAME, observable without a
			// mesh bound — the meta.json id mapping is presentation). Fall back to the
			// component's live sequence only for the owner's own view, where RepAnim is
			// COND_SkipOwner-suppressed and the owning client self-animates locally.
			FName AnimName = C->GetReplicatedAnimName();
			if (AnimName.IsNone())
			{
				if (const UWoTVertexAnimComponent* A = C->FindComponentByClass<UWoTVertexAnimComponent>())
				{
					if (A->IsAnimating())
					{
						AnimName = A->GetCurrentSequence();
					}
				}
			}
			J->SetStringField(TEXT("anim"), AnimName.ToString());
			// The bound body mesh (the faction bind): a non-owner view
			// reporting the faction mesh name proves the CLIENT-side ApplyFactionAppearance
			// ran (the bind is local per instance — nothing replicates the mesh itself).
			const UWoTVertexAnimComponent* Body =
				Cast<UWoTVertexAnimComponent>(C->GetMesh());
			J->SetStringField(TEXT("mesh"), Body && Body->HasDisplayMesh()
				? Body->GetDisplayMeshName().ToString() : TEXT("None"));
			TArray<UWoTLeech*> Leeches;
			C->GetComponents<UWoTLeech>(Leeches);
			TArray<FString> LNames;
			for (const UWoTLeech* L : Leeches) { if (IsValid(L)) { LNames.Add(L->GetClass()->GetName()); } }
			TArray<UWoTReflector*> Reflectors;
			C->GetComponents<UWoTReflector>(Reflectors);
			TArray<FString> RNames;
			for (const UWoTReflector* Rf : Reflectors) { if (IsValid(Rf)) { RNames.Add(Rf->GetClass()->GetName()); } }
			J->SetArrayField(TEXT("leeches"), ToStringArray(LNames));
			J->SetArrayField(TEXT("reflectors"), ToStringArray(RNames));
			// Inventory (Items/SelectedItem replicate COND_OwnerOnly): a client
			// sees its own pawn's chain but 0 items / no selection for others.
			// The diagnostic reads these to spot inventory-replication gaps.
			if (const UWoTInventoryComponent* Inv = C->FindComponentByClass<UWoTInventoryComponent>())
			{
				const AWoTInventory* Sel = Inv->SelectedItem;
				J->SetStringField(TEXT("selected"), Sel ? Sel->GetClass()->GetName() : TEXT(""));
				J->SetNumberField(TEXT("invcount"), Inv->Items.Num());
				// The pickup sweep asserts a touched pickup's class enters the
				// chain on the server AND on the owning client.
				TArray<TSharedPtr<FJsonValue>> Held;
				for (const AWoTInventory* Item : Inv->Items)
				{
					if (!IsValid(Item)) { continue; }
					TSharedPtr<FJsonObject> H = MakeShared<FJsonObject>();
					H->SetStringField(TEXT("cls"), Item->GetClass()->GetName());
					// The retail localized text the info window shows (loaded at
					// runtime from the purchaser's .int, never compiled in).
					H->SetStringField(TEXT("title"), Item->Title);
					H->SetNumberField(TEXT("desc_len"), Item->Description.Len());
					H->SetNumberField(TEXT("quote_len"), Item->Quote.Len());
					if (const AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Item))
					{
						H->SetNumberField(TEXT("charges"), Angreal->CurCharges);
						// AngrealInventory.bTainted (replicated owner-only, like
						// CurCharges): the angreal matrix's Taint observable.
						H->SetBoolField(TEXT("tainted"), Angreal->bTainted);
					}
					Held.Add(MakeShared<FJsonValueObject>(H));
				}
				J->SetArrayField(TEXT("inv"), Held);
			}
			// The hand order behind the angreal hands HUD and the 1-0 hand keys
			// (DT_WoTAngrealHands in dev, WOTPlayer.AngrealHandOrder in guarded
			// builds). Always reported for a local pawn - zero when the hand set
			// is absent - so the sweep can never mistake a missing set for a pass.
			if (C->IsLocallyControlled())
			{
				// hand_plan_classes: planned cells (the shipped order has 42);
				// hand_classes: cells the pawn currently holds (GetClassName is
				// bHaveThis-gated exactly like retail HandInfo.GetClassName).
				int32 HandClasses = 0;
				int32 HandPlanClasses = 0;
				for (int32 HandIndex = 0; IsValid(C->AngrealHandSet)
					&& HandIndex < UWoTHandSet::NumHands; ++HandIndex)
				{
					const UWoTHandInfo* HandInfo = C->AngrealHandSet->GetHand(HandIndex);
					for (int32 HandSlot = 0;
						HandInfo != nullptr && HandSlot < UWoTHandInfo::NumSlots; ++HandSlot)
					{
						HandClasses += HandInfo->GetClassName(HandSlot).IsNone() ? 0 : 1;
						HandPlanClasses += HandInfo->ClassName[HandSlot].IsNone() ? 0 : 1;
					}
				}
				J->SetNumberField(TEXT("hand_classes"), HandClasses);
				J->SetNumberField(TEXT("hand_plan_classes"), HandPlanClasses);
			}
			// Battle per-player flags (COND_OwnerOnly): the server sees every
			// controller's truth; a client sees only its OWN — which is exactly
			// the surface battle_slice asserts (own-channel freeze/overview).
			if (const AWoTPlayerController* WoTPC =
					Cast<AWoTPlayerController>(C->GetController()))
			{
				J->SetBoolField(TEXT("teleportdisabled"), WoTPC->bTeleportingDisabled);
				J->SetBoolField(TEXT("overview"), WoTPC->bShowOverview);
			}
			// NPC brain (server only): the FSM state, orders and goals behind what a
			// troop is doing, plus the nearest alarm's navigability as the captain's
			// GetClosestAlarm would judge it, probed in the unused ExternalDirective slot.
			if (AWoTNPCController* NPCCtl = Cast<AWoTNPCController>(C->GetController()))
			{
				if (UWoTBrainComponent* Brain = NPCCtl->GetBrain())
				{
					TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
					B->SetNumberField(TEXT("state"), static_cast<int32>(NPCCtl->GetBrainState()));
					B->SetNumberField(TEXT("orders"), static_cast<int32>(NPCCtl->GetWorkingOrders()));
					B->SetNumberField(TEXT("goal"), static_cast<int32>(Brain->CurrentGoalIndex));
					B->SetStringField(TEXT("intermediate"), GetNameSafe(
						Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalActor.Get()));
					B->SetStringField(TEXT("threat"), GetNameSafe(
						Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get()));
					AWoTAlarm* NearestAlarm = nullptr;
					double NearestDistSq = FMath::Square(1600.0);
					for (TActorIterator<AWoTAlarm> AlarmIt(World); AlarmIt; ++AlarmIt)
					{
						const double DistSq = FVector::DistSquared(
							AlarmIt->GetActorLocation(), C->GetActorLocation());
						if (DistSq < NearestDistSq)
						{
							NearestDistSq = DistSq;
							NearestAlarm = *AlarmIt;
						}
					}
					if (NearestAlarm && !Brain->IsGoalValid(EWoTGoalIndex::ExternalDirective))
					{
						Brain->InitGoalWithActor(EWoTGoalIndex::ExternalDirective, NearestAlarm);
						B->SetStringField(TEXT("alarm"), NearestAlarm->GetName());
						B->SetBoolField(TEXT("alarm_visible"),
							Brain->IsGoalVisible(EWoTGoalIndex::ExternalDirective));
						B->SetBoolField(TEXT("alarm_reachable"),
							Brain->IsGoalReachable(EWoTGoalIndex::ExternalDirective));
						B->SetBoolField(TEXT("alarm_pathable"),
							Brain->IsGoalPathable(EWoTGoalIndex::ExternalDirective));
						Brain->InvalidateGoal(EWoTGoalIndex::ExternalDirective);
						// Why: the LOS Body ray (LineOfSightTo's single non-pawn ray), the
						// navmesh projection of the alarm, the navmesh raycast toward it and
						// the routed path query, as the brain's queries issue them.
						const FVector Eye = C->GetActorLocation() + FVector(0, 0, C->BaseEyeHeight);
						float AlarmRadius = 0.0f, AlarmHalf = 0.0f;
						NearestAlarm->GetSimpleCollisionCylinder(AlarmRadius, AlarmHalf);
						const FVector AlarmBody = NearestAlarm->GetActorLocation()
							+ FVector(0, 0, 0.8f * AlarmHalf);
						FHitResult LosHit;
						const bool bLosBlocked = WoTVisBlockingTrace(World, Eye, AlarmBody, C, LosHit);
						B->SetBoolField(TEXT("los_blocked"), bLosBlocked);
						if (bLosBlocked)
						{
							B->SetNumberField(TEXT("los_time"), LosHit.Time);
							B->SetStringField(TEXT("los_comp"), GetNameSafe(LosHit.GetComponent()));
							B->SetStringField(TEXT("los_at"), LosHit.ImpactPoint.ToString());
						}
						B->SetStringField(TEXT("alarm_at"), NearestAlarm->GetActorLocation().ToString());
						B->SetNumberField(TEXT("alarm_radius"), AlarmRadius);
						B->SetNumberField(TEXT("alarm_half"), AlarmHalf);
						if (UNavigationSystemV1* NavSys =
							FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
						{
							const ANavigationData* NavData = NavSys->GetDefaultNavDataInstance();
							B->SetBoolField(TEXT("navdata"), NavData != nullptr);
							FNavLocation Projected;
							B->SetBoolField(TEXT("self_on_nav"), NavSys->ProjectPointToNavigation(
								C->GetActorLocation(), Projected));
							const bool bAlarmOnNav = NavSys->ProjectPointToNavigation(
								NearestAlarm->GetActorLocation(), Projected);
							B->SetBoolField(TEXT("alarm_on_nav"), bAlarmOnNav);
							if (bAlarmOnNav)
							{
								B->SetStringField(TEXT("alarm_nav_at"), Projected.Location.ToString());
							}
							FVector RayHit;
							const bool bRayBlocked = NavSys->NavigationRaycast(World,
								C->GetActorLocation(), NearestAlarm->GetActorLocation(), RayHit,
								nullptr, NPCCtl);
							B->SetBoolField(TEXT("navray_blocked"), bRayBlocked);
							B->SetStringField(TEXT("navray_hit"), RayHit.ToString());
							if (NavData)
							{
								FPathFindingQuery Query(C, *NavData, C->GetActorLocation(),
									NearestAlarm->GetActorLocation());
								Query.SetAllowPartialPaths(true);
								const FPathFindingResult Result = NavSys->FindPathSync(Query);
								B->SetNumberField(TEXT("path_result"), static_cast<int32>(Result.Result));
								B->SetBoolField(TEXT("path_partial"),
									Result.Path.IsValid() && Result.Path->IsPartial());
								B->SetNumberField(TEXT("path_points"),
									Result.Path.IsValid() ? Result.Path->GetPathPoints().Num() : 0);
							}
						}
					}
					J->SetObjectField(TEXT("brain"), B);
				}
			}
		}
		Pawns.Add(MakeShared<FJsonValueObject>(J));
	}

	for (TActorIterator<AWoTProjectile> It(World); It; ++It)
	{
		if (!IsValid(*It)) { continue; }
		TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("name"), It->GetName());
		J->SetStringField(TEXT("cls"), It->GetClass()->GetName());
		SetVec(J, It->GetActorLocation(), It->GetVelocity());
		// Seeker navigation probe: what the retail steering (SeekingProjectile.uc:
		// 505-526) would decide from here. The path oracle uses its OWN iterator,
		// so the seeker's live path state is never touched.
		if (const AWoTSeekingProjectile* Seeker = Cast<AWoTSeekingProjectile>(*It))
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetBoolField(TEXT("seeking"), Seeker->bSeeking);
			S->SetStringField(TEXT("dest"), GetNameSafe(Seeker->Destination));
			S->SetStringField(TEXT("navpoint"), GetNameSafe(Seeker->NavPointNode));
			if (Seeker->bSeeking)
			{
				const FVector Loc = IsValid(Seeker->Destination)
					? Seeker->Destination->GetActorLocation() : Seeker->DestLoc;
				S->SetBoolField(TEXT("direct"), Seeker->CanDirectlyReachDestination(Loc));
				FWoTPathNodeIterator Oracle;
				const bool bPath = Oracle.BuildPath(World, Seeker->GetActorLocation(), Loc,
					Seeker->bDesperateSeeker);
				S->SetBoolField(TEXT("path"), bPath);
				S->SetNumberField(TEXT("path_nodes"), Oracle.NodeCount);
				S->SetStringField(TEXT("path_first"), GetNameSafe(bPath ? Oracle.GetFirst() : nullptr));
				S->SetStringField(TEXT("path_last"), GetNameSafe(bPath ? Oracle.GetLast() : nullptr));
			}
			J->SetObjectField(TEXT("seek"), S);
		}
		Projs.Add(MakeShared<FJsonValueObject>(J));
	}

	// The projected path network the seekers navigate (WoTWorldProjector).
	{
		int32 NodeCount = 0;
		int32 EdgeCount = 0;
		int32 Isolated = 0;
		for (TActorIterator<AWoTPathNode> NodeIt(World); NodeIt; ++NodeIt)
		{
			if (!IsValid(*NodeIt)) { continue; }
			++NodeCount;
			EdgeCount += NodeIt->OutEdges.Num();
			Isolated += NodeIt->OutEdges.IsEmpty() && NodeIt->InEdges.IsEmpty() ? 1 : 0;
		}
		TSharedPtr<FJsonObject> Net = MakeShared<FJsonObject>();
		Net->SetNumberField(TEXT("nodes"), NodeCount);
		Net->SetNumberField(TEXT("out_edges"), EdgeCount);
		Net->SetNumberField(TEXT("isolated"), Isolated);
		Root->SetObjectField(TEXT("pathnet"), Net);
	}

	for (TActorIterator<AWoTInventory> It(World); It; ++It)
	{
		if (!IsValid(*It)) { continue; }
		TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("name"), It->GetName());
		J->SetStringField(TEXT("cls"), It->GetClass()->GetName());
		J->SetStringField(TEXT("state"), ItemStateName(It->GetItemState()));
		J->SetBoolField(TEXT("hidden"), It->IsHidden());
		// Position lets a scenario WALK to a world pickup (the real touch-acquire
		// path) instead of only observing state flips.
		SetVec(J, It->GetActorLocation(), It->GetVelocity());
		Pickups.Add(MakeShared<FJsonValueObject>(J));
	}

	// Angreal effect actors (the angreal matrix): every live actor that a pawn
	// or an item instigated/owns and that is not itself a pawn, controller,
	// inventory item, projectile or info - beams, bursts, walls, funnels, light
	// globes, spawned FX. Replicated ones show on a client too, so a missing
	// client entry is a replication finding, not a harness gap.
	TArray<TSharedPtr<FJsonValue>> Effects;
	for (TActorIterator<AActor> It(World); It && Effects.Num() < 96; ++It)
	{
		const AActor* Effect = *It;
		if (!IsValid(Effect) || Effect->IsA<APawn>() || Effect->IsA<AController>()
			|| Effect->IsA<AWoTInventory>() || Effect->IsA<AWoTProjectile>()
			|| Effect->IsA<AInfo>() || Effect->IsA<AHUD>()
			|| Effect->IsA<APlayerCameraManager>())
		{
			continue;
		}
		const APawn* EffectInstigator = Effect->GetInstigator();
		const AActor* EffectOwner = Effect->GetOwner();
		if (EffectInstigator == nullptr
			&& !(EffectOwner && (EffectOwner->IsA<APawn>() || EffectOwner->IsA<AWoTInventory>())))
		{
			continue;
		}
		TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("name"), Effect->GetName());
		J->SetStringField(TEXT("cls"), Effect->GetClass()->GetName());
		J->SetStringField(TEXT("instigator"), GetNameSafe(EffectInstigator));
		J->SetStringField(TEXT("owner"), GetNameSafe(EffectOwner));
		J->SetBoolField(TEXT("hidden"), Effect->IsHidden());
		J->SetBoolField(TEXT("replicates"), Effect->GetIsReplicated());
		SetVec(J, Effect->GetActorLocation(), Effect->GetVelocity());
		Effects.Add(MakeShared<FJsonValueObject>(J));
	}

	Root->SetArrayField(TEXT("pawns"), Pawns);
	Root->SetArrayField(TEXT("projectiles"), Projs);
	Root->SetArrayField(TEXT("pickups"), Pickups);
	Root->SetArrayField(TEXT("effects"), Effects);

	if (const AWoTGameState* GS = World->GetGameState<AWoTGameState>())
	{
		TSharedPtr<FJsonObject> G = MakeShared<FJsonObject>();
		G->SetNumberField(TEXT("remaining"), GS->RemainingTime);
		G->SetNumberField(TEXT("elapsed"), GS->ElapsedTime);
		Root->SetObjectField(TEXT("gamestate"), G);
	}

	// The LOCAL player's match-end signal (owner-only AWoTPlayerController::bMatchEnded). A
	// dedicated server has no local PC → false; a client flips true once the authority ends the
	// match and the flag replicates to its owner. The game_flow scenario polls this.
	bool bLocalMatchEnded = false;
	if (const AWoTPlayerController* WPC = Cast<AWoTPlayerController>(World->GetFirstPlayerController()))
	{
		bLocalMatchEnded = WPC->IsMatchEnded();
	}
	Root->SetBoolField(TEXT("matchended"), bLocalMatchEnded);

	// The LOCAL player's scoreboard/overview visibility. Always false on a dedicated
	// server (no local HUD); the end-to-end scenario's "see scores" step.
	bool bScoreboardVisible = false;
	if (const APlayerController* FirstPC = World->GetFirstPlayerController())
	{
		if (const AWoTHUD* HUD = Cast<AWoTHUD>(FirstPC->GetHUD()))
		{
			bScoreboardVisible = HUD->ShouldDrawArenaScoreboard() ||
				HUD->ShouldDrawCitadelTeamOverview();
		}
	}
	Root->SetBoolField(TEXT("scoreboard"), bScoreboardVisible);
	// The scoreboard/overview header's map line exactly as the HUD gathers it
	// (the retail Level.Title; empty without a local HUD).
	FString ScoreboardMap;
	if (const APlayerController* FirstPC = World->GetFirstPlayerController())
	{
		if (const AWoTHUD* HUD = Cast<AWoTHUD>(FirstPC->GetHUD()))
		{
			ScoreboardMap = HUD->GatherArenaScoreboard().MapName;
			Root->SetStringField(TEXT("overview_map"),
				HUD->GatherCitadelTeamOverview().MapName);
		}
	}
	Root->SetStringField(TEXT("scoreboard_map"), ScoreboardMap);

	// Battle (giMPBattle) observables — the Battle end-to-end scenario's drive/assert
	// surface. BattleInfo state is replicated, so client channels see the phase too;
	// altar/budget positions let a scenario stage pawns into edit zones deterministically
	// (budgets exist server-side only — the array is empty on client channels).
	TActorIterator<AWoTBattleInfo> BattleIt(World);
	if (const AWoTBattleInfo* Battle = BattleIt ? *BattleIt : nullptr)
	{
		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetNumberField(TEXT("numteams"), Battle->NumTeams);
		B->SetBoolField(TEXT("teleportersdisabled"), Battle->bTeleportersDisabled);
		TArray<TSharedPtr<FJsonValue>> Editing, Ready, SealCounts;
		for (int32 i = 0; i < 4; ++i)
		{
			Editing.Add(MakeShared<FJsonValueNumber>(Battle->bEditing[i]));
			Ready.Add(MakeShared<FJsonValueNumber>(Battle->bEditingComplete[i]));
			SealCounts.Add(MakeShared<FJsonValueNumber>(Battle->NumSeals[i]));
		}
		B->SetArrayField(TEXT("editing"), Editing);
		B->SetArrayField(TEXT("ready"), Ready);
		B->SetArrayField(TEXT("numseals"), SealCounts);

		TArray<TSharedPtr<FJsonValue>> Altars;
		for (TActorIterator<AWoTSealAltar> It(World); It; ++It)
		{
			if (!IsValid(*It)) { continue; }
			TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetNumberField(TEXT("team"), It->Team);
			J->SetNumberField(TEXT("goal"), It->SealGoal);
			J->SetBoolField(TEXT("ended"), It->bEnded);
			// Touch-path diagnostics (authority-local altars): the debounce
			// state, the banked seal presence, the touch capsule size and the
			// pawns the engine reports inside it.
			J->SetBoolField(TEXT("inactive"), It->bInactive);
			J->SetBoolField(TEXT("banked"), It->FindSeal() != nullptr);
			if (const UCapsuleComponent* Touch = It->Touch)
			{
				J->SetNumberField(TEXT("touch_radius"), Touch->GetUnscaledCapsuleRadius());
				J->SetNumberField(TEXT("touch_half_height"), Touch->GetUnscaledCapsuleHalfHeight());
				J->SetBoolField(TEXT("touch_collision"),
					Touch->GetCollisionEnabled() != ECollisionEnabled::NoCollision);
				J->SetBoolField(TEXT("touch_physics_state"), Touch->IsPhysicsStateCreated());
				J->SetBoolField(TEXT("touch_registered"), Touch->IsRegistered());
				J->SetBoolField(TEXT("begunplay"), It->HasActorBegunPlay());
				J->SetBoolField(TEXT("initialized"), It->IsActorInitialized());
				TArray<AActor*> Inside;
				Touch->GetOverlappingActors(Inside, APawn::StaticClass());
				TArray<FString> InsideNames;
				for (const AActor* A : Inside) { if (A) { InsideNames.Add(A->GetName()); } }
				J->SetArrayField(TEXT("touching"), ToStringArray(InsideNames));
			}
			const FVector L = It->GetActorLocation();
			J->SetNumberField(TEXT("x"), R1(L.X));
			J->SetNumberField(TEXT("y"), R1(L.Y));
			J->SetNumberField(TEXT("z"), R1(L.Z));
			Altars.Add(MakeShared<FJsonValueObject>(J));
		}
		B->SetArrayField(TEXT("altars"), Altars);

		TArray<TSharedPtr<FJsonValue>> Budgets;
		for (TActorIterator<AWoTBudgetInfo> It(World); It; ++It)
		{
			if (!IsValid(*It)) { continue; }
			TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetNumberField(TEXT("team"), It->Team);
			J->SetNumberField(TEXT("remaining"), It->SealBudgetRemaining());
			Budgets.Add(MakeShared<FJsonValueObject>(J));
		}
		B->SetArrayField(TEXT("budgets"), Budgets);
		Root->SetObjectField(TEXT("battle"), B);
	}

	TArray<TSharedPtr<FJsonValue>> Players;
	if (const AGameStateBase* GSB = World->GetGameState())
	{
		for (const APlayerState* PS : GSB->PlayerArray)
		{
			const AWoTPlayerState* WPS = Cast<AWoTPlayerState>(PS);
			if (!WPS) { continue; }
			TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("name"), WPS->GetPlayerName());
			J->SetNumberField(TEXT("kills"), WPS->Kills);
			J->SetNumberField(TEXT("deaths"), WPS->Deaths);
			J->SetNumberField(TEXT("suicides"), WPS->Suicides);
			J->SetNumberField(TEXT("team"), WPS->Team);
			J->SetNumberField(TEXT("ping"), WPS->GetPingInMilliseconds());
			Players.Add(MakeShared<FJsonValueObject>(J));
		}
	}
	Root->SetArrayField(TEXT("players"), Players);
	return Root;
}

FString WoTNetTest::BuildSnapshotJson(UWorld* World)
{
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(BuildSnapshot(World).ToSharedRef(), Writer);
	return Out;
}

#endif // WOT_WITH_BOOTSTRAP_TEST_HARNESSES
