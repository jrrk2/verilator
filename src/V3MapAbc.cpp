// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Timing hooks into the linked libabc for the mapper
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "V3MapAbc.h"

#include <cmath>

// ABC's headers (compiled, like libabc, inside namespace abc)
#include "base/abc/abc.h"
#include "base/main/main.h"

namespace V3MapAbc {

bool setTimes(const std::vector<double>& ciArrival, const std::vector<double>& coRequired,
              double defaultRequired, std::string& error) {
    using namespace ABC_NAMESPACE;
    Abc_Frame_t* const pAbc = Abc_FrameGetGlobalFrame();
    Abc_Ntk_t* const pNtk = pAbc ? Abc_FrameReadNtk(pAbc) : nullptr;
    if (!pNtk) {
        error = "abc holds no network";
        return false;
    }
    if (static_cast<int>(ciArrival.size()) != Abc_NtkCiNum(pNtk)
        || static_cast<int>(coRequired.size()) != Abc_NtkCoNum(pNtk)) {
        error = "abc network has " + std::to_string(Abc_NtkCiNum(pNtk)) + " inputs and "
                + std::to_string(Abc_NtkCoNum(pNtk)) + " outputs, the mapper "
                + std::to_string(ciArrival.size()) + " and " + std::to_string(coRequired.size());
        return false;
    }
    Abc_NtkTimeSetDefaultArrival(pNtk, 0.0f, 0.0f);
    const float req = static_cast<float>(defaultRequired);
    Abc_NtkTimeSetDefaultRequired(pNtk, req, req);
    for (size_t i = 0; i < ciArrival.size(); ++i) {
        if (std::isnan(ciArrival[i])) continue;
        const float t = static_cast<float>(ciArrival[i]);
        Abc_NtkTimeSetArrival(pNtk, Abc_ObjId(Abc_NtkCi(pNtk, static_cast<int>(i))), t, t);
    }
    for (size_t i = 0; i < coRequired.size(); ++i) {
        if (std::isnan(coRequired[i])) continue;
        const float t = static_cast<float>(coRequired[i]);
        Abc_NtkTimeSetRequired(pNtk, Abc_ObjId(Abc_NtkCo(pNtk, static_cast<int>(i))), t, t);
    }
    return true;
}

}  // namespace V3MapAbc
