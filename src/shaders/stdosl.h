// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage


#ifndef STDOSL_H
#define STDOSL_H


#ifndef M_PI
#define M_PI       3.1415926535897932        /* pi */
#define M_PI_2     1.5707963267948966        /* pi/2 */
#define M_PI_4     0.7853981633974483        /* pi/4 */
#define M_2_PI     0.6366197723675813        /* 2/pi */
#define M_2PI      6.2831853071795865        /* 2*pi */
#define M_4PI     12.566370614359173         /* 4*pi */
#define M_2_SQRTPI 1.1283791670955126        /* 2/sqrt(pi) */
#define M_E        2.7182818284590452        /* e (Euler's number) */
#define M_LN2      0.6931471805599453        /* ln(2) */
#define M_LN10     2.3025850929940457        /* ln(10) */
#define M_LOG2E    1.4426950408889634        /* log_2(e) */
#define M_LOG10E   0.4342944819032518        /* log_10(e) */
#define M_SQRT2    1.4142135623730950        /* sqrt(2) */
#define M_SQRT1_2  0.7071067811865475        /* 1/sqrt(2) */
#endif



// Declaration of built-in functions and closures
#define BUILTIN [[ int builtin = 1 ]]
#define BUILTIN_DERIV [[ int builtin = 1, int deriv = 1 ]]

#define PERCOMP1(name)                          \
    normal name (normal x) BUILTIN;             \
    vector name (vector x) BUILTIN;             \
    point  name (point x) BUILTIN;              \
    color  name (color x) BUILTIN;              \
    float  name (float x) BUILTIN;

// Declare name (T,T) for T in {triples,float}
#define PERCOMP2(name)                          \
    normal name (normal x, normal y) BUILTIN;   \
    vector name (vector x, vector y) BUILTIN;   \
    point  name (point x, point y) BUILTIN;     \
    color  name (color x, color y) BUILTIN;     \
    float  name (float x, float y) BUILTIN;

// Declare name(T,float) for T in {triples}
#define PERCOMP2F(name)                         \
    normal name (normal x, float y) BUILTIN;    \
    vector name (vector x, float y) BUILTIN;    \
    point  name (point x, float y) BUILTIN;     \
    color  name (color x, float y) BUILTIN;

// Basic math
normal degrees (normal x) { return x*(180.0/M_PI); }
vector degrees (vector x) { return x*(180.0/M_PI); }
point  degrees (point x)  { return x*(180.0/M_PI); }
color  degrees (color x)  { return x*(180.0/M_PI); }
float  degrees (float x)  { return x*(180.0/M_PI); }
normal radians (normal x) { return x*(M_PI/180.0); }
vector radians (vector x) { return x*(M_PI/180.0); }
point  radians (point x)  { return x*(M_PI/180.0); }
color  radians (color x)  { return x*(M_PI/180.0); }
float  radians (float x)  { return x*(M_PI/180.0); }
PERCOMP1 (cos)
PERCOMP1 (sin)
PERCOMP1 (tan)
PERCOMP1 (acos)
PERCOMP1 (asin)
PERCOMP1 (atan)
PERCOMP2 (atan2)
PERCOMP1 (cosh)
PERCOMP1 (sinh)
PERCOMP1 (tanh)

normal pow (normal x, normal y) BUILTIN;
vector pow (vector x, vector y) BUILTIN;
point  pow (point x, point y) BUILTIN;
color  pow (color x, color y) BUILTIN;
normal pow (normal x, float y) BUILTIN;
vector pow (vector x, float y) BUILTIN;
point  pow (point x, float y) BUILTIN;
color  pow (color x, float y) BUILTIN;
float  pow (float x, float y) BUILTIN;

PERCOMP1 (exp)
PERCOMP1 (exp2)
PERCOMP1 (expm1)
PERCOMP1 (log)
point  log (point a,  float b) { return log(a)/log(b); }
vector log (vector a, float b) { return log(a)/log(b); }
color  log (color a,  float b) { return log(a)/log(b); }
float  log (float a,  float b) { return log(a)/log(b); }
PERCOMP1 (log2)
PERCOMP1 (log10)
PERCOMP1 (logb)
PERCOMP1 (sqrt)
PERCOMP1 (inversesqrt)
PERCOMP1 (cbrt)
float hypot (float a, float b) { return sqrt (a*a + b*b); }
float hypot (float a, float b, float c) { return sqrt (a*a + b*b + c*c); }
PERCOMP1 (abs)
int abs (int x) BUILTIN;
PERCOMP1 (fabs)
int fabs (int x) BUILTIN;
PERCOMP1 (sign)
PERCOMP1 (floor)
PERCOMP1 (ceil)
PERCOMP1 (round)
PERCOMP1 (trunc)

