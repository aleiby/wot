#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTRetailFamiliesRestoreScope.h"
#include "../WoTAnimTable.h"
#include "../RetailData/WoTRetailBuildContext.h"
#include "../WoTDecal.h"
#include "../WoTRetailBuilderCanonical.h"
#include "../WoTSoundTable.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../Payload/WoTRuntimeWorldResource.h"

namespace
{
constexpr EAutomationTestFlags WoTRetailBuilderFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FWoTRetailBuildContext WoTRetailBuilderContext(
	const FWoTRuntimeClassDefaults& Defaults)
{
	return FWoTRetailBuildContext{
		Defaults,
		[](FStringView, FWoTRetailTextureFacts&) { return false; },
		[](FStringView, TArray<FWoTVertexMeshSequence>&) { return false; }};
}


bool WoTRetailBuilderExactBytes(FAutomationTestBase& Test, const TCHAR* Label,
	const TArray<uint8>& Actual, const ANSICHAR* Expected)
{
	const int32 ExpectedLength = FCStringAnsi::Strlen(Expected);
	return Test.TestEqual(FString(Label) + TEXT(" byte count"),
		Actual.Num(), ExpectedLength)
		&& Test.TestTrue(FString(Label) + TEXT(" exact bytes"),
			FMemory::Memcmp(Actual.GetData(), Expected, ExpectedLength) == 0);
}

constexpr const TCHAR* GWoTRetailBuilderAnimNames[] = {
	TEXT("Trolloc"), TEXT("TrollocSideArm"), TEXT("BATrolloc"),
	TEXT("Myrddraal"), TEXT("Hound"), TEXT("Minion"), TEXT("Soldier"),
	TEXT("Archer"), TEXT("Warder"), TEXT("Whitecloak"), TEXT("AesSedai"),
	TEXT("Forsaken"), TEXT("Sister"), TEXT("BlackAjahBoss"), TEXT("Legion"),
	TEXT("MashFocusPawn"),
};

constexpr const TCHAR* GWoTRetailBuilderSoundNames[] = {
	TEXT("AesSedai"), TEXT("AesSedaiBot"), TEXT("Archer"), TEXT("BATrolloc"),
	TEXT("BlackAjah"), TEXT("BlackAjahBoss"), TEXT("Forsaken"),
	TEXT("ForsakenBot"), TEXT("Hound"), TEXT("HoundBot"), TEXT("Legion"),
	TEXT("Mashadar"), TEXT("Mashadar2"), TEXT("Minion"), TEXT("Myrddraal"),
	TEXT("Questioner"), TEXT("Sister"), TEXT("Sitter"), TEXT("Soldier"),
	TEXT("Trolloc"), TEXT("Warder"), TEXT("Whitecloak"), TEXT("WhitecloakBot"),
};

constexpr const TCHAR* GWoTRetailBuilderTimerNames[] = {
	TEXT("Archer"), TEXT("BATrolloc"), TEXT("BlackAjah"),
	TEXT("BlackAjahBoss"), TEXT("Myrddraal"), TEXT("Questioner"),
	TEXT("Soldier"), TEXT("Trolloc"), TEXT("Warder"),
};

template <typename ValueType>
FWoTRuntimeWorldProperty WoTRetailBuilderValue(const TCHAR* Name,
	EWoTRuntimeWorldPropertyType Type, ValueType Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = Type;
	Property.Value.SetNumUninitialized(sizeof(Value));
	FMemory::Memcpy(Property.Value.GetData(), &Value, sizeof(Value));
	return Property;
}

FWoTRuntimeWorldProperty WoTRetailBuilderName(
	const TCHAR* Name, FName Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Name;
	Property.NameValue = Value.ToString();
	return Property;
}

FWoTRuntimeWorldProperty WoTRetailBuilderString(
	const TCHAR* Name, const FString& Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::String;
	FTCHARToUTF8 Utf8(*Value);
	uint64 Remaining = static_cast<uint64>(Utf8.Length()) + 1u;
	uint8 First = static_cast<uint8>(Remaining & 0x3fu);
	Remaining >>= 6;
	if (Remaining != 0) First |= 0x40u;
	Property.Value.Add(First);
	while (Remaining != 0)
	{
		uint8 Next = static_cast<uint8>(Remaining & 0x7fu);
		Remaining >>= 7;
		if (Remaining != 0) Next |= 0x80u;
		Property.Value.Add(Next);
	}
	Property.Value.Append(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Property.Value.Add(0);
	return Property;
}

FWoTRuntimeWorldProperty WoTRetailBuilderVector(const FVector& Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = TEXT("AnimEndVector");
	Property.Type = EWoTRuntimeWorldPropertyType::Struct;
	Property.StructName = TEXT("Vector");
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("X"), EWoTRuntimeWorldPropertyType::Float,
		static_cast<float>(Value.X)));
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("Y"), EWoTRuntimeWorldPropertyType::Float,
		static_cast<float>(Value.Y)));
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("Z"), EWoTRuntimeWorldPropertyType::Float,
		static_cast<float>(Value.Z)));
	return Property;
}

