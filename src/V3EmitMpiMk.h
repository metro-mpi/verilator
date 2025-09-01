// V3EmitMpiMk.h

#ifndef VERILATOR_V3EMITMPIMK_H_
#define VERILATOR_V3EMITMPIMK_H_

#include "config_build.h"
#include "verilatedos.h"

#pragma once
#include <string>

// This is the public-facing class that Verilator.cpp will interact with.
class V3EmitMpiMk final {
public:
    // The public static function to be called from main()
    static void emitMpiMk(const std::string& argString) VL_MT_DISABLED;
};

#endif // Guard