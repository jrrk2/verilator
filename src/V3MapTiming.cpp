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
//
// The timing graph follows Tatum's model: a cell pin is a node -- IPIN /
// OPIN for combinational pins, SINK / SOURCE for the data pins of clocked
// cells and the top ports, CPIN for clock pins -- a combinational arc or a
// net is an edge, a clock-to-output arc is a launch edge from the CPIN and a
// setup check a capture edge into the SINK.  Delays are a 7-series table
// (the values nextpnr-himbaechel's xc7 timing reports) and a routing
// estimate by fanout, calibrated on routed designs: the aim is to rank
// paths as the placed design will, not to predict picoseconds.
//
//*************************************************************************

#include "V3MapTiming.h"

#include "tatum/SetupAnalysis.hpp"
#include "tatum/TimingConstraints.hpp"
#include "tatum/TimingGraph.hpp"
#include "tatum/TimingGraphNameResolver.hpp"
#include "tatum/TimingReporter.hpp"
#include "tatum/analyzer_factory.hpp"
#include "tatum/delay_calc/FixedDelayCalculator.hpp"
#include "tatum/error.hpp"
#include "tatum/tags/TimingTags.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace {

using tatum::DomainId;
using tatum::EdgeId;
using tatum::EdgeType;
using tatum::NodeId;
using tatum::NodeType;
using tatum::Time;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---- 7-series cell delays, ns ----
constexpr double kLut = 0.124;  // LUT A -> O
constexpr double kMuxF = 0.08;  // MUXF7/8 I -> O
constexpr double kMuxFS = 0.126;  // MUXF7/8 S -> O
constexpr double kFfClkQ = 0.303;
constexpr double kFfSetup = 0.034;
constexpr double kFfCeSetup = 0.10;
constexpr double kFfRstSetup = 0.10;
constexpr double kBramClkQ = 1.80;  // DO without the output register
constexpr double kBramRegClkQ = 0.55;  // with it
constexpr double kBramAddrSetup = 0.42;
constexpr double kBramDataSetup = 0.30;
constexpr double kBramCtlSetup = 0.50;
constexpr double kDspComb = 3.4;  // A/B -> P, no registers
constexpr double kDspCComb = 1.5;  // C / OPMODE -> P
constexpr double kDspClkQ = 0.5;  // P register
constexpr double kDspMregClkQ = 1.6;  // M register through the post-adder
constexpr double kDspAregClkQ = 3.0;  // A/B register through multiplier and adder
constexpr double kDspInRegSetup = 0.5;  // A/B into their registers
constexpr double kDspInSetupToM = 1.9;  // A/B through the multiplier into M
constexpr double kDspInSetupToP = 3.0;  // A/B through multiplier and adder into P
constexpr double kDspCSetup = 1.2;
constexpr double kDspCtlSetup = 0.5;
constexpr double kLutramComb = 0.45;  // asynchronous read
constexpr double kLutramSetup = 0.35;
constexpr double kIbuf = 0.60;
constexpr double kIbufds = 0.90;
constexpr double kObuf = 2.0;
constexpr double kBufg = 0.10;
// Routing, by fanout: a single load is often in the same slice; the
// medians of routed designs run from 0.25 ns at fanout 2 to 1 ns above 32
double netDelay(size_t fanout) {
    if (fanout <= 1) return 0.15;
    return std::min(1.2, 0.30 + 0.012 * static_cast<double>(fanout));
}

bool startsWith(const std::string& s, const char* p) {
    return s.compare(0, std::strlen(p), p) == 0;
}
bool isConst(const std::string& net) { return net.empty() || net[0] == '"'; }

// A cell parameter as an integer: JSON "\"0101\"" binary strings, numbers
int paramInt(const V3MapTiming::Cell& c, const char* name, int dflt) {
    for (const auto& p : c.params) {
        if (p.first != name) continue;
        std::string v = p.second;
        if (!v.empty() && v[0] == '"') v = v.substr(1, v.size() - 2);
        if (v.empty()) return dflt;
        bool bin = v.size() > 1;
        for (const char ch : v) bin &= (ch == '0' || ch == '1');
        if (bin) {
            long r = 0;
            for (const char ch : v) r = (r << 1) | (ch == '1');
            return static_cast<int>(r);
        }
        return std::atoi(v.c_str());
    }
    return dflt;
}

// A real-valued parameter (MMCM multipliers), from a JSON string or number
double paramReal(const V3MapTiming::Cell& c, const char* name, double dflt) {
    for (const auto& p : c.params) {
        if (p.first != name) continue;
        std::string v = p.second;
        if (!v.empty() && v[0] == '"') v = v.substr(1, v.size() - 2);
        if (v.empty()) return dflt;
        bool bin = v.size() > 1;
        for (const char ch : v) bin &= (ch == '0' || ch == '1');
        if (bin) return paramInt(c, name, 0);
        return std::atof(v.c_str());
    }
    return dflt;
}

