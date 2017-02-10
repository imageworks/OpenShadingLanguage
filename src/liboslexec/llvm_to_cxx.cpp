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

#include "oslversion.h"

#include <llvm/ADT/SetVector.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileUtilities.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Transforms/IPO.h>

#if OSL_LLVM_VERSION >= 35
#  include <llvm/IR/Verifier.h>
#  include <llvm/Bitcode/BitcodeWriterPass.h>
#  include <llvm/IR/IRPrintingPasses.h>
# if OSL_LLVM_VERSION >= 39
#  include <llvm/Support/Error.h>
# endif
#else
#  include <llvm/Analysis/Verifier.h>
#  include <llvm/Assembly/PrintModulePass.h>
#  include <llvm/Bitcode/ReaderWriter.h>
#  include <llvm/Support/ManagedStatic.h>
#endif

#if OSL_BUILD_CPP11
#  include <system_error>
#endif

#include <set>
#include <string>
#include <stdlib.h>

#ifdef _WIN32
extern "C" char* __unDName(char *demangled, const char *mangled, int out_len,
                           void * (* pAlloc )(size_t), void (* pFree )(void *),
                           unsigned short int flags);
#else
#  include <cxxabi.h>
#endif

#define WRITE_CXX_11 (OSL_BUILD_CPP11 || OSL_BUILD_CPP14)

struct File {
    FILE *f;
    File(const std::string &base, const std::string &ext, FILE *def) {
        f = base.empty() ? def : ::fopen((base+ext).c_str(), "w");
    }
    ~File() { if (f && f != stdout && f != stderr) ::fclose(f); }

    void write(llvm::StringRef str) { ::fwrite(str.data(), sizeof(char), str.size(), f); }
    void write(llvm::StringRef a, llvm::StringRef b, llvm::StringRef c) {
        write(a); write(b); write(c);
    }
};


class HexDumper {
    File m_header, m_output;
    const llvm::StringRef m_prefix;
    size_t m_max_size;

    // Save the max size encountered and get the smallest type that can hold len
    const char* typeChoice (size_t len);
    
    std::string var_name (const std::string &name, const std::string &post) {
        if (name.empty())
            return m_prefix.str() + post;
        return m_prefix.str() + "_" + name + post;
    }
public:
    HexDumper (const std::string &name, llvm::StringRef p,  bool mapit);
    ~HexDumper ();

    // Dump the bytecode in buf to a c array and length variable named:
    // (m_prefix + name + "_block") & (m_prefix + name + "_size")
    bool operator () (const llvm::SmallVectorImpl<char> &buf,
                      const std::string &name);

    // Generate c++ code for retrieving the bytecode by function name
    bool operator () (const std::set<std::string> &funcs);

    bool valid() const { return m_header.f && m_output.f; }
    void write_both(llvm::StringRef str) { m_header.write(str); m_output.write(str); }
    std::string array_name(const std::string &name) { return var_name(name, "_block"); }
    std::string size_name(const std::string &name) { return var_name(name, "_size"); }

    File &header () { return m_header; }
    File &output () { return m_output; }
};

HexDumper::HexDumper (const std::string &name, llvm::StringRef p, bool mapit) :
    m_header(name, ".h", stderr), m_output(name, ".cpp", stdout), m_prefix(p), m_max_size(0)
{
    if (valid()) {
        // FIXME: cmdline option
        m_header.write("#pragma once\n");
        m_header.write("#include \"oslversion.h\"\n");
        m_header.write("#include <cstddef>\n"); // for size_t
        m_output.write("#include \"",
                       (llvm::sys::path::filename(name).str()+".h").c_str(),
                       "\"\n");

        if (mapit) {
            m_header.write("#include <utility>\n");
            m_header.write("#include <string>\n");
#if WRITE_CXX_11
            m_output.write("#include <unordered_map>\n");
#else
            m_output.write("#include <boost/unordered_map.hpp>\n");
#endif
            m_header.write("#ifndef OSL_SPLIT_BITCODES\n");
        } else
            m_header.write("#ifdef OSL_SPLIT_BITCODES\n");
        m_header.write("#  error \"",
                       "Bitcode generation is misconfigured. "
                       "Re-run CMake with -DOSL_SPLIT_BITCODES=ON|OFF", "\"\n");
        m_header.write("#endif\n");


        // FIXME: cmdline option
        write_both("\nOSL_NAMESPACE_ENTER\n");
        write_both("namespace pvt {\n");
    }
}

