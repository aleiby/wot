#include "WoTBootstrapCore/CanonicalJson.h"

#include "Containers/StringConv.h"

namespace
{
constexpr uint32 MaxUnicodeScalar = 0x10FFFF;
constexpr uint32 HighSurrogateFirst = 0xD800;
constexpr uint32 HighSurrogateLast = 0xDBFF;
constexpr uint32 LowSurrogateFirst = 0xDC00;
constexpr uint32 LowSurrogateLast = 0xDFFF;

/** One past int64's positive range: the largest magnitude a negative literal may carry. */
constexpr uint64 Int64MagnitudeLimit = 9223372036854775808ull;

bool IsAsciiDigit(uint8 Byte)
{
	return Byte >= '0' && Byte <= '9';
}

void AppendUtf8(TArray<uint8>& Out, uint32 Scalar)
{
	// Callers validate the scalar first, so no surrogate or out-of-range value
	// reaches this encoder.
	if (Scalar < 0x80)
	{
		Out.Add(static_cast<uint8>(Scalar));
	}
	else if (Scalar < 0x800)
	{
		Out.Add(static_cast<uint8>(0xC0 | (Scalar >> 6)));
		Out.Add(static_cast<uint8>(0x80 | (Scalar & 0x3F)));
	}
	else if (Scalar < 0x10000)
	{
		Out.Add(static_cast<uint8>(0xE0 | (Scalar >> 12)));
		Out.Add(static_cast<uint8>(0x80 | ((Scalar >> 6) & 0x3F)));
		Out.Add(static_cast<uint8>(0x80 | (Scalar & 0x3F)));
	}
	else
	{
		Out.Add(static_cast<uint8>(0xF0 | (Scalar >> 18)));
		Out.Add(static_cast<uint8>(0x80 | ((Scalar >> 12) & 0x3F)));
		Out.Add(static_cast<uint8>(0x80 | ((Scalar >> 6) & 0x3F)));
		Out.Add(static_cast<uint8>(0x80 | (Scalar & 0x3F)));
	}
}

/**
 * Decode one strictly valid UTF-8 sequence.
 *
 * Returns the number of bytes consumed, or 0 for anything a security-relevant
 * decoder must refuse: a continuation or 5-byte lead in the first position, a
 * missing or malformed continuation, a truncated tail, an overlong encoding, a
 * UTF-8-encoded surrogate, or a scalar above U+10FFFF. Overlong forms matter
 * because they are a second spelling of a codepoint, which is exactly what a
 * canonical format must not have.
 */
int32 DecodeUtf8(const uint8* Data, int32 Available, uint32& OutScalar)
{
	if (Available <= 0)
	{
		return 0;
	}

	const uint8 Lead = Data[0];
	if (Lead < 0x80)
	{
		OutScalar = Lead;
		return 1;
	}

	int32 Length = 0;
	uint32 Scalar = 0;
	uint32 Minimum = 0;
	if ((Lead & 0xE0) == 0xC0)
	{
		Length = 2;
		Scalar = Lead & 0x1F;
		Minimum = 0x80;
	}
	else if ((Lead & 0xF0) == 0xE0)
	{
		Length = 3;
		Scalar = Lead & 0x0F;
		Minimum = 0x800;
	}
	else if ((Lead & 0xF8) == 0xF0)
	{
		Length = 4;
		Scalar = Lead & 0x07;
		Minimum = 0x10000;
	}
	else
	{
		return 0;
	}

	if (Available < Length)
	{
		return 0;
	}
	for (int32 Index = 1; Index < Length; ++Index)
	{
		const uint8 Continuation = Data[Index];
		if ((Continuation & 0xC0) != 0x80)
		{
			return 0;
		}
		Scalar = (Scalar << 6) | (Continuation & 0x3F);
	}

	if (Scalar < Minimum || Scalar > MaxUnicodeScalar)
	{
		return 0;
	}
	if (Scalar >= HighSurrogateFirst && Scalar <= LowSurrogateLast)
	{
		return 0;
	}

	OutScalar = Scalar;
	return Length;
}

/**
 * Order two decoded keys the way Python's `sort_keys=True` does: by codepoint.
 *
 * The comparison runs over UTF-8 encodings because UTF-8 byte order is exactly
 * codepoint order — which UTF-16 code-unit order is not, for anything above the
 * BMP. Returns <0, 0, or >0.
 */
int32 CompareCanonicalKeys(FStringView Left, FStringView Right)
{
	const FTCHARToUTF8 LeftUtf8(Left.GetData(), Left.Len());
	const FTCHARToUTF8 RightUtf8(Right.GetData(), Right.Len());
	const int32 LeftLength = LeftUtf8.Length();
	const int32 RightLength = RightUtf8.Length();
	const int32 Common = FMath::Min(LeftLength, RightLength);
	if (Common > 0)
	{
		const int32 Compared = FMemory::Memcmp(LeftUtf8.Get(), RightUtf8.Get(), Common);
		if (Compared != 0)
		{
			return Compared;
		}
	}
	return LeftLength - RightLength;
}

void AppendAscii(TArray<uint8>& Out, const ANSICHAR* Text)
{
	for (const ANSICHAR* Cursor = Text; *Cursor != '\0'; ++Cursor)
	{
		Out.Add(static_cast<uint8>(*Cursor));
	}
}

void AppendUnicodeEscape(TArray<uint8>& Out, uint32 CodeUnit)
{
	static const ANSICHAR* const Hex = "0123456789abcdef";
	Out.Add('\\');
	Out.Add('u');
	Out.Add(static_cast<uint8>(Hex[(CodeUnit >> 12) & 0xF]));
	Out.Add(static_cast<uint8>(Hex[(CodeUnit >> 8) & 0xF]));
	Out.Add(static_cast<uint8>(Hex[(CodeUnit >> 4) & 0xF]));
	Out.Add(static_cast<uint8>(Hex[CodeUnit & 0xF]));
}

/**
 * A cursor over the supplied bytes.
 *
 * There is no whitespace skipping anywhere in this parser, deliberately: the
 * canonical form has none, so tolerating it would accept two spellings of one
 * document and break the guarantee that re-emission reproduces the input.
 */
struct FCanonicalJsonParser
{
	const uint8* Data = nullptr;
	int32 Size = 0;
	const FWoTCanonicalJsonLimits& Limits;
	int32 Position = 0;

