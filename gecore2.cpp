///////////////////////////////////////////////////////////////////////////////
///
///	\file    gecore2.cpp
///	\author  Paul Ullrich
///	\version March 7, 2014
///
///	<remarks>
///		Copyright 2000-2014 Paul Ullrich
///
///		This file is distributed as part of the Tempest source code package.
///		Permission is granted to use, copy, modify and distribute this
///		source code and its documentation under the terms of the GNU General
///		Public License.  This software is provided "as is" without express
///		or implied warranty.
///	</remarks>

#include "Announce.h"
#include "CommandLine.h"
#include "Exception.h"
#include "GridElements.h"
#include "OverlapMesh.h"
#include "DataMatrix3D.h"
#include "FiniteElementTools.h"
#include "SparseMatrix.h"

#include "OfflineMap.h"
#include "LinearRemapSE0.h"
#include "LinearRemapFV.h"

#include "netcdfcpp.h"
#include <cmath>

///////////////////////////////////////////////////////////////////////////////

void ParseVariableList(
	const std::string & strVariables,
	std::vector< std::string > & vecVariableStrings
) {
	int iVarBegin = 0;
	int iVarCurrent = 0;

	// Parse variable name
	for (;;) {
		if ((iVarCurrent >= strVariables.length()) ||
			(strVariables[iVarCurrent] == ',') ||
			(strVariables[iVarCurrent] == ' ')
		) {
			if (iVarCurrent == iVarBegin) {
				if (iVarCurrent >= strVariables.length()) {
					break;
				}

				continue;
			}

			vecVariableStrings.push_back(
				strVariables.substr(iVarBegin, iVarCurrent - iVarBegin));

			iVarBegin = iVarCurrent + 1;
		}

		iVarCurrent++;
	}
}

///////////////////////////////////////////////////////////////////////////////

void LoadMetaDataFile(
	const std::string & strMetaFile,
	DataMatrix3D<int> & dataGLLNodes,
	DataMatrix3D<double> & dataGLLJacobian
) {
	NcFile ncMeta(strMetaFile.c_str(), NcFile::ReadOnly);

	NcDim * dimNp = ncMeta.get_dim("np");
	NcDim * dimNelem = ncMeta.get_dim("nelem");

	NcVar * varGLLNodes = ncMeta.get_var("GLLnodes");
	NcVar * varGLLJacobian = ncMeta.get_var("J");

	int nP = dimNp->size();
	int nElem = dimNelem->size();

	dataGLLNodes.Initialize(nP, nP, nElem);
	dataGLLJacobian.Initialize(nP, nP, nElem);

	varGLLNodes->get(&(dataGLLNodes[0][0][0]), nP, nP, nElem);
	varGLLJacobian->get(&(dataGLLJacobian[0][0][0]), nP, nP, nElem);
}

///////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv) {

