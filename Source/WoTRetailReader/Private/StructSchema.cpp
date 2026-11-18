#include "StructSchema.h"

#include "RemainingPackage.h"
#include "TypedProperties.h"

namespace
{
constexpr uint32 WoTStructMaximumDepth = 8;
constexpr uint32 WoTStructMaximumFields = 256;
constexpr uint32 WoTStructMaximumFixedArray = 4096;
constexpr uint32 WoTStructMaximumFixedArrayWrappers = 4;
constexpr uint32 WoTStructMaximumAncestors = 32;

enum class EWoTStructFieldKind : uint8
{
	Byte,
	Integer,
	Boolean,
	Float,
	Object,
	Name,
	String,
	Struct,
	Array,
	FixedArray,
	Map,
};

struct FWoTResolvedExport
{
	const FWoTRemainingSource* Source = nullptr;
	int32 Reference = 0;
};

struct FWoTStructField
{
	const FWoTRemainingSource* Source = nullptr;
	int32 Reference = 0;
	FString Name;
	EWoTStructFieldKind Kind = EWoTStructFieldKind::Byte;
	int32 ArrayDim = 0;
	uint32 Flags = 0;
	int32 Next = 0;
	int32 TypeReference = 0;
	int32 AuxiliaryReference = 0;
	int32 Count = 0;
};

struct FWoTStructSchema
{
	const FWoTRemainingSource* Source = nullptr;
	int32 Reference = 0;
	FString Name;
	TArray<const FWoTStructField*> Fields;
};

FString SchemaKey(const FWoTRemainingSource& Source, int32 Reference)
{
	return WoTRemainingFold(Source.Path)
		+ FString::Printf(TEXT("|%d"), Reference);
}

bool PropertyKind(FStringView ClassName, EWoTStructFieldKind& Out)
{
	if (ClassName.Equals(TEXT("ByteProperty"), ESearchCase::IgnoreCase))
		Out = EWoTStructFieldKind::Byte;
	else if (ClassName.Equals(TEXT("IntProperty"), ESearchCase::IgnoreCase))
		Out = EWoTStructFieldKind::Integer;
	else if (ClassName.Equals(TEXT("BoolProperty"), ESearchCase::IgnoreCase))
		Out = EWoTStructFieldKind::Boolean;
	else if (ClassName.Equals(TEXT("FloatProperty"), ESearchCase::IgnoreCase))
		Out = EWoTStructFieldKind::Float;
	else if (ClassName.Equals(TEXT("ObjectProperty"), ESearchCase::IgnoreCase)
		|| ClassName.Equals(TEXT("ClassProperty"), ESearchCase::IgnoreCase))
		Out = EWoTStructFieldKind::Object;
	else if (ClassName.Equals(TEXT("NameProperty"), ESearchCase::IgnoreCase))
		Out = EWoTStructFieldKind::Name;
	else if (ClassName.Equals(TEXT("StrProperty"), ESearchCase::IgnoreCase)
		|| ClassName.Equals(TEXT("StringProperty"), ESearchCase::IgnoreCase))
		Out = EWoTStructFieldKind::String;
	else if (ClassName.Equals(TEXT("StructProperty"), ESearchCase::IgnoreCase))
		Out = EWoTStructFieldKind::Struct;
	else if (ClassName.Equals(TEXT("ArrayProperty"), ESearchCase::IgnoreCase))
		Out = EWoTStructFieldKind::Array;
	else if (ClassName.Equals(TEXT("FixedArrayProperty"), ESearchCase::IgnoreCase))
		Out = EWoTStructFieldKind::FixedArray;
	else if (ClassName.Equals(TEXT("MapProperty"), ESearchCase::IgnoreCase))
		Out = EWoTStructFieldKind::Map;
	else
		return false;
	return true;
}

bool SkipTaggedProperties(FWoTRemainingReader& Reader,
	const FWoTRemainingSource& Source)
{
	for (uint32 PropertyIndex = 0; PropertyIndex < WoTRemainingMaxItems;
		++PropertyIndex)
	{
		FString Name;
		if (!WoTRemainingReadName(Reader, Source, Name)) return false;
		if (Name == TEXT("None")) return true;
		uint8 Info = 0;
		if (!Reader.ReadU8(Info)) return false;
		const uint8 Type = Info & 0x0f;
		if (Type == 10)
		{
			FString StructName;
			if (!WoTRemainingReadName(Reader, Source, StructName)) return false;
		}
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
			uint8 Count = 0;
			if (!Reader.ReadU8(Count)) return false;
			Size = Count;
			break;
		}
		case 0x60:
		{
			uint16 Count = 0;
			if (!Reader.ReadU16(Count)) return false;
			Size = Count;
			break;
		}
		case 0x70:
		{
			uint32 Count = 0;
			if (!Reader.ReadU32(Count)) return false;
			Size = Count;
			break;
		}
		default: return false;
		}
		if ((Info & 0x80) != 0 && Type != 3)
		{
			uint8 First = 0;
			if (!Reader.ReadU8(First)) return false;
			if ((First & 0x80) != 0)
			{
				if ((First & 0xc0) == 0x80)
				{
					uint8 Low = 0;
					if (!Reader.ReadU8(Low)) return false;
				}
				else if (!Reader.Skip(3)) return false;
			}
		}
		// Ordinary UObject bool tags keep their value in the info byte.
		if (Type != 3 && !Reader.Skip(Size)) return false;
	}
	return false;
}

