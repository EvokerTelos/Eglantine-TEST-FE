// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// Eden's Xbyak configuration (its hash maps): every translation unit must see the same one.
#include "common/x64/xbyak.h"

#include "video_core/macro_turing.h"

namespace Tegra {

/// EDEN_MME_JIT=0 runs native macros through TuringMacroInterpreter instead of compiled code.
inline bool TuringMacroJitEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_MME_JIT");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// x86-64 code for the TU104/GA10x MME: every instruction of the uploaded macro memory compiled
// once (each a block that falls through to the next one, branches and loops through a jump
// table), with the semantics of TuringMacroInterpreter. Emits (method writes), register reads
// and the FIFO DMA (methods 0x154/0x155/0x158, then EXTENDED 0x1000 with y = 1) call back into
// C++. Adapted from Eglantine's nx2 branch (macro_turing_jit.h).
class TuringMacroJit {
public:
    using Word = std::uint32_t;
    using Result = TuringMacroInterpreter::Result;
    using MemoryRead = TuringMacroInterpreter::MemoryRead;

    // One instruction's fields, with the lane/output enable masks for a non-zero and a zero
    // predicate register (same bit layout as TuringMacroInterpreter::Decode).
    struct Decoded {
        std::array<Word, 2> imm, op, dst, src_a, src_b, out_method, out_emit;
        Word pred_reg, pred_mode;
        Word mask_true, mask_false;
        bool end;
        std::array<bool, 2> load;
    };

    static Decoded Decode(const std::array<Word, 3>& bits) {
        const auto field = [&](Word low, Word count) {
            std::uint64_t value = bits[low / 32];
            if (low / 32 < 2) value |= std::uint64_t{bits[low / 32 + 1]} << 32;
            return Word((value >> (low % 32)) & ((std::uint64_t{1} << count) - 1));
        };
        Decoded d;
        d.imm = {field(30, 16), field(66, 16)};
        d.op = {field(10, 5), field(46, 5)};
        d.dst = {field(15, 5), field(51, 5)};
        d.src_a = {field(20, 5), field(56, 5)};
        d.src_b = {field(25, 5), field(61, 5)};
        d.out_method = {field(82, 3), field(89, 3)};
        d.out_emit = {field(85, 4), field(92, 4)};
        d.pred_reg = field(5, 5);
        d.pred_mode = field(1, 4);
        d.end = field(0, 1) != 0;
        constexpr std::array<std::string_view, 16> predicates{
            "UUUU", "TTTT", "FFFF", "TTUU", "FFUU", "TFUU", "TUUU", "FUUU",
            "UUTT", "UUTF", "UUTU", "UUFT", "UUFF", "UUFU", "UUUT", "UUUF"};
        d.mask_true = d.mask_false = 15;
        if (d.pred_mode) {
            d.mask_true = d.mask_false = 0;
            for (Word i = 0; i < 4; ++i) {
                if (predicates[d.pred_mode][i] != 'T') d.mask_true |= 1U << i;
                if (predicates[d.pred_mode][i] != 'F') d.mask_false |= 1U << i;
            }
        }
        for (Word i = 0; i < 2; ++i) {
            const Word reg = 28 + i, out = 3 + i;
            d.load[i] = d.pred_reg == reg || d.src_a[0] == reg || d.src_a[1] == reg ||
                        d.src_b[0] == reg || d.src_b[1] == reg || d.out_method[0] == out ||
                        d.out_method[1] == out || d.out_emit[0] == out || d.out_emit[1] == out;
        }
        return d;
    }

    struct State {
        std::array<Word, 24> regs;
        Word carry;
        Word method;
        Word increment;
        Word loop_count;
        Word loop_start;
        Word loop_end;
        Word next_pc;
        Word steps;
        Word start;
        Word consumed;
        Word cursor;
        Word params_size;
        Word fail_pc;
        Word dma_high;
        Word dma_low;
        Word dma_count;
        Word pending_error;
        std::uint8_t end_next;
        std::uint8_t stopped;
        std::uint8_t cancel_end;
        std::uint8_t pad;
        Word scratch_x;
        Word scratch_y;
        const Word* params;
        Word* dram;
        void* user;
        bool (*emit)(State*, Word method, Word value);
        Word (*read)(State*, Word address);
        bool (*extended)(State*, Word x, Word y);
    };

