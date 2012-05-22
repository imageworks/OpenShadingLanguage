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

#ifndef OSLEXEC_PVT_H
#define OSLEXEC_PVT_H

#include <string>
#include <vector>
#include <stack>
#include <map>
#include <list>
#include <set>

#include <boost/regex_fwd.hpp>

#include "OpenImageIO/hash.h"
#include "OpenImageIO/ustring.h"
#include "OpenImageIO/thread.h"
#include "OpenImageIO/paramlist.h"

#include "oslexec.h"
#include "oslclosure.h"
#include "osl_pvt.h"
#include "constantpool.h"


using namespace OSL;
using namespace OSL::pvt;

namespace llvm {
  class ExecutionEngine;
  class Function;
  class FunctionPassManager;
  class LLVMContext;
  class Linker;
  class Module;
  class PassManager;
  class JITMemoryManager;
}

using OIIO::atomic_int;
using OIIO::atomic_ll;
using OIIO::RefCnt;
using OIIO::ParamValueList;
using OIIO::mutex;
using OIIO::lock_guard;
using OIIO::spin_mutex;
using OIIO::spin_lock;
using OIIO::thread_specific_ptr;
using OIIO::ustringHash;
namespace Strutil = OIIO::Strutil;


OSL_NAMESPACE_ENTER



struct PerThreadInfo
{
    PerThreadInfo ();
    ~PerThreadInfo ();
    ShadingContext *pop_context ();  ///< Get the pool top and then pop

    std::stack<ShadingContext *> context_pool;
    llvm::LLVMContext *llvm_context;
    llvm::JITMemoryManager *llvm_jitmm;
};



namespace pvt {

// forward definitions
class ShadingSystemImpl;
class ShaderInstance;
typedef shared_ptr<ShaderInstance> ShaderInstanceRef;
class Dictionary;
class RuntimeOptimizer;


/// Signature of the function that LLVM generates to run the shader
/// group.
typedef void (*RunLLVMGroupFunc)(void* /* shader globals */, void*); 

/// Signature of a constant-folding method
typedef int (*OpFolder) (RuntimeOptimizer &rop, int opnum);

/// Signature of an LLVM-IR-generating method
typedef bool (*OpLLVMGen) (RuntimeOptimizer &rop, int opnum);

struct OpDescriptor {
    ustring name;           // name of op
    OpLLVMGen llvmgen;      // llvm-generating routine
    OpFolder folder;        // constant-folding routine
    bool simple_assign;     // wholy overwites arg0, no other writes,
                            //     no side effects
    OpDescriptor () { }
    OpDescriptor (const char *n, OpLLVMGen ll, OpFolder f=NULL,
                  bool simple=false)
        : name(n), llvmgen(ll), folder(f), simple_assign(simple)
    {}
};





// Prefix for OSL shade up declarations, so LLVM can find them
#define OSL_SHADEOP extern "C" OSLEXECPUBLIC



/// Like an int (of type T), but also internally keeps track of the 
/// maximum value is has held, and the total "requested" deltas.
/// You really shouldn't use an unsigned type for T, for two reasons:
/// (1) Our implementation of '-=' will fail; and (2) you actually
/// want to allow the counter to go negative, to detect if you have
/// made a mistake in your bookkeeping by forgetting an allocation.
template<typename T>
class PeakCounter
{
public:
    typedef T value_t;
    PeakCounter () : m_current(0), m_requested(0), m_peak(0) { }
    /// Reset all counts to zero.
    ///
    void clear () {
        m_current = 0;  m_requested = 0;  m_peak = 0;
    }
    /// Return the current value.
    ///
    value_t operator() () const { return m_current; }

    /// Return the current value.
    ///
    value_t current (void) const { return m_current; }
    /// Return the sum of all requests.
    ///
    value_t requested (void) const { return m_requested; }
    /// Return the peak value we saw.
    ///
    value_t peak (void) const { return m_peak; }

    /// Reassign the current value, adjust peak and requested as necessary.
    ///
    const value_t operator= (value_t newval) {
        value_t cur = m_current;
        if (newval > cur)
            m_requested += (cur-newval);
        m_current = newval;
        if (newval > m_peak)
            m_peak = newval;
        return m_current;
    }
    /// Add to current value, adjust peak and requested as necessary.
    ///
    const value_t operator+= (value_t sz) {
        m_current += sz;
        if (sz > 0) {
            m_requested += sz;
            if (m_current > m_peak)
                m_peak = m_current;
        }
        return m_current;
    }
    /// Subtract from current value
    ///
    const value_t operator-= (value_t sz) {
        *this += (-sz);
        return m_current;
    }

    const value_t operator++ ()    { *this += 1;  return m_current; }
    const value_t operator++ (int) { *this += 1;  return m_current-1; }
    const value_t operator-- ()    { *this -= 1;  return m_current; }
    const value_t operator-- (int) { *this -= 1;  return m_current+1; }

    friend std::ostream & operator<< (std::ostream &out, const PeakCounter &p)
    {
        out << p.requested() << " requested, " << p.peak() << " peak, "
            << p.current() << " current";
        return out;
    }

    std::string memstat () const {
        return Strutil::memformat(requested()) + " requested, "
             + Strutil::memformat(peak()) + " peak, "
             + Strutil::memformat(current()) + " current";
    }

private:
    value_t m_current, m_requested, m_peak;
};



/// Template to count a vector's allocated size, in bytes.
///
template<class T>
inline off_t vectorbytes (const std::vector<T> &v)
{
    return v.capacity() * sizeof(T);
}


/// Template to fully deallocate a stl container using the swap trick.
///
template<class T>
inline void stlfree (T &v)
{
    T tmp;
    std::swap (tmp, v);
    // Now v is no allocated space, and tmp has v's old allocated space.
    // When tmp leaves scope as we return, that space will be freed.
}




/// ShaderMaster is the full internal representation of a complete
/// shader that would be a .oso file on disk: symbols, instructions,
/// arguments, you name it.  A master copy is shared by all the 
/// individual instances of the shader.
class ShaderMaster : public RefCnt {
public:
    typedef intrusive_ptr<ShaderMaster> ref;
    ShaderMaster (ShadingSystemImpl &shadingsys) : m_shadingsys(shadingsys) { }
    ~ShaderMaster ();

