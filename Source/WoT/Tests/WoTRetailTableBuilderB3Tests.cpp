#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTRetailFamiliesRestoreScope.h"
#include "../AI/WoTCreatureData.h"
#include "../RetailData/WoTRetailBuildContext.h"
#include "../Traps/WoTFireWall.h"
#include "../WoTBloodFX.h"
#include "../WoTFootsteps.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeWorldProperties.h"

namespace
{
constexpr EAutomationTestFlags WoTRetailBuilderB3Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FWoTRetailBuildContext B3Context(
	const FWoTRuntimeClassDefaults& Defaults,
	TMap<FString, FWoTRetailTextureFacts> TextureFacts = {},
	TMap<FString, TArray<FWoTVertexMeshSequence>> MeshSequences = {})
{
	return FWoTRetailBuildContext{
		Defaults,
		[TextureFacts = MoveTemp(TextureFacts)](
			FStringView Identity, FWoTRetailTextureFacts& Out)
		{
			const FWoTRetailTextureFacts* Found =
				TextureFacts.Find(FString(Identity).ToLower());
			if (Found == nullptr) return false;
			Out = *Found;
			return true;
		},
		[MeshSequences = MoveTemp(MeshSequences)](
			FStringView Identity, TArray<FWoTVertexMeshSequence>& Out)
		{
			const TArray<FWoTVertexMeshSequence>* Found =
				MeshSequences.Find(FString(Identity).ToLower());
			if (Found == nullptr) return false;
			Out = *Found;
			return true;
		}};
}

FWoTRetailTextureFacts B3TextureFacts(const FWoTParticleTemplate& Template)
{
	return FWoTRetailTextureFacts{
		static_cast<uint32>(FMath::RoundToInt(Template.TexWidth)),
		static_cast<uint32>(FMath::RoundToInt(Template.TexHeight)),
		0};
}

template <typename ValueType>
FWoTRuntimeWorldProperty B3Value(const TCHAR* Name,
	EWoTRuntimeWorldPropertyType Type, ValueType Value, uint32 Index = 0)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = Type;
	Property.ArrayIndex = Index;
	Property.Value.SetNumUninitialized(sizeof(Value));
	FMemory::Memcpy(Property.Value.GetData(), &Value, sizeof(Value));
	return Property;
}

FWoTRuntimeWorldProperty B3Object(const TCHAR* Name, FName Identity,
	uint32 Index = 0)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Object;
	Property.ArrayIndex = Index;
	Property.ObjectIdentity = Identity.IsNone() ? FString() : Identity.ToString();
	return Property;
}

FWoTRuntimeWorldProperty B3Name(const TCHAR* Name, FName Value,
	uint32 Index = 0)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Name;
	Property.ArrayIndex = Index;
	Property.NameValue = Value.ToString();
	return Property;
}

FWoTRuntimeWorldProperty B3String(const TCHAR* Name, const FString& Value,
	uint32 Index = 0)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::String;
	Property.ArrayIndex = Index;
	FTCHARToUTF8 Utf8(*Value);
	uint64 Remaining = static_cast<uint64>(Utf8.Length()) + 1;
	uint8 First = static_cast<uint8>(Remaining & 0x3f);
	Remaining >>= 6;
	if (Remaining != 0) First |= 0x40;
	Property.Value.Add(First);
	while (Remaining != 0)
	{
		uint8 Next = static_cast<uint8>(Remaining & 0x7f);
		Remaining >>= 7;
		if (Remaining != 0) Next |= 0x80;
		Property.Value.Add(Next);
	}
	Property.Value.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Property.Value.Add(0);
	return Property;
}

FWoTRuntimeWorldProperty B3Vector(const TCHAR* Name, const FVector& Value)
{
	const FVector3f V(Value);
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Struct;
	Property.StructName = TEXT("Vector");
	Property.Value.SetNumUninitialized(12);
	FMemory::Memcpy(Property.Value.GetData(), &V, 12);
	return Property;
}

FWoTRuntimeWorldProperty B3Rotator(const TCHAR* Name, int32 Yaw)
{
	const int32 Values[] = {0, Yaw, 0};
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Struct;
	Property.StructName = TEXT("Rotator");
	Property.Value.Append(reinterpret_cast<const uint8*>(Values), sizeof(Values));
	return Property;
}

FWoTRuntimeWorldProperty B3Particle(const FWoTParticleTemplate& T, uint32 Index)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = TEXT("Templates");
	Property.Type = EWoTRuntimeWorldPropertyType::Struct;
	Property.ArrayIndex = Index;
	Property.StructName = TEXT("Particle");
	auto Float = [&Property](float Value)
	{
		Property.Value.Append(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
	};
	Float(T.LifeSpan); Float(T.Weight); Float(T.MaxInitialVelocity);
	Float(T.MinInitialVelocity); Float(T.MaxDrawScale); Float(T.MinDrawScale);
	Float(T.MaxScaleGlow); Float(T.MinScaleGlow); Property.Value.Add(T.GrowPhase);
	Float(T.MaxGrowRate); Float(T.MinGrowRate); Property.Value.Add(T.FadePhase);
	Float(T.MaxFadeRate); Float(T.MinFadeRate);
	return Property;
}

void B3AddRecord(TArray<FWoTRuntimeClassDefaultRecord>& Records,
	const TCHAR* Identity, const TCHAR* Parent = nullptr)
{
	FWoTRuntimeClassDefaultRecord& Record = Records.AddDefaulted_GetRef();
	Record.Identity = Identity;
	Record.ExportIndex = Records.Num();
	if (Parent != nullptr) Record.SuperclassIdentity = Parent;
}

FWoTRuntimeClassDefaultRecord& B3Record(
	TArray<FWoTRuntimeClassDefaultRecord>& Records, FStringView Identity)
{
	return *Records.FindByPredicate([Identity](const FWoTRuntimeClassDefaultRecord& Row)
	{
		return FStringView(Row.Identity).Equals(Identity, ESearchCase::CaseSensitive);
	});
}

void B3AddSprayer(FWoTRuntimeClassDefaultRecord& Record, float Spread,
	float Volume, const FVector& Gravity, float PrimeCount,
	const TArray<FWoTParticleTemplate>& Templates, const TArray<FName>& Textures)
{
	Record.Properties.Add(B3Value(TEXT("Spread"),
		EWoTRuntimeWorldPropertyType::Float, Spread));
	Record.Properties.Add(B3Value(TEXT("Volume"),
		EWoTRuntimeWorldPropertyType::Float, Volume));
	Record.Properties.Add(B3Vector(TEXT("Gravity"), Gravity));
	Record.Properties.Add(B3Value(TEXT("PrimeCount"),
		EWoTRuntimeWorldPropertyType::Float, PrimeCount));
	Record.Properties.Add(B3Value(TEXT("NumTemplates"),
		EWoTRuntimeWorldPropertyType::Byte, static_cast<uint8>(Templates.Num())));
	for (int32 Index = 0; Index < Templates.Num(); ++Index)
	{
		Record.Properties.Add(B3Particle(Templates[Index], Index));
		Record.Properties.Add(B3Object(TEXT("Particles"), Textures[Index], Index));
	}
}

