// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Static timing analysis of the mapped netlist
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#ifndef VERILATOR_V3MAPTIMING_H_
#define VERILATOR_V3MAPTIMING_H_

#include "V3MapSdc.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

// Setup timing of the netlist V3EmitMap has mapped (LUTs, MUXF7/8, CARRY4,
// flip-flops, block and distributed RAM, DSP48E1, I/O and clock buffers),
// with Tatum (third_party/tatum) as the analyser: a pre-placement model of
// 7-series cell delays and fanout-estimated routing, constrained by the
// SDC/XDC clocks and I/O delays (V3MapSdc) or by one period for every clock
// (--map-only-period).  Gives the mapper the arrival time of every signal
// entering abc's logic and the required time of every signal leaving it, so
// the second mapping pass is timing driven, and a report of the worst paths.
class V3MapTiming final {
public:
    // The netlist, as V3EmitMap records it cell by cell (its JSON form)
    struct Conn final {
        std::string port;
        bool out;
        std::vector<std::string> bits;  // net ids as in the JSON: "12", or "\"0\"" / "\"1\""
    };
    struct Cell final {
        std::string type, name;
        std::string path;  // hierarchical instance path of the module it belongs to ("" = top)
        std::string leaf;  // instance name inside that module (Vivado style), "" = use name
        std::vector<std::pair<std::string, std::string>> params;  // JSON-encoded values
        std::vector<Conn> conns;
    };
    struct Port final {
        std::string name, dir;
        std::vector<std::string> bits;
    };
    struct Options final {
        double defaultPeriod
            = 0;  // ns for every clock root the constraints leave unconstrained; 0 = none
        int reportPaths = 20;  // worst paths in the report
    };
    struct Domain final {
        std::string name;
        double period = 0;
        double wns = 0, tns = 0;  // ns; wns > 0 when every endpoint meets the period
        int endpoints = 0, failing = 0;
        double fmax() const { return period - wns > 0 ? 1000.0 / (period - wns) : 0; }  // MHz
    };
    struct Summary final {
        std::vector<Domain> domains;
        size_t unconstrainedStarts = 0, unconstrainedEnds = 0;
        size_t nodes = 0, edges = 0;
        std::vector<std::string> warnings;
    };

    V3MapTiming(const V3MapSdcConstraints& sdc, const Options& opt);
    ~V3MapTiming();
    // Build the timing graph and run the setup analysis; false with `error` if that fails
    bool analyse(const std::vector<Cell>& cells, const std::vector<Port>& ports,
                 std::string& error);
    const Summary& summary() const;
    std::string summaryText() const;  // one line per clock
    // The latest data arrival at the driver of a net (NaN when nothing clocked reaches it), and
    // the earliest required time there (NaN when nothing clocked is reached from it)
    double arrival(const std::string& net) const;
    double required(const std::string& net) const;
    bool writeReport(const std::string& filename, std::string& error) const;
    // The delay abc's LUT library gives a k-input LUT level: the LUT and a net (ns); the net's
    // delay is the model's typical one unless given (averageNetDelay() of an analysed netlist)
    static double lutDelay(int inputs, double netDelay = -1);
    // The mean delay of the nets LUTs and muxes drive, in the analysed netlist
    double averageNetDelay() const;
    // The shortest clock period constrained (0 when none)
    double minPeriod() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  // the Tatum timing graph and analyser
};

#endif  // guard
