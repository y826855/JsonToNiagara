#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

struct FUnityTextureReference;

struct FUnityResolvedTexture
{
	FString SearchName;
	FSoftObjectPath AssetPath;

	bool IsResolved() const
	{
		return AssetPath.IsValid();
	}
};

class FUnity2NiagaraTextureResolver
{
public:
	explicit FUnity2NiagaraTextureResolver(FName InTextureRootPath = TEXT("/Game/Textures"));

	FUnityResolvedTexture Resolve(const FUnityTextureReference& TextureReference) const;

private:
	void CacheTextureAssets() const;
	static FString GetTextureSearchName(const FUnityTextureReference& TextureReference);

private:
	FName TextureRootPath;
	mutable bool bCacheBuilt = false;
	mutable TMap<FString, FSoftObjectPath> TexturePathByLowerName;
};
