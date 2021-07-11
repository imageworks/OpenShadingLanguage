// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#include <string>
#include <vector>

#include "oslcomp_pvt.h"

#include <OpenImageIO/strutil.h>
namespace Strutil = OIIO::Strutil;


OSL_NAMESPACE_ENTER

namespace pvt {  // OSL::pvt


TypeSpec
ASTNode::typecheck(TypeSpec expected)
{
    typecheck_children(expected);
    if (m_typespec == TypeSpec())
        m_typespec = expected;
    return m_typespec;
}



void
ASTNode::typecheck_children(TypeSpec expected)
{
    for (auto&& c : m_children) {
        typecheck_list(c, expected);
    }
}



TypeSpec
ASTNode::typecheck_list(ref node, TypeSpec expected)
{
    TypeSpec t;
    while (node) {
        t    = node->typecheck(expected);
        node = node->next();
    }
    return t;
}



TypeSpec
ASTfunction_declaration::typecheck(TypeSpec expected)
{
    // Typecheck the args, remember to push/pop the function so that the
    // typechecking for 'return' will know which function it belongs to.
    m_compiler->push_function(func());
    typecheck_children(expected);
    m_compiler->pop_function();
    if (m_typespec == TypeSpec())
        m_typespec = expected;
    return m_typespec;
}



TypeSpec ASTvariable_declaration::typecheck(TypeSpec /*expected*/)
{
    typecheck_children(m_typespec);
    ASTNode* init = this->init().get();

    if (!init)
        return m_typespec;

    const TypeSpec& vt = m_typespec;        // Type of the variable
    const TypeSpec& et = init->typespec();  // Type of expression assigned

    if (m_typespec.is_structure() && !m_initlist
        && et.structure() != vt.structure()) {
        // Can't do:  struct foo = 1
        errorf("Cannot initialize %s %s = %s", vt, name(), et);
        return m_typespec;
    }

    // If it's a compound initializer, the rest of the type checking of the
    // individual piece would have already been checked in the child's
    // typecheck method.
    if (init->nodetype() == compound_initializer_node) {
        if (init->nextptr())
            errorf("compound_initializer should be the only initializer");
        init = ((ASTcompound_initializer*)init)->initlist().get();
        if (!init)
            return m_typespec;
    }

    // Special case: ok to assign a literal 0 to a closure to
    // initialize it.
    if (vt.is_closure() && !et.is_closure() && (et.is_float() || et.is_int())
        && init->nodetype() == literal_node
        && ((ASTliteral*)&(*init))->floatval() == 0.0f) {
        return m_typespec;  // it's ok
    }

    // Expression must be of a type assignable to the lvalue
    if (!assignable(vt, et)) {
        // Special case: for int=float, it's just a warning.
        if (vt.is_int() && et.is_float())
            warningf("Assignment may lose precision: %s %s = %s", vt, name(),
                     et);
        else
            errorf("Cannot assign %s %s = %s", vt, name(), et);
        return m_typespec;
    }

    return m_typespec;
}



TypeSpec ASTvariable_ref::typecheck(TypeSpec /*expected*/)
{
    m_is_lvalue = true;  // A var ref is an lvalue
    return m_typespec;
}



TypeSpec ASTpreincdec::typecheck(TypeSpec /*expected*/)
{
    typecheck_children();
    m_is_lvalue = var()->is_lvalue();
    m_typespec  = var()->typespec();
    return m_typespec;
}



TypeSpec ASTpostincdec::typecheck(TypeSpec /*expected*/)
{
    typecheck_children();
    if (!var()->is_lvalue())
        errorf("%s can only be applied to an lvalue", nodetypename());
    m_is_lvalue = false;
    m_typespec  = var()->typespec();
    return m_typespec;
}



TypeSpec ASTindex::typecheck(TypeSpec /*expected*/)
{
    typecheck_children();
    const char* indextype = "";
    TypeSpec t            = lvalue()->typespec();
    if (t.is_structure()) {
        errorf("Cannot use [] indexing on a struct");
        return TypeSpec();
    }
    if (t.is_closure()) {
        errorf("Cannot use [] indexing on a closure");
        return TypeSpec();
    }
    if (index3()) {
        if (!t.is_array() && !t.elementtype().is_matrix())
            errorf("[][][] only valid for a matrix array");
        m_typespec = TypeDesc::FLOAT;
    } else if (t.is_array()) {
        indextype  = "array";
        m_typespec = t.elementtype();
        if (index2()) {
            if (t.aggregate() == TypeDesc::SCALAR)
                errorf("can't use [][] on a simple array");
            m_typespec = TypeDesc::FLOAT;
        }
    } else if (t.aggregate() == TypeDesc::VEC3) {
        indextype         = "component";
        TypeDesc tnew     = t.simpletype();
        tnew.aggregate    = TypeDesc::SCALAR;
        tnew.vecsemantics = TypeDesc::NOXFORM;
        m_typespec        = tnew;
        if (index2())
            errorf("can't use [][] on a %s", t);
    } else if (t.aggregate() == TypeDesc::MATRIX44) {
        indextype         = "component";
        TypeDesc tnew     = t.simpletype();
        tnew.aggregate    = TypeDesc::SCALAR;
        tnew.vecsemantics = TypeDesc::NOXFORM;
        m_typespec        = tnew;
        if (!index2())
            errorf("must use [][] on a matrix, not just []");
    } else {
        errorf("can only use [] indexing for arrays or multi-component types");
        return TypeSpec();
    }

    // Make sure the indices (children 1+) are integers
    for (size_t c = 1; c < nchildren(); ++c)
        if (!child(c)->typespec().is_int())
            errorf("%s index must be an integer, not a %s", indextype,
                   index()->typespec());

    // If the thing we're indexing is an lvalue, so is the indexed element
    m_is_lvalue = lvalue()->is_lvalue();

    return m_typespec;
}



TypeSpec
ASTstructselect::typecheck(TypeSpec expected)
{
    if (compindex()) {
        // Redirected codegen to ASTIndex for named component (e.g., point.x)
        return compindex()->typecheck(expected);
    }

    // The ctr already figured out if this was a valid structure selection
    if (m_fieldid < 0 || m_fieldsym == NULL) {
        return TypeSpec();
    }

    typecheck_children();
    StructSpec* structspec(TypeSpec::structspec(m_structid));
    m_typespec  = structspec->field(m_fieldid).type;
    m_is_lvalue = lvalue()->is_lvalue();
    return m_typespec;
}



TypeSpec ASTconditional_statement::typecheck(TypeSpec /*expected*/)
{
    typecheck_list(cond());
    m_compiler->push_nesting(false);
    typecheck_list(truestmt());
    typecheck_list(falsestmt());
    m_compiler->pop_nesting(false);

    TypeSpec c = cond()->typespec();
    if (c.is_structure())
        errorf("Cannot use a struct as an 'if' condition");
    if (c.is_array())
        errorf("Cannot use an array as an 'if' condition");
    return m_typespec = TypeDesc(TypeDesc::NONE);
}



TypeSpec ASTloop_statement::typecheck(TypeSpec /*expected*/)
{
    typecheck_list(init());
    m_compiler->push_nesting(true);
    typecheck_list(cond());
    typecheck_list(iter());
    typecheck_list(stmt());
    m_compiler->pop_nesting(true);

    TypeSpec c = cond()->typespec();
    if (c.is_closure())
        errorf("Cannot use a closure as an '%s' condition", opname());
    if (c.is_structure())
        errorf("Cannot use a struct as an '%s' condition", opname());
    if (c.is_array())
        errorf("Cannot use an array as an '%s' condition", opname());
    return m_typespec = TypeDesc(TypeDesc::NONE);
}



TypeSpec ASTloopmod_statement::typecheck(TypeSpec /*expected*/)
{
    if (m_compiler->nesting_level(true /*loops*/) < 1)
        errorf("Cannot '%s' here -- not inside a loop.", opname());
    return m_typespec = TypeDesc(TypeDesc::NONE);
}



TypeSpec ASTassign_expression::typecheck(TypeSpec /*expected*/)
{
    TypeSpec vt = var()->typecheck();     // Type of the variable
    TypeSpec et = expr()->typecheck(vt);  // Type of the expression assigned
    m_typespec  = vt;

    if (!var()->is_lvalue()) {
        errorf("Can't assign via %s to something that isn't an lvalue",
               opname());
        return TypeSpec();
    }

    OSL_DASSERT(m_op == Assign);  // all else handled by binary_op
    ustring varname;
    if (var()->nodetype() == variable_ref_node)
        varname = ((ASTvariable_ref*)var().get())->name();

    // Handle array case
    if (vt.is_array() || et.is_array()) {
        if (vt.is_array() && et.is_array()
            && vt.arraylength() >= et.arraylength()) {
            if (vt.structure() && (vt.structure() == et.structure())) {
                return m_typespec;
            }
            if (equivalent(vt.elementtype(), et.elementtype())
                && !vt.structure() && !et.structure()) {
                return m_typespec;
            }
        }
        errorf("Cannot assign %s %s = %s", vt, varname, et);
        return m_typespec;
    }

    // Special case: ok to assign a literal 0 to a closure to
    // initialize it.
    if (vt.is_closure() && !et.is_closure() && (et.is_float() || et.is_int())
        && expr()->nodetype() == literal_node
        && ((ASTliteral*)&(*expr()))->floatval() == 0.0f) {
        return m_typespec;  // it's ok
    }

    // If either argument is a structure, they better both be the same
    // exact kind of structure.
    if (vt.is_structure() || et.is_structure()) {
        int vts = vt.structure(), ets = et.structure();
        if (vts == ets)
            return m_typespec = vt;
        // Otherwise, a structure mismatch
        errorf("Cannot assign %s %s = %s", vt, varname, et);
        return m_typespec;
    }

    // Expression must be of a type assignable to the lvalue
    if (!assignable(vt, et)) {
        // Special case: for int=float, it's just a warning.
        if (vt.is_int() && et.is_float())
            warningf("Assignment may lose precision: %s %s = %s", vt, varname,
                     et);
        else
            errorf("Cannot assign %s %s = %s", vt, varname, et);
        return m_typespec;
    }

    return m_typespec;
}



TypeSpec ASTreturn_statement::typecheck(TypeSpec /*expected*/)
{
    FunctionSymbol* myfunc = m_compiler->current_function();
    if (myfunc) {
        // If it's a user function (as opposed to a main shader body)...
        if (expr()) {
            // If we are returning a value, it must be assignable to the
            // kind of type the function actually returns.  This check
            // will also catch returning a value from a void function.
            TypeSpec et = expr()->typecheck(myfunc->typespec());
            if (!assignable(myfunc->typespec(), et)) {
                errorf("Cannot return a '%s' from '%s %s()'", et,
                       myfunc->typespec(), myfunc->name());
            }
        } else {
            // If we are not returning a value, it must be a void function.
            if (!myfunc->typespec().is_void())
                errorf("You must return a '%s' from function '%s'",
                       myfunc->typespec(), myfunc->name());
        }
        myfunc->encountered_return();
    } else {
        // We're not part of any user function, so this 'return' must
        // be from the main shader body.  That's fine (it's equivalent
        // to calling exit()), but it can't return a value.
        if (expr())
            errorf("Cannot return a value from a shader body");
    }
    return TypeSpec();  // TODO: what should be returned here?
}



TypeSpec
ASTunary_expression::typecheck(TypeSpec expected)
{
    typecheck_children(expected);
    TypeSpec t = expr()->typespec();

    if (m_function_overload) {
        // There was a function with the special name. See if the types
        // match.
        for (FunctionSymbol* f = m_function_overload; f; f = f->nextpoly()) {
            const char* code = f->argcodes().c_str();
            int advance;
            TypeSpec returntype = m_compiler->type_from_code(code, &advance);
            code += advance;
            if (code[0] && check_simple_arg(t, code, true) && !code[0]) {
                return m_typespec = returntype;
            }
        }
        // No match, so forget about the potentially overloaded function
        m_function_overload = nullptr;
    }

    if (t.is_structure() || t.is_array()) {
        errorf("Can't do '%s' to a %s.", opname(), t);
        return TypeSpec();
    }
    switch (m_op) {
    case Sub:
    case Add:
        if (!(t.is_closure() || t.is_numeric())) {
            errorf("Can't do '%s' to a %s.", opname(), t);
            return TypeSpec();
        }
        m_typespec = t;
        break;
    case Not:
        m_typespec = TypeDesc::TypeInt;  // ! is always an int
        break;
    case Compl:
        if (!t.is_int()) {
            errorf("Operator '~' can only be done to an int");
            return TypeSpec();
        }
        m_typespec = t;
        break;
    default: errorf("unknown unary operator");
    }
    return m_typespec;
}



/// Given two types (which are already compatible for numeric ops),
/// return which one has "more precision".  Let's say the op is '+'.  So
/// hp(int,float) == float, hp(vector,float) == vector, etc.
inline TypeDesc
higherprecision(const TypeDesc& a, const TypeDesc& b)
{
    // Aggregate always beats non-aggregate
    if (a.aggregate > b.aggregate)
        return a;
    else if (b.aggregate > a.aggregate)
        return b;
    // Float beats int
    if (b.basetype == TypeDesc::FLOAT)
        return b;
    else
        return a;
}



TypeSpec
ASTbinary_expression::typecheck(TypeSpec expected)
{
    typecheck_children(expected);
    TypeSpec l = left()->typespec();
    TypeSpec r = right()->typespec();

    if (m_function_overload) {
        // There was a function with the special name. See if the types
        // match.
        for (FunctionSymbol* f = m_function_overload; f; f = f->nextpoly()) {
            const char* code = f->argcodes().c_str();
            int advance;
            TypeSpec returntype = m_compiler->type_from_code(code, &advance);
            code += advance;
            if (code[0] && check_simple_arg(l, code, true) && code[0]
                && check_simple_arg(r, code, true) && !code[0]) {
                return m_typespec = returntype;
            }
        }
        // No match, so forget about the potentially overloaded function
        m_function_overload = nullptr;
    }

    // No binary ops work on structs or arrays
    if (l.is_structure() || r.is_structure() || l.is_array() || r.is_array()) {
        errorf("Not allowed: '%s %s %s'", l, opname(), r);
        return TypeSpec();
    }

    // Special for closures -- just a few cases to worry about
    if (l.is_color_closure() || r.is_color_closure()) {
        if (m_op == Add) {
            if (l.is_color_closure() && r.is_color_closure())
                return m_typespec = l;
        }
        if (m_op == Mul) {
            if (l.is_color_closure() != r.is_color_closure()) {
                if (l.is_color_closure()
                    && (r.is_color() || r.is_int_or_float()))
                    return m_typespec = l;
                if (r.is_color_closure()
                    && (l.is_color() || l.is_int_or_float())) {
                    // N.B. Reorder so that it's always r = closure * k,
                    // not r = k * closure.  See codegen for why this helps.
                    std::swap(m_children[0], m_children[1]);
                    return m_typespec = r;
                }
            }
        }
        if (m_op == And || m_op == Or) {
            // Logical ops work can work on closures (since they test
            // for nonemptiness, but always return int.
            return m_typespec = TypeDesc::TypeInt;
        }
        // If we got this far, it's an op that's not allowed
        errorf("Not allowed: '%s %s %s'", l, opname(), r);
        return TypeSpec();
    }

    switch (m_op) {
    case Sub:
    case Add:
    case Mul:
    case Div:
        // Add/Sub/Mul/Div work for any equivalent types, and
        // combination of int/float and other numeric types, but do not
        // work with strings.  Add/Sub don't work with matrices, but
        // Mul/Div do.
        if (l.is_string() || r.is_string())
            break;  // Dispense with strings trivially
        if ((m_op == Sub || m_op == Add) && (l.is_matrix() || r.is_matrix()))
            break;  // Matrices don't combine for + and -
        if (equivalent(l, r)) {
            // handle a few couple special cases
            if (m_op == Sub && l.is_point() && r.is_point())  // p-p == v
                return m_typespec = TypeDesc::TypeVector;
            if ((m_op == Add || m_op == Sub)
                && (l.is_point() || r.is_point()))  // p +/- v, v +/- p == p
                return m_typespec = TypeDesc::TypePoint;
            // everything else: the first operand is also the return type
            return m_typespec = l;
        }
        if ((l.is_numeric() && r.is_int_or_float())
            || (l.is_int_or_float() && r.is_numeric()))
            return m_typespec = higherprecision(l.simpletype(), r.simpletype());
        break;

    case Mod:
        // Mod only works with ints, and return ints.
        if (l.is_int() && r.is_int())
            return m_typespec = TypeDesc::TypeInt;
        break;

    case Equal:
    case NotEqual:
        // Any equivalent types can be compared with == and !=, also a
        // float or int can be compared to any other numeric type.
        // Result is always an int.
        if (equivalent(l, r) || (l.is_numeric() && r.is_int_or_float())
            || (l.is_int_or_float() && r.is_numeric()))
            return m_typespec = TypeDesc::TypeInt;
        break;

    case Greater:
    case Less:
    case GreaterEqual:
    case LessEqual:
        // G/L comparisons only work with floats or ints, and always
        // return int.
        if (l.is_int_or_float() && r.is_int_or_float())
            return m_typespec = TypeDesc::TypeInt;
        break;

    case BitAnd:
    case BitOr:
    case Xor:
    case ShiftLeft:
    case ShiftRight:
        // Bitwise ops only work with ints, and return ints.
        if (l.is_int() && r.is_int())
            return m_typespec = TypeDesc::TypeInt;
        break;

    case And:
    case Or:
        // Logical ops work on any simple type (since they test for
        // nonzeroness), but always return int.
        return m_typespec = TypeDesc::TypeInt;

    default: errorf("unknown binary operator");
    }

    // If we got this far, it's an op that's not allowed
    errorf("Not allowed: '%s %s %s'", l, opname(), r);
    return TypeSpec();
}



TypeSpec
ASTternary_expression::typecheck(TypeSpec expected)
{
    // FIXME - closures
    TypeSpec c = typecheck_list(cond(), TypeDesc::TypeInt);
    TypeSpec t = typecheck_list(trueexpr(), expected);
    TypeSpec f = typecheck_list(falseexpr(), expected);

    if (c.is_closure())
        errorf("Cannot use a closure as a condition");
    if (c.is_structure())
        errorf("Cannot use a struct as a condition");
    if (c.is_array())
        errorf("Cannot use an array as a condition");

    // No arrays
    if (t.is_array() || t.is_array()) {
        errorf("Not allowed: '%s ? %s : %s'", c, t, f);
        return TypeSpec();
    }

    // The true and false clauses need to be equivalent types, or one
    // needs to be assignable to the other (so one can be upcast).
    if (assignable(t, f) || assignable(f, t))
        m_typespec = higherprecision(t.simpletype(), f.simpletype());
    else
        errorf("Not allowed: '%s ? %s : %s'", c, t, f);

    return m_typespec;
}



TypeSpec
ASTcomma_operator::typecheck(TypeSpec expected)
{
    return m_typespec = typecheck_list(expr(), expected);
    // N.B. typecheck_list already returns the type of the LAST node in
    // the list, just like the comma operator is supposed to do.
}



TypeSpec ASTtypecast_expression::typecheck(TypeSpec /*expected*/)
{
    // FIXME - closures
    typecheck_children(m_typespec);
    TypeSpec t = expr()->typespec();
    if (!assignable(m_typespec, t) && !(m_typespec.is_int() && t.is_float())
        &&  // (int)float is ok
        !(m_typespec.is_triple() && t.is_triple()))
        errorf("Cannot cast '%s' to '%s'", t, m_typespec);
    return m_typespec;
}



TypeSpec
ASTtype_constructor::typecheck(TypeSpec expected, bool report, bool bind)
{
    // Hijack the usual function arg-checking routines.
    // So we have a set of valid patterns for each type constructor:
    static const char* float_patterns[]  = { "ff", "fi", NULL };
    static const char* triple_patterns[] = { "cf", "cfff", "csfff", "cc",
                                             "cp", "cv",   "cn",    NULL };
    static const char* matrix_patterns[]
        = { "mf", "msf", "mss", "mffffffffffffffff", "msffffffffffffffff",
            "mm", NULL };
    static const char* int_patterns[] = { "if", "ii", NULL };
    // Select the pattern for the type of constructor we are...
    const char** patterns = NULL;
    TypeSpec argexpected;  // default to unknown
    if (expected.is_float()) {
        patterns    = float_patterns;
        argexpected = TypeDesc::FLOAT;
        // ^^^ Since simetimes tht constructor `float(expr)` is used as a
        // synonym for a cast `(float)expr`, we know that ambiguously typed
        // expressions should favor disambiguating to a float.
    } else if (expected.is_triple()) {
        patterns = triple_patterns;
        // For triples, the constructor that takes just one argument is often
        // is used as a typecast, i.e. (vector)foo <==> vector(foo)
        // So pass on the expected type so it can resolve polymorphism in
        // the expected way. Similarly, the three-argument triple constructor
        // can infer that all three arguments should disambiguate to float.
        if (listlength(args()) == 1)
            argexpected = expected;
        else
            argexpected = TypeDesc::FLOAT;
    } else if (expected.is_matrix()) {
        patterns = matrix_patterns;
    } else if (expected.is_int()) {
        patterns = int_patterns;
    } else {
        if (report)
            errorf("Cannot construct type '%s'", expected);
        return TypeSpec();
    }

    typecheck_children(argexpected);

    // Try to get a match, first without type coercion of the arguments,
    // then with coercion.
    for (int co = 0; co < 2; ++co) {
        bool coerce = co;
        for (const char** pat = patterns; *pat; ++pat) {
            const char* code = *pat;
            if (check_arglist(type_c_str(expected), args(), code + 1, coerce,
                              bind))
                return expected;
        }
    }

    // If we made it this far, no match could be found.
    if (report) {
        std::string err = OIIO::Strutil::sprintf("Cannot construct %s (",
                                                 expected);
        for (ref a = args(); a; a = a->next()) {
            err += a->typespec().string();
            if (a->next())
                err += ", ";
        }
        err += ")";
        errorf("%s", err);
        // FIXME -- it might be nice here to enumerate for the user all the
        // valid combinations.
    }
    return TypeSpec();
}


class ASTcompound_initializer::TypeAdjuster {
public:
    // It is legal to have an incomplete initializer list in some contexts:
    //   struct custom { float x, y, z, w };
    //   custom c = { 0, 1 };
    //   color c[3] = { {1}, {2} };
    //
    // Others (function calls) should initialize all elements to avoid ambiguity
    //   color subproc(color a, color b)
    //   subproc({0, 1}, {2, 3}) -> error, otherwise there may be subtle changes
    //                              in behaviour if overloads added later.
    enum Strictness {
        default_flags = 0,
        no_errors     = 1,       /// Don't report errors in typecheck calls
        must_init_all = 1 << 1,  /// All fields/elements must be inited
        function_arg  = no_errors | must_init_all
    };

