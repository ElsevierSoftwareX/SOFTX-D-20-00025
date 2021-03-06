/* Copyright (c) 2013-2020, MUSEN Development Team. All rights reserved.
   This file is part of MUSEN framework http://msolids.net/musen.
   See LICENSE file for license and warranty information. */

#include "ConsoleSimulator.h"
#include "ScriptRunner.h"

CConsoleSimulator::CConsoleSimulator(CSystemStructure& _systemStructure, std::ostream& _out/* = std::cout*/, std::ostream& _err/* = std::cerr*/) :
	m_out{ _out },
	m_err{ _err },
	m_systemStructure{ _systemStructure }
{
}

CSimulatorManager& CConsoleSimulator::GetSimulatorManager()
{
	return m_simulatorManager;
}

void CConsoleSimulator::Simulate(const SJob* _job /*= nullptr*/)
{
	if (_job)
		m_job = *_job;

	// apply all settings
	Initialize();

	// check that file is loaded
	if (m_systemStructure.GetFileName().empty())
	{
		m_err << "Error: No file is loaded" << std::endl;
		return;
	}

	// check all settings
	if (!SimulationPrecheck())
		return;

	// print all information about the simulation and run it
	PrintSimulationInfo();
	RunSimulation();
}

void CConsoleSimulator::Initialize(const SJob* _job /*= nullptr*/)
{
	if (_job)
		m_job = *_job;

	SetupSystemStructure();
	SetupGenerationManager();
	SetupModelManager();
	SetupSimulationManager();

	// save (potential) changes to always have a consistent file
	m_simulatorManager.SaveConfiguration();
	m_modelManager.SaveConfiguration();
	m_generationManager.SaveConfiguration();
}

void CConsoleSimulator::SetupSystemStructure() const
{
	if (m_job.anisotropyFlag.IsDefined())		m_systemStructure.EnableAnisotropy(m_job.anisotropyFlag.ToBool());
	if (m_job.contactRadiusFlag.IsDefined())	m_systemStructure.EnableContactRadius(m_job.contactRadiusFlag.ToBool());
	if (!m_job.simulationDomain.IsInf())		m_systemStructure.SetSimulationDomain(m_job.simulationDomain);
}

void CConsoleSimulator::SetupGenerationManager()
{
	m_generationManager.SetAgglomeratesDatabase(&m_agglomeratesDatabase);
	m_generationManager.SetSystemStructure(&m_systemStructure);
	m_generationManager.LoadConfiguration();
	m_agglomeratesDatabase.LoadFromFile(m_job.agglomeratesDBFileName);
}

void CConsoleSimulator::SetupModelManager()
{
	// function to set models and their parameters if they were defined in job
	const auto SetupModel = [&](EMusenModelType _type)
	{
		if (!m_job.models[_type].name.empty())			m_modelManager.SetModelPath(_type, m_job.models[_type].name);
		if (!m_job.models[_type].parameters.empty())	m_modelManager.SetModelParameters(_type, m_job.models[_type].parameters);
	};

	// create and setup model manager
	m_modelManager.SetSystemStructure(&m_systemStructure);
#ifndef STATIC_MODULES
	m_modelManager.AddDir(".");	// add current directory
#endif
	m_modelManager.LoadConfiguration();

	// set models and their parameters if they were defined in job
	SetupModel(EMusenModelType::PP);
	SetupModel(EMusenModelType::PW);
	SetupModel(EMusenModelType::SB);
	SetupModel(EMusenModelType::LB);
	SetupModel(EMusenModelType::EF);

	if (m_job.connectedPPContactFlag.IsDefined()) m_modelManager.SetConnectedPPContact(m_job.connectedPPContactFlag.ToBool());
}

