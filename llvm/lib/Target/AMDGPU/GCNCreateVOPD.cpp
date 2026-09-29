//===- GCNCreateVOPD.cpp - Create VOPD Instructions ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Combine VALU pairs into VOPD instructions
/// Only works on wave32
/// Has register requirements, we reject creating VOPD if the requirements are
/// not met.
/// shouldCombineVOPD mutator in postRA machine scheduler puts candidate
/// instructions for VOPD back-to-back
///
//
//===----------------------------------------------------------------------===//

#include "AMDGPU.h"
#include "GCNSubtarget.h"
#include "GCNVOPDUtils.h"
#include "SIInstrInfo.h"
#include "Utils/AMDGPUBaseInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "gcn-create-vopd"
STATISTIC(NumVOPDCreated, "Number of VOPD Insts Created.");

using namespace llvm;

// Experimental local post-RA repair for the gfx12 inference pipeline. Keep the
// generic compiler unchanged unless the module build explicitly opts in.
static cl::opt<unsigned> DLSS5VOPDLookahead(
    "amdgpu-dlss5-vopd-lookahead", cl::Hidden, cl::init(0),
    cl::desc("Search up to N nearby instructions for independent gfx1200/1201 "
             "VOPD partners (0 preserves the normal adjacent-only pass)"));

namespace {

class GCNCreateVOPD {
private:
  class VOPDCombineInfo {
  public:
    VOPDCombineInfo() = default;
    VOPDCombineInfo(MachineInstr *First, MachineInstr *Second,
                    bool VOPD3 = false)
        : FirstMI(First), SecondMI(Second), IsVOPD3(VOPD3) {}

    MachineInstr *FirstMI;
    MachineInstr *SecondMI;
    bool IsVOPD3;
  };

public:
  const GCNSubtarget *ST = nullptr;

  // Move a later VALU instruction earlier only over ordinary, independent
  // register operations. Do not cross memory, matrix, MODE, EXEC changes,
  // inline assembly, bundles, or synchronization. Register queries include
  // implicit operands and physical subregister aliases.
  bool isOrdinaryRegisterOp(const SIInstrInfo &SII,
                            const MachineInstr &Crossed) const {
    if (Crossed.isMetaInstruction() || Crossed.isInlineAsm() ||
        Crossed.isBundle() || Crossed.isBundled() ||
        Crossed.isTerminator() || Crossed.isCall() ||
        Crossed.mayLoadOrStore() || Crossed.hasUnmodeledSideEffects() ||
        SII.isWMMA(Crossed) || SII.isMFMA(Crossed) ||
        (!SII.isSALU(Crossed) && !SII.isVALU(Crossed)))
      return false;
    StringRef Name = SII.getName(Crossed.getOpcode());
    if (Name.starts_with("S_GETREG") || Name.starts_with("S_SETREG") ||
        Name.starts_with("S_WAIT") || Name.starts_with("S_DELAY") ||
        Name.starts_with("S_NOP") || Name.starts_with("S_BARRIER"))
      return false;
    bool HasDef = false;
    for (const auto &Op : Crossed.operands())
      HasDef |= Op.isReg() && Op.isDef() && Op.getReg();
    return HasDef;
  }

  bool canCross(const SIInstrInfo &SII, const MachineInstr &Moving,
                const MachineInstr &Crossed) const {
    if (!isOrdinaryRegisterOp(SII, Crossed))
      return false;

    const TargetRegisterInfo *TRI = ST->getRegisterInfo();
    for (const auto &Op : Moving.operands()) {
      if (!Op.isReg() || !Op.getReg())
        continue;
      if (Op.isUse() && Crossed.modifiesRegister(Op.getReg(), TRI))
        return false; // RAW, including implicit EXEC/MODE.
      if (Op.isDef() && (Crossed.readsRegister(Op.getReg(), TRI) ||
                         Crossed.modifiesRegister(Op.getReg(), TRI)))
        return false; // WAR or WAW.
    }
    return true;
  }

