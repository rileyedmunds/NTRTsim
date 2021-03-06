/*
 * Copyright © 2012, United States Government, as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All rights reserved.
 * 
 * The NASA Tensegrity Robotics Toolkit (NTRT) v1 platform is licensed
 * under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0.
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language
 * governing permissions and limitations under the License.
*/

/**
 * @file JSONSegmentsFeedbackControl.cpp
 * @brief A controller for the template class BaseSpineModelLearning. 
 * @Includes more metrics, such as center of mass of entire structure.
 * @author Brian Mirletz, Dawn Hustig-Schultz
 * @version 1.1.0
 * $Id$
 */

#include "JSONSegmentsFeedbackControl.h"


// Should include tgString, but compiler complains since its been
// included from BaseSpineModelLearning. Perhaps we should move things
// to a cpp over there
#include "core/tgSpringCableActuator.h"
#include "core/tgBasicActuator.h"
#include "controllers/tgImpedanceController.h"
#include "examples/learningSpines/tgCPGActuatorControl.h"
#include "dev/CPG_feedback/tgCPGCableControl.h"

#include "dev/dhustigschultz/BigPuppy_SpineOnly_Stats/BaseQuadModelLearning.h"
#include "helpers/FileHelpers.h"

#include "learning/AnnealEvolution/AnnealEvolution.h"
#include "learning/Configuration/configuration.h"

#include "dev/CPG_feedback/CPGEquationsFB.h"
#include "dev/CPG_feedback/CPGNodeFB.h"

#include "neuralNet/Neural Network v2/neuralNetwork.h"

#include <json/json.h>

#include <string>
#include <iostream>
#include <vector>

//#define LOGGING
#define USE_KINEMATIC

using namespace std;

JSONSegmentsFeedbackControl::Config::Config(int ss,
                                        int tm,
                                        int om,
                                        int param,
                                        int segnum,
                                        double ct,
                                        double la,
                                        double ha,
                                        double lp,
                                        double hp,
                                        double kt,
                                        double kp,
                                        double kv,
                                        bool def,
                                        double cl,
                                        double lf,
                                        double hf,
                                        double ffMin,
                                        double ffMax,
                                        double afMin,
                                        double afMax,
                                        double pfMin,
                                        double pfMax,
					double maxH,
					double minH,
					int ohm,
					int thm,
					int olm,
					int tlm) :
JSONQuadCPGControl::Config::Config(ss, tm, om, param, segnum, ct, la, ha,
                                    lp, hp, kt, kp, kv, def, cl, lf, hf),
freqFeedbackMin(ffMin),
freqFeedbackMax(ffMax),
ampFeedbackMin(afMin),
ampFeedbackMax(afMax),
phaseFeedbackMin(pfMin),
phaseFeedbackMax(pfMax),
maxHeight(maxH),
minHeight(minH),
ourHipMuscles(ohm),
theirHipMuscles(thm),
ourLegMuscles(olm),
theirLegMuscles(tlm)
{
    
}
/**
 * Defining the adapters here assumes the controller is around and
 * attached for the lifecycle of the learning runs. I.E. that the setup
 * and teardown functions are used for tgModel
 */
JSONSegmentsFeedbackControl::JSONSegmentsFeedbackControl(JSONSegmentsFeedbackControl::Config config,	
                                                std::string args,
                                                std::string resourcePath) :
JSONQuadCPGControl(config, args, resourcePath),
m_config(config)
{
    // Path and filename handled by base class
    
}

JSONSegmentsFeedbackControl::~JSONSegmentsFeedbackControl()
{
    delete nn;
}

