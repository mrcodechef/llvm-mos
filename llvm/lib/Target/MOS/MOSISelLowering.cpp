//===-- MOSISelLowering.cpp - MOS DAG Lowering Implementation -------------===//
//
// Part of LLVM-MOS, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the interfaces that MOS uses to lower LLVM code into a
// selection DAG.
//
//===----------------------------------------------------------------------===//

#include "MOSISelLowering.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/ErrorHandling.h"

#include "MCTargetDesc/MOSMCTargetDesc.h"
#include "MOS.h"
#include "MOSRegisterInfo.h"
#include "MOSSubtarget.h"
#include "MOSTargetMachine.h"

using namespace llvm;

MOSTargetLowering::MOSTargetLowering(const MOSTargetMachine &TM,
                                     const MOSSubtarget &STI)
    : TargetLowering(TM) {
  // This is only used for CallLowering to determine how to split large
  // primitive types for the calling convention. All need to be split to 8 bits,
  // so that's all that we report here. The register class is irrelevant.
  addRegisterClass(MVT::i8, &MOS::Anyi8RegClass);
  computeRegisterProperties(STI.getRegisterInfo());

  // The memset intrinsic takes an char, while the C memset takes an int. These
  // are different in the MOS calling convention, since arguments are not
  // automatically promoted to int. "memset" is the C version, and "__memset" is
  // the intrinsic version.
  setLibcallName(RTLIB::MEMSET, "__memset");

  setLibcallName(RTLIB::UDIVREM_I8, "__udivmodqi4");
  setLibcallName(RTLIB::UDIVREM_I16, "__udivmodhi4");
  setLibcallName(RTLIB::UDIVREM_I32, "__udivmodsi4");
  setLibcallName(RTLIB::UDIVREM_I64, "__udivmoddi4");
  setLibcallName(RTLIB::SDIVREM_I8, "__divmodqi4");
  setLibcallName(RTLIB::SDIVREM_I16, "__divmodhi4");
  setLibcallName(RTLIB::SDIVREM_I32, "__divmodsi4");
  setLibcallName(RTLIB::SDIVREM_I64, "__divmoddi4");

  // Used in legalizer (etc.) to refer to the stack pointer.
  setStackPointerRegisterToSaveRestore(MOS::RS0);

  setMaximumJumpTableSize(std::min(256u, getMaximumJumpTableSize()));
}

MVT MOSTargetLowering::getRegisterTypeForCallingConv(
    LLVMContext &Context, CallingConv::ID CC, EVT VT,
    const ISD::ArgFlagsTy &Flags) const {
  if (Flags.isPointer())
    return MVT::i16;
  return TargetLowering::getRegisterTypeForCallingConv(Context, CC, VT, Flags);
}

unsigned MOSTargetLowering::getNumRegistersForCallingConv(
    LLVMContext &Context, CallingConv::ID CC, EVT VT,
    const ISD::ArgFlagsTy &Flags) const {
  if (Flags.isPointer())
    return 1;
  return TargetLowering::getNumRegistersForCallingConv(Context, CC, VT, Flags);
}

unsigned MOSTargetLowering::getNumRegistersForInlineAsm(LLVMContext &Context,
                                                        EVT VT) const {
  // 16-bit inputs and outputs must be passed in Imag16 registers to allow using
  // pointer values in inline assembly.
  if (VT == MVT::i16)
    return 1;
  return TargetLowering::getNumRegistersForInlineAsm(Context, VT);
}

TargetLowering::ConstraintType
MOSTargetLowering::getConstraintType(StringRef Constraint) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    default:
      break;
    case 'a':
    case 'x':
    case 'y':
    case 'd':
    case 'c':
    case 'v':
      return C_Register;
    case 'R':
      return C_RegisterClass;
    }
  }
  return TargetLowering::getConstraintType(Constraint);
}

std::pair<unsigned, const TargetRegisterClass *>
MOSTargetLowering::getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                                                StringRef Constraint,
                                                MVT VT) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    default:
      break;
    case 'r':
      if (VT == MVT::i16)
        return std::make_pair(0U, &MOS::Imag16RegClass);
      return std::make_pair(0U, &MOS::Imag8RegClass);
    case 'R':
      return std::make_pair(0U, &MOS::GPRRegClass);
    case 'a':
      return std::make_pair(MOS::A, &MOS::GPRRegClass);
    case 'x':
      return std::make_pair(MOS::X, &MOS::GPRRegClass);
    case 'y':
      return std::make_pair(MOS::Y, &MOS::GPRRegClass);
    case 'd':
      return std::make_pair(0U, &MOS::XYRegClass);
    case 'c':
      return std::make_pair(MOS::C, &MOS::FlagRegClass);
    case 'v':
      return std::make_pair(MOS::V, &MOS::FlagRegClass);
    }
  }
  if (Constraint == "{cc}")
    return std::make_pair(MOS::P, &MOS::PcRegClass);

  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

