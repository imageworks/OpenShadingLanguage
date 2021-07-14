// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

#include <memory>

#include <OSL/oslconfig.h>
#include <OSL/shaderglobals.h>

#include <OpenImageIO/refcnt.h>


OSL_NAMESPACE_ENTER

// Various forward declarations
class RendererServices;
class ShaderGroup;
typedef std::shared_ptr<ShaderGroup> ShaderGroupRef;
struct ClosureParam;
struct PerThreadInfo;
class ShadingContext;
class ShaderSymbol;
class OSLQuery;
#if OSL_USE_BATCHED
template<int WidthT> struct alignas(64) BatchedShaderGlobals;
#endif



/// Opaque pointer to whatever the renderer uses to represent a
/// (potentially motion-blurred) coordinate transformation.
typedef const void* TransformationPtr;


// Callbacks for closure creation
typedef void (*PrepareClosureFunc)(RendererServices*, int id, void* data);
typedef void (*SetupClosureFunc)(RendererServices*, int id, void* data);


namespace pvt {
class ShadingSystemImpl;
}

#if defined(__CUDA_ARCH__) && OPTIX_VERSION >= 70000
#  define STRINGIFY(x) XSTR(x)
#  define XSTR(x) #x
#  define STRING_PARAMS(x)  UStringHash::Hash(STRINGIFY(x))
#else
#  define STRING_PARAMS(x)  StringParams::x
#endif

namespace Strings {
#ifdef __CUDA_ARCH__
# if OPTIX_VERSION >= 70000
#  define STRDECL(str,var_name)
#else
#  define STRDECL(str,var_name) extern __device__ ustring var_name;
#endif
#else
    // Any strings referenced inside of a libsoslexec/wide/*.cpp
    // or liboslnoise/wide/*.cpp will need OSLEXECPUBLIC
    #define STRDECL(str,var_name) OSLEXECPUBLIC extern const ustring var_name;
#endif
#include <OSL/strdecls.h>
#undef STRDECL
}; // namespace Strings



/// Description of where a symbol is located on the app side.
struct SymLocationDesc {
public:
    using offset_t = int64_t;
    using stride_t = int64_t;
    static const int64_t AutoStride = std::numeric_limits<stride_t>::min();

    SymLocationDesc() {}
    SymLocationDesc(string_view name, TypeDesc type, bool derivs = false,
                    SymArena arena = SymArena::Heap, offset_t offset = -1,
                    stride_t stride = AutoStride)
        : name(name), type(type), offset(offset),
          stride(stride == AutoStride ? type.size() : stride),
          arena(arena), derivs(derivs)
    {}

    bool operator==(ustring n) const { return name == n; }
    friend bool operator<(ustring n, const SymLocationDesc& sld) {
        return n < sld.name;
    }
    friend bool operator<(const SymLocationDesc& sld, ustring n) {
        return sld.name < n;
    }

    ustring name;                     ///< Name of the symbol
    TypeDesc type;                    ///< Data type of the symbol
    offset_t offset = -1;             ///< Offset from arena base for point 0
    stride_t stride = AutoStride;     ///< Stride in bytes between shade points
    SymArena arena = SymArena::Heap;  ///< Memory arena type for the symbol
    bool derivs = false;              ///< Space allocated for derivs also
};



class OSLEXECPUBLIC ShadingSystem
{
public:
    ShadingSystem (RendererServices *renderer=NULL,
                   TextureSystem *texturesystem=NULL,
                   ErrorHandler *err=NULL);
    ~ShadingSystem ();

