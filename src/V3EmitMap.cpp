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
//     -> libabc (linked in): strash / dc2 / dch / if / mfs2 over a LUT
//        library of 1..8 inputs (7 and 8 costing two and four LUT6s)
//     -> BLIF back in, LUT INITs rebuilt from the covers; a 7- or 8-input
//        cover is two or four LUT6s under a MUXF7 / MUXF8 tree (Shannon
//        expansion on its top inputs), as in a slice
//     -> yosys-JSON for nextpnr-himbaechel: LUT1..6, MUXF7/MUXF8, FDRE/FDSE/FDCE/FDPE,
//        LDCE/LDPE, CARRY4 chains, DSP48E1 multipliers, RAMB36E1 tiles
//        (System-Verilog-suite's validated geometry), RAM64X1D distributed
//        RAM, IBUF/OBUF/IBUFDS/BUFG pass-through cells.
//
//   Hard-cell choices made here, after the netlist is elaborated:
//     - a multiply whose operands fit 25 x 18 bits below their sign
//       extension is one DSP48E1 (mulHard), otherwise 24 x 17 partial
//       products summed on carry chains;
//     - flip-flops on a DSP's product become its P register, a second stage
//       its M register, those on its operands its A / B registers (one or
//       two stages), and an accumulator fed by its own sum with the product
//       register folds into the post-adder (absorbDsp*);
//     - the byte lanes V3EmitStruct split a memory into go back into one
//       RAMB36E1 with a write enable per byte (mergeLanes);
//     - a synchronous-read memory of up to 64 words / 512 bits is RAM64X1D
//       with its read registers outside, not a block RAM (emitLutram);
//     - a register read only in complement is stored inverted
//       (flipRegisterPolarity, after abc), removing the inverter LUT1s.
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3EmitMap.h"

#include "V3EmitStruct.h"
#include "V3Global.h"
#include "V3MapAbc.h"
#include "V3MapTiming.h"
#include "V3Os.h"

