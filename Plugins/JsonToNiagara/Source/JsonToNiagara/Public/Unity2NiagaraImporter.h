#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

struct FUnityParticleExportRoot;

struct FUnity2NiagaraImportResult
{
	FSoftObjectPath NiagaraSystemPath;
	TArray<FSoftObjectPath> NiagaraEmitterPaths;
	TArray<FSoftObjectPath> MaterialInstancePaths;

	bool IsSuccess() const
	{
		return NiagaraSystemPath.IsValid();
	}
};

class FUnity2NiagaraImporter
{
public:
	FUnity2NiagaraImportResult CreateNiagaraSystemAsset(const FUnityParticleExportRoot& Root) const;

private:
	static FString MakeSafeObjectName(const FString& SourceName);
	static class UNiagaraEmitter* CreateEmitterAsset(
		class IAssetTools& AssetTools,
		const FString& PackagePath,
		const FString& SourceEmitterName);
	static class UMaterialInstanceConstant* CreateMaterialInstanceAsset(
		class IAssetTools& AssetTools,
		const FString& PackagePath,
		const FString& SourceEmitterName,
		class UMaterialInterface* ParentMaterial,
		class UTexture* ParticleTexture,
		const FLinearColor& ParticleColor,
		const FString& UnityBlendMode);
	static void ApplyEmitterMainSettingsToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void ApplyEmitterStateParametersToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void ApplyMaterialToEmitter(class UNiagaraEmitter& Emitter, class UMaterialInterface& Material);
	static void ApplyTrailRendererToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem, class UMaterialInterface* Material);
	static void ApplyRendererSettingsToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityRendererData& RendererData);
	static void ApplyTextureSheetAnimationToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityTextureSheetAnimationModule& TextureSheetAnimation);
	static void AddTextureSheetAnimationUpdateModuleToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void AddBurstSpawnModuleToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void AddBurstSpawnRateFallbackModuleToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void AddShapeLocationModuleToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void AddShapeVelocityModuleToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void AddSizeOverLifetimeModuleToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void AddColorOverLifetimeModuleToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void ApplyInitialNiagaraParametersToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void ApplyParticleSpawnSetParametersToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void ApplyParticleUpdateSetParametersToEmitter(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void LogEmitterVisibilityDiagnostics(class UNiagaraEmitter& Emitter, const struct FUnityParticleSystemData& ParticleSystem);
	static void LogNiagaraSystemDiagnostics(class UNiagaraSystem& System);
};
