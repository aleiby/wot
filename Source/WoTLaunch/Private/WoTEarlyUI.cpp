#include "WoTEarlyUI.h"

#include "HAL/Platform.h"
#include "HAL/UnrealMemory.h"
#include "Misc/Char.h"
#include "Misc/ScopeLock.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace
{
constexpr ANSICHAR SchemaLine[] = "Schema=EarlyUI-v1";

FWoTBootstrapError EarlyUIError(
	EWoTBootstrapError Code, const TCHAR* Message, const TCHAR* Path = TEXT("EarlyUI-v1"))
{
	return MakeWoTBootstrapError(Code, Message, Path);
}

bool IsFiniteInRange(float Value, float Minimum, float Maximum)
{
	return std::isfinite(Value) && Value >= Minimum && Value <= Maximum;
}

bool IsKnownWindowMode(EWoTWindowMode Mode)
{
	return Mode == EWoTWindowMode::Windowed
		|| Mode == EWoTWindowMode::WindowedFullscreen
		|| Mode == EWoTWindowMode::Fullscreen;
}

bool IsKnownLocale(EWoTShippedLocale Locale)
{
	return Locale == EWoTShippedLocale::EnUS
		|| Locale == EWoTShippedLocale::FrFR
		|| Locale == EWoTShippedLocale::DeDE
		|| Locale == EWoTShippedLocale::EsES
		|| Locale == EWoTShippedLocale::ItIT;
}

bool LimitsAreValid(
	const FWoTEarlyUILimits& Limits, const FWoTInventoryLocaleSet& Locales)
{
	const FWoTEarlyUIV1& Defaults = Limits.Defaults;
	if (Limits.MaxSerializedBytes < sizeof(SchemaLine)
		|| Limits.MaxSerializedBytes > static_cast<uint32>(MAX_int32)
		|| Limits.MinWidth > Limits.MaxWidth
		|| Limits.MinHeight > Limits.MaxHeight
		|| Limits.MinUIScale > Limits.MaxUIScale
		|| Limits.MinBootstrapVolume > Limits.MaxBootstrapVolume
		|| Limits.MinInputSensitivity > Limits.MaxInputSensitivity
		|| !std::isfinite(Limits.MinUIScale)
		|| !std::isfinite(Limits.MaxUIScale)
		|| !std::isfinite(Limits.MinBootstrapVolume)
		|| !std::isfinite(Limits.MaxBootstrapVolume)
		|| !std::isfinite(Limits.MinInputSensitivity)
		|| !std::isfinite(Limits.MaxInputSensitivity)
		|| !IsKnownWindowMode(Defaults.WindowMode)
		|| !IsKnownLocale(Defaults.Locale)
		|| Defaults.Width < Limits.MinWidth || Defaults.Width > Limits.MaxWidth
		|| Defaults.Height < Limits.MinHeight || Defaults.Height > Limits.MaxHeight
		|| !IsFiniteInRange(Defaults.UIScale, Limits.MinUIScale, Limits.MaxUIScale)
		|| !IsFiniteInRange(Defaults.BootstrapVolume,
			Limits.MinBootstrapVolume, Limits.MaxBootstrapVolume)
		|| !IsFiniteInRange(Defaults.InputSensitivity,
			Limits.MinInputSensitivity, Limits.MaxInputSensitivity)
		|| !Locales.Contains(Defaults.Locale))
	{
		return false;
	}
	for (int32 Index = 0; Index < Locales.Locales.Num(); ++Index)
	{
		if (!IsKnownLocale(Locales.Locales[Index]))
		{
			return false;
		}
		for (int32 Other = Index + 1; Other < Locales.Locales.Num(); ++Other)
		{
			if (Locales.Locales[Index] == Locales.Locales[Other])
			{
				return false;
			}
		}
	}
	return true;
}

bool EqualAscii(FAnsiStringView Value, const ANSICHAR* Literal)
{
	return Value.Equals(FAnsiStringView(Literal), ESearchCase::CaseSensitive);
}

bool ParseUnsigned(FAnsiStringView Value, uint32& OutValue)
{
	if (Value.IsEmpty() || Value.Len() > 10)
	{
		return false;
	}
	uint64 Parsed = 0;
	for (const ANSICHAR Character : Value)
	{
		if (Character < '0' || Character > '9')
		{
			return false;
		}
		Parsed = Parsed * 10 + static_cast<uint64>(Character - '0');
		if (Parsed > MAX_uint32)
		{
			return false;
		}
	}
	OutValue = static_cast<uint32>(Parsed);
	return true;
}

bool ParseFloat(FAnsiStringView Value, float& OutValue)
{
	if (Value.IsEmpty() || Value.Len() > 32)
	{
		return false;
	}
	bool bSawDecimalPoint = false;
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		const ANSICHAR Character = Value[Index];
		if (Character == '.')
		{
			if (bSawDecimalPoint || Index == 0 || Index == Value.Len() - 1)
			{
				return false;
			}
			bSawDecimalPoint = true;
			continue;
		}
		if (Character < '0' || Character > '9')
		{
			return false;
		}
	}
	ANSICHAR Buffer[33] = {};
	FMemory::Memcpy(Buffer, Value.GetData(), Value.Len());
	ANSICHAR* End = nullptr;
	errno = 0;
	const float Parsed = std::strtof(Buffer, &End);
	if (errno == ERANGE || End != Buffer + Value.Len() || !std::isfinite(Parsed))
	{
		return false;
	}
	OutValue = Parsed;
	return true;
}