    TypeAdjuster(OSLCompilerImpl* c, unsigned m = default_flags)
        : m_compiler(c)
        , m_mode(Strictness(m))
        , m_success(true)
        , m_debug_successful(false)
    {
    }

    ~TypeAdjuster()
    {
        // Commit infered types of all ASTcompound_initializers scanned.
        if (m_success) {
            for (auto&& initer : m_adjust) {
                ASTcompound_initializer* ciptr = std::get<0>(initer);
                TypeSpec type                  = std::get<1>(initer);
                // Subtlety: don't reset an already-resolved-known-size
                // array to an unlengthed one.
                if (!(ciptr->m_typespec.is_sized_array()
                      && type.is_unsized_array()))
                    ciptr->m_typespec = type;
                ciptr->m_ctor = std::get<2>(initer);
            }
        }
    }

    // Adjust the type of an ASTcompound_initializer to the given type
    void typecheck_init(ASTcompound_initializer* cinit, const TypeSpec& to)
    {
        // Handle the ASTcompound_initializer as a constructor of type to

        if (!cinit->nchildren()) {
            // Init all error's on
            if (m_mode & must_init_all) {
                m_success = false;
                if (errors()) {
                    cinit->errorf("Empty initializer list not allowed to"
                                  "represent '%' here",
                                  to);
                }
            }
            return;
        }

        if (cinit->ASTtype_constructor::typecheck(to, errors(), false) == to)
            mark_type(cinit, to, true);
        else
            m_success = false;
    }

