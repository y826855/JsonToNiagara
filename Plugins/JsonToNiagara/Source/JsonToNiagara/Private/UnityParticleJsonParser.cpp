#include "UnityParticleJsonParser.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UnityParticleJsonParser
{
	FString GetStringField(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName)
	{
		FString Value;
		if (JsonObject.IsValid())
		{
			JsonObject->TryGetStringField(FieldName, Value);
		}
		return Value;
	}

	int32 GetIntField(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName)
	{
		int32 Value = 0;
		if (JsonObject.IsValid())
		{
			JsonObject->TryGetNumberField(FieldName, Value);
		}
		return Value;
	}

	float GetFloatField(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName)
	{
		float Value = 0.0f;
		if (JsonObject.IsValid())
		{
			JsonObject->TryGetNumberField(FieldName, Value);
		}
		return Value;
	}
}

bool FUnityParticleJsonParser::ParseFile(const FString& FilePath, FUnityParticleExportRoot& OutRoot, FText& OutError)
{
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		OutError = FText::Format(NSLOCTEXT("JsonToNiagara", "LoadJsonFailed", "Failed to read JSON file: {0}"), FText::FromString(FilePath));
		return false;
	}

	return ParseString(JsonString, OutRoot, OutError);
}

bool FUnityParticleJsonParser::ParseString(const FString& JsonString, FUnityParticleExportRoot& OutRoot, FText& OutError)
{
	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		OutError = FText::FromString(TEXT("Failed to parse Unity particle JSON."));
		return false;
	}

	OutRoot = FUnityParticleExportRoot();
	OutRoot.ExportVersion = UnityParticleJsonParser::GetStringField(RootObject, TEXT("exportVersion"));
	OutRoot.RootName = UnityParticleJsonParser::GetStringField(RootObject, TEXT("rootName"));
	OutRoot.RootAssetPath = UnityParticleJsonParser::GetStringField(RootObject, TEXT("rootAssetPath"));
	OutRoot.ParticleSystemCount = UnityParticleJsonParser::GetIntField(RootObject, TEXT("particleSystemCount"));

	const TArray<TSharedPtr<FJsonValue>>* ParticleSystemValues = nullptr;
	if (RootObject->TryGetArrayField(TEXT("particleSystems"), ParticleSystemValues))
	{
		for (const TSharedPtr<FJsonValue>& ParticleSystemValue : *ParticleSystemValues)
		{
			const TSharedPtr<FJsonObject> ParticleSystemObject = ParticleSystemValue.IsValid() ? ParticleSystemValue->AsObject() : nullptr;
			if (!ParticleSystemObject.IsValid())
			{
				continue;
			}

			FUnityParticleSystemData ParticleSystem;
			ParseParticleSystem(ParticleSystemObject, ParticleSystem);
			OutRoot.ParticleSystems.Add(MoveTemp(ParticleSystem));
		}
	}

	if (OutRoot.ParticleSystemCount == 0)
	{
		OutRoot.ParticleSystemCount = OutRoot.ParticleSystems.Num();
	}

	return true;
}

