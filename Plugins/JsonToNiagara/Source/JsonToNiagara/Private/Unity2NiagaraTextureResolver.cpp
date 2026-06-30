#include "Unity2NiagaraTextureResolver.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UnityParticleJsonTypes.h"

FUnity2NiagaraTextureResolver::FUnity2NiagaraTextureResolver(FName InTextureRootPath)
	: TextureRootPath(InTextureRootPath)
{
}

FUnityResolvedTexture FUnity2NiagaraTextureResolver::Resolve(const FUnityTextureReference& TextureReference) const
{
	CacheTextureAssets();

	FUnityResolvedTexture Result;
	Result.SearchName = GetTextureSearchName(TextureReference);
	if (Result.SearchName.IsEmpty())
	{
		return Result;
	}

	if (const FSoftObjectPath* AssetPath = TexturePathByLowerName.Find(Result.SearchName.ToLower()))
	{
		Result.AssetPath = *AssetPath;
	}

	return Result;
}

void FUnity2NiagaraTextureResolver::CacheTextureAssets() const
{
	if (bCacheBuilt)
	{
		return;
	}

	bCacheBuilt = true;
	TexturePathByLowerName.Reset();

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> TextureAssets;
	AssetRegistryModule.Get().GetAssetsByPath(TextureRootPath, TextureAssets, true);

	const FTopLevelAssetPath Texture2DClassPath = UTexture2D::StaticClass()->GetClassPathName();
	for (const FAssetData& TextureAsset : TextureAssets)
	{
		if (TextureAsset.AssetClassPath != Texture2DClassPath)
		{
			continue;
		}

		const FString AssetName = TextureAsset.AssetName.ToString();
		if (!AssetName.IsEmpty())
		{
			TexturePathByLowerName.Add(AssetName.ToLower(), TextureAsset.GetSoftObjectPath());
		}
	}
}

FString FUnity2NiagaraTextureResolver::GetTextureSearchName(const FUnityTextureReference& TextureReference)
{
	if (!TextureReference.Texture.Name.IsEmpty())
	{
		return TextureReference.Texture.Name;
	}

	return FPaths::GetBaseFilename(TextureReference.Texture.AssetPath);
}
