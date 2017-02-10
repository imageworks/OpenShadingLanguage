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

#include <cmath>
#include <unordered_map>

#include <OpenImageIO/timer.h>
#include <OpenImageIO/sysutil.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/strutil.h>
#include <OpenImageIO/fmath.h>

#include "oslexec_pvt.h"
#include "../liboslcomp/oslcomp_pvt.h"
#include "backendllvm.h"
#ifndef OSL_LLVM_NO_BITCODE
#  include "llvm_ops_bc.h"
#endif

// Create extrenal declarations for all built-in funcs we may call from LLVM
#define DECL(name,signature) extern "C" void name();
#include "builtindecl.h"
#undef DECL


/*
This whole file is concerned with taking our post-optimized OSO
intermediate code and translating it into LLVM IR code so we can JIT it
and run it directly, for an expected huge speed gain over running our
interpreter.

Schematically, we want to create code that resembles the following:

    // Assume 2 layers. 
    struct GroupData_1 {
        // Array telling if we have already run each layer
        char layer_run[nlayers];
        // Array telling if we have already initialized each
        // needed user data (0 = haven't checked, 1 = checked and there
        // was no userdata, 2 = checked and there was userdata)
        char userdata_initialized[num_userdata];
        // All the user data slots, in order
        float userdata_s;
        float userdata_t;
        // For each layer in the group, we declare all shader params
        // whose values are not known -- they have init ops, or are
        // interpolated from the geom, or are connected to other layers.
        float param_0_foo;   // number is layer ID
        float param_1_bar;
    };

    // Name of layer entry is $layer_ID
    void $layer_0 (ShaderGlobals *sg, GroupData_1 *group)
    {
        // Declare locals, temps, constants, params with known values.
        // Make them all look like stack memory locations:
        float *x = alloca (sizeof(float));
        // ...and so on for all the other locals & temps...

        // then run the shader body:
        *x = sg->u * group->param_0_bar;
        group->param_1_foo = *x;
    }

    void $layer_1 (ShaderGlobals *sg, GroupData_1 *group)
    {
        // Because we need the outputs of layer 0 now, we call it if it
        // hasn't already run:
        if (! group->layer_run[0]) {
            group->layer_run[0] = 1;
            $layer_0 (sg, group);    // because we need its outputs
        }
        *y = sg->u * group->$param_1_bar;
    }

    void $group_1 (ShaderGlobals *sg, GroupData_1 *group)
    {
        group->layer_run[...] = 0;
        // Run just the unconditional layers

        if (! group->layer_run[1]) {
            group->layer_run[1] = 1;
            $layer_1 (sg, group);
        }
    }

*/


using namespace OSL::pvt;

OSL_NAMESPACE_ENTER