normal fmod (normal x, normal y) BUILTIN;
vector fmod (vector x, vector y) BUILTIN;
point  fmod (point x, point y) BUILTIN;
color  fmod (color x, color y) BUILTIN;
normal fmod (normal x, float y) BUILTIN;
vector fmod (vector x, float y) BUILTIN;
point  fmod (point x, float y) BUILTIN;
color  fmod (color x, float y) BUILTIN;
float  fmod (float x, float y) BUILTIN;

int    mod (int    a, int    b) { return a - b*(int)floor(a/b); }
point  mod (point  a, point  b) { return a - b*floor(a/b); }
vector mod (vector a, vector b) { return a - b*floor(a/b); }
normal mod (normal a, normal b) { return a - b*floor(a/b); }
color  mod (color  a, color  b) { return a - b*floor(a/b); }
point  mod (point  a, float  b) { return a - b*floor(a/b); }
vector mod (vector a, float  b) { return a - b*floor(a/b); }
normal mod (normal a, float  b) { return a - b*floor(a/b); }
color  mod (color  a, float  b) { return a - b*floor(a/b); }
float  mod (float  a, float  b) { return a - b*floor(a/b); }
PERCOMP2 (min)
int min (int a, int b) BUILTIN;
PERCOMP2 (max)
int max (int a, int b) BUILTIN;
normal clamp (normal x, normal minval, normal maxval) { return max(min(x,maxval),minval); }
vector clamp (vector x, vector minval, vector maxval) { return max(min(x,maxval),minval); }
point  clamp (point x, point minval, point maxval) { return max(min(x,maxval),minval); }
color  clamp (color x, color minval, color maxval) { return max(min(x,maxval),minval); }
float  clamp (float x, float minval, float maxval) { return max(min(x,maxval),minval); }
int    clamp (int x, int minval, int maxval) { return max(min(x,maxval),minval); }
#if 0
normal mix (normal x, normal y, normal a) { return x*(1-a) + y*a; }
normal mix (normal x, normal y, float  a) { return x*(1-a) + y*a; }
vector mix (vector x, vector y, vector a) { return x*(1-a) + y*a; }
vector mix (vector x, vector y, float  a) { return x*(1-a) + y*a; }
point  mix (point  x, point  y, point  a) { return x*(1-a) + y*a; }
point  mix (point  x, point  y, float  a) { return x*(1-a) + y*a; }
color  mix (color  x, color  y, color  a) { return x*(1-a) + y*a; }
color  mix (color  x, color  y, float  a) { return x*(1-a) + y*a; }
float  mix (float  x, float  y, float  a) { return x*(1-a) + y*a; }
#else
normal mix (normal x, normal y, normal a) BUILTIN;
normal mix (normal x, normal y, float  a) BUILTIN;
vector mix (vector x, vector y, vector a) BUILTIN;
vector mix (vector x, vector y, float  a) BUILTIN;
point  mix (point  x, point  y, point  a) BUILTIN;
point  mix (point  x, point  y, float  a) BUILTIN;
color  mix (color  x, color  y, color  a) BUILTIN;
color  mix (color  x, color  y, float  a) BUILTIN;
float  mix (float  x, float  y, float  a) BUILTIN;
#endif
closure color mix (closure color x, closure color y, float a) { return x*(1-a) + y*a; }
closure color mix (closure color x, closure color y, color a) { return x*(1-a) + y*a; }