void JSONSegmentsFeedbackControl::onSetup(BaseQuadModelLearning& subject)
{
    m_pCPGSys = new CPGEquationsFB(5000);

    Json::Value root; // will contains the root value after parsing.
    Json::Reader reader;

    bool parsingSuccessful = reader.parse( FileHelpers::getFileString(controlFilename.c_str()), root );
    if ( !parsingSuccessful )
    {
        // report to the user the failure and their locations in the document.
        std::cout << "Failed to parse configuration\n"
            << reader.getFormattedErrorMessages();
        throw std::invalid_argument("Bad filename for JSON");
    }
    // Get the value of the member of root named 'encoding', return 'UTF-8' if there is no
    // such member.
    Json::Value nodeVals = root.get("nodeVals", "UTF-8");
    Json::Value edgeVals = root.get("edgeVals", "UTF-8");
    //New sets of -Vals for hips/shoulders and legs:
    Json::Value hipEdgeVals = root.get("hipEdgeVals", "UTF-8");
    Json::Value legEdgeVals = root.get("legEdgeVals", "UTF-8");
    
    std::cout << nodeVals << std::endl;
    
    nodeVals = nodeVals.get("params", "UTF-8");
    edgeVals = edgeVals.get("params", "UTF-8");
    //New sets of -Vals for hips/shoulders and legs:
    hipEdgeVals = hipEdgeVals.get("params", "UTF-8");
    legEdgeVals = legEdgeVals.get("params", "UTF-8");
    
    array_4D edgeParams = scaleEdgeActions(edgeVals,m_config.theirMuscles,m_config.ourMuscles);
    //New sets of Params for hips/shoulders and legs:
    array_4D hipEdgeParams = scaleEdgeActions(hipEdgeVals,m_config.theirHipMuscles,m_config.ourHipMuscles);
    array_4D legEdgeParams = scaleEdgeActions(legEdgeVals,m_config.theirLegMuscles,m_config.ourLegMuscles);

    array_2D nodeParams = scaleNodeActions(nodeVals);

    setupCPGs(subject, nodeParams, edgeParams, hipEdgeParams, legEdgeParams);
    
    Json::Value feedbackParams = root.get("feedbackVals", "UTF-8");
    feedbackParams = feedbackParams.get("params", "UTF-8");
    
    // Setup neural network
    m_config.numStates = feedbackParams.get("numStates", "UTF-8").asInt();
    m_config.numActions = feedbackParams.get("numActions", "UTF-8").asInt();
    //m_config.numHidden = feedbackParams.get("numHidden", "UTF-8").asInt();
    
    std::string nnFile = controlFilePath + feedbackParams.get("neuralFilename", "UTF-8").asString();
    
    nn = new neuralNetwork(m_config.numStates, m_config.numStates*2, m_config.numActions);
    
    nn->loadWeights(nnFile.c_str());
    
    initConditions = subject.getSegmentCOM(m_config.segmentNumber);
    for (int i = 0; i < initConditions.size(); i++)
    {
        std::cout << initConditions[i] << " ";
    }
    std::cout << std::endl;
#ifdef LOGGING // Conditional compile for data logging    
    m_dataObserver.onSetup(subject);
#endif    
    
#if (0) // Conditional Compile for debug info
    std::cout << *m_pCPGSys << std::endl;
#endif    
    m_updateTime = 0.0;
    m_totalTime = 0.0; //For metrics. 
    bogus = false;

    metrics.clear();

    //Getting the center of mass of the entire structure. 
    std::vector<double> structureCOM = subject.getCOM(m_config.segmentNumber);

    for(std::size_t i=0; i<3; i++)
    {
	metrics.push_back(structureCOM[i]);
    }
    
    //"metrics" is a new section of the controller's JSON file that is 
    //added in the getNewFile function in evolution_job_master.py 
    Json::Value prevMetrics = root.get("metrics", Json::nullValue);

    Json::Value subMetrics;
    subMetrics["initial COM x"] = metrics[0];
    subMetrics["initial COM y"] = metrics[1];
    subMetrics["initial COM z"] = metrics[2];
    
    prevMetrics.append(subMetrics);
    root["metrics"] = prevMetrics;
    
    ofstream payloadLog;
    payloadLog.open(controlFilename.c_str(),ofstream::out);
    
    payloadLog << root << std::endl;
}