bool B3BuildBlood(FWoTBloodSet& Out, FString& Error,
	bool bFindTextures = true)
{
	FWoTBloodSet Expected;
	const TCHAR* Names[] = {TEXT("BloodDamageRed1"), TEXT("BloodDamageBlack1"),
		TEXT("BloodBurstRed2"), TEXT("BloodBurstBlack2"), TEXT("BloodSprayRed"),
		TEXT("BloodSprayBlack"), TEXT("BloodSpray01")};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
	{
		FWoTBloodConfig& Config = Expected.Configs.Add(Names[Index]);
		Config.Spread = static_cast<float>(Index + 1);
		Config.Volume = static_cast<float>(Index + 2);
		Config.PrimeCount = static_cast<float>(Index);
		Config.Templates.AddDefaulted();
		Config.TextureNames.Add(FName(*FString::Printf(
			TEXT("Fixture.Blood.Texture%d"), Index)));
	}
	TArray<FWoTRuntimeClassDefaultRecord> Records;
	TMap<FString, FWoTRetailTextureFacts> TextureFacts;
	for (const TPair<FName, FWoTBloodConfig>& Pair : Expected.Configs)
	{
		const FString Identity = TEXT("ParticleSystems.") + Pair.Key.ToString();
		B3AddRecord(Records, *Identity);
		FWoTRuntimeClassDefaultRecord& Record = Records.Last();
		const FWoTBloodConfig& C = Pair.Value;
		B3AddSprayer(Record, C.Spread, C.Volume, C.Gravity, C.PrimeCount,
			C.Templates, C.TextureNames);
		for (int32 Index = 0; Index < C.TextureNames.Num(); ++Index)
		{
			TextureFacts.Add(C.TextureNames[Index].ToString().ToLower(),
				B3TextureFacts(C.Templates[Index]));
		}
		if (!C.DecalTypeName.IsNone())
			Record.Properties.Add(B3Object(TEXT("DecalType"), C.DecalTypeName));
		Record.Properties.Add(B3Value(TEXT("DecalPercent"),
			EWoTRuntimeWorldPropertyType::Float, C.DecalPercent));
	}
	FWoTRuntimeClassDefaults Defaults;
	return Defaults.LoadFixture(MoveTemp(Records), Error)
		&& FWoTBloodFXBuilder::Build(B3Context(Defaults,
			bFindTextures ? MoveTemp(TextureFacts)
				: TMap<FString, FWoTRetailTextureFacts>()), Out, Error);
}

bool B3BuildFire(FWoTFireWallSet& Out, FString& Error,
	bool bFindTextures = true)
{
	FWoTFireWallSet Expected;
	const TCHAR* Identities[] = {TEXT("WoTTraps.FireWallSprayer1"),
		TEXT("WoTTraps.FireWallSprayer2"), TEXT("WoTTraps.FireWallSprayer3")};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Identities); ++Index)
	{
		FWoTFireWallSprayer& Sprayer = Expected.Sprayers.AddDefaulted_GetRef();
		Sprayer.ClassIdentity = Identities[Index];
		Sprayer.Spread = static_cast<float>(Index + 1);
		Sprayer.Volume = static_cast<float>(Index + 2);
		Sprayer.MinVolume = static_cast<float>(Index + 3);
		Sprayer.VolumeScalePct = static_cast<float>(Index + 4);
		Sprayer.VisibilityRadius = static_cast<float>(Index + 5);
		Sprayer.VisibilityHeight = static_cast<float>(Index + 6);
		Sprayer.Offset = FVector(Index + 1, Index + 2, Index + 3);
		FWoTFireWallTemplate& Template = Sprayer.Templates.AddDefaulted_GetRef();
		Template.TextureIdentity = FName(*FString::Printf(
			TEXT("Fixture.Fire.Texture%d"), Index));
	}
	Expected.Defaults.Health = 101;
	Expected.Defaults.DamageAmount = 2.0f;
	Expected.Defaults.DamageInterval = 3.0f;
	Expected.Defaults.SlabWidth = 104;
	Expected.Defaults.SlabMesh = TEXT("Fixture.Fire.Mesh");
	Expected.Defaults.SlabDrawScale = 1.25f;
	Expected.Defaults.CollisionRadius = 6.0f;
	Expected.Defaults.CollisionHeight = 7.0f;
	Expected.Defaults.MaxSlabs = 108;
	Expected.Defaults.InventoryCount = 109;
	TArray<FWoTRuntimeClassDefaultRecord> Records;
	TMap<FString, FWoTRetailTextureFacts> TextureFacts;
	for (int32 Index = 0; Index < Expected.Sprayers.Num(); ++Index)
	{
		const FWoTFireWallSprayer& S = Expected.Sprayers[Index];
		B3AddRecord(Records, *S.ClassIdentity.ToString());
		TArray<FWoTParticleTemplate> Templates;
		TArray<FName> Textures;
		for (const FWoTFireWallTemplate& T : S.Templates)
		{
			Templates.Add(T.Template); Textures.Add(T.TextureIdentity);
			TextureFacts.Add(T.TextureIdentity.ToString().ToLower(),
				B3TextureFacts(T.Template));
		}
		B3AddSprayer(Records.Last(), S.Spread, S.Volume, S.Gravity, 0.0f,
			Templates, Textures);
		Records.Last().Properties.Add(B3Value(TEXT("MinVolume"),
			EWoTRuntimeWorldPropertyType::Float, S.MinVolume));
		Records.Last().Properties.Add(B3Value(TEXT("VolumeScalePct"),
			EWoTRuntimeWorldPropertyType::Float, S.VolumeScalePct));
		Records.Last().Properties.Add(B3Value(TEXT("VisibilityRadius"),
			EWoTRuntimeWorldPropertyType::Float, S.VisibilityRadius));
		Records.Last().Properties.Add(B3Value(TEXT("VisibilityHeight"),
			EWoTRuntimeWorldPropertyType::Float, S.VisibilityHeight));
		if (S.bModulateOnly) Records.Last().Properties.Add(B3Value(TEXT("Style"),
			EWoTRuntimeWorldPropertyType::Byte, uint8(4)));
	}
	B3AddRecord(Records, TEXT("WoTTraps.FireWallSlab"));
	FWoTRuntimeClassDefaultRecord& Slab = Records.Last();
	const FWoTFireWallDefaults& D = Expected.Defaults;
	Slab.Properties.Add(B3Value(TEXT("Health"),
		EWoTRuntimeWorldPropertyType::Integer, D.Health));
	Slab.Properties.Add(B3Value(TEXT("DamageAmount"),
		EWoTRuntimeWorldPropertyType::Float, D.DamageAmount));
	Slab.Properties.Add(B3Value(TEXT("SlabWidth"),
		EWoTRuntimeWorldPropertyType::Integer, D.SlabWidth));
	Slab.Properties.Add(B3Object(TEXT("Mesh"), D.SlabMesh));
	Slab.Properties.Add(B3Value(TEXT("DrawScale"),
		EWoTRuntimeWorldPropertyType::Float, D.SlabDrawScale));
	Slab.Properties.Add(B3Value(TEXT("CollisionRadius"),
		EWoTRuntimeWorldPropertyType::Float, D.CollisionRadius));
	Slab.Properties.Add(B3Value(TEXT("CollisionHeight"),
		EWoTRuntimeWorldPropertyType::Float, D.CollisionHeight));
	for (int32 Index = 0; Index < Expected.Sprayers.Num(); ++Index)
	{
		FWoTRuntimeWorldProperty Offset = B3Vector(
			*FString::Printf(TEXT("FlameSprayerOffset%d"), Index + 1),
			Expected.Sprayers[Index].Offset);
		Slab.Properties.Add(MoveTemp(Offset));
	}
	B3Record(Records, TEXT("WoTTraps.FireWallSprayer1")).Properties.Add(
		B3Value(TEXT("DamageInterval"), EWoTRuntimeWorldPropertyType::Float,
			D.DamageInterval));
	B3AddRecord(Records, TEXT("WoTTraps.FireWall"));
	Records.Last().Properties.Add(B3Value(TEXT("MaxSlabs"),
		EWoTRuntimeWorldPropertyType::Integer, D.MaxSlabs));
	B3AddRecord(Records, TEXT("WoTTraps.FireWallInventory"));
	Records.Last().Properties.Add(B3Value(TEXT("Count"),
		EWoTRuntimeWorldPropertyType::Integer, D.InventoryCount));
	FWoTRuntimeClassDefaults Defaults;
	return Defaults.LoadFixture(MoveTemp(Records), Error)
		&& FWoTFireWallBuilder::Build(B3Context(Defaults,
			bFindTextures ? MoveTemp(TextureFacts)
				: TMap<FString, FWoTRetailTextureFacts>()), Out, Error);
}