normal select (normal x, normal y, normal cond) BUILTIN;
vector select (vector x, vector y, vector cond) BUILTIN;
point  select (point  x, point  y, point  cond) BUILTIN;
color  select (color  x, color  y, color  cond) BUILTIN;
float  select (float  x, float  y, float  cond) BUILTIN;
normal select (normal x, normal y, float cond) BUILTIN;
vector select (vector x, vector y, float cond) BUILTIN;
point  select (point  x, point  y, float cond) BUILTIN;
color  select (color  x, color  y, float cond) BUILTIN;
normal select (normal x, normal y, int cond) BUILTIN;
vector select (vector x, vector y, int cond) BUILTIN;
point  select (point  x, point  y, int cond) BUILTIN;
color  select (color  x, color  y, int cond) BUILTIN;
float  select (float  x, float  y, int cond) BUILTIN;
int isnan (float x) BUILTIN;
int isinf (float x) BUILTIN;
int isfinite (float x) BUILTIN;
float erf (float x) BUILTIN;
float erfc (float x) BUILTIN;

// Vector functions

vector cross (vector a, vector b) BUILTIN;
float dot (vector a, vector b) BUILTIN;
float length (vector v) BUILTIN;
float distance (point a, point b) BUILTIN;
float distance (point a, point b, point q)
{
    vector d = b - a;
    float dd = dot(d, d);
    if(dd == 0.0)
        return distance(q, a);
    float t = dot(q - a, d)/dd;
    return distance(q, a + clamp(t, 0.0, 1.0)*d);
}
normal normalize (normal v) BUILTIN;
vector normalize (vector v) BUILTIN;
vector faceforward (vector N, vector I, vector Nref)
{
    return (dot(I, Nref) > 0) ? -N : N;
}
vector faceforward (vector N, vector I)
{
    return faceforward(N, I, Ng);
}
vector reflect (vector I, vector N) { return I - 2*dot(N,I)*N; }
vector refract (vector I, vector N, float eta) {
    float IdotN = dot (I, N);
    float k = 1 - eta*eta * (1 - IdotN*IdotN);
    return (k < 0) ? vector(0,0,0) : (eta*I - N * (eta*IdotN + sqrt(k)));
}
void fresnel (vector I, normal N, float eta,
              output float Kr, output float Kt,
              output vector R, output vector T)
{
    float sqr(float x) { return x*x; }
    float c = dot(I, N);
    if (c < 0)
        c = -c;
    R = reflect(I, N);
    float g = 1.0 / sqr(eta) - 1.0 + c * c;
    if (g >= 0.0) {
        g = sqrt (g);
        float beta = g - c;
        float F = (c * (g+c) - 1.0) / (c * beta + 1.0);
        F = 0.5 * (1.0 + sqr(F));
        F *= sqr (beta / (g+c));
        Kr = F;
        Kt = (1.0 - Kr) * eta*eta;
        // OPT: the following recomputes some of the above values, but it 
        // gives us the same result as if the shader-writer called refract()
        T = refract(I, N, eta);
    } else {
        // total internal reflection
        Kr = 1.0;
        Kt = 0.0;
        T = vector (0,0,0);
    }
}

void fresnel (vector I, normal N, float eta,
              output float Kr, output float Kt)
{
    vector R, T;
    fresnel(I, N, eta, Kr, Kt, R, T);
}


normal transform (matrix Mto, normal p) BUILTIN;
vector transform (matrix Mto, vector p) BUILTIN;
point  transform (matrix Mto, point p) BUILTIN;
normal transform (string from, string to, normal p) BUILTIN;
vector transform (string from, string to, vector p) BUILTIN;
point  transform (string from, string to, point p) BUILTIN;
normal transform (string to, normal p) { return transform("common",to,p); }
vector transform (string to, vector p) { return transform("common",to,p); }
point  transform (string to, point p)  { return transform("common",to,p); }

float transformu (string tounits, float x) BUILTIN;
float transformu (string fromunits, string tounits, float x) BUILTIN;

point rotate (point p, float angle, point a, point b)
{
    vector axis = normalize (b - a);
    float cosang, sinang;
    sincos (angle, sinang, cosang);
    float cosang1 = 1.0 - cosang;
    float x = axis[0], y = axis[1], z = axis[2];
    matrix M = matrix (x * x + (1.0 - x * x) * cosang,
                       x * y * cosang1 + z * sinang,
                       x * z * cosang1 - y * sinang,
                       0.0,
                       x * y * cosang1 - z * sinang,
                       y * y + (1.0 - y * y) * cosang,
                       y * z * cosang1 + x * sinang,
                       0.0,
                       x * z * cosang1 + y * sinang,
                       y * z * cosang1 - x * sinang,
                       z * z + (1.0 - z * z) * cosang,
                       0.0,
                       0.0, 0.0, 0.0, 1.0);
    return transform (M, p-a) + a;
}