    std::string print ();  // Debugging

    /// Return a pointer to the shading system for this master.
    ///
    ShadingSystemImpl & shadingsys () const { return m_shadingsys; }

    /// Run through the symbols and set up various things we can know
    /// with just the master: the size (including padding), and their
    /// data pointers if they are constants or params (to the defaults).
    /// As a side effect, also set this->m_firstparam/m_lastparam.
    void resolve_syms ();

    /// Find the named symbol, return its index in the symbol array, or
    /// -1 if not found.
    int findsymbol (ustring name) const;

    /// Return a pointer to the symbol (specified by integer index),
    /// or NULL (if index was -1, as returned by 'findsymbol').
    Symbol *symbol (int index) { return index >= 0 ? &m_symbols[index] : NULL; }

    /// Return the name of the shader.
    ///
    const std::string &shadername () const { return m_shadername; }

private:
    ShadingSystemImpl &m_shadingsys;    ///< Back-ptr to the shading system
    ShaderType m_shadertype;            ///< Type of shader
    std::string m_shadername;           ///< Shader name
    std::string m_osofilename;          ///< Full path of oso file
    OpcodeVec m_ops;                    ///< Actual code instructions
    std::vector<int> m_args;            ///< Arguments for all the ops
    // Need the code offsets for each code block
    SymbolVec m_symbols;                ///< Symbols used by the shader
    std::vector<int> m_idefaults;       ///< int default param values
    std::vector<float> m_fdefaults;     ///< float default param values
    std::vector<ustring> m_sdefaults;   ///< string default param values
    std::vector<int> m_iconsts;         ///< int constant values
    std::vector<float> m_fconsts;       ///< float constant values
    std::vector<ustring> m_sconsts;     ///< string constant values
    int m_firstparam, m_lastparam;      ///< Subset of symbols that are params
    int m_maincodebegin, m_maincodeend; ///< Main shader code range

    friend class OSOReaderToMaster;
    friend class ShaderInstance;
};



/// Describe one end of a parameter connetion: the parameter number, and
/// optinally an array index and/or channel number within that parameter.
struct ConnectedParam {
    int param;            ///< Parameter number (in the symbol table)
    int arrayindex;       ///< Array index (-1 for not an index)
    int channel;          ///< Channel number (-1 for no channel selection)
    int offset;           ///< Offset into the data of the element/channel
    TypeSpec type;        ///< Type of data being connected

    ConnectedParam () : param(-1), arrayindex(-1), channel(-1), offset(0) { }

    bool valid () const { return (param >= 0); }
};



/// Describe a parameter connection to an earlier layer.
///
struct Connection {
    int srclayer;          ///< Layer (within our group) of the source
    ConnectedParam src;    ///< Which source parameter (or part thereof)
    ConnectedParam dst;    ///< Which destination parameter (or part thereof)

    Connection (int srclay, const ConnectedParam &srccon,
                const ConnectedParam &dstcon)
        : srclayer (srclay), src (srccon), dst (dstcon)
    { }
};



typedef std::vector<Connection> ConnectionVec;



/// ShaderInstance is a particular instance of a shader, with its own
/// set of parameter values, coordinate transform, and connections to
/// other instances within the same shader group.
class ShaderInstance {
public:
    typedef ShaderInstanceRef ref;
    ShaderInstance (ShaderMaster::ref master, const char *layername="");
    ~ShaderInstance ();

    /// Return the layer name of this instance
    ///
    ustring layername () const { return m_layername; }

    /// Return the name of the shader used by this instance.
    ///
    const std::string &shadername () const { return m_master->shadername(); }

    /// Return a pointer to the master for this instance.
    ///
    ShaderMaster *master () const { return m_master.get(); }

    /// Return a reference to the shading system for this instance.
    ///
    ShadingSystemImpl & shadingsys () const { return m_master->shadingsys(); }

    /// Apply pending parameters
    /// 
    void parameters (const ParamValueList &params);

    /// Find the named symbol, return its index in the symbol array, or
    /// -1 if not found.
    int findsymbol (ustring name) const;

    /// Find the named parameter, return its index in the symbol array, or
    /// -1 if not found.
    int findparam (ustring name) const;

    /// Return a pointer to the symbol (specified by integer index),
    /// or NULL (if index was -1, as returned by 'findsymbol').
    Symbol *symbol (int index) { return index >= 0 ? &m_instsymbols[index] : NULL; }
    const Symbol *symbol (int index) const { return index >= 0 ? &m_instsymbols[index] : NULL; }

    /// Estimate how much to round the required heap size up if npoints
    /// is odd, to account for getting the desired alignment for each
    /// symbol.
    size_t heapround ();

    /// Add a connection
    ///
    void add_connection (int srclayer, const ConnectedParam &srccon,
                         const ConnectedParam &dstcon);

    /// How many connections to earlier layers do we have?
    ///
    int nconnections () const { return (int) m_connections.size (); }

    /// Return a reference to the i-th connection to an earlier layer.
    ///
    const Connection & connection (int i) const { return m_connections[i]; }

    /// Reference to the connection list.
    ///
    ConnectionVec & connections () { return m_connections; }
    const ConnectionVec & connections () const { return m_connections; }

    /// Return the unique ID of this instance.
    ///
    int id () const { return m_id; }