bool is8BitIndex(Type *Ty) {
  if (!Ty)
    return false;
  return Ty == Type::getInt8Ty(Ty->getContext());
}

bool MOSTargetLowering::isLegalAddressingMode(const DataLayout &DL,
                                              const AddrMode &AM, Type *Ty,
                                              unsigned AddrSpace,
                                              Instruction *I) const {
  if (AM.Scale > 1 || AM.Scale < 0)
    return false;

  if (AM.Scale) {
    assert(AM.Scale == 1);
    if (!AM.HasBaseReg) {
      // Indexed addressing mode.
      if (is8BitIndex(AM.ScaleType))
        return true;

      // Consider a reg + 8-bit offset selectable via the indirect indexed
      // addressing mode.
      return !AM.BaseGV && 0 <= AM.BaseOffs && AM.BaseOffs < 256;
    }

    // Indirect indexed addressing mode: 16-bit register + 8-bit index register.
    // Doesn't matter which is 8-bit and which is 16-bit.
    return !AM.BaseGV && !AM.BaseOffs &&
           (is8BitIndex(AM.BaseType) || is8BitIndex(AM.ScaleType));
  }

  if (AM.HasBaseReg) {
    // Indexed addressing mode.
    if (is8BitIndex(AM.BaseType))
      return true;

    // Consider an reg + 8-bit offset selectable via the indirect indexed
    // addressing mode.
    return !AM.BaseGV && 0 <= AM.BaseOffs && AM.BaseOffs < 256;
  }

  // Any other combination of GV and BaseOffset are just global offsets.
  return true;
}

bool MOSTargetLowering::isTruncateFree(Type *SrcTy, Type *DstTy) const {
  if (!SrcTy->isIntegerTy() || !DstTy->isIntegerTy())
    return false;
  return SrcTy->getPrimitiveSizeInBits() > DstTy->getPrimitiveSizeInBits();
}

bool MOSTargetLowering::isZExtFree(Type *SrcTy, Type *DstTy) const {
  if (!SrcTy->isIntegerTy() || !DstTy->isIntegerTy())
    return false;
  return SrcTy->getPrimitiveSizeInBits() < DstTy->getPrimitiveSizeInBits();
}

static MachineBasicBlock *emitSelectImm(MachineInstr &MI,
                                        MachineBasicBlock *MBB);
static MachineBasicBlock *emitIncDecMB(MachineInstr &MI,
                                       MachineBasicBlock *MBB);
static MachineBasicBlock *emitCMPTermZMB(MachineInstr &MI,
                                         MachineBasicBlock *MBB);

MachineBasicBlock *
MOSTargetLowering::EmitInstrWithCustomInserter(MachineInstr &MI,
                                               MachineBasicBlock *MBB) const {
  switch (MI.getOpcode()) {
  default:
    llvm_unreachable("Bad opcode.");
  case MOS::SelectImm:
    return emitSelectImm(MI, MBB);
  case MOS::IncMB:
  case MOS::DecMB:
    return emitIncDecMB(MI, MBB);
  case MOS::CMPTermZMB:
    return emitCMPTermZMB(MI, MBB);
  }
}

