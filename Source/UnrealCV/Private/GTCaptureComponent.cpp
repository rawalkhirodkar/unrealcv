#include "UnrealCVPrivate.h"
#include "GTCaptureComponent.h"
#include "ImageWrapper.h"
#include "ViewMode.h"

DECLARE_CYCLE_STAT(TEXT("SaveExr"), STAT_SaveExr, STATGROUP_UnrealCV);
DECLARE_CYCLE_STAT(TEXT("SavePng"), STAT_SavePng, STATGROUP_UnrealCV);
DECLARE_CYCLE_STAT(TEXT("SaveFile"), STAT_SaveFile, STATGROUP_UnrealCV);
DECLARE_CYCLE_STAT(TEXT("ReadPixels"), STAT_ReadPixels, STATGROUP_UnrealCV);
DECLARE_CYCLE_STAT(TEXT("ImageWrapper"), STAT_ImageWrapper, STATGROUP_UnrealCV);
DECLARE_CYCLE_STAT(TEXT("GetResource"), STAT_GetResource, STATGROUP_UnrealCV);

void InitCaptureComponent(USceneCaptureComponent2D* CaptureComponent)
{
	UWorld* World = FUE4CVServer::Get().GetGameWorld();
	// Can not use ESceneCaptureSource::SCS_SceneColorHDR, this option will disable post-processing
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;

	CaptureComponent->TextureTarget = NewObject<UTextureRenderTarget2D>();
	FServerConfig& Config = FUE4CVServer::Get().Config;
	CaptureComponent->TextureTarget->InitAutoFormat(Config.Width, Config.Height); 

	/*
	UGameViewportClient* GameViewportClient = World->GetGameViewport();
	CaptureComponent->TextureTarget->InitAutoFormat(GameViewportClient->Viewport->GetSizeXY().X,  GameViewportClient->Viewport->GetSizeXY().Y); // TODO: Update this later
	*/

	CaptureComponent->RegisterComponentWithWorld(World); // What happened for this?
	// CaptureComponent->AddToRoot(); This is not necessary since it has been attached to the Pawn.
}


void SaveExr(UTextureRenderTarget2D* RenderTarget, FString Filename)
{
	SCOPE_CYCLE_COUNTER(STAT_SaveExr)
	int32 Width = RenderTarget->SizeX, Height = RenderTarget->SizeY;
	TArray<FFloat16Color> FloatImage;
	FloatImage.AddZeroed(Width * Height);
	FTextureRenderTargetResource* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
	RenderTargetResource->ReadFloat16Pixels(FloatImage);

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	IImageWrapperPtr ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::EXR);

	ImageWrapper->SetRaw(FloatImage.GetData(), FloatImage.GetAllocatedSize(), Width, Height, ERGBFormat::RGBA, 16);
	const TArray<uint8>& PngData = ImageWrapper->GetCompressed(ImageCompression::Uncompressed);
	{
		SCOPE_CYCLE_COUNTER(STAT_SaveFile);
		FFileHelper::SaveArrayToFile(PngData, *Filename);
	}
}

void SavePng(UTextureRenderTarget2D* RenderTarget, FString Filename)
{
	SCOPE_CYCLE_COUNTER(STAT_SavePng);
	static IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	static IImageWrapperPtr ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	{
		int32 Width = RenderTarget->SizeX, Height = RenderTarget->SizeY;
		TArray<FColor> Image;
		FTextureRenderTargetResource* RenderTargetResource;
		Image.AddZeroed(Width * Height);
		{
			SCOPE_CYCLE_COUNTER(STAT_GetResource);
			RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
		}
		{
			SCOPE_CYCLE_COUNTER(STAT_ReadPixels);
			FReadSurfaceDataFlags ReadSurfaceDataFlags;
			ReadSurfaceDataFlags.SetLinearToGamma(false); // This is super important to disable this!
			// Instead of using this flag, we will set the gamma to the correct value directly
			RenderTargetResource->ReadPixels(Image, ReadSurfaceDataFlags);
		}
		{
			SCOPE_CYCLE_COUNTER(STAT_ImageWrapper);
			ImageWrapper->SetRaw(Image.GetData(), Image.GetAllocatedSize(), Width, Height, ERGBFormat::BGRA, 8);
		}
		const TArray<uint8>& ImgData = ImageWrapper->GetCompressed(ImageCompression::Uncompressed);
		{
			SCOPE_CYCLE_COUNTER(STAT_SaveFile);
			FFileHelper::SaveArrayToFile(ImgData, *Filename);
		}
	}
}

