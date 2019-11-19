// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once
#include "NetworkPredictionTypes.h"
#include "NetworkedSimulationModelBuffer.h"
#include "NetworkedSimulationModelTypes.h"
#include "NetworkedSimulationModelReplicators.h"
#include "NetworkedSimulationModelDebugger.h"

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
//	TNetworkedSimulationModel
//	
//	* Has all logic for "ticking, advancing buffers, calling Update, calling ServerRPC etc
//	* Doesn't have anything about update component, movesweep, etc
//	* Concept of "IDriver" which is the owning object that is driving the network sim. This is the interface to the outside UE4 world.
//	* Has 4 buffers:
//		-Input: Generated by a client / not the authority.
//		-Sync: What we are trying to keep in sync. The state that evolves frame to frame with an Update function.
//		-Aux: State that is also an input into the simulation but does not intrinsically evolve from to frame. Changes to this state can be trapped/tracked/predicted.
//		-Debug: Replicated buffer from server->client with server-frame centered debug information. Compiled out of shipping builds.
//
//	* How other code interacts with this:
//		-Network updates will come in through UE4 networking -> FReplicationProxy (on actor/component) -> TNetworkedSimulationModel::RepProxy_* -> Buffers.*
//		-UNetworkSimulationGlobalManager: responsible for ticking simulation (after recv net traffic, prior to UE4 actor ticking)
//		-External game code can interact with the system:
//			-The TNetworkedSimulationModel is mostly public and exposed. It is not recommend to publicly expose to your "user" code (high level scripting, designers, etc).
//			-TNetkSimStateAccessor is a helper for safely reading/writing to state within the system.
//
//	* Notes on ownership and lifetime:
//		-User code will instantiate the underlying simulation class (TSimulation)
//		-Ownership of the simulation is taken over by TNetworkedSimulationModel on instantiation
//		-(user code may still want to cache a pointer to the TSimulation instance for its own, but does not need to destroy the simulation)
//		-Swapping TSimulation instance or releasing it early would be possible but use case is not clear so not implemented.
//		-TNetworkedSimulationModel lifetime is the responsibility of the code that created it (UNetworkPredictionComponent in most cases).
//
// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------


struct TNetSimModelTraitsBase
{

};

template<typename T>
struct TNetSimModelTraits : public TNetSimModelTraitsBase
{
	using TSimulation = void;
	using TBufferTypes = void;	
	using TTickSettings = TNetworkSimTickSettings<>;

};


template <
	typename InTSimulation,								// Final Simulation class
	typename TUserBufferTypes,							// The user types (input, sync, aux, debug). Note this gets wrapped in TInternalBufferTypes internally.
	typename InTTickSettings=TNetworkSimTickSettings<>, // Defines global rules about time keeping and ticking

	// Core proxies that dictate how data replicates and how the simulation evolves for the three main roles
	typename TRepProxyServerRPC =	TReplicator_Server		<TInternalBufferTypes<TUserBufferTypes, InTTickSettings>,	InTTickSettings >,
	typename TRepProxyAutonomous =	TReplicator_Autonomous	<TInternalBufferTypes<TUserBufferTypes, InTTickSettings>,	InTTickSettings>,
	typename TRepProxySimulated =	TReplicator_Simulated	<TInternalBufferTypes<TUserBufferTypes, InTTickSettings>,	InTTickSettings>,

	// Defines how replication happens on these special channels, but doesn't dictate how simulation evolves
	typename TRepProxyReplay =		TReplicator_Sequence	<TInternalBufferTypes<TUserBufferTypes, InTTickSettings>,	InTTickSettings, ENetworkSimBufferTypeId::Sync,  3>,
	typename TRepProxyDebug =		TReplicator_Debug		<TInternalBufferTypes<TUserBufferTypes, InTTickSettings>,	InTTickSettings>
>
class TNetworkedSimulationModel : public INetworkedSimulationModel
{
public:

	using TSimulation = InTSimulation;
	using TDriver = TNetworkedSimulationModelDriver<TUserBufferTypes>;

	using TBufferTypes = TInternalBufferTypes<TUserBufferTypes, InTTickSettings>;
	using TTickSettings = InTTickSettings;

	using TInputCmd = typename TBufferTypes::TInputCmd;
	using TSyncState = typename TBufferTypes::TSyncState;
	using TAuxState = typename TBufferTypes::TAuxState;
	using TDebugState = typename TBufferTypes::TDebugState;

