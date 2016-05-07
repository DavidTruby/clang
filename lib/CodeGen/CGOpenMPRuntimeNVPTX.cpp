//===---- CGOpenMPRuntimeNVPTX.cpp - Interface to OpenMP NVPTX Runtimes ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This provides a class for OpenMP runtime code generation specialized to NVPTX
// targets.
//
//===----------------------------------------------------------------------===//

#include "CGOpenMPRuntimeNVPTX.h"
#include "CGCleanup.h"
#include "CodeGenFunction.h"
#include "clang/AST/DeclOpenMP.h"
#include "clang/AST/StmtOpenMP.h"

using namespace clang;
using namespace CodeGen;

namespace {
enum OpenMPRTLFunctionNVPTX {
  /// \brief Call to void __kmpc_kernel_init(kmp_int32 omp_handle,
  /// kmp_int32 thread_limit);
  OMPRTL_NVPTX__kmpc_kernel_init,
  /// \brief Call to void __kmpc_kernel_deinit();
  OMPRTL_NVPTX__kmpc_kernel_deinit,
  // Call to void __kmpc_serialized_parallel(ident_t *loc, kmp_int32
  // global_tid);
  OMPRTL_NVPTX__kmpc_serialized_parallel,
  // Call to void __kmpc_end_serialized_parallel(ident_t *loc, kmp_int32
  // global_tid);
  OMPRTL_NVPTX__kmpc_end_serialized_parallel,
  /// \brief Call to void __kmpc_kernel_prepare_parallel(void
  /// *outlined_function);
  OMPRTL_NVPTX__kmpc_kernel_prepare_parallel,
  /// \brief Call to bool __kmpc_kernel_parallel(void **outlined_function);
  OMPRTL_NVPTX__kmpc_kernel_parallel,
  /// \brief Call to void __kmpc_kernel_end_parallel();
  OMPRTL_NVPTX__kmpc_kernel_end_parallel,
  /// \brief Call to bool __kmpc_kernel_convergent_parallel(void *buffer, bool
  /// *IsFinal, kmpc_int32 *LaneSource);
  OMPRTL_NVPTX__kmpc_kernel_convergent_parallel,
  /// \brief Call to void __kmpc_kernel_end_convergent_parallel(void *buffer);
  OMPRTL_NVPTX__kmpc_kernel_end_convergent_parallel,
  /// \brief Call to bool __kmpc_kernel_convergent_simd(
  /// void *buffer, bool *IsFinal, kmpc_int32 *LaneSource, kmpc_int32 *LaneId,
  /// kmpc_int32 *NumLanes);
  OMPRTL_NVPTX__kmpc_kernel_convergent_simd,
  /// \brief Call to void __kmpc_kernel_end_convergent_simd(void *buffer);
  OMPRTL_NVPTX__kmpc_kernel_end_convergent_simd,
  /// \brief Call to int32_t __kmpc_warp_active_thread_mask();
  OMPRTL_NVPTX__kmpc_warp_active_thread_mask,
  /// \brief Call to void
  /// __kmpc_initialize_data_sharing_environment(__kmpc_data_sharing_slot
  /// *RootS, size_t InitialDataSize);
  OMPRTL_NVPTX__kmpc_initialize_data_sharing_environment,
  /// \brief Call to void* __kmpc_data_sharing_environment_begin(
  /// __kmpc_data_sharing_slot **SavedSharedSlot, void **SavedSharedStack, void
  /// **SavedSharedFrame, int32_t *SavedActiveThreads, size_t SharingDataSize,
  /// size_t SharingDefaultDataSize, int32_t IsEntryPoint);
  OMPRTL_NVPTX__kmpc_data_sharing_environment_begin,
  /// \brief Call to void __kmpc_data_sharing_environment_end(
  /// __kmpc_data_sharing_slot **SavedSharedSlot, void **SavedSharedStack, void
  /// **SavedSharedFrame, int32_t *SavedActiveThreads);
  OMPRTL_NVPTX__kmpc_data_sharing_environment_end,
  /// \brief Call to void* __kmpc_get_data_sharing_environment_frame(int32_t
  /// SourceThreadID);
  OMPRTL_NVPTX__kmpc_get_data_sharing_environment_frame,

  //
  //  OMPRTL_NVPTX__kmpc_samuel_print
};

// NVPTX Address space
enum ADDRESS_SPACE {
  ADDRESS_SPACE_SHARED = 3,
};

enum STATE_SIZE {
  TASK_STATE_SIZE = 48,
  SIMD_STATE_SIZE = 48,
};

enum DATA_SHARING_SIZES {
  // The maximum number of workers in a kernel.
  DS_Max_Worker_Threads = 992,
  // The size reserved for data in a shared memory slot.
  DS_Slot_Size = 4,
  // The maximum number of threads in a worker warp.
  DS_Max_Worker_Warp_Size = 32,
  // The number of bits required to represent the maximum number of threads in a
  // warp.
  DS_Max_Worker_Warp_Size_Log2 = 5,
  DS_Max_Worker_Warp_Size_Log2_Mask =
      (~0u >> (32 - DS_Max_Worker_Warp_Size_Log2)),
  // The slot size that should be reserved for a working warp.
  DS_Worker_Warp_Slot_Size = DS_Max_Worker_Warp_Size * DS_Slot_Size,
  // the maximum number of teams.
  DS_Max_Teams = 1024
};

} // namespace

// \brief Return the address where the parallelism level is kept in shared
// memory for the current thread. It is assumed we have up to 992 parallel
// worker threads.
// FIXME: Make this value reside in a descriptor whose size is decided at
// runtime (extern shared memory). This can be used for the other thread
// specific state as well.
LValue
CGOpenMPRuntimeNVPTX::getParallelismLevelLValue(CodeGenFunction &CGF) const {
  auto &M = CGM.getModule();

  const char *Name = "__openmp_nvptx_parallelism_levels";
  llvm::GlobalVariable *Gbl = M.getGlobalVariable(Name);

  if (!Gbl) {
    auto *Ty = llvm::ArrayType::get(CGM.Int32Ty, DS_Max_Worker_Threads);
    Gbl = new llvm::GlobalVariable(
        M, Ty,
        /*isConstant=*/false, llvm::GlobalVariable::CommonLinkage,
        llvm::Constant::getNullValue(Ty), Name,
        /*InsertBefore=*/nullptr, llvm::GlobalVariable::NotThreadLocal,
        ADDRESS_SPACE_SHARED);
  }

  llvm::Value *Idx[] = {llvm::Constant::getNullValue(CGM.Int32Ty),
                        getNVPTXThreadID(CGF)};
  llvm::Value *AddrVal = CGF.Builder.CreateInBoundsGEP(Gbl, Idx);
  return CGF.MakeNaturalAlignAddrLValue(
      AddrVal, CGF.getContext().getIntTypeForBitwidth(/*DestWidth=*/32,
                                                      /*isSigned=*/true));
}

// \brief Return an integer with the parallelism level. Zero means that the
// current region is not enclosed in a parallel/simd region. The current level
// is kept in a shared memory array.
llvm::Value *
CGOpenMPRuntimeNVPTX::getParallelismLevel(CodeGenFunction &CGF) const {
  auto Addr = getParallelismLevelLValue(CGF);
  return CGF.EmitLoadOfLValue(Addr, SourceLocation()).getScalarVal();
}

// \brief Increase the value of parallelism level for the current thread.
void CGOpenMPRuntimeNVPTX::increaseParallelismLevel(CodeGenFunction &CGF,
                                                    bool IsSimd) const {
  unsigned Increment = IsSimd ? 10 : 1;
  auto Addr = getParallelismLevelLValue(CGF);
  auto *CurVal = CGF.EmitLoadOfLValue(Addr, SourceLocation()).getScalarVal();
  auto *NewVal =
      CGF.Builder.CreateNSWAdd(CurVal, CGF.Builder.getInt32(Increment));
  CGF.EmitStoreOfScalar(NewVal, Addr);
}

// \brief Decrease the value of parallelism level for the current thread.
void CGOpenMPRuntimeNVPTX::decreaseParallelismLevel(CodeGenFunction &CGF,
                                                    bool IsSimd) const {
  unsigned Increment = IsSimd ? 10 : 1;
  auto Addr = getParallelismLevelLValue(CGF);
  auto *CurVal = CGF.EmitLoadOfLValue(Addr, SourceLocation()).getScalarVal();
  auto *NewVal =
      CGF.Builder.CreateNSWSub(CurVal, CGF.Builder.getInt32(Increment));
  CGF.EmitStoreOfScalar(NewVal, Addr);
}

// \brief Initialize with zero the value of parallelism level for the current
// thread.
void CGOpenMPRuntimeNVPTX::initializeParallelismLevel(
    CodeGenFunction &CGF) const {
  auto Addr = getParallelismLevelLValue(CGF);
  CGF.EmitStoreOfScalar(llvm::Constant::getNullValue(CGM.Int32Ty), Addr);
}

static FieldDecl *addFieldToRecordDecl(ASTContext &C, DeclContext *DC,
                                       QualType FieldTy) {
  auto *Field = FieldDecl::Create(
      C, DC, SourceLocation(), SourceLocation(), /*Id=*/nullptr, FieldTy,
      C.getTrivialTypeSourceInfo(FieldTy, SourceLocation()),
      /*BW=*/nullptr, /*Mutable=*/false, /*InitStyle=*/ICIS_NoInit);
  Field->setAccess(AS_public);
  DC->addDecl(Field);
  return Field;
}

// \brief Type of the data sharing master slot.
QualType CGOpenMPRuntimeNVPTX::getDataSharingMasterSlotQty() {
  //  struct MasterSlot {
  //    Slot *Next;
  //    void *DataEnd;
  //    char Data[DS_Slot_Size]);
  //  };

  const char *Name = "__openmp_nvptx_data_sharing_master_slot_ty";
  if (DataSharingMasterSlotQty.isNull()) {
    ASTContext &C = CGM.getContext();
    auto *RD = C.buildImplicitRecord(Name);
    RD->startDefinition();
    addFieldToRecordDecl(C, RD, C.getPointerType(getDataSharingSlotQty()));
    addFieldToRecordDecl(C, RD, C.VoidPtrTy);
    llvm::APInt NumElems(C.getTypeSize(C.getUIntPtrType()), DS_Slot_Size);
    QualType DataTy = C.getConstantArrayType(
        C.CharTy, NumElems, ArrayType::Normal, /*IndexTypeQuals=*/0);
    addFieldToRecordDecl(C, RD, DataTy);
    RD->completeDefinition();
    DataSharingMasterSlotQty = C.getRecordType(RD);
  }
  return DataSharingMasterSlotQty;
}

// \brief Type of the data sharing worker warp slot.
QualType CGOpenMPRuntimeNVPTX::getDataSharingWorkerWarpSlotQty() {
  //  struct WorkerWarpSlot {
  //    Slot *Next;
  //    void *DataEnd;
  //    char [DS_Worker_Warp_Slot_Size];
  //  };

  const char *Name = "__openmp_nvptx_data_sharing_worker_warp_slot_ty";
  if (DataSharingWorkerWarpSlotQty.isNull()) {
    ASTContext &C = CGM.getContext();
    auto *RD = C.buildImplicitRecord(Name);
    RD->startDefinition();
    addFieldToRecordDecl(C, RD, C.getPointerType(getDataSharingSlotQty()));
    addFieldToRecordDecl(C, RD, C.VoidPtrTy);
    llvm::APInt NumElems(C.getTypeSize(C.getUIntPtrType()),
                         DS_Worker_Warp_Slot_Size);
    QualType DataTy = C.getConstantArrayType(
        C.CharTy, NumElems, ArrayType::Normal, /*IndexTypeQuals=*/0);
    addFieldToRecordDecl(C, RD, DataTy);
    RD->completeDefinition();
    DataSharingWorkerWarpSlotQty = C.getRecordType(RD);
  }
  return DataSharingWorkerWarpSlotQty;
}

// \brief Get the type of the master or worker slot.
QualType CGOpenMPRuntimeNVPTX::getDataSharingSlotQty(bool UseFixedDataSize,
                                                     bool IsMaster) {
  if (UseFixedDataSize) {
    if (IsMaster)
      return getDataSharingMasterSlotQty();
    return getDataSharingWorkerWarpSlotQty();
  }

  //  struct Slot {
  //    Slot *Next;
  //    void *DataEnd;
  //    char Data[];
  //  };

  const char *Name = "__kmpc_data_sharing_slot";
  if (DataSharingSlotQty.isNull()) {
    ASTContext &C = CGM.getContext();
    auto *RD = C.buildImplicitRecord(Name);
    RD->startDefinition();
    addFieldToRecordDecl(C, RD, C.getPointerType(C.getRecordType(RD)));
    addFieldToRecordDecl(C, RD, C.VoidPtrTy);
    QualType DataTy = C.getIncompleteArrayType(C.CharTy, ArrayType::Normal,
                                               /*IndexTypeQuals=*/0);
    addFieldToRecordDecl(C, RD, DataTy);
    RD->completeDefinition();
    DataSharingSlotQty = C.getRecordType(RD);
  }
  return DataSharingSlotQty;
}

llvm::Type *CGOpenMPRuntimeNVPTX::getDataSharingSlotTy(bool UseFixedDataSize,
                                                       bool IsMaster) {
  return CGM.getTypes().ConvertTypeForMem(
      getDataSharingSlotQty(UseFixedDataSize, IsMaster));
}

// \brief Type of the data sharing root slot.
QualType CGOpenMPRuntimeNVPTX::getDataSharingRootSlotQty() {
  // The type of the global with the root slots:
  //  struct Slots {
  //    MasterSlot MS;
  //    WorkerWarpSlot WS[DS_Max_Worker_Threads/DS_Max_Worker_Warp_Size];
  // };
  if (DataSharingRootSlotQty.isNull()) {
    ASTContext &C = CGM.getContext();
    auto *RD = C.buildImplicitRecord("__openmp_nvptx_data_sharing_ty");
    RD->startDefinition();
    addFieldToRecordDecl(C, RD, getDataSharingMasterSlotQty());
    llvm::APInt NumElems(C.getTypeSize(C.getUIntPtrType()),
                         DS_Max_Worker_Threads / DS_Max_Worker_Warp_Size);
    addFieldToRecordDecl(C, RD, C.getConstantArrayType(
                                    getDataSharingWorkerWarpSlotQty(), NumElems,
                                    ArrayType::Normal, /*IndexTypeQuals=*/0));
    RD->completeDefinition();

    llvm::APInt NumTeams(C.getTypeSize(C.getUIntPtrType()), DS_Max_Teams);
    DataSharingRootSlotQty = C.getConstantArrayType(
        C.getRecordType(RD), NumTeams, ArrayType::Normal, /*IndexTypeQuals=*/0);
  }
  return DataSharingRootSlotQty;
}