void FUnityParticleJsonParser::ParseParticleSystem(const TSharedPtr<FJsonObject>& JsonObject, FUnityParticleSystemData& OutParticleSystem)
{
	OutParticleSystem.Name = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("name"));
	OutParticleSystem.Path = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("path"));
	JsonObject->TryGetBoolField(TEXT("isEmptyEmitter"), OutParticleSystem.bIsEmptyEmitter);

	const TArray<TSharedPtr<FJsonValue>>* EnabledModuleValues = nullptr;
	if (JsonObject->TryGetArrayField(TEXT("enabledModules"), EnabledModuleValues))
	{
		for (const TSharedPtr<FJsonValue>& EnabledModuleValue : *EnabledModuleValues)
		{
			FString EnabledModule;
			if (EnabledModuleValue.IsValid() && EnabledModuleValue->TryGetString(EnabledModule))
			{
				OutParticleSystem.EnabledModules.Add(EnabledModule);
			}
		}
	}

	const TSharedPtr<FJsonObject>* NiagaraHintObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("niagaraHint"), NiagaraHintObject))
	{
		ParseNiagaraHint(*NiagaraHintObject, OutParticleSystem.NiagaraHint);
	}

	const TArray<TSharedPtr<FJsonValue>>* TextureReferenceValues = nullptr;
	if (JsonObject->TryGetArrayField(TEXT("textureReferences"), TextureReferenceValues))
	{
		ParseTextureReferences(*TextureReferenceValues, OutParticleSystem.TextureReferences);
	}

	const TSharedPtr<FJsonObject>* MainObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("main"), MainObject))
	{
		ParseMain(*MainObject, OutParticleSystem.Main);
	}

	const TSharedPtr<FJsonObject>* ColorOverLifetimeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("colorOverLifetime"), ColorOverLifetimeObject))
	{
		ParseColorOverLifetime(*ColorOverLifetimeObject, OutParticleSystem.ColorOverLifetime);
	}

	const TSharedPtr<FJsonObject>* EmissionObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("emission"), EmissionObject))
	{
		ParseEmission(*EmissionObject, OutParticleSystem.Emission);
	}

	const TSharedPtr<FJsonObject>* ShapeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("shape"), ShapeObject))
	{
		ParseShape(*ShapeObject, OutParticleSystem.Shape);
	}

	const TSharedPtr<FJsonObject>* SizeOverLifetimeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("sizeOverLifetime"), SizeOverLifetimeObject))
	{
		ParseSizeOverLifetime(*SizeOverLifetimeObject, OutParticleSystem.SizeOverLifetime);
	}

	const TSharedPtr<FJsonObject>* RotationOverLifetimeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("rotationOverLifetime"), RotationOverLifetimeObject))
	{
		ParseRotationOverLifetime(*RotationOverLifetimeObject, OutParticleSystem.RotationOverLifetime);
	}

	const TSharedPtr<FJsonObject>* VelocityOverLifetimeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("velocityOverLifetime"), VelocityOverLifetimeObject))
	{
		ParseVelocityOverLifetime(*VelocityOverLifetimeObject, OutParticleSystem.VelocityOverLifetime);
	}

	const TSharedPtr<FJsonObject>* ForceOverLifetimeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("forceOverLifetime"), ForceOverLifetimeObject))
	{
		ParseForceOverLifetime(*ForceOverLifetimeObject, OutParticleSystem.ForceOverLifetime);
	}

	const TSharedPtr<FJsonObject>* LimitVelocityOverLifetimeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("limitVelocityOverLifetime"), LimitVelocityOverLifetimeObject))
	{
		ParseLimitVelocityOverLifetime(*LimitVelocityOverLifetimeObject, OutParticleSystem.LimitVelocityOverLifetime);
	}

	const TSharedPtr<FJsonObject>* NoiseObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("noise"), NoiseObject))
	{
		ParseNoise(*NoiseObject, OutParticleSystem.Noise);
	}

	const TSharedPtr<FJsonObject>* TrailsObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("trails"), TrailsObject))
	{
		ParseTrails(*TrailsObject, OutParticleSystem.Trails);
	}

	const TSharedPtr<FJsonObject>* TextureSheetAnimationObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("textureSheetAnimation"), TextureSheetAnimationObject))
	{
		ParseTextureSheetAnimation(*TextureSheetAnimationObject, OutParticleSystem.TextureSheetAnimation);
	}

	const TSharedPtr<FJsonObject>* RendererObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("renderer"), RendererObject))
	{
		ParseRenderer(*RendererObject, OutParticleSystem.Renderer);
	}
}

void FUnityParticleJsonParser::ParseNiagaraHint(const TSharedPtr<FJsonObject>& JsonObject, FUnityNiagaraHint& OutHint)
{
	OutHint.EmitterType = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("emitterType"));
	OutHint.Spawn = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("spawn"));
	OutHint.BlendMode = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("blendMode"));
}

void FUnityParticleJsonParser::ParseTextureReferences(const TArray<TSharedPtr<FJsonValue>>& JsonValues, TArray<FUnityTextureReference>& OutTextureReferences)
{
	for (const TSharedPtr<FJsonValue>& JsonValue : JsonValues)
	{
		const TSharedPtr<FJsonObject> TextureReferenceObject = JsonValue.IsValid() ? JsonValue->AsObject() : nullptr;
		if (!TextureReferenceObject.IsValid())
		{
			continue;
		}

		FUnityTextureReference TextureReference;
		TextureReference.Source = UnityParticleJsonParser::GetStringField(TextureReferenceObject, TEXT("source"));

		const TSharedPtr<FJsonObject>* TextureObject = nullptr;
		if (TextureReferenceObject->TryGetObjectField(TEXT("texture"), TextureObject))
		{
			TextureReference.Texture.Name = UnityParticleJsonParser::GetStringField(*TextureObject, TEXT("name"));
			TextureReference.Texture.Type = UnityParticleJsonParser::GetStringField(*TextureObject, TEXT("type"));
			TextureReference.Texture.AssetPath = UnityParticleJsonParser::GetStringField(*TextureObject, TEXT("assetPath"));
			TextureReference.Texture.Guid = UnityParticleJsonParser::GetStringField(*TextureObject, TEXT("guid"));
		}

		OutTextureReferences.Add(MoveTemp(TextureReference));
	}
}