void B3AddMeleeDamageProperties(FWoTRuntimeClassDefaultRecord& Record,
	FName RowName, const TArray<FWoTMeleeAttack>& A)
{
	auto Add = [&Record](const TCHAR* Name, float Value)
	{
		Record.Properties.Add(B3Value(Name,
			EWoTRuntimeWorldPropertyType::Float, Value));
	};
	if (RowName == TEXT("Warder"))
	{
		Add(TEXT("Attack1Damage"), A[0].Damage);
		Add(TEXT("Attack2Damage"), A[2].Damage);
		Add(TEXT("Attack3Damage"), A[4].Damage);
	}
	else if (RowName == TEXT("Trolloc") || RowName == TEXT("TrollocStrong"))
	{
		Add(TEXT("AttackRunDamage"), A[0].Damage);
		Add(TEXT("AttackRunBDamage"), A[1].Damage);
	}
	else if (RowName == TEXT("Myrddraal") || RowName == TEXT("Archer"))
	{
		Add(TEXT("SwipeDamage"), A[0].Damage);
		Add(TEXT("SkewerDamage"), A[1].Damage);
	}
	else if (RowName == TEXT("BATrolloc"))
	{
		Add(TEXT("JabHalberdDamage"), A[0].Damage);
		Add(TEXT("JabHalberdLongDamage"), A[1].Damage);
		Add(TEXT("DbleHalberdDamage"), A[2].Damage);
		Add(TEXT("DblePunchDamage"), A[4].Damage);
		Add(TEXT("HeadTossDamage"), A[6].Damage);
	}
	else if (RowName == TEXT("Minion"))
	{
		Add(TEXT("AttackBiteDamage1"), A[0].Damage);
		Add(TEXT("AttackClawDamage1"), A[1].Damage);
		Add(TEXT("AttackRunClawDamage1"), A[3].Damage);
		Add(TEXT("AttackRunTentDamage1"), A[5].Damage);
		Add(TEXT("AttackTentDamage1"), A[6].Damage);
	}
	else if (RowName == TEXT("Soldier") || RowName == TEXT("Questioner"))
	{
		Add(TEXT("AttackRunShieldDamage"), A[0].Damage);
		Add(TEXT("AttackRunSwipeDamage"), A[1].Damage);
		Add(TEXT("AttackRunLungeDamage"), A[2].Damage);
	}
}

TArray<FString> B3MeleeFunctions(FName RowName)
{
	if (RowName == TEXT("Warder"))
	{
		return {TEXT("Attack1DamageTarget"), TEXT("Attack1DamageTarget"),
			TEXT("AttackSkewDamageTarget"), TEXT("AttackRipDamageTarget"),
			TEXT("Attack3DamageTarget"), TEXT("Attack3DamageTarget")};
	}
	if (RowName == TEXT("Trolloc") || RowName == TEXT("TrollocStrong"))
	{
		return {TEXT("AttackRunDamageTarget"), TEXT("AttackRunBDamageTarget")};
	}
	if (RowName == TEXT("Myrddraal"))
	{
		return {TEXT("SwipeDamageTarget"), TEXT("SkewerDamageTarget")};
	}
	if (RowName == TEXT("BATrolloc"))
	{
		return {TEXT("JabHalberdDamageTarget"),
			TEXT("JabHalberdLongDamageTarget"),
			TEXT("DbleHalberdDamageTarget"), TEXT("DbleHalberdDamageTarget"),
			TEXT("DblePunchDamageTarget"), TEXT("DblePunchDamageTarget"),
			TEXT("HeadTossDamageTarget")};
	}
	if (RowName == TEXT("Minion"))
	{
		return {TEXT("AttackBiteDamage"), TEXT("AttackClawDamage"),
			TEXT("AttackClawDamage"), TEXT("AttackRunClawDamage"),
			TEXT("AttackRunClawDamage"), TEXT("TentacleWhipAttackDamage"),
			TEXT("TentacleGrabAttackDamage1")};
	}
	if (RowName == TEXT("Soldier") || RowName == TEXT("Questioner"))
	{
		return {TEXT("AttackRunShieldDamageTarget"),
			TEXT("AttackRunSwipeDamageTarget"),
			TEXT("AttackRunLungeDamageTarget")};
	}
	if (RowName == TEXT("Archer"))
	{
		return {TEXT("SwipeDamageTarget"), TEXT("SkewerDamageTarget")};
	}
	return {};
}

