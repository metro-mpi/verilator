//This is the main file for Metro-MPI which does all the ground work and builds the stage for
//subsequent support file generation

#ifndef V3METRO_MPI_H
#define V3METRO_MPI_H

#include "V3Ast.h"
#include "V3AstNodeOther.h"
#include "V3Blake2b.h"
#include "V3MMPI_Include.h"
#include "V3MMPI_Makefile.h"
#include "V3MMPI_Verilog.h"
#include "V3MMPI_main_rank_0.h"
#include "V3MMPI_partition_sim.h"

#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

extern std::string argString;

//==================================================================================================
//
//  HELPER UTILITY FUNCTIONS
//
//==================================================================================================

/**
 * @brief Gets a standardized, canonical name for a connection expression.
 * @details This function handles simple variable references (e.g., "my_wire") and single-bit
 * selections from a bus (e.g., "my_bus[3]"). This consistent naming is crucial for
 * mapping connections between modules.
 * @param exprp Pointer to the AST node representing the connection expression.
 * @return A std::string containing the canonical name (e.g., "bus[3]"). Returns an
 * empty string for unhandled or null expressions.
 */
static std::string getCanonicalName(AstNode* exprp) VL_MT_DISABLED {
    if (!exprp) return "";
    // Case 1: Direct variable reference (e.g., "my_wire")
    if (const AstVarRef* varRef = VN_CAST(exprp, VarRef)) { return varRef->name(); }
    // Case 2: Bit-select from a bus (e.g., "my_bus[3]")
    if (const AstSelBit* selp = VN_CAST(exprp, SelBit)) {
        if (const AstVarRef* busVarRef = VN_CAST(selp->op1p(), VarRef)) {
            if (const AstConst* indexConst = VN_CAST(selp->op2p(), Const)) {
                // Construct the name as "bus_name[index]"
                return busVarRef->name() + "[" + std::to_string(indexConst->toUInt()) + "]";
            }
        }
    }
    return "";  // Return empty for unhandled expressions
}

/**
 * @brief Extracts the bit width from a Verilator DType (data type) node.
 * @details Handles both ranged types (e.g., `logic [7:0]`) and non-ranged types (e.g., `logic`),
 * which are assumed to have a width of 1.
 * @param dtp Pointer to the AST data type node.
 * @return The width of the data type in bits. Defaults to 1 for unknown or simple types.
 */
static int getDTypeWidth(const AstNodeDType* dtp) VL_MT_DISABLED {
    if (!dtp) return 1;
    dtp = dtp->skipRefp();  // Skip any type references to get to the base type
    if (const AstBasicDType* bdtp = VN_CAST(dtp, BasicDType)) {
        if (bdtp->isRanged()) {
            // Calculate width from left and right bounds (e.g., [7:0] -> 7-0+1=8)
            return bdtp->left() > bdtp->right() ? bdtp->left() - bdtp->right() + 1
                                                : bdtp->right() - bdtp->left() + 1;
        } else {
            // Non-ranged basic types (e.g. "logic", "wire") have a width of 1.
            return 1;
        }
    }
    // Default for other types like unpacked arrays, structs, etc.
    return 1;
}

/**
 * @brief Searches for a variable declaration by name within a given module scope.
 * @details This function correctly traverses all nodes within a module's definition to find
 * the 'AstVar' that corresponds to a given port or variable name.
 * @param scope The module's AST node to search within (AstNodeModule).
 * @param name The name of the variable to find.
 * @return A pointer to the AstVar node if found; otherwise, nullptr.
 */
static AstVar* findVarInModule(AstNodeModule* scope, const std::string& name) VL_MT_DISABLED {
    if (!scope) return nullptr;
    AstVar* foundVar = nullptr;
    // Use Verilator's foreach to reliably iterate through all nodes in the module.
    // This is guaranteed to find all variable declarations regardless of where they
    // are in the module's AST structure.
    scope->foreach([&](AstVar* varp) {
        if (!foundVar && varp->name() == name) { foundVar = varp; }
    });
    return foundVar;
}

/**
 * @class PartitionPortAnalyzer
 * @brief Performs a detailed analysis of the ports of specified partition instances.
 * @details This class operates on a parent module and a list of child instances designated as
 * "partitions". It traverses the AST to determine how each port of these partitions is connected,
 * who it communicates with, and its properties (direction, width). The final analysis is used
 * to generate a report suitable for MPI (Message Passing Interface) generation.
 */
class PartitionPortAnalyzer {
public:
    /**
     * @struct CommunicationPartner
     * @brief Holds detailed info about a remote connection point.
     */
    struct CommunicationPartner {
        std::string instance;
        std::string port;
        std::string mpi_process;
        int mpi_rank = -1;  // NEW: Added rank for the communication partner
    };
    /**
     * @struct Port
     * @brief A structure to hold detailed information about a single port.
     * @details This stores all analyzed attributes of a module's port, such as its name,
     * direction, width, and connectivity details.
     */
    struct Port {
        std::string name;  ///< The name of the port (e.g., "data_in").
        std::string direction;  ///< Port direction ("Input", "Output", "Inout").
        int width = 0;  ///< The bit width of the port.
        std::string active = "idk";  ///< "Yes" if connected, "No" if unconnected or tied to const.
        std::string type = "idk";  ///< Connection type ("wire", "init" for const, "logic").
        std::string other_end;  ///< Canonical name of the wire or constant value it connects to.
        std::string mpi_process = "idk";  ///< The target MPI process for communication.
        int mpi_rank = -1;  // NEW: Added rank for this port's own process
        std::string comm_type = "idk";  //communication type

        // MODIFIED: Changed to a vector of the new struct.
        std::vector<CommunicationPartner>
            with_whom_is_it_communicating;  ///< List of remote connections.

        /**
         * @brief Helper to get a string of just the communication instance names.
         * @return A formatted string, e.g., "[instance1, instance2]".
         */
        std::string getCommInstancesString() const {
            std::stringstream ss;
            ss << "[";
            for (size_t i = 0; i < with_whom_is_it_communicating.size(); ++i) {
                ss << with_whom_is_it_communicating[i].instance;  // Get instance name
                if (i < with_whom_is_it_communicating.size() - 1) { ss << ", "; }
            }
            ss << "]";
            return ss.str();
        }

