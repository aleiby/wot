#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "WoTMaterialImportLibrary.generated.h"

class UMaterial;

// Python cannot reach UMaterial's expression graph (the Expressions property
// is protected / editor-only). Keep that editor seam small and typed here so
// the import pipeline can update a generated master's Custom-node HLSL in
// place (the shared masters are created once by import_map.py and then
// referenced by every MIC; deleting + recreating them would orphan those
// parent references).
UCLASS()
class UWoTMaterialImportLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	// Replaces the Code of the MaterialExpressionCustom whose Description
	// matches, if the current code differs; the caller recompiles and saves.
	// Returns 0 = node not found, 1 = found and already current, 2 = patched.
	UFUNCTION(BlueprintCallable, Category = "WoT|Editor|Materials")
	static int32 SetCustomNodeCode(
		UMaterial* Material,
		const FString& Description,
		const FString& Code);

	// Splices a VertexInterpolator between the MaterialExpressionCustom whose
	// Description matches and every expression/material-property input that
	// consumes it, so the node evaluates per-vertex and hardware-interpolates
	// (UE1 Gouraud). Idempotent: returns 1 if the custom node's only consumer
	// is already a VertexInterpolator, 2 when spliced, 0 if not found.
	UFUNCTION(BlueprintCallable, Category = "WoT|Editor|Materials")
	static int32 WrapCustomNodeInVertexInterpolator(
		UMaterial* Material,
		const FString& Description);

	// Inverse of WrapCustomNodeInVertexInterpolator: rewires every consumer of
	// the interpolator that feeds from the matching Custom node back to the
	// Custom node and removes the interpolator. Returns 2 when unwrapped,
	// 1 when there was nothing to unwrap, 0 if the custom node is missing.
	UFUNCTION(BlueprintCallable, Category = "WoT|Editor|Materials")
	static int32 UnwrapCustomNodeVertexInterpolator(
		UMaterial* Material,
		const FString& Description);
#endif
};
