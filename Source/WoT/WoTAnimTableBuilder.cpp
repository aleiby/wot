#include "WoTAnimTable.h"

#include "WoTRetailBuilderCanonical.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeStructInheritance.h"
#include "World/WoTRuntimeWorldProperties.h"

namespace
{
struct FWoTAnimClassSpec
{
	const TCHAR* TableName;
	const TCHAR* ClassIdentity;
};

// Retail identities: exports/script/WoTPawns/AnimationTable*.uc:8.
constexpr FWoTAnimClassSpec GWoTAnimClassSpecs[] = {
	{TEXT("Trolloc"), TEXT("WoTPawns.AnimationTableTrolloc")},
	{TEXT("TrollocSideArm"), TEXT("WoTPawns.AnimationTableTrollocSideArm")},
	{TEXT("BATrolloc"), TEXT("WoTPawns.AnimationTableBATrolloc")},
	{TEXT("Myrddraal"), TEXT("WoTPawns.AnimationTableMyrddraal")},
	{TEXT("Hound"), TEXT("WoTPawns.AnimationTableHound")},
	{TEXT("Minion"), TEXT("WoTPawns.AnimationTableMinion")},
	{TEXT("Soldier"), TEXT("WoTPawns.AnimationTableSoldier")},
	{TEXT("Archer"), TEXT("WoTPawns.AnimationTableArcher")},
	{TEXT("Warder"), TEXT("WoTPawns.AnimationTableWarder")},
	{TEXT("Whitecloak"), TEXT("WoTPawns.AnimationTableWhitecloak")},
	{TEXT("AesSedai"), TEXT("WoTPawns.AnimationTableAesSedai")},
	{TEXT("Forsaken"), TEXT("WoTPawns.AnimationTableForsaken")},
	{TEXT("Sister"), TEXT("WoTPawns.AnimationTableSister")},
	{TEXT("BlackAjahBoss"), TEXT("WoTPawns.AnimationTableBlackAjahBoss")},
	{TEXT("Legion"), TEXT("WoTPawns.AnimationTableLegion")},
	{TEXT("MashFocusPawn"), TEXT("WoTPawns.AnimationTableMashFocusPawn")},
};

constexpr uint32 GWoTAnimListSize = 50;

bool WoTAnimReadName(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, FName& Out)
{
	const FWoTRuntimeWorldProperty* Member =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	if (Member == nullptr || Member->Type != EWoTRuntimeWorldPropertyType::Name)
	{
		return false;
	}
	Out = FName(*Member->NameValue);
	return true;
}
bool WoTAnimReadFloat(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, float& Out)
{
	const FWoTRuntimeWorldProperty* Member =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	return Member != nullptr && WoTRuntimeWorldPropertyFloat(*Member, Out);
}
bool WoTAnimReadByte(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, uint8& Out)
{
	const FWoTRuntimeWorldProperty* Member =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	return Member != nullptr && WoTRuntimeWorldPropertyByte(*Member, Out);
}

bool WoTAnimReadVector(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, FVector& Out)
{
	const FWoTRuntimeWorldProperty* Vector =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	if (Vector == nullptr || Vector->Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Vector->StructName.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	float X = 0.0f;
	float Y = 0.0f;
	float Z = 0.0f;
	if (!WoTAnimReadFloat(*Vector, TEXT("X"), X)
		|| !WoTAnimReadFloat(*Vector, TEXT("Y"), Y)
		|| !WoTAnimReadFloat(*Vector, TEXT("Z"), Z))
	{
		return false;
	}
	Out = FVector(X, Y, Z);
	return true;
}

bool WoTAnimDecodeEntry(const FWoTRuntimeWorldProperty& Property,
	uint32 ArrayIndex, FWoTAnimTableEntry& Out, FString& OutMember)
{
	// Property/member spellings: AnimationTableImpl.uc:12 and
	// AnimationTableInterf.uc:48-65.
	if (Property.Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property.StructName.Equals(TEXT("AnimInfoT"), ESearchCase::IgnoreCase))
	{
		OutMember = TEXT("AnimList");
		return false;
	}
	uint8 GameType = 0;
	if (!WoTAnimReadName(Property, TEXT("AnimSlot"), Out.Slot))
	{
		OutMember = TEXT("AnimSlot"); return false;
	}
	if (!WoTAnimReadFloat(Property, TEXT("AnimOdds"), Out.Odds))
	{
		OutMember = TEXT("AnimOdds"); return false;
	}
	if (!WoTAnimReadByte(Property, TEXT("GameType"), GameType))
	{
		OutMember = TEXT("GameType"); return false;
	}
	if (!WoTAnimReadVector(Property, TEXT("AnimEndVector"), Out.EndVector))
	{
		OutMember = TEXT("AnimEndVector"); return false;
	}
	const FWoTRuntimeWorldProperty* EngineInfo =
		WoTRuntimeWorldStructMember(Property, TEXT("EAnimInfo"));
	if (EngineInfo == nullptr
		|| EngineInfo->Type != EWoTRuntimeWorldPropertyType::Struct
		|| !EngineInfo->StructName.Equals(
			TEXT("EngineAnimInfoT"), ESearchCase::IgnoreCase))
	{
		OutMember = TEXT("EAnimInfo"); return false;
	}
	if (!WoTAnimReadName(*EngineInfo, TEXT("AnimName"), Out.AnimName))
	{
		OutMember = TEXT("EAnimInfo.AnimName"); return false;
	}
	if (!WoTAnimReadFloat(*EngineInfo, TEXT("AnimRate"), Out.Rate))
	{
		OutMember = TEXT("EAnimInfo.AnimRate"); return false;
	}
	if (!WoTAnimReadFloat(
		*EngineInfo, TEXT("AnimTweenToTime"), Out.TweenTime))
	{
		OutMember = TEXT("EAnimInfo.AnimTweenToTime"); return false;
	}
	Out.Index = static_cast<int32>(ArrayIndex);
	return true;
}

void WoTAnimApplyPortOverrides(FWoTAnimSet& Table)
{
	// GameType is intentionally absent from FWoTAnimTableEntry because the shipped
	// validity hook always returns true.
	// The retained row cells require no asset-specific substitutions.
	(void)Table;
}

void WoTAnimAppendAscii(TArray<uint8>& Out, const ANSICHAR* Text)
{
	const int32 Length = FCStringAnsi::Strlen(Text);
	Out.Append(reinterpret_cast<const uint8*>(Text), Length);
}

void WoTAnimAppendInteger(TArray<uint8>& Out, int64 Value)
{
	const FString Text = FString::Printf(TEXT("%lld"),
		static_cast<long long>(Value));
	FTCHARToUTF8 Utf8(*Text);
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

void WoTAnimAppendString(TArray<uint8>& Out, const FString& Value)
{
	FTCHARToUTF8 Utf8(*WoTRetailBuilderCanonical::Fold(Value));
	WoTAnimAppendInteger(Out, Utf8.Length());
	WoTAnimAppendAscii(Out, ":");
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

uint32 WoTAnimFloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

void WoTAnimAppendFloat(TArray<uint8>& Out, float Value)
{
	constexpr ANSICHAR Digits[] = "0123456789abcdef";
	const uint32 Bits = WoTAnimFloatBits(Value);
	ANSICHAR Text[10] = {'0', 'x'};
	for (int32 Digit = 0; Digit < 8; ++Digit)
	{
		const uint32 Shift = static_cast<uint32>((7 - Digit) * 4);
		Text[Digit + 2] = Digits[(Bits >> Shift) & 0x0fu];
	}
	Out.Append(reinterpret_cast<const uint8*>(Text), UE_ARRAY_COUNT(Text));
}

void WoTAnimAppendName(TArray<uint8>& Out, FName Value)
{
	WoTAnimAppendString(Out, Value.ToString());
}

TArray<FName> WoTAnimSortedNames(
	const TMap<FName, TArray<FWoTAnimTableEntry>>& Tables)
{
	TArray<FName> Names;
	Tables.GetKeys(Names);
	Names.Sort(WoTRetailBuilderCanonical::NameLess);
	return Names;
}

bool WoTAnimSameFloat(float Left, float Right)
{
	return WoTAnimFloatBits(Left) == WoTAnimFloatBits(Right);
}

bool WoTAnimFailCell(FString& OutError, FName TableName, int32 Row,
	const TCHAR* Cell)
{
	OutError = FString::Printf(TEXT("%s[%d].%s differs"),
		*TableName.ToString(), Row, Cell);
	return false;
}
}

bool FWoTAnimTableBuilder::Build(
	const FWoTRetailBuildContext& Context, FWoTAnimSet& Out,
	FString& OutError)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	FWoTAnimSet Candidate;
	if (!Defaults.IsLoaded())
	{
		OutError = TEXT("animation tables require loaded class defaults");
		return false;
	}
	for (const FWoTAnimClassSpec& Spec : GWoTAnimClassSpecs)
	{
		TArray<FString> Chain;
		if (!Defaults.GetClassChain(Spec.ClassIdentity, Chain))
		{
			OutError = FString::Printf(TEXT("animation table class '%s' is missing"),
				Spec.ClassIdentity);
			return false;
		}
		TArray<FWoTAnimTableEntry> Entries;
		for (uint32 ArrayIndex = 0; ArrayIndex < GWoTAnimListSize; ++ArrayIndex)
		{
			FWoTRuntimeWorldProperty Property;
			if (!WoTRuntimeStructInheritance::Resolve(Defaults,
				Spec.ClassIdentity, TEXT("AnimList"), ArrayIndex, Property)) continue;
			FWoTAnimTableEntry Entry;
			FString Member;
			if (!WoTAnimDecodeEntry(Property, ArrayIndex, Entry, Member))
			{
				OutError = FString::Printf(
					TEXT("animation table '%s' AnimList[%u].%s is malformed"),
					Spec.TableName, ArrayIndex, *Member);
				return false;
			}
			Entries.Add(MoveTemp(Entry));
		}
		Candidate.Tables.Add(FName(Spec.TableName), MoveTemp(Entries));
	}
	WoTAnimApplyPortOverrides(Candidate);
	Out = MoveTemp(Candidate);
	OutError.Reset();
	return true;
}
void FWoTAnimTableBuilder::SerializeCanonical(
	const FWoTAnimSet& Table, TArray<uint8>& Out)
{
	Out.Reset();
	WoTAnimAppendAscii(Out, "wot-anim-tables-v1\n");
	const TArray<FName> Names = WoTAnimSortedNames(Table.Tables);
	WoTAnimAppendAscii(Out, "tables ");
	WoTAnimAppendInteger(Out, Names.Num());
	WoTAnimAppendAscii(Out, "\n");
	for (const FName TableName : Names)
	{
		const TArray<FWoTAnimTableEntry>& Entries =
			Table.Tables.FindChecked(TableName);
		WoTAnimAppendAscii(Out, "table ");
		WoTAnimAppendName(Out, TableName);
		WoTAnimAppendAscii(Out, " ");
		WoTAnimAppendInteger(Out, Entries.Num());
		WoTAnimAppendAscii(Out, "\n");
		for (const FWoTAnimTableEntry& Entry : Entries)
		{
			WoTAnimAppendAscii(Out, "entry ");
			WoTAnimAppendInteger(Out, Entry.Index);
			WoTAnimAppendAscii(Out, " "); WoTAnimAppendName(Out, Entry.Slot);
			WoTAnimAppendAscii(Out, " "); WoTAnimAppendFloat(Out, Entry.Odds);
			WoTAnimAppendAscii(Out, " "); WoTAnimAppendFloat(Out,
				static_cast<float>(Entry.EndVector.X));
			WoTAnimAppendAscii(Out, " "); WoTAnimAppendFloat(Out,
				static_cast<float>(Entry.EndVector.Y));
			WoTAnimAppendAscii(Out, " "); WoTAnimAppendFloat(Out,
				static_cast<float>(Entry.EndVector.Z));
			WoTAnimAppendAscii(Out, " "); WoTAnimAppendName(Out, Entry.AnimName);
			WoTAnimAppendAscii(Out, " "); WoTAnimAppendFloat(Out, Entry.Rate);
			WoTAnimAppendAscii(Out, " "); WoTAnimAppendFloat(Out, Entry.TweenTime);
			WoTAnimAppendAscii(Out, "\n");
		}
	}
}

FString FWoTAnimTableBuilder::CanonicalDigest(const FWoTAnimSet& Table)
{
	TArray<uint8> Bytes;
	SerializeCanonical(Table, Bytes);
	return Sha256(Bytes).ToHex();
}

bool FWoTAnimTableBuilder::AreEquivalent(const FWoTAnimSet& Built,
	const FWoTAnimSet& Expected, FString& OutError)
{
	TSet<FName> NameSet;
	for (const TPair<FName, TArray<FWoTAnimTableEntry>>& Pair : Built.Tables)
		NameSet.Add(Pair.Key);
	for (const TPair<FName, TArray<FWoTAnimTableEntry>>& Pair : Expected.Tables)
		NameSet.Add(Pair.Key);
	TArray<FName> Names = NameSet.Array();
	Names.Sort([](FName Left, FName Right)
	{
		return Left.ToString().Compare(Right.ToString(), ESearchCase::CaseSensitive) < 0;
	});
	for (const FName TableName : Names)
	{
		const TArray<FWoTAnimTableEntry>* BuiltEntries = Built.Tables.Find(TableName);
		const TArray<FWoTAnimTableEntry>* ExpectedEntries = Expected.Tables.Find(TableName);
		if (BuiltEntries == nullptr || ExpectedEntries == nullptr)
		{
			OutError = FString::Printf(TEXT("animation table '%s' is %s"),
				*TableName.ToString(), BuiltEntries == nullptr
					? TEXT("missing from built data") : TEXT("missing from expected data"));
			return false;
		}
		if (BuiltEntries->Num() != ExpectedEntries->Num())
		{
			OutError = FString::Printf(TEXT("%s row count differs: %d != %d"),
				*TableName.ToString(), BuiltEntries->Num(), ExpectedEntries->Num());
			return false;
		}
		for (int32 Row = 0; Row < BuiltEntries->Num(); ++Row)
		{
			const FWoTAnimTableEntry& Left = (*BuiltEntries)[Row];
			const FWoTAnimTableEntry& Right = (*ExpectedEntries)[Row];
			if (Left.Index != Right.Index)
				return WoTAnimFailCell(OutError, TableName, Row, TEXT("Index"));
			if (Left.Slot.ToString() != Right.Slot.ToString())
				return WoTAnimFailCell(OutError, TableName, Row, TEXT("Slot"));
			if (!WoTAnimSameFloat(Left.Odds, Right.Odds))
				return WoTAnimFailCell(OutError, TableName, Row, TEXT("Odds"));
			if (!WoTAnimSameFloat(static_cast<float>(Left.EndVector.X),
				static_cast<float>(Right.EndVector.X)))
				return WoTAnimFailCell(OutError, TableName, Row, TEXT("EndVector.X"));
			if (!WoTAnimSameFloat(static_cast<float>(Left.EndVector.Y),
				static_cast<float>(Right.EndVector.Y)))
				return WoTAnimFailCell(OutError, TableName, Row, TEXT("EndVector.Y"));
			if (!WoTAnimSameFloat(static_cast<float>(Left.EndVector.Z),
				static_cast<float>(Right.EndVector.Z)))
				return WoTAnimFailCell(OutError, TableName, Row, TEXT("EndVector.Z"));
			if (Left.AnimName.ToString() != Right.AnimName.ToString())
				return WoTAnimFailCell(OutError, TableName, Row, TEXT("AnimName"));
			if (!WoTAnimSameFloat(Left.Rate, Right.Rate))
				return WoTAnimFailCell(OutError, TableName, Row, TEXT("Rate"));
			if (!WoTAnimSameFloat(Left.TweenTime, Right.TweenTime))
				return WoTAnimFailCell(OutError, TableName, Row, TEXT("TweenTime"));
		}
	}
	OutError.Reset();
	return true;
}