    // Adjust the type for every element of an array
    void typecheck_array(ASTcompound_initializer* init, TypeSpec expected)
    {
        if (!init->initlist())
            return;  // early out for empty initializer { }
        OSL_DASSERT(expected.is_array());
        // Every element of the array is the same type
        TypeSpec elemtype = expected.elementtype();

        // Start at 1, as oslc would have already failed in either the case of
        // an empty initializer list, or zero-length array.
        int nelem                      = 1;
        ASTcompound_initializer* cinit = init;

        if (init->initlist()->nodetype() != compound_initializer_node) {
            if (!typecheck(init->initlist(), elemtype))
                return;

            cinit = next_initlist(init->initlist().get(), elemtype, nelem);
        } else {
            // Remove the outer brackets:
            //  type a[3] = { {0}, {1}, {2} };
            cinit = static_cast<ASTcompound_initializer*>(
                cinit->initlist().get());
            OSL_DASSERT(!cinit
                        || cinit->nodetype() == compound_initializer_node);
        }

        if (!elemtype.is_structure()) {
            while (cinit) {
                typecheck_init(cinit, elemtype);
                cinit = next_initlist(cinit, elemtype, nelem);
            }
        } else {
            // Every element of the array is the same StructSpec
            while (cinit) {
                typecheck_fields(cinit, cinit->initlist(), elemtype);
                cinit = next_initlist(cinit, elemtype, nelem);
            }
        }

        // Match the number of elements unless expected is unsized.
        if (m_success
            && (expected.is_unsized_array()
                || validate_size(nelem, expected.arraylength()))) {
            mark_type(init, expected);
            return;
        }

        m_success = false;
        if (errors()) {
            init->errorf("Too %s initializers for a '%s'",
                         nelem < expected.arraylength() ? "few" : "many",
                         expected);
        }
    }

    // Adjust the type for every field that has an initializer list
    void typecheck_fields(ASTNode* parent, ref arg, TypeSpec expected)
    {
        OSL_DASSERT(expected.is_structure_based());
        StructSpec* structspec = expected.structspec();
        int ninits = 0, nfields = structspec->numfields();
        while (arg && ninits < nfields) {
            const auto& field = structspec->field(ninits++);
            const auto& ftype = field.type;
            if (arg->nodetype() == compound_initializer_node) {
                // Typecheck the nested initializer list
                auto cinit = static_cast<ASTcompound_initializer*>(arg.get());
                if (!field.type.is_array()) {
                    if (!field.type.is_structure())
                        typecheck_init(cinit, field.type);
                    else if (cinit->initlist())
                        typecheck_fields(cinit, cinit->initlist().get(), ftype);
                    else if (m_mode & must_init_all)
                        m_success = false;  // empty init list not allowed
                } else
                    typecheck_array(cinit, ftype);

                // Just leave if not reporting errors
                if (!m_success && !errors())
                    return;

            } else if (!typecheck(arg, ftype, structspec, &field))
                return;

            arg = arg->next();
        }

        // Can't have left over args, would mean ninits > nfields
        if (m_success && !arg && validate_size(ninits, nfields)) {
            if (parent->nodetype() == compound_initializer_node)
                mark_type(static_cast<ASTcompound_initializer*>(parent),
                          expected);
            return;
        }

        m_success = false;
        if (errors() && (arg || nfields > ninits)) {
            parent->errorf("Too %s initializers for struct '%s'",
                           ninits < nfields ? "few" : "many",
                           structspec->name());
        }
    }

    TypeSpec typecheck(ASTcompound_initializer* ilist, TypeSpec expected)
    {
        if (!expected.is_array()) {
            if (expected.is_structure())
                typecheck_fields(ilist, ilist->initlist(), expected);
            else
                typecheck_init(ilist, expected);
        } else
            typecheck_array(ilist, expected);

        return type();
    }

    // Turn off the automatic binding on destruction
    TypeSpec nobind()
    {
        OSL_DASSERT(!m_success || m_adjust.size());
        m_debug_successful = m_success;
        m_success          = false;  // Turn off the binding in destructor
        return type();
    }

    // Turn automatic binding back on.
    void bind()
    {
        OSL_DASSERT(m_success || m_debug_successful);
        m_success = true;
    }

    TypeSpec type() const
    {
        // If succeeded, root type is at end of m_adjust
        return (m_success || m_debug_successful) && m_adjust.size()
                   ? std::get<1>(m_adjust.back())
                   : TypeSpec(TypeDesc::NONE);
    }

