// Copyright (c) Bixboy, 2026. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FSmoothNetworkSyncModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