        /**
         * @brief Helper to get a string of just the communication port names.
         * @return A formatted string, e.g., "[portA, portB]".
         */
        std::string getCommPortsString() const {
            std::stringstream ss;
            ss << "[";
            for (size_t i = 0; i < with_whom_is_it_communicating.size(); ++i) {
                ss << with_whom_is_it_communicating[i].port;  // Get port name
                if (i < with_whom_is_it_communicating.size() - 1) { ss << ", "; }
            }
            ss << "]";
            return ss.str();
        }

        /**
         * @brief NEW: Helper to get a string of the remote MPI process names.
         * @return A formatted string, e.g., "[t2, system]".
         */
        std::string getCommMpiProcessString() const {
            std::stringstream ss;
            ss << "[";
            for (size_t i = 0; i < with_whom_is_it_communicating.size(); ++i) {
                ss << with_whom_is_it_communicating[i].mpi_process;
                if (i < with_whom_is_it_communicating.size() - 1) { ss << ", "; }
            }
            ss << "]";
            return ss.str();
        }

        /**
         * @brief NEW: Helper to get a string of the remote MPI ranks.
         * @return A formatted string, e.g., "[2, 0]".
         */
        std::string getCommMpiRankString() const {
            std::stringstream ss;
            ss << "[";
            for (size_t i = 0; i < with_whom_is_it_communicating.size(); ++i) {
                ss << with_whom_is_it_communicating[i].mpi_rank;
                if (i < with_whom_is_it_communicating.size() - 1) { ss << ", "; }
            }
            ss << "]";
            return ss.str();
        }
    };

private:
    // --- Private Member Variables ---
    AstNodeModule* m_parentModule;  ///< AST node of the module containing the partitions.
    const std::vector<std::string>& m_partitionInstances;  ///< List of instance names to analyze.
    std::string m_parentModuleName;  ///< Name of the parent module.

    ///< Maps an instance name to a vector of its analyzed ports.
    std::map<std::string, std::vector<Port>> m_partitionData;
    ///< Maps a canonical wire name to all its connection endpoints. An endpoint is a pair of
    ///< {instance_name, port_name}.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> m_wireToEndpoints;
    ///< Maps a wire name (LHS) to the wire it is driven by (RHS) to trace chained assignments.
    std::map<std::string, std::string> m_wireAliasMap;
    ///< Maps an MPI process name ("system" or instance name) to a unique integer rank.
    std::map<std::string, int> m_mpiRankMap;
    // ================== Fix: it doesn't know about the i/o ports of the non-partition modules
    // inside parent module ================== This map will store a pointer to the AST definition
    // for EVERY instance in the parent.
    std::map<std::string, AstNodeModule*> m_instanceToModulePtr;

    /**
     * @class PortGatherVisitor
     * @brief An AST visitor for the initial data gathering phase.
     * @details This visitor traverses the parent module's AST to populate the initial port data
     * for partition instances and to build the crucial `m_wireToEndpoints` and `m_wireAliasMap`
     * structures, which map out the entire connectivity of the parent module.
     */
    class PortGatherVisitor final : public VNVisitorConst {
        PartitionPortAnalyzer* m_analyzer;  ///< Pointer to the main analyzer class.
        AstNodeModule* m_parentModule;  ///< Pointer to the parent module's AST node.

    public:
        /**
         * @brief Constructs the PortGatherVisitor.
         * @param analyzer Pointer to the parent PartitionPortAnalyzer instance.
         * @param parent Pointer to the AST node of the parent module being analyzed.
         */
        PortGatherVisitor(PartitionPortAnalyzer* analyzer, AstNodeModule* parent)
            : m_analyzer(analyzer)
            , m_parentModule(parent) {}

        /**
         * @brief Visits continuous assignments (`assign w1 = w2;`).
         * @details This captures wire-to-wire connections to build the alias map and also treats
         * the parent module's logic as a potential communication endpoint.
         */
        void visit(AstAssignW* assignp) override {
            std::string lhsName = getCanonicalName(assignp->lhsp());
            std::string rhsName = getCanonicalName(assignp->rhsp());

            // If this is a simple wire-to-wire assignment, store it for later chain resolution.
            if (!lhsName.empty() && !rhsName.empty()) {
                m_analyzer->m_wireAliasMap[lhsName] = rhsName;
            }
            // The parent module's logic is considered an endpoint for both nets.
            if (!lhsName.empty()) {
                m_analyzer->m_wireToEndpoints[lhsName].push_back(
                    {m_parentModule->name(), "logic"});
            }
            if (!rhsName.empty()) {
                m_analyzer->m_wireToEndpoints[rhsName].push_back(
                    {m_parentModule->name(), "logic"});
            }
        }

        /** @brief Visits `always` blocks to find assignments within them. */
        void visit(AstAlways* alwaysp) override { iterateChildrenConst(alwaysp); }

        /** @brief Visits blocking assignments (`=`) inside procedural blocks. */
        void visit(AstAssign* assignp) override {
            std::string lhsName = getCanonicalName(assignp->lhsp());
            std::string rhsName = getCanonicalName(assignp->rhsp());
            if (!lhsName.empty() && !rhsName.empty()) {
                m_analyzer->m_wireAliasMap[lhsName] = rhsName;
            }
        }

        /** @brief Visits non-blocking assignments (`<=`) inside procedural blocks. */
        void visit(AstAssignDly* assignp) override {
            std::string lhsName = getCanonicalName(assignp->lhsp());
            std::string rhsName = getCanonicalName(assignp->rhsp());
            if (!lhsName.empty() && !rhsName.empty()) {
                m_analyzer->m_wireAliasMap[lhsName] = rhsName;
            }
        }

