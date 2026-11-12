#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "WoTAngrealGuide.generated.h"

// The angreal field-guide registry. Its rows come from
// Tools/import/data/angreal_guide_{facts,hints}.csv, which
// import_angreal_guide.py compiles into Private/Generated/
// WoTAngrealGuideRows.inl: the guarded build cooks no project data assets, so
// the rows ship in code. Facts only: Title, Description and Quote are the
// purchaser's retail text, resolved at runtime through the onboarding
// retail-text seam and never stored here (CONTRIBUTING: nothing from the
// original is ever pasted; retail text is not compiled in).

USTRUCT()
struct WOTONBOARDING_API FWoTAngrealGuideRow
{
	GENERATED_BODY()

	/** Shipped script class name: the row key, and the retail localization
	    section its display text resolves from. */
	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	FName ClassName;

	/** The ported C++ class backing the row (the Task 7 honesty gate
	    resolves it against the live CDO). */
	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	FName CppClass;

	/** Element flags the class resolves (Air/Earth/Fire/Spirit/Water). */
	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	TArray<FName> Elements;

	/** Common/Uncommon/Rare; None for rows with no angreal rarity (traps). */
	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	FName Rarity;

	/** Combat/Defensive/Offensive role flags. */
	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	TArray<FName> Roles;

	/** Charge envelope; 0 when the class carries none (the traps). */
	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	int32 MaxInitialCharges = 0;

	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	int32 MaxCharges = 0;

	/** Charges one use spends: the class's ChargeCost, 1 unless the class
	    overrides it, as in the original. 0 for an angreal that never runs
	    down, which the item-info window shows as unlimited; the compiled
	    rows also carry 0 for the traps, which have no charges. */
	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	int32 ChargeCost = 1;

	/** Rows THIS row counters (a shield lists the offenses it stops).
	    CounteredBy inverts the relation. */
	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	TArray<FName> Counters;

	/** Our own counter-hint sentence template; {title} and
	    {counter:<Class>} resolve at runtime, the latter only while the named
	    row counters this one (WoTOnboardingHints). Empty for rows without a
	    hint. */
	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	FString HintText;
};

UCLASS()
class WOTONBOARDING_API UWoTAngrealGuide : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	TArray<FWoTAngrealGuideRow> Rows;

	/** The row keyed by the shipped script class name; null when absent. */
	const FWoTAngrealGuideRow* FindByClassName(FName ClassName) const;

	/** Every row whose Counters names ClassName, in Rows order. */
	TArray<const FWoTAngrealGuideRow*> CounteredBy(FName ClassName) const;

	/** A new transient guide holding the compiled rows (CompiledRowsJson);
	    the caller keeps it alive. Null only if those rows fail to parse,
	    which is logged: the aids then run without a guide. */
	static UWoTAngrealGuide* Load();

	/** The compiled rows as one {"rows": [...]} payload document. */
	static FString CompiledRowsJson();

	/** Replace Rows from a JSON payload, {"rows": [{...}]} with keys
	    class_name, cpp_class, elements, rarity, roles, max_initial_charges,
	    max_charges, counters, hint_text; empty strings fold to NAME_None for
	    the name fields. Returns the empty string on success and the problem
	    otherwise, leaving Rows untouched on failure. */
	FString ImportRowsFromJson(const FString& PayloadJson);

	/** Rows serialized back in exactly the payload shape. */
	FString ExportRowsAsJson() const;
};