enum class Kind : uint8_t { CombIn, CombOut, Clk, SeqIn, SeqOut, BbIn, BbOut, Ignore };
struct PinClass final {
    Kind kind = Kind::Ignore;
    const char* clk = "";  // the clock pin of a SeqIn / SeqOut
    double t = 0;  // setup (SeqIn) or clock-to-output (SeqOut)
    double writeSetup = -1;  // a LUTRAM address: also a write-port input with this setup
};
PinClass pin(Kind k, const char* clk = "", double t = 0) {
    PinClass p;
    p.kind = k;
    p.clk = clk;
    p.t = t;
    return p;
}

bool isBufferType(const std::string& t) {
    return startsWith(t, "IBUF") || startsWith(t, "BUFG") || startsWith(t, "BUFH") || t == "BUFR"
           || t == "BUFMR" || t == "BUFIO";
}

PinClass classify(const V3MapTiming::Cell& c, const std::string& p, bool out) {
    const std::string& t = c.type;
    if (startsWith(t, "LUT") || t == "MUXF7" || t == "MUXF8" || t == "CARRY4" || t == "INV")
        return pin(out ? Kind::CombOut : Kind::CombIn);
    if (startsWith(t, "FD")) {
        if (p == "C") return pin(Kind::Clk);
        if (out) return pin(Kind::SeqOut, "C", kFfClkQ);
        if (p == "D") return pin(Kind::SeqIn, "C", kFfSetup);
        if (p == "CE") return pin(Kind::SeqIn, "C", kFfCeSetup);
        return pin(Kind::SeqIn, "C", kFfRstSetup);
    }
    if (t == "LDCE" || t == "LDPE") {
        if (p == "G") return pin(Kind::Clk);
        if (out) return pin(Kind::SeqOut, "G", kFfClkQ);
        return pin(Kind::SeqIn, "G", p == "D" ? kFfSetup : kFfRstSetup);
    }
    if (startsWith(t, "RAMB")) {
        if (p == "CLKARDCLK" || p == "CLKBWRCLK") return pin(Kind::Clk);
        if (out) {
            if (startsWith(p, "DOADO") || startsWith(p, "DOPADOP"))
                return pin(Kind::SeqOut, "CLKARDCLK",
                           paramInt(c, "DOA_REG", 0) ? kBramRegClkQ : kBramClkQ);
            if (startsWith(p, "DOBDO") || startsWith(p, "DOPBDOP"))
                return pin(Kind::SeqOut, "CLKBWRCLK",
                           paramInt(c, "DOB_REG", 0) ? kBramRegClkQ : kBramClkQ);
            return pin(Kind::Ignore);
        }
        static const char* const aPins[]
            = {"ADDRARDADDR", "DIADI",         "DIPADIP",       "WEA",
               "ENARDEN",     "RSTRAMARSTRAM", "RSTREGARSTREG", "REGCEAREGCE"};
        static const char* const bPins[] = {"ADDRBWRADDR", "DIBDI",   "DIPBDIP", "WEBWE",
                                            "ENBWREN",     "RSTRAMB", "RSTREGB", "REGCEB"};
        const double setup = startsWith(p, "ADDR") ? kBramAddrSetup
                             : startsWith(p, "DI") ? kBramDataSetup
                                                   : kBramCtlSetup;
        for (const char* a : aPins)
            if (p == a) return pin(Kind::SeqIn, "CLKARDCLK", setup);
        for (const char* b : bPins)
            if (p == b) return pin(Kind::SeqIn, "CLKBWRCLK", setup);
        return pin(Kind::Ignore);
    }
    if (t == "DSP48E1") {
        const int areg = paramInt(c, "AREG", 0), breg = paramInt(c, "BREG", 0);
        const int mreg = paramInt(c, "MREG", 0), preg = paramInt(c, "PREG", 0);
        const bool clocked = areg || breg || mreg || preg;
        if (p == "CLK") return pin(clocked ? Kind::Clk : Kind::Ignore);
        if (out) {
            if (p != "P") return pin(Kind::Ignore);
            if (preg) return pin(Kind::SeqOut, "CLK", kDspClkQ);
            if (mreg) return pin(Kind::SeqOut, "CLK", kDspMregClkQ);
            if (areg || breg) return pin(Kind::SeqOut, "CLK", kDspAregClkQ);
            return pin(Kind::CombOut);
        }
        if (p == "A" || p == "B") {
            if (p == "A" ? areg : breg) return pin(Kind::SeqIn, "CLK", kDspInRegSetup);
            if (mreg) return pin(Kind::SeqIn, "CLK", kDspInSetupToM);
            if (preg) return pin(Kind::SeqIn, "CLK", kDspInSetupToP);
            return pin(Kind::CombIn);
        }
        if (p == "C") return preg ? pin(Kind::SeqIn, "CLK", kDspCSetup) : pin(Kind::CombIn);
        if (startsWith(p, "CE") || startsWith(p, "RST"))
            return pin(clocked ? Kind::SeqIn : Kind::Ignore, "CLK", kDspCtlSetup);
        if (p == "OPMODE" || p == "ALUMODE" || p == "INMODE" || p == "CARRYINSEL"
            || p == "CARRYIN")
            return (preg || mreg) ? pin(Kind::SeqIn, "CLK", kDspCtlSetup) : pin(Kind::CombIn);
        return pin(Kind::Ignore);
    }
    if (startsWith(t, "RAM")
        && (t.find("X1") != std::string::npos || t.back() == 'M' || t.back() == 'S')) {
        // distributed RAM (RAM64X1D, RAM32M, ...): asynchronous read, clocked write
        if (p == "WCLK") return pin(Kind::Clk);
        if (out) return pin(Kind::CombOut);
        if (startsWith(p, "DPRA")) return pin(Kind::CombIn);
        if (p == "WE" || p[0] == 'D') return pin(Kind::SeqIn, "WCLK", kLutramSetup);
        if (p[0] == 'A') {
            PinClass pc = pin(Kind::CombIn);
            pc.clk = "WCLK";
            pc.writeSetup = kLutramSetup;
            return pc;
        }
        return pin(Kind::Ignore);
    }
    if (isBufferType(t) || startsWith(t, "OBUF") || startsWith(t, "IOBUF")) {
        if (out) return pin(p == "IO" || p == "IOB" ? Kind::Ignore : Kind::CombOut);
        if (p == "CE" || p == "IO" || p == "IOB" || p == "CLR" || p == "CEMASK" || p == "IGNORE")
            return pin(Kind::Ignore);
        return pin(Kind::CombIn);
    }
    if (t == "VCC" || t == "GND") return pin(out ? Kind::BbOut : Kind::Ignore);
    return pin(out ? Kind::BbOut : Kind::BbIn);  // a black box: unconstrained both ways
}

