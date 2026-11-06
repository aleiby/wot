#pragma once

#include "CoreMinimal.h"
#include "RetailData/WoTRetailBuildContext.h"
#include "WoTBootstrapCore/Hash.h"

class FWoTRuntimeClassDefaults;

// Port of WOT.GenericTextureHelper + its WoTPawns subclasses (spec:
// exports/script/WOT/GenericTextureHelper.uc; deep record: PORT_NOTES "Footsteps
// slice"): the floor texture under the pawn keys a surface type, the surface keys
// the WOT.* footstep wave set. Selection is uniform among <=4 matching rows (the
// Odds field is dead code, :460-487); unknown/None surfaces fall back to Stone
// (:445-449, :549-551). The shipped Texture.FootstepSound stub override (FootStubA
// name-carriers) is NOT exported by the pipeline — the texture-NAME prefix parse
// is the port's (sufficient) signal; PORT_NOTES divergence.
enum class EWoTSurface : uint8
{
	None,
	All,      // wildcard rows (Legion) match every surface
	Deck,     // hollow wood
	Dirt,
	Grass,
	Gravel,
	Leaves,
	Marble,
	Metal,
	Mud,
	Puddle,
	Rug,
	Stone,
	Wood
};

enum class EWoTFootstepGroup : uint8
{
	RunWalk,   // SG_RunWalk: the per-step waves
	Landed     // SG_Landed: the landing thud
};

// One MovementEffects row (EffectInfoT, GenericTextureHelper.uc:222-233). The
// shipped EffectName (Puddle rows spawn ParticleSystems.WaterSplash) is deferred
// to the splash slice.
struct FWoTFootstepRow
{
	EWoTFootstepGroup Group = EWoTFootstepGroup::RunWalk;
	EWoTSurface Surface = EWoTSurface::None;
	FName SoundSubPath;   // imported-wave subpath under /Game/WoT/Sounds
	float Volume = 1.0f;
	float Pitch = 1.0f;
	// The shipped EffectName (a WoTWeaveFX burst config): the Puddle rows spawn
	// ParticleSystems.WaterSplash per wet step (GenericTextureHelper.uc:645-648,
	// :667). NAME_None = no effect.
	FName EffectName;
};

// One shipped TextureHelperClass subclass's parameters (PCFemale/NPCMinion/
// NPCBATrolloc/NPCLegion TextureHelper defaults).
struct FWoTFootstepHelper
{
	float BasePitch = 1.0f;
	float BaseVolume = 1.0f;
	// NPCLegionTextureHelper overrides HandleMovingOnTexture to never apply the
	// walk quieting (always full volume).
	bool bNeverWalkQuiet = false;
	// Non-None: this helper's own MovementEffects table (Legion's ST_All rows).
	FName OwnTable;
};

struct FWoTFootstepSet
{
	TMap<FName, TArray<FWoTFootstepRow>> Tables;
	TMap<FName, FWoTFootstepHelper> HelpersByClass;
	// Sound-table leaf -> helper-class leaf. This is built by reading each pawn's
	// SoundTableClass and TextureHelperClass, not a native proxy list.
	TMap<FName, FName> HelperClassBySoundTable;
};

struct FWoTFootstepsBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("footsteps");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTFootstepSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTFootstepSet& Set);
	static void Install(FWoTFootstepSet&& Set);
	static void SerializeCanonical(const FWoTFootstepSet& Set, TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTFootstepSet& Built,
		const FWoTFootstepSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static void Reset();
#endif
};

namespace WoTFootsteps
{
	// ConvertTextureName (GenericTextureHelper.uc:257-327): UPPERCASED texture
	// name -> surface by prefix (specials DIRTPEBB/DIRTLEAV before DIRT; RUG is
	// a 3-char rule). Returns None when unrecognized — callers fall back Stone.
	EWoTSurface SurfaceFromTextureName(const FString& UpperName);

	/** Canonical UE1 texture identity (Package.Group.Texture) -> retail surface. */
	EWoTSurface SurfaceFromTextureIdentity(const FString& TextureIdentity);

	// Imported-material name -> the UE1 texture name (UPPERCASED), "" when
	// unparseable (MI_Sky / MI_Missing / engine materials). Accepts MIC names
	// (MI_W_<Pkg>__<Tex><flags>, MI_<Pkg>__<Tex><flags>) and raw glTF slot names
	// (<Pkg>__<Tex><flags> — bsp2obj.py qualkey; '__' is injective), tolerates a
	// missing <Pkg>__ (stale pre-qualification MICs), and strips the bsp2obj flag
	// suffix tokens (_M/_T/_Mod/_2S/_U/_Sky).
	FString TextureNameFromMaterialName(const FString& MaterialName);

	// Activated MovementEffects table lookup.
	const TArray<FWoTFootstepRow>& Table(FName TableName);

	// LookUpSound (:460-487): the first <=4 rows matching Group AND (Surface or
	// All), picked uniformly (the L/R wave names do NOT alternate feet). Null
	// when nothing matches.
	const FWoTFootstepRow* PickRow(FName TableName, EWoTFootstepGroup Group,
	                               EWoTSurface Surface);

	// The shipped per-class TextureHelperClass assignment, keyed by the pawn's
	// SoundTableName (unique per shipped class — the port derives instead of
	// stamping a parallel field; PORT_NOTES). Female (pitch 1.2): AesSedai,
	// Sister, BlackAjahSister, Sitter, AesSedaiBot, BlackAjahBoss. Minion
	// 2.0/2.0; BATrolloc 0.5/2.0; Legion own-table; everyone else Generic.
	const FWoTFootstepHelper& HelperForSoundTable(FName SoundTableName);

	// HandleMovingOnTexture's walk gate (:600-608): under RunningSoundSpeed=200
	// uu/s the step plays at WalkVolumeMultiplier=0.5 (Legion never quiets).
	float VolumeMultForSpeed(float Speed, bool bNeverWalkQuiet);
}
