//This is going to make metro_mpi.cpp, the file that contains all the declaration of mpi functions and struct

#ifndef V3METRO_MPI_CODEGEN_H
#define V3METRO_MPI_CODEGEN_H

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// This header requires the nlohmann/json library.
// It is assumed to be available in your project's include path.
#include "json.h"

// --- Compilation Flag ---
// To exclude communication with rank 0, you can define this macro
// in your build system (e.g., with -DEXCLUDE_RANK_ZERO) or uncomment it here.
// #define EXCLUDE_RANK_ZERO

using json = nlohmann::json;

class MPICodeGenerator {
private:
    // A struct to hold all the details of a single point-to-point connection
    struct P2P_Link {
        // Receiver's info
        std::string receiver_partition_name;
        int receiver_rank;
        std::string receiver_port_name;
        int receiver_port_width;

        // Sender's info
        std::string sender_instance_name;
        int sender_rank;
        std::string sender_port_name;
    };

    // Maps a port width to the appropriate C++ data type for the struct
    std::string getCppType(int width) {
        if (width == 1) return "bool";
        if (width <= 8) return "uint8_t";
        if (width <= 16) return "uint16_t";
        if (width <= 32) return "uint32_t";
        if (width <= 64) return "uint64_t";
        return "uint64_t";  // Default for larger widths
    }

