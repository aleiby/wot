#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "WoTAngrealGuide.generated.h"

// The angreal field-guide registry, generated from
// Tools/import/data/angreal_guide_{facts,hints}.csv by
// import_angreal_guide.py. Facts only: Title, Description and Quote are the
// purchaser's retail text, resolved at runtime through the onboarding
// retail-text seam and never stored here (CONTRIBUTING: nothing from the
// original is ever pasted; retail text is not compiled in).

USTRUCT()
struct WOTONBOARDING_API FWoTAngrealGuideRow
{
	GENERATED_BODY()

	/** Shipped script class name — the row key, and the retail localization
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

	/** Rows THIS row counters (a shield lists the offenses it stops).
	    CounteredBy inverts the relation. */
	UPROPERTY(EditAnywhere, Category = "WoT|Guide")
	TArray<FName> Counters;

	/** Our own counter-hint sentence template; {title} and {counter} resolve
	    at runtime (the {counter} selection rule is Task 5's). Empty for rows
	    without a hint. */
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

	/** The row whose resolved title is the longest one contained in Text
	    (case-insensitive) — death-recap resolution. ResolveTitle supplies
	    each row's runtime display title, normally through the retail-text
	    seam; a row resolving empty never matches (titles are runtime data and
	    may be absent). Ties keep the earliest row. Null when nothing matches. */
	const FWoTAngrealGuideRow* FindByTitleIn(const FString& Text,
		TFunctionRef<FString(const FWoTAngrealGuideRow&)> ResolveTitle) const;

	/** Every row whose Counters names ClassName, in Rows order. */
	TArray<const FWoTAngrealGuideRow*> CounteredBy(FName ClassName) const;

	/** The imported asset, or null when the content tree does not carry it
	    (a tree whose import pipeline has not run). Quiet in that case: the
	    aids simply have no guide. */
	static UWoTAngrealGuide* Load();

#if WITH_EDITOR
	/** Replace Rows from the importer's JSON payload — {"rows": [{...}]}
	    with keys class_name, cpp_class, elements, rarity, roles,
	    max_initial_charges, max_charges, counters, hint_text; empty strings
	    fold to NAME_None for the name fields. Returns the empty string on
	    success and the problem otherwise, leaving Rows untouched on failure.
	    Reflection-callable (UFUNCTION) on purpose: the editor's python glue
	    generates no wrapper for a plugin module loaded after it, so the
	    importer reaches this through call_method instead of a wrapper type. */
	UFUNCTION() FString ImportRowsFromJson(const FString& PayloadJson);

	/** Rows serialized back in exactly the payload shape, for the importer's
	    change detection and its independent post-save verification. */
	UFUNCTION() FString ExportRowsAsJson() const;
#endif

	/** The object path the importer publishes the asset at. */
	static const TCHAR* AssetPath();
};