// \brief Return address of the initial slot that is used to share data.
LValue CGOpenMPRuntimeNVPTX::getDataSharingRootSlotLValue(CodeGenFunction &CGF,
                                                          bool IsMaster) {
  auto &M = CGM.getModule();

  const char *Name = "__openmp_nvptx_shared_data_slots";
  llvm::GlobalVariable *Gbl = M.getGlobalVariable(Name);

  if (!Gbl) {
    auto *Ty = CGF.getTypes().ConvertTypeForMem(getDataSharingRootSlotQty());
    Gbl = new llvm::GlobalVariable(
        M, Ty,
        /*isConstant=*/false, llvm::GlobalVariable::CommonLinkage,
        llvm::Constant::getNullValue(Ty), Name,
        /*InsertBefore=*/nullptr, llvm::GlobalVariable::NotThreadLocal);
  }

  // Return the master slot if the flag is set, otherwise get the right worker
  // slots.
  if (IsMaster) {
    llvm::Value *Idx[] = {llvm::Constant::getNullValue(CGM.Int32Ty),
                          getNVPTXBlockID(CGF),
                          llvm::Constant::getNullValue(CGM.Int32Ty)};
    llvm::Value *AddrVal = CGF.Builder.CreateInBoundsGEP(Gbl, Idx);
    return CGF.MakeNaturalAlignAddrLValue(AddrVal,
                                          getDataSharingMasterSlotQty());
  }

  auto *WarpID = getNVPTXWarpID(CGF);
  llvm::Value *Idx[] = {llvm::Constant::getNullValue(CGM.Int32Ty),
                        getNVPTXBlockID(CGF),
                        /*WS=*/CGF.Builder.getInt32(1), WarpID};
  llvm::Value *AddrVal = CGF.Builder.CreateInBoundsGEP(Gbl, Idx);
  return CGF.MakeNaturalAlignAddrLValue(AddrVal,
                                        getDataSharingWorkerWarpSlotQty());
}

// \brief Initialize the data sharing slots and pointers.
void CGOpenMPRuntimeNVPTX::initializeDataSharing(CodeGenFunction &CGF,
                                                 bool IsMaster) {
  // We initialized the slot and stack pointer in shared memory with their
  // initial values. Also, we initialize the slots with the initial size.

  auto &Bld = CGF.Builder;
  // auto &Ctx = CGF.getContext();

  // If this is not the OpenMP master thread, make sure that only the warp
  // master does the initialization.
  llvm::BasicBlock *EndBB = CGF.createBasicBlock("after_shared_data_init");

  if (!IsMaster) {
    auto *IsWarpMaster = getNVPTXIsWarpActiveMaster(CGF);
    llvm::BasicBlock *InitBB = CGF.createBasicBlock("shared_data_init");
    Bld.CreateCondBr(IsWarpMaster, InitBB, EndBB);
    CGF.EmitBlock(InitBB);
  }

  auto SlotLV = getDataSharingRootSlotLValue(CGF, IsMaster);

  auto *SlotPtrTy = getDataSharingSlotTy()->getPointerTo();
  auto *CastedSlot =
      Bld.CreateBitCast(SlotLV.getAddress(), SlotPtrTy).getPointer();

  llvm::Value *Args[] = {
      CastedSlot,
      llvm::ConstantInt::get(CGM.SizeTy, IsMaster ? DS_Slot_Size
                                                  : DS_Worker_Warp_Slot_Size)};
  Bld.CreateCall(createNVPTXRuntimeFunction(
                     OMPRTL_NVPTX__kmpc_initialize_data_sharing_environment),
                 Args);

  CGF.EmitBlock(EndBB);
  return;
}

// \brief Initialize the data sharing slots and pointers and return the
// generated call.
llvm::Function *CGOpenMPRuntimeNVPTX::createKernelInitializerFunction(
    llvm::Function *WorkerFunction) {
  auto &Ctx = CGM.getContext();

  // FIXME: Consider to use name based on the worker function name.
  char Name[] = "__omp_kernel_initialization";

  auto RetQTy = Ctx.getCanonicalType(
      Ctx.getIntTypeForBitwidth(/*DestWidth=*/32, /*Signed=*/false));
  auto &CGFI = CGM.getTypes().arrangeLLVMFunctionInfo(
      RetQTy, /*instanceMethod=*/false, /*chainCall=*/false, None,
      FunctionType::ExtInfo(), {}, RequiredArgs::All);

  llvm::Function *InitFn = llvm::Function::Create(
      CGM.getTypes().GetFunctionType(CGFI), llvm::GlobalValue::InternalLinkage,
      Name, &CGM.getModule());

  CGM.SetInternalFunctionAttributes(/*D=*/nullptr, InitFn, CGFI);
  InitFn->setLinkage(llvm::GlobalValue::InternalLinkage);

  CodeGenFunction CGF(CGM, /*suppressNewContext=*/true);
  CGF.StartFunction(GlobalDecl(), RetQTy, InitFn, CGFI, {});

  auto &Bld = CGF.Builder;

  llvm::BasicBlock *WorkerBB = CGF.createBasicBlock(".worker");
  llvm::BasicBlock *MasterCheckBB = CGF.createBasicBlock(".ismaster");
  llvm::BasicBlock *MasterBB = CGF.createBasicBlock(".master");
  llvm::BasicBlock *ExitBB = CGF.createBasicBlock(".exit");

  auto *RetTy = CGM.Int32Ty;
  auto *One = llvm::ConstantInt::get(RetTy, 1);
  auto *Zero = llvm::ConstantInt::get(RetTy, 0);
  CGF.EmitStoreOfScalar(One, CGF.ReturnValue, /*Volatile=*/false, RetQTy);

  auto *IsWorker =
      Bld.CreateICmpULT(getNVPTXThreadID(CGF), getThreadLimit(CGF));
  Bld.CreateCondBr(IsWorker, WorkerBB, MasterCheckBB);

  CGF.EmitBlock(WorkerBB);
  initializeDataSharing(CGF, /*IsMaster=*/false);
  Bld.CreateCall(WorkerFunction);
  CGF.EmitBranch(ExitBB);

  CGF.EmitBlock(MasterCheckBB);
  auto *IsMaster =
      Bld.CreateICmpEQ(getNVPTXThreadID(CGF), getMasterThreadID(CGF));
  Bld.CreateCondBr(IsMaster, MasterBB, ExitBB);

  CGF.EmitBlock(MasterBB);
  initializeDataSharing(CGF, /*IsMaster=*/true);
  CGF.EmitStoreOfScalar(Zero, CGF.ReturnValue, /*Volatile=*/false, RetQTy);
  CGF.EmitBranch(ExitBB);

  CGF.EmitBlock(ExitBB);
  CGF.FinishFunction();

  return InitFn;
}

/// \brief Get the GPU warp size.
llvm::Value *
CGOpenMPRuntimeNVPTX::getNVPTXWarpSize(CodeGenFunction &CGF) const {
  CGBuilderTy &Bld = CGF.Builder;
  return Bld.CreateCall(
      llvm::Intrinsic::getDeclaration(
          &CGM.getModule(), llvm::Intrinsic::nvvm_read_ptx_sreg_warpsize),
      llvm::None, "nvptx_warp_size");
}

/// \brief Get the id of the current thread on the GPU.
llvm::Value *
CGOpenMPRuntimeNVPTX::getNVPTXThreadID(CodeGenFunction &CGF) const {
  CGBuilderTy &Bld = CGF.Builder;
  return Bld.CreateCall(
      llvm::Intrinsic::getDeclaration(
          &CGM.getModule(), llvm::Intrinsic::nvvm_read_ptx_sreg_tid_x),
      llvm::None, "nvptx_tid");
}

/// \brief Get the id of the current thread in the Warp.
llvm::Value *
CGOpenMPRuntimeNVPTX::getNVPTXThreadWarpID(CodeGenFunction &CGF) const {
  CGBuilderTy &Bld = CGF.Builder;
  return Bld.CreateAnd(getNVPTXThreadID(CGF),
                       Bld.getInt32(DS_Max_Worker_Warp_Size_Log2_Mask));
}

/// \brief Get the id of the current block on the GPU.
llvm::Value *CGOpenMPRuntimeNVPTX::getNVPTXBlockID(CodeGenFunction &CGF) const {
  CGBuilderTy &Bld = CGF.Builder;
  return Bld.CreateCall(
      llvm::Intrinsic::getDeclaration(
          &CGM.getModule(), llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_x),
      llvm::None, "nvptx_block_id");
}

/// \brief Get the id of the warp in the block.
llvm::Value *CGOpenMPRuntimeNVPTX::getNVPTXWarpID(CodeGenFunction &CGF) const {
  CGBuilderTy &Bld = CGF.Builder;
  return Bld.CreateAShr(getNVPTXThreadID(CGF), DS_Max_Worker_Warp_Size_Log2,
                        "nvptx_warp_id");
}

// \brief Get the maximum number of threads in a block of the GPU.
llvm::Value *
CGOpenMPRuntimeNVPTX::getNVPTXNumThreads(CodeGenFunction &CGF) const {
  CGBuilderTy &Bld = CGF.Builder;
  return Bld.CreateCall(
      llvm::Intrinsic::getDeclaration(
          &CGM.getModule(), llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_x),
      llvm::None, "nvptx_num_threads");
}

// \brief Get a 32 bit mask, whose bits set to 1 represent the active threads.
llvm::Value *
CGOpenMPRuntimeNVPTX::getNVPTXWarpActiveThreadsMask(CodeGenFunction &CGF) {
  return CGF.EmitRuntimeCall(
      createNVPTXRuntimeFunction(OMPRTL_NVPTX__kmpc_warp_active_thread_mask),
      None, "warp_active_thread_mask");
}

// \brief Get the number of active threads in a warp.
llvm::Value *
CGOpenMPRuntimeNVPTX::getNVPTXWarpActiveNumThreads(CodeGenFunction &CGF) {
  CGBuilderTy &Bld = CGF.Builder;
  return Bld.CreateCall(llvm::Intrinsic::getDeclaration(
                            &CGM.getModule(), llvm::Intrinsic::nvvm_popc_i),
                        getNVPTXWarpActiveThreadsMask(CGF),
                        "warp_active_num_threads");
}

// \brief Get the ID of the thread among the current active threads in the warp.
llvm::Value *
CGOpenMPRuntimeNVPTX::getNVPTXWarpActiveThreadID(CodeGenFunction &CGF) {
  CGBuilderTy &Bld = CGF.Builder;

  // The active thread Id can be computed as the number of bits in the active
  // mask to the right of the current thread:
  // popc( Mask << (32 - (threadID & 0x1f)) );
  auto *WarpID = getNVPTXThreadWarpID(CGF);
  auto *Mask = getNVPTXWarpActiveThreadsMask(CGF);
  auto *ShNum = Bld.CreateSub(Bld.getInt32(32), WarpID);
  auto *Sh = Bld.CreateShl(Mask, ShNum);
  return Bld.CreateCall(llvm::Intrinsic::getDeclaration(
                            &CGM.getModule(), llvm::Intrinsic::nvvm_popc_i),
                        Sh, "warp_active_thread_id");
}

// \brief Get a conditional that is set to true if the thread is the master of
// the active threads in the warp.
llvm::Value *
CGOpenMPRuntimeNVPTX::getNVPTXIsWarpActiveMaster(CodeGenFunction &CGF) {
  CGBuilderTy &Bld = CGF.Builder;
  return Bld.CreateICmpEQ(getNVPTXWarpActiveThreadID(CGF), Bld.getInt32(0),
                          "is_warp_active_master");
}

/// \brief Get barrier to synchronize all threads in a block.
void CGOpenMPRuntimeNVPTX::getNVPTXCTABarrier(CodeGenFunction &CGF) const {
  CGBuilderTy &Bld = CGF.Builder;
  Bld.CreateCall(llvm::Intrinsic::getDeclaration(
      &CGM.getModule(), llvm::Intrinsic::nvvm_barrier0));
}

/// \brief Get barrier #n to synchronize selected (multiple of 32) threads in
/// a block.
void CGOpenMPRuntimeNVPTX::getNVPTXBarrier(CodeGenFunction &CGF, int ID,
                                           int NumThreads) const {
  CGBuilderTy &Bld = CGF.Builder;
  llvm::Value *Args[] = {Bld.getInt32(ID), Bld.getInt32(NumThreads)};
  Bld.CreateCall(llvm::Intrinsic::getDeclaration(&CGM.getModule(),
                                                 llvm::Intrinsic::nvvm_barrier),
                 Args);
}

// \brief Synchronize all GPU threads in a block.
void CGOpenMPRuntimeNVPTX::syncCTAThreads(CodeGenFunction &CGF) const {
  getNVPTXCTABarrier(CGF);
}

// \brief Get the value of the thread_limit clause in the teams directive.
// The runtime always starts thread_limit+warpSize threads.
llvm::Value *CGOpenMPRuntimeNVPTX::getThreadLimit(CodeGenFunction &CGF) const {
  CGBuilderTy &Bld = CGF.Builder;
  return Bld.CreateSub(getNVPTXNumThreads(CGF), getNVPTXWarpSize(CGF),
                       "thread_limit");
}

/// \brief Get the thread id of the OMP master thread.
/// The master thread id is the first thread (lane) of the last warp in the
/// GPU block.  Warp size is assumed to be some power of 2.
/// Thread id is 0 indexed.
/// E.g: If NumThreads is 33, master id is 32.
///      If NumThreads is 64, master id is 32.
///      If NumThreads is 1024, master id is 992.
llvm::Value *CGOpenMPRuntimeNVPTX::getMasterThreadID(CodeGenFunction &CGF) {
  CGBuilderTy &Bld = CGF.Builder;
  llvm::Value *NumThreads = getNVPTXNumThreads(CGF);

  // We assume that the warp size is a power of 2.
  llvm::Value *Mask = Bld.CreateSub(getNVPTXWarpSize(CGF), Bld.getInt32(1));

  return Bld.CreateAnd(Bld.CreateSub(NumThreads, Bld.getInt32(1)),
                       Bld.CreateNot(Mask), "master_tid");
}

/// \brief Get number of OMP workers for parallel region after subtracting
/// the master warp.
llvm::Value *CGOpenMPRuntimeNVPTX::getNumWorkers(CodeGenFunction &CGF) {
  CGBuilderTy &Bld = CGF.Builder;
  return Bld.CreateNUWSub(getNVPTXNumThreads(CGF), Bld.getInt32(32),
                          "num_workers");
}

/// \brief Get thread id in team.
/// FIXME: Remove the expensive remainder operation.
llvm::Value *CGOpenMPRuntimeNVPTX::getTeamThreadId(CodeGenFunction &CGF) {
  CGBuilderTy &Bld = CGF.Builder;
  // N % M = N & (M-1) it M is a power of 2. The master Id is expected to be a
  // power fo two in all cases.
  auto *Mask = Bld.CreateNUWSub(getMasterThreadID(CGF), Bld.getInt32(1));
  return Bld.CreateAnd(getNVPTXThreadID(CGF), Mask, "team_tid");
}

/// \brief Get global thread id.
llvm::Value *CGOpenMPRuntimeNVPTX::getGlobalThreadId(CodeGenFunction &CGF) {
  CGBuilderTy &Bld = CGF.Builder;
  return Bld.CreateAdd(Bld.CreateMul(getNVPTXBlockID(CGF), getNumWorkers(CGF)),
                       getTeamThreadId(CGF), "global_tid");
}

CGOpenMPRuntimeNVPTX::WorkerFunctionState::WorkerFunctionState(
    CodeGenModule &CGM)
    : WorkerFn(nullptr), CGFI(nullptr) {
  createWorkerFunction(CGM);
};

void CGOpenMPRuntimeNVPTX::WorkerFunctionState::createWorkerFunction(
    CodeGenModule &CGM) {
  // Create an worker function with no arguments.
  CGFI = &CGM.getTypes().arrangeNullaryFunction();

  WorkerFn = llvm::Function::Create(
      CGM.getTypes().GetFunctionType(*CGFI), llvm::GlobalValue::InternalLinkage,
      /* placeholder */ "_worker", &CGM.getModule());
  CGM.SetInternalFunctionAttributes(/*D=*/nullptr, WorkerFn, *CGFI);
  WorkerFn->setLinkage(llvm::GlobalValue::InternalLinkage);
  WorkerFn->addFnAttr(llvm::Attribute::NoInline);
}

