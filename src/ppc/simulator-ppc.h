// Copyright 2012 the V8 project authors. All rights reserved.
//
// Copyright IBM Corp. 2012, 2013. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


// Declares a Simulator for PPC instructions if we are not generating a native
// PPC binary. This Simulator allows us to run and debug PPC code generation on
// regular desktop machines.
// V8 calls into generated code by "calling" the CALL_GENERATED_CODE macro,
// which will start execution in the Simulator or forwards to the real entry
// on a PPC HW platform.

#ifndef V8_PPC_SIMULATOR_PPC_H_
#define V8_PPC_SIMULATOR_PPC_H_

#include "allocation.h"

#if !defined(USE_SIMULATOR)
// Running without a simulator on a native ppc platform.

namespace v8 {
namespace internal {

// When running without a simulator we call the entry directly.
#define CALL_GENERATED_CODE(entry, p0, p1, p2, p3, p4) \
  (entry(p0, p1, p2, p3, p4))

typedef int (*ppc_regexp_matcher)(String*, int, const byte*, const byte*,
                                  int*, int, Address, int, void*, Isolate*);


// Call the generated regexp code directly. The code at the entry address
// should act as a function matching the type ppc_regexp_matcher.
// The ninth argument is a dummy that reserves the space used for
// the return address added by the ExitFrame in native calls.
#define CALL_GENERATED_REGEXP_CODE(entry, p0, p1, p2, p3, p4, p5, p6, p7, p8) \
  (FUNCTION_CAST<ppc_regexp_matcher>(entry)(                              \
      p0, p1, p2, p3, p4, p5, p6, p7, NULL, p8))

#define TRY_CATCH_FROM_ADDRESS(try_catch_address) \
  reinterpret_cast<TryCatch*>(try_catch_address)

// The stack limit beyond which we will throw stack overflow errors in
// generated code. Because generated code on ppc uses the C stack, we
// just use the C stack limit.
class SimulatorStack : public v8::internal::AllStatic {
 public:
  static inline uintptr_t JsLimitFromCLimit(v8::internal::Isolate* isolate,
                                            uintptr_t c_limit) {
    USE(isolate);
    return c_limit;
  }

  static inline uintptr_t RegisterCTryCatch(uintptr_t try_catch_address) {
    return try_catch_address;
  }

  static inline void UnregisterCTryCatch() { }
};

} }  // namespace v8::internal

#else  // !defined(USE_SIMULATOR)
// Running with a simulator.

#include "constants-ppc.h"
#include "hashmap.h"
#include "assembler.h"

namespace v8 {
namespace internal {

class CachePage {
 public:
  static const int LINE_VALID = 0;
  static const int LINE_INVALID = 1;

  static const int kPageShift = 12;
  static const int kPageSize = 1 << kPageShift;
  static const int kPageMask = kPageSize - 1;
  static const int kLineShift = 2;  // The cache line is only 4 bytes right now.
  static const int kLineLength = 1 << kLineShift;
  static const int kLineMask = kLineLength - 1;

  CachePage() {
    memset(&validity_map_, LINE_INVALID, sizeof(validity_map_));
  }

  char* ValidityByte(int offset) {
    return &validity_map_[offset >> kLineShift];
  }

  char* CachedData(int offset) {
    return &data_[offset];
  }

 private:
  char data_[kPageSize];   // The cached data.
  static const int kValidityMapSize = kPageSize >> kLineShift;
  char validity_map_[kValidityMapSize];  // One byte per line.
};


class Simulator {
 public:
  friend class PPCDebugger;
  enum Register {
    no_reg = -1,
    r0 = 0, sp, r2, r3, r4, r5, r6, r7,
    r8, r9, r10, r11, r12, r13, r14, r15,
    r16, r17, r18, r19, r20, r21, r22, r23,
    r24, r25, r26, r27, r28, r29, r30, fp,
    kNumGPRs = 32,
    d0 = 0, d1, d2, d3, d4, d5, d6, d7,
    d8, d9, d10, d11, d12, d13, d14, d15,
    d16, d17, d18, d19, d20, d21, d22, d23,
    d24, d25, d26, d27, d28, d29, d30, d31,
    kNumFPRs = 32
  };

  explicit Simulator(Isolate* isolate);
  ~Simulator();

  // The currently executing Simulator instance. Potentially there can be one
  // for each native thread.
  static Simulator* current(v8::internal::Isolate* isolate);

  // Accessors for register state.
  void set_register(int reg, intptr_t value);
  intptr_t get_register(int reg) const;
  double get_double_from_register_pair(int reg);
  void set_d_register_from_double(int dreg, const double dbl) {
    ASSERT(dreg >= 0 && dreg < kNumFPRs);
    fp_register[dreg] = dbl;
  }
  double get_double_from_d_register(int dreg) {
    return fp_register[dreg];
  }