        /**
         * @brief Visits module instantiations (`ModuleType instance_name (...)`).
         * @details This is the core of the visitor. It identifies target partition instances,
         * performs a detailed analysis of their ports, and records the connections of *all*
         * instances to build a complete wire map for determining communication partners.
         */
        void visit(AstCell* cellp) override {
            // ================== Fix: it doesn't know about the i/o ports of the non-partition
            // modules inside parent module ================== Store the AST pointer for EVERY
            // instance, not just partitions. This is crucial for looking up port directions on
            // non-partition modules.
            if (cellp->modp()) {
                m_analyzer->m_instanceToModulePtr[cellp->name()] = cellp->modp();
            }
            // =================== Fix END ===================

            // Check if this instance is one of our target partitions.
            auto it = std::find(m_analyzer->m_partitionInstances.begin(),
                                m_analyzer->m_partitionInstances.end(), cellp->name());

            // --- Case 1: The instance is NOT a target partition ---
            if (it == m_analyzer->m_partitionInstances.end()) {
                // Even if not a target, record its connections to the wire map. This is essential
                // for knowing when a target partition communicates with a non-target.
                for (AstPin* pinp = cellp->pinsp(); pinp;
                     pinp = static_cast<AstPin*>(pinp->nextp())) {
                    std::string canonicalName = getCanonicalName(pinp->exprp());
                    if (!canonicalName.empty()) {
                        m_analyzer->m_wireToEndpoints[canonicalName].push_back(
                            {cellp->name(), pinp->name()});
                    }
                }
                return;  // Done with this non-target instance.
            }

            // --- Case 2: This IS a target partition, so analyze its ports in detail. ---
            std::vector<Port> ports;
            AstNodeModule* partitionModule = cellp->modp();
            if (!partitionModule) return;

            // Iterate over each port (pin) of the instance.
            for (AstPin* pinp = cellp->pinsp(); pinp; pinp = static_cast<AstPin*>(pinp->nextp())) {
                Port p;
                p.name = pinp->name();

                // Find the port's declaration in its module definition to get direction and width.
                if (AstVar* varp = findVarInModule(partitionModule, p.name)) {
                    p.direction = varp->direction().xmlKwd();
                    p.width = getDTypeWidth(varp->dtypep());
                }

                // Determine what the port is connected to.
                if (const AstVarRef* varRef = VN_CAST(pinp->exprp(), VarRef)) {
                    p.type = "wire";
                    p.other_end = varRef->name();
                    // Add this connection to the global wire map.
                    m_analyzer->m_wireToEndpoints[p.other_end].push_back({cellp->name(), p.name});

                } else if (const AstConst* constp = VN_CAST(pinp->exprp(), Const)) {
                    p.type = "init";
                    p.other_end = constp->prettyName();
                    p.active = "No";  // Tied to a constant, not an active communication channel.

                } else if (const AstSelBit* selp = VN_CAST(pinp->exprp(), SelBit)) {
                    // =====================================================================
                    // NEW: Added logic to handle bit-select expressions
                    // =====================================================================
                    std::string busName;
                    std::string indexStr;

                    // The first operand (op1p) of a SelBit is the bus/variable itself.
                    if (const AstVarRef* busVarRef = VN_CAST(selp->op1p(), VarRef)) {
                        busName = busVarRef->name();
                    }

                    // The second operand (op2p) is the index.
                    if (const AstConst* indexConst = VN_CAST(selp->op2p(), Const)) {
                        indexStr = std::to_string(indexConst->toUInt());
                    }

                    // If we successfully parsed it, create the canonical name "bus[index]".
                    if (!busName.empty() && !indexStr.empty()) {
                        p.type = "wire";
                        p.other_end = busName + "[" + indexStr + "]";
                        // Add this connection to the wire map using the new canonical name.
                        m_analyzer->m_wireToEndpoints[p.other_end].push_back(
                            {cellp->name(), p.name});
                    } else {
                        // Fallback for complex bit-selects we don't handle yet.
                        p.type = "logic";
                        p.other_end = "[complex selbit]";
                        p.active = "Yes";
                    }
                } else {
                    // Fallback for other complex expressions.
                    p.type = "logic";
                    p.other_end = "[expression]";
                    p.active = "Yes";
                }
                ports.push_back(p);
            }
            m_analyzer->m_partitionData[cellp->name()] = ports;
        }

        /** @brief Generic visitor function to ensure traversal of the entire AST. */
        void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }
    };

private:
    /**
     * @brief Recursively traces a wire through assignments to find its ultimate source.
     * @details This function follows a chain of assignments (e.g., w1=w2, w2=w3) using the
     * `m_wireAliasMap` to find the final wire in the chain.
     * @param wireName The name of the wire to start tracing from.
     * @param visited A set to prevent infinite loops in case of combinational cycles.
     * @param maxDepth The maximum number of levels to recurse.
     * @return The canonical name of the final wire at the end of the chain.
     */
    std::string resolveWireChain(const std::string& wireName, std::set<std::string>& visited,
                                 int maxDepth = 5) {
        // Base Case 1: Maximum recursion depth reached.
        if (maxDepth <= 0) { return wireName; }
        // Base Case 2: Cycle detected.
        if (visited.count(wireName)) { return wireName; }

        visited.insert(wireName);

        // Check if this wire is the LHS of another assignment.
        auto it = m_wireAliasMap.find(wireName);
        if (it != m_wireAliasMap.end()) {
            // If it is, recurse on the RHS of that assignment.
            return resolveWireChain(it->second, visited, maxDepth - 1);
        } else {
            // Base Case 3: This wire is not driven by another simple wire; it's the source.
            return wireName;
        }
    }

    /**
     * @brief Escapes special characters in a string for JSON compatibility.
     * @param s The input string.
     * @return The escaped string.
     */
    std::string jsonEscape(const std::string& s) {
        std::stringstream o;
        for (auto c = s.cbegin(); c != s.cend(); c++) {
            switch (*c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if ('\x00' <= *c && *c <= '\x1f') {
                    o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)*c;
                } else {
                    o << *c;
                }
            }
        }
        return o.str();
    }

    // ================== NEW HELPER FUNCTION START ==================
    /**
     * @brief Finds the direction of a port given its instance and port name.
     * @details Searches both analyzed partition data and the parent module's ports.
     * @param instanceName The name of the instance owning the port.
     * @param portName The name of the port.
     * @return A string representing the direction ("Input", "Output", "Inout", or "Unknown").
     */
    std::string getPortDirection(const std::string& instanceName,
                                 const std::string& portName) const {
        // Case 1: The instance is a partition we have analyzed in detail.
        const auto& inst_it = m_partitionData.find(instanceName);
        if (inst_it != m_partitionData.end()) {
            const auto& ports = inst_it->second;
            for (const auto& p : ports) {
                if (p.name == portName) {
                    if (p.direction.find("in") != std::string::npos) return "Input";
                    if (p.direction.find("out") != std::string::npos) return "Output";
                    return "Inout";
                }
            }
        }

        // ================== NEW LOGIC START ==================
        // Case 2: The instance is a regular (non-partition) module.
        // Use our new map to find its AST definition and look up the port.
        auto module_ptr_it = m_instanceToModulePtr.find(instanceName);
        if (module_ptr_it != m_instanceToModulePtr.end()) {
            AstNodeModule* moduleDef = module_ptr_it->second;
            if (AstVar* varp = findVarInModule(moduleDef, portName)) {
                std::string dir = varp->direction().xmlKwd();
                if (dir.find("in") != std::string::npos) return "Input";
                if (dir.find("out") != std::string::npos) return "Output";
                return "Inout";
            }
        }
        // =================== NEW LOGIC END ===================

        // Case 3: The endpoint is a port on the parent module itself (the "system").
        if (instanceName == m_parentModuleName) {
            if (AstVar* varp = findVarInModule(m_parentModule, portName)) {
                std::string dir = varp->direction().xmlKwd();
                if (dir.find("in") != std::string::npos) return "Input";
                if (dir.find("out") != std::string::npos) return "Output";
                return "Inout";
            }
        }

        // Case 4: The endpoint is the parent's internal logic.
        if (portName == "logic") { return "Output"; }

        return "Unknown";
    }
    // =================== NEW HELPER FUNCTION END ===================

