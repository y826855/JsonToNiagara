#include "Unity2NiagaraImporter.h"

#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "Engine/Texture.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "IContentBrowserSingleton.h"
#include "Materials/MaterialInstanceBasePropertyOverrides.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "NiagaraConstants.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraTypes.h"
#include "Unity2NiagaraTextureResolver.h"
#include "UnityParticleJsonTypes.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

#include <initializer_list>

namespace Unity2NiagaraImporter
{
	const TCHAR* DefaultParticleMaterialPath = TEXT("/Game/Material/M_DefaultParticle.M_DefaultParticle");
	const FName ParticleTextureParameterName(TEXT("ParticleTexture"));
	const FName ParticleColorParameterName(TEXT("ParticleColor"));

	float GetVisualSizeScale(const FUnityParticleSystemData& ParticleSystem);
	float GetVisualLifetimeScale(const FUnityParticleSystemData& ParticleSystem);
	float GetVisualSpeedScale(const FUnityParticleSystemData& ParticleSystem);
	float GetVisualSpriteRotationOffsetDegrees(const FUnityParticleSystemData& ParticleSystem);
	float GetUnityVisualLifetime(const FUnityParticleSystemData& ParticleSystem, const float DefaultValue, const TCHAR* Salt);

	bool NameContainsAny(const FString& Name, std::initializer_list<const TCHAR*> Tokens)
	{
		for (const TCHAR* Token : Tokens)
		{
			if (Name.Contains(Token, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	float ConvertUnitySizeToNiagaraSpriteSize(float UnitySize)
	{
		return FMath::Max(UnitySize * 100.0f, 1.0f);
	}

	float ConvertUnitySpeedToNiagaraVelocity(float UnitySpeed)
	{
		return UnitySpeed * 100.0f;
	}

	int32 GetUnityTextureSheetTotalFrames(const FUnityTextureSheetAnimationModule& TextureSheetAnimation)
	{
		return FMath::Max(TextureSheetAnimation.NumTilesX, 1) * FMath::Max(TextureSheetAnimation.NumTilesY, 1);
	}

	float ClampUnitySubImageIndex(float SubImageIndex, const FUnityTextureSheetAnimationModule& TextureSheetAnimation)
	{
		const int32 TotalFrames = GetUnityTextureSheetTotalFrames(TextureSheetAnimation);
		const float MaxSubImageIndex = FMath::Max(static_cast<float>(TotalFrames - 1), 0.0f);
		return FMath::Clamp(SubImageIndex, 0.0f, MaxSubImageIndex);
	}

	FVector3f ConvertUnityVectorToNiagaraPosition(const FUnityVector3Data& UnityVector)
	{
		return FVector3f(UnityVector.X * 100.0f, UnityVector.Y * 100.0f, UnityVector.Z * 100.0f);
	}

	float ConvertUnityRotationToNiagaraDegrees(float UnityRotation, const FString& RotationUnit)
	{
		if (RotationUnit.Equals(TEXT("Radians"), ESearchCase::IgnoreCase) || RotationUnit.IsEmpty())
		{
			return FMath::RadiansToDegrees(UnityRotation);
		}

		return UnityRotation;
	}

	float GetStableRandom01(const FUnityParticleSystemData& ParticleSystem, const TCHAR* Salt)
	{
		const FString SeedText = FString::Printf(
			TEXT("%s|%s|%s"),
			ParticleSystem.Path.IsEmpty() ? *ParticleSystem.Name : *ParticleSystem.Path,
			ParticleSystem.Name.IsEmpty() ? TEXT("<empty>") : *ParticleSystem.Name,
			Salt);
		return static_cast<float>(GetTypeHash(SeedText) % 10000) / 9999.0f;
	}

	float GetUnityCurveVisualValue(const FUnityParticleSystemData& ParticleSystem, const FUnityMinMaxCurveData& Curve, const float DefaultValue, const TCHAR* Salt)
	{
		if (Curve.IsTwoConstants())
		{
			const float Alpha = GetStableRandom01(ParticleSystem, Salt);
			return FMath::Lerp(Curve.GetMinValue(DefaultValue), Curve.GetMaxValue(DefaultValue), Alpha);
		}

		return Curve.GetVisualValue(DefaultValue);
	}

	float EvaluateUnityCurveAtTime(const FUnityMinMaxCurveData& Curve, const float Time, const float DefaultValue = 0.0f)
	{
		if (Curve.Keys.IsEmpty())
		{
			return Curve.GetVisualValue(DefaultValue);
		}

		if (Time <= Curve.Keys[0].Time)
		{
			return Curve.Keys[0].Value * Curve.CurveMultiplier;
		}

		for (int32 KeyIndex = 1; KeyIndex < Curve.Keys.Num(); ++KeyIndex)
		{
			const FUnityCurveKeyData& PreviousKey = Curve.Keys[KeyIndex - 1];
			const FUnityCurveKeyData& NextKey = Curve.Keys[KeyIndex];
			if (Time <= NextKey.Time)
			{
				const float Range = FMath::Max(NextKey.Time - PreviousKey.Time, KINDA_SMALL_NUMBER);
				const float Alpha = FMath::Clamp((Time - PreviousKey.Time) / Range, 0.0f, 1.0f);
				return FMath::Lerp(PreviousKey.Value, NextKey.Value, Alpha) * Curve.CurveMultiplier;
			}
		}

		return Curve.Keys.Last().Value * Curve.CurveMultiplier;
	}

	float GetUnityCurvePeakValue(const FUnityMinMaxCurveData& Curve, const float DefaultValue = 1.0f)
	{
		if (Curve.Keys.IsEmpty())
		{
			return Curve.GetVisualValue(DefaultValue);
		}

		float PeakValue = Curve.Keys[0].Value * Curve.CurveMultiplier;
		for (const FUnityCurveKeyData& Key : Curve.Keys)
		{
			PeakValue = FMath::Max(PeakValue, Key.Value * Curve.CurveMultiplier);
		}
		return PeakValue;
	}

	float EvaluateUnityAlphaAtTime(const FUnityColorOverLifetimeModule& ColorOverLifetime, const float Time, const float DefaultAlpha = 1.0f)
	{
		if (ColorOverLifetime.AlphaKeys.IsEmpty())
		{
			return DefaultAlpha;
		}

		if (Time <= ColorOverLifetime.AlphaKeys[0].Time)
		{
			return ColorOverLifetime.AlphaKeys[0].Alpha;
		}

		for (int32 KeyIndex = 1; KeyIndex < ColorOverLifetime.AlphaKeys.Num(); ++KeyIndex)
		{
			const FUnityAlphaKeyData& PreviousKey = ColorOverLifetime.AlphaKeys[KeyIndex - 1];
			const FUnityAlphaKeyData& NextKey = ColorOverLifetime.AlphaKeys[KeyIndex];
			if (Time <= NextKey.Time)
			{
				const float Range = FMath::Max(NextKey.Time - PreviousKey.Time, KINDA_SMALL_NUMBER);
				const float Alpha = FMath::Clamp((Time - PreviousKey.Time) / Range, 0.0f, 1.0f);
				return FMath::Lerp(PreviousKey.Alpha, NextKey.Alpha, Alpha);
			}
		}

		return ColorOverLifetime.AlphaKeys.Last().Alpha;
	}

	FLinearColor EvaluateUnityColorAtTime(const FUnityColorOverLifetimeModule& ColorOverLifetime, const float Time)
	{
		if (ColorOverLifetime.ColorKeys.IsEmpty())
		{
			return ColorOverLifetime.bHasRepresentativeColor
				? ColorOverLifetime.RepresentativeColor.ToLinearColor()
				: FLinearColor::White;
		}

		if (Time <= ColorOverLifetime.ColorKeys[0].Time)
		{
			FLinearColor Color = ColorOverLifetime.ColorKeys[0].Color.ToLinearColor();
			Color.A = EvaluateUnityAlphaAtTime(ColorOverLifetime, Time, Color.A);
			return Color;
		}

		for (int32 KeyIndex = 1; KeyIndex < ColorOverLifetime.ColorKeys.Num(); ++KeyIndex)
		{
			const FUnityColorKeyData& PreviousKey = ColorOverLifetime.ColorKeys[KeyIndex - 1];
			const FUnityColorKeyData& NextKey = ColorOverLifetime.ColorKeys[KeyIndex];
			if (Time <= NextKey.Time)
			{
				const float Range = FMath::Max(NextKey.Time - PreviousKey.Time, KINDA_SMALL_NUMBER);
				const float Alpha = FMath::Clamp((Time - PreviousKey.Time) / Range, 0.0f, 1.0f);
				FLinearColor Color = FLinearColor::LerpUsingHSV(PreviousKey.Color.ToLinearColor(), NextKey.Color.ToLinearColor(), Alpha);
				Color.A = EvaluateUnityAlphaAtTime(ColorOverLifetime, Time, Color.A);
				return Color;
			}
		}

		FLinearColor Color = ColorOverLifetime.ColorKeys.Last().Color.ToLinearColor();
		Color.A = EvaluateUnityAlphaAtTime(ColorOverLifetime, Time, Color.A);
		return Color;
	}

	FLinearColor GetUnityPeakLifetimeColor(const FUnityParticleSystemData& ParticleSystem)
	{
		if (!ParticleSystem.ColorOverLifetime.bEnabled)
		{
			return FLinearColor::White;
		}

		FLinearColor BestColor = EvaluateUnityColorAtTime(ParticleSystem.ColorOverLifetime, 0.0f);
		float BestScore = BestColor.A * BestColor.GetLuminance();
		constexpr int32 SampleCount = 8;
		for (int32 SampleIndex = 1; SampleIndex <= SampleCount; ++SampleIndex)
		{
			const float Time = static_cast<float>(SampleIndex) / static_cast<float>(SampleCount);
			const FLinearColor SampleColor = EvaluateUnityColorAtTime(ParticleSystem.ColorOverLifetime, Time);
			const float SampleScore = SampleColor.A * SampleColor.GetLuminance();
			if (SampleScore > BestScore)
			{
				BestColor = SampleColor;
				BestScore = SampleScore;
			}
		}

		return BestColor;
	}

	FString FormatLinearColorCompact(const FLinearColor& Color)
	{
		return FString::Printf(TEXT("(%.3f, %.3f, %.3f, %.3f)"), Color.R, Color.G, Color.B, Color.A);
	}

	FString FormatVector2Compact(const FVector2f& Vector)
	{
		return FString::Printf(TEXT("(%.3f, %.3f)"), Vector.X, Vector.Y);
	}

	FString FormatUnityColorOverLifetimeSamples(const FUnityParticleSystemData& ParticleSystem)
	{
		if (!ParticleSystem.ColorOverLifetime.bEnabled)
		{
			return TEXT("<disabled>");
		}

		FString Samples;
		const float SampleTimes[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
		for (const float SampleTime : SampleTimes)
		{
			if (!Samples.IsEmpty())
			{
				Samples += TEXT(" | ");
			}

			const FLinearColor SampleColor = EvaluateUnityColorAtTime(ParticleSystem.ColorOverLifetime, SampleTime);
			Samples += FString::Printf(TEXT("%.2f%s"), SampleTime, *FormatLinearColorCompact(SampleColor));
		}

		return Samples;
	}

	FLinearColor GetUnityEndLifetimeColor(const FUnityParticleSystemData& ParticleSystem)
	{
		if (!ParticleSystem.ColorOverLifetime.bEnabled)
		{
			return FLinearColor::White;
		}

		return EvaluateUnityColorAtTime(ParticleSystem.ColorOverLifetime, 1.0f);
	}

	FVector2f EvaluateUnitySizeOverLifetimeAtTime(const FUnitySizeOverLifetimeModule& SizeOverLifetime, const float Time)
	{
		if (!SizeOverLifetime.bEnabled)
		{
			return FVector2f(1.0f, 1.0f);
		}

		if (SizeOverLifetime.bSeparateAxes)
		{
			return FVector2f(
				EvaluateUnityCurveAtTime(SizeOverLifetime.X, Time, 1.0f),
				EvaluateUnityCurveAtTime(SizeOverLifetime.Y, Time, 1.0f));
		}

		const float UniformScale = EvaluateUnityCurveAtTime(SizeOverLifetime.Size, Time, 1.0f);
		return FVector2f(UniformScale, UniformScale);
	}

	FString FormatUnitySizeOverLifetimeSamples(const FUnityParticleSystemData& ParticleSystem)
	{
		if (!ParticleSystem.SizeOverLifetime.bEnabled)
		{
			return TEXT("<disabled>");
		}

		FString Samples;
		const float SampleTimes[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
		for (const float SampleTime : SampleTimes)
		{
			if (!Samples.IsEmpty())
			{
				Samples += TEXT(" | ");
			}

			const FVector2f SampleSizeScale = EvaluateUnitySizeOverLifetimeAtTime(ParticleSystem.SizeOverLifetime, SampleTime);
			Samples += FString::Printf(TEXT("%.2f%s"), SampleTime, *FormatVector2Compact(SampleSizeScale));
		}

		return Samples;
	}

	FVector2f GetUnityPeakSizeOverLifetimeScale2D(const FUnityParticleSystemData& ParticleSystem)
	{
		if (!ParticleSystem.SizeOverLifetime.bEnabled)
		{
			return FVector2f(1.0f, 1.0f);
		}

		FVector2f PeakScale = EvaluateUnitySizeOverLifetimeAtTime(ParticleSystem.SizeOverLifetime, 0.0f);
		constexpr int32 SampleCount = 8;
		for (int32 SampleIndex = 1; SampleIndex <= SampleCount; ++SampleIndex)
		{
			const float Time = static_cast<float>(SampleIndex) / static_cast<float>(SampleCount);
			const FVector2f SampleScale = EvaluateUnitySizeOverLifetimeAtTime(ParticleSystem.SizeOverLifetime, Time);
			PeakScale.X = FMath::Max(PeakScale.X, SampleScale.X);
			PeakScale.Y = FMath::Max(PeakScale.Y, SampleScale.Y);
		}

		PeakScale.X = FMath::Max(PeakScale.X, 0.01f);
		PeakScale.Y = FMath::Max(PeakScale.Y, 0.01f);
		return PeakScale;
	}

	float EvaluateUnityTextureSheetFrameAtTime(const FUnityTextureSheetAnimationModule& TextureSheetAnimation, const float Time)
	{
		const float StartFrame = TextureSheetAnimation.StartFrame.GetVisualValue(0.0f);
		const float Frame = EvaluateUnityCurveAtTime(TextureSheetAnimation.FrameOverTime, Time, StartFrame);
		return ClampUnitySubImageIndex(Frame, TextureSheetAnimation);
	}

	FString FormatUnityTextureSheetSamples(const FUnityParticleSystemData& ParticleSystem)
	{
		if (!ParticleSystem.TextureSheetAnimation.bEnabled)
		{
			return TEXT("<disabled>");
		}

		FString Samples;
		const float SampleTimes[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
		for (const float SampleTime : SampleTimes)
		{
			if (!Samples.IsEmpty())
			{
				Samples += TEXT(" | ");
			}

			const float Frame = EvaluateUnityTextureSheetFrameAtTime(ParticleSystem.TextureSheetAnimation, SampleTime);
			Samples += FString::Printf(TEXT("%.2f=%.3f"), SampleTime, Frame);
		}

		return Samples;
	}

	float GetUnityInitialSizeScale(const FUnityParticleSystemData& ParticleSystem)
	{
		if (!ParticleSystem.SizeOverLifetime.bEnabled)
		{
			return 1.0f;
		}

		const FUnityMinMaxCurveData& SizeCurve = ParticleSystem.SizeOverLifetime.Size;
		return FMath::Max(GetUnityCurvePeakValue(SizeCurve, SizeCurve.GetVisualValue(1.0f)), 0.01f);
	}

	float GetUnityInitialSpriteSize(const FUnityParticleSystemData& ParticleSystem, const float DefaultSize = 1.0f)
	{
		const float StartSize = GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Main.StartSize, DefaultSize, TEXT("StartSize"));
		return StartSize * GetUnityInitialSizeScale(ParticleSystem);
	}

	FVector2f GetUnityInitialSpriteSize2D(const FUnityParticleSystemData& ParticleSystem, const float DefaultSize = 1.0f)
	{
		const float SizeScale = GetUnityInitialSizeScale(ParticleSystem);
		if (ParticleSystem.Main.bStartSize3D)
		{
			return FVector2f(
				GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Main.StartSizeX, DefaultSize, TEXT("StartSizeX")) * SizeScale,
				GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Main.StartSizeY, DefaultSize, TEXT("StartSizeY")) * SizeScale);
		}

		const float StartSize = GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Main.StartSize, DefaultSize, TEXT("StartSize2D")) * SizeScale;
		return FVector2f(StartSize, StartSize);
	}

	float GetUnityInitialSpriteRotationDegrees(const FUnityParticleSystemData& ParticleSystem)
	{
		const FUnityMinMaxCurveData& RotationCurve = ParticleSystem.Main.bStartRotation3D
			? ParticleSystem.Main.StartRotationZ
			: ParticleSystem.Main.StartRotation;
		return ConvertUnityRotationToNiagaraDegrees(
			GetUnityCurveVisualValue(ParticleSystem, RotationCurve, 0.0f, TEXT("StartRotation")),
			ParticleSystem.Main.StartRotationUnit)
			+ GetVisualSpriteRotationOffsetDegrees(ParticleSystem);
	}

	float GetUnityRotationOverLifetimeDegreesPerSecond(const FUnityParticleSystemData& ParticleSystem)
	{
		if (!ParticleSystem.RotationOverLifetime.bEnabled)
		{
			return 0.0f;
		}

		const FUnityMinMaxCurveData& RotationCurve = ParticleSystem.RotationOverLifetime.bSeparateAxes
			? ParticleSystem.RotationOverLifetime.Z
			: ParticleSystem.RotationOverLifetime.AngularVelocity;
		return ConvertUnityRotationToNiagaraDegrees(
			GetUnityCurveVisualValue(ParticleSystem, RotationCurve, 0.0f, TEXT("RotationOverLifetime")),
			ParticleSystem.Main.StartRotationUnit);
	}

	FVector3f GetDeterministicShapeDirection(const FUnityParticleSystemData& ParticleSystem)
	{
		const uint32 Hash = GetTypeHash(ParticleSystem.Path.IsEmpty() ? ParticleSystem.Name : ParticleSystem.Path);
		const float Angle01 = static_cast<float>(Hash % 10000) / 10000.0f;
		const float YawRadians = Angle01 * 2.0f * PI;
		const float PitchRadians = ParticleSystem.Shape.bEnabled
			? FMath::DegreesToRadians(FMath::Clamp(ParticleSystem.Shape.Angle, 0.0f, 89.0f))
			: 0.0f;
		const float HorizontalScale = FMath::Cos(PitchRadians);
		FVector3f Direction(
			FMath::Cos(YawRadians) * HorizontalScale,
			FMath::Sin(YawRadians) * HorizontalScale,
			FMath::Sin(PitchRadians));
		Direction.Normalize();
		return Direction;
	}

	float GetApproximateShapeRadius(const FUnityParticleSystemData& ParticleSystem)
	{
		if (!ParticleSystem.Shape.bEnabled)
		{
			return 0.0f;
		}

		if (ParticleSystem.Shape.ShapeType.Equals(TEXT("Box"), ESearchCase::IgnoreCase))
		{
			return FMath::Max3(
				FMath::Abs(ParticleSystem.Shape.Scale.X),
				FMath::Abs(ParticleSystem.Shape.Scale.Y),
				FMath::Abs(ParticleSystem.Shape.Scale.Z)) * 0.5f;
		}

		if (ParticleSystem.Shape.ShapeType.Equals(TEXT("Hemisphere"), ESearchCase::IgnoreCase)
			|| ParticleSystem.Shape.ShapeType.Equals(TEXT("Sphere"), ESearchCase::IgnoreCase)
			|| ParticleSystem.Shape.ShapeType.Equals(TEXT("Circle"), ESearchCase::IgnoreCase)
			|| ParticleSystem.Shape.ShapeType.Equals(TEXT("Cone"), ESearchCase::IgnoreCase)
			|| ParticleSystem.Shape.ShapeType.Equals(TEXT("Donut"), ESearchCase::IgnoreCase))
		{
			return ParticleSystem.Shape.Radius;
		}

		return FMath::Max(ParticleSystem.Shape.Radius, ParticleSystem.Shape.Length);
	}

	FVector3f GetUnityNoiseVelocityOffset(const FUnityParticleSystemData& ParticleSystem)
	{
		if (!ParticleSystem.Noise.bEnabled)
		{
			return FVector3f::ZeroVector;
		}

		const float Lifetime = GetUnityVisualLifetime(ParticleSystem, 1.0f, TEXT("NoiseLifetime"));
		const float DampingScale = FMath::Clamp(1.0f - ParticleSystem.Noise.Damping, 0.0f, 1.0f);
		const float OctaveScale = FMath::Max(ParticleSystem.Noise.OctaveMultiplier, 0.0f) * static_cast<float>(FMath::Max(ParticleSystem.Noise.OctaveCount, 1));
		const float FrequencyScale = FMath::Max(ParticleSystem.Noise.Frequency, 0.1f);
		if (ParticleSystem.Noise.bSeparateAxes)
		{
			return FVector3f(
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Noise.StrengthX, 0.0f, TEXT("NoiseStrengthX"))),
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Noise.StrengthY, 0.0f, TEXT("NoiseStrengthY"))),
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Noise.StrengthZ, 0.0f, TEXT("NoiseStrengthZ")))) * DampingScale * OctaveScale * FrequencyScale * Lifetime;
		}

		const float Strength = ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Noise.Strength, 0.0f, TEXT("NoiseStrength")));
		return GetDeterministicShapeDirection(ParticleSystem) * Strength * DampingScale * OctaveScale * FrequencyScale * Lifetime;
	}

	FVector3f ApplyUnityLimitVelocity(const FUnityParticleSystemData& ParticleSystem, const FVector3f& Velocity)
	{
		if (!ParticleSystem.LimitVelocityOverLifetime.bEnabled)
		{
			return Velocity;
		}

		const float Dampen = FMath::Clamp(ParticleSystem.LimitVelocityOverLifetime.Dampen, 0.0f, 1.0f);
		if (ParticleSystem.LimitVelocityOverLifetime.bSeparateAxes)
		{
			const FVector3f Limit(
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.LimitVelocityOverLifetime.LimitX, 0.0f, TEXT("LimitVelocityX"))),
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.LimitVelocityOverLifetime.LimitY, 0.0f, TEXT("LimitVelocityY"))),
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.LimitVelocityOverLifetime.LimitZ, 0.0f, TEXT("LimitVelocityZ"))));
			return FVector3f(
				Limit.X > KINDA_SMALL_NUMBER ? FMath::Clamp(Velocity.X, -Limit.X, Limit.X) * (1.0f - Dampen) : Velocity.X,
				Limit.Y > KINDA_SMALL_NUMBER ? FMath::Clamp(Velocity.Y, -Limit.Y, Limit.Y) * (1.0f - Dampen) : Velocity.Y,
				Limit.Z > KINDA_SMALL_NUMBER ? FMath::Clamp(Velocity.Z, -Limit.Z, Limit.Z) * (1.0f - Dampen) : Velocity.Z);
		}

		const float Limit = ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.LimitVelocityOverLifetime.Limit, 0.0f, TEXT("LimitVelocity")));
		if (Limit <= KINDA_SMALL_NUMBER || Velocity.SizeSquared() <= FMath::Square(Limit))
		{
			return Velocity;
		}

		return Velocity.GetSafeNormal() * Limit * (1.0f - Dampen);
	}

	FVector3f GetUnityInitialVelocity(const FUnityParticleSystemData& ParticleSystem)
	{
		const float StartSpeed = ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Main.StartSpeed, 0.0f, TEXT("StartSpeed")))
			* GetVisualSpeedScale(ParticleSystem);
		FVector3f Direction = FVector3f(1.0f, 0.0f, 0.0f);
		if (ParticleSystem.Shape.bEnabled && (ParticleSystem.Shape.ShapeType.Equals(TEXT("Sphere"), ESearchCase::IgnoreCase)
			|| ParticleSystem.Shape.ShapeType.Equals(TEXT("Cone"), ESearchCase::IgnoreCase)
			|| ParticleSystem.Shape.ShapeType.Equals(TEXT("Circle"), ESearchCase::IgnoreCase)
			|| ParticleSystem.Shape.ShapeType.Equals(TEXT("Hemisphere"), ESearchCase::IgnoreCase)
			|| ParticleSystem.Shape.RandomDirectionAmount > 0.0f
			|| ParticleSystem.Shape.SphericalDirectionAmount > 0.0f))
		{
			Direction = GetDeterministicShapeDirection(ParticleSystem);
		}

		FVector3f Velocity = Direction * StartSpeed;
		if (ParticleSystem.VelocityOverLifetime.bEnabled)
		{
			Velocity += FVector3f(
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.VelocityOverLifetime.X, 0.0f, TEXT("VelocityOverLifetimeX"))),
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.VelocityOverLifetime.Y, 0.0f, TEXT("VelocityOverLifetimeY"))),
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.VelocityOverLifetime.Z, 0.0f, TEXT("VelocityOverLifetimeZ"))));
		}
		if (ParticleSystem.ForceOverLifetime.bEnabled)
		{
			const float Lifetime = GetUnityVisualLifetime(ParticleSystem, 1.0f, TEXT("ForceLifetime"));
			Velocity += FVector3f(
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.ForceOverLifetime.X, 0.0f, TEXT("ForceOverLifetimeX"))) * Lifetime,
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.ForceOverLifetime.Y, 0.0f, TEXT("ForceOverLifetimeY"))) * Lifetime,
				ConvertUnitySpeedToNiagaraVelocity(GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.ForceOverLifetime.Z, 0.0f, TEXT("ForceOverLifetimeZ"))) * Lifetime);
		}

		Velocity += GetUnityNoiseVelocityOffset(ParticleSystem);
		return ApplyUnityLimitVelocity(ParticleSystem, Velocity);
	}

	FVector3f GetUnityVelocityOverLifetimeValue(const FUnityParticleSystemData& ParticleSystem, const float NormalizedAge)
	{
		if (!ParticleSystem.VelocityOverLifetime.bEnabled)
		{
			return FVector3f::ZeroVector;
		}

		return FVector3f(
			ConvertUnitySpeedToNiagaraVelocity(EvaluateUnityCurveAtTime(ParticleSystem.VelocityOverLifetime.X, NormalizedAge, 0.0f)),
			ConvertUnitySpeedToNiagaraVelocity(EvaluateUnityCurveAtTime(ParticleSystem.VelocityOverLifetime.Y, NormalizedAge, 0.0f)),
			ConvertUnitySpeedToNiagaraVelocity(EvaluateUnityCurveAtTime(ParticleSystem.VelocityOverLifetime.Z, NormalizedAge, 0.0f)));
	}

	FVector3f GetUnityForceOverLifetimeValue(const FUnityParticleSystemData& ParticleSystem, const float NormalizedAge)
	{
		if (!ParticleSystem.ForceOverLifetime.bEnabled)
		{
			return FVector3f::ZeroVector;
		}

		return FVector3f(
			ConvertUnitySpeedToNiagaraVelocity(EvaluateUnityCurveAtTime(ParticleSystem.ForceOverLifetime.X, NormalizedAge, 0.0f)),
			ConvertUnitySpeedToNiagaraVelocity(EvaluateUnityCurveAtTime(ParticleSystem.ForceOverLifetime.Y, NormalizedAge, 0.0f)),
			ConvertUnitySpeedToNiagaraVelocity(EvaluateUnityCurveAtTime(ParticleSystem.ForceOverLifetime.Z, NormalizedAge, 0.0f)));
	}

	float GetUnityGravityModifierValue(const FUnityParticleSystemData& ParticleSystem)
	{
		return GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Main.GravityModifier, 0.0f, TEXT("GravityModifier"));
	}

	FVector3f GetUnityLifetimeMotionVelocity(const FUnityParticleSystemData& ParticleSystem)
	{
		const float Lifetime = GetUnityVisualLifetime(ParticleSystem, 1.0f, TEXT("MotionLifetime"));
		FVector3f Velocity = GetUnityInitialVelocity(ParticleSystem);
		Velocity += GetUnityVelocityOverLifetimeValue(ParticleSystem, 1.0f);
		Velocity += GetUnityForceOverLifetimeValue(ParticleSystem, 1.0f) * Lifetime;

		const float GravityModifier = GetUnityGravityModifierValue(ParticleSystem);
		if (FMath::Abs(GravityModifier) > KINDA_SMALL_NUMBER)
		{
			Velocity.Z -= 980.0f * GravityModifier * Lifetime;
		}

		Velocity += GetUnityNoiseVelocityOffset(ParticleSystem);
		return ApplyUnityLimitVelocity(ParticleSystem, Velocity);
	}

	bool HasUnityLifetimeMotion(const FUnityParticleSystemData& ParticleSystem)
	{
		return ParticleSystem.VelocityOverLifetime.bEnabled
			|| ParticleSystem.ForceOverLifetime.bEnabled
			|| ParticleSystem.Noise.bEnabled
			|| ParticleSystem.LimitVelocityOverLifetime.bEnabled
			|| FMath::Abs(GetUnityGravityModifierValue(ParticleSystem)) > KINDA_SMALL_NUMBER;
	}

	FLinearColor GetUnitySpawnColor(const FUnityParticleSystemData& ParticleSystem)
	{
		FLinearColor SpawnColor = ParticleSystem.Main.StartColor.ToLinearColor();
		if (ParticleSystem.ColorOverLifetime.bEnabled)
		{
			FLinearColor LifetimeColor = ParticleSystem.ColorOverLifetime.bHasStartEndColor
				? ParticleSystem.ColorOverLifetime.StartColor.ToLinearColor()
				: FLinearColor::White;
			const FLinearColor PeakLifetimeColor = GetUnityPeakLifetimeColor(ParticleSystem);
			if (LifetimeColor.A <= KINDA_SMALL_NUMBER && PeakLifetimeColor.A > LifetimeColor.A)
			{
				LifetimeColor = PeakLifetimeColor;
			}

			SpawnColor = FLinearColor(
				SpawnColor.R * LifetimeColor.R,
				SpawnColor.G * LifetimeColor.G,
				SpawnColor.B * LifetimeColor.B,
				SpawnColor.A * LifetimeColor.A);
		}

		const FString EmitterName = ParticleSystem.Name;
		float ColorScale = 0.92f;
		float AlphaScale = 0.85f;
		if (ParticleSystem.NiagaraHint.BlendMode.Equals(TEXT("Additive"), ESearchCase::IgnoreCase))
		{
			ColorScale = 0.75f;
			AlphaScale = 0.62f;
		}
		if (NameContainsAny(EmitterName, { TEXT("glow") }))
		{
			ColorScale = 0.55f;
			AlphaScale = 0.38f;
		}
		else if (NameContainsAny(EmitterName, { TEXT("circle"), TEXT("ring") }))
		{
			ColorScale = 0.62f;
			AlphaScale = 0.45f;
		}
		else if (NameContainsAny(EmitterName, { TEXT("shadow"), TEXT("cloud"), TEXT("mist"), TEXT("smoke") }))
		{
			ColorScale = 0.88f;
			AlphaScale = 0.70f;
		}
		else if (NameContainsAny(EmitterName, { TEXT("boke"), TEXT("bokeh") }))
		{
			ColorScale = 1.05f;
			AlphaScale = 1.60f;
		}
		else if (NameContainsAny(EmitterName, { TEXT("spark") }))
		{
			ColorScale = 1.10f;
			AlphaScale = 1.20f;
		}

		SpawnColor.R = FMath::Clamp(SpawnColor.R * ColorScale, 0.0f, 1.0f);
		SpawnColor.G = FMath::Clamp(SpawnColor.G * ColorScale, 0.0f, 1.0f);
		SpawnColor.B = FMath::Clamp(SpawnColor.B * ColorScale, 0.0f, 1.0f);
		SpawnColor.A = FMath::Clamp(SpawnColor.A * AlphaScale, 0.0f, 1.0f);
		return SpawnColor;
	}

	float GetVisualSizeScale(const FUnityParticleSystemData& ParticleSystem)
	{
		const FString EmitterName = ParticleSystem.Name;
		if (NameContainsAny(EmitterName, { TEXT("shadow"), TEXT("cloud"), TEXT("mist"), TEXT("smoke") }))
		{
			return 1.30f;
		}
		if (NameContainsAny(EmitterName, { TEXT("glow") }))
		{
			return 1.08f;
		}
		if (NameContainsAny(EmitterName, { TEXT("circle"), TEXT("ring") }))
		{
			return 0.92f;
		}
		if (NameContainsAny(EmitterName, { TEXT("boke"), TEXT("bokeh") }))
		{
			return 0.95f;
		}
		if (NameContainsAny(EmitterName, { TEXT("spark") }))
		{
			return 0.80f;
		}
		return 1.0f;
	}

	float GetVisualLifetimeScale(const FUnityParticleSystemData& ParticleSystem)
	{
		const FString EmitterName = ParticleSystem.Name;
		if (NameContainsAny(EmitterName, { TEXT("shadow"), TEXT("cloud"), TEXT("mist"), TEXT("smoke") }))
		{
			return 1.45f;
		}
		if (NameContainsAny(EmitterName, { TEXT("glow"), TEXT("circle"), TEXT("ring") }))
		{
			return 1.20f;
		}
		if (NameContainsAny(EmitterName, { TEXT("boke"), TEXT("bokeh") }))
		{
			return 1.35f;
		}
		if (NameContainsAny(EmitterName, { TEXT("spark") }))
		{
			return 1.05f;
		}
		return 1.0f;
	}

	float GetVisualSpeedScale(const FUnityParticleSystemData& ParticleSystem)
	{
		const FString EmitterName = ParticleSystem.Name;
		if (NameContainsAny(EmitterName, { TEXT("spark") }))
		{
			return 1.20f;
		}
		if (NameContainsAny(EmitterName, { TEXT("boke"), TEXT("bokeh") }))
		{
			return 0.85f;
		}
		if (NameContainsAny(EmitterName, { TEXT("shadow"), TEXT("cloud"), TEXT("mist"), TEXT("smoke") }))
		{
			return 0.55f;
		}
		return 1.0f;
	}

	float GetVisualSpriteRotationOffsetDegrees(const FUnityParticleSystemData& ParticleSystem)
	{
		if (ParticleSystem.Renderer.RenderMode.Equals(TEXT("Stretch"), ESearchCase::IgnoreCase)
			|| NameContainsAny(ParticleSystem.Name, { TEXT("spark"), TEXT("spike") }))
		{
			return 90.0f;
		}

		return 0.0f;
	}

	float GetUnityVisualLifetime(const FUnityParticleSystemData& ParticleSystem, const float DefaultValue, const TCHAR* Salt)
	{
		return FMath::Max(
			GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Main.StartLifetime, DefaultValue, Salt)
			* GetVisualLifetimeScale(ParticleSystem),
			0.01f);
	}

	FLinearColor GetUnityColorOverLifetimeScale(const FUnityParticleSystemData& ParticleSystem)
	{
		if (!ParticleSystem.ColorOverLifetime.bEnabled)
		{
			return FLinearColor::White;
		}

		const FLinearColor StartColor = ParticleSystem.ColorOverLifetime.bHasStartEndColor
			? ParticleSystem.ColorOverLifetime.StartColor.ToLinearColor()
			: FLinearColor::White;
		const FLinearColor EndColor = ParticleSystem.ColorOverLifetime.bHasStartEndColor
			? ParticleSystem.ColorOverLifetime.EndColor.ToLinearColor()
			: FLinearColor::White;
		const FLinearColor PeakColor = GetUnityPeakLifetimeColor(ParticleSystem);
		const FLinearColor TargetColor = PeakColor.A > EndColor.A ? PeakColor : EndColor;
		return FLinearColor(
			StartColor.R > KINDA_SMALL_NUMBER ? TargetColor.R / StartColor.R : TargetColor.R,
			StartColor.G > KINDA_SMALL_NUMBER ? TargetColor.G / StartColor.G : TargetColor.G,
			StartColor.B > KINDA_SMALL_NUMBER ? TargetColor.B / StartColor.B : TargetColor.B,
			StartColor.A > KINDA_SMALL_NUMBER ? TargetColor.A / StartColor.A : TargetColor.A);
	}

	int32 GetUnityVisualBurstCount(const FUnityParticleSystemData& ParticleSystem, const FUnityBurstData& Burst)
	{
		int32 BurstCount = FMath::Max(Burst.Count, 0);
		if (NameContainsAny(ParticleSystem.Name, { TEXT("boke"), TEXT("bokeh") }))
		{
			BurstCount = FMath::Max(FMath::RoundToInt(static_cast<float>(BurstCount) * 1.6f), 8);
		}
		else if (NameContainsAny(ParticleSystem.Name, { TEXT("spark"), TEXT("spike") }))
		{
			BurstCount = FMath::Max(FMath::RoundToInt(static_cast<float>(BurstCount) * 1.8f), 12);
		}
		return BurstCount;
	}

	int32 GetUnityBurstCountEstimate(const FUnityParticleSystemData& ParticleSystem)
	{
		int32 TotalBurstCount = 0;
		for (const FUnityBurstData& Burst : ParticleSystem.Emission.Bursts)
		{
			TotalBurstCount += GetUnityVisualBurstCount(ParticleSystem, Burst);
		}

		return TotalBurstCount;
	}

	bool ShouldUseShapeVelocityModule(const FUnityParticleSystemData& ParticleSystem)
	{
		const float StartSpeed = GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Main.StartSpeed, 0.0f, TEXT("ShapeVelocityStartSpeed"));
		const int32 BurstCount = GetUnityBurstCountEstimate(ParticleSystem);
		return ParticleSystem.Shape.bEnabled
			&& BurstCount > 1
			&& StartSpeed > KINDA_SMALL_NUMBER
			&& (ParticleSystem.Shape.ShapeType.Equals(TEXT("Sphere"), ESearchCase::IgnoreCase)
				|| ParticleSystem.Shape.ShapeType.Equals(TEXT("Cone"), ESearchCase::IgnoreCase)
				|| ParticleSystem.Shape.ShapeType.Equals(TEXT("Circle"), ESearchCase::IgnoreCase)
				|| ParticleSystem.Shape.ShapeType.Equals(TEXT("Hemisphere"), ESearchCase::IgnoreCase)
				|| ParticleSystem.Shape.ArcMode.Equals(TEXT("Random"), ESearchCase::IgnoreCase)
				|| ParticleSystem.Shape.Arc >= 359.0f);
	}

	bool HasRandomStartRotation(const FUnityParticleSystemData& ParticleSystem)
	{
		const FUnityMinMaxCurveData& RotationCurve = ParticleSystem.Main.bStartRotation3D
			? ParticleSystem.Main.StartRotationZ
			: ParticleSystem.Main.StartRotation;
		return RotationCurve.IsTwoConstants()
			&& GetUnityBurstCountEstimate(ParticleSystem) > 1
			&& !ParticleSystem.Renderer.RenderMode.Equals(TEXT("Stretch"), ESearchCase::IgnoreCase);
	}

	float GetUnitySpawnRate(const FUnityParticleSystemData& ParticleSystem)
	{
		const float RateOverTime = ParticleSystem.Emission.RateOverTime.GetVisualValue(0.0f);
		if (RateOverTime > KINDA_SMALL_NUMBER)
		{
			return RateOverTime;
		}

		return 0.0f;
	}

	EBlendMode ConvertUnityBlendModeToMaterialBlendMode(const FString& UnityBlendMode)
	{
		if (UnityBlendMode.Equals(TEXT("Additive"), ESearchCase::IgnoreCase))
		{
			return BLEND_Additive;
		}

		if (UnityBlendMode.Equals(TEXT("Premultiply"), ESearchCase::IgnoreCase)
			|| UnityBlendMode.Equals(TEXT("Premultiplied"), ESearchCase::IgnoreCase)
			|| UnityBlendMode.Equals(TEXT("AlphaComposite"), ESearchCase::IgnoreCase))
		{
			return BLEND_AlphaComposite;
		}

		return BLEND_Translucent;
	}

	const TCHAR* GetMaterialBlendModeName(EBlendMode BlendMode)
	{
		switch (BlendMode)
		{
		case BLEND_Additive:
			return TEXT("Additive");
		case BLEND_AlphaComposite:
			return TEXT("AlphaComposite");
		case BLEND_Translucent:
			return TEXT("Translucent");
		default:
			return TEXT("Other");
		}
	}

	const TCHAR* GetCompileStatusName(ENiagaraScriptCompileStatus Status)
	{
		switch (Status)
		{
		case ENiagaraScriptCompileStatus::NCS_Unknown:
			return TEXT("Unknown");
		case ENiagaraScriptCompileStatus::NCS_Dirty:
			return TEXT("Dirty");
		case ENiagaraScriptCompileStatus::NCS_Error:
			return TEXT("Error");
		case ENiagaraScriptCompileStatus::NCS_UpToDate:
			return TEXT("UpToDate");
		case ENiagaraScriptCompileStatus::NCS_BeingCreated:
			return TEXT("BeingCreated");
		case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:
			return TEXT("UpToDateWithWarnings");
		case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings:
			return TEXT("ComputeUpToDateWithWarnings");
		default:
			return TEXT("Other");
		}
	}

	void LogNiagaraScriptDiagnostics(const FString& EmitterName, const TCHAR* ScriptLabel, const UNiagaraScript* Script)
	{
		if (Script == nullptr)
		{
			UE_LOG(
				LogTemp,
				Warning,
				TEXT("[JsonToNiagara] Script Diagnostics: Emitter=%s Script=%s Missing=true"),
				*EmitterName,
				ScriptLabel);
			return;
		}

		const bool bReadyToRun = Script->IsReadyToRun(ENiagaraSimTarget::CPUSim);
		const bool bHasByteCode = Script->GetVMExecutableData().HasByteCode();
		const ENiagaraScriptCompileStatus CompileStatus = Script->GetLastCompileStatus();
		TArray<FNiagaraVariable> RapidIterationParameterVariables;
		Script->RapidIterationParameters.GetParameters(RapidIterationParameterVariables);
		UE_LOG(
			LogTemp,
			Display,
			TEXT("[JsonToNiagara] Script Diagnostics: Emitter=%s Script=%s Status=%s ReadyToRun=%s HasByteCode=%s RapidParams=%d"),
			*EmitterName,
			ScriptLabel,
			GetCompileStatusName(CompileStatus),
			bReadyToRun ? TEXT("true") : TEXT("false"),
			bHasByteCode ? TEXT("true") : TEXT("false"),
			RapidIterationParameterVariables.Num());
	}

	template<typename ValueType>
	void SetRapidIterationParameter(
		const FString& UniqueEmitterName,
		UNiagaraScript& TargetScript,
		UNiagaraNodeFunctionCall& TargetFunctionCallNode,
		FName InputName,
		FNiagaraTypeDefinition InputType,
		ValueType Value)
	{
		FNiagaraParameterHandle InputHandle = FNiagaraParameterHandle::CreateModuleParameterHandle(InputName);
		FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, &TargetFunctionCallNode);
		FNiagaraVariable InputVariable(InputType, AliasedInputHandle.GetParameterHandleString());
		const TCHAR* RapidIterationEmitterName = TargetScript.GetUsage() == ENiagaraScriptUsage::SystemSpawnScript
			|| TargetScript.GetUsage() == ENiagaraScriptUsage::SystemUpdateScript
			? nullptr
			: *UniqueEmitterName;
		FNiagaraVariable RapidIterationParameter = FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(
			InputVariable,
			RapidIterationEmitterName,
			TargetScript.GetUsage());
		RapidIterationParameter.SetValue(Value);

		constexpr bool bAddParameterIfMissing = true;
		TargetScript.RapidIterationParameters.SetParameterData(RapidIterationParameter.GetData(), RapidIterationParameter, bAddParameterIfMissing);
	}

	UNiagaraNodeFunctionCall* AddScriptModuleFromPath(const TCHAR* ModulePath, UNiagaraNodeOutput& TargetOutputNode)
	{
		UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, ModulePath);
		if (ModuleScript == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing Niagara module script: %s"), ModulePath);
			return nullptr;
		}

		return FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModuleScript, TargetOutputNode);
	}

	UNiagaraNodeFunctionCall* FindEmitterStateNode(UNiagaraGraph& Graph)
	{
		TArray<UNiagaraNodeFunctionCall*> FunctionCalls;
		Graph.GetNodesOfClass<UNiagaraNodeFunctionCall>(FunctionCalls);
		for (UNiagaraNodeFunctionCall* FunctionCall : FunctionCalls)
		{
			if (FunctionCall == nullptr)
			{
				continue;
			}

			const FString FunctionName = FunctionCall->GetFunctionName();
			const FString ScriptName = FunctionCall->FunctionScript != nullptr ? FunctionCall->FunctionScript->GetName() : FString();
			const FString ScriptPath = FunctionCall->FunctionScript != nullptr ? FunctionCall->FunctionScript->GetPathName() : FString();
			if (NameContainsAny(FunctionName, { TEXT("Emitter State"), TEXT("EmitterState") })
				|| NameContainsAny(ScriptName, { TEXT("EmitterState") })
				|| NameContainsAny(ScriptPath, { TEXT("EmitterState") }))
			{
				return FunctionCall;
			}
		}

		return nullptr;
	}

	bool HasInputPin(const UNiagaraNodeFunctionCall& FunctionCall, FName InputName)
	{
		for (const UEdGraphPin* Pin : FunctionCall.Pins)
		{
			if (Pin != nullptr && Pin->Direction == EGPD_Input && Pin->PinName == InputName)
			{
				return true;
			}
		}

		return false;
	}

	UEdGraphPin* FindInputPin(const UNiagaraNodeFunctionCall& FunctionCall, FName InputName)
	{
		for (UEdGraphPin* Pin : FunctionCall.Pins)
		{
			if (Pin != nullptr && Pin->Direction == EGPD_Input && Pin->PinName == InputName)
			{
				return Pin;
			}
		}

		return nullptr;
	}

	bool SetRapidIterationParameterRaw(
		const FString& UniqueEmitterName,
		UNiagaraScript& TargetScript,
		UNiagaraNodeFunctionCall& TargetFunctionCallNode,
		FName InputName,
		FNiagaraTypeDefinition InputType,
		int32 IntValue)
	{
		FNiagaraParameterHandle InputHandle = FNiagaraParameterHandle::CreateModuleParameterHandle(InputName);
		FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, &TargetFunctionCallNode);
		FNiagaraVariable InputVariable(InputType, AliasedInputHandle.GetParameterHandleString());
		const TCHAR* RapidIterationEmitterName = TargetScript.GetUsage() == ENiagaraScriptUsage::SystemSpawnScript
			|| TargetScript.GetUsage() == ENiagaraScriptUsage::SystemUpdateScript
			? nullptr
			: *UniqueEmitterName;
		FNiagaraVariable RapidIterationParameter = FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(
			InputVariable,
			RapidIterationEmitterName,
			TargetScript.GetUsage());

		RapidIterationParameter.AllocateData();
		uint8* Data = RapidIterationParameter.GetData();
		const uint32 DataSize = RapidIterationParameter.GetSizeInBytes();
		FMemory::Memzero(Data, DataSize);
		if (DataSize == sizeof(uint8))
		{
			*reinterpret_cast<uint8*>(Data) = static_cast<uint8>(IntValue);
		}
		else if (DataSize == sizeof(uint16))
		{
			*reinterpret_cast<uint16*>(Data) = static_cast<uint16>(IntValue);
		}
		else if (DataSize == sizeof(int32))
		{
			*reinterpret_cast<int32*>(Data) = IntValue;
		}
		else
		{
			UE_LOG(
				LogTemp,
				Warning,
				TEXT("[JsonToNiagara] Unsupported rapid param raw size: Param=%s Type=%s Size=%u"),
				*InputName.ToString(),
				*InputType.GetName(),
				DataSize);
			return false;
		}

		constexpr bool bAddParameterIfMissing = true;
		TargetScript.RapidIterationParameters.SetParameterData(RapidIterationParameter.GetData(), RapidIterationParameter, bAddParameterIfMissing);
		return true;
	}

	bool TrySetRapidFloat(
		const FString& UniqueEmitterName,
		UNiagaraScript& TargetScript,
		UNiagaraNodeFunctionCall& FunctionCall,
		FName InputName,
		float Value)
	{
		if (!HasInputPin(FunctionCall, InputName))
		{
			return false;
		}

		SetRapidIterationParameter(UniqueEmitterName, TargetScript, FunctionCall, InputName, FNiagaraTypeDefinition::GetFloatDef(), Value);
		return true;
	}

	bool TrySetRapidIntFromPin(
		const FString& UniqueEmitterName,
		UNiagaraScript& TargetScript,
		UNiagaraNodeFunctionCall& FunctionCall,
		FName InputName,
		int32 Value)
	{
		UEdGraphPin* Pin = FindInputPin(FunctionCall, InputName);
		if (Pin == nullptr)
		{
			return false;
		}

		const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
		return SetRapidIterationParameterRaw(UniqueEmitterName, TargetScript, FunctionCall, InputName, PinType, Value);
	}

	bool TrySetRapidInt(
		const FString& UniqueEmitterName,
		UNiagaraScript& TargetScript,
		UNiagaraNodeFunctionCall& FunctionCall,
		FName InputName,
		int32 Value)
	{
		if (!HasInputPin(FunctionCall, InputName))
		{
			return false;
		}

		SetRapidIterationParameter(UniqueEmitterName, TargetScript, FunctionCall, InputName, FNiagaraTypeDefinition::GetIntDef(), Value);
		return true;
	}
}