	using TSimTime = FNetworkSimTime;
	using TRealTime = FNetworkSimTime::FRealTime;
	
	// Note: Ownership of TSimulation* is taken by this constructor
	template<typename TInDriver>
	TNetworkedSimulationModel(TSimulation* InSimulation, TInDriver* InDriver, const TSyncState& InitialSyncState = TSyncState(), const TAuxState& InitialAuxState = TAuxState())
		: Simulation(InSimulation)
	{
		check(Simulation.IsValid() && InDriver);
		Driver = InDriver;
		*Buffers.Sync.WriteFrame(0) = InitialSyncState;
		*Buffers.Aux.WriteFrame(0) = InitialAuxState;
		Ticker.SetTotalProcessedSimulationTime(FNetworkSimTime(), 0);
		DO_NETSIM_MODEL_DEBUG(FNetworkSimulationModelDebuggerManager::Get().RegisterNetworkSimulationModel(this, Driver->GetVLogOwner()));

		ProcessPendingNetSimCuesFunc = [this, InDriver]()
		{ 
			Buffers.CueDispatcher.template DispatchCueRecord<TInDriver>(*InDriver, Ticker.GetTotalProcessedSimulationTime()); 
		};
	}

	virtual ~TNetworkedSimulationModel()
	{
		SetParentSimulation(nullptr);
		ClearAllDependentSimulations();
	}