void CConsoleSimulator::SetupSimulationManager()
{
	m_simulatorManager.SetSystemStructure(&m_systemStructure);
	m_simulatorManager.LoadConfiguration();
	m_simulatorManager.GetSimulatorPtr()->SetSystemStructure(&m_systemStructure);
	m_simulatorManager.GetSimulatorPtr()->SetGenerationManager(&m_generationManager);
	m_simulatorManager.GetSimulatorPtr()->SetModelManager(&m_modelManager);
	m_simulatorManager.GetSimulatorPtr()->LoadConfiguration();
	if (m_job.simulatorType != ESimulatorType::BASE)
		m_simulatorManager.SetSimulatorType(m_job.simulatorType);

	if (m_simulatorManager.GetSimulatorPtr()->GetType() == ESimulatorType::CPU && m_job.saveCollsionsFlag == true)
		dynamic_cast<CCPUSimulator*>(m_simulatorManager.GetSimulatorPtr())->EnableCollisionsAnalysis(m_job.saveCollsionsFlag.ToBool());

	// set parameters of simulator if they were redefined
	if (m_job.dSimulationTimeStep != 0)	m_simulatorManager.GetSimulatorPtr()->SetInitSimulationStep(m_job.dSimulationTimeStep);
	if (m_job.dSavingTimeStep != 0)		m_simulatorManager.GetSimulatorPtr()->SetSavingStep(m_job.dSavingTimeStep);
	if (m_job.dEndSimulationTime != 0)	m_simulatorManager.GetSimulatorPtr()->SetEndTime(m_job.dEndSimulationTime);
	if (!m_job.vExtAccel.IsInf())		m_simulatorManager.GetSimulatorPtr()->SetExternalAccel(m_job.vExtAccel);

	// set parameters of verlet list if they were redefined
	if (m_job.verletAutoFlag.IsDefined())	m_simulatorManager.GetSimulatorPtr()->SetAutoAdjustFlag(m_job.verletAutoFlag.ToBool());
	if (m_job.verletCoef != 0)				m_simulatorManager.GetSimulatorPtr()->SetVerletCoeff(m_job.verletCoef);
	if (m_job.iVerletMaxCells != 0)			m_simulatorManager.GetSimulatorPtr()->SetMaxCells(m_job.iVerletMaxCells);

	// set parameters of variable time step if they were redefined
	if (m_job.variableTimeStepFlag.IsDefined()) m_simulatorManager.GetSimulatorPtr()->SetVariableTimeStep(m_job.variableTimeStepFlag.ToBool());
	if (m_job.maxPartMove != 0)					m_simulatorManager.GetSimulatorPtr()->SetPartMoveLimit(m_job.maxPartMove);
	if (m_job.stepIncFactor != 0)				m_simulatorManager.GetSimulatorPtr()->SetTimeStepFactor(m_job.stepIncFactor);

	// set selective saving flags
	if (m_job.selectiveSavingFlag.IsDefined())
	{
		m_simulatorManager.GetSimulatorPtr()->SetSelectiveSaving(m_job.selectiveSavingFlag.ToBool());
		m_simulatorManager.GetSimulatorPtr()->SetSelectiveSavingParameters(m_job.selectiveSavingFlags);
	}

	// set additional stop criteria
	if (!m_job.stopCriteria.empty())
	{
		m_simulatorManager.GetSimulatorPtr()->SetStopCriteria(m_job.stopCriteria);
		m_simulatorManager.GetSimulatorPtr()->SetStopValues(m_job.stopValues);
	}
}

bool CConsoleSimulator::SimulationPrecheck() const
{
	std::string errorMessage;
	std::string warningMessage;

	const auto AddErrorMessage = [&](const std::string& _message)
	{
		if (!_message.empty())
			errorMessage += "Error: " + _message + "\n";
	};

	// check materials database
	AddErrorMessage(m_systemStructure.m_MaterialDatabase.IsDataCorrect());

	// check models
	AddErrorMessage(m_modelManager.GetModelError(EMusenModelType::PP));
	AddErrorMessage(m_modelManager.GetModelError(EMusenModelType::PW));
	AddErrorMessage(m_modelManager.GetModelError(EMusenModelType::SB));
	AddErrorMessage(m_modelManager.GetModelError(EMusenModelType::LB));
	AddErrorMessage(m_modelManager.GetModelError(EMusenModelType::EF));

	if (!m_modelManager.IsModelDefined(EMusenModelType::PP))
		warningMessage += "Warning: Particle-particle contact model is not specified\n";
	if (m_systemStructure.GetNumberOfSpecificObjects(TRIANGULAR_WALL))
		if (!m_modelManager.IsModelDefined(EMusenModelType::PW))
			AddErrorMessage("Particle-wall contact model is not specified");
	if (m_systemStructure.GetNumberOfSpecificObjects(SOLID_BOND))
		if (!m_modelManager.IsModelDefined(EMusenModelType::SB))
			AddErrorMessage("Solid bond model is not specified");

	// check simulator
	if (m_simulatorManager.GetSimulatorPtr()->GetType() == ESimulatorType::BASE)
		AddErrorMessage("Wrong simulator type");

	AddErrorMessage(m_simulatorManager.GetSimulatorPtr()->IsDataCorrect());

	// print warnings and errors
	if (!warningMessage.empty())	m_out << warningMessage << std::endl;
	if (!errorMessage.empty())		m_err << errorMessage   << std::endl;

	return errorMessage.empty();
}

