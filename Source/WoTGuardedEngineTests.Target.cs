using System;
using UnrealBuildTool;

public class WoTGuardedEngineTestsTarget : TargetRules
{
	public WoTGuardedEngineTestsTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Program;
		LinkType = TargetLinkType.Monolithic;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		LaunchModuleName = "WoTGuardedEngineTests";

		bIsBuildingConsoleApplication = true;
		bCompileAgainstEngine = true;
		bCompileAgainstCoreUObject = true;
		bCompileAgainstApplicationCore = true;
		// Stays false: this program must match the guarded cooked-data
		// configuration it exists to test. Turning editor-only data on to read
		// the engine's uncooked content does not compile here — the engine
		// headers gate PostEditChangeProperty/NotifyGraphRenamed on WITH_EDITOR,
		// which a non-editor Program does not have.
		bBuildWithEditorOnlyData = false;
		bCompileWithPluginSupport = true;
		// This program tests world-start paths; it has no use for target-platform
		// tooling. Leaving developer tools on drags in Android/iOS
		// TargetPlatformControls and FileUtilities, which fail to compile here
		// (missing platform SDKs and libzip) for reasons unrelated to the patch.
		bBuildDeveloperTools = false;
		// NOT forced in Shipping. These are target-wide, so they switch engine
		// automation tests back on for every module, and stock UE 5.8 does not
		// compile that way: Analytics' AnalyticsPropertyStoreTests.cpp declares
		// its Actual_* locals under `#if DO_CHECK`, which Shipping sets to 0,
		// while `check()` there becomes CA_ASSUME(expr) -> sizeof((bool)(expr))
		// and still type-checks the vanished names. That bug is unfixed upstream
		// — the file is byte-identical on 5.8, ue5-main and our pinned release.
		// This program only needs automation in ITS OWN module, which
		// WoTGuardedEngineTests.Build.cs turns on via PrivateDefinitions.
		if (Target.Configuration != UnrealTargetConfiguration.Shipping)
		{
			bForceCompileDevelopmentAutomationTests = true;
			bForceCompilePerformanceAutomationTests = true;
		}

		GlobalDefinitions.Add("WOT_GUARDED_BOOTSTRAP=1");
		// This program supplies its own process entry (WoTGuardedEngineTestsMain.cpp)
		// while depending on Launch for GEngineLoop. The patched Launch platform
		// files compile out their stock entry when this is set, keeping exactly one
		// main in the link. The WoT game target deliberately does NOT set it — the
		// game uses the stock engine entry point.
		GlobalDefinitions.Add("WOT_PROJECT_OWNS_ENTRYPOINT=1");

		// The guarded wrapper's --link-map request is a project argument, not a
		// built-in UBT option. Translate it here so the Shipping startup/link audit
		// receives the real monolithic link graph instead of a silently ignored
		// command-line token.
		if (Target.Arguments != null)
		{
			foreach (string Argument in Target.Arguments.GetRawArray())
			{
				const string Prefix = "-WoTLinkMap=";
				if (Argument.StartsWith(Prefix, StringComparison.Ordinal))
				{
					string MapPath = Argument.Substring(Prefix.Length);
					AdditionalLinkerArguments =
						Target.Platform == UnrealTargetPlatform.Win64
							? "/MAP:\"" + MapPath + "\""
							: "-Wl,-map," + MapPath;
				}
			}
		}
	}
}