    /// Set an attribute controlling the shading system.  Return true
    /// if the name and type were recognized and the attrib was set.
    /// Documented attributes are as follows:
    /// 1. Attributes that should be exposed to users:
    ///    int statistics:level   Automatically print OSL statistics (0).
    ///    string searchpath:shader  Colon-separated path to search for .oso
    ///                                files ("", meaning test "." only)
    ///    string colorspace      Name of RGB color space ("Rec709")
    ///    int range_checking     Generate extra code for component & array
    ///                              range checking (1)
    ///    int debug_nan          Add extra (expensive) code to pinpoint
    ///                              when NaN/Inf happens (0).
    ///    int debug_uninit       Add extra (expensive) code to pinpoint
    ///                              use of uninitialized variables (0).
    ///    int compile_report     Issue info messages to the renderer for
    ///                              every shader compiled (0).
    ///    int max_warnings_per_thread  Number of warning calls that should be
    ///                              processed per thread (100).
    ///    int buffer_printf      Buffer printf output from shaders and
    ///                              output atomically, to prevent threads
    ///                              from interleaving lines. (1)
    ///    int profile            Perform some rudimentary profiling (0)
    ///    int no_noise           Replace noise with constant value. (0)
    ///    int no_pointcloud      Skip pointcloud lookups. (0)
    ///    int exec_repeat        How many times to run each group (1).
    ///    int opt_warnings       Warn on certain failure to runtime-optimize
    ///                              certain shader constructs. (0)
    ///    int gpu_opt_error      Consider a hard error if certain shader
    ///                              constructs cannot be optimized away. (0)
    /// 2. Attributes that should be set by applications/renderers that
    /// incorporate OSL:
    ///    string commonspace     Name of "common" coord system ("world")
    ///    string[] raytypes      Array of ray type names
    ///    string[] renderer_outputs
    ///                           Array of names of renderer outputs (AOVs)
    ///                              that should not be optimized away.
    ///    int unknown_coordsys_error  Should errors be issued when unknown
    ///                              coord system names are used? (1)
    ///    int connection_error   Should errors be issued when ConnectShaders
    ///                              fails to find the layer or parameter? (1)
    ///    int strict_messages    Issue error if a message is set after
    ///                              being queried (1).
    ///    int error_repeats      If zero, suppress repeats of errors and
    ///                              warnings that are exact duplicates of
    ///                              earlier ones. (1)
    ///    int lazylayers         Evaluate shader layers only when their
    ///                              outputs are first needed (1)
    ///    int lazyglobals        Run layers lazily even if they write to
    ///                              globals (1)
    ///    int lazyunconnected    Run layers lazily even if they have no
    ///                              output connections (1). For debugging.
    ///    int lazyerror          Run layers lazily even if they have error
    ///                              ops after optimization (1).
    ///    int lazy_userdata      Retrieve userdata lazily (0).
    ///    int userdata_isconnected  Should lockgeom=0 params (that may
    ///                              receive userdata) return true from
    ///                              isconnected()? (0)
    ///    int greedyjit          Optimize and compile all shaders up front,
    ///                              versus only as needed (0).
    ///    int llvm_target_host   Target the specific host architecture for
    ///                              LLVM IR generation. (1)
    ///    int llvm_jit_fma       Allow fused mul/add (0). This can increase
    ///                              speed but can change rounding accuracy
    ///                              (generally by being better), and might
    ///                              differ by hardware platform.
    ///    string llvm_jit_target  JIT to a specific ISA: "" or "none" means
    ///                              no special ops, "x64", "SSE4.2", "AVX",
    ///                              "AVX2", "AVX2_noFMA", "AVX512",
    ///                              "AVX512_noFMA", or "host" means to
    ///                              figure out what the host can do. ("")
    ///    int llvm_jit_aggressive  Use LLVM "aggressive" JIT mode. (0)
    ///    int vector_width       Vector width to allow for SIMD ops (4).
    ///    int llvm_debugging_symbols  When JITing, generate debug symbols
    ///                             that associate machine code with shader
    ///                             source and lines. (0)
    ///    int llvm_profiling_events  When JITing, generate events to enable
    ///                             full profiling of shaders. (0)
    ///    int lockgeom           Default 'lockgeom' value for shader params
    ///                              that don't specify it (1).  Lockgeom
    ///                              means a param CANNOT be overridden by
    ///                              interpolated geometric parameters.
    ///    int countlayerexecs    Add extra code to count total layers run.
    ///    int allow_shader_replacement Allow shader to be specified more than
    ///                              once, replacing former definition.
    ///    string archive_groupname  Name of a group to pickle and archive.
    ///    string archive_filename   Name of file to save the group archive.
    /// 3. Attributes that that are intended for developers debugging
    /// liboslexec itself:
    /// These attributes may be helpful for liboslexec developers or
    /// for debugging, but probably not for using OSL in production:
    ///    int debug              Set debug output level (0)
    ///    int clearmemory        Zero out working memory before each shade (0)
    ///    int optimize           Runtime optimization level (2)
    ///       And there are several int options that, if set to 0, will turn
    ///       off individual classes of runtime optimizations:
    ///         opt_simplify_param, opt_constant_fold, opt_stale_assign,
    ///         opt_elide_useless_ops, opt_elide_unconnected_outputs,
    ///         opt_peephole, opt_coalesce_temps, opt_assign, opt_mix
    ///         opt_merge_instances, opt_merge_instance_with_userdata,
    ///         opt_fold_getattribute, opt_middleman, opt_texture_handle
    ///         opt_seed_bblock_aliases
    ///    int opt_passes         Number of optimization passes per layer (10)
    ///    int llvm_optimize      Which of several LLVM optimize strategies (1)
    ///    int llvm_debug         Set LLVM extra debug level (0)
    ///    int llvm_debug_layers  Extra printfs upon entering and leaving
    ///                              layer functions.
    ///    int llvm_debug_ops     Extra printfs for each OSL op (helpful
    ///                              for devs to find crashes)
    ///    int llvm_output_bitcode  Output the full bitcode for each group,
    ///                              for debugging. (0)
    ///    int llvm_dumpasm       Print the CPU assembly code from the JIT (0)
    ///    string llvm_prune_ir_strategy  Strategy for pruning unnecessary
    ///                              IR (choices: "prune" [default],
    ///                              "internalize", or "none").
    ///    int max_local_mem_KB   Error if shader group needs more than this
    ///                              much local storage to execute (1024K)
    ///    string debug_groupname Name of shader group -- debug only this one
    ///    string debug_layername Name of shader layer -- debug only this one
    ///    int optimize_nondebug  If 1, fully optimize shaders that are not
    ///                              designated as the debug shaders.
    ///    string opt_layername   If set, only optimize the named layer
    ///    string only_groupname  Compile only this one group (skip all others)
    ///    int force_derivs       Force all float-based variables to compute
    ///                              and store derivatives. (0)
    ///
    /// Note: the attributes referred to as "string" are actually on the app
    /// side as ustring or const char* (they have the same data layout), NOT
    /// std::string!
    bool attribute (string_view name, TypeDesc type, const void *val);

