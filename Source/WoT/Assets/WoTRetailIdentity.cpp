#include "WoTRetailIdentity.h"

#include "Misc/Parse.h"

#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTRuntimeRegistry.h"

namespace
{
FString WoTRetailStripReferenceSuffix(const FString& Component)
{
	for (const TCHAR* Marker : {TEXT("#export-"), TEXT("#import-")})
	{
		const int32 At = Component.Find(Marker, ESearchCase::CaseSensitive,
			ESearchDir::FromEnd);
		if (At <= 0) continue;
		const FString Ordinal = Component.Mid(At + FCString::Strlen(Marker));
		if (!Ordinal.IsEmpty() && Ordinal.IsNumeric()) return Component.Left(At);
	}
	return Component;
}

}

bool WoTRetailRegistryHolds(EWoTNeutralResourceKind Kind, const FString& LogicalId)
{
	if (!IsWoTPayloadBootstrapActive() || !IsInGameThread()) return false;
	return FWoTRuntimeRegistry::Get().FindDescriptor(
		FWoTNeutralResourceId{LogicalId, Kind}).IsSet();
}

bool WoTParseRetailObjectPath(FStringView ObjectPath, FWoTRetailObjectPath& Out)
{
	Out = FWoTRetailObjectPath();
	TArray<FString> Parts;
	FString(ObjectPath).ParseIntoArray(Parts, TEXT("."), false);
	if (Parts.Num() < 2) return false;
	for (const FString& Part : Parts)
	{
		if (Part.IsEmpty()) return false;
	}
	Out.Package = Parts[0];
	for (int32 Index = 1; Index + 1 < Parts.Num(); ++Index)
	{
		Out.Groups.Add(Parts[Index]);
	}
	Out.Name = WoTRetailStripReferenceSuffix(Parts.Last());
	return !Out.Name.IsEmpty();
}

FString WoTRetailPlainLogicalBase(const FWoTRetailObjectPath& Path)
{
	return Path.Package + TEXT("__") + Path.Name;
}

FString WoTRetailEncodeGroup(FStringView Group)
{
	// Reversible logical-id spelling (Tools/dist/retail_identity.py
	// encode_group): every UTF-8 byte outside [A-Za-z0-9_] becomes "-XX".
	const FTCHARToUTF8 Utf8(Group.GetData(), Group.Len());
	FString Encoded;
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
		const bool bPlain = (Byte >= 'A' && Byte <= 'Z') || (Byte >= 'a' && Byte <= 'z')
			|| (Byte >= '0' && Byte <= '9') || Byte == '_';
		if (bPlain) Encoded.AppendChar(static_cast<TCHAR>(Byte));
		else Encoded += FString::Printf(TEXT("-%02X"), Byte);
	}
	return Encoded;
}

FString WoTRetailExactLogicalBase(const FWoTRetailObjectPath& Path)
{
	FString Outer = Path.Package;
	for (const FString& Group : Path.Groups)
	{
		Outer += TEXT(".");
		Outer += WoTRetailEncodeGroup(Group);
	}
	return Outer + TEXT("__") + Path.Name;
}

FString WoTRetailLogicalIdPrefix(EWoTNeutralResourceKind Kind)
{
	switch (Kind)
	{
	case EWoTNeutralResourceKind::Texture: return TEXT("cache/texture/");
	case EWoTNeutralResourceKind::Sound: return TEXT("cache/sound/");
	case EWoTNeutralResourceKind::Procedural: return TEXT("cache/procedural/");
	case EWoTNeutralResourceKind::VertexMesh: return TEXT("cache/mesh/");
	case EWoTNeutralResourceKind::Animation: return TEXT("cache/animation/");
	case EWoTNeutralResourceKind::Font: return TEXT("cache/font/");
	default: return FString();
	}
}

FString WoTRetailResolveLogicalBase(EWoTNeutralResourceKind Kind,
	const FWoTRetailObjectPath& Path,
	TFunctionRef<bool(const FString& LogicalId)> Exists)
{
	if (!Path.Groups.IsEmpty())
	{
		const FString Exact = WoTRetailExactLogicalBase(Path);
		if (Exists(WoTRetailLogicalIdPrefix(Kind) + Exact)) return Exact;
	}
	return WoTRetailPlainLogicalBase(Path);
}

FString WoTRetailResolveLogicalBase(EWoTNeutralResourceKind Kind,
	const FWoTRetailObjectPath& Path)
{
	return WoTRetailResolveLogicalBase(Kind, Path,
		[Kind](const FString& LogicalId)
		{
			return WoTRetailRegistryHolds(Kind, LogicalId);
		});
}

FString WoTRetailAssetSubPath(EWoTNeutralResourceKind Kind,
	const FWoTRetailObjectPath& Path,
	TFunctionRef<bool(const FString& LogicalId)> Exists)
{
	if (!Path.Groups.IsEmpty()
		&& Exists(WoTRetailLogicalIdPrefix(Kind) + WoTRetailExactLogicalBase(Path)))
	{
		FString SubPath = Path.Package;
		for (const FString& Group : Path.Groups)
		{
			SubPath += TEXT("/");
			SubPath += Group;
		}
		return SubPath + TEXT("/") + Path.Name;
	}
	return Path.Package + TEXT("/") + Path.Name;
}

FString WoTRetailAssetSubPath(EWoTNeutralResourceKind Kind,
	const FWoTRetailObjectPath& Path)
{
	return WoTRetailAssetSubPath(Kind, Path,
		[Kind](const FString& LogicalId)
		{
			return WoTRetailRegistryHolds(Kind, LogicalId);
		});
}