    /// Does this instance potentially write to any global vars?
    ///
    bool writes_globals () const { return m_writes_globals; }

    /// Should this instance only be run lazily (i.e., not
    /// unconditionally)?
    bool run_lazily () const { return m_run_lazily; }
    void run_lazily (bool lazy) { m_run_lazily = lazy; }

    /// Does this instance have any outgoing connections?
    ///
    bool outgoing_connections () const { return m_outgoing_connections; }
    /// Set whether this instance has outgoing connections.
    ///
    void outgoing_connections (bool out) { m_outgoing_connections = out; }

    int maincodebegin () const { return m_maincodebegin; }
    int maincodeend () const { return m_maincodeend; }

    int firstparam () const { return m_firstparam; }
    int lastparam () const { return m_lastparam; }

    /// Return a begin/end Symbol* pair for the set of param symbols
    /// that is suitable to pass as a range for BOOST_FOREACH.
    friend std::pair<Symbol *,Symbol *> param_range (ShaderInstance *i) {
        return std::pair<Symbol*,Symbol*> (i->symbol(i->firstparam()),
                                           i->symbol(i->lastparam()));
    }

    friend std::pair<const Symbol *,const Symbol *> param_range (const ShaderInstance *i) {
        return std::pair<const Symbol*,const Symbol*> (i->symbol(i->firstparam()),
                                                       i->symbol(i->lastparam()));
    }

    int Psym () const { return m_Psym; }
    int Nsym () const { return m_Nsym; }


    const std::vector<int> & args () const { return m_instargs; }
    std::vector<int> & args () { return m_instargs; }
    int arg (int argnum) { return args()[argnum]; }
    Symbol *argsymbol (int argnum) { return symbol(arg(argnum)); }
    const OpcodeVec & ops () const { return m_instops; }
    OpcodeVec & ops () { return m_instops; }

    std::string print ();  // Debugging

    SymbolVec &symbols () { return m_instsymbols; }
    const SymbolVec &symbols () const { return m_instsymbols; }

    /// Make sure there's room for more symbols.
    ///
    void make_symbol_room (size_t moresyms=1);

    /// Does it appear that the layer is completely unused?
    ///
    bool unused () const { return run_lazily() && ! outgoing_connections(); }

    /// Make our own version of the code and args from the master.
    ///
    void copy_code_from_master ();

private:
    ShaderMaster::ref m_master;         ///< Reference to the master
    SymbolVec m_instsymbols;            ///< Symbols used by the instance
    OpcodeVec m_instops;                ///< Actual code instructions
    std::vector<int> m_instargs;        ///< Arguments for all the ops
    ustring m_layername;                ///< Name of this layer
    std::vector<int> m_iparams;         ///< int param values
    std::vector<float> m_fparams;       ///< float param values
    std::vector<ustring> m_sparams;     ///< string param values
    int m_id;                           ///< Unique ID for the instance
    bool m_writes_globals;              ///< Do I have side effects?
    bool m_run_lazily;                  ///< OK to run this layer lazily?
    bool m_outgoing_connections;        ///< Any outgoing connections?
    std::vector<Connection> m_connections; ///< Connected input params
    int m_firstparam, m_lastparam;      ///< Subset of symbols that are params
    int m_maincodebegin, m_maincodeend; ///< Main shader code range
    int m_Psym, m_Nsym;                 ///< Quick lookups of common syms

    friend class ShadingSystemImpl;
    friend class RuntimeOptimizer;
};



/// Macro to loop over just the params & output params of an instance,
/// with each iteration providing a Symbol& to symbolref.  Use like this:
///        FOREACH_PARAM (Symbol &s, inst) { ... stuff with s... }
///
#define FOREACH_PARAM(symboldecl,inst) \
    BOOST_FOREACH (symboldecl, param_range(inst))



/// A ShaderGroup consists of one or more layers (each of which is a
/// ShaderInstance), and the connections among them.
class ShaderGroup {
public:
    ShaderGroup ();
    ShaderGroup (const ShaderGroup &g);
    ~ShaderGroup ();

    /// Clear the layers
    ///
    void clear () { m_layers.clear ();  m_optimized = 0;  m_executions = 0; }

    /// Append a new shader instance on to the end of this group
    ///
    void append (ShaderInstanceRef newlayer) {
        ASSERT (! m_optimized && "should not append to optimized group");
        m_layers.push_back (newlayer);
    }

    /// How many layers are in this group?
    ///
    int nlayers () const { return (int) m_layers.size(); }

    /// Array indexing returns the i-th layer of the group
    ///
    ShaderInstance * operator[] (int i) const { return m_layers[i].get(); }

    int optimized () const { return m_optimized; }
    void optimized (int opt) { m_optimized = opt; }

    size_t llvm_groupdata_size () const { return m_llvm_groupdata_size; }
    void llvm_groupdata_size (size_t size) { m_llvm_groupdata_size = size; }

    RunLLVMGroupFunc llvm_compiled_version() const {
        return m_llvm_compiled_version;
    }
    void llvm_compiled_version (RunLLVMGroupFunc func) {
        m_llvm_compiled_version = func;
    }

    /// Is this shader group equivalent to ret void?
    bool does_nothing() const {
        return m_does_nothing;
    }
    void does_nothing(bool new_val) {
        m_does_nothing = new_val;
    }

    long long int executions () const { return m_executions; }

    void start_running () {
#ifdef DEBUG
       m_executions++;
#endif
    }

    void name (ustring name) { m_name = name; }
    ustring name () const { return m_name; }

private:
    ustring m_name;
    std::vector<ShaderInstanceRef> m_layers;
    RunLLVMGroupFunc m_llvm_compiled_version;
    size_t m_llvm_groupdata_size;
    volatile int m_optimized;        ///< Is it already optimized?
    bool m_does_nothing;             ///< Is the shading group just func() { return; }
    atomic_ll m_executions;          ///< Number of times the group executed
    mutex m_mutex;                   ///< Thread-safe optimization
    friend class ShadingSystemImpl;
};



class ClosureRegistry {
public:

