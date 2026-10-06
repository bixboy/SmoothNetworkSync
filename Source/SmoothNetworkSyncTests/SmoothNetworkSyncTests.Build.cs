// Copyright (c) Bixboy, 2026. All Rights Reserved.

using UnrealBuildTool;

/** Editor-only network regression tests (multiplayer PIE on the example map). Run: Automation RunTests SmoothSync */
public class SmoothNetworkSyncTests : ModuleRules
{
	public SmoothNetworkSyncTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Same engine header workaround as the runtime module (see SmoothNetworkSync.h).
#pragma warning disable CS0618
		if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion == 4)
		{
			bEnableUndefinedIdentifierWarnings = false;
			PCHUsage = ModuleRules.PCHUsageMode.NoSharedPCHs;
			PrivatePCHHeaderFile = "Private/SmoothNetworkSyncTestsPCH.h";
		}
		else if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 5)
		{
			bEnableUndefinedIdentifierWarnings = false;
		}
#pragma warning restore CS0618

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealEd",
				"SmoothNetworkSync",
				"SmoothNetworkSyncExamples",
			}
			);
	}
}
