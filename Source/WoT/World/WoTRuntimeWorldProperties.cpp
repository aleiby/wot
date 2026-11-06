#include "WoTRuntimeWorldProperties.h"

#include "Containers/StringConv.h"

namespace
{
class FWoTRuntimePropertyCursor
{
public:
	FWoTRuntimePropertyCursor(TConstArrayView<uint8> InBytes, int32 InOffset)
		: Bytes(InBytes), Offset(InOffset) {}
	bool ReadU8(uint8& Out)
	{
		if (!CanRead(1)) return false;
		Out = Bytes[Offset++]; return true;
	}
	bool ReadU32(uint32& Out)
	{
		if (!CanRead(4)) return false;
		Out = static_cast<uint32>(Bytes[Offset])
			| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
			| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
			| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
		Offset += 4; return true;
	}
	bool ReadString(FString& Out)
	{
		uint32 Count = 0;
		if (!ReadU32(Count) || Count > 1024u * 1024u || !CanRead(Count))
			return false;
		if (Count == 0) { Out.Empty(); return true; }
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset),
			static_cast<int32>(Count));
		if (Converted.Length() <= 0) return false;
		Out = FString(Converted.Length(), Converted.Get());
		FTCHARToUTF8 RoundTrip(*Out);
		if (RoundTrip.Length() != static_cast<int32>(Count)
			|| FMemory::Memcmp(RoundTrip.Get(), Bytes.GetData() + Offset, Count) != 0)
			return false;
		Offset += static_cast<int32>(Count); return true;
	}
	bool ReadBytes(uint32 Count, TArray<uint8>& Out)
	{
		if (!CanRead(Count)) return false;
		Out.Reset(static_cast<int32>(Count));
		Out.Append(Bytes.GetData() + Offset, static_cast<int32>(Count));
		Offset += static_cast<int32>(Count); return true;
	}
	int32 Tell() const { return Offset; }
	uint64 Remaining() const
	{
		return Offset >= 0
			&& static_cast<uint64>(Offset) <= static_cast<uint64>(Bytes.Num())
			? static_cast<uint64>(Bytes.Num()) - static_cast<uint64>(Offset)
			: 0;
	}
private:
	bool CanRead(uint64 Count) const
	{
		return Offset >= 0 && static_cast<uint64>(Offset) <= static_cast<uint64>(Bytes.Num())
			&& Count <= static_cast<uint64>(Bytes.Num()) - static_cast<uint64>(Offset);
	}
	TConstArrayView<uint8> Bytes;
	int32 Offset = 0;
};

