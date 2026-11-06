#include "WoTFactionCharacters.h"

// Values from each faction's shipped defaultproperties (exports/script/WoTPawns/<Class>.uc).
// Mesh paths follow the deterministic import layout (PORT_NOTES "Mesh binding pass").

AWoTAesSedai::AWoTAesSedai()
{
	// AesSedai.uc:91-104.
	TeamDescription = TEXT("Aes Sedai");
	BodyMeshPath = FSoftObjectPath(
		TEXT("/Game/WoT/Meshes/AesSedai/AesSedai/SkeletalMeshes/AesSedai.AesSedai"));
	BodyDrawScale = 0.95f;
	bIsFemale = true;
	SummonFaction = EWoTCreatureFaction::AesSedai;
	// AesSedai declares no PlayerColor override — the WOTPlayer base default PC_Blue
	// (WOTPlayer.uc:7263) rides in from the AWoTCharacter member default.
	// AnimationTableClass (AesSedai.uc:87) -> the WoTAnimTable registry key.
	AnimTableName = FName(TEXT("AesSedai"));
	// SoundTableClass (AesSedai.uc:86) -> the WoTSoundTable registry key.
	SoundTableName = FName(TEXT("AesSedai"));
	DisguiseIcon = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		TEXT("/Game/WoT/Textures/WoTPawns/H_AesSedaiDisguise.H_AesSedaiDisguise")));
	for (int32 HealthIconSlot = 0; HealthIconSlot < 4; ++HealthIconSlot)
	{
		HealthIcons[HealthIconSlot] = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Textures/WoTPawns/H_AesSedai%d.H_AesSedai%d"),
			HealthIconSlot, HealthIconSlot)));
	}
}

AWoTForsaken::AWoTForsaken()
{
	// Forsaken.uc:85-98.
	TeamDescription = TEXT("Forsaken");
	BodyMeshPath = FSoftObjectPath(
		TEXT("/Game/WoT/Meshes/Forsaken/Forsaken/SkeletalMeshes/Forsaken.Forsaken"));
	BodyDrawScale = 0.67f;
	SummonFaction = EWoTCreatureFaction::Forsaken;
	// Forsaken.uc:90 PlayerColor=PC_Red.
	PlayerColor = TEXT("PC_Red");
	// AnimationTableClass (Forsaken.uc:83) -> the WoTAnimTable registry key.
	AnimTableName = FName(TEXT("Forsaken"));
	// SoundTableClass (Forsaken.uc:82) -> the WoTSoundTable registry key.
	SoundTableName = FName(TEXT("Forsaken"));
	DisguiseIcon = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		TEXT("/Game/WoT/Textures/WoTPawns/H_ForsakenDisguise.H_ForsakenDisguise")));
	for (int32 HealthIconSlot = 0; HealthIconSlot < 4; ++HealthIconSlot)
	{
		HealthIcons[HealthIconSlot] = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Textures/WoTPawns/H_Forsaken%d.H_Forsaken%d"),
			HealthIconSlot, HealthIconSlot)));
	}
}

AWoTHound::AWoTHound()
{
	// Hound.uc:88-103.
	TeamDescription = TEXT("Hound");
	BodyMeshPath = FSoftObjectPath(
		TEXT("/Game/WoT/Meshes/Hound/Hound/SkeletalMeshes/Hound.Hound"));
	BodyDrawScale = 1.44708f;
	SummonFaction = EWoTCreatureFaction::Hound;
	// Hound.uc:93 PlayerColor=PC_Purple.
	PlayerColor = TEXT("PC_Purple");
	// AnimationTableClass (Hound.uc:84) -> the WoTAnimTable registry key.
	AnimTableName = FName(TEXT("Hound"));
	// SoundTableClass (Hound.uc:83) -> the WoTSoundTable registry key.
	SoundTableName = FName(TEXT("Hound"));
	DisguiseIcon = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		TEXT("/Game/WoT/Textures/WoTPawns/H_HoundDisguise.H_HoundDisguise")));
	for (int32 HealthIconSlot = 0; HealthIconSlot < 4; ++HealthIconSlot)
	{
		HealthIcons[HealthIconSlot] = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Textures/WoTPawns/H_Hound%d.H_Hound%d"),
			HealthIconSlot, HealthIconSlot)));
	}
}

AWoTWhitecloak::AWoTWhitecloak()
{
	// Whitecloak.uc:83-96.
	TeamDescription = TEXT("Whitecloak");
	BodyMeshPath = FSoftObjectPath(
		TEXT("/Game/WoT/Meshes/Whitecloak/Whitecloak/SkeletalMeshes/Whitecloak.Whitecloak"));
	BodyDrawScale = 1.08f;
	SummonFaction = EWoTCreatureFaction::Whitecloak;
	// Whitecloak.uc:88 PlayerColor=PC_Gold.
	PlayerColor = TEXT("PC_Gold");
	// AnimationTableClass (Whitecloak.uc:81) -> the WoTAnimTable registry key.
	AnimTableName = FName(TEXT("Whitecloak"));
	// SoundTableClass (Whitecloak.uc:80) -> the WoTSoundTable registry key.
	SoundTableName = FName(TEXT("Whitecloak"));
	DisguiseIcon = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		TEXT("/Game/WoT/Textures/WoTPawns/H_WhiteCloakDisguise.H_WhiteCloakDisguise")));
	for (int32 HealthIconSlot = 0; HealthIconSlot < 4; ++HealthIconSlot)
	{
		HealthIcons[HealthIconSlot] = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Textures/WoTPawns/H_WhiteCloak%d.H_WhiteCloak%d"),
			HealthIconSlot, HealthIconSlot)));
	}
}