FString WoTRetailTextureSubPath(const FWoTRetailObjectPath& Path)
{
	return WoTRetailAssetSubPath(EWoTNeutralResourceKind::Texture, Path,
		[&Path](const FString& TextureLogicalId)
		{
			return WoTRetailRegistryHolds(EWoTNeutralResourceKind::Texture,
					TextureLogicalId)
				|| WoTRetailRegistryHolds(EWoTNeutralResourceKind::Procedural,
					WoTRetailLogicalIdPrefix(EWoTNeutralResourceKind::Procedural)
						+ WoTRetailExactLogicalBase(Path));
		});
}

bool WoTRetailLogicalBaseFromSubPath(FStringView SubPath, FString& OutBase)
{
	OutBase.Reset();
	TArray<FString> Parts;
	FString(SubPath).ParseIntoArray(Parts, TEXT("/"), false);
	if (Parts.Num() < 2) return false;
	// Package and group names never contain "__" (the base splits at its FIRST
	// "__"); retail OBJECT names may (AmbientA.Water.Dive__2).
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		const FString& Part = Parts[Index];
		if (Part.IsEmpty() || Part.Contains(TEXT("."))
			|| (Index + 1 < Parts.Num() && Part.Contains(TEXT("__")))) return false;
	}
	FWoTRetailObjectPath Path;
	Path.Package = Parts[0];
	for (int32 Index = 1; Index + 1 < Parts.Num(); ++Index)
	{
		Path.Groups.Add(Parts[Index]);
	}
	Path.Name = Parts.Last();
	OutBase = WoTRetailExactLogicalBase(Path);
	return true;
}

bool WoTRetailIdentityEquals(FStringView Left, FStringView Right)
{
	return Left.Equals(Right, ESearchCase::IgnoreCase);
}

bool WoTRetailSourceExportMatches(FStringView CachedSourceExport,
	FStringView ReferencePath)
{
	if (CachedSourceExport.IsEmpty() || ReferencePath.IsEmpty()) return false;
	const FString Cached(CachedSourceExport);
	int32 Dot = INDEX_NONE;
	const FString Stripped = Cached.FindLastChar(TEXT('.'), Dot)
		? Cached.Left(Dot + 1) + WoTRetailStripReferenceSuffix(Cached.Mid(Dot + 1))
		: Cached;
	return WoTRetailIdentityEquals(Stripped, ReferencePath);
}

FString WoTRetailDerivedMaterialSuffix(const FWoTRetailObjectPath& Path,
	bool bGrouped)
{
	if (!bGrouped || Path.Groups.IsEmpty())
	{
		return WoTRetailPlainLogicalBase(Path);
	}
	return WoTRetailExactLogicalBase(Path).Replace(TEXT("."), TEXT("-2E"),
		ESearchCase::CaseSensitive);
}

bool WoTRetailTextureSubPathFromDerivedSuffix(FStringView Suffix,
	FString& OutSubPath)
{
	OutSubPath.Reset();
	const FString Text(Suffix);
	const int32 Separator = Text.Find(TEXT("__"), ESearchCase::CaseSensitive);
	if (Separator <= 0 || Separator + 2 >= Text.Len())
	{
		return false;
	}
	const FString Name = Text.Mid(Separator + 2);
	TArray<FString> Outer;
	Text.Left(Separator).ParseIntoArray(Outer, TEXT("-2E"), false);
	if (Outer.IsEmpty())
	{
		return false;
	}
	for (const TCHAR Character : Outer[0])
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
		{
			return false;   // package names are plain identifiers
		}
	}
	OutSubPath = Outer[0];
	for (int32 Index = 1; Index < Outer.Num(); ++Index)
	{
		// Decode the WoTRetailEncodeGroup spelling ("-XX" per UTF-8 byte).
		const FString& Encoded = Outer[Index];
		TArray<uint8> Utf8;
		for (int32 Offset = 0; Offset < Encoded.Len();)
		{
			const TCHAR Character = Encoded[Offset];
			if (Character == TEXT('-'))
			{
				if (Offset + 2 >= Encoded.Len())
				{
					return false;   // a truncated escape
				}
				const TCHAR High = Encoded[Offset + 1];
				const TCHAR Low = Encoded[Offset + 2];
				if (!FChar::IsHexDigit(High) || !FChar::IsHexDigit(Low))
				{
					return false;
				}
				Utf8.Add(static_cast<uint8>(FParse::HexDigit(High) * 16 + FParse::HexDigit(Low)));
				Offset += 3;
			}
			else if (FChar::IsAlnum(Character) || Character == TEXT('_'))
			{
				Utf8.Add(static_cast<uint8>(Character));
				++Offset;
			}
			else
			{
				return false;
			}
		}
		if (Utf8.IsEmpty())
		{
			return false;
		}
		const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(Utf8.GetData()),
			Utf8.Num());
		const FString Group(Decoded.Length(), Decoded.Get());
		if (Group.IsEmpty() || Group.Contains(TEXT("/")) || Group.Contains(TEXT(".")))
		{
			return false;
		}
		OutSubPath += TEXT("/");
		OutSubPath += Group;
	}
	OutSubPath += TEXT("/");
	OutSubPath += Name;
	return true;
}

FString WoTRetailObjectNameSafe(FStringView LogicalBase)
{
	FString Safe(LogicalBase);
	for (TCHAR& Character : Safe)
	{
		const bool bAllowed = (Character >= TEXT('A') && Character <= TEXT('Z'))
			|| (Character >= TEXT('a') && Character <= TEXT('z'))
			|| (Character >= TEXT('0') && Character <= TEXT('9'))
			|| Character == TEXT('_');
		if (!bAllowed) Character = TEXT('_');
	}
	return Safe;
}
