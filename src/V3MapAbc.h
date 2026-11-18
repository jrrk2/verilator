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
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#ifndef VERILATOR_V3MAPABC_H_
#define VERILATOR_V3MAPABC_H_

#include <string>
#include <vector>

// The one unit that includes ABC's own headers (V3EmitMap declares the few
// frame functions it calls itself): the mapper's timing-driven pass gives
// ABC's `if` the arrival time of every combinational input and the required
// time of every combinational output of the network it is about to map.
namespace V3MapAbc {
// On the network ABC's frame holds now (just read from the mapper's AIGER):
// ciArrival[i] is the arrival time of combinational input i, coRequired[i]
// the required time of combinational output i, in the units of the LUT
// library's delays; a NaN leaves that pin at the default (arrival 0,
// required `defaultRequired`).  False, with `error`, if the counts do not
// match the network.
bool setTimes(const std::vector<double>& ciArrival, const std::vector<double>& coRequired,
              double defaultRequired, std::string& error);
}  // namespace V3MapAbc

#endif  // guard