#include "struct_netlist.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <tuple>
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
            svs_netlist::warn("net driven twice (lit " + std::to_string(a) + " vs "
                              + std::to_string(b) + ")");
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
        for (int i = 0; i < w; ++i)
            r[i] = i < static_cast<int>(a.size()) ? a[i] : (sext && !a.empty() ? a.back() : 0);
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
    // are skipped) summed with such adders, unless one operand is a constant
    // (mulConst).
    static constexpr size_t MIN_CARRY = 4;
    static constexpr size_t MIN_DSP = 8;
    struct Chain final {
        Vec x,
            y;  // the operands it adds (with cyinit): balanceSums, Mapper::absorbDspAccumulators
        Vec s, di;  // per bit
        int cyinit = 0;
        Vec o;  // sum bits (placeholders), empty for a compare
        int co = -1;  // final carry (placeholder), -1 when unused
    };
    struct Dsp final {
        Vec a, b;  // 25 / 18 operand bits, two's complement
        Vec p;  // 48 product bits (placeholders)
        // P register absorbed from the flip-flops the product fed
        // (Mapper::absorbDspRegs): PREG = 1, clocked by clk, CEP / RSTP
        bool preg = false;
        int clk = 0, cep = 1, rstp = 0;
        int pw = 0;  // product register bits (the rest of P is unread)
        // Accumulator (Mapper::absorbDspAccumulators): P <= P + M, OPMODE Z = P
        bool acc = false;
        // M register: a second flip-flop stage behind the product (the first
        // stage, PREG until then, moves to MREG = 1 with its CEM / RSTM)
        bool mreg = false;
        int cem = 1, rstm = 0;
        // A / B input registers absorbed from the flip-flops the operands
        // came from (Mapper::absorbDspInputRegs): AREG / BREG stages (0..2),
        // the stage next to the multiplier A2 / B2, the one before it A1 / B1
        int areg = 0, breg = 0;
        int cea1 = 1, cea2 = 1, rsta = 0, ceb1 = 1, ceb2 = 1, rstb = 0;
    };
    std::vector<Chain> m_chains;
    std::vector<Dsp> m_dsps;
    // Chains and DSPs are hashed on their operand literals, so one hard cell
    // serves every identical add, subtract and ordered compare in the
    // flattened design (a < b, a >= b and a - b are one chain)
    std::map<std::tuple<Vec, Vec, int>, size_t> m_chainHash;
    std::map<std::pair<Vec, Vec>, size_t> m_dspHash;
    // The chain computing x + y + cin.  Where a bit's propagate S = x ^ y is
    // 0, x == y, so either is its generate DI: taking the smaller literal
    // makes x + y and y + x the same chain.
    size_t chainFor(const Vec& x, const Vec& y, int cin) {
        Chain c;
        c.x = x;
        c.y = y;
        for (size_t i = 0; i < x.size(); ++i) {
            c.s.push_back(XOR(x[i], y[i]));
            c.di.push_back(std::min(x[i], y[i]));
        }
        c.cyinit = cin;
        const auto key = std::make_tuple(c.s, c.di, cin);
        const auto it = m_chainHash.find(key);
        if (it != m_chainHash.end()) return it->second;
        m_chains.push_back(c);
        m_chainHash.emplace(key, m_chains.size() - 1);
        return m_chains.size() - 1;
    }
    bool m_hard = true;
    std::unordered_set<int> m_pinned;  // hard-cell outputs: never aliased by equate
    int pinnedVar() {
        const int l = newVar();
        m_pinned.insert(l >> 1);
        return l;
    }
    // A partial product whose A or B slice is constant 0 once the netlist is
    // elaborated is itself 0.  mul() skips such slices only when they are
    // literal 0 as it builds them, but an operand bit is often still an
    // unbound placeholder then (a zero-extended operand, an operand read
    // through a net), bound to 0 by equate() later.  After elaboration every
    // alias is known: drop those DSP48E1s and tie their product bits to 0.
    // Value of a literal once every placeholder alias is known: 0, 1, or -1
    // when it depends on a primary input.  Gates were built while their
    // inputs were still placeholders, so a mux of two zero-extended operands
    // only folds to 0 here.  Iterative: AIG depth can exceed the stack.
    int constOf(int lit, std::unordered_map<int, int>& memo) {
        std::vector<int> stack{resolve(lit) >> 1};
        while (!stack.empty()) {
            const int v = stack.back();
            if (v == 0 || memo.count(v)) {
                stack.pop_back();
                continue;
            }
            if (isPlaceholder(v)) {  // unbound: a primary input
                memo[v] = -1;
                stack.pop_back();
                continue;
            }
            const int f0 = resolve(m_fan[v].first);
            const int f1 = resolve(m_fan[v].second);
            const auto val = [&](int l, bool& ready) {
                const int fv = l >> 1;
                if (fv == 0) return l & 1;
                const auto it = memo.find(fv);
                if (it == memo.end()) {
                    ready = false;
                    stack.push_back(fv);
                    return -1;
                }
                return it->second < 0 ? -1 : it->second ^ (l & 1);
            };
            bool ready = true;
            const int a = val(f0, ready);
            if (a == 0 && ready) {  // AND short-circuits on a known 0
                memo[v] = 0;
                stack.pop_back();
                continue;
            }
            const int b = val(f1, ready);
            if (!ready) continue;
            memo[v] = (a == 0 || b == 0) ? 0 : (a == 1 && b == 1) ? 1 : -1;
            stack.pop_back();
        }
        const int r = resolve(lit);
        if ((r >> 1) == 0) return r & 1;
        const int mv = memo[r >> 1];
        return mv < 0 ? -1 : mv ^ (r & 1);
    }
    int pruneDsps() {
        std::unordered_map<int, int> memo;
        const auto zero = [&](const Vec& v) {
            for (const int l : v)
                if (constOf(l, memo) != 0) return false;
            return true;
        };
        size_t k = 0;
        int dropped = 0;
        for (size_t i = 0; i < m_dsps.size(); ++i) {
            if (zero(m_dsps[i].a) || zero(m_dsps[i].b)) {
                for (const int l : m_dsps[i].p) {
                    const int v = l >> 1;  // pinnedVar(): an unaliased placeholder
                    m_pinned.erase(v);
                    m_alias[v] = l & 1;  // constant 0 (complemented if l was)
                }
                ++dropped;
                continue;
            }
            if (k != i) m_dsps[k] = std::move(m_dsps[i]);
            ++k;
        }
        m_dsps.resize(k);
        return dropped;
    }
    // lit with its cone re-built through AND(), which folds constants and
    // complements: gates built while an input was still a placeholder later
    // bound to a constant (a zero-extended operand) simplify only now.
    // Depth-limited: only the few gates next to such an input matter.
    int fold(int lit, std::unordered_map<int, int>& memo, int depth = 8) {
        const int r = resolve(lit);
        const int v = r >> 1;
        if (!v || isPlaceholder(v) || depth == 0) return r;
        const auto it = memo.find(v);
        if (it != memo.end()) return it->second ^ (r & 1);
        const int a = fold(m_fan[v].first, memo, depth - 1);
        const int b = fold(m_fan[v].second, memo, depth - 1);
        const int f = AND(a, b);
        memo[v] = f;
        return f ^ (r & 1);
    }
    // A carry chain that never carries -- carry-in 0, and no bit generates
    // (DI is 0 wherever S is 0; CO = S ? CI : DI) -- sums to its propagate
    // bits, O = S, with CO = 0: an add of a constant 0, such as one of the
    // partial products pruneDsps() tied off.  Drop it and read S.  Repeated,
    // since an add of a bypassed sum can then become idle too.
    int bypassIdleChains() {
        int bypassed = 0;
        for (int pass = 0; pass < 16; ++pass) {
            std::unordered_map<int, int> memo;
            std::vector<Chain> kept;
            int now = 0;
            for (Chain& c : m_chains) {
                bool idle = fold(c.cyinit, memo) == 0;
                std::vector<int> sf(c.s.size());
                for (size_t i = 0; i < c.s.size() && idle; ++i) {
                    sf[i] = fold(c.s[i], memo);
                    idle = AND(sf[i] ^ 1, fold(c.di[i], memo)) == 0;
                }
                if (!idle) {
                    kept.push_back(std::move(c));
                    continue;
                }
                for (size_t i = 0; i < c.o.size(); ++i) {
                    const int v = c.o[i] >> 1;
                    m_pinned.erase(v);
                    m_alias[v] = sf[i] ^ (c.o[i] & 1);
                }
                if (c.co >= 0) {
                    m_pinned.erase(c.co >> 1);
                    m_alias[c.co >> 1] = c.co & 1;
                }
                ++now;
            }
            m_chains = std::move(kept);
            bypassed += now;
            if (!now) break;
        }
        m_chainHash.clear();  // indices are stale; no chain is built after elaboration
        return bypassed;
    }
    // A sum of many terms written as a series of adds -- c = c + x[i] in a
    // loop, ((a + b) + c) + d -- elaborates to carry chains in series, each
    // waiting for the one before: n terms, n - 1 chains deep.  Addition
    // modulo 2^W is associative and commutative, so a chain whose sum is used
    // only as a whole operand of one other add, at least as wide as that
    // add's result, is a node of a sum tree (an inner sum).  Each tree is
    // rebuilt over its terms, the two narrowest added first: about log2(n)
    // adds deep, and the narrow sums near the leaves are gates that ABC packs
    // into LUTs.  roots: every literal the design reads (outputs, register
    // inputs, hard-cell pins).  Returns the number of trees rebuilt.
    int balanceSums(const Vec& roots) {
        const size_t n = m_chains.size();
        // A chain with sum bits is a sum: o, then co when it is used, the
        // exact (w + 1)-bit x + y + cyinit.  One without (an ordered compare)
        // only reads its operands.
        const auto isSum = [&](size_t j) { return !m_chains[j].o.empty(); };
        const auto resultOf = [&](size_t j) {
            Vec r = m_chains[j].o;
            if (m_chains[j].co >= 0) r.push_back(m_chains[j].co);
            return r;
        };
        std::unordered_map<int, size_t> chainOfBit;  // o and co placeholder var -> chain
        for (size_t j = 0; j < n; ++j) {
            for (const int l : resultOf(j)) chainOfBit.emplace(l >> 1, j);
        }
        // The sum whose result op is (all of it or its low bits), or n
        const auto sumOf = [&](const Vec& op) {
            if (op.empty()) return n;
            const auto it = chainOfBit.find(resolve(op[0]) >> 1);
            if (it == chainOfBit.end() || !isSum(it->second)) return n;
            const Vec r = resultOf(it->second);
            if (op.size() > r.size()) return n;
            for (size_t i = 0; i < op.size(); ++i) {
                if (resolve(op[i]) != r[i]) return n;
            }
            return it->second;
        };
        // Which results are read, and how: as the whole operand of one add
        // (wholeUses, consumer), or any other way (otherUse)
        std::vector<char> live(n, 0), otherUse(n, 0);
        std::vector<int> wholeUses(n, 0);
        std::vector<char> seen(m_fan.size(), 0);
        std::vector<int> vars;
        std::vector<size_t> chains;
        for (const int l : roots) vars.push_back(resolve(l) >> 1);
        for (const Dsp& d : m_dsps) {
            for (const Vec* const pinp : {&d.a, &d.b}) {
                for (const int l : *pinp) vars.push_back(resolve(l) >> 1);
            }
        }
        while (!vars.empty() || !chains.empty()) {
            if (!chains.empty()) {
                const size_t k = chains.back();
                chains.pop_back();
                const Chain& c = m_chains[k];
                const size_t width = resultOf(k).size();
                for (const Vec* const opp : {&c.x, &c.y}) {
                    const size_t j = sumOf(*opp);
                    if (j < n && isSum(k) && opp->size() >= width) {
                        ++wholeUses[j];
                        if (!live[j]) {
                            live[j] = 1;
                            chains.push_back(j);
                        }
                    } else {
                        for (const int l : *opp) vars.push_back(resolve(l) >> 1);
                    }
                }
                vars.push_back(resolve(c.cyinit) >> 1);
                continue;
            }
            const int v = vars.back();
            vars.pop_back();
            if (!v || seen[v]) continue;
            seen[v] = 1;
            const auto it = chainOfBit.find(v);
            if (it != chainOfBit.end()) {
                otherUse[it->second] = 1;
                if (!live[it->second]) {
                    live[it->second] = 1;
                    chains.push_back(it->second);
                }
                continue;
            }
            if (isPlaceholder(v)) continue;
            vars.push_back(resolve(m_fan[v].first) >> 1);
            vars.push_back(resolve(m_fan[v].second) >> 1);
        }
        const auto inner = [&](size_t j) { return live[j] && !otherUse[j] && wholeUses[j] == 1; };
        std::unordered_map<int, int> memo;
        std::vector<char> removed(n, 0);
        int trees = 0;
        for (size_t k = 0; k < n; ++k) {
            if (!live[k] || !isSum(k) || inner(k)) continue;
            const Vec r = resultOf(k);
            const size_t width = r.size();
            // The terms of the tree under k, each cut to width bits
            std::vector<Vec> terms;
            std::vector<size_t> todo{k};
            size_t nodes = 0;
            while (!todo.empty()) {
                const Chain& c = m_chains[todo.back()];
                todo.pop_back();
                ++nodes;
                for (const Vec* const opp : {&c.x, &c.y}) {
                    // inner(j): this is j's one use, a whole operand at least
                    // as wide as c's result, so j's sum mod 2^width is its terms'
                    const size_t j = sumOf(*opp);
                    if (j < n && inner(j)) {
                        todo.push_back(j);
                    } else {
                        terms.push_back(resize(*opp, static_cast<int>(width), false));
                    }
                }
                terms.push_back(resize(Vec{c.cyinit}, static_cast<int>(width), false));
            }
            if (nodes < 2) continue;
            // Rebuild: the two narrowest terms first (width: up to the top
            // bit that is not constant 0); a sum of widths a and b needs
            // max(a, b) + 1 bits, or width when that is wider
            using Term = std::pair<size_t, Vec>;
            const auto wider = [](const Term& a, const Term& b) { return a.first > b.first; };
            std::vector<Term> heap;
            for (Vec& t : terms) {
                size_t w = t.size();
                while (w && constOf(t[w - 1], memo) == 0) --w;
                if (!w) continue;
                t.resize(w);
                heap.emplace_back(w, std::move(t));
                std::push_heap(heap.begin(), heap.end(), wider);
            }
            while (heap.size() > 1) {
                std::pop_heap(heap.begin(), heap.end(), wider);
                Term a = std::move(heap.back());
                heap.pop_back();
                std::pop_heap(heap.begin(), heap.end(), wider);
                Term b = std::move(heap.back());
                heap.pop_back();
                const size_t w = std::min(width, std::max(a.first, b.first) + 1);
                heap.emplace_back(w, add(resize(a.second, static_cast<int>(w), false),
                                         resize(b.second, static_cast<int>(w), false)));
                std::push_heap(heap.begin(), heap.end(), wider);
            }
            const Vec sum = resize(heap.empty() ? Vec{} : heap.front().second,
                                   static_cast<int>(width), false);
            for (size_t i = 0; i < width; ++i) {
                const int v = r[i] >> 1;  // pinnedVar(): an unaliased placeholder
                m_pinned.erase(v);
                m_alias[v] = sum[i] ^ (r[i] & 1);
            }
            todo.push_back(k);
            while (!todo.empty()) {
                const size_t j = todo.back();
                todo.pop_back();
                removed[j] = 1;
                for (const Vec* const opp : {&m_chains[j].x, &m_chains[j].y}) {
                    const size_t i = sumOf(*opp);
                    if (i < n && inner(i)) todo.push_back(i);
                }
            }
            ++trees;
        }
        std::vector<Chain> kept;
        for (size_t j = 0; j < m_chains.size(); ++j) {
            if (j >= n || !removed[j]) kept.push_back(std::move(m_chains[j]));
        }
        m_chains = std::move(kept);
        m_chainHash.clear();
        return trees;
    }
    static bool allConst(const Vec& v) {
        for (const int l : v)
            if ((l >> 1) != 0) return false;
        return true;
    }
    Vec add(const Vec& a, const Vec& b, int cin = 0) {
        if (isConst(cin)) {
            // Low bits where the carry can't change -- carry 0 and an operand bit 0, or carry 1
            // and an operand bit 1 -- sum to the other operand bit: wires, not chain (a shifted
            // operand, a constant's trailing bits)
            size_t k = 0;
            while (k < a.size() && (a[k] == cin || b[k] == cin)) ++k;
            if (k) {
                Vec r(a.size());
                for (size_t i = 0; i < k; ++i) r[i] = a[i] == cin ? b[i] : a[i];
                if (k < a.size()) {
                    const Vec hi
                        = add(Vec(a.begin() + k, a.end()), Vec(b.begin() + k, b.end()), cin);
                    std::copy(hi.begin(), hi.end(), r.begin() + k);
                }
                return r;
            }
        }
        if (m_hard && a.size() > MIN_CARRY && a.back() == 0 && b.back() == 0) {
            // Both top bits 0 (a zero-extended carry out): that sum bit is
            // the carry out of the rest, so the chain is one bit shorter
            const Vec la(a.begin(), a.end() - 1), lb(b.begin(), b.end() - 1);
            if (!(allConst(la) && allConst(lb))) {
                const size_t k = chainFor(la, lb, cin);
                if (m_chains[k].o.empty()) {
                    for (size_t i = 0; i < la.size(); ++i) m_chains[k].o.push_back(pinnedVar());
                }
                if (m_chains[k].co < 0) m_chains[k].co = pinnedVar();
                Vec r = m_chains[k].o;
                r.push_back(m_chains[k].co);
                return r;
            }
        }
        if (m_hard && a.size() >= MIN_CARRY && !(allConst(a) && allConst(b))) {
            const size_t k = chainFor(a, b, cin);
            if (m_chains[k].o.empty()) {
                for (size_t i = 0; i < a.size(); ++i) m_chains[k].o.push_back(pinnedVar());
            }
            return m_chains[k].o;
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
    // A constant with at most this many signed digits multiplies by shifts and adds
    static constexpr int MAX_CONST_TERMS = 4;
    Vec mul(const Vec& a, const Vec& b) {
        if (allConst(b) && !allConst(a)) return mulConst(a, b);
        if (allConst(a) && !allConst(b)) return mulConst(b, a);
        return mulVar(a, b);
    }
    // x * c for a constant c: the sum of x shifted by c's signed digits (canonical, so 7 is
    // 8 - 1), as wires and adders whose constant bits stay visible to the mapper -- a DSP48E1
    // would hide them, and 32 * i as an index would select any bit, not one of every 32.
    // A constant of more digits goes to DSP48E1s, less its trailing zeros.
    Vec mulConst(const Vec& x, const Vec& c) {
        const int w = x.size();
        std::vector<int> digit(w, 0);
        int carry = 0, terms = 0;
        for (int i = 0; i < w; ++i) {
            const int bit = (c[i] == 1 ? 1 : 0) + carry;
            const bool next = i + 1 < w && c[i + 1] == 1;
            if (bit == 1) {
                digit[i] = next ? -1 : 1;
                carry = next ? 1 : 0;
            } else {
                carry = bit == 2 ? 1 : 0;
            }
            if (digit[i]) ++terms;
        }
        if (!terms) return constVec(0, w);
        if (terms > MAX_CONST_TERMS && m_hard && static_cast<size_t>(w) >= MIN_DSP) {
            int t = 0;
            while (t < w && c[t] == 0) ++t;
            if (!t) return mulVar(x, c);
            const Vec hi = mul(Vec(x.begin(), x.end() - t), Vec(c.begin() + t, c.end()));
            Vec r(w, 0);
            std::copy(hi.begin(), hi.end(), r.begin() + t);
            return r;
        }
        std::vector<Vec> pos, neg;
        for (int i = 0; i < w; ++i) {
            if (!digit[i]) continue;
            Vec s(w, 0);
            for (int j = 0; i + j < w; ++j) s[i + j] = x[j];
            (digit[i] > 0 ? pos : neg).push_back(s);
        }
        const auto sum = [&](std::vector<Vec> parts) {
            while (parts.size() > 1) {  // balanced adder tree
                std::vector<Vec> next;
                for (size_t k = 0; k + 1 < parts.size(); k += 2)
                    next.push_back(add(parts[k], parts[k + 1]));
                if (parts.size() % 2) next.push_back(parts.back());
                parts.swap(next);
            }
            return parts[0];
        };
        const Vec p = pos.empty() ? constVec(0, w) : sum(pos);
        return neg.empty() ? p : sub(p, sum(neg));
    }
    // Width of a two's complement vector below its sign extension: the bits
    // above it all repeat its top bit (a sign-extended operand), or are 0 (a
    // zero-extended one, whose top bit is then a 0 sign)
    static int signedWidth(const Vec& v) {
        int n = v.size();
        while (n > 1 && v[n - 1] == v[n - 2]) --n;
        return n;
    }
    // A hard multiply is not built while the netlist is being elaborated:
    // its operand bits are often still placeholders then (a register's Q, a
    // net assigned further down), so neither a constant operand nor a sign
    // extension can be seen.  It is recorded with placeholder product bits
    // and built by buildMuls() once every alias is known.
    struct PendingMul final {
        Vec a, b, r;
    };
    std::vector<PendingMul> m_pendingMuls;
    bool m_buildingMuls = false;  // inside buildMuls(): the aliases are known, build at once
    Vec mulVar(const Vec& a, const Vec& b) {
        const int w = a.size();
        if (m_hard && static_cast<size_t>(w) >= MIN_DSP) {
            // Queued again from buildMuls() (mulConst's constant of many digits, less its
            // trailing zeros), it would come straight back to the queue being drained: unbounded
            // recursion, a stack overflow on a 32 x 32 multiply by a wide constant
            if (m_buildingMuls) return mulHard(a, b);
            PendingMul pm;
            pm.a = a;
            pm.b = b;
            for (int k = 0; k < w; ++k) pm.r.push_back(pinnedVar());
            m_pendingMuls.push_back(pm);
            return pm.r;
        }
        return mulGates(a, b);
    }
    void buildMuls() {
        VL_RESTORER(m_buildingMuls);
        m_buildingMuls = true;
        std::vector<PendingMul> muls;
        muls.swap(m_pendingMuls);
        for (const PendingMul& pm : muls) {
            Vec a, b;
            for (const int l : pm.a) a.push_back(resolve(l));
            for (const int l : pm.b) b.push_back(resolve(l));
            const Vec r = allConst(b) && !allConst(a)   ? mulConst(a, b)
                          : allConst(a) && !allConst(b) ? mulConst(b, a)
                                                        : mulHard(a, b);
            for (size_t k = 0; k < pm.r.size(); ++k) {
                const int v = pm.r[k] >> 1;
                m_pinned.erase(v);
                m_alias[v] = r[k];
            }
        }
        if (!m_pendingMuls.empty()) buildMuls();  // mulConst's adders are gates, but be safe
    }
    Vec mulHard(const Vec& a, const Vec& b) {
        const int w = a.size();
        {
            // One DSP48E1 does the whole product when the operands fit its
            // 25 x 18 two's complement multiplier below their sign extension
            // (a 16 x 16 signed multiply widened to 32 bits by the front end
            // is one tile, not three).  The low w bits of a product are the
            // same whether its operands are read signed or unsigned, so each
            // operand goes in sign-extended from its own top bit, and the
            // product is taken from P's low bits, P[47] above them.
            const int sa = signedWidth(a), sb = signedWidth(b);
            const bool ab = sa <= 25 && sb <= 18;
            if (ab || (sa <= 18 && sb <= 25)) {
                const Vec& x = ab ? a : b;
                const Vec& y = ab ? b : a;
                Dsp d;
                for (int k = 0; k < 25; ++k) d.a.push_back(k < w ? x[k] : x[w - 1]);
                for (int k = 0; k < 18; ++k) d.b.push_back(k < w ? y[k] : y[w - 1]);
                const auto hit = m_dspHash.find({d.a, d.b});
                size_t dk;
                if (hit != m_dspHash.end()) {
                    dk = hit->second;
                } else {
                    for (int k = 0; k < 48; ++k) d.p.push_back(pinnedVar());
                    m_dsps.push_back(d);
                    dk = m_dsps.size() - 1;
                    m_dspHash.emplace(std::make_pair(m_dsps[dk].a, m_dsps[dk].b), dk);
                }
                Vec r(w, 0);
                for (int k = 0; k < w; ++k) r[k] = m_dsps[dk].p[std::min(k, 47)];
                return r;
            }
            std::vector<Vec> parts;  // shifted partial products, w bits each
            for (int i = 0; 24 * i < w; ++i) {
                Vec ai;
                for (int k = 0; k < 24; ++k) ai.push_back(24 * i + k < w ? a[24 * i + k] : 0);
                if (allConst(ai)
                    && std::all_of(ai.begin(), ai.end(), [](int l) { return l == 0; }))
                    continue;
                for (int j = 0; 24 * i + 17 * j < w; ++j) {
                    Vec bj;
                    for (int k = 0; k < 17; ++k) bj.push_back(17 * j + k < w ? b[17 * j + k] : 0);
                    if (std::all_of(bj.begin(), bj.end(), [](int l) { return l == 0; })) continue;
                    Dsp d;
                    d.a = ai;
                    d.a.push_back(0);  // A[24] = 0: the 24 bits are unsigned
                    d.b = bj;
                    d.b.push_back(0);  // B[17] = 0
                    const auto hit = m_dspHash.find({d.a, d.b});
                    size_t dk;
                    if (hit != m_dspHash.end()) {
                        dk = hit->second;
                    } else {
                        for (int k = 0; k < 48; ++k) d.p.push_back(pinnedVar());
                        m_dsps.push_back(d);
                        dk = m_dsps.size() - 1;
                        m_dspHash.emplace(std::make_pair(m_dsps[dk].a, m_dsps[dk].b), dk);
                    }
                    const int sh = 24 * i + 17 * j;
                    Vec part(w, 0);
                    for (int k = 0; k < 41 && sh + k < w; ++k) part[sh + k] = m_dsps[dk].p[k];
                    parts.push_back(part);
                }
            }
            if (parts.empty()) return constVec(0, w);
            while (parts.size() > 1) {  // balanced adder tree
                std::vector<Vec> next;
                for (size_t k = 0; k + 1 < parts.size(); k += 2)
                    next.push_back(add(parts[k], parts[k + 1]));
                if (parts.size() % 2) next.push_back(parts.back());
                parts.swap(next);
            }
            return parts[0];
        }
    }
    Vec mulGates(const Vec& a, const Vec& b) {
        const int w = a.size();
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
            const size_t k = chainFor(a, NOTv(b), 1);
            if (m_chains[k].co < 0) m_chains[k].co = pinnedVar();
            return m_chains[k].co ^ 1;
        }
        int borrow = 0;
        for (size_t i = 0; i < a.size(); ++i)
            borrow = OR(AND(a[i] ^ 1, b[i]), AND(OR(a[i] ^ 1, b[i]), borrow));
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
            for (int i = 0; i < w; ++i)
                if (i - d >= 0) sh[i] = r[i - d];
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
            for (int i = 0; i < w; ++i)
                if (i + d < w) sh[i] = r[i + d];
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
    // A hard cell's placeholder output now stands for `lit` (its cell went away)
    void aliasTo(int placeholder, int lit) {
        const int v = placeholder >> 1;
        m_pinned.erase(v);
        m_alias[v] = lit ^ (placeholder & 1);
    }
    int resolve(int lit) {
        int guard = 0;
        while (true) {
            const int v = lit >> 1;
            if (!v || !isPlaceholder(v) || !hasAlias(v)) return lit;
            lit = m_alias[v] ^ (lit & 1);
            if (++guard > 1000000)
                svs_netlist::die("alias cycle in netlist (combinational loop through nets?)");
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
    AigB m_aig;  // the design's combinational logic as an AIG
    Design m_d;  // the structural netlist being mapped
    std::string m_top;  // name of the top module
    std::map<std::string, std::pair<int, bool>>
        m_portRange;  // top port -> (declared lsb, ascending)
    // AIG primary inputs: placeholder var -> PI index; and PI index -> var
    std::unordered_map<int, int> m_piOf;
    std::vector<int> m_piVar;  // PI index -> placeholder var
    // primary outputs
    std::vector<int> m_po;  // literals
    // yosys nets
    int m_nextNet = 2;
    std::unordered_map<int, std::string> m_netOfVar;  // PI var -> net json ("12" or "\"0\"")
    std::vector<std::string> m_poNet;  // PO index -> net json
    std::ostringstream m_cells;  // the mapped cells, as JSON text
    int m_ncells = 0;  // number of cells emitted
    std::unordered_map<std::string, int> m_counts;
    std::vector<std::string> m_warnings;  // warnings collected while mapping, printed at the end
    // --map-only-meminit: one JSON object per memory (see writeMemInit), collected as the memories
    // are emitted
    std::vector<std::string> m_memInit;
    // instance scopes (partitioned mapping): id -> (prefix "a.b.", module type); 0 = top
    std::vector<std::pair<std::string, std::string>> m_scopes;
    std::unordered_map<std::string, int> m_scopeId;
    // the nets of the partition boundary nodes (v<var>) in the last emission, for boundaryTimes
    std::unordered_map<std::string, std::string> m_cutNet;
    // Electrical rule check on the flat netlist: every bit a cell input reads
    // must be driven by a cell output or a top-level input (or be a constant).
    // The first reader of each net, for the report, and every driven net.
    std::unordered_map<std::string, std::string> m_firstLoad;
    std::unordered_set<std::string>
        m_drivenNets;  // every net a cell output or top-level input drives
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
        // The declared index range of each top port.  The structural netlist is
        // written 0-based, so this comes from the AST: `input [5:1] addr` has
        // bit 0 at index 1, which the JSON says as yosys does ("offset": 1, and
        // "upto": 1 for an ascending range).
        for (AstNode* np = v3Global.rootp()->topModulep()->stmtsp(); np; np = np->nextp()) {
            const AstVar* const varp = VN_CAST(np, Var);
            if (!varp || !varp->isIO() || !varp->dtypep()) continue;
            const AstNodeDType* const d = varp->dtypep()->skipRefp();
            VNumRange r;
            if (const AstBasicDType* const bp = VN_CAST(d, BasicDType))
                r = bp->declRange();
            else if (const AstPackArrayDType* const pp = VN_CAST(d, PackArrayDType))
                r = pp->declRange();
            else
                continue;
            if (r.lo() != 0 || r.ascending())
                m_portRange[sanitize(varp->prettyName())] = {r.lo(), r.ascending()};
        }
        if (!mods.count(m_top)) {
            for (const auto& kv : mods) {  // top = a module nobody instantiates
                bool used = false;
                for (const auto& kv2 : mods)
                    for (const auto& it : kv2.second.items)
                        used |= it.kind == svs_netlist::Item::CELL && it.modName == kv.first;
                if (!used) {
                    m_top = kv.first;
                    break;
                }
            }
        }
        // full interfaces of the (* blackbox *) primitives, for the EDIF library
        for (const auto& kv : mods) {
            if (!kv.second.blackbox) continue;
            auto& ports = m_ifaces[kv.first];
            for (const std::string& pn : kv.second.ports) {
                for (const svs_netlist::NetDecl& d : kv.second.nets) {
                    if (d.name == pn)
                        ports.push_back({pn, d.dir >= 2, static_cast<size_t>(d.width)});
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
        m_aig.buildMuls();
        const int deadDsps = m_aig.pruneDsps();
        const int idleChains = m_aig.bypassIdleChains();
        if (!v3Global.opt.mapOnlyKeepSumChains()) {
            const int trees = m_aig.balanceSums(designReads());
            if (trees && v3Global.opt.structOnlyReport())
                std::cerr << "%Info: map-sums: " << trees
                          << " sums of chained adds rebuilt as balanced trees\n";
        }
        if ((deadDsps || idleChains) && v3Global.opt.structOnlyReport())
            std::cerr << "%Info: map-prune: " << deadDsps
                      << " DSP48E1 partial products with a constant-0 operand removed, "
                      << idleChains << " carry chains adding 0 bypassed\n";
        {
            const int merged = v3Global.opt.mapOnlyKeepEquivRegs() ? 0 : mergeEquivRegs();
            int absorbed = absorbDspRegs();
            absorbed += absorbDspAccumulators();
            absorbed += absorbDspRegs();  // a second stage behind P: M and P
            const int inputs = absorbDspInputRegs();
            if ((merged || absorbed || inputs) && v3Global.opt.structOnlyReport())
                std::cerr << "%Info: map-regs: " << merged << " equivalent flip-flops merged, "
                          << absorbed << " flip-flops absorbed into DSP48E1 M/P registers, "
                          << inputs << " into A/B registers\n";
        }
        buildPOs();
        const std::string base = filename.empty() ? v3Global.opt.mapOnlyEdif() : filename;
        const std::string aig = base + ".aig";
        const std::string blif = base + ".blif";
        m_record = !v3Global.opt.mapOnlyEdif().empty() || !v3Global.opt.mapOnlyVerilog().empty();
        // Timing: the constraints, and the analyser the mapped netlist goes through
        std::unique_ptr<V3MapTiming> timing;
        V3MapSdcConstraints sdc;
        if (v3Global.opt.mapOnlyTiming()) {
            if (!v3Global.opt.mapOnlySdc().empty()) {
                std::string err;
                if (!V3MapSdcRead(v3Global.opt.mapOnlySdc(), sdc, err))
                    v3fatal("Synthesis: " << err);
            }
            V3MapTiming::Options to;
            to.defaultPeriod = v3Global.opt.mapOnlyPeriod();
            to.reportPaths = v3Global.opt.mapOnlyTimingPaths();
            timing = std::make_unique<V3MapTiming>(sdc, to);
            m_record = true;
        }
        // With timing, the first mapping is analysed and its arrival and
        // required times at abc's boundaries drive a second mapping (and so on,
        // --map-only-timing-iterations); only the last one is written out.
        const int passes = timing ? 1 + std::max(0, v3Global.opt.mapOnlyTimingIterations()) : 1;
        const std::vector<int> poSnapshot = m_po;
        BoundaryTimes bt;
        std::vector<std::string> timingNotes;
        // The pass the analysis liked best (worst slack, then total) is the one written
        std::map<std::string, Lut> bestLuts;
        std::vector<std::string> bestOrder;
        int bestPass = -1;
        double bestWns = 0, bestTns = 0;
        for (int pass = 0; pass < passes; ++pass) {
            bool last = pass + 1 == passes;
            std::map<std::string, Lut> luts;
            std::vector<std::string> lutOrder;
            if (pass) {
                m_po = poSnapshot;
                resetEmission();
            }
            // Nothing comes out of the fabric (no top outputs, register inputs or
            // hard-cell pins: a module with no outputs, or one whose logic all
            // folded away): no LUTs to map, and ABC asserts on a network with no
            // outputs (Abc_NtkCheckNames), so it is not run
            if (m_po.empty()) {
            } else if (v3Global.opt.mapOnlyWholeProgram()) {
                writeAiger(aig);
                runAbc(aig, blif, timing != nullptr, pass ? &bt.ciArrival : nullptr,
                       pass ? &bt.coRequired : nullptr);
                readBlif(blif, luts, lutOrder);
                if (last && !v3Global.opt.mapOnlyKeepTemps()) {
                    std::remove(aig.c_str());
                    std::remove(blif.c_str());
                }
            } else {
                mapPartitioned(base, luts, lutOrder, timing != nullptr, pass ? &bt : nullptr);
            }
            if (timing && passes > 1) last = false;  // written below, once the best pass is known
            // abc's netlist as read, before the emission reworks it (register
            // polarity, fanout copies): what a re-emission of this pass starts from
            const std::map<std::string, Lut> lutsRead
                = timing ? luts : std::map<std::string, Lut>{};
            const std::vector<std::string> orderRead
                = timing ? lutOrder : std::vector<std::string>{};
            emitJson(last ? filename : std::string{}, luts, lutOrder, last);
            if (timing) {
                std::string err;
                if (!timing->analyse(m_recs, m_portRecs, err)) v3fatal("Synthesis: " << err);
                std::istringstream lines{timing->summaryText()};
                for (std::string l; std::getline(lines, l);)
                    timingNotes.push_back((passes > 1 ? "pass " + std::to_string(pass) + ": " : "")
                                          + l);
                double wns = 0, tns = 0;
                for (const V3MapTiming::Domain& d : timing->summary().domains) {
                    wns = std::min(wns, d.wns);
                    tns += d.tns;
                }
                if (bestPass < 0 || wns > bestWns + 1e-6
                    || (std::abs(wns - bestWns) <= 1e-6 && tns > bestTns)) {
                    bestPass = pass;
                    bestWns = wns;
                    bestTns = tns;
                    bestLuts = lutsRead;
                    bestOrder = orderRead;
                }
                if (pass + 1 < passes) {
                    boundaryTimes(*timing, bt);
                    m_lutNetDelay = timing->averageNetDelay();
                }
            }
        }
        if (timing && passes > 1) {
            m_po = poSnapshot;
            resetEmission();
            emitJson(filename, bestLuts, bestOrder, true);
            std::string err;
            if (!timing->analyse(m_recs, m_portRecs, err)) v3fatal("Synthesis: " << err);
            timingNotes.push_back("written: pass " + std::to_string(bestPass));
        }
        if (timing) {
            for (const std::string& l : timingNotes)
                std::cerr << "%Info: map-timing: " << l << "\n";
            for (const std::string& w : sdc.warnings) m_warnings.push_back(w);
            for (const std::string& w : timing->summary().warnings) m_warnings.push_back(w);
            if (!v3Global.opt.mapOnlyTimingReport().empty()) {
                std::string err;
                if (!timing->writeReport(v3Global.opt.mapOnlyTimingReport(), err))
                    v3fatal("Synthesis: " << err);
            }
        }
        for (const std::string& w : m_warnings) std::cerr << "%Warning-MAP: " << w << "\n";
    }

private:
    // Forget one emission so the next pass starts clean (the net numbering
    // of the sources is kept: it is theirs, not the pass's)
    void resetEmission() {
        m_cells.str("");
        m_cells.clear();
        m_ncells = 0;
        m_counts.clear();
        m_recs.clear();
        m_portRecs.clear();
        m_firstLoad.clear();
        m_drivenNets.clear();
        m_regQ.clear();
        m_netName.clear();
        m_netNames.clear();
        m_warnings.clear();
        m_poNet.clear();
        m_flippedDpo.clear();
        m_flippedVar.clear();
        m_flipGain = 0;
        m_curPath.clear();
        m_curLeaf.clear();
        m_edifIds = 0;
        m_regDup.clear();
        m_spread.clear();
        m_dupLuts = m_dupRegs = 0;
    }

    // ---- fanout ----
    // A net read by more than --map-only-max-fanout pins is slow: the router
    // fans it out through several wires and a long one to the far readers (a
    // fanout of 400 costs a nanosecond on a 7-series).  A LUT driving such a
    // net is copied, each copy taking a share of the readers, and so is a
    // flip-flop (a second register with the same D, enable and reset).  The
    // readers are the LUTs abc wrote (a 7- or 8-input cover is two or four
    // LUT6s) and the pins outside abc (register D, enable and reset pins --
    // an enable serves every bit of its register -- hard-cell pins, pads);
    // the register pins are spread over the copies when the registers are
    // emitted, everything else stays on the original.  Done on abc's netlist
    // before it is numbered.
    struct Spread final {
        std::vector<std::string> names;  // the node, then its copies (BLIF names)
        size_t readerGroup = 0, readerFill = 0;  // where the LUT readers stopped filling
        size_t outsideNext = 0;  // outside consumers handed out so far
    };
    std::unordered_map<std::string, Spread> m_spread;  // by node
    std::map<std::pair<size_t, size_t>, std::vector<std::string>>
        m_regDup;  // (register, bit) -> copies' BLIF names
    int m_dupLuts = 0;
    int m_dupRegs = 0;
    struct Lut;  // abc's LUT (BLIF back, below)
    static bool isBufLut(const Lut& l) { return isBufLutImpl(l); }  // (defined with Lut, below)
    // abc's buffers (`.names a b / 1 1`) become net aliases when emitted: the node a name stands
    // for
    static std::string resolveLut(const std::map<std::string, Lut>& luts, std::string n) {
        for (int guard = 0; guard < 64; ++guard) {
            const auto it = luts.find(n);
            if (it == luts.end() || !isBufLut(it->second)) break;
            n = it->second.ins[0];
        }
        return n;
    }
    // The node a PO's net comes from: a LUT, a PI passed straight through, or none (a constant)
    std::string poNode(const std::map<std::string, Lut>& luts, size_t i) {
        const std::string pn = "po" + std::to_string(i);
        if (luts.count(pn)) return resolveLut(luts, pn);
        const int r = m_aig.resolve(m_po[i]);
        const auto pit = m_piOf.find(r >> 1);
        if (r <= 1 || pit == m_piOf.end()) return std::string{};
        return "pi" + std::to_string(pit->second);
    }
    void limitFanout(std::map<std::string, Lut>& luts, std::vector<std::string>& order) {
        const size_t F = v3Global.opt.mapOnlyMaxFanout();
        if (F < 2) return;
        std::unordered_map<int, std::pair<size_t, size_t>> regOfVar;  // Q var -> (register, bit)
        std::vector<size_t> poUses(m_po.size(), 1);  // pins outside abc reading each PO
        for (size_t ri = 0; ri < m_d.regs.size(); ++ri) {
            const Design::Reg& r = m_d.regs[ri];
            if (r.latch) continue;
            const RegOut& ro = m_regPo[ri];
            if (ro.en >= 0 && static_cast<size_t>(ro.en) < poUses.size())
                poUses[ro.en] += r.q.size() - 1;
            if (ro.rst >= 0 && static_cast<size_t>(ro.rst) < poUses.size())
                poUses[ro.rst] += r.q.size() - 1;
            for (size_t b = 0; b < r.q.size(); ++b) {
                const int ql = m_aig.resolve(r.q[b]);
                if (ql < 2 || (ql & 1)) continue;
                regOfVar[ql >> 1] = {ri, b};
            }
        }
        struct Reader final {
            std::string lut;
            size_t input, weight;
        };
        std::unordered_map<std::string, std::vector<Reader>>
            readers;  // node -> LUT inputs reading it
        std::unordered_map<std::string, size_t> outside;  // node -> pins outside abc reading it
        for (const std::string& o : order) {
            const Lut& l = luts[o];
            if (isBufLut(l)) continue;  // its readers are counted on the node it aliases
            const size_t w = l.ins.size() <= 6 ? 1 : l.ins.size() == 7 ? 2 : 4;
            for (size_t i = 0; i < l.ins.size(); ++i)
                readers[resolveLut(luts, l.ins[i])].push_back({o, i, w});
        }
        for (size_t i = 0; i < m_po.size(); ++i) {
            const std::string node = poNode(luts, i);
            if (!node.empty()) outside[node] += poUses[i];
        }
        const auto spread
            = [&](const std::string& node, const std::function<std::string(int)>& makeCopy) {
                  size_t total = 0;
                  const auto rit = readers.find(node);
                  if (rit != readers.end())
                      for (const Reader& r : rit->second) total += r.weight;
                  const auto oit = outside.find(node);
                  if (oit != outside.end()) total += oit->second;
                  if (total <= F) return 0;
                  Spread sp;
                  sp.names.push_back(node);
                  const size_t copies = (total + F - 1) / F - 1;
                  for (size_t k = 1; k <= copies; ++k)
                      sp.names.push_back(makeCopy(static_cast<int>(k)));
                  size_t g = 0, fill = 0;
                  if (rit != readers.end()) {
                      for (const Reader& r : rit->second) {
                          if (fill + r.weight > F && g + 1 < sp.names.size()) {
                              ++g;
                              fill = 0;
                          }
                          fill += r.weight;
                          if (g) luts[r.lut].ins[r.input] = sp.names[g];
                      }
                  }
                  sp.readerGroup = g;
                  sp.readerFill = fill;
                  m_spread[node] = sp;
                  return static_cast<int>(copies);
              };
        const std::vector<std::string> names = order;  // the copies are appended
        for (const std::string& o : names) {
            if (luts[o].ins.size() < 2)
                continue;  // buffers and constants are aliases, inverters go into the readers
            m_dupLuts += spread(o, [&](int k) {
                Lut c = luts[o];
                c.out = o + "$dup" + std::to_string(k);
                luts[c.out] = c;
                order.push_back(c.out);
                return c.out;
            });
        }
        for (size_t i = 0; i < m_piVar.size(); ++i) {
            const auto rit = regOfVar.find(m_piVar[i]);
            if (rit == regOfVar.end()) continue;
            const std::string pi = "pi" + std::to_string(i);
            m_dupRegs += spread(pi, [&](int k) {
                const std::string cp = pi + "$dup" + std::to_string(k);
                m_regDup[rit->second].push_back(cp);
                return cp;
            });
        }
        if ((m_dupLuts || m_dupRegs) && v3Global.opt.structOnlyReport())
            std::cerr << "%Info: map-fanout: " << m_dupLuts << " LUT and " << m_dupRegs
                      << " flip-flop copies for a fanout of at most " << F << "\n";
    }
    double m_lutNetDelay
        = -1;  // the net a LUT level costs in abc's library (< 0: the model's default)
    // The analysed netlist's times at abc's boundary: when every source (a
    // register output, a hard cell's output, an input pad) delivers each
    // primary input, and when every primary output (a register's D, a hard
    // cell's input, an output pad) must be ready; NaN where unconstrained
    // The times the analysis of one mapping found where abc's networks begin
    // and end, for the next mapping: per primary input and output of the
    // whole netlist, and per partition boundary node by its v<var> name
    struct BoundaryTimes final {
        std::vector<double> ciArrival, coRequired;
        std::unordered_map<std::string, double> cutArrival, cutRequired;
    };
    // The required time abc is given for a net: the analysis's less a margin
    // (a fifth of the period unless given: abc's per-level delay is an
    // average, the routing of what it builds is not), so a cone that has to
    // be faster is asked for; one that meets it keeps the arrival it has, so
    // area recovery does not spend the slack it was given on logic the
    // routing then eats
    double abcRequired(const V3MapTiming& timing, const std::string& net) const {
        if (net.empty() || net[0] == '"') return std::numeric_limits<double>::quiet_NaN();
        const double margin = v3Global.opt.mapOnlyTimingMargin() >= 0
                                  ? v3Global.opt.mapOnlyTimingMargin()
                                  : 0.2 * timing.minPeriod();
        const double req = timing.required(net) - margin;
        const double arr = timing.arrival(net);
        return std::isnan(arr) ? req : std::min(req, arr);
    }
    void boundaryTimes(const V3MapTiming& timing, BoundaryTimes& bt) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        bt.ciArrival.assign(m_piVar.size(), nan);
        bt.coRequired.assign(m_po.size(), nan);
        for (size_t i = 0; i < m_piVar.size(); ++i) {
            const auto it = m_netOfVar.find(m_piVar[i]);
            if (it != m_netOfVar.end()) bt.ciArrival[i] = timing.arrival(it->second);
        }
        for (size_t i = 0; i < m_po.size() && i < m_poNet.size(); ++i)
            bt.coRequired[i] = abcRequired(timing, m_poNet[i]);
        bt.cutArrival.clear();
        bt.cutRequired.clear();
        for (const auto& kv : m_cutNet) {
            bt.cutArrival[kv.first] = timing.arrival(kv.second);
            bt.cutRequired[kv.first] = abcRequired(timing, kv.second);
        }
    }

private:
    static std::string sanitize(const std::string& s) {
        std::string o;
        for (const char c : s)
            o += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') ? c : '_';
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
    struct RegOut final {
        int d0;
        int en, rst;
    };
    std::vector<RegOut> m_regPo;  // per register: PO index of d[0] (d bits consecutive), en, rst
    std::vector<std::vector<int>>
        m_cellInPo;  // per cell: PO index per input pin (consecutive bits)
    std::vector<int> m_topOutPo;  // per top output name (in m_d.outputs order): PO of bit 0

    static bool bitOf(unsigned long long v, size_t b) { return b < 64 && ((v >> b) & 1); }
    // Keep the bits of r for which keep[b], remembering their original index
    static void keepRegBits(Design::Reg& r, const std::vector<bool>& keep) {
        Vec q, d;
        std::vector<int> no;
        for (size_t b = 0; b < r.q.size(); ++b) {
            if (!keep[b]) continue;
            q.push_back(r.q[b]);
            d.push_back(r.d[b]);
            no.push_back(r.bitNo.empty() ? static_cast<int>(b) : r.bitNo[b]);
        }
        r.q = q;
        r.d = d;
        r.bitNo = no;
    }
    void dropEmptyRegs() {
        std::vector<Design::Reg> kept;
        for (Design::Reg& r : m_d.regs)
            if (!r.q.empty()) kept.push_back(std::move(r));
        m_d.regs = std::move(kept);
    }
    // Flip-flops with the same D, clock enable, reset, clock, edge and reset /
    // power-up values hold the same value every cycle: keep the first and
    // read it wherever the others were read (Vivado does the same unless
    // -keep_equivalent_registers).  Only the D literal is compared, so a
    // register whose next state reads another duplicate (a duplicated
    // counter) merges only once its inputs did: repeat until nothing changes.
    // kept Q literal -> the names of the equivalent flip-flops merged into it
    std::map<int, std::vector<std::string>> m_mergedFrom;
    int mergeEquivRegs() {
        int merged = 0;
        for (int pass = 0; pass < 8; ++pass) {
            using Key = std::tuple<int, int, int, int, int, int, int>;
            std::map<Key, int> first;  // -> Q literal of the bit kept
            int now = 0;
            for (Design::Reg& r : m_d.regs) {
                if (r.latch) continue;
                std::vector<bool> keep(r.q.size(), true);
                const int en = m_aig.resolve(r.en), rst = m_aig.resolve(r.rst),
                          clk = m_aig.resolve(r.clk);
                const int flags = (r.async ? 1 : 0) | (r.negedge ? 2 : 0);
                for (size_t b = 0; b < r.q.size(); ++b) {
                    const size_t bn = r.bitNo.empty() ? b : static_cast<size_t>(r.bitNo[b]);
                    const int qv = r.q[b] >> 1;
                    if ((r.q[b] & 1) || !m_aig.isPlaceholder(qv) || m_aig.hasAlias(qv)
                        || m_aig.m_pinned.count(qv))
                        continue;
                    const int vals
                        = (bitOf(r.resetVal, bn) ? 1 : 0) | (bitOf(r.initVal, bn) ? 2 : 0);
                    const Key k{m_aig.resolve(r.d[b]), en, rst, clk, flags, vals, 0};
                    const auto it = first.find(k);
                    if (it == first.end()) {
                        first.emplace(k, r.q[b]);
                        continue;
                    }
                    m_aig.equate(r.q[b], it->second);  // this Q now reads the kept one
                    // remembered on the kept flip-flop (MERGED_EQUIV), so the
                    // placer can split it again where its sinks ended up far apart
                    m_mergedFrom[it->second].push_back(sanitize(r.name) + "_"
                                                       + std::to_string(bn));
                    keep[b] = false;
                    ++now;
                }
                if (std::find(keep.begin(), keep.end(), false) != keep.end()) keepRegBits(r, keep);
            }
            dropEmptyRegs();
            merged += now;
            if (!now) break;
        }
        return merged;
    }
    // A product register: flip-flops whose D is a DSP48E1 product bit go into
    // that DSP's P register (PREG = 1) when every product bit read anywhere is
    // read only by such a flip-flop, all of them on one clock edge with one
    // clock enable and one synchronous reset (RSTP), resetting and powering
    // up to 0.  The flip-flops' Q nets become the DSP's P outputs.
    int absorbDspRegs() {
        // product bit var -> (dsp, bit)
        std::unordered_map<int, std::pair<size_t, size_t>> pOf;
        for (size_t i = 0; i < m_aig.m_dsps.size(); ++i)
            for (size_t k = 0; k < m_aig.m_dsps[i].p.size(); ++k)
                pOf[m_aig.m_dsps[i].p[k] >> 1] = {i, k};
        if (pOf.empty()) return 0;
        // candidate flip-flop bits: D is exactly (uncomplemented) a product bit
        struct Cand final {
            size_t ri, b;
            size_t dsp, k;
        };
        std::vector<Cand> cands;
        std::set<std::pair<size_t, size_t>> isCand;  // (ri, b)
        for (size_t ri = 0; ri < m_d.regs.size(); ++ri) {
            const Design::Reg& r = m_d.regs[ri];
            for (size_t b = 0; b < r.q.size(); ++b) {
                const int d = m_aig.resolve(r.d[b]);
                if (d & 1) continue;
                const auto it = pOf.find(d >> 1);
                if (it == pOf.end()) continue;
                cands.push_back({ri, b, it->second.first, it->second.second});
                isCand.insert({ri, b});
            }
        }
        if (cands.empty()) return 0;
        // product bits read by anything else (logic, other pins, outputs)
        std::unordered_set<int> seen, usedElse;
        std::vector<int> stack;
        auto sink = [&](int lit) { stack.push_back(m_aig.resolve(lit) >> 1); };
        for (const auto& kv : m_d.outputs)
            for (const int l : kv.second) sink(l);
        for (size_t ri = 0; ri < m_d.regs.size(); ++ri) {
            const Design::Reg& r = m_d.regs[ri];
            for (size_t b = 0; b < r.d.size(); ++b)
                if (!isCand.count({ri, b})) sink(r.d[b]);
            sink(r.en);
            sink(r.rst);
            sink(r.clk);
        }
        for (const Design::Cell& c : m_d.cells2)
            for (const auto& pin : c.ins)
                for (const int l : pin.second) sink(l);
        for (const AigB::Chain& ch : m_aig.m_chains) {
            for (const int l : ch.s) sink(l);
            for (const int l : ch.di) sink(l);
            sink(ch.cyinit);
        }
        for (const AigB::Dsp& dd : m_aig.m_dsps) {
            for (const int l : dd.a) sink(l);
            for (const int l : dd.b) sink(l);
        }
        while (!stack.empty()) {
            const int v = stack.back();
            stack.pop_back();
            if (!v || !seen.insert(v).second) continue;
            if (pOf.count(v)) usedElse.insert(v);
            if (m_aig.isPlaceholder(v)) continue;
            stack.push_back(m_aig.resolve(m_aig.fanin(v).first) >> 1);
            stack.push_back(m_aig.resolve(m_aig.fanin(v).second) >> 1);
        }
        // per DSP: its candidates, if they qualify together
        std::map<size_t, std::vector<size_t>> byDsp;
        for (size_t c = 0; c < cands.size(); ++c) byDsp[cands[c].dsp].push_back(c);
        std::map<size_t, std::vector<bool>> drop;  // ri -> bits taken out
        int absorbed = 0;
        for (const auto& kv : byDsp) {
            AigB::Dsp& dsp = m_aig.m_dsps[kv.first];
            bool ok = !dsp.mreg;  // M and P taken: no third stage in the DSP
            std::set<size_t> ks;
            const Design::Reg& r0 = m_d.regs[cands[kv.second[0]].ri];
            if (dsp.preg || dsp.areg || dsp.breg)
                ok = ok && m_aig.resolve(r0.clk) == m_aig.resolve(dsp.clk);
            for (const size_t c : kv.second) {
                const Cand& cd = cands[c];
                const Design::Reg& r = m_d.regs[cd.ri];
                const size_t bn = r.bitNo.empty() ? cd.b : static_cast<size_t>(r.bitNo[cd.b]);
                const int qv = r.q[cd.b] >> 1;
                ok = ok && !r.latch && !r.negedge && !(r.async && m_aig.resolve(r.rst) != 0)
                     && !bitOf(r.resetVal, bn) && !bitOf(r.initVal, bn)
                     && m_aig.resolve(r.clk) == m_aig.resolve(r0.clk)
                     && m_aig.resolve(r.en) == m_aig.resolve(r0.en)
                     && m_aig.resolve(r.rst) == m_aig.resolve(r0.rst) && !(r.q[cd.b] & 1)
                     && m_aig.isPlaceholder(qv) && !m_aig.hasAlias(qv)
                     && ks.insert(cd.k).second;  // one flip-flop per product bit
            }
            for (const int l : dsp.p) ok = ok && !usedElse.count(l >> 1);
            if (!ok) continue;
            if (dsp.preg) {
                // the flip-flops behind P: P's stage becomes M, theirs P
                dsp.mreg = true;
                dsp.cem = dsp.cep;
                dsp.rstm = dsp.rstp;
            }
            dsp.preg = true;
            dsp.clk = r0.clk;
            dsp.cep = r0.en;
            dsp.rstp = r0.rst;
            dsp.pw = ks.size();
            for (const size_t c : kv.second) {
                const Cand& cd = cands[c];
                Design::Reg& r = m_d.regs[cd.ri];
                const int qlit = r.q[cd.b];
                m_aig.m_pinned.erase(dsp.p[cd.k] >> 1);
                dsp.p[cd.k] = qlit;  // P now drives the flip-flop's Q net
                m_aig.m_pinned.insert(qlit >> 1);
                auto& dv = drop[cd.ri];
                if (dv.empty()) dv.assign(r.q.size(), false);
                dv[cd.b] = true;
                ++absorbed;
            }
        }
        for (auto& kv : drop) {
            std::vector<bool> keep(kv.second.size());
            for (size_t b = 0; b < keep.size(); ++b) keep[b] = !kv.second[b];
            keepRegBits(m_d.regs[kv.first], keep);
        }
        dropEmptyRegs();
        return absorbed;
    }

    // Accumulator: a register whose next value is itself plus a DSP48E1's
    // product register (`acc <= acc + mult_reg`, Vivado's macc example) goes
    // into the DSP: the product register becomes M (MREG = 1), the
    // accumulator P with the post-adder fed back (OPMODE Z = P, P <= P + M),
    // and the carry chain of the add goes away.  Conditions: the product
    // register is read only by that chain, extended there by its sign (and
    // the product fits it: sa + sb - 1 bits) or by zeros (both operands
    // non-negative, sa + sb - 2 bits); the chain's other operand is the
    // accumulator's Q, its outputs are read only by the accumulator's D, no
    // carry in or out; the accumulator's flip-flops meet the P register's
    // conditions on the DSP's clock.  Returns the flip-flop bits absorbed.
    int absorbDspAccumulators() {
        int absorbed = 0;
        for (size_t di = 0; di < m_aig.m_dsps.size(); ++di) {
            AigB::Dsp& dsp = m_aig.m_dsps[di];
            if (!dsp.preg || dsp.mreg || dsp.acc || dsp.pw == 0 || dsp.pw > 48) continue;
            const int pw = dsp.pw;
            std::unordered_map<int, size_t> pOf;  // product register Q var -> bit
            for (int k = 0; k < pw; ++k) pOf[dsp.p[k] >> 1] = k;
            std::unordered_map<int, std::pair<size_t, size_t>> qOf;  // Q var -> (reg, bit)
            for (size_t ri = 0; ri < m_d.regs.size(); ++ri)
                for (size_t b = 0; b < m_d.regs[ri].q.size(); ++b)
                    qOf[m_d.regs[ri].q[b] >> 1] = {ri, b};
            const int sa = AigB::signedWidth(dsp.a), sb = AigB::signedWidth(dsp.b);
            const bool nonneg
                = m_aig.resolve(dsp.a.back()) == 0 && m_aig.resolve(dsp.b.back()) == 0;
            for (size_t ci = 0; ci < m_aig.m_chains.size(); ++ci) {
                const AigB::Chain& ch = m_aig.m_chains[ci];
                const size_t w = ch.x.size();
                if (w > 48 || w < static_cast<size_t>(pw) || m_aig.resolve(ch.cyinit) != 0
                    || ch.o.size() != w)
                    continue;
                // which operand is the extended product register
                int side = -1;
                for (int sd = 0; sd < 2 && side < 0; ++sd) {
                    const Vec& y = sd ? ch.y : ch.x;
                    bool match = true;
                    for (int k = 0; k < pw && match; ++k) match = m_aig.resolve(y[k]) == dsp.p[k];
                    if (!match) continue;
                    bool sext = true, zext = true;
                    for (size_t k = pw; k < w; ++k) {
                        const int l = m_aig.resolve(y[k]);
                        sext = sext && l == dsp.p[pw - 1];
                        zext = zext && l == 0;
                    }
                    if ((sext && sa + sb - 1 <= pw) || (zext && nonneg && sa + sb - 2 <= pw)
                        || (sext && zext))
                        side = sd;
                }
                if (side < 0) continue;
                const Vec& x = side ? ch.x : ch.y;  // the accumulator's Q
                // accumulator flip-flops: x[i] is Q of bit i, D of bit i is o[i]
                std::vector<std::pair<size_t, size_t>> bits;
                bool ok = true;
                for (size_t i = 0; i < w && ok; ++i) {
                    const int l = m_aig.resolve(x[i]);
                    const auto it = (l & 1) ? qOf.end() : qOf.find(l >> 1);
                    ok = it != qOf.end();
                    if (!ok) break;
                    const Design::Reg& r = m_d.regs[it->second.first];
                    const size_t bn = r.bitNo.empty()
                                          ? it->second.second
                                          : static_cast<size_t>(r.bitNo[it->second.second]);
                    ok = !r.latch && !r.negedge && !(r.async && m_aig.resolve(r.rst) != 0)
                         && !bitOf(r.resetVal, bn) && !bitOf(r.initVal, bn)
                         && m_aig.resolve(r.d[it->second.second]) == ch.o[i]
                         && m_aig.resolve(r.clk) == m_aig.resolve(dsp.clk)
                         && m_aig.isPlaceholder(l >> 1) && !m_aig.hasAlias(l >> 1);
                    if (ok && i) {
                        const Design::Reg& r0 = m_d.regs[bits[0].first];
                        ok = m_aig.resolve(r.en) == m_aig.resolve(r0.en)
                             && m_aig.resolve(r.rst) == m_aig.resolve(r0.rst);
                    }
                    if (ok) bits.push_back(it->second);
                }
                if (!ok) continue;
                // the product register and the chain's outputs are read nowhere else
                std::unordered_set<int> guarded;
                for (int k = 0; k < pw; ++k) guarded.insert(dsp.p[k] >> 1);
                for (const int l : ch.o) guarded.insert(l >> 1);
                if (ch.co >= 0) guarded.insert(ch.co >> 1);
                std::unordered_set<int> seen;
                std::vector<int> stack;
                auto sink = [&](int lit) { stack.push_back(m_aig.resolve(lit) >> 1); };
                for (const auto& kv : m_d.outputs)
                    for (const int l : kv.second) sink(l);
                std::set<std::pair<size_t, size_t>> accBits(bits.begin(), bits.end());
                for (size_t ri = 0; ri < m_d.regs.size(); ++ri) {
                    const Design::Reg& r = m_d.regs[ri];
                    for (size_t b = 0; b < r.d.size(); ++b)
                        if (!accBits.count({ri, b})) sink(r.d[b]);
                    sink(r.en);
                    sink(r.rst);
                    sink(r.clk);
                }
                for (const Design::Cell& c : m_d.cells2)
                    for (const auto& pin : c.ins)
                        for (const int l : pin.second) sink(l);
                for (size_t cj = 0; cj < m_aig.m_chains.size(); ++cj) {
                    if (cj == ci) continue;
                    for (const int l : m_aig.m_chains[cj].s) sink(l);
                    for (const int l : m_aig.m_chains[cj].di) sink(l);
                    sink(m_aig.m_chains[cj].cyinit);
                }
                for (const AigB::Dsp& dd : m_aig.m_dsps) {
                    for (const int l : dd.a) sink(l);
                    for (const int l : dd.b) sink(l);
                    for (const int l : {dd.clk, dd.cep, dd.rstp, dd.cem, dd.rstm, dd.cea1, dd.cea2,
                                        dd.rsta, dd.ceb1, dd.ceb2, dd.rstb})
                        sink(l);
                }
                bool clean = true;
                while (!stack.empty() && clean) {
                    const int v = stack.back();
                    stack.pop_back();
                    if (!v || !seen.insert(v).second) continue;
                    if (guarded.count(v)) {
                        clean = false;
                        break;
                    }
                    if (m_aig.isPlaceholder(v)) continue;
                    stack.push_back(m_aig.resolve(m_aig.fanin(v).first) >> 1);
                    stack.push_back(m_aig.resolve(m_aig.fanin(v).second) >> 1);
                }
                if (!clean) continue;
                // take it
                const Design::Reg& r0 = m_d.regs[bits[0].first];
                dsp.mreg = true;
                dsp.cem = dsp.cep;
                dsp.rstm = dsp.rstp;
                dsp.acc = true;
                dsp.cep = r0.en;
                dsp.rstp = r0.rst;
                std::map<size_t, std::vector<bool>> drop;
                for (size_t i = 0; i < 48; ++i) {
                    m_aig.m_pinned.erase(dsp.p[i] >> 1);
                    if (i < w) {
                        Design::Reg& r = m_d.regs[bits[i].first];
                        dsp.p[i] = r.q[bits[i].second];
                        auto& dv = drop[bits[i].first];
                        if (dv.empty()) dv.assign(r.q.size(), false);
                        dv[bits[i].second] = true;
                        ++absorbed;
                    } else {
                        dsp.p[i] = m_aig.pinnedVar();
                    }
                    m_aig.m_pinned.insert(dsp.p[i] >> 1);
                }
                dsp.pw = w;
                for (size_t i = 0; i < w; ++i)
                    m_aig.aliasTo(ch.o[i], dsp.p[i]);  // the sums were the accumulator's D: now P
                if (ch.co >= 0) m_aig.aliasTo(ch.co, 0);
                m_aig.m_chains.erase(m_aig.m_chains.begin() + ci);
                for (auto& kv : drop) {
                    std::vector<bool> keep(kv.second.size());
                    for (size_t b = 0; b < keep.size(); ++b) keep[b] = !kv.second[b];
                    keepRegBits(m_d.regs[kv.first], keep);
                }
                dropEmptyRegs();
                break;
            }
        }
        return absorbed;
    }
    // Input registers: the flip-flops whose Q bits are exactly a DSP48E1's A
    // (or B) operand and are read nowhere else go into the DSP's A2 (B2)
    // register, AREG = 1: the operand becomes their D, CEA2 / RSTA their
    // clock enable and synchronous reset.  A further such stage behind them
    // is A1 (AREG = 2, CEA1); the DSP has one RSTA for both, so the stages
    // must share the reset.  Same conditions as the P register otherwise:
    // one clock edge for the whole DSP, no asynchronous reset, resetting and
    // powering up to 0.  A constant-0 operand bit stays 0 through a register
    // that resets and powers up to 0.  Returns the flip-flop bits absorbed.
    int absorbDspInputRegs() {
        if (m_aig.m_dsps.empty()) return 0;
        int absorbed = 0;
        for (int stage = 0; stage < 2; ++stage) {
            std::unordered_map<int, std::pair<size_t, size_t>> qOf;  // Q var -> (reg, bit)
            for (size_t ri = 0; ri < m_d.regs.size(); ++ri)
                for (size_t b = 0; b < m_d.regs[ri].q.size(); ++b)
                    qOf[m_d.regs[ri].q[b] >> 1] = {ri, b};
            // a Q var's readers: DSP operands reading it directly, and
            // everything reached from any other sink
            std::unordered_map<int, std::set<size_t>>
                direct;  // Q var -> dsp * 2 + (0 for A, 1 for B)
            std::unordered_set<int> seen;
            std::vector<int> stack;
            auto sink = [&](int lit) { stack.push_back(m_aig.resolve(lit) >> 1); };
            for (const auto& kv : m_d.outputs)
                for (const int l : kv.second) sink(l);
            for (const Design::Reg& r : m_d.regs) {
                for (const int l : r.d) sink(l);
                sink(r.en);
                sink(r.rst);
                sink(r.clk);
            }
            for (const Design::Cell& c : m_d.cells2)
                for (const auto& pin : c.ins)
                    for (const int l : pin.second) sink(l);
            for (const AigB::Chain& ch : m_aig.m_chains) {
                for (const int l : ch.s) sink(l);
                for (const int l : ch.di) sink(l);
                sink(ch.cyinit);
            }
            for (size_t di = 0; di < m_aig.m_dsps.size(); ++di) {
                const AigB::Dsp& d = m_aig.m_dsps[di];
                for (int op = 0; op < 2; ++op) {
                    for (const int l0 : op ? d.b : d.a) {
                        const int l = m_aig.resolve(l0);
                        if (!(l & 1) && qOf.count(l >> 1))
                            direct[l >> 1].insert(di * 2 + op);
                        else
                            sink(l);
                    }
                }
                for (const int l : {d.clk, d.cep, d.rstp, d.cem, d.rstm, d.cea1, d.cea2, d.rsta,
                                    d.ceb1, d.ceb2, d.rstb})
                    sink(l);
            }
            while (!stack.empty()) {
                const int v = stack.back();
                stack.pop_back();
                if (!v || !seen.insert(v).second) continue;
                if (m_aig.isPlaceholder(v)) continue;
                stack.push_back(m_aig.resolve(m_aig.fanin(v).first) >> 1);
                stack.push_back(m_aig.resolve(m_aig.fanin(v).second) >> 1);
            }
            std::map<size_t, std::vector<bool>> drop;  // reg -> bits taken out
            for (size_t di = 0; di < m_aig.m_dsps.size(); ++di) {
                AigB::Dsp& d = m_aig.m_dsps[di];
                for (int op = 0; op < 2; ++op) {
                    int& nreg = op ? d.breg : d.areg;
                    if (nreg != stage) continue;
                    Vec& v = op ? d.b : d.a;
                    std::vector<std::pair<size_t, size_t>>
                        bits;  // per operand bit; SIZE_MAX = constant 0
                    bool ok = true, have0 = false;
                    size_t r0i = 0;
                    for (const int l0 : v) {
                        const int l = m_aig.resolve(l0);
                        if (l == 0) {
                            bits.push_back({SIZE_MAX, 0});
                            continue;
                        }
                        const auto it = (l & 1) ? qOf.end() : qOf.find(l >> 1);
                        if (it == qOf.end() || seen.count(l >> 1)) {
                            ok = false;
                            break;
                        }
                        const auto du = direct.find(l >> 1);
                        if (du == direct.end() || du->second.size() != 1
                            || *du->second.begin() != di * 2 + op) {
                            ok = false;
                            break;
                        }
                        const Design::Reg& r = m_d.regs[it->second.first];
                        const size_t bn = r.bitNo.empty()
                                              ? it->second.second
                                              : static_cast<size_t>(r.bitNo[it->second.second]);
                        if (r.latch || r.negedge || (r.async && m_aig.resolve(r.rst) != 0)
                            || bitOf(r.resetVal, bn) || bitOf(r.initVal, bn)) {
                            ok = false;
                            break;
                        }
                        if (!have0) {
                            r0i = it->second.first;
                            have0 = true;
                        }
                        const Design::Reg& r0 = m_d.regs[r0i];
                        if (m_aig.resolve(r.clk) != m_aig.resolve(r0.clk)
                            || m_aig.resolve(r.en) != m_aig.resolve(r0.en)
                            || m_aig.resolve(r.rst) != m_aig.resolve(r0.rst)) {
                            ok = false;
                            break;
                        }
                        bits.push_back(it->second);
                    }
                    if (!ok || !have0) continue;
                    const Design::Reg& r0 = m_d.regs[r0i];
                    const bool clocked = d.preg || d.mreg || d.areg || d.breg;
                    if (clocked && m_aig.resolve(r0.clk) != m_aig.resolve(d.clk)) continue;
                    if (stage == 1 && m_aig.resolve(r0.rst) != m_aig.resolve(op ? d.rstb : d.rsta))
                        continue;
                    std::set<std::pair<size_t, size_t>> taken;
                    for (size_t k = 0; k < v.size(); ++k) {
                        if (bits[k].first == SIZE_MAX) continue;
                        const Design::Reg& r = m_d.regs[bits[k].first];
                        v[k] = r.d[bits[k].second];
                        auto& dv = drop[bits[k].first];
                        if (dv.empty()) dv.assign(r.q.size(), false);
                        if (!dv[bits[k].second]) ++absorbed;
                        dv[bits[k].second] = true;
                    }
                    d.clk = r0.clk;
                    if (stage == 0) {
                        (op ? d.ceb2 : d.cea2) = r0.en;
                        (op ? d.rstb : d.rsta) = r0.rst;
                    } else {
                        (op ? d.ceb1 : d.cea1) = r0.en;
                    }
                    nreg = stage + 1;
                }
            }
            if (drop.empty()) break;
            for (auto& kv : drop) {
                std::vector<bool> keep(kv.second.size());
                for (size_t b = 0; b < keep.size(); ++b) keep[b] = !kv.second[b];
                keepRegBits(m_d.regs[kv.first], keep);
            }
            dropEmptyRegs();
        }
        return absorbed;
    }

    // Every literal the design reads apart from the hard cells' own inputs: the
    // top outputs, the register inputs and the kept cells' input pins (as buildPOs)
    Vec designReads() const {
        Vec r;
        for (const auto& kv : m_d.outputs) r.insert(r.end(), kv.second.begin(), kv.second.end());
        for (const Design::Reg& g : m_d.regs) {
            r.insert(r.end(), g.d.begin(), g.d.end());
            r.push_back(g.en);
            r.push_back(g.rst);
        }
        for (const Design::Cell& c : m_d.cells2) {
            for (const auto& pin : c.ins) r.insert(r.end(), pin.second.begin(), pin.second.end());
        }
        return r;
    }
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
            if (d.preg) {
                dp.cep = addPO(d.cep);
                dp.rstp = addPO(d.rstp);
            }
            if (d.mreg) {
                dp.cem = addPO(d.cem);
                dp.rstm = addPO(d.rstm);
            }
            if (d.areg) {
                dp.cea2 = addPO(d.cea2);
                dp.rsta = addPO(d.rsta);
                if (d.areg > 1) dp.cea1 = addPO(d.cea1);
            }
            if (d.breg) {
                dp.ceb2 = addPO(d.ceb2);
                dp.rstb = addPO(d.rstb);
                if (d.breg > 1) dp.ceb1 = addPO(d.ceb1);
            }
            m_dspPo.push_back(dp);
        }
    }
    struct ChainPo final {
        int s, di, cyinit;
    };
    struct DspPo final {
        int a, b, cep = -1, rstp = -1, cem = -1, rstm = -1, cea1 = -1, cea2 = -1, rsta = -1,
                  ceb1 = -1, ceb2 = -1, rstb = -1;
    };
    std::vector<ChainPo> m_chainPo;
    std::vector<DspPo> m_dspPo;

    // ---- AIGER (binary) ----
    // Topological order of the AND nodes reachable from the POs; every
    // placeholder met on the way is registered as a primary input
    std::vector<int> topoOrder() {
        std::vector<int> order;
        std::unordered_map<int, int> mark;  // var -> 1 visiting, 2 done
        bool looped = false;
        std::function<void(int)> dfs = [&](int lit) {
            const int v = m_aig.resolve(lit) >> 1;
            if (!v) return;
            const auto it = mark.find(v);
            if (it != mark.end()) {
                if (it->second == 1) looped = true;
                return;
            }
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
        // ABC's reader takes such a file for out-of-range literals and asserts
        if (looped)
            v3fatal("Synthesis: the design has a combinational loop, which cannot be mapped");
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
        os << "aig " << (I + order.size()) << " " << I << " 0 " << m_po.size() << " "
           << order.size() << "\n";
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
        UINFO(1, "map: AIG " << I << " inputs, " << m_po.size() << " outputs, " << order.size()
                             << " ANDs");
    }

    void runAbc(const std::string& aig, const std::string& blif, bool timed,
                const std::vector<double>* ciArrival, const std::vector<double>* coRequired) {
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
        // Script: dch computes structural choices for the mapper, mfs2
        // simplifies the mapped LUTs with their don't-cares, and mapping
        // again from that simplified logic takes most of the gain without
        // the inverters mfs2 leaves at outputs; -F/-A/-C give the final
        // mapping's area recovery more passes and priority cuts.  7-8% fewer
        // LUTs than a bare "if" on picorv32 and picosoc (1285 and 3876
        // without wide LUTs), in 2-5 s.  (lutpack is not used: it leaves an
        // inverter per repacked output.)
        static const char* const kScript
            = "strash; dc2; dch -f; if; mfs2; strash; dc2; dch -f; if -F 4 -A 8 -C 16";
        // LUT library: unit cost up to 6 inputs, and unless
        // --map-only-no-wide-lut, 7- and 8-input functions costing two and
        // four LUT6s (under a MUXF7, or two MUXF7s and a MUXF8), all at one
        // unit of delay.  abc maps for depth first, so it takes wide
        // functions where they remove a LUT level and recovers area
        // elsewhere, as yosys's synth_xilinx (-luts ...,10,20) and Vivado
        // do; with the script above that costs 3-4% LUTs (picorv32 1334,
        // picosoc 3972) for one or two levels less.  The wide covers become
        // LUT6 + MUXF7 / MUXF8 trees
        // when the cells are emitted.
        const int kMax = v3Global.opt.mapOnlyNoWideLut() ? 6 : 8;
        // With timing, the delays are in ns (a LUT level with an average net,
        // a MUXF7/8 more per input past six) and, from the second pass on, the
        // network carries the arrival and required times the analysis of the
        // previous mapping found at its boundary: abc's `if` then maps for the
        // real slack, pulling late sources (block RAM data) towards the
        // registers and relaxing paths with time to spare.
        const std::string lib = aig + ".lut";
        {
            std::ofstream os{lib};
            os.setf(std::ios::fixed);
            os.precision(3);
            for (int k = 1; k <= kMax; ++k)
                os << k << " "
                   << (k <= 6   ? 1
                       : k == 7 ? 2
                                : 4)
                   << ".00 " << (timed ? V3MapTiming::lutDelay(k, m_lutNetDelay) : 1.0) << "\n";
        }
        const std::string pre = "read_lut " + lib + "; ";
        if (script.empty()) script = kScript;
        std::string cmd = "read_aiger " + aig + "; " + pre + script + "; write_blif " + blif;
        if (ciArrival && coRequired) {
            const std::string read = "read_aiger " + aig;
            UINFO(1, "map: abc: " << read);
            if (ABC_NAMESPACE::Cmd_CommandExecute(pAbc, read.c_str()))
                v3fatalSrc("abc failed on: " << read);
            std::string err;
            double required = 0;
            for (const double r : *coRequired)
                if (!std::isnan(r)) required = std::max(required, r);
            if (!V3MapAbc::setTimes(*ciArrival, *coRequired, required > 0 ? required : 1e9, err))
                v3fatal("Synthesis: " << err);
            cmd = pre + script + "; write_blif " + blif;
        }
        UINFO(1, "map: abc: " << cmd);
        const int rc = ABC_NAMESPACE::Cmd_CommandExecute(pAbc, cmd.c_str());
        if (!v3Global.opt.mapOnlyKeepTemps()) std::remove(lib.c_str());
        if (rc) v3fatalSrc("abc failed on: " << cmd);
    }

    // one abc LUT: inputs, output, and the on-set cover (in-pattern, value)
    struct Lut final {
        std::vector<std::string> ins;
        std::string out;
        std::vector<std::pair<std::string, char>> cover;
    };
    static bool isBufLutImpl(const Lut& l) {
        return l.ins.size() == 1 && l.cover.size() == 1 && l.cover[0].first == "1"
               && l.cover[0].second == '1';
    }

    // ---- partitioned mapping: abc once per instance ----
    // The flat AIG is cut at instance boundaries: every node belongs to the
    // scope that built it, and a scope below --map-only-partition-min AND
    // nodes is folded into its parent's partition so LUTs can still span
    // small modules.  Each partition is handed to abc on its own, with its
    // boundary nodes named v<var> (global AIG variables) so the partitions'
    // LUT netlists join up by name; abc's internal names get a partition
    // prefix.  Top outputs, register and cell inputs keep their po<i> names.
    // With timing the LUT library's delays are in ns (timed), and a pass after
    // the first gives abc the arrival and required times at each partition's
    // boundary that the analysis of the previous mapping found (times).
    void mapPartitioned(const std::string& base, std::map<std::string, Lut>& luts,
                        std::vector<std::string>& order, bool timed, const BoundaryTimes* times) {
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
                std::cerr << "%Info: partition: " << big[i].second << " x"
                          << byType[big[i].second].first << " ands=" << big[i].first << "\n";
            }
        }
        struct Part final {
            std::vector<int> ins, ands, outs;
            std::unordered_set<int> inSet, outSet;
            std::vector<std::pair<int, std::string>> pos;  // (literal, po<i>)
        };
        std::map<int, Part> parts;
        // A partition input's name: a source (register or cell output, input pad) is
        // pi<i> as when mapped whole, which the emission's passes look it up by; a
        // node of another partition is v<var>
        const auto inName = [&](int v) {
            return m_aig.isPlaceholder(v) ? "pi" + std::to_string(m_piOf.at(v))
                                          : "v" + std::to_string(v);
        };
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
            if (!v || m_aig.isPlaceholder(v))
                continue;  // constant / input: passed through by emitJson
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
                for (size_t i = 0; i < P.ins.size(); ++i)
                    os << "i" << i << " " << inName(P.ins[i]) << "\n";
                for (size_t i = 0; i < outs.size(); ++i)
                    os << "o" << i << " " << outs[i].second << "\n";
                os << "c\nverilator --map-only-output " << tag << "\n";
            }
            const auto t0 = std::chrono::steady_clock::now();
            // Timing-driven pass: the arrival times at this partition's inputs
            // and the required times at its outputs, as the analysis of the
            // previous mapping found them
            std::vector<double> ci, co;
            if (times) {
                const double nan = std::numeric_limits<double>::quiet_NaN();
                ci.assign(P.ins.size(), nan);
                for (size_t i = 0; i < P.ins.size(); ++i) {
                    const int v = P.ins[i];
                    if (m_aig.isPlaceholder(v)) {
                        const auto it = m_piOf.find(v);
                        if (it != m_piOf.end()
                            && static_cast<size_t>(it->second) < times->ciArrival.size())
                            ci[i] = times->ciArrival[it->second];
                    } else {
                        const auto it = times->cutArrival.find("v" + std::to_string(v));
                        if (it != times->cutArrival.end()) ci[i] = it->second;
                    }
                }
                co.assign(outs.size(), nan);
                for (size_t i = 0; i < outs.size(); ++i) {
                    const std::string& n = outs[i].second;
                    if (n[0] == 'v') {
                        const auto it = times->cutRequired.find(n);
                        if (it != times->cutRequired.end()) co[i] = it->second;
                    } else {
                        const size_t k = std::strtoul(n.c_str() + 2, nullptr, 10);
                        if (k < times->coRequired.size()) co[i] = times->coRequired[k];
                    }
                }
            }
            runAbc(aig, blif, timed, times ? &ci : nullptr, times ? &co : nullptr);
            std::map<std::string, Lut> pl;
            std::vector<std::string> po;
            readBlif(blif, pl, po);
            if (!v3Global.opt.mapOnlyKeepTemps()) {
                std::remove(aig.c_str());
                std::remove(blif.c_str());
            }
            {
                const auto& sc = m_scopes[kv.first];
                const double secs
                    = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::cerr << "%Info: partition: " << tag << " " << sc.second << " "
                          << (sc.first.empty() ? std::string("<top>") : sc.first)
                          << " ands=" << P.ands.size() << " ins=" << P.ins.size()
                          << " outs=" << outs.size() << " luts=" << po.size()
                          << " abc=" << std::fixed << std::setprecision(1) << secs << "s"
                          << std::endl;
            }
            std::unordered_set<std::string> boundary;
            for (const int v : P.ins) boundary.insert(inName(v));
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
    void readBlif(const std::string& path, std::map<std::string, Lut>& luts,
                  std::vector<std::string>& order) {
        std::ifstream in{path};
        if (!in) v3fatalSrc("abc produced no BLIF: " << path);
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
    // A 3-input LUT that is `s ? d1 ^ p1 : d0 ^ p0` for some order of its
    // inputs and data polarities (abc folds inverters into it):
    // {s, d0, d1, p0, p1} as input indices and polarities, or s = -1
    static std::array<int, 5> lutMux(const Lut& l) {
        if (l.ins.size() != 3) return {-1, -1, -1, 0, 0};
        const std::string init = lutInit(l);  // bit idx at position 7 - idx
        for (int sel = 0; sel < 3; ++sel) {
            for (int d0 = 0; d0 < 3; ++d0) {
                if (d0 == sel) continue;
                const int d1 = 3 - sel - d0;
                for (int pol = 0; pol < 4; ++pol) {
                    const int p0 = pol & 1, p1 = pol >> 1;
                    bool isMux = true;
                    for (int idx = 0; idx < 8 && isMux; ++idx) {
                        const int want
                            = ((idx >> sel) & 1) ? ((idx >> d1) & 1) ^ p1 : ((idx >> d0) & 1) ^ p0;
                        isMux = (init[7 - idx] == '1') == (want == 1);
                    }
                    if (isMux) return {sel, d0, d1, p0, p1};
                }
            }
        }
        return {-1, -1, -1, 0, 0};
    }
    static std::string lutInit(const Lut& l) {
        const int k = l.ins.size();
        const int n = 1 << k;
        std::string init(n, '0');
        if (k == 0) {
            bool one = false;
            for (const auto& c : l.cover)
                if (c.second == '1') one = true;
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
            // BLIF: the listed cubes give the output value (normally all '1'); unlisted -> other
            // value
            bool val;
            if (!l.cover.empty() && l.cover[0].second == '0')
                val = !off;  // off-set listed
            else
                val = on;
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
            cell("LUT1", "inv_" + std::to_string(m_ncells), {{"INIT", "\"01\""}},
                 {{"I0", "[" + it->second + "]"}}, {{"O", "[" + o + "]"}});
            return o;
        }
        return it->second;
    }
    static std::string binParam(long long v, int w = 32) {
        std::string s(w, '0');
        for (int i = 0; i < w; ++i)
            if ((v >> i) & 1) s[w - 1 - i] = '1';
        return "\"" + s + "\"";
    }
    // The netlist as records, for the EDIF writer (filled alongside the JSON)
    using Conn = V3MapTiming::Conn;
    std::vector<std::pair<std::string, std::string>>
        m_regQ;  // (Q net, register cell) for every flip-flop
    using CellRec = V3MapTiming::Cell;
    std::vector<CellRec> m_recs;
    bool m_record = false;  // keep the records: EDIF, Verilog or timing analysis wants them
    std::string m_curPath;  // path given to cells recorded now
    std::string m_curLeaf;  // leaf name for the next recorded cell
    std::map<std::pair<std::string, std::string>, std::string>
        m_netNames;  // (module path, net id) -> name
    // "core.sd.sdclk_div.SD_CLK_O" -> "SD_CLK_O"; "x[7:4]" -> ("x", 4)
    static std::string leafOf(const std::string& hname, int& lo) {
        lo = 0;
        std::string h = hname;
        const size_t br = h.find('[');
        if (br != std::string::npos) {
            int a = 0, b = 0;
            if (std::sscanf(h.c_str() + br + 1, "%d +: %d", &a, &b) == 2)
                lo = a;
            else if (std::sscanf(h.c_str() + br + 1, "%d:%d", &a, &b) == 2)
                lo = std::min(a, b);
            else if (std::sscanf(h.c_str() + br + 1, "%d", &a) == 1)
                lo = a;
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
            if (a[i] == '.') {
                lastDot = i;
                any = true;
            }
            ++i;
        }
        if ((i == a.size() && (i == b.size() || b[i] == '.'))) return a;
        if ((i == b.size() && a[i] == '.')) return b;
        return any ? a.substr(0, lastDot) : std::string();
    }
    using PortRec = V3MapTiming::Port;
    std::vector<PortRec> m_portRecs;
    // "[12, \"0\", 13]" -> {"12", "\"0\"", "13"}
    static std::vector<std::string> splitBits(const std::string& j) {
        std::vector<std::string> out;
        std::string cur;
        for (const char ch : j) {
            if (ch == '[' || ch == ']' || ch == ' ') continue;
            if (ch == ',') {
                out.push_back(cur);
                cur.clear();
                continue;
            }
            cur += ch;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
    void cell(const std::string& type, const std::string& name,
              const std::vector<std::pair<std::string, std::string>>& params,
              const std::vector<std::pair<std::string, std::string>>& ins,
              const std::vector<std::pair<std::string, std::string>>& outs,
              const std::vector<std::pair<std::string, std::string>>& attrs = {}) {
        if (m_record) {
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
                m_firstLoad.emplace(bits[b],
                                    name + "." + p.first
                                        + (bits.size() > 1 ? "[" + std::to_string(b) + "]" : ""));
            }
        }
        for (const auto& p : outs) {
            for (const std::string& n : splitBits(p.second))
                if (!n.empty() && n[0] != '"') m_drivenNets.insert(n);
        }
        // a register's output, for naming it if no signal does (see emitJson)
        if (type.rfind("FD", 0) == 0) {
            for (const auto& p : outs) {
                if (p.first != "Q") continue;
                const std::vector<std::string> bits = splitBits(p.second);
                if (bits.size() == 1 && !bits[0].empty() && bits[0][0] != '"')
                    m_regQ.emplace_back(bits[0], name);
            }
        }
        if (m_ncells) m_cells << ",\n";
        ++m_ncells;
        ++m_counts[type];
        m_cells << "    \"" << name << "\": {\"hide_name\": 0, \"type\": \"" << type
                << "\", \"parameters\": {";
        for (size_t i = 0; i < params.size(); ++i)
            m_cells << (i ? ", " : "") << "\"" << params[i].first << "\": " << params[i].second;
        m_cells << "}, \"attributes\": {";
        for (size_t i = 0; i < attrs.size(); ++i)
            m_cells << (i ? ", " : "") << "\"" << attrs[i].first << "\": " << attrs[i].second;
        m_cells << "}, \"port_directions\": {";
        bool first = true;
        for (const auto& p : ins) {
            m_cells << (first ? "" : ", ") << "\"" << p.first << "\": \"input\"";
            first = false;
        }
        for (const auto& p : outs) {
            m_cells << (first ? "" : ", ") << "\"" << p.first << "\": \"output\"";
            first = false;
        }
        m_cells << "}, \"connections\": {";
        first = true;
        for (const auto& p : ins) {
            m_cells << (first ? "" : ", ") << "\"" << p.first << "\": " << p.second;
            first = false;
        }
        for (const auto& p : outs) {
            m_cells << (first ? "" : ", ") << "\"" << p.first << "\": " << p.second;
            first = false;
        }
        m_cells << "}}";
    }
    static std::string bitsJson(const std::vector<std::string>& nets) {
        std::string s = "[";
        for (size_t i = 0; i < nets.size(); ++i) s += (i ? ", " : "") + nets[i];
        return s + "]";
    }

    // ---- register polarity ----
    // A register read only in complement -- its Q feeding carry-chain S / DI
    // pins, flip-flop enables and resets or output pins through inverters --
    // is stored inverted instead: its D cone, reset and power-up values are
    // complemented, the LUTs reading it absorb the inversion in their INIT,
    // and the inverter LUT1s go.  Done on abc's LUT netlist, where those
    // inverters are visible (abc cannot absorb an inverter between a PI and
    // a PO); a bit is flipped when that removes at least one LUT.
    std::unordered_set<int> m_flippedDpo;  // PO index of a D bit whose register is stored inverted
    std::unordered_set<int> m_flippedVar;  // its Q var: the net holds the signal's complement
    int m_flipGain = 0;
    int flipRegisterPolarity(std::map<std::string, Lut>& luts) {
        std::unordered_map<int, int> piOfVar;
        for (size_t i = 0; i < m_piVar.size(); ++i) piOfVar[m_piVar[i]] = i;
        std::unordered_set<int> direct;  // vars read as clocks: not through abc, not flippable
        for (const Design::Reg& r : m_d.regs) direct.insert(m_aig.resolve(r.clk) >> 1);
        for (const AigB::Dsp& d : m_aig.m_dsps) direct.insert(m_aig.resolve(d.clk) >> 1);
        std::unordered_map<std::string, std::vector<std::string>>
            readers;  // blif name -> LUTs reading it
        for (const auto& kv : luts)
            for (const std::string& i : kv.second.ins) readers[i].push_back(kv.first);
        const auto isBuf = [](const Lut& l) {
            return l.ins.size() == 1 && l.cover.size() == 1 && l.cover[0].first == "1"
                   && l.cover[0].second == '1';
        };
        const auto isInv = [](const Lut& l) {
            return l.ins.size() == 1 && l.cover.size() == 1
                   && ((l.cover[0].first == "0" && l.cover[0].second == '1')
                       || (l.cover[0].first == "1" && l.cover[0].second == '0'));
        };
        // A one-input cover listing its off-set ("0 0" buffer, "1 0"
        // inverter) is rewritten in on-set form, the form the alias and
        // inverter tests above recognise
        const auto canon = [](Lut& l) {
            if (l.ins.size() == 1 && l.cover.size() == 1 && l.cover[0].second == '0'
                && l.cover[0].first != "-")
                l.cover[0] = {l.cover[0].first == "0" ? "1" : "0", '1'};
        };
        const auto flipIn = [&canon](Lut& l, size_t p) {  // complement input p
            for (auto& c : l.cover) {
                char& ch = c.first[p];
                ch = ch == '0' ? '1' : ch == '1' ? '0' : ch;
            }
            canon(l);
        };
        const auto flipOut = [&canon](Lut& l) {  // complement the function
            if (l.cover.empty())
                l.cover.emplace_back(std::string(l.ins.size(), '-'), '1');  // constant 0 -> 1
            else
                for (auto& c : l.cover) c.second = c.second == '1' ? '0' : '1';
            canon(l);
        };
        int flipped = 0;
        for (int pass = 0; pass < 2;
             ++pass) {  // a second pass catches a D made a plain inverter by a flip
            for (size_t ri = 0; ri < m_d.regs.size(); ++ri) {
                const Design::Reg& r = m_d.regs[ri];
                if (r.latch) continue;
                const RegOut& ro = m_regPo[ri];
                for (size_t b = 0; b < r.q.size(); ++b) {
                    const int dpo = ro.d0 + b;
                    if (m_flippedDpo.count(dpo)) continue;
                    const int ql = m_aig.resolve(r.q[b]);
                    if (ql < 2 || (ql & 1)) continue;
                    const int v = ql >> 1;
                    if (direct.count(v)) continue;
                    const auto pit = piOfVar.find(v);
                    if (pit == piOfVar.end()) continue;
                    const std::string pi = "pi" + std::to_string(pit->second);
                    const std::string dpn = "po" + std::to_string(dpo);
                    const auto dit = luts.find(dpn);
                    if (dit == luts.end()) continue;
                    Lut& D = dit->second;
                    // The D cone's complement: a LUT of its own is free, a
                    // buffer of a node read only here complements that node
                    // (an inverter becomes a buffer: +1), a buffer of a shared
                    // node needs an inverter (-1); a (buffered) inverter of
                    // the register itself is a toggle and stays one.
                    const bool dUnary = isBuf(D) || isInv(D);
                    const std::string src = dUnary ? D.ins[0] : std::string();
                    if (src == pi) continue;
                    Lut* privp = nullptr;  // src LUT read only by D
                    if (isBuf(D)) {
                        const auto sit = luts.find(src);
                        if (sit != luts.end() && !sit->second.ins.empty()
                            && readers[src].size() == 1)
                            privp = &sit->second;
                        if (privp && privp->ins.size() == 1 && privp->ins[0] == pi)
                            continue;  // toggle
                    }
                    int gain = 0;
                    const auto rit = readers.find(pi);
                    if (rit != readers.end()) {
                        for (const std::string& n : rit->second) {
                            const Lut& l = luts[n];
                            if (isInv(l))
                                ++gain;  // becomes a buffer
                            else if (isBuf(l))
                                --gain;  // becomes an inverter
                        }
                    }
                    if (isInv(D))
                        ++gain;
                    else if (isBuf(D)) {
                        if (!privp)
                            --gain;
                        else if (isInv(*privp))
                            ++gain;
                    }
                    // abc names a node by one of the outputs it drives, so
                    // other LUTs may read the D node itself: they absorb its
                    // complement at that input
                    const auto rdD = readers.find(dpn);
                    if (rdD != readers.end()) {
                        for (const std::string& n : rdD->second) {
                            const Lut& l = luts[n];
                            if (isInv(l))
                                ++gain;
                            else if (isBuf(l))
                                --gain;
                        }
                    }
                    if (gain < 1) continue;
                    // apply: readers first, then the D cone (both may touch one LUT)
                    if (rit != readers.end()) {
                        for (const std::string& n : rit->second) {
                            Lut& l = luts[n];
                            for (size_t p = 0; p < l.ins.size(); ++p)
                                if (l.ins[p] == pi) flipIn(l, p);
                        }
                    }
                    if (privp)
                        flipOut(*privp);
                    else
                        flipOut(D);
                    if (rdD != readers.end()) {
                        for (const std::string& n : rdD->second) {
                            Lut& l = luts[n];
                            for (size_t p = 0; p < l.ins.size(); ++p)
                                if (l.ins[p] == dpn) flipIn(l, p);
                        }
                    }
                    for (int& po : m_po)
                        if ((po >> 1) == v)
                            po ^= 1;  // the fallback path, when a PO is not in the BLIF
                    m_flippedDpo.insert(dpo);
                    m_flippedVar.insert(v);
                    m_flipGain += gain;
                    ++flipped;
                }
            }
        }
        return flipped;
    }

    void emitJson(const std::string& filename, std::map<std::string, Lut>& luts,
                  std::vector<std::string>& order, bool write) {
        {
            const int n = flipRegisterPolarity(luts);
            if (n && v3Global.opt.structOnlyReport())
                std::cerr << "%Info: map-regs: " << n << " register bits stored inverted, "
                          << m_flipGain << " inverter LUTs removed\n";
        }
        limitFanout(luts, order);
        // 1. nets for PIs (sources) and for every LUT output; resolve BLIF names
        std::unordered_map<std::string, std::string> netOfName;  // blif name -> net json
        for (size_t i = 0; i < m_piVar.size(); ++i) {
            const int v = m_piVar[i];
            auto it = m_netOfVar.find(v);
            if (it == m_netOfVar.end()) it = m_netOfVar.emplace(v, newNet()).first;
            netOfName["pi" + std::to_string(i)] = it->second;
        }
        for (const auto& kv : m_regDup)
            for (const std::string& cp : kv.second) netOfName[cp] = newNet();
        // The net a register pin reads a PO from: the node's, or one of its
        // fanout copies' (limitFanout) once the first group is full
        const size_t F = std::max<size_t>(1, v3Global.opt.mapOnlyMaxFanout());
        const auto poNetFor = [&](int i) -> std::string {
            const std::string& base = m_poNet[i];
            if (base.empty() || base[0] == '"' || m_spread.empty()) return base;
            const auto it = m_spread.find(poNode(luts, i));
            if (it == m_spread.end()) return base;
            Spread& sp = it->second;
            const size_t g = sp.readerGroup + (sp.readerFill + sp.outsideNext++) / F;
            if (g == 0) return base;
            const auto nit = netOfName.find(sp.names[std::min(g, sp.names.size() - 1)]);
            return nit == netOfName.end() ? base : nit->second;
        };
        auto netOf = [&](const std::string& n) -> std::string {
            const auto it = netOfName.find(n);
            if (it == netOfName.end())
                v3fatal("internal: abc BLIF name '" << n << "' has no driver");
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
            } else if (l.ins.size() == 1 && l.cover.size() == 1 && l.cover[0].first == "1"
                       && l.cover[0].second == '1') {
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
                if (it != netOfName.end())
                    netOfName[o] = it->second;
                else
                    left.push_back(o);
            }
            if (left.size() == aliases.size())
                v3fatal("internal: abc BLIF alias '" << left[0] << "' of undefined '"
                                                     << luts[left[0]].ins[0] << "'");
            aliases.swap(left);
        }
        m_cutNet.clear();
        for (const std::string& o : order) {
            if (o.size() > 1 && o[0] == 'v' && std::isdigit(static_cast<unsigned char>(o[1])))
                m_cutNet[o] = netOfName[o];
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
                if (r == 0)
                    m_poNet[i] = "\"0\"";
                else if (r == 1)
                    m_poNet[i] = "\"1\"";
                else
                    m_poNet[i] = netOfLit(r);  // PI (possibly inverted) passed straight through
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
                    for (size_t b = 0; b < m_d.cells2[ci].ins[pi].second.size(); ++b)
                        setPo(m_cellInPo[ci][pi] + b, p);
            }
            for (size_t oi = 0; oi < m_topOutPo.size(); ++oi) {
                auto it = m_d.outputs.begin();
                std::advance(it, oi);
                for (size_t b = 0; b < it->second.size(); ++b) setPo(m_topOutPo[oi] + b, "");
            }
            // readers of each BLIF name
            std::unordered_map<std::string, std::vector<std::string>> readers;
            for (const std::string& o : order)
                for (const std::string& in : luts[o].ins) readers[in].push_back(o);
            std::unordered_map<std::string, bool> known;
            for (const std::string& o : order) {
                if (o.rfind("po", 0) == 0) {
                    const size_t i = std::strtoul(o.c_str() + 2, nullptr, 10);
                    if (i < m_po.size() && poSet[i]) {
                        owner[o] = poOwner[i];
                        known[o] = true;
                    }
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
                        const std::string np
                            = known[o] ? commonPath(owner[o], owner[r]) : owner[r];
                        if (!known[o] || np != owner[o]) {
                            owner[o] = np;
                            known[o] = true;
                            changed = true;
                        }
                    }
                }
            }
        }
        std::set<std::string> complemented;  // LUTs whose INIT is inverted (1b)
        // 1b. Wide muxes: a 3-input LUT that muxes two LUTs read nowhere else
        // becomes a MUXF7 next to them in the slice, and one that muxes two
        // such MUXF7s a MUXF8 -- each saves a LUT and a LUT delay.
        {
            std::unordered_map<std::string, int> fanout;
            for (const std::string& o : order) {
                for (const std::string& i : luts[o].ins) ++fanout[i];
                if (o.compare(0, 2, "po") == 0) ++fanout[o];
            }
            // (a 7/8-input cover is already a MUXF7/8 tree: its output is not
            // a LUT6's, which is all a MUXF7 can take)
            const auto soleLut = [&](const std::string& n) {
                const auto it = luts.find(n);
                return it != luts.end() && !skip.count(n) && !it->second.ins.empty()
                       && it->second.ins.size() <= 6 && fanout[n] == 1;
            };
            std::set<std::string> f7;
            std::set<std::string> f7data;  // LUTs feeding a MUXF7: they stay LUTs
            std::map<std::string, std::pair<std::string, std::string>> f7feeds;
            // An inverted mux input is absorbed by complementing the LUT(s)
            // behind it, which read nothing else
            const auto invert = [&](const std::string& d) {
                const auto fit = f7feeds.find(d);
                if (fit == f7feeds.end()) {
                    complemented.insert(d).second || complemented.erase(d);
                } else {
                    for (const std::string& lut : {fit->second.first, fit->second.second}) {
                        complemented.insert(lut).second || complemented.erase(lut);
                    }
                }
            };
            for (const int level : {7, 8}) {
                for (const std::string& o : order) {
                    if (skip.count(o)) continue;
                    const std::array<int, 5> m = lutMux(luts[o]);
                    if (m[0] < 0) continue;
                    const std::string& d0 = luts[o].ins[m[1]];
                    const std::string& d1 = luts[o].ins[m[2]];
                    const bool ok = level == 7 ? soleLut(d0) && soleLut(d1) && !f7.count(d0)
                                                     && !f7.count(d1) && !f7data.count(o)
                                               : f7.count(d0) && f7.count(d1) && fanout[d0] == 1
                                                     && fanout[d1] == 1;
                    if (!ok) continue;
                    const auto oit = owner.find(o);
                    m_curPath = oit == owner.end() ? std::string() : oit->second;
                    cell(level == 7 ? "MUXF7" : "MUXF8",
                         "muxf" + std::to_string(level) + "_" + sanitize(o) + "_"
                             + std::to_string(m_ncells),
                         {},
                         {{"I0", "[" + netOf(d0) + "]"},
                          {"I1", "[" + netOf(d1) + "]"},
                          {"S", "[" + netOf(luts[o].ins[m[0]]) + "]"}},
                         {{"O", "[" + netOfName[o] + "]"}});
                    skip.insert(o);
                    if (m[3]) invert(d0);
                    if (m[4]) invert(d1);
                    if (level == 7) {
                        f7.insert(o);
                        f7data.insert(d0);
                        f7data.insert(d1);
                        f7feeds[o] = {d0, d1};
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
            for (int i = 0; i < std::min(k, 6); ++i)
                ins.emplace_back("I" + std::to_string(i), "[" + netOf(l.ins[i]) + "]");
            std::string init = lutInit(l);
            if (complemented.count(o)) {
                for (char& c : init) c = c == '1' ? '0' : '1';
            }
            if (k > 6) {
                // Wide cover from abc's LUT library: Shannon expansion on
                // input 6 (and 7).  Leaf q = {in7, in6} holds the 64 INIT
                // bits for that assignment; the leaves pair up under MUXF7s
                // selected by in6, and under a MUXF8 selected by in7.
                const int leaves = 1 << (k - 6);
                const std::string tag = sanitize(o);
                std::vector<std::string> leaf(leaves);
                for (int q = 0; q < leaves; ++q) {
                    leaf[q] = newNet();
                    cell("LUT6",
                         "lut_" + tag + "_" + std::to_string(q) + "_" + std::to_string(m_ncells),
                         {{"INIT", "\"" + init.substr(init.size() - (q + 1) * 64, 64) + "\""}},
                         ins, {{"O", "[" + leaf[q] + "]"}});
                }
                const std::string s6 = "[" + netOf(l.ins[6]) + "]";
                std::vector<std::string> f7(leaves / 2);
                for (int h = 0; h < leaves / 2; ++h) {
                    f7[h] = k == 7 ? netOfName[o] : newNet();
                    cell("MUXF7",
                         "muxf7_" + tag + "_" + std::to_string(h) + "_" + std::to_string(m_ncells),
                         {},
                         {{"I0", "[" + leaf[2 * h] + "]"},
                          {"I1", "[" + leaf[2 * h + 1] + "]"},
                          {"S", s6}},
                         {{"O", "[" + f7[h] + "]"}});
                }
                if (k == 8) {
                    cell("MUXF8", "muxf8_" + tag + "_" + std::to_string(m_ncells), {},
                         {{"I0", "[" + f7[0] + "]"},
                          {"I1", "[" + f7[1] + "]"},
                          {"S", "[" + netOf(l.ins[7]) + "]"}},
                         {{"O", "[" + netOfName[o] + "]"}});
                }
                continue;
            }
            cell("LUT" + std::to_string(k), "lut_" + sanitize(o) + "_" + std::to_string(m_ncells),
                 {{"INIT", "\"" + init + "\""}}, ins, {{"O", "[" + netOfName[o] + "]"}});
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
                    cov.push_back(blk + 1 == nb && k == 3 && ch.co >= 0 ? netOfLit(ch.co)
                                                                        : newNet());
                }
                cell("CARRY4", "carry" + std::to_string(ci) + "_" + std::to_string(blk), {},
                     {{"CI", "[" + carry + "]"},
                      {"CYINIT",
                       "[" + (blk == 0 ? m_poNet[cp.cyinit] : std::string("\"0\"")) + "]"},
                      {"DI", bitsJson(dv)},
                      {"S", bitsJson(sv)}},
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
            auto freshN = [&](size_t n) {
                std::vector<std::string> v;
                for (size_t k = 0; k < n; ++k) v.push_back(newNet());
                return v;
            };
            const std::string z = "[\"0\"]";
            std::vector<std::pair<std::string, std::string>> params
                = {{"A_INPUT", "\"DIRECT\""},
                   {"B_INPUT", "\"DIRECT\""},
                   {"USE_DPORT", "\"FALSE\""},
                   {"USE_MULT", "\"MULTIPLY\""},
                   {"USE_SIMD", "\"ONE48\""}};
            for (const char* r :
                 {"ACASCREG", "ADREG", "ALUMODEREG", "AREG", "BCASCREG", "BREG", "CARRYINREG",
                  "CARRYINSELREG", "CREG", "DREG", "INMODEREG", "MREG", "OPMODEREG", "PREG"}) {
                const std::string rn = r;
                const int n = rn == "PREG"                         ? (d.preg ? 1 : 0)
                              : rn == "MREG"                       ? (d.mreg ? 1 : 0)
                              : (rn == "AREG" || rn == "ACASCREG") ? d.areg
                              : (rn == "BREG" || rn == "BCASCREG") ? d.breg
                                                                   : 0;
                params.emplace_back(r, binParam(n));
            }
            const bool clocked = d.preg || d.mreg || d.areg || d.breg;
            std::vector<std::pair<std::string, std::string>> ins
                = {{"A", bitsJson(a)},
                   {"B", bitsJson(b)},
                   {"C", bitsJson(zeros(48))},
                   {"D", bitsJson(zeros(25))},
                   {"ACIN", bitsJson(zeros(30))},
                   {"BCIN", bitsJson(zeros(18))},
                   {"PCIN", bitsJson(zeros(48))},
                   {"ALUMODE", bitsJson(zeros(4))},
                   {"INMODE", bitsJson(zeros(5))},
                   {"CARRYINSEL", bitsJson(zeros(3))},
                   {"OPMODE", bitsJson({"\"1\"", "\"0\"", "\"1\"", "\"0\"", "\"0\"",
                                        d.acc ? "\"1\"" : "\"0\"",
                                        "\"0\""})},  // X = Y = M, Z = P when accumulating
                   {"CARRYIN", z},
                   {"CARRYCASCIN", z},
                   {"MULTSIGNIN", z},
                   {"CLK", clocked ? "[" + netOfLit(d.clk) + "]" : z}};
            auto po = [&](int idx) { return idx < 0 ? z : "[" + m_poNet[idx] + "]"; };
            for (const char* ce :
                 {"CEA1",    "CEA2",      "CEAD",          "CEALUMODE",  "CEB1",     "CEB2",
                  "CEC",     "CECARRYIN", "CECTRL",        "CED",        "CEINMODE", "CEM",
                  "CEP",     "RSTA",      "RSTALLCARRYIN", "RSTALUMODE", "RSTB",     "RSTC",
                  "RSTCTRL", "RSTD",      "RSTINMODE",     "RSTM",       "RSTP"}) {
                const std::string cn = ce;
                ins.emplace_back(ce, cn == "CEP"    ? po(dp.cep)
                                     : cn == "RSTP" ? po(dp.rstp)
                                     : cn == "CEM"  ? po(dp.cem)
                                     : cn == "RSTM" ? po(dp.rstm)
                                     : cn == "CEA1" ? po(dp.cea1)
                                     : cn == "CEA2" ? po(dp.cea2)
                                     : cn == "RSTA" ? po(dp.rsta)
                                     : cn == "CEB1" ? po(dp.ceb1)
                                     : cn == "CEB2" ? po(dp.ceb2)
                                     : cn == "RSTB" ? po(dp.rstb)
                                                    : z);
            }
            std::vector<std::pair<std::string, std::string>> outs
                = {{"P", bitsJson(p)},
                   {"ACOUT", bitsJson(freshN(30))},
                   {"BCOUT", bitsJson(freshN(18))},
                   {"PCOUT", bitsJson(freshN(48))},
                   {"CARRYCASCOUT", "[" + newNet() + "]"},
                   {"CARRYOUT", bitsJson(freshN(4))},
                   {"MULTSIGNOUT", "[" + newNet() + "]"},
                   {"OVERFLOW", "[" + newNet() + "]"},
                   {"UNDERFLOW", "[" + newNet() + "]"},
                   {"PATTERNDETECT", "[" + newNet() + "]"},
                   {"PATTERNBDETECT", "[" + newNet() + "]"}};
            cell("DSP48E1", "dsp" + std::to_string(di), params, ins, outs);
        }
        // 3. registers
        // A second driver of the same Q would come out as a second cell of
        // the same name: a wrong netlist, so stop instead.
        for (const std::string& n : m_d.multiDriven)
            v3error("Synthesis: more than one register drives '" << n << "'");
        for (size_t ri = 0; ri < m_d.regs.size(); ++ri) {
            const Design::Reg& r = m_d.regs[ri];
            m_curPath = pathOf(r.name);
            const RegOut& ro = m_regPo[ri];
            const std::string clk = netOfLit(r.clk);
            for (size_t b = 0; b < r.q.size(); ++b) {
                const std::string en = poNetFor(ro.en);
                const std::string rst = poNetFor(ro.rst);
                const std::string q = netOfLit(r.q[b]);
                const std::string d = poNetFor(ro.d0 + b);
                const size_t bn = r.bitNo.empty() ? b : static_cast<size_t>(r.bitNo[b]);
                const bool flip = m_flippedDpo.count(ro.d0 + b) != 0;  // stored inverted
                const bool setv = bitOf(r.resetVal, bn) != flip;
                const bool hasRst = rst != "\"0\"";
                std::string type;
                std::vector<std::pair<std::string, std::string>> ins;
                if (r.latch) {
                    // A transparent latch is a 7-series LDCE/LDPE: its enable
                    // is the gate G (GE held high), its reset the async clear
                    ins = {{"D", "[" + d + "]"}, {"G", "[" + en + "]"}, {"GE", "[\"1\"]"}};
                    type = hasRst && setv ? "LDPE" : "LDCE";
                    ins.emplace_back(type == "LDPE" ? "PRE" : "CLR",
                                     "[" + (hasRst ? rst : std::string{"\"0\""}) + "]");
                } else {
                    ins = {{"C", "[" + clk + "]"}, {"CE", "[" + en + "]"}, {"D", "[" + d + "]"}};
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
                }
                const bool initv = bitOf(r.initVal, bn) != flip;
                {
                    int lo = 0;
                    const std::string base = leafOf(r.name, lo);
                    const bool vec = r.q.size() > 1 || !r.bitNo.empty()
                                     || r.name.find('[') != std::string::npos;
                    m_curLeaf
                        = base + "_reg"
                          + (vec ? "[" + std::to_string(lo + static_cast<int>(bn)) + "]" : "");
                }
                std::vector<std::pair<std::string, std::string>> attrs;
                if (flip) attrs.emplace_back("INVERTED_Q", "\"1\"");
                {
                    const auto mf = m_mergedFrom.find(r.q[b]);
                    if (mf != m_mergedFrom.end()) {
                        std::string names;
                        for (const std::string& n : mf->second)
                            names += (names.empty() ? "" : ";") + n;
                        attrs.emplace_back("MERGED_EQUIV", "\"" + names + "\"");
                    }
                }
                cell(type, sanitize(r.name) + "_" + std::to_string(bn),
                     {{"INIT", initv ? "\"1\"" : "\"0\""}}, ins, {{"Q", "[" + q + "]"}}, attrs);
                // its copies for fanout (limitFanout): the same register again
                const auto dit = m_regDup.find({ri, b});
                if (dit != m_regDup.end()) {
                    for (size_t k = 0; k < dit->second.size(); ++k) {
                        m_curLeaf.clear();
                        cell(type,
                             sanitize(r.name) + "_" + std::to_string(bn) + "_dup"
                                 + std::to_string(k + 1),
                             {{"INIT", initv ? "\"1\"" : "\"0\""}}, ins,
                             {{"Q", "[" + netOfName.at(dit->second[k]) + "]"}}, attrs);
                    }
                }
            }
        }
        // 4. memories and pass-through cells
        std::vector<bool> laneDone(m_d.cells2.size(), false);
        for (size_t ci = 0; ci < m_d.cells2.size(); ++ci) {
            const Design::Cell& c = m_d.cells2[ci];
            m_curPath = pathOf(c.name);
            if (c.type.rfind("memory_", 0) == 0) {
                if (laneDone[ci]) continue;
                std::vector<std::pair<Design::Cell, MemPins>> groups;
                const std::vector<size_t> lanes = mergeLanes(ci, groups);
                if (!lanes.empty()) {
                    for (const size_t cj : lanes) laneDone[cj] = true;
                    for (const auto& g : groups) emitMemory(g.first, g.second);
                    continue;
                }
                emitMemory(c, m_cellInPo[ci]);
                continue;
            }
            std::vector<std::pair<std::string, std::string>> params, ins, outs;
            for (const auto& p : c.params) {
                const std::string& v = p.second;
                if (!v.empty() && v[0] == '"') {
                    params.emplace_back(p.first, v);  // string (or real) literal
                } else if (v.rfind("bits:", 0) == 0) {
                    params.emplace_back(p.first,
                                        "\"" + v.substr(5) + "\"");  // sized literal, its width
                } else {
                    bool num = !v.empty();
                    for (const char ch : v)
                        num &= std::isdigit(static_cast<unsigned char>(ch)) != 0;
                    params.emplace_back(p.first,
                                        num ? binParam(std::atoll(v.c_str())) : "\"" + v + "\"");
                }
            }
            for (size_t pi = 0; pi < c.ins.size(); ++pi) {
                std::vector<std::string> nets;
                for (size_t b = 0; b < c.ins[pi].second.size(); ++b)
                    nets.push_back(m_poNet[m_cellInPo[ci][pi] + b]);
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
        auto port = [&](const std::string& name, const char* dir,
                        const std::vector<std::string>& nets) {
            m_portRecs.push_back({name, dir, nets});
            if (std::string(dir) != "output")
                for (const std::string& n : nets) m_drivenNets.insert(n);
            // A port declared [5:1] has bit 0 at index 1: yosys writes that as
            // "offset", and so does this, or every reader that names bits by
            // index (an equivalence check against another synthesis, a pin
            // constraint on addr[1]) takes the port's bit 1 for its bit 0.
            std::string offset;
            {
                const auto rit = m_portRange.find(name);
                if (rit != m_portRange.end()) {
                    if (rit->second.first != 0)
                        offset += ", \"offset\": " + std::to_string(rit->second.first);
                    if (rit->second.second) offset += ", \"upto\": 1";
                }
            }
            ports << (first ? "" : ",\n") << "    \"" << name << "\": {\"direction\": \"" << dir
                  << "\"" << offset << ", \"bits\": " << bitsJson(nets) << "}";
            netnames << (first ? "" : ",\n") << "    \"" << name << "\": {\"hide_name\": 0"
                     << offset << ", \"bits\": " << bitsJson(nets) << ", \"attributes\": {}}";
            first = false;
        };
        // Every pad needs an IO buffer (yosys's iopadmap): a port bit that no
        // IBUF/OBUF/IOBUF/IBUFDS(_GTE2)/... touches gets one inserted here, the
        // pad on its outer side.  nextpnr then does with it what it does with
        // yosys's (GT serial pads included).
        //
        // --map-only-no-iobuf: the top is not the chip but a block mapped once
        // and instantiated (spliced into another netlist) many times -- its
        // ports are nets, not pads, and get no buffers.
        const bool noIobuf = v3Global.opt.mapOnlyNoIobuf();
        static const std::set<std::string> ioTypes
            = {"IBUF", "IBUFG", "IBUFDS", "IBUFGDS", "IBUFDS_GTE2", "IBUFDS_DIFF_OUT",
               "OBUF", "OBUFT", "OBUFDS", "OBUFTDS", "IOBUF",       "IOBUFDS"};
        m_curPath.clear();  // inserted IO buffers sit at the top
        std::set<int> bufferedVars;  // AIG vars (resolved) touching an IO buffer pin
        for (const Design::Cell& c : m_d.cells2) {
            if (!ioTypes.count(c.type)) continue;
            for (const auto& pin : c.ins)
                for (const int l : pin.second) bufferedVars.insert(m_aig.resolve(l) >> 1);
            for (const auto& pin : c.outs)
                for (const int l : pin.second) bufferedVars.insert(m_aig.resolve(l) >> 1);
        }
        // A clock input also gets a BUFG behind its IBUF (yosys's clkbufmap):
        // the clock pins of the registers, DSPs and memories it feeds are then
        // on the global clock tree, where nextpnr would otherwise route a pad
        // clock through the fabric (simpleuart: 147 MHz against 172).
        std::unordered_set<int> clockVars;  // AIG vars (resolved) read as clocks
        for (const Design::Reg& r : m_d.regs) clockVars.insert(m_aig.resolve(r.clk) >> 1);
        for (const AigB::Dsp& d : m_aig.m_dsps) clockVars.insert(m_aig.resolve(d.clk) >> 1);
        for (const Design::Cell& c : m_d.cells2) {
            if (c.type.rfind("memory_", 0) != 0) continue;
            for (const auto& pin : c.ins)
                if (pin.first == "clk")
                    for (const int l : pin.second) clockVars.insert(m_aig.resolve(l) >> 1);
        }
        clockVars.erase(0);  // the constant
        for (const auto& kv : m_d.inputs) {
            std::vector<std::string> nets;
            size_t b = 0;
            for (const int l : kv.second) {
                const std::string n = netOfLit(l);
                const int v = m_aig.resolve(l) >> 1;
                if (noIobuf || bufferedVars.count(v)) {
                    nets.push_back(n);
                } else if (clockVars.count(v)) {
                    const std::string pad = newNet(), ib = newNet();
                    cell("IBUF", "ibuf_" + sanitize(kv.first) + "_" + std::to_string(b), {},
                         {{"I", "[" + pad + "]"}}, {{"O", "[" + ib + "]"}});
                    cell("BUFG", "bufg_" + sanitize(kv.first) + "_" + std::to_string(b), {},
                         {{"I", "[" + ib + "]"}}, {{"O", "[" + n + "]"}});
                    nets.push_back(pad);
                } else {
                    const std::string pad = newNet();
                    cell("IBUF", "ibuf_" + sanitize(kv.first) + "_" + std::to_string(b), {},
                         {{"I", "[" + pad + "]"}}, {{"O", "[" + n + "]"}});
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
                if (noIobuf || inout || (n[0] != '"' && bufferedVars.count(r >> 1))) {
                    nets.push_back(n);
                } else {
                    const std::string pad = newNet();
                    cell("OBUF", "obuf_" + sanitize(kv.first) + "_" + std::to_string(b), {},
                         {{"I", "[" + n + "]"}}, {{"O", "[" + pad + "]"}});
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
                    if (it == m_netOfVar.end())
                        ok = false;
                    else
                        nets.push_back(it->second);
                    if (b < kv.second.drivers.size() && kv.second.drivers[b] == 0) undriven = true;
                } else {
                    ok = false;
                }
            }
            (void)undriven;
            if (!ok) continue;
            // A bit stored inverted (flipRegisterPolarity) is not the signal:
            // its net is named <signal>[i]_n, bit by bit, so nothing matching
            // nets by name (equivalence checks, probes) takes it for the signal
            std::vector<bool> inv(nets.size(), false);
            bool anyInv = false;
            for (size_t b = 0; b < nets.size(); ++b) {
                const int r = m_aig.resolve(kv.second.bits[b]);
                if (r > 1 && m_flippedVar.count(r >> 1)) inv[b] = anyInv = true;
            }
            const auto bitName = [&](const std::string& base, size_t b) {
                return (nets.size() == 1 && !anyInv
                            ? base
                            : base + "[" + std::to_string(b + kv.second.lsb) + "]")
                       + (inv[b] ? "_n" : "");
            };
            for (size_t b = 0; b < nets.size(); ++b) {
                if (!nets[b].empty() && nets[b][0] != '"')
                    m_netName.emplace(nets[b], bitName(kv.first, b));
            }
            if (!anyInv) {
                netnames << ",\n    \"" << kv.first << "\": {\"hide_name\": 0"
                         << (kv.second.lsb != 0 ? ", \"offset\": " + std::to_string(kv.second.lsb)
                                                : std::string())
                         << ", \"bits\": " << bitsJson(nets) << ", \"attributes\": {}}";
            } else {
                for (size_t b = 0; b < nets.size(); ++b) {
                    if (nets[b].empty() || nets[b][0] == '"') continue;
                    netnames << ",\n    \"" << bitName(kv.first, b)
                             << "\": {\"hide_name\": 0, \"bits\": [" << nets[b]
                             << "], \"attributes\": {}}";
                }
            }
            {
                int lo = 0;
                const std::string leaf = leafOf(kv.first, lo);
                const std::string path = pathOf(kv.first);
                for (size_t b = 0; b < nets.size(); ++b) {
                    if (nets[b].empty() || nets[b][0] == '"') continue;
                    m_netNames.emplace(std::make_pair(path, nets[b]), bitName(leaf, b));
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
            std::sort(floating.begin(), floating.end(),
                      [](const auto& a, const auto& b) { return a.second < b.second; });
            for (size_t i = 0; i < floating.size() && i < 20; ++i) {
                const auto nit = m_netName.find(floating[i].first);
                m_warnings.push_back(
                    "floating input " + floating[i].second + ": net "
                    + (nit != m_netName.end() ? nit->second : "#" + floating[i].first)
                    + " has no driver");
            }
            if (floating.size() > 20)
                m_warnings.push_back(std::to_string(floating.size() - 20)
                                     + " more floating inputs");
        }
        if (!write) return;  // a pass the timing analysis looks at, not the one written out
        if (!v3Global.opt.mapOnlyEdif().empty()) writeEdif(v3Global.opt.mapOnlyEdif());
        if (!v3Global.opt.mapOnlyVerilog().empty()) writeVerilog(v3Global.opt.mapOnlyVerilog());
        if (!v3Global.opt.mapOnlyMemInit().empty()) writeMemInit(v3Global.opt.mapOnlyMemInit());
        if (filename.empty()) return;  // --map-only-edif / --map-only-verilog alone
        std::ofstream os{filename};
        os << "{\n  \"creator\": \"verilator --map-only-output\",\n  \"modules\": {\n    \""
           << m_top << "\": {\n"
           << "      \"attributes\": {\"top\": \"00000000000000000000000000000001\"},\n"
           << "      \"ports\": {\n"
           << ports.str() << "\n      },\n"
           << "      \"cells\": {\n"
           << m_cells.str() << "\n      },\n"
           << "      \"netnames\": {\n"
           << netnames.str() << "\n      }\n    }\n  }\n}\n";
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
        for (const char c : n)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
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
    struct LibPort final {
        std::string name;
        bool out;
        size_t width;
    };
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
            in("C");
            in("CE");
            in("D");
            const std::string b = t.substr(0, 4);
            in(b == "FDRE" ? "R" : b == "FDSE" ? "S" : b == "FDCE" ? "CLR" : "PRE");
        } else if (t == "LDCE" || t == "LDPE") {
            out("Q");
            in("D");
            in("G");
            in("GE");
            in(t == "LDCE" ? "CLR" : "PRE");
        } else if (t == "RAMB36E1") {
            out("CASCADEOUTA");
            out("CASCADEOUTB");
            out("DBITERR");
            out("DOADO", 32);
            out("DOBDO", 32);
            out("DOPADOP", 4);
            out("DOPBDOP", 4);
            out("ECCPARITY", 8);
            out("RDADDRECC", 9);
            out("SBITERR");
            in("ADDRARDADDR", 16);
            in("ADDRBWRADDR", 16);
            in("CASCADEINA");
            in("CASCADEINB");
            in("CLKARDCLK");
            in("CLKBWRCLK");
            in("DIADI", 32);
            in("DIBDI", 32);
            in("DIPADIP", 4);
            in("DIPBDIP", 4);
            in("ENARDEN");
            in("ENBWREN");
            in("INJECTDBITERR");
            in("INJECTSBITERR");
            in("REGCEAREGCE");
            in("REGCEB");
            in("RSTRAMARSTRAM");
            in("RSTRAMB");
            in("RSTREGARSTREG");
            in("RSTREGB");
            in("WEA", 4);
            in("WEBWE", 8);
        } else if (t == "RAMB18E1") {
            out("DOADO", 16);
            out("DOBDO", 16);
            out("DOPADOP", 2);
            out("DOPBDOP", 2);
            in("ADDRARDADDR", 14);
            in("ADDRBWRADDR", 14);
            in("CLKARDCLK");
            in("CLKBWRCLK");
            in("DIADI", 16);
            in("DIBDI", 16);
            in("DIPADIP", 2);
            in("DIPBDIP", 2);
            in("ENARDEN");
            in("ENBWREN");
            in("REGCEAREGCE");
            in("REGCEB");
            in("RSTRAMARSTRAM");
            in("RSTRAMB");
            in("RSTREGARSTREG");
            in("RSTREGB");
            in("WEA", 2);
            in("WEBWE", 4);
        } else if (t == "RAM64X1D") {
            out("DPO");
            out("SPO");
            for (int i = 0; i < 6; ++i) in("A" + std::to_string(i));
            in("D");
            for (int i = 0; i < 6; ++i) in("DPRA" + std::to_string(i));
            in("WCLK");
            in("WE");
        } else if (t == "CARRY4") {
            out("CO", 4);
            out("O", 4);
            in("CI");
            in("CYINIT");
            in("DI", 4);
            in("S", 4);
        } else if (t == "MUXF7" || t == "MUXF8") {
            out("O");
            in("I0");
            in("I1");
            in("S");
        } else if (t == "DSP48E1") {
            out("ACOUT", 30);
            out("BCOUT", 18);
            out("CARRYCASCOUT");
            out("CARRYOUT", 4);
            out("MULTSIGNOUT");
            out("OVERFLOW");
            out("P", 48);
            out("PATTERNBDETECT");
            out("PATTERNDETECT");
            out("PCOUT", 48);
            out("UNDERFLOW");
            in("A", 30);
            in("ACIN", 30);
            in("ALUMODE", 4);
            in("B", 18);
            in("BCIN", 18);
            in("C", 48);
            in("CARRYCASCIN");
            in("CARRYIN");
            in("CARRYINSEL", 3);
            in("CEA1");
            in("CEA2");
            in("CEAD");
            in("CEALUMODE");
            in("CEB1");
            in("CEB2");
            in("CEC");
            in("CECARRYIN");
            in("CECTRL");
            in("CED");
            in("CEINMODE");
            in("CEM");
            in("CEP");
            in("CLK");
            in("D", 25);
            in("INMODE", 5);
            in("MULTSIGNIN");
            in("OPMODE", 7);
            in("PCIN", 48);
            in("RSTA");
            in("RSTALLCARRYIN");
            in("RSTALUMODE");
            in("RSTB");
            in("RSTC");
            in("RSTCTRL");
            in("RSTD");
            in("RSTINMODE");
            in("RSTM");
            in("RSTP");
        } else if (t == "IBUF" || t == "OBUF" || t == "BUFG") {
            out("O");
            in("I");
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
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '$')
                plain = false;
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
                const std::string here = pr.bits.size() == 1
                                             ? verilogId(pr.name)
                                             : verilogId(pr.name) + "[" + std::to_string(i) + "]";
                if (b.empty()) continue;
                if (b[0] == '"') {  // a constant output
                    assigns.push_back("  assign " + here + " = 1'b"
                                      + (b.size() > 1 ? b.substr(1, 1) : "x") + ";");
                    continue;
                }
                const auto it = ref.emplace(b, here);
                if (!it.second) {  // the same net on two ports: the later one follows the first
                    if (pr.dir == "input")
                        assigns.push_back("  assign " + it.first->second + " = " + here + ";");
                    else
                        assigns.push_back("  assign " + here + " = " + it.first->second + ";");
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
                    body << (i ? ", " : "") << "." << r.params[i].first << "("
                         << verilogParam(r.params[i].second) << ")";
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
                    for (size_t k = c.bits.size(); k-- > 0;)
                        body << bitRef(c.bits[k]) << (k ? ", " : "");
                    body << "}";
                }
                body << ")";
            }
            body << ");\n";
        }
        std::ofstream os{path};
        os << "// The mapped netlist of " << m_top << " (Verilator --map-only-verilog)\n";
        os << "module " << verilogId(m_top) << " (";
        for (size_t i = 0; i < m_portRecs.size(); ++i)
            os << (i ? ", " : "") << verilogId(m_portRecs[i].name);
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
                for (LibPort& lp : ports)
                    if (lp.name == c.port) {
                        have = true;
                        lp.width = std::max(lp.width, c.bits.size());
                    }
                if (!have) ports.push_back({c.port, c.out, c.bits.size()});
            }
        }
        // nets: id -> port references
        std::map<std::string, std::vector<std::string>> nets;
        auto ref = [&](const std::string& bit, const std::string& portRef) {
            std::string id = bit;
            if (bit == "\"0\"")
                id = "GND";
            else if (bit == "\"1\"")
                id = "VCC";
            else if (!bit.empty() && bit[0] == '"')
                return;  // x/z: unconnected
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
                for (const LibPort& lp : lib[r.type])
                    if (lp.name == c.port) width = lp.width;
                for (size_t b = 0; b < c.bits.size(); ++b) {
                    ref(c.bits[b], "(portRef " + member(c.port, width, b) + " (instanceRef "
                                       + instRefOf(i) + "))");
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
        os << "(edif " << edifName(m_top)
           << "\n  (edifVersion 2 0 0)\n  (edifLevel 0)\n  (keywordMap (keywordLevel 0))\n"
           << "  (comment \"verilator --map-only-edif\")\n";
        os << "  (external LIB\n    (edifLevel 0)\n    (technology (numberDefinition))\n";
        auto libCell = [&](const std::string& type, const std::vector<LibPort>& ports) {
            os << "    (cell " << edifName(type)
               << "\n      (cellType GENERIC)\n      (view VIEW_NETLIST\n        (viewType "
                  "NETLIST)\n        (interface\n";
            for (const LibPort& lp : ports) {
                const char* dir = lp.out ? "OUTPUT" : "INPUT";
                if (lp.width == 1)
                    os << "          (port " << edifName(lp.name) << " (direction " << dir
                       << "))\n";
                else
                    os << "          (port (array (rename " << lp.name << " \"" << lp.name << "["
                       << (lp.width - 1) << ":0]\") " << lp.width << ") (direction " << dir
                       << "))\n";
            }
            os << "        )\n      )\n    )\n";
        };
        if (nets.count("GND")) libCell("GND", {{"G", true, 1}});
        if (nets.count("VCC")) libCell("VCC", {{"P", true, 1}});
        for (const auto& kv : lib)
            if (kv.first != "GND" && kv.first != "VCC") libCell(kv.first, kv.second);
        os << "  )\n";
        os << "  (library DESIGN\n    (edifLevel 0)\n    (technology (numberDefinition))\n";
        if (v3Global.opt.mapOnlyEdifFlat()) {
            writeEdifFlatTop(os, nets, instNames);
        } else {
            writeEdifHier(os, lib, instNames);
        }
        os << "  )\n";
        os << "  (design " << edifName(m_top) << "\n    (cellRef " << edifName(m_top)
           << " (libraryRef DESIGN))\n  )\n)\n";
        UINFO(1, "map: wrote " << path << ": " << m_recs.size() << " instances, " << nets.size()
                               << " nets");
    }

    void writeEdifFlatTop(std::ofstream& os, std::map<std::string, std::vector<std::string>>& nets,
                          const std::vector<std::string>& instNames) {
        os << "    (cell " << edifName(m_top)
           << "\n      (cellType GENERIC)\n      (view VIEW_NETLIST\n        (viewType NETLIST)\n "
              "       (interface\n";
        for (const PortRec& p : m_portRecs) {
            const std::string dir = p.dir == "input"    ? "INPUT"
                                    : p.dir == "output" ? "OUTPUT"
                                                        : "INOUT";
            if (p.bits.size() == 1)
                os << "          (port " << edifName(p.name) << " (direction " << dir << "))\n";
            else
                os << "          (port (array (rename " << p.name << " \"" << p.name << "["
                   << (p.bits.size() - 1) << ":0]\") " << p.bits.size() << ") (direction " << dir
                   << "))\n";
        }
        os << "        )\n        (contents\n";
        if (nets.count("GND"))
            os << "          (instance GND (viewRef VIEW_NETLIST (cellRef GND (libraryRef "
                  "LIB))))\n";
        if (nets.count("VCC"))
            os << "          (instance VCC (viewRef VIEW_NETLIST (cellRef VCC (libraryRef "
                  "LIB))))\n";
        for (size_t i = 0; i < m_recs.size(); ++i) {
            const CellRec& r = m_recs[i];
            os << "          (instance " << instNames[i]
               << "\n            (viewRef VIEW_NETLIST (cellRef " << edifName(r.type)
               << " (libraryRef LIB)))";
            for (const auto& pp : r.params)
                os << "\n            (property " << edifName(pp.first) << " "
                   << edifValue(pp.second) << ")";
            os << ")\n";
        }
        for (const auto& kv : nets) {
            const std::string name = kv.first == "GND"   ? "GND_NET"
                                     : kv.first == "VCC" ? "VCC_NET"
                                                         : "n" + kv.first;
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
        struct Ep final {
            std::string path;
            std::string ref;
            bool driver;
        };
        std::map<std::string, std::vector<Ep>> nets;  // net id -> endpoints
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
                for (const LibPort& lp : lib[r.type])
                    if (lp.name == c.port) width = lp.width;
                for (size_t b = 0; b < c.bits.size(); ++b) {
                    const std::string& bit = c.bits[b];
                    if (bit.empty() || (bit[0] == '"' && bit != "\"0\"" && bit != "\"1\""))
                        continue;
                    // constants are local: each module gets its own GND/VCC
                    const std::string id = bit == "\"0\""   ? "GND@" + r.path
                                           : bit == "\"1\"" ? "VCC@" + r.path
                                                            : bit;
                    nets[id].push_back({r.path,
                                        "(portRef " + member(c.port, width, b) + " (instanceRef @"
                                            + std::to_string(i) + "@))",
                                        c.out});
                }
            }
        }
        for (const PortRec& p : m_portRecs) {
            for (size_t b = 0; b < p.bits.size(); ++b) {
                const std::string& bit = p.bits[b];
                const std::string id = bit == "\"0\"" ? "GND@" : bit == "\"1\"" ? "VCC@" : bit;
                if (bit[0] == '"' && bit != "\"0\"" && bit != "\"1\"") continue;
                nets[id].push_back(
                    {"", "(portRef " + member(p.name, p.bits.size(), b) + ")", p.dir == "input"});
            }
        }
        // per module: its ports (net id -> output?) and its local nets
        std::map<std::string, std::map<std::string, bool>> ports;
        std::map<std::string, std::map<std::string, std::vector<std::string>>>
            local;  // path -> net id -> refs
        std::map<std::string, std::set<std::string>> consts;  // path -> {GND, VCC}
        for (auto& kv : nets) {
            const std::string& id = kv.first;
            const bool isConst = id.rfind("GND@", 0) == 0 || id.rfind("VCC@", 0) == 0;
            std::string lca = kv.second.front().path;
            for (const Ep& e : kv.second) lca = commonPath(lca, e.path);
            std::string drvPath;
            bool haveDrv = false;
            for (const Ep& e : kv.second)
                if (e.driver) {
                    drvPath = e.path;
                    haveDrv = true;
                }
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
                    const bool driverInside
                        = haveDrv
                          && (drvPath == p || drvPath.compare(0, p.size() + 1, p + ".") == 0);
                    ports[p][id] = driverInside;
                }
            }
        }
        auto cellName = [&](const std::string& p) {
            return p.empty() ? edifName(m_top) : edifName("h_" + sanitize(p));
        };
        auto lastName = [&](const std::string& p) {
            const size_t d = p.rfind('.');
            return sanitize(d == std::string::npos ? p : p.substr(d + 1));
        };
        std::vector<std::string> order(paths.begin(), paths.end());
        std::sort(order.begin(), order.end(), [](const std::string& a, const std::string& b) {
            const auto da = std::count(a.begin(), a.end(), '.') + (a.empty() ? -1 : 0);
            const auto db = std::count(b.begin(), b.end(), '.') + (b.empty() ? -1 : 0);
            return da != db ? da > db : a < b;
        });
        std::map<std::string, std::vector<std::string>> children;
        for (const std::string& p : paths)
            if (!p.empty()) children[pathOf(p)].push_back(p);
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
            os << "    (cell " << cellName(p)
               << "\n      (cellType GENERIC)\n      (view VIEW_NETLIST\n        (viewType "
                  "NETLIST)\n        (interface\n";
            if (p.empty()) {
                for (const PortRec& pr : m_portRecs) {
                    const std::string dir = pr.dir == "input"    ? "INPUT"
                                            : pr.dir == "output" ? "OUTPUT"
                                                                 : "INOUT";
                    if (pr.bits.size() == 1)
                        os << "          (port " << edifName(pr.name) << " (direction " << dir
                           << "))\n";
                    else
                        os << "          (port (array (rename " << pr.name << " \"" << pr.name
                           << "[" << (pr.bits.size() - 1) << ":0]\") " << pr.bits.size()
                           << ") (direction " << dir << "))\n";
                }
            } else {
                for (const auto& pp : ports[p])
                    os << "          (port p" << pp.first << " (direction "
                       << (pp.second ? "OUTPUT" : "INPUT") << "))\n";
            }
            os << "        )\n        (contents\n";
            for (const std::string& k : consts[p])
                os << "          (instance " << k << " (viewRef VIEW_NETLIST (cellRef " << k
                   << " (libraryRef LIB))))\n";
            for (const std::string& ch : children[p]) {
                os << "          (instance " << edifName(lastName(ch))
                   << " (viewRef VIEW_NETLIST (cellRef " << cellName(ch)
                   << " (libraryRef DESIGN))))\n";
            }
            for (const size_t i : prims[p]) {
                const CellRec& r = m_recs[i];
                os << "          (instance " << hInst[i]
                   << "\n            (viewRef VIEW_NETLIST (cellRef " << edifName(r.type)
                   << " (libraryRef LIB)))";
                for (const auto& pp : r.params)
                    os << "\n            (property " << edifName(pp.first) << " "
                       << edifValue(pp.second) << ")";
                os << ")\n";
            }
            // nets in this module: local endpoints, child ports carrying the net, our own port
            std::map<std::string, std::vector<std::string>> here = local[p];
            for (const std::string& ch : children[p]) {
                const std::string chInst = edifName(lastName(ch));
                const std::string chRef
                    = chInst[0] == '(' ? chInst.substr(8, chInst.find(' ', 8) - 8) : chInst;
                for (const auto& pp : ports[ch])
                    here[pp.first].push_back("(portRef p" + pp.first + " (instanceRef " + chRef
                                             + "))");
            }
            if (!p.empty())
                for (const auto& pp : ports[p])
                    here[pp.first].push_back("(portRef p" + pp.first + ")");
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
                if (id.rfind("GND@", 0) == 0) {
                    name = "GND_NET";
                    src = "(portRef G (instanceRef GND))";
                }
                if (id.rfind("VCC@", 0) == 0) {
                    name = "VCC_NET";
                    src = "(portRef P (instanceRef VCC))";
                }
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
                if (tok[0] == '@') {
                    addr = std::stoul(tok.substr(1), nullptr, 16);
                    continue;
                }
                uint64_t v = 0;
                for (const char ch : tok) {
                    if (ch == '_') continue;
                    const int x = std::isdigit(static_cast<unsigned char>(ch))
                                      ? ch - '0'
                                      : std::tolower(ch) - 'a' + 10;
                    v = (v << 4) | static_cast<uint64_t>(x & 15);
                }
                if (words.size() <= addr) words.resize(addr + 1, 0);
                words[addr++] = v;
            }
        }
        return true;
    }
    static std::string paramStr(const Design::Cell& c, const char* name) {
        for (const auto& p : c.params)
            if (p.first == name) return p.second;
        return "";
    }
    static int paramOf(const Design::Cell& c, const char* name, int dflt) {
        for (const auto& p : c.params)
            if (p.first == name) return std::atoi(p.second.c_str());
        return dflt;
    }
    // A memory cell's pins as nets: input pins through the AIG's primary
    // outputs, output pins as the nets of their free literals
    struct MemPins final {
        std::map<std::string, std::vector<std::string>> ins, outs;
        std::vector<std::string> weBytes;  // per data byte: its write enable (byte-lane memories)
        std::vector<uint64_t> image;  // initial contents, when not the cell's INIT_FILE
        bool hasImage = false;
    };
    MemPins memPins(const Design::Cell& c, const std::vector<int>& inPo) {
        MemPins p;
        for (size_t i = 0; i < c.ins.size(); ++i) {
            std::vector<std::string> nets;
            for (size_t b = 0; b < c.ins[i].second.size(); ++b)
                nets.push_back(m_poNet[inPo[i] + b]);
            p.ins[c.ins[i].first] = nets;
        }
        for (const auto& o : c.outs) {
            std::vector<std::string> nets;
            for (const int l : o.second) nets.push_back(netOfLit(l));
            p.outs[o.first] = nets;
        }
        return p;
    }
    // V3EmitStruct splits a memory written in byte lanes (`if (wen[k])
    // mem[a][8k +: 8] <= d[8k +: 8]`) into one memory per lane, X_lane0 ..
    // X_laneN, alike but for we / wdata / rdata.  Lanes on byte boundaries
    // go back together here, as many as fit one x36 tile (32 data bits), into
    // block RAMs with a write enable per byte (WEA[3:0]): picosoc's 256 x 32
    // memory is one RAMB36E1, not four, and a wider memory is one tile per
    // four byte lanes (a cache of 1024 x 512 bits with 64 byte enables: 16
    // tiles, not 64).  Fills `groups` with one merged memory per tile, in lane
    // order, and returns the cells (by index) taken into them; empty if `ci`
    // is not the first lane of a memory that can be merged.
    std::vector<size_t> mergeLanes(size_t ci,
                                   std::vector<std::pair<Design::Cell, MemPins>>& groups) {
        const Design::Cell& c0 = m_d.cells2[ci];
        // "<path>.<mem>_lane0_inst": V3EmitStruct names the instance after the lane
        std::string sfx;
        std::string stem = c0.name;
        if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, "_inst") == 0) {
            sfx = "_inst";
            stem.resize(stem.size() - 5);
        }
        const size_t at = stem.rfind("_lane");
        if (at == std::string::npos || stem.substr(at) != "_lane0") return {};
        const std::string base = stem.substr(0, at);
        // why a memory in byte lanes stays one block RAM per lane (--struct-only-report)
        const auto no = [&](const std::string& why) -> std::vector<size_t> {
            if (v3Global.opt.structOnlyReport())
                std::cerr << "%Info: map-mem: byte lanes of " << base << " not merged: " << why
                          << "\n";
            return {};
        };
        if (c0.type != "memory_1w1r_sync" && c0.type != "memory_1w2r_sync"
            && c0.type != "memory_sp_sync")
            return no("a " + c0.type);
        std::vector<size_t> lanes;
        for (size_t k = 0;; ++k) {
            const std::string want = base + "_lane" + std::to_string(k) + sfx;
            size_t found = m_d.cells2.size();
            for (size_t cj = 0; cj < m_d.cells2.size() && found == m_d.cells2.size(); ++cj)
                if (m_d.cells2[cj].name == want) found = cj;
            if (found == m_d.cells2.size()) break;
            lanes.push_back(found);
        }
        if (lanes.size() < 2) return {};
        const int aw = paramOf(c0, "ADDR_WIDTH", 1);
        const int depth = paramOf(c0, "DEPTH", 1 << aw);
        if (depth > 1024)  // x36 tiles hold 1K words; deeper lanes need a tile each anyway
            return no(std::to_string(depth) + " words deep, over the 1024 of a 32-bit tile");
        std::vector<MemPins> lp;
        std::vector<int> widths;
        bool anyImage = false;
        for (const size_t cj : lanes) {
            const Design::Cell& c = m_d.cells2[cj];
            if (c.type != c0.type || paramOf(c, "ADDR_WIDTH", 1) != aw
                || paramOf(c, "DEPTH", 1 << aw) != depth)
                return no("lanes of different kinds or depths");
            widths.push_back(paramOf(c, "DATA_WIDTH", 1));
            for (const auto& prm : c.params)
                if (prm.first == "INIT_FILE" && prm.second.size() > 2) anyImage = true;
            lp.push_back(memPins(c, m_cellInPo[cj]));
        }
        // The tiles: lanes in order, as many to a tile as fit its 32 data bits.  Every lane of
        // a tile but its last must end on a byte, as WEA enables whole bytes.
        std::vector<std::pair<size_t, size_t>> tiles;  // [first lane, one past the last)
        for (size_t k = 0; k < lanes.size();) {
            size_t e = k;
            int total = 0;
            while (e < lanes.size() && total % 8 == 0 && total + widths[e] <= 32)
                total += widths[e++];
            if (e == k) return no("a lane wider than a tile");
            tiles.emplace_back(k, e);
            k = e;
        }
        if (tiles.size() == lanes.size()) return {};  // no two lanes share a tile
        // everything but the lane's own we / wdata / rdata must be the same nets
        // (the same function of the design, compared in the AIG: after mapping each lane's pin
        // can be a net of its own even where all lanes are fed the same address)
        const auto litsOf = [&](size_t cj, const std::string& pin) -> const std::vector<int>* {
            for (const auto& in : m_d.cells2[cj].ins)
                if (in.first == pin) return &in.second;
            return nullptr;
        };
        for (size_t k = 1; k < lp.size(); ++k) {
            for (const auto& kv : lp[0].ins) {
                if (kv.first == "we" || kv.first == "wdata") continue;
                const std::vector<int>* const l0 = litsOf(lanes[0], kv.first);
                const std::vector<int>* const lk = litsOf(lanes[k], kv.first);
                if (!l0 || !lk || *l0 != *lk)
                    return no("lane " + std::to_string(k) + " differs from lane 0 at pin "
                              + kv.first);
            }
            if (lp[k].outs.size() != lp[0].outs.size()) return no("lanes with different outputs");
        }
        for (size_t ti = 0; ti < tiles.size(); ++ti) {
            const size_t first = tiles[ti].first, last = tiles[ti].second;
            int total = 0;
            for (size_t k = first; k < last; ++k) total += widths[k];
            Design::Cell merged = m_d.cells2[lanes[first]];
            // one tile keeps the memory's own name, as before; several are numbered
            merged.name
                = tiles.size() == 1 ? base + sfx : base + "_tile" + std::to_string(ti) + sfx;
            for (auto& prm : merged.params) {
                if (prm.first == "DATA_WIDTH") prm.second = std::to_string(total);
                if (prm.first == "INIT_FILE") prm.second = "\"\"";
            }
            MemPins pins = lp[first];
            pins.ins["wdata"].clear();
            for (auto& kv : pins.outs) kv.second.clear();
            std::string weAny = "\"0\"";
            int lo = 0;
            for (size_t k = first; k < last; ++k) {
                const Design::Cell& c = m_d.cells2[lanes[k]];
                const int w = widths[k];
                const std::string we = lp[k].ins["we"].empty() ? "\"0\"" : lp[k].ins["we"][0];
                for (int b = lo; b < lo + w; b += 8) pins.weBytes.push_back(we);
                weAny = orNet(weAny, we);
                const std::vector<std::string>& wd = lp[k].ins["wdata"];
                for (int b = 0; b < w; ++b)
                    pins.ins["wdata"].push_back(b < static_cast<int>(wd.size()) ? wd[b]
                                                                                : "\"0\"");
                for (auto& kv : pins.outs) {
                    const std::vector<std::string>& rd = lp[k].outs[kv.first];
                    for (int b = 0; b < w; ++b)
                        kv.second.push_back(b < static_cast<int>(rd.size()) ? rd[b] : newNet());
                }
                if (anyImage) {
                    std::vector<uint64_t> words;
                    for (const auto& prm : c.params) {
                        if (prm.first != "INIT_FILE" || prm.second.size() <= 2) continue;
                        const std::string f = prm.second.substr(1, prm.second.size() - 2);
                        if (!readHex(f, words))
                            v3error("Synthesis: memory " + c.name + ": INIT_FILE " + f
                                    + " not readable; contents left 0");
                    }
                    if (pins.image.size() < words.size()) pins.image.resize(words.size(), 0);
                    for (size_t i = 0; i < words.size(); ++i)
                        pins.image[i] |= (words[i] & ((w >= 64 ? 0ULL : (1ULL << w)) - 1)) << lo;
                    pins.hasImage = true;
                }
                lo += w;
            }
            pins.ins["we"] = {weAny};
            groups.emplace_back(merged, pins);
        }
        if (v3Global.opt.structOnlyReport())
            std::cerr << "%Info: map-mem: " << lanes.size() << " byte lanes of " << base
                      << " merged into " << tiles.size() << " block RAM"
                      << (tiles.size() == 1 ? "" : "s") << "\n";
        return lanes;
    }
    void emitMemory(const Design::Cell& c, const std::vector<int>& inPo) {
        emitMemory(c, memPins(c, inPo));
    }
    static std::string jsonStr(const std::string& v) {
        std::string o = "\"";
        for (const char ch : v) {
            if (ch == '"' || ch == '\\') {
                o += '\\';
                o += ch;
            } else if (static_cast<unsigned char>(ch) < 0x20)
                o += ' ';
            else
                o += ch;
        }
        return o + "\"";
    }
    // The file $readmemh named, as readHex finds it: as given, else under $MEM_INIT_DIR; made
    // absolute so that a tool run from another directory finds the same file
    static std::string resolveInitFile(const std::string& f) {
        if (f.empty()) return f;
        std::vector<std::string> cands{f};
        if (const char* d = std::getenv("MEM_INIT_DIR")) cands.push_back(std::string(d) + "/" + f);
        for (const std::string& cand : cands) {
            if (!std::ifstream{cand}) continue;
            return V3Os::filenameRealPath(cand);  // realpath, or _fullpath on Windows
        }
        return f;
    }
    // One memory of the manifest.  The cells of the memory are named <cell>_t<lane tile>[_d<depth
    // block>]: lane tile t holds data bits [t*tw, t*tw+tw) of every word (the last one fewer:
    // width), depth block d the words [d*words_per_tile, (d+1)*words_per_tile).  Word k of a block
    // is in bits [k*tw, k*tw+tw) of the block RAM's INIT_xx lines (the data layout of UG473 for a
    // port of tw data bits), which is what a tool needs to place a new image into a placed design.
    void recordMemInit(const Design::Cell& c, const std::string& initFile, const char* prim,
                       int dw, int depth, int tw, int ntiles, int nblocks, int wordsPerTile,
                       bool cascade) {
        std::ostringstream o;
        const std::string cellBase = sanitize(c.name);
        o << "    {\"name\": " << jsonStr(c.name) << ", \"type\": " << jsonStr(c.type)
          << ", \"kind\": \"bram\""
          << ", \"primitive\": " << jsonStr(prim) << ", \"init_file\": " << jsonStr(initFile)
          << ", \"init_path\": " << jsonStr(resolveInitFile(initFile)) << ", \"width\": " << dw
          << ", \"depth\": " << depth << ", \"lane_bits\": " << tw << ", \"lanes\": " << ntiles
          << ", \"blocks\": " << nblocks << ", \"words_per_block\": " << wordsPerTile
          << ", \"cascade\": " << (cascade ? "true" : "false") << ",\n"
          << "     \"cells\": [";
        bool first = true;
        for (int t = 0; t < ntiles; ++t) {
            for (int d = 0; d < nblocks; ++d) {
                const int lo = t * tw;
                o << (first ? "\n" : ",\n") << "       {\"cell\": "
                  << jsonStr(cellBase + "_t" + std::to_string(t)
                             + (nblocks > 1 ? "_d" + std::to_string(d) : ""))
                  << ", \"lane\": " << t << ", \"block\": " << d << ", \"bit_lo\": " << lo
                  << ", \"bits\": " << std::min(tw, dw - lo)
                  << ", \"word_lo\": " << d * wordsPerTile << "}";
                first = false;
            }
        }
        o << "\n     ]}";
        m_memInit.push_back(o.str());
    }
    void writeMemInit(const std::string& filename) {
        std::ofstream os{filename};
        os << "{\n  \"creator\": \"verilator --map-only-meminit\",\n  \"top\": " << jsonStr(m_top)
           << ",\n  \"memories\": [\n";
        for (size_t i = 0; i < m_memInit.size(); ++i)
            os << m_memInit[i] << (i + 1 < m_memInit.size() ? ",\n" : "\n");
        os << "  ]\n}\n";
    }
    void emitMemory(const Design::Cell& c, const MemPins& pins) {
        const int aw = paramOf(c, "ADDR_WIDTH", 1);
        const int dw = paramOf(c, "DATA_WIDTH", 1);
        const int depth = paramOf(c, "DEPTH", 1 << aw);  // words used
        auto pinNets = [&](const char* name) -> std::vector<std::string> {
            const auto it = pins.ins.find(name);
            return it == pins.ins.end() ? std::vector<std::string>{} : it->second;
        };
        // a pin's bits as resolved AIG literals: equal addresses compare equal here even where
        // per-instance abc runs gave their nets different names
        auto pinLits = [&](const char* name) -> std::vector<int> {
            for (const auto& in : c.ins) {
                if (in.first != name) continue;
                std::vector<int> lits;
                for (const int l : in.second) lits.push_back(m_aig.resolve(l));
                return lits;
            }
            return {};
        };
        auto outNets = [&](const char* name) -> std::vector<std::string> {
            const auto it = pins.outs.find(name);
            return it == pins.outs.end() ? std::vector<std::string>{} : it->second;
        };
        auto bit0 = [](const std::vector<std::string>& v, const char* dflt) {
            return v.empty() ? std::string(dflt) : v[0];
        };
        const bool sync = c.type.find("_sync") != std::string::npos;
        const bool tdp = c.type == "memory_tdp_sync";
        if (c.type == "memory_2w2r_rf" || c.type == "memory_2w2r") {
            v3error("Synthesis: " << "memory " + c.name
                                         + ": two write ports with independent read addresses are "
                                           "not a block RAM; dropped");
            return;
        }
        if (!sync) {
            emitLutram(c, pins);  // asynchronous reads: distributed RAM
            return;
        }
        // A small synchronous-read memory (up to 64 words, 512 bits) is
        // distributed RAM with its read registers outside, not a block RAM
        if (!tdp && depth <= 64 && depth * dw <= 512 && !pins.hasImage
            && paramStr(c, "INIT_FILE").size() <= 2) {
            emitLutram(c, pins, true);
            return;
        }
        // The two tile ports.  A: write (+ read 1 with NO_CHANGE when two reads);
        // B: read (2, or 1 for a 1w1r); a TDP memory maps port for port.
        struct Port final {
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
                P[k].mode
                    = paramOf(c, ("READ_FIRST" + sfx).c_str(), 0) ? "READ_FIRST" : "NO_CHANGE";
            }
        } else {
            const std::string clk = bit0(pinNets("clk"), "\"0\"");
            std::vector<std::string> we = pinNets("we"), waddr = pinNets("waddr"),
                                     wdata = pinNets("wdata");
            if (c.type == "memory_sp") waddr = pinNets("addr");
            const bool is1w1r = c.type == "memory_1w1r_sync";
            std::vector<std::string> re1 = pinNets(is1w1r ? "re" : "re1");
            std::vector<std::string> ra1
                = pinNets(is1w1r ? "raddr" : (c.type == "memory_sp" ? "addr" : "raddr1"));
            std::vector<std::string> re2 = pinNets("re2"), ra2 = pinNets("raddr2");
            std::vector<std::string> rd1
                = outNets(is1w1r || c.type == "memory_sp" ? "rdata" : "rdata1");
            std::vector<std::string> rd2 = outNets("rdata2");
            const bool twoReads = !rd2.empty();
            const std::string weBit = bit0(we, "\"0\"");
            const std::string re1Bit = bit0(re1, "\"1\"");
            const std::string re2Bit = bit0(re2, "\"1\"");
            P[0].clk = P[1].clk = clk;
            P[0].we = weBit;
            P[0].din = wdata;
            P[1].we = "\"0\"";
            const std::vector<int> wl = pinLits(c.type == "memory_sp" ? "addr" : "waddr");
            const bool share1 = !wl.empty() && pinLits(is1w1r ? "raddr" : "raddr1") == wl;
            const bool share2 = !wl.empty() && pinLits("raddr2") == wl;
            if (twoReads && (share1 || share2)) {
                // A read at the write address (LiteEth's buffers, the second
                // port a registered-address read became) shares port A with
                // the write: READ_FIRST gives the word before the write, as the
                // synchronous read does, and the other read has port B to itself
                const bool firstShares = share1;
                // HELDn: V3EmitStruct holds that read's output outside the block RAM after a
                // write-only cycle, so the port may load in every write
                if ((firstShares ? re1Bit : re2Bit) != "\"1\""
                    && !paramOf(c, firstShares ? "HELD1" : "HELD2", 0)) {
                    v3error("Synthesis: memory " + c.name
                            + ": a read sharing the write's port must have no enable; its output "
                              "would change in every write");
                    return;
                }
                P[0].en = orNet(weBit, firstShares ? re1Bit : re2Bit);
                P[0].addr = waddr;
                P[0].dout = firstShares ? rd1 : rd2;
                P[0].mode = "READ_FIRST";
                P[1].en = firstShares ? re2Bit : re1Bit;
                P[1].addr = firstShares ? ra2 : ra1;
                P[1].dout = firstShares ? rd2 : rd1;
            } else if (twoReads) {
                // Two reads at their own addresses and a write need three ports: a read would
                // be lost in every write cycle.  V3EmitStruct replicates these into 1w1r copies.
                // so reaching here is an internal inconsistency between the two passes.
                v3fatalSrc("Synthesis: memory " + c.name
                           + ": a write and two reads at independent addresses do not fit one"
                             " block RAM");
                return;
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
            if (32768 / cand >= depth) {
                tw = cand;
                break;
            }
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
        if (cascade && (nblocks & 1))
            ++nblocks;  // the last pair's UPPER is unused, its contents 0
        const int units = cascade ? nblocks / 2 : nblocks;  // what the output mux chooses between
        const int sel0 = cascade ? 1 : 0;  // the first address bit above a unit
        const bool b36 = std::string(prim) == "RAMB36E1";
        const int addrTotal = b36 ? 16 : 14, diW = b36 ? 32 : 16, dipW = b36 ? 4 : 2,
                  weaW = b36 ? 4 : 2, webweW = b36 ? 8 : 4;
        int shift = 0;
        while ((1 << shift) < tw) ++shift;
        const int mw = tw == 8 ? 9 : tw == 16 ? 18 : tw == 32 ? 36 : tw;
        const int wordAddrW = addrTotal - shift - (b36 ? 1 : 0);  // address bits inside one tile
        int hiW = 0;
        while ((1 << hiW) < nblocks) ++hiW;
        auto addrVec = [&](const std::vector<std::string>& a) {
            std::vector<std::string> v;
            for (int i = 0; i < shift; ++i) v.push_back("\"0\"");
            for (int i = 0; i < wordAddrW; ++i)
                v.push_back(i < static_cast<int>(a.size()) ? a[i] : "\"0\"");
            // bit 15: unused (tied high) on a RAMB36 of its own, the half select of a cascaded
            // pair
            if (b36)
                v.push_back(!cascade                                 ? "\"1\""
                            : wordAddrW < static_cast<int>(a.size()) ? a[wordAddrW]
                                                                     : "\"0\"");
            return v;
        };
        auto rep = [&](const std::string& bit, int n) { return std::vector<std::string>(n, bit); };
        auto zeros = [&](int n) { return rep("\"0\"", n); };
        // Per port: block enables (en & hi == d) and the registered block select
        struct Sel final {
            std::vector<std::string> blockEn, regHi;
        };
        Sel S[2];
        for (int k = 0; k < 2; ++k) {
            if (units == 1) {
                S[k].blockEn = {P[k].en};
                continue;
            }
            std::vector<std::string> hi;
            for (int i = 0; i < hiW; ++i) {
                const size_t idx = wordAddrW + i;
                hi.push_back(idx < P[k].addr.size() ? P[k].addr[idx] : "\"0\"");
            }
            for (int u = 0; u < units; ++u) {
                std::string en = P[k].en;
                for (int i = sel0; i < hiW; ++i)
                    en = andNet(en, (u >> (i - sel0)) & 1 ? hi[i] : notNet(hi[i]));
                S[k].blockEn.push_back(en);
            }
            for (int i = sel0; i < hiW; ++i) {
                const std::string q = newNet();
                cell("FDRE", "bsel_" + std::to_string(m_ncells), {{"INIT", "\"0\""}},
                     {{"C", "[" + P[k].clk + "]"},
                      {"CE", "[" + P[k].en + "]"},
                      {"D", "[" + hi[i] + "]"},
                      {"R", "[\"0\"]"}},
                     {{"Q", "[" + q + "]"}});
                S[k].regHi.push_back(q);
            }
        }
        const int ntiles = (dw + tw - 1) / tw;
        // Initial contents ($readmemh INIT_FILE on the primitive) go into the
        // tiles' INIT_xx strings: lane bit i of word k sits at INIT bit k*tw+i
        // (x1/x9/x36 data layout; parity bits unused, INITP left 0)
        std::vector<uint64_t> image = pins.image;
        std::string initFile;
        for (const auto& p : c.params) {
            if (pins.hasImage || p.first != "INIT_FILE") continue;
            std::string f = p.second;
            if (f.size() >= 2 && f.front() == '"') f = f.substr(1, f.size() - 2);
            if (f.empty()) continue;
            initFile = f;
            if (!readHex(f, image))
                v3error("Synthesis: " << "memory " + c.name + ": INIT_FILE " + f
                                             + " not readable; contents left 0");
        }
        const int wordsPerTile = 32768 / tw;
        if (!v3Global.opt.mapOnlyMemInit().empty())
            recordMemInit(c, initFile, prim, dw, depth, tw, ntiles, nblocks, wordsPerTile,
                          cascade);
        auto initParams = [&](int lo, int sw, int d,
                              std::vector<std::pair<std::string, std::string>>& params) {
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
                    if ((image[w] >> (lo + i)) & 1) {
                        bits[255 - j] = '1';
                        any = true;
                    }
                }
                if (!any) continue;
                char name[16];
                VL_SNPRINTF(name, sizeof name, "INIT_%02X", line);
                params.emplace_back(name, "\"" + bits + "\"");
            }
        };
        // block outputs per port per data bit, muxed afterwards when banked
        std::vector<std::vector<std::vector<std::string>>> blkOut(
            2, std::vector<std::vector<std::string>>(dw));
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
                    for (int b = 0; b < sw; ++b)
                        di[k][b] = lo + b < static_cast<int>(P[k].din.size()) ? P[k].din[lo + b]
                                                                              : "\"0\"";
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
                std::vector<std::pair<std::string, std::string>> params
                    = {{"RAM_MODE", "\"TDP\""},
                       {"WRITE_MODE_A", std::string("\"") + P[0].mode + "\""},
                       {"WRITE_MODE_B", std::string("\"") + P[1].mode + "\""},
                       {"READ_WIDTH_A", binParam(mw)},
                       {"WRITE_WIDTH_A", binParam(mw)},
                       {"READ_WIDTH_B", binParam(mw)},
                       {"WRITE_WIDTH_B",
                        binParam(tdp || cascade ? mw : 0)},  // a cascaded pair is x1 on every port
                       {"DOA_REG", binParam(0)},
                       {"DOB_REG", binParam(0)}};
                initParams(lo, sw, d, params);
                if (cascade) {
                    params.emplace_back("RAM_EXTENSION_A", lower ? "\"LOWER\"" : "\"UPPER\"");
                    params.emplace_back("RAM_EXTENSION_B", lower ? "\"LOWER\"" : "\"UPPER\"");
                }
                std::vector<std::pair<std::string, std::string>> ins
                    = {{"CLKARDCLK", "[" + P[0].clk + "]"},
                       {"CLKBWRCLK", "[" + P[1].clk + "]"},
                       {"ENARDEN", "[" + S[0].blockEn[u] + "]"},
                       {"ENBWREN", "[" + S[1].blockEn[u] + "]"},
                       {"REGCEAREGCE", "[\"0\"]"},
                       {"REGCEB", "[\"0\"]"},
                       {"RSTRAMARSTRAM", "[\"0\"]"},
                       {"RSTRAMB", "[\"0\"]"},
                       {"RSTREGARSTREG", "[\"0\"]"},
                       {"RSTREGB", "[\"0\"]"},
                       {"WEA", bitsJson(weaBits(P[0].we, pins.weBytes, lo, sw, tw, weaW))},
                       {"WEBWE", bitsJson(rep(P[1].we, webweW))},
                       {"ADDRARDADDR", bitsJson(addrVec(P[0].addr))},
                       {"ADDRBWRADDR", bitsJson(addrVec(P[1].addr))},
                       {"DIADI", bitsJson(di[0])},
                       {"DIPADIP", bitsJson(zeros(dipW))},
                       {"DIBDI", bitsJson(di[1])},
                       {"DIPBDIP", bitsJson(zeros(dipW))}};
                std::vector<std::pair<std::string, std::string>> outs
                    = {{"DOADO", bitsJson(dout[0])},
                       {"DOPADOP", bitsJson(dop[0])},
                       {"DOBDO", bitsJson(dout[1])},
                       {"DOPBDOP", bitsJson(dop[1])}};
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
                    m_curLeaf = leafOf(c.name, lo) + "_t" + std::to_string(t)
                                + (nblocks > 1 ? "_d" + std::to_string(d) : "");
                }
                cell(prim,
                     sanitize(c.name) + "_t" + std::to_string(t)
                         + (nblocks > 1 ? "_d" + std::to_string(d) : ""),
                     params, ins, outs);
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
    // Port A's write enables for the tile holding data bits [lo, lo+sw): one
    // we for the whole port, or, for a byte-lane memory, the lane's we on each
    // byte.  In x36 mode WEA[j] enables byte j of the tile; narrower tiles
    // hold one byte or less and use WEA[0] (all four tied alike, UG473).
    static std::vector<std::string> weaBits(const std::string& we,
                                            const std::vector<std::string>& weBytes, int lo,
                                            int sw, int tw, int weaW) {
        std::vector<std::string> v(weaW, we);
        if (weBytes.empty()) return v;
        for (int j = 0; j < weaW; ++j) {
            const size_t byte = tw >= 32 ? lo / 8 + j : lo / 8;
            if (tw >= 32 && 8 * j >= sw)
                v[j] = "\"0\"";
            else
                v[j] = weBytes[std::min(byte, weBytes.size() - 1)];
        }
        return v;
    }
    // Asynchronous-read memory (memory_sp / memory_1w1r / memory_1w2r):
    // RAM64X1D per data bit per 64 words, written through WE decoded from the
    // upper write-address bits, read through DPRA with a LUT mux over the
    // blocks on the upper read-address bits; a second read port is a second
    // copy of the array (both written).
    // registered: synchronous reads (memory_*_sync) -- each read port's data
    // goes through an FDRE with the port's enable on CE
    void emitLutram(const Design::Cell& c, const MemPins& pins, bool registered = false) {
        const int aw = paramOf(c, "ADDR_WIDTH", 1);
        const int dw = paramOf(c, "DATA_WIDTH", 1);
        const int depth = paramOf(c, "DEPTH", 1 << aw);  // words used
        auto pinNets = [&](const char* name) -> std::vector<std::string> {
            const auto it = pins.ins.find(name);
            return it == pins.ins.end() ? std::vector<std::string>{} : it->second;
        };
        auto outNets = [&](const char* name) -> std::vector<std::string> {
            const auto it = pins.outs.find(name);
            return it == pins.outs.end() ? std::vector<std::string>{} : it->second;
        };
        const bool sp = c.type == "memory_sp" || c.type == "memory_sp_sync";
        const std::string clk = pinNets("clk").empty() ? "\"0\"" : pinNets("clk")[0];
        const std::vector<std::string> we = pinNets("we");
        const std::string weBit = we.empty() ? "\"0\"" : we[0];
        const std::vector<std::string> waddr = pinNets(sp ? "addr" : "waddr");
        const std::vector<std::string> wdata = pinNets("wdata");
        std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>>
            reads;  // (raddr, rdata)
        std::vector<std::string> readEn;  // per read: its enable (registered reads)
        const auto re = [&](const char* pin) {
            const auto v = pinNets(pin);
            return v.empty() ? std::string("\"1\"") : v[0];
        };
        if (sp) {
            reads.emplace_back(waddr, outNets("rdata"));
            readEn.push_back(re("re"));
        } else if (c.type == "memory_1w1r_sync") {
            reads.emplace_back(pinNets("raddr"), outNets("rdata"));
            readEn.push_back(re("re"));
        } else {
            if (!outNets("rdata1").empty()) {
                reads.emplace_back(pinNets("raddr1"), outNets("rdata1"));
                readEn.push_back(re("re1"));
            }
            if (!outNets("rdata2").empty()) {
                reads.emplace_back(pinNets("raddr2"), outNets("rdata2"));
                readEn.push_back(re("re2"));
            }
        }
        if (registered) {
            // the LUTRAM's outputs go to fresh nets, then through FDREs to the ports
            for (size_t r = 0; r < reads.size(); ++r) {
                std::vector<std::string> raw;
                for (size_t b = 0; b < reads[r].second.size(); ++b) {
                    const std::string n = newNet();
                    raw.push_back(n);
                    cell("FDRE",
                         sanitize(c.name) + "_r" + std::to_string(r) + "_q" + std::to_string(b),
                         {{"INIT", "\"0\""}},
                         {{"C", "[" + clk + "]"},
                          {"CE", "[" + readEn[r] + "]"},
                          {"D", "[" + n + "]"},
                          {"R", "[\"0\"]"}},
                         {{"Q", "[" + reads[r].second[b] + "]"}});
                }
                reads[r].second = raw;
            }
        }
        const int nblk = (depth + 63) / 64;
        int hiW = 0;
        while ((1 << hiW) < nblk) ++hiW;
        auto bitOr0 = [](const std::vector<std::string>& v, size_t i) {
            return i < v.size() ? v[i] : std::string("\"0\"");
        };
        std::vector<std::string> blockWe;
        for (int d = 0; d < nblk; ++d) {
            std::string en = weBit;
            for (int i = 0; i < hiW; ++i)
                en = andNet(en,
                            (d >> i) & 1 ? bitOr0(waddr, 6 + i) : notNet(bitOr0(waddr, 6 + i)));
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
                    std::vector<std::pair<std::string, std::string>> ins
                        = {{"WCLK", "[" + clk + "]"},
                           {"WE", "[" + blockWe[d] + "]"},
                           {"D", "[" + bitOr0(wdata, b) + "]"}};
                    for (int i = 0; i < 6; ++i)
                        ins.emplace_back("A" + std::to_string(i), "[" + bitOr0(waddr, i) + "]");
                    for (int i = 0; i < 6; ++i)
                        ins.emplace_back("DPRA" + std::to_string(i), "[" + bitOr0(raddr, i) + "]");
                    cell("RAM64X1D",
                         sanitize(c.name) + "_r" + std::to_string(r) + "_b" + std::to_string(b)
                             + "_d" + std::to_string(d),
                         {{"INIT", "\"" + std::string(64, '0') + "\""}}, ins,
                         {{"SPO", "[" + newNet() + "]"}, {"DPO", "[" + dpo + "]"}});
                }
                if (nblk > 1) muxTree(blk, rhi, rdata[b]);
            }
        }
    }
    // out = ins[sel] through a tree of 2:1 LUT3 muxes; the root drives `out`
    void muxTree(const std::vector<std::string>& ins, const std::vector<std::string>& sel,
                 const std::string& out) {
        std::vector<std::string> level = ins;
        for (size_t s = 0; s < sel.size(); ++s) {
            std::vector<std::string> next;
            for (size_t i = 0; i < level.size(); i += 2) {
                const std::string a = level[i];
                const std::string b = i + 1 < level.size() ? level[i + 1] : "\"0\"";
                const bool last = s + 1 == sel.size() && level.size() <= 2;
                const std::string o = last ? out : newNet();
                cell("LUT3", "bmux_" + std::to_string(m_ncells), {{"INIT", "\"11001010\""}},
                     {{"I0", "[" + a + "]"}, {"I1", "[" + b + "]"}, {"I2", "[" + sel[s] + "]"}},
                     {{"O", "[" + o + "]"}});
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
        cell("LUT2", "and_" + std::to_string(m_ncells), {{"INIT", "\"1000\""}},
             {{"I0", "[" + a + "]"}, {"I1", "[" + b + "]"}}, {{"O", "[" + o + "]"}});
        return o;
    }
    std::string notNet(const std::string& a) {
        if (a == "\"0\"") return "\"1\"";
        if (a == "\"1\"") return "\"0\"";
        const std::string o = newNet();
        cell("LUT1", "not_" + std::to_string(m_ncells), {{"INIT", "\"01\""}},
             {{"I0", "[" + a + "]"}}, {{"O", "[" + o + "]"}});
        return o;
    }
    // a | b of two 1-bit nets (LUT2)
    std::string orNet(const std::string& a, const std::string& b) {
        if (a == "\"0\"") return b;
        if (b == "\"0\"") return a;
        if (a == "\"1\"" || b == "\"1\"") return "\"1\"";
        const std::string o = newNet();
        cell("LUT2", "or_" + std::to_string(m_ncells), {{"INIT", "\"1110\""}},
             {{"I0", "[" + a + "]"}, {"I1", "[" + b + "]"}}, {{"O", "[" + o + "]"}});
        return o;
    }
    // we ? waddr : raddr per bit (LUT3 mux)
    std::vector<std::string> muxAddr(const std::string& we, const std::vector<std::string>& wa,
                                     const std::vector<std::string>& ra) {
        std::vector<std::string> out;
        const size_t n = std::max(wa.size(), ra.size());
        for (size_t i = 0; i < n; ++i) {
            const std::string a = i < wa.size() ? wa[i] : "\"0\"";
            const std::string b = i < ra.size() ? ra[i] : "\"0\"";
            if (a == b) {
                out.push_back(a);
                continue;
            }
            const std::string o = newNet();
            // I2=we, I1=a (waddr), I0=b (raddr): out = we ? a : b   INIT index = I2 I1 I0
            cell("LUT3", "amux_" + std::to_string(m_ncells), {{"INIT", "\"11001010\""}},
                 {{"I0", "[" + b + "]"}, {"I1", "[" + a + "]"}, {"I2", "[" + we + "]"}},
                 {{"O", "[" + o + "]"}});
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
