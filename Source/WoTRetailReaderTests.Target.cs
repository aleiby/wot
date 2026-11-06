using System;
using UnrealBuildTool;

/// <summary>UObject-free Program runner for WoTRetailReader contract specs.</summary>
public class WoTRetailReaderTestsTarget : TargetRules
{
	public WoTRetailReaderTestsTarget(TargetInfo Target) : base(Target)
	{
		GlobalDefinitions.Add("WOT_GUARDED_BOOTSTRAP=0");
		Type = TargetType.Program;
		LinkType = TargetLinkType.Monolithic;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		LaunchModuleName = "WoTRetailReaderTests";
		bIsBuildingConsoleApplication = true;
		bCompileAgainstEngine = false;
		bCompileAgainstCoreUObject = false;
		bCompileAgainstApplicationCore = false;
		bBuildWithEditorOnlyData = false;
		bCompileWithPluginSupport = false;
		bBuildDeveloperTools = false;
		bCompileICU = false;
		bUsesSlate = false;

		// The guarded wrappers pass -WoTLinkMap=<path> so the link audit
		// (check_innoextract_link.py) gets a linker map. UBT has no global
		// command-line switch for extra linker arguments — it silently ignores
		// unrecognized -Name=Value args — so the request must be read from
		// Target.Arguments here and turned into the platform's linker syntax.
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