FWoTRuntimeWorldProperty WoTRetailBuilderAnimProperty(
	uint32 ArrayIndex, const FWoTAnimTableEntry& Entry)
{
	FWoTRuntimeWorldProperty EngineInfo;
	EngineInfo.Name = TEXT("EAnimInfo");
	EngineInfo.Type = EWoTRuntimeWorldPropertyType::Struct;
	EngineInfo.StructName = TEXT("EngineAnimInfoT");
	EngineInfo.StructMembers.Add(
		WoTRetailBuilderName(TEXT("AnimName"), Entry.AnimName));
	EngineInfo.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("AnimRate"), EWoTRuntimeWorldPropertyType::Float, Entry.Rate));
	EngineInfo.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("AnimTweenToTime"), EWoTRuntimeWorldPropertyType::Float,
		Entry.TweenTime));

	FWoTRuntimeWorldProperty Property;
	Property.Name = TEXT("AnimList");
	Property.Type = EWoTRuntimeWorldPropertyType::Struct;
	Property.ArrayIndex = ArrayIndex;
	Property.StructName = TEXT("AnimInfoT");
	Property.StructMembers.Add(WoTRetailBuilderName(TEXT("AnimSlot"), Entry.Slot));
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("AnimOdds"), EWoTRuntimeWorldPropertyType::Float, Entry.Odds));
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("GameType"), EWoTRuntimeWorldPropertyType::Byte, uint8(0)));
	Property.StructMembers.Add(WoTRetailBuilderVector(Entry.EndVector));
	Property.StructMembers.Add(MoveTemp(EngineInfo));
	return Property;
}

FWoTRuntimeWorldProperty WoTRetailBuilderSoundProperty(
	uint32 ArrayIndex, const FWoTSoundTableEntry& Entry)
{
	FString RetailSound = Entry.SoundSubPath.ToString();
	RetailSound.ReplaceCharInline(TEXT('/'), TEXT('.'));
	FWoTRuntimeWorldProperty EngineInfo;
	EngineInfo.Name = TEXT("ESoundInfo");
	EngineInfo.Type = EWoTRuntimeWorldPropertyType::Struct;
	EngineInfo.StructName = TEXT("EngineSoundInfoT");
	EngineInfo.StructMembers.Add(
		WoTRetailBuilderString(TEXT("SoundString"), RetailSound));
	EngineInfo.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("Slot"), EWoTRuntimeWorldPropertyType::Byte, uint8(0)));
	EngineInfo.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("Volume"), EWoTRuntimeWorldPropertyType::Float, Entry.Volume));
	EngineInfo.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("bNoOverride"), EWoTRuntimeWorldPropertyType::Boolean, uint8(0)));
	EngineInfo.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("Radius"), EWoTRuntimeWorldPropertyType::Float, Entry.Radius));
	EngineInfo.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("Pitch"), EWoTRuntimeWorldPropertyType::Float, Entry.Pitch));

	FWoTRuntimeWorldProperty Property;
	Property.Name = TEXT("SoundList");
	Property.Type = EWoTRuntimeWorldPropertyType::Struct;
	Property.ArrayIndex = ArrayIndex;
	Property.StructName = TEXT("SoundInfoT");
	Property.StructMembers.Add(WoTRetailBuilderName(TEXT("SoundSlot"), Entry.Slot));
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("SoundOdds"), EWoTRuntimeWorldPropertyType::Float, Entry.Odds));
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("GameType"), EWoTRuntimeWorldPropertyType::Byte, Entry.GameType));
	Property.StructMembers.Add(MoveTemp(EngineInfo));
	return Property;
}

FWoTRuntimeWorldProperty WoTRetailBuilderTimerProperty(
	uint32 ArrayIndex, const FWoTSoundSlotTimer& Timer)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = TEXT("SlotTimers");
	Property.Type = EWoTRuntimeWorldPropertyType::Struct;
	Property.ArrayIndex = ArrayIndex;
	Property.StructName = TEXT("SoundSlotTimerT");
	Property.StructMembers.Add(
		WoTRetailBuilderName(TEXT("SoundSlot"), Timer.Slot));
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("SlotOdds"), EWoTRuntimeWorldPropertyType::Float, Timer.Odds));
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("MinSecsToNextSound"), EWoTRuntimeWorldPropertyType::Float,
		Timer.MinSecs));
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("MaxSecsToNextSound"), EWoTRuntimeWorldPropertyType::Float,
		Timer.MaxSecs));
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("bReset"), EWoTRuntimeWorldPropertyType::Boolean,
		static_cast<uint8>(Timer.bReset)));
	Property.StructMembers.Add(WoTRetailBuilderValue(
		TEXT("NextSoundTime"), EWoTRuntimeWorldPropertyType::Float, 0.0f));
	return Property;
}

TArray<FWoTRuntimeClassDefaultRecord> WoTRetailBuilderEmptyAnimRecords()
{
	TArray<FWoTRuntimeClassDefaultRecord> Records;
	uint32 ExportIndex = 1;
	for (const TCHAR* Name : GWoTRetailBuilderAnimNames)
	{
		FWoTRuntimeClassDefaultRecord Record;
		Record.Identity = FString(TEXT("WoTPawns.AnimationTable")) + Name;
		Record.ExportIndex = ExportIndex++;
		Records.Add(MoveTemp(Record));
	}
	return Records;
}

TArray<FWoTRuntimeClassDefaultRecord> WoTRetailBuilderEmptySoundRecords()
{
	TArray<FWoTRuntimeClassDefaultRecord> Records;
	uint32 ExportIndex = 1;
	for (const TCHAR* Name : GWoTRetailBuilderSoundNames)
	{
		FWoTRuntimeClassDefaultRecord Record;
		Record.Identity = FString(TEXT("WoTPawns.SoundTable")) + Name;
		Record.ExportIndex = ExportIndex++;
		Records.Add(MoveTemp(Record));
	}
	for (const TCHAR* Name : GWoTRetailBuilderTimerNames)
	{
		FWoTRuntimeClassDefaultRecord Record;
		Record.Identity = FString(TEXT("WoTPawns.SoundSlotTimerList")) + Name;
		Record.ExportIndex = ExportIndex++;
		Records.Add(MoveTemp(Record));
	}
	return Records;
}

FWoTRuntimeClassDefaultRecord* WoTRetailBuilderFindRecord(
	TArray<FWoTRuntimeClassDefaultRecord>& Records, const FString& Identity)
{
	return Records.FindByPredicate([&Identity](const FWoTRuntimeClassDefaultRecord& Record)
	{
		return Record.Identity == Identity;
	});
}