HexDumper::~HexDumper ()
{
    // FIXME: cmdline option
    write_both("\n} // namespace pvt\n");
    write_both("OSL_NAMESPACE_EXIT\n");
}

const char*
HexDumper::typeChoice (size_t len)
{
    m_max_size = (std::max)(m_max_size, len);
    if (len <= std::numeric_limits<unsigned char>::max())
        return "unsigned char ";
    else if (len <= std::numeric_limits<unsigned short>::max())
        return "unsigned short ";
    else if (len <= std::numeric_limits<unsigned>::max())
        return "unsigned ";

    return "size__t";
}

bool HexDumper::operator () (const std::set<std::string> &funcs)
{
    // Save some space and use the smallest type that can hold the largest size
    const llvm::StringRef sz_type = typeChoice(m_max_size);

    // condense the stl craziness
    const std::string pair_str("bytecode_func");

    // typedef std::pair<> bytecode_func;

#if WRITE_CXX_11
    m_header.write("typedef std::pair<const unsigned char*, const ", sz_type, "> bytecode_func;\n\n");
    const std::string map_str(std::string("std::unordered_map<std::string, const ") + pair_str + ">");
#else
    m_header.write("typedef std::pair<const unsigned char*, ", sz_type, "> bytecode_func;\n\n");
    const std::string map_str(std::string("boost::unordered_map<std::string, ") + pair_str + ">");
#endif

    // static std::map<> &lookup_map() accessor definition
    m_output.write("\nnamespace {\n");
    m_output.write("static ", map_str, " &lookup_map() {\n");
    m_output.write("    static ", map_str, " sMap;\n");
    m_output.write("    return sMap;\n}\n");
    m_output.write("} // anonymous namespace\n\n");

    // void init_function_bytecodes() signature
    write_both("void init_function_bytecodes ()");
    m_header.write(";\n");

    // definition
    m_output.write(" {\n");
    m_output.write("   ", map_str, " &lmap = lookup_map();\n\n");
    for (const std::string &fname : funcs) {

#if WRITE_CXX_11
        m_output.write("   lmap.emplace(std::make_pair(\"", fname, "\", ");
#else
        m_output.write("   lmap[\"", fname, "\"] = ");
#endif
        m_output.write(pair_str);
        m_output.write("(");
        m_output.write(array_name(fname), ",", size_name(fname));

#if WRITE_CXX_11
        m_output.write(")));\n"); // c++11
#else
        m_output.write(");\n"); // c++0x
#endif
    }
    m_output.write("}\n\n");

    // std::pair<> *bytecode_for_function(const std::string&) signature
    write_both("const ");
    write_both(pair_str);
    write_both(" * bytecode_for_function(const std::string &name)");
    m_header.write(";\n");

    // definition
    m_output.write(" {\n");
    m_output.write("    const ", map_str, " &lmap = lookup_map();\n");
    m_output.write("    const ", map_str, "::const_iterator itr = lmap.find(name);\n");
    m_output.write("    return itr != lmap.end() ? &itr->second : NULL;\n");
    m_output.write("}\n");

    return true;
}