bool ParseBool(FAnsiStringView Value, bool& OutValue)
{
	if (EqualAscii(Value, "0"))
	{
		OutValue = false;
		return true;
	}
	if (EqualAscii(Value, "1"))
	{
		OutValue = true;
		return true;
	}
	return false;
}

enum class EField : uint8
{
	Width,
	Height,
	WindowMode,
	UIScale,
	Locale,
	ReducedMotion,
	HighContrast,
	ScreenReader,
	BootstrapVolume,
	InputSensitivity,
	Count,
};

int32 FieldIndex(FAnsiStringView Key)
{
	static const ANSICHAR* Names[] = {
		"Width", "Height", "WindowMode", "UIScale", "Locale",
		"ReducedMotion", "HighContrast", "ScreenReader",
		"BootstrapVolume", "InputSensitivity",
	};
	for (int32 Index = 0; Index < static_cast<int32>(EField::Count); ++Index)
	{
		if (EqualAscii(Key, Names[Index]))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool ParseWindowMode(FAnsiStringView Value, EWoTWindowMode& OutMode)
{
	if (EqualAscii(Value, "Windowed"))
	{
		OutMode = EWoTWindowMode::Windowed;
		return true;
	}
	if (EqualAscii(Value, "WindowedFullscreen"))
	{
		OutMode = EWoTWindowMode::WindowedFullscreen;
		return true;
	}
	if (EqualAscii(Value, "Fullscreen"))
	{
		OutMode = EWoTWindowMode::Fullscreen;
		return true;
	}
	return false;
}

bool ParseLocale(FAnsiStringView Value, EWoTShippedLocale& OutLocale)
{
	struct FLocaleName
	{
		const ANSICHAR* Name;
		EWoTShippedLocale Locale;
	};
	static const FLocaleName Names[] = {
		{"en-US", EWoTShippedLocale::EnUS},
		{"fr-FR", EWoTShippedLocale::FrFR},
		{"de-DE", EWoTShippedLocale::DeDE},
		{"es-ES", EWoTShippedLocale::EsES},
		{"it-IT", EWoTShippedLocale::ItIT},
	};
	for (const FLocaleName& Name : Names)
	{
		if (EqualAscii(Value, Name.Name))
		{
			OutLocale = Name.Locale;
			return true;
		}
	}
	return false;
}

void ResetField(FWoTEarlyUIV1& Value, const FWoTEarlyUIV1& Defaults, int32 Field)
{
	switch (static_cast<EField>(Field))
	{
	case EField::Width: Value.Width = Defaults.Width; break;
	case EField::Height: Value.Height = Defaults.Height; break;
	case EField::WindowMode: Value.WindowMode = Defaults.WindowMode; break;
	case EField::UIScale: Value.UIScale = Defaults.UIScale; break;
	case EField::Locale: Value.Locale = Defaults.Locale; break;
	case EField::ReducedMotion: Value.bReducedMotion = Defaults.bReducedMotion; break;
	case EField::HighContrast: Value.bHighContrast = Defaults.bHighContrast; break;
	case EField::ScreenReader: Value.bScreenReader = Defaults.bScreenReader; break;
	case EField::BootstrapVolume: Value.BootstrapVolume = Defaults.BootstrapVolume; break;
	case EField::InputSensitivity: Value.InputSensitivity = Defaults.InputSensitivity; break;
	case EField::Count: break;
	}
}

void ApplyField(FWoTEarlyUIV1& Output, const FWoTEarlyUILimits& Limits,
	const FWoTInventoryLocaleSet& LocaleSet, int32 Field, FAnsiStringView Value)
{
	uint32 Unsigned = 0;
	float Scalar = 0.0f;
	bool Flag = false;
	switch (static_cast<EField>(Field))
	{
	case EField::Width:
		if (ParseUnsigned(Value, Unsigned)
			&& Unsigned >= Limits.MinWidth && Unsigned <= Limits.MaxWidth)
		{
			Output.Width = static_cast<uint16>(Unsigned);
		}
		break;
	case EField::Height:
		if (ParseUnsigned(Value, Unsigned)
			&& Unsigned >= Limits.MinHeight && Unsigned <= Limits.MaxHeight)
		{
			Output.Height = static_cast<uint16>(Unsigned);
		}
		break;
	case EField::WindowMode:
		ParseWindowMode(Value, Output.WindowMode);
		break;
	case EField::UIScale:
		if (ParseFloat(Value, Scalar)
			&& IsFiniteInRange(Scalar, Limits.MinUIScale, Limits.MaxUIScale))
		{
			Output.UIScale = Scalar;
		}
		break;
	case EField::Locale:
	{
		EWoTShippedLocale Locale = Limits.Defaults.Locale;
		if (ParseLocale(Value, Locale) && LocaleSet.Contains(Locale))
		{
			Output.Locale = Locale;
		}
		break;
	}
	case EField::ReducedMotion:
		if (ParseBool(Value, Flag)) Output.bReducedMotion = Flag;
		break;
	case EField::HighContrast:
		if (ParseBool(Value, Flag)) Output.bHighContrast = Flag;
		break;
	case EField::ScreenReader:
		if (ParseBool(Value, Flag)) Output.bScreenReader = Flag;
		break;
	case EField::BootstrapVolume:
		if (ParseFloat(Value, Scalar) && IsFiniteInRange(Scalar,
			Limits.MinBootstrapVolume, Limits.MaxBootstrapVolume))
		{
			Output.BootstrapVolume = Scalar;
		}
		break;
	case EField::InputSensitivity:
		if (ParseFloat(Value, Scalar) && IsFiniteInRange(Scalar,
			Limits.MinInputSensitivity, Limits.MaxInputSensitivity))
		{
			Output.InputSensitivity = Scalar;
		}
		break;
	case EField::Count:
		break;
	}
}

FWoTBootstrapStatus AuthorityError(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(EarlyUIError(
		EWoTBootstrapError::CorruptInstallation, Message, TEXT("EarlyUIAuthority")));
}
} // namespace

bool FWoTEarlyUIV1::operator==(const FWoTEarlyUIV1& Other) const
{
	return Width == Other.Width && Height == Other.Height
		&& WindowMode == Other.WindowMode && UIScale == Other.UIScale
		&& Locale == Other.Locale && bReducedMotion == Other.bReducedMotion
		&& bHighContrast == Other.bHighContrast
		&& bScreenReader == Other.bScreenReader
		&& BootstrapVolume == Other.BootstrapVolume
		&& InputSensitivity == Other.InputSensitivity;
}

FWoTEarlyUILimits FWoTEarlyUILimits::Production()
{
	return FWoTEarlyUILimits{};
}

bool FWoTInventoryLocaleSet::Contains(EWoTShippedLocale Locale) const
{
	return Locales.Contains(Locale);
}

FWoTInventoryLocaleSet FWoTInventoryLocaleSet::Production()
{
	FWoTInventoryLocaleSet Result;
	Result.Locales = {
		EWoTShippedLocale::EnUS, EWoTShippedLocale::FrFR,
		EWoTShippedLocale::DeDE, EWoTShippedLocale::EsES,
		EWoTShippedLocale::ItIT,
	};
	return Result;
}

TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> ParseEarlyUIV1(
	TConstArrayView<uint8> Bytes, const FWoTEarlyUILimits& Limits,
	const FWoTInventoryLocaleSet& LocaleSet)
{
	using FResult = TWoTBootstrapResult<FWoTAuthenticatedEarlyUI>;
	if (!LimitsAreValid(Limits, LocaleSet))
	{
		return FResult::Error(EarlyUIError(EWoTBootstrapError::CorruptInstallation,
			TEXT("signed EarlyUI limits or locale inventory are inconsistent")));
	}

	FWoTEarlyUIV1 Parsed = Limits.Defaults;
	if (Bytes.Num() > static_cast<int32>(Limits.MaxSerializedBytes))
	{
		return FResult::Success(FWoTAuthenticatedEarlyUI(MoveTemp(Parsed)));
	}
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte > 0x7f)
		{
			return FResult::Success(FWoTAuthenticatedEarlyUI(MoveTemp(Parsed)));
		}
	}

	TArray<FAnsiStringView> Lines;
	const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(Bytes.GetData());
	int32 Start = 0;
	for (int32 Index = 0; Index < Bytes.Num(); ++Index)
	{
		if (Data[Index] != '\n')
		{
			continue;
		}
		int32 Length = Index - Start;
		if (Length > 0 && Data[Index - 1] == '\r')
		{
			--Length;
		}
		Lines.Emplace(Data + Start, Length);
		Start = Index + 1;
	}
	// A final unterminated record is torn and deliberately ignored.

	int32 SchemaCount = 0;
	bool bSchemaMatches = false;
	for (FAnsiStringView Line : Lines)
	{
		if (Line.StartsWith(FAnsiStringView("Schema=")))
		{
			++SchemaCount;
			bSchemaMatches = EqualAscii(Line, SchemaLine);
		}
	}
	if (SchemaCount != 1 || !bSchemaMatches)
	{
		return FResult::Success(FWoTAuthenticatedEarlyUI(MoveTemp(Parsed)));
	}

	bool Seen[static_cast<int32>(EField::Count)] = {};
	bool Duplicated[static_cast<int32>(EField::Count)] = {};
	for (FAnsiStringView Line : Lines)
	{
		if (Line.IsEmpty() || Line.StartsWith(FAnsiStringView("Schema=")))
		{
			continue;
		}
		int32 Equals = INDEX_NONE;
		for (int32 Index = 0; Index < Line.Len(); ++Index)
		{
			if (Line[Index] == '=')
			{
				Equals = Index;
				break;
			}
		}
		if (Equals <= 0)
		{
			continue;
		}
		const FAnsiStringView Key(Line.GetData(), Equals);
		const int32 Field = FieldIndex(Key);
		if (Field == INDEX_NONE)
		{
			continue;
		}
		if (Seen[Field])
		{
			Duplicated[Field] = true;
			ResetField(Parsed, Limits.Defaults, Field);
			continue;
		}
		Seen[Field] = true;
		if (!Duplicated[Field])
		{
			ApplyField(Parsed, Limits, LocaleSet, Field,
				FAnsiStringView(Line.GetData() + Equals + 1, Line.Len() - Equals - 1));
		}
	}
	return FResult::Success(FWoTAuthenticatedEarlyUI(MoveTemp(Parsed)));
}

