#pragma once

#include "CoreMinimal.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTProjectileLauncher.h"
#include "UObject/UnrealType.h"

// Bot input lifetime only. Shared item/caster policy remains authoritative.
struct FWoTBotCastHold
{
	// Longest default hold: (60/320 + .05) * 20 = 4.75 seconds, plus 1.25
	// seconds of margin. Invalid or unusually long authored budgets still terminate.
	static constexpr float SafetySeconds = 6.0f;

	void Begin(AWoTAngrealInventory* InItem, float Now)
	{
		Item = InItem;
		StartedAt = Now;
		Budget = TNumericLimits<float>::Max();
		float Duration = 0;
		// Capture BEFORE activation: a synchronous use can exhaust the charge group.
		if (InItem->RoundsPerMinute > 0 && InItem->GetCastDuration(Duration)
			&& FMath::IsFinite(Duration) && Duration >= 0) Budget = Duration;
		Facts = FindFacts(InItem);
		if (Facts && Facts->Prime)
		{
			const float Prime = Facts->Prime->GetPropertyValue_InContainer(InItem);
			if (FMath::IsFinite(Prime) && Prime > 0) Budget = Prime;
		}
	}

	bool IsTracking() const { return !Item.IsExplicitlyNull(); }
	void Clear() { Item.Reset(); Facts = nullptr; }
	bool BudgetExpired(float Now) const { return Now - StartedAt >= Budget; }
	bool SafetyExpired(float Now) const { return Now - StartedAt >= SafetySeconds; }
	bool EffectEnded() const
	{
		const AWoTAngrealInventory* Held = Item.Get();
		if (!Held) return true;
		if (const AWoTProjectileLauncher* Launcher = Cast<AWoTProjectileLauncher>(Held))
			return !Launcher->bCasting;
		if (!Facts) return false; // Immediate and independently owned effects use duration/charge policy.
		if (Facts->Active) return !Facts->Active->GetPropertyValue_InContainer(Held);
		// These two held effects enable their actor tick on activation and disable
		// it on every release, including loss of their effect/anchor. No generic
		// assumption is made about actor ticking for other artifact classes.
		return Facts->bTickTracksEffect && !Held->IsActorTickEnabled();
	}
	AWoTAngrealInventory* GetItem() const { return Item.Get(); }

private:
	struct FItemFacts
	{
		UClass* Class;
		const FBoolProperty* Active;
		const FFloatProperty* Prime;
		bool bTickTracksEffect;
		FItemFacts(const TCHAR* Path, const TCHAR* Flag, const TCHAR* PrimeName = nullptr, bool bTick = false)
			: Class(FindObject<UClass>(nullptr, Path)),
			  Active(Flag ? FindFProperty<FBoolProperty>(Class, Flag) : nullptr),
			  Prime(PrimeName ? FindFProperty<FFloatProperty>(Class, PrimeName) : nullptr),
			  bTickTracksEffect(bTick) {}
	};
	static const FItemFacts* FindFacts(const AWoTAngrealInventory* Held)
	{
		// Read existing reflected instance facts without exporting or changing the
		// game module. IsA includes authored subclasses; prime values are not copied.
		static const FItemFacts Table[] = {
			{TEXT("/Script/WoT.WoTAngrealInvBalefire"), TEXT("bCasting"), TEXT("PrimeTime")},
			{TEXT("/Script/WoT.WoTAngrealInvLightning"), TEXT("bCasting")},
			{TEXT("/Script/WoT.WoTAngrealInvLevitate"), TEXT("bLevitating")},
			{TEXT("/Script/WoT.WoTAngrealInvDisguise"), TEXT("bCasting")},
			{TEXT("/Script/WoT.WoTAngrealInvWhirlwind"), nullptr, nullptr, true},
			{TEXT("/Script/WoT.WoTAngrealInvWallOfAir"), nullptr, nullptr, true}
		};
		for (const FItemFacts& Entry : Table)
			if (Entry.Class && Held->IsA(Entry.Class)) return &Entry;
		return nullptr;
	}
	TWeakObjectPtr<AWoTAngrealInventory> Item;
	const FItemFacts* Facts = nullptr;
	float StartedAt = 0;
	float Budget = TNumericLimits<float>::Max();
};