bool HexDumper::operator () (const llvm::SmallVectorImpl<char> &buf,
                             const std::string &name)
{
    // ${prefix}_${name}_<size|block>
    const std::string aname = array_name(name), sname = size_name(name);

    // declarations
    m_header.write("\nextern const unsigned char ", aname, "[];\n");
    //header.write("extern const size_t ", sname, ";\n");

    // byte array
    int col = 0;
    m_output.write("\nconst unsigned char ", aname, "[] = {\n");
    for (const char c : buf) {
        fprintf(m_output.f, "0x%02x, ", int(c) & 0xff);
        col += 6;
        if (col > 80) {
            m_output.write("\n");
            col = 0;
        }
    }
    // No null termination IR size needs to be aligned properly
    m_output.write(" };\n");

    // I know the size better.
    // output.write("const size_t ", sname, " = ");
    // output.write("sizeof(", aname, ")-1;\n");

    const llvm::StringRef type = typeChoice(buf.size());
    m_header.write("extern const ", type, sname);
    m_header.write(";\n");
    m_output.write("const ", type, sname);
    ::fprintf(m_output.f, " = %lu;\n", buf.size());

    return true;
}

static bool Demangle(const std::string &name, std::string& symbol)
{
  struct AutoFree {
    char* str;
    AutoFree(char* Ptr) : str(Ptr) {}
    ~AutoFree() { ::free(str); };
  };
  int status = 0;
#ifdef _WIN32
  AutoFree af(__unDName(0, name.c_str(), 0, ::malloc, ::free, 0));
#else
  AutoFree af(abi::__cxa_demangle(name.c_str(), NULL, NULL, &status));
#endif
    if (status == 0 && af.str) {
        symbol = af.str;
        return true;
    }
    return false;
}

#if OSL_LLVM_VERSION <= 34
typedef llvm::error_code LLVMErr;
typedef llvm::error_code std_error_code;
#else
typedef std::error_code std_error_code;
# if OSL_LLVM_VERSION >= 40
typedef llvm::Error LLVMErr;
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
# else
typedef std::error_code LLVMErr;
# endif /* OSL_LLVM_VERSION >= 40 */
#endif /* OSL_LLVM_VERSION <= 34 */

inline bool error_string (const std_error_code &err, std::string *str) {
    if (err) {
        if (str) *str = err.message();
        return true;
    }
    return false;
}

using namespace llvm;

// Just to limit the help output
static cl::OptionCategory
s_category("dumb options", "options for controlling the IR dump");

// infile or - for stdin
static cl::opt<std::string>
s_input_name(cl::Positional, cl::desc("<input .ll or .bc file>"),
             cl::init("-"), cl::cat(s_category));

// -o <outfile>
static cl::opt<std::string>
s_output_name("o", cl::desc("Override output filename"),
              cl::value_desc("filename"), cl::cat(s_category));

// --prefix= prefix to use writing c++ symbols
static cl::opt<std::string>
s_prefix_name("prefix", cl::desc("Output symbol prefix"),
              cl::value_desc("prefix"), cl::cat(s_category));


// --func= prefix to use writing c++ variables
static cl::list<std::string>
s_prefix_list("func", cl::desc("Specify prefix on functions to extract"),
              cl::ZeroOrMore, cl::value_desc("function prefix"),
              cl::cat(s_category));

// -ir dump individual ll files for the extracted functions
static cl::opt<bool>
s_dumpir_file("ir", cl::desc("Dump individual IR files"),
              cl::value_desc("ir"), cl::cat(s_category));


static bool verify_module(Module &module) {
#if OSL_LLVM_VERSION <= 34
    std::string err;
    if (verifyModule(module, ReturnStatusAction, &err))
#else
    if (verifyModule(module, &errs()))
#endif
    {
        errs() << "assembly parsed, but does not verify as correct!\n";
#if OSL_LLVM_VERSION < 34
        errs() << err;
#endif
        return false;
    }
    return true;
}

std::unique_ptr<Module> static
loadModule(LLVMContext &context, const char **argv)
{
    SMDiagnostic sm_err;
    std::unique_ptr<Module> module(getLazyIRFileModule(s_input_name,
                                                       sm_err, context));
    if (!module) {
        sm_err.print(argv[0], errs());
        return nullptr;
    }
    return module;
}

