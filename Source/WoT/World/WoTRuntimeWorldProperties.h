#pragma once

#include "CoreMinimal.h"
#include "../Payload/WoTRuntimeWorldResource.h"

enum class EWoTStructMemberPolicy : uint8
{
	WorldResolvedSchema,
	ReflectedGeneric,
};

/** Shared strict readers for the typed values retained in world-v3 records. */
WOT_API bool WoTRuntimeWorldPropertyVector(
	const FWoTRuntimeWorldProperty& Property, FVector& Out);
WOT_API bool WoTRuntimeWorldPropertyRotator(
	const FWoTRuntimeWorldProperty& Property, FRotator& Out);
WOT_API bool WoTRuntimeWorldPropertyFloat(
	const FWoTRuntimeWorldProperty& Property, float& Out);
WOT_API bool WoTRuntimeWorldPropertyByte(
	const FWoTRuntimeWorldProperty& Property, uint8& Out);
WOT_API bool WoTRuntimeWorldPropertyBool(
	const FWoTRuntimeWorldProperty& Property, bool& Out);
WOT_API bool WoTRuntimeWorldPropertyInteger(
	const FWoTRuntimeWorldProperty& Property, int32& Out);
WOT_API bool WoTRuntimeWorldPropertyUE1String(
	const FWoTRuntimeWorldProperty& Property, FString& Out);

WOT_API const FWoTRuntimeWorldProperty* WoTRuntimeWorldFindProperty(
	const FWoTRuntimeWorldActor& Actor, FStringView Name,
	uint32 ArrayIndex = 0);

/**
 * A decoded struct's member, by case-insensitive name and static-array index;
 * null when absent. Members are FWoTRuntimeWorldProperty values themselves, so
 * the typed readers above apply to them unchanged and nested structs recurse.
 */
WOT_API const FWoTRuntimeWorldProperty* WoTRuntimeWorldStructMember(
	const FWoTRuntimeWorldProperty& Struct, FStringView MemberName,
	uint32 ArrayIndex = 0);

/** Decode the canonical typed-property block shared by world-v3 and WOTDEF3. */
WOT_API bool WoTRuntimeWorldDecodeProperties(
	TConstArrayView<uint8> Bytes, int32& InOutOffset,
	TArray<FWoTRuntimeWorldProperty>& OutProperties, FString& OutError,
	const TMap<FString, FString>* SemanticClasses = nullptr,
	bool bDecodeStructMembers = false,
	EWoTStructMemberPolicy StructMemberPolicy =
		EWoTStructMemberPolicy::WorldResolvedSchema);

/** UE1 units are retained 1:1. Rotators use Pitch/Yaw/Roll and 65536/turn. */
WOT_API FTransform WoTRuntimeWorldActorTransform(
	const FWoTRuntimeWorldActor& Actor);