    bool success() const { return m_success; }

private:
    // Only adjust the types on success of root initializer
    // Oh for an llvm::SmallVector here!
    std::vector<std::tuple<ASTcompound_initializer*, TypeSpec, bool>> m_adjust;
    OSLCompilerImpl* m_compiler;
    const Strictness m_mode;
    bool m_success;
    bool m_debug_successful;  // Only for nobind() & bind() cycle assertion.

    void mark_type(ASTcompound_initializer* i, TypeSpec t, bool c = false)
    {
        m_adjust.emplace_back(i, t, c);
    }

    // Should errors be reported?
    bool errors() const { return !(m_mode & no_errors); }

    bool validate_size(int expected, int actual) const
    {
        if (expected > actual)
            return false;
        return m_mode & must_init_all ? (expected == actual) : true;
    }

    ASTcompound_initializer*
    next_initlist(ASTNode* node, const TypeSpec& expected, int& nelem) const
    {
        // Finished if already errored and not reporting them.
        // Otherwise keep checking to report as many errors as possible
        if (m_success || errors()) {
            for (node = node->nextptr(); node; node = node->nextptr()) {
                ++nelem;
                if (node->nodetype() == compound_initializer_node)
                    return static_cast<ASTcompound_initializer*>(node);
                node->typecheck(expected);
            }
        }
        return nullptr;
    }

    // Typecheck node, reporting an error.
    // Returns whether to continue iteration (not whether typecheck errored).
    bool typecheck(ref node, TypeSpec expected,
                   const StructSpec* spec             = nullptr,
                   const StructSpec::FieldSpec* field = nullptr)
    {
        if (node->typecheck(expected) != expected &&
            // Alllow assignment with comparable type
            !assignable(expected, node->typespec()) &&
            // Alllow closure assignments to '0'
            !(expected.is_closure() && !node->typespec().is_closure()
              && node->typespec().is_int_or_float()
              && node->nodetype() == literal_node
              && ((ASTliteral*)node.get())->floatval() == 0.0f)) {
            m_success = false;
            if (!errors())
                return false;

            OSL_DASSERT(!spec || field);
            node->errorf("Can't assign '%s' to '%s%s'", node->typespec(),
                         expected,
                         !spec ? ""
                               : Strutil::sprintf(" %s.%s", spec->name(),
                                                  field->name));
        }
        return true;
    }
};



TypeSpec
ASTcompound_initializer::typecheck(TypeSpec expected, unsigned mode)
{
    if (m_ctor || m_typespec.is_structure_based()
        || m_typespec.simpletype().basetype != TypeDesc::UNKNOWN) {
        if (m_typespec != expected)
            errorf("Cannot construct type '%s'", expected);
        return m_typespec;
    }

    // Scoped so ~TypeAdjuster() will bind m_typespec before return m_typespec
    {
        TypeAdjuster(m_compiler, mode).typecheck(this, expected);
    }

    return m_typespec;
}



bool
ASTNode::check_simple_arg(const TypeSpec& argtype, const char*& formals,
                          bool coerce)
{
    int advance;
    TypeSpec formaltype = m_compiler->type_from_code(formals, &advance);
    formals += advance;
    // std::cerr << "\targ is " << argtype.string()
    //           << ", formal is " << formaltype.string() << "\n";
    if (argtype == formaltype)
        return true;  // ok, move on to next arg
    if (coerce && assignable(formaltype, argtype))
        return true;
    // Allow a fixed-length array match to a formal array with
    // unspecified length, if the element types are the same.
    if (formaltype.is_unsized_array() && argtype.is_sized_array()
        && formaltype.elementtype() == argtype.elementtype())
        return true;

    // anything that gets this far we don't consider a match
    return false;
}



bool
ASTNode::check_arglist(const char* /*funcname*/, ASTNode::ref arg,
                       const char* formals, bool coerce, bool bind)
{
    // std::cerr << "ca " << funcname << " formals='" << formals << "\n";
    for (; arg; arg = arg->next()) {
        if (!*formals)  // More formal args, but no more actual args
            return false;
        if (*formals == '*')  // Will match anything left
            return true;
        if (*formals == '.') {  // Special case for token/value pairs
            // FIXME -- require that the tokens be string literals
            if (arg->typespec().is_string() && arg->next()) {
                arg = arg->next();
                continue;
            }
            return false;
        }
        if (*formals == '?') {
            if (formals[1] == '[' && formals[2] == ']') {
                // Any array
                formals += 3;
                if (arg->typespec().is_array())
                    continue;  // match
                else
                    return false;  // wanted an array, didn't get one
            }
            if (arg->typespec().is_array())
                return false;  // wanted any scalar, got an array
            formals += 1;
            continue;  // match anything
        }

        TypeSpec formaltype;
        if (arg->nodetype() == compound_initializer_node) {
            int advance;
            // Get the TypeSpec from the argument string.
            TypeSpec formaltype = m_compiler->type_from_code(formals, &advance);

            // See if the initlist can be used to construct a formaltype.
            ASTcompound_initializer::TypeAdjuster ta(
                m_compiler, ASTcompound_initializer::TypeAdjuster::no_errors);

            OSL_MAYBE_UNUSED TypeSpec itype
                = ta.typecheck(static_cast<ASTcompound_initializer*>(arg.get()),
                               formaltype);
            OSL_DASSERT(!ta.success() || (formaltype == itype));

            // ~TypeAdjuster will set the proper type for the list on success.
            // This will overwrite the prior binding simillar to how legacy
            // function ambiguity resolution took the last definition.
            if (!bind)
                ta.nobind();
        } else
            formaltype = arg->typespec();

        if (!check_simple_arg(formaltype, formals, coerce))
            return false;
        // If check_simple_arg succeeded, it advanced formals, and we
        // repeat for the next argument.
    }
    if (*formals && *formals != '*' && *formals != '.')
        return false;  // Non-*, non-... formals expected, no more actuals

    return true;  // Is this safe?
}



void
ASTfunction_call::mark_optional_output(int firstopt, const char** tags)
{
    bool mark_all = *tags && **tags == '*';
    std::vector<ASTNode::ref> argvec;
    list_to_vec(args(), argvec);

    // Find the beginning of the optional arguments
    int nargs = (int)listlength(args());
    while (firstopt < nargs && !argvec[firstopt]->typespec().is_string())
        ++firstopt;

    // Loop through the optional args, look for any tag
    for (int a = firstopt; a < (int)argvec.size() - 1; a += 2) {
        ASTNode* s    = argvec[a].get();
        bool isoutput = false;
        // compare against output tags
        if (s->typespec().is_string()) {
            if (s->nodetype() == ASTNode::literal_node) {
                // If the token is a string literal, see if it's one of the
                // ones designated as an output slot.
                for (const char** tag = tags; *tag && !isoutput; ++tag)
                    isoutput = isoutput || mark_all
                               || (!strcmp(((ASTliteral*)s)->strval(), *tag));
            } else {
                // If the token is not a literal, we don't know what it'll
                // be at runtime, so mark it conservatively as possible output.
                isoutput = true;
            }
        }
        if (isoutput) {
            // writes to the next arg!
            if (a + 2 < 32)
                argwriteonly(a + 2);  // mark writeable
            else {
                // We can only designate the first 32 args
                // writeable.  So swap it with earlier optional args.
                std::swap(argvec[firstopt], argvec[a]);
                std::swap(argvec[firstopt + 1], argvec[a + 1]);
                argwriteonly(firstopt + 1);
                firstopt += 2;  // advance in case another is needed
            }
        }
    }

    m_children[0] = vec_to_list(argvec);
}



bool
ASTfunction_call::typecheck_printf_args(const char* format, ASTNode* arg)
{
    int argnum = (m_name == "fprintf") ? 3 : 2;
    while (*format != '\0') {
        if (*format == '%') {
            if (format[1] == '%') {
                // '%%' is a literal '%'
                format += 2;  // skip both percentages
                continue;
            }
            const char* oldfmt = format;  // mark beginning of format
            while (*format && *format != 'c' && *format != 'd' && *format != 'e'
                   && *format != 'f' && *format != 'g' && *format != 'i'
                   && *format != 'm' && *format != 'n' && *format != 'o'
                   && *format != 'p' && *format != 's' && *format != 'u'
                   && *format != 'v' && *format != 'x' && *format != 'X')
                ++format;
            char formatchar = *format++;  // Also eat the format char
            if (!arg) {
                errorf(
                    "%s has mismatched format string and arguments (not enough args)",
                    m_name);
                return false;
            }
            if (arg->typespec().is_structure_based()) {
                errorf("struct '%s' is not a valid argument",
                       arg->typespec().structspec()->name());
                return false;
            }

            std::string ourformat(oldfmt, format);  // straddle the format
            // Doctor it to fix mismatches between format and data
            TypeDesc simpletype(arg->typespec().simpletype());
            if ((arg->typespec().is_closure_based()
                 || simpletype.basetype == TypeDesc::STRING)
                && formatchar != 's') {
                errorf(
                    "%s has mismatched format string and arguments (arg %d needs %%s)",
                    m_name);
                return false;
            }
            if (simpletype.basetype == TypeDesc::INT && formatchar != 'd'
                && formatchar != 'i' && formatchar != 'o' && formatchar != 'x'
                && formatchar != 'X') {
                errorf(
                    "%s has mismatched format string and arguments (arg %d needs %%d, %%i, %%o, %%x, or %%X)",
                    m_name, argnum);
                return false;
            }
            if (simpletype.basetype == TypeDesc::FLOAT && formatchar != 'f'
                && formatchar != 'g' && formatchar != 'c' && formatchar != 'e'
                && formatchar != 'm' && formatchar != 'n' && formatchar != 'p'
                && formatchar != 'v') {
                errorf(
                    "%s has mismatched format string and arguments (arg %d needs %%f, %%g, or %%e)",
                    m_name, argnum);
                return false;
            }

            arg = arg->nextptr();
            ++argnum;
        } else {
            // Everything else -- just copy the character and advance
            ++format;
        }
    }
    if (arg) {
        errorf("%s has mismatched format string and arguments (too many args)",
               m_name);
        return false;
    }
    return true;  // all ok
}



void
ASTfunction_call::typecheck_builtin_specialcase()
{
    const char* tex_out_args[]        = { "alpha", "errormessage", NULL };
    const char* pointcloud_out_args[] = { "*", NULL };

    if (m_name == "transform") {
        // Special case for transform: under the covers, it selects
        // vector or normal special versions depending on its use.
        if (typespec().simpletype() == TypeDesc::TypeVector)
            m_name = ustring("transformv");
        else if (typespec().simpletype() == TypeDesc::TypeNormal)
            m_name = ustring("transformn");
    }

    // Void functions DO read their first arg, DON'T write it
    if (typespec().is_void()) {
        argread(0, true);
        argwrite(0, false);
    }

    if (func()->readwrite_special_case()) {
        int nargs = (int)listlength(args());
        if (m_name == "sincos") {
            argwriteonly(1);
            argwriteonly(2);
        } else if (m_name == "getattribute" || m_name == "getmessage"
                   || m_name == "gettextureinfo" || m_name == "getmatrix"
                   || m_name == "dict_value") {
            // these all write to their last argument
            argwriteonly(nargs);
        } else if (m_name == "pointcloud_get") {
            argwriteonly(5);
        } else if (m_name == "pointcloud_search") {
            mark_optional_output(5, pointcloud_out_args);
        } else if ((m_name == "regex_search" || m_name == "regex_match")
                   && nargs == 3) {
            // the kind of regex_search and regex_match that contains a
            // results argument should mark it writeable.
            argwriteonly(2);
        } else if (m_name == "split") {
            argwriteonly(2);
        } else if (func()->texture_args()) {
            mark_optional_output(2, tex_out_args);
        }
    }

    if (func()->printf_args()) {
        ASTNode* arg = args().get();  // first arg
        if (arg && m_name == "fprintf")
            arg = arg->nextptr();  // skip filename param for fprintf
        const char* format = NULL;
        if (arg && arg->nodetype() == ASTNode::literal_node
            && arg->typespec().is_string()
            && (format = ((ASTliteral*)arg)->strval())) {
            arg = arg->nextptr();
            typecheck_printf_args(format, arg);
        } else {
            warningf("%s() uses a format string that is not a constant.",
                     m_name);
        }
    }

    if (func()->takes_derivs()) {
        // Special handling for the few functions that take derivatives
        // of their arguments.  Mark those with argtakesderivs.
        // N.B. This counts arguments in the same way that opcodes do --
        // assuming "arg 0" is the return value.
        size_t nargs = listlength(args());
        if (m_name == "area" || m_name == "filterwidth") {
            argtakesderivs(1, true);
#if 0
        } else if (m_name == "aastep") {
            // all but the 5-arg version take derivs of edge param
            argtakesderivs (1, nargs<5);
            // aastep(f,f) and aastep(f,f,str) take derivs of s param
            if (nargs == 2 || list_nth(args(),2)->typespec().is_string())
                argtakesderivs (2, true);
#endif
        } else if (m_name == "bump" || m_name == "displace") {
            // FIXME -- come back to this
        } else if (m_name == "calculatenormal") {
            argtakesderivs(1, true);
        } else if (m_name == "Dx" || m_name == "Dy" || m_name == "Dz") {
            argtakesderivs(1, true);
        } else if (m_name == "texture") {
            if (nargs == 3 || list_nth(args(), 3)->typespec().is_string()) {
                argtakesderivs(2, true);
                argtakesderivs(3, true);
            }
        } else if (m_name == "texture3d") {
            if (nargs == 2 || list_nth(args(), 2)->typespec().is_string()) {
                argtakesderivs(2, true);
            }
        } else if (m_name == "environment") {
            if (nargs == 2 || list_nth(args(), 2)->typespec().is_string()) {
                argtakesderivs(2, true);
            }
        } else if (m_name == "trace") {
            argtakesderivs(1, true);
            argtakesderivs(2, true);
        } else if (m_name == "noise" || m_name == "pnoise") {
            ASTNode* arg = args().get();  // first argument
            if (arg->typespec().is_string()) {
                // The kind of noise that names the type of noise
                ASTliteral* lit = (arg->nodetype() == ASTNode::literal_node)
                                      ? (ASTliteral*)arg
                                      : NULL;
                if (!lit || (lit->ustrval() == "gabor")) {
                    // unspecified (not a string literal), or known to be
                    // gabor -- take derivs of positional arguments
                    arg = arg->nextptr();  // advance to position
                    for (int n = 2; arg && !arg->typespec().is_string(); ++n) {
                        argtakesderivs(n, true);
                        arg = arg->nextptr();
                    }
                }
            }
        } else {
            OSL_ASSERT(0 && "Missed a takes_derivs case!");
        }
    }
}



TypeSpec
ASTfunction_call::typecheck_struct_constructor()
{
    StructSpec* structspec = m_sym->typespec().structspec();
    OSL_DASSERT(structspec);
    m_typespec = m_sym->typespec();
    if (structspec->numfields() != (int)listlength(args())) {
        // Support a single argument which is an init-list of proper type?
        errorf(
            "Constructor for '%s' has the wrong number of arguments (expected %d, got %d)",
            structspec->name(), structspec->numfields(), listlength(args()));
    }

    ASTcompound_initializer::TypeAdjuster(m_compiler)
        .typecheck_fields(this, args().get(), m_typespec);
    return m_typespec;
}


/// <doc CandidateFunctions>
///
/// Score a set of polymorphic functions based on arguments & return type.
///
/// The idea is that every function with the same name (visible from the scope)
/// is evaluated and 'scored' against the actual arguments.
///
/// An exact match of all arguments will have the highest score and be chosen.
/// If there is no exact match, then each possible overload is given a score
/// based on the number and type of substitutions or coercions they require.
///
/// Different types of coercions having different costs so that: int to float is
/// scored relatively high, beating out all other coercions; spatial-triple
/// to spatial-triple is a closer match than spatial-triple to color;
/// and triple to triple is a closer match than float to triple.
///
/// If a single choice has a best score, it wins.
/// If there is a tie (and only then), the return type is considered in the score.
/// If there is still not a single winner then a function is chosen by ranking
/// the possible return types, using the following precedence:
///   float, int, color, vector, point, normal, matrix, string, color closure,
///     struct, void
/// A warning is shown, printing which was function chosen and the list of all
/// that were considered ambiguous.
///
/// If two or more overloads differ only by return types that are both structs,
/// then the warning above is treated as an error.
///
/// Float to int coercion is scored, but is currently a synmonym for kNoMatch
/// as the spec does not allow implicit float to int conversion.
///
class CandidateFunctions {
    enum {
        kExactMatch    = 100,
        kIntegralToFP  = 77,
        kArrayMatch    = 44,
        kCoercable     = 23,
        kMatchAnything = 1,
        kNoMatch       = 0,