	bool AtEnd() const { return Position >= Size; }
	uint8 Current() const { return Data[Position]; }

	bool Consume(uint8 Byte)
	{
		if (!AtEnd() && Current() == Byte)
		{
			++Position;
			return true;
		}
		return false;
	}

	FWoTBootstrapError Fail(EWoTBootstrapError Code, FString Message) const
	{
		return MakeWoTBootstrapError(Code, MoveTemp(Message),
			FString::Printf(TEXT("byte %d"), Position));
	}

	bool ConsumeLiteral(const ANSICHAR* Text)
	{
		int32 Length = 0;
		while (Text[Length] != '\0')
		{
			++Length;
		}
		if (Position + Length > Size)
		{
			return false;
		}
		for (int32 Index = 0; Index < Length; ++Index)
		{
			if (Data[Position + Index] != static_cast<uint8>(Text[Index]))
			{
				return false;
			}
		}
		Position += Length;
		return true;
	}

	TOptional<FWoTBootstrapError> ParseHex4(uint32& OutValue)
	{
		if (Position + 4 > Size)
		{
			return Fail(EWoTBootstrapError::NonCanonicalJson,
				TEXT("truncated \\u escape"));
		}
		uint32 Value = 0;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const uint8 Digit = Data[Position + Index];
			uint32 Nibble = 0;
			if (IsAsciiDigit(Digit))
			{
				Nibble = Digit - '0';
			}
			else if (Digit >= 'a' && Digit <= 'f')
			{
				Nibble = 10 + (Digit - 'a');
			}
			else if (Digit >= 'A' && Digit <= 'F')
			{
				// Upper-case hex is a second spelling of the same escape.
				return Fail(EWoTBootstrapError::NonCanonicalJson,
					TEXT("\\u escape hex digits must be lower case"));
			}
			else
			{
				return Fail(EWoTBootstrapError::NonCanonicalJson,
					TEXT("\\u escape needs four hex digits"));
			}
			Value = (Value << 4) | Nibble;
		}
		Position += 4;
		OutValue = Value;
		return {};
	}