void CGOpenMPRuntimeNVPTX::emitWorkerFunction(WorkerFunctionState &WST) {
  auto &Ctx = CGM.getContext();

  CodeGenFunction CGF(CGM, /*suppressNewContext=*/true);
  CGF.StartFunction(GlobalDecl(), Ctx.VoidTy, WST.WorkerFn, *WST.CGFI, {});
  emitWorkerLoop(CGF, WST);
  CGF.FinishFunction();
}

void CGOpenMPRuntimeNVPTX::emitWorkerLoop(CodeGenFunction &CGF,
                                          WorkerFunctionState &WST) {
  //
  // The workers enter this loop and wait for parallel work from the master.
  // When the master encounters a parallel region it sets up the work + variable
  // arguments, and wakes up the workers.  The workers first check to see if
  // they are required for the parallel region, i.e., within the # of requested
  // parallel threads.  The activated workers load the variable arguments and
  // execute the parallel work.
  //

  CGBuilderTy &Bld = CGF.Builder;

  llvm::BasicBlock *AwaitBB = CGF.createBasicBlock(".await.work");
  llvm::BasicBlock *SelectWorkersBB = CGF.createBasicBlock(".select.workers");
  llvm::BasicBlock *ExecuteBB = CGF.createBasicBlock(".execute.parallel");
  llvm::BasicBlock *TerminateBB = CGF.createBasicBlock(".terminate.parallel");
  llvm::BasicBlock *BarrierBB = CGF.createBasicBlock(".barrier.parallel");
  llvm::BasicBlock *ExitBB = CGF.createBasicBlock(".exit");

  CGF.EmitBranch(AwaitBB);

  // Workers wait for work from master.
  CGF.EmitBlock(AwaitBB);
  // Wait for parallel work
  syncCTAThreads(CGF);

  Address WorkFn = CGF.CreateTempAlloca(
      CGF.Int8PtrTy, CharUnits::fromQuantity(8), /*Name*/ "work_fn");
  Address ExecStatus =
      CGF.CreateTempAlloca(CGF.Int8Ty, CharUnits::fromQuantity(1),
                           /*Name*/ "exec_status");
  CGF.InitTempAlloca(ExecStatus, Bld.getInt8(/*C=*/0));

  llvm::Value *Args[] = {WorkFn.getPointer()};
  llvm::Value *Ret = CGF.EmitRuntimeCall(
      createNVPTXRuntimeFunction(OMPRTL_NVPTX__kmpc_kernel_parallel), Args);
  Bld.CreateStore(Bld.CreateZExt(Ret, CGF.Int8Ty), ExecStatus);

  // On termination condition (workfn == 0), exit loop.
  llvm::Value *ShouldTerminate = Bld.CreateICmpEQ(
      Bld.CreateLoad(WorkFn), llvm::Constant::getNullValue(CGF.Int8PtrTy),
      "should_terminate");
  Bld.CreateCondBr(ShouldTerminate, ExitBB, SelectWorkersBB);

  // Activate requested workers.
  CGF.EmitBlock(SelectWorkersBB);
  llvm::Value *IsActive =
      Bld.CreateICmpNE(Bld.CreateLoad(ExecStatus), Bld.getInt8(0), "is_active");
  Bld.CreateCondBr(IsActive, ExecuteBB, BarrierBB);

  // Signal start of parallel region.
  CGF.EmitBlock(ExecuteBB);

  // Process work items: outlined parallel functions.
  for (auto *W : Work) {
    // Try to match this outlined function.
    auto ID = Bld.CreatePtrToInt(W, CGM.Int64Ty);
    ID = Bld.CreateIntToPtr(ID, CGM.Int8PtrTy);
    llvm::Value *WorkFnMatch =
        Bld.CreateICmpEQ(Bld.CreateLoad(WorkFn), ID, "work_match");

    llvm::BasicBlock *ExecuteFNBB = CGF.createBasicBlock(".execute.fn");
    llvm::BasicBlock *CheckNextBB = CGF.createBasicBlock(".check.next");
    Bld.CreateCondBr(WorkFnMatch, ExecuteFNBB, CheckNextBB);

    // Execute this outlined function.
    CGF.EmitBlock(ExecuteFNBB);

    // Insert call to work function. We pass the master has source thread ID.
    auto Fn = cast<llvm::Function>(W);
    llvm::Value *Args[] = {getMasterThreadID(CGF)};
    CGF.EmitCallOrInvoke(Fn, Args);

    // Go to end of parallel region.
    CGF.EmitBranch(TerminateBB);

    CGF.EmitBlock(CheckNextBB);
  }

  // Signal end of parallel region.
  CGF.EmitBlock(TerminateBB);
  CGF.EmitRuntimeCall(
      createNVPTXRuntimeFunction(OMPRTL_NVPTX__kmpc_kernel_end_parallel),
      ArrayRef<llvm::Value *>());
  CGF.EmitBranch(BarrierBB);

  // All active and inactive workers wait at a barrier after parallel region.
  CGF.EmitBlock(BarrierBB);
  // Barrier after parallel region.
  syncCTAThreads(CGF);
  CGF.EmitBranch(AwaitBB);

  // Exit target region.
  CGF.EmitBlock(ExitBB);
}

// Setup NVPTX threads for master-worker OpenMP scheme.
void CGOpenMPRuntimeNVPTX::emitEntryHeader(CodeGenFunction &CGF,
                                           EntryFunctionState &EST,
                                           WorkerFunctionState &WST) {
  CGBuilderTy &Bld = CGF.Builder;

  //  // Setup BBs in entry function.
  //  llvm::BasicBlock *WorkerCheckBB =
  //  CGF.createBasicBlock(".check.for.worker");
  //  llvm::BasicBlock *WorkerBB = CGF.createBasicBlock(".worker");
  //  llvm::BasicBlock *MasterBB = CGF.createBasicBlock(".master");
  EST.ExitBB = CGF.createBasicBlock(".sleepy.hollow");
  //
  //  // Get the thread limit.
  //  llvm::Value *ThreadLimit = getThreadLimit(CGF);
  //  // Get the master thread id.
  //  llvm::Value *MasterID = getMasterThreadID(CGF);
  //  // Current thread's identifier.
  //  llvm::Value *ThreadID = getNVPTXThreadID(CGF);
  //
  //  // The head (master thread) marches on while its body of companion threads
  //  in
  //  // the warp go to sleep.  Also put to sleep threads in excess of the
  //  // thread_limit value on the teams directive.
  //  llvm::Value *NotMaster = Bld.CreateICmpNE(ThreadID, MasterID,
  //  "not_master");
  //  llvm::Value *ThreadLimitExcess =
  //      Bld.CreateICmpUGE(ThreadID, ThreadLimit, "thread_limit_excess");
  //  llvm::Value *ShouldDie =
  //      Bld.CreateAnd(ThreadLimitExcess, NotMaster, "excess_threads");
  //  Bld.CreateCondBr(ShouldDie, EST.ExitBB, WorkerCheckBB);
  //
  //  // Select worker threads...
  //  CGF.EmitBlock(WorkerCheckBB);
  //  llvm::Value *IsWorker = Bld.CreateICmpULT(ThreadID, MasterID,
  //  "is_worker");
  //  Bld.CreateCondBr(IsWorker, WorkerBB, MasterBB);
  //
  //  // ... and send to worker loop, awaiting parallel invocation.
  //  CGF.EmitBlock(WorkerBB);
  //  initializeParallelismLevel(CGF);
  //  llvm::SmallVector<llvm::Value *, 16> WorkerVars;
  //  for (auto &I : CGF.CurFn->args()) {
  //    WorkerVars.push_back(&I);
  //  }
  //
  //  CGF.EmitCallOrInvoke(WST.WorkerFn, None);
  //  CGF.EmitBranch(EST.ExitBB);
  //
  //  // Only master thread executes subsequent serial code.
  //  CGF.EmitBlock(MasterBB);

  // Mark the current function as entry point.
  DataSharingFunctionInfoMap[CGF.CurFn].IsEntryPoint = true;
  DataSharingFunctionInfoMap[CGF.CurFn].EntryWorkerFunction = WST.WorkerFn;
  DataSharingFunctionInfoMap[CGF.CurFn].EntryExitBlock = EST.ExitBB;

  // First action in sequential region:
  // Initialize the state of the OpenMP runtime library on the GPU.
  llvm::Value *Args[] = {Bld.getInt32(/*OmpHandle=*/0), getThreadLimit(CGF)};
  CGF.EmitRuntimeCall(
      createNVPTXRuntimeFunction(OMPRTL_NVPTX__kmpc_kernel_init), Args);
}

void CGOpenMPRuntimeNVPTX::emitEntryFooter(CodeGenFunction &CGF,
                                           EntryFunctionState &EST) {
  llvm::BasicBlock *TerminateBB = CGF.createBasicBlock(".termination.notifier");
  CGF.EmitBranch(TerminateBB);

  CGF.EmitBlock(TerminateBB);
  // Signal termination condition.
  CGF.EmitRuntimeCall(
      createNVPTXRuntimeFunction(OMPRTL_NVPTX__kmpc_kernel_deinit), None);
  // Barrier to terminate worker threads.
  syncCTAThreads(CGF);
  // Master thread jumps to exit point.
  CGF.EmitBranch(EST.ExitBB);

  CGF.EmitBlock(EST.ExitBB);
}

/// \brief Returns specified OpenMP runtime function for the current OpenMP
/// implementation.  Specialized for the NVPTX device.
/// \param Function OpenMP runtime function.
/// \return Specified function.
llvm::Constant *
CGOpenMPRuntimeNVPTX::createNVPTXRuntimeFunction(unsigned Function) {
  llvm::Constant *RTLFn = nullptr;
  switch (static_cast<OpenMPRTLFunctionNVPTX>(Function)) {
  case OMPRTL_NVPTX__kmpc_kernel_init: {
    // Build void __kmpc_kernel_init(kmp_int32 omp_handle,
    // kmp_int32 thread_limit);
    llvm::Type *TypeParams[] = {CGM.Int32Ty, CGM.Int32Ty};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_kernel_init");
    break;
  }
  case OMPRTL_NVPTX__kmpc_kernel_deinit: {
    // Build void __kmpc_kernel_deinit();
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, {}, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_kernel_deinit");
    break;
  }
  case OMPRTL_NVPTX__kmpc_serialized_parallel: {
    // Build void __kmpc_serialized_parallel(ident_t *loc, kmp_int32
    // global_tid);
    llvm::Type *TypeParams[] = {getIdentTyPointerTy(), CGM.Int32Ty};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_serialized_parallel");
    break;
  }
  case OMPRTL_NVPTX__kmpc_end_serialized_parallel: {
    // Build void __kmpc_end_serialized_parallel(ident_t *loc, kmp_int32
    // global_tid);
    llvm::Type *TypeParams[] = {getIdentTyPointerTy(), CGM.Int32Ty};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_end_serialized_parallel");
    break;
  }
  case OMPRTL_NVPTX__kmpc_kernel_prepare_parallel: {
    /// Build void __kmpc_kernel_prepare_parallel(
    /// void *outlined_function);
    llvm::Type *TypeParams[] = {CGM.Int8PtrTy};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_kernel_prepare_parallel");
    break;
  }
  case OMPRTL_NVPTX__kmpc_kernel_parallel: {
    /// Build bool __kmpc_kernel_parallel(void **outlined_function);
    llvm::Type *TypeParams[] = {CGM.Int8PtrPtrTy};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(llvm::Type::getInt1Ty(CGM.getLLVMContext()),
                                TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_kernel_parallel");
    break;
  }
  case OMPRTL_NVPTX__kmpc_kernel_end_parallel: {
    /// Build void __kmpc_kernel_end_parallel();
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, {}, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_kernel_end_parallel");
    break;
  }
  case OMPRTL_NVPTX__kmpc_kernel_convergent_parallel: {
    /// \brief Call to bool __kmpc_kernel_convergent_parallel(
    /// void *buffer, bool *IsFinal, kmpc_int32 *LaneSource);
    llvm::Type *TypeParams[] = {CGM.Int8PtrTy, CGM.Int8PtrTy,
                                CGM.Int32Ty->getPointerTo()};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(llvm::Type::getInt1Ty(CGM.getLLVMContext()),
                                TypeParams, /*isVarArg*/ false);
    RTLFn =
        CGM.CreateRuntimeFunction(FnTy, "__kmpc_kernel_convergent_parallel");
    break;
  }
  case OMPRTL_NVPTX__kmpc_kernel_end_convergent_parallel: {
    /// Build void __kmpc_kernel_end_convergent_parallel(void *buffer);
    llvm::Type *TypeParams[] = {CGM.Int8PtrTy};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy,
                                      "__kmpc_kernel_end_convergent_parallel");
    break;
  }
  case OMPRTL_NVPTX__kmpc_kernel_convergent_simd: {
    /// \brief Call to bool __kmpc_kernel_convergent_simd(
    /// void *buffer, bool *IsFinal, kmpc_int32 *LaneSource, kmpc_int32 *LaneId,
    /// kmpc_int32 *NumLanes);
    llvm::Type *TypeParams[] = {
        CGM.Int8PtrTy, CGM.Int8PtrTy, CGM.Int32Ty->getPointerTo(),
        CGM.Int32Ty->getPointerTo(), CGM.Int32Ty->getPointerTo()};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(llvm::Type::getInt1Ty(CGM.getLLVMContext()),
                                TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_kernel_convergent_simd");
    break;
  }
  case OMPRTL_NVPTX__kmpc_kernel_end_convergent_simd: {
    /// Build void __kmpc_kernel_end_convergent_simd(void *buffer);
    llvm::Type *TypeParams[] = {CGM.Int8PtrTy};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn =
        CGM.CreateRuntimeFunction(FnTy, "__kmpc_kernel_end_convergent_simd");
    break;
  }
  case OMPRTL_NVPTX__kmpc_warp_active_thread_mask: {
    /// Build void __kmpc_warp_active_thread_mask();
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.Int32Ty, None, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_warp_active_thread_mask");
    break;
  }
  case OMPRTL_NVPTX__kmpc_initialize_data_sharing_environment: {
    /// Build void
    /// __kmpc_initialize_data_sharing_environment(__kmpc_data_sharing_slot
    /// *RootS, size_t InitialDataSize);
    auto *SlotTy = CGM.getTypes().ConvertTypeForMem(getDataSharingSlotQty());
    llvm::Type *TypeParams[] = {SlotTy->getPointerTo(), CGM.SizeTy};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(
        FnTy, "__kmpc_initialize_data_sharing_environment");
    break;
  }
  case OMPRTL_NVPTX__kmpc_data_sharing_environment_begin: {
    /// Build void* __kmpc_data_sharing_environment_begin(
    /// __kmpc_data_sharing_slot **SavedSharedSlot, void **SavedSharedStack,
    /// void **SavedSharedFrame, int32_t *SavedActiveThreads, size_t
    /// SharingDataSize, size_t SharingDefaultDataSize);
    auto *SlotTy = CGM.getTypes().ConvertTypeForMem(getDataSharingSlotQty());
    llvm::Type *TypeParams[] = {SlotTy->getPointerTo()->getPointerTo(),
                                CGM.VoidPtrPtrTy,
                                CGM.VoidPtrPtrTy,
                                CGM.Int32Ty->getPointerTo(),
                                CGM.SizeTy,
                                CGM.SizeTy};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidPtrTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy,
                                      "__kmpc_data_sharing_environment_begin");
    break;
  }
  case OMPRTL_NVPTX__kmpc_data_sharing_environment_end: {
    /// Build void __kmpc_data_sharing_environment_end( __kmpc_data_sharing_slot
    /// **SavedSharedSlot, void **SavedSharedStack, void **SavedSharedFrame,
    /// int32_t *SavedActiveThreads, int32_t IsEntryPoint);
    auto *SlotTy = CGM.getTypes().ConvertTypeForMem(getDataSharingSlotQty());
    llvm::Type *TypeParams[] = {SlotTy->getPointerTo()->getPointerTo(),
                                CGM.VoidPtrPtrTy, CGM.VoidPtrPtrTy,
                                CGM.Int32Ty->getPointerTo(), CGM.Int32Ty};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn =
        CGM.CreateRuntimeFunction(FnTy, "__kmpc_data_sharing_environment_end");
    break;
  }
  case OMPRTL_NVPTX__kmpc_get_data_sharing_environment_frame: {
    /// Build void* __kmpc_get_data_sharing_environment_frame(int32_t
    /// SourceThreadID);
    llvm::Type *TypeParams[] = {CGM.Int32Ty};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidPtrTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(
        FnTy, "__kmpc_get_data_sharing_environment_frame");
    break;
  }
    //  case OMPRTL_NVPTX__kmpc_samuel_print: {
    //    llvm::Type *TypeParams[] = {CGM.Int64Ty};
    //    llvm::FunctionType *FnTy =
    //        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/
    //        false);
    //    RTLFn = CGM.CreateRuntimeFunction(
    //        FnTy, "__kmpc_samuel_print");
    //    break;
    //  }
  }
  return RTLFn;
}

