//This is going to make the modified verilog files to adapt to the Metro-MPI

#ifndef V3METRO_MPI_FILE_H
#define V3METRO_MPI_FILE_H

#include "V3Ast.h"
#include "V3AstNodeOther.h"
#include "V3Blake2b.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

class MPIFileGenerator {
public:
    template<typename PortType>
    void generateAndModifyFiles(
        const std::string& partitionModuleName,
        const std::string& partitionModuleOrigName,
        const std::map<std::string, std::vector<PortType>>& partitionData,
        const std::unordered_map<std::string, AstNodeModule*>& moduleNameToModulePtr,
        const std::string& parentModuleFilePath,
        const std::string& parentModuleName) {

        if (partitionData.empty()) {
            std::cerr << "  --> ERROR: Partition data is empty, cannot generate files.\n";
            return;
        }

        // Part 1: Generate the generic "Stub" module (this is unchanged)
        // ...
        // (The code for Part 1 is identical to the previous version and is omitted for brevity)
        // ...
        std::cout << "\n[INFO] Part 1: Generating generic DPI stub module for '" << partitionModuleOrigName << "'...\n";
        
        const auto& ports = partitionData.begin()->second;
        std::string stubModuleName = "modified_" + partitionModuleOrigName;
        std::string outStubFileName = "metro_mpi/" + stubModuleName + ".v";
        {
            std::ofstream outStubFile(outStubFileName);
            outStubFile << "`timescale 1ns / 1ps\n\n";
            outStubFile << "module " << stubModuleName << " #(\n";
            outStubFile << "  parameter integer PARTITION_ID = -1\n";
            outStubFile << ") (\n";
            for (size_t i = 0; i < ports.size(); ++i) {
                outStubFile << "  " << ports[i].name << (i == ports.size() - 1 ? "" : ",\n");
            }
            outStubFile << ");\n\n";
            for (const auto& port : ports) {
                std::string directionKeyword;
                if (port.direction == "in") directionKeyword = "input";
                else if (port.direction == "out") directionKeyword = "output";
                else if (port.direction == "input") directionKeyword = "input";
                else if (port.direction == "output") directionKeyword = "output";
                else if (port.direction == "Input") directionKeyword = "input";
                else if (port.direction == "Output") directionKeyword = "output";
                else directionKeyword = "inout";

                // ================== MODIFICATION START ==================
                // Conditionally choose data type based on port direction.
                // Inputs and inouts are nets (wire), outputs from procedural
                // blocks should be variables (reg).
                std::string dataTypeKeyword;
                if (directionKeyword == "input" || directionKeyword == "inout") {
                    dataTypeKeyword = "wire";
                } else { // output
                    dataTypeKeyword = "reg";
                }
                // =================== MODIFICATION END ===================

                if (port.width > 1) {
                    // Use the new dataTypeKeyword instead of hardcoded "logic"
                    outStubFile << "  " << directionKeyword << " " << dataTypeKeyword << " [" << port.width - 1 << ":0] " << port.name << ";\n";
                } else {
                    // Use the new dataTypeKeyword instead of hardcoded "logic"
                    outStubFile << "  " << directionKeyword << " " << dataTypeKeyword << " " << port.name << ";\n";
                }
            }
            outStubFile << "\n";
            std::stringstream dpiImportSignature;
            std::stringstream dpiFunctionCall;
            std::string dpiFunctionName = "dpi_" + partitionModuleOrigName;
            dpiImportSignature << "input int partition_id"; 
            dpiFunctionCall << "PARTITION_ID";
            for (const auto& port : ports) {
                dpiImportSignature << ", ";
                dpiFunctionCall << ", ";
                std::string dpiDataType;
                if (port.width == 1) dpiDataType = "bool";
                else if (port.width <= 32) dpiDataType = "int";
                else if (port.width <= 64) dpiDataType = "longint";
                else dpiDataType = "logic [" + std::to_string(port.width - 1) + ":0]";
                std::string directionKeyword;
                if (port.direction == "in") directionKeyword = "input";
                else if (port.direction == "out") directionKeyword = "output";
                else if (port.direction == "input") directionKeyword = "input";
                else if (port.direction == "output") directionKeyword = "output";
                else if (port.direction == "Input") directionKeyword = "input";
                else if (port.direction == "Output") directionKeyword = "output";
                else directionKeyword = "inout";
                dpiImportSignature << directionKeyword << " " << dpiDataType << " " << port.name;
                dpiFunctionCall << port.name;
            }
            outStubFile << "  import \"DPI-C\" function void " << dpiFunctionName << "(" << dpiImportSignature.str() << ");\n";
            outStubFile << "\n  always @(*) begin\n";
            outStubFile << "    " << dpiFunctionName << "(" << dpiFunctionCall.str() << ");\n";
            outStubFile << "  end\n";
            outStubFile << "endmodule\n";
            outStubFile.close();
            std::cout << "  --> Successfully wrote stub module to '" << outStubFileName << "'\n";
        }


        // Part 2: Generate unique wrapper modules (this is unchanged)
        // ...
        // (The code for Part 2 is identical to the previous version and is omitted for brevity)
        // ...
        std::cout << "\n[INFO] Part 2: Generating unique wrapper modules...\n";
        for (const auto& pair : partitionData) {
            const std::string& instanceName = pair.first;
            const auto& instancePorts = pair.second;
            int mpiRank = instancePorts.front().mpi_rank;
            std::string wrapperModuleName = instanceName + "_" + partitionModuleOrigName + "_wrapper";
            std::string wrapperFileName = "metro_mpi/" + wrapperModuleName + ".v";
            std::ofstream outWrapperFile(wrapperFileName);
            outWrapperFile << "module " << wrapperModuleName << " (\n";
             for (size_t i = 0; i < instancePorts.size(); ++i) {
                outWrapperFile << "  " << instancePorts[i].name << (i == instancePorts.size() - 1 ? "" : ",\n");
            }
            outWrapperFile << "\n);\n\n";
             for (const auto& port : instancePorts) {
                std::string directionKeyword;
                if (port.direction == "in") directionKeyword = "input";
                else if (port.direction == "out") directionKeyword = "output";
                else if (port.direction == "input") directionKeyword = "input";
                else if (port.direction == "output") directionKeyword = "output";
                else if (port.direction == "Input") directionKeyword = "input";
                else if (port.direction == "Output") directionKeyword = "output";
                else directionKeyword = "inout";

                // ================== MODIFICATION START ==================
                // Conditionally choose data type based on port direction.
                std::string dataTypeKeyword;
                if (directionKeyword == "input" || directionKeyword == "inout") {
                    dataTypeKeyword = "wire";
                } else { // output
                    dataTypeKeyword = "reg";
                }
                // =================== MODIFICATION END ===================

                if (port.width > 1) {
                    // Use the new dataTypeKeyword instead of hardcoded "logic"
                    outWrapperFile << "  " << directionKeyword << " " << dataTypeKeyword << " [" << port.width - 1 << ":0] " << port.name << ";\n";
                } else {
                    // Use the new dataTypeKeyword instead of hardcoded "logic"
                    outWrapperFile << "  " << directionKeyword << " " << dataTypeKeyword << " " << port.name << ";\n";
                }
            }
            outWrapperFile << "\n";
            outWrapperFile << "  " << stubModuleName << " #(\n";
            outWrapperFile << "    .PARTITION_ID(" << mpiRank << ")\n";
            outWrapperFile << "  ) inst (\n";
             for (size_t i = 0; i < instancePorts.size(); ++i) {
                outWrapperFile << "    ." << instancePorts[i].name << "(" << instancePorts[i].name << ")" << (i == instancePorts.size() - 1 ? "" : ",\n");
            }
            outWrapperFile << "\n  );\n";
            outWrapperFile << "endmodule\n";
            outWrapperFile.close();
            std::cout << "  --> Wrote wrapper '" << wrapperModuleName << "' for instance '" << instanceName << "' to '" << wrapperFileName << "'\n";
        }


        // =================================================================================
        // Part 3: Generate the modified parent module (MODIFIED)
        // This now uses a more robust regex to handle parameter overrides and comments.
        // =================================================================================
        std::cout << "\n[INFO] Part 3: Generating modified parent module...\n";
        std::cout << "  --> Reading original parent module from: " << parentModuleFilePath << "\n";
        std::ifstream parentFile(parentModuleFilePath);
        if (!parentFile.is_open()) {
            std::cerr << "  --> ERROR: Could not open parent module file.\n";
            return;
        }
        std::stringstream buffer;
        buffer << parentFile.rdbuf();
        std::string parentFileContent = buffer.str();
        parentFile.close();

        // Perform search-and-replace for each partition instance
        for (const auto& pair : partitionData) {
            const std::string& instanceName = pair.first;
            std::string wrapperModuleName = instanceName + "_" + partitionModuleOrigName + "_wrapper";

            // This new, more robust regex finds the module type to replace.
            // It looks for:
            //   - The original module name (as a whole word: \b)
            //   - Then captures anything (parameters, comments, whitespace) up to...
            //   - The instance name (as a whole word) followed by an opening parenthesis.
            std::regex search_regex("(\\b" + partitionModuleOrigName + "\\b)(.*?\\b" + instanceName + "\\b\\s*\\()");

            // The replacement will be:
            //   - The new wrapper module name
            //   - Followed by the captured middle part (the ".*?") and the instance name.
            std::string replace_string = wrapperModuleName + "$2";

            parentFileContent = std::regex_replace(parentFileContent, search_regex, replace_string);
        }
        
        std::string outParentFileName = "metro_mpi/modified_" + parentModuleName + ".v";
        std::ofstream outParentFile(outParentFileName);
        outParentFile << "// Modified by Metro-MPI to use specialized wrappers\n\n";
        outParentFile << parentFileContent;
        outParentFile.close();
        std::cout << "  --> Successfully wrote modified parent to '" << outParentFileName << "'\n";
    }
};

#endif // V3METRO_MPI_FILE_H