	TOptional<FWoTBootstrapError> ParseStringEscape(TArray<uint8>& OutUtf8)
	{
		++Position;  // past the backslash
		if (AtEnd())
		{
			return Fail(EWoTBootstrapError::NonCanonicalJson, TEXT("truncated escape"));
		}

		const uint8 Kind = Data[Position++];
		switch (Kind)
		{
		case '"':  OutUtf8.Add(0x22); return {};
		case '\\': OutUtf8.Add(0x5C); return {};
		case 'b':  OutUtf8.Add(0x08); return {};
		case 'f':  OutUtf8.Add(0x0C); return {};
		case 'n':  OutUtf8.Add(0x0A); return {};
		case 'r':  OutUtf8.Add(0x0D); return {};
		case 't':  OutUtf8.Add(0x09); return {};
		case 'u':  break;
		default:
			// Includes \/ , which is legal JSON but never emitted canonically.
			return Fail(EWoTBootstrapError::NonCanonicalJson,
				FString::Printf(TEXT("unsupported escape \\%c"),
					static_cast<TCHAR>(Kind)));
		}

		uint32 First = 0;
		if (TOptional<FWoTBootstrapError> HexError = ParseHex4(First); HexError.IsSet())
		{
			return HexError;
		}

		uint32 Scalar = First;
		if (First >= HighSurrogateFirst && First <= HighSurrogateLast)
		{
			// Above the BMP the canonical spelling is a surrogate pair, so a high
			// surrogate must be followed by a low one in the same string.
			if (Position + 2 > Size || Data[Position] != '\\' || Data[Position + 1] != 'u')
			{
				return Fail(EWoTBootstrapError::InvalidUtf8,
					TEXT("unpaired high surrogate escape"));
			}
			Position += 2;
			uint32 Second = 0;
			if (TOptional<FWoTBootstrapError> PairError = ParseHex4(Second); PairError.IsSet())
			{
				return PairError;
			}
			if (Second < LowSurrogateFirst || Second > LowSurrogateLast)
			{
				return Fail(EWoTBootstrapError::InvalidUtf8,
					TEXT("high surrogate escape is not followed by a low surrogate"));
			}
			Scalar = 0x10000 + ((First - HighSurrogateFirst) << 10) + (Second - LowSurrogateFirst);
		}
		else if (First >= LowSurrogateFirst && First <= LowSurrogateLast)
		{
			return Fail(EWoTBootstrapError::InvalidUtf8,
				TEXT("lone low surrogate escape"));
		}
		else if (First == 0)
		{
			// FString terminates at NUL, so a U+0000 would silently truncate the
			// value and break the byte-exact round trip. No artifact carries one.
			return Fail(EWoTBootstrapError::NonCanonicalJson,
				TEXT("U+0000 cannot be represented"));
		}
		else if (First == 0x08 || First == 0x09 || First == 0x0A || First == 0x0C || First == 0x0D)
		{
			return Fail(EWoTBootstrapError::NonCanonicalJson,
				TEXT("this control character must use its shorthand escape"));
		}
		else if (First >= 0x20 && First <= 0x7E)
		{
			return Fail(EWoTBootstrapError::NonCanonicalJson,
				TEXT("printable ASCII must appear literally, not escaped"));
		}

		AppendUtf8(OutUtf8, Scalar);
		return {};
	}

