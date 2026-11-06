#include "WoTPlayerColorFX.h"

#include "WoTFactionCharacters.h"

#include "GameFramework/Pawn.h"

namespace
{
	// The shared PC_* -> plain-color translation every retail consumer switch encodes
	// identically (AngrealInvDart.SetColor, ReflectSkinEffect.SetColor, LightGlobe,
	// AppearEffect): PC_Gold and PC_Yellow are one bucket ("Yellow"). Consumers accept
	// both spellings in retail; the port's single seam hands them the plain name their
	// config/registry rows are keyed by. An unknown/unset name falls to Blue — the
	// WOTPlayer base default (PC_Blue, WOTPlayer.uc:7263) and the dart default case.
	FName TranslatePlayerColor(FName PlayerColor)
	{
		const FString Name = PlayerColor.ToString();
		if (Name == TEXT("PC_Red") || Name == TEXT("Red"))
		{
			return FName(TEXT("Red"));
		}
		if (Name == TEXT("PC_Green") || Name == TEXT("Green"))
		{
			return FName(TEXT("Green"));
		}
		if (Name == TEXT("PC_Purple") || Name == TEXT("Purple"))
		{
			return FName(TEXT("Purple"));
		}
		if (Name == TEXT("PC_Gold") || Name == TEXT("Gold")
			|| Name == TEXT("PC_Yellow") || Name == TEXT("Yellow"))
		{
			return FName(TEXT("Yellow"));
		}
		if (Name == TEXT("PC_White") || Name == TEXT("White"))
		{
			return FName(TEXT("White"));
		}
		return FName(TEXT("Blue"));
	}

	// The pawn's live PlayerColor: the (appearance-)class CDO default, the port's
	// equivalent of ChangeClassModelStuff's PlayerColor copy (WOTPlayer.uc:4112).
	FName PlayerColorForPawn(const APawn* Pawn)
	{
		const UClass* SourceClass = Pawn->GetClass();
		if (const AWoTCharacter* Character = Cast<AWoTCharacter>(Pawn))
		{
			if (Character->AppearanceClass)
			{
				SourceClass = *Character->AppearanceClass;
			}
		}
		if (SourceClass->IsChildOf(AWoTCharacter::StaticClass()))
		{
			return SourceClass->GetDefaultObject<AWoTCharacter>()->PlayerColor;
		}
		// Non-WoT player fixtures resolve like the WOTPlayer base default.
		return FName(TEXT("PC_Blue"));
	}

	FName DartColorForFaction(EWoTCreatureFaction Faction)
	{
		switch (Faction)
		{
		case EWoTCreatureFaction::AesSedai: return FName(TEXT("Blue"));
		case EWoTCreatureFaction::Forsaken: return FName(TEXT("Red"));
		case EWoTCreatureFaction::Whitecloak: return FName(TEXT("Yellow"));
		case EWoTCreatureFaction::Hound: return FName(TEXT("Purple"));
		default: return FName(TEXT("Green"));
		}
	}

	bool DartFactionForClass(const UClass* Class, EWoTCreatureFaction& OutFaction)
	{
		if (!Class) { return false; }
		if (Class->IsChildOf(AWoTAesSedai::StaticClass()))
		{
			OutFaction = EWoTCreatureFaction::AesSedai; return true;
		}
		if (Class->IsChildOf(AWoTForsaken::StaticClass()))
		{
			OutFaction = EWoTCreatureFaction::Forsaken; return true;
		}
		if (Class->IsChildOf(AWoTWhitecloak::StaticClass()))
		{
			OutFaction = EWoTCreatureFaction::Whitecloak; return true;
		}
		if (Class->IsChildOf(AWoTHound::StaticClass()))
		{
			OutFaction = EWoTCreatureFaction::Hound; return true;
		}
		return false;
	}
}

FName WoTPlayerColorFX::Resolve(const APawn* Pawn, FName NonPlayerFallback)
{
	// Remote player pawns commonly have no replicated Controller on observers;
	// PlayerState is the stable all-machines discriminator. Player pawns read the
	// class-fixed PlayerColor identity (P02); non-players keep the consumer-authored
	// fallback (retail reads WOTPlayer(Owner).PlayerColor — a non-player owner falls
	// through each consumer's own default case).
	return IsValid(Pawn) && (Pawn->IsPlayerControlled() || Pawn->GetPlayerState() != nullptr)
		? TranslatePlayerColor(PlayerColorForPawn(Pawn))
		: NonPlayerFallback;
}

FName WoTPlayerColorFX::ResolveDart(const APawn* Pawn)
{
	FName NonPlayerColor(TEXT("Green"));
	EWoTCreatureFaction Faction = EWoTCreatureFaction::AesSedai;
	bool bKnownFaction = IsValid(Pawn)
		&& DartFactionForClass(Pawn->GetClass(), Faction);
	if (!bKnownFaction)
	{
		if (const AWoTCharacter* Character = Cast<AWoTCharacter>(Pawn))
		{
			bKnownFaction = DartFactionForClass(
				Character->AppearanceClass.Get(), Faction);
		}
	}
	if (bKnownFaction)
	{
		NonPlayerColor = DartColorForFaction(Faction);
	}
	return Resolve(Pawn, NonPlayerColor);
}