point rotate (point p, float angle, vector axis)
{
    return rotate (p, angle, point(0), axis);
}



// Color functions

float luminance (color c) BUILTIN;
color blackbody (float temperatureK) BUILTIN;
color wavelength_color (float wavelength_nm) BUILTIN;
color transformc (string from, string to, color c) BUILTIN;
color transformc (string to, color c) { return transformc ("rgb", to, c); }



// Matrix functions

float determinant (matrix m) BUILTIN;
matrix transpose (matrix m) BUILTIN;



// Pattern generation

color step (color edge, color x) BUILTIN;
point step (point edge, point x) BUILTIN;
vector step (vector edge, vector x) BUILTIN;
normal step (normal edge, normal x) BUILTIN;
float step (float edge, float x) BUILTIN;
float smoothstep (float edge0, float edge1, float x) BUILTIN;

color smoothstep (color edge0, color edge1, color x)
{
    return color (smoothstep(edge0[0], edge1[0], x[0]),
                  smoothstep(edge0[1], edge1[1], x[1]),
                  smoothstep(edge0[2], edge1[2], x[2]));
}
vector smoothstep (vector edge0, vector edge1, vector x)
{
    return vector (smoothstep(edge0[0], edge1[0], x[0]),
                   smoothstep(edge0[1], edge1[1], x[1]),
                   smoothstep(edge0[2], edge1[2], x[2]));
}

float linearstep (float edge0, float edge1, float x) {
    float result;
    if (edge0 != edge1) {
        float xclamped = clamp (x, edge0, edge1);
        result = (xclamped - edge0) / (edge1 - edge0);
    } else {  // special case: edges coincide
        result = step (edge0, x);
    }
    return result;
}
color linearstep (color edge0, color edge1, color x)
{
    return color (linearstep(edge0[0], edge1[0], x[0]),
                  linearstep(edge0[1], edge1[1], x[1]),
                  linearstep(edge0[2], edge1[2], x[2]));
}
vector linearstep (vector edge0, vector edge1, vector x)
{
    return vector (linearstep(edge0[0], edge1[0], x[0]),
                   linearstep(edge0[1], edge1[1], x[1]),
                   linearstep(edge0[2], edge1[2], x[2]));
}

float smooth_linearstep (float edge0, float edge1, float x_, float eps_) {
    float result;
    if (edge0 != edge1) {
        float rampup (float x, float r) { return 0.5/r * x*x; }
        float width_inv = 1.0 / (edge1 - edge0);
        float eps = eps_ * width_inv;
        float x = (x_ - edge0) * width_inv;
        if      (x <= -eps)                result = 0;
        else if (x >= eps && x <= 1.0-eps) result = x;
        else if (x >= 1.0+eps)             result = 1;
        else if (x < eps)                  result = rampup (x+eps, 2.0*eps);
        else /* if (x < 1.0+eps) */        result = 1.0 - rampup (1.0+eps - x, 2.0*eps);
    } else {
        result = step (edge0, x_);
    }
    return result;
}

color smooth_linearstep (color edge0, color edge1, color x, color eps)
{
    return color (smooth_linearstep(edge0[0], edge1[0], x[0], eps[0]),
                  smooth_linearstep(edge0[1], edge1[1], x[1], eps[1]),
                  smooth_linearstep(edge0[2], edge1[2], x[2], eps[2]));
}
vector smooth_linearstep (vector edge0, vector edge1, vector x, vector eps)
{
    return vector (smooth_linearstep(edge0[0], edge1[0], x[0], eps[0]),
                   smooth_linearstep(edge0[1], edge1[1], x[1], eps[1]),
                   smooth_linearstep(edge0[2], edge1[2], x[2], eps[2]));
}

float aastep (float edge, float s, float dedge, float ds) {
    // Box filtered AA step
    float width = fabs(dedge) + fabs(ds);
    float halfwidth = 0.5*width;
    float e1 = edge-halfwidth;
    return (s <= e1) ? 0.0 : ((s >= (edge+halfwidth)) ? 1.0 : (s-e1)/width);
}
float aastep (float edge, float s, float ds) {
    return aastep (edge, s, filterwidth(edge), ds);
}
float aastep (float edge, float s) {
    return aastep (edge, s, filterwidth(edge), filterwidth(s));
}