    // Shortcuts for common types
    bool attribute (string_view name, int val) {
        return attribute (name, TypeDesc::INT, &val);
    }
    bool attribute (string_view name, float val) {
        return attribute (name, TypeDesc::FLOAT, &val);
    }
    bool attribute (string_view name, double val) {
        float f = (float) val;
        return attribute (name, TypeDesc::FLOAT, &f);
    }
    bool attribute (string_view name, string_view val) {
        const char *s = val.c_str();
        return attribute (name, TypeDesc::STRING, &s);
    }


    /// Set an attribute for a specific shader group.  Return true if the
    /// name and type were recognized and the attrib was set. Documented
    /// attributes are as follows:
    ///    string[] renderer_outputs  Array of names of renderer outputs
    ///                                 (AOVs) specific to this shader group
    ///                                 that should not be optimized away.
    ///    string[] entry_layers      Array of names of layers that may be
    ///                                 callable entry points. They won't
    ///                                 be elided, but nor will they be
    ///                                 called unconditionally.
    ///    int exec_repeat            How many times to run the group (1).
    ///
    bool attribute (ShaderGroup *group, string_view name,
                    TypeDesc type, const void *val);
    bool attribute (ShaderGroup *group, string_view name, int val) {
        return attribute (group, name, TypeDesc::INT, &val);
    }
    bool attribute (ShaderGroup *group, string_view name, float val) {
        return attribute (group, name, TypeDesc::FLOAT, &val);
    }
    bool attribute (ShaderGroup *group, string_view name, double val) {
        float f = (float) val;
        return attribute (group, name, TypeDesc::FLOAT, &f);
    }
    bool attribute (ShaderGroup *group, string_view name, string_view val) {
        const char *s = val.c_str();
        return attribute (group, name, TypeDesc::STRING, &s);
    }

    /// Get the named attribute of the ShadingSystem, store it in `*val`.
    /// Return `true` if found and it was compatible with the type
    /// specified, otherwise return `false` and do not modify the contents
    /// of `*val`.  It is up to the caller to ensure that `val` points to
    /// the right kind and size of storage for the given type.
    ///
    /// In addition to being able to retrieve all the attributes that are
    /// documented as settable by the `attribute()` call, `getattribute()`
    /// can also retrieve the following read-only attributes:
    ///
    /// - `string osl:simd`, `string hw:simd` : A comma-separated list of
    ///   CPU hardware SIMD features. The `osl:simd` is a list of features
    ///   enabled when OSL was built, and `hw:simd` is a list of features
    ///   detected on the actual hardware at runtime.
    ///
    /// - `string osl:cuda_version` : The version string of the Cuda version
    ///   OSL is using (empty string if no Cuda support was enabled at build
    ///   time).
    ///
    /// - `string osl:optix_version` : The version string of the OptiX
    ///   version OSL is using (empty string if no OptiX support was enabled
    ///   at build time).
    ///
    /// - `string osl:dependencies` : A comma-separated list of OSL's major
    ///   library build dependencies and their versions (for example,
    ///   "OIIO-2.3.0,LLVM-10.0.0,OpenEXR-2.5.0").
    ///
    bool getattribute (string_view name, TypeDesc type, void *val);

    /// Shortcut getattribute() for retrieving a single integer.
    /// The value is placed in `val`, and the function returns `true` if the
    /// attribute was found and was legally convertible to an int.
    bool getattribute (string_view name, int &val) {
        return getattribute (name, TypeDesc::INT, &val);
    }
    /// Shortcut getattribute() for retrieving a single float.
    /// The value is placed in `val`, and the function returns `true` if the
    /// attribute was found and was legally convertible to a float.
    bool getattribute (string_view name, float &val) {
        return getattribute (name, TypeDesc::FLOAT, &val);
    }
    bool getattribute (string_view name, double &val) {
        float f;
        bool ok = getattribute (name, TypeDesc::FLOAT, &f);
        if (ok)
            val = f;
        return ok;
    }
    /// Shortcut getattribute() for retrieving a single string as a
    /// `const char*`. The value is placed in `val`, and the function
    /// returns `true` if the attribute was found.
    bool getattribute (string_view name, char **val) {
        return getattribute (name, TypeDesc::STRING, val);
    }
    /// Shortcut getattribute() for retrieving a single string as a
    /// `ustring`. The value is placed in `val`, and the function returns
    /// `true` if the attribute was found.
    bool getattribute (string_view name, ustring &val) {
        return getattribute (name, TypeDesc::STRING, (char **)&val);
    }
    /// Shortcut getattribute() for retrieving a single string as a
    /// `std::string`. The value is placed in `val`, and the function
    /// returns `true` if the attribute was found.
    bool getattribute (string_view name, std::string &val) {
        const char *s = NULL;
        bool ok = getattribute (name, TypeDesc::STRING, &s);
        if (ok)
            val = s;
        return ok;
    }

