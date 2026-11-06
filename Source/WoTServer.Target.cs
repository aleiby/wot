using UnrealBuildTool;

// Dedicated server (Linux x86-64 hosted games). Kept green in CI.
public class WoTServerTarget : TargetRules
{
	public WoTServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("WoT");

		// Match the game target's guarded-configuration boundary. A Shipping
		// dedicated server must never compile developer/test paths under guard=0.
		bool bGuardedConfiguration =
			Target.Configuration == UnrealTargetConfiguration.Test ||
			Target.Configuration == UnrealTargetConfiguration.Shipping;
		GlobalDefinitions.Add(
			bGuardedConfiguration
				? "WOT_GUARDED_BOOTSTRAP=1"
				: "WOT_GUARDED_BOOTSTRAP=0");
	}
}