// Combinational arc delay, NaN when the cell has no such arc
double combDelay(const V3MapTiming::Cell& c, const std::string& in, int ib, const std::string& out,
                 int ob) {
    const std::string& t = c.type;
    if (startsWith(t, "LUT") || t == "INV") return kLut;
    if (t == "MUXF7" || t == "MUXF8") return in == "S" ? kMuxFS : kMuxF;
    if (t == "CARRY4") {
        const bool co = out == "CO";
        if (in == "CI" || in == "CYINIT") return co ? 0.03 + 0.007 * ob : 0.09 + 0.01 * ob;
        if (in == "S") {
            if (co) return ob >= ib ? 0.12 + 0.03 * (ob - ib) : kNaN;
            return ob == ib ? 0.08 : ob > ib ? 0.14 + 0.03 * (ob - ib - 1) : kNaN;
        }
        if (in == "DI") {
            if (co) return ob >= ib ? 0.14 + 0.03 * (ob - ib) : kNaN;
            return ob > ib ? 0.16 + 0.03 * (ob - ib - 1) : kNaN;
        }
        return kNaN;
    }
    if (t == "DSP48E1") {
        if (out != "P") return kNaN;
        return (in == "A" || in == "B") ? kDspComb : kDspCComb;
    }
    if (startsWith(t, "RAM")) {
        if (startsWith(in, "DPRA")) return startsWith(out, "DPO") ? kLutramComb : kNaN;
        if (startsWith(in, "ADDR"))  // RAM32M: ADDRA -> DOA
            return startsWith(out, "DO") && out.size() == 3 && in.size() == 5 && out[2] == in[4]
                       ? kLutramComb
                       : kNaN;
        if (in[0] == 'A') return (out == "SPO" || out == "O") ? kLutramComb : kNaN;
        return kNaN;
    }
    if (startsWith(t, "IBUF")) return t.find("DS") != std::string::npos ? kIbufds : kIbuf;
    if (startsWith(t, "OBUF") || startsWith(t, "IOBUF")) return kObuf;
    if (isBufferType(t)) return kBufg;
    return kNaN;
}

// The tightest launch-to-capture distance between two related clocks: the
// least positive gap from a rising edge of A (period pa, first edge at
// offa) to the next rising edge of B, over their common period
double minEdgeDistance(double pa, double offa, double pb, double offb) {
    const long a = std::lround(pa * 1000), b = std::lround(pb * 1000);
    long g = a, h = b;
    while (h) {
        const long t = g % h;
        g = h;
        h = t;
    }
    const long edgesA = std::min<long>(b / g, 100000);  // launch edges in the common period
    double best = pb;
    for (long k = 0; k < edgesA; ++k) {
        const double t = offa + k * pa;
        const double m = std::ceil((t - offb) / pb + 1e-9);
        const double gap = offb + m * pb - t;
        if (gap > 1e-6 && gap < best) best = gap;
    }
    return best;
}

// Tcl-style * and ? match
bool globMatch(const char* pat, const char* s) {
    for (; *pat; ++pat, ++s) {
        if (*pat == '*') {
            for (const char* q = s;; ++q) {
                if (globMatch(pat + 1, q)) return true;
                if (!*q) return false;
            }
        }
        if (!*s || (*pat != '?' && *pat != *s)) return false;
    }
    return !*s;
}

}  // namespace

//######################################################################

