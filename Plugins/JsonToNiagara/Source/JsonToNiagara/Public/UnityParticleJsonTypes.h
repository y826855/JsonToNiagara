#pragma once

#include "CoreMinimal.h"

struct FUnityTextureData
{
	FString Name;
	FString Type;
	FString AssetPath;
	FString Guid;
};

struct FUnityTextureReference
{
	FString Source;
	FUnityTextureData Texture;
};

struct FUnityNiagaraHint
{
	FString EmitterType;
	FString Spawn;
	FString BlendMode;
};

struct FUnityColorData
{
	float R = 1.0f;
	float G = 1.0f;
	float B = 1.0f;
	float A = 1.0f;

	FLinearColor ToLinearColor() const
	{
		return FLinearColor(R, G, B, A);
	}
};

struct FUnityVector2Data
{
	float X = 0.0f;
	float Y = 0.0f;
};

struct FUnityVector3Data
{
	float X = 0.0f;
	float Y = 0.0f;
	float Z = 0.0f;
};

struct FUnityCurveKeyData
{
	float Time = 0.0f;
	float Value = 0.0f;
};

struct FUnityColorKeyData
{
	float Time = 0.0f;
	FUnityColorData Color;
};

struct FUnityAlphaKeyData
{
	float Time = 0.0f;
	float Alpha = 1.0f;
};

struct FUnityMinMaxCurveData
{
	FString Mode;
	float CurveMultiplier = 1.0f;
	float Constant = 0.0f;
	float ConstantMin = 0.0f;
	float ConstantMax = 0.0f;
	bool bHasValue = false;
	TArray<FUnityCurveKeyData> Keys;

	float GetRepresentativeValue(const float DefaultValue = 0.0f) const
	{
		if (!bHasValue)
		{
			return DefaultValue;
		}

		if (Mode.Equals(TEXT("TwoConstants"), ESearchCase::IgnoreCase))
		{
			return (ConstantMin + ConstantMax) * 0.5f;
		}

		return Constant;
	}

	float GetFirstKeyValue(const float DefaultValue = 0.0f) const
	{
		return Keys.IsEmpty() ? DefaultValue : Keys[0].Value * CurveMultiplier;
	}

	float GetLastKeyValue(const float DefaultValue = 0.0f) const
	{
		return Keys.IsEmpty() ? DefaultValue : Keys.Last().Value * CurveMultiplier;
	}

	bool IsTwoConstants() const
	{
		return Mode.Equals(TEXT("TwoConstants"), ESearchCase::IgnoreCase);
	}

	float GetMinValue(const float DefaultValue = 0.0f) const
	{
		return IsTwoConstants() ? ConstantMin : GetRepresentativeValue(DefaultValue);
	}

	float GetMaxValue(const float DefaultValue = 0.0f) const
	{
		return IsTwoConstants() ? ConstantMax : GetRepresentativeValue(DefaultValue);
	}

	float GetVisualValue(const float DefaultValue = 0.0f) const
	{
		return IsTwoConstants() ? ConstantMax : GetRepresentativeValue(DefaultValue);
	}
};

struct FUnityMainModule
{
	float Duration = 0.0f;
	bool bLoop = false;
	FUnityMinMaxCurveData StartDelay;
	FUnityMinMaxCurveData StartLifetime;
	FUnityMinMaxCurveData StartSpeed;
	bool bStartSize3D = false;
	FUnityMinMaxCurveData StartSize;
	FUnityMinMaxCurveData StartSizeX;
	FUnityMinMaxCurveData StartSizeY;
	FUnityMinMaxCurveData StartSizeZ;
	bool bStartRotation3D = false;
	FString StartRotationUnit;
	FUnityMinMaxCurveData StartRotation;
	FUnityMinMaxCurveData StartRotationX;
	FUnityMinMaxCurveData StartRotationY;
	FUnityMinMaxCurveData StartRotationZ;
	float FlipRotation = 0.0f;
	FUnityColorData StartColor;
	FUnityMinMaxCurveData GravityModifier;
	FString SimulationSpace;
	float SimulationSpeed = 1.0f;
	int32 MaxParticles = 0;
};

struct FUnityColorOverLifetimeModule
{
	bool bEnabled = false;
	bool bHasRepresentativeColor = false;
	bool bHasStartEndColor = false;
	FUnityColorData RepresentativeColor;
	FUnityColorData StartColor;
	FUnityColorData EndColor;
	TArray<FUnityColorKeyData> ColorKeys;
	TArray<FUnityAlphaKeyData> AlphaKeys;
};

struct FUnityBurstData
{
	float Time = 0.0f;
	int32 Count = 0;
	int32 CycleCount = 1;
	float RepeatInterval = 0.0f;
	float Probability = 1.0f;
};

struct FUnityEmissionModule
{
	bool bEnabled = false;
	FUnityMinMaxCurveData RateOverTime;
	FUnityMinMaxCurveData RateOverDistance;
	TArray<FUnityBurstData> Bursts;
};