    struct ClosureEntry {
        // normally a closure is fully identified by its
        // name, but we might want to have an internal id
        // for fast dispatching
        int                       id;
        // The name again
        ustring                   name;
        // Number of formal arguments
        int                       nformal;
        // Number of keyword arguments
        int                       nkeyword;
        // The parameters
        std::vector<ClosureParam> params;
        // the needed size for the structure
        int                       struct_size;
        // Creation callbacks
        PrepareClosureFunc        prepare;
        SetupClosureFunc          setup;
        CompareClosureFunc        compare;
    };

    void register_closure (const char *name, int id, const ClosureParam *params,
                           PrepareClosureFunc prepare, SetupClosureFunc setup, CompareClosureFunc compare);

    const ClosureEntry *get_entry (ustring name) const;
    const ClosureEntry *get_entry (int id) const {
        DASSERT((size_t)id < m_closure_table.size());
        return &m_closure_table[id];
    }

    bool empty () const { return m_closure_table.empty(); }

private:


    // A mapping from name to ID for the compiler
    std::map<ustring, int>    m_closure_name_to_id;
    // And the internal global table, indexed
    // by the internal ID for fast dispatching
    std::vector<ClosureEntry> m_closure_table;
};



class OSLEXECPUBLIC ShadingSystemImpl : public ShadingSystem
{
public:
    OSLEXECPUBLIC ShadingSystemImpl (RendererServices *renderer=NULL,
                       TextureSystem *texturesystem=NULL,
                       ErrorHandler *err=NULL);
    virtual ~ShadingSystemImpl ();

    virtual bool attribute (const std::string &name, TypeDesc type, const void *val);
    virtual bool getattribute (const std::string &name, TypeDesc type, void *val);

    virtual bool Parameter (const char *name, TypeDesc t, const void *val);
    virtual bool Shader (const char *shaderusage,
                         const char *shadername=NULL,
                         const char *layername=NULL);
    virtual bool ShaderGroupBegin (const char *groupname=NULL);
    virtual bool ShaderGroupEnd (void);
    virtual bool ConnectShaders (const char *srclayer, const char *srcparam,
                                 const char *dstlayer, const char *dstparam);
    virtual ShadingAttribStateRef state ();
    virtual void clear_state ();

//    virtual void RunShaders (ShadingAttribStateRef &attribstate,
//                             ShaderUse use);

    /// Internal error reporting routine, with printf-like arguments.
    ///
    void error (const char *message, ...);
    /// Internal warning reporting routine, with printf-like arguments.
    ///
    void warning (const char *message, ...);
    /// Internal info printing routine, with printf-like arguments.
    ///
    void info (const char *message, ...);
    /// Internal message printing routine, with printf-like arguments.
    ///
    void message (const char *message, ...);

    /// Error reporting routines that take a pre-formatted string only.
    ///
    void error (const std::string &message);
    void warning (const std::string &message);
    void info (const std::string &message);
    void message (const std::string &message);

    virtual std::string getstats (int level=1) const;

    ErrorHandler &errhandler () const { return *m_err; }

    ShaderMaster::ref loadshader (const char *name);

    virtual PerThreadInfo * create_thread_info();

    virtual void destroy_thread_info (PerThreadInfo *threadinfo);

    virtual ShadingContext *get_context (PerThreadInfo *threadinfo = NULL);

    virtual void release_context (ShadingContext *ctx);

    virtual bool execute (ShadingContext &ctx, ShadingAttribState &sas,
                          ShaderGlobals &ssg, bool run=true);

    virtual const void* get_symbol (ShadingContext &ctx, ustring name,
                                    TypeDesc &type);

    void operator delete (void *todel) { ::delete ((char *)todel); }

    /// Is the shading system in debug mode, and if so, how verbose?
    ///
    int debug () const { return m_debug; }

    /// Return a pointer to the renderer services object.
    ///
    RendererServices *renderer () const { return m_renderer; }

    /// Return a pointer to the texture system.
    ///
    TextureSystem *texturesys () const { return m_texturesys; }

    bool allow_rebind () const { return m_rebind; }

    bool debug_nan () const { return m_debugnan; }
    bool lockgeom_default () const { return m_lockgeom_default; }
    bool strict_messages() const { return m_strict_messages; }
    bool range_checking() const { return m_range_checking; }
    bool unknown_coordsys_error() const { return m_unknown_coordsys_error; }
    int optimize () const { return m_optimize; }
    int llvm_optimize () const { return m_llvm_optimize; }
    int llvm_debug () const { return m_llvm_debug; }

    ustring commonspace_synonym () const { return m_commonspace_synonym; }

    /// The group is set and won't be changed again; take advantage of
    /// this by optimizing the code knowing all our instance parameters
    /// (at least the ones that can't be overridden by the geometry).
    void optimize_group (ShadingAttribState &attribstate, ShaderGroup &group);

    int *alloc_int_constants (size_t n) { return m_int_pool.alloc (n); }
    float *alloc_float_constants (size_t n) { return m_float_pool.alloc (n); }
    ustring *alloc_string_constants (size_t n) { return m_string_pool.alloc (n); }

    virtual void register_closure(const char *name, int id, const ClosureParam *params,
                                  PrepareClosureFunc prepare, SetupClosureFunc setup, CompareClosureFunc compare);
    const ClosureRegistry::ClosureEntry *find_closure(ustring name) const {
        return m_closure_registry.get_entry(name);
    }
    const ClosureRegistry::ClosureEntry *find_closure(int id) const {
        return m_closure_registry.get_entry(id);
    }

