#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "WoTVertexAnimComponent.h"
#include "WoTAnimData.generated.h"

// Developer-oracle row for the generated per-mesh #exec MESH SEQUENCE table.
// Guarded retail reads the equivalent cache-v3 animation header on first use.
USTRUCT()
struct FWoTAnimSequenceSetRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "WoT")
	TArray<FWoTAnimSequence> Sequences;
};

// Resolver for the generated table. Tolerates missing content (headless tests on a tree that
// hasn't run the importer): every miss is a quiet nullptr/false, never a check.
struct WoTAnimData
{
	// The sequence set for an imported mesh (by mesh asset name), or null when the table or the
	// row doesn't exist.
	static const TArray<FWoTAnimSequence>* FindSequences(
		FName MeshName, FStringView AnimationLogicalId = FStringView());

	// Fill Component->Sequences from the table row for MeshName. Returns false (leaving the
	// component untouched) on a miss.
	static bool PopulateSequences(UWoTVertexAnimComponent* Component,
		FName MeshName, FStringView AnimationLogicalId = FStringView());

	/** Drop generation-bound parsed rows before the active lease is released. */
	static void ResetRuntimeCache();
};