FUnity2NiagaraImportResult FUnity2NiagaraImporter::CreateNiagaraSystemAsset(const FUnityParticleExportRoot& Root) const
{
	FUnity2NiagaraImportResult Result;

	const FString RootName = MakeSafeObjectName(Root.RootName.IsEmpty() ? TEXT("UnityParticleSystem") : Root.RootName);
	const FString BasePackagePath = FString::Printf(TEXT("/Game/Unity2Niagara/Imported/%s"), *RootName);
	const FString BaseAssetName = FString::Printf(TEXT("NS_%s"), *RootName);
	const FString DesiredPackageName = FString::Printf(TEXT("%s/%s"), *BasePackagePath, *BaseAssetName);

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

	FString UniquePackageName;
	FString UniqueAssetName;
	AssetToolsModule.Get().CreateUniqueAssetName(DesiredPackageName, TEXT(""), UniquePackageName, UniqueAssetName);

	const FString UniquePackagePath = FPackageName::GetLongPackagePath(UniquePackageName);
	UNiagaraSystemFactoryNew* Factory = NewObject<UNiagaraSystemFactoryNew>();
	Factory->bEditAfterNew = false;
	UObject* CreatedAsset = AssetToolsModule.Get().CreateAsset(
		UniqueAssetName,
		UniquePackagePath,
		UNiagaraSystem::StaticClass(),
		Factory,
		TEXT("JsonToNiagara"));

	UNiagaraSystem* CreatedSystem = Cast<UNiagaraSystem>(CreatedAsset);
	if (CreatedSystem == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("[JsonToNiagara] Failed to create Niagara System asset: %s"), *UniquePackageName);
		return Result;
	}

	Result.NiagaraSystemPath = FSoftObjectPath(CreatedSystem);
	CreatedSystem->Modify();
	CreatedSystem->bFixedBounds = true;
	CreatedSystem->SetFixedBounds(FBox(FVector(-5000.0), FVector(5000.0)));

	UE_LOG(LogTemp, Display, TEXT("[JsonToNiagara] Created Niagara System: %s"), *Result.NiagaraSystemPath.ToString());

	TArray<UObject*> AssetsToSync;
	AssetsToSync.Add(CreatedSystem);

	UMaterialInterface* DefaultParticleMaterial = LoadObject<UMaterialInterface>(nullptr, Unity2NiagaraImporter::DefaultParticleMaterialPath);
	if (DefaultParticleMaterial == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Default particle material not found: %s"), Unity2NiagaraImporter::DefaultParticleMaterialPath);
	}

	const FUnity2NiagaraTextureResolver TextureResolver(TEXT("/Game/Textures"));

	CreatedSystem->Modify();
	for (const FUnityParticleSystemData& ParticleSystem : Root.ParticleSystems)
	{
		if (ParticleSystem.bIsEmptyEmitter)
		{
			continue;
		}

		UNiagaraEmitter* CreatedEmitter = CreateEmitterAsset(AssetToolsModule.Get(), UniquePackagePath, ParticleSystem.Name);
		if (CreatedEmitter == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Failed to create Niagara Emitter for: %s"), *ParticleSystem.Name);
			continue;
		}

		CreatedEmitter->Modify();
		CreatedEmitter->SetUniqueEmitterName(MakeSafeObjectName(ParticleSystem.Name));
		CreatedEmitter->bIsInheritable = false;
		ApplyEmitterMainSettingsToEmitter(*CreatedEmitter, ParticleSystem);
		ApplyEmitterStateParametersToEmitter(*CreatedEmitter, ParticleSystem);

		UMaterialInstanceConstant* CreatedMaterialInstance = nullptr;
		if (DefaultParticleMaterial != nullptr)
		{
			UTexture* ResolvedTexture = nullptr;
			for (const FUnityTextureReference& TextureReference : ParticleSystem.TextureReferences)
			{
				const FUnityResolvedTexture ResolvedTextureInfo = TextureResolver.Resolve(TextureReference);
				if (ResolvedTextureInfo.IsResolved())
				{
					ResolvedTexture = Cast<UTexture>(ResolvedTextureInfo.AssetPath.TryLoad());
					break;
				}
			}

			const FLinearColor MaterialParticleColor = Unity2NiagaraImporter::GetUnitySpawnColor(ParticleSystem);
			CreatedMaterialInstance = CreateMaterialInstanceAsset(
				AssetToolsModule.Get(),
				UniquePackagePath,
				ParticleSystem.Name,
				DefaultParticleMaterial,
				ResolvedTexture,
				MaterialParticleColor,
				ParticleSystem.NiagaraHint.BlendMode);

			if (CreatedMaterialInstance != nullptr)
			{
				Result.MaterialInstancePaths.Add(FSoftObjectPath(CreatedMaterialInstance));
				AssetsToSync.Add(CreatedMaterialInstance);
				ApplyMaterialToEmitter(*CreatedEmitter, *CreatedMaterialInstance);
			}
		}
		ApplyTrailRendererToEmitter(*CreatedEmitter, ParticleSystem, CreatedMaterialInstance);
		ApplyRendererSettingsToEmitter(*CreatedEmitter, ParticleSystem.Renderer);
		ApplyTextureSheetAnimationToEmitter(*CreatedEmitter, ParticleSystem.TextureSheetAnimation);
		AddTextureSheetAnimationUpdateModuleToEmitter(*CreatedEmitter, ParticleSystem);
		AddBurstSpawnModuleToEmitter(*CreatedEmitter, ParticleSystem);
		AddBurstSpawnRateFallbackModuleToEmitter(*CreatedEmitter, ParticleSystem);
		AddShapeLocationModuleToEmitter(*CreatedEmitter, ParticleSystem);
		AddShapeVelocityModuleToEmitter(*CreatedEmitter, ParticleSystem);
		AddSizeOverLifetimeModuleToEmitter(*CreatedEmitter, ParticleSystem);
		AddColorOverLifetimeModuleToEmitter(*CreatedEmitter, ParticleSystem);
		ApplyInitialNiagaraParametersToEmitter(*CreatedEmitter, ParticleSystem);
		ApplyParticleSpawnSetParametersToEmitter(*CreatedEmitter, ParticleSystem);
		ApplyParticleUpdateSetParametersToEmitter(*CreatedEmitter, ParticleSystem);
		LogEmitterVisibilityDiagnostics(*CreatedEmitter, ParticleSystem);

		CreatedEmitter->MarkPackageDirty();
		Result.NiagaraEmitterPaths.Add(FSoftObjectPath(CreatedEmitter));
		AssetsToSync.Add(CreatedEmitter);

		const FGuid EmitterVersion = CreatedEmitter->GetExposedVersion().VersionGuid;
		const FName EmitterHandleName(*MakeSafeObjectName(ParticleSystem.Name));
		FNiagaraEmitterHandle EmitterHandle = CreatedSystem->AddEmitterHandle(*CreatedEmitter, EmitterHandleName, EmitterVersion);
		FNiagaraEmitterHandle* SystemEmitterHandle = CreatedSystem->GetEmitterHandles().FindByPredicate(
			[EmitterHandle](const FNiagaraEmitterHandle& Candidate)
			{
				return Candidate.GetId() == EmitterHandle.GetId();
			});
		if (SystemEmitterHandle != nullptr && !SystemEmitterHandle->GetIsEnabled())
		{
			SystemEmitterHandle->SetIsEnabled(true, *CreatedSystem, false);
			EmitterHandle = *SystemEmitterHandle;
		}
		UE_LOG(
			LogTemp,
			Display,
			TEXT("[JsonToNiagara] Created Niagara Emitter: %s -> Handle: %s Enabled=%s"),
			*CreatedEmitter->GetPathName(),
			*EmitterHandle.GetName().ToString(),
			EmitterHandle.GetIsEnabled() ? TEXT("true") : TEXT("false"));

		const FUnityBurstData* FirstBurst = ParticleSystem.Emission.Bursts.Num() > 0 ? &ParticleSystem.Emission.Bursts[0] : nullptr;
		const FLinearColor ParticleColor = Unity2NiagaraImporter::GetUnitySpawnColor(ParticleSystem);
		const float VisualLifetime = Unity2NiagaraImporter::GetUnityVisualLifetime(ParticleSystem, 1.0f, TEXT("MainLogLifetime"));
		const float VisualSize = ParticleSystem.Main.StartSize.GetRepresentativeValue() * Unity2NiagaraImporter::GetVisualSizeScale(ParticleSystem);
		UE_LOG(
			LogTemp,
			Display,
			TEXT("[JsonToNiagara] Applied Unity Main: Emitter=%s Duration=%.3f Loop=%s Lifetime=%.3f Size=%.3f MaterialColor=(%.3f, %.3f, %.3f, %.3f) VisualSizeScale=%.3f VisualLifetimeScale=%.3f FirstBurst=%d"),
			*CreatedEmitter->GetName(),
			ParticleSystem.Main.Duration,
			ParticleSystem.Main.bLoop ? TEXT("true") : TEXT("false"),
			VisualLifetime,
			VisualSize,
			ParticleColor.R,
			ParticleColor.G,
			ParticleColor.B,
			ParticleColor.A,
			Unity2NiagaraImporter::GetVisualSizeScale(ParticleSystem),
			Unity2NiagaraImporter::GetVisualLifetimeScale(ParticleSystem),
			FirstBurst != nullptr ? FirstBurst->Count : 0);

		if (ParticleSystem.Shape.bEnabled)
		{
			UE_LOG(
				LogTemp,
				Display,
				TEXT("[JsonToNiagara] Parsed Shape: Emitter=%s Type=%s Radius=%.3f Angle=%.3f Arc=%.3f ArcMode=%s"),
				*CreatedEmitter->GetName(),
				*ParticleSystem.Shape.ShapeType,
				ParticleSystem.Shape.Radius,
				ParticleSystem.Shape.Angle,
				ParticleSystem.Shape.Arc,
				*ParticleSystem.Shape.ArcMode);
		}

		if (ParticleSystem.SizeOverLifetime.bEnabled)
		{
			UE_LOG(
				LogTemp,
				Display,
				TEXT("[JsonToNiagara] Applied SizeOverLifetime initial scale: Emitter=%s Scale=%.3f SizeKeys=%d"),
				*CreatedEmitter->GetName(),
				Unity2NiagaraImporter::GetUnityInitialSizeScale(ParticleSystem),
				ParticleSystem.SizeOverLifetime.Size.Keys.Num());
		}

		if (ParticleSystem.TextureSheetAnimation.bEnabled)
		{
			UE_LOG(
				LogTemp,
				Display,
				TEXT("[JsonToNiagara] Parsed TextureSheetAnimation: Emitter=%s Tiles=%dx%d Sprites=%d StartFrame=%.3f Frame=%.3f"),
				*CreatedEmitter->GetName(),
				ParticleSystem.TextureSheetAnimation.NumTilesX,
				ParticleSystem.TextureSheetAnimation.NumTilesY,
				ParticleSystem.TextureSheetAnimation.Sprites.Num(),
				ParticleSystem.TextureSheetAnimation.StartFrame.GetRepresentativeValue(),
				ParticleSystem.TextureSheetAnimation.FrameOverTime.GetRepresentativeValue());
		}

		if (CreatedMaterialInstance != nullptr)
		{
			UE_LOG(
				LogTemp,
				Display,
				TEXT("[JsonToNiagara] Applied Material Instance: %s -> Emitter: %s"),
				*CreatedMaterialInstance->GetPathName(),
				*CreatedEmitter->GetName());
		}
	}

	CreatedSystem->MarkPackageDirty();
	CreatedSystem->RequestCompile(true);
	CreatedSystem->WaitForCompilationComplete(false, false);
	LogNiagaraSystemDiagnostics(*CreatedSystem);

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	ContentBrowserModule.Get().SyncBrowserToAssets(AssetsToSync);

	return Result;
}