static MachineBasicBlock *emitSelectImm(MachineInstr &MI,
                                        MachineBasicBlock *MBB) {
  // To "insert" Select* instructions, we actually have to insert the triangle
  // control-flow pattern.  The incoming instructions know the destination reg
  // to set, the flag to branch on, and the true/false values to select between.
  //
  // We produce the following control flow if the flag is neither N nor Z:
  //     HeadMBB
  //     |  \
  //     |  IfFalseMBB
  //     | /
  //    TailMBB
  //
  // If the flag is N or Z, then loading the true value in HeadMBB would clobber
  // the flag before the branch. We instead emit the following:
  //     HeadMBB
  //     |  \
  //     |  IfTrueMBB
  //     |      |
  //    IfFalse |
  //     |     /
  //     |    /
  //     TailMBB
  Register Dst = MI.getOperand(0).getReg();
  Register Flag = MI.getOperand(1).getReg();
  int64_t TrueValue = MI.getOperand(2).getImm();
  int64_t FalseValue = MI.getOperand(3).getImm();

  const BasicBlock *LLVM_BB = MBB->getBasicBlock();
  MachineFunction::iterator I = ++MBB->getIterator();
  MachineIRBuilder Builder(*MBB, MI);

  MachineBasicBlock *HeadMBB = MBB;
  MachineFunction *F = MBB->getParent();

  const MOSSubtarget &STI = F->getSubtarget<MOSSubtarget>();

  // Split out all instructions after MI into a new basic block, updating
  // liveins.
  MachineBasicBlock *TailMBB = HeadMBB->splitAt(MI);

  // If MI is the last instruction, splitAt won't insert a new block. In that
  // case, the block must fall through, since there's no branch. Thus the tail
  // MBB is just the next MBB.
  if (TailMBB == HeadMBB)
    TailMBB = &*I;

  HeadMBB->removeSuccessor(TailMBB);

  // Add the false block between HeadMBB and TailMBB
  MachineBasicBlock *IfFalseMBB = F->CreateMachineBasicBlock(LLVM_BB);
  F->insert(TailMBB->getIterator(), IfFalseMBB);
  HeadMBB->addSuccessor(IfFalseMBB);
  for (const auto &LiveIn : TailMBB->liveins())
    if (LiveIn.PhysReg != Dst)
      IfFalseMBB->addLiveIn(LiveIn);
  IfFalseMBB->addSuccessor(TailMBB);

  // Add a true block if necessary to avoid clobbering NZ.
  MachineBasicBlock *IfTrueMBB = nullptr;
  if (Flag == MOS::N || Flag == MOS::Z) {
    IfTrueMBB = F->CreateMachineBasicBlock(LLVM_BB);
    F->insert(TailMBB->getIterator(), IfTrueMBB);
    IfTrueMBB->addSuccessor(TailMBB);

    // Add the unconditional branch from IfFalseMBB to TailMBB.
    Builder.setInsertPt(*IfFalseMBB, IfFalseMBB->begin());
    Builder.buildInstr(STI.has65C02() ? MOS::BRA : MOS::JMP).addMBB(TailMBB);
    for (const auto &LiveIn : IfFalseMBB->liveins())
      IfTrueMBB->addLiveIn(LiveIn);

    Builder.setInsertPt(*HeadMBB, MI.getIterator());
  }

  const auto LDImm = [&Builder, &Dst](int64_t Val) {
    if (MOS::CV_GPR_LSBRegClass.contains(Dst)) {
      Builder.buildInstr(MOS::LDImm1, {Dst}, {Val});
      return;
    }

    assert(MOS::GPRRegClass.contains(Dst));
    Builder.buildInstr(MOS::LDImm, {Dst}, {Val});
  };

  if (IfTrueMBB) {
    // Insert branch.
    Builder.buildInstr(MOS::BR).addMBB(IfTrueMBB).addUse(Flag).addImm(1);
    HeadMBB->addSuccessor(IfTrueMBB);

    Builder.setInsertPt(*IfTrueMBB, IfTrueMBB->begin());
    // Load true value.
    LDImm(TrueValue);
  } else {
    // Load true value.
    LDImm(TrueValue);

    // Insert branch.
    Builder.buildInstr(MOS::BR).addMBB(TailMBB).addUse(Flag).addImm(1);
    HeadMBB->addSuccessor(TailMBB);
  }

  // Insert false load.
  Builder.setInsertPt(*IfFalseMBB, IfFalseMBB->begin());
  LDImm(FalseValue);

  MI.eraseFromParent();

  return TailMBB;
}

// Returns an IncMB that is safe to fold into the given CMPTermZ.
static MachineInstr *findCMPTermZMBInc(MachineInstr &MI) {
  const TargetRegisterInfo *TRI = MI.getMF()->getSubtarget().getRegisterInfo();
  for (auto I = MachineBasicBlock::reverse_iterator(MI.getIterator()),
            E = MI.getParent()->rend();
       I != E; ++I) {
    if (I->hasUnmodeledSideEffects() || I->isCall())
      return nullptr;
    bool ReferencesCmpReg = false;
    for (const MachineOperand &MO : MI.explicit_uses()) {
      if (I->readsRegister(MO.getReg(), TRI) ||
          I->definesRegister(MO.getReg(), TRI)) {
        ReferencesCmpReg = true;
        break;
      }
    }
    if (!ReferencesCmpReg)
      continue;
    if (I->getOpcode() != MOS::IncMB)
      return nullptr;
    for (auto IdxMO : enumerate(MI.explicit_uses()))
      if (I->getOperand(IdxMO.index()).getReg() != IdxMO.value().getReg())
        return nullptr;
    return &*I;
  }
  return nullptr;
}

