#pragma once

#include "CoreMinimal.h"
#include "UnityParticleJsonTypes.h"

class FUnityParticleJsonParser
{
public:
	static bool ParseFile(const FString& FilePath, FUnityParticleExportRoot& OutRoot, FText& OutError);
	static bool ParseString(const FString& JsonString, FUnityParticleExportRoot& OutRoot, FText& OutError);

private:
	static void ParseParticleSystem(const TSharedPtr<FJsonObject>& JsonObject, FUnityParticleSystemData& OutParticleSystem);
	static void ParseNiagaraHint(const TSharedPtr<FJsonObject>& JsonObject, FUnityNiagaraHint& OutHint);
	static void ParseTextureReferences(const TArray<TSharedPtr<FJsonValue>>& JsonValues, TArray<FUnityTextureReference>& OutTextureReferences);
	static void ParseMain(const TSharedPtr<FJsonObject>& JsonObject, FUnityMainModule& OutMain);
	static FUnityVector2Data ParseVector2(const TSharedPtr<FJsonObject>& JsonObject);
	static FUnityVector3Data ParseVector3(const TSharedPtr<FJsonObject>& JsonObject);
	static FUnityMinMaxCurveData ParseMinMaxCurve(const TSharedPtr<FJsonObject>& JsonObject);
	static FUnityColorData ParseColor(const TSharedPtr<FJsonObject>& JsonObject);
	static void ParseColorOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnityColorOverLifetimeModule& OutColorOverLifetime);
	static void ParseEmission(const TSharedPtr<FJsonObject>& JsonObject, FUnityEmissionModule& OutEmission);
	static void ParseShape(const TSharedPtr<FJsonObject>& JsonObject, FUnityShapeModule& OutShape);
	static void ParseSizeOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnitySizeOverLifetimeModule& OutSizeOverLifetime);
	static void ParseRotationOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnityRotationOverLifetimeModule& OutRotationOverLifetime);
	static void ParseVelocityOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnityVelocityOverLifetimeModule& OutVelocityOverLifetime);
	static void ParseForceOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnityForceOverLifetimeModule& OutForceOverLifetime);
	static void ParseLimitVelocityOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnityLimitVelocityOverLifetimeModule& OutLimitVelocityOverLifetime);
	static void ParseNoise(const TSharedPtr<FJsonObject>& JsonObject, FUnityNoiseModule& OutNoise);
	static void ParseTrails(const TSharedPtr<FJsonObject>& JsonObject, FUnityTrailsModule& OutTrails);
	static void ParseTextureSheetAnimation(const TSharedPtr<FJsonObject>& JsonObject, FUnityTextureSheetAnimationModule& OutTextureSheetAnimation);
	static void ParseRenderer(const TSharedPtr<FJsonObject>& JsonObject, FUnityRendererData& OutRenderer);
};