static raw_fd_ostream* file_stream(const std::string &path, std::string &err) {
#if OSL_LLVM_VERSION >= 36
        struct err_to_str {
            std::string &err;
            mutable std::error_code err_code;
            err_to_str(std::string &e) : err(e) {}
            ~err_to_str() { if (err_code) err = err_code.message(); }
            operator std::error_code& () const { return err_code; };
        };
        return new raw_fd_ostream(path, err_to_str(err), sys::fs::F_None);
#else
# if OSL_LLVM_VERSION >= 35
        return new raw_fd_ostream(path, err, sys::fs::F_None);
# else
        return new raw_fd_ostream(path.c_str(), err, sys::fs::F_None);
# endif
#endif
}

static bool dump_module(Module &module,
                        llvm::SmallVectorImpl<char> &buf,
                        const std::string &ir_path = "",
                        bool save_uselist = false)
{
    // Mark the module as fully materialized.
    std::string err;

#if OSL_LLVM_VERSION >= 35
    LLVMErr ec = module.materializeAll();
    if (error_string(std::move(ec), &err))
#else
    if (module.MaterializeAll(&err))
#endif
    {
        errs() << "error materializing module: " << err << "\n";
        return false;
    }

    if (true) {
        legacy::PassManager strip_passes;
        strip_passes.add(createStripDeadDebugInfoPass());  // Remove dead debug info
        strip_passes.add(createStripDeadPrototypesPass()); // Remove dead func decls
        strip_passes.run(module);
    }

    if (!verify_module(module))
        return false;

    legacy::PassManager passes;

    std::unique_ptr<raw_fd_ostream> ir_out;
    if (!ir_path.empty()) {
        std::string err;
        ir_out.reset(file_stream(ir_path, err));
        if (!err.empty()) {
            llvm::errs() << "Could not create '" << ir_path << "': " << err << '\n';
            return false;
        }
#if OSL_LLVM_VERSION >= 37
        passes.add(createPrintModulePass(*ir_out, "", save_uselist));
#elif OSL_LLVM_VERSION >= 35
        passes.add(createPrintModulePass(*ir_out));
#else
        passes.add(createPrintModulePass(ir_out.get()));
#endif
    }

    buf.resize(0);
    llvm::raw_svector_ostream out(buf);

#if OSL_LLVM_VERSION >= 36
    passes.add(createBitcodeWriterPass(out, save_uselist));
#else
    passes.add(createBitcodeWriterPass(out));
#endif
    passes.run(module);
    return true;
}

static bool dump_module(Module &module, const std::string &name,
                        llvm::SmallVectorImpl<char> &buf,
                        const std::string &ir_path,
                        std::vector<GlobalValue*> *globalv = nullptr,
                        bool save_uselist = false)
{
    // Match the module name to the function name
    module.setModuleIdentifier(name);

    std::vector<GlobalValue*> single_func;
    if (!globalv) {
        Function *func = module.getFunction(name);
        if (!func) {
            errs() << "Function '" << name << "' is missing!";
            return false;
        }
        single_func.push_back(func);
        globalv = &single_func;
    }

    // Load everything from backing store that has been requested
    for (GlobalValue *gv : *globalv) {
        std::string err;
#if OSL_LLVM_VERSION >= 35
        LLVMErr ec = gv->materialize();
        if (error_string(std::move(ec), &err))
#else
        if (gv->Materialize(&err))
#endif
        {
            errs() << "error materializing function '" << name << "': " << err << "\n";
            return false;
        }
    }

    // Try to destroy everythin but the globals that are being dumped.
    legacy::PassManager extract;
    extract.add(createGVExtractionPass(*globalv));
    extract.run(module);

    return dump_module(module, buf, ir_path, save_uselist);
}

static bool should_save_function(llvm::StringRef name)
{
    for (const auto &str : s_prefix_list) {
        if (name.startswith(str))
            return true;
    }
    return false;
}

static void file_path(std::string &dir, size_t dir_len, llvm::StringRef name,
                      const char *ext = nullptr)
{
    dir.replace(dir_len, std::string::npos, name);
    if (ext) dir.append(ext);
}

