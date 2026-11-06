#include "WoTDestroyDirective.h"

#include "AI/WoTNPCCharacter.h"
#include "WoTPayloadSubsystem.h"
#include "World/WoTRuntimeClassDefaults.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"

AWoTDestroyDirective::AWoTDestroyDirective()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
}

void AWoTDestroyDirective::BeginPlay()
{
	Super::BeginPlay();
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
}

bool AWoTDestroyDirective::ConfigureObserverClasses(
	const FWoTRuntimeClassDefaults& Defaults, FString& OutError)
{
	ObserverNativeClasses.Reset();
	ObserverRetailClasses.Reset();
	ObserverRetailDescendantLeaves.Reset();
	for (const FName SelectorName : ObserverClasses)
	{
		if (SelectorName.IsNone()) continue;
		const FString Selector = SelectorName.ToString();
		FString RetailIdentity;
		if (!Defaults.FindClassIdentityByLeaf(Selector, RetailIdentity))
		{
			// The world record normally carries the canonical package identity.
			// Preserve it directly when the defaults graph has no unique leaf.
			RetailIdentity = Selector;
		}
		ObserverRetailClasses.AddUnique(RetailIdentity);
		TArray<FString> Descendants;
		Defaults.GetDerivedClassIdentities(RetailIdentity, Descendants);
		for (FString Descendant : Descendants)
		{
			int32 DescendantDot = INDEX_NONE;
			if (Descendant.FindLastChar(TEXT('.'), DescendantDot))
			{
				Descendant.RightChopInline(DescendantDot + 1);
			}
			ObserverRetailDescendantLeaves.Add(FName(*Descendant));
		}

		FString Leaf = Selector;
		int32 Dot = INDEX_NONE;
		if (Leaf.FindLastChar(TEXT('.'), Dot)) Leaf.RightChopInline(Dot + 1);
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(AActor::StaticClass())) continue;
			const FString NativeName = It->GetName();
			if (NativeName.Equals(Leaf, ESearchCase::IgnoreCase)
				|| NativeName.Equals(TEXT("WoT") + Leaf,
					ESearchCase::IgnoreCase))
			{
				ObserverNativeClasses.AddUnique(*It);
				break;
			}
		}
	}
	OutError.Reset();
	return true;
}

bool AWoTDestroyDirective::MatchesObserverClass(const AActor* Candidate) const
{
	for (const TSubclassOf<AActor> NativeClass : ObserverNativeClasses)
		if (NativeClass && Candidate->IsA(NativeClass)) return true;

	const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(Candidate);
	if (!NPC || NPC->CreatureRowName.IsNone()) return false;
	if (ObserverRetailDescendantLeaves.Contains(NPC->CreatureRowName)) return true;
	FString Error;
	FWoTRuntimeClassDefaults* Defaults =
		UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(Error);
	if (Defaults == nullptr) return false;
	FString CandidateRetailClass;
	if (!Defaults->FindClassIdentityByLeaf(
		NPC->CreatureRowName.ToString(), CandidateRetailClass)) return false;
	for (const FString& Selector : ObserverRetailClasses)
		if (Defaults->ClassIsA(CandidateRetailClass, Selector)) return true;
	return false;
}

void AWoTDestroyDirective::WoTTrigger(AActor*, APawn*)
{
	TArray<TWeakObjectPtr<AActor>> Victims;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Candidate = *It;
		if (Candidate == this) continue;
		bool bMatch = ObserverNames.Contains(Candidate->GetFName());
		const IWoTTriggerable* Triggerable = Cast<IWoTTriggerable>(Candidate);
		for (const FName Tag : ObserverTags)
			bMatch |= !Tag.IsNone() && (Candidate->ActorHasTag(Tag)
				|| (Triggerable && Triggerable->GetUE1Tag() == Tag));
		bMatch |= MatchesObserverClass(Candidate);
		if (bMatch) Victims.Add(Candidate);
	}
	for (const TWeakObjectPtr<AActor>& Victim : Victims)
		if (Victim.IsValid()) Victim->Destroy();
}