    /// Convert a color in the named space to RGB.
    ///
    Color3 to_rgb (ustring fromspace, float a, float b, float c);

    /// Convert an XYZ color to RGB in our preferred color space.
    Color3 XYZ_to_RGB (const Color3 &XYZ) { return XYZ * m_XYZ2RGB; }
    Color3 XYZ_to_RGB (float X, float Y, float Z) { return Color3(X,Y,Z) * m_XYZ2RGB; }
    /// Convert an RGB color in our preferred color space to XYZ.
    Color3 RGB_to_XYZ (const Color3 &RGB) { return RGB * m_RGB2XYZ; }
    Color3 RGB_to_XYZ (float R, float G, float B) { return Color3(R,G,B) * m_RGB2XYZ; }

    /// Return the luminance of an RGB color in the current color space.
    float luminance (const Color3 &RGB) { return RGB.dot(m_luminance_scale); }

    /// Return the RGB in the current color space for blackbody radiation
    /// at temperature T (in Kelvin).
    Color3 blackbody_rgb (float T /*Kelvin*/);

    /// Set the current color space.
    bool set_colorspace (ustring colorspace);

    virtual int raytype_bit (ustring name);

    virtual void optimize_all_groups (int nthreads=0);

#ifdef OIIO_HAVE_BOOST_UNORDERED_MAP
    typedef boost::unordered_map<ustring,OpDescriptor,ustringHash> OpDescriptorMap;
#else
    typedef hash_map<ustring,OpDescriptor,ustringHash> OpDescriptorMap;
#endif

    /// Look up OpDescriptor for the named op, return NULL for unknown op.
    ///
    const OpDescriptor *op_descriptor (ustring opname) {
        OpDescriptorMap::const_iterator i = m_op_descriptor.find (opname);
        if (i != m_op_descriptor.end())
            return &(i->second);
        else
            return NULL;
    }

    void pointcloud_stats (int search, int get, int results);

private:
    void printstats () const;

    /// Find the index of the named layer in the current shader group.
    /// If found, return the index >= 0 and put a pointer to the instance
    /// in inst; if not found, return -1 and set inst to NULL.
    /// (This is a helper for ConnectShaders.)
    int find_named_layer_in_group (ustring layername, ShaderInstance * &inst);

    /// Turn a connectionname (such as "Kd" or "Cout[1]", etc.) into a
    /// ConnectedParam descriptor.  This routine is strictly a helper for
    /// ConnectShaders, and will issue error messages on its behalf.
    /// The return value will not be valid() if there is an error.
    ConnectedParam decode_connected_param (const char *connectionname,
                               const char *layername, ShaderInstance *inst);

    /// Get the per-thread info, create it if necessary.
    ///
    PerThreadInfo *get_perthread_info () const {
        PerThreadInfo *p = m_perthread_info.get ();
        if (! p) {
            p = new PerThreadInfo;
            m_perthread_info.reset (p);
        }
        return p;
    }

    /// Set up LLVM -- make sure we have a Context, Module, ExecutionEngine,
    /// retained JITMemoryManager, etc.
    void SetupLLVM ();

    void setup_op_descriptors ();

    RendererServices *m_renderer;         ///< Renderer services
    TextureSystem *m_texturesys;          ///< Texture system

    ErrorHandler *m_err;                  ///< Error handler
    std::list<std::string> m_errseen, m_warnseen;
    static const int m_errseenmax = 32;
    mutable mutex m_errmutex;

    typedef std::map<ustring,ShaderMaster::ref> ShaderNameMap;
    ShaderNameMap m_shader_masters;       ///< name -> shader masters map

    ConstantPool<int> m_int_pool;
    ConstantPool<Float> m_float_pool;
    ConstantPool<ustring> m_string_pool;

    OpDescriptorMap m_op_descriptor;

    // Options
    int m_statslevel;                     ///< Statistics level
    bool m_lazylayers;                    ///< Evaluate layers on demand?
    bool m_lazyglobals;                   ///< Run lazily even if globals write?
    bool m_clearmemory;                   ///< Zero mem before running shader?
    bool m_rebind;                        ///< Allow rebinding?
    bool m_debugnan;                      ///< Root out NaN's?
    bool m_lockgeom_default;              ///< Default value of lockgeom
    bool m_strict_messages;               ///< Strict checking of message passing usage?
    bool m_range_checking;                ///< Range check arrays & components?
    bool m_unknown_coordsys_error;        ///< Error to use unknown xform name?
    bool m_greedyjit;                     ///< JIT as much as we can?
    int m_optimize;                       ///< Runtime optimization level
    bool m_opt_constant_param;            ///< Turn instance params into const?
    bool m_opt_constant_fold;             ///< Allow constant folding?
    bool m_opt_stale_assign;              ///< Optimize stale assignments?
    bool m_opt_elide_useless_ops;         ///< Optimize away useless ops?
    bool m_opt_elide_unconnected_outputs; ///< Elide unconnected outputs?
    bool m_opt_peephole;                  ///< Do some peephole optimizations?
    bool m_opt_coalesce_temps;            ///< Coalesce temporary variables?
    bool m_opt_assign;                    ///< Do various assign optimizations?
    bool m_optimize_nondebug;             ///< Fully optimize non-debug!
    int m_llvm_optimize;                  ///< OSL optimization strategy
    int m_debug;                          ///< Debugging output
    int m_llvm_debug;                     ///< More LLVM debugging output
    ustring m_debug_groupname;            ///< Name of sole group to debug
    ustring m_debug_layername;            ///< Name of sole layer to debug
    ustring m_only_groupname;             ///< Name of sole group to compile
    std::string m_searchpath;             ///< Shader search path
    std::vector<std::string> m_searchpath_dirs; ///< All searchpath dirs
    ustring m_commonspace_synonym;        ///< Synonym for "common" space
    std::vector<ustring> m_raytypes;      ///< Names of ray types
    ustring m_colorspace;                 ///< What RGB colors mean
    int m_max_local_mem_KB;               ///< Local storage can a shader use