TMap<FString, TArray<FWoTVertexMeshSequence>> B3CreatureMeshSequences(
	const FWoTCreatureSet& Expected)
{
	TMap<FString, TArray<FWoTVertexMeshSequence>> Result;
	for (const TPair<FName, FWoTCreatureRow>& Pair : Expected.Rows)
	{
		const TArray<FString> Functions = B3MeleeFunctions(Pair.Key);
		if (Functions.Num() != Pair.Value.MeleeAttacks.Num()) continue;
		TArray<FWoTVertexMeshSequence> Sequences;
		for (int32 Index = 0; Index < Pair.Value.MeleeAttacks.Num(); ++Index)
		{
			const FWoTMeleeAttack& Attack = Pair.Value.MeleeAttacks[Index];
			FWoTVertexMeshSequence& Sequence = Sequences.AddDefaulted_GetRef();
			Sequence.Name = Attack.AnimName.ToString();
			FWoTVertexMeshNotify& Notify = Sequence.Notifies.AddDefaulted_GetRef();
			Notify.Time = Attack.NotifyFraction;
			Notify.Function = Functions[Index];
		}
		Result.Add(Pair.Value.Mesh.ToString().ToLower(), MoveTemp(Sequences));
	}
	return Result;
}

bool B3BuildCreatures(FWoTCreatureSet& Out, FString& Error,
	bool bFindMeshes = true, int32 TracerSpinRollUnits = 65000,
	bool bMinionEyesEnabled = true, float MinionEyeTimeOffset = 0.0f,
	FName MinionEyesTexture = TEXT("Fixture.MinionEyes"))
{
	FWoTCreatureSet Expected;
	const TCHAR* Names[] = {TEXT("Warder"), TEXT("Sister"), TEXT("Sitter"),
		TEXT("Trolloc"), TEXT("TrollocStrong"), TEXT("Myrddraal"),
		TEXT("BATrolloc"), TEXT("Minion"), TEXT("MashadarFocusPawn"),
		TEXT("Legion"), TEXT("Soldier"), TEXT("Archer"), TEXT("Questioner")};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
	{
		FWoTCreatureRow& Row = Expected.Rows.Add(Names[Index]);
		Row.RowName = Names[Index];
		Row.Mesh = FName(*FString::Printf(TEXT("Fixture.Creature.Mesh%d"), Index));
		const int32 AttackCount = B3MeleeFunctions(Row.RowName).Num();
		for (int32 AttackIndex = 0; AttackIndex < AttackCount; ++AttackIndex)
		{
			FWoTMeleeAttack& Attack = Row.MeleeAttacks.AddDefaulted_GetRef();
			Attack.AnimName = FName(*FString::Printf(TEXT("FixtureAttack%d_%d"),
				Index, AttackIndex));
			Attack.Damage = static_cast<float>(AttackIndex + 1);
			Attack.NotifyFraction = static_cast<float>(AttackIndex + 1) / 10.0f;
		}
	}
	const TCHAR* Factions[] = {
		TEXT("AesSedai"), TEXT("AesSedai"), TEXT("AesSedai"),
		TEXT("Forsaken"), TEXT("Forsaken"), TEXT("Forsaken"),
		TEXT("Hound"), TEXT("Hound"), TEXT("Hound"),
		TEXT("Whitecloak"), TEXT("Whitecloak"), TEXT("Whitecloak")};
	const TCHAR* Tiers[] = {
		TEXT("Grunt"), TEXT("Captain"), TEXT("Champion"),
		TEXT("Grunt"), TEXT("Captain"), TEXT("Champion"),
		TEXT("Grunt"), TEXT("Captain"), TEXT("Champion"),
		TEXT("Grunt"), TEXT("Captain"), TEXT("Champion")};
	const TCHAR* Troops[] = {
		TEXT("Warder"), TEXT("Sister"), TEXT("Sitter"),
		TEXT("Trolloc"), TEXT("Myrddraal"), TEXT("BATrolloc"),
		TEXT("Minion"), TEXT("MashadarFocusPawn"), TEXT("Legion"),
		TEXT("Soldier"), TEXT("Archer"), TEXT("Questioner")};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Troops); ++Index)
	{
		FWoTPlayerTroopRow& Troop = Expected.PlayerTroops.AddDefaulted_GetRef();
		Troop.Faction = Factions[Index];
		Troop.Tier = Tiers[Index];
		Troop.RowName = Troops[Index];
	}
	Expected.Rows.FindChecked(TEXT("Warder")).bDefensiveDodge = true;
	FWoTCreatureRow& Minion = Expected.Rows.FindChecked(TEXT("Minion"));
	Minion.bMinionEyesEnabled = bMinionEyesEnabled;
	Minion.MinionEyeOffMin = 1.25f + MinionEyeTimeOffset;
	Minion.MinionEyeOffMax = 2.75f + MinionEyeTimeOffset;
	Minion.MinionEyeOnMin = 0.2f + MinionEyeTimeOffset;
	Minion.MinionEyeOnMax = 0.6f + MinionEyeTimeOffset;
	Minion.MinionEyesTexture = MinionEyesTexture;
	Expected.Rows.FindChecked(TEXT("TrollocStrong")).MaterialSkinIdentities.Add(
		2, TEXT("Fixture.TrollocStrongSkin"));
	Expected.Rows.FindChecked(TEXT("Sitter")).MaterialSkinIdentities.Add(
		5, TEXT("Fixture.SitterSkin"));
	Expected.TracerSpinAccelerationRollDegPerSec2 =
		TracerSpinRollUnits * (360.0f / 65536.0f);
	TMap<FString, TArray<FWoTVertexMeshSequence>> MeshSequences =
		B3CreatureMeshSequences(Expected);
	TArray<FWoTRuntimeClassDefaultRecord> Records;
	B3AddRecord(Records, TEXT("WOT.Captain"));
	B3AddRecord(Records, TEXT("WOT.Champion"), TEXT("WOT.Captain"));
	B3AddRecord(Records, TEXT("WoTPawns.ShieldUser"));
	B3AddRecord(Records, TEXT("Angreal.AngrealTracerProjectile"));
	{
		FWoTRuntimeWorldProperty Spin = B3Rotator(TEXT("SpinRate"), 0);
		const int32 Values[3] = {0, 0, TracerSpinRollUnits};
		Spin.Value.Reset();
		Spin.Value.Append(reinterpret_cast<const uint8*>(Values), sizeof(Values));
		Records.Last().Properties.Add(MoveTemp(Spin));
	}
	for (const TPair<FName, FWoTCreatureRow>& Pair : Expected.Rows)
	{
		const FWoTCreatureRow& R = Pair.Value;
		const TCHAR* Parent = R.bChampionOrders ? TEXT("WOT.Champion")
			: R.bShieldUser ? TEXT("WoTPawns.ShieldUser")
			: R.bIsCaptain ? TEXT("WOT.Captain") : nullptr;
		const FString Identity = FString(TEXT("WoTPawns.")) + Pair.Key.ToString();
		B3AddRecord(Records, *Identity, Parent);
		FWoTRuntimeClassDefaultRecord& Record = Records.Last();
#define WOT_B3_CREATURE_FLOAT(Member, PropertyName) \
		Record.Properties.Add(B3Value(TEXT(PropertyName), \
			EWoTRuntimeWorldPropertyType::Float, R.Member))
		WOT_B3_CREATURE_FLOAT(HealthSPMin, "HealthSPMin");
		WOT_B3_CREATURE_FLOAT(HealthSPMax, "HealthSPMax");
		WOT_B3_CREATURE_FLOAT(HealthMPMin, "HealthMPMin");
		WOT_B3_CREATURE_FLOAT(HealthMPMax, "HealthMPMax");
		Record.Properties.Add(B3Value(TEXT("Health"),
			EWoTRuntimeWorldPropertyType::Integer, R.HealthPlaced));
		WOT_B3_CREATURE_FLOAT(Mass, "Mass");
		WOT_B3_CREATURE_FLOAT(BrokenMoralePct, "BrokenMoralePct");
		WOT_B3_CREATURE_FLOAT(GibForSureFinalHealth, "GibForSureFinalHealth");
		WOT_B3_CREATURE_FLOAT(GibSometimesFinalHealth, "GibSometimesFinalHealth");
		WOT_B3_CREATURE_FLOAT(ChanceOfHitAnim, "ChanceOfHitAnim");
		WOT_B3_CREATURE_FLOAT(HitSoftHealthRatio, "HitSoftHealthRatio");
		WOT_B3_CREATURE_FLOAT(GroundSpeed, "GroundSpeed");
		WOT_B3_CREATURE_FLOAT(GroundSpeedMin, "GroundSpeedMin");
		WOT_B3_CREATURE_FLOAT(GroundSpeedMax, "GroundSpeedMax");
		WOT_B3_CREATURE_FLOAT(BaseWalkingSpeed, "BaseWalkingSpeed");
		WOT_B3_CREATURE_FLOAT(WaterSpeed, "WaterSpeed");
		Record.Properties.Add(B3Value(TEXT("GoalSuggestedSpeeds"),
			EWoTRuntimeWorldPropertyType::Float, R.ChaseSuggestedSpeed, 0));
		Record.Properties.Add(B3Value(TEXT("GoalSuggestedSpeeds"),
			EWoTRuntimeWorldPropertyType::Float, R.WaitSuggestedSpeed, 6));
		WOT_B3_CREATURE_FLOAT(MeleeRange, "MeleeRange");
		WOT_B3_CREATURE_FLOAT(MinOffensiveAngrealUsageInterval,
			"MinOffensiveAngrealUsageInterval");
		WOT_B3_CREATURE_FLOAT(MinDefensiveAngrealUsageInterval,
			"MinDefensiveAngrealUsageInterval");
		WOT_B3_CREATURE_FLOAT(DrawScale, "DrawScale");
		WOT_B3_CREATURE_FLOAT(ScaleGlow, "ScaleGlow");
		WOT_B3_CREATURE_FLOAT(CollisionRadius, "CollisionRadius");
		WOT_B3_CREATURE_FLOAT(CollisionHeight, "CollisionHeight");
		WOT_B3_CREATURE_FLOAT(BaseEyeHeight, "BaseEyeHeight");
		WOT_B3_CREATURE_FLOAT(FovAngle, "FovAngle");
#undef WOT_B3_CREATURE_FLOAT
#define WOT_B3_CREATURE_BOOL(Member, PropertyName) \
		Record.Properties.Add(B3Value(TEXT(PropertyName), \
			EWoTRuntimeWorldPropertyType::Boolean, static_cast<uint8>(R.Member)))
		WOT_B3_CREATURE_BOOL(bIsFemale, "bIsFemale");
		WOT_B3_CREATURE_BOOL(bBlockActors, "bBlockActors");
		WOT_B3_CREATURE_BOOL(bBlockPlayers, "bBlockPlayers");
		WOT_B3_CREATURE_BOOL(bProjTarget, "bProjTarget");
#undef WOT_B3_CREATURE_BOOL
		Record.Properties.Add(B3Value(TEXT("Visibility"),
			EWoTRuntimeWorldPropertyType::Byte, R.Visibility));
		Record.Properties.Add(B3Value(TEXT("SoundRadius"),
			EWoTRuntimeWorldPropertyType::Byte, R.AmbientSoundRadius));
		Record.Properties.Add(B3Value(TEXT("SoundVolume"),
			EWoTRuntimeWorldPropertyType::Byte, R.AmbientSoundVolume));
		Record.Properties.Add(B3Value(TEXT("SoundPitch"),
			EWoTRuntimeWorldPropertyType::Byte, R.AmbientSoundPitch));
		const TPair<const TCHAR*, FName> Objects[] = {
			{TEXT("DisguiseIcon"), R.DisguiseIcon},
			{TEXT("DefaultWeaponType"), R.DefaultWeaponType},
			{TEXT("MeleeWeaponType"), R.MeleeWeaponType},
			{TEXT("RangedWeaponType"), R.RangedWeaponType},
			{TEXT("HandlerFactoryClass"), R.HandlerFactoryClass},
			{TEXT("Mesh"), R.Mesh}, {TEXT("CarcassType"), R.CarcassType},
			{TEXT("SoundTableClass"), R.SoundTableClass},
		};
		for (const TPair<const TCHAR*, FName>& Object : Objects)
			Record.Properties.Add(B3Object(Object.Key, Object.Value));
		for (const TPair<int32, FName>& Skin : R.MaterialSkinIdentities)
			Record.Properties.Add(B3Object(TEXT("MultiSkins"), Skin.Value, Skin.Key));
		if (Pair.Key == TEXT("Minion"))
		{
			Record.Properties.Add(B3Value(TEXT("bEnableEyes"),
				EWoTRuntimeWorldPropertyType::Boolean,
				static_cast<uint8>(R.bMinionEyesEnabled)));
			Record.Properties.Add(B3Value(TEXT("MinEyeOffTime"),
				EWoTRuntimeWorldPropertyType::Float, R.MinionEyeOffMin));
			Record.Properties.Add(B3Value(TEXT("MaxEyeOffTime"),
				EWoTRuntimeWorldPropertyType::Float, R.MinionEyeOffMax));
			Record.Properties.Add(B3Value(TEXT("MinEyeOnTime"),
				EWoTRuntimeWorldPropertyType::Float, R.MinionEyeOnMin));
			Record.Properties.Add(B3Value(TEXT("MaxEyeOnTime"),
				EWoTRuntimeWorldPropertyType::Float, R.MinionEyeOnMax));
			Record.Properties.Add(B3Object(TEXT("EyesTexture"), R.MinionEyesTexture));
		}
		const FName AnimClass = R.AnimTable.IsNone() ? NAME_None
			: FName(*(FString(TEXT("WoTPawns.AnimationTable")) + R.AnimTable.ToString()));
		Record.Properties.Add(B3Object(TEXT("AnimationTableClass"), AnimClass));
		Record.Properties.Add(B3Object(TEXT("DamageHelperClass"),
			R.bBlackBlood ? FName(TEXT("WOT.DamageHelperBlackBlood")) : NAME_None));
		FString Ambient = R.AmbientSoundPath.ToString();
		Ambient.ReplaceInline(TEXT("/"), TEXT("."));
		Record.Properties.Add(B3Object(TEXT("AmbientSound"), FName(*Ambient)));
		if (R.RotationRateYaw != 0)
			Record.Properties.Add(B3Rotator(TEXT("RotationRate"), R.RotationRateYaw));
		if (R.InitialState == EWoTBrainState::Dormant)
			Record.Properties.Add(B3Name(TEXT("InitialState"), TEXT("Dormant")));
		B3AddMeleeDamageProperties(Record, Pair.Key, R.MeleeAttacks);
		for (int32 Index = 0; Index < R.DefaultInventoryTypes.Num(); ++Index)
			Record.Properties.Add(B3Object(TEXT("DefaultInventoryTypes"),
				R.DefaultInventoryTypes[Index], Index));
		for (int32 Index = 0; Index < R.Weaves.Num(); ++Index)
		{
			const FWoTCreatureWeave& W = R.Weaves[Index];
			if (Pair.Key == TEXT("Legion")) continue;
			Record.Properties.Add(B3String(TEXT("AII_AngrealInventoryStr"),
				W.SourcePath.ToString(), Index));
			Record.Properties.Add(B3Value(TEXT("AII_Priority"),
				EWoTRuntimeWorldPropertyType::Float, W.Priority, Index));
			Record.Properties.Add(B3Value(TEXT("AII_MinChargesInGroup"),
				EWoTRuntimeWorldPropertyType::Integer, W.MinChargesInGroup, Index));
			Record.Properties.Add(B3Value(TEXT("AII_MaxChargesInGroup"),
				EWoTRuntimeWorldPropertyType::Integer, W.MaxChargesInGroup, Index));
			Record.Properties.Add(B3Value(TEXT("AII_MinChargeGroupInterval"),
				EWoTRuntimeWorldPropertyType::Float, W.MinChargeGroupInterval, Index));
		}
		if (R.bDefensiveDodge)
		{
			const FString Detector = FString(TEXT("Fixture.Defensive")) + Pair.Key.ToString();
			Record.Properties.Add(B3Object(TEXT("DurationNotifierClasses"),
				FName(*Detector), 3));
			B3AddRecord(Records, *Detector);
			FWoTRuntimeClassDefaultRecord& D = Records.Last();
			D.Properties.Add(B3Value(TEXT("bActiveDetection"),
				EWoTRuntimeWorldPropertyType::Boolean, uint8(1)));
			D.Properties.Add(B3Value(TEXT("CollectionRadius"),
				EWoTRuntimeWorldPropertyType::Float, R.DodgeCollectionRadius));
			FWoTRuntimeWorldProperty Response;
			Response.Name = TEXT("DefensiveResponses"); Response.ArrayIndex = 0;
			Response.Type = EWoTRuntimeWorldPropertyType::Struct;
			Response.StructName = TEXT("TDefensiveResponse");
			Response.StructMembers.Add(B3Value(TEXT("DR_MinResponseTime"),
				EWoTRuntimeWorldPropertyType::Float, R.DodgeResponseMinTime));
			Response.StructMembers.Add(B3Value(TEXT("DR_MaxResponseTime"),
				EWoTRuntimeWorldPropertyType::Float, R.DodgeResponseMaxTime));
			Response.StructMembers.Add(B3Name(TEXT("DR_ResponsePreHint"),
				TEXT("AttemptDodge")));
			Response.StructMembers.Add(B3Name(TEXT("DR_ResponsePostHint"), NAME_None));
			D.Properties.Add(MoveTemp(Response));
		}
	}
	B3AddRecord(Records, TEXT("WoTPawns.LegionWeaponAdapter"));
	const FWoTCreatureRow& Legion = Expected.Rows.FindChecked(TEXT("Legion"));
	Records.Last().Properties.Add(B3Value(TEXT("MinOffensiveUsageInterval"),
		EWoTRuntimeWorldPropertyType::Float,
		Legion.MinOffensiveAngrealUsageInterval));
	Records.Last().Properties.Add(B3Value(TEXT("MinProjectileRange"),
		EWoTRuntimeWorldPropertyType::Float, Legion.AdapterMinProjectileRange));
	Records.Last().Properties.Add(B3Value(TEXT("MaxProjectileRange"),
		EWoTRuntimeWorldPropertyType::Float, Legion.AdapterMaxProjectileRange));
	Records.Last().Properties.Add(B3Value(TEXT("bRequireTargetVisibility"),
		EWoTRuntimeWorldPropertyType::Boolean,
		static_cast<uint8>(Legion.bAdapterRequireTargetVisibility)));
	B3AddRecord(Records, TEXT("WOT.WOTPlayer"));
	for (int32 Index = 0; Index < Expected.PlayerTroops.Num(); ++Index)
	{
		const FWoTPlayerTroopRow& T = Expected.PlayerTroops[Index];
		FWoTRuntimeWorldProperty Property;
		Property.Name = TEXT("PlayerTroops"); Property.ArrayIndex = Index;
		Property.Type = EWoTRuntimeWorldPropertyType::Struct;
		Property.StructName = TEXT("PlayerTroopMap");
		Property.StructMembers.Add(B3Name(TEXT("Player"), T.Faction));
		Property.StructMembers.Add(B3Name(TEXT("Troop"), T.Tier));
		Property.StructMembers.Add(B3String(TEXT("TroopClassName"),
			FString(TEXT("WoTPawns.")) + T.RowName.ToString()));
		Records.Last().Properties.Add(MoveTemp(Property));
	}
	FWoTRuntimeClassDefaults Defaults;
	return Defaults.LoadFixture(MoveTemp(Records), Error)
		&& FWoTCreatureDataBuilder::Build(B3Context(Defaults, {},
			bFindMeshes ? MoveTemp(MeshSequences)
				: TMap<FString, TArray<FWoTVertexMeshSequence>>()), Out, Error);
}