// split in.[bc|ll] -prefix=osl_llvm_compiled_ops_ -o basename
// split in.[bc|ll] -func=osl_ -func=OSL:: -prefix=osl_llvm_compiled_ops_ -o basename
// -> basename.h, basename.cpp

int main(int argc, const char **argv)
{
#if OSL_LLVM_VERSION <= 34
    sys::PrintStackTraceOnErrorSignal();
#else
    sys::PrintStackTraceOnErrorSignal(argv[0]);
#endif

    PrettyStackTraceProgram pstack(argc, argv);
    LLVMContext context;

    llvm_shutdown_obj clean; // Call llvm_shutdown() on exit.

#if OSL_LLVM_VERSION >= 35
    cl::HideUnrelatedOptions(s_category);
#endif
    cl::ParseCommandLineOptions(argc, argv, "extract bitcode to c++\n");

    SMDiagnostic sm_err;
    std::unique_ptr<Module> module = loadModule(context, argv);
    if (!module)
        return EXIT_FAILURE;

    // Stack storage for dumping the IR
    llvm::SmallVector<char, 4096*2> buf;

    const bool splitting = !s_prefix_list.empty();
    HexDumper hex_dump(s_output_name, s_prefix_name, splitting);

    // Save a list of function names that will be saved
    if (splitting) {
        std::string dir;
        if (s_dumpir_file) {
            dir = s_output_name;
            std::string err;
            std_error_code ec = sys::fs::create_directory(dir.c_str());
            if (error_string(std::move(ec), &err)) {
                llvm::errs() << "Could not create '" << dir << "': " << err << '\n';
                return EXIT_FAILURE;
            }

            // Append directory symbol, and get new length
            dir += "/";
        }
        // Write to dir in place later
        const size_t dir_len = dir.size();

        std::vector<GlobalValue*> inlines;
        std::set<std::string> funcs;
        for (Function &F : *module) {
            if (F.isDeclaration())
                continue;

            // Match the function against the user prefixes.
            std::string demangled;
            llvm::StringRef name = F.getName();
            bool found = should_save_function(name);
            if (!found && Demangle(name, demangled))
                found = should_save_function(demangled);

            if (F.hasFnAttribute(Attribute::InlineHint) ||
                F.hasFnAttribute(Attribute::AlwaysInline)) {
                // Inlined functions all get put into a separate block that is
                // loaded as the base module. Everything else is merged into it.
                //
                // TODO: Inspect other functions' usage of any of these
                // inlines and extend the bytecode_func structure with a flag
                // noting that inlines are required.
                inlines.push_back(cast<GlobalValue>(&F));
            }
            else if (found)
                funcs.insert(name);
        }

        if (!inlines.empty()) {
            if (!dump_module(*module, s_prefix_name, buf, dir, &inlines))
                return EXIT_FAILURE;
            hex_dump(buf, "");
            
            if (dir_len) {
                std::string err;
                file_path(dir, dir_len, "inlines", ".ll");
                std::unique_ptr<raw_fd_ostream> outs(file_stream(dir, err));
                if (!err.empty()) {
                    llvm::errs() << "Could not create '" << dir << "': " << err << '\n';
                    return EXIT_FAILURE;
                }
                for (GlobalValue *GV : inlines)
                    cast<Function>(GV)->print(*outs);
            }
            module.reset();
        }
        // else compile time error when buildin llvm_utils.cpp!

        for (const auto &name : funcs) {
            if (!module) {
                module = loadModule(context, argv);
                if (!module)
                    return EXIT_FAILURE;
            }
            if (dir_len)
                file_path(dir, dir_len, name, ".ll");
            
            if (!dump_module(*module, name, buf, dir))
                return EXIT_FAILURE;

            // Dump the hex into c-array and size variables.
            hex_dump(buf, name);
            module.reset();
        }
        // Write the c++ code to initialize and retrieve from the map.
        hex_dump(funcs);
    } else {
        // Monolithic c-array and size variable.
        if (!dump_module(*module, buf))
            return EXIT_FAILURE;
        hex_dump(buf, "");
    }

    return EXIT_SUCCESS;
}