	void Tick(const FNetSimTickParameters& Parameters) final override
	{
		// Update previous DebugState based on what we (might) have sent *after* our last Tick 
		// (property replication and ServerRPC get sent after the tick, rather than forcing awkward callback into the NetSim post replication, we can check it here)
		if (auto* DebugBuffer = GetLocalDebugBuffer())
		{
			if (TDebugState* const PrevDebugState = DebugBuffer->Get(DebugBuffer->HeadFrame()))
			{
				if (Parameters.Role == ROLE_AutonomousProxy)
				{
					PrevDebugState->LastSentInputFrame = RepProxy_ServerRPC.GetLastSerializedFrame();
				}
				else if (Parameters.Role == ROLE_Authority)
				{
					PrevDebugState->LastSentInputFrame = RepProxy_Autonomous.GetLastSerializedFrame();
				}
			}
		}

		// Current frame debug state
		TDebugState* const DebugState = GetNextLocalDebugStateWrite();
		if (DebugState)
		{
			*DebugState = TDebugState();
			DebugState->LocalDeltaTimeSeconds = Parameters.LocalDeltaTimeSeconds;
			DebugState->LocalGFrameNumber = GFrameNumber;
			DebugState->ProcessedFrames.Reset();
			
			if (Parameters.Role == ROLE_AutonomousProxy)
			{
				DebugState->LastReceivedInputFrame = RepProxy_Autonomous.GetLastSerializedFrame();
			}
			else if (Parameters.Role == ROLE_Authority)
			{
				DebugState->LastReceivedInputFrame = RepProxy_ServerRPC.GetLastSerializedFrame();
			}
		}

		// ----------------------------------------------------------------------------------------------------------------
		//	PreSimTick
		//	This is the beginning of a new frame. PreSimTick will decide if we should take Parameters.LocalDeltaTimeSeconds
		//	and advance the simulation or not. It will also generate new local input if necessary.
		// ----------------------------------------------------------------------------------------------------------------
		switch (Parameters.Role)
		{
			case ROLE_Authority:
				RepProxy_ServerRPC.template PreSimTick<TDriver>(Driver, Buffers, Ticker, Parameters);
			break;

			case ROLE_AutonomousProxy:
				RepProxy_Autonomous.template PreSimTick<TDriver>(Driver, Buffers, Ticker, Parameters);
			break;

			case ROLE_SimulatedProxy:
				RepProxy_Simulated.template PreSimTick<TDriver>(Driver, Buffers, Ticker, Parameters);
			break;
		}

		// -------------------------------------------------------------------------------------------------------------------------------------------------
		//												Input Processing & Simulation Update
		// -------------------------------------------------------------------------------------------------------------------------------------------------
		while (Ticker.PendingFrame <= Ticker.MaxAllowedFrame)
		{
			const int32 InputFrame = Ticker.PendingFrame;
			const int32 OutputFrame = Ticker.PendingFrame + 1;

			const TInputCmd* InputCmd = Buffers.Input[InputFrame];
			if (!ensureMsgf(InputCmd, TEXT("No InputCmd available for Frame %d. PendingFrame: %d. MaxAllowedFrame: %d."), InputFrame, Ticker.PendingFrame, Ticker.MaxAllowedFrame))
			{
				break;
			}

			// We have an unprocessed command, do we have enough allotted simulation time to process it?
			if (Ticker.GetRemainingAllowedSimulationTime() >= InputCmd->GetFrameDeltaTime())
			{
				TSyncState* InSyncState = Buffers.Sync[InputFrame];
				if (InSyncState == nullptr)
				{
					// We don't have valid sync state for this frame. This could mean we skipped ahead somehow, such as heavy packet loss where the server doesn't receive all input cmds
					// We will use the last known sync state, but first write it out to the current InputFrame
					UE_LOG(LogNetworkSim, Log, TEXT("%s: No sync state found @ frame %d. Using previous head element @ frame %d."), *Driver->GetDebugName(), InputFrame, Buffers.Sync.HeadFrame());
					InSyncState = Buffers.Sync.WriteFrameInitializedFromHead(InputFrame);
				}

				const TAuxState* InAuxState = Buffers.Aux[InputFrame];
				checkf(InAuxState, TEXT("No AuxState available for Frame %d. PendingFrame: %d. MaxAllowedFrame: %d."), InputFrame, Ticker.PendingFrame, Ticker.MaxAllowedFrame);

				TSyncState* OutSyncState = Buffers.Sync.WriteFrame(OutputFrame);
				check(OutSyncState);

				{
					ESimulationTickContext Context = Parameters.Role == ROLE_Authority ? ESimulationTickContext::Authority : ESimulationTickContext::Predict;
					TScopedSimulationTick UpdateScope(Ticker, Buffers.CueDispatcher, Context, OutputFrame, InputCmd->GetFrameDeltaTime());
					
					Simulation->SimulationTick( 
						{ InputCmd->GetFrameDeltaTime(), Ticker },
						{ *InputCmd, *InSyncState, *InAuxState },
						{ *OutSyncState, Buffers.Aux.LazyWriter(OutputFrame), Buffers.CueDispatcher } );
													
				}

				if (DebugState)
				{
					DebugState->ProcessedFrames.Add(InputFrame);
				}
			}
			else
			{
				break;
			}
		}

		// -------------------------------------------------------------------------------------------------------------------------------------------------
		//												Post Sim Tick: finalize the frame
		// -------------------------------------------------------------------------------------------------------------------------------------------------

		switch (Parameters.Role)
		{
			case ROLE_Authority:
				RepProxy_ServerRPC.template PostSimTick<TDriver>(Driver, Buffers, Ticker, Parameters);
			break;

			case ROLE_AutonomousProxy:
				RepProxy_Autonomous.template PostSimTick<TDriver>(Driver, Buffers, Ticker, Parameters);
			break;

			case ROLE_SimulatedProxy:
				RepProxy_Simulated.template PostSimTick<TDriver>(Driver, Buffers, Ticker, Parameters);
			break;
		}

		// -------------------------------------------------------------------------------------------------------------------------------------------------
		//														Debug
		// -------------------------------------------------------------------------------------------------------------------------------------------------

		// Finish debug state buffer recording (what the server processed each frame)
		if (DebugState)
		{
			DebugState->PendingFrame = Ticker.PendingFrame;
			DebugState->HeadFrame = Buffers.Input.HeadFrame();
			DebugState->RemainingAllowedSimulationTimeSeconds = (float)Ticker.GetRemainingAllowedSimulationTime().ToRealTimeSeconds();
		}

		// Historical data recording (longer buffers for historical reference)
		if (auto* HistoricData = GetHistoricBuffers())
		{
			HistoricData->Input.CopyAndMerge(Buffers.Input);
			HistoricData->Sync.CopyAndMerge(Buffers.Sync);
			HistoricData->Aux.CopyAndMerge(Buffers.Aux);
		}
	}

