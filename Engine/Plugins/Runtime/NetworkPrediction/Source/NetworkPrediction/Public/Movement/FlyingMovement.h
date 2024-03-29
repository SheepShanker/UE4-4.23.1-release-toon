// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/NetSerialization.h"
#include "Engine/EngineTypes.h"
#include "Engine/EngineBaseTypes.h"
#include "WorldCollision.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Misc/OutputDevice.h"
#include "Misc/CoreDelegates.h"
#include "NetworkedSimulationModel.h"
#include "BaseMovementComponent.h"

#include "FlyingMovement.generated.h"

// -------------------------------------------------------------------------------------------------------------------------------
// FlyingMovement: simple flying movement that was based on UE4's FloatingPawnMovement
// -------------------------------------------------------------------------------------------------------------------------------

class USceneComponent;

// State the client generates
struct FFlyingMovementInputCmd
{
	// Input: "pure" input for this frame. At this level, frame time has not been accounted for. (E.g., "move straight" would be (1,0,0) regardless of frame time)
	FRotator RotationInput;
	FVector MovementInput;

	FFlyingMovementInputCmd()
		: RotationInput(ForceInitToZero)
		, MovementInput(ForceInitToZero)
	{ }

	void NetSerialize(const FNetSerializeParams& P)
	{
		P.Ar << RotationInput;
		P.Ar << MovementInput;
	}

	void Log(FStandardLoggingParameters& P) const
	{
		if (P.Context == EStandardLoggingContext::HeaderOnly)
		{
			P.Ar->Logf(TEXT(" %d "), P.Frame);
		}
		else if (P.Context == EStandardLoggingContext::Full)
		{
			P.Ar->Logf(TEXT("Movement: %s"), *MovementInput.ToString());
			P.Ar->Logf(TEXT("ViewRot: %s"), *RotationInput.ToString());
		}
	}
};

// State we are evolving frame to frame and keeping in sync
struct FFlyingMovementSyncState
{
	FVector Location;
	FVector Velocity;
	FRotator Rotation;

	FFlyingMovementSyncState()
		: Location(ForceInitToZero)
		, Velocity(ForceInitToZero)
		, Rotation(ForceInitToZero)
	{ }

	void NetSerialize(const FNetSerializeParams& P)
	{
		P.Ar << Location;
		P.Ar << Velocity;
		P.Ar << Rotation;
	}

	// Compare this state with AuthorityState. return true if a reconcile (correction) should happen
	bool ShouldReconcile(const FFlyingMovementSyncState& AuthorityState)
	{
		const float ErrorTolerance = 1.f;
		return !AuthorityState.Location.Equals(Location, ErrorTolerance);
	}

	void Log(FStandardLoggingParameters& Params) const
	{
		if (Params.Context == EStandardLoggingContext::HeaderOnly)
		{
			Params.Ar->Logf(TEXT(" %d "), Params.Frame);
		}
		else if (Params.Context == EStandardLoggingContext::Full)
		{
			Params.Ar->Logf(TEXT("Frame: %d"), Params.Frame);
			Params.Ar->Logf(TEXT("Loc: %s"), *Location.ToString());
			Params.Ar->Logf(TEXT("Vel: %s"), *Velocity.ToString());
			Params.Ar->Logf(TEXT("Rot: %s"), *Rotation.ToString());
		}
	}

	static void Interpolate(const FFlyingMovementSyncState& From, const FFlyingMovementSyncState& To, const float PCT, FFlyingMovementSyncState& OutDest)
	{
		OutDest.Location = From.Location + ((To.Location - From.Location) * PCT);
		OutDest.Rotation = From.Rotation + ((To.Rotation - From.Rotation) * PCT);
	}
};

// Auxiliary state that is input into the simulation. Doesn't change during the simulation tick.
// (It can change and even be predicted but doing so will trigger more bookeeping, etc. Changes will happen "next tick")
struct FFlyingMovementAuxState
{	
	float MaxSpeed = 1200.f;
	float TurningBoost = 8.f;
	float Deceleration = 8000.f;
	float Acceleration = 4000.f;

	void NetSerialize(const FNetSerializeParams& P)
	{
		P.Ar << MaxSpeed;
		P.Ar << TurningBoost;
		P.Ar << Deceleration;
		P.Ar << Acceleration;
	}

	void Log(FStandardLoggingParameters& P) const
	{
		if (P.Context == EStandardLoggingContext::HeaderOnly)
		{
			P.Ar->Logf(TEXT(" %d "), P.Frame);
		}
		else if (P.Context == EStandardLoggingContext::Full)
		{
			P.Ar->Logf(TEXT("MaxSpeed: %.2f"), MaxSpeed);
			P.Ar->Logf(TEXT("TurningBoost: %.2f"), TurningBoost);
			P.Ar->Logf(TEXT("Deceleration: %.2f"), Deceleration);
			P.Ar->Logf(TEXT("Acceleration: %.2f"), Acceleration);
		}
	}
};
	