  bool doReplace(const SIInstrInfo *SII, VOPDCombineInfo &CI) {
    auto *FirstMI = CI.FirstMI;
    auto *SecondMI = CI.SecondMI;
    unsigned Opc1 = FirstMI->getOpcode();
    unsigned Opc2 = SecondMI->getOpcode();
    unsigned EncodingFamily =
        AMDGPU::getVOPDEncodingFamily(SII->getSubtarget());
    int NewOpcode = AMDGPU::getVOPDFull(AMDGPU::getVOPDOpcode(Opc1, CI.IsVOPD3),
                                        AMDGPU::getVOPDOpcode(Opc2, CI.IsVOPD3),
                                        EncodingFamily, CI.IsVOPD3);
    assert(NewOpcode != -1 &&
           "Should have previously determined this as a possible VOPD\n");

    auto VOPDInst = BuildMI(*FirstMI->getParent(), FirstMI,
                            FirstMI->getDebugLoc(), SII->get(NewOpcode))
                        .setMIFlags(FirstMI->getFlags() | SecondMI->getFlags());

    namespace VOPD = AMDGPU::VOPD;
    MachineInstr *MI[] = {FirstMI, SecondMI};
    auto InstInfo =
        AMDGPU::getVOPDInstInfo(FirstMI->getDesc(), SecondMI->getDesc());

    for (auto CompIdx : VOPD::COMPONENTS) {
      auto MCOprIdx = InstInfo[CompIdx].getIndexOfDstInMCOperands();
      VOPDInst.add(MI[CompIdx]->getOperand(MCOprIdx));
    }

    const AMDGPU::OpName Mods[2][3] = {
        {AMDGPU::OpName::src0X_modifiers, AMDGPU::OpName::vsrc1X_modifiers,
         AMDGPU::OpName::vsrc2X_modifiers},
        {AMDGPU::OpName::src0Y_modifiers, AMDGPU::OpName::vsrc1Y_modifiers,
         AMDGPU::OpName::vsrc2Y_modifiers}};
    const AMDGPU::OpName SrcMods[3] = {AMDGPU::OpName::src0_modifiers,
                                       AMDGPU::OpName::src1_modifiers,
                                       AMDGPU::OpName::src2_modifiers};
    const unsigned VOPDOpc = VOPDInst->getOpcode();

    for (auto CompIdx : VOPD::COMPONENTS) {
      auto CompSrcOprNum = InstInfo[CompIdx].getCompSrcOperandsNum();
      bool IsVOP3 = SII->isVOP3(*MI[CompIdx]);
      for (unsigned CompSrcIdx = 0; CompSrcIdx < CompSrcOprNum; ++CompSrcIdx) {
        if (AMDGPU::hasNamedOperand(VOPDOpc, Mods[CompIdx][CompSrcIdx])) {
          const MachineOperand *Mod =
              SII->getNamedOperand(*MI[CompIdx], SrcMods[CompSrcIdx]);
          VOPDInst.addImm(Mod ? Mod->getImm() : 0);
        }
        auto MCOprIdx =
            InstInfo[CompIdx].getIndexOfSrcInMCOperands(CompSrcIdx, IsVOP3);
        VOPDInst.add(MI[CompIdx]->getOperand(MCOprIdx));
      }
      if (MI[CompIdx]->getOpcode() == AMDGPU::V_CNDMASK_B32_e32 && CI.IsVOPD3)
        VOPDInst.addReg(AMDGPU::VCC_LO);
    }

    if (CI.IsVOPD3) {
      if (unsigned BitOp2 = AMDGPU::getBitOp2(Opc2))
        VOPDInst.addImm(BitOp2);
    }

    SII->fixImplicitOperands(*VOPDInst);
    for (auto CompIdx : VOPD::COMPONENTS)
      VOPDInst.copyImplicitOps(*MI[CompIdx]);

    LLVM_DEBUG(dbgs() << "VOPD Fused: " << *VOPDInst << " from\tX: "
                      << *CI.FirstMI << "\tY: " << *CI.SecondMI << "\n");

    for (auto CompIdx : VOPD::COMPONENTS)
      MI[CompIdx]->eraseFromParent();

    ++NumVOPDCreated;
    return true;
  }

