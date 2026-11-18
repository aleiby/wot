#include "WoTFrontEnd.h"

#include "Misc/Paths.h"
#include "WoTBootstrapSubsystem.h"

namespace
{
	FString WoTFrontEnd_URLWithoutOptions(const FString& MapPackage)
	{
		int32 CutIndex = MapPackage.Len();
		int32 MarkerIndex = INDEX_NONE;
		if (MapPackage.FindChar(TEXT('?'), MarkerIndex))
		{
			CutIndex = FMath::Min(CutIndex, MarkerIndex);
		}
		if (MapPackage.FindChar(TEXT('#'), MarkerIndex))
		{
			CutIndex = FMath::Min(CutIndex, MarkerIndex);
		}
		return MapPackage.Left(CutIndex);
	}

	FString WoTFrontEnd_AppendOption(const FString& URL, const FString& Option)
	{
		return URL + TEXT("?") + Option;
	}

	bool WoTFrontEnd_IsASCIIAlphaNumeric(TCHAR Char)
	{
		return (Char >= TEXT('a') && Char <= TEXT('z'))
			|| (Char >= TEXT('A') && Char <= TEXT('Z'))
			|| (Char >= TEXT('0') && Char <= TEXT('9'));
	}

	FString WoTFrontEnd_FilterPlayerNameCharacters(const FString& In)
	{
		FString Filtered;
		Filtered.Reserve(In.Len());
		for (const TCHAR Char : In)
		{
			if (Char >= 0x20 && Char <= 0x7e
				&& Char != TEXT('?') && Char != TEXT('#')
				&& Char != TEXT('=') && Char != TEXT('"'))
			{
				Filtered.AppendChar(Char);
			}
		}
		return Filtered;
	}

	bool WoTFrontEnd_IsOption(const FString& Field, const TCHAR* ExpectedKey)
	{
		FString Key = Field;
		int32 EqualsIndex = INDEX_NONE;
		if (Key.FindChar(TEXT('='), EqualsIndex))
		{
			Key.LeftInline(EqualsIndex);
		}
		return Key.Equals(ExpectedKey, ESearchCase::IgnoreCase);
	}

	// FURL accepts alternating '?' options and '#' portals, including another
	// option after a portal. Remove player-owned fields from the entire sequence
	// so a fragment such as #Entry?Class=Other cannot override saved choices.
	FString WoTFrontEnd_WithoutPlayerOptions(const FString& URL,
		bool bRemoveSkin)
	{
		int32 FirstMarker = INDEX_NONE;
		for (int32 Index = 0; Index < URL.Len(); ++Index)
		{
			if (URL[Index] == TEXT('?') || URL[Index] == TEXT('#'))
			{
				FirstMarker = Index;
				break;
			}
		}
		if (FirstMarker == INDEX_NONE)
		{
			return URL;
		}

		FString Result = URL.Left(FirstMarker);
		int32 MarkerIndex = FirstMarker;
		while (MarkerIndex < URL.Len())
		{
			const TCHAR Marker = URL[MarkerIndex];
			int32 NextMarker = URL.Len();
			for (int32 Index = MarkerIndex + 1; Index < URL.Len(); ++Index)
			{
				if (URL[Index] == TEXT('?') || URL[Index] == TEXT('#'))
				{
					NextMarker = Index;
					break;
				}
			}

			const FString Field = URL.Mid(
				MarkerIndex + 1, NextMarker - MarkerIndex - 1);
			const bool bPlayerOption = Marker == TEXT('?')
				&& (WoTFrontEnd_IsOption(Field, TEXT("Class"))
					|| (bRemoveSkin && WoTFrontEnd_IsOption(Field, TEXT("Skin"))));
			if (!bPlayerOption)
			{
				Result.AppendChar(Marker);
				Result += Field;
			}
			MarkerIndex = NextMarker;
		}
		return Result;
	}
}

bool FWoTFrontEndModel::CanConstructNormalFrontEnd(
	const UGameInstance* GameInstance)
{
	return UWoTBootstrapSubsystem::IsNormalFrontEndAuthorized(GameInstance);
}

FString FWoTLeaveStateMachine::FrontEndURL()
{
	return FrontEndURLForBuild(WOT_GUARDED_BOOTSTRAP != 0);
}

