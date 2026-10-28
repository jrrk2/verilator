// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit a technology-mapped Xilinx 7-series netlist
//
// --map-only-output <file.json>
//
//   elaborated AST
//     -> V3EmitStruct (in memory): generic primitive netlist
//     -> struct_netlist.h: flatten hierarchy, bit-blast every primitive
//        into an And-Inverter Graph (registers, block-RAM ports and the
//        wrapper's IO/clock buffers are cut points)
//     -> libabc (linked in): strash / dc2 / if -K 6   (LUT6 covering)
//     -> BLIF back in, LUT INITs rebuilt from the covers
//     -> yosys-JSON for nextpnr-himbaechel: LUT1..6, FDRE/FDSE/FDCE/FDPE,
//        RAMB18E1/RAMB36E1 byte-lane tiles (System-Verilog-suite's
//        validated geometry), IBUF/OBUF/IBUFDS/BUFG pass-through cells.
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3EmitMap.h"

#include "V3EmitStruct.h"
#include "V3Global.h"
#include "V3Os.h"
#include <unistd.h>
#include <climits>
#include <cstdlib>

#include "struct_netlist.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

// libabc entry points.  The yosys-bundled ABC is compiled as C++ inside
// `namespace abc` (ABC_USE_NAMESPACE=abc); declaring the handful of
// functions we call avoids pulling ABC's macro-heavy headers into this unit.
namespace abc {
struct Abc_Frame_t_;
typedef struct Abc_Frame_t_ Abc_Frame_t;
void Abc_Start();
void Abc_Stop();
Abc_Frame_t* Abc_FrameGetGlobalFrame();
void Abc_FrameSetBatchMode(int Mode);
int Cmd_CommandExecute(Abc_Frame_t* pAbc, const char* sCommand);
}  // namespace abc
#define ABC_NAMESPACE abc

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

using svs_netlist::Vec;

//######################################################################
// AIG gate backend for svs_netlist::Elaborator
//
// Literal encoding: 2*var + complement.  var 0 is constant false.  Net
// bits are allocated as placeholder variables first (newVar) and later
// bound to their driver by equate(); placeholders that never get a driver
// are the AIG's primary inputs (top inputs, register Qs, cell outputs).

class AigB final {
public:
    using Vec = std::vector<int>;
    AigB() {
        m_fan.emplace_back(-1, -1);  // var 0: constant
        m_alias.push_back(-1);
        m_owner.push_back(0);
    }
    int newVar() {
        m_fan.emplace_back(-1, -1);
        m_alias.push_back(-1);
        m_owner.push_back(m_scope);
        return 2 * static_cast<int>(m_fan.size() - 1);
    }
    // The instance scope new nodes are attributed to (partitioned mapping)
    void setScope(int s) { m_scope = s; }
    int scopeOf(int v) const { return m_owner[v]; }
    int T() const { return 1; }
    int F() const { return 0; }
    bool isConst(int l) const { return (l >> 1) == 0; }
    bool isTrue(int l) const { return l == 1; }
    bool isFalse(int l) const { return l == 0; }
    bool isPlaceholder(int v) const { return m_fan[v].first < 0; }
    bool hasAlias(int v) const { return m_alias[v] >= 0; }
    int nvars() const { return m_fan.size(); }

    void equate(int a, int b) {
        if (a == b) return;
        const int va = a >> 1, vb = b >> 1;
        if (va && isPlaceholder(va) && !hasAlias(va) && !m_pinned.count(va)) {
            m_alias[va] = b ^ (a & 1);
        } else if (vb && isPlaceholder(vb) && !hasAlias(vb) && !m_pinned.count(vb)) {
            m_alias[vb] = a ^ (b & 1);
        } else {
            svs_netlist::warn("net driven twice (lit " + std::to_string(a) + " vs " + std::to_string(b) + ")");
        }
    }
    int AND(int a, int b) {
        if (isFalse(a) || isFalse(b)) return 0;
        if (isTrue(a)) return b;
        if (isTrue(b)) return a;
        if (a == b) return a;
        if (a == (b ^ 1)) return 0;
        const auto key = std::make_pair(std::min(a, b), std::max(a, b));
        const auto it = m_hash.find(key);
        if (it != m_hash.end()) return it->second;
        m_fan.emplace_back(key.first, key.second);
        m_alias.push_back(-1);
        m_owner.push_back(m_scope);
        const int lit = 2 * static_cast<int>(m_fan.size() - 1);
        m_hash[key] = lit;
        return lit;
    }
    int NOT(int a) const { return a ^ 1; }
    int OR(int a, int b) { return AND(a ^ 1, b ^ 1) ^ 1; }
    int XOR(int a, int b) {
        if (isFalse(a)) return b;
        if (isFalse(b)) return a;
        if (isTrue(a)) return b ^ 1;
        if (isTrue(b)) return a ^ 1;
        if (a == b) return 0;
        if (a == (b ^ 1)) return 1;
        return OR(AND(a, b ^ 1), AND(a ^ 1, b));
    }
    int MUX(int s, int a0, int a1) {  // s ? a1 : a0
        if (isTrue(s)) return a1;
        if (isFalse(s)) return a0;
        if (a0 == a1) return a0;
        return OR(AND(s, a1), AND(s ^ 1, a0));
    }
    // ---- word level (same as the CNF backend) ----
    Vec constVec(unsigned long long v, int w) const {
        Vec r(w);
        for (int i = 0; i < w; ++i) r[i] = ((i < 64) && ((v >> i) & 1)) ? 1 : 0;
        return r;
    }
    Vec resize(const Vec& a, int w, bool sext) const {
        Vec r(w);
        for (int i = 0; i < w; ++i) r[i] = i < static_cast<int>(a.size()) ? a[i] : (sext && !a.empty() ? a.back() : 0);
        return r;
    }
    Vec NOTv(const Vec& a) const {
        Vec r(a.size());
        for (size_t i = 0; i < a.size(); ++i) r[i] = a[i] ^ 1;
        return r;
    }
    Vec ANDv(const Vec& a, const Vec& b) {
        Vec r(a.size());
        for (size_t i = 0; i < a.size(); ++i) r[i] = AND(a[i], b[i]);
        return r;
    }
    Vec ORv(const Vec& a, const Vec& b) {
        Vec r(a.size());
        for (size_t i = 0; i < a.size(); ++i) r[i] = OR(a[i], b[i]);
        return r;
    }
    Vec XORv(const Vec& a, const Vec& b) {
        Vec r(a.size());
        for (size_t i = 0; i < a.size(); ++i) r[i] = XOR(a[i], b[i]);
        return r;
    }
    Vec MUXv(int s, const Vec& a0, const Vec& a1) {
        Vec r(a0.size());
        for (size_t i = 0; i < a0.size(); ++i) r[i] = MUX(s, a0[i], a1[i]);
        return r;
    }
    int redAnd(const Vec& a) {
        int r = 1;
        for (const int l : a) r = AND(r, l);
        return r;
    }
    int redOr(const Vec& a) {
        int r = 0;
        for (const int l : a) r = OR(r, l);
        return r;
    }
    int redXor(const Vec& a) {
        int r = 0;
        for (const int l : a) r = XOR(r, l);
        return r;
    }
    // ---- hard arithmetic (CARRY4 chains, DSP48E1) ----
    // An add / subtract / ordered compare of at least MIN_CARRY bits is not
    // built from gates: its propagate (S = a ^ b) and generate (DI = a) bits
    // and initial carry become primary outputs of the AIG, its sum bits and
    // final carry fresh primary inputs, and the mapper emits a CARRY4 chain
    // between them.  ABC still maps the S functions (and whatever feeds them)
    // into the LUTs in front of the chain.  A multiply of at least MIN_DSP
    // bits is split into 24 x 17 partial products on DSP48E1s (pairs whose
    // shift is past the result width, or whose operand slice is constant 0,
    // are skipped) summed with such adders.
    static constexpr size_t MIN_CARRY = 4;
    static constexpr size_t MIN_DSP = 8;
    struct Chain {
        Vec s, di;  // per bit
        int cyinit = 0;
        Vec o;  // sum bits (placeholders), empty for a compare
        int co = -1;  // final carry (placeholder), -1 when unused
    };
    struct Dsp {
        Vec a, b;  // 25 / 18 operand bits (zero-extended)
        Vec p;  // 48 product bits (placeholders)
    };
    std::vector<Chain> m_chains;
    std::vector<Dsp> m_dsps;
    bool m_hard = true;
    std::unordered_set<int> m_pinned;  // hard-cell outputs: never aliased by equate
    int pinnedVar() {
        const int l = newVar();
        m_pinned.insert(l >> 1);
        return l;
    }
    static bool allConst(const Vec& v) {
        for (const int l : v) if ((l >> 1) != 0) return false;
        return true;
    }
    Vec add(const Vec& a, const Vec& b, int cin = 0) {
        if (m_hard && a.size() >= MIN_CARRY && !(allConst(a) && allConst(b))) {
            Chain c;
            for (size_t i = 0; i < a.size(); ++i) {
                c.s.push_back(XOR(a[i], b[i]));
                c.di.push_back(a[i]);
                c.o.push_back(pinnedVar());
            }
            c.cyinit = cin;
            m_chains.push_back(c);
            return m_chains.back().o;
        }
        return addGates(a, b, cin);
    }
    Vec addGates(const Vec& a, const Vec& b, int cin = 0) {
        Vec r(a.size());
        int c = cin;
        for (size_t i = 0; i < a.size(); ++i) {
            const int x = XOR(a[i], b[i]);
            r[i] = XOR(x, c);
            c = OR(AND(a[i], b[i]), AND(x, c));
        }
        return r;
    }
    Vec sub(const Vec& a, const Vec& b) { return add(a, NOTv(b), 1); }
    Vec neg(const Vec& a) { return add(NOTv(a), constVec(0, a.size()), 1); }
    Vec mul(const Vec& a, const Vec& b) {
        const int w = a.size();
        if (m_hard && static_cast<size_t>(w) >= MIN_DSP) {
            std::vector<Vec> parts;  // shifted partial products, w bits each
            for (int i = 0; 24 * i < w; ++i) {
                Vec ai;
                for (int k = 0; k < 24; ++k) ai.push_back(24 * i + k < w ? a[24 * i + k] : 0);
                if (allConst(ai) && std::all_of(ai.begin(), ai.end(), [](int l) { return l == 0; })) continue;
                for (int j = 0; 24 * i + 17 * j < w; ++j) {
                    Vec bj;
                    for (int k = 0; k < 17; ++k) bj.push_back(17 * j + k < w ? b[17 * j + k] : 0);
                    if (std::all_of(bj.begin(), bj.end(), [](int l) { return l == 0; })) continue;
                    Dsp d;
                    d.a = ai;
                    d.a.push_back(0);  // A[24] = 0: the 24 bits are unsigned
                    d.b = bj;
                    d.b.push_back(0);  // B[17] = 0
                    for (int k = 0; k < 48; ++k) d.p.push_back(pinnedVar());
                    m_dsps.push_back(d);
                    const int sh = 24 * i + 17 * j;
                    Vec part(w, 0);
                    for (int k = 0; k < 41 && sh + k < w; ++k) part[sh + k] = m_dsps.back().p[k];
                    parts.push_back(part);
                }
            }
            if (parts.empty()) return constVec(0, w);
            while (parts.size() > 1) {  // balanced adder tree
                std::vector<Vec> next;
                for (size_t k = 0; k + 1 < parts.size(); k += 2) next.push_back(add(parts[k], parts[k + 1]));
                if (parts.size() % 2) next.push_back(parts.back());
                parts.swap(next);
            }
            return parts[0];
        }
        Vec acc = constVec(0, w);
        for (int i = 0; i < w; ++i) {
            if (isFalse(b[i])) continue;
            Vec part(w, 0);
            for (int j = 0; i + j < w; ++j) part[i + j] = AND(a[j], b[i]);
            acc = addGates(acc, part);
        }
        return acc;
    }
    int eq(const Vec& a, const Vec& b) { return redOr(XORv(a, b)) ^ 1; }
    int ltU(const Vec& a, const Vec& b) {
        if (m_hard && a.size() >= MIN_CARRY && !(allConst(a) && allConst(b))) {
            // a < b  <=>  no carry out of a + ~b + 1
            Chain c;
            for (size_t i = 0; i < a.size(); ++i) {
                c.s.push_back(XOR(a[i], b[i] ^ 1));
                c.di.push_back(a[i]);
            }
            c.cyinit = 1;
            c.co = pinnedVar();
            m_chains.push_back(c);
            return m_chains.back().co ^ 1;
        }
        int borrow = 0;
        for (size_t i = 0; i < a.size(); ++i) borrow = OR(AND(a[i] ^ 1, b[i]), AND(OR(a[i] ^ 1, b[i]), borrow));
        return borrow;
    }
    int ltS(const Vec& a, const Vec& b) {
        Vec a2 = a, b2 = b;
        a2.back() ^= 1;
        b2.back() ^= 1;
        return ltU(a2, b2);
    }
    Vec shl(const Vec& a, const Vec& amt) {
        Vec r = a;
        const int w = a.size();
        for (size_t k = 0; k < amt.size(); ++k) {
            if (isFalse(amt[k])) continue;
            Vec sh(w, 0);
            const long long d = 1LL << std::min<size_t>(k, 62);
            for (int i = 0; i < w; ++i) if (i - d >= 0) sh[i] = r[i - d];
            r = MUXv(amt[k], r, sh);
            if (d >= w) break;
        }
        return r;
    }
    Vec shr(const Vec& a, const Vec& amt, bool arith) {
        Vec r = a;
        const int w = a.size();
        const int fill = arith ? a.back() : 0;
        for (size_t k = 0; k < amt.size(); ++k) {
            if (isFalse(amt[k])) continue;
            Vec sh(w, fill);
            const long long d = 1LL << std::min<size_t>(k, 62);
            for (int i = 0; i < w; ++i) if (i + d < w) sh[i] = r[i + d];
            r = MUXv(amt[k], r, sh);
            if (d >= w) break;
        }
        return r;
    }
    std::pair<Vec, Vec> divmodU(const Vec& a, const Vec& b) {
        const int w = a.size();
        Vec rem = constVec(0, w);
        Vec q(w, 0);
        for (int i = w - 1; i >= 0; --i) {
            Vec r2(w, 0);
            for (int j = 1; j < w; ++j) r2[j] = rem[j - 1];
            r2[0] = a[i];
            const int ge = ltU(r2, b) ^ 1;
            rem = MUXv(ge, r2, sub(r2, b));
            q[i] = ge;
        }
        const int bz = redOr(b) ^ 1;
        return {MUXv(bz, q, constVec(0, w)), MUXv(bz, rem, constVec(0, w))};
    }
    std::pair<Vec, Vec> divmodS(const Vec& a, const Vec& b) {
        const int sa = a.back(), sb = b.back();
        const Vec ua = MUXv(sa, a, neg(a)), ub = MUXv(sb, b, neg(b));
        auto qr = divmodU(ua, ub);
        const int qs = XOR(sa, sb);
        return {MUXv(qs, qr.first, neg(qr.first)), MUXv(sa, qr.second, neg(qr.second))};
    }
    // ---- resolution through placeholder aliases ----
    int resolve(int lit) {
        int guard = 0;
        while (true) {
            const int v = lit >> 1;
            if (!v || !isPlaceholder(v) || !hasAlias(v)) return lit;
            lit = m_alias[v] ^ (lit & 1);
            if (++guard > 1000000) svs_netlist::die("alias cycle in netlist (combinational loop through nets?)");
        }
    }
    const std::pair<int, int>& fanin(int v) const { return m_fan[v]; }

private:
    std::vector<std::pair<int, int>> m_fan;  // per var: (lit0, lit1) or (-1,-1) placeholder
    std::vector<int> m_alias;  // per placeholder var: driver literal or -1
    std::vector<int> m_owner;  // per var: the instance scope it was built in
    int m_scope = 0;
    std::map<std::pair<int, int>, int> m_hash;
};