void FUnityParticleJsonParser::ParseMain(const TSharedPtr<FJsonObject>& JsonObject, FUnityMainModule& OutMain)
{
	OutMain.Duration = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("duration"));
	JsonObject->TryGetBoolField(TEXT("loop"), OutMain.bLoop);
	OutMain.SimulationSpace = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("simulationSpace"));
	JsonObject->TryGetBoolField(TEXT("startSize3D"), OutMain.bStartSize3D);
	JsonObject->TryGetBoolField(TEXT("startRotation3D"), OutMain.bStartRotation3D);
	OutMain.StartRotationUnit = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("startRotationUnit"));
	OutMain.FlipRotation = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("flipRotation"));
	JsonObject->TryGetNumberField(TEXT("simulationSpeed"), OutMain.SimulationSpeed);
	OutMain.MaxParticles = UnityParticleJsonParser::GetIntField(JsonObject, TEXT("maxParticles"));

	const TSharedPtr<FJsonObject>* StartDelayObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startDelay"), StartDelayObject))
	{
		OutMain.StartDelay = ParseMinMaxCurve(*StartDelayObject);
	}

	const TSharedPtr<FJsonObject>* StartLifetimeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startLifetime"), StartLifetimeObject))
	{
		OutMain.StartLifetime = ParseMinMaxCurve(*StartLifetimeObject);
	}

	const TSharedPtr<FJsonObject>* StartSpeedObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startSpeed"), StartSpeedObject))
	{
		OutMain.StartSpeed = ParseMinMaxCurve(*StartSpeedObject);
	}

	const TSharedPtr<FJsonObject>* StartSizeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startSize"), StartSizeObject))
	{
		OutMain.StartSize = ParseMinMaxCurve(*StartSizeObject);
	}

	const TSharedPtr<FJsonObject>* StartSizeXObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startSizeX"), StartSizeXObject))
	{
		OutMain.StartSizeX = ParseMinMaxCurve(*StartSizeXObject);
	}

	const TSharedPtr<FJsonObject>* StartSizeYObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startSizeY"), StartSizeYObject))
	{
		OutMain.StartSizeY = ParseMinMaxCurve(*StartSizeYObject);
	}

	const TSharedPtr<FJsonObject>* StartSizeZObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startSizeZ"), StartSizeZObject))
	{
		OutMain.StartSizeZ = ParseMinMaxCurve(*StartSizeZObject);
	}

	const TSharedPtr<FJsonObject>* StartRotationObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startRotation"), StartRotationObject))
	{
		OutMain.StartRotation = ParseMinMaxCurve(*StartRotationObject);
	}

	const TSharedPtr<FJsonObject>* StartRotationXObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startRotationX"), StartRotationXObject))
	{
		OutMain.StartRotationX = ParseMinMaxCurve(*StartRotationXObject);
	}

	const TSharedPtr<FJsonObject>* StartRotationYObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startRotationY"), StartRotationYObject))
	{
		OutMain.StartRotationY = ParseMinMaxCurve(*StartRotationYObject);
	}

	const TSharedPtr<FJsonObject>* StartRotationZObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startRotationZ"), StartRotationZObject))
	{
		OutMain.StartRotationZ = ParseMinMaxCurve(*StartRotationZObject);
	}

	const TSharedPtr<FJsonObject>* StartColorObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startColor"), StartColorObject))
	{
		OutMain.StartColor = ParseColor(*StartColorObject);
	}

	const TSharedPtr<FJsonObject>* GravityModifierObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("gravityModifier"), GravityModifierObject))
	{
		OutMain.GravityModifier = ParseMinMaxCurve(*GravityModifierObject);
	}
}

FUnityVector2Data FUnityParticleJsonParser::ParseVector2(const TSharedPtr<FJsonObject>& JsonObject)
{
	FUnityVector2Data Vector;
	if (JsonObject.IsValid())
	{
		JsonObject->TryGetNumberField(TEXT("x"), Vector.X);
		JsonObject->TryGetNumberField(TEXT("y"), Vector.Y);
	}
	return Vector;
}

FUnityVector3Data FUnityParticleJsonParser::ParseVector3(const TSharedPtr<FJsonObject>& JsonObject)
{
	FUnityVector3Data Vector;
	if (JsonObject.IsValid())
	{
		JsonObject->TryGetNumberField(TEXT("x"), Vector.X);
		JsonObject->TryGetNumberField(TEXT("y"), Vector.Y);
		JsonObject->TryGetNumberField(TEXT("z"), Vector.Z);
	}
	return Vector;
}