    /// Get the named attribute about a particular shader group, store it
    /// in value.  Attributes that are currently documented include:
    ///   string groupname           The name of the shader group.
    ///   int num_layers             The number of layers in the group.
    ///   string[] layer_names       The names of the layers in the group.
    ///   int num_textures_needed    The number of texture names that are
    ///                                known to be potentially needed by the
    ///                                group (after optimization).
    ///   ptr textures_needed        Retrieves a pointer to the ustring array
    ///                                containing all textures known to be
    ///                                needed.
    ///   int unknown_textures_needed  Nonzero if additional textures may be
    ///                                needed, whose names can't be known
    ///                                without actually running the shader.
    ///   int num_closures_needed    The number of named closures needed.
    ///   ptr closures_needed        Retrieves a pointer to the ustring array
    ///                                containing all closures known to be
    ///                                needed.
    ///   int unknown_closures_needed  Nonzero if additional closures may be
    ///                                needed, whose names can't be known
    ///                                without actually running the shader.
    ///   int globals_read           Bitfield ("or'ed" SGBits values) of
    ///                                which ShaderGlobals may be read by
    ///                                by the shader group.
    ///   int globals_write         Bitfield ("or'ed" SGBits values) of
    ///                                which ShaderGlobals may be written by
    ///                                by the shader group.
    ///   int num_globals_needed     The number of named globals needed.
    ///   ptr globals_needed         Retrieves a pointer to the ustring array
    ///                                containing all globals needed.
    ///   int num_userdata           The number of "user data" variables
    ///                                retrieved by the shader.
    ///   ptr userdata_names         Retrieves a pointer to the array of
    ///                                ustring holding the userdata names.
    ///   ptr userdata_types         Retrieves a pointer to the array of
    ///                                 TypeDesc describing the userdata.
    ///   ptr userdata_offsets       Retrieves a pointer to the array of
    ///                                 int describing the userdata offsets
    ///                                 within the heap.
    ///   int num_attributes_needed  The number of attribute/scope pairs that
    ///                                are known to be queried by the group (the
    ///                                length of the attributes_needed and
    ///                                attribute_scopes arrays).
    ///   ptr attributes_needed      Retrieves a pointer to the ustring array
    ///                                containing the names of the needed attributes.
    ///	                               Note that if the same attribute
    ///                                is requested in multiple scopes, it will
    ///                                appear in the array multiple times - once for
    ///                                each scope in which is is queried.
    ///   ptr attribute_scopes       Retrieves a pointer to a ustring array containing
    ///                                the scopes associated with each attribute query
    ///                                in the attributes_needed array.
    ///   int unknown_attributes_needed  Nonzero if additional attributes may be
    ///                                  needed, whose names will not be known
    ///                                  until the shader actually runs.
    ///   int num_renderer_outputs   Number of named renderer outputs.
    ///   string renderer_outputs[]  List of renderer outputs.
    ///   int raytype_queries        Bit field of all possible rayquery
    ///   int num_entry_layers       Number of named entry point layers.
    ///   string entry_layers[]      List of entry point layers.
    ///   string pickle              Retrieves a serialized representation
    ///                                 of the shader group declaration.
    /// Note: the attributes referred to as "string" are actually on the app
    /// side as ustring or const char* (they have the same data layout), NOT
    /// std::string!
    bool getattribute (ShaderGroup *group, string_view name,
                       TypeDesc type, void *val);
    // Shortcuts for common types
    bool getattribute (ShaderGroup *group, string_view name, int &val) {
        return getattribute (group, name, TypeDesc::INT, &val);
    }
    bool getattribute (ShaderGroup *group, string_view name, float &val) {
        return getattribute (group, name, TypeDesc::FLOAT, &val);
    }
    bool getattribute (ShaderGroup *group, string_view name, double &val) {
        float f;
        bool ok = getattribute (group, name, TypeDesc::FLOAT, &f);
        if (ok)
            val = f;
        return ok;
    }
    bool getattribute (ShaderGroup *group, string_view name, char **val) {
        return getattribute (group, name, TypeDesc::STRING, val);
    }
    bool getattribute (ShaderGroup *group, string_view name, ustring &val) {
        return getattribute (group, name, TypeDesc::STRING, (char **)&val);
    }
    bool getattribute (ShaderGroup *group, string_view name, std::string &val) {
        const char *s = NULL;
        bool ok = getattribute (group, name, TypeDesc::STRING, &s);
        if (ok)
            val = s;
        return ok;
    }


    /// Load compiled shader (oso) from a memory buffer, overriding
    /// shader lookups in the shader search path
    bool LoadMemoryCompiledShader (string_view shadername,
                                   string_view buffer);

    // The basic sequence for declaring a shader group looks like this:
    // ShadingSystem *ss = ...;
    // ShaderGroupRef group = ss->ShaderGroupBegin (groupname);
    //    /* First layer - texture lookup shader: */
    //       /* Specify instance parameter values */
    //       const char *mapname = "colormap.exr";
    //       ss->Parameter (*group, "texturename", mapname);
    //       float blur = 0.001;
    //       ss->Parameter (*group, "blur", blur);
    //       Vec3 colorfilter (0.5f, 0.5f, 1.0f);
    //       ss->Parameter (*group, "colorfilter", TypeDesc::TypeColor,
    //                      &colorfilter);
    //    ss->Shader ("surface", "texmap", "texturelayer");
    //    /* Second layer - generate the BSDF closure: */
    //       float roughness = 0.05;
    //       ss->Parameter (*group, "roughness", roughness);
    //    ss->Shader (*group, "surface", "plastic", "illumlayer");
    //    /* Make a connection between the layers */
    //    ss->ConnectShaders (*group, "texturelayer", "Cout",
    //                       "illumlayer", "Cs");
    // ss->ShaderGroupEnd (*group);

    /// Signal the start of a new shader group.  The return value is a
    /// reference-counted opaque handle to the ShaderGroup.
    ShaderGroupRef ShaderGroupBegin (string_view groupname = string_view());