void JSONSegmentsFeedbackControl::onStep(BaseQuadModelLearning& subject, double dt)
{
    m_updateTime += dt;
    m_totalTime += dt;
    if (m_updateTime >= m_config.controlTime)
    {
#if (1)
        std::vector<double> desComs = getFeedback(subject);

#else        
        std::size_t numControllers = subject.getNumberofMuslces() * 3;
        
        double descendingCommand = 0.0;
        std::vector<double> desComs (numControllers, descendingCommand);
#endif       
        try
        {
            m_pCPGSys->update(desComs, m_updateTime);
        }
        catch (std::runtime_error& e)
        {
            //  Stops the trial immediately,  lets teardown know it broke
            bogus = true;
            throw (e);
        }
        
#ifdef LOGGING // Conditional compile for data logging        
        m_dataObserver.onStep(subject, m_updateTime);
#endif
		notifyStep(m_updateTime);
        m_updateTime = 0;
    }
    
    double currentHeight = subject.getSegmentCOM(m_config.segmentNumber)[1];
    
    /// Max and min heights added to config
    if (currentHeight > m_config.maxHeight || currentHeight < m_config.minHeight)
    {
		/// @todo if bogus, stop trial (reset simulation)
		bogus = true;
		throw std::runtime_error("Height out of range");
    }
    //every 100 steps, get the COM and tensions of active muscles and store them in the JSON file.
    if(1){
	    static int count = 0;
	    if(count > 100) {
		std::cout << m_totalTime << std::endl;

		//Getting the center of mass of the entire structure. 
		std::vector<double> structureCOM = subject.getCOM(m_config.segmentNumber);
		std::cout  << "COM: " << structureCOM[0] << " " << structureCOM[1] << " " << structureCOM[2] << " "; 
	    	std::cout << std::endl;
		//Clear the metrics vector for ease of adding tensions. 
		//metrics.clear();

		//Print out all spine tensions and lengths: 
		std::vector<tgSpringCableActuator* > tmpStrings = subject.find<tgSpringCableActuator> ("spine ");

		for(std::size_t i=0; i<tmpStrings.size(); i++)
		{
		    std::cout << "Spine Muscle Tension " << i << ": " << tmpStrings[i]->getTension() << std::endl;
		}
		std::cout << std::endl;
		
		for(std::size_t i=0; i<tmpStrings.size(); i++)
		{
		    std::cout << "Spine Muscle Length " << i << ": " << tmpStrings[i]->getCurrentLength() << std::endl;
		}
		std::cout << std::endl;

		//Print out all hip tensions and lengths:
		std::vector<tgSpringCableActuator* > tmpHipStrings = subject.find<tgSpringCableActuator> ("hip ");

		for(std::size_t i=0; i<tmpHipStrings.size(); i++)
		{
		    std::cout << "Hip Muscle Tension " << i << ": " << tmpHipStrings[i]->getTension() << std::endl;
		}
		std::cout << std::endl;

		for(std::size_t i=0; i<tmpHipStrings.size(); i++)
		{
		    std::cout << "Hip Muscle Length " << i << ": " << tmpHipStrings[i]->getCurrentLength() << std::endl;
		}
		std::cout << std::endl;

		//Print out all leg tensions and lengths:
		std::vector<tgSpringCableActuator* > tmpLegStrings = subject.find<tgSpringCableActuator> ("leg ");

		for(std::size_t i=0; i<tmpLegStrings.size(); i++)
		{
		    std::cout << "Leg Muscle Tension " << i << ": " << tmpLegStrings[i]->getTension() << std::endl;
		}
		std::cout << std::endl;

		for(std::size_t i=0; i<tmpLegStrings.size(); i++)
		{
		    std::cout << "Leg Muscle Length " << i << ": " << tmpLegStrings[i]->getCurrentLength() << std::endl;
		}
		std::cout << std::endl;

		count = 0;
	    }
	    else {
		count++;
	    }
    }
}

