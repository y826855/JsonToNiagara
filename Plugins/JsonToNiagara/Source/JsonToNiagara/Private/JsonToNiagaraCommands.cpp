// Copyright Epic Games, Inc. All Rights Reserved.

#include "JsonToNiagaraCommands.h"

#define LOCTEXT_NAMESPACE "FJsonToNiagaraModule"

void FJsonToNiagaraCommands::RegisterCommands()
{
	UI_COMMAND(OpenPluginWindow, "Import Unity Particle JSON", "Import a Unity ParticleSystem export JSON for Niagara preview.", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