	TOptional<FWoTBootstrapError> ParseString(FString& OutText, TArray<uint8>& OutUtf8)
	{
		if (!Consume('"'))
		{
			return Fail(EWoTBootstrapError::NonCanonicalJson, TEXT("expected a string"));
		}

		OutUtf8.Reset();
		for (;;)
		{
			if (AtEnd())
			{
				return Fail(EWoTBootstrapError::NonCanonicalJson,
					TEXT("unterminated string"));
			}

			const uint8 Byte = Current();
			if (Byte == '"')
			{
				++Position;
				break;
			}

			if (Byte == '\\')
			{
				if (TOptional<FWoTBootstrapError> EscapeError = ParseStringEscape(OutUtf8);
					EscapeError.IsSet())
				{
					return EscapeError;
				}
			}
			else if (Byte < 0x20)
			{
				return Fail(EWoTBootstrapError::NonCanonicalJson,
					FString::Printf(TEXT("raw control byte 0x%02x must be escaped"), Byte));
			}
			else if (Byte >= 0x7F)
			{
				// Classify before rejecting: malformed bytes are a UTF-8 fault,
				// while well-formed non-ASCII text is merely the wrong spelling.
				uint32 Scalar = 0;
				if (Byte > 0x7F && DecodeUtf8(Data + Position, Size - Position, Scalar) == 0)
				{
					return Fail(EWoTBootstrapError::InvalidUtf8,
						TEXT("malformed UTF-8 sequence"));
				}
				return Fail(EWoTBootstrapError::NonCanonicalJson,
					TEXT("non-ASCII text must be written as a \\u escape"));
			}
			else
			{
				OutUtf8.Add(Byte);
				++Position;
			}

			if (static_cast<uint64>(OutUtf8.Num()) > Limits.MaxStringBytes)
			{
				return Fail(EWoTBootstrapError::SizeLimit,
					TEXT("string exceeds MaxStringBytes"));
			}
		}

		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(OutUtf8.GetData()), OutUtf8.Num());
		OutText = FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
		return {};
	}

	TOptional<FWoTBootstrapError> ParseNumber(int64& OutValue)
	{
		const bool bNegative = Consume('-');

		const int32 DigitStart = Position;
		uint64 Magnitude = 0;
		while (!AtEnd() && IsAsciiDigit(Current()))
		{
			const uint64 Digit = static_cast<uint64>(Current() - '0');
			if (Magnitude > (Int64MagnitudeLimit - Digit) / 10)
			{
				return Fail(EWoTBootstrapError::InvalidNumber,
					TEXT("integer is outside the int64 range"));
			}
			Magnitude = Magnitude * 10 + Digit;
			++Position;
		}

		const int32 DigitCount = Position - DigitStart;
		if (DigitCount == 0)
		{
			return Fail(EWoTBootstrapError::InvalidNumber,
				TEXT("number needs at least one digit"));
		}
		if (DigitCount > 1 && Data[DigitStart] == '0')
		{
			return Fail(EWoTBootstrapError::InvalidNumber,
				TEXT("leading zeros are not canonical"));
		}
		if (bNegative && Magnitude == 0)
		{
			// -0 and 0 would be two spellings of the same value.
			return Fail(EWoTBootstrapError::InvalidNumber,
				TEXT("negative zero is not canonical"));
		}
		if (!bNegative && Magnitude > Int64MagnitudeLimit - 1)
		{
			return Fail(EWoTBootstrapError::InvalidNumber,
				TEXT("integer is outside the int64 range"));
		}
		if (!AtEnd() && (Current() == '.' || Current() == 'e' || Current() == 'E'))
		{
			// Protocol fields are integers: a fraction or exponent has no canonical
			// integer spelling and must not be silently truncated.
			return Fail(EWoTBootstrapError::InvalidNumber,
				TEXT("only integers are allowed"));
		}

		if (bNegative)
		{
			OutValue = (Magnitude == Int64MagnitudeLimit)
				? MIN_int64 : -static_cast<int64>(Magnitude);
		}
		else
		{
			OutValue = static_cast<int64>(Magnitude);
		}
		return {};
	}

	TOptional<FWoTBootstrapError> ParseArray(uint16 Depth, FWoTCanonicalJsonValue& Out)
	{
		++Position;  // past '['
		Out = FWoTCanonicalJsonValue::MakeArray();
		if (Consume(']'))
		{
			return {};
		}

		for (;;)
		{
			if (static_cast<uint64>(Out.Num()) >= Limits.MaxMembers)
			{
				return Fail(EWoTBootstrapError::SizeLimit,
					TEXT("array exceeds MaxMembers"));
			}

			FWoTCanonicalJsonValue Element;
			if (TOptional<FWoTBootstrapError> ElementError = ParseValue(Depth, Element);
				ElementError.IsSet())
			{
				return ElementError;
			}
			Out.AddElement(MoveTemp(Element));

			if (Consume(','))
			{
				continue;
			}
			if (Consume(']'))
			{
				return {};
			}
			return Fail(EWoTBootstrapError::NonCanonicalJson,
				TEXT("expected ',' or ']' in an array"));
		}
	}

	TOptional<FWoTBootstrapError> ParseObject(uint16 Depth, FWoTCanonicalJsonValue& Out)
	{
		++Position;  // past '{'
		Out = FWoTCanonicalJsonValue::MakeObject();
		if (Consume('}'))
		{
			return {};
		}

		FString PreviousKey;
		bool bHasPrevious = false;
		for (;;)
		{
			if (static_cast<uint64>(Out.Num()) >= Limits.MaxMembers)
			{
				return Fail(EWoTBootstrapError::SizeLimit,
					TEXT("object exceeds MaxMembers"));
			}

			FString Key;
			TArray<uint8> KeyUtf8;
			if (TOptional<FWoTBootstrapError> KeyError = ParseString(Key, KeyUtf8);
				KeyError.IsSet())
			{
				return KeyError;
			}

			if (bHasPrevious)
			{
				// Equal is a duplicate field; smaller is merely out of order. The
				// two are different faults and callers act differently on them.
				const int32 Compared = CompareCanonicalKeys(PreviousKey, Key);
				if (Compared == 0)
				{
					return Fail(EWoTBootstrapError::DuplicateField,
						FString::Printf(TEXT("duplicate object key '%s'"), *Key));
				}
				if (Compared > 0)
				{
					return Fail(EWoTBootstrapError::NonCanonicalJson,
						FString::Printf(TEXT("object key '%s' does not follow '%s'"),
							*Key, *PreviousKey));
				}
			}

			if (!Consume(':'))
			{
				return Fail(EWoTBootstrapError::NonCanonicalJson,
					TEXT("expected ':' after an object key"));
			}

			FWoTCanonicalJsonValue Value;
			if (TOptional<FWoTBootstrapError> ValueError = ParseValue(Depth, Value);
				ValueError.IsSet())
			{
				return ValueError;
			}

			PreviousKey = Key;
			bHasPrevious = true;
			Out.AddMember(MoveTemp(Key), MoveTemp(Value));

			if (Consume(','))
			{
				continue;
			}
			if (Consume('}'))
			{
				return {};
			}
			return Fail(EWoTBootstrapError::NonCanonicalJson,
				TEXT("expected ',' or '}' in an object"));
		}
	}

	/** Depth counts containers already open, so the top-level call passes 0. */
	TOptional<FWoTBootstrapError> ParseValue(uint16 Depth, FWoTCanonicalJsonValue& Out)
	{
		if (AtEnd())
		{
			return Fail(EWoTBootstrapError::NonCanonicalJson,
				TEXT("unexpected end of document"));
		}

		const uint8 Byte = Current();
		if (Byte == '{' || Byte == '[')
		{
			const uint32 Nested = static_cast<uint32>(Depth) + 1;
			if (Nested > Limits.MaxDepth)
			{
				return Fail(EWoTBootstrapError::SizeLimit,
					TEXT("document exceeds MaxDepth"));
			}
			const uint16 NestedDepth = static_cast<uint16>(Nested);
			return Byte == '{' ? ParseObject(NestedDepth, Out) : ParseArray(NestedDepth, Out);
		}

		if (Byte == '"')
		{
			FString Text;
			TArray<uint8> Utf8;
			if (TOptional<FWoTBootstrapError> TextError = ParseString(Text, Utf8);
				TextError.IsSet())
			{
				return TextError;
			}
			Out = FWoTCanonicalJsonValue::MakeString(MoveTemp(Text));
			return {};
		}

		if (Byte == 'n')
		{
			if (!ConsumeLiteral("null"))
			{
				return Fail(EWoTBootstrapError::NonCanonicalJson, TEXT("expected null"));
			}
			Out = FWoTCanonicalJsonValue::MakeNull();
			return {};
		}
		if (Byte == 't')
		{
			if (!ConsumeLiteral("true"))
			{
				return Fail(EWoTBootstrapError::NonCanonicalJson, TEXT("expected true"));
			}
			Out = FWoTCanonicalJsonValue::MakeBool(true);
			return {};
		}
		if (Byte == 'f')
		{
			if (!ConsumeLiteral("false"))
			{
				return Fail(EWoTBootstrapError::NonCanonicalJson, TEXT("expected false"));
			}
			Out = FWoTCanonicalJsonValue::MakeBool(false);
			return {};
		}

		if (Byte == '-' || IsAsciiDigit(Byte))
		{
			int64 Value = 0;
			if (TOptional<FWoTBootstrapError> NumberError = ParseNumber(Value);
				NumberError.IsSet())
			{
				return NumberError;
			}
			Out = FWoTCanonicalJsonValue::MakeInteger(Value);
			return {};
		}

		return Fail(EWoTBootstrapError::NonCanonicalJson,
			FString::Printf(TEXT("unexpected byte 0x%02x where a value was required"), Byte));
	}
};

