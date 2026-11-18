#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTFractalSim.h"
#include "../WoTMeshFX.h"
#include "../WoTParticleSprayer.h"
#include "../WoTSpriteBillboard.h"
#include "WoTTestWorld.h"

#include "Engine/Texture.h"
#include "Engine/World.h"

namespace
{
constexpr EAutomationTestFlags WoTTextureAnimationFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

const FWoTTextureAnimationRow* Production15HzRing(FAutomationTestBase& Test)
{
	const FWoTMeshEffectRow* Shield = WoTMeshFX::Find(TEXT("AirShieldVisual"));
	if (!Test.TestNotNull(TEXT("production AirShieldVisual row"), Shield)
		|| !Test.TestTrue(TEXT("production shield has a selected texture member"),
			Shield && !Shield->SkinIdentities.IsEmpty()))
	{
		return nullptr;
	}
	const FWoTTextureAnimationRow* Ring =
		WoTMeshFX::FindTextureAnimation(Shield->SkinIdentities[0]);
	if (!Test.TestNotNull(TEXT("decoded AirShield head AnimNext ring"), Ring))
	{
		return nullptr;
	}
	Test.TestEqual(TEXT("production ring minimum rate"), Ring->MinFrameRate, 15.0f);
	Test.TestEqual(TEXT("production ring maximum rate"), Ring->MaxFrameRate, 15.0f);
	Test.TestEqual(TEXT("production ring frame count"), Ring->FrameIdentities.Num(), 5);
	return Ring;
}

AWoTWeaveSpriteEffect* SpawnProductionBillboard(UWorld& World,
	const FWoTTextureAnimationRow& Ring)
{
	AWoTWeaveSpriteEffect* Effect = World.SpawnActor<AWoTWeaveSpriteEffect>();
	if (Effect && Effect->Sprite)
	{
		Effect->Sprite->SetAnimatedTextureSeries(Ring.FrameIdentities,
			Ring.TexWidth, Ring.TexHeight, Ring.MinFrameRate, Ring.MaxFrameRate);
	}
	return Effect;
}

void DrawAndTick(UWorld* World, UWoTSpriteBillboardComponent* Sprite,
	const int32 Count = 1)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Sprite->MarkDrawnForTesting();
		WoTTest::TickWorld(World, 1);
	}
}

bool BoundToCurrentDecodedFrame(FAutomationTestBase& Test,
	UWoTSpriteBillboardComponent& Sprite, const TCHAR* What)
{
	UTexture* Bound = Sprite.GetBoundTextureForTesting();
	const FString Identity = Sprite.GetCurrentFrameIdentityForTesting();
	int32 Dot = INDEX_NONE;
	const FString ExpectedName = Identity.FindLastChar(TEXT('.'), Dot)
		? Identity.RightChop(Dot + 1) : FString();
	return Test.TestNotNull(FString(What) + TEXT(" bound MID texture"), Bound)
		&& Test.TestEqual(FString(What) + TEXT(" MID uses decoded current frame"),
			Bound->GetName(), ExpectedName);
}

template <typename FMarkDrawn>
int32 ExerciseOneFrameLaggedCadence(UWorld* World, const FString& Identity,
	FMarkDrawn&& MarkDrawn)
{
	constexpr float Step = 1.0f / 60.0f;
	// During game frame N, the default one-frame render-thread lag exposes the
	// render timestamp from N-2 rather than the immediately previous game frame.
	WoTTest::TickWorld(World, 2, Step);
	MarkDrawn(World->GetTimeSeconds() - Step);
	WoTTest::TickWorld(World, 1, Step);
	UWoTTextureAnimationSubsystem* Animation =
		World->GetSubsystem<UWoTTextureAnimationSubsystem>();
	int32 Previous = Animation
		? Animation->GetFrameForTesting(Identity) : INDEX_NONE;
	int32 Advances = 0;
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		MarkDrawn(World->GetTimeSeconds() - Step);
		WoTTest::TickWorld(World, 1, Step);
		const int32 Current = Animation
			? Animation->GetFrameForTesting(Identity) : INDEX_NONE;
		Advances += Current != Previous ? 1 : 0;
		Previous = Current;
	}
	return Advances;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureAnimationRateRuleSpec,
	"WoT.TextureAnimation.RetailRateRule", WoTTextureAnimationFlags)

