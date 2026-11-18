#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTMeshFX.h"
#include "../WoTMeshParticleRenderer.h"
#include "../WoTVertexAnimComponent.h"
#include "WoTTestWorld.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"

namespace
{
	constexpr EAutomationTestFlags ShieldParticleFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	AWoTMeshEffect* SpawnShield(FAutomationTestBase& Test, UWorld* World,
		const TCHAR* Config = TEXT("EarthShieldVisual"))
	{
		const FWoTMeshEffectRow* Row = WoTMeshFX::Find(Config);
		if (!Test.TestNotNull(TEXT("production shield row"), Row)) return nullptr;
		AWoTMeshEffect* Effect = WoTMeshFX::SpawnLocal(
			*World, Config, FVector(0.0, 0.0, 100.0));
		if (Test.TestNotNull(TEXT("shield effect spawned"), Effect))
		{
			// Keep timing under the spec instead of allowing the world's lifespan
			// manager to destroy the one-second retail effect.
			Effect->SetLifeSpan(0.0f);
		}
		return Effect;
	}

	float FirstParticleColor(const AWoTMeshEffect* Effect)
	{
		return Effect && Effect->ParticleQuads
			&& !Effect->ParticleQuads->PerInstanceSMCustomData.IsEmpty()
			? Effect->ParticleQuads->PerInstanceSMCustomData[0] : -1.0f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTShieldParticleCloudSpec,
	"WoT.ShieldParticles.Cloud", ShieldParticleFlags)
bool FWoTShieldParticleCloudSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMeshEffect* Effect = SpawnShield(*this, World);
	if (!Effect)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 1);

	TestFalse(TEXT("solid shield surface is hidden"), Effect->Mesh->IsVisible());
	TestTrue(TEXT("hidden shield pose driver keeps ticking"),
		Effect->Mesh->IsComponentTickEnabled());
	TestTrue(TEXT("particle cloud is the visible presentation"),
		Effect->ParticleQuads && Effect->ParticleQuads->IsVisible());

	TArray<FVector> Pose;
	if (TestTrue(TEXT("shield current pose is CPU-readable"),
		Effect->Mesh->GetCurrentPoseVertexPositions(Pose)))
	{
		// Retail package reader: Angreal.ShieldParticleMesh FrameVerts=36.
		TestEqual(TEXT("pose dedup preserves all retail logical vertices"),
			Pose.Num(), 36);
	}
	if (TestNotNull(TEXT("shield particle quad pool"), Effect->ParticleQuads.Get()))
	{
		TestEqual(TEXT("one quad is drawn for every retail FrameVerts entry"),
			Effect->ParticleQuads->GetInstanceCount(), 36);
		FTransform Quad;
		if (TestTrue(TEXT("first shield quad transform resolves"),
			Effect->ParticleQuads->GetInstanceTransform(0, Quad, true)))
		{
			UTexture* BoundTexture = Effect->GetBoundTextureForTesting();
			if (TestNotNull(TEXT("shield quad has a bound retail texture"), BoundTexture))
			{
				FIntPoint Dims(
					static_cast<int32>(BoundTexture->GetSurfaceWidth()),
					static_cast<int32>(BoundTexture->GetSurfaceHeight()));
#if WITH_EDITORONLY_DATA
				// Fresh editor assets under null RHI have no platform resource.
				// Compare with the authored source rather than accepting zero-size quads.
				if (const UTexture2D* Imported = Cast<UTexture2D>(BoundTexture);
					Imported && Imported->Source.IsValid())
				{
					Dims = FIntPoint(Imported->Source.GetSizeX(), Imported->Source.GetSizeY());
				}
#endif
				TestTrue(TEXT("shield texture has nonzero authored dimensions"), Dims.X > 0 && Dims.Y > 0);
				TestEqual(TEXT("renderer records the bound texture USize/VSize"),
					Effect->GetParticleTextureSizeForTesting(), Dims);
				const float DrawScale = 0.25f;
				TestTrue(TEXT("quad world size is current USize/VSize times DrawScale"),
					Quad.GetScale3D().Equals(FVector(
						Dims.X * DrawScale / 100.0f,
						Dims.Y * DrawScale / 100.0f, 1.0f),
						KINDA_SMALL_NUMBER));
			}
		}
	}
	// A cooked pose-read failure must not reveal the surface or leave stale
	// instances. The renderer warns once even if subsequent ticks also fail.
	UWoTMeshParticleRenderer* Renderer =
		Cast<UWoTMeshParticleRenderer>(Effect->ParticleQuads);
	UWoTVertexAnimComponent* UnboundPoseDriver =
		NewObject<UWoTVertexAnimComponent>(Effect);
	if (TestNotNull(TEXT("shield particle renderer"), Renderer)
		&& TestNotNull(TEXT("unbound failure-test pose driver"), UnboundPoseDriver))
	{
		// Clearing the production component's cache is insufficient: a bound
		// runtime mesh can immediately repopulate it. An unbound driver makes
		// GetCurrentPoseVertexPositions fail deterministically.
		Renderer->InitializeParticleRenderer(UnboundPoseDriver, true);
	}
	AddExpectedError(TEXT("cannot read bParticles current pose"),
		EAutomationExpectedErrorFlags::Contains, 1);
	Effect->Tick(0.01f);
	Effect->Tick(0.01f);
	TestEqual(TEXT("pose-read failure clears every visible quad"),
		Effect->ParticleQuads->GetInstanceCount(), 0);
	TestFalse(TEXT("pose-read failure never falls back to the solid surface"),
		Effect->Mesh->IsVisible());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTShieldParticleFadeColorSpec,
	"WoT.ShieldParticles.FadeColor", ShieldParticleFlags)
bool FWoTShieldParticleFadeColorSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMeshEffect* Effect = SpawnShield(*this, World);
	if (!Effect)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Fade-in t=.1: Scalar=.5, ScaleGlow=.5, color=(.5*.5)*.5=.125.
	Effect->Tick(0.1f);
	TestEqual(TEXT("fade-in ScaleGlow sample"), Effect->ScaleGlow, 0.5f, 1.0e-4f);
	TestEqual(TEXT("fade-in quad color uses retail double ScaleGlow"),
		FirstParticleColor(Effect), 0.125f, 1.0e-4f);

	// Sustain t=.3: exact ScaleGlow=1 keeps retail's discontinuous no-second-multiply path.
	Effect->Tick(0.2f);
	TestEqual(TEXT("sustain ScaleGlow sample"), Effect->ScaleGlow, 1.0f, 1.0e-4f);
	TestEqual(TEXT("sustain quad color preserves exact-one retail rule"),
		FirstParticleColor(Effect), 0.5f, 1.0e-4f);

	// Fade-out t=.7: halfway through the .6-second fade, back to .5/.125.
	Effect->Tick(0.4f);
	TestEqual(TEXT("fade-out ScaleGlow sample"), Effect->ScaleGlow, 0.5f, 1.0e-4f);
	TestEqual(TEXT("fade-out quad color uses retail double ScaleGlow"),
		FirstParticleColor(Effect), 0.125f, 1.0e-4f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTShieldParticleTextureCadenceSpec,
	"WoT.ShieldParticles.TextureCadence", ShieldParticleFlags)
bool FWoTShieldParticleTextureCadenceSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMeshEffect* Effect = SpawnShield(*this, World);
	const FWoTMeshEffectRow* Row = WoTMeshFX::Find(TEXT("EarthShieldVisual"));
	if (!Effect || !TestNotNull(TEXT("production earth shield row"), Row))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("shield starts at TextureSet zero"),
		Effect->GetCurrentSkinIndexForTesting(), 0);
	UTexture* FirstTexture = Effect->GetBoundTextureForTesting();
	TestNotNull(TEXT("TextureSet zero is bound to the quad material"), FirstTexture);

	Effect->Tick(1.0f / 15.0f - 0.001f);
	TestEqual(TEXT("TextureSet holds before the 15 Hz boundary"),
		Effect->GetCurrentSkinIndexForTesting(), 0);
	Effect->Tick(0.002f);
	TestEqual(TEXT("TextureSet advances at the 15 Hz boundary"),
		Effect->GetCurrentSkinIndexForTesting(), 1);
	TestTrue(TEXT("quad material swaps to the new TextureSet member"),
		Effect->GetBoundTextureForTesting() != nullptr
			&& Effect->GetBoundTextureForTesting() != FirstTexture);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
