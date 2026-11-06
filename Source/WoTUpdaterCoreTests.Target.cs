using UnrealBuildTool;

/// <summary>Standalone runner for the engine-free WoTUpdaterCore specs.</summary>
public class WoTUpdaterCoreTestsTarget : TargetRules
{
	public WoTUpdaterCoreTestsTarget(TargetInfo Target) : base(Target)
	{
		GlobalDefinitions.Add("WOT_GUARDED_BOOTSTRAP=0");
		Type = TargetType.Program;
		LinkType = TargetLinkType.Monolithic;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		LaunchModuleName = "WoTUpdaterCoreTests";

		bIsBuildingConsoleApplication = true;

		bCompileAgainstEngine = false;
		bCompileAgainstCoreUObject = false;
		bCompileAgainstApplicationCore = false;
		bBuildWithEditorOnlyData = false;
		bCompileWithPluginSupport = false;
		bBuildDeveloperTools = false;
		bCompileICU = false;
		bUsesSlate = false;
		bIsBuildingConsoleApplication = true;
	}
}