bool ValidateResolvedStruct(const FWoTRuntimeWorldProperty& Property,
	FString& OutError, FStringView Where)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::Struct) return true;
	auto MemberIs = [&Property](int32 Index, const TCHAR* Name,
		EWoTRuntimeWorldPropertyType Type, int32 ValueBytes)
	{
		return Property.StructMembers.IsValidIndex(Index)
			&& Property.StructMembers[Index].Name == Name
			&& Property.StructMembers[Index].ArrayIndex == 0
			&& Property.StructMembers[Index].Type == Type
			&& Property.StructMembers[Index].Value.Num() == ValueBytes
			&& Property.StructMembers[Index].StructMembers.IsEmpty();
	};
	bool bValid = false;
	if (Property.StructName.Equals(TEXT("Resource"), ESearchCase::IgnoreCase))
	{
		bValid = Property.StructMembers.Num() == 3
			&& MemberIs(0, TEXT("ResourcePackageName"),
				EWoTRuntimeWorldPropertyType::Name, 0)
			&& MemberIs(1, TEXT("ResourceAdapterName"),
				EWoTRuntimeWorldPropertyType::Name, 0)
			&& MemberIs(2, TEXT("ItemCount"),
				EWoTRuntimeWorldPropertyType::Integer, 4);
	}
	else if (Property.StructName.Equals(
		TEXT("SoundEvent"), ESearchCase::IgnoreCase))
	{
		bValid = Property.StructMembers.Num() == 6
			&& MemberIs(0, TEXT("Delay"),
				EWoTRuntimeWorldPropertyType::Float, 4)
			&& MemberIs(1, TEXT("Event"),
				EWoTRuntimeWorldPropertyType::Name, 0)
			&& MemberIs(2, TEXT("SoundSourceTag"),
				EWoTRuntimeWorldPropertyType::Name, 0)
			&& MemberIs(3, TEXT("Sound"),
				EWoTRuntimeWorldPropertyType::Object, 0)
			&& MemberIs(4, TEXT("SoundTableTag"),
				EWoTRuntimeWorldPropertyType::Name, 0)
			&& MemberIs(5, TEXT("SoundTableSlot"),
				EWoTRuntimeWorldPropertyType::Name, 0);
	}
	else if (Property.StructName.Equals(TEXT("ThingT"), ESearchCase::IgnoreCase))
	{
		bValid = Property.StructMembers.Num() == 6
			&& MemberIs(0, TEXT("Actor"),
				EWoTRuntimeWorldPropertyType::Object, 0)
			&& MemberIs(1, TEXT("Count"),
				EWoTRuntimeWorldPropertyType::Integer, 4)
			&& MemberIs(2, TEXT("MinSpeed"),
				EWoTRuntimeWorldPropertyType::Float, 4)
			&& MemberIs(3, TEXT("MaxSpeed"),
				EWoTRuntimeWorldPropertyType::Float, 4)
			&& MemberIs(4, TEXT("DetailLevel"),
				EWoTRuntimeWorldPropertyType::Byte, 1)
			&& MemberIs(5, TEXT("DrawScale"),
				EWoTRuntimeWorldPropertyType::Float, 4);
	}
	else
	{
		bValid = Property.StructMembers.IsEmpty();
	}
	if (!bValid)
	{
		OutError = FString(Where) + TEXT(": invalid resolved struct-member schema");
	}
	return bValid;
}