bool SkipSerializedScript(FWoTRemainingReader& Reader, int32 MemorySize)
{
	if (MemorySize < 0) return false;
	int64 MemoryOffset = 0;
	uint32 RecursionDepth = 0;
	auto Raw = [&Reader, &MemoryOffset](uint32 Size)
	{
		if (!Reader.Skip(Size)) return false;
		MemoryOffset += Size;
		return true;
	};
	auto Reference = [&Reader, &MemoryOffset](int32* Out)
	{
		int32 Value = 0;
		if (!Reader.ReadCompact(Value)) return false;
		MemoryOffset += 4;
		if (Out != nullptr) *Out = Value;
		return true;
	};
	TFunction<bool(uint8&)> Expression;
	TFunction<bool()> Parameters;
	Parameters = [&Expression]()
	{
		for (uint32 Index = 0; Index < WoTRemainingMaxItems; ++Index)
		{
			uint8 Token = 0;
			if (!Expression(Token)) return false;
			if (Token == 0x16) return true;
		}
		return false;
	};
	Expression = [&Reader, &MemoryOffset, &RecursionDepth, &Raw,
		&Reference, &Expression, &Parameters](uint8& OutToken)
	{
		if (++RecursionDepth > 256) return false;
		uint8 Token = 0;
		if (!Reader.ReadU8(Token)) { --RecursionDepth; return false; }
		++MemoryOffset;
		bool bOk = true;
		auto OneExpression = [&Expression]()
		{
			uint8 Ignored = 0;
			return Expression(Ignored);
		};
		const bool bNoOperands = Token == 0x08 || Token == 0x0b || Token == 0x16
			|| Token == 0x17 || Token == 0x25 || Token == 0x26 || Token == 0x27
			|| Token == 0x28 || Token == 0x2a || Token == 0x2d || Token == 0x30
			|| Token == 0x31;
		const bool bObjectReference = Token == 0x00 || Token == 0x01
			|| Token == 0x02 || Token == 0x20 || Token == 0x29;
		if (Token >= 0x39 && Token < 0x60) bOk = OneExpression();
		else if (Token >= 0x70) bOk = Parameters();
		else if (Token >= 0x60) bOk = Raw(1) && Parameters();
		else if (Token == 0x0f || Token == 0x10 || Token == 0x14 || Token == 0x1a)
			bOk = OneExpression() && OneExpression();
		else if (Token == 0x06) bOk = Raw(2);
		else if (bObjectReference) bOk = Reference(nullptr);
		else if (bNoOperands) bOk = true;
		else if (Token == 0x04 || Token == 0x0d || Token == 0x0e)
			bOk = OneExpression();
		else if (Token == 0x1c || Token == 0x1b || Token == 0x38)
			bOk = Reference(nullptr) && Parameters();
		else if (Token == 0x12 || Token == 0x19)
			bOk = OneExpression() && Raw(3) && OneExpression();
		else if (Token == 0x11)
		{
			for (int32 Index = 0; bOk && Index < 4; ++Index) bOk = OneExpression();
		}
		else if (Token == 0x1d || Token == 0x1e) bOk = Raw(4);
		else if (Token == 0x1f)
		{
			bOk = false;
			for (uint32 Index = 0; Index < WoTRemainingMaxSourceBytes; ++Index)
			{
				uint8 Byte = 0;
				if (!Reader.ReadU8(Byte)) break;
				++MemoryOffset;
				if (Byte == 0) { bOk = true; break; }
			}
		}
		else if (Token == 0x34)
		{
			bOk = false;
			for (uint32 Index = 0; Index < WoTRemainingMaxSourceBytes / 2; ++Index)
			{
				uint16 Value = 0;
				if (!Reader.ReadU16(Value)) break;
				MemoryOffset += 2;
				if (Value == 0) { bOk = true; break; }
			}
		}
		else if (Token == 0x21) bOk = Reference(nullptr);
		else if (Token == 0x22 || Token == 0x23) bOk = Raw(12);
		else if (Token == 0x24 || Token == 0x2c) bOk = Raw(1);
		else if (Token == 0x13 || Token == 0x2e)
			bOk = Reference(nullptr) && OneExpression();
		else if (Token == 0x07) bOk = Raw(2) && OneExpression();
		else if (Token == 0x2f) bOk = OneExpression() && Raw(2);
		else if (Token == 0x05) bOk = Raw(1) && OneExpression();
		else if (Token == 0x09) bOk = Raw(2) && OneExpression();
		else if (Token == 0x0a)
		{
			uint16 Destination = 0;
			bOk = Reader.ReadU16(Destination);
			if (bOk) MemoryOffset += 2;
			if (bOk && Destination != 0xffff) bOk = OneExpression();
		}
		else if (Token == 0x0c)
		{
			bOk = (MemoryOffset & 3) == 0;
			for (uint32 Index = 0; bOk && Index < WoTRemainingMaxItems; ++Index)
			{
				int32 Label = -1;
				bOk = Reference(&Label) && Raw(4);
				if (bOk && Label == 0) break;
				if (Index + 1 == WoTRemainingMaxItems) bOk = false;
			}
		}
		else if (Token == 0x18) bOk = Raw(2) && OneExpression();
		else if (Token == 0x32 || Token == 0x33)
			bOk = Reference(nullptr) && OneExpression() && OneExpression();
		else if (Token == 0x36) bOk = Reference(nullptr) && OneExpression();
		else bOk = false;
		OutToken = Token;
		--RecursionDepth;
		return bOk;
	};
	for (uint32 Index = 0; MemoryOffset < MemorySize && Index < WoTRemainingMaxItems;
		++Index)
	{
		uint8 Token = 0;
		if (!Expression(Token)) return false;
	}
	return MemoryOffset == MemorySize;
}
}

class FWoTStructSchemaResolver::FImpl
{
public:
	explicit FImpl(FWoTRemainingPackageResolver& InPackages)
		: Packages(InPackages) {}