TOptional<FWoTBootstrapError> WriteStringBytes(const FString& Text, TArray<uint8>& Out)
{
	const FTCHARToUTF8 Utf8(*Text);
	const uint8* const Bytes = reinterpret_cast<const uint8*>(Utf8.Get());
	const int32 Length = Utf8.Length();

	Out.Add('"');
	int32 Index = 0;
	while (Index < Length)
	{
		uint32 Scalar = 0;
		const int32 Consumed = DecodeUtf8(Bytes + Index, Length - Index, Scalar);
		if (Consumed == 0)
		{
			// A string that is not valid Unicode has no canonical form; refusing to
			// write it is the only safe answer when the bytes will be signed.
			return MakeWoTBootstrapError(EWoTBootstrapError::InvalidUtf8,
				TEXT("string is not valid UTF-8"), Text);
		}
		Index += Consumed;

		switch (Scalar)
		{
		case 0x22: AppendAscii(Out, "\\\""); continue;
		case 0x5C: AppendAscii(Out, "\\\\"); continue;
		case 0x08: AppendAscii(Out, "\\b");  continue;
		case 0x09: AppendAscii(Out, "\\t");  continue;
		case 0x0A: AppendAscii(Out, "\\n");  continue;
		case 0x0C: AppendAscii(Out, "\\f");  continue;
		case 0x0D: AppendAscii(Out, "\\r");  continue;
		default: break;
		}

		if (Scalar >= 0x20 && Scalar <= 0x7E)
		{
			Out.Add(static_cast<uint8>(Scalar));
		}
		else if (Scalar <= 0xFFFF)
		{
			AppendUnicodeEscape(Out, Scalar);
		}
		else
		{
			// ASCII-only output means non-BMP scalars are written as the surrogate
			// pair that Python's ensure_ascii produces.
			const uint32 Adjusted = Scalar - 0x10000;
			AppendUnicodeEscape(Out, HighSurrogateFirst + (Adjusted >> 10));
			AppendUnicodeEscape(Out, LowSurrogateFirst + (Adjusted & 0x3FF));
		}
	}
	Out.Add('"');
	return {};
}