UNiagaraEmitter* FUnity2NiagaraImporter::CreateEmitterAsset(IAssetTools& AssetTools, const FString& PackagePath, const FString& SourceEmitterName)
{
	const FString SafeEmitterName = MakeSafeObjectName(SourceEmitterName.IsEmpty() ? TEXT("Emitter") : SourceEmitterName);
	const FString DesiredPackageName = FString::Printf(TEXT("%s/NE_%s"), *PackagePath, *SafeEmitterName);

	FString UniquePackageName;
	FString UniqueAssetName;
	AssetTools.CreateUniqueAssetName(DesiredPackageName, TEXT(""), UniquePackageName, UniqueAssetName);

	UNiagaraEmitterFactoryNew* Factory = NewObject<UNiagaraEmitterFactoryNew>();
	Factory->bEditAfterNew = false;
	Factory->EmitterToCopy = nullptr;
	Factory->bUseInheritance = false;
	Factory->bAddDefaultModulesAndRenderersToEmptyEmitter = true;

	UObject* CreatedAsset = AssetTools.CreateAsset(
		UniqueAssetName,
		FPackageName::GetLongPackagePath(UniquePackageName),
		UNiagaraEmitter::StaticClass(),
		Factory,
		TEXT("JsonToNiagara"));

	return Cast<UNiagaraEmitter>(CreatedAsset);
}