struct V3MapTiming::Impl final {
    const V3MapSdcConstraints& sdc;
    const Options opt;
    tatum::TimingGraph tg;
    tatum::TimingConstraints tc;
    std::vector<std::string> nodeName, nodeType;  // by NodeId
    std::vector<std::string> nodeNet;  // the net a pin node is on (empty for ports' own nodes etc)
    std::vector<int> nodeCell;  // the cell a pin node belongs to, -1 for ports and helpers
    std::vector<double> edgeDelay, edgeSetup;  // by EdgeId
    struct Net final {
        NodeId driver;
        std::vector<NodeId> sinks;
    };
    std::unordered_map<std::string, Net> nets;
    std::map<std::string, std::vector<NodeId>> portNodes;  // "led" -> all bits, "led[3]" -> one
    std::unordered_map<std::string, NodeId> pinNodes;  // "cell/PORT" or "cell/PORT[3]" -> node
    std::map<DomainId, double> periods;
    struct Wave final {  // a clock's edges, for the constraints between related clocks
        double period = 0, offset = 0;
        int family = -1;  // clocks of one MMCM, with the clock feeding it
    };
    std::map<DomainId, Wave> waves;
    NodeId constNode;
    const std::vector<Cell>* cells = nullptr;
    std::unique_ptr<tatum::FixedDelayCalculator> dc;
    std::unique_ptr<tatum::SetupTimingAnalyzer> analyzer;
    Summary summary;

    Impl(const V3MapSdcConstraints& s, const Options& o)
        : sdc{s}
        , opt{o} {}

    NodeId addNode(NodeType t, const std::string& name, const std::string& type,
                   const std::string& net = "", int cell = -1) {
        const NodeId id = tg.add_node(t);
        nodeName.push_back(name);
        nodeType.push_back(type);
        nodeNet.push_back(net);
        nodeCell.push_back(cell);
        return id;
    }
    EdgeId addEdge(EdgeType t, NodeId a, NodeId b, double delay, double setup = 0) {
        const EdgeId id = tg.add_edge(t, a, b);
        edgeDelay.push_back(delay);
        edgeSetup.push_back(setup);
        return id;
    }
    void warn(const std::string& s) { summary.warnings.push_back(s); }

    void drive(const std::string& net, NodeId n) {
        if (isConst(net)) return;
        nets[net].driver = n;
    }
    void sink(const std::string& net, NodeId n) {
        if (isConst(net)) {
            addEdge(EdgeType::INTERCONNECT, constNode, n, 0);
            return;
        }
        nets[net].sinks.push_back(n);
    }

    void build(const std::vector<Cell>& cellsIn, const std::vector<Port>& ports) {
        cells = &cellsIn;
        constNode = addNode(NodeType::SOURCE, "<const>", "const");
        tc.set_constant_generator(constNode);
        for (const Port& p : ports) {
            const bool in = p.dir == "input";
            if (!in && p.dir != "output") continue;  // an inout pad: its IOBUF carries the timing
            for (size_t b = 0; b < p.bits.size(); ++b) {
                const std::string bn = p.name + "[" + std::to_string(b) + "]";
                const std::string nm = p.bits.size() > 1 ? bn : p.name;
                const NodeId n
                    = addNode(in ? NodeType::SOURCE : NodeType::SINK, nm, p.dir, p.bits[b]);
                if (in)
                    drive(p.bits[b], n);
                else
                    sink(p.bits[b], n);
                portNodes[p.name].push_back(n);
                portNodes[bn].push_back(n);
            }
        }
        struct PinNode final {
            NodeId n;
            PinClass pc;
            std::string port;
            int bit;
        };
        std::set<std::string> unknownTypes;
        for (size_t ci = 0; ci < cellsIn.size(); ++ci) {
            const Cell& c = cellsIn[ci];
            std::vector<PinNode> pins;
            std::map<std::string, NodeId> clkNode;
            for (const Conn& cn : c.conns) {
                for (size_t b = 0; b < cn.bits.size(); ++b) {
                    const PinClass pc = classify(c, cn.port, cn.out);
                    if (pc.kind == Kind::Ignore) continue;
                    if (pc.kind == Kind::BbIn || pc.kind == Kind::BbOut)
                        unknownTypes.insert(c.type);
                    const std::string& net = cn.bits[b];
                    const std::string pinName
                        = c.name + "/" + cn.port
                          + (cn.bits.size() > 1 ? "[" + std::to_string(b) + "]" : "");
                    if (pc.kind == Kind::Clk) {
                        if (isConst(net))
                            continue;  // a tied-off clock: the cell's pins stay unconstrained
                        const NodeId n
                            = addNode(NodeType::CPIN, pinName, c.type, net, static_cast<int>(ci));
                        clkNode[cn.port] = n;
                        pinNodes[pinName] = n;
                        sink(net, n);
                        continue;
                    }
                    const NodeType nt = pc.kind == Kind::CombIn    ? NodeType::IPIN
                                        : pc.kind == Kind::CombOut ? NodeType::OPIN
                                        : (pc.kind == Kind::SeqIn || pc.kind == Kind::BbIn)
                                            ? NodeType::SINK
                                            : NodeType::SOURCE;
                    const NodeId n = addNode(nt, pinName, c.type, net, static_cast<int>(ci));
                    pinNodes[pinName] = n;
                    if (cn.out)
                        drive(net, n);
                    else
                        sink(net, n);
                    pins.push_back({n, pc, cn.port, static_cast<int>(b)});
                    if (pc.writeSetup >= 0) {  // a LUTRAM address is also the write port's
                        const NodeId w = addNode(NodeType::SINK, pinName + "(write)", c.type, net,
                                                 static_cast<int>(ci));
                        sink(net, w);
                        PinClass wc = pin(Kind::SeqIn, pc.clk, pc.writeSetup);
                        pins.push_back({w, wc, cn.port, static_cast<int>(b)});
                    }
                    if (c.type == "VCC" || c.type == "GND") tc.set_constant_generator(n);
                }
            }
            for (const PinNode& p : pins) {
                if (p.pc.kind != Kind::SeqIn && p.pc.kind != Kind::SeqOut) continue;
                const auto it = clkNode.find(p.pc.clk);
                if (it == clkNode.end()) continue;  // no clock: unconstrained
                if (p.pc.kind == Kind::SeqIn)
                    addEdge(EdgeType::PRIMITIVE_CLOCK_CAPTURE, it->second, p.n, 0, p.pc.t);
                else
                    addEdge(EdgeType::PRIMITIVE_CLOCK_LAUNCH, it->second, p.n, p.pc.t);
            }
            for (const PinNode& i : pins) {
                if (i.pc.kind != Kind::CombIn) continue;
                for (const PinNode& o : pins) {
                    if (o.pc.kind != Kind::CombOut) continue;
                    const double d = combDelay(c, i.port, i.bit, o.port, o.bit);
                    if (!std::isnan(d)) addEdge(EdgeType::PRIMITIVE_COMBINATIONAL, i.n, o.n, d);
                }
            }
        }
        for (const std::string& t : unknownTypes)
            warn("cell type " + t + " has no timing model: treated as a black box");
        for (auto& kv : nets) {
            const Net& n = kv.second;
            if (!n.driver) continue;  // floating: its readers stay unconstrained
            const double d = netDelay(n.sinks.size());
            for (const NodeId s : n.sinks) addEdge(EdgeType::INTERCONNECT, n.driver, s, d);
        }
    }