FUnityMinMaxCurveData FUnityParticleJsonParser::ParseMinMaxCurve(const TSharedPtr<FJsonObject>& JsonObject)
{
	FUnityMinMaxCurveData Curve;
	if (!JsonObject.IsValid())
	{
		return Curve;
	}

	Curve.Mode = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("mode"));
	JsonObject->TryGetNumberField(TEXT("curveMultiplier"), Curve.CurveMultiplier);
	if (JsonObject->TryGetNumberField(TEXT("constant"), Curve.Constant))
	{
		Curve.bHasValue = true;
	}
	if (JsonObject->TryGetNumberField(TEXT("constantMin"), Curve.ConstantMin))
	{
		Curve.bHasValue = true;
	}
	if (JsonObject->TryGetNumberField(TEXT("constantMax"), Curve.ConstantMax))
	{
		Curve.bHasValue = true;
	}

	const TSharedPtr<FJsonObject>* CurveObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("curve"), CurveObject))
	{
		const TArray<TSharedPtr<FJsonValue>>* KeyValues = nullptr;
		if ((*CurveObject)->TryGetArrayField(TEXT("keys"), KeyValues))
		{
			for (const TSharedPtr<FJsonValue>& KeyValue : *KeyValues)
			{
				const TSharedPtr<FJsonObject> KeyObject = KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;
				if (!KeyObject.IsValid())
				{
					continue;
				}

				FUnityCurveKeyData Key;
				KeyObject->TryGetNumberField(TEXT("time"), Key.Time);
				KeyObject->TryGetNumberField(TEXT("value"), Key.Value);
				Curve.Keys.Add(Key);
			}
		}
	}

	return Curve;
}

FUnityColorData FUnityParticleJsonParser::ParseColor(const TSharedPtr<FJsonObject>& JsonObject)
{
	FUnityColorData Color;
	if (!JsonObject.IsValid())
	{
		return Color;
	}

	const TSharedPtr<FJsonObject>* ColorObject = nullptr;
	if (!JsonObject->TryGetObjectField(TEXT("color"), ColorObject))
	{
		return Color;
	}

	(*ColorObject)->TryGetNumberField(TEXT("r"), Color.R);
	(*ColorObject)->TryGetNumberField(TEXT("g"), Color.G);
	(*ColorObject)->TryGetNumberField(TEXT("b"), Color.B);
	(*ColorObject)->TryGetNumberField(TEXT("a"), Color.A);

	return Color;
}