namespace pvt {

static spin_mutex llvm_mutex;

static ustring op_end("end");
static ustring op_nop("nop");
static ustring op_aassign("aassign");
static ustring op_compassign("compassign");
static ustring op_aref("aref");
static ustring op_compref("compref");
static ustring op_useparam("useparam");


struct HelperFuncRecord {
    const char *argtypes;
    void (*function)();
    HelperFuncRecord (const char *argtypes=NULL, void (*function)()=NULL)
        : argtypes(argtypes), function(function) {}
};

typedef std::unordered_map<std::string,HelperFuncRecord> HelperFuncMap;
HelperFuncMap llvm_helper_function_map;
atomic_int llvm_helper_function_map_initialized (0);
spin_mutex llvm_helper_function_map_mutex;
string_set external_function_names;



static void
initialize_llvm_helper_function_map ()
{
    if (llvm_helper_function_map_initialized)
        return;  // already done
    spin_lock lock (llvm_helper_function_map_mutex);
    if (llvm_helper_function_map_initialized)
        return;
#define DECL(name,signature) \
    llvm_helper_function_map[#name] = HelperFuncRecord(signature,name); \
    external_function_names.insert (#name);
#include "builtindecl.h"
#undef DECL

#ifdef OSL_SPLIT_BITCODES
    init_function_bytecodes();
#endif
    llvm_helper_function_map_initialized = 1;
}



void *
helper_function_lookup (const std::string &name)
{
    HelperFuncMap::const_iterator i = llvm_helper_function_map.find (name);
    if (i == llvm_helper_function_map.end())
        return NULL;
    return (void *) i->second.function;
}



llvm::Type *
BackendLLVM::llvm_type_sg ()
{
    // Create a type that defines the ShaderGlobals for LLVM IR.  This
    // absolutely MUST exactly match the ShaderGlobals struct in oslexec.h.
    if (m_llvm_type_sg)
        return m_llvm_type_sg;

    // Derivs look like arrays of 3 values
    llvm::Type *float_deriv = llvm_type (TypeDesc(TypeDesc::FLOAT, TypeDesc::SCALAR, 3));
    llvm::Type *triple_deriv = llvm_type (TypeDesc(TypeDesc::FLOAT, TypeDesc::VEC3, 3));
    std::vector<llvm::Type*> sg_types;
    sg_types.push_back (triple_deriv);      // P, dPdx, dPdy
    sg_types.push_back (ll.type_triple());  // dPdz
    sg_types.push_back (triple_deriv);      // I, dIdx, dIdy
    sg_types.push_back (ll.type_triple());  // N
    sg_types.push_back (ll.type_triple());  // Ng
    sg_types.push_back (float_deriv);       // u, dudx, dudy
    sg_types.push_back (float_deriv);       // v, dvdx, dvdy
    sg_types.push_back (ll.type_triple());  // dPdu
    sg_types.push_back (ll.type_triple());  // dPdv
    sg_types.push_back (ll.type_float());   // time
    sg_types.push_back (ll.type_float());   // dtime
    sg_types.push_back (ll.type_triple());  // dPdtime
    sg_types.push_back (triple_deriv);      // Ps

    llvm::Type *vp = (llvm::Type *)ll.type_void_ptr();
    sg_types.push_back(vp);                 // opaque renderstate*
    sg_types.push_back(vp);                 // opaque tracedata*
    sg_types.push_back(vp);                 // opaque objdata*
    sg_types.push_back(vp);                 // ShadingContext*
    sg_types.push_back(vp);                 // RendererServices*
    sg_types.push_back(vp);                 // object2common
    sg_types.push_back(vp);                 // shader2common
    sg_types.push_back(vp);                 // Ci

    sg_types.push_back (ll.type_float());   // surfacearea
    sg_types.push_back (ll.type_int());     // raytype
    sg_types.push_back (ll.type_int());     // flipHandedness
    sg_types.push_back (ll.type_int());     // backfacing

    return m_llvm_type_sg = ll.type_struct (sg_types, "ShaderGlobals");
}



llvm::Type *
BackendLLVM::llvm_type_sg_ptr ()
{
    return ll.type_ptr (llvm_type_sg());
}



llvm::Type *
BackendLLVM::llvm_type_groupdata ()
{
    // If already computed, return it
    if (m_llvm_type_groupdata)
        return m_llvm_type_groupdata;

    std::vector<llvm::Type*> fields;
    int offset = 0;
    int order = 0;

    if (llvm_debug() >= 2)
        std::cout << "Group param struct:\n";

    // First, add the array that tells if each layer has run.  But only make
    // slots for the layers that may be called/used.
    if (llvm_debug() >= 2)
        std::cout << "  layers run flags: " << m_num_used_layers
                  << " at offset " << offset << "\n";
    int sz = (m_num_used_layers + 3) & (~3);  // Round up to 32 bit boundary
    fields.push_back (ll.type_array (ll.type_bool(), sz));
    offset += sz * sizeof(bool);
    ++order;

    // Now add the array that tells which userdata have been initialized,
    // and the space for the userdata values.
    int nuserdata = (int) group().m_userdata_names.size();
    if (nuserdata) {
        if (llvm_debug() >= 2)
            std::cout << "  userdata initialized flags: " << nuserdata
                      << " at offset " << offset << ", field " << order << "\n";
        ustring *names = & group().m_userdata_names[0];
        TypeDesc *types = & group().m_userdata_types[0];
        int *offsets = & group().m_userdata_offsets[0];
        int sz = (nuserdata + 3) & (~3);
        fields.push_back (ll.type_array (ll.type_bool(), sz));
        offset += nuserdata * sizeof(bool);
        ++order;
        for (int i = 0; i < nuserdata; ++i) {
            TypeDesc type = types[i];
            int n = type.numelements() * 3;   // always make deriv room
            type.arraylen = n;
            fields.push_back (llvm_type (type));
            // Alignment
            int align = type.basesize();
            offset = OIIO::round_to_multiple_of_pow2 (offset, align);
            if (llvm_debug() >= 2)
                std::cout << "  userdata " << names[i] << ' ' << type
                          << ", field " << order << ", offset " << offset << "\n";
            offsets[i] = offset;
            offset += int(type.size());
            ++order;
        }
    }

    // For each layer in the group, add entries for all params that are
    // connected or interpolated, and output params.  Also mark those
    // symbols with their offset within the group struct.
    m_param_order_map.clear ();
    for (int layer = 0;  layer < group().nlayers();  ++layer) {
        ShaderInstance *inst = group()[layer];
        if (inst->unused())
            continue;
        FOREACH_PARAM (Symbol &sym, inst) {
            TypeSpec ts = sym.typespec();
            if (ts.is_structure())  // skip the struct symbol itself
                continue;
            const int arraylen = std::max (1, sym.typespec().arraylength());
            const int derivSize = (sym.has_derivs() ? 3 : 1);
            ts.make_array (arraylen * derivSize);
            fields.push_back (llvm_type (ts));

            // Alignment
            size_t align = sym.typespec().is_closure_based() ? sizeof(void*) :
                    sym.typespec().simpletype().basesize();
            if (offset & (align-1))
                offset += align - (offset & (align-1));
            if (llvm_debug() >= 2)
                std::cout << "  " << inst->layername() 
                          << " (" << inst->id() << ") " << sym.mangled()
                          << " " << ts.c_str() << ", field " << order 
                          << ", size " << derivSize * int(sym.size())
                          << ", offset " << offset << std::endl;
            sym.dataoffset ((int)offset);
            offset += derivSize* int(sym.size());

            m_param_order_map[&sym] = order;
            ++order;
        }
    }
    group().llvm_groupdata_size (offset);
    if (llvm_debug() >= 2)
        std::cout << " Group struct had " << order << " fields, total size "
                  << offset << "\n\n";

    std::string groupdataname = Strutil::format("Groupdata_%llu",
                                                (long long unsigned int)group().name().hash());
    m_llvm_type_groupdata = ll.type_struct (fields, groupdataname);

    return m_llvm_type_groupdata;
}



llvm::Type *
BackendLLVM::llvm_type_groupdata_ptr ()
{
    return ll.type_ptr (llvm_type_groupdata());
}



llvm::Type *
BackendLLVM::llvm_type_closure_component ()
{
    if (m_llvm_type_closure_component)
        return m_llvm_type_closure_component;

    std::vector<llvm::Type*> comp_types;
    comp_types.push_back (ll.type_int());     // id
    comp_types.push_back (ll.type_triple());  // w
    comp_types.push_back (ll.type_int());     // fake field for char mem[4]

    return m_llvm_type_closure_component = ll.type_struct (comp_types, "ClosureComponent");
}



llvm::Type *
BackendLLVM::llvm_type_closure_component_ptr ()
{
    return ll.type_ptr (llvm_type_closure_component());
}



void
BackendLLVM::llvm_assign_initial_value (const Symbol& sym)
{
    // Don't write over connections!  Connection values are written into
    // our layer when the earlier layer is run, as part of its code.  So
    // we just don't need to initialize it here at all.
    if (sym.valuesource() == Symbol::ConnectedVal &&
          !sym.typespec().is_closure_based())
        return;
    if (sym.typespec().is_closure_based() && sym.symtype() == SymTypeGlobal)
        return;

    int arraylen = std::max (1, sym.typespec().arraylength());

    // Closures need to get their storage before anything can be
    // assigned to them.  Unless they are params, in which case we took
    // care of it in the group entry point.
    if (sym.typespec().is_closure_based() &&
        sym.symtype() != SymTypeParam && sym.symtype() != SymTypeOutputParam) {
        llvm_assign_zero (sym);
        return;
    }

    if ((sym.symtype() == SymTypeLocal || sym.symtype() == SymTypeTemp)
          && shadingsys().debug_uninit()) {
        // Handle the "debug uninitialized values" case
        bool isarray = sym.typespec().is_array();
        int alen = isarray ? sym.typespec().arraylength() : 1;
        llvm::Value *u = NULL;
        if (sym.typespec().is_closure_based()) {
            // skip closures
        }
        else if (sym.typespec().is_floatbased())
            u = ll.constant (std::numeric_limits<float>::quiet_NaN());
        else if (sym.typespec().is_int_based())
            u = ll.constant (std::numeric_limits<int>::min());
        else if (sym.typespec().is_string_based())
            u = ll.constant (Strings::uninitialized_string);
        if (u) {
            for (int a = 0;  a < alen;  ++a) {
                llvm::Value *aval = isarray ? ll.constant(a) : NULL;
                for (int c = 0;  c < (int)sym.typespec().aggregate(); ++c)
                    llvm_store_value (u, sym, 0, aval, c);
            }
        }
        return;
    }

    if ((sym.symtype() == SymTypeLocal || sym.symtype() == SymTypeTemp) &&
        sym.typespec().is_string_based()) {
        // Strings are pointers.  Can't take any chance on leaving
        // local/tmp syms uninitialized.
        llvm_assign_zero (sym);
        return;  // we're done, the parts below are just for params
    }
    ASSERT_MSG (sym.symtype() == SymTypeParam || sym.symtype() == SymTypeOutputParam,
                "symtype was %d, data type was %s", (int)sym.symtype(), sym.typespec().c_str());

    // Handle interpolated params by calling osl_bind_interpolated_param,
    // which will check if userdata is already retrieved, if not it will
    // call RendererServices::get_userdata to retrived it. In either case,
    // it will return 1 if it put the userdata in the right spot (either
    // retrieved de novo or copied from a previous retrieval), or 0 if no
    // such userdata was available.
    llvm::BasicBlock *after_userdata_block = NULL;
    if (! sym.lockgeom() && ! sym.typespec().is_closure() && ! (sym.symtype() == SymTypeOutputParam)) {
        int userdata_index = -1;
        ustring symname = sym.name();
        TypeDesc type = sym.typespec().simpletype();
        for (int i = 0, e = (int)group().m_userdata_names.size(); i < e; ++i) {
            if (symname == group().m_userdata_names[i] &&
                    equivalent (type, group().m_userdata_types[i])) {
                userdata_index = i;
                break;
            }
        }
        ASSERT (userdata_index >= 0);
        std::vector<llvm::Value*> args;
        args.push_back (sg_void_ptr());
        args.push_back (ll.constant (symname));
        args.push_back (ll.constant (type));
        args.push_back (ll.constant ((int) group().m_userdata_derivs[userdata_index]));
        args.push_back (groupdata_field_ptr (2 + userdata_index)); // userdata data ptr
        args.push_back (ll.constant ((int) sym.has_derivs()));
        args.push_back (llvm_void_ptr (sym));
        args.push_back (ll.constant (sym.derivsize()));
        args.push_back (ll.void_ptr (userdata_initialized_ref(userdata_index)));
        args.push_back (ll.constant (userdata_index));
        llvm::Value *got_userdata =
            ll.call_function ("osl_bind_interpolated_param",
                              &args[0], args.size());
        if (shadingsys().debug_nan() && type.basetype == TypeDesc::FLOAT) {
            // check for NaN/Inf for float-based types
            int ncomps = type.numelements() * type.aggregate;
            llvm::Value *args[] = { ll.constant(ncomps), llvm_void_ptr(sym),
                 ll.constant((int)sym.has_derivs()), sg_void_ptr(),
                 ll.constant(ustring(inst()->shadername())),
                 ll.constant(0), ll.constant(sym.name()),
                 ll.constant(0), ll.constant(ncomps),
                 ll.constant("<get_userdata>")
            };
            ll.call_function ("osl_naninf_check", args, 10);
        }
        // We will enclose the subsequent initialization of default values
        // or init ops in an "if" so that the extra copies or code don't
        // happen if the userdata was retrieved.
        llvm::BasicBlock *no_userdata_block = ll.new_basic_block ("no_userdata");
        after_userdata_block = ll.new_basic_block ();
        llvm::Value *cond_val = ll.op_eq (got_userdata, ll.constant(0));
        ll.op_branch (cond_val, no_userdata_block, after_userdata_block);
    }

    if (sym.has_init_ops() && sym.valuesource() == Symbol::DefaultVal) {
        // Handle init ops.
        build_llvm_code (sym.initbegin(), sym.initend());
    } else if (! sym.lockgeom() && ! sym.typespec().is_closure()) {
        // geometrically-varying param; memcpy its default value
        TypeDesc t = sym.typespec().simpletype();
        ll.op_memcpy (llvm_void_ptr (sym), ll.constant_ptr (sym.data()),
                      t.size(), t.basesize() /*align*/);
        if (sym.has_derivs())
            llvm_zero_derivs (sym);
    } else {
        // Use default value
        int num_components = sym.typespec().simpletype().aggregate;
        TypeSpec elemtype = sym.typespec().elementtype();
        for (int a = 0, c = 0; a < arraylen;  ++a) {
            llvm::Value *arrind = sym.typespec().is_array() ? ll.constant(a) : NULL;
            if (sym.typespec().is_closure_based())
                continue;
            for (int i = 0; i < num_components; ++i, ++c) {
                // Fill in the constant val
                llvm::Value* init_val = 0;
                if (elemtype.is_floatbased())
                    init_val = ll.constant (((float*)sym.data())[c]);
                else if (elemtype.is_string())
                    init_val = ll.constant (((ustring*)sym.data())[c]);
                else if (elemtype.is_int())
                    init_val = ll.constant (((int*)sym.data())[c]);
                ASSERT (init_val);
                llvm_store_value (init_val, sym, 0, arrind, i);
            }
        }
        if (sym.has_derivs())
            llvm_zero_derivs (sym);
    }

    if (after_userdata_block) {
        // If we enclosed the default initialization in an "if", jump to the
        // next basic block now.
        ll.op_branch (after_userdata_block);
    }
}



void
BackendLLVM::llvm_generate_debugnan (const Opcode &op)
{
    for (int i = 0;  i < op.nargs();  ++i) {
        Symbol &sym (*opargsym (op, i));
        if (! op.argwrite(i))
            continue;
        TypeDesc t = sym.typespec().simpletype();
        if (t.basetype != TypeDesc::FLOAT)
            continue;  // just check float-based types
        llvm::Value *ncomps = ll.constant (int(t.numelements() * t.aggregate));
        llvm::Value *offset = ll.constant(0);
        llvm::Value *ncheck = ncomps;
        if (op.opname() == op_aassign) {
            // Special case -- array assignment -- only check one element
            ASSERT (i == 0 && "only arg 0 is written for aassign");
            llvm::Value *ind = llvm_load_value (*opargsym (op, 1));
            llvm::Value *agg = ll.constant(t.aggregate);
            offset = t.aggregate == 1 ? ind : ll.op_mul (ind, agg);
            ncheck = agg;
        } else if (op.opname() == op_compassign) {
            // Special case -- component assignment -- only check one channel
            ASSERT (i == 0 && "only arg 0 is written for compassign");
            llvm::Value *ind = llvm_load_value (*opargsym (op, 1));
            offset = ind;
            ncheck = ll.constant(1);
        }

        llvm::Value *args[] = { ncomps,
                                llvm_void_ptr(sym),
                                ll.constant((int)sym.has_derivs()),
                                sg_void_ptr(), 
                                ll.constant(op.sourcefile()),
                                ll.constant(op.sourceline()),
                                ll.constant(sym.name()),
                                offset,
                                ncheck,
                                ll.constant(op.opname())
                              };
        ll.call_function ("osl_naninf_check", args, 10);
    }
}



void
BackendLLVM::llvm_generate_debug_uninit (const Opcode &op)
{
    if (op.opname() == op_useparam) {
        // Don't check the args of a useparam before the op; they are by
        // definition potentially net yet set before the useparam action
        // itself puts values into them. Checking them for uninitialized
        // values will result in false positives.
        return;
    }
    for (int i = 0;  i < op.nargs();  ++i) {
        Symbol &sym (*opargsym (op, i));
        if (! op.argread(i))
            continue;
        if (sym.typespec().is_closure_based())
            continue;
        TypeDesc t = sym.typespec().simpletype();
        if (t.basetype != TypeDesc::FLOAT && t.basetype != TypeDesc::INT &&
            t.basetype != TypeDesc::STRING)
            continue;  // just check float, int, string based types
        llvm::Value *ncheck = ll.constant (int(t.numelements() * t.aggregate));
        llvm::Value *offset = ll.constant(0);
        // Some special cases...
        if (op.opname() == Strings::op_for && i == 0) {
            // The first argument of 'for' is the condition temp, but
            // note that it may not have had its initializer run yet, so
            // don't generate uninit test code for it.
            continue;
        }
        if (op.opname() == op_aref && i == 1) {
            // Special case -- array assignment -- only check one element
            llvm::Value *ind = llvm_load_value (*opargsym (op, 2));
            llvm::Value *agg = ll.constant(t.aggregate);
            offset = t.aggregate == 1 ? ind : ll.op_mul (ind, agg);
            ncheck = agg;
        } else if (op.opname() == op_compref && i == 1) {
            // Special case -- component assignment -- only check one channel
            llvm::Value *ind = llvm_load_value (*opargsym (op, 2));
            offset = ind;
            ncheck = ll.constant(1);
        }

        llvm::Value *args[] = { ll.constant(t),
                                llvm_void_ptr(sym),
                                sg_void_ptr(), 
                                ll.constant(op.sourcefile()),
                                ll.constant(op.sourceline()),
                                ll.constant(group().name()),
                                ll.constant(layer()),
                                ll.constant(inst()->layername()),
                                ll.constant(inst()->shadername().c_str()),
                                ll.constant(int(&op - &inst()->ops()[0])),
                                ll.constant(op.opname()),
                                ll.constant(i),
                                ll.constant(sym.name()),
                                offset,
                                ncheck
                              };
        ll.call_function ("osl_uninit_check", args, 15);
    }
}



void
BackendLLVM::llvm_generate_debug_op_printf (const Opcode &op)
{
    std::ostringstream msg;
    msg << op.sourcefile() << ':' << op.sourceline() << ' ' << op.opname();
    for (int i = 0;  i < op.nargs();  ++i)
        msg << ' ' << opargsym (op, i)->mangled();
    llvm_gen_debug_printf (msg.str());
}



bool
BackendLLVM::build_llvm_code (int beginop, int endop, llvm::BasicBlock *bb)
{
    if (bb)
        ll.set_insert_point (bb);

    for (int opnum = beginop;  opnum < endop;  ++opnum) {
        const Opcode& op = inst()->ops()[opnum];
        const OpDescriptor *opd = shadingsys().op_descriptor (op.opname());
        if (opd && opd->llvmgen) {
            if (shadingsys().debug_uninit() /* debug uninitialized vals */)
                llvm_generate_debug_uninit (op);
            if (shadingsys().llvm_debug_ops())
                llvm_generate_debug_op_printf (op);
            bool ok = (*opd->llvmgen) (*this, opnum);
            if (! ok)
                return false;
            if (shadingsys().debug_nan() /* debug NaN/Inf */
                && op.farthest_jump() < 0 /* Jumping ops don't need it */) {
                llvm_generate_debugnan (op);
            }
        } else if (op.opname() == op_nop ||
                   op.opname() == op_end) {
            // Skip this op, it does nothing...
        } else {
            shadingcontext()->error ("LLVMOSL: Unsupported op %s in layer %s\n",
                                     op.opname(), inst()->layername());
            return false;
        }

        // If the op we coded jumps around, skip past its recursive block
        // executions.
        int next = op.farthest_jump ();
        if (next >= 0)
            opnum = next-1;
    }
    return true;
}



llvm::Function*
BackendLLVM::build_llvm_init ()
{
    // Make a group init function: void group_init(ShaderGlobals*, GroupData*)
    // Note that the GroupData* is passed as a void*.
    std::string unique_name = Strutil::format ("group_%d_init", group().id());
    ll.current_function (
           ll.make_function (unique_name, false,
                             ll.type_void(), // return type
                             llvm_type_sg_ptr(), llvm_type_groupdata_ptr()));

    // Get shader globals and groupdata pointers
    m_llvm_shaderglobals_ptr = ll.current_function_arg(0); //arg_it++;
    m_llvm_groupdata_ptr = ll.current_function_arg(1); //arg_it++;

    // Set up a new IR builder
    llvm::BasicBlock *entry_bb = ll.new_basic_block (unique_name);
    ll.new_builder (entry_bb);
#if 0 /* helpful for debugging */
    if (llvm_debug()) {
        llvm_gen_debug_printf (Strutil::format("\n\n\n\nGROUP! %s",group().name()));
        llvm_gen_debug_printf ("enter group initlayer %d %s %s");                               this->layer(), inst()->layername(), inst()->shadername()));
    }
#endif

    // Group init clears all the "layer_run" and "userdata_initialized" flags.
    if (m_num_used_layers > 1) {
        int sz = (m_num_used_layers + 3) & (~3);  // round up to 32 bits
        ll.op_memset (ll.void_ptr(layer_run_ref(0)), 0, sz, 4 /*align*/);
    }
    int num_userdata = (int) group().m_userdata_names.size();
    if (num_userdata) {
        int sz = (num_userdata + 3) & (~3);  // round up to 32 bits
        ll.op_memset (ll.void_ptr(userdata_initialized_ref(0)), 0, sz, 4 /*align*/);
    }

    // Group init also needs to allot space for ALL layers' params
    // that are closures (to avoid weird order of layer eval problems).
    for (int i = 0;  i < group().nlayers();  ++i) {
        ShaderInstance *gi = group()[i];
        if (gi->unused() || gi->empty_instance())
            continue;
        FOREACH_PARAM (Symbol &sym, gi) {
           if (sym.typespec().is_closure_based()) {
                int arraylen = std::max (1, sym.typespec().arraylength());
                llvm::Value *val = ll.constant_ptr(NULL, ll.type_void_ptr());
                for (int a = 0; a < arraylen;  ++a) {
                    llvm::Value *arrind = sym.typespec().is_array() ? ll.constant(a) : NULL;
                    llvm_store_value (val, sym, 0, arrind, 0);
                }
            }
        }
    }


    // All done
#if 0 /* helpful for debugging */
    if (llvm_debug())
        llvm_gen_debug_printf (Strutil::format("exit group init %s",
                                               group().name());
#endif
    ll.op_return();

    if (llvm_debug())
        std::cout << "group init func (" << unique_name << ") "
                  << " after llvm  = " 
                  << ll.bitcode_string(ll.current_function()) << "\n";

    ll.end_builder();  // clear the builder

    return ll.current_function();
}



llvm::Function*
BackendLLVM::build_llvm_instance (bool groupentry)
{
    // Make a layer function: void layer_func(ShaderGlobals*, GroupData*)
    // Note that the GroupData* is passed as a void*.
    std::string unique_layer_name = Strutil::format ("%s_%d", inst()->layername(), inst()->id());

    bool is_entry_layer = group().is_entry_layer(layer());
    ll.current_function (
           ll.make_function (unique_layer_name,
                             !is_entry_layer, // fastcall for non-entry layer functions
                             ll.type_void(), // return type
                             llvm_type_sg_ptr(), llvm_type_groupdata_ptr()));

    // Get shader globals and groupdata pointers
    m_llvm_shaderglobals_ptr = ll.current_function_arg(0); //arg_it++;
    m_llvm_groupdata_ptr = ll.current_function_arg(1); //arg_it++;

    llvm::BasicBlock *entry_bb = ll.new_basic_block (unique_layer_name);
    m_exit_instance_block = NULL;

    // Set up a new IR builder
    ll.new_builder (entry_bb);

    llvm::Value *layerfield = layer_run_ref(layer_remap(layer()));
    if (is_entry_layer && ! group().is_last_layer(layer())) {
        // For entry layers, we need an extra check to see if it already
        // ran. If it has, do an early return. Otherwise, set the 'ran' flag
        // and then run the layer.
        if (shadingsys().llvm_debug_layers())
            llvm_gen_debug_printf (Strutil::format("checking for already-run layer %d %s %s",
                                   this->layer(), inst()->layername(), inst()->shadername()));
        llvm::Value *executed = ll.op_eq (ll.op_load (layerfield), ll.constant_bool(true));
        llvm::BasicBlock *then_block = ll.new_basic_block();
        llvm::BasicBlock *after_block = ll.new_basic_block();
        ll.op_branch (executed, then_block, after_block);
        // insert point is now then_block
        // we've already executed, so return early
        if (shadingsys().llvm_debug_layers())
            llvm_gen_debug_printf (Strutil::format("  taking early exit, already executed layer %d %s %s",
                                   this->layer(), inst()->layername(), inst()->shadername()));
        ll.op_return ();
        ll.set_insert_point (after_block);
    }

    if (shadingsys().llvm_debug_layers())
        llvm_gen_debug_printf (Strutil::format("enter layer %d %s %s",
                               this->layer(), inst()->layername(), inst()->shadername()));
    // Mark this layer as executed
    if (! group().is_last_layer(layer())) {
        ll.op_store (ll.constant_bool(true), layerfield);
        if (shadingsys().countlayerexecs())
            ll.call_function ("osl_incr_layers_executed", sg_void_ptr());
    }

    // Setup the symbols
    m_named_values.clear ();
    m_layers_already_run.clear ();
    for (auto&& s : inst()->symbols()) {
        // Skip constants -- we always inline scalar constants, and for
        // array constants we will just use the pointers to the copy of
        // the constant that belongs to the instance.
        if (s.symtype() == SymTypeConst)
            continue;
        // Skip structure placeholders
        if (s.typespec().is_structure())
            continue;
        // Allocate space for locals, temps, aggregate constants
        if (s.symtype() == SymTypeLocal || s.symtype() == SymTypeTemp ||
                s.symtype() == SymTypeConst)
            getOrAllocateLLVMSymbol (s);
        // Set initial value for constants, closures, and strings that are
        // not parameters.
        if (s.symtype() != SymTypeParam && s.symtype() != SymTypeOutputParam &&
            s.symtype() != SymTypeGlobal &&
            (s.is_constant() || s.typespec().is_closure_based() ||
             s.typespec().is_string_based() || 
             ((s.symtype() == SymTypeLocal || s.symtype() == SymTypeTemp)
              && shadingsys().debug_uninit())))
            llvm_assign_initial_value (s);
        // If debugnan is turned on, globals check that their values are ok
        if (s.symtype() == SymTypeGlobal && shadingsys().debug_nan()) {
            TypeDesc t = s.typespec().simpletype();
            if (t.basetype == TypeDesc::FLOAT) { // just check float-based types
                int ncomps = t.numelements() * t.aggregate;
                llvm::Value *args[] = { ll.constant(ncomps), llvm_void_ptr(s),
                     ll.constant((int)s.has_derivs()), sg_void_ptr(), 
                     ll.constant(ustring(inst()->shadername())),
                     ll.constant(0), ll.constant(s.name()),
                     ll.constant(0), ll.constant(ncomps),
                     ll.constant("<none>")
                };
                ll.call_function ("osl_naninf_check", args, 10);
            }
        }
    }
    // make a second pass for the parameters (which may make use of
    // locals and constants from the first pass)
    FOREACH_PARAM (Symbol &s, inst()) {
        // Skip structure placeholders
        if (s.typespec().is_structure())
            continue;
        // Skip if it's never read and isn't connected
        if (! s.everread() && ! s.connected_down() && ! s.connected()
              && ! s.renderer_output())
            continue;
        // Skip if it's an interpolated (userdata) parameter and we're
        // initializing them lazily.
        if (s.symtype() == SymTypeParam
                && ! s.lockgeom() && ! s.typespec().is_closure()
                && ! s.connected() && ! s.connected_down()
                && shadingsys().lazy_userdata())
            continue;
        // Set initial value for params (may contain init ops)
        llvm_assign_initial_value (s);
    }

    // All the symbols are stack allocated now.

    if (groupentry) {
        // Group entries also need to run any earlier layers that must be
        // run unconditionally. It's important that we do this AFTER all the
        // parameter initialization for this layer.
        for (int i = 0;  i < group().nlayers()-1;  ++i) {
            ShaderInstance *gi = group()[i];
            if (!gi->unused() && !gi->empty_instance() && !gi->run_lazily())
                llvm_call_layer (i, true /* unconditionally run */);
        }
    }

    // Mark all the basic blocks, including allocating llvm::BasicBlock
    // records for each.
    find_basic_blocks ();
    find_conditionals ();

    build_llvm_code (inst()->maincodebegin(), inst()->maincodeend());

    if (llvm_has_exit_instance_block())
        ll.op_branch (m_exit_instance_block); // also sets insert point

    // Transfer all of this layer's outputs into the downstream shader's
    // inputs.
    for (int layer = this->layer()+1;  layer < group().nlayers();  ++layer) {
        ShaderInstance *child = group()[layer];
        for (int c = 0;  c < child->nconnections();  ++c) {
            const Connection &con (child->connection (c));
            if (con.srclayer == this->layer()) {
                ASSERT (con.src.arrayindex == -1 && con.src.channel == -1 &&
                        con.dst.arrayindex == -1 && con.dst.channel == -1 &&
                        "no support for individual element/channel connection");
                Symbol *srcsym (inst()->symbol (con.src.param));
                Symbol *dstsym (child->symbol (con.dst.param));
                llvm_run_connected_layers (*srcsym, con.src.param);
                // FIXME -- I'm not sure I understand this.  Isn't this
                // unnecessary if we wrote to the parameter ourself?
                llvm_assign_impl (*dstsym, *srcsym);
            }
        }
    }
    // llvm_gen_debug_printf ("done copying connections");

    // All done
    if (shadingsys().llvm_debug_layers())
        llvm_gen_debug_printf (Strutil::format("exit layer %d %s %s",
                               this->layer(), inst()->layername(), inst()->shadername()));
    ll.op_return();

    if (llvm_debug())
        std::cout << "layer_func (" << unique_layer_name << ") "<< this->layer() 
                  << "/" << group().nlayers() << " after llvm  = " 
                  << ll.bitcode_string(ll.current_function()) << "\n";

    ll.end_builder();  // clear the builder

    return ll.current_function();
}



void
BackendLLVM::initialize_llvm_group ()
{
    ll.setup_optimization_passes (shadingsys().llvm_optimize());

    // Clear the shaderglobals and groupdata types -- they will be
    // created on demand.
    m_llvm_type_sg = NULL;
    m_llvm_type_groupdata = NULL;
    m_llvm_type_closure_component = NULL;

    initialize_llvm_helper_function_map();
    ll.InstallLazyFunctionCreator (helper_function_lookup);

    for (HelperFuncMap::iterator i = llvm_helper_function_map.begin(),
         e = llvm_helper_function_map.end(); i != e; ++i) {
        const char *funcname = i->first.c_str();
        bool varargs = false;
        const char *types = i->second.argtypes;
        int advance;
        TypeSpec rettype = OSLCompilerImpl::type_from_code (types, &advance);
        types += advance;
        std::vector<llvm::Type*> params;
        while (*types) {
            TypeSpec t = OSLCompilerImpl::type_from_code (types, &advance);
            if (t.simpletype().basetype == TypeDesc::UNKNOWN) {
                if (*types == '*')
                    varargs = true;
                else
                    ASSERT (0);
            } else {
                params.push_back (llvm_pass_type (t));
            }
            types += advance;
        }
        ll.make_function (funcname, false, llvm_type(rettype), params, varargs);
    }

    // Needed for closure setup
    std::vector<llvm::Type*> params(3);
    params[0] = (llvm::Type *) ll.type_char_ptr();
    params[1] = ll.type_int();
    params[2] = (llvm::Type *) ll.type_char_ptr();
    m_llvm_type_prepare_closure_func = ll.type_function_ptr (ll.type_void(), params);
    m_llvm_type_setup_closure_func = m_llvm_type_prepare_closure_func;
}



static void empty_group_func (void*, void*)
{
}



void
BackendLLVM::run ()
{
    if (group().does_nothing()) {
        group().llvm_compiled_init ((RunLLVMGroupFunc)empty_group_func);
        group().llvm_compiled_version ((RunLLVMGroupFunc)empty_group_func);
        return;
    }

    // At this point, we already hold the lock for this group, by virtue
    // of ShadingSystemImpl::optimize_group.
    OIIO::Timer timer;
    std::string err;

    {
#ifdef OSL_LLVM_NO_BITCODE
    // I don't know which exact part has thread safety issues, but it
    // crashes on windows when we don't lock.
    // FIXME -- try subsequent LLVM releases on Windows to see if this
    // is a problem that is eventually fixed on the LLVM side.
    static spin_mutex mutex;
    OIIO::spin_lock lock (mutex);
#endif

#ifdef OSL_LLVM_NO_BITCODE
    ll.module (ll.new_module ("llvm_ops"));
#else
    ll.module (ll.module_from_bitcode (osl_llvm_compiled_ops_block,
                                       osl_llvm_compiled_ops_size,
                                       "llvm_ops", &err));
    if (err.length())
        shadingcontext()->error ("ParseBitcodeFile returned '%s'\n", err.c_str());
    ASSERT (ll.module());
#endif

    // Create the ExecutionEngine
    if (! ll.make_jit_execengine (&err)) {
        shadingcontext()->error ("Failed to create engine: %s\n", err.c_str());
        ASSERT (0);
        return;
    }

    // End of mutex lock, for the OSL_LLVM_NO_BITCODE case
    }

    m_stat_llvm_setup_time += timer.lap();

    // Set up m_num_used_layers to be the number of layers that are
    // actually used, and m_layer_remap[] to map original layer numbers
    // to the shorter list of actually-called layers. We also note that
    // if m_layer_remap[i] is < 0, it's not a layer that's used.
    int nlayers = group().nlayers();
    m_layer_remap.resize (nlayers, -1);
    m_num_used_layers = 0;
    if (debug() >= 1)
        std::cout << "\nLayers used: (group " << group().name() << ")\n";
    for (int layer = 0;  layer < nlayers;  ++layer) {
        // Skip unused or empty layers, unless they are callable entry
        // points.
        ShaderInstance *inst = group()[layer];
        bool is_single_entry = (layer == (nlayers-1) && group().num_entry_layers() == 0);
        if (inst->entry_layer() || is_single_entry ||
            (! inst->unused() && !inst->empty_instance())) {
            if (debug() >= 1)
                std::cout << "  " << layer << ' ' << inst->layername() << "\n";
            m_layer_remap[layer] = m_num_used_layers++;
        }
    }
    shadingsys().m_stat_empty_instances += nlayers - m_num_used_layers;

    initialize_llvm_group ();

    // Generate the LLVM IR for each layer.  Skip unused layers.
    m_llvm_local_mem = 0;
    llvm::Function* init_func = build_llvm_init ();
    std::vector<llvm::Function*> funcs (nlayers, NULL);
    for (int layer = 0; layer < nlayers; ++layer) {
        set_inst (layer);
        if (m_layer_remap[layer] != -1) {
            // If no entry points were specified, the last layer is special,
            // it's the single entry point for the whole group.
            bool is_single_entry = (layer == (nlayers-1) && group().num_entry_layers() == 0);
            funcs[layer] = build_llvm_instance (is_single_entry);
        }
    }
    // llvm::Function* entry_func = group().num_entry_layers() ? NULL : funcs[m_num_used_layers-1];
    m_stat_llvm_irgen_time += timer.lap();

    if (shadingsys().m_max_local_mem_KB &&
        m_llvm_local_mem/1024 > shadingsys().m_max_local_mem_KB) {
        shadingcontext()->error ("Shader group \"%s\" needs too much local storage: %d KB",
                                 group().name(), m_llvm_local_mem/1024);
    }

    // The module contains tons of "library" functions that our generated
    // IR might call. But probably not. We don't want to incur the overhead
    // of fully compiling those, so we tell LLVM_Util to turn them into
    // non-externally-visible symbols (allowing them to be discarded if not
    // used internal to the module). We need to make exceptions for our
    // entry points, as well as for all the external functions that are
    // just declarations (not definitions) in the module (which we have
    // conveniently stashed in external_function_names).
    string_set entry_function_names;
    entry_function_names.insert (ll.func_name(init_func));
    for (int layer = 0; layer < nlayers; ++layer) {
        // set_inst (layer);
        llvm::Function* f = funcs[layer];
        if (f && group().is_entry_layer(layer))
            entry_function_names.insert (ll.func_name(f));
    }
    ll.internalize_module_functions ("osl_", external_function_names, entry_function_names);

    // Optimize the LLVM IR unless it's a do-nothing group.
    if (! group().does_nothing())
        ll.do_optimize();

    m_stat_llvm_opt_time += timer.lap();

    if (llvm_debug()) {
        for (int layer = 0; layer < nlayers; ++layer)
            if (funcs[layer])
                std::cout << "func after opt  = " << ll.bitcode_string (funcs[layer]) << "\n";
        std::cout.flush();
    }

    // Debug code to dump the resulting bitcode to a file
    if (llvm_debug() >= 2) {
        std::string name = Strutil::format ("%s_%d.bc", inst()->layername(),
                                            inst()->id());
        ll.write_bitcode_file (name.c_str());
    }

    // Force the JIT to happen now and retrieve the JITed function pointers
    // for the initialization and all public entry points.
    group().llvm_compiled_init ((RunLLVMGroupFunc) ll.getPointerToFunction(init_func));
    for (int layer = 0; layer < nlayers; ++layer) {
        llvm::Function* f = funcs[layer];
        if (f && group().is_entry_layer (layer))
            group().llvm_compiled_layer (layer, (RunLLVMGroupFunc) ll.getPointerToFunction(f));
    }
    if (group().num_entry_layers())
        group().llvm_compiled_version (NULL);
    else
        group().llvm_compiled_version (group().llvm_compiled_layer(nlayers-1));

    // Remove the IR for the group layer functions, we've already JITed it
    // and will never need the IR again.  This saves memory, and also saves
    // a huge amount of time since we won't re-optimize it again and again
    // if we keep adding new shader groups to the same Module.
    for (int i = 0; i < nlayers; ++i) {
        if (funcs[i])
            ll.delete_func_body (funcs[i]);
    }
    ll.delete_func_body (init_func);

    // Free the exec and module to reclaim all the memory.  This definitely
    // saves memory, and has almost no effect on runtime.
    ll.execengine (NULL);

    // N.B. Destroying the EE should have destroyed the module as well.
    ll.module (NULL);

    m_stat_llvm_jit_time += timer.lap();

    m_stat_total_llvm_time = timer();

    if (shadingsys().m_compile_report) {
        shadingcontext()->info ("JITed shader group %s:", group().name());
        shadingcontext()->info ("    (%1.2fs = %1.2f setup, %1.2f ir, %1.2f opt, %1.2f jit; local mem %dKB)",
                           m_stat_total_llvm_time, 
                           m_stat_llvm_setup_time,
                           m_stat_llvm_irgen_time, m_stat_llvm_opt_time,
                           m_stat_llvm_jit_time,
                           m_llvm_local_mem/1024);
    }
}



}; // namespace pvt
OSL_NAMESPACE_EXIT