void JSONSegmentsFeedbackControl::onTeardown(BaseQuadModelLearning& subject)
{
    scores.clear();
    metrics.clear();
    // @todo - check to make sure we ran for the right amount of time
    
    std::vector<double> finalConditions = subject.getSegmentCOM(m_config.segmentNumber);
    
    const double newX = finalConditions[0];
    const double newZ = finalConditions[2];
    const double oldX = initConditions[0];
    const double oldZ = initConditions[2];
    
    const double distanceMoved = sqrt((newX-oldX) * (newX-oldX) + 
                                        (newZ-oldZ) * (newZ-oldZ));
    
    if (bogus)
    {
        scores.push_back(-1.0);
    }
    else
    {
        scores.push_back(distanceMoved);
    }
    
    /// @todo - consolidate with other controller classes. 
    /// @todo - return length scale as a parameter
    double totalEnergySpent=0;
    
    //Calculating total enery for spine:
    std::vector<tgSpringCableActuator* > tmpStrings = subject.find<tgSpringCableActuator> ("spine ");
    
    for(std::size_t i=0; i<tmpStrings.size(); i++)
    {
        tgSpringCableActuator::SpringCableActuatorHistory stringHist = tmpStrings[i]->getHistory();
        
        for(std::size_t j=1; j<stringHist.tensionHistory.size(); j++)
        {
            const double previousTension = stringHist.tensionHistory[j-1];
            const double previousLength = stringHist.restLengths[j-1];
            const double currentLength = stringHist.restLengths[j];
            //TODO: examine this assumption - free spinning motor may require more power
            double motorSpeed = (currentLength-previousLength);
            if(motorSpeed > 0) // Vestigial code
                motorSpeed = 0;
            const double workDone = previousTension * motorSpeed;
            totalEnergySpent += workDone;
        }
    }
    
    //Repeating the process for hips:
    std::vector<tgSpringCableActuator* > tmpHipStrings = subject.find<tgSpringCableActuator> ("hip ");
    
    for(std::size_t i=0; i<tmpHipStrings.size(); i++)
    {
        tgSpringCableActuator::SpringCableActuatorHistory stringHipHist = tmpHipStrings[i]->getHistory();
        
        for(std::size_t j=1; j<stringHipHist.tensionHistory.size(); j++)
        {
            const double previousTension = stringHipHist.tensionHistory[j-1];
            const double previousLength = stringHipHist.restLengths[j-1];
            const double currentLength = stringHipHist.restLengths[j];
            //TODO: examine this assumption - free spinning motor may require more power
            double motorSpeed = (currentLength-previousLength);
            if(motorSpeed > 0) // Vestigial code
                motorSpeed = 0;
            const double workDone = previousTension * motorSpeed;
            totalEnergySpent += workDone;
        }
    }
    
    //Repeating the process for legs:
    std::vector<tgSpringCableActuator* > tmpLegStrings = subject.find<tgSpringCableActuator> ("leg ");
    
    for(std::size_t i=0; i<tmpLegStrings.size(); i++)
    {
        tgSpringCableActuator::SpringCableActuatorHistory stringLegHist = tmpLegStrings[i]->getHistory();
        
        for(std::size_t j=1; j<stringLegHist.tensionHistory.size(); j++)
        {
            const double previousTension = stringLegHist.tensionHistory[j-1];
            const double previousLength = stringLegHist.restLengths[j-1];
            const double currentLength = stringLegHist.restLengths[j];
            //TODO: examine this assumption - free spinning motor may require more power
            double motorSpeed = (currentLength-previousLength);
            if(motorSpeed > 0) // Vestigial code
                motorSpeed = 0;
            const double workDone = previousTension * motorSpeed;
            totalEnergySpent += workDone;
        }
    }
    
    scores.push_back(totalEnergySpent);

    //Getting the center of mass of the entire structure. 
    std::vector<double> structureCOM = subject.getCOM(m_config.segmentNumber);

    for(std::size_t i=0; i<3; i++)
    {
	metrics.push_back(structureCOM[i]);
    }
    
    std::cout << "Dist travelled " << scores[0] << std::endl;
    
    Json::Value root; // will contain the root value after parsing.
    Json::Reader reader;

    bool parsingSuccessful = reader.parse( FileHelpers::getFileString(controlFilename.c_str()), root );
    if ( !parsingSuccessful )
    {
        // report to the user the failure and their locations in the document.
        std::cout << "Failed to parse configuration\n"
            << reader.getFormattedErrorMessages();
        throw std::invalid_argument("Bad filename for JSON");
    }
    
    Json::Value prevScores = root.get("scores", Json::nullValue);
    Json::Value prevMetrics = root.get("metrics", Json::nullValue);
    
    Json::Value subScores;
    subScores["distance"] = scores[0];
    subScores["energy"] = scores[1];

    Json::Value subMetrics;
    subMetrics["final COM x"] = metrics[0];
    subMetrics["final COM y"] = metrics[1];
    subMetrics["final COM z"] = metrics[2];
    
    prevScores.append(subScores);
    prevMetrics.append(subMetrics);

    root["scores"] = prevScores;
    root["metrics"] = prevMetrics;
    
    ofstream payloadLog;
    payloadLog.open(controlFilename.c_str(),ofstream::out);
    
    payloadLog << root << std::endl;
    
    delete m_pCPGSys;
    m_pCPGSys = NULL;
    
    for(size_t i = 0; i < m_spineControllers.size(); i++)
    {
        delete m_spineControllers[i];
    }
    m_spineControllers.clear();    
}

