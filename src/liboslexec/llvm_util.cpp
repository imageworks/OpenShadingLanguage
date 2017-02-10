/*
Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Sony Pictures Imageworks nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <memory>
#include <OpenImageIO/thread.h>
#include <boost/thread/tss.hpp>   /* for thread_specific_ptr */

#include "OSL/oslconfig.h"
#include "OSL/llvm_util.h"

#if OSL_LLVM_VERSION < 34
#error "LLVM minimum version required for OSL is 3.4"
#endif

#if OSL_LLVM_VERSION >= 35 && OSL_CPLUSPLUS_VERSION < 11
#error "LLVM >= 3.5 requires C++11 or newer"
#endif

// Use MCJIT for LLVM 3.6 and beyind, old JIT for earlier
#if !OSL_USE_ORC_JIT
#  define USE_OLD_JIT (OSL_LLVM_VERSION <  36)
#  define USE_MCJIT   (OSL_LLVM_VERSION >= 36)
#  define OSL_USE_ORC_JIT 0
#endif

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DataLayout.h>
#if OSL_LLVM_VERSION >= 35
#  include <llvm/Linker/Linker.h>
#  include <llvm/Support/FileSystem.h>
#else
#  include <llvm/Linker.h>
#endif
#include <llvm/Support/ErrorOr.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/TargetRegistry.h>

#if OSL_LLVM_VERSION < 40
#  include <llvm/Bitcode/ReaderWriter.h>
#else
#  include <llvm/Bitcode/BitcodeReader.h>
#  include <llvm/Bitcode/BitcodeWriter.h>
#endif
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#if OSL_USE_ORC_JIT
#  include <llvm/Support/DynamicLibrary.h>
#  include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#  include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#  include <llvm/ExecutionEngine/Orc/LazyEmittingLayer.h>
#  include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#elif USE_MCJIT
#  include <llvm/ExecutionEngine/MCJIT.h>
#elif USE_OLD_JIT
#  include <llvm/ExecutionEngine/JIT.h>
#  include <llvm/ExecutionEngine/JITMemoryManager.h>
#endif
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/PrettyStackTrace.h>
#if OSL_LLVM_VERSION >= 35
#  include <llvm/IR/Verifier.h>
#else
#  include <llvm/Analysis/Verifier.h>
#endif
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Utils/UnifyFunctionExitNodes.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#if OSL_LLVM_VERSION >= 36
#  include <llvm/ExecutionEngine/SectionMemoryManager.h>
#endif
#if OSL_LLVM_VERSION >= 39
#  include <llvm/Transforms/Scalar/GVN.h>
#endif

#ifndef OSL_LLVM_NO_BITCODE
#  include "llvm_ops_bc.h"
#endif

OSL_NAMESPACE_ENTER

namespace pvt {

#if USE_OLD_JIT
    typedef llvm::JITMemoryManager LLVMMemoryManager;
#else
    typedef llvm::SectionMemoryManager LLVMMemoryManager;
#endif

#if OSL_LLVM_VERSION >= 35
#if OSL_LLVM_VERSION < 40
    typedef std::error_code LLVMErr;
#else
    typedef llvm::Error LLVMErr;
#endif
#endif


namespace {
static OIIO::spin_mutex llvm_global_mutex;
static bool setup_done = false;
static boost::thread_specific_ptr<LLVM_Util::PerThreadInfo> perthread_infos;
static std::vector<std::shared_ptr<LLVMMemoryManager> > jitmm_hold;
};




// We hold certain things (LLVM context and custom JIT memory manager)
// per thread and retained across LLVM_Util invocations.  We are
// intentionally "leaking" them.
struct LLVM_Util::PerThreadInfo {
    PerThreadInfo () : llvm_context(NULL), llvm_jitmm(NULL) {}
    ~PerThreadInfo () {
        delete llvm_context;
        // N.B. Do NOT delete the jitmm -- another thread may need the
        // code! Don't worry, we stashed a pointer in jitmm_hold.
    }
    static void destroy (PerThreadInfo *threadinfo) { delete threadinfo; }
    static PerThreadInfo *get () {
        PerThreadInfo *p = perthread_infos.get ();
        if (! p) {
            p = new PerThreadInfo();
            perthread_infos.reset (p);
        }
        return p;
    }

    llvm::LLVMContext *llvm_context;
    LLVMMemoryManager *llvm_jitmm;
};




size_t
LLVM_Util::total_jit_memory_held ()
{
    size_t jitmem = 0;
    OIIO::spin_lock lock (llvm_global_mutex);
#if USE_OLD_JIT
    for (size_t i = 0;  i < jitmm_hold.size();  ++i) {
        LLVMMemoryManager *mm = jitmm_hold[i].get();
        if (mm)
            jitmem += mm->GetDefaultCodeSlabSize() * mm->GetNumCodeSlabs()
                    + mm->GetDefaultDataSlabSize() * mm->GetNumDataSlabs()
                    + mm->GetDefaultStubSlabSize() * mm->GetNumStubSlabs();
    }
#endif
    return jitmem;
}


#if OSL_LLVM_VERSION >= 35
#if OSL_LLVM_VERSION < 40
inline bool error_string (const std::error_code &err, std::string *str) {
    if (err) {
        if (str) *str = err.message();
        return true;
    }
    return false;
}
#else
inline bool error_string (llvm::Error err, std::string *str) {
    if (err) {
        if (str) {
            llvm::handleAllErrors(std::move(err),
                      [str](llvm::ErrorInfoBase &E) { *str += E.message(); });
        }
        return true;
    }
    return false;
}
#endif
#endif



/// MemoryManager - Create a shell that passes on requests
/// to a real LLVMMemoryManager underneath, but can be retained after the
/// dummy is destroyed.  Also, we don't pass along any deallocations.
class LLVM_Util::MemoryManager : public LLVMMemoryManager {
protected:
    LLVMMemoryManager *mm;  // the real one
public:

#if USE_OLD_JIT // llvm::JITMemoryManager
    MemoryManager(LLVMMemoryManager *realmm) : mm(realmm) { HasGOT = realmm->isManagingGOT(); }

    virtual void setMemoryWritable() { mm->setMemoryWritable(); }
    virtual void setMemoryExecutable() { mm->setMemoryExecutable(); }
    virtual void setPoisonMemory(bool poison) { mm->setPoisonMemory(poison); }
    virtual void AllocateGOT() { ASSERT(HasGOT == false); ASSERT(HasGOT == mm->isManagingGOT()); mm->AllocateGOT(); HasGOT = true; ASSERT(HasGOT == mm->isManagingGOT()); }
    virtual uint8_t *getGOTBase() const { return mm->getGOTBase(); }
    virtual uint8_t *startFunctionBody(const llvm::Function *F,
                                       uintptr_t &ActualSize) {
        return mm->startFunctionBody (F, ActualSize);
    }
    virtual uint8_t *allocateStub(const llvm::GlobalValue* F, unsigned StubSize,
                                  unsigned Alignment) {
        return mm->allocateStub (F, StubSize, Alignment);
    }
    virtual void endFunctionBody(const llvm::Function *F,
                                 uint8_t *FunctionStart, uint8_t *FunctionEnd) {
        mm->endFunctionBody (F, FunctionStart, FunctionEnd);
    }
    virtual uint8_t *allocateSpace(intptr_t Size, unsigned Alignment) {
        return mm->allocateSpace (Size, Alignment);
    }
    virtual uint8_t *allocateGlobal(uintptr_t Size, unsigned Alignment) {
        return mm->allocateGlobal (Size, Alignment);
    }
    virtual void deallocateFunctionBody(void *Body) {
        // DON'T DEALLOCATE mm->deallocateFunctionBody (Body);
    }
    virtual bool CheckInvariants(std::string &s) {
        return mm->CheckInvariants(s);
    }
    virtual size_t GetDefaultCodeSlabSize() {
        return mm->GetDefaultCodeSlabSize();
    }
    virtual size_t GetDefaultDataSlabSize() {
        return mm->GetDefaultDataSlabSize();
    }
    virtual size_t GetDefaultStubSlabSize() {
        return mm->GetDefaultStubSlabSize();
    }
    virtual unsigned GetNumCodeSlabs() { return mm->GetNumCodeSlabs(); }
    virtual unsigned GetNumDataSlabs() { return mm->GetNumDataSlabs(); }
    virtual unsigned GetNumStubSlabs() { return mm->GetNumStubSlabs(); }