using FlyingMovementBufferTypes = TNetworkSimBufferTypes<FFlyingMovementInputCmd, FFlyingMovementSyncState, FFlyingMovementAuxState>;

static FName SimulationGroupName("FlyingMovement");

class FFlyingMovementSimulation : public FBaseMovementSimulation
{
public:

	/** Tick group the simulation maps to */
	static const FName GroupName;

	/** Dev tool to force simple mispredict */
	static bool ForceMispredict;

	/** Main update function */
	NETWORKPREDICTION_API void SimulationTick(const FNetSimTimeStep& TimeStep, const TNetSimInput<FlyingMovementBufferTypes>& Input, const TNetSimOutput<FlyingMovementBufferTypes>& Output);

	// Called prior to running the sim to make sure to make sure the collision component is in the right place. 
	// This is unfortunate and not good, but is needed to ensure our collision and world position have not been moved out from under us.
	// Refactoring primitive component movement to allow the sim to do all collision queries outside of the component code would be ideal.
	void PreSimSync(const FFlyingMovementSyncState& SyncState);

protected:

	float SlideAlongSurface(const FVector& Delta, float Time, const FQuat Rotation, const FVector& Normal, FHitResult& Hit, bool bHandleImpact);
};

// Actual definition of our network simulation.
template<int32 InFixedStepMS=0>
using FFlyingMovementSystem = TNetworkedSimulationModel<FFlyingMovementSimulation, FlyingMovementBufferTypes, TNetworkSimTickSettings<InFixedStepMS>>;

// general tolerance value for rotation checks
static const float ROTATOR_TOLERANCE = (1e-3);

// Needed to trick UHT into letting UMockNetworkSimulationComponent implement. UHT cannot parse the ::
// Also needed for forward declaring. Can't just be a typedef/using =
class IFlyingMovementSystemDriver : public TNetworkedSimulationModelDriver<FlyingMovementBufferTypes> { };

// -------------------------------------------------------------------------------------------------------------------------------
// ActorComponent for running FlyingMovement 
// -------------------------------------------------------------------------------------------------------------------------------

UCLASS(BlueprintType, meta=(BlueprintSpawnableComponent))
class NETWORKPREDICTION_API UFlyingMovementComponent : public UBaseMovementComponent, public IFlyingMovementSystemDriver
{
	GENERATED_BODY()

public:

	UFlyingMovementComponent();
	
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;

	DECLARE_DELEGATE_TwoParams(FProduceFlyingInput, const FNetworkSimTime /*SimTime*/, FFlyingMovementInputCmd& /*Cmd*/)
	FProduceFlyingInput ProduceInputDelegate;

	TNetSimStateAccessor<FFlyingMovementSyncState> MovementSyncState;
	TNetSimStateAccessor<FFlyingMovementAuxState> MovementAuxState;

	// IFlyingMovementSystemDriver
	FString GetDebugName() const override;
	const AActor* GetVLogOwner() const override;
	void VisualLog(const FFlyingMovementInputCmd* Input, const FFlyingMovementSyncState* Sync, const FFlyingMovementAuxState* Aux, const FVisualLoggingParameters& SystemParameters) const override;
	
	void ProduceInput(const FNetworkSimTime SimTime, FFlyingMovementInputCmd& Cmd) override;
	void FinalizeFrame(const FFlyingMovementSyncState& SyncState, const FFlyingMovementAuxState& AuxState) override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=FlyingMovementNetworking)
	bool bEnableInterpolation = true;

protected:

	// Network Prediction
	virtual INetworkedSimulationModel* InstantiateNetworkedSimulation() override;
	FFlyingMovementSimulation* MovementSimulation = nullptr;

	// Init function. This is broken up from ::InstantiateNetworkedSimulation and templated so that subclasses can share the init code
	template<typename TSimulation>
	void InitFlyingMovementSimulation(TSimulation* Simulation, FFlyingMovementSyncState& InitialSyncState, FFlyingMovementAuxState& InitialAuxState)
	{
		check(UpdatedComponent);
		check(MovementSimulation == nullptr); // Reinstantiation not supported
		MovementSimulation = Simulation;
		

		Simulation->UpdatedComponent = UpdatedComponent;
		Simulation->UpdatedPrimitive = UpdatedPrimitive;

		InitialSyncState.Location = UpdatedComponent->GetComponentLocation();
		InitialSyncState.Rotation = UpdatedComponent->GetComponentQuat().Rotator();	

		InitialAuxState.MaxSpeed = GetDefaultMaxSpeed();
	}

	template<typename TNetSimModel>
	void InitFlyingMovementNetSimModel(TNetSimModel* Model)
	{
		Model->RepProxy_Simulated.bAllowSimulatedExtrapolation = !bEnableInterpolation;
		MovementSyncState.Bind(Model);
		MovementAuxState.Bind(Model);
	}

	static float GetDefaultMaxSpeed();
};