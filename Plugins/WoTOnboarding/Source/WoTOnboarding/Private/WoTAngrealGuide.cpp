#include "WoTAngrealGuide.h"

#include "Misc/PackageName.h"

#if WITH_EDITOR
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogWoTOnboarding, Log, All);

const FWoTAngrealGuideRow* UWoTAngrealGuide::FindByClassName(
	FName ClassName) const
{
	if (ClassName.IsNone())
	{
		return nullptr;
	}
	return Rows.FindByPredicate([ClassName](const FWoTAngrealGuideRow& Row)
	{
		return Row.ClassName == ClassName;
	});
}

const FWoTAngrealGuideRow* UWoTAngrealGuide::FindByTitleIn(
	const FString& Text,
	TFunctionRef<FString(const FWoTAngrealGuideRow&)> ResolveTitle) const
{
	// Longest contained resolved title wins: a recap line naming one item may
	// contain another row's shorter title as a substring, and the longer
	// match is the one the line actually names. Ties keep the earliest row,
	// so the result is deterministic in Rows order.
	const FWoTAngrealGuideRow* Best = nullptr;
	int32 BestLength = 0;
	for (const FWoTAngrealGuideRow& Row : Rows)
	{
		const FString Title = ResolveTitle(Row);
		// An empty title (unresolved retail text) would "match" any text;
		// such a row is simply unresolvable.
		if (Title.Len() <= BestLength || Title.Len() > Text.Len()
			|| !Text.Contains(Title, ESearchCase::IgnoreCase))
		{
			continue;
		}
		Best = &Row;
		BestLength = Title.Len();
	}
	return Best;
}

TArray<const FWoTAngrealGuideRow*> UWoTAngrealGuide::CounteredBy(
	FName ClassName) const
{
	TArray<const FWoTAngrealGuideRow*> Counters;
	if (ClassName.IsNone())
	{
		return Counters;
	}
	for (const FWoTAngrealGuideRow& Row : Rows)
	{
		if (Row.Counters.Contains(ClassName))
		{
			Counters.Add(&Row);
		}
	}
	return Counters;
}

UWoTAngrealGuide* UWoTAngrealGuide::Load()
{
	// A tree whose import pipeline has not run carries no package at all;
	// answer null quietly then (LoadObject would log load errors) so callers
	// can treat "no guide" as the ordinary degraded state.
	const FString PackageName =
		FPackageName::ObjectPathToPackageName(FString(AssetPath()));
	if (!FPackageName::DoesPackageExist(PackageName))
	{
		UE_LOG(LogWoTOnboarding, Log,
			TEXT("Angreal guide package %s is not present; the onboarding "
			     "aids run without a guide until the importer publishes it."),
			*PackageName);
		return nullptr;
	}
	return LoadObject<UWoTAngrealGuide>(nullptr, AssetPath());
}

const TCHAR* UWoTAngrealGuide::AssetPath()
{
	return TEXT("/Game/WoT/UI/Guide/DA_WoTAngrealGuide.DA_WoTAngrealGuide");
}

#if WITH_EDITOR

namespace
{
FName WoTGuideNameFromString(const FString& Value)
{
	return Value.IsEmpty() ? NAME_None : FName(*Value);
}

FString WoTGuideStringFromName(FName Value)
{
	return Value.IsNone() ? FString() : Value.ToString();
}

bool WoTGuideReadString(const FJsonObject& Object, const TCHAR* Key,
	FString& OutValue, FString& OutError)
{
	if (!Object.TryGetStringField(Key, OutValue))
	{
		OutError = FString::Printf(TEXT("missing or non-string %s"), Key);
		return false;
	}
	return true;
}

bool WoTGuideReadNames(const FJsonObject& Object, const TCHAR* Key,
	TArray<FName>& OutValues, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.TryGetArrayField(Key, Values))
	{
		OutError = FString::Printf(TEXT("missing or non-array %s"), Key);
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Text;
		if (!Value.IsValid() || !Value->TryGetString(Text) || Text.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("%s entries must be non-empty strings"), Key);
			return false;
		}
		OutValues.Add(FName(*Text));
	}
	return true;
}

bool WoTGuideReadCount(const FJsonObject& Object, const TCHAR* Key,
	int32& OutValue, FString& OutError)
{
	double Number = 0.0;
	if (!Object.TryGetNumberField(Key, Number)
		|| Number < 0.0 || Number != FMath::TruncToDouble(Number))
	{
		OutError = FString::Printf(
			TEXT("%s must be a non-negative integer"), Key);
		return false;
	}
	OutValue = static_cast<int32>(Number);
	return true;
}

