#include "WoTAbsorbReflector.h"

#include "WoTAngrealInventory.h"
#include "WoTInventoryComponent.h"
#include "WoTLeech.h"
#include "../Projectiles/WoTAngrealProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTMeshFX.h"
#include "../WoTSound.h"
#include "../WoTUE1Names.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"

UWoTAbsorbReflector::UWoTAbsorbReflector()
{
	Priority = 170;   // AbsorbReflector.uc:232 (above Reflect 160 — absorb wins)
	bRemoveExisting = true;
	bDeleterious = false;   // it's good for you
	// AbsorbReflector.uc:224-231, exact shipped steal/affectability lists.
	NonStealableTypes[0] = TEXT("AngrealInvAirBurst");
	UnAffectedProjs[0] = TEXT("MashadarGuide");
	UnAffectedProjs[1] = TEXT("MachinShin");
	UnAffectedProjs[2] = TEXT("AMAPearl");
	UnAffectedProjs[3] = TEXT("AngrealDistantEyeProjectile");
	UnAffectedProjs[4] = TEXT("AngrealWallOfAirProjectile");
	UnAffectedProjs[5] = TEXT("EarthTremorRock");
	UnAffectedProjs[6] = TEXT("LavaRock");
	InvokeSound = FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/AbsorbABS.AbsorbABS"));
}

bool UWoTAbsorbReflector::BuildTouchReflectorDescription(
	FWoTTouchReflectorDescription& Out) const
{
	Out = FWoTTouchReflectorDescription{};
	Out.Kind = EWoTTouchReflectorKind::Absorb;
	Out.Priority = Priority;
	Out.InstallSeq = InstallSeq;
	for (const FName Type : UnAffectedProjs)
		if (!Type.IsNone()) Out.UnaffectedProjectileTypes.Add(Type);
	return true;
}

void UWoTAbsorbReflector::Install(APawn* NewHost)
{
	Super::Install(NewHost);
	if (GetOwnerRole() != ROLE_Authority || GetOwner() != NewHost
		|| !IsRegistered() || !IsValid(NewHost))
	{
		return;
	}

	// AbsorbReflector.uc:71-133: leeches are searched before reflectors and the
	// first eligible effect consumes this one-shot, regardless of steal outcome.
	// Both retail lists walk NEWEST-first (prepend linked lists), and only
	// attached/installed members exist in them — mirror both.
	TArray<UWoTLeech*> Leeches;
	NewHost->GetComponents<UWoTLeech>(Leeches);
	Leeches.RemoveAll([](const UWoTLeech* Leech)
	{
		return !IsValid(Leech) || !Leech->IsRegistered();
	});
	Leeches.Sort([](const UWoTLeech& A, const UWoTLeech& B)
	{
		return A.AttachSeq > B.AttachSeq;
	});
	for (UWoTLeech* Leech : Leeches)
	{
		if (Leech->bRemovable && Leech->bDeleterious && !Leech->bFromProjectile)
		{
			StealArtifact(Leech->SourceAngreal);
			Leech->Unattach();
			Uninstall();
			return;
		}
	}

	TArray<UWoTReflector*> Reflectors;
	if (const AWoTCharacter* HostCharacter = Cast<AWoTCharacter>(NewHost))
	{
		HostCharacter->GetOrderedReflectors(Reflectors);
	}
	for (UWoTReflector* Reflector : Reflectors)
	{
		if (Reflector != this && IsValid(Reflector) && Reflector->bRemovable
			&& Reflector->bDeleterious && !Reflector->bFromProjectile)
		{
			StealArtifact(Reflector->SourceAngreal);
			Reflector->Uninstall();
			Uninstall();
			return;
		}
	}
}

bool UWoTAbsorbReflector::ConsumesEffect(const FWoTDamageEffectSpec& Effect)
{
	// AbsorbReflector.uc:50-68: projectile/splash damage belongs exclusively to
	// NotifyHitByAngrealProjectile; self-sourced effects pass down the chain.
	if (!Effect.bDeleterious || Effect.SourceProjClass
		|| Effect.SourceAngreal == SourceAngreal)
	{
		return false;
	}
	StealArtifact(Effect.SourceAngreal);
	Uninstall();
	return true;
}

