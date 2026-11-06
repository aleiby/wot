#pragma once

#include "CoreMinimal.h"

namespace WoTRetailBuilderCanonical
{
inline void Ascii(TArray<uint8>& Out, const ANSICHAR* Value)
{
	Out.Append(reinterpret_cast<const uint8*>(Value), FCStringAnsi::Strlen(Value));
}

inline void Integer(TArray<uint8>& Out, int64 Value)
{
	const FString Text = FString::Printf(TEXT("%lld"), static_cast<long long>(Value));
	FTCHARToUTF8 Utf8(*Text);
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

/**
 * Canonical text holds every name and string ASCII case-folded. UE1 names and
 * FName identity are case-insensitive, and a packaged build (no
 * WITH_CASE_PRESERVING_NAME) returns from FName::ToString() whichever casing
 * the process registered first, so raw casing would make a family digest
 * depend on process history instead of on the table. Only A-Z fold, which
 * keeps the bytes independent of platform locale tables.
 */
inline FString Fold(FString Value)
{
	auto& Characters = Value.GetCharArray();
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		TCHAR& Character = Characters[Index];
		if (Character >= TEXT('A') && Character <= TEXT('Z'))
		{
			Character = static_cast<TCHAR>(Character - TEXT('A') + TEXT('a'));
		}
	}
	return Value;
}

inline void String(TArray<uint8>& Out, const FString& Value)
{
	FTCHARToUTF8 Utf8(*Fold(Value));
	Integer(Out, Utf8.Length());
	Ascii(Out, ":");
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

inline void Name(TArray<uint8>& Out, FName Value)
{
	String(Out, Value.IsNone() ? FString(TEXT("None")) : Value.ToString());
}

inline uint32 FloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

inline void Float(TArray<uint8>& Out, float Value)
{
	constexpr ANSICHAR Digits[] = "0123456789abcdef";
	const uint32 Bits = FloatBits(Value);
	ANSICHAR Text[10] = {'0', 'x'};
	for (int32 Digit = 0; Digit < 8; ++Digit)
	{
		Text[Digit + 2] = Digits[(Bits >> ((7 - Digit) * 4)) & 0x0fu];
	}
	Out.Append(reinterpret_cast<const uint8*>(Text), UE_ARRAY_COUNT(Text));
}

inline void Bool(TArray<uint8>& Out, bool bValue)
{
	Ascii(Out, bValue ? "1" : "0");
}

inline bool NameLess(FName Left, FName Right)
{
	return Fold(Left.ToString()).Compare(
		Fold(Right.ToString()), ESearchCase::CaseSensitive) < 0;
}
}