void FUnityParticleJsonParser::ParseColorOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnityColorOverLifetimeModule& OutColorOverLifetime)
{
	if (!JsonObject.IsValid())
	{
		return;
	}

	JsonObject->TryGetBoolField(TEXT("enabled"), OutColorOverLifetime.bEnabled);
	if (!OutColorOverLifetime.bEnabled)
	{
		return;
	}

	const TSharedPtr<FJsonObject>* ColorObject = nullptr;
	const TSharedPtr<FJsonObject>* GradientObject = nullptr;
	if (!JsonObject->TryGetObjectField(TEXT("color"), ColorObject) || !(*ColorObject)->TryGetObjectField(TEXT("gradient"), GradientObject))
	{
		return;
	}

	FLinearColor AccumulatedColor = FLinearColor::Black;
	int32 ColorKeyCount = 0;
	FUnityColorData FirstColorKey;
	FUnityColorData LastColorKey;
	const TArray<TSharedPtr<FJsonValue>>* ColorKeyValues = nullptr;
	if ((*GradientObject)->TryGetArrayField(TEXT("colorKeys"), ColorKeyValues))
	{
		for (const TSharedPtr<FJsonValue>& ColorKeyValue : *ColorKeyValues)
		{
			const TSharedPtr<FJsonObject> ColorKeyObject = ColorKeyValue.IsValid() ? ColorKeyValue->AsObject() : nullptr;
			if (!ColorKeyObject.IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* KeyColorObject = nullptr;
			if (ColorKeyObject->TryGetObjectField(TEXT("color"), KeyColorObject))
			{
				FUnityColorData KeyColor;
				float KeyTime = 0.0f;
				ColorKeyObject->TryGetNumberField(TEXT("time"), KeyTime);
				(*KeyColorObject)->TryGetNumberField(TEXT("r"), KeyColor.R);
				(*KeyColorObject)->TryGetNumberField(TEXT("g"), KeyColor.G);
				(*KeyColorObject)->TryGetNumberField(TEXT("b"), KeyColor.B);
				(*KeyColorObject)->TryGetNumberField(TEXT("a"), KeyColor.A);
				AccumulatedColor += KeyColor.ToLinearColor();
				if (ColorKeyCount == 0)
				{
					FirstColorKey = KeyColor;
				}
				LastColorKey = KeyColor;
				++ColorKeyCount;

				FUnityColorKeyData StoredColorKey;
				StoredColorKey.Time = KeyTime;
				StoredColorKey.Color = KeyColor;
				OutColorOverLifetime.ColorKeys.Add(StoredColorKey);
			}
		}
	}

	float MaxAlpha = 0.0f;
	float FirstAlpha = 1.0f;
	float LastAlpha = 1.0f;
	bool bHasAlpha = false;
	const TArray<TSharedPtr<FJsonValue>>* AlphaKeyValues = nullptr;
	if ((*GradientObject)->TryGetArrayField(TEXT("alphaKeys"), AlphaKeyValues))
	{
		for (const TSharedPtr<FJsonValue>& AlphaKeyValue : *AlphaKeyValues)
		{
			const TSharedPtr<FJsonObject> AlphaKeyObject = AlphaKeyValue.IsValid() ? AlphaKeyValue->AsObject() : nullptr;
			if (!AlphaKeyObject.IsValid())
			{
				continue;
			}

			float Alpha = 0.0f;
			if (AlphaKeyObject->TryGetNumberField(TEXT("alpha"), Alpha))
			{
				float KeyTime = 0.0f;
				AlphaKeyObject->TryGetNumberField(TEXT("time"), KeyTime);
				MaxAlpha = bHasAlpha ? FMath::Max(MaxAlpha, Alpha) : Alpha;
				if (!bHasAlpha)
				{
					FirstAlpha = Alpha;
				}
				LastAlpha = Alpha;
				bHasAlpha = true;

				FUnityAlphaKeyData StoredAlphaKey;
				StoredAlphaKey.Time = KeyTime;
				StoredAlphaKey.Alpha = Alpha;
				OutColorOverLifetime.AlphaKeys.Add(StoredAlphaKey);
			}
		}
	}

	if (ColorKeyCount > 0)
	{
		const FLinearColor RepresentativeColor = AccumulatedColor * (1.0f / static_cast<float>(ColorKeyCount));
		OutColorOverLifetime.RepresentativeColor.R = RepresentativeColor.R;
		OutColorOverLifetime.RepresentativeColor.G = RepresentativeColor.G;
		OutColorOverLifetime.RepresentativeColor.B = RepresentativeColor.B;
		OutColorOverLifetime.RepresentativeColor.A = bHasAlpha ? MaxAlpha : RepresentativeColor.A;
		OutColorOverLifetime.bHasRepresentativeColor = true;

		OutColorOverLifetime.StartColor = FirstColorKey;
		OutColorOverLifetime.EndColor = LastColorKey;
		OutColorOverLifetime.StartColor.A = bHasAlpha ? FirstAlpha : FirstColorKey.A;
		OutColorOverLifetime.EndColor.A = bHasAlpha ? LastAlpha : LastColorKey.A;
		OutColorOverLifetime.bHasStartEndColor = true;
	}
}

void FUnityParticleJsonParser::ParseEmission(const TSharedPtr<FJsonObject>& JsonObject, FUnityEmissionModule& OutEmission)
{
	if (!JsonObject.IsValid())
	{
		return;
	}

	JsonObject->TryGetBoolField(TEXT("enabled"), OutEmission.bEnabled);

	const TSharedPtr<FJsonObject>* RateOverTimeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("rateOverTime"), RateOverTimeObject))
	{
		OutEmission.RateOverTime = ParseMinMaxCurve(*RateOverTimeObject);
	}

	const TSharedPtr<FJsonObject>* RateOverDistanceObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("rateOverDistance"), RateOverDistanceObject))
	{
		OutEmission.RateOverDistance = ParseMinMaxCurve(*RateOverDistanceObject);
	}

	const TArray<TSharedPtr<FJsonValue>>* BurstValues = nullptr;
	if (!JsonObject->TryGetArrayField(TEXT("bursts"), BurstValues))
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& BurstValue : *BurstValues)
	{
		const TSharedPtr<FJsonObject> BurstObject = BurstValue.IsValid() ? BurstValue->AsObject() : nullptr;
		if (!BurstObject.IsValid())
		{
			continue;
		}

		FUnityBurstData Burst;
		BurstObject->TryGetNumberField(TEXT("time"), Burst.Time);
		Burst.CycleCount = FMath::Max(UnityParticleJsonParser::GetIntField(BurstObject, TEXT("cycleCount")), 1);
		Burst.RepeatInterval = UnityParticleJsonParser::GetFloatField(BurstObject, TEXT("repeatInterval"));
		Burst.Probability = UnityParticleJsonParser::GetFloatField(BurstObject, TEXT("probability"));
		if (Burst.Probability <= 0.0f)
		{
			Burst.Probability = 1.0f;
		}

		float CountValue = 0.0f;
		if (BurstObject->TryGetNumberField(TEXT("count"), CountValue))
		{
			Burst.Count = FMath::RoundToInt(CountValue);
		}
		else
		{
			const TSharedPtr<FJsonObject>* CountObject = nullptr;
			if (BurstObject->TryGetObjectField(TEXT("count"), CountObject))
			{
				Burst.Count = FMath::RoundToInt(ParseMinMaxCurve(*CountObject).GetRepresentativeValue());
			}
		}
		OutEmission.Bursts.Add(Burst);
	}
}