	virtual void Reconcile(const ENetRole Role) final override
	{
		// --------------------------------------------------------------------------------------------------------------------------
		//	Reconcile
		//	This will eventually be called outside the Tick loop, only after processing a network bunch
		//	Reconcile is about "making things right" after a network update. We are not processing "more" simulation yet.
		// --------------------------------------------------------------------------------------------------------------------------
		switch (Role)
		{
			case ROLE_Authority:
				RepProxy_ServerRPC.template Reconcile<TSimulation, TDriver>(Simulation.Get(), Driver, Buffers, Ticker);
			break;

			case ROLE_AutonomousProxy:
				RepProxy_Autonomous.template Reconcile<TSimulation, TDriver>(Simulation.Get(), Driver, Buffers, Ticker);
			break;

			case ROLE_SimulatedProxy:
				RepProxy_Simulated.template Reconcile<TSimulation, TDriver>(Simulation.Get(), Driver, Buffers, Ticker);
			break;
		}
	}
	
	void InitializeForNetworkRole(const ENetRole Role, const FNetworkSimulationModelInitParameters& Parameters) final override
	{
		// FIXME: buffer sizes are now inlined allocated but we want to support role based buffer sizes

		//Buffers.Input.SetBufferSize(Parameters.InputBufferSize);
		//Buffers.Sync.SetBufferSize(Parameters.SyncedBufferSize);
		//Buffers.Aux.SetBufferSize(Parameters.AuxBufferSize); AUXFIXME

		if (GetLocalDebugBuffer())
		{
			//GetLocalDebugBuffer()->SetBufferSize(Parameters.DebugBufferSize);
		}

		if (auto* MyHistoricBuffers = GetHistoricBuffers(true))
		{
			//MyHistoricBuffers->Input.SetBufferSize(Parameters.HistoricBufferSize);
			//MyHistoricBuffers->Sync.SetBufferSize(Parameters.HistoricBufferSize);
			//MyHistoricBuffers->Aux.SetBufferSize(Parameters.HistoricBufferSize); AUXFIXME
		}

		//Ticker.InitSimulationTimeBuffer(Parameters.SyncedBufferSize);
	}

	void NetSerializeProxy(EReplicationProxyTarget Target, const FNetSerializeParams& Params) final override
	{
		switch(Target)
		{
		case EReplicationProxyTarget::ServerRPC:
			RepProxy_ServerRPC.NetSerialize(Params, Buffers, Ticker);
			break;
		case EReplicationProxyTarget::AutonomousProxy:
			RepProxy_Autonomous.NetSerialize(Params, Buffers, Ticker);
			break;
		case EReplicationProxyTarget::SimulatedProxy:
			RepProxy_Simulated.NetSerialize(Params, Buffers, Ticker);
			break;
		case EReplicationProxyTarget::Replay:
			RepProxy_Replay.NetSerialize(Params, Buffers, Ticker);
			break;
		case EReplicationProxyTarget::Debug:
#if NETSIM_MODEL_DEBUG
			RepProxy_Debug.NetSerialize(Params, Buffers, Ticker);
			break;
#endif
		default:
			checkf(false, TEXT("Unknown: %d"), (int32)Target);
		};
	}

	int32 GetProxyDirtyCount(EReplicationProxyTarget Target) final override
	{
		switch(Target)
		{
		case EReplicationProxyTarget::ServerRPC:
			return RepProxy_ServerRPC.GetProxyDirtyCount(Buffers);
		case EReplicationProxyTarget::AutonomousProxy:
			return RepProxy_Autonomous.GetProxyDirtyCount(Buffers);
		case EReplicationProxyTarget::SimulatedProxy:
			return RepProxy_Simulated.GetProxyDirtyCount(Buffers);
		case EReplicationProxyTarget::Replay:
			return RepProxy_Replay.GetProxyDirtyCount(Buffers);
		case EReplicationProxyTarget::Debug:
#if NETSIM_MODEL_DEBUG
			return RepProxy_Debug.GetProxyDirtyCount(Buffers);
#endif
		default:
			checkf(false, TEXT("Unknown: %d"), (int32)Target);
			return 0;
		};
	}

	ESimulatedUpdateMode GetSimulatedUpdateMode() const
	{
		return RepProxy_Simulated.GetSimulatedUpdateMode();
	}