    /// Alternate way to specify a shader group. The group specification
    /// syntax looks like this: (as a string, all whitespace is equivalent):
    ///     param <typename> <paramname> <value>... [[hints]] ;
    ///     shader <shadername> <layername> ;
    ///     connect <layername>.<paramname> <layername>.<paramname> ;
    /// For the sake of easy assembling on command lines, a comma ',' may
    /// substitute for the semicolon as a separator, and the last separator
    /// before the end of the string is optional.
    ShaderGroupRef ShaderGroupBegin (string_view groupname,
                                     string_view shaderusage,
                                     string_view groupspec = string_view());

    /// Signal the end of a new shader group.
    ///
    bool ShaderGroupEnd (ShaderGroup& group);

    /// Set a parameter of the next shader that will be added to the group,
    /// optionally setting the 'lockgeom' metadata for that parameter
    /// (despite how it may have been set in the shader).  If lockgeom is
    /// false, it means that this parameter should NOT be considered locked
    /// against changes by the geometry, and therefore the shader should not
    /// optimize assuming that the instance value (the 'val' specified by
    /// this call) is a constant.
    bool Parameter (ShaderGroup& group, string_view name, TypeDesc t,
                    const void *val, bool lockgeom=true);
    // Shortcuts for param passing a single int, float, or string.
    bool Parameter (ShaderGroup& group, string_view name,
                    int val, bool lockgeom=true) {
        return Parameter (group, name, TypeDesc::INT, &val, lockgeom);
    }
    bool Parameter (ShaderGroup& group, string_view name,
                    float val, bool lockgeom=true) {
        return Parameter (group, name, TypeDesc::FLOAT, &val, lockgeom);
    }
    bool Parameter (ShaderGroup& group, string_view name,
                    const std::string& val, bool lockgeom=true) {
        const char *s = val.c_str();
        return Parameter (group, name, TypeDesc::STRING, &s, lockgeom);
    }
    bool Parameter (ShaderGroup& group, string_view name,
                    ustring val, bool lockgeom=true) {
        return Parameter (group, name, TypeDesc::STRING, (const char**)&val, lockgeom);
    }

    /// Append a new shader instance onto the specified group. The shader
    /// instance will get any pending parameters that were set by
    /// Parameter() calls since the last Shader() call for the group.
    bool Shader (ShaderGroup& group, string_view shaderusage,
                 string_view shadername, string_view layername);

    /// Connect two shaders within the specified group. The source layer
    /// must be *upstream* of down destination layer (i.e. source must be
    /// declared earlier within the shader group). The named parameters must
    /// be of compatible type -- float to float, color to color, array to
    /// array of the same length and element type, etc. In general, it is
    /// permissible to connect type A to type B if and only if it is allowed
    /// within OSL to assign an A to a B (i.e., if `A = B` is legal). So any
    /// "triple" may be connected to any other triple, and a float output
    /// may be connected to a triple input (but not the other way around).
    /// It is permitted to connect a single component of an aggregate to a
    /// float and vice versa, for example,
    ///   `ConnectShaders (group, "lay1", "mycolorout[2]",
    ///                    "lay2", "myfloatinput")`
    ///
    bool ConnectShaders (ShaderGroup &group,
                         string_view srclayer, string_view srcparam,
                         string_view dstlayer, string_view dstparam);

    /// Replace a parameter value in a previously-declared shader group.
    /// This is meant to called after the ShaderGroupBegin/End, but will
    /// fail if the shader has already been irrevocably optimized/compiled,
    /// unless the particular parameter is marked as lockgeom=0 (which
    /// indicates that it's a parameter that may be overridden by the
    /// geometric primitive).  This call gives you a way of changing the
    /// instance value, even if it's not a geometric override.
    bool ReParameter (ShaderGroup &group,
                      string_view layername, string_view paramname,
                      TypeDesc type, const void *val);
    // Shortcuts for param passing a single int, float, or string.
    bool ReParameter (ShaderGroup &group, string_view layername,
                      string_view paramname, int val) {
        return ReParameter (group, layername, paramname, TypeDesc::INT, &val);
    }
    bool ReParameter (ShaderGroup &group, string_view layername,
                      string_view paramname, float val) {
        return ReParameter (group, layername, paramname, TypeDesc::FLOAT, &val);
    }
    bool ReParameter (ShaderGroup &group, string_view layername,
                      string_view paramname, const std::string& val) {
        const char *s = val.c_str();
        return ReParameter (group, layername, paramname, TypeDesc::STRING, &s);
    }
    bool ReParameter (ShaderGroup &group, string_view layername,
                      string_view paramname, ustring val) {
        return ReParameter (group, layername, paramname, TypeDesc::STRING,
                            (const char**)&val);
    }

    // Non-threadsafe versions of Parameter, Shader, ConnectShaders, and
    // ShaderGroupEnd. These depend on some persistent state about which
    // shader group is the "current" one being amended. It's fine to use
    // that as long as all shader specification is done from one thread only
    // (or at least that you are sure no two groups are being specified
    // concurrently). If there is any doubt about that, use the versions
    // above that take an explicit `ShaderGroup&`, which are thread-safe
    // and re-entrant.
    bool Parameter (string_view name, TypeDesc t, const void *val,
                    bool lockgeom=true);
    bool Shader (string_view shaderusage, string_view shadername,
                 string_view layername);
    bool ConnectShaders (string_view srclayer, string_view srcparam,
                         string_view dstlayer, string_view dstparam);
    bool ShaderGroupEnd (void);