FString FWoTLeaveStateMachine::FrontEndURLForBuild(bool bGuardedBuild)
{
	return bGuardedBuild
		? TEXT("/Game/WoT/Maps/WoTBootstrap")
		: TEXT("/Game/WoT/Maps/WoTFrontEnd");
}

bool FWoTLeaveStateMachine::Begin(bool bHasNamedSession)
{
	if (State != EWoTLeaveState::Idle)
	{
		return false;
	}
	State = bHasNamedSession
		? EWoTLeaveState::DestroyingSession
		: EWoTLeaveState::ReturningToMenu;
	return true;
}

bool FWoTLeaveStateMachine::CompleteDestroy()
{
	if (State != EWoTLeaveState::DestroyingSession)
	{
		return false;
	}
	State = EWoTLeaveState::ReturningToMenu;
	return true;
}

FString FWoTLeaveStateMachine::TakeReturnURL()
{
	if (State != EWoTLeaveState::ReturningToMenu)
	{
		return FString();
	}
	State = EWoTLeaveState::Idle;
	return FrontEndURL();
}

void FWoTLeaveStateMachine::Reset()
{
	State = EWoTLeaveState::Idle;
}

const TArray<FWoTFactionChoice>& FWoTFrontEndModel::FactionChoices()
{
	// menuPlayer.uc:504-507 plus each faction's shipped TeamDescription.
	static const TArray<FWoTFactionChoice> Choices = {
		{EWoTPlayerFaction::AesSedai, TEXT("WOTPawns.AesSedai"), TEXT("Aes Sedai")},
		{EWoTPlayerFaction::Forsaken, TEXT("WOTPawns.Forsaken"), TEXT("Forsaken")},
		{EWoTPlayerFaction::Hound, TEXT("WOTPawns.Hound"), TEXT("Hound")},
		{EWoTPlayerFaction::Whitecloak, TEXT("WOTPawns.Whitecloak"), TEXT("Whitecloak")}
	};
	return Choices;
}

EWoTPlayerFaction FWoTFrontEndModel::NormalizeFaction(EWoTPlayerFaction Faction)
{
	switch (Faction)
	{
	case EWoTPlayerFaction::AesSedai:
	case EWoTPlayerFaction::Forsaken:
	case EWoTPlayerFaction::Hound:
	case EWoTPlayerFaction::Whitecloak:
		return Faction;
	default:
		return EWoTPlayerFaction::AesSedai;
	}
}

FString FWoTFrontEndModel::ClassOption(EWoTPlayerFaction Faction)
{
	const EWoTPlayerFaction Normalized = NormalizeFaction(Faction);
	for (const FWoTFactionChoice& Choice : FactionChoices())
	{
		if (Choice.Faction == Normalized)
		{
			return Choice.ClassOption;
		}
	}
	return TEXT("WOTPawns.AesSedai");
}

FString FWoTFrontEndModel::TeamDescription(EWoTPlayerFaction Faction)
{
	const EWoTPlayerFaction Normalized = NormalizeFaction(Faction);
	for (const FWoTFactionChoice& Choice : FactionChoices())
	{
		if (Choice.Faction == Normalized)
		{
			return Choice.TeamDescription;
		}
	}
	return TEXT("Aes Sedai");
}

const TArray<FWoTSkinChoice>& FWoTFrontEndModel::SkinChoices()
{
	// menuPlayer.FindSkin scans each <MeshName>Skins package. All four shipped
	// player meshes expose this same retail .int order.
	static const TArray<FWoTSkinChoice> Choices = {
		{EWoTPlayerSkinColor::Black, TEXT("Black")},
		{EWoTPlayerSkinColor::Blue, TEXT("Blue")},
		{EWoTPlayerSkinColor::Green, TEXT("Green")},
		{EWoTPlayerSkinColor::Red, TEXT("Red")},
		{EWoTPlayerSkinColor::Base, TEXT("Base")}
	};
	return Choices;
}

