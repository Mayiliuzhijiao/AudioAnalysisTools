// Georgy Treshchev 2023.

#include "AudioAnalysisToolsLibrary.h"
#include "AudioAnalysisToolsDefines.h"

#include "Analyzers/CoreFrequencyDomainFeatures.h"
#include "Analyzers/CoreTimeDomainFeatures.h"
#include "Analyzers/BeatDetection.h"
#include "Analyzers/OnsetDetection.h"

#include "Analyzers/FFTAnalyzer.h"

#include "Async/Async.h"
#include "UObject/GCObjectScopeGuard.h"

UAudioAnalysisToolsLibrary::UAudioAnalysisToolsLibrary()
	: FFTConfigured(false)
{
}

void UAudioAnalysisToolsLibrary::BeginDestroy()
{
	if (FFTConfigured)
	{
		FreeFFT();
	}

	Super::BeginDestroy();
}

UAudioAnalysisToolsLibrary* UAudioAnalysisToolsLibrary::CreateAudioAnalysisTools(int32 FrameSize, EAnalysisWindowType WindowType)
{
	UAudioAnalysisToolsLibrary* AudioAnalysisTools = NewObject<UAudioAnalysisToolsLibrary>();

	AudioAnalysisTools->Initialize(FrameSize, WindowType);

	return AudioAnalysisTools;
}

bool UAudioAnalysisToolsLibrary::GetAudioFrameFromSoundWave(UImportedSoundWave* ImportedSoundWave, TArray<float>& AudioFrame)
{
	return GetAudioFrameFromSoundWaveByFrames(ImportedSoundWave, CurrentAudioFrame.Num(), AudioFrame);
}

bool UAudioAnalysisToolsLibrary::GetAudioFrameFromSoundWaveByFrames(UImportedSoundWave* ImportedSoundWave, int32 FrameSize, TArray<float>& AudioFrame)
{
	const int32 StartFrame{ImportedSoundWave ? static_cast<int32>(ImportedSoundWave->GetNumOfPlayedFrames()) : 0};
	const int32 EndFrame{StartFrame + FrameSize};

	return GetAudioFrameFromSoundWaveByFramesCustom(ImportedSoundWave, StartFrame, EndFrame, AudioFrame);
}

bool UAudioAnalysisToolsLibrary::GetAudioFrameFromSoundWaveByFramesCustom(UImportedSoundWave* ImportedSoundWave, int32 StartFrame, int32 EndFrame, TArray<float>& AudioFrame)
{
	if (!(StartFrame >= 0 && StartFrame < EndFrame))
	{
		UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the frame data: start frame is '%d', expected >= '0.0' and < '%d'"), StartFrame, EndFrame);
		return false;
	}

	if (!(EndFrame > 0 && EndFrame > StartFrame))
	{
		UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the frame data: end frame is '%d', expected > '0.0' and < '%d'"), EndFrame, StartFrame);
		return false;
	}

	FScopeLock Lock(&ImportedSoundWave->DataGuard);

	if (static_cast<uint32>(EndFrame) > ImportedSoundWave->GetPCMBuffer().PCMNumOfFrames)
	{
		UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the PCM Data: end frame ('%d') must be less than total number of frames ('%d')"), EndFrame, ImportedSoundWave->GetPCMBuffer().PCMNumOfFrames);
		return false;
	}

	const int32 NumChannels{ImportedSoundWave->NumChannels};

	const float* RetrievedPCMData = ImportedSoundWave->GetPCMBuffer().PCMData.GetView().GetData() + (StartFrame * NumChannels);
	const uint32 RetrievedPCMDataSize = EndFrame - StartFrame;

	if (!RetrievedPCMData)
	{
		UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the PCM Data: retrieved PCM Data is nullptr"));
		return false;
	}

	if (RetrievedPCMDataSize <= 0)
	{
		UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the PCM Data: retrieved PCM Data size is '%d', must be > 0 (%d - %d)"), RetrievedPCMDataSize, EndFrame, StartFrame);
		return false;
	}

	if (RetrievedPCMDataSize > static_cast<uint32>(ImportedSoundWave->GetPCMBuffer().PCMData.GetView().Num()))
	{
		UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the PCM Data: retrieved PCM Data size (%d) must be less than the total size (%d)"), RetrievedPCMDataSize, static_cast<int32>(ImportedSoundWave->GetPCMBuffer().PCMData.GetView().Num()));
		return false;
	}

	AudioFrame = TArray<float>(RetrievedPCMData, RetrievedPCMDataSize);

	return true;
}

bool UAudioAnalysisToolsLibrary::GetAudioFrameFromSoundWaveByTime(UImportedSoundWave* ImportedSoundWave, float TimeLength, TArray<float>& AudioFrame)
{
	const float StartTime{ImportedSoundWave ? ImportedSoundWave->GetPlaybackTime() : 0.f};
	const float EndTime{StartTime + TimeLength};

	return GetAudioFrameFromSoundWaveByTimeCustom(ImportedSoundWave, StartTime, EndTime, AudioFrame);
}