bool B3Exact(FAutomationTestBase& Test, const TCHAR* Label,
	const TArray<uint8>& Actual, const ANSICHAR* Expected)
{
	const int32 Count = FCStringAnsi::Strlen(Expected);
	return Test.TestEqual(FString(Label) + TEXT(" byte count"), Actual.Num(), Count)
		&& Test.TestTrue(FString(Label) + TEXT(" exact bytes"),
			FMemory::Memcmp(Actual.GetData(), Expected, Count) == 0);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB3CreaturePresentationMutationSpec,
	"WoT.RetailBuilders.B3.CreaturePresentationMutation",
	WoTRetailBuilderB3Flags)
bool FWoTRetailBuilderB3CreaturePresentationMutationSpec::RunTest(const FString&)
{
	FString Error;
	FWoTCreatureSet First;
	FWoTCreatureSet Mutated;
	if (!TestTrue(TEXT("first decoded creature fixture builds"),
		B3BuildCreatures(First, Error, true, 65000, true, 0.0f,
			TEXT("Fixture.MinionEyes"))))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("mutated decoded creature fixture builds"),
		B3BuildCreatures(Mutated, Error, true, 32000, false, 10.0f,
			TEXT("Fixture.MutatedMinionEyes"))))
	{
		AddError(Error);
		return false;
	}
	const FWoTCreatureRow& FirstMinion = First.Rows.FindChecked(TEXT("Minion"));
	const FWoTCreatureRow& MutatedMinion = Mutated.Rows.FindChecked(TEXT("Minion"));
	TestFalse(TEXT("Minion eye slot is not synthesized into decoded MultiSkins"),
		FirstMinion.MaterialSkinIdentities.Contains(4));
	TestTrue(TEXT("Minion eye enable follows decoded defaults"),
		FirstMinion.bMinionEyesEnabled);
	TestFalse(TEXT("mutating Minion eye enable changes the runtime row"),
		MutatedMinion.bMinionEyesEnabled);
	TestEqual(TEXT("Minion eye texture follows decoded defaults"),
		FirstMinion.MinionEyesTexture, FName(TEXT("Fixture.MinionEyes")));
	TestEqual(TEXT("mutating Minion eye texture changes the runtime row"),
		MutatedMinion.MinionEyesTexture,
		FName(TEXT("Fixture.MutatedMinionEyes")));
	TestEqual(TEXT("Minion eye-off minimum follows decoded defaults"),
		FirstMinion.MinionEyeOffMin, 1.25f);
	TestEqual(TEXT("Minion eye-off maximum follows decoded defaults"),
		FirstMinion.MinionEyeOffMax, 2.75f);
	TestEqual(TEXT("Minion eye-on minimum follows decoded defaults"),
		FirstMinion.MinionEyeOnMin, 0.2f);
	TestEqual(TEXT("Minion eye-on maximum follows decoded defaults"),
		FirstMinion.MinionEyeOnMax, 0.6f);
	TestEqual(TEXT("mutating Minion eye-off minimum changes the runtime row"),
		MutatedMinion.MinionEyeOffMin, 11.25f);
	TestEqual(TEXT("mutating Minion eye-off maximum changes the runtime row"),
		MutatedMinion.MinionEyeOffMax, 12.75f);
	TestEqual(TEXT("mutating Minion eye-on minimum changes the runtime row"),
		MutatedMinion.MinionEyeOnMin, 10.2f);
	TestEqual(TEXT("mutating Minion eye-on maximum changes the runtime row"),
		MutatedMinion.MinionEyeOnMax, 10.6f);
	TestEqual(TEXT("TrollocStrong material slot is retained from MultiSkins"),
		First.Rows.FindChecked(TEXT("TrollocStrong"))
			.MaterialSkinIdentities.FindRef(2),
		FName(TEXT("Fixture.TrollocStrongSkin")));
	TestEqual(TEXT("Sitter material slot is retained from MultiSkins"),
		First.Rows.FindChecked(TEXT("Sitter")).MaterialSkinIdentities.FindRef(5),
		FName(TEXT("Fixture.SitterSkin")));
	TestEqual(TEXT("Tracer converts decoded UE1 roll units to runtime degrees"),
		First.TracerSpinAccelerationRollDegPerSec2,
		65000.0f * 360.0f / 65536.0f, 0.001f);
	TestEqual(TEXT("mutating decoded Tracer SpinRate changes runtime acceleration"),
		Mutated.TracerSpinAccelerationRollDegPerSec2,
		32000.0f * 360.0f / 65536.0f, 0.001f);
	TestNotEqual(TEXT("decoded creature presentation mutation changes canonical digest"),
		FWoTCreatureDataBuilder::CanonicalDigest(First),
		FWoTCreatureDataBuilder::CanonicalDigest(Mutated));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB3FailureSpec,
	"WoT.RetailBuilders.B3.MissingClassWrongTypeTransactional", WoTRetailBuilderB3Flags)