    // Maps a port width to the corresponding MPI_Datatype
    std::string getMpiType(int width) {
        if (width == 1) return "MPI_C_BOOL";
        if (width <= 8) return "MPI_UINT8_T";
        if (width <= 16) return "MPI_UINT16_T";
        if (width <= 32) return "MPI_UINT32_T";
        if (width <= 64) return "MPI_UINT64_T";
        return "MPI_UINT64_T";
    }

public:
    // Main function to generate the MPI source file from a JSON report
    void generateMpiVerificationFile(const std::string& jsonFilePath) {

        std::ifstream inputFile(jsonFilePath);
        if (!inputFile.is_open()) {
            std::cerr << "Error [MPICodeGenerator]: Could not open file " << jsonFilePath
                      << std::endl;
            return;
        }

        json data;
        try {
            data = json::parse(inputFile);
        } catch (json::parse_error& e) {
            std::cerr << "JSON parsing error [MPICodeGenerator]: " << e.what() << std::endl;
            return;
        }

        // --- Data Structure to store all P2P links ---
        std::map<std::pair<int, int>, std::vector<P2P_Link>> communication_graph;
        std::set<std::tuple<int, int, std::string, std::string>> processed_physical_links;

        // --- PHASE 1: Populate the data structure from the JSON file ---
        for (auto const& [partitionName, ports] : data["partitions"].items()) {
            for (const auto& port : ports) {
                if (port["active"] == "Yes"
                    && (port["Comm"] == "P2P" || port["Comm"] == "broadcast")) {
                    for (const auto& commPartner : port["with_whom_is_it_communicating"]) {
                        int current_rank = port["mpi_rank"];
                        int partner_rank = commPartner["mpi_rank"];

#ifdef EXCLUDE_RANK_ZERO
                        if (current_rank == 0 || partner_rank == 0) { continue; }
#endif

                        std::string current_port_name = port["port_name"];
                        std::string partner_port_name = commPartner["port"];

                        std::tuple<int, int, std::string, std::string> physical_link_key;
                        if (current_rank < partner_rank) {
                            physical_link_key = {current_rank, partner_rank, current_port_name,
                                                 partner_port_name};
                        } else {
                            physical_link_key = {partner_rank, current_rank, partner_port_name,
                                                 current_port_name};
                        }

                        if (processed_physical_links.count(physical_link_key)) { continue; }
                        processed_physical_links.insert(physical_link_key);

                        P2P_Link new_link;
                        std::string direction = port["direction"];

                        if (direction == "in" || direction == "Input") {
                            new_link.receiver_partition_name = partitionName;
                            new_link.receiver_rank = current_rank;
                            new_link.receiver_port_name = current_port_name;
                            new_link.receiver_port_width = port["width"];
                            new_link.sender_instance_name = commPartner["instance"];
                            new_link.sender_rank = partner_rank;
                            new_link.sender_port_name = partner_port_name;
                        } else {
                            new_link.sender_instance_name = partitionName;
                            new_link.sender_rank = current_rank;
                            new_link.sender_port_name = current_port_name;
                            new_link.receiver_partition_name = commPartner["instance"];
                            new_link.receiver_rank = partner_rank;
                            new_link.receiver_port_name = commPartner["port"];
                            new_link.receiver_port_width = port["width"];
                        }

                        std::pair<int, int> key = {new_link.sender_rank, new_link.receiver_rank};
                        communication_graph[key].push_back(new_link);
                    }
                }
            }
        }

        // --- PHASE 2: Generate the C++ MPI code file ---
        std::ofstream outputFile("metro_mpi/metro_mpi.cpp");
        outputFile << "// Generated by Metro-MPI Tool\n\n";
        outputFile << "#include <mpi.h>\n";
        outputFile << "#include <cstdint>\n";
        outputFile << "#include <cstddef>\n";
        // *** FIX: Add iostream and using declarations for cout/endl ***
        outputFile << "#include <iostream>\n\n";
        outputFile << "using std::cout;\n";
        outputFile << "using std::endl;\n\n";

        // Generate structs and MPI Datatype variables
        for (const auto& [ranks, links] : communication_graph) {
            if (links.empty()) continue;
            outputFile << "// Struct for communication from rank " << ranks.first << " to "
                       << ranks.second << "\n";
            outputFile << "struct mpi_rank_" << ranks.first << "_to_" << ranks.second << "_t {\n";
            for (const auto& link : links) {
                outputFile << "    " << getCppType(link.receiver_port_width) << " "
                           << link.sender_port_name << "; // -> maps to receiver port "
                           << link.receiver_port_name << "\n";
            }
            outputFile << "};\n\n";
            outputFile << "MPI_Datatype mpi_type_rank_" << ranks.first << "_to_" << ranks.second
                       << ";\n\n";
        }

        // Generate the initialize_mpi_types function
        outputFile << "\nvoid initialize_mpi_types() {\n";
        for (const auto& [ranks, links] : communication_graph) {
            if (links.empty()) continue;
            std::string structName = "mpi_rank_" + std::to_string(ranks.first) + "_to_"
                                     + std::to_string(ranks.second) + "_t";
            std::string mpiTypeName = "mpi_type_rank_" + std::to_string(ranks.first) + "_to_"
                                      + std::to_string(ranks.second);

            outputFile << "    {\n";
            outputFile << "        const int nitems = " << links.size() << ";\n";
            outputFile << "        int blocklengths[" << links.size() << "] = {";
            for (size_t i = 0; i < links.size(); ++i)
                outputFile << "1" << (i == links.size() - 1 ? "" : ", ");
            outputFile << "};\n";

            outputFile << "        MPI_Datatype types[" << links.size() << "] = {";
            for (size_t i = 0; i < links.size(); ++i)
                outputFile << getMpiType(links[i].receiver_port_width)
                           << (i == links.size() - 1 ? "" : ", ");
            outputFile << "};\n";

            outputFile << "        MPI_Aint offsets[" << links.size() << "];\n";
            for (size_t i = 0; i < links.size(); ++i) {
                outputFile << "        offsets[" << i << "] = offsetof(" << structName << ", "
                           << links[i].sender_port_name << ");\n";
            }

            outputFile << "        MPI_Type_create_struct(nitems, blocklengths, offsets, types, &"
                       << mpiTypeName << ");\n";
            outputFile << "        MPI_Type_commit(&" << mpiTypeName << ");\n";
            outputFile << "    }\n";
        }
        outputFile << "}\n\n";

        // Generate specific send/receive functions for each link
        for (const auto& [ranks, links] : communication_graph) {
            if (links.empty()) continue;
            std::string structName = "mpi_rank_" + std::to_string(ranks.first) + "_to_"
                                     + std::to_string(ranks.second) + "_t";
            std::string mpiTypeName = "mpi_type_rank_" + std::to_string(ranks.first) + "_to_"
                                      + std::to_string(ranks.second);

            outputFile << "extern void mpi_send_rank_" << ranks.first << "_to_" << ranks.second
                       << "(" << structName << " message) {\n";
            outputFile << "    MPI_Send(&message, 1, " << mpiTypeName << ", " << ranks.second
                       << ", 0, MPI_COMM_WORLD);\n";
            outputFile << "}\n\n";

            // *** FIX: Make receive function name unique by including the receiver's rank ***
            outputFile << "extern " << structName << " mpi_receive_from_rank_" << ranks.first
                       << "_to_" << ranks.second << "() {\n";
            outputFile << "    " << structName << " message;\n";
            outputFile << "    MPI_Recv(&message, 1, " << mpiTypeName << ", " << ranks.first
                       << ", 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);\n";
            outputFile << "    return message;\n";
            outputFile << "}\n\n";
        }

        // Add final MPI lifecycle functions
        outputFile << "int getRank()\n";
        outputFile << "{\n";
        outputFile << "    int rank;\n";
        outputFile << "    MPI_Comm_rank(MPI_COMM_WORLD, &rank);\n";
        outputFile << "    return rank;\n";
        outputFile << "}\n\n";

        outputFile << "int getSize()\n";
        outputFile << "{\n";
        outputFile << "    int size;\n";
        outputFile << "    MPI_Comm_size(MPI_COMM_WORLD, &size);\n";
        outputFile << "    return size;\n";
        outputFile << "}\n\n";

        outputFile << "extern void mpi_initialize() {\n";
        outputFile << "    MPI_Init(NULL, NULL);\n";
        outputFile << "    initialize_mpi_types();\n";
        outputFile << "}\n\n";

        outputFile << "extern void mpi_finalize() {\n";
        outputFile << "    cout << \"Ending Communication from Rank \" << getRank() << endl;\n";
        outputFile << "    MPI_Finalize();\n";
        outputFile << "}\n";

        outputFile.close();
        std::cout << "\n[Metro-MPI] Successfully generated metro_mpi/metro_mpi.cpp" << std::endl;
    }
};

#endif  // V3METRO_MPI_CODEGEN_H