bool UAudioAnalysisToolsLibrary::GetAudioFrameFromSoundWaveByTimeCustom(UImportedSoundWave* ImportedSoundWave, float StartTime, float EndTime, TArray<float>& AudioFrame)
{
	if (!ImportedSoundWave)
	{
		UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the audio frame: the specified sound wave is nullptr"));
		return false;
	}

	int32 StartFrame, EndFrame;
	{
		FScopeLock Lock(&ImportedSoundWave->DataGuard);

		if (!ImportedSoundWave->GetPCMBuffer().IsValid())
		{
			UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the audio frame: PCM buffer is invalid"));
			return false;
		}

		if (ImportedSoundWave->GetPCMBuffer().PCMData.GetView().Num() <= 0)
		{
			UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the frame data: PCM Data Size is '%lld', expected > '0'"), static_cast<int64>(ImportedSoundWave->GetPCMBuffer().PCMData.GetView().Num()));
			return false;
		}

		if (ImportedSoundWave->GetPCMBuffer().PCMNumOfFrames <= 0)
		{
			UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the frame data: PCM Num Of Frames is '%d', expected more than '0'"), ImportedSoundWave->GetPCMBuffer().PCMNumOfFrames);
			return false;
		}

		StartTime -= ImportedSoundWave->GetDurationOffset_Internal();
		EndTime -= ImportedSoundWave->GetDurationOffset_Internal();

		if (!(StartTime >= 0 && StartTime < EndTime))
		{
			UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the frame data: start time is '%f', expected >= '0.0' and < '%f'"), StartTime, EndTime);
			return false;
		}

		if (!(EndTime > 0 && EndTime > StartTime))
		{
			UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the frame data: end time is '%f', expected > '0.0' and < '%f'"), EndTime, StartTime);
			return false;
		}

		const float Duration{ImportedSoundWave->GetDurationConst_Internal()};

		if (EndTime > Duration)
		{
			UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to get the PCM Data: end time (%f) must be less than the sound wave duration (%f)"), EndTime, Duration);
			return false;
		}

		const int32 SamplingRate{ImportedSoundWave->GetSampleRate()};

		StartFrame = StartTime * SamplingRate;
		EndFrame = EndTime * SamplingRate;
	}

	return GetAudioFrameFromSoundWaveByFramesCustom(ImportedSoundWave, StartFrame, EndFrame, AudioFrame);
}

void UAudioAnalysisToolsLibrary::Initialize(int32 FrameSize, EAnalysisWindowType InWindowType)
{
	BeatDetectionRef = UBeatDetection::CreateBeatDetection();
	OnsetDetectionRef = UOnsetDetection::CreateOnsetDetection(FrameSize);

	WindowType = InWindowType;

	UpdateFrameSize(FrameSize);
}

const TArray<float>& UAudioAnalysisToolsLibrary::GetMagnitudeSpectrum() const
{
	return MagnitudeSpectrum;
}

const TArray<float>& UAudioAnalysisToolsLibrary::GetFFTReal() const
{
	return FFTReal;
}

const TArray<float>& UAudioAnalysisToolsLibrary::GetFFTImaginary() const
{
	return FFTImaginary;
}

void UAudioAnalysisToolsLibrary::ProcessAudioFrame(const TArray<float>& AudioFrame, bool bProcessToBeatDetection)
{
	if (AudioFrame.Num() != CurrentAudioFrame.Num())
	{
		UpdateFrameSize(AudioFrame.Num());
	}

	CurrentAudioFrame = AudioFrame;

	AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this, bProcessToBeatDetection]()
	{
		FGCObjectScopeGuard Guard(this);
		PerformFFT();

		if (bProcessToBeatDetection)
		{
			BeatDetectionRef->ProcessMagnitude(MagnitudeSpectrum);
		}
	});
}

void UAudioAnalysisToolsLibrary::UpdateFrameSize(int32 FrameSize)
{
	const int32 MagnitudeSpectrumSize{FrameSize / 2};

	CurrentAudioFrame.SetNum(FrameSize);

	WindowFunction = UWindowsLibrary::CreateWindow(FrameSize, WindowType);

	FFTReal.SetNum(FrameSize);
	FFTImaginary.SetNum(FrameSize);
	MagnitudeSpectrum.SetNum(MagnitudeSpectrumSize);

	ConfigureFFT();
}

bool UAudioAnalysisToolsLibrary::IsBeat(int32 Subband) const
{
	return BeatDetectionRef->IsBeat(Subband);
}

bool UAudioAnalysisToolsLibrary::IsKick() const
{
	return BeatDetectionRef->IsKick();
}

bool UAudioAnalysisToolsLibrary::IsSnare() const
{
	return BeatDetectionRef->IsSnare();
}

bool UAudioAnalysisToolsLibrary::IsHiHat() const
{
	return BeatDetectionRef->IsHiHat();
}