llvm::Value *CGOpenMPRuntimeNVPTX::getThreadID(CodeGenFunction &CGF,
                                               SourceLocation Loc) {
  assert(CGF.CurFn && "No function in current CodeGenFunction.");
  return getGlobalThreadId(CGF);
}

void CGOpenMPRuntimeNVPTX::emitCapturedVars(
    CodeGenFunction &CGF, const OMPExecutableDirective &S,
    llvm::SmallVector<llvm::Value *, 16> &CapturedVars) {

  // We emit the variables exactly like the default implementation, but we
  // record the context because it is important to derive the enclosing
  // environment.

  CGOpenMPRuntime::emitCapturedVars(CGF, S, CapturedVars);
}

/// \brief Registers the context of a parallel region with the runtime
/// codegen implementation.
void CGOpenMPRuntimeNVPTX::registerParallelContext(
    CodeGenFunction &CGF, const OMPExecutableDirective &S) {
  CurrentParallelContext = CGF.CurCodeDecl;

  if (isOpenMPParallelDirective(S.getDirectiveKind()) ||
      isOpenMPSimdDirective(S.getDirectiveKind())) {
    createDataSharingInfo(CGF);
  }
}

void CGOpenMPRuntimeNVPTX::createOffloadEntry(llvm::Constant *ID,
                                              llvm::Constant *Addr,
                                              uint64_t Size) {
  auto *F = dyn_cast<llvm::Function>(Addr);
  // TODO: Add support for global variables on the device after declare target
  // support.
  if (!F)
    return;
  llvm::Module *M = F->getParent();
  llvm::LLVMContext &Ctx = M->getContext();

  // Get "nvvm.annotations" metadata node
  llvm::NamedMDNode *MD = M->getOrInsertNamedMetadata("nvvm.annotations");

  llvm::Metadata *MDVals[] = {
      llvm::ConstantAsMetadata::get(F), llvm::MDString::get(Ctx, "kernel"),
      llvm::ConstantAsMetadata::get(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), 1))};
  // Append metadata to nvvm.annotations
  MD->addOperand(llvm::MDNode::get(Ctx, MDVals));
}

void CGOpenMPRuntimeNVPTX::emitTargetOutlinedFunction(
    const OMPExecutableDirective &D, StringRef ParentName,
    llvm::Function *&OutlinedFn, llvm::Constant *&OutlinedFnID,
    bool IsOffloadEntry, const RegionCodeGenTy &CodeGen) {
  if (!IsOffloadEntry) // Nothing to do.
    return;

  assert(!ParentName.empty() && "Invalid target region parent name!");

  EntryFunctionState EST;
  WorkerFunctionState WST(CGM);

  // Emit target region as a standalone region.
  class NVPTXPrePostActionTy : public PrePostActionTy {
    CGOpenMPRuntimeNVPTX &RT;
    CGOpenMPRuntimeNVPTX::EntryFunctionState &EST;
    CGOpenMPRuntimeNVPTX::WorkerFunctionState &WST;

  public:
    NVPTXPrePostActionTy(CGOpenMPRuntimeNVPTX &RT,
                         CGOpenMPRuntimeNVPTX::EntryFunctionState &EST,
                         CGOpenMPRuntimeNVPTX::WorkerFunctionState &WST)
        : RT(RT), EST(EST), WST(WST) {}
    void Enter(CodeGenFunction &CGF) override {
      RT.emitEntryHeader(CGF, EST, WST);
    }
    void Exit(CodeGenFunction &CGF) override { RT.emitEntryFooter(CGF, EST); }
  } Action(*this, EST, WST);
  CodeGen.setAction(Action);
  emitTargetOutlinedFunctionHelper(D, ParentName, OutlinedFn, OutlinedFnID,
                                   IsOffloadEntry, CodeGen);

  // Create the worker function
  emitWorkerFunction(WST);

  // Now change the name of the worker function to correspond to this target
  // region's entry function.
  WST.WorkerFn->setName(OutlinedFn->getName() + "_worker");
  return;
}

namespace {
///
/// FIXME: This is stupid!
/// These class definitions are duplicated from CGOpenMPRuntime.cpp.  They
/// should instead be placed in the header file CGOpenMPRuntime.h and made
/// accessible to CGOpenMPRuntimeNVPTX.cpp.  Otherwise not only do we have
/// to duplicate code, but we have to ensure that both these definitions are
/// always the same.  This is a problem because a CGOpenMPRegionInfo object
/// from CGOpenMPRuntimeNVPTX.cpp is accessed in methods of CGOpenMPRuntime.cpp.
///
/// \brief Base class for handling code generation inside OpenMP regions.
class CGOpenMPRegionInfo : public CodeGenFunction::CGCapturedStmtInfo {
public:
  /// \brief Kinds of OpenMP regions used in codegen.
  enum CGOpenMPRegionKind {
    /// \brief Region with outlined function for standalone 'parallel'
    /// directive.
    ParallelOutlinedRegion,
    /// \brief Region with outlined function for standalone 'simd'
    /// directive.
    SimdOutlinedRegion,
    /// \brief Region with outlined function for standalone 'task' directive.
    TaskOutlinedRegion,
    /// \brief Region for constructs that do not require function outlining,
    /// like 'for', 'sections', 'atomic' etc. directives.
    InlinedRegion,
    /// \brief Region with outlined function for standalone 'target' directive.
    TargetRegion,
  };

  CGOpenMPRegionInfo(const CapturedStmt &CS,
                     const CGOpenMPRegionKind RegionKind,
                     const RegionCodeGenTy &CodeGen, OpenMPDirectiveKind Kind,
                     bool HasCancel)
      : CGCapturedStmtInfo(CS, CR_OpenMP), RegionKind(RegionKind),
        CodeGen(CodeGen), Kind(Kind), HasCancel(HasCancel) {}

  CGOpenMPRegionInfo(const CGOpenMPRegionKind RegionKind,
                     const RegionCodeGenTy &CodeGen, OpenMPDirectiveKind Kind,
                     bool HasCancel)
      : CGCapturedStmtInfo(CR_OpenMP), RegionKind(RegionKind), CodeGen(CodeGen),
        Kind(Kind), HasCancel(HasCancel) {}

  /// \brief Get a variable or parameter for storing the lane id
  /// inside OpenMP construct.
  virtual const VarDecl *getLaneIDVariable() const { return nullptr; }

  /// \brief Get a variable or parameter for storing the number of lanes
  /// inside OpenMP construct.
  virtual const VarDecl *getNumLanesVariable() const { return nullptr; }

  /// \brief Get a variable or parameter for storing global thread id
  /// inside OpenMP construct.
  virtual const VarDecl *getThreadIDVariable() const = 0;

  /// \brief Emit the captured statement body.
  void EmitBody(CodeGenFunction &CGF, const Stmt *S) override;

  /// \brief Get an LValue for the current ThreadID variable.
  /// \return LValue for thread id variable. This LValue always has type int32*.
  virtual LValue getThreadIDVariableLValue(CodeGenFunction &CGF);

  /// \brief Get an LValue for the current LaneID variable.
  /// \return LValue for lane id variable. This LValue always has type int32*.
  virtual LValue getLaneIDVariableLValue(CodeGenFunction &CGF);

  /// \brief Get an LValue for the current NumLanes variable.
  /// \return LValue for num lanes variable. This LValue always has type int32*.
  virtual LValue getNumLanesVariableLValue(CodeGenFunction &CGF);

  CGOpenMPRegionKind getRegionKind() const { return RegionKind; }

  OpenMPDirectiveKind getDirectiveKind() const { return Kind; }

  bool hasCancel() const { return HasCancel; }

  static bool classof(const CGCapturedStmtInfo *Info) {
    return Info->getKind() == CR_OpenMP;
  }

protected:
  CGOpenMPRegionKind RegionKind;
  RegionCodeGenTy CodeGen;
  OpenMPDirectiveKind Kind;
  bool HasCancel;
};

/// \brief API for captured statement code generation in OpenMP constructs.
class CGOpenMPOutlinedRegionInfo : public CGOpenMPRegionInfo {
public:
  CGOpenMPOutlinedRegionInfo(const CapturedStmt &CS, const VarDecl *ThreadIDVar,
                             const RegionCodeGenTy &CodeGen,
                             OpenMPDirectiveKind Kind, bool HasCancel)
      : CGOpenMPRegionInfo(CS, ParallelOutlinedRegion, CodeGen, Kind,
                           HasCancel),
        ThreadIDVar(ThreadIDVar) {
    assert(ThreadIDVar != nullptr && "No ThreadID in OpenMP region.");
  }
  /// \brief Get a variable or parameter for storing global thread id
  /// inside OpenMP construct.
  const VarDecl *getThreadIDVariable() const override { return ThreadIDVar; }

  /// \brief Get the name of the capture helper.
  StringRef getHelperName() const override { return ".omp_outlined."; }

  static bool classof(const CGCapturedStmtInfo *Info) {
    return CGOpenMPRegionInfo::classof(Info) &&
           cast<CGOpenMPRegionInfo>(Info)->getRegionKind() ==
               ParallelOutlinedRegion;
  }

private:
  /// \brief A variable or parameter storing global thread id for OpenMP
  /// constructs.
  const VarDecl *ThreadIDVar;
};

/// \brief API for captured statement code generation in OpenMP constructs.
class CGOpenMPSimdOutlinedRegionInfo : public CGOpenMPRegionInfo {
public:
  CGOpenMPSimdOutlinedRegionInfo(const CapturedStmt &CS,
                                 const VarDecl *LaneIDVar,
                                 const VarDecl *NumLanesVar,
                                 const RegionCodeGenTy &CodeGen,
                                 OpenMPDirectiveKind Kind)
      : CGOpenMPRegionInfo(CS, SimdOutlinedRegion, CodeGen, Kind, false),
        LaneIDVar(LaneIDVar), NumLanesVar(NumLanesVar) {
    assert(LaneIDVar != nullptr && "No LaneID in OpenMP region.");
    assert(NumLanesVar != nullptr && "No # Lanes in OpenMP region.");
  }

  /// \brief Get a variable or parameter for storing the lane id
  /// inside OpenMP construct.
  const VarDecl *getLaneIDVariable() const override { return LaneIDVar; }

  /// \brief Get a variable or parameter for storing the number of lanes
  /// inside OpenMP construct.
  const VarDecl *getNumLanesVariable() const override { return NumLanesVar; }

  /// \brief This is unused for simd regions.
  const VarDecl *getThreadIDVariable() const override { return nullptr; }

  /// \brief Get the name of the capture helper.
  StringRef getHelperName() const override { return ".omp_simd_outlined."; }

  static bool classof(const CGCapturedStmtInfo *Info) {
    return CGOpenMPRegionInfo::classof(Info) &&
           cast<CGOpenMPRegionInfo>(Info)->getRegionKind() ==
               SimdOutlinedRegion;
  }

private:
  /// \brief A variable or parameter storing the lane id for OpenMP
  /// constructs.
  const VarDecl *LaneIDVar;
  /// \brief A variable or parameter storing the number of lanes for OpenMP
  /// constructs.
  const VarDecl *NumLanesVar;
};
}

LValue CGOpenMPRegionInfo::getThreadIDVariableLValue(CodeGenFunction &CGF) {
  return CGF.EmitLoadOfPointerLValue(
      CGF.GetAddrOfLocalVar(getThreadIDVariable()),
      getThreadIDVariable()->getType()->castAs<PointerType>());
}

LValue CGOpenMPRegionInfo::getLaneIDVariableLValue(CodeGenFunction &CGF) {
  return CGF.EmitLoadOfPointerLValue(
      CGF.GetAddrOfLocalVar(getLaneIDVariable()),
      getLaneIDVariable()->getType()->castAs<PointerType>());
}

LValue CGOpenMPRegionInfo::getNumLanesVariableLValue(CodeGenFunction &CGF) {
  return CGF.EmitLoadOfPointerLValue(
      CGF.GetAddrOfLocalVar(getNumLanesVariable()),
      getNumLanesVariable()->getType()->castAs<PointerType>());
}

void CGOpenMPRegionInfo::EmitBody(CodeGenFunction &CGF, const Stmt * /*S*/) {
  if (!CGF.HaveInsertPoint())
    return;
  // 1.2.2 OpenMP Language Terminology
  // Structured block - An executable statement with a single entry at the
  // top and a single exit at the bottom.
  // The point of exit cannot be a branch out of the structured block.
  // longjmp() and throw() must not violate the entry/exit criteria.
  CGF.EHStack.pushTerminate();
  {
    CodeGenFunction::RunCleanupsScope Scope(CGF);
    CodeGen(CGF);
  }
  CGF.EHStack.popTerminate();
}

namespace {
class ParallelNestingLevelRAII {
private:
  int &ParallelNestingLevel;
  int Increment;

public:
  // If in Simd we increase the parallelism level by a bunch to make sure all
  // the Simd regions nested are implemented in a sequential way.
  ParallelNestingLevelRAII(int &ParallelNestingLevel, bool IsSimd = false)
      : ParallelNestingLevel(ParallelNestingLevel), Increment(IsSimd ? 10 : 1) {
    ParallelNestingLevel += Increment;
  }
  ~ParallelNestingLevelRAII() { ParallelNestingLevel -= Increment; }
};
} // namespace

llvm::Value *CGOpenMPRuntimeNVPTX::emitParallelOrTeamsOutlinedFunction(
    const OMPExecutableDirective &D, const VarDecl *ThreadIDVar,
    OpenMPDirectiveKind InnermostKind, const RegionCodeGenTy &CodeGen) {
  assert(ThreadIDVar->getType()->isPointerType() &&
         "thread id variable must be of type kmp_int32 *");

  llvm::Function *OutlinedFun = nullptr;
  if (isa<OMPTeamsDirective>(D)) {
    // no outlining happening for teams
  } else {
    const CapturedStmt *CS = cast<CapturedStmt>(D.getAssociatedStmt());
    CodeGenFunction CGF(CGM, true);
    bool HasCancel = false;
    if (auto *OPD = dyn_cast<OMPParallelDirective>(&D))
      HasCancel = OPD->hasCancel();
    else if (auto *OPSD = dyn_cast<OMPParallelSectionsDirective>(&D))
      HasCancel = OPSD->hasCancel();
    else if (auto *OPFD = dyn_cast<OMPParallelForDirective>(&D))
      HasCancel = OPFD->hasCancel();

    // Include updates in runtime parallelism level.
    auto &&CodeGenWithDataSharing = [this, &CodeGen](CodeGenFunction &CGF,
                                                     PrePostActionTy &) {
      increaseParallelismLevel(CGF);
      CodeGen(CGF);
      decreaseParallelismLevel(CGF);
    };

    // Save the current parallel context because it may be overwritten by the
    // innermost regions.
    const Decl *CurrentContext = CurrentParallelContext;

    CGOpenMPOutlinedRegionInfo CGInfo(*CS, ThreadIDVar, CodeGenWithDataSharing,
                                      InnermostKind, HasCancel);
    CodeGenFunction::CGCapturedStmtRAII CapInfoRAII(CGF, &CGInfo);
    {
      ParallelNestingLevelRAII NestingRAII(ParallelNestingLevel);
      // The outlined function takes as arguments the global_tid, bound_tid,
      // and a capture structure created from the captured variables.
      OutlinedFun = CGF.GenerateOpenMPCapturedStmtFunction(*CS);
    }
    auto *WrapperFun =
        createDataSharingParallelWrapper(D, *OutlinedFun, *CS, CurrentContext);
    WrapperFunctionsMap[OutlinedFun] = WrapperFun;
  }
  return OutlinedFun;
}