void FUnityParticleJsonParser::ParseShape(const TSharedPtr<FJsonObject>& JsonObject, FUnityShapeModule& OutShape)
{
	if (!JsonObject.IsValid())
	{
		return;
	}

	JsonObject->TryGetBoolField(TEXT("enabled"), OutShape.bEnabled);
	OutShape.ShapeType = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("shapeType"));
	OutShape.Angle = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("angle"));
	OutShape.Radius = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("radius"));
	OutShape.RadiusThickness = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("radiusThickness"));
	OutShape.Arc = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("arc"));
	OutShape.ArcMode = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("arcMode"));
	OutShape.ArcSpread = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("arcSpread"));
	OutShape.Length = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("length"));
	JsonObject->TryGetBoolField(TEXT("alignToDirection"), OutShape.bAlignToDirection);
	OutShape.RandomDirectionAmount = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("randomDirectionAmount"));
	OutShape.SphericalDirectionAmount = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("sphericalDirectionAmount"));
	OutShape.RandomPositionAmount = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("randomPositionAmount"));

	const TSharedPtr<FJsonObject>* ArcSpeedObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("arcSpeed"), ArcSpeedObject))
	{
		OutShape.ArcSpeed = ParseMinMaxCurve(*ArcSpeedObject);
	}

	const TSharedPtr<FJsonObject>* BoxThicknessObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("boxThickness"), BoxThicknessObject))
	{
		OutShape.BoxThickness = ParseVector3(*BoxThicknessObject);
	}

	const TSharedPtr<FJsonObject>* ScaleObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("scale"), ScaleObject))
	{
		OutShape.Scale = ParseVector3(*ScaleObject);
	}

	const TSharedPtr<FJsonObject>* PositionObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("position"), PositionObject))
	{
		OutShape.Position = ParseVector3(*PositionObject);
	}

	const TSharedPtr<FJsonObject>* RotationObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("rotation"), RotationObject))
	{
		OutShape.Rotation = ParseVector3(*RotationObject);
	}
}

void FUnityParticleJsonParser::ParseSizeOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnitySizeOverLifetimeModule& OutSizeOverLifetime)
{
	if (!JsonObject.IsValid())
	{
		return;
	}

	JsonObject->TryGetBoolField(TEXT("enabled"), OutSizeOverLifetime.bEnabled);
	JsonObject->TryGetBoolField(TEXT("separateAxes"), OutSizeOverLifetime.bSeparateAxes);

	const TSharedPtr<FJsonObject>* SizeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("size"), SizeObject))
	{
		OutSizeOverLifetime.Size = ParseMinMaxCurve(*SizeObject);
	}

	const TSharedPtr<FJsonObject>* XObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("x"), XObject))
	{
		OutSizeOverLifetime.X = ParseMinMaxCurve(*XObject);
	}

	const TSharedPtr<FJsonObject>* YObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("y"), YObject))
	{
		OutSizeOverLifetime.Y = ParseMinMaxCurve(*YObject);
	}

	const TSharedPtr<FJsonObject>* ZObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("z"), ZObject))
	{
		OutSizeOverLifetime.Z = ParseMinMaxCurve(*ZObject);
	}
}

void FUnityParticleJsonParser::ParseRotationOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnityRotationOverLifetimeModule& OutRotationOverLifetime)
{
	if (!JsonObject.IsValid())
	{
		return;
	}

	JsonObject->TryGetBoolField(TEXT("enabled"), OutRotationOverLifetime.bEnabled);
	JsonObject->TryGetBoolField(TEXT("separateAxes"), OutRotationOverLifetime.bSeparateAxes);

	const TSharedPtr<FJsonObject>* AngularVelocityObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("angularVelocity"), AngularVelocityObject))
	{
		OutRotationOverLifetime.AngularVelocity = ParseMinMaxCurve(*AngularVelocityObject);
	}

	const TSharedPtr<FJsonObject>* XObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("x"), XObject))
	{
		OutRotationOverLifetime.X = ParseMinMaxCurve(*XObject);
	}

	const TSharedPtr<FJsonObject>* YObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("y"), YObject))
	{
		OutRotationOverLifetime.Y = ParseMinMaxCurve(*YObject);
	}

	const TSharedPtr<FJsonObject>* ZObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("z"), ZObject))
	{
		OutRotationOverLifetime.Z = ParseMinMaxCurve(*ZObject);
	}
}

void FUnityParticleJsonParser::ParseVelocityOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnityVelocityOverLifetimeModule& OutVelocityOverLifetime)
{
	if (!JsonObject.IsValid())
	{
		return;
	}

	JsonObject->TryGetBoolField(TEXT("enabled"), OutVelocityOverLifetime.bEnabled);
	OutVelocityOverLifetime.bInWorldSpace = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("space")).Equals(TEXT("World"), ESearchCase::IgnoreCase);

	const TSharedPtr<FJsonObject>* XObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("x"), XObject))
	{
		OutVelocityOverLifetime.X = ParseMinMaxCurve(*XObject);
	}

	const TSharedPtr<FJsonObject>* YObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("y"), YObject))
	{
		OutVelocityOverLifetime.Y = ParseMinMaxCurve(*YObject);
	}

	const TSharedPtr<FJsonObject>* ZObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("z"), ZObject))
	{
		OutVelocityOverLifetime.Z = ParseMinMaxCurve(*ZObject);
	}
}