try {

	// Method
	std::string strMethod;

	// Input mesh file
	std::string strInputMesh;

	// Metadata file
	std::string strMetaFile;

	// Input data is spectral element
	bool fInputSE;

	// Output data is spectral element
	bool fOutputSE;

	// Order of polynomial in each element
	int nP;

	// Order of polynomial in each output element
	int nPout;

	// Use bubble on interior of spectral element nodes
	bool fBubble;

	// Enforce monotonicity
	bool fMonotone;

	// Turn off checking for conservation / consistency
	bool fNoCheck;

	// Output mesh file
	std::string strOutputMesh;

	// Overlap mesh file
	std::string strOverlapMesh;

	// Variable list
	std::string strVariables;

	// Output map file
	std::string strOutputMap;

	// Input data file
	std::string strInputData;

	// Output data file
	std::string strOutputData;

	// Name of the ncol variable
	std::string strNColName;

	// Parse the command line
	BeginCommandLine()
		//CommandLineStringD(strMethod, "method", "", "[se]");
		CommandLineString(strInputMesh, "in_mesh", "");
		CommandLineString(strOutputMesh, "out_mesh", "");
		CommandLineString(strMetaFile, "in_meta", "");
		CommandLineBool(fInputSE, "in_se");
		CommandLineBool(fOutputSE, "out_se");
		CommandLineInt(nP, "np", 4);
		CommandLineInt(nPout, "out_np", 4);
		CommandLineBool(fBubble, "bubble");
		CommandLineBool(fMonotone, "mono");
		CommandLineBool(fNoCheck, "nocheck");
		CommandLineString(strOverlapMesh, "ov_mesh", "");
		CommandLineString(strVariables, "var", "");
		CommandLineString(strOutputMap, "out_map", "");
		CommandLineString(strInputData, "in_data", "");
		CommandLineString(strOutputData, "out_data", "");
		CommandLineString(strNColName, "ncol_name", "ncol");

		ParseCommandLine(argc, argv);
	EndCommandLine(argv)

	AnnounceBanner();

	// Check command line parameters
	if ((strInputData != "") && (strOutputData == "")) {
		_EXCEPTIONT("in_data specified without out_data");
	}
	if ((strInputData == "") && (strOutputData != "")) {
		_EXCEPTIONT("out_data specified without in_data");
	}

	// Parse variable list
	std::vector< std::string > vecVariableStrings;
	ParseVariableList(strVariables, vecVariableStrings);

	if ((strInputData != "") && (vecVariableStrings.size() == 0)) {
		_EXCEPTIONT("No variables specified");
	}

	// Load input mesh
	AnnounceStartBlock("Loading input mesh");
	Mesh meshInput(strInputMesh);
	meshInput.RemoveZeroEdges();
	AnnounceEndBlock(NULL);

	// Calculate Face areas
	AnnounceStartBlock("Calculating input mesh Face areas");
	double dTotalAreaInput = meshInput.CalculateFaceAreas();
	Announce("Input Mesh Geometric Area: %1.15e", dTotalAreaInput);
	AnnounceEndBlock(NULL);

	// Input mesh areas
	DataVector<double> vecInputAreas;
	if (!fInputSE) {
		vecInputAreas = meshInput.vecFaceArea;
	}

	// Load output mesh
	AnnounceStartBlock("Loading output mesh");
	Mesh meshOutput(strOutputMesh);
	meshOutput.RemoveZeroEdges();
	AnnounceEndBlock(NULL);

	// Calculate Face areas
	AnnounceStartBlock("Calculating output mesh Face areas");
	Real dTotalAreaOutput = meshOutput.CalculateFaceAreas();
	Announce("Output Mesh Geometric Area: %1.15e", dTotalAreaOutput);
	AnnounceEndBlock(NULL);

	// Output mesh areas
	DataVector<double> vecOutputAreas;
	if (!fOutputSE) {
		vecOutputAreas = meshOutput.vecFaceArea;
	}

	// Load overlap mesh
	AnnounceStartBlock("Loading overlap mesh");
	Mesh meshOverlap(strOverlapMesh);
	meshOverlap.RemoveZeroEdges();

	// Verify that overlap mesh is in the correct order
	int ixFirstFaceMax = (-1);
	for (int i = 0; i < meshOverlap.vecFirstFaceIx.size(); i++) {
		if (meshOverlap.vecFirstFaceIx[i] + 1 > ixFirstFaceMax) {
			ixFirstFaceMax = meshOverlap.vecFirstFaceIx[i] + 1;
		}
	}

	if (ixFirstFaceMax  == meshInput.faces.size()) {
		Announce("Overlap mesh primary correspondence found");

	} else if (ixFirstFaceMax == meshOutput.faces.size()) {
		Announce("Overlap mesh reverse correspondence found (reversing)");

		// Reorder overlap mesh
		meshOverlap.ExchangeFirstAndSecondMesh();

	} else {
		_EXCEPTION1("Invalid overlap mesh:\n"
			"    No correspondence found with input and output meshes (%i)",
			ixFirstFaceMax);
	}

	AnnounceEndBlock(NULL);

	// Calculate Face areas
	AnnounceStartBlock("Calculating overlap mesh Face areas");
	Real dTotalAreaOverlap = meshOverlap.CalculateFaceAreas();
	Announce("Overlap Mesh Area: %1.15e", dTotalAreaOverlap);
	AnnounceEndBlock(NULL);

	// Partial cover
	if (fabs(dTotalAreaOverlap - dTotalAreaInput) > 1.0e-10) {
		if (!fNoCheck) {
			Announce("WARNING: Significant mismatch between overlap mesh area "
				"and input mesh area.\n  Automatically enabling --nocheck");
			fNoCheck = true;
		}
	}
/*
	// Recalculate input mesh area from overlap mesh
	if (fabs(dTotalAreaOverlap - dTotalAreaInput) > 1.0e-10) {
		AnnounceStartBlock("Overlap mesh only covers a sub-area of the sphere");
		Announce("Recalculating source mesh areas");
		dTotalAreaInput = meshInput.CalculateFaceAreasFromOverlap(meshOverlap);
		Announce("New Input Mesh Geometric Area: %1.15e", dTotalAreaInput);
		AnnounceEndBlock(NULL);
	}
*/
	// Offline Map
	OfflineMap mapRemap;

	// Finite volume input / Finite volume output
	if ((!fInputSE) && (!fOutputSE)) {

		// Generate reverse node array
		meshInput.ConstructReverseNodeArray();

		// Construct OfflineMap
		AnnounceStartBlock("Calculating offline map");
		mapRemap.InitializeInputDimensionsFromFile(strInputMesh);
		mapRemap.InitializeOutputDimensionsFromFile(strOutputMesh);

		LinearRemapFVtoFV(
			meshInput, meshOutput, meshOverlap, nP, mapRemap);

	// Finite volume input / Spectral element output
	} else if ((!fInputSE) && (fOutputSE)) {
		DataMatrix3D<int> dataGLLNodes;
		DataMatrix3D<double> dataGLLJacobian;

		if (strMetaFile != "") {
			AnnounceStartBlock("Loading meta data file");
			LoadMetaDataFile(strMetaFile, dataGLLNodes, dataGLLJacobian);
			AnnounceEndBlock(NULL);

		} else {
			AnnounceStartBlock("Generating output mesh meta data");
			double dNumericalArea =
				GenerateMetaData(
					meshOutput,
					nPout,
					fBubble,
					dataGLLNodes,
					dataGLLJacobian);

			Announce("Output Mesh Numerical Area: %1.15e", dNumericalArea);
			AnnounceEndBlock(NULL);
/*
			if (fabs(dNumericalArea - dTotalAreaInput) > 1.0e-12) {
				Announce("WARNING: Significant mismatch between numerical area "
					"and geometric area");
			}
*/
		}
/*
		// Generate the unique Jacobian
		GenerateUniqueJacobian(
			dataGLLNodes,
			dataGLLJacobian,
			vecOutputAreas);
*/
		GenerateDiscontinuousJacobian(
			dataGLLJacobian,
			vecOutputAreas);

		// Generate reverse node array
		meshInput.ConstructReverseNodeArray();

		// Generate remap weights
		AnnounceStartBlock("Calculating offline map");
		mapRemap.InitializeInputDimensionsFromFile(strInputMesh);
		mapRemap.InitializeOutputDimensionsFromFile(strOutputMesh);

		LinearRemapFVtoGLL(
			meshInput,
			meshOutput,
			meshOverlap,
			dataGLLNodes,
			dataGLLJacobian,
			nP,
			mapRemap,
			fMonotone);

	// Spectral element input / Finite volume output
	} else if ((fInputSE) && (!fOutputSE)) {
		DataMatrix3D<int> dataGLLNodes;
		DataMatrix3D<double> dataGLLJacobian;

		if (strMetaFile != "") {
			AnnounceStartBlock("Loading meta data file");
			LoadMetaDataFile(strMetaFile, dataGLLNodes, dataGLLJacobian);
			AnnounceEndBlock(NULL);

		} else {
			AnnounceStartBlock("Generating input mesh meta data");
			double dNumericalArea =
				GenerateMetaData(
					meshInput,
					nP,
					fBubble,
					dataGLLNodes,
					dataGLLJacobian);

			Announce("Input Mesh Numerical Area: %1.15e", dNumericalArea);
			AnnounceEndBlock(NULL);

			if (fabs(dNumericalArea - dTotalAreaInput) > 1.0e-12) {
				Announce("WARNING: Significant mismatch between input mesh "
					"numerical area and geometric area");
			}
		}

		if (dataGLLNodes.GetSubColumns() != meshInput.faces.size()) {
			_EXCEPTIONT("Number of element does not match between metadata and "
				"input mesh");
		}

		// Generate the unique Jacobian
		GenerateUniqueJacobian(
			dataGLLNodes,
			dataGLLJacobian,
			vecInputAreas);

		// Generate offline map
		AnnounceStartBlock("Calculating offline map");
		mapRemap.InitializeInputDimensionsFromFile(strInputMesh);
		mapRemap.InitializeOutputDimensionsFromFile(strOutputMesh);

		LinearRemapSE4(
			meshInput,
			meshOutput,
			meshOverlap,
			dataGLLNodes,
			dataGLLJacobian,
			fMonotone,
			mapRemap
		);

	} else {
		_EXCEPTIONT("Not implemented");
	}

	// Verify consistency, conservation and monotonicity
	if (!fNoCheck) {
		AnnounceStartBlock("Verifying map");
		mapRemap.IsConsistent(1.0e-8);
		mapRemap.IsConservative(vecInputAreas, vecOutputAreas, 1.0e-8);

		if (fMonotone) {
			mapRemap.IsMonotone(1.0e-12);
		}
		AnnounceEndBlock(NULL);
	}

	AnnounceEndBlock(NULL);

	// Output the Offline Map
	if (strOutputMap != "") {
		AnnounceStartBlock("Writing offline map");
		mapRemap.Write(
			strOutputMap,
			meshInput.vecFaceArea,
			meshOutput.vecFaceArea);
		AnnounceEndBlock(NULL);
	}

	// Apply Offline Map to data
	if (strInputData != "") {
		AnnounceStartBlock("Applying offline map to data");
		mapRemap.Apply(
			vecInputAreas,
			meshOutput.vecFaceArea,
			strInputData,
			strOutputData,
			vecVariableStrings,
			strNColName,
			false,
			false);
		AnnounceEndBlock(NULL);
	}

	AnnounceBanner();

} catch(Exception & e) {
	Announce(e.ToString().c_str());
}
}

///////////////////////////////////////////////////////////////////////////////