	// ------------------------------------------------------------------------------------------------------
	// State Accessors. This allows game code to read or conditionally write to the pending frame values.
	// This mirrors what TNetSimStateAccessor do, but is callable directly on the NetworkedSimulationModel.
	// Accessor conditionally gives access to the current (pending) Sync/Aux state to outside code.
	// Reads are always allowed, Writes are conditional and null will sometimes be returned!
	//
	// Authority can always write to the pending frame. Non authority requires the netsim to be currently processing an ::SimulationTick.
	// If you aren't inside an ::SimulationTick call, it is really not safe to predict state changes. It is safest and simplest to just not predict these changes.
	//
	// Explanation: During the scope of an ::SimulationTick call, we know exactly 'when' we are relative to what the server is processing. If the predicting client wants
	// to predict a change to sync/aux state during an update, the server will do it at the exact same time (assuming not a mis prediction). When a state change
	// happens "out of band" (outside an ::SimulationTick call) - we really have no way to correlate when the server will do it. While its tempting to think "we will get
	// a correction anyways, might as well guess at it and maybe get a smaller correction" - but this opens us up to other problems. The server may actually not 
	// change the state at all and you may not get an update that corrects you. You could add a timeout and track the state change somewhere but that really complicates
	// things and could leave you open to "double" problems: if the state change is additive, you may stack the authority change on top of the local predicted change, or
	// you may roll back the predicted change to then later receive the authority change.
	//	
	// What still may make sense to do is allow the "In Update" bool to be temporarily disabled if we enter code that we know is not rollback friendly.
	// ------------------------------------------------------------------------------------------------------

	template<typename TState>
	const TState* GetPendingStateRead() const
	{
		// Reading is always allowed
		auto& Buffer = NetSimBufferSelect::Get<TNetworkSimBufferContainer<TBufferTypes>, TState>(Buffers);
		return Buffer[Ticker.PendingFrame];
	}

	template<typename TState>
	TState* GetPendingStateWrite(bool bHasAuthority)
	{
		// Writes are always allowed on the authority or during a simulation update for predicting clients
		if (bHasAuthority || Ticker.bUpdateInProgress)
		{
			auto& Buffer = NetSimBufferSelect::Get<TNetworkSimBufferContainer<TBufferTypes>, TState>(Buffers);
			return Buffer.WriteFrameInitializedFromHead(Ticker.PendingFrame);
		}
		return nullptr;
	}

	// ------------------------------------------------------------------------------------------------------
	//	Dependent Simulations
	// ------------------------------------------------------------------------------------------------------

	void SetParentSimulation(INetworkedSimulationModel* ParentSimulation) final override
	{
		if (RepProxy_Simulated.ParentSimulation)
		{
			RepProxy_Simulated.ParentSimulation->RemoveDependentSimulation(this);
		}
		
		RepProxy_Simulated.ParentSimulation = ParentSimulation;
		if (ParentSimulation)
		{
			ParentSimulation->AddDependentSimulation(this);
		}
	}

	INetworkedSimulationModel* GetParentSimulation() const final override
	{
		return RepProxy_Simulated.ParentSimulation;
	}

	void AddDependentSimulation(INetworkedSimulationModel* DependentSimulation) final override
	{
		check(RepProxy_Autonomous.DependentSimulations.Contains(DependentSimulation) == false);
		RepProxy_Autonomous.DependentSimulations.Add(DependentSimulation);
		NotifyDependentSimNeedsReconcile(); // force reconcile on purpose
	}

	void RemoveDependentSimulation(INetworkedSimulationModel* DependentSimulation) final override
	{
		RepProxy_Autonomous.DependentSimulations.Remove(DependentSimulation);
	}

	void NotifyDependentSimNeedsReconcile()
	{
		RepProxy_Autonomous.bDependentSimulationNeedsReconcile = true;
	}

	void BeginRollback(const FNetworkSimTime& RollbackDeltaTime, const int32 ParentFrame) final override
	{
		RepProxy_Simulated.template DependentRollbackBegin<TSimulation, TDriver>(Simulation.Get(), Driver, Buffers, Ticker, RollbackDeltaTime, ParentFrame);
	}

	void StepRollback(const FNetworkSimTime& Step, const int32 ParentFrame, const bool bFinalStep) final override
	{
		RepProxy_Simulated.template DependentRollbackStep<TSimulation, TDriver>(Simulation.Get(), Driver, Buffers, Ticker, Step, ParentFrame, bFinalStep);
	}

