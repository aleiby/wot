using UnrealBuildTool;

/// <summary>
/// Standalone runner for the WoTBootstrapCore automation specs and the
/// canonical-JSON round-trip protocol.
///
/// A Program target, not an editor commandlet, because the module under test is
/// UObject-free by contract: the specs must be provable without CoreUObject, the
/// Engine, or a project. The same binary also serves the length-prefixed
/// round-trip protocol that Tools/dist/artifact_roundtrip.py drives, so the
/// Python and C++ canonical writers are compared as bytes rather than by
/// inspection.
/// </summary>
public class WoTBootstrapCoreTestsTarget : TargetRules
{
	public WoTBootstrapCoreTestsTarget(TargetInfo Target) : base(Target)
	{
		GlobalDefinitions.Add("WOT_GUARDED_BOOTSTRAP=0");
		Type = TargetType.Program;
		LinkType = TargetLinkType.Monolithic;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		LaunchModuleName = "WoTBootstrapCoreTests";

		bIsBuildingConsoleApplication = true;

		// The dependency floor is the point of this target: if the runner ever
		// needs CoreUObject or the Engine to link, WoTBootstrapCore has acquired a
		// dependency it is not allowed to have.
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