    // ---- constraints ----
    // A clock domain's source must be a SOURCE node: a clock defined on a
    // combinational pin gets a source of its own feeding that pin
    NodeId clockSource(NodeId n) {
        if (tg.node_type(n) == NodeType::SOURCE) return n;
        const NodeId src = addNode(NodeType::SOURCE, nodeName[size_t(n)] + "(clock)", "clock");
        addEdge(EdgeType::INTERCONNECT, src, n, 0);
        return src;
    }
    std::vector<NodeId> resolveTarget(const std::string& s) {
        std::vector<NodeId> out;
        if (s.find('/') != std::string::npos) {
            auto it = pinNodes.find(s);
            if (it == pinNodes.end()) it = pinNodes.find(s + "[0]");
            if (it != pinNodes.end()) out.push_back(it->second);
            return out;
        }
        const bool glob = s.find_first_of("*?") != std::string::npos;
        if (!glob) {
            const auto it = portNodes.find(s);
            if (it != portNodes.end()) out = it->second;
            return out;
        }
        for (const auto& kv : portNodes) {
            if (kv.first.back() == ']') continue;  // match whole ports
            if (globMatch(s.c_str(), kv.first.c_str()))
                out.insert(out.end(), kv.second.begin(), kv.second.end());
        }
        return out;
    }
    std::vector<DomainId> resolveClocks(const std::vector<std::string>& names) {
        std::vector<DomainId> out;
        for (const auto& kv : periods) {
            const std::string dn = tc.clock_domain_name(kv.first);
            bool hit = names.empty();
            for (const std::string& n : names) hit |= globMatch(n.c_str(), dn.c_str());
            if (hit) out.push_back(kv.first);
        }
        return out;
    }
    DomainId newDomain(const std::string& name, double period, double offset = 0,
                       int family = -1) {
        const DomainId d = tc.create_clock_domain(name);
        periods[d] = period;
        waves[d] = {period, offset, family};
        tc.set_setup_constraint(d, d, Time(period));
        return d;
    }
    // The clock root a pin is fed from, through I/O and clock buffers, and
    // the delay of that path (nets and buffers)
    NodeId clockRoot(NodeId pin, double* delay = nullptr) {
        NodeId n = pin;
        if (delay) *delay = 0;
        for (int guard = 0; guard < 16; ++guard) {
            const auto nit = nets.find(nodeNet[size_t(n)]);
            if (nit == nets.end() || !nit->second.driver) return NodeId::INVALID();
            if (delay) *delay += netDelay(nit->second.sinks.size());
            n = nit->second.driver;
            if (tg.node_type(n) != NodeType::OPIN) return n;
            const int ci = nodeCell[size_t(n)];
            if (ci < 0 || !isBufferType(nodeType[size_t(n)])) return n;
            if (delay && cells) {
                const double d = combDelay((*cells)[ci], "I", 0, "O", 0);
                if (!std::isnan(d)) *delay += d;
            }
            // the buffer's data input: the first IPIN of the same cell
            NodeId in;
            for (size_t k = size_t(n); k-- > 0;) {
                if (nodeCell[k] != ci) break;
                if (tg.node_type(NodeId(k)) == NodeType::IPIN) in = NodeId(k);
            }
            if (!in) return n;
            n = in;
        }
        return n;
    }
    // The clock domain a node's net comes from, through the clock and I/O buffers
    DomainId domainFeeding(NodeId n) {
        const NodeId r = clockRoot(n);
        return r ? tc.node_clock_domain(r) : DomainId::INVALID();
    }
    // An MMCME2 / PLLE2 output is a clock of its own, with the period its
    // multiplier and dividers make of the input clock's (as Vivado derives
    // it): CLKOUTn = CLKIN1 * DIVCLK_DIVIDE * CLKOUTn_DIVIDE / CLKFBOUT_MULT
    // The outputs are propagated clocks: their edges sit where the input
    // clock's do at CLKIN1 (its insertion from the pad, plus any latency of
    // its own domain), less the feedback path the MMCM deskews (CLKFBOUT to
    // CLKFBIN through its buffer, for ZHOLD / BUF_IN compensation), shifted
    // by CLKOUTn_PHASE; and the clocks of one MMCM, with the clock feeding
    // it, are related: every pair gets the tightest launch-to-capture gap of
    // their waveforms as its setup constraint.
    void generatedClocks() {
        if (!cells) return;
        int families = 0;
        for (size_t ci = 0; ci < cells->size(); ++ci) {
            const Cell& c = (*cells)[ci];
            const bool mmcm = startsWith(c.type, "MMCME2");
            const bool pll = startsWith(c.type, "PLLE2");
            if (!mmcm && !pll) continue;
            double in = 0, latency = 0;
            int family = -1;
            const auto cit = pinNodes.find(c.name + "/CLKIN1");
            if (cit != pinNodes.end()) {
                double insertion = 0;
                const NodeId root = clockRoot(cit->second, &insertion);
                const DomainId d = root ? tc.node_clock_domain(root) : DomainId::INVALID();
                if (d) {
                    in = periods[d];
                    latency = insertion + tc.source_latency(d, tatum::ArrivalType::LATE).value();
                    Wave& w = waves[d];
                    if (w.family < 0) w.family = families++;
                    family = w.family;
                }
            }
            if (in <= 0) in = paramReal(c, "CLKIN1_PERIOD", 0);
            if (in <= 0) {
                warn(c.name
                     + ": no clock on CLKIN1 and no CLKIN1_PERIOD: its outputs are not clocks");
                continue;
            }
            if (family < 0) family = families++;
            // the deskew: what the feedback path takes, when it is the cell's own CLKFBOUT
            const auto fit = pinNodes.find(c.name + "/CLKFBIN");
            const auto fot = pinNodes.find(c.name + "/CLKFBOUT");
            if (fit != pinNodes.end() && fot != pinNodes.end()) {
                double fb = 0;
                const NodeId root = clockRoot(fit->second, &fb);
                if (root && root == fot->second) latency -= fb;
            }
            const double mult = paramReal(c, mmcm ? "CLKFBOUT_MULT_F" : "CLKFBOUT_MULT", 5);
            const double div = paramReal(c, "DIVCLK_DIVIDE", 1);
            for (int n = 0; n < 7; ++n) {
                const std::string odivName = (mmcm && n == 0)
                                                 ? "CLKOUT0_DIVIDE_F"
                                                 : "CLKOUT" + std::to_string(n) + "_DIVIDE";
                const double odiv = paramReal(c, odivName.c_str(), 1);
                const double phase
                    = paramReal(c, ("CLKOUT" + std::to_string(n) + "_PHASE").c_str(), 0);
                for (const char* suffix : {"", "B"}) {
                    const std::string pinName = c.name + "/CLKOUT" + std::to_string(n) + suffix;
                    const auto pit = pinNodes.find(pinName);
                    if (pit == pinNodes.end()) continue;
                    const NodeId node = pit->second;
                    if (tc.node_is_clock_source(node)) continue;  // create_clock named it
                    const auto nit = nets.find(nodeNet[size_t(node)]);
                    if (nit == nets.end() || nit->second.sinks.empty()) continue;  // unused output
                    if (mult <= 0 || div <= 0 || odiv <= 0) continue;
                    const double period = in * div * odiv / mult;
                    double offset = period * phase / 360.0 + (suffix[0] ? period / 2 : 0);
                    offset -= period * std::floor(offset / period);
                    const DomainId d = newDomain(pinName, period, offset, family);
                    tc.set_clock_domain_source(clockSource(node), d);
                    tc.set_source_latency(d, tatum::ArrivalType::EARLY, Time(latency));
                    tc.set_source_latency(d, tatum::ArrivalType::LATE, Time(latency));
                }
            }
        }
        for (const auto& a : waves) {
            if (a.second.family < 0) continue;
            for (const auto& b : waves) {
                if (a.first == b.first || b.second.family != a.second.family) continue;
                if (tc.should_analyze(a.first, b.first)) continue;  // constrained already
                tc.set_setup_constraint(a.first, b.first,
                                        Time(minEdgeDistance(a.second.period, a.second.offset,
                                                             b.second.period, b.second.offset)));
            }
        }
    }
    void constrain() {
        for (const V3MapSdcClock& c : sdc.clocks) {
            if (tc.find_clock_domain(c.name)) {
                warn("clock " + c.name + " defined twice; the second definition is ignored");
                continue;
            }
            const DomainId d = newDomain(c.name, c.period);
            for (const std::string& t : c.targets) {
                const std::vector<NodeId> nodes = resolveTarget(t);
                if (nodes.empty())
                    warn("create_clock " + c.name + ": no port or pin matches " + t);
                for (const NodeId n : nodes) tc.set_clock_domain_source(clockSource(n), d);
            }
        }
        generatedClocks();
        if (opt.defaultPeriod > 0) {
            std::map<NodeId, std::string> roots;
            for (const NodeId n : tg.nodes()) {
                if (tg.node_type(n) != NodeType::CPIN) continue;
                const NodeId r = clockRoot(n);
                if (r && !tc.node_is_clock_source(r)) roots.emplace(r, nodeName[size_t(r)]);
            }
            for (const auto& kv : roots) {
                if (tc.node_is_clock_source(kv.first)) continue;
                tc.set_clock_domain_source(clockSource(kv.first),
                                           newDomain(kv.second, opt.defaultPeriod));
            }
        } else {
            std::set<std::string> loose;
            for (const NodeId n : tg.nodes()) {
                if (tg.node_type(n) != NodeType::CPIN) continue;
                const NodeId r = clockRoot(n);
                if (r && !tc.node_is_clock_source(r)) loose.insert(nodeName[size_t(r)]);
            }
            for (const std::string& s : loose)
                warn("clock from " + s + " has no create_clock: its registers are not analysed");
        }
        for (const V3MapSdcIoDelay& io : sdc.ios) {
            if (!io.isMax) continue;
            const DomainId d = tc.find_clock_domain(io.clock);
            if (!d) {
                warn(sdc.filename + ":" + std::to_string(io.line) + ": clock " + io.clock
                     + " is not defined");
                continue;
            }
            for (const std::string& pat : io.ports) {
                const std::vector<NodeId> nodes = resolveTarget(pat);
                if (nodes.empty())
                    warn(sdc.filename + ":" + std::to_string(io.line) + ": no port matches "
                         + pat);
                for (const NodeId n : nodes) {
                    if (io.input && tg.node_type(n) == NodeType::SOURCE)
                        tc.set_input_constraint(n, d, tatum::DelayType::MAX, Time(io.delay));
                    else if (!io.input && tg.node_type(n) == NodeType::SINK)
                        tc.set_output_constraint(n, d, tatum::DelayType::MAX, Time(io.delay));
                }
            }
        }
        for (const V3MapSdcPath& p : sdc.paths) {
            for (const DomainId a : resolveClocks(p.from)) {
                for (const DomainId b : resolveClocks(p.to)) {
                    if (p.kind == V3MapSdcPath::MAX_DELAY)
                        tc.set_setup_constraint(a, b, Time(p.value));
                    else if (p.kind == V3MapSdcPath::MULTICYCLE)
                        tc.set_setup_constraint(a, b, Time(p.value * periods[b]));
                    // FALSE_PATH: paths between different clocks are not analysed unless
                    // constrained
                    else if (a == b)
                        warn("set_false_path within clock " + tc.clock_domain_name(a)
                             + " is not applied");
                }
            }
        }
    }