FWoTAnimTableEntry WoTRetailBuilderAnimEntry(int32 Index,
	const TCHAR* Slot, const TCHAR* AnimName, float Odds = 1.0f)
{
	FWoTAnimTableEntry Entry;
	Entry.Index = Index;
	Entry.Slot = FName(Slot);
	Entry.Odds = Odds;
	Entry.EndVector = FVector(1.0, 2.0, 3.0);
	Entry.AnimName = FName(AnimName);
	Entry.Rate = 0.5f;
	Entry.TweenTime = 0.25f;
	return Entry;
}

FWoTSoundTableEntry WoTRetailBuilderSoundEntry(
	const TCHAR* Slot, const TCHAR* Path, float Odds = 1.0f)
{
	FWoTSoundTableEntry Entry;
	Entry.Slot = FName(Slot);
	Entry.Odds = Odds;
	Entry.SoundSubPath = FName(Path);
	Entry.Volume = 1.0f;
	Entry.Radius = 512.0f;
	Entry.Pitch = 1.0f;
	return Entry;
}

FWoTSoundSlotTimer WoTRetailBuilderTimer(const TCHAR* Slot, float Odds)
{
	FWoTSoundSlotTimer Timer;
	Timer.Slot = FName(Slot);
	Timer.Odds = Odds;
	Timer.MinSecs = 3.0f;
	Timer.MaxSecs = 7.0f;
	Timer.bReset = true;
	return Timer;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderChainResolutionSpec,
	"WoT.RetailBuilders.ChainResolution", WoTRetailBuilderFlags)