    enum Error : Word {
        None = 0,
        FifoUnderflow,
        InvalidSource,
        InvalidDestination,
        UnsupportedExtended,
        InvalidMerge,
        InvalidLoop,
        DataRamReadOutOfBounds,
        DataRamWriteOutOfBounds,
        InvalidOutput,
        UnsupportedEmit,
        OutsideCode,
        StepLimit,
        DmaReadFailed,
        DmaLengthOutOfBounds,
        UnsupportedDma,
    };

    static std::string_view ErrorText(Word error) {
        constexpr std::array<std::string_view, 16> texts{
            "",
            "parameter FIFO underflow",
            "invalid source register",
            "invalid destination register",
            "unsupported extended operation",
            "invalid merge destination",
            "invalid loop",
            "data RAM read out of bounds",
            "data RAM write out of bounds",
            "invalid output selector",
            "unsupported emitted method",
            "instruction outside uploaded RAM",
            "instruction limit exceeded",
            "FIFO DMA memory read failed",
            "FIFO DMA length out of bounds",
            "unsupported DMA method",
        };
        return error < texts.size() ? texts[error] : "unknown";
    }

    /// Compiles `code` (3 words per instruction) when it differs from what was compiled.
    /// Returns false, leaving the old code in place, if a macro is still running on it.
    [[nodiscard]] bool Prepare(std::span<const Word> code, std::uint64_t generation) {
        if (program && generation == compiled_generation) {
            return true;
        }
        if (running != 0) {
            return false;
        }
        program = std::make_unique<Program>(code);
        compiled_generation = generation;
        return true;
    }

    /// `data_ram` must hold 0xc00 words.
    template <typename Read, typename Emit>
    Result Execute(Word start, std::span<const Word> parameters, std::span<Word> data_ram, Read read,
                   Emit emit, const MemoryRead& memory_read = {}) {
        State state;
        state.regs.fill(0);
        state.carry = state.method = state.increment = 0;
        state.loop_count = state.loop_start = state.loop_end = 0;
        state.consumed = state.cursor = 0;
        state.fail_pc = 0;
        state.dma_high = state.dma_low = state.dma_count = 0;
        state.pending_error = None;
        state.end_next = state.stopped = state.cancel_end = 0;
        state.start = start;
        state.steps = 1000000;
        state.params = parameters.data();
        state.params_size = static_cast<Word>(parameters.size());
        state.dram = data_ram.data();
        std::array<Word, 1024> dma_fifo; // only the words a FIFO DMA read fills are used
        Thunks<Read, Emit> thunks{read, emit, memory_read, dma_fifo.data()};
        state.user = &thunks;
        state.emit = &Thunks<Read, Emit>::CallEmit;
        state.read = &Thunks<Read, Emit>::CallRead;
        state.extended = &Thunks<Read, Emit>::CallExtended;
        ++running;
        Word error = program->entry(&state);
        --running;
        if (error == None) {
            return {true, state.fail_pc, state.consumed, {}};
        }
        if (state.pending_error != None) {
            error = state.pending_error;
        }
        return {false, state.fail_pc, state.consumed, ErrorText(error)};
    }

private:
    template <typename Read, typename Emit>
    struct Thunks {
        Read& read;
        Emit& emit;
        const MemoryRead& memory_read;
        Word* dma_fifo;