llvm::Value *CGOpenMPRuntimeNVPTX::emitSimdOutlinedFunction(
    const OMPExecutableDirective &D, const VarDecl *LaneIDVar,
    const VarDecl *NumLanesVar, OpenMPDirectiveKind InnermostKind,
    const RegionCodeGenTy &CodeGen) {
  llvm::Function *OutlinedFun = nullptr;

  const CapturedStmt *CS = cast<CapturedStmt>(D.getAssociatedStmt());

  // Include updates in runtime parallelism level.
  auto &&CodeGenWithDataSharing = [this, &CodeGen](CodeGenFunction &CGF,
                                                   PrePostActionTy &) {
    increaseParallelismLevel(CGF, /*IsSimd=*/true);
    CodeGen(CGF);
    decreaseParallelismLevel(CGF, /*IsSimd=*/true);
  };

  // Save the current parallel context because it may be overwritten by the
  // innermost regions.
  const Decl *CurrentContext = CurrentParallelContext;

  CodeGenFunction CGF(CGM, true);
  CGOpenMPSimdOutlinedRegionInfo CGInfo(*CS, LaneIDVar, NumLanesVar,
                                        CodeGenWithDataSharing, InnermostKind);
  CodeGenFunction::CGCapturedStmtRAII CapInfoRAII(CGF, &CGInfo);
  {
    ParallelNestingLevelRAII NestingRAII(ParallelNestingLevel, /*IsSimd=*/true);
    OutlinedFun = CGF.GenerateOpenMPCapturedStmtFunction(*CS);
  }

  auto *WrapperFun = createDataSharingParallelWrapper(
      D, *OutlinedFun, *CS, CurrentContext, /*IsSimd=*/true);
  WrapperFunctionsMap[OutlinedFun] = WrapperFun;
  return OutlinedFun;
}

bool CGOpenMPRuntimeNVPTX::InL0() {
  return !IsOrphaned && ParallelNestingLevel == 0;
}

bool CGOpenMPRuntimeNVPTX::InL1() {
  return !IsOrphaned && ParallelNestingLevel == 1;
}

bool CGOpenMPRuntimeNVPTX::InL1Plus() {
  return !IsOrphaned && ParallelNestingLevel >= 1;
}

bool CGOpenMPRuntimeNVPTX::IndeterminateLevel() { return IsOrphaned; }

// \brief Obtain the data sharing info for the current context.
const CGOpenMPRuntimeNVPTX::DataSharingInfo &
CGOpenMPRuntimeNVPTX::getDataSharingInfo(const Decl *Context) {
  assert(Context &&
         "A parallel region is expected to be enclosed in a context.");

  auto It = DataSharingInfoMap.find(Context);
  assert(It != DataSharingInfoMap.end() && "Data sharing info does not exist.");
  return It->second;
}

void CGOpenMPRuntimeNVPTX::createDataSharingInfo(CodeGenFunction &CGF) {
  auto &Context = CGF.CurCodeDecl;
  assert(Context &&
         "A parallel region is expected to be enclosed in a context.");

  ASTContext &C = CGM.getContext();

  if (DataSharingInfoMap.find(Context) != DataSharingInfoMap.end())
    return;

  auto &Info = DataSharingInfoMap[Context];

  // Get the body of the region. The region context is either a function or a
  // captured declaration.
  const Stmt *Body;
  if (auto *D = dyn_cast<CapturedDecl>(Context))
    Body = D->getBody();
  else
    Body = cast<FunctionDecl>(D)->getBody();

  // Find all the captures in all enclosed regions and obtain their captured
  // statements.
  SmallVector<const CapturedStmt *, 8> CapturedStmts;
  SmallVector<const Stmt *, 64> WorkList;
  WorkList.push_back(Body);
  while (!WorkList.empty()) {
    const Stmt *CurStmt = WorkList.pop_back_val();
    if (!CurStmt)
      continue;

    // Is this a parallel region.
    if (auto *Dir = dyn_cast<OMPExecutableDirective>(CurStmt)) {
      if (isOpenMPParallelDirective(Dir->getDirectiveKind()) ||
          isOpenMPSimdDirective(Dir->getDirectiveKind()))
        CapturedStmts.push_back(cast<CapturedStmt>(Dir->getAssociatedStmt()));
      else {
        if(Dir->hasAssociatedStmt()) {
          // Look into the associated statement of OpenMP directives.
          const CapturedStmt &CS = *cast<CapturedStmt>(Dir->getAssociatedStmt());
          CurStmt = CS.getCapturedStmt();

          WorkList.push_back(CurStmt);
        }
      }

      continue;
    }

    // Keep looking for other regions.
    WorkList.append(CurStmt->child_begin(), CurStmt->child_end());
  }

  assert(!CapturedStmts.empty() && "Expecting at least one parallel region!");

  // Scan the captured statements and generate a record to contain all the data
  // to be shared. Make sure we do not share the same thing twice.
  auto *SharedMasterRD =
      C.buildImplicitRecord("__openmp_nvptx_data_sharing_master_record");
  auto *SharedWarpRD =
      C.buildImplicitRecord("__openmp_nvptx_data_sharing_warp_record");
  SharedMasterRD->startDefinition();
  SharedWarpRD->startDefinition();

  llvm::SmallSet<const VarDecl *, 32> AlreadySharedDecls;
  for (auto *CS : CapturedStmts) {

    // add lower and upper bounds to recorddecl (slot)

    const RecordDecl *RD = CS->getCapturedRecordDecl();
    auto CurField = RD->field_begin();
    auto CurCap = CS->capture_begin();
    for (CapturedStmt::const_capture_init_iterator I = CS->capture_init_begin(),
                                                   E = CS->capture_init_end();
         I != E; ++I, ++CurField, ++CurCap) {

      const VarDecl *CurVD = nullptr;

      // Track the data sharing type.
      DataSharingInfo::DataSharingType DST = DataSharingInfo::DST_Val;

      if (CurField->hasCapturedVLAType()) {
        assert("VLAs are not yet supported in NVPTX target data sharing!");
        continue;
      } else if (CurCap->capturesThis()) {
        // We use null to indicate 'this'.
        CurVD = nullptr;
      } else if (CurCap->capturesVariableByCopy()) {
        assert("Not expecting to capture variables by copy in NVPTX target "
               "data sharing!");
        continue;
      } else {
        // Get the reference to the variable that is initializing the capture.
        const DeclRefExpr *DRE = cast<DeclRefExpr>(*I);
        CurVD = cast<VarDecl>(DRE->getDecl());

        assert(CurVD->hasLocalStorage() &&
               "Expecting to capture only variables with local storage.");

        // If we have an alloca for this variable, then we need to share the
        // storage too, not only the reference.
        auto *Val =
            cast<llvm::Instruction>(CGF.GetAddrOfLocalVar(CurVD).getPointer());
        if (isa<llvm::LoadInst>(Val))
          DST = DataSharingInfo::DST_Ref;
        else if (isa<llvm::BitCastInst>(Val))
          DST = DataSharingInfo::DST_Cast;
      }

      // Do not insert the same declaration twice.
      if (AlreadySharedDecls.count(CurVD))
        continue;

      AlreadySharedDecls.insert(CurVD);
      Info.add(CurVD, DST);

      QualType ElemTy = (*I)->getType();
      if (DST == DataSharingInfo::DST_Ref)
        ElemTy = C.getPointerType(ElemTy);

      addFieldToRecordDecl(C, SharedMasterRD, ElemTy);
      llvm::APInt NumElems(C.getTypeSize(C.getUIntPtrType()),
                           DS_Max_Worker_Warp_Size);
      auto QTy = C.getConstantArrayType(ElemTy, NumElems, ArrayType::Normal,
                                        /*IndexTypeQuals=*/0);
      addFieldToRecordDecl(C, SharedWarpRD, QTy);
    }
  }

  SharedMasterRD->completeDefinition();
  SharedWarpRD->completeDefinition();
  Info.MasterRecordType = C.getRecordType(SharedMasterRD);
  Info.WorkerWarpRecordType = C.getRecordType(SharedWarpRD);

  return;
}