EWoTPlayerSkinColor FWoTFrontEndModel::NormalizeSkinColor(
	EWoTPlayerSkinColor Color)
{
	switch (Color)
	{
	case EWoTPlayerSkinColor::Black:
	case EWoTPlayerSkinColor::Blue:
	case EWoTPlayerSkinColor::Red:
	case EWoTPlayerSkinColor::Green:
	case EWoTPlayerSkinColor::Base:
		return Color;
	default:
		return EWoTPlayerSkinColor::Base;
	}
}

FString FWoTFrontEndModel::SkinOption(EWoTPlayerFaction Faction,
	EWoTPlayerSkinColor Color)
{
	const TCHAR* Package = TEXT("AesSedaiSkins");
	const TCHAR* Prefix = TEXT("Aes");
	switch (NormalizeFaction(Faction))
	{
	case EWoTPlayerFaction::Forsaken:
		Package = TEXT("ForsakenSkins"); Prefix = TEXT("For"); break;
	case EWoTPlayerFaction::Hound:
		Package = TEXT("HoundSkins"); Prefix = TEXT("Hou"); break;
	case EWoTPlayerFaction::Whitecloak:
		Package = TEXT("WhitecloakSkins"); Prefix = TEXT("Whi"); break;
	case EWoTPlayerFaction::AesSedai:
	default:
		break;
	}
	const EWoTPlayerSkinColor Normalized = NormalizeSkinColor(Color);
	const TCHAR* Suffix = TEXT("Base");
	switch (Normalized)
	{
	case EWoTPlayerSkinColor::Black: Suffix = TEXT("Black"); break;
	case EWoTPlayerSkinColor::Blue: Suffix = TEXT("Blue"); break;
	case EWoTPlayerSkinColor::Red: Suffix = TEXT("Red"); break;
	case EWoTPlayerSkinColor::Green: Suffix = TEXT("Green"); break;
	case EWoTPlayerSkinColor::Base:
	default: break;
	}
	return FString::Printf(TEXT("%s.%s_%s"), Package, Prefix, Suffix);
}

