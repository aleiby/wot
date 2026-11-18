#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"

struct FWoTTypedProperty
{
	FString Name;
	uint8 Type = 0;
	uint32 ArrayIndex = 0;
	FString StructName;
	TArray<uint8> Value;
	FString NameValue;
	TOptional<int32> ObjectReference;
	FString ObjectIdentity;
	FString ObjectClassIdentity;
	TArray<FWoTTypedProperty> StructMembers;
};

struct FWoTTypedPropertyReadOptions
{
	uint64 MaximumProperties = 1000000;
	uint64* TotalPropertyCount = nullptr;
	bool bSavedClassDefaults = false;
	bool bRejectUnsupportedTypes = false;
	bool bRejectDuplicateIndexedFields = false;
	bool bNormalizeNegativeZero = false;
	bool bSortCanonical = false;
	bool bDecodeKnownStructs = true;
};

inline FString WoTTypedPropertyFold(FStringView Value)
{
	FString Result(Value);
	for (int32 Index = 0; Index < Result.Len(); ++Index)
		if (Result[Index] >= TEXT('A') && Result[Index] <= TEXT('Z'))
			Result[Index] += TEXT('a') - TEXT('A');
	return Result;
}

inline bool WoTTypedPropertySupported(uint8 Type)
{
	return Type == 1 || Type == 2 || Type == 3 || Type == 4
		|| Type == 5 || Type == 6 || Type == 10 || Type == 13;
}