public:
    /**
     * @brief Constructor for the PartitionPortAnalyzer.
     * @param parentModule AST node of the module containing the partition instances.
     * @param partitionInstances A vector of strings with the names of the instances to analyze.
     */
    PartitionPortAnalyzer(AstNodeModule* parentModule,
                          std::vector<std::string>& partitionInstances)  // Note: non-const now
        : m_parentModule(parentModule)
        , m_partitionInstances(partitionInstances) {
        m_parentModuleName = parentModule->name();

        // --- NEW: Create the MPI Rank Map ---
        // Rule: "system" process is always rank 0.
        m_mpiRankMap["system"] = 0;

        // Sort partition names to ensure deterministic rank assignment.
        std::sort(partitionInstances.begin(), partitionInstances.end());

        // Assign ranks 1, 2, 3... to the sorted partition instances.
        int currentRank = 1;
        for (const auto& instName : m_partitionInstances) {
            m_mpiRankMap[instName] = currentRank++;
        }
    }

    /**
     * @brief Runs the multi-phase analysis process to determine partition connectivity.
     * @details This function orchestrates the entire analysis workflow. It is structured
     * into distinct phases to ensure correctness and clarity.
     *
     * 1.  Phase 1 (Data Gathering): A `PortGatherVisitor` traverses the parent
     * module's AST to populate initial data structures, including a map of all wire
     * endpoints and a map of wire-to-wire assignments (`assign` statements).
     *
     * 2.  Main Analysis Loop: After gathering data, a single, efficient loop
     * processes each partition port completely through a series of sequential steps:
     * - Step 2.1 (Wire Chain Resolution): Traces through chained `assign`
     * statements to find the ultimate source wire for the port.
     * - Step 2.2 (Partner Population): Identifies all other ports/logic on the
     * source wire and applies sophisticated filtering to build a list of valid
     * communication partners, prioritizing true data originators.
     * - Step 2.3 (Status Finalization): Sets the port's preliminary 'active'
     * state and communication type ("P2P", "broadcast", "NULL").
     *
     * 3.  Phase 3 (Global Name Disambiguation): After the main loop, this final
     * phase performs a global analysis. It groups all connections by their communication
     * link (e.g., all signals from rank 1 to rank 0). Within each link, it finds and
     * renames any duplicate remote port names to guarantee uniqueness in the final
     * generated code. This correctly handles complex cases where different local ports
     * connect to remote ports that share the same name.
     */
    void analyze() {
        // === PHASE 1: Gather port info, endpoints, and wire aliases ===
        m_parentModule->foreach([&](AstVar* varp) {
            if (varp->isIO()) {
                const std::string parentPortName = varp->name();
                if (!parentPortName.empty()) {
                    m_wireToEndpoints[parentPortName].push_back(
                        {m_parentModuleName, parentPortName});
                }
            }
        });
        PortGatherVisitor gatherer(this, m_parentModule);
        gatherer.iterateConst(m_parentModule);

        // === Main Analysis Loop (Combines preliminary processing for each port) ===
        for (auto& inst_pair : m_partitionData) {
            for (auto& port : inst_pair.second) {

                // --- Step 2.1: Resolve wire chains for the current port ---
                if (port.type == "wire") {
                    std::set<std::string> visitedWires;
                    std::string initialWire = port.other_end;
                    std::string finalWire = resolveWireChain(initialWire, visitedWires);
                    if (initialWire != finalWire) {
                        port.other_end = finalWire;
                        auto& initialEndpoints = m_wireToEndpoints[initialWire];
                        for (auto it = initialEndpoints.begin(); it != initialEndpoints.end();) {
                            if (it->first == inst_pair.first && it->second == port.name) {
                                m_wireToEndpoints[finalWire].push_back(*it);
                                it = initialEndpoints.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                }

                // --- Step 2.2: Populate 'with_whom_is_it_communicating' for the current port ---
                if (port.type == "wire") {
                    const auto& endpoints = m_wireToEndpoints[port.other_end];
                    const std::string& sourceRawDir = port.direction;
                    std::string sourceDirection = "Unknown";
                    if (sourceRawDir.find("in") != std::string::npos)
                        sourceDirection = "Input";
                    else if (sourceRawDir.find("out") != std::string::npos)
                        sourceDirection = "Output";
                    else if (sourceRawDir.find("inout") != std::string::npos)
                        sourceDirection = "Inout";

                    bool trueOutputSourceExists = false;
                    if (sourceDirection == "Input") {
                        for (const auto& endpoint : endpoints) {
                            if (endpoint.first == inst_pair.first) continue;
                            if (getPortDirection(endpoint.first, endpoint.second) == "Output") {
                                trueOutputSourceExists = true;
                                break;
                            }
                        }
                    }

                    for (const auto& endpoint : endpoints) {
                        if (endpoint.first == inst_pair.first) { continue; }

                        bool isParentInternalLogic
                            = (endpoint.first == m_parentModuleName && endpoint.second == "logic");
                        std::string endpointDirection
                            = getPortDirection(endpoint.first, endpoint.second);

                        bool isValidConnection = false;
                        if (sourceDirection == "Input" && trueOutputSourceExists
                            && isParentInternalLogic) {
                            isValidConnection = false;
                        } else if (isParentInternalLogic) {
                            isValidConnection = true;
                        } else if (sourceDirection == "Inout" || endpointDirection == "Inout") {
                            isValidConnection = true;
                        } else if (sourceDirection == "Input" && endpointDirection == "Output") {
                            isValidConnection = true;
                        } else if (sourceDirection == "Output" && endpointDirection == "Input") {
                            isValidConnection = true;
                        }

                        if (!isValidConnection) { continue; }

                        std::string remote_port_name = endpoint.second;
                        if (isParentInternalLogic) { remote_port_name = "logic_" + port.name; }

                        std::string partner_mpi_process = "system";
                        if (std::find(m_partitionInstances.begin(), m_partitionInstances.end(),
                                      endpoint.first)
                            != m_partitionInstances.end()) {
                            partner_mpi_process = endpoint.first;
                        }
                        int partner_mpi_rank = m_mpiRankMap[partner_mpi_process];
                        port.with_whom_is_it_communicating.push_back(
                            {endpoint.first, remote_port_name, partner_mpi_process,
                             partner_mpi_rank});
                    }
                }

                // --- Step 2.3: Determine preliminary status for the current port ---
                port.mpi_process = inst_pair.first;
                port.mpi_rank = m_mpiRankMap[port.mpi_process];
                const size_t comm_count = port.with_whom_is_it_communicating.size();
                if (comm_count == 0) {
                    port.comm_type = "NULL";
                } else if (comm_count == 1) {
                    port.comm_type = "P2P";
                } else {
                    port.comm_type = "broadcast";
                }
                if (port.active == "idk") {
                    port.active = port.with_whom_is_it_communicating.empty() ? "No" : "Yes";
                }
            }
        }

        // === PHASE 3: Global Name Disambiguation ===
        std::map<std::pair<int, int>, std::vector<std::pair<CommunicationPartner*, Port*>>>
            comm_links;
        for (auto& inst_pair : m_partitionData) {
            for (auto& port : inst_pair.second) {
                for (auto& partner : port.with_whom_is_it_communicating) {
                    int sender_rank, receiver_rank;
                    if (port.direction.find("out") != std::string::npos) {
                        sender_rank = port.mpi_rank;
                        receiver_rank = partner.mpi_rank;
                    } else {
                        sender_rank = partner.mpi_rank;
                        receiver_rank = port.mpi_rank;
                    }
                    comm_links[{sender_rank, receiver_rank}].push_back({&partner, &port});
                }
            }
        }
        for (auto const& [link, partners_with_context] : comm_links) {
            if (partners_with_context.size() <= 1) continue;
            std::map<std::string, int> name_counts;
            for (const auto& pair : partners_with_context) { name_counts[pair.first->port]++; }
            std::set<std::string> duplicate_names;
            for (const auto& count_pair : name_counts) {
                if (count_pair.second > 1) { duplicate_names.insert(count_pair.first); }
            }
            if (!duplicate_names.empty()) {
                for (auto& pair : partners_with_context) {
                    CommunicationPartner* partner_ptr = pair.first;
                    Port* local_port_ptr = pair.second;
                    if (duplicate_names.count(partner_ptr->port)) {
                        partner_ptr->port = partner_ptr->port + "_" + local_port_ptr->name;
                    }
                }
            }
        }
    }

    // A public getter to provide access to the analysis results.
    const std::map<std::string, std::vector<Port>>& getPartitionData() const {
        return m_partitionData;
    }

    /**
     * @brief Prints a formatted report of the analysis results to standard output.
     */
    void printReport() {
        for (const auto& inst_pair : m_partitionData) {
            const std::string& inst_name = inst_pair.first;
            const std::vector<Port>& ports = inst_pair.second;

            std::cout << "\nInstance: " << inst_name << "\n";
            std::cout << "-------------------------------------------\n";

            printf("%-25s %-10s %-7s %-10s %-10s %-12s %-25s %-25s %-20s %s\n", "Port Name",
                   "Direction", "Width", "Own Rank", "Own MPI Process", "Comm Type",
                   "Remote Instance", "Remote Port", "Remote MPI Process", "Remote MPI Rank");

            for (const auto& port : ports) {
                // MODIFIED: Added port.comm_type.c_str() to the output
                printf("%-25s %-10s %-7d %-10d %-10s %-12s %-25s %-25s %-20s %s\n",
                       port.name.c_str(), port.direction.c_str(), port.width, port.mpi_rank,
                       port.mpi_process.c_str(), port.comm_type.c_str(),
                       port.getCommInstancesString().c_str(), port.getCommPortsString().c_str(),
                       port.getCommMpiProcessString().c_str(),
                       port.getCommMpiRankString().c_str());
            }
        }
        std::cout << "\n=========================================================================="
                     "============================================================================"
                     "========\n";
    }

    /**
     * @brief Writes the analysis results to a JSON file.
     * @param filename The name of the output JSON file.
     */
    void writeJsonReport(const std::string& filename) {
        std::ofstream jsonFile(filename);
        if (!jsonFile.is_open()) {
            std::cerr << "Error: Could not open file for writing JSON report: " << filename
                      << std::endl;
            return;
        }

        jsonFile << "{\n";
        jsonFile << "  \"partitions\": {\n";

        for (auto it = m_partitionData.begin(); it != m_partitionData.end(); ++it) {
            const std::string& inst_name = it->first;
            const std::vector<Port>& ports = it->second;

            jsonFile << "    \"" << jsonEscape(inst_name) << "\": [\n";

            for (auto port_it = ports.begin(); port_it != ports.end(); ++port_it) {
                const auto& port = *port_it;
                jsonFile << "      {\n";
                jsonFile << "        \"port_name\": \"" << jsonEscape(port.name) << "\",\n";
                jsonFile << "        \"direction\": \"" << jsonEscape(port.direction) << "\",\n";
                jsonFile << "        \"width\": " << port.width << ",\n";
                jsonFile << "        \"active\": \"" << jsonEscape(port.active) << "\",\n";
                jsonFile << "        \"type\": \"" << jsonEscape(port.type) << "\",\n";
                jsonFile << "        \"connecting_wire\": \"" << jsonEscape(port.other_end)
                         << "\",\n";
                // MODIFIED: This field now correctly represents this port's own MPI process
                jsonFile << "        \"mpi_process\": \"" << jsonEscape(port.mpi_process)
                         << "\",\n";

                jsonFile << "        \"mpi_rank\": " << port.mpi_rank << ",\n";
                jsonFile << "        \"Comm\": \"" << jsonEscape(port.comm_type) << "\",\n";
                jsonFile << "        \"with_whom_is_it_communicating\": [";

                // MODIFIED: Generate an array of JSON objects with the new mpi_process field
                for (auto comm_it = port.with_whom_is_it_communicating.begin();
                     comm_it != port.with_whom_is_it_communicating.end(); ++comm_it) {
                    jsonFile << "{";
                    jsonFile << "\"instance\": \"" << jsonEscape(comm_it->instance) << "\", ";
                    jsonFile << "\"port\": \"" << jsonEscape(comm_it->port) << "\", ";
                    jsonFile << "\"mpi_process\": \"" << jsonEscape(comm_it->mpi_process)
                             << "\", ";
                    jsonFile << "\"mpi_rank\": " << comm_it->mpi_rank;
                    jsonFile << "}";
                    if (std::next(comm_it) != port.with_whom_is_it_communicating.end()) {
                        jsonFile << ", ";
                    }
                }
                // END OF MODIFICATION

                jsonFile << "]\n";
                jsonFile << "      }";
                if (std::next(port_it) != ports.end()) { jsonFile << ","; }
                jsonFile << "\n";
            }

            jsonFile << "    ]";
            if (std::next(it) != m_partitionData.end()) { jsonFile << ","; }
            jsonFile << "\n";
        }
        jsonFile << "  }\n";
        jsonFile << "}\n";

        jsonFile.close();
        std::cout << "Successfully wrote JSON report to " << filename << "\n";
    }
};

/**
 * @class StringTable
 * @brief A simple helper class for string interning.
 */
class StringTable {
private:
    std::set<std::string> m_table;

public:
    const char* intern(const std::string& str) {
        auto it = m_table.insert(str);
        return it.first->c_str();
    }
    const char* intern(const char* str) {
        if (!str) return nullptr;
        auto it = m_table.insert(str);
        return it.first->c_str();
    }
};

/**
 * @class InstancePortPrinterVisitor
 * @brief A general-purpose diagnostic visitor for printing port connections.
 */
class InstancePortPrinterVisitor final : public VNVisitorConst {
    std::string m_targetModuleName;
    std::unordered_map<const char*, int> m_connectionMap;
    int m_nextConnectionId = 0;
    StringTable m_keyTable;

    const char* getCanonicalConnectionKey(AstNode* exprp) {
        if (!exprp) { return m_keyTable.intern("nullptr"); }
        if (const AstConst* constp = VN_CAST(exprp, Const)) {
            return m_keyTable.intern(constp->prettyName());
        }
        if (const AstVarRef* varRef = VN_CAST(exprp, VarRef)) {
            return m_keyTable.intern(varRef->name());
        }
        if (const AstSelBit* selp = VN_CAST(exprp, SelBit)) {
            const char* busKey = getCanonicalConnectionKey(selp->op1p());
            const char* indexKey = getCanonicalConnectionKey(selp->op2p());
            return m_keyTable.intern(std::string(busKey) + "[" + std::string(indexKey) + "]");
        }
        std::string fallbackKey = std::string(exprp->typeName()) + "@"
                                  + std::to_string(reinterpret_cast<uintptr_t>(exprp));
        return m_keyTable.intern(fallbackKey);
    }

    std::string getConnectionName(AstNode* exprp) {
        if (!exprp) { return "[Unconnected]"; }
        if (const AstConst* constp = VN_CAST(exprp, Const)) { return constp->prettyName(); }
        const char* canonicalKey = getCanonicalConnectionKey(exprp);
        auto it = m_connectionMap.find(canonicalKey);
        if (it != m_connectionMap.end()) {
            return std::string("[") + canonicalKey + " | ID: " + std::to_string(it->second) + "]";
        } else {
            int newId = m_nextConnectionId++;
            m_connectionMap[canonicalKey] = newId;
            return std::string("[") + canonicalKey + " | ID: " + std::to_string(newId) + "]";
        }
    }

public:
    InstancePortPrinterVisitor(const std::string& targetModuleName)
        : m_targetModuleName(targetModuleName) {}
    void visit(AstCell* nodep) override {
        if (nodep->modName() == m_targetModuleName) {
            std::cout << "\n--------------------------------------------------\n";
            std::cout << "Instance: '" << nodep->name() << "' (Type: '" << nodep->modName()
                      << "')\n";
            std::cout << "  Port Connections:\n";
            for (AstPin* pinp = nodep->pinsp(); pinp; pinp = static_cast<AstPin*>(pinp->nextp())) {
                std::cout << "    - Port '" << pinp->name() << "' -> connects to -> "
                          << getConnectionName(pinp->exprp()) << "\n";
            }
        }
        if (nodep->modp()) { iterateChildrenConst(nodep->modp()); }
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }
};

/**
 * @class HierCellsGraphVisitor
 * @brief Builds a hierarchical graph of the design to automatically detect partition candidates.
 */
class HierCellsGraphVisitor final : public VNVisitorConst {
    struct CellInfo {
        std::string name;
        std::string submodname;
        std::string hier;
    };
    struct ModNode {
        std::string moduleName;
        std::string instanceName;
        std::string hierInstance;
        std::string hierModule;
        std::string hash;
        int weight;
        ModNode() = default;
        ModNode(const std::string& mod, const std::string& inst, const std::string& hInst,
                const std::string& hMod, const std::string& blake, const int w)
            : moduleName(mod)
            , instanceName(inst)
            , hierInstance(hInst)
            , hierModule(hMod)
            , hash(blake)
            , weight(w) {}
    };

    std::string m_rootHier;
    std::unordered_map<std::string, ModNode> nodeMetadata;
    std::vector<std::pair<std::string, std::string>> edges;
    std::vector<CellInfo> nodes;
    std::unordered_map<std::string, AstNodeModule*> instanceToModuleMap;
    std::unordered_map<std::string, AstNodeModule*> moduleNameToModulePtr;
    std::string m_hier;
    std::string m_hierWRTModuleName;
    std::unordered_map<std::string, std::vector<ModNode>> adjacency;

    std::string stripTrailingDot(const std::string& str) {
        if (!str.empty() && str.back() == '.') { return str.substr(0, str.size() - 1); }
        return str;
    }

    // Recursive traversal algorithm to get the module's file path
    void collectPartitionFiles(AstNodeModule* module, std::set<std::string>& fileSet) {
        if (!module || module->dead()) return;

        // Add the current module's file to our set
        fileSet.insert(module->fileline()->filename());

        // Recurse into all child instances (AstCells)
        module->foreach([&](AstCell* cellp) {
            if (cellp->modp()) { collectPartitionFiles(cellp->modp(), fileSet); }
        });
    }

    void dfs(const std::string& nodeHier, std::unordered_set<std::string>& visited) {
        if (visited.count(nodeHier)) return;
        visited.insert(nodeHier);
        int totalChildWeight = 0;
        bool hasChild = false;
        auto it = adjacency.find(nodeHier);
        if (it != adjacency.end()) {
            for (const ModNode& child : it->second) {
                hasChild = true;
                dfs(child.hierInstance, visited);
                totalChildWeight += nodeMetadata[child.hierInstance].weight;
            }
        }
        if (nodeMetadata.count(nodeHier)) {
            nodeMetadata[nodeHier].weight = hasChild ? totalChildWeight : 1;
        }
    }

    void visit(AstNodeModule* nodep) override {
        if (!nodep->dead()) {
            m_hier = "$root";
            m_hierWRTModuleName = "$root";
            iterateChildrenConst(nodep);
        }
    }

    void visit(AstCell* nodep) override {
        if (nodep->modp() && nodep->modp()->dead()) return;
        if (nodep->modp()) {
            instanceToModuleMap[nodep->name()] = nodep->modp();
            moduleNameToModulePtr[nodep->modName()] = nodep->modp();
        }
        std::string parentHier = stripTrailingDot(m_hier);
        std::string instanceName = nodep->name();
        std::string modName = nodep->modName();
        std::string childHierWRTInstanceName = parentHier + "." + instanceName;
        std::string childHierWRTModuleName = stripTrailingDot(m_hierWRTModuleName) + "." + modName;
        ModNode childNode(modName, instanceName, childHierWRTInstanceName, childHierWRTModuleName,
                          blake2b_128_hex(childHierWRTModuleName), 0);
        nodeMetadata[childHierWRTInstanceName] = childNode;
        edges.emplace_back(parentHier, childHierWRTInstanceName);
        adjacency[parentHier].push_back(childNode);
        const std::string oldHier = m_hier;
        const std::string oldModHier = m_hierWRTModuleName;
        m_hier = childHierWRTInstanceName;
        m_hierWRTModuleName = childHierWRTModuleName;
        iterateChildrenConst(nodep->modp());
        m_hier = oldHier;
        m_hierWRTModuleName = oldModHier;
    }

    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    HierCellsGraphVisitor(AstNetlist* rootp) {
        AstNodeModule* top = rootp->topModulep();
        m_hier = stripTrailingDot(top->name()) + ".";
        m_rootHier = stripTrailingDot(top->origName());
        nodeMetadata["$root"]
            = ModNode("$root", "$root", "$root", "$root", blake2b_128_hex("$root"), 0);
        iterateConst(top);
    }

    bool runBFSCheckDuplicateHashes(std::string& o_partitionModuleName,
                                    std::vector<std::string>& o_partitionInstanceNames,
                                    std::string& o_parentHier) {
        std::queue<std::string> q;
        q.push("$root");
        std::unordered_set<std::string> visited;
        visited.insert("$root");
        int level = 0;
        while (!q.empty()) {
            size_t levelSize = q.size();
            std::unordered_map<std::string, std::vector<ModNode>> hashToNodes;
            for (size_t i = 0; i < levelSize; ++i) {
                std::string current = q.front();
                q.pop();
                const auto& children = adjacency[current];
                for (const auto& child : children) {
                    if (visited.count(child.hierInstance)) continue;
                    visited.insert(child.hierInstance);
                    hashToNodes[child.hash].push_back(child);
                    q.push(child.hierInstance);
                }
            }
            bool foundAny = false;
            std::string bestHash;
            int maxWeight = -1;
            for (const auto& [hash, nodes] : hashToNodes) {
                if (nodes.size() > 1) {
                    int currentWeightSum = 0;
                    for (const auto& node : nodes) {
                        if (nodeMetadata.count(node.hierInstance)) {
                            currentWeightSum += nodeMetadata[node.hierInstance].weight;
                        }
                    }
                    if (currentWeightSum > maxWeight) {
                        maxWeight = currentWeightSum;
                        bestHash = hash;
                        foundAny = true;
                    }
                }
            }
            if (foundAny) {
                std::cout << "Duplicate hash(es) found at level " << level << ":\n";
                std::cout << "  Hash: " << bestHash << " (Max weight sum: " << maxWeight << ")\n";
                const auto& bestNodes = hashToNodes[bestHash];
                o_partitionModuleName = bestNodes[0].moduleName;
                size_t lastDot = bestNodes[0].hierInstance.rfind('.');
                o_parentHier = (lastDot != std::string::npos)
                                   ? bestNodes[0].hierInstance.substr(0, lastDot)
                                   : "$root";
                for (const auto& node : bestNodes) {
                    std::cout << "    Module: " << node.moduleName
                              << ", Instance: " << node.instanceName
                              << ", Hier: " << node.hierInstance
                              << ", Weight: " << nodeMetadata[node.hierInstance].weight << "\n";
                    o_partitionInstanceNames.push_back(node.instanceName);
                }
                return true;
            }
            ++level;
        }
        std::cout << "No duplicate hashes found to select a partition top.\n";
        return false;
    }

    void runDFS() {
        std::unordered_set<std::string> visited;
        dfs("$root", visited);
    }

    void dumpDot(std::ostream& os) {
        os << "digraph G {\n";
        for (const auto& edge : edges) {
            os << "  \"" << edge.first << "\" -> \"" << edge.second << "\";\n";
        }
        os << "}\n";
    }

    void dumpAdjacency(std::ostream& os) {
        for (const auto& [parent, children] : adjacency) {
            os << "Parent: " << parent << "\n";
            for (const auto& child : children) {
                os << "  └─ Instance: " << child.instanceName << ", Module: " << child.moduleName
                   << ", Hier: " << child.hierInstance << "\n";
            }
        }
    }

public:
    /**
     * @brief The main entry point for the automatic partitioning analysis.
     */
    void findAndPrintPartitionPorts(AstNetlist* rootp) {
        std::cout << "Building hierarchy graph and calculating weights...\n";
        runDFS();
        std::cout << "\nFinding partition instances via BFS hash check...\n";
        std::string partitionModuleName;
        std::vector<std::string> partitionInstanceNames;
        std::string parentHier;
        bool foundPartitions
            = runBFSCheckDuplicateHashes(partitionModuleName, partitionInstanceNames, parentHier);

        if (foundPartitions) {
            // =================================================================
            // Creating the output directory before generating any files
            // =================================================================
            const char* dir_name = "metro_mpi";
            // The mkdir function returns 0 on success.
            // We check if it's not successful AND the reason is not that it already exists.
            if (mkdir(dir_name, 0775) != 0 && errno != EEXIST) {
                std::cerr << "  --> ERROR: Could not create output directory '" << dir_name
                          << "'\n";
                return;  // Stopping if we can't create the directory
            }
            // =================================================================

            std::cout << "\n======================================================================"
                         "===================================================\n";
            std::cout << "PARTITION ANALYSIS REPORT\n";
            std::cout << "Found " << partitionInstanceNames.size()
                      << " partition instances of module '" << partitionModuleName << "'\n";

            std::string partitionModuleOrigName = partitionModuleName;  // Default to same name
            if (moduleNameToModulePtr.count(partitionModuleName)) {
                partitionModuleOrigName = moduleNameToModulePtr[partitionModuleName]->origName();
            }
            std::cout << "  --> Original source name: '" << partitionModuleOrigName << "'\n";
            std::cout << "Debug 1 " << partitionModuleOrigName << endl;

            if (nodeMetadata.count(parentHier)) {
                std::string parentModuleName = nodeMetadata[parentHier].moduleName;
                if (moduleNameToModulePtr.count(parentModuleName)) {
                    // --- NEW LOGIC STARTS HERE ---
                    std::cout << "\n[Metro-MPI] Collecting source files for partition '"
                              << partitionModuleOrigName << "'...\n";
                    std::set<std::string> partitionFileSet;
                    if (moduleNameToModulePtr.count(partitionModuleName)) {
                        AstNodeModule* partitionTopModule
                            = moduleNameToModulePtr.at(partitionModuleName);
                        collectPartitionFiles(partitionTopModule, partitionFileSet);
                    }

                    std::vector<std::string> partitionFiles(partitionFileSet.begin(),
                                                            partitionFileSet.end());
                    std::cout << "  --> Found " << partitionFiles.size()
                              << " unique source files:\n";
                    for (const auto& file : partitionFiles) {
                        std::cout << "    - " << file << "\n";
                    }

                    MakefileGenerator makefileGenerator;
                    makefileGenerator.generate(argString, partitionModuleOrigName, partitionFiles);
                    // Now we have the 'partitionFiles' vector to use for generating the Makefile

                    AstNodeModule* parentModulePtr = moduleNameToModulePtr[parentModuleName];
                    std::cout << "Parent Module: '" << parentModuleName
                              << "' (Hier: " << parentHier << ")\n";

                    const std::string parentModuleFilePath
                        = parentModulePtr->fileline()->filename();

                    PartitionPortAnalyzer analyzer(parentModulePtr, partitionInstanceNames);
                    analyzer.analyze();
                    analyzer.printReport();
                    analyzer.writeJsonReport("metro_mpi/partition_report.json");
                    // =================================================================
                    // ### Calling the MPI File Generator ###

                    MPIFileGenerator fileGenerator;
                    fileGenerator.generateAndModifyFiles(
                        partitionModuleName, partitionModuleOrigName, analyzer.getPartitionData(),
                        moduleNameToModulePtr, parentModuleFilePath, parentModuleName);

                    // =================================================================
                    // NEW: Calling the metro_mpi.cpp code generator

                    MPICodeGenerator codeGenerator;
                    codeGenerator.generateMpiVerificationFile("metro_mpi/partition_report.json");

                    // =================================================================
                    // ### NEW: Calling the main <PartitionModuleName>_main.cpp generator ###
                    MPIMainGenerator mainGenerator;
                    std::cout << "Debug 2 " << partitionModuleOrigName << endl;
                    mainGenerator.generate("metro_mpi/partition_report.json",
                                           partitionModuleOrigName);

                    // =================================================================
                    // ### NEW: Calling the Rank 0 Main C++ driver generator ###
                    if (rootp->topModulep()) {
                        std::string topModuleNameForRank0;
                        AstNodeModule* currentTop = rootp->topModulep();

                        if (currentTop && currentTop->name() == "$root") {
                            // After Verilator's wrapTop pass, the user's top module is the single
                            // cell instantiated inside the new '$root' module.
                            AstCell* topCell = nullptr;

                            // Use the correct 'foreach' visitor to find the cell inside the
                            // module.
                            currentTop->foreach([&](AstCell* cellp) {
                                if (!topCell) {  // Find the first (and should be only) cell
                                    topCell = cellp;
                                }
                            });

                            if (topCell && topCell->modp()) {
                                // Correctly get the module pointer from the cell, then get its
                                // original name.
                                topModuleNameForRank0 = topCell->modp()->origName();
                                std::cout
                                    << "[Metro-MPI] Detected wrapped top module. Rank 0 top is '"
                                    << topModuleNameForRank0 << "'.\n";
                            } else {
                                std::cerr << "  --> WARNING: Could not find top-level instance "
                                             "inside $root module. Falling back.\n";
                                topModuleNameForRank0 = v3Global.opt.topModule();
                            }
                        } else if (currentTop) {
                            // This is a fallback in case the analysis runs before wrapTop
                            topModuleNameForRank0 = currentTop->origName();
                        } else {
                            std::cerr << "  --> WARNING: Could not determine top-level module. "
                                         "Falling back.\n";
                            topModuleNameForRank0 = v3Global.opt.topModule();
                        }

                        if (topModuleNameForRank0.empty()) {
                            std::cerr << "  --> FATAL: Top module name for Rank 0 generator is "
                                         "empty. Aborting.\n";
                            return;  // Stop further processing
                        }
                        Rank0MainGenerator rank0Generator;
                        std::cout << "topModuleName -> " << topModuleNameForRank0 << std::endl;
                        rank0Generator.generate("metro_mpi/partition_report.json",
                                                topModuleNameForRank0);

                    } else {
                        std::cerr << "  --> ERROR: Could not determine top-level module name for "
                                     "Rank 0 generator.\n";
                    }

                } else {
                    std::cout << "ERROR: Could not find AST pointer for parent module '"
                              << parentModuleName << "'\n";
                }
            } else {
                std::cout << "ERROR: Could not find metadata for parent hierarchy '" << parentHier
                          << "'\n";
            }
        } else {
            std::cout << "\nNo partition top was selected, skipping port printing.\n";
        }
    }
};

#endif  // V3METRO_MPI_H