UMaterialInstanceConstant* FUnity2NiagaraImporter::CreateMaterialInstanceAsset(
	IAssetTools& AssetTools,
	const FString& PackagePath,
	const FString& SourceEmitterName,
	UMaterialInterface* ParentMaterial,
	UTexture* ParticleTexture,
	const FLinearColor& ParticleColor,
	const FString& UnityBlendMode)
{
	if (ParentMaterial == nullptr)
	{
		return nullptr;
	}

	const FString SafeEmitterName = MakeSafeObjectName(SourceEmitterName.IsEmpty() ? TEXT("Emitter") : SourceEmitterName);
	const FString DesiredPackageName = FString::Printf(TEXT("%s/MI_%s"), *PackagePath, *SafeEmitterName);

	FString UniquePackageName;
	FString UniqueAssetName;
	AssetTools.CreateUniqueAssetName(DesiredPackageName, TEXT(""), UniquePackageName, UniqueAssetName);

	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->bEditAfterNew = false;
	Factory->InitialParent = ParentMaterial;

	UObject* CreatedAsset = AssetTools.CreateAsset(
		UniqueAssetName,
		FPackageName::GetLongPackagePath(UniquePackageName),
		UMaterialInstanceConstant::StaticClass(),
		Factory,
		TEXT("JsonToNiagara"));

	UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(CreatedAsset);
	if (MaterialInstance == nullptr)
	{
		return nullptr;
	}

	MaterialInstance->Modify();
	if (ParticleTexture != nullptr)
	{
		MaterialInstance->SetTextureParameterValueEditorOnly(Unity2NiagaraImporter::ParticleTextureParameterName, ParticleTexture);
	}
	MaterialInstance->SetVectorParameterValueEditorOnly(Unity2NiagaraImporter::ParticleColorParameterName, ParticleColor);

	const EBlendMode MaterialBlendMode = Unity2NiagaraImporter::ConvertUnityBlendModeToMaterialBlendMode(UnityBlendMode);
	MaterialInstance->BasePropertyOverrides.bOverride_BlendMode = true;
	MaterialInstance->BasePropertyOverrides.BlendMode = MaterialBlendMode;
	MaterialInstance->BasePropertyOverrides.bOverride_TwoSided = true;
	MaterialInstance->BasePropertyOverrides.TwoSided = true;
	MaterialInstance->UpdateStaticPermutation();

	MaterialInstance->PostEditChange();
	MaterialInstance->MarkPackageDirty();

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Applied Material Blend: MI=%s UnityBlend=%s UnrealBlend=%s TwoSided=true"),
		*MaterialInstance->GetPathName(),
		UnityBlendMode.IsEmpty() ? TEXT("<empty>") : *UnityBlendMode,
		Unity2NiagaraImporter::GetMaterialBlendModeName(MaterialBlendMode));

	return MaterialInstance;
}

void FUnity2NiagaraImporter::ApplyEmitterMainSettingsToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	Emitter.Modify();
	const bool bLocalSpace = ParticleSystem.Main.SimulationSpace.Equals(TEXT("Local"), ESearchCase::IgnoreCase);
	const int32 BurstCountEstimate = Unity2NiagaraImporter::GetUnityBurstCountEstimate(ParticleSystem);
	const int32 MaxParticles = FMath::Max(ParticleSystem.Main.MaxParticles > 0 ? ParticleSystem.Main.MaxParticles : 2048, 16);
	const int32 PreAllocationCount = BurstCountEstimate > 0 ? FMath::Clamp(BurstCountEstimate * 2, 16, MaxParticles) : 0;

	EmitterData->bLocalSpace = bLocalSpace;
	EmitterData->SimTarget = ENiagaraSimTarget::CPUSim;
	EmitterData->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
	EmitterData->FixedBounds = FBox(FVector(-5000.0), FVector(5000.0));
	if (PreAllocationCount > 0)
	{
		EmitterData->AllocationMode = EParticleAllocationMode::ManualEstimate;
		EmitterData->PreAllocationCount = PreAllocationCount;
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Applied Emitter Main Settings: Emitter=%s Duration=%.3f Loop=%s SimulationSpace=%s LocalSpace=%s SimTarget=CPU PreAllocation=%d FixedBounds=true"),
		*Emitter.GetName(),
		ParticleSystem.Main.Duration,
		ParticleSystem.Main.bLoop ? TEXT("true") : TEXT("false"),
		ParticleSystem.Main.SimulationSpace.IsEmpty() ? TEXT("<empty>") : *ParticleSystem.Main.SimulationSpace,
		bLocalSpace ? TEXT("true") : TEXT("false"),
		PreAllocationCount);
}

void FUnity2NiagaraImporter::ApplyEmitterStateParametersToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (ScriptSource == nullptr || ScriptSource->NodeGraph == nullptr || EmitterData->EmitterUpdateScriptProps.Script == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing emitter update graph for Emitter State params: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeFunctionCall* EmitterStateNode = Unity2NiagaraImporter::FindEmitterStateNode(*ScriptSource->NodeGraph);
	if (EmitterStateNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Emitter State node not found: Emitter=%s"), *Emitter.GetName());
		return;
	}

	const FString UniqueEmitterName = Emitter.GetUniqueEmitterName();
	UNiagaraScript& EmitterUpdateScript = *EmitterData->EmitterUpdateScriptProps.Script;
	const float RepresentativeLifetime = Unity2NiagaraImporter::GetUnityVisualLifetime(ParticleSystem, 1.0f, TEXT("EmitterStateLifetime"));
	const float LoopDuration = FMath::Max3(ParticleSystem.Main.Duration, RepresentativeLifetime, 0.1f);
	const bool bApplyEmitterStateRapidParams = false;
	const int32 LifeCycleMode = 0;
	const bool bForceInfiniteEmitterState = false;
	const int32 LoopBehavior = bForceInfiniteEmitterState ? 0 : (ParticleSystem.Main.bLoop ? 0 : 2);
	const int32 InactiveResponse = 0;
	const int32 LoopCount = ParticleSystem.Main.bLoop ? 0 : 1;
	int32 ChangedCount = 0;

	if (bApplyEmitterStateRapidParams)
	{
		ChangedCount += Unity2NiagaraImporter::TrySetRapidIntFromPin(UniqueEmitterName, EmitterUpdateScript, *EmitterStateNode, TEXT("Life Cycle Mode"), LifeCycleMode) ? 1 : 0;
		ChangedCount += Unity2NiagaraImporter::TrySetRapidIntFromPin(UniqueEmitterName, EmitterUpdateScript, *EmitterStateNode, TEXT("Loop Behavior"), LoopBehavior) ? 1 : 0;
		ChangedCount += Unity2NiagaraImporter::TrySetRapidIntFromPin(UniqueEmitterName, EmitterUpdateScript, *EmitterStateNode, TEXT("Inactive Response"), InactiveResponse) ? 1 : 0;
		ChangedCount += Unity2NiagaraImporter::TrySetRapidIntFromPin(UniqueEmitterName, EmitterUpdateScript, *EmitterStateNode, TEXT("UseLoopDelay"), 0) ? 1 : 0;
		if (!bForceInfiniteEmitterState)
		{
			ChangedCount += Unity2NiagaraImporter::TrySetRapidIntFromPin(UniqueEmitterName, EmitterUpdateScript, *EmitterStateNode, TEXT("Loop Duration Mode"), 0) ? 1 : 0;
			ChangedCount += Unity2NiagaraImporter::TrySetRapidFloat(UniqueEmitterName, EmitterUpdateScript, *EmitterStateNode, TEXT("Loop Duration"), LoopDuration) ? 1 : 0;
			ChangedCount += Unity2NiagaraImporter::TrySetRapidFloat(UniqueEmitterName, EmitterUpdateScript, *EmitterStateNode, TEXT("Loop Delay"), 0.0f) ? 1 : 0;
			ChangedCount += Unity2NiagaraImporter::TrySetRapidIntFromPin(UniqueEmitterName, EmitterUpdateScript, *EmitterStateNode, TEXT("Loop Count"), LoopCount) ? 1 : 0;
		}
	}

	if (bApplyEmitterStateRapidParams && ChangedCount == 0)
	{
		FString InputPinNames;
		for (const UEdGraphPin* Pin : EmitterStateNode->Pins)
		{
			if (Pin != nullptr && Pin->Direction == EGPD_Input)
			{
				if (!InputPinNames.IsEmpty())
				{
					InputPinNames.Append(TEXT(", "));
				}
				InputPinNames.Append(Pin->PinName.ToString());
			}
		}
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("[JsonToNiagara] Emitter State params matched no pins: Emitter=%s Node=%s InputPins=%s"),
			*Emitter.GetName(),
			*EmitterStateNode->GetFunctionName(),
			InputPinNames.IsEmpty() ? TEXT("<none>") : *InputPinNames);
	}

	FString InputPinDetails;
	for (const UEdGraphPin* Pin : EmitterStateNode->Pins)
	{
		if (Pin != nullptr && Pin->Direction == EGPD_Input)
		{
			if (!InputPinDetails.IsEmpty())
			{
				InputPinDetails.Append(TEXT(" | "));
			}

			const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			InputPinDetails.Append(FString::Printf(
				TEXT("%s Type=%s Default=%s"),
				*Pin->PinName.ToString(),
				*PinType.GetName(),
				Pin->DefaultValue.IsEmpty() ? TEXT("<empty>") : *Pin->DefaultValue));
		}
	}
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Emitter State Pin Defaults: Emitter=%s %s"),
		*Emitter.GetName(),
		InputPinDetails.IsEmpty() ? TEXT("<none>") : *InputPinDetails);

	EmitterData->InvalidateCompileResults();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Applied Emitter State Params: Emitter=%s Node=%s ApplyRapid=%s Changed=%d Duration=%.3f LifeCycleMode=%d LoopBehavior=%d ForceInfinite=%s UnityLoop=%s LoopCount=%d InactiveResponse=%d"),
		*Emitter.GetName(),
		*EmitterStateNode->GetFunctionName(),
		bApplyEmitterStateRapidParams ? TEXT("true") : TEXT("false"),
		ChangedCount,
		LoopDuration,
		LifeCycleMode,
		LoopBehavior,
		bForceInfiniteEmitterState ? TEXT("true") : TEXT("false"),
		ParticleSystem.Main.bLoop ? TEXT("true") : TEXT("false"),
		LoopCount,
		InactiveResponse);
}

void FUnity2NiagaraImporter::ApplyMaterialToEmitter(UNiagaraEmitter& Emitter, UMaterialInterface& Material)
{
	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	EmitterData->ForEachRenderer([&Material](UNiagaraRendererProperties* Renderer)
	{
		UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderer);
		if (SpriteRenderer == nullptr)
		{
			return;
		}

		SpriteRenderer->Modify();
		SpriteRenderer->Material = &Material;
		SpriteRenderer->PostEditChange();
	});
}

void FUnity2NiagaraImporter::ApplyTrailRendererToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem, UMaterialInterface* Material)
{
	if (!ParticleSystem.Trails.bEnabled)
	{
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	bool bHasRibbonRenderer = false;
	EmitterData->ForEachRenderer([&bHasRibbonRenderer](UNiagaraRendererProperties* Renderer)
	{
		if (Cast<UNiagaraRibbonRendererProperties>(Renderer) != nullptr)
		{
			bHasRibbonRenderer = true;
		}
	});

	if (bHasRibbonRenderer)
	{
		return;
	}

	UNiagaraRibbonRendererProperties* RibbonRenderer = NewObject<UNiagaraRibbonRendererProperties>(&Emitter, NAME_None, RF_Transactional);
	if (RibbonRenderer == nullptr)
	{
		return;
	}

	RibbonRenderer->Modify();
	RibbonRenderer->Material = Material;
	RibbonRenderer->FacingMode = ENiagaraRibbonFacingMode::Screen;
	RibbonRenderer->MaxNumRibbons = 1;
	RibbonRenderer->DrawDirection = ENiagaraRibbonDrawDirection::FrontToBack;
	RibbonRenderer->Shape = ENiagaraRibbonShapeMode::Plane;
	RibbonRenderer->bUseGPUInit = false;
	RibbonRenderer->bLinkOrderUseUniqueID = true;
	RibbonRenderer->bCastShadows = false;
	RibbonRenderer->PostEditChange();

	Emitter.AddRenderer(RibbonRenderer, Emitter.GetExposedVersion().VersionGuid);
	EmitterData->InvalidateCompileResults();

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Added Trail Ribbon Renderer: Emitter=%s Ratio=%.3f Lifetime=%.3f MinVertexDistance=%.3f WorldSpace=%s DieWithParticles=%s"),
		*Emitter.GetName(),
		ParticleSystem.Trails.Ratio,
		ParticleSystem.Trails.Lifetime,
		ParticleSystem.Trails.MinVertexDistance,
		ParticleSystem.Trails.bWorldSpace ? TEXT("true") : TEXT("false"),
		ParticleSystem.Trails.bDieWithParticles ? TEXT("true") : TEXT("false"));
}