void JSONSegmentsFeedbackControl::setupCPGs(BaseQuadModelLearning& subject, array_2D nodeActions, array_4D edgeActions, array_4D hipEdgeActions, array_4D legEdgeActions)
{
	    
    std::vector <tgSpringCableActuator*> spineMuscles = subject.find<tgSpringCableActuator> ("spine ");
    std::vector <tgSpringCableActuator*> hipMuscles = subject.find<tgSpringCableActuator> ("hip ");
    //std::vector <tgSpringCableActuator*> legMuscles = subject.find<tgSpringCableActuator> ("legAct ");
    
    CPGEquationsFB& m_CPGFBSys = *(tgCast::cast<CPGEquations, CPGEquationsFB>(m_pCPGSys));
    
    for (std::size_t i = 0; i < spineMuscles.size(); i++)
    {

        tgPIDController::Config config(20000.0, 0.0, 5.0, true); // Non backdrivable
        tgCPGCableControl* pStringControl = new tgCPGCableControl(config);

        spineMuscles[i]->attach(pStringControl);
        
        // First assign node numbers
        pStringControl->assignNodeNumberFB(m_CPGFBSys, nodeActions);
        
        m_spineControllers.push_back(pStringControl);
    }
    
    // Then determine connectivity and setup string
    for (std::size_t i = 0; i < m_spineControllers.size(); i++)
    {
        tgCPGActuatorControl * const pStringInfo = m_spineControllers[i];
        assert(pStringInfo != NULL);
        pStringInfo->setConnectivity(m_spineControllers, edgeActions);
        
        //String will own this pointer
        tgImpedanceController* p_ipc = new tgImpedanceController( m_config.tension,
                                                        m_config.kPosition,
                                                        m_config.kVelocity);
        if (m_config.useDefault)
        {
			pStringInfo->setupControl(*p_ipc);
		}
		else
		{
			pStringInfo->setupControl(*p_ipc, m_config.controlLength);
		}
    }
    
    for (std::size_t i = 0; i < hipMuscles.size(); i++)
    {

        tgPIDController::Config config(20000.0, 0.0, 5.0, true); // Non backdrivable
        tgCPGCableControl* pStringControl = new tgCPGCableControl(config);

        hipMuscles[i]->attach(pStringControl);
        
        // First assign node numbers
        pStringControl->assignNodeNumberFB(m_CPGFBSys, nodeActions);
        
        m_hipControllers.push_back(pStringControl);
    }
    
    // Then determine connectivity and setup string
    for (std::size_t i = 0; i < m_hipControllers.size(); i++)
    {
        tgCPGActuatorControl * const pStringInfo = m_hipControllers[i];
        assert(pStringInfo != NULL);
        pStringInfo->setConnectivity(m_hipControllers, hipEdgeActions);
        
        //String will own this pointer
        tgImpedanceController* p_ipc = new tgImpedanceController( m_config.tension,
                                                        m_config.kPosition,
                                                        m_config.kVelocity);
        if (m_config.useDefault)
        {
			pStringInfo->setupControl(*p_ipc);
		}
		else
		{
			pStringInfo->setupControl(*p_ipc, m_config.controlLength);
		}
    }

    /*for (std::size_t i = 0; i < legMuscles.size(); i++)
    {

        tgPIDController::Config config(20000.0, 0.0, 5.0, true); // Non backdrivable
        tgCPGCableControl* pStringControl = new tgCPGCableControl(config);

        legMuscles[i]->attach(pStringControl);
        
        // First assign node numbers
        pStringControl->assignNodeNumberFB(m_CPGFBSys, nodeActions);
        
        m_legControllers.push_back(pStringControl);
    }
    
    // Then determine connectivity and setup string
    for (std::size_t i = 0; i < m_legControllers.size(); i++)
    {
        tgCPGActuatorControl * const pStringInfo = m_legControllers[i];
        assert(pStringInfo != NULL);
        pStringInfo->setConnectivity(m_legControllers, legEdgeActions);
        
        //String will own this pointer
        tgImpedanceController* p_ipc = new tgImpedanceController( m_config.tension,
                                                        m_config.kPosition,
                                                        m_config.kVelocity);
        if (m_config.useDefault)
        {
			pStringInfo->setupControl(*p_ipc);
		}
		else
		{
			pStringInfo->setupControl(*p_ipc, m_config.controlLength);
		}
    }*/
	
}