UMaterial* UGTCaptureComponent::GetMaterial(FString InModeName = TEXT(""))
{
	// Load material for visualization
	static TMap<FString, FString>* MaterialPathMap = nullptr;
	if (MaterialPathMap == nullptr)
	{
		MaterialPathMap = new TMap<FString, FString>();
		MaterialPathMap->Add(TEXT("depth"), TEXT("Material'/UnrealCV/SceneDepthWorldUnits.SceneDepthWorldUnits'"));
		MaterialPathMap->Add(TEXT("vis_depth"), TEXT("Material'/UnrealCV/SceneDepth.SceneDepth'"));
		MaterialPathMap->Add(TEXT("debug"), TEXT("Material'/UnrealCV/debug.debug'"));
		// MaterialPathMap->Add(TEXT("object_mask"), TEXT("Material'/UnrealCV/VertexColorMaterial.VertexColorMaterial'"));
		MaterialPathMap->Add(TEXT("normal"), TEXT("Material'/UnrealCV/WorldNormal.WorldNormal'"));
	}

	static TMap<FString, UMaterial*>* StaticMaterialMap = nullptr;
	if (StaticMaterialMap == nullptr)
	{
		StaticMaterialMap = new TMap<FString, UMaterial*>();
		for (auto& Elem : *MaterialPathMap)
		{
			FString ModeName = Elem.Key;
			FString MaterialPath = Elem.Value;
			ConstructorHelpers::FObjectFinder<UMaterial> Material(*MaterialPath); // ConsturctorHelpers is only available for UObject.

			if (Material.Object != NULL)
			{
				StaticMaterialMap->Add(ModeName, (UMaterial*)Material.Object);
			}
		}
	}

	UMaterial* Material = StaticMaterialMap->FindRef(InModeName);
	if (Material == nullptr)
	{
		UE_LOG(LogUnrealCV, Warning, TEXT("Can not recognize visualization mode %s"), *InModeName);
	}
	return Material;
}

UGTCaptureComponent* UGTCaptureComponent::Create(APawn* InPawn, TArray<FString> Modes)
{
	UWorld* World = FUE4CVServer::Get().GetGameWorld();
	UGTCaptureComponent* GTCapturer = NewObject<UGTCaptureComponent>();

	GTCapturer->bIsActive = true;
	// check(GTCapturer->IsComponentTickEnabled() == true);
	GTCapturer->Pawn = InPawn; // This GTCapturer should depend on the Pawn and be released together with the Pawn.

	// This snippet is from Engine/Source/Runtime/Engine/Private/Components/SceneComponent.cpp, AttachTo
	FAttachmentTransformRules AttachmentRules(EAttachmentRule::KeepRelative, false);
	ConvertAttachLocation(EAttachLocation::KeepRelativeOffset, AttachmentRules.LocationRule, AttachmentRules.RotationRule, AttachmentRules.ScaleRule);
	GTCapturer->AttachToComponent(InPawn->GetRootComponent(), AttachmentRules);
	// GTCapturer->AddToRoot();
	GTCapturer->RegisterComponentWithWorld(World);

	for (FString Mode : Modes)
	{
		// DEPRECATED_FORGAME(4.6, "CaptureComponent2D should not be accessed directly, please use GetCaptureComponent2D() function instead. CaptureComponent2D will soon be private and your code will not compile.")
		USceneCaptureComponent2D* CaptureComponent = NewObject<USceneCaptureComponent2D>();
		CaptureComponent->bIsActive = false; // Disable it by default for performance consideration
		GTCapturer->CaptureComponents.Add(Mode, CaptureComponent);

		// CaptureComponent needs to be attached to somewhere immediately, otherwise it will be gc-ed

		CaptureComponent->AttachToComponent(GTCapturer, AttachmentRules);
		InitCaptureComponent(CaptureComponent);

		UMaterial* Material = GetMaterial(Mode);
		if (Mode == "lit") // For rendered images
		{
			FViewMode::Lit(CaptureComponent->ShowFlags);
			CaptureComponent->TextureTarget->TargetGamma = GEngine->GetDisplayGamma();
			// float DisplayGamma = SceneViewport->GetDisplayGamma();
		}
		else if (Mode == "default")
		{
			continue;
		}
		else // for ground truth
		{
			CaptureComponent->TextureTarget->TargetGamma = 1;
			if (Mode == "object_mask") // For object mask
			{
				FViewMode::Lit(CaptureComponent->ShowFlags);
				FViewMode::VertexColor(CaptureComponent->ShowFlags);
			}
			else if (Mode == "wireframe") // For object mask
			{
				FViewMode::Wireframe(CaptureComponent->ShowFlags);
			}
			else
			{
				check(Material);
				// GEngine->GetDisplayGamma(), the default gamma is 2.2
				// CaptureComponent->TextureTarget->TargetGamma = 2.2;
				FViewMode::PostProcess(CaptureComponent->ShowFlags);

				CaptureComponent->PostProcessSettings.AddBlendable(Material, 1);
				// Instead of switching post-process materials, we create several SceneCaptureComponent, so that we can capture different GT within the same frame.
			}
		}
	}
	return GTCapturer;
}

