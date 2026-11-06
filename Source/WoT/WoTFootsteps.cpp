#include "WoTFootsteps.h"
#include "WoTRandom.h"

namespace WoTFootsteps
{

EWoTSurface SurfaceFromTextureName(const FString& UpperName)
{
	// ConvertTextureName (GenericTextureHelper.uc:257-327). Longest specials
	// first (DIRTPEBB/DIRTLEAV shadow DIRT), then the 4-char prefixes; RUG is a
	// 3-char rule; 'sDav' caps to SDAV. Unrecognized -> None (callers Stone it,
	// :445-449).
	if (UpperName.StartsWith(TEXT("DIRTPEBB"))) { return EWoTSurface::Gravel; }
	if (UpperName.StartsWith(TEXT("DIRTLEAV"))) { return EWoTSurface::Leaves; }
	if (UpperName.StartsWith(TEXT("RUG"))) { return EWoTSurface::Rug; }
	struct FPrefix { const TCHAR* P; EWoTSurface S; };
	static const FPrefix Prefixes[] = {
	    {TEXT("DECK"), EWoTSurface::Deck},
	    {TEXT("DIRT"), EWoTSurface::Dirt},
	    {TEXT("PATH"), EWoTSurface::Dirt},
	    {TEXT("SILT"), EWoTSurface::Dirt},
	    {TEXT("FABR"), EWoTSurface::Rug},
	    {TEXT("TPST"), EWoTSurface::Rug},
	    {TEXT("GRAS"), EWoTSurface::Grass},
	    {TEXT("GRSS"), EWoTSurface::Grass},
	    {TEXT("GRAV"), EWoTSurface::Gravel},
	    {TEXT("LEAF"), EWoTSurface::Leaves},
	    {TEXT("MARB"), EWoTSurface::Marble},
	    {TEXT("TILE"), EWoTSurface::Marble},
	    {TEXT("METL"), EWoTSurface::Metal},
	    {TEXT("MUDD"), EWoTSurface::Mud},
	    {TEXT("WATR"), EWoTSurface::Puddle},
	    {TEXT("SDAV"), EWoTSurface::Stone},
	    {TEXT("STON"), EWoTSurface::Stone},
	    {TEXT("ROCK"), EWoTSurface::Stone},
	    {TEXT("BLOK"), EWoTSurface::Stone},
	    {TEXT("BRIK"), EWoTSurface::Stone},
	    {TEXT("MOSA"), EWoTSurface::Stone},
	    {TEXT("SLTE"), EWoTSurface::Stone},
	    {TEXT("SFLO"), EWoTSurface::Stone},
	    {TEXT("COBB"), EWoTSurface::Stone},
	    {TEXT("WOOD"), EWoTSurface::Wood},
	};
	for (const FPrefix& Row : Prefixes)
	{
		if (UpperName.StartsWith(Row.P))
		{
			return Row.S;
		}
	}
	return EWoTSurface::None;
}

EWoTSurface SurfaceFromTextureIdentity(const FString& TextureIdentity)
{
	if (TextureIdentity.IsEmpty()
		|| TextureIdentity.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		return EWoTSurface::None;
	}
	FString Leaf;
	if (!TextureIdentity.Split(TEXT("."), nullptr, &Leaf,
		ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		Leaf = TextureIdentity;
	}
	return SurfaceFromTextureName(Leaf.ToUpper());
}

FString TextureNameFromMaterialName(const FString& MaterialName)
{
	// Undo the import pipeline's naming: glTF slot = <Pkg>__<Tex><flags>
	// (bsp2obj.py qualkey — '__' never occurs in UE1 names, so the split is
	// unambiguous); map MICs prefix MI_W_/MI_ (import_map.py assign_materials);
	// flag suffixes _M/_T/_Mod/_2S/_U/_Sky appended by bsp2obj flag_suffix.
	FString S = MaterialName;
	if (S.StartsWith(TEXT("MI_W_")))
	{
		S.RightChopInline(5);
	}
	else if (S.StartsWith(TEXT("MI_")))
	{
		S.RightChopInline(3);
	}
	if (S.IsEmpty() || S == TEXT("Sky") || S == TEXT("Missing"))
	{
		return FString();
	}
	const int32 Qual = S.Find(TEXT("__"));
	if (Qual != INDEX_NONE)
	{
		S.RightChopInline(Qual + 2);
	}
	static const TCHAR* FlagTokens[] = {
	    TEXT("_M"), TEXT("_T"), TEXT("_Mod"), TEXT("_2S"), TEXT("_U"), TEXT("_Sky")};
	bool bStripped = true;
	while (bStripped && !S.IsEmpty())
	{
		bStripped = false;
		for (const TCHAR* Token : FlagTokens)
		{
			if (S.EndsWith(Token, ESearchCase::CaseSensitive))
			{
				S.LeftChopInline(FCString::Strlen(Token));
				bStripped = true;
			}
		}
	}
	return S.ToUpper();
}

namespace
{
	TUniquePtr<FWoTFootstepSet>& InstalledFootstepRegistry()
	{
		static TUniquePtr<FWoTFootstepSet> Set;
		return Set;
	}

	const FWoTFootstepSet* ActiveFootstepRegistry()
	{
		if (const TUniquePtr<FWoTFootstepSet>& Installed = InstalledFootstepRegistry())
		{
			return Installed.Get();
		}
		return nullptr;
	}
}

}   // namespace WoTFootsteps

void FWoTFootstepsBuilder::Install(FWoTFootstepSet&& Set)
{
	WoTFootsteps::InstalledFootstepRegistry() =
		MakeUnique<FWoTFootstepSet>(MoveTemp(Set));
}

#if WITH_AUTOMATION_TESTS
void FWoTFootstepsBuilder::Reset()
{
	WoTFootsteps::InstalledFootstepRegistry().Reset();
}
#endif

namespace WoTFootsteps
{

const TArray<FWoTFootstepRow>& Table(FName TableName)
{
	if (const FWoTFootstepSet* Set = ActiveFootstepRegistry())
	{
		if (const TArray<FWoTFootstepRow>* Found = Set->Tables.Find(TableName))
		{
			return *Found;
		}
	}
	static const TArray<FWoTFootstepRow> Empty;
	return Empty;
}

const FWoTFootstepRow* PickRow(FName TableName, EWoTFootstepGroup Group,
                               EWoTSurface Surface)
{
	// LookUpSound (GenericTextureHelper.uc:460-487): collect up to MaxMatches=4
	// rows matching Group AND (Surface or All), pick uniformly (Odds is dead).
	const FWoTFootstepRow* Matches[4];
	int32 NumMatches = 0;
	for (const FWoTFootstepRow& Row : Table(TableName))
	{
		if (Row.Group == Group &&
		    (Row.Surface == Surface || Row.Surface == EWoTSurface::All))
		{
			Matches[NumMatches++] = &Row;
			if (NumMatches == 4)
			{
				break;
			}
		}
	}
	return NumMatches > 0 ? Matches[WoTRandom::RandHelper(NumMatches)] : nullptr;
}

const FWoTFootstepHelper& HelperForSoundTable(FName SoundTableName)
{
	static const FWoTFootstepHelper Empty;
	const FWoTFootstepSet* Set = ActiveFootstepRegistry();
	if (Set == nullptr) return Empty;
	const FName* HelperClass = Set->HelperClassBySoundTable.Find(SoundTableName);
	if (HelperClass != nullptr)
	{
		if (const FWoTFootstepHelper* Found = Set->HelpersByClass.Find(*HelperClass))
		{
			return *Found;
		}
	}
	if (const FWoTFootstepHelper* Generic =
		Set->HelpersByClass.Find(TEXT("GenericTextureHelper"))) return *Generic;
	return Empty;
}

float VolumeMultForSpeed(float Speed, bool bNeverWalkQuiet)
{
	// HandleMovingOnTexture (:600-608): RunningSoundSpeed=200,
	// WalkVolumeMultiplier=0.5; Legion's override never quiets.
	return (!bNeverWalkQuiet && Speed < 200.0f) ? 0.5f : 1.0f;
}

}   // namespace WoTFootsteps