TOptional<FWoTBootstrapError> WriteValueBytes(const FWoTCanonicalJsonValue& Value, TArray<uint8>& Out)
{
	switch (Value.GetKind())
	{
	case EWoTCanonicalJsonKind::Null:
		AppendAscii(Out, "null");
		return {};

	case EWoTCanonicalJsonKind::Bool:
		AppendAscii(Out, Value.GetBool() ? "true" : "false");
		return {};

	case EWoTCanonicalJsonKind::Integer:
	{
		const FString Text = FString::Printf(TEXT("%lld"), Value.GetInteger());
		for (const TCHAR Character : Text)
		{
			Out.Add(static_cast<uint8>(Character));
		}
		return {};
	}

	case EWoTCanonicalJsonKind::String:
		return WriteStringBytes(Value.GetString(), Out);

	case EWoTCanonicalJsonKind::Array:
	{
		Out.Add('[');
		for (int32 Index = 0; Index < Value.Num(); ++Index)
		{
			if (Index > 0)
			{
				Out.Add(',');
			}
			if (TOptional<FWoTBootstrapError> ElementError =
					WriteValueBytes(Value.GetElement(Index), Out); ElementError.IsSet())
			{
				return ElementError;
			}
		}
		Out.Add(']');
		return {};
	}

	case EWoTCanonicalJsonKind::Object:
	{
		Out.Add('{');
		for (int32 Index = 0; Index < Value.Num(); ++Index)
		{
			if (Index > 0)
			{
				// AddMember already refuses out-of-order keys, so this is a
				// defence-in-depth check: nothing may emit non-canonical bytes.
				if (CompareCanonicalKeys(Value.GetMemberKey(Index - 1),
						Value.GetMemberKey(Index)) >= 0)
				{
					return MakeWoTBootstrapError(EWoTBootstrapError::NonCanonicalJson,
						TEXT("object members are not in canonical order"),
						Value.GetMemberKey(Index));
				}
				Out.Add(',');
			}
			if (TOptional<FWoTBootstrapError> KeyError =
					WriteStringBytes(Value.GetMemberKey(Index), Out); KeyError.IsSet())
			{
				return KeyError;
			}
			Out.Add(':');
			if (TOptional<FWoTBootstrapError> MemberError =
					WriteValueBytes(Value.GetMemberValue(Index), Out); MemberError.IsSet())
			{
				return MemberError;
			}
		}
		Out.Add('}');
		return {};
	}
	}

	return MakeWoTBootstrapError(EWoTBootstrapError::NonCanonicalJson,
		TEXT("unknown canonical JSON kind"));
}
}  // namespace

FWoTCanonicalJsonValue::FWoTCanonicalJsonValue() = default;
FWoTCanonicalJsonValue::~FWoTCanonicalJsonValue() = default;
FWoTCanonicalJsonValue::FWoTCanonicalJsonValue(FWoTCanonicalJsonValue&& Other) = default;
FWoTCanonicalJsonValue& FWoTCanonicalJsonValue::operator=(FWoTCanonicalJsonValue&& Other) = default;

FWoTCanonicalJsonValue FWoTCanonicalJsonValue::MakeNull()
{
	return FWoTCanonicalJsonValue();
}

FWoTCanonicalJsonValue FWoTCanonicalJsonValue::MakeBool(bool bValue)
{
	FWoTCanonicalJsonValue Value;
	Value.Kind = EWoTCanonicalJsonKind::Bool;
	Value.bBoolValue = bValue;
	return Value;
}

FWoTCanonicalJsonValue FWoTCanonicalJsonValue::MakeInteger(int64 Integer)
{
	FWoTCanonicalJsonValue Value;
	Value.Kind = EWoTCanonicalJsonKind::Integer;
	Value.IntegerValue = Integer;
	return Value;
}

