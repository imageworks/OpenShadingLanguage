// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#include <OpenImageIO/thread.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo_util.h>

#include <OSL/oslexec.h>

using namespace OSL;
using namespace OSL::pvt;

// avoid naming conflict with MSVC macro
#ifdef RGB
#undef RGB
#endif

OSL_NAMESPACE_ENTER



bool
shade_image (ShadingSystem &shadingsys, ShaderGroup &group,
             const ShaderGlobals *defaultsg,
             OIIO::ImageBuf &buf, cspan<ustring> outputs,
             ShadeImageLocations shadelocations,
             OIIO::ROI roi, OIIO::parallel_options popt)
{
    using namespace OIIO;
    using namespace ImageBufAlgo;
    if (! roi.defined())
        roi = buf.roi();
    if (buf.spec().format != TypeDesc::FLOAT) {
#if OIIO_VERSION >= 20300
        buf.errorfmt("Cannot OSL::shade_image() into a {} buffer, float is required",
                     buf.spec().format);
#else
        buf.error("Cannot OSL::shade_image() into a %s buffer, float is required",
                  buf.spec().format);
#endif
        return false;
    }

    parallel_image (roi, popt, [&](OIIO::ROI roi){

    // Request an OSL::PerThreadInfo for this thread.
    OSL::PerThreadInfo *thread_info = shadingsys.create_thread_info();

    // Request a shading context so that we can execute the shader.
    // We could get_context/release_context for each shading point,
    // but to save overhead, it's more efficient to reuse a context
    // within a thread.
    ShadingContext *ctx = shadingsys.get_context (thread_info);

    // Ensure the group has already been optimized
    shadingsys.optimize_group (&group, ctx);

    Matrix44 Mshad, Mobj;  // just let these be identity for now
    OIIO::ROI roi_full = buf.roi_full();
    int xres = roi_full.width();
    int yres = roi_full.height();
    int zres = roi_full.depth();

    // Gather some information about the outputs once, rather than for
    // each pixel.
    const ShaderSymbol **output_sym  = OIIO_ALLOCA(const ShaderSymbol*, outputs.size());
    TypeDesc *output_type = OIIO_ALLOCA(TypeDesc, outputs.size());
    int *output_nchans = OIIO_ALLOCA(int, outputs.size());
    for (int i = 0;  i < int(outputs.size());  ++i) {
        output_sym[i] = shadingsys.find_symbol (group, outputs[i]);
        output_type[i] = shadingsys.symbol_typedesc (output_sym[i]);
        output_nchans[i] = output_type[i].numelements() * output_type[i].aggregate;
    }

    // Set up shader globals and a little test grid of points to shade.
    // Note that some of the fields can be set up once and used for all of
    // the shades. Others need to be changed for every point shaded.
    //
    // Note that because we are shading a single object that is a flat image
    // plane, a lot of this is simplified. In a real 3D render, most of
    // these fields would need to be reset for every shade.
    ShaderGlobals sg;
    if (defaultsg) {
        // If the caller passed a default SG template, use it to initialize
        // the sg and in particular to set all the constant fields.
        memcpy ((char *)&sg, (const char*)defaultsg, sizeof(ShaderGlobals));
    } else {
        // No SG template was passed, so set up reasonable defaults.
        memset ((char *)&sg, 0, sizeof(ShaderGlobals));
        // Set "shader" space to be Mshad.  In a real renderer, this may be
        // different for each shader group.
        sg.shader2common = OSL::TransformationPtr (&Mshad);
        // Set "object" space to be Mobj.  In a real renderer, this may be
        // different for each object.
        sg.object2common = OSL::TransformationPtr (&Mobj);
        // Just make it look like all shades are the result of 'raytype' rays.
        sg.raytype = 0;  // default ray type
        // Set the surface area of the patch to 1 (which it is).  This is
        // only used for light shaders that call the surfacearea() function.
        sg.surfacearea = 1;
        // Derivs are constant across the image
        if (shadelocations == ShadePixelCenters) {
            sg.dudx  = 1.0f / xres;    // sg.dudy is already 0
            sg.dvdy  = 1.0f / yres;    // sg.dvdx is already 0
        } else {
            sg.dudx  = 1.0f / std::max(1,(xres-1));
            sg.dvdy  = 1.0f / std::max(1,(yres-1));
        }
        // Derivatives with respect to x,y
        sg.dPdx = Vec3 (1.0f, 0.0f, 0.0f);
        sg.dPdy = Vec3 (0.0f, 1.0f, 0.0f);
        sg.dPdz = Vec3 (0.0f, 0.0f, 1.0f);
        // Tangents of P with respect to surface u,v
        sg.dPdu = Vec3 (xres, 0.0f, 0.0f);
        sg.dPdv = Vec3 (0.0f, yres, 0.0f);
        sg.dPdz = Vec3 (0.0f, 0.0f, zres);
        // That also implies that our normal points to (0,0,1)
        sg.N    = Vec3 (0, 0, 1);
        sg.Ng   = Vec3 (0, 0, 1);
        // In our SimpleRenderer, the "renderstate" itself just a pointer to
        // the ShaderGlobals.
        // sg.renderstate = &sg;
    }

    // Loop over all pixels in the image (in x and y)...
    for (OIIO::ImageBuf::Iterator<float> p (buf, roi);  ! p.done();  ++p) {
        // Set the shader globals that vary from point to pixel to pixel
        sg.P = Vec3 (p.x(), p.y(), p.z());
        if (shadelocations == ShadePixelCenters) {
            sg.u    = float(p.x()-roi_full.xbegin+0.5f) / xres;
            sg.v    = float(p.y()-roi_full.ybegin+0.5f) / yres;
            // float w = float(p.z()-roi_full.zbegin+0.5f) / zres;
        } else {
            sg.u    = (xres == 1) ? 0.5f : float(p.x()-roi_full.xbegin) / (xres - 1);
            sg.v    = (yres == 1) ? 0.5f : float(p.y()-roi_full.ybegin) / (yres - 1);
            // float w = (zres == 1) ? 0.5f : float(p.z()-roi_full.zbegin) / (zres - 1);
        }

        // Actually run the shader for this point
        shadingsys.execute (*ctx, group, sg);

        // Save all the designated outputs.
        int chan = 0;
        for (int i = 0;  i < int(outputs.size());  ++i) {
            const void *data = shadingsys.symbol_address (*ctx, output_sym[i]);
            if (!data)
                continue;  // Skip if symbol isn't found
            TypeDesc t = output_type[i];
            int tvals = output_nchans[i];
            if (chan+tvals > buf.nchannels())
                break;
            if (t.basetype == TypeDesc::FLOAT) {
                for (int c = 0; c < tvals; ++c)
                    p[chan++] = ((const float *)data)[c];
            } else if (t.basetype == TypeDesc::INT) {
                for (int c = 0; c < int(t.numelements())*t.aggregate; ++c)
                    p[chan++] = ((const int *)data)[c];
            }
            // N.B. Drop any outputs that aren't float- or int-based
        }
    }

    // We're done shading with this context.
    shadingsys.release_context (ctx);
    shadingsys.destroy_thread_info (thread_info);

    });   // end of parallel_image
    return true;
}



OSL_NAMESPACE_EXIT

