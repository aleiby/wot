#pragma once

#include "CoreMinimal.h"

class UTexture2D;

enum class EWoTHUDStyle : uint8
{
	Normal,
	Masked,
	Translucent
};

enum class EWoTTextAlignment : uint8
{
	Left,
	Center,
	Right
};

// One of PlayerPawn's eight local progress-message/color slots. Kept beside
// the draw command so both the controller owner and pure HUD layout share it.
struct FWoTProgressEntry
{
	FString Message;
	FLinearColor Color = FLinearColor::White;
};

struct FWoTHUDDrawCmd
{
	FName TexId;
	TSoftObjectPtr<UTexture2D> Texture;
	FString Text;
	FName FontId;
	float X = 0.0f;
	float Y = 0.0f;
	float Scale = 1.0f;
	EWoTHUDStyle Style = EWoTHUDStyle::Normal;
	FLinearColor Tint = FLinearColor::White;
	EWoTTextAlignment TextAlignment = EWoTTextAlignment::Left;
	// Retained for existing HUD layouts. New layouts should use TextAlignment.
	bool bCenterX = false;
};