bool DecodeProperty(FWoTRuntimePropertyCursor& Cursor,
	FWoTRuntimeWorldProperty& Property, FString& OutError,
	const TMap<FString, FString>* SemanticClasses,
	bool bDecodeStructMembers, EWoTStructMemberPolicy StructMemberPolicy,
	uint32 Depth, uint32& DescendantBudget, FStringView Where)
{
	uint8 TypeByte = 0;
	uint32 ValueBytes = 0;
	if (Depth > 8 || !Cursor.ReadString(Property.Name) || Property.Name.IsEmpty()
		|| !Cursor.ReadU8(TypeByte) || !Cursor.ReadU32(Property.ArrayIndex)
		|| !Cursor.ReadString(Property.StructName)
		|| !Cursor.ReadString(Property.NameValue)
		|| !Cursor.ReadString(Property.ObjectIdentity)
		|| !Cursor.ReadString(Property.ObjectClassIdentity)
		|| !Cursor.ReadU32(ValueBytes))
	{
		OutError = FString(Where) + TEXT(": header unreadable");
		return false;
	}
	if (TypeByte != 1 && TypeByte != 2 && TypeByte != 3 && TypeByte != 4
		&& TypeByte != 5 && TypeByte != 6 && TypeByte != 10 && TypeByte != 13)
	{
		OutError = FString(Where) + TEXT(": unsupported type"); return false;
	}
	if (((TypeByte == 5 || TypeByte == 6) && ValueBytes != 0)
		|| (TypeByte == 1 && ValueBytes != 1)
		|| (TypeByte == 2 && ValueBytes != 4)
		|| (TypeByte == 3 && ValueBytes != 1)
		|| (TypeByte == 4 && ValueBytes != 4)
		|| (TypeByte == 10 && Property.StructName.IsEmpty())
		|| (TypeByte != 10 && !Property.StructName.IsEmpty())
		|| (TypeByte == 6 && Property.NameValue.IsEmpty())
		|| (TypeByte != 6 && !Property.NameValue.IsEmpty())
		|| (TypeByte != 5 && (!Property.ObjectIdentity.IsEmpty()
			|| !Property.ObjectClassIdentity.IsEmpty()))
		|| (Property.ObjectIdentity.IsEmpty()
			!= Property.ObjectClassIdentity.IsEmpty()))
	{
		OutError = FString(Where) + TEXT(": invalid typed value shape");
		return false;
	}
	Property.Type = static_cast<EWoTRuntimeWorldPropertyType>(TypeByte);
	if (!Cursor.ReadBytes(ValueBytes, Property.Value))
	{
		OutError = FString(Where) + TEXT(": value truncated"); return false;
	}
	if (SemanticClasses != nullptr && !Property.ObjectIdentity.IsEmpty())
	{
		const FString* ActualClass = SemanticClasses->Find(
			Property.ObjectIdentity.ToLower());
		if (ActualClass == nullptr
			|| !ActualClass->Equals(Property.ObjectClassIdentity,
				ESearchCase::CaseSensitive))
		{
			OutError = FString(Where)
				+ TEXT(": object binding is outside the semantic registry");
			return false;
		}
	}
	if (bDecodeStructMembers)
	{
		uint32 MemberCount = 0;
		if (!Cursor.ReadU32(MemberCount))
		{
			OutError = FString(Where) + TEXT(": invalid struct-member count");
			return false;
		}
		// Even an empty encoded property consumes 34 bytes (seven length/value
		// prefixes plus type, array index, and nested-member count).  Apply the
		// closed schema and byte budget before Reserve or recursive decoding.
		static constexpr uint64 MinimumEncodedPropertyBytes = 34;
		if ((TypeByte != 10 && MemberCount != 0)
			|| MemberCount > DescendantBudget
			|| static_cast<uint64>(MemberCount)
				* MinimumEncodedPropertyBytes > Cursor.Remaining())
		{
			OutError = FString(Where)
				+ TEXT(": invalid resolved struct member count");
			return false;
		}
		DescendantBudget -= MemberCount;
		Property.StructMembers.Reserve(static_cast<int32>(MemberCount));
		TSet<FString> SeenMembers;
		for (uint32 MemberIndex = 0; MemberIndex < MemberCount; ++MemberIndex)
		{
			FWoTRuntimeWorldProperty& Member =
				Property.StructMembers.AddDefaulted_GetRef();
			const FString MemberWhere = FString::Printf(
				TEXT("%.*s member %u"), Where.Len(), Where.GetData(), MemberIndex);
			if (!DecodeProperty(Cursor, Member, OutError, SemanticClasses,
				true, StructMemberPolicy, Depth + 1,
				DescendantBudget, MemberWhere)) return false;
			const FString Key = Member.Name.ToLower()
				+ FString::Printf(TEXT("[%u]"), Member.ArrayIndex);
			if (SeenMembers.Contains(Key))
			{
				OutError = MemberWhere + TEXT(": duplicate member"); return false;
			}
			SeenMembers.Add(Key);
		}
	}
	return !bDecodeStructMembers
		|| StructMemberPolicy == EWoTStructMemberPolicy::ReflectedGeneric
		|| ValidateResolvedStruct(Property, OutError, Where);
}
}