bool WoTGuideRowFromJson(const FJsonObject& Object,
	FWoTAngrealGuideRow& OutRow, FString& OutError)
{
	FString ClassName, CppClass, Rarity;
	if (!WoTGuideReadString(Object, TEXT("class_name"), ClassName, OutError)
		|| !WoTGuideReadString(Object, TEXT("cpp_class"), CppClass, OutError)
		|| !WoTGuideReadString(Object, TEXT("rarity"), Rarity, OutError)
		|| !WoTGuideReadString(Object, TEXT("hint_text"), OutRow.HintText,
			OutError)
		|| !WoTGuideReadNames(Object, TEXT("elements"), OutRow.Elements,
			OutError)
		|| !WoTGuideReadNames(Object, TEXT("roles"), OutRow.Roles, OutError)
		|| !WoTGuideReadNames(Object, TEXT("counters"), OutRow.Counters,
			OutError)
		|| !WoTGuideReadCount(Object, TEXT("max_initial_charges"),
			OutRow.MaxInitialCharges, OutError)
		|| !WoTGuideReadCount(Object, TEXT("max_charges"), OutRow.MaxCharges,
			OutError))
	{
		return false;
	}
	if (ClassName.IsEmpty())
	{
		OutError = TEXT("empty class_name");
		return false;
	}
	OutRow.ClassName = FName(*ClassName);
	OutRow.CppClass = WoTGuideNameFromString(CppClass);
	OutRow.Rarity = WoTGuideNameFromString(Rarity);
	return true;
}
} // namespace

FString UWoTAngrealGuide::ImportRowsFromJson(const FString& PayloadJson)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(PayloadJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return TEXT("payload is not a JSON object");
	}
	const TArray<TSharedPtr<FJsonValue>>* RowValues = nullptr;
	if (!Root->TryGetArrayField(TEXT("rows"), RowValues))
	{
		return TEXT("payload carries no \"rows\" array");
	}
	// Build aside and assign only on full success, so a failed import never
	// half-clobbers the asset.
	TArray<FWoTAngrealGuideRow> Imported;
	TSet<FName> Seen;
	for (int32 Index = 0; Index < RowValues->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* RowObject = nullptr;
		if (!(*RowValues)[Index].IsValid()
			|| !(*RowValues)[Index]->TryGetObject(RowObject))
		{
			return FString::Printf(TEXT("rows[%d] is not an object"), Index);
		}
		FWoTAngrealGuideRow Row;
		FString Error;
		if (!WoTGuideRowFromJson(**RowObject, Row, Error))
		{
			return FString::Printf(TEXT("rows[%d]: %s"), Index, *Error);
		}
		bool bAlreadySeen = false;
		Seen.Add(Row.ClassName, &bAlreadySeen);
		if (bAlreadySeen)
		{
			return FString::Printf(TEXT("rows[%d]: duplicate class_name %s"),
				Index, *Row.ClassName.ToString());
		}
		Imported.Add(MoveTemp(Row));
	}
	Rows = MoveTemp(Imported);
	return FString();
}

FString UWoTAngrealGuide::ExportRowsAsJson() const
{
	TArray<TSharedPtr<FJsonValue>> RowValues;
	for (const FWoTAngrealGuideRow& Row : Rows)
	{
		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("class_name"),
			WoTGuideStringFromName(Row.ClassName));
		Object->SetStringField(TEXT("cpp_class"),
			WoTGuideStringFromName(Row.CppClass));
		TArray<TSharedPtr<FJsonValue>> Elements;
		for (const FName& Element : Row.Elements)
		{
			Elements.Add(MakeShared<FJsonValueString>(Element.ToString()));
		}
		Object->SetArrayField(TEXT("elements"), Elements);
		Object->SetStringField(TEXT("rarity"),
			WoTGuideStringFromName(Row.Rarity));
		TArray<TSharedPtr<FJsonValue>> Roles;
		for (const FName& Role : Row.Roles)
		{
			Roles.Add(MakeShared<FJsonValueString>(Role.ToString()));
		}
		Object->SetArrayField(TEXT("roles"), Roles);
		Object->SetNumberField(TEXT("max_initial_charges"),
			Row.MaxInitialCharges);
		Object->SetNumberField(TEXT("max_charges"), Row.MaxCharges);
		TArray<TSharedPtr<FJsonValue>> Counters;
		for (const FName& Counter : Row.Counters)
		{
			Counters.Add(MakeShared<FJsonValueString>(Counter.ToString()));
		}
		Object->SetArrayField(TEXT("counters"), Counters);
		Object->SetStringField(TEXT("hint_text"), Row.HintText);
		RowValues.Add(MakeShared<FJsonValueObject>(Object));
	}
	const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("rows"), RowValues);
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
		Writer = TJsonWriterFactory<TCHAR,
			TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Out;
}

#endif // WITH_EDITOR