    void analyseGraph() {
        const auto loops = tatum::identify_combinational_loops(tg);
        for (const auto& loop : loops) {
            for (size_t i = 0; i < loop.size(); ++i) {
                const EdgeId e = tg.find_edge(loop[i], loop[(i + 1) % loop.size()]);
                if (e) tg.disable_edge(e);
            }
        }
        if (!loops.empty())
            warn(std::to_string(loops.size()) + " combinational loop(s) cut for the analysis");
        tg.levelize();
        tatum::util::linear_map<EdgeId, Time> delays, setups;
        for (size_t i = 0; i < edgeDelay.size(); ++i) {
            delays.push_back(Time(edgeDelay[i]));
            setups.push_back(Time(edgeSetup[i]));
        }
        dc = std::make_unique<tatum::FixedDelayCalculator>(delays, setups);
        analyzer = tatum::AnalyzerFactory<tatum::SetupAnalysis>::make(tg, tc, *dc);
        analyzer->update_timing();
        summary.nodes = nodeName.size();
        summary.edges = edgeDelay.size();
        summary.unconstrainedStarts = analyzer->num_unconstrained_startpoints();
        summary.unconstrainedEnds = analyzer->num_unconstrained_endpoints();
        std::map<DomainId, Domain> doms;
        for (const auto& kv : periods) {
            Domain d;
            d.name = tc.clock_domain_name(kv.first);
            d.period = kv.second;
            d.wns = std::numeric_limits<double>::infinity();
            doms[kv.first] = d;
        }
        for (const NodeId n : tg.logical_outputs()) {
            std::map<DomainId, double> worst;
            for (const tatum::TimingTag& tag : analyzer->setup_slacks(n)) {
                const DomainId d = tag.capture_clock_domain();
                const double s = tag.time().value();
                const auto it = worst.find(d);
                if (it == worst.end() || s < it->second) worst[d] = s;
            }
            for (const auto& kv : worst) {
                const auto dit = doms.find(kv.first);
                if (dit == doms.end()) continue;
                Domain& d = dit->second;
                ++d.endpoints;
                d.wns = std::min(d.wns, kv.second);
                if (kv.second < 0) {
                    ++d.failing;
                    d.tns += kv.second;
                }
            }
        }
        for (auto& kv : doms) {
            if (kv.second.endpoints == 0) kv.second.wns = kv.second.period;
            summary.domains.push_back(kv.second);
        }
    }