void CConsoleSimulator::RunSimulation() const
{
	m_out << " ==================== Simulation started  ====================" << std::endl;
	m_simulatorManager.GetSimulatorPtr()->p_out = &m_out;
	m_simulatorManager.GetSimulatorPtr()->Simulate();
	m_out << " ==================== Simulation finished ====================" << std::endl;
}

void CConsoleSimulator::PrintSimulationInfo()
{
	// converts bool variable to Yes/No text
	const auto B2S = [](bool _b) { return _b ? "Yes" : "No"; };

	const CBaseSimulator* simulator = m_simulatorManager.GetSimulatorPtr();
	const ESimulatorType simType = simulator->GetType();
	const SPBC pbc = m_systemStructure.GetPBC();
	const SVolumeType simDomain = m_systemStructure.GetSimulationDomain();
	const CVector3 extAccel = simulator->GetExternalAccel();

	if (!m_job.agglomeratesDBFileName.empty())
		PrintFormatted("Agglomerates database", m_job.agglomeratesDBFileName);
	PrintFormatted("Simulator type", simType == ESimulatorType::CPU ? "CPU" : simType == ESimulatorType::GPU ? "GPU" : "Unknown");
	if (simType == ESimulatorType::GPU)
		PrintGPUInfo();
	PrintFormatted("Initial simulation time step [s]", simulator->GetInitSimulationStep());
	PrintFormatted("Saving time step [s]", simulator->GetSavingStep());
	PrintFormatted("End time [s]", simulator->GetEndTime());
	PrintFormatted("Variable simulation time step", B2S(simulator->GetVariableTimeStep()));
	if (simulator->GetVariableTimeStep())
	{
		PrintFormatted("Max allowed particles movement [m]", simulator->GetPartMoveLimit());
		PrintFormatted("Time step increase factor", simulator->GetTimeStepFactor());
	}
	if (!simulator->GetStopCriteria().empty())
		for (const auto& criterion : simulator->GetStopCriteria())
			switch (criterion)
			{
			case CBaseSimulator::EStopCriteria::NONE: break;
			case CBaseSimulator::EStopCriteria::BROKEN_BONDS:
				PrintFormatted("Additional stop criterion", "BROKEN_BONDS", simulator->GetStopValues().maxBrokenBonds);
				break;
			}
	PrintModelsInfo();
	PrintFormatted("Auto-adjust Verlet distance", B2S(simulator->GetAutoAdjustFlag()));
	PrintFormatted(simulator->GetAutoAdjustFlag() ? "Initial Verlet coefficient" : "Verlet coefficient", simulator->GetVerletCoeff());
	PrintFormatted("Max Verlet cell number", simulator->GetMaxCells());
	PrintFormatted("Consider particles anisotropy", B2S(m_systemStructure.IsAnisotropyEnabled()));
	PrintFormatted("Extended contact radius", B2S(m_systemStructure.IsContactRadiusEnabled()));
	PrintFormatted("Collisions saving", B2S(simType == ESimulatorType::CPU && dynamic_cast<const CCPUSimulator*>(simulator)->IsCollisionsAnalysisEnabled()));
	PrintFormatted("Selective saving", B2S(simulator->IsSelectiveSavingEnabled()));
	PrintFormatted("Periodic boundaries", B2S(pbc.bEnabled));
	if (pbc.bEnabled)
	{
		PrintFormatted(" Periodic boundaries enabled (X:Y:Z)", B2S(pbc.bX), ":", B2S(pbc.bY), ":", B2S(pbc.bZ));
		if (pbc.bX)	PrintFormatted(" Periodic boundary domain X [m]", pbc.initDomain.coordBeg.x, "to", pbc.initDomain.coordEnd.x);
		if (pbc.bY)	PrintFormatted(" Periodic boundary domain Y [m]", pbc.initDomain.coordBeg.y, "to", pbc.initDomain.coordEnd.y);
		if (pbc.bZ)	PrintFormatted(" Periodic boundary domain Z [m]", pbc.initDomain.coordBeg.z, "to", pbc.initDomain.coordEnd.z);
	}
	PrintFormatted("External acceleration (X:Y:Z) [m/s^2]", extAccel.x, ":", extAccel.y, ":", extAccel.z);
	PrintFormatted("Simulation domain X [m]", simDomain.coordBeg.x, "to", simDomain.coordEnd.x);
	PrintFormatted("Simulation domain Y [m]", simDomain.coordBeg.y, "to", simDomain.coordEnd.y);
	PrintFormatted("Simulation domain Z [m]", simDomain.coordBeg.z, "to", simDomain.coordEnd.z);

	m_out << std::endl;

	PrintFormatted("Total number of particles",    m_systemStructure.GetNumberOfSpecificObjects(SPHERE));
	PrintFormatted("Total number of walls",        m_systemStructure.GetNumberOfSpecificObjects(TRIANGULAR_WALL));
	PrintFormatted("Total number of solid bonds",  m_systemStructure.GetNumberOfSpecificObjects(SOLID_BOND));
	PrintFormatted("Total number of liquid bonds", m_systemStructure.GetNumberOfSpecificObjects(LIQUID_BOND));

	m_out << std::endl;
}