void CGOpenMPRuntimeNVPTX::createDataSharingPerFunctionInfrastructure(
    CodeGenFunction &EnclosingCGF) {
  const Decl *CD = EnclosingCGF.CurCodeDecl;
  auto &Ctx = CGM.getContext();

  assert(CD && "Function does not have a context associated!");

  // Create the data sharing information.
  auto &DSI = getDataSharingInfo(CD);

  // If there is nothing being captured in the parallel regions, we do not need
  // to do anything.
  if (DSI.CapturesValues.empty())
    return;

  auto &EnclosingFuncInfo = DataSharingFunctionInfoMap[EnclosingCGF.CurFn];

  // If we already have a data sharing initializer of this function, don't need
  // to create a new one.
  if (EnclosingFuncInfo.InitializationFunction)
    return;

  auto IsEntryPoint = EnclosingFuncInfo.IsEntryPoint;

  // Create function to do the initialization. The first four arguments are the
  // slot/stack/frame saved addresses and then we have pairs of pointers to the
  // shared address and each declaration to be shared.
  // FunctionArgList ArgList;
  SmallVector<ImplicitParamDecl, 4> ArgImplDecls;

  // Create the variables to save the slot, stack, frame and active threads.
  QualType SlotPtrTy = Ctx.getPointerType(getDataSharingSlotQty());
  QualType Int32QTy =
      Ctx.getIntTypeForBitwidth(/*DestWidth=*/32, /*Signed=*/false);
  ArgImplDecls.push_back(ImplicitParamDecl(
      Ctx, /*DC=*/nullptr, SourceLocation(),
      &Ctx.Idents.get("data_share_saved_slot"), Ctx.getPointerType(SlotPtrTy)));
  ArgImplDecls.push_back(
      ImplicitParamDecl(Ctx, /*DC=*/nullptr, SourceLocation(),
                        &Ctx.Idents.get("data_share_saved_stack"),
                        Ctx.getPointerType(Ctx.VoidPtrTy)));
  ArgImplDecls.push_back(
      ImplicitParamDecl(Ctx, /*DC=*/nullptr, SourceLocation(),
                        &Ctx.Idents.get("data_share_saved_frame"),
                        Ctx.getPointerType(Ctx.VoidPtrTy)));
  ArgImplDecls.push_back(
      ImplicitParamDecl(Ctx, /*DC=*/nullptr, SourceLocation(),
                        &Ctx.Idents.get("data_share_active_threads"),
                        Ctx.getPointerType(Int32QTy)));

  auto *MasterRD = DSI.MasterRecordType->getAs<RecordType>()->getDecl();
  auto CapturesIt = DSI.CapturesValues.begin();
  for (auto *F : MasterRD->fields()) {
    QualType ArgTy = F->getType();

    // If this is not a reference the right address type is the pointer type of
    // the type that is the record.
    if (CapturesIt->second != DataSharingInfo::DST_Ref)
      ArgTy = Ctx.getPointerType(ArgTy);

    StringRef BaseName =
        CapturesIt->first ? CapturesIt->first->getName() : "this";

    // If this is not a reference, we need to return by reference the new
    // address to be replaced.
    if (CapturesIt->second != DataSharingInfo::DST_Ref) {
      std::string Name = BaseName;
      Name += ".addr";
      auto &NameID = Ctx.Idents.get(Name);
      ImplicitParamDecl D(Ctx, /*DC=*/nullptr, SourceLocation(), &NameID,
                          Ctx.getPointerType(ArgTy));
      ArgImplDecls.push_back(D);
    }

    std::string NameOrig = BaseName;
    NameOrig += ".orig";
    auto &NameOrigID = Ctx.Idents.get(NameOrig);
    ImplicitParamDecl OrigD(Ctx, /*DC=*/nullptr, SourceLocation(), &NameOrigID,
                            ArgTy);
    ArgImplDecls.push_back(OrigD);

    ++CapturesIt;
  }

  FunctionArgList ArgList;
  for (auto &I : ArgImplDecls)
    ArgList.push_back(&I);

  auto &CGFI =
      CGM.getTypes().arrangeBuiltinFunctionDeclaration(Ctx.VoidTy, ArgList);
  auto *Fn = llvm::Function::Create(
      CGM.getTypes().GetFunctionType(CGFI), llvm::GlobalValue::InternalLinkage,
      EnclosingCGF.CurFn->getName() + ".data_share", &CGM.getModule());
  CGM.SetInternalFunctionAttributes(/*D=*/nullptr, Fn, CGFI);
  Fn->setLinkage(llvm::GlobalValue::InternalLinkage);

  CodeGenFunction CGF(CGM, /*suppressNewContext=*/true);
  CGF.StartFunction(GlobalDecl(), Ctx.VoidTy, Fn, CGFI, ArgList);

  // If this is an entry point, all the threads except the master should skip
  // this.
  auto *ExitBB = CGF.createBasicBlock(".exit");
  if (IsEntryPoint) {
    auto *MasterBB = CGF.createBasicBlock(".master");
    auto *Cond =
        CGF.Builder.CreateICmpEQ(getMasterThreadID(CGF), getNVPTXThreadID(CGF));
    CGF.Builder.CreateCondBr(Cond, MasterBB, ExitBB);
    CGF.EmitBlock(MasterBB);
  }

  // Create the variables to save the slot, stack, frame and active threads.
  auto ArgsIt = ArgList.begin();
  auto SavedSlotAddr =
      CGF.EmitLoadOfPointer(CGF.GetAddrOfLocalVar(*ArgsIt),
                            (*ArgsIt)->getType()->getAs<PointerType>());
  ++ArgsIt;
  auto SavedStackAddr =
      CGF.EmitLoadOfPointer(CGF.GetAddrOfLocalVar(*ArgsIt),
                            (*ArgsIt)->getType()->getAs<PointerType>());
  ++ArgsIt;
  auto SavedFrameAddr =
      CGF.EmitLoadOfPointer(CGF.GetAddrOfLocalVar(*ArgsIt),
                            (*ArgsIt)->getType()->getAs<PointerType>());
  ++ArgsIt;
  auto SavedActiveThreadsAddr =
      CGF.EmitLoadOfPointer(CGF.GetAddrOfLocalVar(*ArgsIt),
                            (*ArgsIt)->getType()->getAs<PointerType>());
  ++ArgsIt;

  auto *SavedSlot = SavedSlotAddr.getPointer();
  auto *SavedStack = SavedStackAddr.getPointer();
  auto *SavedFrame = SavedFrameAddr.getPointer();
  auto *SavedActiveThreads = SavedActiveThreadsAddr.getPointer();

  // Get the addresses where each data shared address will be stored.
  SmallVector<Address, 32> NewAddressPtrs;
  SmallVector<Address, 32> OrigAddresses;
  // We iterate two by two.
  for (auto CapturesIt = DSI.CapturesValues.begin(); ArgsIt != ArgList.end();
       ++ArgsIt, ++CapturesIt) {
    if (CapturesIt->second != DataSharingInfo::DST_Ref) {
      NewAddressPtrs.push_back(
          CGF.EmitLoadOfPointer(CGF.GetAddrOfLocalVar(*ArgsIt),
                                (*ArgsIt)->getType()->getAs<PointerType>()));
      ++ArgsIt;
    }

    OrigAddresses.push_back(
        CGF.EmitLoadOfPointer(CGF.GetAddrOfLocalVar(*ArgsIt),
                              (*ArgsIt)->getType()->getAs<PointerType>()));
  }

  auto &&L0ParallelGen = [this, &DSI, MasterRD, &Ctx, SavedSlot, SavedStack,
                          SavedFrame, SavedActiveThreads, &NewAddressPtrs,
                          &OrigAddresses](CodeGenFunction &CGF,
                                          PrePostActionTy &) {
    auto &Bld = CGF.Builder;

    // In the Level 0 regions, we use the master record to get the data.
    auto *DataSize = llvm::ConstantInt::get(
        CGM.SizeTy, Ctx.getTypeSizeInChars(DSI.MasterRecordType).getQuantity());
    auto *DefaultDataSize = llvm::ConstantInt::get(CGM.SizeTy, DS_Slot_Size);

    llvm::Value *Args[] = {SavedSlot,          SavedStack, SavedFrame,
                           SavedActiveThreads, DataSize,   DefaultDataSize};
    auto *DataShareAddr =
        Bld.CreateCall(createNVPTXRuntimeFunction(
                           OMPRTL_NVPTX__kmpc_data_sharing_environment_begin),
                       Args, "data_share_master_addr");
    auto DataSharePtrQTy = Ctx.getPointerType(DSI.MasterRecordType);
    auto *DataSharePtrTy = CGF.getTypes().ConvertTypeForMem(DataSharePtrQTy);
    auto *CasterDataShareAddr =
        Bld.CreateBitOrPointerCast(DataShareAddr, DataSharePtrTy);

    // For each field, return the address by reference if it is not a reference
    // capture, otherwise copy the original pointer to the shared address space.
    // If it is a cast, we need to copy the pointee into shared memory.
    auto FI = MasterRD->field_begin();
    auto CapturesIt = DSI.CapturesValues.begin();
    auto NewAddressIt = NewAddressPtrs.begin();
    for (unsigned i = 0; i < OrigAddresses.size(); ++i, ++FI, ++CapturesIt) {
      llvm::Value *Idx[] = {Bld.getInt32(0), Bld.getInt32(i)};
      auto *NewAddr = Bld.CreateInBoundsGEP(CasterDataShareAddr, Idx);

      switch (CapturesIt->second) {
      case DataSharingInfo::DST_Ref: {
        auto Addr = CGF.MakeNaturalAlignAddrLValue(NewAddr, FI->getType());
        CGF.EmitStoreOfScalar(OrigAddresses[i].getPointer(), Addr);
      } break;
      case DataSharingInfo::DST_Cast: {
        // Copy the pointee to the new location.
        auto *PointeeVal =
            CGF.EmitLoadOfScalar(OrigAddresses[i], /*Volatiole=*/false,
                                 FI->getType(), SourceLocation());
        auto NewAddrLVal =
            CGF.MakeNaturalAlignAddrLValue(NewAddr, FI->getType());
        CGF.EmitStoreOfScalar(PointeeVal, NewAddrLVal);
      } // fallthrough.
      case DataSharingInfo::DST_Val: {
        CGF.EmitStoreOfScalar(NewAddr, *NewAddressIt, /*Volatile=*/false,
                              Ctx.getPointerType(FI->getType()));
        ++NewAddressIt;
      } break;
      }
    }
  };
  auto &&L1ParallelGen = [this, &DSI, MasterRD, &Ctx, SavedSlot, SavedStack,
                          SavedFrame, SavedActiveThreads, &NewAddressPtrs,
                          &OrigAddresses](CodeGenFunction &CGF,
                                          PrePostActionTy &) {
    auto &Bld = CGF.Builder;

    // In the Level 1 regions, we use the worker record that has each capture
    // organized as an array.
    auto *DataSize = llvm::ConstantInt::get(
        CGM.SizeTy,
        Ctx.getTypeSizeInChars(DSI.WorkerWarpRecordType).getQuantity());
    auto *DefaultDataSize =
        llvm::ConstantInt::get(CGM.SizeTy, DS_Worker_Warp_Slot_Size);

    llvm::Value *Args[] = {SavedSlot,          SavedStack, SavedFrame,
                           SavedActiveThreads, DataSize,   DefaultDataSize};
    auto *DataShareAddr =
        Bld.CreateCall(createNVPTXRuntimeFunction(
                           OMPRTL_NVPTX__kmpc_data_sharing_environment_begin),
                       Args, "data_share_master_addr");
    auto DataSharePtrQTy = Ctx.getPointerType(DSI.WorkerWarpRecordType);
    auto *DataSharePtrTy = CGF.getTypes().ConvertTypeForMem(DataSharePtrQTy);
    auto *CasterDataShareAddr =
        Bld.CreateBitOrPointerCast(DataShareAddr, DataSharePtrTy);

    // Get the threadID in the warp. We have a frame per warp.
    auto *ThreadWarpID = getNVPTXThreadWarpID(CGF);

    // For each field, generate the shared address and store it in the new
    // addresses array.
    auto FI = MasterRD->field_begin();
    auto CapturesIt = DSI.CapturesValues.begin();
    auto NewAddressIt = NewAddressPtrs.begin();
    for (unsigned i = 0; i < OrigAddresses.size(); ++i, ++FI, ++CapturesIt) {
      llvm::Value *Idx[] = {Bld.getInt32(0), Bld.getInt32(i), ThreadWarpID};
      auto *NewAddr = Bld.CreateInBoundsGEP(CasterDataShareAddr, Idx);

      switch (CapturesIt->second) {
      case DataSharingInfo::DST_Ref: {
        auto Addr = CGF.MakeNaturalAlignAddrLValue(NewAddr, FI->getType());
        CGF.EmitStoreOfScalar(OrigAddresses[i].getPointer(), Addr);
      } break;
      case DataSharingInfo::DST_Cast: {
        // Copy the pointee to the new location.
        auto *PointeeVal =
            CGF.EmitLoadOfScalar(OrigAddresses[i], /*Volatiole=*/false,
                                 FI->getType(), SourceLocation());
        auto NewAddrLVal =
            CGF.MakeNaturalAlignAddrLValue(NewAddr, FI->getType());
        CGF.EmitStoreOfScalar(PointeeVal, NewAddrLVal);
      } // fallthrough.
      case DataSharingInfo::DST_Val: {
        CGF.EmitStoreOfScalar(NewAddr, *NewAddressIt, /*Volatile=*/false,
                              Ctx.getPointerType(FI->getType()));
        ++NewAddressIt;
      } break;
      }
    }
  };
  auto &&Sequential = [this, &DSI, &Ctx, MasterRD, &NewAddressPtrs,
                       &OrigAddresses](CodeGenFunction &CGF,
                                       PrePostActionTy &) {
    // In the sequential regions, we just use the regular allocas.
    auto FI = MasterRD->field_begin();
    auto CapturesIt = DSI.CapturesValues.begin();
    auto NewAddressIt = NewAddressPtrs.begin();
    for (unsigned i = 0; i < OrigAddresses.size(); ++i, ++FI, ++CapturesIt) {
      // If capturing a reference, the original value will be used.
      if (CapturesIt->second == DataSharingInfo::DST_Ref)
        continue;

      llvm::Value *OriginalVal = OrigAddresses[i].getPointer();
      CGF.EmitStoreOfScalar(OriginalVal, *NewAddressIt,
                            /*Volatile=*/false,
                            Ctx.getPointerType(FI->getType()));
      ++NewAddressIt;
    }
  };

  emitParallelismLevelCode(CGF, L0ParallelGen, L1ParallelGen, Sequential);

  // Generate the values to replace.
  auto FI = MasterRD->field_begin();
  for (unsigned i = 0; i < OrigAddresses.size(); ++i, ++FI) {
    llvm::Value *OriginalVal = nullptr;
    if (DSI.CapturesValues[i].first) {
      OriginalVal = EnclosingCGF.GetAddrOfLocalVar(DSI.CapturesValues[i].first)
                        .getPointer();
    } else
      OriginalVal = CGF.LoadCXXThis();

    assert(OriginalVal && "Can't obtain value to replace with??");

    EnclosingFuncInfo.ValuesToBeReplaced.push_back(OriginalVal);

    //    llvm::errs() << "Inserting instruction to be replaced:\n";
    //    OriginalVal->dump();
  }

  CGF.EmitBlock(ExitBB);
  CGF.FinishFunction();

  EnclosingFuncInfo.InitializationFunction = CGF.CurFn;
}

// \brief Create the data sharing arguments and call the parallel outlined
// function.
llvm::Function *CGOpenMPRuntimeNVPTX::createDataSharingParallelWrapper(
    const OMPExecutableDirective &D, llvm::Function &OutlinedParallelFn,
    const CapturedStmt &CS, const Decl *CurrentContext, bool IsSimd) {
  auto &Ctx = CGM.getContext();

  // Create a function that takes as argument the source lane.
  FunctionArgList WrapperArgs;
  QualType Int32QTy =
      Ctx.getIntTypeForBitwidth(/*DestWidth=*/32, /*Signed=*/false);
  QualType Int32PtrQTy = Ctx.getPointerType(Int32QTy);
  ImplicitParamDecl WrapperArg(Ctx, /*DC=*/nullptr, SourceLocation(),
                               /*Id=*/nullptr, Int32QTy);
  ImplicitParamDecl WrapperLaneArg(Ctx, /*DC=*/nullptr, SourceLocation(),
                                   /*Id=*/nullptr, Int32PtrQTy);
  ImplicitParamDecl WrapperNumLanesArg(Ctx, /*DC=*/nullptr, SourceLocation(),
                                       /*Id=*/nullptr, Int32PtrQTy);
  WrapperArgs.push_back(&WrapperArg);
  if (IsSimd) {
    WrapperArgs.push_back(&WrapperLaneArg);
    WrapperArgs.push_back(&WrapperNumLanesArg);
  }

  auto &CGFI =
      CGM.getTypes().arrangeBuiltinFunctionDeclaration(Ctx.VoidTy, WrapperArgs);

  auto *Fn = llvm::Function::Create(
      CGM.getTypes().GetFunctionType(CGFI), llvm::GlobalValue::InternalLinkage,
      OutlinedParallelFn.getName() + "_wrapper", &CGM.getModule());
  CGM.SetInternalFunctionAttributes(/*D=*/nullptr, Fn, CGFI);
  Fn->setLinkage(llvm::GlobalValue::InternalLinkage);

  CodeGenFunction CGF(CGM, /*suppressNewContext=*/true);
  CGF.StartFunction(GlobalDecl(), Ctx.VoidTy, Fn, CGFI, WrapperArgs);

  // Get the source thread ID, it is the argument of the current function.
  auto SourceLaneIDAddr = CGF.GetAddrOfLocalVar(&WrapperArg);
  auto *SourceLaneID = CGF.EmitLoadOfScalar(
      SourceLaneIDAddr, /*Volatile=*/false, Int32QTy, SourceLocation());

  // Create temporary variables to contain the new args.
  SmallVector<Address, 32> ArgsAddresses;

  auto *RD = CS.getCapturedRecordDecl();
  auto CurField = RD->field_begin();
  for (CapturedStmt::const_capture_iterator CI = CS.capture_begin(),
                                            CE = CS.capture_end();
       CI != CE; ++CI, ++CurField) {
    assert(!CI->capturesVariableArrayType() && "Not expecting to capture VLA!");
    assert(!CI->capturesVariableByCopy() &&
           "Not expecting to capture by copy values!");

    StringRef Name;
    if (CI->capturesThis())
      Name = "this";
    else
      Name = CI->getCapturedVar()->getName();

    ArgsAddresses.push_back(
        CGF.CreateMemTemp(CurField->getType(), Name + ".addr"));
  }

  // Get the data sharing information for the context that encloses the current
  // one.
  auto &DSI = getDataSharingInfo(CurrentContext);

  auto &&L0ParallelGen = [this, &DSI, &Ctx, &CS, &RD, &ArgsAddresses,
                          SourceLaneID](CodeGenFunction &CGF,
                                        PrePostActionTy &) {
    auto &Bld = CGF.Builder;

    // In the Level 0 regions, we need to get the record of the master thread.
    auto *DataAddr = Bld.CreateCall(
        createNVPTXRuntimeFunction(
            OMPRTL_NVPTX__kmpc_get_data_sharing_environment_frame),
        getMasterThreadID(CGF));
    auto *RTy = CGF.getTypes().ConvertTypeForMem(DSI.MasterRecordType);
    auto *CastedDataAddr =
        Bld.CreateBitOrPointerCast(DataAddr, RTy->getPointerTo());

    // For each capture obtain the pointer by calculating the right offset in
    // the host record.
    unsigned ArgsIdx = 0;
    auto FI =
        DSI.MasterRecordType->getAs<RecordType>()->getDecl()->field_begin();
    for (CapturedStmt::const_capture_iterator CI = CS.capture_begin(),
                                              CE = CS.capture_end();
         CI != CE; ++CI, ++ArgsIdx, ++FI) {
      const VarDecl *VD = CI->capturesThis() ? nullptr : CI->getCapturedVar();
      unsigned Idx = 0;
      for (; Idx < DSI.CapturesValues.size(); ++Idx)
        if (DSI.CapturesValues[Idx].first == VD)
          break;
      assert(Idx != DSI.CapturesValues.size() && "Capture must exist!");

      llvm::Value *Idxs[] = {Bld.getInt32(0), Bld.getInt32(Idx)};
      auto *Arg = Bld.CreateInBoundsGEP(CastedDataAddr, Idxs);

      // If what is being shared is the reference, we should load it.
      if (DSI.CapturesValues[Idx].second == DataSharingInfo::DST_Ref) {
        auto Addr = CGF.MakeNaturalAlignAddrLValue(Arg, FI->getType());
        Arg = CGF.EmitLoadOfScalar(Addr, SourceLocation());
        CGF.EmitStoreOfScalar(Arg, ArgsAddresses[ArgsIdx], /*Volatile=*/false,
                              FI->getType());
      } else
        CGF.EmitStoreOfScalar(Arg, ArgsAddresses[ArgsIdx], /*Volatile=*/false,
                              Ctx.getPointerType(FI->getType()));
    }

    // Get the extra ub and lb.

  };

  auto &&L1ParallelGen = [this, &DSI, &Ctx, &CS, &RD, &ArgsAddresses,
                          SourceLaneID](CodeGenFunction &CGF,
                                        PrePostActionTy &) {
    auto &Bld = CGF.Builder;

    // In the Level 1 regions, we need to get the record of the current worker
    // thread.
    auto *DataAddr = Bld.CreateCall(
        createNVPTXRuntimeFunction(
            OMPRTL_NVPTX__kmpc_get_data_sharing_environment_frame),
        getNVPTXThreadID(CGF));
    auto *RTy = CGF.getTypes().ConvertTypeForMem(DSI.WorkerWarpRecordType);
    auto *CastedDataAddr =
        Bld.CreateBitOrPointerCast(DataAddr, RTy->getPointerTo());

    // For each capture obtain the pointer by calculating the right offset in
    // the host record.
    unsigned ArgsIdx = 0;
    auto FI =
        DSI.MasterRecordType->getAs<RecordType>()->getDecl()->field_begin();
    for (CapturedStmt::const_capture_iterator CI = CS.capture_begin(),
                                              CE = CS.capture_end();
         CI != CE; ++CI, ++ArgsIdx, ++FI) {
      const VarDecl *VD = CI->capturesThis() ? nullptr : CI->getCapturedVar();
      unsigned Idx = 0;
      for (; Idx < DSI.CapturesValues.size(); ++Idx)
        if (DSI.CapturesValues[Idx].first == VD)
          break;
      assert(Idx != DSI.CapturesValues.size() && "Capture must exist!");

      llvm::Value *Idxs[] = {Bld.getInt32(0), Bld.getInt32(Idx), SourceLaneID};
      auto *Arg = Bld.CreateInBoundsGEP(CastedDataAddr, Idxs);

      // If the what is being shared is the reference, we should load it.
      if (DSI.CapturesValues[Idx].second == DataSharingInfo::DST_Ref) {
        auto Addr = CGF.MakeNaturalAlignAddrLValue(Arg, FI->getType());
        Arg = CGF.EmitLoadOfScalar(Addr, SourceLocation());
        CGF.EmitStoreOfScalar(Arg, ArgsAddresses[ArgsIdx], /*Volatile=*/false,
                              FI->getType());
      } else
        CGF.EmitStoreOfScalar(Arg, ArgsAddresses[ArgsIdx], /*Volatile=*/false,
                              Ctx.getPointerType(FI->getType()));
    }
  };
  auto &&Sequential = [](CodeGenFunction &CGF, PrePostActionTy &) {
    // A sequential region does not use the wrapper.
  };

  // In Simd we only support L1 level.
  if (IsSimd)
    emitParallelismLevelCode(CGF, Sequential, L1ParallelGen, Sequential);
  else
    emitParallelismLevelCode(CGF, L0ParallelGen, L1ParallelGen, Sequential);

  // Get the array of arguments.
  SmallVector<llvm::Value *, 8> Args;

  if (IsSimd) {
    auto *LaneID =
        CGF.EmitLoadOfScalar(CGF.GetAddrOfLocalVar(&WrapperLaneArg),
                             /*Volatile=*/false, Int32PtrQTy, SourceLocation());
    auto *NumLanes =
        CGF.EmitLoadOfScalar(CGF.GetAddrOfLocalVar(&WrapperNumLanesArg),
                             /*Volatile=*/false, Int32PtrQTy, SourceLocation());
    Args.push_back(LaneID);
    Args.push_back(NumLanes);
  } else {
    Args.push_back(llvm::Constant::getNullValue(CGM.Int32Ty->getPointerTo()));
    Args.push_back(llvm::Constant::getNullValue(CGM.Int32Ty->getPointerTo()));
    if (D.getDirectiveKind() == OMPD_distribute_parallel_for) {
      // combining distribute with for requires sharing each distribute chunk
      // lower and upper bounds with the pragma for chunking mechanism
      // TODO: add support for composite distribute parallel for
      Args.push_back(llvm::Constant::getNullValue(CGM.Int32Ty));
      Args.push_back(llvm::Constant::getNullValue(CGM.Int32Ty));
    }
  }

  auto FI = DSI.MasterRecordType->getAs<RecordType>()->getDecl()->field_begin();
  for (unsigned i = 0; i < ArgsAddresses.size(); ++i, ++FI) {
    auto *Arg = CGF.EmitLoadOfScalar(ArgsAddresses[i], /*Volatile=*/false,
                                     Ctx.getPointerType(FI->getType()),
                                     SourceLocation());
    Args.push_back(Arg);
  }

  CGF.EmitCallOrInvoke(&OutlinedParallelFn, Args);
  CGF.FinishFunction();
  return Fn;
}