array_4D JSONSegmentsFeedbackControl::scaleEdgeActions  
                            (Json::Value edgeParam, int theirMuscles, int ourMuscles)
{
    assert(edgeParam[0].size() == 2);
    
    double lowerLimit = m_config.lowPhase;
    double upperLimit = m_config.highPhase;
    double range = upperLimit - lowerLimit;
    
    array_4D actionList(boost::extents[m_config.segmentSpan][theirMuscles][ourMuscles][m_config.params]);
    
    /* Horrid while loop to populate upper diagonal of matrix, since
    * its symmetric and we want to minimze parameters used in learing
    * note that i==1, j==k will refer to the same muscle
    * @todo use boost to set up array so storage is only allocated for 
    * elements that are used
    */
    int i = 0;
    int j = 0;
    int k = 0;
    
    // Quirk of the old learning code. Future examples can move forward
    Json::Value::iterator edgeIt = edgeParam.end();
    
    int count = 0;
    
    while (i < m_config.segmentSpan)
    {
        while(j < theirMuscles)
        {
            while(k < ourMuscles)
            {
                if (edgeIt == edgeParam.begin())
                {
                    std::cout << "ran out before table populated!"
                    << std::endl;
                    /// @todo consider adding exception here
                    break;
                }
                else
                {
                    if (i == 1 && j == k)
                    {
                        // std::cout << "Skipped identical muscle" << std::endl;
                        //Skip since its the same muscle
                    }
                    else
                    {
                        edgeIt--;
                        Json::Value edgeParam = *edgeIt;
                        assert(edgeParam.size() == 2);
                        // Weight from 0 to 1
                        actionList[i][j][k][0] = edgeParam[0].asDouble();
                        //std::cout << actionList[i][j][k][0] << " ";
                        // Phase offset from -pi to pi
                        actionList[i][j][k][1] = edgeParam[1].asDouble() * 
                                                (range) + lowerLimit;
                        //std::cout <<  actionList[i][j][k][1] << std::endl;
                        count++;
                    }
                }
                k++;
            }
            j++;
            k = j;
            
        }
        j = 0;
        k = 0;
        i++;
    }
    
    std::cout<< "Params used: " << count << std::endl;
    
    assert(edgeParam.begin() == edgeIt);
    
    return actionList;
}

array_2D JSONSegmentsFeedbackControl::scaleNodeActions (Json::Value actions)
{
    std::size_t numControllers = actions.size();
    std::size_t numActions = actions[0].size();
    
    array_2D nodeActions(boost::extents[numControllers][numActions]);
    
    array_2D limits(boost::extents[2][numActions]);
    
    // Check if we need to update limits
    assert(numActions == 5);
    
	limits[0][0] = m_config.lowFreq;
	limits[1][0] = m_config.highFreq;
	limits[0][1] = m_config.lowAmp;
	limits[1][1] = m_config.highAmp;
    limits[0][2] = m_config.freqFeedbackMin;
    limits[1][2] = m_config.freqFeedbackMax;
    limits[0][3] = m_config.ampFeedbackMin;
    limits[1][3] = m_config.ampFeedbackMax;
    limits[0][4] = m_config.phaseFeedbackMin;
    limits[1][4] = m_config.phaseFeedbackMax;
    
    Json::Value::iterator nodeIt = actions.begin();
    
    // This one is square
    for( std::size_t i = 0; i < numControllers; i++)
    {
        Json::Value nodeParam = *nodeIt;
        for( std::size_t j = 0; j < numActions; j++)
        {
            nodeActions[i][j] = ( (nodeParam.get(j, 0.0)).asDouble() *  
                    (limits[1][j] - limits[0][j])) + limits[0][j];
        }
        nodeIt++;
    }
    
    return nodeActions;
}