    double tagExtreme(const std::string& net, tatum::TagType type, bool max) const {
        const auto it = nets.find(net);
        if (it == nets.end() || !it->second.driver || !analyzer) return kNaN;
        double r = kNaN;
        for (const tatum::TimingTag& tag : analyzer->setup_tags(it->second.driver, type)) {
            const double t = tag.time().value();
            if (std::isnan(r) || (max ? t > r : t < r)) r = t;
        }
        return r;
    }
};

//######################################################################

V3MapTiming::V3MapTiming(const V3MapSdcConstraints& sdc, const Options& opt)
    : m_impl{std::make_unique<Impl>(sdc, opt)} {}
V3MapTiming::~V3MapTiming() = default;

bool V3MapTiming::analyse(const std::vector<Cell>& cells, const std::vector<Port>& ports,
                          std::string& error) {
    const V3MapSdcConstraints& sdc = m_impl->sdc;
    const Options opt = m_impl->opt;
    m_impl = std::make_unique<Impl>(sdc, opt);
    try {
        m_impl->build(cells, ports);
        m_impl->constrain();
        m_impl->analyseGraph();
    } catch (const tatum::Error& e) {
        error = std::string("timing analysis: ") + e.what();
        return false;
    } catch (const std::exception& e) {
        error = std::string("timing analysis: ") + e.what();
        return false;
    }
    return true;
}