        // Additional named rules
        kFPToIntegral = kNoMatch,
        // ^^ = kIntegralToFP to match c++
        kSpatialCoerce = kCoercable + 9,
        // ^^ prefer vector/point/normal conversion over color
        kTripleCoerce = kCoercable + 4,
        // ^^ prefer triple conversion over float promotion
    };

    typedef std::vector<std::pair<ASTcompound_initializer*,
                                  ASTcompound_initializer::TypeAdjuster>>
        InitBindings;

    struct Candidate {
        FunctionSymbol* sym;
        TypeSpec rtype;
        InitBindings bindings;
        int ascore;
        int rscore;

        Candidate(FunctionSymbol* s, TypeSpec rt, int as, int rs)
            : sym(s), rtype(rt), ascore(as), rscore(rs)
        {
        }

        string_view name() const { return sym->name(); }
    };
    typedef std::vector<Candidate> Candidates;

    OSLCompilerImpl* m_compiler;
    Candidates m_candidates;
    std::set<ustring> m_scored;
    TypeSpec m_rval;
    ASTNode::ref m_args;
    size_t m_nargs;
    FunctionSymbol* m_called;  // Function called by name (can be NULL!)
    bool m_had_initlist;

    const char* scoreWildcard(int& argscore, size_t& fargs,
                              const char* args) const
    {
        while (fargs < m_nargs) {
            argscore += kMatchAnything;
            ++fargs;
        }
        return args + 1;
    }

    static int scoreType(TypeSpec expected, TypeSpec actual)
    {
        if (expected == actual)
            return kExactMatch;

        if (!actual.is_closure() && actual.is_scalarnum()
            && !expected.is_closure() && expected.is_scalarnum())
            return expected.is_int() ? kFPToIntegral : kIntegralToFP;

        if (expected.is_unsized_array() && actual.is_sized_array()
            && expected.elementtype() == actual.elementtype()) {
            // Allow a fixed-length array match to a formal array with
            // unspecified length, if the element types are the same.
            return kArrayMatch;
        }

        if (assignable(expected, actual)) {
            // Prefer conversion between spatial types
            if (actual.is_vectriple_based() && expected.is_vectriple_based())
                return kSpatialCoerce;
            // Prefer conversion between triple types
            if (!actual.is_closure() && actual.is_triple()
                && !expected.is_closure() && expected.is_triple())
                return kTripleCoerce;
            // Everything else
            return kCoercable;
        }

        return kNoMatch;
    }

