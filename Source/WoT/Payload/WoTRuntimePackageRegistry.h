// Authenticated semantic package registry decoded at its first lookup.
#pragma once

#include "CoreMinimal.h"
#include "WoTRuntimeRegistry.h"

struct FWoTRuntimePackageExport
{
	FString ClassIdentity;
	uint32 ExportIndex = 0;
	bool bPlaceholder = false;
};

class WOT_API FWoTRuntimePackageRegistry final
{
public:
	FWoTRuntimePackageRegistry() = default;
	FWoTRuntimePackageRegistry(FWoTRuntimePackageRegistry&&) = default;
	FWoTRuntimePackageRegistry& operator=(FWoTRuntimePackageRegistry&&) = default;
	FWoTRuntimePackageRegistry(const FWoTRuntimePackageRegistry&) = delete;
	FWoTRuntimePackageRegistry& operator=(
		const FWoTRuntimePackageRegistry&) = delete;

	FWoTBootstrapStatus LoadFirstUse(
		const FWoTNeutralResourceId& Resource);
	const FWoTRuntimePackageExport* FindExport(FStringView Identity) const;
	bool ContainsName(FStringView Name) const;

private:
	TOptional<FWoTRuntimeVerifiedObject> Object;
	TSet<FString> Names;
	TMap<FString, FWoTRuntimePackageExport> Exports;
};