        // The FIFO DMA methods configure the next EXTENDED load; they are not engine methods.
        static bool CallEmit(State* state, Word method, Word value) {
            switch (method) {
            case 0x154:
                state->dma_high = value & 0xff;
                return true;
            case 0x155:
                state->dma_low = value;
                return true;
            case 0x158:
                if (value > 1024) {
                    state->pending_error = DmaLengthOutOfBounds;
                    return false;
                }
                state->dma_count = value;
                return true;
            case 0x156: case 0x157: case 0x159: case 0x15a:
                state->pending_error = UnsupportedDma;
                return false;
            default:
                return static_cast<Thunks*>(state->user)->emit(method, value);
            }
        }
        static Word CallRead(State* state, Word address) {
            return static_cast<Thunks*>(state->user)->read(address);
        }
        // EXTENDED: x = 0x1000, y = 1 replaces the parameter FIFO with the configured DMA read.
        static bool CallExtended(State* state, Word x, Word y) {
            if (x != 0x1000 || y != 1) {
                return false;
            }
            if (state->dma_count == 0) {
                return true;
            }
            auto& self = *static_cast<Thunks*>(state->user);
            const std::uint64_t address = (std::uint64_t{state->dma_high} << 32) | state->dma_low;
            const std::span<Word> words{self.dma_fifo, state->dma_count};
            if (!self.memory_read || !self.memory_read(address, words)) {
                state->pending_error = DmaReadFailed;
                return false;
            }
            state->params = self.dma_fifo;
            state->params_size = state->dma_count;
            state->cursor = 0;
            state->dma_count = 0;
            return true;
        }
    };

    struct Program : Xbyak::CodeGenerator {
        explicit Program(std::span<const Word> code)
            : Xbyak::CodeGenerator(1 << 20, Xbyak::AutoGrow) {
            Generate(code);
            ready();
            entry = getCode<Word (*)(State*)>();
        }

        Word (*entry)(State*) = nullptr;

    private:
        using Decoded = TuringMacroJit::Decoded;
#ifdef _WIN32
        const Xbyak::Reg64 arg0 = rcx, arg1 = rdx, arg2 = r8;
#else
        const Xbyak::Reg64 arg0 = rdi, arg1 = rsi, arg2 = rdx;
#endif
        // rbx: State*; r12d/r13d: the instruction's FIFO loads; r14d/r15d: its ALU results.
        const Xbyak::Reg64 st = rbx;

        static constexpr std::size_t RegOffset(Word reg) {
            return offsetof(State, regs) + reg * sizeof(Word);
        }

        void Generate(std::span<const Word> code) {
            const Word count = static_cast<Word>(code.size() / 3);
            labels.resize(count);
            std::vector<Decoded> decoded(count);
            for (Word pc = 0; pc < count; ++pc) {
                decoded[pc] = TuringMacroJit::Decode(
                    {code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]});
            }
            Analyze(decoded);
            push(rbx); push(rbp); push(r12); push(r13); push(r14); push(r15);
            sub(rsp, 40);
            mov(st, arg0);
            mov(eax, dword[st + offsetof(State, start)]);
            cmp(eax, count);
            jae(fail_outside, T_NEAR);
            lea(rcx, ptr[rip + table]);
            jmp(ptr[rcx + rax * 8]);

            for (Word pc = 0; pc < count; ++pc) {
                L(labels[pc]);
                Instruction(pc, count, decoded[pc]);
            }

            // Dispatch on eax (the next pc). Every backward edge comes through here, so the
            // instruction limit is counted here.
            L(dispatch);
            sub(dword[st + offsetof(State, steps)], 1);
            jz(fail_steps, T_NEAR);
            cmp(eax, count);
            jae(fail_outside, T_NEAR);
            lea(rcx, ptr[rip + table]);
            jmp(ptr[rcx + rax * 8]);

            L(success);
            xor_(eax, eax);
            L(leave);
            add(rsp, 40);
            pop(r15); pop(r14); pop(r13); pop(r12); pop(rbp); pop(rbx);
            ret();
            const auto fail_with = [&](Xbyak::Label& label, Word error) {
                L(label);
                mov(eax, error);
                jmp(leave, T_NEAR);
            };
            fail_with(fail_underflow, FifoUnderflow);
            fail_with(fail_source, InvalidSource);
            fail_with(fail_destination, InvalidDestination);
            fail_with(fail_extended, UnsupportedExtended);
            fail_with(fail_merge, InvalidMerge);
            fail_with(fail_loop, InvalidLoop);
            fail_with(fail_dread, DataRamReadOutOfBounds);
            fail_with(fail_dwrite, DataRamWriteOutOfBounds);
            fail_with(fail_output, InvalidOutput);
            fail_with(fail_emit, UnsupportedEmit);
            fail_with(fail_outside, OutsideCode);
            fail_with(fail_steps, StepLimit);