static MachineBasicBlock *emitIncDecMB(MachineInstr &MI,
                                       MachineBasicBlock *MBB) {
  if (!MBB->getParent()->getProperties().hasProperty(
          MachineFunctionProperties::Property::NoVRegs))
    return MBB;

  // If this instruction will be folded into a later CMPTermZMB, then defer
  // expanding it.
  if (MI.getOpcode() == MOS::IncMB && MI.getNumExplicitDefs() > 1) {
    auto Term = MBB->getFirstTerminator();
    if (Term != MBB->end() && Term->getOpcode() == MOS::CMPTermZMB &&
        findCMPTermZMBInc(*Term) == &MI) {
      if (std::prev(Term) != MI) {
        // The expansion of an intervening multi-byte instruction could separate
        // the IncMB from its CMPTermZMB, so move it right before the
        // CMPTermZMB. This is guaranteed safe by findCMPTermZMBInc.
        MBB->insert(Term, MI.removeFromParent());
      }
      return MBB;
    }
  }

  MachineIRBuilder Builder(MI);
  bool IsDec = MI.getOpcode() == MOS::DecMB;
  assert(IsDec || MI.getOpcode() == MOS::IncMB);
  unsigned FirstUseIdx = MI.getNumExplicitDefs();
  unsigned FirstDefIdx = IsDec ? 1 : 0;
  if (IsDec && FirstUseIdx < MI.getNumExplicitOperands() - 1) {
    if (MI.getOperand(FirstUseIdx).isReg()) {
      Builder.buildCopy(MI.getOperand(0), MI.getOperand(FirstUseIdx));
    } else {
      Builder.buildInstr(MOS::LDAbs)
          .addDef(MI.getOperand(0).getReg())
          .add(MI.getOperand(FirstUseIdx));
    }
  }
  MachineInstrBuilder First;
  if (MI.getOperand(FirstUseIdx).isReg()) {
    First = Builder.buildInstr(IsDec ? MOS::DEC : MOS::INC)
                .add(MI.getOperand(FirstDefIdx))
                .add(MI.getOperand(FirstUseIdx));
    ++FirstDefIdx;
  } else {
    First = Builder.buildInstr(IsDec ? MOS::DECAbs : MOS::INCAbs)
                .add(MI.getOperand(FirstUseIdx));
  }
  if (FirstUseIdx == MI.getNumExplicitOperands() - 1) {
    MI.eraseFromParent();
    return MBB;
  }

  if (IsDec)
    Builder.buildInstr(MOS::CMPImm)
        .addDef(MOS::C)
        .addUse(MI.getOperand(0).getReg())
        .addImm(INT64_C(0))
        .addDef(MOS::Z, RegState::Implicit);
  else
    First.addDef(MOS::Z, RegState::Implicit);

  MachineBasicBlock *TailMBB = MBB->splitAt(MI);
  // If MI is the last instruction, splitAt won't insert a new block. In that
  // case, the block must fall through, since there's no branch. Thus the tail
  // MBB is just the next MBB.
  if (TailMBB == MBB)
    TailMBB = &*std::next(MBB->getIterator());

  MachineFunction *F = MBB->getParent();
  MachineBasicBlock *RestMBB = F->CreateMachineBasicBlock(MBB->getBasicBlock());
  F->insert(TailMBB->getIterator(), RestMBB);
  for (const auto &LiveIn : TailMBB->liveins())
    RestMBB->addLiveIn(LiveIn);

  Builder.buildInstr(MOS::BR).addMBB(RestMBB).addUse(MOS::Z).addImm(
      INT64_C(-1));
  Builder.buildInstr(MOS::JMP).addMBB(TailMBB);
  MBB->addSuccessor(RestMBB);

  Builder.setInsertPt(*RestMBB, RestMBB->end());
  auto Rest = Builder.buildInstr(MI.getOpcode());
  if (IsDec)
    Rest.addDef(MI.getOperand(0).getReg());
  for (unsigned I = FirstDefIdx, E = MI.getNumExplicitOperands(); I != E; ++I) {
    if (I == FirstUseIdx)
      continue;
    Rest.add(MI.getOperand(I));
    if (MI.getOperand(I).isReg())
      RestMBB->addLiveIn(MI.getOperand(I).getReg());
  }
  Builder.buildInstr(MOS::JMP).addMBB(TailMBB);
  RestMBB->addSuccessor(TailMBB);
  RestMBB->sortUniqueLiveIns();

  MI.eraseFromParent();

  return RestMBB;
}