void FUnityParticleJsonParser::ParseForceOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnityForceOverLifetimeModule& OutForceOverLifetime)
{
	if (!JsonObject.IsValid())
	{
		return;
	}

	JsonObject->TryGetBoolField(TEXT("enabled"), OutForceOverLifetime.bEnabled);
	JsonObject->TryGetBoolField(TEXT("randomized"), OutForceOverLifetime.bRandomized);

	const TSharedPtr<FJsonObject>* XObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("x"), XObject))
	{
		OutForceOverLifetime.X = ParseMinMaxCurve(*XObject);
	}

	const TSharedPtr<FJsonObject>* YObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("y"), YObject))
	{
		OutForceOverLifetime.Y = ParseMinMaxCurve(*YObject);
	}

	const TSharedPtr<FJsonObject>* ZObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("z"), ZObject))
	{
		OutForceOverLifetime.Z = ParseMinMaxCurve(*ZObject);
	}
}

void FUnityParticleJsonParser::ParseLimitVelocityOverLifetime(const TSharedPtr<FJsonObject>& JsonObject, FUnityLimitVelocityOverLifetimeModule& OutLimitVelocityOverLifetime)
{
	if (!JsonObject.IsValid())
	{
		return;
	}

	JsonObject->TryGetBoolField(TEXT("enabled"), OutLimitVelocityOverLifetime.bEnabled);
	JsonObject->TryGetBoolField(TEXT("separateAxes"), OutLimitVelocityOverLifetime.bSeparateAxes);
	OutLimitVelocityOverLifetime.Dampen = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("dampen"));

	const TSharedPtr<FJsonObject>* LimitObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("limit"), LimitObject))
	{
		OutLimitVelocityOverLifetime.Limit = ParseMinMaxCurve(*LimitObject);
	}

	const TSharedPtr<FJsonObject>* LimitXObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("limitX"), LimitXObject))
	{
		OutLimitVelocityOverLifetime.LimitX = ParseMinMaxCurve(*LimitXObject);
	}

	const TSharedPtr<FJsonObject>* LimitYObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("limitY"), LimitYObject))
	{
		OutLimitVelocityOverLifetime.LimitY = ParseMinMaxCurve(*LimitYObject);
	}

	const TSharedPtr<FJsonObject>* LimitZObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("limitZ"), LimitZObject))
	{
		OutLimitVelocityOverLifetime.LimitZ = ParseMinMaxCurve(*LimitZObject);
	}
}

void FUnityParticleJsonParser::ParseNoise(const TSharedPtr<FJsonObject>& JsonObject, FUnityNoiseModule& OutNoise)
{
	if (!JsonObject.IsValid())
	{
		return;
	}

	JsonObject->TryGetBoolField(TEXT("enabled"), OutNoise.bEnabled);
	JsonObject->TryGetBoolField(TEXT("separateAxes"), OutNoise.bSeparateAxes);
	OutNoise.Frequency = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("frequency"));
	OutNoise.ScrollSpeed = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("scrollSpeed"));
	OutNoise.Damping = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("damping"));
	OutNoise.OctaveCount = FMath::Max(UnityParticleJsonParser::GetIntField(JsonObject, TEXT("octaveCount")), 1);
	OutNoise.OctaveMultiplier = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("octaveMultiplier"));
	OutNoise.OctaveScale = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("octaveScale"));
	if (OutNoise.OctaveMultiplier <= 0.0f)
	{
		OutNoise.OctaveMultiplier = 0.5f;
	}
	if (OutNoise.OctaveScale <= 0.0f)
	{
		OutNoise.OctaveScale = 2.0f;
	}

	const TSharedPtr<FJsonObject>* StrengthObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("strength"), StrengthObject))
	{
		OutNoise.Strength = ParseMinMaxCurve(*StrengthObject);
	}

	const TSharedPtr<FJsonObject>* StrengthXObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("strengthX"), StrengthXObject))
	{
		OutNoise.StrengthX = ParseMinMaxCurve(*StrengthXObject);
	}

	const TSharedPtr<FJsonObject>* StrengthYObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("strengthY"), StrengthYObject))
	{
		OutNoise.StrengthY = ParseMinMaxCurve(*StrengthYObject);
	}

	const TSharedPtr<FJsonObject>* StrengthZObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("strengthZ"), StrengthZObject))
	{
		OutNoise.StrengthZ = ParseMinMaxCurve(*StrengthZObject);
	}
}

void FUnityParticleJsonParser::ParseTrails(const TSharedPtr<FJsonObject>& JsonObject, FUnityTrailsModule& OutTrails)
{
	if (!JsonObject.IsValid())
	{
		return;
	}

	JsonObject->TryGetBoolField(TEXT("enabled"), OutTrails.bEnabled);
	OutTrails.Ratio = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("ratio"));
	OutTrails.Lifetime = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("lifetime"));
	OutTrails.MinVertexDistance = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("minVertexDistance"));
	JsonObject->TryGetBoolField(TEXT("worldSpace"), OutTrails.bWorldSpace);
	JsonObject->TryGetBoolField(TEXT("dieWithParticles"), OutTrails.bDieWithParticles);
	JsonObject->TryGetBoolField(TEXT("sizeAffectsWidth"), OutTrails.bSizeAffectsWidth);
	JsonObject->TryGetBoolField(TEXT("inheritParticleColor"), OutTrails.bInheritParticleColor);
}