bool WoTRuntimeWorldDecodeProperties(
	TConstArrayView<uint8> Bytes, int32& InOutOffset,
	TArray<FWoTRuntimeWorldProperty>& OutProperties, FString& OutError,
	const TMap<FString, FString>* SemanticClasses,
	bool bDecodeStructMembers, EWoTStructMemberPolicy StructMemberPolicy)
{
	FWoTRuntimePropertyCursor Cursor(Bytes, InOutOffset);
	uint32 Count = 0;
	const uint64 MinimumEncodedPropertyBytes = bDecodeStructMembers ? 34u : 30u;
	if (!Cursor.ReadU32(Count) || Count > 4000000
		|| static_cast<uint64>(Count) * MinimumEncodedPropertyBytes
			> Cursor.Remaining())
	{
		OutError = TEXT("property count unreadable or over bound");
		return false;
	}
	FString PreviousFolded;
	FString PreviousName;
	uint32 PreviousArray = 0;
	TArray<FWoTRuntimeWorldProperty> Decoded;
	Decoded.Reserve(static_cast<int32>(Count));
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		FWoTRuntimeWorldProperty Property;
		uint32 DescendantBudget = 4096;
		if (!DecodeProperty(Cursor, Property, OutError, SemanticClasses,
			bDecodeStructMembers, StructMemberPolicy, 0, DescendantBudget,
			FString::Printf(TEXT("property %u"), Index))) return false;
		const FString Where = FString::Printf(TEXT("property %u '%s'"),
			Index, *Property.Name);
		FString Folded = Property.Name.ToLower();
		if (Index > 0 && (Folded < PreviousFolded
			|| (Folded == PreviousFolded && (Property.ArrayIndex < PreviousArray
				|| (Property.ArrayIndex == PreviousArray
					&& Property.Name <= PreviousName)))))
		{
			OutError = Where + TEXT(": canonical order violated"); return false;
		}
		PreviousFolded = MoveTemp(Folded);
		PreviousName = Property.Name;
		PreviousArray = Property.ArrayIndex;
		Decoded.Add(MoveTemp(Property));
	}
	InOutOffset = Cursor.Tell();
	OutProperties = MoveTemp(Decoded);
	return true;
}

bool WoTRuntimeWorldPropertyVector(
	const FWoTRuntimeWorldProperty& Property, FVector& Out)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property.StructName.Equals(TEXT("Vector"), ESearchCase::IgnoreCase)
		|| Property.Value.Num() != 12)
	{
		return false;
	}
	float Values[3];
	FMemory::Memcpy(Values, Property.Value.GetData(), sizeof(Values));
	if (!FMath::IsFinite(Values[0]) || !FMath::IsFinite(Values[1])
		|| !FMath::IsFinite(Values[2]))
	{
		return false;
	}
	Out = FVector(Values[0], Values[1], Values[2]);
	return Out.GetAbsMax() <= 10000000.0;
}

bool WoTRuntimeWorldPropertyRotator(
	const FWoTRuntimeWorldProperty& Property, FRotator& Out)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property.StructName.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase)
		|| Property.Value.Num() != 12)
	{
		return false;
	}
	int32 Values[3];
	FMemory::Memcpy(Values, Property.Value.GetData(), sizeof(Values));
	constexpr double UnitsToDegrees = 360.0 / 65536.0;
	Out = FRotator(Values[0] * UnitsToDegrees,
		Values[1] * UnitsToDegrees, Values[2] * UnitsToDegrees);
	return true;
}

bool WoTRuntimeWorldPropertyFloat(
	const FWoTRuntimeWorldProperty& Property, float& Out)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::Float
		|| Property.Value.Num() != 4)
	{
		return false;
	}
	FMemory::Memcpy(&Out, Property.Value.GetData(), sizeof(Out));
	return FMath::IsFinite(Out);
}

bool WoTRuntimeWorldPropertyByte(
	const FWoTRuntimeWorldProperty& Property, uint8& Out)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::Byte
		|| Property.Value.Num() != 1)
	{
		return false;
	}
	Out = Property.Value[0];
	return true;
}

bool WoTRuntimeWorldPropertyBool(
	const FWoTRuntimeWorldProperty& Property, bool& Out)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::Boolean
		|| Property.Value.Num() != 1 || Property.Value[0] > 1)
	{
		return false;
	}
	Out = Property.Value[0] != 0;
	return true;
}

bool WoTRuntimeWorldPropertyInteger(
	const FWoTRuntimeWorldProperty& Property, int32& Out)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::Integer
		|| Property.Value.Num() != 4)
	{
		return false;
	}
	FMemory::Memcpy(&Out, Property.Value.GetData(), sizeof(Out));
	return true;
}