bool UAudioAnalysisToolsLibrary::IsBeatRange(int32 Low, int32 High, int32 Threshold) const
{
	return BeatDetectionRef->IsBeatRange(Low, High, Threshold);
}

float UAudioAnalysisToolsLibrary::GetBand(int32 Subband) const
{
	return BeatDetectionRef->GetBand(Subband);
}

float UAudioAnalysisToolsLibrary::GetRootMeanSquare()
{
	return UCoreTimeDomainFeatures::GetRootMeanSquare(CurrentAudioFrame);
}

float UAudioAnalysisToolsLibrary::GetPeakEnergy()
{
	return UCoreTimeDomainFeatures::GetPeakEnergy(CurrentAudioFrame);
}

float UAudioAnalysisToolsLibrary::GetZeroCrossingRate()
{
	return UCoreTimeDomainFeatures::GetZeroCrossingRate(CurrentAudioFrame);
}

float UAudioAnalysisToolsLibrary::GetSpectralCentroid()
{
	return UCoreFrequencyDomainFeatures::GetSpectralCentroid(MagnitudeSpectrum);
}

float UAudioAnalysisToolsLibrary::GetSpectralFlatness()
{
	return UCoreFrequencyDomainFeatures::GetSpectralFlatness(MagnitudeSpectrum);
}

float UAudioAnalysisToolsLibrary::GetSpectralCrest()
{
	return UCoreFrequencyDomainFeatures::GetSpectralCrest(MagnitudeSpectrum);
}

float UAudioAnalysisToolsLibrary::GetSpectralRolloff()
{
	return UCoreFrequencyDomainFeatures::GetSpectralRolloff(MagnitudeSpectrum);
}

float UAudioAnalysisToolsLibrary::GetSpectralKurtosis()
{
	return UCoreFrequencyDomainFeatures::GetSpectralKurtosis(MagnitudeSpectrum);
}

float UAudioAnalysisToolsLibrary::GetEnergyDifference()
{
	return OnsetDetectionRef->GetEnergyDifference(CurrentAudioFrame);
}

float UAudioAnalysisToolsLibrary::GetSpectralDifference()
{
	return OnsetDetectionRef->GetSpectralDifference(MagnitudeSpectrum);
}

float UAudioAnalysisToolsLibrary::GetSpectralDifferenceHWR()
{
	return OnsetDetectionRef->GetSpectralDifferenceHWR(MagnitudeSpectrum);
}

float UAudioAnalysisToolsLibrary::GetComplexSpectralDifference()
{
	return OnsetDetectionRef->GetComplexSpectralDifference(FFTReal, FFTImaginary);
}

float UAudioAnalysisToolsLibrary::GetHighFrequencyContent()
{
	return OnsetDetectionRef->GetHighFrequencyContent(MagnitudeSpectrum);
}

void UAudioAnalysisToolsLibrary::ConfigureFFT()
{
	if (FFTConfigured)
	{
		FreeFFT();
	}

	const int32 FrameSize{CurrentAudioFrame.Num()};

	FFT_InSamples = new FFTComplexSamples[FrameSize];
	FFT_OutSamples = new FFTComplexSamples[FrameSize];
	FFT_Configuration = UFFTAnalyzer::PerformFFTAlloc(FrameSize, 0, nullptr, nullptr);

	FFTConfigured = true;
}

void UAudioAnalysisToolsLibrary::FreeFFT()
{
	// Free the Kiss FFT configuration
	FMemory::Free(FFT_Configuration);

	delete[] FFT_InSamples;
	delete[] FFT_OutSamples;

	FFT_Configuration = nullptr;
	FFT_InSamples = nullptr;
	FFT_OutSamples = nullptr;
}

void UAudioAnalysisToolsLibrary::PerformFFT()
{
	if (!FFT_InSamples || !FFT_OutSamples || !FFT_Configuration)
	{
		UE_LOG(LogAudioAnalysis, Error, TEXT("Unable to perform FFT analysis because the buffers are invalid"));
		return;
	}

	const int32 FrameSize{CurrentAudioFrame.Num()};

	for (int32 Index = 0; Index < FrameSize; ++Index)
	{
		FFT_InSamples[Index].Real = CurrentAudioFrame[Index] * WindowFunction[Index];
		FFT_InSamples[Index].Imaginary = 0.0;
	}

	// Execute kiss fft
	UFFTAnalyzer::PerformFFT(FFT_Configuration, FFT_InSamples, FFT_OutSamples);

	// Store real and imaginary parts of FFT
	for (int32 Index = 0; Index < FrameSize; ++Index)
	{
		FFTReal[Index] = FFT_OutSamples[Index].Real;
		FFTImaginary[Index] = FFT_OutSamples[Index].Imaginary;
	}

	// Calculate the magnitude spectrum
	for (int32 Index = 0; Index < FrameSize / 2; ++Index)
	{
		MagnitudeSpectrum[Index] = FMath::Sqrt(FMath::Pow(FFTReal[Index], 2) + FMath::Pow(FFTImaginary[Index], 2));
	}
}