FWoTBootstrapStatus FWoTEarlyUIAuthority::PublishOnce(
	FWoTAuthenticatedEarlyUI&& Value)
{
	FScopeLock Lock(&Guard);
	if (State != EState::Empty || PublishedValue.IsSet()
		|| bNativeProjectionApplied)
	{
		return AuthorityError(TEXT("EarlyUI publication was absent, replayed, or reordered"));
	}
	PublishedValue.Emplace(MoveTemp(Value));
	State = EState::Published;
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus FWoTEarlyUIAuthority::ApplyNativeProjectionOnce(
	const FWoTEarlyUINativeProjectionSink& Sink)
{
	FScopeLock Lock(&Guard);
	if (State != EState::Published || !PublishedValue.IsSet()
		|| bNativeProjectionApplied)
	{
		return AuthorityError(TEXT("EarlyUI native projection was absent, replayed, or reordered"));
	}
	// Invalidate before calling the fallible narrow sink. The authority lock
	// remains held so a concurrent take cannot observe an in-flight or failed
	// projection as successful.
	bNativeProjectionApplied = true;
	const FWoTEarlyUIV1& Projection = PublishedValue->Get();
	FWoTBootstrapStatus Applied = Sink.Apply(
		Projection.Width, Projection.Height, Projection.WindowMode);
	if (Applied.IsError())
	{
		PublishedValue.Reset();
		State = EState::Shutdown;
	}
	return Applied;
}

TWoTBootstrapResult<FWoTAuthenticatedEarlyUI>
FWoTEarlyUIAuthority::TakeOnce()
{
	using FResult = TWoTBootstrapResult<FWoTAuthenticatedEarlyUI>;
	FScopeLock Lock(&Guard);
	if (State != EState::Published || !PublishedValue.IsSet()
		|| !bNativeProjectionApplied)
	{
		return FResult::Error(EarlyUIError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("EarlyUI take was absent, replayed, or preceded native projection"),
			TEXT("EarlyUIAuthority")));
	}
	FWoTAuthenticatedEarlyUI Result(MoveTemp(PublishedValue.GetValue()));
	PublishedValue.Reset();
	State = EState::Taken;
	return FResult::Success(MoveTemp(Result));
}

FWoTBootstrapStatus FWoTEarlyUIAuthority::Shutdown()
{
	FScopeLock Lock(&Guard);
	if (State == EState::Shutdown)
	{
		return AuthorityError(TEXT("EarlyUI authority shutdown was replayed"));
	}
	PublishedValue.Reset();
	State = EState::Shutdown;
	bNativeProjectionApplied = false;
	return FWoTBootstrapStatus::Success();
}

#if WITH_AUTOMATION_TESTS
void FWoTEarlyUIAuthority::ResetForTesting()
{
	FScopeLock Lock(&Guard);
	PublishedValue.Reset();
	State = EState::Empty;
	bNativeProjectionApplied = false;
}
#endif

FWoTEarlyUIAuthority& GetEarlyUIAuthority()
{
	static FWoTEarlyUIAuthority Authority;
	return Authority;
}