bool WoTRuntimeWorldPropertyUE1String(
	const FWoTRuntimeWorldProperty& Property, FString& Out)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::String
		|| Property.Value.IsEmpty())
	{
		return false;
	}
	const TArray<uint8>& Bytes = Property.Value;
	uint64 Magnitude = Bytes[0] & 0x3f;
	const bool bNegative = (Bytes[0] & 0x80) != 0;
	bool bMore = (Bytes[0] & 0x40) != 0;
	uint32 Shift = 6;
	int32 Offset = 1;
	for (uint32 Index = 0; bMore && Index < 4; ++Index)
	{
		if (!Bytes.IsValidIndex(Offset)) return false;
		const uint8 Next = Bytes[Offset++];
		Magnitude |= static_cast<uint64>(Next & 0x7f) << Shift;
		bMore = (Next & 0x80) != 0;
		Shift += 7;
	}
	if (bNegative || bMore || Magnitude == 0
		|| Magnitude > static_cast<uint64>(Bytes.Num() - Offset)
		|| static_cast<uint64>(Bytes.Num() - Offset) != Magnitude
		|| Bytes.Last() != 0)
	{
		return false;
	}
	Out.Reset(static_cast<int32>(Magnitude - 1));
	for (int32 Index = Offset; Index + 1 < Bytes.Num(); ++Index)
	{
		if (Bytes[Index] == 0) return false;
		Out.AppendChar(static_cast<TCHAR>(Bytes[Index]));
	}
	return !Out.IsEmpty();
}

const FWoTRuntimeWorldProperty* WoTRuntimeWorldFindProperty(
	const FWoTRuntimeWorldActor& Actor, FStringView Name, uint32 ArrayIndex)
{
	for (const FWoTRuntimeWorldProperty& Property : Actor.Properties)
	{
		if (Property.ArrayIndex == ArrayIndex
			&& FStringView(Property.Name).Equals(Name, ESearchCase::IgnoreCase))
		{
			return &Property;
		}
	}
	return nullptr;
}

const FWoTRuntimeWorldProperty* WoTRuntimeWorldStructMember(
	const FWoTRuntimeWorldProperty& Struct, FStringView MemberName, uint32 ArrayIndex)
{
	if (Struct.Type != EWoTRuntimeWorldPropertyType::Struct)
	{
		return nullptr;
	}
	for (const FWoTRuntimeWorldProperty& Member : Struct.StructMembers)
	{
		if (Member.ArrayIndex == ArrayIndex
			&& FStringView(Member.Name).Equals(MemberName, ESearchCase::IgnoreCase))
		{
			return &Member;
		}
	}
	return nullptr;
}

FTransform WoTRuntimeWorldActorTransform(const FWoTRuntimeWorldActor& Actor)
{
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector::OneVector;
	float UniformScale = 1.0f;
	for (const FWoTRuntimeWorldProperty& Property : Actor.Properties)
	{
		if (Property.Name.Equals(TEXT("Location"), ESearchCase::IgnoreCase))
		{
			(void)WoTRuntimeWorldPropertyVector(Property, Location);
		}
		else if (Property.Name.Equals(TEXT("Rotation"), ESearchCase::IgnoreCase))
		{
			(void)WoTRuntimeWorldPropertyRotator(Property, Rotation);
		}
		else if (Property.Name.Equals(TEXT("DrawScale3D"), ESearchCase::IgnoreCase))
		{
			(void)WoTRuntimeWorldPropertyVector(Property, Scale);
		}
		else if (Property.Name.Equals(TEXT("DrawScale"), ESearchCase::IgnoreCase))
		{
			(void)WoTRuntimeWorldPropertyFloat(Property, UniformScale);
		}
	}
	return FTransform(Rotation, Location, Scale * UniformScale);
}