std::vector<double> JSONSegmentsFeedbackControl::getFeedback(BaseQuadModelLearning& subject)
{
    // Placeholder
    std::vector<double> feedback;
    
    const std::vector<tgSpringCableActuator*>& spineCables = subject.find<tgSpringCableActuator> ("spine ");
    
    double *inputs = new double[m_config.numStates];
    
    std::size_t n = spineCables.size();
    for(std::size_t i = 0; i != n; i++)
    {
        std::vector< std::vector<double> > actions;
        
        const tgSpringCableActuator& cable = *(spineCables[i]);
        std::vector<double > state = getCableState(cable);
        
        // Rescale to 0 to 1 (consider doing this inside getState
        for (std::size_t i = 0; i < state.size(); i++)
        {
            inputs[i]=state[i] / 2.0 + 0.5;
        }
        
        double *output = nn->feedForwardPattern(inputs);
        vector<double> tmpAct;
        for(int j=0;j<m_config.numActions;j++)
        {
            tmpAct.push_back(output[j]);
        }
        actions.push_back(tmpAct);

        std::vector<double> cableFeedback = transformFeedbackActions(actions);
        
        feedback.insert(feedback.end(), cableFeedback.begin(), cableFeedback.end());
    }
    
    //Doing the same for the hips and legs... consider changing this to allMuscles, by including the short muscles in the spine, to shorten this function. 
    const std::vector<tgSpringCableActuator*>& hipCables = subject.find<tgSpringCableActuator> ("hip ");
    
    std::size_t n2 = hipCables.size();
    for(std::size_t i = 0; i != n2; i++)
    {
        std::vector< std::vector<double> > actions;
        
        const tgSpringCableActuator& cable = *(hipCables[i]);
        std::vector<double > state = getCableState(cable);
        
        // Rescale to 0 to 1 (consider doing this inside getState
        for (std::size_t i = 0; i < state.size(); i++)
        {
            inputs[i]=state[i] / 2.0 + 0.5;
        }
        
        double *output = nn->feedForwardPattern(inputs);
        vector<double> tmpAct;
        for(int j=0;j<m_config.numActions;j++)
        {
            tmpAct.push_back(output[j]);
        }
        actions.push_back(tmpAct);

        std::vector<double> cableFeedback = transformFeedbackActions(actions);
        
        feedback.insert(feedback.end(), cableFeedback.begin(), cableFeedback.end());
    }

    /*const std::vector<tgSpringCableActuator*>& legCables = subject.find<tgSpringCableActuator> ("legAct ");
    
    std::size_t n3 = legCables.size();
    for(std::size_t i = 0; i != n3; i++)
    {
        std::vector< std::vector<double> > actions;
        
        const tgSpringCableActuator& cable = *(legCables[i]);
        std::vector<double > state = getCableState(cable);
        
        // Rescale to 0 to 1 (consider doing this inside getState
        for (std::size_t i = 0; i < state.size(); i++)
        {
            inputs[i]=state[i] / 2.0 + 0.5;
        }
        
        double *output = nn->feedForwardPattern(inputs);
        vector<double> tmpAct;
        for(int j=0;j<m_config.numActions;j++)
        {
            tmpAct.push_back(output[j]);
        }
        actions.push_back(tmpAct);

        std::vector<double> cableFeedback = transformFeedbackActions(actions);
        
        feedback.insert(feedback.end(), cableFeedback.begin(), cableFeedback.end());
    }*/
    
    
    return feedback;
}

std::vector<double> JSONSegmentsFeedbackControl::getCableState(const tgSpringCableActuator& cable)
{
	// For each string, scale value from -1 to 1 based on initial length or max tension of motor
    
    std::vector<double> state;
    
    // Scale length by starting length
    const double startLength = cable.getStartLength();
    state.push_back((cable.getCurrentLength() - startLength) / startLength);
    
    const double maxTension = cable.getConfig().maxTens;
    state.push_back((cable.getTension() - maxTension / 2.0) / maxTension);
    
	return state;
}

std::vector<double> JSONSegmentsFeedbackControl::transformFeedbackActions(std::vector< std::vector<double> >& actions)
{
	// Placeholder
	std::vector<double> feedback;
    
    // Leave in place for generalization later
    const std::size_t numControllers = 1;
    const std::size_t numActions = m_config.numActions;
    
    assert( actions.size() == numControllers);
    assert( actions[0].size() == numActions);
    
    // Scale values back to -1 to +1
    for( std::size_t i = 0; i < numControllers; i++)
    {
        for( std::size_t j = 0; j < numActions; j++)
        {
            feedback.push_back(actions[i][j] * 2.0 - 1.0);
        }
    }
    
	return feedback;
}