    int addCandidate(FunctionSymbol* func)
    {
        // Early out if this declaration has already been scored
        if (m_scored.count(func->argcodes()))
            return kNoMatch;
        m_scored.insert(func->argcodes());

        int advance;
        const char* formals = func->argcodes().c_str();
        TypeSpec rtype      = m_compiler->type_from_code(formals, &advance);
        formals += advance;

        InitBindings bindings;
        int argscore = 0;
        size_t fargs = 0;
        for (ASTNode::ref arg = m_args; *formals && arg;
             ++fargs, arg = arg->next()) {
            switch (*formals) {
            case '*':  // Will match anything left
                formals = scoreWildcard(argscore, fargs, formals);
                OSL_DASSERT(*formals == 0);
                continue;

            case '.':  // Token/value pairs
                if (arg->typespec().is_string() && arg->next()) {
                    formals = scoreWildcard(argscore, fargs, formals);
                    OSL_DASSERT(*formals == 0);
                    continue;
                }
                return kNoMatch;

            case '?':
                if (formals[1] == '[' && formals[2] == ']') {
                    // Any array
                    formals += 3;
                    if (!arg->typespec().is_array())
                        return kNoMatch;  // wanted an array, didn't get one
                    argscore += kMatchAnything;
                } else if (!arg->typespec().is_array()) {
                    formals += 1;  // match anything
                    argscore += kMatchAnything;
                } else
                    return kNoMatch;  // wanted any scalar, got an array
                continue;

            default: break;
            }
            // To many arguments for the function, done without a match.
            if (fargs >= m_nargs)
                return kNoMatch;

            TypeSpec argtype;
            TypeSpec formaltype = m_compiler->type_from_code(formals, &advance);
            formals += advance;

            if (arg->nodetype() == ASTNode::compound_initializer_node) {
                m_had_initlist = true;
                auto ilist = static_cast<ASTcompound_initializer*>(arg.get());
                bindings.emplace_back(
                    ilist,
                    ASTcompound_initializer::TypeAdjuster(
                        m_compiler,
                        ASTcompound_initializer::TypeAdjuster::function_arg));

                // Typecheck the init list can construct the formal type.
                bindings.back().second.typecheck(ilist, formaltype);

                // Don't bind the type yet, that only occurs when the final
                // candidate is chosen.
                argtype = bindings.back().second.nobind();

                // Couldn't create the formaltype from this list...no match.
                if (argtype.simpletype().basetype == TypeDesc::NONE)
                    return kNoMatch;
            } else
                argtype = arg->typespec();

            int score = scoreType(formaltype, argtype);
            if (score == kNoMatch)
                return kNoMatch;

            argscore += score;
        }

        // Check any remaining arguments
        switch (*formals) {
        case '*':
        case '.':
            // Skip over the unused optional args
            ++formals;
            ++fargs;
        case '\0':
            if (fargs < m_nargs)
                return kNoMatch;
            break;

        default:
            // TODO: Scoring default function arguments would go here
            // Curently an unused formal argument, so no match at all.
            return kNoMatch;
        }
        OSL_DASSERT(*formals == 0);

        int highscore = m_candidates.empty() ? 0 : m_candidates.front().ascore;
        if (argscore < highscore)
            return kNoMatch;

        // clear any prior ambiguous matches
        if (argscore != highscore)
            m_candidates.clear();

        // append the latest high scoring function
        m_candidates.emplace_back(func, rtype, argscore,
                                  scoreType(m_rval, rtype));

        // save the initializer list types
        m_candidates.back().bindings.swap(bindings);

        return argscore;
    }

public:
    CandidateFunctions(OSLCompilerImpl* compiler, TypeSpec rval,
                       ASTNode::ref args, FunctionSymbol* func)
        : m_compiler(compiler)
        , m_rval(rval)
        , m_args(args)
        , m_nargs(0)
        , m_called(func)
        , m_had_initlist(false)
    {
        //std::cerr << "Matching " << func->name() << " formals='" << (rval.simpletype().basetype != TypeDesc::UNKNOWN ?  compiler->code_from_type (rval) : " ");
        for (ASTNode::ref arg = m_args; arg; arg = arg->next()) {
            //std::cerr << compiler->code_from_type (arg->typespec());
            ++m_nargs;
        }
        //std::cerr << "'\n";

        while (func) {
            //int score =
            addCandidate(func);
            //std::cerr << '\t' << func->name() << " formals='" << func->argcodes().c_str() << "'  " << score << ", " << (score ? m_candidates.back().rscore : 0) << "\n";
            func = func->nextpoly();
        }
    }

    std::string reportAmbiguity(ustring funcname, bool candidateMsg,
                                string_view msg) const
    {
        std::string argstr = funcname.string();
        argstr += " (";
        const char* comma = "";
        for (ASTNode::ref arg = m_args; arg; arg = arg->next()) {
            argstr += comma;
            if (arg->typespec().simpletype().is_unknown()
                && arg->nodetype() == ASTNode::compound_initializer_node) {
                argstr += "initializer-list";
            } else {
                argstr += arg->typespec().string();
            }
            comma = ", ";
        }
        argstr += ")";
        return Strutil::sprintf("%s '%s'%s\n", msg, argstr,
                                candidateMsg ? "\n  Candidates are:" : "");
    }

    std::string reportFunction(FunctionSymbol* sym) const
    {
        int advance;
        const char* formals = sym->argcodes().c_str();
        TypeSpec returntype = m_compiler->type_from_code(formals, &advance);
        formals += advance;
        std::string msg = "    ";
        if (ASTNode* decl = sym->node())
            msg += Strutil::sprintf("%s:%d\t", decl->sourcefile(),
                                    decl->sourceline());
        msg += Strutil::sprintf("%s %s (%s)\n", returntype, sym->name(),
                                m_compiler->typelist_from_code(formals));
        return msg;
    }

    std::pair<FunctionSymbol*, TypeSpec> best(ASTNode* caller,
                                              const ustring& funcname)
    {
        OSL_DASSERT(
            caller);  // Assertion that passed ASTNode::ref was not empty

        // When successful, bind all the initializer list types.
        auto best = [](Candidate* c) -> std::pair<FunctionSymbol*, TypeSpec> {
            for (auto&& t : c->bindings)
                t.second.bind();
            return { c->sym, c->rtype };
        };

        std::string errmsg;
        switch (m_candidates.size()) {
        case 0:
            // Nothing at all, Error
            // If m_called is 0, then user tried to call an undefined func.
            // Might be nice to fuzzy match funcname against m_compiler->symtab()
            errmsg = reportAmbiguity(funcname,
                                     m_called != nullptr /*Candidate Msg?*/,
                                     "No matching function call to");
            for (FunctionSymbol* f = m_called; f; f = f->nextpoly())
                errmsg += reportFunction(f);
            caller->errorf("%s", errmsg);
            return { nullptr, TypeSpec() };

        case 1:  // Success
            return best(&m_candidates[0]);

        default: break;
        }

        int ambiguity                = -1;
        std::pair<Candidate*, int> c = { nullptr, -1 };
        for (auto& candidate : m_candidates) {
            // re-score based on matching return value
            if (candidate.rscore > c.second) {
                ambiguity = -1;  // higher score, no longer ambiguous
                c         = std::make_pair(&candidate, candidate.rscore);
            } else if (candidate.rscore == c.second)
                ambiguity = candidate.rscore;
        }

        OSL_DASSERT(c.first && c.first->sym);

        if (ambiguity != -1) {
            unsigned userstructs = 0;

            auto rank = [&userstructs](const TypeSpec& s) -> int {
                // Arrays are currently not ranked as they cannot be returned.
                OSL_DASSERT(!s.is_array());
                OSL_DASSERT(!s.is_closure() || s.is_color_closure());

                const TypeDesc& td = s.simpletype();
                if (td == TypeDesc::TypeFloat)
                    return 0;
                if (td == TypeDesc::TypeInt)
                    return 1;
                if (td == TypeDesc::TypeColor)
                    return 2;
                if (td == TypeDesc::TypeVector)
                    return 3;
                if (td == TypeDesc::TypePoint)
                    return 4;
                if (td == TypeDesc::TypeNormal)
                    return 5;
                if (td == TypeDesc::TypeMatrix)
                    return 6;
                if (td == TypeDesc::TypeString)
                    return 7;

                if (s.is_color_closure())
                    return 8;
                if (s.is_structure_based()) {
                    ++userstructs;
                    return 9;
                }
                if (s.is_void())
                    return 10;

                OSL_DASSERT(0 && "Unranked type");
                return std::numeric_limits<int>::max();
            };

            if (true /*m_rval.simpletype().is_unknown()*/) {
                // Ambiguity because the return type desired is unknown
                //   float noise(point p)
                //   color noise(point p);
                //   float mix(color a, color b, float mx);
                //   color mix(color a, color b, color mx);
                //   mix(c0, c1, noise(P));

                // Sort m_candidates, so the ranking code can be much more
                // legible, and ambiguities will be reported in order they
                // would be chosen.
                std::sort(m_candidates.begin(), m_candidates.end(),
                          [rank](const Candidate& a,
                                 const Candidate& b) -> bool {
                              return rank(a.rtype) < rank(b.rtype);
                          });

                // New choice is now front of the list
                c = std::make_pair(&m_candidates.front(),
                                   m_candidates.front().rscore);
            }

            if (userstructs) {
                // std::sort can call 'rank' multiple times, and we can't use an
                // address to store the actual count (as we are sorting!).
                userstructs = 0;
                for (Candidates::const_reverse_iterator i
                     = m_candidates.rbegin(),
                     e = m_candidates.rend();
                     i != e; ++i) {
                    if (i->rtype.is_structure_based() && ++userstructs > 1)
                        break;
                }
            }

            bool warn = userstructs < 2;

            // Also force an error for ambiguous init-lists.
            if (warn && m_had_initlist) {
                const InitBindings* bindings = nullptr;
                for (auto& candidate : m_candidates) {
                    // If no bindings, nothing to compare against.
                    if (!bindings) {
                        bindings = &candidate.bindings;
                        continue;
                    }
                    // Number of bindings should match, otherwise score shouldn't
                    OSL_ASSERT(candidate.bindings.size() == bindings->size());
                    for (auto a = bindings->cbegin(),
                              b = candidate.bindings.cbegin(),
                              e = bindings->cend();
                         a != e; ++a, ++b) {
                        if (a->second.type() != b->second.type()) {
                            warn = false;
                            break;
                        }
                    }
                }
            }

            std::string errmsg
                = reportAmbiguity(funcname, !warn /* "Candidates are" msg*/,
                                  "Ambiguous call to");
            if (warn) {
                errmsg += Strutil::sprintf("  Chosen function is:\n%s",
                                           reportFunction(c.first->sym));
                errmsg += "  Other candidates are:\n";
                for (auto& candidate : m_candidates)
                    if (candidate.sym != c.first->sym)
                        errmsg += reportFunction(candidate.sym);
                caller->warningf("%s", errmsg);
            } else {
                for (auto& candidate : m_candidates)
                    errmsg += reportFunction(candidate.sym);
                caller->errorf("%s", errmsg);
            }
        }

        return best(c.first);
    }

