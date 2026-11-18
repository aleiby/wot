using UnrealBuildTool;

/// <summary>Core-only Program runner for the update-transport loopback suite.
/// This is the ONLY build where WOT_BOOTSTRAP_TEST_ENDPOINTS-gated specs can
/// execute on both lanes: the guarded Test GAME cannot force automation
/// compilation without dragging Epic's own (Test-config-broken) engine test
/// sources into the build, and every other target leaves the macro 0.</summary>
public class WoTUpdateTransportsTestsTarget : TargetRules
{
	public WoTUpdateTransportsTestsTarget(TargetInfo Target) : base(Target)
	{
		GlobalDefinitions.Add("WOT_GUARDED_BOOTSTRAP=0");
		Type = TargetType.Program;
		LinkType = TargetLinkType.Monolithic;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		LaunchModuleName = "WoTUpdateTransportsTests";
		bIsBuildingConsoleApplication = true;
		bCompileAgainstEngine = false;
		bCompileAgainstCoreUObject = false;
		bCompileAgainstApplicationCore = false;
		bBuildWithEditorOnlyData = false;
		bCompileWithPluginSupport = false;
		bBuildDeveloperTools = false;
		bCompileICU = false;
		bUsesSlate = false;
		bForceCompileDevelopmentAutomationTests = true;
		bForceCompilePerformanceAutomationTests = true;
		GlobalDefinitions.Add("WOT_BOOTSTRAP_TEST_ENDPOINTS=1");
	}
}