	bool DecodeClassDefaultStruct(const FWoTRemainingSource& ValueSource,
		int32 ClassExportReference, FStringView ClassIdentity,
		FWoTTypedProperty& Property, FString& OutError)
	{
		const FWoTStructField* Declaration = nullptr;
		if (!FindClassProperty(ValueSource, ClassExportReference,
			Property.Name, Declaration, OutError)) return false;
		if (Declaration == nullptr || Declaration->Kind != EWoTStructFieldKind::Struct)
		{
			OutError = TEXT("the reflected class field is not a struct property");
			return false;
		}
		if (Property.ArrayIndex >= static_cast<uint32>(Declaration->ArrayDim))
		{
			OutError = TEXT("the struct tag array index exceeds its reflected ArrayDim");
			return false;
		}
		FWoTResolvedExport StructExport;
		if (!ResolveReference(*Declaration->Source, Declaration->TypeReference,
			StructExport, OutError)) return false;
		const FWoTStructSchema* Schema = nullptr;
		TSet<FString> SchemaStack;
		if (!GetStructSchema(
			StructExport, Schema, SchemaStack, 0, OutError)) return false;
		if (Schema == nullptr || !Schema->Name.Equals(
			Property.StructName, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("the tag names struct '%s' but reflection names '%s'"),
				*Property.StructName, Schema == nullptr ? TEXT("") : *Schema->Name);
			return false;
		}
		FWoTRemainingReader Reader(Property.Value);
		TSet<FString> NestedStack;
		if (!DecodeStruct(*Schema, ValueSource, Property.Value, Reader,
			0, NestedStack, Property.StructMembers, OutError)) return false;
		if (Reader.Tell() != Reader.GetEnd())
		{
			OutError = TEXT("the reflected struct did not consume its exact value bytes");
			return false;
		}
		(void)ClassIdentity;
		return true;
	}

	bool MaterializeImplicitZeroClassDefaultStruct(
		const FWoTRemainingSource& ValueSource,
		int32 ClassExportReference, FStringView PropertyName,
		FWoTTypedProperty& OutProperty, FString& OutError)
	{
		const FWoTStructField* Declaration = nullptr;
		if (!FindClassProperty(ValueSource, ClassExportReference,
			PropertyName, Declaration, OutError)) return false;
		if (Declaration == nullptr || Declaration->Kind != EWoTStructFieldKind::Struct
			|| Declaration->ArrayDim != 1)
		{
			OutError = TEXT("the implicit-zero class field is not a scalar struct property");
			return false;
		}
		FWoTResolvedExport StructExport;
		if (!ResolveReference(*Declaration->Source, Declaration->TypeReference,
			StructExport, OutError)) return false;
		const FWoTStructSchema* Schema = nullptr;
		TSet<FString> SchemaStack;
		if (!GetStructSchema(StructExport, Schema, SchemaStack, 0, OutError)
			|| Schema == nullptr) return false;

		OutProperty = FWoTTypedProperty();
		OutProperty.Name = FString(PropertyName);
		OutProperty.Type = 10;
		OutProperty.StructName = Schema->Name;
		TSet<FString> ValueStack;
		TArray<FWoTTypedProperty> ReflectedMembers;
		// The common class-default pass below decodes the synthesized raw bytes
		// exactly once, alongside every tagged struct property.
		return BuildImplicitZeroStruct(*Schema, 0, ValueStack,
			OutProperty.Value, ReflectedMembers, OutError);
	}

private:
	bool ResolveReference(const FWoTRemainingSource& Source, int32 Reference,
		FWoTResolvedExport& Out, FString& OutError)
	{
		if (Reference > 0)
		{
			if (!Source.Exports.IsValidIndex(Reference - 1))
			{
				OutError = TEXT("a reflected export reference is out of range");
				return false;
			}
			Out = {&Source, Reference};
			return true;
		}
		if (Reference == 0)
		{
			OutError = TEXT("a required reflected reference is null");
			return false;
		}
		const int64 ImportIndex64 = -static_cast<int64>(Reference) - 1;
		if (ImportIndex64 < 0 || ImportIndex64 >= Source.Imports.Num())
		{
			OutError = TEXT("a reflected import reference is out of range");
			return false;
		}
		const FWoTRemainingImport& Import =
			Source.Imports[static_cast<int32>(ImportIndex64)];
		TWoTBootstrapResult<FString> ImportedPath =
			WoTRemainingObjectPath(Source, Reference);
		if (ImportedPath.IsError())
		{
			OutError = TEXT("a reflected import has an invalid outer chain");
			return false;
		}
		int32 Dot = INDEX_NONE;
		if (!ImportedPath.GetValue().FindChar(TEXT('.'), Dot) || Dot <= 0)
		{
			OutError = TEXT("a reflected import has no package-qualified path");
			return false;
		}
		const FString PackageName = ImportedPath.GetValue().Left(Dot);
		TWoTBootstrapResult<const FWoTRemainingSource*> ResolvedPackage =
			Packages.Resolve(PackageName, Source);
		if (ResolvedPackage.IsError())
		{
			OutError = FString::Printf(
				TEXT("the declaring package '%s' cannot be resolved"), *PackageName);
			return false;
		}
		const FWoTRemainingSource& Target = *ResolvedPackage.GetValue();
		int32 Match = 0;
		for (int32 ExportIndex = 0; ExportIndex < Target.Exports.Num(); ++ExportIndex)
		{
			const int32 CandidateReference = ExportIndex + 1;
			TWoTBootstrapResult<FString> CandidatePath =
				WoTRemainingObjectPath(Target, CandidateReference);
			if (CandidatePath.IsError()
				|| !CandidatePath.GetValue().Equals(
					ImportedPath.GetValue(), ESearchCase::IgnoreCase)) continue;
			const FWoTRemainingExport& Candidate = Target.Exports[ExportIndex];
			const FString* CandidateClass = Candidate.ClassReference == 0
				? nullptr : Target.ObjectName(Candidate.ClassReference);
			const bool bClassMatches = Import.ClassName.Equals(
				TEXT("Class"), ESearchCase::IgnoreCase)
				? Candidate.ClassReference == 0
				: CandidateClass != nullptr && CandidateClass->Equals(
					Import.ClassName, ESearchCase::IgnoreCase);
			if (!bClassMatches) continue;
			if (Match != 0)
			{
				OutError = TEXT("a reflected import resolves ambiguously");
				return false;
			}
			Match = CandidateReference;
		}
		if (Match == 0)
		{
			OutError = FString::Printf(
				TEXT("reflected import '%s' cannot be resolved"),
				*ImportedPath.GetValue());
			return false;
		}
		Out = {&Target, Match};
		return true;
	}