    bool empty() const { return m_candidates.empty(); }

    // Remove when LegacyOverload checking is removed.
    bool hadinitlist() const { return m_had_initlist; }
};


///
/// Check how a polymorphic function variant was chosen in prior versions.
/// Check is performed based on the env variable OSL_LEGACY_FUNCTION_RESOLUTION
///
/// OSL_LEGACY_FUNCTION_RESOLUTION      // check resolution matches old behavior
/// OSL_LEGACY_FUNCTION_RESOLUTION=0    // no checking
/// OSL_LEGACY_FUNCTION_RESOLUTION=err  // check and error on mismatch
/// OSL_LEGACY_FUNCTION_RESOLUTION=use  // check and use prior on mismatch
class LegacyOverload {
    OSLCompilerImpl* m_compiler;
    ASTfunction_call* m_func;
    FunctionSymbol* m_root;
    bool (ASTNode::*m_check_arglist)(const char* funcname, ASTNode::ref arg,
                                     const char* frmls, bool coerce, bool bnd);

    std::pair<FunctionSymbol*, TypeSpec>
    typecheck_polys(TypeSpec expected, bool coerceargs, bool equivreturn)
    {
        const char* name = m_func->func()->name().c_str();
        for (FunctionSymbol* poly = m_root; poly; poly = poly->nextpoly()) {
            const char* code = poly->argcodes().c_str();
            int advance;
            TypeSpec returntype = m_compiler->type_from_code(code, &advance);
            code += advance;
            if ((m_func->*m_check_arglist)(name, m_func->args(), code,
                                           coerceargs, false)) {
                // Return types also must match if not coercible
                if (expected == returntype
                    || (equivreturn && equivalent(expected, returntype))
                    || expected == TypeSpec()) {
                    return { poly, returntype };
                }
            }
        }
        return { nullptr, TypeSpec() };
    }

public:
    LegacyOverload(
        OSLCompilerImpl* comp, ASTfunction_call* func, FunctionSymbol* root,
        bool (ASTNode::*checkfunc)(const char* funcname, ASTNode::ref arg,
                                   const char* formals, bool coerce, bool bind))
        : m_compiler(comp)
        , m_func(func)
        , m_root(root)
        , m_check_arglist(checkfunc)
    {
    }