bool FWoTRetailBuilderChainResolutionSpec::RunTest(const FString&)
{
	TArray<FWoTRuntimeClassDefaultRecord> Records =
		WoTRetailBuilderEmptyAnimRecords();
	FWoTRuntimeClassDefaultRecord* Parent = WoTRetailBuilderFindRecord(
		Records, TEXT("WoTPawns.AnimationTableTrolloc"));
	FWoTRuntimeClassDefaultRecord* Child = WoTRetailBuilderFindRecord(
		Records, TEXT("WoTPawns.AnimationTableTrollocSideArm"));
	Parent->Properties.Add(WoTRetailBuilderAnimProperty(
		3, WoTRetailBuilderAnimEntry(3, TEXT("Death"), TEXT("ParentDeath"))));
	Parent->Properties.Add(WoTRetailBuilderAnimProperty(
		30, WoTRetailBuilderAnimEntry(30, TEXT("ProjectileAttack"), TEXT("ParentShot"))));
	Child->SuperclassIdentity = Parent->Identity;
	FWoTRuntimeWorldProperty ChildDelta = WoTRetailBuilderAnimProperty(
		30, WoTRetailBuilderAnimEntry(30, TEXT("IgnoredSlot"), TEXT("ChildShot")));
	ChildDelta.StructMembers.RemoveAll([](const FWoTRuntimeWorldProperty& Member)
	{
		return Member.Name == TEXT("AnimSlot") || Member.Name == TEXT("AnimOdds")
			|| Member.Name == TEXT("AnimEndVector");
	});
	FWoTRuntimeWorldProperty* ChildEngine = ChildDelta.StructMembers.FindByPredicate(
		[](const FWoTRuntimeWorldProperty& Member)
		{
			return Member.Name == TEXT("EAnimInfo");
		});
	ChildEngine->StructMembers.RemoveAll([](const FWoTRuntimeWorldProperty& Member)
	{
		return Member.Name == TEXT("AnimRate");
	});
	Child->Properties.Add(MoveTemp(ChildDelta));

	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("chain fixture loads"),
		Defaults.LoadFixture(MoveTemp(Records), Error)))
	{
		AddError(Error); return false;
	}
	FWoTAnimSet Built;
	if (!TestTrue(TEXT("animation family builds"),
		FWoTAnimTableBuilder::Build(
			WoTRetailBuilderContext(Defaults), Built, Error)))
	{
		AddError(Error); return false;
	}
	const TArray<FWoTAnimTableEntry>& SideArm =
		Built.Tables.FindChecked(TEXT("TrollocSideArm"));
	TestEqual(TEXT("inherited and overridden indices are retained"), SideArm.Num(), 2);
	TestEqual(TEXT("inherited index remains first"), SideArm[0].Index, 3);
	TestEqual(TEXT("parent value resolves"), SideArm[0].AnimName, FName(TEXT("ParentDeath")));
	TestEqual(TEXT("child-authored nested member wins"), SideArm[1].AnimName,
		FName(TEXT("ChildShot")));
	TestEqual(TEXT("omitted outer member remains inherited"), SideArm[1].Slot,
		FName(TEXT("ProjectileAttack")));
	TestEqual(TEXT("omitted nested member remains inherited"), SideArm[1].Rate,
		0.5f);
	TestEqual(TEXT("omitted nested struct remains inherited"),
		SideArm[1].EndVector, FVector(1.0, 2.0, 3.0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderStructArraysSpec,
	"WoT.RetailBuilders.StructArrays", WoTRetailBuilderFlags)

bool FWoTRetailBuilderStructArraysSpec::RunTest(const FString&)
{
	TArray<FWoTRuntimeClassDefaultRecord> Records =
		WoTRetailBuilderEmptySoundRecords();
	FWoTRuntimeClassDefaultRecord* Table = WoTRetailBuilderFindRecord(
		Records, TEXT("WoTPawns.SoundTableTrolloc"));
	Table->Properties.Add(WoTRetailBuilderSoundProperty(
		1, WoTRetailBuilderSoundEntry(TEXT("First"), TEXT("Pack/First"))));
	Table->Properties.Add(WoTRetailBuilderSoundProperty(
		7, WoTRetailBuilderSoundEntry(TEXT("Second"), TEXT("Pack/Second"))));
	FWoTRuntimeClassDefaultRecord* Timer = WoTRetailBuilderFindRecord(
		Records, TEXT("WoTPawns.SoundSlotTimerListTrolloc"));
	Timer->Properties.Add(WoTRetailBuilderTimerProperty(
		2, WoTRetailBuilderTimer(TEXT("First"), 0.25f)));
	Timer->Properties.Add(WoTRetailBuilderTimerProperty(
		9, WoTRetailBuilderTimer(TEXT("Second"), 0.5f)));

	FWoTRuntimeClassDefaults Defaults;
	FWoTSoundSet Built;
	FString Error;
	TestTrue(TEXT("struct-array fixture loads"),
		Defaults.LoadFixture(MoveTemp(Records), Error));
	if (!TestTrue(TEXT("sound family builds"),
		FWoTSoundTableBuilder::Build(
			WoTRetailBuilderContext(Defaults), Built, Error)))
	{
		AddError(Error); return false;
	}
	const TArray<FWoTSoundTableEntry>& Entries =
		Built.Tables.FindChecked(TEXT("Trolloc"));
	const TArray<FWoTSoundSlotTimer>& Timers =
		Built.TimerLists.FindChecked(TEXT("Trolloc"));
	TestEqual(TEXT("both sparse sound structs decode"), Entries.Num(), 2);
	TestEqual(TEXT("sound array scan is index ordered"), Entries[1].Slot,
		FName(TEXT("Second")));
	TestEqual(TEXT("both sparse timer structs decode"), Timers.Num(), 2);
	TestEqual(TEXT("timer nested scalar survives"), Timers[1].Odds, 0.5f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderSoundInheritanceSpec,
	"WoT.RetailBuilders.SoundInheritanceChildAndParent", WoTRetailBuilderFlags)

bool FWoTRetailBuilderSoundInheritanceSpec::RunTest(const FString&)
{
	TArray<FWoTRuntimeClassDefaultRecord> Records =
		WoTRetailBuilderEmptySoundRecords();
	FWoTRuntimeClassDefaultRecord* Parent = WoTRetailBuilderFindRecord(
		Records, TEXT("WoTPawns.SoundTableAesSedai"));
	FWoTRuntimeClassDefaultRecord* Child = WoTRetailBuilderFindRecord(
		Records, TEXT("WoTPawns.SoundTableAesSedaiBot"));
	FWoTSoundTableEntry ParentLow = WoTRetailBuilderSoundEntry(
		TEXT("BaseLow"), TEXT("Pack/BaseLow"), 0.25f);
	ParentLow.Volume = 0.625f;
	ParentLow.Pitch = 0.75f;
	Parent->Properties.Add(WoTRetailBuilderSoundProperty(0, ParentLow));
	Parent->Properties.Add(WoTRetailBuilderSoundProperty(
		40, WoTRetailBuilderSoundEntry(TEXT("BaseHigh"), TEXT("Pack/BaseHigh"))));
	Child->SuperclassIdentity = Parent->Identity;
	FWoTRuntimeWorldProperty Partial = WoTRetailBuilderSoundProperty(
		0, WoTRetailBuilderSoundEntry(TEXT("ChildOverride"), TEXT("Pack/Override")));
	Partial.StructMembers.RemoveAll([](const FWoTRuntimeWorldProperty& Member)
	{
		return Member.Name == TEXT("SoundOdds");
	});
	FWoTRuntimeWorldProperty* Engine = Partial.StructMembers.FindByPredicate(
		[](const FWoTRuntimeWorldProperty& Member)
		{
			return Member.Name == TEXT("ESoundInfo");
		});
	Engine->StructMembers.RemoveAll([](const FWoTRuntimeWorldProperty& Member)
	{
		return Member.Name == TEXT("Volume") || Member.Name == TEXT("Pitch");
	});
	Child->Properties.Add(MoveTemp(Partial));
	Child->Properties.Add(WoTRetailBuilderSoundProperty(
		48, WoTRetailBuilderSoundEntry(TEXT("Child"), TEXT("Pack/Child"))));
	FWoTRuntimeClassDefaultRecord* Sister = WoTRetailBuilderFindRecord(
		Records, TEXT("WoTPawns.SoundTableSister"));
	FWoTRuntimeClassDefaultRecord* Sitter = WoTRetailBuilderFindRecord(
		Records, TEXT("WoTPawns.SoundTableSitter"));
	Sister->Properties.Add(WoTRetailBuilderSoundProperty(
		0, WoTRetailBuilderSoundEntry(TEXT("Sister"), TEXT("Pack/Sister"))));
	Sitter->SuperclassIdentity = Sister->Identity;

	FWoTRuntimeClassDefaults Defaults;
	FWoTSoundSet Built;
	FString Error;
	TestTrue(TEXT("sparse inheritance fixture loads"),
		Defaults.LoadFixture(MoveTemp(Records), Error));
	if (!TestTrue(TEXT("sound family builds"),
		FWoTSoundTableBuilder::Build(
			WoTRetailBuilderContext(Defaults), Built, Error)))
	{
		AddError(Error); return false;
	}
	const TArray<FWoTSoundTableEntry>& Entries =
		Built.Tables.FindChecked(TEXT("AesSedaiBot"));
	TestEqual(TEXT("bot carries inherited and child-authored rows"),
		Entries.Num(), 3);
	TestEqual(TEXT("child outer member overrides"), Entries[0].Slot,
		FName(TEXT("ChildOverride")));
	TestEqual(TEXT("omitted outer member remains inherited"), Entries[0].Odds,
		0.25f);
	TestEqual(TEXT("omitted nested volume remains inherited"), Entries[0].Volume,
		0.625f);
	TestEqual(TEXT("omitted nested pitch remains inherited"), Entries[0].Pitch,
		0.75f);
	TestEqual(TEXT("child nested member overrides"), Entries[0].SoundSubPath,
		FName(TEXT("Pack/Override")));
	TestEqual(TEXT("high inherited sparse index survives"), Entries[1].Slot,
		FName(TEXT("BaseHigh")));
	TestEqual(TEXT("child-authored sparse index survives"), Entries[2].Slot,
		FName(TEXT("Child")));
	TestEqual(TEXT("Sitter carries its parent's SoundList"),
		Built.Tables.FindChecked(TEXT("Sitter")).Num(), 1);
	TestEqual(TEXT("Sitter inherited row is intact"),
		Built.Tables.FindChecked(TEXT("Sitter"))[0].Slot,
		FName(TEXT("Sister")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderMissingClassSpec,
	"WoT.RetailBuilders.MissingClass", WoTRetailBuilderFlags)

bool FWoTRetailBuilderMissingClassSpec::RunTest(const FString&)
{
	TArray<FWoTRuntimeClassDefaultRecord> Records =
		WoTRetailBuilderEmptyAnimRecords();
	Records.RemoveAt(0);
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	TestTrue(TEXT("incomplete fixture itself is valid"),
		Defaults.LoadFixture(MoveTemp(Records), Error));
	FWoTAnimSet Built;
	Built.Tables.Add(FName(TEXT("sentinel")), TArray<FWoTAnimTableEntry>());
	TestFalse(TEXT("a required class is rejected"),
		FWoTAnimTableBuilder::Build(
			WoTRetailBuilderContext(Defaults), Built, Error));
	TestTrue(TEXT("error identifies the missing class"), Error.Contains(TEXT("Trolloc")));
	TestTrue(TEXT("failed build leaves output untouched"),
		Built.Tables.Num() == 1 && Built.Tables.Contains(TEXT("sentinel")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderWrongTypeSpec,
	"WoT.RetailBuilders.WrongType", WoTRetailBuilderFlags)

bool FWoTRetailBuilderWrongTypeSpec::RunTest(const FString&)
{
	TArray<FWoTRuntimeClassDefaultRecord> Records =
		WoTRetailBuilderEmptyAnimRecords();
	FWoTRuntimeClassDefaultRecord* Record = WoTRetailBuilderFindRecord(
		Records, TEXT("WoTPawns.AnimationTableTrolloc"));
	FWoTRuntimeWorldProperty Wrong = WoTRetailBuilderValue(
		TEXT("AnimList"), EWoTRuntimeWorldPropertyType::Float, 1.0f);
	Wrong.ArrayIndex = 4;
	Record->Properties.Add(MoveTemp(Wrong));
	FWoTRuntimeClassDefaults Defaults;
	FWoTAnimSet Built;
	FString Error;
	TestTrue(TEXT("wrong-type fixture loads"),
		Defaults.LoadFixture(MoveTemp(Records), Error));
	TestFalse(TEXT("present wrong-type struct fails closed"),
		FWoTAnimTableBuilder::Build(
			WoTRetailBuilderContext(Defaults), Built, Error));
	TestTrue(TEXT("error identifies the exact static-array cell"),
		Error.Contains(TEXT("AnimList[4]")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderEmptyTableSpec,
	"WoT.RetailBuilders.EmptyTable", WoTRetailBuilderFlags)

bool FWoTRetailBuilderEmptyTableSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaults AnimDefaults;
	FWoTRuntimeClassDefaults SoundDefaults;
	FString Error;
	TestTrue(TEXT("empty animation fixture loads"), AnimDefaults.LoadFixture(
		WoTRetailBuilderEmptyAnimRecords(), Error));
	TestTrue(TEXT("empty sound fixture loads"), SoundDefaults.LoadFixture(
		WoTRetailBuilderEmptySoundRecords(), Error));
	FWoTAnimSet Anim;
	FWoTSoundSet Sound;
	TestTrue(TEXT("empty animation classes are valid"),
		FWoTAnimTableBuilder::Build(
			WoTRetailBuilderContext(AnimDefaults), Anim, Error));
	TestTrue(TEXT("empty sound classes are valid"),
		FWoTSoundTableBuilder::Build(
			WoTRetailBuilderContext(SoundDefaults), Sound, Error));
	TestTrue(TEXT("MashFocusPawn remains a present empty table"),
		Anim.Tables.Contains(TEXT("MashFocusPawn"))
		&& Anim.Tables.FindChecked(TEXT("MashFocusPawn")).IsEmpty());
	TestTrue(TEXT("Sitter remains a present empty table"),
		Sound.Tables.Contains(TEXT("Sitter"))
		&& Sound.Tables.FindChecked(TEXT("Sitter")).IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderOverrideLayerSpec,
	"WoT.RetailBuilders.OverrideLayer", WoTRetailBuilderFlags)

bool FWoTRetailBuilderOverrideLayerSpec::RunTest(const FString&)
{
	TArray<FWoTRuntimeClassDefaultRecord> Records =
		WoTRetailBuilderEmptySoundRecords();
	FWoTRuntimeClassDefaultRecord* Record = WoTRetailBuilderFindRecord(
		Records, TEXT("WoTPawns.SoundTableTrolloc"));
	Record->Properties.Add(WoTRetailBuilderSoundProperty(
		0, WoTRetailBuilderSoundEntry(
			TEXT("FixtureSlot"), TEXT("FixtureBank/FixtureSound"))));
	FWoTRuntimeClassDefaults Defaults;
	FWoTSoundSet Built;
	FString Error;
	TestTrue(TEXT("override fixture loads"),
		Defaults.LoadFixture(MoveTemp(Records), Error));
	TestTrue(TEXT("sound family builds"),
		FWoTSoundTableBuilder::Build(
			WoTRetailBuilderContext(Defaults), Built, Error));
	TestEqual(TEXT("PORT_NOTES sound identity transform runs after decode"),
		Built.Tables.FindChecked(TEXT("Trolloc"))[0].SoundSubPath,
		FName(TEXT("FixtureBank/FixtureSound")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderCanonicalStabilitySpec,
	"WoT.RetailBuilders.CanonicalStability", WoTRetailBuilderFlags)

bool FWoTRetailBuilderCanonicalStabilitySpec::RunTest(const FString&)
{
	TestEqual(TEXT("animation family activation key"),
		FString(FWoTAnimTableBuilder::FamilyName), FString(TEXT("anim-tables")));
	TestEqual(TEXT("sound family activation key"),
		FString(FWoTSoundTableBuilder::FamilyName), FString(TEXT("sound-tables")));
	FWoTAnimSet Left;
	FWoTAnimSet Right;
	const FWoTAnimTableEntry Alpha = WoTRetailBuilderAnimEntry(
		0, TEXT("ExactSlot"), TEXT("ExactAnim"));
	const FWoTAnimTableEntry Zulu = WoTRetailBuilderAnimEntry(
		7, TEXT("ZuluSlot"), TEXT("ZuluAnim"), 0.5f);
	Left.Tables.Add(TEXT("Zulu"), {Zulu});
	Left.Tables.Add(TEXT("Alpha"), {Alpha});
	Right.Tables.Add(TEXT("Alpha"), {Alpha});
	Right.Tables.Add(TEXT("Zulu"), {Zulu});
	TArray<uint8> LeftBytes;
	TArray<uint8> RightBytes;
	FWoTAnimTableBuilder::SerializeCanonical(Left, LeftBytes);
	FWoTAnimTableBuilder::SerializeCanonical(Right, RightBytes);
	TestTrue(TEXT("map insertion order cannot affect canonical bytes"),
		LeftBytes == RightBytes);
	TestTrue(TEXT("map insertion order cannot affect SHA-256"),
		FWoTAnimTableBuilder::CanonicalDigest(Left)
			== FWoTAnimTableBuilder::CanonicalDigest(Right));
	const FString AnimDigest = FWoTAnimTableBuilder::CanonicalDigest(Left);
	static constexpr ANSICHAR ExpectedAnim[] =
		"wot-anim-tables-v1\ntables 2\n"
		"table 5:alpha 1\n"
		"entry 0 9:exactslot 0x3f800000 0x3f800000 0x40000000 "
		"0x40400000 9:exactanim 0x3f000000 0x3e800000\n"
		"table 4:zulu 1\n"
		"entry 7 8:zuluslot 0x3f000000 0x3f800000 0x40000000 "
		"0x40400000 8:zuluanim 0x3f000000 0x3e800000\n";
	WoTRetailBuilderExactBytes(*this, TEXT("animation canonical"),
		LeftBytes, ExpectedAnim);
	TestEqual(TEXT("animation canonical SHA-256"), AnimDigest,
		FString(TEXT("1850ef33563a128f88a4e3b39f5cd07fc536fd805fa7faab921a2114f6469a21")));
	Right.Tables.FindChecked(TEXT("Alpha"))[0].Rate = 0.25f;
	TestTrue(TEXT("a cell change changes the digest"),
		FWoTAnimTableBuilder::CanonicalDigest(Left)
			!= FWoTAnimTableBuilder::CanonicalDigest(Right));

	FWoTSoundSet SoundLeft;
	FWoTSoundSet SoundRight;
	const FWoTSoundTableEntry SoundAlpha = WoTRetailBuilderSoundEntry(
		TEXT("ExactSoundSlot"), TEXT("ExactPack/ExactSound"));
	const FWoTSoundTableEntry SoundZulu = WoTRetailBuilderSoundEntry(
		TEXT("ZuluSoundSlot"), TEXT("ZuluPack/ZuluSound"), 0.5f);
	const FWoTSoundSlotTimer TimerAlpha =
		WoTRetailBuilderTimer(TEXT("ExactSoundSlot"), 0.25f);
	const FWoTSoundSlotTimer TimerZulu =
		WoTRetailBuilderTimer(TEXT("ZuluSoundSlot"), 0.75f);
	SoundLeft.Tables.Add(TEXT("Zulu"), {SoundZulu});
	SoundLeft.Tables.Add(TEXT("Alpha"), {SoundAlpha});
	SoundLeft.TimerLists.Add(TEXT("Zulu"), {TimerZulu});
	SoundLeft.TimerLists.Add(TEXT("Alpha"), {TimerAlpha});
	SoundRight.Tables.Add(TEXT("Alpha"), {SoundAlpha});
	SoundRight.Tables.Add(TEXT("Zulu"), {SoundZulu});
	SoundRight.TimerLists.Add(TEXT("Alpha"), {TimerAlpha});
	SoundRight.TimerLists.Add(TEXT("Zulu"), {TimerZulu});
	TArray<uint8> SoundLeftBytes;
	TArray<uint8> SoundRightBytes;
	FWoTSoundTableBuilder::SerializeCanonical(SoundLeft, SoundLeftBytes);
	FWoTSoundTableBuilder::SerializeCanonical(SoundRight, SoundRightBytes);
	TestTrue(TEXT("sound and timer map order cannot affect canonical bytes"),
		SoundLeftBytes == SoundRightBytes);
	TestTrue(TEXT("sound and timer map order cannot affect SHA-256"),
		FWoTSoundTableBuilder::CanonicalDigest(SoundLeft)
			== FWoTSoundTableBuilder::CanonicalDigest(SoundRight));
	const FString SoundDigest = FWoTSoundTableBuilder::CanonicalDigest(SoundLeft);
	static constexpr ANSICHAR ExpectedSound[] =
		"wot-sound-tables-v1\ntables 2\n"
		"table 5:alpha 1\n"
		"entry 14:exactsoundslot 0x3f800000 0 20:exactpack/exactsound "
		"0x3f800000 0x44000000 0x3f800000\n"
		"table 4:zulu 1\n"
		"entry 13:zulusoundslot 0x3f000000 0 18:zulupack/zulusound "
		"0x3f800000 0x44000000 0x3f800000\n"
		"timer-lists 2\ntimer-list 5:alpha 1\n"
		"timer 14:exactsoundslot 0x3e800000 0x40400000 0x40e00000 1\n"
		"timer-list 4:zulu 1\n"
		"timer 13:zulusoundslot 0x3f400000 0x40400000 0x40e00000 1\n";
	WoTRetailBuilderExactBytes(*this, TEXT("sound canonical"),
		SoundLeftBytes, ExpectedSound);
	TestEqual(TEXT("sound canonical SHA-256"), SoundDigest,
		FString(TEXT("5539e35ee56b2da52116630661cd34a566d6f0a1f41181c120e250affd39791b")));
	SoundRight.TimerLists.FindChecked(TEXT("Alpha"))[0].MaxSecs = 9.0f;
	TestTrue(TEXT("a timer cell change changes the sound-family digest"),
		FWoTSoundTableBuilder::CanonicalDigest(SoundLeft)
			!= FWoTSoundTableBuilder::CanonicalDigest(SoundRight));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderInstallResetInjectionSpec,
	"WoT.RetailBuilders.InstallResetInjection", WoTRetailBuilderFlags)

bool FWoTRetailBuilderInstallResetInjectionSpec::RunTest(const FString&)
{
	FWoTRetailFamiliesRestoreScope RestoreFamilies;
	FWoTAnimTableBuilder::Reset();
	FWoTSoundTableBuilder::Reset();
	TestNull(TEXT("animation is unavailable before install under final switch"),
		WoTAnimTable::Find(TEXT("Trolloc")));
	TestNull(TEXT("sound is unavailable before install under final switch"),
		WoTSoundTable::Find(TEXT("Trolloc")));
	TestNull(TEXT("timers are unavailable before install under final switch"),
		WoTSoundTable::FindTimers(TEXT("Trolloc")));

	TArray<FWoTRuntimeClassDefaultRecord> AnimRecords =
		WoTRetailBuilderEmptyAnimRecords();
	WoTRetailBuilderFindRecord(AnimRecords,
		TEXT("WoTPawns.AnimationTableTrolloc"))->Properties.Add(
			WoTRetailBuilderAnimProperty(7,
				WoTRetailBuilderAnimEntry(7, TEXT("Fixture"), TEXT("Injected"))));
	FWoTRuntimeClassDefaults AnimDefaults;
	FWoTAnimSet AnimFixture;
	FString Error;
	if (!TestTrue(TEXT("animation injection fixture loads"),
		AnimDefaults.LoadFixture(MoveTemp(AnimRecords), Error))
		|| !TestTrue(TEXT("animation injection fixture builds purely"),
			FWoTAnimTableBuilder::Build(
				WoTRetailBuilderContext(AnimDefaults), AnimFixture, Error)))
	{
		AddError(Error); return false;
	}
	TestNull(TEXT("Build does not publish the animation set"),
		WoTAnimTable::Find(TEXT("Trolloc")));
	FWoTAnimTableBuilder::Install(MoveTemp(AnimFixture));
	const TArray<FWoTAnimTableEntry>* InjectedAnim =
		WoTAnimTable::Find(TEXT("Trolloc"));
	TestNotNull(TEXT("Find reads the installed animation set"), InjectedAnim);
	if (InjectedAnim != nullptr)
	{
		TestEqual(TEXT("Install replaces the animation table"), InjectedAnim->Num(), 1);
		TestEqual(TEXT("installed animation row is intact"),
			(*InjectedAnim)[0].AnimName, FName(TEXT("Injected")));
	}

	TArray<FWoTRuntimeClassDefaultRecord> SoundRecords =
		WoTRetailBuilderEmptySoundRecords();
	WoTRetailBuilderFindRecord(SoundRecords,
		TEXT("WoTPawns.SoundTableTrolloc"))->Properties.Add(
			WoTRetailBuilderSoundProperty(7,
				WoTRetailBuilderSoundEntry(TEXT("Fixture"), TEXT("Pack/Injected"))));
	WoTRetailBuilderFindRecord(SoundRecords,
		TEXT("WoTPawns.SoundSlotTimerListTrolloc"))->Properties.Add(
			WoTRetailBuilderTimerProperty(7,
				WoTRetailBuilderTimer(TEXT("Fixture"), 0.5f)));
	FWoTRuntimeClassDefaults SoundDefaults;
	FWoTSoundSet SoundFixture;
	if (!TestTrue(TEXT("sound injection fixture loads"),
		SoundDefaults.LoadFixture(MoveTemp(SoundRecords), Error))
		|| !TestTrue(TEXT("sound injection fixture builds purely"),
			FWoTSoundTableBuilder::Build(
				WoTRetailBuilderContext(SoundDefaults), SoundFixture, Error)))
	{
		FWoTAnimTableBuilder::Reset();
		AddError(Error); return false;
	}
	TestNull(TEXT("Build does not publish the sound set"),
		WoTSoundTable::Find(TEXT("Trolloc")));
	FWoTSoundTableBuilder::Install(MoveTemp(SoundFixture));
	const TArray<FWoTSoundTableEntry>* InjectedSound =
		WoTSoundTable::Find(TEXT("Trolloc"));
	const TArray<FWoTSoundSlotTimer>* InjectedTimer =
		WoTSoundTable::FindTimers(TEXT("Trolloc"));
	TestNotNull(TEXT("Find reads the installed sound set"), InjectedSound);
	TestNotNull(TEXT("FindTimers reads the installed timer set"), InjectedTimer);
	if (InjectedSound != nullptr)
	{
		TestEqual(TEXT("Install replaces the sound table"), InjectedSound->Num(), 1);
		TestEqual(TEXT("installed sound row is intact"),
			(*InjectedSound)[0].SoundSubPath, FName(TEXT("Pack/Injected")));
	}
	if (InjectedTimer != nullptr)
	{
		TestEqual(TEXT("Install replaces the timer table"), InjectedTimer->Num(), 1);
		TestEqual(TEXT("installed timer row is intact"),
			(*InjectedTimer)[0].Odds, 0.5f);
	}

	FWoTAnimTableBuilder::Reset();
	FWoTSoundTableBuilder::Reset();
	TestNull(TEXT("animation Reset returns to unavailable"),
		WoTAnimTable::Find(TEXT("Trolloc")));
	TestNull(TEXT("sound Reset returns to unavailable"),
		WoTSoundTable::Find(TEXT("Trolloc")));
	TestNull(TEXT("timer Reset returns to unavailable"),
		WoTSoundTable::FindTimers(TEXT("Trolloc")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderCanonicalCaseFoldSpec,
	"WoT.RetailBuilders.CanonicalCaseFold", WoTRetailBuilderFlags)

bool FWoTRetailBuilderCanonicalCaseFoldSpec::RunTest(const FString&)
{
	// A packaged build has no WITH_CASE_PRESERVING_NAME, so FName::ToString()
	// returns whichever casing the process registered first. Editor FNames
	// keep each spelling, which lets this spec feed two spellings of the same
	// names and require identical canonical bytes, as a packaged build needs.
	auto Text = [](const TArray<uint8>& Bytes)
	{
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	};
	FString Mixed = TEXT("AZaz09_.:");
	Mixed.AppendChar(static_cast<TCHAR>(0xC4));
	FString Folded = TEXT("azaz09_.:");
	Folded.AppendChar(static_cast<TCHAR>(0xC4));
	// FString TestEqual/TestNotEqual ignore case, so case checks compare explicitly.
	TestTrue(TEXT("Fold maps A-Z only"), WoTRetailBuilderCanonical::Fold(Mixed)
		.Equals(Folded, ESearchCase::CaseSensitive));
#if WITH_CASE_PRESERVING_NAME
	TestFalse(TEXT("this build keeps per-spelling FName display text"),
		FName(TEXT("SOUNDTABLECASEFOLD")).ToString().Equals(
			FName(TEXT("SoundTableCaseFold")).ToString(), ESearchCase::CaseSensitive));
#endif

	auto SoundSet = [](const TCHAR* Table, const TCHAR* Slot, const TCHAR* Sound)
	{
		FWoTSoundSet Set;
		FWoTSoundTableEntry Entry;
		Entry.Slot = FName(Slot);
		Entry.Odds = 1.0f;
		Entry.SoundSubPath = FName(Sound);
		Set.Tables.Add(FName(Table)).Add(Entry);
		return Set;
	};
	const FWoTSoundSet UpperSound = SoundSet(
		TEXT("SOUNDTABLECASEFOLD"), TEXT("CASEFOLDHIT"), TEXT("CASEFOLDPACK/HIT1"));
	const FWoTSoundSet MixedSound = SoundSet(
		TEXT("SoundTableCaseFold"), TEXT("CaseFoldHit"), TEXT("CaseFoldPack/Hit1"));
	TestEqual(TEXT("sound-tables digest ignores name casing"),
		FWoTSoundTableBuilder::CanonicalDigest(UpperSound),
		FWoTSoundTableBuilder::CanonicalDigest(MixedSound));
	TArray<uint8> Bytes;
	FWoTSoundTableBuilder::SerializeCanonical(MixedSound, Bytes);
	TestTrue(TEXT("sound-tables canonical names are folded"),
		Text(Bytes).Contains(TEXT("soundtablecasefold"), ESearchCase::CaseSensitive)
		&& !Text(Bytes).Contains(TEXT("CaseFold"), ESearchCase::CaseSensitive));

	auto DecalSet = [](const TCHAR* First, const TCHAR* Second, const TCHAR* Texture)
	{
		FWoTDecalSet Set;
		for (const TCHAR* Name : {First, Second})
		{
			FWoTDecalConfig& Config = Set.Configs.Add(FName(Name));
			Config.Textures.Add(FName(Texture));
		}
		return Set;
	};
	const FWoTDecalSet UpperDecal = DecalSet(
		TEXT("CASEFOLDBETA"), TEXT("CASEFOLDALPHA"), TEXT("CASEFOLDSPLAT"));
	const FWoTDecalSet MixedDecal = DecalSet(
		TEXT("CaseFoldBeta"), TEXT("casefoldalpha"), TEXT("CaseFoldSplat"));
	TestEqual(TEXT("decal digest ignores name casing"),
		FWoTDecalBuilder::CanonicalDigest(UpperDecal),
		FWoTDecalBuilder::CanonicalDigest(MixedDecal));
	FWoTDecalBuilder::SerializeCanonical(MixedDecal, Bytes);
	const FString DecalText = Text(Bytes);
	const int32 Alpha = DecalText.Find(TEXT("casefoldalpha"), ESearchCase::CaseSensitive);
	const int32 Beta = DecalText.Find(TEXT("casefoldbeta"), ESearchCase::CaseSensitive);
	TestTrue(TEXT("canonical order compares folded names (alpha before Beta)"),
		Alpha != INDEX_NONE && Beta != INDEX_NONE && Alpha < Beta);
	return !HasAnyErrors();
}

#endif