    /// Create a per-thread data needed for shader execution.  It's very
    /// important for the app to never use a PerThreadInfo from more than
    /// one thread (and probably a good idea allocate only one PerThreadInfo
    /// for each renderer thread), and destroy it with destroy_thread_info
    /// when the thread terminates (and before the ShadingSystem is
    /// destroyed).
    PerThreadInfo * create_thread_info();

    /// Destroy a PerThreadInfo that was allocated by
    /// create_thread_info().
    void destroy_thread_info (PerThreadInfo *threadinfo);

    /// Get a ShadingContext that we can use.  The context is specific to a
    /// renderer thread, and should never be passed between or shared by
    /// more than one thread.  The 'threadinfo' parameter should be a
    /// thread-specific pointer created by create_thread_info.  The context
    /// can be used to shade many points; a typical usage is to allocate
    /// just one context per thread and use it for the whole run.
    ShadingContext *get_context (PerThreadInfo *threadinfo,
                                 TextureSystem::Perthread *texture_threadinfo=NULL);

    /// Return a ShadingContext to the pool.
    ///
    void release_context (ShadingContext *ctx);

    /// Execute the shader group in this context on shading point
    /// `shadeindex`. If ctx is nullptr, then execute will request one
    /// (based on the running thread) on its own and then return it when
    /// it's done.  This is just a wrapper around execute_init,
    /// execute_layer of the last (presumably group entry) layer, and
    /// execute_cleanup. If run==false, just do the binding and setup, don't
    /// actually run the shader.
    bool execute(ShadingContext &ctx, ShaderGroup &group, int shadeindex,
                 ShaderGlobals& globals, void* output_base_ptr,
                 bool run = true);

    // DEPRECATED(2.0): no shadeindex or base pointers
    bool execute (ShadingContext &ctx, ShaderGroup &group,
                  ShaderGlobals &globals, bool run=true) {
        return execute(ctx, group, 0, globals, nullptr, run);
    }

    // DEPRECATED(2.0): ctx pointer
    bool execute (ShadingContext *ctx, ShaderGroup &group,
                  ShaderGlobals &globals, bool run=true) {
        return execute(*ctx, group, globals, run);
    }

    /// Bind a shader group and globals to the context, in preparation to
    /// execute, including optimization and JIT of the group (if it has not
    /// already been done).  If 'run' is true, also run any initialization
    /// necessary. If 'run' is false, we are not planning to actually
    /// execute any part of the shader, so do all the usual binding
    /// preparation, but don't actually run the shader.  Return true if the
    /// shader executed, false if it did not (including if the shader itself
    /// was empty).
    bool execute_init(ShadingContext &ctx, ShaderGroup &group, int shadeindex,
                      ShaderGlobals &globals, void* output_base_ptr,
                      bool run=true);
    // DEPRECATED(2.0): no shadeindex or base pointers
    bool execute_init (ShadingContext &ctx, ShaderGroup &group,
                       ShaderGlobals &globals, bool run=true) {
        return execute_init(ctx, group, 0, globals, nullptr, run);
    }

    /// Execute the layer whose layernumber is specified, in this context.
    /// It is presumed that execute_init() has already been called, with
    /// run==true, and that the call to execute_init() returned true. (One
    /// reason why it might have returned false is if the shader group
    /// turned out, after optimization, to do nothing.)
    bool execute_layer(ShadingContext &ctx, int shadeindex, ShaderGlobals &globals,
                       void* output_base_ptr, int layernumber);
    /// Execute the layer by name.
    bool execute_layer(ShadingContext &ctx, int shadeindex, ShaderGlobals &globals,
                       void* output_base_ptr, ustring layername);
    /// Execute the layer that has the given ShaderSymbol as an output.
    /// (The symbol is one returned by find_symbol()).
    bool execute_layer(ShadingContext &ctx, int shadeindex, ShaderGlobals &globals,
                       void* output_base_ptr, const ShaderSymbol *symbol);

    // DEPRECATED(2.0): no shadeindex or base pointers
    bool execute_layer (ShadingContext &ctx, ShaderGlobals &globals,
                        int layernumber) {
        return execute_layer(ctx, 0, globals, nullptr, layernumber);
    }
    bool execute_layer (ShadingContext &ctx, ShaderGlobals &globals,
                        ustring layername) {
        return execute_layer(ctx, 0, globals, nullptr, layername);
    }
    bool execute_layer (ShadingContext &ctx, ShaderGlobals &globals,
                        const ShaderSymbol *symbol) {
        return execute_layer(ctx, 0, globals, nullptr, symbol);
    }

    /// Signify that the context is done with the current execution of the
    /// group that was kicked off by execute_init and one or more calls to
    /// execute_layer.
    bool execute_cleanup (ShadingContext &ctx);

    /// Find the named layer within a group and return its index, or -1
    /// if no such named layer exists.
    int find_layer (const ShaderGroup &group, ustring layername) const;

    /// Get a raw pointer to a named symbol (such as you'd need to pull
    /// out the value of an output parameter).  ctx is the shading
    /// context (presumably already run), name is the name of the
    /// symbol.  If found, get_symbol will return the pointer to the
    /// symbol's data, and type will get the symbol's type.  If the
    /// symbol is not found, get_symbol will return NULL.
    /// If you give just a symbol name, it will search for the symbol in all
    /// layers, last-to-first. If a specific layer is named, it will search
    /// only that layer. You can specify a layer either by naming it
    /// separately, or by concatenating "layername.symbolname", but note
    /// that the latter will involve string manipulation inside get_symbol
    /// and is much more expensive than specifying them separately.
    ///
    /// These are considered somewhat deprecated, in favor of using
    /// find_symbol(), symbol_typedesc(), and symbol_address().
    const void* get_symbol (const ShadingContext &ctx, ustring layername,
                            ustring symbolname, TypeDesc &type) const;
    const void* get_symbol (const ShadingContext &ctx, ustring symbolname,
                            TypeDesc &type) const;