bool FWoTTextureAnimationRateRuleSpec::RunTest(const FString&)
{
	float Accumulator = 0.0f;
	int32 Steps = 0;
	for (int32 Frame = 0; Frame < 3; ++Frame)
	{
		TestFalse(TEXT("15 fps does not advance during the first three 60 Hz frames"),
			WoTAdvanceRateLimitedTexture(Accumulator, 0.0f, 15.0f,
				1.0f / 60.0f, [&Steps]() { ++Steps; }));
	}
	TestTrue(TEXT("15 fps advances on the fourth 60 Hz frame"),
		WoTAdvanceRateLimitedTexture(Accumulator, 0.0f, 15.0f,
			1.0f / 60.0f, [&Steps]() { ++Steps; }));
	TestEqual(TEXT("rate limiter advances at most once"), Steps, 1);

	Accumulator = 0.0f;
	Steps = 0;
	TestTrue(TEXT("MaxTime branch advances once after a hitch"),
		WoTAdvanceRateLimitedTexture(Accumulator, 2.0f, 15.0f, 1.6f,
			[&Steps]() { ++Steps; }));
	TestEqual(TEXT("MaxTime branch still caps the request to one step"), Steps, 1);
	TestEqual(TEXT("MaxTime branch caps carry at one long interval"),
		Accumulator, 0.5f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureAnimationHiddenCatchUpSpec,
	"WoT.TextureAnimation.ProductionBillboardHiddenCatchUp",
	WoTTextureAnimationFlags)

bool FWoTTextureAnimationHiddenCatchUpSpec::RunTest(const FString&)
{
	const FWoTTextureAnimationRow* Ring = Production15HzRing(*this);
	UWorld* World = Ring ? WoTTest::CreateSpecWorld() : nullptr;
	AWoTWeaveSpriteEffect* Effect = World
		? SpawnProductionBillboard(*World, *Ring) : nullptr;
	if (!TestNotNull(TEXT("production billboard"), Effect) || !Effect->Sprite)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	DrawAndTick(World, Effect->Sprite, 5);
	TestEqual(TEXT("visible 15 Hz ring reaches exactly its decoded next frame"),
		Effect->Sprite->GetCurrentFrameForTesting(), 1);
	WoTTest::TickWorld(World, 60);
	DrawAndTick(World, Effect->Sprite);
	TestEqual(TEXT("one-second hidden interval advances exactly once when redrawn"),
		Effect->Sprite->GetCurrentFrameForTesting(), 2);
	TestTrue(TEXT("redrawn MID receives the decoded next texture"),
		BoundToCurrentDecodedFrame(*this, *Effect->Sprite, TEXT("redrawn billboard")));
	DrawAndTick(World, Effect->Sprite);
	TestEqual(TEXT("MaxTime branch retains its capped remainder"),
		Effect->Sprite->GetCurrentFrameForTesting(), 3);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureAnimationHiddenFreezeSpec,
	"WoT.TextureAnimation.ProductionBillboardHiddenFreeze",
	WoTTextureAnimationFlags)

bool FWoTTextureAnimationHiddenFreezeSpec::RunTest(const FString&)
{
	const FWoTTextureAnimationRow* Ring = Production15HzRing(*this);
	UWorld* World = Ring ? WoTTest::CreateSpecWorld() : nullptr;
	AWoTWeaveSpriteEffect* Effect = World
		? SpawnProductionBillboard(*World, *Ring) : nullptr;
	if (!TestNotNull(TEXT("production billboard"), Effect) || !Effect->Sprite)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	DrawAndTick(World, Effect->Sprite, 5);
	const int32 LastDrawnFrame = Effect->Sprite->GetCurrentFrameForTesting();
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("first frame after drawing stops does not use a grace window"),
		Effect->Sprite->GetCurrentFrameForTesting(), LastDrawnFrame);
	WoTTest::TickWorld(World, 60);
	TestEqual(TEXT("hidden production billboard never advances"),
		Effect->Sprite->GetCurrentFrameForTesting(), LastDrawnFrame);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureAnimationSharedFrameSpec,
	"WoT.TextureAnimation.ProductionBillboardSharedFrame",
	WoTTextureAnimationFlags)

