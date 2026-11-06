#include "WoTSoundTable.h"

#include "WoTRetailBuilderCanonical.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeStructInheritance.h"
#include "World/WoTRuntimeWorldProperties.h"

namespace
{
struct FWoTSoundClassSpec
{
	const TCHAR* TableName;
	const TCHAR* ClassIdentity;
};

// Retail identities: exports/script/WoTPawns/SoundTable*.uc:8 (Mashadar2:11).
constexpr FWoTSoundClassSpec GWoTSoundClassSpecs[] = {
	{TEXT("AesSedai"), TEXT("WoTPawns.SoundTableAesSedai")},
	{TEXT("AesSedaiBot"), TEXT("WoTPawns.SoundTableAesSedaiBot")},
	{TEXT("Archer"), TEXT("WoTPawns.SoundTableArcher")},
	{TEXT("BATrolloc"), TEXT("WoTPawns.SoundTableBATrolloc")},
	{TEXT("BlackAjah"), TEXT("WoTPawns.SoundTableBlackAjah")},
	{TEXT("BlackAjahBoss"), TEXT("WoTPawns.SoundTableBlackAjahBoss")},
	{TEXT("Forsaken"), TEXT("WoTPawns.SoundTableForsaken")},
	{TEXT("ForsakenBot"), TEXT("WoTPawns.SoundTableForsakenBot")},
	{TEXT("Hound"), TEXT("WoTPawns.SoundTableHound")},
	{TEXT("HoundBot"), TEXT("WoTPawns.SoundTableHoundBot")},
	{TEXT("Legion"), TEXT("WoTPawns.SoundTableLegion")},
	{TEXT("Mashadar"), TEXT("WoTPawns.SoundTableMashadar")},
	{TEXT("Mashadar2"), TEXT("WoTPawns.SoundTableMashadar2")},
	{TEXT("Minion"), TEXT("WoTPawns.SoundTableMinion")},
	{TEXT("Myrddraal"), TEXT("WoTPawns.SoundTableMyrddraal")},
	{TEXT("Questioner"), TEXT("WoTPawns.SoundTableQuestioner")},
	{TEXT("Sister"), TEXT("WoTPawns.SoundTableSister")},
	{TEXT("Sitter"), TEXT("WoTPawns.SoundTableSitter")},
	{TEXT("Soldier"), TEXT("WoTPawns.SoundTableSoldier")},
	{TEXT("Trolloc"), TEXT("WoTPawns.SoundTableTrolloc")},
	{TEXT("Warder"), TEXT("WoTPawns.SoundTableWarder")},
	{TEXT("Whitecloak"), TEXT("WoTPawns.SoundTableWhitecloak")},
	{TEXT("WhitecloakBot"), TEXT("WoTPawns.SoundTableWhitecloakBot")},
};

// Retail identities: exports/script/WoTPawns/SoundSlotTimerList*.uc:8.
constexpr FWoTSoundClassSpec GWoTSoundTimerClassSpecs[] = {
	{TEXT("Archer"), TEXT("WoTPawns.SoundSlotTimerListArcher")},
	{TEXT("BATrolloc"), TEXT("WoTPawns.SoundSlotTimerListBATrolloc")},
	{TEXT("BlackAjah"), TEXT("WoTPawns.SoundSlotTimerListBlackAjah")},
	{TEXT("BlackAjahBoss"), TEXT("WoTPawns.SoundSlotTimerListBlackAjahBoss")},
	{TEXT("Myrddraal"), TEXT("WoTPawns.SoundSlotTimerListMyrddraal")},
	{TEXT("Questioner"), TEXT("WoTPawns.SoundSlotTimerListQuestioner")},
	{TEXT("Soldier"), TEXT("WoTPawns.SoundSlotTimerListSoldier")},
	{TEXT("Trolloc"), TEXT("WoTPawns.SoundSlotTimerListTrolloc")},
	{TEXT("Warder"), TEXT("WoTPawns.SoundSlotTimerListWarder")},
};

constexpr uint32 GWoTSoundListSize = 50;
constexpr uint32 GWoTSoundTimerListSize = 10;

bool WoTSoundReadName(const FWoTRuntimeWorldProperty& Struct,
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

bool WoTSoundReadFloat(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, float& Out)
{
	const FWoTRuntimeWorldProperty* Member =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	return Member != nullptr && WoTRuntimeWorldPropertyFloat(*Member, Out);
}

bool WoTSoundReadByte(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, uint8& Out)
{
	const FWoTRuntimeWorldProperty* Member =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	return Member != nullptr && WoTRuntimeWorldPropertyByte(*Member, Out);
}

bool WoTSoundReadBool(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, bool& Out)
{
	const FWoTRuntimeWorldProperty* Member =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	return Member != nullptr && WoTRuntimeWorldPropertyBool(*Member, Out);
}

bool WoTSoundReadString(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, FString& Out)
{
	const FWoTRuntimeWorldProperty* Member =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	return Member != nullptr && WoTRuntimeWorldPropertyUE1String(*Member, Out);
}

bool WoTSoundDecodeEntry(const FWoTRuntimeWorldProperty& Property,
	FWoTSoundTableEntry& Out, FString& OutMember)
{
	// Property/member spellings: SoundTableImpl.uc:13 and
	// SoundTableInterf.uc:24-43.
	if (Property.Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property.StructName.Equals(TEXT("SoundInfoT"), ESearchCase::IgnoreCase))
	{
		OutMember = TEXT("SoundList"); return false;
	}
	if (!WoTSoundReadName(Property, TEXT("SoundSlot"), Out.Slot))
	{
		OutMember = TEXT("SoundSlot"); return false;
	}
	if (!WoTSoundReadFloat(Property, TEXT("SoundOdds"), Out.Odds))
	{
		OutMember = TEXT("SoundOdds"); return false;
	}
	if (!WoTSoundReadByte(Property, TEXT("GameType"), Out.GameType))
	{
		OutMember = TEXT("GameType"); return false;
	}
	const FWoTRuntimeWorldProperty* EngineInfo =
		WoTRuntimeWorldStructMember(Property, TEXT("ESoundInfo"));
	if (EngineInfo == nullptr
		|| EngineInfo->Type != EWoTRuntimeWorldPropertyType::Struct
		|| !EngineInfo->StructName.Equals(
			TEXT("EngineSoundInfoT"), ESearchCase::IgnoreCase))
	{
		OutMember = TEXT("ESoundInfo"); return false;
	}
	FString SoundString;
	uint8 SoundSlot = 0;
	bool bNoOverride = false;
	if (!WoTSoundReadString(*EngineInfo, TEXT("SoundString"), SoundString))
	{
		OutMember = TEXT("ESoundInfo.SoundString"); return false;
	}
	if (!WoTSoundReadByte(*EngineInfo, TEXT("Slot"), SoundSlot))
	{
		OutMember = TEXT("ESoundInfo.Slot"); return false;
	}
	if (!WoTSoundReadFloat(*EngineInfo, TEXT("Volume"), Out.Volume))
	{
		OutMember = TEXT("ESoundInfo.Volume"); return false;
	}
	if (!WoTSoundReadBool(*EngineInfo, TEXT("bNoOverride"), bNoOverride))
	{
		OutMember = TEXT("ESoundInfo.bNoOverride"); return false;
	}
	if (!WoTSoundReadFloat(*EngineInfo, TEXT("Radius"), Out.Radius))
	{
		OutMember = TEXT("ESoundInfo.Radius"); return false;
	}
	if (!WoTSoundReadFloat(*EngineInfo, TEXT("Pitch"), Out.Pitch))
	{
		OutMember = TEXT("ESoundInfo.Pitch"); return false;
	}
	(void)SoundSlot;
	(void)bNoOverride;
	Out.SoundSubPath = FName(*SoundString);
	return true;
}

bool WoTSoundDecodeTimer(const FWoTRuntimeWorldProperty& Property,
	FWoTSoundSlotTimer& Out, FString& OutMember)
{
	// Property/member spellings: SoundSlotTimerListImpl.uc:15 and
	// SoundSlotTimerListInterf.uc:45-53.
	if (Property.Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property.StructName.Equals(
			TEXT("SoundSlotTimerT"), ESearchCase::IgnoreCase))
	{
		OutMember = TEXT("SlotTimers"); return false;
	}
	float NextSoundTime = 0.0f;
	if (!WoTSoundReadName(Property, TEXT("SoundSlot"), Out.Slot))
	{
		OutMember = TEXT("SoundSlot"); return false;
	}
	if (!WoTSoundReadFloat(Property, TEXT("SlotOdds"), Out.Odds))
	{
		OutMember = TEXT("SlotOdds"); return false;
	}
	if (!WoTSoundReadFloat(
		Property, TEXT("MinSecsToNextSound"), Out.MinSecs))
	{
		OutMember = TEXT("MinSecsToNextSound"); return false;
	}
	if (!WoTSoundReadFloat(
		Property, TEXT("MaxSecsToNextSound"), Out.MaxSecs))
	{
		OutMember = TEXT("MaxSecsToNextSound"); return false;
	}
	if (!WoTSoundReadBool(Property, TEXT("bReset"), Out.bReset))
	{
		OutMember = TEXT("bReset"); return false;
	}
	if (!WoTSoundReadFloat(Property, TEXT("NextSoundTime"), NextSoundTime))
	{
		OutMember = TEXT("NextSoundTime"); return false;
	}
	(void)NextSoundTime;
	return true;
}

bool WoTSoundApplyPortOverrides(FWoTSoundSet& Table, FString& OutError)
{
	// Imported Package.Sound identities become deterministic subpaths below
	// /Game/WoT/Sounds. The engine channel
	// and bNoOverride cells have no UE5 row field, but were validated above.
	for (TPair<FName, TArray<FWoTSoundTableEntry>>& Pair : Table.Tables)
	{
		for (FWoTSoundTableEntry& Entry : Pair.Value)
		{
			FString Source = Entry.SoundSubPath.ToString();
			int32 Dot = INDEX_NONE;
			if (!Source.FindChar(TEXT('.'), Dot) || Dot <= 0 || Dot >= Source.Len() - 1
				|| Source.Find(TEXT("."), ESearchCase::CaseSensitive,
					ESearchDir::FromStart, Dot + 1) != INDEX_NONE
				|| Source.Contains(TEXT("/")))
			{
				OutError = FString::Printf(
					TEXT("sound table '%s' has invalid SoundString '%s'"),
					*Pair.Key.ToString(), *Source);
				return false;
			}
			Source[Dot] = TEXT('/');
			Entry.SoundSubPath = FName(*Source);
		}
	}
	return true;
}

void WoTSoundAppendAscii(TArray<uint8>& Out, const ANSICHAR* Text)
{
	const int32 Length = FCStringAnsi::Strlen(Text);
	Out.Append(reinterpret_cast<const uint8*>(Text), Length);
}

void WoTSoundAppendInteger(TArray<uint8>& Out, int64 Value)
{
	const FString Text = FString::Printf(TEXT("%lld"),
		static_cast<long long>(Value));
	FTCHARToUTF8 Utf8(*Text);
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

void WoTSoundAppendString(TArray<uint8>& Out, const FString& Value)
{
	FTCHARToUTF8 Utf8(*WoTRetailBuilderCanonical::Fold(Value));
	WoTSoundAppendInteger(Out, Utf8.Length());
	WoTSoundAppendAscii(Out, ":");
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

uint32 WoTSoundFloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

void WoTSoundAppendFloat(TArray<uint8>& Out, float Value)
{
	constexpr ANSICHAR Digits[] = "0123456789abcdef";
	const uint32 Bits = WoTSoundFloatBits(Value);
	ANSICHAR Text[10] = {'0', 'x'};
	for (int32 Digit = 0; Digit < 8; ++Digit)
	{
		const uint32 Shift = static_cast<uint32>((7 - Digit) * 4);
		Text[Digit + 2] = Digits[(Bits >> Shift) & 0x0fu];
	}
	Out.Append(reinterpret_cast<const uint8*>(Text), UE_ARRAY_COUNT(Text));
}

void WoTSoundAppendName(TArray<uint8>& Out, FName Value)
{
	WoTSoundAppendString(Out, Value.ToString());
}

template <typename RowType>
TArray<FName> WoTSoundSortedNames(const TMap<FName, TArray<RowType>>& Tables)
{
	TArray<FName> Names;
	Tables.GetKeys(Names);
	Names.Sort(WoTRetailBuilderCanonical::NameLess);
	return Names;
}

bool WoTSoundSameFloat(float Left, float Right)
{
	return WoTSoundFloatBits(Left) == WoTSoundFloatBits(Right);
}

bool WoTSoundFailCell(FString& OutError, const TCHAR* Family,
	FName TableName, int32 Row, const TCHAR* Cell)
{
	OutError = FString::Printf(TEXT("%s %s[%d].%s differs"), Family,
		*TableName.ToString(), Row, Cell);
	return false;
}
}

bool FWoTSoundTableBuilder::Build(
	const FWoTRetailBuildContext& Context, FWoTSoundSet& Out,
	FString& OutError)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	FWoTSoundSet Candidate;
	if (!Defaults.IsLoaded())
	{
		OutError = TEXT("sound tables require loaded class defaults");
		return false;
	}
	for (const FWoTSoundClassSpec& Spec : GWoTSoundClassSpecs)
	{
		TArray<FString> Chain;
		if (!Defaults.GetClassChain(Spec.ClassIdentity, Chain))
		{
			OutError = FString::Printf(TEXT("sound table class '%s' is missing"),
				Spec.ClassIdentity);
			return false;
		}
		TArray<FWoTSoundTableEntry> Entries;
		for (uint32 ArrayIndex = 0; ArrayIndex < GWoTSoundListSize; ++ArrayIndex)
		{
			FWoTRuntimeWorldProperty Property;
			if (!WoTRuntimeStructInheritance::Resolve(Defaults,
				Spec.ClassIdentity, TEXT("SoundList"), ArrayIndex, Property)) continue;
			FWoTSoundTableEntry Entry;
			FString Member;
			if (!WoTSoundDecodeEntry(Property, Entry, Member))
			{
				OutError = FString::Printf(
					TEXT("sound table '%s' SoundList[%u].%s is malformed"),
					Spec.TableName, ArrayIndex, *Member);
				return false;
			}
			Entries.Add(MoveTemp(Entry));
		}
		Candidate.Tables.Add(FName(Spec.TableName), MoveTemp(Entries));
	}
	for (const FWoTSoundClassSpec& Spec : GWoTSoundTimerClassSpecs)
	{
		TArray<FString> Chain;
		if (!Defaults.GetClassChain(Spec.ClassIdentity, Chain))
		{
			OutError = FString::Printf(TEXT("sound timer class '%s' is missing"),
				Spec.ClassIdentity);
			return false;
		}
		TArray<FWoTSoundSlotTimer> Timers;
		for (uint32 ArrayIndex = 0;
			ArrayIndex < GWoTSoundTimerListSize; ++ArrayIndex)
		{
			FWoTRuntimeWorldProperty Property;
			if (!WoTRuntimeStructInheritance::Resolve(Defaults,
				Spec.ClassIdentity, TEXT("SlotTimers"), ArrayIndex, Property)) continue;
			FWoTSoundSlotTimer Timer;
			FString Member;
			if (!WoTSoundDecodeTimer(Property, Timer, Member))
			{
				OutError = FString::Printf(
					TEXT("sound timer '%s' SlotTimers[%u].%s is malformed"),
					Spec.TableName, ArrayIndex, *Member);
				return false;
			}
			Timers.Add(MoveTemp(Timer));
		}
		Candidate.TimerLists.Add(FName(Spec.TableName), MoveTemp(Timers));
	}
	if (!WoTSoundApplyPortOverrides(Candidate, OutError)) return false;
	Out = MoveTemp(Candidate);
	OutError.Reset();
	return true;
}

void FWoTSoundTableBuilder::SerializeCanonical(
	const FWoTSoundSet& Table, TArray<uint8>& Out)
{
	Out.Reset();
	WoTSoundAppendAscii(Out, "wot-sound-tables-v1\n");
	const TArray<FName> TableNames = WoTSoundSortedNames(Table.Tables);
	WoTSoundAppendAscii(Out, "tables ");
	WoTSoundAppendInteger(Out, TableNames.Num());
	WoTSoundAppendAscii(Out, "\n");
	for (const FName TableName : TableNames)
	{
		const TArray<FWoTSoundTableEntry>& Entries =
			Table.Tables.FindChecked(TableName);
		WoTSoundAppendAscii(Out, "table "); WoTSoundAppendName(Out, TableName);
		WoTSoundAppendAscii(Out, " "); WoTSoundAppendInteger(Out, Entries.Num());
		WoTSoundAppendAscii(Out, "\n");
		for (const FWoTSoundTableEntry& Entry : Entries)
		{
			WoTSoundAppendAscii(Out, "entry "); WoTSoundAppendName(Out, Entry.Slot);
			WoTSoundAppendAscii(Out, " "); WoTSoundAppendFloat(Out, Entry.Odds);
			WoTSoundAppendAscii(Out, " "); WoTSoundAppendInteger(Out, Entry.GameType);
			WoTSoundAppendAscii(Out, " "); WoTSoundAppendName(Out, Entry.SoundSubPath);
			WoTSoundAppendAscii(Out, " "); WoTSoundAppendFloat(Out, Entry.Volume);
			WoTSoundAppendAscii(Out, " "); WoTSoundAppendFloat(Out, Entry.Radius);
			WoTSoundAppendAscii(Out, " "); WoTSoundAppendFloat(Out, Entry.Pitch);
			WoTSoundAppendAscii(Out, "\n");
		}
	}
	const TArray<FName> TimerNames = WoTSoundSortedNames(Table.TimerLists);
	WoTSoundAppendAscii(Out, "timer-lists ");
	WoTSoundAppendInteger(Out, TimerNames.Num());
	WoTSoundAppendAscii(Out, "\n");
	for (const FName TableName : TimerNames)
	{
		const TArray<FWoTSoundSlotTimer>& Timers =
			Table.TimerLists.FindChecked(TableName);
		WoTSoundAppendAscii(Out, "timer-list "); WoTSoundAppendName(Out, TableName);
		WoTSoundAppendAscii(Out, " "); WoTSoundAppendInteger(Out, Timers.Num());
		WoTSoundAppendAscii(Out, "\n");
		for (const FWoTSoundSlotTimer& Timer : Timers)
		{
			WoTSoundAppendAscii(Out, "timer "); WoTSoundAppendName(Out, Timer.Slot);
			WoTSoundAppendAscii(Out, " "); WoTSoundAppendFloat(Out, Timer.Odds);
			WoTSoundAppendAscii(Out, " "); WoTSoundAppendFloat(Out, Timer.MinSecs);
			WoTSoundAppendAscii(Out, " "); WoTSoundAppendFloat(Out, Timer.MaxSecs);
			WoTSoundAppendAscii(Out, Timer.bReset ? " 1\n" : " 0\n");
		}
	}
}

FString FWoTSoundTableBuilder::CanonicalDigest(const FWoTSoundSet& Table)
{
	TArray<uint8> Bytes;
	SerializeCanonical(Table, Bytes);
	return Sha256(Bytes).ToHex();
}

bool FWoTSoundTableBuilder::AreEquivalent(const FWoTSoundSet& Built,
	const FWoTSoundSet& Expected, FString& OutError)
{
	auto CompareEntries = [&OutError](FName TableName,
		const TArray<FWoTSoundTableEntry>& Left,
		const TArray<FWoTSoundTableEntry>& Right)
	{
		if (Left.Num() != Right.Num())
		{
			OutError = FString::Printf(TEXT("sound table %s row count differs: %d != %d"),
				*TableName.ToString(), Left.Num(), Right.Num());
			return false;
		}
		for (int32 Row = 0; Row < Left.Num(); ++Row)
		{
			if (Left[Row].Slot.ToString() != Right[Row].Slot.ToString())
				return WoTSoundFailCell(OutError, TEXT("sound table"), TableName, Row, TEXT("Slot"));
			if (!WoTSoundSameFloat(Left[Row].Odds, Right[Row].Odds))
				return WoTSoundFailCell(OutError, TEXT("sound table"), TableName, Row, TEXT("Odds"));
			if (Left[Row].GameType != Right[Row].GameType)
				return WoTSoundFailCell(OutError, TEXT("sound table"), TableName, Row, TEXT("GameType"));
			if (Left[Row].SoundSubPath.ToString() != Right[Row].SoundSubPath.ToString())
				return WoTSoundFailCell(OutError, TEXT("sound table"), TableName, Row, TEXT("SoundSubPath"));
			if (!WoTSoundSameFloat(Left[Row].Volume, Right[Row].Volume))
				return WoTSoundFailCell(OutError, TEXT("sound table"), TableName, Row, TEXT("Volume"));
			if (!WoTSoundSameFloat(Left[Row].Radius, Right[Row].Radius))
				return WoTSoundFailCell(OutError, TEXT("sound table"), TableName, Row, TEXT("Radius"));
			if (!WoTSoundSameFloat(Left[Row].Pitch, Right[Row].Pitch))
				return WoTSoundFailCell(OutError, TEXT("sound table"), TableName, Row, TEXT("Pitch"));
		}
		return true;
	};
	auto CompareTimers = [&OutError](FName TableName,
		const TArray<FWoTSoundSlotTimer>& Left,
		const TArray<FWoTSoundSlotTimer>& Right)
	{
		if (Left.Num() != Right.Num())
		{
			OutError = FString::Printf(TEXT("sound timer %s row count differs: %d != %d"),
				*TableName.ToString(), Left.Num(), Right.Num());
			return false;
		}
		for (int32 Row = 0; Row < Left.Num(); ++Row)
		{
			if (Left[Row].Slot.ToString() != Right[Row].Slot.ToString())
				return WoTSoundFailCell(OutError, TEXT("sound timer"), TableName, Row, TEXT("Slot"));
			if (!WoTSoundSameFloat(Left[Row].Odds, Right[Row].Odds))
				return WoTSoundFailCell(OutError, TEXT("sound timer"), TableName, Row, TEXT("Odds"));
			if (!WoTSoundSameFloat(Left[Row].MinSecs, Right[Row].MinSecs))
				return WoTSoundFailCell(OutError, TEXT("sound timer"), TableName, Row, TEXT("MinSecs"));
			if (!WoTSoundSameFloat(Left[Row].MaxSecs, Right[Row].MaxSecs))
				return WoTSoundFailCell(OutError, TEXT("sound timer"), TableName, Row, TEXT("MaxSecs"));
			if (Left[Row].bReset != Right[Row].bReset)
				return WoTSoundFailCell(OutError, TEXT("sound timer"), TableName, Row, TEXT("bReset"));
		}
		return true;
	};

	TSet<FName> EntryNameSet;
	for (const TPair<FName, TArray<FWoTSoundTableEntry>>& Pair : Built.Tables)
		EntryNameSet.Add(Pair.Key);
	for (const TPair<FName, TArray<FWoTSoundTableEntry>>& Pair : Expected.Tables)
		EntryNameSet.Add(Pair.Key);
	TArray<FName> EntryNames = EntryNameSet.Array();
	EntryNames.Sort([](FName Left, FName Right)
	{
		return Left.ToString().Compare(Right.ToString(), ESearchCase::CaseSensitive) < 0;
	});
	for (const FName TableName : EntryNames)
	{
		const TArray<FWoTSoundTableEntry>* Left = Built.Tables.Find(TableName);
		const TArray<FWoTSoundTableEntry>* Right = Expected.Tables.Find(TableName);
		if (Left == nullptr || Right == nullptr)
		{
			OutError = FString::Printf(TEXT("sound table '%s' is %s"),
				*TableName.ToString(), Left == nullptr
					? TEXT("missing from built data") : TEXT("missing from expected data"));
			return false;
		}
		if (!CompareEntries(TableName, *Left, *Right)) return false;
	}

	TSet<FName> TimerNameSet;
	for (const TPair<FName, TArray<FWoTSoundSlotTimer>>& Pair : Built.TimerLists)
		TimerNameSet.Add(Pair.Key);
	for (const TPair<FName, TArray<FWoTSoundSlotTimer>>& Pair : Expected.TimerLists)
		TimerNameSet.Add(Pair.Key);
	TArray<FName> TimerNames = TimerNameSet.Array();
	TimerNames.Sort([](FName Left, FName Right)
	{
		return Left.ToString().Compare(Right.ToString(), ESearchCase::CaseSensitive) < 0;
	});
	for (const FName TableName : TimerNames)
	{
		const TArray<FWoTSoundSlotTimer>* Left = Built.TimerLists.Find(TableName);
		const TArray<FWoTSoundSlotTimer>* Right = Expected.TimerLists.Find(TableName);
		if (Left == nullptr || Right == nullptr)
		{
			OutError = FString::Printf(TEXT("sound timer '%s' is %s"),
				*TableName.ToString(), Left == nullptr
					? TEXT("missing from built data") : TEXT("missing from expected data"));
			return false;
		}
		if (!CompareTimers(TableName, *Left, *Right)) return false;
	}
	OutError.Reset();
	return true;
}