            align(8);
            L(table);
            for (Word pc = 0; pc < count; ++pc) {
                putL(labels[pc]);
            }
        }

        // Loads MME register `reg` (as an operand of `lane`) into `dst`.
        void Get(const Xbyak::Reg32& dst, const Decoded& d, Word reg, Word lane) {
            if (reg < 24) {
                mov(dst, dword[st + RegOffset(reg)]);
                return;
            }
            switch (reg) {
            case 24: xor_(dst, dst); return;
            case 25: mov(dst, Word(std::int32_t(std::int16_t(d.imm[lane])))); return;
            case 26: mov(dst, Word(std::int32_t(std::int16_t(d.imm[1 - lane])))); return;
            case 27: mov(dst, (d.imm[0] << 16) | d.imm[1]); return;
            case 28: mov(dst, r12d); return;
            case 29: mov(dst, r13d); return;
            default: jmp(fail_source, T_NEAR); return;
            }
        }

        static bool IsFlow(Word op) {
            return op >= 23 && op <= 29;
        }

        // Where a loop can end (its back edge is checked only there) and which instructions can
        // run right after an END (only those check for the end of the macro).
        void Analyze(const std::vector<Decoded>& decoded) {
            const Word count = static_cast<Word>(decoded.size());
            loop_end.assign(count, false);
            after_end.assign(count, false);
            std::vector<Word> loop_starts_of_end(count, ~0U);
            const auto mark = [&](std::vector<bool>& set, std::int64_t pc) {
                if (pc >= 0 && pc < count) set[static_cast<std::size_t>(pc)] = true;
            };
            for (Word pc = 0; pc < count; ++pc) {
                for (Word lane = 0; lane < 2; ++lane) {
                    const Decoded& d = decoded[pc];
                    if (d.op[lane] == 23 && d.imm[lane] >= 2 && pc + d.imm[lane] - 1 < count) {
                        loop_end[pc + d.imm[lane] - 1] = true;
                    }
                }
            }
            for (Word pc = 0; pc < count; ++pc) {
                const Decoded& d = decoded[pc];
                if (!d.end) continue;
                mark(after_end, std::int64_t{pc} + 1);
                for (Word lane = 0; lane < 2; ++lane) {
                    const Word imm = d.imm[lane];
                    switch (d.op[lane]) {
                    case 23: mark(after_end, std::int64_t{pc} + imm); break;
                    case 24: mark(after_end, std::int64_t{pc} + (imm & 0x7fff)); break;
                    case 25: case 26: case 27: case 28: case 29:
                        mark(after_end, std::int64_t{pc} + (std::int32_t(imm << 19) >> 19));
                        break;
                    default: break;
                    }
                }
            }
            // An END at a loop's last instruction can be followed by the loop's second one.
            for (Word pc = 0; pc < count; ++pc) {
                for (Word lane = 0; lane < 2; ++lane) {
                    const Decoded& d = decoded[pc];
                    if (d.op[lane] == 23 && d.imm[lane] >= 2) {
                        const Word end = pc + d.imm[lane] - 1;
                        if (end < count && decoded[end].end) mark(after_end, std::int64_t{pc} + 1);
                    }
                }
            }
        }

        void Instruction(Word pc, Word count, const Decoded& d) {
            // FIFO loads, in lane order.
            for (Word i = 0; i < 2; ++i) {
                const Xbyak::Reg32 load = i == 0 ? r12d : r13d;
                if (!d.load[i]) {
                    xor_(load, load);
                    continue;
                }
                mov(eax, dword[st + offsetof(State, cursor)]);
                cmp(eax, dword[st + offsetof(State, params_size)]);
                jae(fail_underflow, T_NEAR);
                mov(rcx, qword[st + offsetof(State, params)]);
                mov(load, dword[rcx + rax * 4]);
                inc(eax);
                mov(dword[st + offsetof(State, cursor)], eax);
                inc(dword[st + offsetof(State, consumed)]);
            }
            if (!d.pred_mode) {
                Body(pc, count, d, 15);
                return;
            }
            Xbyak::Label when_false;
            Get(eax, d, d.pred_reg, 0);
            test(eax, eax);
            jz(when_false, T_NEAR);
            Body(pc, count, d, d.mask_true);
            L(when_false);
            Body(pc, count, d, d.mask_false);
        }