  // Special case of set_register and get_register to access the raw PC value.
  void set_pc(intptr_t value);
  intptr_t get_pc() const;

  // Accessor to the internal simulator stack area.
  uintptr_t StackLimit() const;

  // Executes PPC instructions until the PC reaches end_sim_pc.
  void Execute();

  // Call on program start.
  static void Initialize(Isolate* isolate);

  // V8 generally calls into generated JS code with 5 parameters and into
  // generated RegExp code with 7 parameters. This is a convenience function,
  // which sets up the simulator state and grabs the result on return.
  intptr_t Call(byte* entry, int argument_count, ...);

  // Push an address onto the JS stack.
  uintptr_t PushAddress(uintptr_t address);

  // Pop an address from the JS stack.
  uintptr_t PopAddress();

  // Debugger input.
  void set_last_debugger_input(char* input);
  char* last_debugger_input() { return last_debugger_input_; }

  // ICache checking.
  static void FlushICache(v8::internal::HashMap* i_cache, void* start,
                          size_t size);

  // Returns true if pc register contains one of the 'special_values' defined
  // below (bad_lr, end_sim_pc).
  bool has_bad_pc() const;

 private:
  enum special_values {
    // Known bad pc value to ensure that the simulator does not execute
    // without being properly setup.
    bad_lr = -1,
    // A pc value used to signal the simulator to stop execution.  Generally
    // the lr is set to this value on transition from native C code to
    // simulated execution, so that the simulator can "return" to the native
    // C code.
    end_sim_pc = -2
  };

  // Unsupported instructions use Format to print an error and stop execution.
  void Format(Instruction* instr, const char* format);

  // Helper functions to set the conditional flags in the architecture state.
  bool CarryFrom(int32_t left, int32_t right, int32_t carry = 0);
  bool BorrowFrom(int32_t left, int32_t right);
  bool OverflowFrom(int32_t alu_out,
                    int32_t left,
                    int32_t right,
                    bool addition);

  // Helper functions to decode common "addressing" modes
  int32_t GetShiftRm(Instruction* instr, bool* carry_out);
  int32_t GetImm(Instruction* instr, bool* carry_out);
  void ProcessPUW(Instruction* instr,
                  int num_regs,
                  int operand_size,
                  intptr_t* start_address,
                  intptr_t* end_address);
  void HandleRList(Instruction* instr, bool load);
  void HandleVList(Instruction* inst);
  void SoftwareInterrupt(Instruction* instr);

  // Stop helper functions.
  inline bool isStopInstruction(Instruction* instr);
  inline bool isWatchedStop(uint32_t bkpt_code);
  inline bool isEnabledStop(uint32_t bkpt_code);
  inline void EnableStop(uint32_t bkpt_code);
  inline void DisableStop(uint32_t bkpt_code);
  inline void IncreaseStopCounter(uint32_t bkpt_code);
  void PrintStopInfo(uint32_t code);

  // Read and write memory.
  inline uint8_t ReadBU(intptr_t addr);
  inline int8_t ReadB(intptr_t addr);
  inline void WriteB(intptr_t addr, uint8_t value);
  inline void WriteB(intptr_t addr, int8_t value);

  inline uint16_t ReadHU(intptr_t addr, Instruction* instr);
  inline int16_t ReadH(intptr_t addr, Instruction* instr);
  // Note: Overloaded on the sign of the value.
  inline void WriteH(intptr_t addr, uint16_t value, Instruction* instr);
  inline void WriteH(intptr_t addr, int16_t value, Instruction* instr);

  inline uint32_t ReadWU(intptr_t addr, Instruction* instr);
  inline int32_t ReadW(intptr_t addr, Instruction* instr);
  inline void WriteW(intptr_t addr, uint32_t value, Instruction* instr);
  inline void WriteW(intptr_t addr, int32_t value, Instruction* instr);

  intptr_t* ReadDW(intptr_t addr);
  void WriteDW(intptr_t addr, int64_t value);

  // PowerPC
  void SetCR0(intptr_t result, bool setSO = false);
  void DecodeBranchConditional(Instruction* instr);
  void DecodeExt1(Instruction* instr);
  bool DecodeExt2_10bit(Instruction* instr);
  bool DecodeExt2_9bit_part1(Instruction* instr);
  void DecodeExt2_9bit_part2(Instruction* instr);
  void DecodeExt2(Instruction* instr);
  void DecodeExt4(Instruction* instr);
#if V8_TARGET_ARCH_PPC64
  void DecodeExt5(Instruction* instr);
#endif

  // Executes one instruction.
  void InstructionDecode(Instruction* instr);

  // ICache.
  static void CheckICache(v8::internal::HashMap* i_cache, Instruction* instr);
  static void FlushOnePage(v8::internal::HashMap* i_cache, intptr_t start,
                           int size);
  static CachePage* GetCachePage(v8::internal::HashMap* i_cache, void* page);

