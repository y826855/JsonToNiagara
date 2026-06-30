// Copyright Epic Games, Inc. All Rights Reserved.

#include "JsonToNiagara.h"
#include "JsonToNiagaraStyle.h"
#include "JsonToNiagaraCommands.h"
#include "Unity2NiagaraImporter.h"
#include "Unity2NiagaraTextureResolver.h"
#include "UnityParticleJsonParser.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditor.h"
#include "Misc/Paths.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "ToolMenus.h"

static const FName JsonToNiagaraTabName("JsonToNiagara");

#define LOCTEXT_NAMESPACE "FJsonToNiagaraModule"

namespace JsonToNiagara
{
	const TCHAR* DefaultTextureFolder = TEXT("/Game/Textures");
	const TCHAR* DefaultParticleMaterial = TEXT("/Game/Material/M_DefaultParticle.M_DefaultParticle");

	void LogImportPreview(const FString& SourceJsonPath, const FUnityParticleExportRoot& Root)
	{
		const FUnity2NiagaraTextureResolver TextureResolver(DefaultTextureFolder);
		int32 ImportEmitterCount = 0;
		int32 SkippedEmitterCount = 0;

		for (const FUnityParticleSystemData& ParticleSystem : Root.ParticleSystems)
		{
			if (ParticleSystem.bIsEmptyEmitter)
			{
				++SkippedEmitterCount;
			}
			else
			{
				++ImportEmitterCount;
			}
		}

		UE_LOG(LogTemp, Display, TEXT("[JsonToNiagara] Source JSON: %s"), *SourceJsonPath);
		UE_LOG(LogTemp, Display, TEXT("[JsonToNiagara] Root: %s"), *Root.RootName);
		UE_LOG(LogTemp, Display, TEXT("[JsonToNiagara] Particle Systems: %d"), Root.ParticleSystemCount);
		UE_LOG(LogTemp, Display, TEXT("[JsonToNiagara] Parsed Particle Systems: %d"), Root.ParticleSystems.Num());
		UE_LOG(LogTemp, Display, TEXT("[JsonToNiagara] Import Emitters: %d"), ImportEmitterCount);
		UE_LOG(LogTemp, Display, TEXT("[JsonToNiagara] Skip Empty Emitters: %d"), SkippedEmitterCount);
		UE_LOG(LogTemp, Display, TEXT("[JsonToNiagara] Texture search root: %s"), DefaultTextureFolder);
		UE_LOG(LogTemp, Display, TEXT("[JsonToNiagara] Default material: %s"), DefaultParticleMaterial);

		for (const FUnityParticleSystemData& ParticleSystem : Root.ParticleSystems)
		{
			UE_LOG(
				LogTemp,
				Display,
				TEXT("[JsonToNiagara] Emitter: %s%s"),
				*ParticleSystem.Name,
				ParticleSystem.bIsEmptyEmitter ? TEXT(" (Skipped: Empty Emitter)") : TEXT(""));

			UE_LOG(LogTemp, Display, TEXT("  Path: %s"), *ParticleSystem.Path);
			UE_LOG(LogTemp, Display, TEXT("  Type: %s"), *ParticleSystem.NiagaraHint.EmitterType);
			UE_LOG(LogTemp, Display, TEXT("  Spawn: %s"), *ParticleSystem.NiagaraHint.Spawn);
			UE_LOG(LogTemp, Display, TEXT("  BlendMode: %s"), *ParticleSystem.NiagaraHint.BlendMode);
			UE_LOG(LogTemp, Display, TEXT("  Renderer: %s / %s / SortingOrder %d"), *ParticleSystem.Renderer.RenderMode, *ParticleSystem.Renderer.Alignment, ParticleSystem.Renderer.SortingOrder);
			UE_LOG(LogTemp, Display, TEXT("  EnabledModules: %s"), *FString::Join(ParticleSystem.EnabledModules, TEXT(", ")));
			const FLinearColor ParticleColor = ParticleSystem.ColorOverLifetime.bEnabled && ParticleSystem.ColorOverLifetime.bHasRepresentativeColor
				? ParticleSystem.ColorOverLifetime.RepresentativeColor.ToLinearColor()
				: ParticleSystem.Main.StartColor.ToLinearColor();
			UE_LOG(
				LogTemp,
				Display,
				TEXT("  Main: Duration %.3f / Loop %s / Lifetime %.3f / Size %.3f / Speed %.3f / MaterialColor %.3f, %.3f, %.3f, %.3f"),
				ParticleSystem.Main.Duration,
				ParticleSystem.Main.bLoop ? TEXT("true") : TEXT("false"),
				ParticleSystem.Main.StartLifetime.GetRepresentativeValue(),
				ParticleSystem.Main.StartSize.GetRepresentativeValue(),
				ParticleSystem.Main.StartSpeed.GetRepresentativeValue(),
				ParticleColor.R,
				ParticleColor.G,
				ParticleColor.B,
				ParticleColor.A);
			if (ParticleSystem.ColorOverLifetime.bEnabled)
			{
				const FLinearColor ColorStart = ParticleSystem.ColorOverLifetime.StartColor.ToLinearColor();
				const FLinearColor ColorEnd = ParticleSystem.ColorOverLifetime.EndColor.ToLinearColor();
				UE_LOG(
					LogTemp,
					Display,
					TEXT("  ColorOverLifetime: Enabled / RepresentativeColor %s / ColorKeys %d / AlphaKeys %d / Start %.3f, %.3f, %.3f, %.3f / End %.3f, %.3f, %.3f, %.3f"),
					ParticleSystem.ColorOverLifetime.bHasRepresentativeColor ? TEXT("yes") : TEXT("no"),
					ParticleSystem.ColorOverLifetime.ColorKeys.Num(),
					ParticleSystem.ColorOverLifetime.AlphaKeys.Num(),
					ColorStart.R,
					ColorStart.G,
					ColorStart.B,
					ColorStart.A,
					ColorEnd.R,
					ColorEnd.G,
					ColorEnd.B,
					ColorEnd.A);
			}

			if (ParticleSystem.Shape.bEnabled)
			{
				UE_LOG(
					LogTemp,
					Display,
					TEXT("  Shape: %s / Radius %.3f / Angle %.3f / Arc %.3f %s / ArcSpeed %.3f"),
					*ParticleSystem.Shape.ShapeType,
					ParticleSystem.Shape.Radius,
					ParticleSystem.Shape.Angle,
					ParticleSystem.Shape.Arc,
					*ParticleSystem.Shape.ArcMode,
					ParticleSystem.Shape.ArcSpeed.GetRepresentativeValue());
			}

			if (ParticleSystem.SizeOverLifetime.bEnabled)
			{
				UE_LOG(
					LogTemp,
					Display,
					TEXT("  SizeOverLifetime: SeparateAxes %s / SizeKeys %d / Size %.3f -> %.3f"),
					ParticleSystem.SizeOverLifetime.bSeparateAxes ? TEXT("true") : TEXT("false"),
					ParticleSystem.SizeOverLifetime.Size.Keys.Num(),
					ParticleSystem.SizeOverLifetime.Size.GetFirstKeyValue(ParticleSystem.SizeOverLifetime.Size.GetRepresentativeValue(1.0f)),
					ParticleSystem.SizeOverLifetime.Size.GetLastKeyValue(ParticleSystem.SizeOverLifetime.Size.GetRepresentativeValue(1.0f)));
			}

			if (ParticleSystem.TextureSheetAnimation.bEnabled)
			{
				UE_LOG(
					LogTemp,
					Display,
					TEXT("  TextureSheetAnimation: %s / Tiles %dx%d / Animation %s / StartFrame %.3f / FrameOverTime %.3f / Sprites %d"),
					*ParticleSystem.TextureSheetAnimation.Mode,
					ParticleSystem.TextureSheetAnimation.NumTilesX,
					ParticleSystem.TextureSheetAnimation.NumTilesY,
					*ParticleSystem.TextureSheetAnimation.Animation,
					ParticleSystem.TextureSheetAnimation.StartFrame.GetRepresentativeValue(),
					ParticleSystem.TextureSheetAnimation.FrameOverTime.GetRepresentativeValue(),
					ParticleSystem.TextureSheetAnimation.Sprites.Num());
			}

			for (const FUnityBurstData& Burst : ParticleSystem.Emission.Bursts)
			{
				UE_LOG(LogTemp, Display, TEXT("  Burst: Time %.3f, Count %d"), Burst.Time, Burst.Count);
			}

			if (ParticleSystem.TextureReferences.IsEmpty())
			{
				UE_LOG(LogTemp, Display, TEXT("  Texture: <none>"));
			}
			else
			{
				for (const FUnityTextureReference& TextureReference : ParticleSystem.TextureReferences)
				{
					const FUnityResolvedTexture ResolvedTexture = TextureResolver.Resolve(TextureReference);
					if (ResolvedTexture.IsResolved())
					{
						UE_LOG(
							LogTemp,
							Display,
							TEXT("  Texture: %s -> %s (Source: %s, UnityPath: %s)"),
							*ResolvedTexture.SearchName,
							*ResolvedTexture.AssetPath.ToString(),
							*TextureReference.Source,
							*TextureReference.Texture.AssetPath);
					}
					else
					{
						UE_LOG(
							LogTemp,
							Warning,
							TEXT("  Texture: %s -> Missing (Source: %s, UnityPath: %s)"),
							*ResolvedTexture.SearchName,
							*TextureReference.Source,
							*TextureReference.Texture.AssetPath);
					}
				}
			}
		}
	}
}

void FJsonToNiagaraModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	
	FJsonToNiagaraStyle::Initialize();
	FJsonToNiagaraStyle::ReloadTextures();

	FJsonToNiagaraCommands::Register();
	
	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FJsonToNiagaraCommands::Get().OpenPluginWindow,
		FExecuteAction::CreateRaw(this, &FJsonToNiagaraModule::PluginButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FJsonToNiagaraModule::RegisterMenus));
	
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(JsonToNiagaraTabName, FOnSpawnTab::CreateRaw(this, &FJsonToNiagaraModule::OnSpawnPluginTab))
		.SetDisplayName(LOCTEXT("FJsonToNiagaraTabTitle", "JsonToNiagara"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FJsonToNiagaraModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.

	UToolMenus::UnRegisterStartupCallback(this);

	UToolMenus::UnregisterOwner(this);

	FJsonToNiagaraStyle::Shutdown();

	FJsonToNiagaraCommands::Unregister();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(JsonToNiagaraTabName);
}

TSharedRef<SDockTab> FJsonToNiagaraModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
	FText WidgetText = FText::Format(
		LOCTEXT("WindowWidgetText", "Add code to {0} in {1} to override this window's contents"),
		FText::FromString(TEXT("FJsonToNiagaraModule::OnSpawnPluginTab")),
		FText::FromString(TEXT("JsonToNiagara.cpp"))
		);

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			// Put your tab content here!
			SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(WidgetText)
			]
		];
}