        void Body(Word pc, Word count, const Decoded& d, Word mask) {
            // An inactive lane's result reads as 0 when an output selects it.
            for (Word lane = 0; lane < 2; ++lane) {
                if (mask & (1U << lane)) continue;
                bool selected = false;
                for (Word out = 0; out < 2; ++out) {
                    if (!(mask & (4U << out))) continue;
                    selected |= d.out_method[out] == lane + 1 || d.out_emit[out] == lane + 1;
                }
                if (selected) {
                    xor_(lane == 0 ? r14d : r15d, lane == 0 ? r14d : r15d);
                }
            }
            const bool flow = ((mask & 1) && IsFlow(d.op[0])) || ((mask & 2) && IsFlow(d.op[1]));
            if (flow) {
                mov(dword[st + offsetof(State, next_pc)], pc + 1);
                mov(byte[st + offsetof(State, cancel_end)], 0);
            }
            // Lane 1's operands are read before lane 0 commits its destination (or calls out).
            const bool lane0_active = (mask & 1) != 0;
            const bool early_operands =
                (mask & 2) && lane0_active &&
                (d.op[0] == 22 || (d.dst[0] < 24 && (d.dst[0] == d.src_a[1] || d.dst[0] == d.src_b[1])));
            if (early_operands) {
                Get(eax, d, d.src_a[1], 1);
                mov(dword[st + offsetof(State, scratch_x)], eax);
                Get(eax, d, d.src_b[1], 1);
                mov(dword[st + offsetof(State, scratch_y)], eax);
            }
            for (Word lane = 0; lane < 2; ++lane) {
                if (!(mask & (1U << lane))) {
                    continue;
                }
                if (lane == 1 && early_operands) {
                    mov(ecx, dword[st + offsetof(State, scratch_x)]);
                    mov(edx, dword[st + offsetof(State, scratch_y)]);
                } else {
                    Get(ecx, d, d.src_a[lane], lane);
                    Get(edx, d, d.src_b[lane], lane);
                }
                Lane(pc, d, lane);
                // eax holds the lane's value.
                mov(lane == 0 ? r14d : r15d, eax);
                if (d.dst[lane] < 24) {
                    mov(dword[st + RegOffset(d.dst[lane])], eax);
                } else if (d.dst[lane] != 24) {
                    jmp(fail_destination, T_NEAR);
                }
            }
            for (Word lane = 0; lane < 2; ++lane) {
                if (!(mask & (4U << lane))) {
                    continue;
                }
                if (d.out_method[lane]) {
                    Output(eax, d, d.out_method[lane]);
                    mov(ecx, eax);
                    and_(ecx, 0xfff);
                    mov(dword[st + offsetof(State, method)], ecx);
                    shr(eax, 12);
                    and_(eax, 15);
                    mov(dword[st + offsetof(State, increment)], eax);
                }
                if (d.out_emit[lane]) {
                    Output(eax, d, d.out_emit[lane]);
                    mov(arg2.cvt32(), eax);
                    mov(arg1.cvt32(), dword[st + offsetof(State, method)]);
                    mov(arg0, st);
                    call(ptr[st + offsetof(State, emit)]);
                    test(al, al);
                    jz(fail_emit, T_NEAR);
                    mov(eax, dword[st + offsetof(State, increment)]);
                    add(dword[st + offsetof(State, method)], eax);
                }
            }
            // An END in the previous instruction ends the macro after this one.
            if (after_end[pc]) {
                cmp(byte[st + offsetof(State, end_next)], 0);
                jne(success, T_NEAR);
            }
            if (flow) {
                cmp(byte[st + offsetof(State, stopped)], 0);
                jne(success, T_NEAR);
            }
            if (d.end) {
                if (flow) {
                    mov(al, byte[st + offsetof(State, cancel_end)]);
                    xor_(al, 1);
                    mov(byte[st + offsetof(State, end_next)], al);
                } else {
                    mov(byte[st + offsetof(State, end_next)], 1);
                }
            }
            if (loop_end[pc]) {
                Xbyak::Label no_loop;
                mov(eax, dword[st + offsetof(State, loop_count)]);
                test(eax, eax);
                jz(no_loop);
                cmp(dword[st + offsetof(State, loop_end)], pc);
                jne(no_loop);
                dec(dword[st + offsetof(State, loop_count)]);
                mov(eax, dword[st + offsetof(State, loop_start)]);
                inc(eax);
                jmp(dispatch, T_NEAR);
                L(no_loop);
            }
            if (flow) {
                mov(eax, dword[st + offsetof(State, next_pc)]);
                if (pc + 1 < count) {
                    cmp(eax, pc + 1);
                    jne(dispatch, T_NEAR);
                    jmp(labels[pc + 1], T_NEAR);
                } else {
                    jmp(dispatch, T_NEAR);
                }
            } else if (pc + 1 < count) {
                jmp(labels[pc + 1], T_NEAR);
            } else {
                jmp(fail_outside, T_NEAR);
            }
        }