  bool run(MachineFunction &MF) {
    ST = &MF.getSubtarget<GCNSubtarget>();
    if (!AMDGPU::hasVOPD(*ST) || !ST->isWave32())
      return false;
    LLVM_DEBUG(dbgs() << "CreateVOPD Pass:\n");

    const SIInstrInfo *SII = ST->getInstrInfo();
    bool Changed = false;
    unsigned EncodingFamily = AMDGPU::getVOPDEncodingFamily(*ST);
    bool HasVOPD3 = ST->hasVOPD3();
    const unsigned Lookahead =
        (ST->getCPU() == "gfx1200" || ST->getCPU() == "gfx1201") &&
                !MF.getFunction().hasFnAttribute(Attribute::StrictFP)
            ? DLSS5VOPDLookahead.getValue()
            : 0;

    SmallVector<VOPDCombineInfo> ReplaceCandidates;

    for (auto &MBB : MF) {
      auto MII = MBB.begin(), E = MBB.end();
      while (MII != E) {
        auto *FirstMI = &*MII;
        MII = next_nodbg(MII, MBB.end());
        if (MII == MBB.end())
          break;
        if (FirstMI->isDebugInstr())
          continue;
        auto *SecondMI = &*MII;
        unsigned Opc = FirstMI->getOpcode();
        unsigned Opc2 = SecondMI->getOpcode();
        VOPDCombineInfo CI;

        const auto checkVOPD = [&](bool VOPD3) -> bool {
          llvm::AMDGPU::CanBeVOPD FirstCanBeVOPD =
              AMDGPU::getCanBeVOPD(Opc, EncodingFamily, VOPD3);
          llvm::AMDGPU::CanBeVOPD SecondCanBeVOPD =
              AMDGPU::getCanBeVOPD(Opc2, EncodingFamily, VOPD3);

          if (FirstCanBeVOPD.X && SecondCanBeVOPD.Y)
            CI = VOPDCombineInfo(FirstMI, SecondMI, VOPD3);
          else if (FirstCanBeVOPD.Y && SecondCanBeVOPD.X)
            CI = VOPDCombineInfo(SecondMI, FirstMI, VOPD3);
          else
            return false;
          // checkVOPDRegConstraints cares about program order, but doReplace
          // cares about X-Y order in the constituted VOPD
          return llvm::checkVOPDRegConstraints(*SII, *FirstMI, *SecondMI,
                                               VOPD3);
        };

        bool CanCombine = checkVOPD(false) || (HasVOPD3 && checkVOPD(true));
        if (!CanCombine && Lookahead && !FirstMI->isBundled()) {
          auto Candidate = std::next(MII);
          for (unsigned Distance = 2;
               Candidate != E && Distance <= Lookahead;
               ++Candidate, ++Distance) {
            auto *Later = &*Candidate;
            // A barrier also bounds the search, rather than merely rejecting
            // that particular possible partner.
            if (!isOrdinaryRegisterOp(*SII, *std::prev(Candidate)))
              break;
            if (Later->isDebugInstr() || Later->isBundled())
              break;
            SecondMI = Later;
            Opc2 = SecondMI->getOpcode();
            if (!(checkVOPD(false) || (HasVOPD3 && checkVOPD(true))))
              continue;
            bool Independent = true;
            for (auto I = MII; I != Candidate; ++I)
              Independent &= canCross(*SII, *SecondMI, *I);
            if (!Independent)
              continue;

            // The old kill markers describe the old order. Missing kills are
            // safe here; retaining a moved last-use marker would be wrong.
            for (auto &Op : SecondMI->operands())
              if (Op.isReg() && Op.isUse())
                Op.setIsKill(false);
            MBB.splice(MII, &MBB, Candidate);
            MII = SecondMI->getIterator();
            CanCombine = true;
            break;
          }
        }
        if (CanCombine) {
          ReplaceCandidates.push_back(CI);
          ++MII;
        }
      }
    }
    for (auto &CI : ReplaceCandidates) {
      Changed |= doReplace(SII, CI);
    }

    return Changed;
  }
};

class GCNCreateVOPDLegacy : public MachineFunctionPass {
public:
  static char ID;
  GCNCreateVOPDLegacy() : MachineFunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override {
    return "GCN Create VOPD Instructions";
  }
  bool runOnMachineFunction(MachineFunction &MF) override {
    if (skipFunction(MF.getFunction()))
      return false;

    return GCNCreateVOPD().run(MF);
  }
};

} // namespace

PreservedAnalyses
llvm::GCNCreateVOPDPass::run(MachineFunction &MF,
                             MachineFunctionAnalysisManager &AM) {
  if (!GCNCreateVOPD().run(MF))
    return PreservedAnalyses::all();
  return getMachineFunctionPassPreservedAnalyses().preserveSet<CFGAnalyses>();
}

char GCNCreateVOPDLegacy::ID = 0;

char &llvm::GCNCreateVOPDID = GCNCreateVOPDLegacy::ID;

INITIALIZE_PASS(GCNCreateVOPDLegacy, DEBUG_TYPE, "GCN Create VOPD Instructions",
                false, false)