	void ClearAllDependentSimulations()
	{
		TArray<INetworkedSimulationModel*> LocalList = MoveTemp(RepProxy_Autonomous.DependentSimulations);
		for (INetworkedSimulationModel* DependentSim : LocalList)
		{
			DependentSim->SetParentSimulation(nullptr);
		}
	}

	// -------------------------------------------------------------------------------------------------------

	TUniquePtr<TSimulation> Simulation;
	TDriver* Driver = nullptr;

	TSimulationTicker<TTickSettings> Ticker;

	TNetworkSimBufferContainer<TBufferTypes> Buffers;

	TRepProxyServerRPC RepProxy_ServerRPC;
	TRepProxyAutonomous RepProxy_Autonomous;
	TRepProxySimulated RepProxy_Simulated;
	TRepProxyReplay RepProxy_Replay;

	using TInputBuffer = typename TNetworkSimBufferContainer<TBufferTypes>::TInputBuffer;
	using TSyncBuffer = typename TNetworkSimBufferContainer<TBufferTypes>::TSyncBuffer;
	using TAuxBuffer = typename TNetworkSimBufferContainer<TBufferTypes>::TAuxBuffer;
	using TDebugBuffer = typename TNetworkSimBufferContainer<TBufferTypes>::TDebugBuffer;

	// ------------------------------------------------------------------
	// RPC Sending helper: provides basic send frequency settings for tracking when the Server RPC can be invoked.
	// Note that the Driver is the one that must call the RPC, that cannot be rolled into this templated structure.
	// More flexbile/dynamic send rates may be desireable. There is not reason this *has* to be done here, it could
	// completely be tracked at the driver level, but that will also push more boilerplate to that layer for users.
	// ------------------------------------------------------------------

	void SetDesiredServerRPCSendFrequency(float DesiredHz) final override { ServerRPCThresholdTimeSeconds = 1.f / DesiredHz; }
	bool ShouldSendServerRPC(float DeltaTimeSeconds) final override
	{
		// Don't allow a large delta time to pollute the accumulator
		const float CappedDeltaTimeSeconds = FMath::Min<float>(DeltaTimeSeconds, ServerRPCThresholdTimeSeconds);
		ServerRPCAccumulatedTimeSeconds += DeltaTimeSeconds;
		if (ServerRPCAccumulatedTimeSeconds >= ServerRPCThresholdTimeSeconds)
		{
			ServerRPCAccumulatedTimeSeconds -= ServerRPCThresholdTimeSeconds;
			return true;
		}

		return false;
	}

	// Simulation class should have a static const FName GroupName member
	FName GetSimulationGroupName() const final override { return TSimulation::GroupName; }

private:
	float ServerRPCAccumulatedTimeSeconds = 0.f;
	float ServerRPCThresholdTimeSeconds = 1.f / 999.f; // Default is to send at a max of 999hz. This part of the system needs to be build out more (better handling of super high FPS clients and fixed rate servers)

	// ------------------------------------------------------------------
	//	Debugging
	// ------------------------------------------------------------------
public:

#if NETSIM_MODEL_DEBUG
	TDebugBuffer* GetLocalDebugBuffer() {	return &Buffers.Debug; }
	TDebugState* GetNextLocalDebugStateWrite() { return Buffers.Debug.WriteFrame( Buffers.Debug.HeadFrame() + 1 ); }
	TNetworkSimBufferContainer<TBufferTypes>* GetHistoricBuffers(bool bCreate=false)
	{
		if (HistoricBuffers.IsValid() == false && bCreate) { HistoricBuffers.Reset(new TNetworkSimBufferContainer<TBufferTypes>()); }
		return HistoricBuffers.Get();
	}

	TDebugBuffer* GetRemoteDebugBuffer() {	return &RepProxy_Debug.ReceivedBuffer; }
#else
	TDebugBuffer* GetLocalDebugBuffer() {	return nullptr; }
	TDebugState* GetNextLocalDebugStateWrite() { return nullptr; }
	TNetworkSimBufferContainer<TBufferTypes>* GetHistoricBuffers(bool bCreate=false) { return nullptr; }
	TDebugBuffer* GetRemoteDebugBuffer() {	return nullptr; }
#endif

private:

#if NETSIM_MODEL_DEBUG
	TRepProxyDebug RepProxy_Debug;
	TUniquePtr<TNetworkSimBufferContainer<TBufferTypes>> HistoricBuffers;
#endif

};