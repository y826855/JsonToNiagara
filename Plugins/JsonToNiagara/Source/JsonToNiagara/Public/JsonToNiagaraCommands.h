// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Framework/Commands/Commands.h"
#include "JsonToNiagaraStyle.h"

class FJsonToNiagaraCommands : public TCommands<FJsonToNiagaraCommands>
{
public:

	FJsonToNiagaraCommands()
		: TCommands<FJsonToNiagaraCommands>(TEXT("JsonToNiagara"), NSLOCTEXT("Contexts", "JsonToNiagara", "JsonToNiagara Plugin"), NAME_None, FJsonToNiagaraStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > OpenPluginWindow;
};