    // Derived/cached calculations from options:
    Color3 m_Red, m_Green, m_Blue;        ///< Color primaries (xyY)
    Color3 m_White;                       ///< White point (xyY)
    Matrix33 m_XYZ2RGB;                   ///< XYZ to RGB conversion matrix
    Matrix33 m_RGB2XYZ;                   ///< RGB to XYZ conversion matrix
    Color3 m_luminance_scale;             ///< Scaling for RGB->luma
    std::vector<Color3> m_blackbody_table; ///< Precomputed blackbody table

    // State
    bool m_in_group;                      ///< Are we specifying a group?
    ShaderUse m_group_use;                ///< Use of group
    ustring m_group_name;                 ///< Name of group
    ParamValueList m_pending_params;      ///< Pending Parameter() values
    ShadingAttribStateRef m_curattrib;    ///< Current shading attribute state
    mutable mutex m_mutex;                ///< Thread safety
    mutable thread_specific_ptr<PerThreadInfo> m_perthread_info;

    // Stats
    atomic_int m_stat_shaders_loaded;     ///< Stat: shaders loaded
    atomic_int m_stat_shaders_requested;  ///< Stat: shaders requested
    PeakCounter<int> m_stat_instances;    ///< Stat: instances
    PeakCounter<int> m_stat_contexts;     ///< Stat: shading contexts
    int m_stat_groups;                    ///< Stat: shading groups
    int m_stat_groupinstances;            ///< Stat: total inst in all groups
    atomic_int m_stat_instances_compiled; ///< Stat: instances compiled
    atomic_int m_stat_groups_compiled;    ///< Stat: groups compiled
    atomic_int m_stat_empty_instances;    ///< Stat: shaders empty after opt
    atomic_int m_stat_empty_groups;       ///< Stat: groups empty after opt
    atomic_int m_stat_regexes;            ///< Stat: how many regex's compiled
    atomic_int m_stat_preopt_syms;        ///< Stat: pre-optimization symbols
    atomic_int m_stat_postopt_syms;       ///< Stat: post-optimization symbols
    atomic_int m_stat_preopt_ops;         ///< Stat: pre-optimization ops
    atomic_int m_stat_postopt_ops;        ///< Stat: post-optimization ops
    double m_stat_optimization_time;      ///< Stat: time spent optimizing
    double m_stat_opt_locking_time;       ///<   locking time
    double m_stat_specialization_time;    ///<   runtime specialization time
    double m_stat_total_llvm_time;        ///<   total time spent on LLVM
    double m_stat_llvm_setup_time;        ///<     llvm setup time
    double m_stat_llvm_irgen_time;        ///<     llvm IR generation time
    double m_stat_llvm_opt_time;          ///<     llvm IR optimization time
    double m_stat_llvm_jit_time;          ///<     llvm JIT time 
    double m_stat_getattribute_time;      ///< Stat: time spent in getattribute
    double m_stat_getattribute_fail_time; ///< Stat: time spent in getattribute
    atomic_ll m_stat_getattribute_calls;  ///< Stat: Number of getattribute
    long long m_stat_pointcloud_searches;
    long long m_stat_pointcloud_searches_total_results;
    int m_stat_pointcloud_max_results;
    int m_stat_pointcloud_failures;
    long long m_stat_pointcloud_gets;

    int m_stat_max_llvm_local_mem;        ///< Stat: max LLVM local mem
    PeakCounter<off_t> m_stat_memory;     ///< Stat: all shading system memory

    PeakCounter<off_t> m_stat_mem_master; ///< Stat: master-related mem
    PeakCounter<off_t> m_stat_mem_master_ops;
    PeakCounter<off_t> m_stat_mem_master_args;
    PeakCounter<off_t> m_stat_mem_master_syms;
    PeakCounter<off_t> m_stat_mem_master_defaults;
    PeakCounter<off_t> m_stat_mem_master_consts;
    PeakCounter<off_t> m_stat_mem_inst;   ///< Stat: instance-related mem
    PeakCounter<off_t> m_stat_mem_inst_syms;
    PeakCounter<off_t> m_stat_mem_inst_paramvals;
    PeakCounter<off_t> m_stat_mem_inst_connections;

    spin_mutex m_stat_mutex;              ///< Mutex for non-atomic stats
    ClosureRegistry m_closure_registry;
    std::vector<ShadingAttribStateRef> m_groups_to_compile;
    atomic_int m_groups_to_compile_count;
    atomic_int m_threads_currently_compiling;
    spin_mutex m_groups_to_compile_mutex;

    // LLVM stuff
    spin_mutex m_llvm_mutex;
    // Can't throw away jitmm's until we're totally done
    std::vector<shared_ptr<llvm::JITMemoryManager> > m_llvm_jitmm_hold;

    friend class OSL::ShadingContext;
    friend class ShaderMaster;
    friend class ShaderInstance;
    friend class RuntimeOptimizer;
};



template<int BlockSize>
class SimplePool {
public:
    SimplePool() {
        m_blocks.push_back(new char[BlockSize]);
        m_block_offset = BlockSize;
        m_current_block = 0;
    }

    ~SimplePool() {
        for (size_t i =0; i < m_blocks.size(); ++i)
            delete [] m_blocks[i];
    }

    char * alloc(size_t size) {
        ASSERT(size < BlockSize);
        if (size <= m_block_offset) {
            m_block_offset -= size;
        } else {
            m_current_block++;
            m_block_offset = BlockSize - size;
            if (m_blocks.size() == m_current_block)
                m_blocks.push_back(new char[BlockSize]);
        }
        return m_blocks[m_current_block] + m_block_offset;
    }