void FUnity2NiagaraImporter::ApplyRendererSettingsToEmitter(UNiagaraEmitter& Emitter, const FUnityRendererData& RendererData)
{
	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	EmitterData->ForEachRenderer([&RendererData](UNiagaraRendererProperties* Renderer)
	{
		UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderer);
		if (SpriteRenderer == nullptr)
		{
			return;
		}

		SpriteRenderer->Modify();
		SpriteRenderer->SortOrderHint = RendererData.SortingOrder + FMath::RoundToInt(RendererData.SortingFudge);
		SpriteRenderer->PivotInUVSpace = FVector2D(
			FMath::Clamp(static_cast<double>(0.5f + RendererData.Pivot.X), 0.0, 1.0),
			FMath::Clamp(static_cast<double>(0.5f + RendererData.Pivot.Y), 0.0, 1.0));

		if (RendererData.RenderMode.Equals(TEXT("Stretch"), ESearchCase::IgnoreCase))
		{
			SpriteRenderer->Alignment = ENiagaraSpriteAlignment::VelocityAligned;
		}
		else
		{
			SpriteRenderer->Alignment = ENiagaraSpriteAlignment::Unaligned;
		}

		if (RendererData.Alignment.Equals(TEXT("View"), ESearchCase::IgnoreCase))
		{
			SpriteRenderer->FacingMode = ENiagaraSpriteFacingMode::FaceCameraPlane;
		}
		else if (RendererData.Alignment.Equals(TEXT("Facing"), ESearchCase::IgnoreCase))
		{
			SpriteRenderer->FacingMode = ENiagaraSpriteFacingMode::FaceCamera;
		}
		else
		{
			SpriteRenderer->FacingMode = ENiagaraSpriteFacingMode::Automatic;
		}

		SpriteRenderer->PostEditChange();
	});

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Applied Renderer Settings: Emitter=%s RenderMode=%s Alignment=%s Sort=%d SortingFudge=%.3f LengthScale=%.3f VelocityScale=%.3f Pivot=(%.3f, %.3f) AllowRoll=%s"),
		*Emitter.GetName(),
		RendererData.RenderMode.IsEmpty() ? TEXT("<empty>") : *RendererData.RenderMode,
		RendererData.Alignment.IsEmpty() ? TEXT("<empty>") : *RendererData.Alignment,
		RendererData.SortingOrder,
		RendererData.SortingFudge,
		RendererData.LengthScale,
		RendererData.VelocityScale,
		RendererData.Pivot.X,
		RendererData.Pivot.Y,
		RendererData.bAllowRoll ? TEXT("true") : TEXT("false"));
}

void FUnity2NiagaraImporter::ApplyTextureSheetAnimationToEmitter(UNiagaraEmitter& Emitter, const FUnityTextureSheetAnimationModule& TextureSheetAnimation)
{
	if (!TextureSheetAnimation.bEnabled)
	{
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	const int32 NumTilesX = FMath::Max(TextureSheetAnimation.NumTilesX, 1);
	const int32 NumTilesY = FMath::Max(TextureSheetAnimation.NumTilesY, 1);
	int32 ChangedRendererCount = 0;

	EmitterData->ForEachRenderer([&TextureSheetAnimation, NumTilesX, NumTilesY, &ChangedRendererCount](UNiagaraRendererProperties* Renderer)
	{
		UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderer);
		if (SpriteRenderer == nullptr)
		{
			return;
		}

		SpriteRenderer->Modify();
		SpriteRenderer->SubImageSize = FVector2D(static_cast<double>(NumTilesX), static_cast<double>(NumTilesY));
		SpriteRenderer->bSubImageBlend = TextureSheetAnimation.FrameOverTime.Keys.Num() > 1 || NumTilesX * NumTilesY > 1;
		SpriteRenderer->PostEditChange();
		++ChangedRendererCount;
	});

	if (ChangedRendererCount > 0)
	{
		EmitterData->InvalidateCompileResults();
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Applied Texture Sheet Renderer: Emitter=%s Tiles=%dx%d Blend=%s Renderers=%d"),
		*Emitter.GetName(),
		NumTilesX,
		NumTilesY,
		TextureSheetAnimation.FrameOverTime.Keys.Num() > 1 || NumTilesX * NumTilesY > 1 ? TEXT("true") : TEXT("false"),
		ChangedRendererCount);
}

void FUnity2NiagaraImporter::AddTextureSheetAnimationUpdateModuleToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	if (!ParticleSystem.TextureSheetAnimation.bEnabled)
	{
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (ScriptSource == nullptr || ScriptSource->NodeGraph == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing graph source for texture sheet animation module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeOutput* ParticleUpdateOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
		ENiagaraScriptUsage::ParticleUpdateScript,
		EmitterData->UpdateScriptProps.Script != nullptr ? EmitterData->UpdateScriptProps.Script->GetUsageId() : FGuid());
	if (ParticleUpdateOutputNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing Particle Update output node for texture sheet animation module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeFunctionCall* SubUVNode = Unity2NiagaraImporter::AddScriptModuleFromPath(
		TEXT("/Niagara/Modules/Update/SubUV/SubUVAnimation.SubUVAnimation"),
		*ParticleUpdateOutputNode);
	if (SubUVNode == nullptr)
	{
		return;
	}

	EmitterData->InvalidateCompileResults();

	const int32 TotalFrames = Unity2NiagaraImporter::GetUnityTextureSheetTotalFrames(ParticleSystem.TextureSheetAnimation);
	const float StartFrame = Unity2NiagaraImporter::ClampUnitySubImageIndex(ParticleSystem.TextureSheetAnimation.StartFrame.GetRepresentativeValue(0.0f), ParticleSystem.TextureSheetAnimation);
	const float EndFrame = Unity2NiagaraImporter::EvaluateUnityTextureSheetFrameAtTime(ParticleSystem.TextureSheetAnimation, 1.0f);
	const float FrameRange = FMath::Max(EndFrame - StartFrame, 0.0f);
	const float CycleCount = static_cast<float>(FMath::Max(ParticleSystem.TextureSheetAnimation.CycleCount, 1));
	const FString FrameSamples = Unity2NiagaraImporter::FormatUnityTextureSheetSamples(ParticleSystem);
	int32 ChangedSubUVParameterCount = 0;
	FString InputPinSummary;
	UNiagaraScript* ParticleUpdateScript = EmitterData->UpdateScriptProps.Script;
	if (ParticleUpdateScript != nullptr)
	{
		const FString UniqueEmitterName = Emitter.GetUniqueEmitterName();
		for (UEdGraphPin* Pin : SubUVNode->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			const FString PinName = Pin->PinName.ToString();
			if (PinName.Equals(TEXT("InputMap"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (!InputPinSummary.IsEmpty())
			{
				InputPinSummary += TEXT(" | ");
			}
			InputPinSummary += FString::Printf(TEXT("%s:%s"), *PinName, *PinType.GetName());

			if (PinType == FNiagaraTypeDefinition::GetFloatDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Start"), TEXT("First"), TEXT("Initial") })
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Frame"), TEXT("Image"), TEXT("Index") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleUpdateScript, *SubUVNode, Pin->PinName, PinType, StartFrame);
				++ChangedSubUVParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetFloatDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Range") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleUpdateScript, *SubUVNode, Pin->PinName, PinType, FrameRange);
				++ChangedSubUVParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetFloatDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Rate"), TEXT("Cycle"), TEXT("Loop") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleUpdateScript, *SubUVNode, Pin->PinName, PinType, CycleCount);
				++ChangedSubUVParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetFloatDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Frame"), TEXT("Image"), TEXT("Index") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleUpdateScript, *SubUVNode, Pin->PinName, PinType, EndFrame);
				++ChangedSubUVParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetFloatDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Total"), TEXT("Count"), TEXT("Num") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleUpdateScript, *SubUVNode, Pin->PinName, PinType, static_cast<float>(TotalFrames));
				++ChangedSubUVParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetIntDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Mode") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleUpdateScript, *SubUVNode, Pin->PinName, PinType, 2);
				++ChangedSubUVParameterCount;
			}
		}
	}
	if (InputPinSummary.IsEmpty())
	{
		InputPinSummary = TEXT("<none>");
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Added Texture Sheet Animation module: Emitter=%s Changed=%d Tiles=%dx%d TotalFrames=%d StartFrame=%.3f EndFrame=%.3f FrameRange=%.3f Cycles=%.3f Samples=%s InputPins=%s"),
		*Emitter.GetName(),
		ChangedSubUVParameterCount,
		FMath::Max(ParticleSystem.TextureSheetAnimation.NumTilesX, 1),
		FMath::Max(ParticleSystem.TextureSheetAnimation.NumTilesY, 1),
		TotalFrames,
		StartFrame,
		EndFrame,
		FrameRange,
		CycleCount,
		*FrameSamples,
		*InputPinSummary);
}

void FUnity2NiagaraImporter::AddBurstSpawnModuleToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	if (ParticleSystem.Emission.Bursts.IsEmpty())
	{
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (ScriptSource == nullptr || ScriptSource->NodeGraph == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing graph source for burst module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeOutput* EmitterUpdateOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
		ENiagaraScriptUsage::EmitterUpdateScript,
		EmitterData->EmitterUpdateScriptProps.Script->GetUsageId());
	if (EmitterUpdateOutputNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing Emitter Update output node for burst module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraScript* EmitterUpdateScript = EmitterData->EmitterUpdateScriptProps.Script;
	int32 AddedBurstModuleCount = 0;
	for (const FUnityBurstData& Burst : ParticleSystem.Emission.Bursts)
	{
		UNiagaraNodeFunctionCall* BurstNode = Unity2NiagaraImporter::AddScriptModuleFromPath(
			TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous"),
			*EmitterUpdateOutputNode);
		if (BurstNode == nullptr)
		{
			continue;
		}

		++AddedBurstModuleCount;
		int32 ChangedBurstParameterCount = 0;
		if (EmitterUpdateScript != nullptr)
		{
			const FString UniqueEmitterName = Emitter.GetUniqueEmitterName();
			const int32 BurstCount = Unity2NiagaraImporter::GetUnityVisualBurstCount(ParticleSystem, Burst);
			const float BurstTime = FMath::Max(Burst.Time, 0.0f);
			const float RepeatInterval = FMath::Max(Burst.RepeatInterval, 0.0f);
			const float SpawnProbability = FMath::Clamp(Burst.Probability, 0.0f, 1.0f);
			Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, TEXT("SpawnCountInt"), FNiagaraTypeDefinition::GetIntDef(), BurstCount);
			Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, TEXT("SpawnCount"), FNiagaraTypeDefinition::GetFloatDef(), static_cast<float>(BurstCount));
			Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, TEXT("InterpStartDt"), FNiagaraTypeDefinition::GetFloatDef(), BurstTime);
			Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, TEXT("IntervalDt"), FNiagaraTypeDefinition::GetFloatDef(), RepeatInterval);
			Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, TEXT("SpawnProbability"), FNiagaraTypeDefinition::GetFloatDef(), SpawnProbability);
			Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, TEXT("SpawnGroup"), FNiagaraTypeDefinition::GetIntDef(), 0);
			Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, TEXT("CycleCount"), FNiagaraTypeDefinition::GetIntDef(), FMath::Max(Burst.CycleCount, 1));
			for (UEdGraphPin* Pin : BurstNode->Pins)
			{
				if (Pin == nullptr || Pin->Direction != EGPD_Input || Pin->PinName == FName(TEXT("InputMap")))
				{
					continue;
				}

				const FString PinName = Pin->PinName.ToString();
				const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
				if (PinType == FNiagaraTypeDefinition::GetIntDef()
					&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Count"), TEXT("Num"), TEXT("Spawn") }))
				{
					Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, Pin->PinName, PinType, BurstCount);
					++ChangedBurstParameterCount;
				}
				else if (PinType == FNiagaraTypeDefinition::GetFloatDef()
					&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Count"), TEXT("Num"), TEXT("Spawn") }))
				{
					Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, Pin->PinName, PinType, static_cast<float>(BurstCount));
					++ChangedBurstParameterCount;
				}
				else if (PinType == FNiagaraTypeDefinition::GetFloatDef()
					&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Time"), TEXT("Start") }))
				{
					Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, Pin->PinName, PinType, BurstTime);
					++ChangedBurstParameterCount;
				}
				else if (PinType == FNiagaraTypeDefinition::GetFloatDef()
					&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Interval"), TEXT("Repeat") }))
				{
					Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, Pin->PinName, PinType, RepeatInterval);
					++ChangedBurstParameterCount;
				}
				else if (PinType == FNiagaraTypeDefinition::GetFloatDef()
					&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Probability"), TEXT("Chance") }))
				{
					Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, Pin->PinName, PinType, SpawnProbability);
					++ChangedBurstParameterCount;
				}
				else if (PinType == FNiagaraTypeDefinition::GetIntDef()
					&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Cycle"), TEXT("Loop") }))
				{
					Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *BurstNode, Pin->PinName, PinType, FMath::Max(Burst.CycleCount, 1));
					++ChangedBurstParameterCount;
				}
			}
		}

		UE_LOG(
			LogTemp,
			Display,
			TEXT("[JsonToNiagara] Added Burst Spawn module: Emitter=%s BurstCount=%d SourceBurstCount=%d BurstTime=%.3f CycleCount=%d RepeatInterval=%.3f Probability=%.3f RapidParams=%s Changed=%d"),
			*Emitter.GetName(),
			Unity2NiagaraImporter::GetUnityVisualBurstCount(ParticleSystem, Burst),
			Burst.Count,
			Burst.Time,
			Burst.CycleCount,
			Burst.RepeatInterval,
			Burst.Probability,
			EmitterUpdateScript != nullptr ? TEXT("true") : TEXT("false"),
			ChangedBurstParameterCount);
	}

	if (AddedBurstModuleCount > 0)
	{
		EmitterData->InvalidateCompileResults();
	}
}

void FUnity2NiagaraImporter::AddBurstSpawnRateFallbackModuleToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	const int32 BurstCount = Unity2NiagaraImporter::GetUnityBurstCountEstimate(ParticleSystem);
	if (BurstCount <= 1)
	{
		return;
	}

	const bool bNeedsFallback = Unity2NiagaraImporter::NameContainsAny(ParticleSystem.Name, { TEXT("boke"), TEXT("bokeh"), TEXT("spark"), TEXT("spike") });
	if (!bNeedsFallback)
	{
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (ScriptSource == nullptr || ScriptSource->NodeGraph == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing graph source for burst fallback spawn rate module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeOutput* EmitterUpdateOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
		ENiagaraScriptUsage::EmitterUpdateScript,
		EmitterData->EmitterUpdateScriptProps.Script->GetUsageId());
	if (EmitterUpdateOutputNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing Emitter Update output node for burst fallback spawn rate module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeFunctionCall* SpawnRateNode = Unity2NiagaraImporter::AddScriptModuleFromPath(
		TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"),
		*EmitterUpdateOutputNode);
	if (SpawnRateNode == nullptr)
	{
		return;
	}

	UNiagaraScript* EmitterUpdateScript = EmitterData->EmitterUpdateScriptProps.Script;
	int32 ChangedSpawnRateParameterCount = 0;
	const float SpawnWindow = FMath::Max3(ParticleSystem.Main.Duration, Unity2NiagaraImporter::GetUnityVisualLifetime(ParticleSystem, 1.0f, TEXT("FallbackSpawnWindow")), 0.5f);
	const float FallbackSpawnRate = static_cast<float>(BurstCount) / SpawnWindow;
	if (EmitterUpdateScript != nullptr)
	{
		const FString UniqueEmitterName = Emitter.GetUniqueEmitterName();
		Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *SpawnRateNode, TEXT("SpawnRate"), FNiagaraTypeDefinition::GetFloatDef(), FallbackSpawnRate);
		++ChangedSpawnRateParameterCount;

		for (UEdGraphPin* Pin : SpawnRateNode->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Input || Pin->PinName == FName(TEXT("InputMap")))
			{
				continue;
			}

			const FString PinName = Pin->PinName.ToString();
			const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (PinType == FNiagaraTypeDefinition::GetFloatDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Rate"), TEXT("Spawn") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *EmitterUpdateScript, *SpawnRateNode, Pin->PinName, PinType, FallbackSpawnRate);
				++ChangedSpawnRateParameterCount;
			}
		}
	}

	EmitterData->InvalidateCompileResults();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Added Burst SpawnRate Fallback: Emitter=%s BurstCount=%d Window=%.3f SpawnRate=%.3f Changed=%d"),
		*Emitter.GetName(),
		BurstCount,
		SpawnWindow,
		FallbackSpawnRate,
		ChangedSpawnRateParameterCount);
}