//######################################################################
// Mapper

class Mapper final {
    using Design = svs_netlist::Design;
    AigB m_aig;
    Design m_d;
    std::string m_top;
    // AIG primary inputs: placeholder var -> PI index; and PI index -> var
    std::unordered_map<int, int> m_piOf;
    std::vector<int> m_piVar;
    // primary outputs
    std::vector<int> m_po;  // literals
    // yosys nets
    int m_nextNet = 2;
    std::unordered_map<int, std::string> m_netOfVar;  // PI var -> net json ("12" or "\"0\"")
    std::vector<std::string> m_poNet;  // PO index -> net json
    std::ostringstream m_cells;
    int m_ncells = 0;
    std::unordered_map<std::string, int> m_counts;
    std::vector<std::string> m_warnings;
    // --map-only-meminit: one JSON object per memory (see writeMemInit), collected as the memories are emitted
    std::vector<std::string> m_memInit;
    // instance scopes (partitioned mapping): id -> (prefix "a.b.", module type); 0 = top
    std::vector<std::pair<std::string, std::string>> m_scopes;
    std::unordered_map<std::string, int> m_scopeId;
    // Electrical rule check on the flat netlist: every bit a cell input reads
    // must be driven by a cell output or a top-level input (or be a constant).
    // The first reader of each net, for the report, and every driven net.
    std::unordered_map<std::string, std::string> m_firstLoad;
    std::unordered_set<std::string> m_drivenNets;
    std::unordered_map<std::string, std::string> m_netName;  // first named bit of each net

public:
    void run(const std::string& filename) {
        const std::string text = V3EmitStruct::emitString();
        std::map<std::string, svs_netlist::Module> mods;
        {
            svs_netlist::Lexer lx{text, "<struct>"};
            svs_netlist::Parser p{lx.run(), "<struct>"};
            p.parseFile(mods);
        }
        m_top = sanitize(v3Global.rootp()->topModulep()->prettyName());
        if (!mods.count(m_top)) {
            for (const auto& kv : mods) {  // top = a module nobody instantiates
                bool used = false;
                for (const auto& kv2 : mods) for (const auto& it : kv2.second.items) used |= it.kind == svs_netlist::Item::CELL && it.modName == kv.first;
                if (!used) { m_top = kv.first; break; }
            }
        }
        // full interfaces of the (* blackbox *) primitives, for the EDIF library
        for (const auto& kv : mods) {
            if (!kv.second.blackbox) continue;
            auto& ports = m_ifaces[kv.first];
            for (const std::string& pn : kv.second.ports) {
                for (const svs_netlist::NetDecl& d : kv.second.nets) {
                    if (d.name == pn) ports.push_back({pn, d.dir >= 2, static_cast<size_t>(d.width)});
                }
            }
        }
        m_d.label = "design";
        std::set<std::string> bb;
        svs_netlist::Elaborator<AigB> e{m_aig, mods, bb, true};
        m_scopes.emplace_back("", m_top);
        m_scopeId[""] = 0;
        e.onScope = [this](const std::string& prefix, const std::string& type) {
            auto it = m_scopeId.find(prefix);
            if (it == m_scopeId.end()) {
                it = m_scopeId.emplace(prefix, static_cast<int>(m_scopes.size())).first;
                m_scopes.emplace_back(prefix, type);
            }
            m_aig.setScope(it->second);
        };
        e.run(m_top, m_d);
        buildPOs();
        const std::string base = filename.empty() ? v3Global.opt.mapOnlyEdif() : filename;
        std::map<std::string, Lut> luts;
        std::vector<std::string> lutOrder;
        if (v3Global.opt.mapOnlyWholeProgram()) {
            const std::string aig = base + ".aig";
            const std::string blif = base + ".blif";
            writeAiger(aig);
            runAbc(aig, blif);
            readBlif(blif, luts, lutOrder);
            if (!v3Global.opt.mapOnlyKeepTemps()) {
                unlink(aig.c_str());
                unlink(blif.c_str());
            }
        } else {
            mapPartitioned(base, luts, lutOrder);
        }
        emitJson(filename, luts, lutOrder);
        for (const std::string& w : m_warnings) std::cerr << "%Warning-MAP: " << w << "\n";
    }

private:
    static std::string sanitize(const std::string& s) {
        std::string o;
        for (const char c : s) o += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') ? c : '_';
        return o;
    }
    int piIndex(int var) {
        const auto it = m_piOf.find(var);
        if (it != m_piOf.end()) return it->second;
        const int idx = m_piVar.size();
        m_piOf[var] = idx;
        m_piVar.push_back(var);
        return idx;
    }
    int addPO(int lit) {
        m_po.push_back(m_aig.resolve(lit));
        return m_po.size() - 1;
    }

    // What has to come out of the LUT fabric: top outputs, register D/CE/R,
    // memory-tile and black-box input pins.
    struct RegOut { int d0; int en, rst; };
    std::vector<RegOut> m_regPo;  // per register: PO index of d[0] (d bits consecutive), en, rst
    std::vector<std::vector<int>> m_cellInPo;  // per cell: PO index per input pin (consecutive bits)
    std::vector<int> m_topOutPo;  // per top output name (in m_d.outputs order): PO of bit 0

    void buildPOs() {
        for (const auto& kv : m_d.outputs) {
            m_topOutPo.push_back(m_po.size());
            for (const int l : kv.second) addPO(l);
        }
        for (const Design::Reg& r : m_d.regs) {
            RegOut ro;
            ro.d0 = m_po.size();
            for (const int l : r.d) addPO(l);
            ro.en = addPO(r.en);
            ro.rst = addPO(r.rst);
            m_regPo.push_back(ro);
        }
        for (const Design::Cell& c : m_d.cells2) {
            std::vector<int> pos;
            for (const auto& pin : c.ins) {
                pos.push_back(m_po.size());
                for (const int l : pin.second) addPO(l);
            }
            m_cellInPo.push_back(pos);
        }
        for (const AigB::Chain& ch : m_aig.m_chains) {
            ChainPo cp;
            cp.s = m_po.size();
            for (const int l : ch.s) addPO(l);
            cp.di = m_po.size();
            for (const int l : ch.di) addPO(l);
            cp.cyinit = addPO(ch.cyinit);
            m_chainPo.push_back(cp);
        }
        for (const AigB::Dsp& d : m_aig.m_dsps) {
            DspPo dp;
            dp.a = m_po.size();
            for (const int l : d.a) addPO(l);
            dp.b = m_po.size();
            for (const int l : d.b) addPO(l);
            m_dspPo.push_back(dp);
        }
    }
    struct ChainPo { int s, di, cyinit; };
    struct DspPo { int a, b; };
    std::vector<ChainPo> m_chainPo;
    std::vector<DspPo> m_dspPo;

    // ---- AIGER (binary) ----
    // Topological order of the AND nodes reachable from the POs; every
    // placeholder met on the way is registered as a primary input
    std::vector<int> topoOrder() {
        std::vector<int> order;
        std::unordered_map<int, int> mark;  // var -> 1 visiting, 2 done
        std::function<void(int)> dfs = [&](int lit) {
            const int v = m_aig.resolve(lit) >> 1;
            if (!v) return;
            const auto it = mark.find(v);
            if (it != mark.end()) return;
            if (m_aig.isPlaceholder(v)) {
                mark[v] = 2;
                piIndex(v);
                return;
            }
            mark[v] = 1;
            dfs(m_aig.fanin(v).first);
            dfs(m_aig.fanin(v).second);
            mark[v] = 2;
            order.push_back(v);
        };
        for (const int l : m_po) dfs(l);
        return order;
    }
    void writeAiger(const std::string& path) {
        const std::vector<int> order = topoOrder();
        std::unordered_map<int, int> avar;  // var -> aiger var
        for (size_t i = 0; i < m_piVar.size(); ++i) avar[m_piVar[i]] = i + 1;
        const int I = m_piVar.size();
        for (size_t k = 0; k < order.size(); ++k) avar[order[k]] = I + 1 + k;
        auto alit = [&](int lit) {
            const int r = m_aig.resolve(lit);
            const int v = r >> 1;
            if (!v) return r & 1;
            return 2 * avar.at(v) + (r & 1);
        };
        std::ofstream os{path, std::ios::binary};
        os << "aig " << (I + order.size()) << " " << I << " 0 " << m_po.size() << " " << order.size() << "\n";
        for (const int l : m_po) os << alit(l) << "\n";
        auto leb = [&](unsigned x) {
            while (x >= 0x80) {
                os.put(static_cast<char>((x & 0x7f) | 0x80));
                x >>= 7;
            }
            os.put(static_cast<char>(x));
        };
        for (size_t k = 0; k < order.size(); ++k) {
            const int v = order[k];
            const int lhs = 2 * (I + 1 + k);
            int r0 = alit(m_aig.fanin(v).first), r1 = alit(m_aig.fanin(v).second);
            if (r0 < r1) std::swap(r0, r1);
            leb(lhs - r0);
            leb(r0 - r1);
        }
        for (size_t i = 0; i < m_piVar.size(); ++i) os << "i" << i << " pi" << i << "\n";
        for (size_t i = 0; i < m_po.size(); ++i) os << "o" << i << " po" << i << "\n";
        os << "c\nverilator --map-only-output\n";
        UINFO(1, "map: AIG " << I << " inputs, " << m_po.size() << " outputs, " << order.size() << " ANDs");
    }

    void runAbc(const std::string& aig, const std::string& blif) {
        static bool started = false;
        if (!started) {
            ABC_NAMESPACE::Abc_Start();
            started = true;
        }
        ABC_NAMESPACE::Abc_Frame_t* const pAbc = ABC_NAMESPACE::Abc_FrameGetGlobalFrame();
        // Batch mode: otherwise ABC appends every command to ./abc.history,
        // which here is the design's own directory.
        ABC_NAMESPACE::Abc_FrameSetBatchMode(1);
        std::string script = v3Global.opt.mapOnlyAbcScript();
        if (script.empty()) script = "strash; dc2; if -K 6";
        const std::string cmd = "read_aiger " + aig + "; " + script + "; write_blif " + blif;
        UINFO(1, "map: abc: " << cmd);
        if (ABC_NAMESPACE::Cmd_CommandExecute(pAbc, cmd.c_str())) {
            v3fatal("abc failed on: " << cmd);
        }
    }

    // one abc LUT: inputs, output, and the on-set cover (in-pattern, value)
    struct Lut {
        std::vector<std::string> ins;
        std::string out;
        std::vector<std::pair<std::string, char>> cover;
    };

    // ---- partitioned mapping: abc once per instance ----
    // The flat AIG is cut at instance boundaries: every node belongs to the
    // scope that built it, and a scope below --map-only-partition-min AND
    // nodes is folded into its parent's partition so LUTs can still span
    // small modules.  Each partition is handed to abc on its own, with its
    // boundary nodes named v<var> (global AIG variables) so the partitions'
    // LUT netlists join up by name; abc's internal names get a partition
    // prefix.  Top outputs, register and cell inputs keep their po<i> names.
    void mapPartitioned(const std::string& base, std::map<std::string, Lut>& luts,
                        std::vector<std::string>& order) {
        const std::vector<int> topo = topoOrder();
        const int nScopes = m_scopes.size();
        std::vector<long long> ands(nScopes, 0);
        for (const int v : topo) ++ands[m_aig.scopeOf(v)];
        // parent of each scope: the prefix one instance shorter
        std::vector<int> parentOf(nScopes, 0);
        for (int s = 1; s < nScopes; ++s) {
            std::string p = m_scopes[s].first;
            if (!p.empty()) p.pop_back();
            const size_t dot = p.rfind('.');
            p = dot == std::string::npos ? std::string() : p.substr(0, dot + 1);
            const auto it = m_scopeId.find(p);
            parentOf[s] = it == m_scopeId.end() ? 0 : it->second;
        }
        // fold small scopes upward, children first
        const long long minAnds = v3Global.opt.mapOnlyPartitionMin();
        std::vector<int> part(nScopes);
        std::vector<long long> partAnds = ands;
        std::vector<int> byDepth(nScopes);
        for (int s = 0; s < nScopes; ++s) part[s] = byDepth[s] = s;
        std::sort(byDepth.begin(), byDepth.end(), [&](int a, int b) {
            return m_scopes[a].first.size() > m_scopes[b].first.size();
        });
        for (const int s : byDepth) {
            if (s == 0 || partAnds[s] >= minAnds) continue;
            const int p = parentOf[s];
            partAnds[p] += partAnds[s];
            partAnds[s] = 0;
            part[s] = p;
        }
        auto root = [&](int s) {
            while (part[s] != s) s = part[s];
            return s;
        };
        auto partOf = [&](int v) { return root(m_aig.scopeOf(v)); };
        {
            std::map<std::string, std::pair<int, long long>> byType;  // type -> (instances, ANDs)
            for (int s = 0; s < nScopes; ++s) {
                auto& e = byType[m_scopes[s].second];
                ++e.first;
                e.second += ands[s];
            }
            std::vector<std::pair<long long, std::string>> big;
            for (const auto& kv : byType) big.emplace_back(kv.second.second, kv.first);
            std::sort(big.rbegin(), big.rend());
            int nParts = 0;
            for (int s = 0; s < nScopes; ++s) nParts += part[s] == s && partAnds[s] > 0;
            std::cerr << "%Info: partition: " << topo.size() << " AND nodes in " << nScopes
                      << " instances, " << nParts << " partitions (min " << minAnds << " ANDs)\n";
            for (size_t i = 0; i < big.size() && i < 25 && big[i].first; ++i) {
                std::cerr << "%Info: partition: " << big[i].second << " x" << byType[big[i].second].first
                          << " ands=" << big[i].first << "\n";
            }
        }
        struct Part {
            std::vector<int> ins, ands, outs;
            std::unordered_set<int> inSet, outSet;
            std::vector<std::pair<int, std::string>> pos;  // (literal, po<i>)
        };
        std::map<int, Part> parts;
        auto noteIn = [&](Part& P, int v) {
            if (P.inSet.insert(v).second) P.ins.push_back(v);
        };
        auto noteOut = [&](int v) {
            Part& O = parts[partOf(v)];
            if (O.outSet.insert(v).second) O.outs.push_back(v);
        };
        for (const int v : topo) {
            const int p = partOf(v);
            Part& P = parts[p];
            P.ands.push_back(v);
            for (const int f : {m_aig.fanin(v).first, m_aig.fanin(v).second}) {
                const int fv = m_aig.resolve(f) >> 1;
                if (!fv) continue;
                if (m_aig.isPlaceholder(fv)) {
                    noteIn(P, fv);
                } else if (partOf(fv) != p) {
                    noteIn(P, fv);
                    noteOut(fv);
                }
            }
        }
        for (size_t i = 0; i < m_po.size(); ++i) {
            const int r = m_aig.resolve(m_po[i]);
            const int v = r >> 1;
            if (!v || m_aig.isPlaceholder(v)) continue;  // constant / input: passed through by emitJson
            parts[partOf(v)].pos.emplace_back(r, "po" + std::to_string(i));
        }
        int k = 0;
        for (auto& kv : parts) {
            Part& P = kv.second;
            if (P.ands.empty()) continue;
            const std::string tag = "p" + std::to_string(k++);
            const std::string aig = base + "." + tag + ".aig";
            const std::string blif = base + "." + tag + ".blif";
            std::unordered_map<int, int> avar;
            for (size_t i = 0; i < P.ins.size(); ++i) avar[P.ins[i]] = i + 1;
            const int I = P.ins.size();
            for (size_t j = 0; j < P.ands.size(); ++j) avar[P.ands[j]] = I + 1 + j;
            auto alit = [&](int lit) {
                const int r = m_aig.resolve(lit);
                const int v = r >> 1;
                if (!v) return r & 1;
                return 2 * avar.at(v) + (r & 1);
            };
            std::vector<std::pair<int, std::string>> outs;
            for (const int v : P.outs) outs.emplace_back(2 * v, "v" + std::to_string(v));
            for (const auto& po : P.pos) outs.push_back(po);
            {
                std::ofstream os{aig, std::ios::binary};
                os << "aig " << (I + P.ands.size()) << " " << I << " 0 " << outs.size() << " "
                   << P.ands.size() << "\n";
                for (const auto& o : outs) os << alit(o.first) << "\n";
                auto leb = [&](unsigned x) {
                    while (x >= 0x80) {
                        os.put(static_cast<char>((x & 0x7f) | 0x80));
                        x >>= 7;
                    }
                    os.put(static_cast<char>(x));
                };
                for (size_t j = 0; j < P.ands.size(); ++j) {
                    const int v = P.ands[j];
                    const int lhs = 2 * (I + 1 + j);
                    int r0 = alit(m_aig.fanin(v).first), r1 = alit(m_aig.fanin(v).second);
                    if (r0 < r1) std::swap(r0, r1);
                    leb(lhs - r0);
                    leb(r0 - r1);
                }
                for (size_t i = 0; i < P.ins.size(); ++i) os << "i" << i << " v" << P.ins[i] << "\n";
                for (size_t i = 0; i < outs.size(); ++i) os << "o" << i << " " << outs[i].second << "\n";
                os << "c\nverilator --map-only-output " << tag << "\n";
            }
            const auto t0 = std::chrono::steady_clock::now();
            runAbc(aig, blif);
            std::map<std::string, Lut> pl;
            std::vector<std::string> po;
            readBlif(blif, pl, po);
            if (!v3Global.opt.mapOnlyKeepTemps()) {
                unlink(aig.c_str());
                unlink(blif.c_str());
            }
            {
                const auto& sc = m_scopes[kv.first];
                const double secs
                    = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::cerr << "%Info: partition: " << tag << " " << sc.second << " "
                          << (sc.first.empty() ? std::string("<top>") : sc.first) << " ands="
                          << P.ands.size() << " ins=" << P.ins.size() << " outs=" << outs.size()
                          << " luts=" << po.size() << " abc=" << std::fixed << std::setprecision(1)
                          << secs << "s" << std::endl;
            }
            std::unordered_set<std::string> boundary;
            for (const int v : P.ins) boundary.insert("v" + std::to_string(v));
            for (const auto& o : outs) boundary.insert(o.second);
            auto rn = [&](const std::string& n) { return boundary.count(n) ? n : tag + "_" + n; };
            for (const std::string& o : po) {
                Lut l = pl[o];
                l.out = rn(o);
                for (std::string& in : l.ins) in = rn(in);
                luts[l.out] = l;
                order.push_back(l.out);
            }
        }
        UINFO(1, "map: " << k << " partitions mapped, " << order.size() << " LUT nodes");
    }