    void clear () { m_current_block = 0; m_block_offset = BlockSize; }

private:
    std::vector<char *> m_blocks;
    size_t              m_current_block;
    size_t              m_block_offset;
};

/// Represents a single message for use by getmessage and setmessage opcodes
///
struct Message {
    Message(ustring name, const TypeDesc& type, int layeridx, ustring sourcefile, int sourceline, Message* next) :
       name(name), data(NULL), type(type), layeridx(layeridx), sourcefile(sourcefile), sourceline(sourceline), next(next) {}

    /// Some messages don't have data because getmessage() was called before setmessage
    /// (which is flagged as an error to avoid ambiguities caused by execution order)
    ///
    bool has_data() const { return data != NULL; }

    ustring name;           ///< name of this message
    char* data;             ///< actual data of the message (will never change once the message is created)
    TypeDesc type;          ///< what kind of data is stored here? FIXME: should be TypeSpec
    int layeridx;           ///< layer index where this was message was created
    ustring sourcefile;     ///< source code file that contains the call that created this message
    int sourceline;         ///< source code line that contains the call that created this message
    Message* next;          ///< linked list of messages (managed by MessageList below)
};

/// Represents the list of messages set by a given shader using setmessage and getmessage
///
struct MessageList {
     MessageList() : list_head(NULL), message_data() {}

     void clear() {
         list_head = NULL;
         message_data.clear();
     }

    const Message* find(ustring name) const {
        for (const Message* m = list_head; m != NULL; m = m->next)
            if (m->name == name)
                return m; // name matches
        return NULL; // not found
    }

    void add(ustring name, void* data, const TypeDesc& type, int layeridx, ustring sourcefile, int sourceline) {
        list_head = new (message_data.alloc(sizeof(Message))) Message(name, type, layeridx, sourcefile, sourceline, list_head);
        if (data) {
            list_head->data = message_data.alloc(type.size());
            memcpy(list_head->data, data, type.size());
        }
    }

private:
    Message*         list_head;
    SimplePool<1024> message_data;
};


}; // namespace pvt



/// The full context for executing a shader group.
///
class OSLEXECPUBLIC ShadingContext {
public:
    ShadingContext (ShadingSystemImpl &shadingsys, PerThreadInfo *threadinfo);
    ~ShadingContext ();

    /// Return a reference to the shading system for this context.
    ///
    ShadingSystemImpl & shadingsys () const { return m_shadingsys; }

    /// Get a pointer to the RendererServices for this execution.
    ///
    RendererServices *renderer () const { return m_renderer; }

    /// Execute the shaders for the given use (for example,
    /// ShadUseSurface).  If 'run' is false, do all the usual
    /// preparation, but don't actually run the shader.  Return true if
    /// the shader executed, false if it did not (including if the
    /// shader itself was empty).
    bool execute (ShaderUse use, ShadingAttribState &sas,
                  ShaderGlobals &ssg, bool run=true);

    /// Return the current shader use being executed.
    ///
    ShaderUse use () const { return (ShaderUse) m_curuse; }

    ClosureComponent * closure_component_allot(int id, size_t prim_size, int nattrs) {
        size_t needed = sizeof(ClosureComponent) + (prim_size >= 4 ? prim_size - 4 : 0)
                                                 + sizeof(ClosureComponent::Attr) * nattrs;
        ClosureComponent *comp = (ClosureComponent *) m_closure_pool.alloc(needed);
        comp->type = ClosureColor::COMPONENT;
        comp->id = id;
        comp->size = prim_size;
        comp->nattrs = nattrs;
        return comp;
    }

    ClosureMul *closure_mul_allot (const Color3 &w, const ClosureColor *c) {
        ClosureMul *mul = (ClosureMul *) m_closure_pool.alloc(sizeof(ClosureMul));
        mul->type = ClosureColor::MUL;
        mul->weight = w;
        mul->closure = c;
        return mul;
    }

    ClosureMul *closure_mul_allot (float w, const ClosureColor *c) {
        ClosureMul *mul = (ClosureMul *) m_closure_pool.alloc(sizeof(ClosureMul));
        mul->type = ClosureColor::MUL;
        mul->weight.setValue (w,w,w);
        mul->closure = c;
        return mul;
    }

    ClosureAdd *closure_add_allot (const ClosureColor *a, const ClosureColor *b) {
        ClosureAdd *add = (ClosureAdd *) m_closure_pool.alloc(sizeof(ClosureAdd));
        add->type = ClosureColor::ADD;
        add->closureA = a;
        add->closureB = b;
        return add;
    }


    /// Find the named symbol in the (already-executed!) stack of
    /// shaders of the given use, with priority given to
    /// later laters over earlier layers (if they name the same symbol).
    /// Return NULL if no such symbol is found.
    Symbol * symbol (ShaderUse use, ustring name);

    /// Return a pointer to where the symbol's data lives.
    void *symbol_data (Symbol &sym);

    /// Return a reference to a compiled regular expression for the
    /// given string, being careful to cache already-created ones so we
    /// aren't constantly compiling new ones.
    const boost::regex & find_regex (ustring r);

    /// Return a pointer to the shading attribs for this context.
    ///
    ShadingAttribState *attribs () { return m_attribs; }

    /// Return a reference to the MessageList containing messages.
    ///
    MessageList & messages () { return m_messages; }

    /// Look up a query from a dictionary (typically XML), staring the
    /// search from the root of the dictionary, and returning ID of the
    /// first matching node.
    int dict_find (ustring dictionaryname, ustring query);
    /// Look up a query from a dictionary (typically XML), staring the
    /// search from the given nodeID within the dictionary, and
    /// returning ID of the first matching node.
    int dict_find (int nodeID, ustring query);
    /// Return the next match of the same query that gave the nodeID.
    int dict_next (int nodeID);
    /// Look up an attribute of the given dictionary node.  If
    /// attribname is "", return the value of the node itself.
    int dict_value (int nodeID, ustring attribname, TypeDesc type, void *data);

