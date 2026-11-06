#include "WoTReleaseNotes.h"

namespace WoTReleaseNotes
{
	namespace
	{
		constexpr TCHAR ReleaseNotesFormat[] = TEXT("wot-release-notes-v1");

		TWoTBootstrapResult<bool> InvalidReleaseNotes(
			EWoTBootstrapError Code, const TCHAR* Message)
		{
			return TWoTBootstrapResult<bool>::Error(
				MakeWoTBootstrapError(Code, Message));
		}

		bool IsReleaseNotesWhitespace(TCHAR Character)
		{
			const uint32 CodePoint = static_cast<uint32>(Character);
			return (CodePoint >= 0x0009 && CodePoint <= 0x000d)
				|| (CodePoint >= 0x001c && CodePoint <= 0x0020)
				|| CodePoint == 0x0085 || CodePoint == 0x00a0
				|| CodePoint == 0x1680
				|| (CodePoint >= 0x2000 && CodePoint <= 0x200a)
				|| CodePoint == 0x2028 || CodePoint == 0x2029
				|| CodePoint == 0x202f || CodePoint == 0x205f
				|| CodePoint == 0x3000;
		}

		bool HasTrailingWhitespace(const FString& Text)
		{
			if (Text.IsEmpty()
				|| IsReleaseNotesWhitespace(Text[Text.Len() - 1]))
			{
				return true;
			}
			for (int32 Index = 1; Index < Text.Len(); ++Index)
			{
				if (Text[Index] == TEXT('\n')
					&& Text[Index - 1] != TEXT('\n')
					&& IsReleaseNotesWhitespace(Text[Index - 1]))
				{
					return true;
				}
			}
			return false;
		}
	}

	TWoTBootstrapResult<bool> ParseAndSelect(TConstArrayView<uint8> Bytes,
		uint64 ReleaseSequence, FString& OutText)
	{
		using FResult = TWoTBootstrapResult<bool>;
		OutText.Reset();
		FWoTCanonicalJsonLimits Limits;
		Limits.MaxBytes = MaximumFileBytes;
		Limits.MaxDepth = 3;
		Limits.MaxMembers = MaximumEntries;
		Limits.MaxStringBytes = MaximumEntryBytes;
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
			ParseCanonicalJson(Bytes, Limits);
		if (Parsed.IsError())
		{
			return FResult::Error(MoveTemp(Parsed).GetError());
		}

		const FWoTCanonicalJsonValue& Root = Parsed.GetValue();
		static const TCHAR* const RootFields[] = {
			TEXT("format"), TEXT("notes")};
		if (TOptional<FWoTBootstrapError> Error = RequireExactMembers(
				Root, RootFields, TEXT("release notes")); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		FString Format;
		if (TOptional<FWoTBootstrapError> Error = ReadStringField(
				Root, TEXT("format"), TEXT("release notes"), Format);
			Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (!Format.Equals(ReleaseNotesFormat, ESearchCase::CaseSensitive))
		{
			return InvalidReleaseNotes(EWoTBootstrapError::UnsupportedProtocol,
				TEXT("release notes format is unsupported"));
		}
		const FWoTCanonicalJsonValue* Notes = Root.FindMember(TEXT("notes"));
		if (Notes == nullptr || Notes->GetKind() != EWoTCanonicalJsonKind::Array)
		{
			return InvalidReleaseNotes(EWoTBootstrapError::UnknownField,
				TEXT("release notes notes field is not an array"));
		}
		if (Notes->Num() > MaximumEntries)
		{
			return InvalidReleaseNotes(EWoTBootstrapError::SizeLimit,
				TEXT("release notes has too many entries"));
		}

		uint64 PreviousSequence = 0;
		FString SelectedText;
		for (int32 Index = 0; Index < Notes->Num(); ++Index)
		{
			const FWoTCanonicalJsonValue& Entry = Notes->GetElement(Index);
			static const TCHAR* const EntryFields[] = {
				TEXT("release_sequence"), TEXT("text")};
			if (TOptional<FWoTBootstrapError> Error = RequireExactMembers(
					Entry, EntryFields, TEXT("release notes entry")); Error.IsSet())
			{
				return FResult::Error(MoveTemp(Error.GetValue()));
			}
			uint64 Sequence = 0;
			if (TOptional<FWoTBootstrapError> Error = ReadUnsignedField(
					Entry, TEXT("release_sequence"), TEXT("release notes entry"),
					Sequence); Error.IsSet())
			{
				return FResult::Error(MoveTemp(Error.GetValue()));
			}
			if (Sequence == 0 || Sequence <= PreviousSequence)
			{
				return InvalidReleaseNotes(EWoTBootstrapError::InvalidNumber,
					TEXT("release note sequences must be positive, unique and increasing"));
			}
			FString Text;
			if (TOptional<FWoTBootstrapError> Error = ReadStringField(
					Entry, TEXT("text"), TEXT("release notes entry"), Text);
				Error.IsSet())
			{
				return FResult::Error(MoveTemp(Error.GetValue()));
			}
			FTCHARToUTF8 Utf8(*Text);
			if (Text.IsEmpty() || Text.Contains(TEXT("\r"), ESearchCase::CaseSensitive)
				|| HasTrailingWhitespace(Text)
				|| Utf8.Length() > static_cast<int32>(MaximumEntryBytes))
			{
				return InvalidReleaseNotes(EWoTBootstrapError::NonCanonicalJson,
					TEXT("release note text is not normalized"));
			}
			if (Sequence == ReleaseSequence)
			{
				SelectedText = MoveTemp(Text);
			}
			PreviousSequence = Sequence;
		}
		OutText = MoveTemp(SelectedText);
		return FResult::Success(!OutText.IsEmpty());
	}

}