template <typename ReaderType, typename ReadNameType, typename ReferenceValidType>
bool WoTReadTypedProperties(ReaderType& Reader, ReadNameType&& ReadName,
	ReferenceValidType&& ReferenceValid, TArray<FWoTTypedProperty>& Out,
	const FWoTTypedPropertyReadOptions& Options)
{
	TSet<FString> Seen;
	uint64 LocalCount = 0;
	for (;;)
	{
		uint64& Count = Options.TotalPropertyCount != nullptr
			? *Options.TotalPropertyCount : LocalCount;
		if (++Count > Options.MaximumProperties) return false;
		FString Name;
		if (!ReadName(Reader, Name)) return false;
		if (Name == TEXT("None"))
		{
			if (Options.bSortCanonical)
			{
				Out.Sort([](const FWoTTypedProperty& Left,
					const FWoTTypedProperty& Right)
				{
					const FString LF = WoTTypedPropertyFold(Left.Name);
					const FString RF = WoTTypedPropertyFold(Right.Name);
					if (LF != RF) return LF < RF;
					if (Left.ArrayIndex != Right.ArrayIndex)
						return Left.ArrayIndex < Right.ArrayIndex;
					return Left.Name < Right.Name;
				});
			}
			return true;
		}
		uint8 Info = 0;
		if (!Reader.ReadU8(Info)) return false;
		FWoTTypedProperty Property;
		Property.Name = MoveTemp(Name);
		Property.Type = Info & 0x0f;
		if (Options.bRejectUnsupportedTypes
			&& !WoTTypedPropertySupported(Property.Type)) return false;
		if (Property.Type == 10
			&& !ReadName(Reader, Property.StructName)) return false;
		uint64 Size = 0;
		switch (Info & 0x70)
		{
		case 0x00: Size = 1; break;
		case 0x10: Size = 2; break;
		case 0x20: Size = 4; break;
		case 0x30: Size = 12; break;
		case 0x40: Size = 16; break;
		case 0x50:
		{
			uint8 Value = 0;
			if (!Reader.ReadU8(Value)) return false;
			Size = Value;
			break;
		}
		case 0x60:
		{
			uint16 Value = 0;
			if (!Reader.ReadU16(Value)) return false;
			Size = Value;
			break;
		}
		case 0x70:
		{
			uint32 Value = 0;
			if (!Reader.ReadU32(Value)) return false;
			Size = Value;
			break;
		}
		default: return false;
		}
		if ((Info & 0x80) != 0 && Property.Type != 3)
		{
			uint8 First = 0;
			if (!Reader.ReadU8(First)) return false;
			if ((First & 0x80) == 0) Property.ArrayIndex = First;
			else if ((First & 0xc0) == 0x80)
			{
				uint8 Low = 0;
				if (!Reader.ReadU8(Low)) return false;
				Property.ArrayIndex =
					(static_cast<uint32>(First & 0x7f) << 8) | Low;
			}
			else
			{
				uint8 B0 = 0, B1 = 0, B2 = 0;
				if (!Reader.ReadU8(B0) || !Reader.ReadU8(B1)
					|| !Reader.ReadU8(B2)) return false;
				Property.ArrayIndex = (static_cast<uint32>(First & 0x3f) << 24)
					| (static_cast<uint32>(B0) << 16)
					| (static_cast<uint32>(B1) << 8) | B2;
			}
		}
		if (Options.bRejectDuplicateIndexedFields)
		{
			const FString Indexed = WoTTypedPropertyFold(Property.Name)
				+ FString::Printf(TEXT("[%u]"), Property.ArrayIndex);
			if (Seen.Contains(Indexed)) return false;
			Seen.Add(Indexed);
		}
		if (Property.Type == 3)
		{
			if (Options.bSavedClassDefaults && !Reader.Skip(Size)) return false;
			Property.Value.Add((Info & 0x80) != 0 ? 1 : 0);
		}
		else
		{
			if (!Reader.ReadBytes(Size, Property.Value)) return false;
			if (Options.bNormalizeNegativeZero && Property.Type == 4
				&& Property.Value.Num() == 4 && Property.Value[0] == 0
				&& Property.Value[1] == 0 && Property.Value[2] == 0
				&& Property.Value[3] == 0x80)
			{
				Property.Value[3] = 0;
			}
			else if (Property.Type == 5)
			{
				ReaderType ValueReader(Property.Value);
				int32 Reference = 0;
				if (!ValueReader.ReadCompact(Reference)
					|| ValueReader.Tell() != ValueReader.GetEnd()
					|| !ReferenceValid(Reference)) return false;
				Property.ObjectReference = Reference;
			}
			else if (Property.Type == 6)
			{
				ReaderType ValueReader(Property.Value);
				if (!ReadName(ValueReader, Property.NameValue)
					|| ValueReader.Tell() != ValueReader.GetEnd()) return false;
			}
			else if (Options.bDecodeKnownStructs && Property.Type == 10
				&& (Property.StructName.Equals(TEXT("Resource"),
						ESearchCase::IgnoreCase)
					|| Property.StructName.Equals(TEXT("SoundEvent"),
						ESearchCase::IgnoreCase)
					|| Property.StructName.Equals(TEXT("ThingT"),
						ESearchCase::IgnoreCase)))
			{
				ReaderType ValueReader(Property.Value);
				auto ReadResolvedName = [&ValueReader, &ReadName](
					const TCHAR* MemberName, FWoTTypedProperty& Member)
				{
					Member.Name = MemberName;
					Member.Type = 6;
					return ReadName(ValueReader, Member.NameValue);
				};
				auto ReadScalar = [&ValueReader](const TCHAR* MemberName,
					uint8 Type, uint32 Bytes, FWoTTypedProperty& Member)
				{
					Member.Name = MemberName;
					Member.Type = Type;
					return ValueReader.ReadBytes(Bytes, Member.Value);
				};
				auto ReadObject = [&ValueReader, &ReferenceValid](
					const TCHAR* MemberName, FWoTTypedProperty& Member)
				{
					int32 Reference = 0;
					Member.Name = MemberName;
					Member.Type = 5;
					if (!ValueReader.ReadCompact(Reference)
						|| !ReferenceValid(Reference)) return false;
					Member.ObjectReference = Reference;
					return true;
				};
				if (Property.StructName.Equals(
					TEXT("Resource"), ESearchCase::IgnoreCase))
				{
					Property.StructMembers.SetNum(3);
					if (!ReadResolvedName(TEXT("ResourcePackageName"),
							Property.StructMembers[0])
						|| !ReadResolvedName(TEXT("ResourceAdapterName"),
							Property.StructMembers[1])
						|| !ReadScalar(TEXT("ItemCount"), 2, 4,
							Property.StructMembers[2])) return false;
				}
				else if (Property.StructName.Equals(
					TEXT("SoundEvent"), ESearchCase::IgnoreCase))
				{
					Property.StructMembers.SetNum(6);
					if (!ReadScalar(TEXT("Delay"), 4, 4,
							Property.StructMembers[0])
						|| !ReadResolvedName(TEXT("Event"),
							Property.StructMembers[1])
						|| !ReadResolvedName(TEXT("SoundSourceTag"),
							Property.StructMembers[2])
						|| !ReadObject(TEXT("Sound"),
							Property.StructMembers[3])
						|| !ReadResolvedName(TEXT("SoundTableTag"),
							Property.StructMembers[4])
						|| !ReadResolvedName(TEXT("SoundTableSlot"),
							Property.StructMembers[5])) return false;
				}
				else
				{
					// WOT.SpewTrigger.ThingT declaration order from SpewTrigger.uc.
					Property.StructMembers.SetNum(6);
					if (!ReadObject(TEXT("Actor"), Property.StructMembers[0])
						|| !ReadScalar(TEXT("Count"), 2, 4,
							Property.StructMembers[1])
						|| !ReadScalar(TEXT("MinSpeed"), 4, 4,
							Property.StructMembers[2])
						|| !ReadScalar(TEXT("MaxSpeed"), 4, 4,
							Property.StructMembers[3])
						|| !ReadScalar(TEXT("DetailLevel"), 1, 1,
							Property.StructMembers[4])
						|| !ReadScalar(TEXT("DrawScale"), 4, 4,
							Property.StructMembers[5])) return false;
				}
				if (ValueReader.Tell() != ValueReader.GetEnd()) return false;
			}
		}
		Out.Add(MoveTemp(Property));
	}
}

struct FWoTTypedObjectBinding
{
	FString Identity;
	FString ClassIdentity;
};

template <typename BinderType>
FWoTBootstrapStatus WoTBindTypedPropertyReferences(
	TArray<FWoTTypedProperty>& Properties, BinderType&& Binder)
{
	for (FWoTTypedProperty& Property : Properties)
	{
		FWoTBootstrapStatus Members = WoTBindTypedPropertyReferences(
			Property.StructMembers, Binder);
		if (Members.IsError()) return Members;
		if (!Property.ObjectReference.IsSet()
			|| Property.ObjectReference.GetValue() == 0) continue;
		TWoTBootstrapResult<FWoTTypedObjectBinding> Binding =
			Binder(Property.ObjectReference.GetValue());
		if (Binding.IsError())
			return FWoTBootstrapStatus::Error(MoveTemp(Binding).GetError());
		FWoTTypedObjectBinding Bound = MoveTemp(Binding).GetValue();
		Property.ObjectIdentity = MoveTemp(Bound.Identity);
		Property.ObjectClassIdentity = MoveTemp(Bound.ClassIdentity);
	}
	return FWoTBootstrapStatus::Success();
}

bool WoTAppendCanonicalTypedProperties(TArray<uint8>& Payload,
	TConstArrayView<FWoTTypedProperty> Properties,
	bool bIncludeStructMembers = false);