UGTCaptureComponent::UGTCaptureComponent()
{
	GetMaterial(); // Initialize the TMap
	PrimaryComponentTick.bCanEverTick = true;
	// bIsTicking = false;

	// Create USceneCaptureComponent2D
	// Design Choice: do I need one capture component with changing post-process materials or multiple components?
	// USceneCaptureComponent2D* CaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("GTCaptureComponent"));
}

// Each GTCapturer can serve as one camera of the scene

FAsyncRecord* UGTCaptureComponent::Capture(FString Mode, FString InFilename)
{
	// Flush location and rotation

	check(CaptureComponents.Num() != 0);
	USceneCaptureComponent2D* CaptureComponent = CaptureComponents.FindRef(Mode);
	if (CaptureComponent == nullptr)
		return nullptr;

	const FRotator PawnViewRotation = Pawn->GetViewRotation();
	if (!PawnViewRotation.Equals(CaptureComponent->GetComponentRotation()))
	{
		CaptureComponent->SetWorldRotation(PawnViewRotation);
	}

	FAsyncRecord* AsyncRecord = FAsyncRecord::Create();
	FGTCaptureTask GTCaptureTask = FGTCaptureTask(Mode, InFilename, GFrameCounter, AsyncRecord);
	this->PendingTasks.Enqueue(GTCaptureTask);

	return AsyncRecord;
}

void UGTCaptureComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
// void UGTCaptureComponent::Tick(float DeltaTime) // This tick function should be called by the scene instead of been
{
	// Render pixels out in the next tick. To allow time to render images out.

	// Update rotation of each frame
	// from ab237f46dc0eee40263acbacbe938312eb0dffbb:CameraComponent.cpp:232
	check(this->Pawn); // this GTCapturer should be released, if the Pawn is deleted.
	const APawn* OwningPawn = this->Pawn;
	const AController* OwningController = OwningPawn ? OwningPawn->GetController() : nullptr;
	if (OwningController && OwningController->IsLocalPlayerController())
	{
		const FRotator PawnViewRotation = OwningPawn->GetViewRotation();
		for (auto Elem : CaptureComponents)
		{
			USceneCaptureComponent2D* CaptureComponent = Elem.Value;
			if (!PawnViewRotation.Equals(CaptureComponent->GetComponentRotation()))
			{
				CaptureComponent->SetWorldRotation(PawnViewRotation);
			}
		}
	}

	while (!PendingTasks.IsEmpty())
	{
		FGTCaptureTask Task;
		PendingTasks.Peek(Task);
		uint64 CurrentFrame = GFrameCounter;

		int32 SkipFrame = 1;
		if (!(CurrentFrame > Task.CurrentFrame + SkipFrame)) // TODO: This is not an elegant solution, fix it later.
		{ // Wait for the rendering thread to catch up game thread.
			break;
		}

		PendingTasks.Dequeue(Task);
		USceneCaptureComponent2D* CaptureComponent = this->CaptureComponents.FindRef(Task.Mode);
		if (CaptureComponent == nullptr)
		{
			UE_LOG(LogUnrealCV, Warning, TEXT("Unrecognized capture mode %s"), *Task.Mode);
		}
		else
		{
			FString LowerCaseFilename = Task.Filename.ToLower();
			if (LowerCaseFilename.EndsWith("png"))
			{
				SavePng(CaptureComponent->TextureTarget, Task.Filename);
			}
			else if (LowerCaseFilename.EndsWith("exr"))
			{
				SaveExr(CaptureComponent->TextureTarget, Task.Filename);
			}
			else
			{
				UE_LOG(LogUnrealCV, Warning, TEXT("Unrecognized image file extension %s"), *LowerCaseFilename);
			}
		}
		Task.AsyncRecord->bIsCompleted = true;
	}
}
