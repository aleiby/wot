#include "WoTOnboardingHints.h"

#include "WoTAngrealGuide.h"

namespace
{
const TCHAR* const WoTOnboardingHintCounterPrefix = TEXT("counter:");

/** The row Name, when the guide carries it and it counters Row. */
const FWoTAngrealGuideRow* WoTOnboardingBackedCounter(
	const UWoTAngrealGuide& Guide, const FWoTAngrealGuideRow& Row, FName Name)
{
	const FWoTAngrealGuideRow* Counter = Guide.FindByClassName(Name);
	return Counter && Counter->Counters.Contains(Row.ClassName)
		? Counter : nullptr;
}

FString WoTOnboardingTitleOf(const FWoTAngrealGuideRow& Row,
	TFunctionRef<FString(const FWoTAngrealGuideRow&)> ResolveTitle)
{
	FString Title = ResolveTitle(Row);
	return Title.IsEmpty()
		? WoTOnboardingHints::SpacedClassName(Row.ClassName) : Title;
}
} // namespace

FString WoTOnboardingHints::SpacedClassName(FName ClassName)
{
	if (ClassName.IsNone())
	{
		return FString();
	}
	FString Name = ClassName.ToString();
	// Registry affixes, not display text: the angreal classes carry an
	// AngrealInv prefix, the traps an Inventory suffix.
	Name.RemoveFromStart(TEXT("AngrealInv"));
	Name.RemoveFromEnd(TEXT("Inventory"));
	FString Spaced;
	Spaced.Reserve(Name.Len() + 4);
	for (int32 Index = 0; Index < Name.Len(); ++Index)
	{
		const TCHAR Char = Name[Index];
		if (Index > 0 && FChar::IsUpper(Char) && FChar::IsLower(Name[Index - 1]))
		{
			Spaced.AppendChar(TEXT(' '));
		}
		Spaced.AppendChar(Char);
	}
	return Spaced;
}

const FWoTAngrealGuideRow* WoTOnboardingHints::HintCounter(
	const UWoTAngrealGuide& Guide, const FWoTAngrealGuideRow& Row)
{
	const FString& Text = Row.HintText;
	const FString Token = FString(TEXT("{")) + WoTOnboardingHintCounterPrefix;
	const int32 Start = Text.Find(Token, ESearchCase::CaseSensitive);
	if (Start == INDEX_NONE)
	{
		return nullptr;
	}
	const int32 NameStart = Start + Token.Len();
	const int32 End = Text.Find(TEXT("}"), ESearchCase::CaseSensitive,
		ESearchDir::FromStart, NameStart);
	if (End == INDEX_NONE || End == NameStart)
	{
		return nullptr;
	}
	return WoTOnboardingBackedCounter(Guide, Row,
		FName(*Text.Mid(NameStart, End - NameStart)));
}

FString WoTOnboardingHints::ResolveHintLine(const UWoTAngrealGuide& Guide,
	const FWoTAngrealGuideRow& Row,
	TFunctionRef<FString(const FWoTAngrealGuideRow&)> ResolveTitle)
{
	const FString& Template = Row.HintText;
	FString Line;
	Line.Reserve(Template.Len() + 16);
	int32 Index = 0;
	while (Index < Template.Len())
	{
		const int32 Open = Template.Find(TEXT("{"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, Index);
		if (Open == INDEX_NONE)
		{
			Line += Template.Mid(Index);
			break;
		}
		const int32 Close = Template.Find(TEXT("}"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, Open + 1);
		if (Close == INDEX_NONE)
		{
			// An unterminated brace is not ours to guess at: no line.
			return FString();
		}
		Line += Template.Mid(Index, Open - Index);
		const FString Inner = Template.Mid(Open + 1, Close - Open - 1);
		if (Inner == TEXT("title"))
		{
			Line += WoTOnboardingTitleOf(Row, ResolveTitle);
		}
		else if (Inner.StartsWith(WoTOnboardingHintCounterPrefix,
			ESearchCase::CaseSensitive))
		{
			const FWoTAngrealGuideRow* Counter = WoTOnboardingBackedCounter(
				Guide, Row, FName(*Inner.Mid(FCString::Strlen(
					WoTOnboardingHintCounterPrefix))));
			if (!Counter)
			{
				// The template names a counter the data does not back: no
				// line, rather than advice the game does not support.
				return FString();
			}
			Line += WoTOnboardingTitleOf(*Counter, ResolveTitle);
		}
		else
		{
			return FString();
		}
		Index = Close + 1;
	}
	return Line;
}