bool FWoTTextureAnimationSharedFrameSpec::RunTest(const FString&)
{
	const FWoTTextureAnimationRow* Ring = Production15HzRing(*this);
	UWorld* World = Ring ? WoTTest::CreateSpecWorld() : nullptr;
	AWoTWeaveSpriteEffect* First = World
		? SpawnProductionBillboard(*World, *Ring) : nullptr;
	AWoTWeaveSpriteEffect* Second = World
		? SpawnProductionBillboard(*World, *Ring) : nullptr;
	if (!TestNotNull(TEXT("first production billboard"), First)
		|| !TestNotNull(TEXT("second production billboard"), Second)
		|| !First->Sprite || !Second->Sprite)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	for (int32 Frame = 0; Frame < 5; ++Frame)
	{
		First->Sprite->MarkDrawnForTesting();
		Second->Sprite->MarkDrawnForTesting();
		WoTTest::TickWorld(World, 1);
		TestEqual(TEXT("two consumers share the same per-world texture frame"),
			Second->Sprite->GetCurrentFrameForTesting(),
			First->Sprite->GetCurrentFrameForTesting());
	}
	TestEqual(TEXT("shared state advances the decoded ring only once per frame"),
		First->Sprite->GetCurrentFrameForTesting(), 1);
	TestTrue(TEXT("first MID receives the shared decoded frame"),
		BoundToCurrentDecodedFrame(*this, *First->Sprite, TEXT("first billboard")));
	TestTrue(TEXT("second MID receives the shared decoded frame"),
		BoundToCurrentDecodedFrame(*this, *Second->Sprite, TEXT("second billboard")));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureAnimationTailWrapSpec,
	"WoT.TextureAnimation.ProductionBillboardTailWrapsToDrawnTexture",
	WoTTextureAnimationFlags)

bool FWoTTextureAnimationTailWrapSpec::RunTest(const FString&)
{
	const FWoTTextureAnimationRow* Head = Production15HzRing(*this);
	const FWoTTextureAnimationRow* Tail = Head && Head->FrameIdentities.Num() > 2
		? WoTMeshFX::FindTextureAnimation(Head->FrameIdentities[2]) : nullptr;
	if (!TestNotNull(TEXT("a decoded mid-chain production row exists"), Tail)
		|| !TestTrue(TEXT("the mid-chain row retains at least its drawn frame and tail"),
			Tail && Tail->FrameIdentities.Num() > 1))
	{
		return false;
	}
	TestEqual(TEXT("the row begins at the exact texture the consumer draws"),
		Tail->FrameIdentities[0], Head->FrameIdentities[2]);
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTWeaveSpriteEffect* Effect = SpawnProductionBillboard(*World, *Tail);
	if (!TestNotNull(TEXT("mid-chain production billboard"), Effect)
		|| !Effect->Sprite)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	for (int32 Frame = 0; Frame < 60
		&& Effect->Sprite->GetCurrentFrameForTesting()
			!= Tail->FrameIdentities.Num() - 1; ++Frame)
	{
		DrawAndTick(World, Effect->Sprite);
	}
	TestEqual(TEXT("the drawn mid-chain consumer reaches its None tail"),
		Effect->Sprite->GetCurrentFrameForTesting(),
		Tail->FrameIdentities.Num() - 1);
	for (int32 Frame = 0; Frame < 60
		&& Effect->Sprite->GetCurrentFrameForTesting() != 0; ++Frame)
	{
		DrawAndTick(World, Effect->Sprite);
	}
	// UE1 sets AnimCurrent back to the texture supplied to Get (the drawn
	// texture), not an unrelated global chain head.
	TestEqual(TEXT("AnimNext None wraps to the exact drawn mid-chain texture"),
		Effect->Sprite->GetCurrentFrameIdentityForTesting(),
		Tail->FrameIdentities[0]);
	TestNotEqual(TEXT("the mid-chain wrap is not the global chain head"),
		Effect->Sprite->GetCurrentFrameIdentityForTesting(),
		Head->FrameIdentities[0]);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureAnimationDistinctIdentitySpec,
	"WoT.TextureAnimation.ProductionBillboardDistinctExactIdentities",
	WoTTextureAnimationFlags)