    // ---- BLIF back ----
    void readBlif(const std::string& path, std::map<std::string, Lut>& luts, std::vector<std::string>& order) {
        std::ifstream in{path};
        if (!in) v3fatal("abc produced no BLIF: " << path);
        std::string line, cur;
        Lut* curLut = nullptr;
        auto flush = [&]() { curLut = nullptr; };
        std::string pending;
        while (std::getline(in, line)) {
            while (!line.empty() && line.back() == '\\') {
                line.pop_back();
                std::string more;
                std::getline(in, more);
                line += more;
            }
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls{line};
            std::string tok;
            ls >> tok;
            if (tok == ".names") {
                flush();
                std::vector<std::string> names;
                while (ls >> tok) names.push_back(tok);
                Lut l;
                l.out = names.back();
                names.pop_back();
                l.ins = names;
                luts[l.out] = l;
                order.push_back(l.out);
                curLut = &luts[l.out];
            } else if (tok[0] == '.') {
                flush();
            } else if (curLut) {
                if (curLut->ins.empty()) {
                    curLut->cover.emplace_back("", tok[0]);  // constant
                } else {
                    std::string val;
                    ls >> val;
                    curLut->cover.emplace_back(tok, val.empty() ? '1' : val[0]);
                }
            }
        }
    }
    static std::string lutInit(const Lut& l) {
        const int k = l.ins.size();
        const int n = 1 << k;
        std::string init(n, '0');
        if (k == 0) {
            bool one = false;
            for (const auto& c : l.cover) if (c.second == '1') one = true;
            return one ? "1" : "0";
        }
        for (int idx = 0; idx < n; ++idx) {
            bool on = false;
            bool off = false;
            for (const auto& c : l.cover) {
                bool match = true;
                for (int b = 0; b < k && match; ++b) {
                    const char p = c.first[b];
                    const int bit = (idx >> b) & 1;
                    if (p == '1' && !bit) match = false;
                    if (p == '0' && bit) match = false;
                }
                if (match) (c.second == '1' ? on : off) = true;
            }
            // BLIF: the listed cubes give the output value (normally all '1'); unlisted -> other value
            bool val;
            if (!l.cover.empty() && l.cover[0].second == '0') val = !off;  // off-set listed
            else val = on;
            init[n - 1 - idx] = val ? '1' : '0';  // MSB first string, bit idx at position n-1-idx
        }
        return init;
    }