    virtual void notifyObjectLoaded(llvm::ExecutionEngine *EE, const llvm::ObjectImage *oi) {
        mm->notifyObjectLoaded (EE, oi);
    }

#else // MCJITMemoryManager

    MemoryManager(LLVMMemoryManager *realmm) : mm(realmm) {}
    
    virtual void notifyObjectLoaded(llvm::ExecutionEngine *EE, const llvm::object::ObjectFile &oi) {
        mm->notifyObjectLoaded (EE, oi);
    }

#if OSL_LLVM_VERSION <= 37
  virtual void reserveAllocationSpace(
    uintptr_t CodeSize, uintptr_t DataSizeRO, uintptr_t DataSizeRW) {
        return mm->reserveAllocationSpace(CodeSize, DataSizeRO, DataSizeRW);
}
#else
    virtual void reserveAllocationSpace(uintptr_t CodeSize, uint32_t CodeAlign,
                                        uintptr_t RODataSize,
                                        uint32_t RODataAlign,
                                        uintptr_t RWDataSize,
                                        uint32_t RWDataAlign) {
        return mm->reserveAllocationSpace(CodeSize, CodeAlign, RODataSize, RODataAlign, RWDataSize, RWDataAlign);
    }
#endif

    virtual bool needsToReserveAllocationSpace() {
        return mm->needsToReserveAllocationSpace();
    }

    virtual void invalidateInstructionCache() {
        mm->invalidateInstructionCache();
    }
    
#endif

    // Common
    virtual ~MemoryManager() {}

    virtual void *getPointerToNamedFunction(const std::string &Name,
                                            bool AbortOnFailure = true) {
        return mm->getPointerToNamedFunction (Name, AbortOnFailure);
    }
    virtual uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                             unsigned SectionID, llvm::StringRef SectionName) {
        return mm->allocateCodeSection(Size, Alignment, SectionID, SectionName);
    }
    virtual uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                             unsigned SectionID, llvm::StringRef SectionName,
                             bool IsReadOnly) {
        return mm->allocateDataSection(Size, Alignment, SectionID,
                                       SectionName, IsReadOnly);
    }
    virtual void registerEHFrames(uint8_t *Addr, uint64_t LoadAddr, size_t Size) {
        mm->registerEHFrames (Addr, LoadAddr, Size);
    }
    virtual void deregisterEHFrames(uint8_t *Addr, uint64_t LoadAddr, size_t Size) {
        mm->deregisterEHFrames(Addr, LoadAddr, Size);
    }
    virtual uint64_t getSymbolAddress(const std::string &Name) {
        return mm->getSymbolAddress (Name);
    }
    virtual bool finalizeMemory(std::string *ErrMsg = 0) {
        return mm->finalizeMemory (ErrMsg);
    }
};



#if OSL_LLVM_VERSION <= 38
class LLVM_Util::IRBuilder : public llvm::IRBuilder<true,llvm::ConstantFolder,
                                        llvm::IRBuilderDefaultInserter<true> > {
    typedef llvm::IRBuilder<true, llvm::ConstantFolder,
                                  llvm::IRBuilderDefaultInserter<true> > Base;
public:
    IRBuilder(llvm::BasicBlock *TheBB) : Base(TheBB) {}
};
#else
class LLVM_Util::IRBuilder : public llvm::IRBuilder<llvm::ConstantFolder,
                                               llvm::IRBuilderDefaultInserter> {
    typedef llvm::IRBuilder<llvm::ConstantFolder,
                            llvm::IRBuilderDefaultInserter> Base;
public:
    IRBuilder(llvm::BasicBlock *TheBB) : Base(TheBB) {}
};
#endif

#if OSL_USE_ORC_JIT

class Target {
    const llvm::Target *m_target;
    const llvm::Triple m_triple;

    static Target* on_err (std::string *out_err, const std::string *info = NULL) {
        if (out_err) {
            *out_err = "Target could not be created";
            if (info) {
                out_err->append(": ");
                out_err->append(*info);
            }
        }
        return NULL;
    }

    static Target*
    create (std::string *out_err) {
        // Use C++ static initializer to make sure this is run once.
        llvm::sys::DynamicLibrary::LoadLibraryPermanently(NULL);

        std::string err;
        const llvm::Triple trp (llvm::sys::getProcessTriple());
        const llvm::Target *trg = llvm::TargetRegistry::lookupTarget
                                                     (trp.getTriple(), err);
        return trg ? new Target(trg, trp) : (Target*)on_err (out_err);
    }

    Target (const llvm::Target *target, const llvm::Triple &triple) :
        m_target(target), m_triple(triple) {}

public:
    llvm::TargetMachine *createMachine(std::string *out_err, unsigned opt_level = llvm::CodeGenOpt::Level::Default) {
        const std::string cpu;
        const std::string features;
        const llvm::TargetOptions options = llvm::TargetOptions();
        const llvm::CodeModel::Model code_model = llvm::CodeModel::JITDefault;
        llvm::TargetMachine *machine =
            m_target->createTargetMachine(m_triple.str(), cpu, features,
                                   options, llvm::Optional<llvm::Reloc::Model>(),
                                   code_model);
        return machine ? machine : (llvm::TargetMachine*)on_err (out_err);
    }

    // Use a single instance that all JIT's will reference
    // Pointer to handle possible runtime failure (PPC I'm looking at you).
    static Target *instance (std::string *err = NULL) {
        static std::unique_ptr<Target> s_target(create(err));
        return s_target.get();
    }

    const llvm::Triple &triple() { return m_triple; }
};


class LLVM_Util::OrcJIT {

#if OSL_LLVM_VERSION < 40
    typedef llvm::orc::TargetAddress JITTargetAddress;
    typedef llvm::orc::JITSymbol JITSymbol;
    typedef llvm::RuntimeDyld::SymbolInfo JITEvaluatedSymbol;
    typedef llvm::RuntimeDyld::SymbolResolver JITSymbolResolver;
#else
    typedef llvm::JITTargetAddress JITTargetAddress;
    typedef llvm::JITSymbol JITSymbol;
    typedef llvm::JITSymbol JITEvaluatedSymbol;
    typedef llvm::JITSymbolResolver JITSymbolResolver;
#endif

    // Simple JITSymbolResolver that looks back into its parent
    //
    // The difference between findSymbol and findSymbolInLogicalDylib is that
    // findSymbol should ignore hidden/weak and findSymbolInLogicalDylib should not
    // Ignore this fact for now, possibly ever.
    struct SimpleResolver : public JITSymbolResolver {
        OrcJIT &m_parent;
    public:
        SimpleResolver(OrcJIT &p) : m_parent(p) {}

        JITEvaluatedSymbol findSymbol (const std::string &name) {
            uint64_t ptr = cast_ptr(m_parent.m_lookup_sym(name.substr(1)));
            return ptr ? JITEvaluatedSymbol(ptr, llvm::JITSymbolFlags::Exported) : NULL;
        }
        JITEvaluatedSymbol findSymbolInLogicalDylib (const std::string &name) {
            uint64_t ptr = llvm::RTDyldMemoryManager::getSymbolAddressInProcess(name);
            return ptr ? JITEvaluatedSymbol(ptr, llvm::JITSymbolFlags::Exported) : NULL;
        }

        // Work around:
        // ORC expecting a pointer to this object (we pass it a reference)
        //
        SimpleResolver& operator * () { return *this; }
    };

    typedef void* (*LazyLookup)(const std::string &);
    static void* default_lookup (const std::string&) { return NULL; }

    static void* cast_ptr (uintptr_t ptr) { return reinterpret_cast<void*>(ptr); }
    static uintptr_t cast_ptr (void *ptr) { return reinterpret_cast<uintptr_t>(ptr); }

public:
    // This doesn't implement on-request or lazy compilation.
    // It uses Orc's eager compilation layer directly - IRCompileLayer.
    // It also uses the basis object layer - ObjectLinkingLayer - directly.
    // Orc's SimpleCompiler is used compile the module; it runs LLVM's
    // codegen and MC on the module, producing an object file in memory. No
    // IR-level optimizations are run by the JIT.
    typedef llvm::orc::SimpleCompiler SimpleCompiler;
    typedef llvm::orc::ObjectLinkingLayer<> ObjectLayer;
    typedef llvm::orc::IRCompileLayer<ObjectLayer> CompileLayer;