bool UWoTAbsorbReflector::IsAffectable(const AActor* Other) const
{
	if (!IsValid(Other))
	{
		return false;
	}
	for (const FName& TypeName : UnAffectedProjs)
	{
		if (WoTUE1Names::ActorIsA(Other, TypeName))
		{
			return false;
		}
	}
	return true;
}

bool UWoTAbsorbReflector::OnHitByProjectile(AWoTAngrealProjectile* Proj)
{
	if (!IsAffectable(Proj))
	{
		return false;
	}

	// AbsorbReflector.uc:32-42: ANY affectable weave is eaten and spends the
	// one-shot — StealArtifact silently no-ops for a non-stealable artifact
	// (the host cancels the hit but gains nothing).
	StealArtifact(Proj->SourceAngreal);

	// "Only useful once."
	Uninstall();
	return true;
}

bool UWoTAbsorbReflector::IsStealable(const AWoTAngrealInventory* A) const
{
	if (!IsValid(A) || A == SourceAngreal)
	{
		return false;
	}
	for (const FName& TypeName : NonStealableTypes)
	{
		if (WoTUE1Names::ActorIsA(A, TypeName))
		{
			return false;
		}
	}
	return true;
}

bool UWoTAbsorbReflector::StealArtifact(AWoTAngrealInventory* A)
{
	APawn* Host = Cast<APawn>(GetOwner());
	if (!IsValid(Host) || !IsStealable(A))
	{
		return false;
	}
	UWorld* World = GetWorld();
	if (!IsValid(World))
	{
		return false;
	}

	// AbsorbReflector.uc:145-155: WOTPawns (NPCs) keep their loadout; the host
	// receives a fresh class-default actor. WOTPlayers lose the original actor.
	if (const AWoTCharacter* OwnerCharacter = Cast<AWoTCharacter>(A->GetOwner());
		OwnerCharacter && OwnerCharacter->IsNPCPawn())
	{
		const FTransform SpawnTransform = A->GetActorTransform();
		AWoTAngrealInventory* Copy =
			World->SpawnActorDeferred<AWoTAngrealInventory>(
				A->GetClass(), SpawnTransform, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!IsValid(Copy))
		{
			return false;
		}
		Copy->bHeldItem = true;
		Copy->FinishSpawning(SpawnTransform);
		A = Copy;
	}
	else if (UWoTInventoryComponent* OwnerInv = A->GetOwnerInventory())
	{
		OwnerInv->DeleteInventory(A);
	}

	// Refund the charge that was spent on us, then hand it to the host — through
	// the game's pickup query (AbsorbReflector.uc:163), so a duplicate the host
	// already holds merges the stolen charges (AngrealInventory.HandlePickupQuery
	// steals them + SetRespawn's the extra) instead of stacking a second item.
	A->AddCharges(1);
	const AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game || Game->PickupQuery(Host, A))
	{
		A->GiveTo(Host);
	}
	DoCoolEffect();
	return true;
}

void UWoTAbsorbReflector::DoCoolEffect() const
{
	// AbsorbReflector.uc:174-185: Spawn(class'AbsorbVisual', Owner) — the
	// translucent trailer mesh one-shot on the absorbing pawn —
	// plus Owner.PlaySound.
	if (AActor* Host = GetOwner())
	{
		if (UWorld* World = Host->GetWorld())
		{
			WoTMeshFX::SpawnBurstMarker(*World, TEXT("AbsorbVisual"),
				Host->GetActorLocation(), FRotator::ZeroRotator, Host);
		}
		WoTSound::PlayOneShot(this, WoTSound::Resolve(InvokeSound),
			Host->GetActorLocation(), 1600.0f);   // Actor.PlaySound zero-radius fallback
	}
}
