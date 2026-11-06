#include "TypedProperties.h"

#include "Containers/StringConv.h"

namespace
{
void AppendU32(TArray<uint8>& Bytes, uint32 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
	Bytes.Add(static_cast<uint8>(Value >> 16));
	Bytes.Add(static_cast<uint8>(Value >> 24));
}

bool AppendString(TArray<uint8>& Bytes, FStringView Value)
{
	const FString Owned(Value);
	FTCHARToUTF8 Encoded(*Owned);
	if (Encoded.Length() < 0 || Encoded.Length() > 1024 * 1024) return false;
	AppendU32(Bytes, static_cast<uint32>(Encoded.Length()));
	Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	return true;
}

bool AppendProperty(TArray<uint8>& Payload, const FWoTTypedProperty& Property,
	bool bIncludeStructMembers, uint32 Depth)
{
	if (Depth > 8 || !AppendString(Payload, Property.Name)) return false;
	Payload.Add(Property.Type);
	AppendU32(Payload, Property.ArrayIndex);
	if (!AppendString(Payload, Property.StructName)
		|| !AppendString(Payload, Property.NameValue)
		|| !AppendString(Payload, Property.ObjectIdentity)
		|| !AppendString(Payload, Property.ObjectClassIdentity)) return false;
	// ObjectProperty and NameProperty values are package-table indices. Their
	// resolved strings above are the canonical value after source tables go.
	const bool bResolvedValue = Property.Type == 5 || Property.Type == 6;
	AppendU32(Payload, bResolvedValue
		? 0u : static_cast<uint32>(Property.Value.Num()));
	if (!bResolvedValue) Payload.Append(Property.Value);
	if (bIncludeStructMembers)
	{
		AppendU32(Payload, static_cast<uint32>(Property.StructMembers.Num()));
		for (const FWoTTypedProperty& Member : Property.StructMembers)
			if (!AppendProperty(Payload, Member, true, Depth + 1)) return false;
	}
	return true;
}
}

bool WoTAppendCanonicalTypedProperties(TArray<uint8>& Payload,
	TConstArrayView<FWoTTypedProperty> Properties, bool bIncludeStructMembers)
{
	AppendU32(Payload, static_cast<uint32>(Properties.Num()));
	for (const FWoTTypedProperty& Property : Properties)
	{
		if (!AppendProperty(Payload, Property, bIncludeStructMembers, 0))
			return false;
	}
	return true;
}
