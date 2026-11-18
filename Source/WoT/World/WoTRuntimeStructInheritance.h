#pragma once

#include "CoreMinimal.h"
#include "WoTRuntimeClassDefaults.h"

/**
 * Reconstruct one UE1 fixed-array struct cell member by member.
 *
 * UE1 defaultproperties edits an inherited CDO struct in place. A child may
 * therefore author only an outer member, or only one member of a nested
 * struct, while every omitted member retains the parent value. The retail
 * codec normally exposes the saved, complete CDO cell; this resolver also
 * preserves the rule for member-precise fixtures and future codecs.
 */
namespace WoTRuntimeStructInheritance
{
	inline FWoTRuntimeWorldProperty* FindMember(
		FWoTRuntimeWorldProperty& Struct, const FWoTRuntimeWorldProperty& Member)
	{
		return Struct.StructMembers.FindByPredicate(
			[&Member](const FWoTRuntimeWorldProperty& Candidate)
			{
				return Candidate.ArrayIndex == Member.ArrayIndex
					&& Candidate.Name.Equals(Member.Name, ESearchCase::IgnoreCase);
			});
	}

	inline void Overlay(FWoTRuntimeWorldProperty& Base,
		const FWoTRuntimeWorldProperty& Authored)
	{
		if (Base.Type != EWoTRuntimeWorldPropertyType::Struct
			|| Authored.Type != EWoTRuntimeWorldPropertyType::Struct
			// Vector/Rotator are atomic packed structs, not reflected member
			// lists. Keeping the parent's raw bytes would erase child overrides.
			|| (Authored.StructMembers.IsEmpty() && !Authored.Value.IsEmpty())
			|| !Base.StructName.Equals(
				Authored.StructName, ESearchCase::IgnoreCase))
		{
			Base = Authored;
			return;
		}

		for (const FWoTRuntimeWorldProperty& AuthoredMember :
			Authored.StructMembers)
		{
			FWoTRuntimeWorldProperty* BaseMember = FindMember(Base, AuthoredMember);
			if (BaseMember == nullptr)
			{
				Base.StructMembers.Add(AuthoredMember);
			}
			else
			{
				Overlay(*BaseMember, AuthoredMember);
			}
		}
		// Generic reflection retains both packed bytes and named coordinates for
		// Vector/Rotator. Rebuild the bytes consumed by the strict readers after
		// member overlay, including partial-coordinate fixture overrides.
		const bool bVector = Base.StructName.Equals(TEXT("Vector"), ESearchCase::IgnoreCase);
		const bool bRotator = Base.StructName.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase);
		if (bVector || bRotator)
		{
			const TCHAR* Names[3] = { bVector ? TEXT("X") : TEXT("Pitch"),
				bVector ? TEXT("Y") : TEXT("Yaw"), bVector ? TEXT("Z") : TEXT("Roll") };
			if (Base.Value.Num() != 12) Base.Value.SetNumZeroed(12);
			for (int32 Coordinate = 0; Coordinate < 3; ++Coordinate)
				for (const auto& Member : Base.StructMembers)
					if (Member.Name.Equals(Names[Coordinate], ESearchCase::IgnoreCase)
						&& Member.ArrayIndex == 0 && Member.Value.Num() == 4)
						FMemory::Memcpy(Base.Value.GetData() + Coordinate * 4, Member.Value.GetData(), 4);
		}
	}

	inline bool Resolve(const FWoTRuntimeClassDefaults& Defaults,
		FStringView ClassIdentity, FStringView PropertyName, uint32 ArrayIndex,
		FWoTRuntimeWorldProperty& Out)
	{
		TArray<FString> Chain;
		if (!Defaults.GetClassChain(ClassIdentity, Chain))
		{
			return false;
		}

		bool bFound = false;
		for (int32 ChainIndex = Chain.Num() - 1; ChainIndex >= 0; --ChainIndex)
		{
			const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(
				Chain[ChainIndex], PropertyName, ArrayIndex);
			if (Property == nullptr)
			{
				continue;
			}
			if (!bFound)
			{
				Out = *Property;
				bFound = true;
			}
			else
			{
				Overlay(Out, *Property);
			}
		}
		return bFound;
	}
}