bool FWoTFrontEndModel::IsValidSkinOption(const FString& Candidate)
{
	for (const FWoTFactionChoice& Faction : FactionChoices())
	{
		for (const FWoTSkinChoice& Skin : SkinChoices())
		{
			if (Candidate.Equals(SkinOption(Faction.Faction, Skin.Color),
				ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
	}
	return false;
}

FString FWoTFrontEndModel::SanitizePlayerName(const FString& In)
{
	FString Sanitized = WoTFrontEnd_FilterPlayerNameCharacters(In);
	Sanitized.TrimStartAndEndInline();
	Sanitized.LeftInline(20);
	Sanitized.TrimStartAndEndInline();
	if (Sanitized == TEXT("_"))
	{
		Sanitized.Reset();
	}
	return Sanitized;
}

FString FWoTFrontEndModel::FilterPlayerNameInput(const FString& In)
{
	FString Filtered = WoTFrontEnd_FilterPlayerNameCharacters(In);
	Filtered.LeftInline(20);
	return Filtered;
}

bool FWoTFrontEndModel::SanitizeDirectAddress(const FString& Address,
	FString& OutAddress)
{
	OutAddress = Address;
	OutAddress.TrimStartAndEndInline();
	if (OutAddress.IsEmpty() || OutAddress.Contains(TEXT("/"))
		|| OutAddress.Contains(TEXT("?")) || OutAddress.Contains(TEXT("#")))
	{
		OutAddress.Reset();
		return false;
	}

	FString Host = OutAddress;
	FString Port;
	int32 ColonIndex = INDEX_NONE;
	if (OutAddress.FindChar(TEXT(':'), ColonIndex))
	{
		int32 LastColonIndex = INDEX_NONE;
		OutAddress.FindLastChar(TEXT(':'), LastColonIndex);
		if (ColonIndex != LastColonIndex)
		{
			OutAddress.Reset();
			return false;
		}
		Host = OutAddress.Left(ColonIndex);
		Port = OutAddress.Mid(ColonIndex + 1);
	}

	if (Host.IsEmpty() || Host.Len() > 253)
	{
		OutAddress.Reset();
		return false;
	}
	TArray<FString> Labels;
	Host.ParseIntoArray(Labels, TEXT("."), /*bCullEmpty*/ false);
	for (const FString& Label : Labels)
	{
		if (Label.IsEmpty() || Label.Len() > 63
			|| !WoTFrontEnd_IsASCIIAlphaNumeric(Label[0])
			|| !WoTFrontEnd_IsASCIIAlphaNumeric(Label[Label.Len() - 1]))
		{
			OutAddress.Reset();
			return false;
		}
		for (const TCHAR Char : Label)
		{
			if (!WoTFrontEnd_IsASCIIAlphaNumeric(Char) && Char != TEXT('-'))
			{
				OutAddress.Reset();
				return false;
			}
		}
	}

	if (!Port.IsEmpty() || ColonIndex != INDEX_NONE)
	{
		int32 PortNumber = 0;
		if (Port.IsEmpty())
		{
			OutAddress.Reset();
			return false;
		}
		for (const TCHAR Char : Port)
		{
			if (Char < TEXT('0') || Char > TEXT('9'))
			{
				OutAddress.Reset();
				return false;
			}
			PortNumber = PortNumber * 10 + (Char - TEXT('0'));
			if (PortNumber > 65535)
			{
				OutAddress.Reset();
				return false;
			}
		}
		if (PortNumber < 1)
		{
			OutAddress.Reset();
			return false;
		}
	}
	return true;
}

FString FWoTFrontEndModel::ShortMapName(const FString& MapPackage)
{
	return FPaths::GetBaseFilename(WoTFrontEnd_URLWithoutOptions(MapPackage));
}

bool FWoTFrontEndModel::IsArenaMap(const FString& MapPackage)
{
	const FString SanitizedMapPackage = WoTFrontEnd_URLWithoutOptions(MapPackage);
	return SanitizedMapPackage.StartsWith(
			TEXT("/Game/WoT/Maps/"), ESearchCase::CaseSensitive)
		&& ShortMapName(SanitizedMapPackage).StartsWith(
			TEXT("Arena_"), ESearchCase::CaseSensitive);
}

bool FWoTFrontEndModel::IsBattleMap(const FString& MapPackage)
{
	const FString SanitizedMapPackage = WoTFrontEnd_URLWithoutOptions(MapPackage);
	return SanitizedMapPackage.StartsWith(
			TEXT("/Game/WoT/Maps/"), ESearchCase::CaseSensitive)
		&& ShortMapName(SanitizedMapPackage).StartsWith(
			TEXT("Battle"), ESearchCase::CaseSensitive);
}

bool FWoTFrontEndModel::IsPlayableMap(const FString& MapPackage)
{
	return IsArenaMap(MapPackage) || IsBattleMap(MapPackage);
}

int32 FWoTFrontEndModel::BattleCitadelCount(const FString& MapPackage)
{
	if (!IsBattleMap(MapPackage))
	{
		return 0;
	}
	const FString MapName = ShortMapName(MapPackage);
	for (int32 Count = 2; Count <= 4; ++Count)
	{
		if (MapName.StartsWith(FString::Printf(TEXT("Battle%d_"), Count),
			ESearchCase::CaseSensitive))
		{
			return Count;
		}
	}
	return 0;
}

int32 FWoTFrontEndModel::MinSealBudget(int32 SealGoal, int32 NumCitadels)
{
	const int32 ClampedGoal = FMath::Clamp(SealGoal, 2, 4);
	const int32 ClampedCitadels = FMath::Clamp(NumCitadels, 2, 4);
	return FMath::DivideAndRoundUp(ClampedGoal, ClampedCitadels);
}

int32 FWoTFrontEndModel::MaxSealBudget(int32 SealGoal)
{
	return FMath::Clamp(SealGoal, 2, 4) - 1;
}

int32 FWoTFrontEndModel::NormalizeMaxClientTickRate(int32 TickRate)
{
	return TickRate <= 0 ? 0 : FMath::Clamp(TickRate, 10, 30);
}

bool FWoTFrontEndModel::SanitizeHostPassword(const FString& Password,
	FString& OutPassword)
{
	OutPassword.Reset();
	if (Password.Len() > 32)
	{
		return false;
	}
	for (const TCHAR Char : Password)
	{
		if (Char <= 0x20 || Char > 0x7e || Char == TEXT('?')
			|| Char == TEXT('#') || Char == TEXT('=') || Char == TEXT('"')
			|| Char == TEXT('\\'))
		{
			return false;
		}
	}
	OutPassword = Password;
	return true;
}

void FWoTFrontEndModel::UpdateBattlePlayerLimits(FWoTHostRequest& Request,
	int32 NumCitadels)
{
	const int32 Citadels = FMath::Clamp(NumCitadels, 2, 4);
	Request.MaxTeamSize = FMath::Clamp(Request.MaxTeamSize, 1, 16 / Citadels);
	if (Request.bAutoTeamSelect)
	{
		Request.MaxPlayers = Citadels * Request.MaxTeamSize;
	}
	else
	{
		Request.MaxPlayers = FMath::Clamp(Request.MaxPlayers, Citadels, 16);
		Request.MaxTeamSize = FMath::Min(Request.MaxTeamSize, Request.MaxPlayers);
	}
}

void FWoTFrontEndModel::UpdateSealBudget(FWoTHostRequest& Request,
	int32 SealBudget, int32 NumCitadels)
{
	Request.SealGoal = FMath::Clamp(Request.SealGoal, 2, 4);
	Request.SealBudget = FMath::Clamp(SealBudget,
		MinSealBudget(Request.SealGoal, NumCitadels),
		MaxSealBudget(Request.SealGoal));
}

void FWoTFrontEndModel::UpdateSealGoal(FWoTHostRequest& Request,
	int32 SealGoal, int32 NumCitadels)
{
	Request.SealGoal = FMath::Clamp(SealGoal, 2, 4);
	UpdateSealBudget(Request, Request.SealBudget, NumCitadels);
}

FWoTHostRequest FWoTFrontEndModel::DefaultHostRequest(const FString& MapPackage)
{
	FWoTHostRequest Request;
	Request.MapPackage = WoTFrontEnd_URLWithoutOptions(MapPackage);
	// giMPArena authors 20. giMPBattle derives from giMP rather than Arena,
	// so its inherited retail FragLimit is zero. Both inherit TimeLimit zero.
	Request.FragLimit = IsBattleMap(MapPackage) ? 0 : 20;
	Request.TimeLimit = 0;
	Request.MaxPlayers = 6;
	Request.MaxTeamSize = 3;
	Request.bCycleLevels = true;
	Request.bAutoTeamSelect = false;
	const int32 Citadels = BattleCitadelCount(MapPackage);
	if (Citadels > 0)
	{
		Request.bCycleLevels = Citadels <= 2;
		UpdateBattlePlayerLimits(Request, Citadels);
	}
	return Request;
}

FString FWoTFrontEndModel::AppendClassOption(const FString& URL,
	EWoTPlayerFaction Faction)
{
	const FString URLWithoutClass = WoTFrontEnd_WithoutPlayerOptions(
		URL, /*bRemoveSkin*/ false);
	int32 FragmentIndex = INDEX_NONE;
	URLWithoutClass.FindChar(TEXT('#'), FragmentIndex);
	const FString Fragment = FragmentIndex == INDEX_NONE
		? FString() : URLWithoutClass.Mid(FragmentIndex);
	FString Result = FragmentIndex == INDEX_NONE
		? URLWithoutClass : URLWithoutClass.Left(FragmentIndex);
	Result = WoTFrontEnd_AppendOption(Result,
		FString::Printf(TEXT("Class=%s"), *ClassOption(Faction)));
	return Result + Fragment;
}

FString FWoTFrontEndModel::AppendPlayerOptions(const FString& URL,
	EWoTPlayerFaction Faction, EWoTPlayerSkinColor SkinColor)
{
	const FString Sanitized = WoTFrontEnd_WithoutPlayerOptions(
		URL, /*bRemoveSkin*/ true);
	int32 FragmentIndex = INDEX_NONE;
	Sanitized.FindChar(TEXT('#'), FragmentIndex);
	const FString Fragment = FragmentIndex == INDEX_NONE
		? FString() : Sanitized.Mid(FragmentIndex);
	FString Result = FragmentIndex == INDEX_NONE
		? Sanitized : Sanitized.Left(FragmentIndex);
	Result = WoTFrontEnd_AppendOption(Result,
		FString::Printf(TEXT("Class=%s"), *ClassOption(Faction)));
	if (NormalizeSkinColor(SkinColor) != EWoTPlayerSkinColor::Base)
	{
		Result = WoTFrontEnd_AppendOption(Result,
			FString::Printf(TEXT("Skin=%s"), *SkinOption(Faction, SkinColor)));
	}
	return Result + Fragment;
}

FString FWoTFrontEndModel::BuildHostTravelURL(const FWoTHostRequest& Request,
	EWoTPlayerFaction Faction, EWoTPlayerSkinColor SkinColor)
{
	const FString SanitizedMapPackage =
		WoTFrontEnd_URLWithoutOptions(Request.MapPackage);
	if (!IsPlayableMap(SanitizedMapPackage))
	{
		return FString();
	}
	FWoTHostRequest Clamped = Request;
	const bool bBattleMap = IsBattleMap(SanitizedMapPackage);
	if (bBattleMap)
	{
		const int32 Citadels = BattleCitadelCount(SanitizedMapPackage);
		UpdateBattlePlayerLimits(Clamped, Citadels > 0 ? Citadels : 2);
		UpdateSealGoal(Clamped, Clamped.SealGoal, Citadels > 0 ? Citadels : 2);
	}

	FString URL = SanitizedMapPackage;
	URL = WoTFrontEnd_AppendOption(URL, TEXT("listen"));
	if (Clamped.bLAN)
	{
		// LAN listen stays on IP: NetDriverEOS::InitListen passes a URL with
		// ?bIsLanMatch through to IpNetDriver even once EOS is started in this
		// process. An internet host omits it and listens on EOS P2P.
		URL = WoTFrontEnd_AppendOption(URL, TEXT("bIsLanMatch"));
	}
	if (bBattleMap)
	{
		URL = WoTFrontEnd_AppendOption(
			URL, TEXT("game=/Script/WoT.WoTBattleGameMode"));
	}
	URL = WoTFrontEnd_AppendOption(URL, FString::Printf(
		TEXT("FragLimit=%d"), FMath::Clamp(Clamped.FragLimit, 0, 100)));
	URL = WoTFrontEnd_AppendOption(URL, FString::Printf(
		TEXT("TimeLimit=%d"), FMath::Clamp(Clamped.TimeLimit, 0, 60)));
	URL = WoTFrontEnd_AppendOption(URL, FString::Printf(
		TEXT("Cycle=%d"), Clamped.bCycleLevels ? 1 : 0));
	URL = WoTFrontEnd_AppendOption(URL, FString::Printf(
		TEXT("MaxPlayers=%d"), FMath::Clamp(Clamped.MaxPlayers, 2, 16)));
	URL = WoTFrontEnd_AppendOption(URL, FString::Printf(
		TEXT("MaxClientTickRate=%d"),
		NormalizeMaxClientTickRate(Clamped.MaxClientTickRate)));
	if (bBattleMap)
	{
		URL = WoTFrontEnd_AppendOption(URL, FString::Printf(
			TEXT("SealGoal=%d"), Clamped.SealGoal));
		URL = WoTFrontEnd_AppendOption(URL, FString::Printf(
			TEXT("SealBudget=%d"), Clamped.SealBudget));
		URL = WoTFrontEnd_AppendOption(URL, FString::Printf(
			TEXT("AutoTeamSelect=%d"), Clamped.bAutoTeamSelect ? 1 : 0));
		URL = WoTFrontEnd_AppendOption(URL, FString::Printf(
			TEXT("MaxTeamSize=%d"), Clamped.MaxTeamSize));
	}
	FString AdminPassword;
	FString GamePassword;
	if (!SanitizeHostPassword(Clamped.AdminPassword, AdminPassword)
		|| !SanitizeHostPassword(Clamped.GamePassword, GamePassword))
	{
		return FString();
	}
	if (!AdminPassword.IsEmpty())
	{
		URL = WoTFrontEnd_AppendOption(URL, TEXT("AdminPassword=") + AdminPassword);
	}
	if (!GamePassword.IsEmpty())
	{
		URL = WoTFrontEnd_AppendOption(URL, TEXT("GamePassword=") + GamePassword);
	}
	return AppendPlayerOptions(URL, Faction, SkinColor);
}