bool FWoTTextureAnimationDistinctIdentitySpec::RunTest(const FString&)
{
	const FWoTTextureAnimationRow* Head = Production15HzRing(*this);
	const FWoTTextureAnimationRow* SecondRow = Head
		&& Head->FrameIdentities.Num() > 2
		? WoTMeshFX::FindTextureAnimation(Head->FrameIdentities[1]) : nullptr;
	if (!TestNotNull(TEXT("the second exact texture has its production row"),
			SecondRow) || !SecondRow || SecondRow->FrameIdentities.Num() < 2)
	{
		return false;
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTWeaveSpriteEffect* First = SpawnProductionBillboard(*World, *Head);
	AWoTWeaveSpriteEffect* Second =
		SpawnProductionBillboard(*World, *SecondRow);
	if (!TestNotNull(TEXT("first identity production billboard"), First)
		|| !TestNotNull(TEXT("second identity production billboard"), Second)
		|| !First->Sprite || !Second->Sprite)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	First->Sprite->SetAnimatedTextureSeries(Head->FrameIdentities,
		Head->TexWidth, Head->TexHeight, 0.0f, 0.0f);
	Second->Sprite->SetAnimatedTextureSeries(SecondRow->FrameIdentities,
		SecondRow->TexWidth, SecondRow->TexHeight, 0.0f, 0.0f);
	DrawAndTick(World, First->Sprite, 2);
	TestEqual(TEXT("the first exact identity advances twice"),
		First->Sprite->GetCurrentFrameForTesting(), 2);
	UWoTTextureAnimationSubsystem* Animation =
		World->GetSubsystem<UWoTTextureAnimationSubsystem>();
	TestEqual(TEXT("only the drawn first identity owns state"),
		Animation ? Animation->GetStateCountForTesting() : INDEX_NONE, 1);

	DrawAndTick(World, Second->Sprite);
	TestEqual(TEXT("the independently drawn second identity advances once"),
		Second->Sprite->GetCurrentFrameForTesting(), 1);
	TestEqual(TEXT("drawing the second identity does not advance the first"),
		First->Sprite->GetCurrentFrameForTesting(), 2);
	TestEqual(TEXT("different exact texture identities own two world states"),
		Animation ? Animation->GetStateCountForTesting() : INDEX_NONE, 2);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureAnimationZeroRateSpec,
	"WoT.TextureAnimation.ProductionBillboardZeroMaxRateFirstDraw",
	WoTTextureAnimationFlags)

bool FWoTTextureAnimationZeroRateSpec::RunTest(const FString&)
{
	const FWoTTextureAnimationRow* Ring = Production15HzRing(*this);
	UWorld* World = Ring ? WoTTest::CreateSpecWorld() : nullptr;
	AWoTWeaveSpriteEffect* Effect = World
		? SpawnProductionBillboard(*World, *Ring) : nullptr;
	if (!TestNotNull(TEXT("zero-rate production billboard"), Effect)
		|| !Effect->Sprite)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Effect->Sprite->SetAnimatedTextureSeries(Ring->FrameIdentities,
		Ring->TexWidth, Ring->TexHeight, 0.0f, 0.0f);
	TestEqual(TEXT("zero-rate series begins on the drawn texture"),
		Effect->Sprite->GetCurrentFrameForTesting(), 0);
	DrawAndTick(World, Effect->Sprite);
	TestEqual(TEXT("MaxFrameRate zero advances on the first drawn frame"),
		Effect->Sprite->GetCurrentFrameForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureAnimationNeverDrawnSpec,
	"WoT.TextureAnimation.ProductionBillboardNeverDrawnAllocatesNoState",
	WoTTextureAnimationFlags)

bool FWoTTextureAnimationNeverDrawnSpec::RunTest(const FString&)
{
	const FWoTTextureAnimationRow* Ring = Production15HzRing(*this);
	UWorld* World = Ring ? WoTTest::CreateSpecWorld() : nullptr;
	AWoTWeaveSpriteEffect* Effect = World
		? SpawnProductionBillboard(*World, *Ring) : nullptr;
	if (!TestNotNull(TEXT("never-drawn production billboard"), Effect)
		|| !Effect->Sprite)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTTextureAnimationSubsystem* Animation =
		World->GetSubsystem<UWoTTextureAnimationSubsystem>();
	TestEqual(TEXT("configuration alone allocates no per-world animation state"),
		Animation ? Animation->GetStateCountForTesting() : INDEX_NONE, 0);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureAnimationRenderLagSpec,
	"WoT.TextureAnimation.ProductionConsumersTolerateRenderLag",
	WoTTextureAnimationFlags)

bool FWoTTextureAnimationRenderLagSpec::RunTest(const FString&)
{
	const FWoTTextureAnimationRow* Ring = Production15HzRing(*this);
	if (!Ring) return false;
	const FString Identity = Ring->FrameIdentities[0];

	UWorld* BillboardWorld = WoTTest::CreateSpecWorld();
	AWoTWeaveSpriteEffect* Billboard =
		SpawnProductionBillboard(*BillboardWorld, *Ring);
	if (TestNotNull(TEXT("lagged production billboard"), Billboard)
		&& Billboard && Billboard->Sprite)
	{
		const int32 Advances = ExerciseOneFrameLaggedCadence(BillboardWorld,
			Identity, [Billboard](const float DrawTime)
			{
				Billboard->Sprite->MarkDrawnForTesting(DrawTime);
			});
		TestEqual(TEXT("one-frame-lagged 15 Hz billboard advances 30 times in 2 s"),
			Advances, 30);
	}
	WoTTest::DestroySpecWorld(BillboardWorld);

	const FWoTMeshEffectRow* Shield = WoTMeshFX::Find(TEXT("AirShieldVisual"));
	UWorld* MeshWorld = Shield ? WoTTest::CreateSpecWorld() : nullptr;
	AWoTMeshEffect* Mesh = MeshWorld ? MeshWorld->SpawnActor<AWoTMeshEffect>() : nullptr;
	if (TestNotNull(TEXT("production AirShield row for lag fixture"), Shield)
		&& TestNotNull(TEXT("lagged production mesh effect"), Mesh))
	{
		FWoTMeshEffectRow AnimationOnly = *Shield;
		AnimationOnly.SkinStepSeconds = 0.0f;
		AnimationOnly.bRandomSkinEachTick = false;
		Mesh->Configure(TEXT("AirShieldVisual"), AnimationOnly, nullptr);
		const int32 Advances = ExerciseOneFrameLaggedCadence(MeshWorld,
			Identity, [Mesh](const float DrawTime)
			{
				Mesh->MarkDrawnForTesting(DrawTime);
			});
		TestEqual(TEXT("one-frame-lagged 15 Hz mesh effect advances 30 times in 2 s"),
			Advances, 30);
		UWoTTextureAnimationSubsystem* Animation =
			MeshWorld->GetSubsystem<UWoTTextureAnimationSubsystem>();
		int32 CurrentFrame = Animation
			? Animation->GetFrameForTesting(Identity) : INDEX_NONE;
		for (int32 Frame = 0; Animation && CurrentFrame == 0 && Frame < 10; ++Frame)
		{
			constexpr float Step = 1.0f / 60.0f;
			Mesh->MarkDrawnForTesting(MeshWorld->GetTimeSeconds() - Step);
			WoTTest::TickWorld(MeshWorld, 1, Step);
			CurrentFrame = Animation->GetFrameForTesting(Identity);
		}
		TestTrue(TEXT("drawn shield advances to a nonzero AnimNext frame"),
			Ring->FrameIdentities.IsValidIndex(CurrentFrame) && CurrentFrame != 0);
		UTexture* Bound = Mesh->GetBoundTextureForTesting();
		const FString ExpectedIdentity = Ring->FrameIdentities.IsValidIndex(CurrentFrame)
			? Ring->FrameIdentities[CurrentFrame] : FString();
		int32 Dot = INDEX_NONE;
		const FString ExpectedName = ExpectedIdentity.FindLastChar(TEXT('.'), Dot)
			? ExpectedIdentity.RightChop(Dot + 1) : FString();
		TestNotNull(TEXT("drawn shield quad has a bound animated texture"), Bound);
		TestEqual(TEXT("drawn shield selects the subsystem's decoded ring frame"),
			Mesh->GetCurrentTextureIdentityForTesting(), ExpectedIdentity);
		if (Bound)
		{
			TestEqual(TEXT("shield quad MID uses its decoded current ring frame"),
				Bound->GetName(), ExpectedName);
		}
	}
	WoTTest::DestroySpecWorld(MeshWorld);

	UWorld* SprayerWorld = WoTTest::CreateSpecWorld();
	const FTransform At(FRotator::ZeroRotator, FVector::ZeroVector);
	AWoTParticleSprayer* Sprayer =
		SprayerWorld->SpawnActorDeferred<AWoTParticleSprayer>(
			AWoTParticleSprayer::StaticClass(), At, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (TestNotNull(TEXT("lagged production sprayer"), Sprayer))
	{
		Sprayer->Templates.AddDefaulted();
		Sprayer->bOn = false;
		Sprayer->ConfigureTemplateAnimation(0, Identity,
			Ring->FrameIdentities, {}, Ring->MinFrameRate, Ring->MaxFrameRate);
		Sprayer->FinishSpawning(At);
		const int32 Advances = ExerciseOneFrameLaggedCadence(SprayerWorld,
			Identity, [Sprayer](const float DrawTime)
			{
				Sprayer->MarkTemplateDrawnForTesting(0, DrawTime);
			});
		TestEqual(TEXT("one-frame-lagged 15 Hz sprayer advances 30 times in 2 s"),
			Advances, 30);
	}
	WoTTest::DestroySpecWorld(SprayerWorld);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureAnimationObservedDrawClockSpec,
	"WoT.TextureAnimation.ProductionBillboardUsesObservedDrawClock",
	WoTTextureAnimationFlags)

bool FWoTTextureAnimationObservedDrawClockSpec::RunTest(const FString&)
{
	const FWoTTextureAnimationRow* Ring = Production15HzRing(*this);
	UWorld* World = Ring ? WoTTest::CreateSpecWorld() : nullptr;
	AWoTWeaveSpriteEffect* Effect = World
		? SpawnProductionBillboard(*World, *Ring) : nullptr;
	if (!TestNotNull(TEXT("draw-clock production billboard"), Effect)
		|| !Effect || !Effect->Sprite)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	constexpr float DrawStep = 1.0f / 60.0f;
	constexpr float GameStep = 1.0f / 30.0f;
	float DrawTime = 100.0f;
	Effect->Sprite->MarkDrawnForTesting(DrawTime);
	WoTTest::TickWorld(World, 1, GameStep);
	int32 Previous = Effect->Sprite->GetCurrentFrameForTesting();
	int32 Advances = 0;
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		DrawTime += DrawStep;
		Effect->Sprite->MarkDrawnForTesting(DrawTime);
		WoTTest::TickWorld(World, 1, GameStep);
		const int32 Current = Effect->Sprite->GetCurrentFrameForTesting();
		Advances += Current != Previous ? 1 : 0;
		Previous = Current;
	}
	TestEqual(TEXT("two seconds of observed draw time, not four seconds of game time, "
		"drive the 15 Hz ring"), Advances, 30);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