void FJsonToNiagaraModule::PluginButtonClicked()
{
	ImportUnityParticleJson();
}

void FJsonToNiagaraModule::ImportUnityParticleJson()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("[JsonToNiagara] DesktopPlatform module is not available."));
		return;
	}

	const void* ParentWindowHandle = nullptr;
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr))
	{
		ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	}

	TArray<FString> SelectedFiles;
	const bool bOpened = DesktopPlatform->OpenFileDialog(
		ParentWindowHandle,
		TEXT("Import Unity Particle JSON"),
		FPaths::ProjectDir(),
		TEXT(""),
		TEXT("JSON files (*.json)|*.json"),
		EFileDialogFlags::None,
		SelectedFiles);

	if (!bOpened || SelectedFiles.IsEmpty())
	{
		UE_LOG(LogTemp, Display, TEXT("[JsonToNiagara] Import canceled."));
		return;
	}

	const FString JsonFilePath = SelectedFiles[0];
	UE_LOG(LogTemp, Display, TEXT("[JsonToNiagara] Selected JSON: %s"), *JsonFilePath);

	FUnityParticleExportRoot ParsedRoot;
	FText ParseError;
	if (!FUnityParticleJsonParser::ParseFile(JsonFilePath, ParsedRoot, ParseError))
	{
		UE_LOG(LogTemp, Error, TEXT("[JsonToNiagara] %s"), *ParseError.ToString());
		return;
	}

	JsonToNiagara::LogImportPreview(JsonFilePath, ParsedRoot);

	const FUnity2NiagaraImporter Importer;
	const FUnity2NiagaraImportResult ImportResult = Importer.CreateNiagaraSystemAsset(ParsedRoot);
	if (!ImportResult.IsSuccess())
	{
		UE_LOG(LogTemp, Error, TEXT("[JsonToNiagara] Import stopped because Niagara System creation failed."));
	}
}

void FJsonToNiagaraModule::RegisterMenus()
{
	// Owner will be used for cleanup in call to UToolMenus::UnregisterOwner
	FToolMenuOwnerScoped OwnerScoped(this);

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("JsonToNiagara", LOCTEXT("JsonToNiagaraMenuSection", "JsonToNiagara"));
			Section.AddMenuEntryWithCommandList(FJsonToNiagaraCommands::Get().OpenPluginWindow, PluginCommands);
		}
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");
			{
				FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FJsonToNiagaraCommands::Get().OpenPluginWindow));
				Entry.SetCommandList(PluginCommands);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FJsonToNiagaraModule, JsonToNiagara)
