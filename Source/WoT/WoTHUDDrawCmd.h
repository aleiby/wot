#pragma once

#include "CoreMinimal.h"

class UFont;
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
	/** A face chosen at run time (a positioned message's own font): it wins
	 * over FontId when set. A weak reference, rebuilt with every frame's
	 * commands at the cost of a pointer: the message that chose the face
	 * keeps it alive, nothing needs its asset path, and a face that is gone
	 * leaves FontId to resolve. */
	TWeakObjectPtr<UFont> Font;
};
