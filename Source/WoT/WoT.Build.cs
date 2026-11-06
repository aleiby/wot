using UnrealBuildTool;

public class WoT : ModuleRules
{
	public WoT(ReadOnlyTargetRules Target) : base(Target)
	{
		// Editor targets share UnrealEditor's build environment and cannot add
		// GlobalDefinitions; guarded game targets define this to 1 globally.
		bool bGuardedBootstrap = System.Linq.Enumerable.Contains(
			Target.GlobalDefinitions, "WOT_GUARDED_BOOTSTRAP=1");
		if (!bGuardedBootstrap)
		{
			PublicDefinitions.Add("WOT_GUARDED_BOOTSTRAP=0");
		}
		// One predicate owns declarations, includes, and implementations for all
		// diagnostic harness code. This keeps guarded Shipping game and server
		// translation units on the same compile surface.
		bool bCompileBootstrapTestHarnesses = !(
			bGuardedBootstrap &&
			Target.Configuration == UnrealTargetConfiguration.Shipping);
		PublicDefinitions.Add(
			"WOT_WITH_BOOTSTRAP_TEST_HARNESSES=" +
			(bCompileBootstrapTestHarnesses ? "1" : "0"));

		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		if (Target.ProjectFile != null
			&& Target.Type != TargetType.Editor && Target.Type != TargetType.Program)
		{
			RuntimeDependencies.Add(
				"$(ProjectDir)/Build/WoTRelease/release-identity.json",
				StagedFileType.NonUFS);
		}

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"NetCore",
			"Json",
			// Bootstrap/updater trust substrate: canonical formats, signatures,
			// durable records, and transaction fences shared with the standalone
			// updater and commit service. Public so editor automation discovers the
			// module's specs alongside the game's.
			"WoTBootstrapCore",
			// One-way guarded world transition: WoT implements the callback,
			// WoTLaunch owns only the permanent fail-closed dispatcher.
			"WoTLaunch",
			"NavigationSystem",
			"AIModule",
			"GameplayTasks",
			// Net test harness (Source/WoT/Testing, non-shipping): TCP command
			// channel over line-JSON. FTcpListener lives in Networking; the socket
			// primitives in Sockets.
			"Sockets",
			"Networking",
			// The OSS Sessions API (UWoTSessionSubsystem). The concrete
			// Steam/Null platform service is config-selected, never linked here.
			"OnlineSubsystem",
			"OnlineSubsystemUtils"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			// The net-test capture drives the non-realtime mixer one fixed step per
			// frame (FMixerPlatformNonRealtime::RenderAudio), as Movie Render Queue does.
			"AudioMixer",
			"NonRealtimeAudioRenderer",
			// Listen harness rollback fixtures scope the seamless transition map
			// to their retained current world.
			"EngineSettings",
			// The shipping game composes the proven per-user updater; dependency
			// direction remains WoT -> WoTUpdaterCore, never the reverse.
			"WoTUpdaterCore",
			// The pure hop engine + platform HTTP executors the production
			// transport adapters compose. Core-only module so its loopback
			// suite runs as an engine-free test program on both lanes.
			"WoTUpdateTransports",
			// Authenticated cache-v3 contract and bounded native retail readers.
			"WoTRetailReader",
			// Code-only normal front end and payload-free bootstrap screen. This
			// deliberately adds no UMG/editor asset or runtime style dependency.
			"Slate",
			"SlateCore",
			"AssetRegistry",
			// Payload texture specs fence queued resource deletion during teardown.
			"RenderCore",
			// Mesh payload injection builds public external-morph buffers and the
			// editor import bridge strips through LOD-0 MeshDescription source data.
			"RHI",
			"ImageCore",
			"MeshDescription",
			"SkeletalMeshDescription",
			// MeshAttribute::VertexInstance::Color (the vertex-color attribute
			// key the stripped bridge zeroes) is defined in StaticMeshDescription.
			"StaticMeshDescription",
			// BSP proof: player-local derived files serialize completed Chaos
			// triangle meshes so cached launches skip runtime recooking.
			"PhysicsCore",
			"Chaos",
			// Payload sound wave: UWoTPayloadSoundWave derives from
			// USoundWaveProcedural, whose IAudioProxyDataFactory proxy hook is
			// implemented in AudioExtensions.
			"AudioExtensions"
		});

		// The bootstrap-worker spec fixture signs synthetic v1/v2 releases with
		// OpenSSL (EVP_DigestSign), the same way the WoTUpdaterCore test
		// fixtures do — and the same modular-link lesson applies: a PRIVATE
		// static OpenSSL in a lower module does not resolve across DLL
		// boundaries, so the module whose TU calls EVP_* must declare it.
		// Gated to test configurations; the shipping dependency floor is
		// unchanged.
		if (Target.Configuration != UnrealTargetConfiguration.Shipping
			&& Target.Configuration != UnrealTargetConfiguration.Test)
		{
			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		}

		// Internet play: OnlineSubsystemEOS (OSS v1, EOS Connect Device ID) on the
		// two client platforms the uproject enables it for (Win64 + Mac, Game and
		// Editor targets). The game calls EOS_Connect_CreateDeviceId /
		// DeleteDeviceId through the OSS platform handle, which no engine path
		// does. The Linux dedicated server and every other target compile the
		// internet path out (WOT_WITH_EOS=0): SocketSubsystemEOS has no Linux
		// module and NetDriverEOS reports unavailable on a dedicated server.
		if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			// WoTKeychainAccess: the no-UI probe of the EOS keychain item.
			PublicFrameworks.Add("Security");
			PublicFrameworks.Add("AppKit");
			PublicFrameworks.Add("UniformTypeIdentifiers");
		}

		bool bWithEOS =
			(Target.Platform == UnrealTargetPlatform.Win64
				|| Target.Platform == UnrealTargetPlatform.Mac)
			&& (Target.Type == TargetType.Game || Target.Type == TargetType.Editor);
		PublicDefinitions.Add("WOT_WITH_EOS=" + (bWithEOS ? "1" : "0"));
		if (bWithEOS)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"OnlineSubsystemEOS",
				"EOSShared",
				"EOSSDK"
			});
		}

		if (Target.bBuildEditor)
		{
			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				// Guarded-memory BDF regression exercises the same pinned parser as Slate.
				AddEngineThirdPartyPrivateStaticDependencies(Target, "FreeType2");
			}
			// The editor-only mesh weld regression creates a skeletal fixture from
			// MeshDescription before exercising the production strip/rebuild path.
			PrivateDependencyModuleNames.Add("SkeletalMeshUtilitiesCommon");
		}

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Per-user cache root discovery uses SHGetKnownFolderPath; cache
			// publication remains a direct unelevated filesystem operation.
			PublicSystemLibraries.AddRange(new string[]
			{
				"Ole32.lib",
				"Shell32.lib"
			});
		}
	}
}