    // More layers to come....
    typedef CompileLayer FinalCompileLayer;
    typedef FinalCompileLayer::ModuleSetHandleT ModuleHandle;

    FinalCompileLayer &compiler() { return m_compile_layer; }

    OrcJIT(llvm::TargetMachine *machine, llvm::Module *module, LLVMMemoryManager* mem_manager) :
        m_machine(machine), m_mem_manager(mem_manager), m_lookup_sym(default_lookup),
        m_data_layout(machine->createDataLayout()),
        m_compile_layer(m_object_layer, SimpleCompiler(*machine)),
        m_symbol_resolver(*this), m_linker(*module) {
            module->setDataLayout (m_data_layout);
        }

    static OrcJIT*
    create(LLVMMemoryManager *mem_manager, llvm::Module *module, std::string *out_err,
           unsigned opt_level = llvm::CodeGenOpt::Level::Default) {
        if (Target *target = Target::instance(out_err)) {
            if (llvm::TargetMachine *M = target->createMachine(out_err, opt_level))
                return new OrcJIT(M, module, mem_manager);
        }
        return NULL;
    }

    // Add a module to the JIT.
    ModuleHandle addModule(llvm::Module *module) {
        llvm::SmallVector<llvm::Module*, 1> mod_set(1, module);
        auto H = compiler().addModuleSet(mod_set, m_mem_manager, // std::unique_ptr<MemoryManager>(new MemoryManager(m_mem_manager))
                                         m_symbol_resolver);
        m_module_h.push_back(H);
        return H;
    }

    // Remove a module from the JIT.
    void removeModule(ModuleHandle H) {
        m_module_h.erase(std::find(m_module_h.begin(), m_module_h.end(), H));
        compiler().removeModuleSet(H);
    }

    // Get the runtime address of the compiled symbol whose name is given.
    JITSymbol findSymbol(const std::string name) {
        std::string mangled;
        {
            llvm::raw_string_ostream strm(mangled);
            llvm::Mangler::getNameWithPrefix(strm, name, m_data_layout);
        }

        for (auto H : llvm::make_range(m_module_h.rbegin(), m_module_h.rend())) {
            if (auto sym = m_compile_layer.findSymbolIn(H, mangled, true))
                return sym;
        }
        return NULL;
    }

    void addSingleModule (llvm::Module *module) {
        // Currently we know there is only a single module, and adding it more
        // than once makes little sense.
        if (!m_module_h.empty ())
            return;
        addModule (module);
    }

    void linkModule (std::unique_ptr<llvm::Module> module) {
        m_linker.linkInModule(std::move(module));
    }

    // API matching avoids some ifdefs below
    void InstallLazyFunctionCreator (LazyLookup func) {
        m_lookup_sym = func;
    }
    void *getPointerToFunction (const llvm::Function *func) {
        return cast_ptr(findSymbol(func->getName()).getAddress());
    }
    const llvm::DataLayout &dataLayout () const { return m_data_layout; }

private:
    std::unique_ptr<llvm::TargetMachine> m_machine;
    LLVMMemoryManager *m_mem_manager;
    LazyLookup m_lookup_sym;

    llvm::DataLayout m_data_layout;
    ObjectLayer m_object_layer;
    CompileLayer m_compile_layer;
    std::vector<ModuleHandle> m_module_h;
    SimpleResolver m_symbol_resolver;
    llvm::Linker m_linker;
};
#endif



class LLVM_Util::PassManager : public llvm::legacy::PassManager {
    llvm::legacy::PassManager m_pass_manager;
    llvm::legacy::FunctionPassManager *m_fpass_manager;
    bool m_run_func_pass;

    static void initPassManager(llvm::legacy::PassManagerBase &manager,
                                llvm::Module *module) {
        // LLVM keeps changing names and call sequence. This part is easier to
        // understand if we explicitly break it into individual LLVM versions.
      #if OSL_LLVM_VERSION >= 37
            // nothing
      #elif OSL_LLVM_VERSION >= 36
            manager.add (new llvm::DataLayoutPass());
      #elif OSL_LLVM_VERSION == 35
            manager.add (new llvm::DataLayoutPass(module));
      #elif OSL_LLVM_VERSION == 34
            manager.add (new llvm::DataLayout(module));
      #endif
    }

public:
    PassManager(llvm::Module *module) :
        m_fpass_manager(nullptr), m_run_func_pass(false) {
        initPassManager(m_pass_manager, module);
    }
    ~PassManager() { delete m_fpass_manager; }

    // Create a function pass manager
    llvm::legacy::FunctionPassManager*
    createFunctionPass(llvm::Module *module, int optlevel) {
        ASSERT (m_fpass_manager==nullptr && "Function pass manager already set");
        m_fpass_manager = new llvm::legacy::FunctionPassManager(module);
        initPassManager(*m_fpass_manager, module);
        m_run_func_pass = true;
        return m_fpass_manager;
    }

    // Use LLVM's default passes. Must not call createFunctionPass before!
    void defaultPasses(llvm::Module *module, int optlevel) {
        createFunctionPass(module, optlevel);
        // function passes will automatically be run
        m_run_func_pass = false;

        llvm::PassManagerBuilder builder;
        builder.OptLevel = optlevel;
        builder.Inliner = llvm::createFunctionInliningPass();
        // builder.DisableUnrollLoops = true;
        builder.populateFunctionPassManager (*m_fpass_manager);
        builder.populateModulePassManager (m_pass_manager);
    }
    
    // Run the passes over the module
    void run (llvm::Module &module) {
        // Using defaultPasses wil run these automatically, otherwise must do
        // it by hand
        if (m_fpass_manager && m_run_func_pass) {
            m_fpass_manager->doInitialization();
            for (llvm::Function &func : module) {
                if (!func.isDeclaration())
                    m_fpass_manager->run(func);
            }
            m_fpass_manager->doFinalization();
        }
        m_pass_manager.run (module);
    }

    llvm::legacy::PassManager& modulePass() { return m_pass_manager; }
};


LLVM_Util::LLVM_Util (int debuglevel)
    : m_debug(debuglevel), m_thread(NULL),
      m_llvm_context(NULL), m_llvm_module(NULL),
      m_builder(NULL), m_llvm_jitmm(NULL),
      m_current_function(NULL),
      m_llvm_passes(NULL),
      m_llvm_exec(NULL)
{
    SetupLLVM ();
    m_thread = PerThreadInfo::get();
    ASSERT (m_thread);

    {
        OIIO::spin_lock lock (llvm_global_mutex);
        if (! m_thread->llvm_context)
            m_thread->llvm_context = new llvm::LLVMContext();

        if (! m_thread->llvm_jitmm) {
#if USE_OLD_JIT
            m_thread->llvm_jitmm = llvm::JITMemoryManager::CreateDefaultMemManager();
#else
            m_thread->llvm_jitmm = new LLVMMemoryManager;
#endif
            ASSERT (m_thread->llvm_jitmm);
            jitmm_hold.push_back (std::shared_ptr<LLVMMemoryManager>(m_thread->llvm_jitmm));
        }
#if USE_OLD_JIT
        m_llvm_jitmm = new MemoryManager(m_thread->llvm_jitmm);
#else
        // Hold the REAL manager and use it as an argument later
        m_llvm_jitmm = reinterpret_cast<MemoryManager*>(m_thread->llvm_jitmm);
#endif
    }

    m_llvm_context = m_thread->llvm_context;

    // Set up aliases for types we use over and over
    m_llvm_type_float = (llvm::Type *) llvm::Type::getFloatTy (*m_llvm_context);
    m_llvm_type_int = (llvm::Type *) llvm::Type::getInt32Ty (*m_llvm_context);
    if (sizeof(char *) == 4)
        m_llvm_type_addrint = (llvm::Type *) llvm::Type::getInt32Ty (*m_llvm_context);
    else
        m_llvm_type_addrint = (llvm::Type *) llvm::Type::getInt64Ty (*m_llvm_context);
    m_llvm_type_int_ptr = (llvm::PointerType *) llvm::Type::getInt32PtrTy (*m_llvm_context);
    m_llvm_type_bool = (llvm::Type *) llvm::Type::getInt1Ty (*m_llvm_context);
    m_llvm_type_char = (llvm::Type *) llvm::Type::getInt8Ty (*m_llvm_context);
    m_llvm_type_longlong = (llvm::Type *) llvm::Type::getInt64Ty (*m_llvm_context);
    m_llvm_type_void = (llvm::Type *) llvm::Type::getVoidTy (*m_llvm_context);
    m_llvm_type_char_ptr = (llvm::PointerType *) llvm::Type::getInt8PtrTy (*m_llvm_context);
    m_llvm_type_float_ptr = (llvm::PointerType *) llvm::Type::getFloatPtrTy (*m_llvm_context);
    m_llvm_type_ustring_ptr = (llvm::PointerType *) llvm::PointerType::get (m_llvm_type_char_ptr, 0);
    m_llvm_type_void_ptr = m_llvm_type_char_ptr;

    // A triple is a struct composed of 3 floats
    std::vector<llvm::Type*> triplefields(3, m_llvm_type_float);
    m_llvm_type_triple = type_struct (triplefields, "Vec3");
    m_llvm_type_triple_ptr = (llvm::PointerType *) llvm::PointerType::get (m_llvm_type_triple, 0);

    // A matrix is a struct composed 16 floats
    std::vector<llvm::Type*> matrixfields(16, m_llvm_type_float);
    m_llvm_type_matrix = type_struct (matrixfields, "Matrix4");
    m_llvm_type_matrix_ptr = (llvm::PointerType *) llvm::PointerType::get (m_llvm_type_matrix, 0);
}