    /// Search for an output symbol by name (and optionally, layer) within
    /// the optimized shader group. If the symbol is found, return an opaque
    /// identifying pointer to it, otherwise return NULL. This is somewhat
    /// expensive because of the name-based search, but once done, you can
    /// reuse the pointer to the symbol for the lifetime of the group.
    ///
    /// If you give just a symbol name, it will search for the symbol in all
    /// layers, last-to-first. If a specific layer is named, it will search
    /// only that layer. You can specify a layer either by naming it
    /// separately, or by concatenating "layername.symbolname", but note
    /// that the latter will involve string manipulation inside find_symbol
    /// and is much more expensive than specifying them separately.
    const ShaderSymbol* find_symbol (const ShaderGroup &group,
                             ustring layername, ustring symbolname) const;
    const ShaderSymbol* find_symbol (const ShaderGroup &group,
                                     ustring symbolname) const;

    /// Given an opaque ShaderSymbol*, return the TypeDesc describing it.
    /// Note that a closure will end up with a TypeDesc::UNKNOWN value.
    TypeDesc symbol_typedesc (const ShaderSymbol *sym) const;

    /// Given a context (that has executed a shader) and an opaque
    /// ShaderSymbol*, return the actual memory address where the value of
    /// the symbol resides within the heap memory of the context. This
    /// is only valid for the shader execution that had happened immediately
    /// prior for this context, but it is a very inexpensive operation.
    const void* symbol_address (const ShadingContext &ctx,
                                const ShaderSymbol *sym) const;

#if OSL_USE_BATCHED
    /// Based on currently set attributes for llvm_jit_target and
    /// llvm_jit_fma, test if current machine is capable of supporting
    /// batched execution at the specified width.  If no specific
    /// target was requested, sets llvm_jit_target and llvm_jit_fma
    /// to the supported configuration for the requested width.
    /// Returns true if supported, false otherwise
    bool configure_batch_execution_at(int width);

    template<int WidthT>
    class OSLEXECPUBLIC BatchedExecutor {
        ShadingSystem & m_shading_system;
    public:
        explicit OSL_FORCEINLINE BatchedExecutor(ShadingSystem & ss)
        :m_shading_system(ss)
        {}
        OSL_FORCEINLINE BatchedExecutor(const BatchedExecutor&) = default;

        /// Ensure that the group has been JITed.
        void jit_group (ShaderGroup *group, ShadingContext *ctx);

        /// If option "greedyjit" was set, this call will trigger all
        /// shader groups that have not yet been compiled to do so with the
        /// specified number of threads (0 means use all available HW cores).
        void jit_all_groups (int nthreads=0);

        bool execute(ShadingContext &ctx, ShaderGroup &group, int batch_size,
                         BatchedShaderGlobals<WidthT> &globals_batch, bool run=true);

        bool execute_init (ShadingContext &ctx, ShaderGroup &group, int batch_size,
                                 BatchedShaderGlobals<WidthT> &globals_batch, bool run=true);

        bool execute_layer (ShadingContext &ctx, int batch_size, BatchedShaderGlobals<WidthT> &globals_batch,
                            int layernumber);
        bool execute_layer (ShadingContext &ctx, int batch_size, BatchedShaderGlobals<WidthT> &globals_batch,
                                  ustring layername);
        bool execute_layer (ShadingContext &ctx, int batch_size, BatchedShaderGlobals<WidthT> &globals_batch,
                                  const ShaderSymbol *symbol);
    };

    template<int WidthT>
    OSL_FORCEINLINE BatchedExecutor<WidthT> batched() {
        return BatchedExecutor<WidthT>(*this);
    }
#endif


    /// Return the statistics output as a huge string.
    ///
    std::string getstats (int level=1) const;

    void register_closure (string_view name, int id, const ClosureParam *params,
                           PrepareClosureFunc prepare, SetupClosureFunc setup);

    /// Query either by name or id an existing closure. If name is non
    /// NULL it will use it for the search, otherwise id would be used
    /// and the name will be placed in name if successful. Also return
    /// pointer to the params array in the last argument. All args are
    /// optional but at least one of name or id must non NULL.
    bool query_closure (const char **name, int *id,
                        const ClosureParam **params);

    /// For the proposed shader "global" name, return the corresponding
    /// SGBits enum.
    static SGBits globals_bit (ustring name);

    /// For the SGBits value, return the shader "globals" name.
    static ustring globals_name (SGBits bit);

    /// For the proposed raytype name, return the bit pattern that
    /// describes it, or 0 for an unrecognized name.  (This retrieves
    /// data passed in via attribute("raytypes")).
    int raytype_bit (ustring name);

    /// Configure the default raytypes to assume to be on (or off) at optimization
    /// time for the given group. The raytypes_on gives a bitfield describing which
    /// ray flags are known to be 1, and raytypes_off describes which ray flags are
    /// known to be 0. Bits that are not set in either set of flags are not known
    /// to the optimizer, and will be determined strictly at execution time.
    void set_raytypes(ShaderGroup *group, int raytypes_on, int raytypes_off);