void FUnity2NiagaraImporter::AddShapeLocationModuleToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	if (!ParticleSystem.Shape.bEnabled)
	{
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (ScriptSource == nullptr || ScriptSource->NodeGraph == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing graph source for shape module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeOutput* ParticleSpawnOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
		ENiagaraScriptUsage::ParticleSpawnScript,
		EmitterData->SpawnScriptProps.Script->GetUsageId());
	if (ParticleSpawnOutputNode == nullptr)
	{
		ParticleSpawnOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
			ENiagaraScriptUsage::ParticleSpawnScriptInterpolated,
			EmitterData->SpawnScriptProps.Script->GetUsageId());
	}
	if (ParticleSpawnOutputNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing Particle Spawn output node for shape module: %s"), *Emitter.GetName());
		return;
	}

	const FString ShapeType = ParticleSystem.Shape.ShapeType;
	const TCHAR* ShapeModulePath = TEXT("/Niagara/Modules/Spawn/Location/SphereLocation.SphereLocation");
	if (ShapeType.Equals(TEXT("Box"), ESearchCase::IgnoreCase))
	{
		ShapeModulePath = TEXT("/Niagara/Modules/Spawn/Location/BoxLocation.BoxLocation");
	}
	else if (ShapeType.Equals(TEXT("Cone"), ESearchCase::IgnoreCase))
	{
		ShapeModulePath = TEXT("/Niagara/Modules/Spawn/Location/ConeLocation.ConeLocation");
	}
	else if (ShapeType.Equals(TEXT("Circle"), ESearchCase::IgnoreCase) || ShapeType.Equals(TEXT("Hemisphere"), ESearchCase::IgnoreCase))
	{
		ShapeModulePath = TEXT("/Niagara/Modules/Spawn/Location/CylinderLocation.CylinderLocation");
	}
	else if (ShapeType.Equals(TEXT("Donut"), ESearchCase::IgnoreCase) || ShapeType.Equals(TEXT("Torus"), ESearchCase::IgnoreCase))
	{
		ShapeModulePath = TEXT("/Niagara/Modules/Spawn/Location/TorusLocation.TorusLocation");
	}

	UNiagaraNodeFunctionCall* ShapeNode = Unity2NiagaraImporter::AddScriptModuleFromPath(ShapeModulePath, *ParticleSpawnOutputNode);
	if (ShapeNode == nullptr)
	{
		return;
	}

	int32 ChangedShapeParameterCount = 0;
	UNiagaraScript* ParticleSpawnScript = EmitterData->SpawnScriptProps.Script;
	if (ParticleSpawnScript != nullptr)
	{
		const FString UniqueEmitterName = Emitter.GetUniqueEmitterName();
		const float Radius = Unity2NiagaraImporter::ConvertUnitySizeToNiagaraSpriteSize(Unity2NiagaraImporter::GetApproximateShapeRadius(ParticleSystem));
		const FVector3f Position = Unity2NiagaraImporter::ConvertUnityVectorToNiagaraPosition(ParticleSystem.Shape.Position);
		for (UEdGraphPin* Pin : ShapeNode->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			const FString PinName = Pin->PinName.ToString();
			const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (PinType == FNiagaraTypeDefinition::GetFloatDef() && Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Radius") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleSpawnScript, *ShapeNode, Pin->PinName, PinType, Radius);
				++ChangedShapeParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetFloatDef() && Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Hemisphere"), TEXT("Thickness") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleSpawnScript, *ShapeNode, Pin->PinName, PinType, ParticleSystem.Shape.RadiusThickness);
				++ChangedShapeParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetFloatDef() && Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Height"), TEXT("Length") }))
			{
				const float Length = Unity2NiagaraImporter::ConvertUnitySizeToNiagaraSpriteSize(FMath::Max(ParticleSystem.Shape.Length, ParticleSystem.Shape.Radius));
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleSpawnScript, *ShapeNode, Pin->PinName, PinType, Length);
				++ChangedShapeParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetVec3Def() && Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Position"), TEXT("Offset") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleSpawnScript, *ShapeNode, Pin->PinName, PinType, Position);
				++ChangedShapeParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetVec3Def() && Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Size"), TEXT("Scale"), TEXT("Extent") }))
			{
				const FVector3f BoxSize = Unity2NiagaraImporter::ConvertUnityVectorToNiagaraPosition(ParticleSystem.Shape.Scale);
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleSpawnScript, *ShapeNode, Pin->PinName, PinType, BoxSize);
				++ChangedShapeParameterCount;
			}
		}
	}

	EmitterData->InvalidateCompileResults();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Added Shape Location module: Emitter=%s Shape=%s Module=%s ApproxRadius=%.3f Radius=%.3f Length=%.3f Scale=(%.3f, %.3f, %.3f) Angle=%.3f Arc=%.3f RandomDir=%.3f SphericalDir=%.3f RandomPos=%.3f RapidParams=%d"),
		*Emitter.GetName(),
		*ParticleSystem.Shape.ShapeType,
		ShapeModulePath,
		Unity2NiagaraImporter::GetApproximateShapeRadius(ParticleSystem),
		ParticleSystem.Shape.Radius,
		ParticleSystem.Shape.Length,
		ParticleSystem.Shape.Scale.X,
		ParticleSystem.Shape.Scale.Y,
		ParticleSystem.Shape.Scale.Z,
		ParticleSystem.Shape.Angle,
		ParticleSystem.Shape.Arc,
		ParticleSystem.Shape.RandomDirectionAmount,
		ParticleSystem.Shape.SphericalDirectionAmount,
		ParticleSystem.Shape.RandomPositionAmount,
		ChangedShapeParameterCount);
}