LLVM_Util::~LLVM_Util ()
{
    execengine (NULL);
    delete m_llvm_passes;
    delete m_builder;
    module (NULL);
    // DO NOT delete m_llvm_jitmm;  // just the dummy wrapper around the real MM
}



void
LLVM_Util::SetupLLVM ()
{
    OIIO::spin_lock lock (llvm_global_mutex);
    if (setup_done)
        return;
    // Some global LLVM initialization for the first thread that
    // gets here.

#if OSL_LLVM_VERSION < 35
    // enable it to be thread-safe
    llvm::llvm_start_multithreaded ();
#endif
// new versions (>=3.5)don't need this anymore


#if OSL_USE_ORC_JIT || USE_MCJIT
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeDisassembler();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
# if USE_MCJIT
    LLVMLinkInMCJIT();
# endif
#else
    llvm::InitializeNativeTarget();
#endif

    if (debug()) {
#if OSL_LLVM_VERSION <= 36
# define OSL_TGT_DEF(t) t->
        for (llvm::TargetRegistry::iterator t = llvm::TargetRegistry::begin();
             t != llvm::TargetRegistry::end();  ++t)
#else
# define OSL_TGT_DEF(t) t.
        for (auto t : llvm::TargetRegistry::targets())
#endif
        {
            std::cout << "Target: '" << OSL_TGT_DEF(t)getName() << "' "
                      << OSL_TGT_DEF(t)getShortDescription() << "\n";
        }
#undef OSL_TGT_DEF
        std::cout << "\n";
    }

    setup_done = true;
}



llvm::Module *
LLVM_Util::new_module (const char *id, std::string *err)
{
    return new llvm::Module(id, context());
}


llvm::Module *
LLVM_Util::module_from_bitcode (const unsigned char *ubytes, size_t size,
                                const std::string &name, std::string *err)
{
    if (err)
        err->clear();

    const char *bitcode = reinterpret_cast<const char*>(ubytes);

#if OSL_LLVM_VERSION <= 35 /* Old JIT vvvvvvvvvvvvvvvvvvvvvvvvvvvv */
    llvm::MemoryBuffer* buf =
        llvm::MemoryBuffer::getMemBuffer (llvm::StringRef(bitcode, size), name);

    // Create a lazily deserialized IR module
    // This can only be done for old JIT
# if OSL_LLVM_VERSION >= 35
    llvm::Module *m = llvm::getLazyBitcodeModule (buf, context()).get();
# else
    llvm::Module *m = llvm::getLazyBitcodeModule (buf, context(), err);
# endif
    // don't delete buf, the module has taken ownership of it

#if 0
    // Debugging: print all functions in the module
    for (llvm::Module::iterator i = m->begin(); i != m->end(); ++i)
        std::cout << "  found " << i->getName().data() << "\n";
#endif
    return m;
#endif /* End of LLVM <= 3.5 Old JIT section ^^^^^^^^^^^^^^^^^^^^^ */


#if OSL_LLVM_VERSION >= 36  /* MCJIT vvvvvvvvvvvvvvvvvvvvvvvvvvvvv */
# if OSL_LLVM_VERSION >= 40
    typedef llvm::Expected<std::unique_ptr<llvm::Module> > ErrorOrModule;
# else
    typedef llvm::ErrorOr<std::unique_ptr<llvm::Module> > ErrorOrModule;
# endif

# if OSL_LLVM_VERSION >= 40 || defined(OSL_FORCE_BITCODE_PARSE)
    llvm::MemoryBufferRef buf =
        llvm::MemoryBufferRef(llvm::StringRef(bitcode, size), name);
#  ifdef OSL_FORCE_BITCODE_PARSE
    //
    // None of the below seems to be an issue for 3.9 and above.
    // In other JIT code I've seen a related issue, though only on OS X.
    // So if it is still is broken somewhere between 3.6 and 3.8: instead of
    // defining OSL_FORCE_BITCODE_PARSE (which is slower), you may want to
    // try prepending a "_" in two methods above:
    //   LLVM_Util::MemoryManager::getPointerToNamedFunction
    //   LLVM_Util::MemoryManager::getSymbolAddress.
    //
    // Using MCJIT should not require unconditionally parsing
    // the bitcode. But for now, when using getLazyBitcodeModule to
    // lazily deserialize the bitcode, MCJIT is unable to find the
    // called functions due to disagreement about whether a leading "_"
    // is part of the symbol name.
    ErrorOrModule ModuleOrErr = llvm::parseBitcodeFile (buf, context());
#  else
    ErrorOrModule ModuleOrErr = llvm::getLazyBitcodeModule(buf, context());
#  endif

# else /* !OSL_FORCE_BITCODE_PARSE */
    std::unique_ptr<llvm::MemoryBuffer> buf (
        llvm::MemoryBuffer::getMemBuffer (llvm::StringRef(bitcode, size), name, false));
    ErrorOrModule ModuleOrErr = llvm::getLazyBitcodeModule(std::move(buf), context());
# endif

    if (err) {
# if OSL_LLVM_VERSION >= 40
        error_string(ModuleOrErr.takeError(), err);
# else
        error_string(ModuleOrErr.getError(), err);
# endif
    }
    llvm::Module *m = ModuleOrErr ? ModuleOrErr->release() : nullptr;
# if 0
    // Debugging: print all functions in the module
    for (llvm::Module::iterator i = m->begin(); i != m->end(); ++i)
        std::cout << "  found " << i->getName().data() << "\n";
# endif
    return m;

#endif /* MCJIT ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ */
}


void
LLVM_Util::new_builder (llvm::BasicBlock *block)
{
    end_builder();
    if (! block)
        block = new_basic_block ();
    m_builder = new IRBuilder (block);
}


/// Return the current IR builder, create a new one (for the current
/// function) if necessary.
LLVM_Util::IRBuilder &
LLVM_Util::builder () {
    if (! m_builder)
        new_builder ();
    return *m_builder;
}


void
LLVM_Util::end_builder ()
{
    delete m_builder;
    m_builder = NULL;
}