        void Output(const Xbyak::Reg32& dst, const Decoded& d, Word selector) {
            switch (selector) {
            case 1: mov(dst, r14d); return;
            case 2: mov(dst, r15d); return;
            case 3: mov(dst, r12d); return;
            case 4: mov(dst, r13d); return;
            case 5: mov(dst, d.imm[0]); return;
            case 6: mov(dst, d.imm[1]); return;
            case 8: mov(dst, d.imm[0] >> 12); return;
            case 9: mov(dst, d.imm[1] >> 12); return;
            case 10: mov(dst, (d.imm[0] << 16) | d.imm[1]); return;
            default: jmp(fail_output, T_NEAR); return;
            }
        }

        // ecx = x, edx = y; leaves the value in eax.
        void Compare(Word op) {
            cmp(ecx, edx);
            switch (op) {
            case 17: case 25: setl(al); break;
            case 18: case 26: setb(al); break;
            case 19: case 27: setle(al); break;
            case 20: case 28: setbe(al); break;
            default: sete(al); break;
            }
            movzx(eax, al);
        }

        void Lane(Word pc, const Decoded& d, Word lane) {
            const Word imm = d.imm[lane];
            const Word op = d.op[lane];
            switch (op) {
            case 0:
                mov(eax, ecx);
                add(eax, edx);
                setc(r8b);
                movzx(r8d, r8b);
                mov(dword[st + offsetof(State, carry)], r8d);
                return;
            case 1:
                lea(eax, ptr[rcx + rdx]);
                add(eax, dword[st + offsetof(State, carry)]);
                return;
            case 2:
                mov(eax, ecx);
                sub(eax, edx);
                setc(r8b);
                movzx(r8d, r8b);
                mov(dword[st + offsetof(State, carry)], r8d);
                return;
            case 3:
                mov(eax, ecx);
                sub(eax, edx);
                sub(eax, dword[st + offsetof(State, carry)]);
                return;
            case 4:
                movsxd(rax, ecx);
                movsxd(rdx, edx);
                imul(rax, rdx);
                mov(rcx, rax);
                shr(rcx, 32);
                mov(dword[st + offsetof(State, carry)], ecx);
                return;
            case 6:
                mov(eax, ecx);
                mov(edx, edx);
                imul(rax, rdx);
                mov(rcx, rax);
                shr(rcx, 32);
                mov(dword[st + offsetof(State, carry)], ecx);
                return;
            case 5:
                mov(eax, dword[st + offsetof(State, carry)]);
                return;
            case 7:
                // ecx = x, edx = y. Copy y first: on Windows arg1 is rdx.
                mov(arg2.cvt32(), edx);
                mov(arg1.cvt32(), ecx);
                mov(arg0, st);
                call(ptr[st + offsetof(State, extended)]);
                test(al, al);
                jz(fail_extended, T_NEAR);
                xor_(eax, eax);
                return;
            case 8: {
                Xbyak::Label zero, done;
                test(ecx, ecx);
                jz(zero);
                bsr(eax, ecx);
                xor_(eax, 31);
                jmp(done);
                L(zero);
                mov(eax, 32);
                L(done);
                return;
            }
            case 9:
                mov(eax, ecx);
                mov(ecx, edx);
                and_(ecx, 31);
                shl(eax, cl);
                return;
            case 10:
                mov(eax, ecx);
                mov(ecx, edx);
                and_(ecx, 31);
                shr(eax, cl);
                return;
            case 11:
                mov(eax, ecx);
                mov(ecx, edx);
                and_(ecx, 31);
                sar(eax, cl);
                return;
            case 12: mov(eax, ecx); and_(eax, edx); return;
            case 13: mov(eax, ecx); and_(eax, edx); not_(eax); return;
            case 14: mov(eax, ecx); or_(eax, edx); return;
            case 15: mov(eax, ecx); xor_(eax, edx); return;
            case 16: {
                const Word dest = (imm >> 10) & 63, bit_count = (imm >> 5) & 31;
                const Word source = imm & 31;
                if (dest >= 32) {
                    jmp(fail_merge, T_NEAR);
                    return;
                }
                const Word bits_mask = (1U << bit_count) - 1;
                mov(eax, ecx);
                and_(eax, ~(bits_mask << dest));
                shr(edx, source);
                and_(edx, bits_mask);
                shl(edx, dest);
                or_(eax, edx);
                return;
            }
            case 17: case 18: case 19: case 20: case 21:
                Compare(op);
                neg(eax);
                return;
            case 22:
                lea(eax, ptr[rcx + rdx]);
                and_(eax, 0xffff);
                mov(arg1.cvt32(), eax);
                mov(arg0, st);
                call(ptr[st + offsetof(State, read)]);
                return;
            case 23: {
                if (imm < 2) {
                    jmp(fail_loop, T_NEAR);
                    return;
                }
                Xbyak::Label skip, done;
                cmp(dword[st + offsetof(State, loop_count)], 0);
                jne(fail_loop, T_NEAR);
                test(ecx, ecx);
                jz(skip);
                dec(ecx);
                mov(dword[st + offsetof(State, loop_count)], ecx);
                mov(dword[st + offsetof(State, loop_start)], pc);
                mov(dword[st + offsetof(State, loop_end)], pc + imm - 1);
                jmp(done);
                L(skip);
                mov(dword[st + offsetof(State, next_pc)], pc + imm);
                L(done);
                xor_(eax, eax);
                return;
            }
            case 24:
                mov(dword[st + offsetof(State, next_pc)], pc + (imm & 0x7fff));
                xor_(eax, eax);
                return;
            case 25: case 26: case 27: case 28: case 29: {
                Xbyak::Label not_taken;
                Compare(op);
                cmp(eax, (imm & 0x8000) ? 1 : 0);
                jne(not_taken);
                const auto offset = std::int32_t(imm << 19) >> 19;
                if (offset == -4096) {
                    mov(byte[st + offsetof(State, stopped)], 1);
                } else {
                    mov(dword[st + offsetof(State, next_pc)], Word(std::int32_t(pc) + offset));
                    mov(byte[st + offsetof(State, cancel_end)], (imm & 0x4000) ? 1 : 0);
                }
                L(not_taken);
                xor_(eax, eax);
                return;
            }
            case 30:
                cmp(ecx, 0xc00);
                jae(fail_dread, T_NEAR);
                mov(rax, qword[st + offsetof(State, dram)]);
                mov(eax, dword[rax + rcx * 4]);
                return;
            case 31:
                cmp(ecx, 0xc00);
                jae(fail_dwrite, T_NEAR);
                mov(rax, qword[st + offsetof(State, dram)]);
                mov(dword[rax + rcx * 4], edx);
                xor_(eax, eax);
                return;
            default:
                xor_(eax, eax);
                return;
            }
        }

        std::vector<Xbyak::Label> labels;
        std::vector<bool> loop_end, after_end;
        Xbyak::Label table, dispatch, success, leave;
        Xbyak::Label fail_underflow, fail_source, fail_destination, fail_extended, fail_merge,
            fail_loop, fail_dread, fail_dwrite, fail_output, fail_emit, fail_outside, fail_steps;
    };

    std::unique_ptr<Program> program;
    std::uint64_t compiled_generation{~std::uint64_t{0}};
    std::uint32_t running{};
};

} // namespace Tegra
