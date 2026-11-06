using System;
using System.Linq;
using UnrealBuildTool;

public class WoTTarget : TargetRules
{
	public WoTTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("WoT");

		bool bGuardedConfiguration =
			Target.Configuration == UnrealTargetConfiguration.Test ||
			Target.Configuration == UnrealTargetConfiguration.Shipping;

		bool bTestIdentityRequested = DefinitionRequested(
			Target, "WOT_BOOTSTRAP_TEST_IDENTITIES");
		bool bTestEndpointRequested = DefinitionRequested(
			Target, "WOT_BOOTSTRAP_TEST_ENDPOINTS");
		if (Target.Configuration != UnrealTargetConfiguration.Test &&
			(bTestIdentityRequested || bTestEndpointRequested))
		{
			throw new BuildException(
				"WOT_BOOTSTRAP_TEST_IDENTITIES and WOT_BOOTSTRAP_TEST_ENDPOINTS " +
				"are legal only in UnrealTargetConfiguration.Test");
		}

		if (!bGuardedConfiguration)
		{
			GlobalDefinitions.Add("WOT_GUARDED_BOOTSTRAP=0");
		}

		if (bGuardedConfiguration)
		{
			LinkType = TargetLinkType.Monolithic;
			ExtraModuleNames.Add("WoTLaunch");
			GlobalDefinitions.Add("WOT_GUARDED_BOOTSTRAP=1");
			// Guarded games own one explicit data root. Generic crash-context
			// initialization can otherwise create Epic-product marker/config state
			// through UserSettingsDir before our module starts. The standalone
			// CrashReportClient is deliberately neither built nor staged.
			GlobalDefinitions.Add("NOINITCRASHREPORTER=1");
			GlobalDefinitions.Add("CRASH_REPORTER_WITH_ANALYTICS=0");
			GlobalDefinitions.Add("USE_CRASH_REPORTER_MONITOR=0");

			if (Target.Configuration == UnrealTargetConfiguration.Test)
			{
				GlobalDefinitions.Add("WOT_BOOTSTRAP_TEST_IDENTITIES=1");
				GlobalDefinitions.Add("WOT_BOOTSTRAP_TEST_ENDPOINTS=1");
				GlobalDefinitions.Add(
					"WOT_BOOTSTRAP_COMPILED_COMPONENT_IDENTITY=\"wot-game-test-v1\"");
				GlobalDefinitions.Add(
					"WOT_BOOTSTRAP_LIFECYCLE_ENDPOINT=\"127.0.0.1:38429\"");
				// TraceLog's remote-control listener binds 0.0.0.0:1985 in every
				// non-Shipping build, so each new install path of a Test game
				// raised a Windows Firewall prompt. Insights
				// can still trace a Test game it launches with -trace; only the
				// inbound control socket goes. Shipping compiles trace out anyway.
				GlobalDefinitions.Add("UE_TRACE_ALLOW_TCP_CONTROL=0");
			}
			else
			{
				// Shipping binds production policy and deliberately has no test-only
				// definition in its compile environment or target receipt.
				GlobalDefinitions.Add(
					"WOT_BOOTSTRAP_COMPILED_COMPONENT_IDENTITY=\"wot-game-production-v1\"");
				GlobalDefinitions.Add(
					"WOT_BOOTSTRAP_LIFECYCLE_ENDPOINT=\"production\"");
			}

			if (Target.Platform == UnrealTargetPlatform.Mac && ProjectFile != null)
			{
				// UBT's path-masking VFS is off for Apple toolchains, so __FILE__
				// in check/ensure strings would carry the build machine's absolute
				// project path into the shipped binary. Strip it to repo-relative.
				AdditionalCompilerArguments +=
					" -ffile-prefix-map=\"" + ProjectFile.Directory.FullName + "/\"=";
				// The debug map (N_OSO) names every object file by its absolute
				// build path; UBT strips it only for Shipping, so strip Test too.
				MacPlatform.bStripSymbols = true;
				// The strip action is only kept in the graph when it produces a file.
				MacPlatform.bCreateStripFlagFile = true;
			}

			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				// WoTLaunch.Build.cs records this exact value in the target receipt;
				// every Win64 BuildCookRun/package wrapper must pass it through.
				GlobalDefinitions.Add(
					"WOT_REQUIRED_UAT_ARGUMENT_NO_BOOTSTRAP_EXE=1"); // -NoBootstrapExe
			}
		}
	}

	private bool DefinitionRequested(TargetInfo Target, string Name)
	{
		Func<string, bool> Matches = Value =>
			Value.Equals(Name, StringComparison.Ordinal) ||
			Value.StartsWith(Name + "=", StringComparison.Ordinal);

		return GlobalDefinitions.Any(Matches) ||
			ProjectDefinitions.Any(Matches) ||
			(Target.Arguments != null && Target.Arguments.GetRawArray().Any(
				Value => Value.IndexOf(Name, StringComparison.Ordinal) >= 0));
	}
}