void FUnity2NiagaraImporter::AddShapeVelocityModuleToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	if (!Unity2NiagaraImporter::ShouldUseShapeVelocityModule(ParticleSystem))
	{
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (ScriptSource == nullptr || ScriptSource->NodeGraph == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing graph source for shape velocity module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeOutput* ParticleSpawnOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
		ENiagaraScriptUsage::ParticleSpawnScript,
		EmitterData->SpawnScriptProps.Script->GetUsageId());
	if (ParticleSpawnOutputNode == nullptr)
	{
		ParticleSpawnOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
			ENiagaraScriptUsage::ParticleSpawnScriptInterpolated,
			EmitterData->SpawnScriptProps.Script->GetUsageId());
	}
	if (ParticleSpawnOutputNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing Particle Spawn output node for shape velocity module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeFunctionCall* VelocityNode = Unity2NiagaraImporter::AddScriptModuleFromPath(
		TEXT("/Niagara/Modules/Spawn/Velocity/AddVelocityInCone.AddVelocityInCone"),
		*ParticleSpawnOutputNode);
	if (VelocityNode == nullptr)
	{
		return;
	}

	const float UnityStartSpeed = Unity2NiagaraImporter::GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Main.StartSpeed, 0.0f, TEXT("ShapeVelocityStartSpeed"))
		* Unity2NiagaraImporter::GetVisualSpeedScale(ParticleSystem);
	const float VelocityMagnitude = Unity2NiagaraImporter::ConvertUnitySpeedToNiagaraVelocity(UnityStartSpeed);
	const float ConeAngle = FMath::Clamp(ParticleSystem.Shape.Angle > 0.0f ? ParticleSystem.Shape.Angle : 90.0f, 1.0f, 180.0f);
	const FVector3f ConeAxis = Unity2NiagaraImporter::GetDeterministicShapeDirection(ParticleSystem);

	int32 ChangedVelocityParameterCount = 0;
	FString InputPinSummary;
	UNiagaraScript* ParticleSpawnScript = EmitterData->SpawnScriptProps.Script;
	if (ParticleSpawnScript != nullptr)
	{
		const FString UniqueEmitterName = Emitter.GetUniqueEmitterName();
		for (UEdGraphPin* Pin : VelocityNode->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			const FString PinName = Pin->PinName.ToString();
			if (PinName.Equals(TEXT("InputMap"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (!InputPinSummary.IsEmpty())
			{
				InputPinSummary += TEXT(" | ");
			}
			InputPinSummary += FString::Printf(TEXT("%s:%s"), *PinName, *PinType.GetName());

			if (PinType == FNiagaraTypeDefinition::GetFloatDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Velocity"), TEXT("Speed"), TEXT("Magnitude"), TEXT("Strength") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleSpawnScript, *VelocityNode, Pin->PinName, PinType, VelocityMagnitude);
				++ChangedVelocityParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetFloatDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Angle"), TEXT("Cone") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleSpawnScript, *VelocityNode, Pin->PinName, PinType, ConeAngle);
				++ChangedVelocityParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetVec3Def()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Direction"), TEXT("Axis"), TEXT("Vector") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleSpawnScript, *VelocityNode, Pin->PinName, PinType, ConeAxis);
				++ChangedVelocityParameterCount;
			}
		}
	}
	if (InputPinSummary.IsEmpty())
	{
		InputPinSummary = TEXT("<none>");
	}

	EmitterData->InvalidateCompileResults();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Added Shape Velocity module: Emitter=%s Module=AddVelocityInCone Burst=%d UnitySpeed=%.3f Velocity=%.3f ConeAngle=%.3f Axis=(%.3f, %.3f, %.3f) Changed=%d InputPins=%s"),
		*Emitter.GetName(),
		Unity2NiagaraImporter::GetUnityBurstCountEstimate(ParticleSystem),
		UnityStartSpeed,
		VelocityMagnitude,
		ConeAngle,
		ConeAxis.X,
		ConeAxis.Y,
		ConeAxis.Z,
		ChangedVelocityParameterCount,
		*InputPinSummary);
}

void FUnity2NiagaraImporter::AddSizeOverLifetimeModuleToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	if (!ParticleSystem.SizeOverLifetime.bEnabled)
	{
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (ScriptSource == nullptr || ScriptSource->NodeGraph == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing graph source for size over lifetime module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeOutput* ParticleUpdateOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
		ENiagaraScriptUsage::ParticleUpdateScript,
		EmitterData->UpdateScriptProps.Script != nullptr ? EmitterData->UpdateScriptProps.Script->GetUsageId() : FGuid());
	if (ParticleUpdateOutputNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing Particle Update output node for size over lifetime module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeFunctionCall* SizeNode = Unity2NiagaraImporter::AddScriptModuleFromPath(
		TEXT("/Niagara/Modules/Update/Size/ScaleSpriteSize.ScaleSpriteSize"),
		*ParticleUpdateOutputNode);
	if (SizeNode == nullptr)
	{
		return;
	}

	EmitterData->InvalidateCompileResults();

	const FUnityMinMaxCurveData& SizeCurve = ParticleSystem.SizeOverLifetime.Size;
	const float FirstScale = SizeCurve.GetFirstKeyValue(SizeCurve.GetVisualValue(1.0f));
	const float LastScale = SizeCurve.GetLastKeyValue(SizeCurve.GetVisualValue(1.0f));
	const float PeakScale = Unity2NiagaraImporter::GetUnityCurvePeakValue(SizeCurve, SizeCurve.GetVisualValue(1.0f));
	const float RelativeEndScale = FirstScale > KINDA_SMALL_NUMBER ? PeakScale / FirstScale : PeakScale;
	const FVector2f FirstScale2D = Unity2NiagaraImporter::EvaluateUnitySizeOverLifetimeAtTime(ParticleSystem.SizeOverLifetime, 0.0f);
	const FVector2f PeakScale2D = Unity2NiagaraImporter::GetUnityPeakSizeOverLifetimeScale2D(ParticleSystem);
	const FVector2f RelativePeakScale2D(
		FirstScale2D.X > KINDA_SMALL_NUMBER ? PeakScale2D.X / FirstScale2D.X : PeakScale2D.X,
		FirstScale2D.Y > KINDA_SMALL_NUMBER ? PeakScale2D.Y / FirstScale2D.Y : PeakScale2D.Y);
	const FString SizeSamples = Unity2NiagaraImporter::FormatUnitySizeOverLifetimeSamples(ParticleSystem);
	int32 ChangedSizeParameterCount = 0;
	FString InputPinSummary;
	UNiagaraScript* ParticleUpdateScript = EmitterData->UpdateScriptProps.Script;
	if (ParticleUpdateScript != nullptr)
	{
		const FString UniqueEmitterName = Emitter.GetUniqueEmitterName();
		for (UEdGraphPin* Pin : SizeNode->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			const FString PinName = Pin->PinName.ToString();
			if (PinName.Equals(TEXT("InputMap"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (!InputPinSummary.IsEmpty())
			{
				InputPinSummary += TEXT(" | ");
			}
			InputPinSummary += FString::Printf(TEXT("%s:%s"), *PinName, *PinType.GetName());

			if (PinType == FNiagaraTypeDefinition::GetVec2Def()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Scale"), TEXT("Factor"), TEXT("Size") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleUpdateScript, *SizeNode, Pin->PinName, PinType, RelativePeakScale2D);
				++ChangedSizeParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetFloatDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Scale"), TEXT("Factor"), TEXT("Size") }))
			{
				const float UniformRelativePeakScale = FMath::Max(RelativePeakScale2D.X, RelativePeakScale2D.Y);
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleUpdateScript, *SizeNode, Pin->PinName, PinType, UniformRelativePeakScale);
				++ChangedSizeParameterCount;
			}
		}
	}
	if (InputPinSummary.IsEmpty())
	{
		InputPinSummary = TEXT("<none>");
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Added Size Over Lifetime module: Emitter=%s Changed=%d SeparateAxes=%s Keys=%d First=%.3f Last=%.3f Peak=%.3f RelativePeak=%.3f First2D=%s Peak2D=%s RelativePeak2D=%s Samples=%s InputPins=%s"),
		*Emitter.GetName(),
		ChangedSizeParameterCount,
		ParticleSystem.SizeOverLifetime.bSeparateAxes ? TEXT("true") : TEXT("false"),
		SizeCurve.Keys.Num(),
		FirstScale,
		LastScale,
		PeakScale,
		RelativeEndScale,
		*Unity2NiagaraImporter::FormatVector2Compact(FirstScale2D),
		*Unity2NiagaraImporter::FormatVector2Compact(PeakScale2D),
		*Unity2NiagaraImporter::FormatVector2Compact(RelativePeakScale2D),
		*SizeSamples,
		*InputPinSummary);
}

void FUnity2NiagaraImporter::AddColorOverLifetimeModuleToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	if (!ParticleSystem.ColorOverLifetime.bEnabled)
	{
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (ScriptSource == nullptr || ScriptSource->NodeGraph == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing graph source for color over lifetime module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeOutput* ParticleUpdateOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
		ENiagaraScriptUsage::ParticleUpdateScript,
		EmitterData->UpdateScriptProps.Script != nullptr ? EmitterData->UpdateScriptProps.Script->GetUsageId() : FGuid());
	if (ParticleUpdateOutputNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing Particle Update output node for color over lifetime module: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeFunctionCall* ColorNode = Unity2NiagaraImporter::AddScriptModuleFromPath(
		TEXT("/Niagara/Modules/Update/Color/ScaleColor.ScaleColor"),
		*ParticleUpdateOutputNode);
	if (ColorNode == nullptr)
	{
		return;
	}

	EmitterData->InvalidateCompileResults();

	const FLinearColor StartColor = Unity2NiagaraImporter::GetUnitySpawnColor(ParticleSystem);
	const FLinearColor EndScale = Unity2NiagaraImporter::GetUnityColorOverLifetimeScale(ParticleSystem);
	const FLinearColor PeakColor = Unity2NiagaraImporter::GetUnityPeakLifetimeColor(ParticleSystem);
	const FLinearColor EndColor = Unity2NiagaraImporter::GetUnityEndLifetimeColor(ParticleSystem);
	const FString ColorSamples = Unity2NiagaraImporter::FormatUnityColorOverLifetimeSamples(ParticleSystem);
	int32 ChangedColorParameterCount = 0;
	FString InputPinSummary;
	UNiagaraScript* ParticleUpdateScript = EmitterData->UpdateScriptProps.Script;
	if (ParticleUpdateScript != nullptr)
	{
		const FString UniqueEmitterName = Emitter.GetUniqueEmitterName();
		for (UEdGraphPin* Pin : ColorNode->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			const FString PinName = Pin->PinName.ToString();
			if (PinName.Equals(TEXT("InputMap"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (!InputPinSummary.IsEmpty())
			{
				InputPinSummary += TEXT(" | ");
			}
			InputPinSummary += FString::Printf(TEXT("%s:%s"), *PinName, *PinType.GetName());

			if (PinType == FNiagaraTypeDefinition::GetColorDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Color"), TEXT("Colour"), TEXT("Scale") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleUpdateScript, *ColorNode, Pin->PinName, PinType, EndScale);
				++ChangedColorParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetVec4Def()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Color"), TEXT("Colour"), TEXT("Scale") }))
			{
				const FVector4f ColorScaleValue(EndScale.R, EndScale.G, EndScale.B, EndScale.A);
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleUpdateScript, *ColorNode, Pin->PinName, PinType, ColorScaleValue);
				++ChangedColorParameterCount;
			}
			else if (PinType == FNiagaraTypeDefinition::GetFloatDef()
				&& Unity2NiagaraImporter::NameContainsAny(PinName, { TEXT("Alpha"), TEXT("Opacity") }))
			{
				Unity2NiagaraImporter::SetRapidIterationParameter(UniqueEmitterName, *ParticleUpdateScript, *ColorNode, Pin->PinName, PinType, EndScale.A);
				++ChangedColorParameterCount;
			}
		}
	}
	if (InputPinSummary.IsEmpty())
	{
		InputPinSummary = TEXT("<none>");
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Added Color Over Lifetime module: Emitter=%s Changed=%d Start=%s Peak=%s End=%s EndScale=%s Samples=%s InputPins=%s"),
		*Emitter.GetName(),
		ChangedColorParameterCount,
		*Unity2NiagaraImporter::FormatLinearColorCompact(StartColor),
		*Unity2NiagaraImporter::FormatLinearColorCompact(PeakColor),
		*Unity2NiagaraImporter::FormatLinearColorCompact(EndColor),
		*Unity2NiagaraImporter::FormatLinearColorCompact(EndScale),
		*ColorSamples,
		*InputPinSummary);
}

void FUnity2NiagaraImporter::ApplyInitialNiagaraParametersToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	const int32 BurstCount = Unity2NiagaraImporter::GetUnityBurstCountEstimate(ParticleSystem);
	const float Lifetime = Unity2NiagaraImporter::GetUnityVisualLifetime(ParticleSystem, 1.0f, TEXT("StartLifetime"));
	const float SpriteSize = Unity2NiagaraImporter::GetUnityInitialSpriteSize(ParticleSystem, 10.0f) * Unity2NiagaraImporter::GetVisualSizeScale(ParticleSystem);
	const float StartSpeed = Unity2NiagaraImporter::GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Main.StartSpeed, 0.0f, TEXT("StartSpeedLog"))
		* Unity2NiagaraImporter::GetVisualSpeedScale(ParticleSystem);
	const FVector3f InitialVelocity = Unity2NiagaraImporter::GetUnityInitialVelocity(ParticleSystem);
	const float StartVelocity = InitialVelocity.Size();
	const float SpriteRotation = Unity2NiagaraImporter::GetUnityInitialSpriteRotationDegrees(ParticleSystem);
	const float ShapeRadius = ParticleSystem.Shape.bEnabled ? Unity2NiagaraImporter::ConvertUnitySizeToNiagaraSpriteSize(Unity2NiagaraImporter::GetApproximateShapeRadius(ParticleSystem)) : 0.0f;
	const float ShapeAngle = ParticleSystem.Shape.Angle;
	const float ShapeArc = ParticleSystem.Shape.Arc;
	const float ShapeArcSpeed = ParticleSystem.Shape.ArcSpeed.GetRepresentativeValue(0.0f);
	const FVector3f NoiseVelocity = Unity2NiagaraImporter::GetUnityNoiseVelocityOffset(ParticleSystem);
	const float LimitVelocity = ParticleSystem.LimitVelocityOverLifetime.bEnabled
		? Unity2NiagaraImporter::ConvertUnitySpeedToNiagaraVelocity(Unity2NiagaraImporter::GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.LimitVelocityOverLifetime.Limit, 0.0f, TEXT("LimitVelocityLog")))
		: 0.0f;
	const FUnityMinMaxCurveData& SizeOverLifetimeCurve = ParticleSystem.SizeOverLifetime.Size;
	const float SizeOverLifetimeFirstScale = SizeOverLifetimeCurve.GetFirstKeyValue(SizeOverLifetimeCurve.GetVisualValue(1.0f));
	const float SizeOverLifetimeLastScale = Unity2NiagaraImporter::GetUnityCurvePeakValue(SizeOverLifetimeCurve, SizeOverLifetimeCurve.GetVisualValue(1.0f));
	const float SizeOverLifetimeRelativeEndScale = SizeOverLifetimeFirstScale > KINDA_SMALL_NUMBER
		? SizeOverLifetimeLastScale / SizeOverLifetimeFirstScale
		: SizeOverLifetimeLastScale;
	const FVector2f SizeOverLifetimePeakScale2D = Unity2NiagaraImporter::GetUnityPeakSizeOverLifetimeScale2D(ParticleSystem);
	const FString SizeOverLifetimeSamples = Unity2NiagaraImporter::FormatUnitySizeOverLifetimeSamples(ParticleSystem);
	const FLinearColor ColorOverLifetimeScale = Unity2NiagaraImporter::GetUnityColorOverLifetimeScale(ParticleSystem);
	const bool bHasTextureSheetAnimation = ParticleSystem.TextureSheetAnimation.bEnabled;
	const int32 TextureSheetTotalFrames = bHasTextureSheetAnimation
		? Unity2NiagaraImporter::GetUnityTextureSheetTotalFrames(ParticleSystem.TextureSheetAnimation)
		: 1;
	const float TextureSheetStartFrame = bHasTextureSheetAnimation
		? Unity2NiagaraImporter::ClampUnitySubImageIndex(ParticleSystem.TextureSheetAnimation.StartFrame.GetRepresentativeValue(0.0f), ParticleSystem.TextureSheetAnimation)
		: 0.0f;
	const float TextureSheetFrame = bHasTextureSheetAnimation
		? Unity2NiagaraImporter::EvaluateUnityTextureSheetFrameAtTime(ParticleSystem.TextureSheetAnimation, 1.0f)
		: 0.0f;
	const float TextureSheetCycleCount = bHasTextureSheetAnimation ? static_cast<float>(ParticleSystem.TextureSheetAnimation.CycleCount) : 1.0f;
	const FString TextureSheetSamples = Unity2NiagaraImporter::FormatUnityTextureSheetSamples(ParticleSystem);
	const float SpawnRate = Unity2NiagaraImporter::GetUnitySpawnRate(ParticleSystem);
	const FLinearColor ParticleColor = Unity2NiagaraImporter::GetUnitySpawnColor(ParticleSystem);
	const FLinearColor ColorOverLifetimePeak = Unity2NiagaraImporter::GetUnityPeakLifetimeColor(ParticleSystem);
	const FLinearColor ColorOverLifetimeEnd = Unity2NiagaraImporter::GetUnityEndLifetimeColor(ParticleSystem);
	const FString ColorOverLifetimeSamples = Unity2NiagaraImporter::FormatUnityColorOverLifetimeSamples(ParticleSystem);

	TArray<UNiagaraScript*> Scripts;
	EmitterData->GetScripts(Scripts, false, false);

	int32 ChangedParameterCount = 0;
	int32 ChangedColorOverLifetimeParameterCount = 0;
	int32 ChangedSizeOverLifetimeParameterCount = 0;
	int32 ChangedTextureSheetParameterCount = 0;
	for (UNiagaraScript* Script : Scripts)
	{
		if (Script == nullptr)
		{
			continue;
		}

		const FString ScriptNameString = Script->GetName();
		const bool bIsScaleSpriteSizeScript = ScriptNameString.Contains(TEXT("ScaleSpriteSize"), ESearchCase::IgnoreCase)
			|| ScriptNameString.Contains(TEXT("Scale Sprite Size"), ESearchCase::IgnoreCase);
		const bool bIsScaleColorScript = ScriptNameString.Contains(TEXT("ScaleColor"), ESearchCase::IgnoreCase)
			|| ScriptNameString.Contains(TEXT("Scale Color"), ESearchCase::IgnoreCase);
		const bool bIsSubUVAnimationScript = ScriptNameString.Contains(TEXT("SubUV"), ESearchCase::IgnoreCase)
			|| ScriptNameString.Contains(TEXT("Sub UV"), ESearchCase::IgnoreCase);
		FNiagaraParameterStore& RapidIterationParameters = Script->RapidIterationParameters;
		const TArrayView<const FNiagaraVariableWithOffset> Parameters = RapidIterationParameters.ReadParameterVariables();
		UE_LOG(
			LogTemp,
			Display,
			TEXT("[JsonToNiagara] Rapid Params: Emitter=%s Script=%s Usage=%d Count=%d"),
			*Emitter.GetName(),
			*Script->GetName(),
			static_cast<int32>(Script->GetUsage()),
			Parameters.Num());

		for (const FNiagaraVariableWithOffset& ParameterWithOffset : Parameters)
		{
			const FName ParameterName = ParameterWithOffset.GetName();
			const FString ParameterNameString = ParameterName.ToString();
			const FNiagaraTypeDefinition& ParameterType = ParameterWithOffset.GetType();
			const bool bIsFloat = ParameterType == FNiagaraTypeDefinition::GetFloatDef();
			const bool bIsInt = ParameterType == FNiagaraTypeDefinition::GetIntDef();
			const bool bIsColor = ParameterType == FNiagaraTypeDefinition::GetColorDef();
			const bool bIsVec2 = ParameterType == FNiagaraTypeDefinition::GetVec2Def();
			const bool bIsVec3 = ParameterType == FNiagaraTypeDefinition::GetVec3Def();
			const bool bIsVec4 = ParameterType == FNiagaraTypeDefinition::GetVec4Def();

			bool bChanged = false;
			if (bIsFloat && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("SpawnRate"), TEXT("Spawn Rate") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(SpawnRate, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (bIsFloat && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Lifetime"), TEXT("Life Time") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(Lifetime, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (ParticleSystem.SizeOverLifetime.bEnabled && bIsFloat && bIsScaleSpriteSizeScript && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Scale"), TEXT("Factor"), TEXT("Size") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(SizeOverLifetimeRelativeEndScale, FNiagaraVariable(ParameterType, ParameterName));
				if (bChanged)
				{
					++ChangedSizeOverLifetimeParameterCount;
				}
			}
			else if (ParticleSystem.SizeOverLifetime.bEnabled && bIsVec2 && bIsScaleSpriteSizeScript && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Scale"), TEXT("Factor"), TEXT("Size") }))
			{
				const FVector2f SizeScaleValue(SizeOverLifetimeRelativeEndScale, SizeOverLifetimeRelativeEndScale);
				bChanged = RapidIterationParameters.SetParameterValue(SizeScaleValue, FNiagaraVariable(ParameterType, ParameterName));
				if (bChanged)
				{
					++ChangedSizeOverLifetimeParameterCount;
				}
			}
			else if (ParticleSystem.ColorOverLifetime.bEnabled && bIsColor && bIsScaleColorScript && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Color"), TEXT("Colour"), TEXT("Scale") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(ColorOverLifetimeScale, FNiagaraVariable(ParameterType, ParameterName));
				if (bChanged)
				{
					++ChangedColorOverLifetimeParameterCount;
				}
			}
			else if (ParticleSystem.ColorOverLifetime.bEnabled && bIsVec4 && bIsScaleColorScript && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Color"), TEXT("Colour"), TEXT("Scale") }))
			{
				const FVector4f ColorScaleValue(ColorOverLifetimeScale.R, ColorOverLifetimeScale.G, ColorOverLifetimeScale.B, ColorOverLifetimeScale.A);
				bChanged = RapidIterationParameters.SetParameterValue(ColorScaleValue, FNiagaraVariable(ParameterType, ParameterName));
				if (bChanged)
				{
					++ChangedColorOverLifetimeParameterCount;
				}
			}
			else if (ParticleSystem.ColorOverLifetime.bEnabled && bIsFloat && bIsScaleColorScript && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Alpha") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(ColorOverLifetimeScale.A, FNiagaraVariable(ParameterType, ParameterName));
				if (bChanged)
				{
					++ChangedColorOverLifetimeParameterCount;
				}
			}
			else if (bHasTextureSheetAnimation && bIsFloat && bIsSubUVAnimationScript && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Start"), TEXT("First") }) && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Frame"), TEXT("Image"), TEXT("Index") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(TextureSheetStartFrame, FNiagaraVariable(ParameterType, ParameterName));
				if (bChanged)
				{
					++ChangedTextureSheetParameterCount;
				}
			}
			else if (bHasTextureSheetAnimation && bIsFloat && bIsSubUVAnimationScript && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Frame"), TEXT("Image"), TEXT("Index") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(TextureSheetFrame, FNiagaraVariable(ParameterType, ParameterName));
				if (bChanged)
				{
					++ChangedTextureSheetParameterCount;
				}
			}
			else if (bHasTextureSheetAnimation && bIsFloat && bIsSubUVAnimationScript && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Cycle"), TEXT("Loop") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(TextureSheetCycleCount, FNiagaraVariable(ParameterType, ParameterName));
				if (bChanged)
				{
					++ChangedTextureSheetParameterCount;
				}
			}
			else if (bHasTextureSheetAnimation && bIsInt && bIsSubUVAnimationScript && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Frame"), TEXT("Image"), TEXT("Count"), TEXT("Num") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(TextureSheetTotalFrames, FNiagaraVariable(ParameterType, ParameterName));
				if (bChanged)
				{
					++ChangedTextureSheetParameterCount;
				}
			}
			else if (bIsFloat && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Sprite Size"), TEXT("SpriteSize"), TEXT("Size") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(SpriteSize, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (bIsVec2 && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Sprite Size"), TEXT("SpriteSize"), TEXT("Size") }))
			{
				const FVector2f SpriteSizeValue(SpriteSize, SpriteSize);
				bChanged = RapidIterationParameters.SetParameterValue(SpriteSizeValue, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (ParticleSystem.Shape.bEnabled && bIsFloat && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Radius") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(ShapeRadius, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (ParticleSystem.Shape.bEnabled && bIsFloat && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Angle") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(ShapeAngle, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (ParticleSystem.Shape.bEnabled && bIsFloat && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Arc Speed"), TEXT("ArcSpeed") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(ShapeArcSpeed, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (ParticleSystem.Shape.bEnabled && bIsFloat && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Arc") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(ShapeArc, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (bIsFloat && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Velocity"), TEXT("Speed") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(StartVelocity, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (bIsVec3 && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Velocity") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(InitialVelocity, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (bIsFloat && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Rotation"), TEXT("Angle") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(SpriteRotation, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (bIsColor && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Color"), TEXT("Colour") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(ParticleColor, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (bIsInt && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Spawn Count"), TEXT("SpawnCount"), TEXT("Burst Count"), TEXT("BurstCount"), TEXT("Count") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(BurstCount, FNiagaraVariable(ParameterType, ParameterName));
			}
			else if (bIsFloat && Unity2NiagaraImporter::NameContainsAny(ParameterNameString, { TEXT("Spawn Count"), TEXT("SpawnCount"), TEXT("Burst Count"), TEXT("BurstCount"), TEXT("Count") }))
			{
				bChanged = RapidIterationParameters.SetParameterValue(static_cast<float>(BurstCount), FNiagaraVariable(ParameterType, ParameterName));
			}

			if (bChanged)
			{
				++ChangedParameterCount;
				UE_LOG(
					LogTemp,
					Display,
					TEXT("[JsonToNiagara] Set Rapid Param: Emitter=%s Param=%s Type=%s"),
					*Emitter.GetName(),
					*ParameterNameString,
					*ParameterType.GetName());
			}
			else
			{
				UE_LOG(
					LogTemp,
					Display,
					TEXT("[JsonToNiagara] Rapid Param Candidate: Emitter=%s Param=%s Type=%s"),
					*Emitter.GetName(),
					*ParameterNameString,
					*ParameterType.GetName());
			}
		}
	}

	if (ParticleSystem.ColorOverLifetime.bEnabled && ChangedColorOverLifetimeParameterCount == 0)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("[JsonToNiagara] Color Over Lifetime module added but no rapid color params matched: Emitter=%s"),
			*Emitter.GetName());
	}
	if (bHasTextureSheetAnimation && ChangedTextureSheetParameterCount == 0)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("[JsonToNiagara] Texture Sheet Animation module added but no rapid SubUV params matched: Emitter=%s"),
			*Emitter.GetName());
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Applied Niagara Params: Emitter=%s Changed=%d SizeLifeChanged=%d ColorLifeChanged=%d TextureSheetChanged=%d Burst=%d SpawnRate=%.3f Lifetime=%.3f Size=%.3f Speed=%.3f Velocity=%.3f RotationDeg=%.3f Shape=%s ShapeRadius=%.3f Noise=%s NoiseVelocity=(%.3f, %.3f, %.3f) LimitVelocity=%s Limit=%.3f Trails=%s TrailRatio=%.3f TrailLifetime=%.3f SizeLifeScale=%.3f SizePeak2D=%s SizeSamples=%s ColorLifeScale=%s ColorPeak=%s ColorEnd=%s ColorSamples=%s SubUV(Start=%.3f Frame=%.3f Total=%d Cycles=%.3f Samples=%s)"),
		*Emitter.GetName(),
		ChangedParameterCount,
		ChangedSizeOverLifetimeParameterCount,
		ChangedColorOverLifetimeParameterCount,
		ChangedTextureSheetParameterCount,
		BurstCount,
		SpawnRate,
		Lifetime,
		SpriteSize,
		StartSpeed,
		StartVelocity,
		SpriteRotation,
		*ParticleSystem.Shape.ShapeType,
		ShapeRadius,
		ParticleSystem.Noise.bEnabled ? TEXT("true") : TEXT("false"),
		NoiseVelocity.X,
		NoiseVelocity.Y,
		NoiseVelocity.Z,
		ParticleSystem.LimitVelocityOverLifetime.bEnabled ? TEXT("true") : TEXT("false"),
		LimitVelocity,
		ParticleSystem.Trails.bEnabled ? TEXT("true") : TEXT("false"),
		ParticleSystem.Trails.Ratio,
		ParticleSystem.Trails.Lifetime,
		SizeOverLifetimeRelativeEndScale,
		*Unity2NiagaraImporter::FormatVector2Compact(SizeOverLifetimePeakScale2D),
		*SizeOverLifetimeSamples,
		*Unity2NiagaraImporter::FormatLinearColorCompact(ColorOverLifetimeScale),
		*Unity2NiagaraImporter::FormatLinearColorCompact(ColorOverLifetimePeak),
		*Unity2NiagaraImporter::FormatLinearColorCompact(ColorOverLifetimeEnd),
		*ColorOverLifetimeSamples,
		TextureSheetStartFrame,
		TextureSheetFrame,
		TextureSheetTotalFrames,
		TextureSheetCycleCount,
		*TextureSheetSamples);
}

void FUnity2NiagaraImporter::ApplyParticleSpawnSetParametersToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (ScriptSource == nullptr || ScriptSource->NodeGraph == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing graph source for Niagara emitter: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeOutput* ParticleSpawnOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
		ENiagaraScriptUsage::ParticleSpawnScript,
		EmitterData->SpawnScriptProps.Script->GetUsageId());
	if (ParticleSpawnOutputNode == nullptr)
	{
		ParticleSpawnOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
			ENiagaraScriptUsage::ParticleSpawnScriptInterpolated,
			EmitterData->SpawnScriptProps.Script->GetUsageId());
	}
	if (ParticleSpawnOutputNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing Particle Spawn output node for Niagara emitter: %s"), *Emitter.GetName());
		return;
	}

	const float Lifetime = Unity2NiagaraImporter::GetUnityVisualLifetime(ParticleSystem, 1.0f, TEXT("SpawnLifetime"));
	const FVector2f UnitySpriteSize = Unity2NiagaraImporter::GetUnityInitialSpriteSize2D(ParticleSystem) * Unity2NiagaraImporter::GetVisualSizeScale(ParticleSystem);
	FVector2f SpriteSize(
		Unity2NiagaraImporter::ConvertUnitySizeToNiagaraSpriteSize(UnitySpriteSize.X),
		Unity2NiagaraImporter::ConvertUnitySizeToNiagaraSpriteSize(UnitySpriteSize.Y));
	if (ParticleSystem.Renderer.RenderMode.Equals(TEXT("Stretch"), ESearchCase::IgnoreCase))
	{
		SpriteSize.Y *= FMath::Max(ParticleSystem.Renderer.LengthScale, 1.0f);
	}
	const float StartSpeed = Unity2NiagaraImporter::GetUnityCurveVisualValue(ParticleSystem, ParticleSystem.Main.StartSpeed, 0.0f, TEXT("SpawnStartSpeed"));
	FVector3f InitialVelocity = Unity2NiagaraImporter::GetUnityInitialVelocity(ParticleSystem);
	const bool bUseShapeVelocityModule = Unity2NiagaraImporter::ShouldUseShapeVelocityModule(ParticleSystem);
	if (ParticleSystem.Renderer.RenderMode.Equals(TEXT("Stretch"), ESearchCase::IgnoreCase) && InitialVelocity.SizeSquared() <= KINDA_SMALL_NUMBER)
	{
		InitialVelocity = Unity2NiagaraImporter::GetDeterministicShapeDirection(ParticleSystem) * 100.0f;
	}
	if (ParticleSystem.Renderer.RenderMode.Equals(TEXT("Stretch"), ESearchCase::IgnoreCase))
	{
		SpriteSize.Y += InitialVelocity.Size() * FMath::Max(ParticleSystem.Renderer.VelocityScale, 0.0f);
	}
	const bool bSetInitialVelocity = !bUseShapeVelocityModule
		&& (InitialVelocity.SizeSquared() > KINDA_SMALL_NUMBER || ParticleSystem.Renderer.RenderMode.Equals(TEXT("Stretch"), ESearchCase::IgnoreCase));
	FVector3f SpriteAlignment = InitialVelocity.SizeSquared() > KINDA_SMALL_NUMBER
		? InitialVelocity.GetSafeNormal()
		: Unity2NiagaraImporter::GetDeterministicShapeDirection(ParticleSystem);
	const float SpriteRotation = Unity2NiagaraImporter::GetUnityInitialSpriteRotationDegrees(ParticleSystem);
	const bool bSetSpriteRotation = !Unity2NiagaraImporter::HasRandomStartRotation(ParticleSystem);
	const FVector2f PivotOffset(
		FMath::Clamp(0.5f + ParticleSystem.Renderer.Pivot.X, 0.0f, 1.0f),
		FMath::Clamp(0.5f + ParticleSystem.Renderer.Pivot.Y, 0.0f, 1.0f));
	const FVector3f InitialPosition = ParticleSystem.Shape.bEnabled
		? Unity2NiagaraImporter::ConvertUnityVectorToNiagaraPosition(ParticleSystem.Shape.Position)
		: FVector3f::ZeroVector;
	const bool bSetInitialPosition = !ParticleSystem.Shape.bEnabled;
	const bool bHasTextureSheetAnimation = ParticleSystem.TextureSheetAnimation.bEnabled;
	const float StartSubImageIndex = bHasTextureSheetAnimation
		? Unity2NiagaraImporter::ClampUnitySubImageIndex(ParticleSystem.TextureSheetAnimation.StartFrame.GetRepresentativeValue(0.0f), ParticleSystem.TextureSheetAnimation)
		: 0.0f;
	const FLinearColor ParticleColor = Unity2NiagaraImporter::GetUnitySpawnColor(ParticleSystem);

	TArray<FNiagaraVariable> Variables;
	Variables.Add(SYS_PARAM_PARTICLES_SPRITE_SIZE);
	Variables.Add(SYS_PARAM_PARTICLES_LIFETIME);
	Variables.Add(SYS_PARAM_PARTICLES_COLOR);
	Variables.Add(SYS_PARAM_PARTICLES_PIVOT_OFFSET);
	Variables.Add(SYS_PARAM_PARTICLES_SPRITE_ALIGNMENT);
	if (bSetSpriteRotation)
	{
		Variables.Add(SYS_PARAM_PARTICLES_SPRITE_ROTATION);
	}
	if (bSetInitialPosition)
	{
		Variables.Add(SYS_PARAM_PARTICLES_POSITION);
	}
	if (bSetInitialVelocity)
	{
		Variables.Add(SYS_PARAM_PARTICLES_VELOCITY);
	}
	if (bHasTextureSheetAnimation)
	{
		Variables.Add(SYS_PARAM_PARTICLES_SUB_IMAGE_INDEX);
	}
	if (ParticleSystem.Trails.bEnabled)
	{
		Variables.Add(SYS_PARAM_PARTICLES_RIBBONID);
		Variables.Add(SYS_PARAM_PARTICLES_RIBBONWIDTH);
		Variables.Add(SYS_PARAM_PARTICLES_RIBBONFACING);
	}

	TArray<FString> Defaults;
	Defaults.Add(FString::Printf(TEXT("X=%.3f Y=%.3f"), SpriteSize.X, SpriteSize.Y));
	Defaults.Add(FString::SanitizeFloat(Lifetime));
	Defaults.Add(ParticleColor.ToString());
	Defaults.Add(FString::Printf(TEXT("X=%.3f Y=%.3f"), PivotOffset.X, PivotOffset.Y));
	Defaults.Add(FString::Printf(TEXT("X=%.3f Y=%.3f Z=%.3f"), SpriteAlignment.X, SpriteAlignment.Y, SpriteAlignment.Z));
	if (bSetSpriteRotation)
	{
		Defaults.Add(FString::SanitizeFloat(SpriteRotation));
	}
	if (bSetInitialPosition)
	{
		Defaults.Add(FString::Printf(TEXT("X=%.3f Y=%.3f Z=%.3f"), InitialPosition.X, InitialPosition.Y, InitialPosition.Z));
	}
	if (bSetInitialVelocity)
	{
		Defaults.Add(FString::Printf(TEXT("X=%.3f Y=%.3f Z=%.3f"), InitialVelocity.X, InitialVelocity.Y, InitialVelocity.Z));
	}
	if (bHasTextureSheetAnimation)
	{
		Defaults.Add(FString::SanitizeFloat(StartSubImageIndex));
	}
	if (ParticleSystem.Trails.bEnabled)
	{
		const float TrailWidth = FMath::Max(FMath::Min(SpriteSize.X, SpriteSize.Y) * FMath::Clamp(ParticleSystem.Trails.Ratio, 0.05f, 1.0f), 1.0f);
		const FVector3f RibbonFacing = SpriteAlignment.SizeSquared() > KINDA_SMALL_NUMBER ? SpriteAlignment : FVector3f(0.0f, 0.0f, 1.0f);
		Defaults.Add(TEXT("0"));
		Defaults.Add(FString::SanitizeFloat(TrailWidth));
		Defaults.Add(FString::Printf(TEXT("X=%.3f Y=%.3f Z=%.3f"), RibbonFacing.X, RibbonFacing.Y, RibbonFacing.Z));
	}

	UNiagaraNodeAssignment* AssignmentNode = FNiagaraStackGraphUtilities::AddParameterModuleToStack(
		Variables,
		*ParticleSpawnOutputNode,
		INDEX_NONE,
		Defaults);
	if (AssignmentNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Failed to add Unity Set Parameters module: %s"), *Emitter.GetName());
		return;
	}

	EmitterData->InvalidateCompileResults();

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Added Unity Spawn Set Parameters: Emitter=%s SpriteSize=(%.3f, %.3f) Lifetime=%.3f RotationSet=%s RotationDeg=%.3f Pivot=(%.3f, %.3f) Alignment=(%.3f, %.3f, %.3f) PositionSet=%s Position=(%.3f, %.3f, %.3f) VelocitySet=%s ShapeVelocityModule=%s Velocity=(%.3f, %.3f, %.3f) SubImage=%.3f Color=(%.3f, %.3f, %.3f, %.3f)"),
		*Emitter.GetName(),
		SpriteSize.X,
		SpriteSize.Y,
		Lifetime,
		bSetSpriteRotation ? TEXT("true") : TEXT("false"),
		SpriteRotation,
		PivotOffset.X,
		PivotOffset.Y,
		SpriteAlignment.X,
		SpriteAlignment.Y,
		SpriteAlignment.Z,
		bSetInitialPosition ? TEXT("true") : TEXT("false"),
		InitialPosition.X,
		InitialPosition.Y,
		InitialPosition.Z,
		bSetInitialVelocity ? TEXT("true") : TEXT("false"),
		bUseShapeVelocityModule ? TEXT("true") : TEXT("false"),
		InitialVelocity.X,
		InitialVelocity.Y,
		InitialVelocity.Z,
		StartSubImageIndex,
		ParticleColor.R,
		ParticleColor.G,
		ParticleColor.B,
		ParticleColor.A);
}

void FUnity2NiagaraImporter::ApplyParticleUpdateSetParametersToEmitter(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	const bool bApplySize = false;
	const bool bApplyColor = false;
	const bool bApplySubImage = ParticleSystem.TextureSheetAnimation.bEnabled;
	const bool bApplyRotation = ParticleSystem.RotationOverLifetime.bEnabled;
	const bool bApplyMotion = Unity2NiagaraImporter::HasUnityLifetimeMotion(ParticleSystem)
		&& !Unity2NiagaraImporter::ShouldUseShapeVelocityModule(ParticleSystem);
	if (!bApplySize && !bApplyColor && !bApplySubImage && !bApplyRotation && !bApplyMotion)
	{
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		return;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (ScriptSource == nullptr || ScriptSource->NodeGraph == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing graph source for Unity Update Set Parameters: %s"), *Emitter.GetName());
		return;
	}

	UNiagaraNodeOutput* ParticleUpdateOutputNode = ScriptSource->NodeGraph->FindEquivalentOutputNode(
		ENiagaraScriptUsage::ParticleUpdateScript,
		EmitterData->UpdateScriptProps.Script != nullptr ? EmitterData->UpdateScriptProps.Script->GetUsageId() : FGuid());
	if (ParticleUpdateOutputNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Missing Particle Update output node for Unity Update Set Parameters: %s"), *Emitter.GetName());
		return;
	}

	const FVector2f UpdateUnitySpriteSize = Unity2NiagaraImporter::GetUnityInitialSpriteSize2D(ParticleSystem) * Unity2NiagaraImporter::GetVisualSizeScale(ParticleSystem);
	FVector2f UpdateSpriteSize(
		Unity2NiagaraImporter::ConvertUnitySizeToNiagaraSpriteSize(UpdateUnitySpriteSize.X),
		Unity2NiagaraImporter::ConvertUnitySizeToNiagaraSpriteSize(UpdateUnitySpriteSize.Y));
	if (ParticleSystem.Renderer.RenderMode.Equals(TEXT("Stretch"), ESearchCase::IgnoreCase))
	{
		UpdateSpriteSize.Y *= FMath::Max(ParticleSystem.Renderer.LengthScale, 1.0f);
	}
	const FLinearColor SpawnColor = Unity2NiagaraImporter::GetUnitySpawnColor(ParticleSystem);
	const FLinearColor PeakLifetimeColor = Unity2NiagaraImporter::GetUnityPeakLifetimeColor(ParticleSystem);
	const FLinearColor UpdateColor(
		FMath::Clamp(SpawnColor.R * PeakLifetimeColor.R, 0.0f, 1.0f),
		FMath::Clamp(SpawnColor.G * PeakLifetimeColor.G, 0.0f, 1.0f),
		FMath::Clamp(SpawnColor.B * PeakLifetimeColor.B, 0.0f, 1.0f),
		FMath::Clamp(SpawnColor.A * FMath::Max(PeakLifetimeColor.A, 0.01f), 0.0f, 1.0f));
	const float Lifetime = Unity2NiagaraImporter::GetUnityVisualLifetime(ParticleSystem, 1.0f, TEXT("UpdateLifetime"));
	const float RotationRateDegrees = Unity2NiagaraImporter::GetUnityRotationOverLifetimeDegreesPerSecond(ParticleSystem);
	const float UpdateSpriteRotation = Unity2NiagaraImporter::GetUnityInitialSpriteRotationDegrees(ParticleSystem) + RotationRateDegrees * Lifetime;
	const FVector3f UpdateVelocity = Unity2NiagaraImporter::GetUnityLifetimeMotionVelocity(ParticleSystem);
	const FVector3f EndVelocityOverLifetime = Unity2NiagaraImporter::GetUnityVelocityOverLifetimeValue(ParticleSystem, 1.0f);
	const FVector3f EndForceOverLifetime = Unity2NiagaraImporter::GetUnityForceOverLifetimeValue(ParticleSystem, 1.0f);
	const float GravityModifier = Unity2NiagaraImporter::GetUnityGravityModifierValue(ParticleSystem);
	const float UpdateSubImage = Unity2NiagaraImporter::ClampUnitySubImageIndex(
		ParticleSystem.TextureSheetAnimation.FrameOverTime.GetRepresentativeValue(
			ParticleSystem.TextureSheetAnimation.StartFrame.GetRepresentativeValue(0.0f)),
		ParticleSystem.TextureSheetAnimation);

	TArray<FNiagaraVariable> Variables;
	TArray<FString> Defaults;

	if (bApplySize)
	{
		Variables.Add(SYS_PARAM_PARTICLES_SPRITE_SIZE);
		Defaults.Add(FString::Printf(TEXT("X=%.3f Y=%.3f"), UpdateSpriteSize.X, UpdateSpriteSize.Y));
	}

	if (bApplyColor)
	{
		Variables.Add(SYS_PARAM_PARTICLES_COLOR);
		Defaults.Add(UpdateColor.ToString());
	}

	if (bApplySubImage)
	{
		Variables.Add(SYS_PARAM_PARTICLES_SUB_IMAGE_INDEX);
		Defaults.Add(FString::SanitizeFloat(UpdateSubImage));
	}

	if (bApplyRotation)
	{
		Variables.Add(SYS_PARAM_PARTICLES_SPRITE_ROTATION);
		Defaults.Add(FString::SanitizeFloat(UpdateSpriteRotation));
	}

	if (bApplyMotion)
	{
		Variables.Add(SYS_PARAM_PARTICLES_VELOCITY);
		Defaults.Add(FString::Printf(TEXT("X=%.3f Y=%.3f Z=%.3f"), UpdateVelocity.X, UpdateVelocity.Y, UpdateVelocity.Z));
	}

	UNiagaraNodeAssignment* AssignmentNode = FNiagaraStackGraphUtilities::AddParameterModuleToStack(
		Variables,
		*ParticleUpdateOutputNode,
		INDEX_NONE,
		Defaults);
	if (AssignmentNode == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Failed to add Unity Update Set Parameters module: %s"), *Emitter.GetName());
		return;
	}

	EmitterData->InvalidateCompileResults();

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Added Unity Update Set Parameters: Emitter=%s SizeSet=%s SpriteSize=(%.3f, %.3f) ColorSet=%s Color=(%.3f, %.3f, %.3f, %.3f) SubImageSet=%s SubImage=%.3f RotationSet=%s RotationDeg=%.3f RotationRateDeg=%.3f MotionSet=%s Velocity=(%.3f, %.3f, %.3f) VelocityLife=(%.3f, %.3f, %.3f) ForceLife=(%.3f, %.3f, %.3f) Gravity=%.3f"),
		*Emitter.GetName(),
		bApplySize ? TEXT("true") : TEXT("false"),
		UpdateSpriteSize.X,
		UpdateSpriteSize.Y,
		bApplyColor ? TEXT("true") : TEXT("false"),
		UpdateColor.R,
		UpdateColor.G,
		UpdateColor.B,
		UpdateColor.A,
		bApplySubImage ? TEXT("true") : TEXT("false"),
		UpdateSubImage,
		bApplyRotation ? TEXT("true") : TEXT("false"),
		UpdateSpriteRotation,
		RotationRateDegrees,
		bApplyMotion ? TEXT("true") : TEXT("false"),
		UpdateVelocity.X,
		UpdateVelocity.Y,
		UpdateVelocity.Z,
		EndVelocityOverLifetime.X,
		EndVelocityOverLifetime.Y,
		EndVelocityOverLifetime.Z,
		EndForceOverLifetime.X,
		EndForceOverLifetime.Y,
		EndForceOverLifetime.Z,
		GravityModifier);
}

void FUnity2NiagaraImporter::LogEmitterVisibilityDiagnostics(UNiagaraEmitter& Emitter, const FUnityParticleSystemData& ParticleSystem)
{
	FVersionedNiagaraEmitterData* EmitterData = Emitter.GetLatestEmitterData();
	if (EmitterData == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] Visibility Diagnostics: Emitter=%s MissingEmitterData"), *Emitter.GetName());
		return;
	}

	int32 RendererCount = 0;
	int32 SpriteRendererCount = 0;
	int32 SpriteRendererWithMaterialCount = 0;
	EmitterData->ForEachRenderer([&RendererCount, &SpriteRendererCount, &SpriteRendererWithMaterialCount](UNiagaraRendererProperties* Renderer)
	{
		++RendererCount;
		const UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderer);
		if (SpriteRenderer != nullptr)
		{
			++SpriteRendererCount;
			if (SpriteRenderer->Material != nullptr)
			{
				++SpriteRendererWithMaterialCount;
			}
		}
	});

	const int32 BurstCount = Unity2NiagaraImporter::GetUnityBurstCountEstimate(ParticleSystem);
	const float Lifetime = Unity2NiagaraImporter::GetUnityVisualLifetime(ParticleSystem, 1.0f, TEXT("VisibilityLifetime"));
	const float SpriteSizeCm = Unity2NiagaraImporter::ConvertUnitySizeToNiagaraSpriteSize(
		Unity2NiagaraImporter::GetUnityInitialSpriteSize(ParticleSystem) * Unity2NiagaraImporter::GetVisualSizeScale(ParticleSystem));
	const bool bHasUpdateApproximation = ParticleSystem.SizeOverLifetime.bEnabled
		|| ParticleSystem.ColorOverLifetime.bEnabled
		|| ParticleSystem.TextureSheetAnimation.bEnabled
		|| ParticleSystem.RotationOverLifetime.bEnabled
		|| Unity2NiagaraImporter::HasUnityLifetimeMotion(ParticleSystem);
	const bool bLikelyVisible = SpriteRendererCount > 0
		&& SpriteRendererWithMaterialCount > 0
		&& (BurstCount > 0 || ParticleSystem.Main.bLoop)
		&& Lifetime > 0.0f
		&& SpriteSizeCm > 0.0f;

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] Visibility Diagnostics: Emitter=%s LikelyVisible=%s Renderers=%d SpriteRenderers=%d SpriteMaterials=%d Burst=%d Lifetime=%.3f SpriteSizeCm=%.3f UpdateApprox=%s MotionApprox=%s Noise=%s LimitVelocity=%s Trails=%s LocalSpace=%s PreAllocation=%d FixedBounds=%s"),
		*Emitter.GetName(),
		bLikelyVisible ? TEXT("true") : TEXT("false"),
		RendererCount,
		SpriteRendererCount,
		SpriteRendererWithMaterialCount,
		BurstCount,
		Lifetime,
		SpriteSizeCm,
		bHasUpdateApproximation ? TEXT("true") : TEXT("false"),
		Unity2NiagaraImporter::HasUnityLifetimeMotion(ParticleSystem) ? TEXT("true") : TEXT("false"),
		ParticleSystem.Noise.bEnabled ? TEXT("true") : TEXT("false"),
		ParticleSystem.LimitVelocityOverLifetime.bEnabled ? TEXT("true") : TEXT("false"),
		ParticleSystem.Trails.bEnabled ? TEXT("true") : TEXT("false"),
		EmitterData->bLocalSpace ? TEXT("true") : TEXT("false"),
		EmitterData->PreAllocationCount,
		EmitterData->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Fixed ? TEXT("true") : TEXT("false"));
}

