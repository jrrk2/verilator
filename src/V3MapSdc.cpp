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
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
//
// libsdcparse (third_party/sdcparse) reads the SDC subset: create_clock,
// set_input_delay / set_output_delay, set_clock_groups, set_false_path,
// set_max_delay / set_min_delay, set_multicycle_path and a few more, with
// [get_ports ...] / [get_clocks ...] / [get_pins ...] object lists.  It is
// not a Tcl interpreter, so an XDC file is filtered first: only statements
// that start with one of those commands reach it, which drops the
// set_property pin and I/O-standard lines a Vivado constraint file is
// mostly made of.
//
//*************************************************************************

#include "V3MapSdc.h"

#include "sdcparse.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace {

const std::set<std::string> kSdcCommands
    = {"create_clock",        "create_generated_clock", "set_input_delay",   "set_output_delay",
       "set_clock_groups",    "set_false_path",         "set_max_delay",     "set_min_delay",
       "set_multicycle_path", "set_clock_uncertainty",  "set_clock_latency", "set_disable_timing",
       "set_timing_derate"};

std::string firstWord(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\r') ++j;
    return s.substr(i, j - i);
}

// Join continuation lines, keep SDC timing statements, replace everything
// else by blank lines so libsdcparse's line numbers stay the file's
std::string filterSdc(std::istream& in, std::set<std::string>& skipped) {
    std::string out;
    std::string line;
    std::string stmt;
    int joined = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t end = line.size();
        while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
        if (end > 0 && line[end - 1] == '\\') {  // continued on the next line
            stmt += line.substr(0, end - 1) + " ";
            ++joined;
            continue;
        }
        stmt += line;
        const std::string w = firstWord(stmt);
        if (kSdcCommands.count(w)) {
            out += stmt + "\n";
        } else {
            if (!w.empty() && w[0] != '#') skipped.insert(w);
            out += "\n";
        }
        for (; joined > 0; --joined) out += "\n";
        stmt.clear();
    }
    return out;
}

class Collector final : public sdcparse::Callback {
    V3MapSdcConstraints& m_out;  // the constraints being collected
    std::string& m_error;  // first parse error, if any
    int m_line = 0;  // line of the command being parsed, for messages