LLVM_Util::JitEngine
LLVM_Util::make_jit_execengine (std::string *err)
{
    execengine (NULL);   // delete and clear any existing engine
    if (err)
        err->clear ();

#if OSL_USE_ORC_JIT

    m_llvm_exec = OrcJIT::create(reinterpret_cast<LLVMMemoryManager*>(m_llvm_jitmm),
                                 m_llvm_module, err);
    return m_llvm_exec;

#else /* !OSL_USE_ORC_JIT : [USE_OLD_JIT or USE_MCJIT] */

# if OSL_LLVM_VERSION >= 36
    llvm::EngineBuilder engine_builder ((std::unique_ptr<llvm::Module>(module())));
# else /* < 36: */
    llvm::EngineBuilder engine_builder (module());
# endif

    engine_builder.setEngineKind (llvm::EngineKind::JIT);
    engine_builder.setErrorStr (err);

# if USE_OLD_JIT
    engine_builder.setJITMemoryManager (m_llvm_jitmm);
    // N.B. createJIT will take ownership of the the JITMemoryManager!
    engine_builder.setUseMCJIT (0);
# else
    // We are actually holding a LLVMMemoryManager
    engine_builder.setMCJITMemoryManager (std::unique_ptr<llvm::RTDyldMemoryManager>
        (new MemoryManager(reinterpret_cast<LLVMMemoryManager*>(m_llvm_jitmm))));
# endif /* USE_OLD_JIT */

    engine_builder.setOptLevel (llvm::CodeGenOpt::Default);

    m_llvm_exec = engine_builder.create();
    if (! m_llvm_exec)
        return NULL;

    // Force it to JIT as soon as we ask it for the code pointer,
    // don't take any chances that it might JIT lazily, since we
    // will be stealing the JIT code memory from under its nose and
    // destroying the Module & ExecutionEngine.
    m_llvm_exec->DisableLazyCompilation ();
    return m_llvm_exec;

#endif /* OSL_USE_ORC_JIT */
}



void
LLVM_Util::execengine (JitEngine exec)
{
    delete m_llvm_exec;
    m_llvm_exec = exec;
}



void *
LLVM_Util::getPointerToFunction (llvm::Function *func)
{
    DASSERT (func && "passed NULL to getPointerToFunction");
    JitEngine exec = execengine();

#if OSL_USE_ORC_JIT
    exec->addSingleModule (m_llvm_module);
#elif USE_MCJIT
    exec->finalizeObject ();
#endif

    void *f = exec->getPointerToFunction (func);
    ASSERT (f && "could not getPointerToFunction");
    return f;
}



void
LLVM_Util::InstallLazyFunctionCreator (void* (*P)(const std::string &))
{
    JitEngine exec = execengine();
    exec->InstallLazyFunctionCreator (P);
}



void
LLVM_Util::setup_optimization_passes (int optlevel)
{
    ASSERT (m_llvm_passes == NULL);

    // Construct the per-function passes and module-wide (interprocedural
    // optimization) passes.
    //

    // Is there aany reason to call this before an llvm::Module exists ?
    llvm::Module *mod = module();
    m_llvm_passes = new PassManager(mod);

    // Should everything other than 0 branch to else ?
    if (optlevel <= 0 || optlevel >= 4) {
        // Unknown choices for llvm_optimize: use the same basic
        // set of passes that we always have.

        llvm::legacy::PassManager &mpm = m_llvm_passes->modulePass();

#if OSL_LLVM_VERSION <= 34 // Prior behavior, not longer required
        // Always add verifier?
        mpm.add (llvm::createVerifierPass());
#endif

        // Strip and merge functions.
        mpm.add (llvm::createStripDeadPrototypesPass());
        mpm.add (llvm::createMergeFunctionsPass());

        // Simplify the call graph if possible (deleting unreachable blocks, etc.)
        mpm.add (llvm::createCFGSimplificationPass());
        // Change memory references to registers
        //  mpm.add (llvm::createPromoteMemoryToRegisterPass());
#if OSL_LLVM_VERSION <= 36
        // Is there a replacement for this in newer LLVM?
        mpm.add (llvm::createScalarReplAggregatesPass());
#endif
        // Combine instructions where possible -- peephole opts & bit-twiddling
        mpm.add (llvm::createInstructionCombiningPass());
        // Inline small functions
        mpm.add (llvm::createFunctionInliningPass());  // 250?
        // Eliminate early returns
        mpm.add (llvm::createUnifyFunctionExitNodesPass());
        // resassociate exprssions (a = x + (3 + y) -> a = x + y + 3)
        mpm.add (llvm::createReassociatePass());
        // Eliminate common sub-expressions
        mpm.add (llvm::createGVNPass());
        // Constant propagation with SCCP
        mpm.add (llvm::createSCCPPass());
        // More dead code elimination
        mpm.add (llvm::createAggressiveDCEPass());
        // Combine instructions where possible -- peephole opts & bit-twiddling
        mpm.add (llvm::createInstructionCombiningPass());
        // Simplify the call graph if possible (deleting unreachable blocks, etc.)
        mpm.add (llvm::createCFGSimplificationPass());
        // Try to make stuff into registers one last time.
        mpm.add (llvm::createPromoteMemoryToRegisterPass());

#if OSL_LLVM_VERSION > 34
        // Verify the resulting module ...
        // mpm.add (llvm::createVerifierPass());
#endif
    } else {
        // For LLVM 3.0 and higher, llvm_optimize 1-3 means to use the
        // same set of optimizations as clang -O1, -O2, -O3
        m_llvm_passes->defaultPasses(mod, optlevel);
    }
}



void
LLVM_Util::do_optimize (std::string *out_err)
{
    ASSERT(m_llvm_module && "No module to optimize!");

#if OSL_LLVM_VERSION > 35 && !defined(OSL_FORCE_BITCODE_PARSE)
    LLVMErr err = m_llvm_module->materializeAll();
    if (error_string(std::move(err), out_err))
        return;
#endif

    m_llvm_passes->run (*m_llvm_module);
}



