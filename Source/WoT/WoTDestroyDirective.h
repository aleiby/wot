#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTEvents.h"
#include "WoTDestroyDirective.generated.h"

class FWoTRuntimeClassDefaults;

/** Legend.DestroyDirective: triggering destroys every configured observer. */
UCLASS()
class AWoTDestroyDirective final : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	AWoTDestroyDirective();
	UPROPERTY(EditAnywhere, Category = "WoT") FName TagName;
	UPROPERTY(EditAnywhere, Category = "WoT") TArray<FName> ObserverClasses;
	UPROPERTY(EditAnywhere, Category = "WoT") TArray<FName> ObserverTags;
	UPROPERTY(EditAnywhere, Category = "WoT") TArray<FName> ObserverNames;
	UPROPERTY(Transient) TArray<TSubclassOf<AActor>> ObserverNativeClasses;
	// Canonical retail class identities retained for data-driven native actors
	// (notably AWoTNPCCharacter, whose CreatureRowName carries the retail leaf).
	TArray<FString> ObserverRetailClasses;
	// Retained at Configure time so matching never depends on the active payload
	// singleton still being the graph which projected this directive.
	TSet<FName> ObserverRetailDescendantLeaves;
	bool ConfigureObserverClasses(const FWoTRuntimeClassDefaults& Defaults,
		FString& OutError);
	virtual void BeginPlay() override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual FName GetUE1Tag() const override { return TagName; }

private:
	bool MatchesObserverClass(const AActor* Candidate) const;
};