	bool ValidateOptionalReference(const FWoTRemainingSource& Source,
		int32 Reference, FString& OutError)
	{
		if (Reference == 0) return true;
		FWoTResolvedExport Ignored;
		return ResolveReference(Source, Reference, Ignored, OutError);
	}

	bool ReadCommonFieldNext(const FWoTResolvedExport& Field, int32& OutNext,
		FString& OutError)
	{
		const FWoTRemainingExport& Export =
			Field.Source->Exports[Field.Reference - 1];
		FWoTRemainingReader Reader(Field.Source->Bytes);
		int32 SuperField = 0;
		if (!WoTRemainingOpenExport(*Field.Source, Export, Reader)
			|| !SkipTaggedProperties(Reader, *Field.Source)
			|| !Reader.ReadCompact(SuperField) || !Reader.ReadCompact(OutNext)
			|| !ValidateOptionalReference(*Field.Source, SuperField, OutError)
			|| !ValidateOptionalReference(*Field.Source, OutNext, OutError))
		{
			if (OutError.IsEmpty())
				OutError = TEXT("a reflected UField prefix is malformed");
			return false;
		}
		return true;
	}

	bool ValidateEnum(const FWoTRemainingSource& Source, int32 Reference,
		FString& OutError)
	{
		if (Reference == 0) return true;
		FWoTResolvedExport EnumExport;
		if (!ResolveReference(Source, Reference, EnumExport, OutError)) return false;
		const FWoTRemainingExport& Export =
			EnumExport.Source->Exports[EnumExport.Reference - 1];
		const FString* ClassName = EnumExport.Source->ObjectName(Export.ClassReference);
		FWoTRemainingReader Reader(EnumExport.Source->Bytes);
		int32 SuperField = 0, NextField = 0, Count = 0;
		if (ClassName == nullptr
			|| !ClassName->Equals(TEXT("Enum"), ESearchCase::IgnoreCase)
			|| !WoTRemainingOpenExport(*EnumExport.Source, Export, Reader)
			|| !SkipTaggedProperties(Reader, *EnumExport.Source)
			|| !Reader.ReadCompact(SuperField) || !Reader.ReadCompact(NextField)
			|| !Reader.ReadCompact(Count) || Count < 0
			|| Count > static_cast<int32>(WoTRemainingMaxItems)
			|| !ValidateOptionalReference(*EnumExport.Source, SuperField, OutError)
			|| !ValidateOptionalReference(*EnumExport.Source, NextField, OutError))
		{
			if (OutError.IsEmpty()) OutError = TEXT("a reflected enum is malformed");
			return false;
		}
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FString EnumName;
			if (!WoTRemainingReadName(Reader, *EnumExport.Source, EnumName))
			{
				OutError = TEXT("a reflected enum name is invalid");
				return false;
			}
		}
		if (Reader.Tell() != Reader.GetEnd())
		{
			OutError = TEXT("a reflected enum has trailing bytes");
			return false;
		}
		return true;
	}

	bool GetProperty(const FWoTResolvedExport& Resolved,
		const FWoTStructField*& Out, FString& OutError)
	{
		const FString Key = SchemaKey(*Resolved.Source, Resolved.Reference);
		if (const TUniquePtr<FWoTStructField>* Cached = Properties.Find(Key))
		{
			Out = Cached->Get();
			return true;
		}
		if (PropertiesInProgress.Contains(Key))
		{
			OutError = TEXT("a reflected property-type cycle was found");
			return false;
		}
		PropertiesInProgress.Add(Key);
		const FWoTRemainingExport& Export =
			Resolved.Source->Exports[Resolved.Reference - 1];
		const FString* ClassName = Resolved.Source->ObjectName(Export.ClassReference);
		EWoTStructFieldKind Kind;
		if (ClassName == nullptr || !PropertyKind(*ClassName, Kind))
		{
			PropertiesInProgress.Remove(Key);
			OutError = TEXT("a reflected child is not a supported UProperty class");
			return false;
		}
		FWoTRemainingReader Reader(Resolved.Source->Bytes);
		TUniquePtr<FWoTStructField> Property = MakeUnique<FWoTStructField>();
		Property->Source = Resolved.Source;
		Property->Reference = Resolved.Reference;
		Property->Name = Export.Name;
		Property->Kind = Kind;
		int32 SuperField = 0;
		FString Category;
		bool bOk = WoTRemainingOpenExport(*Resolved.Source, Export, Reader)
			&& SkipTaggedProperties(Reader, *Resolved.Source)
			&& Reader.ReadCompact(SuperField)
			&& Reader.ReadCompact(Property->Next)
			&& Reader.ReadI32(Property->ArrayDim) && Property->ArrayDim > 0
			&& Property->ArrayDim <= static_cast<int32>(WoTStructMaximumFields)
			&& Reader.ReadU32(Property->Flags)
			&& WoTRemainingReadName(Reader, *Resolved.Source, Category);
		if (bOk && (Property->Flags & 0x20) != 0)
		{
			uint16 RepOffset = 0;
			bOk = Reader.ReadU16(RepOffset);
		}
		if (bOk)
		{
			switch (Kind)
			{
			case EWoTStructFieldKind::Byte:
			case EWoTStructFieldKind::Object:
			case EWoTStructFieldKind::Struct:
			case EWoTStructFieldKind::Array:
				bOk = Reader.ReadCompact(Property->TypeReference);
				break;
			case EWoTStructFieldKind::FixedArray:
				bOk = Reader.ReadCompact(Property->TypeReference)
					&& Reader.ReadI32(Property->Count) && Property->Count > 0
					&& Property->Count <= static_cast<int32>(WoTStructMaximumFixedArray);
				break;
			case EWoTStructFieldKind::Map:
				bOk = Reader.ReadCompact(Property->TypeReference)
					&& Reader.ReadCompact(Property->AuxiliaryReference);
				break;
			default: break;
			}
			if (Kind == EWoTStructFieldKind::Object
				&& ClassName->Equals(TEXT("ClassProperty"), ESearchCase::IgnoreCase))
			{
				bOk = bOk && Reader.ReadCompact(Property->AuxiliaryReference);
			}
		}
		bOk = bOk && Reader.Tell() == Reader.GetEnd()
			&& ValidateOptionalReference(*Resolved.Source, SuperField, OutError)
			&& ValidateOptionalReference(*Resolved.Source, Property->Next, OutError);
		if (bOk && Kind == EWoTStructFieldKind::Byte)
			bOk = ValidateEnum(*Resolved.Source, Property->TypeReference, OutError);
		else if (bOk && Kind == EWoTStructFieldKind::Object)
		{
			bOk = WoTRemainingReferenceValid(
				*Resolved.Source, Property->TypeReference)
				&& WoTRemainingReferenceValid(
					*Resolved.Source, Property->AuxiliaryReference);
			if (!bOk) OutError = TEXT("an object property's class reference is invalid");
		}
		else if (bOk && (Kind == EWoTStructFieldKind::Struct
			|| Kind == EWoTStructFieldKind::Array
			|| Kind == EWoTStructFieldKind::FixedArray
			|| Kind == EWoTStructFieldKind::Map))
		{
			bOk = ValidateOptionalReference(
				*Resolved.Source, Property->TypeReference, OutError);
			if (bOk && Kind == EWoTStructFieldKind::Map)
			{
				bOk = ValidateOptionalReference(
					*Resolved.Source, Property->AuxiliaryReference, OutError);
			}
		}
		PropertiesInProgress.Remove(Key);
		if (!bOk)
		{
			if (OutError.IsEmpty())
				OutError = TEXT("a reflected UProperty payload is malformed");
			return false;
		}
		TUniquePtr<FWoTStructField>& Slot = Properties.Add(Key, MoveTemp(Property));
		Out = Slot.Get();
		return true;
	}

	bool GetStructSchema(const FWoTResolvedExport& Resolved,
		const FWoTStructSchema*& Out, TSet<FString>& Stack,
		uint32 AncestorCount, FString& OutError)
	{
		if (AncestorCount > WoTStructMaximumAncestors)
		{
			OutError = TEXT("the reflected superstruct chain exceeds 32 ancestors");
			return false;
		}
		const FString Key = SchemaKey(*Resolved.Source, Resolved.Reference);
		if (const TUniquePtr<FWoTStructSchema>* Cached = Structs.Find(Key))
		{
			Out = Cached->Get();
			return true;
		}
		if (Stack.Contains(Key))
		{
			OutError = TEXT("the reflected superstruct chain contains a cycle");
			return false;
		}
		Stack.Add(Key);
		const FWoTRemainingExport& Export =
			Resolved.Source->Exports[Resolved.Reference - 1];
		const FString* ClassName = Resolved.Source->ObjectName(Export.ClassReference);
		if (ClassName == nullptr
			|| (!ClassName->Equals(TEXT("ScriptStruct"), ESearchCase::IgnoreCase)
				&& !ClassName->Equals(TEXT("Struct"), ESearchCase::IgnoreCase)))
		{
			Stack.Remove(Key);
			OutError = TEXT("a struct reference does not name a Struct export");
			return false;
		}
		FWoTRemainingReader Reader(Resolved.Source->Bytes);
		int32 SuperField = 0, NextField = 0, ScriptText = 0, Children = 0;
		int32 FriendlyIndex = 0, Line = 0, TextPosition = 0, ScriptSize = 0;
		bool bOk = WoTRemainingOpenExport(*Resolved.Source, Export, Reader);
		if (bOk && Export.ClassReference != 0)
			bOk = SkipTaggedProperties(Reader, *Resolved.Source);
		bOk = bOk && Reader.ReadCompact(SuperField) && Reader.ReadCompact(NextField)
			&& Reader.ReadCompact(ScriptText) && Reader.ReadCompact(Children)
			&& Reader.ReadCompact(FriendlyIndex)
			&& Resolved.Source->NameAt(FriendlyIndex) != nullptr
			&& Reader.ReadI32(Line) && Reader.ReadI32(TextPosition)
			&& Reader.ReadI32(ScriptSize) && SkipSerializedScript(Reader, ScriptSize)
			&& Reader.Tell() == Reader.GetEnd()
			&& ValidateOptionalReference(*Resolved.Source, SuperField, OutError)
			&& ValidateOptionalReference(*Resolved.Source, NextField, OutError)
			&& ValidateOptionalReference(*Resolved.Source, ScriptText, OutError)
			&& ValidateOptionalReference(*Resolved.Source, Children, OutError);
		TUniquePtr<FWoTStructSchema> Schema = MakeUnique<FWoTStructSchema>();
		Schema->Source = Resolved.Source;
		Schema->Reference = Resolved.Reference;
		Schema->Name = Export.Name;
		TSet<FString> SeenChildren;
		int32 Current = Children;
		while (bOk && Current != 0)
		{
			FWoTResolvedExport Child;
			bOk = ResolveReference(*Resolved.Source, Current, Child, OutError);
			if (!bOk) break;
			const FString ChildKey = SchemaKey(*Child.Source, Child.Reference);
			if (SeenChildren.Contains(ChildKey))
			{
				OutError = TEXT("a reflected struct Children/Next chain contains a cycle");
				bOk = false;
				break;
			}
			SeenChildren.Add(ChildKey);
			const FWoTRemainingExport& ChildExport =
				Child.Source->Exports[Child.Reference - 1];
			const FString* ChildClass = Child.Source->ObjectName(
				ChildExport.ClassReference);
			EWoTStructFieldKind ChildKind;
			int32 ChildNext = 0;
			if (ChildClass != nullptr && PropertyKind(*ChildClass, ChildKind))
			{
				const FWoTStructField* Property = nullptr;
				bOk = GetProperty(Child, Property, OutError);
				if (bOk)
				{
					Schema->Fields.Add(Property);
					ChildNext = Property->Next;
				}
			}
			else
			{
				bOk = ReadCommonFieldNext(Child, ChildNext, OutError);
			}
			if (Schema->Fields.Num() > static_cast<int32>(WoTStructMaximumFields))
			{
				OutError = TEXT("a reflected struct exceeds its flattened field bound");
				bOk = false;
			}
			Current = ChildNext;
		}
		if (bOk && SuperField != 0)
		{
			FWoTResolvedExport SuperExport;
			const FWoTStructSchema* SuperSchema = nullptr;
			bOk = ResolveReference(*Resolved.Source, SuperField, SuperExport, OutError)
				&& GetStructSchema(SuperExport, SuperSchema, Stack,
					AncestorCount + 1, OutError);
			if (bOk)
			{
				Schema->Fields.Append(SuperSchema->Fields);
				if (Schema->Fields.Num() > static_cast<int32>(WoTStructMaximumFields))
				{
					OutError = TEXT("a reflected struct exceeds its flattened field bound");
					bOk = false;
				}
			}
		}
		Stack.Remove(Key);
		if (!bOk)
		{
			if (OutError.IsEmpty()) OutError = TEXT("a reflected UStruct is malformed");
			return false;
		}
		TUniquePtr<FWoTStructSchema>& Slot = Structs.Add(Key, MoveTemp(Schema));
		Out = Slot.Get();
		return true;
	}

	bool FindClassProperty(const FWoTRemainingSource& Source,
		int32 ClassReference, FStringView Name, const FWoTStructField*& Out,
		FString& OutError)
	{
		FWoTResolvedExport Current{&Source, ClassReference};
		TSet<FString> Seen;
		for (uint32 Depth = 0; Depth < WoTRemainingMaxItems; ++Depth)
		{
			if (Current.Reference <= 0
				|| !Current.Source->Exports.IsValidIndex(Current.Reference - 1))
			{
				OutError = TEXT("a class property lookup has an invalid class export");
				return false;
			}
			const FString Key = SchemaKey(*Current.Source, Current.Reference);
			if (Seen.Contains(Key))
			{
				OutError = TEXT("the reflected class super chain contains a cycle");
				return false;
			}
			Seen.Add(Key);
			const FWoTStructField* Match = nullptr;
			for (int32 ExportIndex = 0; ExportIndex < Current.Source->Exports.Num();
				++ExportIndex)
			{
				const FWoTRemainingExport& Candidate =
					Current.Source->Exports[ExportIndex];
				if (Candidate.Outer != Current.Reference
					|| !FStringView(Candidate.Name).Equals(
						Name, ESearchCase::IgnoreCase)) continue;
				const FString* CandidateClass =
					Current.Source->ObjectName(Candidate.ClassReference);
				EWoTStructFieldKind CandidateKind;
				if (CandidateClass == nullptr
					|| !PropertyKind(*CandidateClass, CandidateKind)) continue;
				if (Match != nullptr)
				{
					OutError = TEXT("a reflected class declares a duplicate property");
					return false;
				}
				FWoTResolvedExport PropertyExport{Current.Source, ExportIndex + 1};
				if (!GetProperty(PropertyExport, Match, OutError)) return false;
			}
			if (Match != nullptr)
			{
				Out = Match;
				return true;
			}
			const int32 SuperReference =
				Current.Source->Exports[Current.Reference - 1].SuperReference;
			if (SuperReference == 0) break;
			if (!ResolveReference(*Current.Source, SuperReference, Current, OutError))
				return false;
		}
		OutError = FString::Printf(TEXT("property '%.*s' has no reflected declaration"),
			Name.Len(), Name.GetData());
		return false;
	}

	bool BuildImplicitZeroStruct(const FWoTStructSchema& Schema,
		uint32 Depth, TSet<FString>& Stack, TArray<uint8>& OutValue,
		TArray<FWoTTypedProperty>& OutMembers, FString& OutError)
	{
		if (Depth > WoTStructMaximumDepth)
		{
			OutError = TEXT("implicit-zero struct materialization exceeds depth 8");
			return false;
		}
		const FString Key = SchemaKey(*Schema.Source, Schema.Reference);
		if (Stack.Contains(Key))
		{
			OutError = TEXT("implicit-zero struct materialization contains a cycle");
			return false;
		}
		Stack.Add(Key);
		OutValue.Reset();
		OutMembers.Reset();
		for (const FWoTStructField* Field : Schema.Fields)
		{
			if ((Field->Flags & (0x1000u | 0x2000u)) != 0) continue;
			for (int32 ArrayIndex = 0; ArrayIndex < Field->ArrayDim; ++ArrayIndex)
			{
				FWoTTypedProperty Member;
				Member.Name = Field->Name;
				Member.ArrayIndex = static_cast<uint32>(ArrayIndex);
				switch (Field->Kind)
				{
				case EWoTStructFieldKind::Byte:
					Member.Type = 1;
					Member.Value.AddZeroed(1);
					break;
				case EWoTStructFieldKind::Integer:
					Member.Type = 2;
					Member.Value.AddZeroed(4);
					break;
				case EWoTStructFieldKind::Boolean:
					Member.Type = 3;
					Member.Value.AddZeroed(1);
					break;
				case EWoTStructFieldKind::Float:
					Member.Type = 4;
					Member.Value.AddZeroed(4);
					break;
				case EWoTStructFieldKind::Struct:
				{
					Member.Type = 10;
					FWoTResolvedExport NestedExport;
					const FWoTStructSchema* Nested = nullptr;
					TSet<FString> NestedSchemaStack;
					if (!ResolveReference(*Field->Source, Field->TypeReference,
						NestedExport, OutError)
						|| !GetStructSchema(NestedExport, Nested,
							NestedSchemaStack, 0, OutError)
						|| Nested == nullptr)
					{
						Stack.Remove(Key);
						return false;
					}
					Member.StructName = Nested->Name;
					if (!BuildImplicitZeroStruct(*Nested, Depth + 1, Stack,
						Member.Value, Member.StructMembers, OutError))
					{
						Stack.Remove(Key);
						return false;
					}
					break;
				}
				default:
					OutError = FString::Printf(TEXT(
						"implicit-zero member '%s' has an unsupported reflected type"),
						*Field->Name);
					Stack.Remove(Key);
					return false;
				}
				OutValue.Append(Member.Value);
				OutMembers.Add(MoveTemp(Member));
			}
		}
		Stack.Remove(Key);
		return true;
	}

	bool CopyValueSpan(TConstArrayView<uint8> Bytes, uint64 Start, uint64 End,
		TArray<uint8>& Out)
	{
		if (End < Start || End > static_cast<uint64>(Bytes.Num())
			|| End - Start > static_cast<uint64>(MAX_int32)) return false;
		Out.Reset(static_cast<int32>(End - Start));
		Out.Append(Bytes.GetData() + Start, static_cast<int32>(End - Start));
		return true;
	}

	bool DecodeStruct(const FWoTStructSchema& Schema,
		const FWoTRemainingSource& ValueSource, TConstArrayView<uint8> ValueBytes,
		FWoTRemainingReader& Reader, uint32 Depth, TSet<FString>& Stack,
		TArray<FWoTTypedProperty>& Out, FString& OutError)
	{
		if (Depth > WoTStructMaximumDepth)
		{
			OutError = TEXT("nested struct decoding exceeds depth 8");
			return false;
		}
		const FString Key = SchemaKey(*Schema.Source, Schema.Reference);
		if (Stack.Contains(Key))
		{
			OutError = TEXT("nested struct resolution contains a cycle");
			return false;
		}
		Stack.Add(Key);
		TSet<FString> ActivePropertyReferences;
		for (const FWoTStructField* Field : Schema.Fields)
		{
			if ((Field->Flags & (0x1000u | 0x2000u)) != 0) continue;
			for (int32 ArrayIndex = 0; ArrayIndex < Field->ArrayDim; ++ArrayIndex)
			{
				if (!DecodeField(*Field, Field->Name,
					static_cast<uint32>(ArrayIndex), ValueSource, ValueBytes,
					Reader, Depth, Stack, ActivePropertyReferences, 0,
					Out, OutError))
				{
					Stack.Remove(Key);
					return false;
				}
			}
		}
		Stack.Remove(Key);
		return true;
	}

	bool DecodeField(const FWoTStructField& Field, FStringView MemberName,
		uint32 ArrayIndex, const FWoTRemainingSource& ValueSource,
		TConstArrayView<uint8> ValueBytes, FWoTRemainingReader& Reader,
		uint32 Depth, TSet<FString>& Stack,
		TSet<FString>& ActivePropertyReferences, uint32 FixedArrayDepth,
		TArray<FWoTTypedProperty>& Out, FString& OutError)
	{
		if (Field.Kind == EWoTStructFieldKind::Array
			|| Field.Kind == EWoTStructFieldKind::Map)
		{
			OutError = FString::Printf(TEXT("member '%.*s' uses an unsupported %s"),
				MemberName.Len(), MemberName.GetData(),
				Field.Kind == EWoTStructFieldKind::Array
					? TEXT("dynamic array") : TEXT("map"));
			return false;
		}
		if (Field.Kind == EWoTStructFieldKind::FixedArray)
		{
			if (FixedArrayDepth >= WoTStructMaximumFixedArrayWrappers)
			{
				OutError = TEXT("fixed-array property wrappers exceed depth 4");
				return false;
			}
			const FString PropertyKey = SchemaKey(*Field.Source, Field.Reference);
			if (ActivePropertyReferences.Contains(PropertyKey))
			{
				OutError = TEXT("fixed-array property references contain a cycle");
				return false;
			}
			ActivePropertyReferences.Add(PropertyKey);
			FWoTResolvedExport InnerExport;
			const FWoTStructField* Inner = nullptr;
			if (!ResolveReference(*Field.Source, Field.TypeReference,
				InnerExport, OutError) || !GetProperty(InnerExport, Inner, OutError))
			{
				ActivePropertyReferences.Remove(PropertyKey);
				return false;
			}
			for (int32 InnerIndex = 0; InnerIndex < Field.Count; ++InnerIndex)
			{
				const uint64 Flattened = static_cast<uint64>(ArrayIndex)
					* static_cast<uint64>(Field.Count) + static_cast<uint64>(InnerIndex);
				if (Flattened > MAX_uint32
					|| !DecodeField(*Inner, MemberName, static_cast<uint32>(Flattened),
						ValueSource, ValueBytes, Reader, Depth, Stack,
						ActivePropertyReferences, FixedArrayDepth + 1, Out, OutError))
				{
					ActivePropertyReferences.Remove(PropertyKey);
					return false;
				}
			}
			ActivePropertyReferences.Remove(PropertyKey);
			return true;
		}

		FWoTTypedProperty Member;
		Member.Name = FString(MemberName);
		Member.ArrayIndex = ArrayIndex;
		const uint64 Start = Reader.Tell();
		switch (Field.Kind)
		{
		case EWoTStructFieldKind::Byte:
			Member.Type = 1;
			if (!Reader.ReadBytes(1, Member.Value)) return ValueFailure(MemberName, OutError);
			break;
		case EWoTStructFieldKind::Integer:
			Member.Type = 2;
			if (!Reader.ReadBytes(4, Member.Value)) return ValueFailure(MemberName, OutError);
			break;
		case EWoTStructFieldKind::Boolean:
			Member.Type = 3;
			if (!Reader.ReadBytes(1, Member.Value)) return ValueFailure(MemberName, OutError);
			break;
		case EWoTStructFieldKind::Float:
			Member.Type = 4;
			if (!Reader.ReadBytes(4, Member.Value)) return ValueFailure(MemberName, OutError);
			break;
		case EWoTStructFieldKind::Object:
		{
			Member.Type = 5;
			int32 Reference = 0;
			if (!Reader.ReadCompact(Reference)
				|| !WoTRemainingReferenceValid(ValueSource, Reference))
				return ValueFailure(MemberName, OutError);
			Member.ObjectReference = Reference;
			break;
		}
		case EWoTStructFieldKind::Name:
			Member.Type = 6;
			if (!WoTRemainingReadName(Reader, ValueSource, Member.NameValue))
				return ValueFailure(MemberName, OutError);
			break;
		case EWoTStructFieldKind::String:
		{
			Member.Type = 13;
			int32 Count = 0;
			if (!Reader.ReadCompact(Count)) return ValueFailure(MemberName, OutError);
			if (Count != 0)
			{
				const int64 CharacterCount = Count < 0
					? -static_cast<int64>(Count) : static_cast<int64>(Count);
				const int64 ByteCount = Count < 0 ? CharacterCount * 2 : CharacterCount;
				TArray<uint8> Encoded;
				if (CharacterCount <= 0 || ByteCount <= 0 || ByteCount > MAX_int32
					|| !Reader.ReadBytes(static_cast<uint64>(ByteCount), Encoded)
					|| (Count > 0 && Encoded.Last() != 0)
					|| (Count < 0 && (Encoded.Num() < 2
						|| Encoded[Encoded.Num() - 1] != 0
						|| Encoded[Encoded.Num() - 2] != 0)))
					return ValueFailure(MemberName, OutError);
			}
			if (!CopyValueSpan(ValueBytes, Start, Reader.Tell(), Member.Value))
				return ValueFailure(MemberName, OutError);
			break;
		}
		case EWoTStructFieldKind::Struct:
		{
			Member.Type = 10;
			FWoTResolvedExport StructExport;
			const FWoTStructSchema* Nested = nullptr;
			TSet<FString> SchemaStack;
			if (!ResolveReference(*Field.Source, Field.TypeReference,
				StructExport, OutError)
				|| !GetStructSchema(
					StructExport, Nested, SchemaStack, 0, OutError))
				return false;
			Member.StructName = Nested->Name;
			if (!DecodeStruct(*Nested, ValueSource, ValueBytes, Reader,
				Depth + 1, Stack, Member.StructMembers, OutError)
				|| !CopyValueSpan(ValueBytes, Start, Reader.Tell(), Member.Value))
				return false;
			break;
		}
		default:
			OutError = TEXT("an unsupported reflected member kind reached decoding");
			return false;
		}
		Out.Add(MoveTemp(Member));
		return true;
	}

	bool ValueFailure(FStringView MemberName, FString& OutError)
	{
		OutError = FString::Printf(TEXT("member '%.*s' has truncated or invalid bytes"),
			MemberName.Len(), MemberName.GetData());
		return false;
	}

	FWoTRemainingPackageResolver& Packages;
	TMap<FString, TUniquePtr<FWoTStructField>> Properties;
	TMap<FString, TUniquePtr<FWoTStructSchema>> Structs;
	TSet<FString> PropertiesInProgress;
};

FWoTStructSchemaResolver::FWoTStructSchemaResolver(
	FWoTRemainingPackageResolver& InPackages)
	: Impl(MakeUnique<FImpl>(InPackages))
{
}

FWoTStructSchemaResolver::~FWoTStructSchemaResolver() = default;

bool FWoTStructSchemaResolver::DecodeClassDefaultStruct(
	const FWoTRemainingSource& ValueSource, int32 ClassExportReference,
	FStringView ClassIdentity, FWoTTypedProperty& Property, FString& OutError)
{
	return Impl->DecodeClassDefaultStruct(ValueSource, ClassExportReference,
		ClassIdentity, Property, OutError);
}

bool FWoTStructSchemaResolver::MaterializeImplicitZeroClassDefaultStruct(
	const FWoTRemainingSource& ValueSource, int32 ClassExportReference,
	FStringView PropertyName, FWoTTypedProperty& OutProperty, FString& OutError)
{
	return Impl->MaterializeImplicitZeroClassDefaultStruct(ValueSource,
		ClassExportReference, PropertyName, OutProperty, OutError);
}