static MachineBasicBlock *emitCMPTermZMB(MachineInstr &MI,
                                         MachineBasicBlock *MBB) {
  if (!MBB->getParent()->getProperties().hasProperty(
          MachineFunctionProperties::Property::NoVRegs))
    return MBB;
  const TargetInstrInfo &TII = *MI.getMF()->getSubtarget().getInstrInfo();

  if (MI.getNumExplicitOperands() - 1 == 1) {
    MI.setDesc(TII.get(MOS::CMPTermZ));
    return MBB;
  }

  MachineInstr *Inc = findCMPTermZMBInc(MI);

  SmallVector<MachineBasicBlock::RegisterMaskPair> LiveOuts;
  for (const MachineBasicBlock::RegisterMaskPair &P : MBB->liveouts())
    LiveOuts.push_back(P);

  MachineBasicBlock *TBB;
  MachineBasicBlock *FBB;
  SmallVector<MachineOperand> Cond;
  if (TII.analyzeBranch(*MBB, TBB, FBB, Cond))
    llvm_unreachable("Could not analyze branch.");
  assert(TBB && Cond.size() == 2 && "Expected conditional branch.");
  assert(Cond[0].getReg() == MOS::Z && "Must branch on Z.");
  if (Cond[1].getImm()) {
    TII.reverseBranchCondition(Cond);
    std::swap(TBB, FBB);
  }
  assert(!Cond[1].getImm());
  if (!TBB)
    TBB = MBB->getFallThrough();
  TII.removeBranch(*MBB);

  Register Reg;
  if (Inc) {
    Reg = MI.getOperand(1).getReg();
    MI.removeOperand(1);
  } else {
    Reg = MI.getOperand(MI.getNumExplicitOperands() - 1).getReg();
    MI.removeOperand(MI.getNumExplicitOperands() - 1);
  }
  MI.removeFromParent();

  MachineFunction &MF = *MBB->getParent();

  const BasicBlock *BB = MBB->getBasicBlock();
  auto MBBI = std::next(MBB->getIterator());
  MachineBasicBlock *NextMBB = MF.CreateMachineBasicBlock(BB);
  MF.insert(MBBI, NextMBB);
  NextMBB->transferSuccessors(MBB);
  for (const auto &P : LiveOuts)
    NextMBB->addLiveIn(P.PhysReg, P.LaneMask);
  for (MachineOperand &MO : MI.explicit_uses())
    NextMBB->addLiveIn(MO.getReg());
  NextMBB->sortUniqueLiveIns();

  MachineIRBuilder Builder(*MBB, MBB->end());
  if (Inc)
    Builder.buildInstr(MOS::INC, {Reg}, {Reg});
  auto Cmp = Builder.buildInstr(MOS::CMPTermZ, {MOS::C}, {Reg});
  Cmp->getOperand(0).setIsDead();
  TII.insertBranch(*MBB, TBB, nullptr, Cond, Builder.getDL());
  MBB->addSuccessor(TBB);
  MBB->addSuccessor(NextMBB);

  if (Inc) {
    Builder.setInsertPt(*NextMBB, NextMBB->end());
    auto NewInc = Builder.buildInstr(MOS::IncMB);
    for (unsigned I = 1, E = Inc->getNumExplicitDefs(); I != E; ++I)
      NewInc.addDef(Inc->getOperand(I).getReg());
    for (unsigned I = 1, E = Inc->getNumExplicitDefs(); I != E; ++I)
      NewInc.addUse(Inc->getOperand(I).getReg());
    Inc->eraseFromParent();
  }
  NextMBB->insert(NextMBB->end(), &MI);
  TII.insertBranch(*NextMBB, TBB, FBB, Cond, Builder.getDL());

  return NextMBB;
}
