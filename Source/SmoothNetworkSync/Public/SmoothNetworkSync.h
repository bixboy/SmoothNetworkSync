// Copyright (c) Bixboy, 2026. All Rights Reserved.

#pragma once

#ifndef __has_feature
	#define __has_feature(x) 0
#endif

#include "Modules/ModuleManager.h"

class FSmoothNetworkSyncModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