// \brief Emit the code that each thread requires to execute when it encounters
// one of the three possible parallelism level. This also emits the required
// data sharing code for each level.
void CGOpenMPRuntimeNVPTX::emitParallelismLevelCode(
    CodeGenFunction &CGF, const RegionCodeGenTy &Level0,
    const RegionCodeGenTy &Level1, const RegionCodeGenTy &Sequential) {
  auto &Bld = CGF.Builder;

  // Flags that prevent code to be emitted if it can be proven that threads
  // cannot reach this function at a given level.
  //
  // FIXME: This current relies on a simple analysis that may not be correct if
  // we have function in a target region.
  bool OnlyInL0 = InL0();
  bool OnlyInL1 = InL1();
  bool OnlySequential = !IsOrphaned && !InL0() && !InL1();

  // Emit runtime checks if we cannot prove this code is reached only at a
  // certain parallelism level.
  //
  // For each level i the code will look like:
  //
  //   isLevel = icmp Level, i;
  //   br isLevel, .leveli.parallel, .next.parallel
  //
  // .leveli.parallel:
  //   ; code for level i + shared data code
  //   br .after.parallel
  //
  // .next.parallel

  llvm::BasicBlock *AfterBB = CGF.createBasicBlock(".after.parallel");

  // Do we need to emit L0 code?
  if (!OnlyInL1 && !OnlySequential) {
    llvm::BasicBlock *LBB = CGF.createBasicBlock(".level0.parallel");
    llvm::BasicBlock *NextBB = nullptr;

    // Do we need runtime checks
    if (!OnlyInL0) {
      NextBB = CGF.createBasicBlock(".next.parallel");
      auto *ThreadID = getNVPTXThreadID(CGF);
      auto *MasterID = getMasterThreadID(CGF);
      auto *Cond = Bld.CreateICmpEQ(ThreadID, MasterID);
      Bld.CreateCondBr(Cond, LBB, NextBB);
    }

    CGF.EmitBlock(LBB);

    Level0(CGF);

    CGF.EmitBranch(AfterBB);
    if (NextBB)
      CGF.EmitBlock(NextBB);
  }

  // Do we need to emit L1 code?
  if (!OnlyInL0 && !OnlySequential) {
    llvm::BasicBlock *LBB = CGF.createBasicBlock(".level1.parallel");
    llvm::BasicBlock *NextBB = nullptr;

    // Do we need runtime checks
    if (!OnlyInL1) {
      NextBB = CGF.createBasicBlock(".next.parallel");
      auto *ParallelLevelVal = getParallelismLevel(CGF);
      auto *Cond = Bld.CreateICmpEQ(ParallelLevelVal, Bld.getInt32(1));
      Bld.CreateCondBr(Cond, LBB, NextBB);
    }

    CGF.EmitBlock(LBB);

    Level1(CGF);

    CGF.EmitBranch(AfterBB);
    if (NextBB)
      CGF.EmitBlock(NextBB);
  }

  // Do we need to emit sequential code?
  if (!OnlyInL0 && !OnlyInL1) {
    llvm::BasicBlock *SeqBB = CGF.createBasicBlock(".sequential.parallel");

    // Do we need runtime checks
    if (!OnlySequential) {
      auto *ParallelLevelVal = getParallelismLevel(CGF);
      auto *Cond = Bld.CreateICmpSGT(ParallelLevelVal, Bld.getInt32(1));
      Bld.CreateCondBr(Cond, SeqBB, AfterBB);
    }

    CGF.EmitBlock(SeqBB);
    Sequential(CGF);
  }

  CGF.EmitBlock(AfterBB);
}

void CGOpenMPRuntimeNVPTX::emitParallelCall(
    CodeGenFunction &CGF, SourceLocation Loc, llvm::Value *OutlinedFn,
    ArrayRef<llvm::Value *> CapturedVars, const Expr *IfCond) {
  if (!CGF.HaveInsertPoint())
    return;

  llvm::Function *Fn = cast<llvm::Function>(OutlinedFn);
  llvm::Function *WFn = WrapperFunctionsMap[Fn];
  assert(WFn && "Wrapper function does not exist??");

  // Force inline this outlined function at its call site.
  // Fn->addFnAttr(llvm::Attribute::AlwaysInline);
  Fn->setLinkage(llvm::GlobalValue::InternalLinkage);

  // Emit code that does the data sharing changes in the beginning of the
  // function.
  createDataSharingPerFunctionInfrastructure(CGF);

  auto *RTLoc = emitUpdateLocation(CGF, Loc);
  auto &&L0ParallelGen = [this, WFn, &CapturedVars](CodeGenFunction &CGF,
                                                    PrePostActionTy &) {
    CGBuilderTy &Bld = CGF.Builder;

    auto ID = Bld.CreateBitOrPointerCast(WFn, CGM.Int8PtrTy);

    // Prepare for parallel region. Indicate the outlined function.
    llvm::Value *Args[] = {ID};
    CGF.EmitRuntimeCall(
        createNVPTXRuntimeFunction(OMPRTL_NVPTX__kmpc_kernel_prepare_parallel),
        Args);

    // Activate workers.
    syncCTAThreads(CGF);

    // Barrier at end of parallel region.
    syncCTAThreads(CGF);

    // Remember for post-processing in worker loop.
    Work.push_back(WFn);
  };
  auto &&L1ParallelGen = [this, WFn, &CapturedVars, &RTLoc,
                          &Loc](CodeGenFunction &CGF, PrePostActionTy &) {
    CGBuilderTy &Bld = CGF.Builder;
    clang::ASTContext &Ctx = CGF.getContext();

    Address IsFinal =
        CGF.CreateTempAlloca(CGF.Int8Ty, CharUnits::fromQuantity(1),
                             /*Name*/ "is_final");
    Address WorkSource =
        CGF.CreateTempAlloca(CGF.Int32Ty, CharUnits::fromQuantity(4),
                             /*Name*/ "work_source");
    llvm::APInt TaskBufferSize(/*numBits=*/32, TASK_STATE_SIZE);
    auto TaskBufferTy = Ctx.getConstantArrayType(
        Ctx.CharTy, TaskBufferSize, ArrayType::Normal, /*IndexTypeQuals=*/0);
    auto TaskState = CGF.CreateMemTemp(TaskBufferTy, CharUnits::fromQuantity(8),
                                       /*Name=*/"task_state")
                         .getPointer();
    CGF.InitTempAlloca(IsFinal, Bld.getInt8(/*C=*/0));
    CGF.InitTempAlloca(WorkSource, Bld.getInt32(/*C=*/-1));

    llvm::BasicBlock *DoBodyBB = CGF.createBasicBlock(".do.body");
    llvm::BasicBlock *ExecuteBB = CGF.createBasicBlock(".do.body.execute");
    llvm::BasicBlock *DoCondBB = CGF.createBasicBlock(".do.cond");
    llvm::BasicBlock *DoEndBB = CGF.createBasicBlock(".do.end");

    CGF.EmitBranch(DoBodyBB);
    CGF.EmitBlock(DoBodyBB);
    auto ArrayDecay = Bld.CreateConstInBoundsGEP2_32(
        llvm::ArrayType::get(CGM.Int8Ty, TASK_STATE_SIZE), TaskState,
        /*Idx0=*/0, /*Idx1=*/0);
    llvm::Value *Args[] = {ArrayDecay, IsFinal.getPointer(),
                           WorkSource.getPointer()};
    llvm::Value *IsActive =
        CGF.EmitRuntimeCall(createNVPTXRuntimeFunction(
                                OMPRTL_NVPTX__kmpc_kernel_convergent_parallel),
                            Args);
    Bld.CreateCondBr(IsActive, ExecuteBB, DoCondBB);

    CGF.EmitBlock(ExecuteBB);

    // Execute the work, and pass the thread source from where the data should
    // be used.
    auto *SourceThread = CGF.EmitLoadOfScalar(
        WorkSource, /*Volatile=*/false,
        Ctx.getIntTypeForBitwidth(/*DestWidth=*/32, /*Signed=*/false),
        SourceLocation());
    CGF.EmitCallOrInvoke(WFn, SourceThread);
    ArrayDecay = Bld.CreateConstInBoundsGEP2_32(
        llvm::ArrayType::get(CGM.Int8Ty, TASK_STATE_SIZE), TaskState,
        /*Idx0=*/0, /*Idx1=*/0);
    llvm::Value *EndArgs[] = {ArrayDecay};
    CGF.EmitRuntimeCall(createNVPTXRuntimeFunction(
                            OMPRTL_NVPTX__kmpc_kernel_end_convergent_parallel),
                        EndArgs);
    CGF.EmitBranch(DoCondBB);

    CGF.EmitBlock(DoCondBB);
    llvm::Value *IsDone = Bld.CreateICmpEQ(Bld.CreateLoad(IsFinal),
                                           Bld.getInt8(/*C=*/1), "is_done");
    Bld.CreateCondBr(IsDone, DoEndBB, DoBodyBB);

    CGF.EmitBlock(DoEndBB);
  };

  auto &&SeqGen = [this, Fn, &CapturedVars, &RTLoc, &Loc](CodeGenFunction &CGF,
                                                          PrePostActionTy &) {
    auto DL = CGM.getDataLayout();
    auto ThreadID = getThreadID(CGF, Loc);
    // Build calls:
    // __kmpc_serialized_parallel(&Loc, GTid);
    llvm::Value *Args[] = {RTLoc, ThreadID};
    CGF.EmitRuntimeCall(
        createNVPTXRuntimeFunction(OMPRTL_NVPTX__kmpc_serialized_parallel),
        Args);

    llvm::SmallVector<llvm::Value *, 16> OutlinedFnArgs;
    OutlinedFnArgs.push_back(
        llvm::Constant::getNullValue(CGM.Int32Ty->getPointerTo()));
    OutlinedFnArgs.push_back(
        llvm::Constant::getNullValue(CGM.Int32Ty->getPointerTo()));
    OutlinedFnArgs.append(CapturedVars.begin(), CapturedVars.end());
    CGF.EmitCallOrInvoke(Fn, OutlinedFnArgs);

    // __kmpc_end_serialized_parallel(&Loc, GTid);
    llvm::Value *EndArgs[] = {emitUpdateLocation(CGF, Loc), ThreadID};
    CGF.EmitRuntimeCall(
        createNVPTXRuntimeFunction(OMPRTL_NVPTX__kmpc_end_serialized_parallel),
        EndArgs);
  };

  auto &&ThenGen = [this, &L0ParallelGen, &L1ParallelGen,
                    &SeqGen](CodeGenFunction &CGF, PrePostActionTy &) {
    emitParallelismLevelCode(CGF, L0ParallelGen, L1ParallelGen, SeqGen);
  };

  if (IfCond) {
    emitOMPIfClause(CGF, IfCond, ThenGen, SeqGen);
  } else {
    CodeGenFunction::RunCleanupsScope Scope(CGF);
    RegionCodeGenTy ThenRCG(ThenGen);
    ThenRCG(CGF);
  }
}