FWoTCanonicalJsonValue FWoTCanonicalJsonValue::MakeString(FString Text)
{
	FWoTCanonicalJsonValue Value;
	Value.Kind = EWoTCanonicalJsonKind::String;
	Value.StringValue = MoveTemp(Text);
	return Value;
}

FWoTCanonicalJsonValue FWoTCanonicalJsonValue::MakeArray()
{
	FWoTCanonicalJsonValue Value;
	Value.Kind = EWoTCanonicalJsonKind::Array;
	return Value;
}

FWoTCanonicalJsonValue FWoTCanonicalJsonValue::MakeObject()
{
	FWoTCanonicalJsonValue Value;
	Value.Kind = EWoTCanonicalJsonKind::Object;
	return Value;
}

bool FWoTCanonicalJsonValue::GetBool() const
{
	check(Kind == EWoTCanonicalJsonKind::Bool);
	return bBoolValue;
}

int64 FWoTCanonicalJsonValue::GetInteger() const
{
	check(Kind == EWoTCanonicalJsonKind::Integer);
	return IntegerValue;
}

const FString& FWoTCanonicalJsonValue::GetString() const
{
	check(Kind == EWoTCanonicalJsonKind::String);
	return StringValue;
}

int32 FWoTCanonicalJsonValue::Num() const
{
	return Children.Num();
}

const FWoTCanonicalJsonValue& FWoTCanonicalJsonValue::GetElement(int32 Index) const
{
	check(Kind == EWoTCanonicalJsonKind::Array);
	return *Children[Index];
}

const FString& FWoTCanonicalJsonValue::GetMemberKey(int32 Index) const
{
	check(Kind == EWoTCanonicalJsonKind::Object);
	return MemberKeys[Index];
}

const FWoTCanonicalJsonValue& FWoTCanonicalJsonValue::GetMemberValue(int32 Index) const
{
	check(Kind == EWoTCanonicalJsonKind::Object);
	return *Children[Index];
}

const FWoTCanonicalJsonValue* FWoTCanonicalJsonValue::FindMember(FStringView Key) const
{
	if (Kind != EWoTCanonicalJsonKind::Object)
	{
		return nullptr;
	}

	// Members are sorted by construction, so lookup is a binary search rather than
	// a scan — and uses the same comparator that defines the order.
	int32 Low = 0;
	int32 High = MemberKeys.Num() - 1;
	while (Low <= High)
	{
		const int32 Middle = Low + (High - Low) / 2;
		const int32 Compared = CompareCanonicalKeys(MemberKeys[Middle], Key);
		if (Compared == 0)
		{
			return Children[Middle].Get();
		}
		if (Compared < 0)
		{
			Low = Middle + 1;
		}
		else
		{
			High = Middle - 1;
		}
	}
	return nullptr;
}

void FWoTCanonicalJsonValue::AddElement(FWoTCanonicalJsonValue&& Element)
{
	check(Kind == EWoTCanonicalJsonKind::Array);
	Children.Add(MakeUnique<FWoTCanonicalJsonValue>(MoveTemp(Element)));
}

bool FWoTCanonicalJsonValue::AddMember(FString Key, FWoTCanonicalJsonValue&& Value)
{
	check(Kind == EWoTCanonicalJsonKind::Object);
	if (MemberKeys.Num() > 0 && CompareCanonicalKeys(MemberKeys.Last(), Key) >= 0)
	{
		return false;
	}
	MemberKeys.Add(MoveTemp(Key));
	Children.Add(MakeUnique<FWoTCanonicalJsonValue>(MoveTemp(Value)));
	return true;
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> ParseCanonicalJson(
	TConstArrayView<uint8> Bytes, const FWoTCanonicalJsonLimits& Limits)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;

	if (static_cast<uint64>(Bytes.Num()) > Limits.MaxBytes)
	{
		// Checked before parsing so an oversized document costs no work.
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
			FString::Printf(TEXT("document is %d bytes, over the %u byte limit"),
				Bytes.Num(), Limits.MaxBytes)));
	}

	FCanonicalJsonParser Parser{Bytes.GetData(), Bytes.Num(), Limits, 0};
	FWoTCanonicalJsonValue Value;
	if (TOptional<FWoTBootstrapError> ParseError = Parser.ParseValue(0, Value);
		ParseError.IsSet())
	{
		return FResult::Error(MoveTemp(ParseError.GetValue()));
	}
	if (Parser.Position != Bytes.Num())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::NonCanonicalJson,
			TEXT("trailing bytes after the document"),
			FString::Printf(TEXT("byte %d"), Parser.Position)));
	}
	return FResult::Success(MoveTemp(Value));
}