const V3MapTiming::Summary& V3MapTiming::summary() const { return m_impl->summary; }

std::string V3MapTiming::summaryText() const {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    for (const Domain& d : m_impl->summary.domains) {
        os.precision(3);
        os << "clock " << d.name << " period " << d.period << " ns: ";
        if (!d.endpoints) {
            os << "no endpoints\n";
            continue;
        }
        os << "WNS " << d.wns << " ns, TNS " << d.tns << " ns, " << d.failing << "/" << d.endpoints
           << " endpoints failing";
        os.precision(1);
        os << " (Fmax " << d.fmax() << " MHz)\n";
    }
    if (m_impl->summary.unconstrainedStarts || m_impl->summary.unconstrainedEnds)
        os << m_impl->summary.unconstrainedStarts << " unconstrained startpoints, "
           << m_impl->summary.unconstrainedEnds << " unconstrained endpoints\n";
    return os.str();
}

double V3MapTiming::arrival(const std::string& net) const {
    return m_impl->tagExtreme(net, tatum::TagType::DATA_ARRIVAL, true);
}
double V3MapTiming::required(const std::string& net) const {
    return m_impl->tagExtreme(net, tatum::TagType::DATA_REQUIRED, false);
}

double V3MapTiming::lutDelay(int inputs, double netDelay) {
    const double net = netDelay >= 0 ? netDelay : 0.35;
    if (inputs <= 6) return kLut + net;
    return kLut + net + kMuxF * (inputs - 6);
}

double V3MapTiming::minPeriod() const {
    double p = 0;
    for (const auto& kv : m_impl->periods)
        if (p == 0 || kv.second < p) p = kv.second;
    return p;
}

double V3MapTiming::averageNetDelay() const {
    double sum = 0;
    size_t n = 0;
    for (const auto& kv : m_impl->nets) {
        const Impl::Net& net = kv.second;
        if (!net.driver || net.sinks.empty()) continue;
        const std::string& t = m_impl->nodeType[size_t(net.driver)];
        if (!startsWith(t, "LUT") && t != "MUXF7" && t != "MUXF8") continue;
        sum += netDelay(net.sinks.size());
        ++n;
    }
    return n ? sum / n : -1;
}

namespace {
class Resolver final : public tatum::TimingGraphNameResolver {
    const std::vector<std::string>& m_names;  // timing node index -> net name
    const std::vector<std::string>& m_types;  // timing node index -> cell type

public:
    Resolver(const std::vector<std::string>& names, const std::vector<std::string>& types)
        : m_names{names}
        , m_types{types} {}
    std::string node_name(tatum::NodeId node) const override { return m_names[size_t(node)]; }
    std::string node_type_name(tatum::NodeId node) const override { return m_types[size_t(node)]; }
};
}  // namespace

bool V3MapTiming::writeReport(const std::string& filename, std::string& error) const {
    if (!m_impl->analyzer) {
        error = "no timing analysis to report";
        return false;
    }
    std::ofstream os{filename};
    if (!os) {
        error = "cannot write " + filename;
        return false;
    }
    os << "# verilator --map-only-output timing report (ns; pre-placement estimate)\n";
    os << summaryText();
    for (const std::string& w : m_impl->summary.warnings) os << "# warning: " << w << "\n";
    os << "\n";
    try {
        const Resolver res{m_impl->nodeName, m_impl->nodeType};
        tatum::TimingReporter rep{res, m_impl->tg, m_impl->tc, 1.0f, 3};
        rep.report_timing_setup(os, *m_impl->analyzer, m_impl->opt.reportPaths);
        rep.report_unconstrained_setup(os, *m_impl->analyzer);
    } catch (const std::exception& e) {
        error = std::string("timing report: ") + e.what();
        return false;
    }
    return true;
}