void FUnity2NiagaraImporter::LogNiagaraSystemDiagnostics(UNiagaraSystem& System)
{
	const bool bReadyToRun = System.IsReadyToRun();
	const bool bNeedsRequestCompile = System.NeedsRequestCompile();
	const TArray<FNiagaraEmitterHandle>& EmitterHandles = System.GetEmitterHandles();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[JsonToNiagara] System Diagnostics: System=%s ReadyToRun=%s NeedsRequestCompile=%s CompileForEdit=%s FixedBounds=%s Emitters=%d"),
		*System.GetPathName(),
		bReadyToRun ? TEXT("true") : TEXT("false"),
		bNeedsRequestCompile ? TEXT("true") : TEXT("false"),
		System.GetCompileForEdit() ? TEXT("true") : TEXT("false"),
		System.bFixedBounds ? TEXT("true") : TEXT("false"),
		EmitterHandles.Num());

	for (const FNiagaraEmitterHandle& EmitterHandle : EmitterHandles)
	{
		const FVersionedNiagaraEmitter EmitterInstance = EmitterHandle.GetInstance();
		UNiagaraEmitter* Emitter = EmitterInstance.Emitter;
		const FString EmitterName = Emitter != nullptr ? Emitter->GetName() : EmitterHandle.GetName().ToString();
		UE_LOG(
			LogTemp,
			Display,
			TEXT("[JsonToNiagara] System Emitter Handle: Handle=%s Enabled=%s AllowedByScalability=%s Emitter=%s"),
			*EmitterHandle.GetName().ToString(),
			EmitterHandle.GetIsEnabled() ? TEXT("true") : TEXT("false"),
			EmitterHandle.IsAllowedByScalability() ? TEXT("true") : TEXT("false"),
			Emitter != nullptr ? *Emitter->GetPathName() : TEXT("<null>"));

		if (Emitter == nullptr)
		{
			continue;
		}

		const FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
		if (EmitterData == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("[JsonToNiagara] System Emitter Diagnostics: Emitter=%s MissingEmitterData"), *EmitterName);
			continue;
		}

		Unity2NiagaraImporter::LogNiagaraScriptDiagnostics(EmitterName, TEXT("EmitterSpawn"), EmitterData->EmitterSpawnScriptProps.Script);
		Unity2NiagaraImporter::LogNiagaraScriptDiagnostics(EmitterName, TEXT("EmitterUpdate"), EmitterData->EmitterUpdateScriptProps.Script);
		Unity2NiagaraImporter::LogNiagaraScriptDiagnostics(EmitterName, TEXT("ParticleSpawn"), EmitterData->SpawnScriptProps.Script);
		Unity2NiagaraImporter::LogNiagaraScriptDiagnostics(EmitterName, TEXT("ParticleUpdate"), EmitterData->UpdateScriptProps.Script);
	}
}

FString FUnity2NiagaraImporter::MakeSafeObjectName(const FString& SourceName)
{
	FString SafeName = SourceName;
	const TCHAR InvalidCharacters[] = TEXT(" .,;:'\"/\\|[]{}()-+=!?@#$%^&*`~");
	for (const TCHAR InvalidCharacter : InvalidCharacters)
	{
		if (InvalidCharacter == TCHAR('\0'))
		{
			break;
		}

		SafeName.ReplaceCharInline(InvalidCharacter, TEXT('_'));
	}

	return SafeName.IsEmpty() ? TEXT("UnityParticleSystem") : SafeName;
}