    static std::vector<std::string> strings(const sdcparse::StringGroup& g) { return g.strings; }
    void unsupported(const std::string& what) {
        m_out.warnings.push_back(m_out.filename + ":" + std::to_string(m_line) + ": " + what
                                 + " is not applied");
    }
    static std::string fromTo(const sdcparse::StringGroup& g, bool& clocks) {
        clocks = g.type == sdcparse::StringGroupType::CLOCK;
        std::string s;
        for (const std::string& x : g.strings) s += (s.empty() ? "" : " ") + x;
        return s;
    }
    void path(V3MapSdcPath::Kind kind, const sdcparse::StringGroup& from,
              const sdcparse::StringGroup& to, double value, const char* cmd) {
        bool fc = false;
        bool tc = false;
        fromTo(from, fc);
        fromTo(to, tc);
        if ((!from.strings.empty() && !fc) || (!to.strings.empty() && !tc)) {
            unsupported(std::string(cmd) + " between objects other than clocks");
            return;
        }
        V3MapSdcPath p;
        p.kind = kind;
        p.from = from.strings;
        p.to = to.strings;
        p.value = value;
        p.line = m_line;
        m_out.paths.push_back(p);
    }

public:
    Collector(V3MapSdcConstraints& out, std::string& error)
        : m_out{out}
        , m_error{error} {}
    void start_parse() override {}
    void filename(std::string) override {}
    void lineno(int line_num) override { m_line = line_num; }
    void create_clock(const sdcparse::CreateClock& cmd) override {
        V3MapSdcClock c;
        c.name = cmd.name;
        c.period = cmd.period;
        c.line = m_line;
        if (!cmd.is_virtual) c.targets = strings(cmd.targets);
        if (c.name.empty() && !c.targets.empty()) c.name = c.targets[0];
        if (c.name.empty()) {
            unsupported("create_clock without a name or a target");
            return;
        }
        if (!(c.period > 0)) {
            unsupported("create_clock " + c.name + " without a period");
            return;
        }
        m_out.clocks.push_back(c);
    }
    void set_io_delay(const sdcparse::SetIoDelay& cmd) override {
        V3MapSdcIoDelay d;
        d.input = cmd.type == sdcparse::IoDelayType::INPUT;
        d.isMax = cmd.is_max || !cmd.is_min;
        d.clock = cmd.clock_name;
        d.delay = cmd.delay;
        d.ports = strings(cmd.target_ports);
        d.line = m_line;
        m_out.ios.push_back(d);
    }
    void set_clock_groups(const sdcparse::SetClockGroups& cmd) override {
        // Clocks in different groups are unrelated: a false path each way
        for (size_t i = 0; i < cmd.clock_groups.size(); ++i) {
            for (size_t j = 0; j < cmd.clock_groups.size(); ++j) {
                if (i == j) continue;
                V3MapSdcPath p;
                p.kind = V3MapSdcPath::FALSE_PATH;
                p.from = cmd.clock_groups[i].strings;
                p.to = cmd.clock_groups[j].strings;
                p.line = m_line;
                m_out.paths.push_back(p);
            }
        }
    }
    void set_false_path(const sdcparse::SetFalsePath& cmd) override {
        path(V3MapSdcPath::FALSE_PATH, cmd.from, cmd.to, 0, "set_false_path");
    }
    void set_min_max_delay(const sdcparse::SetMinMaxDelay& cmd) override {
        if (cmd.type != sdcparse::MinMaxType::MAX) return;  // hold side: not analysed
        path(V3MapSdcPath::MAX_DELAY, cmd.from, cmd.to, cmd.value, "set_max_delay");
    }
    void set_multicycle_path(const sdcparse::SetMulticyclePath& cmd) override {
        if (!cmd.is_setup && cmd.is_hold) return;
        path(V3MapSdcPath::MULTICYCLE, cmd.from, cmd.to, cmd.mcp_value, "set_multicycle_path");
    }
    void set_clock_uncertainty(const sdcparse::SetClockUncertainty&) override {
        unsupported("set_clock_uncertainty");
    }
    void set_clock_latency(const sdcparse::SetClockLatency&) override {
        unsupported("set_clock_latency");
    }
    void set_disable_timing(const sdcparse::SetDisableTiming&) override {
        unsupported("set_disable_timing");
    }
    void set_timing_derate(const sdcparse::SetTimingDerate&) override {
        unsupported("set_timing_derate");
    }
    void finish_parse() override {}
    void parse_error(const int curr_lineno, const std::string& near_text,
                     const std::string& msg) override {
        if (!m_error.empty()) return;
        m_error = m_out.filename + ":" + std::to_string(curr_lineno) + ": " + msg
                  + (near_text.empty() ? "" : " near '" + near_text + "'");
    }
};

}  // namespace

bool V3MapSdcRead(const std::string& filename, V3MapSdcConstraints& out, std::string& error) {
    out.filename = filename;
    std::ifstream in{filename};
    if (!in) {
        error = "cannot read constraints file " + filename;
        return false;
    }
    std::set<std::string> skipped;
    const std::string text = filterSdc(in, skipped);
    if (!skipped.empty()) {
        std::string s;
        for (const std::string& w : skipped) s += (s.empty() ? "" : ", ") + w;
        out.warnings.push_back(filename + ": skipped non-timing commands: " + s);
    }
    FILE* const fp = std::tmpfile();
    if (!fp) {
        error = "cannot create a temporary file for the constraints";
        return false;
    }
    std::fwrite(text.data(), 1, text.size(), fp);
    std::rewind(fp);
    Collector cb{out, error};
    try {
        sdcparse::sdc_parse_file(fp, cb, filename.c_str());
    } catch (const std::exception& e) {
        if (error.empty()) error = filename + ": " + e.what();
    }
    std::fclose(fp);
    return error.empty();
}