// Noise and related functions

int hash (int u) BUILTIN;
int hash (float u) BUILTIN;
int hash (float u, float v) BUILTIN;
int hash (point p) BUILTIN;
int hash (point p, float t) BUILTIN;

// Derivatives and area operators


// Displacement functions
void displace (vector offset) {
    P += offset;
    N = normalize (calculatenormal (P));
}
void displace (float amp) {
    displace (amp * N);
}
void displace (string space, float amp) {
    float len = length (transform (space, N));
    displace (amp * len * N);
}
void displace (string space, vector offset) {
    displace (transform (space, "common", offset));
}

void bump (vector offset) {
    N = normalize (calculatenormal (P+offset));
}
void bump (float amp) {
    bump (amp * N);
}
void bump (string space, float amp) {
    float len = length (transform (space, N));
    bump (amp * len * N);
}
void bump (string space, vector offset) {
    bump (transform (space, "common", offset));
}



// String functions
int strlen (string s) BUILTIN;
int hash (string s) BUILTIN;
int getchar (string s, int index) BUILTIN;
int startswith (string s, string prefix) BUILTIN;
int endswith (string s, string suffix) BUILTIN;
string substr (string s, int start, int len) BUILTIN;
string substr (string s, int start) { return substr (s, start, strlen(s)); }
float stof (string str) BUILTIN;
int stoi (string str) BUILTIN;

// Define concat in terms of shorter concat
string concat (string a, string b, string c) {
    return concat(concat(a,b), c);
}
string concat (string a, string b, string c, string d) {
    return concat(concat(a,b,c), d);
}
string concat (string a, string b, string c, string d, string e) {
    return concat(concat(a,b,c,d), e);
}
string concat (string a, string b, string c, string d, string e, string f) {
    return concat(concat(a,b,c,d,e), f);
}


// Texture


// Closures

closure color emission() BUILTIN;
closure color background() BUILTIN;
closure color diffuse(normal N) BUILTIN;
closure color oren_nayar (normal N, float sigma) BUILTIN;
closure color translucent(normal N) BUILTIN;
closure color phong(normal N, float exponent) BUILTIN;
closure color ward(normal N, vector T,float ax, float ay) BUILTIN;
closure color microfacet(string distribution, normal N, vector U, float xalpha,
                         float yalpha, float eta, int refract) BUILTIN;
closure color microfacet(string distribution, normal N, float alpha, float eta,
                         int refr)
{
    return microfacet(distribution, N, vector(0), alpha, alpha, eta, refr);
}
closure color reflection(normal N, float eta) BUILTIN;
closure color reflection(normal N) { return reflection (N, 0.0); }
closure color refraction(normal N, float eta) BUILTIN;
closure color transparent() BUILTIN;
closure color debug(string tag) BUILTIN;
closure color holdout() BUILTIN;
closure color subsurface(float eta, float g, color mfp, color albedo) BUILTIN;

#ifdef MATERIALX_CLOSURES