void CGOpenMPRuntimeNVPTX::emitSimdCall(CodeGenFunction &CGF,
                                        SourceLocation Loc,
                                        llvm::Value *OutlinedFn,
                                        ArrayRef<llvm::Value *> CapturedVars) {
  if (!CGF.HaveInsertPoint())
    return;

  llvm::Function *Fn = cast<llvm::Function>(OutlinedFn);
  llvm::Function *WFn = WrapperFunctionsMap[Fn];
  assert(WFn && "Wrapper function does not exist??");

  // Force inline this outlined function at its call site.
  // Fn->addFnAttr(llvm::Attribute::AlwaysInline);
  Fn->setLinkage(llvm::GlobalValue::InternalLinkage);

  // Emit code that does the data sharing changes in the beginning of the
  // function.
  createDataSharingPerFunctionInfrastructure(CGF);

  auto *RTLoc = emitUpdateLocation(CGF, Loc);

  auto &&L1SimdGen = [this, WFn, RTLoc, Loc](CodeGenFunction &CGF,
                                             PrePostActionTy &) {
    CGBuilderTy &Bld = CGF.Builder;
    clang::ASTContext &Ctx = CGF.getContext();

    Address IsFinal =
        CGF.CreateTempAlloca(CGF.Int8Ty, CharUnits::fromQuantity(1),
                             /*Name*/ "is_final");
    Address WorkSource =
        CGF.CreateTempAlloca(CGF.Int32Ty, CharUnits::fromQuantity(4),
                             /*Name*/ "work_source");
    Address LaneId =
        CGF.CreateTempAlloca(CGF.Int32Ty, CharUnits::fromQuantity(4),
                             /*Name*/ "lane_id");
    Address NumLanes =
        CGF.CreateTempAlloca(CGF.Int32Ty, CharUnits::fromQuantity(4),
                             /*Name*/ "num_lanes");
    llvm::APInt TaskBufferSize(/*numBits=*/32, SIMD_STATE_SIZE);
    auto TaskBufferTy = Ctx.getConstantArrayType(
        Ctx.CharTy, TaskBufferSize, ArrayType::Normal, /*IndexTypeQuals=*/0);
    auto TaskState = CGF.CreateMemTemp(TaskBufferTy, CharUnits::fromQuantity(8),
                                       /*Name=*/"task_state")
                         .getPointer();
    CGF.InitTempAlloca(IsFinal, Bld.getInt8(/*C=*/0));
    CGF.InitTempAlloca(WorkSource, Bld.getInt32(/*C=*/-1));

    llvm::BasicBlock *DoBodyBB = CGF.createBasicBlock(".do.body");
    llvm::BasicBlock *ExecuteBB = CGF.createBasicBlock(".do.body.execute");
    llvm::BasicBlock *DoCondBB = CGF.createBasicBlock(".do.cond");
    llvm::BasicBlock *DoEndBB = CGF.createBasicBlock(".do.end");

    CGF.EmitBranch(DoBodyBB);
    CGF.EmitBlock(DoBodyBB);
    auto ArrayDecay = Bld.CreateConstInBoundsGEP2_32(
        llvm::ArrayType::get(CGM.Int8Ty, SIMD_STATE_SIZE), TaskState,
        /*Idx0=*/0, /*Idx1=*/0);
    llvm::Value *Args[] = {ArrayDecay, IsFinal.getPointer(),
                           WorkSource.getPointer(), LaneId.getPointer(),
                           NumLanes.getPointer()};
    llvm::Value *IsActive = CGF.EmitRuntimeCall(
        createNVPTXRuntimeFunction(OMPRTL_NVPTX__kmpc_kernel_convergent_simd),
        Args);
    Bld.CreateCondBr(IsActive, ExecuteBB, DoCondBB);

    CGF.EmitBlock(ExecuteBB);

    llvm::SmallVector<llvm::Value *, 16> OutlinedFnArgs;

    auto *SourceThread = CGF.EmitLoadOfScalar(
        WorkSource, /*Volatile=*/false,
        Ctx.getIntTypeForBitwidth(/*DestWidth=*/32, /*Signed=*/false),
        SourceLocation());
    OutlinedFnArgs.push_back(SourceThread);
    OutlinedFnArgs.push_back(LaneId.getPointer());
    OutlinedFnArgs.push_back(NumLanes.getPointer());
    CGF.EmitCallOrInvoke(WFn, OutlinedFnArgs);
    ArrayDecay = Bld.CreateConstInBoundsGEP2_32(
        llvm::ArrayType::get(CGM.Int8Ty, SIMD_STATE_SIZE), TaskState,
        /*Idx0=*/0, /*Idx1=*/0);
    llvm::Value *EndArgs[] = {ArrayDecay};
    CGF.EmitRuntimeCall(createNVPTXRuntimeFunction(
                            OMPRTL_NVPTX__kmpc_kernel_end_convergent_simd),
                        EndArgs);
    CGF.EmitBranch(DoCondBB);

    CGF.EmitBlock(DoCondBB);
    llvm::Value *IsDone = Bld.CreateICmpEQ(Bld.CreateLoad(IsFinal),
                                           Bld.getInt8(/*C=*/1), "is_done");
    Bld.CreateCondBr(IsDone, DoEndBB, DoBodyBB);

    CGF.EmitBlock(DoEndBB);
  };

  auto &&SeqGen = [Fn, &CapturedVars](CodeGenFunction &CGF, PrePostActionTy &) {
    CGBuilderTy &Bld = CGF.Builder;
    Address LaneId =
        CGF.CreateTempAlloca(CGF.Int32Ty, CharUnits::fromQuantity(4),
                             /*Name*/ "lane_id");
    Address NumLanes =
        CGF.CreateTempAlloca(CGF.Int32Ty, CharUnits::fromQuantity(4),
                             /*Name*/ "num_lanes");

    CGF.InitTempAlloca(LaneId, Bld.getInt32(/*C=*/0));
    CGF.InitTempAlloca(NumLanes, Bld.getInt32(/*C=*/1));

    llvm::SmallVector<llvm::Value *, 16> OutlinedFnArgs;
    OutlinedFnArgs.push_back(LaneId.getPointer());
    OutlinedFnArgs.push_back(NumLanes.getPointer());
    OutlinedFnArgs.append(CapturedVars.begin(), CapturedVars.end());
    CGF.EmitCallOrInvoke(Fn, OutlinedFnArgs);
  };

  CodeGenFunction::RunCleanupsScope Scope(CGF);
  // We only do SIMD if nested in a parallel region.
  emitParallelismLevelCode(CGF, SeqGen, L1SimdGen, SeqGen);
}

//
// Generate optimized code resembling static schedule with chunk size of 1
// whenever the standard gives us freedom.  This allows maximum coalescing on
// the NVPTX target.
//
bool CGOpenMPRuntimeNVPTX::generateCoalescedSchedule(
    OpenMPScheduleClauseKind ScheduleKind, bool ChunkSizeOne,
    bool ordered) const {
  return !ordered && (ScheduleKind == OMPC_SCHEDULE_unknown ||
                      ScheduleKind == OMPC_SCHEDULE_auto ||
                      (ScheduleKind == OMPC_SCHEDULE_static && ChunkSizeOne));
}

bool CGOpenMPRuntimeNVPTX::requiresBarrier(const OMPLoopDirective &S) const {
  const bool Ordered = S.getSingleClause<OMPOrderedClause>() != nullptr;
  OpenMPScheduleClauseKind ScheduleKind = OMPC_SCHEDULE_unknown;
  if (auto *C = S.getSingleClause<OMPScheduleClause>())
    ScheduleKind = C->getScheduleKind();
  return Ordered || ScheduleKind == OMPC_SCHEDULE_dynamic ||
         ScheduleKind == OMPC_SCHEDULE_guided;
}

CGOpenMPRuntimeNVPTX::CGOpenMPRuntimeNVPTX(CodeGenModule &CGM)
    : CGOpenMPRuntime(CGM), IsOrphaned(false), ParallelNestingLevel(0) {
  if (!CGM.getLangOpts().OpenMPIsDevice)
    llvm_unreachable("OpenMP NVPTX can only handle device code.");
}

void CGOpenMPRuntimeNVPTX::emitNumTeamsClause(CodeGenFunction &CGF,
                                              const Expr *NumTeams,
                                              const Expr *ThreadLimit,
                                              SourceLocation Loc) {}

void CGOpenMPRuntimeNVPTX::emitTeamsCall(CodeGenFunction &CGF,
                                         const OMPExecutableDirective &D,
                                         SourceLocation Loc,
                                         llvm::Value *OutlinedFn,
                                         ArrayRef<llvm::Value *> CapturedVars) {

  // just emit the statements in the teams region inlined
  auto &&CodeGen = [&D](CodeGenFunction &CGF, PrePostActionTy &) {
    CodeGenFunction::OMPPrivateScope PrivateScope(CGF);
    (void)CGF.EmitOMPFirstprivateClause(D, PrivateScope);
    CGF.EmitOMPPrivateClause(D, PrivateScope);
    (void)PrivateScope.Privatize();

    CGF.EmitStmt(cast<CapturedStmt>(D.getAssociatedStmt())->getCapturedStmt());
  };

  emitInlinedDirective(CGF, OMPD_teams, CodeGen);
}

llvm::Function *CGOpenMPRuntimeNVPTX::emitRegistrationFunction() {
  auto &Ctx = CGM.getContext();
  unsigned PointerAlign = Ctx.getTypeAlignInChars(Ctx.VoidPtrTy).getQuantity();
  unsigned Int32Align =
      Ctx.getTypeAlignInChars(
             Ctx.getIntTypeForBitwidth(/*DestWidth=*/32, /*Signed=*/true))
          .getQuantity();

  auto *SlotTy = getDataSharingSlotTy();

  // Scan all the functions that have data sharing info.
  for (auto &DS : DataSharingFunctionInfoMap) {
    llvm::Function *Fn = DS.first;
    DataSharingFunctionInfo &DSI = DS.second;

    llvm::BasicBlock &HeaderBB = Fn->front();

    // Find the last alloca and the last replacement that is not an alloca.
    llvm::Instruction *LastAlloca = nullptr;
    llvm::Instruction *LastNonAllocaReplacement = nullptr;

    for (auto &I : HeaderBB) {
      if (isa<llvm::AllocaInst>(I)) {
        LastAlloca = &I;
        continue;
      }

      auto It = std::find(DSI.ValuesToBeReplaced.begin(),
                          DSI.ValuesToBeReplaced.end(), &I);
      if (It == DSI.ValuesToBeReplaced.end())
        continue;

      LastNonAllocaReplacement = cast<llvm::Instruction>(*It);
    }

    // We will start inserting after the first alloca or at the beginning of the
    // function.
    llvm::Instruction *InsertPtr = nullptr;
    if (LastAlloca)
      InsertPtr = LastAlloca->getNextNode();
    else
      InsertPtr = &(*HeaderBB.begin());

    assert(InsertPtr && "Empty function???");

    // Helper to emit the initializaion code at the provided insertion point.
    auto &&InitializeEntryPoint = [this, &DSI](llvm::Instruction *&InsertPtr) {
      assert(DSI.EntryWorkerFunction &&
             "All entry function are expected to have an worker function.");
      assert(DSI.EntryExitBlock &&
             "All entry function are expected to have an exit basic block.");

      auto *ShouldReturnImmediatelly = llvm::CallInst::Create(
          createKernelInitializerFunction(DSI.EntryWorkerFunction), "",
          InsertPtr);
      auto *Cond = llvm::ICmpInst::Create(
          llvm::CmpInst::ICmp, llvm::CmpInst::ICMP_EQ, ShouldReturnImmediatelly,
          llvm::Constant::getNullValue(CGM.Int32Ty), "", InsertPtr);
      auto *CurrentBB = InsertPtr->getParent();
      auto *MasterBB = CurrentBB->splitBasicBlock(InsertPtr, ".master");

      // Adjust the terminator of the current block.
      CurrentBB->getTerminator()->eraseFromParent();
      llvm::BranchInst::Create(MasterBB, DSI.EntryExitBlock, Cond, CurrentBB);

      // Continue inserting in the master basic block.
      InsertPtr = &*MasterBB->begin();
    };

    // If there is nothing to share, and this is an entry point, we should
    // initialize the data sharing logic anyways.
    if (!DSI.InitializationFunction && DSI.IsEntryPoint) {
      InitializeEntryPoint(InsertPtr);
      continue;
    }

    SmallVector<llvm::Value *, 16> InitArgs;
    SmallVector<std::pair<llvm::Value *, llvm::Value *>, 16> Replacements;

    // Create the saved slot/stack/frame/active thread variables.
    InitArgs.push_back(
        new llvm::AllocaInst(SlotTy->getPointerTo(), /*ArraySize=*/nullptr,
                             PointerAlign, "data_share_slot_saved", InsertPtr));
    InitArgs.push_back(
        new llvm::AllocaInst(CGM.VoidPtrTy, /*ArraySize=*/nullptr, PointerAlign,
                             "data_share_stack_saved", InsertPtr));
    InitArgs.push_back(
        new llvm::AllocaInst(CGM.VoidPtrTy, /*ArraySize=*/nullptr, PointerAlign,
                             "data_share_frame_saved", InsertPtr));
    InitArgs.push_back(
        new llvm::AllocaInst(CGM.Int32Ty, /*ArraySize=*/nullptr, Int32Align,
                             "data_share_active_thd_saved", InsertPtr));

    // Create the remaining arguments. One if it is a reference sharing (the
    // reference itself), two otherwise (the address of the replacement and the
    // value to be replaced).
    for (auto *R : DSI.ValuesToBeReplaced) {

      // Is it a reference? If not, create the address alloca.
      if (!isa<llvm::LoadInst>(R)) {
        InitArgs.push_back(new llvm::AllocaInst(
            R->getType(), /*ArraySize=*/nullptr, PointerAlign,
            R->getName() + ".shared", InsertPtr));
        // We will have to replace the uses of R by the load of new alloca.
        Replacements.push_back(std::make_pair(R, InitArgs.back()));
      }
      InitArgs.push_back(R);
    }

    // We now need to insert the sharing calls. We insert after the last value
    // to be replaced or after the alloca.
    if (LastNonAllocaReplacement)
      InsertPtr = LastNonAllocaReplacement->getNextNode();

    // Do the replacements now.
    for (auto &R : Replacements) {
      auto *From = R.first;
      auto *To = new llvm::LoadInst(R.second, "", /*isVolatile=*/false,
                                    PointerAlign, InsertPtr);

      //      auto *Arg = llvm::CastInst::CreateBitOrPointerCast(To,
      //      CGM.Int64Ty, "", InsertPtr);
      //      llvm::Value *Args[] = { Arg };
      //      llvm::CallInst::Create(createNVPTXRuntimeFunction(OMPRTL_NVPTX__kmpc_samuel_print),
      //      Args, "", InsertPtr);

      // Check if there are uses of From before To and move them after To. These
      // are usually the function epilogue stores.
      for (auto II = HeaderBB.begin(), IE = HeaderBB.end(); II != IE;) {
        llvm::Instruction *I = &*II;
        ++II;

        if (I == To)
          break;
        if (I == From)
          continue;

        bool NeedsToMove = false;
        for (auto *U : From->users()) {
          // Is this a user of from? If so we need to move it.
          if (I == U) {
            NeedsToMove = true;
            break;
          }
        }

        if (!NeedsToMove)
          continue;

        I->moveBefore(To->getNextNode());
      }

      From->replaceAllUsesWith(To);

      // Make sure the following calls are inserted before these loads.
      InsertPtr = To;
    }

    // If this is an entry point, we have to initialize the data sharing first.
    if (DSI.IsEntryPoint)
      InitializeEntryPoint(InsertPtr);
    (void)llvm::CallInst::Create(DSI.InitializationFunction, InitArgs, "",
                                 InsertPtr);

    // Close the environment. The saved stack is in the 4 first entries of the
    // arguments array.
    llvm::Value *ClosingArgs[]{
        InitArgs[0], InitArgs[1], InitArgs[2], InitArgs[3],
        // If an entry point we need to signal the clean up.
        llvm::ConstantInt::get(CGM.Int32Ty, DSI.IsEntryPoint ? 1 : 0)};
    for (llvm::BasicBlock &BB : *Fn)
      if (auto *Ret = dyn_cast<llvm::ReturnInst>(BB.getTerminator()))
        (void)llvm::CallInst::Create(
            createNVPTXRuntimeFunction(
                OMPRTL_NVPTX__kmpc_data_sharing_environment_end),
            ClosingArgs, "", Ret);
  }

  // Make the default registration procedure.
  return CGOpenMPRuntime::emitRegistrationFunction();
}