void CConsoleSimulator::PrintModelsInfo()
{
	const auto PrintModel = [&](const CAbstractDEMModel* _model, const std::string& _message)
	{
		if (!_model) return;
		PrintFormatted(_message, _model->GetName());
		if (!_model->GetParametersStr().empty())
			m_out << "\t" << _model->GetParametersStr() << std::endl;
	};
	PrintModel(m_modelManager.GetModel(EMusenModelType::PP), "Particle-particle contacts");
	PrintModel(m_modelManager.GetModel(EMusenModelType::PW), "Particle-wall contacts");
	PrintModel(m_modelManager.GetModel(EMusenModelType::SB), "Solid bonds");
	PrintModel(m_modelManager.GetModel(EMusenModelType::LB), "Liquid bonds");
	PrintModel(m_modelManager.GetModel(EMusenModelType::EF), "External force");
}

void CConsoleSimulator::PrintGPUInfo() const
{
	m_out << " +----------- GPU Info -----------+" << std::endl;
	int nDevices;
	cudaGetDeviceCount(&nDevices);
	if(nDevices > 0)
	{
		cudaDeviceProp prop{};
		cudaGetDeviceProperties(&prop, 0);
		PrintFormatted(" GPU name", prop.name);
		PrintFormatted(" Memory clock rate [MHz]", prop.memoryClockRate / 1000);
		PrintFormatted(" Memory bus width [bits]", prop.memoryBusWidth);
		PrintFormatted(" Peak memory bandwidth [GB/s]", 2.0 * prop.memoryClockRate * (prop.memoryBusWidth / 8) / 1.0e6);
		PrintFormatted(" Cuda blocks number", CCUDADefines::GetSettings(0).first);
		PrintFormatted(" Cuda threads per block", CCUDADefines::GetSettings(0).second);
	}
	else
		PrintFormatted("\tNo suitable GPU devices found");
	m_out << " +--------------------------------+" << std::endl;
}

template <typename ... Args>
void CConsoleSimulator::PrintFormatted(const std::string& _message, Args... args) const
{
	// length of the message
	static int length = 38;
	// justify text
	m_out.setf(std::ios::left, std::ios::adjustfield);
	// print the message
	m_out << std::setw(length) << _message + ":";
	// print all parameters
	using expander = int[];
	(void)expander { 0, (void(m_out << ' ' << std::forward<Args>(args)), 0)... };
	// finish with eol
	m_out << std::endl;
}