  // Runtime call support.
  static void* RedirectExternalReference(
      void* external_function,
      v8::internal::ExternalReference::Type type);

  // For use in calls that take double value arguments.
  void GetFpArgs(double* x, double* y);
  void GetFpArgs(double* x);
  void GetFpArgs(double* x, intptr_t* y);
  void SetFpResult(const double& result);
  void TrashCallerSaveRegisters();

  template<class ReturnType, int register_size>
      ReturnType GetFromFPRegister(int reg_index);

  template<class InputType, int register_size>
      void SetFPRegister(int reg_index, const InputType& value);

  // Architecture state.
  // Saturating instructions require a Q flag to indicate saturation.
  // There is currently no way to read the CPSR directly, and thus read the Q
  // flag, so this is left unimplemented.
  intptr_t registers_[kNumGPRs];  // PowerPC
  int32_t condition_reg_;  // PowerPC
  int32_t fp_condition_reg_;  // PowerPC
  intptr_t special_reg_lr_;  // PowerPC
  intptr_t special_reg_pc_;  // PowerPC
  intptr_t special_reg_ctr_;  // PowerPC
  int32_t special_reg_xer_;  // PowerPC

  double fp_register[kNumFPRs];

  // Simulator support.
  char* stack_;
  bool pc_modified_;
  int icount_;

  // Debugger input.
  char* last_debugger_input_;

  // Icache simulation
  v8::internal::HashMap* i_cache_;

  // Registered breakpoints.
  Instruction* break_pc_;
  Instr break_instr_;

  v8::internal::Isolate* isolate_;

  // A stop is watched if its code is less than kNumOfWatchedStops.
  // Only watched stops support enabling/disabling and the counter feature.
  static const uint32_t kNumOfWatchedStops = 256;

  // Breakpoint is disabled if bit 31 is set.
  static const uint32_t kStopDisabledBit = 1 << 31;

  // A stop is enabled, meaning the simulator will stop when meeting the
  // instruction, if bit 31 of watched_stops[code].count is unset.
  // The value watched_stops[code].count & ~(1 << 31) indicates how many times
  // the breakpoint was hit or gone through.
  struct StopCountAndDesc {
    uint32_t count;
    char* desc;
  };
  StopCountAndDesc watched_stops[kNumOfWatchedStops];
};


// When running with the simulator transition into simulated execution at this
// point.
#define CALL_GENERATED_CODE(entry, p0, p1, p2, p3, p4) \
  reinterpret_cast<Object*>(Simulator::current(Isolate::Current())->Call( \
                              FUNCTION_ADDR(entry), 5,                  \
                              (intptr_t)p0,                             \
                              (intptr_t)p1,                             \
                              (intptr_t)p2,                             \
                              (intptr_t)p3,                             \
                              (intptr_t)p4))

#define CALL_GENERATED_REGEXP_CODE(entry, p0, p1, p2, p3, p4, p5, p6, p7, p8) \
  Simulator::current(Isolate::Current())->Call(                         \
    entry, 10,                                                          \
    (intptr_t)p0,                                                       \
    (intptr_t)p1,                                                       \
    (intptr_t)p2,                                                       \
    (intptr_t)p3,                                                       \
    (intptr_t)p4,                                                       \
    (intptr_t)p5,                                                       \
    (intptr_t)p6,                                                       \
    (intptr_t)p7,                                                       \
    (intptr_t)NULL,                                                     \
    (intptr_t)p8)

#define TRY_CATCH_FROM_ADDRESS(try_catch_address)                              \
  try_catch_address == NULL ?                                                  \
      NULL : *(reinterpret_cast<TryCatch**>(try_catch_address))


// The simulator has its own stack. Thus it has a different stack limit from
// the C-based native code.  Setting the c_limit to indicate a very small
// stack cause stack overflow errors, since the simulator ignores the input.
// This is unlikely to be an issue in practice, though it might cause testing
// trouble down the line.
class SimulatorStack : public v8::internal::AllStatic {
 public:
  static inline uintptr_t JsLimitFromCLimit(v8::internal::Isolate* isolate,
                                            uintptr_t c_limit) {
    return Simulator::current(isolate)->StackLimit();
  }

  static inline uintptr_t RegisterCTryCatch(uintptr_t try_catch_address) {
    Simulator* sim = Simulator::current(Isolate::Current());
    return sim->PushAddress(try_catch_address);
  }

  static inline void UnregisterCTryCatch() {
    Simulator::current(Isolate::Current())->PopAddress();
  }
};

} }  // namespace v8::internal

#endif  // !defined(USE_SIMULATOR)
#endif  // V8_PPC_SIMULATOR_PPC_H_