// -------------------------------------------------------------//
// BSDF closures                                                //
// -------------------------------------------------------------//
​
// Constructs a diffuse reflection BSDF based on the Oren-Nayar reflectance model.
//
//  \param  N           Normal vector of the surface point beeing shaded.
//  \param  albedo      Surface albedo.
//  \param  roughness   Surface roughness [0,1]. A value of 0.0 gives Lambertian reflectance.
//  \param  label       Optional string parameter to name this component. For use in AOVs / LPEs.
//
closure color oren_nayar_diffuse_bsdf(normal N, color albedo, float roughness) BUILTIN;
​
// Constructs a diffuse reflection BSDF based on the corresponding component of 
// the Disney Principled shading model.
//
//  \param  N           Normal vector of the surface point beeing shaded.
//  \param  albedo      Surface albedo.
//  \param  roughness   Surface roughness [0,1].
//  \param  label       Optional string parameter to name this component. For use in AOVs / LPEs.
//
closure color burley_diffuse_bsdf(normal N, color albedo, float roughness) BUILTIN;
​
// Constructs a reflection and/or transmission BSDF based on a microfacet reflectance
// model and a Fresnel curve for dielectrics. The two tint parameters control the 
// contribution of each reflection/transmission lobe. The tints should remain 100% white
// for a physically correct dielectric, but can be tweaked for artistic control or set
// to 0.0 for disabling a lobe.
// The closure may be vertically layered over a base BSDF for the surface beneath the
// dielectric layer. This is done using the layer() closure. By chaining multiple 
// dielectric_bsdf closures you can describe a surface with multiple specular lobes.
// If transmission is enabled (transmission_tint > 0.0) the closure may be layered over
// a VDF closure describing the surface interior to handle absorption and scattering
// inside the medium.
//
//  \param  N                   Normal vector of the surface point beeing shaded.
//  \param  U                   Tangent vector of the surface point beeing shaded.
//  \param  reflection_tint     Weight per color channel for the reflection lobe. Should be (1,1,1) for a physically-correct dielectric surface, 
//                              but can be tweaked for artistic control. Set to (0,0,0) to disable reflection.
//  \param  transmission_tint   Weight per color channel for the transmission lobe. Should be (1,1,1) for a physically-correct dielectric surface, 
//                              but can be tweaked for artistic control. Set to (0,0,0) to disable transmission.
//  \param  roughness_x         Surface roughness in the U direction with a perceptually linear response over its range.
//  \param  roughness_y         Surface roughness in the V direction with a perceptually linear response over its range.
//  \param  ior                 Refraction index.
//  \param  distribution        Microfacet distribution. An implementation is expected to support the following distributions: { "ggx" }
//  \param  thinfilm_thickness  Optional float parameter for thickness of an iridescent thin film layer on top of this BSDF. Given in nanometers.
//  \param  thinfilm_ior        Optional float parameter for refraction index of the thin film layer.
//  \param  label               Optional string parameter to name this component. For use in AOVs / LPEs.
//
closure color dielectric_bsdf(normal N, vector U, color reflection_tint, color transmission_tint, float roughness_x, float roughness_y, float ior, string distribution) BUILTIN;
​
// Constructs a reflection BSDF based on a microfacet reflectance model.
// Uses a Fresnel curve with complex refraction index for conductors/metals.
// If an artistic parametrization is preferred the artistic_ior() utility function
// can be used to convert from artistic to physical parameters.
//
//  \param  N                   Normal vector of the surface point beeing shaded.
//  \param  U                   Tangent vector of the surface point beeing shaded.
//  \param  roughness_x         Surface roughness in the U direction with a perceptually linear response over its range.
//  \param  roughness_y         Surface roughness in the V direction with a perceptually linear response over its range.
//  \param  ior                 Refraction index.
//  \param  extinction          Extinction coefficient.
//  \param  distribution        Microfacet distribution. An implementation is expected to support the following distributions: { "ggx" }
//  \param  thinfilm_thickness  Optional float parameter for thickness of an iridescent thin film layer on top of this BSDF. Given in nanometers.
//  \param  thinfilm_ior        Optional float parameter for refraction index of the thin film layer.
//  \param  label               Optional string parameter to name this component. For use in AOVs / LPEs.
//
closure color conductor_bsdf(normal N, vector U, float roughness_x, float roughness_y, color ior, color extinction, string distribution) BUILTIN;
​
// Constructs a reflection and/or transmission BSDF based on a microfacet reflectance model
// and a generalized Schlick Fresnel curve. The two tint parameters control the contribution
// of each reflection/transmission lobe.
// The closure may be vertically layered over a base BSDF for the surface beneath the
// dielectric layer. This is done using the layer() closure. By chaining multiple 
// dielectric_bsdf closures you can describe a surface with multiple specular lobes.
// If transmission is enabled (transmission_tint > 0.0) the closure may be layered over
// a VDF closure describing the surface interior to handle absorption and scattering
// inside the medium.
//
//  \param  N                   Normal vector of the surface point beeing shaded.
//  \param  U                   Tangent vector of the surface point beeing shaded.
//  \param  reflection_tint     Weight per color channel for the reflection lobe. Set to (0,0,0) to disable reflection.
//  \param  transmission_tint   Weight per color channel for the transmission lobe. Set to (0,0,0) to disable transmission.
//  \param  roughness_x         Surface roughness in the U direction with a perceptually linear response over its range.
//  \param  roughness_y         Surface roughness in the V direction with a perceptually linear response over its range.
//  \param  f0                  Reflectivity per color channel at facing angles.
//  \param  f90                 Reflectivity per color channel at grazing angles.
//  \param  distribution        Microfacet distribution. An implementation is expected to support the following distributions: { "ggx" }
//  \param  thinfilm_thickness  Optional float parameter for thickness of an iridescent thin film layer on top of this BSDF. Given in nanometers.
//  \param  thinfilm_ior        Optional float parameter for refraction index of the thin film layer.
//  \param  label               Optional string parameter to name this component. For use in AOVs / LPEs.
//
closure color generalized_schlick_bsdf(normal N, vector U, color reflection_tint, color transmission_tint, float roughness_x, float roughness_y, color f0, color f90, float exponent, string distribution) BUILTIN;
​
// Constructs a translucent (diffuse transmission) BSDF based on the Lambert reflectance model.
//
//  \param  N           Normal vector of the surface point beeing shaded.
//  \param  albedo      Surface albedo.
//  \param  label       Optional string parameter to name this component. For use in AOVs / LPEs.
//
closure color translucent_bsdf(normal N, color albedo) BUILTIN;

