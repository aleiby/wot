#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

struct FWoTRemainingSource;
struct FWoTTypedProperty;
class FWoTRemainingPackageResolver;

/** Reflection-backed decoder for saved UE1 struct values in class defaults. */
class FWoTStructSchemaResolver
{
public:
	explicit FWoTStructSchemaResolver(FWoTRemainingPackageResolver& InPackages);
	~FWoTStructSchemaResolver();

	FWoTStructSchemaResolver(const FWoTStructSchemaResolver&) = delete;
	FWoTStructSchemaResolver& operator=(const FWoTStructSchemaResolver&) = delete;

	bool DecodeClassDefaultStruct(const FWoTRemainingSource& ValueSource,
		int32 ClassExportReference, FStringView ClassIdentity,
		FWoTTypedProperty& Property, FString& OutError);
	bool MaterializeImplicitZeroClassDefaultStruct(
		const FWoTRemainingSource& ValueSource,
		int32 ClassExportReference, FStringView PropertyName,
		FWoTTypedProperty& OutProperty, FString& OutError);

private:
	class FImpl;
	TUniquePtr<FImpl> Impl;
};
