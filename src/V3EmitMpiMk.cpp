#include "V3PchAstNoMT.h"  // For precompiled headers

// Add all necessary includes AFTER the precompiled header
#include "V3EmitMpiMk.h"   // Your new header
#include "V3Options.h"     // To get command line options
#include "V3Global.h"      // To access the global state (v3Global)
#include "V3EmitCBase.h"   // For V3OutMkFile
#include "V3Ast.h"         // For AstNode, AstVFile, and VN_CAST
#include "V3String.h"      // FIX: Re-added for V3StringList typedef

#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <iterator>

// Your implementation class
class EmitMpiMk final {
private:
    // Helper function to check string suffix
    bool endsWith(const std::string& str, const std::string& suffix) {
        if (str.length() < suffix.length()) {
            return false;
        }
        return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
    }

public:
    // The main method that generates the Makefile content
    void emit(const std::string& argString) {
        // 1. Define the output filename.
        // Note: You changed this to "Makefile". The original was "Makefile.mpi"
        const string filename = v3Global.opt.makeDir() + "/" + "Makefile";

        // 2. Create a V3OutMkFile object.
        V3OutMkFile of{filename};
        
        // 3. Write the standard Verilator header.
        of.putsHeader();

        // 4. Add descriptive comments to your Makefile.
        of.puts("# DESCRIPTION: Verilator MPI output: Makefile for MPI-based simulation\n");
        of.puts("#\n");
        of.puts("# This Makefile orchestrates the multi-stage Verilation and simulation process.\n");
        of.puts("\n");

        // 5. Populate Makefile variables from the global options object.
        of.puts("### Variables...\n");
        of.puts("VERILATOR_EXE ?= verilator\n");
        of.puts("TOP_MODULE    = " + v3Global.opt.topModule() + "\n");
        of.puts("PREFIX        = " + v3Global.opt.prefix() + "\n");
        of.puts("OBJ_DIR       = " + v3Global.opt.makeDir() + "\n");
        of.puts("\n");
        
        // 5a. Create SRC_FILES variable by iterating through the AST file list
        of.puts("# All .v and .sv source files from the command line\n");
        of.puts("SRC_FILES = \\");
        std::set<string> source_file_set;
        for (AstNode* nodep = v3Global.rootp()->filesp(); nodep; nodep = nodep->nextp()) {
            const AstVFile* filep = VN_CAST(nodep, VFile);
            const string& file = filep->name();
            if (endsWith(file, ".v") || endsWith(file, ".sv")) {
                of.puts("\t" + file + " \\");
                source_file_set.insert(file);
            }
        }
        of.puts("\n");

        // 5b. Create VERILATOR_FLAGS from the original argString, filtering out source files.
        std::stringstream ss(argString);
        std::istream_iterator<std::string> begin(ss);
        std::istream_iterator<std::string> end;
        std::vector<std::string> tokens(begin, end);

        string flags_for_makefile;
        for (const auto& token : tokens) {
            if (source_file_set.find(token) == source_file_set.end() && token != "--mmpi-mk") {
                flags_for_makefile += "\t" + token + " \\\n";
            }
        }
        
        of.puts("# All other Verilator flags from the original command line\n");
        of.puts("VERILATOR_FLAGS = \\");
        if (!flags_for_makefile.empty()) {
            flags_for_makefile.erase(flags_for_makefile.length() - 2);
            of.puts(flags_for_makefile);
        }
        of.puts("\n");

        of.puts("# Add flags from the original Verilator command line for the C++ compiler\n");
        of.puts("VM_USER_CFLAGS = \\\n");
        const V3StringList& cFlags = v3Global.opt.cFlags();
        for (const string& i : cFlags) of.puts("\t" + i + " \\\n");
        of.puts("\n");
        
        of.puts("VM_USER_LDLIBS = \\\n");
        const V3StringList& ldLibs = v3Global.opt.ldLibs();
        for (const string& i : ldLibs) of.puts("\t" + i + " \\\n");
        of.puts("\n");

        // 6. Define your Makefile rules.
        of.puts("### Rules...\n");
        of.puts(".PHONY: all elaborate compile run\n");
        of.puts("\n");
        
        of.puts("all: run\n");
        of.puts("\n");
        
        of.puts("elaborate:\n");
        of.puts("\t@echo \"Running Verilator to elaborate the design...\"\n");
        of.puts("\t$(VERILATOR_EXE) $(VERILATOR_FLAGS) --Mdir $(OBJ_DIR) --prefix $(PREFIX) --exe YourSimMain.cpp $(SRC_FILES)\n");
        of.puts("\n");

        of.puts("compile:\n");
        of.puts("\t@echo \"Compiling the Verilated C++ code...\"\n");
        of.puts("\t$(MAKE) -C $(OBJ_DIR) -f $(PREFIX).mk\n");
        of.puts("\n");

        of.puts("run: compile\n");
        of.puts("\t@echo \"Running the simulation...\"\n");
        of.puts("\t$(OBJ_DIR)/$(PREFIX)\n");
    }
};

// This is the public static function defined in the header.
void V3EmitMpiMk::emitMpiMk(const std::string& argString) {
    EmitMpiMk emitter;
    emitter.emit(argString);
}