TWoTBootstrapResult<TArray<uint8>> WriteCanonicalJson(const FWoTCanonicalJsonValue& Value)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;

	TArray<uint8> Bytes;
	if (TOptional<FWoTBootstrapError> WriteError = WriteValueBytes(Value, Bytes);
		WriteError.IsSet())
	{
		return FResult::Error(MoveTemp(WriteError.GetValue()));
	}
	return FResult::Success(MoveTemp(Bytes));
}

// --- Closed-schema readers ---------------------------------------------------

TOptional<FWoTBootstrapError> RequireExactMembers(const FWoTCanonicalJsonValue& Object,
	TConstArrayView<const TCHAR*> Expected, const TCHAR* What)
{
	if (!Object.IsObject())
	{
		return MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			FString::Printf(TEXT("%s is not an object"), What));
	}

	for (int32 Index = 0; Index < Object.Num(); ++Index)
	{
		const FString& Key = Object.GetMemberKey(Index);
		bool bAllowed = false;
		for (const TCHAR* Candidate : Expected)
		{
			bAllowed |= Key.Equals(Candidate, ESearchCase::CaseSensitive);
		}
		if (!bAllowed)
		{
			return MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
				FString::Printf(TEXT("%s has an unknown field '%s'"), What, *Key));
		}
	}

	for (const TCHAR* Candidate : Expected)
	{
		if (Object.FindMember(Candidate) == nullptr)
		{
			return MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
				FString::Printf(TEXT("%s is missing field '%s'"), What, Candidate));
		}
	}
	return {};
}

TOptional<FWoTBootstrapError> ReadStringField(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, const TCHAR* What, FString& OutValue)
{
	const FWoTCanonicalJsonValue* const Member = Object.FindMember(Field);
	if (Member == nullptr || Member->GetKind() != EWoTCanonicalJsonKind::String)
	{
		return MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			FString::Printf(TEXT("%s field '%s' is not a string"), What, Field));
	}
	OutValue = Member->GetString();
	return {};
}

TOptional<FWoTBootstrapError> ReadBoolField(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, const TCHAR* What, bool& bOutValue)
{
	const FWoTCanonicalJsonValue* const Member = Object.FindMember(Field);
	if (Member == nullptr || Member->GetKind() != EWoTCanonicalJsonKind::Bool)
	{
		return MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			FString::Printf(TEXT("%s field '%s' is not a bool"), What, Field));
	}
	bOutValue = Member->GetBool();
	return {};
}

TOptional<FWoTBootstrapError> ReadUnsignedField(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, const TCHAR* What, uint64& OutValue)
{
	const FWoTCanonicalJsonValue* const Member = Object.FindMember(Field);
	if (Member == nullptr || Member->GetKind() != EWoTCanonicalJsonKind::Integer)
	{
		return MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			FString::Printf(TEXT("%s field '%s' is not an integer"), What, Field));
	}
	const int64 Value = Member->GetInteger();
	if (Value < 0)
	{
		// Sequences, epochs, and sizes count forward. A negative one is not a small
		// number, it is a field that would wrap into an enormous unsigned value.
		return MakeWoTBootstrapError(EWoTBootstrapError::InvalidNumber,
			FString::Printf(TEXT("%s field '%s' must not be negative"), What, Field));
	}
	OutValue = static_cast<uint64>(Value);
	return {};
}

TOptional<FWoTBootstrapError> ReadUnsigned32Field(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, const TCHAR* What, uint32& OutValue)
{
	uint64 Wide = 0;
	if (TOptional<FWoTBootstrapError> FieldError =
			ReadUnsignedField(Object, Field, What, Wide); FieldError.IsSet())
	{
		return FieldError;
	}
	if (Wide > static_cast<uint64>(MAX_uint32))
	{
		return MakeWoTBootstrapError(EWoTBootstrapError::InvalidNumber,
			FString::Printf(TEXT("%s field '%s' does not fit in 32 bits"), What, Field));
	}
	OutValue = static_cast<uint32>(Wide);
	return {};
}

TOptional<FWoTBootstrapError> ReadObjectField(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, const TCHAR* What, const FWoTCanonicalJsonValue*& OutValue)
{
	const FWoTCanonicalJsonValue* const Member = Object.FindMember(Field);
	if (Member == nullptr || !Member->IsObject())
	{
		return MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			FString::Printf(TEXT("%s field '%s' is not an object"), What, Field));
	}
	OutValue = Member;
	return {};
}