void
LLVM_Util::internalize_module_functions (const std::string &prefix,
                                         const string_set &exceptions,
                                         const string_set &moreexceptions)
{
#if OSL_LLVM_VERSION < 40
    for (llvm::Module::iterator iter = module()->begin(); iter != module()->end(); iter++) {
        llvm::Function *sym = static_cast<llvm::Function*>(iter);
#else
    for (llvm::Function& func : module()->getFunctionList()) {
        llvm::Function *sym = &func;
#endif
        std::string symname = sym->getName();
        if (prefix.size() && ! OIIO::Strutil::starts_with(symname, prefix))
            continue;

        if (! exceptions.count(sym->getName()) &&
            ! moreexceptions.count(sym->getName())) {
            llvm::GlobalValue::LinkageTypes linkage = sym->getLinkage();
            // std::cout << "    unnecessary LLVM module function "
            //           << sym->getName().str() << " linkage " << int(linkage) << "\n";
            if (linkage == llvm::GlobalValue::ExternalLinkage)
                sym->setLinkage (llvm::GlobalValue::LinkOnceODRLinkage);
            // ExternalLinkage means it's potentially externally callable,
            // and so will definitely have code generated.
            // LinkOnceODRLinkage keeps one copy so it can be inlined or
            // called internally to the module, but allows it to be
            // discarded otherwise.
        }
    }
#if 0
    // I don't think we need to worry about linkage of global symbols, but
    // here is an example of how to iterate over the globals anyway.
    for (llvm::Module::global_iterator iter = module()->global_begin(); iter != module()->global_end(); iter++) {
        llvm::GlobalValue *sym = llvm::dyn_cast<llvm::GlobalValue>(iter);
        if (!sym)
            continue;
        std::string symname = sym->getName();
        if (prefix.size() && ! OIIO::Strutil::starts_with(symname, prefix))
            continue;

        if (! exceptions.count(sym->getName())) {
            llvm::GlobalValue::LinkageTypes linkage = sym->getLinkage();
            // std::cout << "    unnecessary LLVM global " << sym->getName().str()
            //           << " linkage " << int(linkage) << "\n";
            if (linkage == llvm::GlobalValue::ExternalLinkage)
                f->setLinkage (llvm::GlobalValue::LinkOnceODRLinkage);
        }
    }
#endif
}



llvm::Function *
LLVM_Util::make_function (const std::string &name, bool fastcall,
                          llvm::Type *rettype,
                          llvm::Type *arg1,
                          llvm::Type *arg2,
                          llvm::Type *arg3,
                          llvm::Type *arg4)
{
    llvm::Function *func = llvm::cast<llvm::Function>(
        module()->getOrInsertFunction (name, rettype,
                                       arg1, arg2, arg3, arg4, NULL));
    if (fastcall)
        func->setCallingConv(llvm::CallingConv::Fast);
    return func;
}



llvm::Function *
LLVM_Util::make_function (const std::string &name, bool fastcall,
                          llvm::Type *rettype,
                          const std::vector<llvm::Type*> &params,
                          bool varargs)
{
    llvm::FunctionType *functype = type_function (rettype, params, varargs);
    llvm::Constant *c = module()->getOrInsertFunction (name, functype);
    ASSERT (c && "getOrInsertFunction returned NULL");
    ASSERT_MSG (llvm::isa<llvm::Function>(c),
                "Declaration for %s is wrong, LLVM had to make a cast", name.c_str());
    llvm::Function *func = llvm::cast<llvm::Function>(c);
    if (fastcall)
        func->setCallingConv(llvm::CallingConv::Fast);
    return func;
}



llvm::Value *
LLVM_Util::current_function_arg (int a)
{
    llvm::Function::arg_iterator arg_it = current_function()->arg_begin();
    for (int i = 0;  i < a;  ++i)
        ++arg_it;
#if OSL_LLVM_VERSION <= 36
    return arg_it;
#else
    return &(*arg_it);
#endif
}



llvm::BasicBlock *
LLVM_Util::new_basic_block (const std::string &name)
{
    return llvm::BasicBlock::Create (context(), name, current_function());
}



llvm::BasicBlock *
LLVM_Util::push_function (llvm::BasicBlock *after)
{
    if (! after)
        after = new_basic_block ();
    m_return_block.push_back (after);
    return after;
}



void
LLVM_Util::pop_function ()
{
    ASSERT (! m_return_block.empty());
    builder().SetInsertPoint (m_return_block.back());
    m_return_block.pop_back ();
}



llvm::BasicBlock *
LLVM_Util::return_block () const
{
    ASSERT (! m_return_block.empty());
    return m_return_block.back();
}



void 
LLVM_Util::push_loop (llvm::BasicBlock *step, llvm::BasicBlock *after)
{
    m_loop_step_block.push_back (step);
    m_loop_after_block.push_back (after);
}



void 
LLVM_Util::pop_loop ()
{
    ASSERT (! m_loop_step_block.empty() && ! m_loop_after_block.empty());
    m_loop_step_block.pop_back ();
    m_loop_after_block.pop_back ();
}



llvm::BasicBlock *
LLVM_Util::loop_step_block () const
{
    ASSERT (! m_loop_step_block.empty());
    return m_loop_step_block.back();
}



llvm::BasicBlock *
LLVM_Util::loop_after_block () const
{
    ASSERT (! m_loop_after_block.empty());
    return m_loop_after_block.back();
}




llvm::Type *
LLVM_Util::type_union(const std::vector<llvm::Type *> &types)
{
    llvm::DataLayout target(module());
    size_t max_size = 0;
    size_t max_align = 1;
    for (size_t i = 0; i < types.size(); ++i) {
        size_t size = target.getTypeStoreSize(types[i]);
        size_t align = target.getABITypeAlignment(types[i]);
        max_size  = size  > max_size  ? size  : max_size;
        max_align = align > max_align ? align : max_align;
    }
    size_t padding = (max_size % max_align) ? max_align - (max_size % max_align) : 0;
    size_t union_size = max_size + padding;

    llvm::Type * base_type = NULL;
    // to ensure the alignment when included in a struct use
    // an appropiate type for the array
    if (max_align == sizeof(void*))
        base_type = type_void_ptr();
    else if (max_align == 4)
        base_type = (llvm::Type *) llvm::Type::getInt32Ty (context());
    else if (max_align == 2)
        base_type = (llvm::Type *) llvm::Type::getInt16Ty (context());
    else
        base_type = (llvm::Type *) llvm::Type::getInt8Ty (context());

    size_t array_len = union_size / target.getTypeStoreSize(base_type);
    return (llvm::Type *) llvm::ArrayType::get (base_type, array_len);
}



llvm::Type *
LLVM_Util::type_struct (const std::vector<llvm::Type *> &types,
                        const std::string &name)
{
    return llvm::StructType::create(context(), types, name);
}



llvm::Type *
LLVM_Util::type_ptr (llvm::Type *type)
{
    return llvm::PointerType::get (type, 0);
}



llvm::Type *
LLVM_Util::type_array (llvm::Type *type, int n)
{
    return llvm::ArrayType::get (type, n);
}



llvm::FunctionType *
LLVM_Util::type_function (llvm::Type *rettype,
                          const std::vector<llvm::Type*> &params,
                          bool varargs)
{
    return llvm::FunctionType::get (rettype, params, varargs);
}



llvm::PointerType *
LLVM_Util::type_function_ptr (llvm::Type *rettype,
                              const std::vector<llvm::Type*> &params,
                              bool varargs)
{
    llvm::FunctionType *functype = type_function (rettype, params, varargs);
    return llvm::PointerType::getUnqual (functype);
}



std::string
LLVM_Util::llvm_typename (llvm::Type *type) const
{
    std::string s;
    llvm::raw_string_ostream stream (s);
    stream << (*type);
    return stream.str();
}



llvm::Type *
LLVM_Util::llvm_typeof (llvm::Value *val) const
{
    return val->getType();
}



std::string
LLVM_Util::llvm_typenameof (llvm::Value *val) const
{
    return llvm_typename (llvm_typeof (val));
}



llvm::Value *
LLVM_Util::constant (float f)
{
    return llvm::ConstantFP::get (context(), llvm::APFloat(f));
}



llvm::Value *
LLVM_Util::constant (int i)
{
    return llvm::ConstantInt::get (context(), llvm::APInt(32,i));
}



llvm::Value *
LLVM_Util::constant (size_t i)
{
    int bits = sizeof(size_t)*8;
    return llvm::ConstantInt::get (context(), llvm::APInt(bits,i));
}



llvm::Value *
LLVM_Util::constant_bool (bool i)
{
    return llvm::ConstantInt::get (context(), llvm::APInt(1,i));
}



llvm::Value *
LLVM_Util::constant_ptr (void *p, llvm::PointerType *type)
{
    if (! type)
        type = type_void_ptr();
    return builder().CreateIntToPtr (constant (size_t (p)), type, "const pointer");
}



llvm::Value *
LLVM_Util::constant (ustring s)
{
    // Create a const size_t with the ustring contents
    size_t bits = sizeof(size_t)*8;
    llvm::Value *str = llvm::ConstantInt::get (context(),
                               llvm::APInt(bits,size_t(s.c_str()), true));
    // Then cast the int to a char*.
    return builder().CreateIntToPtr (str, type_string(), "ustring constant");
}



llvm::Value *
LLVM_Util::constant (const TypeDesc &type)
{
    long long *i = (long long *)&type;
    return llvm::ConstantInt::get (context(), llvm::APInt(64,*i));
}



llvm::Value *
LLVM_Util::void_ptr_null ()
{
    return llvm::ConstantPointerNull::get (type_void_ptr());
}



llvm::Value *
LLVM_Util::ptr_to_cast (llvm::Value* val, llvm::Type *type)
{
    return builder().CreatePointerCast(val,llvm::PointerType::get(type, 0));
}



llvm::Value *
LLVM_Util::ptr_cast (llvm::Value* val, llvm::Type *type)
{
    return builder().CreatePointerCast(val,type);
}



llvm::Value *
LLVM_Util::ptr_cast (llvm::Value* val, const TypeDesc &type)
{
    return ptr_cast (val, llvm::PointerType::get (llvm_type(type), 0));
}



llvm::Value *
LLVM_Util::void_ptr (llvm::Value* val)
{
    return builder().CreatePointerCast(val,type_void_ptr());
}




llvm::Type *
LLVM_Util::llvm_type (const TypeDesc &typedesc)
{
    TypeDesc t = typedesc.elementtype();
    llvm::Type *lt = NULL;
    if (t == TypeDesc::FLOAT)
        lt = type_float();
    else if (t == TypeDesc::INT)
        lt = type_int();
    else if (t == TypeDesc::STRING)
        lt = type_string();
    else if (t.aggregate == TypeDesc::VEC3)
        lt = type_triple();
    else if (t.aggregate == TypeDesc::MATRIX44)
        lt = type_matrix();
    else if (t == TypeDesc::NONE)
        lt = type_void();
    else if (t == TypeDesc::UINT8)
        lt = type_char();
    else if (t == TypeDesc::PTR)
        lt = type_void_ptr();
    else {
        std::cerr << "Bad llvm_type(" << typedesc << ")\n";
        ASSERT (0 && "not handling this type yet");
    }
    if (typedesc.arraylen)
        lt = llvm::ArrayType::get (lt, typedesc.arraylen);
    DASSERT (lt);
    return lt;
}



llvm::Value *
LLVM_Util::offset_ptr (llvm::Value *ptr, int offset, llvm::Type *ptrtype)
{
    llvm::Value *i = builder().CreatePtrToInt (ptr, type_addrint());
    i = builder().CreateAdd (i, constant ((size_t)offset));
    ptr = builder().CreateIntToPtr (i, type_void_ptr());
    if (ptrtype)
        ptr = ptr_cast (ptr, ptrtype);
    return ptr;
}



llvm::Value *
LLVM_Util::op_alloca (llvm::Type *llvmtype, int n, const std::string &name)
{
    llvm::ConstantInt* numalloc = (llvm::ConstantInt*)constant(n);
    return builder().CreateAlloca (llvmtype, numalloc, name);
}



llvm::Value *
LLVM_Util::op_alloca (const TypeDesc &type, int n, const std::string &name)
{
    return op_alloca (llvm_type(type.elementtype()), n*type.numelements(), name);
}



llvm::Value *
LLVM_Util::call_function (llvm::Value *func, llvm::Value **args, int nargs)
{
    ASSERT (func);
#if 0
    llvm::outs() << "llvm_call_function " << *func << "\n";
    llvm::outs() << nargs << " args:\n";
    for (int i = 0;  i < nargs;  ++i)
        llvm::outs() << "\t" << *(args[i]) << "\n";
#endif
    //llvm_gen_debug_printf (std::string("start ") + std::string(name));
    llvm::Value *r = builder().CreateCall (func, llvm::ArrayRef<llvm::Value *>(args, nargs));
    //llvm_gen_debug_printf (std::string(" end  ") + std::string(name));
    return r;
}



llvm::Value *
LLVM_Util::call_function (const char *name, llvm::Value **args, int nargs)
{
    llvm::Function *func = module()->getFunction (name);
    if (! func) {
#ifdef OSL_SPLIT_BITCODES
        if (const bytecode_func *bf = bytecode_for_function (name)) {

            std::string err;
            std::unique_ptr<llvm::Module> mod(
                                module_from_bitcode (bf->first, bf->second,
                                                     name, &err));
            if (! mod) {
                std::cerr << "Couldn't load bitcode for '" << name << "'\n";
                return NULL;
            }
#if !OSL_USE_ORC_JIT
            func = mod->getFunction (name);
            if (! func) {
                std::cerr << "Function '" << name << "' not in module '"
# if OSL_LLVM_VERSION >= 36
                          << mod->getName().str ()
# else
                          << mod->getModuleIdentifier ()
# endif
                          << "'\n" << "  : " << err << '\n';

                return NULL;
            }

# if OSL_LLVM_VERSION >= 35
            LLVMErr ec = func->materialize();
            if (error_string (std::move(ec), &err))
# else
            if (func->Materialize(&err))
# endif
            {
                std::cerr << "Error materializing function '"
                          << name << "'\n" << "  : " << err << '\n';
                return NULL;
            }
            // Merge the module into the active base module.
            // -mod- cannot be used after this call.
# if OSL_LLVM_VERSION >= 38
            if (llvm::Linker::linkModules (*m_llvm_module, std::move(mod)))
# else
            if (llvm::Linker::LinkModules (m_llvm_module, mod.get(),
                                           llvm::Linker::DestroySource, &err))
# endif
            {
                std::cerr << "Couldn't link modules\n";
                return NULL;
            }

#else /* OSL_USE_ORC_JIT */
            execengine()->linkModule(std::move(mod));
#endif
            // Grab the function from the merged module
            func = m_llvm_module->getFunction (name);
            if (func)
                return call_function (func, args, nargs);
        }
#endif /* OSL_SPLIT_BITCODES */
        std::cerr << "Couldn't find function '" << name << "'\n";
    }
    return call_function (func, args, nargs);
}



void
LLVM_Util::mark_fast_func_call (llvm::Value *funccall)
{
    llvm::CallInst* call_inst = llvm::cast<llvm::CallInst>(funccall);
    call_inst->setCallingConv (llvm::CallingConv::Fast);
}



void
LLVM_Util::op_branch (llvm::BasicBlock *block)
{
    builder().CreateBr (block);
    set_insert_point (block);
}



void
LLVM_Util::op_branch (llvm::Value *cond, llvm::BasicBlock *trueblock,
                      llvm::BasicBlock *falseblock)
{
    builder().CreateCondBr (cond, trueblock, falseblock);
    set_insert_point (trueblock);
}



void
LLVM_Util::set_insert_point (llvm::BasicBlock *block)
{
    builder().SetInsertPoint (block);
}



void
LLVM_Util::op_return (llvm::Value *retval)
{
    if (retval)
        builder().CreateRet (retval);
    else
        builder().CreateRetVoid ();
}



void
LLVM_Util::op_memset (llvm::Value *ptr, int val, int len, int align)
{
    op_memset(ptr, val, constant(len), align);
}



void
LLVM_Util::op_memset (llvm::Value *ptr, int val, llvm::Value *len, int align)
{
    // memset with i32 len
    // and with an i8 pointer (dst) for LLVM-2.8
    llvm::Type* types[] = {
        (llvm::Type *) llvm::PointerType::get(llvm::Type::getInt8Ty(context()), 0),
        (llvm::Type *) llvm::Type::getInt32Ty(context())
    };

    llvm::Function* func = llvm::Intrinsic::getDeclaration (module(),
        llvm::Intrinsic::memset,
        llvm::ArrayRef<llvm::Type *>(types, sizeof(types)/sizeof(llvm::Type*)));

    // NOTE(boulos): constant(0) would return an i32
    // version of 0, but we need the i8 version. If we make an
    // ::constant(char val) though then we'll get ambiguity
    // everywhere.
    llvm::Value* fill_val = llvm::ConstantInt::get (context(),
                                                    llvm::APInt(8, val));
    // Non-volatile (allow optimizer to move it around as it wishes
    // and even remove it if it can prove it's useless)
#if OSL_LLVM_VERSION <= 36
    builder().CreateCall5 (func, ptr, fill_val, len, constant(align),
                           constant_bool(false));
#else
    llvm::Value *args[5] = {
        ptr, fill_val, len, constant(align), constant_bool(false)
    };
    builder().CreateCall (func, llvm::ArrayRef<llvm::Value*>(args, 5));

#endif
}



void
LLVM_Util::op_memcpy (llvm::Value *dst, llvm::Value *src, int len, int align)
{
    // i32 len
    // and with i8 pointers (dst and src) for LLVM-2.8
    llvm::Type* types[] = {
        (llvm::Type *) llvm::PointerType::get(llvm::Type::getInt8Ty(context()), 0),
        (llvm::Type *) llvm::PointerType::get(llvm::Type::getInt8Ty(context()), 0),
        (llvm::Type *) llvm::Type::getInt32Ty(context())
    };

    llvm::Function* func = llvm::Intrinsic::getDeclaration (module(),
        llvm::Intrinsic::memcpy,
        llvm::ArrayRef<llvm::Type *>(types, sizeof(types)/sizeof(llvm::Type*)));

    // Non-volatile (allow optimizer to move it around as it wishes
    // and even remove it if it can prove it's useless)
#if OSL_LLVM_VERSION <= 36
    builder().CreateCall5 (func, dst, src,
                           constant(len), constant(align), constant_bool(false));
#else
    llvm::Value *args[5] = {
        dst, src, constant(len), constant(align), constant_bool(false)
    };
    builder().CreateCall (func, llvm::ArrayRef<llvm::Value*>(args, 5));
#endif
}



llvm::Value *
LLVM_Util::op_load (llvm::Value *ptr)
{
    return builder().CreateLoad (ptr);
}



void
LLVM_Util::op_store (llvm::Value *val, llvm::Value *ptr)
{
    builder().CreateStore (val, ptr);
}



llvm::Value *
LLVM_Util::GEP (llvm::Value *ptr, llvm::Value *elem)
{
    return builder().CreateGEP (ptr, elem);
}



llvm::Value *
LLVM_Util::GEP (llvm::Value *ptr, int elem)
{
    return builder().CreateConstGEP1_32 (ptr, elem);
}



llvm::Value *
LLVM_Util::GEP (llvm::Value *ptr, int elem1, int elem2)
{
#if OSL_LLVM_VERSION <= 36
    return builder().CreateConstGEP2_32 (ptr, elem1, elem2);
#else
    return builder().CreateConstGEP2_32 (nullptr, ptr, elem1, elem2);
#endif
}



llvm::Value *
LLVM_Util::op_add (llvm::Value *a, llvm::Value *b)
{
    if (a->getType() == type_float() && b->getType() == type_float())
        return builder().CreateFAdd (a, b);
    if (a->getType() == type_int() && b->getType() == type_int())
        return builder().CreateAdd (a, b);
    ASSERT (0 && "Op has bad value type combination");
}



llvm::Value *
LLVM_Util::op_sub (llvm::Value *a, llvm::Value *b)
{
    if (a->getType() == type_float() && b->getType() == type_float())
        return builder().CreateFSub (a, b);
    if (a->getType() == type_int() && b->getType() == type_int())
        return builder().CreateSub (a, b);
    ASSERT (0 && "Op has bad value type combination");
}



llvm::Value *
LLVM_Util::op_neg (llvm::Value *a)
{
    if (a->getType() == type_float())
        return builder().CreateFNeg (a);
    if (a->getType() == type_int())
        return builder().CreateNeg (a);
    ASSERT (0 && "Op has bad value type combination");
}



llvm::Value *
LLVM_Util::op_mul (llvm::Value *a, llvm::Value *b)
{
    if (a->getType() == type_float() && b->getType() == type_float())
        return builder().CreateFMul (a, b);
    if (a->getType() == type_int() && b->getType() == type_int())
        return builder().CreateMul (a, b);
    ASSERT (0 && "Op has bad value type combination");
}



llvm::Value *
LLVM_Util::op_div (llvm::Value *a, llvm::Value *b)
{
    if (a->getType() == type_float() && b->getType() == type_float())
        return builder().CreateFDiv (a, b);
    if (a->getType() == type_int() && b->getType() == type_int())
        return builder().CreateSDiv (a, b);
    ASSERT (0 && "Op has bad value type combination");
}



llvm::Value *
LLVM_Util::op_mod (llvm::Value *a, llvm::Value *b)
{
    if (a->getType() == type_float() && b->getType() == type_float())
        return builder().CreateFRem (a, b);
    if (a->getType() == type_int() && b->getType() == type_int())
        return builder().CreateSRem (a, b);
    ASSERT (0 && "Op has bad value type combination");
}



llvm::Value *
LLVM_Util::op_float_to_int (llvm::Value* a)
{
    if (a->getType() == type_float())
        return builder().CreateFPToSI(a, type_int());
    if (a->getType() == type_int())
        return a;
    ASSERT (0 && "Op has bad value type combination");
}



llvm::Value *
LLVM_Util::op_float_to_double (llvm::Value* a)
{
    ASSERT (a->getType() == type_float());
    return builder().CreateFPExt(a, llvm::Type::getDoubleTy(context()));
}



llvm::Value *
LLVM_Util::op_int_to_float (llvm::Value* a)
{
    if (a->getType() == type_int())
        return builder().CreateSIToFP(a, type_float());
    if (a->getType() == type_float())
        return a;
    ASSERT (0 && "Op has bad value type combination");
}



llvm::Value *
LLVM_Util::op_bool_to_int (llvm::Value* a)
{
    if (a->getType() == type_bool())
        return builder().CreateZExt (a, type_int());
    if (a->getType() == type_int())
        return a;
    ASSERT (0 && "Op has bad value type combination");
}



llvm::Value *
LLVM_Util::op_and (llvm::Value *a, llvm::Value *b)
{
    return builder().CreateAnd (a, b);
}


llvm::Value *
LLVM_Util::op_or (llvm::Value *a, llvm::Value *b)
{
    return builder().CreateOr (a, b);
}


llvm::Value *
LLVM_Util::op_xor (llvm::Value *a, llvm::Value *b)
{
    return builder().CreateXor (a, b);
}


llvm::Value *
LLVM_Util::op_shl (llvm::Value *a, llvm::Value *b)
{
    return builder().CreateShl (a, b);
}


llvm::Value *
LLVM_Util::op_shr (llvm::Value *a, llvm::Value *b)
{
    if (a->getType() == type_int() && b->getType() == type_int())
        return builder().CreateAShr (a, b);  // signed int -> arithmetic shift
    ASSERT (0 && "Op has bad value type combination");
}



llvm::Value *
LLVM_Util::op_not (llvm::Value *a)
{
    return builder().CreateNot (a);
}



llvm::Value *
LLVM_Util::op_select (llvm::Value *cond, llvm::Value *a, llvm::Value *b)
{
    return builder().CreateSelect (cond, a, b);
}



llvm::Value *
LLVM_Util::op_eq (llvm::Value *a, llvm::Value *b, bool ordered)
{
    ASSERT (a->getType() == b->getType());
    if (a->getType() == type_float())
        return ordered ? builder().CreateFCmpOEQ (a, b) : builder().CreateFCmpUEQ (a, b);
    else
        return builder().CreateICmpEQ (a, b);
}



llvm::Value *
LLVM_Util::op_ne (llvm::Value *a, llvm::Value *b, bool ordered)
{
    ASSERT (a->getType() == b->getType());
    if (a->getType() == type_float())
        return ordered ? builder().CreateFCmpONE (a, b) : builder().CreateFCmpUNE (a, b);
    else
        return builder().CreateICmpNE (a, b);
}



llvm::Value *
LLVM_Util::op_gt (llvm::Value *a, llvm::Value *b, bool ordered)
{
    ASSERT (a->getType() == b->getType());
    if (a->getType() == type_float())
        return ordered ? builder().CreateFCmpOGT (a, b) : builder().CreateFCmpUGT (a, b);
    else
        return builder().CreateICmpSGT (a, b);
}



llvm::Value *
LLVM_Util::op_lt (llvm::Value *a, llvm::Value *b, bool ordered)
{
    ASSERT (a->getType() == b->getType());
    if (a->getType() == type_float())
        return ordered ? builder().CreateFCmpOLT (a, b) : builder().CreateFCmpULT (a, b);
    else
        return builder().CreateICmpSLT (a, b);
}



llvm::Value *
LLVM_Util::op_ge (llvm::Value *a, llvm::Value *b, bool ordered)
{
    ASSERT (a->getType() == b->getType());
    if (a->getType() == type_float())
        return ordered ? builder().CreateFCmpOGE (a, b) : builder().CreateFCmpUGE (a, b);
    else
        return builder().CreateICmpSGE (a, b);
}



llvm::Value *
LLVM_Util::op_le (llvm::Value *a, llvm::Value *b, bool ordered)
{
    ASSERT (a->getType() == b->getType());
    if (a->getType() == type_float())
        return ordered ? builder().CreateFCmpOLE (a, b) : builder().CreateFCmpULE (a, b);
    else
        return builder().CreateICmpSLE (a, b);
}



void
LLVM_Util::write_bitcode_file (const char *filename, std::string *err)
{
#if OSL_LLVM_VERSION >= 36
    std::error_code local_error;
    llvm::raw_fd_ostream out (filename, local_error, llvm::sys::fs::F_None);
#elif OSL_LLVM_VERSION >= 35
    std::string local_error;
    llvm::raw_fd_ostream out (filename, err ? *err : local_error, llvm::sys::fs::F_None);
#else
    std::string local_error;
    llvm::raw_fd_ostream out (filename, err ? *err : local_error);
#endif
    llvm::WriteBitcodeToFile (module(), out);

#if OSL_LLVM_VERSION >= 36
    if (err && local_error)
        *err = local_error.message ();
#endif
}



std::string
LLVM_Util::bitcode_string (llvm::Function *func)
{
    std::string s;
    llvm::raw_string_ostream stream (s);
    stream << (*func);
    return stream.str();
}



void
LLVM_Util::delete_func_body (llvm::Function *func)
{
    func->deleteBody ();
}



bool
LLVM_Util::func_is_empty (llvm::Function *func)
{
    return func->size() == 1 // func has just one basic block
        && func->front().size() == 1;  // the block has one instruction,
                                       ///   presumably the ret
}


std::string
LLVM_Util::func_name (llvm::Function *func)
{
    return func->getName().str();
}


}; // namespace pvt
OSL_NAMESPACE_EXIT