// Constructs a closure that represents straight transmission through a surface.
//
//  \param  label       Optional string parameter to name this component. For use in AOVs / LPEs.
//
// NOTE:
//  - This is not a node in the MaterialX library, but the surface shader constructor
//    node has an 'opacity' parameter to control textured cutout opacity.
//
closure color transparent_bsdf() BUILTIN;
​
// Constructs a BSSRDF for subsurface scattering within a homogeneous medium.
//
//  \param  N                   Normal vector of the surface point beeing shaded.
//  \param  albedo              Single-scattering albedo of the medium.
//  \param  transmission_depth  Distance travelled inside the medium by white light before its color becomes transmission_color by Beer's law.
//                              Given in scene length units, range [0,infinity). Together with transmission_color this determines the extinction
//                              coefficient of the medium.
//  \param  transmission_color  Desired color resulting from white light transmitted a distance of 'transmission_depth' through the medium.
//                              Together with transmission_depth this determines the extinction coefficient of the medium.
//  \param  anisotropy          Scattering anisotropy [-1,1]. Negative values give backwards scattering, positive values give forward scattering, 
//                              and 0.0 gives uniform scattering.
//  \param  label               Optional string parameter to name this component. For use in AOVs / LPEs.
//
closure color subsurface_bssrdf(normal N, color albedo, float transmission_depth, color transmission_color, float anisotropy) BUILTIN;
​
// Constructs a microfacet BSDF for the back-scattering properties of cloth-like materials.
// This closure may be vertically layered over a base BSDF, where energy that is not reflected
// will be transmitted to the base closure.
//
//  \param  N           Normal vector of the surface point beeing shaded.
//  \param  albedo      Surface albedo.
//  \param  roughness   Surface roughness [0,1].
//  \param  label       Optional string parameter to name this component. For use in AOVs / LPEs.
//
closure color sheen_bsdf(normal N, color albedo, float roughness) BUILTIN;
​
​
// -------------------------------------------------------------//
// EDF closures                                                 //
// -------------------------------------------------------------//
​
// Constructs an EDF emitting light uniformly in all directions.
//
//  \param  emittance   Radiant emittance of light leaving the surface.
//  \param  label       Optional string parameter to name this component. For use in AOVs / LPEs.
//
closure color uniform_edf(color emittance) BUILTIN;
​
​
// -------------------------------------------------------------//
// VDF closures                                                 //
// -------------------------------------------------------------//
​
// Constructs a VDF scattering light for a general participating medium, based on the Henyey-Greenstein
// phase function. Forward, backward and uniform scattering is supported and controlled by the anisotropy input.
//
//  \param  albedo      Volume single-scattering albedo.
//  \param  extinction  Volume extinction coefficient.
//  \param  anisotropy  Scattering anisotropy [-1,1]. Negative values give backwards scattering, positive values give forward scattering, 
//                      and 0.0 gives uniform scattering.
//  \param  label       Optional string parameter to name this component. For use in AOVs / LPEs.
//
closure color anisotropic_vdf(color albedo, color extinction, float anisotropy) BUILTIN;

