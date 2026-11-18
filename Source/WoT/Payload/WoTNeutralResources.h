// Neutral identifiers carried by cooked objects after retail-derived layouts go.
#pragma once

#include "CoreMinimal.h"

enum class EWoTNeutralResourceKind : uint8
{
	Texture,
	Sound,
	Music,
	VertexMesh,
	Animation,
	BSP,
	World,
	Font,
	Procedural,
	Registry,
	Localization,
	ClassDefaults,
};

/**
 * A cooked carrier identifies meaning, never representation.
 *
 * Blob paths, offsets, counts, topology, bounds, sections, glyph metrics, and
 * decoded retail bytes belong to the authenticated cache object and are parsed
 * only after ReadVerifiedCacheBlob returns at the point of first use.
 */
struct FWoTNeutralResourceId
{
	FString LogicalId;
	EWoTNeutralResourceKind Kind = EWoTNeutralResourceKind::Texture;

	bool IsValid() const;
};

WOT_API const TCHAR* LexToString(EWoTNeutralResourceKind Kind);
WOT_API FString WoTNeutralResourceSchema(EWoTNeutralResourceKind Kind);