bool FWoTRetailBuilderB3FailureSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaults Empty;
	FString Error;
	TArray<FWoTRuntimeClassDefaultRecord> Records;
	B3AddRecord(Records, TEXT("Fixture.Unrelated"));
	TestTrue(TEXT("unrelated fixture loads"), Empty.LoadFixture(MoveTemp(Records), Error));
	FWoTBloodSet Blood; Blood.Configs.Add(TEXT("sentinel"), FWoTBloodConfig());
	TestFalse(TEXT("blood missing class fails"),
		FWoTBloodFXBuilder::Build(B3Context(Empty), Blood, Error));
	TestTrue(TEXT("blood failure is transactional"), Blood.Configs.Contains(TEXT("sentinel")));
	FWoTFireWallSet Fire; Fire.Defaults.Health = 123;
	TestFalse(TEXT("fire missing class fails"),
		FWoTFireWallBuilder::Build(B3Context(Empty), Fire, Error));
	TestEqual(TEXT("fire failure is transactional"), Fire.Defaults.Health, 123);
	FWoTFootstepSet Foot; Foot.Tables.Add(TEXT("sentinel"));
	TestFalse(TEXT("footstep missing class fails"),
		FWoTFootstepsBuilder::Build(B3Context(Empty), Foot, Error));
	TestTrue(TEXT("footstep failure is transactional"), Foot.Tables.Contains(TEXT("sentinel")));
	FWoTCreatureSet Creatures; Creatures.Rows.Add(TEXT("sentinel"), FWoTCreatureRow());
	TestFalse(TEXT("creature missing class fails"),
		FWoTCreatureDataBuilder::Build(B3Context(Empty), Creatures, Error));
	TestTrue(TEXT("creature failure is transactional"),
		Creatures.Rows.Contains(TEXT("sentinel")));

	FWoTBloodSet UnknownBlood;
	UnknownBlood.Configs.Add(TEXT("sentinel"), FWoTBloodConfig());
	TestFalse(TEXT("an unknown blood texture fails the build"),
		B3BuildBlood(UnknownBlood, Error, false));
	TestTrue(TEXT("unknown blood texture failure is transactional"),
		UnknownBlood.Configs.Contains(TEXT("sentinel")));
	FWoTFireWallSet UnknownFire;
	UnknownFire.Defaults.Health = 321;
	TestFalse(TEXT("an unknown fire-wall texture fails the build"),
		B3BuildFire(UnknownFire, Error, false));
	TestEqual(TEXT("unknown fire-wall texture failure is transactional"),
		UnknownFire.Defaults.Health, 321);
	FWoTCreatureSet UnknownMesh;
	UnknownMesh.Rows.Add(TEXT("sentinel"), FWoTCreatureRow());
	TestFalse(TEXT("an unknown creature mesh fails the build"),
		B3BuildCreatures(UnknownMesh, Error, false));
	TestTrue(TEXT("unknown creature mesh failure is transactional"),
		UnknownMesh.Rows.Contains(TEXT("sentinel")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB3CanonicalSpec,
	"WoT.RetailBuilders.B3.CanonicalPins", WoTRetailBuilderB3Flags)

bool FWoTRetailBuilderB3CanonicalSpec::RunTest(const FString&)
{
	FWoTCreatureSet Creature;
	FWoTBloodSet Blood;
	FWoTFireWallSet Fire;
	FWoTFootstepSet Foot;
	TArray<uint8> Bytes;
	FWoTCreatureDataBuilder::SerializeCanonical(Creature, Bytes);
	B3Exact(*this, TEXT("creature canonical"), Bytes,
		"wot-creature-data-v3\nmovement-patterns 0\ntracer-spin 0x00000000\nrows 0\ntroops 0\n");
	TestEqual(TEXT("creature SHA-256"),
		FWoTCreatureDataBuilder::CanonicalDigest(Creature),
		FString(TEXT("b5ca657df70dce597c7e81439a7ca9bdc229d6527ada49cb00e752c98fce4b20")));
	FWoTBloodFXBuilder::SerializeCanonical(Blood, Bytes);
	B3Exact(*this, TEXT("blood canonical"), Bytes,
		"wot-blood-fx-v1\nconfigs 0\n");
	TestEqual(TEXT("blood SHA-256"), FWoTBloodFXBuilder::CanonicalDigest(Blood),
		FString(TEXT("c9faf52b326cfd99180be567e90f8df1536b2981daa2fd13e055f060a95bd6e3")));
	FWoTFireWallBuilder::SerializeCanonical(Fire, Bytes);
	B3Exact(*this, TEXT("fire canonical"), Bytes,
		"wot-fire-wall-v1\nsprayers 0\n"
		"defaults 0 0x00000000 0x00000000 0 4:none 0x3f800000 "
		"0x00000000 0x00000000 0 0\n");
	TestEqual(TEXT("fire SHA-256"), FWoTFireWallBuilder::CanonicalDigest(Fire),
		FString(TEXT("431404ae509717d33262bf8d018b8c28cf3583d95b04d642a7a36d6654672524")));
	FWoTFootstepsBuilder::SerializeCanonical(Foot, Bytes);
	B3Exact(*this, TEXT("foot canonical"), Bytes,
		"wot-footsteps-v1\ntables 0\nhelpers 0\nassignments 0\n");
	TestEqual(TEXT("foot SHA-256"), FWoTFootstepsBuilder::CanonicalDigest(Foot),
		FString(TEXT("bae099ef71ecd21cbc48d4ab55cf8251d54b9f0d7bcb658745f6badf629b1292")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB3InstallSpec,
	"WoT.RetailBuilders.B3.InstallResetInjection", WoTRetailBuilderB3Flags)

bool FWoTRetailBuilderB3InstallSpec::RunTest(const FString&)
{
	FWoTRetailFamiliesRestoreScope RestoreFamilies;
	FWoTCreatureDataBuilder::Reset(); FWoTBloodFXBuilder::Reset();
	FWoTFireWallBuilder::Reset(); FWoTFootstepsBuilder::Reset();
	TestNull(TEXT("creature unavailable before install"), WoTCreatureData::Find(TEXT("Warder")));
	TestNull(TEXT("blood unavailable before install"), WoTBloodFX::Find(TEXT("BloodDamageRed1")));
	TestEqual(TEXT("footsteps unavailable before install"),
		WoTFootsteps::Table(TEXT("Generic")).Num(), 0);
	FWoTCreatureSet Creatures; FWoTCreatureRow Creature; Creature.RowName = TEXT("Fixture");
	Creatures.Rows.Add(TEXT("Fixture"), Creature);
	FWoTCreatureDataBuilder::Install(MoveTemp(Creatures));
	TestNotNull(TEXT("installed creature visible"), WoTCreatureData::Find(TEXT("Fixture")));
	TestNull(TEXT("uninstalled creature absent after install"), WoTCreatureData::Find(TEXT("Warder")));
	FWoTBloodSet Blood; Blood.Configs.Add(TEXT("Fixture"), FWoTBloodConfig());
	FWoTBloodFXBuilder::Install(MoveTemp(Blood));
	TestNotNull(TEXT("installed blood visible"), WoTBloodFX::Find(TEXT("Fixture")));
	TestNull(TEXT("uninstalled blood absent after install"), WoTBloodFX::Find(TEXT("BloodDamageRed1")));
	FWoTFootstepSet Foot; Foot.Tables.Add(TEXT("Fixture"), {FWoTFootstepRow()});
	FWoTFootstepsBuilder::Install(MoveTemp(Foot));
	TestEqual(TEXT("installed footsteps visible"),
		WoTFootsteps::Table(TEXT("Fixture")).Num(), 1);
	TestEqual(TEXT("uninstalled footsteps absent after install"),
		WoTFootsteps::Table(TEXT("Generic")).Num(), 0);
	FWoTFireWallSet Fire;
	Fire.Defaults.Health = 777;
	Fire.Defaults.DamageAmount = 13.0f;
	Fire.Defaults.DamageInterval = 0.75f;
	Fire.Defaults.SlabWidth = 47;
	Fire.Defaults.MaxSlabs = 23;
	Fire.Defaults.InventoryCount = 7;
	FWoTFireWallBuilder::Install(MoveTemp(Fire));
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTFireWallSlab* Slab = World->SpawnActor<AWoTFireWallSlab>();
	AWoTFireWall* Wall = World->SpawnActor<AWoTFireWall>();
	AWoTFireWallInventory* Inventory = World->SpawnActor<AWoTFireWallInventory>();
	if (TestNotNull(TEXT("installed fire slab spawns"), Slab))
	{
		TestEqual(TEXT("installed slab health visible"), Slab->Health, 777);
		TestEqual(TEXT("installed slab damage visible"),
			static_cast<float>(Slab->DamageAmount), 13.0f);
		TestEqual(TEXT("installed slab interval visible"), Slab->DamageInterval, 0.75f);
		TestEqual(TEXT("installed slab width visible"), Slab->SlabWidth, 47);
	}
	if (TestNotNull(TEXT("installed fire wall spawns"), Wall))
	{
		TestEqual(TEXT("installed wall capacity visible"), Wall->MaxSlabs, 23);
	}
	if (TestNotNull(TEXT("installed fire inventory spawns"), Inventory))
	{
		TestEqual(TEXT("installed inventory count visible"), Inventory->Count, 7);
	}
	WoTTest::DestroySpecWorld(World);
	FWoTCreatureDataBuilder::Reset(); FWoTBloodFXBuilder::Reset();
	FWoTFireWallBuilder::Reset(); FWoTFootstepsBuilder::Reset();
	TestNull(TEXT("creature unavailable after reset"), WoTCreatureData::Find(TEXT("Fixture")));
	TestNull(TEXT("blood unavailable after reset"), WoTBloodFX::Find(TEXT("Fixture")));
	TestEqual(TEXT("footsteps unavailable after reset"),
		WoTFootsteps::Table(TEXT("Fixture")).Num(), 0);
	return !HasAnyErrors();
}

#endif