void FUnityParticleJsonParser::ParseTextureSheetAnimation(const TSharedPtr<FJsonObject>& JsonObject, FUnityTextureSheetAnimationModule& OutTextureSheetAnimation)
{
	if (!JsonObject.IsValid())
	{
		return;
	}

	JsonObject->TryGetBoolField(TEXT("enabled"), OutTextureSheetAnimation.bEnabled);
	OutTextureSheetAnimation.Mode = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("mode"));
	OutTextureSheetAnimation.NumTilesX = UnityParticleJsonParser::GetIntField(JsonObject, TEXT("numTilesX"));
	OutTextureSheetAnimation.NumTilesY = UnityParticleJsonParser::GetIntField(JsonObject, TEXT("numTilesY"));
	OutTextureSheetAnimation.Animation = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("animation"));
	JsonObject->TryGetBoolField(TEXT("useRandomRow"), OutTextureSheetAnimation.bUseRandomRow);
	OutTextureSheetAnimation.RowIndex = UnityParticleJsonParser::GetIntField(JsonObject, TEXT("rowIndex"));
	OutTextureSheetAnimation.CycleCount = UnityParticleJsonParser::GetIntField(JsonObject, TEXT("cycleCount"));
	OutTextureSheetAnimation.UvChannelMask = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("uvChannelMask"));
	OutTextureSheetAnimation.RowMode = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("rowMode"));

	const TSharedPtr<FJsonObject>* FrameOverTimeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("frameOverTime"), FrameOverTimeObject))
	{
		OutTextureSheetAnimation.FrameOverTime = ParseMinMaxCurve(*FrameOverTimeObject);
	}

	const TSharedPtr<FJsonObject>* StartFrameObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("startFrame"), StartFrameObject))
	{
		OutTextureSheetAnimation.StartFrame = ParseMinMaxCurve(*StartFrameObject);
	}

	const TSharedPtr<FJsonObject>* SpeedRangeObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("speedRange"), SpeedRangeObject))
	{
		OutTextureSheetAnimation.SpeedRange = ParseVector2(*SpeedRangeObject);
	}

	const TArray<TSharedPtr<FJsonValue>>* SpriteValues = nullptr;
	if (JsonObject->TryGetArrayField(TEXT("sprites"), SpriteValues))
	{
		for (const TSharedPtr<FJsonValue>& SpriteValue : *SpriteValues)
		{
			const TSharedPtr<FJsonObject> SpriteObject = SpriteValue.IsValid() ? SpriteValue->AsObject() : nullptr;
			if (!SpriteObject.IsValid())
			{
				continue;
			}

			FUnitySpriteData Sprite;
			Sprite.Name = UnityParticleJsonParser::GetStringField(SpriteObject, TEXT("name"));
			Sprite.Type = UnityParticleJsonParser::GetStringField(SpriteObject, TEXT("type"));
			Sprite.AssetPath = UnityParticleJsonParser::GetStringField(SpriteObject, TEXT("assetPath"));
			Sprite.Guid = UnityParticleJsonParser::GetStringField(SpriteObject, TEXT("guid"));
			OutTextureSheetAnimation.Sprites.Add(MoveTemp(Sprite));
		}
	}
}

void FUnityParticleJsonParser::ParseRenderer(const TSharedPtr<FJsonObject>& JsonObject, FUnityRendererData& OutRenderer)
{
	OutRenderer.RenderMode = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("renderMode"));
	OutRenderer.Alignment = UnityParticleJsonParser::GetStringField(JsonObject, TEXT("alignment"));
	OutRenderer.LengthScale = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("lengthScale"));
	OutRenderer.VelocityScale = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("velocityScale"));
	OutRenderer.CameraVelocityScale = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("cameraVelocityScale"));
	OutRenderer.SortingFudge = UnityParticleJsonParser::GetFloatField(JsonObject, TEXT("sortingFudge"));
	JsonObject->TryGetBoolField(TEXT("allowRoll"), OutRenderer.bAllowRoll);
	OutRenderer.SortingOrder = UnityParticleJsonParser::GetIntField(JsonObject, TEXT("sortingOrder"));

	const TSharedPtr<FJsonObject>* FlipObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("flip"), FlipObject))
	{
		OutRenderer.Flip = ParseVector3(*FlipObject);
	}

	const TSharedPtr<FJsonObject>* PivotObject = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("pivot"), PivotObject))
	{
		OutRenderer.Pivot = ParseVector3(*PivotObject);
	}
}
