#include "WoTOnboardingHints.h"

#include "WoTAngrealGuide.h"

namespace
{
/** The pinned preference order, lowest first. */
enum class EWoTOnboardingCounterTier : uint8
{
	Dedicated,
	ReflectFamily,
	ElementShield,
};

EWoTOnboardingCounterTier WoTOnboardingClassifyCounter(
	const UWoTAngrealGuide& Guide, const FWoTAngrealGuideRow& Counterer)
{
	if (Counterer.Counters.Num() == 1)
	{
		return EWoTOnboardingCounterTier::Dedicated;
	}
	// An element shield's derived edges all carry the shield's element, so the
	// countered rows share at least one; the Reflect family's edges span the
	// projectile roster and share none. Rows the asset does not carry (an edge
	// into a pruned roster) contribute nothing either way.
	TSet<FName> SharedElements;
	bool bAnyCounteredRow = false;
	for (const FName& Countered : Counterer.Counters)
	{
		const FWoTAngrealGuideRow* CounteredRow =
			Guide.FindByClassName(Countered);
		if (!CounteredRow)
		{
			continue;
		}
		TSet<FName> Elements(CounteredRow->Elements);
		if (!bAnyCounteredRow)
		{
			SharedElements = MoveTemp(Elements);
			bAnyCounteredRow = true;
		}
		else
		{
			SharedElements = SharedElements.Intersect(Elements);
		}
		if (SharedElements.IsEmpty())
		{
			break;
		}
	}
	return bAnyCounteredRow && !SharedElements.IsEmpty()
		? EWoTOnboardingCounterTier::ElementShield
		: EWoTOnboardingCounterTier::ReflectFamily;
}
} // namespace

FString WoTOnboardingHints::SpacedClassName(FName ClassName)
{
	if (ClassName.IsNone())
	{
		return FString();
	}
	FString Name = ClassName.ToString();
	// Registry affixes, not display text: the angreal classes carry an
	// AngrealInv prefix, the traps an Inventory suffix.
	Name.RemoveFromStart(TEXT("AngrealInv"));
	Name.RemoveFromEnd(TEXT("Inventory"));
	FString Spaced;
	Spaced.Reserve(Name.Len() + 4);
	for (int32 Index = 0; Index < Name.Len(); ++Index)
	{
		const TCHAR Char = Name[Index];
		if (Index > 0 && FChar::IsUpper(Char) && FChar::IsLower(Name[Index - 1]))
		{
			Spaced.AppendChar(TEXT(' '));
		}
		Spaced.AppendChar(Char);
	}
	return Spaced;
}

const FWoTAngrealGuideRow* WoTOnboardingHints::SelectCounter(
	const UWoTAngrealGuide& Guide, const FWoTAngrealGuideRow& Row)
{
	const TArray<const FWoTAngrealGuideRow*> Counterers =
		Guide.CounteredBy(Row.ClassName);
	const FWoTAngrealGuideRow* Best = nullptr;
	EWoTOnboardingCounterTier BestTier = EWoTOnboardingCounterTier::ElementShield;
	for (const FWoTAngrealGuideRow* Counterer : Counterers)
	{
		const EWoTOnboardingCounterTier Tier =
			WoTOnboardingClassifyCounter(Guide, *Counterer);
		// Strictly-better only: within a tier the first counterer in Rows
		// order stays (the documented tie-break).
		if (!Best || Tier < BestTier)
		{
			Best = Counterer;
			BestTier = Tier;
		}
		if (BestTier == EWoTOnboardingCounterTier::Dedicated)
		{
			break;
		}
	}
	return Best;
}

FString WoTOnboardingHints::ResolveHintLine(const UWoTAngrealGuide& Guide,
	const FWoTAngrealGuideRow& Row,
	TFunctionRef<FString(const FWoTAngrealGuideRow&)> ResolveTitle)
{
	if (Row.HintText.IsEmpty())
	{
		return FString();
	}
	FString Line = Row.HintText;
	if (Line.Contains(TEXT("{counter}")))
	{
		const FWoTAngrealGuideRow* Counter = SelectCounter(Guide, Row);
		if (!Counter)
		{
			// The template claims a counter the data does not back: no line.
			return FString();
		}
		FString CounterTitle = ResolveTitle(*Counter);
		if (CounterTitle.IsEmpty())
		{
			CounterTitle = SpacedClassName(Counter->ClassName);
		}
		Line = Line.Replace(TEXT("{counter}"), *CounterTitle);
	}
	FString Title = ResolveTitle(Row);
	if (Title.IsEmpty())
	{
		Title = SpacedClassName(Row.ClassName);
	}
	return Line.Replace(TEXT("{title}"), *Title);
}