struct FUnityShapeModule
{
	bool bEnabled = false;
	FString ShapeType;
	float Angle = 0.0f;
	float Radius = 0.0f;
	float RadiusThickness = 0.0f;
	float Arc = 0.0f;
	FString ArcMode;
	float ArcSpread = 0.0f;
	FUnityMinMaxCurveData ArcSpeed;
	float Length = 0.0f;
	FUnityVector3Data BoxThickness;
	FUnityVector3Data Scale;
	FUnityVector3Data Position;
	FUnityVector3Data Rotation;
	bool bAlignToDirection = false;
	float RandomDirectionAmount = 0.0f;
	float SphericalDirectionAmount = 0.0f;
	float RandomPositionAmount = 0.0f;
};

struct FUnitySizeOverLifetimeModule
{
	bool bEnabled = false;
	bool bSeparateAxes = false;
	FUnityMinMaxCurveData Size;
	FUnityMinMaxCurveData X;
	FUnityMinMaxCurveData Y;
	FUnityMinMaxCurveData Z;
};

struct FUnityRotationOverLifetimeModule
{
	bool bEnabled = false;
	bool bSeparateAxes = false;
	FUnityMinMaxCurveData AngularVelocity;
	FUnityMinMaxCurveData X;
	FUnityMinMaxCurveData Y;
	FUnityMinMaxCurveData Z;
};

struct FUnityVelocityOverLifetimeModule
{
	bool bEnabled = false;
	bool bInWorldSpace = false;
	FUnityMinMaxCurveData X;
	FUnityMinMaxCurveData Y;
	FUnityMinMaxCurveData Z;
};

struct FUnityForceOverLifetimeModule
{
	bool bEnabled = false;
	bool bRandomized = false;
	FUnityMinMaxCurveData X;
	FUnityMinMaxCurveData Y;
	FUnityMinMaxCurveData Z;
};

struct FUnityLimitVelocityOverLifetimeModule
{
	bool bEnabled = false;
	bool bSeparateAxes = false;
	float Dampen = 0.0f;
	FUnityMinMaxCurveData Limit;
	FUnityMinMaxCurveData LimitX;
	FUnityMinMaxCurveData LimitY;
	FUnityMinMaxCurveData LimitZ;
};

struct FUnityNoiseModule
{
	bool bEnabled = false;
	bool bSeparateAxes = false;
	float Frequency = 0.0f;
	float ScrollSpeed = 0.0f;
	float Damping = 0.0f;
	int32 OctaveCount = 1;
	float OctaveMultiplier = 0.5f;
	float OctaveScale = 2.0f;
	FUnityMinMaxCurveData Strength;
	FUnityMinMaxCurveData StrengthX;
	FUnityMinMaxCurveData StrengthY;
	FUnityMinMaxCurveData StrengthZ;
};

struct FUnityTrailsModule
{
	bool bEnabled = false;
	float Ratio = 0.0f;
	float Lifetime = 0.0f;
	float MinVertexDistance = 0.0f;
	bool bWorldSpace = false;
	bool bDieWithParticles = true;
	bool bSizeAffectsWidth = true;
	bool bInheritParticleColor = true;
};

struct FUnitySpriteData
{
	FString Name;
	FString Type;
	FString AssetPath;
	FString Guid;
};

struct FUnityTextureSheetAnimationModule
{
	bool bEnabled = false;
	FString Mode;
	int32 NumTilesX = 1;
	int32 NumTilesY = 1;
	FString Animation;
	bool bUseRandomRow = false;
	int32 RowIndex = 0;
	FUnityMinMaxCurveData FrameOverTime;
	FUnityMinMaxCurveData StartFrame;
	int32 CycleCount = 1;
	FString UvChannelMask;
	FString RowMode;
	FUnityVector2Data SpeedRange;
	TArray<FUnitySpriteData> Sprites;
};

struct FUnityRendererData
{
	FString RenderMode;
	FString Alignment;
	float LengthScale = 1.0f;
	float VelocityScale = 0.0f;
	float CameraVelocityScale = 0.0f;
	float SortingFudge = 0.0f;
	FUnityVector3Data Flip;
	bool bAllowRoll = true;
	FUnityVector3Data Pivot;
	int32 SortingOrder = 0;
};

struct FUnityParticleSystemData
{
	FString Name;
	FString Path;
	bool bIsEmptyEmitter = false;
	TArray<FString> EnabledModules;
	FUnityNiagaraHint NiagaraHint;
	TArray<FUnityTextureReference> TextureReferences;
	FUnityMainModule Main;
	FUnityColorOverLifetimeModule ColorOverLifetime;
	FUnityEmissionModule Emission;
	FUnityShapeModule Shape;
	FUnitySizeOverLifetimeModule SizeOverLifetime;
	FUnityRotationOverLifetimeModule RotationOverLifetime;
	FUnityVelocityOverLifetimeModule VelocityOverLifetime;
	FUnityForceOverLifetimeModule ForceOverLifetime;
	FUnityLimitVelocityOverLifetimeModule LimitVelocityOverLifetime;
	FUnityNoiseModule Noise;
	FUnityTrailsModule Trails;
	FUnityTextureSheetAnimationModule TextureSheetAnimation;
	FUnityRendererData Renderer;
};

struct FUnityParticleExportRoot
{
	FString ExportVersion;
	FString RootName;
	FString RootAssetPath;
	int32 ParticleSystemCount = 0;
	TArray<FUnityParticleSystemData> ParticleSystems;
};