    FunctionSymbol* operator()(TypeSpec expected)
    {
        bool match = false;
        TypeSpec typespec;
        FunctionSymbol* sym;

        // Look for an exact match, including expected return type
        std::tie(sym, typespec) = typecheck_polys(expected, false, false);
        if (typespec != TypeSpec())
            match = true;

        // Now look for an exact match for arguments, but equivalent return type
        std::tie(sym, typespec) = typecheck_polys(expected, false, true);
        if (typespec != TypeSpec())
            match = true;

        // Now look for an exact match on args, but any return type
        if (!match && expected != TypeSpec()) {
            std::tie(sym, typespec) = typecheck_polys(TypeSpec(), false, false);
            if (typespec != TypeSpec())
                match = true;
        }

        // Now look for a coercible match of args, exact march on return type
        if (!match) {
            std::tie(sym, typespec) = typecheck_polys(expected, true, false);
            if (typespec != TypeSpec())
                match = true;
        }

        // Now look for a coercible match of args, equivalent march on return type
        if (!match) {
            std::tie(sym, typespec) = typecheck_polys(expected, true, true);
            if (typespec != TypeSpec())
                match = true;
        }

        // All that failed, try for a coercible match on everything
        if (!match && expected != TypeSpec()) {
            std::tie(sym, typespec) = typecheck_polys(TypeSpec(), true, false);
            if (typespec != TypeSpec())
                match = true;
        }

        return match ? sym : nullptr;
    }
};



TypeSpec
ASTfunction_call::typecheck(TypeSpec expected)
{
    if (is_struct_ctr()) {
        // Looks like function call, but is actually struct constructor
        return typecheck_struct_constructor();
    }

    // Instead of typecheck_children, typecheck all arguments except for
    // initializer lists, who will be checked against each overload's formal
    // specification or each field's known type if is_struct_ctr.
    bool any_args_are_compound_initializers = false;
    for (ref arg = args(); arg; arg = arg->next()) {
        if (arg->nodetype() != compound_initializer_node)
            typecheck_list(arg, expected);
        else
            any_args_are_compound_initializers = true;
    }

    // Save the currently choosen symbol for error reporting later
    FunctionSymbol* poly = func();

    CandidateFunctions candidates(m_compiler, expected, args(), poly);
    std::tie(m_sym, m_typespec) = candidates.best(this, m_name);

    // Check resolution against prior versions of OSL.
    // Skip the check if any arguments used initializer list syntax.
    static const char* OSL_LEGACY = ::getenv("OSL_LEGACY_FUNCTION_RESOLUTION");
    if (!candidates.hadinitlist() && OSL_LEGACY && strcmp(OSL_LEGACY, "0")) {
        auto* legacy = LegacyOverload(m_compiler, this, poly,
                                      &ASTfunction_call::check_arglist)(
            expected);
        if (m_sym != legacy) {
            bool as_warning = true;
            if (Strutil::iequals(OSL_LEGACY, "err"))
                as_warning = false;  // full error
            std::string errmsg = "  Current overload is\n";
            if (m_sym)
                errmsg += candidates.reportFunction(
                    static_cast<FunctionSymbol*>(m_sym));
            else
                errmsg += "<none>";
            errmsg += "\n  Prior overload was ";
            if (legacy)
                errmsg += candidates.reportFunction(legacy);
            else
                errmsg += "<none>";
            if (Strutil::iequals(OSL_LEGACY, "use"))
                m_sym = legacy;
            if (as_warning)
                warningf("overload chosen differs from OSL 1.9\n%s", errmsg);
            else
                errorf("overload chosen differs from OSL 1.9\n%s", errmsg);
        }
    }

    // Fix up actual arguments compound initializers that were paired with
    // user function formal arguments that are unsigned arrays. They were
    // not yet typechecked, do so now.
    if (any_args_are_compound_initializers && is_user_function()) {
        ASTNode* formal = user_function()->formals().get();
        for (ASTNode* arg = args().get(); arg && formal;
             arg = arg->nextptr(), formal = formal->nextptr()) {
            if (arg->nodetype() == compound_initializer_node
                && formal->typespec().is_unsized_array()) {
                auto ci_arg       = (ASTcompound_initializer*)arg;
                TypeSpec expected = formal->typespec();
                expected.make_array(listlength(ci_arg->initlist()));
                arg->typecheck(expected);
            }
        }
    }

    if (m_sym != nullptr) {
        if (is_user_function()) {
            if (func()->number_of_returns() == 0
                && !func()->typespec().is_void()) {
                errorf("non-void function \"%s\" had no 'return' statement.",
                       func()->name());
            }
        } else {
            // built-in
            typecheck_builtin_specialcase();
        }
        return m_typespec;
    }

    return TypeSpec();
}



// Key:
//    x - void (only used for first char to indicate void return type)
//    i - int
//    f - float
//    c - color
//    p - point
//    v - vector
//    n - normal
//    m - matrix
//    s - string
//    ? - one arg of any type
//    X[] - an array of X's of any size
//    X[int] - an array of X's of definite length
//    * - 0 or more args of any type
//    . - 2*n args of alternating string/value
//    C - color closure
//
// There are also entries that don't describe polymorphisms, but just mark
// the functions as having special properties:
//   "!rw"      nonstandard behavior about which args are read vs written.
//   "!printf"  has a printf-like argument list
//   "!tex"     has a texture()-like token/value pair optional argument list
//   "!deriv"   takes derivs of its arguments

// clang-format off
#define ANY_ONE_FLOAT_BASED "ff", "cc", "pp", "vv", "nn"
#define NOISE_ARGS "ff", "fff", "fp", "fpf", \
                   "cf", "cff", "cp", "cpf", \
                   "vf", "vff", "vp", "vpf"
#define PNOISE_ARGS "fff", "fffff", "fpp", "fpfpf", \
                    "cff", "cffff", "cpp", "cpfpf", \
                    "vff", "vffff", "vpp", "vpfpf"
#define GNOISE_ARGS "fsf.", "fsff.", "fsp.", "fspf.", \
                    "csf.", "csff.", "csp.", "cspf.", \
                    "vsf.", "vsff.", "vsp.", "vspf."
#define PGNOISE_ARGS "fsff.", "fsffff.", "fspp.", "fspfpf.", \
                     "csff.", "csffff.", "cspp.", "cspfpf.", \
                     "vsff.", "vsffff.", "vspp.", "vspfpf."

static const char * builtin_func_args [] = {
    "area", "fp", "!deriv", NULL,
    "arraylength", "i?[]", NULL,
    "calculatenormal", "vp", "!deriv", NULL,
    "cellnoise", NOISE_ARGS, NULL,
    "concat", "sss", /*"ss.",*/ NULL,   // FIXME -- further checking
    "dict_find", "iss", "iis", NULL,
    "dict_next", "ii", NULL,
    "dict_value", "iis?", "!rw", NULL,
    "Dx", "ff", "vp", "vv", "vn", "cc", "!deriv", NULL,
    "Dy", "ff", "vp", "vv", "vn", "cc", "!deriv", NULL,
    "Dz", "ff", "vp", "vv", "vn", "cc", "!deriv", NULL,
    "environment", "fsv.", "fsvvv.","csv.", "csvvv.",
               "vsv.", "vsvvv.", "!tex", "!rw", "!deriv", NULL,
    "error", "xs*", "!printf", NULL,
    "exit", "x", NULL,
    "filterwidth", "ff", "vp", "vv", "!deriv", NULL,
    "format", "ss*", "!printf", NULL,
    "fprintf", "xss*", "!printf", NULL,
    "getattribute", "is?", "is?[]", "iss?", "iss?[]",  "isi?", "isi?[]", "issi?", "issi?[]", "!rw", NULL,  // FIXME -- further checking?
    "getmessage", "is?", "is?[]", "iss?", "iss?[]", "!rw", NULL,
    "gettextureinfo", "iss?", "iss?[]", "!rw", NULL,  // FIXME -- further checking?
    "hashnoise", NOISE_ARGS, NULL,
    "isconnected", "i?", NULL,
    "isconstant", "i?", NULL,
    "noise", GNOISE_ARGS, NOISE_ARGS, "!deriv", NULL,
    "pnoise", PGNOISE_ARGS, PNOISE_ARGS, "!deriv", NULL,
    "pointcloud_search", "ispfi.", "ispfii.", "!rw", NULL,
    "pointcloud_get", "isi[]is?[]", "!rw", NULL,
    "pointcloud_write", "isp.", NULL,
    "printf", "xs*", "!printf", NULL,
    "psnoise", PNOISE_ARGS, NULL,
    "random", "f", "c", "p", "v", "n", NULL,
    "regex_match", "iss", "isi[]s", "!rw", NULL,
    "regex_search", "iss", "isi[]s", "!rw", NULL,
    "setmessage", "xs?", "xs?[]", NULL,
    "sincos", "xfff", "xccc", "xppp", "xvvv", "xnnn", "!rw", NULL,
    "snoise", NOISE_ARGS, NULL,
    "spline", "fsff[]", "csfc[]", "psfp[]", "vsfv[]", "nsfn[]", "fsfif[]", "csfic[]", "psfip[]", "vsfiv[]", "nsfin[]", NULL,
    "splineinverse", "fsff[]", "fsfif[]", NULL,
    "split", "iss[]si", "iss[]s", "iss[]", "!rw", NULL,
    "surfacearea", "f", NULL,
    "texture", "fsff.", "fsffffff.","csff.", "csffffff.",
               "vsff.", "vsffffff.", "!tex", "!rw", "!deriv", NULL,
    "texture3d", "fsp.", "fspvvv.","csp.", "cspvvv.",
               "vsp.", "vspvvv.", "!tex", "!rw", "!deriv", NULL,
    "trace", "ipv.", "!deriv", NULL,
    "warning", "xs*", "!printf", NULL,   // FIXME -- further checking
    NULL
#undef ANY_ONE_FLOAT_BASED
#undef NOISE_ARGS
#undef PNOISE_ARGS
};
// clang-format on



void
OSLCompilerImpl::initialize_builtin_funcs()
{
    for (int i = 0; builtin_func_args[i]; ++i) {
        ustring funcname(builtin_func_args[i++]);
        // Count the number of polymorphic versions and look for any
        // special hint markers.
        int npoly                   = 0;
        bool readwrite_special_case = false;
        bool texture_args           = false;
        bool printf_args            = false;
        bool takes_derivs           = false;
        for (npoly = 0; builtin_func_args[i + npoly]; ++npoly) {
            if (!strcmp(builtin_func_args[i + npoly], "!rw"))
                readwrite_special_case = true;
            else if (!strcmp(builtin_func_args[i + npoly], "!tex"))
                texture_args = true;
            else if (!strcmp(builtin_func_args[i + npoly], "!printf"))
                printf_args = true;
            else if (!strcmp(builtin_func_args[i + npoly], "!deriv"))
                takes_derivs = true;
        }
        // Now add them in reverse order, so the order in the table is
        // the priority order for approximate matches.
        for (int j = npoly - 1; j >= 0; --j) {
            if (builtin_func_args[i + j][0] == '!')  // Skip special hints
                continue;
            ustring poly(builtin_func_args[i + j]);
            Symbol* last = symtab().clash(funcname);
            OSL_DASSERT(last == NULL || last->symtype() == SymTypeFunction);
            TypeSpec rettype  = type_from_code(poly.c_str());
            FunctionSymbol* f = new FunctionSymbol(funcname, rettype);
            f->nextpoly((FunctionSymbol*)last);
            f->argcodes(poly);
            f->readwrite_special_case(readwrite_special_case);
            f->texture_args(texture_args);
            f->printf_args(printf_args);
            f->takes_derivs(takes_derivs);
            symtab().insert(f);
        }
        i += npoly;
    }
}



TypeSpec
OSLCompilerImpl::type_from_code(const char* code, int* advance)
{
    TypeSpec t;
    int i = 0;
    switch (code[i]) {
    case 'i': t = TypeDesc::TypeInt; break;
    case 'f': t = TypeDesc::TypeFloat; break;
    case 'c': t = TypeDesc::TypeColor; break;
    case 'p': t = TypeDesc::TypePoint; break;
    case 'v': t = TypeDesc::TypeVector; break;
    case 'n': t = TypeDesc::TypeNormal; break;
    case 'm': t = TypeDesc::TypeMatrix; break;
    case 's': t = TypeDesc::TypeString; break;
    case 'x': t = TypeDesc(TypeDesc::NONE); break;
    case 'X': t = TypeDesc(TypeDesc::PTR); break;
    case 'L': t = TypeDesc(TypeDesc::LONGLONG); break;
    case 'C':  // color closure
        t = TypeSpec(TypeDesc::TypeColor, true);
        break;
    case 'S':  // structure
        // Following the 'S' is the numeric structure ID
        t = TypeSpec("struct", atoi(code + i + 1));
        // Skip to the last digit
        while (isdigit(code[i + 1]))
            ++i;
        break;
    case '?': break;  // anything will match, so keep 'UNKNOWN'
    case '*': break;  // anything will match, so keep 'UNKNOWN'
    case '.': break;  // anything will match, so keep 'UNKNOWN'
    default:
        OSL_DASSERT_MSG(0, "Don't know how to decode type code '%d'",
                        (int)code[0]);
        if (advance)
            *advance = 1;
        return TypeSpec();
    }
    ++i;

    if (code[i] == '[') {
        ++i;
        t.make_array(-1);  // signal arrayness, unknown length
        if (isdigit(code[i]) || code[i] == ']') {
            if (isdigit(code[i]))
                t.make_array(atoi(code + i));
            while (isdigit(code[i]))
                ++i;
            if (code[i] == ']')
                ++i;
        }
    }

    if (advance)
        *advance = i;
    return t;
}



std::string
OSLCompilerImpl::typelist_from_code(const char* code) const
{
    std::string ret;
    while (*code) {
        // Handle some special cases
        int advance = 1;
        if (ret.length())
            ret += ", ";
        if (*code == '.') {
            ret += "...";
        } else if (*code == 'T') {
            ret += "...";
        } else if (*code == '?') {
            ret += "<any>";
        } else {
            TypeSpec t = type_from_code(code, &advance);
            ret += type_c_str(t);
        }
        code += advance;
        if (*code == '[') {
            ret += "[]";
            ++code;
            while (isdigit(*code))
                ++code;
            if (*code == ']')
                ++code;
        }
    }

    return ret;
}



std::string
OSLCompilerImpl::code_from_type(TypeSpec type) const
{
    std::string out;
    TypeDesc elem = type.elementtype().simpletype();
    if (type.is_structure() || type.is_structure_array()) {
        out = Strutil::sprintf("S%d", type.structure());
    } else if (type.is_closure() || type.is_closure_array()) {
        out = 'C';
    } else {
        if (elem == TypeDesc::TypeInt)
            out = 'i';
        else if (elem == TypeDesc::TypeFloat)
            out = 'f';
        else if (elem == TypeDesc::TypeColor)
            out = 'c';
        else if (elem == TypeDesc::TypePoint)
            out = 'p';
        else if (elem == TypeDesc::TypeVector)
            out = 'v';
        else if (elem == TypeDesc::TypeNormal)
            out = 'n';
        else if (elem == TypeDesc::TypeMatrix)
            out = 'm';
        else if (elem == TypeDesc::TypeString)
            out = 's';
        else if (elem == TypeDesc::NONE)
            out = 'x';
        else {
            out = 'x';
            // This only happens in error circumstances. Seems safe to
            // return the code for 'void' and hope everything sorts itself
            // out with the downstream errors.
        }
    }

    if (type.is_array()) {
        if (type.is_unsized_array())
            out += "[]";
        else
            out += Strutil::sprintf("[%d]", type.arraylength());
    }

    return out;
}



void
OSLCompilerImpl::typespecs_from_codes(const char* code,
                                      std::vector<TypeSpec>& types) const
{
    types.clear();
    while (code && *code) {
        int advance;
        types.push_back(type_from_code(code, &advance));
        code += advance;
    }
}



};  // namespace pvt

OSL_NAMESPACE_EXIT
