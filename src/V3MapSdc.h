// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Timing constraints (SDC / XDC) for the mapper
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

#ifndef VERILATOR_V3MAPSDC_H_
#define VERILATOR_V3MAPSDC_H_

#include <cstdint>
#include <string>
#include <vector>

// The timing constraints the mapper analyses against (V3MapTiming), as
// read from an SDC or XDC file by V3MapSdc.cpp through libsdcparse.  Times
// are in nanoseconds; names are as written in the file (ports may carry
// Tcl-style * and ? wildcards, pins are "cell/pin").

struct V3MapSdcClock final {
    std::string name;
    double period = 0;
    std::vector<std::string> targets;  // "port", "port[3]" or "cell/pin"; none = a virtual clock
    int line = 0;
};

struct V3MapSdcIoDelay final {
    bool input = true;  // set_input_delay, else set_output_delay
    bool isMax = true;  // -max (or neither flag); a -min only delay is not used by the setup analysis
    std::string clock;  // -clock
    double delay = 0;
    std::vector<std::string> ports;
    int line = 0;
};

struct V3MapSdcPath final {  // a path constraint between clocks
    enum Kind : uint8_t { FALSE_PATH, MAX_DELAY, MULTICYCLE };
    Kind kind = FALSE_PATH;
    std::vector<std::string> from;  // clock names; empty = any
    std::vector<std::string> to;
    double value = 0;  // ns for MAX_DELAY, cycles for MULTICYCLE
    int line = 0;
};

struct V3MapSdcConstraints final {
    std::string filename;
    std::vector<V3MapSdcClock> clocks;
    std::vector<V3MapSdcIoDelay> ios;
    std::vector<V3MapSdcPath> paths;
    std::vector<std::string> warnings;  // what was read but is not applied
    bool empty() const { return clocks.empty() && ios.empty() && paths.empty(); }
};

// Read an SDC or XDC file.  Statements that are not SDC timing commands
// (Vivado's set_property and friends) are skipped.  False, with `error`, on
// a syntax error or an unreadable file.
bool V3MapSdcRead(const std::string& filename, V3MapSdcConstraints& out, std::string& error);

#endif  // guard