// Constructs a VDF for light passing through a dielectric homogenous medium, such as glass or liquids.
// The parameters transmission_depth and transmission_color control the extinction coefficient of the medium
// in and artist-friendly way. A priority can be set to determine the ordering of overlapping media.
//
//  \param  albedo              Single-scattering albedo of the medium.
//  \param  transmission_depth  Distance travelled inside the medium by white light before its color becomes transmission_color by Beer's law.
//                              Given in scene length units, range [0,infinity). Together with transmission_color this determines the extinction
//                              coefficient of the medium.
//  \param  transmission_color  Desired color resulting from white light transmitted a distance of 'transmission_depth' through the medium.
//                              Together with transmission_depth this determines the extinction coefficient of the medium.
//  \param  anisotropy          Scattering anisotropy [-1,1]. Negative values give backwards scattering, positive values give forward scattering, 
//                              and 0.0 gives uniform scattering.
//  \param  ior                 Refraction index of the medium.
//  \param  priority            Priority of this medium (for nested dielectrics).
//  \param  label               Optional string parameter to name this component. For use in AOVs / LPEs.
//
closure color medium_vdf(color albedo, float transmission_depth, color transmission_color, float anisotropy, float ior, int priority) BUILTIN;

// -------------------------------------------------------------//
// Layering closures                                            //
// -------------------------------------------------------------//
​
// Vertically layer a layerable BSDF such as dielectric_bsdf, generalized_schlick_bsdf or
// sheen_bsdf over a BSDF or VDF. The implementation is target specific, but a standard way
// of handling this is by albedo scaling, using "base*(1-reflectance(top)) + top", where
// reflectance() calculates the directional albedo of a given top BSDF.
//
//  \param  top   Closure defining the top layer.
//  \param  base  Closure defining the base layer.
//
// TODO:
// - This could also be achived by closure nesting where each layerable closure takes
//   a closure color "base" input instead.
// - One advantage having a dedicated layer() closure is that in the future we may want to
//   introduce parameters to describe the sandwitched medium between the layer interfaces.
//   Such parameterization could then be added on this layer() closure as extra arguments.
// - Do we want/need parameters for the medium here now, or do we look at that later?
//
closure color layer(closure color top, closure color base) BUILTIN;

// NOTE: For "horisontal layering" closure mix() already exists in OSL.
​
​
// -------------------------------------------------------------//
// Utility functions                                            //
// -------------------------------------------------------------//
​
// Converts the artistic parameterization reflectivity and edge_tint to complex IOR values.
// To be used with the conductor_bsdf() closure.
// [OG14] "Artist Friendly Metallic Fresnel", http://jcgt.org/published/0003/04/03/paper.pdf
//
//  \param  reflectivity  Reflectivity per color channel at facing angles ('r' parameter in [OG14]).
//  \param  edge_tint     Color bias for grazing angles ('g' parameter in [OG14]).
//                        NOTE: This is not equal to 'f90' in a Schlick Fresnel parameterization.
//  \param  ior           Output refraction index.
//  \param  extinction    Output extinction coefficient.
//
void artistic_ior(color reflectivity, color edge_tint, output color ior, output color extinction)
​{
    color r = clamp(reflectivity, 0.0, 0.99);
    color r_sqrt = sqrt(r);
    color n_min = (1.0 - r) / (1.0 + r);
    color n_max = (1.0 + r_sqrt) / (1.0 - r_sqrt);
    ior = mix(n_max, n_min, edge_tint);

    color np1 = ior + 1.0;
    color nm1 = ior - 1.0;
    color k2 = (np1*np1 * r - nm1*nm1) / (1.0 - r);
    k2 = max(k2, 0.0);
    extinction = sqrt(k2);
}

#endif // MATERIALX_CLOSURES

// Renderer state
int backfacing () BUILTIN;
int raytype (string typename) BUILTIN;
// the individual 'isFOOray' functions are deprecated
int iscameraray () { return raytype("camera"); }
int isdiffuseray () { return raytype("diffuse"); }
int isglossyray () { return raytype("glossy"); }
int isshadowray () { return raytype("shadow"); }
int getmatrix (string fromspace, string tospace, output matrix M) BUILTIN;
int getmatrix (string fromspace, output matrix M) {
    return getmatrix (fromspace, "common", M);
}


// Miscellaneous




#undef BUILTIN
#undef BUILTIN_DERIV
#undef PERCOMP1
#undef PERCOMP2
#undef PERCOMP2F

#endif /* STDOSL_H */