    // ---- yosys JSON ----
    std::string newNet() { return std::to_string(m_nextNet++); }
    std::string netOfLit(int lit) {
        const int r = m_aig.resolve(lit);
        if (r == 0) return "\"0\"";
        if (r == 1) return "\"1\"";
        const int v = r >> 1;
        if (!m_aig.isPlaceholder(v)) v3fatal("internal: non-PI literal asked for a net");
        auto it = m_netOfVar.find(v);
        if (it == m_netOfVar.end()) it = m_netOfVar.emplace(v, newNet()).first;
        if (r & 1) {
            // inverted source: LUT1 inverter
            const std::string o = newNet();
            cell("LUT1", "inv_" + std::to_string(m_ncells), {{"INIT", "\"01\""}}, {{"I0", "[" + it->second + "]"}}, {{"O", "[" + o + "]"}});
            return o;
        }
        return it->second;
    }
    static std::string binParam(long long v, int w = 32) {
        std::string s(w, '0');
        for (int i = 0; i < w; ++i) if ((v >> i) & 1) s[w - 1 - i] = '1';
        return "\"" + s + "\"";
    }
    // The netlist as records, for the EDIF writer (filled alongside the JSON)
    struct Conn { std::string port; bool out; std::vector<std::string> bits; };
    std::vector<std::pair<std::string, std::string>> m_regQ;  // (Q net, register cell) for every flip-flop
    struct CellRec {
        std::string type, name;
        std::string path;  // hierarchical instance path of the module it belongs to ("" = top)
        std::string leaf;  // instance name inside that module (Vivado style), "" = use name
        std::vector<std::pair<std::string, std::string>> params;  // JSON-encoded values
        std::vector<Conn> conns;
    };
    std::vector<CellRec> m_recs;
    std::string m_curPath;  // path given to cells recorded now
    std::string m_curLeaf;  // leaf name for the next recorded cell
    std::map<std::pair<std::string, std::string>, std::string> m_netNames;  // (module path, net id) -> name
    // "core.sd.sdclk_div.SD_CLK_O" -> "SD_CLK_O"; "x[7:4]" -> ("x", 4)
    static std::string leafOf(const std::string& hname, int& lo) {
        lo = 0;
        std::string h = hname;
        const size_t br = h.find('[');
        if (br != std::string::npos) {
            int a = 0, b = 0;
            if (std::sscanf(h.c_str() + br + 1, "%d +: %d", &a, &b) == 2) lo = a;
            else if (std::sscanf(h.c_str() + br + 1, "%d:%d", &a, &b) == 2) lo = std::min(a, b);
            else if (std::sscanf(h.c_str() + br + 1, "%d", &a) == 1) lo = a;
            h = h.substr(0, br);
        }
        const size_t dot = h.rfind('.');
        return dot == std::string::npos ? h : h.substr(dot + 1);
    }
    // "core.vm.acc[3:0]" -> "core.vm"
    static std::string pathOf(const std::string& hname) {
        const size_t br = hname.find('[');
        const std::string h = br == std::string::npos ? hname : hname.substr(0, br);
        const size_t dot = h.rfind('.');
        return dot == std::string::npos ? std::string() : h.substr(0, dot);
    }
    static std::string commonPath(const std::string& a, const std::string& b) {
        if (a == b) return a;
        size_t i = 0, lastDot = 0;
        bool any = false;
        while (i < a.size() && i < b.size() && a[i] == b[i]) {
            if (a[i] == '.') { lastDot = i; any = true; }
            ++i;
        }
        if ((i == a.size() && (i == b.size() || b[i] == '.')) ) return a;
        if ((i == b.size() && a[i] == '.')) return b;
        return any ? a.substr(0, lastDot) : std::string();
    }
    struct PortRec { std::string name, dir; std::vector<std::string> bits; };
    std::vector<PortRec> m_portRecs;
    // "[12, \"0\", 13]" -> {"12", "\"0\"", "13"}
    static std::vector<std::string> splitBits(const std::string& j) {
        std::vector<std::string> out;
        std::string cur;
        for (const char ch : j) {
            if (ch == '[' || ch == ']' || ch == ' ') continue;
            if (ch == ',') { out.push_back(cur); cur.clear(); continue; }
            cur += ch;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
    void cell(const std::string& type, const std::string& name,
              const std::vector<std::pair<std::string, std::string>>& params,
              const std::vector<std::pair<std::string, std::string>>& ins,
              const std::vector<std::pair<std::string, std::string>>& outs) {
        if (!v3Global.opt.mapOnlyEdif().empty() || !v3Global.opt.mapOnlyVerilog().empty()) {
            CellRec r{type, name, m_curPath, m_curLeaf, params, {}};
            m_curLeaf.clear();
            for (const auto& p : ins) r.conns.push_back({p.first, false, splitBits(p.second)});
            for (const auto& p : outs) r.conns.push_back({p.first, true, splitBits(p.second)});
            m_recs.push_back(std::move(r));
        }
        for (const auto& p : ins) {
            const std::vector<std::string> bits = splitBits(p.second);
            for (size_t b = 0; b < bits.size(); ++b) {
                if (bits[b].empty() || bits[b][0] == '"') continue;
                m_firstLoad.emplace(bits[b], name + "." + p.first + (bits.size() > 1 ? "[" + std::to_string(b) + "]" : ""));
            }
        }
        for (const auto& p : outs) {
            for (const std::string& n : splitBits(p.second)) if (!n.empty() && n[0] != '"') m_drivenNets.insert(n);
        }
        // a register's output, for naming it if no signal does (see emitJson)
        if (type.rfind("FD", 0) == 0) {
            for (const auto& p : outs) {
                if (p.first != "Q") continue;
                const std::vector<std::string> bits = splitBits(p.second);
                if (bits.size() == 1 && !bits[0].empty() && bits[0][0] != '"') m_regQ.emplace_back(bits[0], name);
            }
        }
        if (m_ncells) m_cells << ",\n";
        ++m_ncells;
        ++m_counts[type];
        m_cells << "    \"" << name << "\": {\"hide_name\": 0, \"type\": \"" << type << "\", \"parameters\": {";
        for (size_t i = 0; i < params.size(); ++i) m_cells << (i ? ", " : "") << "\"" << params[i].first << "\": " << params[i].second;
        m_cells << "}, \"attributes\": {}, \"port_directions\": {";
        bool first = true;
        for (const auto& p : ins) { m_cells << (first ? "" : ", ") << "\"" << p.first << "\": \"input\""; first = false; }
        for (const auto& p : outs) { m_cells << (first ? "" : ", ") << "\"" << p.first << "\": \"output\""; first = false; }
        m_cells << "}, \"connections\": {";
        first = true;
        for (const auto& p : ins) { m_cells << (first ? "" : ", ") << "\"" << p.first << "\": " << p.second; first = false; }
        for (const auto& p : outs) { m_cells << (first ? "" : ", ") << "\"" << p.first << "\": " << p.second; first = false; }
        m_cells << "}}";
    }
    static std::string bitsJson(const std::vector<std::string>& nets) {
        std::string s = "[";
        for (size_t i = 0; i < nets.size(); ++i) s += (i ? ", " : "") + nets[i];
        return s + "]";
    }

    void emitJson(const std::string& filename, std::map<std::string, Lut>& luts, const std::vector<std::string>& order) {
        // 1. nets for PIs (sources) and for every LUT output; resolve BLIF names
        std::unordered_map<std::string, std::string> netOfName;  // blif name -> net json
        for (size_t i = 0; i < m_piVar.size(); ++i) {
            const int v = m_piVar[i];
            auto it = m_netOfVar.find(v);
            if (it == m_netOfVar.end()) it = m_netOfVar.emplace(v, newNet()).first;
            netOfName["pi" + std::to_string(i)] = it->second;
            netOfName["v" + std::to_string(v)] = it->second;  // the partitions' name for it
        }
        auto netOf = [&](const std::string& n) -> std::string {
            const auto it = netOfName.find(n);
            if (it == netOfName.end()) v3fatal("internal: abc BLIF name '" << n << "' has no driver");
            return it->second;
        };
        // Identity buffers (abc writes `.names piN poM / 1 1` for an output that
        // is a plain input) become net aliases, constants become "0"/"1";
        // everything else gets a fresh net.  abc's BLIF is not in topological
        // order, so first number every real LUT output, then resolve the
        // alias chains to a fixpoint.
        std::set<std::string> skip;
        std::vector<std::string> aliases;
        for (const std::string& o : order) {
            const Lut& l = luts[o];
            if (l.ins.empty()) {
                netOfName[o] = lutInit(l) == "1" ? "\"1\"" : "\"0\"";
                skip.insert(o);
            } else if (l.ins.size() == 1 && l.cover.size() == 1 && l.cover[0].first == "1" && l.cover[0].second == '1') {
                aliases.push_back(o);
                skip.insert(o);
            } else if (!netOfName.count(o)) {
                netOfName[o] = newNet();
            }
        }
        while (!aliases.empty()) {
            std::vector<std::string> left;
            for (const std::string& o : aliases) {
                const auto it = netOfName.find(luts[o].ins[0]);
                if (it != netOfName.end()) netOfName[o] = it->second;
                else left.push_back(o);
            }
            if (left.size() == aliases.size()) v3fatal("internal: abc BLIF alias '" << left[0] << "' of undefined '" << luts[left[0]].ins[0] << "'");
            aliases.swap(left);
        }
        // POs that abc wrote as .names outputs get those nets; others may be
        // direct PI buffers / constants handled by abc as LUT1s anyway
        m_poNet.assign(m_po.size(), "");
        for (size_t i = 0; i < m_po.size(); ++i) {
            const std::string pn = "po" + std::to_string(i);
            const auto it = netOfName.find(pn);
            if (it != netOfName.end()) {
                m_poNet[i] = it->second;
            } else {
                const int r = m_po[i];
                if (r == 0) m_poNet[i] = "\"0\"";
                else if (r == 1) m_poNet[i] = "\"1\"";
                else m_poNet[i] = netOfLit(r);  // PI (possibly inverted) passed straight through
            }
        }
        // LUT owners for the hierarchical EDIF: a primary output belongs to the
        // module of the register / cell / port it feeds; a LUT to the deepest
        // module containing everything it feeds (common prefix of its readers)
        std::unordered_map<std::string, std::string> owner;
        if (!v3Global.opt.mapOnlyEdif().empty()) {
            std::vector<std::string> poOwner(m_po.size(), "");
            std::vector<bool> poSet(m_po.size(), false);
            auto setPo = [&](size_t i, const std::string& p) {
                if (i >= m_po.size()) return;
                poOwner[i] = poSet[i] ? commonPath(poOwner[i], p) : p;
                poSet[i] = true;
            };
            for (size_t ri = 0; ri < m_d.regs.size(); ++ri) {
                const std::string p = pathOf(m_d.regs[ri].name);
                const RegOut& ro = m_regPo[ri];
                for (size_t b = 0; b < m_d.regs[ri].q.size(); ++b) setPo(ro.d0 + b, p);
                setPo(ro.en, p);
                setPo(ro.rst, p);
            }
            for (size_t ci = 0; ci < m_d.cells2.size(); ++ci) {
                const std::string p = pathOf(m_d.cells2[ci].name);
                for (size_t pi = 0; pi < m_d.cells2[ci].ins.size(); ++pi)
                    for (size_t b = 0; b < m_d.cells2[ci].ins[pi].second.size(); ++b) setPo(m_cellInPo[ci][pi] + b, p);
            }
            for (size_t oi = 0; oi < m_topOutPo.size(); ++oi) {
                auto it = m_d.outputs.begin();
                std::advance(it, oi);
                for (size_t b = 0; b < it->second.size(); ++b) setPo(m_topOutPo[oi] + b, "");
            }
            // readers of each BLIF name
            std::unordered_map<std::string, std::vector<std::string>> readers;
            for (const std::string& o : order) for (const std::string& in : luts[o].ins) readers[in].push_back(o);
            std::unordered_map<std::string, bool> known;
            for (const std::string& o : order) {
                if (o.rfind("po", 0) == 0) {
                    const size_t i = std::strtoul(o.c_str() + 2, nullptr, 10);
                    if (i < m_po.size() && poSet[i]) { owner[o] = poOwner[i]; known[o] = true; }
                }
            }
            // propagate backwards to a fixpoint (owners only get shallower)
            for (bool changed = true; changed;) {
                changed = false;
                for (auto it = order.rbegin(); it != order.rend(); ++it) {
                    const std::string& o = *it;
                    const auto rit = readers.find(o);
                    if (rit == readers.end()) continue;
                    for (const std::string& r : rit->second) {
                        if (!known[r]) continue;
                        const std::string np = known[o] ? commonPath(owner[o], owner[r]) : owner[r];
                        if (!known[o] || np != owner[o]) { owner[o] = np; known[o] = true; changed = true; }
                    }
                }
            }
        }
        // 2. LUT cells
        for (const std::string& o : order) {
            if (skip.count(o)) continue;
            const Lut& l = luts[o];
            {
                const auto oit = owner.find(o);
                m_curPath = oit == owner.end() ? std::string() : oit->second;
            }
            const int k = l.ins.size();
            std::vector<std::pair<std::string, std::string>> ins;
            for (int i = 0; i < k; ++i) ins.emplace_back("I" + std::to_string(i), "[" + netOf(l.ins[i]) + "]");
            cell("LUT" + std::to_string(k), "lut_" + sanitize(o) + "_" + std::to_string(m_ncells), {{"INIT", "\"" + lutInit(l) + "\""}}, ins, {{"O", "[" + netOfName[o] + "]"}});
        }
        // 2b. carry chains: blocks of four bits, the first with CI = 0 and the
        // initial carry on CYINIT, each later one's CI from the previous CO[3];
        // bits past the width propagate (S = 1, DI = 0)
        for (size_t ci = 0; ci < m_aig.m_chains.size(); ++ci) {
            const AigB::Chain& ch = m_aig.m_chains[ci];
            const ChainPo& cp = m_chainPo[ci];
            const size_t w = ch.s.size();
            const size_t nb = (w + 3) / 4;
            std::string carry = "\"0\"";
            m_curPath.clear();
            for (size_t blk = 0; blk < nb; ++blk) {
                std::vector<std::string> sv, dv, ov, cov;
                for (size_t k = 0; k < 4; ++k) {
                    const size_t i = 4 * blk + k;
                    sv.push_back(i < w ? m_poNet[cp.s + i] : "\"1\"");
                    dv.push_back(i < w ? m_poNet[cp.di + i] : "\"0\"");
                    ov.push_back(i < w && !ch.o.empty() ? netOfLit(ch.o[i]) : newNet());
                    cov.push_back(blk + 1 == nb && k == 3 && ch.co >= 0 ? netOfLit(ch.co) : newNet());
                }
                cell("CARRY4", "carry" + std::to_string(ci) + "_" + std::to_string(blk), {},
                     {{"CI", "[" + carry + "]"}, {"CYINIT", "[" + (blk == 0 ? m_poNet[cp.cyinit] : std::string("\"0\"")) + "]"},
                      {"DI", bitsJson(dv)}, {"S", bitsJson(sv)}},
                     {{"CO", bitsJson(cov)}, {"O", bitsJson(ov)}});
                carry = cov[3];
            }
        }
        // 2c. DSP48E1: combinational multiply, P = A * B (OPMODE X = Y = M, Z = 0)
        for (size_t di = 0; di < m_aig.m_dsps.size(); ++di) {
            const AigB::Dsp& d = m_aig.m_dsps[di];
            const DspPo& dp = m_dspPo[di];
            auto zeros = [](size_t n) { return std::vector<std::string>(n, "\"0\""); };
            std::vector<std::string> a = zeros(30), b = zeros(18), p, fresh;
            for (size_t k = 0; k < d.a.size(); ++k) a[k] = m_poNet[dp.a + k];
            for (size_t k = 0; k < d.b.size(); ++k) b[k] = m_poNet[dp.b + k];
            for (const int l : d.p) p.push_back(netOfLit(l));
            auto freshN = [&](size_t n) { std::vector<std::string> v; for (size_t k = 0; k < n; ++k) v.push_back(newNet()); return v; };
            const std::string z = "[\"0\"]";
            std::vector<std::pair<std::string, std::string>> params = {
                {"A_INPUT", "\"DIRECT\""}, {"B_INPUT", "\"DIRECT\""}, {"USE_DPORT", "\"FALSE\""}, {"USE_MULT", "\"MULTIPLY\""},
                {"USE_SIMD", "\"ONE48\""}};
            for (const char* r : {"ACASCREG", "ADREG", "ALUMODEREG", "AREG", "BCASCREG", "BREG", "CARRYINREG", "CARRYINSELREG",
                                  "CREG", "DREG", "INMODEREG", "MREG", "OPMODEREG", "PREG"})
                params.emplace_back(r, binParam(0));
            std::vector<std::pair<std::string, std::string>> ins = {
                {"A", bitsJson(a)}, {"B", bitsJson(b)}, {"C", bitsJson(zeros(48))}, {"D", bitsJson(zeros(25))},
                {"ACIN", bitsJson(zeros(30))}, {"BCIN", bitsJson(zeros(18))}, {"PCIN", bitsJson(zeros(48))},
                {"ALUMODE", bitsJson(zeros(4))}, {"INMODE", bitsJson(zeros(5))}, {"CARRYINSEL", bitsJson(zeros(3))},
                {"OPMODE", bitsJson({"\"1\"", "\"0\"", "\"1\"", "\"0\"", "\"0\"", "\"0\"", "\"0\""})},
                {"CARRYIN", z}, {"CARRYCASCIN", z}, {"MULTSIGNIN", z}, {"CLK", z}};
            for (const char* ce : {"CEA1", "CEA2", "CEAD", "CEALUMODE", "CEB1", "CEB2", "CEC", "CECARRYIN", "CECTRL", "CED",
                                   "CEINMODE", "CEM", "CEP", "RSTA", "RSTALLCARRYIN", "RSTALUMODE", "RSTB", "RSTC", "RSTCTRL",
                                   "RSTD", "RSTINMODE", "RSTM", "RSTP"})
                ins.emplace_back(ce, z);
            std::vector<std::pair<std::string, std::string>> outs = {
                {"P", bitsJson(p)}, {"ACOUT", bitsJson(freshN(30))}, {"BCOUT", bitsJson(freshN(18))}, {"PCOUT", bitsJson(freshN(48))},
                {"CARRYCASCOUT", "[" + newNet() + "]"}, {"CARRYOUT", bitsJson(freshN(4))}, {"MULTSIGNOUT", "[" + newNet() + "]"},
                {"OVERFLOW", "[" + newNet() + "]"}, {"UNDERFLOW", "[" + newNet() + "]"}, {"PATTERNDETECT", "[" + newNet() + "]"},
                {"PATTERNBDETECT", "[" + newNet() + "]"}};
            cell("DSP48E1", "dsp" + std::to_string(di), params, ins, outs);
        }
        // 3. registers
        for (size_t ri = 0; ri < m_d.regs.size(); ++ri) {
            const Design::Reg& r = m_d.regs[ri];
            m_curPath = pathOf(r.name);
            const RegOut& ro = m_regPo[ri];
            const std::string clk = netOfLit(r.clk);
            const std::string en = m_poNet[ro.en];
            const std::string rst = m_poNet[ro.rst];
            for (size_t b = 0; b < r.q.size(); ++b) {
                const std::string q = netOfLit(r.q[b]);
                const std::string d = m_poNet[ro.d0 + b];
                const bool setv = (r.resetVal >> b) & 1;
                const bool hasRst = rst != "\"0\"";
                std::string type;
                std::vector<std::pair<std::string, std::string>> ins = {{"C", "[" + clk + "]"}, {"CE", "[" + en + "]"}, {"D", "[" + d + "]"}};
                if (!hasRst) {
                    type = "FDRE";
                    ins.emplace_back("R", "[\"0\"]");
                } else if (r.async) {
                    type = setv ? "FDPE" : "FDCE";
                    ins.emplace_back(setv ? "PRE" : "CLR", "[" + rst + "]");
                } else {
                    type = setv ? "FDSE" : "FDRE";
                    ins.emplace_back(setv ? "S" : "R", "[" + rst + "]");
                }
                if (r.negedge) type += "_1";
                const bool initv = b < 64 && ((r.initVal >> b) & 1);
                {
                    int lo = 0;
                    const std::string base = leafOf(r.name, lo);
                    const bool vec = r.q.size() > 1 || r.name.find('[') != std::string::npos;
                    m_curLeaf = base + "_reg" + (vec ? "[" + std::to_string(lo + static_cast<int>(b)) + "]" : "");
                }
                cell(type, sanitize(r.name) + "_" + std::to_string(b), {{"INIT", initv ? "\"1\"" : "\"0\""}}, ins, {{"Q", "[" + q + "]"}});
            }
        }
        // 4. memories and pass-through cells
        for (size_t ci = 0; ci < m_d.cells2.size(); ++ci) {
            const Design::Cell& c = m_d.cells2[ci];
            m_curPath = pathOf(c.name);
            if (c.type.rfind("memory_", 0) == 0) {
                emitMemory(c, m_cellInPo[ci]);
                continue;
            }
            std::vector<std::pair<std::string, std::string>> params, ins, outs;
            for (const auto& p : c.params) {
                const std::string& v = p.second;
                if (!v.empty() && v[0] == '"') {
                    params.emplace_back(p.first, v);  // string (or real) literal
                } else if (v.rfind("bits:", 0) == 0) {
                    params.emplace_back(p.first, "\"" + v.substr(5) + "\"");  // sized literal, its width
                } else {
                    bool num = !v.empty();
                    for (const char ch : v) num &= std::isdigit(static_cast<unsigned char>(ch)) != 0;
                    params.emplace_back(p.first, num ? binParam(std::atoll(v.c_str())) : "\"" + v + "\"");
                }
            }
            for (size_t pi = 0; pi < c.ins.size(); ++pi) {
                std::vector<std::string> nets;
                for (size_t b = 0; b < c.ins[pi].second.size(); ++b) nets.push_back(m_poNet[m_cellInPo[ci][pi] + b]);
                ins.emplace_back(c.ins[pi].first, bitsJson(nets));
            }
            for (const auto& o : c.outs) {
                std::vector<std::string> nets;
                for (const int l : o.second) nets.push_back(netOfLit(l));
                outs.emplace_back(o.first, bitsJson(nets));
            }
            {
                int lo = 0;
                m_curLeaf = leafOf(c.name, lo);
            }
            cell(c.type, sanitize(c.name), params, ins, outs);
        }
        // 5. ports
        std::ostringstream ports, netnames;
        bool first = true;
        auto port = [&](const std::string& name, const char* dir, const std::vector<std::string>& nets) {
            m_portRecs.push_back({name, dir, nets});
            if (std::string(dir) != "output") for (const std::string& n : nets) m_drivenNets.insert(n);
            ports << (first ? "" : ",\n") << "    \"" << name << "\": {\"direction\": \"" << dir << "\", \"bits\": " << bitsJson(nets) << "}";
            netnames << (first ? "" : ",\n") << "    \"" << name << "\": {\"hide_name\": 0, \"bits\": " << bitsJson(nets) << ", \"attributes\": {}}";
            first = false;
        };
        // Every pad needs an IO buffer (yosys's iopadmap): a port bit that no
        // IBUF/OBUF/IOBUF/IBUFDS(_GTE2)/... touches gets one inserted here, the
        // pad on its outer side.  nextpnr then does with it what it does with
        // yosys's (GT serial pads included).
        static const std::set<std::string> ioTypes = {
            "IBUF", "IBUFG", "IBUFDS", "IBUFGDS", "IBUFDS_GTE2", "IBUFDS_DIFF_OUT", "OBUF", "OBUFT",
            "OBUFDS", "OBUFTDS", "IOBUF", "IOBUFDS"};
        m_curPath.clear();  // inserted IO buffers sit at the top
        std::set<int> bufferedVars;  // AIG vars (resolved) touching an IO buffer pin
        for (const Design::Cell& c : m_d.cells2) {
            if (!ioTypes.count(c.type)) continue;
            for (const auto& pin : c.ins) for (const int l : pin.second) bufferedVars.insert(m_aig.resolve(l) >> 1);
            for (const auto& pin : c.outs) for (const int l : pin.second) bufferedVars.insert(m_aig.resolve(l) >> 1);
        }
        for (const auto& kv : m_d.inputs) {
            std::vector<std::string> nets;
            size_t b = 0;
            for (const int l : kv.second) {
                const std::string n = netOfLit(l);
                if (bufferedVars.count(m_aig.resolve(l) >> 1)) {
                    nets.push_back(n);
                } else {
                    const std::string pad = newNet();
                    cell("IBUF", "ibuf_" + sanitize(kv.first) + "_" + std::to_string(b), {}, {{"I", "[" + pad + "]"}}, {{"O", "[" + n + "]"}});
                    nets.push_back(pad);
                }
                ++b;
            }
            port(kv.first, "input", nets);
        }
        size_t oi = 0;
        for (const auto& kv : m_d.outputs) {
            const bool inout = m_d.inouts.count(kv.first) != 0;
            std::vector<std::string> nets;
            for (size_t b = 0; b < kv.second.size(); ++b) {
                const std::string n = m_poNet[m_topOutPo[oi] + b];
                const int r = m_aig.resolve(kv.second[b]);
                // a constant output gets its OBUF too, as from iopadmap (ethmin's
                // PHY ties eth_rst_n high); var 0 is the constant, not a buffered net
                if (inout || (n[0] != '"' && bufferedVars.count(r >> 1))) {
                    nets.push_back(n);
                } else {
                    const std::string pad = newNet();
                    cell("OBUF", "obuf_" + sanitize(kv.first) + "_" + std::to_string(b), {}, {{"I", "[" + n + "]"}}, {{"O", "[" + pad + "]"}});
                    nets.push_back(pad);
                }
            }
            ++oi;
            port(kv.first, inout ? "inout" : "output", nets);
        }
        // Named nets whose bits are all fabric sources (PIs: inputs, register
        // Qs, cell outputs) or constants keep their names, for debugging and
        // for nextpnr reports; nets with no driver at all are reported.
        for (const auto& kv : m_d.nets) {
            if (m_d.inputs.count(kv.first) || m_d.outputs.count(kv.first)) continue;
            std::vector<std::string> nets;
            bool ok = true;
            bool undriven = false;
            for (size_t b = 0; b < kv.second.bits.size() && ok; ++b) {
                const int r = m_aig.resolve(kv.second.bits[b]);
                if (r <= 1) {
                    nets.push_back(r ? "\"1\"" : "\"0\"");
                } else if (m_aig.isPlaceholder(r >> 1) && !(r & 1)) {
                    const auto it = m_netOfVar.find(r >> 1);
                    if (it == m_netOfVar.end()) ok = false;
                    else nets.push_back(it->second);
                    if (b < kv.second.drivers.size() && kv.second.drivers[b] == 0) undriven = true;
                } else {
                    ok = false;
                }
            }
            (void)undriven;
            if (!ok) continue;
            for (size_t b = 0; b < nets.size(); ++b) {
                if (!nets[b].empty() && nets[b][0] != '"')
                    m_netName.emplace(nets[b], nets.size() == 1 ? kv.first : kv.first + "[" + std::to_string(b + kv.second.lsb) + "]");
            }
            netnames << ",\n    \"" << kv.first << "\": {\"hide_name\": 0, \"bits\": " << bitsJson(nets) << ", \"attributes\": {}}";
            {
                int lo = 0;
                const std::string leaf = leafOf(kv.first, lo);
                const std::string path = pathOf(kv.first);
                for (size_t b = 0; b < nets.size(); ++b) {
                    if (nets[b].empty() || nets[b][0] == '"') continue;
                    const std::string nm = nets.size() == 1 ? leaf : leaf + "[" + std::to_string(b + kv.second.lsb) + "]";
                    m_netNames.emplace(std::make_pair(path, nets[b]), nm);
                }
            }
        }
        // A register whose output no signal names -- one the mapper made (a ROM's
        // tile select) or one whose value only reaches a pad -- gets <cell>_Q.
        // Unnamed, a reader of the JSON invents a name for the net (yosys:
        // $auto$jsonparse...), and nothing that matches registers by name (an
        // LVS against the placement) can find it.  Not the cell's own name:
        // yosys keeps cells and wires in one namespace.
        for (const auto& rq : m_regQ) {
            if (m_netName.count(rq.first)) continue;
            const std::string nm = rq.second + "_Q";
            m_netName.emplace(rq.first, nm);
            netnames << ",\n    \"" << nm << "\": {\"hide_name\": 0, \"bits\": [" << rq.first
                     << "], \"attributes\": {}}";
        }
        // Floating gates: hierarchy does not come into it -- a net with no driver
        // that no cell reads is harmless wherever it is named, and one that a
        // cell reads is an error wherever it is
        {
            std::vector<std::pair<std::string, std::string>> floating;
            for (const auto& kv : m_firstLoad) {
                if (!m_drivenNets.count(kv.first)) floating.push_back(kv);
            }
            std::sort(floating.begin(), floating.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            for (size_t i = 0; i < floating.size() && i < 20; ++i) {
                const auto nit = m_netName.find(floating[i].first);
                m_warnings.push_back("floating input " + floating[i].second + ": net "
                                     + (nit != m_netName.end() ? nit->second : "#" + floating[i].first) + " has no driver");
            }
            if (floating.size() > 20)
                m_warnings.push_back(std::to_string(floating.size() - 20) + " more floating inputs");
        }
        if (!v3Global.opt.mapOnlyEdif().empty()) writeEdif(v3Global.opt.mapOnlyEdif());
        if (!v3Global.opt.mapOnlyVerilog().empty()) writeVerilog(v3Global.opt.mapOnlyVerilog());
        if (!v3Global.opt.mapOnlyMemInit().empty()) writeMemInit(v3Global.opt.mapOnlyMemInit());
        if (filename.empty()) return;  // --map-only-edif / --map-only-verilog alone
        std::ofstream os{filename};
        os << "{\n  \"creator\": \"verilator --map-only-output\",\n  \"modules\": {\n    \"" << m_top << "\": {\n"
           << "      \"attributes\": {\"top\": \"00000000000000000000000000000001\"},\n"
           << "      \"ports\": {\n" << ports.str() << "\n      },\n"
           << "      \"cells\": {\n" << m_cells.str() << "\n      },\n"
           << "      \"netnames\": {\n" << netnames.str() << "\n      }\n    }\n  }\n}\n";
        std::ostringstream summary;
        for (const auto& kv : m_counts) summary << " " << kv.first << "=" << kv.second;
        UINFO(1, "map: wrote " << filename << ":" << summary.str());
        if (v3Global.opt.structOnlyReport()) std::cerr << "%Info: map:" << summary.str() << "\n";
    }

    // ---- block RAM tiles (System-Verilog-suite fpga_bram_resolve geometry) ----
    // ---- EDIF (Vivado read_edif) ----
    // The same netlist as the JSON: an external LIB of the primitives used
    // (ports as found on the instances), one DESIGN cell holding every
    // instance, GND/VCC instances for constant bits.  Conventions follow
    // yosys's `write_edif -pvector bra`: a bus port is an array renamed to
    // "NAME[W-1:0]" whose member 0 is the MSB; a property whose JSON value is
    // a bit string is an integer up to 32 bits and a "W'hHEX" string above.
    static bool edifIdOk(const std::string& n) {
        if (n.empty() || !std::isalpha(static_cast<unsigned char>(n[0]))) return false;
        for (const char c : n) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
        return n.size() < 250;
    }
    static std::string edifQuote(const std::string& s) {
        std::string o;
        for (const char c : s) o += (c == '"' || c == '\\') ? '_' : c;
        return o;
    }
    std::string edifName(const std::string& n) {
        if (edifIdOk(n)) return n;
        return "(rename id" + std::to_string(++m_edifIds) + " \"" + edifQuote(n) + "\")";
    }
    int m_edifIds = 0;
    static std::string edifValue(const std::string& json) {
        // json is a quoted JSON string: "TDP", "0101...", "5.0"
        std::string v = json;
        if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
        bool bits = !v.empty();
        for (const char c : v) bits &= c == '0' || c == '1' || c == 'x' || c == 'z';
        if (!bits) return "(string \"" + edifQuote(v) + "\")";
        const size_t w = v.size();
        // exactly 32 bits: a Verilog integer parameter (READ_WIDTH_A, DIVCLK_DIVIDE);
        // any other width was a sized literal and Vivado types many of those
        // as binary (GTXE2 ALIGN_COMMA_ENABLE = 10'b1111111111), so spell it so
        if (w == 32) {
            uint64_t n = 0;
            for (const char c : v) n = (n << 1) | (c == '1' ? 1u : 0u);
            return "(integer " + std::to_string(n) + ")";
        }
        if (w < 32) {
            std::string b;
            for (const char c : v) b += (c == '1') ? '1' : '0';
            return "(string \"" + std::to_string(w) + "'b" + b + "\")";
        }
        std::string hex;
        for (size_t end = w; end > 0;) {  // nibbles from the LSB
            const size_t beg = end >= 4 ? end - 4 : 0;
            int d = 0;
            for (size_t i = beg; i < end; ++i) d = (d << 1) | (v[i] == '1' ? 1 : 0);
            hex.insert(hex.begin(), "0123456789abcdef"[d]);
            end = beg;
        }
        return "(string \"" + std::to_string(w) + "'h" + hex + "\")";
    }
    struct LibPort { std::string name; bool out; size_t width; };
    std::map<std::string, std::vector<LibPort>> m_ifaces;  // (* blackbox *) declarations
    // Complete interfaces of the primitives the mapper itself creates, so
    // Vivado's own cell views replace them without "additional ports" notes
    static std::vector<LibPort> builtinPorts(const std::string& t) {
        std::vector<LibPort> v;
        auto in = [&](const std::string& n, size_t w = 1) { v.push_back({n, false, w}); };
        auto out = [&](const std::string& n, size_t w = 1) { v.push_back({n, true, w}); };
        if (t.size() == 4 && t.compare(0, 3, "LUT") == 0 && t[3] >= '1' && t[3] <= '6') {
            out("O");
            for (int i = 0; i < t[3] - '0'; ++i) in("I" + std::to_string(i));
        } else if (t.rfind("FD", 0) == 0) {
            out("Q");
            in("C"); in("CE"); in("D");
            const std::string b = t.substr(0, 4);
            in(b == "FDRE" ? "R" : b == "FDSE" ? "S" : b == "FDCE" ? "CLR" : "PRE");
        } else if (t == "RAMB36E1") {
            out("CASCADEOUTA"); out("CASCADEOUTB"); out("DBITERR"); out("DOADO", 32); out("DOBDO", 32);
            out("DOPADOP", 4); out("DOPBDOP", 4); out("ECCPARITY", 8); out("RDADDRECC", 9); out("SBITERR");
            in("ADDRARDADDR", 16); in("ADDRBWRADDR", 16); in("CASCADEINA"); in("CASCADEINB"); in("CLKARDCLK");
            in("CLKBWRCLK"); in("DIADI", 32); in("DIBDI", 32); in("DIPADIP", 4); in("DIPBDIP", 4); in("ENARDEN");
            in("ENBWREN"); in("INJECTDBITERR"); in("INJECTSBITERR"); in("REGCEAREGCE"); in("REGCEB");
            in("RSTRAMARSTRAM"); in("RSTRAMB"); in("RSTREGARSTREG"); in("RSTREGB"); in("WEA", 4); in("WEBWE", 8);
        } else if (t == "RAMB18E1") {
            out("DOADO", 16); out("DOBDO", 16); out("DOPADOP", 2); out("DOPBDOP", 2);
            in("ADDRARDADDR", 14); in("ADDRBWRADDR", 14); in("CLKARDCLK"); in("CLKBWRCLK"); in("DIADI", 16);
            in("DIBDI", 16); in("DIPADIP", 2); in("DIPBDIP", 2); in("ENARDEN"); in("ENBWREN"); in("REGCEAREGCE");
            in("REGCEB"); in("RSTRAMARSTRAM"); in("RSTRAMB"); in("RSTREGARSTREG"); in("RSTREGB"); in("WEA", 2); in("WEBWE", 4);
        } else if (t == "RAM64X1D") {
            out("DPO"); out("SPO");
            for (int i = 0; i < 6; ++i) in("A" + std::to_string(i));
            in("D");
            for (int i = 0; i < 6; ++i) in("DPRA" + std::to_string(i));
            in("WCLK"); in("WE");
        } else if (t == "CARRY4") {
            out("CO", 4); out("O", 4); in("CI"); in("CYINIT"); in("DI", 4); in("S", 4);
        } else if (t == "DSP48E1") {
            out("ACOUT", 30); out("BCOUT", 18); out("CARRYCASCOUT"); out("CARRYOUT", 4); out("MULTSIGNOUT"); out("OVERFLOW");
            out("P", 48); out("PATTERNBDETECT"); out("PATTERNDETECT"); out("PCOUT", 48); out("UNDERFLOW");
            in("A", 30); in("ACIN", 30); in("ALUMODE", 4); in("B", 18); in("BCIN", 18); in("C", 48); in("CARRYCASCIN");
            in("CARRYIN"); in("CARRYINSEL", 3); in("CEA1"); in("CEA2"); in("CEAD"); in("CEALUMODE"); in("CEB1"); in("CEB2");
            in("CEC"); in("CECARRYIN"); in("CECTRL"); in("CED"); in("CEINMODE"); in("CEM"); in("CEP"); in("CLK"); in("D", 25);
            in("INMODE", 5); in("MULTSIGNIN"); in("OPMODE", 7); in("PCIN", 48); in("RSTA"); in("RSTALLCARRYIN");
            in("RSTALUMODE"); in("RSTB"); in("RSTC"); in("RSTCTRL"); in("RSTD"); in("RSTINMODE"); in("RSTM"); in("RSTP");
        } else if (t == "IBUF" || t == "OBUF" || t == "BUFG") {
            out("O"); in("I");
        }
        return v;
    }
    // The mapped netlist as structural Verilog, for simulating it against the
    // primitives' models (unisims, or yosys's cells_sim.v): one flat module of
    // the design's ports, a wire per net and an instance per cell, with the
    // same cells, parameters and connections as the JSON.  A net that is a
    // port bit is that port bit; any other takes its signal's name (or n<id>),
    // escaped as Verilog needs, and moved aside with _net if a port or a cell
    // already has it -- Verilog keeps wires and instances in one namespace.
    static std::string verilogId(const std::string& n) {
        bool plain = !n.empty() && (std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_');
        for (const char c : n)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '$') plain = false;
        return plain ? n : "\\" + n + " ";
    }
    static std::string verilogParam(const std::string& json) {
        if (json.empty() || json[0] != '"') return json;  // a plain number
        std::string v = json.substr(1, json.size() >= 2 ? json.size() - 2 : 0);
        const bool bits = !v.empty() && v.find_first_not_of("01xz") == std::string::npos;
        if (!bits) return "\"" + v + "\"";
        const size_t w = v.size();
        if (v.find_first_of("xz") != std::string::npos) return std::to_string(w) + "'b" + v;
        std::string hex;
        for (size_t end = w; end > 0;) {
            const size_t start = end >= 4 ? end - 4 : 0;
            int nib = 0;
            for (size_t i = start; i < end; ++i) nib = (nib << 1) | (v[i] == '1');
            hex.insert(hex.begin(), "0123456789abcdef"[nib]);
            end = start;
        }
        return std::to_string(w) + "'h" + hex;
    }
    void writeVerilog(const std::string& path) {
        std::set<std::string> taken;  // port and cell names: a wire may not reuse one
        std::map<std::string, std::string> ref;  // net id -> how it is written
        std::vector<std::string> assigns;
        for (const PortRec& pr : m_portRecs) {
            taken.insert(pr.name);
            for (size_t i = 0; i < pr.bits.size(); ++i) {
                const std::string& b = pr.bits[i];
                const std::string here = pr.bits.size() == 1 ? verilogId(pr.name) : verilogId(pr.name) + "[" + std::to_string(i) + "]";
                if (b.empty()) continue;
                if (b[0] == '"') {  // a constant output
                    assigns.push_back("  assign " + here + " = 1'b" + (b.size() > 1 ? b.substr(1, 1) : "x") + ";");
                    continue;
                }
                const auto it = ref.emplace(b, here);
                if (!it.second) {  // the same net on two ports: the later one follows the first
                    if (pr.dir == "input") assigns.push_back("  assign " + it.first->second + " = " + here + ";");
                    else assigns.push_back("  assign " + here + " = " + it.first->second + ";");
                }
            }
        }
        for (const CellRec& r : m_recs) taken.insert(r.name);
        std::vector<std::string> wires;
        auto bitRef = [&](const std::string& b) -> std::string {
            if (b.empty()) return "1'bx";
            if (b[0] == '"') return std::string("1'b") + (b.size() > 1 ? b.substr(1, 1) : "x");
            const auto it = ref.find(b);
            if (it != ref.end()) return it->second;
            const auto nn = m_netName.find(b);
            std::string nm = nn != m_netName.end() ? nn->second : "n" + b;
            const std::string base = nm.substr(0, nm.find('['));
            if (taken.count(nm) || taken.count(base)) nm += "_net";
            const std::string id = verilogId(nm);
            ref.emplace(b, id);
            wires.push_back(id);
            return id;
        };
        std::ostringstream body;
        for (const CellRec& r : m_recs) {
            body << "  " << r.type;
            if (!r.params.empty()) {
                body << " #(";
                for (size_t i = 0; i < r.params.size(); ++i)
                    body << (i ? ", " : "") << "." << r.params[i].first << "(" << verilogParam(r.params[i].second) << ")";
                body << ")";
            }
            body << " " << verilogId(r.name) << " (";
            for (size_t i = 0; i < r.conns.size(); ++i) {
                const Conn& c = r.conns[i];
                body << (i ? "," : "") << "\n    ." << c.port << "(";
                if (c.bits.size() == 1) {
                    body << bitRef(c.bits[0]);
                } else {
                    body << "{";
                    for (size_t k = c.bits.size(); k-- > 0;) body << bitRef(c.bits[k]) << (k ? ", " : "");
                    body << "}";
                }
                body << ")";
            }
            body << ");\n";
        }
        std::ofstream os{path};
        os << "// verilator --map-only-verilog: the mapped netlist of " << m_top << "\n";
        os << "module " << verilogId(m_top) << " (";
        for (size_t i = 0; i < m_portRecs.size(); ++i) os << (i ? ", " : "") << verilogId(m_portRecs[i].name);
        os << ");\n";
        for (const PortRec& pr : m_portRecs) {
            os << "  " << pr.dir;
            if (pr.bits.size() > 1) os << " [" << pr.bits.size() - 1 << ":0]";
            os << " " << verilogId(pr.name) << ";\n";
        }
        for (const std::string& w : wires) os << "  wire " << w << ";\n";
        for (const std::string& a : assigns) os << a << "\n";
        os << body.str() << "endmodule\n";
    }
    void writeEdif(const std::string& path) {
        // library cells: type -> ports (name, out, width): the declared or
        // built-in interface, widened by anything an instance connects
        std::map<std::string, std::vector<LibPort>> lib;
        for (const CellRec& r : m_recs) {
            if (!lib.count(r.type)) {
                const auto it = m_ifaces.find(r.type);
                lib[r.type] = it != m_ifaces.end() ? it->second : builtinPorts(r.type);
            }
        }
        for (const CellRec& r : m_recs) {
            auto& ports = lib[r.type];
            for (const Conn& c : r.conns) {
                bool have = false;
                for (LibPort& lp : ports) if (lp.name == c.port) { have = true; lp.width = std::max(lp.width, c.bits.size()); }
                if (!have) ports.push_back({c.port, c.out, c.bits.size()});
            }
        }
        // nets: id -> port references
        std::map<std::string, std::vector<std::string>> nets;
        auto ref = [&](const std::string& bit, const std::string& portRef) {
            std::string id = bit;
            if (bit == "\"0\"") id = "GND";
            else if (bit == "\"1\"") id = "VCC";
            else if (!bit.empty() && bit[0] == '"') return;  // x/z: unconnected
            nets[id].push_back(portRef);
        };
        auto member = [&](const std::string& port, size_t width, size_t bit) {
            if (width == 1) return edifName(port);
            return "(member " + edifName(port) + " " + std::to_string(width - 1 - bit) + ")";
        };
        // instance names must be unique EDIF identifiers
        std::vector<std::string> instNames;
        instNames.reserve(m_recs.size());
        for (const CellRec& r : m_recs) instNames.push_back(edifName(r.name));
        auto instRefOf = [&](size_t i) {
            const std::string& n = instNames[i];
            if (n[0] != '(') return n;
            // (rename idN "x") -> idN
            return n.substr(8, n.find(' ', 8) - 8);
        };
        for (size_t i = 0; i < m_recs.size(); ++i) {
            const CellRec& r = m_recs[i];
            for (const Conn& c : r.conns) {
                size_t width = 1;
                for (const LibPort& lp : lib[r.type]) if (lp.name == c.port) width = lp.width;
                for (size_t b = 0; b < c.bits.size(); ++b) {
                    ref(c.bits[b], "(portRef " + member(c.port, width, b) + " (instanceRef " + instRefOf(i) + "))");
                }
            }
        }
        std::map<std::string, std::string> topPortName;
        for (const PortRec& p : m_portRecs) {
            for (size_t b = 0; b < p.bits.size(); ++b) {
                ref(p.bits[b], "(portRef " + member(p.name, p.bits.size(), b) + ")");
            }
        }
        std::ofstream os{path};
        os << "(edif " << edifName(m_top) << "\n  (edifVersion 2 0 0)\n  (edifLevel 0)\n  (keywordMap (keywordLevel 0))\n"
           << "  (comment \"verilator --map-only-edif\")\n";
        os << "  (external LIB\n    (edifLevel 0)\n    (technology (numberDefinition))\n";
        auto libCell = [&](const std::string& type, const std::vector<LibPort>& ports) {
            os << "    (cell " << edifName(type) << "\n      (cellType GENERIC)\n      (view VIEW_NETLIST\n        (viewType NETLIST)\n        (interface\n";
            for (const LibPort& lp : ports) {
                const char* dir = lp.out ? "OUTPUT" : "INPUT";
                if (lp.width == 1) os << "          (port " << edifName(lp.name) << " (direction " << dir << "))\n";
                else os << "          (port (array (rename " << lp.name << " \"" << lp.name << "[" << (lp.width - 1) << ":0]\") " << lp.width << ") (direction " << dir << "))\n";
            }
            os << "        )\n      )\n    )\n";
        };
        if (nets.count("GND")) libCell("GND", {{"G", true, 1}});
        if (nets.count("VCC")) libCell("VCC", {{"P", true, 1}});
        for (const auto& kv : lib) if (kv.first != "GND" && kv.first != "VCC") libCell(kv.first, kv.second);
        os << "  )\n";
        os << "  (library DESIGN\n    (edifLevel 0)\n    (technology (numberDefinition))\n";
        if (v3Global.opt.mapOnlyEdifFlat()) {
            writeEdifFlatTop(os, nets, instNames);
        } else {
            writeEdifHier(os, lib, instNames);
        }
        os << "  )\n";
        os << "  (design " << edifName(m_top) << "\n    (cellRef " << edifName(m_top) << " (libraryRef DESIGN))\n  )\n)\n";
        UINFO(1, "map: wrote " << path << ": " << m_recs.size() << " instances, " << nets.size() << " nets");
    }

    void writeEdifFlatTop(std::ofstream& os, std::map<std::string, std::vector<std::string>>& nets,
                          const std::vector<std::string>& instNames) {
        os << "    (cell " << edifName(m_top) << "\n      (cellType GENERIC)\n      (view VIEW_NETLIST\n        (viewType NETLIST)\n        (interface\n";
        for (const PortRec& p : m_portRecs) {
            const std::string dir = p.dir == "input" ? "INPUT" : p.dir == "output" ? "OUTPUT" : "INOUT";
            if (p.bits.size() == 1) os << "          (port " << edifName(p.name) << " (direction " << dir << "))\n";
            else os << "          (port (array (rename " << p.name << " \"" << p.name << "[" << (p.bits.size() - 1) << ":0]\") " << p.bits.size() << ") (direction " << dir << "))\n";
        }
        os << "        )\n        (contents\n";
        if (nets.count("GND")) os << "          (instance GND (viewRef VIEW_NETLIST (cellRef GND (libraryRef LIB))))\n";
        if (nets.count("VCC")) os << "          (instance VCC (viewRef VIEW_NETLIST (cellRef VCC (libraryRef LIB))))\n";
        for (size_t i = 0; i < m_recs.size(); ++i) {
            const CellRec& r = m_recs[i];
            os << "          (instance " << instNames[i] << "\n            (viewRef VIEW_NETLIST (cellRef " << edifName(r.type) << " (libraryRef LIB)))";
            for (const auto& pp : r.params) os << "\n            (property " << edifName(pp.first) << " " << edifValue(pp.second) << ")";
            os << ")\n";
        }
        for (const auto& kv : nets) {
            const std::string name = kv.first == "GND" ? "GND_NET" : kv.first == "VCC" ? "VCC_NET" : "n" + kv.first;
            os << "          (net " << name << " (joined\n";
            if (kv.first == "GND") os << "              (portRef G (instanceRef GND))\n";
            if (kv.first == "VCC") os << "              (portRef P (instanceRef VCC))\n";
            for (const std::string& pr : kv.second) os << "              " << pr << "\n";
            os << "            )\n          )\n";
        }
        os << "        )\n      )\n    )\n";
    }
    // One EDIF cell per module instance (unique: hierarchy is not shared),
    // children before parents.  A net lives in the deepest module holding all
    // its endpoints; every module between an endpoint and there gets a port.
    void writeEdifHier(std::ofstream& os, std::map<std::string, std::vector<LibPort>>& lib,
                       const std::vector<std::string>& instNames) {
        struct Ep { std::string path; std::string ref; bool driver; };
        std::map<std::string, std::vector<Ep>> nets;  // net id -> endpoints
        auto instRefOf = [&](size_t i) {
            const std::string& n = instNames[i];
            if (n[0] != '(') return n;
            return n.substr(8, n.find(' ', 8) - 8);
        };
        auto member = [&](const std::string& port, size_t width, size_t bit) {
            if (width == 1) return edifName(port);
            return "(member " + edifName(port) + " " + std::to_string(width - 1 - bit) + ")";
        };
        std::set<std::string> paths = {""};
        for (size_t i = 0; i < m_recs.size(); ++i) {
            const CellRec& r = m_recs[i];
            for (std::string p = r.path; !p.empty(); p = pathOf(p)) paths.insert(p);
            for (const Conn& c : r.conns) {
                size_t width = 1;
                for (const LibPort& lp : lib[r.type]) if (lp.name == c.port) width = lp.width;
                for (size_t b = 0; b < c.bits.size(); ++b) {
                    const std::string& bit = c.bits[b];
                    if (bit.empty() || (bit[0] == '"' && bit != "\"0\"" && bit != "\"1\"")) continue;
                    // constants are local: each module gets its own GND/VCC
                    const std::string id = bit == "\"0\"" ? "GND@" + r.path : bit == "\"1\"" ? "VCC@" + r.path : bit;
                    nets[id].push_back({r.path, "(portRef " + member(c.port, width, b) + " (instanceRef @" + std::to_string(i) + "@))", c.out});
                }
            }
        }
        for (const PortRec& p : m_portRecs) {
            for (size_t b = 0; b < p.bits.size(); ++b) {
                const std::string& bit = p.bits[b];
                const std::string id = bit == "\"0\"" ? "GND@" : bit == "\"1\"" ? "VCC@" : bit;
                if (bit[0] == '"' && bit != "\"0\"" && bit != "\"1\"") continue;
                nets[id].push_back({"", "(portRef " + member(p.name, p.bits.size(), b) + ")", p.dir == "input"});
            }
        }
        // per module: its ports (net id -> output?) and its local nets
        std::map<std::string, std::map<std::string, bool>> ports;
        std::map<std::string, std::map<std::string, std::vector<std::string>>> local;  // path -> net id -> refs
        std::map<std::string, std::set<std::string>> consts;  // path -> {GND, VCC}
        for (auto& kv : nets) {
            const std::string& id = kv.first;
            const bool isConst = id.rfind("GND@", 0) == 0 || id.rfind("VCC@", 0) == 0;
            std::string lca = kv.second.front().path;
            for (const Ep& e : kv.second) lca = commonPath(lca, e.path);
            std::string drvPath;
            bool haveDrv = false;
            for (const Ep& e : kv.second) if (e.driver) { drvPath = e.path; haveDrv = true; }
            if (isConst) {
                const std::string where = id.substr(4);
                consts[where].insert(id.substr(0, 3));
                for (const Ep& e : kv.second) local[e.path][id].push_back(e.ref);
                continue;
            }
            for (const Ep& e : kv.second) {
                local[e.path][id].push_back(e.ref);
                // ports from the endpoint up to (not including) the LCA
                for (std::string p = e.path; p != lca && !p.empty(); p = pathOf(p)) {
                    const bool driverInside = haveDrv && (drvPath == p || drvPath.compare(0, p.size() + 1, p + ".") == 0);
                    ports[p][id] = driverInside;
                }
            }
        }
        auto cellName = [&](const std::string& p) { return p.empty() ? edifName(m_top) : edifName("h_" + sanitize(p)); };
        auto lastName = [&](const std::string& p) { const size_t d = p.rfind('.'); return sanitize(d == std::string::npos ? p : p.substr(d + 1)); };
        std::vector<std::string> order(paths.begin(), paths.end());
        std::sort(order.begin(), order.end(), [](const std::string& a, const std::string& b) {
            const auto da = std::count(a.begin(), a.end(), '.') + (a.empty() ? -1 : 0);
            const auto db = std::count(b.begin(), b.end(), '.') + (b.empty() ? -1 : 0);
            return da != db ? da > db : a < b;
        });
        std::map<std::string, std::vector<std::string>> children;
        for (const std::string& p : paths) if (!p.empty()) children[pathOf(p)].push_back(p);
        std::map<std::string, std::vector<size_t>> prims;
        for (size_t i = 0; i < m_recs.size(); ++i) prims[m_recs[i].path].push_back(i);
        // instance names inside each module: the Vivado-style leaf when there
        // is one and it is free, else the flat name
        std::vector<std::string> hInst(m_recs.size());
        for (const auto& kv : prims) {
            std::set<std::string> used;
            for (const std::string& ch : children[kv.first]) used.insert(lastName(ch));
            for (const size_t i : kv.second) {
                std::string n = m_recs[i].leaf.empty() ? m_recs[i].name : m_recs[i].leaf;
                if (used.count(n)) n = m_recs[i].name;
                while (used.count(n)) n += "_";
                used.insert(n);
                hInst[i] = edifName(n);
            }
        }
        for (const std::string& p : order) {
            os << "    (cell " << cellName(p) << "\n      (cellType GENERIC)\n      (view VIEW_NETLIST\n        (viewType NETLIST)\n        (interface\n";
            if (p.empty()) {
                for (const PortRec& pr : m_portRecs) {
                    const std::string dir = pr.dir == "input" ? "INPUT" : pr.dir == "output" ? "OUTPUT" : "INOUT";
                    if (pr.bits.size() == 1) os << "          (port " << edifName(pr.name) << " (direction " << dir << "))\n";
                    else os << "          (port (array (rename " << pr.name << " \"" << pr.name << "[" << (pr.bits.size() - 1) << ":0]\") " << pr.bits.size() << ") (direction " << dir << "))\n";
                }
            } else {
                for (const auto& pp : ports[p]) os << "          (port p" << pp.first << " (direction " << (pp.second ? "OUTPUT" : "INPUT") << "))\n";
            }
            os << "        )\n        (contents\n";
            for (const std::string& k : consts[p]) os << "          (instance " << k << " (viewRef VIEW_NETLIST (cellRef " << k << " (libraryRef LIB))))\n";
            for (const std::string& ch : children[p]) {
                os << "          (instance " << edifName(lastName(ch)) << " (viewRef VIEW_NETLIST (cellRef " << cellName(ch) << " (libraryRef DESIGN))))\n";
            }
            for (const size_t i : prims[p]) {
                const CellRec& r = m_recs[i];
                os << "          (instance " << hInst[i] << "\n            (viewRef VIEW_NETLIST (cellRef " << edifName(r.type) << " (libraryRef LIB)))";
                for (const auto& pp : r.params) os << "\n            (property " << edifName(pp.first) << " " << edifValue(pp.second) << ")";
                os << ")\n";
            }
            // nets in this module: local endpoints, child ports carrying the net, our own port
            std::map<std::string, std::vector<std::string>> here = local[p];
            for (const std::string& ch : children[p]) {
                const std::string chInst = edifName(lastName(ch));
                const std::string chRef = chInst[0] == '(' ? chInst.substr(8, chInst.find(' ', 8) - 8) : chInst;
                for (const auto& pp : ports[ch]) here[pp.first].push_back("(portRef p" + pp.first + " (instanceRef " + chRef + "))");
            }
            if (!p.empty()) for (const auto& pp : ports[p]) here[pp.first].push_back("(portRef p" + pp.first + ")");
            std::set<std::string> usedNets;
            for (const auto& kv : here) {
                const std::string& id = kv.first;
                std::string name = "n" + id;
                {
                    const auto nit = m_netNames.find({p, id});
                    if (nit != m_netNames.end() && !usedNets.count(nit->second)) {
                        name = edifName(nit->second);
                        usedNets.insert(nit->second);
                    }
                }
                std::string src;
                if (id.rfind("GND@", 0) == 0) { name = "GND_NET"; src = "(portRef G (instanceRef GND))"; }
                if (id.rfind("VCC@", 0) == 0) { name = "VCC_NET"; src = "(portRef P (instanceRef VCC))"; }
                os << "          (net " << name << " (joined\n";
                if (!src.empty()) os << "              " << src << "\n";
                for (std::string r : kv.second) {
                    // primitive refs carry @index@ until the per-module instance name is known
                    const size_t a = r.find("(instanceRef @");
                    if (a != std::string::npos) {
                        const size_t e = r.find('@', a + 14);
                        const size_t idx = std::stoul(r.substr(a + 14, e - a - 14));
                        const std::string& n = hInst[idx];
                        const std::string ref = n[0] == '(' ? n.substr(8, n.find(' ', 8) - 8) : n;
                        r = r.substr(0, a) + "(instanceRef " + ref + r.substr(e + 1);
                    }
                    os << "              " << r << "\n";
                }
                os << "            )\n          )\n";
            }
            os << "        )\n      )\n    )\n";
        }
    }

    // $readmemh text: hex words, @addr markers, // comments (also under $MEM_INIT_DIR)
    static bool readHex(const std::string& path, std::vector<uint64_t>& words) {
        std::ifstream in{path};
        if (!in) {
            if (const char* d = std::getenv("MEM_INIT_DIR")) in.open(std::string(d) + "/" + path);
        }
        if (!in) return false;
        std::string line, tok;
        size_t addr = 0;
        while (std::getline(in, line)) {
            const size_t cp = line.find("//");
            if (cp != std::string::npos) line.resize(cp);
            std::istringstream ls{line};
            while (ls >> tok) {
                if (tok[0] == '@') { addr = std::stoul(tok.substr(1), nullptr, 16); continue; }
                uint64_t v = 0;
                for (const char ch : tok) {
                    if (ch == '_') continue;
                    const int x = std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : std::tolower(ch) - 'a' + 10;
                    v = (v << 4) | static_cast<uint64_t>(x & 15);
                }
                if (words.size() <= addr) words.resize(addr + 1, 0);
                words[addr++] = v;
            }
        }
        return true;
    }
    static int paramOf(const Design::Cell& c, const char* name, int dflt) {
        for (const auto& p : c.params) if (p.first == name) return std::atoi(p.second.c_str());
        return dflt;
    }
    static std::string jsonStr(const std::string& v) {
        std::string o = "\"";
        for (const char ch : v) {
            if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
            else if (static_cast<unsigned char>(ch) < 0x20) o += ' ';
            else o += ch;
        }
        return o + "\"";
    }
    // The file $readmemh named, as readHex finds it: as given, else under $MEM_INIT_DIR; made absolute so that a tool
    // run from another directory finds the same file
    static std::string resolveInitFile(const std::string& f) {
        if (f.empty()) return f;
        std::vector<std::string> cands{f};
        if (const char* d = std::getenv("MEM_INIT_DIR")) cands.push_back(std::string(d) + "/" + f);
        for (const std::string& cand : cands) {
            if (!std::ifstream{cand}) continue;
            return V3Os::filenameRealPath(cand);   // realpath, or _fullpath on Windows
        }
        return f;
    }
    // One memory of the manifest.  The cells of the memory are named <cell>_t<lane tile>[_d<depth block>]: lane tile t holds
    // data bits [t*tw, t*tw+tw) of every word (the last one fewer: width), depth block d the words [d*words_per_tile,
    // (d+1)*words_per_tile).  Word k of a block is in bits [k*tw, k*tw+tw) of the block RAM's INIT_xx lines (the data layout
    // of UG473 for a port of tw data bits), which is what a tool needs to place a new image into a placed design.
    void recordMemInit(const Design::Cell& c, const std::string& initFile, const char* prim, int dw, int depth, int tw,
                       int ntiles, int nblocks, int wordsPerTile, bool cascade) {
        std::ostringstream o;
        const std::string cellBase = sanitize(c.name);
        o << "    {\"name\": " << jsonStr(c.name) << ", \"type\": " << jsonStr(c.type) << ", \"kind\": \"bram\""
          << ", \"primitive\": " << jsonStr(prim) << ", \"init_file\": " << jsonStr(initFile)
          << ", \"init_path\": " << jsonStr(resolveInitFile(initFile)) << ", \"width\": " << dw << ", \"depth\": " << depth
          << ", \"lane_bits\": " << tw << ", \"lanes\": " << ntiles << ", \"blocks\": " << nblocks
          << ", \"words_per_block\": " << wordsPerTile << ", \"cascade\": " << (cascade ? "true" : "false") << ",\n"
          << "     \"cells\": [";
        bool first = true;
        for (int t = 0; t < ntiles; ++t) {
            for (int d = 0; d < nblocks; ++d) {
                const int lo = t * tw;
                o << (first ? "\n" : ",\n") << "       {\"cell\": "
                  << jsonStr(cellBase + "_t" + std::to_string(t) + (nblocks > 1 ? "_d" + std::to_string(d) : ""))
                  << ", \"lane\": " << t << ", \"block\": " << d << ", \"bit_lo\": " << lo
                  << ", \"bits\": " << std::min(tw, dw - lo) << ", \"word_lo\": " << d * wordsPerTile << "}";
                first = false;
            }
        }
        o << "\n     ]}";
        m_memInit.push_back(o.str());
    }
    void writeMemInit(const std::string& filename) {
        std::ofstream os{filename};
        os << "{\n  \"creator\": \"verilator --map-only-meminit\",\n  \"top\": " << jsonStr(m_top) << ",\n  \"memories\": [\n";
        for (size_t i = 0; i < m_memInit.size(); ++i) os << m_memInit[i] << (i + 1 < m_memInit.size() ? ",\n" : "\n");
        os << "  ]\n}\n";
    }
    void emitMemory(const Design::Cell& c, const std::vector<int>& inPo) {
        const int aw = paramOf(c, "ADDR_WIDTH", 1);
        const int dw = paramOf(c, "DATA_WIDTH", 1);
        const int depth = paramOf(c, "DEPTH", 1 << aw);  // words used
        auto pinNets = [&](const char* name) -> std::vector<std::string> {
            for (size_t i = 0; i < c.ins.size(); ++i) {
                if (c.ins[i].first == name) {
                    std::vector<std::string> nets;
                    for (size_t b = 0; b < c.ins[i].second.size(); ++b) nets.push_back(m_poNet[inPo[i] + b]);
                    return nets;
                }
            }
            return {};
        };
        auto outNets = [&](const char* name) -> std::vector<std::string> {
            for (const auto& o : c.outs) if (o.first == name) {
                std::vector<std::string> nets;
                for (const int l : o.second) nets.push_back(netOfLit(l));
                return nets;
            }
            return {};
        };
        auto bit0 = [](const std::vector<std::string>& v, const char* dflt) { return v.empty() ? std::string(dflt) : v[0]; };
        const bool sync = c.type.find("_sync") != std::string::npos;
        const bool tdp = c.type == "memory_tdp_sync";
        if (c.type == "memory_2w2r_rf" || c.type == "memory_2w2r") {
            m_warnings.push_back("memory " + c.name + ": two write ports with independent read addresses are not a block RAM; dropped");
            return;
        }
        if (!sync) {
            emitLutram(c, inPo);  // asynchronous reads: distributed RAM
            return;
        }
        // The two tile ports.  A: write (+ read 1 with NO_CHANGE when two reads);
        // B: read (2, or 1 for a 1w1r); a TDP memory maps port for port.
        struct Port {
            std::vector<std::string> addr, din, dout;
            std::string clk, en, we;
            const char* mode = "READ_FIRST";
        };
        Port P[2];
        if (tdp) {
            for (int k = 0; k < 2; ++k) {
                const std::string sfx = std::to_string(k + 1);
                P[k].clk = bit0(pinNets(("clk" + sfx).c_str()), "\"0\"");
                P[k].en = bit0(pinNets(("en" + sfx).c_str()), "\"1\"");
                P[k].we = bit0(pinNets(("we" + sfx).c_str()), "\"0\"");
                P[k].addr = pinNets(("addr" + sfx).c_str());
                P[k].din = pinNets(("wdata" + sfx).c_str());
                P[k].dout = outNets(("rdata" + sfx).c_str());
                P[k].mode = "NO_CHANGE";
            }
        } else {
            const std::string clk = bit0(pinNets("clk"), "\"0\"");
            std::vector<std::string> we = pinNets("we"), waddr = pinNets("waddr"), wdata = pinNets("wdata");
            if (c.type == "memory_sp") waddr = pinNets("addr");
            const bool is1w1r = c.type == "memory_1w1r_sync";
            std::vector<std::string> re1 = pinNets(is1w1r ? "re" : "re1");
            std::vector<std::string> ra1 = pinNets(is1w1r ? "raddr" : (c.type == "memory_sp" ? "addr" : "raddr1"));
            std::vector<std::string> re2 = pinNets("re2"), ra2 = pinNets("raddr2");
            std::vector<std::string> rd1 = outNets(is1w1r || c.type == "memory_sp" ? "rdata" : "rdata1");
            std::vector<std::string> rd2 = outNets("rdata2");
            const bool twoReads = !rd2.empty();
            const std::string weBit = bit0(we, "\"0\"");
            const std::string re1Bit = bit0(re1, "\"1\"");
            const std::string re2Bit = bit0(re2, "\"1\"");
            P[0].clk = P[1].clk = clk;
            P[0].we = weBit;
            P[0].din = wdata;
            P[1].we = "\"0\"";
            if (twoReads && (ra1 == waddr || ra2 == waddr)) {
                // A read at the write address (LiteEth's buffers, the second
                // port a registered-address read became) shares port A with
                // the write: READ_FIRST gives the word before the write, as the
                // synchronous read does, and the other read has port B to itself
                const bool firstShares = ra1 == waddr;
                P[0].en = orNet(weBit, firstShares ? re1Bit : re2Bit);
                P[0].addr = waddr;
                P[0].dout = firstShares ? rd1 : rd2;
                P[0].mode = "READ_FIRST";
                P[1].en = firstShares ? re2Bit : re1Bit;
                P[1].addr = firstShares ? ra2 : ra1;
                P[1].dout = firstShares ? rd2 : rd1;
            } else if (twoReads) {
                // two reads at their own addresses: port A takes the write and,
                // in a cycle without one, the first read (NO_CHANGE keeps it)
                P[0].en = orNet(weBit, re1Bit);
                P[0].addr = muxAddr(weBit, waddr, ra1);
                P[0].dout = rd1;
                P[0].mode = "NO_CHANGE";
                P[1].en = re2Bit;
                P[1].addr = ra2;
                P[1].dout = rd2;
            } else {
                P[0].en = weBit;
                P[0].addr = waddr;
                P[1].en = re1Bit;
                P[1].addr = ra1;
                P[1].dout = rd1;
            }
        }
        // Tile plan: RAMB36E1 only, in the lane widths the open flow has been
        // seen to get right on hardware: x36 (32 data bits), x9 (8) and x1.
        // x2/x4 come out of nextpnr miscompiled (bytecode/fpga/vc707-ethmin
        // notes) and x18 / RAMB18E1 are unproven, so they are not used.
        // Deeper memories use x1 lanes in 32K blocks with a registered block
        // select (tdp_ram32's scheme).
        int tw = 1;
        const char* prim = "RAMB36E1";
        for (const int cand : {32, 8, 1}) {
            if (32768 / cand >= depth) { tw = cand; break; }
        }
        int nblocks = 1;
        if (depth > 32768) {
            tw = 1;
            prim = "RAMB36E1";
            nblocks = (depth + 32767) / 32768;
        }
        // --map-only-bram-cascade: the 32K x1 blocks of a deep memory go in
        // pairs, a LOWER and an UPPER RAMB36 joined by the column's cascade
        // wires into one 64K x1 memory (UG473; as Vivado maps a 64K x1 RAM):
        // address bit 15 to both picks the half, only the UPPER's DO is read,
        // and the select logic and output mux work on pairs, halving the mux
        const bool cascade = v3Global.opt.mapOnlyBramCascade() && nblocks > 1;
        if (cascade && (nblocks & 1)) ++nblocks;  // the last pair's UPPER is unused, its contents 0
        const int units = cascade ? nblocks / 2 : nblocks;  // what the output mux chooses between
        const int sel0 = cascade ? 1 : 0;  // the first address bit above a unit
        const bool b36 = std::string(prim) == "RAMB36E1";
        const int addrTotal = b36 ? 16 : 14, diW = b36 ? 32 : 16, dipW = b36 ? 4 : 2, weaW = b36 ? 4 : 2, webweW = b36 ? 8 : 4;
        int shift = 0;
        while ((1 << shift) < tw) ++shift;
        const int mw = tw == 8 ? 9 : tw == 16 ? 18 : tw == 32 ? 36 : tw;
        const int wordAddrW = addrTotal - shift - (b36 ? 1 : 0);  // address bits inside one tile
        int hiW = 0;
        while ((1 << hiW) < nblocks) ++hiW;
        auto addrVec = [&](const std::vector<std::string>& a) {
            std::vector<std::string> v;
            for (int i = 0; i < shift; ++i) v.push_back("\"0\"");
            for (int i = 0; i < wordAddrW; ++i) v.push_back(i < static_cast<int>(a.size()) ? a[i] : "\"0\"");
            // bit 15: unused (tied high) on a RAMB36 of its own, the half select of a cascaded pair
            if (b36) v.push_back(!cascade ? "\"1\"" : wordAddrW < static_cast<int>(a.size()) ? a[wordAddrW] : "\"0\"");
            return v;
        };
        auto rep = [&](const std::string& bit, int n) { return std::vector<std::string>(n, bit); };
        auto zeros = [&](int n) { return rep("\"0\"", n); };
        // Per port: block enables (en & hi == d) and the registered block select
        struct Sel { std::vector<std::string> blockEn, regHi; };
        Sel S[2];
        for (int k = 0; k < 2; ++k) {
            if (units == 1) { S[k].blockEn = {P[k].en}; continue; }
            std::vector<std::string> hi;
            for (int i = 0; i < hiW; ++i) {
                const size_t idx = wordAddrW + i;
                hi.push_back(idx < P[k].addr.size() ? P[k].addr[idx] : "\"0\"");
            }
            for (int u = 0; u < units; ++u) {
                std::string en = P[k].en;
                for (int i = sel0; i < hiW; ++i) en = andNet(en, (u >> (i - sel0)) & 1 ? hi[i] : notNet(hi[i]));
                S[k].blockEn.push_back(en);
            }
            for (int i = sel0; i < hiW; ++i) {
                const std::string q = newNet();
                cell("FDRE", "bsel_" + std::to_string(m_ncells), {{"INIT", "\"0\""}},
                     {{"C", "[" + P[k].clk + "]"}, {"CE", "[" + P[k].en + "]"}, {"D", "[" + hi[i] + "]"}, {"R", "[\"0\"]"}},
                     {{"Q", "[" + q + "]"}});
                S[k].regHi.push_back(q);
            }
        }
        const int ntiles = (dw + tw - 1) / tw;
        // Initial contents ($readmemh INIT_FILE on the primitive) go into the
        // tiles' INIT_xx strings: lane bit i of word k sits at INIT bit k*tw+i
        // (x1/x9/x36 data layout; parity bits unused, INITP left 0)
        std::vector<uint64_t> image;
        std::string initFile;
        for (const auto& p : c.params) {
            if (p.first != "INIT_FILE") continue;
            std::string f = p.second;
            if (f.size() >= 2 && f.front() == '"') f = f.substr(1, f.size() - 2);
            if (f.empty()) continue;
            initFile = f;
            if (!readHex(f, image)) m_warnings.push_back("memory " + c.name + ": INIT_FILE " + f + " not readable; contents left 0");
        }
        const int wordsPerTile = 32768 / tw;
        if (!v3Global.opt.mapOnlyMemInit().empty()) recordMemInit(c, initFile, prim, dw, depth, tw, ntiles, nblocks, wordsPerTile, cascade);
        auto initParams = [&](int lo, int sw, int d, std::vector<std::pair<std::string, std::string>>& params) {
            if (image.empty()) return;
            for (int line = 0; line < 128; ++line) {
                std::string bits(256, '0');  // MSB first
                bool any = false;
                for (int j = 0; j < 256; ++j) {
                    const int pos = line * 256 + j;
                    const int k = pos / tw;
                    const int i = pos % tw;
                    if (i >= sw) continue;
                    const size_t w = static_cast<size_t>(d) * wordsPerTile + k;
                    if (w >= image.size()) continue;
                    if ((image[w] >> (lo + i)) & 1) { bits[255 - j] = '1'; any = true; }
                }
                if (!any) continue;
                char name[16];
                std::snprintf(name, sizeof name, "INIT_%02X", line);
                params.emplace_back(name, "\"" + bits + "\"");
            }
        };
        // block outputs per port per data bit, muxed afterwards when banked
        std::vector<std::vector<std::vector<std::string>>> blkOut(2, std::vector<std::vector<std::string>>(dw));
        for (int t = 0; t < ntiles; ++t) {
            const int lo = t * tw;
            const int sw = std::min(tw, dw - lo);
            std::string casOut[2];  // a LOWER's cascade outputs, for the UPPER after it
            for (int d = 0; d < nblocks; ++d) {
                const bool lower = cascade && !(d & 1);
                const int u = cascade ? d / 2 : d;
                std::vector<std::string> di[2], dout[2], dop[2];
                for (int k = 0; k < 2; ++k) {
                    di[k] = zeros(diW);
                    for (int b = 0; b < sw; ++b) di[k][b] = lo + b < static_cast<int>(P[k].din.size()) ? P[k].din[lo + b] : "\"0\"";
                    for (int b = 0; b < diW; ++b) {
                        std::string n;
                        if (b < sw && lo + b < static_cast<int>(P[k].dout.size()) && !lower) {
                            n = units == 1 ? P[k].dout[lo + b] : newNet();
                            if (units > 1) blkOut[k][lo + b].push_back(n);
                        } else {
                            n = newNet();
                        }
                        dout[k].push_back(n);
                    }
                    for (int b = 0; b < dipW; ++b) dop[k].push_back(newNet());
                }
                std::vector<std::pair<std::string, std::string>> params = {
                    {"RAM_MODE", "\"TDP\""}, {"WRITE_MODE_A", std::string("\"") + P[0].mode + "\""}, {"WRITE_MODE_B", std::string("\"") + P[1].mode + "\""},
                    {"READ_WIDTH_A", binParam(mw)}, {"WRITE_WIDTH_A", binParam(mw)},
                    {"READ_WIDTH_B", binParam(mw)}, {"WRITE_WIDTH_B", binParam(tdp || cascade ? mw : 0)},  // a cascaded pair is x1 on every port
                    {"DOA_REG", binParam(0)}, {"DOB_REG", binParam(0)}};
                initParams(lo, sw, d, params);
                if (cascade) {
                    params.emplace_back("RAM_EXTENSION_A", lower ? "\"LOWER\"" : "\"UPPER\"");
                    params.emplace_back("RAM_EXTENSION_B", lower ? "\"LOWER\"" : "\"UPPER\"");
                }
                std::vector<std::pair<std::string, std::string>> ins = {
                    {"CLKARDCLK", "[" + P[0].clk + "]"}, {"CLKBWRCLK", "[" + P[1].clk + "]"},
                    {"ENARDEN", "[" + S[0].blockEn[u] + "]"}, {"ENBWREN", "[" + S[1].blockEn[u] + "]"},
                    {"REGCEAREGCE", "[\"0\"]"}, {"REGCEB", "[\"0\"]"},
                    {"RSTRAMARSTRAM", "[\"0\"]"}, {"RSTRAMB", "[\"0\"]"}, {"RSTREGARSTREG", "[\"0\"]"}, {"RSTREGB", "[\"0\"]"},
                    {"WEA", bitsJson(rep(P[0].we, weaW))}, {"WEBWE", bitsJson(rep(P[1].we, webweW))},
                    {"ADDRARDADDR", bitsJson(addrVec(P[0].addr))},
                    {"ADDRBWRADDR", bitsJson(addrVec(P[1].addr))},
                    {"DIADI", bitsJson(di[0])}, {"DIPADIP", bitsJson(zeros(dipW))},
                    {"DIBDI", bitsJson(di[1])}, {"DIPBDIP", bitsJson(zeros(dipW))}};
                std::vector<std::pair<std::string, std::string>> outs = {
                    {"DOADO", bitsJson(dout[0])}, {"DOPADOP", bitsJson(dop[0])}, {"DOBDO", bitsJson(dout[1])}, {"DOPBDOP", bitsJson(dop[1])}};
                if (lower) {
                    casOut[0] = newNet();
                    casOut[1] = newNet();
                    ins.push_back({"CASCADEINA", "[\"1\"]"});
                    ins.push_back({"CASCADEINB", "[\"1\"]"});
                    outs.push_back({"CASCADEOUTA", "[" + casOut[0] + "]"});
                    outs.push_back({"CASCADEOUTB", "[" + casOut[1] + "]"});
                } else if (cascade) {
                    ins.push_back({"CASCADEINA", "[" + casOut[0] + "]"});
                    ins.push_back({"CASCADEINB", "[" + casOut[1] + "]"});
                }
                {
                    int lo = 0;
                    m_curLeaf = leafOf(c.name, lo) + "_t" + std::to_string(t) + (nblocks > 1 ? "_d" + std::to_string(d) : "");
                }
                cell(prim, sanitize(c.name) + "_t" + std::to_string(t) + (nblocks > 1 ? "_d" + std::to_string(d) : ""), params, ins, outs);
            }
        }
        if (units > 1) {
            for (int k = 0; k < 2; ++k) {
                for (int b = 0; b < dw && b < static_cast<int>(P[k].dout.size()); ++b) {
                    muxTree(blkOut[k][b], S[k].regHi, P[k].dout[b]);
                }
            }
        }
    }
    // Asynchronous-read memory (memory_sp / memory_1w1r / memory_1w2r):
    // RAM64X1D per data bit per 64 words, written through WE decoded from the
    // upper write-address bits, read through DPRA with a LUT mux over the
    // blocks on the upper read-address bits; a second read port is a second
    // copy of the array (both written).
    void emitLutram(const Design::Cell& c, const std::vector<int>& inPo) {
        const int aw = paramOf(c, "ADDR_WIDTH", 1);
        const int dw = paramOf(c, "DATA_WIDTH", 1);
        const int depth = paramOf(c, "DEPTH", 1 << aw);  // words used
        auto pinNets = [&](const char* name) -> std::vector<std::string> {
            for (size_t i = 0; i < c.ins.size(); ++i) {
                if (c.ins[i].first == name) {
                    std::vector<std::string> nets;
                    for (size_t b = 0; b < c.ins[i].second.size(); ++b) nets.push_back(m_poNet[inPo[i] + b]);
                    return nets;
                }
            }
            return {};
        };
        auto outNets = [&](const char* name) -> std::vector<std::string> {
            for (const auto& o : c.outs) if (o.first == name) {
                std::vector<std::string> nets;
                for (const int l : o.second) nets.push_back(netOfLit(l));
                return nets;
            }
            return {};
        };
        const bool sp = c.type == "memory_sp";
        const std::string clk = pinNets("clk").empty() ? "\"0\"" : pinNets("clk")[0];
        const std::vector<std::string> we = pinNets("we");
        const std::string weBit = we.empty() ? "\"0\"" : we[0];
        const std::vector<std::string> waddr = pinNets(sp ? "addr" : "waddr");
        const std::vector<std::string> wdata = pinNets("wdata");
        std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> reads;  // (raddr, rdata)
        if (sp) {
            reads.emplace_back(waddr, outNets("rdata"));
        } else {
            if (!outNets("rdata1").empty()) reads.emplace_back(pinNets("raddr1"), outNets("rdata1"));
            if (!outNets("rdata2").empty()) reads.emplace_back(pinNets("raddr2"), outNets("rdata2"));
        }
        const int nblk = (depth + 63) / 64;
        int hiW = 0;
        while ((1 << hiW) < nblk) ++hiW;
        auto bitOr0 = [](const std::vector<std::string>& v, size_t i) { return i < v.size() ? v[i] : std::string("\"0\""); };
        std::vector<std::string> blockWe;
        for (int d = 0; d < nblk; ++d) {
            std::string en = weBit;
            for (int i = 0; i < hiW; ++i) en = andNet(en, (d >> i) & 1 ? bitOr0(waddr, 6 + i) : notNet(bitOr0(waddr, 6 + i)));
            blockWe.push_back(en);
        }
        for (size_t r = 0; r < reads.size(); ++r) {
            const std::vector<std::string>& raddr = reads[r].first;
            const std::vector<std::string>& rdata = reads[r].second;
            std::vector<std::string> rhi;
            for (int i = 0; i < hiW; ++i) rhi.push_back(bitOr0(raddr, 6 + i));
            for (int b = 0; b < dw && b < static_cast<int>(rdata.size()); ++b) {
                std::vector<std::string> blk;
                for (int d = 0; d < nblk; ++d) {
                    const std::string dpo = nblk == 1 ? rdata[b] : newNet();
                    blk.push_back(dpo);
                    std::vector<std::pair<std::string, std::string>> ins = {{"WCLK", "[" + clk + "]"}, {"WE", "[" + blockWe[d] + "]"}, {"D", "[" + bitOr0(wdata, b) + "]"}};
                    for (int i = 0; i < 6; ++i) ins.emplace_back("A" + std::to_string(i), "[" + bitOr0(waddr, i) + "]");
                    for (int i = 0; i < 6; ++i) ins.emplace_back("DPRA" + std::to_string(i), "[" + bitOr0(raddr, i) + "]");
                    cell("RAM64X1D", sanitize(c.name) + "_r" + std::to_string(r) + "_b" + std::to_string(b) + "_d" + std::to_string(d),
                         {{"INIT", "\"" + std::string(64, '0') + "\""}}, ins, {{"SPO", "[" + newNet() + "]"}, {"DPO", "[" + dpo + "]"}});
                }
                if (nblk > 1) muxTree(blk, rhi, rdata[b]);
            }
        }
    }
    // out = ins[sel] through a tree of 2:1 LUT3 muxes; the root drives `out`
    void muxTree(const std::vector<std::string>& ins, const std::vector<std::string>& sel, const std::string& out) {
        std::vector<std::string> level = ins;
        for (size_t s = 0; s < sel.size(); ++s) {
            std::vector<std::string> next;
            for (size_t i = 0; i < level.size(); i += 2) {
                const std::string a = level[i];
                const std::string b = i + 1 < level.size() ? level[i + 1] : "\"0\"";
                const bool last = s + 1 == sel.size() && level.size() <= 2;
                const std::string o = last ? out : newNet();
                cell("LUT3", "bmux_" + std::to_string(m_ncells), {{"INIT", "\"11001010\""}},
                     {{"I0", "[" + a + "]"}, {"I1", "[" + b + "]"}, {"I2", "[" + sel[s] + "]"}}, {{"O", "[" + o + "]"}});
                next.push_back(o);
            }
            level = next;
        }
    }
    std::string andNet(const std::string& a, const std::string& b) {
        if (a == "\"0\"" || b == "\"0\"") return "\"0\"";
        if (a == "\"1\"") return b;
        if (b == "\"1\"") return a;
        const std::string o = newNet();
        cell("LUT2", "and_" + std::to_string(m_ncells), {{"INIT", "\"1000\""}}, {{"I0", "[" + a + "]"}, {"I1", "[" + b + "]"}}, {{"O", "[" + o + "]"}});
        return o;
    }
    std::string notNet(const std::string& a) {
        if (a == "\"0\"") return "\"1\"";
        if (a == "\"1\"") return "\"0\"";
        const std::string o = newNet();
        cell("LUT1", "not_" + std::to_string(m_ncells), {{"INIT", "\"01\""}}, {{"I0", "[" + a + "]"}}, {{"O", "[" + o + "]"}});
        return o;
    }
    // a | b of two 1-bit nets (LUT2)
    std::string orNet(const std::string& a, const std::string& b) {
        if (a == "\"0\"") return b;
        if (b == "\"0\"") return a;
        if (a == "\"1\"" || b == "\"1\"") return "\"1\"";
        const std::string o = newNet();
        cell("LUT2", "or_" + std::to_string(m_ncells), {{"INIT", "\"1110\""}}, {{"I0", "[" + a + "]"}, {"I1", "[" + b + "]"}}, {{"O", "[" + o + "]"}});
        return o;
    }
    // we ? waddr : raddr per bit (LUT3 mux)
    std::vector<std::string> muxAddr(const std::string& we, const std::vector<std::string>& wa, const std::vector<std::string>& ra) {
        std::vector<std::string> out;
        const size_t n = std::max(wa.size(), ra.size());
        for (size_t i = 0; i < n; ++i) {
            const std::string a = i < wa.size() ? wa[i] : "\"0\"";
            const std::string b = i < ra.size() ? ra[i] : "\"0\"";
            if (a == b) { out.push_back(a); continue; }
            const std::string o = newNet();
            // I2=we, I1=a (waddr), I0=b (raddr): out = we ? a : b   INIT index = I2 I1 I0
            cell("LUT3", "amux_" + std::to_string(m_ncells), {{"INIT", "\"11001010\""}}, {{"I0", "[" + b + "]"}, {"I1", "[" + a + "]"}, {"I2", "[" + we + "]"}}, {{"O", "[" + o + "]"}});
            out.push_back(o);
        }
        return out;
    }
};

}  // namespace

void V3EmitMap::emit(const std::string& filename) {
    UINFO(2, __FUNCTION__ << ":");
    Mapper m;
    m.run(filename);
}