    /// Various setup of the context done by execute().  Return true if
    /// the function should be executed, otherwise false.
    bool prepare_execution (ShaderUse use, ShadingAttribState &sas);

    bool osl_get_attribute (void *renderstate, void *objdata, int dest_derivs,
                            ustring obj_name, ustring attr_name,
                            int array_lookup, int index,
                            TypeDesc attr_type, void *attr_dest);

    PerThreadInfo *thread_info () { return m_threadinfo; }

private:

    /// Execute the llvm-compiled shaders for the given use (for example,
    /// ShadUseSurface).  The context must already be bound.  If
    /// runflags are not supplied, they will be auto-generated with all
    /// points turned on.
    void execute_llvm (ShaderUse use, Runflag *rf=NULL,
                       int *ind=NULL, int nind=0);

    void free_dict_resources ();

    ShadingSystemImpl &m_shadingsys;    ///< Backpointer to shadingsys
    RendererServices *m_renderer;       ///< Ptr to renderer services
    PerThreadInfo *m_threadinfo;        ///< Ptr to our thread's info
    ShadingAttribState *m_attribs;      ///< Ptr to shading attrib state
    std::vector<char> m_heap;           ///< Heap memory
    size_t m_closures_allotted;         ///< Closure memory allotted
    int m_curuse;                       ///< Current use that we're running
#ifdef OIIO_HAVE_BOOST_UNORDERED_MAP
    typedef boost::unordered_map<ustring, boost::regex*, ustringHash> RegexMap;
#else
    typedef hash_map<ustring, boost::regex*, ustringHash> RegexMap;
#endif
    RegexMap m_regex_map;               ///< Compiled regex's
    MessageList m_messages;             ///< Message blackboard

    SimplePool<20 * 1024> m_closure_pool;

    Dictionary *m_dictionary;

    // Struct for holding a record of getattributes we've tried and
    // failed, to speed up subsequent getattributes calls.
    struct GetAttribQuery {
        void *objdata;
        ustring obj_name, attr_name;
        TypeDesc attr_type;
        int array_lookup, index;
        GetAttribQuery () : objdata(NULL), array_lookup(0), index(0) { }
    };
    static const int FAILED_ATTRIBS = 16;
    GetAttribQuery m_failed_attribs[FAILED_ATTRIBS];
    int m_next_failed_attrib;
};





class ShadingAttribState
{
public:
    ShadingAttribState () { }

    ~ShadingAttribState () { }

    /// Return a reference to the shader group for a particular use
    ///
    ShaderGroup & shadergroup (ShaderUse use) {
        return m_shaders[(int)use];
    }

    /// Called when the shaders of the attrib state change (invalidate LLVM ?)
    void changed_shaders () { }

private:
    OSL::pvt::ShaderGroup m_shaders[OSL::pvt::ShadUseLast];
};



namespace Strings {
    extern OSLEXECPUBLIC ustring camera, common, object, shader, screen, NDC;
    extern OSLEXECPUBLIC ustring rgb, RGB, hsv, hsl, YIQ, XYZ, xyz, xyY;
    extern OSLEXECPUBLIC ustring null, default_;
    extern OSLEXECPUBLIC ustring label;
    extern OSLEXECPUBLIC ustring sidedness, front, back, both;
    extern OSLEXECPUBLIC ustring P, I, N, Ng, dPdu, dPdv, u, v, time, dtime, dPdtime, Ps;
    extern OSLEXECPUBLIC ustring Ci;
    extern OSLEXECPUBLIC ustring width, swidth, twidth, rwidth;
    extern OSLEXECPUBLIC ustring blur, sblur, tblur, rblur;
    extern OSLEXECPUBLIC ustring wrap, swrap, twrap, rwrap;
    extern OSLEXECPUBLIC ustring black, clamp, periodic, mirror;
    extern OSLEXECPUBLIC ustring firstchannel, fill, alpha;
    extern OSLEXECPUBLIC ustring interp, closest, linear, cubic, smartcubic;
    extern OSLEXECPUBLIC ustring perlin, uperlin, noise, snoise, pnoise, psnoise;
    extern OSLEXECPUBLIC ustring cell, cellnoise, pcellnoise;
    extern OSLEXECPUBLIC ustring genericnoise, genericpnoise, gabor, gabornoise, gaborpnoise;
    extern OSLEXECPUBLIC ustring anisotropic, direction, do_filter, bandwidth, impulses;
    extern OSLEXECPUBLIC ustring op_dowhile, op_for, op_while;
    extern OSLEXECPUBLIC ustring subimage, subimagename;
}; // namespace Strings



inline int
tex_interp_to_code (ustring modename)
{
    int mode = -1;
    if (modename == Strings::smartcubic)
        mode = TextureOpt::InterpSmartBicubic;
    else if (modename == Strings::linear)
        mode = TextureOpt::InterpBilinear;
    else if (modename == Strings::cubic)
        mode = TextureOpt::InterpBicubic;
    else if (modename == Strings::closest)
        mode = TextureOpt::InterpClosest;
    return mode;
}



// Layout of structure we use to pass noise parameters
struct NoiseParams {
    int anisotropic;
    int do_filter;
    Vec3 direction;
    float bandwidth;
    float impulses;

    NoiseParams ()
        : anisotropic(0), do_filter(true), direction(1.0f,0.0f,0.0f),
          bandwidth(1.0f), impulses(16.0f)
    {
    }
};


OSL_NAMESPACE_EXIT

#endif /* OSLEXEC_PVT_H */