    /// Clear any known mappings of symbol locations.
    void clear_symlocs();
    void clear_symlocs(ShaderGroup* group);

    /// Add symbol location mappings.
    void add_symlocs(cspan<SymLocationDesc> symlocs);
    void add_symlocs(ShaderGroup* group, cspan<SymLocationDesc> symlocs);

    /// Ensure that the group has been optimized and optionally JITed. The ctx pointer
    /// supplies a ShadingContext to use.
    void optimize_group (ShaderGroup *group, ShadingContext *ctx, bool do_jit = true);

    /// Ensure that the group has been optimized and optionally JITed. This is a
    /// convenience function that simply calls set_raytypes followed by
    /// optimize_group. The ctx supplies a ShadingContext to use.
    void optimize_group (ShaderGroup *group, int raytypes_on,
                         int raytypes_off, ShadingContext *ctx, bool do_jit = true);

    /// If option "greedyjit" was set, this call will trigger all
    /// shader groups that have not yet been compiled to do so with the
    /// specified number of threads (0 means use all available HW cores).
    void optimize_all_groups (int nthreads=0, bool do_jit = true);

    /// Return a pointer to the TextureSystem being used.
    TextureSystem * texturesys () const;

    /// Return a pointer to the RendererServices being used.
    RendererServices * renderer () const;

    /// Archive the entire shader group so that it can be reconstituted
    /// later.
    bool archive_shadergroup (ShaderGroup &group, string_view filename);

    // DEPRECATED(2.0)
    bool archive_shadergroup (ShaderGroup *group, string_view filename);

    /// Construct and return an OSLQuery initialized with an existing
    /// ShaderGroup. For a shader group already loaded by the ShadingSystem,
    /// this is much less expensive than constructing an OSLQuery by reading
    /// the oso from disk, as would be done by liboslquery.
    OSLQuery oslquery(const ShaderGroup& group, int layernum);

    /// Helper function -- copy or convert a source value (described by
    /// srctype) to destination (described by dsttype).  The function
    /// returns true upon success, or false if the types differ in a way
    /// that cannot be converted.  As a special case, if dst==NULL or
    /// src==NULL, no copying is performed, and convert_value merely
    /// returns a bool indicating if the proposed type conversion is
    /// allowed.
    ///
    /// The following type conversions are supported:
    /// 1. Identical types copy without modification.
    /// 2. Conversions following the same rules as type casting and
    /// assignment in OSL itself:
    ///   int -> float             convert to float
    ///   int -> triple            convert to float and replicate x3
    ///   int -> float[4]          convert to float and replicate x4
    ///   float -> triple          replicate x3
    ///   float -> float[4]        replicate x4
    ///   float -> int             truncate like a (int) type cast
    ///   triple -> triple         copy, regardless of differing vector types
    /// 3. Additional rules not allowed in OSL source code:
    ///   float -> float[2]        replicate x2
    ///   int -> float[2]          convert to float and replicate x2
    ///   float[2] -> triple       (f[0], f[1], 0)
    ///   float[4] -> vec4         allow conversion to OIIO type (no vec4 in OSL)
    ///
    /// Observation: none of the supported conversions require more
    /// storage for src than for dst.
    static bool convert_value(void* dst, TypeDesc dsttype, const void* src,
                              TypeDesc srctype);

private:
    pvt::ShadingSystemImpl* m_impl;
};



#ifdef OPENIMAGEIO_IMAGEBUFALGO_H
// To keep from polluting all OSL clients with ImageBuf & ROI, only expose
// the following declarations if they have included OpenImageIO/imagebufalgo.h.

// enum describing where shades are located for shade_image().
enum ShadeImageLocations {
    ShadePixelCenters,  // locate shades at pixel centers: (i+0.5)/res
    ShadePixelGrid      // locate shades at grid nodes: i/(res-1)
};


/// Utility to execute a shader group on each pixel in a rectangular region
/// of an ImageBuf (which must already be allocated and which must have
/// FLOAT pixels).  The output parameters to save are specified by an array
/// of ustring values in 'outputs'. If there are multiple outputs, they will
/// simply be concatenated channel by channel in the image.
///
/// The roi specifies the region of the ImageBuf to shade (defaulting to the
/// whole thing), any pixels outside the roi will not be altered.
///
/// The 'defaultsg', if non-NULL, provides a template for the default
/// ShaderGlobals to use for each point. If not provided, reasonable
/// defaults will be chosen.
///
/// When shading, P will have the pixel lattice coordinates (i,j,k), and u
/// and v will vary from 0->1 across the full (aka "display") window.
/// Depending on the value of 'shadelocations', the shading locations
/// themselves will either be at "pixel centers" (position (i+0.5)/res), or
/// as if it were a grid that is shaded at exact endpoints (position
/// i/(res+1)). In either case, derivatives will be set appropriately.
OSLEXECPUBLIC
bool
shade_image(ShadingSystem& shadingsys, ShaderGroup& group,
            const ShaderGlobals* defaultsg, OIIO::ImageBuf& buf,
            cspan<ustring> outputs,
            ShadeImageLocations shadelocations              = ShadePixelCenters,
            OIIO::ROI roi                                   = OIIO::ROI(),
            OIIO::parallel_options popt = 0);

#endif


OSL_NAMESPACE_EXIT
